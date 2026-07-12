#include "ReservoirResources.h"
#include "GpuDevice.h"
#include "GpuResources.h"
#include "RTLog.h"

ReservoirResources::~ReservoirResources()
{
	Destroy();
}

void ReservoirResources::Create(const GpuDevice& dev, uint32_t width, uint32_t height)
{
	Destroy();

	m_Device = &dev;

	if (width == 0 || height == 0)
		return;

	m_Width = width;
	m_Height = height;

	VkDeviceSize entryCount = VkDeviceSize(width) * VkDeviceSize(height);
	m_BufferSize = entryCount * sizeof(SIReservoir);
	m_SurfaceHistorySize = entryCount * sizeof(SISurfaceHistory);

	VkBufferUsageFlags usage =
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
		VK_BUFFER_USAGE_TRANSFER_DST_BIT;

	VkMemoryPropertyFlags memProps =
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	GpuResources::CreateBuffer(dev, m_BufferSize, usage, memProps,
	                           m_HistoryBuffer, m_HistoryBufferMemory);

	GpuResources::CreateBuffer(dev, m_BufferSize, usage, memProps,
	                           m_ScratchBuffer, m_ScratchBufferMemory);

	GpuResources::CreateBuffer(dev, m_SurfaceHistorySize, usage, memProps,
	                           m_SurfaceHistoryBuffer, m_SurfaceHistoryBufferMemory);

	if (m_HistoryBuffer == VK_NULL_HANDLE || m_ScratchBuffer == VK_NULL_HANDLE
	    || m_SurfaceHistoryBuffer == VK_NULL_HANDLE)
	{
		RT_LOG("[ReservoirResources] Failed to allocate buffers "
		       "(%ux%u, %llu + %llu + %llu bytes)",
		       width, height,
		       (unsigned long long)m_BufferSize,
		       (unsigned long long)m_BufferSize,
		       (unsigned long long)m_SurfaceHistorySize);
		Destroy();
		return;
	}

	RT_LOG("[ReservoirResources] Allocated %ux%u buffers "
	       "(history+scratch: 2 x %llu + surfaceHistory: %llu bytes total)",
	       width, height,
	       (unsigned long long)m_BufferSize,
	       (unsigned long long)m_SurfaceHistorySize);
}

void ReservoirResources::Destroy()
{
	if (m_Device)
	{
		GpuResources::DestroyBuffer(*m_Device, m_HistoryBuffer, m_HistoryBufferMemory);
		GpuResources::DestroyBuffer(*m_Device, m_ScratchBuffer, m_ScratchBufferMemory);
		GpuResources::DestroyBuffer(*m_Device, m_SurfaceHistoryBuffer, m_SurfaceHistoryBufferMemory);
	}
	m_HistoryBuffer = VK_NULL_HANDLE;
	m_HistoryBufferMemory = VK_NULL_HANDLE;
	m_ScratchBuffer = VK_NULL_HANDLE;
	m_ScratchBufferMemory = VK_NULL_HANDLE;
	m_SurfaceHistoryBuffer = VK_NULL_HANDLE;
	m_SurfaceHistoryBufferMemory = VK_NULL_HANDLE;
	m_Width = 0;
	m_Height = 0;
	m_BufferSize = 0;
	m_SurfaceHistorySize = 0;
}

void ReservoirResources::ClearHistory(VkCommandBuffer cmd)
{
	if (m_HistoryBuffer == VK_NULL_HANDLE)
		return;

	VkBufferMemoryBarrier preBarriers[2] = {};
	for (int i = 0; i < 2; i++)
	{
		preBarriers[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		preBarriers[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
		preBarriers[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		preBarriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		preBarriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	}
	preBarriers[0].buffer = m_HistoryBuffer;
	preBarriers[0].offset = 0;
	preBarriers[0].size = m_BufferSize;
	preBarriers[1].buffer = m_SurfaceHistoryBuffer;
	preBarriers[1].offset = 0;
	preBarriers[1].size = m_SurfaceHistorySize;

	vkCmdPipelineBarrier(cmd,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
		0, nullptr, 2, preBarriers, 0, nullptr);

	vkCmdFillBuffer(cmd, m_HistoryBuffer, 0, m_BufferSize, 0);
	vkCmdFillBuffer(cmd, m_SurfaceHistoryBuffer, 0, m_SurfaceHistorySize, 0);

	VkBufferMemoryBarrier postBarriers[2] = {};
	for (int i = 0; i < 2; i++)
	{
		postBarriers[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		postBarriers[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		postBarriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		postBarriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		postBarriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	}
	postBarriers[0].buffer = m_HistoryBuffer;
	postBarriers[0].offset = 0;
	postBarriers[0].size = m_BufferSize;
	postBarriers[1].buffer = m_SurfaceHistoryBuffer;
	postBarriers[1].offset = 0;
	postBarriers[1].size = m_SurfaceHistorySize;

	vkCmdPipelineBarrier(cmd,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
		0, nullptr, 2, postBarriers, 0, nullptr);
}