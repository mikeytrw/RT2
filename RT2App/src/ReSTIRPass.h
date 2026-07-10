#pragma once

#include "vulkan/vulkan.h"
#include "shader_interface.h"
#include <cstdint>

struct GpuDevice;

// ReSTIRPass — compute pipelines for ReSTIR DI (temporal + spatial reuse).
//
// Two compute pipelines:
//   1. Temporal: initial candidates + temporal reuse (history → scratch)
//   2. Spatial:  spatial neighbor reuse (scratch → history)
//
// Both reuse the existing set 0 + set 1 descriptor layouts from PathTracePass
// and GBufferTarget. No separate descriptor set needed — set 0 already declares
// all scene + reservoir bindings (14/15/16).
//
// Pipeline (raster-first mode only):
//   [Raster] → G-buffer
//   [Temporal] → scratch   ← this pass (reads history + surfaceHistory)
//   [Spatial]  → history   ← this pass (reads scratch)
//   [Shading]  reads history (PathTracePass secondary_raygen)
//
// Push constants: SIReSTIRPushConstants (32 bytes).
class ReSTIRPass
{
public:
	ReSTIRPass() = default;
	~ReSTIRPass() { Destroy(); }

	bool Init(const GpuDevice& dev,
	          VkDescriptorSetLayout set0Layout,
	          VkDescriptorSetLayout set1Layout);
	void Destroy();

	// Record the temporal dispatch (history → scratch).
	// G-buffer images must be in GENERAL layout with SHADER_READ access.
	void RecordTemporal(VkCommandBuffer cmd, uint32_t width, uint32_t height,
	                    VkDescriptorSet set0, VkDescriptorSet set1,
	                    const SIReSTIRPushConstants& pc) const;

	// Record the spatial dispatch (scratch → history).
	void RecordSpatial(VkCommandBuffer cmd, uint32_t width, uint32_t height,
	                   VkDescriptorSet set0, VkDescriptorSet set1,
	                   const SIReSTIRPushConstants& pc) const;

	// Record both temporal + spatial with proper barriers.
	void Record(VkCommandBuffer cmd, uint32_t width, uint32_t height,
	            VkDescriptorSet set0, VkDescriptorSet set1,
	            const SIReSTIRPushConstants& pc) const;

	bool IsAvailable() const { return m_TemporalPipeline != VK_NULL_HANDLE; }

private:
	VkDevice m_Device = VK_NULL_HANDLE;

	VkPipeline m_TemporalPipeline = VK_NULL_HANDLE;
	VkPipeline m_SpatialPipeline = VK_NULL_HANDLE;
	VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;

	VkShaderModule m_TemporalShader = VK_NULL_HANDLE;
	VkShaderModule m_SpatialShader = VK_NULL_HANDLE;
};