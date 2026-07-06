#include "RendererGPU.h"
#include "ShaderManager.h"
#include "RTLog.h"
#include "VulkanUtils.h"
#include "shader_interface.h"
#include "GpuResources.h"
#include "CommandUtils.h"
#include "StagingArena.h"
#include "Walnut/RTDispatch.h"
#include "backends/imgui_impl_vulkan.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <iostream>
#include <cstring>

bool RendererGPU::Init()
{
	if (m_Initialized) return true;

	m_Device.InitFromWalnut();

	if (!m_Device.rayTracingSupported)
	{
		RT_LOG("[RT2] Ray tracing not supported, GPU renderer unavailable.");
		return false;
	}

	m_AS.SetDevice(m_Device);
	ShaderManager::Init(m_Device.device);

	// Create G-buffer descriptor set layout (set 1) first — needed by PathTracePass
	CreateGBufferDescriptorSet();

	if (!m_PathTracePass.Init(m_Device, m_GBufferSetLayout))
	{
		RT_LOG("[RT2] GPU renderer initialization failed (pipeline creation error)");
		return false;
	}
	m_ComposePass.Init(m_Device);

	if (!m_RasterPass.Init(m_Device, m_PathTracePass.GetDescriptorSetLayout(), m_GBufferSetLayout))
	{
		RT_LOG("[RT2] RasterPass init failed (non-fatal, RT primary visibility will be used)");
	}

	m_GBufferDebugPass.Init(m_Device, m_PathTracePass.GetDescriptorSetLayout(), m_GBufferSetLayout);

	// Create shared texture samplers
	m_TextureSampler = GpuResources::CreateSampler(m_Device,
		VK_FILTER_LINEAR, VK_FILTER_LINEAR,
		VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_MIPMAP_MODE_LINEAR);
	m_CDFTextureSampler = GpuResources::CreateSampler(m_Device,
		VK_FILTER_LINEAR, VK_FILTER_LINEAR,
		VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_MIPMAP_MODE_NEAREST);

	// Create frames-in-flight ring
	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
		m_Frames[i].Init(m_Device.device, m_Device.queueFamily);

	m_Initialized = true;
	return true;
}

void RendererGPU::Destroy()
{
	VkDevice device = m_Device.device;
	vkDeviceWaitIdle(device);

	m_NRD.Destroy();
	m_PathTracePass.Destroy();
	m_ComposePass.Destroy();
	m_RasterPass.Destroy();
	m_GBufferDebugPass.Destroy();
	DestroyOutputImage();
	DestroyGBufferImages();
	DestroyTextures();
	DestroyEnvMapCDFTextures();
	GpuResources::DestroySampler(m_Device, m_TextureSampler);
	GpuResources::DestroySampler(m_Device, m_CDFTextureSampler);
	GpuResources::DestroyBuffer(m_Device, m_CameraUBO, m_CameraUBOMemory);
	GpuResources::DestroyBuffer(m_Device, m_MaterialBuffer, m_MaterialBufferMemory);
	GpuResources::DestroyBuffer(m_Device, m_LightBuffer, m_LightBufferMemory);
	GpuResources::DestroyBuffer(m_Device, m_InstanceTransformBuffer, m_InstanceTransformBufferMemory);
	GpuResources::DestroyBuffer(m_Device, m_InstanceTransformPrevBuffer, m_InstanceTransformPrevBufferMemory);
	GpuResources::DestroyBuffer(m_Device, m_InstanceMaterialIndexBuffer, m_InstanceMaterialIndexBufferMemory);
	GpuResources::DestroyBuffer(m_Device, m_NRDUBO, m_NRDUBOMemory);

	if (m_GBufferPool)
	{
		vkDestroyDescriptorPool(device, m_GBufferPool, nullptr);
		m_GBufferPool = VK_NULL_HANDLE;
	}
	if (m_GBufferSetLayout)
	{
		vkDestroyDescriptorSetLayout(device, m_GBufferSetLayout, nullptr);
		m_GBufferSetLayout = VK_NULL_HANDLE;
	}

	// Destroy frames-in-flight ring
	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
		m_Frames[i].Destroy(device);

	m_Initialized = false;
}

void RendererGPU::CreateOutputImage()
{
	VkDevice device = m_Device.device;

	// Create output image via GpuResources (image + memory + view)
	GpuImage outputImg;
	GpuResources::CreateImage(m_Device, m_Width, m_Height,
		VK_FORMAT_R32G32B32A32_SFLOAT,
		VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, outputImg);
	m_OutputImage    = outputImg.image;
	m_OutputMemory   = outputImg.memory;
	m_OutputImageView = outputImg.view;

	// Create output sampler (clamp + extended lod range for ImGui display)
	m_Sampler = GpuResources::CreateSampler(m_Device,
		VK_FILTER_LINEAR, VK_FILTER_LINEAR,
		VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_MIPMAP_MODE_LINEAR);

	// Transition image to general layout for compute writes
	CommandUtils::ImmediateSubmit(m_Device, [&](VkCommandBuffer cmd) {
		VkImageMemoryBarrier barrier = {};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = m_OutputImage;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.layerCount = 1;

		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	});

	m_OutputImageLayout = VK_IMAGE_LAYOUT_GENERAL;

	// Create ImGui descriptor set for display
	m_ImGuiDescriptorSet = (VkDescriptorSet)ImGui_ImplVulkan_AddTexture(m_Sampler, m_OutputImageView, VK_IMAGE_LAYOUT_GENERAL);
}

void RendererGPU::DestroyOutputImage()
{
	VkDevice device = m_Device.device;

	// Free the ImGui descriptor set before destroying the image view/sampler
	// it references — otherwise the GPU may sample a destroyed resource.
	if (m_ImGuiDescriptorSet)
	{
		vkFreeDescriptorSets(device, m_Device.descriptorPool,
		                     1, &m_ImGuiDescriptorSet);
		m_ImGuiDescriptorSet = VK_NULL_HANDLE;
	}

	if (m_Sampler) vkDestroySampler(device, m_Sampler, nullptr);
	if (m_OutputImageView) vkDestroyImageView(device, m_OutputImageView, nullptr);
	if (m_OutputImage) vkDestroyImage(device, m_OutputImage, nullptr);
	if (m_OutputMemory) vkFreeMemory(device, m_OutputMemory, nullptr);
	m_Sampler = VK_NULL_HANDLE;
	m_OutputImageView = VK_NULL_HANDLE;
	m_OutputImage = VK_NULL_HANDLE;
	m_OutputMemory = VK_NULL_HANDLE;
	m_OutputImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}

void RendererGPU::OnResize(uint32_t width, uint32_t height)
{
	if (width == 0 || height == 0)
		return;

	if (m_Width == width && m_Height == height && m_OutputImage != VK_NULL_HANDLE)
		return;

	VkDevice device = m_Device.device;
	vkDeviceWaitIdle(device);

	// Free old descriptor set before allocating a new one
	m_PathTracePass.FreeDescriptorSet(device, m_Device.descriptorPool);

	DestroyOutputImage();

	m_Width = width;
	m_Height = height;

	CreateOutputImage();
	CreateGBufferImages();
	m_PathTracePass.CreateDescriptorSet(m_Device, m_Device.descriptorPool);
	UpdateGBufferDescriptorSet();

	// Initialize NRD if enabled
	if (m_NRDEnabled && !m_NRD.IsAvailable())
	{
		m_NRD.Init(m_Device.instance,
		           m_Device.physicalDevice,
		           m_Device.device,
		           m_Device.queue,
		           m_Device.queueFamily,
		           m_Width, m_Height);
	}
	else if (m_NRDEnabled && m_NRD.IsAvailable())
	{
		m_NRD.OnResize(m_Width, m_Height);
	}
	UpdatePathTraceDescriptorSet();

	m_FrameIndex = 1;
}

void RendererGPU::UpdatePathTraceDescriptorSet()
{
	if (!m_PathTracePass.IsAvailable()) return;
	if (!m_AS.IsValid()) { RT_LOG("[UpdateDS] skip: AS not valid"); return; }

	// Create camera UBO if needed
	if (!m_CameraUBO)
	{
		GpuResources::CreateBuffer(m_Device, sizeof(SICameraData),
		             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		             m_CameraUBO, m_CameraUBOMemory);
	}
	if (!m_MaterialBuffer) { RT_LOG("[UpdateDS] skip: no material buffer"); return; }

	// Build texture image infos — use shared texture sampler for all textures
	std::vector<VkDescriptorImageInfo> textureImageInfos;
	for (const auto& gt : m_Textures)
	{
		if (!gt.view) continue;
		VkDescriptorImageInfo imgInfo = {};
		imgInfo.sampler = m_TextureSampler;
		imgInfo.imageView = gt.view;
		imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		textureImageInfos.push_back(imgInfo);
	}

	m_PathTracePass.UpdateDescriptorSet(m_Device,
		m_OutputImageView, m_Sampler,
		m_CameraUBO, m_MaterialBuffer,
		m_AS.GetNormalBuffer(), m_AS.GetInstanceOffsetBuffer(),
		m_AS.GetTangentBuffer(), m_AS.GetUVBuffer(), m_AS.GetPositionBuffer(),
		m_LightBuffer, m_InstanceTransformBuffer,
		m_InstanceTransformPrevBuffer, m_InstanceMaterialIndexBuffer,
		m_AS.GetTLAS(),
		textureImageInfos);

	RT_LOG("[UpdateDS] done (textures=%d, descSet=%p, TLAS=%p, outView=%p, camUBO=%p, matBuf=%p)",
	       (int)textureImageInfos.size(), (void*)m_PathTracePass.GetDescriptorSet(),
	       (void*)m_AS.GetTLAS(), (void*)m_OutputImageView,
	       (void*)m_CameraUBO, (void*)m_MaterialBuffer);
}

void RendererGPU::CreateMaterialBuffer()
{
	GpuResources::DestroyBuffer(m_Device, m_MaterialBuffer, m_MaterialBufferMemory);

	size_t matCount = m_CurrentScene.materials.size();
	if (matCount == 0) matCount = 1; // always at least 1

	m_MaterialBufferSize = matCount * sizeof(GPUMaterial);

	GpuResources::CreateBuffer(m_Device, m_MaterialBufferSize,
	             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
	             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	             m_MaterialBuffer, m_MaterialBufferMemory);

	VkDevice device = m_Device.device;
	void* data;
	vkMapMemory(device, m_MaterialBufferMemory, 0, m_MaterialBufferSize, 0, &data);
	memcpy(data, m_CurrentScene.materials.data(), m_MaterialBufferSize);
	vkUnmapMemory(device, m_MaterialBufferMemory);
}

void RendererGPU::CreateLightBuffer()
{
	GpuResources::DestroyBuffer(m_Device, m_LightBuffer, m_LightBufferMemory);

	// Layout: 16-byte header + N * sizeof(GPUTriangleLight)
	// Header: uint lightCount, float totalLightArea, uint pad, uint pad
	size_t lightCount = m_CurrentScene.lights.size();
	m_LightBufferSize = 16 + lightCount * sizeof(GPUTriangleLight);

	// Always at least the header so the buffer is non-empty
	if (m_LightBufferSize < 16) m_LightBufferSize = 16;

	GpuResources::CreateBuffer(m_Device, m_LightBufferSize,
	             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
	             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	             m_LightBuffer, m_LightBufferMemory);

	// Build the buffer contents: header + lights
	std::vector<uint8_t> bufData(m_LightBufferSize, 0);
	uint32_t count = static_cast<uint32_t>(lightCount);
	float totalArea = m_CurrentScene.totalLightArea;
	memcpy(bufData.data() + 0,  &count,     sizeof(uint32_t));
	memcpy(bufData.data() + 4,  &totalArea, sizeof(float));
	// bytes 8-15: padding (already zeroed)
	if (lightCount > 0)
	{
		memcpy(bufData.data() + 16, m_CurrentScene.lights.data(),
		       lightCount * sizeof(GPUTriangleLight));
	}

	VkDevice device = m_Device.device;
	void* data;
	vkMapMemory(device, m_LightBufferMemory, 0, m_LightBufferSize, 0, &data);
	memcpy(data, bufData.data(), m_LightBufferSize);
	vkUnmapMemory(device, m_LightBufferMemory);

	RT_LOG("[RT2] Light buffer: %d lights, totalArea=%f", lightCount, totalArea);
}

void RendererGPU::CreateInstanceTransformBuffer()
{
	GpuResources::DestroyBuffer(m_Device, m_InstanceTransformBuffer, m_InstanceTransformBufferMemory);

	// Previous transform buffer: swap current → prev before creating new
	GpuResources::DestroyBuffer(m_Device, m_InstanceTransformPrevBuffer, m_InstanceTransformPrevBufferMemory);
	m_InstanceTransformPrevBuffer = m_InstanceTransformBuffer;
	m_InstanceTransformPrevBufferMemory = m_InstanceTransformBufferMemory;
	m_InstanceTransformBuffer = VK_NULL_HANDLE;
	m_InstanceTransformBufferMemory = VK_NULL_HANDLE;

	size_t instanceCount = m_CurrentScene.instances.size();
	VkDeviceSize bufferSize = instanceCount * sizeof(glm::mat4);
	if (bufferSize == 0) bufferSize = sizeof(glm::mat4);

	GpuResources::CreateBuffer(m_Device, bufferSize,
	             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
	             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	             m_InstanceTransformBuffer, m_InstanceTransformBufferMemory);

	std::vector<glm::mat4> transforms(instanceCount);
	for (size_t i = 0; i < instanceCount; i++)
		transforms[i] = m_CurrentScene.instances[i].worldMatrix;

	VkDevice device = m_Device.device;
	void* data;
	vkMapMemory(device, m_InstanceTransformBufferMemory, 0, bufferSize, 0, &data);
	memcpy(data, transforms.data(), instanceCount * sizeof(glm::mat4));
	vkUnmapMemory(device, m_InstanceTransformBufferMemory);

	// If prev buffer is empty (first creation), fill it with current transforms
	if (m_InstanceTransformPrevBuffer == VK_NULL_HANDLE)
	{
		GpuResources::CreateBuffer(m_Device, bufferSize,
		             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		             m_InstanceTransformPrevBuffer, m_InstanceTransformPrevBufferMemory);
		vkMapMemory(device, m_InstanceTransformPrevBufferMemory, 0, bufferSize, 0, &data);
		memcpy(data, transforms.data(), instanceCount * sizeof(glm::mat4));
		vkUnmapMemory(device, m_InstanceTransformPrevBufferMemory);
	}

	// Create per-instance material index buffer
	GpuResources::DestroyBuffer(m_Device, m_InstanceMaterialIndexBuffer, m_InstanceMaterialIndexBufferMemory);
	VkDeviceSize matIdxSize = instanceCount * sizeof(uint32_t);
	if (matIdxSize == 0) matIdxSize = sizeof(uint32_t);
	GpuResources::CreateBuffer(m_Device, matIdxSize,
	             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
	             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	             m_InstanceMaterialIndexBuffer, m_InstanceMaterialIndexBufferMemory);

	std::vector<uint32_t> matIndices(instanceCount);
	for (size_t i = 0; i < instanceCount; i++)
		matIndices[i] = m_CurrentScene.instances[i].materialIndex;

	vkMapMemory(device, m_InstanceMaterialIndexBufferMemory, 0, matIdxSize, 0, &data);
	memcpy(data, matIndices.data(), instanceCount * sizeof(uint32_t));
	vkUnmapMemory(device, m_InstanceMaterialIndexBufferMemory);

	RT_LOG("[RT2] Instance transform buffer: %d instances (%zu bytes)",
	       (int)instanceCount, (size_t)bufferSize);
}

void RendererGPU::CreateTextures(const std::vector<SceneTexture>& textures)
{
	VkDevice device = m_Device.device;
	DestroyTextures();

	m_Textures.resize(textures.size());

	// First pass: compute total staging size needed, create images
	VkDeviceSize totalStagingSize = 0;
	for (size_t i = 0; i < textures.size(); i++)
	{
		const auto& tex = textures[i];
		if (tex.floatPixels.empty() && tex.pixels.empty())
			continue;

		bool isHDR = tex.isHDR && !tex.floatPixels.empty();
		VkDeviceSize imageSize = isHDR
			? (VkDeviceSize)(tex.width * tex.height * 8)
			: (VkDeviceSize)(tex.width * tex.height * 4);

		totalStagingSize += imageSize;
	}

	if (totalStagingSize == 0)
	{
		RT_LOG("[RT2] No textures to create");
		return;
	}

	// Single staging arena for all texture uploads
	StagingArena arena;
	if (!arena.Init(m_Device, totalStagingSize))
	{
		RT_LOG("[RT2] Failed to create staging arena for textures");
		return;
	}

	// Second pass: create images, copy pixel data into arena, record offsets
	struct TextureUpload {
		VkDeviceSize offset;
		VkDeviceSize size;
		bool isHDR;
	};
	std::vector<TextureUpload> uploads(textures.size());

	for (size_t i = 0; i < textures.size(); i++)
	{
		const auto& tex = textures[i];
		if (tex.floatPixels.empty() && tex.pixels.empty())
		{
			RT_LOG("[RT2] Texture %d: no pixel data, skipping", (int)i);
			continue;
		}

		GpuImage& gt = m_Textures[i];
		gt.width = tex.width;
		gt.height = tex.height;

		bool isHDR = tex.isHDR && !tex.floatPixels.empty();
		VkFormat format = isHDR ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
		gt.format = format;
		VkDeviceSize imageSize = isHDR
			? (VkDeviceSize)(tex.width * tex.height * 8)
			: (VkDeviceSize)(tex.width * tex.height * 4);

		// Allocate from arena (16-byte alignment for safety)
		VkDeviceSize offset = arena.Alloc(imageSize, 16);
		if (offset == VK_WHOLE_SIZE)
		{
			RT_LOG("[RT2] Texture %d: staging arena out of space", (int)i);
			continue;
		}

		// Copy pixel data into mapped arena
		if (isHDR)
		{
			// Convert float pixels to half-float
			uint16_t* dst = static_cast<uint16_t*>(arena.GetMappedPointer(offset));
			for (size_t p = 0; p < tex.floatPixels.size(); p++)
				dst[p] = glm::packHalf1x16(tex.floatPixels[p]);
		}
		else
		{
			memcpy(arena.GetMappedPointer(offset), tex.pixels.data(), (size_t)imageSize);
		}

		uploads[i] = { offset, imageSize, isHDR };

		// Create VkImage via GpuResources
		GpuResources::CreateImage(m_Device, tex.width, tex.height, format,
			VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, gt);
	}

	// Batch all texture copies + transitions into a single command buffer
	VkBuffer stagingBuffer = arena.GetBuffer();
	CommandUtils::ImmediateSubmit(m_Device, [&](VkCommandBuffer cmd) {
		for (size_t i = 0; i < textures.size(); i++)
		{
			const auto& tex = textures[i];
			if (tex.floatPixels.empty() && tex.pixels.empty()) continue;

			GpuImage& gt = m_Textures[i];
			const auto& up = uploads[i];

			// Transition UNDEFINED -> TRANSFER_DST_OPTIMAL
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
			barrier.subresourceRange.levelCount = 1;
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

			vkCmdCopyBufferToImage(cmd, stagingBuffer, gt.image,
			                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

			// Transition TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL
			VkImageMemoryBarrier shaderBarrier = barrier;
			shaderBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			shaderBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			shaderBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			shaderBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
			                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &shaderBarrier);
		}
	});

	// Arena destroyed automatically when it goes out of scope

	RT_LOG("[RT2] Created %d GPU textures (staging arena: %zu bytes, %zu used)",
	       (int)m_Textures.size(), (size_t)arena.GetCapacity(), (size_t)arena.GetUsed());
}

void RendererGPU::DestroyTextures()
{
	for (auto& gt : m_Textures)
		GpuResources::DestroyImage(m_Device, gt);
	m_Textures.clear();
}

void RendererGPU::CreateEnvMapCDFTextures(const GPUSceneData& sceneData)
{
	m_EnvMapIndex = sceneData.envMapIndex;
	m_CDFWidth = sceneData.cdfWidth;
	m_CDFHeight = sceneData.cdfHeight;
	m_MarginalCDFIndex = -1;
	m_ConditionalCDFIndex = -1;

	if (sceneData.envMapIndex < 0 || sceneData.marginalCDF.empty() || sceneData.conditionalCDF.empty())
		return;

	// Compute total staging size: marginal (cdfHeight * 1 * 4) + conditional (cdfWidth * cdfHeight * 4)
	VkDeviceSize marginalSize = (VkDeviceSize)(sceneData.cdfHeight * 1 * 4);
	VkDeviceSize conditionalSize = (VkDeviceSize)(sceneData.cdfWidth * sceneData.cdfHeight * 4);

	StagingArena arena;
	if (!arena.Init(m_Device, marginalSize + conditionalSize))
	{
		RT_LOG("[RT2] Failed to create staging arena for CDF textures");
		return;
	}

	struct CDFEntry {
		GpuImage img;
		VkDeviceSize offset;
		int w, h;
	};

	auto prepareCDFTexture = [&](const std::vector<float>& cdfData, int w, int h) -> CDFEntry {
		CDFEntry e;
		e.img.width = w;
		e.img.height = h;
		e.img.format = VK_FORMAT_R32_SFLOAT;
		e.w = w;
		e.h = h;
		VkDeviceSize imageSize = (VkDeviceSize)(w * h * 4);

		e.offset = arena.Alloc(imageSize, 16);
		memcpy(arena.GetMappedPointer(e.offset), cdfData.data(), (size_t)imageSize);

		GpuResources::CreateImage1D(m_Device, w, h, VK_FORMAT_R32_SFLOAT,
			VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, e.img);
		return e;
	};

	CDFEntry entries[2];
	entries[0] = prepareCDFTexture(sceneData.marginalCDF, sceneData.cdfHeight, 1);
	entries[1] = prepareCDFTexture(sceneData.conditionalCDF, sceneData.cdfWidth, sceneData.cdfHeight);

	// Batch both CDF texture copies + transitions into a single command buffer
	VkBuffer stagingBuffer = arena.GetBuffer();
	CommandUtils::ImmediateSubmit(m_Device, [&](VkCommandBuffer cmd) {
		for (int i = 0; i < 2; i++)
		{
			const auto& e = entries[i];
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
			vkCmdCopyBufferToImage(cmd, stagingBuffer, e.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

			VkImageMemoryBarrier shaderBarrier = barrier;
			shaderBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			shaderBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			shaderBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			shaderBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
			                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &shaderBarrier);
		}
	});

	// Arena destroyed automatically when it goes out of scope

	// Append to m_Textures
	for (int i = 0; i < 2; i++)
	{
		int idx = (int)m_Textures.size();
		m_Textures.push_back(entries[i].img);
		if (i == 0) m_MarginalCDFIndex = idx;
		else        m_ConditionalCDFIndex = idx;
	}

	RT_LOG("[RT2] Env map CDF textures: marginal idx=%d conditional idx=%d (%dx%d)",
	       m_MarginalCDFIndex, m_ConditionalCDFIndex, sceneData.cdfWidth, sceneData.cdfHeight);
}

void RendererGPU::DestroyEnvMapCDFTextures()
{
	// CDF textures are in m_Textures, destroyed by DestroyTextures()
	m_EnvMapIndex = -1;
	m_MarginalCDFIndex = -1;
	m_ConditionalCDFIndex = -1;
}

void RendererGPU::ResetAccumulation()
{
	m_FrameIndex = 1;
	m_HasPrevMatrices = false;
}

void RendererGPU::SetScene(const GPUSceneData& sceneData)
{
	VkDevice device = m_Device.device;
	RT_LOG("[SetScene] enter: textures=%d, envMapIndex=%d, meshes=%d, materials=%d",
	       (int)sceneData.textures.size(), sceneData.envMapIndex,
	       (int)sceneData.meshes.size(), (int)sceneData.materials.size());
	vkDeviceWaitIdle(device);  // Wait for GPU to finish before destroying textures

	m_CurrentScene = sceneData;
	m_NeedsASRebuild = true;
	m_FrameIndex = 1;
	RT_LOG("[SetScene] destroying old textures (count=%d)", (int)m_Textures.size());
	DestroyTextures();
	RT_LOG("[SetScene] creating new textures");
	CreateTextures(m_CurrentScene.textures);
	RT_LOG("[SetScene] destroying old CDF textures");
	DestroyEnvMapCDFTextures();
	RT_LOG("[SetScene] creating new CDF textures");
	CreateEnvMapCDFTextures(m_CurrentScene);
	RT_LOG("[SetScene] CDF indices: marginal=%d conditional=%d envMap=%d",
	       m_MarginalCDFIndex, m_ConditionalCDFIndex, m_EnvMapIndex);

	// Update descriptor set immediately so the new texture views are bound
	// before the next render. But skip if AS needs rebuild — the old TLAS
	// handle will be destroyed during rebuild, and UpdateDescriptorSet will
	// be called again after the rebuild with the new TLAS handle.
	if (m_AS.IsValid() && !m_NeedsASRebuild)
	{
		RT_LOG("[SetScene] updating descriptor set (AS valid, no rebuild needed)");
		UpdatePathTraceDescriptorSet();
	}
	else
	{
		RT_LOG("[SetScene] skipping descriptor update (AS needs rebuild or not valid)");
	}
	RT_LOG("[SetScene] done");
}

void RendererGPU::RebuildAccelerationStructures()
{
	RT_LOG("[RebuildAS] enter: meshes=%d instances=%d",
	       (int)m_CurrentScene.meshes.size(), (int)m_CurrentScene.instances.size());

	// Build one BLAS per unique mesh geometry (object space)
	std::vector<BLASGeometry> geometries;
	geometries.reserve(m_CurrentScene.meshes.size());
	for (const auto& mesh : m_CurrentScene.meshes)
	{
		BLASGeometry geo;
		geo.vertices = &mesh.vertices;
		geo.indices = &mesh.indices;
		geo.vertexUVs = &mesh.vertexUVs;
		geo.tangents = &mesh.tangents;
		geo.materialIndex = mesh.materialIndex;

		// Check transparency from the mesh's material
		uint32_t matIdx = mesh.materialIndex;
		if (matIdx < m_CurrentScene.materials.size())
		{
			float alphaMode = m_CurrentScene.materials[matIdx].alphaMode;
			geo.isTransparent = (alphaMode > 0.5f);
		}

		geometries.push_back(geo);
	}
	RT_LOG("[RebuildAS] built %d geometry entries, calling BuildBLASes", (int)geometries.size());

	CommandUtils::ImmediateSubmit(m_Device, [&](VkCommandBuffer cmd) {
		bool blasOK = m_AS.BuildBLASes(cmd, geometries);
		RT_LOG("[RebuildAS] BuildBLASes result=%d", blasOK);

		// Barrier: BLAS builds must complete before TLAS build reads them
		VkMemoryBarrier blasBarrier = {};
		blasBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		blasBarrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
		blasBarrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;

		vkCmdPipelineBarrier(cmd,
			VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
			VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
			0, 1, &blasBarrier, 0, nullptr, 0, nullptr);

		// Build TLAS instances from GPUInstance list
		std::vector<BLASInstance> instances;
		std::vector<uint32_t> instanceMeshIndices;
		instances.reserve(m_CurrentScene.instances.size());
		instanceMeshIndices.reserve(m_CurrentScene.instances.size());
		for (const auto& gpuInst : m_CurrentScene.instances)
		{
			BLASInstance inst = {};
			inst.blasAddress = m_AS.GetBLASAddress(gpuInst.meshIndex);
			inst.customIndex = gpuInst.materialIndex;
			inst.sbtHitOffset = gpuInst.isTransparent ? 1u : 0u;

			// Convert glm::mat4 to VkTransformMatrixKHR (3x4 row-major)
			const glm::mat4& w = gpuInst.worldMatrix;
			VkTransformMatrixKHR& t = inst.transform;
			t.matrix[0][0] = w[0][0]; t.matrix[0][1] = w[1][0]; t.matrix[0][2] = w[2][0]; t.matrix[0][3] = w[3][0];
			t.matrix[1][0] = w[0][1]; t.matrix[1][1] = w[1][1]; t.matrix[1][2] = w[2][1]; t.matrix[1][3] = w[3][1];
			t.matrix[2][0] = w[0][2]; t.matrix[2][1] = w[1][2]; t.matrix[2][2] = w[2][2]; t.matrix[2][3] = w[3][2];

			instances.push_back(inst);
			instanceMeshIndices.push_back(gpuInst.meshIndex);
		}

		bool tlasOK = m_AS.BuildTLAS(cmd, instances, instanceMeshIndices);
		RT_LOG("[RebuildAS] TLAS build result=%d instances=%d", tlasOK, (int)instances.size());
	});

	// Build combined buffers after TLAS (needs instance-to-BLAS mapping)
	// Object-space data — shader transforms to world space via instanceTransforms SSBO
	m_AS.BuildCombinedBuffers();

	// Build raster vertex buffers + draw data
	m_RasterPass.CreateVertexBuffers(m_Device, m_CurrentScene);
	m_RasterPass.CreateDrawData(m_Device, m_CurrentScene);

	RT_LOG("[RebuildAS] creating material buffer");
	CreateMaterialBuffer();
	RT_LOG("[RebuildAS] creating light buffer");
	CreateLightBuffer();
	RT_LOG("[RebuildAS] creating instance transform buffer");
	CreateInstanceTransformBuffer();
	RT_LOG("[RebuildAS] updating descriptor set");
	UpdatePathTraceDescriptorSet();

	m_NeedsASRebuild = false;
	m_ASJustBuilt = true;
}

void RendererGPU::UpdateSceneInstances(const GPUSceneData& sceneData)
{
	if (!m_AS.IsValid() || m_AS.GetBLASCount() == 0)
	{
		RT_LOG("[UpdateInstances] skip: AS not valid or no BLASes");
		return;
	}

	// Update instance + light data from the new scene data
	m_CurrentScene.instances = sceneData.instances;
	m_CurrentScene.lights = sceneData.lights;
	m_CurrentScene.totalLightArea = sceneData.totalLightArea;

	// Build TLAS instances from the updated GPUInstance list
	std::vector<BLASInstance> instances;
	std::vector<uint32_t> instanceMeshIndices;
	instances.reserve(m_CurrentScene.instances.size());
	instanceMeshIndices.reserve(m_CurrentScene.instances.size());
	for (const auto& gpuInst : m_CurrentScene.instances)
	{
		BLASInstance inst = {};
		inst.blasAddress = m_AS.GetBLASAddress(gpuInst.meshIndex);
		inst.customIndex = gpuInst.materialIndex;
		inst.sbtHitOffset = gpuInst.isTransparent ? 1u : 0u;

		const glm::mat4& w = gpuInst.worldMatrix;
		VkTransformMatrixKHR& t = inst.transform;
		t.matrix[0][0] = w[0][0]; t.matrix[0][1] = w[1][0]; t.matrix[0][2] = w[2][0]; t.matrix[0][3] = w[3][0];
		t.matrix[1][0] = w[0][1]; t.matrix[1][1] = w[1][1]; t.matrix[1][2] = w[2][1]; t.matrix[1][3] = w[3][1];
		t.matrix[2][0] = w[0][2]; t.matrix[2][1] = w[1][2]; t.matrix[2][2] = w[2][2]; t.matrix[2][3] = w[3][2];

		instances.push_back(inst);
		instanceMeshIndices.push_back(gpuInst.meshIndex);
	}

	// Rebuild TLAS only (BLASes unchanged)
	CommandUtils::ImmediateSubmit(m_Device, [&](VkCommandBuffer cmd) {
		m_AS.RebuildTLASOnly(cmd, instances, instanceMeshIndices);
	});

	// Update instance transform + light buffers
	CreateInstanceTransformBuffer();
	CreateLightBuffer();

	// Update descriptor set (TLAS handle changed)
	UpdatePathTraceDescriptorSet();

	RT_LOG("[UpdateInstances] done: instances=%d lights=%d",
	       (int)m_CurrentScene.instances.size(), (int)m_CurrentScene.lights.size());
}

void RendererGPU::UpdateCameraUBO(const Camera& camera)
{
	SICameraData ubo = {};
	ubo.position = glm::vec4(camera.GetPosition(), (float)m_FrameIndex);

	// NRD camera jitter (Halton sequence, subpixel offset in [-0.5, 0.5])
	m_NRDJitterPrev = m_NRDJitter;
	if (m_NRDEnabled)
	{
		// Halton sequence (base 2, base 3) for low-discrepancy jitter
		auto halton = [](int index, int base) -> float {
			float f = 1.0f, r = 0.0f;
			int i = index;
			while (i > 0) {
				f /= base;
				r += f * (i % base);
				i /= base;
			}
			return r;
		};
		int frame = (m_FrameIndex - 1) % 16 + 1; // cycle through 16 offsets
		m_NRDJitter = glm::vec2(halton(frame, 2) - 0.5f, halton(frame, 3) - 0.5f);
	}
	else
	{
		m_NRDJitter = glm::vec2(0.0f);
	}

	ubo.forward = glm::vec4(camera.GetDirection(), m_NRDJitter.x);
	glm::vec3 right = glm::cross(camera.GetDirection(), glm::vec3(0, 1, 0));
	glm::vec3 up = glm::cross(right, camera.GetDirection());
	ubo.right = glm::vec4(right, m_NRDJitter.y);
	ubo.up = glm::vec4(up, 0.0f);
	int maxBouncesClamped = m_MaxBounces;
	const int bounceLimit = (int)m_PathTracePass.GetMaxRecursionDepth() - 1;
	if (maxBouncesClamped > bounceLimit) maxBouncesClamped = bounceLimit;
	ubo.viewportSPP = glm::vec4((float)m_Width, (float)m_Height, (float)m_SPP, (float)maxBouncesClamped);
	ubo.apertureFocal = glm::vec4(camera.m_Aperture, camera.m_FocusDistance, m_ShowBackground ? 1.0f : 0.0f, m_EmissiveBoost);
	ubo.envMap = glm::vec4((float)m_EnvMapIndex, m_EnvIntensity, (float)m_MarginalCDFIndex, (float)m_ConditionalCDFIndex);
	ubo.inverseProjection = camera.GetInverseProjection();
	ubo.inverseView = camera.GetInverseView();
	ubo.viewToClip = camera.GetProjection();
	ubo.worldToView = camera.GetView();
	ubo.viewToClipPrev = m_HasPrevMatrices ? m_PrevViewToClip : ubo.viewToClip;
	ubo.worldToViewPrev = m_HasPrevMatrices ? m_PrevWorldToView : ubo.worldToView;

	// Update previous frame matrices for next frame
	m_PrevViewToClip = ubo.viewToClip;
	m_PrevWorldToView = ubo.worldToView;
	m_HasPrevMatrices = true;

	m_CameraUBOData = ubo; // stash for vkCmdUpdateBuffer in Render()
}

void RendererGPU::Render(const Camera& camera)
{
	if (!m_Initialized || m_OutputImage == VK_NULL_HANDLE) return;

	RT_LOG("[Render] frame=%d needsASRebuild=%d", m_FrameIndex, m_NeedsASRebuild);

	if (m_NeedsASRebuild)
	{
		RT_LOG("[Render] rebuilding AS...");
		RebuildAccelerationStructures();
		RT_LOG("[Render] AS rebuild done, AS valid=%d", m_AS.IsValid());
	}

	if (!m_AS.IsValid())
	{
		static bool warned = false;
		if (!warned) { RT_LOG("[RT2] Render: TLAS not valid (no mesh loaded?)"); warned = true; }
		return;
	}

	if (const_cast<Camera&>(camera).checkHasMoved())
		m_FrameIndex = 1;

	// Lazy-init NRD when toggled on
	if (m_NRDEnabled && !m_NRD.IsAvailable() && m_Width > 0 && m_Height > 0)
	{
		RT_LOG("[Render] initializing NRD (%ux%u)", m_Width, m_Height);
		m_NRD.Init(m_Device.instance,
		           m_Device.physicalDevice,
		           m_Device.device,
		           m_Device.queue,
		           m_Device.queueFamily,
		           m_Width, m_Height);
	}

	UpdateCameraUBO(camera);

	if (!m_PathTracePass.IsAvailable() || !m_PathTracePass.GetDescriptorSet() || !m_CameraUBO || !m_MaterialBuffer)
	{
		RT_LOG("[RT2] Render: missing resource (pipe=%d ubo=%p mat=%p)",
		       m_PathTracePass.IsAvailable(), (void*)m_CameraUBO, (void*)m_MaterialBuffer);
		return;
	}

	VkDevice device = m_Device.device;

	// NRD requires all previous GPU work to be complete before NewFrame().
	// With frames-in-flight, previous frames may still be in flight.
	// Temporarily use full device idle for NRD mode until NRD pipelining is investigated.
	if (m_NRDEnabled && m_NRD.IsAvailable())
		vkDeviceWaitIdle(device);

	// ---- Frames-in-flight ring: wait for this frame slot to be free ----
	FrameContext& frame = m_Frames[m_CurrentFrame];
	frame.WaitForFence(device);
	frame.Begin(device);
	VkCommandBuffer cmd = frame.commandBuffer;

	// ---- Top-of-frame barrier: protect ImGui's read of previous frame's output ----
	// ImGui's fragment shader reads the output image from the previous frame's submission.
	// With pipelining, that read may still be in flight when we start writing.
	{
		VkImageMemoryBarrier topBarrier = {};
		topBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		topBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
		topBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		topBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
		topBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		topBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		topBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		topBarrier.image = m_OutputImage;
		topBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		topBarrier.subresourceRange.levelCount = 1;
		topBarrier.subresourceRange.layerCount = 1;
		vkCmdPipelineBarrier(cmd,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0,
			0, nullptr, 0, nullptr, 1, &topBarrier);
	}

	// If the AS was just rebuilt this frame (via ImmediateSubmit), the GPU is
	// provably done. The barrier is technically redundant but harmless.
	if (m_ASJustBuilt)
	{
		VkMemoryBarrier asBarrier = {};
		asBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		asBarrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
		asBarrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
		vkCmdPipelineBarrier(cmd,
			VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
			VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
			0, 1, &asBarrier, 0, nullptr, 0, nullptr);
		m_ASJustBuilt = false;
	}

	// ---- UBO updates via vkCmdUpdateBuffer (avoids host-write/GPU-read race) ----
	// Barrier: protect previous frame's uniform read from our transfer write
	VkMemoryBarrier uboPreBarrier = {};
	uboPreBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	uboPreBarrier.srcAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
	uboPreBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(cmd,
		VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 1, &uboPreBarrier, 0, nullptr, 0, nullptr);

	// Update camera UBO via vkCmdUpdateBuffer (496 bytes, within 65536 limit)
	vkCmdUpdateBuffer(cmd, m_CameraUBO, 0, sizeof(SICameraData), &m_CameraUBOData);

	// Update NRD UBO via vkCmdUpdateBuffer (16 bytes)
	SINRDUniformData nrdData = { m_NRDEnabled ? 1u : 0u, 0, 0, 0 };
	if (m_NRDUBO)
		vkCmdUpdateBuffer(cmd, m_NRDUBO, 0, sizeof(SINRDUniformData), &nrdData);

	// Barrier: make transfer writes visible to ray tracing shader
	VkMemoryBarrier uboPostBarrier = {};
	uboPostBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	uboPostBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	uboPostBarrier.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
	vkCmdPipelineBarrier(cmd,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
		0, 1, &uboPostBarrier, 0, nullptr, 0, nullptr);

	// ---- Raster G-buffer pass (primary visibility) ----
	if (m_RasterPass.IsAvailable() && m_DepthImageView)
	{
		// Clear G-buffer images to zero so uncovered pixels don't retain stale data
		VkImage gbufferClearImgs[] = {
			m_GNormalRoughness, m_GViewZ, m_GMotion,
			m_GAlbedoF0, m_GDirectEmission,
			m_GPrimHit, m_GPrimGeoNormal, m_GPrimUV
		};
		VkClearColorValue clearVal = {};
		for (auto img : gbufferClearImgs)
		{
			VkImageSubresourceRange range = {};
			range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			range.levelCount = 1;
			range.layerCount = 1;
			vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_GENERAL, &clearVal, 1, &range);
		}

		// Barrier: clears must complete before FS writes via imageStore
		for (auto img : gbufferClearImgs)
		{
			VkImageMemoryBarrier b = {};
			b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
			b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
			b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
			b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.image = img;
			b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			b.subresourceRange.levelCount = 1;
			b.subresourceRange.layerCount = 1;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
			                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
			                     0, nullptr, 0, nullptr, 1, &b);
		}

		// Depth image: UNDEFINED → DEPTH_ATTACHMENT_OPTIMAL (cleared by loadOp)
		VkImageMemoryBarrier depthBarrier = {};
		depthBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		depthBarrier.srcAccessMask = 0;
		depthBarrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		depthBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		depthBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
		depthBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		depthBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		depthBarrier.image = m_DepthImage;
		depthBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		depthBarrier.subresourceRange.levelCount = 1;
		depthBarrier.subresourceRange.layerCount = 1;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		                     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0,
		                     0, nullptr, 0, nullptr, 1, &depthBarrier);

		m_RasterPass.Record(cmd, m_Width, m_Height,
		                    m_PathTracePass.GetDescriptorSet(), m_GBufferSet,
		                    m_DepthImageView);

		// Barrier: FS writes to G-buffer (storage images) must complete before RT/compute reads
		VkImage gbufferImgs[] = {
			m_GNormalRoughness, m_GViewZ, m_GMotion,
			m_GAlbedoF0, m_GDirectEmission,
			m_GPrimHit, m_GPrimGeoNormal, m_GPrimUV
		};
		for (auto img : gbufferImgs)
		{
			VkImageMemoryBarrier b = {};
			b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
			b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
			b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
			b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
			b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.image = img;
			b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			b.subresourceRange.levelCount = 1;
			b.subresourceRange.layerCount = 1;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			                     VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
			                     0, nullptr, 0, nullptr, 1, &b);
		}
	}

	// ---- G-buffer debug view (skips path tracing + NRD) ----
	if (m_GBufferDebugMode >= 0 && m_GBufferDebugPass.IsAvailable())
	{
		m_GBufferDebugPass.Record(cmd, m_Width, m_Height,
		                          m_PathTracePass.GetDescriptorSet(), m_GBufferSet,
		                          (uint32_t)m_GBufferDebugMode);
	}
	else
	{
		// Trace rays via PathTracePass
		m_PathTracePass.Record(cmd, m_Width, m_Height, m_GBufferSet);

	// NRD denoising pass
	if (m_NRDEnabled && m_NRD.IsAvailable())
	{
		// Barrier: RT writes to G-buffer images must complete before NRD reads them
		VkImage gbufferImgs[] = {
			m_GNormalRoughness, m_GViewZ, m_GMotion,
			m_GDiffRadiance, m_GSpecRadiance
		};
		for (auto img : gbufferImgs)
		{
			VkImageMemoryBarrier b = {};
			b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
			b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
			b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
			b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.image = img;
			b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			b.subresourceRange.levelCount = 1;
			b.subresourceRange.layerCount = 1;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
			                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
			                     0, nullptr, 0, nullptr, 1, &b);
		}

		m_NRD.NewFrame();

		// Set common settings
		const Camera& cam = camera;
		glm::mat4 viewToClip = cam.GetProjection();
		glm::mat4 worldToView = cam.GetView();
		glm::mat4 prevViewToClip = m_HasPrevMatrices ? m_PrevViewToClip : viewToClip;
		glm::mat4 prevWorldToView = m_HasPrevMatrices ? m_PrevWorldToView : worldToView;

		bool reset = (m_FrameIndex == 1);
		m_NRD.SetCommonSettings(
			glm::value_ptr(viewToClip),
			glm::value_ptr(prevViewToClip),
			glm::value_ptr(worldToView),
			glm::value_ptr(prevWorldToView),
			m_NRDJitter.x, m_NRDJitter.y,
			m_NRDJitterPrev.x, m_NRDJitterPrev.y,
			m_FrameIndex, reset, m_NRDSplitScreen);

		m_NRD.SetReblurSettings(m_NRDMaxBlurRadius, (uint32_t)m_NRDMaxAccumFrames,
		                        m_NRDAntiFirefly, m_NRDSplitScreen);

		m_NRD.Denoise(cmd,
			m_GNormalRoughness, VK_FORMAT_R8G8B8A8_UNORM,
			m_GViewZ, VK_FORMAT_R16_SFLOAT,
			m_GMotion, VK_FORMAT_R16G16_SFLOAT,
			m_GDiffRadiance, VK_FORMAT_R16G16B16A16_SFLOAT,
			m_GSpecRadiance, VK_FORMAT_R16G16B16A16_SFLOAT,
			m_NRDDiffOut, m_NRDSpecOut);

		// NRD with restoreInitialState=true + GENERAL layout restores images
		// to GENERAL after denoising, so no manual barriers needed.

		// Compose pass: remodulate NRD outputs by albedo/F0, write to beauty
		if (m_ComposePass.IsAvailable())
		{
			// Update descriptor set with current image views
			m_ComposePass.UpdateDescriptorSet(m_Device,
				m_OutputImageView, m_NRDDiffOutView, m_NRDSpecOutView,
				m_GAlbedoF0View, m_GDirectEmissionView);

			// Barrier: NRD outputs (compute writes) → compose (compute reads)
			VkImage composeImgs[] = { m_NRDDiffOut, m_NRDSpecOut, m_GAlbedoF0 };
			for (auto img : composeImgs)
			{
				VkImageMemoryBarrier b = {};
				b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
				b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
				b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
				b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
				b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
				b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				b.image = img;
				b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				b.subresourceRange.levelCount = 1;
				b.subresourceRange.layerCount = 1;
				vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
				                     0, nullptr, 0, nullptr, 1, &b);
			}

		m_ComposePass.Record(cmd, m_Width, m_Height);
	}
	}
	} // end else (not debug mode)

	VkImageMemoryBarrier rtReadBarrier = {};
	rtReadBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	rtReadBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	rtReadBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	rtReadBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	rtReadBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	rtReadBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	rtReadBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	rtReadBarrier.image = m_OutputImage;
	rtReadBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	rtReadBarrier.subresourceRange.levelCount = 1;
	rtReadBarrier.subresourceRange.layerCount = 1;

	// If NRD+compose ran, the output image was written by compute (compose).
	// If debug mode, written by compute (debug pass).
	// Otherwise it was written by the ray tracing shader.
	VkPipelineStageFlags srcStage;
	if (m_GBufferDebugMode >= 0 && m_GBufferDebugPass.IsAvailable())
		srcStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
	else if (m_NRDEnabled && m_NRD.IsAvailable() && m_ComposePass.IsAvailable())
		srcStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
	else
		srcStage = VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;

	vkCmdPipelineBarrier(cmd, srcStage, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &rtReadBarrier);

	// ---- Submit this frame's work (async, no wait) ----
	frame.Submit(m_Device.queue);

	m_CurrentFrame = (m_CurrentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
	m_FrameIndex++;
}

// ---- Output readback for screenshots ---------------------------------------

bool RendererGPU::ReadbackOutput(std::vector<uint8_t>& outPixelsRGBA8, uint32_t& outWidth, uint32_t& outHeight)
{
	if (!m_Initialized || m_OutputImage == VK_NULL_HANDLE || m_Width == 0 || m_Height == 0)
		return false;

	VkDevice device = m_Device.device;

	// Create a host-visible staging buffer
	VkBuffer stagingBuffer;
	VkDeviceMemory stagingMemory;
	VkDeviceSize imageSize = (VkDeviceSize)m_Width * m_Height * 16; // R32G32B32A32_SFLOAT = 16 bytes/pixel

	GpuResources::CreateBuffer(m_Device, imageSize,
	             VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	             stagingBuffer, stagingMemory);

	// Wait for all in-flight frames, then use ImmediateSubmit for the copy
	vkDeviceWaitIdle(device);

	CommandUtils::ImmediateSubmit(m_Device, [&](VkCommandBuffer cmd) {
		VkImageMemoryBarrier toTransfer = {};
		toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		toTransfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		toTransfer.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
		toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toTransfer.image = m_OutputImage;
		toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		toTransfer.subresourceRange.levelCount = 1;
		toTransfer.subresourceRange.layerCount = 1;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
		                     0, nullptr, 0, nullptr, 1, &toTransfer);

		VkBufferImageCopy region = {};
		region.bufferOffset = 0;
		region.bufferRowLength = 0;
		region.bufferImageHeight = 0;
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.mipLevel = 0;
		region.imageSubresource.baseArrayLayer = 0;
		region.imageSubresource.layerCount = 1;
		region.imageOffset = { 0, 0, 0 };
		region.imageExtent = { m_Width, m_Height, 1 };
		vkCmdCopyImageToBuffer(cmd, m_OutputImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stagingBuffer, 1, &region);

		// Transition back to GENERAL
		VkImageMemoryBarrier toGeneral = toTransfer;
		toGeneral.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		toGeneral.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		toGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0,
		                     0, nullptr, 0, nullptr, 1, &toGeneral);
	});

	// Map and convert R32G32B32A32 float → RGBA8 (tonemap + sRGB)
	void* mapped = nullptr;
	VkResult err = vkMapMemory(device, stagingMemory, 0, imageSize, 0, &mapped);
	if (err != VK_SUCCESS || !mapped)
	{
		RT_LOG("[Readback] vkMapMemory failed: %d", (int)err);
		GpuResources::DestroyBuffer(m_Device, stagingBuffer, stagingMemory);
		return false;
	}

	const float* floatData = static_cast<const float*>(mapped);
	outPixelsRGBA8.resize((size_t)m_Width * m_Height * 4);
	for (size_t i = 0; i < (size_t)m_Width * m_Height; i++)
	{
		float r = floatData[i * 4 + 0];
		float g = floatData[i * 4 + 1];
		float b = floatData[i * 4 + 2];
		float a = floatData[i * 4 + 3];

		// Reinhard tonemap + clamp
		r = r / (1.0f + r);
		g = g / (1.0f + g);
		b = b / (1.0f + b);
		a = (a > 1.0f) ? 1.0f : (a < 0.0f ? 0.0f : a);

		// sRGB encode (gamma 2.2 approx)
		auto toSRGB = [](float c) -> uint8_t {
			if (c <= 0.0031308f) return (uint8_t)(c * 12.92f * 255.0f + 0.5f);
			return (uint8_t)((1.055f * powf(c, 1.0f / 2.4f) - 0.055f) * 255.0f + 0.5f);
		};

		outPixelsRGBA8[i * 4 + 0] = toSRGB(r);
		outPixelsRGBA8[i * 4 + 1] = toSRGB(g);
		outPixelsRGBA8[i * 4 + 2] = toSRGB(b);
		outPixelsRGBA8[i * 4 + 3] = (uint8_t)(a * 255.0f + 0.5f);
	}

	vkUnmapMemory(device, stagingMemory);
	GpuResources::DestroyBuffer(m_Device, stagingBuffer, stagingMemory);

	outWidth = m_Width;
	outHeight = m_Height;
	RT_LOG("[Readback] captured %ux%u → %zu bytes", m_Width, m_Height, outPixelsRGBA8.size());
	return true;
}

// ---- NRD G-buffer images (set 1) -------------------------------------------

void RendererGPU::CreateGBufferImages()
{
	DestroyGBufferImages();

	VkDevice device = m_Device.device;

	struct GBufferImageSpec
	{
		VkFormat format;
		VkImage& image;
		VkDeviceMemory& mem;
		VkImageView& view;
	};

	GBufferImageSpec specs[] = {
		{ VK_FORMAT_R8G8B8A8_UNORM,     m_GNormalRoughness, m_GNormalRoughnessMem, m_GNormalRoughnessView },
		{ VK_FORMAT_R16_SFLOAT,          m_GViewZ,           m_GViewZMem,           m_GViewZView },
		{ VK_FORMAT_R16G16_SFLOAT,       m_GMotion,          m_GMotionMem,          m_GMotionView },
		{ VK_FORMAT_R16G16B16A16_SFLOAT, m_GDiffRadiance,    m_GDiffRadianceMem,    m_GDiffRadianceView },
		{ VK_FORMAT_R16G16B16A16_SFLOAT, m_GSpecRadiance,    m_GSpecRadianceMem,    m_GSpecRadianceView },
		{ VK_FORMAT_R16G16B16A16_SFLOAT, m_GAlbedoF0,        m_GAlbedoF0Mem,        m_GAlbedoF0View },
		{ VK_FORMAT_R16G16B16A16_SFLOAT, m_GDirectEmission,  m_GDirectEmissionMem,  m_GDirectEmissionView },
		{ VK_FORMAT_R16G16B16A16_SFLOAT, m_NRDDiffOut,       m_NRDDiffOutMem,       m_NRDDiffOutView },
		{ VK_FORMAT_R16G16B16A16_SFLOAT, m_NRDSpecOut,       m_NRDSpecOutMem,       m_NRDSpecOutView },
		{ VK_FORMAT_R32G32B32A32_SFLOAT, m_GPrimHit,         m_GPrimHitMem,         m_GPrimHitView },
		{ VK_FORMAT_R8G8B8A8_UNORM,      m_GPrimGeoNormal,   m_GPrimGeoNormalMem,   m_GPrimGeoNormalView },
		{ VK_FORMAT_R16G16_SFLOAT,       m_GPrimUV,          m_GPrimUVMem,          m_GPrimUVView },
	};

	for (auto& s : specs)
	{
		VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

		GpuImage tmp;
		GpuResources::CreateImage(m_Device, m_Width, m_Height, s.format,
			usage,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, tmp);
		s.image = tmp.image;
		s.mem   = tmp.memory;
		s.view  = tmp.view;
	}

	// Transition all G-buffer images to GENERAL layout in a single command buffer
	CommandUtils::ImmediateSubmit(m_Device, [&](VkCommandBuffer cmd) {
		for (auto& s : specs)
		{
			VkImageMemoryBarrier barrier = {};
			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
			barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = s.image;
			barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			barrier.subresourceRange.levelCount = 1;
			barrier.subresourceRange.layerCount = 1;

			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		}
	});

	// Create depth image for raster pass
	{
		VkImageCreateInfo depthInfo = {};
		depthInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		depthInfo.imageType = VK_IMAGE_TYPE_2D;
		depthInfo.format = VK_FORMAT_D32_SFLOAT;
		depthInfo.extent = { m_Width, m_Height, 1 };
		depthInfo.mipLevels = 1;
		depthInfo.arrayLayers = 1;
		depthInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		depthInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		depthInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
		depthInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		vkCreateImage(device, &depthInfo, nullptr, &m_DepthImage);

		VkMemoryRequirements memReq;
		vkGetImageMemoryRequirements(device, m_DepthImage, &memReq);
		uint32_t memType = m_Device.FindMemoryType(memReq.memoryTypeBits,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VkMemoryAllocateInfo allocInfo = {};
		allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocInfo.allocationSize = memReq.size;
		allocInfo.memoryTypeIndex = memType;
		vkAllocateMemory(device, &allocInfo, nullptr, &m_DepthImageMem);
		vkBindImageMemory(device, m_DepthImage, m_DepthImageMem, 0);

		VkImageViewCreateInfo viewInfo = {};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = m_DepthImage;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = VK_FORMAT_D32_SFLOAT;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.layerCount = 1;
		vkCreateImageView(device, &viewInfo, nullptr, &m_DepthImageView);
	}
}

void RendererGPU::DestroyGBufferImages()
{
	VkDevice device = m_Device.device;

	struct ImgPair { VkImage& img; VkDeviceMemory& mem; VkImageView& view; };
	ImgPair pairs[] = {
		{ m_GNormalRoughness, m_GNormalRoughnessMem, m_GNormalRoughnessView },
		{ m_GViewZ,           m_GViewZMem,           m_GViewZView },
		{ m_GMotion,          m_GMotionMem,          m_GMotionView },
		{ m_GDiffRadiance,    m_GDiffRadianceMem,    m_GDiffRadianceView },
		{ m_GSpecRadiance,    m_GSpecRadianceMem,    m_GSpecRadianceView },
		{ m_GAlbedoF0,        m_GAlbedoF0Mem,        m_GAlbedoF0View },
		{ m_GDirectEmission,  m_GDirectEmissionMem,  m_GDirectEmissionView },
		{ m_NRDDiffOut,       m_NRDDiffOutMem,       m_NRDDiffOutView },
		{ m_NRDSpecOut,       m_NRDSpecOutMem,       m_NRDSpecOutView },
		{ m_GPrimHit,         m_GPrimHitMem,         m_GPrimHitView },
		{ m_GPrimGeoNormal,   m_GPrimGeoNormalMem,   m_GPrimGeoNormalView },
		{ m_GPrimUV,          m_GPrimUVMem,          m_GPrimUVView },
	};

	for (auto& p : pairs)
	{
		if (p.view) { vkDestroyImageView(device, p.view, nullptr); p.view = VK_NULL_HANDLE; }
		if (p.img)  { vkDestroyImage(device, p.img, nullptr);      p.img  = VK_NULL_HANDLE; }
		if (p.mem)  { vkFreeMemory(device, p.mem, nullptr);        p.mem  = VK_NULL_HANDLE; }
	}

	if (m_DepthImageView) { vkDestroyImageView(device, m_DepthImageView, nullptr); m_DepthImageView = VK_NULL_HANDLE; }
	if (m_DepthImage) { vkDestroyImage(device, m_DepthImage, nullptr); m_DepthImage = VK_NULL_HANDLE; }
	if (m_DepthImageMem) { vkFreeMemory(device, m_DepthImageMem, nullptr); m_DepthImageMem = VK_NULL_HANDLE; }
}

void RendererGPU::CreateGBufferDescriptorSet()
{
	VkDevice device = m_Device.device;

	// Set 1 layout: 10 storage images (0-5, 7-10) + 1 UBO (6)
	VkDescriptorSetLayoutBinding bindings[11] = {};
	// Bindings 0-5: storage images (gNormalRoughness, gViewZ, gMotion, gDiff, gSpec, gAlbedoF0)
	for (int i = 0; i < 6; i++)
	{
		bindings[i].binding = i;
		bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
		                         VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
	}
	// Binding 6: UBO (nrdData)
	bindings[SI_BINDING_NRD_UBO].binding = SI_BINDING_NRD_UBO;
	bindings[SI_BINDING_NRD_UBO].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	bindings[SI_BINDING_NRD_UBO].descriptorCount = 1;
	bindings[SI_BINDING_NRD_UBO].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
	                                          VK_SHADER_STAGE_COMPUTE_BIT;
	// Binding 7: gDirectEmission storage image
	bindings[SI_BINDING_G_DIRECT_EMISSION].binding = SI_BINDING_G_DIRECT_EMISSION;
	bindings[SI_BINDING_G_DIRECT_EMISSION].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	bindings[SI_BINDING_G_DIRECT_EMISSION].descriptorCount = 1;
	bindings[SI_BINDING_G_DIRECT_EMISSION].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
	                                                    VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
	// Bindings 8-10: new raster G-buffer images
	bindings[8].binding = SI_BINDING_G_PRIM_HIT;
	bindings[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	bindings[8].descriptorCount = 1;
	bindings[8].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

	bindings[9].binding = SI_BINDING_G_PRIM_GEO_NORMAL;
	bindings[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	bindings[9].descriptorCount = 1;
	bindings[9].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

	bindings[10].binding = SI_BINDING_G_PRIM_UV;
	bindings[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	bindings[10].descriptorCount = 1;
	bindings[10].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

	VkDescriptorSetLayoutCreateInfo layoutInfo = {};
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = 11;
	layoutInfo.pBindings = bindings;

	vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_GBufferSetLayout);

	// Create dedicated pool
	VkDescriptorPoolSize poolSizes[2] = {};
	poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	poolSizes[0].descriptorCount = 10;
	poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	poolSizes[1].descriptorCount = 1;

	VkDescriptorPoolCreateInfo poolInfo = {};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets = 1;
	poolInfo.poolSizeCount = 2;
	poolInfo.pPoolSizes = poolSizes;

	vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_GBufferPool);

	VkDescriptorSetAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.descriptorPool = m_GBufferPool;
	allocInfo.descriptorSetCount = 1;
	allocInfo.pSetLayouts = &m_GBufferSetLayout;

	vkAllocateDescriptorSets(device, &allocInfo, &m_GBufferSet);

	// Create NRD UBO
	if (!m_NRDUBO)
	{
		GpuResources::CreateBuffer(m_Device, sizeof(SINRDUniformData),
		             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		             m_NRDUBO, m_NRDUBOMemory);
	}
}

void RendererGPU::UpdateGBufferDescriptorSet()
{
	VkDevice device = m_Device.device;

	VkDescriptorImageInfo imageInfos[10] = {};
	VkImageView views[] = {
		m_GNormalRoughnessView, m_GViewZView, m_GMotionView,
		m_GDiffRadianceView, m_GSpecRadianceView, m_GAlbedoF0View,
		m_GDirectEmissionView,
		m_GPrimHitView, m_GPrimGeoNormalView, m_GPrimUVView
	};
	for (int i = 0; i < 10; i++)
	{
		imageInfos[i].imageView = views[i];
		imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	}

	VkDescriptorBufferInfo uboInfo = {};
	uboInfo.buffer = m_NRDUBO;
	uboInfo.range = VK_WHOLE_SIZE;

	VkWriteDescriptorSet writes[11] = {};
	// Storage images: bindings 0-5 and 7-10
	for (int i = 0; i < 6; i++)
	{
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = m_GBufferSet;
		writes[i].dstBinding = i;
		writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		writes[i].descriptorCount = 1;
		writes[i].pImageInfo = &imageInfos[i];
	}
	// UBO: binding 6
	writes[SI_BINDING_NRD_UBO].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[SI_BINDING_NRD_UBO].dstSet = m_GBufferSet;
	writes[SI_BINDING_NRD_UBO].dstBinding = SI_BINDING_NRD_UBO;
	writes[SI_BINDING_NRD_UBO].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	writes[SI_BINDING_NRD_UBO].descriptorCount = 1;
	writes[SI_BINDING_NRD_UBO].pBufferInfo = &uboInfo;
	// Direct emission: binding 7
	writes[SI_BINDING_G_DIRECT_EMISSION].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[SI_BINDING_G_DIRECT_EMISSION].dstSet = m_GBufferSet;
	writes[SI_BINDING_G_DIRECT_EMISSION].dstBinding = SI_BINDING_G_DIRECT_EMISSION;
	writes[SI_BINDING_G_DIRECT_EMISSION].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	writes[SI_BINDING_G_DIRECT_EMISSION].descriptorCount = 1;
	writes[SI_BINDING_G_DIRECT_EMISSION].pImageInfo = &imageInfos[6];
	// New G-buffer: bindings 8-10
	writes[8].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[8].dstSet = m_GBufferSet;
	writes[8].dstBinding = SI_BINDING_G_PRIM_HIT;
	writes[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	writes[8].descriptorCount = 1;
	writes[8].pImageInfo = &imageInfos[7];

	writes[9].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[9].dstSet = m_GBufferSet;
	writes[9].dstBinding = SI_BINDING_G_PRIM_GEO_NORMAL;
	writes[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	writes[9].descriptorCount = 1;
	writes[9].pImageInfo = &imageInfos[8];

	writes[10].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[10].dstSet = m_GBufferSet;
	writes[10].dstBinding = SI_BINDING_G_PRIM_UV;
	writes[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	writes[10].descriptorCount = 1;
	writes[10].pImageInfo = &imageInfos[9];

	vkUpdateDescriptorSets(device, 11, writes, 0, nullptr);
}