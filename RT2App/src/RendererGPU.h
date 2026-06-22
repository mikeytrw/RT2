#pragma once

#ifndef RENDERER_GPU_H
#define RENDERER_GPU_H

#include "vulkan/vulkan.h"
#include "AccelerationStructure.h"
#include "Camera.h"
#include "Material.h"
#include <memory>

struct GPUMeshData
{
	std::vector<float> vertices;
	std::vector<uint32_t> indices;
	int materialType;
	glm::vec3 albedo;
	float fuzz;
	float ior;
	glm::vec3 position;
	glm::vec3 rotation;
	float scale;
};

class RendererGPU
{
public:
	RendererGPU() = default;
	~RendererGPU() { Destroy(); }

	void Destroy();

	bool IsAvailable() const { return m_Initialized; }

	void OnResize(uint32_t width, uint32_t height);
	void Render(const Camera& camera);
	void SetMesh(const GPUMeshData& meshData);

	VkDescriptorSet GetOutputDescriptorSet() const { return m_ImGuiDescriptorSet; }
	bool HasOutput() const { return m_OutputImage != VK_NULL_HANDLE; }
	uint32_t GetWidth() const { return m_Width; }
	uint32_t GetHeight() const { return m_Height; }

	bool Init();
	void RebuildAccelerationStructures();
	void ResetAccumulation();

	int m_SPP = 5;
	int m_MaxBounces = 8;

private:
	void CreateOutputImage();
	void DestroyOutputImage();
	void CreatePipeline();
	void DestroyPipeline();
	void CreateDescriptorSet();
	void UpdateDescriptorSet();
	void CreateMaterialBuffer();
	void UpdateCameraUBO(const Camera& camera);

	bool m_Initialized = false;

	uint32_t m_Width = 0;
	uint32_t m_Height = 0;

	VkImage m_OutputImage = VK_NULL_HANDLE;
	VkDeviceMemory m_OutputMemory = VK_NULL_HANDLE;
	VkImageView m_OutputImageView = VK_NULL_HANDLE;
	VkSampler m_Sampler = VK_NULL_HANDLE;
	VkDescriptorSet m_ImGuiDescriptorSet = VK_NULL_HANDLE;

	VkPipeline m_ComputePipeline = VK_NULL_HANDLE;
	VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_DescriptorSetLayout = VK_NULL_HANDLE;
	VkDescriptorSet m_DescriptorSet = VK_NULL_HANDLE;
	VkShaderModule m_ShaderModule = VK_NULL_HANDLE;

	VkBuffer m_CameraUBO = VK_NULL_HANDLE;
	VkDeviceMemory m_CameraUBOMemory = VK_NULL_HANDLE;

	VkBuffer m_MaterialBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_MaterialBufferMemory = VK_NULL_HANDLE;

	AccelerationStructure m_AS;

	GPUMeshData m_CurrentMesh;
	bool m_NeedsASRebuild = false;

	uint32_t m_FrameIndex = 1;
	bool m_TemporalAccumulation = false;

	void CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
	                  VkBuffer& buffer, VkDeviceMemory& memory);
	void DestroyBuffer(VkBuffer buffer, VkDeviceMemory memory);
	uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
};

#endif // !RENDERER_GPU_H