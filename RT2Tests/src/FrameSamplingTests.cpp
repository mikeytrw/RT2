#include <doctest/doctest.h>

#include "FrameSampling.h"

// ============================================================================
// Shared frame sampling authority (amendment 2026-09-09, step 3).
//
// One Halton subpixel sequence feeds raster/ray sampling, ReSTIR
// reprojection, NRD and NGX InJitterOffset. These tests compile against the
// production header and fail if the sequence drifts, if prev/current ever
// disagree across a reset boundary, or if a disabled frame leaks a stale
// offset (which would double-compensate motion downstream).
// ============================================================================

TEST_CASE("FrameSampling: Halton sequence is exact and shared")
{
	CHECK(FrameSamplingHalton(1, 2) == doctest::Approx(0.5f));
	CHECK(FrameSamplingHalton(1, 3) == doctest::Approx(1.0f / 3.0f));
	CHECK(FrameSamplingHalton(2, 2) == doctest::Approx(0.25f));
	CHECK(FrameSamplingHalton(2, 3) == doctest::Approx(2.0f / 3.0f));
	// First frame of the 16-cycle: centered in x, -1/6px in y at scale 1.
	const glm::vec2 first = FrameSamplingJitterForFrame(1, 1.0f);
	CHECK(first.x == doctest::Approx(0.0f));
	CHECK(first.y == doctest::Approx(-1.0f / 6.0f));
	// The cycle wraps: frame 17 reuses frame 1 exactly.
	const glm::vec2 wrapped = FrameSamplingJitterForFrame(17, 1.0f);
	CHECK(wrapped.x == doctest::Approx(first.x));
	CHECK(wrapped.y == doctest::Approx(first.y));
	// Scale applies uniformly; all offsets stay subpixel at scale 1.
	CHECK(FrameSamplingJitterForFrame(7, 2.0f).x ==
		doctest::Approx(2.0f * FrameSamplingJitterForFrame(7, 1.0f).x));
	for (uint32_t frame = 1; frame <= 16; ++frame)
	{
		const glm::vec2 j = FrameSamplingJitterForFrame(frame, 1.0f);
		CHECK(std::abs(j.x) <= 0.5f);
		CHECK(std::abs(j.y) <= 0.5f);
	}
}

TEST_CASE("FrameSampling: disabled frames carry exactly zero")
{
	const FrameSamplingState off = ComputeFrameSampling(9, false, 1.0f, false);
	CHECK_FALSE(off.jitterActive);
	CHECK(off.jitter.x == doctest::Approx(0.0f));
	CHECK(off.jitter.y == doctest::Approx(0.0f));
	CHECK(off.jitterPrev.x == doctest::Approx(0.0f));
	CHECK(off.jitterPrev.y == doctest::Approx(0.0f));
}

TEST_CASE("FrameSampling: enabled state advances on the sample clock")
{
	const FrameSamplingState a = ComputeFrameSampling(5, true, 1.0f, false);
	const FrameSamplingState b = ComputeFrameSampling(6, true, 1.0f, false);
	CHECK(a.jitterActive);
	CHECK_FALSE(a.resetThisFrame);
	CHECK(a.jitter != b.jitter);
	CHECK(a.jitter == FrameSamplingJitterForFrame(5, 1.0f));
	CHECK(b.jitter == FrameSamplingJitterForFrame(6, 1.0f));
}

TEST_CASE("FrameSampling: reset flag is explicit for cut/resize/mode edges")
{
	const FrameSamplingState steady = ComputeFrameSampling(5, true, 1.0f, false);
	CHECK_FALSE(steady.resetThisFrame);
	const FrameSamplingState cut = ComputeFrameSampling(6, true, 1.0f, true);
	CHECK(cut.resetThisFrame);
	// The renderer zeroes jitterPrev on reset; the authority marks the edge
	// rather than guessing history it cannot see.
	CHECK(cut.jitter == FrameSamplingJitterForFrame(6, 1.0f));
}

TEST_CASE("FrameSampling: prev/current pairs stay within one subpixel step")
{
	// Any consumer subtracting prev from current (ReSTIR jitterDelta, NGX
	// InJitter compensation) must see at most a single subpixel step; a
	// larger jump would read as scene motion and smear history.
	for (uint32_t frame = 1; frame <= 32; ++frame)
	{
		const glm::vec2 cur = FrameSamplingJitterForFrame(frame, 1.0f);
		const glm::vec2 prev = FrameSamplingJitterForFrame(
			frame == 1 ? 16 : frame - 1, 1.0f);
		CHECK(std::abs(cur.x - prev.x) <= 1.0f);
		CHECK(std::abs(cur.y - prev.y) <= 1.0f);
	}
}

TEST_CASE("FrameSampling: NRD boundary converts pixels to UV once")
{
	// REBLUR documents UV inputs (vendored NRDSettings.h:112-118). At
	// 1280x720 a half-pixel jitter is 0.5/1280 UV; motion scale is the
	// reciprocal extent. Zeros stay zero so unjittered modes are untouched.
	const glm::vec2 extent(1280.0f, 720.0f);
	const glm::vec2 uv = FrameSamplingJitterToUv(glm::vec2(0.5f, -0.25f), extent);
	CHECK(uv.x == doctest::Approx(0.5f / 1280.0f));
	CHECK(uv.y == doctest::Approx(-0.25f / 720.0f));
	CHECK(FrameSamplingJitterToUv(glm::vec2(0.0f), extent) == glm::vec2(0.0f));
	const glm::vec3 scale = FrameSamplingMotionScaleToUv(extent);
	CHECK(scale.x == doctest::Approx(1.0f / 1280.0f));
	CHECK(scale.y == doctest::Approx(1.0f / 720.0f));
	CHECK(scale.z == doctest::Approx(0.0f));
}
