#pragma once

#ifndef ACCELERATION_STRUCTURE_H
#define ACCELERATION_STRUCTURE_H

#include "vulkan/vulkan.h"
#include "GpuDevice.h"
#include <glm/glm.hpp>
#include <vector>
#include <string>

struct BLASInstance
{
	VkDeviceAddress blasAddress;
	uint32_t customIndex; // material index
	VkTransformMatrixKHR transform;
	uint32_t sbtHitOffset = 0; // 0 = opaque hit group, 1 = alpha hit group
};

// Geometry for a single BLAS build.
struct BLASGeometry
{
	const std::vector<float>*    vertices;    // position.xyz, stride 3
	const std::vector<uint32_t>* indices;     // triangle indices
	const std::vector<float>*    vertexUVs;   // 6 floats per triangle (3 UVs × xy)
	const std::vector<float>*    tangents;    // 12 floats per triangle (3 tangents × xyz)
	uint32_t                     materialIndex;
	bool                         isTransparent = false; // if true, clear VK_GEOMETRY_OPAQUE_BIT_KHR so any-hit runs
};

class AccelerationStructure
{
public:
	AccelerationStructure() = default;
	~AccelerationStructure() { Destroy(); }

	void Destroy();
	void SetDevice(const GpuDevice& dev) { m_Device = dev; }

	// Build one BLAS per mesh geometry. Each BLAS gets its own vertex/index/
	// normal buffers. Returns true if all builds succeeded.
	bool BuildBLASes(VkCommandBuffer cmdBuffer,
	                 const std::vector<BLASGeometry>& meshes);

	// Build TLAS over the previously-built BLASes. Each instance references
	// a BLAS by index and carries a customIndex (material index).
	// Also stores the instance-to-BLAS mapping for combined buffer offsets.
	bool BuildTLAS(VkCommandBuffer cmdBuffer,
	               const std::vector<BLASInstance>& instances,
	               const std::vector<uint32_t>& instanceMeshIndices = {});

	// Rebuild TLAS only (no BLAS rebuild, no combined buffer rebuild).
	// Uses existing BLASes — just updates instance transforms.
	// Call after BuildBLASes + BuildTLAS have been called at least once.
	bool RebuildTLASOnly(VkCommandBuffer cmdBuffer,
	                     const std::vector<BLASInstance>& instances,
	                     const std::vector<uint32_t>& instanceMeshIndices);

	// Build combined normal/position/UV/tangent buffers from BLAS data.
	// Must be called after BuildTLAS (needs instance-to-BLAS mapping).
	// Emits per-BLAS object-space data (deduplicated). The shader transforms
	// to world space at hit time using the instance transform SSBO.
	void BuildCombinedBuffers();

	VkDeviceAddress GetTLASDeviceAddress() const { return m_TLASDeviceAddress; }
	bool IsValid() const { return m_TLAS != VK_NULL_HANDLE; }
	VkAccelerationStructureKHR GetTLAS() const { return m_TLAS; }

	uint32_t GetBLASCount() const { return static_cast<uint32_t>(m_BLASes.size()); }
	VkDeviceAddress GetBLASAddress(uint32_t index) const;

	VkBuffer GetNormalBuffer() const { return m_CombinedNormalBuffer; }
	VkBuffer GetInstanceOffsetBuffer() const { return m_InstanceOffsetBuffer; }
	VkBuffer GetUVBuffer() const { return m_CombinedUVBuffer; }
	VkBuffer GetPositionBuffer() const { return m_CombinedPositionBuffer; }
	VkBuffer GetTangentBuffer() const { return m_CombinedTangentBuffer; }
	uint32_t GetTriangleCount() const { return m_TotalTriangleCount; }

private:
	struct BLASData
	{
		VkBuffer vertexBuffer = VK_NULL_HANDLE;
		VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
		VkBuffer indexBuffer = VK_NULL_HANDLE;
		VkDeviceMemory indexMemory = VK_NULL_HANDLE;
		VkBuffer normalBuffer = VK_NULL_HANDLE;
		VkDeviceMemory normalMemory = VK_NULL_HANDLE;
		VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
		VkBuffer blasBuffer = VK_NULL_HANDLE;
		VkDeviceMemory blasMemory = VK_NULL_HANDLE;
		VkBuffer scratchBuffer = VK_NULL_HANDLE;
		VkDeviceMemory scratchMemory = VK_NULL_HANDLE;
		VkDeviceAddress deviceAddress = 0;
		uint32_t triangleCount = 0;
		std::vector<float> triPositions; // 9 floats per triangle (3 × xyz)
		std::vector<float> triUVs;       // 6 floats per triangle (3 × uv)
		std::vector<float> triTangents;  // 9 floats per triangle (3 × xyz)
	};

	std::vector<BLASData> m_BLASes;

	// Combined normal buffer (all triangles from all BLASes) + per-instance offsets
	VkBuffer m_CombinedNormalBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_CombinedNormalMemory = VK_NULL_HANDLE;
	VkBuffer m_InstanceOffsetBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_InstanceOffsetMemory = VK_NULL_HANDLE;
	VkBuffer m_CombinedUVBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_CombinedUVMemory = VK_NULL_HANDLE;
	VkBuffer m_CombinedPositionBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_CombinedPositionMemory = VK_NULL_HANDLE;
	VkBuffer m_CombinedTangentBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_CombinedTangentMemory = VK_NULL_HANDLE;

	// TLAS
	VkAccelerationStructureKHR m_TLAS = VK_NULL_HANDLE;
	VkBuffer m_TLASBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_TLASMemory = VK_NULL_HANDLE;
	VkDeviceAddress m_TLASDeviceAddress = 0;

	// Instance buffer
	VkBuffer m_InstanceBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_InstanceMemory = VK_NULL_HANDLE;

	// TLAS scratch
	VkBuffer m_TLASScratchBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_TLASScratchMemory = VK_NULL_HANDLE;

	uint32_t m_TotalTriangleCount = 0;

	// Maps TLAS instance index → BLAS index (for combined buffer offsets)
	std::vector<uint32_t> m_InstanceToBLAS;

	GpuDevice m_Device;
};

#endif // !ACCELERATION_STRUCTURE_H