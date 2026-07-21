#include "ScriptFieldRegistry.h"

#include "ScriptSandbox.h"

#include <sol/sol.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace rt2::core {

namespace {

// One field as declared by a rt2.field.* constructor, before the name is
// known. The constructors run during the chunk, at which point the field's
// name is still just a table key in the rt2.fields literal; names are
// attached afterwards when we walk that table.
struct PendingField
{
    ScriptFieldType            type = ScriptFieldType::Float;
    ScriptFieldValue           defaultValue;
    std::optional<std::string> alias;
};

std::string ReadFileText(const std::filesystem::path& path, bool& ok)
{
    ok = false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    ok = true;
    return ss.str();
}

std::optional<std::string> ReadAlias(const sol::optional<sol::table>& opts)
{
    if (!opts) return std::nullopt;
    sol::object a = (*opts)["alias"];
    if (a.is<std::string>())
    {
        auto s = a.as<std::string>();
        if (!s.empty()) return s;
    }
    return std::nullopt;
}

// FNV-1a over the source text. Used alongside (mtime, size) for cache
// invalidation: filesystem timestamp granularity is coarse enough that a
// same-length in-place edit within one clock tick (a formatter rewriting
// 5.0 -> 9.0) would otherwise be served stale from cache. Hashing a file we
// have already read costs nothing measurable.
uint64_t HashSource(const std::string& s)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s)
    {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

} // namespace

ScriptFieldRegistry::ScriptFieldRegistry()
    : m_Lua(new sol::state())
{
    // Same library set as ScriptSystem: no io/os/debug/package. The parse
    // environment additionally nils dofile/loadfile/require/load.
    m_Lua->open_libraries(sol::lib::base,
                          sol::lib::math,
                          sol::lib::string,
                          sol::lib::table);
}

ScriptFieldRegistry::~ScriptFieldRegistry()
{
    delete m_Lua;
    m_Lua = nullptr;
}

void ScriptFieldRegistry::Clear()
{
    m_Cache.clear();
}

void ScriptFieldRegistry::EvictIfNeeded()
{
    if (m_Cache.size() <= kMaxCachedPaths) return;
    auto oldest = m_Cache.begin();
    for (auto it = m_Cache.begin(); it != m_Cache.end(); ++it)
        if (it->second.lastUse < oldest->second.lastUse)
            oldest = it;
    m_Cache.erase(oldest);
}

bool ScriptFieldRegistry::ParseFile(const std::filesystem::path& path,
                                    const std::string& source,
                                    std::vector<ScriptFieldDescriptor>& out,
                                    std::string& diagnostic)
{
    out.clear();
    diagnostic.clear();

    // Fresh environment per parse, so two scripts cannot see each other's
    // globals and a re-parse cannot inherit stale declarations.
    sol::environment env(*m_Lua, sol::create, m_Lua->globals());
    InstallSandbox(*m_Lua, env);

    // Declarations accumulate here as the constructors run.
    auto pending = std::make_shared<std::vector<PendingField>>();

    sol::table rt2   = m_Lua->create_table();
    sol::table field = m_Lua->create_table();

    // Each constructor records the declaration C++-side and returns a marker
    // table carrying its index. Marshalling the default value back out of
    // Lua later would lose the declared type (a Lua number is both int and
    // float), which is exactly the distinction 6B needs to keep.
    auto marker = [this, pending](ScriptFieldType type,
                                  ScriptFieldValue def,
                                  const sol::optional<sol::table>& opts) -> sol::table
    {
        PendingField pf;
        pf.type = type;
        pf.defaultValue = std::move(def);
        pf.alias = ReadAlias(opts);
        pending->push_back(std::move(pf));

        sol::table m = m_Lua->create_table();
        m["__rt2_field"] = true;
        m["__rt2_index"] = static_cast<int64_t>(pending->size() - 1);
        return m;
    };

    field["bool"] = [marker](sol::optional<bool> v,
                             sol::optional<sol::table> opts) {
        return marker(ScriptFieldType::Bool, ScriptFieldValue{ v.value_or(false) }, opts);
    };
    field["int"] = [marker](sol::optional<int64_t> v,
                            sol::optional<sol::table> opts) {
        return marker(ScriptFieldType::Int, ScriptFieldValue{ v.value_or(int64_t{0}) }, opts);
    };
    field["float"] = [marker](sol::optional<double> v,
                              sol::optional<sol::table> opts) {
        return marker(ScriptFieldType::Float, ScriptFieldValue{ v.value_or(0.0) }, opts);
    };
    field["string"] = [marker](sol::optional<std::string> v,
                               sol::optional<sol::table> opts) {
        return marker(ScriptFieldType::String,
                      ScriptFieldValue{ v.value_or(std::string{}) }, opts);
    };
    field["uuid"] = [marker](sol::optional<std::string> v,
                             sol::optional<sol::table> opts) {
        // Default is the nil UUID, meaning "unset". Validation is
        // format-only: a script may legitimately reference an entity that
        // does not exist until runtime.
        UUID u;
        if (v && !v->empty()) u = UUID::Parse(*v);
        return marker(ScriptFieldType::Uuid, ScriptFieldValue{ u }, opts);
    };
    field["vec3"] = [marker](sol::optional<double> x, sol::optional<double> y,
                             sol::optional<double> z, sol::optional<sol::table> opts) {
        glm::vec3 v{ static_cast<float>(x.value_or(0.0)),
                     static_cast<float>(y.value_or(0.0)),
                     static_cast<float>(z.value_or(0.0)) };
        return marker(ScriptFieldType::Vec3, ScriptFieldValue{ v }, opts);
    };
    field["color"] = [marker](sol::optional<double> r, sol::optional<double> g,
                              sol::optional<double> b, sol::optional<sol::table> opts) {
        glm::vec3 v{ static_cast<float>(r.value_or(1.0)),
                     static_cast<float>(g.value_or(1.0)),
                     static_cast<float>(b.value_or(1.0)) };
        return marker(ScriptFieldType::Color, ScriptFieldValue{ v }, opts);
    };

    rt2["field"] = field;
    env["rt2"] = rt2;

    // Run the chunk under the instruction budget. The chunk DEFINES the
    // lifecycle callbacks; we never invoke them.
    sol::protected_function_result result;
    {
        ScriptInstructionBudget guard(m_Lua->lua_state(), kInstructionBudget);
        result = m_Lua->safe_script(
            source, env, sol::script_pass_on_error, path.string());
    }

    if (!result.valid())
    {
        sol::error err = result;
        diagnostic = err.what();
        return false;
    }

    // Read back rt2.fields.
    //
    // "No declarations" and "malformed declarations" must NOT be conflated.
    // A script that never assigns rt2.fields legitimately declares nothing,
    // and returning an empty parsed=true set is correct. But a script that
    // replaced the injected `rt2` object, or set `fields` to a non-table
    // (`rt2.fields = "unfinished"` mid-edit), is structurally broken — and
    // reporting THAT as a clean empty set defeats D10 entirely: W2
    // reconciliation would read it as "the author deleted every field" and
    // destroy the authored values, which is the exact outcome D10 exists to
    // prevent. Malformed structure is therefore a parse failure, which
    // routes it through the last-known-good path.
    sol::object rt2Obj = env["rt2"];
    if (!rt2Obj.is<sol::table>())
    {
        diagnostic = "the `rt2` table was replaced or removed by the script; "
                     "field declarations cannot be read";
        return false;
    }

    sol::object fieldsObj = rt2Obj.as<sol::table>()["fields"];
    if (fieldsObj == sol::nil)
        return true;   // never assigned: legitimately declares no fields
    if (!fieldsObj.is<sol::table>())
    {
        diagnostic = "`rt2.fields` must be a table of field declarations";
        return false;
    }

    sol::table fields = fieldsObj.as<sol::table>();
    fields.for_each([&](sol::object key, sol::object value) {
        if (!key.is<std::string>() || !value.is<sol::table>())
            return;   // ignore array-style or non-declaration entries
        sol::table entry = value.as<sol::table>();
        sol::object marked = entry["__rt2_field"];
        sol::object idxObj = entry["__rt2_index"];
        if (!marked.is<bool>() || !marked.as<bool>() || !idxObj.is<int64_t>())
            return;   // a plain table that is not a rt2.field.* declaration
        const auto idx = static_cast<size_t>(idxObj.as<int64_t>());
        if (idx >= pending->size())
            return;

        // Copied, not moved: one declaration object may legally be bound to
        // more than one field name.
        const PendingField& pf = (*pending)[idx];
        ScriptFieldDescriptor d;
        d.name         = key.as<std::string>();
        d.type         = pf.type;
        d.defaultValue = pf.defaultValue;
        d.alias        = pf.alias;
        out.push_back(std::move(d));
    });

    // D2: Lua table iteration is unordered, which would reshuffle the
    // inspector between frames. Sort by name for a deterministic layout.
    std::sort(out.begin(), out.end(),
              [](const ScriptFieldDescriptor& a, const ScriptFieldDescriptor& b) {
                  return a.name < b.name;
              });

    return true;
}

ScriptFieldRegistry::Result
ScriptFieldRegistry::GetDeclaredFields(const std::filesystem::path& path)
{
    Result result;

    const std::string key = path.string();
    if (key.empty())
    {
        result.parsed = false;
        result.diagnostic = "no script path bound";
        return result;
    }

    // Stat the file to decide whether a cached parse is still current.
    int64_t  mtime = 0;
    uint64_t size  = 0;
    bool     stated = false;
    {
        std::error_code ec;
        const auto st = std::filesystem::status(path, ec);
        if (!ec && std::filesystem::is_regular_file(st))
        {
            const auto t = std::filesystem::last_write_time(path, ec);
            if (!ec)
            {
                mtime = static_cast<int64_t>(t.time_since_epoch().count());
                const auto sz = std::filesystem::file_size(path, ec);
                if (!ec)
                {
                    size = static_cast<uint64_t>(sz);
                    stated = true;
                }
            }
        }
    }

    // Read the source up front and hash it. (mtime, size) alone is not a
    // sound invalidation key: filesystem timestamp granularity is coarse
    // enough that a same-length in-place edit within one clock tick — a
    // formatter rewriting 5.0 -> 9.0, say — would be served stale forever.
    // Hashing costs one pass over a file we must read to parse anyway, and
    // it makes 6C's file watcher correct by construction.
    bool readOk = false;
    const std::string source = ReadFileText(path, readOk);
    const uint64_t hash = readOk ? HashSource(source) : 0;

    auto it = m_Cache.find(key);
    const bool cacheHit = it != m_Cache.end()
                       && it->second.everParsed
                       && stated
                       && readOk
                       && it->second.mtime == mtime
                       && it->second.size  == size
                       && it->second.hash  == hash;

    if (cacheHit)
    {
        it->second.lastUse = ++m_UseTick;
        result.descriptors = it->second.descriptors;
        result.parsed = true;
        return result;
    }

    std::vector<ScriptFieldDescriptor> parsed;
    std::string diagnostic;
    bool ok = false;
    if (!readOk)
        diagnostic = "failed to read script file: " + path.string();
    else
        ok = ParseFile(path, source, parsed, diagnostic);

    CacheEntry& entry = m_Cache[key];
    entry.lastUse = ++m_UseTick;
    if (ok)
    {
        entry.descriptors = parsed;
        entry.everParsed  = true;
        entry.mtime       = mtime;
        entry.size        = size;
        entry.hash        = hash;
        result.descriptors = std::move(parsed);
        result.parsed = true;
    }
    else
    {
        // D10: hand back the last known-good set (empty if there never was
        // one) so the inspector keeps rendering, and mark the parse failed
        // so callers skip reconciliation rather than treating every field as
        // removed. Deliberately does NOT update mtime/size: the next query
        // re-attempts the parse, so fixing the syntax error recovers without
        // needing another edit.
        result.descriptors = entry.descriptors;
        result.parsed = false;
        result.diagnostic = std::move(diagnostic);
    }

    EvictIfNeeded();
    return result;
}

} // namespace rt2::core
