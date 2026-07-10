#pragma once

#include "vulkan/vulkan.h"
#include "GpuResources.h"
#include "GPUSceneData.h"
#include "AccelerationStructure.h"
#include "AsyncTextureLoader.h"
#include <vector>
#include <cstdint>

struct GpuDevice;

// SceneResources — owns all per-scene GPU resources:
// - Material buffer (SSBO, host-visible)
// - Light buffer (SSBO, host-visible, std430 with 16-byte header)
// - Instance transform buffer (current + prev, for motion vectors)
// - Instance material index buffer (for raster pass)
// - Texture array (bindless, scene textures + CDF textures appended)
// - Texture samplers (scene + CDF)
// - Texture descriptor set (set 2, bindless)
// - Acceleration structure (BLAS + TLAS)
// - Env map CDF indices + metadata
//
// Does NOT own: camera UBO, output image, G-buffer images (those are
// frame-level resources owned by RendererGPU / GBufferTarget).
class SceneResources
{
public:
	SceneResources() = default;
	~SceneResources();

	SceneResources(const SceneResources&) = delete;
	SceneResources& operator=(const SceneResources&) = delete;

	// Initialize the texture descriptor set layout + pool. Must be called
	// once after device init, before SetScene.
	void InitDescriptorSet(const GpuDevice& dev) {} // no-op (textures in PathTracePass set 0)

	// Create samplers. Must be called once after device init.
	void InitSamplers(const GpuDevice& dev);

	// Upload scene data: textures, CDF textures, set m_NeedsASRebuild=true.
	// Does NOT build AS or create material/transform buffers — call
	// RebuildAccelerationStructures() for that (needs RasterPass too).
	// Textures are uploaded asynchronously via AsyncTextureLoader; poll
	// PollTextureUpload() each frame and call UpdatePathTraceDescriptorSet
	// (via RendererGPU) when it returns true.
	void SetScene(const GpuDevice& dev, const GPUSceneData& sceneData);

	// Update scene data (meshes, instances, materials, lights) WITHOUT
	// re-uploading textures. Use this when only entities/transforms/materials
	// changed (add/delete entity, material edit) but textures are unchanged.
	// Marks AS for rebuild. Existing textures + descriptor set remain valid.
	void SetSceneKeepTextures(const GpuDevice& dev, const GPUSceneData& sceneData);

	// Returns true if async texture upload just completed (once per upload).
	// Caller should update the texture descriptor set when this returns true.
	bool PollTextureUpload();

	// Rebuild BLAS + TLAS, create material/light/transform buffers,
	// build raster vertex buffers + draw data.
	// rasterPass is a callback that receives the GpuDevice + sceneData
	// to call RasterPass::CreateVertexBuffers/CreateDrawData.
	void RebuildAccelerationStructures(const GpuDevice& dev,
		std::function<void(const GpuDevice&, const GPUSceneData&)> rasterPassBuild);

	// Update instances (TLAS rebuild only) + transform/light buffers.
	void UpdateInstances(const GpuDevice& dev, const GPUSceneData& sceneData);

	// Destroy all GPU resources (textures, buffers, AS, samplers, descriptor).
	void Destroy();

	bool IsValid() const { return m_AS.IsValid(); }
	bool NeedsASRebuild() const { return m_NeedsASRebuild; }
	void ClearASRebuildFlag() { m_NeedsASRebuild = false; }
	bool ASJustBuilt() const { return m_ASJustBuilt; }
	void ClearASJustBuilt() { m_ASJustBuilt = false; }
	bool NeedsTransformAdvance() const { return m_NeedsTransformAdvance; }
	void ClearTransformAdvance() { m_NeedsTransformAdvance = false; }

	// Copy current transform buffer → prev transform buffer (via cmd buffer).
	// Call on the frame after a scene edit so motion vectors go to zero.
	void AdvanceTransformBuffers(VkCommandBuffer cmd);

	const GPUSceneData& GetScene() const { return m_CurrentScene; }

	// Buffer accessors (for descriptor set updates)
	VkBuffer GetMaterialBuffer() const { return m_MaterialBuffer; }
	VkBuffer GetLightBuffer() const { return m_LightBuffer; }
	VkBuffer GetInstanceTransformBuffer() const { return m_InstanceTransformBuffer; }
	VkBuffer GetInstanceTransformPrevBuffer() const { return m_InstanceTransformPrevBuffer; }
	VkBuffer GetInstanceMaterialIndexBuffer() const { return m_InstanceMaterialIndexBuffer; }

	// AS accessors
	VkAccelerationStructureKHR GetTLAS() const { return m_AS.GetTLAS(); }
	VkBuffer GetNormalBuffer() const { return m_AS.GetNormalBuffer(); }
	VkBuffer GetTangentBuffer() const { return m_AS.GetTangentBuffer(); }
	VkBuffer GetUVBuffer() const { return m_AS.GetUVBuffer(); }
	VkBuffer GetPositionBuffer() const { return m_AS.GetPositionBuffer(); }
	VkBuffer GetInstanceOffsetBuffer() const { return m_AS.GetInstanceOffsetBuffer(); }

	// Texture accessors
	const std::vector<GpuImage>& GetTextures() const { return m_Textures; }
	VkSampler GetTextureSampler() const { return m_TextureSampler; }

	// True if an async texture upload is in flight.
	bool IsTextureUploadPending() const { return m_TextureLoader.IsBusy(); }

	// Debug: dump GPU instance transform buffer contents to log.
	// Reads back instanceTransforms[] and instanceTransformsPrev[] from GPU
	// memory and prints the world position (translation column) for each
	// instance, plus material index from the material index buffer.
	void DumpInstanceTransforms() const;
	void DumpNEEBuffers() const;

	// Env map metadata
	int GetEnvMapIndex() const { return m_EnvMapIndex; }
	int GetMarginalCDFIndex() const { return m_MarginalCDFIndex; }
	int GetConditionalCDFIndex() const { return m_ConditionalCDFIndex; }

private:
	void CreateMaterialBuffer(const GpuDevice& dev);
	void CreateLightBuffer(const GpuDevice& dev);
	void CreateInstanceTransformBuffer(const GpuDevice& dev);
	void CreateTextures(const GpuDevice& dev, const std::vector<SceneTexture>& textures);
	void DestroyTextures();
	void CreateEnvMapCDFTextures(const GpuDevice& dev, const GPUSceneData& sceneData);
	void DestroyEnvMapCDFTextures();

	GpuDevice const* m_Device = nullptr;

	// Material buffer
	VkBuffer m_MaterialBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_MaterialBufferMemory = VK_NULL_HANDLE;

	// Light buffer (std430: 16-byte header + GPUTriangleLight[])
	VkBuffer m_LightBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_LightBufferMemory = VK_NULL_HANDLE;

	// Instance transforms (current + prev)
	VkBuffer m_InstanceTransformBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_InstanceTransformBufferMemory = VK_NULL_HANDLE;
	VkBuffer m_InstanceTransformPrevBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_InstanceTransformPrevBufferMemory = VK_NULL_HANDLE;
	VkDeviceSize m_InstanceTransformBufferSize = 0;

	// Per-instance material index
	VkBuffer m_InstanceMaterialIndexBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_InstanceMaterialIndexBufferMemory = VK_NULL_HANDLE;

	// Textures (bindless array — scene textures + CDF textures appended)
	std::vector<GpuImage> m_Textures;
	VkSampler m_TextureSampler = VK_NULL_HANDLE;

	// Async texture loader (background decode + GPU upload)
	AsyncTextureLoader m_TextureLoader;

	// Env map CDF metadata
	int m_MarginalCDFIndex = -1;
	int m_ConditionalCDFIndex = -1;
	int m_EnvMapIndex = -1;
	int m_CDFWidth = 0;
	int m_CDFHeight = 0;

	// Acceleration structure
	AccelerationStructure m_AS;
	bool m_NeedsASRebuild = false;
	bool m_ASJustBuilt = false;
	bool m_NeedsTransformAdvance = false;

	// Current scene data (CPU-side mirror)
	GPUSceneData m_CurrentScene;
};