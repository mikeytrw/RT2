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
	RT_LOG("[BuildBLASes] meshes=%d prevBLASes=%d", (int)meshes.size(), (int)m_BLASes.size());

	vkDeviceWaitIdle(device);

	// Destroy previous BLASes
	for (size_t i = 0; i < m_BLASes.size(); i++)
	{
		auto& blas = m_BLASes[i];
		GpuResources::DestroyBuffer(m_Device, blas.vertexBuffer, blas.vertexMemory);
		GpuResources::DestroyBuffer(m_Device, blas.indexBuffer, blas.indexMemory);
		GpuResources::DestroyBuffer(m_Device, blas.blasBuffer, blas.blasMemory);
		GpuResources::DestroyBuffer(m_Device, blas.scratchBuffer, blas.scratchMemory);
		if (blas.handle)
			g_RTDispatch.DestroyAccelerationStructureKHR(device, blas.handle, nullptr);
	}
	m_BLASes.clear();

	GpuResources::DestroyBuffer(m_Device, m_VertexBuffer, m_VertexMemory);
	GpuResources::DestroyBuffer(m_Device, m_IndexBuffer, m_IndexMemory);
	GpuResources::DestroyBuffer(m_Device, m_NormalBuffer, m_NormalMemory);
	GpuResources::DestroyBuffer(m_Device, m_UVBuffer, m_UVMemory);
	GpuResources::DestroyBuffer(m_Device, m_InstanceMeshInfoBuffer, m_InstanceMeshInfoMemory);
	GpuResources::DestroyBuffer(m_Device, m_MaterialIndexBuffer, m_MaterialIndexMemory);
	GpuResources::DestroyBuffer(m_Device, m_InstanceMatOffsetBuffer, m_InstanceMatOffsetMemory);

	m_BLASes.resize(meshes.size());
	m_TotalTriangleCount = 0;

	for (size_t i = 0; i < meshes.size(); i++)
	{
		const auto& mesh = meshes[i];
		BLASData& blas = m_BLASes[i];

		uint32_t triCount = static_cast<uint32_t>(mesh.indices->size() / 3);
		blas.triangleCount = triCount;
		blas.vertexCount = static_cast<uint32_t>(mesh.vertices->size() / 3);
		m_TotalTriangleCount += triCount;
		blas.srcVertices = mesh.vertices;
		blas.srcIndices = mesh.indices;
		blas.srcNormals = mesh.normals;
		blas.srcUVs = mesh.uvs;
		blas.srcMaterialIndices = mesh.materialIndices;
		blas.materialIndex = mesh.materialIndex;

		// --- Vertex buffer (DEVICE_LOCAL + staging) ---
		VkDeviceSize vertexBufferSize = mesh.vertices->size() * sizeof(float);
		{
			VkBuffer stagingBuf; VkDeviceMemory stagingMem;
			GpuResources::CreateBuffer(m_Device, vertexBufferSize,
			             VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			             stagingBuf, stagingMem);
			void* data;
			vkMapMemory(device, stagingMem, 0, vertexBufferSize, 0, &data);
			memcpy(data, mesh.vertices->data(), vertexBufferSize);
			vkUnmapMemory(device, stagingMem);

			GpuResources::CreateBuffer(m_Device, vertexBufferSize,
			             VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
			             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			             blas.vertexBuffer, blas.vertexMemory);

			VkBuffer sBuf = stagingBuf; VkDeviceMemory sMem = stagingMem; VkBuffer dB = blas.vertexBuffer; VkDeviceSize sz = vertexBufferSize;
			CommandUtils::ImmediateSubmit(m_Device, [&](VkCommandBuffer cmd) {
				VkBufferCopy region = {}; region.size = sz;
				vkCmdCopyBuffer(cmd, sBuf, dB, 1, &region);
			});
			GpuResources::DestroyBuffer(m_Device, sBuf, sMem);
		}

		// --- Index buffer (DEVICE_LOCAL + staging) ---
		VkDeviceSize indexBufferSize = mesh.indices->size() * sizeof(uint32_t);
		{
			VkBuffer stagingBuf; VkDeviceMemory stagingMem;
			GpuResources::CreateBuffer(m_Device, indexBufferSize,
			             VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			             stagingBuf, stagingMem);
			void* data;
			vkMapMemory(device, stagingMem, 0, indexBufferSize, 0, &data);
			memcpy(data, mesh.indices->data(), indexBufferSize);
			vkUnmapMemory(device, stagingMem);

			GpuResources::CreateBuffer(m_Device, indexBufferSize,
			             VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
			             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			             blas.indexBuffer, blas.indexMemory);

			VkBuffer sBuf = stagingBuf; VkDeviceMemory sMem = stagingMem; VkBuffer dB = blas.indexBuffer; VkDeviceSize sz = indexBufferSize;
			CommandUtils::ImmediateSubmit(m_Device, [&](VkCommandBuffer cmd) {
				VkBufferCopy region = {}; region.size = sz;
				vkCmdCopyBuffer(cmd, sBuf, dB, 1, &region);
			});
			GpuResources::DestroyBuffer(m_Device, sBuf, sMem);
		}

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
		g_RTDispatch.GetAccelerationStructureBuildSizesKHR(device,
			VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
			&buildInfo, &primitiveCount, &sizeInfo);

		// --- BLAS buffer ---
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
		g_RTDispatch.CreateAccelerationStructureKHR(device, &createInfo, nullptr, &blas.handle);

		// --- Scratch buffer ---
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
		g_RTDispatch.CmdBuildAccelerationStructuresKHR(cmdBuffer, 1, &buildInfo, &pBuildRangeInfos);

		// --- Get BLAS device address ---
		VkAccelerationStructureDeviceAddressInfoKHR addrInfo = {};
		addrInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
		addrInfo.accelerationStructure = blas.handle;
		blas.deviceAddress = g_RTDispatch.GetAccelerationStructureDeviceAddressKHR(device, &addrInfo);

		// NOTE: scratch/vertex/index buffers are freed after vkQueueWaitIdle
		// (in next BuildBLASes call or Destroy), NOT here — freeing during
		// command buffer recording causes VK_ERROR_DEVICE_LOST on large meshes.
	}

	RT_LOG("[BuildBLASes] done: %d BLASes, %d total tris", (int)m_BLASes.size(), (int)m_TotalTriangleCount);

	return true;
}

void AccelerationStructure::BuildAttributeBuffers()
{
	VkDevice device = m_Device.device;
	RT_LOG("[BuildAttributeBuffers] enter: blasCount=%d instances=%d",
	       (int)m_BLASes.size(), (int)m_InstanceToBLAS.size());

	// Destroy previous attribute buffers
	GpuResources::DestroyBuffer(m_Device, m_VertexBuffer, m_VertexMemory);
	GpuResources::DestroyBuffer(m_Device, m_IndexBuffer, m_IndexMemory);
	GpuResources::DestroyBuffer(m_Device, m_NormalBuffer, m_NormalMemory);
	GpuResources::DestroyBuffer(m_Device, m_UVBuffer, m_UVMemory);
	GpuResources::DestroyBuffer(m_Device, m_InstanceMeshInfoBuffer, m_InstanceMeshInfoMemory);
	GpuResources::DestroyBuffer(m_Device, m_MaterialIndexBuffer, m_MaterialIndexMemory);
	GpuResources::DestroyBuffer(m_Device, m_InstanceMatOffsetBuffer, m_InstanceMatOffsetMemory);

	// Compute total counts and per-BLAS offsets
	uint32_t totalVerts = 0, totalIndices = 0, totalNormals = 0, totalUVs = 0;
	for (const auto& blas : m_BLASes)
	{
		totalVerts += blas.vertexCount;
		totalIndices += blas.triangleCount * 3;
	}

	if (totalVerts == 0 || totalIndices == 0)
	{
		RT_LOG("[BuildAttributeBuffers] no geometry, skipping");
		return;
	}

	// Per-BLAS offsets into mega-buffers
	struct BlasOffsets { uint32_t vert, idx, norm, uv, matIdx; };
	std::vector<BlasOffsets> offsets(m_BLASes.size());

	// Per-instance material indices (from TLAS instance customIndex)
	// Used to fill the material index buffer when a mesh has no per-triangle materials.
	std::vector<uint32_t> instanceMaterialIndices;
	bool hasInstances = !m_InstanceToBLAS.empty();

	uint32_t vertOffset = 0, idxOffset = 0;
	uint32_t totalNormCount = 0, totalUVCount = 0;
	uint32_t totalMatIdxCount = 0;

	// When there are instances, material index buffer is per-instance (each
	// instance gets its own material index for all its triangles).
	// When no instances (fallback), it's per-BLAS.
	if (hasInstances)
	{
		// We need the per-instance material indices. These come from the
		// TLAS instances' customIndex, which we don't have directly here.
		// Instead, we store them during BuildTLAS.
		// For now, compute per-instance offsets.
		for (size_t inst = 0; inst < m_InstanceToBLAS.size(); inst++)
		{
			uint32_t blasIdx = m_InstanceToBLAS[inst];
			if (blasIdx < m_BLASes.size())
				totalMatIdxCount += m_BLASes[blasIdx].triangleCount;
		}
	}
	else
	{
		for (const auto& blas : m_BLASes)
			totalMatIdxCount += blas.triangleCount;
	}

	for (size_t b = 0; b < m_BLASes.size(); b++)
	{
		auto& blas = m_BLASes[b];
		offsets[b].vert = vertOffset;
		offsets[b].idx = idxOffset;
		offsets[b].norm = totalNormCount;
		offsets[b].uv = totalUVCount;
		offsets[b].matIdx = 0; // not used for per-instance material indices
		vertOffset += blas.vertexCount;
		idxOffset += blas.triangleCount * 3;
		if (blas.srcNormals && !blas.srcNormals->empty())
			totalNormCount += blas.vertexCount;
		if (blas.srcUVs && !blas.srcUVs->empty())
			totalUVCount += blas.vertexCount;
	}

	VkDeviceSize vertBufSize = (VkDeviceSize)totalVerts * sizeof(glm::vec4);
	VkDeviceSize idxBufSize  = (VkDeviceSize)totalIndices * sizeof(uint32_t);
	VkDeviceSize normBufSize = (VkDeviceSize)totalNormCount * sizeof(glm::vec4);
	VkDeviceSize uvBufSize   = (VkDeviceSize)totalUVCount * sizeof(glm::vec4);
	VkDeviceSize matIdxBufSize = (VkDeviceSize)totalMatIdxCount * sizeof(uint32_t);

	RT_LOG("[BuildAttributeBuffers] vert=%uMB idx=%uMB norm=%uMB uv=%uMB",
	       (uint32_t)(vertBufSize / (1024 * 1024)), (uint32_t)(idxBufSize / (1024 * 1024)),
	       (uint32_t)(normBufSize / (1024 * 1024)), (uint32_t)(uvBufSize / (1024 * 1024)));

	// Create DEVICE_LOCAL mega-buffers
	auto createDeviceLocal = [&](VkBuffer& buf, VkDeviceMemory& mem, VkDeviceSize size) {
		if (size == 0) return;
		GpuResources::CreateBuffer(m_Device, size,
		             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
		             VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, buf, mem);
	};
	createDeviceLocal(m_VertexBuffer, m_VertexMemory, vertBufSize);
	RT_LOG("[BuildAttributeBuffers] vertex buffer created (%zuMB)", (size_t)vertBufSize / (1024 * 1024));
	createDeviceLocal(m_IndexBuffer, m_IndexMemory, idxBufSize);
	RT_LOG("[BuildAttributeBuffers] index buffer created (%zuMB)", (size_t)idxBufSize / (1024 * 1024));
	createDeviceLocal(m_NormalBuffer, m_NormalMemory, normBufSize);
	RT_LOG("[BuildAttributeBuffers] normal buffer created (%zuMB)", (size_t)normBufSize / (1024 * 1024));
	createDeviceLocal(m_UVBuffer, m_UVMemory, uvBufSize);
	RT_LOG("[BuildAttributeBuffers] UV buffer created (%zuMB)", (size_t)uvBufSize / (1024 * 1024));
	createDeviceLocal(m_MaterialIndexBuffer, m_MaterialIndexMemory, matIdxBufSize);
	RT_LOG("[BuildAttributeBuffers] material index buffer created (%zuMB)", (size_t)matIdxBufSize / (1024 * 1024));

	RT_LOG("[BuildAttributeBuffers] starting chunked upload...");

	// Chunked upload: pack float3→vec4 in a staging buffer, one BLAS at a time.
	// This avoids building the entire vec4 array in CPU memory.
	auto uploadPackedVec4 = [&](VkBuffer dstBuf, uint32_t dstOffsetElements,
	                            const std::vector<float>* srcData, uint32_t elementCount,
	                            float w, uint32_t srcStride) {
		if (!srcData || srcData->empty() || elementCount == 0) return;
		// Sub-chunk to keep HOST_VISIBLE staging small (max 16MB = 1M vec4s)
		const uint32_t CHUNK = 1000000;
		for (uint32_t off = 0; off < elementCount; off += CHUNK)
		{
			uint32_t thisCount = std::min(CHUNK, elementCount - off);
			VkDeviceSize chunkSize = (VkDeviceSize)thisCount * sizeof(glm::vec4);
			VkBuffer stagingBuf; VkDeviceMemory stagingMem;
			GpuResources::CreateBuffer(m_Device, chunkSize,
			             VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			             stagingBuf, stagingMem);
			void* mapped = nullptr;
			vkMapMemory(device, stagingMem, 0, chunkSize, 0, &mapped);
			glm::vec4* dst = static_cast<glm::vec4*>(mapped);
			const float* src = srcData->data() + (size_t)off * srcStride;
			for (uint32_t i = 0; i < thisCount; i++)
			{
				if (srcStride == 3)
					dst[i] = glm::vec4(src[i * 3], src[i * 3 + 1], src[i * 3 + 2], w);
				else // srcStride == 2
					dst[i] = glm::vec4(src[i * 2], src[i * 2 + 1], 0.0f, w);
			}
			vkUnmapMemory(device, stagingMem);
			VkBufferCopy region = {};
			region.size = chunkSize;
			region.dstOffset = (VkDeviceSize)(dstOffsetElements + off) * sizeof(glm::vec4);
			VkBuffer sBuf = stagingBuf; VkDeviceMemory sMem = stagingMem; VkBuffer dB = dstBuf; VkDeviceSize sz = chunkSize;
			CommandUtils::ImmediateSubmit(m_Device, [&](VkCommandBuffer cmd) {
				vkCmdCopyBuffer(cmd, sBuf, dB, 1, &region);
			});
			GpuResources::DestroyBuffer(m_Device, sBuf, sMem);
		}
	};

	auto uploadRaw = [&](VkBuffer dstBuf, uint32_t dstOffsetElements,
	                     const std::vector<uint32_t>* srcData, uint32_t elementCount) {
		if (!srcData || srcData->empty() || elementCount == 0) return;
		const uint32_t CHUNK = 3000000;
		for (uint32_t off = 0; off < elementCount; off += CHUNK)
		{
			uint32_t thisCount = std::min(CHUNK, elementCount - off);
			VkDeviceSize chunkSize = (VkDeviceSize)thisCount * sizeof(uint32_t);
			VkBuffer stagingBuf; VkDeviceMemory stagingMem;
			GpuResources::CreateBuffer(m_Device, chunkSize,
			             VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			             stagingBuf, stagingMem);
			void* mapped = nullptr;
			vkMapMemory(device, stagingMem, 0, chunkSize, 0, &mapped);
			memcpy(mapped, srcData->data() + off, (size_t)chunkSize);
			vkUnmapMemory(device, stagingMem);
			VkBufferCopy region = {};
			region.size = chunkSize;
			region.dstOffset = (VkDeviceSize)(dstOffsetElements + off) * sizeof(uint32_t);
			VkBuffer sBuf = stagingBuf; VkDeviceMemory sMem = stagingMem; VkBuffer dB = dstBuf; VkDeviceSize sz = chunkSize;
			CommandUtils::ImmediateSubmit(m_Device, [&](VkCommandBuffer cmd) {
				vkCmdCopyBuffer(cmd, sBuf, dB, 1, &region);
			});
			GpuResources::DestroyBuffer(m_Device, sBuf, sMem);
		}
	};

	for (size_t b = 0; b < m_BLASes.size(); b++)
	{
		auto& blas = m_BLASes[b];
		uploadPackedVec4(m_VertexBuffer, offsets[b].vert, blas.srcVertices, blas.vertexCount, 1.0f, 3);
		uploadRaw(m_IndexBuffer, offsets[b].idx, blas.srcIndices, blas.triangleCount * 3);
		if (blas.srcNormals && !blas.srcNormals->empty())
			uploadPackedVec4(m_NormalBuffer, offsets[b].norm, blas.srcNormals, blas.vertexCount, 0.0f, 3);
		if (blas.srcUVs && !blas.srcUVs->empty())
			uploadPackedVec4(m_UVBuffer, offsets[b].uv, blas.srcUVs, blas.vertexCount, 0.0f, 2);
	}

	// Build per-instance material index buffer.
	// Each instance gets its own section. If the mesh has per-triangle
	// material indices AND the instance has no override (0xFFFFFFFF), use them.
	// Otherwise, fill with the instance's override material index.
	static const uint32_t MAT_OVERRIDE_SENTINEL = 0xFFFFFFFFu;
	std::vector<uint32_t> matIndexData;
	matIndexData.reserve(totalMatIdxCount);
	if (hasInstances)
	{
		for (size_t inst = 0; inst < m_InstanceToBLAS.size(); inst++)
		{
			uint32_t blasIdx = m_InstanceToBLAS[inst];
			if (blasIdx >= m_BLASes.size())
				continue;
			const auto& blas = m_BLASes[blasIdx];
			uint32_t instMatIdx = (inst < m_InstanceMaterialIndices.size())
				? m_InstanceMaterialIndices[inst] : MAT_OVERRIDE_SENTINEL;

			bool usePerTri = (instMatIdx == MAT_OVERRIDE_SENTINEL) &&
			                 blas.srcMaterialIndices && !blas.srcMaterialIndices->empty();
			if (usePerTri)
			{
				for (uint32_t t = 0; t < blas.triangleCount; t++)
					matIndexData.push_back((*blas.srcMaterialIndices)[t]);
			}
			else
			{
				uint32_t fillIdx = (instMatIdx == MAT_OVERRIDE_SENTINEL) ? 0u : instMatIdx;
				for (uint32_t t = 0; t < blas.triangleCount; t++)
					matIndexData.push_back(fillIdx);
			}
		}
	}
	else
	{
		for (size_t b = 0; b < m_BLASes.size(); b++)
		{
			const auto& blas = m_BLASes[b];
			if (blas.srcMaterialIndices && !blas.srcMaterialIndices->empty())
			{
				for (uint32_t t = 0; t < blas.triangleCount; t++)
					matIndexData.push_back((*blas.srcMaterialIndices)[t]);
			}
			else
			{
				for (uint32_t t = 0; t < blas.triangleCount; t++)
					matIndexData.push_back(blas.materialIndex);
			}
		}
	}

	// Upload material index buffer
	if (!matIndexData.empty())
	{
		VkDeviceSize matSize = matIndexData.size() * sizeof(uint32_t);
		RT_LOG("[MatIdx] matIndexData.size()=%zu matIdxBufSize=%zu",
		       matIndexData.size(), (size_t)matIdxBufSize);
		if (matSize > matIdxBufSize)
		{
			RT_LOG("[MatIdx] ERROR: matSize > matIdxBufSize! Clamping.");
			matSize = matIdxBufSize;
		}
		VkBuffer stagingBuf; VkDeviceMemory stagingMem;
		GpuResources::CreateBuffer(m_Device, matSize,
		             VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		             stagingBuf, stagingMem);
		void* mapped = nullptr;
		vkMapMemory(device, stagingMem, 0, matSize, 0, &mapped);
		memcpy(mapped, matIndexData.data(), (size_t)matSize);
		vkUnmapMemory(device, stagingMem);
		VkBufferCopy region = {};
		region.size = matSize;
		VkBuffer sBuf = stagingBuf; VkDeviceMemory sMem = stagingMem; VkBuffer dB = m_MaterialIndexBuffer;
		CommandUtils::ImmediateSubmit(m_Device, [&](VkCommandBuffer cmd) {
			vkCmdCopyBuffer(cmd, sBuf, dB, 1, &region);
		});
		GpuResources::DestroyBuffer(m_Device, sBuf, sMem);
	}

	// Build per-instance mesh info (uvec4 per instance: vertOffset, idxOffset, normOffset, uvOffset)
	// and per-instance material index offsets.
	std::vector<glm::uvec4> instanceMeshInfo;
	std::vector<uint32_t> instanceMatOffsets;
	if (hasInstances)
	{
		instanceMeshInfo.reserve(m_InstanceToBLAS.size());
		instanceMatOffsets.reserve(m_InstanceToBLAS.size());
		uint32_t matIdxRunning = 0;
		for (size_t inst = 0; inst < m_InstanceToBLAS.size(); inst++)
		{
			uint32_t blasIdx = m_InstanceToBLAS[inst];
			if (blasIdx < offsets.size())
			{
				instanceMeshInfo.push_back(glm::uvec4(offsets[blasIdx].vert, offsets[blasIdx].idx,
				                                      offsets[blasIdx].norm, offsets[blasIdx].uv));
				instanceMatOffsets.push_back(matIdxRunning);
				matIdxRunning += m_BLASes[blasIdx].triangleCount;
			}
			else
			{
				instanceMeshInfo.push_back(glm::uvec4(0));
				instanceMatOffsets.push_back(0);
			}
		}
	}
	else
	{
		instanceMeshInfo.reserve(m_BLASes.size());
		uint32_t matOff = 0;
		for (const auto& off : offsets)
		{
			instanceMeshInfo.push_back(glm::uvec4(off.vert, off.idx, off.norm, off.uv));
			instanceMatOffsets.push_back(matOff);
			matOff += m_BLASes[&off - offsets.data()].triangleCount;
		}
	}

	// Upload instance mesh info (HOST_VISIBLE — small, read by shaders)
	VkDeviceSize infoSize = instanceMeshInfo.size() * sizeof(glm::uvec4);
	GpuResources::CreateBuffer(m_Device, infoSize,
	             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
	             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	             m_InstanceMeshInfoBuffer, m_InstanceMeshInfoMemory);
	void* infoMapped = nullptr;
	vkMapMemory(device, m_InstanceMeshInfoMemory, 0, infoSize, 0, &infoMapped);
	memcpy(infoMapped, instanceMeshInfo.data(), (size_t)infoSize);
	vkUnmapMemory(device, m_InstanceMeshInfoMemory);

	// Upload per-instance material index offsets
	GpuResources::DestroyBuffer(m_Device, m_InstanceMatOffsetBuffer, m_InstanceMatOffsetMemory);
	VkDeviceSize matOffSize = instanceMatOffsets.size() * sizeof(uint32_t);
	if (matOffSize > 0)
	{
		GpuResources::CreateBuffer(m_Device, matOffSize,
		             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		             m_InstanceMatOffsetBuffer, m_InstanceMatOffsetMemory);
		void* matOffMapped = nullptr;
		vkMapMemory(device, m_InstanceMatOffsetMemory, 0, matOffSize, 0, &matOffMapped);
		memcpy(matOffMapped, instanceMatOffsets.data(), (size_t)matOffSize);
		vkUnmapMemory(device, m_InstanceMatOffsetMemory);
	}

	RT_LOG("[BuildAttributeBuffers] done: verts=%u indices=%u instances=%zu",
	       totalVerts, totalIndices, instanceMeshInfo.size());
}

bool AccelerationStructure::BuildTLAS(VkCommandBuffer cmdBuffer,
                                        const std::vector<BLASInstance>& instances,
                                        const std::vector<uint32_t>& instanceMeshIndices)
{
	VkDevice device = m_Device.device;

	// Store instance-to-BLAS mapping for combined buffer offset computation
	m_InstanceToBLAS = instanceMeshIndices;

	// Store per-instance material indices (from customIndex)
	m_InstanceMaterialIndices.clear();
	m_InstanceMaterialIndices.reserve(instances.size());
	for (const auto& inst : instances)
		m_InstanceMaterialIndices.push_back(inst.customIndex);

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

	// Update per-instance material indices
	m_InstanceMaterialIndices.clear();
	m_InstanceMaterialIndices.reserve(instances.size());
	for (const auto& inst : instances)
		m_InstanceMaterialIndices.push_back(inst.customIndex);

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
		GpuResources::DestroyBuffer(m_Device, blas.blasBuffer, blas.blasMemory);
		GpuResources::DestroyBuffer(m_Device, blas.scratchBuffer, blas.scratchMemory);
		if (blas.handle)
			g_RTDispatch.DestroyAccelerationStructureKHR(device, blas.handle, nullptr);
	}
	m_BLASes.clear();

	GpuResources::DestroyBuffer(m_Device, m_VertexBuffer, m_VertexMemory);
	GpuResources::DestroyBuffer(m_Device, m_IndexBuffer, m_IndexMemory);
	GpuResources::DestroyBuffer(m_Device, m_NormalBuffer, m_NormalMemory);
	GpuResources::DestroyBuffer(m_Device, m_UVBuffer, m_UVMemory);
	GpuResources::DestroyBuffer(m_Device, m_InstanceMeshInfoBuffer, m_InstanceMeshInfoMemory);
	GpuResources::DestroyBuffer(m_Device, m_MaterialIndexBuffer, m_MaterialIndexMemory);
	GpuResources::DestroyBuffer(m_Device, m_InstanceMatOffsetBuffer, m_InstanceMatOffsetMemory);

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