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

	VkBufferUsageFlags usage =
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
		VK_BUFFER_USAGE_TRANSFER_DST_BIT;

	VkMemoryPropertyFlags memProps =
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	GpuResources::CreateBuffer(dev, m_BufferSize, usage, memProps,
	                           m_CurrentBuffer, m_CurrentBufferMemory);

	GpuResources::CreateBuffer(dev, m_BufferSize, usage, memProps,
	                           m_PrevBuffer, m_PrevBufferMemory);

	if (m_CurrentBuffer == VK_NULL_HANDLE || m_PrevBuffer == VK_NULL_HANDLE)
	{
		RT_LOG("[ReservoirResources] Failed to allocate reservoir buffers "
		       "(%ux%u, %llu bytes)", width, height,
		       (unsigned long long)m_BufferSize);
		Destroy();
		return;
	}

	RT_LOG("[ReservoirResources] Allocated %ux%u reservoir buffers "
	       "(2 x %llu bytes = %llu bytes total)",
	       width, height,
	       (unsigned long long)m_BufferSize,
	       (unsigned long long)(m_BufferSize * 2));
}

void ReservoirResources::Destroy()
{
	if (m_Device)
	{
		GpuResources::DestroyBuffer(*m_Device, m_CurrentBuffer, m_CurrentBufferMemory);
		GpuResources::DestroyBuffer(*m_Device, m_PrevBuffer,    m_PrevBufferMemory);
	}
	m_CurrentBuffer = VK_NULL_HANDLE;
	m_CurrentBufferMemory = VK_NULL_HANDLE;
	m_PrevBuffer = VK_NULL_HANDLE;
	m_PrevBufferMemory = VK_NULL_HANDLE;
	m_Width = 0;
	m_Height = 0;
	m_BufferSize = 0;
}

void ReservoirResources::Swap()
{
	if (m_CurrentBuffer == VK_NULL_HANDLE)
		return;

	VkBuffer tmpBuf = m_CurrentBuffer;
	VkDeviceMemory tmpMem = m_CurrentBufferMemory;
	m_CurrentBuffer = m_PrevBuffer;
	m_CurrentBufferMemory = m_PrevBufferMemory;
	m_PrevBuffer = tmpBuf;
	m_PrevBufferMemory = tmpMem;
}