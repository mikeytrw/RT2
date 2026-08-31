#include "RRGuidePass.h"

#include "GpuDevice.h"
#include "RTLog.h"
#include "ShaderManager.h"
#include "VulkanUtils.h"

bool RRGuidePass::Init(const GpuDevice& device, VkDescriptorSetLayout sceneSetLayout,
	VkDescriptorSetLayout gbufferSetLayout)
{
	m_Device = device.device;
	VkShaderModule shader = ShaderManager::LoadShader("rr_guides.spv");
	if (!shader) { RT_LOG("[RRGuides] shader load failed"); return false; }
	VkDescriptorSetLayout sets[] = { sceneSetLayout, gbufferSetLayout };
	VkPipelineLayoutCreateInfo layoutInfo{};
	layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layoutInfo.setLayoutCount = 2;
	layoutInfo.pSetLayouts = sets;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &layoutInfo, nullptr, &m_PipelineLayout));
	VkPipelineShaderStageCreateInfo stage{};
	stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	stage.module = shader;
	stage.pName = "main";
	VkComputePipelineCreateInfo pipelineInfo{};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipelineInfo.stage = stage;
	pipelineInfo.layout = m_PipelineLayout;
	VkResult result = vkCreateComputePipelines(m_Device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_Pipeline);
	vkDestroyShaderModule(m_Device, shader, nullptr);
	if (result != VK_SUCCESS)
	{
		RT_LOG("[RRGuides] vkCreateComputePipelines failed: %d", (int)result);
		return false;
	}
	return true;
}

void RRGuidePass::Destroy()
{
	if (m_Pipeline) { vkDestroyPipeline(m_Device, m_Pipeline, nullptr); m_Pipeline = VK_NULL_HANDLE; }
	if (m_PipelineLayout) { vkDestroyPipelineLayout(m_Device, m_PipelineLayout, nullptr); m_PipelineLayout = VK_NULL_HANDLE; }
}

void RRGuidePass::Record(VkCommandBuffer cmd, const RenderExtent& extent,
	VkDescriptorSet sceneSet, VkDescriptorSet gbufferSet) const
{
	if (!m_Pipeline || !extent.IsValid()) return;
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_PipelineLayout, 0, 1, &sceneSet, 0, nullptr);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_PipelineLayout, 1, 1, &gbufferSet, 0, nullptr);
	vkCmdDispatch(cmd, (extent.Width() + 15u) / 16u, (extent.Height() + 15u) / 16u, 1);
}
