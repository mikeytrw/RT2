#pragma once

#ifndef GPU_RENDERER_H
#define GPU_RENDERER_H

#include "Utility.h"
#include <memory>
#include <vector>
#include <vulkan/vulkan.h>
#include "Walnut/Image.h"

class GpuRenderer {
public:
    GpuRenderer();
    ~GpuRenderer();

    void OnResize(uint32_t width, uint32_t height);
    void Render(Camera& camera);

    std::shared_ptr<Walnut::Image> GetFinalImage() const { return m_FinalImage; }
    void setTemporalAccumulation(bool enabled) { m_TemporalAccumulation = enabled; }

private:
    struct SphereData
    {
        glm::vec3 Center{0.0f};
        float Radius = 0.0f;
    };

    struct InstanceData
    {
        uint32_t BlasIndex = 0;
    };

    std::vector<SphereData> m_BlasGeometries;
    std::vector<InstanceData> m_TlasInstances;
    void BuildBLAS();
    void BuildTLAS();
    void CreateRayTracingPipeline();
    void CreateShaderBindingTable();
    void CreateOutputImage(uint32_t width, uint32_t height);

private:
    std::shared_ptr<Walnut::Image> m_FinalImage;
    HittableList m_World;

    bool m_TemporalAccumulation = false;

    // Vulkan handles
    VkDevice m_Device = VK_NULL_HANDLE;
    VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
    VkQueue m_Queue = VK_NULL_HANDLE;
    VkPipeline m_Pipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_DescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet m_DescriptorSet = VK_NULL_HANDLE;
    VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;
    VkShaderModule m_ComputeShader = VK_NULL_HANDLE;
    VkBuffer m_StorageBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_StorageBufferMemory = VK_NULL_HANDLE;
    uint32_t m_ImageWidth = 0, m_ImageHeight = 0;
};

#endif // GPU_RENDERER_H
