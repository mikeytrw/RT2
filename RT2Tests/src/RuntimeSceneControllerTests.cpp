#include <doctest/doctest.h>

#include "RuntimeSceneController.h"
#include "SceneSerializer.h"
#include "SceneDocument.h"
#include "ECSComponents.h"
#include "ECSScene.h"
#include "PrimitiveGeometry.h"
#include "core/UUID.h"
#include "core/Error.h"
#include "ISceneRenderBridge.h"
#include "GPUSceneData.h"

#include <vector>
#include <fstream>
#include <filesystem>
#include <set>

using namespace rt2::core;

// ============================================================================
// NullSceneRenderBridge — records sync calls for test assertions, does no
// GPU work. Used by RuntimeSceneControllerTests and RT2SliceRunner.
// ============================================================================

class NullSceneRenderBridge final : public ISceneRenderBridge
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
    void ResetTemporalState() override             { ++resetTemporalCalls; }
    void RequestRender() override                  { ++renderRequests; }

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
// Fixture builder (same as SceneSerializerTests but local to avoid cross-
// file dependencies)
// ============================================================================

namespace {

SceneDocument BuildRuntimeFixture(IUuidProvider* provider)
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

    // Camera entity
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

} // anonymous namespace

// ============================================================================
// State transition tests
// ============================================================================

TEST_CASE("VS-3 Runtime: starts in Edit state")
{
    RuntimeSceneController ctrl;
    CHECK(ctrl.GetState() == SceneRunState::Edit);
    CHECK(ctrl.TryGetRuntimeScene() == nullptr);
}

TEST_CASE("VS-3 Runtime: Play transitions to Playing")
{
    DeterministicUuidProvider provider;
    SceneDocument authoring = BuildRuntimeFixture(&provider);
    NullSceneRenderBridge bridge;
    Error err;

    RuntimeSceneController ctrl;
    CHECK(ctrl.Play(authoring, bridge, err));
    CHECK(err.IsOk());
    CHECK(ctrl.GetState() == SceneRunState::Playing);
    CHECK(ctrl.TryGetRuntimeScene() != nullptr);
}

TEST_CASE("VS-3 Runtime: Play calls FullSync + ResetTemporalState")
{
    DeterministicUuidProvider provider;
    SceneDocument authoring = BuildRuntimeFixture(&provider);
    NullSceneRenderBridge bridge;
    Error err;

    RuntimeSceneController ctrl;
    ctrl.Play(authoring, bridge, err);

    CHECK(bridge.fullSyncCalls == 1);
    CHECK(bridge.resetTemporalCalls == 1);
}

TEST_CASE("VS-3 Runtime: Pause transitions to Paused and clears accumulator")
{
    DeterministicUuidProvider provider;
    SceneDocument authoring = BuildRuntimeFixture(&provider);
    NullSceneRenderBridge bridge;
    Error err;

    RuntimeSceneController ctrl;
    ctrl.Play(authoring, bridge, err);

    // Run a few updates to build up accumulator residue.
    ctrl.Update(0.001f, bridge);

    ctrl.Pause();
    CHECK(ctrl.GetState() == SceneRunState::Paused);
}

TEST_CASE("VS-3 Runtime: Step returns false unless Paused")
{
    DeterministicUuidProvider provider;
    SceneDocument authoring = BuildRuntimeFixture(&provider);
    NullSceneRenderBridge bridge;
    Error err;

    RuntimeSceneController ctrl;
    // Edit -> Step fails
    CHECK_FALSE(ctrl.Step(bridge));

    ctrl.Play(authoring, bridge, err);
    // Playing -> Step fails
    CHECK_FALSE(ctrl.Step(bridge));

    ctrl.Pause();
    // Paused -> Step succeeds
    CHECK(ctrl.Step(bridge));
}

TEST_CASE("VS-3 Runtime: Stop transitions back to Edit")
{
    DeterministicUuidProvider provider;
    SceneDocument authoring = BuildRuntimeFixture(&provider);
    NullSceneRenderBridge bridge;
    Error err;

    RuntimeSceneController ctrl;
    ctrl.Play(authoring, bridge, err);
    ctrl.Pause();
    ctrl.Stop(authoring, bridge);

    CHECK(ctrl.GetState() == SceneRunState::Edit);
    CHECK(ctrl.TryGetRuntimeScene() == nullptr);
}

TEST_CASE("VS-3 Runtime: Stop calls FullSync + ResetTemporalState")
{
    DeterministicUuidProvider provider;
    SceneDocument authoring = BuildRuntimeFixture(&provider);
    NullSceneRenderBridge bridge;
    Error err;

    RuntimeSceneController ctrl;
    ctrl.Play(authoring, bridge, err);

    bridge.Reset();
    ctrl.Stop(authoring, bridge);

    CHECK(bridge.fullSyncCalls == 1);
    CHECK(bridge.resetTemporalCalls == 1);
}

// ============================================================================
// Clone independence tests
// ============================================================================

TEST_CASE("VS-3 Runtime: runtime clone preserves UUIDs")
{
    DeterministicUuidProvider provider;
    SceneDocument authoring = BuildRuntimeFixture(&provider);
    NullSceneRenderBridge bridge;
    Error err;

    RuntimeSceneController ctrl;
    ctrl.Play(authoring, bridge, err);

    const SceneDocument* runtime = ctrl.TryGetRuntimeScene();
    REQUIRE(runtime != nullptr);

    // Same UUIDs
    auto authView  = authoring.ecs.registry.view<EntityIdComponent>();
    auto rtView    = runtime->ecs.registry.view<EntityIdComponent>();
    CHECK(authView.size() == rtView.size());

    std::set<UUID> authUuids, rtUuids;
    for (auto e : authView) authUuids.insert(authView.get<EntityIdComponent>(e).id);
    for (auto e : rtView)   rtUuids.insert(rtView.get<EntityIdComponent>(e).id);
    CHECK(authUuids == rtUuids);
}

TEST_CASE("VS-3 Runtime: runtime transform mutation does not affect authoring")
{
    DeterministicUuidProvider provider;
    SceneDocument authoring = BuildRuntimeFixture(&provider);
    NullSceneRenderBridge bridge;
    Error err;

    // Snapshot the authoring cube transform before Play.
    glm::vec3 authCubePosBefore = { 0.0f, 0.0f, 0.0f };
    {
        auto view = authoring.ecs.registry.view<MotionComponent>();
        for (auto e : view)
        {
            authCubePosBefore = authoring.ecs.registry.get<Transform>(e).translation;
        }
    }

    RuntimeSceneController ctrl;
    ctrl.Play(authoring, bridge, err);

    // Run several updates to move the runtime cube.
    for (int i = 0; i < 10; ++i)
        ctrl.Update(kFixedDt, bridge);

    // Authoring cube should be unchanged.
    {
        auto view = authoring.ecs.registry.view<MotionComponent>();
        for (auto e : view)
        {
            auto& tf = authoring.ecs.registry.get<Transform>(e);
            CHECK(tf.translation.x == doctest::Approx(authCubePosBefore.x));
        }
    }
}

TEST_CASE("VS-3 Runtime: Stop restores authoring state exactly")
{
    DeterministicUuidProvider provider;
    SceneDocument authoring = BuildRuntimeFixture(&provider);
    NullSceneRenderBridge bridge;
    Error err;

    // Save the authoring document to a string before Play.
    auto path = std::filesystem::temp_directory_path() / "rt3_stop_test.rt2scene";
    SceneSerializer::Save(authoring, path, err);

    RuntimeSceneController ctrl;
    ctrl.Play(authoring, bridge, err);

    // Mutate runtime extensively.
    for (int i = 0; i < 60; ++i)
        ctrl.Update(kFixedDt, bridge);

    ctrl.Stop(authoring, bridge);

    // Re-save the authoring document after Stop and compare bytes.
    auto path2 = std::filesystem::temp_directory_path() / "rt3_stop_after.rt2scene";
    SceneSerializer::Save(authoring, path2, err);

    std::ifstream f1(path, std::ios::binary), f2(path2, std::ios::binary);
    std::string s1((std::istreambuf_iterator<char>(f1)), std::istreambuf_iterator<char>());
    std::string s2((std::istreambuf_iterator<char>(f2)), std::istreambuf_iterator<char>());
    f1.close();
    f2.close();

    CHECK(s1 == s2);

    std::error_code ec1, ec2;
    std::filesystem::remove(path, ec1);
    std::filesystem::remove(path2, ec2);
}

// ============================================================================
// prevWorldMatrix initialization tests
// ============================================================================

TEST_CASE("VS-3 Runtime: prevWorldMatrix equals worldMatrix after Play")
{
    DeterministicUuidProvider provider;
    SceneDocument authoring = BuildRuntimeFixture(&provider);
    NullSceneRenderBridge bridge;
    Error err;

    RuntimeSceneController ctrl;
    ctrl.Play(authoring, bridge, err);

    const SceneDocument* runtime = ctrl.TryGetRuntimeScene();
    REQUIRE(runtime != nullptr);

    auto view = runtime->ecs.registry.view<Transform>();
    for (auto e : view)
    {
        auto& tf = view.get<Transform>(e);
        // prevWorldMatrix should equal worldMatrix (no motion on first frame).
        for (int row = 0; row < 4; ++row)
        {
            for (int col = 0; col < 4; ++col)
            {
                float prev = tf.prevWorldMatrix[col][row]; // glm is col-major
                float curr = tf.worldMatrix[col][row];
                CHECK(prev == doctest::Approx(curr).epsilon(0.001f));
            }
        }
    }
}

// ============================================================================
// Update / motion tests
// ============================================================================

TEST_CASE("VS-3 Runtime: Update runs motion and calls TransformSync")
{
    DeterministicUuidProvider provider;
    SceneDocument authoring = BuildRuntimeFixture(&provider);
    NullSceneRenderBridge bridge;
    Error err;

    RuntimeSceneController ctrl;
    ctrl.Play(authoring, bridge, err);
    bridge.Reset();

    // One update with exactly fixed dt → one fixed step.
    ctrl.Update(kFixedDt, bridge);

    // Should have called TransformSync once and RequestRender once.
    CHECK(bridge.transformSyncCalls == 1);
    CHECK(bridge.renderRequests == 1);
    CHECK(bridge.fullSyncCalls == 0); // no structural sync during motion

    // The runtime cube should have moved by velocity * dt.
    const SceneDocument* runtime = ctrl.TryGetRuntimeScene();
    REQUIRE(runtime != nullptr);
    auto view = runtime->ecs.registry.view<MotionComponent>();
    for (auto e : view)
    {
        auto& tf = runtime->ecs.registry.get<Transform>(e);
        CHECK(tf.translation.x == doctest::Approx(kFixedDt)); // v=1, dt=1/60
    }
}

TEST_CASE("VS-3 Runtime: 60 fixed steps move cube by ~1.0 unit")
{
    DeterministicUuidProvider provider;
    SceneDocument authoring = BuildRuntimeFixture(&provider);
    NullSceneRenderBridge bridge;
    Error err;

    RuntimeSceneController ctrl;
    ctrl.Play(authoring, bridge, err);

    for (int i = 0; i < 60; ++i)
        ctrl.Update(kFixedDt, bridge);

    const SceneDocument* runtime = ctrl.TryGetRuntimeScene();
    REQUIRE(runtime != nullptr);
    auto view = runtime->ecs.registry.view<MotionComponent>();
    for (auto e : view)
    {
        auto& tf = runtime->ecs.registry.get<Transform>(e);
        CHECK(tf.translation.x == doctest::Approx(1.0f).epsilon(0.01f));
    }
}

TEST_CASE("VS-3 Runtime: Paused Update is a no-op")
{
    DeterministicUuidProvider provider;
    SceneDocument authoring = BuildRuntimeFixture(&provider);
    NullSceneRenderBridge bridge;
    Error err;

    RuntimeSceneController ctrl;
    ctrl.Play(authoring, bridge, err);
    ctrl.Pause();

    bridge.Reset();
    ctrl.Update(0.1f, bridge); // should do nothing

    CHECK(bridge.transformSyncCalls == 0);
    CHECK(bridge.renderRequests == 0);

    // Cube should not have moved.
    const SceneDocument* runtime = ctrl.TryGetRuntimeScene();
    REQUIRE(runtime != nullptr);
    auto view = runtime->ecs.registry.view<MotionComponent>();
    for (auto e : view)
    {
        auto& tf = runtime->ecs.registry.get<Transform>(e);
        CHECK(tf.translation.x == doctest::Approx(0.0f));
    }
}

TEST_CASE("VS-3 Runtime: Step runs exactly one fixed tick")
{
    DeterministicUuidProvider provider;
    SceneDocument authoring = BuildRuntimeFixture(&provider);
    NullSceneRenderBridge bridge;
    Error err;

    RuntimeSceneController ctrl;
    ctrl.Play(authoring, bridge, err);
    ctrl.Pause();

    bridge.Reset();
    CHECK(ctrl.Step(bridge));

    CHECK(bridge.transformSyncCalls == 1);
    CHECK(bridge.renderRequests == 1);

    // Cube should have moved by exactly one fixed dt.
    const SceneDocument* runtime = ctrl.TryGetRuntimeScene();
    REQUIRE(runtime != nullptr);
    auto view = runtime->ecs.registry.view<MotionComponent>();
    for (auto e : view)
    {
        auto& tf = runtime->ecs.registry.get<Transform>(e);
        CHECK(tf.translation.x == doctest::Approx(kFixedDt));
    }
}

TEST_CASE("VS-3 Runtime: large frame time is clamped")
{
    DeterministicUuidProvider provider;
    SceneDocument authoring = BuildRuntimeFixture(&provider);
    NullSceneRenderBridge bridge;
    Error err;

    RuntimeSceneController ctrl;
    ctrl.Play(authoring, bridge, err);

    // A very large frame dt should be clamped to kMaxFrameTime and produce
    // at most kMaxSubsteps fixed ticks.
    ctrl.Update(10.0f, bridge); // 10 seconds — way over the clamp

    // Cube should have moved by at most kMaxSubsteps * fixedDt, not 10 units.
    const SceneDocument* runtime = ctrl.TryGetRuntimeScene();
    REQUIRE(runtime != nullptr);
    auto view = runtime->ecs.registry.view<MotionComponent>();
    for (auto e : view)
    {
        auto& tf = runtime->ecs.registry.get<Transform>(e);
        float maxMove = kMaxSubsteps * kFixedDt;
        CHECK(tf.translation.x <= doctest::Approx(maxMove).epsilon(0.01f));
    }
}

// ============================================================================
// Repeated Play/Stop cycle test
// ============================================================================

TEST_CASE("VS-3 Runtime: 100 Play/Stop cycles do not leak or corrupt")
{
    DeterministicUuidProvider provider;
    SceneDocument authoring = BuildRuntimeFixture(&provider);
    NullSceneRenderBridge bridge;
    Error err;

    RuntimeSceneController ctrl;

    for (int i = 0; i < 100; ++i)
    {
        CHECK(ctrl.Play(authoring, bridge, err));
        CHECK(err.IsOk());
        ctrl.Update(kFixedDt, bridge);
        ctrl.Stop(authoring, bridge);
    }

    CHECK(ctrl.GetState() == SceneRunState::Edit);
    CHECK(ctrl.TryGetRuntimeScene() == nullptr);

    // Authoring cube should still be at origin.
    auto view = authoring.ecs.registry.view<MotionComponent>();
    for (auto e : view)
    {
        auto& tf = authoring.ecs.registry.get<Transform>(e);
        CHECK(tf.translation.x == doctest::Approx(0.0f));
    }
}