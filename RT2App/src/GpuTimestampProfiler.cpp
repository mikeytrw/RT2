#include "GpuTimestampProfiler.h"
#include "GpuDevice.h"
#include "RTLog.h"
#include "VulkanUtils.h"
#include <vector>

const char* GpuTimestampProfiler::RegionName(Region region)
{
	switch (region)
	{
	case Region::Frame:             return "GPU Frame";
	case Region::Raster:            return "Raster";
	case Region::ReSTIRDITemporal:  return "ReSTIR DI Temporal";
	case Region::ReSTIRDISpatial:   return "ReSTIR DI Spatial";
	case Region::ReSTIRGITemporal:  return "ReSTIR GI Temporal";
	case Region::ReSTIRGIHistory:   return "ReSTIR GI History";
	case Region::RTShading:         return "RT Shading";
	case Region::NRD:               return "NRD";
	case Region::Compose:           return "Compose";
	case Region::Tonemap:           return "Tonemap";
	case Region::Count:             break;
	}
	return "Unknown";
}

bool GpuTimestampProfiler::Init(const GpuDevice& gpu, uint32_t frameSlotCount)
{
	if (m_QueryPool || frameSlotCount == 0 || frameSlotCount > m_Slots.size())
		return m_QueryPool != VK_NULL_HANDLE;

	VkPhysicalDeviceProperties properties = {};
	vkGetPhysicalDeviceProperties(gpu.physicalDevice, &properties);

	uint32_t queueFamilyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(gpu.physicalDevice, &queueFamilyCount, nullptr);
	std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
	vkGetPhysicalDeviceQueueFamilyProperties(gpu.physicalDevice, &queueFamilyCount, queueFamilies.data());
	if (gpu.queueFamily >= queueFamilyCount || queueFamilies[gpu.queueFamily].timestampValidBits == 0)
	{
		RT_LOG("[GpuTiming] timestamps unavailable on queue family %u", gpu.queueFamily);
		return false;
	}

	m_FrameSlotCount = frameSlotCount;
	m_ActiveFrameSlot = 0;
	m_TimestampValidBits = queueFamilies[gpu.queueFamily].timestampValidBits;
	m_TimestampPeriod = properties.limits.timestampPeriod;

	VkQueryPoolCreateInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	info.queryType = VK_QUERY_TYPE_TIMESTAMP;
	info.queryCount = frameSlotCount * QueriesPerSlot;
	VK_CHECK(vkCreateQueryPool(gpu.device, &info, nullptr, &m_QueryPool));
	RT_LOG("[GpuTiming] enabled (%u queries, %.3f ns/tick, %u valid bits)",
	       info.queryCount, m_TimestampPeriod, m_TimestampValidBits);
	return true;
}

void GpuTimestampProfiler::Destroy(VkDevice device)
{
	if (m_QueryPool)
		vkDestroyQueryPool(device, m_QueryPool, nullptr);
	m_QueryPool = VK_NULL_HANDLE;
	m_Slots = {};
	m_Latest = {};
	m_FrameSlotCount = 0;
	m_ActiveFrameSlot = 0;
}

uint32_t GpuTimestampProfiler::QueryIndex(uint32_t frameSlot, Region region, bool end) const
{
	return frameSlot * QueriesPerSlot + static_cast<uint32_t>(region) * QueriesPerRegion + (end ? 1u : 0u);
}

uint64_t GpuTimestampProfiler::TickDelta(uint64_t begin, uint64_t end) const
{
	if (end >= begin)
		return end - begin;
	if (m_TimestampValidBits == 64)
		return end - begin;
	return end + (uint64_t(1) << m_TimestampValidBits) - begin;
}

void GpuTimestampProfiler::ReadCompletedSlot(VkDevice device, uint32_t frameSlot)
{
	if (!IsAvailable() || frameSlot >= m_FrameSlotCount)
		return;

	Slot& slot = m_Slots[frameSlot];
	if (!slot.submitted)
		return;

	Timings timings = {};
	timings.frameIndex = slot.frameIndex;
	for (uint32_t region = 0; region < RegionCount; region++)
	{
		if ((slot.issuedMask & (1u << region)) == 0)
			continue;

		uint64_t values[2] = {};
		VkResult result = vkGetQueryPoolResults(device, m_QueryPool,
			QueryIndex(frameSlot, static_cast<Region>(region), false), 2,
			sizeof(values), values, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
		if (result != VK_SUCCESS)
		{
			RT_LOG("[GpuTiming] query read failed for region %u: %d", region, result);
			continue;
		}

		timings.validMask |= 1u << region;
		timings.milliseconds[region] = float(TickDelta(values[0], values[1]) * double(m_TimestampPeriod) / 1000000.0);
	}
	// Flushing more than one completed frame slot can encounter them out of
	// ring order. Never let an older slot replace a newer measurement.
	if (timings.validMask != 0 &&
	    (m_Latest.validMask == 0 || timings.frameIndex >= m_Latest.frameIndex))
		m_Latest = timings;
	slot.submitted = false;
}

void GpuTimestampProfiler::BeginFrame(VkCommandBuffer cmd, uint32_t frameSlot, uint64_t frameIndex)
{
	if (!IsAvailable() || frameSlot >= m_FrameSlotCount)
		return;

	Slot& slot = m_Slots[frameSlot];
	slot.frameIndex = frameIndex;
	slot.issuedMask = 0;
	slot.activeMask = 0;
	m_ActiveFrameSlot = frameSlot;
	vkCmdResetQueryPool(cmd, m_QueryPool, frameSlot * QueriesPerSlot, QueriesPerSlot);
}

void GpuTimestampProfiler::EndFrame(uint32_t frameSlot)
{
	if (!IsAvailable() || frameSlot >= m_FrameSlotCount)
		return;
	Slot& slot = m_Slots[frameSlot];
	if (slot.activeMask != 0)
		RT_LOG("[GpuTiming] frame ended with active timestamp regions: 0x%x", slot.activeMask);
	slot.activeMask = 0;
	slot.submitted = true;
}

void GpuTimestampProfiler::BeginRegion(VkCommandBuffer cmd, Region region, VkPipelineStageFlagBits stage)
{
	if (!IsAvailable())
		return;
	Slot& slot = m_Slots[m_ActiveFrameSlot];
	const uint32_t bit = 1u << static_cast<uint32_t>(region);
	if ((slot.issuedMask & bit) != 0 || (slot.activeMask & bit) != 0)
	{
		RT_LOG("[GpuTiming] duplicate region in frame %llu: %s",
		       static_cast<unsigned long long>(slot.frameIndex), RegionName(region));
		return;
	}
	vkCmdWriteTimestamp(cmd, stage, m_QueryPool, QueryIndex(m_ActiveFrameSlot, region, false));
	slot.activeMask |= bit;
}

void GpuTimestampProfiler::EndRegion(VkCommandBuffer cmd, Region region, VkPipelineStageFlagBits stage)
{
	if (!IsAvailable())
		return;
	Slot& slot = m_Slots[m_ActiveFrameSlot];
	const uint32_t bit = 1u << static_cast<uint32_t>(region);
	if ((slot.activeMask & bit) == 0)
		return;
	vkCmdWriteTimestamp(cmd, stage, m_QueryPool, QueryIndex(m_ActiveFrameSlot, region, true));
	slot.activeMask &= ~bit;
	slot.issuedMask |= bit;
}
