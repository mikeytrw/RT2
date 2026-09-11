#include <doctest/doctest.h>

#include "CameraPresentation.h"
#include "RRFeatureLifecycle.h"
#include "ToneMapMath.h"
#include "TonemapPushConstants.h"

#include <cstddef>
#include <cstdint>

namespace
{
struct TestCapture
{
    uint64_t submitSequence = 0;
    int toneOperator = 0;
    bool valid = false;
};
} // namespace

TEST_CASE("tone-map push constants are a 16-byte block matching the GLSL layout")
{
    CHECK(sizeof(TonemapPushConstants) == 16);
    CHECK(offsetof(TonemapPushConstants, toneOperator) == 0);
    CHECK(offsetof(TonemapPushConstants, exposureMultiplier) == 4);
    CHECK(offsetof(TonemapPushConstants, legacyDiagnostic) == 8);
    CHECK(offsetof(TonemapPushConstants, padding) == 12);
    // The integer numbering is shared with the shader branch and the
    // CameraPresentation contract; never renumber either side alone.
    CHECK(static_cast<int32_t>(ToneMapOperator::AgX) == 0);
    CHECK(static_cast<int32_t>(ToneMapOperator::ACESFitted) == 1);
    CHECK(static_cast<int32_t>(ToneMapOperator::Reinhard) == 2);
}

TEST_CASE("push-constant builder resolves beauty looks from the camera")
{
    TonemapPushConstants pc{};
    CameraPresentation agx;
    REQUIRE(TryBuildTonemapPushConstants(agx, false, pc));
    CHECK(pc.toneOperator == 0);
    CHECK(pc.exposureMultiplier == doctest::Approx(1.0f));
    CHECK(pc.legacyDiagnostic == 0);
    CHECK(pc.padding == doctest::Approx(0.0f));

    CameraPresentation aces;
    aces.toneMap = ToneMapOperator::ACESFitted;
    aces.exposureEV = 2.0f;
    REQUIRE(TryBuildTonemapPushConstants(aces, false, pc));
    CHECK(pc.toneOperator == 1);
    CHECK(pc.exposureMultiplier == doctest::Approx(4.0f));

    CameraPresentation legacy;
    legacy.toneMap = ToneMapOperator::Reinhard;
    legacy.exposureEV = -8.0f;
    REQUIRE(TryBuildTonemapPushConstants(legacy, false, pc));
    CHECK(pc.toneOperator == 2);
    CHECK(pc.exposureMultiplier == doctest::Approx(1.0f / 256.0f));

    // Negative zero canonicalizes to a +1.0x multiplier, never a cut.
    CameraPresentation negZero;
    negZero.exposureEV = -0.0f;
    REQUIRE(TryBuildTonemapPushConstants(negZero, false, pc));
    CHECK(pc.exposureMultiplier == doctest::Approx(1.0f));

    // Invalid input is rejected without touching the output.
    TonemapPushConstants sentinel{ 7, 7.0f, 7, 7.0f };
    CameraPresentation badEv;
    badEv.exposureEV = 20.0f;
    CHECK_FALSE(TryBuildTonemapPushConstants(badEv, false, sentinel));
    CHECK(sentinel.toneOperator == 7);
    CameraPresentation badOp;
    badOp.toneMap = static_cast<ToneMapOperator>(9);
    CHECK_FALSE(TryBuildTonemapPushConstants(badOp, false, sentinel));
    CHECK(sentinel.toneOperator == 7);
}

TEST_CASE("diagnostic views keep the legacy Reinhard-at-0EV mapping")
{
    CHECK_FALSE(IsTonemapDiagnosticView(-1));
    CHECK(IsTonemapDiagnosticView(0));
    CHECK(IsTonemapDiagnosticView(5));
    CHECK(IsTonemapDiagnosticView(19));

    // Any camera look, including bright ACES exposures, resolves to the
    // same legacy constants in a diagnostic view.
    CameraPresentation look;
    look.toneMap = ToneMapOperator::ACESFitted;
    look.exposureEV = 3.0f;
    TonemapPushConstants pc{};
    REQUIRE(TryBuildTonemapPushConstants(look, true, pc));
    CHECK(pc.toneOperator == static_cast<int32_t>(ToneMapOperator::Reinhard));
    CHECK(pc.exposureMultiplier == doctest::Approx(1.0f));
    CHECK(pc.legacyDiagnostic == 1);

    // The readback path converts diagnostic captures through those same
    // resolved constants, never through the camera look.
    uint8_t out[4] = {};
    REQUIRE(ToneMapMath::ConvertHdrPixelToDisplay8(0.5f, 0.5f, 0.5f, 1.0f,
        static_cast<ToneMapOperator>(pc.toneOperator), pc.exposureMultiplier, out));
    const uint8_t expected = ColorTransfer::LinearToSRGB8(ColorTransfer::Reinhard(0.5f));
    CHECK(out[0] == expected);
    CHECK(out[1] == expected);
    CHECK(out[2] == expected);
}

TEST_CASE("capture tracker publishes only fence-proven pairs")
{
    SnapshotTracker<TestCapture, 2> tracker;
    CHECK_FALSE(tracker.ReapCompleted(0).has_value());

    TestCapture first;
    first.submitSequence = 1;
    first.toneOperator = 0;
    first.valid = true;
    tracker.Submit(0, first);
    // Submission alone publishes nothing on another slot.
    CHECK_FALSE(tracker.ReapCompleted(1).has_value());
    // The submitted slot promotes exactly once.
    auto promoted = tracker.ReapCompleted(0);
    REQUIRE(promoted.has_value());
    CHECK(promoted->submitSequence == 1);
    CHECK_FALSE(tracker.ReapCompleted(0).has_value());
}

TEST_CASE("capture drain keeps the newest pair and consumes the rest")
{
    SnapshotTracker<TestCapture, 2> tracker;
    CHECK_FALSE(tracker.ReapNewest().has_value());

    TestCapture older;
    older.submitSequence = 1;
    older.toneOperator = 0;
    older.valid = true;
    TestCapture newer;
    newer.submitSequence = 2;
    newer.toneOperator = 1;
    newer.valid = true;
    tracker.Submit(0, older);
    tracker.Submit(1, newer);

    auto winner = tracker.ReapNewest();
    REQUIRE(winner.has_value());
    CHECK(winner->submitSequence == 2);
    CHECK(winner->toneOperator == 1);
    // Both slots were consumed; nothing stale can promote later.
    CHECK_FALSE(tracker.ReapCompleted(0).has_value());
    CHECK_FALSE(tracker.ReapCompleted(1).has_value());
    CHECK_FALSE(tracker.ReapNewest().has_value());
}

TEST_CASE("discarded and resized captures never publish")
{
    SnapshotTracker<TestCapture, 2> tracker;
    TestCapture proven;
    proven.submitSequence = 1;
    proven.valid = true;
    tracker.Submit(0, proven);
    REQUIRE(tracker.ReapCompleted(0).has_value());

    // A failed/discarded frame submits nothing: the drain stays empty and
    // the caller retains its previous completed pair.
    CHECK_FALSE(tracker.ReapNewest().has_value());

    // A resize drops pending work: a submitted-but-unproven slot cannot
    // promote a destroyed source afterwards.
    TestCapture pending;
    pending.submitSequence = 2;
    pending.valid = true;
    tracker.Submit(1, pending);
    tracker.Reset();
    CHECK_FALSE(tracker.ReapCompleted(1).has_value());
    CHECK_FALSE(tracker.ReapNewest().has_value());
}

TEST_CASE("readback conversion uses the captured presentation exactly")
{
    // The readback path converts through ConvertHdrPixelToDisplay8 with the
    // captured operator and exp2(EV). These vectors pin the pairing the
    // renderer performs: same multiplier the builder produced.
    TonemapPushConstants pc{};
    CameraPresentation look;
    look.toneMap = ToneMapOperator::ACESFitted;
    look.exposureEV = 1.0f;
    REQUIRE(TryBuildTonemapPushConstants(look, false, pc));
    const float exposureMult = CameraExposureMultiplier(look.exposureEV);
    CHECK(pc.exposureMultiplier == doctest::Approx(exposureMult));

    uint8_t out[4] = {};
    REQUIRE(ToneMapMath::ConvertHdrPixelToDisplay8(
        0.5f, 0.5f, 0.5f, 1.0f, look.toneMap, exposureMult, out));
    // A +1 EV exposure of mid gray must brighten every channel versus 0 EV.
    uint8_t dark[4] = {};
    REQUIRE(ToneMapMath::ConvertHdrPixelToDisplay8(
        0.5f, 0.5f, 0.5f, 1.0f, look.toneMap, 1.0f, dark));
    CHECK(out[0] > dark[0]);
    CHECK(out[1] > dark[1]);
    CHECK(out[2] > dark[2]);
    CHECK(out[3] == 255);
}
