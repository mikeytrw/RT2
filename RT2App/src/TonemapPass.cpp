#include "TonemapPass.h"
#include "GpuDevice.h"
#include "ShaderManager.h"
#include "VulkanUtils.h"
#include "RTLog.h"

bool TonemapPass::Init(const GpuDevice& dev)
{
    if (m_Pipeline) return true;
    m_Device = dev.device;

    m_Shader = ShaderManager::LoadShader("tonemap.spv");
    if (!m_Shader)
        m_Shader = ShaderManager::LoadShader("RT2App/shaders/tonemap.spv");
    if (!m_Shader)
    {
        RT_LOG("[TonemapPass] Failed to load tonemap.spv");
        return false;
    }

    VkDescriptorSetLayoutBinding bindings[2] = {};
    for (uint32_t i = 0; i < 2; ++i)
    {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo setInfo = {};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.bindingCount = 2;
    setInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(dev.device, &setInfo, nullptr, &m_SetLayout));

    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_SetLayout;
    VK_CHECK(vkCreatePipelineLayout(dev.device, &layoutInfo, nullptr, &m_PipelineLayout));

    VkComputePipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = m_Shader;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = m_PipelineLayout;
    VK_CHECK(vkCreateComputePipelines(dev.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_Pipeline));

    VkDescriptorPoolSize poolSize = {};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSize.descriptorCount = 2;
    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(dev.device, &poolInfo, nullptr, &m_Pool));

    VkDescriptorSetAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_Pool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_SetLayout;
    VK_CHECK(vkAllocateDescriptorSets(dev.device, &allocInfo, &m_DescriptorSet));
    return true;
}

void TonemapPass::Destroy()
{
    if (!m_Device) return;
    if (m_Pipeline) vkDestroyPipeline(m_Device, m_Pipeline, nullptr);
    if (m_PipelineLayout) vkDestroyPipelineLayout(m_Device, m_PipelineLayout, nullptr);
    if (m_SetLayout) vkDestroyDescriptorSetLayout(m_Device, m_SetLayout, nullptr);
    if (m_Pool) vkDestroyDescriptorPool(m_Device, m_Pool, nullptr);
    if (m_Shader) vkDestroyShaderModule(m_Device, m_Shader, nullptr);
    m_Pipeline = VK_NULL_HANDLE;
    m_PipelineLayout = VK_NULL_HANDLE;
    m_SetLayout = VK_NULL_HANDLE;
    m_DescriptorSet = VK_NULL_HANDLE;
    m_Pool = VK_NULL_HANDLE;
    m_Shader = VK_NULL_HANDLE;
    m_Device = VK_NULL_HANDLE;
}

void TonemapPass::UpdateDescriptorSet(const GpuDevice& dev, VkImageView inputView, VkImageView outputView)
{
    VkDescriptorImageInfo infos[2] = {};
    infos[0].imageView = inputView;
    infos[1].imageView = outputView;
    infos[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    infos[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[2] = {};
    for (uint32_t i = 0; i < 2; ++i)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = m_DescriptorSet;
        writes[i].dstBinding = i;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].descriptorCount = 1;
        writes[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(dev.device, 2, writes, 0, nullptr);
}

void TonemapPass::Record(VkCommandBuffer cmd, uint32_t width, uint32_t height) const
{
    if (!m_Pipeline || !m_DescriptorSet) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_PipelineLayout,
                            0, 1, &m_DescriptorSet, 0, nullptr);
    vkCmdDispatch(cmd, (width + 15) / 16, (height + 15) / 16, 1);
}
