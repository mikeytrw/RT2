#pragma once

#include "vulkan/vulkan.h"

// FrameContext — per-frame-in-flight command pool, command buffer, and fence.
// Used in a ring of MAX_FRAMES_IN_FLIGHT slots to keep the CPU ahead of the GPU.
struct FrameContext
{
	VkCommandPool   commandPool   = VK_NULL_HANDLE;
	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	VkFence         renderFence   = VK_NULL_HANDLE;  // signaled when GPU finishes this frame's work
	bool            fenceSignaled = false;          // tracks whether fence is in signaled state

	// Create pool + command buffer + fence (fence starts signaled so the first
	// WaitForFence on frame 0 doesn't block forever).
	void Init(VkDevice device, uint32_t queueFamily);
	void Destroy(VkDevice device);

	// Wait for this frame's GPU work to complete, then reset the fence.
	void WaitForFence(VkDevice device);

	// Reset command pool + begin recording.
	void Begin(VkDevice device);

	// End recording + submit to queue with renderFence (no wait — async).
	void Submit(VkQueue queue);
};