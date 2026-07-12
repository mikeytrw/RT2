# GPU Memory Layout

How geometry, materials, textures, and transient data are stored on the GPU.
Includes the current architecture (pre-refactor) and the target architecture
(post-refactor) that eliminates the combined buffer duplication.

---

## Current Architecture (Pre-Refactor) — The Problem

The same geometry data exists in **6 simultaneous copies** during scene load:

```
MeshRegistry (CPU)          ~622 MB  (vertices, indices, normals, UVs)
  ↓ copied
GPUSceneData (CPU)          ~613 MB  (vertices, indices, per-tri UVs, per-tri tangents)
  ↓ copied
SceneResources (CPU mirror) ~613 MB  (same, kept for transform-only updates)
  ↓ copied to GPU
BLAS vertex/index buffers   ~137 MB  (HOST_VISIBLE, per-BLAS)
BLAS per-tri data (CPU)     ~538 MB  (triPositions, triUVs, triTangents — transient)
  ↓ copied to GPU
Combined GPU buffers        ~856 MB  (DEVICE_LOCAL: vec4 normals/positions/UVs/tangents)
  +
Raster mega vertex buffer   ~varies  (DEVICE_LOCAL: per-tri non-indexed, interleaved)

Total peak for 5.6M tris:   ~3.4 GB  (CPU + GPU combined)
```

The combined buffers alone (856 MB) require allocation on the GPU, and the
CPU-side staging vectors to fill them add another 856 MB of peak RAM. The
HOST_VISIBLE heap pressure comes from the BLAS vertex/index buffers (137 MB)
plus the staging buffers used during upload. On many GPUs the HOST_VISIBLE
heap is limited, and the peak CPU-side allocation for the combined buffer
staging vectors (~856 MB) can cause OOM on large scenes.

---

## Target Architecture (Post-Refactor)

Eliminate all per-triangle expansion. Store per-vertex attributes in indexed
buffers (exactly like a vertex shader would use). The closest-hit shader
fetches attributes via `gl_PrimitiveID` → index buffer → vertex buffer.

```
MeshRegistry (CPU)          ~622 MB  (vertices, indices, normals, UVs — canonical source)
  ↓ copied once
GPUSceneData (CPU)          ~400 MB  (vertices, indices, normals, UVs — no per-tri expansion)
  ↓ uploaded to GPU
Per-mesh GPU buffers:       ~533 MB  (DEVICE_LOCAL, vec4 stride)
  ├─ vertexBuffer    (vec4, 16-byte stride, indexed)
  ├─ indexBuffer     (uint32)
  ├─ normalBuffer    (vec4, 16-byte stride, indexed)  [if present]
  └─ uvBuffer        (vec4, 16-byte stride, indexed)   [if present]
  +
BLAS acceleration structure ~400 MB  (DEVICE_LOCAL — same as before)
  +
Raster mega vertex buffer   ~varies  (DEVICE_LOCAL — same, but can share vertex data)

Total for 5.6M tris:        ~1.4 GB  (CPU + GPU combined)
```

**65% memory reduction**. No per-triangle data. No combined buffers. No
peak CPU-side staging vector allocation. HOST_VISIBLE usage drops to just
the BLAS vertex/index buffers (or zero if those are also moved to DEVICE_LOCAL
with staging upload).

---

## GPU Buffer Reference (Post-Refactor)

### Per-Mesh Geometry Buffers (DEVICE_LOCAL)

| Buffer | Format | Created by | Read by | Notes |
|--------|--------|-----------|---------|-------|
| Vertex buffer | `vec4[]` (std430, 16-byte stride) | `AccelerationStructure` | BLAS build + closest-hit shader | Object-space positions, .xyz = pos, .w = 1.0 |
| Index buffer | `uint[]` (std430, 4-byte stride) | `AccelerationStructure` | BLAS build + closest-hit shader | Triangle indices |
| Normal buffer | `vec4[]` (std430, 16-byte stride) | `AccelerationStructure` | closest-hit shader | Object-space normals, .xyz = normal, .w = 0.0 (optional) |
| UV buffer | `vec4[]` (std430, 16-byte stride) | `AccelerationStructure` | closest-hit shader | Texture coords, .xy = UV, .zw = 0.0 (optional) |

All attribute buffers use **vec4 storage** (16-byte stride) to match std430
array stride without requiring `GL_EXT_scalar_block_layout`. See
`docs/refactor-plan.md` Phase 3.0 for the ABI decision rationale.

All per-mesh buffers are in a single large allocation (or concatenated into
mega-buffers with per-mesh offsets). The closest-hit shader uses:
- `gl_InstanceID` → instance-to-mesh lookup table → mesh buffer offsets
- `gl_PrimitiveID` → index buffer → vertex indices → fetch from vertex/normal/UV buffers

### Acceleration Structures (DEVICE_LOCAL)

| Buffer | Created by | Notes |
|--------|-----------|-------|
| BLAS buffer | `AccelerationStructure::BuildBLASes` | One per unique mesh, `VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR` |
| BLAS scratch | `AccelerationStructure::BuildBLASes` | Transient, freed after build |
| TLAS buffer | `AccelerationStructure::BuildTLAS` | One per scene, rebuilt when instances change |

### Per-Instance Buffers (HOST_VISIBLE)

| Buffer | Format | Purpose |
|--------|--------|---------|
| Instance transform buffer | `mat4[]` (64 bytes each) | Current-frame world matrices |
| Instance transform prev buffer | `mat4[]` (64 bytes each) | Previous-frame (for motion vectors) |
| Instance material index buffer | `uint32[]` (4 bytes each) | Per-instance material override |
| Instance mesh offset buffer | `uvec4[]` (16 bytes each) | Per-instance: {vertexOffset, indexOffset, normalOffset, uvOffset} — maps gl_InstanceID to buffer regions |

### Material & Light Buffers (HOST_VISIBLE)

| Buffer | Format | Purpose |
|--------|--------|---------|
| Material buffer | `GPUMaterial[]` (80 bytes each, std430) | PBR material parameters |
| Light buffer | 16-byte header + `GPUTriangleLight[]` (32 bytes each) | Emissive triangle list for NEE |

### Texture Array (Bindless)

| Resource | Format | Purpose |
|----------|--------|---------|
| Texture array | `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` (partially bound, max 1000) | All scene textures + env map |
| Env map CDF (marginal) | `R32_SFLOAT` texture | Row CDF for env map importance sampling |
| Env map CDF (conditional) | `R32_SFLOAT` texture | Per-row column CDF |

### G-Buffer Images

| Image | Format | Attachment | Contents |
|-------|--------|-----------|----------|
| gNormalRoughness | `A2B10G10R10_UNORM` | 0 | NRD oct-packed normal + linear roughness |
| gViewZ | `R32_SFLOAT` | 1 | View-space Z |
| gMotion | `R16G16_SFLOAT` | 2 | Screen-space motion vector (UV delta) |
| gAlbedoF0 | `R16G16B16A16_SFLOAT` | 3 | Demodulated albedo (rgb) + metallic (a) |
| gDirectEmission | `R16G16B16A16_SFLOAT` | 4 | Direct emission (bypasses NRD) |
| gPrimHit | `R32G32B32A32_SFLOAT` | 5 | World position (xyz) + packed material index (w) |
| gPrimGeoNormal | `R8G8B8A8_UNORM` | 6 | Geometric normal (0.5+0.5 encode) |
| gPrimUV | `R16G16_SFLOAT` | 7 | UV at primary hit |
| Depth | `D32_SFLOAT` | — | Depth buffer for raster pass |

### ReSTIR Reservoir Buffers

| Buffer | Size | Purpose |
|--------|------|---------|
| Reservoir history | `width × height × 32 bytes` | Previous frame reservoirs (2× uvec4 per pixel) |
| Reservoir scratch | `width × height × 32 bytes` | Temporal output / spatial input |
| Surface history | `width × height × 16 bytes` | Temporal validation metadata (oct normal, viewZ, matID, validity) |

### Camera & NRD UBOs

| Buffer | Size | Purpose |
|--------|------|---------|
| Camera UBO | 496 bytes | Position, directions, jitter, matrices (current + prev), viewport, SPP |
| NRD UBO | 16 bytes | NRD enabled flag, lobe dither mode |

---

## Memory Budget Analysis

For a large scene (San Miguel low-poly: 5.6M triangles, 17.5M vertices):

| Category | Pre-Refactor | Post-Refactor |
|----------|-------------|---------------|
| CPU: MeshRegistry | 622 MB | 622 MB (unchanged) |
| CPU: GPUSceneData | 613 MB | 400 MB (no per-tri expansion; per-vertex normals/UVs as vec4) |
| CPU: BLAS per-tri (transient) | 538 MB | 0 (eliminated) |
| CPU: Combined buffer staging | 856 MB | 0 (eliminated) |
| GPU: Per-mesh vertex/index | 137 MB (HOST_VISIBLE) | 533 MB (DEVICE_LOCAL, vec4 stride) |
| GPU: Per-mesh normal/UV | 0 | 355 MB (DEVICE_LOCAL, vec4 stride) |
| GPU: Combined buffers (DEVICE_LOCAL) | 856 MB | 0 (eliminated) |
| GPU: BLAS structures | 393 MB (DEVICE_LOCAL) | 393 MB (unchanged) |
| GPU: Raster mega vertex | ~400 MB (DEVICE_LOCAL) | ~400 MB (or share with RT) |
| **Total HOST_VISIBLE (peak)** | **~993 MB** | **~70 MB** (one staging buffer at a time) |
| **Total DEVICE_LOCAL** | ~1649 MB | ~1681 MB |
| **Total peak (CPU+GPU)** | **~3.4 GB** | **~1.4 GB** |

The key win: HOST_VISIBLE memory usage drops from ~1 GB to ~0 (only transient
staging buffers, allocated and freed per upload). DEVICE_LOCAL memory is
abundant on modern GPUs (RTX 3090: 24 GB).

---

## Upload Strategy

All DEVICE_LOCAL geometry buffers use a **chunked staging upload** to keep
peak HOST_VISIBLE memory low:

```
For each mesh (or bounded chunk of a mega-mesh):
  1. Create one staging buffer (HOST_VISIBLE, sized for this mesh's data)
  2. Map + fill vertex data (vec4 per vertex)
  3. vkCmdCopyBuffer (staging → DEVICE_LOCAL vertex mega-buffer region)
  4. Map + fill index data, copy
  5. Map + fill normal data (if present), copy
  6. Map + fill UV data (if present), copy
  7. Destroy staging buffer
  → Peak HOST_VISIBLE per iteration = one staging buffer (typically 10-70 MB)
```

This avoids recreating the OOM failure mode where all expanded data was
built in CPU vectors before any staging upload began. CPU-side `GPUSceneData`
vectors are direct per-vertex copies (no per-triangle expansion), so they
are small enough to keep in memory alongside the MeshRegistry.

Uses `CommandUtils::ImmediateSubmit` for one-off copies.

Textures use `AsyncTextureLoader` with a `StagingArena` (bump allocator) for
batched async upload via a dedicated command buffer + fence.

---

## Future: Shared Vertex Buffers

The raster pass currently builds its own "mega vertex buffer" (interleaved
`{vec3 pos, vec2 uv, vec3 tangent}`, per-triangle non-indexed). Post-refactor,
the RT path uses indexed per-mesh buffers. These could be unified:

- Use the same indexed vertex/index buffers for both raster and RT
- Raster vertex shader fetches pos/uv from the indexed buffers (add a normal
  fetch too for the fragment shader)
- Eliminates the raster mega vertex buffer entirely (~400 MB saved)
- Requires updating `RasterPass` to use indexed draws + vertex buffer binding

This is a future optimization, not part of the initial refactor.