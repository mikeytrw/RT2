#include <doctest/doctest.h>

#include "SceneManager.h"
#include "PrimitiveGeometry.h"
#include "ECSComponents.h"
#include "ECSScene.h"
#include "ScriptFieldValue.h"
#include "ScriptFieldRegistry.h"
#include "core/UUID.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>

// ============================================================================
// Phase 6B tests.
//
// W0 (app wiring) lands first: SceneManager gains the authoring API for
// ScriptComponent (HasScript / GetScriptState / SetScriptState) that the
// inspector and SetScriptCommand will drive in W4/W5. Until those land, this
// API has no production caller, so these tests are the only thing proving it.
//
// The W1-W6 cases from the Phase 6B plan (reflection, reconciliation,
// serialization v3, command undo/redo, inspector guards) append here.
// ============================================================================

namespace
{

struct ScriptFixture
{
    rt2::core::DeterministicUuidProvider ids;
    SceneManager manager;

    ScriptFixture()
    {
        manager.SetUuidProvider(&ids);
        manager.AddMaterial(SceneMaterial{});
    }

    rt2::core::UUID AddBox(const char* name)
    {
        const auto entity = manager.AddObjectWithGeometry(
            name, PrimitiveGeometry::CreateCube(2.0f), {0, 0, 0}, {}, 1.0f, 0);
        return manager.GetEntityUuid(entity);
    }

    SceneManager::EntityId EntityOf(const rt2::core::UUID& uuid)
    {
        return SceneManager::EntityId{ manager.FindEntityByUuid(uuid) };
    }
};

// A ScriptComponent bound to a path, with one authored field value.
ScriptComponent MakeScript(const char* path, double speed)
{
    ScriptComponent sc;
    sc.asset.kind = AssetKind::Script;
    sc.asset.path = path;
    sc.asset.sourceKey = std::string("lua:asset=") + path;
    sc.fieldValues["speed"] = rt2::core::ScriptFieldValue{ speed };
    return sc;
}

} // namespace

TEST_CASE("Phase6B W0: SetScriptState adds, replaces, and removes ScriptComponent")
{
    ScriptFixture fx;
    const auto box = fx.AddBox("Box");

    // Absent to begin with.
    CHECK_FALSE(fx.manager.HasScript(fx.EntityOf(box)));
    CHECK_FALSE(fx.manager.GetScriptState(box).has_value());

    // Add.
    const auto added = fx.manager.SetScriptState(box, MakeScript("scripts/spin.lua", 5.0));
    CHECK(added.success);
    CHECK(fx.manager.HasScript(fx.EntityOf(box)));

    auto read = fx.manager.GetScriptState(box);
    REQUIRE(read.has_value());
    CHECK(read->asset.kind == AssetKind::Script);
    CHECK(read->asset.path == "scripts/spin.lua");
    REQUIRE(read->fieldValues.count("speed") == 1);
    CHECK(std::get<double>(read->fieldValues.at("speed")) == doctest::Approx(5.0));

    // Replace (the field-edit path W4/W5 will drive).
    CHECK(fx.manager.SetScriptState(box, MakeScript("scripts/spin.lua", 7.0)).success);
    read = fx.manager.GetScriptState(box);
    REQUIRE(read.has_value());
    CHECK(std::get<double>(read->fieldValues.at("speed")) == doctest::Approx(7.0));

    // Remove.
    CHECK(fx.manager.SetScriptState(box, std::nullopt).success);
    CHECK_FALSE(fx.manager.HasScript(fx.EntityOf(box)));
    CHECK_FALSE(fx.manager.GetScriptState(box).has_value());

    // Removing again is a successful no-op, not a failure.
    CHECK(fx.manager.SetScriptState(box, std::nullopt).success);
}

TEST_CASE("Phase6B W0: script edits report SyncImpact::None")
{
    // D8: script bindings and field values are authoring/runtime state that
    // never reaches the GPU scene. The analogous SetMotionState returns None;
    // a Structural impact here would trigger a needless full GPU resync on
    // every field drag.
    ScriptFixture fx;
    const auto box = fx.AddBox("Box");

    const auto added = fx.manager.SetScriptState(box, MakeScript("scripts/spin.lua", 5.0));
    CHECK(added.success);
    CHECK(added.syncImpact == rt2::core::SyncImpact::None);
    REQUIRE(added.affectedEntities.size() == 1);
    CHECK(added.affectedEntities[0] == box);

    const auto removed = fx.manager.SetScriptState(box, std::nullopt);
    CHECK(removed.success);
    CHECK(removed.syncImpact == rt2::core::SyncImpact::None);
}

TEST_CASE("Phase6B W0: SetScriptState on a missing entity fails gracefully")
{
    ScriptFixture fx;
    fx.AddBox("Box");

    // A UUID that was never in this scene. Deliberately NOT exercised via
    // RemoveEntity: that path has a known pre-existing SIGSEGV
    // (SceneManagerTests.cpp:85) and would mask what this case is testing.
    const auto ghost = rt2::core::UUID::Parse("00000000-0000-4000-8000-000000000abc");
    REQUIRE_FALSE(ghost.IsNull());

    const auto result = fx.manager.SetScriptState(ghost, MakeScript("scripts/spin.lua", 1.0));
    CHECK_FALSE(result.success);
    CHECK(result.error.code == rt2::core::Error::InvalidEntity);

    // And the read side is null-safe on a dead entity.
    CHECK_FALSE(fx.manager.GetScriptState(ghost).has_value());
}

TEST_CASE("Phase6B W0: HasScript is false for invalid entity ids")
{
    ScriptFixture fx;
    CHECK_FALSE(fx.manager.HasScript(SceneManager::EntityId{}));
}

// ============================================================================
// W1 — reflection: the rt2.fields DSL and ScriptFieldRegistry.
// ============================================================================

namespace
{

// Writes .lua files into a unique temp directory and cleans up after itself.
struct ScriptDir
{
    std::filesystem::path root;

    ScriptDir()
    {
        const auto stamp = std::chrono::high_resolution_clock::now()
                               .time_since_epoch().count();
        root = std::filesystem::temp_directory_path()
             / ("rt2-6b-" + std::to_string(stamp));
        std::filesystem::create_directories(root);
    }

    ~ScriptDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    std::filesystem::path Write(const char* name, const std::string& text) const
    {
        const auto p = root / name;
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << text;
        out.close();
        return p;
    }
};

const rt2::core::ScriptFieldDescriptor*
Find(const std::vector<rt2::core::ScriptFieldDescriptor>& v, const char* name)
{
    for (const auto& d : v)
        if (d.name == name) return &d;
    return nullptr;
}

} // namespace

TEST_CASE("Phase6B W1: every rt2.field constructor parses with its declared type")
{
    ScriptDir dir;
    const auto path = dir.Write("all.lua", R"(
rt2.fields = {
  enabled = rt2.field.bool(true),
  count   = rt2.field.int(3),
  speed   = rt2.field.float(5.0),
  label   = rt2.field.string("cube"),
  target  = rt2.field.uuid("00000000-0000-4000-8000-000000000abc"),
  offset  = rt2.field.vec3(1, 2, 3),
  tint    = rt2.field.color(0.25, 0.5, 0.75),
}

function on_update(entity, dt, input, world) end
)");

    rt2::core::ScriptFieldRegistry reg;
    const auto result = reg.GetDeclaredFields(path);
    REQUIRE(result.parsed);
    CHECK(result.diagnostic.empty());
    REQUIRE(result.descriptors.size() == 7);

    using rt2::core::ScriptFieldType;

    const auto* enabled = Find(result.descriptors, "enabled");
    REQUIRE(enabled);
    CHECK(enabled->type == ScriptFieldType::Bool);
    CHECK(std::get<bool>(enabled->defaultValue) == true);

    const auto* count = Find(result.descriptors, "count");
    REQUIRE(count);
    CHECK(count->type == ScriptFieldType::Int);
    CHECK(std::get<int64_t>(count->defaultValue) == 3);

    const auto* speed = Find(result.descriptors, "speed");
    REQUIRE(speed);
    CHECK(speed->type == ScriptFieldType::Float);
    CHECK(std::get<double>(speed->defaultValue) == doctest::Approx(5.0));

    const auto* label = Find(result.descriptors, "label");
    REQUIRE(label);
    CHECK(label->type == ScriptFieldType::String);
    CHECK(std::get<std::string>(label->defaultValue) == "cube");

    const auto* target = Find(result.descriptors, "target");
    REQUIRE(target);
    CHECK(target->type == ScriptFieldType::Uuid);
    CHECK_FALSE(std::get<rt2::core::UUID>(target->defaultValue).IsNull());

    const auto* offset = Find(result.descriptors, "offset");
    REQUIRE(offset);
    CHECK(offset->type == ScriptFieldType::Vec3);
    CHECK(std::get<glm::vec3>(offset->defaultValue).y == doctest::Approx(2.0f));

    // Color shares the glm::vec3 arm but keeps its own declared type, which
    // is what selects the color-picker widget in the inspector.
    const auto* tint = Find(result.descriptors, "tint");
    REQUIRE(tint);
    CHECK(tint->type == ScriptFieldType::Color);
    CHECK(std::get<glm::vec3>(tint->defaultValue).z == doctest::Approx(0.75f));
    CHECK(rt2::core::ScriptFieldTypesCompatible(ScriptFieldType::Vec3,
                                                ScriptFieldType::Color));
    CHECK_FALSE(rt2::core::ScriptFieldTypesCompatible(ScriptFieldType::Float,
                                                      ScriptFieldType::Vec3));
}

TEST_CASE("Phase6B W1: descriptors are sorted by name (D2 determinism)")
{
    ScriptDir dir;
    const auto path = dir.Write("order.lua", R"(
rt2.fields = {
  zeta  = rt2.field.float(1.0),
  alpha = rt2.field.float(2.0),
  mid   = rt2.field.float(3.0),
}
)");

    rt2::core::ScriptFieldRegistry reg;
    const auto result = reg.GetDeclaredFields(path);
    REQUIRE(result.parsed);
    REQUIRE(result.descriptors.size() == 3);
    CHECK(result.descriptors[0].name == "alpha");
    CHECK(result.descriptors[1].name == "mid");
    CHECK(result.descriptors[2].name == "zeta");
}

TEST_CASE("Phase6B W1: alias is captured and names the old field")
{
    ScriptDir dir;
    const auto path = dir.Write("alias.lua", R"(
rt2.fields = {
  vel = rt2.field.float(1.0, { alias = "speed" }),
  raw = rt2.field.float(2.0),
}
)");

    rt2::core::ScriptFieldRegistry reg;
    const auto result = reg.GetDeclaredFields(path);
    REQUIRE(result.parsed);

    const auto* vel = Find(result.descriptors, "vel");
    REQUIRE(vel);
    REQUIRE(vel->alias.has_value());
    CHECK(*vel->alias == "speed");

    const auto* raw = Find(result.descriptors, "raw");
    REQUIRE(raw);
    CHECK_FALSE(raw->alias.has_value());
}

TEST_CASE("Phase6B W1: a script with no declarations parses to zero fields")
{
    ScriptDir dir;
    rt2::core::ScriptFieldRegistry reg;

    const auto none = dir.Write("none.lua",
        "function on_update(entity, dt, input, world) end\n");
    auto r = reg.GetDeclaredFields(none);
    CHECK(r.parsed);
    CHECK(r.descriptors.empty());

    // Q10d: an empty file is a legal script, not a failure.
    const auto empty = dir.Write("empty.lua", "");
    r = reg.GetDeclaredFields(empty);
    CHECK(r.parsed);
    CHECK(r.descriptors.empty());
}

TEST_CASE("Phase6B W1: a syntax error yields last-good descriptors, not a throw")
{
    ScriptDir dir;
    rt2::core::ScriptFieldRegistry reg;

    // First parse cleanly so there IS a last-good set.
    const auto path = dir.Write("live.lua", R"(
rt2.fields = { speed = rt2.field.float(5.0) }
)");
    auto good = reg.GetDeclaredFields(path);
    REQUIRE(good.parsed);
    REQUIRE(good.descriptors.size() == 1);

    // Now break it. D10: the registry must hand back the last-good set with
    // parsed=false so callers SKIP reconciliation — reconciling against zero
    // declarations would treat every field as removed and delete the user's
    // authored values on a transient typo.
    dir.Write("live.lua", "rt2.fields = { speed = rt2.field.float(5.0 }\n");
    auto broken = reg.GetDeclaredFields(path);
    CHECK_FALSE(broken.parsed);
    CHECK_FALSE(broken.diagnostic.empty());
    REQUIRE(broken.descriptors.size() == 1);
    CHECK(broken.descriptors[0].name == "speed");

    // Fixing the file recovers without needing a further edit: the failed
    // parse deliberately did not stamp mtime/size.
    dir.Write("live.lua", R"(
rt2.fields = { speed = rt2.field.float(9.0), extra = rt2.field.bool(false) }
)");
    auto fixed = reg.GetDeclaredFields(path);
    CHECK(fixed.parsed);
    CHECK(fixed.descriptors.size() == 2);
}

TEST_CASE("Phase6B W1: a missing file fails without throwing")
{
    rt2::core::ScriptFieldRegistry reg;
    const auto r = reg.GetDeclaredFields(
        std::filesystem::temp_directory_path() / "rt2-does-not-exist-6b.lua");
    CHECK_FALSE(r.parsed);
    CHECK_FALSE(r.diagnostic.empty());
    CHECK(r.descriptors.empty());

    // An empty path is the unbound-script case, not a crash.
    const auto none = reg.GetDeclaredFields({});
    CHECK_FALSE(none.parsed);
    CHECK(none.descriptors.empty());
}

TEST_CASE("Phase6B W1: a runaway loop at file scope is bounded, not hung")
{
    // D3: the editor parses arbitrary user .lua on selection, so the parse
    // runs under an instruction-count hook. Without it this test never
    // returns.
    ScriptDir dir;
    const auto path = dir.Write("hang.lua", R"(
local n = 0
while true do n = n + 1 end
rt2.fields = { speed = rt2.field.float(5.0) }
)");

    rt2::core::ScriptFieldRegistry reg;
    const auto r = reg.GetDeclaredFields(path);
    CHECK_FALSE(r.parsed);
    CHECK_FALSE(r.diagnostic.empty());
    CHECK(r.descriptors.empty());
}

TEST_CASE("Phase6B W1: dangerous base functions are unavailable to a parse")
{
    ScriptDir dir;
    rt2::core::ScriptFieldRegistry reg;

    // Regression: these were previously assigned sol::nil, which REMOVES the
    // key and so falls straight back through to globals. loadfile and load
    // both survived that and returned normally, leaving the sandbox open.
    int i = 0;
    for (const char* call : { "dofile('x')", "loadfile('x')",
                              "require('os')", "load('return 1')" })
    {
        const auto name = "sandbox" + std::to_string(i++) + ".lua";
        const auto path = dir.Write(name.c_str(), std::string(call) + "\n");
        const auto r = reg.GetDeclaredFields(path);
        INFO("call: " << call);
        CHECK_FALSE(r.parsed);
    }

    // io/os are not opened at all.
    const auto p = dir.Write("sandbox_io.lua", "local f = io.open('x')\n");
    CHECK_FALSE(reg.GetDeclaredFields(p).parsed);
}

TEST_CASE("Phase6B W1: _G and the raw* family are denied (isolation guarantee)")
{
    // _G is the shared globals table behind every per-entity environment's
    // __index fallback. Reachable, `_G.x = 1` in one entity's script is
    // visible to every other entity's script, which breaks the per-entity
    // isolation the environment design exists to provide. The raw* family
    // bypasses metatables and would defeat any protection placed on a shared
    // table.
    ScriptDir dir;
    rt2::core::ScriptFieldRegistry reg;

    int i = 0;
    for (const char* call : { "_G.leak = 1",
                              "rawset(_G, 'leak', 1)",
                              "local t = rawget(_G, 'print')",
                              "collectgarbage('collect')" })
    {
        const auto name = "denied" + std::to_string(i++) + ".lua";
        const auto path = dir.Write(name.c_str(), std::string(call) + "\n");
        const auto r = reg.GetDeclaredFields(path);
        INFO("call: " << call);
        CHECK_FALSE(r.parsed);
    }

    // Ordinary base functions a real script needs stay available.
    const auto ok = dir.Write("allowed.lua", R"(
local t = { 3, 1, 2 }
table.sort(t)
local s = string.format("%d", #t)
local n = math.floor(2.7)
if type(s) ~= "string" then error("unexpected") end
rt2.fields = { speed = rt2.field.float(5.0) }
)");
    const auto r = reg.GetDeclaredFields(ok);
    CHECK(r.parsed);
    CHECK(r.descriptors.size() == 1);
}

TEST_CASE("Phase6B W1: the environment metatable does not leak the globals table")
{
    // Denying the bare names is not enough if the environment's own
    // metatable hands back the globals table: getmetatable(_ENV).__index IS
    // the globals table, so __index.loadfile is the real loadfile no matter
    // what the environment shadows.
    ScriptDir dir;
    rt2::core::ScriptFieldRegistry reg;

    int i = 0;
    for (const char* call : {
            "local g = getmetatable(_ENV).__index; g.loadfile('x')",
            "local g = getmetatable(_ENV).__index; g.leak = 1",
            "local m = getmetatable(''); m.__index.upper = nil",
         })
    {
        const auto name = "meta" + std::to_string(i++) + ".lua";
        const auto path = dir.Write(name.c_str(), std::string(call) + "\n");
        const auto r = reg.GetDeclaredFields(path);
        INFO("call: " << call);
        CHECK_FALSE(r.parsed);
    }
}

TEST_CASE("Phase6B W1: mutating a library table does not leak to another script")
{
    // math/string/table are reached through the globals fallback, so without
    // per-environment copies `math.floor = ...` in one script poisons every
    // other script sharing the Lua state.
    ScriptDir dir;
    rt2::core::ScriptFieldRegistry reg;

    const auto poison = dir.Write("poison.lua", R"(
math.floor = function(x) return 999 end
rt2.fields = { a = rt2.field.float(1.0) }
)");
    CHECK(reg.GetDeclaredFields(poison).parsed);

    const auto victim = dir.Write("victim.lua", R"(
if math.floor(2.7) ~= 2 then error("math was poisoned by another script") end
rt2.fields = { b = rt2.field.float(2.0) }
)");
    const auto r = reg.GetDeclaredFields(victim);
    INFO("diagnostic: " << r.diagnostic);
    CHECK(r.parsed);
}

TEST_CASE("Phase6B W1: a malformed rt2.fields is a parse failure, not an empty set")
{
    // D10 hinges on this. "rt2.fields is missing" legitimately means zero
    // declarations, but "rt2.fields is a string" or "rt2 was replaced" is a
    // half-finished edit. Reporting parsed=true with zero descriptors would
    // let W2 reconciliation read it as "the author deleted every field" and
    // destroy the authored values.
    ScriptDir dir;
    rt2::core::ScriptFieldRegistry reg;

    int i = 0;
    for (const char* body : { "rt2.fields = \"unfinished\"",
                              "rt2.fields = 42",
                              "rt2 = nil",
                              "rt2 = { fields = \"nope\" }" })
    {
        const auto name = "malformed" + std::to_string(i++) + ".lua";
        const auto path = dir.Write(name.c_str(), std::string(body) + "\n");
        const auto r = reg.GetDeclaredFields(path);
        INFO("body: " << body);
        CHECK_FALSE(r.parsed);
        CHECK_FALSE(r.diagnostic.empty());
    }

    // But simply not declaring anything remains a clean, empty parse.
    const auto silent = dir.Write("silent.lua", "local x = 1\n");
    CHECK(reg.GetDeclaredFields(silent).parsed);
}

TEST_CASE("Phase6B W1: the cache re-parses on edit and Clear() drops it")
{
    ScriptDir dir;
    rt2::core::ScriptFieldRegistry reg;

    const auto path = dir.Write("cached.lua", R"(
rt2.fields = { speed = rt2.field.float(5.0) }
)");
    auto first = reg.GetDeclaredFields(path);
    REQUIRE(first.parsed);
    REQUIRE(first.descriptors.size() == 1);
    CHECK(reg.CachedCount() == 1);

    // A second query is served from cache and must agree.
    auto cached = reg.GetDeclaredFields(path);
    CHECK(cached.parsed);
    CHECK(cached.descriptors.size() == 1);
    CHECK(reg.CachedCount() == 1);

    // Editing the file invalidates by (mtime, size). Sleep so the write time
    // is observably different even on a coarse filesystem clock; the size
    // also changes here, so the test does not depend on timer resolution.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    dir.Write("cached.lua", R"(
rt2.fields = { speed = rt2.field.float(5.0), height = rt2.field.float(2.0) }
)");
    auto reparsed = reg.GetDeclaredFields(path);
    CHECK(reparsed.parsed);
    CHECK(reparsed.descriptors.size() == 2);

    reg.Clear();
    CHECK(reg.CachedCount() == 0);
    auto afterClear = reg.GetDeclaredFields(path);
    CHECK(afterClear.parsed);
    CHECK(afterClear.descriptors.size() == 2);
    CHECK(reg.CachedCount() == 1);
}

TEST_CASE("Phase6B W1: non-declaration entries in rt2.fields are ignored")
{
    ScriptDir dir;
    const auto path = dir.Write("mixed.lua", R"(
rt2.fields = {
  speed  = rt2.field.float(5.0),
  bogus  = { not_a_field = true },
  [1]    = rt2.field.float(1.0),
}
)");

    rt2::core::ScriptFieldRegistry reg;
    const auto r = reg.GetDeclaredFields(path);
    REQUIRE(r.parsed);
    REQUIRE(r.descriptors.size() == 1);
    CHECK(r.descriptors[0].name == "speed");
}

TEST_CASE("Phase6B W1: one declaration bound to two names yields two fields")
{
    ScriptDir dir;
    const auto path = dir.Write("shared.lua", R"(
local shared = rt2.field.float(4.0)
rt2.fields = { a = shared, b = shared }
)");

    rt2::core::ScriptFieldRegistry reg;
    const auto r = reg.GetDeclaredFields(path);
    REQUIRE(r.parsed);
    REQUIRE(r.descriptors.size() == 2);
    CHECK(r.descriptors[0].name == "a");
    CHECK(r.descriptors[1].name == "b");
    CHECK(std::get<double>(r.descriptors[1].defaultValue) == doctest::Approx(4.0));
}
