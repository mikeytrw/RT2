#include "FrameRenderer.h"
#include "Camera.h"
#include "RTLog.h"
#include <cmath>

void FrameRenderer::RecordFrame(VkCommandBuffer cmd, Context& ctx)
{
	RT_LOG("[Frame] RecordFrame begin");
	if (ctx.gpuProfiler)
		ctx.gpuProfiler->BeginRegion(cmd, GpuTimestampProfiler::Region::Frame, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
	RecordTopBarrier(cmd, ctx);
	RecordASBarrier(cmd, ctx);
	RecordUBOUpdates(cmd, ctx);

	// Advance prev transform buffers to current after a scene edit so
	// motion vectors go to zero on subsequent frames (NRD finding #3).
	if (ctx.scene.NeedsTransformAdvance())
	{
		RT_LOG("[Frame] advancing transform buffers");
		ctx.scene.AdvanceTransformBuffers(cmd);
		ctx.scene.ClearTransformAdvance();
	}

	RecordRasterPass(cmd, ctx);
	RT_LOG("[Frame] raster done");
	RecordRRGuidePass(cmd, ctx);
	RT_LOG("[Frame] RR guides done");
	RecordReSTIRPass(cmd, ctx);
	RT_LOG("[Frame] ReSTIR done");
	RecordReSTIRGIPass(cmd, ctx);
	RT_LOG("[Frame] ReSTIR GI done");
	RecordPathTraceOrDebug(cmd, ctx);
	RT_LOG("[Frame] pathtrace/debug done");
	RecordTonemapPass(cmd, ctx);
	RecordOutputTransition(cmd, ctx);
	if (ctx.gpuProfiler)
		ctx.gpuProfiler->EndRegion(cmd, GpuTimestampProfiler::Region::Frame, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
	RT_LOG("[Frame] RecordFrame end");
}

void FrameRenderer::RecordTopBarrier(VkCommandBuffer cmd, Context& ctx)
{
	VkImageMemoryBarrier topBarriers[2] = {};
	VkImageMemoryBarrier& topBarrier = topBarriers[0];
	topBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	// The output is both sampled by the UI and read as the previous frame by
	// temporalAccumulate. Synchronize the complete read/modify/write chain.
	topBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	topBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	topBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	topBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	topBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	topBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	topBarrier.image = ctx.outputImage.image;
	topBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	topBarrier.subresourceRange.levelCount = 1;
	topBarrier.subresourceRange.layerCount = 1;
	topBarriers[1] = topBarrier;
	topBarriers[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	topBarriers[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	topBarriers[1].image = ctx.displayImage.image;
	vkCmdPipelineBarrier(cmd,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
		0, nullptr, 0, nullptr, 2, topBarriers);
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
		VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
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

	// NRD UBO: nrdEnabled, lobeDither, restirGIEnabled, restirGIReservoirIndex.
	// The spare fields are repurposed for GI control without growing the UBO.
	// When NRD is off, force lobe dither to 0 (white noise) — Bayer/IGN dithering
	// is only needed for NRD's hit-distance reconstruction of skipped lobes.
	uint32_t effectiveLobeDither = ctx.nrdEnabled ? (uint32_t)ctx.lobeDither : 0u;
	SINRDUniformData nrdData = {
		ctx.nrdEnabled ? 1u : 0u,
		effectiveLobeDither,
		ctx.restirGIEnabled ? 1u : 0u,
		ctx.giReservoirIndex
	};
	if (ctx.nrdUBO)
		vkCmdUpdateBuffer(cmd, ctx.nrdUBO, 0, sizeof(SINRDUniformData), &nrdData);

	VkMemoryBarrier uboPostBarrier = {};
	uboPostBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	uboPostBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	uboPostBarrier.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
	vkCmdPipelineBarrier(cmd,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
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
	if (ctx.gpuProfiler)
		ctx.gpuProfiler->BeginRegion(cmd, GpuTimestampProfiler::Region::Raster, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
	ctx.rasterPass.Record(cmd, ctx.renderExtent,
	                    ctx.pathTracePass.GetDescriptorSet(), ctx.gbufferSet,
	                    ctx.gbuffer.GetDepth().view, gbufferViews);
	if (ctx.gpuProfiler)
		ctx.gpuProfiler->EndRegion(cmd, GpuTimestampProfiler::Region::Raster, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

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

void FrameRenderer::RecordRRGuidePass(VkCommandBuffer cmd, Context& ctx)
{
	// W3 guides are produced only on the raster-first path. They are separate
	// RT2-owned images and never enter NRD/compose/tone-map.
	if (!ctx.rasterFirst || ctx.camera.m_Aperture > 0.0f ||
	    !ctx.rrGuidePass.IsAvailable() || !ctx.rrGuides.IsValid())
		return;
	// Establish an explicit per-pixel producer sentinel before either the
	// material guide compute pass or raygen writes.  The reporter rejects any
	// sentinel left behind, so finite stale contents cannot masquerade as
	// complete production.  All images remain GENERAL; the barriers describe
	// the transfer clear and subsequent shader ownership exactly.
	const RRGuideKind kinds[] = { RRGuideKind::NoisyHdr, RRGuideKind::DiffuseAlbedo,
		RRGuideKind::SpecularAlbedo, RRGuideKind::NormalRoughness,
		RRGuideKind::SpecularHitDistance };
	const VkClearColorValue sentinels[] = {
		{{65504.0f, 65504.0f, 65504.0f, 0.0f}},
		{{0.0f, 0.0f, 0.0f, 0.0f}},
		{{0.0f, 0.0f, 0.0f, 0.0f}},
		{{NAN, NAN, NAN, NAN}},
		{{-1.0f, -1.0f, -1.0f, -1.0f}}
	};
	VkImageMemoryBarrier clearToTransfer[5] = {};
	for (uint32_t i = 0; i < 5; ++i)
	{
		clearToTransfer[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		clearToTransfer[i].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		clearToTransfer[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		clearToTransfer[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
		clearToTransfer[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
		clearToTransfer[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		clearToTransfer[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		clearToTransfer[i].image = ctx.rrGuides.Get(kinds[i]).image;
		clearToTransfer[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		clearToTransfer[i].subresourceRange.levelCount = 1;
		clearToTransfer[i].subresourceRange.layerCount = 1;
	}
	vkCmdPipelineBarrier(cmd,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 5, clearToTransfer);
	for (uint32_t i = 0; i < 5; ++i)
		vkCmdClearColorImage(cmd, ctx.rrGuides.Get(kinds[i]).image,
			VK_IMAGE_LAYOUT_GENERAL, &sentinels[i], 1, &clearToTransfer[i].subresourceRange);
	VkImageMemoryBarrier clearToShader[5] = {};
	for (uint32_t i = 0; i < 5; ++i)
	{
		clearToShader[i] = clearToTransfer[i];
		clearToShader[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		clearToShader[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	}
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
		0, 0, nullptr, 0, nullptr, 5, clearToShader);
	ctx.rrGuidePass.Record(cmd, ctx.renderExtent,
		ctx.pathTracePass.GetDescriptorSet(), ctx.gbufferSet);
	VkImageMemoryBarrier barriers[3] = {};
	const RRGuideKind materialKinds[] = { RRGuideKind::DiffuseAlbedo, RRGuideKind::SpecularAlbedo,
		RRGuideKind::NormalRoughness };
	for (uint32_t i = 0; i < 3; ++i)
	{
		barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barriers[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barriers[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
		barriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
		barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[i].image = ctx.rrGuides.Get(materialKinds[i]).image;
		barriers[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barriers[i].subresourceRange.levelCount = 1;
		barriers[i].subresourceRange.layerCount = 1;
	}
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
		0, 0, nullptr, 0, nullptr, 3, barriers);
}

void FrameRenderer::RecordReSTIRPass(VkCommandBuffer cmd, Context& ctx)
{
	// ReSTIR is raster-first mode only (needs G-buffer). Skip if disabled,
	// pipeline unavailable, or no reservoir buffers.
	if (!ctx.restirEnabled || !ctx.restirPass.IsAvailable() || !ctx.reservoirs.IsValid())
		return;
	if (!ctx.rasterFirst)
		return;

	VkBuffer historyBuf = ctx.reservoirs.GetHistoryBuffer();
	VkBuffer scratchBuf = ctx.reservoirs.GetScratchBuffer();
	VkBuffer surfaceHistBuf = ctx.reservoirs.GetSurfaceHistoryBuffer();

	// 1. Barrier: history + surfaceHistory (previous frame's spatial/temporal write) → temporal read
	VkBufferMemoryBarrier temporalPreBarriers[3] = {};
	for (int i = 0; i < 3; i++)
	{
		temporalPreBarriers[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		temporalPreBarriers[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
		temporalPreBarriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		temporalPreBarriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		temporalPreBarriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	}
	temporalPreBarriers[0].buffer = historyBuf;
	temporalPreBarriers[0].size = ctx.reservoirs.GetBufferSize();
	temporalPreBarriers[1].buffer = surfaceHistBuf;
	temporalPreBarriers[1].size = ctx.reservoirs.GetSurfaceHistorySize();

	// Also need scratch → temporal write barrier
	temporalPreBarriers[2].buffer = scratchBuf;
	temporalPreBarriers[2].size = ctx.reservoirs.GetBufferSize();
	temporalPreBarriers[2].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	temporalPreBarriers[2].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;

	vkCmdPipelineBarrier(cmd,
	                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
	                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
	                     0, nullptr, 3, temporalPreBarriers, 0, nullptr);

	// 2. Dispatch temporal pass: history → scratch
	if (ctx.gpuProfiler)
		ctx.gpuProfiler->BeginRegion(cmd, GpuTimestampProfiler::Region::ReSTIRDITemporal, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	ctx.restirPass.RecordTemporal(cmd, ctx.renderExtent,
	                              ctx.pathTracePass.GetDescriptorSet(), ctx.gbufferSet,
	                              ctx.restirPC);
	if (ctx.gpuProfiler)
		ctx.gpuProfiler->EndRegion(cmd, GpuTimestampProfiler::Region::ReSTIRDITemporal, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	// 3. Barrier: scratch (temporal write) → spatial read
	//    + history and surface history (temporal read) → spatial write
	VkBufferMemoryBarrier spatialPreBarriers[3] = {};
	for (int i = 0; i < 3; i++)
	{
		spatialPreBarriers[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		spatialPreBarriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		spatialPreBarriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	}
	spatialPreBarriers[0].buffer = scratchBuf;
	spatialPreBarriers[0].size = ctx.reservoirs.GetBufferSize();
	spatialPreBarriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	spatialPreBarriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

	spatialPreBarriers[1].buffer = historyBuf;
	spatialPreBarriers[1].size = ctx.reservoirs.GetBufferSize();
	spatialPreBarriers[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	spatialPreBarriers[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;

	spatialPreBarriers[2].buffer = surfaceHistBuf;
	spatialPreBarriers[2].size = ctx.reservoirs.GetSurfaceHistorySize();
	spatialPreBarriers[2].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	spatialPreBarriers[2].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;

	vkCmdPipelineBarrier(cmd,
	                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
	                     0, nullptr, 3, spatialPreBarriers, 0, nullptr);

	// 4. Dispatch spatial pass: scratch → history
	if (ctx.gpuProfiler)
		ctx.gpuProfiler->BeginRegion(cmd, GpuTimestampProfiler::Region::ReSTIRDISpatial, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	ctx.restirPass.RecordSpatial(cmd, ctx.renderExtent,
	                             ctx.pathTracePass.GetDescriptorSet(), ctx.gbufferSet,
	                             ctx.restirPC);
	if (ctx.gpuProfiler)
		ctx.gpuProfiler->EndRegion(cmd, GpuTimestampProfiler::Region::ReSTIRDISpatial, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	// 5. Barrier: history (spatial write) → RT shader read
	VkBufferMemoryBarrier postBarrier = {};
	postBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	postBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	postBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	postBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	postBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	postBarrier.buffer = historyBuf;
	postBarrier.offset = 0;
	postBarrier.size = ctx.reservoirs.GetBufferSize();
	vkCmdPipelineBarrier(cmd,
	                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	                     VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0,
	                     0, nullptr, 1, &postBarrier, 0, nullptr);
}

void FrameRenderer::RecordReSTIRGIPass(VkCommandBuffer cmd, Context& ctx)
{
	// GI requires raster-first G-buffer. It does NOT require ReSTIR DI.
	// Skip if disabled, pipeline unavailable, no GI buffer, or dummy buffer.
	if (!ctx.restirGIEnabled || !ctx.restirGIPass.IsAvailable())
		return;
	if (!ctx.rasterFirst)
		return;
	if (!ctx.giReservoirs.IsValid() || ctx.giReservoirs.IsDummy())
		return;

	VkBuffer giBuf = ctx.giReservoirs.GetBuffer();
	VkDeviceSize reservoirRegionSize = ctx.giReservoirs.GetReservoirRegionSize();
	VkDeviceSize historyRegionSize    = ctx.giReservoirs.GetReceiverHistoryRegionSize();
	VkDeviceSize totalSize            = ctx.giReservoirs.GetTotalSize();

	// Both regions must be masked to the parity bit. `giFrameIndex ^ 1u`
	// only happens to equal the other region for frame indices 0 and 1; from
	// frame 2 on it returns 3, 5, 7... and the barriers below then covered
	// ranges past the end of the buffer instead of the previous region, so
	// the real previous reservoir was never synchronised at all.
	uint32_t curRegion  = ctx.giFrameIndex & 1u;
	uint32_t prevRegion = curRegion ^ 1u;

	VkDeviceSize curReservoirOffset  = ctx.giReservoirs.GetReservoirRegionOffset(curRegion);
	VkDeviceSize prevReservoirOffset = ctx.giReservoirs.GetReservoirRegionOffset(prevRegion);
	VkDeviceSize curHistoryOffset    = ctx.giReservoirs.GetReceiverHistoryRegionOffset(curRegion);
	VkDeviceSize prevHistoryOffset   = ctx.giReservoirs.GetReceiverHistoryRegionOffset(prevRegion);

	// 1. Barrier: prev reservoir + prev receiver history → compute read;
	//    current reservoir → compute write. G-buffer images are already
	//    compute-readable after the post-raster barrier.
	VkBufferMemoryBarrier temporalPreBarriers[3] = {};
	for (int i = 0; i < 3; i++)
	{
		temporalPreBarriers[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		temporalPreBarriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		temporalPreBarriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	}
	// Previous reservoir region: shader write/read → compute read.
	temporalPreBarriers[0].buffer = giBuf;
	temporalPreBarriers[0].offset = prevReservoirOffset;
	temporalPreBarriers[0].size   = reservoirRegionSize;
	temporalPreBarriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
	temporalPreBarriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	// Previous receiver history: shader write/read → compute read.
	temporalPreBarriers[1].buffer = giBuf;
	temporalPreBarriers[1].offset = prevHistoryOffset;
	temporalPreBarriers[1].size   = historyRegionSize;
	temporalPreBarriers[1].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
	temporalPreBarriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	// Current reservoir region: shader read → compute write.
	temporalPreBarriers[2].buffer = giBuf;
	temporalPreBarriers[2].offset = curReservoirOffset;
	temporalPreBarriers[2].size   = reservoirRegionSize;
	temporalPreBarriers[2].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	temporalPreBarriers[2].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;

	vkCmdPipelineBarrier(cmd,
	                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
	                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
	                     0, nullptr, 3, temporalPreBarriers, 0, nullptr);

	// 2. Dispatch temporal pass: prev reservoir + prev history → current reservoir.
	//    Phase 0 stub writes empty (invalid) reservoirs only.
	if (ctx.gpuProfiler)
		ctx.gpuProfiler->BeginRegion(cmd, GpuTimestampProfiler::Region::ReSTIRGITemporal, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	ctx.restirGIPass.RecordTemporal(cmd, ctx.renderExtent,
	                               ctx.pathTracePass.GetDescriptorSet(), ctx.gbufferSet,
	                               ctx.restirGIPC);
	if (ctx.gpuProfiler)
		ctx.gpuProfiler->EndRegion(cmd, GpuTimestampProfiler::Region::ReSTIRGITemporal, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	// 3. Barrier: serialize — temporal writes (current reservoir) complete
	//    before the history-write dispatch reads G-buffer and writes current
	//    receiver history. Also transition current reservoir: compute write →
	//    ray-tracing read for final shading.
	VkBufferMemoryBarrier betweenBarriers[2] = {};
	for (int i = 0; i < 2; i++)
	{
		betweenBarriers[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		betweenBarriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		betweenBarriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	}
	// Current reservoir: compute write → RT read.
	betweenBarriers[0].buffer = giBuf;
	betweenBarriers[0].offset = curReservoirOffset;
	betweenBarriers[0].size   = reservoirRegionSize;
	betweenBarriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	betweenBarriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	// Current receiver history: shader read → compute write (history dispatch).
	betweenBarriers[1].buffer = giBuf;
	betweenBarriers[1].offset = curHistoryOffset;
	betweenBarriers[1].size   = historyRegionSize;
	betweenBarriers[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	betweenBarriers[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;

	vkCmdPipelineBarrier(cmd,
	                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0,
	                     0, nullptr, 2, betweenBarriers, 0, nullptr);

	// 4. Dispatch history-write pass: G-buffer → current receiver history.
	//    Runs AFTER all temporal reads of the previous history region finish,
	//    avoiding the read/write race a single dispatch would create.
	if (ctx.gpuProfiler)
		ctx.gpuProfiler->BeginRegion(cmd, GpuTimestampProfiler::Region::ReSTIRGIHistory, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	ctx.restirGIPass.RecordHistoryWrite(cmd, ctx.renderExtent,
	                                    ctx.pathTracePass.GetDescriptorSet(), ctx.gbufferSet,
	                                    ctx.restirGIPC);
	if (ctx.gpuProfiler)
		ctx.gpuProfiler->EndRegion(cmd, GpuTimestampProfiler::Region::ReSTIRGIHistory, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	// 5. Barrier: current receiver history (compute write) → next-frame compute read.
	VkBufferMemoryBarrier historyPostBarrier = {};
	historyPostBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	historyPostBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	historyPostBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	historyPostBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	historyPostBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	historyPostBarrier.buffer = giBuf;
	historyPostBarrier.offset = curHistoryOffset;
	historyPostBarrier.size   = historyRegionSize;
	vkCmdPipelineBarrier(cmd,
	                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0,
	                     0, nullptr, 1, &historyPostBarrier, 0, nullptr);
}

void FrameRenderer::RecordPathTraceOrDebug(VkCommandBuffer cmd, Context& ctx)
{
    if (ctx.gbufferDebugMode >= 0 && ctx.gbufferDebugMode < 19 &&
        ctx.gbufferDebugPass.IsAvailable())
	{
		ctx.gbufferDebugPass.Record(cmd, ctx.renderExtent,
		                          ctx.pathTracePass.GetDescriptorSet(), ctx.gbufferSet,
		                          (uint32_t)ctx.gbufferDebugMode);
		return;
	}

	bool useRasterFirst = ctx.rasterFirst && (ctx.camera.m_Aperture <= 0.0f);
	if (ctx.gpuProfiler)
		ctx.gpuProfiler->BeginRegion(cmd, GpuTimestampProfiler::Region::RTShading, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);
	ctx.pathTracePass.Record(cmd, ctx.renderExtent, ctx.gbufferSet, useRasterFirst);
	if (ctx.gpuProfiler)
		ctx.gpuProfiler->EndRegion(cmd, GpuTimestampProfiler::Region::RTShading, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);
	if (useRasterFirst && ctx.rrGuides.IsValid())
	{
		VkImageMemoryBarrier guideBarriers[2] = {};
		const RRGuideKind kinds[2] = { RRGuideKind::NoisyHdr, RRGuideKind::SpecularHitDistance };
		for (uint32_t i = 0; i < 2; ++i)
		{
			guideBarriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			guideBarriers[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
			guideBarriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			guideBarriers[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
			guideBarriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
			guideBarriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			guideBarriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			guideBarriers[i].image = ctx.rrGuides.Get(kinds[i]).image;
			guideBarriers[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			guideBarriers[i].subresourceRange.levelCount = 1;
			guideBarriers[i].subresourceRange.layerCount = 1;
		}
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
			0, 0, nullptr, 0, nullptr, 2, guideBarriers);
	}

	// Modes 19-20 inspect the packed NRD inputs produced by raster-first RT
	// shading. Run them after the RT dispatch and before NRD consumes the images.
	if (ctx.gbufferDebugMode >= 19 && ctx.gbufferDebugMode <= 26 &&
	    (ctx.gbufferDebugMode < 22 || ctx.rrGuides.IsValid()) &&
	    ctx.gbufferDebugPass.IsAvailable())
	{
		VkImage debugImages[] = {
			ctx.gbuffer.GetColor(GBufferTarget::DIFF_RADIANCE).image,
			ctx.gbuffer.GetColor(GBufferTarget::SPEC_RADIANCE).image
		};
		VkImageMemoryBarrier barriers[2] = {};
		for (int i = 0; i < 2; ++i)
		{
			barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			barriers[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
			barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			barriers[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
			barriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
			barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barriers[i].image = debugImages[i];
			barriers[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			barriers[i].subresourceRange.levelCount = 1;
			barriers[i].subresourceRange.layerCount = 1;
		}
		vkCmdPipelineBarrier(cmd,
		                     VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
		                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
		                     0, nullptr, 0, nullptr, 2, barriers);
		ctx.gbufferDebugPass.Record(cmd, ctx.renderExtent,
		                            ctx.pathTracePass.GetDescriptorSet(), ctx.gbufferSet,
		                            (uint32_t)ctx.gbufferDebugMode);
		return;
	}

	if (ctx.nrdEnabled && ctx.nrd.IsAvailable() && useRasterFirst)
		RecordNRDAndCompose(cmd, ctx);
}

void FrameRenderer::RecordNRDAndCompose(VkCommandBuffer cmd, Context& ctx)
{
	RT_LOG("[NRD] RecordNRDAndCompose begin");

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
	                        ctx.nrdResponsiveRoughnessThreshold,
	                        (uint32_t)ctx.nrdResponsiveMinAccumFrames,
	                        ctx.nrdAntiFirefly, ctx.nrdSplitScreen);

	if (ctx.gpuProfiler)
		ctx.gpuProfiler->BeginRegion(cmd, GpuTimestampProfiler::Region::NRD, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	ctx.nrd.Denoise(cmd,
		ctx.gbuffer.GetColor(GBufferTarget::NORMAL_ROUGHNESS).image, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
		ctx.gbuffer.GetColor(GBufferTarget::VIEWZ).image, VK_FORMAT_R32_SFLOAT,
		ctx.gbuffer.GetColor(GBufferTarget::MOTION).image, VK_FORMAT_R16G16_SFLOAT,
		ctx.gbuffer.GetColor(GBufferTarget::DIFF_RADIANCE).image, VK_FORMAT_R16G16B16A16_SFLOAT,
		ctx.gbuffer.GetColor(GBufferTarget::SPEC_RADIANCE).image, VK_FORMAT_R16G16B16A16_SFLOAT,
		ctx.gbuffer.GetColor(GBufferTarget::NRD_DIFF_OUT).image, ctx.gbuffer.GetColor(GBufferTarget::NRD_SPEC_OUT).image);
	if (ctx.gpuProfiler)
		ctx.gpuProfiler->EndRegion(cmd, GpuTimestampProfiler::Region::NRD, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	RT_LOG("[NRD] Denoise recorded");

	if (ctx.composePass.IsAvailable())
	{
		if (!ctx.composeDescriptorSetCached)
		{
			RT_LOG("[NRD] updating compose descriptor set (7 images + UBO)");
			ctx.composePass.UpdateDescriptorSet(ctx.device,
				ctx.outputImage.view,
				ctx.gbuffer.GetColor(GBufferTarget::NRD_DIFF_OUT).view,
				ctx.gbuffer.GetColor(GBufferTarget::NRD_SPEC_OUT).view,
				ctx.gbuffer.GetColor(GBufferTarget::ALBEDO_F0).view,
				ctx.gbuffer.GetColor(GBufferTarget::DIRECT_EMISSION).view,
				ctx.gbuffer.GetColor(GBufferTarget::VIEWZ).view,
				ctx.gbuffer.GetColor(GBufferTarget::NORMAL_ROUGHNESS).view,
				ctx.cameraUBO);
			ctx.composeDescriptorSetCached = true;
		}

		VkImage composeImgs[] = {
			ctx.gbuffer.GetColor(GBufferTarget::NRD_DIFF_OUT).image,
			ctx.gbuffer.GetColor(GBufferTarget::NRD_SPEC_OUT).image,
			ctx.gbuffer.GetColor(GBufferTarget::ALBEDO_F0).image,
			ctx.gbuffer.GetColor(GBufferTarget::DIRECT_EMISSION).image,
			ctx.gbuffer.GetColor(GBufferTarget::VIEWZ).image,
			ctx.gbuffer.GetColor(GBufferTarget::NORMAL_ROUGHNESS).image
		};
		VkImageMemoryBarrier composeBarriers[6] = {};
		for (int i = 0; i < 6; i++)
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
		                     0, nullptr, 0, nullptr, 6, composeBarriers);

		if (ctx.gpuProfiler)
			ctx.gpuProfiler->BeginRegion(cmd, GpuTimestampProfiler::Region::Compose, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
		ctx.composePass.Record(cmd, ctx.renderExtent);
		if (ctx.gpuProfiler)
			ctx.gpuProfiler->EndRegion(cmd, GpuTimestampProfiler::Region::Compose, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
		RT_LOG("[NRD] compose dispatched (%ux%u)", ctx.renderExtent.Width(), ctx.renderExtent.Height());
	}

	RT_LOG("[NRD] RecordNRDAndCompose end");
}

void FrameRenderer::RecordTonemapPass(VkCommandBuffer cmd, Context& ctx)
{
	if (!ctx.tonemapPass.IsAvailable()) return;

	VkImageMemoryBarrier linearReady = {};
	linearReady.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	linearReady.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	linearReady.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	linearReady.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	linearReady.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	linearReady.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	linearReady.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	linearReady.image = ctx.outputImage.image;
	linearReady.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	linearReady.subresourceRange.levelCount = 1;
	linearReady.subresourceRange.layerCount = 1;

	vkCmdPipelineBarrier(cmd,
		VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
		0, nullptr, 0, nullptr, 1, &linearReady);
	if (ctx.gpuProfiler)
		ctx.gpuProfiler->BeginRegion(cmd, GpuTimestampProfiler::Region::Tonemap, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	ctx.tonemapPass.Record(cmd, ctx.outputExtent);
	if (ctx.gpuProfiler)
		ctx.gpuProfiler->EndRegion(cmd, GpuTimestampProfiler::Region::Tonemap, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
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
	rtReadBarrier.image = ctx.displayImage.image;
	rtReadBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	rtReadBarrier.subresourceRange.levelCount = 1;
	rtReadBarrier.subresourceRange.layerCount = 1;

	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
	                     0, nullptr, 0, nullptr, 1, &rtReadBarrier);
}
