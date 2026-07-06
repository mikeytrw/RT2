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
	void Record(VkCommandBuffer cmd, uint32_t width, uint32_t height,
	            VkDescriptorSet sceneSet, VkDescriptorSet gbufferSet,
	            VkImageView depthView) const;

	bool IsAvailable() const { return m_Pipeline != VK_NULL_HANDLE; }

private:
	GpuDevice m_Device;
	VkPipeline m_Pipeline = VK_NULL_HANDLE;
	VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;

	// Single mega-vertex buffer (all meshes concatenated, per-triangle non-indexed)
	VkBuffer m_MegaVertexBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_MegaVertexMemory = VK_NULL_HANDLE;

	// Per-mesh vertex offset into the mega buffer
	std::vector<uint32_t> m_MeshVertexOffsets;

	// Indirect draw buffer: one VkDrawIndirectCommand per instance
	VkBuffer m_DrawBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_DrawMemory = VK_NULL_HANDLE;
	uint32_t m_DrawCount = 0;

	// Instance index buffer (gl_DrawID → instance index for transform lookup)
	// Stored as a push constant (just the draw ID), actual transforms via SSBO.
};

#endif // !RASTER_PASS_H