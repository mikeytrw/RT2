#pragma once

#include "vulkan/vulkan.h"
#include <cstdint>

struct GpuDevice;

// ComposePass — compute shader that remodulates NRD diff/spec outputs
// by material factors, adds direct emission, writes to beauty output image.
//
// Bindings: 0=output, 1=nrdDiffOut, 2=nrdSpecOut, 3=albedoF0, 4=directEmission,
//           5=viewZ, 6=normalRoughness
class ComposePass
{
public:
	ComposePass() = default;
	~ComposePass() { Destroy(); }

	bool Init(const GpuDevice& dev);
	void Destroy();

	void OnResize(const GpuDevice& dev, uint32_t width, uint32_t height);
	void UpdateDescriptorSet(const GpuDevice& dev,
	                         VkImageView outputView,
	                         VkImageView nrdDiffOutView,
	                         VkImageView nrdSpecOutView,
	                         VkImageView albedoF0View,
	                         VkImageView directEmissionView,
	                         VkImageView viewZView,
	                         VkImageView normalRoughnessView,
	                         VkBuffer cameraUBO);

	// Record the compose dispatch into the given command buffer.
	// Caller is responsible for barriers before/after.
	void Record(VkCommandBuffer cmd, uint32_t width, uint32_t height) const;

	bool IsAvailable() const { return m_Pipeline != VK_NULL_HANDLE; }
	VkDescriptorSet GetDescriptorSet() const { return m_DescriptorSet; }

private:
	VkPipeline         m_Pipeline       = VK_NULL_HANDLE;
	VkPipelineLayout   m_PipelineLayout = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_SetLayout   = VK_NULL_HANDLE;
	VkDescriptorSet    m_DescriptorSet  = VK_NULL_HANDLE;
	VkDescriptorPool   m_Pool           = VK_NULL_HANDLE;
	VkShaderModule     m_Shader         = VK_NULL_HANDLE;
	VkDevice           m_Device         = VK_NULL_HANDLE; // cached for Destroy
};