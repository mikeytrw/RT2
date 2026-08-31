#include "RasterPass.h"
#include "RTLog.h"
#include "VulkanUtils.h"
#include "GPUSceneData.h"
#include "GpuResources.h"
#include "CommandUtils.h"
#include "ShaderManager.h"
#include "Walnut/RTDispatch.h"
#include <cstring>
#include <vector>

bool RasterPass::Init(const GpuDevice& dev, VkDescriptorSetLayout sceneSetLayout,
                      VkDescriptorSetLayout gbufferSetLayout)
{
	m_Device = dev;

	VkShaderModule vertModule = ShaderManager::LoadShader("raster.spv");
	VkShaderModule fragModule = ShaderManager::LoadShader("rasterfrag.spv");
	if (!vertModule || !fragModule)
	{
		RT_LOG("[RasterPass] Failed to load shaders");
		if (vertModule) vkDestroyShaderModule(dev.device, vertModule, nullptr);
		if (fragModule) vkDestroyShaderModule(dev.device, fragModule, nullptr);
		return false;
	}

	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vertModule;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fragModule;
	stages[1].pName = "main";

	// Vertex input: interleaved {vec3 pos, vec2 uv, vec3 normal, vec4 tangent}.
	// Tangent.w is the glTF bitangent handedness.
	VkVertexInputBindingDescription bindingDesc = {};
	bindingDesc.binding = 0;
	bindingDesc.stride = sizeof(float) * 12;
	bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

	VkVertexInputAttributeDescription attrDescs[4] = {};
	attrDescs[0].location = 0;
	attrDescs[0].binding = 0;
	attrDescs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
	attrDescs[0].offset = 0;
	attrDescs[1].location = 1;
	attrDescs[1].binding = 0;
	attrDescs[1].format = VK_FORMAT_R32G32_SFLOAT;
	attrDescs[1].offset = sizeof(float) * 3;
	attrDescs[2].location = 2;
	attrDescs[2].binding = 0;
	attrDescs[2].format = VK_FORMAT_R32G32B32_SFLOAT;
	attrDescs[2].offset = sizeof(float) * 5;
	attrDescs[3].location = 3;
	attrDescs[3].binding = 0;
	attrDescs[3].format = VK_FORMAT_R32G32B32A32_SFLOAT;
	attrDescs[3].offset = sizeof(float) * 8;

	VkPipelineVertexInputStateCreateInfo vertexInputInfo = {};
	vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInputInfo.vertexBindingDescriptionCount = 1;
	vertexInputInfo.pVertexBindingDescriptions = &bindingDesc;
	vertexInputInfo.vertexAttributeDescriptionCount = 4;
	vertexInputInfo.pVertexAttributeDescriptions = attrDescs;

	VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
	inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	inputAssembly.primitiveRestartEnable = VK_FALSE;

	VkPipelineViewportStateCreateInfo viewportState = {};
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo rasterizer = {};
	rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterizer.depthClampEnable = VK_FALSE;
	rasterizer.rasterizerDiscardEnable = VK_FALSE;
	rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
	rasterizer.lineWidth = 1.0f;
	rasterizer.cullMode = VK_CULL_MODE_NONE; // RT path hits both faces — raster must match
	rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterizer.depthBiasEnable = VK_FALSE;

	VkPipelineMultisampleStateCreateInfo multisampling = {};
	multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampling.sampleShadingEnable = VK_FALSE;
	multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo depthStencil = {};
	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable = VK_TRUE;
	depthStencil.depthWriteEnable = VK_TRUE;
	depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
	depthStencil.depthBoundsTestEnable = VK_FALSE;
	depthStencil.stencilTestEnable = VK_FALSE;

	// MRT: 8 color attachments, no blending (G-buffer writes are direct)
	VkPipelineColorBlendAttachmentState blendAttachments[8] = {};
	for (int i = 0; i < 8; i++)
	{
		blendAttachments[i].blendEnable = VK_FALSE;
		blendAttachments[i].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
		                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	}

	VkPipelineColorBlendStateCreateInfo colorBlending = {};
	colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlending.logicOpEnable = VK_FALSE;
	colorBlending.attachmentCount = 8;
	colorBlending.pAttachments = blendAttachments;

	VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamicState = {};
	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicState.dynamicStateCount = 2;
	dynamicState.pDynamicStates = dynamicStates;

	VkDescriptorSetLayout setLayouts[] = { sceneSetLayout, gbufferSetLayout };
	VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = 2;
	pipelineLayoutInfo.pSetLayouts = setLayouts;

	VK_CHECK(vkCreatePipelineLayout(m_Device.device, &pipelineLayoutInfo, nullptr, &m_PipelineLayout));

	VkPipelineRenderingCreateInfoKHR renderingInfo = {};
	renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
	// 8 MRT color attachments: gNormalRoughness, gViewZ, gMotion, gAlbedoF0,
	// gDirectEmission, gPrimHit, gPrimGeoNormal, gPrimUV
	VkFormat colorFormats[8] = {
		VK_FORMAT_A2B10G10R10_UNORM_PACK32, // 0: gNormalRoughness (NRD encoding=2)
		VK_FORMAT_R32_SFLOAT,               // 1: gViewZ (fp32 — fp16 overflows for large scenes)
		VK_FORMAT_R16G16_SFLOAT,       // 2: gMotion
		VK_FORMAT_R16G16B16A16_SFLOAT, // 3: gAlbedoF0
		VK_FORMAT_R16G16B16A16_SFLOAT, // 4: gDirectEmission
		VK_FORMAT_R32G32B32A32_SFLOAT, // 5: gPrimHit
		VK_FORMAT_R8G8B8A8_UNORM,     // 6: gPrimGeoNormal
		VK_FORMAT_R16G16_SFLOAT,      // 7: gPrimUV
	};
	renderingInfo.colorAttachmentCount = 8;
	renderingInfo.pColorAttachmentFormats = colorFormats;
	renderingInfo.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;

	VkGraphicsPipelineCreateInfo pipelineInfo = {};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineInfo.stageCount = 2;
	pipelineInfo.pStages = stages;
	pipelineInfo.pVertexInputState = &vertexInputInfo;
	pipelineInfo.pInputAssemblyState = &inputAssembly;
	pipelineInfo.pViewportState = &viewportState;
	pipelineInfo.pRasterizationState = &rasterizer;
	pipelineInfo.pMultisampleState = &multisampling;
	pipelineInfo.pDepthStencilState = &depthStencil;
	pipelineInfo.pColorBlendState = &colorBlending;
	pipelineInfo.pDynamicState = &dynamicState;
	pipelineInfo.layout = m_PipelineLayout;
	pipelineInfo.pNext = &renderingInfo;

	VkResult err = vkCreateGraphicsPipelines(m_Device.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_Pipeline);

	// Masked pipeline: same as opaque but depthWrite=OFF so discard'd fragments don't occlude
	VkPipelineDepthStencilStateCreateInfo maskedDepth = depthStencil;
	maskedDepth.depthWriteEnable = VK_FALSE;
	pipelineInfo.pDepthStencilState = &maskedDepth;
	VkResult err2 = vkCreateGraphicsPipelines(m_Device.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_MaskedPipeline);

	vkDestroyShaderModule(dev.device, vertModule, nullptr);
	vkDestroyShaderModule(dev.device, fragModule, nullptr);

	if (err != VK_SUCCESS)
	{
		RT_LOG("[RasterPass] vkCreateGraphicsPipelines (opaque) failed: %d", (int)err);
		return false;
	}
	if (err2 != VK_SUCCESS)
	{
		RT_LOG("[RasterPass] vkCreateGraphicsPipelines (masked) failed: %d", (int)err2);
		return false;
	}

	RT_LOG("[RasterPass] initialized");
	return true;
}

void RasterPass::Destroy()
{
	DestroyDrawData();
	DestroyVertexBuffers();
	if (m_Pipeline) { vkDestroyPipeline(m_Device.device, m_Pipeline, nullptr); m_Pipeline = VK_NULL_HANDLE; }
	if (m_MaskedPipeline) { vkDestroyPipeline(m_Device.device, m_MaskedPipeline, nullptr); m_MaskedPipeline = VK_NULL_HANDLE; }
	if (m_PipelineLayout) { vkDestroyPipelineLayout(m_Device.device, m_PipelineLayout, nullptr); m_PipelineLayout = VK_NULL_HANDLE; }
}

void RasterPass::CreateVertexBuffers(const GpuDevice& dev, const GPUSceneData& scene)
{
	DestroyVertexBuffers();

	// Build a single mega-vertex buffer with all meshes concatenated.
	// Per-triangle non-indexed (3 vertices per triangle, 12 floats per vertex).
	std::vector<float> megaVerts;
	std::vector<uint32_t> megaIndices;
	m_MeshVertexOffsets.resize(scene.meshes.size());
	m_MeshIndexOffsets.resize(scene.meshes.size());

	size_t totalVertexCount = 0;
	size_t totalIndexCount = 0;
	for (const auto& mesh : scene.meshes)
	{
		totalVertexCount += mesh.vertices->size() / 3;
		totalIndexCount += mesh.indices->size();
	}
	megaVerts.reserve(totalVertexCount * 12);
	megaIndices.reserve(totalIndexCount);

	for (size_t m = 0; m < scene.meshes.size(); m++)
	{
		m_MeshVertexOffsets[m] = static_cast<uint32_t>(megaVerts.size() / 12);
		m_MeshIndexOffsets[m] = static_cast<uint32_t>(megaIndices.size());

		const auto& mesh = scene.meshes[m];
		const auto& verts = *mesh.vertices;
		const auto& idxs = *mesh.indices;
		const auto& uvs = mesh.uvs ? *mesh.uvs : std::vector<float>{};
		const auto& normals = mesh.normals ? *mesh.normals : std::vector<float>{};
		const auto& tangents = mesh.tangents ? *mesh.tangents : std::vector<float>{};
		uint32_t vertexCount = static_cast<uint32_t>(verts.size() / 3);
		for (uint32_t idx = 0; idx < vertexCount; idx++)
		{
			uint32_t vi = idx * 3;
			megaVerts.push_back(verts[vi]);
			megaVerts.push_back(verts[vi + 1]);
			megaVerts.push_back(verts[vi + 2]);
			if (idx * 2 + 1 < uvs.size())
			{
				megaVerts.push_back(uvs[idx * 2]);
				megaVerts.push_back(uvs[idx * 2 + 1]);
			}
			else
			{
				megaVerts.push_back(0.0f);
				megaVerts.push_back(0.0f);
			}
			if (idx * 3 + 2 < normals.size())
			{
				megaVerts.push_back(normals[idx * 3]);
				megaVerts.push_back(normals[idx * 3 + 1]);
				megaVerts.push_back(normals[idx * 3 + 2]);
			}
			else
			{
				megaVerts.insert(megaVerts.end(), { 0.0f, 0.0f, 0.0f });
			}
			if (idx * 4 + 3 < tangents.size())
			{
				megaVerts.push_back(tangents[idx * 4]);
				megaVerts.push_back(tangents[idx * 4 + 1]);
				megaVerts.push_back(tangents[idx * 4 + 2]);
				megaVerts.push_back(tangents[idx * 4 + 3]);
			}
			else
			{
				megaVerts.insert(megaVerts.end(), { 1.0f, 0.0f, 0.0f, 1.0f });
			}
		}
		megaIndices.insert(megaIndices.end(), idxs.begin(), idxs.end());
	}

	VkDeviceSize vertexBufferSize = megaVerts.size() * sizeof(float);
	VkDeviceSize indexBufferSize = megaIndices.size() * sizeof(uint32_t);
	if (vertexBufferSize == 0 || indexBufferSize == 0) return;

	// Staging buffer (host-visible) → device-local vertex buffer
	VkBuffer vertexStagingBuffer;
	VkDeviceMemory vertexStagingMemory;
	GpuResources::CreateBuffer(dev, vertexBufferSize,
	             VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	             vertexStagingBuffer, vertexStagingMemory);

	void* data;
	vkMapMemory(dev.device, vertexStagingMemory, 0, vertexBufferSize, 0, &data);
	memcpy(data, megaVerts.data(), vertexBufferSize);
	vkUnmapMemory(dev.device, vertexStagingMemory);

	GpuResources::CreateBuffer(dev, vertexBufferSize,
	             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
	             m_MegaVertexBuffer, m_MegaVertexMemory);

	VkBuffer indexStagingBuffer;
	VkDeviceMemory indexStagingMemory;
	GpuResources::CreateBuffer(dev, indexBufferSize,
	             VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	             indexStagingBuffer, indexStagingMemory);
	vkMapMemory(dev.device, indexStagingMemory, 0, indexBufferSize, 0, &data);
	memcpy(data, megaIndices.data(), indexBufferSize);
	vkUnmapMemory(dev.device, indexStagingMemory);

	GpuResources::CreateBuffer(dev, indexBufferSize,
	             VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
	             m_MegaIndexBuffer, m_MegaIndexMemory);

	CommandUtils::ImmediateSubmit(dev, [&](VkCommandBuffer cmd) {
		VkBufferCopy vertexCopy = {};
		vertexCopy.size = vertexBufferSize;
		vkCmdCopyBuffer(cmd, vertexStagingBuffer, m_MegaVertexBuffer, 1, &vertexCopy);
		VkBufferCopy indexCopy = {};
		indexCopy.size = indexBufferSize;
		vkCmdCopyBuffer(cmd, indexStagingBuffer, m_MegaIndexBuffer, 1, &indexCopy);
	});

	GpuResources::DestroyBuffer(dev, vertexStagingBuffer, vertexStagingMemory);
	GpuResources::DestroyBuffer(dev, indexStagingBuffer, indexStagingMemory);

	RT_LOG("[RasterPass] Indexed mega-mesh: %zu vertex bytes, %zu index bytes, %zu meshes",
	       (size_t)vertexBufferSize, (size_t)indexBufferSize, scene.meshes.size());
}

void RasterPass::DestroyVertexBuffers()
{
	GpuResources::DestroyBuffer(m_Device, m_MegaVertexBuffer, m_MegaVertexMemory);
	GpuResources::DestroyBuffer(m_Device, m_MegaIndexBuffer, m_MegaIndexMemory);
	m_MegaVertexBuffer = VK_NULL_HANDLE;
	m_MegaVertexMemory = VK_NULL_HANDLE;
	m_MegaIndexBuffer = VK_NULL_HANDLE;
	m_MegaIndexMemory = VK_NULL_HANDLE;
	m_MeshVertexOffsets.clear();
	m_MeshIndexOffsets.clear();
}

void RasterPass::CreateDrawData(const GpuDevice& dev, const GPUSceneData& scene)
{
	DestroyDrawData();

	uint32_t totalInstances = static_cast<uint32_t>(scene.instances.size());
	if (totalInstances == 0) return;

	// Split instances into opaque (alphaMode 0) and masked (alphaMode 1).
	// BLEND (alphaMode 2) goes into masked pass too (no depth write).
	std::vector<VkDrawIndexedIndirectCommand> opaqueCmds;
	std::vector<VkDrawIndexedIndirectCommand> maskedCmds;
	opaqueCmds.reserve(totalInstances);
	maskedCmds.reserve(totalInstances);

	for (uint32_t i = 0; i < totalInstances; i++)
	{
		const auto& inst = scene.instances[i];
		uint32_t effMatIdx = inst.materialIndex;
		if (effMatIdx >= scene.materials.size())
			effMatIdx = 0;
		float alphaMode = scene.materials[effMatIdx].alphaMode;

		VkDrawIndexedIndirectCommand cmd = {};
		cmd.firstInstance = i;
		cmd.instanceCount = 1;

		if (inst.meshIndex < m_MeshVertexOffsets.size() &&
		    inst.meshIndex < m_MeshIndexOffsets.size())
		{
			cmd.indexCount = static_cast<uint32_t>(scene.meshes[inst.meshIndex].indices->size());
			cmd.firstIndex = m_MeshIndexOffsets[inst.meshIndex];
			cmd.vertexOffset = static_cast<int32_t>(m_MeshVertexOffsets[inst.meshIndex]);
		}
		else
		{
			cmd.indexCount = 0;
			cmd.firstIndex = 0;
			cmd.vertexOffset = 0;
		}

		if (alphaMode < 0.5f)
			opaqueCmds.push_back(cmd);
		else
			maskedCmds.push_back(cmd);
	}

	auto createDrawBuffer = [&](const std::vector<VkDrawIndexedIndirectCommand>& cmds,
	                            VkBuffer& buf, VkDeviceMemory& mem, uint32_t& count) {
		count = static_cast<uint32_t>(cmds.size());
		if (count == 0) return;
		VkDeviceSize bufSize = cmds.size() * sizeof(VkDrawIndexedIndirectCommand);
		GpuResources::CreateBuffer(dev, bufSize,
		             VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
		             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		             buf, mem);
		void* data;
		vkMapMemory(dev.device, mem, 0, bufSize, 0, &data);
		memcpy(data, cmds.data(), bufSize);
		vkUnmapMemory(dev.device, mem);
	};

	createDrawBuffer(opaqueCmds, m_OpaqueDrawBuffer, m_OpaqueDrawMemory, m_OpaqueDrawCount);
	createDrawBuffer(maskedCmds, m_MaskedDrawBuffer, m_MaskedDrawMemory, m_MaskedDrawCount);

	RT_LOG("[RasterPass] Draw data: %u opaque, %u masked", m_OpaqueDrawCount, m_MaskedDrawCount);
}

void RasterPass::DestroyDrawData()
{
	GpuResources::DestroyBuffer(m_Device, m_OpaqueDrawBuffer, m_OpaqueDrawMemory);
	m_OpaqueDrawBuffer = VK_NULL_HANDLE;
	m_OpaqueDrawMemory = VK_NULL_HANDLE;
	m_OpaqueDrawCount = 0;

	GpuResources::DestroyBuffer(m_Device, m_MaskedDrawBuffer, m_MaskedDrawMemory);
	m_MaskedDrawBuffer = VK_NULL_HANDLE;
	m_MaskedDrawMemory = VK_NULL_HANDLE;
	m_MaskedDrawCount = 0;
}

void RasterPass::Record(VkCommandBuffer cmd, const RenderExtent& extent,
	VkDescriptorSet sceneSet, VkDescriptorSet gbufferSet,
	VkImageView depthView, const VkImageView gbufferViews[8]) const
{
	const uint32_t width = extent.Width();
	const uint32_t height = extent.Height();
	if (!m_Pipeline) return;

	// Even with 0 draw calls, we must clear the G-buffer so stale data
	// from the previous frame doesn't persist (raster-first path tracer
	// would shade deleted geometry).
	VkClearValue clearValues[9] = {}; // 8 color + 1 depth
	// Color attachments cleared to zero
	for (int i = 0; i < 8; i++)
		clearValues[i].color = VkClearColorValue{};
	// Depth
	clearValues[8].depthStencil.depth = 1.0f;

	VkRenderingAttachmentInfoKHR colorAttachments[8] = {};
	for (int i = 0; i < 8; i++)
	{
		colorAttachments[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
		colorAttachments[i].imageView = gbufferViews[i];
		colorAttachments[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		colorAttachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		colorAttachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		colorAttachments[i].clearValue = clearValues[i];
	}

	VkRenderingAttachmentInfoKHR depthAttachment = {};
	depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
	depthAttachment.imageView = depthView;
	depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL_KHR;
	depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	depthAttachment.clearValue = clearValues[8];

	VkRenderingInfoKHR renderingInfo = {};
	renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR;
	renderingInfo.renderArea.offset.x = 0;
	renderingInfo.renderArea.offset.y = 0;
	renderingInfo.renderArea.extent.width = width;
	renderingInfo.renderArea.extent.height = height;
	renderingInfo.layerCount = 1;
	renderingInfo.colorAttachmentCount = 8;
	renderingInfo.pColorAttachments = colorAttachments;
	renderingInfo.pDepthAttachment = &depthAttachment;

	// KHR entry point, not the core 1.3 name: the instance is Vulkan 1.2 and
	// gets dynamic rendering from VK_KHR_dynamic_rendering. See RTDispatch.h.
	if (!g_RTDispatch.CmdBeginRenderingKHR || !g_RTDispatch.CmdEndRenderingKHR)
	{
		RT_LOG("[RasterPass] ERROR: dynamic rendering entry points not loaded");
		return;
	}
	g_RTDispatch.CmdBeginRenderingKHR(cmd, &renderingInfo);

	VkViewport viewport = {};
	viewport.x = 0.0f;
	viewport.y = 0.0f;
	viewport.width = (float)width;
	viewport.height = (float)height;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	vkCmdSetViewport(cmd, 0, 1, &viewport);

	VkRect2D scissor = {};
	scissor.extent.width = width;
	scissor.extent.height = height;
	vkCmdSetScissor(cmd, 0, 1, &scissor);

	VkDeviceSize offsets[] = { 0 };

	// Pass 1: opaque instances (depth write ON)
	if (m_MegaVertexBuffer && m_MegaIndexBuffer && m_OpaqueDrawCount > 0)
	{
		vkCmdBindVertexBuffers(cmd, 0, 1, &m_MegaVertexBuffer, offsets);
		vkCmdBindIndexBuffer(cmd, m_MegaIndexBuffer, 0, VK_INDEX_TYPE_UINT32);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Pipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_PipelineLayout,
		                        0, 1, &sceneSet, 0, nullptr);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_PipelineLayout,
		                        1, 1, &gbufferSet, 0, nullptr);
		vkCmdDrawIndexedIndirect(cmd, m_OpaqueDrawBuffer, 0, m_OpaqueDrawCount, sizeof(VkDrawIndexedIndirectCommand));
	}

	// Pass 2: masked/blend instances (depth write OFF — discard'd fragments don't occlude)
	if (m_MegaVertexBuffer && m_MegaIndexBuffer && m_MaskedDrawCount > 0)
	{
		vkCmdBindVertexBuffers(cmd, 0, 1, &m_MegaVertexBuffer, offsets);
		vkCmdBindIndexBuffer(cmd, m_MegaIndexBuffer, 0, VK_INDEX_TYPE_UINT32);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_MaskedPipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_PipelineLayout,
		                        0, 1, &sceneSet, 0, nullptr);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_PipelineLayout,
		                        1, 1, &gbufferSet, 0, nullptr);
		vkCmdDrawIndexedIndirect(cmd, m_MaskedDrawBuffer, 0, m_MaskedDrawCount, sizeof(VkDrawIndexedIndirectCommand));
	}

	g_RTDispatch.CmdEndRenderingKHR(cmd);
}
