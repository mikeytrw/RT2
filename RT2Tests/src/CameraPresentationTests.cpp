#include <doctest/doctest.h>

#include "CameraPresentation.h"
#include "ColorTransfer.h"
#include "EditorCameraWorkflow.h"
#include "EditorSceneState.h"
#include "ISceneRenderBridge.h"
#include "SceneManager.h"
#include "ToneMapMath.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace
{
class RecordingBridge final : public rt2::core::ISceneRenderBridge
{
public:
    int temporalReset = 0;
    void FullSync(GPUSceneData&) override {}
    void MaterialSync(GPUSceneData&) override {}
    void TransformSync(GPUSceneData&) override {}
    void ResetTemporalState() override { ++temporalReset; }
    void RequestRender() override {}
};

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

bool Convert(float r, float g, float b, float a, ToneMapOperator op, float ev, uint8_t out[4])
{
    return ToneMapMath::ConvertHdrPixelToDisplay8(r, g, b, a, op, CameraExposureMultiplier(ev), out);
}
} // namespace

TEST_CASE("camera presentation defaults to AgX at 0 EV with stable tokens")
{
    const CameraPresentation def = DefaultCameraPresentation();
    CHECK(def.toneMap == ToneMapOperator::AgX);
    CHECK(def.exposureEV == doctest::Approx(0.0f));
    CHECK(std::strcmp(ToneMapOperatorName(ToneMapOperator::AgX), "agx") == 0);
    CHECK(std::strcmp(ToneMapOperatorName(ToneMapOperator::ACESFitted), "aces") == 0);
    CHECK(std::strcmp(ToneMapOperatorName(ToneMapOperator::Reinhard), "reinhard") == 0);
    // Out-of-range enums stay observable; they must not silently become AgX.
    CHECK(std::strcmp(ToneMapOperatorName(static_cast<ToneMapOperator>(99)), "unknown") == 0);
    // Integer numbering is shared with the GPU push constants; do not renumber.
    CHECK(static_cast<int>(ToneMapOperator::AgX) == 0);
    CHECK(static_cast<int>(ToneMapOperator::ACESFitted) == 1);
    CHECK(static_cast<int>(ToneMapOperator::Reinhard) == 2);
}

TEST_CASE("camera presentation parsing is exact and case-sensitive")
{
    ToneMapOperator op = ToneMapOperator::Reinhard;
    CHECK(TryParseToneMapOperator("agx", op));
    CHECK(op == ToneMapOperator::AgX);
    CHECK(TryParseToneMapOperator("aces", op));
    CHECK(op == ToneMapOperator::ACESFitted);
    CHECK(TryParseToneMapOperator("reinhard", op));
    CHECK(op == ToneMapOperator::Reinhard);
    // Rejections leave the output untouched so callers cannot partially adopt.
    CHECK_FALSE(TryParseToneMapOperator("AGX", op));
    CHECK_FALSE(TryParseToneMapOperator("AgX", op));
    CHECK_FALSE(TryParseToneMapOperator("", op));
    CHECK_FALSE(TryParseToneMapOperator(nullptr, op));
    CHECK_FALSE(TryParseToneMapOperator("aces-fitted", op));
    CHECK(op == ToneMapOperator::Reinhard);
}

TEST_CASE("camera presentation EV validation and canonicalization")
{
    CHECK(IsValidExposureEV(-8.0f));
    CHECK(IsValidExposureEV(8.0f));
    CHECK(IsValidExposureEV(0.0f));
    CHECK(IsValidExposureEV(-0.0f));
    CHECK_FALSE(IsValidExposureEV(-8.001f));
    CHECK_FALSE(IsValidExposureEV(8.001f));
    CHECK_FALSE(IsValidExposureEV(std::numeric_limits<float>::quiet_NaN()));
    CHECK_FALSE(IsValidExposureEV(std::numeric_limits<float>::infinity()));

    CameraPresentation badOp;
    badOp.toneMap = static_cast<ToneMapOperator>(7);
    CHECK_FALSE(IsValidCameraPresentation(badOp));
    CameraPresentation badEv;
    badEv.exposureEV = 9.0f;
    CHECK_FALSE(IsValidCameraPresentation(badEv));

    // -0.0f is valid but canonicalizes to +0.0f bit pattern.
    CameraPresentation negZero;
    negZero.exposureEV = -0.0f;
    CHECK(IsValidCameraPresentation(negZero));
    const CameraPresentation canon = CanonicalCameraPresentation(negZero);
    CHECK(canon.exposureEV == 0.0f);
    uint32_t bits = 0;
    std::memcpy(&bits, &canon.exposureEV, sizeof(bits));
    CHECK(bits == 0u);

    CameraPresentation rejected = badEv;
    CHECK_FALSE(TryCanonicalizeCameraPresentation(rejected));
    CHECK(rejected.exposureEV == doctest::Approx(9.0f));

    CHECK(CameraExposureMultiplier(0.0f) == doctest::Approx(1.0f));
    CHECK(CameraExposureMultiplier(1.0f) == doctest::Approx(2.0f));
    CHECK(CameraExposureMultiplier(-1.0f) == doctest::Approx(0.5f));
    CHECK(CameraExposureMultiplier(8.0f) == doctest::Approx(256.0f));
}

TEST_CASE("legacy Reinhard at 0 EV retains the existing ColorTransfer branch")
{
    // Production oracle: per-channel Reinhard + single sRGB encode.
    const float samples[] = { 0.0f, 0.18f, 0.5f, 1.0f, 4.0f };
    for (float v : samples)
    {
        uint8_t out[4] = {};
        REQUIRE(Convert(v, v, v, 1.0f, ToneMapOperator::Reinhard, 0.0f, out));
        const uint8_t expected = ColorTransfer::LinearToSRGB8(ColorTransfer::Reinhard(v));
        CHECK(out[0] == expected);
        CHECK(out[1] == expected);
        CHECK(out[2] == expected);
        CHECK(out[3] == 255);
    }
    // Known bytes from the existing headless conversion contract.
    uint8_t gray[4] = {};
    REQUIRE(Convert(0.18f, 0.18f, 0.18f, 1.0f, ToneMapOperator::Reinhard, 0.0f, gray));
    CHECK(gray[0] == 109);
}

TEST_CASE("reference math stays finite over black, gray, bright and saturated inputs")
{
    const ToneMapOperator ops[] = {
        ToneMapOperator::AgX, ToneMapOperator::ACESFitted, ToneMapOperator::Reinhard
    };
    struct Sample { float r, g, b; };
    const Sample samples[] = {
        { 0.0f, 0.0f, 0.0f },
        { 0.18f, 0.18f, 0.18f },
        { 4.0f, 4.0f, 4.0f },
        { 4.0f, 0.5f, 0.1f },
        { 0.1f, 0.5f, 4.0f },
        { 16.0f, 8.0f, 2.0f },
    };
    const float evs[] = { -8.0f, 0.0f, 8.0f };
    for (ToneMapOperator op : ops)
    {
        for (const Sample& s : samples)
        {
            for (float ev : evs)
            {
                uint8_t out[4] = {};
                INFO("op=" << static_cast<int>(op) << " ev=" << ev
                           << " rgb=" << s.r << "," << s.g << "," << s.b);
                REQUIRE(Convert(s.r, s.g, s.b, 1.0f, op, ev, out));
                CHECK(std::isfinite(static_cast<float>(out[0])));
            }
        }
    }
    // Exposure brightens monotonically for each operator on middle gray.
    for (ToneMapOperator op : ops)
    {
        uint8_t dark[4] = {}, bright[4] = {};
        REQUIRE(Convert(0.18f, 0.18f, 0.18f, 1.0f, op, -2.0f, dark));
        REQUIRE(Convert(0.18f, 0.18f, 0.18f, 1.0f, op, 2.0f, bright));
        CHECK(bright[0] > dark[0]);
    }
    // Operators disagree on bright highlights (sanity, not a quality bar).
    uint8_t agx[4] = {}, reinhard[4] = {};
    REQUIRE(Convert(4.0f, 4.0f, 4.0f, 1.0f, ToneMapOperator::AgX, 0.0f, agx));
    REQUIRE(Convert(4.0f, 4.0f, 4.0f, 1.0f, ToneMapOperator::Reinhard, 0.0f, reinhard));
    CHECK(agx[0] != reinhard[0]);
}

TEST_CASE("tone-map pixel guards nonfinite input and exposure loudly")
{
    float r = 0.0f, g = 0.0f, b = 0.0f;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    CHECK_FALSE(ToneMapMath::ToneMapPixel(nan, 0.5f, 0.5f, ToneMapOperator::AgX, 1.0f, r, g, b));
    CHECK_FALSE(ToneMapMath::ToneMapPixel(0.5f, inf, 0.5f, ToneMapOperator::ACESFitted, 1.0f, r, g, b));
    CHECK_FALSE(ToneMapMath::ToneMapPixel(0.5f, 0.5f, -inf, ToneMapOperator::Reinhard, 1.0f, r, g, b));
    // Invalid exposure multipliers fail instead of scaling by garbage.
    CHECK_FALSE(ToneMapMath::ToneMapPixel(0.5f, 0.5f, 0.5f, ToneMapOperator::AgX, 0.0f, r, g, b));
    CHECK_FALSE(ToneMapMath::ToneMapPixel(0.5f, 0.5f, 0.5f, ToneMapOperator::AgX, -1.0f, r, g, b));
    CHECK_FALSE(ToneMapMath::ToneMapPixel(0.5f, 0.5f, 0.5f, ToneMapOperator::AgX, nan, r, g, b));
    CHECK_FALSE(ToneMapMath::ToneMapPixel(0.5f, 0.5f, 0.5f, ToneMapOperator::AgX, inf, r, g, b));
    // Negative finite HDR clamps to black instead of failing.
    uint8_t out[4] = {};
    REQUIRE(Convert(-1.0f, -1.0f, -1.0f, 1.0f, ToneMapOperator::AgX, 0.0f, out));
    uint8_t black[4] = {};
    REQUIRE(Convert(0.0f, 0.0f, 0.0f, 1.0f, ToneMapOperator::AgX, 0.0f, black));
    CHECK(out[0] == black[0]);
    CHECK(out[1] == black[1]);
    CHECK(out[2] == black[2]);
}

TEST_CASE("alpha is never tone-mapped: clamps finite, replaces nonfinite with 1")
{
    uint8_t out[4] = {};
    REQUIRE(Convert(0.5f, 0.5f, 0.5f, 0.5f, ToneMapOperator::AgX, 0.0f, out));
    CHECK(out[3] == 128);
    REQUIRE(Convert(0.5f, 0.5f, 0.5f, 2.0f, ToneMapOperator::AgX, 0.0f, out));
    CHECK(out[3] == 255);
    REQUIRE(Convert(0.5f, 0.5f, 0.5f, -1.0f, ToneMapOperator::AgX, 0.0f, out));
    CHECK(out[3] == 0);
    REQUIRE(Convert(0.5f, 0.5f, 0.5f, std::numeric_limits<float>::quiet_NaN(),
                    ToneMapOperator::AgX, 0.0f, out));
    CHECK(out[3] == 255);
}

TEST_CASE("editor pose validation covers presentation and normalizes -0 EV")
{
    EditorCameraPose pose = MakePose();
    CHECK(IsValidEditorCameraPose(pose));

    EditorCameraPose badEv = pose;
    badEv.presentation.exposureEV = 20.0f;
    CHECK_FALSE(IsValidEditorCameraPose(badEv));

    EditorCameraPose badOp = pose;
    badOp.presentation.toneMap = static_cast<ToneMapOperator>(-1);
    CHECK_FALSE(IsValidEditorCameraPose(badOp));

    EditorCameraPose negZero = pose;
    negZero.presentation.exposureEV = -0.0f;
    REQUIRE(TryNormalizeEditorCameraPose(negZero));
    uint32_t bits = 0xFFFFFFFFu;
    std::memcpy(&bits, &negZero.presentation.exposureEV, sizeof(bits));
    CHECK(bits == 0u);

    EditorCameraPose invalid = pose;
    invalid.presentation.exposureEV = std::numeric_limits<float>::infinity();
    CHECK_FALSE(TryNormalizeEditorCameraPose(invalid));
}

TEST_CASE("transport comparator excludes presentation so look edits never read as cuts")
{
    const EditorCameraPose base = MakePose();

    EditorCameraPose same = base;
    CHECK(EditorCameraTransportEqual(base, same));

    EditorCameraPose evOnly = base;
    evOnly.presentation.exposureEV = 2.0f;
    CHECK(EditorCameraTransportEqual(base, evOnly));

    EditorCameraPose opOnly = base;
    opOnly.presentation.toneMap = ToneMapOperator::Reinhard;
    CHECK(EditorCameraTransportEqual(base, opOnly));

    EditorCameraPose negZero = base;
    negZero.presentation.exposureEV = -0.0f;
    CHECK(EditorCameraTransportEqual(base, negZero));

    // Unnormalized but identical direction is not a cut.
    EditorCameraPose scaled = base;
    scaled.forward = { 0.0f, 0.0f, -5.0f };
    CHECK(EditorCameraTransportEqual(base, scaled));

    EditorCameraPose moved = base;
    moved.position.x += 0.5f;
    CHECK_FALSE(EditorCameraTransportEqual(base, moved));

    EditorCameraPose turned = base;
    turned.forward = glm::normalize(glm::vec3(0.1f, 0.0f, -1.0f));
    CHECK_FALSE(EditorCameraTransportEqual(base, turned));

    EditorCameraPose fov = base;
    fov.verticalFOV += 1.0f;
    CHECK_FALSE(EditorCameraTransportEqual(base, fov));

    EditorCameraPose aperture = base;
    aperture.aperture = 0.5f;
    CHECK_FALSE(EditorCameraTransportEqual(base, aperture));

    EditorCameraPose focus = base;
    focus.focusDistance += 1.0f;
    CHECK_FALSE(EditorCameraTransportEqual(base, focus));

    EditorCameraPose far = base;
    far.farClip *= 2.0f;
    CHECK_FALSE(EditorCameraTransportEqual(base, far));

    EditorCameraPose degenerate = base;
    degenerate.forward = glm::vec3(0.0f);
    CHECK_FALSE(EditorCameraTransportEqual(base, degenerate));
}

TEST_CASE("presentation-only change must not route through the cut API")
{
    RecordingBridge bridge;
    EditorCameraPose before = MakePose();
    EditorCameraPose after = before;
    after.presentation.toneMap = ToneMapOperator::ACESFitted;
    after.presentation.exposureEV = -1.0f;

    // Policy: transport-equal means no temporal reset. The cut API itself
    // always resets when called, so routing a look slider through it would
    // be a regression this comparator exists to prevent.
    REQUIRE(EditorCameraTransportEqual(before, after));
    CHECK(bridge.temporalReset == 0);

    EditorCameraPose moved = before;
    moved.position.x += 1.0f;
    REQUIRE_FALSE(EditorCameraTransportEqual(before, moved));
    REQUIRE(ApplyEditorCameraCut(moved, bridge,
        [](const EditorCameraPose&) { return true; }));
    CHECK(bridge.temporalReset == 1);
}

TEST_CASE("entity pose adoption carries the destination presentation")
{
    rt2::core::DeterministicUuidProvider ids;
    rt2::core::SceneDocument document;
    document.SetUuidProvider(&ids);
    const entt::entity entity = document.ecs.registry.create();
    auto& transform = document.ecs.registry.emplace<Transform>(entity);
    transform.translation = { 7.0f, 8.0f, 9.0f };
    auto& camera = document.ecs.registry.emplace<CameraComponent>(entity);
    camera.verticalFOV = 50.0f;
    camera.aperture = 0.2f;
    camera.focusDistance = 11.0f;
    camera.forwardDirection = { 0.0f, 0.0f, -1.0f };
    camera.presentation.toneMap = ToneMapOperator::ACESFitted;
    camera.presentation.exposureEV = 2.0f;
    const auto uuid = document.AssignNewUuid(entity);

    EditorCameraPose fallback = MakePose(); // AgX/0 editor look
    REQUIRE(fallback.presentation.toneMap == ToneMapOperator::AgX);
    EditorCameraPose adopted;
    REQUIRE(TryGetCameraEntityPose(document, uuid, fallback, adopted));
    CHECK(adopted.presentation.toneMap == ToneMapOperator::ACESFitted);
    CHECK(adopted.presentation.exposureEV == doctest::Approx(2.0f));
    CHECK(adopted.position.x == doctest::Approx(7.0f));
}

TEST_CASE("focus, frame and bookmarks preserve the camera look")
{
    EditorCameraPose current = MakePose();
    current.presentation.toneMap = ToneMapOperator::Reinhard;
    current.presentation.exposureEV = 1.5f;

    EditorSelectionBounds bounds;
    bounds.minimum = { 4.5f, -0.5f, -0.5f };
    bounds.maximum = { 5.5f, 0.5f, 0.5f };
    bounds.valid = true;
    EditorCameraPose focused;
    REQUIRE(TryFocusEditorCamera(current, bounds, 0.1f, focused));
    CHECK(focused.presentation == current.presentation);

    EditorCameraPose framed;
    REQUIRE(TryFrameEditorCamera(current, bounds, {}, framed));
    CHECK(framed.presentation == current.presentation);

    EditorSceneState state;
    REQUIRE(state.CaptureCameraBookmark(0, current));
    const EditorCameraPose* stored = state.CameraBookmark(0);
    REQUIRE(stored != nullptr);
    CHECK(stored->presentation == current.presentation);
}

TEST_CASE("align camera to view carries presentation into the component")
{
    rt2::core::DeterministicUuidProvider ids;
    SceneManager manager;
    manager.SetUuidProvider(&ids);
    const auto cameraUuid = manager.CreateEmpty("Camera").affectedEntities.front();
    const entt::entity entity = manager.FindEntityByUuid(cameraUuid);
    manager.GetECS().registry.emplace<CameraComponent>(entity);

    EditorCameraPose pose = MakePose();
    pose.presentation.toneMap = ToneMapOperator::Reinhard;
    pose.presentation.exposureEV = -2.0f;
    REQUIRE(manager.AlignCameraEntityToView(cameraUuid, pose).success);
    const auto& stored = manager.GetECS().registry.get<CameraComponent>(entity);
    CHECK(stored.presentation.toneMap == ToneMapOperator::Reinhard);
    CHECK(stored.presentation.exposureEV == doctest::Approx(-2.0f));

    EditorCameraPose readBack;
    REQUIRE(TryGetCameraEntityPose(manager.AuthoringDoc(), cameraUuid, MakePose(), readBack));
    CHECK(readBack.presentation == stored.presentation);
}

TEST_CASE("F4 extreme finite HDR stays finite through every operator and EV")
{
    using namespace ToneMapMath;
    const float huge[] = {
        1.0e10f, 1.0e20f, 1.0e30f, 1.0e37f,
        std::numeric_limits<float>::max(),
    };
    const ToneMapOperator ops[] = {
        ToneMapOperator::AgX, ToneMapOperator::ACESFitted,
        ToneMapOperator::Reinhard,
    };
    for (float v : huge)
    {
        for (ToneMapOperator op : ops)
        {
            for (float ev : {-8.0f, 0.0f, 8.0f})
            {
                float r = 0.0f, g = 0.0f, b = 0.0f;
                INFO("op=" << static_cast<int>(op) << " v=" << v << " ev=" << ev);
                REQUIRE(ToneMapPixel(v, v, v, op, CameraExposureMultiplier(ev), r, g, b));
                CHECK(std::isfinite(r));
                CHECK(std::isfinite(g));
                CHECK(std::isfinite(b));
                uint8_t out[4] = {};
                REQUIRE(ConvertHdrPixelToDisplay8(v, v, v, 1.0f, op,
                    CameraExposureMultiplier(ev), out));
            }
        }
    }
    // Saturated white: extreme brights land on display white (255) on
    // every channel instead of failing.
    uint8_t white[4] = {};
    REQUIRE(ConvertHdrPixelToDisplay8(std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
        1.0f, ToneMapOperator::Reinhard, 256.0f, white));
    CHECK(white[0] == 255);
    CHECK(white[1] == 255);
    CHECK(white[2] == 255);
    uint8_t hugeWhite[4] = {};
    REQUIRE(ConvertHdrPixelToDisplay8(1.0e37f, 1.0e37f, 1.0e37f, 1.0f,
        ToneMapOperator::ACESFitted, 256.0f, hugeWhite));
    CHECK(hugeWhite[0] == 255);
    CHECK(hugeWhite[1] == 255);
    CHECK(hugeWhite[2] == 255);
}

TEST_CASE("F4 ACES fit saturates invisibly at the high-range bound")
{
    using namespace ToneMapMath;
    // Inputs above the bound clamp to it: identical bytes, still near-white.
    uint8_t atBound[4] = {}, aboveBound[4] = {};
    REQUIRE(ConvertHdrPixelToDisplay8(1.0e6f, 1.0e6f, 1.0e6f, 1.0f,
        ToneMapOperator::ACESFitted, 1.0f, atBound));
    REQUIRE(ConvertHdrPixelToDisplay8(1.0e7f, 1.0e7f, 1.0e7f, 1.0f,
        ToneMapOperator::ACESFitted, 1.0f, aboveBound));
    CHECK(atBound[0] == aboveBound[0]);
    CHECK(atBound[1] == aboveBound[1]);
    CHECK(atBound[2] == aboveBound[2]);
    // Just below the bound the unclamped path stays within a wide visual
    // tolerance of the bound value, so normal-range output is unchanged.
    uint8_t below[4] = {};
    REQUIRE(ConvertHdrPixelToDisplay8(1.0e5f, 1.0e5f, 1.0e5f, 1.0f,
        ToneMapOperator::ACESFitted, 1.0f, below));
    CHECK(std::abs(static_cast<int>(below[0]) - static_cast<int>(atBound[0])) <= 30);
}

TEST_CASE("F5 AgX black is exact and the log floor preserves the toe")
{
    using namespace ToneMapMath;
    // The pinned floor equals exp2 of the toe bound (bit-verified literal).
    CHECK(static_cast<double>(std::exp2(static_cast<double>(Detail::kAgXMinEV))) ==
          doctest::Approx(static_cast<double>(Detail::kAgXLogFloor)));
    CHECK(Detail::kAgXLogFloor > 0.0f);

    // Black converts to exact black through AgX at 0 EV on every channel.
    uint8_t black[4] = {};
    REQUIRE(ConvertHdrPixelToDisplay8(0.0f, 0.0f, 0.0f, 1.0f,
        ToneMapOperator::AgX, 1.0f, black));
    CHECK(black[0] == 0);
    CHECK(black[1] == 0);
    CHECK(black[2] == 0);
    CHECK(black[3] == 255);

    // Floor-neighborhood inputs stay finite and dark, never NaN.
    for (float v : {1.0e-10f, 1.0e-6f, Detail::kAgXLogFloor, 1.0e-3f})
    {
        float r = 0.0f, g = 0.0f, b = 0.0f;
        INFO("v=" << v);
        REQUIRE(ToneMapPixel(v, v, v, ToneMapOperator::AgX, 1.0f, r, g, b));
        CHECK(std::isfinite(r));
        uint8_t out[4] = {};
        REQUIRE(ConvertHdrPixelToDisplay8(v, v, v, 1.0f,
            ToneMapOperator::AgX, 1.0f, out));
        CHECK(out[0] <= 8);
    }
}
