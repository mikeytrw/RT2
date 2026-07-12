# Rendering Pipeline

The GPU frame pipeline — what happens inside `RendererGPU::Render()` and
`FrameRenderer::RecordFrame()`. One render submission per frame, async
(non-blocking), 2 frames in flight.

---

## Frame Overview

```
RendererGPU::Render(camera)
  ├─ 0. Pre-frame: dirty check, AS rebuild, texture poll, camera UBO update
  ├─ 1. Wait for frame slot fence (frames-in-flight ring)
  ├─ 2. Begin command buffer
  ├─ 3. FrameRenderer::RecordFrame(cmd, ctx)
  │    ├─ A. Top-of-frame barrier + AS barrier
  │    ├─ B. UBO updates (camera + NRD via vkCmdUpdateBuffer)
  │    ├─ C. Transform buffer advance (current ← prev)
  │    ├─ D. Raster G-buffer pass (or G-buffer debug)
  │    ├─ E. ReSTIR temporal pass      [if ReSTIR enabled]
  │    ├─ F. ReSTIR spatial pass       [if ReSTIR enabled]
  │    ├─ G. RT shading pass            [or G-buffer debug bypass]
  │    ├─ H. NRD denoise + compose      [if NRD enabled]
  │    └─ I. Output image layout transition
  └─ 4. Submit (fence signaled, non-blocking)
```

---

## Step A: Barriers

```
vkCmdPipelineBarrier:
  - Top-of-pipe → Transfer stage (camera UBO writable)
  - Transfer → Vertex shader (instance transforms readable)
  - Acceleration structure build barrier (TLAS readable after build)
```

## Step B: UBO Updates

```
vkCmdUpdateBuffer(m_CameraUBO, &m_CameraUBOData)
  - position, forward, right, up (jitter in .w components)
  - viewportSPP (width, height, spp, maxBounces)
  - inverseProjection, inverseView, viewToClip, viewToClipPrev
  - worldToView, worldToViewPrev

vkCmdUpdateBuffer(m_NRDUBO, &nrdUniformData)
  - nrdEnabled, lobeDither mode
```

## Step C: Transform Buffer Advance

If NRD or motion vectors are enabled, the current frame's instance transform
buffer is copied to the previous-frame buffer:

```
vkCmdCopyBuffer(currentTransforms → prevTransforms)
```

Then the current transform buffer is updated with new world matrices from the
ECS (via `SceneManager::SyncTransformsToGPU` if any transforms changed).

## Step D: Raster G-buffer Pass

```
vkCmdBeginRendering (dynamic rendering, 8 color attachments + depth)
  - Color 0: gNormalRoughness  (A2B10G10R10_UNORM — NRD oct-packed)
  - Color 1: gViewZ             (R32_SFLOAT)
  - Color 2: gMotion            (R16G16_SFLOAT)
  - Color 3: gAlbedoF0          (R16G16B16A16_SFLOAT)
  - Color 4: gDirectEmission    (R16G16B16A16_SFLOAT)
  - Color 5: gPrimHit           (R32G32B32A32_SFLOAT — world pos + matIdx)
  - Color 6: gPrimGeoNormal     (R8G8B8A8_UNORM)
  - Color 7: gPrimUV            (R16G16_SFLOAT)
  - Depth:   D32_SFLOAT

vkCmdBindPipeline (opaque or masked)
vkCmdBindDescriptorSets (set 0: scene, set 1: G-buffer)
vkCmdBindVertexBuffers (mega vertex buffer: {vec3 pos, vec2 uv, vec3 tangent})
vkCmdDrawIndirect (opaque draws, then masked draws)
vkCmdEndRendering
```

**Output**: 8 G-buffer images filled with primary-hit data. The rasterizer
handles primary visibility (cheaper than RT), jitter (vertex shader offsets
clip-space position), and motion vectors (reprojection of prev-world-pos).

**Barrier after**: G-buffer images transition to SHADER_READ for RT pass.

## Step E: ReSTIR Temporal Pass

```
vkCmdBindPipeline (restir_temporal.comp)
vkCmdBindDescriptorSets (set 0: scene + reservoirs)
vkCmdPushConstants (SIReSTIRPushConstants: candidate count, M cap, thresholds)
vkCmdDispatch ((width + 15) / 16, (height + 15) / 16, 1)
```

All compute passes use `local_size_x = 16, local_size_y = 16`.

Fused initial candidate generation + temporal reuse. Reads from:
- G-buffer images (world pos, normal, viewZ, material)
- `reservoirHistory` (previous frame's reservoirs)
- `surfaceHistory` (previous frame's surface metadata for validation)

Writes to:
- `reservoirScratch` (this frame's temporal output)

**Barrier after**: reservoirScratch → SHADER_READ for spatial pass.

## Step F: ReSTIR Spatial Pass

```
vkCmdBindPipeline (restir_spatial.comp)
vkCmdBindDescriptorSets (set 0: scene + reservoirs)
vkCmdPushConstants (same push constants)
vkCmdDispatch ((width + 15) / 16, (height + 15) / 16, 1)
```

Spatial neighbor reuse. Reads from:
- `reservoirScratch` (temporal output)
- G-buffer images (for neighbor compatibility checks)

Writes to:
- `reservoirHistory` (final reservoirs for this frame — used by RT shading)
- `surfaceHistory` (current surface metadata for next frame's temporal)

**Barrier after**: reservoirHistory → SHADER_READ for RT pass.

## Step G: RT Shading Pass

```
vkCmdBindPipeline (VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR)
vkCmdBindDescriptorSets (set 0: scene, set 1: G-buffer)
vkCmdTraceRaysKHR (SBT regions, width, height, 1)
```

Two raygen modes (selected by SBT offset):
- **secondary_raygen.rgen** (raster-first, default): reads G-buffer for
  primary hit data, traces only secondary rays for lighting
- **raygen.rgen** (RT-primary): traces full camera rays, no G-buffer read

**Closest-hit shader** does:
1. Fetch vertex attributes (position, UV, tangent) via combined buffers
   *(post-refactor: via per-mesh vertex/index buffers + gl_PrimitiveID)*
2. Transform to world space via instance transform matrix
3. Sample material textures (base color, metallicRoughness, normal, emissive)
4. Evaluate GGX BRDF + NEE (with optional ReSTIR DI from reservoirHistory)
5. Trace shadow ray (shadow.rmiss / shadow.rahit)
6. Russian roulette + recursive traceRayEXT (up to maxBounces)

**Output** (raster-first path): writes to G-buffer diff/spec radiance images
(for NRD) + output image (beauty). Emissive pixels bypass NRD.

**Barrier after**: output image → SHADER_READ for compose; G-buffer diff/spec
→ SHADER_READ for NRD.

## Step H: NRD Denoise + Compose

### NRD

```
NRDWrapper::Denoise(cmd, ...)
  - SetCommonSettings (viewToClip, prevViewToClip, worldToView, prevWorldToView,
    jitter, frameIndex, reset flag)
  - SetReblurSettings (blur radius, accum frames, anti-firefly)
  - Denoise: dispatches multiple compute passes internally
```

NRD reads: gNormalRoughness, gViewZ, gMotion, gDiffRadiance, gSpecRadiance
Writes: denoised diffuse + specular radiance

### Compose

```
vkCmdBindPipeline (compose.comp)
vkCmdBindDescriptorSets
vkCmdDispatch (width / 8, height / 8, 1)
```

Compose reads NRD's denoised output, remodulates with material albedo/F0
(removed before denoising to reduce noise), adds direct emission, applies
tone mapping, writes final color to output image.

## Step I: Output Transition

```
vkCmdPipelineBarrier:
  output image: SHADER_WRITE → SHADER_READ (for ImGui viewport display)
```

---

## GPU Timestamp Regions

Profiling via `GpuTimestampProfiler` with timestamp queries at region
boundaries:

| Region | What it measures |
|--------|-----------------|
| Frame | Total GPU frame time |
| Raster | G-buffer pass (vertex + fragment) |
| ReSTIRTemporal | Temporal candidate + reuse dispatch |
| ReSTIRSpatial | Spatial neighbor reuse dispatch |
| RTShading | Ray tracing dispatch (secondary rays + bounces) |
| NRD | NRD denoise passes |
| Compose | Compose compute dispatch |

---

## G-buffer Debug Mode

When `gbufferDebugMode >= 0`, the pipeline runs Raster → ReSTIR (temporal +
spatial) → G-buffer debug dispatch. ReSTIR runs before the debug dispatch
because the ReSTIR reservoir debug view needs reservoirs to exist. The debug
pass replaces the RT shading + NRD + Compose stages — `gbuffer_debug.comp`
visualizes a selected G-buffer channel (or ReSTIR reservoir data) directly
to the output image.

---

## Non-NRD Path (Temporal Accumulation)

When NRD is disabled, the RT shading pass writes directly to the output image
using temporal accumulation (frame-blended exponential moving average). This
is the "raw" path tracer output — noisy but unbiased. Reset on camera move.