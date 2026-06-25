#include "AccelerationStructure.h"
#include "Walnut/Application.h"
#include <glm/glm.hpp>
#include <iostream>
#include <cstring>

uint32_t AccelerationStructure::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
	VkPhysicalDeviceMemoryProperties memProperties;
	vkGetPhysicalDeviceMemoryProperties(Walnut::Application::GetPhysicalDevice(), &memProperties);

	for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++)
	{
		if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
			return i;
	}

	std::cerr << "[RT2] Failed to find suitable memory type!\n";
	return 0;
}

void AccelerationStructure::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
                                          VkBuffer& buffer, VkDeviceMemory& memory)
{
	VkDevice device = Walnut::Application::GetDevice();

	VkBufferCreateInfo bufferInfo = {};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = size;
	bufferInfo.usage = usage;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VkResult err = vkCreateBuffer(device, &bufferInfo, nullptr, &buffer);
	if (err != VK_SUCCESS)
	{
		std::cerr << "[RT2] vkCreateBuffer failed: " << err << " (size=" << size << ")\n";
		return;
	}

	VkMemoryRequirements memRequirements;
	vkGetBufferMemoryRequirements(device, buffer, &memRequirements);

	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memRequirements.size;
	allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits, properties);

	// For buffers that need device addresses, add the export info
	VkMemoryAllocateFlagsInfo allocateFlagsInfo = {};
	if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
	{
		allocateFlagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
		allocateFlagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
		allocInfo.pNext = &allocateFlagsInfo;
	}

	err = vkAllocateMemory(device, &allocInfo, nullptr, &memory);
	if (err != VK_SUCCESS)
	{
		std::cerr << "[RT2] vkAllocateMemory failed: " << err << " (size=" << memRequirements.size << ")\n";
		vkDestroyBuffer(device, buffer, nullptr);
		buffer = VK_NULL_HANDLE;
		return;
	}

	vkBindBufferMemory(device, buffer, memory, 0);
}

void AccelerationStructure::DestroyBuffer(VkBuffer buffer, VkDeviceMemory memory)
{
	VkDevice device = Walnut::Application::GetDevice();
	if (buffer) vkDestroyBuffer(device, buffer, nullptr);
	if (memory) vkFreeMemory(device, memory, nullptr);
}

VkDeviceAddress AccelerationStructure::GetBufferDeviceAddress(VkBuffer buffer)
{
	VkBufferDeviceAddressInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
	info.buffer = buffer;
	return vkGetBufferDeviceAddress(Walnut::Application::GetDevice(), &info);
}

bool AccelerationStructure::BuildBLAS(VkCommandBuffer cmdBuffer,
                                       const std::vector<float>& vertices,
                                       const std::vector<uint32_t>& indices,
                                       uint32_t materialIndex)
{
	VkDevice device = Walnut::Application::GetDevice();
	m_TriangleCount = static_cast<uint32_t>(indices.size() / 3);

	// Create vertex buffer
	VkDeviceSize vertexBufferSize = vertices.size() * sizeof(float);
	CreateBuffer(vertexBufferSize,
	             VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
	             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
	             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	             m_VertexBuffer, m_VertexMemory);

	void* vertexData;
	vkMapMemory(device, m_VertexMemory, 0, vertexBufferSize, 0, &vertexData);
	memcpy(vertexData, vertices.data(), vertexBufferSize);
	vkUnmapMemory(device, m_VertexMemory);

	// Create index buffer
	VkDeviceSize indexBufferSize = indices.size() * sizeof(uint32_t);
	CreateBuffer(indexBufferSize,
	             VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
	             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
	             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	             m_IndexBuffer, m_IndexMemory);

	void* indexData;
	vkMapMemory(device, m_IndexMemory, 0, indexBufferSize, 0, &indexData);
	memcpy(indexData, indices.data(), indexBufferSize);
	vkUnmapMemory(device, m_IndexMemory);

	// Create normal buffer (one normal per triangle, stored as vec4 for 16-byte alignment)
	std::vector<glm::vec4> normals(m_TriangleCount);
	for (uint32_t i = 0; i < m_TriangleCount; i++)
	{
		uint32_t i0 = indices[i * 3 + 0] * 3;
		uint32_t i1 = indices[i * 3 + 1] * 3;
		uint32_t i2 = indices[i * 3 + 2] * 3;

		glm::vec3 v0(vertices[i0], vertices[i0 + 1], vertices[i0 + 2]);
		glm::vec3 v1(vertices[i1], vertices[i1 + 1], vertices[i1 + 2]);
		glm::vec3 v2(vertices[i2], vertices[i2 + 1], vertices[i2 + 2]);

		glm::vec3 normal = glm::normalize(glm::cross(v1 - v0, v2 - v0));

		normals[i] = glm::vec4(normal, 0.0f);
	}

	VkDeviceSize normalBufferSize = normals.size() * sizeof(glm::vec4);
	CreateBuffer(normalBufferSize,
	             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
	             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	             m_NormalBuffer, m_NormalMemory);

	void* normalData;
	vkMapMemory(device, m_NormalMemory, 0, normalBufferSize, 0, &normalData);
	memcpy(normalData, normals.data(), normalBufferSize);
	vkUnmapMemory(device, m_NormalMemory);

	// Set up BLAS geometry
	VkAccelerationStructureGeometryKHR geometry = {};
	geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
	geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;

	VkAccelerationStructureGeometryTrianglesDataKHR trianglesData = {};
	trianglesData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
	trianglesData.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
	trianglesData.vertexData.deviceAddress = GetBufferDeviceAddress(m_VertexBuffer);
	trianglesData.vertexStride = 3 * sizeof(float);
	trianglesData.maxVertex = static_cast<uint32_t>(vertices.size() / 3) - 1;
	trianglesData.indexType = VK_INDEX_TYPE_UINT32;
	trianglesData.indexData.deviceAddress = GetBufferDeviceAddress(m_IndexBuffer);

	geometry.geometry.triangles = trianglesData;

	// Get build sizes
	VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {};
	buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	buildInfo.geometryCount = 1;
	buildInfo.pGeometries = &geometry;

	VkAccelerationStructureBuildSizesInfoKHR sizeInfo = {};
	sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;

	uint32_t primitiveCount = m_TriangleCount;

	PFN_vkGetAccelerationStructureBuildSizesKHR pvkGetAccelerationStructureBuildSizesKHR =
		(PFN_vkGetAccelerationStructureBuildSizesKHR)vkGetInstanceProcAddr(
			Walnut::Application::GetInstance(), "vkGetAccelerationStructureBuildSizesKHR");

	pvkGetAccelerationStructureBuildSizesKHR(device,
		VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
		&buildInfo, &primitiveCount, &sizeInfo);

	// Create BLAS buffer
	CreateBuffer(sizeInfo.accelerationStructureSize,
	             VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
	             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
	             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
	             m_BLASBuffer, m_BLASMemory);

	// Create BLAS
	VkAccelerationStructureCreateInfoKHR createInfo = {};
	createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
	createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	createInfo.buffer = m_BLASBuffer;
	createInfo.size = sizeInfo.accelerationStructureSize;

	PFN_vkCreateAccelerationStructureKHR pvkCreateAccelerationStructureKHR =
		(PFN_vkCreateAccelerationStructureKHR)vkGetInstanceProcAddr(
			Walnut::Application::GetInstance(), "vkCreateAccelerationStructureKHR");

	pvkCreateAccelerationStructureKHR(device, &createInfo, nullptr, &m_BLAS);

	// Create scratch buffer
	DestroyBuffer(m_BLASScratchBuffer, m_BLASScratchMemory);
	CreateBuffer(sizeInfo.buildScratchSize,
	             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
	             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
	             m_BLASScratchBuffer, m_BLASScratchMemory);

	buildInfo.dstAccelerationStructure = m_BLAS;
	buildInfo.scratchData.deviceAddress = GetBufferDeviceAddress(m_BLASScratchBuffer);

	VkAccelerationStructureBuildRangeInfoKHR buildRangeInfo = {};
	buildRangeInfo.primitiveCount = primitiveCount;
	buildRangeInfo.primitiveOffset = 0;
	buildRangeInfo.firstVertex = 0;
	buildRangeInfo.transformOffset = 0;

	VkAccelerationStructureBuildRangeInfoKHR* pBuildRangeInfos = &buildRangeInfo;

	PFN_vkCmdBuildAccelerationStructuresKHR pvkCmdBuildAccelerationStructuresKHR =
		(PFN_vkCmdBuildAccelerationStructuresKHR)vkGetInstanceProcAddr(
			Walnut::Application::GetInstance(), "vkCmdBuildAccelerationStructuresKHR");

	pvkCmdBuildAccelerationStructuresKHR(cmdBuffer, 1, &buildInfo, &pBuildRangeInfos);

	// Get BLAS device address
	VkAccelerationStructureDeviceAddressInfoKHR blasAddrInfo = {};
	blasAddrInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
	blasAddrInfo.accelerationStructure = m_BLAS;
	PFN_vkGetAccelerationStructureDeviceAddressKHR pvkGetASAddr =
		(PFN_vkGetAccelerationStructureDeviceAddressKHR)vkGetInstanceProcAddr(
			Walnut::Application::GetInstance(), "vkGetAccelerationStructureDeviceAddressKHR");
	VkDeviceAddress blasAddr = pvkGetASAddr(device, &blasAddrInfo);

	return true;
}

bool AccelerationStructure::BuildTLAS(VkCommandBuffer cmdBuffer,
                                       const std::vector<BLASInstance>& instances)
{
	VkDevice device = Walnut::Application::GetDevice();

	if (instances.empty())
	{
		std::cerr << "[RT2] No instances for TLAS build\n";
		return false;
	}

	// Create instance buffer
	VkDeviceSize instanceBufferSize = instances.size() * sizeof(VkAccelerationStructureInstanceKHR);
	CreateBuffer(instanceBufferSize,
	             VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
	             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
	             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	             m_InstanceBuffer, m_InstanceMemory);

	std::vector<VkAccelerationStructureInstanceKHR> vkInstances;
	vkInstances.reserve(instances.size());
	for (const auto& inst : instances)
	{
		VkAccelerationStructureInstanceKHR vkInst = {};
		vkInst.transform = inst.transform;
		vkInst.instanceCustomIndex = inst.customIndex;
		vkInst.mask = 0xFF;
		vkInst.instanceShaderBindingTableRecordOffset = 0;
		vkInst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
		vkInst.accelerationStructureReference = inst.blasAddress;
		vkInstances.push_back(vkInst);
	}

	void* instanceData;
	vkMapMemory(device, m_InstanceMemory, 0, instanceBufferSize, 0, &instanceData);
	memcpy(instanceData, vkInstances.data(), instanceBufferSize);
	vkUnmapMemory(device, m_InstanceMemory);

	// Set up TLAS geometry
	VkAccelerationStructureGeometryKHR geometry = {};
	geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;

	VkAccelerationStructureGeometryInstancesDataKHR instancesData = {};
	instancesData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	instancesData.data.deviceAddress = GetBufferDeviceAddress(m_InstanceBuffer);

	geometry.geometry.instances = instancesData;

	// Get build sizes
	VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {};
	buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	buildInfo.geometryCount = 1;
	buildInfo.pGeometries = &geometry;

	VkAccelerationStructureBuildSizesInfoKHR sizeInfo = {};
	sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;

	uint32_t primitiveCount = static_cast<uint32_t>(instances.size());

	PFN_vkGetAccelerationStructureBuildSizesKHR pvkGetAccelerationStructureBuildSizesKHR =
		(PFN_vkGetAccelerationStructureBuildSizesKHR)vkGetInstanceProcAddr(
			Walnut::Application::GetInstance(), "vkGetAccelerationStructureBuildSizesKHR");

	pvkGetAccelerationStructureBuildSizesKHR(device,
		VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
		&buildInfo, &primitiveCount, &sizeInfo);

	// Create TLAS buffer
	CreateBuffer(sizeInfo.accelerationStructureSize,
	             VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
	             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
	             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
	             m_TLASBuffer, m_TLASMemory);

	// Create TLAS
	VkAccelerationStructureCreateInfoKHR createInfo = {};
	createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
	createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	createInfo.buffer = m_TLASBuffer;
	createInfo.size = sizeInfo.accelerationStructureSize;

	PFN_vkCreateAccelerationStructureKHR pvkCreateAccelerationStructureKHR =
		(PFN_vkCreateAccelerationStructureKHR)vkGetInstanceProcAddr(
			Walnut::Application::GetInstance(), "vkCreateAccelerationStructureKHR");

	pvkCreateAccelerationStructureKHR(device, &createInfo, nullptr, &m_TLAS);

	// Create TLAS scratch buffer (separate from BLAS scratch)
	DestroyBuffer(m_TLASScratchBuffer, m_TLASScratchMemory);
	CreateBuffer(sizeInfo.buildScratchSize,
	             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
	             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
	             m_TLASScratchBuffer, m_TLASScratchMemory);

	buildInfo.dstAccelerationStructure = m_TLAS;
	buildInfo.scratchData.deviceAddress = GetBufferDeviceAddress(m_TLASScratchBuffer);

	VkAccelerationStructureBuildRangeInfoKHR buildRangeInfo = {};
	buildRangeInfo.primitiveCount = primitiveCount;
	buildRangeInfo.primitiveOffset = 0;

	VkAccelerationStructureBuildRangeInfoKHR* pBuildRangeInfos = &buildRangeInfo;

	PFN_vkCmdBuildAccelerationStructuresKHR pvkCmdBuildAccelerationStructuresKHR =
		(PFN_vkCmdBuildAccelerationStructuresKHR)vkGetInstanceProcAddr(
			Walnut::Application::GetInstance(), "vkCmdBuildAccelerationStructuresKHR");

	pvkCmdBuildAccelerationStructuresKHR(cmdBuffer, 1, &buildInfo, &pBuildRangeInfos);

	// Get TLAS device address
	VkAccelerationStructureDeviceAddressInfoKHR addressInfo = {};
	addressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
	addressInfo.accelerationStructure = m_TLAS;

	PFN_vkGetAccelerationStructureDeviceAddressKHR pvkGetAccelerationStructureDeviceAddressKHR =
		(PFN_vkGetAccelerationStructureDeviceAddressKHR)vkGetInstanceProcAddr(
			Walnut::Application::GetInstance(), "vkGetAccelerationStructureDeviceAddressKHR");

	m_TLASDeviceAddress = pvkGetAccelerationStructureDeviceAddressKHR(device, &addressInfo);

	return true;
}

void AccelerationStructure::Destroy()
{
	VkDevice device = Walnut::Application::GetDevice();

	DestroyBuffer(m_VertexBuffer, m_VertexMemory);
	DestroyBuffer(m_IndexBuffer, m_IndexMemory);
	DestroyBuffer(m_NormalBuffer, m_NormalMemory);
	DestroyBuffer(m_BLASBuffer, m_BLASMemory);
	DestroyBuffer(m_TLASBuffer, m_TLASMemory);
	DestroyBuffer(m_InstanceBuffer, m_InstanceMemory);
	DestroyBuffer(m_BLASScratchBuffer, m_BLASScratchMemory);
	DestroyBuffer(m_TLASScratchBuffer, m_TLASScratchMemory);

	if (m_BLAS)
	{
		PFN_vkDestroyAccelerationStructureKHR pvkDestroyAccelerationStructureKHR =
			(PFN_vkDestroyAccelerationStructureKHR)vkGetInstanceProcAddr(
				Walnut::Application::GetInstance(), "vkDestroyAccelerationStructureKHR");
		pvkDestroyAccelerationStructureKHR(device, m_BLAS, nullptr);
		m_BLAS = VK_NULL_HANDLE;
	}

	if (m_TLAS)
	{
		PFN_vkDestroyAccelerationStructureKHR pvkDestroyAccelerationStructureKHR =
			(PFN_vkDestroyAccelerationStructureKHR)vkGetInstanceProcAddr(
				Walnut::Application::GetInstance(), "vkDestroyAccelerationStructureKHR");
		pvkDestroyAccelerationStructureKHR(device, m_TLAS, nullptr);
		m_TLAS = VK_NULL_HANDLE;
	}

	m_TLASDeviceAddress = 0;
}