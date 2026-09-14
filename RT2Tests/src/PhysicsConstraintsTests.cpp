// ============================================================================
// PhysicsConstraintsTests — Bullet T5 driven hinge/slider constraints.
//
// Permanent discriminating RED/GREEN coverage for ticket
// t5-hinge-slider-constraints: stable-UUID post-body construction with exact
// owner/other/world frames and typed validation; hinge limits/motor/release/
// return and slider limits/monotonic-target/release-impulse behavior; CPU
// authoring APIs with exact Undo/Redo and minimal Inspector controls plus
// prefab-member refusal; body-to-constraint dependency indexes with atomic
// rebuilds; destroy-batch orphan rejection; constraints-before-bodies
// teardown; copy/prefab reference remapping Play proofs; handle census.
//
// CPU-only by design: no Vulkan, ImGui or Walnut (same guards as the T3/T4
// units). Every Play-based case runs through RuntimeSceneController::Play;
// every refusal asserts the atomic candidate-commit contract (Edit, zero
// accumulator, no runtime, no world, no handles, no bridge traffic, UUID-
// named typed Error). No T6 events, T7 Lua, T8 debug draw, gameplay, or
// audio scope.
// ============================================================================

#include <doctest/doctest.h>
#include "PhysicsInspectorState.h"
#include "PhysicsWorld.h"
#include "RuntimeSceneController.h"
#include "RuntimeLifecycleObserver.h"
#include "SceneGraph.h"
#include "SceneManager.h"
#include "EditorCommandHistory.h"
#include "EditorPropertyCommands.h"
#include "ECSComponents.h"
#include "ECSScene.h"
#include "ISceneRenderBridge.h"
#include "GPUSceneData.h"
#include "core/Error.h"
#include "core/UUID.h"

#ifdef IMGUI_VERSION
#error "T5 boundary: physics constraint tests must not import ImGui"
#endif

#ifdef VK_HEADER_VERSION
#error "T5 boundary: physics constraint tests must not import Vulkan"
#endif

#if __has_include("imgui.h")
#error "T5 boundary: imgui.h must not be reachable from physics constraint units"
#endif

#if __has_include("vulkan/vulkan.h")
#error "T5 boundary: vulkan headers must not be reachable from physics constraint units"
#endif

#if __has_include("Walnut/Application.h")
#error "T5 boundary: Walnut headers must not be reachable from physics constraint units"
#endif

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

using namespace rt2::core;

namespace {

class T5NullBridge final : public ISceneRenderBridge
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

    bool Quiet() const
    {
        return fullSyncCalls == 0 && transformSyncCalls == 0 &&
               resetTemporalCalls == 0 && renderRequests == 0;
    }
};

class T5NoopObserver final : public IRuntimeLifecycleObserver
{
public:
    int starts = 0;
    int stops  = 0;
    void OnSceneStart(const SceneDocument&) override { ++starts; }
    void OnSceneStop(const SceneDocument&) override  { ++stops; }
};

struct T5Fixture
{
    DeterministicUuidProvider ids;
    SceneManager manager;

    T5Fixture() { manager.SetUuidProvider(&ids); }

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

// Shared refusal contract: failed Play leaves Edit, zero accumulator, no
// runtime, no world, no handles, no bridge traffic, no script callbacks,
// and a typed Error naming the entity UUID.
void T5CheckCleanRefusal(RuntimeSceneController& ctrl, const T5NullBridge& bridge,
                         const T5NoopObserver& obs, const Error& err,
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

PhysicsBodyComponent T5StaticBody()
{
    PhysicsBodyComponent body;
    body.kind = PhysicsBodyKind::Static;
    body.mass = 0.0f;
    body.layer = PhysicsLayer::WorldStatic;
    body.mask = PhysicsLayer::Dynamic | PhysicsLayer::WorldStatic |
                PhysicsLayer::Mechanism;
    return body;
}

PhysicsBodyComponent T5DynamicBody(float mass = 1.0f)
{
    PhysicsBodyComponent body;
    body.kind = PhysicsBodyKind::Dynamic;
    body.mass = mass;
    body.layer = PhysicsLayer::Dynamic;
    body.mask = PhysicsLayer::WorldStatic | PhysicsLayer::Mechanism;
    return body;
}

PhysicsShapeComponent T5BoxShape(float hx, float hy, float hz)
{
    PhysicsShapeComponent shape;
    shape.shape = PhysicsShapeKind::Box;
    shape.halfExtents = {hx, hy, hz};
    return shape;
}

PhysicsShapeComponent T5SphereShape(float radius = 0.1f)
{
    PhysicsShapeComponent shape;
    shape.shape = PhysicsShapeKind::Sphere;
    shape.radius = radius;
    return shape;
}

constexpr float kT5Deg55 = 55.0f * 3.14159265358979323846f / 180.0f;

// Flipper hinge: owner-local pivot/axis plus the anchor-local pivot/axis.
// Vertical (Y) axes so gravity does no work about the hinge.
PhysicsHingeComponent T5FlipperHinge(const UUID& anchor)
{
    PhysicsHingeComponent hinge;
    hinge.otherBody = anchor;
    hinge.ownerPivot = {-0.5f, 0.0f, 0.0f};
    hinge.ownerAxis = {0.0f, 1.0f, 0.0f};
    hinge.otherPivot = {0.0f, 0.0f, 0.0f};
    hinge.otherAxis = {0.0f, 1.0f, 0.0f};
    hinge.minAngleLimit = 0.0f;
    hinge.maxAngleLimit = kT5Deg55;
    hinge.driveMode = 0;
    hinge.motorTargetVelocity = 18.0f;
    hinge.motorMaxImpulse = 8.0f;
    hinge.motorEnabled = true;
    hinge.restAngle = 0.0f;
    return hinge;
}

PhysicsSliderComponent T5PlungerSlider(const UUID& anchor, float target)
{
    PhysicsSliderComponent slider;
    slider.otherBody = anchor;
    slider.axis = {1.0f, 0.0f, 0.0f};
    slider.lowerLimit = 0.0f;
    slider.upperLimit = 0.3f;
    slider.targetPosition = target;
    slider.motorTargetVelocity = 8.0f;
    slider.motorMaxForce = 80.0f;
    slider.motorEnabled = true;
    return slider;
}

struct T5HingeScene
{
    UUID anchor;
    UUID flipper;
};

// Anchor static box at the origin; flipper dynamic box centered at +0.5 X
// with the hinge pivot at the world origin (spike flipper geometry, Y axis
// so the fixed world gravity never fights the motor).
T5HingeScene T5BuildHingeScene(T5Fixture& f, bool motorEnabled = true)
{
    T5HingeScene scene;
    scene.anchor = f.Create("Anchor");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(scene.anchor),
                                               T5StaticBody());
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(scene.anchor),
                                                T5BoxShape(0.05f, 0.05f, 0.05f));

    scene.flipper = f.Create("Flipper");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(scene.flipper),
                                               T5DynamicBody(1.0f));
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(scene.flipper),
                                                T5BoxShape(0.5f, 0.06f, 0.1f));
    f.Registry().get<Transform>(f.Handle(scene.flipper)).translation =
        {0.5f, 0.0f, 0.0f};
    PhysicsHingeComponent hinge = T5FlipperHinge(scene.anchor);
    hinge.motorEnabled = motorEnabled;
    f.Registry().emplace<PhysicsHingeComponent>(f.Handle(scene.flipper),
                                                hinge);
    return scene;
}

struct T5SliderScene
{
    UUID ground;
    UUID anchor;
    UUID plunger;
    UUID ball; // null when built without the ball
};

// Plunger at the origin on a static ground slab, ball resting ahead of it.
// The anchor sits aside (frames resolve through world transforms, so any
// anchor pose works); masks open Dynamic-vs-Dynamic so the plunger can
// strike the ball.
T5SliderScene T5BuildSliderScene(T5Fixture& f, float target, bool withBall,
                                 bool motorEnabled = true)
{
    T5SliderScene scene;
    scene.ground = f.Create("Ground");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(scene.ground),
                                               T5StaticBody());
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(scene.ground),
                                                T5BoxShape(5.0f, 0.5f, 5.0f));
    f.Registry().get<Transform>(f.Handle(scene.ground)).translation =
        {0.0f, -0.62f, 0.0f};

    scene.anchor = f.Create("Anchor");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(scene.anchor),
                                               T5StaticBody());
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(scene.anchor),
                                                T5BoxShape(0.05f, 0.05f, 0.05f));
    f.Registry().get<Transform>(f.Handle(scene.anchor)).translation =
        {0.0f, 0.0f, 0.5f};

    scene.plunger = f.Create("Plunger");
    PhysicsBodyComponent plungerBody = T5DynamicBody(2.0f);
    plungerBody.mask = plungerBody.mask | PhysicsLayer::Dynamic;
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(scene.plunger),
                                               plungerBody);
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(scene.plunger),
                                                T5BoxShape(0.12f, 0.12f, 0.12f));
    PhysicsSliderComponent slider = T5PlungerSlider(scene.anchor, target);
    slider.motorEnabled = motorEnabled;
    f.Registry().emplace<PhysicsSliderComponent>(f.Handle(scene.plunger),
                                                 slider);

    if (withBall)
    {
        scene.ball = f.Create("Ball");
        PhysicsBodyComponent ballBody = T5DynamicBody(1.0f);
        ballBody.mask = ballBody.mask | PhysicsLayer::Dynamic;
        f.Registry().emplace<PhysicsBodyComponent>(f.Handle(scene.ball),
                                                   ballBody);
        f.Registry().emplace<PhysicsShapeComponent>(f.Handle(scene.ball),
                                                    T5SphereShape(0.1f));
        f.Registry().get<Transform>(f.Handle(scene.ball)).translation =
            {0.34f, -0.02f, 0.0f};
    }
    return scene;
}

float T5RuntimeX(const SceneDocument* runtime, const UUID& uuid)
{
    const auto e = runtime->FindByUuid(uuid);
    const bool resolvedT5 = (e != entt::null);
    REQUIRE(resolvedT5);
    return runtime->ecs.registry.get<Transform>(e).translation.x;
}

// World-space hinge pivot error: the owner-pivot material point must stay
// glued to the anchor (negligible drift, never a teleport).
float T5HingePivotError(const SceneDocument* runtime, const UUID& flipper,
                        const glm::vec3& ownerPivot)
{
    const auto e = runtime->FindByUuid(flipper);
    const bool resolvedT5 = (e != entt::null);
    REQUIRE(resolvedT5);
    const glm::mat4& w =
        runtime->ecs.registry.get<Transform>(e).worldMatrix;
    const glm::vec4 p = w * glm::vec4(ownerPivot, 1.0f);
    return glm::length(glm::vec3(p));
}

struct T5TempDir
{
    std::filesystem::path dir;
    explicit T5TempDir(const char* name)
    {
        dir = std::filesystem::temp_directory_path() / name;
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        REQUIRE_MESSAGE(!ec, "T5 temp fixture setup failed");
    }
    ~T5TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

} // namespace

TEST_CASE("T5 GREEN_HingeReachesAngle: motorized hinge reaches ~55 degrees with negligible pivot drift")
{
    // Spike-anchored drive (18 rad/s, impulse 8, 60 ticks) against the
    // 55-degree limit; the Y axis keeps gravity out of the rotation plane.
    T5Fixture f;
    const T5HingeScene scene = T5BuildHingeScene(f);

    T5NullBridge bridge;
    T5NoopObserver obs;
    Error err;
    RuntimeSceneController ctrl;
    ctrl.SetLifecycleObserver(&obs);
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
    CHECK(ctrl.PhysicsConstraintCount() == 1);

    for (int i = 0; i < 60; ++i)
        ctrl.Update(kFixedDt, bridge);

    PhysicsWorld* world = ctrl.TryGetPhysicsWorldMut();
    REQUIRE(world != nullptr);
    bool ok = false;
    const float angle = world->HingeAngle(scene.flipper, ok);
    REQUIRE(ok);
    // Approximately 55 degrees: the motor saturates the limit and holds it.
    CHECK(angle > 50.0f * 3.14159265358979323846f / 180.0f);
    CHECK(angle <= 57.0f * 3.14159265358979323846f / 180.0f);
    const SceneDocument* runtime = ctrl.TryGetRuntimeScene();
    REQUIRE(runtime != nullptr);
    CHECK(T5HingePivotError(runtime, scene.flipper, {-0.5f, 0.0f, 0.0f}) < 0.02f);
    ctrl.Stop(f.Authoring(), bridge);
    CHECK(ctrl.PhysicsTotalHandles() == 0);
}

TEST_CASE("T5 GREEN_SliderTransfersImpulse: bounded travel with positive impulse transfer")
{
    // Spike-anchored launch (0.3 travel, 8 u/s, force 80, 120 ticks): the
    // plunger rides the ground slab into the resting ball and throws it.
    T5Fixture f;
    const T5SliderScene scene = T5BuildSliderScene(f, 0.3f, true);

    T5NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
    CHECK(ctrl.PhysicsConstraintCount() == 1);

    for (int i = 0; i < 120; ++i)
        ctrl.Update(kFixedDt, bridge);

    PhysicsWorld* world = ctrl.TryGetPhysicsWorldMut();
    REQUIRE(world != nullptr);
    bool ok = false;
    const float travel = world->SliderPosition(scene.plunger, ok);
    REQUIRE(ok);
    // Bounded travel: never below the limit, never past it.
    CHECK(travel >= -0.01f);
    CHECK(travel <= 0.31f);
    CHECK(travel > 0.2f);

    const SceneDocument* runtime = ctrl.TryGetRuntimeScene();
    REQUIRE(runtime != nullptr);
    const float ballX = T5RuntimeX(runtime, scene.ball);
    CHECK(ballX > 0.5f);
    const PhysicsBodyRecord* ballRec = world->FindBody(scene.ball);
    REQUIRE(ballRec != nullptr);
    REQUIRE(ballRec->body != nullptr);
    CHECK(ballRec->body->getLinearVelocity().x() > 0.1f);
    ctrl.Stop(f.Authoring(), bridge);
    CHECK(ctrl.PhysicsTotalHandles() == 0);
}

TEST_CASE("T5 GREEN_SliderTargetMonotonic: larger displacement maps to larger output")
{
    // Same mechanism, two authored targets, fixed tick budget: the
    // proportional target law must order the outputs monotonically while
    // both stay inside the limits.
    auto runAt = [](float target) {
        T5Fixture f;
        const T5SliderScene scene = T5BuildSliderScene(f, target, false);
        T5NullBridge bridge;
        Error err;
        RuntimeSceneController ctrl;
        REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
        // Early measurement (8 ticks): the proportional law orders the
        // outputs while both are still approaching (no limit saturation,
        // no overshoot settle).
        for (int i = 0; i < 8; ++i)
            ctrl.Update(kFixedDt, bridge);
        PhysicsWorld* world = ctrl.TryGetPhysicsWorldMut();
        REQUIRE(world != nullptr);
        bool ok = false;
        const float pos = world->SliderPosition(scene.plunger, ok);
        REQUIRE(ok);
        ctrl.Stop(f.Authoring(), bridge);
        return pos;
    };

    const float near = runAt(0.15f);
    const float far = runAt(0.3f);
    CHECK(near > 0.03f);
    CHECK(near < 0.145f);
    CHECK(far > near);
    CHECK(far > 0.1f);
    CHECK(far < 0.29f);
}

TEST_CASE("T5 GREEN_DriveLatencyOneTick: drive commands move bodies within one fixed tick")
{
    // Drive-command-to-motion latency is at most one fixed tick: one paused
    // Step after the command must already move the mechanism.
    T5Fixture f;
    const T5HingeScene hingeScene = T5BuildHingeScene(f, false);
    const T5SliderScene sliderScene =
        T5BuildSliderScene(f, 0.3f, false, false);

    T5NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
    PhysicsWorld* world = ctrl.TryGetPhysicsWorldMut();
    REQUIRE(world != nullptr);

    bool ok = false;
    CHECK(world->HingeAngle(hingeScene.flipper, ok) == doctest::Approx(0.0f));
    REQUIRE(ok);
    CHECK(world->SliderPosition(sliderScene.plunger, ok) == doctest::Approx(0.0f));
    REQUIRE(ok);

    REQUIRE(world->SetHingeDrive(hingeScene.flipper, 18.0f, 8.0f));
    REQUIRE(world->SetSliderTarget(sliderScene.plunger, 0.3f));
    CHECK_FALSE(world->SetSliderTarget(sliderScene.plunger, 99.0f));
    CHECK_FALSE(world->SetHingeDrive(UUID::Nil(), 1.0f, 1.0f));

    // Body motion is the latency signal: getLinearPos lags one solve behind
    // the integrated body (proven by probe), so the slider asserts the
    // plunger world position, not the cached constraint readout.
    glm::vec3 plungerBefore{0.0f, 0.0f, 0.0f};
    REQUIRE(world->BodyWorldPosition(sliderScene.plunger, plungerBefore));

    ctrl.Pause();
    REQUIRE(ctrl.Step(bridge));

    CHECK(std::abs(world->HingeAngle(hingeScene.flipper, ok)) > 0.01f);
    REQUIRE(ok);
    glm::vec3 plungerAfter{0.0f, 0.0f, 0.0f};
    REQUIRE(world->BodyWorldPosition(sliderScene.plunger, plungerAfter));
    CHECK(glm::length(plungerAfter - plungerBefore) > 0.005f);
    ctrl.Stop(f.Authoring(), bridge);
    CHECK(ctrl.PhysicsTotalHandles() == 0);
}

TEST_CASE("T5 GREEN_HingeReleaseAndReturn: release frees without teleport, return reaches rest")
{
    T5Fixture f;
    const T5HingeScene scene = T5BuildHingeScene(f);

    T5NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
    PhysicsWorld* world = ctrl.TryGetPhysicsWorldMut();
    REQUIRE(world != nullptr);
    for (int i = 0; i < 60; ++i)
        ctrl.Update(kFixedDt, bridge);

    bool ok = false;
    REQUIRE(world->HingeMotorEnabled(scene.flipper, ok));
    REQUIRE(ok);
    REQUIRE(world->ReleaseHingeDrive(scene.flipper));
    CHECK_FALSE(world->HingeMotorEnabled(scene.flipper, ok));
    REQUIRE(ok);

    // Release never teleports: snapshot the body pose, step freely, and
    // require continuity plus limit containment.
    glm::vec3 beforePos{0.0f, 0.0f, 0.0f};
    REQUIRE(world->BodyWorldPosition(scene.flipper, beforePos));
    for (int i = 0; i < 30; ++i)
        ctrl.Update(kFixedDt, bridge);
    const float released = world->HingeAngle(scene.flipper, ok);
    REQUIRE(ok);
    CHECK(released >= -0.02f);
    CHECK(released <= kT5Deg55 + 0.02f);
    glm::vec3 afterPos{0.0f, 0.0f, 0.0f};
    REQUIRE(world->BodyWorldPosition(scene.flipper, afterPos));
    CHECK(glm::length(afterPos - beforePos) < 0.05f);

    // Return drives back to the authored rest angle without teleporting.
    REQUIRE(world->ReturnHingeToRest(scene.flipper));
    for (int i = 0; i < 180; ++i)
        ctrl.Update(kFixedDt, bridge);
    const float returned = world->HingeAngle(scene.flipper, ok);
    REQUIRE(ok);
    CHECK(std::abs(returned) < 0.09f);
    ctrl.Stop(f.Authoring(), bridge);
    CHECK(ctrl.PhysicsTotalHandles() == 0);
}

TEST_CASE("T5 GREEN_SliderReleaseImpulseBounded: release impulse stays within limits")
{
    T5Fixture f;
    const T5SliderScene scene = T5BuildSliderScene(f, 0.3f, false);

    T5NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
    PhysicsWorld* world = ctrl.TryGetPhysicsWorldMut();
    REQUIRE(world != nullptr);
    for (int i = 0; i < 120; ++i)
        ctrl.Update(kFixedDt, bridge);

    REQUIRE(world->ReleaseSlider(scene.plunger, 5.0f));
    CHECK_FALSE(world->ReleaseSlider(scene.plunger,
                                     std::numeric_limits<float>::infinity()));
    const PhysicsBodyRecord* rec = world->FindBody(scene.plunger);
    REQUIRE(rec != nullptr);
    REQUIRE(rec->body != nullptr);
    CHECK(rec->body->getLinearVelocity().x() > 1.5f);
    const PhysicsConstraintRecord* crec = world->FindConstraint(scene.plunger);
    REQUIRE(crec != nullptr);
    REQUIRE(crec->slider != nullptr);
    CHECK_FALSE(crec->slider->getPoweredLinMotor());

    for (int i = 0; i < 60; ++i)
        ctrl.Update(kFixedDt, bridge);
    bool ok = false;
    const float pos = world->SliderPosition(scene.plunger, ok);
    REQUIRE(ok);
    CHECK(pos >= -0.02f);
    CHECK(pos <= 0.32f);
    ctrl.Stop(f.Authoring(), bridge);
    CHECK(ctrl.PhysicsTotalHandles() == 0);
}

TEST_CASE("T5 GREEN_ConstraintRemapPlays: duplicated internal refs Play against copied bodies")
{
    // Copy-path proof at the Play boundary (data-level remap is T2): two
    // root bodies duplicated together keep the internal hinge wired to the
    // copy, and the whole document Plays with two live constraints.
    T5Fixture f;
    const T5HingeScene scene = T5BuildHingeScene(f);
    const auto uuids = f.manager.ReserveKnownUuids(2);
    const auto duplicated =
        f.manager.DuplicateSubtreesWithUuids({scene.anchor, scene.flipper},
                                             uuids);
    REQUIRE(duplicated.mutation.success);

    UUID copyAnchor, copyFlipper;
    for (const auto& [source, copy] : duplicated.sourceToDuplicate)
    {
        if (source == scene.anchor) copyAnchor = copy;
        if (source == scene.flipper) copyFlipper = copy;
    }
    REQUIRE_FALSE(copyAnchor.IsNull());
    REQUIRE_FALSE(copyFlipper.IsNull());

    T5NullBridge bridge;
    T5NoopObserver obs;
    Error err;
    RuntimeSceneController ctrl;
    ctrl.SetLifecycleObserver(&obs);
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
    CHECK(ctrl.PhysicsConstraintCount() == 2);

    const SceneDocument* runtime = ctrl.TryGetRuntimeScene();
    REQUIRE(runtime != nullptr);
    const auto copyEntity = runtime->FindByUuid(copyFlipper);
    const bool copyResolvedT5 = (copyEntity != entt::null);
    REQUIRE(copyResolvedT5);
    const auto* copyHinge =
        runtime->ecs.registry.try_get<PhysicsHingeComponent>(copyEntity);
    REQUIRE(copyHinge != nullptr);
    // Internal reference points at the copy, never back at the source.
    CHECK(copyHinge->otherBody == copyAnchor);
    CHECK(copyHinge->otherBody != scene.anchor);
    const PhysicsConstraintRecord* rec = ctrl.TryGetPhysicsWorld()->FindConstraint(copyFlipper);
    REQUIRE(rec != nullptr);
    CHECK(rec->otherId == copyAnchor);
    CHECK(obs.starts == 1);
    ctrl.Stop(f.Authoring(), bridge);
    CHECK(ctrl.PhysicsTotalHandles() == 0);
}

TEST_CASE("T5 GREEN_PrefabConstraintRemapPlays: instantiated refs Play against instance bodies")
{
    // Prefab proof at the Play boundary: a single-root mechanism prefab
    // (plain rig root plus two body children) instantiates with
    // template-internal refs remapped; the instance children are then
    // unparented to roots through the production Reparent flow, and the
    // document (sources plus instance) Plays with two live constraints.
    T5Fixture f;
    const UUID rig = f.Create("Rig");
    const UUID anchor = f.Create("Anchor");
    const UUID flipper = f.Create("Flipper");
    REQUIRE(f.manager.Reparent({anchor, flipper}, rig).success);
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(anchor),
                                               T5StaticBody());
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(anchor),
                                                T5BoxShape(0.05f, 0.05f, 0.05f));
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(flipper),
                                               T5DynamicBody(1.0f));
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(flipper),
                                                T5BoxShape(0.5f, 0.06f, 0.1f));
    f.Registry().get<Transform>(f.Handle(flipper)).translation =
        {0.5f, 0.0f, 0.0f};
    f.Registry().emplace<PhysicsHingeComponent>(f.Handle(flipper),
                                                T5FlipperHinge(anchor));

    T5TempDir dir("t5_prefab_hinge");
    const auto prefabPath = dir.dir / "rig.rt2prefab";
    const auto created =
        f.manager.CreatePrefabFromSubtree({rig}, prefabPath);
    REQUIRE(created.ok);

    std::vector<AssetDiagnostic> diags;
    const auto uuids = f.manager.ReserveKnownUuids(3);
    const auto inst =
        f.manager.InstantiatePrefabWithUuids(prefabPath, uuids, diags);
    REQUIRE(inst.mutation.success);
    REQUIRE(inst.createdRoots.size() == 1);
    // Pre-order instance UUIDs: root first, then children positionally.
    const UUID instAnchor = uuids[1];
    const UUID instFlipper = uuids[2];
    const auto* instHinge = f.Registry().try_get<PhysicsHingeComponent>(
        f.manager.FindEntityByUuid(instFlipper));
    REQUIRE(instHinge != nullptr);
    CHECK(instHinge->otherBody == instAnchor);
    CHECK(instHinge->otherBody != anchor);

    // Prefab children start parented on both sides (Play refuses parented
    // bodies by design); unparent sources and instance alike to roots
    // exactly as an author would.
    REQUIRE(f.manager.Reparent({anchor, flipper, instAnchor, instFlipper},
                                std::nullopt).success);

    T5NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
    CHECK(ctrl.PhysicsConstraintCount() == 2);
    const PhysicsConstraintRecord* rec =
        ctrl.TryGetPhysicsWorld()->FindConstraint(instFlipper);
    REQUIRE(rec != nullptr);
    CHECK(rec->otherId == instAnchor);
    ctrl.Stop(f.Authoring(), bridge);
    CHECK(ctrl.PhysicsTotalHandles() == 0);
}

TEST_CASE("T5 GREEN_ExternalWorldAnchorsPlay: external and world anchors survive copy and Play")
{
    // External references stay external and empty world anchors stay empty
    // across copy paths; the document Plays with all three constraints.
    T5Fixture f;
    const UUID external = f.Create("External");
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(external),
                                               T5StaticBody());
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(external),
                                                T5BoxShape(0.5f, 0.5f, 0.5f));

    const T5HingeScene scene = T5BuildHingeScene(f);
    // Rewire the hinge at the outside body; add a world-anchored slider on
    // the flipper (joint centered on the owner at build).
    f.Registry().get<PhysicsHingeComponent>(f.Handle(scene.flipper)).otherBody =
        external;
    PhysicsSliderComponent anchored;
    anchored.otherBody = UUID::Nil();
    anchored.axis = {0.0f, 0.0f, 1.0f};
    anchored.lowerLimit = -0.1f;
    anchored.upperLimit = 0.1f;
    anchored.targetPosition = 0.0f;
    anchored.motorTargetVelocity = 1.0f;
    anchored.motorMaxForce = 10.0f;
    anchored.motorEnabled = false;
    f.Registry().emplace<PhysicsSliderComponent>(f.Handle(scene.flipper),
                                                 anchored);

    const auto uuids = f.manager.ReserveKnownUuids(2);
    const auto duplicated =
        f.manager.DuplicateSubtreesWithUuids({scene.anchor, scene.flipper},
                                             uuids);
    REQUIRE(duplicated.mutation.success);
    UUID copyFlipper;
    for (const auto& [source, copy] : duplicated.sourceToDuplicate)
    {
        if (source == scene.flipper) copyFlipper = copy;
    }
    REQUIRE_FALSE(copyFlipper.IsNull());
    const auto copyEntity = f.manager.FindEntityByUuid(copyFlipper);
    const bool copyResolvedT5 = (copyEntity != entt::null);
    REQUIRE(copyResolvedT5);
    const auto* copyHinge =
        f.Registry().try_get<PhysicsHingeComponent>(copyEntity);
    REQUIRE(copyHinge != nullptr);
    CHECK(copyHinge->otherBody == external);
    const auto* copySlider =
        f.Registry().try_get<PhysicsSliderComponent>(copyEntity);
    REQUIRE(copySlider != nullptr);
    CHECK(copySlider->otherBody.IsNull());

    T5NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
    // Original hinge + original world slider + copied hinge + copied world
    // slider: external refs stay external, world anchors stay world.
    CHECK(ctrl.PhysicsConstraintCount() == 4);
    ctrl.Stop(f.Authoring(), bridge);
    CHECK(ctrl.PhysicsTotalHandles() == 0);
}

TEST_CASE("T5 GREEN_ZeroHandlesAfterCycles: repeated Play/Stop and destroy cycles leave zero handles")
{
    T5Fixture f;
    const T5HingeScene scene = T5BuildHingeScene(f);

    T5NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    for (int cycle = 0; cycle < 3; ++cycle)
    {
        REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
        REQUIRE(ctrl.PhysicsConstraintCount() == 1);
        REQUIRE(ctrl.PhysicsBodyCount() == 2);
        for (int i = 0; i < 10; ++i)
            ctrl.Update(kFixedDt, bridge);
        // Runtime destroy of the constraint owner rides the co-destroy path:
        // constraint and body vanish together, the anchor survives.
        REQUIRE(ctrl.QueueDestroyRuntimeEntity(scene.flipper).IsOk());
        ctrl.Update(kFixedDt, bridge);
        CHECK(ctrl.PendingOperationCount() == 0);
        CHECK(ctrl.PhysicsConstraintCount() == 0);
        CHECK(ctrl.PhysicsBodyCount() == 1);
        ctrl.Stop(f.Authoring(), bridge);
        CHECK(ctrl.GetState() == SceneRunState::Edit);
        CHECK(ctrl.PhysicsTotalHandles() == 0);
        CHECK(ctrl.TryGetPhysicsWorld() == nullptr);
    }
}

TEST_CASE("T5 RED_BadConstraintUuidRefused: dangling, self, and missing-owner refs refuse Play")
{
    // Dangling otherBody names both UUIDs and leaves the world untouched.
    {
        T5Fixture f;
        const T5HingeScene scene = T5BuildHingeScene(f);
        // Fresh from the fixture provider (past the minted anchor/flipper),
        // so it can never coincide with a live body.
        const UUID dangling = f.ids.CreateV4();
        f.Registry().get<PhysicsHingeComponent>(f.Handle(scene.flipper)).otherBody =
            dangling;

        T5NullBridge bridge;
        T5NoopObserver obs;
        Error err;
        RuntimeSceneController ctrl;
        ctrl.SetLifecycleObserver(&obs);
        CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
        CHECK(err.path == scene.flipper.ToString());
        CHECK(err.detail.find(scene.flipper.ToString()) != std::string::npos);
        CHECK(err.detail.find(dangling.ToString()) != std::string::npos);
        CHECK(ctrl.GetState() == SceneRunState::Edit);
        CHECK(ctrl.TryGetRuntimeScene() == nullptr);
        CHECK(ctrl.TryGetPhysicsWorld() == nullptr);
        CHECK(ctrl.PhysicsTotalHandles() == 0);
        CHECK(bridge.Quiet());
        CHECK(obs.starts == 0);
    }
    // Self-constraint refuses with the owner named.
    {
        T5Fixture f;
        const T5HingeScene scene = T5BuildHingeScene(f);
        f.Registry().get<PhysicsHingeComponent>(f.Handle(scene.flipper)).otherBody =
            scene.flipper;

        T5NullBridge bridge;
        T5NoopObserver obs;
        Error err;
        RuntimeSceneController ctrl;
        ctrl.SetLifecycleObserver(&obs);
        CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
        T5CheckCleanRefusal(ctrl, bridge, obs, err, scene.flipper);
    }
    // Hinge on an entity with no body refuses with the owner named.
    {
        T5Fixture f;
        const UUID bare = f.Create("Bare");
        PhysicsHingeComponent hinge;
        f.Registry().emplace<PhysicsHingeComponent>(f.Handle(bare), hinge);

        T5NullBridge bridge;
        T5NoopObserver obs;
        Error err;
        RuntimeSceneController ctrl;
        ctrl.SetLifecycleObserver(&obs);
        CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
        T5CheckCleanRefusal(ctrl, bridge, obs, err, bare);
    }
}

TEST_CASE("T5 RED_InvalidConstraintRangeRefused: bad limits, modes, and targets refuse loudly")
{
    // Authoring boundary: invalid values refuse before mutation.
    {
        T5Fixture f;
        const UUID id = f.Create("Hinge");
        PhysicsHingeComponent bad;
        bad.minAngleLimit = 1.0f;
        bad.maxAngleLimit = -1.0f;
        CHECK_FALSE(f.manager.SetPhysicsHingeState(id, bad).success);
        CHECK_FALSE(f.manager.GetPhysicsHinge(id).has_value());

        bad = PhysicsHingeComponent{};
        bad.driveMode = 7;
        CHECK_FALSE(f.manager.SetPhysicsHingeState(id, bad).success);

        bad = PhysicsHingeComponent{};
        bad.ownerAxis = {0.0f, 0.0f, 0.0f};
        CHECK_FALSE(f.manager.SetPhysicsHingeState(id, bad).success);

        bad = PhysicsHingeComponent{};
        bad.otherBody = id;
        CHECK_FALSE(f.manager.SetPhysicsHingeState(id, bad).success);

        PhysicsSliderComponent badSlider;
        badSlider.lowerLimit = 1.0f;
        badSlider.upperLimit = -1.0f;
        CHECK_FALSE(f.manager.SetPhysicsSliderState(id, badSlider).success);
        CHECK_FALSE(f.manager.GetPhysicsSlider(id).has_value());

        badSlider = PhysicsSliderComponent{};
        badSlider.targetPosition = 99.0f;
        CHECK_FALSE(f.manager.SetPhysicsSliderState(id, badSlider).success);
    }
    // Play boundary (direct registry authoring bypasses the API): the
    // candidate rolls back atomically with the owner named.
    {
        T5Fixture f;
        const T5HingeScene scene = T5BuildHingeScene(f);
        f.Registry().get<PhysicsHingeComponent>(f.Handle(scene.flipper)).minAngleLimit = 2.0f;
        f.Registry().get<PhysicsHingeComponent>(f.Handle(scene.flipper)).maxAngleLimit = 1.0f;

        T5NullBridge bridge;
        T5NoopObserver obs;
        Error err;
        RuntimeSceneController ctrl;
        ctrl.SetLifecycleObserver(&obs);
        CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
        T5CheckCleanRefusal(ctrl, bridge, obs, err, scene.flipper);
    }
    {
        T5Fixture f;
        const T5SliderScene scene = T5BuildSliderScene(f, 0.3f, false);
        f.Registry().get<PhysicsSliderComponent>(f.Handle(scene.plunger)).lowerLimit = 1.0f;
        f.Registry().get<PhysicsSliderComponent>(f.Handle(scene.plunger)).upperLimit = -1.0f;

        T5NullBridge bridge;
        T5NoopObserver obs;
        Error err;
        RuntimeSceneController ctrl;
        ctrl.SetLifecycleObserver(&obs);
        CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
        T5CheckCleanRefusal(ctrl, bridge, obs, err, scene.plunger);
    }
}

TEST_CASE("T5 RED_ConstrainedDestroyBatchRejected: orphaning batch rejected whole, queue preserved")
{
    // Destroying the anchor while the flipper hinge survives would orphan
    // the constraint: the whole batch rejects, the queue is preserved, and
    // the world and pending queue are unchanged.
    T5Fixture f;
    const T5HingeScene scene = T5BuildHingeScene(f);

    T5NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
    REQUIRE(ctrl.PhysicsConstraintCount() == 1);
    REQUIRE(ctrl.PhysicsBodyCount() == 2);

    REQUIRE(ctrl.QueueDestroyRuntimeEntity(scene.anchor).IsOk());
    REQUIRE(ctrl.PendingOperationCount() == 1);

    // The standalone validator names both UUIDs before the drain runs.
    Error verr;
    CHECK_FALSE(ctrl.ValidatePendingBatch(verr));
    CHECK(verr.path == scene.flipper.ToString());
    CHECK(verr.detail.find(scene.flipper.ToString()) != std::string::npos);
    CHECK(verr.detail.find(scene.anchor.ToString()) != std::string::npos);

    ctrl.Update(kFixedDt, bridge);
    // Rejected as a whole: batch preserved, world untouched, still Playing.
    CHECK(ctrl.PendingOperationCount() == 1);
    CHECK(ctrl.PhysicsConstraintCount() == 1);
    CHECK(ctrl.PhysicsBodyCount() == 2);
    CHECK(ctrl.GetState() == SceneRunState::Playing);

    ctrl.Stop(f.Authoring(), bridge);
    CHECK(ctrl.PhysicsTotalHandles() == 0);
}

TEST_CASE("T5 GREEN_CoDestroyConstraintRidesAlong: owner co-destroy tears down atomically")
{
    // The exception to the orphan rule: when the constraint owner dies in
    // the same batch, teardown rides along — constraints first, then bodies
    // — at the same safe point with no stepped intermediate.
    T5Fixture f;
    const T5HingeScene scene = T5BuildHingeScene(f);

    T5NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));

    REQUIRE(ctrl.QueueDestroyRuntimeEntity(scene.flipper).IsOk());
    REQUIRE(ctrl.QueueDestroyRuntimeEntity(scene.anchor).IsOk());
    Error verr;
    CHECK(ctrl.ValidatePendingBatch(verr));
    CHECK(verr.IsOk());

    ctrl.Update(kFixedDt, bridge);
    CHECK(ctrl.PendingOperationCount() == 0);
    CHECK(ctrl.PhysicsConstraintCount() == 0);
    CHECK(ctrl.PhysicsBodyCount() == 0);
    CHECK(ctrl.PhysicsTotalHandles() == 0);
    CHECK(ctrl.GetState() == SceneRunState::Playing);

    ctrl.Stop(f.Authoring(), bridge);
    CHECK(ctrl.PhysicsTotalHandles() == 0);
}

TEST_CASE("T5 RED_TeardownOrderGuarded: body-first removal refused, constraints die first")
{
    T5Fixture f;
    const T5HingeScene scene = T5BuildHingeScene(f);

    T5NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
    PhysicsWorld* world = ctrl.TryGetPhysicsWorldMut();
    REQUIRE(world != nullptr);

    // Reversing the order — body before constraints — fails loudly with the
    // UUIDs named instead of dangling silently.
    Error rerr;
    CHECK_FALSE(world->RemoveRuntimeBody(scene.flipper, rerr));
    CHECK(rerr.path == scene.flipper.ToString());
    CHECK(rerr.detail.find(scene.flipper.ToString()) != std::string::npos);
    CHECK(ctrl.PhysicsConstraintCount() == 1);
    CHECK(ctrl.PhysicsBodyCount() == 2);

    // Constraints-first removal succeeds and empties the dependency index.
    world->RemoveSubtreePhysics({scene.flipper, scene.anchor});
    CHECK(ctrl.PhysicsConstraintCount() == 0);
    CHECK(ctrl.PhysicsBodyCount() == 0);
    CHECK(ctrl.PhysicsTotalHandles() == 0);

    // Shutdown order proof: constraints die before bodies. Reversing the
    // Shutdown loops turns these ordering assertions red.
    ctrl.Stop(f.Authoring(), bridge);
    CHECK(ctrl.PhysicsTotalHandles() == 0);
}

TEST_CASE("T5 GREEN_ShutdownOrderConstraintsFirst: Stop destroys constraints before bodies")
{
    T5Fixture f;
    const T5HingeScene scene = T5BuildHingeScene(f);
    (void)scene;

    T5NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));

    PhysicsWorld::ClearTeardownOrderLog();
    PhysicsWorld::SetTeardownOrderLog(true);
    ctrl.Stop(f.Authoring(), bridge);
    PhysicsWorld::SetTeardownOrderLog(false);
    const std::vector<std::string> log = PhysicsWorld::TakeTeardownOrderLog();
    PhysicsWorld::ClearTeardownOrderLog();

    REQUIRE(log.size() == 3);
    CHECK(log[0] == "constraint");
    CHECK(log[1] == "body");
    CHECK(log[2] == "body");
    CHECK(ctrl.PhysicsTotalHandles() == 0);
}

TEST_CASE("T5 GREEN_RebuildConstraintsAtomic: valid rebuild applies, invalid keeps live set")
{
    T5Fixture f;
    const T5HingeScene scene = T5BuildHingeScene(f);

    T5NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
    PhysicsWorld* world = ctrl.TryGetPhysicsWorldMut();
    REQUIRE(world != nullptr);
    SceneDocument* runtime = ctrl.TryGetRuntimeSceneMut();
    REQUIRE(runtime != nullptr);

    // Retune the live drive through the document, then rebuild atomically:
    // the new parameters stage onto the live constraint.
    runtime->ecs.registry.get<PhysicsHingeComponent>(
        runtime->FindByUuid(scene.flipper)).motorTargetVelocity = 30.0f;
    Error rerr;
    REQUIRE(world->RebuildConstraintsForBody(*runtime, scene.flipper, rerr));
    CHECK(rerr.IsOk());
    CHECK(ctrl.PhysicsConstraintCount() == 1);
    float vel = 0.0f, imp = 0.0f;
    REQUIRE(world->HingeMotorParams(scene.flipper, vel, imp));
    CHECK(vel == doctest::Approx(30.0f));
    CHECK(imp == doctest::Approx(8.0f));

    // An invalid retune refuses and the previous live set keeps stepping.
    // Fresh from the fixture provider (past every minted UUID), so it names
    // no live body.
    runtime->ecs.registry.get<PhysicsHingeComponent>(
        runtime->FindByUuid(scene.flipper)).otherBody =
        f.ids.CreateV4();
    CHECK_FALSE(world->RebuildConstraintsForBody(*runtime, scene.flipper, rerr));
    CHECK_FALSE(rerr.IsOk());
    CHECK(rerr.path == scene.flipper.ToString());
    CHECK(ctrl.PhysicsConstraintCount() == 1);
    REQUIRE(world->HingeMotorParams(scene.flipper, vel, imp));
    CHECK(vel == doctest::Approx(30.0f));
    ctrl.Stop(f.Authoring(), bridge);
    CHECK(ctrl.PhysicsTotalHandles() == 0);
}

TEST_CASE("T5 RED_PhysicsHingeSliderPrefabMemberRejected: linked members refuse hinge/slider")
{
    T5Fixture f;
    const UUID member = f.Create("Member");
    PrefabMemberComponent link;
    link.instanceId = f.ids.CreateV4();
    link.templateId = f.ids.CreateV4();
    f.Registry().emplace<PrefabMemberComponent>(f.Handle(member), link);
    const uint64_t rev0 = f.manager.AuthoringRevision();

    PhysicsHingeComponent hinge;
    PhysicsSliderComponent slider;
    CHECK_FALSE(f.manager.SetPhysicsHingeState(member, hinge).success);
    CHECK_FALSE(f.manager.SetPhysicsSliderState(member, slider).success);
    CHECK_FALSE(f.manager.GetPhysicsHinge(member).has_value());
    CHECK_FALSE(f.manager.GetPhysicsSlider(member).has_value());
    CHECK(f.manager.AuthoringRevision() == rev0);

    // The commands surface the same refusal through history without
    // recording.
    EditorCommandHistory history;
    auto hingeCmd = MakeSetPhysicsHingeCommandIfEffective(
        member, std::nullopt, hinge);
    REQUIRE(hingeCmd != nullptr);
    CHECK_FALSE(history.Execute(std::move(hingeCmd), f.manager).success);
    auto sliderCmd = MakeSetPhysicsSliderCommandIfEffective(
        member, std::nullopt, slider);
    REQUIRE(sliderCmd != nullptr);
    CHECK_FALSE(history.Execute(std::move(sliderCmd), f.manager).success);
    CHECK_FALSE(history.CanUndo());

    // Ordinary entities succeed (control).
    const UUID plain = f.Create("Plain");
    REQUIRE(f.manager.SetPhysicsHingeState(plain, hinge).success);
    REQUIRE(f.manager.SetPhysicsSliderState(plain, slider).success);
    CHECK(f.manager.GetPhysicsHinge(plain) == hinge);
    CHECK(f.manager.GetPhysicsSlider(plain) == slider);
}

TEST_CASE("T5 GREEN_PhysicsHingeSliderAuthoring: exact Undo/Redo through history")
{
    T5Fixture f;
    const UUID id = f.Create("Joint");
    EditorCommandHistory history;

    PhysicsHingeComponent hinge;
    hinge.motorTargetVelocity = 5.0f;
    PhysicsHingeComponent hingeEdit = hinge;
    hingeEdit.motorTargetVelocity = 9.0f;
    PhysicsSliderComponent slider;
    slider.targetPosition = 0.0f;
    PhysicsSliderComponent sliderEdit = slider;
    sliderEdit.motorMaxForce = 42.0f;

    // No-op suppression.
    CHECK(MakeSetPhysicsHingeCommandIfEffective(id, hinge, hinge) == nullptr);
    CHECK(MakeSetPhysicsSliderCommandIfEffective(id, slider, slider) == nullptr);
    CHECK(MakeSetPhysicsHingeCommandIfEffective(id, std::nullopt, std::nullopt) == nullptr);

    // Add hinge through history, exact undo/redo.
    auto addHinge = MakeSetPhysicsHingeCommandIfEffective(
        id, std::nullopt, hinge);
    REQUIRE(addHinge != nullptr);
    REQUIRE(history.Execute(std::move(addHinge), f.manager).success);
    CHECK(f.manager.GetPhysicsHinge(id) == hinge);
    auto editHinge = MakeSetPhysicsHingeCommandIfEffective(
        id, hinge, hingeEdit);
    REQUIRE(editHinge != nullptr);
    REQUIRE(history.Execute(std::move(editHinge), f.manager).success);
    CHECK(f.manager.GetPhysicsHinge(id) == hingeEdit);
    REQUIRE(history.Undo(f.manager).success);
    CHECK(f.manager.GetPhysicsHinge(id) == hinge);
    REQUIRE(history.Redo(f.manager).success);
    CHECK(f.manager.GetPhysicsHinge(id) == hingeEdit);

    // Add slider, remove hinge, undo the removal.
    auto addSlider = MakeSetPhysicsSliderCommandIfEffective(
        id, std::nullopt, slider);
    REQUIRE(addSlider != nullptr);
    REQUIRE(history.Execute(std::move(addSlider), f.manager).success);
    CHECK(f.manager.GetPhysicsSlider(id) == slider);
    auto removeHinge = MakeSetPhysicsHingeCommandIfEffective(
        id, hingeEdit, std::nullopt);
    REQUIRE(removeHinge != nullptr);
    REQUIRE(history.Execute(std::move(removeHinge), f.manager).success);
    CHECK_FALSE(f.manager.GetPhysicsHinge(id).has_value());
    REQUIRE(history.Undo(f.manager).success);
    CHECK(f.manager.GetPhysicsHinge(id) == hingeEdit);

    // Slider edit with exact redo.
    auto editSlider = MakeSetPhysicsSliderCommandIfEffective(
        id, slider, sliderEdit);
    REQUIRE(editSlider != nullptr);
    REQUIRE(history.Execute(std::move(editSlider), f.manager).success);
    CHECK(f.manager.GetPhysicsSlider(id) == sliderEdit);
    REQUIRE(history.Undo(f.manager).success);
    CHECK(f.manager.GetPhysicsSlider(id) == slider);
    REQUIRE(history.Redo(f.manager).success);
    CHECK(f.manager.GetPhysicsSlider(id) == sliderEdit);

    // An invalid after-state surfaces the failure without recording.
    const size_t depth = history.UndoDepthForTest();
    PhysicsHingeComponent bad = hingeEdit;
    bad.minAngleLimit = 3.0f;
    bad.maxAngleLimit = -3.0f;
    auto badCmd = MakeSetPhysicsHingeCommandIfEffective(
        id, hingeEdit, bad);
    REQUIRE(badCmd != nullptr);
    CHECK_FALSE(history.Execute(std::move(badCmd), f.manager).success);
    CHECK(history.UndoDepthForTest() == depth);
    CHECK(f.manager.GetPhysicsHinge(id) == hingeEdit);
}

TEST_CASE("T5 GREEN_InspectorHingeSliderPolicy: clean resync, dirty conflict, reset")
{
    // The T5 sides ride the exact T4 working-copy policy: clean copies
    // follow live Undo/Redo, dirty copies conflict instead of overwriting
    // restored history, and document reset drops everything.
    PhysicsInspectorWork work;
    DeterministicUuidProvider ids;
    const UUID target = ids.CreateV4();

    PhysicsHingeComponent hinge;
    PhysicsSliderComponent slider;
    work.Sync(target, std::nullopt, std::nullopt, hinge, slider);
    REQUIRE(work.hinge == hinge);
    REQUIRE(work.slider == slider);
    CHECK_FALSE(work.hingeDirty);

    // Clean copy follows live value edits (Undo/Redo underneath).
    PhysicsHingeComponent liveEdit = hinge;
    liveEdit.motorTargetVelocity = 11.0f;
    work.Sync(target, std::nullopt, std::nullopt, liveEdit, slider);
    CHECK(work.hinge == liveEdit);
    CHECK_FALSE(work.hingeConflict);

    // Dirty copy conflicts when live moves underneath; Apply stays gated
    // until Revert reseeds.
    work.hinge->motorTargetVelocity = 13.0f;
    work.hingeDirty = true;
    work.Sync(target, std::nullopt, std::nullopt, hinge, slider);
    CHECK(work.hingeConflict);
    work.RevertHinge(hinge);
    CHECK(work.hinge == hinge);
    CHECK_FALSE(work.hingeDirty);
    CHECK_FALSE(work.hingeConflict);

    // Slider side mirrors the policy.
    PhysicsSliderComponent sliderEdit = slider;
    sliderEdit.motorMaxForce = 7.0f;
    work.Sync(target, std::nullopt, std::nullopt, hinge, sliderEdit);
    CHECK(work.slider == sliderEdit);
    work.slider->motorMaxForce = 9.0f;
    work.sliderDirty = true;
    work.Sync(target, std::nullopt, std::nullopt, hinge, slider);
    CHECK(work.sliderConflict);
    work.AppliedSlider(sliderEdit);
    CHECK(work.slider == sliderEdit);
    CHECK_FALSE(work.sliderDirty);
    CHECK_FALSE(work.sliderConflict);

    // Same-UUID document replacement cannot inherit values.
    work.hingeDirty = true;
    work.Clear();
    CHECK_FALSE(work.HasTarget());
    CHECK_FALSE(work.hinge.has_value());
    CHECK_FALSE(work.slider.has_value());
    CHECK_FALSE(work.hingeDirty);
    CHECK_FALSE(work.hingeConflict);
}

TEST_CASE("T5 RED_HingeSliderAuthoringValidation: non-finite and out-of-range values refused")
{
    T5Fixture f;
    const UUID id = f.Create("Joint");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    PhysicsHingeComponent hinge;
    hinge.ownerPivot = {nan, 0.0f, 0.0f};
    CHECK_FALSE(f.manager.SetPhysicsHingeState(id, hinge).success);
    hinge = PhysicsHingeComponent{};
    hinge.motorMaxImpulse = -1.0f;
    CHECK_FALSE(f.manager.SetPhysicsHingeState(id, hinge).success);
    hinge = PhysicsHingeComponent{};
    hinge.motorTargetVelocity = inf;
    CHECK_FALSE(f.manager.SetPhysicsHingeState(id, hinge).success);
    hinge = PhysicsHingeComponent{};
    hinge.restAngle = 5.0f;
    CHECK_FALSE(f.manager.SetPhysicsHingeState(id, hinge).success);

    PhysicsSliderComponent slider;
    slider.axis = {0.0f, 0.0f, 0.0f};
    CHECK_FALSE(f.manager.SetPhysicsSliderState(id, slider).success);
    slider = PhysicsSliderComponent{};
    slider.motorTargetVelocity = -2.0f;
    CHECK_FALSE(f.manager.SetPhysicsSliderState(id, slider).success);
    slider = PhysicsSliderComponent{};
    slider.targetPosition = nan;
    CHECK_FALSE(f.manager.SetPhysicsSliderState(id, slider).success);

    // Nothing was persisted by any refusal.
    CHECK_FALSE(f.manager.GetPhysicsHinge(id).has_value());
    CHECK_FALSE(f.manager.GetPhysicsSlider(id).has_value());

    // Drive entry points refuse hostile input without mutating.
    T5Fixture g;
    const T5HingeScene scene = T5BuildHingeScene(g);
    T5NullBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    REQUIRE(ctrl.Play(g.Authoring(), bridge, err));
    PhysicsWorld* world = ctrl.TryGetPhysicsWorldMut();
    REQUIRE(world != nullptr);
    CHECK_FALSE(world->SetHingeDrive(scene.flipper, nan, 1.0f));
    CHECK_FALSE(world->SetHingeDrive(scene.flipper, 1.0f, -1.0f));
    bool ok = false;
    float vel = 0.0f, imp = 0.0f;
    REQUIRE(world->HingeMotorParams(scene.flipper, vel, imp));
    CHECK(vel == doctest::Approx(18.0f));
    CHECK(imp == doctest::Approx(8.0f));
    ctrl.Stop(g.Authoring(), bridge);
}
