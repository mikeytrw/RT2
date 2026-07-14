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

// ============================================================================
// Descriptor schema — single source of truth for set 0 bindings.
// The layout, binding flags, and descriptor writes are all derived from this.
//
// Array index in this vector == array index passed to vkCreateDescriptorSetLayout.
// Binding numbers can be sparse (e.g. 11 is skipped — textures moved to 18).
// ============================================================================
static const std::vector<BindingDef>& GetSet0Bindings()
{
	static const VkShaderStageFlags allRTFlags =
		VK_SHADER_STAGE_RAYGEN_BIT_KHR |
		VK_SHADER_STAGE_MISS_BIT_KHR |
		VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
		VK_SHADER_STAGE_ANY_HIT_BIT_KHR;

	static const VkShaderStageFlags allRTComputeFlags = allRTFlags |
		VK_SHADER_STAGE_COMPUTE_BIT;

	static const VkShaderStageFlags allGraphicsRTFlags = allRTFlags |
		VK_SHADER_STAGE_VERTEX_BIT |
		VK_SHADER_STAGE_FRAGMENT_BIT |
		VK_SHADER_STAGE_COMPUTE_BIT;

	static const std::vector<BindingDef> bindings = {
		{ SI_BINDING_OUTPUT_IMAGE,            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,              1, allGraphicsRTFlags, 0 },
		{ SI_BINDING_CAMERA_UBO,              VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,             1, allGraphicsRTFlags, 0 },
		{ SI_BINDING_MATERIAL_BUFFER,         VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1, allGraphicsRTFlags, 0 },
		{ SI_BINDING_VERTEX_BUFFER,           VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1, allGraphicsRTFlags, 0 },
		{ SI_BINDING_TLAS,                    VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, allRTComputeFlags,  0 },
		{ SI_BINDING_INDEX_BUFFER,            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1, allGraphicsRTFlags, 0 },
		{ SI_BINDING_NORMAL_BUFFER,           VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1, allGraphicsRTFlags, 0 },
		{ SI_BINDING_UV_BUFFER,               VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1, allGraphicsRTFlags, 0 },
		{ SI_BINDING_INSTANCE_MESH_INFO,      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1, allGraphicsRTFlags, 0 },
		{ SI_BINDING_LIGHT_BUFFER,            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1, allGraphicsRTFlags, 0 },
		{ SI_BINDING_INSTANCE_TRANSFORMS,     VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1, allGraphicsRTFlags, 0 },
		// Binding 11: ReSTIR GI monolithic buffer (reservoir A/B + receiver history prev/cur).
		// Visible to compute (GI passes) and raygen (final shading consumes stored GI sample).
		{ SI_BINDING_GI_DATA,                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             1, allRTComputeFlags,  0 },
		{ SI_BINDING_INSTANCE_TRANSFORMS_PREV,    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         1, allGraphicsRTFlags, 0 },
		{ SI_BINDING_INSTANCE_MATERIAL_INDICES,   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         1, allGraphicsRTFlags, 0 },
		{ SI_BINDING_RESERVOIR_HISTORY,           VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         1, allGraphicsRTFlags, 0 },
		{ SI_BINDING_RESERVOIR_SCRATCH,           VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         1, allGraphicsRTFlags, 0 },
		{ SI_BINDING_SURFACE_HISTORY,             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         1, allGraphicsRTFlags, 0 },
		{ SI_BINDING_INSTANCE_MAT_OFFSETS,        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         1, allGraphicsRTFlags, 0 },
		{ SI_BINDING_TEXTURE_ARRAY,               VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, PathTracePass::MAX_TEXTURES, allGraphicsRTFlags,
			VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT_EXT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT_EXT },
	};

	return bindings;
}

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

	// ---- Create descriptor set layout from schema ----
	const auto& schema = GetSet0Bindings();

	std::vector<VkDescriptorSetLayoutBinding> layoutBindings(schema.size());
	for (size_t i = 0; i < schema.size(); i++)
	{
		layoutBindings[i].binding = schema[i].binding;
		layoutBindings[i].descriptorType = schema[i].type;
		layoutBindings[i].descriptorCount = schema[i].descriptorCount;
		layoutBindings[i].stageFlags = schema[i].stageFlags;
		layoutBindings[i].pImmutableSamplers = nullptr;
	}

	std::vector<VkDescriptorBindingFlagsEXT> bindingFlags(schema.size());
	for (size_t i = 0; i < schema.size(); i++)
		bindingFlags[i] = schema[i].flags;

	VkDescriptorSetLayoutBindingFlagsCreateInfoEXT bindingFlagsInfo = {};
	bindingFlagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO_EXT;
	bindingFlagsInfo.bindingCount = (uint32_t)bindingFlags.size();
	bindingFlagsInfo.pBindingFlags = bindingFlags.data();

	VkDescriptorSetLayoutCreateInfo layoutInfo = {};
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = (uint32_t)layoutBindings.size();
	layoutInfo.pBindings = layoutBindings.data();
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

	if (!GpuResources::CreateBuffer(dev, sbtSize,
		VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		m_SBTBuffer))
	{
		RT_LOG("[PathTracePass] failed to create SBT buffer");
		return false;
	}
	m_SBTSize = sbtSize;

	std::vector<uint8_t> handles(7 * handleSize);
	err = g_RTDispatch.GetRayTracingShaderGroupHandlesKHR(
		m_Device, m_Pipeline, 0, 7, handles.size(), handles.data());
	if (err != VK_SUCCESS)
	{
		RT_LOG("[PathTracePass] GetRayTracingShaderGroupHandlesKHR failed: %d", (int)err);
		return false;
	}

	void* mapped = nullptr;
	vkMapMemory(m_Device, m_SBTBuffer.memory, 0, sbtSize, 0, &mapped);
	std::memset(mapped, 0, (size_t)sbtSize);
	uint8_t* dst = static_cast<uint8_t*>(mapped);
	std::memcpy(dst + 0, handles.data() + 0 * handleSize, handleSize);
	std::memcpy(dst + baseAlign, handles.data() + 6 * handleSize, handleSize);
	std::memcpy(dst + m_RgenRegionSize, handles.data() + 1 * handleSize, handleSize);
	std::memcpy(dst + m_RgenRegionSize + baseAlign, handles.data() + 2 * handleSize, handleSize);
	VkDeviceSize hitBase = m_RgenRegionSize + m_MissRegionSize;
	std::memcpy(dst + hitBase, handles.data() + 3 * handleSize, handleSize);
	std::memcpy(dst + hitBase + baseAlign, handles.data() + 4 * handleSize, handleSize);
	std::memcpy(dst + hitBase + baseAlign * 2, handles.data() + 5 * handleSize, handleSize);
	std::memcpy(dst + hitBase + baseAlign * 3, handles.data() + 5 * handleSize, handleSize);
	vkUnmapMemory(m_Device, m_SBTBuffer.memory);

	// ---- Dedicated descriptor pool for set 0 ----
	// Walnut's pool has only 1000 COMBINED_IMAGE_SAMPLER descriptors, shared
	// with ImGui. Large scenes need more. We create a separate pool sized for
	// MAX_TEXTURES per set, plus enough for the non-texture bindings.
	VkDescriptorPoolSize poolSizes[] = {
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,              4 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,             4 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,            64 },
		{ VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,  4 },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,    MAX_TEXTURES },
	};

	VkDescriptorPoolCreateInfo poolInfo = {};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	poolInfo.maxSets = 4;  // only need 1-2 sets (current + resize)
	poolInfo.poolSizeCount = (uint32_t)(sizeof(poolSizes) / sizeof(poolSizes[0]));
	poolInfo.pPoolSizes = poolSizes;

	err = vkCreateDescriptorPool(m_Device, &poolInfo, nullptr, &m_DescriptorPool);
	if (err != VK_SUCCESS)
	{
		RT_LOG("[PathTracePass] vkCreateDescriptorPool failed: %d", (int)err);
		return false;
	}

	RT_LOG("[PathTracePass] initialized (recursion=%u, SBT stride=%llu, MAX_TEXTURES=%u, pool created)",
	       m_MaxRecursionDepth, (unsigned long long)m_SBTStride, MAX_TEXTURES);
	return true;
}

void PathTracePass::Destroy()
{
	if (!m_Pipeline && !m_DescriptorPool) return;
	VkDevice device = m_Device;
	if (m_DescriptorSet) { vkFreeDescriptorSets(device, m_DescriptorPool, 1, &m_DescriptorSet); m_DescriptorSet = VK_NULL_HANDLE; }
	m_AllocatedTextureCount = 0;
	if (m_DescriptorPool) { vkDestroyDescriptorPool(device, m_DescriptorPool, nullptr); m_DescriptorPool = VK_NULL_HANDLE; }
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
}

bool PathTracePass::CreateDescriptorSet(const GpuDevice& dev, uint32_t textureCount)
{
	if (textureCount > MAX_TEXTURES)
	{
		RT_LOG("[PathTracePass] textureCount=%u exceeds MAX_TEXTURES=%u", textureCount, MAX_TEXTURES);
		return false;
	}

	// If we already have a set with enough slots, no-op
	if (m_DescriptorSet != VK_NULL_HANDLE && m_AllocatedTextureCount >= textureCount)
		return true;

	// Free old set if any
	if (m_DescriptorSet != VK_NULL_HANDLE)
	{
		vkDeviceWaitIdle(dev.device);
		vkFreeDescriptorSets(dev.device, m_DescriptorPool, 1, &m_DescriptorSet);
		m_DescriptorSet = VK_NULL_HANDLE;
		m_AllocatedTextureCount = 0;
	}

	// Allocate with variable descriptor count
	uint32_t variableCount = textureCount;
	VkDescriptorSetVariableDescriptorCountAllocateInfoEXT varCountInfo = {};
	varCountInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO_EXT;
	varCountInfo.descriptorSetCount = 1;
	varCountInfo.pDescriptorCounts = &variableCount;

	VkDescriptorSetAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.descriptorPool = m_DescriptorPool;
	allocInfo.descriptorSetCount = 1;
	allocInfo.pSetLayouts = &m_DescriptorSetLayout;
	allocInfo.pNext = &varCountInfo;

	VkResult err = vkAllocateDescriptorSets(dev.device, &allocInfo, &m_DescriptorSet);
	if (err != VK_SUCCESS)
	{
		RT_LOG("[PathTracePass] vkAllocateDescriptorSets failed: %d (textureCount=%u)", (int)err, textureCount);
		return false;
	}

	m_AllocatedTextureCount = textureCount;
	RT_LOG("[PathTracePass] descriptor set allocated (%u texture slots)", textureCount);
	return true;
}

void PathTracePass::FreeDescriptorSet()
{
	if (m_DescriptorSet)
	{
		vkFreeDescriptorSets(m_Device, m_DescriptorPool, 1, &m_DescriptorSet);
		m_DescriptorSet = VK_NULL_HANDLE;
		m_AllocatedTextureCount = 0;
	}
}

void PathTracePass::UpdateDescriptorSet(const GpuDevice& dev,
	VkImageView outputView, VkSampler outputSampler,
	VkBuffer cameraUBO, VkBuffer materialBuffer,
	VkBuffer vertexBuffer, VkBuffer indexBuffer,
	VkBuffer normalBuffer, VkBuffer uvBuffer,
	VkBuffer instanceMeshInfoBuffer,
	VkBuffer lightBuffer, VkBuffer instanceTransformBuffer,
	VkBuffer instanceTransformPrevBuffer,
	VkBuffer instanceMaterialIndexBuffer,
	VkBuffer instanceMatOffsetBuffer,
	VkAccelerationStructureKHR tlas,
	VkBuffer reservoirBuffer, VkBuffer reservoirScratchBuffer,
	VkBuffer surfaceHistoryBuffer,
	VkBuffer giDataBuffer,
	const std::vector<VkDescriptorImageInfo>& textureImageInfos)
{
	// Local descriptor info structs — must stay alive until vkUpdateDescriptorSets
	VkDescriptorImageInfo outputImageInfo = {};
	outputImageInfo.imageView = outputView;
	outputImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	outputImageInfo.sampler = outputSampler;

	VkDescriptorBufferInfo cameraBufInfo = { cameraUBO, 0, VK_WHOLE_SIZE };
	VkDescriptorBufferInfo materialBufInfo = { materialBuffer, 0, VK_WHOLE_SIZE };
	VkDescriptorBufferInfo vertexBufInfo = { vertexBuffer, 0, VK_WHOLE_SIZE };
	VkDescriptorBufferInfo indexBufInfo = { indexBuffer, 0, VK_WHOLE_SIZE };
	VkDescriptorBufferInfo normalBufInfo = { normalBuffer, 0, VK_WHOLE_SIZE };
	VkDescriptorBufferInfo uvBufInfo = { uvBuffer, 0, VK_WHOLE_SIZE };
	VkDescriptorBufferInfo meshInfoBufInfo = { instanceMeshInfoBuffer, 0, VK_WHOLE_SIZE };
	VkDescriptorBufferInfo lightBufInfo = { lightBuffer, 0, VK_WHOLE_SIZE };
	VkDescriptorBufferInfo transformBufInfo = { instanceTransformBuffer, 0, VK_WHOLE_SIZE };
	VkDescriptorBufferInfo transformPrevBufInfo = { instanceTransformPrevBuffer, 0, VK_WHOLE_SIZE };
	VkDescriptorBufferInfo matIdxBufInfo = { instanceMaterialIndexBuffer, 0, VK_WHOLE_SIZE };
	VkDescriptorBufferInfo matOffsetBufInfo = { instanceMatOffsetBuffer, 0, VK_WHOLE_SIZE };
	VkDescriptorBufferInfo reservoirBufInfo = { reservoirBuffer, 0, VK_WHOLE_SIZE };
	VkDescriptorBufferInfo reservoirScratchBufInfo = { reservoirScratchBuffer, 0, VK_WHOLE_SIZE };
	VkDescriptorBufferInfo surfaceHistoryBufInfo = { surfaceHistoryBuffer, 0, VK_WHOLE_SIZE };
	VkDescriptorBufferInfo giDataBufInfo = { giDataBuffer, 0, VK_WHOLE_SIZE };

	VkWriteDescriptorSetAccelerationStructureKHR asInfo = {};
	asInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
	asInfo.accelerationStructureCount = 1;
	asInfo.pAccelerationStructures = &tlas;

	// Build writes vector — one write per binding, no gaps, no fragile index arithmetic
	std::vector<VkWriteDescriptorSet> writes;
	writes.reserve(18);

	auto addBufferWrite = [&](uint32_t binding, VkDescriptorType type, VkDescriptorBufferInfo* info) {
		VkWriteDescriptorSet w = {};
		w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w.dstSet = m_DescriptorSet;
		w.dstBinding = binding;
		w.descriptorType = type;
		w.descriptorCount = 1;
		w.pBufferInfo = info;
		writes.push_back(w);
	};

	auto addImageWrite = [&](uint32_t binding, VkDescriptorType type, VkDescriptorImageInfo* info) {
		VkWriteDescriptorSet w = {};
		w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w.dstSet = m_DescriptorSet;
		w.dstBinding = binding;
		w.descriptorType = type;
		w.descriptorCount = 1;
		w.pImageInfo = info;
		writes.push_back(w);
	};

	addImageWrite(SI_BINDING_OUTPUT_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &outputImageInfo);
	addBufferWrite(SI_BINDING_CAMERA_UBO, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &cameraBufInfo);
	addBufferWrite(SI_BINDING_MATERIAL_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &materialBufInfo);
	addBufferWrite(SI_BINDING_VERTEX_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &vertexBufInfo);

	// TLAS — uses pNext for acceleration structure
	{
		VkWriteDescriptorSet w = {};
		w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w.dstSet = m_DescriptorSet;
		w.dstBinding = SI_BINDING_TLAS;
		w.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
		w.descriptorCount = 1;
		w.pNext = &asInfo;
		writes.push_back(w);
	}

	addBufferWrite(SI_BINDING_INDEX_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &indexBufInfo);
	addBufferWrite(SI_BINDING_NORMAL_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &normalBufInfo);
	addBufferWrite(SI_BINDING_UV_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &uvBufInfo);
	addBufferWrite(SI_BINDING_INSTANCE_MESH_INFO, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &meshInfoBufInfo);
	addBufferWrite(SI_BINDING_LIGHT_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &lightBufInfo);
	addBufferWrite(SI_BINDING_INSTANCE_TRANSFORMS, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &transformBufInfo);
	addBufferWrite(SI_BINDING_GI_DATA, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &giDataBufInfo);
	addBufferWrite(SI_BINDING_INSTANCE_TRANSFORMS_PREV, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &transformPrevBufInfo);
	addBufferWrite(SI_BINDING_INSTANCE_MATERIAL_INDICES, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &matIdxBufInfo);
	addBufferWrite(SI_BINDING_RESERVOIR_HISTORY, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &reservoirBufInfo);
	addBufferWrite(SI_BINDING_RESERVOIR_SCRATCH, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &reservoirScratchBufInfo);
	addBufferWrite(SI_BINDING_SURFACE_HISTORY, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &surfaceHistoryBufInfo);
	addBufferWrite(SI_BINDING_INSTANCE_MAT_OFFSETS, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &matOffsetBufInfo);

	// Texture array — only write if we have textures
	if (!textureImageInfos.empty())
	{
		VkWriteDescriptorSet w = {};
		w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w.dstSet = m_DescriptorSet;
		w.dstBinding = SI_BINDING_TEXTURE_ARRAY;
		w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		w.descriptorCount = (uint32_t)textureImageInfos.size();
		w.pImageInfo = textureImageInfos.data();
		writes.push_back(w);
	}

	RT_LOG("[UpdateDS] writeCount=%zu (textures=%zu, allocatedSlots=%u)",
	       writes.size(), textureImageInfos.size(), m_AllocatedTextureCount);
	fflush(stdout);

	vkUpdateDescriptorSets(dev.device, (uint32_t)writes.size(), writes.data(), 0, nullptr);
	RT_LOG("[UpdateDS] vkUpdateDescriptorSets done"); fflush(stdout);
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

	VkDeviceSize rgenOffset = rasterFirst ? m_SBTStride : 0;

	VkStridedDeviceAddressRegionKHR rgenRegion = {};
	rgenRegion.deviceAddress = sbtAddress + rgenOffset;
	rgenRegion.stride        = m_SBTStride;
	rgenRegion.size          = m_SBTStride;

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