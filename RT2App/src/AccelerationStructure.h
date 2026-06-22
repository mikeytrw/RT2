#pragma once

#ifndef ACCELERATION_STRUCTURE_H
#define ACCELERATION_STRUCTURE_H

#include "vulkan/vulkan.h"
#include <vector>
#include <string>

struct BLASInstance
{
	VkDeviceAddress blasAddress;
	uint32_t customIndex; // material index
	VkTransformMatrixKHR transform;
};

class AccelerationStructure
{
public:
	AccelerationStructure() = default;
	~AccelerationStructure() { Destroy(); }

	void Destroy();

	bool BuildBLAS(VkCommandBuffer cmdBuffer,
	               const std::vector<float>& vertices,
	               const std::vector<uint32_t>& indices,
	               uint32_t materialIndex);

	bool BuildTLAS(VkCommandBuffer cmdBuffer,
	               const std::vector<BLASInstance>& instances);

	VkDeviceAddress GetTLASDeviceAddress() const { return m_TLASDeviceAddress; }
	bool IsValid() const { return m_TLAS != VK_NULL_HANDLE; }
	VkAccelerationStructureKHR GetBLAS() const { return m_BLAS; }
	VkAccelerationStructureKHR GetTLAS() const { return m_TLAS; }

	VkBuffer GetVertexBuffer() const { return m_VertexBuffer; }
	VkBuffer GetIndexBuffer() const { return m_IndexBuffer; }
	VkBuffer GetNormalBuffer() const { return m_NormalBuffer; }
	uint32_t GetTriangleCount() const { return m_TriangleCount; }

private:
	void CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
	                  VkBuffer& buffer, VkDeviceMemory& memory);
	void DestroyBuffer(VkBuffer buffer, VkDeviceMemory memory);
	VkDeviceAddress GetBufferDeviceAddress(VkBuffer buffer);

	// BLAS
	VkBuffer m_VertexBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_VertexMemory = VK_NULL_HANDLE;
	VkBuffer m_IndexBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_IndexMemory = VK_NULL_HANDLE;
	VkBuffer m_NormalBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_NormalMemory = VK_NULL_HANDLE;

	VkAccelerationStructureKHR m_BLAS = VK_NULL_HANDLE;
	VkBuffer m_BLASBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_BLASMemory = VK_NULL_HANDLE;

	// TLAS
	VkAccelerationStructureKHR m_TLAS = VK_NULL_HANDLE;
	VkBuffer m_TLASBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_TLASMemory = VK_NULL_HANDLE;
	VkDeviceAddress m_TLASDeviceAddress = 0;

	// Instance buffer
	VkBuffer m_InstanceBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_InstanceMemory = VK_NULL_HANDLE;

	// Scratch buffers (kept alive until command buffer finishes)
	VkBuffer m_BLASScratchBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_BLASScratchMemory = VK_NULL_HANDLE;
	VkBuffer m_TLASScratchBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_TLASScratchMemory = VK_NULL_HANDLE;

	uint32_t m_TriangleCount = 0;

	uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
};

#endif // !ACCELERATION_STRUCTURE_H