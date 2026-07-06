#include "FrameContext.h"
#include "VulkanUtils.h"

void FrameContext::Init(VkDevice device, uint32_t queueFamily)
{
	// Command pool
	VkCommandPoolCreateInfo poolInfo = {};
	poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	poolInfo.queueFamilyIndex = queueFamily;
	VK_CHECK(vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool));

	// Command buffer
	VkCommandBufferAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.commandPool = commandPool;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = 1;
	VK_CHECK(vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer));

	// Fence — start signaled so first WaitForFence doesn't block
	VkFenceCreateInfo fenceInfo = {};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	VK_CHECK(vkCreateFence(device, &fenceInfo, nullptr, &renderFence));
	fenceSignaled = true;
}

void FrameContext::Destroy(VkDevice device)
{
	if (renderFence)   { vkDestroyFence(device, renderFence, nullptr);   renderFence = VK_NULL_HANDLE; }
	if (commandBuffer) { vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer); commandBuffer = VK_NULL_HANDLE; }
	if (commandPool)   { vkDestroyCommandPool(device, commandPool, nullptr); commandPool = VK_NULL_HANDLE; }
	fenceSignaled = false;
}

void FrameContext::WaitForFence(VkDevice device)
{
	if (!fenceSignaled) return;  // nothing in flight
	VK_CHECK(vkWaitForFences(device, 1, &renderFence, VK_TRUE, UINT64_MAX));
	VK_CHECK(vkResetFences(device, 1, &renderFence));
	fenceSignaled = false;
}

void FrameContext::Begin(VkDevice device)
{
	VK_CHECK(vkResetCommandPool(device, commandPool, 0));
	VkCommandBufferBeginInfo beginInfo = {};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VK_CHECK(vkBeginCommandBuffer(commandBuffer, &beginInfo));
}

void FrameContext::Submit(VkQueue queue)
{
	VK_CHECK(vkEndCommandBuffer(commandBuffer));
	VkSubmitInfo submitInfo = {};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &commandBuffer;
	VK_CHECK(vkQueueSubmit(queue, 1, &submitInfo, renderFence));
	fenceSignaled = true;
}