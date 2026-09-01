#pragma once

#include "vulkan/vulkan.h"
#include "GpuDevice.h"
#include "GBufferTarget.h"
#include "SceneResources.h"
#include "PathTracePass.h"
#include "RasterPass.h"
#include "GBufferDebugPass.h"
#include "RRGuidePass.h"
#include "RRGuideResources.h"
#include "ComposePass.h"
#include "TonemapPass.h"
#include "ReSTIRPass.h"
#include "ReSTIRGIPass.h"
#include "ReservoirResources.h"
#include "ReservoirGIResources.h"
#include "NRDIntegration.h"
#include "FrameContext.h"
#include "GpuTimestampProfiler.h"
#include "RenderExtents.h"
#include "shader_interface.h"
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <functional>

class Camera;

// FrameRenderer — orchestrates per-frame command buffer recording.
// Takes a FrameRenderContext with references to all resources needed.
// The pre-frame setup (AS rebuild, NRD init, camera UBO update) stays
// in RendererGPU. FrameRenderer handles the command buffer recording:
// barriers, raster pass, RT dispatch, NRD denoise, compose pass.
class FrameRenderer
{
public:
	// Context struct — passed to RecordFrame each frame.
	struct Context
	{
		const GpuDevice& device;
		GpuTimestampProfiler* gpuProfiler;
		GBufferTarget& gbuffer;
		SceneResources& scene;
		PathTracePass& pathTracePass;
		RasterPass& rasterPass;
		GBufferDebugPass& gbufferDebugPass;
		RRGuidePass& rrGuidePass;
		RRGuideResources& rrGuides;
		ComposePass& composePass;
		TonemapPass& tonemapPass;
		ReSTIRPass& restirPass;
		ReservoirResources& reservoirs;
		NRDWrapper& nrd;

		GpuImage& outputImage;
		GpuImage& displayImage;
		VkDescriptorSet gbufferSet;
		VkBuffer cameraUBO;
		VkBuffer nrdUBO;
		const SICameraData& cameraUBOData;

		RenderExtent renderExtent;
		OutputExtent outputExtent;

		// Render mode flags
		bool rasterFirst;
		bool rrGuideReportMode;
		bool nrdEnabled;
		int  lobeDither;  // 0=off, 1=Bayer, 2=IGN
		bool restirEnabled;
		SIReSTIRPushConstants restirPC;
		int gbufferDebugMode;

		// NRD settings
		float nrdMaxBlurRadius;
		int nrdMaxAccumFrames;
		float nrdResponsiveRoughnessThreshold;
		int nrdResponsiveMinAccumFrames;
		bool nrdAntiFirefly;
		float nrdSplitScreen;
		glm::vec2 nrdJitter;
		glm::vec2 nrdJitterPrev;
		uint32_t nrdFrameIndex;
		bool& nrdNeedsReset;  // mutable — set to false after NRD dispatch

		// Previous matrices (for NRD reprojection)
		bool hasPrevMatrices;
		glm::mat4 prevViewToClip;
		glm::mat4 prevWorldToView;

		// Compose descriptor caching
		bool& composeDescriptorSetCached;  // mutable — set to true after first update

		// Camera (for NRD matrix settings)
		const Camera& camera;

		// ReSTIR GI (one-bounce diffuse GI, temporal reuse, raster-first only).
		// Independent of DI — requires raster-first G-buffer but not DI enabled.
		ReSTIRGIPass& restirGIPass;
		ReservoirGIResources& giReservoirs;
		bool restirGIEnabled;
		SIGIPushConstants restirGIPC;
		uint32_t giFrameIndex;        // drives reservoir/receiver-history parity
		uint32_t giReservoirIndex;    // current region index read by raygen (frame parity)
	};

	// Record the full frame into the given command buffer.
	// Handles: top-of-frame barrier, UBO updates, raster G-buffer pass,
	// RT dispatch (or G-buffer debug), NRD denoise, compose pass,
	// output image transition.
	static void RecordFrame(VkCommandBuffer cmd, Context& ctx);

private:
	static void RecordTopBarrier(VkCommandBuffer cmd, Context& ctx);
	static void RecordASBarrier(VkCommandBuffer cmd, Context& ctx);
	static void RecordUBOUpdates(VkCommandBuffer cmd, Context& ctx);
	static void RecordRasterPass(VkCommandBuffer cmd, Context& ctx);
	static void RecordRRGuidePass(VkCommandBuffer cmd, Context& ctx);
	static void RecordReSTIRPass(VkCommandBuffer cmd, Context& ctx);
	static void RecordReSTIRGIPass(VkCommandBuffer cmd, Context& ctx);
	static void RecordPathTraceOrDebug(VkCommandBuffer cmd, Context& ctx);
	static void RecordNRDAndCompose(VkCommandBuffer cmd, Context& ctx);
	static void RecordTonemapPass(VkCommandBuffer cmd, Context& ctx);
	static void RecordOutputTransition(VkCommandBuffer cmd, Context& ctx);
};
