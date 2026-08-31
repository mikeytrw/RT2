#include "GBufferTarget.h"
#include "GpuDevice.h"
#include "CommandUtils.h"

GBufferTarget::~GBufferTarget()
{
	Destroy();
}

void GBufferTarget::Create(const GpuDevice& dev, const RenderExtent& extent)
{
	if (!extent.IsValid()) return;
	const uint32_t width = extent.Width();
	const uint32_t height = extent.Height();
	Destroy();

	m_Device = &dev;
	m_Extent = extent;

	VkImageUsageFlags colorUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
	                               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

	for (uint32_t i = 0; i < COLOR_COUNT; i++)
	{
		if (i == 6) continue; // slot 6 = NRD UBO, not a color image
		GpuResources::CreateImage(dev, width, height, GetColorFormat(i),
			colorUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_ColorImages[i]);
	}

	// Transition all color images to GENERAL layout in a single command buffer
	CommandUtils::ImmediateSubmit(dev, [&](VkCommandBuffer cmd) {
		for (uint32_t i = 0; i < COLOR_COUNT; i++)
		{
			if (i == 6) continue; // slot 6 = NRD UBO, not a color image
			VkImageMemoryBarrier barrier = {};
			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
			barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = m_ColorImages[i].image;
			barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			barrier.subresourceRange.levelCount = 1;
			barrier.subresourceRange.layerCount = 1;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
			                     VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0,
			                     0, nullptr, 0, nullptr, 1, &barrier);
		}
	});

	// Create depth image
	VkDevice device = dev.device;
	VkImageCreateInfo depthInfo = {};
	depthInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	depthInfo.imageType = VK_IMAGE_TYPE_2D;
	depthInfo.format = DEPTH_FORMAT;
	depthInfo.extent = { width, height, 1 };
	depthInfo.mipLevels = 1;
	depthInfo.arrayLayers = 1;
	depthInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	depthInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	depthInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	depthInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	vkCreateImage(device, &depthInfo, nullptr, &m_DepthImage.image);

	VkMemoryRequirements memReq;
	vkGetImageMemoryRequirements(device, m_DepthImage.image, &memReq);
	uint32_t memType = dev.FindMemoryType(memReq.memoryTypeBits,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memReq.size;
	allocInfo.memoryTypeIndex = memType;
	vkAllocateMemory(device, &allocInfo, nullptr, &m_DepthImage.memory);
	vkBindImageMemory(device, m_DepthImage.image, m_DepthImage.memory, 0);

	VkImageViewCreateInfo viewInfo = {};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = m_DepthImage.image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = DEPTH_FORMAT;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = 1;
	vkCreateImageView(device, &viewInfo, nullptr, &m_DepthImage.view);
}

void GBufferTarget::Destroy()
{
	if (!m_Device) return;

	for (uint32_t i = 0; i < COLOR_COUNT; i++)
	{
		if (i == 6) continue; // slot 6 = NRD UBO, not a color image
		GpuResources::DestroyImage(*m_Device, m_ColorImages[i]);
	}

	GpuResources::DestroyImage(*m_Device, m_DepthImage);

	m_Extent = {};
	m_Device = nullptr;
}

void GBufferTarget::GetMRTViews(VkImageView outViews[8]) const
{
	for (int i = 0; i < 8; i++)
		outViews[i] = m_ColorImages[MRT_TO_COLOR[i]].view;
}

void GBufferTarget::GetMRTImages(VkImage outImages[8]) const
{
	for (int i = 0; i < 8; i++)
		outImages[i] = m_ColorImages[MRT_TO_COLOR[i]].image;
}

VkFormat GBufferTarget::GetColorFormat(uint32_t index)
{
	switch (index)
	{
	case NORMAL_ROUGHNESS:  return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
	case VIEWZ:             return VK_FORMAT_R32_SFLOAT;
	case MOTION:            return VK_FORMAT_R16G16_SFLOAT;
	case DIFF_RADIANCE:     return VK_FORMAT_R16G16B16A16_SFLOAT;
	case SPEC_RADIANCE:     return VK_FORMAT_R16G16B16A16_SFLOAT;
	case ALBEDO_F0:         return VK_FORMAT_R16G16B16A16_SFLOAT;
	case DIRECT_EMISSION:   return VK_FORMAT_R16G16B16A16_SFLOAT;
	case PRIM_HIT:          return VK_FORMAT_R32G32B32A32_SFLOAT;
	case PRIM_GEO_NORMAL:   return VK_FORMAT_R8G8B8A8_UNORM;
	case PRIM_UV:           return VK_FORMAT_R16G16_SFLOAT;
	case NRD_DIFF_OUT:      return VK_FORMAT_R16G16B16A16_SFLOAT;
	case NRD_SPEC_OUT:      return VK_FORMAT_R16G16B16A16_SFLOAT;
	default:                return VK_FORMAT_UNDEFINED;
	}
}
