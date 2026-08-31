#pragma once

#include "vulkan/vulkan.h"
#include <cstdint>
#include "RenderExtents.h"

struct GpuDevice;

// Converts the linear HDR accumulation image to an LDR display image. The
// linear image remains untouched so temporal accumulation never averages
// tone-mapped values.
class TonemapPass
{
public:
    ~TonemapPass() { Destroy(); }

    bool Init(const GpuDevice& dev);
    void Destroy();
    void UpdateDescriptorSet(const GpuDevice& dev, VkImageView inputView, VkImageView outputView);
    void Record(VkCommandBuffer cmd, const OutputExtent& extent) const;

    bool IsAvailable() const { return m_Pipeline != VK_NULL_HANDLE; }

private:
    VkPipeline m_Pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_SetLayout = VK_NULL_HANDLE;
    VkDescriptorSet m_DescriptorSet = VK_NULL_HANDLE;
    VkDescriptorPool m_Pool = VK_NULL_HANDLE;
    VkShaderModule m_Shader = VK_NULL_HANDLE;
    VkDevice m_Device = VK_NULL_HANDLE;
};
