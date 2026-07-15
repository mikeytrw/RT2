#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ColorTransfer
{
	inline float Reinhard(float linear)
	{
		linear = std::max(linear, 0.0f);
		return linear / (1.0f + linear);
	}

	inline float LinearToSRGB(float linear)
	{
		linear = std::max(linear, 0.0f);
		if (linear <= 0.0031308f)
			return 12.92f * linear;
		return 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
	}

	inline uint8_t LinearToSRGB8(float linear)
	{
		const float encoded = std::clamp(LinearToSRGB(linear), 0.0f, 1.0f);
		return static_cast<uint8_t>(encoded * 255.0f + 0.5f);
	}
}
