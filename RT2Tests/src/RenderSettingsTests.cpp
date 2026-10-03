#include <doctest/doctest.h>

#include "RenderSettings.h"

#include <limits>

// ============================================================================
// RenderSettings struct tests
//
// RenderSettings is a POD struct with default values + a dirty flag.
// The dirty flag is set by RendererGPU::ApplySettings() when any field
// changes. These tests verify the struct's defaults and that the
// dirty flag mechanism works as designed (the comparison logic lives
// in RendererGPU::ApplySettings, but the struct provides the field).
// ============================================================================

TEST_CASE("RenderSettings: default values")
{
	RenderSettings s;
	CHECK(s.spp == 5);
	CHECK(s.maxBounces == 8);
	CHECK(s.showBackground == false);
	CHECK(s.neeOnly == false);
	CHECK(s.emissiveBoost == 1.0f);
	CHECK(s.envIntensity == 1.0f);
	CHECK(s.rasterFirst == true);
	CHECK(s.denoiserMode == DenoiserMode::NRD);
	CHECK(s.dlssQuality == DlssQualityMode::Quality);
	CHECK(s.nrdMaxBlurRadius == 30.0f);
	CHECK(s.nrdMaxAccumFrames == 63);
	CHECK(s.restirEnabled == true);
	CHECK(s.restirGIEnabled == true);
	CHECK(s.nrdAntiFirefly == true);
	CHECK(s.nrdSplitScreen == 0.0f);
	CHECK(s.nrdJitterEnabled == true);
	CHECK(s.nrdJitterScale == 1.0f);
	CHECK(s.gbufferDebugMode == -1);
	CHECK(s.dirty == false);
}

TEST_CASE("RenderSettings: roulette defaults reproduce the legacy hard-codes")
{
	RenderSettings s;
	CHECK(s.rrStartDepth == 3u);
	CHECK(s.rrMinP == 0.05f);
	CHECK(s.rrMaxP == 0.95f);
	CHECK(kDefaultRRStartDepth == 3u);
	CHECK(kDefaultRRMinP == 0.05f);
	CHECK(kDefaultRRMaxP == 0.95f);
}

TEST_CASE("RenderSettings: canonical roulette values pass through untouched")
{
	RenderSettings s;
	CanonicalizeRouletteSettings(s);
	CHECK(s.rrStartDepth == 3u);
	CHECK(s.rrMinP == 0.05f);
	CHECK(s.rrMaxP == 0.95f);

	s.rrStartDepth = 2;
	s.rrMinP = 0.1f;
	s.rrMaxP = 0.9f;
	CanonicalizeRouletteSettings(s);
	CHECK(s.rrStartDepth == 2u);
	CHECK(s.rrMinP == 0.1f);
	CHECK(s.rrMaxP == 0.9f);
}

TEST_CASE("RenderSettings: non-finite roulette bounds fall back to defaults")
{
	RenderSettings s;
	s.rrMinP = std::numeric_limits<float>::quiet_NaN();
	s.rrMaxP = std::numeric_limits<float>::infinity();
	CanonicalizeRouletteSettings(s);
	CHECK(s.rrMinP == 0.05f);
	CHECK(s.rrMaxP == 0.95f);

	s.rrMinP = -std::numeric_limits<float>::infinity();
	s.rrMaxP = std::numeric_limits<float>::quiet_NaN();
	CanonicalizeRouletteSettings(s);
	CHECK(s.rrMinP == 0.05f);
	CHECK(s.rrMaxP == 0.95f);
}

TEST_CASE("RenderSettings: out-of-range or inverted roulette bounds reset")
{
	RenderSettings s;
	// Outside (0,1): each bound resets independently.
	s.rrMinP = 0.0f;
	s.rrMaxP = 1.0f;
	CanonicalizeRouletteSettings(s);
	CHECK(s.rrMinP == 0.05f);
	CHECK(s.rrMaxP == 0.95f);

	// In-range but inverted or degenerate: the whole window resets.
	s.rrMinP = 0.8f;
	s.rrMaxP = 0.2f;
	CanonicalizeRouletteSettings(s);
	CHECK(s.rrMinP == 0.05f);
	CHECK(s.rrMaxP == 0.95f);

	s.rrMinP = 0.5f;
	s.rrMaxP = 0.5f;
	CanonicalizeRouletteSettings(s);
	CHECK(s.rrMinP == 0.05f);
	CHECK(s.rrMaxP == 0.95f);
}

TEST_CASE("RenderSettings: roulette start depth floors at 1")
{
	RenderSettings s;
	s.rrStartDepth = 0;
	CanonicalizeRouletteSettings(s);
	CHECK(s.rrStartDepth == 1u);
}

TEST_CASE("RenderSettings: dirty flag starts false")
{
	RenderSettings s;
	CHECK_FALSE(s.dirty);
}

TEST_CASE("RenderSettings: copy preserves all fields")
{
	RenderSettings s;
	s.spp = 10;
	s.maxBounces = 16;
	s.envIntensity = 2.5f;
	s.rasterFirst = true;
	s.denoiserMode = DenoiserMode::RayReconstruction;
	s.dlssQuality = DlssQualityMode::Balanced;
	s.gbufferDebugMode = 3;

	RenderSettings copy = s;
	CHECK(copy.spp == 10);
	CHECK(copy.maxBounces == 16);
	CHECK(copy.envIntensity == 2.5f);
	CHECK(copy.rasterFirst == true);
	CHECK(copy.denoiserMode == DenoiserMode::RayReconstruction);
	CHECK(copy.dlssQuality == DlssQualityMode::Balanced);
	CHECK(copy.gbufferDebugMode == 3);
}

TEST_CASE("RenderSettings: field mutation is independent between instances")
{
	RenderSettings a;
	RenderSettings b;
	a.spp = 50;
	CHECK(b.spp == 5); // b unchanged
	b.maxBounces = 32;
	CHECK(a.maxBounces == 8); // a unchanged
}
// ============================================================================
// Editor presentation gating.
//
// `showBackground` is authored state that only applies while authoring. The
// rule is small but the failure it prevents is not: shipping a game whose
// sky is a debug colour, or -- worse the other way -- having Play silently
// rewrite the user's setting so it stays off after they stop.
// ============================================================================
TEST_CASE("RenderSettings: background is visible only in editor presentation")
{
	RenderSettings settings;

	settings.showBackground = true;
	CHECK(IsBackgroundVisible(settings, true));
	CHECK_FALSE(IsBackgroundVisible(settings, false));

	settings.showBackground = false;
	CHECK_FALSE(IsBackgroundVisible(settings, true));
	CHECK_FALSE(IsBackgroundVisible(settings, false));
}

TEST_CASE("RenderSettings: gating derives from the flag and never mutates it")
{
	RenderSettings settings;
	settings.showBackground = true;

	// Entering and leaving Play must leave the authored value untouched --
	// the whole reason this is a free function rather than a field.
	(void)IsBackgroundVisible(settings, false);
	CHECK(settings.showBackground);
	(void)IsBackgroundVisible(settings, true);
	CHECK(settings.showBackground);
}
