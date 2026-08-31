#pragma once

#include "RenderExtents.h"
#include "vulkan/vulkan.h"

struct GpuDevice;

class RRGuidePass
{
public:
	~RRGuidePass() { Destroy(); }
	bool Init(const GpuDevice& device, VkDescriptorSetLayout sceneSetLayout,
		VkDescriptorSetLayout gbufferSetLayout);
	void Destroy();
	void Record(VkCommandBuffer cmd, const RenderExtent& extent,
		VkDescriptorSet sceneSet, VkDescriptorSet gbufferSet) const;
	bool IsAvailable() const { return m_Pipeline != VK_NULL_HANDLE; }

private:
	VkDevice m_Device = VK_NULL_HANDLE;
	VkPipeline m_Pipeline = VK_NULL_HANDLE;
	VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
};
