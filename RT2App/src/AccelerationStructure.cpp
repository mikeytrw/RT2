#include "AccelerationStructure.h"
#include "RTLog.h"
#include "VulkanUtils.h"
#include "GpuResources.h"
#include "CommandUtils.h"
#include "Walnut/RTDispatch.h"
#include <glm/glm.hpp>
#include <cmath>
#include <iostream>
#include <cstring>

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
		GpuResources::DestroyBuffer(m_Device, blas.vertexBuffer, blas.vertexMemory);
		GpuResources::DestroyBuffer(m_Device, blas.indexBuffer, blas.indexMemory);
		GpuResources::DestroyBuffer(m_Device, blas.normalBuffer, blas.normalMemory);
		GpuResources::DestroyBuffer(m_Device, blas.blasBuffer, blas.blasMemory);
		GpuResources::DestroyBuffer(m_Device, blas.scratchBuffer, blas.scratchMemory);
		if (blas.handle)
			g_RTDispatch.DestroyAccelerationStructureKHR(device, blas.handle, nullptr);
	}
	m_BLASes.clear();
	RT_LOG("[BuildBLASes] old BLASes destroyed, destroying combined buffers");

	GpuResources::DestroyBuffer(m_Device, m_CombinedNormalBuffer, m_CombinedNormalMemory);
	GpuResources::DestroyBuffer(m_Device, m_InstanceOffsetBuffer, m_InstanceOffsetMemory);
	GpuResources::DestroyBuffer(m_Device, m_CombinedUVBuffer, m_CombinedUVMemory);
	GpuResources::DestroyBuffer(m_Device, m_CombinedPositionBuffer, m_CombinedPositionMemory);
	GpuResources::DestroyBuffer(m_Device, m_CombinedTangentBuffer, m_CombinedTangentMemory);
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
		GpuResources::CreateBuffer(m_Device, vertexBufferSize,
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
		GpuResources::CreateBuffer(m_Device, indexBufferSize,
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
		trianglesData.vertexData.deviceAddress = m_Device.GetBufferDeviceAddress(blas.vertexBuffer);
		trianglesData.vertexStride = 3 * sizeof(float);
		trianglesData.maxVertex = static_cast<uint32_t>(mesh.vertices->size() / 3) - 1;
		trianglesData.indexType = VK_INDEX_TYPE_UINT32;
		trianglesData.indexData.deviceAddress = m_Device.GetBufferDeviceAddress(blas.indexBuffer);
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
		GpuResources::CreateBuffer(m_Device, sizeInfo.accelerationStructureSize,
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
		GpuResources::CreateBuffer(m_Device, sizeInfo.buildScratchSize,
		             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		             blas.scratchBuffer, blas.scratchMemory);

		buildInfo.dstAccelerationStructure = blas.handle;
		buildInfo.scratchData.deviceAddress = m_Device.GetBufferDeviceAddress(blas.scratchBuffer);

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

	RT_LOG("[BuildBLASes] all meshes built, BuildCombinedBuffers deferred to after BuildTLAS");

	return true;
}

void AccelerationStructure::BuildCombinedBuffers()
{
	VkDevice device = m_Device.device;
	RT_LOG("[BuildCombinedBuffers] enter: totalTris=%d blasCount=%d instances=%d",
	       (int)m_TotalTriangleCount, (int)m_BLASes.size(), (int)m_InstanceToBLAS.size());

	// Destroy previous combined buffers (already destroyed in BuildBLASes, but safe no-op)
	GpuResources::DestroyBuffer(m_Device, m_CombinedNormalBuffer, m_CombinedNormalMemory);
	GpuResources::DestroyBuffer(m_Device, m_InstanceOffsetBuffer, m_InstanceOffsetMemory);
	GpuResources::DestroyBuffer(m_Device, m_CombinedUVBuffer, m_CombinedUVMemory);
	GpuResources::DestroyBuffer(m_Device, m_CombinedPositionBuffer, m_CombinedPositionMemory);
	GpuResources::DestroyBuffer(m_Device, m_CombinedTangentBuffer, m_CombinedTangentMemory);
	RT_LOG("[BuildCombinedBuffers] old buffers destroyed (no-op if already done)");

	// Pre-compute total triangle count for direct GPU buffer allocation.
	// For large scenes (5.6M tris), building intermediate std::vector<glm::vec4>
	// arrays would require ~896MB of CPU RAM on top of the BLAS data. Instead,
	// we allocate the GPU buffers directly and write via mapped memory.
	uint32_t totalTriCount = 0;
	for (size_t b = 0; b < m_BLASes.size(); b++)
		totalTriCount += m_BLASes[b].triangleCount;

	if (totalTriCount == 0)
	{
		RT_LOG("[BuildCombinedBuffers] no triangles, skipping");
		return;
	}

	VkDeviceSize normalSize   = (VkDeviceSize)totalTriCount * sizeof(glm::vec4);
	VkDeviceSize positionSize = (VkDeviceSize)totalTriCount * 3 * sizeof(glm::vec4);
	VkDeviceSize uvSize       = (VkDeviceSize)totalTriCount * 3 * sizeof(glm::vec4);
	VkDeviceSize tangentSize  = (VkDeviceSize)totalTriCount * 3 * sizeof(glm::vec4);

	RT_LOG("[BuildCombinedBuffers] totalTris=%u, GPU buffers: normal=%zuMB pos=%zuMB uv=%zuMB tan=%zuMB",
	       totalTriCount,
	       normalSize/(1024*1024), positionSize/(1024*1024),
	       uvSize/(1024*1024), tangentSize/(1024*1024));

	// Allocate GPU-side DEVICE_LOCAL buffers and upload via a staging buffer.
	// HOST_VISIBLE memory is limited (~256MB-1GB on most GPUs), so we can't
	// map 856MB directly. Instead, build the data in a temporary CPU buffer,
	// create a DEVICE_LOCAL GPU buffer, and copy via a staging buffer.
	auto createDeviceLocal = [&](VkBuffer& buf, VkDeviceMemory& mem,
	                              const std::vector<glm::vec4>& data) {
		if (data.empty()) return;
		VkDeviceSize size = (VkDeviceSize)(data.size() * sizeof(glm::vec4));
		GpuResources::CreateBuffer(m_Device, size,
		             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
		             VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		             buf, mem);
		VkBuffer stagingBuf; VkDeviceMemory stagingMem;
		GpuResources::CreateBuffer(m_Device, size,
		             VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		             stagingBuf, stagingMem);
		void* mapped = nullptr;
		vkMapMemory(device, stagingMem, 0, size, 0, &mapped);
		memcpy(mapped, data.data(), (size_t)size);
		vkUnmapMemory(device, stagingMem);
		VkBuffer stagingBufCapture = stagingBuf;
		VkDeviceMemory stagingMemCapture = stagingMem;
		VkBuffer dstBufCapture = buf;
		VkDeviceSize sizeCapture = size;
		CommandUtils::ImmediateSubmit(m_Device, [&](VkCommandBuffer cmd) {
			VkBufferCopy region = {}; region.size = sizeCapture;
			vkCmdCopyBuffer(cmd, stagingBufCapture, dstBufCapture, 1, &region);
		});
		GpuResources::DestroyBuffer(m_Device, stagingBufCapture, stagingMemCapture);
	};

	// Build per-BLAS triangle data into CPU-side vectors, then upload to
	// DEVICE_LOCAL GPU buffers via staging. Free BLAS per-triangle data after
	// copying to reduce peak memory.
	std::vector<glm::vec4> allNormals, allPositions, allUVs, allTangents;
	allNormals.reserve(totalTriCount);
	allPositions.reserve(totalTriCount * 3);
	allUVs.reserve(totalTriCount * 3);
	allTangents.reserve(totalTriCount * 3);
	std::vector<uint32_t> blasOffsets(m_BLASes.size());

	RT_LOG("[BuildCombinedBuffers] vectors reserved, starting BLAS loop");

	try
	{
	for (size_t b = 0; b < m_BLASes.size(); b++)
	{
		auto& blas = m_BLASes[b];
		blasOffsets[b] = static_cast<uint32_t>(allNormals.size());

		uint32_t triCount = blas.triangleCount;
		RT_LOG("[BuildCombinedBuffers] blas=%zu: triCount=%u triPositions.size=%zu triUVs.size=%zu triTangents.size=%zu",
		       b, triCount, blas.triPositions.size(), blas.triUVs.size(), blas.triTangents.size());
		for (uint32_t t = 0; t < triCount; t++)
		{
			if (t % 1000000 == 0)
				RT_LOG("[BuildCombinedBuffers] blas=%zu tri=%u/%u", b, t, triCount);

			size_t posIdx = (size_t)t * 9;
			size_t uvIdx = (size_t)t * 6;
			size_t tanIdx = (size_t)t * 9;

			if (posIdx + 8 >= blas.triPositions.size() ||
			    uvIdx + 5 >= blas.triUVs.size() ||
			    tanIdx + 8 >= blas.triTangents.size())
			{
				RT_LOG("[BuildCombinedBuffers] bounds check FAILED: blas=%zu tri=%u posIdx=%zu/%zu uvIdx=%zu/%zu tanIdx=%zu/%zu",
				       b, t, posIdx, blas.triPositions.size(),
				       uvIdx, blas.triUVs.size(),
				       tanIdx, blas.triTangents.size());
				continue;
			}

			glm::vec3 v0(blas.triPositions[posIdx + 0], blas.triPositions[posIdx + 1], blas.triPositions[posIdx + 2]);
			glm::vec3 v1(blas.triPositions[posIdx + 3], blas.triPositions[posIdx + 4], blas.triPositions[posIdx + 5]);
			glm::vec3 v2(blas.triPositions[posIdx + 6], blas.triPositions[posIdx + 7], blas.triPositions[posIdx + 8]);

			allPositions.push_back(glm::vec4(v0, 1.0f));
			allPositions.push_back(glm::vec4(v1, 1.0f));
			allPositions.push_back(glm::vec4(v2, 1.0f));

			glm::vec3 normal = glm::normalize(glm::cross(v1 - v0, v2 - v0));
			allNormals.push_back(glm::vec4(normal, 0.0f));

			allUVs.push_back(glm::vec4(blas.triUVs[t * 6 + 0], blas.triUVs[t * 6 + 1], 0.0f, 0.0f));
			allUVs.push_back(glm::vec4(blas.triUVs[t * 6 + 2], blas.triUVs[t * 6 + 3], 0.0f, 0.0f));
			allUVs.push_back(glm::vec4(blas.triUVs[t * 6 + 4], blas.triUVs[t * 6 + 5], 0.0f, 0.0f));

			allTangents.push_back(glm::vec4(blas.triTangents[t * 9 + 0], blas.triTangents[t * 9 + 1], blas.triTangents[t * 9 + 2], 0.0f));
			allTangents.push_back(glm::vec4(blas.triTangents[t * 9 + 3], blas.triTangents[t * 9 + 4], blas.triTangents[t * 9 + 5], 0.0f));
			allTangents.push_back(glm::vec4(blas.triTangents[t * 9 + 6], blas.triTangents[t * 9 + 7], blas.triTangents[t * 9 + 8], 0.0f));
		}

		// Free BLAS per-triangle data to reduce peak memory
		blas.triPositions.clear();
		blas.triPositions.shrink_to_fit();
		blas.triUVs.clear();
		blas.triUVs.shrink_to_fit();
		blas.triTangents.clear();
		blas.triTangents.shrink_to_fit();
	}
	}
	catch (const std::exception& e)
	{
		RT_LOG("[BuildCombinedBuffers] EXCEPTION: %s", e.what());
		return;
	}

	RT_LOG("[BuildCombinedBuffers] uploading to DEVICE_LOCAL: normals=%zuMB pos=%zuMB uv=%zuMB tan=%zuMB",
	       allNormals.size() * sizeof(glm::vec4) / (1024*1024),
	       allPositions.size() * sizeof(glm::vec4) / (1024*1024),
	       allUVs.size() * sizeof(glm::vec4) / (1024*1024),
	       allTangents.size() * sizeof(glm::vec4) / (1024*1024));

	createDeviceLocal(m_CombinedNormalBuffer, m_CombinedNormalMemory, allNormals);
	createDeviceLocal(m_CombinedPositionBuffer, m_CombinedPositionMemory, allPositions);
	createDeviceLocal(m_CombinedUVBuffer, m_CombinedUVMemory, allUVs);
	createDeviceLocal(m_CombinedTangentBuffer, m_CombinedTangentMemory, allTangents);

	// Free CPU-side vectors after upload
	allNormals.clear(); allNormals.shrink_to_fit();
	allPositions.clear(); allPositions.shrink_to_fit();
	allUVs.clear(); allUVs.shrink_to_fit();
	allTangents.clear(); allTangents.shrink_to_fit();

	// Build per-instance offset table: each instance maps to its BLAS's offset.
	std::vector<uint32_t> offsets;
	if (!m_InstanceToBLAS.empty())
	{
		offsets.reserve(m_InstanceToBLAS.size());
		for (size_t inst = 0; inst < m_InstanceToBLAS.size(); inst++)
		{
			uint32_t blasIdx = m_InstanceToBLAS[inst];
			offsets.push_back((blasIdx < blasOffsets.size()) ? blasOffsets[blasIdx] : 0u);
		}
	}
	else
	{
		offsets = blasOffsets;
	}

	RT_LOG("[BuildCombinedBuffers] instances=%zu, creating offset buffer (%zu bytes)",
	       offsets.size(), offsets.size() * sizeof(uint32_t));

	VkDeviceSize offsetSize = offsets.size() * sizeof(uint32_t);
	GpuResources::CreateBuffer(m_Device, offsetSize,
	             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
	             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	             m_InstanceOffsetBuffer, m_InstanceOffsetMemory);
	void* offsetMapped = nullptr;
	vkMapMemory(device, m_InstanceOffsetMemory, 0, offsetSize, 0, &offsetMapped);
	memcpy(offsetMapped, offsets.data(), (size_t)offsetSize);
	vkUnmapMemory(device, m_InstanceOffsetMemory);

	RT_LOG("[BuildCombinedBuffers] done");
}

bool AccelerationStructure::BuildTLAS(VkCommandBuffer cmdBuffer,
                                        const std::vector<BLASInstance>& instances,
                                        const std::vector<uint32_t>& instanceMeshIndices)
{
	VkDevice device = m_Device.device;

	// Store instance-to-BLAS mapping for combined buffer offset computation
	m_InstanceToBLAS = instanceMeshIndices;

	// Destroy previous TLAS
	if (m_TLAS)
	{
		g_RTDispatch.DestroyAccelerationStructureKHR(device, m_TLAS, nullptr);
		m_TLAS = VK_NULL_HANDLE;
	}
	GpuResources::DestroyBuffer(m_Device, m_TLASBuffer, m_TLASMemory);
	GpuResources::DestroyBuffer(m_Device, m_InstanceBuffer, m_InstanceMemory);
	GpuResources::DestroyBuffer(m_Device, m_TLASScratchBuffer, m_TLASScratchMemory);

	// --- Instance buffer ---
	// Allocate at least 1 byte so the buffer has a valid device address
	// even with 0 instances (Vulkan allows TLAS build with primitiveCount=0).
	VkDeviceSize instanceBufferSize = instances.size() * sizeof(VkAccelerationStructureInstanceKHR);
	if (instanceBufferSize == 0)
		instanceBufferSize = 1;
	GpuResources::CreateBuffer(m_Device, instanceBufferSize,
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
	if (!vkInstances.empty())
		memcpy(instanceData, vkInstances.data(), vkInstances.size() * sizeof(VkAccelerationStructureInstanceKHR));
	vkUnmapMemory(device, m_InstanceMemory);

	// --- TLAS geometry ---
	VkAccelerationStructureGeometryKHR geometry = {};
	geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;

	VkAccelerationStructureGeometryInstancesDataKHR instancesData = {};
	instancesData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	instancesData.data.deviceAddress = m_Device.GetBufferDeviceAddress(m_InstanceBuffer);
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
	GpuResources::CreateBuffer(m_Device, sizeInfo.accelerationStructureSize,
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
	GpuResources::CreateBuffer(m_Device, sizeInfo.buildScratchSize,
	             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
	             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
	             m_TLASScratchBuffer, m_TLASScratchMemory);

	buildInfo.dstAccelerationStructure = m_TLAS;
	buildInfo.scratchData.deviceAddress = m_Device.GetBufferDeviceAddress(m_TLASScratchBuffer);

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

bool AccelerationStructure::RebuildTLASOnly(VkCommandBuffer cmdBuffer,
                                             const std::vector<BLASInstance>& instances,
                                             const std::vector<uint32_t>& instanceMeshIndices)
{
	VkDevice device = m_Device.device;

	if (m_BLASes.empty())
	{
		RT_LOG("[RebuildTLASOnly] No BLASes — need full rebuild first");
		return false;
	}

	// Update instance-to-BLAS mapping
	m_InstanceToBLAS = instanceMeshIndices;

	// Destroy previous TLAS (keep BLASes)
	if (m_TLAS)
	{
		g_RTDispatch.DestroyAccelerationStructureKHR(device, m_TLAS, nullptr);
		m_TLAS = VK_NULL_HANDLE;
	}
	GpuResources::DestroyBuffer(m_Device, m_TLASBuffer, m_TLASMemory);
	GpuResources::DestroyBuffer(m_Device, m_InstanceBuffer, m_InstanceMemory);
	GpuResources::DestroyBuffer(m_Device, m_TLASScratchBuffer, m_TLASScratchMemory);

	// --- Instance buffer ---
	// Allocate at least 1 byte for valid device address with 0 instances.
	VkDeviceSize instanceBufferSize = instances.size() * sizeof(VkAccelerationStructureInstanceKHR);
	if (instanceBufferSize == 0)
		instanceBufferSize = 1;
	GpuResources::CreateBuffer(m_Device, instanceBufferSize,
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
	if (!vkInstances.empty())
		memcpy(instanceData, vkInstances.data(), vkInstances.size() * sizeof(VkAccelerationStructureInstanceKHR));
	vkUnmapMemory(device, m_InstanceMemory);

	// --- TLAS geometry ---
	VkAccelerationStructureGeometryKHR geometry = {};
	geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;

	VkAccelerationStructureGeometryInstancesDataKHR instancesData = {};
	instancesData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	instancesData.data.deviceAddress = m_Device.GetBufferDeviceAddress(m_InstanceBuffer);
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
	GpuResources::CreateBuffer(m_Device, sizeInfo.accelerationStructureSize,
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
	GpuResources::CreateBuffer(m_Device, sizeInfo.buildScratchSize,
	             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
	             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
	             m_TLASScratchBuffer, m_TLASScratchMemory);

	buildInfo.dstAccelerationStructure = m_TLAS;
	buildInfo.scratchData.deviceAddress = m_Device.GetBufferDeviceAddress(m_TLASScratchBuffer);

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

	RT_LOG("[RebuildTLASOnly] done: instances=%d", (int)instances.size());
	return true;
}

void AccelerationStructure::Destroy()
{
	VkDevice device = m_Device.device;

	for (auto& blas : m_BLASes)
	{
		GpuResources::DestroyBuffer(m_Device, blas.vertexBuffer, blas.vertexMemory);
		GpuResources::DestroyBuffer(m_Device, blas.indexBuffer, blas.indexMemory);
		GpuResources::DestroyBuffer(m_Device, blas.normalBuffer, blas.normalMemory);
		GpuResources::DestroyBuffer(m_Device, blas.blasBuffer, blas.blasMemory);
		GpuResources::DestroyBuffer(m_Device, blas.scratchBuffer, blas.scratchMemory);
		if (blas.handle)
			g_RTDispatch.DestroyAccelerationStructureKHR(device, blas.handle, nullptr);
	}
	m_BLASes.clear();

	GpuResources::DestroyBuffer(m_Device, m_CombinedNormalBuffer, m_CombinedNormalMemory);
	GpuResources::DestroyBuffer(m_Device, m_InstanceOffsetBuffer, m_InstanceOffsetMemory);
	GpuResources::DestroyBuffer(m_Device, m_CombinedUVBuffer, m_CombinedUVMemory);
	GpuResources::DestroyBuffer(m_Device, m_CombinedPositionBuffer, m_CombinedPositionMemory);
	GpuResources::DestroyBuffer(m_Device, m_CombinedTangentBuffer, m_CombinedTangentMemory);

	if (m_TLAS)
	{
		g_RTDispatch.DestroyAccelerationStructureKHR(device, m_TLAS, nullptr);
		m_TLAS = VK_NULL_HANDLE;
	}
	GpuResources::DestroyBuffer(m_Device, m_TLASBuffer, m_TLASMemory);
	GpuResources::DestroyBuffer(m_Device, m_InstanceBuffer, m_InstanceMemory);
	GpuResources::DestroyBuffer(m_Device, m_TLASScratchBuffer, m_TLASScratchMemory);

	m_TLASDeviceAddress = 0;
	m_TotalTriangleCount = 0;
}