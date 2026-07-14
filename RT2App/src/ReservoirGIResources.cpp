#include "ReservoirGIResources.h"
#include "GpuDevice.h"
#include "GpuResources.h"
#include "RTLog.h"

ReservoirGIResources::~ReservoirGIResources()
{
	Destroy();
}

void ReservoirGIResources::Create(const GpuDevice& dev, uint32_t width, uint32_t height)
{
	Destroy();

	m_Device = &dev;

	if (width == 0 || height == 0)
		return;

	m_Width = width;
	m_Height = height;
	m_IsDummy = false;

	VkDeviceSize pixelCount = VkDeviceSize(width) * VkDeviceSize(height);
	m_ReservoirRegionSize = pixelCount * sizeof(SIGIReservoir);          // 48 B/px
	m_ReceiverHistoryRegionSize = pixelCount * sizeof(SISurfaceHistory); // 32 B/px
	m_TotalSize = 2 * m_ReservoirRegionSize + 2 * m_ReceiverHistoryRegionSize;

	VkBufferUsageFlags usage =
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
		VK_BUFFER_USAGE_TRANSFER_DST_BIT;

	VkMemoryPropertyFlags memProps =
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	GpuResources::CreateBuffer(dev, m_TotalSize, usage, memProps,
	                           m_Buffer, m_BufferMemory);

	if (m_Buffer == VK_NULL_HANDLE)
	{
		RT_LOG("[ReservoirGIResources] Failed to allocate buffer "
		       "(%ux%u, %llu bytes)",
		       width, height, (unsigned long long)m_TotalSize);
		Destroy();
		return;
	}

	RT_LOG("[ReservoirGIResources] Allocated %ux%u GI buffer "
	       "(2 x reservoir %llu + 2 x history %llu = %llu bytes total, 160 B/px)",
	       width, height,
	       (unsigned long long)m_ReservoirRegionSize,
	       (unsigned long long)m_ReceiverHistoryRegionSize,
	       (unsigned long long)m_TotalSize);
}

void ReservoirGIResources::CreateDummy(const GpuDevice& dev)
{
	Destroy();

	m_Device = &dev;
	m_IsDummy = true;

	// 16 bytes is one uvec4 — a legal, never-read storage buffer.
	m_TotalSize = 16;
	m_ReservoirRegionSize = 0;
	m_ReceiverHistoryRegionSize = 0;
	m_Width = 0;
	m_Height = 0;

	VkBufferUsageFlags usage =
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
		VK_BUFFER_USAGE_TRANSFER_DST_BIT;

	VkMemoryPropertyFlags memProps =
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	GpuResources::CreateBuffer(dev, m_TotalSize, usage, memProps,
	                           m_Buffer, m_BufferMemory);

	if (m_Buffer == VK_NULL_HANDLE)
	{
		RT_LOG("[ReservoirGIResources] Failed to allocate dummy buffer");
		Destroy();
		return;
	}

	RT_LOG("[ReservoirGIResources] Allocated 16-byte dummy GI buffer (GI disabled)");
}

void ReservoirGIResources::Destroy()
{
	if (m_Device)
	{
		GpuResources::DestroyBuffer(*m_Device, m_Buffer, m_BufferMemory);
	}
	m_Buffer = VK_NULL_HANDLE;
	m_BufferMemory = VK_NULL_HANDLE;
	m_Width = 0;
	m_Height = 0;
	m_IsDummy = false;
	m_ReservoirRegionSize = 0;
	m_ReceiverHistoryRegionSize = 0;
	m_TotalSize = 0;
}

VkDeviceSize ReservoirGIResources::GetReservoirRegionOffset(uint32_t region) const
{
	return VkDeviceSize(region) * m_ReservoirRegionSize;
}

VkDeviceSize ReservoirGIResources::GetReceiverHistoryRegionOffset(uint32_t region) const
{
	return 2 * m_ReservoirRegionSize + VkDeviceSize(region) * m_ReceiverHistoryRegionSize;
}

void ReservoirGIResources::ClearAll(VkCommandBuffer cmd)
{
	if (m_Buffer == VK_NULL_HANDLE || m_IsDummy)
		return;

	VkBufferMemoryBarrier preBarrier = {};
	preBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	preBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
	preBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	preBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	preBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	preBarrier.buffer = m_Buffer;
	preBarrier.offset = 0;
	preBarrier.size = m_TotalSize;

	vkCmdPipelineBarrier(cmd,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
		0, nullptr, 1, &preBarrier, 0, nullptr);

	vkCmdFillBuffer(cmd, m_Buffer, 0, m_TotalSize, 0);

	VkBufferMemoryBarrier postBarrier = {};
	postBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	postBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	postBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	postBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	postBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	postBarrier.buffer = m_Buffer;
	postBarrier.offset = 0;
	postBarrier.size = m_TotalSize;

	vkCmdPipelineBarrier(cmd,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
		0, nullptr, 1, &postBarrier, 0, nullptr);
}