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
  │    ├─ E. ReSTIR DI temporal pass      [if ReSTIR DI enabled]
  │    ├─ F. ReSTIR DI spatial pass       [if ReSTIR DI enabled]
  │    ├─ G. ReSTIR GI temporal pass      [if ReSTIR GI enabled]
  │    │    - fresh candidate + temporal reuse (prev reservoir + prev
  │    │      receiver history → current reservoir)
  │    ├─ H. ReSTIR GI history-write pass [if ReSTIR GI enabled]
  │    │    - G-buffer → current receiver history (separate dispatch,
  │    │      runs after temporal reads finish to avoid history race)
  │    ├─ I. RT shading pass               [or G-buffer debug bypass]
  │    ├─ J. NRD denoise + compose          [if NRD enabled]
  │    ├─ K. Tonemap pass                   [separate compute dispatch]
  │    └─ L. Output image layout transition
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

vkCmdUpdateBuffer(m_NRDUBO, &nrdUniformData)  // SINRDUniformData, 16 bytes
  - nrdEnabled, lobeDither
  - restirGIEnabled, restirGIReservoirIndex  (spare fields repurposed for GI)
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
vkCmdBindVertexBuffers (indexed mega vertex buffer: position, UV, normal, tangent)
vkCmdBindIndexBuffer (uint32 mega index buffer)
vkCmdDrawIndexedIndirect (opaque draws, then masked draws)
vkCmdEndRendering
```

**Output**: 8 G-buffer images filled with primary-hit data. The rasterizer
handles primary visibility (cheaper than RT), jitter (vertex shader offsets
clip-space position), and motion vectors (reprojection of prev-world-pos).
OBJ import deduplicates position/normal/UV index tuples, and the raster pass
preserves mesh indices instead of expanding every triangle corner into a new
vertex. Primitive order remains unchanged so per-triangle material lookup via
`gl_PrimitiveID` still matches the ray-tracing buffers.

**Barrier after**: G-buffer images transition to SHADER_READ for RT pass.

## Step E: ReSTIR DI Temporal Pass

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

## Step F: ReSTIR DI Spatial Pass

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

## Step G: ReSTIR GI Temporal Pass

```
vkCmdBindPipeline (restir_gi_temporal.comp)
vkCmdBindDescriptorSets (set 0: scene + GI buffer, set 1: G-buffer)
vkCmdPushConstants (SIGIPushConstants: freshCandidateCount, temporalMCap,
                    maxTemporalAge, thresholds, frameIndex, jitter)
vkCmdDispatch ((width + 15) / 16, (height + 15) / 16, 1)
```

ReSTIR GI requires raster-first G-buffer but does NOT require ReSTIR DI.
The GI buffer (binding 11) holds four regions ping-ponging by frame parity:
reservoir A/B and receiver history prev/cur.

The temporal dispatch reads:
- G-buffer images (world pos, normal, viewZ, material, UV)
- previous reservoir region (last frame's GI samples)
- previous receiver history region (last frame's receiver metadata)
- TLAS via `GL_EXT_ray_query` (compute-stage ray queries for fresh
  candidate generation and history re-evaluation)

It writes:
- current reservoir region only (never touches current receiver history)

Each pixel generates `freshCandidateCount` cosine-weighted diffuse directions,
traces them with ray queries, evaluates `Lo` (environment miss / emissive hit
/ secondary NEE), and canonically merges with re-evaluated history.

**Barrier after**: current reservoir → SHADER_READ for RT pass; current
receiver history → SHADER_WRITE for the history-write dispatch.

## Step H: ReSTIR GI History-Write Pass

```
vkCmdBindPipeline (restir_gi_history.comp)
vkCmdBindDescriptorSets (set 0, set 1)
vkCmdPushConstants (same SIGIPushConstants)
vkCmdDispatch ((width + 15) / 16, (height + 15) / 16, 1)
```

A separate lightweight dispatch writes the current receiver history region
from G-buffer data only — no ray queries, no reads of other pixels' history.
It runs AFTER the temporal dispatch finishes to avoid a read/write race on
the shared receiver-history region. The temporal dispatch reads previous
history; this dispatch writes current history.

**Barrier after**: current receiver history → SHADER_READ for next frame's
temporal dispatch.

## Step I: RT Shading Pass

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
1. Fetch vertex attributes via indexed buffers (position, UV) using
   `gl_PrimitiveID` → index buffer → vertex buffer
2. Transform to world space via instance transform matrix
3. Sample material textures (base color, metallicRoughness, normal, emissive)
4. Evaluate GGX BRDF + NEE (with optional ReSTIR DI from reservoirHistory)
5. Trace shadow ray (shadow.rmiss / shadow.rahit)
6. Russian roulette + recursive traceRayEXT (up to maxBounces)

Conventional NEE samples either the emissive-triangle family or the environment
family. Its proposal PDF includes that family-selection probability. NEE
evaluates the diffuse BRDF only, so ordinary MIS competes against
`P_diffuse * pdfDiffuse`; specular and transmission continuations retain their
full terminal contribution. The raster-first NRD joint-lobe estimator instead
competes with its full mixture PDF and carries the diffuse MIS weight separately
so the reconstructed specular signal is not attenuated.

When ReSTIR DI is enabled, its reservoir is the complete primary diffuse direct
estimator. BSDF-sampled primary diffuse hits on both the environment and
emissive triangles are suppressed, while primary specular transport and all
later-bounce conventional NEE remain active.

**ReSTIR GI consumption**: when the primary scatter selects the diffuse lobe
and `restirGIEnabled` is set, `secondary_raygen.rgen` loads the current GI
reservoir and uses its stored `Lo` / `hitT` / `W` directly — no GI retrace is
issued. The contribution is divided by the diffuse-lobe selection probability
to preserve the one-lobe mixture estimator. Specular/transmission lobes and
invalid-reservoir fallbacks use the existing recursive BSDF path.

**Output** (raster-first path): writes to G-buffer diff/spec radiance images
(for NRD) + output image (beauty, linear HDR). Emissive pixels bypass NRD.

**Barrier after**: output image → SHADER_READ for compose/tonemap; G-buffer
diff/spec → SHADER_READ for NRD.

## Step J: NRD Denoise + Compose

### NRD

```
NRDWrapper::Denoise(cmd, ...)
  - SetCommonSettings (viewToClip, prevViewToClip, worldToView, prevWorldToView,
    jitter, frameIndex, reset flag)
  - SetReblurSettings (blur radius, accum frames, anti-firefly)
  - Denoise: dispatches multiple compute passes internally
```

NRD reads: gNormalRoughness, gViewZ, gMotion, gDiffRadiance, gSpecRadiance
Writes: denoised diffuse + specular radiance (NRD_DIFF_OUT, NRD_SPEC_OUT)

Diffuse/specular radiance is demodulated and remodulated with the same NRD
material factors. The minimum factor is `0.02`, matching the vendored NRD
default; the existing demodulated anti-firefly safety clamp remains `100.0`.

### Compose

```
vkCmdBindPipeline (compose.comp)
vkCmdBindDescriptorSets
vkCmdDispatch (width / 8, height / 8, 1)
```

Compose reads NRD's denoised output, remodulates with material albedo/F0
(removed before denoising to reduce noise), adds direct emission, and writes
linear HDR color to the output image. Tone mapping is NOT done here — it is
a separate pass (Step K).

## Step K: Tonemap Pass

```
vkCmdBindPipeline (tonemap.comp)
vkCmdDispatch (width / 8, height / 8, 1)
```

A dedicated compute dispatch reads the linear HDR output image and writes
Reinhard-tone-mapped, exact sRGB-encoded color to the UNORM display image.
Encoding explicitly is required because the display image is a storage image;
the ImGui/swapchain path expects nonlinear sRGB byte values. The CPU headless
readback uses the same Reinhard and piecewise sRGB transfer. Splitting tonemap from
compose keeps the linear output available for debug views and future
upscalers (FSR 2 / DLSS) that operate on linear HDR input.

## Step L: Output Transition

```
vkCmdPipelineBarrier:
  display image: SHADER_WRITE → SHADER_READ (for ImGui viewport display)
```

---

## GPU Timestamp Regions

Profiling via `GpuTimestampProfiler` with timestamp queries at region
boundaries:

| Region | What it measures |
|--------|-----------------|
| Frame | Total GPU frame time |
| Raster | G-buffer pass (vertex + fragment) |
| ReSTIR DI Temporal | DI candidate generation and temporal reuse |
| ReSTIR DI Spatial | DI spatial neighbor reuse |
| ReSTIR GI Temporal | GI fresh candidates, ray-query evaluation, and temporal re-evaluation |
| ReSTIR GI History | GI receiver-history write dispatch |
| RTShading | Ray tracing dispatch (secondary rays + bounces) |
| NRD | NRD denoise passes |
| Compose | Compose compute dispatch |
| Tonemap | Linear HDR to display-image compute dispatch |

---

## G-buffer Debug Mode

When `gbufferDebugMode >= 0`, `gbuffer_debug.comp` visualizes a selected
G-buffer channel, ReSTIR DI/GI reservoir, or packed NRD input directly to the
output image. Two execution paths exist:

- **Modes 0-18** (G-buffer + ReSTIR reservoirs): the pipeline runs Raster →
  ReSTIR DI (temporal + spatial) → ReSTIR GI (temporal + history) → debug
  dispatch. ReSTIR runs before the debug dispatch because the reservoir debug
  views need reservoirs to exist. This path replaces RT shading + NRD +
  Compose + Tonemap.

- **Modes 19-21** (packed NRD inputs): the pipeline runs Raster → ReSTIR →
  RT shading → debug dispatch. These modes inspect diff/spec radiance after
  the RT dispatch writes them, so they require the RT pass to run first.
  Replaces NRD + Compose + Tonemap.

Mode index reference (defined in `gbuffer_debug.comp`):

| Mode | View |
|---|---|
| 0-10 | G-buffer channels: normal, roughness, viewZ, motion, albedo, F0, direct emission, world pos, geo normal, UV, material index |
| 11 | ReSTIR DI reservoir |
| 12-18 | ReSTIR GI: direction, Lo, hitT, M/Age, fresh vs history, validity, weight W |
| 19-20 | Packed NRD diffuse / specular input |
| 21 | ReSTIR DI normalization weight W |

---

## Non-NRD Path (Temporal Accumulation)

When NRD is disabled, the RT shading pass writes directly to the output image
using temporal accumulation (frame-blended exponential moving average). This
is the "raw" path tracer output — noisy but unbiased. Reset on camera move.
