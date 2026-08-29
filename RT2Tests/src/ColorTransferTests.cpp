#include <doctest/doctest.h>

#include "ColorTransfer.h"
#include "HeadlessImageOutput.h"

TEST_CASE("linear to sRGB uses the exact piecewise transfer")
{
	CHECK(ColorTransfer::LinearToSRGB(0.0f) == doctest::Approx(0.0f));
	CHECK(ColorTransfer::LinearToSRGB(0.0031308f) == doctest::Approx(0.0404499f));
	CHECK(ColorTransfer::LinearToSRGB(0.18f) == doctest::Approx(0.461356f));
	CHECK(ColorTransfer::LinearToSRGB(0.5f) == doctest::Approx(0.735357f));
	CHECK(ColorTransfer::LinearToSRGB(1.0f) == doctest::Approx(1.0f));
}

TEST_CASE("headless display conversion matches Reinhard followed by sRGB")
{
	CHECK(ColorTransfer::LinearToSRGB8(ColorTransfer::Reinhard(0.0f)) == 0);
	CHECK(ColorTransfer::LinearToSRGB8(ColorTransfer::Reinhard(0.18f)) == 109);
	CHECK(ColorTransfer::LinearToSRGB8(ColorTransfer::Reinhard(0.5f)) == 156);
	CHECK(ColorTransfer::LinearToSRGB8(ColorTransfer::Reinhard(1.0f)) == 188);
}

TEST_CASE("headless image export preserves top-down GPU readback rows")
{
	const std::vector<uint8_t> rgba8 = {
		1, 2, 3, 4, 5, 6, 7, 8,
		11, 12, 13, 14, 15, 16, 17, 18
	};
	std::vector<uint8_t> packed8;
	REQUIRE(HeadlessImageOutput::PackRGBA8TopDown(rgba8, 2, 2, packed8));
	CHECK(packed8 == rgba8);
	CHECK(packed8[0] == 1);
	CHECK(packed8[8] == 11);

	const std::vector<float> rgba32 = {
		1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f,
		11.0f, 12.0f, 13.0f, 14.0f, 15.0f, 16.0f, 17.0f, 18.0f
	};
	std::vector<float> packed32;
	REQUIRE(HeadlessImageOutput::PackRGB32FTopDown(rgba32, 2, 2, packed32));
	CHECK(packed32 == std::vector<float>{
		1.0f, 2.0f, 3.0f, 5.0f, 6.0f, 7.0f,
		11.0f, 12.0f, 13.0f, 15.0f, 16.0f, 17.0f
	});
	CHECK(packed32[0] == 1.0f);
	CHECK(packed32[6] == 11.0f);

	CHECK_FALSE(HeadlessImageOutput::PackRGBA8TopDown(rgba8, 1, 1, packed8));
	CHECK_FALSE(HeadlessImageOutput::PackRGB32FTopDown(rgba32, 0, 2, packed32));
}
