#include "FrameRenderer.h"
#include "Camera.h"
#include "RTLog.h"

void FrameRenderer::RecordFrame(VkCommandBuffer cmd, Context& ctx)
{
	RecordTopBarrier(cmd, ctx);
	RecordASBarrier(cmd, ctx);
	RecordUBOUpdates(cmd, ctx);
	RecordRasterPass(cmd, ctx);
	RecordPathTraceOrDebug(cmd, ctx);
	RecordOutputTransition(cmd, ctx);
}

void FrameRenderer::RecordTopBarrier(VkCommandBuffer cmd, Context& ctx)
{
	VkImageMemoryBarrier topBarrier = {};
	topBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	topBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	topBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	topBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	topBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	topBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	topBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	topBarrier.image = ctx.outputImage.image;
	topBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	topBarrier.subresourceRange.levelCount = 1;
	topBarrier.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(cmd,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0,
		0, nullptr, 0, nullptr, 1, &topBarrier);
}

void FrameRenderer::RecordASBarrier(VkCommandBuffer cmd, Context& ctx)
{
	if (!ctx.scene.ASJustBuilt())
		return;

	VkMemoryBarrier asBarrier = {};
	asBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	asBarrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	asBarrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
	vkCmdPipelineBarrier(cmd,
		VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
		VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
		0, 1, &asBarrier, 0, nullptr, 0, nullptr);
	ctx.scene.ClearASJustBuilt();
}

void FrameRenderer::RecordUBOUpdates(VkCommandBuffer cmd, Context& ctx)
{
	VkMemoryBarrier uboPreBarrier = {};
	uboPreBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	uboPreBarrier.srcAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
	uboPreBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(cmd,
		VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 1, &uboPreBarrier, 0, nullptr, 0, nullptr);

	vkCmdUpdateBuffer(cmd, ctx.cameraUBO, 0, sizeof(SICameraData), &ctx.cameraUBOData);

	SINRDUniformData nrdData = { ctx.nrdEnabled ? 1u : 0u, 0, 0, 0 };
	if (ctx.nrdUBO)
		vkCmdUpdateBuffer(cmd, ctx.nrdUBO, 0, sizeof(SINRDUniformData), &nrdData);

	VkMemoryBarrier uboPostBarrier = {};
	uboPostBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	uboPostBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	uboPostBarrier.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
	vkCmdPipelineBarrier(cmd,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
		0, 1, &uboPostBarrier, 0, nullptr, 0, nullptr);
}

void FrameRenderer::RecordRasterPass(VkCommandBuffer cmd, Context& ctx)
{
	if (!ctx.rasterPass.IsAvailable() || !ctx.gbuffer.GetDepth().view)
		return;

	VkImage gbufferImgs[8];
	ctx.gbuffer.GetMRTImages(gbufferImgs);

	VkImageMemoryBarrier gbufferBarriers[8] = {};
	for (int i = 0; i < 8; i++)
	{
		gbufferBarriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		gbufferBarriers[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
		gbufferBarriers[i].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		gbufferBarriers[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
		gbufferBarriers[i].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		gbufferBarriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		gbufferBarriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		gbufferBarriers[i].image = gbufferImgs[i];
		gbufferBarriers[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		gbufferBarriers[i].subresourceRange.levelCount = 1;
		gbufferBarriers[i].subresourceRange.layerCount = 1;
	}
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
	                     0, nullptr, 0, nullptr, 8, gbufferBarriers);

	VkImageMemoryBarrier depthBarrier = {};
	depthBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	depthBarrier.srcAccessMask = 0;
	depthBarrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	depthBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	depthBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
	depthBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	depthBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	depthBarrier.image = ctx.gbuffer.GetDepth().image;
	depthBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	depthBarrier.subresourceRange.levelCount = 1;
	depthBarrier.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
	                     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0,
	                     0, nullptr, 0, nullptr, 1, &depthBarrier);

	VkImageView gbufferViews[8];
	ctx.gbuffer.GetMRTViews(gbufferViews);
	ctx.rasterPass.Record(cmd, ctx.width, ctx.height,
	                    ctx.pathTracePass.GetDescriptorSet(), ctx.gbufferSet,
	                    ctx.gbuffer.GetDepth().view, gbufferViews);

	VkImageMemoryBarrier postRasterBarriers[8] = {};
	for (int i = 0; i < 8; i++)
	{
		postRasterBarriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		postRasterBarriers[i].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		postRasterBarriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		postRasterBarriers[i].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		postRasterBarriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
		postRasterBarriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		postRasterBarriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		postRasterBarriers[i].image = gbufferImgs[i];
		postRasterBarriers[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		postRasterBarriers[i].subresourceRange.levelCount = 1;
		postRasterBarriers[i].subresourceRange.layerCount = 1;
	}
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	                     VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
	                     0, nullptr, 0, nullptr, 8, postRasterBarriers);
}

void FrameRenderer::RecordPathTraceOrDebug(VkCommandBuffer cmd, Context& ctx)
{
	if (ctx.gbufferDebugMode >= 0 && ctx.gbufferDebugPass.IsAvailable())
	{
		ctx.gbufferDebugPass.Record(cmd, ctx.width, ctx.height,
		                          ctx.pathTracePass.GetDescriptorSet(), ctx.gbufferSet,
		                          (uint32_t)ctx.gbufferDebugMode);
		return;
	}

	bool useRasterFirst = ctx.rasterFirst && (ctx.camera.m_Aperture <= 0.0f);
	ctx.pathTracePass.Record(cmd, ctx.width, ctx.height, ctx.gbufferSet, useRasterFirst);

	if (ctx.nrdEnabled && ctx.nrd.IsAvailable() && useRasterFirst)
		RecordNRDAndCompose(cmd, ctx);
}

void FrameRenderer::RecordNRDAndCompose(VkCommandBuffer cmd, Context& ctx)
{
	VkImage preNrdImgs[] = {
		ctx.gbuffer.GetColor(GBufferTarget::DIFF_RADIANCE).image,
		ctx.gbuffer.GetColor(GBufferTarget::SPEC_RADIANCE).image,
		ctx.gbuffer.GetColor(GBufferTarget::NORMAL_ROUGHNESS).image,
		ctx.gbuffer.GetColor(GBufferTarget::VIEWZ).image,
		ctx.gbuffer.GetColor(GBufferTarget::MOTION).image,
		ctx.gbuffer.GetColor(GBufferTarget::DIRECT_EMISSION).image
	};
	VkAccessFlags preNrdSrc[] = {
		VK_ACCESS_SHADER_WRITE_BIT,
		VK_ACCESS_SHADER_WRITE_BIT,
		VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	};
	VkImageMemoryBarrier preNrdBarriers[6] = {};
	for (int i = 0; i < 6; i++)
	{
		preNrdBarriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		preNrdBarriers[i].srcAccessMask = preNrdSrc[i];
		preNrdBarriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		preNrdBarriers[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
		preNrdBarriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
		preNrdBarriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		preNrdBarriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		preNrdBarriers[i].image = preNrdImgs[i];
		preNrdBarriers[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		preNrdBarriers[i].subresourceRange.levelCount = 1;
		preNrdBarriers[i].subresourceRange.layerCount = 1;
	}
	vkCmdPipelineBarrier(cmd,
	                     VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
	                     0, nullptr, 0, nullptr, 6, preNrdBarriers);

	ctx.nrd.NewFrame();

	const Camera& cam = ctx.camera;
	glm::mat4 viewToClip = cam.GetProjection();
	glm::mat4 worldToView = cam.GetView();
	glm::mat4 prevViewToClip = ctx.hasPrevMatrices ? ctx.prevViewToClip : viewToClip;
	glm::mat4 prevWorldToView = ctx.hasPrevMatrices ? ctx.prevWorldToView : worldToView;

	bool reset = ctx.nrdNeedsReset;
	ctx.nrd.SetCommonSettings(
		glm::value_ptr(viewToClip),
		glm::value_ptr(prevViewToClip),
		glm::value_ptr(worldToView),
		glm::value_ptr(prevWorldToView),
		ctx.nrdJitter.x, ctx.nrdJitter.y,
		ctx.nrdJitterPrev.x, ctx.nrdJitterPrev.y,
		ctx.nrdFrameIndex, reset, ctx.nrdSplitScreen);
	ctx.nrdNeedsReset = false;

	ctx.nrd.SetReblurSettings(ctx.nrdMaxBlurRadius, (uint32_t)ctx.nrdMaxAccumFrames,
	                        ctx.nrdAntiFirefly, ctx.nrdSplitScreen);

	ctx.nrd.Denoise(cmd,
		ctx.gbuffer.GetColor(GBufferTarget::NORMAL_ROUGHNESS).image, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
		ctx.gbuffer.GetColor(GBufferTarget::VIEWZ).image, VK_FORMAT_R32_SFLOAT,
		ctx.gbuffer.GetColor(GBufferTarget::MOTION).image, VK_FORMAT_R16G16_SFLOAT,
		ctx.gbuffer.GetColor(GBufferTarget::DIFF_RADIANCE).image, VK_FORMAT_R16G16B16A16_SFLOAT,
		ctx.gbuffer.GetColor(GBufferTarget::SPEC_RADIANCE).image, VK_FORMAT_R16G16B16A16_SFLOAT,
		ctx.gbuffer.GetColor(GBufferTarget::NRD_DIFF_OUT).image, ctx.gbuffer.GetColor(GBufferTarget::NRD_SPEC_OUT).image);

	if (ctx.composePass.IsAvailable())
	{
		if (!ctx.composeDescriptorSetCached)
		{
			ctx.composePass.UpdateDescriptorSet(ctx.device,
				ctx.outputImage.view,
				ctx.gbuffer.GetColor(GBufferTarget::NRD_DIFF_OUT).view,
				ctx.gbuffer.GetColor(GBufferTarget::NRD_SPEC_OUT).view,
				ctx.gbuffer.GetColor(GBufferTarget::ALBEDO_F0).view,
				ctx.gbuffer.GetColor(GBufferTarget::DIRECT_EMISSION).view,
				ctx.gbuffer.GetColor(GBufferTarget::VIEWZ).view);
			ctx.composeDescriptorSetCached = true;
		}

		VkImage composeImgs[] = {
			ctx.gbuffer.GetColor(GBufferTarget::NRD_DIFF_OUT).image,
			ctx.gbuffer.GetColor(GBufferTarget::NRD_SPEC_OUT).image,
			ctx.gbuffer.GetColor(GBufferTarget::ALBEDO_F0).image,
			ctx.gbuffer.GetColor(GBufferTarget::DIRECT_EMISSION).image,
			ctx.gbuffer.GetColor(GBufferTarget::VIEWZ).image
		};
		VkImageMemoryBarrier composeBarriers[5] = {};
		for (int i = 0; i < 5; i++)
		{
			composeBarriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			composeBarriers[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
			composeBarriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			composeBarriers[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
			composeBarriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
			composeBarriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			composeBarriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			composeBarriers[i].image = composeImgs[i];
			composeBarriers[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			composeBarriers[i].subresourceRange.levelCount = 1;
			composeBarriers[i].subresourceRange.layerCount = 1;
		}
		vkCmdPipelineBarrier(cmd,
		                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
		                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
		                     0, nullptr, 0, nullptr, 5, composeBarriers);

		ctx.composePass.Record(cmd, ctx.width, ctx.height);
	}
}

void FrameRenderer::RecordOutputTransition(VkCommandBuffer cmd, Context& ctx)
{
	VkImageMemoryBarrier rtReadBarrier = {};
	rtReadBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	rtReadBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	rtReadBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	rtReadBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	rtReadBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	rtReadBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	rtReadBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	rtReadBarrier.image = ctx.outputImage.image;
	rtReadBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	rtReadBarrier.subresourceRange.levelCount = 1;
	rtReadBarrier.subresourceRange.layerCount = 1;

	VkPipelineStageFlags srcStage;
	if (ctx.gbufferDebugMode >= 0 && ctx.gbufferDebugPass.IsAvailable())
		srcStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
	else if (ctx.nrdEnabled && ctx.nrd.IsAvailable() && ctx.composePass.IsAvailable())
		srcStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
	else
		srcStage = VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;

	vkCmdPipelineBarrier(cmd, srcStage, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &rtReadBarrier);
}