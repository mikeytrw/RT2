#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace HeadlessImageOutput
{
	inline bool ComponentCount(uint32_t width, uint32_t height, size_t components, size_t& outCount)
	{
		if (width == 0 || height == 0 || components == 0)
			return false;

		const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
		if (pixelCount / static_cast<size_t>(width) != static_cast<size_t>(height) ||
		    pixelCount > std::numeric_limits<size_t>::max() / components)
			return false;

		outCount = pixelCount * components;
		return true;
	}

	// vkCmdCopyImageToBuffer preserves image texel row order. RT2's output images
	// already use conventional top-to-bottom rows, so PNG export must not apply a
	// second presentation-oriented vertical flip.
	inline bool PackRGBA8TopDown(const std::vector<uint8_t>& rgba,
	                            uint32_t width,
	                            uint32_t height,
	                            std::vector<uint8_t>& outRGBA)
	{
		size_t expected = 0;
		if (!ComponentCount(width, height, 4, expected) || rgba.size() != expected)
			return false;

		outRGBA = rgba;
		return true;
	}

	inline bool PackRGB32FTopDown(const std::vector<float>& rgba,
	                             uint32_t width,
	                             uint32_t height,
	                             std::vector<float>& outRGB)
	{
		size_t expectedRGBA = 0;
		size_t expectedRGB = 0;
		if (!ComponentCount(width, height, 4, expectedRGBA) || rgba.size() != expectedRGBA ||
		    !ComponentCount(width, height, 3, expectedRGB))
			return false;

		outRGB.resize(expectedRGB);
		for (size_t pixel = 0; pixel < expectedRGBA / 4; ++pixel)
		{
			outRGB[pixel * 3 + 0] = rgba[pixel * 4 + 0];
			outRGB[pixel * 3 + 1] = rgba[pixel * 4 + 1];
			outRGB[pixel * 3 + 2] = rgba[pixel * 4 + 2];
		}
		return true;
	}
}
