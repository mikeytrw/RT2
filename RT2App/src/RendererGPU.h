#pragma once

#ifndef RENDERER_GPU_H
#define RENDERER_GPU_H

#include "vulkan/vulkan.h"
#include "AccelerationStructure.h"
#include "Camera.h"
#include "GPUSceneData.h"
#include "NRDIntegration.h"
#include "Scene.h"
#include <memory>

// A single GPU texture (VkImage + view + sampler + staging)
struct GPUTexture
{
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VkSampler sampler = VK_NULL_HANDLE;
	int width = 0;
	int height = 0;
	VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
};

class RendererGPU
{
public:
	RendererGPU() = default;
	~RendererGPU() { Destroy(); }

	void Destroy();

	bool IsAvailable() const { return m_Initialized; }

	void OnResize(uint32_t width, uint32_t height);
	void Render(const Camera& camera);
	void SetScene(const GPUSceneData& sceneData);

	VkDescriptorSet GetOutputDescriptorSet() const { return m_ImGuiDescriptorSet; }
	bool HasOutput() const { return m_OutputImage != VK_NULL_HANDLE; }
	uint32_t GetWidth() const { return m_Width; }
	uint32_t GetHeight() const { return m_Height; }

	bool Init();
	void RebuildAccelerationStructures();
	void ResetAccumulation();

	int m_SPP = 5;
	int m_MaxBounces = 8;
	bool m_ShowBackground = false;
	bool m_NEEOnly = false;
	float m_EmissiveBoost = 1.0f;
	float m_EnvIntensity = 1.0f;
	bool m_NRDEnabled = false;  // NRD denoiser toggle

	// NRD tunable settings (exposed in UI)
	float m_NRDMaxBlurRadius = 30.0f;
	int m_NRDMaxAccumFrames = 30;
	bool m_NRDAntiFirefly = true;
	float m_NRDSplitScreen = 0.0f;

private:
	void CreateOutputImage();
	void DestroyOutputImage();
	void CreatePipeline();
	void DestroyPipeline();
	void CreateDescriptorSet();
	void UpdateDescriptorSet();
	void CreateMaterialBuffer();
	void CreateLightBuffer();
	void UpdateCameraUBO(const Camera& camera);
	void CreateTextures(const std::vector<SceneTexture>& textures);
	void DestroyTextures();
	void CreateEnvMapCDFTextures(const GPUSceneData& sceneData);
	void DestroyEnvMapCDFTextures();

	// NRD G-buffer images (set 1, bindings 0-4)
	void CreateGBufferImages();
	void DestroyGBufferImages();
	void CreateGBufferDescriptorSet();
	void UpdateGBufferDescriptorSet();

	// Compose pass (compute shader)
	void CreateComposePipeline();
	void DestroyComposePipeline();
	void CreateComposeDescriptorSet();
	void UpdateComposeDescriptorSet();

	bool m_Initialized = false;

	uint32_t m_Width = 0;
	uint32_t m_Height = 0;

	VkImage m_OutputImage = VK_NULL_HANDLE;
	VkDeviceMemory m_OutputMemory = VK_NULL_HANDLE;
	VkImageView m_OutputImageView = VK_NULL_HANDLE;
	VkSampler m_Sampler = VK_NULL_HANDLE;
	VkDescriptorSet m_ImGuiDescriptorSet = VK_NULL_HANDLE;

	// Ray tracing pipeline + SBT
	VkPipeline m_RTPipeline = VK_NULL_HANDLE;
	VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_DescriptorSetLayout = VK_NULL_HANDLE;
	VkDescriptorSet m_DescriptorSet = VK_NULL_HANDLE;

	VkShaderModule m_RgenShader   = VK_NULL_HANDLE;
	VkShaderModule m_MissShader    = VK_NULL_HANDLE;
	VkShaderModule m_ShadowShader  = VK_NULL_HANDLE;
	VkShaderModule m_ClosestShader = VK_NULL_HANDLE;
	VkShaderModule m_AnyHitShader  = VK_NULL_HANDLE;
	VkShaderModule m_ShadowHitShader = VK_NULL_HANDLE;

	VkBuffer m_SBTBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_SBTMemory = VK_NULL_HANDLE;
	VkDeviceSize m_SBTSize = 0;
	VkDeviceSize m_SBTStride = 0;
	VkDeviceSize m_RgenRegionSize = 0;
	VkDeviceSize m_MissRegionSize = 0;  // covers both miss entries (sky + shadow)
	VkDeviceSize m_HitRegionSize = 0;
	uint32_t m_MaxRecursionDepth = 1;

	VkBuffer m_CameraUBO = VK_NULL_HANDLE;
	VkDeviceMemory m_CameraUBOMemory = VK_NULL_HANDLE;

	VkBuffer m_MaterialBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_MaterialBufferMemory = VK_NULL_HANDLE;
	VkDeviceSize m_MaterialBufferSize = 0;

	// Light buffer (NEE) — std430 with 16-byte header + TriangleLight[]
	VkBuffer m_LightBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_LightBufferMemory = VK_NULL_HANDLE;
	VkDeviceSize m_LightBufferSize = 0;

	// Textures (bindless array)
	std::vector<GPUTexture> m_Textures;
	VkDescriptorSetLayout m_TextureDescriptorSetLayout = VK_NULL_HANDLE;
	VkDescriptorSet m_TextureDescriptorSet = VK_NULL_HANDLE;

	// Environment map CDF textures (M8) — appended to m_Textures array as extra entries
	int m_MarginalCDFIndex = -1;    // index into m_Textures for marginal CDF
	int m_ConditionalCDFIndex = -1; // index into m_Textures for conditional CDF
	int m_EnvMapIndex = -1;
	int m_CDFWidth = 0;
	int m_CDFHeight = 0;

	AccelerationStructure m_AS;

	GPUSceneData m_CurrentScene;
	bool m_NeedsASRebuild = false;
	bool m_ASJustBuilt = false;

	uint32_t m_FrameIndex = 1;
	VkImageLayout m_OutputImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	// NRD G-buffer images (set 1, bindings 0-4)
	VkImage m_GNormalRoughness = VK_NULL_HANDLE;
	VkDeviceMemory m_GNormalRoughnessMem = VK_NULL_HANDLE;
	VkImageView m_GNormalRoughnessView = VK_NULL_HANDLE;

	VkImage m_GViewZ = VK_NULL_HANDLE;
	VkDeviceMemory m_GViewZMem = VK_NULL_HANDLE;
	VkImageView m_GViewZView = VK_NULL_HANDLE;

	VkImage m_GMotion = VK_NULL_HANDLE;
	VkDeviceMemory m_GMotionMem = VK_NULL_HANDLE;
	VkImageView m_GMotionView = VK_NULL_HANDLE;

	VkImage m_GDiffRadiance = VK_NULL_HANDLE;
	VkDeviceMemory m_GDiffRadianceMem = VK_NULL_HANDLE;
	VkImageView m_GDiffRadianceView = VK_NULL_HANDLE;

	VkImage m_GSpecRadiance = VK_NULL_HANDLE;
	VkDeviceMemory m_GSpecRadianceMem = VK_NULL_HANDLE;
	VkImageView m_GSpecRadianceView = VK_NULL_HANDLE;

	// Albedo + F0 for compose pass remodulation (set 1 binding 6, RT writes, compose reads)
	VkImage m_GAlbedoF0 = VK_NULL_HANDLE;
	VkDeviceMemory m_GAlbedoF0Mem = VK_NULL_HANDLE;
	VkImageView m_GAlbedoF0View = VK_NULL_HANDLE;

	// Direct emission (emissive surfaces + sky) — bypasses NRD, added in compose
	VkImage m_GDirectEmission = VK_NULL_HANDLE;
	VkDeviceMemory m_GDirectEmissionMem = VK_NULL_HANDLE;
	VkImageView m_GDirectEmissionView = VK_NULL_HANDLE;

	// NRD output images (denoised)
	VkImage m_NRDDiffOut = VK_NULL_HANDLE;
	VkDeviceMemory m_NRDDiffOutMem = VK_NULL_HANDLE;
	VkImageView m_NRDDiffOutView = VK_NULL_HANDLE;

	VkImage m_NRDSpecOut = VK_NULL_HANDLE;
	VkDeviceMemory m_NRDSpecOutMem = VK_NULL_HANDLE;
	VkImageView m_NRDSpecOutView = VK_NULL_HANDLE;

	// NRD UBO (set 1, binding 5)
	VkBuffer m_NRDUBO = VK_NULL_HANDLE;
	VkDeviceMemory m_NRDUBOMemory = VK_NULL_HANDLE;

	// Set 1 descriptor set layout + set
	VkDescriptorSetLayout m_GBufferSetLayout = VK_NULL_HANDLE;
	VkDescriptorSet m_GBufferSet = VK_NULL_HANDLE;
	VkDescriptorPool m_GBufferPool = VK_NULL_HANDLE;

	// Compose pass (compute shader: NRD outputs + albedo/F0 -> beauty)
	VkPipeline m_ComposePipeline = VK_NULL_HANDLE;
	VkPipelineLayout m_ComposePipelineLayout = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_ComposeSetLayout = VK_NULL_HANDLE;
	VkDescriptorSet m_ComposeSet = VK_NULL_HANDLE;
	VkDescriptorPool m_ComposePool = VK_NULL_HANDLE;
	VkShaderModule m_ComposeShader = VK_NULL_HANDLE;

	// NRD integration wrapper
	NRDWrapper m_NRD;

	// Previous frame matrices for motion vectors
	glm::mat4 m_PrevViewToClip = glm::mat4(1.0f);
	glm::mat4 m_PrevWorldToView = glm::mat4(1.0f);
	bool m_HasPrevMatrices = false;

	void CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
	                  VkBuffer& buffer, VkDeviceMemory& memory);
	void DestroyBuffer(VkBuffer& buffer, VkDeviceMemory& memory);
	VkDeviceAddress GetBufferDeviceAddress(VkBuffer buffer);
	uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
};

#endif // !RENDERER_GPU_H