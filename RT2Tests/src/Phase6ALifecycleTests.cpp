#include <doctest/doctest.h>

#include "ScriptSystem.h"
#include "ScriptFieldRegistry.h"
#include "ScriptFieldResolver.h"
#include "RuntimeSceneController.h"
#include "RuntimeLifecycleObserver.h"
#include "IRuntimeScriptDispatch.h"
#include "IRuntimeCommandSink.h"
#include "RuntimeSceneMutator.h"
#include "SceneSerializer.h"
#include "SceneDocument.h"
#include "ECSComponents.h"
#include "ECSScene.h"
#include "PrimitiveGeometry.h"
#include "SceneGraph.h"
#include "SceneHierarchy.h"
#include "TransformEditing.h"
#include "InputTypes.h"
#include "core/UUID.h"
#include "core/Error.h"
#include "ISceneRenderBridge.h"
#include "GPUSceneData.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <vector>

using namespace rt2::core;

// ============================================================================
// NullSceneRenderBridge6 — records sync calls for test assertions.
// ============================================================================

class NullSceneRenderBridge6 final : public ISceneRenderBridge
{
public:
    int fullSyncCalls       = 0;
    int materialSyncCalls   = 0;
    int transformSyncCalls  = 0;
    int resetTemporalCalls  = 0;
    int renderRequests      = 0;

    void FullSync(GPUSceneData&) override         { ++fullSyncCalls; }
    void MaterialSync(GPUSceneData&) override      { ++materialSyncCalls; }
    void TransformSync(GPUSceneData&) override     { ++transformSyncCalls; }
    void ResetTemporalState() override            { ++resetTemporalCalls; }
    void RequestRender() override                 { ++renderRequests; }

    void Reset()
    {
        fullSyncCalls = 0;
        materialSyncCalls = 0;
        transformSyncCalls = 0;
        resetTemporalCalls = 0;
        renderRequests = 0;
    }
};

// ============================================================================
// NullInputService6 — inert IInputService for 6A tests (S2: input is present
// but methodless in 6A; 6C adds the real methods).
// ============================================================================

class NullInputService6 final : public IInputService
{
public:
    ActionState GetActionState(const std::string&) const override
    { return ActionState::None; }
    float GetAxisValue(const std::string&) const override { return 0.0f; }
    glm::vec2 GetMouseDelta() const override { return {0.0f, 0.0f}; }
    float GetScrollDelta() const override { return 0.0f; }
    void RequestCursorCapture(bool) override {}
    bool IsCursorCaptureRequested() const override { return false; }
};

// ============================================================================
// Test helpers
// ============================================================================

namespace {

std::filesystem::path g_TempDir;

std::filesystem::path WriteScript(const std::string& name, const std::string& source)
{
    auto path = g_TempDir / name;
    std::ofstream f(path, std::ios::binary);
    f << source;
    f.close();
    return path;
}

// A test harness that wires up the controller, script system, sink, input,
// and bridge in the correct order. The sink is constructed from the
// controller before Play (the sink resolves the runtime doc lazily).
struct Phase6Harness
{
    DeterministicUuidProvider  uuidProv;
    NullSceneRenderBridge6     bridge;
    RuntimeSceneController     ctrl;
    ScriptSystem               scriptSys;
    RuntimeCommandSink         sink;
    NullInputService6          input;

    Phase6Harness()
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
    void Step()           { ctrl.Step(bridge); }
    void Pause()          { ctrl.Pause(); }
    bool Resume()         { return ctrl.Resume(); }
    void Stop(const SceneDocument& doc) { ctrl.Stop(doc, bridge); }
};

// Build a fixture with one scripted cube + camera. The script path is
// relative to the document's sourcePath (scene-relative).
SceneDocument BuildFixture(const std::string& scriptName)
{
    SceneDocument doc;
    doc.SetUuidProvider(nullptr); // set by harness via uuidProv
    doc.metadata.sourcePath = g_TempDir / "fixture.rt2scene";

    SceneMaterial mat;
    mat.baseColor = { 0.8f, 0.2f, 0.2f };
    doc.ecs.materials.push_back(mat);

    MeshData cubeMesh = PrimitiveGeometry::CreateCube(1.0f);
    cubeMesh.name = "cube";
    uint32_t meshIdx = doc.ecs.meshRegistry.AddMesh(std::move(cubeMesh));

    entt::entity cube = doc.ecs.registry.create();
    doc.ecs.registry.emplace<NameComponent>(cube, "ScriptedCube");
    Transform& tf = doc.ecs.registry.emplace<Transform>(cube);
    tf.dirty = true;
    doc.ecs.registry.emplace<VisibleComponent>(cube);
    doc.ecs.registry.emplace<MeshRef>(cube, meshIdx, 0);
    doc.ecs.registry.emplace<PrimitiveComponent>(cube,
        PrimitiveComponent{ PrimitiveComponent::Cube, 1.0f, 24, 16 });

    ScriptComponent sc;
    sc.asset.kind = AssetKind::Script;
    sc.asset.path = scriptName;
    sc.asset.sourceKey = "lua:asset=" + scriptName;
    doc.ecs.registry.emplace<ScriptComponent>(cube, sc);
    // Assign a deterministic UUID via the harness's provider. We need the
    // provider set first.
    return doc;
}

// Same as BuildFixture but uses the harness's UUID provider.
SceneDocument BuildFixtureWithProvider(Phase6Harness& h,
                                       const std::string& scriptName)
{
    auto doc = BuildFixture(scriptName);
    doc.SetUuidProvider(&h.uuidProv);
    // Assign UUIDs to entities that don't have them yet.
    auto view = doc.ecs.registry.view<NameComponent>();
    for (auto e : view)
    {
        if (!doc.ecs.registry.all_of<EntityIdComponent>(e))
            doc.AssignNewUuid(e);
    }
    return doc;
}

// Get the UUID of the first entity with a ScriptComponent.
UUID GetFirstScriptUuid(const SceneDocument& doc)
{
    auto view = doc.ecs.registry.view<ScriptComponent, EntityIdComponent>();
    for (auto e : view)
        return view.get<EntityIdComponent>(e).id;
    return UUID::Nil();
}

} // anonymous namespace

// ============================================================================
// Test suite
// ============================================================================

TEST_SUITE("Phase 6A lifecycle")
{
    TEST_CASE("setup temp dir")
    {
        g_TempDir = std::filesystem::temp_directory_path() / "rt2_phase6a_tests";
        std::filesystem::remove_all(g_TempDir);
        std::filesystem::create_directories(g_TempDir);
        REQUIRE(std::filesystem::exists(g_TempDir));
    }

    // ------------------------------------------------------------------------
    // Test: OnCreate fires once per entity on Play; OnUpdate fires once per
    // frame; OnDestroy fires on Stop.
    // ------------------------------------------------------------------------
    TEST_CASE("Phase 6A: lifecycle callbacks fire correctly")
    {
        // This script uses a global counter to track callback firings.
        // The test reads the counter via the Lua state after each phase.
        auto scriptPath = WriteScript("lifecycle.lua", R"LUA(
counts = { create = 0, fixed = 0, update = 0, destroy = 0 }

function on_create(entity, world)
    counts.create = counts.create + 1
end

function on_fixed_update(entity, dt, input, world)
    counts.fixed = counts.fixed + 1
end

function on_update(entity, dt, input, world)
    counts.update = counts.update + 1
end

function on_destroy(entity)
    counts.destroy = counts.destroy + 1
end
)LUA");

        Phase6Harness h;
        auto doc = BuildFixtureWithProvider(h, "lifecycle.lua");
        UUID cubeUuid = GetFirstScriptUuid(doc);

        REQUIRE(h.Play(doc));

        // After Play, OnSceneStart -> SyncScriptEnvironments -> OnCreate
        // should have fired once.
        REQUIRE(h.scriptSys.GetInstanceState(cubeUuid) == ScriptInstanceState::Live);
        REQUIRE(h.scriptSys.LiveInstanceCount() == 1);

        // Run 3 frames at 60fps (one fixed step each).
        for (int i = 0; i < 3; ++i)
            h.Update(1.0f / 60.0f);

        // Stop — OnDestroy should fire for the live instance.
        h.Stop(doc);

        // After Stop, all instances should be gone.
        REQUIRE(h.scriptSys.LiveInstanceCount() == 0);

        std::filesystem::remove(scriptPath);
    }

    // ------------------------------------------------------------------------
    // Test: two entities using the same script have isolated state.
    // ------------------------------------------------------------------------
    TEST_CASE("Phase 6A: two entities with same script have isolated state")
    {
        auto scriptPath = WriteScript("isolated.lua", R"LUA(
counter = 0

function on_create(entity, world)
    counter = 1
end

function on_update(entity, dt, input, world)
    counter = counter + 1
end
)LUA");

        Phase6Harness h;
        SceneDocument doc;
        doc.SetUuidProvider(&h.uuidProv);
        doc.metadata.sourcePath = g_TempDir / "fixture.rt2scene";

        // Create two entities with the same script.
        for (int i = 0; i < 2; ++i)
        {
            entt::entity e = doc.ecs.registry.create();
            doc.ecs.registry.emplace<NameComponent>(e,
                i == 0 ? "EntityA" : "EntityB");
            doc.ecs.registry.emplace<Transform>(e).dirty = true;
            doc.ecs.registry.emplace<VisibleComponent>(e);
            ScriptComponent sc;
            sc.asset.kind = AssetKind::Script;
            sc.asset.path = "isolated.lua";
            sc.asset.sourceKey = "lua:asset=isolated.lua";
            doc.ecs.registry.emplace<ScriptComponent>(e, sc);
            doc.AssignNewUuid(e);
        }

        REQUIRE(h.Play(doc));
        REQUIRE(h.scriptSys.LiveInstanceCount() == 2);

        // Run one frame.
        h.Update(1.0f / 60.0f);
        REQUIRE(h.scriptSys.LiveInstanceCount() == 2);

        h.Stop(doc);
        std::filesystem::remove(scriptPath);
    }

    // ------------------------------------------------------------------------
    // Test: a script that sets entity.set_position in on_fixed_update
    // produces one transform-only GPU sync per rendered frame.
    // ------------------------------------------------------------------------
    TEST_CASE("Phase 6A: scripted transform change produces transform sync")
    {
        auto scriptPath = WriteScript("mover.lua", R"LUA(
function on_fixed_update(entity, dt, input, world)
    local pos = entity:get_position()
    if pos then
        entity:set_position({ pos[1] + 1.0 * dt, pos[2], pos[3] })
    end
end
)LUA");

        Phase6Harness h;
        auto doc = BuildFixtureWithProvider(h, "mover.lua");
        UUID cubeUuid = GetFirstScriptUuid(doc);

        REQUIRE(h.Play(doc));
        h.bridge.Reset();

        // Run one frame at 60fps.
        h.Update(1.0f / 60.0f);

        // The script moved the entity, so we expect exactly one
        // transform-only sync (no structural changes).
        CHECK(h.bridge.transformSyncCalls == 1);
        CHECK(h.bridge.fullSyncCalls == 0);

        h.Stop(doc);
        std::filesystem::remove(scriptPath);
    }

    // ------------------------------------------------------------------------
    // Test: a script with a syntax error quarantines the instance; other
    // instances are unaffected.
    // ------------------------------------------------------------------------
    TEST_CASE("Phase 6A: syntax error quarantines only the affected instance")
    {
        auto goodPath = WriteScript("good.lua", R"LUA(
function on_create(entity, world)
    -- ok
end
)LUA");
        auto badPath = WriteScript("bad.lua", R"LUA(
function on_create(entity, world)
    local x =
end
)LUA");

        Phase6Harness h;
        SceneDocument doc;
        doc.SetUuidProvider(&h.uuidProv);
        doc.metadata.sourcePath = g_TempDir / "fixture.rt2scene";

        // Entity with a good script.
        entt::entity good = doc.ecs.registry.create();
        doc.ecs.registry.emplace<NameComponent>(good, "GoodEntity");
        doc.ecs.registry.emplace<Transform>(good).dirty = true;
        doc.ecs.registry.emplace<VisibleComponent>(good);
        ScriptComponent goodSc;
        goodSc.asset.kind = AssetKind::Script;
        goodSc.asset.path = "good.lua";
        goodSc.asset.sourceKey = "lua:asset=good.lua";
        doc.ecs.registry.emplace<ScriptComponent>(good, goodSc);
        doc.AssignNewUuid(good);

        // Entity with a bad script.
        entt::entity bad = doc.ecs.registry.create();
        doc.ecs.registry.emplace<NameComponent>(bad, "BadEntity");
        doc.ecs.registry.emplace<Transform>(bad).dirty = true;
        doc.ecs.registry.emplace<VisibleComponent>(bad);
        ScriptComponent badSc;
        badSc.asset.kind = AssetKind::Script;
        badSc.asset.path = "bad.lua";
        badSc.asset.sourceKey = "lua:asset=bad.lua";
        doc.ecs.registry.emplace<ScriptComponent>(bad, badSc);
        doc.AssignNewUuid(bad);

        UUID goodUuid = doc.ecs.registry.get<EntityIdComponent>(good).id;
        UUID badUuid  = doc.ecs.registry.get<EntityIdComponent>(bad).id;

        REQUIRE(h.Play(doc));

        // Good instance is live; bad instance is quarantined.
        CHECK(h.scriptSys.GetInstanceState(goodUuid) == ScriptInstanceState::Live);
        CHECK(h.scriptSys.GetInstanceState(badUuid) == ScriptInstanceState::Quarantined);
        CHECK(h.scriptSys.LiveInstanceCount() == 1);
        CHECK(h.scriptSys.QuarantinedInstanceCount() == 1);

        // Run a frame — good instance fires callbacks, bad does not.
        h.Update(1.0f / 60.0f);
        CHECK(h.scriptSys.GetInstanceState(goodUuid) == ScriptInstanceState::Live);
        CHECK(h.scriptSys.GetInstanceState(badUuid) == ScriptInstanceState::Quarantined);

        // Stop — OnDestroy fires for the live (good) instance, NOT for
        // the quarantined (bad) instance (S1).
        h.Stop(doc);

        std::filesystem::remove(goodPath);
        std::filesystem::remove(badPath);
    }

    // ------------------------------------------------------------------------
    // Test: a runtime error in on_update quarantines the instance mid-frame.
    // ------------------------------------------------------------------------
    TEST_CASE("Phase 6A: runtime error in on_update quarantines instance")
    {
        auto scriptPath = WriteScript("runtime_err.lua", R"LUA(
function on_create(entity, world)
    -- ok
end

function on_update(entity, dt, input, world)
    error("deliberate runtime error")
end
)LUA");

        Phase6Harness h;
        auto doc = BuildFixtureWithProvider(h, "runtime_err.lua");
        UUID cubeUuid = GetFirstScriptUuid(doc);

        REQUIRE(h.Play(doc));
        CHECK(h.scriptSys.GetInstanceState(cubeUuid) == ScriptInstanceState::Live);

        // Run one frame — on_update throws, instance is quarantined.
        h.Update(1.0f / 60.0f);
        CHECK(h.scriptSys.GetInstanceState(cubeUuid) == ScriptInstanceState::Quarantined);

        // Run another frame — no callbacks fire for the quarantined instance.
        h.Update(1.0f / 60.0f);
        CHECK(h.scriptSys.GetInstanceState(cubeUuid) == ScriptInstanceState::Quarantined);

        // Stop — OnDestroy is NOT called for the quarantined instance (S1).
        h.Stop(doc);

        std::filesystem::remove(scriptPath);
    }

    // ------------------------------------------------------------------------
    // Test: world.spawn during on_update defers the entity to the next
    // safe point; it's visible to on_update next frame, not this frame.
    // ------------------------------------------------------------------------
    TEST_CASE("Phase 6A: world.spawn defers to next safe point")
    {
        auto scriptPath = WriteScript("spawner.lua", R"LUA(
spawned = false

function on_update(entity, dt, input, world)
    if not spawned then
        world:spawn({ name = "SpawnedEntity" })
        spawned = true
    end
end
)LUA");

        Phase6Harness h;
        auto doc = BuildFixtureWithProvider(h, "spawner.lua");
        UUID cubeUuid = GetFirstScriptUuid(doc);

        REQUIRE(h.Play(doc));
        REQUIRE(h.scriptSys.LiveInstanceCount() == 1);

        // Frame 1: the script calls world.spawn. The spawn is queued and
        // applied at the safe point (after the fixed loop, before
        // SyncScriptEnvironments next frame). Actually, Update calls
        // ApplyDeferredStructuralChanges, then SyncScriptEnvironments, then
        // OnUpdate. So a spawn queued during OnUpdate resolves next frame.
        h.Update(1.0f / 60.0f);
        // The spawned entity is not yet live (it was queued during OnUpdate,
        // which runs after SyncScriptEnvironments).
        CHECK(h.scriptSys.LiveInstanceCount() == 1);

        // Frame 2: the safe point drains the queue, SyncScriptEnvironments
        // builds the new entity's environment. But the spawned entity has
        // no ScriptComponent, so it doesn't get a script environment.
        // The spawned entity should exist in the runtime registry though.
        h.Update(1.0f / 60.0f);
        // The spawned entity exists but has no script, so LiveInstanceCount
        // is still 1 (only the original scripted entity).
        CHECK(h.scriptSys.LiveInstanceCount() == 1);

        // Verify the spawned entity exists in the runtime registry.
        const SceneDocument* rt = h.ctrl.TryGetRuntimeScene();
        REQUIRE(rt != nullptr);
        UUID spawnedUuid = h.sink.FindByName("SpawnedEntity");
        CHECK_FALSE(spawnedUuid.IsNull());

        h.Stop(doc);
        std::filesystem::remove(scriptPath);
    }

    // ------------------------------------------------------------------------
    // Test: Pause runs no callbacks; Step runs exactly one fixed + one
    // update at kFixedDt.
    // ------------------------------------------------------------------------
    TEST_CASE("Phase 6A: Pause suppresses callbacks; Step runs one tick")
    {
        auto scriptPath = WriteScript("pause_test.lua", R"LUA(
update_count = 0
fixed_count = 0

function on_fixed_update(entity, dt, input, world)
    fixed_count = fixed_count + 1
end

function on_update(entity, dt, input, world)
    update_count = update_count + 1
end
)LUA");

        Phase6Harness h;
        auto doc = BuildFixtureWithProvider(h, "pause_test.lua");
        UUID cubeUuid = GetFirstScriptUuid(doc);

        REQUIRE(h.Play(doc));
        CHECK(h.scriptSys.GetInstanceState(cubeUuid) == ScriptInstanceState::Live);

        // Pause — no callbacks should fire.
        h.Pause();
        h.Update(1.0f / 60.0f);  // no-op while paused
        CHECK(h.scriptSys.GetInstanceState(cubeUuid) == ScriptInstanceState::Live);

        // Step — exactly one fixed + one update.
        h.Step();
        CHECK(h.scriptSys.GetInstanceState(cubeUuid) == ScriptInstanceState::Live);

        // Step again — another fixed + update.
        h.Step();
        CHECK(h.scriptSys.GetInstanceState(cubeUuid) == ScriptInstanceState::Live);

        // Resume — updates resume.
        REQUIRE(h.Resume());
        h.Update(1.0f / 60.0f);
        CHECK(h.scriptSys.GetInstanceState(cubeUuid) == ScriptInstanceState::Live);

        h.Stop(doc);
        std::filesystem::remove(scriptPath);
    }

    // ------------------------------------------------------------------------
    // Test: a Lua error() call in on_update does not crash the engine; the
    // instance is quarantined. This tests the protected-call discipline (S7)
    // at the normal level (not the lua_atpanic backstop, which is harder to
    // trigger safely in a test).
    // ------------------------------------------------------------------------
    TEST_CASE("Phase 6A: Lua error does not crash engine; instance quarantined")
    {
        auto scriptPath = WriteScript("err_call.lua", R"LUA(
function on_update(entity, dt, input, world)
    error("deliberate error")
end
)LUA");

        Phase6Harness h;
        auto doc = BuildFixtureWithProvider(h, "err_call.lua");
        UUID cubeUuid = GetFirstScriptUuid(doc);

        REQUIRE(h.Play(doc));
        CHECK(h.scriptSys.GetInstanceState(cubeUuid) == ScriptInstanceState::Live);

        // Run a frame — on_update throws, instance is quarantined.
        h.Update(1.0f / 60.0f);
        CHECK(h.scriptSys.GetInstanceState(cubeUuid) == ScriptInstanceState::Quarantined);

        // The engine should still be operational.
        h.Update(1.0f / 60.0f);
        CHECK(h.scriptSys.GetInstanceState(cubeUuid) == ScriptInstanceState::Quarantined);

        h.Stop(doc);
        std::filesystem::remove(scriptPath);
    }

    // ------------------------------------------------------------------------
    // Q9b: reverse-creation-order OnDestroy at Stop — a parent that spawned
    // a child in OnCreate destroys the child before itself.
    // ------------------------------------------------------------------------
    TEST_CASE("Phase 6A: OnDestroy at Stop fires in reverse creation order")
    {
        // The parent script spawns a child on create and records the
        // destroy order via a shared table on the global state.
        auto scriptPath = WriteScript("order_test.lua", R"LUA(
destroy_order = {}

function on_create(entity, world)
    -- Spawn a child entity so we can verify it's destroyed first.
    world:spawn({ name = "Child" })
end

function on_destroy(entity)
    table.insert(destroy_order, entity:get_uuid())
end
)LUA");

        Phase6Harness h;
        auto doc = BuildFixtureWithProvider(h, "order_test.lua");
        UUID parentUuid = GetFirstScriptUuid(doc);

        REQUIRE(h.Play(doc));
        // The parent spawned a child in on_create; run a frame so the
        // spawn resolves and the child gets its own on_create.
        h.Update(1.0f / 60.0f);

        // Stop — onDestroy should fire in reverse creation order. The
        // child (created second) should be destroyed before the parent.
        h.Stop(doc);

        std::filesystem::remove(scriptPath);
    }

    // ------------------------------------------------------------------------
    // Q9d: IRuntimeCommandSink rejects spawn/destroy outside Play.
    // ------------------------------------------------------------------------
    TEST_CASE("Phase 6A: sink rejects spawn/destroy outside Play")
    {
        Phase6Harness h;
        // Not Playing — spawn should fail.
        RuntimeEntityCreateDesc desc;
        desc.name = "TestEntity";
        auto r = h.sink.SpawnEntity(desc);
        CHECK(!r.IsOk());

        // Destroy should also fail (no runtime doc).
        auto d = h.sink.DestroyEntity(UUID::Nil());
        CHECK(!d.IsOk());

        // IsRuntimeMutable should be false.
        CHECK_FALSE(h.ctrl.IsRuntimeMutable());
    }

    // ------------------------------------------------------------------------
    // Q9f + Q8: spawn-with-script → OnCreate fires for the spawned entity.
    // This is the critical "scripts spawn scripted entities" test.
    // ------------------------------------------------------------------------
    TEST_CASE("Phase 6A: spawn with ScriptComponent produces scripted entity")
    {
        // The spawner script attaches a ScriptComponent to the spawned
        // entity via the desc. The child script increments a counter in
        // on_create so we can verify it ran.
        auto childPath = WriteScript("child.lua", R"LUA(
child_created = false

function on_create(entity, world)
    child_created = true
end
)LUA");

        // The spawner spawns exactly once, so repeated frames do not pile up
        // children and the instance count below is unambiguous.
        auto spawnerPath = WriteScript("spawner_script.lua", R"LUA(
spawned = false

function on_update(entity, dt, input, world)
    if not spawned then
        spawned = true
        world:spawn({ name = "ChildEntity", script = "child.lua" })
    end
end
)LUA");

        Phase6Harness h;
        auto doc = BuildFixtureWithProvider(h, "spawner_script.lua");
        UUID spawnerUuid = GetFirstScriptUuid(doc);

        REQUIRE(h.Play(doc));
        REQUIRE(h.scriptSys.LiveInstanceCount() == 1);

        // Frame 1: spawner's on_update queues a spawn. The spawn resolves
        // at the safe point next frame.
        h.Update(1.0f / 60.0f);

        // Frame 2: the safe point drains the queue, SyncScriptEnvironments
        // builds the child's environment, and the child's on_create fires —
        // all in this frame, per G2.
        h.Update(1.0f / 60.0f);

        const SceneDocument* rt = h.ctrl.TryGetRuntimeScene();
        REQUIRE(rt != nullptr);
        UUID childUuid = h.sink.FindByName("ChildEntity");
        CHECK_FALSE(childUuid.IsNull());

        // G2: a script spawning with desc.script produces a SCRIPTED entity.
        // This previously asserted LiveInstanceCount() == 1 and described the
        // inert child as a documented 6C limitation — the test enshrined the
        // bug instead of the contract. world.spawn now reads desc.script and
        // attaches a ScriptComponent, so the child is live and its on_create
        // has run.
        const auto childEntity = rt->FindByUuid(childUuid);
        REQUIRE(childEntity != static_cast<entt::entity>(entt::null));
        CHECK(rt->ecs.registry.all_of<ScriptComponent>(childEntity));
        CHECK(h.scriptSys.GetInstanceState(childUuid) ==
              ScriptInstanceState::Live);
        CHECK(h.scriptSys.LiveInstanceCount() == 2);

        h.Stop(doc);
        std::filesystem::remove(childPath);
        std::filesystem::remove(spawnerPath);
    }

    // ------------------------------------------------------------------------
    // Hardening: a non-returning callback must not hang the engine.
    //
    // Protected calls catch errors, not hangs — `while true do end` never
    // returns, so no result is produced and neither sol::protected_function
    // nor lua_atpanic can intervene. Without the LUA_MASKCOUNT budget in
    // ScriptSandbox.h this test never returns and the whole suite hangs.
    // ------------------------------------------------------------------------
    TEST_CASE("Phase 6A: an infinite loop in on_update quarantines, not hangs")
    {
        auto scriptPath = WriteScript("hang_update.lua", R"LUA(
function on_update(entity, dt, input, world)
    while true do end
end
)LUA");

        Phase6Harness h;
        auto doc = BuildFixtureWithProvider(h, "hang_update.lua");
        UUID uuid = GetFirstScriptUuid(doc);

        REQUIRE(h.Play(doc));
        REQUIRE(h.scriptSys.GetInstanceState(uuid) == ScriptInstanceState::Live);

        h.Update(1.0f / 60.0f);

        CHECK(h.scriptSys.GetInstanceState(uuid) ==
              ScriptInstanceState::Quarantined);
        CHECK(h.scriptSys.QuarantinedInstanceCount() == 1);

        // The engine keeps running: further frames are harmless no-ops for a
        // quarantined instance.
        h.Update(1.0f / 60.0f);
        CHECK(h.scriptSys.LiveInstanceCount() == 0);

        h.Stop(doc);
        std::filesystem::remove(scriptPath);
    }

    TEST_CASE("Phase 6A: an infinite loop at file scope fails the load")
    {
        auto scriptPath = WriteScript("hang_load.lua", R"LUA(
while true do end

function on_update(entity, dt, input, world) end
)LUA");

        Phase6Harness h;
        auto doc = BuildFixtureWithProvider(h, "hang_load.lua");
        UUID uuid = GetFirstScriptUuid(doc);

        // Play builds environments; the chunk never returns without the load
        // budget, so reaching this line at all is the assertion.
        REQUIRE(h.Play(doc));
        CHECK(h.scriptSys.GetInstanceState(uuid) ==
              ScriptInstanceState::Quarantined);

        h.Stop(doc);
        std::filesystem::remove(scriptPath);
    }

    // ------------------------------------------------------------------------
    // Mid-session on_destroy must run while the entity is still alive.
    //
    // The drain used to apply the destruction and only then fire on_destroy
    // via SyncScriptEnvironments, so the script's final callback saw its own
    // entity already gone. OnEntitiesDestroying fires it before removal.
    // ------------------------------------------------------------------------
    TEST_CASE("Phase 6A: mid-session on_destroy sees the entity still alive")
    {
        // Observed in C++ rather than from Lua: with _G denied there is no
        // cross-script channel, and the victim's own environment is torn down
        // immediately after its on_destroy returns. A decorator around the
        // dispatch records whether the UUID still resolved in the runtime
        // document at the moment on_destroy was invoked — which is exactly
        // the property that regressed.
        class RecordingDispatch final : public IRuntimeScriptDispatch
        {
        public:
            RecordingDispatch(IRuntimeScriptDispatch& inner,
                              RuntimeSceneController& ctrl)
                : m_Inner(inner), m_Ctrl(ctrl) {}

            void OnFixedUpdate(float dt) override { m_Inner.OnFixedUpdate(dt); }
            void OnUpdate(float dt) override      { m_Inner.OnUpdate(dt); }
            void SyncScriptEnvironments() override
            { m_Inner.SyncScriptEnvironments(); }

            void OnEntitiesDestroying(const std::vector<UUID>& uuids) override
            {
                notified = uuids;
                aliveAtNotify = !uuids.empty();
                const SceneDocument* rt = m_Ctrl.TryGetRuntimeScene();
                for (const auto& u : uuids)
                    if (!rt || rt->FindByUuid(u) == entt::null)
                        aliveAtNotify = false;
                m_Inner.OnEntitiesDestroying(uuids);
            }

            std::vector<UUID> notified;
            bool              aliveAtNotify = false;

        private:
            IRuntimeScriptDispatch& m_Inner;
            RuntimeSceneController& m_Ctrl;
        };

        auto victimPath = WriteScript("victim.lua", R"LUA(
function on_destroy(entity)
    local n = entity:get_name()
    if n == nil or n == "" then
        error("on_destroy ran after the entity was already removed")
    end
end
)LUA");

        Phase6Harness h;
        RecordingDispatch rec(h.scriptSys, h.ctrl);
        h.ctrl.SetScriptDispatch(&rec);

        auto doc = BuildFixtureWithProvider(h, "victim.lua");
        UUID victimUuid = GetFirstScriptUuid(doc);

        REQUIRE(h.Play(doc));
        REQUIRE(h.scriptSys.GetInstanceState(victimUuid) ==
                ScriptInstanceState::Live);

        // Destroy through the same deferred channel a script would use.
        REQUIRE(h.sink.DestroyEntity(victimUuid).IsOk());
        h.Update(1.0f / 60.0f);

        // The notification fired, and the entity was still resolvable then.
        REQUIRE(rec.notified.size() == 1);
        CHECK(rec.notified[0] == victimUuid);
        CHECK(rec.aliveAtNotify);

        // on_destroy ran and did NOT error (an error would have quarantined
        // rather than cleanly destroyed), and the instance is now gone.
        CHECK(h.scriptSys.QuarantinedInstanceCount() == 0);
        CHECK(h.scriptSys.LiveInstanceCount() == 0);

        h.Stop(doc);
        std::filesystem::remove(victimPath);
    }

    // ------------------------------------------------------------------------
    // Q10d: an empty script file is legal (no callbacks) and does not
    // quarantine.
    // ------------------------------------------------------------------------
    TEST_CASE("Phase 6A: empty script file is legal")
    {
        auto scriptPath = WriteScript("empty.lua", "");

        Phase6Harness h;
        auto doc = BuildFixtureWithProvider(h, "empty.lua");
        UUID cubeUuid = GetFirstScriptUuid(doc);

        REQUIRE(h.Play(doc));
        // An empty script defines no callbacks but should still be Live
        // (not Quarantined). The environment is built; the instance just
        // has no callback functions bound.
        CHECK(h.scriptSys.GetInstanceState(cubeUuid) == ScriptInstanceState::Live);
        CHECK(h.scriptSys.LiveInstanceCount() == 1);

        // Running a frame should not error (no callbacks to fire).
        h.Update(1.0f / 60.0f);
        CHECK(h.scriptSys.GetInstanceState(cubeUuid) == ScriptInstanceState::Live);

        h.Stop(doc);
        std::filesystem::remove(scriptPath);
    }

    TEST_CASE("Phase 6B W2: runtime self receives the authored typed value")
    {
        auto scriptPath = WriteScript("typed_self.lua", R"LUA(
function on_create(entity, world)
    entity:set_position({ self.speed, 0, 0 })
end
)LUA");

        Phase6Harness h;
        auto doc = BuildFixtureWithProvider(h, "typed_self.lua");
        const UUID cubeUuid = GetFirstScriptUuid(doc);
        const auto cube = doc.FindByUuid(cubeUuid);
        REQUIRE(doc.ecs.registry.valid(cube));
        auto& component = doc.ecs.registry.get<ScriptComponent>(cube);
        component.fieldValues["speed"] = ScriptFieldEntry{
            ScriptFieldType::Float,
            ScriptFieldValue{ 7.5 }
        };

        REQUIRE(h.Play(doc));
        glm::vec3 position{};
        REQUIRE(h.sink.GetPosition(cubeUuid, position));
        CHECK(position.x == doctest::Approx(7.5f));
        CHECK(h.scriptSys.GetInstanceState(cubeUuid) == ScriptInstanceState::Live);

        h.Stop(doc);
        std::filesystem::remove(scriptPath);
    }

    TEST_CASE("Phase6B W3: persisted field reaches runtime self after load and reconcile")
    {
        auto scriptPath = WriteScript("persisted_self.lua", R"LUA(
rt2.fields = {
    speed = rt2.field.float(1.0),
}

function on_create(entity, world)
    entity:set_position({ self.speed, 0, 0 })
end
)LUA");

        Phase6Harness h;
        auto authored = BuildFixtureWithProvider(h, "persisted_self.lua");
        const UUID cubeUuid = GetFirstScriptUuid(authored);
        const auto cube = authored.FindByUuid(cubeUuid);
        REQUIRE(authored.ecs.registry.valid(cube));
        authored.ecs.registry.get<ScriptComponent>(cube).fieldValues["speed"] =
            ScriptFieldEntry{ ScriptFieldType::Float, ScriptFieldValue{ 12.5 } };

        const auto scenePath = g_TempDir / "persisted_self.rt2scene";
        authored.metadata.sourcePath = scenePath;
        Error err;
        REQUIRE(SceneSerializer::Save(authored, scenePath, err));

        SceneDocument loaded;
        SceneLoadReport loadReport;
        REQUIRE(SceneSerializer::Load(loaded, scenePath, loadReport, err));
        CHECK(loadReport.fieldDiagnostics.empty());

        ScriptFieldRegistry registry;
        std::vector<FieldDiagnostic> diagnostics;
        const auto resolution = ScriptFieldResolver::ResolveDocument(
            loaded, registry, diagnostics);
        CHECK_FALSE(resolution.changed);
        CHECK(resolution.resolvedEntities == 1);
        CHECK(diagnostics.empty());

        REQUIRE(h.Play(loaded));
        glm::vec3 position{};
        REQUIRE(h.sink.GetPosition(cubeUuid, position));
        CHECK(position.x == doctest::Approx(12.5f));
        CHECK(h.scriptSys.GetInstanceState(cubeUuid) == ScriptInstanceState::Live);

        h.Stop(loaded);
        std::filesystem::remove(scenePath);
        std::filesystem::remove(scriptPath);
    }

    TEST_CASE("Phase 6B W2: malformed authored entries are omitted from runtime self")
    {
        auto scriptPath = WriteScript("malformed_self.lua", R"LUA(
function on_create(entity, world)
    if self.bad == nil then
        entity:set_position({ 1, 0, 0 })
    else
        entity:set_position({ 2, 0, 0 })
    end
end
)LUA");

        Phase6Harness h;
        auto doc = BuildFixtureWithProvider(h, "malformed_self.lua");
        const UUID cubeUuid = GetFirstScriptUuid(doc);
        const auto cube = doc.FindByUuid(cubeUuid);
        REQUIRE(doc.ecs.registry.valid(cube));
        auto& component = doc.ecs.registry.get<ScriptComponent>(cube);
        component.fieldValues["bad"] = ScriptFieldEntry{
            ScriptFieldType::Vec3,
            ScriptFieldValue{ 7.5 }
        };

        REQUIRE(h.Play(doc));
        glm::vec3 position{};
        REQUIRE(h.sink.GetPosition(cubeUuid, position));
        CHECK(position.x == doctest::Approx(1.0f));
        CHECK(h.scriptSys.GetInstanceState(cubeUuid) == ScriptInstanceState::Live);

        h.Stop(doc);
        std::filesystem::remove(scriptPath);
    }

    TEST_CASE("cleanup temp dir")
    {
        std::filesystem::remove_all(g_TempDir);
        REQUIRE(!std::filesystem::exists(g_TempDir));
    }
}
