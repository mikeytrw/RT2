#include <doctest/doctest.h>

#include "SceneManager.h"
#include "PrimitiveGeometry.h"
#include "ECSComponents.h"
#include "ECSScene.h"
#include "EditorCommand.h"
#include "EditorCommandHistory.h"
#include "EditorPropertyCommands.h"
#include "EditorSyncRouter.h"
#include "ISceneRenderBridge.h"
#include "PropertyEditSession.h"
#include "ScriptComponentValidation.h"
#include "ScriptFieldValue.h"
#include "ScriptFieldReconcile.h"
#include "ScriptFieldRegistry.h"
#include "ScriptFieldResolver.h"
#include "ScriptFieldChangePolicy.h"
#include "SceneSerializer.h"
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
    sc.fieldValues["speed"] = rt2::core::ScriptFieldEntry{
        rt2::core::ScriptFieldType::Float,
        rt2::core::ScriptFieldValue{ speed }
    };
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
    CHECK(read->fieldValues.at("speed").type == rt2::core::ScriptFieldType::Float);
    CHECK(std::get<double>(read->fieldValues.at("speed").value) == doctest::Approx(5.0));

    // Replace (the field-edit path W4/W5 will drive).
    CHECK(fx.manager.SetScriptState(box, MakeScript("scripts/spin.lua", 7.0)).success);
    read = fx.manager.GetScriptState(box);
    REQUIRE(read.has_value());
    CHECK(std::get<double>(read->fieldValues.at("speed").value) == doctest::Approx(7.0));

    // Remove.
    CHECK(fx.manager.SetScriptState(box, std::nullopt).success);
    CHECK_FALSE(fx.manager.HasScript(fx.EntityOf(box)));
    CHECK_FALSE(fx.manager.GetScriptState(box).has_value());

    // Removing again is a successful no-op, not a failure.
    CHECK(fx.manager.SetScriptState(box, std::nullopt).success);
}

TEST_CASE("Phase6B W3: SetScriptState validates and canonicalizes before mutation")
{
    ScriptFixture fx;
    const auto box = fx.AddBox("Box");
    fx.manager.ClearDirty();
    const uint64_t revisionBefore = fx.manager.AuthoringRevision();

    ScriptComponent malformed = MakeScript("scripts/spin.lua", 5.0);
    malformed.fieldValues["speed"].type = rt2::core::ScriptFieldType::Bool;
    const auto rejected = fx.manager.SetScriptState(box, malformed);
    CHECK_FALSE(rejected.success);
    CHECK(rejected.error.code == rt2::core::Error::InvalidArgument);
    CHECK(fx.manager.AuthoringRevision() == revisionBefore);
    CHECK_FALSE(fx.manager.AuthoringDoc().metadata.dirty);
    CHECK_FALSE(fx.manager.GetScriptState(box).has_value());

    ScriptComponent invalidText = MakeScript("scripts/spin.lua", 5.0);
    invalidText.fieldValues["label"] = {
        rt2::core::ScriptFieldType::String,
        std::string(1, static_cast<char>(0xff)) };
    CHECK_FALSE(fx.manager.SetScriptState(box, invalidText).success);
    CHECK(fx.manager.AuthoringRevision() == revisionBefore);

    ScriptComponent emptyName = MakeScript("scripts/spin.lua", 5.0);
    emptyName.fieldValues[""] = {
        rt2::core::ScriptFieldType::Bool, true };
    CHECK_FALSE(fx.manager.SetScriptState(box, emptyName).success);
    CHECK(fx.manager.AuthoringRevision() == revisionBefore);

    ScriptComponent stale = MakeScript("scripts/spin.lua", 7.0);
    stale.asset.sourceKey = "lua:asset=scripts/old.lua";
    REQUIRE(fx.manager.SetScriptState(box, stale).success);
    const auto stored = fx.manager.GetScriptState(box);
    REQUIRE(stored.has_value());
    CHECK(stored->asset.sourceKey == "lua:asset=scripts/spin.lua");

    ScriptComponent invalidUnbound;
    invalidUnbound.asset.kind = AssetKind::Script;
    invalidUnbound.fieldValues["speed"] = rt2::core::ScriptFieldEntry{
        rt2::core::ScriptFieldType::Float, 1.0 };
    CHECK_FALSE(fx.manager.SetScriptState(box, invalidUnbound).success);

    ScriptComponent unbound;
    unbound.asset.kind = AssetKind::Script;
    unbound.asset.sourceKey = "stale-but-derived";
    REQUIRE(fx.manager.SetScriptState(box, unbound).success);
    const auto storedUnbound = fx.manager.GetScriptState(box);
    REQUIRE(storedUnbound.has_value());
    CHECK(storedUnbound->asset.path.empty());
    CHECK(storedUnbound->asset.sourceKey.empty());
}

TEST_CASE("Phase6B W3: normalization and destructive loss remain distinguishable")
{
    rt2::core::SceneLoadReport loadReport;
    loadReport.normalizedScriptMetadata = true;
    rt2::core::ScriptFieldResolutionResult resolution;
    std::vector<rt2::core::FieldDiagnostic> diagnostics;

    auto classification = rt2::core::ClassifyScriptFieldChanges(
        loadReport, resolution, diagnostics);
    CHECK(classification.requiresSave);
    CHECK_FALSE(classification.destructive);

    rt2::core::FieldDiagnostic removed;
    removed.kind = rt2::core::FieldDiagnostic::Kind::Removed;
    diagnostics.push_back(removed);
    resolution.changed = true;
    classification = rt2::core::ClassifyScriptFieldChanges(
        loadReport, resolution, diagnostics);
    CHECK(classification.requiresSave);
    CHECK(classification.destructive);

    loadReport = rt2::core::SceneLoadReport{};
    diagnostics.clear();
    resolution.changed = false;
    classification = rt2::core::ClassifyScriptFieldChanges(
        loadReport, resolution, diagnostics);
    CHECK_FALSE(classification.requiresSave);
    CHECK_FALSE(classification.destructive);

    loadReport.normalizedScriptFieldData = true;
    classification = rt2::core::ClassifyScriptFieldChanges(
        loadReport, resolution, diagnostics);
    CHECK(classification.requiresSave);
    CHECK_FALSE(classification.destructive);
}

TEST_CASE("Phase6B W3: destructive repair gate blocks autosave through acknowledgement")
{
    rt2::core::ScriptRepairPersistenceGate gate;
    CHECK_FALSE(gate.SuppressAutosave());
    CHECK_FALSE(gate.ConsumeSaveAcknowledgement());

    gate.Adopt(true);
    CHECK(gate.Pending());
    CHECK(gate.SuppressAutosave());
    CHECK(gate.ConsumeSaveAcknowledgement());
    CHECK(gate.Acknowledged());
    CHECK(gate.SuppressAutosave());
    CHECK_FALSE(gate.ConsumeSaveAcknowledgement());
    CHECK(gate.SuppressAutosave());

    gate.OnPersistedOrReset();
    CHECK_FALSE(gate.Pending());
    CHECK_FALSE(gate.SuppressAutosave());

    gate.Adopt(false);
    CHECK_FALSE(gate.Pending());
    CHECK_FALSE(gate.ConsumeSaveAcknowledgement());
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

TEST_CASE("Phase6B W3: reflection rejects unpersistable names and string defaults")
{
    ScriptDir dir;
    const auto emptyName = dir.Write("empty-name.lua", R"(
rt2.fields = { [""] = rt2.field.bool(true) }
)");
    const auto invalidText = dir.Write("invalid-text.lua", R"(
rt2.fields = { label = rt2.field.string(string.char(255)) }
)");

    rt2::core::ScriptFieldRegistry registry;
    const auto emptyResult = registry.GetDeclaredFields(emptyName);
    CHECK_FALSE(emptyResult.parsed);
    CHECK(emptyResult.diagnostic.find("empty field name") != std::string::npos);

    const auto textResult = registry.GetDeclaredFields(invalidText);
    CHECK_FALSE(textResult.parsed);
    CHECK(textResult.diagnostic.find("cannot be persisted") != std::string::npos);
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

// ============================================================================
// W2 — typed field storage and deterministic reconciliation.
// ============================================================================

namespace
{

rt2::core::ScriptFieldEntry Entry(rt2::core::ScriptFieldType type,
                                  rt2::core::ScriptFieldValue value)
{
    return { type, std::move(value) };
}

rt2::core::ScriptFieldDescriptor Descriptor(
    std::string name,
    rt2::core::ScriptFieldType type,
    rt2::core::ScriptFieldValue defaultValue,
    std::optional<std::string> alias = std::nullopt)
{
    rt2::core::ScriptFieldDescriptor descriptor;
    descriptor.name = std::move(name);
    descriptor.type = type;
    descriptor.defaultValue = std::move(defaultValue);
    descriptor.alias = std::move(alias);
    return descriptor;
}

size_t CountDiagnostics(const std::vector<rt2::core::FieldDiagnostic>& diagnostics,
                        rt2::core::FieldDiagnostic::Kind kind)
{
    size_t count = 0;
    for (const auto& diagnostic : diagnostics)
        if (diagnostic.kind == kind) ++count;
    return count;
}

const rt2::core::UUID kW2Entity = rt2::core::UUID::Parse(
    "00000000-0000-4000-8000-000000000abc");

} // namespace

TEST_CASE("Phase6B W2: same-type values survive while added and removed fields reconcile")
{
    using namespace rt2::core;
    ScriptFieldMap persisted;
    persisted["speed"] = Entry(ScriptFieldType::Float, 7.0);
    // Insert removed fields in reverse lexical order to prove diagnostics do
    // not inherit unordered_map iteration order.
    persisted["obsolete_z"] = Entry(ScriptFieldType::String, std::string("old"));
    persisted["obsolete_a"] = Entry(ScriptFieldType::Bool, false);

    // Deliberately unsorted input: the reconciler owns deterministic order.
    std::vector<ScriptFieldDescriptor> declared{
        Descriptor("speed", ScriptFieldType::Float, 5.0),
        Descriptor("enabled", ScriptFieldType::Bool, true),
    };
    std::vector<FieldDiagnostic> diagnostics;
    const auto result = ReconcileScriptFields(persisted, declared, kW2Entity,
                                              diagnostics);

    REQUIRE(result.size() == 2);
    CHECK(std::get<double>(result.at("speed").value) == doctest::Approx(7.0));
    CHECK(std::get<bool>(result.at("enabled").value));
    REQUIRE(diagnostics.size() == 3);
    CHECK(diagnostics[0].kind == FieldDiagnostic::Kind::Added);
    CHECK(diagnostics[0].field == "enabled");
    CHECK(diagnostics[1].kind == FieldDiagnostic::Kind::Removed);
    CHECK(diagnostics[1].field == "obsolete_a");
    CHECK(diagnostics[2].kind == FieldDiagnostic::Kind::Removed);
    CHECK(diagnostics[2].field == "obsolete_z");
}

TEST_CASE("Phase6B W2: aliases migrate once and an existing target wins")
{
    using namespace rt2::core;
    const auto declaration = Descriptor("velocity", ScriptFieldType::Float,
                                        1.0, std::string("speed"));

    ScriptFieldMap oldOnly;
    oldOnly["speed"] = Entry(ScriptFieldType::Float, 4.0);
    std::vector<FieldDiagnostic> migratedDiagnostics;
    auto migrated = ReconcileScriptFields(oldOnly, { declaration }, kW2Entity,
                                          migratedDiagnostics);
    CHECK(std::get<double>(migrated.at("velocity").value) == doctest::Approx(4.0));
    REQUIRE(migratedDiagnostics.size() == 1);
    CHECK(migratedDiagnostics[0].kind == FieldDiagnostic::Kind::Renamed);
    CHECK(migratedDiagnostics[0].fromField == "speed");

    ScriptFieldMap targetWins = oldOnly;
    targetWins["velocity"] = Entry(ScriptFieldType::Float, 9.0);
    std::vector<FieldDiagnostic> targetDiagnostics;
    auto preserved = ReconcileScriptFields(targetWins, { declaration }, kW2Entity,
                                           targetDiagnostics);
    CHECK(std::get<double>(preserved.at("velocity").value) == doctest::Approx(9.0));
    REQUIRE(targetDiagnostics.size() == 1);
    CHECK(targetDiagnostics[0].kind == FieldDiagnostic::Kind::Removed);
    CHECK(targetDiagnostics[0].field == "speed");
}

TEST_CASE("Phase6B W2: ambiguous aliases use defaults and emit one diagnostic")
{
    using namespace rt2::core;
    ScriptFieldMap persisted;
    persisted["speed"] = Entry(ScriptFieldType::Float, 8.0);
    std::vector<ScriptFieldDescriptor> declared{
        Descriptor("velocity", ScriptFieldType::Float, 2.0, std::string("speed")),
        Descriptor("thrust", ScriptFieldType::Float, 3.0, std::string("speed")),
    };
    std::vector<FieldDiagnostic> diagnostics;
    const auto result = ReconcileScriptFields(persisted, declared, kW2Entity,
                                              diagnostics);

    CHECK(std::get<double>(result.at("thrust").value) == doctest::Approx(3.0));
    CHECK(std::get<double>(result.at("velocity").value) == doctest::Approx(2.0));
    CHECK(CountDiagnostics(diagnostics, FieldDiagnostic::Kind::AmbiguousAlias) == 1);
    CHECK(CountDiagnostics(diagnostics, FieldDiagnostic::Kind::Removed) == 0);
}

TEST_CASE("Phase6B W2: aliases without an authored source are ordinary additions")
{
    using namespace rt2::core;
    std::vector<ScriptFieldDescriptor> declared{
        Descriptor("speed", ScriptFieldType::Float, 1.0),
        Descriptor("velocity", ScriptFieldType::Float, 2.0, std::string("speed")),
    };
    std::vector<FieldDiagnostic> diagnostics;
    const auto result = ReconcileScriptFields({}, declared, kW2Entity,
                                              diagnostics);

    CHECK(std::get<double>(result.at("speed").value) == doctest::Approx(1.0));
    CHECK(std::get<double>(result.at("velocity").value) == doctest::Approx(2.0));
    CHECK(CountDiagnostics(diagnostics, FieldDiagnostic::Kind::AmbiguousAlias) == 0);
    CHECK(CountDiagnostics(diagnostics, FieldDiagnostic::Kind::Added) == 2);
}

TEST_CASE("Phase6B W2: incompatible and malformed stored values reset safely")
{
    using namespace rt2::core;
    ScriptFieldMap persisted;
    persisted["speed"] = Entry(ScriptFieldType::Float, 7.0);
    // The tag says Vec3 but the payload is the Float arm.
    persisted["offset"] = Entry(ScriptFieldType::Vec3, 12.0);

    std::vector<ScriptFieldDescriptor> declared{
        Descriptor("speed", ScriptFieldType::String, std::string("slow")),
        Descriptor("offset", ScriptFieldType::Vec3, glm::vec3(1.0f)),
    };
    std::vector<FieldDiagnostic> diagnostics;
    const auto result = ReconcileScriptFields(persisted, declared, kW2Entity,
                                              diagnostics);

    CHECK(std::get<std::string>(result.at("speed").value) == "slow");
    CHECK(std::get<glm::vec3>(result.at("offset").value).x == doctest::Approx(1.0f));
    REQUIRE(diagnostics.size() == 2);
    CHECK(diagnostics[0].field == "offset");
    CHECK(diagnostics[0].kind == FieldDiagnostic::Kind::InvalidStoredValue);
    CHECK(diagnostics[1].field == "speed");
    CHECK(diagnostics[1].kind == FieldDiagnostic::Kind::TypeChanged);
}

TEST_CASE("Phase6B W2: malformed alias sources reset the target safely")
{
    using namespace rt2::core;
    ScriptFieldMap persisted;
    persisted["old_offset"] = Entry(ScriptFieldType::Vec3, 12.0);
    std::vector<FieldDiagnostic> diagnostics;
    const auto result = ReconcileScriptFields(
        persisted,
        { Descriptor("offset", ScriptFieldType::Vec3, glm::vec3(3.0f),
                     std::string("old_offset")) },
        kW2Entity, diagnostics);

    CHECK(std::get<glm::vec3>(result.at("offset").value).x == doctest::Approx(3.0f));
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].kind == FieldDiagnostic::Kind::InvalidStoredValue);
    CHECK(diagnostics[0].field == "offset");
    CHECK(diagnostics[0].fromField == "old_offset");
}

TEST_CASE("Phase6B W2: vec3 and color share a compatible value arm")
{
    using namespace rt2::core;
    ScriptFieldMap persisted;
    persisted["tint"] = Entry(ScriptFieldType::Vec3,
                               glm::vec3(0.25f, 0.5f, 0.75f));
    std::vector<FieldDiagnostic> diagnostics;
    const auto result = ReconcileScriptFields(
        persisted,
        { Descriptor("tint", ScriptFieldType::Color, glm::vec3(1.0f)) },
        kW2Entity, diagnostics);

    CHECK(result.at("tint").type == ScriptFieldType::Color);
    CHECK(std::get<glm::vec3>(result.at("tint").value).z == doctest::Approx(0.75f));
    CHECK(diagnostics.empty());
}

TEST_CASE("Phase6B W2: bool int and uuid values migrate through aliases")
{
    using namespace rt2::core;
    const UUID target = UUID::Parse("00000000-0000-4000-8000-000000000def");
    ScriptFieldMap persisted;
    persisted["old_bool"] = Entry(ScriptFieldType::Bool, true);
    persisted["old_int"] = Entry(ScriptFieldType::Int, int64_t{42});
    persisted["old_uuid"] = Entry(ScriptFieldType::Uuid, target);

    std::vector<ScriptFieldDescriptor> declared{
        Descriptor("new_uuid", ScriptFieldType::Uuid, UUID::Nil(), std::string("old_uuid")),
        Descriptor("new_bool", ScriptFieldType::Bool, false, std::string("old_bool")),
        Descriptor("new_int", ScriptFieldType::Int, int64_t{0}, std::string("old_int")),
    };
    std::vector<FieldDiagnostic> diagnostics;
    const auto result = ReconcileScriptFields(persisted, declared, kW2Entity,
                                              diagnostics);

    CHECK(std::get<bool>(result.at("new_bool").value));
    CHECK(std::get<int64_t>(result.at("new_int").value) == 42);
    CHECK(std::get<UUID>(result.at("new_uuid").value) == target);
    CHECK(CountDiagnostics(diagnostics, FieldDiagnostic::Kind::Renamed) == 3);
}

TEST_CASE("Phase6B W2: resolver reconciles entities independently in UUID order")
{
    using namespace rt2::core;
    ScriptDir dir;
    dir.Write("shared.lua", R"(
rt2.fields = {
  speed = rt2.field.float(5.0),
  enabled = rt2.field.bool(true),
}
)");

    ScriptFixture fx;
    fx.manager.AuthoringDoc().metadata.sourcePath = dir.root / "scene.rt2scene";
    const auto a = fx.AddBox("A");
    const auto b = fx.AddBox("B");
    REQUIRE(fx.manager.SetScriptState(a, MakeScript("shared.lua", 7.0)).success);
    REQUIRE(fx.manager.SetScriptState(b, MakeScript("shared.lua", 9.0)).success);

    ScriptFieldRegistry registry;
    AssetResolutionContext assetContext{dir.root, nullptr};
    std::vector<AssetDiagnostic> assetDiagnostics;
    std::vector<FieldDiagnostic> diagnostics;
    const auto resolution = ScriptFieldResolver::ResolveDocument(
        fx.manager.AuthoringDoc(), registry, assetContext, assetDiagnostics,
        diagnostics);

    CHECK(resolution.changed);
    CHECK(resolution.resolvedEntities == 2);
    CHECK(resolution.skippedEntities == 0);
    REQUIRE(CountDiagnostics(diagnostics, FieldDiagnostic::Kind::Added) == 2);
    REQUIRE(diagnostics.size() == 2);
    CHECK(diagnostics[0].entity < diagnostics[1].entity);

    const auto stateA = fx.manager.GetScriptState(a);
    const auto stateB = fx.manager.GetScriptState(b);
    REQUIRE(stateA.has_value());
    REQUIRE(stateB.has_value());
    CHECK(std::get<double>(stateA->fieldValues.at("speed").value) == doctest::Approx(7.0));
    CHECK(std::get<double>(stateB->fieldValues.at("speed").value) == doctest::Approx(9.0));
    CHECK(std::get<bool>(stateA->fieldValues.at("enabled").value));
    CHECK(std::get<bool>(stateB->fieldValues.at("enabled").value));

    diagnostics.clear();
    assetDiagnostics.clear();
    const auto secondResolution = ScriptFieldResolver::ResolveDocument(
        fx.manager.AuthoringDoc(), registry, assetContext, assetDiagnostics,
        diagnostics);
    CHECK_FALSE(secondResolution.changed);
    CHECK(secondResolution.resolvedEntities == 2);
    CHECK(secondResolution.skippedEntities == 0);
    CHECK(diagnostics.empty());
}

TEST_CASE("Phase6B W2: resolver accounts for invalid asset kinds and missing UUIDs")
{
    using namespace rt2::core;
    ScriptFixture fx;
    const auto invalidKindUuid = fx.manager.CreateEmpty("Invalid kind")
                                     .affectedEntities.front();
    auto invalidKind = MakeScript("not-a-script.gltf", 1.0);
    invalidKind.asset.kind = AssetKind::Model;

    auto& registry = fx.manager.AuthoringDoc().ecs.registry;
    // Bypass the validated authoring API deliberately: this test exercises
    // the resolver's defensive handling of corrupt/raw registry state.
    registry.emplace<ScriptComponent>(
        fx.manager.FindEntityByUuid(invalidKindUuid), invalidKind);
    const auto unidentified = registry.create();
    registry.emplace<ScriptComponent>(unidentified, MakeScript("missing-id.lua", 2.0));

    ScriptFieldRegistry fields;
    AssetResolutionContext assetContext{
        std::filesystem::temp_directory_path(), nullptr};
    std::vector<AssetDiagnostic> assetDiagnostics;
    std::vector<FieldDiagnostic> diagnostics;
    const auto resolution = ScriptFieldResolver::ResolveDocument(
        fx.manager.AuthoringDoc(), fields, assetContext, assetDiagnostics,
        diagnostics);

    CHECK_FALSE(resolution.changed);
    CHECK(resolution.resolvedEntities == 0);
    CHECK(resolution.skippedEntities == 2);
    REQUIRE(diagnostics.size() == 2);
    CHECK(diagnostics[0].kind == FieldDiagnostic::Kind::InvalidAssetKind);
    CHECK(diagnostics[0].entity == invalidKindUuid);
    CHECK(diagnostics[1].kind == FieldDiagnostic::Kind::MissingEntityId);
    CHECK(diagnostics[1].entity.IsNull());
}

TEST_CASE("Phase6B W2: resolver parse failure preserves authored values exactly")
{
    using namespace rt2::core;
    ScriptDir dir;
    const auto path = dir.Write("broken.lua",
        "rt2.fields = { speed = rt2.field.float(5.0) }\n");
    ScriptFieldRegistry registry;
    REQUIRE(registry.GetDeclaredFields(path).parsed);
    dir.Write("broken.lua", "rt2.fields = { speed = rt2.field.float( }\n");

    ScriptFixture fx;
    fx.manager.AuthoringDoc().metadata.sourcePath = dir.root / "scene.rt2scene";
    const auto entity = fx.AddBox("Broken");
    const auto authored = MakeScript("broken.lua", 8.0);
    REQUIRE(fx.manager.SetScriptState(entity, authored).success);

    std::vector<FieldDiagnostic> diagnostics;
    AssetResolutionContext assetContext{dir.root, nullptr};
    std::vector<AssetDiagnostic> assetDiagnostics;
    const auto resolution = ScriptFieldResolver::ResolveDocument(
        fx.manager.AuthoringDoc(), registry, assetContext, assetDiagnostics,
        diagnostics);
    CHECK_FALSE(resolution.changed);
    CHECK(resolution.resolvedEntities == 0);
    CHECK(resolution.skippedEntities == 1);
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].kind == FieldDiagnostic::Kind::ParseFailed);
    REQUIRE(assetDiagnostics.size() == 2);
    CHECK(std::count_if(
        assetDiagnostics.begin(), assetDiagnostics.end(),
        [](const AssetDiagnostic& diagnostic) {
            return diagnostic.severity == AssetDiagnostic::Malformed;
        }) == 1);
    CHECK(std::count_if(
        assetDiagnostics.begin(), assetDiagnostics.end(),
        [](const AssetDiagnostic& diagnostic) {
            return diagnostic.severity == AssetDiagnostic::Stale;
        }) == 1);
    REQUIRE(fx.manager.GetScriptState(entity).has_value());
    CHECK(fx.manager.GetScriptState(entity)->fieldValues == authored.fieldValues);
}

TEST_CASE("Phase6B W2: clone and subtree duplication preserve typed entries")
{
    using namespace rt2::core;
    ScriptFixture fx;
    // Use a durable empty entity. AddObjectWithGeometry creates legacy inline
    // MeshRef data with no primitive/source provenance, which CloneInMemory
    // intentionally rejects as non-reopenable before it reaches scripts.
    const auto source = fx.manager.CreateEmpty("Source").affectedEntities.front();
    auto script = MakeScript("shared.lua", 6.0);
    script.fieldValues["tint"] = Entry(
        ScriptFieldType::Color, glm::vec3(0.1f, 0.2f, 0.3f));
    REQUIRE(fx.manager.SetScriptState(source, script).success);

    const auto duplicateUuid = fx.ids.CreateV4();
    const auto duplicated = fx.manager.DuplicateSubtreesWithUuids(
        { source }, { duplicateUuid });
    REQUIRE(duplicated.mutation.success);
    const auto duplicateState = fx.manager.GetScriptState(duplicateUuid);
    REQUIRE(duplicateState.has_value());
    CHECK(duplicateState->fieldValues == script.fieldValues);

    SceneDocument clone;
    clone.SetUuidProvider(&fx.ids);
    Error error;
    REQUIRE(SceneSerializer::CloneInMemory(fx.manager.AuthoringDoc(), clone, error));
    const auto clonedEntity = clone.FindByUuid(source);
    REQUIRE(clone.ecs.registry.valid(clonedEntity));
    const auto* clonedScript = clone.ecs.registry.try_get<ScriptComponent>(clonedEntity);
    REQUIRE(clonedScript != nullptr);
    CHECK(clonedScript->fieldValues == script.fieldValues);
}

// ============================================================================
// Phase 6B W4 — SetScriptCommand and history integration.
//
// W4.0 made SetScriptState suppress canonical no-ops (present→same-present and
// absent→absent). W4.1 added SetScriptCommand + MakeSetScriptCommandIfEffective.
// These tests prove add/remove/path-edit/field-edit through Execute/Undo/Redo,
// no-op suppression at the factory and manager level, invalid-state rejection,
// staged-commit semantics, save/reopen round-trip, and zero GPU sync.
// ============================================================================

namespace
{

// A RecordingBridge counts sync calls so W4 can assert zero GPU work.
class RecordingBridge final : public rt2::core::ISceneRenderBridge
{
public:
    int fullSync = 0;
    int materialSync = 0;
    int transformSync = 0;
    int temporalReset = 0;
    int renderRequests = 0;
    void FullSync(GPUSceneData&) override { ++fullSync; }
    void MaterialSync(GPUSceneData&) override { ++materialSync; }
    void TransformSync(GPUSceneData&) override { ++transformSync; }
    void ResetTemporalState() override { ++temporalReset; }
    void RequestRender() override { ++renderRequests; }
};

} // namespace

TEST_CASE("Phase6B W4: factory suppresses absent→absent and equal canonical present→present")
{
    ScriptFixture fx;
    const auto box = fx.AddBox("Box");

    // absent→absent
    REQUIRE(!MakeSetScriptCommandIfEffective(box, std::nullopt, std::nullopt));

    // present→same-present (canonical)
    const auto script = MakeScript("scripts/spin.lua", 5.0);
    REQUIRE(fx.manager.SetScriptState(box, script).success);
    const auto stored = fx.manager.GetScriptState(box);
    REQUIRE(stored.has_value());
    REQUIRE(!MakeSetScriptCommandIfEffective(box, *stored, *stored));

    // stale sourceKey with same path is still a no-op (factory canonicalizes
    // the before-state, and the after-state is compared canonically).
    ScriptComponent staleBefore = *stored;
    staleBefore.asset.sourceKey = "lua:asset=scripts/old.lua";
    ScriptComponent staleAfter = *stored;
    staleAfter.asset.sourceKey = "lua:asset=scripts/other.lua";
    REQUIRE(!MakeSetScriptCommandIfEffective(box, staleBefore, staleAfter));
}

TEST_CASE("Phase6B W4: factory suppresses reversed ScriptFieldMap insertion order")
{
    ScriptFixture fx;
    const auto box = fx.AddBox("Box");

    ScriptComponent a;
    a.asset.kind = AssetKind::Script;
    a.asset.path = "scripts/multi.lua";
    a.fieldValues["alpha"] = { rt2::core::ScriptFieldType::Float, 1.0 };
    a.fieldValues["beta"]  = { rt2::core::ScriptFieldType::Int,   int64_t{2} };
    a.fieldValues["gamma"] = { rt2::core::ScriptFieldType::String, std::string("g") };

    ScriptComponent b = a; // same content, different insertion order
    b.fieldValues.clear();
    b.fieldValues["gamma"] = { rt2::core::ScriptFieldType::String, std::string("g") };
    b.fieldValues["beta"]  = { rt2::core::ScriptFieldType::Int,   int64_t{2} };
    b.fieldValues["alpha"] = { rt2::core::ScriptFieldType::Float, 1.0 };

    REQUIRE(!MakeSetScriptCommandIfEffective(box, a, b));
}

TEST_CASE("Phase6B W4: same vector payload tagged vec3 vs color is effective")
{
    ScriptFixture fx;
    const auto box = fx.AddBox("Box");

    ScriptComponent before;
    before.asset.kind = AssetKind::Script;
    before.asset.path = "scripts/tint.lua";
    before.fieldValues["tint"] = { rt2::core::ScriptFieldType::Vec3,
                                    glm::vec3(0.2f, 0.4f, 0.6f) };

    ScriptComponent after = before;
    after.fieldValues["tint"].type = rt2::core::ScriptFieldType::Color;

    auto cmd = MakeSetScriptCommandIfEffective(box, before, after);
    REQUIRE(cmd);
}

TEST_CASE("Phase6B W4: invalid before-snapshot is rejected by the factory")
{
    ScriptFixture fx;
    const auto box = fx.AddBox("Box");

    ScriptComponent goodAfter = MakeScript("scripts/spin.lua", 5.0);
    ScriptComponent badBefore = MakeScript("scripts/spin.lua", 5.0);
    badBefore.fieldValues["speed"].type = rt2::core::ScriptFieldType::Bool; // mismatched arm

    REQUIRE(!MakeSetScriptCommandIfEffective(box, badBefore, goodAfter));
}

TEST_CASE("Phase6B W4: direct manager no-op for present→same-present")
{
    ScriptFixture fx;
    const auto box = fx.AddBox("Box");
    fx.manager.ClearDirty();
    const uint64_t rev0 = fx.manager.AuthoringRevision();

    const auto script = MakeScript("scripts/spin.lua", 5.0);
    REQUIRE(fx.manager.SetScriptState(box, script).success);
    const uint64_t rev1 = fx.manager.AuthoringRevision();
    REQUIRE(fx.manager.AuthoringDoc().metadata.dirty);
    CHECK(rev1 == rev0 + 1);

    // Re-apply the canonical same state.
    const auto stored = fx.manager.GetScriptState(box);
    REQUIRE(stored.has_value());
    const auto result = fx.manager.SetScriptState(box, *stored);
    CHECK(result.success);
    CHECK(result.syncImpact == rt2::core::SyncImpact::None);
    CHECK(result.affectedEntities.empty());
    CHECK(fx.manager.AuthoringRevision() == rev1);
    CHECK(fx.manager.AuthoringDoc().metadata.dirty);
}

TEST_CASE("Phase6B W4: direct manager no-op for absent→absent removal")
{
    ScriptFixture fx;
    const auto box = fx.AddBox("Box");
    fx.manager.ClearDirty();
    const uint64_t rev0 = fx.manager.AuthoringRevision();

    const auto result = fx.manager.SetScriptState(box, std::nullopt);
    CHECK(result.success);
    CHECK(result.syncImpact == rt2::core::SyncImpact::None);
    CHECK(result.affectedEntities.empty());
    CHECK(fx.manager.AuthoringRevision() == rev0);
    CHECK_FALSE(fx.manager.AuthoringDoc().metadata.dirty);
}

TEST_CASE("Phase6B W4: add/remove/field-edit round trip through Execute/Undo/Redo")
{
    ScriptFixture fx;
    EditorCommandHistory history;
    const auto box = fx.AddBox("Box");
    fx.manager.ClearDirty();
    const uint64_t rev0 = fx.manager.AuthoringRevision();

    // --- Add ---
    auto afterAdd = MakeScript("scripts/spin.lua", 5.0);
    auto cmdAdd = MakeSetScriptCommandIfEffective(box, std::nullopt, afterAdd);
    REQUIRE(cmdAdd);
    CHECK(cmdAdd->Description() == "Add Script");
    auto r0 = history.Execute(std::move(cmdAdd), fx.manager);
    REQUIRE(r0.success);
    REQUIRE(fx.manager.HasScript(fx.EntityOf(box)));
    const uint64_t rev1 = fx.manager.AuthoringRevision();
    CHECK(rev1 == rev0 + 1);
    CHECK(fx.manager.AuthoringDoc().metadata.dirty);

    auto stored = fx.manager.GetScriptState(box);
    REQUIRE(stored.has_value());
    CHECK(std::get<double>(stored->fieldValues.at("speed").value) == doctest::Approx(5.0));

    // --- Field edit ---
    auto beforeEdit = *stored;
    auto afterEdit = MakeScript("scripts/spin.lua", 9.0);
    auto cmdEdit = MakeSetScriptCommandIfEffective(box, beforeEdit, afterEdit);
    REQUIRE(cmdEdit);
    CHECK(cmdEdit->Description() == "Edit Script");
    auto r1 = history.Execute(std::move(cmdEdit), fx.manager);
    REQUIRE(r1.success);
    const uint64_t rev2 = fx.manager.AuthoringRevision();
    CHECK(rev2 == rev1 + 1);
    stored = fx.manager.GetScriptState(box);
    REQUIRE(stored.has_value());
    CHECK(std::get<double>(stored->fieldValues.at("speed").value) == doctest::Approx(9.0));

    // --- Undo field edit ---
    auto r2 = history.Undo(fx.manager);
    REQUIRE(r2.success);
    stored = fx.manager.GetScriptState(box);
    REQUIRE(stored.has_value());
    CHECK(std::get<double>(stored->fieldValues.at("speed").value) == doctest::Approx(5.0));
    CHECK(fx.manager.AuthoringRevision() == rev2 + 1); // undo bumps revision

    // --- Redo field edit ---
    auto r3 = history.Redo(fx.manager);
    REQUIRE(r3.success);
    stored = fx.manager.GetScriptState(box);
    REQUIRE(stored.has_value());
    CHECK(std::get<double>(stored->fieldValues.at("speed").value) == doctest::Approx(9.0));

    // --- Path edit ---
    auto beforePath = *stored;
    auto afterPath = MakeScript("scripts/wobble.lua", 9.0);
    auto cmdPath = MakeSetScriptCommandIfEffective(box, beforePath, afterPath);
    REQUIRE(cmdPath);
    auto r4 = history.Execute(std::move(cmdPath), fx.manager);
    REQUIRE(r4.success);
    stored = fx.manager.GetScriptState(box);
    REQUIRE(stored.has_value());
    CHECK(stored->asset.path == "scripts/wobble.lua");

    // --- Remove ---
    auto beforeRemove = *stored;
    auto cmdRem = MakeSetScriptCommandIfEffective(box, beforeRemove, std::nullopt);
    REQUIRE(cmdRem);
    CHECK(cmdRem->Description() == "Remove Script");
    auto r5 = history.Execute(std::move(cmdRem), fx.manager);
    REQUIRE(r5.success);
    CHECK_FALSE(fx.manager.HasScript(fx.EntityOf(box)));

    // --- Undo remove ---
    auto r6 = history.Undo(fx.manager);
    REQUIRE(r6.success);
    stored = fx.manager.GetScriptState(box);
    REQUIRE(stored.has_value());
    CHECK(stored->asset.path == "scripts/wobble.lua");
    CHECK(std::get<double>(stored->fieldValues.at("speed").value) == doctest::Approx(9.0));

    // --- Redo remove ---
    auto r7 = history.Redo(fx.manager);
    REQUIRE(r7.success);
    CHECK_FALSE(fx.manager.HasScript(fx.EntityOf(box)));
}

TEST_CASE("Phase6B W4: remove with all seven field types; Undo restores every tag and payload")
{
    using namespace rt2::core;
    ScriptFixture fx;
    EditorCommandHistory history;
    const auto box = fx.AddBox("Box");

    ScriptComponent full;
    full.asset.kind = AssetKind::Script;
    full.asset.path = "scripts/full.lua";
    full.fieldValues["enabled"] = { ScriptFieldType::Bool,   true };
    full.fieldValues["count"]   = { ScriptFieldType::Int,    int64_t{42} };
    full.fieldValues["speed"]   = { ScriptFieldType::Float,  7.5 };
    full.fieldValues["label"]   = { ScriptFieldType::String, std::string("runner") };
    full.fieldValues["target"]  = { ScriptFieldType::Uuid,   UUID::Nil() };
    full.fieldValues["offset"]  = { ScriptFieldType::Vec3,   glm::vec3(1, 2, 3) };
    full.fieldValues["tint"]    = { ScriptFieldType::Color,  glm::vec3(0.2f, 0.4f, 0.6f) };

    auto cmdAdd = MakeSetScriptCommandIfEffective(box, std::nullopt, full);
    REQUIRE(cmdAdd);
    REQUIRE(history.Execute(std::move(cmdAdd), fx.manager).success);

    const auto stored = fx.manager.GetScriptState(box);
    REQUIRE(stored.has_value());

    auto cmdRem = MakeSetScriptCommandIfEffective(box, *stored, std::nullopt);
    REQUIRE(cmdRem);
    REQUIRE(history.Execute(std::move(cmdRem), fx.manager).success);
    CHECK_FALSE(fx.manager.HasScript(fx.EntityOf(box)));

    // Undo remove → restore every type tag and payload exactly.
    REQUIRE(history.Undo(fx.manager).success);
    const auto restored = fx.manager.GetScriptState(box);
    REQUIRE(restored.has_value());
    CHECK(restored->fieldValues == full.fieldValues);

    // Redo remove.
    REQUIRE(history.Redo(fx.manager).success);
    CHECK_FALSE(fx.manager.HasScript(fx.EntityOf(box)));
}

TEST_CASE("Phase6B W4: each Execute/Undo/Redo reports SyncImpact::None and target UUID")
{
    ScriptFixture fx;
    EditorCommandHistory history;
    const auto box = fx.AddBox("Box");

    auto cmdAdd = MakeSetScriptCommandIfEffective(box, std::nullopt, MakeScript("s.lua", 1.0));
    REQUIRE(cmdAdd);
    auto r0 = history.Execute(std::move(cmdAdd), fx.manager);
    REQUIRE(r0.success);
    CHECK(r0.syncImpact == rt2::core::SyncImpact::None);
    REQUIRE(r0.affectedEntities.size() == 1);
    CHECK(r0.affectedEntities[0] == box);

    auto r1 = history.Undo(fx.manager);
    REQUIRE(r1.success);
    CHECK(r1.syncImpact == rt2::core::SyncImpact::None);
    REQUIRE(r1.affectedEntities.size() == 1);
    CHECK(r1.affectedEntities[0] == box);

    auto r2 = history.Redo(fx.manager);
    REQUIRE(r2.success);
    CHECK(r2.syncImpact == rt2::core::SyncImpact::None);
    REQUIRE(r2.affectedEntities.size() == 1);
    CHECK(r2.affectedEntities[0] == box);
}

TEST_CASE("Phase6B W4: no-op factory output never changes revision, dirty, or history")
{
    ScriptFixture fx;
    EditorCommandHistory history;
    const auto box = fx.AddBox("Box");
    fx.manager.ClearDirty();
    const uint64_t rev0 = fx.manager.AuthoringRevision();

    auto cmd = MakeSetScriptCommandIfEffective(box, std::nullopt, std::nullopt);
    REQUIRE(!cmd);
    CHECK(fx.manager.AuthoringRevision() == rev0);
    CHECK_FALSE(fx.manager.AuthoringDoc().metadata.dirty);
    CHECK_FALSE(history.CanUndo());
    CHECK_FALSE(history.CanRedo());
}

TEST_CASE("Phase6B W4: invalid after-state fails atomically and does not enter history")
{
    ScriptFixture fx;
    EditorCommandHistory history;
    const auto box = fx.AddBox("Box");
    fx.manager.ClearDirty();
    const uint64_t rev0 = fx.manager.AuthoringRevision();

    // Invalid after-state: mismatched type/payload arm.
    ScriptComponent invalid = MakeScript("scripts/spin.lua", 5.0);
    invalid.fieldValues["speed"].type = rt2::core::ScriptFieldType::Bool;

    auto cmd = MakeSetScriptCommandIfEffective(box, std::nullopt, invalid);
    REQUIRE(cmd); // factory does NOT suppress invalid after-states
    auto r = history.Execute(std::move(cmd), fx.manager);
    CHECK_FALSE(r.success);
    CHECK_FALSE(fx.manager.HasScript(fx.EntityOf(box)));
    CHECK(fx.manager.AuthoringRevision() == rev0);
    CHECK_FALSE(fx.manager.AuthoringDoc().metadata.dirty);
    CHECK_FALSE(history.CanUndo());
    CHECK_FALSE(history.CanRedo());
}

TEST_CASE("Phase6B W4: missing target fails gracefully and leaves both stacks unchanged")
{
    ScriptFixture fx;
    EditorCommandHistory history;
    fx.AddBox("Box");

    const auto ghost = rt2::core::UUID::Parse("00000000-0000-4000-8000-000000000abc");
    auto cmd = MakeSetScriptCommandIfEffective(ghost, std::nullopt, MakeScript("s.lua", 1.0));
    REQUIRE(cmd);
    auto r = history.Execute(std::move(cmd), fx.manager);
    CHECK_FALSE(r.success);

    // History should be empty (failed Execute leaves both stacks unchanged).
    CHECK_FALSE(history.CanUndo());
    CHECK_FALSE(history.CanRedo());
}

TEST_CASE("Phase6B W4: failed Execute does not clear the redo stack")
{
    ScriptFixture fx;
    EditorCommandHistory history;
    const auto box = fx.AddBox("Box");

    // Establish one undo entry and one redo entry.
    auto cmdAdd = MakeSetScriptCommandIfEffective(box, std::nullopt, MakeScript("s.lua", 1.0));
    REQUIRE(history.Execute(std::move(cmdAdd), fx.manager).success);
    REQUIRE(history.Undo(fx.manager).success);
    REQUIRE(history.CanRedo());

    // Now attempt a failed Execute (invalid after-state on a different entity).
    const auto box2 = fx.AddBox("Box2");
    ScriptComponent invalid = MakeScript("s.lua", 1.0);
    invalid.fieldValues["speed"].type = rt2::core::ScriptFieldType::Bool;
    auto cmdBad = MakeSetScriptCommandIfEffective(box2, std::nullopt, invalid);
    REQUIRE(cmdBad);
    auto r = history.Execute(std::move(cmdBad), fx.manager);
    CHECK_FALSE(r.success);

    // Redo stack must survive a failed Execute.
    CHECK(history.CanRedo());
}

TEST_CASE("Phase6B W4: staged working-copy edits do not touch document until Execute")
{
    ScriptFixture fx;
    EditorCommandHistory history;
    const auto box = fx.AddBox("Box");

    // Capture canonical before-state.
    const auto before = std::optional<ScriptComponent>{};
    fx.manager.ClearDirty();
    const uint64_t rev0 = fx.manager.AuthoringRevision();

    // Simulate W5: build an after-state off-document without calling
    // SetScriptState per frame.
    auto stagedAfter = MakeScript("scripts/spin.lua", 3.0);

    // Document is untouched while staging.
    CHECK_FALSE(fx.manager.HasScript(fx.EntityOf(box)));
    CHECK(fx.manager.AuthoringRevision() == rev0);
    CHECK_FALSE(fx.manager.AuthoringDoc().metadata.dirty);

    // Commit via history.
    auto cmd = MakeSetScriptCommandIfEffective(box, before, stagedAfter);
    REQUIRE(cmd);
    auto r = history.Execute(std::move(cmd), fx.manager);
    REQUIRE(r.success);
    CHECK(fx.manager.HasScript(fx.EntityOf(box)));
    const auto stored = fx.manager.GetScriptState(box);
    REQUIRE(stored.has_value());
    CHECK(std::get<double>(stored->fieldValues.at("speed").value) == doctest::Approx(3.0));
}

TEST_CASE("Phase6B W4: net-zero staged edit produces no command")
{
    ScriptFixture fx;
    EditorCommandHistory history;
    const auto box = fx.AddBox("Box");

    const auto script = MakeScript("scripts/spin.lua", 5.0);
    REQUIRE(fx.manager.SetScriptState(box, script).success);
    fx.manager.ClearDirty();
    const uint64_t rev0 = fx.manager.AuthoringRevision();

    const auto stored = fx.manager.GetScriptState(box);
    REQUIRE(stored.has_value());

    // Simulate a drag that returned to its exact initial value.
    auto stagedAfter = *stored;
    auto cmd = MakeSetScriptCommandIfEffective(box, *stored, stagedAfter);
    REQUIRE(!cmd); // no command, no dirty, no revision bump
    CHECK(fx.manager.AuthoringRevision() == rev0);
    CHECK_FALSE(fx.manager.AuthoringDoc().metadata.dirty);
}

TEST_CASE("Phase6B W4: EditorSyncRouter records zero sync for script commands")
{
    ScriptFixture fx;
    EditorCommandHistory history;
    const auto box = fx.AddBox("Box");

    RecordingBridge bridge;
    EditorSyncRouter router;
    int transformCalls = 0, materialCalls = 0, fullCalls = 0, resetCalls = 0;
    router.SetTransformSync([&] { ++transformCalls; });
    router.SetMaterialSync([&] { ++materialCalls; });
    router.SetFullSync([&] { ++fullCalls; });
    router.SetResetAccum([&] { ++resetCalls; });
    router.SetRendererAvailable([&] { return true; });
    router.SetTextureUploadPending([&] { return false; });

    auto cmdAdd = MakeSetScriptCommandIfEffective(box, std::nullopt, MakeScript("s.lua", 1.0));
    REQUIRE(cmdAdd);
    auto r0 = history.Execute(std::move(cmdAdd), fx.manager);
    REQUIRE(r0.success);
    router.Route(r0, fx.manager);

    auto cmdEdit = MakeSetScriptCommandIfEffective(
        box, MakeScript("s.lua", 1.0), MakeScript("s.lua", 8.0));
    REQUIRE(cmdEdit);
    auto r1 = history.Execute(std::move(cmdEdit), fx.manager);
    REQUIRE(r1.success);
    router.Route(r1, fx.manager);

    auto r2 = history.Undo(fx.manager);
    REQUIRE(r2.success);
    router.Route(r2, fx.manager);

    auto r3 = history.Redo(fx.manager);
    REQUIRE(r3.success);
    router.Route(r3, fx.manager);

    auto cmdRem = MakeSetScriptCommandIfEffective(box, MakeScript("s.lua", 8.0), std::nullopt);
    REQUIRE(cmdRem);
    auto r4 = history.Execute(std::move(cmdRem), fx.manager);
    REQUIRE(r4.success);
    router.Route(r4, fx.manager);

    CHECK(transformCalls == 0);
    CHECK(materialCalls == 0);
    CHECK(fullCalls == 0);
    CHECK(resetCalls == 0);
}

TEST_CASE("Phase6B W4: save/reopen after Execute contains after-state; after Undo contains before-state")
{
    using namespace rt2::core;
    ScriptFixture fx;
    EditorCommandHistory history;
    // Use CreateEmpty so the entity has no MeshRef — AddObjectWithGeometry
    // creates legacy inline geometry the serializer rejects.
    const auto box = fx.manager.CreateEmpty("Box").affectedEntities.front();

    auto afterAdd = MakeScript("scripts/spin.lua", 6.0);
    afterAdd.fieldValues["tint"] = { ScriptFieldType::Color, glm::vec3(0.1f, 0.2f, 0.3f) };
    auto cmdAdd = MakeSetScriptCommandIfEffective(box, std::nullopt, afterAdd);
    REQUIRE(cmdAdd);
    REQUIRE(history.Execute(std::move(cmdAdd), fx.manager).success);

    // Save after Execute.
    const auto path = std::filesystem::temp_directory_path() / "rt2_w4_cmd_after.rt2scene";
    Error err;
    REQUIRE(SceneSerializer::Save(fx.manager.AuthoringDoc(), path, err));

    // Reopen and verify after-state.
    SceneDocument loaded;
    loaded.SetUuidProvider(&fx.ids);
    REQUIRE(SceneSerializer::Load(loaded, path, err));
    const auto loadedEntity = loaded.FindByUuid(box);
    REQUIRE(loadedEntity != static_cast<entt::entity>(entt::null));
    const auto& afterRt = loaded.ecs.registry.get<ScriptComponent>(loadedEntity);
    CHECK(afterRt.asset.path == "scripts/spin.lua");
    CHECK(std::get<double>(afterRt.fieldValues.at("speed").value) == doctest::Approx(6.0));
    CHECK(afterRt.fieldValues.at("tint").type == ScriptFieldType::Color);
    std::filesystem::remove(path);

    // Undo the add, then save and reopen — should have no script.
    REQUIRE(history.Undo(fx.manager).success);
    CHECK_FALSE(fx.manager.HasScript(fx.EntityOf(box)));
    const auto path2 = std::filesystem::temp_directory_path() / "rt2_w4_cmd_before.rt2scene";
    REQUIRE(SceneSerializer::Save(fx.manager.AuthoringDoc(), path2, err));
    SceneDocument loaded2;
    loaded2.SetUuidProvider(&fx.ids);
    REQUIRE(SceneSerializer::Load(loaded2, path2, err));
    const auto loaded2Entity = loaded2.FindByUuid(box);
    REQUIRE(loaded2Entity != static_cast<entt::entity>(entt::null));
    CHECK_FALSE(loaded2.ecs.registry.all_of<ScriptComponent>(loaded2Entity));
    std::filesystem::remove(path2);
}

TEST_CASE("Phase6B W4: unbound component add and remove round-trips")
{
    ScriptFixture fx;
    EditorCommandHistory history;
    const auto box = fx.AddBox("Box");

    ScriptComponent unbound;
    unbound.asset.kind = AssetKind::Script;

    auto cmdAdd = MakeSetScriptCommandIfEffective(box, std::nullopt, unbound);
    REQUIRE(cmdAdd);
    CHECK(cmdAdd->Description() == "Add Script");
    REQUIRE(history.Execute(std::move(cmdAdd), fx.manager).success);

    const auto stored = fx.manager.GetScriptState(box);
    REQUIRE(stored.has_value());
    CHECK(stored->asset.path.empty());
    CHECK(stored->asset.sourceKey.empty());
    CHECK(stored->fieldValues.empty());

    REQUIRE(history.Undo(fx.manager).success);
    CHECK_FALSE(fx.manager.HasScript(fx.EntityOf(box)));

    REQUIRE(history.Redo(fx.manager).success);
    const auto restored = fx.manager.GetScriptState(box);
    REQUIRE(restored.has_value());
    CHECK(restored->asset.path.empty());
    CHECK(restored->fieldValues.empty());
}

TEST_CASE("Phase6B W4: complete field-map replacement round-trips independently")
{
    using namespace rt2::core;
    ScriptFixture fx;
    EditorCommandHistory history;
    const auto box = fx.AddBox("Box");

    ScriptComponent first;
    first.asset.kind = AssetKind::Script;
    first.asset.path = "scripts/multi.lua";
    first.fieldValues["a"] = { ScriptFieldType::Float, 1.0 };
    first.fieldValues["b"] = { ScriptFieldType::Int,   int64_t{10} };

    auto cmdAdd = MakeSetScriptCommandIfEffective(box, std::nullopt, first);
    REQUIRE(history.Execute(std::move(cmdAdd), fx.manager).success);

    ScriptComponent second;
    second.asset.kind = AssetKind::Script;
    second.asset.path = "scripts/multi.lua";
    second.fieldValues["c"] = { ScriptFieldType::String, std::string("hello") };
    second.fieldValues["d"] = { ScriptFieldType::Bool,   true };

    auto cmdReplace = MakeSetScriptCommandIfEffective(box, first, second);
    REQUIRE(cmdReplace);
    REQUIRE(history.Execute(std::move(cmdReplace), fx.manager).success);

    const auto stored = fx.manager.GetScriptState(box);
    REQUIRE(stored.has_value());
    CHECK(stored->fieldValues.size() == 2);
    CHECK(stored->fieldValues.count("a") == 0);
    CHECK(stored->fieldValues.count("b") == 0);
    CHECK(stored->fieldValues.count("c") == 1);
    CHECK(stored->fieldValues.count("d") == 1);

    // Undo → back to first map.
    REQUIRE(history.Undo(fx.manager).success);
    const auto undone = fx.manager.GetScriptState(box);
    REQUIRE(undone.has_value());
    CHECK(undone->fieldValues.size() == 2);
    CHECK(undone->fieldValues.count("a") == 1);
    CHECK(undone->fieldValues.count("b") == 1);

    // Redo → second map.
    REQUIRE(history.Redo(fx.manager).success);
    const auto redone = fx.manager.GetScriptState(box);
    REQUIRE(redone.has_value());
    CHECK(redone->fieldValues.count("c") == 1);
    CHECK(redone->fieldValues.count("d") == 1);
}

// ============================================================================
// Phase 6B W4 review fixes — phantom-entry and canonical-snapshot coverage.
// ============================================================================

TEST_CASE("Phase6B W4: factory-emitted command suppressed by manager does not record in history")
{
    ScriptFixture fx;
    EditorCommandHistory history;
    const auto box = fx.AddBox("Box");

    // Add a script so the entity has a stored state.
    const auto initial = MakeScript("scripts/spin.lua", 5.0);
    REQUIRE(history.Execute(
        MakeSetScriptCommandIfEffective(box, std::nullopt, initial), fx.manager).success);
    REQUIRE(history.CanUndo());

    // Now simulate the W5 crossing case: capture a before-state from
    // GetScriptState, then an out-of-band mutation changes the stored state
    // before the command is submitted.
    const auto staleBefore = fx.manager.GetScriptState(box);
    REQUIRE(staleBefore.has_value());

    // Out-of-band: set the field to 9.0 directly (e.g. via a prior undo or
    // another command). Now stored != staleBefore.
    REQUIRE(fx.manager.SetScriptState(box, MakeScript("scripts/spin.lua", 9.0)).success);

    // The user commits a staged edit whose value happens to equal the current
    // stored state (9.0). The factory sees staleBefore(5.0) != after(9.0) and
    // emits a command. The manager sees stored(9.0) == after(9.0) and
    // suppresses — success=true, effective=false.
    auto after = MakeScript("scripts/spin.lua", 9.0);
    auto cmd = MakeSetScriptCommandIfEffective(box, *staleBefore, after);
    REQUIRE(cmd);
    auto r = history.Execute(std::move(cmd), fx.manager);
    CHECK(r.success);
    CHECK_FALSE(r.effective);

    // The phantom entry must NOT be in history. The undo stack still has
    // only the original add, and redo is NOT cleared (because the suppressed
    // command was not an effective submission).
    REQUIRE(history.CanUndo());
    CHECK(history.UndoDescription() == "Add Script");

    // Undo the original add — should work cleanly.
    auto r2 = history.Undo(fx.manager);
    REQUIRE(r2.success);
    CHECK_FALSE(fx.manager.HasScript(fx.EntityOf(box)));
}

TEST_CASE("Phase6B W4: add-command with stale sourceKey stores canonical after-value")
{
    ScriptFixture fx;
    EditorCommandHistory history;
    const auto box = fx.AddBox("Box");

    // An add-command with a deliberately stale sourceKey and non-default
    // importSettings. The factory must canonicalize the after-state on the
    // add path (before is absent), so AfterValue() matches what the manager
    // will store.
    ScriptComponent raw;
    raw.asset.kind = AssetKind::Script;
    raw.asset.path = "scripts/spin.lua";
    raw.asset.sourceKey = "lua:asset=scripts/old.lua"; // stale
    raw.asset.importSettings.triangulate = false;       // non-default
    raw.fieldValues["speed"] = { rt2::core::ScriptFieldType::Float, 3.0 };

    auto cmd = MakeSetScriptCommandIfEffective(box, std::nullopt, raw);
    REQUIRE(cmd);
    CHECK(cmd->Description() == "Add Script");

    // Cast to SetScriptCommand to inspect the stored after-value.
    auto* scriptCmd = dynamic_cast<SetScriptCommand*>(cmd.get());
    REQUIRE(scriptCmd != nullptr);
    const auto& afterVal = scriptCmd->AfterValue();
    REQUIRE(afterVal.has_value());
    CHECK(afterVal->asset.sourceKey == "lua:asset=scripts/spin.lua");
    CHECK(afterVal->asset.importSettings.triangulate == true); // reset to default

    // Execute and confirm the document matches the command's snapshot.
    auto r = history.Execute(std::move(cmd), fx.manager);
    REQUIRE(r.success);
    const auto stored = fx.manager.GetScriptState(box);
    REQUIRE(stored.has_value());
    CHECK(stored->asset.sourceKey == afterVal->asset.sourceKey);
    CHECK(stored->asset.importSettings.triangulate == afterVal->asset.importSettings.triangulate);
    CHECK(stored->fieldValues == afterVal->fieldValues);
}

TEST_CASE("Phase6B W4: absent→absent command describes itself as no change")
{
    // The factory rejects this shape, but SetScriptCommand is publicly
    // constructible. The description must be self-identifying rather than
    // the misleading "Edit Script".
    rt2::core::UUID target = rt2::core::UUID::Parse("00000000-0000-4000-8000-000000000001");
    SetScriptCommand cmd(target, std::nullopt, std::nullopt);
    CHECK(cmd.Description() == "Script (no change)");
}

// ============================================================================
// Phase 6B W5 — Inspector guard tests.
//
// These tests cover the PropertyEditSession<ScriptComponent> lifecycle and
// the registry fast-path staleness gate. The interactive acceptance gate
// (W5.7) is manual and cannot be automated without a GLFW/ImGui harness.
// ============================================================================

TEST_CASE("Phase6B W5: PropertyEditSession<ScriptComponent> lifecycle")
{
    PropertyEditSession<ScriptComponent> session;
    CHECK_FALSE(session.IsOpen());

    ScriptComponent before = MakeScript("scripts/spin.lua", 5.0);
    const rt2::core::UUID target = rt2::core::UUID::Parse("00000000-0000-4000-8000-000000000001");

    // Open.
    session.OnActivated(target, before);
    CHECK(session.IsOpen());
    CHECK(session.Target() == target);

    // Close without a commit → no record.
    auto rec = session.CloseDeferred(before);
    CHECK_FALSE(rec.has_value());
    CHECK_FALSE(session.IsOpen());

    // Open + commit + close → record.
    session.OnActivated(target, before);
    session.OnEditCommitted();
    ScriptComponent after = MakeScript("scripts/spin.lua", 9.0);
    rec = session.CloseDeferred(after);
    REQUIRE(rec.has_value());
    CHECK(std::get<double>(rec->before.fieldValues.at("speed").value) == doctest::Approx(5.0));
    CHECK(std::get<double>(rec->after.fieldValues.at("speed").value) == doctest::Approx(9.0));
    CHECK_FALSE(session.IsOpen());

    // Open + cancel → no record.
    session.OnActivated(target, before);
    session.OnCancelled();
    CHECK_FALSE(session.IsOpen());
    rec = session.CloseDeferred(after);
    CHECK_FALSE(rec.has_value());

    // Open + commit + guard failure → no record.
    session.OnActivated(target, before);
    session.OnEditCommitted();
    rec = session.CloseDeferred(after, { []() { return false; } });
    CHECK_FALSE(rec.has_value());
    CHECK_FALSE(session.IsOpen());
}

TEST_CASE("Phase6B W5: registry fast-path avoids re-reading unchanged files")
{
    ScriptDir dir;
    const auto path = dir.Write("fast.lua", R"(
rt2.fields = { speed = rt2.field.float(5.0) }
)");

    rt2::core::ScriptFieldRegistry registry;
    auto r1 = registry.GetDeclaredFields(path);
    REQUIRE(r1.parsed);
    REQUIRE(r1.descriptors.size() == 1);
    CHECK(r1.descriptors[0].name == "speed");

    // Second query: (mtime, size) unchanged → should return cached without
    // re-reading or re-parsing. Verify it still returns the same descriptors.
    auto r2 = registry.GetDeclaredFields(path);
    REQUIRE(r2.parsed);
    REQUIRE(r2.descriptors.size() == 1);
    CHECK(r2.descriptors[0].name == "speed");

    // Modify the file (change size) → cache should invalidate.
    dir.Write("fast.lua", R"(
rt2.fields = { speed = rt2.field.float(5.0), height = rt2.field.float(2.0) }
)");
    auto r3 = registry.GetDeclaredFields(path);
    REQUIRE(r3.parsed);
    REQUIRE(r3.descriptors.size() == 2);
}

TEST_CASE("Phase6B W5: registry fast-path with same-size edit re-parses on hash mismatch")
{
    ScriptDir dir;
    // Write a file, then rewrite with the same byte count but different content.
    dir.Write("same.lua", R"(
rt2.fields = { speed = rt2.field.float(5.0) }
)");
    // "5.0" → "9.0" is same length, but we also need the same total file size.
    // Construct two scripts with identical byte count.
    const std::string original = R"(rt2.fields = { speed = rt2.field.float(5.0) }
)";
    const std::string modified = R"(rt2.fields = { speed = rt2.field.float(9.0) }
)";
    REQUIRE(original.size() == modified.size());
    dir.Write("same.lua", original);

    rt2::core::ScriptFieldRegistry registry;
    auto r1 = registry.GetDeclaredFields(dir.root / "same.lua");
    REQUIRE(r1.parsed);
    REQUIRE(r1.descriptors.size() == 1);
    CHECK(std::get<double>(r1.descriptors[0].defaultValue) == doctest::Approx(5.0));

    // Rewrite with same size, different content. The fast-path (mtime, size)
    // may or may not catch this depending on timestamp granularity. If it
    // doesn't (same tick), the hash check will. Either way, the next query
    // after the timestamp advances must see the new default.
    dir.Write("same.lua", modified);
    // Sleep briefly to ensure the timestamp advances.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto r2 = registry.GetDeclaredFields(dir.root / "same.lua");
    REQUIRE(r2.parsed);
    REQUIRE(r2.descriptors.size() == 1);
    CHECK(std::get<double>(r2.descriptors[0].defaultValue) == doctest::Approx(9.0));
}
