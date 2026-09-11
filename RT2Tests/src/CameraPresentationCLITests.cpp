#include <doctest/doctest.h>

#include "CameraPresentation.h"
#include "CLIArgs.h"

namespace
{
CLIArgs ParseArgs(std::initializer_list<const char*> words)
{
    std::vector<const char*> argv(words);
    return CLIArgs::Parse(static_cast<int>(argv.size()),
                          const_cast<char**>(argv.data()));
}
} // namespace

TEST_CASE("CLI tone-map and exposure flags parse with independent presence")
{
    CLIArgs args = ParseArgs({ "RT2App", "--tone-map", "aces", "--exposure-ev", "-1.5" });
    CHECK(args.presentationError.empty());
    CHECK(args.hasToneMap);
    CHECK(args.toneMap == ToneMapOperator::ACESFitted);
    CHECK(args.hasExposureEV);
    CHECK(args.exposureEV == doctest::Approx(-1.5f));

    // Each flag stands alone: EV keeps the camera operator and vice versa.
    CLIArgs evOnly = ParseArgs({ "RT2App", "--exposure-ev", "2" });
    CHECK(evOnly.presentationError.empty());
    CHECK_FALSE(evOnly.hasToneMap);
    CHECK(evOnly.hasExposureEV);
    CHECK(evOnly.exposureEV == doctest::Approx(2.0f));

    CLIArgs opOnly = ParseArgs({ "RT2App", "--tone-map", "reinhard" });
    CHECK(opOnly.presentationError.empty());
    CHECK(opOnly.hasToneMap);
    CHECK(opOnly.toneMap == ToneMapOperator::Reinhard);
    CHECK_FALSE(opOnly.hasExposureEV);

    CLIArgs agx = ParseArgs({ "RT2App", "--tone-map", "agx" });
    CHECK(agx.presentationError.empty());
    CHECK(agx.toneMap == ToneMapOperator::AgX);
}

TEST_CASE("CLI tone-map spelling is exact and case-sensitive")
{
    for (const char* bad : { "AGX", "Aces", "ACES-FITTED", "hdr", "", "reinhard " })
    {
        CLIArgs args = ParseArgs({ "RT2App", "--tone-map", bad });
        CHECK_FALSE(args.presentationError.empty());
        CHECK_FALSE(args.hasToneMap);
    }
    CLIArgs missing = ParseArgs({ "RT2App", "--tone-map" });
    CHECK_FALSE(missing.presentationError.empty());
    CHECK_FALSE(missing.hasToneMap);
}

TEST_CASE("CLI exposure rejects non-numeric, non-finite and out-of-range input")
{
    for (const char* bad : { "abc", "nan", "NaN", "inf", "-inf", "20", "-8.5",
                             "8.001", "1e3", "2x", "" })
    {
        CLIArgs args = ParseArgs({ "RT2App", "--exposure-ev", bad });
        CHECK_FALSE(args.presentationError.empty());
        CHECK_FALSE(args.hasExposureEV);
    }
    for (const char* good : { "-8", "8", "0", "-0.0", "2.5", "+3" })
    {
        CLIArgs args = ParseArgs({ "RT2App", "--exposure-ev", good });
        CHECK(args.presentationError.empty());
        CHECK(args.hasExposureEV);
    }
    CLIArgs missing = ParseArgs({ "RT2App", "--exposure-ev" });
    CHECK_FALSE(missing.presentationError.empty());
    CHECK_FALSE(missing.hasExposureEV);
}

TEST_CASE("CLI presentation seed overlays each axis independently")
{
    const CameraPresentation agx;
    CameraPresentation out;

    // EV alone keeps the resolved operator.
    REQUIRE(TryApplyPresentationSeed(agx, false, ToneMapOperator::Reinhard,
                                     true, -2.0f, out));
    CHECK(out.toneMap == ToneMapOperator::AgX);
    CHECK(out.exposureEV == doctest::Approx(-2.0f));

    // Operator alone keeps the resolved EV.
    CameraPresentation exposed = agx;
    exposed.exposureEV = 1.0f;
    REQUIRE(TryApplyPresentationSeed(exposed, true, ToneMapOperator::ACESFitted,
                                     false, 9.0f, out));
    CHECK(out.toneMap == ToneMapOperator::ACESFitted);
    CHECK(out.exposureEV == doctest::Approx(1.0f));

    // Both flags apply together and canonicalize negative zero.
    CameraPresentation negZero = agx;
    negZero.exposureEV = -0.0f;
    REQUIRE(TryApplyPresentationSeed(agx, true, ToneMapOperator::Reinhard,
                                     true, -0.0f, out));
    CHECK(out.toneMap == ToneMapOperator::Reinhard);
    CHECK(out.exposureEV == doctest::Approx(0.0f));

    // No flags is an exact copy (a consumed seed is a no-op).
    REQUIRE(TryApplyPresentationSeed(exposed, false, ToneMapOperator::AgX,
                                     false, 0.0f, out));
    CHECK(out == exposed);

    // Invalid input never lands: the output is untouched so a failed seed
    // can never defeat later UI edits.
    CameraPresentation sentinel;
    sentinel.toneMap = ToneMapOperator::Reinhard;
    sentinel.exposureEV = 3.0f;
    CameraPresentation untouched = sentinel;
    CHECK_FALSE(TryApplyPresentationSeed(agx, false, ToneMapOperator::AgX,
                                         true, 20.0f, untouched));
    CHECK(untouched == sentinel);
    CHECK_FALSE(TryApplyPresentationSeed(agx, true, static_cast<ToneMapOperator>(9),
                                         false, 0.0f, untouched));
    CHECK(untouched == sentinel);
}
