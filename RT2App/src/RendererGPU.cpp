#include "RendererGPU.h"
#include "ShaderManager.h"
#include "Walnut/Application.h"
#include "Walnut/RTDispatch.h"
#include "backends/imgui_impl_vulkan.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
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
	return 0;
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

	vkCreateBuffer(device, &bufferInfo, nullptr, &buffer);

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

	vkAllocateMemory(device, &allocInfo, nullptr, &memory);
	vkBindBufferMemory(device, buffer, memory, 0);
}

void RendererGPU::DestroyBuffer(VkBuffer buffer, VkDeviceMemory memory)
{
	VkDevice device = Walnut::Application::GetDevice();
	if (buffer) vkDestroyBuffer(device, buffer, nullptr);
	if (memory) vkFreeMemory(device, memory, nullptr);
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
	m_Initialized = true;
	return true;
}

void RendererGPU::Destroy()
{
	VkDevice device = Walnut::Application::GetDevice();
	vkDeviceWaitIdle(device);

	DestroyPipeline();
	DestroyOutputImage();
	DestroyTextures();
	DestroyBuffer(m_CameraUBO, m_CameraUBOMemory);
	DestroyBuffer(m_MaterialBuffer, m_MaterialBufferMemory);
	DestroyBuffer(m_LightBuffer, m_LightBufferMemory);
	m_AS.Destroy();

	m_Initialized = false;
}

void RendererGPU::CreatePipeline()
{
	VkDevice device = Walnut::Application::GetDevice();

	// Load the four RT shader modules.
	m_RgenShader   = ShaderManager::LoadShader("raygen.spv");
	if (!m_RgenShader)   m_RgenShader   = ShaderManager::LoadShader("RT2App/shaders/raygen.spv");
	m_MissShader    = ShaderManager::LoadShader("miss.spv");
	if (!m_MissShader)    m_MissShader    = ShaderManager::LoadShader("RT2App/shaders/miss.spv");
	m_ShadowShader  = ShaderManager::LoadShader("shadow.spv");
	if (!m_ShadowShader)  m_ShadowShader  = ShaderManager::LoadShader("RT2App/shaders/shadow.spv");
	m_ClosestShader = ShaderManager::LoadShader("closesthit.spv");
	if (!m_ClosestShader) m_ClosestShader = ShaderManager::LoadShader("RT2App/shaders/closesthit.spv");

	if (!m_RgenShader || !m_MissShader || !m_ShadowShader || !m_ClosestShader)
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
	                                      VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;

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
	VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = 1;
	pipelineLayoutInfo.pSetLayouts = &m_DescriptorSetLayout;

	vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &m_PipelineLayout);

	// Ray tracing pipeline
	const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& rtProps =
		Walnut::Application::GetRayTracingPipelineProperties();

	VkPipelineShaderStageCreateInfo stages[4] = {};
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

	VkRayTracingShaderGroupCreateInfoKHR groups[4] = {};
	// 0: raygen
	groups[0].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
	groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
	groups[0].generalShader = 0;            // index into stages[]
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
	// 3: hit (triangles) — closest hit only, no any-hit
	groups[3].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
	groups[3].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
	groups[3].generalShader = VK_SHADER_UNUSED_KHR;
	groups[3].closestHitShader = 3;
	groups[3].anyHitShader = VK_SHADER_UNUSED_KHR;
	groups[3].intersectionShader = VK_SHADER_UNUSED_KHR;

	// Recursion depth: set to device maximum so the slider can change
	// maxBounces without rebuilding the pipeline. The shader's
	// depth >= maxBounces check prevents infinite recursion.
	m_MaxRecursionDepth = rtProps.maxRayRecursionDepth;
	if (m_MaxRecursionDepth < 2) m_MaxRecursionDepth = 2;

	VkRayTracingPipelineCreateInfoKHR pipelineInfo = {};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
	pipelineInfo.stageCount = 4;
	pipelineInfo.pStages = stages;
	pipelineInfo.groupCount = 4;
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
	// Layout: [rgen][miss_sky][miss_shadow][hit]
	// Each handle padded to baseAlignment. The miss region covers both
	// miss entries (sky + shadow), so its size = 2 × baseAlign.
	const uint32_t handleSize  = rtProps.shaderGroupHandleSize;
	const uint32_t baseAlign   = std::max<uint32_t>(rtProps.shaderGroupBaseAlignment, 1);
	const uint32_t handleAlign = std::max<uint32_t>(rtProps.shaderGroupHandleAlignment, 1);

	m_SBTStride       = baseAlign; // each region's stride (must be >= handleSize, aligned to base)
	m_RgenRegionSize  = baseAlign;
	m_MissRegionSize  = baseAlign * 2;  // 2 miss entries: sky + shadow
	m_HitRegionSize   = baseAlign;

	VkDeviceSize sbtSize = m_RgenRegionSize + m_MissRegionSize + m_HitRegionSize;

	CreateBuffer(sbtSize,
	             VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
	             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
	             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	             m_SBTBuffer, m_SBTMemory);
	m_SBTSize = sbtSize;

	// Fetch all four group handles in one call.
	std::vector<uint8_t> handles(4 * handleSize);
	err = g_RTDispatch.GetRayTracingShaderGroupHandlesKHR(
		device, m_RTPipeline, 0, 4, handles.size(), handles.data());
	if (err != VK_SUCCESS)
	{
		std::cerr << "[RT2] vkGetRayTracingShaderGroupHandlesKHR failed: " << err << "\n";
		return;
	}

	// Copy each handle into the SBT buffer at the right baseAlignment-aligned offset.
	void* mapped = nullptr;
	vkMapMemory(device, m_SBTMemory, 0, sbtSize, 0, &mapped);
	std::memset(mapped, 0, (size_t)sbtSize);
	uint8_t* dst = static_cast<uint8_t*>(mapped);
	std::memcpy(dst + 0,                       handles.data() + 0 * handleSize, handleSize); // rgen
	std::memcpy(dst + m_RgenRegionSize,        handles.data() + 1 * handleSize, handleSize); // miss_sky
	std::memcpy(dst + m_RgenRegionSize + baseAlign,
	            handles.data() + 2 * handleSize, handleSize);                                 // miss_shadow
	std::memcpy(dst + m_RgenRegionSize + m_MissRegionSize,
	            handles.data() + 3 * handleSize, handleSize);                                 // hit
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
	DestroyBuffer(m_SBTBuffer, m_SBTMemory);
	m_RTPipeline = VK_NULL_HANDLE;
	m_PipelineLayout = VK_NULL_HANDLE;
	m_DescriptorSetLayout = VK_NULL_HANDLE;
	m_RgenShader = m_MissShader = m_ShadowShader = m_ClosestShader = VK_NULL_HANDLE;
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
	CreateDescriptorSet();
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
	if (!m_AS.IsValid()) return;
	if (!m_DescriptorSet) return;
	if (!m_AS.GetNormalBuffer()) return;
	if (!m_MaterialBuffer) return;

	VkDevice device = Walnut::Application::GetDevice();

	// Create camera UBO if needed
	if (!m_CameraUBO)
	{
		CreateBuffer(256,
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

	// Texture array image infos (binding 10) — skip textures with null handles
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
	vkUpdateDescriptorSets(device, writeCount, writes, 0, nullptr);
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
		if (tex.pixels.empty() || tex.width <= 0 || tex.height <= 0)
		{
			std::cerr << "[RT2] Texture " << i << ": no pixel data, skipping\n";
			continue;
		}

		GPUTexture& gt = m_Textures[i];
		gt.width = tex.width;
		gt.height = tex.height;

		VkDeviceSize imageSize = (VkDeviceSize)(tex.width * tex.height * 4);

		// Staging buffer
		VkBuffer stagingBuffer;
		VkDeviceMemory stagingMemory;
		CreateBuffer(imageSize,
		             VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		             stagingBuffer, stagingMemory);

		void* data;
		vkMapMemory(device, stagingMemory, 0, imageSize, 0, &data);
		memcpy(data, tex.pixels.data(), (size_t)imageSize);
		vkUnmapMemory(device, stagingMemory);

		// Create VkImage
		VkImageCreateInfo imageInfo = {};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
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
		viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
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

void RendererGPU::ResetAccumulation()
{
	m_FrameIndex = 1;
}

void RendererGPU::SetScene(const GPUSceneData& sceneData)
{
	m_CurrentScene = sceneData;
	m_NeedsASRebuild = true;
	m_FrameIndex = 1;
	DestroyTextures();
	CreateTextures(m_CurrentScene.textures);
}

void RendererGPU::RebuildAccelerationStructures()
{
	VkCommandBuffer cmd = Walnut::Application::GetCommandBuffer(true);

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
		geometries.push_back(geo);
	}

	bool blasOK = m_AS.BuildBLASes(cmd, geometries);

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
		instances.push_back(inst);
	}

	bool tlasOK = m_AS.BuildTLAS(cmd, instances);

	Walnut::Application::FlushCommandBuffer(cmd);

	CreateMaterialBuffer();
	CreateLightBuffer();
	UpdateDescriptorSet();

	m_NeedsASRebuild = false;
	m_ASJustBuilt = true;
}

void RendererGPU::UpdateCameraUBO(const Camera& camera)
{
	struct CameraUBO
	{
		glm::vec4 position;     // xyz = position, w = frameIndex
		glm::vec4 forward;      // xyz = forward, w = pad
		glm::vec4 right;        // xyz = right, w = pad
		glm::vec4 up;           // xyz = up, w = pad
		glm::vec4 viewportSPP;  // x = width, y = height, z = spp, w = maxBounces
		glm::vec4 apertureFocal;// x = aperture, y = focusDistance, z = showBackground, w = pad
		glm::mat4 inverseProjection;
		glm::mat4 inverseView;
	};

	CameraUBO ubo = {};
	ubo.position = glm::vec4(camera.GetPosition(), (float)m_FrameIndex);
	ubo.forward = glm::vec4(camera.GetDirection(), 0.0f);
	glm::vec3 right = glm::cross(camera.GetDirection(), glm::vec3(0, 1, 0));
	glm::vec3 up = glm::cross(right, camera.GetDirection());
	ubo.right = glm::vec4(right, 0.0f);
	ubo.up = glm::vec4(up, 0.0f);
	ubo.viewportSPP = glm::vec4((float)m_Width, (float)m_Height, (float)m_SPP, (float)m_MaxBounces);
	ubo.apertureFocal = glm::vec4(camera.m_Aperture, camera.m_FocusDistance, m_ShowBackground ? 1.0f : 0.0f, m_NEEOnly ? 1.0f : 0.0f);
	ubo.inverseProjection = camera.GetInverseProjection();
	ubo.inverseView = camera.GetInverseView();

	VkDevice device = Walnut::Application::GetDevice();
	void* data;
	vkMapMemory(device, m_CameraUBOMemory, 0, sizeof(CameraUBO), 0, &data);
	memcpy(data, &ubo, sizeof(CameraUBO));
	vkUnmapMemory(device, m_CameraUBOMemory);
}

void RendererGPU::Render(const Camera& camera)
{
	if (!m_Initialized || m_OutputImage == VK_NULL_HANDLE) return;

	if (m_NeedsASRebuild)
		RebuildAccelerationStructures();

	if (!m_AS.IsValid())
	{
		static bool warned = false;
		if (!warned) { std::cerr << "[RT2] Render: TLAS not valid (no mesh loaded?)\n"; warned = true; }
		return;
	}

	if (const_cast<Camera&>(camera).checkHasMoved())
		m_FrameIndex = 1;

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

	// Trace rays
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_RTPipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_PipelineLayout, 0, 1, &m_DescriptorSet, 0, nullptr);

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

	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &rtReadBarrier);

	Walnut::Application::FlushCommandBuffer(cmd);

	m_FrameIndex++;
}