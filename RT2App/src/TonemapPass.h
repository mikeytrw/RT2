#pragma once

#include "vulkan/vulkan.h"
#include <cstdint>
#include "RenderExtents.h"
#include "TonemapPushConstants.h"

struct GpuDevice;

// Converts the linear HDR accumulation image to an LDR display image. The
// linear image remains untouched so temporal accumulation never averages
// tone-mapped values. The camera look travels as 16 bytes of push constants
// (TonemapPushConstants); no per-operator pipelines, no extra images.
class TonemapPass
{
public:
    ~TonemapPass() { Destroy(); }

    bool Init(const GpuDevice& dev);
    void Destroy();
    void UpdateDescriptorSet(const GpuDevice& dev, VkImageView inputView, VkImageView outputView);
    // Checked record: pushes the resolved presentation and dispatches.
    // Returns false (leaving nothing recorded) when the selected pipeline,
    // layout, descriptor set or push-constant state is missing, so a broken
    // tone-map stage can never report a successful capture.
    bool Record(VkCommandBuffer cmd, const OutputExtent& extent,
                const TonemapPushConstants& pc, bool useRR = false) const;

    bool IsAvailable() const { return m_Pipeline != VK_NULL_HANDLE; }
    bool IsRRTonemapAvailable() const { return m_RRPipeline != VK_NULL_HANDLE; }
    bool HasPushConstants() const { return m_PushConstantBytes == sizeof(TonemapPushConstants); }

private:
    VkPipeline m_Pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_SetLayout = VK_NULL_HANDLE;
    VkDescriptorSet m_DescriptorSet = VK_NULL_HANDLE;
    VkDescriptorPool m_Pool = VK_NULL_HANDLE;
    VkShaderModule m_Shader = VK_NULL_HANDLE;
    VkShaderModule m_RRShader = VK_NULL_HANDLE;
    VkPipeline m_RRPipeline = VK_NULL_HANDLE;
    VkImageView m_BoundInputView = VK_NULL_HANDLE;
    VkImageView m_BoundOutputView = VK_NULL_HANDLE;
    VkDevice m_Device = VK_NULL_HANDLE;
    uint32_t m_PushConstantBytes = 0;
};
