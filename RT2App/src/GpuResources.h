#pragma once

#include "vulkan/vulkan.h"
#include <cstdint>
#include <vector>

struct GpuDevice;

// GpuImage — simple image + memory + view wrapper
struct GpuImage
{
	VkImage        image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImageView    view  = VK_NULL_HANDLE;
	VkFormat       format = VK_FORMAT_UNDEFINED;
	uint32_t       width = 0;
	uint32_t       height = 0;

	bool IsValid() const { return image != VK_NULL_HANDLE; }
};

// GpuBuffer — simple buffer + memory wrapper
struct GpuBuffer
{
	VkBuffer       buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkDeviceSize   size = 0;

	bool IsValid() const { return buffer != VK_NULL_HANDLE; }
};

// Resource creation helpers (all take GpuDevice& instead of Walnut statics)
namespace GpuResources
{
	// Create a 2D image with view. Returns false on failure.
	bool CreateImage(const GpuDevice& dev, uint32_t width, uint32_t height,
	                 VkFormat format, VkImageUsageFlags usage,
	                 VkMemoryPropertyFlags memProperties,
	                 GpuImage& outImage);

	// Create a buffer with given size/usage/memory. Returns false on failure.
	bool CreateBuffer(const GpuDevice& dev, VkDeviceSize size,
	                  VkBufferUsageFlags usage, VkMemoryPropertyFlags memProperties,
	                  GpuBuffer& outBuffer);

	// Convenience overload for callers that hold decomposed VkBuffer/VkDeviceMemory pairs.
	bool CreateBuffer(const GpuDevice& dev, VkDeviceSize size,
	                  VkBufferUsageFlags usage, VkMemoryPropertyFlags memProperties,
	                  VkBuffer& outBuffer, VkDeviceMemory& outMemory);

	// Destroy an image (image + memory + view). Safe to call on zeroed struct.
	void DestroyImage(const GpuDevice& dev, GpuImage& img);

	// Destroy a buffer (buffer + memory). Safe to call on zeroed struct.
	void DestroyBuffer(const GpuDevice& dev, GpuBuffer& buf);

	// Convenience overload for decomposed VkBuffer/VkDeviceMemory pairs.
	void DestroyBuffer(const GpuDevice& dev, VkBuffer& buffer, VkDeviceMemory& memory);

	// Transition an image between layouts via pipeline barrier in the given cmd buffer.
	void TransitionImage(VkCommandBuffer cmd, VkImage image,
	                     VkAccessFlags srcAccess, VkAccessFlags dstAccess,
	                     VkImageLayout oldLayout, VkImageLayout newLayout,
	                     VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage);

	// Upload data to a buffer via a staging buffer + one-time submit.
	// The host-visible staging buffer is destroyed after the copy completes.
	bool UploadToBuffer(const GpuDevice& dev, VkBuffer dstBuffer,
	                     const void* data, VkDeviceSize size);

	// Create a sampler with standard linear/clamp settings.
	VkSampler CreateSampler(const GpuDevice& dev,
	                         VkFilter magFilter = VK_FILTER_LINEAR,
	                         VkFilter minFilter = VK_FILTER_LINEAR);
	void DestroySampler(const GpuDevice& dev, VkSampler& sampler);
}