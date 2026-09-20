// ============================================================================
// PhysicsEventsTests — Bullet T6 deterministic physics events + safe point.
//
// Permanent discriminating RED/GREEN coverage for ticket
// t6-physics-events-safe-point: manifold scrape into canonical UUID-pair
// Contact events (position/normal/impulse); ghost overlap set-diff into
// TriggerEnter/Stay/Exit; 0-5 tick accumulation into one immutable
// non-consuming per-frame snapshot (explicit order/dedup, no stale zero-tick
// data); destroy-UUID filtering before OnUpdate; Stop/reset overlap cleanup.
// The T5 one-pass frozen FIFO drain, its callback/ECS+Bullet-alive seam, the
// constraints-first teardown, next-safe-point reentrancy, and the
// destroying-UUID refusal are preserved and exercised here through their
// physics interplay — no lifecycle callbacks are added.
//
// CPU-only by design: no Vulkan, ImGui or Walnut (same guards as the T3/T4/T5
// units). Every Play-based case runs through RuntimeSceneController::Play;
// every refusal asserts the atomic candidate-commit contract. No T7 Lua
// syntax, T8 debug draw, gameplay, or audio scope.
// ============================================================================

#include <doctest/doctest.h>
#include "PhysicsEvents.h"
#include "PhysicsWorld.h"
#include "RuntimeSceneController.h"
#include "RuntimeLifecycleObserver.h"
#include "ScriptSystem.h"
#include "SceneGraph.h"
#include "SceneManager.h"
#include "ECSComponents.h"
#include "ECSScene.h"
#include "ISceneRenderBridge.h"
#include "GPUSceneData.h"
#include "core/Error.h"
#include "core/UUID.h"

#ifdef IMGUI_VERSION
#error "T6 boundary: physics event tests must not import ImGui"
#endif

#ifdef VK_HEADER_VERSION
#error "T6 boundary: physics event tests must not import Vulkan"
#endif

#if __has_include("imgui.h")
#error "T6 boundary: imgui.h must not be reachable from physics event units"
#endif

#if __has_include("vulkan/vulkan.h")
#error "T6 boundary: vulkan headers must not be reachable from physics event units"
#endif

#if __has_include("Walnut/Application.h")
#error "T6 boundary: Walnut headers must not be reachable from physics event units"
#endif

#include <string>
#include <utility>
#include <vector>

using namespace rt2::core;

namespace {

class T6NullBridge final : public ISceneRenderBridge
{
public:
    int fullSyncCalls      = 0;
    int transformSyncCalls = 0;
    int resetTemporalCalls = 0;
    int renderRequests     = 0;

    void FullSync(GPUSceneData&) override        { ++fullSyncCalls; }
    void MaterialSync(GPUSceneData&) override    {}
    void TransformSync(GPUSceneData&) override   { ++transformSyncCalls; }
    void ResetTemporalState() override           { ++resetTemporalCalls; }
    void RequestRender() override                { ++renderRequests; }
};

struct T6Fixture
{
    DeterministicUuidProvider ids;
    SceneManager manager;

    T6Fixture() { manager.SetUuidProvider(&ids); }

    UUID Create(const char* name)
    {
        return manager.CreateEmpty(name).affectedEntities.front();
    }

    entt::entity Handle(const UUID& uuid) const
    {
        return manager.FindEntityByUuid(uuid);
    }

    entt::registry& Registry() { return manager.GetECS().registry; }
    const SceneDocument& Authoring() const { return manager.AuthoringDoc(); }
};

// Two-consumer probe: stands in for two Lua scripts' OnUpdate. Each OnUpdate
// reads the controller snapshot twice ("script A", "script B"); a consuming
// queue would serve different contents to the second reader. Also records
// OnEntitiesDestroying calls and supports the T5-style reentrancy arms.
class T6EventProbeDispatch final : public IRuntimeScriptDispatch
{
public:
    RuntimeSceneController* ctrl = nullptr;
    RuntimeCommandSink* sink = nullptr;

    std::vector<PhysicsEvent> consumerA;
    std::vector<PhysicsEvent> consumerB;
    int updateCalls = 0;

    int destroyCallCount = 0;
    std::vector<std::vector<UUID>> destroyCalls;

    // Reentrancy arms (one-shot, consumed by the first OnEntitiesDestroying).
    bool queueDestroyArmed = false;
    UUID queuedDestroyTarget;
    bool destroyingWriteArmed = false;
    UUID writeTarget;
    glm::vec3 writePos{0.0f, 0.0f, 0.0f};
    bool writeResult = true;
    glm::vec3 postWritePos{0.0f, 0.0f, 0.0f};
    bool postWriteOk = false;

    void OnFixedUpdate(float) override {}
    void OnUpdate(float) override
    {
        ++updateCalls;
        if (ctrl != nullptr)
        {
            consumerA = ctrl->PhysicsEvents();
            consumerB = ctrl->PhysicsEvents();
        }
    }
    void SyncScriptEnvironments() override {}
    void OnEntitiesDestroying(const std::vector<UUID>& uuids) override
    {
        ++destroyCallCount;
        destroyCalls.push_back(uuids);
        if (queueDestroyArmed && ctrl != nullptr)
        {
            queueDestroyArmed = false;
            ctrl->QueueDestroyRuntimeEntity(queuedDestroyTarget);
        }
        if (destroyingWriteArmed && sink != nullptr)
        {
            destroyingWriteArmed = false;
            writeResult = sink->SetPosition(writeTarget, writePos);
            postWriteOk = sink->GetPosition(writeTarget, postWritePos);
        }
    }
};

PhysicsBodyComponent T6StaticBody()
{
    PhysicsBodyComponent body;
    body.kind = PhysicsBodyKind::Static;
    body.mass = 0.0f;
    body.layer = PhysicsLayer::WorldStatic;
    body.mask = PhysicsLayer::Dynamic | PhysicsLayer::WorldStatic |
                PhysicsLayer::Mechanism;
    return body;
}

PhysicsBodyComponent T6DynamicBody(float mass = 1.0f)
{
    PhysicsBodyComponent body;
    body.kind = PhysicsBodyKind::Dynamic;
    body.mass = mass;
    body.layer = PhysicsLayer::Dynamic;
    body.mask = PhysicsLayer::WorldStatic | PhysicsLayer::Mechanism;
    return body;
}

PhysicsBodyComponent T6KinematicBody()
{
    PhysicsBodyComponent body;
    body.kind = PhysicsBodyKind::Kinematic;
    body.mass = 0.0f;
    body.layer = PhysicsLayer::Dynamic;
    body.mask = PhysicsLayer::WorldStatic | PhysicsLayer::Mechanism;
    return body;
}

PhysicsShapeComponent T6BoxShape(float hx, float hy, float hz)
{
    PhysicsShapeComponent shape;
    shape.shape = PhysicsShapeKind::Box;
    shape.halfExtents = {hx, hy, hz};
    return shape;
}

PhysicsShapeComponent T6SphereShape(float radius = 0.2f)
{
    PhysicsShapeComponent shape;
    shape.shape = PhysicsShapeKind::Sphere;
    shape.radius = radius;
    return shape;
}

// Static ghost slab: overlaps, gives no response, follows the Static bake.
PhysicsShapeComponent T6GhostSlab(float hx, float hy, float hz)
{
    PhysicsShapeComponent shape;
    shape.shape = PhysicsShapeKind::Box;
    shape.halfExtents = {hx, hy, hz};
    shape.isTrigger = true;
    return shape;
}

PhysicsBodyComponent T6GhostBody()
{
    PhysicsBodyComponent body;
    body.kind = PhysicsBodyKind::Static;
    body.mass = 0.0f;
    body.layer = PhysicsLayer::Trigger;
    body.mask = PhysicsLayer::Dynamic;
    return body;
}

struct T6RestScene
{
    UUID ground;
    UUID box;
};

// Ground slab (top at y=0) + a dynamic box started 0.01 penetrating so the
// very first tick solves a real contact with positive applied impulse.
T6RestScene T6BuildRestScene(T6Fixture& f)
{
    T6RestScene scene;
    scene.ground = f.Create("Ground");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(scene.ground),
                                               T6StaticBody());
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(scene.ground),
                                                T6BoxShape(5.0f, 0.5f, 5.0f));
    f.Registry().get<Transform>(f.Handle(scene.ground)).translation =
        {0.0f, -0.5f, 0.0f};

    scene.box = f.Create("Box");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(scene.box),
                                               T6DynamicBody(1.0f));
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(scene.box),
                                                T6BoxShape(0.25f, 0.25f, 0.25f));
    f.Registry().get<Transform>(f.Handle(scene.box)).translation =
        {0.0f, 0.24f, 0.0f};
    return scene;
}

struct T6GhostScene
{
    UUID ghost;
    UUID ball;
};

// Static ghost slab (y in [0.75, 1.25]) + a dynamic ball above it. The ball
// mask opens the Trigger bit so the broadphase reports the overlap; the
// ghost gives no response, so gravity pulls the ball straight through.
T6GhostScene T6BuildGhostScene(T6Fixture& f)
{
    T6GhostScene scene;
    scene.ghost = f.Create("Ghost");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(scene.ghost),
                                               T6GhostBody());
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(scene.ghost),
                                                T6GhostSlab(2.0f, 0.25f, 2.0f));
    f.Registry().get<Transform>(f.Handle(scene.ghost)).translation =
        {0.0f, 1.0f, 0.0f};

    scene.ball = f.Create("Ball");
    PhysicsBodyComponent ballBody = T6DynamicBody(1.0f);
    ballBody.mask = ballBody.mask | PhysicsLayer::Trigger;
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(scene.ball), ballBody);
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(scene.ball),
                                                T6SphereShape(0.2f));
    f.Registry().get<Transform>(f.Handle(scene.ball)).translation =
        {0.0f, 2.5f, 0.0f};
    return scene;
}

float T6RuntimeY(const SceneDocument* runtime, const UUID& uuid)
{
    const auto e = runtime->FindByUuid(uuid);
    const bool resolvedT6 = (e != entt::null);
    REQUIRE(resolvedT6);
    return runtime->ecs.registry.get<Transform>(e).translation.y;
}

bool T6EventMentions(const PhysicsEvent& e, const UUID& uuid)
{
    return e.bodyA == uuid || e.bodyB == uuid;
}

void T6CheckCanonical(const PhysicsEvent& e)
{
    CHECK(e.bodyA < e.bodyB);
    CHECK_FALSE(e.bodyA.IsNull());
    CHECK_FALSE(e.bodyB.IsNull());
    CHECK(std::isfinite(e.position.x));
    CHECK(std::isfinite(e.position.y));
    CHECK(std::isfinite(e.position.z));
    CHECK(std::isfinite(e.normal.x));
    CHECK(std::isfinite(e.normal.y));
    CHECK(std::isfinite(e.normal.z));
    const float len = glm::length(e.normal);
    CHECK(len > 0.99f);
    CHECK(len < 1.01f);
}

} // namespace

TEST_CASE("T6 GREEN_CanonicalPairOrder: swap canonicalizes the pair and flips the normal")
{
    // Rule 1 unit guard: Bullet-order input and its swap must produce the
    // same canonical pair with negated normals.
    UUID lo = UUID::Parse("00000000-0000-4000-8000-000000000001");
    UUID hi = UUID::Parse("00000000-0000-4000-8000-000000000002");
    REQUIRE(lo < hi);
    const glm::vec3 n{0.0f, 1.0f, 0.0f};
    UUID a1, b1, a2, b2;
    glm::vec3 n1, n2;
    CanonicalizePhysicsPair(lo, hi, n, a1, b1, n1);
    CanonicalizePhysicsPair(hi, lo, -n, a2, b2, n2);
    CHECK(a1 == lo);
    CHECK(b1 == hi);
    CHECK(a2 == lo);
    CHECK(b2 == hi);
    CHECK(n1 == n);
    CHECK(n2 == n);
}

TEST_CASE("T6 GREEN_MultiTickAccumulationAndDedup: one coalesced contact per pair per tick, preserved across ticks")
{
    // A resting face-to-face box pair reports several Bullet manifold points
    // per tick; the snapshot must carry exactly one Contact per pair per
    // tick (dedup), and a three-tick frame must carry all three ticks'
    // events with tick-major order (accumulation).
    T6Fixture f;
    const T6RestScene scene = T6BuildRestScene(f);
    const UUID lo = (scene.ground < scene.box) ? scene.ground : scene.box;
    const UUID hi = (scene.ground < scene.box) ? scene.box : scene.ground;

    T6NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));

    ctrl.Update(kFixedDt, bridge);
    const std::vector<PhysicsEvent> one = ctrl.PhysicsEvents();
    REQUIRE(one.size() == 1);
    CHECK(one.front().kind == PhysicsEventKind::Contact);
    CHECK(one.front().bodyA == lo);
    CHECK(one.front().bodyB == hi);
    CHECK(one.front().tickIndex == 0);
    CHECK(one.front().impulse > 0.0f);
    T6CheckCanonical(one.front());
    ctrl.Stop(f.Authoring(), bridge);

    // Fresh session: three ticks in one frame accumulate three per-tick
    // contacts, one per tickIndex, in tick-major order.
    T6Fixture f2;
    const T6RestScene scene2 = T6BuildRestScene(f2);
    const UUID lo2 = (scene2.ground < scene2.box) ? scene2.ground : scene2.box;
    const UUID hi2 = (scene2.ground < scene2.box) ? scene2.box : scene2.ground;
    T6NullBridge bridge2;
    RuntimeSceneController ctrl2;
    REQUIRE(ctrl2.Play(f2.Authoring(), bridge2, err));
    ctrl2.Update(3.0f * kFixedDt, bridge2);
    const std::vector<PhysicsEvent> three = ctrl2.PhysicsEvents();
    REQUIRE(three.size() == 3);
    for (size_t i = 0; i < three.size(); ++i)
    {
        CHECK(three[i].kind == PhysicsEventKind::Contact);
        CHECK(three[i].bodyA == lo2);
        CHECK(three[i].bodyB == hi2);
        CHECK(three[i].tickIndex == (uint32_t)i);
        CHECK(three[i].impulse > 0.0f);
        T6CheckCanonical(three[i]);
    }
    CHECK(ctrl2.TryGetPhysicsWorldMut()->StepCount() == 3);
    ctrl2.Stop(f2.Authoring(), bridge2);
}

TEST_CASE("T6 GREEN_GhostPassThrough: ball falls through the trigger with Enter/Stay/Exit and no contact response")
{
    // The ghost gives no response (pass-through) while the overlap set-diff
    // reports the deterministic Enter -> Stay* -> Exit sequence. A solid
    // slab would stop the ball at y ~= 1.45; the ghost lets it fall past.
    T6Fixture f;
    const T6GhostScene scene = T6BuildGhostScene(f);

    T6NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));

    std::vector<std::vector<PhysicsEvent>> frames;
    for (int i = 0; i < 120; ++i)
    {
        ctrl.Update(kFixedDt, bridge);
        frames.push_back(ctrl.PhysicsEvents());
    }

    // Pass-through: the ball fell well below the ghost slab ([0.75, 1.25]).
    const SceneDocument* runtime = ctrl.TryGetRuntimeScene();
    REQUIRE(runtime != nullptr);
    CHECK(T6RuntimeY(runtime, scene.ball) < 0.5f);

    // Overlap sequence: Enter before Exit, at least one Stay between runs,
    // canonical pairs throughout, and never a Contact involving the ghost.
    const UUID lo = (scene.ghost < scene.ball) ? scene.ghost : scene.ball;
    const UUID hi = (scene.ghost < scene.ball) ? scene.ball : scene.ghost;
    int enterFrame = -1;
    int exitFrame = -1;
    int stayCount = 0;
    for (size_t i = 0; i < frames.size(); ++i)
    {
        for (const auto& e : frames[i])
        {
            T6CheckCanonical(e);
            if (e.kind == PhysicsEventKind::Contact)
                CHECK_FALSE(T6EventMentions(e, scene.ghost));
            if (e.bodyA == lo && e.bodyB == hi)
            {
                if (e.kind == PhysicsEventKind::TriggerEnter && enterFrame < 0)
                    enterFrame = (int)i;
                if (e.kind == PhysicsEventKind::TriggerStay)
                    ++stayCount;
                if (e.kind == PhysicsEventKind::TriggerExit && exitFrame < 0)
                    exitFrame = (int)i;
            }
        }
    }
    REQUIRE(enterFrame >= 0);
    REQUIRE(exitFrame >= 0);
    CHECK(exitFrame > enterFrame);
    CHECK(stayCount > 0);
    ctrl.Stop(f.Authoring(), bridge);
    CHECK(ctrl.PhysicsEvents().empty());
}

TEST_CASE("T6 GREEN_TwoScriptsSameSnapshot: two OnUpdate consumers see identical non-consuming data")
{
    // The snapshot is immutable and non-consuming: the second reader in the
    // same OnUpdate observes exactly what the first reader saw, regardless
    // of UUID-sorted callback order.
    T6Fixture f;
    const T6RestScene scene = T6BuildRestScene(f);
    (void)scene;

    T6NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    T6EventProbeDispatch probe;
    probe.ctrl = &ctrl;
    ctrl.SetScriptDispatch(&probe);
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));

    ctrl.Update(kFixedDt, bridge);
    REQUIRE(probe.updateCalls == 1);
    REQUIRE_FALSE(probe.consumerA.empty());
    CHECK(probe.consumerA.size() == probe.consumerB.size());
    REQUIRE(probe.consumerA.size() == probe.consumerB.size());
    for (size_t i = 0; i < probe.consumerA.size(); ++i)
        CHECK(probe.consumerA[i] == probe.consumerB[i]);
    ctrl.Stop(f.Authoring(), bridge);
}

TEST_CASE("T6 GREEN_DestroyFilteredEvents: events for a drain-destroyed UUID never reach OnUpdate")
{
    // Two frames of contact flow, then the box is destroyed at the safe
    // point: the published snapshot must carry no event mentioning the box,
    // while the surviving ground simply has no partner left to contact.
    T6Fixture f;
    const T6RestScene scene = T6BuildRestScene(f);

    T6NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    T6EventProbeDispatch probe;
    probe.ctrl = &ctrl;
    ctrl.SetScriptDispatch(&probe);
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));

    ctrl.Update(kFixedDt, bridge);
    REQUIRE_FALSE(ctrl.PhysicsEvents().empty());
    ctrl.Update(kFixedDt, bridge);
    REQUIRE_FALSE(ctrl.PhysicsEvents().empty());

    REQUIRE(ctrl.QueueDestroyRuntimeEntity(scene.box).IsOk());
    ctrl.Update(kFixedDt, bridge);

    for (const auto& e : ctrl.PhysicsEvents())
        CHECK_FALSE(T6EventMentions(e, scene.box));
    const SceneDocument* runtime = ctrl.TryGetRuntimeScene();
    REQUIRE(runtime != nullptr);
    CHECK_FALSE(runtime->uuidIndex.Contains(scene.box));
    CHECK(runtime->uuidIndex.Contains(scene.ground));
    // The physics teardown rode the same destroy position: the body is gone.
    CHECK(ctrl.PhysicsBodyCount() == 1);
    ctrl.Stop(f.Authoring(), bridge);
}

TEST_CASE("T6 GREEN_DestroyGhostPurgesOverlapNoPhantomExit: destroying an overlapped ghost emits no Exit")
{
    // Overlap history is purged at the destroy position (RemoveSubtreePhysics):
    // killing the ghost mid-overlap must not fabricate a TriggerExit for a
    // UUID that no longer resolves.
    T6Fixture f;
    const T6GhostScene scene = T6BuildGhostScene(f);

    T6NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));

    // Run until the overlap is live (Enter observed).
    bool entered = false;
    for (int i = 0; i < 60 && !entered; ++i)
    {
        ctrl.Update(kFixedDt, bridge);
        for (const auto& e : ctrl.PhysicsEvents())
        {
            if (e.kind == PhysicsEventKind::TriggerEnter &&
                T6EventMentions(e, scene.ghost))
                entered = true;
        }
    }
    REQUIRE(entered);
    REQUIRE(ctrl.TryGetPhysicsWorldMut()->PrevOverlapCount() > 0);

    REQUIRE(ctrl.QueueDestroyRuntimeEntity(scene.ghost).IsOk());
    ctrl.Update(kFixedDt, bridge);

    for (const auto& e : ctrl.PhysicsEvents())
    {
        CHECK_FALSE(T6EventMentions(e, scene.ghost));
        CHECK(e.kind != PhysicsEventKind::TriggerExit);
    }
    CHECK(ctrl.TryGetPhysicsWorldMut()->PrevOverlapCount() == 0);
    ctrl.Stop(f.Authoring(), bridge);
}

TEST_CASE("T6 GREEN_CreateThenDestroySameUuidClean: create-then-destroy drains once with selective event filtering")
{
    // T6 dimension of the T5 frozen-batch proof: the drain runs over a live
    // physics world, the ephemeral UUID's tick events (none: it carries no
    // body) and any destroy residue are absent, while the unrelated resting
    // pair's contacts still flow — the filter is selective, not a wipe.
    T6Fixture f;
    const T6RestScene scene = T6BuildRestScene(f);

    T6NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    DeterministicUuidProvider runtimeIds;
    ctrl.SetRuntimeUuidProvider(&runtimeIds);
    T6EventProbeDispatch probe;
    probe.ctrl = &ctrl;
    ctrl.SetScriptDispatch(&probe);
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
    CHECK(ctrl.PhysicsBodyCount() == 2);

    RuntimeEntityCreateDesc desc;
    desc.name = "Ephemeral";
    auto rC = ctrl.QueueCreateRuntimeEntity(desc);
    REQUIRE(rC.IsOk());
    REQUIRE(ctrl.QueueDestroyRuntimeEntity(rC.value).IsOk());
    ctrl.Update(kFixedDt, bridge);

    REQUIRE(probe.destroyCallCount == 1);
    REQUIRE(probe.destroyCalls.front().size() == 1);
    CHECK(probe.destroyCalls.front().front() == rC.value);
    const SceneDocument* runtime = ctrl.TryGetRuntimeScene();
    REQUIRE(runtime != nullptr);
    CHECK_FALSE(runtime->uuidIndex.Contains(rC.value));
    CHECK(runtime->uuidIndex.Contains(scene.ground));
    CHECK(runtime->uuidIndex.Contains(scene.box));
    CHECK(ctrl.PhysicsBodyCount() == 2);
    // No event mentions the ephemeral UUID; the resting contact survived.
    bool sawRestContact = false;
    for (const auto& e : ctrl.PhysicsEvents())
    {
        CHECK_FALSE(T6EventMentions(e, rC.value));
        if (e.kind == PhysicsEventKind::Contact)
            sawRestContact = true;
    }
    CHECK(sawRestContact);
    CHECK(ctrl.PendingOperationCount() == 0);
    ctrl.Stop(f.Authoring(), bridge);
    CHECK(ctrl.PhysicsTotalHandles() == 0);
}

TEST_CASE("T6 RED_OnDestroyEnqueueDeferred: callback work defers past the drain while event flow continues")
{
    // T6 dimension of the T5 reentrancy proof: the frozen batch still diverts
    // callback-enqueued destroys to the next safe point (no live-vector
    // invalidation), the physics world keeps stepping underneath, and the
    // snapshots never reference the destroyed UUIDs.
    T6Fixture f;
    const T6RestScene scene = T6BuildRestScene(f);
    const UUID plain = f.Create("Plain");

    T6NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    DeterministicUuidProvider runtimeIds;
    ctrl.SetRuntimeUuidProvider(&runtimeIds);
    T6EventProbeDispatch probe;
    probe.ctrl = &ctrl;
    ctrl.SetScriptDispatch(&probe);
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));

    probe.queueDestroyArmed = true;
    probe.queuedDestroyTarget = plain;
    REQUIRE(ctrl.QueueDestroyRuntimeEntity(scene.box).IsOk());
    ctrl.Update(kFixedDt, bridge);

    // First drain: the box is gone with its callback; the plain destroy
    // deferred to the next safe point; no snapshot mentions the box.
    REQUIRE(probe.destroyCallCount == 1);
    const SceneDocument* runtime = ctrl.TryGetRuntimeScene();
    REQUIRE(runtime != nullptr);
    CHECK_FALSE(runtime->uuidIndex.Contains(scene.box));
    CHECK(runtime->uuidIndex.Contains(plain));
    CHECK(ctrl.PendingOperationCount() == 1);
    CHECK(ctrl.PhysicsBodyCount() == 1);
    for (const auto& e : ctrl.PhysicsEvents())
        CHECK_FALSE(T6EventMentions(e, scene.box));

    ctrl.Update(kFixedDt, bridge);

    // Second drain: the deferred destroy applied with its own callback.
    REQUIRE(probe.destroyCallCount == 2);
    CHECK_FALSE(runtime->uuidIndex.Contains(plain));
    CHECK(runtime->uuidIndex.Contains(scene.ground));
    CHECK(ctrl.PendingOperationCount() == 0);
    CHECK(ctrl.PhysicsBodyCount() == 1);
    for (const auto& e : ctrl.PhysicsEvents())
        CHECK_FALSE(T6EventMentions(e, scene.box));
    ctrl.Stop(f.Authoring(), bridge);
}

TEST_CASE("T6 RED_CommandTargetingDestroyingUuidRejected: kinematic write refuses inside the drain without mutation")
{
    // T6 discrimination beyond the T5 plain-entity proof: a Kinematic body
    // accepts sink pose writes in normal play (ECS -> Bullet authority), so
    // a drain-time refusal proves the frozen destroy-set gate rather than
    // the per-kind authority gate. Reads stay allowed; the drain completes.
    T6Fixture f;
    const UUID ground = f.Create("Ground");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(ground), T6StaticBody());
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(ground),
                                                T6BoxShape(5.0f, 0.5f, 5.0f));
    f.Registry().get<Transform>(f.Handle(ground)).translation =
        {0.0f, -0.5f, 0.0f};
    const UUID kin = f.Create("Kin");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(kin), T6KinematicBody());
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(kin),
                                                T6BoxShape(0.25f, 0.25f, 0.25f));
    f.Registry().get<Transform>(f.Handle(kin)).translation =
        {0.0f, 2.0f, 0.0f};

    T6NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    DeterministicUuidProvider runtimeIds;
    ctrl.SetRuntimeUuidProvider(&runtimeIds);
    RuntimeCommandSink sink(ctrl);
    T6EventProbeDispatch probe;
    probe.ctrl = &ctrl;
    probe.sink = &sink;
    ctrl.SetScriptDispatch(&probe);
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));

    // Control: outside the drain the kinematic pose write is accepted.
    REQUIRE(sink.SetPosition(kin, {0.0f, 3.0f, 0.0f}));
    glm::vec3 before{0.0f, 0.0f, 0.0f};
    REQUIRE(sink.GetPosition(kin, before));
    CHECK(before == glm::vec3(0.0f, 3.0f, 0.0f));

    probe.destroyingWriteArmed = true;
    probe.writeTarget = kin;
    probe.writePos = {5.0f, 5.0f, 5.0f};
    REQUIRE(ctrl.QueueDestroyRuntimeEntity(ground).IsOk());
    REQUIRE(ctrl.QueueDestroyRuntimeEntity(kin).IsOk());
    ctrl.Update(kFixedDt, bridge);

    CHECK_FALSE(probe.writeResult);
    REQUIRE(probe.postWriteOk);
    CHECK(probe.postWritePos == before);
    CHECK(probe.destroyCallCount == 2);
    const SceneDocument* runtime = ctrl.TryGetRuntimeScene();
    REQUIRE(runtime != nullptr);
    CHECK_FALSE(runtime->uuidIndex.Contains(ground));
    CHECK_FALSE(runtime->uuidIndex.Contains(kin));
    CHECK(ctrl.PendingOperationCount() == 0);
    CHECK(ctrl.PhysicsTotalHandles() == 0);
    ctrl.Stop(f.Authoring(), bridge);
}

TEST_CASE("T6 GREEN_ValidationFailurePreservesBatchAndEvents: failed validation mutates nothing while events flow")
{
    // The T5 batch-preservation contract plus the T6 event contract: a drain
    // that fails validation leaves the queue, the ECS, and the Bullet world
    // untouched — and still publishes this frame's tick events (the failure
    // destroyed nothing, so the filter is empty).
    T6Fixture f;
    const T6RestScene scene = T6BuildRestScene(f);

    T6NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));

    ctrl.Update(kFixedDt, bridge);
    REQUIRE_FALSE(ctrl.PhysicsEvents().empty());

    const UUID missing = UUID::Parse("00000000-0000-4000-8000-00000000ffff");
    REQUIRE(ctrl.QueueDestroyRuntimeEntity(missing).IsOk());
    ctrl.Update(kFixedDt, bridge);

    // Queue preserved, world untouched, events still published.
    CHECK(ctrl.PendingOperationCount() == 1);
    const SceneDocument* runtime = ctrl.TryGetRuntimeScene();
    REQUIRE(runtime != nullptr);
    CHECK(runtime->uuidIndex.Contains(scene.ground));
    CHECK(runtime->uuidIndex.Contains(scene.box));
    CHECK(ctrl.PhysicsBodyCount() == 2);
    const std::vector<PhysicsEvent> snap = ctrl.PhysicsEvents();
    REQUIRE_FALSE(snap.empty());
    for (const auto& e : snap)
    {
        CHECK(e.kind == PhysicsEventKind::Contact);
        T6CheckCanonical(e);
    }
    ctrl.Stop(f.Authoring(), bridge);
}

TEST_CASE("T6 GREEN_ZeroTickPublishesFreshEmptySnapshot: a tickless frame never serves stale data")
{
    // After a frame with real contact events, a zero-dt frame runs zero
    // fixed ticks: the published snapshot must be a fresh empty one, not
    // the previous frame's contents.
    T6Fixture f;
    const T6RestScene scene = T6BuildRestScene(f);
    (void)scene;

    T6NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));

    ctrl.Update(kFixedDt, bridge);
    REQUIRE_FALSE(ctrl.PhysicsEvents().empty());

    ctrl.Update(0.0f, bridge);
    CHECK(ctrl.PhysicsEvents().empty());
    CHECK(ctrl.TryGetPhysicsWorldMut()->StepCount() == 1);
    ctrl.Stop(f.Authoring(), bridge);
}

TEST_CASE("T6 GREEN_StopClearsSnapshotAndOverlapHistory: re-Play starts with Enter, never a phantom Exit")
{
    // Stop drops the published snapshot and the ghost-overlap history
    // without fabricating events. A re-Play over the same authoring scene
    // therefore reports TriggerEnter on first overlap — no stale Exit leaks
    // across the session boundary.
    T6Fixture f;
    const T6GhostScene scene = T6BuildGhostScene(f);

    T6NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));

    bool entered = false;
    for (int i = 0; i < 60 && !entered; ++i)
    {
        ctrl.Update(kFixedDt, bridge);
        for (const auto& e : ctrl.PhysicsEvents())
        {
            if (e.kind == PhysicsEventKind::TriggerEnter &&
                T6EventMentions(e, scene.ghost))
                entered = true;
        }
    }
    REQUIRE(entered);

    ctrl.Stop(f.Authoring(), bridge);
    CHECK(ctrl.PhysicsEvents().empty());
    CHECK(ctrl.TryGetPhysicsWorld() == nullptr);

    // Re-Play: the first trigger event for the pair must be an Enter.
    T6NullBridge bridge2;
    REQUIRE(ctrl.Play(f.Authoring(), bridge2, err));
    CHECK(ctrl.PhysicsEvents().empty());
    PhysicsEventKind firstKind = PhysicsEventKind::TriggerExit;
    bool sawTrigger = false;
    for (int i = 0; i < 60 && !sawTrigger; ++i)
    {
        ctrl.Update(kFixedDt, bridge2);
        for (const auto& e : ctrl.PhysicsEvents())
        {
            if (T6EventMentions(e, scene.ghost) &&
                T6EventMentions(e, scene.ball) &&
                e.kind != PhysicsEventKind::Contact)
            {
                firstKind = e.kind;
                sawTrigger = true;
                break;
            }
        }
    }
    REQUIRE(sawTrigger);
    CHECK(firstKind == PhysicsEventKind::TriggerEnter);
    ctrl.Stop(f.Authoring(), bridge2);
}
