#pragma once

#ifndef RENDERER_GPU_H
#define RENDERER_GPU_H

#include "vulkan/vulkan.h"
#include "AccelerationStructure.h"
#include "Camera.h"
#include "GPUSceneData.h"
#include "GpuDevice.h"
#include "ComposePass.h"
#include "TonemapPass.h"
#include "FrameRenderer.h"
#include "GBufferTarget.h"
#include "PathTracePass.h"
#include "RasterPass.h"
#include "GBufferDebugPass.h"
#include "NRDIntegration.h"
#include "RenderSettings.h"
#include "SceneResources.h"
#include "ReservoirResources.h"
#include "ReservoirGIResources.h"
#include "ReSTIRPass.h"
#include "ReSTIRGIPass.h"
#include "GpuResources.h"
#include "FrameContext.h"
#include "GpuTimestampProfiler.h"
#include "RenderInstanceMap.h"
#include "GpuPickingPass.h"
#include <array>
#include <memory>
#include <optional>

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
	void SetScene(GPUSceneData& sceneData, const RenderInstanceMap& instanceMap = {});

	// Update scene data WITHOUT re-uploading textures. Use when only
	// entities/transforms/materials changed (add/delete entity, material edit).
	void SetSceneKeepTextures(const GPUSceneData& sceneData, const RenderInstanceMap& instanceMap = {});

	// Async texture upload polling — forward to SceneResources.
	bool IsTextureUploadPending() const { return m_Scene.IsTextureUploadPending(); }
	// Adopts completed textures AND refreshes the path-trace descriptor set,
	// mirroring what Render() does inline. The loading modal drives this
	// outside Render(), so without the descriptor refresh a texture-only
	// upload (e.g. an env map) would leave the renderer bound to stale images.
	bool PollTextureUpload();

	// AS rebuild status + trigger (used by the async loading modal to
	// run the BLAS/TLAS build outside Render() so the modal stays visible).
	bool NeedsASRebuild() const { return m_Scene.NeedsASRebuild(); }
	void RebuildAccelerationStructures();

	// Async AS rebuild — submit with a fence, poll each frame.
	bool BeginRebuildAccelerationStructures();
	bool IsASRebuildPending() const { return m_Scene.IsASRebuildPending(); }
	bool PollASRebuild() { return m_Scene.PollASRebuild(); }

	// Update the path-trace descriptor set after an async AS rebuild
	// completes (called by the loading modal).
	void UpdateDescriptorSetAfterAS();

	// Update instance transforms + lights + TLAS only (no BLAS rebuild).
	// Call after ECS transforms have changed (e.g. animation).
	void UpdateSceneInstances(const GPUSceneData& sceneData, const RenderInstanceMap& instanceMap = {});

	VkDescriptorSet GetOutputDescriptorSet() const { return m_ImGuiDescriptorSet; }
	bool HasOutput() const { return m_OutputImage.IsValid() && m_DisplayImage.IsValid(); }
	uint32_t GetWidth() const { return m_Width; }
	uint32_t GetHeight() const { return m_Height; }

	struct PickResult
	{
		uint64_t serial = 0;
		bool hit = false;
		rt2::core::UUID entityUuid = rt2::core::UUID::Nil();
		glm::vec3 worldPosition{ 0.0f };
	};

	uint64_t RequestPick(const CameraRay& ray, float maxDistance = 100000.0f);
	std::optional<PickResult> ConsumePickResult();
	void CancelPicks();

	bool Init();
	void ResetAccumulation();

	// ReSTIR history invalidation — call whenever the receiver, light
	// distribution, reservoir normalization, or surface correspondence
	// changes. Sets the clear flag and increments the history version.
	void InvalidateReSTIRHistory();

	// ReSTIR GI history invalidation — call when the GI sample domain
	// changes (scene/light-list/env/resize/material/camera-mode change or
	// GI setting change). Transform-only updates do NOT call this —
	// rigid motion is handled by reprojection + re-evaluation.
	// Sets the clear flag, resets the GI frame index (parity), and
	// requests an NRD reset.
	void InvalidateGIHistory();

	// Read back the output image to CPU as RGBA8 (tonemapped+sRGB). Returns false on failure.
	bool ReadbackOutput(std::vector<uint8_t>& outPixelsRGBA8, uint32_t& outWidth, uint32_t& outHeight);

	// Read back the output image to CPU as RGBA32F linear HDR. Returns false on failure.
	bool ReadbackOutputLinear(std::vector<float>& outPixelsRGBA32F, uint32_t& outWidth, uint32_t& outHeight);

	// Render settings — the only writable configuration surface.
	// Mutate a copy, then call ApplySettings() to detect changes and
	// auto-trigger ResetAccumulation on the next Render().
	RenderSettings GetSettings() const { return m_Settings; }
	void ApplySettings(const RenderSettings& newSettings);

	// Camera jitter for NRD temporal AA (Halton sequence) — internal,
	// computed each frame from settings.nrdJitterEnabled + nrdJitterScale.
	glm::vec2 GetNRDJitter() const { return m_NRDJitter; }
	glm::vec2 GetNRDJitterPrev() const { return m_NRDJitterPrev; }
	const GpuTimestampProfiler::Timings& GetGpuTimings() const { return m_GpuProfiler.GetLatest(); }
	bool HasGpuTimings() const { return m_GpuProfiler.IsAvailable(); }

	// Last BLAS/TLAS build timings (milliseconds, CPU wall-clock around the
	// build/record calls). -1.0 means no build has run yet. Forwarded from
	// SceneResources for the Performance window's level-3 view.
	float GetLastBlasBuildMs() const { return m_Scene.GetLastBlasBuildMs(); }
	float GetLastTlasBuildMs()  const { return m_Scene.GetLastTlasBuildMs(); }
	float GetLastAsTotalMs()    const { return m_Scene.GetLastAsTotalMs(); }
	uint32_t GetBlasCount() const { return m_Scene.GetBlasCount(); }
	// Wait for submitted frames and collect the newest timestamp slot. Intended
	// for headless benchmarks and explicit capture points, not the live loop.
	void FlushGpuTimings();

	// Debug: dump GPU instance transform buffer contents to log.
	void DumpInstanceTransforms() const;
	void DumpNEEBuffers() const;

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

	void CreateFallbackTexture();

	bool m_Initialized = false;

	GpuDevice m_Device;

	uint32_t m_Width = 0;
	uint32_t m_Height = 0;

	GpuImage m_OutputImage;  // RGBA32F linear beauty + accumulation history
	GpuImage m_DisplayImage; // RGBA8 Reinhard-tonemapped viewport image
	GpuImage m_FallbackTexture; // 1x1 white, used for missing texture views
	VkSampler m_Sampler = VK_NULL_HANDLE;
	VkDescriptorSet m_ImGuiDescriptorSet = VK_NULL_HANDLE;

	// Ray tracing pipeline + SBT (owned by PathTracePass)
	PathTracePass m_PathTracePass;

	// Render settings (user-tunable knobs)
	RenderSettings m_Settings;

	// NRD jitter state (computed each frame from m_Settings)
	glm::vec2 m_NRDJitter = glm::vec2(0.0f);
	glm::vec2 m_NRDJitterPrev = glm::vec2(0.0f);

	VkBuffer m_CameraUBO = VK_NULL_HANDLE;
	VkDeviceMemory m_CameraUBOMemory = VK_NULL_HANDLE;
	SICameraData m_CameraUBOData = {}; // stashed by UpdateCameraUBO, written via vkCmdUpdateBuffer in Render()

	// Scene resources — materials, lights, transforms, textures, AS
	SceneResources m_Scene;

	uint32_t m_FrameIndex = 1; // non-NRD temporal accumulation frame counter (resets on camera move)
	uint32_t m_NRDFrameIndex = 1; // NRD frame counter (continuously increments, resets only on explicit ResetAccumulation)
	bool m_NRDNeedsReset = true; // triggers NRD CLEAR_AND_RESTART on next frame (set on init/scene change/reset)
	glm::vec3 m_PrevCameraPos = glm::vec3(0.0f);
	glm::vec3 m_PrevCameraForward = glm::vec3(0.0f, 0.0f, -1.0f);
	bool m_HasPrevCamera = false;
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
	TonemapPass m_TonemapPass;
	bool m_ComposeDescriptorSetCached = false;

	// ReSTIR DI (Reservoir-based Resampling for Direct Illumination) pass + resources
	ReSTIRPass m_ReSTIRPass;
	ReservoirResources m_Reservoirs;
	bool m_ReSTIRHistoryInvalidated = true;  // set on resize/scene change/enable toggle
	uint32_t m_ReSTIRFrameIndex = 1;         // independent of m_FrameIndex — monotonically increasing
	uint32_t m_ReSTIRHistoryVersion = 0;     // incremented on each InvalidateReSTIRHistory()

	// ReSTIR GI (one-bounce diffuse GI, temporal reuse, raster-first only).
	// Independent of DI — requires raster-first G-buffer but not DI enabled.
	// m_GIFrameIndex drives reservoir/receiver-history parity (giCurrentRegion).
	// Incremented only after a submitted frame (end of Render()).
	ReSTIRGIPass m_ReSTIRGIPass;
	ReservoirGIResources m_GIReservoirs;
	bool m_GIHistoryInvalidated = true;   // set on resize/scene/setting change
	uint32_t m_GIFrameIndex = 1;          // parity driver; reset to 1 on invalidation

	// Raster pass (primary visibility G-buffer)
	RasterPass m_RasterPass;
	GBufferDebugPass m_GBufferDebugPass;

	// NRD integration wrapper
	NRDWrapper m_NRD;

	// Previous frame matrices for motion vectors
	glm::mat4 m_PrevViewToClip = glm::mat4(1.0f);
	glm::mat4 m_PrevWorldToView = glm::mat4(1.0f);
	bool m_HasPrevMatrices = false;

	// Prev matrices captured for the current frame (before UpdateCameraUBO
	// overwrites m_Prev* with current). Passed to FrameRenderer for NRD.
	glm::mat4 m_PrevViewToClipForFrame = glm::mat4(1.0f);
	glm::mat4 m_PrevWorldToViewForFrame = glm::mat4(1.0f);

	// Frames in flight ring
	std::array<FrameContext, MAX_FRAMES_IN_FLIGHT> m_Frames;
	uint32_t m_CurrentFrame = 0;
	RenderInstanceMap m_RenderInstanceMap;
	GpuPickingPass m_PickingPass;
	struct PendingPick
	{
		uint64_t serial = 0;
		CameraRay ray;
		float maxDistance = 100000.0f;
	};
	std::optional<PendingPick> m_PendingPick;
	std::optional<PickResult> m_CompletedPick;
	uint64_t m_LatestPickSerial = 0;
	GpuTimestampProfiler m_GpuProfiler;
};

#endif // !RENDERER_GPU_H
