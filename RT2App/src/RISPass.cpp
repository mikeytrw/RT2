#include "RISPass.h"
#include "GpuDevice.h"
#include "ShaderManager.h"
#include "VulkanUtils.h"
#include "RTLog.h"

bool RISPass::Init(const GpuDevice& dev,
                   VkDescriptorSetLayout set0Layout,
                   VkDescriptorSetLayout set1Layout)
{
	if (m_Pipeline != VK_NULL_HANDLE)
		return true;

	m_Device = dev.device;

	m_Shader = ShaderManager::LoadShader("ris.spv");
	if (!m_Shader)
		m_Shader = ShaderManager::LoadShader("RT2App/shaders/ris.spv");
	if (!m_Shader)
	{
		RT_LOG("[RISPass] Failed to load ris.spv");
		return false;
	}

	VkPipelineLayoutCreateInfo layoutInfo = {};
	layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

	VkDescriptorSetLayout setLayouts[2] = { set0Layout, set1Layout };
	layoutInfo.setLayoutCount = 2;
	layoutInfo.pSetLayouts = setLayouts;

	// Push constant: candidate count (M)
	VkPushConstantRange pcRange = {};
	pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pcRange.offset = 0;
	pcRange.size = sizeof(uint32_t);
	layoutInfo.pushConstantRangeCount = 1;
	layoutInfo.pPushConstantRanges = &pcRange;

	VK_CHECK(vkCreatePipelineLayout(m_Device, &layoutInfo, nullptr, &m_PipelineLayout));

	VkComputePipelineCreateInfo pipelineInfo = {};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	pipelineInfo.stage.module = m_Shader;
	pipelineInfo.stage.pName = "main";
	pipelineInfo.layout = m_PipelineLayout;

	VK_CHECK(vkCreateComputePipelines(m_Device, VK_NULL_HANDLE, 1,
	                                  &pipelineInfo, nullptr, &m_Pipeline));

	RT_LOG("[RISPass] initialized");
	return true;
}

void RISPass::Destroy()
{
	if (m_Pipeline)       { vkDestroyPipeline(m_Device, m_Pipeline, nullptr);       m_Pipeline = VK_NULL_HANDLE; }
	if (m_PipelineLayout) { vkDestroyPipelineLayout(m_Device, m_PipelineLayout, nullptr); m_PipelineLayout = VK_NULL_HANDLE; }
	if (m_Shader)         { vkDestroyShaderModule(m_Device, m_Shader, nullptr);     m_Shader = VK_NULL_HANDLE; }
}

void RISPass::Record(VkCommandBuffer cmd, uint32_t width, uint32_t height,
                     VkDescriptorSet set0, VkDescriptorSet set1,
                     uint32_t candidateCount) const
{
	if (!m_Pipeline)
		return;

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Pipeline);

	VkDescriptorSet sets[2] = { set0, set1 };
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
	                        m_PipelineLayout, 0, 2, sets, 0, nullptr);

	uint32_t M = candidateCount;
	vkCmdPushConstants(cmd, m_PipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
	                   0, sizeof(uint32_t), &M);

	vkCmdDispatch(cmd, (width + 15) / 16, (height + 15) / 16, 1);
}