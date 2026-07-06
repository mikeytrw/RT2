#pragma once

#ifndef RENDERER_GPU_H
#define RENDERER_GPU_H

#include "vulkan/vulkan.h"
#include "AccelerationStructure.h"
#include "Camera.h"
#include "GPUSceneData.h"
#include "GpuDevice.h"
#include "ComposePass.h"
#include "PathTracePass.h"
#include "NRDIntegration.h"
#include "GpuResources.h"
#include "FrameContext.h"
#include "Scene.h"
#include <array>
#include <memory>

class RendererGPU
{
public:
	static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;
	RendererGPU() = default;
	~RendererGPU() { Destroy(); }

	void Destroy();

	bool IsAvailable() const { return m_Initialized; }

	void OnResize(uint32_t width, uint32_t height);
	void Render(const Camera& camera);
	void SetScene(const GPUSceneData& sceneData);

	// Update instance transforms + lights + TLAS only (no BLAS rebuild).
	// Call after ECS transforms have changed (e.g. animation).
	void UpdateSceneInstances(const GPUSceneData& sceneData);

	VkDescriptorSet GetOutputDescriptorSet() const { return m_ImGuiDescriptorSet; }
	bool HasOutput() const { return m_OutputImage != VK_NULL_HANDLE; }
	uint32_t GetWidth() const { return m_Width; }
	uint32_t GetHeight() const { return m_Height; }

	bool Init();
	void RebuildAccelerationStructures();
	void ResetAccumulation();

	// Read back the output image to CPU as RGBA8 (tonemapped+sRGB). Returns false on failure.
	bool ReadbackOutput(std::vector<uint8_t>& outPixelsRGBA8, uint32_t& outWidth, uint32_t& outHeight);

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

	// Camera jitter for NRD temporal AA (Halton sequence)
	glm::vec2 m_NRDJitter = glm::vec2(0.0f);
	glm::vec2 m_NRDJitterPrev = glm::vec2(0.0f);

private:
	void CreateOutputImage();
	void DestroyOutputImage();
	void CreateMaterialBuffer();
	void CreateLightBuffer();
	void CreateInstanceTransformBuffer();
	void UpdateCameraUBO(const Camera& camera);
	void UpdatePathTraceDescriptorSet();
	void CreateTextures(const std::vector<SceneTexture>& textures);
	void DestroyTextures();
	void CreateEnvMapCDFTextures(const GPUSceneData& sceneData);
	void DestroyEnvMapCDFTextures();

	// NRD G-buffer images (set 1, bindings 0-4)
	void CreateGBufferImages();
	void DestroyGBufferImages();
	void CreateGBufferDescriptorSet();
	void UpdateGBufferDescriptorSet();

	bool m_Initialized = false;

	GpuDevice m_Device;

	uint32_t m_Width = 0;
	uint32_t m_Height = 0;

	VkImage m_OutputImage = VK_NULL_HANDLE;
	VkDeviceMemory m_OutputMemory = VK_NULL_HANDLE;
	VkImageView m_OutputImageView = VK_NULL_HANDLE;
	VkSampler m_Sampler = VK_NULL_HANDLE;
	VkDescriptorSet m_ImGuiDescriptorSet = VK_NULL_HANDLE;

	// Ray tracing pipeline + SBT (owned by PathTracePass)
	PathTracePass m_PathTracePass;

	VkBuffer m_CameraUBO = VK_NULL_HANDLE;
	VkDeviceMemory m_CameraUBOMemory = VK_NULL_HANDLE;
	SICameraData m_CameraUBOData = {}; // stashed by UpdateCameraUBO, written via vkCmdUpdateBuffer in Render()

	VkBuffer m_MaterialBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_MaterialBufferMemory = VK_NULL_HANDLE;
	VkDeviceSize m_MaterialBufferSize = 0;

	// Light buffer (NEE) — std430 with 16-byte header + TriangleLight[]
	VkBuffer m_LightBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_LightBufferMemory = VK_NULL_HANDLE;
	VkDeviceSize m_LightBufferSize = 0;

	// Instance transform buffer — one mat4 per instance (object-to-world)
	VkBuffer m_InstanceTransformBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_InstanceTransformBufferMemory = VK_NULL_HANDLE;

	// Textures (bindless array) — GpuImage wrapper, shared samplers
	std::vector<GpuImage> m_Textures;
	VkSampler m_TextureSampler = VK_NULL_HANDLE;      // REPEAT + LINEAR mipmap (scene textures)
	VkSampler m_CDFTextureSampler = VK_NULL_HANDLE;   // CLAMP_TO_EDGE + NEAREST mipmap (CDF textures)
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
	ComposePass m_ComposePass;

	// NRD integration wrapper
	NRDWrapper m_NRD;

	// Previous frame matrices for motion vectors
	glm::mat4 m_PrevViewToClip = glm::mat4(1.0f);
	glm::mat4 m_PrevWorldToView = glm::mat4(1.0f);
	bool m_HasPrevMatrices = false;

	// Frames in flight ring
	std::array<FrameContext, MAX_FRAMES_IN_FLIGHT> m_Frames;
	uint32_t m_CurrentFrame = 0;
};

#endif // !RENDERER_GPU_H