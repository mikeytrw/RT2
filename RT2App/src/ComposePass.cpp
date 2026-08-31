#include "ComposePass.h"
#include "GpuDevice.h"
#include "VulkanUtils.h"
#include "ShaderManager.h"
#include "RTLog.h"
#include <iostream>

bool ComposePass::Init(const GpuDevice& dev)
{
	if (m_Pipeline != VK_NULL_HANDLE) return true;

	m_Device = dev.device; // cache for Destroy

	m_Shader = ShaderManager::LoadShader("compose.spv");
	if (!m_Shader)
		m_Shader = ShaderManager::LoadShader("RT2App/shaders/compose.spv");
	if (!m_Shader)
	{
		RT_LOG("[ComposePass] Failed to load compose.spv");
		return false;
	}

	VkDescriptorSetLayoutBinding bindings[8] = {};
	for (int i = 0; i < 7; i++)
	{
		bindings[i].binding = i;
		bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	// Binding 7: camera UBO (for view direction reconstruction)
	bindings[7].binding = 7;
	bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	bindings[7].descriptorCount = 1;
	bindings[7].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

	VkDescriptorSetLayoutCreateInfo layoutInfo = {};
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = 8;
	layoutInfo.pBindings = bindings;
	VK_CHECK(vkCreateDescriptorSetLayout(dev.device, &layoutInfo, nullptr, &m_SetLayout));

	VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = 1;
	pipelineLayoutInfo.pSetLayouts = &m_SetLayout;
	VK_CHECK(vkCreatePipelineLayout(dev.device, &pipelineLayoutInfo, nullptr, &m_PipelineLayout));

	VkComputePipelineCreateInfo pipelineInfo = {};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	pipelineInfo.stage.module = m_Shader;
	pipelineInfo.stage.pName = "main";
	pipelineInfo.layout = m_PipelineLayout;

	VK_CHECK(vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_Pipeline));

	// Create descriptor pool + set
	VkDescriptorPoolSize poolSizes[2] = {};
	poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	poolSizes[0].descriptorCount = 7;
	poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	poolSizes[1].descriptorCount = 1;

	VkDescriptorPoolCreateInfo poolInfo = {};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets = 1;
	poolInfo.poolSizeCount = 2;
	poolInfo.pPoolSizes = poolSizes;
	VK_CHECK(vkCreateDescriptorPool(dev.device, &poolInfo, nullptr, &m_Pool));

	VkDescriptorSetAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.descriptorPool = m_Pool;
	allocInfo.descriptorSetCount = 1;
	allocInfo.pSetLayouts = &m_SetLayout;
	VK_CHECK(vkAllocateDescriptorSets(dev.device, &allocInfo, &m_DescriptorSet));

	RT_LOG("[ComposePass] initialized");
	return true;
}

void ComposePass::Destroy()
{
	if (!m_Pipeline) return;
	VkDevice device = m_Device;
	if (m_Pipeline)       { vkDestroyPipeline(device, m_Pipeline, nullptr);       m_Pipeline = VK_NULL_HANDLE; }
	if (m_PipelineLayout) { vkDestroyPipelineLayout(device, m_PipelineLayout, nullptr); m_PipelineLayout = VK_NULL_HANDLE; }
	if (m_SetLayout)      { vkDestroyDescriptorSetLayout(device, m_SetLayout, nullptr); m_SetLayout = VK_NULL_HANDLE; }
	if (m_Pool)           { vkDestroyDescriptorPool(device, m_Pool, nullptr);     m_Pool = VK_NULL_HANDLE; }
	if (m_Shader)         { vkDestroyShaderModule(device, m_Shader, nullptr);     m_Shader = VK_NULL_HANDLE; }
	m_DescriptorSet = VK_NULL_HANDLE;
}

void ComposePass::OnResize(const GpuDevice& dev, const RenderExtent& extent)
{
	(void)dev;
	(void)extent;
	// Compose pass doesn't own images, no resize needed
}

void ComposePass::UpdateDescriptorSet(const GpuDevice& dev,
                                      VkImageView outputView,
                                      VkImageView nrdDiffOutView,
                                      VkImageView nrdSpecOutView,
                                      VkImageView albedoF0View,
                                      VkImageView directEmissionView,
                                      VkImageView viewZView,
                                      VkImageView normalRoughnessView,
                                      VkBuffer cameraUBO)
{
	VkDescriptorImageInfo imageInfos[7] = {};
	VkImageView views[] = { outputView, nrdDiffOutView, nrdSpecOutView, albedoF0View, directEmissionView, viewZView, normalRoughnessView };
	for (int i = 0; i < 7; i++)
	{
		imageInfos[i].imageView = views[i];
		imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	}

	VkDescriptorBufferInfo uboInfo = {};
	uboInfo.buffer = cameraUBO;
	uboInfo.offset = 0;
	uboInfo.range = VK_WHOLE_SIZE;

	VkWriteDescriptorSet writes[8] = {};
	for (int i = 0; i < 7; i++)
	{
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = m_DescriptorSet;
		writes[i].dstBinding = i;
		writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		writes[i].descriptorCount = 1;
		writes[i].pImageInfo = &imageInfos[i];
	}
	writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[7].dstSet = m_DescriptorSet;
	writes[7].dstBinding = 7;
	writes[7].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	writes[7].descriptorCount = 1;
	writes[7].pBufferInfo = &uboInfo;

	vkUpdateDescriptorSets(dev.device, 8, writes, 0, nullptr);
}

void ComposePass::Record(VkCommandBuffer cmd, const RenderExtent& extent) const
{
	if (!m_Pipeline || !m_DescriptorSet) return;
	const uint32_t width = extent.Width();
	const uint32_t height = extent.Height();

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_PipelineLayout, 0, 1, &m_DescriptorSet, 0, nullptr);
	vkCmdDispatch(cmd, (width + 15) / 16, (height + 15) / 16, 1);
}
