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
#include "PhysicsCollisionAssetProvider.h"
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
#include <filesystem>
#include <fstream>
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

// Defined with the other file-local helpers below; the required-playfield
// builder (a fixture member above) needs it first.
AssetReference T8ModelRef(const std::string& path, const std::string& key);

struct T8Fixture
{
    DeterministicUuidProvider ids;
    SceneManager manager;
    RuntimeSceneController ctrl;
    T8NullBridge bridge;
    T8NoopObserver observer;

    // Owned UUIDs for owner-identity assertions.
    UUID ground, ball, platform, mast, trigger, hingeOwner, ramp;

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

    // Required acceptance playfield for the frame-alignment proof:
    // provider-backed static triangle ramp ("ramp.obj" must already exist in
    // the provider context dir), dynamic ball, kinematic platform, static
    // trigger, a driven hinge (ball -> platform), a driven nil-otherBody
    // hinge (mast -> world anchor), and a driven slider
    // (platform -> ball). Drive params are authored data. Integration note:
    // T5 stages one live Bullet constraint per authored component (census 3)
    // and refuses two constraint kinds on one owner, so the nil-world hinge
    // lives on a dedicated kinematic mast that shares the platform's exact
    // world transform — every captured endpoint and pixel literal below is
    // unchanged. The adapter still visualizes the persisted components
    // without constructing constraints itself (no T5 duplication).
    void BuildRequiredPlayfield()
    {
        {
            const UUID uuid = manager.CreateEmpty("Ramp").affectedEntities.front();
            auto e = manager.FindEntityByUuid(uuid);
            manager.GetECS().registry.emplace<PhysicsBodyComponent>(e, T8StaticBody());
            PhysicsShapeComponent rampShape;
            rampShape.shape = PhysicsShapeKind::StaticTriMesh;
            rampShape.triMesh = T8ModelRef("ramp.obj", "obj:whole-model");
            manager.GetECS().registry.emplace<PhysicsShapeComponent>(e, rampShape);
            auto& tf = manager.GetECS().registry.get<Transform>(e);
            tf.translation = { -4.0f, 0.0f, 0.0f };
            SceneGraph::MarkDirty(manager.GetECS().registry, e);
            ramp = uuid;
        }

        {
            const UUID uuid = manager.CreateEmpty("Ball").affectedEntities.front();
            auto e = manager.FindEntityByUuid(uuid);
            manager.GetECS().registry.emplace<PhysicsBodyComponent>(e, T8DynamicBody());
            manager.GetECS().registry.emplace<PhysicsShapeComponent>(e, T8SphereShape(0.5f));
            auto& tf = manager.GetECS().registry.get<Transform>(e);
            tf.translation = { 0.0f, 1.0f, 0.0f };
            tf.rotation = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0, 1, 0));
            tf.scale = glm::vec3(2.0f);
            SceneGraph::MarkDirty(manager.GetECS().registry, e);
            ball = uuid;
            hingeOwner = uuid;
        }

        {
            const UUID uuid = manager.CreateEmpty("Platform").affectedEntities.front();
            auto e = manager.FindEntityByUuid(uuid);
            manager.GetECS().registry.emplace<PhysicsBodyComponent>(e, T8KinematicBody());
            manager.GetECS().registry.emplace<PhysicsShapeComponent>(e, T8BoxShape(0.5f));
            auto& tf = manager.GetECS().registry.get<Transform>(e);
            tf.translation = { 2.0f, 0.5f, 0.0f };
            tf.rotation = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0, 0, 1));
            tf.scale = glm::vec3(1.5f);
            SceneGraph::MarkDirty(manager.GetECS().registry, e);
            platform = uuid;
        }

        {
            PhysicsBodyComponent triggerBody = T8StaticBody();
            triggerBody.layer = PhysicsLayer::Trigger;
            triggerBody.mask = PhysicsLayer::Dynamic;
            PhysicsShapeComponent triggerShape = T8BoxShape(0.5f);
            triggerShape.isTrigger = true;
            trigger = AddBox("Trigger", { -2.0f, 0.5f, 0.0f },
                triggerBody, triggerShape);
        }

        // Driven hinge: owner ball, otherBody platform (both live bodies).
        {
            auto e = manager.FindEntityByUuid(ball);
            PhysicsHingeComponent hinge;
            hinge.otherBody = platform;
            hinge.ownerPivot = { 0.25f, 0.5f, -0.25f };
            hinge.ownerAxis = { 0.0f, 0.0f, 1.0f };
            hinge.otherPivot = { 0.5f, 0.0f, 0.0f };
            hinge.otherAxis = { 1.0f, 0.0f, 0.0f };
            hinge.minAngleLimit = 0.0f;
            hinge.maxAngleLimit = 1.0f;
            hinge.driveMode = 0;
            hinge.motorTargetVelocity = 2.0f;
            hinge.motorMaxImpulse = 1.5f;
            hinge.motorEnabled = true;
            manager.GetECS().registry.emplace<PhysicsHingeComponent>(e, hinge);
        }

        // Driven nil-otherBody hinge: owner mast, world anchor. The other
        // frame is authored in world space and must capture verbatim.
        // Integration note: the mast shares the platform's exact world
        // transform, so the owner-frame endpoints below are identical to the
        // T8-reviewed literals. A separate owner is required because T5
        // refuses hinge+slider coexistence on one owner.
        {
            const UUID uuid = manager.CreateEmpty("Mast").affectedEntities.front();
            auto m = manager.FindEntityByUuid(uuid);
            manager.GetECS().registry.emplace<PhysicsBodyComponent>(m, T8KinematicBody());
            manager.GetECS().registry.emplace<PhysicsShapeComponent>(m, T8BoxShape(0.5f));
            auto& mtf = manager.GetECS().registry.get<Transform>(m);
            mtf.translation = { 2.0f, 0.5f, 0.0f };
            mtf.rotation = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0, 0, 1));
            mtf.scale = glm::vec3(1.5f);
            SceneGraph::MarkDirty(manager.GetECS().registry, m);
            mast = uuid;

            PhysicsHingeComponent hinge;
            hinge.otherBody = UUID{};
            hinge.ownerPivot = { 0.0f, 0.5f, 0.0f };
            hinge.ownerAxis = { 1.0f, 0.0f, 0.0f };
            // T5 world-anchor consistency: the authored world frame must
            // express the same joint frame as the transformed owner frame
            // (owner pivot (0,0.5,0) -> (1.25,0.5,0), axis (1,0,0) -> (0,1,0);
            // disagreement refuses Play). The capture must still emit the
            // world frame verbatim rather than re-transforming it.
            hinge.otherPivot = { 1.25f, 0.5f, 0.0f };
            hinge.otherAxis = { 0.0f, 1.0f, 0.0f };
            hinge.minAngleLimit = -0.5f;
            hinge.maxAngleLimit = 0.5f;
            hinge.driveMode = 0;
            hinge.motorTargetVelocity = -1.0f;
            hinge.motorMaxImpulse = 0.75f;
            hinge.motorEnabled = true;
            manager.GetECS().registry.emplace<PhysicsHingeComponent>(m, hinge);
        }

        // Driven slider: owner platform, non-null otherBody ball.
        {
            auto e = manager.FindEntityByUuid(platform);
            PhysicsSliderComponent slider;
            slider.otherBody = ball;
            slider.axis = { 1.0f, 0.0f, 0.0f };
            slider.lowerLimit = 0.25f;
            slider.upperLimit = 0.75f;
            slider.targetPosition = 0.5f;
            slider.motorTargetVelocity = 1.0f;
            slider.motorMaxForce = 5.0f;
            slider.motorEnabled = true;
            manager.GetECS().registry.emplace<PhysicsSliderComponent>(e, slider);
        }
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

const PhysicsDebugSegment* T8FindSegment(const PhysicsDebugLines& lines,
                                         const UUID& owner, const glm::vec3& a,
                                         const glm::vec3& b)
{
    for (const auto& s : lines.segments)
        if (s.owner == owner && s.kind == PhysicsDebugLineKind::Constraint &&
            glm::length(s.a - a) < 1e-4f && glm::length(s.b - b) < 1e-4f)
            return &s;
    return nullptr;
}

// Pin exact screen pixels for a world point through one view. Expected values
// are independently calculated literals (see the alignment test table), so a
// wrong captured frame or a wrong projection fails here — inequality between
// two endpoints would not discriminate.
void T8CheckPixels(const glm::vec3& world, const glm::mat4& viewProj,
                   float expX, float expY)
{
    glm::vec2 screen{ 0.0f };
    float depth = 0.0f;
    REQUIRE(ProjectToViewport(world, viewProj, kImageMin, kImageSize,
        screen, depth));
    CHECK(screen.x == doctest::Approx(expX).epsilon(1e-3));
    CHECK(screen.y == doctest::Approx(expY).epsilon(1e-3));
}

AssetReference T8ModelRef(const std::string& path, const std::string& key)
{
    AssetReference ref;
    ref.kind = AssetKind::Model;
    ref.path = path;
    ref.sourceKey = key;
    return ref;
}

void T8WriteTextFile(const std::filesystem::path& path, const std::string& text)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    REQUIRE_MESSAGE(!ec, "T8 fixture directory creation failed");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE_MESSAGE(out.is_open(), "T8 fixture file open failed");
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    REQUIRE_MESSAGE(out.good(), "T8 fixture file write failed");
}

// Own temp dir (distinct from the T4 collision dir) holding the required
// static triangle ramp OBJ.
struct T8TempDir
{
    std::filesystem::path dir;
    T8TempDir()
    {
        dir = std::filesystem::temp_directory_path() / "t8_debug_ramp";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        REQUIRE_MESSAGE(!ec, "T8 fixture cleanup failed");
        std::filesystem::create_directories(dir, ec);
        REQUIRE_MESSAGE(!ec, "T8 fixture directory creation failed");
    }
    ~T8TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

// Sloped static triangle ramp (both windings, mirroring the T4 ramp proof so
// contact never depends on triangle sidedness).
const char* kT8RampObj =
    "v 0 2 -1\nv 4 0 -1\nv 4 0 1\nv 0 2 1\n"
    "f 1 2 3\nf 1 3 4\nf 1 4 3\nf 1 3 2\n";

// Clears the deterministic allocation-failure arm even when a test aborts
// early, so one injected failure can never poison a later test's capture.
struct T8ThrowGuard
{
    ~T8ThrowGuard() { PhysicsDebugDrawer::SetTestThrowOnNextLine(false); }
};

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

    // Integration note: T5 stages one live Bullet constraint per authored
    // component (census 2: hinge + slider); the adapter still visualizes the
    // persisted components without constructing any itself (no duplication).
    CHECK(fx.ctrl.PhysicsConstraintCount() == 2);

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

TEST_CASE("T8 GREEN_ConstraintFramesUseWorldTransforms: required ramp/dynamic/driven-hinge/driven-slider/trigger fixture captures exact endpoints and exact top/side pixels")
{
    T8TempDir assets;
    T8WriteTextFile(assets.dir / "ramp.obj", kT8RampObj);
    PhysicsCollisionAssetProvider provider;
    provider.SetContext(AssetResolutionContext{ assets.dir, nullptr });

    T8Fixture fx;
    fx.BuildRequiredPlayfield();
    // NOTE: T8Fixture::Play pins a null provider; the required fixture needs
    // the live provider, so Play through the controller directly.
    fx.ctrl.SetCollisionProvider(&provider);

    Error err;
    REQUIRE(fx.ctrl.Play(fx.manager.AuthoringDoc(), fx.bridge, err));
    CHECK(err.IsOk());

    // T5 stages one live Bullet constraint per authored component on the
    // integrated branch (census 3: two hinges + one slider); the adapter
    // still visualizes the persisted components without constructing any.
    CHECK(fx.ctrl.PhysicsConstraintCount() == 3);

    // The acceptance fixture is literally driven: motor params are authored
    // on the persisted hinge/slider components (read back from authoring).
    {
        auto ballE = fx.manager.FindEntityByUuid(fx.ball);
        const bool ballResolved = (ballE != entt::null);
        REQUIRE(ballResolved);
        const auto& hinge =
            fx.manager.GetECS().registry.get<PhysicsHingeComponent>(ballE);
        CHECK(hinge.motorEnabled);
        CHECK(hinge.motorTargetVelocity == doctest::Approx(2.0f));
        CHECK(hinge.motorMaxImpulse == doctest::Approx(1.5f));
        CHECK_FALSE(hinge.otherBody.IsNull());

        auto mastE = fx.manager.FindEntityByUuid(fx.mast);
        const bool mastResolved = (mastE != entt::null);
        REQUIRE(mastResolved);
        const auto& worldHinge =
            fx.manager.GetECS().registry.get<PhysicsHingeComponent>(mastE);
        CHECK(worldHinge.motorEnabled);
        CHECK(worldHinge.otherBody.IsNull());

        auto platE = fx.manager.FindEntityByUuid(fx.platform);
        const bool platResolved = (platE != entt::null);
        REQUIRE(platResolved);
        const auto& slider =
            fx.manager.GetECS().registry.get<PhysicsSliderComponent>(platE);
        CHECK(slider.motorEnabled);
        CHECK(slider.motorTargetVelocity == doctest::Approx(1.0f));
        CHECK(slider.motorMaxForce == doctest::Approx(5.0f));
        CHECK(slider.otherBody == fx.ball);
    }

    const PhysicsDebugLines& lines = fx.ctrl.GetPhysicsDebugLines();
    // Fixture census: provider-backed ramp stages and captures, dynamic ball,
    // kinematic platform, kinematic mast, and trigger volume are all present.
    CHECK(T8OwnerPresent(lines, fx.ramp));
    CHECK(T8OwnerPresent(lines, fx.ball));
    CHECK(T8OwnerPresent(lines, fx.platform));
    CHECK(T8OwnerPresent(lines, fx.mast));
    CHECK(T8OwnerPresent(lines, fx.trigger));
    CHECK(lines.CountByKind(PhysicsDebugLineKind::Static) > 0);
    CHECK(lines.CountByKind(PhysicsDebugLineKind::Trigger) > 0);
    CHECK(lines.CountByKind(PhysicsDebugLineKind::Constraint) > 0);

    // Exact world endpoints, hand-derived from the authored frames through
    // the full Play-time world matrices (ball: T(0,1,0) R_y(90) S2;
    // platform: T(2,0.5,0) R_z(90) S1.5):
    //   hinge owner pivot (0.25,0.5,-0.25) -> (-0.5,2.0,-0.5),
    //     axis (0,0,1) -> (1,0,0), len 0.5;
    //   hinge other pivot (0.5,0,0) -> (2.0,1.25,0.0),
    //     axis (1,0,0) -> (0,1,0), len 0.5;
    //   slider origin (2.0,0.5,0.0), axis (1,0,0) -> (0,1,0), lo/hi .25/.75;
    //   nil-hinge owner pivot (0,0.5,0) -> (1.25,0.5,0.0),
    //     axis (1,0,0) -> (0,1,0), len 0.5;
    //   nil-hinge world frame verbatim and T5-agreed: (1.25,0.5,0),
    //     axis (0,1,0) (swap-indistinguishable by construction once T5
    //     requires agreement; T5 WorldHingeFramesExact covers refusal).
    const glm::vec3 hingePivot(-0.5f, 2.0f, -0.5f);
    const glm::vec3 hingeEnd(0.0f, 2.0f, -0.5f);
    const glm::vec3 otherPivot(2.0f, 1.25f, 0.0f);
    const glm::vec3 otherEnd(2.0f, 1.75f, 0.0f);
    const glm::vec3 sliderA(2.0f, 0.75f, 0.0f);
    const glm::vec3 sliderB(2.0f, 1.25f, 0.0f);
    const glm::vec3 nilOwnerA(1.25f, 0.5f, 0.0f);
    const glm::vec3 nilOwnerB(1.25f, 1.0f, 0.0f);
    const glm::vec3 nilWorldA(1.25f, 0.5f, 0.0f);
    const glm::vec3 nilWorldB(1.25f, 1.0f, 0.0f);
    CHECK(T8HasSegment(lines, fx.ball, hingePivot, hingeEnd));
    CHECK(T8HasSegment(lines, fx.ball, otherPivot, otherEnd));
    CHECK(T8HasSegment(lines, fx.platform, sliderA, sliderB));
    // Nil-otherBody discriminator: the owner frame is transformed, the world
    // anchor is verbatim. The nil hinge lives on the mast (same world
    // transform as the platform held on the T8 branch, so all literals hold).
    CHECK(T8HasSegment(lines, fx.mast, nilOwnerA, nilOwnerB));
    CHECK(T8HasSegment(lines, fx.mast, nilWorldA, nilWorldB));

    // Exact top/side pixels for the CAPTURED endpoints (found in the DTO, not
    // re-projected constants). Independently calculated literals: any wrong
    // frame or wrong projection misses these by far more than tolerance.
    struct T8PixelRow
    {
        UUID owner;
        glm::vec3 a, b;
        float topAx, topAy, topBx, topBy;
        float sideAx, sideAy, sideBx, sideBy;
    };
    const T8PixelRow rows[] = {
        { fx.ball, hingePivot, hingeEnd,
          454.73f, 304.73f, 500.00f, 304.73f,
          534.49f, 281.02f, 536.21f, 277.57f },
        { fx.ball, otherPivot, otherEnd,
          665.55f, 350.00f, 675.58f, 350.00f,
          500.00f, 327.37f, 500.00f, 282.10f },
        { fx.platform, sliderA, sliderB,
          656.60f, 350.00f, 665.55f, 350.00f,
          500.00f, 372.63f, 500.00f, 327.37f },
        { fx.mast, nilOwnerA, nilOwnerB,
          595.30f, 350.00f, 600.59f, 350.00f,
          500.00f, 391.39f, 500.00f, 350.00f },
        { fx.mast, nilWorldA, nilWorldB,
          595.30f, 350.00f, 600.59f, 350.00f,
          500.00f, 391.39f, 500.00f, 350.00f },
    };
    const glm::mat4 top = T8TopViewProj();
    const glm::mat4 side = T8SideViewProj();
    for (const auto& row : rows)
    {
        const PhysicsDebugSegment* seg =
            T8FindSegment(lines, row.owner, row.a, row.b);
        REQUIRE_MESSAGE(seg != nullptr, "captured constraint segment missing");
        T8CheckPixels(seg->a, top, row.topAx, row.topAy);
        T8CheckPixels(seg->b, top, row.topBx, row.topBy);
        T8CheckPixels(seg->a, side, row.sideAx, row.sideAy);
        T8CheckPixels(seg->b, side, row.sideBx, row.sideBy);
    }

    fx.ctrl.Stop(fx.manager.AuthoringDoc(), fx.bridge);
}

TEST_CASE("T8 RED_DebugCaptureAllocationIsTypedAndAtomic: drawer detaches and snapshot never publishes partial output")
{
    T8ThrowGuard guard;
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

TEST_CASE("T8 RED_DebugCaptureUpdateFailureRetainsSnapshot: injected Update failure keeps Playing, the prior dump, and a detached drawer")
{
    T8ThrowGuard guard;
    T8Fixture fx;
    fx.BuildPlayfield();

    Error err;
    REQUIRE(fx.Play(err));
    REQUIRE(fx.ctrl.GetState() == SceneRunState::Playing);
    const std::string before = fx.ctrl.DumpPhysicsDebugLines();
    REQUIRE_FALSE(before.empty());
    REQUIRE(fx.ctrl.LastPhysicsDebugError().IsOk());
    PhysicsWorld* world = fx.ctrl.TryGetPhysicsWorldMut();
    REQUIRE(world != nullptr);
    REQUIRE_FALSE(world->HasDebugDrawerForTests());

    // One injected capture failure during Playing Update: the tick still
    // simulates, but the snapshot must not be replaced by partial output.
    PhysicsDebugDrawer::SetTestThrowOnNextLine(true);
    fx.ctrl.Update(kFixedDt, fx.bridge);
    CHECK(fx.ctrl.GetState() == SceneRunState::Playing);
    CHECK(fx.ctrl.TryGetPhysicsWorld() == world);
    CHECK(fx.ctrl.LastPhysicsDebugError().code == Error::Io);
    CHECK(fx.ctrl.DumpPhysicsDebugLines() == before);
    CHECK(fx.ctrl.PhysicsDebugLineCount() > 0);
    CHECK_FALSE(world->HasDebugDrawerForTests());

    // Recovery: the next clean Update clears the typed error and refreshes.
    fx.ctrl.Update(kFixedDt, fx.bridge);
    CHECK(fx.ctrl.LastPhysicsDebugError().IsOk());
    CHECK_FALSE(fx.ctrl.DumpPhysicsDebugLines().empty());
    CHECK_FALSE(world->HasDebugDrawerForTests());

    fx.ctrl.Stop(fx.manager.AuthoringDoc(), fx.bridge);
}

TEST_CASE("T8 RED_DebugCaptureStepFailureRetainsSnapshot: injected paused-Step failure returns false after one tick with state, dump, and drawer intact")
{
    T8ThrowGuard guard;
    T8Fixture fx;
    fx.BuildPlayfield();

    Error err;
    REQUIRE(fx.Play(err));
    fx.ctrl.Pause();
    REQUIRE(fx.ctrl.GetState() == SceneRunState::Paused);
    const std::string before = fx.ctrl.DumpPhysicsDebugLines();
    REQUIRE_FALSE(before.empty());
    const uint64_t stepsBefore = fx.ctrl.PhysicsStepCount();
    PhysicsWorld* world = fx.ctrl.TryGetPhysicsWorldMut();
    REQUIRE(world != nullptr);
    REQUIRE_FALSE(world->HasDebugDrawerForTests());

    // The paused Step completes exactly one physics tick, then its capture
    // fails: Step reports false without a half-committed lifecycle.
    PhysicsDebugDrawer::SetTestThrowOnNextLine(true);
    CHECK_FALSE(fx.ctrl.Step(fx.bridge));
    CHECK(fx.ctrl.PhysicsStepCount() == stepsBefore + 1);
    CHECK(fx.ctrl.GetState() == SceneRunState::Paused);
    CHECK(fx.ctrl.TryGetPhysicsWorld() == world);
    CHECK(fx.ctrl.LastPhysicsDebugError().code == Error::Io);
    CHECK(fx.ctrl.DumpPhysicsDebugLines() == before);
    CHECK_FALSE(world->HasDebugDrawerForTests());

    fx.ctrl.Stop(fx.manager.AuthoringDoc(), fx.bridge);
}

TEST_CASE("T8 RED_DebugCaptureRestoresPriorDrawer: RAII restores the exact pre-existing Bullet drawer on success and failure")
{
    T8ThrowGuard guard;
    T8Fixture fx;
    fx.BuildPlayfield();

    Error err;
    REQUIRE(fx.Play(err));
    PhysicsWorld* world = fx.ctrl.TryGetPhysicsWorldMut();
    REQUIRE(world != nullptr);
    REQUIRE(world->DebugDrawerForTests() == nullptr);

    // Inert sentinel: never begins a capture, so its own drawLine is a
    // no-op; only its pointer identity is observed.
    PhysicsDebugDrawer sentinel;
    world->SetDebugDrawerForTests(&sentinel);

    // Success path restores the exact sentinel (not merely "a" drawer).
    fx.ctrl.Update(kFixedDt, fx.bridge);
    CHECK(fx.ctrl.LastPhysicsDebugError().IsOk());
    CHECK(world->DebugDrawerForTests() == &sentinel);
    CHECK_FALSE(fx.ctrl.DumpPhysicsDebugLines().empty());

    // Failure path restores the exact sentinel too — never null, never the
    // destroyed temporary.
    PhysicsDebugDrawer::SetTestThrowOnNextLine(true);
    fx.ctrl.Update(kFixedDt, fx.bridge);
    CHECK(fx.ctrl.LastPhysicsDebugError().code == Error::Io);
    CHECK(world->DebugDrawerForTests() == &sentinel);

    world->SetDebugDrawerForTests(nullptr);
    fx.ctrl.Update(kFixedDt, fx.bridge);
    CHECK(fx.ctrl.LastPhysicsDebugError().IsOk());
    CHECK_FALSE(world->HasDebugDrawerForTests());

    fx.ctrl.Stop(fx.manager.AuthoringDoc(), fx.bridge);
}
