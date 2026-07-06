#include "StagingArena.h"
#include "GpuDevice.h"
#include "VulkanUtils.h"
#include "RTLog.h"

StagingArena::~StagingArena()
{
	Destroy();
}

bool StagingArena::Init(const GpuDevice& dev, VkDeviceSize size)
{
	Destroy();

	if (size == 0)
		return false;

	VkBufferCreateInfo bufferInfo = {};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = size;
	bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VK_CHECK(vkCreateBuffer(dev.device, &bufferInfo, nullptr, &m_Buffer));

	VkMemoryRequirements memReqs;
	vkGetBufferMemoryRequirements(dev.device, m_Buffer, &memReqs);

	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memReqs.size;
	allocInfo.memoryTypeIndex = dev.FindMemoryType(memReqs.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (allocInfo.memoryTypeIndex == 0xFFFFFFFF)
	{
		RT_LOG("[StagingArena] no valid memory type");
		vkDestroyBuffer(dev.device, m_Buffer, nullptr);
		m_Buffer = VK_NULL_HANDLE;
		return false;
	}

	VK_CHECK(vkAllocateMemory(dev.device, &allocInfo, nullptr, &m_Memory));
	VK_CHECK(vkBindBufferMemory(dev.device, m_Buffer, m_Memory, 0));

	VK_CHECK(vkMapMemory(dev.device, m_Memory, 0, VK_WHOLE_SIZE, 0, &m_Mapped));

	m_Device  = dev.device;
	m_Capacity = memReqs.size;  // may be larger than requested due to alignment
	m_Cursor   = 0;

	return true;
}

void StagingArena::Destroy()
{
	if (m_Mapped)
	{
		vkUnmapMemory(m_Device, m_Memory);
		m_Mapped = nullptr;
	}
	if (m_Buffer)
	{
		vkDestroyBuffer(m_Device, m_Buffer, nullptr);
		m_Buffer = VK_NULL_HANDLE;
	}
	if (m_Memory)
	{
		vkFreeMemory(m_Device, m_Memory, nullptr);
		m_Memory = VK_NULL_HANDLE;
	}
	m_Device   = VK_NULL_HANDLE;
	m_Capacity = 0;
	m_Cursor   = 0;
}

VkDeviceSize StagingArena::Alloc(VkDeviceSize size, VkDeviceSize alignment)
{
	if (!IsValid() || size == 0)
		return VK_WHOLE_SIZE;

	// Align cursor up
	VkDeviceSize aligned = (m_Cursor + alignment - 1) & ~(alignment - 1);

	if (aligned + size > m_Capacity)
	{
		RT_LOG("[StagingArena] out of space: requested %zu at offset %zu, capacity %zu",
		       (size_t)size, (size_t)aligned, (size_t)m_Capacity);
		return VK_WHOLE_SIZE;
	}

	m_Cursor = aligned + size;
	return aligned;
}