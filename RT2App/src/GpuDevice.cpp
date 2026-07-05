#include "GpuDevice.h"
#include "Walnut/Application.h"
#include "VulkanUtils.h"
#include "RTLog.h"

void GpuDevice::InitFromWalnut()
{
	instance       = Walnut::Application::GetInstance();
	physicalDevice = Walnut::Application::GetPhysicalDevice();
	device         = Walnut::Application::GetDevice();
	queue          = Walnut::Application::GetQueue();
	queueFamily    = Walnut::Application::GetQueueFamily();
	descriptorPool = Walnut::Application::GetDescriptorPool();
	rayTracingSupported       = Walnut::Application::IsRayTracingSupported();
	rayTracingPipelineSupported = Walnut::Application::IsRayTracingPipelineSupported();
	rtPipelineProps = Walnut::Application::GetRayTracingPipelineProperties();
}

uint32_t GpuDevice::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const
{
	VkPhysicalDeviceMemoryProperties memProperties;
	vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

	for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++)
	{
		if ((typeFilter & (1u << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
			return i;
	}
	RT_LOG("[GpuDevice] FindMemoryType FAILED: typeFilter=0x%X properties=0x%X", typeFilter, properties);
	return 0xFFFFFFFFu;
}

VkDeviceAddress GpuDevice::GetBufferDeviceAddress(VkBuffer buffer) const
{
	VkBufferDeviceAddressInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
	info.buffer = buffer;
	return vkGetBufferDeviceAddress(device, &info);
}