#pragma once

#include "vulkan/vulkan.h"
#include <cstdint>

// GpuDevice — captures all Vulkan device-level handles so renderer code
// doesn't need to call Walnut::Application statics.
// Created once at init, passed by const reference to all resource/pass code.
struct GpuDevice
{
	VkInstance       instance       = VK_NULL_HANDLE;
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	VkDevice         device         = VK_NULL_HANDLE;
	VkQueue          queue          = VK_NULL_HANDLE;
	uint32_t         queueFamily    = 0;
	VkDescriptorPool descriptorPool  = VK_NULL_HANDLE;
	VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtPipelineProps = {};

	bool rayTracingSupported = false;
	bool rayTracingPipelineSupported = false;

	// Cached memory properties (avoids re-querying on every FindMemoryType call)
	VkPhysicalDeviceMemoryProperties cachedMemProps = {};

	// maxMemoryAllocationCount from VkPhysicalDeviceLimits — the hard cap
	// on vkAllocateMemory calls. Typical: 4096 (NVIDIA/AMD), 1024 (Intel).
	// When the app creates more allocations than this, vkAllocateMemory
	// returns VK_ERROR_TOO_MANY_OBJECTS and VK_CHECK aborts.
	uint32_t maxMemoryAllocationCount = 0;

	// Initialize from Walnut::Application statics
	void InitFromWalnut();

	// Helpers
	uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;
	VkDeviceAddress GetBufferDeviceAddress(VkBuffer buffer) const;
};