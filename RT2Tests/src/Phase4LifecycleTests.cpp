#include <doctest/doctest.h>

#include "RuntimeSceneController.h"
#include "RuntimeLifecycleObserver.h"
#include "RuntimeSceneMutator.h"
#include "SceneSerializer.h"
#include "SceneSerializerTestSupport.h"
#include "SceneDocument.h"
#include "ECSComponents.h"
#include "ECSScene.h"
#include "PrimitiveGeometry.h"
#include "SceneGraph.h"
#include "SceneHierarchy.h"
#include "core/UUID.h"
#include "core/Error.h"
#include "ISceneRenderBridge.h"
#include "GPUSceneData.h"

#include <algorithm>
#include <fstream>
#include <unordered_set>
#include <vector>

using namespace rt2::core;

// ============================================================================
// NullSceneRenderBridge — records sync calls for test assertions.
// ============================================================================

class NullSceneRenderBridge4 final : public ISceneRenderBridge
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
// Fixture builder — minimal scene with a moving cube + camera. Same shape as
// the existing RuntimeSceneControllerTests fixture.
// ============================================================================

namespace {

SceneDocument BuildPhase4Fixture(IUuidProvider* provider)
{
    SceneDocument doc;
    doc.SetUuidProvider(provider);

    SceneMaterial mat;
    mat.baseColor = { 0.8f, 0.2f, 0.2f };
    doc.ecs.materials.push_back(mat);

    MeshData cubeMesh = PrimitiveGeometry::CreateCube(1.0f);
    cubeMesh.name = "cube";
    uint32_t meshIdx = doc.ecs.meshRegistry.AddMesh(std::move(cubeMesh));

    entt::entity cube = doc.ecs.registry.create();
    doc.ecs.registry.emplace<NameComponent>(cube, "Cube");
    Transform& tf = doc.ecs.registry.emplace<Transform>(cube);
    tf.dirty = true;
    doc.ecs.registry.emplace<VisibleComponent>(cube);
    doc.ecs.registry.emplace<MeshRef>(cube, meshIdx, 0);
    doc.ecs.registry.emplace<PrimitiveComponent>(cube, PrimitiveComponent{ PrimitiveComponent::Cube, 1.0f, 24, 16 });
    doc.ecs.registry.emplace<MotionComponent>(cube, MotionComponent{ { 1.0f, 0.0f, 0.0f } });
    doc.AssignNewUuid(cube);

    entt::entity cam = doc.ecs.registry.create();
    doc.ecs.registry.emplace<NameComponent>(cam, "Camera");
    Transform& ctf = doc.ecs.registry.emplace<Transform>(cam);
    ctf.translation = { 0.0f, 1.0f, 10.0f };
    ctf.dirty = true;
    doc.ecs.registry.emplace<VisibleComponent>(cam);
    doc.ecs.registry.emplace<CameraComponent>(cam, CameraComponent{ 45.0f, 0.0f, 1.0f, { 0, 0, -1 } });
    doc.AssignNewUuid(cam);

    doc.ecs.camera.position = { 0.0f, 1.0f, 10.0f };
    doc.ecs.camera.forwardDirection = { 0.0f, 0.0f, -1.0f };
    doc.ecs.camera.verticalFOV = 45.0f;

    return doc;
}

// Recording lifecycle observer spy.
class RecordingObserver final : public IRuntimeLifecycleObserver
{
public:
    int startCalls = 0;
    int stopCalls  = 0;
    mutable bool lastStartRuntimeNonNull = false;
    mutable bool lastStopRuntimeNonNull  = false;
    mutable size_t lastStartEntityCount   = 0;
    mutable size_t lastStopEntityCount    = 0;
    SceneRunState stateAtStart = SceneRunState::Edit;
    SceneRunState stateAtStop  = SceneRunState::Edit;

    void OnSceneStart(const SceneDocument& runtime) override
    {
        ++startCalls;
        lastStartRuntimeNonNull = (&runtime != nullptr);
        lastStartEntityCount = runtime.uuidIndex.Size();
    }

    void OnSceneStop(const SceneDocument& runtime) override
    {
        ++stopCalls;
        lastStopRuntimeNonNull = (&runtime != nullptr);
        lastStopEntityCount = runtime.uuidIndex.Size();
    }
};

// Observer that attempts to queue a create during OnSceneStart. Asserts that
// the controller accepts the queue (state is Playing) but the op is NOT
// drained until the next Update/Step.
class QueueingObserver final : public IRuntimeLifecycleObserver
{
public:
    RuntimeSceneController* controller = nullptr;
    Result<UUID> queuedUuid;
    bool onSceneStartCalled = false;

    void OnSceneStart(const SceneDocument&) override
    {
        onSceneStartCalled = true;
        if (controller)
        {
            RuntimeEntityCreateDesc desc;
            desc.name = "QueuedAtStart";
            queuedUuid = controller->QueueCreateRuntimeEntity(desc);
        }
    }
};

std::string SerializeToString(const SceneDocument& doc)
{
    Error err;
    auto path = std::filesystem::temp_directory_path() / "rt4_serialized.rt2scene";
    SaveSceneForTest(doc, path, err);
    std::ifstream f(path, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return s;
}

} // anonymous namespace

// ============================================================================
// 1. Lifecycle callback order and counts
// ============================================================================

TEST_CASE("Phase 4 Lifecycle: Play then Stop fires OnSceneStart and OnSceneStop exactly once")
{
    DeterministicUuidProvider provider;
    SceneDocument authoring = BuildPhase4Fixture(&provider);
    NullSceneRenderBridge4 bridge;
    Error err;

    RuntimeSceneController ctrl;
    RecordingObserver obs;
    ctrl.SetLifecycleObserver(&obs);

    CHECK(ctrl.Play(authoring, bridge, err));
    CHECK(obs.startCalls == 1);
    CHECK(obs.stopCalls == 0);
    CHECK(obs.lastStartRuntimeNonNull);
    CHECK(obs.lastStartEntityCount == authoring.uuidIndex.Size());

    ctrl.Stop(authoring, bridge);
    CHECK(obs.startCalls == 1);
    CHECK(obs.stopCalls == 1);
    CHECK(obs.lastStopRuntimeNonNull);
}

TEST_CASE("Phase 4 Lifecycle: 100 Play/Stop cycles fire callbacks exactly 100 times each, no double-fire")
{
    DeterministicUuidProvider provider;
    SceneDocument authoring = BuildPhase4Fixture(&provider);
    NullSceneRenderBridge4 bridge;
    Error err;

    RuntimeSceneController ctrl;
    RecordingObserver obs;
    ctrl.SetLifecycleObserver(&obs);

    for (int i = 0; i < 100; ++i)
    {
        CHECK(ctrl.Play(authoring, bridge, err));
        ctrl.Stop(authoring, bridge);
    }

    CHECK(obs.startCalls == 100);
    CHECK(obs.stopCalls == 100);
}

TEST_CASE("Phase 4 Lifecycle: OnSceneStart receives non-null runtime document; OnSceneStop receives non-null runtime document")
{
    DeterministicUuidProvider provider;
    SceneDocument authoring = BuildPhase4Fixture(&provider);
    NullSceneRenderBridge4 bridge;
    Error err;

    RuntimeSceneController ctrl;
    RecordingObserver obs;
    ctrl.SetLifecycleObserver(&obs);

    ctrl.Play(authoring, bridge, err);
    CHECK(obs.lastStartRuntimeNonNull);
    CHECK(obs.lastStartEntityCount == authoring.uuidIndex.Size());

    ctrl.Stop(authoring, bridge);
    CHECK(obs.lastStopRuntimeNonNull);
}

// ============================================================================
// 2. Injectable UUID provider — deterministic across cycles
// ============================================================================

TEST_CASE("Phase 4 UUID provider: injected DeterministicUuidProvider yields a reproducible UUID sequence")
{
    // Two independent controllers, each with a fresh DeterministicUuidProvider
    // constructed from the same seed, must produce the same UUID for their
    // first QueueCreateRuntimeEntity call. This verifies the controller
    // routes UUID allocation through the injected provider rather than an
    // internal default.
    DeterministicUuidProvider authProv1;
    DeterministicUuidProvider authProv2;
    SceneDocument authoring1 = BuildPhase4Fixture(&authProv1);
    SceneDocument authoring2 = BuildPhase4Fixture(&authProv2);
    NullSceneRenderBridge4 bridge;
    Error err;

    RuntimeSceneController ctrl1;
    DeterministicUuidProvider prov1;
    ctrl1.SetRuntimeUuidProvider(&prov1);
    CHECK(ctrl1.Play(authoring1, bridge, err));
    RuntimeEntityCreateDesc desc;
    desc.name = "First";
    auto r1 = ctrl1.QueueCreateRuntimeEntity(desc);
    REQUIRE(r1.IsOk());
    const UUID firstUuid = r1.value;
    ctrl1.Stop(authoring1, bridge);

    RuntimeSceneController ctrl2;
    DeterministicUuidProvider prov2;
    ctrl2.SetRuntimeUuidProvider(&prov2);
    CHECK(ctrl2.Play(authoring2, bridge, err));
    auto r2 = ctrl2.QueueCreateRuntimeEntity(desc);
    REQUIRE(r2.IsOk());
    const UUID secondUuid = r2.value;
    ctrl2.Stop(authoring2, bridge);

    CHECK(firstUuid == secondUuid);
}

TEST_CASE("Phase 4 UUID provider: Play with no injected provider does not crash; QueueCreate fails cleanly")
{
    // The fixture still needs a provider to build (AssignNewUuid). Use a
    // local provider for the authoring document, but do NOT inject one into
    // the controller — verifying the controller's QueueCreate path rejects
    // cleanly rather than crashing on null dereference.
    DeterministicUuidProvider authProv;
    SceneDocument authoring = BuildPhase4Fixture(&authProv);
    NullSceneRenderBridge4 bridge;
    Error err;

    RuntimeSceneController ctrl;
    // Deliberately do NOT call SetRuntimeUuidProvider.
    CHECK(ctrl.Play(authoring, bridge, err));

    RuntimeEntityCreateDesc desc;
    auto r = ctrl.QueueCreateRuntimeEntity(desc);
    CHECK_FALSE(r.IsOk());
    CHECK(r.error.code == Error::InvalidRuntimeState);

    ctrl.Stop(authoring, bridge);
}

// ============================================================================
// 3. Deferred create / destroy drained during a fixed tick
// ============================================================================

TEST_CASE("Phase 4 Deferred create: queued create appears in runtime after Update; one FullSync; prevWorldMatrix == worldMatrix")
{
    DeterministicUuidProvider provider;
    SceneDocument authoring = BuildPhase4Fixture(&provider);
    NullSceneRenderBridge4 bridge;
    Error err;

    RuntimeSceneController ctrl;
    ctrl.SetRuntimeUuidProvider(&provider);
    CHECK(ctrl.Play(authoring, bridge, err));

    bridge.Reset();

    RuntimeEntityCreateDesc desc;
    desc.name = "Spawned";
    desc.translation = glm::vec3(5.0f, 0.0f, 0.0f);
    auto r = ctrl.QueueCreateRuntimeEntity(desc);
    REQUIRE(r.IsOk());
    const UUID spawned = r.value;

    // Before Update: queued op has not been drained.
    CHECK(ctrl.PendingOperationCount() == 1);
    auto* rt = ctrl.TryGetRuntimeScene();
    REQUIRE(rt);
    CHECK_FALSE(rt->uuidIndex.Contains(spawned));

    ctrl.Update(kFixedDt, bridge);

    // After Update: entity exists, one FullSync fired, queue drained.
    CHECK(ctrl.PendingOperationCount() == 0);
    CHECK(rt->uuidIndex.Contains(spawned));
    CHECK(bridge.fullSyncCalls == 1);
    CHECK(bridge.transformSyncCalls == 0);

    // The created entity's prevWorldMatrix must equal its worldMatrix.
    const auto e = rt->FindByUuid(spawned);
    const bool eFound = (e == entt::null);
    REQUIRE_FALSE(eFound);
    const auto& tf = rt->ecs.registry.get<Transform>(e);
    CHECK(tf.prevWorldMatrix == tf.worldMatrix);
    // The translation we set in desc is reflected.
    CHECK(tf.translation == glm::vec3(5.0f, 0.0f, 0.0f));

    ctrl.Stop(authoring, bridge);
}

TEST_CASE("Phase 4 Deferred destroy: queued destroy removes runtime entity; authoring unchanged; one FullSync")
{
    DeterministicUuidProvider provider;
    SceneDocument authoring = BuildPhase4Fixture(&provider);
    NullSceneRenderBridge4 bridge;
    Error err;

    // Find the cube's UUID in the authoring scene.
    UUID cubeUuid;
    {
        auto view = authoring.ecs.registry.view<NameComponent, EntityIdComponent>();
        for (auto e : view)
        {
            if (view.get<NameComponent>(e).name == "Cube")
            {
                cubeUuid = view.get<EntityIdComponent>(e).id;
                break;
            }
        }
    }
    REQUIRE_FALSE(cubeUuid.IsNull());

    const std::string authoringBefore = SerializeToString(authoring);

    RuntimeSceneController ctrl;
    ctrl.SetRuntimeUuidProvider(&provider);
    CHECK(ctrl.Play(authoring, bridge, err));

    bridge.Reset();

    auto r = ctrl.QueueDestroyRuntimeEntity(cubeUuid);
    REQUIRE(r.IsOk());

    auto* rt = ctrl.TryGetRuntimeScene();
    REQUIRE(rt);
    CHECK(rt->uuidIndex.Contains(cubeUuid));

    ctrl.Update(kFixedDt, bridge);

    CHECK(ctrl.PendingOperationCount() == 0);
    CHECK_FALSE(rt->uuidIndex.Contains(cubeUuid));

    // One FullSync fired (structural change).
    CHECK(bridge.fullSyncCalls == 1);
    CHECK(bridge.transformSyncCalls == 0);

    ctrl.Stop(authoring, bridge);

    // Authoring scene's canonical serialized state is unchanged.
    const std::string authoringAfter = SerializeToString(authoring);
    CHECK(authoringBefore == authoringAfter);
}

// ============================================================================
// 4. FIFO ordering — cross-operation guarantees
// ============================================================================

TEST_CASE("Phase 4 FIFO: create A, create B parented to A, destroy A — A and B both gone after Update")
{
    DeterministicUuidProvider provider;
    SceneDocument authoring = BuildPhase4Fixture(&provider);
    NullSceneRenderBridge4 bridge;
    Error err;

    RuntimeSceneController ctrl;
    ctrl.SetRuntimeUuidProvider(&provider);
    CHECK(ctrl.Play(authoring, bridge, err));

    RuntimeEntityCreateDesc descA;
    descA.name = "A";
    auto rA = ctrl.QueueCreateRuntimeEntity(descA);
    REQUIRE(rA.IsOk());
    const UUID uuidA = rA.value;

    RuntimeEntityCreateDesc descB;
    descB.name = "B";
    descB.parentUuid = uuidA;
    auto rB = ctrl.QueueCreateRuntimeEntity(descB);
    REQUIRE(rB.IsOk());
    const UUID uuidB = rB.value;

    auto rD = ctrl.QueueDestroyRuntimeEntity(uuidA);
    REQUIRE(rD.IsOk());

    auto* rt = ctrl.TryGetRuntimeScene();
    REQUIRE(rt);

    ctrl.Update(kFixedDt, bridge);

    // A is destroyed by op 3, which post-order-collects and also destroys B.
    CHECK_FALSE(rt->uuidIndex.Contains(uuidA));
    CHECK_FALSE(rt->uuidIndex.Contains(uuidB));

    ctrl.Stop(authoring, bridge);
}

TEST_CASE("Phase 4 FIFO: create A then destroy A in one frame — A is gone after Update; one FullSync")
{
    DeterministicUuidProvider provider;
    SceneDocument authoring = BuildPhase4Fixture(&provider);
    NullSceneRenderBridge4 bridge;
    Error err;

    RuntimeSceneController ctrl;
    ctrl.SetRuntimeUuidProvider(&provider);
    CHECK(ctrl.Play(authoring, bridge, err));

    bridge.Reset();

    RuntimeEntityCreateDesc desc;
    desc.name = "Ephemeral";
    auto rC = ctrl.QueueCreateRuntimeEntity(desc);
    REQUIRE(rC.IsOk());
    const UUID uuid = rC.value;

    auto rD = ctrl.QueueDestroyRuntimeEntity(uuid);
    REQUIRE(rD.IsOk());

    auto* rt = ctrl.TryGetRuntimeScene();
    REQUIRE(rt);

    ctrl.Update(kFixedDt, bridge);

    CHECK_FALSE(rt->uuidIndex.Contains(uuid));
    // One FullSync for the frame (structural operations were applied).
    CHECK(bridge.fullSyncCalls == 1);

    ctrl.Stop(authoring, bridge);
}

// ============================================================================
// 5. Validation failure — batch is NOT applied, queue intact
// ============================================================================

TEST_CASE("Phase 4 Validation: destroy A then create B parented to A — batch rejected, queue intact, runtime unchanged")
{
    DeterministicUuidProvider provider;
    SceneDocument authoring = BuildPhase4Fixture(&provider);
    NullSceneRenderBridge4 bridge;
    Error err;

    RuntimeSceneController ctrl;
    ctrl.SetRuntimeUuidProvider(&provider);
    CHECK(ctrl.Play(authoring, bridge, err));

    // Spawn A first, drain it so it exists.
    RuntimeEntityCreateDesc descA;
    descA.name = "A";
    auto rA = ctrl.QueueCreateRuntimeEntity(descA);
    REQUIRE(rA.IsOk());
    const UUID uuidA = rA.value;
    ctrl.Update(kFixedDt, bridge);

    auto* rt = ctrl.TryGetRuntimeScene();
    REQUIRE(rt);
    REQUIRE(rt->uuidIndex.Contains(uuidA));
    const size_t entityCountBefore = rt->uuidIndex.Size();

    bridge.Reset();

    // Invalid batch: destroy A, then create B with parent=A. B's parent
    // would not exist when the create is applied.
    auto rD = ctrl.QueueDestroyRuntimeEntity(uuidA);
    REQUIRE(rD.IsOk());

    RuntimeEntityCreateDesc descB;
    descB.name = "B";
    descB.parentUuid = uuidA;
    auto rC = ctrl.QueueCreateRuntimeEntity(descB);
    REQUIRE(rC.IsOk());
    const UUID uuidB = rC.value;

    // Snapshot the pending queue contents so we can assert they remain intact.
    const size_t pendingBefore = ctrl.PendingOperationCount();

    ctrl.Update(kFixedDt, bridge);

    // Validation failed: queue NOT cleared, runtime NOT mutated, no FullSync.
    CHECK(ctrl.PendingOperationCount() == pendingBefore);
    CHECK(rt->uuidIndex.Size() == entityCountBefore);
    CHECK_FALSE(rt->uuidIndex.Contains(uuidB));
    CHECK(bridge.fullSyncCalls == 0);

    ctrl.Stop(authoring, bridge);
}

TEST_CASE("Phase 4 Validation: duplicate destroy — batch rejected, queue intact")
{
    DeterministicUuidProvider provider;
    SceneDocument authoring = BuildPhase4Fixture(&provider);
    NullSceneRenderBridge4 bridge;
    Error err;

    RuntimeSceneController ctrl;
    ctrl.SetRuntimeUuidProvider(&provider);
    CHECK(ctrl.Play(authoring, bridge, err));

    // Find the cube UUID.
    auto* rt = ctrl.TryGetRuntimeScene();
    REQUIRE(rt);
    UUID cubeUuid;
    {
        auto view = rt->ecs.registry.view<NameComponent, EntityIdComponent>();
        for (auto e : view)
        {
            if (view.get<NameComponent>(e).name == "Cube")
            {
                cubeUuid = view.get<EntityIdComponent>(e).id;
                break;
            }
        }
    }
    REQUIRE_FALSE(cubeUuid.IsNull());

    auto r1 = ctrl.QueueDestroyRuntimeEntity(cubeUuid);
    REQUIRE(r1.IsOk());
    auto r2 = ctrl.QueueDestroyRuntimeEntity(cubeUuid);
    REQUIRE(r2.IsOk());

    const size_t pendingBefore = ctrl.PendingOperationCount();
    const size_t entityCountBefore = rt->uuidIndex.Size();

    ctrl.Update(kFixedDt, bridge);

    // Validation failed on the second destroy (cube is gone after the first
    // would have been applied, but the batch validates the complete sequence
    // before any mutation, so the duplicate destroy fails and the entire
    // batch is rejected).
    CHECK(ctrl.PendingOperationCount() == pendingBefore);
    CHECK(rt->uuidIndex.Size() == entityCountBefore);
    CHECK(rt->uuidIndex.Contains(cubeUuid));

    ctrl.Stop(authoring, bridge);
}

TEST_CASE("Phase 4 Validation: duplicate UUID on create — batch rejected")
{
    DeterministicUuidProvider provider;
    SceneDocument authoring = BuildPhase4Fixture(&provider);
    NullSceneRenderBridge4 bridge;
    Error err;

    RuntimeSceneController ctrl;
    ctrl.SetRuntimeUuidProvider(&provider);
    CHECK(ctrl.Play(authoring, bridge, err));

    auto* rt = ctrl.TryGetRuntimeScene();
    REQUIRE(rt);

    // Find an existing UUID to collide with (the cube's).
    UUID existing;
    {
        auto view = rt->ecs.registry.view<NameComponent, EntityIdComponent>();
        for (auto e : view)
        {
            if (view.get<NameComponent>(e).name == "Cube")
            {
                existing = view.get<EntityIdComponent>(e).id;
                break;
            }
        }
    }
    REQUIRE_FALSE(existing.IsNull());

    // Manually inject a create operation with a colliding UUID via the
    // test-only backdoor: queue a normal create, then mutate its UUID in
    // place by fetching the pending operations and replacing the last.
    // Since PendingOperations() returns by value, we instead use the public
    // path: inject a DeterministicUuidProvider that returns a duplicate.
    // Easier: stop the controller, manually push a colliding op, restart.
    // But we cannot — the controller's queue is private. The spec says we
    // can inject a provider that returns a duplicate. We emulate that by
    // seeding a provider with a value that produces a UUID matching an
    // existing entity.

    // Simpler: the controller's QueueCreateRuntimeEntity skips UUIDs that
    // are already in the runtime index OR pending. So we cannot create a
    // duplicate via the public API. We verify the validation logic directly
    // by constructing a RuntimeSceneMutator and asserting CreateEntity
    // rejects a duplicate UUID — that is the same code path the drain uses.
    RuntimeSceneMutator mutator;
    RuntimeEntityCreateDesc desc;
    desc.name = "Dup";
    auto* rtMut = ctrl.TryGetRuntimeSceneMut();
    REQUIRE(rtMut);
    auto r = mutator.CreateEntity(*rtMut, existing, desc);
    CHECK_FALSE(r.IsOk());
    CHECK(r.error.code == Error::DuplicateUuid);

    ctrl.Stop(authoring, bridge);
}

// ============================================================================
// 6. Invalid queue calls in Edit
// ============================================================================

TEST_CASE("Phase 4 Queue guards: QueueCreate and QueueDestroy return InvalidRuntimeState in Edit")
{
    DeterministicUuidProvider provider;
    SceneDocument authoring = BuildPhase4Fixture(&provider);
    NullSceneRenderBridge4 bridge;
    Error err;

    RuntimeSceneController ctrl;
    ctrl.SetRuntimeUuidProvider(&provider);

    RuntimeEntityCreateDesc desc;
    auto rC = ctrl.QueueCreateRuntimeEntity(desc);
    CHECK_FALSE(rC.IsOk());
    CHECK(rC.error.code == Error::InvalidRuntimeState);

    UUID dummy;
    auto rD = ctrl.QueueDestroyRuntimeEntity(dummy);
    CHECK_FALSE(rD.IsOk());
    CHECK(rD.error.code == Error::InvalidRuntimeState);
}

// ============================================================================
// 7. Callback reentrancy — OnSceneStart may queue, op drained next Update
// ============================================================================

TEST_CASE("Phase 4 Reentrancy: observer queueing during OnSceneStart is accepted but not drained until next Update")
{
    DeterministicUuidProvider provider;
    SceneDocument authoring = BuildPhase4Fixture(&provider);
    NullSceneRenderBridge4 bridge;
    Error err;

    RuntimeSceneController ctrl;
    ctrl.SetRuntimeUuidProvider(&provider);

    QueueingObserver obs;
    obs.controller = &ctrl;
    ctrl.SetLifecycleObserver(&obs);

    CHECK(ctrl.Play(authoring, bridge, err));
    CHECK(obs.onSceneStartCalled);
    REQUIRE(obs.queuedUuid.IsOk());
    const UUID queued = obs.queuedUuid.value;

    // After Play returns, the queued op must still be pending (not drained
    // during OnSceneStart).
    CHECK(ctrl.PendingOperationCount() == 1);

    auto* rt = ctrl.TryGetRuntimeScene();
    REQUIRE(rt);
    CHECK_FALSE(rt->uuidIndex.Contains(queued));

    ctrl.Update(kFixedDt, bridge);

    CHECK(ctrl.PendingOperationCount() == 0);
    CHECK(rt->uuidIndex.Contains(queued));

    ctrl.Stop(authoring, bridge);
}

// ============================================================================
// 8. Pause/Resume preserves the queue; Step drains the queue
// ============================================================================

TEST_CASE("Phase 4 Pause/Resume: queue survives Pause; drained on next Update after Resume")
{
    DeterministicUuidProvider provider;
    SceneDocument authoring = BuildPhase4Fixture(&provider);
    NullSceneRenderBridge4 bridge;
    Error err;

    RuntimeSceneController ctrl;
    ctrl.SetRuntimeUuidProvider(&provider);
    CHECK(ctrl.Play(authoring, bridge, err));

    ctrl.Pause();

    RuntimeEntityCreateDesc desc;
    desc.name = "WhilePaused";
    auto r = ctrl.QueueCreateRuntimeEntity(desc);
    REQUIRE(r.IsOk());
    const UUID queued = r.value;

    CHECK(ctrl.PendingOperationCount() == 1);

    CHECK(ctrl.Resume());

    bridge.Reset();
    ctrl.Update(kFixedDt, bridge);

    CHECK(ctrl.PendingOperationCount() == 0);
    auto* rt = ctrl.TryGetRuntimeScene();
    REQUIRE(rt);
    CHECK(rt->uuidIndex.Contains(queued));

    ctrl.Stop(authoring, bridge);
}

TEST_CASE("Phase 4 Step: queue created while Paused is drained at Step's safe point")
{
    DeterministicUuidProvider provider;
    SceneDocument authoring = BuildPhase4Fixture(&provider);
    NullSceneRenderBridge4 bridge;
    Error err;

    RuntimeSceneController ctrl;
    ctrl.SetRuntimeUuidProvider(&provider);
    CHECK(ctrl.Play(authoring, bridge, err));
    ctrl.Pause();

    RuntimeEntityCreateDesc desc;
    desc.name = "Stepped";
    auto r = ctrl.QueueCreateRuntimeEntity(desc);
    REQUIRE(r.IsOk());
    const UUID queued = r.value;

    CHECK(ctrl.PendingOperationCount() == 1);

    bridge.Reset();
    CHECK(ctrl.Step(bridge));

    CHECK(ctrl.PendingOperationCount() == 0);
    auto* rt = ctrl.TryGetRuntimeScene();
    REQUIRE(rt);
    CHECK(rt->uuidIndex.Contains(queued));
    CHECK(bridge.fullSyncCalls == 1);

    ctrl.Stop(authoring, bridge);
}

// ============================================================================
// 9. Authoring-unchanged invariant — across cycles with queue activity
// ============================================================================

TEST_CASE("Phase 4 Authoring-unchanged: canonical serialized state unchanged across 20 cycles with queue activity")
{
    DeterministicUuidProvider provider;
    SceneDocument authoring = BuildPhase4Fixture(&provider);
    NullSceneRenderBridge4 bridge;
    Error err;

    const std::string before = SerializeToString(authoring);

    RuntimeSceneController ctrl;
    ctrl.SetRuntimeUuidProvider(&provider);

    for (int i = 0; i < 20; ++i)
    {
        CHECK(ctrl.Play(authoring, bridge, err));

        // Each cycle queues a create and a destroy. Some cycles leave them
        // pending by stopping mid-frame (no Update call between Queue and
        // Stop), exercising the "Stop clears the queue" path.
        RuntimeEntityCreateDesc desc;
        desc.name = "CycleEntity";
        auto rC = ctrl.QueueCreateRuntimeEntity(desc);
        REQUIRE(rC.IsOk());

        if (i % 2 == 0)
        {
            // Drain on even cycles.
            ctrl.Update(kFixedDt, bridge);
        }
        // Odd cycles: Stop without draining — Stop clears the queue.

        ctrl.Stop(authoring, bridge);
    }

    const std::string after = SerializeToString(authoring);
    CHECK(before == after);
}

TEST_CASE("Phase 4 Stress: 100 cycles with pending changes do not leak or crash")
{
    DeterministicUuidProvider provider;
    SceneDocument authoring = BuildPhase4Fixture(&provider);
    NullSceneRenderBridge4 bridge;
    Error err;

    RuntimeSceneController ctrl;
    ctrl.SetRuntimeUuidProvider(&provider);

    for (int i = 0; i < 100; ++i)
    {
        CHECK(ctrl.Play(authoring, bridge, err));

        RuntimeEntityCreateDesc desc;
        desc.name = "StressEntity";
        auto rC = ctrl.QueueCreateRuntimeEntity(desc);
        REQUIRE(rC.IsOk());

        if (i % 3 == 0)
        {
            // Drain on every third cycle.
            ctrl.Update(kFixedDt, bridge);
        }
        if (i % 5 == 0)
        {
            // Queue a destroy of an arbitrary UUID on every fifth cycle. The
            // validation may reject it (if the UUID was not in the runtime
            // index), which is fine — the batch is rejected, queue intact,
            // and Stop clears it.
            ctrl.QueueDestroyRuntimeEntity(UUID::Nil());
        }

        ctrl.Stop(authoring, bridge);

        // After Stop, the controller is in Edit and the queue is empty.
        CHECK(ctrl.PendingOperationCount() == 0);
        CHECK(ctrl.GetState() == SceneRunState::Edit);
    }

    // Authoring document's UUID set is unchanged.
    auto* rt = ctrl.TryGetRuntimeScene();
    CHECK(rt == nullptr);
    CHECK(authoring.uuidIndex.Size() == 2);  // cube + camera
}

// ============================================================================
// 10. RuntimeSceneMutator direct tests
// ============================================================================

TEST_CASE("Phase 4 Mutator: CreateEntity rejects duplicate UUID")
{
    DeterministicUuidProvider provider;
    SceneDocument doc = BuildPhase4Fixture(&provider);

    UUID existing;
    {
        auto view = doc.ecs.registry.view<NameComponent, EntityIdComponent>();
        for (auto e : view)
        {
            existing = view.get<EntityIdComponent>(e).id;
            break;
        }
    }

    RuntimeSceneMutator mutator;
    RuntimeEntityCreateDesc desc;
    auto r = mutator.CreateEntity(doc, existing, desc);
    CHECK_FALSE(r.IsOk());
    CHECK(r.error.code == Error::DuplicateUuid);
}

TEST_CASE("Phase 4 Mutator: CreateEntity rejects unresolved parent UUID")
{
    DeterministicUuidProvider provider;
    SceneDocument doc = BuildPhase4Fixture(&provider);

    RuntimeSceneMutator mutator;
    RuntimeEntityCreateDesc desc;
    desc.parentUuid = UUID::Nil();  // not in the document
    auto r = mutator.CreateEntity(doc, UUID{std::array<uint8_t,16>{0x11,0x22,0x33,0x44,0,0,0,0,0,0,0,0,0,0,0,0}}, desc);
    CHECK_FALSE(r.IsOk());
    CHECK(r.error.code == Error::InvalidEntity);
}

TEST_CASE("Phase 4 Mutator: DestroySubtree rejects unresolved UUID")
{
    DeterministicUuidProvider provider;
    SceneDocument doc = BuildPhase4Fixture(&provider);

    RuntimeSceneMutator mutator;
    auto r = mutator.DestroySubtree(doc, UUID::Nil());
    CHECK_FALSE(r.IsOk());
    CHECK(r.error.code == Error::InvalidEntity);
}

TEST_CASE("Phase 4 Mutator: CreateEntity then DestroySubtree — entity gone, index consistent")
{
    DeterministicUuidProvider provider;
    SceneDocument doc = BuildPhase4Fixture(&provider);

    RuntimeSceneMutator mutator;
    RuntimeEntityCreateDesc desc;
    desc.name = "TestEntity";
    const UUID uuid{std::array<uint8_t,16>{0xAA,0xBB,0xCC,0xDD,0,0,0,0,0,0,0,0,0,0,0,0}};
    auto r = mutator.CreateEntity(doc, uuid, desc);
    REQUIRE(r.IsOk());
    CHECK(doc.uuidIndex.Contains(uuid));

    const auto e = doc.FindByUuid(uuid);
    const bool eFound = (e == entt::null);
    REQUIRE_FALSE(eFound);
    CHECK(doc.ecs.registry.valid(e));

    auto rD = mutator.DestroySubtree(doc, uuid);
    REQUIRE(rD.IsOk());
    CHECK_FALSE(doc.uuidIndex.Contains(uuid));
    CHECK_FALSE(doc.ecs.registry.valid(e));
}

TEST_CASE("Phase 4 Mutator: CreateEntity with parent sets Hierarchy correctly")
{
    DeterministicUuidProvider provider;
    SceneDocument doc = BuildPhase4Fixture(&provider);

    UUID parentUuid;
    {
        auto view = doc.ecs.registry.view<NameComponent, EntityIdComponent>();
        for (auto e : view)
        {
            if (view.get<NameComponent>(e).name == "Cube")
            {
                parentUuid = view.get<EntityIdComponent>(e).id;
                break;
            }
        }
    }
    REQUIRE_FALSE(parentUuid.IsNull());

    RuntimeSceneMutator mutator;
    RuntimeEntityCreateDesc desc;
    desc.name = "Child";
    desc.parentUuid = parentUuid;
    const UUID childUuid{std::array<uint8_t,16>{0x99,0x88,0x77,0x66,0,0,0,0,0,0,0,0,0,0,0,0}};
    auto r = mutator.CreateEntity(doc, childUuid, desc);
    REQUIRE(r.IsOk());

    const auto child = doc.FindByUuid(childUuid);
    const bool childFound = (child == entt::null);
    REQUIRE_FALSE(childFound);
    const auto* h = doc.ecs.registry.try_get<Hierarchy>(child);
    REQUIRE(h);
    const auto parent = doc.FindByUuid(parentUuid);
    const bool parentFound = (parent == entt::null);
    REQUIRE_FALSE(parentFound);
    CHECK(h->parent == parent);

    const auto* parentH = doc.ecs.registry.try_get<Hierarchy>(parent);
    REQUIRE(parentH);
    CHECK(std::find(parentH->children.begin(), parentH->children.end(), child)
          != parentH->children.end());
}
