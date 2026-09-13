// ============================================================================
// T3: physics world lifecycle — CPU-only PhysicsWorld ownership and the
// candidate-commit Play sequence.
//
// The controller owns at most one committed PhysicsWorld per Play session
// (never global); Play validates early invariants on the authoring document,
// clones, refreshes world transforms, constructs a complete private candidate
// and commits it before any bridge sync or script callback. Any failure
// destroys the candidate and the clone, leaves Edit with a zero accumulator,
// and emits no bridge call and no script callback (typed named Error).
//
// The outer RT2 1/60 accumulator is the sole substepper: each fixed tick runs
// exactly one PhysicsWorld::Step(kFixedDt) == stepSimulation(kFixedDt, 0).
// Pause/Step/Stop semantics are unchanged; Stop destroys constraints, then
// ghosts/bodies, then shapes, then the world before the runtime clone reset.
//
// Boundaries pinned by this file:
// - GREEN_EmptyPlayHasZeroHandles: empty scenes behave exactly as before.
// - GREEN_StepIsOneTick: paused Step is exactly one kFixedDt tick.
// - GREEN_StopZeroHandles / GREEN_StopRestoresAuthoring: repeated cycles leak
//   no handles and leave authoring bytes identical.
// - GREEN_NonIdentityTransformRefreshed: world transforms are rebuilt before
//   the candidate commits.
// - GREEN_ProviderPresentLetsPlayProceed: the collision-provider seam is
//   interface-only in T3 (refs plus a provider Play clean with zero handles;
//   decoding is T4).
// - RED_MotionPlusBodyRefused / RED_ParentedBodyRefused /
//   RED_DynamicTriMeshRefused / RED_BadLayerMaskRefused (non-single layer,
//   zero/unknown mask) / RED_TriggerLayerMismatchRefused /
//   RED_MissingEntityIdRefused / RED_MissingCollisionProviderRefusesPlay /
//   RED_BadConstraintIdentityRefusesPlay: every early invariant refuses Play
//   loudly with the entity UUID (or component wire, when no UUID exists)
//   named.
// - GREEN_LayerMaskPolicyAccepted: the settled single-layer policy table
//   Plays clean.
// - RED_PhysicsPlayConstructionIsAtomic: a late candidate failure (after a
//   live Bullet world exists) proves construction, destructor rollback, a
//   refreshed construction-time pose observation, and zero observable
//   mutation/callback/sync.
//
// Out of scope (T4+): collision-asset decoding, body creation, transform
// simulation, constraints, events, Lua, editor controls, debug drawing.
// ============================================================================

#include <doctest/doctest.h>

#include "PhysicsWorld.h"
#include "IPhysicsCollisionAssetProvider.h"
#include "RuntimeSceneController.h"
#include "RuntimeLifecycleObserver.h"
#include "SceneManager.h"
#include "SceneSerializer.h"
#include "SceneSerializerTestSupport.h"
#include "ECSComponents.h"
#include "ECSScene.h"
#include "ISceneRenderBridge.h"
#include "GPUSceneData.h"
#include "core/Error.h"
#include "core/UUID.h"

#ifdef IMGUI_VERSION
#error "T3 boundary: physics lifecycle headers must not import ImGui"
#endif

#ifdef VK_HEADER_VERSION
#error "T3 boundary: physics lifecycle headers must not import Vulkan"
#endif

#if __has_include("imgui.h")
#error "T3 boundary: imgui.h must not be reachable from physics lifecycle units"
#endif

#if __has_include("vulkan/vulkan.h")
#error "T3 boundary: vulkan headers must not be reachable from physics lifecycle units"
#endif

#if __has_include("Walnut/Application.h")
#error "T3 boundary: Walnut headers must not be reachable from physics lifecycle units"
#endif

#include <filesystem>
#include <fstream>
#include <string>

using namespace rt2::core;

namespace {

class T3NullBridge final : public ISceneRenderBridge
{
public:
    int fullSyncCalls      = 0;
    int materialSyncCalls  = 0;
    int transformSyncCalls = 0;
    int resetTemporalCalls = 0;
    int renderRequests     = 0;

    void FullSync(GPUSceneData&) override        { ++fullSyncCalls; }
    void MaterialSync(GPUSceneData&) override     { ++materialSyncCalls; }
    void TransformSync(GPUSceneData&) override    { ++transformSyncCalls; }
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

    bool Quiet() const
    {
        return fullSyncCalls == 0 && materialSyncCalls == 0 &&
               transformSyncCalls == 0 && resetTemporalCalls == 0 &&
               renderRequests == 0;
    }
};

class T3RecordingObserver final : public IRuntimeLifecycleObserver
{
public:
    int starts = 0;
    int stops  = 0;

    // Optional production-seam capture: when wired to the controller under
    // test, records what is still alive at OnSceneStop (which fires while
    // the runtime document exists, before world teardown and clone reset).
    const RuntimeSceneController* observed = nullptr;
    bool worldAliveAtStop = false;
    bool runtimeAliveAtStop = false;
    size_t liveWorldsAtStop = 0;

    void OnSceneStart(const SceneDocument&) override { ++starts; }
    void OnSceneStop(const SceneDocument&) override
    {
        ++stops;
        if (observed != nullptr)
        {
            worldAliveAtStop = observed->TryGetPhysicsWorld() != nullptr;
            runtimeAliveAtStop = observed->TryGetRuntimeScene() != nullptr;
            liveWorldsAtStop = PhysicsWorld::LiveWorldCount();
        }
    }
};

// T3 keeps the collision-provider seam interface-only: a tag provider the
// tests inject. T4 extends the interface with the decoder and cache.
struct T3StubProvider final : public IPhysicsCollisionAssetProvider
{
};

struct T3Fixture
{
    DeterministicUuidProvider ids;
    SceneManager manager;

    T3Fixture() { manager.SetUuidProvider(&ids); }

    UUID Create(const char* name)
    {
        return manager.CreateEmpty(name).affectedEntities.front();
    }

    UUID CreateChild(const char* name, const UUID& parent)
    {
        return manager.CreateEmpty(name, parent).affectedEntities.front();
    }

    entt::entity Handle(const UUID& uuid) const
    {
        return manager.FindEntityByUuid(uuid);
    }

    entt::registry& Registry() { return manager.GetECS().registry; }
    const SceneDocument& Authoring() const { return manager.AuthoringDoc(); }
};

PhysicsBodyComponent T3StaticBody()
{
    PhysicsBodyComponent body;
    body.kind = PhysicsBodyKind::Static;
    body.mass = 0.0f;
    body.layer = PhysicsLayer::WorldStatic;
    body.mask = PhysicsLayer::Dynamic | PhysicsLayer::WorldStatic |
                PhysicsLayer::Mechanism;
    return body;
}

PhysicsBodyComponent T3DynamicBody()
{
    PhysicsBodyComponent body;
    body.kind = PhysicsBodyKind::Dynamic;
    body.mass = 1.5f;
    body.layer = PhysicsLayer::Dynamic;
    body.mask = PhysicsLayer::WorldStatic | PhysicsLayer::Mechanism;
    return body;
}

PhysicsShapeComponent T3SphereShape()
{
    PhysicsShapeComponent shape;
    shape.shape = PhysicsShapeKind::Sphere;
    shape.radius = 0.5f;
    return shape;
}

PhysicsShapeComponent T3HullShape()
{
    PhysicsShapeComponent shape;
    shape.shape = PhysicsShapeKind::ConvexHull;
    shape.hull.kind = AssetKind::Model;
    shape.hull.path = "colliders/hull.obj";
    shape.hull.sourceKey = "obj:whole-model";
    return shape;
}

PhysicsHingeComponent T3Hinge(const UUID& other)
{
    PhysicsHingeComponent hinge;
    hinge.otherBody = other;
    return hinge;
}

// A valid dynamic pair joined by a world-agnostic hinge: the standard valid
// physics scene for GREEN Play tests (no collision refs, so no provider).
void T3AttachValidPair(T3Fixture& f, UUID& a, UUID& b)
{
    a = f.Create("BodyA");
    b = f.Create("BodyB");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(a), T3DynamicBody());
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(a), T3SphereShape());
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(b), T3DynamicBody());
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(b), T3SphereShape());
    f.Registry().emplace<PhysicsHingeComponent>(f.Handle(a), T3Hinge(b));
}

std::string T3SaveAuthoringBytes(const SceneDocument& doc,
                                 const std::filesystem::path& path, Error& err)
{
    if (!SaveSceneForTest(doc, path, err))
        return {};
    std::ifstream in(path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
    return bytes;
}

// Shared refusal contract: failed Play leaves Edit, zero accumulator, no
// runtime, no world, no handles, no bridge traffic, no script callbacks, and
// a typed Error naming the entity UUID.
void T3CheckCleanRefusal(RuntimeSceneController& ctrl, const T3NullBridge& bridge,
                         const T3RecordingObserver& obs, const Error& err,
                         const UUID& uuid)
{
    CHECK_FALSE(err.IsOk());
    CHECK(err.path == uuid.ToString());
    CHECK(err.detail.find(uuid.ToString()) != std::string::npos);
    CHECK(ctrl.GetState() == SceneRunState::Edit);
    CHECK(ctrl.TryGetRuntimeScene() == nullptr);
    CHECK(ctrl.TryGetPhysicsWorld() == nullptr);
    CHECK(ctrl.PhysicsTotalHandles() == 0);
    CHECK(ctrl.DebugAccumulator() == doctest::Approx(0.0f));
    CHECK(bridge.Quiet());
    CHECK(obs.starts == 0);
    CHECK(obs.stops == 0);
}

} // namespace

TEST_CASE("T3 GREEN_EmptyPlayHasZeroHandles: empty Play commits an empty world and steps silently")
{
    T3Fixture f;
    T3NullBridge bridge;
    T3RecordingObserver obs;
    Error err;

    RuntimeSceneController ctrl;
    ctrl.SetLifecycleObserver(&obs);
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
    CHECK(err.IsOk());
    CHECK(ctrl.GetState() == SceneRunState::Playing);
    REQUIRE(ctrl.TryGetPhysicsWorld() != nullptr);
    CHECK(ctrl.PhysicsBodyCount() == 0);
    CHECK(ctrl.PhysicsConstraintCount() == 0);
    CHECK(ctrl.PhysicsShapeCount() == 0);
    CHECK(ctrl.PhysicsGhostCount() == 0);
    CHECK(ctrl.PhysicsTotalHandles() == 0);
    CHECK(ctrl.PhysicsStepCount() == 0);
    CHECK(obs.starts == 1);

    // Empty scenes behave exactly as before: one Update is one physics tick
    // plus the standard transform sync and presentation pass.
    bridge.Reset();
    ctrl.Update(kFixedDt, bridge);
    CHECK(ctrl.PhysicsStepCount() == 1);
    CHECK(ctrl.PhysicsTotalHandles() == 0);
    CHECK(bridge.transformSyncCalls == 1);
    CHECK(bridge.renderRequests == 1);

    ctrl.Stop(f.Authoring(), bridge);
    CHECK(ctrl.GetState() == SceneRunState::Edit);
    CHECK(ctrl.TryGetPhysicsWorld() == nullptr);
    CHECK(ctrl.PhysicsTotalHandles() == 0);
    CHECK(obs.stops == 1);
}

TEST_CASE("T3 GREEN_StepIsOneTick: paused Step advances exactly one kFixedDt tick")
{
    T3Fixture f;
    T3NullBridge bridge;
    Error err;

    RuntimeSceneController ctrl;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
    ctrl.Pause();

    bridge.Reset();
    REQUIRE(ctrl.Step(bridge));
    CHECK(ctrl.PhysicsStepCount() == 1);
    CHECK(ctrl.DebugAccumulator() == doctest::Approx(0.0f));
    CHECK(bridge.transformSyncCalls == 1);
    CHECK(bridge.renderRequests == 1);

    // A second Step is a second tick; the accumulator is never advanced by Step.
    REQUIRE(ctrl.Step(bridge));
    CHECK(ctrl.PhysicsStepCount() == 2);
    CHECK(ctrl.DebugAccumulator() == doctest::Approx(0.0f));

    // Paused Update is a no-op for physics as well.
    bridge.Reset();
    ctrl.Update(0.1f, bridge);
    CHECK(ctrl.PhysicsStepCount() == 2);
    CHECK(bridge.Quiet());

    // The five-tick frame cap still bounds physics: a 10s stall steps five.
    REQUIRE(ctrl.Resume());
    ctrl.Update(10.0f, bridge);
    CHECK(ctrl.PhysicsStepCount() == 7);
}

TEST_CASE("T3 GREEN_StopZeroHandles: repeated Play/Stop cycles leave zero handles")
{
    T3Fixture f;
    UUID a, b;
    T3AttachValidPair(f, a, b);
    T3NullBridge bridge;
    T3RecordingObserver obs;
    Error err;

    const size_t baseline = PhysicsWorld::LiveWorldCount();
    RuntimeSceneController ctrl;
    ctrl.SetLifecycleObserver(&obs);
    obs.observed = &ctrl;
    // Destruction-boundary probe: the real ~PhysicsWorld() records whether
    // the runtime clone is still alive at the actual teardown point. Set
    // around each Stop and cleared immediately after, so no probe outlives
    // the observation it was installed for.
    std::vector<int> destroySawRuntimeAlive;
    for (int i = 0; i < 5; ++i)
    {
        REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
        CHECK(err.IsOk());
        REQUIRE(ctrl.TryGetPhysicsWorld() != nullptr);
        // Nonzero live census while committed: a real world exists, not just
        // a non-null pointer.
        CHECK(PhysicsWorld::LiveWorldCount() == baseline + 1);
        CHECK(ctrl.PhysicsTotalHandles() == 0);
        for (int s = 0; s < 3; ++s)
            ctrl.Update(kFixedDt, bridge);
        // StepCount is per committed world (one Play session): three ticks.
        CHECK(ctrl.PhysicsStepCount() == 3);
        PhysicsWorld::SetTestDestroyProbe(
            [&ctrl, &destroySawRuntimeAlive]() {
                destroySawRuntimeAlive.push_back(
                    ctrl.TryGetRuntimeScene() != nullptr ? 1 : 0);
            });
        ctrl.Stop(f.Authoring(), bridge);
        PhysicsWorld::SetTestDestroyProbe(nullptr);
        // Teardown bracket: at OnSceneStop (production seam) the world and
        // the runtime clone are both still alive with a nonzero live census;
        // after Stop the world is destroyed (not pointer-discarded) before
        // the clone reset, restoring the baseline.
        CHECK(obs.worldAliveAtStop);
        CHECK(obs.runtimeAliveAtStop);
        CHECK(obs.liveWorldsAtStop == baseline + 1);
        CHECK(ctrl.GetState() == SceneRunState::Edit);
        CHECK(ctrl.TryGetPhysicsWorld() == nullptr);
        CHECK(ctrl.PhysicsTotalHandles() == 0);
        CHECK(PhysicsWorld::LiveWorldCount() == baseline);
    }
    CHECK(obs.stops == 5);
    // Every Stop destroyed its world while the clone was still alive: five
    // destruction-boundary observations, all alive. Reversing only the two
    // resets in Stop() records zeros and turns this red.
    REQUIRE(destroySawRuntimeAlive.size() == 5);
    for (int alive : destroySawRuntimeAlive)
        CHECK(alive == 1);
}

TEST_CASE("T3 GREEN_StopRestoresAuthoring: Play/Stop cycles leave authoring bytes identical")
{
    T3Fixture f;
    UUID a, b;
    T3AttachValidPair(f, a, b);
    // Non-identity authored transform proves Stop restores posed content too.
    f.Registry().get<Transform>(f.Handle(a)).translation = {2.0f, -1.0f, 0.5f};
    T3NullBridge bridge;
    Error err;

    const auto beforePath =
        std::filesystem::temp_directory_path() / "t3_authoring_before.rt2scene";
    const auto afterPath =
        std::filesystem::temp_directory_path() / "t3_authoring_after.rt2scene";
    const std::string before = T3SaveAuthoringBytes(f.Authoring(), beforePath, err);
    REQUIRE(err.IsOk());

    RuntimeSceneController ctrl;
    for (int i = 0; i < 3; ++i)
    {
        REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
        for (int s = 0; s < 60; ++s)
            ctrl.Update(kFixedDt, bridge);
        ctrl.Stop(f.Authoring(), bridge);
    }

    const std::string after = T3SaveAuthoringBytes(f.Authoring(), afterPath, err);
    REQUIRE(err.IsOk());
    CHECK(after == before);

    std::error_code ec;
    std::filesystem::remove(beforePath, ec);
    std::filesystem::remove(afterPath, ec);
}

TEST_CASE("T3 GREEN_NonIdentityTransformRefreshed: runtime worldMatrix matches non-identity authoring")
{
    T3Fixture f;
    const UUID id = f.Create("Posed");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(id), T3StaticBody());
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(id), T3SphereShape());
    f.Registry().get<Transform>(f.Handle(id)).translation = {2.0f, 3.0f, 4.0f};
    T3NullBridge bridge;
    Error err;

    // The construction pose probe records what the candidate observed in the
    // runtime clone at Create() time — not post-Play state. If construction
    // moved before InitPrevTransforms, the clone still holds identity
    // matrices and this goes red.
    PhysicsWorld::SetTestPoseProbe(true);
    RuntimeSceneController ctrl;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
    auto poses = PhysicsWorld::TestRecordedPoses();
    PhysicsWorld::SetTestPoseProbe(false);

    const SceneDocument* runtime = ctrl.TryGetRuntimeScene();
    REQUIRE(runtime != nullptr);

    const auto e = runtime->FindByUuid(id);
    const bool resolved = (e != entt::null);
    REQUIRE(resolved);
    const auto& tf = runtime->ecs.registry.get<Transform>(e);
    CHECK(tf.worldMatrix[3][0] == doctest::Approx(2.0f));
    CHECK(tf.worldMatrix[3][1] == doctest::Approx(3.0f));
    CHECK(tf.worldMatrix[3][2] == doctest::Approx(4.0f));
    // prevWorldMatrix was snapshotted from the refreshed world matrix.
    CHECK(tf.prevWorldMatrix == tf.worldMatrix);

    REQUIRE(poses.size() == 1);
    CHECK(poses[0].id == id);
    CHECK(poses[0].translation.x == doctest::Approx(2.0f));
    CHECK(poses[0].translation.y == doctest::Approx(3.0f));
    CHECK(poses[0].translation.z == doctest::Approx(4.0f));
    ctrl.Stop(f.Authoring(), bridge);
}

TEST_CASE("T3 GREEN_ProviderPresentLetsPlayProceed: collision refs with a provider Play clean")
{
    T3Fixture f;
    const UUID id = f.Create("Hull");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(id), T3StaticBody());
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(id), T3HullShape());
    T3NullBridge bridge;
    Error err;

    T3StubProvider provider;
    RuntimeSceneController ctrl;
    ctrl.SetCollisionProvider(&provider);
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
    CHECK(err.IsOk());
    // T3 ships the seam only: no decoder runs, so zero handles are committed.
    // T4 decodes these same refs through this same provider pointer.
    CHECK(ctrl.PhysicsTotalHandles() == 0);
    ctrl.Stop(f.Authoring(), bridge);
    CHECK(ctrl.TryGetPhysicsWorld() == nullptr);
}

TEST_CASE("T3 RED_MotionPlusBodyRefused: MotionComponent plus physics body refuses Play")
{
    T3Fixture f;
    const UUID id = f.Create("Mover");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(id), T3StaticBody());
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(id), T3SphereShape());
    f.Registry().emplace<MotionComponent>(f.Handle(id),
                                          MotionComponent{{1.0f, 0.0f, 0.0f}});
    T3NullBridge bridge;
    T3RecordingObserver obs;
    Error err;

    RuntimeSceneController ctrl;
    ctrl.SetLifecycleObserver(&obs);
    CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
    T3CheckCleanRefusal(ctrl, bridge, obs, err, id);
}

TEST_CASE("T3 RED_ParentedBodyRefused: parented physics body refuses Play")
{
    T3Fixture f;
    const UUID parent = f.Create("Parent");
    const UUID child = f.CreateChild("Child", parent);
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(child), T3StaticBody());
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(child), T3SphereShape());
    T3NullBridge bridge;
    T3RecordingObserver obs;
    Error err;

    RuntimeSceneController ctrl;
    ctrl.SetLifecycleObserver(&obs);
    CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
    T3CheckCleanRefusal(ctrl, bridge, obs, err, child);
}

TEST_CASE("T3 RED_DynamicTriMeshRefused: dynamic triangle mesh refuses Play")
{
    T3Fixture f;
    const UUID id = f.Create("Ramp");
    PhysicsBodyComponent body = T3DynamicBody();
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(id), body);
    PhysicsShapeComponent shape;
    shape.shape = PhysicsShapeKind::StaticTriMesh;
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(id), shape);
    T3NullBridge bridge;
    T3RecordingObserver obs;
    Error err;

    RuntimeSceneController ctrl;
    ctrl.SetLifecycleObserver(&obs);
    CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
    T3CheckCleanRefusal(ctrl, bridge, obs, err, id);
}

TEST_CASE("T3 RED_BadLayerMaskRefused: non-single layer or bad mask refuses Play")
{
    // Zero layer.
    {
        T3Fixture f;
        const UUID id = f.Create("NoLayer");
        PhysicsBodyComponent body = T3StaticBody();
        body.layer = 0;
        f.Registry().emplace<PhysicsBodyComponent>(f.Handle(id), body);
        T3NullBridge bridge;
        T3RecordingObserver obs;
        Error err;

        RuntimeSceneController ctrl;
        ctrl.SetLifecycleObserver(&obs);
        CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
        T3CheckCleanRefusal(ctrl, bridge, obs, err, id);
    }
    // Multi-bit layer: a group bitset is refused, never silently narrowed.
    {
        T3Fixture f;
        const UUID id = f.Create("GroupLayer");
        PhysicsBodyComponent body = T3StaticBody();
        body.layer = PhysicsLayer::Dynamic | PhysicsLayer::WorldStatic;
        f.Registry().emplace<PhysicsBodyComponent>(f.Handle(id), body);
        T3NullBridge bridge;
        T3RecordingObserver obs;
        Error err;

        RuntimeSceneController ctrl;
        ctrl.SetLifecycleObserver(&obs);
        CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
        T3CheckCleanRefusal(ctrl, bridge, obs, err, id);
    }
    // Unknown one-hot layer: nonzero and single-bit, so only the known-bit
    // clause can refuse it. Otherwise-valid body and shape isolate that
    // exact branch.
    {
        T3Fixture f;
        const UUID id = f.Create("UnknownLayer");
        PhysicsBodyComponent body = T3StaticBody();
        body.layer = 0x10;
        f.Registry().emplace<PhysicsBodyComponent>(f.Handle(id), body);
        f.Registry().emplace<PhysicsShapeComponent>(f.Handle(id), T3SphereShape());
        T3NullBridge bridge;
        T3RecordingObserver obs;
        Error err;

        RuntimeSceneController ctrl;
        ctrl.SetLifecycleObserver(&obs);
        CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
        T3CheckCleanRefusal(ctrl, bridge, obs, err, id);
    }
    // Unknown mask bit.
    {
        T3Fixture f;
        const UUID id = f.Create("BadMask");
        PhysicsBodyComponent body = T3StaticBody();
        body.mask = 0x10;
        f.Registry().emplace<PhysicsBodyComponent>(f.Handle(id), body);
        T3NullBridge bridge;
        T3RecordingObserver obs;
        Error err;

        RuntimeSceneController ctrl;
        ctrl.SetLifecycleObserver(&obs);
        CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
        T3CheckCleanRefusal(ctrl, bridge, obs, err, id);
    }
    // Zero mask: collides with nothing, refused as a malformed policy.
    {
        T3Fixture f;
        const UUID id = f.Create("EmptyMask");
        PhysicsBodyComponent body = T3StaticBody();
        body.mask = 0;
        f.Registry().emplace<PhysicsBodyComponent>(f.Handle(id), body);
        T3NullBridge bridge;
        T3RecordingObserver obs;
        Error err;

        RuntimeSceneController ctrl;
        ctrl.SetLifecycleObserver(&obs);
        CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
        T3CheckCleanRefusal(ctrl, bridge, obs, err, id);
    }
}

TEST_CASE("T3 RED_TriggerLayerMismatchRefused: trigger shapes and Trigger layer must agree")
{
    // Trigger shape on a non-Trigger layer.
    {
        T3Fixture f;
        const UUID id = f.Create("Ghost");
        PhysicsBodyComponent body = T3StaticBody();
        body.layer = PhysicsLayer::WorldStatic;
        f.Registry().emplace<PhysicsBodyComponent>(f.Handle(id), body);
        PhysicsShapeComponent shape = T3SphereShape();
        shape.isTrigger = true;
        f.Registry().emplace<PhysicsShapeComponent>(f.Handle(id), shape);
        T3NullBridge bridge;
        T3RecordingObserver obs;
        Error err;

        RuntimeSceneController ctrl;
        ctrl.SetLifecycleObserver(&obs);
        CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
        T3CheckCleanRefusal(ctrl, bridge, obs, err, id);
    }
    // Solid shape claiming the Trigger layer.
    {
        T3Fixture f;
        const UUID id = f.Create("SolidTrigger");
        PhysicsBodyComponent body = T3StaticBody();
        body.layer = PhysicsLayer::Trigger;
        body.mask = PhysicsLayer::Dynamic;
        f.Registry().emplace<PhysicsBodyComponent>(f.Handle(id), body);
        f.Registry().emplace<PhysicsShapeComponent>(f.Handle(id), T3SphereShape());
        T3NullBridge bridge;
        T3RecordingObserver obs;
        Error err;

        RuntimeSceneController ctrl;
        ctrl.SetLifecycleObserver(&obs);
        CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
        T3CheckCleanRefusal(ctrl, bridge, obs, err, id);
    }
}

TEST_CASE("T3 GREEN_LayerMaskPolicyAccepted: settled single-layer policy Plays clean")
{
    struct AcceptedCase
    {
        const char* name;
        uint16_t layer;
        uint16_t mask;
        bool isTrigger;
    };
    const AcceptedCase cases[] = {
        {"Dynamic", PhysicsLayer::Dynamic, PhysicsLayer::WorldStatic | PhysicsLayer::Mechanism, false},
        {"WorldStatic", PhysicsLayer::WorldStatic, PhysicsLayer::Dynamic, false},
        {"Mechanism", PhysicsLayer::Mechanism, PhysicsLayer::Dynamic, false},
        {"Trigger", PhysicsLayer::Trigger, PhysicsLayer::Dynamic, true},
    };
    for (const auto& accepted : cases)
    {
        T3Fixture f;
        const UUID id = f.Create(accepted.name);
        PhysicsBodyComponent body = T3StaticBody();
        body.layer = accepted.layer;
        body.mask = accepted.mask;
        f.Registry().emplace<PhysicsBodyComponent>(f.Handle(id), body);
        PhysicsShapeComponent shape = T3SphereShape();
        shape.isTrigger = accepted.isTrigger;
        f.Registry().emplace<PhysicsShapeComponent>(f.Handle(id), shape);
        T3NullBridge bridge;
        Error err;

        RuntimeSceneController ctrl;
        CHECK(ctrl.Play(f.Authoring(), bridge, err));
        CHECK(err.IsOk());
        CHECK(ctrl.PhysicsTotalHandles() == 0);
        ctrl.Stop(f.Authoring(), bridge);
        CHECK(ctrl.TryGetPhysicsWorld() == nullptr);
    }
}

TEST_CASE("T3 RED_MissingEntityIdRefused: physics components without authored IDs refuse Play")
{
    // Body without EntityIdComponent: refused before clone can omit it.
    {
        T3Fixture f;
        const auto e = f.Registry().create();
        f.Registry().emplace<Transform>(e);
        f.Registry().emplace<PhysicsBodyComponent>(e, T3StaticBody());
        T3NullBridge bridge;
        T3RecordingObserver obs;
        Error err;

        RuntimeSceneController ctrl;
        ctrl.SetLifecycleObserver(&obs);
        CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
        CHECK_FALSE(err.IsOk());
        CHECK(err.code == Error::InvalidEntity);
        CHECK(err.path == "PhysicsBodyComponent");
        CHECK(err.detail.find("EntityIdComponent") != std::string::npos);
        CHECK(ctrl.GetState() == SceneRunState::Edit);
        CHECK(ctrl.TryGetRuntimeScene() == nullptr);
        CHECK(ctrl.TryGetPhysicsWorld() == nullptr);
        CHECK(bridge.Quiet());
        CHECK(obs.starts == 0);
        CHECK(obs.stops == 0);
    }
    // Shape without EntityIdComponent (and no body): the shape wire is named.
    {
        T3Fixture f;
        const auto e = f.Registry().create();
        f.Registry().emplace<Transform>(e);
        f.Registry().emplace<PhysicsShapeComponent>(e, T3SphereShape());
        T3NullBridge bridge;
        T3RecordingObserver obs;
        Error err;

        RuntimeSceneController ctrl;
        ctrl.SetLifecycleObserver(&obs);
        CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
        CHECK_FALSE(err.IsOk());
        CHECK(err.code == Error::InvalidEntity);
        CHECK(err.path == "PhysicsShapeComponent");
        CHECK(ctrl.TryGetRuntimeScene() == nullptr);
        CHECK(ctrl.TryGetPhysicsWorld() == nullptr);
        CHECK(bridge.Quiet());
        CHECK(obs.starts == 0);
    }
    // Hinge without EntityIdComponent: the hinge wire is named (pass-0 runs
    // before owner-body validation, so the missing ID is what refuses).
    {
        T3Fixture f;
        const auto e = f.Registry().create();
        f.Registry().emplace<Transform>(e);
        f.Registry().emplace<PhysicsHingeComponent>(e, T3Hinge(UUID::Nil()));
        T3NullBridge bridge;
        T3RecordingObserver obs;
        Error err;

        RuntimeSceneController ctrl;
        ctrl.SetLifecycleObserver(&obs);
        CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
        CHECK_FALSE(err.IsOk());
        CHECK(err.code == Error::InvalidEntity);
        CHECK(err.path == "PhysicsHingeComponent");
        CHECK(ctrl.TryGetRuntimeScene() == nullptr);
        CHECK(ctrl.TryGetPhysicsWorld() == nullptr);
        CHECK(bridge.Quiet());
        CHECK(obs.starts == 0);
    }
    // Slider without EntityIdComponent: the slider wire is named.
    {
        T3Fixture f;
        const auto e = f.Registry().create();
        f.Registry().emplace<Transform>(e);
        PhysicsSliderComponent slider;
        f.Registry().emplace<PhysicsSliderComponent>(e, slider);
        T3NullBridge bridge;
        T3RecordingObserver obs;
        Error err;

        RuntimeSceneController ctrl;
        ctrl.SetLifecycleObserver(&obs);
        CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
        CHECK_FALSE(err.IsOk());
        CHECK(err.code == Error::InvalidEntity);
        CHECK(err.path == "PhysicsSliderComponent");
        CHECK(ctrl.TryGetRuntimeScene() == nullptr);
        CHECK(ctrl.TryGetPhysicsWorld() == nullptr);
        CHECK(bridge.Quiet());
        CHECK(obs.starts == 0);
    }
}

TEST_CASE("T3 RED_BadScaleRefused: non-uniform or non-positive scale refuses Play")
{
    // Non-uniform scale.
    {
        T3Fixture f;
        const UUID id = f.Create("Stretched");
        f.Registry().emplace<PhysicsBodyComponent>(f.Handle(id), T3StaticBody());
        f.Registry().get<Transform>(f.Handle(id)).scale = {2.0f, 1.0f, 1.0f};
        T3NullBridge bridge;
        T3RecordingObserver obs;
        Error err;

        RuntimeSceneController ctrl;
        ctrl.SetLifecycleObserver(&obs);
        CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
        T3CheckCleanRefusal(ctrl, bridge, obs, err, id);
    }
    // Zero scale (singular).
    {
        T3Fixture f;
        const UUID id = f.Create("Flat");
        f.Registry().emplace<PhysicsBodyComponent>(f.Handle(id), T3StaticBody());
        f.Registry().get<Transform>(f.Handle(id)).scale = {0.0f, 1.0f, 1.0f};
        T3NullBridge bridge;
        T3RecordingObserver obs;
        Error err;

        RuntimeSceneController ctrl;
        ctrl.SetLifecycleObserver(&obs);
        CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
        T3CheckCleanRefusal(ctrl, bridge, obs, err, id);
    }
}

TEST_CASE("T3 RED_MissingCollisionProviderRefusesPlay: refs without a provider refuse Play")
{
    // Convex-hull ref names physicsShape.hull.
    {
        T3Fixture f;
        const UUID id = f.Create("Hull");
        f.Registry().emplace<PhysicsBodyComponent>(f.Handle(id), T3StaticBody());
        f.Registry().emplace<PhysicsShapeComponent>(f.Handle(id), T3HullShape());
        T3NullBridge bridge;
        T3RecordingObserver obs;
        Error err;

        RuntimeSceneController ctrl;
        ctrl.SetLifecycleObserver(&obs);
        CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
        T3CheckCleanRefusal(ctrl, bridge, obs, err, id);
        CHECK(err.detail.find("physicsShape.hull") != std::string::npos);
    }
    // Static triangle-mesh ref names physicsShape.triMesh.
    {
        T3Fixture f;
        const UUID id = f.Create("TriMesh");
        f.Registry().emplace<PhysicsBodyComponent>(f.Handle(id), T3StaticBody());
        PhysicsShapeComponent shape;
        shape.shape = PhysicsShapeKind::StaticTriMesh;
        shape.triMesh.kind = AssetKind::Model;
        shape.triMesh.path = "colliders/ramp.obj";
        shape.triMesh.sourceKey = "obj:whole-model";
        f.Registry().emplace<PhysicsShapeComponent>(f.Handle(id), shape);
        T3NullBridge bridge;
        T3RecordingObserver obs;
        Error err;

        RuntimeSceneController ctrl;
        ctrl.SetLifecycleObserver(&obs);
        CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
        T3CheckCleanRefusal(ctrl, bridge, obs, err, id);
        CHECK(err.detail.find("physicsShape.triMesh") != std::string::npos);
    }
}

TEST_CASE("T3 RED_BadConstraintIdentityRefusesPlay: malformed constraint identities refuse Play")
{
    // Dangling otherBody names both UUIDs.
    {
        T3Fixture f;
        const UUID owner = f.Create("Owner");
        const UUID missing = f.ids.CreateV4();
        f.Registry().emplace<PhysicsBodyComponent>(f.Handle(owner), T3DynamicBody());
        f.Registry().emplace<PhysicsHingeComponent>(f.Handle(owner), T3Hinge(missing));
        T3NullBridge bridge;
        T3RecordingObserver obs;
        Error err;

        RuntimeSceneController ctrl;
        ctrl.SetLifecycleObserver(&obs);
        CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
        T3CheckCleanRefusal(ctrl, bridge, obs, err, owner);
        CHECK(err.detail.find(missing.ToString()) != std::string::npos);
    }
    // Static hinge owner is refused.
    {
        T3Fixture f;
        const UUID owner = f.Create("StaticOwner");
        f.Registry().emplace<PhysicsBodyComponent>(f.Handle(owner), T3StaticBody());
        f.Registry().emplace<PhysicsHingeComponent>(f.Handle(owner), T3Hinge(UUID::Nil()));
        T3NullBridge bridge;
        T3RecordingObserver obs;
        Error err;

        RuntimeSceneController ctrl;
        ctrl.SetLifecycleObserver(&obs);
        CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
        T3CheckCleanRefusal(ctrl, bridge, obs, err, owner);
    }
    // Singular slider axis is refused.
    {
        T3Fixture f;
        const UUID owner = f.Create("SliderOwner");
        f.Registry().emplace<PhysicsBodyComponent>(f.Handle(owner), T3DynamicBody());
        PhysicsSliderComponent slider;
        slider.otherBody = UUID::Nil();
        slider.axis = {0.0f, 0.0f, 0.0f};
        f.Registry().emplace<PhysicsSliderComponent>(f.Handle(owner), slider);
        T3NullBridge bridge;
        T3RecordingObserver obs;
        Error err;

        RuntimeSceneController ctrl;
        ctrl.SetLifecycleObserver(&obs);
        CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
        T3CheckCleanRefusal(ctrl, bridge, obs, err, owner);
    }
}

TEST_CASE("T3 RED_PhysicsPlayConstructionIsAtomic: late candidate failure leaves zero observable mutation")
{
    T3Fixture f;
    UUID a, b;
    T3AttachValidPair(f, a, b);
    // Non-identity authored transform: the failed Play must not disturb it,
    // and the construction probe below proves the candidate observed the
    // refreshed (not stale-clone) value before rollback.
    f.Registry().get<Transform>(f.Handle(a)).translation = {2.0f, 3.0f, 4.0f};
    T3NullBridge bridge;
    T3RecordingObserver obs;
    Error err;

    const auto snapPath =
        std::filesystem::temp_directory_path() / "t3_atomic_before.rt2scene";
    const std::string before = T3SaveAuthoringBytes(f.Authoring(), snapPath, err);
    REQUIRE(err.IsOk());

    const size_t baseline = PhysicsWorld::LiveWorldCount();
    RuntimeSceneController ctrl;
    ctrl.SetLifecycleObserver(&obs);
    PhysicsWorld::SetTestPoseProbe(true);
    PhysicsWorld::SetTestInjectCreateFailure(true);
    const bool played = ctrl.Play(f.Authoring(), bridge, err);
    PhysicsWorld::SetTestInjectCreateFailure(false);
    auto poses = PhysicsWorld::TestRecordedPoses();
    PhysicsWorld::SetTestPoseProbe(false);

    CHECK_FALSE(played);
    CHECK_FALSE(err.IsOk());
    CHECK(err.detail.find("candidate") != std::string::npos);
    CHECK(ctrl.GetState() == SceneRunState::Edit);
    CHECK(ctrl.TryGetRuntimeScene() == nullptr);
    CHECK(ctrl.TryGetPhysicsWorld() == nullptr);
    CHECK(ctrl.PhysicsTotalHandles() == 0);
    CHECK(ctrl.DebugAccumulator() == doctest::Approx(0.0f));
    CHECK(bridge.Quiet());
    CHECK(obs.starts == 0);
    CHECK(obs.stops == 0);

    // A live Bullet world was constructed and then destroyed: the census is
    // back at baseline (a pre-construction early-out would also read zero
    // here, but it could not have recorded the poses below).
    CHECK(PhysicsWorld::LiveWorldCount() == baseline);

    // The candidate observed the refreshed runtime clone before rollback:
    // both bodies recorded at construction time with a's non-identity pose.
    // A pre-construction failure records nothing; construction before
    // InitPrevTransforms records stale identity matrices.
    REQUIRE(poses.size() == 2);
    bool sawA = false;
    for (const auto& pose : poses)
    {
        if (pose.id == a)
        {
            sawA = true;
            CHECK(pose.translation.x == doctest::Approx(2.0f));
            CHECK(pose.translation.y == doctest::Approx(3.0f));
            CHECK(pose.translation.z == doctest::Approx(4.0f));
        }
    }
    CHECK(sawA);

    // Authoring is byte-identical after the failed Play.
    Error cmpErr;
    const std::string after = T3SaveAuthoringBytes(f.Authoring(), snapPath, cmpErr);
    REQUIRE(cmpErr.IsOk());
    CHECK(after == before);

    // The failure is transient: the same scene Plays clean once the probe
    // clears, with the non-identity transform refreshed in the runtime.
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
    CHECK(err.IsOk());
    REQUIRE(ctrl.TryGetPhysicsWorld() != nullptr);
    CHECK(PhysicsWorld::LiveWorldCount() == baseline + 1);
    const SceneDocument* runtime = ctrl.TryGetRuntimeScene();
    REQUIRE(runtime != nullptr);
    const auto e = runtime->FindByUuid(a);
    const bool resolved = (e != entt::null);
    REQUIRE(resolved);
    CHECK(runtime->ecs.registry.get<Transform>(e).worldMatrix[3][0] ==
          doctest::Approx(2.0f));
    ctrl.Stop(f.Authoring(), bridge);
    CHECK(PhysicsWorld::LiveWorldCount() == baseline);

    std::error_code ec;
    std::filesystem::remove(snapPath, ec);
}
