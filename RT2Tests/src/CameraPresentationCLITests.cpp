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

// ============================================================================
// RR5-T1 fixup (review P2): strict roulette CLI parsing. Full-token,
// finite, in-range values with explicit presence; anything else sets
// rouletteError (headless exits nonzero before output) and never lands.
// ============================================================================

TEST_CASE("CLI roulette defaults are valid and unset")
{
    CLIArgs args = ParseArgs({ "RT2App" });
    CHECK(args.rouletteError.empty());
    CHECK_FALSE(args.hasRrStartDepth);
    CHECK_FALSE(args.hasRrMinP);
    CHECK_FALSE(args.hasRrMaxP);
}

TEST_CASE("CLI roulette valid values parse with explicit presence")
{
    CLIArgs args = ParseArgs({ "RT2App", "--rr-start-depth", "2",
                               "--rr-min-p", "0.1", "--rr-max-p", "0.9" });
    CHECK(args.rouletteError.empty());
    CHECK(args.hasRrStartDepth);
    CHECK(args.rrStartDepth == 2);
    CHECK(args.hasRrMinP);
    CHECK(args.rrMinP == doctest::Approx(0.1f));
    CHECK(args.hasRrMaxP);
    CHECK(args.rrMaxP == doctest::Approx(0.9f));

    // Each flag stands alone.
    CLIArgs depthOnly = ParseArgs({ "RT2App", "--rr-start-depth", "1" });
    CHECK(depthOnly.rouletteError.empty());
    CHECK(depthOnly.hasRrStartDepth);
    CHECK_FALSE(depthOnly.hasRrMinP);
    CHECK_FALSE(depthOnly.hasRrMaxP);
}

TEST_CASE("CLI roulette rejects malformed, missing and out-of-range depth")
{
    for (const char* bad : { "abc", "2x", "", "0", "-3", "2.5",
                             "99999999999999999999" })
    {
        CLIArgs args = ParseArgs({ "RT2App", "--rr-start-depth", bad });
        CHECK_FALSE(args.rouletteError.empty());
        CHECK_FALSE(args.hasRrStartDepth);
    }
    CLIArgs missing = ParseArgs({ "RT2App", "--rr-start-depth" });
    CHECK_FALSE(missing.rouletteError.empty());
    CHECK_FALSE(missing.hasRrStartDepth);
}

TEST_CASE("CLI roulette rejects non-numeric, non-finite and out-of-range bounds")
{
    for (const char* bad : { "abc", "nan", "NaN", "inf", "-inf", "1e999",
                             "0", "1", "-0.5", "1.5", "0.05x", "" })
    {
        CLIArgs minArgs = ParseArgs({ "RT2App", "--rr-min-p", bad });
        CHECK_FALSE(minArgs.rouletteError.empty());
        CHECK_FALSE(minArgs.hasRrMinP);
        CLIArgs maxArgs = ParseArgs({ "RT2App", "--rr-max-p", bad });
        CHECK_FALSE(maxArgs.rouletteError.empty());
        CHECK_FALSE(maxArgs.hasRrMaxP);
    }
    CLIArgs missingMin = ParseArgs({ "RT2App", "--rr-min-p" });
    CHECK_FALSE(missingMin.rouletteError.empty());
    CHECK_FALSE(missingMin.hasRrMinP);
    CLIArgs missingMax = ParseArgs({ "RT2App", "--rr-max-p" });
    CHECK_FALSE(missingMax.rouletteError.empty());
    CHECK_FALSE(missingMax.hasRrMaxP);
}

TEST_CASE("CLI roulette rejects an explicitly inverted window")
{
    CLIArgs inverted = ParseArgs({ "RT2App", "--rr-min-p", "0.9",
                                   "--rr-max-p", "0.1" });
    CHECK_FALSE(inverted.rouletteError.empty());
    CLIArgs equal = ParseArgs({ "RT2App", "--rr-min-p", "0.5",
                                "--rr-max-p", "0.5" });
    CHECK_FALSE(equal.rouletteError.empty());
    // A valid window stays valid regardless of flag order.
    CLIArgs ordered = ParseArgs({ "RT2App", "--rr-max-p", "0.9",
                                  "--rr-min-p", "0.1" });
    CHECK(ordered.rouletteError.empty());
    CHECK(ordered.hasRrMinP);
    CHECK(ordered.hasRrMaxP);
    // Explicit defaults are valid.
    CLIArgs explicitDefaults = ParseArgs({ "RT2App", "--rr-min-p", "0.05",
                                           "--rr-max-p", "0.95" });
    CHECK(explicitDefaults.rouletteError.empty());
    CHECK(explicitDefaults.hasRrMinP);
    CHECK(explicitDefaults.hasRrMaxP);
}

// RR5-T1 fixup (review R1): a single bound is validated against the
// omitted default, so --rr-min-p 0.96 fails (default max is 0.95)
// instead of silently canonicalizing back to defaults.
TEST_CASE("CLI roulette validates single flags against omitted defaults")
{
    CLIArgs minAboveDefaultMax = ParseArgs({ "RT2App", "--rr-min-p", "0.96" });
    CHECK_FALSE(minAboveDefaultMax.rouletteError.empty());
    // The token itself parsed; the effective window against the omitted
    // default is what fails. Presence stays truthful (exit happens before
    // any application site reads it).
    CHECK(minAboveDefaultMax.hasRrMinP);

    CLIArgs maxBelowDefaultMin = ParseArgs({ "RT2App", "--rr-max-p", "0.04" });
    CHECK_FALSE(maxBelowDefaultMin.rouletteError.empty());
    CHECK(maxBelowDefaultMin.hasRrMaxP);

    CLIArgs minBelowDefaultMax = ParseArgs({ "RT2App", "--rr-min-p", "0.04" });
    CHECK(minBelowDefaultMax.rouletteError.empty());
    CHECK(minBelowDefaultMax.hasRrMinP);

    CLIArgs maxAboveDefaultMin = ParseArgs({ "RT2App", "--rr-max-p", "0.96" });
    CHECK(maxAboveDefaultMin.rouletteError.empty());
    CHECK(maxAboveDefaultMin.hasRrMaxP);
}

// RR5-T1 fixup (review R1): the narrowed destination float is
// validated, not just the double token. 1e-50 is finite in (0,1) as a
// double but narrows to float 0; a decimal rounding up to 1.0f is
// equally unusable as a survival-probability bound.
TEST_CASE("CLI roulette rejects float underflow and round-up tokens")
{
    CLIArgs underflowMin = ParseArgs({ "RT2App", "--rr-min-p", "1e-50" });
    CHECK_FALSE(underflowMin.rouletteError.empty());
    CHECK_FALSE(underflowMin.hasRrMinP);

    CLIArgs underflowMax = ParseArgs({ "RT2App", "--rr-max-p", "1e-50" });
    CHECK_FALSE(underflowMax.rouletteError.empty());
    CHECK_FALSE(underflowMax.hasRrMaxP);

    CLIArgs roundUpMax = ParseArgs({ "RT2App", "--rr-max-p", "0.99999999999999999" });
    CHECK_FALSE(roundUpMax.rouletteError.empty());
    CHECK_FALSE(roundUpMax.hasRrMaxP);
}

// ============================================================================
// RR5-T0: strict diagnostic-seed CLI. Full-token unsigned values with
// explicit presence; malformed input sets diagSeedError (headless exits
// nonzero before output) and never lands.
// ============================================================================

TEST_CASE("CLI diag-seed defaults are valid and unset")
{
    CLIArgs args = ParseArgs({ "RT2App" });
    CHECK(args.diagSeedError.empty());
    CHECK(args.diagSeed == 0u);
    CHECK_FALSE(args.hasDiagSeed);
    CHECK_FALSE(args.hasDiagRngPixel);
    CHECK(args.diagRngDump.empty());
}

TEST_CASE("CLI diag-seed valid values parse with explicit presence")
{
    CLIArgs args = ParseArgs({ "RT2App", "--diag-seed", "12345" });
    CHECK(args.diagSeedError.empty());
    CHECK(args.hasDiagSeed);
    CHECK(args.diagSeed == 12345u);

    CLIArgs hex = ParseArgs({ "RT2App", "--diag-seed", "0xFF" });
    CHECK(hex.diagSeedError.empty());
    CHECK(hex.diagSeed == 255u);

    CLIArgs zero = ParseArgs({ "RT2App", "--diag-seed", "0" });
    CHECK(zero.diagSeedError.empty());
    CHECK(zero.hasDiagSeed);
    CHECK(zero.diagSeed == 0u);
}

// RR5-T0 review P2: the full uint32 domain is representable — the CLI
// and the InputScalar-U32 widget carry every value faithfully, with no
// INT_MAX truncation. (The widget itself is not drivable in CPU tests;
// these cases pin the parsing/application boundary it shares.)
TEST_CASE("CLI diag-seed spans the full uint32 domain")
{
    CLIArgs intMax = ParseArgs({ "RT2App", "--diag-seed", "2147483647" });
    CHECK(intMax.diagSeedError.empty());
    CHECK(intMax.diagSeed == 2147483647u);

    CLIArgs uintMax = ParseArgs({ "RT2App", "--diag-seed", "4294967295" });
    CHECK(uintMax.diagSeedError.empty());
    CHECK(uintMax.hasDiagSeed);
    CHECK(uintMax.diagSeed == 4294967295u);
}

TEST_CASE("CLI diag-seed rejects malformed, negative, overflowing and missing input")
{
    for (const char* bad : { "abc", "12x", "", "-1", "-0", "99999999999999999999",
                             "4294967296", "1e3" })
    {
        CLIArgs args = ParseArgs({ "RT2App", "--diag-seed", bad });
        CHECK_FALSE(args.diagSeedError.empty());
        CHECK_FALSE(args.hasDiagSeed);
    }
    CLIArgs missing = ParseArgs({ "RT2App", "--diag-seed" });
    CHECK_FALSE(missing.diagSeedError.empty());
    CHECK_FALSE(missing.hasDiagSeed);
}

TEST_CASE("CLI diag-rng-pixel needs two non-negative integers")
{
    CLIArgs args = ParseArgs({ "RT2App", "--diag-rng-pixel", "24", "72" });
    CHECK(args.diagSeedError.empty());
    CHECK(args.hasDiagRngPixel);
    CHECK(args.diagRngPixelX == 24u);
    CHECK(args.diagRngPixelY == 72u);

    for (const char* bad : { "abc", "-1", "4x" })
    {
        CLIArgs badArgs = ParseArgs({ "RT2App", "--diag-rng-pixel", bad, "72" });
        CHECK_FALSE(badArgs.diagSeedError.empty());
        CHECK_FALSE(badArgs.hasDiagRngPixel);
    }
    CLIArgs missingY = ParseArgs({ "RT2App", "--diag-rng-pixel", "24" });
    CHECK_FALSE(missingY.diagSeedError.empty());
    CHECK_FALSE(missingY.hasDiagRngPixel);
    CLIArgs missingBoth = ParseArgs({ "RT2App", "--diag-rng-pixel" });
    CHECK_FALSE(missingBoth.diagSeedError.empty());
    CHECK_FALSE(missingBoth.hasDiagRngPixel);
}

TEST_CASE("CLI diag-rng-dump requires an explicit probe pixel")
{
    CLIArgs noPixel = ParseArgs({ "RT2App", "--diag-rng-dump", "out.json" });
    CHECK_FALSE(noPixel.diagSeedError.empty());

    CLIArgs paired = ParseArgs({ "RT2App", "--diag-rng-pixel", "24", "72",
                                 "--diag-rng-dump", "out.json" });
    CHECK(paired.diagSeedError.empty());
    CHECK(paired.hasDiagRngPixel);
    CHECK(paired.diagRngDump == "out.json");

    CLIArgs missingPath = ParseArgs({ "RT2App", "--diag-rng-pixel", "24", "72",
                                      "--diag-rng-dump" });
    CHECK_FALSE(missingPath.diagSeedError.empty());
}
