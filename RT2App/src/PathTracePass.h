#pragma once

#include "vulkan/vulkan.h"
#include "GpuResources.h"
#include <cstdint>
#include <vector>

struct GpuDevice;
class AccelerationStructure;

// PathTracePass — owns the RT pipeline, SBT, and set 0 descriptor layout.
// Set 0 bindings (see shader_interface.h):
//   0: output image, 1: camera UBO, 2: materials, 3: normals, 4: TLAS,
//   5: instance offsets, 6: tangents, 7: UVs, 8: positions, 9: lights,
//   10: instance transforms, 11: textures
class PathTracePass
{
public:
	PathTracePass() = default;
	~PathTracePass() { Destroy(); }

	bool Init(const GpuDevice& dev, VkDescriptorSetLayout gbufferSetLayout);
	void Destroy();

	// Create/allocate set 0 descriptor set (needs descriptor pool from caller)
	bool CreateDescriptorSet(const GpuDevice& dev, VkDescriptorPool pool);
	void UpdateDescriptorSet(const GpuDevice& dev,
		VkImageView outputView, VkSampler outputSampler,
		VkBuffer cameraUBO, VkBuffer materialBuffer,
		VkBuffer normalBuffer, VkBuffer instanceOffsetBuffer,
		VkBuffer tangentBuffer, VkBuffer uvBuffer, VkBuffer positionBuffer,
		VkBuffer lightBuffer, VkBuffer instanceTransformBuffer,
		VkBuffer instanceTransformPrevBuffer,
		VkBuffer instanceMaterialIndexBuffer,
		VkAccelerationStructureKHR tlas,
		VkBuffer reservoirBuffer, VkBuffer reservoirPrevBuffer,
		const std::vector<VkDescriptorImageInfo>& textureImageInfos);

	// Record trace into command buffer. Caller handles pre/post barriers.
	// rasterFirst=true: use secondary_raygen (reads G-buffer, traces secondary rays only)
	// rasterFirst=false: use raygen (traces primary ray — RT-primary path, accumulation-only)
	void Record(VkCommandBuffer cmd, uint32_t width, uint32_t height,
	            VkDescriptorSet gbufferSet, bool rasterFirst = false) const;

	bool IsAvailable() const { return m_Pipeline != VK_NULL_HANDLE; }
	VkDescriptorSet GetDescriptorSet() const { return m_DescriptorSet; }
	VkDescriptorSetLayout GetDescriptorSetLayout() const { return m_DescriptorSetLayout; }
	VkPipelineLayout GetPipelineLayout() const { return m_PipelineLayout; }
	VkPipeline GetPipeline() const { return m_Pipeline; }
	VkBuffer GetSBTBuffer() const { return m_SBTBuffer.buffer; }
	VkDeviceSize GetSBTSize() const { return m_SBTSize; }
	VkDeviceSize GetSBTStride() const { return m_SBTStride; }
	VkDeviceSize GetRgenRegionSize() const { return m_RgenRegionSize; }
	VkDeviceSize GetMissRegionSize() const { return m_MissRegionSize; }
	VkDeviceSize GetHitRegionSize() const { return m_HitRegionSize; }

	uint32_t GetMaxRecursionDepth() const { return m_MaxRecursionDepth; }

	// Free the descriptor set (for resize). Caller provides the pool.
	void FreeDescriptorSet(VkDevice device, VkDescriptorPool pool);

private:
	VkDevice m_Device = VK_NULL_HANDLE;

	// Pipeline + layout
	VkPipeline m_Pipeline = VK_NULL_HANDLE;
	VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_DescriptorSetLayout = VK_NULL_HANDLE;

	// Set 0 descriptor set
	VkDescriptorSet m_DescriptorSet = VK_NULL_HANDLE;

	// Shader modules
	VkShaderModule m_RgenShader = VK_NULL_HANDLE;
	VkShaderModule m_SecondaryRgenShader = VK_NULL_HANDLE;  // raster-first path
	VkShaderModule m_MissShader = VK_NULL_HANDLE;
	VkShaderModule m_ShadowShader = VK_NULL_HANDLE;
	VkShaderModule m_ClosestShader = VK_NULL_HANDLE;
	VkShaderModule m_AnyHitShader = VK_NULL_HANDLE;
	VkShaderModule m_ShadowHitShader = VK_NULL_HANDLE;

	// SBT
	GpuBuffer m_SBTBuffer;
	VkDeviceSize m_SBTSize = 0;
	VkDeviceSize m_SBTStride = 0;
	VkDeviceSize m_RgenRegionSize = 0;
	VkDeviceSize m_MissRegionSize = 0;
	VkDeviceSize m_HitRegionSize = 0;
	uint32_t m_MaxRecursionDepth = 1;
};