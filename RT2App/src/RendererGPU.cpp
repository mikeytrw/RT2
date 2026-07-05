#include "RendererGPU.h"
#include "ShaderManager.h"
#include "RTLog.h"
#include "VulkanUtils.h"
#include "Walnut/Application.h"
#include "Walnut/RTDispatch.h"
#include "backends/imgui_impl_vulkan.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <iostream>
#include <cstring>

uint32_t RendererGPU::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
	VkPhysicalDeviceMemoryProperties memProperties;
	vkGetPhysicalDeviceMemoryProperties(Walnut::Application::GetPhysicalDevice(), &memProperties);

	for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++)
	{
		if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
			return i;
	}
	RT_LOG("[FindMemoryType] FAILED: typeFilter=0x%X properties=0x%X — no matching memory type", typeFilter, properties);
	return 0xFFFFFFFF; // invalid — callers should check
}

void RendererGPU::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
                                VkBuffer& buffer, VkDeviceMemory& memory)
{
	VkDevice device = Walnut::Application::GetDevice();

	VkBufferCreateInfo bufferInfo = {};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = size;
	bufferInfo.usage = usage;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VK_CHECK(vkCreateBuffer(device, &bufferInfo, nullptr, &buffer));

	VkMemoryRequirements memRequirements;
	vkGetBufferMemoryRequirements(device, buffer, &memRequirements);

	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memRequirements.size;
	allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits, properties);
	if (allocInfo.memoryTypeIndex == 0xFFFFFFFF)
	{
		RT_LOG("[CreateBuffer] no valid memory type for buffer size=%llu", (unsigned long long)size);
		return;
	}

	VkMemoryAllocateFlagsInfo allocateFlagsInfo = {};
	if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
	{
		allocateFlagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
		allocateFlagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
		allocInfo.pNext = &allocateFlagsInfo;
	}

	VK_CHECK(vkAllocateMemory(device, &allocInfo, nullptr, &memory));
	VK_CHECK(vkBindBufferMemory(device, buffer, memory, 0));
}

void RendererGPU::DestroyBuffer(VkBuffer& buffer, VkDeviceMemory& memory)
{
	VkDevice device = Walnut::Application::GetDevice();
	if (buffer) { vkDestroyBuffer(device, buffer, nullptr); buffer = VK_NULL_HANDLE; }
	if (memory) { vkFreeMemory(device, memory, nullptr);    memory = VK_NULL_HANDLE; }
}

VkDeviceAddress RendererGPU::GetBufferDeviceAddress(VkBuffer buffer)
{
	VkBufferDeviceAddressInfo info = {};
	info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
	info.buffer = buffer;
	return vkGetBufferDeviceAddress(Walnut::Application::GetDevice(), &info);
}

bool RendererGPU::Init()
{
	if (m_Initialized) return true;

	if (!Walnut::Application::IsRayTracingSupported())
	{
		std::cerr << "[RT2] Ray tracing not supported, GPU renderer unavailable.\n";
		return false;
	}

	CreatePipeline();
	if (m_RTPipeline == VK_NULL_HANDLE)
	{
		std::cerr << "[RT2] GPU renderer initialization failed (shader or pipeline creation error)\n";
		return false;
	}
	CreateComposePipeline();
	m_Initialized = true;
	return true;
}

void RendererGPU::Destroy()
{
	VkDevice device = Walnut::Application::GetDevice();
	vkDeviceWaitIdle(device);

	m_NRD.Destroy();
	DestroyPipeline();
	DestroyComposePipeline();
	DestroyOutputImage();
	DestroyGBufferImages();
	DestroyTextures();
	DestroyEnvMapCDFTextures();
	DestroyBuffer(m_CameraUBO, m_CameraUBOMemory);
	DestroyBuffer(m_MaterialBuffer, m_MaterialBufferMemory);
	DestroyBuffer(m_LightBuffer, m_LightBufferMemory);
	DestroyBuffer(m_NRDUBO, m_NRDUBOMemory);

	if (m_GBufferPool)
	{
		vkDestroyDescriptorPool(device, m_GBufferPool, nullptr);
		m_GBufferPool = VK_NULL_HANDLE;
	}
	if (m_GBufferSetLayout)
	{
		vkDestroyDescriptorSetLayout(device, m_GBufferSetLayout, nullptr);
		m_GBufferSetLayout = VK_NULL_HANDLE;
	}

	m_Initialized = false;
}

void RendererGPU::CreatePipeline()
{
	VkDevice device = Walnut::Application::GetDevice();

	// Load the six RT shader modules.
	m_RgenShader   = ShaderManager::LoadShader("raygen.spv");
	if (!m_RgenShader)   m_RgenShader   = ShaderManager::LoadShader("RT2App/shaders/raygen.spv");
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

	if (!m_RgenShader || !m_MissShader || !m_ShadowShader || !m_ClosestShader ||
	    !m_AnyHitShader || !m_ShadowHitShader)
	{
		std::cerr << "[RT2] Failed to load RT shaders\n";
		return;
	}

	// Descriptor set layout — 11 bindings:
	//   0: storage image (output)
	//   1: uniform buffer (camera)
	//   2: storage buffer (materials)
	//   3: storage buffer (combined normals)
	//   4: acceleration structure (TLAS)
	//   5: storage buffer (per-instance normal offsets)
	//   6: storage buffer (combined tangents, 3 per triangle)
	//   7: storage buffer (combined UVs, 3 per triangle)
	//   8: storage buffer (combined positions, 3 per triangle)
	//   9: storage buffer (lights for NEE, std430 with header)
	//  10: combined image sampler array (textures, bindless, variable count) — MUST be last binding
	const VkShaderStageFlags allRTFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR |
	                                      VK_SHADER_STAGE_MISS_BIT_KHR |
	                                      VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
	                                      VK_SHADER_STAGE_ANY_HIT_BIT_KHR;

	const uint32_t maxTextures = 1024;

	VkDescriptorSetLayoutBinding bindings[11] = {};

	bindings[0] = {};
	bindings[0].binding = 0;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	bindings[0].descriptorCount = 1;
	bindings[0].stageFlags = allRTFlags;

	bindings[1] = {};
	bindings[1].binding = 1;
	bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	bindings[1].descriptorCount = 1;
	bindings[1].stageFlags = allRTFlags;

	bindings[2] = {};
	bindings[2].binding = 2;
	bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[2].descriptorCount = 1;
	bindings[2].stageFlags = allRTFlags;

	bindings[3] = {};
	bindings[3].binding = 3;
	bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[3].descriptorCount = 1;
	bindings[3].stageFlags = allRTFlags;

	bindings[4] = {};
	bindings[4].binding = 4;
	bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
	bindings[4].descriptorCount = 1;
	bindings[4].stageFlags = allRTFlags;

	bindings[5] = {};
	bindings[5].binding = 5;
	bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[5].descriptorCount = 1;
	bindings[5].stageFlags = allRTFlags;

	bindings[6] = {};
	bindings[6].binding = 6;
	bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[6].descriptorCount = 1;
	bindings[6].stageFlags = allRTFlags;

	bindings[7] = {};
	bindings[7].binding = 7;
	bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[7].descriptorCount = 1;
	bindings[7].stageFlags = allRTFlags;

	bindings[8] = {};
	bindings[8].binding = 8;
	bindings[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[8].descriptorCount = 1;
	bindings[8].stageFlags = allRTFlags;

	bindings[9] = {};
	bindings[9].binding = 9;
	bindings[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[9].descriptorCount = 1;
	bindings[9].stageFlags = allRTFlags;

	bindings[10] = {};
	bindings[10].binding = 10;
	bindings[10].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[10].descriptorCount = maxTextures;
	bindings[10].stageFlags = allRTFlags;

	// Binding flags: binding 10 (texture array) is partially bound + variable descriptor count
	// Must be the highest-numbered binding per Vulkan spec.
	VkDescriptorBindingFlagsEXT bindingFlags[11] = {};
	bindingFlags[10] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT_EXT |
	                   VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT_EXT;

	VkDescriptorSetLayoutBindingFlagsCreateInfoEXT bindingFlagsInfo = {};
	bindingFlagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO_EXT;
	bindingFlagsInfo.bindingCount = 11;
	bindingFlagsInfo.pBindingFlags = bindingFlags;

	VkDescriptorSetLayoutCreateInfo layoutInfo = {};
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = 11;
	layoutInfo.pBindings = bindings;
	layoutInfo.pNext = &bindingFlagsInfo;

	vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_DescriptorSetLayout);

	// Pipeline layout
	// Create G-buffer descriptor set layout (set 1)
	CreateGBufferDescriptorSet();

	VkDescriptorSetLayout setLayouts[2] = { m_DescriptorSetLayout, m_GBufferSetLayout };

	VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = 2;
	pipelineLayoutInfo.pSetLayouts = setLayouts;

	vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &m_PipelineLayout);

	// Ray tracing pipeline
	const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& rtProps =
		Walnut::Application::GetRayTracingPipelineProperties();

	// 6 stages: rgen, miss_sky, miss_shadow, closesthit, anyhit, shadow_anyhit
	VkPipelineShaderStageCreateInfo stages[6] = {};
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

	// 6 groups — SBT hit layout: [primary_opaque, primary_alpha, shadow_opaque, shadow_alpha]
	//   Primary rays:  hit offset 0, instance adds 0 (opaque) or 1 (alpha)
	//   Shadow rays:   hit offset 2, instance adds 0 (opaque) or 1 (alpha)
	VkRayTracingShaderGroupCreateInfoKHR groups[6] = {};
	// 0: raygen
	groups[0].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
	groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
	groups[0].generalShader = 0;
	groups[0].closestHitShader = VK_SHADER_UNUSED_KHR;
	groups[0].anyHitShader = VK_SHADER_UNUSED_KHR;
	groups[0].intersectionShader = VK_SHADER_UNUSED_KHR;
	// 1: miss (sky) — SBT miss index 0
	groups[1].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
	groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
	groups[1].generalShader = 1;
	groups[1].closestHitShader = VK_SHADER_UNUSED_KHR;
	groups[1].anyHitShader = VK_SHADER_UNUSED_KHR;
	groups[1].intersectionShader = VK_SHADER_UNUSED_KHR;
	// 2: miss (shadow) — SBT miss index 1
	groups[2].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
	groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
	groups[2].generalShader = 2;
	groups[2].closestHitShader = VK_SHADER_UNUSED_KHR;
	groups[2].anyHitShader = VK_SHADER_UNUSED_KHR;
	groups[2].intersectionShader = VK_SHADER_UNUSED_KHR;
	// 3: hit_primary_opaque — closesthit only (SBT hit offset 0)
	groups[3].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
	groups[3].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
	groups[3].generalShader = VK_SHADER_UNUSED_KHR;
	groups[3].closestHitShader = 3;
	groups[3].anyHitShader = VK_SHADER_UNUSED_KHR;
	groups[3].intersectionShader = VK_SHADER_UNUSED_KHR;
	// 4: hit_primary_alpha — closesthit + anyhit (SBT hit offset 0 + instance offset 1)
	groups[4].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
	groups[4].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
	groups[4].generalShader = VK_SHADER_UNUSED_KHR;
	groups[4].closestHitShader = 3;
	groups[4].anyHitShader = 4;
	groups[4].intersectionShader = VK_SHADER_UNUSED_KHR;
	// 5: hit_shadow — shadow anyhit only, no closesthit (SBT hit offset 2 + instance offset 0 or 1)
	groups[5].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
	groups[5].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
	groups[5].generalShader = VK_SHADER_UNUSED_KHR;
	groups[5].closestHitShader = VK_SHADER_UNUSED_KHR;
	groups[5].anyHitShader = 5;
	groups[5].intersectionShader = VK_SHADER_UNUSED_KHR;

	// Recursion depth: a path of maxBounces bounces needs maxBounces+1 levels
	// (bounce chain + NEE shadow rays at the deepest shading level). Do NOT
	// request the device maximum (31 on NVIDIA): the driver sizes the default
	// ray stack proportionally to maxPipelineRayRecursionDepth, so an oversized
	// value wastes stack memory and kills occupancy. The UBO maxBounces value
	// is clamped to m_MaxRecursionDepth - 1 so the slider can never exceed it.
	const uint32_t kRecursionCap = 16;
	m_MaxRecursionDepth = rtProps.maxRayRecursionDepth < kRecursionCap
	                    ? rtProps.maxRayRecursionDepth : kRecursionCap;
	if (m_MaxRecursionDepth < 2) m_MaxRecursionDepth = 2;

	VkRayTracingPipelineCreateInfoKHR pipelineInfo = {};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
	pipelineInfo.stageCount = 6;
	pipelineInfo.pStages = stages;
	pipelineInfo.groupCount = 6;
	pipelineInfo.pGroups = groups;
	pipelineInfo.maxPipelineRayRecursionDepth = m_MaxRecursionDepth;
	pipelineInfo.layout = m_PipelineLayout;

	VkResult err = g_RTDispatch.CreateRayTracingPipelinesKHR(
		device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_RTPipeline);
	if (err != VK_SUCCESS)
	{
		std::cerr << "[RT2] vkCreateRayTracingPipelinesKHR failed: " << err << "\n";
		m_RTPipeline = VK_NULL_HANDLE;
		return;
	}

	// ---- Shader Binding Table ----
	// Layout: [rgen][miss_sky][miss_shadow][hit_primary_opaque][hit_primary_alpha][hit_shadow_opaque][hit_shadow_alpha]
	// Each handle padded to baseAlignment.
	// Miss region: 2 entries (sky + shadow) = 2 × baseAlign
	// Hit region:  4 entries (primary_opaque, primary_alpha, shadow_opaque, shadow_alpha) = 4 × baseAlign
	//   Primary rays: hit offset 0, instance adds 0 (opaque) or 1 (alpha)
	//   Shadow rays:  hit offset 2, instance adds 0 (opaque) or 1 (alpha)
	const uint32_t handleSize  = rtProps.shaderGroupHandleSize;
	const uint32_t baseAlign   = std::max<uint32_t>(rtProps.shaderGroupBaseAlignment, 1);
	const uint32_t handleAlign = std::max<uint32_t>(rtProps.shaderGroupHandleAlignment, 1);

	m_SBTStride       = baseAlign;
	m_RgenRegionSize  = baseAlign;
	m_MissRegionSize  = baseAlign * 2;  // 2 miss entries: sky + shadow
	m_HitRegionSize   = baseAlign * 4;  // 4 hit entries: primary_opaque, primary_alpha, shadow_opaque, shadow_alpha

	VkDeviceSize sbtSize = m_RgenRegionSize + m_MissRegionSize + m_HitRegionSize;

	CreateBuffer(sbtSize,
	             VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
	             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
	             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	             m_SBTBuffer, m_SBTMemory);
	m_SBTSize = sbtSize;

	// Fetch all six group handles in one call.
	std::vector<uint8_t> handles(6 * handleSize);
	err = g_RTDispatch.GetRayTracingShaderGroupHandlesKHR(
		device, m_RTPipeline, 0, 6, handles.size(), handles.data());
	if (err != VK_SUCCESS)
	{
		std::cerr << "[RT2] vkGetRayTracingShaderGroupHandlesKHR failed: " << err << "\n";
		return;
	}

	// Copy handles into the SBT buffer at baseAlignment-aligned offsets.
	// Hit region: [group3=primary_opaque, group4=primary_alpha, group5=shadow, group5=shadow]
	// (shadow_opaque and shadow_alpha both point to the same group 5)
	void* mapped = nullptr;
	vkMapMemory(device, m_SBTMemory, 0, sbtSize, 0, &mapped);
	std::memset(mapped, 0, (size_t)sbtSize);
	uint8_t* dst = static_cast<uint8_t*>(mapped);
	std::memcpy(dst + 0,                       handles.data() + 0 * handleSize, handleSize); // rgen
	std::memcpy(dst + m_RgenRegionSize,        handles.data() + 1 * handleSize, handleSize); // miss_sky
	std::memcpy(dst + m_RgenRegionSize + baseAlign,
	            handles.data() + 2 * handleSize, handleSize);                                 // miss_shadow
	// Hit region: 4 entries at baseAlign stride
	VkDeviceSize hitBase = m_RgenRegionSize + m_MissRegionSize;
	std::memcpy(dst + hitBase,                   handles.data() + 3 * handleSize, handleSize); // primary_opaque
	std::memcpy(dst + hitBase + baseAlign,       handles.data() + 4 * handleSize, handleSize); // primary_alpha
	std::memcpy(dst + hitBase + baseAlign * 2,   handles.data() + 5 * handleSize, handleSize); // shadow_opaque
	std::memcpy(dst + hitBase + baseAlign * 3,   handles.data() + 5 * handleSize, handleSize); // shadow_alpha (same shader)
	vkUnmapMemory(device, m_SBTMemory);

	// Print the SBT device address and region layout for verification.
	VkDeviceAddress sbtAddr = GetBufferDeviceAddress(m_SBTBuffer);
	std::cerr << "[RT2] SBT deviceAddress=0x" << std::hex << sbtAddr << std::dec
	          << " stride=" << m_SBTStride << "\n";
}

void RendererGPU::DestroyPipeline()
{
	VkDevice device = Walnut::Application::GetDevice();
	if (m_RTPipeline) vkDestroyPipeline(device, m_RTPipeline, nullptr);
	if (m_PipelineLayout) vkDestroyPipelineLayout(device, m_PipelineLayout, nullptr);
	if (m_DescriptorSetLayout) vkDestroyDescriptorSetLayout(device, m_DescriptorSetLayout, nullptr);
	if (m_RgenShader)   vkDestroyShaderModule(device, m_RgenShader, nullptr);
	if (m_MissShader)    vkDestroyShaderModule(device, m_MissShader, nullptr);
	if (m_ShadowShader)  vkDestroyShaderModule(device, m_ShadowShader, nullptr);
	if (m_ClosestShader) vkDestroyShaderModule(device, m_ClosestShader, nullptr);
	if (m_AnyHitShader)  vkDestroyShaderModule(device, m_AnyHitShader, nullptr);
	if (m_ShadowHitShader) vkDestroyShaderModule(device, m_ShadowHitShader, nullptr);
	DestroyBuffer(m_SBTBuffer, m_SBTMemory);
	m_RTPipeline = VK_NULL_HANDLE;
	m_PipelineLayout = VK_NULL_HANDLE;
	m_DescriptorSetLayout = VK_NULL_HANDLE;
	m_RgenShader = m_MissShader = m_ShadowShader = m_ClosestShader = VK_NULL_HANDLE;
	m_AnyHitShader = m_ShadowHitShader = VK_NULL_HANDLE;
	m_SBTBuffer = VK_NULL_HANDLE;
	m_SBTMemory = VK_NULL_HANDLE;
	m_SBTSize = 0;
}

void RendererGPU::CreateOutputImage()
{
	VkDevice device = Walnut::Application::GetDevice();

	VkImageCreateInfo imageInfo = {};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
	imageInfo.extent.width = m_Width;
	imageInfo.extent.height = m_Height;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VkResult err = vkCreateImage(device, &imageInfo, nullptr, &m_OutputImage);
	if (err != VK_SUCCESS)
	{
		std::cerr << "[RT2] vkCreateImage failed: " << err << "\n";
		return;
	}

	VkMemoryRequirements memRequirements;
	vkGetImageMemoryRequirements(device, m_OutputImage, &memRequirements);

	VkMemoryAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memRequirements.size;
	allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	err = vkAllocateMemory(device, &allocInfo, nullptr, &m_OutputMemory);
	if (err != VK_SUCCESS)
	{
		std::cerr << "[RT2] vkAllocateMemory for output image failed: " << err << " (size=" << memRequirements.size << ")\n";
		vkDestroyImage(device, m_OutputImage, nullptr);
		m_OutputImage = VK_NULL_HANDLE;
		return;
	}
	vkBindImageMemory(device, m_OutputImage, m_OutputMemory, 0);

	VkImageViewCreateInfo viewInfo = {};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = m_OutputImage;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = 1;

	vkCreateImageView(device, &viewInfo, nullptr, &m_OutputImageView);

	VkSamplerCreateInfo samplerInfo = {};
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = VK_FILTER_LINEAR;
	samplerInfo.minFilter = VK_FILTER_LINEAR;
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.minLod = -1000;
	samplerInfo.maxLod = 1000;

	vkCreateSampler(device, &samplerInfo, nullptr, &m_Sampler);

	// Transition image to general layout for compute writes
	VkCommandBuffer cmd = Walnut::Application::GetCommandBuffer(true);

	VkImageMemoryBarrier barrier = {};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = m_OutputImage;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;

	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0, nullptr, 0, nullptr, 1, &barrier);

	Walnut::Application::FlushCommandBuffer(cmd);

	m_OutputImageLayout = VK_IMAGE_LAYOUT_GENERAL;

	// Create ImGui descriptor set for display
	m_ImGuiDescriptorSet = (VkDescriptorSet)ImGui_ImplVulkan_AddTexture(m_Sampler, m_OutputImageView, VK_IMAGE_LAYOUT_GENERAL);
}

void RendererGPU::DestroyOutputImage()
{
	VkDevice device = Walnut::Application::GetDevice();

	// Free the ImGui descriptor set before destroying the image view/sampler
	// it references — otherwise the GPU may sample a destroyed resource.
	if (m_ImGuiDescriptorSet)
	{
		vkFreeDescriptorSets(device, Walnut::Application::GetDescriptorPool(),
		                     1, &m_ImGuiDescriptorSet);
		m_ImGuiDescriptorSet = VK_NULL_HANDLE;
	}

	if (m_Sampler) vkDestroySampler(device, m_Sampler, nullptr);
	if (m_OutputImageView) vkDestroyImageView(device, m_OutputImageView, nullptr);
	if (m_OutputImage) vkDestroyImage(device, m_OutputImage, nullptr);
	if (m_OutputMemory) vkFreeMemory(device, m_OutputMemory, nullptr);
	m_Sampler = VK_NULL_HANDLE;
	m_OutputImageView = VK_NULL_HANDLE;
	m_OutputImage = VK_NULL_HANDLE;
	m_OutputMemory = VK_NULL_HANDLE;
	m_OutputImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}

void RendererGPU::OnResize(uint32_t width, uint32_t height)
{
	if (width == 0 || height == 0)
		return;

	if (m_Width == width && m_Height == height && m_OutputImage != VK_NULL_HANDLE)
		return;

	VkDevice device = Walnut::Application::GetDevice();
	vkDeviceWaitIdle(device);

	// Free old descriptor set before allocating a new one
	if (m_DescriptorSet)
	{
		vkFreeDescriptorSets(device, Walnut::Application::GetDescriptorPool(), 1, &m_DescriptorSet);
		m_DescriptorSet = VK_NULL_HANDLE;
	}

	DestroyOutputImage();

	m_Width = width;
	m_Height = height;

	CreateOutputImage();
	CreateGBufferImages();
	CreateDescriptorSet();
	UpdateGBufferDescriptorSet();

	// Compose pass descriptor set (needs NRD output views + albedoF0 view)
	if (m_ComposeSetLayout && !m_ComposeSet)
		CreateComposeDescriptorSet();
	if (m_ComposeSet)
		UpdateComposeDescriptorSet();

	// Initialize NRD if enabled
	if (m_NRDEnabled && !m_NRD.IsAvailable())
	{
		m_NRD.Init(Walnut::Application::GetInstance(),
		           Walnut::Application::GetPhysicalDevice(),
		           Walnut::Application::GetDevice(),
		           Walnut::Application::GetQueue(),
		           Walnut::Application::GetQueueFamily(),
		           m_Width, m_Height);
	}
	else if (m_NRDEnabled && m_NRD.IsAvailable())
	{
		m_NRD.OnResize(m_Width, m_Height);
	}
	UpdateDescriptorSet();

	m_FrameIndex = 1;
}

void RendererGPU::CreateDescriptorSet()
{
	VkDevice device = Walnut::Application::GetDevice();

	// Allocate with fixed maxTextures count so the descriptor set is valid
	// regardless of how many textures are currently loaded. The variable
	// descriptor count feature allows us to specify the actual count at
	// allocation time, but using the max avoids reallocation when textures
	// are added/removed after a resize.
	const uint32_t maxTextures = 1024;

	VkDescriptorSetVariableDescriptorCountAllocateInfoEXT varCountInfo = {};
	varCountInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO_EXT;
	varCountInfo.descriptorSetCount = 1;
	varCountInfo.pDescriptorCounts = &maxTextures;

	VkDescriptorSetAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.descriptorPool = Walnut::Application::GetDescriptorPool();
	allocInfo.descriptorSetCount = 1;
	allocInfo.pSetLayouts = &m_DescriptorSetLayout;
	allocInfo.pNext = &varCountInfo;

	vkAllocateDescriptorSets(device, &allocInfo, &m_DescriptorSet);
}

void RendererGPU::UpdateDescriptorSet()
{
	if (!m_AS.IsValid()) { RT_LOG("[UpdateDS] skip: AS not valid"); return; }
	if (!m_DescriptorSet) { RT_LOG("[UpdateDS] skip: no descriptor set"); return; }
	if (!m_AS.GetNormalBuffer()) { RT_LOG("[UpdateDS] skip: no normal buffer"); return; }
	if (!m_MaterialBuffer) { RT_LOG("[UpdateDS] skip: no material buffer"); return; }

	RT_LOG("[UpdateDS] enter: textures=%d validTextures=%d",
	       (int)m_Textures.size(), (int)[&]() { int c = 0; for (auto& t : m_Textures) if (t.view && t.sampler) c++; return c; }());

	VkDevice device = Walnut::Application::GetDevice();

	// Create camera UBO if needed
	if (!m_CameraUBO)
	{
		CreateBuffer(512,
		             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
		             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		             m_CameraUBO, m_CameraUBOMemory);
	}

	if (!m_MaterialBuffer)
	{
		std::cerr << "[RT2] Warning: material buffer not created yet\n";
		return;
	}

	if (!m_LightBuffer)
	{
		std::cerr << "[RT2] Warning: light buffer not created yet\n";
		return;
	}

	VkDescriptorImageInfo imageInfo = {};
	imageInfo.imageView = m_OutputImageView;
	imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	imageInfo.sampler = m_Sampler;

	VkDescriptorBufferInfo cameraBufferInfo = {};
	cameraBufferInfo.buffer = m_CameraUBO;
	cameraBufferInfo.range = VK_WHOLE_SIZE;

	VkDescriptorBufferInfo materialBufferInfo = {};
	materialBufferInfo.buffer = m_MaterialBuffer;
	materialBufferInfo.range = VK_WHOLE_SIZE;

	VkDescriptorBufferInfo normalBufferInfo = {};
	normalBufferInfo.buffer = m_AS.GetNormalBuffer();
	normalBufferInfo.range = VK_WHOLE_SIZE;

	VkDescriptorBufferInfo offsetBufferInfo = {};
	offsetBufferInfo.buffer = m_AS.GetInstanceOffsetBuffer();
	offsetBufferInfo.range = VK_WHOLE_SIZE;

	VkDescriptorBufferInfo uvBufferInfo = {};
	uvBufferInfo.buffer = m_AS.GetUVBuffer();
	uvBufferInfo.range = VK_WHOLE_SIZE;

	VkDescriptorBufferInfo positionBufferInfo = {};
	positionBufferInfo.buffer = m_AS.GetPositionBuffer();
	positionBufferInfo.range = VK_WHOLE_SIZE;

	VkDescriptorBufferInfo tangentBufferInfo = {};
	tangentBufferInfo.buffer = m_AS.GetTangentBuffer();
	tangentBufferInfo.range = VK_WHOLE_SIZE;

	VkDescriptorBufferInfo lightBufferInfo = {};
	lightBufferInfo.buffer = m_LightBuffer;
	lightBufferInfo.range = VK_WHOLE_SIZE;

	// Texture array image infos (binding 10) — write only valid textures
	std::vector<VkDescriptorImageInfo> textureImageInfos;
	for (const auto& gt : m_Textures)
	{
		if (!gt.view || !gt.sampler) continue; // skip unloaded textures
		VkDescriptorImageInfo imgInfo = {};
		imgInfo.sampler = gt.sampler;
		imgInfo.imageView = gt.view;
		imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		textureImageInfos.push_back(imgInfo);
	}

	VkWriteDescriptorSetAccelerationStructureKHR asInfo = {};
	asInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
	asInfo.accelerationStructureCount = 1;
	VkAccelerationStructureKHR tlas = m_AS.GetTLAS();
	asInfo.pAccelerationStructures = &tlas;

	std::cerr << "[RT2] UpdateDescriptorSet: TLAS=" << (void*)tlas
	          << " descSet=" << m_DescriptorSet << "\n";

	VkWriteDescriptorSet writes[11] = {};

	writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[0].dstSet = m_DescriptorSet;
	writes[0].dstBinding = 0;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	writes[0].descriptorCount = 1;
	writes[0].pImageInfo = &imageInfo;

	writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[1].dstSet = m_DescriptorSet;
	writes[1].dstBinding = 1;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	writes[1].descriptorCount = 1;
	writes[1].pBufferInfo = &cameraBufferInfo;

	writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[2].dstSet = m_DescriptorSet;
	writes[2].dstBinding = 2;
	writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[2].descriptorCount = 1;
	writes[2].pBufferInfo = &materialBufferInfo;

	writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[3].dstSet = m_DescriptorSet;
	writes[3].dstBinding = 3;
	writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[3].descriptorCount = 1;
	writes[3].pBufferInfo = &normalBufferInfo;

	writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[4].dstSet = m_DescriptorSet;
	writes[4].dstBinding = 4;
	writes[4].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
	writes[4].descriptorCount = 1;
	writes[4].pNext = &asInfo;

	writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[5].dstSet = m_DescriptorSet;
	writes[5].dstBinding = 5;
	writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[5].descriptorCount = 1;
	writes[5].pBufferInfo = &offsetBufferInfo;

	writes[6] = {};
	writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[6].dstSet = m_DescriptorSet;
	writes[6].dstBinding = 6;
	writes[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[6].descriptorCount = 1;
	writes[6].pBufferInfo = &tangentBufferInfo;

	writes[7] = {};
	writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[7].dstSet = m_DescriptorSet;
	writes[7].dstBinding = 7;
	writes[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[7].descriptorCount = 1;
	writes[7].pBufferInfo = &uvBufferInfo;

	writes[8] = {};
	writes[8].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[8].dstSet = m_DescriptorSet;
	writes[8].dstBinding = 8;
	writes[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[8].descriptorCount = 1;
	writes[8].pBufferInfo = &positionBufferInfo;

	writes[9] = {};
	writes[9].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[9].dstSet = m_DescriptorSet;
	writes[9].dstBinding = 9;
	writes[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[9].descriptorCount = 1;
	writes[9].pBufferInfo = &lightBufferInfo;

	uint32_t writeCount = 10; // bindings 0-9 always written
	if (!textureImageInfos.empty())
	{
		writes[10].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[10].dstSet = m_DescriptorSet;
		writes[10].dstBinding = 10;
		writes[10].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[10].descriptorCount = (uint32_t)textureImageInfos.size();
		writes[10].pImageInfo = textureImageInfos.data();
		writeCount = 11;
	}
	RT_LOG("[UpdateDS] writing %d descriptors (textureInfos=%d)", writeCount, (int)textureImageInfos.size());
	vkUpdateDescriptorSets(device, writeCount, writes, 0, nullptr);
	RT_LOG("[UpdateDS] done");
}

void RendererGPU::CreateMaterialBuffer()
{
	DestroyBuffer(m_MaterialBuffer, m_MaterialBufferMemory);

	size_t matCount = m_CurrentScene.materials.size();
	if (matCount == 0) matCount = 1; // always at least 1

	m_MaterialBufferSize = matCount * sizeof(GPUMaterial);

	CreateBuffer(m_MaterialBufferSize,
	             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
	             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	             m_MaterialBuffer, m_MaterialBufferMemory);

	VkDevice device = Walnut::Application::GetDevice();
	void* data;
	vkMapMemory(device, m_MaterialBufferMemory, 0, m_MaterialBufferSize, 0, &data);
	memcpy(data, m_CurrentScene.materials.data(), m_MaterialBufferSize);
	vkUnmapMemory(device, m_MaterialBufferMemory);
}

void RendererGPU::CreateLightBuffer()
{
	DestroyBuffer(m_LightBuffer, m_LightBufferMemory);

	// Layout: 16-byte header + N * sizeof(GPUTriangleLight)
	// Header: uint lightCount, float totalLightArea, uint pad, uint pad
	size_t lightCount = m_CurrentScene.lights.size();
	m_LightBufferSize = 16 + lightCount * sizeof(GPUTriangleLight);

	// Always at least the header so the buffer is non-empty
	if (m_LightBufferSize < 16) m_LightBufferSize = 16;

	CreateBuffer(m_LightBufferSize,
	             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
	             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	             m_LightBuffer, m_LightBufferMemory);

	// Build the buffer contents: header + lights
	std::vector<uint8_t> bufData(m_LightBufferSize, 0);
	uint32_t count = static_cast<uint32_t>(lightCount);
	float totalArea = m_CurrentScene.totalLightArea;
	memcpy(bufData.data() + 0,  &count,     sizeof(uint32_t));
	memcpy(bufData.data() + 4,  &totalArea, sizeof(float));
	// bytes 8-15: padding (already zeroed)
	if (lightCount > 0)
	{
		memcpy(bufData.data() + 16, m_CurrentScene.lights.data(),
		       lightCount * sizeof(GPUTriangleLight));
	}

	VkDevice device = Walnut::Application::GetDevice();
	void* data;
	vkMapMemory(device, m_LightBufferMemory, 0, m_LightBufferSize, 0, &data);
	memcpy(data, bufData.data(), m_LightBufferSize);
	vkUnmapMemory(device, m_LightBufferMemory);

	std::cerr << "[RT2] Light buffer: " << lightCount << " lights, totalArea=" << totalArea << "\n";
}

void RendererGPU::CreateTextures(const std::vector<SceneTexture>& textures)
{
	VkDevice device = Walnut::Application::GetDevice();
	DestroyTextures();

	m_Textures.resize(textures.size());

	for (size_t i = 0; i < textures.size(); i++)
	{
		const auto& tex = textures[i];
		if (tex.floatPixels.empty() && tex.pixels.empty())
		{
			std::cerr << "[RT2] Texture " << i << ": no pixel data, skipping\n";
			continue;
		}

		GPUTexture& gt = m_Textures[i];
		gt.width = tex.width;
		gt.height = tex.height;

		bool isHDR = tex.isHDR && !tex.floatPixels.empty();
		VkFormat format = isHDR ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
		gt.format = format;
		VkDeviceSize imageSize = isHDR
			? (VkDeviceSize)(tex.width * tex.height * 8)  // 4× half-float = 8 bytes
			: (VkDeviceSize)(tex.width * tex.height * 4);  // 4× uint8 = 4 bytes

		// For HDR, convert float pixels to half-float
		std::vector<uint16_t> halfPixels;
		if (isHDR)
		{
			halfPixels.resize(tex.width * tex.height * 4);
			for (size_t p = 0; p < tex.floatPixels.size(); p++)
				halfPixels[p] = glm::packHalf1x16(tex.floatPixels[p]);
		}

		// Staging buffer
		VkBuffer stagingBuffer;
		VkDeviceMemory stagingMemory;
		CreateBuffer(imageSize,
		             VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		             stagingBuffer, stagingMemory);

		void* data;
		vkMapMemory(device, stagingMemory, 0, imageSize, 0, &data);
		if (isHDR)
			memcpy(data, halfPixels.data(), (size_t)imageSize);
		else
			memcpy(data, tex.pixels.data(), (size_t)imageSize);
		vkUnmapMemory(device, stagingMemory);

		// Create VkImage
		VkImageCreateInfo imageInfo = {};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.format = format;
		imageInfo.extent.width = tex.width;
		imageInfo.extent.height = tex.height;
		imageInfo.extent.depth = 1;
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = 1;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		vkCreateImage(device, &imageInfo, nullptr, &gt.image);

		VkMemoryRequirements memReq;
		vkGetImageMemoryRequirements(device, gt.image, &memReq);

		VkMemoryAllocateInfo allocInfo = {};
		allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocInfo.allocationSize = memReq.size;
		allocInfo.memoryTypeIndex = FindMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

		vkAllocateMemory(device, &allocInfo, nullptr, &gt.memory);
		vkBindImageMemory(device, gt.image, gt.memory, 0);

		// Transition + copy via command buffer
		VkCommandBuffer cmd = Walnut::Application::GetCommandBuffer(true);

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
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.layerCount = 1;

		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

		VkBufferImageCopy region = {};
		region.bufferOffset = 0;
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

		// Transition to shader read optimal
		VkImageMemoryBarrier shaderBarrier = barrier;
		shaderBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		shaderBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		shaderBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		shaderBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &shaderBarrier);

		Walnut::Application::FlushCommandBuffer(cmd);

		DestroyBuffer(stagingBuffer, stagingMemory);

		// Create image view
		VkImageViewCreateInfo viewInfo = {};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = gt.image;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = format;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.layerCount = 1;
		vkCreateImageView(device, &viewInfo, nullptr, &gt.view);

		// Create sampler
		VkSamplerCreateInfo samplerInfo = {};
		samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		samplerInfo.magFilter = VK_FILTER_LINEAR;
		samplerInfo.minFilter = VK_FILTER_LINEAR;
		samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.minLod = 0;
		samplerInfo.maxLod = 0;
		vkCreateSampler(device, &samplerInfo, nullptr, &gt.sampler);
	}

	std::cerr << "[RT2] Created " << m_Textures.size() << " GPU textures\n";
}

void RendererGPU::DestroyTextures()
{
	VkDevice device = Walnut::Application::GetDevice();
	for (auto& gt : m_Textures)
	{
		if (gt.sampler) vkDestroySampler(device, gt.sampler, nullptr);
		if (gt.view) vkDestroyImageView(device, gt.view, nullptr);
		if (gt.image) vkDestroyImage(device, gt.image, nullptr);
		if (gt.memory) vkFreeMemory(device, gt.memory, nullptr);
	}
	m_Textures.clear();
}

void RendererGPU::CreateEnvMapCDFTextures(const GPUSceneData& sceneData)
{
	m_EnvMapIndex = sceneData.envMapIndex;
	m_CDFWidth = sceneData.cdfWidth;
	m_CDFHeight = sceneData.cdfHeight;
	m_MarginalCDFIndex = -1;
	m_ConditionalCDFIndex = -1;

	if (sceneData.envMapIndex < 0 || sceneData.marginalCDF.empty() || sceneData.conditionalCDF.empty())
		return;

	VkDevice device = Walnut::Application::GetDevice();

	// Helper: create a CDF texture and append to m_Textures
	auto createCDFTexture = [&](const std::vector<float>& cdfData, int w, int h) -> int
	{
		GPUTexture gt;
		gt.width = w;
		gt.height = h;
		gt.format = VK_FORMAT_R32_SFLOAT;
		VkDeviceSize imageSize = (VkDeviceSize)(w * h * 4);

		VkBuffer stagingBuffer;
		VkDeviceMemory stagingMemory;
		CreateBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		             stagingBuffer, stagingMemory);
		void* data;
		vkMapMemory(device, stagingMemory, 0, imageSize, 0, &data);
		memcpy(data, cdfData.data(), (size_t)imageSize);
		vkUnmapMemory(device, stagingMemory);

		VkImageCreateInfo imageInfo = {};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = (h == 1) ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D;
		imageInfo.format = VK_FORMAT_R32_SFLOAT;
		imageInfo.extent.width = w;
		imageInfo.extent.height = (h == 1) ? 1 : h;
		imageInfo.extent.depth = 1;
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = 1;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		vkCreateImage(device, &imageInfo, nullptr, &gt.image);

		VkMemoryRequirements memReq;
		vkGetImageMemoryRequirements(device, gt.image, &memReq);
		VkMemoryAllocateInfo allocInfo = {};
		allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocInfo.allocationSize = memReq.size;
		allocInfo.memoryTypeIndex = FindMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		vkAllocateMemory(device, &allocInfo, nullptr, &gt.memory);
		vkBindImageMemory(device, gt.image, gt.memory, 0);

		VkCommandBuffer cmd = Walnut::Application::GetCommandBuffer(true);
		VkImageMemoryBarrier barrier = {};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.image = gt.image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.layerCount = 1;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

		VkBufferImageCopy region = {};
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.layerCount = 1;
		region.imageExtent = {(uint32_t)w, (h == 1) ? 1u : (uint32_t)h, 1};
		vkCmdCopyBufferToImage(cmd, stagingBuffer, gt.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

		VkImageMemoryBarrier shaderBarrier = barrier;
		shaderBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		shaderBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		shaderBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		shaderBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &shaderBarrier);
		Walnut::Application::FlushCommandBuffer(cmd);
		DestroyBuffer(stagingBuffer, stagingMemory);

		VkImageViewCreateInfo viewInfo = {};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = gt.image;
		viewInfo.viewType = (h == 1) ? VK_IMAGE_VIEW_TYPE_1D : VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = VK_FORMAT_R32_SFLOAT;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.layerCount = 1;
		vkCreateImageView(device, &viewInfo, nullptr, &gt.view);

		VkSamplerCreateInfo samplerInfo = {};
		samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		samplerInfo.magFilter = VK_FILTER_LINEAR;
		samplerInfo.minFilter = VK_FILTER_LINEAR;
		samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.minLod = 0;
		samplerInfo.maxLod = 0;
		vkCreateSampler(device, &samplerInfo, nullptr, &gt.sampler);

		int idx = (int)m_Textures.size();
		m_Textures.push_back(gt);
		return idx;
	};

	m_MarginalCDFIndex = createCDFTexture(sceneData.marginalCDF, sceneData.cdfHeight, 1);
	m_ConditionalCDFIndex = createCDFTexture(sceneData.conditionalCDF, sceneData.cdfWidth, sceneData.cdfHeight);

	std::cerr << "[RT2] Env map CDF textures: marginal idx=" << m_MarginalCDFIndex
	          << " conditional idx=" << m_ConditionalCDFIndex
	          << " (" << sceneData.cdfWidth << "x" << sceneData.cdfHeight << ")\n";
}

void RendererGPU::DestroyEnvMapCDFTextures()
{
	// CDF textures are in m_Textures, destroyed by DestroyTextures()
	m_EnvMapIndex = -1;
	m_MarginalCDFIndex = -1;
	m_ConditionalCDFIndex = -1;
}

void RendererGPU::ResetAccumulation()
{
	m_FrameIndex = 1;
	m_HasPrevMatrices = false;
}

void RendererGPU::SetScene(const GPUSceneData& sceneData)
{
	VkDevice device = Walnut::Application::GetDevice();
	RT_LOG("[SetScene] enter: textures=%d, envMapIndex=%d, meshes=%d, materials=%d",
	       (int)sceneData.textures.size(), sceneData.envMapIndex,
	       (int)sceneData.meshes.size(), (int)sceneData.materials.size());
	vkDeviceWaitIdle(device);  // Wait for GPU to finish before destroying textures

	m_CurrentScene = sceneData;
	m_NeedsASRebuild = true;
	m_FrameIndex = 1;
	RT_LOG("[SetScene] destroying old textures (count=%d)", (int)m_Textures.size());
	DestroyTextures();
	RT_LOG("[SetScene] creating new textures");
	CreateTextures(m_CurrentScene.textures);
	RT_LOG("[SetScene] destroying old CDF textures");
	DestroyEnvMapCDFTextures();
	RT_LOG("[SetScene] creating new CDF textures");
	CreateEnvMapCDFTextures(m_CurrentScene);
	RT_LOG("[SetScene] CDF indices: marginal=%d conditional=%d envMap=%d",
	       m_MarginalCDFIndex, m_ConditionalCDFIndex, m_EnvMapIndex);

	// Update descriptor set immediately so the new texture views are bound
	// before the next render. But skip if AS needs rebuild — the old TLAS
	// handle will be destroyed during rebuild, and UpdateDescriptorSet will
	// be called again after the rebuild with the new TLAS handle.
	if (m_AS.IsValid() && !m_NeedsASRebuild)
	{
		RT_LOG("[SetScene] updating descriptor set (AS valid, no rebuild needed)");
		UpdateDescriptorSet();
	}
	else
	{
		RT_LOG("[SetScene] skipping descriptor update (AS needs rebuild or not valid)");
	}
	RT_LOG("[SetScene] done");
}

void RendererGPU::RebuildAccelerationStructures()
{
	RT_LOG("[RebuildAS] enter: meshes=%d", (int)m_CurrentScene.meshes.size());
	VkCommandBuffer cmd = Walnut::Application::GetCommandBuffer(true);
	RT_LOG("[RebuildAS] got command buffer");

	// Build one BLAS per mesh geometry
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

		// Check if this mesh's material uses alpha blending/cutout
		uint32_t matIdx = mesh.materialIndex;
		if (matIdx < m_CurrentScene.materials.size())
		{
			float alphaMode = m_CurrentScene.materials[matIdx].alphaMode;
			geo.isTransparent = (alphaMode > 0.5f);
		}

		geometries.push_back(geo);
	}
	RT_LOG("[RebuildAS] built %d geometry entries, calling BuildBLASes", (int)geometries.size());

	bool blasOK = m_AS.BuildBLASes(cmd, geometries);
	RT_LOG("[RebuildAS] BuildBLASes result=%d", blasOK);

	// Barrier: BLAS builds must complete before TLAS build reads them
	VkMemoryBarrier blasBarrier = {};
	blasBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	blasBarrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	blasBarrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;

	vkCmdPipelineBarrier(cmd,
		VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
		VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
		0, 1, &blasBarrier, 0, nullptr, 0, nullptr);

	// Build TLAS instances — one per BLAS, with customIndex = material index
	// sbtHitOffset: 0 = opaque hit group (no any-hit), 1 = alpha hit group (any-hit)
	std::vector<BLASInstance> instances;
	instances.reserve(m_CurrentScene.meshes.size());
	for (size_t i = 0; i < m_CurrentScene.meshes.size(); i++)
	{
		VkTransformMatrixKHR transform = {};
		transform.matrix[0][0] = 1.0f;
		transform.matrix[1][1] = 1.0f;
		transform.matrix[2][2] = 1.0f;

		BLASInstance inst = {};
		inst.blasAddress = m_AS.GetBLASAddress(static_cast<uint32_t>(i));
		inst.customIndex = m_CurrentScene.meshes[i].materialIndex;
		inst.transform = transform;

		// Check if this mesh's material uses alpha blending/cutout
		uint32_t matIdx = m_CurrentScene.meshes[i].materialIndex;
		if (matIdx < m_CurrentScene.materials.size())
		{
			float alphaMode = m_CurrentScene.materials[matIdx].alphaMode;
			inst.sbtHitOffset = (alphaMode > 0.5f) ? 1u : 0u;  // 1 = alpha hit group
		}

		instances.push_back(inst);
	}

	bool tlasOK = m_AS.BuildTLAS(cmd, instances);
	RT_LOG("[RebuildAS] TLAS build result=%d instances=%d", tlasOK, (int)instances.size());

	Walnut::Application::FlushCommandBuffer(cmd);

	RT_LOG("[RebuildAS] creating material buffer");
	CreateMaterialBuffer();
	RT_LOG("[RebuildAS] creating light buffer");
	CreateLightBuffer();
	RT_LOG("[RebuildAS] updating descriptor set");
	UpdateDescriptorSet();

	m_NeedsASRebuild = false;
	m_ASJustBuilt = true;
}

void RendererGPU::UpdateCameraUBO(const Camera& camera)
{
	SICameraData ubo = {};
	ubo.position = glm::vec4(camera.GetPosition(), (float)m_FrameIndex);

	// NRD camera jitter (Halton sequence, subpixel offset in [-0.5, 0.5])
	m_NRDJitterPrev = m_NRDJitter;
	if (m_NRDEnabled)
	{
		// Halton sequence (base 2, base 3) for low-discrepancy jitter
		auto halton = [](int index, int base) -> float {
			float f = 1.0f, r = 0.0f;
			int i = index;
			while (i > 0) {
				f /= base;
				r += f * (i % base);
				i /= base;
			}
			return r;
		};
		int frame = (m_FrameIndex - 1) % 16 + 1; // cycle through 16 offsets
		m_NRDJitter = glm::vec2(halton(frame, 2) - 0.5f, halton(frame, 3) - 0.5f);
	}
	else
	{
		m_NRDJitter = glm::vec2(0.0f);
	}

	ubo.forward = glm::vec4(camera.GetDirection(), m_NRDJitter.x);
	glm::vec3 right = glm::cross(camera.GetDirection(), glm::vec3(0, 1, 0));
	glm::vec3 up = glm::cross(right, camera.GetDirection());
	ubo.right = glm::vec4(right, m_NRDJitter.y);
	ubo.up = glm::vec4(up, 0.0f);
	int maxBouncesClamped = m_MaxBounces;
	const int bounceLimit = (int)m_MaxRecursionDepth - 1;
	if (maxBouncesClamped > bounceLimit) maxBouncesClamped = bounceLimit;
	ubo.viewportSPP = glm::vec4((float)m_Width, (float)m_Height, (float)m_SPP, (float)maxBouncesClamped);
	ubo.apertureFocal = glm::vec4(camera.m_Aperture, camera.m_FocusDistance, m_ShowBackground ? 1.0f : 0.0f, m_EmissiveBoost);
	ubo.envMap = glm::vec4((float)m_EnvMapIndex, m_EnvIntensity, (float)m_MarginalCDFIndex, (float)m_ConditionalCDFIndex);
	ubo.inverseProjection = camera.GetInverseProjection();
	ubo.inverseView = camera.GetInverseView();
	ubo.viewToClip = camera.GetProjection();
	ubo.worldToView = camera.GetView();
	ubo.viewToClipPrev = m_HasPrevMatrices ? m_PrevViewToClip : ubo.viewToClip;
	ubo.worldToViewPrev = m_HasPrevMatrices ? m_PrevWorldToView : ubo.worldToView;

	// Update previous frame matrices for next frame
	m_PrevViewToClip = ubo.viewToClip;
	m_PrevWorldToView = ubo.worldToView;
	m_HasPrevMatrices = true;

	VkDevice device = Walnut::Application::GetDevice();
	void* data;
	vkMapMemory(device, m_CameraUBOMemory, 0, sizeof(SICameraData), 0, &data);
	memcpy(data, &ubo, sizeof(SICameraData));
	vkUnmapMemory(device, m_CameraUBOMemory);
}

void RendererGPU::Render(const Camera& camera)
{
	if (!m_Initialized || m_OutputImage == VK_NULL_HANDLE) return;

	RT_LOG("[Render] frame=%d needsASRebuild=%d", m_FrameIndex, m_NeedsASRebuild);

	if (m_NeedsASRebuild)
	{
		RT_LOG("[Render] rebuilding AS...");
		RebuildAccelerationStructures();
		RT_LOG("[Render] AS rebuild done, AS valid=%d", m_AS.IsValid());
	}

	if (!m_AS.IsValid())
	{
		static bool warned = false;
		if (!warned) { std::cerr << "[RT2] Render: TLAS not valid (no mesh loaded?)\n"; warned = true; }
		return;
	}

	if (const_cast<Camera&>(camera).checkHasMoved())
		m_FrameIndex = 1;

	// Lazy-init NRD when toggled on
	if (m_NRDEnabled && !m_NRD.IsAvailable() && m_Width > 0 && m_Height > 0)
	{
		RT_LOG("[Render] initializing NRD (%ux%u)", m_Width, m_Height);
		m_NRD.Init(Walnut::Application::GetInstance(),
		           Walnut::Application::GetPhysicalDevice(),
		           Walnut::Application::GetDevice(),
		           Walnut::Application::GetQueue(),
		           Walnut::Application::GetQueueFamily(),
		           m_Width, m_Height);
	}

	UpdateCameraUBO(camera);

	if (!m_DescriptorSet || !m_RTPipeline || !m_CameraUBO || !m_MaterialBuffer)
	{
		std::cerr << "[RT2] Render: missing resource (ds=" << m_DescriptorSet
		          << " pipe=" << m_RTPipeline << " ubo=" << m_CameraUBO
		          << " mat=" << m_MaterialBuffer << ")\n";
		return;
	}

	VkCommandBuffer cmd = Walnut::Application::GetCommandBuffer(true);

	// If the AS was just rebuilt this frame, insert a barrier so the
	// traceRayEXT sees the completed TLAS/BLAS writes (host-side flush
	// of the build command buffer is not sufficient for device visibility
	// across command buffers).
	if (m_ASJustBuilt)
	{
		VkMemoryBarrier asBarrier = {};
		asBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		asBarrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
		asBarrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
		vkCmdPipelineBarrier(cmd,
			VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
			VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
			0, 1, &asBarrier, 0, nullptr, 0, nullptr);
		m_ASJustBuilt = false;
	}

	// The output image stays in VK_IMAGE_LAYOUT_GENERAL for its entire lifetime:
	// both the RT storage-image write and the ImGui sampled-image read (whose
	// descriptor was registered with GENERAL in CreateOutputImage) are valid in
	// GENERAL. The previous GENERAL<->SHADER_READ_ONLY dance desynced from the
	// ImGui descriptor and produced validation errors + a black screen.
	VkImageMemoryBarrier rtWriteBarrier = {};
	rtWriteBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	rtWriteBarrier.srcAccessMask = 0;
	rtWriteBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	rtWriteBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	rtWriteBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	rtWriteBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	rtWriteBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	rtWriteBarrier.image = m_OutputImage;
	rtWriteBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	rtWriteBarrier.subresourceRange.levelCount = 1;
	rtWriteBarrier.subresourceRange.layerCount = 1;

	vkCmdPipelineBarrier(cmd,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0, nullptr, 0, nullptr, 1, &rtWriteBarrier);

	// Update NRD UBO
	VkDevice device = Walnut::Application::GetDevice();
	if (m_NRDUBO)
	{
		SINRDUniformData nrdData = { m_NRDEnabled ? 1u : 0u, 0, 0, 0 };
		void* nrdMapped;
		vkMapMemory(device, m_NRDUBOMemory, 0, sizeof(SINRDUniformData), 0, &nrdMapped);
		memcpy(nrdMapped, &nrdData, sizeof(SINRDUniformData));
		vkUnmapMemory(device, m_NRDUBOMemory);
	}

	// Trace rays
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_RTPipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_PipelineLayout, 0, 1, &m_DescriptorSet, 0, nullptr);
	if (m_GBufferSet)
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_PipelineLayout, 1, 1, &m_GBufferSet, 0, nullptr);

	VkDeviceAddress sbtAddress = GetBufferDeviceAddress(m_SBTBuffer);

	// Barrier: ensure host writes to SBT (from CreatePipeline) are visible to
	// the ray tracing pipeline. HOST_COHERENT should handle this, but some
	// drivers need an explicit barrier for the SBT specifically.
	VkBufferMemoryBarrier sbtBarrier = {};
	sbtBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	sbtBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
	sbtBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	sbtBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	sbtBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	sbtBarrier.buffer = m_SBTBuffer;
	sbtBarrier.offset = 0;
	sbtBarrier.size = m_SBTSize;

	vkCmdPipelineBarrier(cmd,
		VK_PIPELINE_STAGE_HOST_BIT,
		VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
		0, 0, nullptr, 1, &sbtBarrier, 0, nullptr);

	VkStridedDeviceAddressRegionKHR rgenRegion = {};
	rgenRegion.deviceAddress = sbtAddress + 0;
	rgenRegion.stride        = m_SBTStride;
	rgenRegion.size          = m_RgenRegionSize;

	VkStridedDeviceAddressRegionKHR missRegion = {};
	missRegion.deviceAddress = sbtAddress + m_RgenRegionSize;
	missRegion.stride        = m_SBTStride;
	missRegion.size          = m_MissRegionSize;

	VkStridedDeviceAddressRegionKHR hitRegion = {};
	hitRegion.deviceAddress = sbtAddress + m_RgenRegionSize + m_MissRegionSize;
	hitRegion.stride        = m_SBTStride;
	hitRegion.size          = m_HitRegionSize;

	VkStridedDeviceAddressRegionKHR callableRegion = {}; // unused

	g_RTDispatch.CmdTraceRaysKHR(cmd,
		&rgenRegion, &missRegion, &hitRegion, &callableRegion,
		m_Width, m_Height, 1);

	if (!g_RTDispatch.CmdTraceRaysKHR)
		std::cerr << "[RT2] ERROR: CmdTraceRaysKHR is NULL!\n";

	// NRD denoising pass
	if (m_NRDEnabled && m_NRD.IsAvailable())
	{
		// Barrier: RT writes to G-buffer images must complete before NRD reads them
		VkImage gbufferImgs[] = {
			m_GNormalRoughness, m_GViewZ, m_GMotion,
			m_GDiffRadiance, m_GSpecRadiance
		};
		for (auto img : gbufferImgs)
		{
			VkImageMemoryBarrier b = {};
			b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
			b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
			b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
			b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.image = img;
			b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			b.subresourceRange.levelCount = 1;
			b.subresourceRange.layerCount = 1;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
			                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
			                     0, nullptr, 0, nullptr, 1, &b);
		}

		m_NRD.NewFrame();

		// Set common settings
		const Camera& cam = camera;
		glm::mat4 viewToClip = cam.GetProjection();
		glm::mat4 worldToView = cam.GetView();
		glm::mat4 prevViewToClip = m_HasPrevMatrices ? m_PrevViewToClip : viewToClip;
		glm::mat4 prevWorldToView = m_HasPrevMatrices ? m_PrevWorldToView : worldToView;

		bool reset = (m_FrameIndex == 1);
		m_NRD.SetCommonSettings(
			glm::value_ptr(viewToClip),
			glm::value_ptr(prevViewToClip),
			glm::value_ptr(worldToView),
			glm::value_ptr(prevWorldToView),
			m_NRDJitter.x, m_NRDJitter.y,
			m_NRDJitterPrev.x, m_NRDJitterPrev.y,
			m_FrameIndex, reset, m_NRDSplitScreen);

		m_NRD.SetReblurSettings(m_NRDMaxBlurRadius, (uint32_t)m_NRDMaxAccumFrames,
		                        m_NRDAntiFirefly, m_NRDSplitScreen);

		m_NRD.Denoise(cmd,
			m_GNormalRoughness, VK_FORMAT_R8G8B8A8_UNORM,
			m_GViewZ, VK_FORMAT_R16_SFLOAT,
			m_GMotion, VK_FORMAT_R16G16_SFLOAT,
			m_GDiffRadiance, VK_FORMAT_R16G16B16A16_SFLOAT,
			m_GSpecRadiance, VK_FORMAT_R16G16B16A16_SFLOAT,
			m_NRDDiffOut, m_NRDSpecOut);

		// NRD with restoreInitialState=true + GENERAL layout restores images
		// to GENERAL after denoising, so no manual barriers needed.

		// Compose pass: remodulate NRD outputs by albedo/F0, write to beauty
		if (m_ComposePipeline && m_ComposeSet)
		{
			// Barrier: NRD outputs (compute writes) → compose (compute reads)
			VkImage composeImgs[] = { m_NRDDiffOut, m_NRDSpecOut, m_GAlbedoF0 };
			for (auto img : composeImgs)
			{
				VkImageMemoryBarrier b = {};
				b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
				b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
				b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
				b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
				b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
				b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				b.image = img;
				b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				b.subresourceRange.levelCount = 1;
				b.subresourceRange.layerCount = 1;
				vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
				                     0, nullptr, 0, nullptr, 1, &b);
			}

			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ComposePipeline);
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ComposePipelineLayout, 0, 1, &m_ComposeSet, 0, nullptr);
			vkCmdDispatch(cmd, (m_Width + 15) / 16, (m_Height + 15) / 16, 1);
		}
	}

	VkImageMemoryBarrier rtReadBarrier = {};
	rtReadBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	rtReadBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	rtReadBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	rtReadBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	rtReadBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	rtReadBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	rtReadBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	rtReadBarrier.image = m_OutputImage;
	rtReadBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	rtReadBarrier.subresourceRange.levelCount = 1;
	rtReadBarrier.subresourceRange.layerCount = 1;

	// If NRD+compose ran, the output image was written by compute (compose).
	// Otherwise it was written by the ray tracing shader.
	VkPipelineStageFlags srcStage = (m_NRDEnabled && m_NRD.IsAvailable() && m_ComposePipeline)
		? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
		: VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;

	vkCmdPipelineBarrier(cmd, srcStage, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &rtReadBarrier);

	Walnut::Application::FlushCommandBuffer(cmd);

	m_FrameIndex++;
}

// ---- Output readback for screenshots ---------------------------------------

bool RendererGPU::ReadbackOutput(std::vector<uint8_t>& outPixelsRGBA8, uint32_t& outWidth, uint32_t& outHeight)
{
	if (!m_Initialized || m_OutputImage == VK_NULL_HANDLE || m_Width == 0 || m_Height == 0)
		return false;

	VkDevice device = Walnut::Application::GetDevice();

	// Create a host-visible staging buffer
	VkBuffer stagingBuffer;
	VkDeviceMemory stagingMemory;
	VkDeviceSize imageSize = (VkDeviceSize)m_Width * m_Height * 16; // R32G32B32A32_SFLOAT = 16 bytes/pixel

	CreateBuffer(imageSize,
	             VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	             stagingBuffer, stagingMemory);

	// Transition output image to TRANSFER_SRC layout, copy to buffer
	VkCommandBuffer cmd = Walnut::Application::GetCommandBuffer(true);

	VkImageMemoryBarrier toTransfer = {};
	toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	toTransfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	toTransfer.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toTransfer.image = m_OutputImage;
	toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	toTransfer.subresourceRange.levelCount = 1;
	toTransfer.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
	                     0, nullptr, 0, nullptr, 1, &toTransfer);

	VkBufferImageCopy region = {};
	region.bufferOffset = 0;
	region.bufferRowLength = 0;
	region.bufferImageHeight = 0;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.mipLevel = 0;
	region.imageSubresource.baseArrayLayer = 0;
	region.imageSubresource.layerCount = 1;
	region.imageOffset = { 0, 0, 0 };
	region.imageExtent = { m_Width, m_Height, 1 };
	vkCmdCopyImageToBuffer(cmd, m_OutputImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stagingBuffer, 1, &region);

	// Transition back to GENERAL
	VkImageMemoryBarrier toGeneral = toTransfer;
	toGeneral.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	toGeneral.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	toGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0,
	                     0, nullptr, 0, nullptr, 1, &toGeneral);

	Walnut::Application::FlushCommandBuffer(cmd);

	// Map and convert R32G32B32A32 float → RGBA8 (tonemap + sRGB)
	void* mapped = nullptr;
	VkResult err = vkMapMemory(device, stagingMemory, 0, imageSize, 0, &mapped);
	if (err != VK_SUCCESS || !mapped)
	{
		fprintf(stderr, "[Readback] vkMapMemory failed: %d\n", err);
		DestroyBuffer(stagingBuffer, stagingMemory);
		return false;
	}

	const float* floatData = static_cast<const float*>(mapped);
	outPixelsRGBA8.resize((size_t)m_Width * m_Height * 4);
	for (size_t i = 0; i < (size_t)m_Width * m_Height; i++)
	{
		float r = floatData[i * 4 + 0];
		float g = floatData[i * 4 + 1];
		float b = floatData[i * 4 + 2];
		float a = floatData[i * 4 + 3];

		// Reinhard tonemap + clamp
		r = r / (1.0f + r);
		g = g / (1.0f + g);
		b = b / (1.0f + b);
		a = (a > 1.0f) ? 1.0f : (a < 0.0f ? 0.0f : a);

		// sRGB encode (gamma 2.2 approx)
		auto toSRGB = [](float c) -> uint8_t {
			if (c <= 0.0031308f) return (uint8_t)(c * 12.92f * 255.0f + 0.5f);
			return (uint8_t)((1.055f * powf(c, 1.0f / 2.4f) - 0.055f) * 255.0f + 0.5f);
		};

		outPixelsRGBA8[i * 4 + 0] = toSRGB(r);
		outPixelsRGBA8[i * 4 + 1] = toSRGB(g);
		outPixelsRGBA8[i * 4 + 2] = toSRGB(b);
		outPixelsRGBA8[i * 4 + 3] = (uint8_t)(a * 255.0f + 0.5f);
	}

	vkUnmapMemory(device, stagingMemory);
	DestroyBuffer(stagingBuffer, stagingMemory);

	outWidth = m_Width;
	outHeight = m_Height;
	RT_LOG("[Readback] captured %ux%u → %zu bytes", m_Width, m_Height, outPixelsRGBA8.size());
	return true;
}

// ---- NRD G-buffer images (set 1) -------------------------------------------

void RendererGPU::CreateGBufferImages()
{
	DestroyGBufferImages();

	VkDevice device = Walnut::Application::GetDevice();

	struct GBufferImageSpec
	{
		VkFormat format;
		VkImage& image;
		VkDeviceMemory& mem;
		VkImageView& view;
	};

	GBufferImageSpec specs[] = {
		{ VK_FORMAT_R8G8B8A8_UNORM,     m_GNormalRoughness, m_GNormalRoughnessMem, m_GNormalRoughnessView },
		{ VK_FORMAT_R16_SFLOAT,          m_GViewZ,           m_GViewZMem,           m_GViewZView },
		{ VK_FORMAT_R16G16_SFLOAT,       m_GMotion,          m_GMotionMem,          m_GMotionView },
		{ VK_FORMAT_R16G16B16A16_SFLOAT, m_GDiffRadiance,    m_GDiffRadianceMem,    m_GDiffRadianceView },
		{ VK_FORMAT_R16G16B16A16_SFLOAT, m_GSpecRadiance,    m_GSpecRadianceMem,    m_GSpecRadianceView },
		{ VK_FORMAT_R16G16B16A16_SFLOAT, m_GAlbedoF0,        m_GAlbedoF0Mem,        m_GAlbedoF0View },
		{ VK_FORMAT_R16G16B16A16_SFLOAT, m_GDirectEmission,  m_GDirectEmissionMem,  m_GDirectEmissionView },
		{ VK_FORMAT_R16G16B16A16_SFLOAT, m_NRDDiffOut,       m_NRDDiffOutMem,       m_NRDDiffOutView },
		{ VK_FORMAT_R16G16B16A16_SFLOAT, m_NRDSpecOut,       m_NRDSpecOutMem,       m_NRDSpecOutView },
	};

	VkCommandBuffer cmd = Walnut::Application::GetCommandBuffer(true);

	for (auto& s : specs)
	{
		VkImageCreateInfo imageInfo = {};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.format = s.format;
		imageInfo.extent.width = m_Width;
		imageInfo.extent.height = m_Height;
		imageInfo.extent.depth = 1;
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = 1;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		vkCreateImage(device, &imageInfo, nullptr, &s.image);

		VkMemoryRequirements memReq;
		vkGetImageMemoryRequirements(device, s.image, &memReq);

		VkMemoryAllocateInfo allocInfo = {};
		allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocInfo.allocationSize = memReq.size;
		allocInfo.memoryTypeIndex = FindMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

		vkAllocateMemory(device, &allocInfo, nullptr, &s.mem);
		vkBindImageMemory(device, s.image, s.mem, 0);

		VkImageViewCreateInfo viewInfo = {};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = s.image;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = s.format;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.layerCount = 1;

		vkCreateImageView(device, &viewInfo, nullptr, &s.view);

		// Transition to GENERAL layout
		VkImageMemoryBarrier barrier = {};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = s.image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.layerCount = 1;

		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	}

	Walnut::Application::FlushCommandBuffer(cmd);
}

void RendererGPU::DestroyGBufferImages()
{
	VkDevice device = Walnut::Application::GetDevice();

	struct ImgPair { VkImage& img; VkDeviceMemory& mem; VkImageView& view; };
	ImgPair pairs[] = {
		{ m_GNormalRoughness, m_GNormalRoughnessMem, m_GNormalRoughnessView },
		{ m_GViewZ,           m_GViewZMem,           m_GViewZView },
		{ m_GMotion,          m_GMotionMem,          m_GMotionView },
		{ m_GDiffRadiance,    m_GDiffRadianceMem,    m_GDiffRadianceView },
		{ m_GSpecRadiance,    m_GSpecRadianceMem,    m_GSpecRadianceView },
		{ m_GAlbedoF0,        m_GAlbedoF0Mem,        m_GAlbedoF0View },
		{ m_GDirectEmission,  m_GDirectEmissionMem,  m_GDirectEmissionView },
		{ m_NRDDiffOut,       m_NRDDiffOutMem,       m_NRDDiffOutView },
		{ m_NRDSpecOut,       m_NRDSpecOutMem,       m_NRDSpecOutView },
	};

	for (auto& p : pairs)
	{
		if (p.view) { vkDestroyImageView(device, p.view, nullptr); p.view = VK_NULL_HANDLE; }
		if (p.img)  { vkDestroyImage(device, p.img, nullptr);      p.img  = VK_NULL_HANDLE; }
		if (p.mem)  { vkFreeMemory(device, p.mem, nullptr);        p.mem  = VK_NULL_HANDLE; }
	}
}

void RendererGPU::CreateGBufferDescriptorSet()
{
	VkDevice device = Walnut::Application::GetDevice();

	// Set 1 layout: 7 storage images (0-5, 7) + 1 UBO (6)
	VkDescriptorSetLayoutBinding bindings[8] = {};
	// Bindings 0-5: storage images (gNormalRoughness, gViewZ, gMotion, gDiff, gSpec, gAlbedoF0)
	for (int i = 0; i < 6; i++)
	{
		bindings[i].binding = i;
		bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
	}
	// Binding 6: UBO (nrdData)
	bindings[6].binding = 6;
	bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	bindings[6].descriptorCount = 1;
	bindings[6].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
	// Binding 7: gDirectEmission storage image
	bindings[7].binding = 7;
	bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	bindings[7].descriptorCount = 1;
	bindings[7].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;

	VkDescriptorSetLayoutCreateInfo layoutInfo = {};
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = 8;
	layoutInfo.pBindings = bindings;

	vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_GBufferSetLayout);

	// Create dedicated pool
	VkDescriptorPoolSize poolSizes[2] = {};
	poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	poolSizes[0].descriptorCount = 7;
	poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	poolSizes[1].descriptorCount = 1;

	VkDescriptorPoolCreateInfo poolInfo = {};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets = 1;
	poolInfo.poolSizeCount = 2;
	poolInfo.pPoolSizes = poolSizes;

	vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_GBufferPool);

	VkDescriptorSetAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.descriptorPool = m_GBufferPool;
	allocInfo.descriptorSetCount = 1;
	allocInfo.pSetLayouts = &m_GBufferSetLayout;

	vkAllocateDescriptorSets(device, &allocInfo, &m_GBufferSet);

	// Create NRD UBO
	if (!m_NRDUBO)
	{
		CreateBuffer(16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
		             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		             m_NRDUBO, m_NRDUBOMemory);
	}
}

void RendererGPU::UpdateGBufferDescriptorSet()
{
	VkDevice device = Walnut::Application::GetDevice();

	VkDescriptorImageInfo imageInfos[7] = {};
	VkImageView views[] = { m_GNormalRoughnessView, m_GViewZView, m_GMotionView, m_GDiffRadianceView, m_GSpecRadianceView, m_GAlbedoF0View, m_GDirectEmissionView };
	for (int i = 0; i < 7; i++)
	{
		imageInfos[i].imageView = views[i];
		imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	}

	VkDescriptorBufferInfo uboInfo = {};
	uboInfo.buffer = m_NRDUBO;
	uboInfo.range = VK_WHOLE_SIZE;

	VkWriteDescriptorSet writes[8] = {};
	// Storage images: bindings 0-5 and 7
	for (int i = 0; i < 6; i++)
	{
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = m_GBufferSet;
		writes[i].dstBinding = i;
		writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		writes[i].descriptorCount = 1;
		writes[i].pImageInfo = &imageInfos[i];
	}
	// UBO: binding 6
	writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[6].dstSet = m_GBufferSet;
	writes[6].dstBinding = 6;
	writes[6].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	writes[6].descriptorCount = 1;
	writes[6].pBufferInfo = &uboInfo;
	// Direct emission: binding 7
	writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[7].dstSet = m_GBufferSet;
	writes[7].dstBinding = 7;
	writes[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	writes[7].descriptorCount = 1;
	writes[7].pImageInfo = &imageInfos[6];

	vkUpdateDescriptorSets(device, 8, writes, 0, nullptr);
}

// ---- Compose pass (compute shader) ------------------------------------------

void RendererGPU::CreateComposePipeline()
{
	VkDevice device = Walnut::Application::GetDevice();

	m_ComposeShader = ShaderManager::LoadShader("compose.spv");
	if (!m_ComposeShader)
		m_ComposeShader = ShaderManager::LoadShader("RT2App/shaders/compose.spv");
	if (!m_ComposeShader)
	{
		std::cerr << "[RT2] Failed to load compose shader\n";
		return;
	}

	// Descriptor set layout: 5 storage images (output, nrdDiff, nrdSpec, albedoF0, directEmission)
	VkDescriptorSetLayoutBinding bindings[5] = {};
	for (int i = 0; i < 5; i++)
	{
		bindings[i].binding = i;
		bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}

	VkDescriptorSetLayoutCreateInfo layoutInfo = {};
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = 5;
	layoutInfo.pBindings = bindings;
	vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_ComposeSetLayout);

	// Pipeline layout
	VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = 1;
	pipelineLayoutInfo.pSetLayouts = &m_ComposeSetLayout;
	vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &m_ComposePipelineLayout);

	// Compute pipeline
	VkComputePipelineCreateInfo pipelineInfo = {};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	pipelineInfo.stage.module = m_ComposeShader;
	pipelineInfo.stage.pName = "main";
	pipelineInfo.layout = m_ComposePipelineLayout;

	vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_ComposePipeline);
}

void RendererGPU::DestroyComposePipeline()
{
	VkDevice device = Walnut::Application::GetDevice();
	if (m_ComposePipeline) { vkDestroyPipeline(device, m_ComposePipeline, nullptr); m_ComposePipeline = VK_NULL_HANDLE; }
	if (m_ComposePipelineLayout) { vkDestroyPipelineLayout(device, m_ComposePipelineLayout, nullptr); m_ComposePipelineLayout = VK_NULL_HANDLE; }
	if (m_ComposeSetLayout) { vkDestroyDescriptorSetLayout(device, m_ComposeSetLayout, nullptr); m_ComposeSetLayout = VK_NULL_HANDLE; }
	if (m_ComposePool) { vkDestroyDescriptorPool(device, m_ComposePool, nullptr); m_ComposePool = VK_NULL_HANDLE; }
	if (m_ComposeShader) { vkDestroyShaderModule(device, m_ComposeShader, nullptr); m_ComposeShader = VK_NULL_HANDLE; }
	m_ComposeSet = VK_NULL_HANDLE;
}

void RendererGPU::CreateComposeDescriptorSet()
{
	VkDevice device = Walnut::Application::GetDevice();

	VkDescriptorPoolSize poolSize = {};
	poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	poolSize.descriptorCount = 5;

	VkDescriptorPoolCreateInfo poolInfo = {};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets = 1;
	poolInfo.poolSizeCount = 1;
	poolInfo.pPoolSizes = &poolSize;
	vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_ComposePool);

	VkDescriptorSetAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.descriptorPool = m_ComposePool;
	allocInfo.descriptorSetCount = 1;
	allocInfo.pSetLayouts = &m_ComposeSetLayout;
	vkAllocateDescriptorSets(device, &allocInfo, &m_ComposeSet);
}

void RendererGPU::UpdateComposeDescriptorSet()
{
	VkDevice device = Walnut::Application::GetDevice();

	VkDescriptorImageInfo imageInfos[5] = {};
	VkImageView views[] = { m_OutputImageView, m_NRDDiffOutView, m_NRDSpecOutView, m_GAlbedoF0View, m_GDirectEmissionView };
	for (int i = 0; i < 5; i++)
	{
		imageInfos[i].imageView = views[i];
		imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	}

	VkWriteDescriptorSet writes[5] = {};
	for (int i = 0; i < 5; i++)
	{
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = m_ComposeSet;
		writes[i].dstBinding = i;
		writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		writes[i].descriptorCount = 1;
		writes[i].pImageInfo = &imageInfos[i];
	}

	vkUpdateDescriptorSets(device, 5, writes, 0, nullptr);
}