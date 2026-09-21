// PhysicsLuaControlsTests — Bullet T7 bounded Lua physics controls.
//
// Permanent discriminating RED/GREEN coverage for ticket
// t7-lua-physics-controls: queued validated velocity/impulse/constraint/
// reset commands, immutable non-consuming Lua physics events, atomic
// reset-body-pose semantics, command timing/refusal/quarantine/Stop cleanup,
// and complete prefab acceptance.
//
// Production path: every Lua-surface case drives a REAL ScriptSystem +
// RuntimeCommandSink through RuntimeSceneController::Play/Update/Stop with
// scene-relative .lua files on disk (the Phase 6C harness pattern), so the
// tests observe the binding parse gates, the controller queue, the pre-step
// drain, and Bullet — never a helper in isolation. Refusal paths that cannot
// be reached from Lua (destroying-UUID drain set, unknown-UUID sink calls)
// go through the sink/controller directly with the same assertions.
//
// CPU-only by design: no Vulkan, ImGui or Walnut (same guards as T3-T6).
// No pinball gameplay, audio, runtime physics-body spawning, queries,
// arbitrary constraints, or renderer/UI scope.

#include <doctest/doctest.h>

#include "PhysicsWorld.h"
#include "RuntimeSceneController.h"
#include "RuntimeLifecycleObserver.h"
#include "IRuntimeScriptDispatch.h"
#include "ScriptSystem.h"
#include "SceneGraph.h"
#include "SceneManager.h"
#include "ECSComponents.h"
#include "ECSScene.h"
#include "ISceneRenderBridge.h"
#include "GPUSceneData.h"
#include "PhysicsComponents.h"
#include "PhysicsEvents.h"
#include "core/Error.h"
#include "core/UUID.h"

#ifdef IMGUI_VERSION
#error "T7 boundary: physics Lua-control tests must not import ImGui"
#endif

#ifdef VK_HEADER_VERSION
#error "T7 boundary: physics Lua-control tests must not import Vulkan"
#endif

#if __has_include("imgui.h")
#error "T7 boundary: imgui.h must not be reachable from physics Lua-control units"
#endif

#if __has_include("vulkan/vulkan.h")
#error "T7 boundary: vulkan headers must not be reachable from physics Lua-control units"
#endif

#if __has_include("Walnut/Application.h")
#error "T7 boundary: Walnut headers must not be reachable from physics Lua-control units"
#endif

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

using namespace rt2::core;

namespace {

class T7NullBridge final : public ISceneRenderBridge
{
public:
    void FullSync(GPUSceneData&) override      {}
    void MaterialSync(GPUSceneData&) override  {}
    void TransformSync(GPUSceneData&) override {}
    void ResetTemporalState() override         {}
    void RequestRender() override              {}
};

// Lazily created temp dir for scene-relative .lua files (the 6C pattern:
// a silently-empty temp dir would resolve every path against CWD instead
// of failing, so creation is eager and fatal on failure).
const std::filesystem::path& T7TempDir()
{
    static const std::filesystem::path dir = [] {
        auto d = std::filesystem::temp_directory_path() / "rt2_t7_lua_tests";
        std::filesystem::remove_all(d);
        std::filesystem::create_directories(d);
        return d;
    }();
    return dir;
}

std::filesystem::path T7WriteScript(const std::string& name,
                                    const std::string& source)
{
    auto path = T7TempDir() / name;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << source;
    f.close();
    return path;
}

struct T7Harness
{
    DeterministicUuidProvider uuidProv;
    T7NullBridge bridge;
    RuntimeSceneController ctrl;
    AssetResolutionContext assetContext;
    std::vector<AssetDiagnostic> assetDiagnostics;
    ScriptSystem scriptSys;
    RuntimeCommandSink sink;

    T7Harness()
        : scriptSys(uuidProv, assetContext, assetDiagnostics)
        , sink(ctrl)
    {
        ctrl.SetRuntimeUuidProvider(&uuidProv);
        ctrl.SetLifecycleObserver(&scriptSys);
        ctrl.SetScriptDispatch(&scriptSys);
        ctrl.SetInputService(nullptr);
        ctrl.SetRuntimeCommandSink(&sink);
    }

    bool Play(const SceneDocument& doc, Error& err)
    {
        assetContext.assetRoot = doc.metadata.sourcePath.parent_path();
        assetContext.database = nullptr;
        return ctrl.Play(doc, bridge, err);
    }

    bool Play(const SceneDocument& doc)
    {
        Error err;
        const bool ok = Play(doc, err);
        if (!ok)
            printf("[T7] Play refused: %s\n", err.Format().c_str());
        return ok;
    }

    void Update(float dt) { ctrl.Update(dt, bridge); }
    void Stop(const SceneDocument& doc) { ctrl.Stop(doc, bridge); }
};

// Raw SceneDocument builder for Lua-driven physics scenes (the 6C shape:
// explicit Transform/Name/Script emplaces, UUIDs assigned at the end, the
// scene file path roots script resolution at the temp dir).
struct T7DocBuilder
{
    T7Harness& h;
    SceneDocument doc;

    explicit T7DocBuilder(T7Harness& harness)
        : h(harness)
    {
        doc.metadata.sourcePath = T7TempDir() / "t7_fixture.rt2scene";
        doc.SetUuidProvider(&h.uuidProv);
    }

    entt::entity Create(const char* name)
    {
        entt::entity e = doc.ecs.registry.create();
        doc.ecs.registry.emplace<NameComponent>(e, NameComponent{name});
        Transform& tf = doc.ecs.registry.emplace<Transform>(e);
        tf.dirty = true;
        // Assign the Authoring UUID eagerly (not at Finish): constraint
        // components built below read owner/other UUIDs during scene
        // construction, and get<> on a missing EntityIdComponent is UB.
        // Creation order is UUID order (deterministic provider).
        doc.AssignNewUuid(e);
        return e;
    }

    void AttachScript(entt::entity e, const std::string& scriptName)
    {
        ScriptComponent sc;
        sc.asset.kind = AssetKind::Script;
        sc.asset.path = scriptName;
        sc.asset.sourceKey = "lua:asset=" + scriptName;
        doc.ecs.registry.emplace<ScriptComponent>(e, sc);
    }

    void Finish()
    {
        auto view = doc.ecs.registry.view<NameComponent>();
        for (auto e : view)
        {
            if (!doc.ecs.registry.all_of<EntityIdComponent>(e))
                doc.AssignNewUuid(e);
        }
    }

    UUID UuidOf(entt::entity e) const
    {
        return doc.ecs.registry.get<EntityIdComponent>(e).id;
    }
};

PhysicsBodyComponent T7StaticBody()
{
    PhysicsBodyComponent body;
    body.kind = PhysicsBodyKind::Static;
    body.mass = 0.0f;
    body.layer = PhysicsLayer::WorldStatic;
    body.mask = PhysicsLayer::Dynamic | PhysicsLayer::WorldStatic |
                PhysicsLayer::Mechanism;
    return body;
}

PhysicsBodyComponent T7DynamicBody(float mass = 1.0f)
{
    PhysicsBodyComponent body;
    body.kind = PhysicsBodyKind::Dynamic;
    body.mass = mass;
    body.layer = PhysicsLayer::Dynamic;
    body.mask = PhysicsLayer::WorldStatic | PhysicsLayer::Mechanism;
    return body;
}

PhysicsBodyComponent T7KinematicBody()
{
    PhysicsBodyComponent body;
    body.kind = PhysicsBodyKind::Kinematic;
    body.mass = 0.0f;
    body.layer = PhysicsLayer::Dynamic;
    body.mask = PhysicsLayer::WorldStatic | PhysicsLayer::Mechanism;
    return body;
}

PhysicsBodyComponent T7GhostBody()
{
    PhysicsBodyComponent body;
    body.kind = PhysicsBodyKind::Static;
    body.mass = 0.0f;
    body.layer = PhysicsLayer::Trigger;
    body.mask = PhysicsLayer::Dynamic;
    return body;
}

PhysicsShapeComponent T7BoxShape(float hx, float hy, float hz)
{
    PhysicsShapeComponent shape;
    shape.shape = PhysicsShapeKind::Box;
    shape.halfExtents = {hx, hy, hz};
    return shape;
}

PhysicsShapeComponent T7SphereShape(float radius = 0.2f)
{
    PhysicsShapeComponent shape;
    shape.shape = PhysicsShapeKind::Sphere;
    shape.radius = radius;
    return shape;
}

PhysicsShapeComponent T7GhostSlab(float hx, float hy, float hz)
{
    PhysicsShapeComponent shape;
    shape.shape = PhysicsShapeKind::Box;
    shape.halfExtents = {hx, hy, hz};
    shape.isTrigger = true;
    return shape;
}

void T7EmplaceBody(T7DocBuilder& b, entt::entity e,
                   const PhysicsBodyComponent& body,
                   const PhysicsShapeComponent& shape)
{
    b.doc.ecs.registry.emplace<PhysicsBodyComponent>(e, body);
    b.doc.ecs.registry.emplace<PhysicsShapeComponent>(e, shape);
}

// ---- Runtime read-back (through the runtime clone, never authoring) ------

const SceneDocument* T7Runtime(T7Harness& h)
{
    return h.ctrl.TryGetRuntimeScene();
}

glm::vec3 T7RuntimePos(T7Harness& h, const UUID& uuid)
{
    const SceneDocument* rt = T7Runtime(h);
    if (!rt)
        return glm::vec3{0.0f, 0.0f, 0.0f};
    const auto e = rt->FindByUuid(uuid);
    if (e == entt::null)
        return glm::vec3{0.0f, 0.0f, 0.0f};
    const auto* tf = rt->ecs.registry.try_get<Transform>(e);
    return tf ? tf->translation : glm::vec3{0.0f, 0.0f, 0.0f};
}

std::string T7RuntimeName(T7Harness& h, const UUID& uuid)
{
    const SceneDocument* rt = T7Runtime(h);
    if (!rt)
        return {};
    const auto e = rt->FindByUuid(uuid);
    if (e == entt::null)
        return {};
    const auto* nc = rt->ecs.registry.try_get<NameComponent>(e);
    return nc ? nc->name : std::string{};
}

size_t T7RuntimeEntityCount(T7Harness& h)
{
    const SceneDocument* rt = T7Runtime(h);
    if (!rt)
        return 0;
    size_t count = 0;
    auto view = rt->ecs.registry.view<EntityIdComponent>();
    for (auto e : view)
    {
        (void)e;
        ++count;
    }
    return count;
}

// C++ probe dispatch for the destroying-UUID refusal case (mirrors the T6
// probe: OnEntitiesDestroying runs mid-drain while ECS + Bullet are still
// present, so sink calls there must refuse loudly without mutation).
struct T7DestroyProbeDispatch final : public IRuntimeScriptDispatch
{
    RuntimeSceneController* ctrl = nullptr;
    RuntimeCommandSink* sink = nullptr;
    UUID dying;
    bool velRefused = false;
    bool impulseRefused = false;
    bool resetRefused = false;
    bool hingeRefused = false;
    bool eventsEmpty = true;

    void OnFixedUpdate(float) override {}
    void OnUpdate(float) override {}
    void SyncScriptEnvironments() override {}
    void OnEntitiesDestroying(const std::vector<UUID>&) override
    {
        if (!sink)
            return;
        glm::vec3 v{1.0f, 0.0f, 0.0f};
        velRefused = !sink->SetLinearVelocity(dying, v);
        impulseRefused = !sink->ApplyImpulse(dying, v);
        PhysicsPoseReset reset;
        reset.position = glm::vec3{0.0f, 5.0f, 0.0f};
        resetRefused = !sink->ResetBodyPose(dying, reset);
        hingeRefused = !sink->SetHingeDrive(dying, 1.0f, 1.0f);
        if (ctrl && !ctrl->PhysicsEvents().empty())
            eventsEmpty = false;
    }
};

} // namespace

// ============================================================================
// Timing + round trip: queued writes land at the pre-step boundary
// ============================================================================

TEST_CASE("T7 GREEN_QueuedCommandsDrainAtPreStep: sink writes queue without touching Bullet until the next tick")
{
    T7Harness h;
    T7DocBuilder b(h);
    entt::entity ball = b.Create("Ball");
    T7EmplaceBody(b, ball, T7DynamicBody(1.0f), T7SphereShape(0.2f));
    b.doc.ecs.registry.get<Transform>(ball).translation = {0.0f, 5.0f, 0.0f};
    b.Finish();
    const UUID ballId = b.UuidOf(ball);
    REQUIRE(h.Play(b.doc));

    // Queue-time: the command waits, Bullet still reports the old state.
    CHECK(h.ctrl.QueuedPhysicsCommandCount() == 0);
    REQUIRE(h.sink.SetLinearVelocity(ballId, glm::vec3{3.0f, 0.0f, 0.0f}));
    CHECK(h.ctrl.QueuedPhysicsCommandCount() == 1);
    glm::vec3 stillOld{0.0f, 0.0f, 0.0f};
    REQUIRE(h.sink.GetLinearVelocity(ballId, stillOld));
    CHECK(stillOld.x == doctest::Approx(0.0f));
    CHECK(T7RuntimePos(h, ballId).x == doctest::Approx(0.0f));

    // Exactly one fixed tick: drain applies, the step integrates, the queue
    // is empty again. A second queued command from the same phase would
    // apply in FIFO order; one command proves the boundary.
    h.Update(kFixedDt);
    CHECK(h.ctrl.QueuedPhysicsCommandCount() == 0);
    glm::vec3 applied{0.0f, 0.0f, 0.0f};
    REQUIRE(h.sink.GetLinearVelocity(ballId, applied));
    CHECK(applied.x == doctest::Approx(3.0f));
    CHECK(T7RuntimePos(h, ballId).x == doctest::Approx(3.0f * kFixedDt).epsilon(0.05));
    h.Stop(b.doc);
}

TEST_CASE("T7 GREEN_FixedUpdateVelocitySameTick: Lua set from OnFixedUpdate moves the same tick's step")
{
    T7WriteScript("t7_fixed_vel.lua",
        "function on_fixed_update(entity, dt, input, world)\n"
        "  entity:set_velocity({2.0, 0.0, 0.0})\n"
        "end\n");
    T7Harness h;
    T7DocBuilder b(h);
    entt::entity ball = b.Create("Ball");
    T7EmplaceBody(b, ball, T7DynamicBody(1.0f), T7SphereShape(0.2f));
    b.doc.ecs.registry.get<Transform>(ball).translation = {0.0f, 5.0f, 0.0f};
    b.AttachScript(ball, "t7_fixed_vel.lua");
    b.Finish();
    const UUID ballId = b.UuidOf(ball);
    REQUIRE(h.Play(b.doc));

    h.Update(kFixedDt);
    // Same-tick proof: one Update runs exactly one fixed tick, and the ball
    // already carries the scripted x-velocity's full-tick displacement. An
    // inline-immediate write would also pass this; the queued-not-inline
    // half is proved by GREEN_QueuedCommandsDrainAtPreStep above.
    CHECK(T7RuntimePos(h, ballId).x ==
          doctest::Approx(2.0f * kFixedDt).epsilon(0.05));
    glm::vec3 v{0.0f, 0.0f, 0.0f};
    REQUIRE(h.sink.GetLinearVelocity(ballId, v));
    CHECK(v.x == doctest::Approx(2.0f));
    h.Stop(b.doc);
}

TEST_CASE("T7 GREEN_VelocityWriteReadRoundTrip: Lua set from OnUpdate reads back the next frame")
{
    T7WriteScript("t7_vel_roundtrip.lua",
        "local armed = true\n"
        "function on_update(entity, dt, input, world)\n"
        "  if armed then\n"
        "    armed = false\n"
        "    entity:set_velocity({3.0, 0.0, 0.0})\n"
        "  end\n"
        "  local v = entity:get_velocity()\n"
        "  if v ~= nil then\n"
        "    entity:set_name(string.format(\"v:%.3f\", v[1]))\n"
        "  else\n"
        "    entity:set_name(\"v:nil\")\n"
        "  end\n"
        "end\n");
    T7Harness h;
    T7DocBuilder b(h);
    entt::entity ball = b.Create("Ball");
    T7EmplaceBody(b, ball, T7DynamicBody(1.0f), T7SphereShape(0.2f));
    b.doc.ecs.registry.get<Transform>(ball).translation = {0.0f, 5.0f, 0.0f};
    b.AttachScript(ball, "t7_vel_roundtrip.lua");
    b.Finish();
    const UUID ballId = b.UuidOf(ball);
    REQUIRE(h.Play(b.doc));

    // Frame 0: the write queues during OnUpdate (after this frame's steps),
    // so the read still reports pre-write state.
    h.Update(kFixedDt);
    CHECK(T7RuntimeName(h, ballId) == "v:0.000");
    // Frame 1: the pre-step drain applied it before the first tick, so the
    // read reports the commanded velocity. Two frames bound the documented
    // at-most-one-tick latency for OnUpdate-issued commands.
    h.Update(kFixedDt);
    CHECK(T7RuntimeName(h, ballId) == "v:3.000");
    h.Stop(b.doc);
}

TEST_CASE("T7 GREEN_ApplyImpulseChangesVelocity: central impulse obeys J equals m times dv")
{
    T7WriteScript("t7_impulse.lua",
        "local armed = true\n"
        "function on_update(entity, dt, input, world)\n"
        "  if armed then\n"
        "    armed = false\n"
        "    entity:apply_impulse({4.0, 0.0, 0.0})\n"
        "  end\n"
        "  local v = entity:get_velocity()\n"
        "  if v ~= nil then\n"
        "    entity:set_name(string.format(\"v:%.3f\", v[1]))\n"
        "  else\n"
        "    entity:set_name(\"v:nil\")\n"
        "  end\n"
        "end\n");
    T7Harness h;
    T7DocBuilder b(h);
    entt::entity ball = b.Create("Ball");
    T7EmplaceBody(b, ball, T7DynamicBody(2.0f), T7SphereShape(0.2f));
    b.doc.ecs.registry.get<Transform>(ball).translation = {0.0f, 5.0f, 0.0f};
    b.AttachScript(ball, "t7_impulse.lua");
    b.Finish();
    const UUID ballId = b.UuidOf(ball);
    REQUIRE(h.Play(b.doc));

    h.Update(kFixedDt);
    CHECK(T7RuntimeName(h, ballId) == "v:0.000");
    h.Update(kFixedDt);
    // J = 4 on m = 2 gives dv = 2. Gravity acts on y only; x is exact.
    CHECK(T7RuntimeName(h, ballId) == "v:2.000");
    h.Stop(b.doc);
}

// ============================================================================
// Constraints through Lua: hinge drive/release, slider target/release
// ============================================================================

TEST_CASE("T7 GREEN_HingeDriveReleaseAtTickBoundary: Lua drive moves the arm within one tick and release parks the motor")
{
    T7WriteScript("t7_hinge.lua",
        "local phase = 0\n"
        "function on_update(entity, dt, input, world)\n"
        "  if phase == 0 then\n"
        "    entity:set_hinge_drive({velocity = 6.0, impulse = 8.0})\n"
        "    phase = 1\n"
        "  end\n"
        "  local w = entity:get_angular_velocity()\n"
        "  if w ~= nil then\n"
        "    entity:set_name(string.format(\"w:%.3f\", w[2]))\n"
        "  else\n"
        "    entity:set_name(\"w:nil\")\n"
        "  end\n"
        "end\n");
    T7Harness h;
    T7DocBuilder b(h);
    // Anchor static box at the origin; flipper dynamic box at +0.5 X with a
    // Y-axis hinge (gravity does no work about it — the T5 flipper shape).
    entt::entity anchor = b.Create("Anchor");
    T7EmplaceBody(b, anchor, T7StaticBody(), T7BoxShape(0.05f, 0.05f, 0.05f));
    entt::entity flipper = b.Create("Flipper");
    T7EmplaceBody(b, flipper, T7DynamicBody(1.0f), T7BoxShape(0.5f, 0.06f, 0.1f));
    b.doc.ecs.registry.get<Transform>(flipper).translation = {0.5f, 0.0f, 0.0f};
    PhysicsHingeComponent hinge;
    hinge.otherBody = b.UuidOf(anchor);
    hinge.ownerPivot = {-0.5f, 0.0f, 0.0f};
    hinge.ownerAxis = {0.0f, 1.0f, 0.0f};
    hinge.otherPivot = {0.0f, 0.0f, 0.0f};
    hinge.otherAxis = {0.0f, 1.0f, 0.0f};
    hinge.minAngleLimit = 0.0f;
    hinge.maxAngleLimit = 0.96f;
    hinge.motorTargetVelocity = 18.0f;
    hinge.motorMaxImpulse = 8.0f;
    hinge.motorEnabled = false;
    hinge.restAngle = 0.0f;
    b.doc.ecs.registry.emplace<PhysicsHingeComponent>(flipper, hinge);
    b.AttachScript(flipper, "t7_hinge.lua");
    b.Finish();
    const UUID flipperId = b.UuidOf(flipper);
    REQUIRE(h.Play(b.doc));

    // Boundary proof: the drive queues in frame 0's OnUpdate and is already
    // moving the arm after frame 1's single tick — no later than one tick.
    h.Update(kFixedDt);
    bool angleOk0 = false;
    const float angle0 =
        h.ctrl.TryGetPhysicsWorldMut()->HingeAngle(flipperId, angleOk0);
    REQUIRE(angleOk0);
    h.Update(kFixedDt);
    bool angleOk1 = false;
    const float angle1 =
        h.ctrl.TryGetPhysicsWorldMut()->HingeAngle(flipperId, angleOk1);
    REQUIRE(angleOk1);
    CHECK(std::fabs(angle1) > 1e-4f);
    CHECK(angle1 != doctest::Approx(angle0));
    // The staged motor params match the Lua drive table exactly.
    bool motorOk = false;
    CHECK(h.ctrl.TryGetPhysicsWorldMut()->HingeMotorEnabled(flipperId, motorOk));
    CHECK(motorOk);
    float velOut = 0.0f, impOut = 0.0f;
    REQUIRE(h.ctrl.TryGetPhysicsWorldMut()->HingeMotorParams(
        flipperId, velOut, impOut));
    CHECK(velOut == doctest::Approx(6.0f));
    CHECK(impOut == doctest::Approx(8.0f));
    // The spinning arm reports nonzero angular velocity through Lua.
    const std::string wName = T7RuntimeName(h, flipperId);
    CHECK(wName.rfind("w:", 0) == 0);
    CHECK(wName != "w:0.000");

    // Release parks the motor (queued release drains before the next step).
    REQUIRE(h.sink.ReleaseHingeDrive(flipperId));
    h.Update(kFixedDt);
    bool motorOk2 = false;
    CHECK_FALSE(
        h.ctrl.TryGetPhysicsWorldMut()->HingeMotorEnabled(flipperId, motorOk2));
    CHECK(motorOk2);
    h.Stop(b.doc);
}

TEST_CASE("T7 GREEN_SliderTargetReleaseAtTickBoundary: Lua target walks the plunger and release cuts the motor")
{
    T7WriteScript("t7_slider.lua",
        "local armed = true\n"
        "function on_update(entity, dt, input, world)\n"
        "  if armed then\n"
        "    armed = false\n"
        "    entity:set_slider_target(0.25)\n"
        "  end\n"
        "end\n");
    T7Harness h;
    T7DocBuilder b(h);
    entt::entity ground = b.Create("Ground");
    T7EmplaceBody(b, ground, T7StaticBody(), T7BoxShape(5.0f, 0.5f, 5.0f));
    b.doc.ecs.registry.get<Transform>(ground).translation = {0.0f, -0.62f, 0.0f};
    entt::entity anchor = b.Create("Anchor");
    T7EmplaceBody(b, anchor, T7StaticBody(), T7BoxShape(0.05f, 0.05f, 0.05f));
    b.doc.ecs.registry.get<Transform>(anchor).translation = {0.0f, 0.0f, 0.5f};
    entt::entity plunger = b.Create("Plunger");
    PhysicsBodyComponent plungerBody = T7DynamicBody(2.0f);
    plungerBody.mask = plungerBody.mask | PhysicsLayer::Dynamic;
    T7EmplaceBody(b, plunger, plungerBody, T7BoxShape(0.12f, 0.12f, 0.12f));
    PhysicsSliderComponent slider;
    slider.otherBody = b.UuidOf(anchor);
    slider.axis = {1.0f, 0.0f, 0.0f};
    slider.lowerLimit = 0.0f;
    slider.upperLimit = 0.3f;
    slider.targetPosition = 0.0f;
    slider.motorTargetVelocity = 8.0f;
    slider.motorMaxForce = 80.0f;
    slider.motorEnabled = false;
    b.doc.ecs.registry.emplace<PhysicsSliderComponent>(plunger, slider);
    b.AttachScript(plunger, "t7_slider.lua");
    b.Finish();
    const UUID plungerId = b.UuidOf(plunger);
    REQUIRE(h.Play(b.doc));

    // The Lua target (0.25, inside [0, 0.3]) walks the plunger off zero and
    // toward the setpoint within the tick budget.
    for (int i = 0; i < 120; ++i)
        h.Update(kFixedDt);
    bool sliderOk = false;
    const float pos =
        h.ctrl.TryGetPhysicsWorldMut()->SliderPosition(plungerId, sliderOk);
    REQUIRE(sliderOk);
    CHECK(pos > 0.05f);
    CHECK(pos <= doctest::Approx(0.25f).epsilon(0.2));

    // Release cuts the motor and kicks along the axis; the call succeeds
    // and the position stays readable and bounded by the limits.
    REQUIRE(h.sink.ReleaseSlider(plungerId, 1.0f));
    h.Update(kFixedDt);
    bool sliderOk2 = false;
    const float pos2 =
        h.ctrl.TryGetPhysicsWorldMut()->SliderPosition(plungerId, sliderOk2);
    REQUIRE(sliderOk2);
    CHECK(pos2 >= -0.05f);
    CHECK(pos2 <= 0.35f);
    h.Stop(b.doc);
}

// ============================================================================
// Events through Lua: identical immutable polls, OnUpdate-only validity
// ============================================================================

TEST_CASE("T7 GREEN_PhysicsEventsTwoLuaScriptsIdentical: two OnUpdate polls receive identical ordered events")
{
    T7WriteScript("t7_events_a.lua",
        "function on_update(entity, dt, input, world)\n"
        "  local ev = world:physics_events()\n"
        "  local parts = {}\n"
        "  for i, e in ipairs(ev) do\n"
        "    parts[#parts + 1] = e.kind .. \":\" .. e.bodyA .. \":\" .. e.bodyB .. \":\" .. e.tick\n"
        "  end\n"
        "  entity:set_name(\"ev:\" .. #ev .. \":\" .. table.concat(parts, \"|\"))\n"
        "end\n");
    T7WriteScript("t7_events_b.lua",
        "function on_update(entity, dt, input, world)\n"
        "  local ev = world:physics_events()\n"
        "  local parts = {}\n"
        "  for i, e in ipairs(ev) do\n"
        "    parts[#parts + 1] = e.kind .. \":\" .. e.bodyA .. \":\" .. e.bodyB .. \":\" .. e.tick\n"
        "  end\n"
        "  entity:set_name(\"ev:\" .. #ev .. \":\" .. table.concat(parts, \"|\"))\n"
        "end\n");
    T7Harness h;
    T7DocBuilder b(h);
    // Penetrating rest pair: every tick scrapes a real Contact, so both
    // polls always have something to agree on.
    entt::entity ground = b.Create("Ground");
    T7EmplaceBody(b, ground, T7StaticBody(), T7BoxShape(5.0f, 0.5f, 5.0f));
    b.doc.ecs.registry.get<Transform>(ground).translation = {0.0f, -0.5f, 0.0f};
    entt::entity box = b.Create("Box");
    T7EmplaceBody(b, box, T7DynamicBody(1.0f), T7BoxShape(0.25f, 0.25f, 0.25f));
    b.doc.ecs.registry.get<Transform>(box).translation = {0.0f, 0.24f, 0.0f};
    entt::entity obsA = b.Create("ObsA");
    b.AttachScript(obsA, "t7_events_a.lua");
    entt::entity obsB = b.Create("ObsB");
    b.AttachScript(obsB, "t7_events_b.lua");
    b.Finish();
    const UUID aId = b.UuidOf(obsA);
    const UUID bId = b.UuidOf(obsB);
    REQUIRE(h.Play(b.doc));

    for (int i = 0; i < 3; ++i)
    {
        h.Update(kFixedDt);
        const std::string aName = T7RuntimeName(h, aId);
        const std::string bName = T7RuntimeName(h, bId);
        // Identical ordered events for both consumers (the "ev:N:" prefixes
        // match, so the counts match too), and the payload carries Contact.
        CHECK(aName == bName);
        CHECK(aName.rfind("ev:", 0) == 0);
        CHECK(aName.find("contact:") != std::string::npos);
    }
    h.Stop(b.doc);
}

TEST_CASE("T7 GREEN_PhysicsEventsPollValidOnlyInOnUpdate: Lua sees empty pre-publication and data in OnUpdate")
{
    T7WriteScript("t7_events_phase.lua",
        "local fixedSeen = -1\n"
        "function on_fixed_update(entity, dt, input, world)\n"
        "  fixedSeen = #world:physics_events()\n"
        "end\n"
        "function on_update(entity, dt, input, world)\n"
        "  entity:set_name(\"fixed:\" .. fixedSeen .. \":update:\" .. #world:physics_events())\n"
        "end\n");
    T7Harness h;
    T7DocBuilder b(h);
    entt::entity ground = b.Create("Ground");
    T7EmplaceBody(b, ground, T7StaticBody(), T7BoxShape(5.0f, 0.5f, 5.0f));
    b.doc.ecs.registry.get<Transform>(ground).translation = {0.0f, -0.5f, 0.0f};
    entt::entity box = b.Create("Box");
    T7EmplaceBody(b, box, T7DynamicBody(1.0f), T7BoxShape(0.25f, 0.25f, 0.25f));
    b.doc.ecs.registry.get<Transform>(box).translation = {0.0f, 0.24f, 0.0f};
    b.AttachScript(box, "t7_events_phase.lua");
    b.Finish();
    const UUID boxId = b.UuidOf(box);
    REQUIRE(h.Play(b.doc));

    h.Update(kFixedDt);
    // The fixed callback polled before publication (empty); OnUpdate polled
    // the published Contact. A phase leak in either direction renames this.
    const std::string name = T7RuntimeName(h, boxId);
    CHECK(name.rfind("fixed:0:update:", 0) == 0);
    const int updateCount =
        std::stoi(name.substr(std::string("fixed:0:update:").size()));
    CHECK(updateCount >= 1);
    h.Stop(b.doc);
}

// ============================================================================
// Reset: the ticket acceptance (trigger -> reset -> impulse, body reused)
// ============================================================================

TEST_CASE("T7 GREEN_ResetBodyPoseReusesBody: trigger event, reset, impulse with no new entity and clean overlap state")
{
    T7WriteScript("t7_reset.lua",
        "local resetDone = false\n"
        "local impulseDone = false\n"
        "local enters = 0\n"
        "local earlyExit = false\n"
        "function on_update(entity, dt, input, world)\n"
        "  local ev = world:physics_events()\n"
        "  for i, e in ipairs(ev) do\n"
        "    if e.kind == \"trigger_enter\" then enters = enters + 1 end\n"
        "    if e.kind == \"trigger_exit\" and enters < 2 then earlyExit = true end\n"
        "  end\n"
        "  if not resetDone and enters >= 1 then\n"
        "    if entity:reset_body_pose({0.0, 5.0, 0.0}, {0.0, 0.0, 0.0, 1.0}) then\n"
        "      resetDone = true\n"
        "    end\n"
        "  elseif resetDone and not impulseDone then\n"
        "    if entity:apply_impulse({2.0, 0.0, 0.0}) then impulseDone = true end\n"
        "  end\n"
        "  entity:set_name(\"reset:\" .. enters .. \":\" .. (earlyExit and \"early\" or \"clean\"))\n"
        "end\n");
    T7Harness h;
    T7DocBuilder b(h);
    entt::entity ghost = b.Create("Ghost");
    T7EmplaceBody(b, ghost, T7GhostBody(), T7GhostSlab(2.0f, 0.25f, 2.0f));
    b.doc.ecs.registry.get<Transform>(ghost).translation = {0.0f, 1.0f, 0.0f};
    entt::entity ball = b.Create("Ball");
    PhysicsBodyComponent ballBody = T7DynamicBody(1.0f);
    ballBody.mask = ballBody.mask | PhysicsLayer::Trigger;
    T7EmplaceBody(b, ball, ballBody, T7SphereShape(0.2f));
    b.doc.ecs.registry.get<Transform>(ball).translation = {0.0f, 2.5f, 0.0f};
    b.AttachScript(ball, "t7_reset.lua");
    b.Finish();
    const UUID ballId = b.UuidOf(ball);
    REQUIRE(h.Play(b.doc));

    const size_t bodies0 = h.ctrl.PhysicsBodyCount();
    const size_t ghosts0 = h.ctrl.PhysicsGhostCount();
    const size_t handles0 = h.ctrl.PhysicsTotalHandles();
    const size_t entities0 = T7RuntimeEntityCount(h);
    REQUIRE(bodies0 == 1);
    REQUIRE(ghosts0 == 1);

    // Wait for the first Enter + reset (the fall takes ~30 ticks).
    std::string name;
    int resetFrame = -1;
    for (int i = 0; i < 120; ++i)
    {
        h.Update(kFixedDt);
        name = T7RuntimeName(h, ballId);
        if (name.rfind("reset:1:", 0) == 0 ||
            name.rfind("reset:2:", 0) == 0)
        {
            resetFrame = i;
            break;
        }
    }
    REQUIRE(resetFrame >= 0);

    // The reset queued in frame N applies at frame N+1's pre-step: run that
    // frame, then read the teleported pose (one tick of fall ≈ 1.4mm). The
    // same body moved — entity/resource counts below prove no growth and no
    // partially-applied state.
    h.Update(kFixedDt);
    const glm::vec3 p = T7RuntimePos(h, ballId);
    CHECK(p.x == doctest::Approx(0.0f).epsilon(0.05));
    CHECK(p.y > 4.5f);
    CHECK(p.y <= 5.0f);
    CHECK(p.z == doctest::Approx(0.0f).epsilon(0.05));
    CHECK(h.ctrl.PhysicsBodyCount() == bodies0);
    CHECK(h.ctrl.PhysicsGhostCount() == ghosts0);
    CHECK(h.ctrl.PhysicsTotalHandles() == handles0);
    CHECK(T7RuntimeEntityCount(h) == entities0);

    // Reusability: the body falls back through the trigger (a SECOND Enter
    // — the purge left no stale overlap behind) and the post-reset impulse
    // took effect (nonzero x-velocity). No TriggerExit may appear before
    // that second Enter: the teleport must not fabricate one.
    for (int i = 0; i < 240 && name != "reset:2:clean"; ++i)
    {
        h.Update(kFixedDt);
        name = T7RuntimeName(h, ballId);
        if (name.find(":early") != std::string::npos)
            break;
    }
    CHECK(name == "reset:2:clean");
    glm::vec3 v{0.0f, 0.0f, 0.0f};
    REQUIRE(h.sink.GetLinearVelocity(ballId, v));
    CHECK(std::fabs(v.x) > 0.5f);
    CHECK(h.ctrl.PhysicsBodyCount() == bodies0);
    CHECK(h.ctrl.PhysicsGhostCount() == ghosts0);
    CHECK(T7RuntimeEntityCount(h) == entities0);
    h.Stop(b.doc);
}

TEST_CASE("T7 GREEN_ResetKinematicPoseAndMotion: kinematic reset lands exactly and authored motion stays green")
{
    T7WriteScript("t7_kinematic.lua",
        "local armed = true\n"
        "function on_update(entity, dt, input, world)\n"
        "  if armed then\n"
        "    armed = false\n"
        "    local resetOk = entity:reset_body_pose({1.0, 2.0, 3.0}, {0.0, 0.0, 0.0, 1.0})\n"
        "    local setOk = entity:set_position({7.0, 7.0, 7.0})\n"
        "    if resetOk and setOk then entity:set_name(\"kin:green\") else entity:set_name(\"kin:red\") end\n"
        "  end\n"
        "end\n");
    T7Harness h;
    T7DocBuilder b(h);
    entt::entity plat = b.Create("Platform");
    T7EmplaceBody(b, plat, T7KinematicBody(), T7BoxShape(0.5f, 0.1f, 0.5f));
    b.doc.ecs.registry.get<Transform>(plat).translation = {0.0f, 0.0f, 0.0f};
    b.AttachScript(plat, "t7_kinematic.lua");
    b.Finish();
    const UUID platId = b.UuidOf(plat);
    REQUIRE(h.Play(b.doc));

    h.Update(kFixedDt);
    h.Update(kFixedDt);
    // Authored kinematic motion stays green (reset queued true, set true).
    CHECK(T7RuntimeName(h, platId) == "kin:green");
    // Queue-vs-inline ordering, documented: the inline set_position wrote
    // (7,7,7) during frame 0's OnUpdate, but the queued reset applied later
    // at frame 1's pre-step — so the reset wins and the final pose is the
    // reset target, not the authored write. Neither fought; both returned
    // true; the order is deterministic.
    CHECK(T7RuntimePos(h, platId).x == doctest::Approx(1.0f));

    // Sink-level exactness: a lone reset with no competing write lands
    // exactly and reports the same pose back through Bullet.
    PhysicsPoseReset reset;
    reset.position = glm::vec3{1.0f, 2.0f, 3.0f};
    REQUIRE(h.sink.ResetBodyPose(platId, reset));
    h.Update(kFixedDt);
    const glm::vec3 p = T7RuntimePos(h, platId);
    CHECK(p.x == doctest::Approx(1.0f));
    CHECK(p.y == doctest::Approx(2.0f));
    CHECK(p.z == doctest::Approx(3.0f));

    // And authored motion still works after the reset: an inline kinematic
    // set lands exactly (the reset did not wedge the authority path).
    REQUIRE(h.sink.SetPosition(platId, glm::vec3{7.0f, 7.0f, 7.0f}));
    h.Update(kFixedDt);
    CHECK(T7RuntimePos(h, platId).x == doctest::Approx(7.0f));
    h.Stop(b.doc);
}

// ============================================================================
// Cleanup: reload, quarantine, and Stop leave no queued commands
// ============================================================================

TEST_CASE("T7 GREEN_ReloadClearsQueuedCommands: a reload drops unapplied physics work")
{
    T7WriteScript("t7_reload.lua",
        "function on_update(entity, dt, input, world)\n"
        "end\n");
    T7Harness h;
    T7DocBuilder b(h);
    entt::entity ball = b.Create("Ball");
    T7EmplaceBody(b, ball, T7DynamicBody(1.0f), T7SphereShape(0.2f));
    b.doc.ecs.registry.get<Transform>(ball).translation = {0.0f, 5.0f, 0.0f};
    b.AttachScript(ball, "t7_reload.lua");
    b.Finish();
    const UUID ballId = b.UuidOf(ball);
    REQUIRE(h.Play(b.doc));

    // Queue between frames (no drain can intervene), then reload the very
    // script that would have owned the next tick's motion.
    REQUIRE(h.sink.SetLinearVelocity(ballId, glm::vec3{5.0f, 0.0f, 0.0f}));
    CHECK(h.ctrl.QueuedPhysicsCommandCount() == 1);
    h.scriptSys.ReloadScript(T7TempDir() / "t7_reload.lua");
    CHECK(h.ctrl.QueuedPhysicsCommandCount() == 0);

    // The dropped command never moves the body in x (gravity acts on y).
    h.Update(kFixedDt);
    h.Update(kFixedDt);
    CHECK(T7RuntimePos(h, ballId).x == doctest::Approx(0.0f));
    CHECK(h.ctrl.QueuedPhysicsCommandCount() == 0);
    h.Stop(b.doc);
}

TEST_CASE("T7 GREEN_QuarantineClearsQueuedCommands: a failing script cannot keep moving bodies")
{
    T7WriteScript("t7_quar_queuer.lua",
        "local armed = true\n"
        "function on_update(entity, dt, input, world)\n"
        "  if armed then\n"
        "    armed = false\n"
        "    entity:set_velocity({5.0, 0.0, 0.0})\n"
        "  end\n"
        "end\n");
    T7WriteScript("t7_quar_fail.lua",
        "function on_update(entity, dt, input, world)\n"
        "  error(\"t7 quarantine boom\")\n"
        "end\n");
    T7Harness h;
    T7DocBuilder b(h);
    // Creation order is UUID order (deterministic provider): the queuer
    // runs first and enqueues, the failing script quarantines second and
    // the quarantine clear drops the queuer's still-pending command.
    entt::entity queuer = b.Create("Queuer");
    T7EmplaceBody(b, queuer, T7DynamicBody(1.0f), T7SphereShape(0.2f));
    b.doc.ecs.registry.get<Transform>(queuer).translation = {0.0f, 5.0f, 0.0f};
    b.AttachScript(queuer, "t7_quar_queuer.lua");
    entt::entity failer = b.Create("Failer");
    b.AttachScript(failer, "t7_quar_fail.lua");
    b.Finish();
    const UUID queuerId = b.UuidOf(queuer);
    const UUID failerId = b.UuidOf(failer);
    REQUIRE(queuerId < failerId);
    REQUIRE(h.Play(b.doc));

    h.Update(kFixedDt);
    // The failure quarantined exactly one instance and the queue is empty:
    // the queuer's frame-0 command never reached the pre-step drain.
    CHECK(h.scriptSys.QuarantinedInstanceCount() == 1);
    CHECK(h.scriptSys.GetInstanceState(failerId) ==
          ScriptInstanceState::Quarantined);
    CHECK(h.ctrl.QueuedPhysicsCommandCount() == 0);
    h.Update(kFixedDt);
    CHECK(T7RuntimePos(h, queuerId).x == doctest::Approx(0.0f));
    glm::vec3 v{0.0f, 0.0f, 0.0f};
    REQUIRE(h.sink.GetLinearVelocity(queuerId, v));
    CHECK(std::fabs(v.x) < 0.05f);
    h.Stop(b.doc);
}

TEST_CASE("T7 GREEN_StopClearsQueuedCommands: re-Play inherits no stale physics work")
{
    T7Harness h;
    T7DocBuilder b(h);
    entt::entity ball = b.Create("Ball");
    T7EmplaceBody(b, ball, T7DynamicBody(1.0f), T7SphereShape(0.2f));
    b.doc.ecs.registry.get<Transform>(ball).translation = {0.0f, 5.0f, 0.0f};
    b.Finish();
    const UUID ballId = b.UuidOf(ball);
    REQUIRE(h.Play(b.doc));

    REQUIRE(h.sink.SetLinearVelocity(ballId, glm::vec3{5.0f, 0.0f, 0.0f}));
    CHECK(h.ctrl.QueuedPhysicsCommandCount() == 1);
    h.Stop(b.doc);
    CHECK(h.ctrl.QueuedPhysicsCommandCount() == 0);

    // Re-Play starts clean: no phantom velocity, no phantom queue.
    REQUIRE(h.Play(b.doc));
    CHECK(h.ctrl.QueuedPhysicsCommandCount() == 0);
    glm::vec3 v{9.0f, 9.0f, 9.0f};
    REQUIRE(h.sink.GetLinearVelocity(ballId, v));
    CHECK(std::fabs(v.x) < 0.05f);
    h.Update(kFixedDt);
    CHECK(T7RuntimePos(h, ballId).x == doctest::Approx(0.0f));
    h.Stop(b.doc);
}

// ============================================================================
// Scope guards: no widened spawn, no widened surface, Dynamic pose refusal
// ============================================================================

TEST_CASE("T7 GREEN_SpawnCarriesNoPhysicsBody: runtime spawn cannot mint simulated bodies")
{
    T7WriteScript("t7_spawn.lua",
        "local spawned = false\n"
        "function on_update(entity, dt, input, world)\n"
        "  if not spawned then\n"
        "    spawned = true\n"
        "    world:spawn({name = \"T7Child\"})\n"
        "  end\n"
        "end\n");
    T7Harness h;
    T7DocBuilder b(h);
    entt::entity spawner = b.Create("Spawner");
    b.AttachScript(spawner, "t7_spawn.lua");
    b.Finish();
    REQUIRE(h.Play(b.doc));

    h.Update(kFixedDt);
    h.Update(kFixedDt);
    const UUID child = h.sink.FindByName("T7Child");
    REQUIRE_FALSE(child.IsNull());
    // The spawned entity is inert: no body component, and every physics
    // op refuses (nil reads, false writes) without touching the world.
    const SceneDocument* rt = T7Runtime(h);
    REQUIRE(rt != nullptr);
    const entt::entity childEntity = rt->FindByUuid(child);
    REQUIRE(rt->ecs.registry.valid(childEntity));
    CHECK(rt->ecs.registry.try_get<PhysicsBodyComponent>(childEntity) ==
          nullptr);
    CHECK(rt->ecs.registry.try_get<PhysicsShapeComponent>(childEntity) ==
          nullptr);
    glm::vec3 v{0.0f, 0.0f, 0.0f};
    CHECK_FALSE(h.sink.GetLinearVelocity(child, v));
    CHECK_FALSE(h.sink.SetLinearVelocity(child, v));
    CHECK_FALSE(h.sink.ApplyImpulse(child, v));
    PhysicsPoseReset reset;
    CHECK_FALSE(h.sink.ResetBodyPose(child, reset));
    CHECK_FALSE(h.sink.SetHingeDrive(child, 1.0f, 1.0f));
    CHECK(h.ctrl.PhysicsTotalHandles() == 0);
    h.Stop(b.doc);
}

TEST_CASE("T7 RED_DynamicSetPositionRefusedWhileResetGreen: pose authority stays with the solver except through reset")
{
    T7WriteScript("t7_pose_auth.lua",
        "function on_update(entity, dt, input, world)\n"
        "  local setOk = entity:set_position({9.0, 9.0, 9.0})\n"
        "  local badReset = entity:reset_body_pose({0/0, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0})\n"
        "  local resetOk = entity:reset_body_pose({1.0, 2.0, 3.0}, {0.0, 0.0, 0.0, 1.0})\n"
        "  if (not setOk) and (not badReset) and resetOk then\n"
        "    entity:set_name(\"pose:correct\")\n"
        "  else\n"
        "    entity:set_name(\"pose:wrong\")\n"
        "  end\n"
        "end\n");
    T7Harness h;
    T7DocBuilder b(h);
    entt::entity ball = b.Create("Ball");
    T7EmplaceBody(b, ball, T7DynamicBody(1.0f), T7SphereShape(0.2f));
    b.doc.ecs.registry.get<Transform>(ball).translation = {0.0f, 5.0f, 0.0f};
    b.AttachScript(ball, "t7_pose_auth.lua");
    b.Finish();
    const UUID ballId = b.UuidOf(ball);
    REQUIRE(h.Play(b.doc));

    h.Update(kFixedDt);
    // Direct Dynamic set_position stays red, the NaN reset stays red, and
    // the well-formed reset queues green — all in one frame.
    CHECK(T7RuntimeName(h, ballId) == "pose:correct");
    // The refused set_position never moved the body; the queued reset lands
    // at the next pre-step (far from both the spawn and the refused target).
    h.Update(kFixedDt);
    const glm::vec3 p = T7RuntimePos(h, ballId);
    CHECK(p.x == doctest::Approx(1.0f).epsilon(0.05));
    CHECK(p.y > 1.5f);
    CHECK(p.y < 3.5f);
    h.Stop(b.doc);
}

TEST_CASE("T7 RED_ResetRefusalsMutationFree: static, missing, destroying, and malformed resets change nothing")
{
    T7Harness h;
    T7DocBuilder b(h);
    entt::entity statik = b.Create("Static");
    T7EmplaceBody(b, statik, T7StaticBody(), T7BoxShape(1.0f, 1.0f, 1.0f));
    entt::entity ball = b.Create("Ball");
    T7EmplaceBody(b, ball, T7DynamicBody(1.0f), T7SphereShape(0.2f));
    b.doc.ecs.registry.get<Transform>(ball).translation = {0.0f, 5.0f, 0.0f};
    b.Finish();
    const UUID staticId = b.UuidOf(statik);
    const UUID ballId = b.UuidOf(ball);
    const UUID missing = h.uuidProv.CreateV4();
    REQUIRE(h.Play(b.doc));

    const glm::vec3 ballPos0 = T7RuntimePos(h, ballId);
    glm::vec3 ballVel0{0.0f, 0.0f, 0.0f};
    REQUIRE(h.sink.GetLinearVelocity(ballId, ballVel0));
    const size_t handles0 = h.ctrl.PhysicsTotalHandles();
    PhysicsWorld* world = h.ctrl.TryGetPhysicsWorldMut();
    REQUIRE(world != nullptr);
    const size_t overlaps0 = world->PrevOverlapCount();

    PhysicsPoseReset good;
    good.position = glm::vec3{1.0f, 2.0f, 3.0f};
    // Static refuses (baked at Play).
    CHECK_FALSE(h.sink.ResetBodyPose(staticId, good));
    // Missing body refuses.
    CHECK_FALSE(h.sink.ResetBodyPose(missing, good));
    // Malformed pose shapes refuse: NaN position, degenerate (zero)
    // rotation, infinite velocity, oversized position.
    PhysicsPoseReset nanPos = good;
    nanPos.position.x = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(h.sink.ResetBodyPose(ballId, nanPos));
    PhysicsPoseReset zeroRot = good;
    zeroRot.rotation = glm::quat{0.0f, 0.0f, 0.0f, 0.0f};
    CHECK_FALSE(h.sink.ResetBodyPose(ballId, zeroRot));
    PhysicsPoseReset infVel = good;
    infVel.hasLinearVelocity = true;
    infVel.linearVelocity =
        glm::vec3{std::numeric_limits<float>::infinity(), 0.0f, 0.0f};
    CHECK_FALSE(h.sink.ResetBodyPose(ballId, infVel));
    PhysicsPoseReset hugePos = good;
    hugePos.position.x = std::numeric_limits<float>::infinity();
    CHECK_FALSE(h.sink.ResetBodyPose(ballId, hugePos));

    // Nothing enqueued, nothing mutated: pose, velocities, handles, and
    // overlap history are all exactly as before.
    CHECK(h.ctrl.QueuedPhysicsCommandCount() == 0);
    const glm::vec3 ballPos1 = T7RuntimePos(h, ballId);
    CHECK(ballPos1.x == doctest::Approx(ballPos0.x));
    CHECK(ballPos1.y == doctest::Approx(ballPos0.y));
    CHECK(ballPos1.z == doctest::Approx(ballPos0.z));
    glm::vec3 ballVel1{9.0f, 9.0f, 9.0f};
    REQUIRE(h.sink.GetLinearVelocity(ballId, ballVel1));
    CHECK(ballVel1.x == doctest::Approx(ballVel0.x));
    CHECK(h.ctrl.PhysicsTotalHandles() == handles0);
    CHECK(world->PrevOverlapCount() == overlaps0);
    h.Stop(b.doc);
}

TEST_CASE("T7 RED_InvalidOpsLoudMutationFree: wrong UUID, kind, and arguments refuse without enqueueing")
{
    T7Harness h;
    T7DocBuilder b(h);
    entt::entity statik = b.Create("Static");
    T7EmplaceBody(b, statik, T7StaticBody(), T7BoxShape(1.0f, 1.0f, 1.0f));
    entt::entity ball = b.Create("Ball");
    T7EmplaceBody(b, ball, T7DynamicBody(1.0f), T7SphereShape(0.2f));
    b.doc.ecs.registry.get<Transform>(ball).translation = {0.0f, 5.0f, 0.0f};
    entt::entity plat = b.Create("Platform");
    T7EmplaceBody(b, plat, T7KinematicBody(), T7BoxShape(0.5f, 0.1f, 0.5f));
    entt::entity ghost = b.Create("Ghost");
    T7EmplaceBody(b, ghost, T7GhostBody(), T7GhostSlab(2.0f, 0.25f, 2.0f));
    b.doc.ecs.registry.get<Transform>(ghost).translation = {0.0f, 1.0f, 0.0f};
    entt::entity plain = b.Create("Plain");
    b.Finish();
    const UUID staticId = b.UuidOf(statik);
    const UUID ballId = b.UuidOf(ball);
    const UUID platId = b.UuidOf(plat);
    const UUID ghostId = b.UuidOf(ghost);
    const UUID plainId = b.UuidOf(plain);
    const UUID missing = h.uuidProv.CreateV4();
    REQUIRE(h.Play(b.doc));

    const glm::vec3 ballPos0 = T7RuntimePos(h, ballId);
    const size_t bodies0 = h.ctrl.PhysicsBodyCount();
    const size_t handles0 = h.ctrl.PhysicsTotalHandles();
    const glm::vec3 v{1.0f, 2.0f, 3.0f};
    glm::vec3 out{0.0f, 0.0f, 0.0f};

    // Unknown UUID: every op refuses (reads false, writes false).
    CHECK_FALSE(h.sink.GetLinearVelocity(missing, out));
    CHECK_FALSE(h.sink.GetAngularVelocity(missing, out));
    CHECK_FALSE(h.sink.SetLinearVelocity(missing, v));
    CHECK_FALSE(h.sink.ApplyImpulse(missing, v));
    CHECK_FALSE(h.sink.SetHingeDrive(missing, 1.0f, 1.0f));
    CHECK_FALSE(h.sink.ReleaseHingeDrive(missing));
    CHECK_FALSE(h.sink.SetSliderTarget(missing, 0.1f));
    CHECK_FALSE(h.sink.ReleaseSlider(missing, 1.0f));
    PhysicsPoseReset reset;
    CHECK_FALSE(h.sink.ResetBodyPose(missing, reset));
    // No body component at all (plain entity): reads and writes refuse.
    CHECK_FALSE(h.sink.GetLinearVelocity(plainId, out));
    CHECK_FALSE(h.sink.SetLinearVelocity(plainId, v));
    CHECK_FALSE(h.sink.ApplyImpulse(plainId, v));
    CHECK_FALSE(h.sink.ResetBodyPose(plainId, reset));
    CHECK_FALSE(h.sink.SetHingeDrive(plainId, 1.0f, 1.0f));
    CHECK_FALSE(h.sink.SetSliderTarget(plainId, 0.1f));
    // Wrong kind: Static refuses velocity/impulse/reset; Kinematic refuses
    // impulse (infinite mass); ghost-only refuses velocity reads/writes.
    CHECK_FALSE(h.sink.SetLinearVelocity(staticId, v));
    CHECK_FALSE(h.sink.ApplyImpulse(staticId, v));
    CHECK_FALSE(h.sink.ResetBodyPose(staticId, reset));
    CHECK_FALSE(h.sink.ApplyImpulse(platId, v));
    CHECK_FALSE(h.sink.GetLinearVelocity(ghostId, out));
    CHECK_FALSE(h.sink.GetAngularVelocity(ghostId, out));
    CHECK_FALSE(h.sink.SetLinearVelocity(ghostId, v));
    // Wrong constraint kind: hinge ops on a body without one, slider ops
    // on a body without one (and vice versa once constraints exist —
    // covered by the hinge/slider GREEN cases above).
    CHECK_FALSE(h.sink.SetHingeDrive(ballId, 1.0f, 1.0f));
    CHECK_FALSE(h.sink.ReleaseHingeDrive(ballId));
    CHECK_FALSE(h.sink.SetSliderTarget(ballId, 0.1f));
    CHECK_FALSE(h.sink.ReleaseSlider(ballId, 1.0f));
    // Bad arguments: non-finite velocity/impulse/drive/target never queue.
    const glm::vec3 nan{std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f};
    CHECK_FALSE(h.sink.SetLinearVelocity(ballId, nan));
    CHECK_FALSE(h.sink.ApplyImpulse(ballId, nan));
    CHECK_FALSE(h.sink.SetHingeDrive(ballId, nan.x, 1.0f));
    CHECK_FALSE(h.sink.SetSliderTarget(ballId, nan.x));
    CHECK_FALSE(h.sink.ReleaseSlider(ballId, nan.x));
    CHECK_FALSE(h.sink.SetHingeDrive(ballId, 1.0f, -1.0f));

    // Deterministic and mutation-free: nothing enqueued, the world untouched.
    CHECK(h.ctrl.QueuedPhysicsCommandCount() == 0);
    const glm::vec3 ballPos1 = T7RuntimePos(h, ballId);
    CHECK(ballPos1.x == doctest::Approx(ballPos0.x));
    CHECK(ballPos1.y == doctest::Approx(ballPos0.y));
    CHECK(ballPos1.z == doctest::Approx(ballPos0.z));
    CHECK(h.ctrl.PhysicsBodyCount() == bodies0);
    CHECK(h.ctrl.PhysicsTotalHandles() == handles0);
    h.Stop(b.doc);
}

TEST_CASE("T7 RED_DestroyingUuidPhysicsRefused: drain-time physics writes refuse like the T6 transform gate")
{
    T7Harness h;
    T7DocBuilder b(h);
    entt::entity ball = b.Create("Ball");
    T7EmplaceBody(b, ball, T7DynamicBody(1.0f), T7SphereShape(0.2f));
    b.doc.ecs.registry.get<Transform>(ball).translation = {0.0f, 5.0f, 0.0f};
    b.Finish();
    const UUID ballId = b.UuidOf(ball);
    // The probe replaces the script dispatch (no Lua runs here): its
    // OnEntitiesDestroying fires mid-drain while ECS + Bullet are live.
    T7DestroyProbeDispatch probe;
    RuntimeCommandSink probeSink(h.ctrl);
    probe.ctrl = &h.ctrl;
    probe.sink = &probeSink;
    probe.dying = ballId;
    h.ctrl.SetScriptDispatch(&probe);
    // ScriptSystem stays the lifecycle observer (harmless without scripts).
    REQUIRE(h.Play(b.doc));

    const glm::vec3 pos0 = T7RuntimePos(h, ballId);
    REQUIRE(h.ctrl.QueueDestroyRuntimeEntity(ballId).IsOk());
    h.Update(kFixedDt);

    // All four physics writes refused inside the drain; events stayed empty
    // for the destroy callback (the T6 pre-publication contract holds while
    // the new gates refuse).
    CHECK(probe.velRefused);
    CHECK(probe.impulseRefused);
    CHECK(probe.resetRefused);
    CHECK(probe.hingeRefused);
    CHECK(probe.eventsEmpty);
    CHECK(h.ctrl.QueuedPhysicsCommandCount() == 0);
    // The destroy itself still completed (refusal did not wedge the drain).
    CHECK(h.ctrl.PhysicsBodyCount() == 0);
    h.Stop(b.doc);
    h.ctrl.SetScriptDispatch(&h.scriptSys);
}

TEST_CASE("T7 RED_NoWidenedLuaSurface: queries, arbitrary constraints, and teleport stay out of Lua")
{
    T7WriteScript("t7_surface.lua",
        "function on_update(entity, dt, input, world)\n"
        "  local clean = (world.raycast == nil) and (world.sweep == nil)\n"
        "    and (world.shapecast == nil) and (world.overlap == nil)\n"
        "    and (world.create_body == nil) and (world.add_constraint == nil)\n"
        "    and (world.remove_constraint == nil) and (world.set_gravity == nil)\n"
        "    and (entity.set_mass == nil) and (entity.add_force == nil)\n"
        "    and (entity.add_torque == nil) and (entity.teleport == nil)\n"
        "    and (entity.set_angular_velocity == nil) and (entity.set_gravity_scale == nil)\n"
        "  if clean then entity:set_name(\"surface:clean\") else entity:set_name(\"surface:wide\") end\n"
        "end\n");
    T7Harness h;
    T7DocBuilder b(h);
    entt::entity ball = b.Create("Ball");
    T7EmplaceBody(b, ball, T7DynamicBody(1.0f), T7SphereShape(0.2f));
    b.doc.ecs.registry.get<Transform>(ball).translation = {0.0f, 5.0f, 0.0f};
    b.AttachScript(ball, "t7_surface.lua");
    b.Finish();
    const UUID ballId = b.UuidOf(ball);
    REQUIRE(h.Play(b.doc));

    h.Update(kFixedDt);
    // If a future change adds any of these bindings, this names the widening
    // instead of silently extending the T7 surface.
    CHECK(T7RuntimeName(h, ballId) == "surface:clean");
    h.Stop(b.doc);
}

// ============================================================================
// Prefab acceptance: sources carry physics, members refuse attach/edit/remove
// ============================================================================

TEST_CASE("T7 GREEN_PrefabPhysicsComplete: sources carry physics, members refuse attach, edit, and remove")
{
    // SceneManager fixture (authoring-side, no Play): prefab sources may
    // contain all four physics component types.
    DeterministicUuidProvider ids;
    SceneManager manager;
    manager.SetUuidProvider(&ids);
    auto create = [&](const char* name) {
        return manager.CreateEmpty(name).affectedEntities.front();
    };

    const UUID root = create("Rig");
    auto createChild = [&](const char* name, const UUID& parent) {
        return manager.CreateEmpty(name, parent).affectedEntities.front();
    };
    const UUID bodyA = createChild("BodyA", root);
    const UUID bodyB = createChild("BodyB", root);
    const UUID bodyC = createChild("BodyC", root);
    auto& reg = manager.GetECS().registry;
    PhysicsBodyComponent srcBody = T7DynamicBody(1.0f);
    PhysicsShapeComponent srcShape = T7BoxShape(0.25f, 0.25f, 0.25f);
    reg.emplace<PhysicsBodyComponent>(manager.FindEntityByUuid(bodyA), srcBody);
    reg.emplace<PhysicsShapeComponent>(manager.FindEntityByUuid(bodyA), srcShape);
    reg.emplace<PhysicsBodyComponent>(manager.FindEntityByUuid(bodyB),
                                     T7DynamicBody(1.0f));
    PhysicsHingeComponent srcHinge;
    srcHinge.otherBody = bodyA;
    srcHinge.ownerPivot = {-0.5f, 0.0f, 0.0f};
    srcHinge.ownerAxis = {0.0f, 1.0f, 0.0f};
    srcHinge.otherPivot = {0.0f, 0.0f, 0.0f};
    srcHinge.otherAxis = {0.0f, 1.0f, 0.0f};
    srcHinge.minAngleLimit = 0.0f;
    srcHinge.maxAngleLimit = 0.5f;
    reg.emplace<PhysicsHingeComponent>(manager.FindEntityByUuid(bodyB), srcHinge);
    reg.emplace<PhysicsBodyComponent>(manager.FindEntityByUuid(bodyC),
                                     T7DynamicBody(1.0f));
    PhysicsSliderComponent srcSlider;
    srcSlider.otherBody = UUID::Nil();
    srcSlider.axis = {1.0f, 0.0f, 0.0f};
    srcSlider.lowerLimit = 0.0f;
    srcSlider.upperLimit = 0.3f;
    reg.emplace<PhysicsSliderComponent>(manager.FindEntityByUuid(bodyC),
                                       srcSlider);

    const auto dir = T7TempDir() / "t7_prefab_physics";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto prefabPath = dir / "rig.rt2prefab";
    const auto created = manager.CreatePrefabFromSubtree({root}, prefabPath);
    REQUIRE(created.ok);

    // Instantiate: all four types copy verbatim with template-internal refs
    // remapped to the instance (never silently dropped, never stale).
    std::vector<AssetDiagnostic> diags;
    const auto uuids = manager.ReserveKnownUuids(4);
    const auto inst = manager.InstantiatePrefabWithUuids(prefabPath, uuids, diags);
    REQUIRE(inst.mutation.success);
    const UUID instA = uuids[1];
    const UUID instB = uuids[2];
    const UUID instC = uuids[3];
    const auto instBody = manager.GetPhysicsBody(instA);
    REQUIRE(instBody.has_value());
    CHECK(*instBody == srcBody);
    const auto instShape = manager.GetPhysicsShape(instA);
    REQUIRE(instShape.has_value());
    CHECK(*instShape == srcShape);
    const auto instHinge = manager.GetPhysicsHinge(instB);
    REQUIRE(instHinge.has_value());
    CHECK(instHinge->otherBody == instA);
    CHECK(instHinge->otherBody != bodyA);
    const auto instSlider = manager.GetPhysicsSlider(instC);
    REQUIRE(instSlider.has_value());
    CHECK(instSlider->otherBody.IsNull());

    // Link the instance subtree as prefab members, then prove the full
    // attach/edit/remove matrix refuses for all four types — with no
    // mutation and no revision bump. Attach targets a member carrying no
    // such component; edit/remove target members carrying one. Instantiate
    // already links instance entities, so (re)link defensively.
    const UUID bareMember = create("BareMember");
    reg.emplace<PrefabMemberComponent>(manager.FindEntityByUuid(bareMember),
                                      PrefabMemberComponent{});
    for (const UUID member : {instA, instB, instC})
    {
        reg.emplace_or_replace<PrefabMemberComponent>(
            manager.FindEntityByUuid(member), PrefabMemberComponent{});
    }
    const uint64_t rev0 = manager.AuthoringRevision();

    // Body: attach (bare), edit + remove (instA).
    CHECK_FALSE(manager.SetPhysicsBodyState(bareMember, srcBody).success);
    PhysicsBodyComponent otherBody = T7StaticBody();
    CHECK_FALSE(manager.SetPhysicsBodyState(instA, otherBody).success);
    CHECK_FALSE(manager.SetPhysicsBodyState(instA, std::nullopt).success);
    // Shape: attach (bare), edit + remove (instA).
    CHECK_FALSE(manager.SetPhysicsShapeState(bareMember, srcShape).success);
    PhysicsShapeComponent otherShape = T7SphereShape(0.1f);
    CHECK_FALSE(manager.SetPhysicsShapeState(instA, otherShape).success);
    CHECK_FALSE(manager.SetPhysicsShapeState(instA, std::nullopt).success);
    // Hinge: attach (bare), edit + remove (instB).
    CHECK_FALSE(manager.SetPhysicsHingeState(bareMember, srcHinge).success);
    PhysicsHingeComponent otherHinge = srcHinge;
    otherHinge.maxAngleLimit = 0.9f;
    CHECK_FALSE(manager.SetPhysicsHingeState(instB, otherHinge).success);
    CHECK_FALSE(manager.SetPhysicsHingeState(instB, std::nullopt).success);
    // Slider: attach (bare), edit + remove (instC).
    CHECK_FALSE(manager.SetPhysicsSliderState(bareMember, srcSlider).success);
    PhysicsSliderComponent otherSlider = srcSlider;
    otherSlider.upperLimit = 0.2f;
    CHECK_FALSE(manager.SetPhysicsSliderState(instC, otherSlider).success);
    CHECK_FALSE(manager.SetPhysicsSliderState(instC, std::nullopt).success);

    // Values and revision untouched by all twelve refusals.
    CHECK(manager.GetPhysicsBody(instA).has_value());
    CHECK(*manager.GetPhysicsBody(instA) == srcBody);
    CHECK(manager.GetPhysicsShape(instA).has_value());
    CHECK(*manager.GetPhysicsShape(instA) == srcShape);
    CHECK(manager.GetPhysicsHinge(instB).has_value());
    CHECK(manager.GetPhysicsHinge(instB)->otherBody == instA);
    CHECK(manager.GetPhysicsSlider(instC).has_value());
    CHECK_FALSE(manager.GetPhysicsBody(bareMember).has_value());
    CHECK_FALSE(manager.GetPhysicsShape(bareMember).has_value());
    CHECK_FALSE(manager.GetPhysicsHinge(bareMember).has_value());
    CHECK_FALSE(manager.GetPhysicsSlider(bareMember).has_value());
    CHECK(manager.AuthoringRevision() == rev0);

    // Control: the prefab SOURCE stays editable (refusal is member-only).
    CHECK(manager.SetPhysicsBodyState(bodyA, otherBody).success);
    CHECK(*manager.GetPhysicsBody(bodyA) == otherBody);

    std::filesystem::remove_all(dir);
}