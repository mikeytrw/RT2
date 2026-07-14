#pragma once

#include "vulkan/vulkan.h"
#include "shader_interface.h"
#include <cstdint>

struct GpuDevice;

// ReSTIRGIPass — compute pipelines for ReSTIR GI.
//
// Two compute pipelines, each with its own shader but sharing one pipeline
// layout (set 0 + set 1 + SIGIPushConstants):
//   1. Temporal: fresh candidates + temporal reuse (prev reservoir + prev
//      receiver history → current reservoir). Reads previous regions, writes
//      current reservoir only — never touches current receiver history.
//   2. History:  writes current receiver history from G-buffer data only
//      (no ray queries, no reads of other pixels' history). Runs AFTER the
//      temporal dispatch finishes to avoid a read/write race on the shared
//      receiver-history region.
//
// Uses the existing set 0 + set 1 descriptor layouts from PathTracePass and
// GBufferTarget. No separate descriptor set — set 0 already declares the GI
// buffer at binding 11.
//
// Push constants: SIGIPushConstants (48 bytes), separate from
// SIReSTIRPushConstants (DI and GI pipelines are independent).
class ReSTIRGIPass
{
public:
	ReSTIRGIPass() = default;
	~ReSTIRGIPass() { Destroy(); }

	bool Init(const GpuDevice& dev,
	          VkDescriptorSetLayout set0Layout,
	          VkDescriptorSetLayout set1Layout);
	void Destroy();

	// Record the temporal dispatch (prev reservoir + prev history → current reservoir).
	// G-buffer images must be in GENERAL layout with SHADER_READ access.
	void RecordTemporal(VkCommandBuffer cmd, uint32_t width, uint32_t height,
	                    VkDescriptorSet set0, VkDescriptorSet set1,
	                    const SIGIPushConstants& pc) const;

	// Record the history-write dispatch (G-buffer → current receiver history).
	// Must run AFTER RecordTemporal finishes (caller inserts barrier).
	void RecordHistoryWrite(VkCommandBuffer cmd, uint32_t width, uint32_t height,
	                         VkDescriptorSet set0, VkDescriptorSet set1,
	                         const SIGIPushConstants& pc) const;

	bool IsAvailable() const { return m_TemporalPipeline != VK_NULL_HANDLE; }

private:
	VkDevice m_Device = VK_NULL_HANDLE;

	VkPipeline       m_TemporalPipeline = VK_NULL_HANDLE;
	VkPipeline       m_HistoryPipeline  = VK_NULL_HANDLE;
	VkPipelineLayout m_PipelineLayout   = VK_NULL_HANDLE;

	VkShaderModule m_TemporalShader = VK_NULL_HANDLE;
	VkShaderModule m_HistoryShader  = VK_NULL_HANDLE;
};