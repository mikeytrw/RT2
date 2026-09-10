#include <doctest/doctest.h>

#include "SkyMotion.h"
#include <glm/gtc/matrix_transform.hpp>
#include <fstream>
#include <iterator>
#include <string>

// ============================================================================
// Infinite-sky rotation-only motion contract (sky shimmer repair ticket).
//
// One production CPU-testable authority: the sky has no parallax, motion is
// current-to-previous render pixels, jitter never enters (NGX receives it
// separately), identical orientations yield bit-exact zero. The GLSL twin
// (skyMotionPixels) and the guide reporter derive the same expectation
// independently; source-contract pins below keep the three copies honest.
// ============================================================================

namespace
{
std::string ReadSource(const char* path)
{
	std::ifstream in(path, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

glm::mat4 TestProjection()
{
	return glm::perspective(glm::radians(60.0f), 1280.0f / 720.0f, 0.1f, 1000.0f);
}

glm::mat4 YawView(float radians)
{
	glm::mat4 view(1.0f);
	const float c = std::cos(radians), s = std::sin(radians);
	view[0][0] = c;
	view[0][2] = -s;
	view[2][0] = s;
	view[2][2] = c;
	return view;
}
}

TEST_CASE("SkyMotion: static camera yields bit-exact zero")
{
	const glm::mat4 projection = TestProjection();
	const glm::mat4 view = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 3.0f, 0.0f));
	const glm::vec2 extent(1280.0f, 720.0f);
	for (const glm::vec2 pixel : {glm::vec2(0.0f), glm::vec2(640.0f, 360.0f),
		glm::vec2(1279.0f, 719.0f), glm::vec2(321.0f, 90.0f)})
	{
		const SkyMotionResult r = ComputeInfiniteSkyMotion(
			pixel, extent, projection, view, projection, view);
		CHECK(r.motionPixels.x == 0.0f);
		CHECK(r.motionPixels.y == 0.0f);
		CHECK_FALSE(r.degenerate);
	}
}

TEST_CASE("SkyMotion: translation without rotation yields exactly zero")
{
	const glm::mat4 projection = TestProjection();
	glm::mat4 view = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 3.0f, 0.0f));
	glm::mat4 moved = glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, 3.0f, 1.5f));
	const glm::vec2 extent(1280.0f, 720.0f);
	const glm::vec2 pixel(640.0f, 360.0f);
	// A 2.5-unit translation is a real cut-sized move, yet the infinite sky
	// does not move: rotations are identical, so the short-circuit holds.
	const SkyMotionResult r = ComputeInfiniteSkyMotion(
		pixel, extent, projection, view, projection, moved);
	CHECK(r.motionPixels.x == 0.0f);
	CHECK(r.motionPixels.y == 0.0f);
	// The finite-point mutant this replaces reports over a pixel here:
	// parallax at 1000 units for a 2.5-unit move is far above tolerance.
	const glm::vec3 dir = glm::normalize(glm::vec3(0.0f, 0.0f, -1.0f));
	const glm::vec4 finitePrev = projection * moved * glm::vec4(dir * 1000.0f, 1.0f);
	const glm::vec4 finiteCur = projection * view * glm::vec4(dir * 1000.0f, 1.0f);
	const glm::vec2 mutant = (glm::vec2(finitePrev) / finitePrev.w * 0.5f + 0.5f -
		(glm::vec2(finiteCur) / finiteCur.w * 0.5f + 0.5f)) * extent;
	CHECK(glm::length(mutant) > 0.25f);
}

TEST_CASE("SkyMotion: yaw agrees with the thin-lens expectation within 0.25px")
{
	const glm::mat4 projection = TestProjection();
	const glm::mat4 view = glm::mat4(1.0f);
	const float yaw = glm::radians(1.0f);
	const glm::mat4 prevView = YawView(-yaw);
	const glm::vec2 extent(1280.0f, 720.0f);
	const glm::vec2 pixel(640.0f, 360.0f);
	const SkyMotionResult r = ComputeInfiniteSkyMotion(
		pixel, extent, projection, view, projection, prevView);
	CHECK_FALSE(r.degenerate);
	// Thin-lens closed form at the centre pixel: focal * yaw angle.
	const float focalPx = (extent.y * 0.5f) / std::tan(glm::radians(30.0f));
	const glm::vec2 expected(focalPx * yaw, 0.0f);
	CHECK(std::abs(r.motionPixels.x - expected.x) <= 0.25f);
	CHECK(std::abs(r.motionPixels.y - expected.y) <= 0.25f);
	// Rotation invariance: translating both cameras changes nothing.
	glm::mat4 movedView = glm::translate(glm::mat4(1.0f), glm::vec3(5.0f, -2.0f, 7.0f)) * view;
	glm::mat4 movedPrev = glm::translate(glm::mat4(1.0f), glm::vec3(5.0f, -2.0f, 7.0f)) * prevView;
	const SkyMotionResult shifted = ComputeInfiniteSkyMotion(
		pixel, extent, projection, movedView, projection, movedPrev);
	CHECK(shifted.motionPixels.x == doctest::Approx(r.motionPixels.x));
	CHECK(shifted.motionPixels.y == doctest::Approx(r.motionPixels.y));
}

TEST_CASE("SkyMotion: producer sources share one jitter-free sky convention")
{
	const std::string shared = ReadSource("RT2App/shaders/pathtracer_shared.glsl");
	REQUIRE(!shared.empty());
	CHECK(shared.find("skyMotionPixels") != std::string::npos);
	CHECK(shared.find("1000.0") == std::string::npos);
	CHECK(shared.find("camera.forward.w") == std::string::npos);
	CHECK(shared.find("camera.right.w") == std::string::npos);
	const std::string secondary = ReadSource("RT2App/shaders/secondary_raygen.rgen");
	REQUIRE(!secondary.empty());
	// Sky radiance sampling stays jittered (TAA design); only motion went
	// rotation-only. A "fix" that unjitters sampling trips this pin.
	CHECK(secondary.find("float(pixel.x) + 0.5 + camera.forward.w") != std::string::npos);
	CHECK(secondary.find("writeNRDSkyDefaults(pixel, skyRadiance)") != std::string::npos);
	const std::string raygen = ReadSource("RT2App/shaders/raygen.rgen");
	REQUIRE(!raygen.empty());
	// Pure-RT miss shares the convention instead of conflicting zero.
	CHECK(raygen.find("skyMotionPixels(pixel)") != std::string::npos);
	const std::string reporter = ReadSource("RT2App/src/RRGuideResources.cpp");
	REQUIRE(!reporter.empty());
	// The independent expectation projects directions (w=0), never a
	// finite camera-position point.
	CHECK(reporter.find("1000.0f") == std::string::npos);
	CHECK(reporter.find("prevViewDirection") != std::string::npos);
}
