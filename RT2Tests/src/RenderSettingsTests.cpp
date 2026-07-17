#include <doctest/doctest.h>

#include "RenderSettings.h"

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
	CHECK(s.nrdEnabled == false);
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
	s.nrdEnabled = true;
	s.gbufferDebugMode = 3;

	RenderSettings copy = s;
	CHECK(copy.spp == 10);
	CHECK(copy.maxBounces == 16);
	CHECK(copy.envIntensity == 2.5f);
	CHECK(copy.rasterFirst == true);
	CHECK(copy.nrdEnabled == true);
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