#include "RendererGPU.h"
#include "ShaderManager.h"
#include "Walnut/Application.h"
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

bool RendererGPU::Init()
{
	if (m_Initialized) return true;

	if (!Walnut::Application::IsRayTracingSupported())
	{
		std::cerr << "[RT2] Ray tracing not supported, GPU renderer unavailable.\n";
		return false;
	}

	CreatePipeline();
	if (m_ComputePipeline == VK_NULL_HANDLE)
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
	DestroyBuffer(m_CameraUBO, m_CameraUBOMemory);
	DestroyBuffer(m_MaterialBuffer, m_MaterialBufferMemory);
	m_AS.Destroy();

	m_Initialized = false;
}

void RendererGPU::CreatePipeline()
{
	VkDevice device = Walnut::Application::GetDevice();

	m_ShaderModule = ShaderManager::LoadShader("pathtracer.spv");
	if (!m_ShaderModule)
	{
		m_ShaderModule = ShaderManager::LoadShader("RT2App/shaders/pathtracer.spv");
	}
	if (!m_ShaderModule)
	{
		std::cerr << "[RT2] Failed to load path tracer shader\n";
		return;
	}

	// Descriptor set layout
	VkDescriptorSetLayoutBinding bindings[5] = {};

	bindings[0] = {};
	bindings[0].binding = 0;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	bindings[0].descriptorCount = 1;
	bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

	bindings[1] = {};
	bindings[1].binding = 1;
	bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	bindings[1].descriptorCount = 1;
	bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

	bindings[2] = {};
	bindings[2].binding = 2;
	bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[2].descriptorCount = 1;
	bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

	bindings[3] = {};
	bindings[3].binding = 3;
	bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[3].descriptorCount = 1;
	bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

	bindings[4] = {};
	bindings[4].binding = 4;
	bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
	bindings[4].descriptorCount = 1;
	bindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

	VkDescriptorSetLayoutCreateInfo layoutInfo = {};
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = 5;
	layoutInfo.pBindings = bindings;

	vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_DescriptorSetLayout);

	// Pipeline layout
	VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = 1;
	pipelineLayoutInfo.pSetLayouts = &m_DescriptorSetLayout;

	vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &m_PipelineLayout);

	// Compute pipeline
	VkComputePipelineCreateInfo pipelineInfo = {};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipelineInfo.layout = m_PipelineLayout;
	pipelineInfo.stage = {};
	pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	pipelineInfo.stage.module = m_ShaderModule;
	pipelineInfo.stage.pName = "main";

	vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_ComputePipeline);
}

void RendererGPU::DestroyPipeline()
{
	VkDevice device = Walnut::Application::GetDevice();
	if (m_ComputePipeline) vkDestroyPipeline(device, m_ComputePipeline, nullptr);
	if (m_PipelineLayout) vkDestroyPipelineLayout(device, m_PipelineLayout, nullptr);
	if (m_DescriptorSetLayout) vkDestroyDescriptorSetLayout(device, m_DescriptorSetLayout, nullptr);
	if (m_ShaderModule) vkDestroyShaderModule(device, m_ShaderModule, nullptr);
	m_ComputePipeline = VK_NULL_HANDLE;
	m_PipelineLayout = VK_NULL_HANDLE;
	m_DescriptorSetLayout = VK_NULL_HANDLE;
	m_ShaderModule = VK_NULL_HANDLE;
}

void RendererGPU::CreateOutputImage()
{
	std::cerr << "[RT2] Creating output image (" << m_Width << "x" << m_Height << ")\n";
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

	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

	Walnut::Application::FlushCommandBuffer(cmd);

	// Create ImGui descriptor set for display
	m_ImGuiDescriptorSet = (VkDescriptorSet)ImGui_ImplVulkan_AddTexture(m_Sampler, m_OutputImageView, VK_IMAGE_LAYOUT_GENERAL);
}

void RendererGPU::DestroyOutputImage()
{
	VkDevice device = Walnut::Application::GetDevice();
	if (m_Sampler) vkDestroySampler(device, m_Sampler, nullptr);
	if (m_OutputImageView) vkDestroyImageView(device, m_OutputImageView, nullptr);
	if (m_OutputImage) vkDestroyImage(device, m_OutputImage, nullptr);
	if (m_OutputMemory) vkFreeMemory(device, m_OutputMemory, nullptr);
	m_Sampler = VK_NULL_HANDLE;
	m_OutputImageView = VK_NULL_HANDLE;
	m_OutputImage = VK_NULL_HANDLE;
	m_OutputMemory = VK_NULL_HANDLE;
}

void RendererGPU::OnResize(uint32_t width, uint32_t height)
{
	if (m_Width == width && m_Height == height && m_OutputImage != VK_NULL_HANDLE)
		return;

	std::cerr << "[RT2] GPU OnResize: " << width << "x" << height << "\n";
	VkDevice device = Walnut::Application::GetDevice();
	vkDeviceWaitIdle(device);

	DestroyOutputImage();

	m_Width = width;
	m_Height = height;

	if (width > 0 && height > 0)
	{
		CreateOutputImage();
		CreateDescriptorSet();
		UpdateDescriptorSet();
	}

	m_FrameIndex = 1;
}

void RendererGPU::CreateDescriptorSet()
{
	VkDevice device = Walnut::Application::GetDevice();

	VkDescriptorSetAllocateInfo allocInfo = {};
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.descriptorPool = Walnut::Application::GetDescriptorPool();
	allocInfo.descriptorSetCount = 1;
	allocInfo.pSetLayouts = &m_DescriptorSetLayout;

	vkAllocateDescriptorSets(device, &allocInfo, &m_DescriptorSet);
}

void RendererGPU::UpdateDescriptorSet()
{
	if (!m_AS.IsValid()) return;
	if (!m_DescriptorSet) return;

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
		std::cerr << "[RT2] Warning: material buffer not created yet in UpdateDescriptorSet\n";
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

	VkWriteDescriptorSetAccelerationStructureKHR asInfo = {};
	asInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
	asInfo.accelerationStructureCount = 1;
	VkAccelerationStructureKHR tlas = m_AS.GetTLAS();
	asInfo.pAccelerationStructures = &tlas;

	VkWriteDescriptorSet writes[5] = {};

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

	vkUpdateDescriptorSets(device, 5, writes, 0, nullptr);
}

void RendererGPU::CreateMaterialBuffer()
{
	// Single material for now
	struct GPUMaterial
	{
		glm::vec3 albedo;
		int type;
		float fuzz;
		float ior;
		float pad0;
		float pad1;
	};

	GPUMaterial mat;
	mat.albedo = m_CurrentMesh.albedo;
	mat.type = m_CurrentMesh.materialType;
	mat.fuzz = m_CurrentMesh.fuzz;
	mat.ior = m_CurrentMesh.ior;

	DestroyBuffer(m_MaterialBuffer, m_MaterialBufferMemory);
	CreateBuffer(sizeof(GPUMaterial),
	             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
	             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	             m_MaterialBuffer, m_MaterialBufferMemory);

	VkDevice device = Walnut::Application::GetDevice();
	void* data;
	vkMapMemory(device, m_MaterialBufferMemory, 0, sizeof(GPUMaterial), 0, &data);
	memcpy(data, &mat, sizeof(GPUMaterial));
	vkUnmapMemory(device, m_MaterialBufferMemory);
}

void RendererGPU::ResetAccumulation()
{
	m_FrameIndex = 1;
}

void RendererGPU::SetMesh(const GPUMeshData& meshData)
{
	m_CurrentMesh = meshData;
	m_NeedsASRebuild = true;
	m_FrameIndex = 1;
}

void RendererGPU::RebuildAccelerationStructures()
{
	std::cerr << "[RT2] Building acceleration structures...\n";

	VkCommandBuffer cmd = Walnut::Application::GetCommandBuffer(true);

	m_AS.BuildBLAS(cmd, m_CurrentMesh.vertices, m_CurrentMesh.indices, 0);

	// Barrier: BLAS build must complete before TLAS build reads it
	VkMemoryBarrier blasBarrier = {};
	blasBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	blasBarrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	blasBarrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;

	vkCmdPipelineBarrier(cmd,
		VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
		VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
		0, 1, &blasBarrier, 0, nullptr, 0, nullptr);

	VkTransformMatrixKHR transform = {};
	transform.matrix[0][0] = 1.0f;
	transform.matrix[1][1] = 1.0f;
	transform.matrix[2][2] = 1.0f;
	// Translation in column 3: matrix[row][3]
	transform.matrix[0][3] = 0.0f;
	transform.matrix[1][3] = 0.0f;
	transform.matrix[2][3] = 0.0f;

	VkDeviceAddress blasAddress;
	{
		VkAccelerationStructureDeviceAddressInfoKHR addressInfo = {};
		addressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
		addressInfo.accelerationStructure = m_AS.GetBLAS();

		PFN_vkGetAccelerationStructureDeviceAddressKHR pvkGetAccelerationStructureDeviceAddressKHR =
			(PFN_vkGetAccelerationStructureDeviceAddressKHR)vkGetInstanceProcAddr(
				Walnut::Application::GetInstance(), "vkGetAccelerationStructureDeviceAddressKHR");

		blasAddress = pvkGetAccelerationStructureDeviceAddressKHR(Walnut::Application::GetDevice(), &addressInfo);
	}

	BLASInstance instance = {};
	instance.blasAddress = blasAddress;
	instance.customIndex = 0; // material index
	instance.transform = transform;

	std::vector<BLASInstance> instances = { instance };
	m_AS.BuildTLAS(cmd, instances);

	Walnut::Application::FlushCommandBuffer(cmd);

	CreateMaterialBuffer();
	UpdateDescriptorSet();

	m_NeedsASRebuild = false;
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
		glm::vec4 apertureFocal;// x = aperture, y = focusDistance, z = pad, w = pad
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
	ubo.apertureFocal = glm::vec4(camera.m_Aperture, camera.m_FocusDistance, 0.0f, 0.0f);
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

	if (!m_AS.IsValid()) return;

	if (const_cast<Camera&>(camera).checkHasMoved())
		m_FrameIndex = 1;

	UpdateCameraUBO(camera);

	if (!m_DescriptorSet || !m_ComputePipeline || !m_CameraUBO || !m_MaterialBuffer)
		return;

	VkCommandBuffer cmd = Walnut::Application::GetCommandBuffer(true);

	// Transition to general layout for compute write
	// First render: image is in GENERAL (from CreateOutputImage), not SHADER_READ_ONLY
	VkImageLayout oldLayout = (m_FrameIndex == 1) ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	VkImageMemoryBarrier toGeneral = {};
	toGeneral.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	toGeneral.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	toGeneral.oldLayout = oldLayout;
	toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toGeneral.image = m_OutputImage;
	toGeneral.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	toGeneral.subresourceRange.levelCount = 1;
	toGeneral.subresourceRange.layerCount = 1;

	vkCmdPipelineBarrier(cmd,
		(m_FrameIndex == 1) ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toGeneral);

	// Dispatch compute
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ComputePipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_PipelineLayout, 0, 1, &m_DescriptorSet, 0, nullptr);

	uint32_t groupX = (m_Width + 7) / 8;
	uint32_t groupY = (m_Height + 7) / 8;
	vkCmdDispatch(cmd, groupX, groupY, 1);

	// Transition back to shader read for ImGui display
	VkImageMemoryBarrier toRead = {};
	toRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	toRead.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	toRead.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toRead.image = m_OutputImage;
	toRead.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	toRead.subresourceRange.levelCount = 1;
	toRead.subresourceRange.layerCount = 1;

	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toRead);

	Walnut::Application::FlushCommandBuffer(cmd);

	m_FrameIndex++;
}