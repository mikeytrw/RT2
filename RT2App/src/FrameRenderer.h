#pragma once

#include "vulkan/vulkan.h"
#include "GpuDevice.h"
#include "GBufferTarget.h"
#include "SceneResources.h"
#include "PathTracePass.h"
#include "RasterPass.h"
#include "GBufferDebugPass.h"
#include "ComposePass.h"
#include "ReSTIRPass.h"
#include "ReservoirResources.h"
#include "NRDIntegration.h"
#include "FrameContext.h"
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
		GBufferTarget& gbuffer;
		SceneResources& scene;
		PathTracePass& pathTracePass;
		RasterPass& rasterPass;
		GBufferDebugPass& gbufferDebugPass;
		ComposePass& composePass;
		ReSTIRPass& restirPass;
		ReservoirResources& reservoirs;
		NRDWrapper& nrd;

		GpuImage& outputImage;
		VkDescriptorSet gbufferSet;
		VkBuffer cameraUBO;
		VkBuffer nrdUBO;
		const SICameraData& cameraUBOData;

		uint32_t width;
		uint32_t height;

		// Render mode flags
		bool rasterFirst;
		bool nrdEnabled;
		int  lobeDither;  // 0=off, 1=Bayer, 2=IGN
		bool restirEnabled;
		SIReSTIRPushConstants restirPC;
		int gbufferDebugMode;

		// NRD settings
		float nrdMaxBlurRadius;
		int nrdMaxAccumFrames;
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
	static void RecordReSTIRPass(VkCommandBuffer cmd, Context& ctx);
	static void RecordPathTraceOrDebug(VkCommandBuffer cmd, Context& ctx);
	static void RecordNRDAndCompose(VkCommandBuffer cmd, Context& ctx);
	static void RecordOutputTransition(VkCommandBuffer cmd, Context& ctx);
};