#include "SceneResources.h"
#include "GpuDevice.h"
#include "CommandUtils.h"
#include "StagingArena.h"
#include "RTLog.h"
#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>
#include <functional>
#include <algorithm>

SceneResources::~SceneResources()
{
	Destroy();
}

void SceneResources::InitSamplers(const GpuDevice& dev)
{
	m_Device = &dev;
	m_TextureSampler = GpuResources::CreateSampler(dev,
		VK_FILTER_LINEAR, VK_FILTER_LINEAR,
		VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_MIPMAP_MODE_LINEAR);
}

void SceneResources::Destroy()
{
	if (!m_Device) return;

	m_TextureLoader.Destroy();
	DestroyTextures();
	GpuResources::DestroyBuffer(*m_Device, m_MaterialBuffer, m_MaterialBufferMemory);
	GpuResources::DestroyBuffer(*m_Device, m_LightBuffer, m_LightBufferMemory);
	GpuResources::DestroyBuffer(*m_Device, m_InstanceTransformBuffer, m_InstanceTransformBufferMemory);
	GpuResources::DestroyBuffer(*m_Device, m_InstanceTransformPrevBuffer, m_InstanceTransformPrevBufferMemory);
	GpuResources::DestroyBuffer(*m_Device, m_InstanceMaterialIndexBuffer, m_InstanceMaterialIndexBufferMemory);
	GpuResources::DestroySampler(*m_Device, m_TextureSampler);
	m_AS.Destroy();

	m_NeedsASRebuild = false;
	m_ASJustBuilt = false;
}

void SceneResources::SetScene(const GpuDevice& dev, const GPUSceneData& sceneData)
{
	m_Device = &dev;
	VkDevice device = dev.device;
	RT_LOG("[SetScene] enter: textures=%d, envMapIndex=%d, meshes=%d, materials=%d",
	       (int)sceneData.textures.size(), sceneData.envMapIndex,
	       (int)sceneData.meshes.size(), (int)sceneData.materials.size());
	vkDeviceWaitIdle(device);

	m_CurrentScene = sceneData;
	m_NeedsASRebuild = true;

	// Note: old textures are NOT destroyed here — they stay alive until
	// PollTextureUpload() adopts the new ones, so the descriptor set
	// remains valid during the async upload window.

	// If a previous async upload is still in flight, cancel it and
	// adopt its results (or discard if incomplete) before starting a new one.
	if (m_TextureLoader.IsBusy())
	{
		m_TextureLoader.Cancel();
		m_TextureLoader.Destroy();
	}

	// Determine env map pixel data for the async loader.
	// WalnutApp appends the env map as the last texture in sceneData.textures
	// and sets envMapIndex to its index. Extract its float pixels from the
	// scene textures so the loader can rebuild the full array.
	std::vector<float> envMapFloat;
	int envMapW = 0, envMapH = 0;
	if (sceneData.envMapIndex >= 0 && sceneData.envMapIndex < (int)sceneData.textures.size())
	{
		const auto& envTex = sceneData.textures[sceneData.envMapIndex];
		if (envTex.isHDR && !envTex.floatPixels.empty())
		{
			envMapFloat = envTex.floatPixels;
			envMapW = envTex.width;
			envMapH = envTex.height;
		}
	}

	// Strip env map from the texture list passed to the loader (loader
	// re-appends it internally to match the existing convention).
	std::vector<SceneTexture> baseTextures = sceneData.textures;
	if (sceneData.envMapIndex >= 0 && sceneData.envMapIndex < (int)baseTextures.size())
		baseTextures.erase(baseTextures.begin() + sceneData.envMapIndex);

	// If there are no textures at all (e.g. Clear HDR with no scene textures),
	// destroy old textures immediately and skip the loader.
	if (baseTextures.empty() && envMapFloat.empty())
	{
		RT_LOG("[SetScene] no textures to load — destroying old textures");
		DestroyTextures();
		DestroyEnvMapCDFTextures();
		m_EnvMapIndex = -1;
		m_MarginalCDFIndex = -1;
		m_ConditionalCDFIndex = -1;
		return;
	}

	RT_LOG("[SetScene] kicking async texture loader (%d base + envMap=%dx%d)",
	       (int)baseTextures.size(), envMapW, envMapH);
	m_TextureLoader.Begin(dev, baseTextures,
	                      envMapFloat, envMapW, envMapH,
	                      sceneData.marginalCDF, sceneData.conditionalCDF,
	                      sceneData.cdfWidth, sceneData.cdfHeight);
	RT_LOG("[SetScene] done (textures loading async)");
}

bool SceneResources::PollTextureUpload()
{
	if (!m_TextureLoader.IsBusy()) return false;
	if (!m_TextureLoader.IsComplete()) return false;

	std::vector<GpuImage> newTextures;
	int envMapIdx = -1, margCDFIdx = -1, condCDFIdx = -1;
	m_TextureLoader.Adopt(newTextures, envMapIdx, margCDFIdx, condCDFIdx);

	// Now safe to destroy old textures — new ones are ready
	DestroyTextures();
	DestroyEnvMapCDFTextures();

	m_Textures = std::move(newTextures);
	m_EnvMapIndex = envMapIdx;
	m_MarginalCDFIndex = margCDFIdx;
	m_ConditionalCDFIndex = condCDFIdx;
	if (m_CurrentScene.envMapIndex >= 0)
	{
		m_CDFWidth = m_CurrentScene.cdfWidth;
		m_CDFHeight = m_CurrentScene.cdfHeight;
	}

	RT_LOG("[PollTextureUpload] textures adopted: %d (envMap=%d, margCDF=%d, condCDF=%d)",
	       (int)m_Textures.size(), m_EnvMapIndex, m_MarginalCDFIndex, m_ConditionalCDFIndex);
	return true;
}

void SceneResources::RebuildAccelerationStructures(const GpuDevice& dev,
	std::function<void(const GpuDevice&, const GPUSceneData&)> rasterPassBuild)
{
	RT_LOG("[RebuildAS] enter: meshes=%d instances=%d",
	       (int)m_CurrentScene.meshes.size(), (int)m_CurrentScene.instances.size());

	std::vector<BLASGeometry> geometries;
	geometries.reserve(m_CurrentScene.meshes.size());
	for (const auto& mesh : m_CurrentScene.meshes)
	{
		BLASGeometry geo;
		geo.vertices = &mesh.vertices;
		geo.indices = &mesh.indices;
		geo.vertexUVs = &mesh.vertexUVs;
		geo.tangents = &mesh.tangents;
		geo.materialIndex = mesh.materialIndex;

		uint32_t matIdx = mesh.materialIndex;
		if (matIdx < m_CurrentScene.materials.size())
		{
			float alphaMode = m_CurrentScene.materials[matIdx].alphaMode;
			geo.isTransparent = (alphaMode > 0.5f);
		}

		geometries.push_back(geo);
	}
	RT_LOG("[RebuildAS] built %d geometry entries, calling BuildBLASes", (int)geometries.size());

	m_AS.SetDevice(dev);
	CommandUtils::ImmediateSubmit(dev, [&](VkCommandBuffer cmd) {
		bool blasOK = m_AS.BuildBLASes(cmd, geometries);
		RT_LOG("[RebuildAS] BuildBLASes result=%d", blasOK);

		VkMemoryBarrier blasBarrier = {};
		blasBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		blasBarrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
		blasBarrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;

		vkCmdPipelineBarrier(cmd,
			VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
			VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
			0, 1, &blasBarrier, 0, nullptr, 0, nullptr);

		std::vector<BLASInstance> instances;
		std::vector<uint32_t> instanceMeshIndices;
		instances.reserve(m_CurrentScene.instances.size());
		instanceMeshIndices.reserve(m_CurrentScene.instances.size());
		for (const auto& gpuInst : m_CurrentScene.instances)
		{
			BLASInstance inst = {};
			inst.blasAddress = m_AS.GetBLASAddress(gpuInst.meshIndex);
			inst.customIndex = gpuInst.materialIndex;
			inst.sbtHitOffset = gpuInst.isTransparent ? 1u : 0u;

			const glm::mat4& w = gpuInst.worldMatrix;
			VkTransformMatrixKHR& t = inst.transform;
			t.matrix[0][0] = w[0][0]; t.matrix[0][1] = w[1][0]; t.matrix[0][2] = w[2][0]; t.matrix[0][3] = w[3][0];
			t.matrix[1][0] = w[0][1]; t.matrix[1][1] = w[1][1]; t.matrix[1][2] = w[2][1]; t.matrix[1][3] = w[3][1];
			t.matrix[2][0] = w[0][2]; t.matrix[2][1] = w[1][2]; t.matrix[2][2] = w[2][2]; t.matrix[2][3] = w[3][2];

			instances.push_back(inst);
			instanceMeshIndices.push_back(gpuInst.meshIndex);
		}

		bool tlasOK = m_AS.BuildTLAS(cmd, instances, instanceMeshIndices);
		RT_LOG("[RebuildAS] TLAS build result=%d instances=%d", tlasOK, (int)instances.size());
	});

	m_AS.BuildCombinedBuffers();

	if (rasterPassBuild)
		rasterPassBuild(dev, m_CurrentScene);

	RT_LOG("[RebuildAS] creating material buffer");
	CreateMaterialBuffer(dev);
	RT_LOG("[RebuildAS] creating light buffer");
	CreateLightBuffer(dev);
	RT_LOG("[RebuildAS] creating instance transform buffer");
	CreateInstanceTransformBuffer(dev);

	m_NeedsASRebuild = false;
	m_ASJustBuilt = true;
}

void SceneResources::UpdateInstances(const GpuDevice& dev, const GPUSceneData& sceneData)
{
	if (!m_AS.IsValid() || m_AS.GetBLASCount() == 0)
	{
		RT_LOG("[UpdateInstances] skip: AS not valid or no BLASes");
		return;
	}

	m_CurrentScene.instances = sceneData.instances;
	m_CurrentScene.lights = sceneData.lights;
	m_CurrentScene.totalLightArea = sceneData.totalLightArea;

	std::vector<BLASInstance> instances;
	std::vector<uint32_t> instanceMeshIndices;
	instances.reserve(m_CurrentScene.instances.size());
	instanceMeshIndices.reserve(m_CurrentScene.instances.size());
	for (const auto& gpuInst : m_CurrentScene.instances)
	{
		BLASInstance inst = {};
		inst.blasAddress = m_AS.GetBLASAddress(gpuInst.meshIndex);
		inst.customIndex = gpuInst.materialIndex;
		inst.sbtHitOffset = gpuInst.isTransparent ? 1u : 0u;

		const glm::mat4& w = gpuInst.worldMatrix;
		VkTransformMatrixKHR& t = inst.transform;
		t.matrix[0][0] = w[0][0]; t.matrix[0][1] = w[1][0]; t.matrix[0][2] = w[2][0]; t.matrix[0][3] = w[3][0];
		t.matrix[1][0] = w[0][1]; t.matrix[1][1] = w[1][1]; t.matrix[1][2] = w[2][1]; t.matrix[1][3] = w[3][1];
		t.matrix[2][0] = w[0][2]; t.matrix[2][1] = w[1][2]; t.matrix[2][2] = w[2][2]; t.matrix[2][3] = w[3][2];

		instances.push_back(inst);
		instanceMeshIndices.push_back(gpuInst.meshIndex);
	}

	CommandUtils::ImmediateSubmit(dev, [&](VkCommandBuffer cmd) {
		m_AS.RebuildTLASOnly(cmd, instances, instanceMeshIndices);
	});

	CreateInstanceTransformBuffer(dev);
	CreateLightBuffer(dev);

	RT_LOG("[UpdateInstances] done: instances=%d lights=%d",
	       (int)m_CurrentScene.instances.size(), (int)m_CurrentScene.lights.size());
}

void SceneResources::CreateMaterialBuffer(const GpuDevice& dev)
{
	GpuResources::DestroyBuffer(dev, m_MaterialBuffer, m_MaterialBufferMemory);

	size_t matCount = m_CurrentScene.materials.size();
	if (matCount == 0) matCount = 1;

	VkDeviceSize bufferSize = matCount * sizeof(GPUMaterial);

	GpuResources::CreateBuffer(dev, bufferSize,
	             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
	             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	             m_MaterialBuffer, m_MaterialBufferMemory);

	VkDevice device = dev.device;
	void* data;
	vkMapMemory(device, m_MaterialBufferMemory, 0, bufferSize, 0, &data);
	memcpy(data, m_CurrentScene.materials.data(), bufferSize);
	vkUnmapMemory(device, m_MaterialBufferMemory);
}

void SceneResources::CreateLightBuffer(const GpuDevice& dev)
{
	GpuResources::DestroyBuffer(dev, m_LightBuffer, m_LightBufferMemory);

	size_t lightCount = m_CurrentScene.lights.size();
	VkDeviceSize bufferSize = 16 + lightCount * sizeof(GPUTriangleLight);
	if (bufferSize < 16) bufferSize = 16;

	GpuResources::CreateBuffer(dev, bufferSize,
	             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
	             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	             m_LightBuffer, m_LightBufferMemory);

	std::vector<uint8_t> bufData(bufferSize, 0);
	uint32_t count = static_cast<uint32_t>(lightCount);
	float totalArea = m_CurrentScene.totalLightArea;
	memcpy(bufData.data() + 0,  &count,     sizeof(uint32_t));
	memcpy(bufData.data() + 4,  &totalArea, sizeof(float));
	if (lightCount > 0)
	{
		memcpy(bufData.data() + 16, m_CurrentScene.lights.data(),
		       lightCount * sizeof(GPUTriangleLight));
	}

	VkDevice device = dev.device;
	void* data;
	vkMapMemory(device, m_LightBufferMemory, 0, bufferSize, 0, &data);
	memcpy(data, bufData.data(), bufferSize);
	vkUnmapMemory(device, m_LightBufferMemory);

	RT_LOG("[RT2] Light buffer: %d lights, totalArea=%f", lightCount, totalArea);
}

void SceneResources::CreateInstanceTransformBuffer(const GpuDevice& dev)
{
	GpuResources::DestroyBuffer(dev, m_InstanceTransformBuffer, m_InstanceTransformBufferMemory);

	GpuResources::DestroyBuffer(dev, m_InstanceTransformPrevBuffer, m_InstanceTransformPrevBufferMemory);
	m_InstanceTransformPrevBuffer = m_InstanceTransformBuffer;
	m_InstanceTransformPrevBufferMemory = m_InstanceTransformBufferMemory;
	m_InstanceTransformBuffer = VK_NULL_HANDLE;
	m_InstanceTransformBufferMemory = VK_NULL_HANDLE;

	size_t instanceCount = m_CurrentScene.instances.size();
	VkDeviceSize bufferSize = instanceCount * sizeof(glm::mat4);
	if (bufferSize == 0) bufferSize = sizeof(glm::mat4);

	GpuResources::CreateBuffer(dev, bufferSize,
	             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
	             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	             m_InstanceTransformBuffer, m_InstanceTransformBufferMemory);

	std::vector<glm::mat4> transforms(instanceCount);
	for (size_t i = 0; i < instanceCount; i++)
		transforms[i] = m_CurrentScene.instances[i].worldMatrix;

	VkDevice device = dev.device;
	void* data;
	vkMapMemory(device, m_InstanceTransformBufferMemory, 0, bufferSize, 0, &data);
	memcpy(data, transforms.data(), instanceCount * sizeof(glm::mat4));
	vkUnmapMemory(device, m_InstanceTransformBufferMemory);

	if (m_InstanceTransformPrevBuffer == VK_NULL_HANDLE)
	{
		GpuResources::CreateBuffer(dev, bufferSize,
		             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		             m_InstanceTransformPrevBuffer, m_InstanceTransformPrevBufferMemory);
		vkMapMemory(device, m_InstanceTransformPrevBufferMemory, 0, bufferSize, 0, &data);
		memcpy(data, transforms.data(), instanceCount * sizeof(glm::mat4));
		vkUnmapMemory(device, m_InstanceTransformPrevBufferMemory);
	}

	GpuResources::DestroyBuffer(dev, m_InstanceMaterialIndexBuffer, m_InstanceMaterialIndexBufferMemory);
	VkDeviceSize matIdxSize = instanceCount * sizeof(uint32_t);
	if (matIdxSize == 0) matIdxSize = sizeof(uint32_t);
	GpuResources::CreateBuffer(dev, matIdxSize,
	             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
	             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	             m_InstanceMaterialIndexBuffer, m_InstanceMaterialIndexBufferMemory);

	std::vector<uint32_t> matIndices(instanceCount);
	for (size_t i = 0; i < instanceCount; i++)
		matIndices[i] = m_CurrentScene.instances[i].materialIndex;

	vkMapMemory(device, m_InstanceMaterialIndexBufferMemory, 0, matIdxSize, 0, &data);
	memcpy(data, matIndices.data(), instanceCount * sizeof(uint32_t));
	vkUnmapMemory(device, m_InstanceMaterialIndexBufferMemory);

	RT_LOG("[RT2] Instance transform buffer: %d instances (%zu bytes)",
	       (int)instanceCount, (size_t)bufferSize);
}

void SceneResources::CreateTextures(const GpuDevice& dev, const std::vector<SceneTexture>& textures)
{
	VkDevice device = dev.device;
	DestroyTextures();

	m_Textures.resize(textures.size());

	VkDeviceSize totalStagingSize = 0;
	for (size_t i = 0; i < textures.size(); i++)
	{
		const auto& tex = textures[i];
		if (tex.floatPixels.empty() && tex.pixels.empty())
			continue;

		bool isHDR = tex.isHDR && !tex.floatPixels.empty();
		VkDeviceSize imageSize = isHDR
			? (VkDeviceSize)(tex.width * tex.height * 8)
			: (VkDeviceSize)(tex.width * tex.height * 4);

		totalStagingSize += imageSize;
	}

	if (totalStagingSize == 0)
	{
		RT_LOG("[RT2] No textures to create");
		return;
	}

	StagingArena arena;
	if (!arena.Init(dev, totalStagingSize))
	{
		RT_LOG("[RT2] Failed to create staging arena for textures");
		return;
	}

	struct TextureUpload {
		VkDeviceSize offset;
		VkDeviceSize size;
		bool isHDR;
		uint32_t mipLevels;
	};
	std::vector<TextureUpload> uploads(textures.size());

	for (size_t i = 0; i < textures.size(); i++)
	{
		const auto& tex = textures[i];
		if (tex.floatPixels.empty() && tex.pixels.empty())
		{
			RT_LOG("[RT2] Texture %d: no pixel data, skipping", (int)i);
			continue;
		}

		GpuImage& gt = m_Textures[i];
		gt.width = tex.width;
		gt.height = tex.height;

		bool isHDR = tex.isHDR && !tex.floatPixels.empty();
		VkFormat format;
		if (isHDR)
			format = VK_FORMAT_R16G16B16A16_SFLOAT;
		else
			format = tex.isSRGB ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
		gt.format = format;

		uint32_t mipLevels = 1;
		uint32_t w = tex.width, h = tex.height;
		while (w > 1 || h > 1) { w = std::max(w / 2, 1u); h = std::max(h / 2, 1u); mipLevels++; }

		VkDeviceSize imageSize = isHDR
			? (VkDeviceSize)(tex.width * tex.height * 8)
			: (VkDeviceSize)(tex.width * tex.height * 4);

		VkDeviceSize offset = arena.Alloc(imageSize, 16);
		if (offset == VK_WHOLE_SIZE)
		{
			RT_LOG("[RT2] Texture %d: staging arena out of space", (int)i);
			continue;
		}

		if (isHDR)
		{
			uint16_t* dst = static_cast<uint16_t*>(arena.GetMappedPointer(offset));
			for (size_t p = 0; p < tex.floatPixels.size(); p++)
				dst[p] = glm::packHalf1x16(tex.floatPixels[p]);
		}
		else
		{
			memcpy(arena.GetMappedPointer(offset), tex.pixels.data(), (size_t)imageSize);
		}

		uploads[i] = { offset, imageSize, isHDR, mipLevels };

		GpuResources::CreateImage(dev, tex.width, tex.height, format,
			VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, gt, mipLevels);
	}

	VkBuffer stagingBuffer = arena.GetBuffer();
	CommandUtils::ImmediateSubmit(dev, [&](VkCommandBuffer cmd) {
		for (size_t i = 0; i < textures.size(); i++)
		{
			const auto& tex = textures[i];
			if (tex.floatPixels.empty() && tex.pixels.empty()) continue;

			GpuImage& gt = m_Textures[i];
			const auto& up = uploads[i];

			// Transition mip 0 to TRANSFER_DST_OPTIMAL
			VkImageMemoryBarrier barrier = {};
			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			barrier.srcAccessMask = 0;
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = gt.image;
			barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			barrier.subresourceRange.baseMipLevel = 0;
			barrier.subresourceRange.levelCount = up.mipLevels;
			barrier.subresourceRange.layerCount = 1;

			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

			VkBufferImageCopy region = {};
			region.bufferOffset = up.offset;
			region.bufferRowLength = 0;
			region.bufferImageHeight = 0;
			region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			region.imageSubresource.mipLevel = 0;
			region.imageSubresource.baseArrayLayer = 0;
			region.imageSubresource.layerCount = 1;
			region.imageOffset = {0, 0, 0};
			region.imageExtent = {(uint32_t)tex.width, (uint32_t)tex.height, 1};

			vkCmdCopyBufferToImage(cmd, stagingBuffer, gt.image,
			                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

			// Generate mip chain via blits (if more than 1 mip)
			if (up.mipLevels > 1)
			{
				uint32_t mipW = (uint32_t)tex.width;
				uint32_t mipH = (uint32_t)tex.height;

				for (uint32_t mip = 1; mip < up.mipLevels; mip++)
				{
					// Transition previous mip to TRANSFER_SRC_OPTIMAL
					VkImageMemoryBarrier srcBarrier = {};
					srcBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
					srcBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
					srcBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
					srcBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
					srcBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
					srcBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
					srcBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
					srcBarrier.image = gt.image;
					srcBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
					srcBarrier.subresourceRange.baseMipLevel = mip - 1;
					srcBarrier.subresourceRange.levelCount = 1;
					srcBarrier.subresourceRange.layerCount = 1;

					vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
					                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &srcBarrier);

					uint32_t dstW = std::max(mipW / 2, 1u);
					uint32_t dstH = std::max(mipH / 2, 1u);

					VkImageBlit blit = {};
					blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
					blit.srcSubresource.mipLevel = mip - 1;
					blit.srcSubresource.baseArrayLayer = 0;
					blit.srcSubresource.layerCount = 1;
					blit.srcOffsets[0] = {0, 0, 0};
					blit.srcOffsets[1] = {(int32_t)mipW, (int32_t)mipH, 1};
					blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
					blit.dstSubresource.mipLevel = mip;
					blit.dstSubresource.baseArrayLayer = 0;
					blit.dstSubresource.layerCount = 1;
					blit.dstOffsets[0] = {0, 0, 0};
					blit.dstOffsets[1] = {(int32_t)dstW, (int32_t)dstH, 1};

					vkCmdBlitImage(cmd, gt.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					              gt.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					              1, &blit, VK_FILTER_LINEAR);

					mipW = dstW;
					mipH = dstH;
				}
			}

			// Transition all mips to SHADER_READ_ONLY_OPTIMAL
			VkImageMemoryBarrier shaderBarrier = {};
			shaderBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			shaderBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			shaderBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			shaderBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			shaderBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			shaderBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			shaderBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			shaderBarrier.image = gt.image;
			shaderBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			shaderBarrier.subresourceRange.baseMipLevel = 0;
			shaderBarrier.subresourceRange.levelCount = up.mipLevels;
			shaderBarrier.subresourceRange.layerCount = 1;

			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
			                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &shaderBarrier);
		}
	});

	RT_LOG("[RT2] Created %d GPU textures (staging arena: %zu bytes, %zu used)",
	       (int)m_Textures.size(), (size_t)arena.GetCapacity(), (size_t)arena.GetUsed());
}

void SceneResources::DestroyTextures()
{
	if (!m_Device) return;
	for (auto& gt : m_Textures)
		GpuResources::DestroyImage(*m_Device, gt);
	m_Textures.clear();
}

void SceneResources::CreateEnvMapCDFTextures(const GpuDevice& dev, const GPUSceneData& sceneData)
{
	m_EnvMapIndex = sceneData.envMapIndex;
	m_CDFWidth = sceneData.cdfWidth;
	m_CDFHeight = sceneData.cdfHeight;
	m_MarginalCDFIndex = -1;
	m_ConditionalCDFIndex = -1;

	if (sceneData.envMapIndex < 0 || sceneData.marginalCDF.empty() || sceneData.conditionalCDF.empty())
		return;

	VkDeviceSize marginalSize = (VkDeviceSize)(sceneData.cdfHeight * 1 * 4);
	VkDeviceSize conditionalSize = (VkDeviceSize)(sceneData.cdfWidth * sceneData.cdfHeight * 4);

	StagingArena arena;
	if (!arena.Init(dev, marginalSize + conditionalSize))
	{
		RT_LOG("[RT2] Failed to create staging arena for CDF textures");
		return;
	}

	struct CDFEntry {
		GpuImage img;
		VkDeviceSize offset;
		int w, h;
	};

	auto prepareCDFTexture = [&](const std::vector<float>& cdfData, int w, int h) -> CDFEntry {
		CDFEntry e;
		e.img.width = w;
		e.img.height = h;
		e.img.format = VK_FORMAT_R32_SFLOAT;
		e.w = w;
		e.h = h;
		VkDeviceSize imageSize = (VkDeviceSize)(w * h * 4);

		e.offset = arena.Alloc(imageSize, 16);
		memcpy(arena.GetMappedPointer(e.offset), cdfData.data(), (size_t)imageSize);

		GpuResources::CreateImage1D(dev, w, h, VK_FORMAT_R32_SFLOAT,
			VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, e.img);
		return e;
	};

	CDFEntry entries[2];
	entries[0] = prepareCDFTexture(sceneData.marginalCDF, sceneData.cdfHeight, 1);
	entries[1] = prepareCDFTexture(sceneData.conditionalCDF, sceneData.cdfWidth, sceneData.cdfHeight);

	VkBuffer stagingBuffer = arena.GetBuffer();
	CommandUtils::ImmediateSubmit(dev, [&](VkCommandBuffer cmd) {
		for (int i = 0; i < 2; i++)
		{
			const auto& e = entries[i];
			VkImageMemoryBarrier barrier = {};
			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barrier.image = e.img.image;
			barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			barrier.subresourceRange.levelCount = 1;
			barrier.subresourceRange.layerCount = 1;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

			VkBufferImageCopy region = {};
			region.bufferOffset = e.offset;
			region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			region.imageSubresource.layerCount = 1;
			region.imageExtent = {(uint32_t)e.w, (e.h == 1) ? 1u : (uint32_t)e.h, 1};
			vkCmdCopyBufferToImage(cmd, stagingBuffer, e.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

			VkImageMemoryBarrier shaderBarrier = barrier;
			shaderBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			shaderBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			shaderBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			shaderBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
			                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &shaderBarrier);
		}
	});

	for (int i = 0; i < 2; i++)
	{
		int idx = (int)m_Textures.size();
		m_Textures.push_back(entries[i].img);
		if (i == 0) m_MarginalCDFIndex = idx;
		else        m_ConditionalCDFIndex = idx;
	}

	RT_LOG("[RT2] Env map CDF textures: marginal idx=%d conditional idx=%d (%dx%d)",
	       m_MarginalCDFIndex, m_ConditionalCDFIndex, sceneData.cdfWidth, sceneData.cdfHeight);
}

void SceneResources::DestroyEnvMapCDFTextures()
{
	m_EnvMapIndex = -1;
	m_MarginalCDFIndex = -1;
	m_ConditionalCDFIndex = -1;
}