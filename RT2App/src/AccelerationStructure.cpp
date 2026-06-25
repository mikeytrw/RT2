#include "AccelerationStructure.h"
#include "Walnut/Application.h"
#include "Walnut/RTDispatch.h"
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

bool AccelerationStructure::BuildBLASes(VkCommandBuffer cmdBuffer,
                                         const std::vector<BLASGeometry>& meshes)
{
	VkDevice device = Walnut::Application::GetDevice();

	// Destroy previous BLASes
	for (auto& blas : m_BLASes)
	{
		DestroyBuffer(blas.vertexBuffer, blas.vertexMemory);
		DestroyBuffer(blas.indexBuffer, blas.indexMemory);
		DestroyBuffer(blas.normalBuffer, blas.normalMemory);
		DestroyBuffer(blas.blasBuffer, blas.blasMemory);
		DestroyBuffer(blas.scratchBuffer, blas.scratchMemory);
		if (blas.handle)
			g_RTDispatch.DestroyAccelerationStructureKHR(device, blas.handle, nullptr);
	}
	m_BLASes.clear();

	DestroyBuffer(m_CombinedNormalBuffer, m_CombinedNormalMemory);
	DestroyBuffer(m_InstanceOffsetBuffer, m_InstanceOffsetMemory);
	m_BLASes.resize(meshes.size());
	m_TotalTriangleCount = 0;

	for (size_t i = 0; i < meshes.size(); i++)
	{
		const auto& mesh = meshes[i];
		BLASData& blas = m_BLASes[i];

		uint32_t triCount = static_cast<uint32_t>(mesh.indices->size() / 3);
		blas.triangleCount = triCount;
		m_TotalTriangleCount += triCount;

		// --- Vertex buffer ---
		VkDeviceSize vertexBufferSize = mesh.vertices->size() * sizeof(float);
		CreateBuffer(vertexBufferSize,
		             VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
		             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		             blas.vertexBuffer, blas.vertexMemory);

		void* vertexData;
		vkMapMemory(device, blas.vertexMemory, 0, vertexBufferSize, 0, &vertexData);
		memcpy(vertexData, mesh.vertices->data(), vertexBufferSize);
		vkUnmapMemory(device, blas.vertexMemory);

		// --- Index buffer ---
		VkDeviceSize indexBufferSize = mesh.indices->size() * sizeof(uint32_t);
		CreateBuffer(indexBufferSize,
		             VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
		             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		             blas.indexBuffer, blas.indexMemory);

		void* indexData;
		vkMapMemory(device, blas.indexMemory, 0, indexBufferSize, 0, &indexData);
		memcpy(indexData, mesh.indices->data(), indexBufferSize);
		vkUnmapMemory(device, blas.indexMemory);

		// --- BLAS geometry ---
		VkAccelerationStructureGeometryKHR geometry = {};
		geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
		geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
		geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;

		VkAccelerationStructureGeometryTrianglesDataKHR trianglesData = {};
		trianglesData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
		trianglesData.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
		trianglesData.vertexData.deviceAddress = GetBufferDeviceAddress(blas.vertexBuffer);
		trianglesData.vertexStride = 3 * sizeof(float);
		trianglesData.maxVertex = static_cast<uint32_t>(mesh.vertices->size() / 3) - 1;
		trianglesData.indexType = VK_INDEX_TYPE_UINT32;
		trianglesData.indexData.deviceAddress = GetBufferDeviceAddress(blas.indexBuffer);
		geometry.geometry.triangles = trianglesData;

		// --- Build sizes ---
		VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {};
		buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
		buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
		buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
		buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
		buildInfo.geometryCount = 1;
		buildInfo.pGeometries = &geometry;

		VkAccelerationStructureBuildSizesInfoKHR sizeInfo = {};
		sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;

		uint32_t primitiveCount = triCount;
		g_RTDispatch.GetAccelerationStructureBuildSizesKHR(device,
			VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
			&buildInfo, &primitiveCount, &sizeInfo);

		// --- BLAS buffer ---
		CreateBuffer(sizeInfo.accelerationStructureSize,
		             VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
		             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		             blas.blasBuffer, blas.blasMemory);

		VkAccelerationStructureCreateInfoKHR createInfo = {};
		createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
		createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
		createInfo.buffer = blas.blasBuffer;
		createInfo.size = sizeInfo.accelerationStructureSize;
		g_RTDispatch.CreateAccelerationStructureKHR(device, &createInfo, nullptr, &blas.handle);

		// --- Scratch buffer ---
		CreateBuffer(sizeInfo.buildScratchSize,
		             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		             blas.scratchBuffer, blas.scratchMemory);

		buildInfo.dstAccelerationStructure = blas.handle;
		buildInfo.scratchData.deviceAddress = GetBufferDeviceAddress(blas.scratchBuffer);

		VkAccelerationStructureBuildRangeInfoKHR buildRangeInfo = {};
		buildRangeInfo.primitiveCount = primitiveCount;
		buildRangeInfo.primitiveOffset = 0;
		buildRangeInfo.firstVertex = 0;
		buildRangeInfo.transformOffset = 0;

		VkAccelerationStructureBuildRangeInfoKHR* pBuildRangeInfos = &buildRangeInfo;
		g_RTDispatch.CmdBuildAccelerationStructuresKHR(cmdBuffer, 1, &buildInfo, &pBuildRangeInfos);

		// --- Get BLAS device address ---
		VkAccelerationStructureDeviceAddressInfoKHR addrInfo = {};
		addrInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
		addrInfo.accelerationStructure = blas.handle;
		blas.deviceAddress = g_RTDispatch.GetAccelerationStructureDeviceAddressKHR(device, &addrInfo);
	}

	// Build combined normal buffer (all triangles from all BLASes) and
	// per-instance offset buffer so the shader can index normals as
	// normalOffsets[gl_InstanceID] + gl_PrimitiveID.
	BuildCombinedNormalBuffer();

	return true;
}

void AccelerationStructure::BuildCombinedNormalBuffer()
{
	VkDevice device = Walnut::Application::GetDevice();

	// Destroy previous combined buffers
	DestroyBuffer(m_CombinedNormalBuffer, m_CombinedNormalMemory);
	DestroyBuffer(m_InstanceOffsetBuffer, m_InstanceOffsetMemory);

	// Collect all normals into one buffer, track per-BLAS offsets
	std::vector<glm::vec4> allNormals;
	std::vector<uint32_t> offsets;
	allNormals.reserve(m_TotalTriangleCount);
	offsets.reserve(m_BLASes.size());

	for (auto& blas : m_BLASes)
	{
		offsets.push_back(static_cast<uint32_t>(allNormals.size()));

		uint32_t triCount = blas.triangleCount;
		for (uint32_t t = 0; t < triCount; t++)
		{
		// Read vertices from the BLAS vertex buffer (host-visible)
		void* vtxMapped = nullptr;
		VkDeviceSize vtxSize = 0;
			{
				VkMemoryRequirements req;
				vkGetBufferMemoryRequirements(device, blas.vertexBuffer, &req);
				vtxSize = req.size;
			}
			vkMapMemory(device, blas.vertexMemory, 0, vtxSize, 0, &vtxMapped);
			const float* verts = static_cast<const float*>(vtxMapped);

			void* idxMapped = nullptr;
			VkDeviceSize idxSize = 0;
			{
				VkMemoryRequirements req;
				vkGetBufferMemoryRequirements(device, blas.indexBuffer, &req);
				idxSize = req.size;
			}
			vkMapMemory(device, blas.indexMemory, 0, idxSize, 0, &idxMapped);
			const uint32_t* indices = static_cast<const uint32_t*>(idxMapped);

			uint32_t i0 = indices[t * 3 + 0] * 3;
			uint32_t i1 = indices[t * 3 + 1] * 3;
			uint32_t i2 = indices[t * 3 + 2] * 3;

			glm::vec3 v0(verts[i0], verts[i0 + 1], verts[i0 + 2]);
			glm::vec3 v1(verts[i1], verts[i1 + 1], verts[i1 + 2]);
			glm::vec3 v2(verts[i2], verts[i2 + 1], verts[i2 + 2]);

			glm::vec3 normal = glm::normalize(glm::cross(v1 - v0, v2 - v0));
			allNormals.push_back(glm::vec4(normal, 0.0f));

			vkUnmapMemory(device, blas.vertexMemory);
			vkUnmapMemory(device, blas.indexMemory);
		}
	}

	// Create combined normal buffer
	VkDeviceSize normalBufferSize = allNormals.size() * sizeof(glm::vec4);
	if (normalBufferSize > 0)
	{
		CreateBuffer(normalBufferSize,
		             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		             m_CombinedNormalBuffer, m_CombinedNormalMemory);

		void* normalData;
		vkMapMemory(device, m_CombinedNormalMemory, 0, normalBufferSize, 0, &normalData);
		memcpy(normalData, allNormals.data(), normalBufferSize);
		vkUnmapMemory(device, m_CombinedNormalMemory);
	}

	// Create instance offset buffer
	VkDeviceSize offsetBufferSize = offsets.size() * sizeof(uint32_t);
	if (offsetBufferSize > 0)
	{
		CreateBuffer(offsetBufferSize,
		             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		             m_InstanceOffsetBuffer, m_InstanceOffsetMemory);

		void* offsetData;
		vkMapMemory(device, m_InstanceOffsetMemory, 0, offsetBufferSize, 0, &offsetData);
		memcpy(offsetData, offsets.data(), offsetBufferSize);
		vkUnmapMemory(device, m_InstanceOffsetMemory);
	}
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

	// Destroy previous TLAS
	if (m_TLAS)
	{
		g_RTDispatch.DestroyAccelerationStructureKHR(device, m_TLAS, nullptr);
		m_TLAS = VK_NULL_HANDLE;
	}
	DestroyBuffer(m_TLASBuffer, m_TLASMemory);
	DestroyBuffer(m_InstanceBuffer, m_InstanceMemory);
	DestroyBuffer(m_TLASScratchBuffer, m_TLASScratchMemory);

	// --- Instance buffer ---
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

	// --- TLAS geometry ---
	VkAccelerationStructureGeometryKHR geometry = {};
	geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;

	VkAccelerationStructureGeometryInstancesDataKHR instancesData = {};
	instancesData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	instancesData.data.deviceAddress = GetBufferDeviceAddress(m_InstanceBuffer);
	geometry.geometry.instances = instancesData;

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
	g_RTDispatch.GetAccelerationStructureBuildSizesKHR(device,
		VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
		&buildInfo, &primitiveCount, &sizeInfo);

	// --- TLAS buffer ---
	CreateBuffer(sizeInfo.accelerationStructureSize,
	             VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
	             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
	             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
	             m_TLASBuffer, m_TLASMemory);

	VkAccelerationStructureCreateInfoKHR createInfo = {};
	createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
	createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	createInfo.buffer = m_TLASBuffer;
	createInfo.size = sizeInfo.accelerationStructureSize;
	g_RTDispatch.CreateAccelerationStructureKHR(device, &createInfo, nullptr, &m_TLAS);

	// --- TLAS scratch ---
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
	g_RTDispatch.CmdBuildAccelerationStructuresKHR(cmdBuffer, 1, &buildInfo, &pBuildRangeInfos);

	// --- Get TLAS device address ---
	VkAccelerationStructureDeviceAddressInfoKHR addressInfo = {};
	addressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
	addressInfo.accelerationStructure = m_TLAS;
	m_TLASDeviceAddress = g_RTDispatch.GetAccelerationStructureDeviceAddressKHR(device, &addressInfo);

	return true;
}

VkDeviceAddress AccelerationStructure::GetBLASAddress(uint32_t index) const
{
	if (index < m_BLASes.size())
		return m_BLASes[index].deviceAddress;
	return 0;
}

void AccelerationStructure::Destroy()
{
	VkDevice device = Walnut::Application::GetDevice();

	for (auto& blas : m_BLASes)
	{
		DestroyBuffer(blas.vertexBuffer, blas.vertexMemory);
		DestroyBuffer(blas.indexBuffer, blas.indexMemory);
		DestroyBuffer(blas.normalBuffer, blas.normalMemory);
		DestroyBuffer(blas.blasBuffer, blas.blasMemory);
		DestroyBuffer(blas.scratchBuffer, blas.scratchMemory);
		if (blas.handle)
			g_RTDispatch.DestroyAccelerationStructureKHR(device, blas.handle, nullptr);
	}
	m_BLASes.clear();

	DestroyBuffer(m_CombinedNormalBuffer, m_CombinedNormalMemory);
	DestroyBuffer(m_InstanceOffsetBuffer, m_InstanceOffsetMemory);

	if (m_TLAS)
	{
		g_RTDispatch.DestroyAccelerationStructureKHR(device, m_TLAS, nullptr);
		m_TLAS = VK_NULL_HANDLE;
	}
	DestroyBuffer(m_TLASBuffer, m_TLASMemory);
	DestroyBuffer(m_InstanceBuffer, m_InstanceMemory);
	DestroyBuffer(m_TLASScratchBuffer, m_TLASScratchMemory);

	m_TLASDeviceAddress = 0;
	m_TotalTriangleCount = 0;
}