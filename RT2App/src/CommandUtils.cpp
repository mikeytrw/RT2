#include "CommandUtils.h"
#include "GpuDevice.h"
#include "VulkanUtils.h"

namespace CommandUtils
{
	void ImmediateSubmit(const GpuDevice& dev, std::function<void(VkCommandBuffer)> record)
	{
		VkDevice device = dev.device;

		// Create transient command pool
		VkCommandPoolCreateInfo poolInfo = {};
		poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
		poolInfo.queueFamilyIndex = dev.queueFamily;

		VkCommandPool pool;
		VK_CHECK(vkCreateCommandPool(device, &poolInfo, nullptr, &pool));

		// Allocate command buffer
		VkCommandBufferAllocateInfo allocInfo = {};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.commandPool = pool;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = 1;

		VkCommandBuffer cmd;
		VK_CHECK(vkAllocateCommandBuffers(device, &allocInfo, &cmd));

		// Begin recording
		VkCommandBufferBeginInfo beginInfo = {};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		VK_CHECK(vkBeginCommandBuffer(cmd, &beginInfo));

		// Record user work
		record(cmd);

		// End + submit
		VK_CHECK(vkEndCommandBuffer(cmd));

		VkSubmitInfo submitInfo = {};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &cmd;
		VK_CHECK(vkQueueSubmit(dev.queue, 1, &submitInfo, VK_NULL_HANDLE));

		// Wait + cleanup
		VK_CHECK(vkQueueWaitIdle(dev.queue));
		vkFreeCommandBuffers(device, pool, 1, &cmd);
		vkDestroyCommandPool(device, pool, nullptr);
	}
}