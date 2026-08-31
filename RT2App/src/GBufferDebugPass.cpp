#include "GBufferDebugPass.h"
#include "RTLog.h"
#include "VulkanUtils.h"
#include "ShaderManager.h"

bool GBufferDebugPass::Init(const GpuDevice& dev, VkDescriptorSetLayout outputSetLayout,
                            VkDescriptorSetLayout gbufferSetLayout)
{
	m_Device = dev.device;

	VkShaderModule shader = ShaderManager::LoadShader("gbufferdebug.spv");
	if (!shader)
	{
		RT_LOG("[GBufferDebug] Failed to load shader");
		return false;
	}

	VkDescriptorSetLayout setLayouts[] = { outputSetLayout, gbufferSetLayout };

	VkPipelineLayoutCreateInfo layoutInfo = {};
	layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layoutInfo.setLayoutCount = 2;
	layoutInfo.pSetLayouts = setLayouts;

	VkPushConstantRange pushRange = {};
	pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pushRange.offset = 0;
	pushRange.size = sizeof(uint32_t);
	layoutInfo.pushConstantRangeCount = 1;
	layoutInfo.pPushConstantRanges = &pushRange;

	VK_CHECK(vkCreatePipelineLayout(m_Device, &layoutInfo, nullptr, &m_PipelineLayout));

	VkPipelineShaderStageCreateInfo stage = {};
	stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	stage.module = shader;
	stage.pName = "main";

	VkComputePipelineCreateInfo pipelineInfo = {};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipelineInfo.stage = stage;
	pipelineInfo.layout = m_PipelineLayout;

	VkResult err = vkCreateComputePipelines(m_Device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_Pipeline);
	vkDestroyShaderModule(m_Device, shader, nullptr);

	if (err != VK_SUCCESS)
	{
		RT_LOG("[GBufferDebug] vkCreateComputePipelines failed: %d", (int)err);
		return false;
	}

	RT_LOG("[GBufferDebug] initialized");
	return true;
}

void GBufferDebugPass::Destroy()
{
	if (m_Pipeline) { vkDestroyPipeline(m_Device, m_Pipeline, nullptr); m_Pipeline = VK_NULL_HANDLE; }
	if (m_PipelineLayout) { vkDestroyPipelineLayout(m_Device, m_PipelineLayout, nullptr); m_PipelineLayout = VK_NULL_HANDLE; }
}

void GBufferDebugPass::Record(VkCommandBuffer cmd, const RenderExtent& extent,
                              VkDescriptorSet outputSet, VkDescriptorSet gbufferSet,
                              uint32_t mode) const
{
	const uint32_t width = extent.Width(); const uint32_t height = extent.Height();
	if (!m_Pipeline) return;

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_PipelineLayout,
	                        0, 1, &outputSet, 0, nullptr);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_PipelineLayout,
	                        1, 1, &gbufferSet, 0, nullptr);
	vkCmdPushConstants(cmd, m_PipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
	                  sizeof(uint32_t), &mode);

	uint32_t groupsX = (width + 15) / 16;
	uint32_t groupsY = (height + 15) / 16;
	vkCmdDispatch(cmd, groupsX, groupsY, 1);
}
