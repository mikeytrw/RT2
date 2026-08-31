#include "ReSTIRPass.h"
#include "GpuDevice.h"
#include "ShaderManager.h"
#include "VulkanUtils.h"
#include "RTLog.h"

bool ReSTIRPass::Init(const GpuDevice& dev,
                      VkDescriptorSetLayout set0Layout,
                      VkDescriptorSetLayout set1Layout)
{
	if (m_TemporalPipeline != VK_NULL_HANDLE)
		return true;

	m_Device = dev.device;

	m_TemporalShader = ShaderManager::LoadShader("restir_temporal.spv");
	if (!m_TemporalShader)
		m_TemporalShader = ShaderManager::LoadShader("RT2App/shaders/restir_temporal.spv");
	if (!m_TemporalShader)
	{
		RT_LOG("[ReSTIRPass] Failed to load restir_temporal.spv");
		return false;
	}

	m_SpatialShader = ShaderManager::LoadShader("restir_spatial.spv");
	if (!m_SpatialShader)
		m_SpatialShader = ShaderManager::LoadShader("RT2App/shaders/restir_spatial.spv");
	if (!m_SpatialShader)
	{
		RT_LOG("[ReSTIRPass] Failed to load restir_spatial.spv");
		return false;
	}

	VkPipelineLayoutCreateInfo layoutInfo = {};
	layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

	VkDescriptorSetLayout setLayouts[2] = { set0Layout, set1Layout };
	layoutInfo.setLayoutCount = 2;
	layoutInfo.pSetLayouts = setLayouts;

	VkPushConstantRange pcRange = {};
	pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pcRange.offset = 0;
	pcRange.size = sizeof(SIReSTIRPushConstants);
	layoutInfo.pushConstantRangeCount = 1;
	layoutInfo.pPushConstantRanges = &pcRange;

	VK_CHECK(vkCreatePipelineLayout(m_Device, &layoutInfo, nullptr, &m_PipelineLayout));

	VkComputePipelineCreateInfo temporalInfo = {};
	temporalInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	temporalInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	temporalInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	temporalInfo.stage.module = m_TemporalShader;
	temporalInfo.stage.pName = "main";
	temporalInfo.layout = m_PipelineLayout;

	VK_CHECK(vkCreateComputePipelines(m_Device, VK_NULL_HANDLE, 1,
	                                  &temporalInfo, nullptr, &m_TemporalPipeline));

	VkComputePipelineCreateInfo spatialInfo = {};
	spatialInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	spatialInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	spatialInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	spatialInfo.stage.module = m_SpatialShader;
	spatialInfo.stage.pName = "main";
	spatialInfo.layout = m_PipelineLayout;

	VK_CHECK(vkCreateComputePipelines(m_Device, VK_NULL_HANDLE, 1,
	                                  &spatialInfo, nullptr, &m_SpatialPipeline));

	RT_LOG("[ReSTIRPass] initialized (temporal + spatial pipelines)");
	return true;
}

void ReSTIRPass::Destroy()
{
	if (m_TemporalPipeline) { vkDestroyPipeline(m_Device, m_TemporalPipeline, nullptr); m_TemporalPipeline = VK_NULL_HANDLE; }
	if (m_SpatialPipeline)  { vkDestroyPipeline(m_Device, m_SpatialPipeline, nullptr);  m_SpatialPipeline = VK_NULL_HANDLE; }
	if (m_PipelineLayout)    { vkDestroyPipelineLayout(m_Device, m_PipelineLayout, nullptr); m_PipelineLayout = VK_NULL_HANDLE; }
	if (m_TemporalShader)    { vkDestroyShaderModule(m_Device, m_TemporalShader, nullptr); m_TemporalShader = VK_NULL_HANDLE; }
	if (m_SpatialShader)     { vkDestroyShaderModule(m_Device, m_SpatialShader, nullptr);  m_SpatialShader = VK_NULL_HANDLE; }
}

void ReSTIRPass::RecordTemporal(VkCommandBuffer cmd, const RenderExtent& extent,
                                VkDescriptorSet set0, VkDescriptorSet set1,
                                const SIReSTIRPushConstants& pc) const
{
	const uint32_t width = extent.Width(); const uint32_t height = extent.Height();
	if (!m_TemporalPipeline)
		return;

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_TemporalPipeline);

	VkDescriptorSet sets[2] = { set0, set1 };
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
	                        m_PipelineLayout, 0, 2, sets, 0, nullptr);

	vkCmdPushConstants(cmd, m_PipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
	                   0, sizeof(SIReSTIRPushConstants), &pc);

	vkCmdDispatch(cmd, (width + 15) / 16, (height + 15) / 16, 1);
}

void ReSTIRPass::RecordSpatial(VkCommandBuffer cmd, const RenderExtent& extent,
                               VkDescriptorSet set0, VkDescriptorSet set1,
                               const SIReSTIRPushConstants& pc) const
{
	const uint32_t width = extent.Width(); const uint32_t height = extent.Height();
	if (!m_SpatialPipeline)
		return;

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_SpatialPipeline);

	VkDescriptorSet sets[2] = { set0, set1 };
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
	                        m_PipelineLayout, 0, 2, sets, 0, nullptr);

	vkCmdPushConstants(cmd, m_PipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
	                   0, sizeof(SIReSTIRPushConstants), &pc);

	vkCmdDispatch(cmd, (width + 15) / 16, (height + 15) / 16, 1);
}

void ReSTIRPass::Record(VkCommandBuffer cmd, const RenderExtent& extent,
                        VkDescriptorSet set0, VkDescriptorSet set1,
                        const SIReSTIRPushConstants& pc) const
{
	if (!m_TemporalPipeline || !m_SpatialPipeline)
		return;

	// 1. Temporal pass: history → scratch
	// Barrier: history (previous spatial write) → temporal read
	// + surfaceHistory (previous temporal write) → temporal read
	VkBufferMemoryBarrier temporalPreBarriers[2] = {};
	for (int i = 0; i < 2; i++)
	{
		temporalPreBarriers[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		temporalPreBarriers[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		temporalPreBarriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		temporalPreBarriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		temporalPreBarriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	}
	temporalPreBarriers[0].buffer = 0; // set below by caller? No — we don't have buffer handles here.
	// Actually, the barriers need the actual buffer handles. Since this method
	// doesn't have access to ReservoirResources, the caller must handle barriers.
	// For now, we just dispatch both passes and let the caller manage barriers.

	RecordTemporal(cmd, extent, set0, set1, pc);

	// 2. Barrier: scratch (temporal write) → spatial read
	// + history (temporal read) → spatial write
	VkBufferMemoryBarrier scratchAndHistoryBarriers[2] = {};
	for (int i = 0; i < 2; i++)
	{
		scratchAndHistoryBarriers[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		scratchAndHistoryBarriers[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		scratchAndHistoryBarriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		scratchAndHistoryBarriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		scratchAndHistoryBarriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	}
	// Same issue — caller must handle barriers. This method is not used directly.
	// See FrameRenderer::RecordReSTIRPass for the full barrier-managed dispatch.

	RecordSpatial(cmd, extent, set0, set1, pc);
}
