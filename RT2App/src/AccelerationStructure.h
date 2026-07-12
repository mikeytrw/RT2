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
	const std::vector<float>*    normals;     // normal.xyz, stride 3 (may be null)
	const std::vector<float>*    uvs;         // texcoord.xy, stride 2 (may be null)
	const std::vector<uint32_t>* materialIndices; // per-triangle material index (may be null)
	uint32_t                     materialIndex;
	bool                         isTransparent = false;
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

	// Build attribute buffers (vertex as vec4, index as uint, normal as vec4,
	// UV as vec4, instanceMeshInfo as uvec4). Must be called after BuildTLAS.
	void BuildAttributeBuffers();

	VkDeviceAddress GetTLASDeviceAddress() const { return m_TLASDeviceAddress; }
	bool IsValid() const { return m_TLAS != VK_NULL_HANDLE; }
	VkAccelerationStructureKHR GetTLAS() const { return m_TLAS; }

	uint32_t GetBLASCount() const { return static_cast<uint32_t>(m_BLASes.size()); }
	VkDeviceAddress GetBLASAddress(uint32_t index) const;

	VkBuffer GetVertexBuffer() const { return m_VertexBuffer; }
	VkBuffer GetIndexBuffer() const { return m_IndexBuffer; }
	VkBuffer GetNormalBuffer() const { return m_NormalBuffer; }
	VkBuffer GetUVBuffer() const { return m_UVBuffer; }
	VkBuffer GetInstanceMeshInfoBuffer() const { return m_InstanceMeshInfoBuffer; }
	VkBuffer GetMaterialIndexBuffer() const { return m_MaterialIndexBuffer; }
	uint32_t GetTriangleCount() const { return m_TotalTriangleCount; }

private:
	struct BLASData
	{
		VkBuffer vertexBuffer = VK_NULL_HANDLE;
		VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
		VkBuffer indexBuffer = VK_NULL_HANDLE;
		VkDeviceMemory indexMemory = VK_NULL_HANDLE;
		VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
		VkBuffer blasBuffer = VK_NULL_HANDLE;
		VkDeviceMemory blasMemory = VK_NULL_HANDLE;
		VkBuffer scratchBuffer = VK_NULL_HANDLE;
		VkDeviceMemory scratchMemory = VK_NULL_HANDLE;
		VkDeviceAddress deviceAddress = 0;
		uint32_t triangleCount = 0;
		uint32_t vertexCount = 0;
		const std::vector<float>*    srcVertices = nullptr;
		const std::vector<uint32_t>* srcIndices = nullptr;
		const std::vector<float>*    srcNormals = nullptr;
		const std::vector<float>*    srcUVs = nullptr;
		const std::vector<uint32_t>* srcMaterialIndices = nullptr;
		uint32_t                     materialIndex = 0;
	};

	std::vector<BLASData> m_BLASes;

	// Attribute mega-buffers (DEVICE_LOCAL, vec4 storage)
	VkBuffer m_VertexBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_VertexMemory = VK_NULL_HANDLE;
	VkBuffer m_IndexBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_IndexMemory = VK_NULL_HANDLE;
	VkBuffer m_NormalBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_NormalMemory = VK_NULL_HANDLE;
	VkBuffer m_UVBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_UVMemory = VK_NULL_HANDLE;
	VkBuffer m_InstanceMeshInfoBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_InstanceMeshInfoMemory = VK_NULL_HANDLE;
	VkBuffer m_MaterialIndexBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_MaterialIndexMemory = VK_NULL_HANDLE;

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