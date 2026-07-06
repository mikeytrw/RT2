#pragma once

#include "vulkan/vulkan.h"
#include <functional>

struct GpuDevice;

namespace CommandUtils
{
	// Synchronous one-shot command buffer: allocates a transient pool,
	// records the given lambda, submits, waits on a fence, and tears down.
	// Used for one-time uploads, layout transitions, AS builds.
	void ImmediateSubmit(const GpuDevice& dev, std::function<void(VkCommandBuffer)> record);
}