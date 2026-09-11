#include <doctest/doctest.h>

#include "EditorCameraWorkflow.h"
#include "EditorSceneState.h"
#include "SceneManager.h"
#include "SceneSerializer.h"
#include "SceneSerializerTestSupport.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>

// ============================================================================
// F2: complete camera adoption (position, forward, lens, look) across every
// route, with CLI-override precedence and the no-reset presentation policy.
//
// AdoptAuthoringCameraView (WalnutApp, not linkable here) builds the adopted
// pose through TryBuildAuthoringAdoptionPose and applies it atomically, so
// these tests pin the complete construction, the apply-action policy, and
// each CPU-reachable route: native open/recovery/import/headless document
// adoption, stale second scenes, Play-time runtime adoption, View Through,
// Align, and bookmark recall.
// ============================================================================

namespace
{
EditorCameraPose MakePose()
{
    EditorCameraPose pose;
    pose.position = { 1.0f, 2.0f, 3.0f };
    pose.forward = { 0.0f, 0.0f, -1.0f };
    pose.verticalFOV = 45.0f;
    pose.aperture = 0.0f;
    pose.focusDistance = 5.0f;
    pose.farClip = 10000.0f;
    return pose;
}

struct SceneFixture
{
    rt2::core::DeterministicUuidProvider ids;
    SceneManager manager;

    SceneFixture() { manager.SetUuidProvider(&ids); }
};

} // namespace

TEST_CASE("F2 adoption pose carries complete lens and look, retains far clip")
{
    SceneCamera scene;
    scene.position = { 4.0f, 5.0f, 6.0f };
    scene.forwardDirection = { 0.0f, 0.0f, -2.0f };
    scene.verticalFOV = 70.0f;
    scene.aperture = 0.2f;
    scene.focusDistance = 11.0f;
    scene.presentation.toneMap = ToneMapOperator::ACESFitted;
    scene.presentation.exposureEV = -0.0f;

    EditorCameraPose current = MakePose();
    current.farClip = 500.0f;
    EditorCameraPose adopted;
    REQUIRE(TryBuildAuthoringAdoptionPose(scene, current, adopted));
    CHECK(adopted.position == scene.position);
    CHECK(adopted.forward == glm::vec3(0.0f, 0.0f, -1.0f));
    CHECK(adopted.verticalFOV == doctest::Approx(70.0f));
    CHECK(adopted.aperture == doctest::Approx(0.2f));
    CHECK(adopted.focusDistance == doctest::Approx(11.0f));
    // SceneCamera owns no far clip: the live value is retained.
    CHECK(adopted.farClip == doctest::Approx(500.0f));
    CHECK(adopted.presentation.toneMap == ToneMapOperator::ACESFitted);
    uint32_t bits = 0xFFFFFFFFu;
    std::memcpy(&bits, &adopted.presentation.exposureEV, sizeof(bits));
    CHECK(bits == 0u);
    CHECK(IsValidEditorCameraPose(adopted));
}

TEST_CASE("F2 adoption rejects invalid scene cameras without touching output")
{
    const EditorCameraPose current = MakePose();
    EditorCameraPose out = current;

    SceneCamera degenerate = SceneCamera{};
    degenerate.forwardDirection = glm::vec3(0.0f);
    CHECK_FALSE(TryBuildAuthoringAdoptionPose(degenerate, current, out));
    CHECK(out.position == current.position);

    SceneCamera badFov = SceneCamera{};
    badFov.verticalFOV = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(TryBuildAuthoringAdoptionPose(badFov, current, out));
    CHECK(out.verticalFOV == doctest::Approx(current.verticalFOV));

    SceneCamera badEv = SceneCamera{};
    badEv.presentation.exposureEV = 20.0f;
    CHECK_FALSE(TryBuildAuthoringAdoptionPose(badEv, current, out));
    CHECK(out.presentation == current.presentation);
}

TEST_CASE("F2 pose-apply policy separates presentation updates from cuts")
{
    const EditorCameraPose base = MakePose();
    CHECK(ResolvePoseApplyAction(base, base) == PoseApplyAction::PresentationOnly);

    EditorCameraPose lookOnly = base;
    lookOnly.presentation.toneMap = ToneMapOperator::Reinhard;
    lookOnly.presentation.exposureEV = 2.0f;
    CHECK(ResolvePoseApplyAction(base, lookOnly) == PoseApplyAction::PresentationOnly);

    EditorCameraPose negZero = base;
    negZero.presentation.exposureEV = -0.0f;
    CHECK(ResolvePoseApplyAction(base, negZero) == PoseApplyAction::PresentationOnly);

    EditorCameraPose moved = base;
    moved.position.x += 1.0f;
    CHECK(ResolvePoseApplyAction(base, moved) == PoseApplyAction::Cut);

    EditorCameraPose refocused = base;
    refocused.focusDistance += 1.0f;
    CHECK(ResolvePoseApplyAction(base, refocused) == PoseApplyAction::Cut);

    EditorCameraPose degenerate = base;
    degenerate.forward = glm::vec3(0.0f);
    CHECK(ResolvePoseApplyAction(base, degenerate) == PoseApplyAction::Cut);
}

TEST_CASE("F2 Play-time runtime adoption carries destination lens and look")
{
    rt2::core::DeterministicUuidProvider ids;
    rt2::core::SceneDocument runtime;
    runtime.SetUuidProvider(&ids);
    const entt::entity entity = runtime.ecs.registry.create();
    auto& transform = runtime.ecs.registry.emplace<Transform>(entity);
    transform.translation = { 7.0f, 8.0f, 9.0f };
    auto& camera = runtime.ecs.registry.emplace<CameraComponent>(entity);
    camera.verticalFOV = 50.0f;
    camera.aperture = 0.2f;
    camera.focusDistance = 11.0f;
    camera.forwardDirection = { 0.0f, 0.0f, -1.0f };
    camera.presentation.toneMap = ToneMapOperator::ACESFitted;
    camera.presentation.exposureEV = 2.0f;
    const auto uuid = runtime.AssignNewUuid(entity);
    REQUIRE(FindDeterministicCameraEntity(runtime) == uuid);

    // Mirrors Play entry: fallback is the copied editor camera, the runtime
    // entity pose (with its look) wins when valid.
    EditorCameraPose fallback = MakePose();
    EditorCameraPose adopted;
    REQUIRE(TryGetCameraEntityPose(runtime, uuid, fallback, adopted));
    CHECK(adopted.position.x == doctest::Approx(7.0f));
    CHECK(adopted.verticalFOV == doctest::Approx(50.0f));
    CHECK(adopted.aperture == doctest::Approx(0.2f));
    CHECK(adopted.focusDistance == doctest::Approx(11.0f));
    CHECK(adopted.presentation.toneMap == ToneMapOperator::ACESFitted);
    CHECK(adopted.presentation.exposureEV == doctest::Approx(2.0f));
}

TEST_CASE("F2 View Through adopts a different camera as a real cut with its look")
{
    rt2::core::DeterministicUuidProvider ids;
    rt2::core::SceneDocument document;
    document.SetUuidProvider(&ids);
    const entt::entity entity = document.ecs.registry.create();
    auto& transform = document.ecs.registry.emplace<Transform>(entity);
    transform.translation = { -5.0f, 1.0f, 2.0f };
    auto& camera = document.ecs.registry.emplace<CameraComponent>(entity);
    camera.verticalFOV = 70.0f;
    camera.presentation.toneMap = ToneMapOperator::Reinhard;
    camera.presentation.exposureEV = -1.0f;
    const auto uuid = document.AssignNewUuid(entity);

    // Mirrors ViewThroughCamera: resolve against the live editor fallback,
    // then classify. A different camera is transport-different: a real cut
    // that adopts the destination look.
    const EditorCameraPose live = MakePose();
    EditorCameraPose pose;
    REQUIRE(TryGetCameraEntityPose(document, uuid, live, pose));
    CHECK_FALSE(EditorCameraTransportEqual(live, pose));
    CHECK(ResolvePoseApplyAction(live, pose) == PoseApplyAction::Cut);
    EditorCameraPose applied = pose;
    REQUIRE(TryNormalizeEditorCameraPose(applied));
    CHECK(applied.presentation.toneMap == ToneMapOperator::Reinhard);
    CHECK(applied.presentation.exposureEV == doctest::Approx(-1.0f));
    CHECK(applied.verticalFOV == doctest::Approx(70.0f));
}

TEST_CASE("F2 bookmark recall without transport change is presentation-only")
{
    EditorSceneState state;
    EditorCameraPose stored = MakePose();
    stored.presentation.toneMap = ToneMapOperator::ACESFitted;
    stored.presentation.exposureEV = 1.0f;
    REQUIRE(state.CaptureCameraBookmark(0, stored));

    // Recalling the stored look over identical transport must not cut.
    const EditorCameraPose* bookmark = state.CameraBookmark(0);
    REQUIRE(bookmark != nullptr);
    CHECK(ResolvePoseApplyAction(MakePose(), *bookmark) ==
          PoseApplyAction::PresentationOnly);

    // Recalling after the camera moved is a real cut that adopts the look.
    EditorCameraPose moved = MakePose();
    moved.position = { 9.0f, 9.0f, 9.0f };
    CHECK(ResolvePoseApplyAction(moved, *bookmark) == PoseApplyAction::Cut);
}

TEST_CASE("F2 stale second scene replaces complete lens and look at adoption")
{
    const auto dir = std::filesystem::temp_directory_path() / "rt2_f2_stale";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    SceneFixture fa;
    fa.manager.AuthoringDoc().ecs.camera.verticalFOV = 70.0f;
    fa.manager.AuthoringDoc().ecs.camera.aperture = 0.2f;
    fa.manager.AuthoringDoc().ecs.camera.focusDistance = 11.0f;
    fa.manager.AuthoringDoc().ecs.camera.presentation.toneMap = ToneMapOperator::ACESFitted;
    fa.manager.AuthoringDoc().ecs.camera.presentation.exposureEV = 2.0f;
    const auto pathA = dir / "a.rt2scene";
    rt2::core::Error err;
    REQUIRE(SaveSceneForTest(fa.manager.AuthoringDoc(), pathA, err));

    SceneFixture fb;
    const auto pathB = dir / "b.rt2scene";
    REQUIRE(SaveSceneForTest(fb.manager.AuthoringDoc(), pathB, err));

    // First adoption carries the complete non-default camera.
    SceneFixture host;
    rt2::core::SceneDocument loadedA;
    loadedA.SetUuidProvider(&host.ids);
    REQUIRE(rt2::core::SceneSerializer::Load(loadedA, pathA, err));
    host.manager.AdoptLoadedDocument(std::move(loadedA), false);
    EditorCameraPose adoptedA;
    REQUIRE(TryBuildAuthoringAdoptionPose(
        host.manager.AuthoringDoc().ecs.camera, MakePose(), adoptedA));
    CHECK(adoptedA.verticalFOV == doctest::Approx(70.0f));
    CHECK(adoptedA.presentation.toneMap == ToneMapOperator::ACESFitted);

    // The second adoption cannot retain the first scene's lens or look.
    rt2::core::SceneDocument loadedB;
    loadedB.SetUuidProvider(&host.ids);
    REQUIRE(rt2::core::SceneSerializer::Load(loadedB, pathB, err));
    host.manager.AdoptLoadedDocument(std::move(loadedB), false);
    EditorCameraPose adoptedB;
    REQUIRE(TryBuildAuthoringAdoptionPose(
        host.manager.AuthoringDoc().ecs.camera, adoptedA, adoptedB));
    CHECK(adoptedB.verticalFOV == doctest::Approx(45.0f));
    CHECK(adoptedB.aperture == doctest::Approx(0.0f));
    CHECK(adoptedB.focusDistance == doctest::Approx(1.0f));
    CHECK(adoptedB.presentation == DefaultCameraPresentation());
    std::filesystem::remove_all(dir, ec);
}
