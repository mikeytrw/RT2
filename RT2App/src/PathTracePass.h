#pragma once

#include "vulkan/vulkan.h"
#include "GpuResources.h"
#include <cstdint>
#include <vector>

struct GpuDevice;
class AccelerationStructure;

// DescriptorSchema — single source of truth for set 0 binding definitions.
// The layout, binding flags, and descriptor writes are all derived from this table.
struct BindingDef
{
	uint32_t              binding;
	VkDescriptorType      type;
	uint32_t              descriptorCount;  // 1 for non-array, MAX_TEXTURES for texture array
	VkShaderStageFlags    stageFlags;
	VkDescriptorBindingFlagsEXT flags;
};

// PathTracePass — owns the RT pipeline, SBT, and set 0 descriptor layout.
// Set 0 bindings (see shader_interface.h):
//   0: output image, 1: camera UBO, 2: materials, 3: vertex buffer, 4: TLAS,
//   5: index buffer, 6: normals, 7: UVs, 8: instance mesh info, 9: lights,
//   10: instance transforms, 11: GI data, 12: transforms prev,
//   13: instance material indices, 14: reservoir history, 15: reservoir scratch,
//   16: surface history, 17: instance mat offsets, 18: texture array (variable count)
class PathTracePass
{
public:
	static constexpr uint32_t MAX_TEXTURES = 4096;

	PathTracePass() = default;
	~PathTracePass() { Destroy(); }

	bool Init(const GpuDevice& dev, VkDescriptorSetLayout gbufferSetLayout);
	void Destroy();

	// Allocate (or re-allocate) the descriptor set with space for the given
	// texture count. If a set is already allocated with >= textureCount slots,
	// this is a no-op. Otherwise the old set is freed and a new one allocated.
	// Returns false on allocation failure.
	bool CreateDescriptorSet(const GpuDevice& dev, uint32_t textureCount);

	// Free the descriptor set (for resize / destroy). Caller must ensure
	// GPU is idle before calling.
	void FreeDescriptorSet();

	void UpdateDescriptorSet(const GpuDevice& dev,
		VkImageView outputView, VkSampler outputSampler,
		VkBuffer cameraUBO, VkBuffer materialBuffer,
		VkBuffer vertexBuffer, VkBuffer indexBuffer,
		VkBuffer normalBuffer, VkBuffer uvBuffer,
		VkBuffer instanceMeshInfoBuffer,
		VkBuffer lightBuffer, VkBuffer punctualLightBuffer,
		VkBuffer instanceTransformBuffer,
		VkBuffer instanceTransformPrevBuffer,
		VkBuffer instanceMaterialIndexBuffer,
		VkBuffer instanceMatOffsetBuffer,
		VkAccelerationStructureKHR tlas,
		VkBuffer reservoirBuffer, VkBuffer reservoirScratchBuffer,
		VkBuffer surfaceHistoryBuffer,
		VkBuffer giDataBuffer,
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

	// Number of texture slots the current descriptor set was allocated with.
	uint32_t GetAllocatedTextureCount() const { return m_AllocatedTextureCount; }

private:
	VkDevice m_Device = VK_NULL_HANDLE;

	// Pipeline + layout
	VkPipeline m_Pipeline = VK_NULL_HANDLE;
	VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_DescriptorSetLayout = VK_NULL_HANDLE;

	// Dedicated descriptor pool for set 0 (separate from Walnut/ImGui pool)
	VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;

	// Set 0 descriptor set
	VkDescriptorSet m_DescriptorSet = VK_NULL_HANDLE;
	uint32_t m_AllocatedTextureCount = 0;  // texture slots in the current set

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