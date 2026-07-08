#include "AsyncTextureLoader.h"
#include "GpuDevice.h"
#include "CommandUtils.h"
#include "RTLog.h"
#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>
#include <algorithm>

AsyncTextureLoader::~AsyncTextureLoader()
{
	Destroy();
}

bool AsyncTextureLoader::Begin(const GpuDevice& dev,
                                const std::vector<SceneTexture>& textures,
                                const std::vector<float>& envMapFloatPixels,
                                int envMapWidth, int envMapHeight,
                                const std::vector<float>& marginalCDF,
                                const std::vector<float>& conditionalCDF,
                                int cdfWidth, int cdfHeight)
{
	if (m_Busy.load())
	{
		RT_LOG("[AsyncTex] Begin called while busy — ignoring");
		return false;
	}

	if (textures.empty() && envMapFloatPixels.empty())
	{
		RT_LOG("[AsyncTex] Begin: no textures to load");
		return false;
	}

	m_Device = &dev;

	m_EnvMapFloatPixels = envMapFloatPixels;
	m_EnvMapWidth  = envMapWidth;
	m_EnvMapHeight = envMapHeight;
	m_MarginalCDF  = marginalCDF;
	m_ConditionalCDF = conditionalCDF;
	m_CDFWidth  = cdfWidth;
	m_CDFHeight = cdfHeight;

	m_Busy.store(true);
	m_Complete.store(false);

	m_Thread = std::thread(&AsyncTextureLoader::WorkerThread, this,
	                       m_Device, textures,
	                       m_EnvMapFloatPixels,
	                       m_MarginalCDF, m_ConditionalCDF);

	return true;
}

bool AsyncTextureLoader::IsComplete() const
{
	if (!m_Busy.load()) return false;
	return m_Complete.load();
}

void AsyncTextureLoader::Adopt(std::vector<GpuImage>& outTextures,
                               int& envMapIndex,
                               int& marginalCDFIndex,
                               int& conditionalCDFIndex)
{
	if (!IsComplete())
	{
		RT_LOG("[AsyncTex] Adopt called but not complete");
		return;
	}

	if (m_Thread.joinable())
		m_Thread.join();

	outTextures = std::move(m_ResultTextures);
	envMapIndex = m_ResultEnvMapIndex;
	marginalCDFIndex = m_ResultMarginalCDFIndex;
	conditionalCDFIndex = m_ResultConditionalCDFIndex;

	m_ResultTextures.clear();

	if (m_UploadFence != VK_NULL_HANDLE && m_Device)
	{
		vkDestroyFence(m_Device->device, m_UploadFence, nullptr);
		m_UploadFence = VK_NULL_HANDLE;
	}
	if (m_CmdPool != VK_NULL_HANDLE && m_Device)
	{
		vkDestroyCommandPool(m_Device->device, m_CmdPool, nullptr);
		m_CmdPool = VK_NULL_HANDLE;
	}
	m_Staging.Destroy();

	m_Busy.store(false);
	m_Complete.store(false);
}

void AsyncTextureLoader::Cancel()
{
	if (m_Thread.joinable())
		m_Thread.join();
}

void AsyncTextureLoader::Destroy()
{
	Cancel();

	if (m_Device)
	{
		for (auto& img : m_ResultTextures)
			GpuResources::DestroyImage(*m_Device, img);
		m_ResultTextures.clear();

		if (m_UploadFence != VK_NULL_HANDLE)
		{
			vkDestroyFence(m_Device->device, m_UploadFence, nullptr);
			m_UploadFence = VK_NULL_HANDLE;
		}
		if (m_CmdPool != VK_NULL_HANDLE)
		{
			vkDestroyCommandPool(m_Device->device, m_CmdPool, nullptr);
			m_CmdPool = VK_NULL_HANDLE;
		}
	}
	m_Staging.Destroy();

	m_Busy.store(false);
	m_Complete.store(false);
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

struct AsyncUploadEntry {
	VkDeviceSize offset;
	VkDeviceSize size;
	bool isHDR;
	uint32_t mipLevels;
};

void AsyncTextureLoader::WorkerThread(const GpuDevice* devPtr,
                                       std::vector<SceneTexture> textures,
                                       std::vector<float> envMapFloat,
                                       std::vector<float> marginalCDF,
                                       std::vector<float> conditionalCDF)
{
	const GpuDevice& dev = *devPtr;
	VkDevice device = dev.device;

	// 1. Append env map as a texture (matches WalnutApp convention)
	int envMapIndex = -1;
	if (!envMapFloat.empty() && m_EnvMapWidth > 0 && m_EnvMapHeight > 0)
	{
		SceneTexture envTex;
		envTex.isHDR = true;
		envTex.width  = m_EnvMapWidth;
		envTex.height = m_EnvMapHeight;
		envTex.floatPixels = std::move(envMapFloat);
		textures.push_back(std::move(envTex));
		envMapIndex = (int)textures.size() - 1;
	}

	// 2. Compute total staging size for main textures + CDF textures
	VkDeviceSize totalStaging = 0;
	for (const auto& tex : textures)
	{
		if (tex.floatPixels.empty() && tex.pixels.empty()) continue;
		bool isHDR = tex.isHDR && !tex.floatPixels.empty();
		VkDeviceSize sz = isHDR
			? (VkDeviceSize)(tex.width * tex.height * 8)
			: (VkDeviceSize)(tex.width * tex.height * 4);
		totalStaging += sz;
	}

	VkDeviceSize marginalSize = marginalCDF.empty() ? 0
		: (VkDeviceSize)(m_CDFHeight * 4);
	VkDeviceSize conditionalSize = conditionalCDF.empty() ? 0
		: (VkDeviceSize)(m_CDFWidth * m_CDFHeight * 4);
	totalStaging += marginalSize + conditionalSize;

	if (totalStaging == 0)
	{
		RT_LOG("[AsyncTex] No staging needed — creating empty textures only");
		m_ResultTextures.resize(textures.size());
		m_ResultEnvMapIndex = envMapIndex;
		m_ResultMarginalCDFIndex = -1;
		m_ResultConditionalCDFIndex = -1;
		m_Complete.store(true);
		return;
	}

	if (!m_Staging.Init(dev, totalStaging))
	{
		RT_LOG("[AsyncTex] Failed to init staging arena");
		m_ResultTextures.clear();
		m_ResultEnvMapIndex = -1;
		m_ResultMarginalCDFIndex = -1;
		m_ResultConditionalCDFIndex = -1;
		m_Complete.store(true);
		return;
	}

	std::vector<GpuImage> images(textures.size());
	std::vector<AsyncUploadEntry> uploads(textures.size());

	// 3. Create images + copy CPU pixels to staging
	for (size_t i = 0; i < textures.size(); i++)
	{
		const auto& tex = textures[i];
		if (tex.floatPixels.empty() && tex.pixels.empty()) continue;

		GpuImage& gt = images[i];
		gt.width  = tex.width;
		gt.height = tex.height;

		bool isHDR = tex.isHDR && !tex.floatPixels.empty();
		VkFormat format;
		if (isHDR)
			format = VK_FORMAT_R16G16B16A16_SFLOAT;
		else
			format = tex.isSRGB ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
		gt.format = format;

		uint32_t mipLevels = 1;
		uint32_t w = tex.width, h = tex.height;
		while (w > 1 || h > 1) { w = std::max(w / 2, 1u); h = std::max(h / 2, 1u); mipLevels++; }

		VkDeviceSize imageSize = isHDR
			? (VkDeviceSize)(tex.width * tex.height * 8)
			: (VkDeviceSize)(tex.width * tex.height * 4);

		VkDeviceSize offset = m_Staging.Alloc(imageSize, 16);
		if (offset == VK_WHOLE_SIZE)
		{
			RT_LOG("[AsyncTex] Texture %d: staging arena full", (int)i);
			continue;
		}

		if (isHDR)
		{
			uint16_t* dst = static_cast<uint16_t*>(m_Staging.GetMappedPointer(offset));
			for (size_t p = 0; p < tex.floatPixels.size(); p++)
				dst[p] = glm::packHalf1x16(tex.floatPixels[p]);
		}
		else
		{
			memcpy(m_Staging.GetMappedPointer(offset), tex.pixels.data(), (size_t)imageSize);
		}

		uploads[i] = { offset, imageSize, isHDR, mipLevels };

		GpuResources::CreateImage(dev, tex.width, tex.height, format,
			VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, gt, mipLevels);
	}

	// 4. Create CDF textures
	struct CDFEntry {
		GpuImage img;
		VkDeviceSize offset;
		int w, h;
	};
	CDFEntry cdfEntries[2];
	int marginalCDFIndex = -1;
	int conditionalCDFIndex = -1;
	bool hasCDF = !marginalCDF.empty() && !conditionalCDF.empty() && envMapIndex >= 0;

	if (hasCDF)
	{
		VkDeviceSize off = m_Staging.Alloc(marginalSize, 16);
		memcpy(m_Staging.GetMappedPointer(off), marginalCDF.data(), (size_t)marginalSize);
		cdfEntries[0].img.width = m_CDFHeight;
		cdfEntries[0].img.height = 1;
		cdfEntries[0].img.format = VK_FORMAT_R32_SFLOAT;
		cdfEntries[0].offset = off;
		cdfEntries[0].w = m_CDFHeight;
		cdfEntries[0].h = 1;
		GpuResources::CreateImage1D(dev, m_CDFHeight, 1, VK_FORMAT_R32_SFLOAT,
			VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, cdfEntries[0].img);

		off = m_Staging.Alloc(conditionalSize, 16);
		memcpy(m_Staging.GetMappedPointer(off), conditionalCDF.data(), (size_t)conditionalSize);
		cdfEntries[1].img.width = m_CDFWidth;
		cdfEntries[1].img.height = m_CDFHeight;
		cdfEntries[1].img.format = VK_FORMAT_R32_SFLOAT;
		cdfEntries[1].offset = off;
		cdfEntries[1].w = m_CDFWidth;
		cdfEntries[1].h = m_CDFHeight;
		GpuResources::CreateImage1D(dev, m_CDFWidth, m_CDFHeight, VK_FORMAT_R32_SFLOAT,
			VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, cdfEntries[1].img);
	}

	// 5. Create dedicated command pool (thread-local)
	VkCommandPoolCreateInfo poolInfo = {};
	poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	poolInfo.queueFamilyIndex = dev.queueFamily;
	if (vkCreateCommandPool(device, &poolInfo, nullptr, &m_CmdPool) != VK_SUCCESS)
	{
		RT_LOG("[AsyncTex] Failed to create command pool");
		m_Complete.store(true);
		return;
	}

	VkCommandBufferAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.commandPool = m_CmdPool;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = 1;

	VkCommandBuffer cmd;
	if (vkAllocateCommandBuffers(device, &allocInfo, &cmd) != VK_SUCCESS)
	{
		RT_LOG("[AsyncTex] Failed to allocate command buffer");
		m_Complete.store(true);
		return;
	}

	VkCommandBufferBeginInfo beginInfo = {};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &beginInfo);

	// 6. Record copy + mip blits for each texture
	for (size_t i = 0; i < textures.size(); i++)
	{
		const auto& tex = textures[i];
		if (tex.floatPixels.empty() && tex.pixels.empty()) continue;

		GpuImage& gt = images[i];
		const auto& up = uploads[i];

		VkImageMemoryBarrier barrier = {};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = gt.image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.baseMipLevel = 0;
		barrier.subresourceRange.levelCount = up.mipLevels;
		barrier.subresourceRange.layerCount = 1;

		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

		VkBufferImageCopy region = {};
		region.bufferOffset = up.offset;
		region.bufferRowLength = 0;
		region.bufferImageHeight = 0;
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.mipLevel = 0;
		region.imageSubresource.baseArrayLayer = 0;
		region.imageSubresource.layerCount = 1;
		region.imageOffset = {0, 0, 0};
		region.imageExtent = {(uint32_t)tex.width, (uint32_t)tex.height, 1};

		vkCmdCopyBufferToImage(cmd, m_Staging.GetBuffer(), gt.image,
		                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

		if (up.mipLevels > 1)
		{
			uint32_t mipW = (uint32_t)tex.width;
			uint32_t mipH = (uint32_t)tex.height;

			for (uint32_t mip = 1; mip < up.mipLevels; mip++)
			{
				VkImageMemoryBarrier srcBarrier = {};
				srcBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
				srcBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				srcBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
				srcBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				srcBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
				srcBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				srcBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				srcBarrier.image = gt.image;
				srcBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				srcBarrier.subresourceRange.baseMipLevel = mip - 1;
				srcBarrier.subresourceRange.levelCount = 1;
				srcBarrier.subresourceRange.layerCount = 1;

				vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
				                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &srcBarrier);

				uint32_t dstW = std::max(mipW / 2, 1u);
				uint32_t dstH = std::max(mipH / 2, 1u);

				VkImageBlit blit = {};
				blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				blit.srcSubresource.mipLevel = mip - 1;
				blit.srcSubresource.baseArrayLayer = 0;
				blit.srcSubresource.layerCount = 1;
				blit.srcOffsets[0] = {0, 0, 0};
				blit.srcOffsets[1] = {(int32_t)mipW, (int32_t)mipH, 1};
				blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				blit.dstSubresource.mipLevel = mip;
				blit.dstSubresource.baseArrayLayer = 0;
				blit.dstSubresource.layerCount = 1;
				blit.dstOffsets[0] = {0, 0, 0};
				blit.dstOffsets[1] = {(int32_t)dstW, (int32_t)dstH, 1};

				vkCmdBlitImage(cmd, gt.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				              gt.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				              1, &blit, VK_FILTER_LINEAR);

				mipW = dstW;
				mipH = dstH;
			}
		}

		VkImageMemoryBarrier shaderBarrier = {};
		shaderBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		shaderBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		shaderBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		shaderBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		shaderBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		shaderBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		shaderBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		shaderBarrier.image = gt.image;
		shaderBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		shaderBarrier.subresourceRange.baseMipLevel = 0;
		shaderBarrier.subresourceRange.levelCount = up.mipLevels;
		shaderBarrier.subresourceRange.layerCount = 1;

		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &shaderBarrier);
	}

	// 7. Record CDF texture copies
	if (hasCDF)
	{
		for (int i = 0; i < 2; i++)
		{
			const auto& e = cdfEntries[i];
			VkImageMemoryBarrier barrier = {};
			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barrier.image = e.img.image;
			barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			barrier.subresourceRange.levelCount = 1;
			barrier.subresourceRange.layerCount = 1;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

			VkBufferImageCopy region = {};
			region.bufferOffset = e.offset;
			region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			region.imageSubresource.layerCount = 1;
			region.imageExtent = {(uint32_t)e.w, (e.h == 1) ? 1u : (uint32_t)e.h, 1};
			vkCmdCopyBufferToImage(cmd, m_Staging.GetBuffer(), e.img.image,
			                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

			VkImageMemoryBarrier shaderBarrier = barrier;
			shaderBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			shaderBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			shaderBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			shaderBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
			                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &shaderBarrier);
		}
	}

	vkEndCommandBuffer(cmd);

	// 8. Create fence + submit (non-blocking)
	VkFenceCreateInfo fenceInfo = {};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	if (vkCreateFence(device, &fenceInfo, nullptr, &m_UploadFence) != VK_SUCCESS)
	{
		RT_LOG("[AsyncTex] Failed to create fence");
		m_Complete.store(true);
		return;
	}

	VkSubmitInfo submitInfo = {};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &cmd;
	if (vkQueueSubmit(dev.queue, 1, &submitInfo, m_UploadFence) != VK_SUCCESS)
	{
		RT_LOG("[AsyncTex] Failed to submit command buffer");
		m_Complete.store(true);
		return;
	}

	// Append CDF textures to the images array
	if (hasCDF)
	{
		marginalCDFIndex = (int)images.size();
		images.push_back(cdfEntries[0].img);
		conditionalCDFIndex = (int)images.size();
		images.push_back(cdfEntries[1].img);
	}

	m_ResultTextures     = std::move(images);
	m_ResultEnvMapIndex  = envMapIndex;
	m_ResultMarginalCDFIndex    = marginalCDFIndex;
	m_ResultConditionalCDFIndex = conditionalCDFIndex;

	RT_LOG("[AsyncTex] Worker done: %d textures queued (envMap=%d, margCDF=%d, condCDF=%d)",
	       (int)m_ResultTextures.size(), m_ResultEnvMapIndex,
	       m_ResultMarginalCDFIndex, m_ResultConditionalCDFIndex);

	m_Complete.store(true);
}