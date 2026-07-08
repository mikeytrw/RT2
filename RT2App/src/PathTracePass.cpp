#include "PathTracePass.h"
#include "GpuDevice.h"
#include "GpuResources.h"
#include "VulkanUtils.h"
#include "ShaderManager.h"
#include "shader_interface.h"
#include "Walnut/RTDispatch.h"
#include "RTLog.h"
#include <iostream>
#include <cstring>
#include <algorithm>
#include <vector>

bool PathTracePass::Init(const GpuDevice& dev, VkDescriptorSetLayout gbufferSetLayout)
{
	if (m_Pipeline != VK_NULL_HANDLE) return true;
	m_Device = dev.device;

	// Load the seven RT shader modules (6 original + secondary_raygen for raster-first).
	m_RgenShader   = ShaderManager::LoadShader("raygen.spv");
	if (!m_RgenShader)   m_RgenShader   = ShaderManager::LoadShader("RT2App/shaders/raygen.spv");
	m_SecondaryRgenShader = ShaderManager::LoadShader("secondary_raygen.spv");
	if (!m_SecondaryRgenShader) m_SecondaryRgenShader = ShaderManager::LoadShader("RT2App/shaders/secondary_raygen.spv");
	m_MissShader    = ShaderManager::LoadShader("miss.spv");
	if (!m_MissShader)    m_MissShader    = ShaderManager::LoadShader("RT2App/shaders/miss.spv");
	m_ShadowShader  = ShaderManager::LoadShader("shadow.spv");
	if (!m_ShadowShader)  m_ShadowShader  = ShaderManager::LoadShader("RT2App/shaders/shadow.spv");
	m_ClosestShader = ShaderManager::LoadShader("closesthit.spv");
	if (!m_ClosestShader) m_ClosestShader = ShaderManager::LoadShader("RT2App/shaders/closesthit.spv");
	m_AnyHitShader  = ShaderManager::LoadShader("anyhit.spv");
	if (!m_AnyHitShader)  m_AnyHitShader  = ShaderManager::LoadShader("RT2App/shaders/anyhit.spv");
	m_ShadowHitShader = ShaderManager::LoadShader("shadowhit.spv");
	if (!m_ShadowHitShader) m_ShadowHitShader = ShaderManager::LoadShader("RT2App/shaders/shadowhit.spv");

	if (!m_RgenShader || !m_SecondaryRgenShader || !m_MissShader || !m_ShadowShader ||
	    !m_ClosestShader || !m_AnyHitShader || !m_ShadowHitShader)
	{
		RT_LOG("[PathTracePass] Failed to load RT shaders");
		return false;
	}

	const VkShaderStageFlags allRTFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR |
	                                      VK_SHADER_STAGE_MISS_BIT_KHR |
	                                      VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
	                                      VK_SHADER_STAGE_ANY_HIT_BIT_KHR;

	const VkShaderStageFlags allGraphicsRTFlags = allRTFlags |
	                                      VK_SHADER_STAGE_VERTEX_BIT |
	                                      VK_SHADER_STAGE_FRAGMENT_BIT |
	                                      VK_SHADER_STAGE_COMPUTE_BIT;

	const uint32_t maxTextures = 1000;

	VkDescriptorSetLayoutBinding bindings[14] = {};

	bindings[0] = {};
	bindings[0].binding = SI_BINDING_OUTPUT_IMAGE;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	bindings[0].descriptorCount = 1;
	bindings[0].stageFlags = allGraphicsRTFlags;

	bindings[1] = {};
	bindings[1].binding = SI_BINDING_CAMERA_UBO;
	bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	bindings[1].descriptorCount = 1;
	bindings[1].stageFlags = allGraphicsRTFlags;

	bindings[2] = {};
	bindings[2].binding = SI_BINDING_MATERIAL_BUFFER;
	bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[2].descriptorCount = 1;
	bindings[2].stageFlags = allGraphicsRTFlags;

	bindings[3] = {};
	bindings[3].binding = SI_BINDING_NORMAL_BUFFER;
	bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[3].descriptorCount = 1;
	bindings[3].stageFlags = allRTFlags;

	bindings[4] = {};
	bindings[4].binding = SI_BINDING_TLAS;
	bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
	bindings[4].descriptorCount = 1;
	bindings[4].stageFlags = allRTFlags;

	bindings[5] = {};
	bindings[5].binding = SI_BINDING_INSTANCE_OFFSETS;
	bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[5].descriptorCount = 1;
	bindings[5].stageFlags = allGraphicsRTFlags;

	bindings[6] = {};
	bindings[6].binding = SI_BINDING_TANGENT_BUFFER;
	bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[6].descriptorCount = 1;
	bindings[6].stageFlags = allRTFlags;

	bindings[7] = {};
	bindings[7].binding = SI_BINDING_UV_BUFFER;
	bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[7].descriptorCount = 1;
	bindings[7].stageFlags = allRTFlags;

	bindings[8] = {};
	bindings[8].binding = SI_BINDING_POSITION_BUFFER;
	bindings[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[8].descriptorCount = 1;
	bindings[8].stageFlags = allRTFlags;

	bindings[9] = {};
	bindings[9].binding = SI_BINDING_LIGHT_BUFFER;
	bindings[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[9].descriptorCount = 1;
	bindings[9].stageFlags = allRTFlags;

	bindings[10] = {};
	bindings[10].binding = SI_BINDING_INSTANCE_TRANSFORMS;
	bindings[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[10].descriptorCount = 1;
	bindings[10].stageFlags = allGraphicsRTFlags;

	bindings[11] = {};
	bindings[11].binding = SI_BINDING_TEXTURE_ARRAY;
	bindings[11].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[11].descriptorCount = maxTextures;
	bindings[11].stageFlags = allGraphicsRTFlags;

	bindings[12] = {};
	bindings[12].binding = SI_BINDING_INSTANCE_TRANSFORMS_PREV;
	bindings[12].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[12].descriptorCount = 1;
	bindings[12].stageFlags = allGraphicsRTFlags;

	bindings[13] = {};
	bindings[13].binding = SI_BINDING_INSTANCE_MATERIAL_INDICES;
	bindings[13].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[13].descriptorCount = 1;
	bindings[13].stageFlags = allGraphicsRTFlags;

	VkDescriptorBindingFlagsEXT bindingFlags[14] = {};
	bindingFlags[11] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT_EXT;
	// Note: no VARIABLE_DESCRIPTOR_COUNT_BIT — binding 11 is no longer the
	// highest binding number (12, 13 exist). Use fixed descriptorCount instead.

	VkDescriptorSetLayoutBindingFlagsCreateInfoEXT bindingFlagsInfo = {};
	bindingFlagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO_EXT;
	bindingFlagsInfo.bindingCount = 14;
	bindingFlagsInfo.pBindingFlags = bindingFlags;

	VkDescriptorSetLayoutCreateInfo layoutInfo = {};
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = 14;
	layoutInfo.pBindings = bindings;
	layoutInfo.pNext = &bindingFlagsInfo;

	VK_CHECK(vkCreateDescriptorSetLayout(m_Device, &layoutInfo, nullptr, &m_DescriptorSetLayout));

	VkDescriptorSetLayout setLayouts[2] = { m_DescriptorSetLayout, gbufferSetLayout };

	VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = 2;
	pipelineLayoutInfo.pSetLayouts = setLayouts;

	VK_CHECK(vkCreatePipelineLayout(m_Device, &pipelineLayoutInfo, nullptr, &m_PipelineLayout));

	// 7 stages (6 original + secondary_raygen)
	VkPipelineShaderStageCreateInfo stages[7] = {};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
	stages[0].module = m_RgenShader;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_MISS_BIT_KHR;
	stages[1].module = m_MissShader;
	stages[1].pName = "main";
	stages[2].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[2].stage = VK_SHADER_STAGE_MISS_BIT_KHR;
	stages[2].module = m_ShadowShader;
	stages[2].pName = "main";
	stages[3].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[3].stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
	stages[3].module = m_ClosestShader;
	stages[3].pName = "main";
	stages[4].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[4].stage = VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
	stages[4].module = m_AnyHitShader;
	stages[4].pName = "main";
	stages[5].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[5].stage = VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
	stages[5].module = m_ShadowHitShader;
	stages[5].pName = "main";
	stages[6].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[6].stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
	stages[6].module = m_SecondaryRgenShader;
	stages[6].pName = "main";

	// 7 groups — must init ALL fields, VK_SHADER_UNUSED_KHR is ~0u not 0
	// Group layout:
	//   0: raygen (RT-primary), 1: miss_sky, 2: miss_shadow,
	//   3: hit_primary_opaque, 4: hit_primary_alpha, 5: hit_shadow,
	//   6: secondary_raygen (raster-first)
	VkRayTracingShaderGroupCreateInfoKHR groups[7];
	for (int i = 0; i < 7; i++)
	{
		groups[i].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
		groups[i].pNext = nullptr;
		groups[i].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
		groups[i].generalShader      = VK_SHADER_UNUSED_KHR;
		groups[i].closestHitShader   = VK_SHADER_UNUSED_KHR;
		groups[i].anyHitShader       = VK_SHADER_UNUSED_KHR;
		groups[i].intersectionShader = VK_SHADER_UNUSED_KHR;
		groups[i].pShaderGroupCaptureReplayHandle = nullptr;
	}
	groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
	groups[0].generalShader = 0;
	groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
	groups[1].generalShader = 1;
	groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
	groups[2].generalShader = 2;
	groups[3].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
	groups[3].closestHitShader = 3;
	groups[4].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
	groups[4].closestHitShader = 3;
	groups[4].anyHitShader = 4;
	groups[5].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
	groups[5].anyHitShader = 5;
	// Group 6: secondary_raygen (raster-first path)
	groups[6].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
	groups[6].generalShader = 6;

	const uint32_t kRecursionCap = 16;
	m_MaxRecursionDepth = dev.rtPipelineProps.maxRayRecursionDepth < kRecursionCap
	                    ? dev.rtPipelineProps.maxRayRecursionDepth : kRecursionCap;
	if (m_MaxRecursionDepth < 2) m_MaxRecursionDepth = 2;

	VkRayTracingPipelineCreateInfoKHR pipelineInfo = {};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
	pipelineInfo.stageCount = 7;
	pipelineInfo.pStages = stages;
	pipelineInfo.groupCount = 7;
	pipelineInfo.pGroups = groups;
	pipelineInfo.maxPipelineRayRecursionDepth = m_MaxRecursionDepth;
	pipelineInfo.layout = m_PipelineLayout;

	VkResult err = g_RTDispatch.CreateRayTracingPipelinesKHR(
		m_Device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_Pipeline);
	if (err != VK_SUCCESS)
	{
		RT_LOG("[PathTracePass] vkCreateRayTracingPipelinesKHR failed: %d", (int)err);
		m_Pipeline = VK_NULL_HANDLE;
		return false;
	}

	// ---- SBT ----
	const uint32_t handleSize  = dev.rtPipelineProps.shaderGroupHandleSize;
	const uint32_t baseAlign   = std::max<uint32_t>(dev.rtPipelineProps.shaderGroupBaseAlignment, 1);
	const uint32_t handleAlign = std::max<uint32_t>(dev.rtPipelineProps.shaderGroupHandleAlignment, 1);

	m_SBTStride       = baseAlign;
	m_RgenRegionSize  = baseAlign * 2;  // 2 raygen records: raygen + secondary_raygen
	m_MissRegionSize  = baseAlign * 2;
	m_HitRegionSize   = baseAlign * 4;

	VkDeviceSize sbtSize = m_RgenRegionSize + m_MissRegionSize + m_HitRegionSize;

	// Create SBT buffer via GpuResources (ensures correct memory flags + device address)
	if (!GpuResources::CreateBuffer(dev, sbtSize,
		VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		m_SBTBuffer))
	{
		RT_LOG("[PathTracePass] failed to create SBT buffer");
		return false;
	}
	m_SBTSize = sbtSize;

	// Fetch group handles (7 groups)
	std::vector<uint8_t> handles(7 * handleSize);
	err = g_RTDispatch.GetRayTracingShaderGroupHandlesKHR(
		m_Device, m_Pipeline, 0, 7, handles.size(), handles.data());
	if (err != VK_SUCCESS)
	{
		RT_LOG("[PathTracePass] GetRayTracingShaderGroupHandlesKHR failed: %d", (int)err);
		return false;
	}

	// Copy handles into SBT
	// Rgen region: [0] = raygen (RT-primary), [baseAlign] = secondary_raygen (raster-first)
	void* mapped = nullptr;
	vkMapMemory(m_Device, m_SBTBuffer.memory, 0, sbtSize, 0, &mapped);
	std::memset(mapped, 0, (size_t)sbtSize);
	uint8_t* dst = static_cast<uint8_t*>(mapped);
	std::memcpy(dst + 0, handles.data() + 0 * handleSize, handleSize);
	std::memcpy(dst + baseAlign, handles.data() + 6 * handleSize, handleSize);  // secondary_raygen
	std::memcpy(dst + m_RgenRegionSize, handles.data() + 1 * handleSize, handleSize);
	std::memcpy(dst + m_RgenRegionSize + baseAlign, handles.data() + 2 * handleSize, handleSize);
	VkDeviceSize hitBase = m_RgenRegionSize + m_MissRegionSize;
	std::memcpy(dst + hitBase, handles.data() + 3 * handleSize, handleSize);
	std::memcpy(dst + hitBase + baseAlign, handles.data() + 4 * handleSize, handleSize);
	std::memcpy(dst + hitBase + baseAlign * 2, handles.data() + 5 * handleSize, handleSize);
	std::memcpy(dst + hitBase + baseAlign * 3, handles.data() + 5 * handleSize, handleSize);
	vkUnmapMemory(m_Device, m_SBTBuffer.memory);

	RT_LOG("[PathTracePass] initialized (recursion=%u, SBT stride=%llu)",
	       m_MaxRecursionDepth, (unsigned long long)m_SBTStride);
	return true;
}

void PathTracePass::Destroy()
{
	if (!m_Pipeline) return;
	VkDevice device = m_Device;
	if (m_Pipeline) { vkDestroyPipeline(device, m_Pipeline, nullptr); m_Pipeline = VK_NULL_HANDLE; }
	if (m_PipelineLayout) { vkDestroyPipelineLayout(device, m_PipelineLayout, nullptr); m_PipelineLayout = VK_NULL_HANDLE; }
	if (m_DescriptorSetLayout) { vkDestroyDescriptorSetLayout(device, m_DescriptorSetLayout, nullptr); m_DescriptorSetLayout = VK_NULL_HANDLE; }
	if (m_RgenShader) { vkDestroyShaderModule(device, m_RgenShader, nullptr); m_RgenShader = VK_NULL_HANDLE; }
	if (m_SecondaryRgenShader) { vkDestroyShaderModule(device, m_SecondaryRgenShader, nullptr); m_SecondaryRgenShader = VK_NULL_HANDLE; }
	if (m_MissShader) { vkDestroyShaderModule(device, m_MissShader, nullptr); m_MissShader = VK_NULL_HANDLE; }
	if (m_ShadowShader) { vkDestroyShaderModule(device, m_ShadowShader, nullptr); m_ShadowShader = VK_NULL_HANDLE; }
	if (m_ClosestShader) { vkDestroyShaderModule(device, m_ClosestShader, nullptr); m_ClosestShader = VK_NULL_HANDLE; }
	if (m_AnyHitShader) { vkDestroyShaderModule(device, m_AnyHitShader, nullptr); m_AnyHitShader = VK_NULL_HANDLE; }
	if (m_ShadowHitShader) { vkDestroyShaderModule(device, m_ShadowHitShader, nullptr); m_ShadowHitShader = VK_NULL_HANDLE; }
	if (m_SBTBuffer.buffer) { vkDestroyBuffer(device, m_SBTBuffer.buffer, nullptr); m_SBTBuffer.buffer = VK_NULL_HANDLE; }
	if (m_SBTBuffer.memory)  { vkFreeMemory(device, m_SBTBuffer.memory, nullptr);  m_SBTBuffer.memory = VK_NULL_HANDLE; }
	m_SBTBuffer.size = 0;
	m_SBTSize = 0;
	m_DescriptorSet = VK_NULL_HANDLE;
}

bool PathTracePass::CreateDescriptorSet(const GpuDevice& dev, VkDescriptorPool pool)
{
	VkDescriptorSetAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.descriptorPool = pool;
	allocInfo.descriptorSetCount = 1;
	allocInfo.pSetLayouts = &m_DescriptorSetLayout;

	VkResult err = vkAllocateDescriptorSets(dev.device, &allocInfo, &m_DescriptorSet);
	if (err != VK_SUCCESS)
	{
		RT_LOG("[PathTracePass] vkAllocateDescriptorSets failed: %d", (int)err);
		return false;
	}
	RT_LOG("[PathTracePass] descriptor set allocated");
	return true;
}

void PathTracePass::FreeDescriptorSet(VkDevice device, VkDescriptorPool pool)
{
	if (m_DescriptorSet)
	{
		vkFreeDescriptorSets(device, pool, 1, &m_DescriptorSet);
		m_DescriptorSet = VK_NULL_HANDLE;
	}
}

void PathTracePass::UpdateDescriptorSet(const GpuDevice& dev,
	VkImageView outputView, VkSampler outputSampler,
	VkBuffer cameraUBO, VkBuffer materialBuffer,
	VkBuffer normalBuffer, VkBuffer instanceOffsetBuffer,
	VkBuffer tangentBuffer, VkBuffer uvBuffer, VkBuffer positionBuffer,
	VkBuffer lightBuffer, VkBuffer instanceTransformBuffer,
	VkBuffer instanceTransformPrevBuffer,
	VkBuffer instanceMaterialIndexBuffer,
	VkAccelerationStructureKHR tlas,
	const std::vector<VkDescriptorImageInfo>& textureImageInfos)
{
	VkDescriptorImageInfo imageInfo = {};
	imageInfo.imageView = outputView;
	imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	imageInfo.sampler = outputSampler;

	VkDescriptorBufferInfo cameraBufferInfo = {};
	cameraBufferInfo.buffer = cameraUBO;
	cameraBufferInfo.range = VK_WHOLE_SIZE;

	VkDescriptorBufferInfo materialBufferInfo = {};
	materialBufferInfo.buffer = materialBuffer;
	materialBufferInfo.range = VK_WHOLE_SIZE;

	VkDescriptorBufferInfo normalBufferInfo = {};
	normalBufferInfo.buffer = normalBuffer;
	normalBufferInfo.range = VK_WHOLE_SIZE;

	VkDescriptorBufferInfo offsetBufferInfo = {};
	offsetBufferInfo.buffer = instanceOffsetBuffer;
	offsetBufferInfo.range = VK_WHOLE_SIZE;

	VkDescriptorBufferInfo uvBufferInfo = {};
	uvBufferInfo.buffer = uvBuffer;
	uvBufferInfo.range = VK_WHOLE_SIZE;

	VkDescriptorBufferInfo positionBufferInfo = {};
	positionBufferInfo.buffer = positionBuffer;
	positionBufferInfo.range = VK_WHOLE_SIZE;

	VkDescriptorBufferInfo tangentBufferInfo = {};
	tangentBufferInfo.buffer = tangentBuffer;
	tangentBufferInfo.range = VK_WHOLE_SIZE;

	VkDescriptorBufferInfo lightBufferInfo = {};
	lightBufferInfo.buffer = lightBuffer;
	lightBufferInfo.range = VK_WHOLE_SIZE;

	VkDescriptorBufferInfo instanceTransformBufferInfo = {};
	instanceTransformBufferInfo.buffer = instanceTransformBuffer;
	instanceTransformBufferInfo.range = VK_WHOLE_SIZE;

	VkDescriptorBufferInfo instanceTransformPrevBufferInfo = {};
	instanceTransformPrevBufferInfo.buffer = instanceTransformPrevBuffer;
	instanceTransformPrevBufferInfo.range = VK_WHOLE_SIZE;

	VkDescriptorBufferInfo instanceMaterialIndexBufferInfo = {};
	instanceMaterialIndexBufferInfo.buffer = instanceMaterialIndexBuffer;
	instanceMaterialIndexBufferInfo.range = VK_WHOLE_SIZE;

	VkWriteDescriptorSetAccelerationStructureKHR asInfo = {};
	asInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
	asInfo.accelerationStructureCount = 1;
	asInfo.pAccelerationStructures = &tlas;

	VkWriteDescriptorSet writes[14] = {};

	writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[0].dstSet = m_DescriptorSet;
	writes[0].dstBinding = SI_BINDING_OUTPUT_IMAGE;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	writes[0].descriptorCount = 1;
	writes[0].pImageInfo = &imageInfo;

	writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[1].dstSet = m_DescriptorSet;
	writes[1].dstBinding = SI_BINDING_CAMERA_UBO;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	writes[1].descriptorCount = 1;
	writes[1].pBufferInfo = &cameraBufferInfo;

	writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[2].dstSet = m_DescriptorSet;
	writes[2].dstBinding = SI_BINDING_MATERIAL_BUFFER;
	writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[2].descriptorCount = 1;
	writes[2].pBufferInfo = &materialBufferInfo;

	writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[3].dstSet = m_DescriptorSet;
	writes[3].dstBinding = SI_BINDING_NORMAL_BUFFER;
	writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[3].descriptorCount = 1;
	writes[3].pBufferInfo = &normalBufferInfo;

	writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[4].dstSet = m_DescriptorSet;
	writes[4].dstBinding = SI_BINDING_TLAS;
	writes[4].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
	writes[4].descriptorCount = 1;
	writes[4].pNext = &asInfo;

	writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[5].dstSet = m_DescriptorSet;
	writes[5].dstBinding = SI_BINDING_INSTANCE_OFFSETS;
	writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[5].descriptorCount = 1;
	writes[5].pBufferInfo = &offsetBufferInfo;

	writes[6] = {};
	writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[6].dstSet = m_DescriptorSet;
	writes[6].dstBinding = SI_BINDING_TANGENT_BUFFER;
	writes[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[6].descriptorCount = 1;
	writes[6].pBufferInfo = &tangentBufferInfo;

	writes[7] = {};
	writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[7].dstSet = m_DescriptorSet;
	writes[7].dstBinding = SI_BINDING_UV_BUFFER;
	writes[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[7].descriptorCount = 1;
	writes[7].pBufferInfo = &uvBufferInfo;

	writes[8] = {};
	writes[8].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[8].dstSet = m_DescriptorSet;
	writes[8].dstBinding = SI_BINDING_POSITION_BUFFER;
	writes[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[8].descriptorCount = 1;
	writes[8].pBufferInfo = &positionBufferInfo;

	writes[9] = {};
	writes[9].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[9].dstSet = m_DescriptorSet;
	writes[9].dstBinding = SI_BINDING_LIGHT_BUFFER;
	writes[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[9].descriptorCount = 1;
	writes[9].pBufferInfo = &lightBufferInfo;

	writes[10] = {};
	writes[10].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[10].dstSet = m_DescriptorSet;
	writes[10].dstBinding = SI_BINDING_INSTANCE_TRANSFORMS;
	writes[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[10].descriptorCount = 1;
	writes[10].pBufferInfo = &instanceTransformBufferInfo;

	writes[11] = {};
	writes[11].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[11].dstSet = m_DescriptorSet;
	writes[11].dstBinding = SI_BINDING_INSTANCE_TRANSFORMS_PREV;
	writes[11].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[11].descriptorCount = 1;
	writes[11].pBufferInfo = &instanceTransformPrevBufferInfo;

	writes[12] = {};
	writes[12].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[12].dstSet = m_DescriptorSet;
	writes[12].dstBinding = SI_BINDING_INSTANCE_MATERIAL_INDICES;
	writes[12].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[12].descriptorCount = 1;
	writes[12].pBufferInfo = &instanceMaterialIndexBufferInfo;

	uint32_t writeCount = 13;
	if (!textureImageInfos.empty())
	{
		writes[13].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[13].dstSet = m_DescriptorSet;
		writes[13].dstBinding = SI_BINDING_TEXTURE_ARRAY;
		writes[13].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[13].descriptorCount = (uint32_t)textureImageInfos.size();
		writes[13].pImageInfo = textureImageInfos.data();
		writeCount = 14;
	}

	vkUpdateDescriptorSets(dev.device, writeCount, writes, 0, nullptr);
}

void PathTracePass::Record(VkCommandBuffer cmd, uint32_t width, uint32_t height,
                            VkDescriptorSet gbufferSet, bool rasterFirst) const
{
	if (!m_Pipeline || !m_DescriptorSet) return;

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_Pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_PipelineLayout, 0, 1, &m_DescriptorSet, 0, nullptr);
	if (gbufferSet)
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_PipelineLayout, 1, 1, &gbufferSet, 0, nullptr);

	VkBufferDeviceAddressInfo sbtAddrInfo = {};
	sbtAddrInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
	sbtAddrInfo.buffer = m_SBTBuffer.buffer;
	VkDeviceAddress sbtAddress = vkGetBufferDeviceAddress(m_Device, &sbtAddrInfo);

	// SBT is written once during Init() and never changes. The host→shader
	// visibility is handled by the first vkQueueSubmit after Init. No per-frame
	// barrier needed.

	// Select raygen SBT entry:
	// rasterFirst=false → offset 0 (raygen, RT-primary)
	// rasterFirst=true  → offset baseAlign (secondary_raygen, raster-first)
	VkDeviceSize rgenOffset = rasterFirst ? m_SBTStride : 0;

	VkStridedDeviceAddressRegionKHR rgenRegion = {};
	rgenRegion.deviceAddress = sbtAddress + rgenOffset;
	rgenRegion.stride        = m_SBTStride;
	rgenRegion.size          = m_SBTStride;  // single record per dispatch

	VkStridedDeviceAddressRegionKHR missRegion = {};
	missRegion.deviceAddress = sbtAddress + m_RgenRegionSize;
	missRegion.stride        = m_SBTStride;
	missRegion.size          = m_MissRegionSize;

	VkStridedDeviceAddressRegionKHR hitRegion = {};
	hitRegion.deviceAddress = sbtAddress + m_RgenRegionSize + m_MissRegionSize;
	hitRegion.stride        = m_SBTStride;
	hitRegion.size          = m_HitRegionSize;

	VkStridedDeviceAddressRegionKHR callableRegion = {};

	if (!g_RTDispatch.CmdTraceRaysKHR)
	{
		RT_LOG("[PathTracePass] ERROR: CmdTraceRaysKHR is NULL!");
		return;
	}
	g_RTDispatch.CmdTraceRaysKHR(cmd,
		&rgenRegion, &missRegion, &hitRegion, &callableRegion,
		width, height, 1);
}