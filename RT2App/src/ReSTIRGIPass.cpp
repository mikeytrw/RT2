#include "ReSTIRGIPass.h"
#include "GpuDevice.h"
#include "ShaderManager.h"
#include "VulkanUtils.h"
#include "RTLog.h"

bool ReSTIRGIPass::Init(const GpuDevice& dev,
                        VkDescriptorSetLayout set0Layout,
                        VkDescriptorSetLayout set1Layout,
                        bool allowDiag)
{
	if (m_TemporalPipeline != VK_NULL_HANDLE)
		return true;

	m_Device = dev.device;

	m_TemporalShader = ShaderManager::LoadShader("restir_gi_temporal.spv");
	if (!m_TemporalShader)
		m_TemporalShader = ShaderManager::LoadShader("RT2App/shaders/restir_gi_temporal.spv");
	if (!m_TemporalShader)
	{
		RT_LOG("[ReSTIRGIPass] Failed to load restir_gi_temporal.spv");
		return false;
	}

	m_HistoryShader = ShaderManager::LoadShader("restir_gi_history.spv");
	if (!m_HistoryShader)
		m_HistoryShader = ShaderManager::LoadShader("RT2App/shaders/restir_gi_history.spv");
	if (!m_HistoryShader)
	{
		RT_LOG("[ReSTIRGIPass] Failed to load restir_gi_history.spv");
		return false;
	}

	// Shadow-tail diagnostic (RT4-T4, ON-only temporal module). Gated by
	// allowDiag so ordinary startup on clock-unsupported devices never
	// touches tail modules. Soft-fail: production is unaffected; a
	// requested diagnostic without this module fails loud at request
	// time (IsDiagAvailable), never with silent zeros.
	m_DiagAvailable = false;
	if (allowDiag)
	{
		m_TemporalTailShader = ShaderManager::LoadShader("restir_gi_temporal_tail.spv");
		if (!m_TemporalTailShader)
			m_TemporalTailShader = ShaderManager::LoadShader("RT2App/shaders/restir_gi_temporal_tail.spv");
		m_DiagAvailable = (m_TemporalTailShader != VK_NULL_HANDLE);
	}
	RT_LOG("[ReSTIRGIPass] shadow-tail variant %s", m_DiagAvailable ? "available" : "unavailable (tail SPV not loaded)");

	VkPipelineLayoutCreateInfo layoutInfo = {};
	layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

	VkDescriptorSetLayout setLayouts[2] = { set0Layout, set1Layout };
	layoutInfo.setLayoutCount = 2;
	layoutInfo.pSetLayouts = setLayouts;

	VkPushConstantRange pcRange = {};
	pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pcRange.offset = 0;
	pcRange.size = sizeof(SIGIPushConstants);
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

	// Diagnostic temporal pipeline shares the layout; built only when the
	// tail module loaded. Soft-fail (VkResult checked, never VK_CHECK
	// abort): an ordinary startup must never abort on a diagnostic
	// pipeline, and a requested diagnostic without it fails loud via
	// IsDiagAvailable at request time.
	if (m_DiagAvailable)
	{
		VkComputePipelineCreateInfo diagInfo = temporalInfo;
		diagInfo.stage.module = m_TemporalTailShader;
		VkPipeline diagPipeline = VK_NULL_HANDLE;
		const VkResult diagErr = vkCreateComputePipelines(m_Device, VK_NULL_HANDLE, 1,
		                                                  &diagInfo, nullptr, &diagPipeline);
		if (diagErr != VK_SUCCESS || diagPipeline == VK_NULL_HANDLE)
		{
			RT_LOG("[ReSTIRGIPass] diagnostic temporal pipeline build failed: %d (loud, no fallback)", (int)diagErr);
			if (diagPipeline) { vkDestroyPipeline(m_Device, diagPipeline, nullptr); }
			m_DiagAvailable = false;
			if (m_TemporalTailShader) { vkDestroyShaderModule(m_Device, m_TemporalTailShader, nullptr); m_TemporalTailShader = VK_NULL_HANDLE; }
		}
		else
			m_DiagTemporalPipeline = diagPipeline;
	}

	VkComputePipelineCreateInfo historyInfo = {};
	historyInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	historyInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	historyInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	historyInfo.stage.module = m_HistoryShader;
	historyInfo.stage.pName = "main";
	historyInfo.layout = m_PipelineLayout;

	VK_CHECK(vkCreateComputePipelines(m_Device, VK_NULL_HANDLE, 1,
	                                  &historyInfo, nullptr, &m_HistoryPipeline));

	RT_LOG("[ReSTIRGIPass] initialized (temporal + history pipelines)");
	return true;
}

void ReSTIRGIPass::Destroy()
{
	if (m_TemporalPipeline) { vkDestroyPipeline(m_Device, m_TemporalPipeline, nullptr); m_TemporalPipeline = VK_NULL_HANDLE; }
	if (m_DiagTemporalPipeline) { vkDestroyPipeline(m_Device, m_DiagTemporalPipeline, nullptr); m_DiagTemporalPipeline = VK_NULL_HANDLE; }
	if (m_HistoryPipeline)  { vkDestroyPipeline(m_Device, m_HistoryPipeline,  nullptr); m_HistoryPipeline  = VK_NULL_HANDLE; }
	if (m_PipelineLayout)   { vkDestroyPipelineLayout(m_Device, m_PipelineLayout,   nullptr); m_PipelineLayout   = VK_NULL_HANDLE; }
	if (m_TemporalShader)   { vkDestroyShaderModule(m_Device, m_TemporalShader, nullptr); m_TemporalShader = VK_NULL_HANDLE; }
	if (m_TemporalTailShader) { vkDestroyShaderModule(m_Device, m_TemporalTailShader, nullptr); m_TemporalTailShader = VK_NULL_HANDLE; }
	if (m_HistoryShader)    { vkDestroyShaderModule(m_Device, m_HistoryShader,  nullptr); m_HistoryShader  = VK_NULL_HANDLE; }
	m_DiagAvailable = false;
}

void ReSTIRGIPass::RecordTemporal(VkCommandBuffer cmd, const RenderExtent& extent,
                                  VkDescriptorSet set0, VkDescriptorSet set1,
                                  const SIGIPushConstants& pc, bool useDiag) const
{
	const uint32_t width = extent.Width(); const uint32_t height = extent.Height();
	// No silent fallback: a requested-but-missing diag pipeline binds
	// nothing (loud RendererGPU-level gate fires first — see
	// RendererGPU::Render). Never render production data as diagnostic data.
	VkPipeline pipeline = useDiag ? m_DiagTemporalPipeline : m_TemporalPipeline;
	if (!pipeline)
		return;

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);

	VkDescriptorSet sets[2] = { set0, set1 };
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
	                        m_PipelineLayout, 0, 2, sets, 0, nullptr);

	vkCmdPushConstants(cmd, m_PipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
	                   0, sizeof(SIGIPushConstants), &pc);

	vkCmdDispatch(cmd, (width + 15) / 16, (height + 15) / 16, 1);
}

void ReSTIRGIPass::RecordHistoryWrite(VkCommandBuffer cmd, const RenderExtent& extent,
                                       VkDescriptorSet set0, VkDescriptorSet set1,
                                       const SIGIPushConstants& pc) const
{
	const uint32_t width = extent.Width(); const uint32_t height = extent.Height();
	if (!m_HistoryPipeline)
		return;

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_HistoryPipeline);

	VkDescriptorSet sets[2] = { set0, set1 };
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
	                        m_PipelineLayout, 0, 2, sets, 0, nullptr);

	vkCmdPushConstants(cmd, m_PipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
	                   0, sizeof(SIGIPushConstants), &pc);

	vkCmdDispatch(cmd, (width + 15) / 16, (height + 15) / 16, 1);
}
