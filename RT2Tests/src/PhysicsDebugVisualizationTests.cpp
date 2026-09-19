// ============================================================================
// PhysicsDebugVisualizationTests — Bullet T8 physics debug visualization.
//
// Permanent coverage for ticket t8-physics-debug-visualization: CPU-only
// PhysicsDebugLines DTO captured through a minimal btIDebugDraw, immutable
// Play/Paused controller exposure, stable UUID/segment order with a
// deterministic headless dump, top/side projection through the existing
// EditorViewportIcons CPU seam, Pause-retains/Stop-clears lifecycle, and
// invalid geometry keeping the T4 Play diagnostic (never an empty shape
// that looks successful).
//
// Constraint note (T5 independence): hinge/slider Bullet constraints are
// built on another branch and are not duplicated here. Constraint-debug
// segments come from the AppendConstraintAdapterLines adapter over the
// already-persisted hinge/slider components (T2 data, T3 validation); the
// final T5 merge point is documented in PhysicsDebugCapture.h. Tests use
// synthetic persisted hinge/slider fixtures through that adapter.
//
// CPU-only by design: no Vulkan, ImGui or Walnut (same guards as T3/T4).
// Drawing lives in the RT2App-only PhysicsDebugOverlay TU, which this
// project never links; projection here uses only ProjectToViewport.
// ISceneRenderBridge carries no debug geometry.
// ============================================================================

#include <doctest/doctest.h>
#include "EditorViewportIcons.h"
#include "PhysicsDebugCapture.h"
#include "PhysicsDebugLines.h"
#include "PhysicsWorld.h"
#include "RuntimeSceneController.h"
#include "RuntimeLifecycleObserver.h"
#include "SceneGraph.h"
#include "SceneManager.h"
#include "ISceneRenderBridge.h"
#include "GPUSceneData.h"
#include "core/Error.h"
#include "core/UUID.h"

#ifdef IMGUI_VERSION
#error "T8 boundary: physics debug tests must not import ImGui"
#endif

#ifdef VK_HEADER_VERSION
#error "T8 boundary: physics debug tests must not import Vulkan"
#endif

#if __has_include("imgui.h")
#error "T8 boundary: imgui.h must not be reachable from physics debug units"
#endif

#if __has_include("vulkan/vulkan.h")
#error "T8 boundary: vulkan headers must not be reachable from physics debug units"
#endif

#if __has_include("Walnut/Application.h")
#error "T8 boundary: Walnut headers must not be reachable from physics debug units"
#endif

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>
#include <string>

using namespace rt2::core;

namespace {

class T8NullBridge final : public ISceneRenderBridge
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

class T8NoopObserver final : public IRuntimeLifecycleObserver
{
public:
    void OnSceneStart(const SceneDocument&) override {}
    void OnSceneStop(const SceneDocument&) override {}
};

constexpr float kImageW = 800.0f;
constexpr float kImageH = 600.0f;
const glm::vec2 kImageMin{ 100.0f, 50.0f };
const glm::vec2 kImageSize{ kImageW, kImageH };

// Top-down orthographic-ish perspective: eye above the origin looking down
// -Y. Up must not parallel the view direction, so use -Z as up.
glm::mat4 T8TopViewProj()
{
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 10.0f, 0.0f),
        glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    glm::mat4 proj = glm::perspectiveFov(glm::radians(45.0f),
        kImageW, kImageH, 0.1f, 100.0f);
    proj[1][1] *= -1.0f; // Vulkan y-flip, mirroring the icon test seam
    return proj * view;
}

// Side view: eye on +X looking at the dynamic body's height.
glm::mat4 T8SideViewProj()
{
    const glm::mat4 view = glm::lookAt(glm::vec3(10.0f, 1.0f, 0.0f),
        glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 proj = glm::perspectiveFov(glm::radians(45.0f),
        kImageW, kImageH, 0.1f, 100.0f);
    proj[1][1] *= -1.0f;
    return proj * view;
}

PhysicsBodyComponent T8StaticBody()
{
    PhysicsBodyComponent body;
    body.kind = PhysicsBodyKind::Static;
    body.mass = 0.0f;
    body.layer = PhysicsLayer::WorldStatic;
    body.mask = PhysicsLayer::Dynamic | PhysicsLayer::WorldStatic |
                PhysicsLayer::Mechanism;
    return body;
}

PhysicsBodyComponent T8DynamicBody()
{
    PhysicsBodyComponent body;
    body.kind = PhysicsBodyKind::Dynamic;
    body.mass = 1.0f;
    body.layer = PhysicsLayer::Dynamic;
    body.mask = PhysicsLayer::WorldStatic | PhysicsLayer::Mechanism;
    return body;
}

PhysicsBodyComponent T8KinematicBody()
{
    PhysicsBodyComponent body;
    body.kind = PhysicsBodyKind::Kinematic;
    body.mass = 0.0f;
    body.layer = PhysicsLayer::Mechanism;
    body.mask = PhysicsLayer::Dynamic | PhysicsLayer::WorldStatic |
                PhysicsLayer::Mechanism;
    return body;
}

PhysicsShapeComponent T8BoxShape(float half)
{
    PhysicsShapeComponent shape;
    shape.shape = PhysicsShapeKind::Box;
    shape.halfExtents = { half, half, half };
    return shape;
}

PhysicsShapeComponent T8SphereShape(float radius)
{
    PhysicsShapeComponent shape;
    shape.shape = PhysicsShapeKind::Sphere;
    shape.radius = radius;
    return shape;
}

struct T8Fixture
{
    DeterministicUuidProvider ids;
    SceneManager manager;
    RuntimeSceneController ctrl;
    T8NullBridge bridge;
    T8NoopObserver observer;

    // Owned UUIDs for owner-identity assertions.
    UUID ground, ball, platform, trigger, hingeOwner;

    T8Fixture() { manager.SetUuidProvider(&ids); }

    UUID AddBox(const char* name, const glm::vec3& pos,
                const PhysicsBodyComponent& body,
                const PhysicsShapeComponent& shape)
    {
        const UUID uuid = manager.CreateEmpty(name).affectedEntities.front();
        auto e = manager.FindEntityByUuid(uuid);
        manager.GetECS().registry.emplace<PhysicsBodyComponent>(e, body);
        manager.GetECS().registry.emplace<PhysicsShapeComponent>(e, shape);
        auto& tf = manager.GetECS().registry.get<Transform>(e);
        tf.translation = pos;
        SceneGraph::MarkDirty(manager.GetECS().registry, e);
        return uuid;
    }

    // Synthetic T8 playfield: static ground, dynamic ball, kinematic
    // platform, static trigger volume, plus persisted hinge (ball ->
    // platform) and slider (platform -> world) fixtures for the constraint
    // adapter. No hull/triMesh refs, so no provider is needed.
    void BuildPlayfield()
    {
        ground = AddBox("Ground", { 0.0f, -1.0f, 0.0f },
            T8StaticBody(), T8BoxShape(2.0f));

        {
            const UUID uuid = manager.CreateEmpty("Ball").affectedEntities.front();
            auto e = manager.FindEntityByUuid(uuid);
            manager.GetECS().registry.emplace<PhysicsBodyComponent>(e, T8DynamicBody());
            manager.GetECS().registry.emplace<PhysicsShapeComponent>(e, T8SphereShape(0.5f));
            auto& tf = manager.GetECS().registry.get<Transform>(e);
            tf.translation = { 0.0f, 1.0f, 0.0f };
            SceneGraph::MarkDirty(manager.GetECS().registry, e);
            ball = uuid;
            hingeOwner = uuid;
        }

        platform = AddBox("Platform", { 2.0f, 0.5f, 0.0f },
            T8KinematicBody(), T8BoxShape(0.5f));

        {
            PhysicsBodyComponent triggerBody = T8StaticBody();
            triggerBody.layer = PhysicsLayer::Trigger;
            triggerBody.mask = PhysicsLayer::Dynamic;
            PhysicsShapeComponent triggerShape = T8BoxShape(0.5f);
            triggerShape.isTrigger = true;
            trigger = AddBox("Trigger", { -2.0f, 0.5f, 0.0f },
                triggerBody, triggerShape);
        }

        // Persisted hinge: owner ball, otherBody platform (both live bodies).
        {
            auto e = manager.FindEntityByUuid(ball);
            PhysicsHingeComponent hinge;
            hinge.otherBody = platform;
            hinge.ownerPivot = { 0.0f, 0.0f, 0.0f };
            hinge.ownerAxis = { 0.0f, 0.0f, 1.0f };
            hinge.otherPivot = { 0.0f, 0.0f, 0.0f };
            hinge.otherAxis = { 0.0f, 0.0f, 1.0f };
            hinge.minAngleLimit = 0.0f;
            hinge.maxAngleLimit = 1.0f;
            manager.GetECS().registry.emplace<PhysicsHingeComponent>(e, hinge);
        }

        // Persisted slider: owner platform, world anchor (empty otherBody).
        {
            auto e = manager.FindEntityByUuid(platform);
            PhysicsSliderComponent slider;
            slider.axis = { 1.0f, 0.0f, 0.0f };
            slider.lowerLimit = 0.0f;
            slider.upperLimit = 0.5f;
            manager.GetECS().registry.emplace<PhysicsSliderComponent>(e, slider);
        }
    }

    bool Play(Error& err)
    {
        ctrl.SetCollisionProvider(nullptr);
        return ctrl.Play(manager.AuthoringDoc(), bridge, err);
    }
};

bool T8DumpSorted(const PhysicsDebugLines& lines)
{
    PhysicsDebugLines copy = lines;
    const std::string before = copy.Dump();
    copy.SortStable();
    return copy.Dump() == before;
}

bool T8OwnerPresent(const PhysicsDebugLines& lines, const UUID& owner)
{
    for (const auto& s : lines.segments)
        if (s.owner == owner) return true;
    return false;
}

bool T8HasSegment(const PhysicsDebugLines& lines, const UUID& owner,
                  const glm::vec3& a, const glm::vec3& b)
{
    for (const auto& s : lines.segments)
        if (s.owner == owner && s.kind == PhysicsDebugLineKind::Constraint &&
            glm::length(s.a - a) < 1e-4f && glm::length(s.b - b) < 1e-4f)
            return true;
    return false;
}

} // namespace

TEST_CASE("T8 GREEN_PhysicsDebugLinesViaIconProjection: debug DTO projects through the existing icon projection split with no bridge change")
{
    T8Fixture fx;
    fx.BuildPlayfield();

    Error err;
    REQUIRE(fx.Play(err));
    CHECK(err.IsOk());
    // Play performs exactly one FullSync; debug geometry never travels the
    // bridge (no debug entry exists on ISceneRenderBridge by construction).
    CHECK(fx.bridge.fullSyncCalls == 1);

    const PhysicsDebugLines& lines = fx.ctrl.GetPhysicsDebugLines();
    CHECK_FALSE(lines.Empty());
    CHECK(fx.ctrl.PhysicsDebugLineCount() == lines.Count());
    // Five ownership groups: static ground, dynamic ball, kinematic
    // platform, trigger volume, persisted hinge+slider constraints.
    CHECK(lines.CountByKind(PhysicsDebugLineKind::Static) > 0);
    CHECK(lines.CountByKind(PhysicsDebugLineKind::Dynamic) > 0);
    CHECK(lines.CountByKind(PhysicsDebugLineKind::Kinematic) > 0);
    CHECK(lines.CountByKind(PhysicsDebugLineKind::Trigger) > 0);
    CHECK(lines.CountByKind(PhysicsDebugLineKind::Constraint) > 0);

    CHECK(T8OwnerPresent(lines, fx.ground));
    CHECK(T8OwnerPresent(lines, fx.ball));
    CHECK(T8OwnerPresent(lines, fx.platform));
    CHECK(T8OwnerPresent(lines, fx.trigger));
    CHECK(T8OwnerPresent(lines, fx.hingeOwner));

    // Deterministic headless dump: stable across consecutive reads and
    // already in UUID/segment order.
    const std::string dump1 = fx.ctrl.DumpPhysicsDebugLines();
    const std::string dump2 = fx.ctrl.DumpPhysicsDebugLines();
    CHECK_FALSE(dump1.empty());
    CHECK(dump1 == dump2);
    CHECK(T8DumpSorted(lines));

    // Top and side alignment through the existing CPU projection seam: the
    // dynamic ball at (0,1,0) sits on both view axes and must project to
    // the image centre in each view.
    glm::vec2 screen{ 0.0f };
    float depth = 0.0f;
    REQUIRE(ProjectToViewport({ 0.0f, 1.0f, 0.0f }, T8TopViewProj(),
        kImageMin, kImageSize, screen, depth));
    CHECK(screen.x == doctest::Approx(kImageMin.x + kImageW * 0.5f).epsilon(0.01));
    CHECK(screen.y == doctest::Approx(kImageMin.y + kImageH * 0.5f).epsilon(0.01));
    REQUIRE(ProjectToViewport({ 0.0f, 1.0f, 0.0f }, T8SideViewProj(),
        kImageMin, kImageSize, screen, depth));
    CHECK(screen.x == doctest::Approx(kImageMin.x + kImageW * 0.5f).epsilon(0.01));
    CHECK(screen.y == doctest::Approx(kImageMin.y + kImageH * 0.5f).epsilon(0.01));

    // At least one captured segment projects in each view (the playfield is
    // arranged around the origin for exactly this).
    size_t topHits = 0, sideHits = 0;
    for (const auto& seg : lines.segments)
    {
        glm::vec2 sa{ 0.0f }, sb{ 0.0f };
        float da = 0.0f, db = 0.0f;
        if (ProjectToViewport(seg.a, T8TopViewProj(), kImageMin, kImageSize, sa, da) &&
            ProjectToViewport(seg.b, T8TopViewProj(), kImageMin, kImageSize, sb, db))
            ++topHits;
        if (ProjectToViewport(seg.a, T8SideViewProj(), kImageMin, kImageSize, sa, da) &&
            ProjectToViewport(seg.b, T8SideViewProj(), kImageMin, kImageSize, sb, db))
            ++sideHits;
    }
    CHECK(topHits > 0);
    CHECK(sideHits > 0);

    fx.ctrl.Stop(fx.manager.AuthoringDoc(), fx.bridge);
}

TEST_CASE("T8 GREEN_PhysicsDebugPauseRetainsStopClears: Pause keeps the last snapshot, Stop empties it")
{
    T8Fixture fx;
    fx.BuildPlayfield();

    Error err;
    REQUIRE(fx.Play(err));
    const std::string atPlay = fx.ctrl.DumpPhysicsDebugLines();
    REQUIRE_FALSE(atPlay.empty());

    fx.ctrl.Pause();
    REQUIRE(fx.ctrl.GetState() == SceneRunState::Paused);
    // Pause performs no refresh: the snapshot is retained verbatim.
    CHECK(fx.ctrl.DumpPhysicsDebugLines() == atPlay);
    CHECK(fx.ctrl.PhysicsDebugLineCount() > 0);

    // A paused Step still simulates exactly one tick and re-captures.
    REQUIRE(fx.ctrl.Step(fx.bridge));
    CHECK_FALSE(fx.ctrl.DumpPhysicsDebugLines().empty());

    const std::string beforeStop = fx.ctrl.DumpPhysicsDebugLines();
    CHECK_FALSE(beforeStop.empty());
    fx.ctrl.Stop(fx.manager.AuthoringDoc(), fx.bridge);
    CHECK(fx.ctrl.GetState() == SceneRunState::Edit);
    CHECK(fx.ctrl.PhysicsDebugLineCount() == 0);
    CHECK(fx.ctrl.DumpPhysicsDebugLines().empty());
    CHECK(fx.ctrl.GetPhysicsDebugLines().Empty());
}

TEST_CASE("T8 GREEN_PhysicsConstraintAdapterUsesPersistedComponents: hinge/slider fixtures yield owned constraint segments without T5 Bullet constraints")
{
    T8Fixture fx;
    fx.BuildPlayfield();

    Error err;
    REQUIRE(fx.Play(err));

    // T5 builds no Bullet constraints on this branch: the constraint census
    // stays zero while the adapter still visualizes persisted components.
    CHECK(fx.ctrl.PhysicsConstraintCount() == 0);

    const PhysicsDebugLines& lines = fx.ctrl.GetPhysicsDebugLines();
    const size_t hingeSegs = lines.CountByKind(PhysicsDebugLineKind::Constraint);
    CHECK(hingeSegs > 0);
    // Both the hinge owner (ball) and the slider owner (platform) name
    // constraint segments.
    CHECK(T8OwnerPresent(lines, fx.ball));
    bool platformConstraint = false;
    for (const auto& s : lines.segments)
        if (s.owner == fx.platform && s.kind == PhysicsDebugLineKind::Constraint)
            platformConstraint = true;
    CHECK(platformConstraint);

    // Every constraint segment names a live body owner (never a null UUID).
    for (const auto& s : lines.segments)
    {
        if (s.kind != PhysicsDebugLineKind::Constraint) continue;
        CHECK_FALSE(s.owner.IsNull());
    }

    fx.ctrl.Stop(fx.manager.AuthoringDoc(), fx.bridge);
}

TEST_CASE("T8 RED_InvalidGeometryKeepsT4Diagnostic: missing colliders refuse Play with the UUID diagnostic and no debug shape")
{
    DeterministicUuidProvider ids;
    SceneManager manager;
    manager.SetUuidProvider(&ids);

    const UUID uuid = manager.CreateEmpty("BodyWithoutShape").affectedEntities.front();
    {
        auto e = manager.FindEntityByUuid(uuid);
        PhysicsBodyComponent body = T8DynamicBody();
        manager.GetECS().registry.emplace<PhysicsBodyComponent>(e, body);
        auto& tf = manager.GetECS().registry.get<Transform>(e);
        tf.translation = { 0.0f, 1.0f, 0.0f };
        SceneGraph::MarkDirty(manager.GetECS().registry, e);
    }

    RuntimeSceneController ctrl;
    T8NullBridge bridge;
    T8NoopObserver observer;
    ctrl.SetCollisionProvider(nullptr);

    Error err;
    CHECK_FALSE(ctrl.Play(manager.AuthoringDoc(), bridge, err));
    // Same T4 diagnostic contract: typed Error naming the entity UUID, plus
    // the atomic refusal (Edit, zero accumulator, no runtime/world).
    CHECK_FALSE(err.IsOk());
    CHECK(err.path == uuid.ToString());
    CHECK(err.detail.find(uuid.ToString()) != std::string::npos);
    CHECK(ctrl.GetState() == SceneRunState::Edit);
    CHECK(ctrl.TryGetPhysicsWorld() == nullptr);
    // No debug shape that looks successful: the snapshot stays empty.
    CHECK(ctrl.PhysicsDebugLineCount() == 0);
    CHECK(ctrl.DumpPhysicsDebugLines().empty());
    CHECK(ctrl.GetPhysicsDebugLines().Empty());
}

TEST_CASE("T8 GREEN_ConstraintFramesUseWorldTransforms: rotated scaled local frames and world anchor capture exact endpoints")
{
    T8Fixture fx;
    fx.BuildPlayfield();
    auto ball = fx.manager.FindEntityByUuid(fx.ball);
    auto platform = fx.manager.FindEntityByUuid(fx.platform);
    auto& ballTf = fx.manager.GetECS().registry.get<Transform>(ball);
    ballTf.rotation = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0, 1, 0));
    ballTf.scale = glm::vec3(2.0f);
    auto& platformTf = fx.manager.GetECS().registry.get<Transform>(platform);
    platformTf.rotation = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0, 0, 1));
    platformTf.scale = glm::vec3(1.5f);
    SceneGraph::MarkDirty(fx.manager.GetECS().registry, ball);
    SceneGraph::MarkDirty(fx.manager.GetECS().registry, platform);
    auto& hinge = fx.manager.GetECS().registry.get<PhysicsHingeComponent>(ball);
    hinge.ownerPivot = { 0.25f, 0.5f, -0.25f };
    hinge.ownerAxis = { 0, 0, 1 };
    hinge.otherPivot = { 0.5f, 0, 0 };
    hinge.otherAxis = { 1, 0, 0 };
    auto& slider = fx.manager.GetECS().registry.get<PhysicsSliderComponent>(platform);
    slider.otherBody = fx.ball;
    slider.axis = { 1, 0, 0 };
    slider.lowerLimit = 0.25f;
    slider.upperLimit = 0.75f;
    Error err;
    REQUIRE(fx.Play(err));
    const auto& lines = fx.ctrl.GetPhysicsDebugLines();
    const glm::vec3 hingePivot(-0.5f, 2.0f, -0.5f);
    const glm::vec3 hingeEnd(0.0f, 2.0f, -0.5f);
    const glm::vec3 otherPivot(2.0f, 1.25f, 0.0f);
    const glm::vec3 otherEnd(2.0f, 1.75f, 0.0f);
    const glm::vec3 sliderA(2.0f, 0.75f, 0.0f);
    const glm::vec3 sliderB(2.0f, 1.25f, 0.0f);
    CHECK(T8HasSegment(lines, fx.ball, hingePivot, hingeEnd));
    CHECK(T8HasSegment(lines, fx.ball, otherPivot, otherEnd));
    CHECK(T8HasSegment(lines, fx.platform, sliderA, sliderB));
    glm::vec2 topA, topB, sideA, sideB; float d = 0.0f, d2 = 0.0f;
    REQUIRE(ProjectToViewport(hingePivot, T8TopViewProj(), kImageMin, kImageSize, topA, d));
    REQUIRE(ProjectToViewport(hingeEnd, T8TopViewProj(), kImageMin, kImageSize, topB, d2));
    REQUIRE(ProjectToViewport(hingePivot, T8SideViewProj(), kImageMin, kImageSize, sideA, d));
    REQUIRE(ProjectToViewport(hingeEnd, T8SideViewProj(), kImageMin, kImageSize, sideB, d2));
    CHECK(topA != topB);
    CHECK(sideA != sideB);
    fx.ctrl.Stop(fx.manager.AuthoringDoc(), fx.bridge);
}

TEST_CASE("T8 RED_DebugCaptureAllocationIsTypedAndAtomic: drawer detaches and snapshot never publishes partial output")
{
    struct Guard { ~Guard() { PhysicsDebugDrawer::SetTestThrowOnNextLine(false); } } guard;
    T8Fixture fx;
    fx.BuildPlayfield();
    PhysicsDebugDrawer::SetTestThrowOnNextLine(true);
    Error err;
    CHECK_FALSE(fx.Play(err));
    CHECK(err.code == Error::Io);
    CHECK(fx.ctrl.GetState() == SceneRunState::Edit);
    CHECK(fx.ctrl.TryGetRuntimeScene() == nullptr);
    CHECK(fx.ctrl.TryGetPhysicsWorld() == nullptr);
    CHECK(fx.ctrl.GetPhysicsDebugLines().Empty());
}
