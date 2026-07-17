#include "GpuDiagnostics.h"
#include "GpuDevice.h"
#include "GpuResources.h"
#include "RTLog.h"
#include <cstring>

bool GpuDiagnostics::Init(const GpuDevice& device, uint32_t frameSlotCount)
{
	if (IsAvailable())
		return true;
	if (frameSlotCount == 0 || frameSlotCount > MaxFrameSlots)
		return false;

	m_FrameSlotCount = frameSlotCount;
	if (!GpuResources::CreateBuffer(device, SlotSize * frameSlotCount,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
		VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_DeviceBuffer))
	{
		RT_LOG("[GpuDiagnostics] failed to create device counter buffer");
		return false;
	}

	for (uint32_t i = 0; i < frameSlotCount; ++i)
	{
		if (!GpuResources::CreateBuffer(device, SlotSize,
			VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			m_ReadbackBuffers[i]))
		{
			RT_LOG("[GpuDiagnostics] failed to create readback buffer %u", i);
			Destroy(device);
			return false;
		}
	}

	RT_LOG("[GpuDiagnostics] enabled (%u counters, %u frame slots)",
	       CounterCount, frameSlotCount);
	return true;
}

void GpuDiagnostics::Destroy(const GpuDevice& device)
{
	for (auto& buffer : m_ReadbackBuffers)
		GpuResources::DestroyBuffer(device, buffer);
	GpuResources::DestroyBuffer(device, m_DeviceBuffer);
	m_Slots = {};
	m_Latest = {};
	m_FrameSlotCount = 0;
}

VkDeviceSize GpuDiagnostics::GetAllocatedBytes() const
{
	VkDeviceSize total = m_DeviceBuffer.size;
	for (uint32_t i = 0; i < m_FrameSlotCount; ++i)
		total += m_ReadbackBuffers[i].size;
	return total;
}

void GpuDiagnostics::ReadCompletedSlot(const GpuDevice& device, uint32_t frameSlot)
{
	if (!IsAvailable() || frameSlot >= m_FrameSlotCount)
		return;
	Slot& slot = m_Slots[frameSlot];
	if (!slot.submitted)
		return;

	Snapshot snapshot = {};
	snapshot.frameIndex = slot.frameIndex;
	void* mapped = nullptr;
	if (vkMapMemory(device.device, m_ReadbackBuffers[frameSlot].memory,
	                0, SlotSize, 0, &mapped) == VK_SUCCESS)
	{
		std::memcpy(snapshot.counters.data(), mapped, static_cast<size_t>(SlotSize));
		vkUnmapMemory(device.device, m_ReadbackBuffers[frameSlot].memory);
		snapshot.valid = true;
	}
	else
	{
		RT_LOG("[GpuDiagnostics] failed to map completed frame slot %u", frameSlot);
	}

	if (snapshot.valid && (!m_Latest.valid || snapshot.frameIndex >= m_Latest.frameIndex))
		m_Latest = snapshot;
	slot.submitted = false;
}

void GpuDiagnostics::BeginFrame(VkCommandBuffer cmd, uint32_t frameSlot, uint64_t frameIndex)
{
	if (!IsAvailable() || frameSlot >= m_FrameSlotCount)
		return;

	const VkDeviceSize offset = SlotSize * frameSlot;
	vkCmdFillBuffer(cmd, m_DeviceBuffer.buffer, offset, SlotSize, 0u);

	VkBufferMemoryBarrier barrier = {};
	barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer = m_DeviceBuffer.buffer;
	barrier.offset = offset;
	barrier.size = SlotSize;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
	                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	                     0, 0, nullptr, 1, &barrier, 0, nullptr);

	m_Slots[frameSlot].frameIndex = frameIndex;
	m_Slots[frameSlot].submitted = false;
}

void GpuDiagnostics::EndFrame(VkCommandBuffer cmd, uint32_t frameSlot)
{
	if (!IsAvailable() || frameSlot >= m_FrameSlotCount)
		return;

	const VkDeviceSize offset = SlotSize * frameSlot;
	VkBufferMemoryBarrier toTransfer = {};
	toTransfer.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	toTransfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toTransfer.buffer = m_DeviceBuffer.buffer;
	toTransfer.offset = offset;
	toTransfer.size = SlotSize;
	vkCmdPipelineBarrier(cmd,
	                     VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
	                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	                     VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     0, 0, nullptr, 1, &toTransfer, 0, nullptr);

	VkBufferCopy copy = {};
	copy.srcOffset = offset;
	copy.dstOffset = 0;
	copy.size = SlotSize;
	vkCmdCopyBuffer(cmd, m_DeviceBuffer.buffer,
	                m_ReadbackBuffers[frameSlot].buffer, 1, &copy);

	VkBufferMemoryBarrier toHost = {};
	toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toHost.buffer = m_ReadbackBuffers[frameSlot].buffer;
	toHost.offset = 0;
	toHost.size = SlotSize;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     VK_PIPELINE_STAGE_HOST_BIT,
	                     0, 0, nullptr, 1, &toHost, 0, nullptr);

	m_Slots[frameSlot].submitted = true;
}

const char* GpuDiagnostics::CounterName(uint32_t counter)
{
	static const char* names[CounterCount] = {
		"primary_rays", "continuation_rays", "shadow_rays", "shadow_occluded",
		"gi_fresh_rays", "gi_reevaluation_rays", "gi_secondary_shadow_rays",
		"di_fresh_candidates", "di_fresh_valid", "di_temporal_candidates",
		"di_temporal_accepted", "di_spatial_candidates", "di_spatial_accepted",
		"di_empty_reservoirs", "gi_fresh_candidates", "gi_fresh_valid",
		"gi_temporal_candidates", "gi_temporal_accepted", "gi_empty_reservoirs",
		"di_reject_offscreen", "gi_reject_offscreen",
		"di_reject_invalid_history", "gi_reject_invalid_history",
		"di_reject_material", "gi_reject_material",
		"di_reject_normal", "gi_reject_normal",
		"di_reject_depth", "gi_reject_depth",
		"di_reject_world_position", "gi_reject_world_position",
		"di_reject_age", "gi_reject_age",
		"di_reject_target", "gi_reject_target",
		"di_nonfinite", "gi_nonfinite", "fallback_paths",
		"di_clamped", "gi_clamped",
		"di_age_0", "di_age_1", "di_age_2", "di_age_3",
		"di_age_4", "di_age_5", "di_age_6", "di_age_7_plus",
		"gi_age_0", "gi_age_1", "gi_age_2", "gi_age_3",
		"gi_age_4", "gi_age_5", "gi_age_6", "gi_age_7_plus"
	};
	return counter < CounterCount ? names[counter] : "unknown";
}
