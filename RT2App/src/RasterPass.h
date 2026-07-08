#pragma once

#ifndef RASTER_PASS_H
#define RASTER_PASS_H

#include "vulkan/vulkan.h"
#include "GpuDevice.h"
#include "GpuResources.h"
#include <cstdint>
#include <vector>

struct GPUSceneData;

// RasterPass — graphics pipeline for primary visibility G-buffer generation.
//
// Rasterizes all instances into the G-buffer images (normal, roughness, viewZ,
// motion, albedo, F0, emission, worldPos, geoNormal, UV) using dynamic rendering.
// The path tracer reads this G-buffer instead of tracing primary rays.
//
// Vertex format: interleaved {vec3 pos, vec2 uv} per vertex (per-BLAS).
// Index buffer: reused from BLAS builds (uint32 indices).
// Instance data: per-draw push constant with instance index → SSBO lookup.
class RasterPass
{
public:
	RasterPass() = default;
	~RasterPass() { Destroy(); }

	// Initialize pipeline (shaders compiled from shaders/ dir)
	bool Init(const GpuDevice& dev, VkDescriptorSetLayout sceneSetLayout,
	          VkDescriptorSetLayout gbufferSetLayout);
	void Destroy();

	// Build raster vertex buffers from the scene's meshes.
	// Called when BLASes are (re)built — one interleaved vertex buffer per mesh.
	void CreateVertexBuffers(const GpuDevice& dev, const GPUSceneData& scene);
	void DestroyVertexBuffers();

	// Build the per-instance draw command buffer (indirect draw data).
	// Called when instances change (scene load or TLAS rebuild).
	void CreateDrawData(const GpuDevice& dev, const GPUSceneData& scene);
	void DestroyDrawData();

	// Record raster pass into command buffer.
	// G-buffer images must be in COLOR_ATTACHMENT_OPTIMAL layout.
	// Depth image must be in DEPTH_STENCIL_ATTACHMENT_OPTIMAL layout.
	// gbufferViews: 8 image views in MRT order (gNormalRoughness, gViewZ, gMotion,
	//               gAlbedoF0, gDirectEmission, gPrimHit, gPrimGeoNormal, gPrimUV)
	void Record(VkCommandBuffer cmd, uint32_t width, uint32_t height,
	            VkDescriptorSet sceneSet, VkDescriptorSet gbufferSet,
	            VkImageView depthView, const VkImageView gbufferViews[8]) const;

	bool IsAvailable() const { return m_Pipeline != VK_NULL_HANDLE; }

private:
	GpuDevice m_Device;
	VkPipeline m_Pipeline = VK_NULL_HANDLE;       // opaque (depthWrite=ON)
	VkPipeline m_MaskedPipeline = VK_NULL_HANDLE;  // alpha-tested (depthWrite=OFF)
	VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;

	// Mega-vertex buffer (all meshes concatenated, per-triangle non-indexed, DEVICE_LOCAL)
	VkBuffer m_MegaVertexBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_MegaVertexMemory = VK_NULL_HANDLE;
	VkBuffer m_MegaVertexStaging = VK_NULL_HANDLE;
	VkDeviceMemory m_MegaVertexStagingMemory = VK_NULL_HANDLE;

	// Per-mesh vertex offset into the mega buffer
	std::vector<uint32_t> m_MeshVertexOffsets;

	// Indirect draw buffers: split into opaque and masked passes
	VkBuffer m_OpaqueDrawBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_OpaqueDrawMemory = VK_NULL_HANDLE;
	uint32_t m_OpaqueDrawCount = 0;

	VkBuffer m_MaskedDrawBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_MaskedDrawMemory = VK_NULL_HANDLE;
	uint32_t m_MaskedDrawCount = 0;

	// Instance index buffer (gl_DrawID → instance index for transform lookup)
	// Stored as a push constant (just the draw ID), actual transforms via SSBO.
};

#endif // !RASTER_PASS_H