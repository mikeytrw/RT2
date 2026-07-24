#include <doctest/doctest.h>

#include "ScriptSystem.h"
#include "ScriptScenarioCompare.h"
#include "ScriptFileWatchPolicy.h"
#include "SceneRunState.h"
#include "RuntimeSceneController.h"
#include "RuntimeLifecycleObserver.h"
#include "IRuntimeScriptDispatch.h"
#include "IRuntimeCommandSink.h"
#include "SceneDocument.h"
#include "ECSComponents.h"
#include "ECSScene.h"
#include "PrimitiveGeometry.h"
#include "InputTypes.h"
#include "ISceneRenderBridge.h"
#include "GPUSceneData.h"
#include "core/UUID.h"
#include "core/Error.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace rt2::core;

// ============================================================================
// Phase 6C tests (W8).
//
// Two halves:
//
//  1. The pure seams extracted in W8 — ScriptScenarioCompare.h (the headless
//     runner's verdict logic) and ScriptFileWatchPolicy.h (what a changed
//     .lua file means for the current run state). No Lua, no scene.
//
//  2. Integration cases chosen from what was BROKEN AND INVISIBLE during the
//     W0-W7 reviews. Each of these passed review only because someone read
//     the code; none of them had a test that would have failed. They are
//     written to fail loudly if the corresponding fix is ever reverted:
//
//       - timer vector reallocation during FireTimers (was a UAF)
//       - rt2:reload() re-entering the Lua stack (was synchronous)
//       - a quarantined instance being invisible to the headless runner
//       - set_camera fabricating a component on a non-camera entity
//       - ReloadScript failing to match a native-separator absolute path
// ============================================================================

namespace {

// ---- Test doubles (suffixed 6C: Phase6A/6B declare their own at file
// scope in other TUs, and identically-named classes would collide at link
// time) --------------------------------------------------------------------

class NullRenderBridge6C final : public ISceneRenderBridge
{
public:
    void FullSync(GPUSceneData&) override      {}
    void MaterialSync(GPUSceneData&) override  {}
    void TransformSync(GPUSceneData&) override {}
    void ResetTemporalState() override         {}
    void RequestRender() override              {}
};

// Inert by default (every action None, every axis 0), so tests that don't
// care about input can ignore it; settable so the binding tests can drive
// specific values through the Lua `input` table.
class MockInputService6C final : public IInputService
{
public:
    std::map<std::string, ActionState> actions;
    std::map<std::string, float>       axes;
    glm::vec2                          mouseDelta{0.0f, 0.0f};
    float                              scrollDelta = 0.0f;
    mutable bool                       cursorCaptured = false;

    ActionState GetActionState(const std::string& name) const override
    {
        auto it = actions.find(name);
        return it == actions.end() ? ActionState::None : it->second;
    }
    float GetAxisValue(const std::string& name) const override
    {
        auto it = axes.find(name);
        return it == axes.end() ? 0.0f : it->second;
    }
    glm::vec2 GetMouseDelta() const override { return mouseDelta; }
    float GetScrollDelta() const override { return scrollDelta; }
    void RequestCursorCapture(bool locked) override { cursorCaptured = locked; }
    bool IsCursorCaptureRequested() const override { return cursorCaptured; }
};

// Lazily created on first use rather than by a setup TEST_CASE. A setup case
// only works when it runs first, which doctest guarantees under declaration
// order but not under --test-case= filtering or shuffled ordering — and a
// silently-empty temp dir would make every path relative to the CWD instead
// of failing.
const std::filesystem::path& TempDir6C()
{
    static const std::filesystem::path dir = [] {
        auto d = std::filesystem::temp_directory_path() / "rt2_phase6c_tests";
        std::filesystem::remove_all(d);
        std::filesystem::create_directories(d);
        return d;
    }();
    return dir;
}

std::filesystem::path WriteScript6C(const std::string& name,
                                    const std::string& source)
{
    auto path = TempDir6C() / name;
    std::ofstream f(path, std::ios::binary);
    f << source;
    f.close();
    return path;
}

struct Harness6C
{
    DeterministicUuidProvider uuidProv;
    NullRenderBridge6C        bridge;
    RuntimeSceneController    ctrl;
    ScriptSystem              scriptSys;
    RuntimeCommandSink        sink;
    MockInputService6C        input;

    Harness6C()
        : scriptSys(uuidProv)
        , sink(ctrl)
    {
        ctrl.SetRuntimeUuidProvider(&uuidProv);
        ctrl.SetLifecycleObserver(&scriptSys);
        ctrl.SetScriptDispatch(&scriptSys);
        ctrl.SetInputService(&input);
        ctrl.SetRuntimeCommandSink(&sink);
    }

    bool Play(const SceneDocument& doc)
    {
        Error err;
        return ctrl.Play(doc, bridge, err);
    }

    void Update(float dt) { ctrl.Update(dt, bridge); }
    void Stop(const SceneDocument& doc) { ctrl.Stop(doc, bridge); }
    void Pause()  { ctrl.Pause(); }
    bool Resume() { return ctrl.Resume(); }
};

struct CubeOptions
{
    bool withLight = false;
    // Authored ScriptComponent field values, as the serializer would have
    // loaded them. Reconciliation on reload must preserve these.
    ScriptFieldMap fieldValues;
};

// One scripted cube. The script path is scene-relative, resolved against
// metadata.sourcePath the same way the editor resolves it.
SceneDocument BuildScriptedCube(Harness6C& h, const std::string& scriptName,
                                const CubeOptions& opts = {})
{
    SceneDocument doc;
    doc.metadata.sourcePath = TempDir6C() / "fixture.rt2scene";

    SceneMaterial mat;
    doc.ecs.materials.push_back(mat);

    MeshData cubeMesh = PrimitiveGeometry::CreateCube(1.0f);
    cubeMesh.name = "cube";
    const uint32_t meshIdx = doc.ecs.meshRegistry.AddMesh(std::move(cubeMesh));

    entt::entity cube = doc.ecs.registry.create();
    doc.ecs.registry.emplace<NameComponent>(cube, "ScriptedCube");
    Transform& tf = doc.ecs.registry.emplace<Transform>(cube);
    tf.dirty = true;
    doc.ecs.registry.emplace<VisibleComponent>(cube);
    doc.ecs.registry.emplace<MeshRef>(cube, meshIdx, 0);
    doc.ecs.registry.emplace<PrimitiveComponent>(cube,
        PrimitiveComponent{ PrimitiveComponent::Cube, 1.0f, 24, 16 });

    if (opts.withLight)
    {
        LightComponent lc;
        lc.intensity = 1.0f;
        lc.color = {1.0f, 1.0f, 1.0f};
        doc.ecs.registry.emplace<LightComponent>(cube, lc);
    }

    ScriptComponent sc;
    sc.asset.kind = AssetKind::Script;
    sc.asset.path = scriptName;
    sc.asset.sourceKey = "lua:asset=" + scriptName;
    sc.fieldValues = opts.fieldValues;
    doc.ecs.registry.emplace<ScriptComponent>(cube, sc);

    doc.SetUuidProvider(&h.uuidProv);
    auto view = doc.ecs.registry.view<NameComponent>();
    for (auto e : view)
    {
        if (!doc.ecs.registry.all_of<EntityIdComponent>(e))
            doc.AssignNewUuid(e);
    }
    return doc;
}

UUID FirstScriptUuid(const SceneDocument& doc)
{
    auto view = doc.ecs.registry.view<ScriptComponent, EntityIdComponent>();
    for (auto e : view)
        return view.get<EntityIdComponent>(e).id;
    return UUID::Nil();
}

// Read back through the runtime document, not the authoring one: scripts
// mutate the runtime copy and Stop discards it.
std::string RuntimeName(Harness6C& h, const UUID& uuid)
{
    const SceneDocument* rt = h.ctrl.TryGetRuntimeScene();
    if (!rt) return {};
    const auto e = rt->FindByUuid(uuid);
    if (e == entt::null) return {};
    const auto* nc = rt->ecs.registry.try_get<NameComponent>(e);
    return nc ? nc->name : std::string{};
}

glm::vec3 RuntimePosition(Harness6C& h, const UUID& uuid)
{
    const SceneDocument* rt = h.ctrl.TryGetRuntimeScene();
    if (!rt) return {};
    const auto e = rt->FindByUuid(uuid);
    if (e == entt::null) return {};
    const auto* tf = rt->ecs.registry.try_get<Transform>(e);
    return tf ? tf->translation : glm::vec3{};
}

ScenarioEntityState MakeEntity(const char* uuid, glm::vec3 pos)
{
    ScenarioEntityState s;
    s.uuid = uuid;
    s.translation = pos;
    return s;
}

} // anonymous namespace

// ============================================================================
// Part 1 — pure seams
// ============================================================================

TEST_SUITE("Phase 6C scenario compare")
{
    TEST_CASE("Phase 6C: position inside epsilon passes, outside fails")
    {
        std::vector<ScenarioEntityState> actual{
            MakeEntity("A", {1.0f, 2.0f, 3.0f}) };

        ScenarioExpectation exp;
        exp.uuid = "A";
        exp.hasPosition = true;

        // Well inside the 1e-4 radius.
        exp.position = {1.00001f, 2.0f, 3.0f};
        CHECK(CompareTransforms(actual, {exp}).empty());

        // Well outside it.
        exp.position = {1.5f, 2.0f, 3.0f};
        auto m = CompareTransforms(actual, {exp});
        REQUIRE(m.size() == 1);
        CHECK(m[0].uuid == "A");
        CHECK(m[0].field == "position");
    }

    TEST_CASE("Phase 6C: rotation comparison absorbs quaternion double-cover")
    {
        // q and -q are the same orientation. A naive component-wise compare
        // would report a mismatch for every axis.
        ScenarioEntityState ent;
        ent.uuid = "A";
        ent.rotation = glm::quat(0.7071068f, 0.7071068f, 0.0f, 0.0f);

        ScenarioExpectation exp;
        exp.uuid = "A";
        exp.hasRotation = true;
        exp.rotation = glm::quat(-0.7071068f, -0.7071068f, 0.0f, 0.0f);

        CHECK(CompareTransforms({ent}, {exp}).empty());

        // A genuinely different orientation still fails.
        exp.rotation = glm::quat(0.7071068f, 0.0f, 0.7071068f, 0.0f);
        CHECK(CompareTransforms({ent}, {exp}).size() == 1);
    }

    TEST_CASE("Phase 6C: an expectation for a destroyed entity is a mismatch")
    {
        // The original Main.cpp loop iterated ENTITIES and skipped any without
        // an expectation, so a script that destroyed the entity under test
        // produced a clean pass. Driving off the expectation list catches it.
        std::vector<ScenarioEntityState> actual{
            MakeEntity("A", {0.0f, 0.0f, 0.0f}) };

        ScenarioExpectation exp;
        exp.uuid = "GONE";
        exp.hasPosition = true;
        exp.position = {1.0f, 0.0f, 0.0f};

        auto m = CompareTransforms(actual, {exp});
        REQUIRE(m.size() == 1);
        CHECK(m[0].uuid == "GONE");
        CHECK(m[0].field == "entity");
        CHECK(m[0].actual == "<missing>");
    }

    TEST_CASE("Phase 6C: unpinned fields are not compared")
    {
        // A scenario that pins only position must not fail because the scale
        // happens to differ from the struct default.
        ScenarioEntityState ent;
        ent.uuid = "A";
        ent.translation = {1.0f, 0.0f, 0.0f};
        ent.scale = {7.0f, 7.0f, 7.0f};
        ent.rotation = glm::quat(0.0f, 1.0f, 0.0f, 0.0f);

        ScenarioExpectation exp;
        exp.uuid = "A";
        exp.hasPosition = true;
        exp.position = {1.0f, 0.0f, 0.0f};

        CHECK(CompareTransforms({ent}, {exp}).empty());

        // And an empty expectation list never fails.
        CHECK(CompareTransforms({ent}, {}).empty());
    }

    TEST_CASE("Phase 6C: exit code precedence is script error > mismatch > pass")
    {
        ScenarioResult r;
        CHECK(r.Pass());
        CHECK(r.Exit() == ScenarioExit::Pass);

        r.spawnViolation = true;
        CHECK_FALSE(r.Pass());
        CHECK(r.Exit() == ScenarioExit::ExpectationFailed);

        r.mismatches.push_back({"A", "position", "x", "y"});
        CHECK(r.Exit() == ScenarioExit::ExpectationFailed);

        // A dead script outranks the transforms it failed to write.
        r.scriptError = true;
        CHECK(r.Exit() == ScenarioExit::ScriptError);

        // The numeric contract the docs and run_script_test.ps1 depend on.
        CHECK(static_cast<int>(ScenarioExit::Pass) == 0);
        CHECK(static_cast<int>(ScenarioExit::ScenarioParse) == 1);
        CHECK(static_cast<int>(ScenarioExit::SceneLoad) == 2);
        CHECK(static_cast<int>(ScenarioExit::PlayFailed) == 3);
        CHECK(static_cast<int>(ScenarioExit::OutputFailed) == 4);
        CHECK(static_cast<int>(ScenarioExit::ExpectationFailed) == 5);
        CHECK(static_cast<int>(ScenarioExit::ScriptError) == 6);
    }

    TEST_CASE("Phase 6C: script-error and spawn-violation detection")
    {
        // Quarantine is always an error.
        CHECK(DetectScriptError(0, 1, 1));
        CHECK(DetectScriptError(3, 1, 4));

        // No live instances while entities remain means the script never
        // loaded — the case that used to report a clean pass with every
        // transform sitting at its authored value.
        CHECK(DetectScriptError(0, 0, 1));

        // A genuinely empty scene is not an error.
        CHECK_FALSE(DetectScriptError(0, 0, 0));
        CHECK_FALSE(DetectScriptError(1, 0, 1));

        // forbidSpawn ratchets against the authoring count.
        CHECK(DetectSpawnViolation(true, 3, 2));
        CHECK_FALSE(DetectSpawnViolation(true, 2, 2));
        CHECK_FALSE(DetectSpawnViolation(true, 1, 2));  // net destroy is fine
        CHECK_FALSE(DetectSpawnViolation(false, 99, 2));
    }
}

TEST_SUITE("Phase 6C file watch policy")
{
    TEST_CASE("Phase 6C: Paused reloads rather than only invalidating")
    {
        // Playing and Paused both hand off to ScriptSystem::ReloadScript,
        // which owns the three-way branch. Gating on Playing here is what
        // made its Paused branch — and the whole pending-reload queue —
        // unreachable from the watcher.
        CHECK(DecideScriptFileChange(SceneRunState::Playing, true, true)
                  .reloadScript);
        CHECK(DecideScriptFileChange(SceneRunState::Paused, true, true)
                  .reloadScript);
        CHECK_FALSE(DecideScriptFileChange(SceneRunState::Edit, true, true)
                        .reloadScript);
    }

    TEST_CASE("Phase 6C: the inspector cache is invalidated in every run state")
    {
        // The two effects are independent. ScriptSystem::m_FieldRegistry and
        // WalnutApp::m_InspectorFieldRegistry are SEPARATE caches, so a
        // running reload does not refresh what the inspector displays. An
        // either/or policy left the inspector showing declarations parsed
        // from the pre-edit file for the whole Play session.
        for (auto state : { SceneRunState::Edit,
                            SceneRunState::Playing,
                            SceneRunState::Paused })
        {
            CHECK(DecideScriptFileChange(state, true, true)
                      .invalidateFieldRegistry);
        }
    }

    TEST_CASE("Phase 6C: missing subsystems degrade instead of dereferencing")
    {
        // Each flag is gated on its own subsystem being present, so neither
        // can drive a call through a null pointer.
        const auto noScriptSys =
            DecideScriptFileChange(SceneRunState::Playing, false, true);
        CHECK_FALSE(noScriptSys.reloadScript);
        CHECK(noScriptSys.invalidateFieldRegistry);

        const auto neither =
            DecideScriptFileChange(SceneRunState::Playing, false, false);
        CHECK_FALSE(neither.reloadScript);
        CHECK_FALSE(neither.invalidateFieldRegistry);

        const auto noRegistry =
            DecideScriptFileChange(SceneRunState::Edit, true, false);
        CHECK_FALSE(noRegistry.reloadScript);
        CHECK_FALSE(noRegistry.invalidateFieldRegistry);
    }
}

// ============================================================================
// Part 2 — integration
// ============================================================================

TEST_SUITE("Phase 6C scripting")
{
    // ------------------------------------------------------------------------
    // Regression: FireTimers used to hold a reference into m_Timers across
    // the callback. A callback that schedules a timer appends to that same
    // vector; once it outgrows its capacity the reference dangles and the
    // post-call re-index writes into freed memory.
    //
    // 50 timers all due on the same frame, each scheduling one more, takes
    // the vector from 50 to 100 entries mid-loop — several reallocations
    // while the loop is live.
    // ------------------------------------------------------------------------
    TEST_CASE("Phase 6C: self-rescheduling timers survive vector reallocation")
    {
        WriteScript6C("timer_realloc.lua", R"LUA(
function on_create(entity, world)
    for i = 1, 50 do
        timer:after(0.01, function()
            local p = entity:get_position()
            entity:set_position({ p[1] + 1.0, p[2], p[3] })
            -- Force growth of m_Timers from inside the callback.
            timer:after(1000.0, function() end)
        end)
    end
end

function on_update(entity, dt, input, world) end
)LUA");

        Harness6C h;
        auto doc = BuildScriptedCube(h, "timer_realloc.lua");
        const UUID cube = FirstScriptUuid(doc);
        REQUIRE(h.Play(doc));

        // One frame long enough to make all 50 due at once.
        h.Update(0.1f);

        CHECK(h.scriptSys.QuarantinedInstanceCount() == 0);
        CHECK(h.scriptSys.LiveInstanceCount() == 1);
        CHECK(RuntimePosition(h, cube).x == doctest::Approx(50.0f));

        h.Stop(doc);
    }

    // ------------------------------------------------------------------------
    // Regression: rt2:reload() used to call ReloadScript synchronously, from
    // inside a Lua callback — destroying the environment whose frame was
    // still on the stack. It now queues, and OnUpdate drains before any
    // callback runs.
    // ------------------------------------------------------------------------
    TEST_CASE("Phase 6C: rt2:reload() from on_update does not re-enter")
    {
        WriteScript6C("self_reload.lua", R"LUA(
function on_create(entity, world)
    entity:set_name("alive")
end

function on_update(entity, dt, input, world)
    rt2:reload()
    entity:set_name("alive")
end
)LUA");

        Harness6C h;
        auto doc = BuildScriptedCube(h, "self_reload.lua");
        const UUID cube = FirstScriptUuid(doc);
        REQUIRE(h.Play(doc));

        for (int i = 0; i < 5; ++i)
            h.Update(1.0f / 60.0f);

        CHECK(h.scriptSys.QuarantinedInstanceCount() == 0);
        CHECK(h.scriptSys.LiveInstanceCount() == 1);
        CHECK(RuntimeName(h, cube) == "alive");

        h.Stop(doc);
    }

    // ------------------------------------------------------------------------
    // A runtime error must quarantine the instance AND be visible to the
    // headless runner's verdict, which is what turns it into exit code 6
    // rather than a silent "no mismatches" pass.
    // ------------------------------------------------------------------------
    TEST_CASE("Phase 6C: a runtime error quarantines and surfaces as exit 6")
    {
        WriteScript6C("boom.lua", R"LUA(
function on_create(entity, world) end

function on_update(entity, dt, input, world)
    error("boom")
end
)LUA");

        Harness6C h;
        auto doc = BuildScriptedCube(h, "boom.lua");
        const UUID cube = FirstScriptUuid(doc);
        REQUIRE(h.Play(doc));
        REQUIRE(h.scriptSys.GetInstanceState(cube) == ScriptInstanceState::Live);

        h.Update(1.0f / 60.0f);

        CHECK(h.scriptSys.GetInstanceState(cube) ==
              ScriptInstanceState::Quarantined);
        CHECK(h.scriptSys.QuarantinedInstanceCount() == 1);
        CHECK(h.scriptSys.LiveInstanceCount() == 0);

        // The runner reads exactly these two counts.
        ScenarioResult r;
        r.liveInstances = h.scriptSys.LiveInstanceCount();
        r.quarantinedInstances = h.scriptSys.QuarantinedInstanceCount();
        r.runtimeEntityCount = 1;
        r.scriptError = DetectScriptError(r.liveInstances,
                                          r.quarantinedInstances,
                                          r.runtimeEntityCount);
        CHECK(r.scriptError);
        CHECK(r.Exit() == ScenarioExit::ScriptError);

        h.Stop(doc);
    }

    // ------------------------------------------------------------------------
    // set_camera/set_light are mutate-only. An earlier draft used emplace,
    // which would give a mesh a CameraComponent and leave the scene with two
    // cameras — a state the renderer does not define.
    // ------------------------------------------------------------------------
    TEST_CASE("Phase 6C: set_camera on a non-camera entity fails without emplacing")
    {
        WriteScript6C("no_camera.lua", R"LUA(
function on_create(entity, world) end

function on_update(entity, dt, input, world)
    local ok = entity:set_camera({ fov = 90.0 })
    entity:set_name(tostring(ok))
end
)LUA");

        Harness6C h;
        auto doc = BuildScriptedCube(h, "no_camera.lua");
        const UUID cube = FirstScriptUuid(doc);
        REQUIRE(h.Play(doc));

        h.Update(1.0f / 60.0f);

        CHECK(h.scriptSys.QuarantinedInstanceCount() == 0);
        CHECK(RuntimeName(h, cube) == "false");

        // The component must not have been fabricated.
        const SceneDocument* rt = h.ctrl.TryGetRuntimeScene();
        REQUIRE(rt != nullptr);
        const auto e = rt->FindByUuid(cube);
        // Extra parens: doctest's expression decomposition otherwise makes
        // entt's operator!=(Entity, null_t) ambiguous.
        REQUIRE((e != entt::null));
        CHECK(rt->ecs.registry.try_get<CameraComponent>(e) == nullptr);

        h.Stop(doc);
    }

    // ------------------------------------------------------------------------
    // timer:cancel marks a timer cancelled; FireTimers skips it and the GC
    // pass drops it. Nothing previously proved a cancelled timer stops
    // firing — the reallocation test proves the vector is sound, not that
    // the cancelled flag is honoured.
    // ------------------------------------------------------------------------
    TEST_CASE("Phase 6C: a cancelled timer does not fire")
    {
        // x: a repeating timer, never cancelled  -> fires every frame
        // y: cancelled before it can ever fire    -> must stay 0
        // z: cancels itself from inside its own callback -> fires exactly once
        WriteScript6C("timer_cancel.lua", R"LUA(
function on_create(entity, world)
    local hy = timer:every(0.01, function()
        local p = entity:get_position()
        entity:set_position({ p[1], p[2] + 1.0, p[3] })
    end)
    timer:cancel(hy)

    timer:every(0.01, function()
        local p = entity:get_position()
        entity:set_position({ p[1] + 1.0, p[2], p[3] })
    end)

    local hz
    hz = timer:every(0.01, function()
        local p = entity:get_position()
        entity:set_position({ p[1], p[2], p[3] + 1.0 })
        timer:cancel(hz)
    end)
end

function on_update(entity, dt, input, world) end
)LUA");

        Harness6C h;
        auto doc = BuildScriptedCube(h, "timer_cancel.lua");
        const UUID cube = FirstScriptUuid(doc);
        REQUIRE(h.Play(doc));

        for (int i = 0; i < 3; ++i)
            h.Update(0.1f);

        CHECK(h.scriptSys.QuarantinedInstanceCount() == 0);
        const glm::vec3 p = RuntimePosition(h, cube);
        CHECK(p.x == doctest::Approx(3.0f));  // uncancelled: one fire/frame
        CHECK(p.y == doctest::Approx(0.0f));  // cancelled before firing
        CHECK(p.z == doctest::Approx(1.0f));  // self-cancelled after one fire

        h.Stop(doc);
    }

    // ------------------------------------------------------------------------
    // L4: every mutating sink method is gated on IsRuntimeMutable(). Tested
    // at the sink rather than through Lua because the states where the gate
    // matters (before Play, after Stop) are exactly the states where no
    // script is running to observe it.
    // ------------------------------------------------------------------------
    TEST_CASE("Phase 6C: sink setters are gated on runtime mutability")
    {
        Harness6C h;
        auto doc = BuildScriptedCube(h, "gate.lua");
        WriteScript6C("gate.lua", "function on_update() end\n");
        const UUID cube = FirstScriptUuid(doc);

        // Before Play there is no runtime document at all.
        CHECK_FALSE(h.ctrl.IsRuntimeMutable());
        CHECK_FALSE(h.sink.SetPosition(cube, {1.0f, 0.0f, 0.0f}));
        CHECK_FALSE(h.sink.SetName(cube, "nope"));
        CHECK_FALSE(h.sink.SetVisible(cube, false));
        CHECK_FALSE(h.sink.SetLight(cube, LightComponent{}));
        CHECK_FALSE(h.sink.SetCamera(cube, CameraComponent{}));
        CHECK_FALSE(h.sink.SetMaterialIndex(cube, 0));

        REQUIRE(h.Play(doc));
        CHECK(h.ctrl.IsRuntimeMutable());
        CHECK(h.sink.SetPosition(cube, {1.0f, 0.0f, 0.0f}));

        h.Stop(doc);

        // After Stop the runtime document is gone again.
        CHECK_FALSE(h.ctrl.IsRuntimeMutable());
        CHECK_FALSE(h.sink.SetPosition(cube, {2.0f, 0.0f, 0.0f}));
        CHECK_FALSE(h.sink.SetMaterialIndex(cube, 0));
    }

    // ------------------------------------------------------------------------
    // W6: a reload copies the outgoing `self` table into rt2.previous_state
    // so a script can carry state across the swap. Previously only the
    // absence of a re-entrancy crash was covered, not that the old state is
    // actually reachable.
    // ------------------------------------------------------------------------
    TEST_CASE("Phase 6C: rt2.previous_state carries self across a reload")
    {
        auto scriptPath = WriteScript6C("prev_state.lua", R"LUA(
function on_create(entity, world)
    self.tag = "before"
end

function on_update(entity, dt, input, world)
    entity:set_name("v1")
end
)LUA");

        Harness6C h;
        auto doc = BuildScriptedCube(h, "prev_state.lua");
        const UUID cube = FirstScriptUuid(doc);
        REQUIRE(h.Play(doc));
        h.Update(1.0f / 60.0f);
        REQUIRE(RuntimeName(h, cube) == "v1");

        WriteScript6C("prev_state.lua", R"LUA(
function on_create(entity, world) end

function on_update(entity, dt, input, world)
    if rt2.previous_state == nil then
        entity:set_name("no-previous-state")
    else
        entity:set_name(tostring(rt2.previous_state.tag))
    end
end
)LUA");

        h.scriptSys.ReloadScript(
            std::filesystem::absolute(scriptPath).make_preferred());
        h.Update(1.0f / 60.0f);

        CHECK(h.scriptSys.QuarantinedInstanceCount() == 0);
        CHECK(RuntimeName(h, cube) == "before");

        h.Stop(doc);
    }

    // ------------------------------------------------------------------------
    // Plan W8 asks for "syntax error quarantines all instances of that
    // source". The CODE deliberately does the opposite (ScriptSystem.cpp
    // ~296): a parse failure keeps instances in their current state, because
    // the running code is still valid and the author is mid-keystroke. The
    // code's behaviour is the better one; this test pins it, and the plan
    // line is stale. Flagged for W9.
    // ------------------------------------------------------------------------
    TEST_CASE("Phase 6C: a mid-edit syntax error does not kill the running instance")
    {
        auto scriptPath = WriteScript6C("syntax.lua", R"LUA(
function on_create(entity, world) end

function on_update(entity, dt, input, world)
    entity:set_name("running")
end
)LUA");

        Harness6C h;
        auto doc = BuildScriptedCube(h, "syntax.lua");
        const UUID cube = FirstScriptUuid(doc);
        REQUIRE(h.Play(doc));
        h.Update(1.0f / 60.0f);
        REQUIRE(RuntimeName(h, cube) == "running");

        // Half-typed edit: unbalanced `function`.
        WriteScript6C("syntax.lua", "function on_update(entity, dt\n");
        h.scriptSys.ReloadScript(
            std::filesystem::absolute(scriptPath).make_preferred());
        h.Update(1.0f / 60.0f);

        CHECK(h.scriptSys.GetInstanceState(cube) == ScriptInstanceState::Live);
        CHECK(h.scriptSys.QuarantinedInstanceCount() == 0);
        CHECK(RuntimeName(h, cube) == "running");  // old callback still bound

        h.Stop(doc);
    }

    // ------------------------------------------------------------------------
    // S1: Quarantined -> Live. A runtime error quarantines; a subsequent
    // valid reload must bring the instance back, otherwise hot reload is
    // useless for the exact case you most want it (fixing the crash).
    // ------------------------------------------------------------------------
    TEST_CASE("Phase 6C: a valid reload un-quarantines a dead instance")
    {
        auto scriptPath = WriteScript6C("unquarantine.lua", R"LUA(
function on_create(entity, world) end

function on_update(entity, dt, input, world)
    error("dead")
end
)LUA");

        Harness6C h;
        auto doc = BuildScriptedCube(h, "unquarantine.lua");
        const UUID cube = FirstScriptUuid(doc);
        REQUIRE(h.Play(doc));
        h.Update(1.0f / 60.0f);
        REQUIRE(h.scriptSys.GetInstanceState(cube) ==
                ScriptInstanceState::Quarantined);

        WriteScript6C("unquarantine.lua", R"LUA(
function on_create(entity, world) end

function on_update(entity, dt, input, world)
    entity:set_name("recovered")
end
)LUA");
        h.scriptSys.ReloadScript(
            std::filesystem::absolute(scriptPath).make_preferred());

        CHECK(h.scriptSys.GetInstanceState(cube) == ScriptInstanceState::Live);
        CHECK(h.scriptSys.QuarantinedInstanceCount() == 0);

        h.Update(1.0f / 60.0f);
        CHECK(RuntimeName(h, cube) == "recovered");

        h.Stop(doc);
    }

    // ------------------------------------------------------------------------
    // D9 trap 1: all four callbacks must be RESET before rebinding. A reload
    // that drops on_update has to unbind it — otherwise the stale callable
    // from the previous environment keeps firing against a dead `self`.
    // ------------------------------------------------------------------------
    TEST_CASE("Phase 6C: reload binds an added callback and unbinds a removed one")
    {
        auto scriptPath = WriteScript6C("callbacks.lua", R"LUA(
function on_create(entity, world)
    entity:set_name("created")
end
)LUA");

        Harness6C h;
        auto doc = BuildScriptedCube(h, "callbacks.lua");
        const UUID cube = FirstScriptUuid(doc);
        REQUIRE(h.Play(doc));
        h.Update(1.0f / 60.0f);
        // No on_update declared, so the name is whatever on_create set.
        REQUIRE(RuntimeName(h, cube) == "created");

        // Add on_update.
        WriteScript6C("callbacks.lua", R"LUA(
function on_create(entity, world) end

function on_update(entity, dt, input, world)
    entity:set_name("added")
end
)LUA");
        h.scriptSys.ReloadScript(
            std::filesystem::absolute(scriptPath).make_preferred());
        h.Update(1.0f / 60.0f);
        CHECK(RuntimeName(h, cube) == "added");

        // Remove it again. The name must stop changing, so park a marker
        // first and check it survives.
        REQUIRE(h.sink.SetName(cube, "marker"));
        WriteScript6C("callbacks.lua", R"LUA(
function on_create(entity, world) end
)LUA");
        h.scriptSys.ReloadScript(
            std::filesystem::absolute(scriptPath).make_preferred());
        h.Update(1.0f / 60.0f);
        h.Update(1.0f / 60.0f);

        CHECK(h.scriptSys.QuarantinedInstanceCount() == 0);
        CHECK(RuntimeName(h, cube) == "marker");

        h.Stop(doc);
    }

    // ------------------------------------------------------------------------
    // Declarations must be rt2.field.<type>(default) CONSTRUCTOR CALLS. The
    // plain-table form parses without error and is silently skipped, so a
    // script using it reports parsed=true with zero descriptors: the field
    // never reaches the inspector and never gets a default. The shipped W7
    // fixture had exactly this bug and only worked because its one value was
    // also authored into the .rt2scene.
    // ------------------------------------------------------------------------
    TEST_CASE("Phase 6C: rt2.field constructors declare; plain tables do not")
    {
        auto good = WriteScript6C("decl_good.lua", R"LUA(
rt2.fields = {
    speed = rt2.field.float(2.5),
    name  = rt2.field.string("hi"),
    on    = rt2.field.bool(true)
}
function on_update() end
)LUA");

        ScriptFieldRegistry registry;
        auto r = registry.GetDeclaredFields(good);
        REQUIRE(r.parsed);
        REQUIRE(r.descriptors.size() == 3);

        std::map<std::string, ScriptFieldType> byName;
        for (const auto& d : r.descriptors) byName[d.name] = d.type;
        CHECK(byName["speed"] == ScriptFieldType::Float);
        CHECK(byName["name"]  == ScriptFieldType::String);
        CHECK(byName["on"]    == ScriptFieldType::Bool);

        // The plain-table form: parses, declares nothing. Pinning this so the
        // silence is a documented behaviour rather than a surprise.
        auto bad = WriteScript6C("decl_bad.lua", R"LUA(
rt2.fields = {
    speed = { type = "float", default = 2.5 }
}
function on_update() end
)LUA");
        auto rb = registry.GetDeclaredFields(bad);
        CHECK(rb.parsed);
        CHECK(rb.descriptors.empty());
    }

    // ------------------------------------------------------------------------
    // Reload runs the 6B compatibility rules: an added field takes its
    // declared default, and a field the user already authored keeps the
    // authored value rather than being reset to the default.
    // ------------------------------------------------------------------------
    TEST_CASE("Phase 6C: reload preserves authored fields and defaults new ones")
    {
        auto scriptPath = WriteScript6C("fields.lua", R"LUA(
rt2.fields = {
    speed = rt2.field.float(1.0)
}

function on_create(entity, world) end

function on_update(entity, dt, input, world)
    local p = entity:get_position()
    entity:set_position({ self.speed, p[2], p[3] })
end
)LUA");

        CubeOptions opts;
        opts.fieldValues["speed"] = ScriptFieldEntry{
            ScriptFieldType::Float, ScriptFieldValue{ 5.0 } };

        Harness6C h;
        auto doc = BuildScriptedCube(h, "fields.lua", opts);
        const UUID cube = FirstScriptUuid(doc);
        REQUIRE(h.Play(doc));
        h.Update(1.0f / 60.0f);
        // Authored 5.0 wins over the declared default of 1.0.
        REQUIRE(RuntimePosition(h, cube).x == doctest::Approx(5.0f));

        // Add a second field; the first must survive untouched.
        WriteScript6C("fields.lua", R"LUA(
rt2.fields = {
    speed = rt2.field.float(1.0),
    boost = rt2.field.float(7.0)
}

function on_create(entity, world) end

function on_update(entity, dt, input, world)
    -- Reported as a string, not written through set_position: a nil field
    -- would fail the vec3 validation and silently leave the old position,
    -- which reads as "field defaulted to 0" instead of "field is missing".
    entity:set_name(tostring(self.speed) .. "/" .. tostring(self.boost))
end
)LUA");
        h.scriptSys.ReloadScript(
            std::filesystem::absolute(scriptPath).make_preferred());
        h.Update(1.0f / 60.0f);

        CHECK(h.scriptSys.QuarantinedInstanceCount() == 0);
        CHECK(RuntimeName(h, cube) == "5.0/7.0");  // preserved / defaulted

        h.Stop(doc);
    }

    // ------------------------------------------------------------------------
    // B3: reload cancels the outgoing instance's timers before the swap.
    // A timer captured from the old environment would otherwise keep firing
    // with its callable closed over state that no longer exists.
    // ------------------------------------------------------------------------
    TEST_CASE("Phase 6C: reload cancels timers owned by the reloaded instance")
    {
        auto scriptPath = WriteScript6C("timer_reload.lua", R"LUA(
function on_create(entity, world)
    timer:every(0.01, function()
        local p = entity:get_position()
        entity:set_position({ p[1] + 1.0, p[2], p[3] })
    end)
end

function on_update(entity, dt, input, world) end
)LUA");

        Harness6C h;
        auto doc = BuildScriptedCube(h, "timer_reload.lua");
        const UUID cube = FirstScriptUuid(doc);
        REQUIRE(h.Play(doc));

        h.Update(0.1f);
        h.Update(0.1f);
        REQUIRE(RuntimePosition(h, cube).x == doctest::Approx(2.0f));

        // The new source schedules nothing, so after the swap the counter
        // must freeze.
        WriteScript6C("timer_reload.lua", R"LUA(
function on_create(entity, world) end
function on_update(entity, dt, input, world) end
)LUA");
        h.scriptSys.ReloadScript(
            std::filesystem::absolute(scriptPath).make_preferred());

        for (int i = 0; i < 4; ++i)
            h.Update(0.1f);

        CHECK(h.scriptSys.QuarantinedInstanceCount() == 0);
        CHECK(RuntimePosition(h, cube).x == doctest::Approx(2.0f));

        h.Stop(doc);
    }

    // ------------------------------------------------------------------------
    // B2: reload while Paused queues rather than swapping mid-freeze, and
    // the queue drains on the first Playing frame after Resume. This is the
    // path W8's DecideScriptFileChange fix made reachable from the watcher.
    // ------------------------------------------------------------------------
    TEST_CASE("Phase 6C: a paused reload queues and drains on resume")
    {
        auto scriptPath = WriteScript6C("paused.lua", R"LUA(
function on_create(entity, world) end

function on_update(entity, dt, input, world)
    entity:set_name("v1")
end
)LUA");

        Harness6C h;
        auto doc = BuildScriptedCube(h, "paused.lua");
        const UUID cube = FirstScriptUuid(doc);
        REQUIRE(h.Play(doc));
        h.Update(1.0f / 60.0f);
        REQUIRE(RuntimeName(h, cube) == "v1");

        h.Pause();
        WriteScript6C("paused.lua", R"LUA(
function on_create(entity, world) end

function on_update(entity, dt, input, world)
    entity:set_name("v2")
end
)LUA");
        h.scriptSys.ReloadScript(
            std::filesystem::absolute(scriptPath).make_preferred());

        // Still frozen on the old code: the swap has not happened.
        h.Update(1.0f / 60.0f);
        CHECK(RuntimeName(h, cube) == "v1");

        REQUIRE(h.Resume());
        h.Update(1.0f / 60.0f);
        CHECK(h.scriptSys.QuarantinedInstanceCount() == 0);
        CHECK(RuntimeName(h, cube) == "v2");

        h.Stop(doc);
    }

    // ------------------------------------------------------------------------
    // Reload with no Play session must not touch instances (there are none)
    // and must not crash — it only invalidates the declaration cache.
    // ------------------------------------------------------------------------
    TEST_CASE("Phase 6C: reload while stopped is a cache invalidation only")
    {
        auto scriptPath = WriteScript6C("stopped.lua",
                                        "function on_update() end\n");
        Harness6C h;

        h.scriptSys.ReloadScript(
            std::filesystem::absolute(scriptPath).make_preferred());

        CHECK(h.scriptSys.LiveInstanceCount() == 0);
        CHECK(h.scriptSys.QuarantinedInstanceCount() == 0);

        // Still usable afterwards.
        auto doc = BuildScriptedCube(h, "stopped.lua");
        REQUIRE(h.Play(doc));
        CHECK(h.scriptSys.LiveInstanceCount() == 1);
        h.Stop(doc);
    }

    // ------------------------------------------------------------------------
    // The watcher hands ReloadScript a native-separator absolute path; the
    // instance stored a path built from parent_path() / a forward-slash
    // relative asset path. Without canonicalising both sides the comparison
    // never matches and every reload silently no-ops.
    // ------------------------------------------------------------------------
    TEST_CASE("Phase 6C: ReloadScript matches a native absolute path")
    {
        auto scriptPath = WriteScript6C("reload_path.lua", R"LUA(
function on_create(entity, world) end

function on_update(entity, dt, input, world)
    entity:set_name("v1")
end
)LUA");

        Harness6C h;
        auto doc = BuildScriptedCube(h, "reload_path.lua");
        const UUID cube = FirstScriptUuid(doc);
        REQUIRE(h.Play(doc));

        h.Update(1.0f / 60.0f);
        REQUIRE(RuntimeName(h, cube) == "v1");

        WriteScript6C("reload_path.lua", R"LUA(
function on_create(entity, world) end

function on_update(entity, dt, input, world)
    entity:set_name("v2")
end
)LUA");

        // A path for a DIFFERENT file must not trigger the swap — this is
        // the over-matching failure, the mirror of the one above.
        WriteScript6C("unrelated.lua", "function on_update() end\n");
        h.scriptSys.ReloadScript(
            (TempDir6C() / "unrelated.lua").make_preferred());
        h.Update(1.0f / 60.0f);
        CHECK(RuntimeName(h, cube) == "v1");

        // Native separators, absolute — exactly what efsw reports on Win32.
        h.scriptSys.ReloadScript(
            std::filesystem::absolute(scriptPath).make_preferred());
        h.Update(1.0f / 60.0f);

        CHECK(h.scriptSys.QuarantinedInstanceCount() == 0);
        CHECK(h.scriptSys.LiveInstanceCount() == 1);
        CHECK(RuntimeName(h, cube) == "v2");

        h.Stop(doc);
    }

    // ------------------------------------------------------------------------
    // Input bindings against a mock IInputService. Covers the three shapes
    // that differ: action states (is_down spans Pressed and Held),
    // continuous axes, and the index-based mouse-delta table — the last of
    // which was returning a table keyed "x"/"y" that Lua could not read.
    // ------------------------------------------------------------------------
    TEST_CASE("Phase 6C: input bindings read through to the service")
    {
        WriteScript6C("input.lua", R"LUA(
function on_create(entity, world) end

function on_update(entity, dt, input, world)
    local parts = {}
    parts[#parts + 1] = tostring(input:is_down("fire"))
    parts[#parts + 1] = tostring(input:is_pressed("fire"))
    parts[#parts + 1] = tostring(input:is_down("jump"))
    parts[#parts + 1] = string.format("%.1f", input:get_axis("move"))
    local d = input:get_mouse_delta()
    parts[#parts + 1] = string.format("%.1f,%.1f", d[1], d[2])
    parts[#parts + 1] = string.format("%.1f", input:get_scroll_delta())
    entity:set_name(table.concat(parts, "|"))
end
)LUA");

        Harness6C h;
        auto doc = BuildScriptedCube(h, "input.lua");
        const UUID cube = FirstScriptUuid(doc);

        h.input.actions["fire"] = ActionState::Held;   // down, not pressed
        h.input.actions["jump"] = ActionState::None;
        h.input.axes["move"] = 0.5f;
        h.input.mouseDelta = {3.0f, -4.0f};
        h.input.scrollDelta = 2.0f;

        REQUIRE(h.Play(doc));
        h.Update(1.0f / 60.0f);

        CHECK(h.scriptSys.QuarantinedInstanceCount() == 0);
        CHECK(RuntimeName(h, cube) == "true|false|false|0.5|3.0,-4.0|2.0");

        h.Stop(doc);
    }

    // ------------------------------------------------------------------------
    // Light get/set round-trip on an entity that actually has the component.
    // The set is an overlay: keys absent from the table keep their current
    // value rather than being zeroed.
    // ------------------------------------------------------------------------
    TEST_CASE("Phase 6C: light get/set round-trips and overlays partial tables")
    {
        WriteScript6C("light.lua", R"LUA(
function on_create(entity, world) end

function on_update(entity, dt, input, world)
    local l = entity:get_light()
    if l == nil then
        entity:set_name("no-light")
        return
    end
    -- Overlay intensity only; color must survive untouched.
    entity:set_light({ intensity = 42.0 })
    local after = entity:get_light()
    entity:set_name(string.format("%.1f/%.1f", after.intensity, after.color[1]))
end
)LUA");

        CubeOptions opts;
        opts.withLight = true;

        Harness6C h;
        auto doc = BuildScriptedCube(h, "light.lua", opts);
        const UUID cube = FirstScriptUuid(doc);
        REQUIRE(h.Play(doc));
        h.Update(1.0f / 60.0f);

        CHECK(h.scriptSys.QuarantinedInstanceCount() == 0);
        CHECK(RuntimeName(h, cube) == "42.0/1.0");

        h.Stop(doc);
    }

    // ------------------------------------------------------------------------
    // H2: set_material_index is bounds-checked against [-1, materialCount).
    // -1 is the "no override" sentinel and must be accepted; an index past
    // the end must be rejected rather than written and read back by the GPU
    // scene builder.
    // ------------------------------------------------------------------------
    TEST_CASE("Phase 6C: set_material_index is bounds-checked")
    {
        WriteScript6C("material.lua", R"LUA(
function on_create(entity, world) end

function on_update(entity, dt, input, world)
    local parts = {}
    parts[#parts + 1] = tostring(entity:set_material_index(0))    -- valid
    parts[#parts + 1] = tostring(entity:set_material_index(-1))   -- sentinel
    parts[#parts + 1] = tostring(entity:set_material_index(1))    -- past end
    parts[#parts + 1] = tostring(entity:set_material_index(999))  -- far past
    parts[#parts + 1] = tostring(entity:set_material_index(-2))   -- below
    entity:set_name(table.concat(parts, "|"))
end
)LUA");

        // The fixture has exactly one material, so valid indices are -1 and 0.
        Harness6C h;
        auto doc = BuildScriptedCube(h, "material.lua");
        const UUID cube = FirstScriptUuid(doc);
        REQUIRE(h.Play(doc));
        h.Update(1.0f / 60.0f);

        CHECK(h.scriptSys.QuarantinedInstanceCount() == 0);
        CHECK(RuntimeName(h, cube) == "true|true|false|false|false");

        h.Stop(doc);
    }

    // ------------------------------------------------------------------------
    // Timers must not outlive the Play session. Stop tears down m_Timers
    // along with the instances; a leaked timer would fire into a destroyed
    // environment on the next Play.
    // ------------------------------------------------------------------------
    TEST_CASE("Phase 6C: timers do not survive Stop into the next Play")
    {
        WriteScript6C("timer_stop.lua", R"LUA(
function on_create(entity, world)
    timer:every(0.01, function()
        local p = entity:get_position()
        entity:set_position({ p[1] + 1.0, p[2], p[3] })
    end)
end

function on_update(entity, dt, input, world) end
)LUA");

        Harness6C h;
        auto doc = BuildScriptedCube(h, "timer_stop.lua");
        const UUID cube = FirstScriptUuid(doc);

        REQUIRE(h.Play(doc));
        h.Update(0.1f);
        h.Update(0.1f);
        REQUIRE(RuntimePosition(h, cube).x == doctest::Approx(2.0f));
        h.Stop(doc);

        // Second session: on_create schedules one fresh timer. If Stop had
        // leaked the first, this frame would advance by 2, not 1.
        REQUIRE(h.Play(doc));
        h.Update(0.1f);

        CHECK(h.scriptSys.QuarantinedInstanceCount() == 0);
        CHECK(RuntimePosition(h, cube).x == doctest::Approx(1.0f));

        h.Stop(doc);
    }
}
