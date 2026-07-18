#include "GpuPickingPass.h"

#include "RTLog.h"
#include "ShaderManager.h"
#include "VulkanUtils.h"

#include <cstring>

bool GpuPickingPass::Init(const GpuDevice& device,
	VkDescriptorSetLayout sceneSetLayout, uint32_t frameSlotCount)
{
	if (IsAvailable()) return true;
	if (frameSlotCount == 0 || frameSlotCount > MaxFrameSlots) return false;
	m_Device = device;
	m_FrameSlotCount = frameSlotCount;

	VkDescriptorSetLayoutBinding resultBinding{};
	resultBinding.binding = 0;
	resultBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	resultBinding.descriptorCount = 1;
	resultBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	VkDescriptorSetLayoutCreateInfo setInfo{};
	setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	setInfo.bindingCount = 1;
	setInfo.pBindings = &resultBinding;
	VK_CHECK(vkCreateDescriptorSetLayout(device.device, &setInfo, nullptr,
	                                     &m_ResultSetLayout));

	VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, frameSlotCount };
	VkDescriptorPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets = frameSlotCount;
	poolInfo.poolSizeCount = 1;
	poolInfo.pPoolSizes = &poolSize;
	VK_CHECK(vkCreateDescriptorPool(device.device, &poolInfo, nullptr,
	                                &m_DescriptorPool));

	for (uint32_t i = 0; i < frameSlotCount; ++i)
	{
		if (!GpuResources::CreateBuffer(device, sizeof(GpuResult),
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			m_Slots[i].resultBuffer))
		{
			Destroy();
			return false;
		}
		VkDescriptorSetAllocateInfo allocateInfo{};
		allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		allocateInfo.descriptorPool = m_DescriptorPool;
		allocateInfo.descriptorSetCount = 1;
		allocateInfo.pSetLayouts = &m_ResultSetLayout;
		VK_CHECK(vkAllocateDescriptorSets(device.device, &allocateInfo,
		                                  &m_Slots[i].descriptorSet));
		VkDescriptorBufferInfo bufferInfo{
			m_Slots[i].resultBuffer.buffer, 0, sizeof(GpuResult)
		};
		VkWriteDescriptorSet write{};
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = m_Slots[i].descriptorSet;
		write.dstBinding = 0;
		write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		write.pBufferInfo = &bufferInfo;
		vkUpdateDescriptorSets(device.device, 1, &write, 0, nullptr);
	}

	VkDescriptorSetLayout layouts[] = { sceneSetLayout, m_ResultSetLayout };
	VkPushConstantRange pushRange{};
	pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pushRange.offset = 0;
	pushRange.size = sizeof(glm::vec4) * 2;
	VkPipelineLayoutCreateInfo layoutInfo{};
	layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layoutInfo.setLayoutCount = 2;
	layoutInfo.pSetLayouts = layouts;
	layoutInfo.pushConstantRangeCount = 1;
	layoutInfo.pPushConstantRanges = &pushRange;
	VK_CHECK(vkCreatePipelineLayout(device.device, &layoutInfo, nullptr,
	                               &m_PipelineLayout));

	VkShaderModule shader = ShaderManager::LoadShader("picking.spv");
	if (!shader)
	{
		RT_LOG("[GpuPickingPass] failed to load picking.spv");
		Destroy();
		return false;
	}
	VkPipelineShaderStageCreateInfo stage{};
	stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	stage.module = shader;
	stage.pName = "main";
	VkComputePipelineCreateInfo pipelineInfo{};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipelineInfo.stage = stage;
	pipelineInfo.layout = m_PipelineLayout;
	const VkResult result = vkCreateComputePipelines(device.device, VK_NULL_HANDLE,
		1, &pipelineInfo, nullptr, &m_Pipeline);
	vkDestroyShaderModule(device.device, shader, nullptr);
	if (result != VK_SUCCESS)
	{
		RT_LOG("[GpuPickingPass] pipeline creation failed: %d", int(result));
		Destroy();
		return false;
	}
	return true;
}

void GpuPickingPass::Destroy()
{
	if (m_Device.device)
	{
		for (auto& slot : m_Slots)
			GpuResources::DestroyBuffer(m_Device, slot.resultBuffer);
		if (m_Pipeline) vkDestroyPipeline(m_Device.device, m_Pipeline, nullptr);
		if (m_PipelineLayout) vkDestroyPipelineLayout(m_Device.device, m_PipelineLayout, nullptr);
		if (m_DescriptorPool) vkDestroyDescriptorPool(m_Device.device, m_DescriptorPool, nullptr);
		if (m_ResultSetLayout) vkDestroyDescriptorSetLayout(m_Device.device, m_ResultSetLayout, nullptr);
	}
	m_Pipeline = VK_NULL_HANDLE;
	m_PipelineLayout = VK_NULL_HANDLE;
	m_DescriptorPool = VK_NULL_HANDLE;
	m_ResultSetLayout = VK_NULL_HANDLE;
	m_Slots = {};
	m_FrameSlotCount = 0;
}

std::optional<GpuPickingPass::CompletedPick>
GpuPickingPass::ReadCompletedSlot(uint32_t frameSlot)
{
	if (frameSlot >= m_FrameSlotCount || !m_Slots[frameSlot].submitted)
		return std::nullopt;
	Slot& slot = m_Slots[frameSlot];
	GpuResult gpu{};
	void* mapped = nullptr;
	if (vkMapMemory(m_Device.device, slot.resultBuffer.memory, 0,
	                sizeof(GpuResult), 0, &mapped) != VK_SUCCESS)
	{
		slot.submitted = false;
		slot.instanceMap.clear();
		return std::nullopt;
	}
	std::memcpy(&gpu, mapped, sizeof(gpu));
	vkUnmapMemory(m_Device.device, slot.resultBuffer.memory);

	CompletedPick completed;
	completed.serial = slot.serial;
	completed.hit = gpu.hit != 0;
	completed.instanceIndex = gpu.instanceIndex;
	completed.worldPosition = glm::vec3(gpu.worldPosition);
	completed.instanceMap = std::move(slot.instanceMap);
	slot.submitted = false;
	return completed;
}

void GpuPickingPass::Record(VkCommandBuffer cmd, uint32_t frameSlot,
	VkDescriptorSet sceneSet, const glm::vec3& origin, const glm::vec3& direction,
	float tMax, uint64_t serial, const RenderInstanceMap& instanceMap)
{
	if (!IsAvailable() || frameSlot >= m_FrameSlotCount) return;
	Slot& slot = m_Slots[frameSlot];
	vkCmdFillBuffer(cmd, slot.resultBuffer.buffer, 0, sizeof(GpuResult), 0u);
	VkBufferMemoryBarrier before{};
	before.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	before.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	before.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.buffer = slot.resultBuffer.buffer;
	before.offset = 0;
	before.size = sizeof(GpuResult);
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
	                     0, nullptr, 1, &before, 0, nullptr);

	const glm::vec4 constants[] = {
		glm::vec4(origin, 0.001f), glm::vec4(glm::normalize(direction), tMax)
	};
	VkDescriptorSet sets[] = { sceneSet, slot.descriptorSet };
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
		m_PipelineLayout, 0, 2, sets, 0, nullptr);
	vkCmdPushConstants(cmd, m_PipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
		0, sizeof(constants), constants);
	vkCmdDispatch(cmd, 1, 1, 1);

	VkBufferMemoryBarrier after = before;
	after.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	after.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	                     VK_PIPELINE_STAGE_HOST_BIT, 0,
	                     0, nullptr, 1, &after, 0, nullptr);
	slot.serial = serial;
	slot.submitted = true;
	slot.instanceMap = instanceMap;
}

void GpuPickingPass::Invalidate()
{
	for (auto& slot : m_Slots)
	{
		slot.submitted = false;
		slot.instanceMap.clear();
	}
}
