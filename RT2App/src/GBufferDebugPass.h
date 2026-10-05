#pragma once

#ifndef GBUFFER_DEBUG_PASS_H
#define GBUFFER_DEBUG_PASS_H

#include "vulkan/vulkan.h"
#include "GpuDevice.h"
#include "RenderExtents.h"

// GBufferDebugPass — compute shader that copies a G-buffer image to the
// output image for visual inspection. Uses the same set 0 (output image)
// and set 1 (G-buffer) descriptor sets as the path tracer.
//
// RR5-T2: mode kScatterContractMode (27) runs the sampling-weight
// contract battery (scatter_contract.spv) through the same sets/layout
// instead of the debug views. The battery invokes the consumed
// scatterPrimaryHit on matched materials and records exact numeric
// results into the output image for headless HDR capture.
class GBufferDebugPass
{
public:
	static constexpr uint32_t kScatterContractMode = 27;

	GBufferDebugPass() = default;
	~GBufferDebugPass() { Destroy(); }

	bool Init(const GpuDevice& dev, VkDescriptorSetLayout outputSetLayout,
	          VkDescriptorSetLayout gbufferSetLayout);
	void Destroy();

	void Record(VkCommandBuffer cmd, const RenderExtent& extent,
	            VkDescriptorSet outputSet, VkDescriptorSet gbufferSet,
	            uint32_t mode) const;

	bool IsAvailable() const { return m_Pipeline != VK_NULL_HANDLE; }
	bool IsContractAvailable() const { return m_ContractPipeline != VK_NULL_HANDLE; }

private:
	VkDevice m_Device = VK_NULL_HANDLE;
	VkPipeline m_Pipeline = VK_NULL_HANDLE;
	VkPipeline m_ContractPipeline = VK_NULL_HANDLE;
	VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
};

#endif // !GBUFFER_DEBUG_PASS_H
