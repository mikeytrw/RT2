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

#include <cmath>
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

    // Re-review P1(1) stale-read discriminator: every pre-publication read
    // must observe an empty snapshot. Counts all OnFixedUpdate calls and
    // records whether any of them (or the destroy callback) saw events.
    int fixedCalls = 0;
    bool fixedSawNonEmpty = false;
    bool destroyingSawNonEmpty = false;

    // Reentrancy arms (one-shot, consumed by the first OnEntitiesDestroying).
    bool queueDestroyArmed = false;
    UUID queuedDestroyTarget;
    bool destroyingWriteArmed = false;
    UUID writeTarget;
    glm::vec3 writePos{0.0f, 0.0f, 0.0f};
    bool writeResult = true;
    glm::vec3 postWritePos{0.0f, 0.0f, 0.0f};
    bool postWriteOk = false;

    void OnFixedUpdate(float) override
    {
        ++fixedCalls;
        if (ctrl != nullptr && !ctrl->PhysicsEvents().empty())
            fixedSawNonEmpty = true;
    }
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
        if (ctrl != nullptr && !ctrl->PhysicsEvents().empty())
            destroyingSawNonEmpty = true;
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

    // Re-review payload semantics: the canonical normal points from bodyB
    // toward bodyA (never the reverse), and the coalesced position sits at
    // the ground-top/box-bottom interface (y ~= 0), not at an arbitrary
    // manifold point. A mutation replacing the mean with one raw point or
    // dropping the swap-negation fails here.
    glm::vec3 posA{0.0f, 0.0f, 0.0f};
    glm::vec3 posB{0.0f, 0.0f, 0.0f};
    REQUIRE(ctrl2.TryGetPhysicsWorldMut()->BodyWorldPosition(lo2, posA));
    REQUIRE(ctrl2.TryGetPhysicsWorldMut()->BodyWorldPosition(hi2, posB));
    for (const auto& e : three)
    {
        CHECK(glm::dot(e.normal, posA - posB) > 0.0f);
        CHECK(std::abs(e.position.y) < 0.15f);
    }
    ctrl2.Stop(f2.Authoring(), bridge2);

    // Five-tick frame: the kMaxSubsteps cap accumulates all five ticks.
    T6Fixture f3;
    const T6RestScene scene3 = T6BuildRestScene(f3);
    T6NullBridge bridge3;
    RuntimeSceneController ctrl3;
    REQUIRE(ctrl3.Play(f3.Authoring(), bridge3, err));
    ctrl3.Update(5.0f * kFixedDt, bridge3);
    const std::vector<PhysicsEvent> five = ctrl3.PhysicsEvents();
    REQUIRE(five.size() == 5);
    for (size_t i = 0; i < five.size(); ++i)
    {
        CHECK(five[i].kind == PhysicsEventKind::Contact);
        CHECK(five[i].tickIndex == (uint32_t)i);
        CHECK(five[i].impulse > 0.0f);
    }
    CHECK(ctrl3.TryGetPhysicsWorldMut()->StepCount() == 5);
    ctrl3.Stop(f3.Authoring(), bridge3);
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

TEST_CASE("T6 RED_StaleSnapshotHiddenPrePublication: OnFixedUpdate and OnDestroy never see the previous frame")
{
    // Re-review P1(1): frame N publishes a contact; frame N+1's fixed
    // callbacks run before the new scrape and its destroy callback runs
    // before the new publish. Both must observe an empty snapshot — the old
    // code retained frame N's vector across BeginPhysicsFrame, so a destroy
    // callback could read exactly the stale event the later filter exists
    // to suppress. The current frame's OnUpdate then carries the correctly
    // filtered snapshot.
    T6Fixture f;
    const T6RestScene scene = T6BuildRestScene(f);
    const UUID plain = f.Create("Plain");

    T6NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    T6EventProbeDispatch probe;
    probe.ctrl = &ctrl;
    ctrl.SetScriptDispatch(&probe);
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));

    // Frame 1 publishes a real contact snapshot.
    ctrl.Update(kFixedDt, bridge);
    REQUIRE_FALSE(ctrl.PhysicsEvents().empty());
    REQUIRE(probe.fixedCalls == 1);

    // Frame 2 destroys an unrelated entity: fixed + destroy callbacks run
    // pre-publication and must see nothing; OnUpdate sees frame 2's events.
    probe.fixedCalls = 0;
    probe.fixedSawNonEmpty = false;
    probe.destroyingSawNonEmpty = false;
    REQUIRE(ctrl.QueueDestroyRuntimeEntity(plain).IsOk());
    ctrl.Update(kFixedDt, bridge);

    REQUIRE(probe.fixedCalls == 1);
    CHECK_FALSE(probe.fixedSawNonEmpty);
    REQUIRE(probe.destroyCallCount == 1);
    CHECK_FALSE(probe.destroyingSawNonEmpty);
    const std::vector<PhysicsEvent> snap = ctrl.PhysicsEvents();
    REQUIRE_FALSE(snap.empty());
    for (const auto& e : snap)
    {
        CHECK(e.kind == PhysicsEventKind::Contact);
        CHECK_FALSE(T6EventMentions(e, plain));
        T6CheckCanonical(e);
    }
    const SceneDocument* runtime = ctrl.TryGetRuntimeScene();
    REQUIRE(runtime != nullptr);
    CHECK_FALSE(runtime->uuidIndex.Contains(plain));
    ctrl.Stop(f.Authoring(), bridge);
    CHECK(ctrl.PhysicsEvents().empty());
}

TEST_CASE("T6 GREEN_RotatedGhostNarrowphaseConfirmed: AABB-only pairs stay silent while true overlap reports")
{
    // Re-review P1(2): the ghost pair cache is broadphase-AABB-based. A thin
    // slab rotated 45 degrees about Z has a world AABB far larger than its
    // shape; a ball inside that AABB but clear of the oriented box must not
    // produce Enter/Stay, while a genuinely overlapping pair still does.
    T6Fixture f;

    // False control: rotated thin slab at the origin + a held kinematic
    // ball at (1.2, 0, 0) — inside the slab's AABB (+/-1.49) but 0.85 off
    // the slab plane (shape half-thickness 0.1 + radius 0.2 = 0.3).
    const UUID slab = f.Create("RotatedSlab");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(slab), T6GhostBody());
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(slab),
                                                T6GhostSlab(2.0f, 0.1f, 2.0f));
    f.Registry().get<Transform>(f.Handle(slab)).rotation =
        glm::angleAxis(3.14159265358979323846f / 4.0f,
                       glm::vec3{0.0f, 0.0f, 1.0f});
    const UUID near = f.Create("NearBall");
    PhysicsBodyComponent nearBody = T6KinematicBody();
    nearBody.mask = nearBody.mask | PhysicsLayer::Trigger;
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(near), nearBody);
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(near),
                                                T6SphereShape(0.2f));
    f.Registry().get<Transform>(f.Handle(near)).translation =
        {1.2f, 0.0f, 0.0f};

    // True control: axis-aligned ghost slab with a ball falling through it.
    const UUID ghost = f.Create("Ghost");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(ghost), T6GhostBody());
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(ghost),
                                                T6GhostSlab(2.0f, 0.25f, 2.0f));
    f.Registry().get<Transform>(f.Handle(ghost)).translation =
        {0.0f, -3.0f, 0.0f};
    const UUID ball = f.Create("Ball");
    PhysicsBodyComponent ballBody = T6DynamicBody(1.0f);
    ballBody.mask = ballBody.mask | PhysicsLayer::Trigger;
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(ball), ballBody);
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(ball),
                                                T6SphereShape(0.2f));
    f.Registry().get<Transform>(f.Handle(ball)).translation =
        {0.0f, -1.5f, 0.0f};

    T6NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));

    bool trueEnterSeen = false;
    for (int i = 0; i < 120; ++i)
    {
        ctrl.Update(kFixedDt, bridge);
        for (const auto& e : ctrl.PhysicsEvents())
        {
            // The AABB-only pair must never appear in any event.
            const bool falsePairMentioned =
                T6EventMentions(e, slab) && T6EventMentions(e, near);
            CHECK_FALSE(falsePairMentioned);
            if (e.kind == PhysicsEventKind::TriggerEnter &&
                T6EventMentions(e, ghost) && T6EventMentions(e, ball))
                trueEnterSeen = true;
        }
    }
    // The genuinely overlapping pair reported; the false pair never did.
    CHECK(trueEnterSeen);
    CHECK(T6RuntimeY(ctrl.TryGetRuntimeScene(), ball) < -3.5f);
    ctrl.Stop(f.Authoring(), bridge);
}

TEST_CASE("T6 GREEN_TriggerTriggerOverlapAndDisappearance: ghost pairs report Enter and vanish without Exit")
{
    // Re-review P1(2) second half: two overlapping static ghosts form a real
    // shape-overlap pair (both directions confirm), and destroying one
    // participant purges the history so no Exit is fabricated for a UUID
    // that no longer resolves.
    T6Fixture f;
    const UUID ghostA = f.Create("GhostA");
    PhysicsBodyComponent ghostABody = T6GhostBody();
    ghostABody.mask = ghostABody.mask | PhysicsLayer::Trigger;
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(ghostA), ghostABody);
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(ghostA),
                                                T6GhostSlab(1.0f, 1.0f, 1.0f));
    const UUID ghostB = f.Create("GhostB");
    PhysicsBodyComponent ghostBBody = T6GhostBody();
    ghostBBody.mask = ghostBBody.mask | PhysicsLayer::Trigger;
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(ghostB), ghostBBody);
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(ghostB),
                                                T6GhostSlab(1.0f, 1.0f, 1.0f));
    f.Registry().get<Transform>(f.Handle(ghostB)).translation =
        {0.5f, 0.0f, 0.0f};

    T6NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));

    // Overlapping volumes: the ghost-ghost pair reports Enter.
    ctrl.Update(kFixedDt, bridge);
    bool pairEnterSeen = false;
    for (const auto& e : ctrl.PhysicsEvents())
    {
        if (e.kind == PhysicsEventKind::TriggerEnter &&
            T6EventMentions(e, ghostA) && T6EventMentions(e, ghostB))
            pairEnterSeen = true;
    }
    REQUIRE(pairEnterSeen);

    // Destroy one participant mid-overlap: no Exit follows for the dead pair.
    REQUIRE(ctrl.QueueDestroyRuntimeEntity(ghostB).IsOk());
    ctrl.Update(kFixedDt, bridge);
    for (const auto& e : ctrl.PhysicsEvents())
    {
        CHECK_FALSE(T6EventMentions(e, ghostB));
        CHECK(e.kind != PhysicsEventKind::TriggerExit);
    }
    const SceneDocument* runtime = ctrl.TryGetRuntimeScene();
    REQUIRE(runtime != nullptr);
    CHECK_FALSE(runtime->uuidIndex.Contains(ghostB));
    CHECK(runtime->uuidIndex.Contains(ghostA));
    ctrl.Stop(f.Authoring(), bridge);
}

TEST_CASE("T6 GREEN_ZeroImpulseContactPreserved: a real touching manifold reports even with zero solver impulse")
{
    // Re-review P1(3): Contact is narrowphase-confirmed touch, not impact. A
    // kinematic box resting (penetrating 0.01) on a static slab forms a real
    // manifold the solver leaves at zero impulse — the old gate discarded
    // it. The dynamic pair beside it keeps a meaningful positive impulse.
    T6Fixture f;
    const UUID slab = f.Create("Slab");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(slab), T6StaticBody());
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(slab),
                                                T6BoxShape(5.0f, 0.5f, 5.0f));
    f.Registry().get<Transform>(f.Handle(slab)).translation =
        {0.0f, -0.5f, 0.0f};

    const UUID kin = f.Create("Kin");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(kin), T6KinematicBody());
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(kin),
                                                T6BoxShape(0.25f, 0.25f, 0.25f));
    f.Registry().get<Transform>(f.Handle(kin)).translation =
        {-2.0f, 0.24f, 0.0f};

    const UUID box = f.Create("Box");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(box), T6DynamicBody(1.0f));
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(box),
                                                T6BoxShape(0.25f, 0.25f, 0.25f));
    f.Registry().get<Transform>(f.Handle(box)).translation =
        {2.0f, 0.24f, 0.0f};

    T6NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));

    ctrl.Update(kFixedDt, bridge);
    const std::vector<PhysicsEvent> snap = ctrl.PhysicsEvents();
    bool zeroContactSeen = false;
    bool positiveContactSeen = false;
    for (const auto& e : snap)
    {
        REQUIRE(e.kind == PhysicsEventKind::Contact);
        T6CheckCanonical(e);
        if (T6EventMentions(e, kin) && T6EventMentions(e, slab))
        {
            zeroContactSeen = true;
            CHECK(e.impulse == 0.0f);
        }
        if (T6EventMentions(e, box) && T6EventMentions(e, slab))
        {
            positiveContactSeen = true;
            CHECK(e.impulse > 0.0f);
        }
    }
    CHECK(zeroContactSeen);
    CHECK(positiveContactSeen);
    ctrl.Stop(f.Authoring(), bridge);
}

TEST_CASE("T6 GREEN_MixedTriggerGroupOrder: one tick emits contacts, enters, stays, exits in canonical order")
{
    // Re-review P2(4): with one pair staying, one entering, and one exiting
    // in the same tick, the snapshot must group by kind (contacts, then all
    // enters, then all stays, then all exits; pair order within each group).
    // The staying pair sorts lower than the entering pair, so the old
    // pair-major interleave (Stay before Enter) fails this test.
    T6Fixture f;

    // Dynamic resting contact: the contacts group anchor.
    const T6RestScene rest = T6BuildRestScene(f);

    auto makeGhostBall = [&](const char* gname, const char* bname,
                             const glm::vec3& ballPos) {
        const UUID ghost = f.Create(gname);
        f.Registry().emplace<PhysicsBodyComponent>(f.Handle(ghost),
                                                   T6GhostBody());
        f.Registry().emplace<PhysicsShapeComponent>(f.Handle(ghost),
                                                    T6GhostSlab(0.5f, 0.5f, 0.5f));
        const UUID ball = f.Create(bname);
        PhysicsBodyComponent ballBody = T6KinematicBody();
        ballBody.mask = ballBody.mask | PhysicsLayer::Trigger;
        f.Registry().emplace<PhysicsBodyComponent>(f.Handle(ball), ballBody);
        f.Registry().emplace<PhysicsShapeComponent>(f.Handle(ball),
                                                    T6SphereShape(0.2f));
        f.Registry().get<Transform>(f.Handle(ball)).translation = ballPos;
        return std::pair<UUID, UUID>(ghost, ball);
    };

    // Creation order keeps the staying pairs' UUIDs below the entering one.
    const auto stay1 = makeGhostBall("G1", "B1", {10.0f, 0.0f, 0.0f});
    f.Registry().get<Transform>(f.Handle(stay1.first)).translation =
        {10.0f, 0.0f, 0.0f};
    const auto stay2 = makeGhostBall("G2", "B2", {20.0f, 0.0f, 0.0f});
    f.Registry().get<Transform>(f.Handle(stay2.first)).translation =
        {20.0f, 0.0f, 0.0f};
    const auto exiting = makeGhostBall("G3", "B3", {30.0f, 0.0f, 0.0f});
    f.Registry().get<Transform>(f.Handle(exiting.first)).translation =
        {30.0f, 0.0f, 0.0f};
    const auto entering = makeGhostBall("G4", "B4", {40.0f, 5.0f, 0.0f});
    f.Registry().get<Transform>(f.Handle(entering.first)).translation =
        {40.0f, 0.0f, 0.0f};

    T6NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    DeterministicUuidProvider runtimeIds;
    ctrl.SetRuntimeUuidProvider(&runtimeIds);
    RuntimeCommandSink sink(ctrl);
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));

    // Frame 1: three overlaps report Enter.
    ctrl.Update(kFixedDt, bridge);
    int frame1Enters = 0;
    for (const auto& e : ctrl.PhysicsEvents())
    {
        if (e.kind == PhysicsEventKind::TriggerEnter)
            ++frame1Enters;
    }
    REQUIRE(frame1Enters == 3);

    // Frame 2: B3 leaves its ghost (Exit), B4 drops into its ghost (Enter),
    // B1/B2 hold still (Stay). Kinematic pose writes apply next pre-step.
    REQUIRE(sink.SetPosition(exiting.second, {30.0f, 5.0f, 0.0f}));
    REQUIRE(sink.SetPosition(entering.second, {40.0f, 0.0f, 0.0f}));
    ctrl.Update(kFixedDt, bridge);

    const std::vector<PhysicsEvent> snap = ctrl.PhysicsEvents();
    // The trap is armed only if a staying pair sorts below the entering one.
    const UUID stayPairA =
        (stay1.first < stay1.second) ? stay1.first : stay1.second;
    const UUID enterPairA = (entering.first < entering.second)
                                ? entering.first
                                : entering.second;
    REQUIRE(stayPairA < enterPairA);

    // Exact group contract: contacts, then enters, then stays, then exits;
    // canonical pair order inside each group.
    REQUIRE(snap.size() == 5);
    CHECK(snap[0].kind == PhysicsEventKind::Contact);
    CHECK(snap[1].kind == PhysicsEventKind::TriggerEnter);
    CHECK(snap[2].kind == PhysicsEventKind::TriggerStay);
    CHECK(snap[3].kind == PhysicsEventKind::TriggerStay);
    CHECK(snap[4].kind == PhysicsEventKind::TriggerExit);
    CHECK(T6EventMentions(snap[0], rest.ground));
    CHECK(T6EventMentions(snap[0], rest.box));
    CHECK(T6EventMentions(snap[1], entering.first));
    CHECK(T6EventMentions(snap[1], entering.second));
    CHECK(T6EventMentions(snap[4], exiting.first));
    CHECK(T6EventMentions(snap[4], exiting.second));
    const UUID stayA1 = snap[2].bodyA;
    const UUID stayA2 = snap[3].bodyA;
    CHECK(stayA1 < stayA2);
    for (const auto& e : snap)
        T6CheckCanonical(e);
    ctrl.Stop(f.Authoring(), bridge);
}
