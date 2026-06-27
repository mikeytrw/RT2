#pragma once

#ifndef RENDERER_GPU_H
#define RENDERER_GPU_H

#include "vulkan/vulkan.h"
#include "AccelerationStructure.h"
#include "Camera.h"
#include "GPUSceneData.h"
#include "Scene.h"
#include <memory>

// A single GPU texture (VkImage + view + sampler + staging)
struct GPUTexture
{
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VkSampler sampler = VK_NULL_HANDLE;
	int width = 0;
	int height = 0;
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
	void SetScene(const GPUSceneData& sceneData);

	VkDescriptorSet GetOutputDescriptorSet() const { return m_ImGuiDescriptorSet; }
	bool HasOutput() const { return m_OutputImage != VK_NULL_HANDLE; }
	uint32_t GetWidth() const { return m_Width; }
	uint32_t GetHeight() const { return m_Height; }

	bool Init();
	void RebuildAccelerationStructures();
	void ResetAccumulation();

	int m_SPP = 5;
	int m_MaxBounces = 8;
	bool m_ShowBackground = false;
	bool m_NEEOnly = false;
	float m_EmissiveBoost = 1.0f;

private:
	void CreateOutputImage();
	void DestroyOutputImage();
	void CreatePipeline();
	void DestroyPipeline();
	void CreateDescriptorSet();
	void UpdateDescriptorSet();
	void CreateMaterialBuffer();
	void CreateLightBuffer();
	void UpdateCameraUBO(const Camera& camera);
	void CreateTextures(const std::vector<SceneTexture>& textures);
	void DestroyTextures();

	bool m_Initialized = false;

	uint32_t m_Width = 0;
	uint32_t m_Height = 0;

	VkImage m_OutputImage = VK_NULL_HANDLE;
	VkDeviceMemory m_OutputMemory = VK_NULL_HANDLE;
	VkImageView m_OutputImageView = VK_NULL_HANDLE;
	VkSampler m_Sampler = VK_NULL_HANDLE;
	VkDescriptorSet m_ImGuiDescriptorSet = VK_NULL_HANDLE;

	// Ray tracing pipeline + SBT
	VkPipeline m_RTPipeline = VK_NULL_HANDLE;
	VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_DescriptorSetLayout = VK_NULL_HANDLE;
	VkDescriptorSet m_DescriptorSet = VK_NULL_HANDLE;

	VkShaderModule m_RgenShader   = VK_NULL_HANDLE;
	VkShaderModule m_MissShader    = VK_NULL_HANDLE;
	VkShaderModule m_ShadowShader  = VK_NULL_HANDLE;
	VkShaderModule m_ClosestShader = VK_NULL_HANDLE;
	VkShaderModule m_AnyHitShader  = VK_NULL_HANDLE;
	VkShaderModule m_ShadowHitShader = VK_NULL_HANDLE;

	VkBuffer m_SBTBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_SBTMemory = VK_NULL_HANDLE;
	VkDeviceSize m_SBTSize = 0;
	VkDeviceSize m_SBTStride = 0;
	VkDeviceSize m_RgenRegionSize = 0;
	VkDeviceSize m_MissRegionSize = 0;  // covers both miss entries (sky + shadow)
	VkDeviceSize m_HitRegionSize = 0;
	uint32_t m_MaxRecursionDepth = 1;

	VkBuffer m_CameraUBO = VK_NULL_HANDLE;
	VkDeviceMemory m_CameraUBOMemory = VK_NULL_HANDLE;

	VkBuffer m_MaterialBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_MaterialBufferMemory = VK_NULL_HANDLE;
	VkDeviceSize m_MaterialBufferSize = 0;

	// Light buffer (NEE) — std430 with 16-byte header + TriangleLight[]
	VkBuffer m_LightBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_LightBufferMemory = VK_NULL_HANDLE;
	VkDeviceSize m_LightBufferSize = 0;

	// Textures (bindless array)
	std::vector<GPUTexture> m_Textures;
	VkDescriptorSetLayout m_TextureDescriptorSetLayout = VK_NULL_HANDLE;
	VkDescriptorSet m_TextureDescriptorSet = VK_NULL_HANDLE;

	AccelerationStructure m_AS;

	GPUSceneData m_CurrentScene;
	bool m_NeedsASRebuild = false;
	bool m_ASJustBuilt = false;

	uint32_t m_FrameIndex = 1;
	VkImageLayout m_OutputImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	void CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
	                  VkBuffer& buffer, VkDeviceMemory& memory);
	void DestroyBuffer(VkBuffer buffer, VkDeviceMemory memory);
	VkDeviceAddress GetBufferDeviceAddress(VkBuffer buffer);
	uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
};

#endif // !RENDERER_GPU_H