#include "GpuResources.h"
#include "GpuDevice.h"
#include "VulkanUtils.h"
#include "RTLog.h"

namespace GpuResources
{
	bool CreateImage(const GpuDevice& dev, uint32_t width, uint32_t height,
	                 VkFormat format, VkImageUsageFlags usage,
	                 VkMemoryPropertyFlags memProps,
	                 GpuImage& outImage, uint32_t mipLevels)
	{
		VkImageCreateInfo imageInfo = {};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.format = format;
		imageInfo.extent = { width, height, 1 };
		imageInfo.mipLevels = mipLevels;
		imageInfo.arrayLayers = 1;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.usage = usage;
		imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		VK_CHECK(vkCreateImage(dev.device, &imageInfo, nullptr, &outImage.image));

		VkMemoryRequirements memReqs;
		vkGetImageMemoryRequirements(dev.device, outImage.image, &memReqs);

		VkMemoryAllocateInfo allocInfo = {};
		allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memReqs.size;
		allocInfo.memoryTypeIndex = dev.FindMemoryType(memReqs.memoryTypeBits, memProps);
		if (allocInfo.memoryTypeIndex == 0xFFFFFFFF)
		{
			RT_LOG("[GpuResources::CreateImage] no valid memory type");
			vkDestroyImage(dev.device, outImage.image, nullptr);
			outImage.image = VK_NULL_HANDLE;
			return false;
		}

	static int s_createImageCount = 0;
	s_createImageCount++;
	RT_LOG("[GpuResources::CreateImage #%d] %ux%u mip=%u: allocSize=%zu typeBits=0x%X memType=%u props=0x%X",
	       s_createImageCount, width, height, mipLevels, (size_t)memReqs.size, memReqs.memoryTypeBits,
	       allocInfo.memoryTypeIndex, memProps);
		dev.LogMemoryUsage("CreateImage pre-alloc");

		VK_CHECK(vkAllocateMemory(dev.device, &allocInfo, nullptr, &outImage.memory));
		VK_CHECK(vkBindImageMemory(dev.device, outImage.image, outImage.memory, 0));

		VkImageViewCreateInfo viewInfo = {};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = outImage.image;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = format;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.levelCount = mipLevels;
		viewInfo.subresourceRange.layerCount = 1;

		VK_CHECK(vkCreateImageView(dev.device, &viewInfo, nullptr, &outImage.view));

		outImage.format = format;
		outImage.width = width;
		outImage.height = height;
		return true;
	}

	bool CreateImage1D(const GpuDevice& dev, uint32_t width, uint32_t height,
	                 VkFormat format, VkImageUsageFlags usage,
	                 VkMemoryPropertyFlags memProps,
	                 GpuImage& outImage)
	{
		bool is1D = (height == 1);

		VkImageCreateInfo imageInfo = {};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = is1D ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D;
		imageInfo.format = format;
		imageInfo.extent = { width, is1D ? 1u : height, 1 };
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = 1;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.usage = usage;
		imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		VK_CHECK(vkCreateImage(dev.device, &imageInfo, nullptr, &outImage.image));

		VkMemoryRequirements memReqs;
		vkGetImageMemoryRequirements(dev.device, outImage.image, &memReqs);

		VkMemoryAllocateInfo allocInfo = {};
		allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocInfo.allocationSize = memReqs.size;
		allocInfo.memoryTypeIndex = dev.FindMemoryType(memReqs.memoryTypeBits, memProps);
		if (allocInfo.memoryTypeIndex == 0xFFFFFFFF)
		{
			RT_LOG("[GpuResources::CreateImage1D] no valid memory type");
			vkDestroyImage(dev.device, outImage.image, nullptr);
			outImage.image = VK_NULL_HANDLE;
			return false;
		}

		VK_CHECK(vkAllocateMemory(dev.device, &allocInfo, nullptr, &outImage.memory));
		VK_CHECK(vkBindImageMemory(dev.device, outImage.image, outImage.memory, 0));

		VkImageViewCreateInfo viewInfo = {};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = outImage.image;
		viewInfo.viewType = is1D ? VK_IMAGE_VIEW_TYPE_1D : VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = format;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.layerCount = 1;

		VK_CHECK(vkCreateImageView(dev.device, &viewInfo, nullptr, &outImage.view));

		outImage.format = format;
		outImage.width = width;
		outImage.height = height;
		return true;
	}

	bool CreateBuffer(const GpuDevice& dev, VkDeviceSize size,
	                  VkBufferUsageFlags usage, VkMemoryPropertyFlags memProps,
	                  GpuBuffer& outBuffer)
	{
		VkBufferCreateInfo bufferInfo = {};
		bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufferInfo.size = size;
		bufferInfo.usage = usage;
		bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		VK_CHECK(vkCreateBuffer(dev.device, &bufferInfo, nullptr, &outBuffer.buffer));

		VkMemoryRequirements memReqs;
		vkGetBufferMemoryRequirements(dev.device, outBuffer.buffer, &memReqs);

		VkMemoryAllocateInfo allocInfo = {};
		allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocInfo.allocationSize = memReqs.size;
		allocInfo.memoryTypeIndex = dev.FindMemoryType(memReqs.memoryTypeBits, memProps);
		if (allocInfo.memoryTypeIndex == 0xFFFFFFFF)
		{
			RT_LOG("[GpuResources::CreateBuffer] no valid memory type");
			vkDestroyBuffer(dev.device, outBuffer.buffer, nullptr);
			outBuffer.buffer = VK_NULL_HANDLE;
			return false;
		}

		VkMemoryAllocateFlagsInfo flagsInfo = {};
		if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
		{
			flagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
			flagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
			allocInfo.pNext = &flagsInfo;
		}

		VK_CHECK(vkAllocateMemory(dev.device, &allocInfo, nullptr, &outBuffer.memory));
		VK_CHECK(vkBindBufferMemory(dev.device, outBuffer.buffer, outBuffer.memory, 0));

		outBuffer.size = size;
		return true;
	}

	bool CreateBuffer(const GpuDevice& dev, VkDeviceSize size,
	                  VkBufferUsageFlags usage, VkMemoryPropertyFlags memProps,
	                  VkBuffer& outBuffer, VkDeviceMemory& outMemory)
	{
		GpuBuffer tmp;
		if (!CreateBuffer(dev, size, usage, memProps, tmp))
		{
			outBuffer = VK_NULL_HANDLE;
			outMemory = VK_NULL_HANDLE;
			return false;
		}
		outBuffer = tmp.buffer;
		outMemory = tmp.memory;
		return true;
	}

	void DestroyImage(const GpuDevice& dev, GpuImage& img)
	{
		if (img.view)   { vkDestroyImageView(dev.device, img.view, nullptr);   img.view = VK_NULL_HANDLE; }
		if (img.image)  { vkDestroyImage(dev.device, img.image, nullptr);      img.image = VK_NULL_HANDLE; }
		if (img.memory) { vkFreeMemory(dev.device, img.memory, nullptr);       img.memory = VK_NULL_HANDLE; }
	}

	void DestroyBuffer(const GpuDevice& dev, GpuBuffer& buf)
	{
		if (buf.buffer) { vkDestroyBuffer(dev.device, buf.buffer, nullptr);  buf.buffer = VK_NULL_HANDLE; }
		if (buf.memory)  { vkFreeMemory(dev.device, buf.memory, nullptr);     buf.memory = VK_NULL_HANDLE; }
		buf.size = 0;
	}

	void DestroyBuffer(const GpuDevice& dev, VkBuffer& buffer, VkDeviceMemory& memory)
	{
		if (buffer) { vkDestroyBuffer(dev.device, buffer, nullptr); buffer = VK_NULL_HANDLE; }
		if (memory) { vkFreeMemory(dev.device, memory, nullptr);    memory = VK_NULL_HANDLE; }
	}

	void TransitionImage(VkCommandBuffer cmd, VkImage image,
	                     VkAccessFlags srcAccess, VkAccessFlags dstAccess,
	                     VkImageLayout oldLayout, VkImageLayout newLayout,
	                     VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
	{
		VkImageMemoryBarrier barrier = {};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcAccessMask = srcAccess;
		barrier.dstAccessMask = dstAccess;
		barrier.oldLayout = oldLayout;
		barrier.newLayout = newLayout;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.layerCount = 1;

		vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0,
		                     0, nullptr, 0, nullptr, 1, &barrier);
	}

	VkSampler CreateSampler(const GpuDevice& dev,
	                         VkFilter magFilter, VkFilter minFilter)
	{
		VkSamplerCreateInfo samplerInfo = {};
		samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		samplerInfo.magFilter = magFilter;
		samplerInfo.minFilter = minFilter;
		samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.minLod = -1000;
		samplerInfo.maxLod = 1000;

		VkSampler sampler = VK_NULL_HANDLE;
		VK_CHECK(vkCreateSampler(dev.device, &samplerInfo, nullptr, &sampler));
		return sampler;
	}

	VkSampler CreateSampler(const GpuDevice& dev,
	                         VkFilter magFilter, VkFilter minFilter,
	                         VkSamplerAddressMode addressMode,
	                         VkSamplerMipmapMode mipmapMode)
	{
		VkSamplerCreateInfo samplerInfo = {};
		samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		samplerInfo.magFilter = magFilter;
		samplerInfo.minFilter = minFilter;
		samplerInfo.mipmapMode = mipmapMode;
		samplerInfo.addressModeU = addressMode;
		samplerInfo.addressModeV = addressMode;
		samplerInfo.addressModeW = addressMode;
		samplerInfo.minLod = 0;
		samplerInfo.maxLod = 0;

		VkSampler sampler = VK_NULL_HANDLE;
		VK_CHECK(vkCreateSampler(dev.device, &samplerInfo, nullptr, &sampler));
		return sampler;
	}

	void DestroySampler(const GpuDevice& dev, VkSampler& sampler)
	{
		if (sampler) { vkDestroySampler(dev.device, sampler, nullptr); sampler = VK_NULL_HANDLE; }
	}
}