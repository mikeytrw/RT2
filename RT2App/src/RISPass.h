#pragma once

#include "vulkan/vulkan.h"
#include <cstdint>

struct GpuDevice;

// RISPass — compute shader that generates per-pixel RIS reservoirs.
//
// Reads G-buffer (set 1) + scene resources (set 0, shared with PathTracePass)
// and writes reservoirBuffer (set 0, binding SI_BINDING_RESERVOIR).
//
// Pipeline (raster-first mode only):
//   [Raster] → G-buffer
//   [RIS]   → reservoirBuffer   ← this pass
//   [Shading] reads reservoirBuffer (PathTracePass secondary_raygen)
//
// Phase 1 (basic RIS): per-pixel streaming RIS with M candidates from the
// uniform light distribution. p_hat = unoccluded contribution in area measure.
// No shadow rays in this pass — visibility tested once in the shading pass.
//
// Descriptor sets: REUSES PathTracePass's set 0 descriptor set + the G-buffer
// set 1 descriptor set. No separate descriptor set needed — set 0 already
// declares all scene bindings + reservoir bindings (14/15). The RIS compute
// shader references a subset of these (materials, lights, transforms, G-buffer
// images, reservoir). The TLAS binding (4) is in the layout but unused by
// compute — valid in Vulkan (compute just doesn't reference it).
//
// Future (RIS.4/5): temporal reuse (reads reservoirBufferPrev), spatial reuse
// (writes reservoirBufferB). Those passes will share the same set 0/1 layout.
class RISPass
{
public:
	RISPass() = default;
	~RISPass() { Destroy(); }

	// Init the compute pipeline. Uses the existing set 0 + set 1 layouts
	// (passed in) so the pipeline layout matches PathTracePass's sets.
	bool Init(const GpuDevice& dev,
	          VkDescriptorSetLayout set0Layout,
	          VkDescriptorSetLayout set1Layout);
	void Destroy();

	// Record the RIS dispatch. Binds the provided set 0 + set 1 descriptor
	// sets (shared with PathTracePass). Caller handles pre-barrier
	// (raster→compute G-buffer) and post-barrier (compute→RT reservoir SSBO).
	// candidateCount = M (push constant, default 8).
	// No-op if pipeline not available.
	void Record(VkCommandBuffer cmd, uint32_t width, uint32_t height,
	            VkDescriptorSet set0, VkDescriptorSet set1,
	            uint32_t candidateCount) const;

	bool IsAvailable() const { return m_Pipeline != VK_NULL_HANDLE; }

private:
	VkDevice m_Device = VK_NULL_HANDLE;

	VkPipeline m_Pipeline = VK_NULL_HANDLE;
	VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;

	VkShaderModule m_Shader = VK_NULL_HANDLE;
};