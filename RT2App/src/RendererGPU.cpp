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

	m_Scene.InitSamplers(m_Device);
	CreateFallbackTexture();
	ShaderManager::Init(m_Device.device);

	// Create G-buffer descriptor set layout (set 1) first — needed by PathTracePass
	CreateGBufferDescriptorSet();

	if (!m_PathTracePass.Init(m_Device, m_GBufferSetLayout))
	{
		RT_LOG("[RT2] GPU renderer initialization failed (pipeline creation error)");
		return false;
	}
	m_ComposePass.Init(m_Device);

	m_ReSTIRPass.Init(m_Device, m_PathTracePass.GetDescriptorSetLayout(), m_GBufferSetLayout);

	if (!m_RasterPass.Init(m_Device, m_PathTracePass.GetDescriptorSetLayout(), m_GBufferSetLayout))
	{
		RT_LOG("[RT2] RasterPass init failed (non-fatal, RT primary visibility will be used)");
	}

	m_GBufferDebugPass.Init(m_Device, m_PathTracePass.GetDescriptorSetLayout(), m_GBufferSetLayout);

	// Create frames-in-flight ring
	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
		m_Frames[i].Init(m_Device.device, m_Device.queueFamily);
	m_GpuProfiler.Init(m_Device, MAX_FRAMES_IN_FLIGHT);

	m_Initialized = true;
	return true;
}

void RendererGPU::Destroy()
{
	VkDevice device = m_Device.device;
	vkDeviceWaitIdle(device);
	m_GpuProfiler.Destroy(device);

	m_NRD.Destroy();
	m_ReSTIRPass.Destroy();
	m_Reservoirs.Destroy();
	m_PathTracePass.Destroy();
	m_ComposePass.Destroy();
	m_RasterPass.Destroy();
	m_GBufferDebugPass.Destroy();
	DestroyOutputImage();
	DestroyGBufferImages();
	m_Scene.Destroy();
	GpuResources::DestroyImage(m_Device, m_FallbackTexture);
	GpuResources::DestroyBuffer(m_Device, m_CameraUBO, m_CameraUBOMemory);
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
	m_OutputImage.image    = outputImg.image;
	m_OutputImage.memory   = outputImg.memory;
	m_OutputImage.view = outputImg.view;

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
		barrier.image = m_OutputImage.image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.layerCount = 1;

		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	});

	m_OutputImageLayout = VK_IMAGE_LAYOUT_GENERAL;

	// Create ImGui descriptor set for display
	m_ImGuiDescriptorSet = (VkDescriptorSet)ImGui_ImplVulkan_AddTexture(m_Sampler, m_OutputImage.view, VK_IMAGE_LAYOUT_GENERAL);
}

void RendererGPU::DestroyOutputImage()
{
	VkDevice device = m_Device.device;

	// Free the ImGui descriptor set before destroying the image view/sampler
	// it references â€” otherwise the GPU may sample a destroyed resource.
	if (m_ImGuiDescriptorSet)
	{
		vkFreeDescriptorSets(device, m_Device.descriptorPool,
		                     1, &m_ImGuiDescriptorSet);
		m_ImGuiDescriptorSet = VK_NULL_HANDLE;
	}

	if (m_Sampler) vkDestroySampler(device, m_Sampler, nullptr);
	if (m_OutputImage.view) vkDestroyImageView(device, m_OutputImage.view, nullptr);
	if (m_OutputImage.image) vkDestroyImage(device, m_OutputImage.image, nullptr);
	if (m_OutputImage.memory) vkFreeMemory(device, m_OutputImage.memory, nullptr);
	m_Sampler = VK_NULL_HANDLE;
	m_OutputImage.view = VK_NULL_HANDLE;
	m_OutputImage.image = VK_NULL_HANDLE;
	m_OutputImage.memory = VK_NULL_HANDLE;
	m_OutputImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}

void RendererGPU::CreateFallbackTexture()
{
	uint8_t white[4] = {255, 255, 255, 255};

	GpuResources::CreateImage(m_Device, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
		VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_FallbackTexture);

	VkBuffer stagingBuf; VkDeviceMemory stagingMem;
	GpuResources::CreateBuffer(m_Device, 4,
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		stagingBuf, stagingMem);

	void* mapped = nullptr;
	vkMapMemory(m_Device.device, stagingMem, 0, 4, 0, &mapped);
	memcpy(mapped, white, 4);
	vkUnmapMemory(m_Device.device, stagingMem);

	CommandUtils::ImmediateSubmit(m_Device, [&](VkCommandBuffer cmd) {
		VkImageMemoryBarrier barrier = {};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = m_FallbackTexture.image;
		barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

		VkBufferImageCopy region = {};
		region.bufferOffset = 0;
		region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		region.imageExtent = {1, 1, 1};
		vkCmdCopyBufferToImage(cmd, stagingBuf, m_FallbackTexture.image,
		                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

		VkImageMemoryBarrier shaderBarrier = {};
		shaderBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		shaderBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		shaderBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		shaderBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		shaderBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		shaderBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		shaderBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		shaderBarrier.image = m_FallbackTexture.image;
		shaderBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &shaderBarrier);
	});

	GpuResources::DestroyBuffer(m_Device, stagingBuf, stagingMem);
}

void RendererGPU::OnResize(uint32_t width, uint32_t height)
{
	if (width == 0 || height == 0)
		return;

	if (m_Width == width && m_Height == height && m_OutputImage.image != VK_NULL_HANDLE)
		return;

	VkDevice device = m_Device.device;
	vkDeviceWaitIdle(device);

	// Free old descriptor set before allocating a new one
	m_PathTracePass.FreeDescriptorSet();

	DestroyOutputImage();

	m_Width = width;
	m_Height = height;

	CreateOutputImage();
	CreateGBufferImages();
	m_Reservoirs.Create(m_Device, m_Width, m_Height);
	// Allocate descriptor set with current texture count (0 if no scene yet)
	uint32_t texCount = (uint32_t)m_Scene.GetTextures().size();
	m_PathTracePass.CreateDescriptorSet(m_Device, texCount > 0 ? texCount : 1);
	UpdateGBufferDescriptorSet();

	// Initialize NRD if enabled
	if (m_Settings.nrdEnabled && !m_NRD.IsAvailable())
	{
		m_NRD.Init(m_Device.instance,
		           m_Device.physicalDevice,
		           m_Device.device,
		           m_Device.queue,
		           m_Device.queueFamily,
		           m_Width, m_Height);
	}
	else if (m_Settings.nrdEnabled && m_NRD.IsAvailable())
	{
		m_NRD.OnResize(m_Width, m_Height);
	}
	UpdatePathTraceDescriptorSet();

	m_FrameIndex = 1;
	m_NRDFrameIndex = 1;
	m_NRDNeedsReset = true;
	m_ComposeDescriptorSetCached = false;
	InvalidateReSTIRHistory();
}

void RendererGPU::UpdatePathTraceDescriptorSet()
{
	RT_LOG("[UpdateDS] enter"); fflush(stdout);
	if (!m_PathTracePass.IsAvailable()) return;
	if (!m_Scene.IsValid()) { RT_LOG("[UpdateDS] skip: AS not valid"); return; }

	// Create camera UBO if needed
	if (!m_CameraUBO)
	{
		RT_LOG("[UpdateDS] creating camera UBO"); fflush(stdout);
		GpuResources::CreateBuffer(m_Device, sizeof(SICameraData),
		             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		             m_CameraUBO, m_CameraUBOMemory);
	}
	if (!m_Scene.GetMaterialBuffer()) { RT_LOG("[UpdateDS] skip: no material buffer"); return; }

	RT_LOG("[UpdateDS] checking reservoirs (valid=%d w=%d h=%d)", (int)m_Reservoirs.IsValid(), m_Width, m_Height); fflush(stdout);
	if (!m_Reservoirs.IsValid() || !m_Reservoirs.MatchesSize(m_Width, m_Height))
	{
		if (m_Width > 0 && m_Height > 0)
			m_Reservoirs.Create(m_Device, m_Width, m_Height);
	}

	RT_LOG("[UpdateDS] building texture infos"); fflush(stdout);
	std::vector<VkDescriptorImageInfo> textureImageInfos;
	for (const auto& gt : m_Scene.GetTextures())
	{
		VkDescriptorImageInfo imgInfo = {};
		imgInfo.sampler = m_Scene.GetTextureSampler();
		imgInfo.imageView = gt.view ? gt.view : m_FallbackTexture.view;
		imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		textureImageInfos.push_back(imgInfo);
	}

	// Bounds check: texture count must not exceed MAX_TEXTURES
	const uint32_t MAX_TEXTURES = PathTracePass::MAX_TEXTURES;
	if (textureImageInfos.size() > MAX_TEXTURES)
	{
		RT_LOG("[UpdateDS] ERROR: texture count %zu exceeds MAX_TEXTURES %u — skipping descriptor update",
		       textureImageInfos.size(), MAX_TEXTURES);
		return;
	}

	// Ensure descriptor set has enough texture slots allocated.
	// CreateDescriptorSet re-allocates if the current set has fewer slots than needed.
	uint32_t texCount = (uint32_t)textureImageInfos.size();
	if (texCount == 0) texCount = 1;  // need at least 1 slot for fallback
	if (!m_PathTracePass.CreateDescriptorSet(m_Device, texCount))
	{
		RT_LOG("[UpdateDS] ERROR: failed to allocate descriptor set for %u textures", texCount);
		return;
	}

	// Bounds check: material texture indices must be < texture count
	const auto& scene = m_Scene.GetScene();
	uint32_t sceneTexCount = (uint32_t)m_Scene.GetTextures().size();
		for (size_t i = 0; i < scene.materials.size(); i++)
	{
		const auto& mat = scene.materials[i];
		int indices[] = { mat.textureIndices.x, mat.textureIndices.y, mat.textureIndices.z, mat.metallicRoughnessTextureIndex };
		const char* names[] = { "baseColor", "normal", "emissive", "metallicRoughness" };
		for (int j = 0; j < 4; j++)
		{
			if (indices[j] >= 0 && (uint32_t)indices[j] >= sceneTexCount)
			{
				RT_LOG("[UpdateDS] ERROR: material %zu %s texIdx=%d >= texCount=%u — skipping descriptor update",
				       i, names[j], indices[j], sceneTexCount);
				return;
			}
		}
	}

	// Bounds check: env/CDF indices
	if (m_Scene.GetEnvMapIndex() >= 0 && (uint32_t)m_Scene.GetEnvMapIndex() >= sceneTexCount)
	{
		RT_LOG("[UpdateDS] ERROR: envMapIndex=%d >= texCount=%u — skipping", m_Scene.GetEnvMapIndex(), sceneTexCount);
		return;
	}
	if (m_Scene.GetMarginalCDFIndex() >= 0 && (uint32_t)m_Scene.GetMarginalCDFIndex() >= sceneTexCount)
	{
		RT_LOG("[UpdateDS] ERROR: marginalCDFIndex=%d >= texCount=%u — skipping", m_Scene.GetMarginalCDFIndex(), sceneTexCount);
		return;
	}
	if (m_Scene.GetConditionalCDFIndex() >= 0 && (uint32_t)m_Scene.GetConditionalCDFIndex() >= sceneTexCount)
	{
		RT_LOG("[UpdateDS] ERROR: conditionalCDFIndex=%d >= texCount=%u — skipping", m_Scene.GetConditionalCDFIndex(), sceneTexCount);
		return;
	}

	RT_LOG("[UpdateDS] calling UpdateDescriptorSet (matOffBuf=%p)", (void*)m_Scene.GetInstanceMatOffsetBuffer()); fflush(stdout);
	m_PathTracePass.UpdateDescriptorSet(m_Device,
		m_OutputImage.view, m_Sampler,
		m_CameraUBO, m_Scene.GetMaterialBuffer(),
		m_Scene.GetVertexBuffer(), m_Scene.GetIndexBuffer(),
		m_Scene.GetNormalBuffer(), m_Scene.GetUVBuffer(),
		m_Scene.GetInstanceMeshInfoBuffer(),
		m_Scene.GetLightBuffer(), m_Scene.GetInstanceTransformBuffer(),
		m_Scene.GetInstanceTransformPrevBuffer(), m_Scene.GetMaterialIndexBuffer(),
		m_Scene.GetInstanceMatOffsetBuffer(),
		m_Scene.GetTLAS(),
		m_Reservoirs.GetHistoryBuffer(), m_Reservoirs.GetScratchBuffer(),
		m_Reservoirs.GetSurfaceHistoryBuffer(),
		textureImageInfos);

	RT_LOG("[UpdateDS] done (textures=%d, descSet=%p, TLAS=%p, outView=%p, camUBO=%p, matBuf=%p)",
	       (int)textureImageInfos.size(), (void*)m_PathTracePass.GetDescriptorSet(),
	       (void*)m_Scene.GetTLAS(), (void*)m_OutputImage.view,
	       (void*)m_CameraUBO, (void*)m_Scene.GetMaterialBuffer());
}

void RendererGPU::SetSceneKeepTextures(const GPUSceneData& sceneData)
{
	RT_LOG("[SetSceneKeepTextures] enter: meshes=%zu instances=%zu lights=%zu",
	       sceneData.meshes.size(), sceneData.instances.size(), sceneData.lights.size());
	m_Scene.SetSceneKeepTextures(m_Device, sceneData);
	m_FrameIndex = 1;
	m_NRDFrameIndex = 1;
	m_NRDNeedsReset = true;
	m_ComposeDescriptorSetCached = false;
	InvalidateReSTIRHistory();

	if (m_Scene.IsValid() && !m_Scene.NeedsASRebuild() && !m_Scene.IsTextureUploadPending())
	{
		RT_LOG("[SetSceneKeepTextures] updating descriptor set");
		UpdatePathTraceDescriptorSet();
	}
	RT_LOG("[SetSceneKeepTextures] done");
}

void RendererGPU::SetScene(GPUSceneData& sceneData)
{
	RT_LOG("[SetScene] enter: meshes=%zu instances=%zu lights=%zu",
	       sceneData.meshes.size(), sceneData.instances.size(), sceneData.lights.size());
	m_Scene.SetScene(m_Device, sceneData);
	m_FrameIndex = 1;
	m_NRDFrameIndex = 1;
	m_NRDNeedsReset = true;
	InvalidateReSTIRHistory();

	// Update descriptor set if AS is already valid (no rebuild needed)
	// and textures are not still loading asynchronously.
	if (m_Scene.IsValid() && !m_Scene.NeedsASRebuild() && !m_Scene.IsTextureUploadPending())
	{
		RT_LOG("[SetScene] updating descriptor set (AS valid, no rebuild needed)");
		UpdatePathTraceDescriptorSet();
	}
	else
	{
		RT_LOG("[SetScene] skipping descriptor update (AS needs rebuild or textures loading)");
	}
}

void RendererGPU::UpdateSceneInstances(const GPUSceneData& sceneData)
{
	vkDeviceWaitIdle(m_Device.device);

	m_Scene.UpdateInstances(m_Device, sceneData);
	UpdatePathTraceDescriptorSet();
	InvalidateReSTIRHistory();
}

void RendererGPU::DumpInstanceTransforms() const
{
	m_Scene.DumpInstanceTransforms();
}

void RendererGPU::DumpNEEBuffers() const
{
	m_Scene.DumpNEEBuffers();
	RT_LOG("  m_FrameIndex=%d m_NRDFrameIndex=%d m_NRDNeedsReset=%d m_HasPrevMatrices=%d",
	       m_FrameIndex, m_NRDFrameIndex, m_NRDNeedsReset, m_HasPrevMatrices);
	RT_LOG("  rasterFirst=%d nrdEnabled=%d",
	       m_Settings.rasterFirst, m_Settings.nrdEnabled);
}

void RendererGPU::ResetAccumulation()
{
	m_FrameIndex = 1;
	m_NRDFrameIndex = 1;
	m_NRDNeedsReset = true;
	m_HasPrevMatrices = false;
	m_PrevViewToClipForFrame = glm::mat4(1.0f);
	m_PrevWorldToViewForFrame = glm::mat4(1.0f);
}

void RendererGPU::InvalidateReSTIRHistory()
{
	m_ReSTIRHistoryInvalidated = true;
	m_ReSTIRHistoryVersion++;
	m_ReSTIRFrameIndex = 1;
}

void RendererGPU::ApplySettings(const RenderSettings& newSettings)
{
	bool wasRestirEnabled = m_Settings.restirEnabled;
	bool restirJitterPolicyChanged = m_Settings.nrdJitterEnabled != newSettings.nrdJitterEnabled ||
	                                m_Settings.nrdJitterScale != newSettings.nrdJitterScale;
	bool restirPolicyChanged = m_Settings.restirFreshCandidates != newSettings.restirFreshCandidates ||
	                          m_Settings.restirTemporalMCap != newSettings.restirTemporalMCap ||
	                          m_Settings.restirSpatialMCap != newSettings.restirSpatialMCap ||
	                          m_Settings.restirTemporalReuse != newSettings.restirTemporalReuse ||
	                          m_Settings.restirSpatialReuse != newSettings.restirSpatialReuse ||
	                          m_Settings.restirSpatialNeighbors != newSettings.restirSpatialNeighbors ||
	                          m_Settings.restirSpatialRadius != newSettings.restirSpatialRadius ||
	                          m_Settings.restirDepthThreshold != newSettings.restirDepthThreshold ||
	                          m_Settings.restirNormalThreshold != newSettings.restirNormalThreshold ||
	                          m_Settings.restirWorldPosThreshold != newSettings.restirWorldPosThreshold ||
	                          m_Settings.restirMaxTemporalAge != newSettings.restirMaxTemporalAge;
	if (m_Settings.spp != newSettings.spp ||
	    m_Settings.maxBounces != newSettings.maxBounces ||
	    m_Settings.showBackground != newSettings.showBackground ||
	    m_Settings.neeOnly != newSettings.neeOnly ||
	    m_Settings.emissiveBoost != newSettings.emissiveBoost ||
	    m_Settings.envIntensity != newSettings.envIntensity ||
	    m_Settings.rasterFirst != newSettings.rasterFirst ||
	    m_Settings.nrdEnabled != newSettings.nrdEnabled ||
	    m_Settings.nrdLobeDither != newSettings.nrdLobeDither ||
	    m_Settings.nrdMaxBlurRadius != newSettings.nrdMaxBlurRadius ||
	    m_Settings.nrdMaxAccumFrames != newSettings.nrdMaxAccumFrames ||
	    m_Settings.nrdAntiFirefly != newSettings.nrdAntiFirefly ||
	    m_Settings.nrdSplitScreen != newSettings.nrdSplitScreen ||
	    m_Settings.nrdJitterEnabled != newSettings.nrdJitterEnabled ||
	    m_Settings.nrdJitterScale != newSettings.nrdJitterScale ||
	    m_Settings.restirEnabled != newSettings.restirEnabled ||
	    m_Settings.restirFreshCandidates != newSettings.restirFreshCandidates ||
	    m_Settings.restirTemporalReuse != newSettings.restirTemporalReuse ||
	    m_Settings.restirSpatialReuse != newSettings.restirSpatialReuse ||
	    m_Settings.restirSpatialNeighbors != newSettings.restirSpatialNeighbors ||
	    m_Settings.restirSpatialRadius != newSettings.restirSpatialRadius ||
	    m_Settings.restirTemporalMCap != newSettings.restirTemporalMCap ||
	    m_Settings.restirSpatialMCap != newSettings.restirSpatialMCap ||
	    m_Settings.restirDepthThreshold != newSettings.restirDepthThreshold ||
	    m_Settings.restirNormalThreshold != newSettings.restirNormalThreshold ||
	    m_Settings.restirWorldPosThreshold != newSettings.restirWorldPosThreshold ||
	    m_Settings.restirMaxTemporalAge != newSettings.restirMaxTemporalAge ||
	    m_Settings.gbufferDebugMode != newSettings.gbufferDebugMode)
	{
		m_Settings.dirty = true;
	}
	m_Settings = newSettings;

	if (m_Settings.restirEnabled != wasRestirEnabled ||
	    (m_Settings.restirEnabled && (restirJitterPolicyChanged || restirPolicyChanged)))
		InvalidateReSTIRHistory();
}

void RendererGPU::UpdateCameraUBO(const Camera& camera)
{
	SICameraData ubo = {};
	ubo.position = glm::vec4(camera.GetPosition(), (float)m_FrameIndex);

	// NRD camera jitter (Halton sequence, subpixel offset in [-0.5, 0.5])
	m_NRDJitterPrev = m_NRDJitter;
	if (m_Settings.nrdEnabled && m_Settings.nrdJitterEnabled)
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
		// Use m_NRDFrameIndex (continuously incrementing) for stable jitter sequence
		int frame = (m_NRDFrameIndex - 1) % 16 + 1; // cycle through 16 offsets
		m_NRDJitter = (glm::vec2(halton(frame, 2) - 0.5f, halton(frame, 3) - 0.5f)) * m_Settings.nrdJitterScale;
	}
	else
	{
		m_NRDJitter = glm::vec2(0.0f);
	}

	ubo.forward = glm::vec4(camera.GetDirection(), m_NRDJitter.x);
	glm::vec3 right = glm::cross(camera.GetDirection(), glm::vec3(0, 1, 0));
	glm::vec3 up = glm::cross(right, camera.GetDirection());
	ubo.right = glm::vec4(right, m_NRDJitter.y);
	ubo.up = glm::vec4(up, m_Settings.restirEnabled ? 1.0f : 0.0f);
	int maxBouncesClamped = m_Settings.maxBounces;
	const int bounceLimit = (int)m_PathTracePass.GetMaxRecursionDepth() - 1;
	if (maxBouncesClamped > bounceLimit) maxBouncesClamped = bounceLimit;
	ubo.viewportSPP = glm::vec4((float)m_Width, (float)m_Height, (float)m_Settings.spp, (float)maxBouncesClamped);
	ubo.apertureFocal = glm::vec4(camera.m_Aperture, camera.m_FocusDistance, m_Settings.showBackground ? 1.0f : 0.0f, m_Settings.emissiveBoost);
	ubo.envMap = glm::vec4((float)m_Scene.GetEnvMapIndex(), m_Settings.envIntensity, (float)m_Scene.GetMarginalCDFIndex(), (float)m_Scene.GetConditionalCDFIndex());
	ubo.inverseProjection = camera.GetInverseProjection();
	ubo.inverseView = camera.GetInverseView();
	ubo.viewToClip = camera.GetProjection();
	ubo.worldToView = camera.GetView();
	glm::mat4 prevViewToClip = m_HasPrevMatrices ? m_PrevViewToClip : camera.GetProjection();
	glm::mat4 prevWorldToView = m_HasPrevMatrices ? m_PrevWorldToView : camera.GetView();
	ubo.viewToClipPrev = prevViewToClip;
	ubo.worldToViewPrev = prevWorldToView;

	// Update previous frame matrices for next frame
	m_PrevViewToClip = ubo.viewToClip;
	m_PrevWorldToView = ubo.worldToView;
	m_HasPrevMatrices = true;

	m_CameraUBOData = ubo; // stash for vkCmdUpdateBuffer in Render()
	m_PrevViewToClipForFrame = prevViewToClip;
	m_PrevWorldToViewForFrame = prevWorldToView;
}

void RendererGPU::Render(const Camera& camera)
{
	if (!m_Initialized || m_OutputImage.image == VK_NULL_HANDLE) return;

	// Check dirty flag — auto reset accumulation on settings change
	if (m_Settings.dirty)
	{
		ResetAccumulation();
		m_Settings.dirty = false;
	}

	RT_LOG("[Render] frame=%d needsASRebuild=%d", m_FrameIndex, m_Scene.NeedsASRebuild());

	// Poll async texture upload — adopt textures when the GPU fence signals
	if (m_Scene.PollTextureUpload())
	{
		RT_LOG("[Render] async textures adopted, updating descriptor set");
		if (m_Scene.IsValid() && !m_Scene.NeedsASRebuild())
			UpdatePathTraceDescriptorSet();
	}

	if (m_Scene.NeedsASRebuild())
	{
		RT_LOG("[Render] rebuilding AS...");
		m_Scene.RebuildAccelerationStructures(m_Device, [this](const GpuDevice& dev, const GPUSceneData& scene) {
			m_RasterPass.CreateVertexBuffers(dev, scene);
			m_RasterPass.CreateDrawData(dev, scene);
		});
		RT_LOG("[Render] AS rebuild done, AS valid=%d", m_Scene.IsValid());
		if (m_Scene.IsValid())
			UpdatePathTraceDescriptorSet();
	}

	if (!m_Scene.IsValid())
	{
		static bool warned = false;
		if (!warned) { RT_LOG("[RT2] Render: TLAS not valid (no mesh loaded?)"); warned = true; }
		return;
	}

	if (m_Scene.IsTextureUploadPending())
	{
		return;
	}

	if (const_cast<Camera&>(camera).checkHasMoved())
		m_FrameIndex = 1;

	// Lazy-init NRD when toggled on
	if (m_Settings.nrdEnabled && !m_NRD.IsAvailable() && m_Width > 0 && m_Height > 0)
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

	if (!m_PathTracePass.IsAvailable() || !m_PathTracePass.GetDescriptorSet() || !m_CameraUBO || !m_Scene.GetMaterialBuffer())
	{
		RT_LOG("[RT2] Render: missing resource (pipe=%d ubo=%p mat=%p)",
		       m_PathTracePass.IsAvailable(), (void*)m_CameraUBO, (void*)m_Scene.GetMaterialBuffer());
		return;
	}

	VkDevice device = m_Device.device;

	// ---- Frames-in-flight ring: wait for this frame slot to be free ----
	FrameContext& frame = m_Frames[m_CurrentFrame];
	frame.WaitForFence(device);
	m_GpuProfiler.ReadCompletedSlot(device, m_CurrentFrame);
	frame.Begin(device);
	VkCommandBuffer cmd = frame.commandBuffer;
	m_GpuProfiler.BeginFrame(cmd, m_CurrentFrame, m_FrameIndex);

	// Clear ReSTIR history if invalidated (resize, scene change, enable toggle)
	if (m_ReSTIRHistoryInvalidated && m_Reservoirs.IsValid())
	{
		m_Reservoirs.ClearHistory(cmd);
		m_ReSTIRHistoryInvalidated = false;
	}

	// Build ReSTIR push constants from settings
	SIReSTIRPushConstants restirPC = {};
	restirPC.freshCandidateCount = m_Settings.restirFreshCandidates;
	restirPC.temporalMCap = m_Settings.restirTemporalMCap;
	restirPC.spatialMCap = m_Settings.restirSpatialMCap;
	restirPC.spatialNeighborCount = m_Settings.restirSpatialNeighbors;
	restirPC.spatialRadius = m_Settings.restirSpatialRadius;
	restirPC.depthThreshold = m_Settings.restirDepthThreshold;
	restirPC.normalThreshold = m_Settings.restirNormalThreshold;
	restirPC.worldPosThreshold = m_Settings.restirWorldPosThreshold;
	restirPC.maxTemporalAge = m_Settings.restirMaxTemporalAge;
	restirPC.flags = 0;
	if (m_Settings.restirTemporalReuse) restirPC.flags |= 1u;
	if (m_Settings.restirSpatialReuse)  restirPC.flags |= 2u;
	restirPC.frameIndex = m_ReSTIRFrameIndex;
	restirPC.jitter = glm::vec4(m_NRDJitter, m_NRDJitterPrev);

	// Build the frame render context and delegate to FrameRenderer
	FrameRenderer::Context ctx = {
		m_Device,
		&m_GpuProfiler,
		m_GBuffer,
		m_Scene,
		m_PathTracePass,
		m_RasterPass,
		m_GBufferDebugPass,
		m_ComposePass,
		m_ReSTIRPass,
		m_Reservoirs,
		m_NRD,
		m_OutputImage,
		m_GBufferSet,
		m_CameraUBO,
		m_NRDUBO,
		m_CameraUBOData,
		m_Width,
		m_Height,
		m_Settings.rasterFirst,
		m_Settings.nrdEnabled,
		m_Settings.nrdLobeDither,
		m_Settings.restirEnabled,
		restirPC,
		m_Settings.gbufferDebugMode,
		m_Settings.nrdMaxBlurRadius,
		m_Settings.nrdMaxAccumFrames,
		m_Settings.nrdAntiFirefly,
		m_Settings.nrdSplitScreen,
		m_NRDJitter,
		m_NRDJitterPrev,
		m_NRDFrameIndex,
		m_NRDNeedsReset,
		m_HasPrevMatrices,
		m_PrevViewToClipForFrame,
		m_PrevWorldToViewForFrame,
		m_ComposeDescriptorSetCached,
		camera
	};

	FrameRenderer::RecordFrame(cmd, ctx);
	m_GpuProfiler.EndFrame(m_CurrentFrame);

	// ---- Submit this frame's work (async, no wait) ----
	RT_LOG("[Render] submitting frame %d (NRD=%d composeCached=%d)",
	       m_FrameIndex, m_Settings.nrdEnabled ? 1 : 0, m_ComposeDescriptorSetCached ? 1 : 0);
	frame.Submit(m_Device.queue);
	RT_LOG("[Render] submit ok, frame %d", m_FrameIndex);

	m_CurrentFrame = (m_CurrentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
	m_FrameIndex++;
	m_NRDFrameIndex++;
	m_ReSTIRFrameIndex++;
}

bool RendererGPU::ReadbackOutput(std::vector<uint8_t>& outPixelsRGBA8, uint32_t& outWidth, uint32_t& outHeight)
{
	if (!m_Initialized || m_OutputImage.image == VK_NULL_HANDLE || m_Width == 0 || m_Height == 0)
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
		toTransfer.image = m_OutputImage.image;
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
		vkCmdCopyImageToBuffer(cmd, m_OutputImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stagingBuffer, 1, &region);

		// Transition back to GENERAL
		VkImageMemoryBarrier toGeneral = toTransfer;
		toGeneral.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		toGeneral.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		toGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0,
		                     0, nullptr, 0, nullptr, 1, &toGeneral);
	});

	// Map and convert R32G32B32A32 float â†’ RGBA8 (tonemap + sRGB)
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
	RT_LOG("[Readback] captured %ux%u â†’ %zu bytes", m_Width, m_Height, outPixelsRGBA8.size());
	return true;
}

// ---- NRD G-buffer images (set 1) -------------------------------------------

void RendererGPU::CreateGBufferImages()
{
	m_GBuffer.Create(m_Device, m_Width, m_Height);
}

void RendererGPU::DestroyGBufferImages()
{
	m_GBuffer.Destroy();
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
		m_GBuffer.GetColor(GBufferTarget::NORMAL_ROUGHNESS).view, m_GBuffer.GetColor(GBufferTarget::VIEWZ).view, m_GBuffer.GetColor(GBufferTarget::MOTION).view,
		m_GBuffer.GetColor(GBufferTarget::DIFF_RADIANCE).view, m_GBuffer.GetColor(GBufferTarget::SPEC_RADIANCE).view, m_GBuffer.GetColor(GBufferTarget::ALBEDO_F0).view,
		m_GBuffer.GetColor(GBufferTarget::DIRECT_EMISSION).view,
		m_GBuffer.GetColor(GBufferTarget::PRIM_HIT).view, m_GBuffer.GetColor(GBufferTarget::PRIM_GEO_NORMAL).view, m_GBuffer.GetColor(GBufferTarget::PRIM_UV).view
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
