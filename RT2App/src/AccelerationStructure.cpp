#include "AccelerationStructure.h"
#include "RTLog.h"
#include "VulkanUtils.h"
#include "Walnut/RTDispatch.h"
#include <glm/glm.hpp>
#include <iostream>
#include <cstring>

uint32_t AccelerationStructure::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
	return m_Device.FindMemoryType(typeFilter, properties);
}

void AccelerationStructure::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
                                          VkBuffer& buffer, VkDeviceMemory& memory)
{
	VkDevice device = m_Device.device;

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

void AccelerationStructure::DestroyBuffer(VkBuffer& buffer, VkDeviceMemory& memory)
{
	VkDevice device = m_Device.device;
	if (buffer) { vkDestroyBuffer(device, buffer, nullptr); buffer = VK_NULL_HANDLE; }
	if (memory) { vkFreeMemory(device, memory, nullptr); memory = VK_NULL_HANDLE; }
}

VkDeviceAddress AccelerationStructure::GetBufferDeviceAddress(VkBuffer buffer)
{
	VkBufferDeviceAddressInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
	info.buffer = buffer;
	return vkGetBufferDeviceAddress(m_Device.device, &info);
}

bool AccelerationStructure::BuildBLASes(VkCommandBuffer cmdBuffer,
                                         const std::vector<BLASGeometry>& meshes)
{
	VkDevice device = m_Device.device;
	RT_LOG("[BuildBLASes] enter: meshes=%d prevBLASes=%d", (int)meshes.size(), (int)m_BLASes.size());

	// Wait for GPU to finish before destroying old BLAS buffers
	RT_LOG("[BuildBLASes] calling vkDeviceWaitIdle");
	vkDeviceWaitIdle(device);
	RT_LOG("[BuildBLASes] vkDeviceWaitIdle done");

	// Destroy previous BLASes
	for (size_t i = 0; i < m_BLASes.size(); i++)
	{
		auto& blas = m_BLASes[i];
		RT_LOG("[BuildBLASes] destroying BLAS %d/%d", (int)i, (int)m_BLASes.size());
		DestroyBuffer(blas.vertexBuffer, blas.vertexMemory);
		DestroyBuffer(blas.indexBuffer, blas.indexMemory);
		DestroyBuffer(blas.normalBuffer, blas.normalMemory);
		DestroyBuffer(blas.blasBuffer, blas.blasMemory);
		DestroyBuffer(blas.scratchBuffer, blas.scratchMemory);
		if (blas.handle)
			g_RTDispatch.DestroyAccelerationStructureKHR(device, blas.handle, nullptr);
	}
	m_BLASes.clear();
	RT_LOG("[BuildBLASes] old BLASes destroyed, destroying combined buffers");

	DestroyBuffer(m_CombinedNormalBuffer, m_CombinedNormalMemory);
	DestroyBuffer(m_InstanceOffsetBuffer, m_InstanceOffsetMemory);
	DestroyBuffer(m_CombinedUVBuffer, m_CombinedUVMemory);
	DestroyBuffer(m_CombinedPositionBuffer, m_CombinedPositionMemory);
	DestroyBuffer(m_CombinedTangentBuffer, m_CombinedTangentMemory);
	RT_LOG("[BuildBLASes] combined buffers destroyed, resizing");
	m_BLASes.resize(meshes.size());
	m_TotalTriangleCount = 0;

	for (size_t i = 0; i < meshes.size(); i++)
	{
		const auto& mesh = meshes[i];
		BLASData& blas = m_BLASes[i];
		RT_LOG("[BuildBLASes] mesh %d: verts=%d indices=%d", (int)i, (int)mesh.vertices->size(), (int)mesh.indices->size());

		uint32_t triCount = static_cast<uint32_t>(mesh.indices->size() / 3);
		blas.triangleCount = triCount;
		m_TotalTriangleCount += triCount;

		// Store per-triangle positions, UVs, and tangents for later combined buffer build
		blas.triPositions.resize(triCount * 9);
		blas.triUVs.resize(triCount * 6);
		blas.triTangents.resize(triCount * 9);
		for (uint32_t t = 0; t < triCount; t++)
		{
			uint32_t vi0 = (*mesh.indices)[t * 3 + 0] * 3;
			uint32_t vi1 = (*mesh.indices)[t * 3 + 1] * 3;
			uint32_t vi2 = (*mesh.indices)[t * 3 + 2] * 3;

			// Positions (9 floats)
			blas.triPositions[t * 9 + 0] = (*mesh.vertices)[vi0];
			blas.triPositions[t * 9 + 1] = (*mesh.vertices)[vi0 + 1];
			blas.triPositions[t * 9 + 2] = (*mesh.vertices)[vi0 + 2];
			blas.triPositions[t * 9 + 3] = (*mesh.vertices)[vi1];
			blas.triPositions[t * 9 + 4] = (*mesh.vertices)[vi1 + 1];
			blas.triPositions[t * 9 + 5] = (*mesh.vertices)[vi1 + 2];
			blas.triPositions[t * 9 + 6] = (*mesh.vertices)[vi2];
			blas.triPositions[t * 9 + 7] = (*mesh.vertices)[vi2 + 1];
			blas.triPositions[t * 9 + 8] = (*mesh.vertices)[vi2 + 2];

			// UVs (6 floats)
			if (mesh.vertexUVs && !mesh.vertexUVs->empty())
			{
				blas.triUVs[t * 6 + 0] = (*mesh.vertexUVs)[t * 6 + 0];
				blas.triUVs[t * 6 + 1] = (*mesh.vertexUVs)[t * 6 + 1];
				blas.triUVs[t * 6 + 2] = (*mesh.vertexUVs)[t * 6 + 2];
				blas.triUVs[t * 6 + 3] = (*mesh.vertexUVs)[t * 6 + 3];
				blas.triUVs[t * 6 + 4] = (*mesh.vertexUVs)[t * 6 + 4];
				blas.triUVs[t * 6 + 5] = (*mesh.vertexUVs)[t * 6 + 5];
			}

			// Tangents (9 floats = 3 tangents × xyz)
			if (mesh.tangents && !mesh.tangents->empty())
			{
				for (int v = 0; v < 3; v++)
				{
					blas.triTangents[t * 9 + v * 3 + 0] = (*mesh.tangents)[t * 9 + v * 3 + 0];
					blas.triTangents[t * 9 + v * 3 + 1] = (*mesh.tangents)[t * 9 + v * 3 + 1];
					blas.triTangents[t * 9 + v * 3 + 2] = (*mesh.tangents)[t * 9 + v * 3 + 2];
				}
			}
		}

		// --- Vertex buffer ---
		VkDeviceSize vertexBufferSize = mesh.vertices->size() * sizeof(float);
		RT_LOG("[BuildBLASes] mesh %d: creating vertex buffer (%zu bytes)", (int)i, (size_t)vertexBufferSize);
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
		RT_LOG("[BuildBLASes] mesh %d: creating index buffer (%zu bytes)", (int)i, (size_t)indexBufferSize);
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
		geometry.flags = mesh.isTransparent ? 0 : VK_GEOMETRY_OPAQUE_BIT_KHR;

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
		RT_LOG("[BuildBLASes] mesh %d: getting build sizes (prims=%d)", (int)i, primitiveCount);
		g_RTDispatch.GetAccelerationStructureBuildSizesKHR(device,
			VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
			&buildInfo, &primitiveCount, &sizeInfo);

		// --- BLAS buffer ---
		RT_LOG("[BuildBLASes] mesh %d: creating BLAS buffer (%zu bytes)", (int)i, (size_t)sizeInfo.accelerationStructureSize);
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
		RT_LOG("[BuildBLASes] mesh %d: creating AS handle", (int)i);
		g_RTDispatch.CreateAccelerationStructureKHR(device, &createInfo, nullptr, &blas.handle);

		// --- Scratch buffer ---
		RT_LOG("[BuildBLASes] mesh %d: creating scratch buffer (%zu bytes)", (int)i, (size_t)sizeInfo.buildScratchSize);
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
		RT_LOG("[BuildBLASes] mesh %d: cmdBuildAS", (int)i);
		g_RTDispatch.CmdBuildAccelerationStructuresKHR(cmdBuffer, 1, &buildInfo, &pBuildRangeInfos);

		// --- Get BLAS device address ---
		VkAccelerationStructureDeviceAddressInfoKHR addrInfo = {};
		addrInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
		addrInfo.accelerationStructure = blas.handle;
		blas.deviceAddress = g_RTDispatch.GetAccelerationStructureDeviceAddressKHR(device, &addrInfo);
		RT_LOG("[BuildBLASes] mesh %d: done", (int)i);
	}

	RT_LOG("[BuildBLASes] all meshes built, calling BuildCombinedBuffers");
	BuildCombinedBuffers();
	RT_LOG("[BuildBLASes] BuildCombinedBuffers done, returning true");

	return true;
}

void AccelerationStructure::BuildCombinedBuffers()
{
	VkDevice device = m_Device.device;
	RT_LOG("[BuildCombinedBuffers] enter: totalTris=%d blasCount=%d", (int)m_TotalTriangleCount, (int)m_BLASes.size());

	// Destroy previous combined buffers (already destroyed in BuildBLASes, but safe no-op)
	DestroyBuffer(m_CombinedNormalBuffer, m_CombinedNormalMemory);
	DestroyBuffer(m_InstanceOffsetBuffer, m_InstanceOffsetMemory);
	DestroyBuffer(m_CombinedUVBuffer, m_CombinedUVMemory);
	DestroyBuffer(m_CombinedPositionBuffer, m_CombinedPositionMemory);
	DestroyBuffer(m_CombinedTangentBuffer, m_CombinedTangentMemory);
	RT_LOG("[BuildCombinedBuffers] old buffers destroyed (no-op if already done)");

	// Collect all normals, positions, UVs, and tangents into combined buffers.
	// Per-triangle data layout (using vec4 arrays for std430 alignment):
	//   normals:    1 × vec4 per triangle (xyz = face normal)
	//   positions:  3 × vec4 per triangle (3 vertex positions)
	//   UVs:        3 × vec4 per triangle (3 vertex UVs in xy, zw = pad)
	//   tangents:   3 × vec4 per triangle (3 vertex tangents in xyz, w = pad)
	std::vector<glm::vec4> allNormals;
	std::vector<glm::vec4> allPositions;
	std::vector<glm::vec4> allUVs;
	std::vector<glm::vec4> allTangents;
	std::vector<uint32_t> offsets;
	allNormals.reserve(m_TotalTriangleCount);
	allPositions.reserve(m_TotalTriangleCount * 3);
	allUVs.reserve(m_TotalTriangleCount * 3);
	allTangents.reserve(m_TotalTriangleCount * 3);
	offsets.reserve(m_BLASes.size());

	for (size_t b = 0; b < m_BLASes.size(); b++)
	{
		auto& blas = m_BLASes[b];
		offsets.push_back(static_cast<uint32_t>(allNormals.size()));

		uint32_t triCount = blas.triangleCount;
		for (uint32_t t = 0; t < triCount; t++)
		{
			// Positions from stored triPositions
			glm::vec3 v0(blas.triPositions[t * 9 + 0], blas.triPositions[t * 9 + 1], blas.triPositions[t * 9 + 2]);
			glm::vec3 v1(blas.triPositions[t * 9 + 3], blas.triPositions[t * 9 + 4], blas.triPositions[t * 9 + 5]);
			glm::vec3 v2(blas.triPositions[t * 9 + 6], blas.triPositions[t * 9 + 7], blas.triPositions[t * 9 + 8]);

			allPositions.push_back(glm::vec4(v0, 0.0f));
			allPositions.push_back(glm::vec4(v1, 0.0f));
			allPositions.push_back(glm::vec4(v2, 0.0f));

			// Face normal
			glm::vec3 normal = glm::normalize(glm::cross(v1 - v0, v2 - v0));
			allNormals.push_back(glm::vec4(normal, 0.0f));

			// UVs from stored triUVs
			allUVs.push_back(glm::vec4(blas.triUVs[t * 6 + 0], blas.triUVs[t * 6 + 1], 0.0f, 0.0f));
			allUVs.push_back(glm::vec4(blas.triUVs[t * 6 + 2], blas.triUVs[t * 6 + 3], 0.0f, 0.0f));
			allUVs.push_back(glm::vec4(blas.triUVs[t * 6 + 4], blas.triUVs[t * 6 + 5], 0.0f, 0.0f));

			// Tangents from stored triTangents (9 floats per triangle = 3 × xyz)
			allTangents.push_back(glm::vec4(blas.triTangents[t * 9 + 0], blas.triTangents[t * 9 + 1], blas.triTangents[t * 9 + 2], 0.0f));
			allTangents.push_back(glm::vec4(blas.triTangents[t * 9 + 3], blas.triTangents[t * 9 + 4], blas.triTangents[t * 9 + 5], 0.0f));
			allTangents.push_back(glm::vec4(blas.triTangents[t * 9 + 6], blas.triTangents[t * 9 + 7], blas.triTangents[t * 9 + 8], 0.0f));
		}
	}

	RT_LOG("[BuildCombinedBuffers] collecting data: normals=%d positions=%d uvs=%d tangents=%d",
	       (int)allNormals.size(), (int)allPositions.size(), (int)allUVs.size(), (int)allTangents.size());

	auto createCombined = [&](VkBuffer& buf, VkDeviceMemory& mem,
	                          const void* data, VkDeviceSize size)
	{
		if (size == 0) return;
		CreateBuffer(size,
		             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		             buf, mem);
		void* mapped;
		vkMapMemory(device, mem, 0, size, 0, &mapped);
		memcpy(mapped, data, (size_t)size);
		vkUnmapMemory(device, mem);
	};

	RT_LOG("[BuildCombinedBuffers] creating normal buffer (%zu bytes)", allNormals.size() * sizeof(glm::vec4));
	createCombined(m_CombinedNormalBuffer, m_CombinedNormalMemory,
	               allNormals.data(), allNormals.size() * sizeof(glm::vec4));
	RT_LOG("[BuildCombinedBuffers] creating position buffer (%zu bytes)", allPositions.size() * sizeof(glm::vec4));
	createCombined(m_CombinedPositionBuffer, m_CombinedPositionMemory,
	               allPositions.data(), allPositions.size() * sizeof(glm::vec4));
	RT_LOG("[BuildCombinedBuffers] creating UV buffer (%zu bytes)", allUVs.size() * sizeof(glm::vec4));
	createCombined(m_CombinedUVBuffer, m_CombinedUVMemory,
	               allUVs.data(), allUVs.size() * sizeof(glm::vec4));
	RT_LOG("[BuildCombinedBuffers] creating tangent buffer (%zu bytes)", allTangents.size() * sizeof(glm::vec4));
	createCombined(m_CombinedTangentBuffer, m_CombinedTangentMemory,
	               allTangents.data(), allTangents.size() * sizeof(glm::vec4));
	RT_LOG("[BuildCombinedBuffers] creating offset buffer (%zu bytes)", offsets.size() * sizeof(uint32_t));
	createCombined(m_InstanceOffsetBuffer, m_InstanceOffsetMemory,
	               offsets.data(), offsets.size() * sizeof(uint32_t));
	RT_LOG("[BuildCombinedBuffers] done");
}

bool AccelerationStructure::BuildTLAS(VkCommandBuffer cmdBuffer,
                                       const std::vector<BLASInstance>& instances)
{
	VkDevice device = m_Device.device;

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
		vkInst.instanceShaderBindingTableRecordOffset = inst.sbtHitOffset;
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
	VkDevice device = m_Device.device;

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
	DestroyBuffer(m_CombinedUVBuffer, m_CombinedUVMemory);
	DestroyBuffer(m_CombinedPositionBuffer, m_CombinedPositionMemory);
	DestroyBuffer(m_CombinedTangentBuffer, m_CombinedTangentMemory);

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