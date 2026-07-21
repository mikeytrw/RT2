#include "AsyncTextureLoader.h"
#include "GpuDevice.h"
#include "CommandUtils.h"
#include "RTLog.h"
#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>
#include <algorithm>
#include <cmath>

AsyncTextureLoader::~AsyncTextureLoader()
{
	Destroy();
}

bool AsyncTextureLoader::Begin(const GpuDevice& dev,
                                const std::vector<SceneTexture>& textures,
                                int envMapIndex,
                                const std::vector<float>& marginalCDF,
                                const std::vector<float>& conditionalCDF,
                                int cdfWidth, int cdfHeight)
{
	if (m_Busy.load())
	{
		RT_LOG("[AsyncTex] Begin called while busy — ignoring");
		return false;
	}

	if (textures.empty())
	{
		RT_LOG("[AsyncTex] Begin: no textures to load");
		return false;
	}

	m_Device = &dev;

	m_EnvMapIndex  = envMapIndex;
	m_MarginalCDF  = marginalCDF;
	m_ConditionalCDF = conditionalCDF;
	m_CDFWidth  = cdfWidth;
	m_CDFHeight = cdfHeight;

	m_Busy.store(true);
	m_Complete.store(false);

	m_Thread = std::thread(&AsyncTextureLoader::WorkerThread, this,
	                       m_Device, textures,
	                       m_EnvMapIndex,
	                       m_MarginalCDF, m_ConditionalCDF);

	return true;
}

bool AsyncTextureLoader::IsComplete() const
{
	if (!m_Busy.load()) return false;
	if (!m_Complete.load()) return false;
	return true;
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

	printf("[AsyncTex] Adopt: joining thread...\n"); fflush(stdout);
	if (m_Thread.joinable())
		m_Thread.join();

	// Submit the recorded command buffer from the main thread (queues
	// are not thread-safe — worker only records).
	bool submitFailed = false;
	bool waitFailed   = false;
	if (m_CmdBuffer != VK_NULL_HANDLE && m_UploadFence != VK_NULL_HANDLE && m_Device)
	{
		printf("[AsyncTex] Adopt: vkQueueSubmit...\n"); fflush(stdout);
		VkSubmitInfo submitInfo = {};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &m_CmdBuffer;
		VkResult r = vkQueueSubmit(m_Device->queue, 1, &submitInfo, m_UploadFence);
		if (r != VK_SUCCESS)
		{
			// A failed submit never signals the fence. The old code logged
			// this and then fell through into vkWaitForFences(UINT64_MAX),
			// which froze the main thread forever — the UI hung on
			// "Uploading textures to GPU...". Never wait on a fence that was
			// not successfully submitted.
			RT_LOG("[AsyncTex] vkQueueSubmit failed: %d — skipping fence wait", (int)r);
			printf("[AsyncTex] Adopt: vkQueueSubmit FAILED (%d) — skipping fence wait\n", (int)r);
			fflush(stdout);
			submitFailed = true;
		}
		else
		{
			// Bounded wait. An unbounded wait turns any lost or
			// never-signalled fence into an unrecoverable freeze; slicing it
			// keeps a stuck upload visible in the console and recoverable.
			printf("[AsyncTex] Adopt: vkWaitForFences...\n"); fflush(stdout);
			constexpr uint64_t kSliceNs   = 1000000000ull; // 1 second
			constexpr int      kMaxSlices = 30;            // 30 second budget
			VkResult w = VK_TIMEOUT;
			for (int i = 0; i < kMaxSlices; ++i)
			{
				w = vkWaitForFences(m_Device->device, 1, &m_UploadFence, VK_TRUE, kSliceNs);
				if (w != VK_TIMEOUT) break;
				printf("[AsyncTex] Adopt: still waiting for upload fence (%ds)\n", i + 1);
				fflush(stdout);
			}
			if (w != VK_SUCCESS)
			{
				RT_LOG("[AsyncTex] upload fence wait failed/timed out: %d", (int)w);
				printf("[AsyncTex] Adopt: fence wait failed/timed out (%d)\n", (int)w);
				fflush(stdout);
				waitFailed = true;
			}
			else
			{
				printf("[AsyncTex] Adopt: vkWaitForFences returned\n"); fflush(stdout);
			}
		}
	}

	if (submitFailed || waitFailed)
	{
		// The upload did not complete — do not hand these images to the
		// renderer. On a failed submit nothing is in flight, so the images
		// are safe to destroy. On a timeout the GPU may still reference
		// them, so deliberately leak rather than risk a use-after-free.
		if (submitFailed && m_Device)
		{
			for (auto& img : m_ResultTextures)
				GpuResources::DestroyImage(*m_Device, img);
		}
		m_ResultTextures.clear();
		outTextures.clear();
		envMapIndex = -1;
		marginalCDFIndex = -1;
		conditionalCDFIndex = -1;
	}
	else
	{
		outTextures = std::move(m_ResultTextures);
		envMapIndex = m_ResultEnvMapIndex;
		marginalCDFIndex = m_ResultMarginalCDFIndex;
		conditionalCDFIndex = m_ResultConditionalCDFIndex;
	}

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

	// Destroy per-texture staging buffers (kept alive until fence signalled)
	if (m_Device)
	{
		for (size_t i = 0; i < m_PendingStagingBufs.size(); i++)
			GpuResources::DestroyBuffer(*m_Device, m_PendingStagingBufs[i], m_PendingStagingMems[i]);
	}
	m_PendingStagingBufs.clear();
	m_PendingStagingMems.clear();

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
		for (size_t i = 0; i < m_PendingStagingBufs.size(); i++)
			GpuResources::DestroyBuffer(*m_Device, m_PendingStagingBufs[i], m_PendingStagingMems[i]);
	}
	m_PendingStagingBufs.clear();
	m_PendingStagingMems.clear();
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
                                       int envMapIndex,
                                       std::vector<float> marginalCDF,
                                       std::vector<float> conditionalCDF)
{
	const GpuDevice& dev = *devPtr;
	VkDevice device = dev.device;

	RT_LOG("[AsyncTex] WorkerThread: textures.size=%zu, envMapIndex=%d", textures.size(), envMapIndex);
	dev.LogMemoryUsage("AsyncTex worker start");

	// 1. Compute total staging size for main textures + CDF textures
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
	VkDeviceSize cdfStaging = marginalSize + conditionalSize;

	if (totalStaging == 0 && cdfStaging == 0)
	{
		RT_LOG("[AsyncTex] No staging needed — creating empty textures only");
		m_ResultTextures.resize(textures.size());
		m_ResultEnvMapIndex = envMapIndex;
		m_ResultMarginalCDFIndex = -1;
		m_ResultConditionalCDFIndex = -1;
		m_Complete.store(true);
		return;
	}

	// Use per-texture staging to avoid allocating all textures at once.
	// For large scenes (265 textures × 4K = ~17GB), a single staging arena
	// would exceed HOST_VISIBLE memory. Instead, create+map+copy one staging
	// buffer per texture, but record ALL copy commands into a single command
	// buffer that is submitted from the main thread in Adopt().
	const VkDeviceSize MAX_STAGING_CHUNK = 64 * 1024 * 1024; // 64MB threshold
	bool usePerTextureStaging = (totalStaging > MAX_STAGING_CHUNK);

	if (!usePerTextureStaging && totalStaging + cdfStaging > 0)
	{
		if (!m_Staging.Init(dev, totalStaging + cdfStaging))
		{
			RT_LOG("[AsyncTex] Staging arena failed (%zuMB), falling back to per-texture",
			       (size_t)(totalStaging + cdfStaging) / (1024 * 1024));
			usePerTextureStaging = true;
		}
	}

	// When using per-texture staging, still need a small arena for CDF textures
	if (usePerTextureStaging && cdfStaging > 0)
	{
		if (!m_Staging.Init(dev, cdfStaging))
		{
			RT_LOG("[AsyncTex] CDF staging arena failed (%zuMB), CDFs will be skipped",
			       (size_t)(cdfStaging) / (1024 * 1024));
			cdfStaging = 0;
		}
	}

	std::vector<GpuImage> images(textures.size());
	std::vector<AsyncUploadEntry> uploads(textures.size());

	// 3. Create dedicated command pool+buffer early — ALL upload commands
	// (both per-texture and arena paths) record into this single buffer.
	// It is submitted from the main thread in Adopt(), never from the worker.
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

	// 4. Create images + copy CPU pixels to staging + record GPU commands
	size_t nonEmptyCount = 0;

	for (size_t i = 0; i < textures.size(); i++)
	{
		const auto& tex = textures[i];
		if (tex.floatPixels.empty() && tex.pixels.empty()) continue;

		if (usePerTextureStaging && nonEmptyCount % 50 == 0)
			RT_LOG("[AsyncTex] uploaded %zu/%zu textures", nonEmptyCount, textures.size());
		nonEmptyCount++;

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

		// Determine the staging source for this texture
		VkBuffer stagingSrcBuf;
		VkDeviceSize stagingOffset;

		if (usePerTextureStaging)
		{
			// Per-texture staging: create a dedicated buffer, copy pixels,
			// keep it alive until Adopt() destroys it after fence signals.
			VkBuffer stageBuf; VkDeviceMemory stageMem;
			GpuResources::CreateBuffer(dev, imageSize,
			             VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			             stageBuf, stageMem);
			void* mapped = nullptr;
			vkMapMemory(device, stageMem, 0, imageSize, 0, &mapped);
			if (isHDR)
			{
				uint16_t* dst = static_cast<uint16_t*>(mapped);
				for (size_t p = 0; p < tex.floatPixels.size(); p++)
				{
					float v = tex.floatPixels[p];
					if (isnan(v) || isinf(v))
						v = 0.0f;
					v = glm::min(v, 65504.0f);
					dst[p] = glm::packHalf1x16(v);
				}
			}
			else
			{
				memcpy(mapped, tex.pixels.data(), (size_t)imageSize);
			}
			vkUnmapMemory(device, stageMem);
			m_PendingStagingBufs.push_back(stageBuf);
			m_PendingStagingMems.push_back(stageMem);

			stagingSrcBuf = stageBuf;
			stagingOffset = 0;
		}
		else
		{
			// Arena staging: sub-allocate from the shared arena
			stagingOffset = m_Staging.Alloc(imageSize, 16);
			if (stagingOffset == VK_WHOLE_SIZE)
			{
				RT_LOG("[AsyncTex] Texture %d: staging arena full", (int)i);
				continue;
			}

			if (isHDR)
			{
				uint16_t* dst = static_cast<uint16_t*>(m_Staging.GetMappedPointer(stagingOffset));
				for (size_t p = 0; p < tex.floatPixels.size(); p++)
				{
					float v = tex.floatPixels[p];
					if (isnan(v) || isinf(v))
						v = 0.0f;
					v = glm::min(v, 65504.0f);
					dst[p] = glm::packHalf1x16(v);
				}
			}
			else
			{
				memcpy(m_Staging.GetMappedPointer(stagingOffset), tex.pixels.data(), (size_t)imageSize);
			}

			stagingSrcBuf = m_Staging.GetBuffer();
		}

		GpuResources::CreateImage(dev, tex.width, tex.height, format,
			VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, gt, mipLevels);

		// Layout transition: UNDEFINED → TRANSFER_DST_OPTIMAL
		VkImageMemoryBarrier barrier = {};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = gt.image;
		barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 1};
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

		// Copy from staging buffer to image
		VkBufferImageCopy region = {};
		region.bufferOffset = stagingOffset;
		region.bufferRowLength = 0;
		region.bufferImageHeight = 0;
		region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		region.imageOffset = {0, 0, 0};
		region.imageExtent = {(uint32_t)tex.width, (uint32_t)tex.height, 1};
		vkCmdCopyBufferToImage(cmd, stagingSrcBuf, gt.image,
		                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

		// Mip blits
		if (mipLevels > 1)
		{
			uint32_t mipW = (uint32_t)tex.width, mipH = (uint32_t)tex.height;
			for (uint32_t mip = 1; mip < mipLevels; mip++)
			{
				VkImageMemoryBarrier srcB = {};
				srcB.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
				srcB.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				srcB.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
				srcB.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				srcB.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
				srcB.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				srcB.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				srcB.image = gt.image;
				srcB.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 1, 0, 1};
				vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
				                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &srcB);

				VkImageBlit blit = {};
				blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 0, 1};
				blit.srcOffsets[0] = {0, 0, 0};
				blit.srcOffsets[1] = {(int32_t)mipW, (int32_t)mipH, 1};
				blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
				blit.dstOffsets[0] = {0, 0, 0};
				int32_t mipW2 = std::max(mipW / 2, 1u);
				int32_t mipH2 = std::max(mipH / 2, 1u);
				blit.dstOffsets[1] = {mipW2, mipH2, 1};
				vkCmdBlitImage(cmd, gt.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				               gt.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				               1, &blit, VK_FILTER_LINEAR);
				mipW = mipW2; mipH = mipH2;
			}
		}

		// Generated mips 0..N-2 are TRANSFER_SRC_OPTIMAL; only the final mip
		// remains TRANSFER_DST_OPTIMAL. Transition the two ranges separately.
		// A single TRANSFER_DST transition across every level is invalid and can
		// leave sampled environment textures in undefined layout state.
		const VkPipelineStageFlags shaderReadStages =
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
			VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
		VkImageMemoryBarrier finalB = {};
		finalB.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		finalB.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
		finalB.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		finalB.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		finalB.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		finalB.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		finalB.image = gt.image;
		finalB.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

		if (mipLevels > 1)
		{
			VkImageMemoryBarrier finalBarriers[2] = { finalB, finalB };
			finalBarriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			finalBarriers[0].subresourceRange.levelCount = mipLevels - 1;
			finalBarriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			finalBarriers[1].subresourceRange.baseMipLevel = mipLevels - 1;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, shaderReadStages,
			                     0, 0, nullptr, 0, nullptr, 2, finalBarriers);
		}
		else
		{
			finalB.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, shaderReadStages,
			                     0, 0, nullptr, 0, nullptr, 1, &finalB);
		}

		uploads[i] = { stagingOffset, imageSize, isHDR, mipLevels };
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
		// Bindless textures are declared as sampler2D in every shader. Keep the
		// marginal CDF as a height-by-1 2D image as well; binding a 1D image view
		// to sampler2D is descriptor/view dimensional mismatch and makes texelFetch
		// results undefined (observed as near-zero PDF differences in ReSTIR).
		GpuResources::CreateImage(dev, m_CDFHeight, 1, VK_FORMAT_R32_SFLOAT,
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

	// 5. Record CDF texture copies into the same command buffer
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
			                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
			                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
			                     VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
			                     0, 0, nullptr, 0, nullptr, 1, &shaderBarrier);
		}
	}

	vkEndCommandBuffer(cmd);

	// Store the command buffer for main-thread submission. Vulkan queues
	// are not thread-safe, so the worker must NOT call vkQueueSubmit.
	m_CmdBuffer = cmd;

	// Create fence now (main thread will use it when submitting)
	VkFenceCreateInfo fenceInfo = {};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	if (vkCreateFence(device, &fenceInfo, nullptr, &m_UploadFence) != VK_SUCCESS)
	{
		RT_LOG("[AsyncTex] Failed to create fence");
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
