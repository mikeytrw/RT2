#include <doctest/doctest.h>

#include "ColorTransfer.h"

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
