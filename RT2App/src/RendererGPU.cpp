#include "RendererGPU.h"
#include "ShaderManager.h"
#include "RTLog.h"
#include "VulkanUtils.h"
#include "shader_interface.h"
#include "GpuResources.h"
#include "Walnut/Application.h"
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
	DestroyOutputImage();
	DestroyGBufferImages();
	DestroyTextures();
	DestroyEnvMapCDFTextures();
	GpuResources::DestroySampler(m_Device, m_TextureSampler);
	GpuResources::DestroySampler(m_Device, m_CDFTextureSampler);
	GpuResources::DestroyBuffer(m_Device, m_CameraUBO, m_CameraUBOMemory);
	GpuResources::DestroyBuffer(m_Device, m_MaterialBuffer, m_MaterialBufferMemory);
	GpuResources::DestroyBuffer(m_Device, m_LightBuffer, m_LightBufferMemory);
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
	VkCommandBuffer cmd = Walnut::Application::GetCommandBuffer(true);

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

	Walnut::Application::FlushCommandBuffer(cmd);

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
		             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
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
		m_LightBuffer,
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

void RendererGPU::CreateTextures(const std::vector<SceneTexture>& textures)
{
	VkDevice device = m_Device.device;
	DestroyTextures();

	m_Textures.resize(textures.size());

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
			? (VkDeviceSize)(tex.width * tex.height * 8)  // 4× half-float = 8 bytes
			: (VkDeviceSize)(tex.width * tex.height * 4);  // 4× uint8 = 4 bytes

		// For HDR, convert float pixels to half-float
		std::vector<uint16_t> halfPixels;
		if (isHDR)
		{
			halfPixels.resize(tex.width * tex.height * 4);
			for (size_t p = 0; p < tex.floatPixels.size(); p++)
				halfPixels[p] = glm::packHalf1x16(tex.floatPixels[p]);
		}

		// Staging buffer
		VkBuffer stagingBuffer;
		VkDeviceMemory stagingMemory;
		GpuResources::CreateBuffer(m_Device, imageSize,
		             VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		             stagingBuffer, stagingMemory);

		void* data;
		vkMapMemory(device, stagingMemory, 0, imageSize, 0, &data);
		if (isHDR)
			memcpy(data, halfPixels.data(), (size_t)imageSize);
		else
			memcpy(data, tex.pixels.data(), (size_t)imageSize);
		vkUnmapMemory(device, stagingMemory);

		// Create VkImage via GpuResources
		GpuResources::CreateImage(m_Device, tex.width, tex.height, format,
			VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, gt);

		// Transition + copy via command buffer
		VkCommandBuffer cmd = Walnut::Application::GetCommandBuffer(true);

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
		region.bufferOffset = 0;
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

		// Transition to shader read optimal
		VkImageMemoryBarrier shaderBarrier = barrier;
		shaderBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		shaderBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		shaderBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		shaderBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &shaderBarrier);

		Walnut::Application::FlushCommandBuffer(cmd);

		GpuResources::DestroyBuffer(m_Device, stagingBuffer, stagingMemory);
	}

	RT_LOG("[RT2] Created %d GPU textures", (int)m_Textures.size());
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

	VkDevice device = m_Device.device;

	// Helper: create a CDF texture and append to m_Textures
	auto createCDFTexture = [&](const std::vector<float>& cdfData, int w, int h) -> int
	{
		GpuImage gt;
		gt.width = w;
		gt.height = h;
		gt.format = VK_FORMAT_R32_SFLOAT;
		VkDeviceSize imageSize = (VkDeviceSize)(w * h * 4);

		VkBuffer stagingBuffer;
		VkDeviceMemory stagingMemory;
		GpuResources::CreateBuffer(m_Device, imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		             stagingBuffer, stagingMemory);
		void* data;
		vkMapMemory(device, stagingMemory, 0, imageSize, 0, &data);
		memcpy(data, cdfData.data(), (size_t)imageSize);
		vkUnmapMemory(device, stagingMemory);

		// Create 1D/2D image via GpuResources (handles image + memory + view)
		GpuResources::CreateImage1D(m_Device, w, h, VK_FORMAT_R32_SFLOAT,
			VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, gt);

		VkCommandBuffer cmd = Walnut::Application::GetCommandBuffer(true);
		VkImageMemoryBarrier barrier = {};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.image = gt.image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.layerCount = 1;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

		VkBufferImageCopy region = {};
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.layerCount = 1;
		region.imageExtent = {(uint32_t)w, (h == 1) ? 1u : (uint32_t)h, 1};
		vkCmdCopyBufferToImage(cmd, stagingBuffer, gt.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

		VkImageMemoryBarrier shaderBarrier = barrier;
		shaderBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		shaderBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		shaderBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		shaderBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &shaderBarrier);
		Walnut::Application::FlushCommandBuffer(cmd);
		GpuResources::DestroyBuffer(m_Device, stagingBuffer, stagingMemory);

		int idx = (int)m_Textures.size();
		m_Textures.push_back(gt);
		return idx;
	};

	m_MarginalCDFIndex = createCDFTexture(sceneData.marginalCDF, sceneData.cdfHeight, 1);
	m_ConditionalCDFIndex = createCDFTexture(sceneData.conditionalCDF, sceneData.cdfWidth, sceneData.cdfHeight);

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
	RT_LOG("[RebuildAS] enter: meshes=%d", (int)m_CurrentScene.meshes.size());
	VkCommandBuffer cmd = Walnut::Application::GetCommandBuffer(true);
	RT_LOG("[RebuildAS] got command buffer");

	// Build one BLAS per mesh geometry
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

		// Check if this mesh's material uses alpha blending/cutout
		uint32_t matIdx = mesh.materialIndex;
		if (matIdx < m_CurrentScene.materials.size())
		{
			float alphaMode = m_CurrentScene.materials[matIdx].alphaMode;
			geo.isTransparent = (alphaMode > 0.5f);
		}

		geometries.push_back(geo);
	}
	RT_LOG("[RebuildAS] built %d geometry entries, calling BuildBLASes", (int)geometries.size());

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

	// Build TLAS instances — one per BLAS, with customIndex = material index
	// sbtHitOffset: 0 = opaque hit group (no any-hit), 1 = alpha hit group (any-hit)
	std::vector<BLASInstance> instances;
	instances.reserve(m_CurrentScene.meshes.size());
	for (size_t i = 0; i < m_CurrentScene.meshes.size(); i++)
	{
		VkTransformMatrixKHR transform = {};
		transform.matrix[0][0] = 1.0f;
		transform.matrix[1][1] = 1.0f;
		transform.matrix[2][2] = 1.0f;

		BLASInstance inst = {};
		inst.blasAddress = m_AS.GetBLASAddress(static_cast<uint32_t>(i));
		inst.customIndex = m_CurrentScene.meshes[i].materialIndex;
		inst.transform = transform;

		// Check if this mesh's material uses alpha blending/cutout
		uint32_t matIdx = m_CurrentScene.meshes[i].materialIndex;
		if (matIdx < m_CurrentScene.materials.size())
		{
			float alphaMode = m_CurrentScene.materials[matIdx].alphaMode;
			inst.sbtHitOffset = (alphaMode > 0.5f) ? 1u : 0u;  // 1 = alpha hit group
		}

		instances.push_back(inst);
	}

	bool tlasOK = m_AS.BuildTLAS(cmd, instances);
	RT_LOG("[RebuildAS] TLAS build result=%d instances=%d", tlasOK, (int)instances.size());

	Walnut::Application::FlushCommandBuffer(cmd);

	RT_LOG("[RebuildAS] creating material buffer");
	CreateMaterialBuffer();
	RT_LOG("[RebuildAS] creating light buffer");
	CreateLightBuffer();
	RT_LOG("[RebuildAS] updating descriptor set");
	UpdatePathTraceDescriptorSet();

	m_NeedsASRebuild = false;
	m_ASJustBuilt = true;
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

	VkDevice device = m_Device.device;
	void* data;
	vkMapMemory(device, m_CameraUBOMemory, 0, sizeof(SICameraData), 0, &data);
	memcpy(data, &ubo, sizeof(SICameraData));
	vkUnmapMemory(device, m_CameraUBOMemory);
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

	VkCommandBuffer cmd = Walnut::Application::GetCommandBuffer(true);

	// If the AS was just rebuilt this frame, insert a barrier so the
	// traceRayEXT sees the completed TLAS/BLAS writes (host-side flush
	// of the build command buffer is not sufficient for device visibility
	// across command buffers).
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

	// The output image stays in VK_IMAGE_LAYOUT_GENERAL for its entire lifetime:
	// both the RT storage-image write and the ImGui sampled-image read (whose
	// descriptor was registered with GENERAL in CreateOutputImage) are valid in
	// GENERAL. The previous GENERAL<->SHADER_READ_ONLY dance desynced from the
	// ImGui descriptor and produced validation errors + a black screen.
	VkImageMemoryBarrier rtWriteBarrier = {};
	rtWriteBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	rtWriteBarrier.srcAccessMask = 0;
	rtWriteBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	rtWriteBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	rtWriteBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	rtWriteBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	rtWriteBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	rtWriteBarrier.image = m_OutputImage;
	rtWriteBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	rtWriteBarrier.subresourceRange.levelCount = 1;
	rtWriteBarrier.subresourceRange.layerCount = 1;

	vkCmdPipelineBarrier(cmd,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0, nullptr, 0, nullptr, 1, &rtWriteBarrier);

	// Update NRD UBO
	VkDevice device = m_Device.device;
	if (m_NRDUBO)
	{
		SINRDUniformData nrdData = { m_NRDEnabled ? 1u : 0u, 0, 0, 0 };
		void* nrdMapped;
		vkMapMemory(device, m_NRDUBOMemory, 0, sizeof(SINRDUniformData), 0, &nrdMapped);
		memcpy(nrdMapped, &nrdData, sizeof(SINRDUniformData));
		vkUnmapMemory(device, m_NRDUBOMemory);
	}

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
	// Otherwise it was written by the ray tracing shader.
	VkPipelineStageFlags srcStage = (m_NRDEnabled && m_NRD.IsAvailable() && m_ComposePass.IsAvailable())
		? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
		: VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;

	vkCmdPipelineBarrier(cmd, srcStage, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &rtReadBarrier);

	Walnut::Application::FlushCommandBuffer(cmd);

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

	// Transition output image to TRANSFER_SRC layout, copy to buffer
	VkCommandBuffer cmd = Walnut::Application::GetCommandBuffer(true);

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

	Walnut::Application::FlushCommandBuffer(cmd);

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
	};

	VkCommandBuffer cmd = Walnut::Application::GetCommandBuffer(true);

	for (auto& s : specs)
	{
		// Create image via GpuResources (image + memory + view)
		GpuImage tmp;
		GpuResources::CreateImage(m_Device, m_Width, m_Height, s.format,
			VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, tmp);
		s.image = tmp.image;
		s.mem   = tmp.memory;
		s.view  = tmp.view;

		// Transition to GENERAL layout
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

	Walnut::Application::FlushCommandBuffer(cmd);
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
	};

	for (auto& p : pairs)
	{
		if (p.view) { vkDestroyImageView(device, p.view, nullptr); p.view = VK_NULL_HANDLE; }
		if (p.img)  { vkDestroyImage(device, p.img, nullptr);      p.img  = VK_NULL_HANDLE; }
		if (p.mem)  { vkFreeMemory(device, p.mem, nullptr);        p.mem  = VK_NULL_HANDLE; }
	}
}

void RendererGPU::CreateGBufferDescriptorSet()
{
	VkDevice device = m_Device.device;

	// Set 1 layout: 7 storage images (0-5, 7) + 1 UBO (6)
	VkDescriptorSetLayoutBinding bindings[8] = {};
	// Bindings 0-5: storage images (gNormalRoughness, gViewZ, gMotion, gDiff, gSpec, gAlbedoF0)
	for (int i = 0; i < 6; i++)
	{
		bindings[i].binding = i; // G-buffer bindings 0-5 match SI_BINDING_G_* 0-5
		bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
	}
	// Binding 6: UBO (nrdData)
	bindings[SI_BINDING_NRD_UBO].binding = SI_BINDING_NRD_UBO;
	bindings[SI_BINDING_NRD_UBO].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	bindings[SI_BINDING_NRD_UBO].descriptorCount = 1;
	bindings[SI_BINDING_NRD_UBO].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
	// Binding 7: gDirectEmission storage image
	bindings[SI_BINDING_G_DIRECT_EMISSION].binding = SI_BINDING_G_DIRECT_EMISSION;
	bindings[SI_BINDING_G_DIRECT_EMISSION].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	bindings[SI_BINDING_G_DIRECT_EMISSION].descriptorCount = 1;
	bindings[SI_BINDING_G_DIRECT_EMISSION].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;

	VkDescriptorSetLayoutCreateInfo layoutInfo = {};
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = 8;
	layoutInfo.pBindings = bindings;

	vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_GBufferSetLayout);

	// Create dedicated pool
	VkDescriptorPoolSize poolSizes[2] = {};
	poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	poolSizes[0].descriptorCount = 7;
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
		GpuResources::CreateBuffer(m_Device, sizeof(SINRDUniformData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
		             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		             m_NRDUBO, m_NRDUBOMemory);
	}
}

void RendererGPU::UpdateGBufferDescriptorSet()
{
	VkDevice device = m_Device.device;

	VkDescriptorImageInfo imageInfos[7] = {};
	VkImageView views[] = { m_GNormalRoughnessView, m_GViewZView, m_GMotionView, m_GDiffRadianceView, m_GSpecRadianceView, m_GAlbedoF0View, m_GDirectEmissionView };
	for (int i = 0; i < 7; i++)
	{
		imageInfos[i].imageView = views[i];
		imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	}

	VkDescriptorBufferInfo uboInfo = {};
	uboInfo.buffer = m_NRDUBO;
	uboInfo.range = VK_WHOLE_SIZE;

	VkWriteDescriptorSet writes[8] = {};
	// Storage images: bindings 0-5 and 7
	for (int i = 0; i < 6; i++)
	{
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = m_GBufferSet;
		writes[i].dstBinding = i; // G-buffer 0-5
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

	vkUpdateDescriptorSets(device, 8, writes, 0, nullptr);
}