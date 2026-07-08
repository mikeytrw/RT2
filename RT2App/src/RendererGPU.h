#pragma once

#ifndef RENDERER_GPU_H
#define RENDERER_GPU_H

#include "vulkan/vulkan.h"
#include "AccelerationStructure.h"
#include "Camera.h"
#include "GPUSceneData.h"
#include "GpuDevice.h"
#include "ComposePass.h"
#include "FrameRenderer.h"
#include "GBufferTarget.h"
#include "PathTracePass.h"
#include "RasterPass.h"
#include "GBufferDebugPass.h"
#include "NRDIntegration.h"
#include "SceneResources.h"
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
	bool HasOutput() const { return m_OutputImage.IsValid(); }
	uint32_t GetWidth() const { return m_Width; }
	uint32_t GetHeight() const { return m_Height; }

	bool Init();
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
	bool m_RasterFirst = false; // raster-first path (raster primary + RT secondary)

	// NRD tunable settings (exposed in UI)
	float m_NRDMaxBlurRadius = 30.0f;
	int m_NRDMaxAccumFrames = 30;
	bool m_NRDAntiFirefly = true;
	float m_NRDSplitScreen = 0.0f;
	bool m_NRDJitterEnabled = true; // toggle camera jitter (for debugging NRD vs noise)
	float m_NRDJitterScale = 1.0f; // jitter magnitude scale [0.0 = none, 1.0 = full ±0.5 pixel]

	int m_GBufferDebugMode = -1; // -1 = off, 0-10 = G-buffer view modes

	// Camera jitter for NRD temporal AA (Halton sequence)
	glm::vec2 m_NRDJitter = glm::vec2(0.0f);
	glm::vec2 m_NRDJitterPrev = glm::vec2(0.0f);

private:
	void CreateOutputImage();
	void DestroyOutputImage();
	void UpdateCameraUBO(const Camera& camera);
	void UpdatePathTraceDescriptorSet();

	// G-buffer images + descriptor set
	void CreateGBufferImages();
	void DestroyGBufferImages();
	void CreateGBufferDescriptorSet();
	void UpdateGBufferDescriptorSet();

	bool m_Initialized = false;

	GpuDevice m_Device;

	uint32_t m_Width = 0;
	uint32_t m_Height = 0;

	GpuImage m_OutputImage; // RGBA32F beauty output
	VkSampler m_Sampler = VK_NULL_HANDLE;
	VkDescriptorSet m_ImGuiDescriptorSet = VK_NULL_HANDLE;

	// Ray tracing pipeline + SBT (owned by PathTracePass)
	PathTracePass m_PathTracePass;

	VkBuffer m_CameraUBO = VK_NULL_HANDLE;
	VkDeviceMemory m_CameraUBOMemory = VK_NULL_HANDLE;
	SICameraData m_CameraUBOData = {}; // stashed by UpdateCameraUBO, written via vkCmdUpdateBuffer in Render()

	// Scene resources — materials, lights, transforms, textures, AS
	SceneResources m_Scene;

	uint32_t m_FrameIndex = 1; // non-NRD temporal accumulation frame counter (resets on camera move)
	uint32_t m_NRDFrameIndex = 1; // NRD frame counter (continuously increments, resets only on explicit ResetAccumulation)
	bool m_NRDNeedsReset = true; // triggers NRD CLEAR_AND_RESTART on next frame (set on init/scene change/reset)
	VkImageLayout m_OutputImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	// G-buffer target — owns all G-buffer color images + depth image
	GBufferTarget m_GBuffer;

	// NRD UBO (set 1, binding 5)
	VkBuffer m_NRDUBO = VK_NULL_HANDLE;
	VkDeviceMemory m_NRDUBOMemory = VK_NULL_HANDLE;

	// Set 1 descriptor set layout + set
	VkDescriptorSetLayout m_GBufferSetLayout = VK_NULL_HANDLE;
	VkDescriptorSet m_GBufferSet = VK_NULL_HANDLE;
	VkDescriptorPool m_GBufferPool = VK_NULL_HANDLE;

	// Compose pass (compute shader: NRD outputs + albedo/F0 -> beauty)
	ComposePass m_ComposePass;
	bool m_ComposeDescriptorSetCached = false;

	// Raster pass (primary visibility G-buffer)
	RasterPass m_RasterPass;
	GBufferDebugPass m_GBufferDebugPass;

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