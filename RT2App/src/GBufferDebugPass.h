#pragma once

#ifndef GBUFFER_DEBUG_PASS_H
#define GBUFFER_DEBUG_PASS_H

#include "vulkan/vulkan.h"
#include "GpuDevice.h"
#include "RenderExtents.h"

// GBufferDebugPass — compute shader that copies a G-buffer image to the
// output image for visual inspection. Uses the same set 0 (output image)
// and set 1 (G-buffer) descriptor sets as the path tracer.
class GBufferDebugPass
{
public:
	GBufferDebugPass() = default;
	~GBufferDebugPass() { Destroy(); }

	bool Init(const GpuDevice& dev, VkDescriptorSetLayout outputSetLayout,
	          VkDescriptorSetLayout gbufferSetLayout);
	void Destroy();

	void Record(VkCommandBuffer cmd, const RenderExtent& extent,
	            VkDescriptorSet outputSet, VkDescriptorSet gbufferSet,
	            uint32_t mode) const;

	bool IsAvailable() const { return m_Pipeline != VK_NULL_HANDLE; }

private:
	VkDevice m_Device = VK_NULL_HANDLE;
	VkPipeline m_Pipeline = VK_NULL_HANDLE;
	VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
};

#endif // !GBUFFER_DEBUG_PASS_H
