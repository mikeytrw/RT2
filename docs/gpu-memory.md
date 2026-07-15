# GPU Memory Layout

How geometry, materials, textures, and transient data are stored on the GPU.

---

## Architecture

Per-vertex indexed buffers — no per-triangle expansion. The closest-hit shader
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
BLAS acceleration structure ~400 MB  (DEVICE_LOCAL)
  +
Raster mega vertex buffer   ~varies  (DEVICE_LOCAL)

Total for 5.6M tris:        ~1.4 GB  (CPU + GPU combined)
```

---

## GPU Buffer Reference

### Per-Mesh Geometry Buffers (DEVICE_LOCAL)

| Buffer | Format | Created by | Read by | Notes |
|--------|--------|-----------|---------|-------|
| Vertex buffer | `vec4[]` (std430, 16-byte stride) | `AccelerationStructure` | BLAS build + closest-hit shader | Object-space positions, .xyz = pos, .w = 1.0 |
| Index buffer | `uint[]` (std430, 4-byte stride) | `AccelerationStructure` | BLAS build + closest-hit shader | Triangle indices |
| Normal buffer | `vec4[]` (std430, 16-byte stride) | `AccelerationStructure` | closest-hit shader | Object-space normals, .xyz = normal, .w = 0.0 (optional) |
| UV buffer | `vec4[]` (std430, 16-byte stride) | `AccelerationStructure` | closest-hit shader | Texture coords, .xy = UV, .zw = 0.0 (optional) |

All attribute buffers use **vec4 storage** (16-byte stride) to match std430
array stride without requiring `GL_EXT_scalar_block_layout`.

All per-mesh buffers are concatenated into mega-buffers with per-mesh offsets.
The closest-hit shader uses:
- `gl_InstanceID` → `instanceMeshInfo[gl_InstanceID]` → mesh buffer offsets
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
| Instance material index buffer | `uint32[]` (4 bytes each) | Per-triangle material index |
| Instance mesh offset buffer | `uvec4[]` (16 bytes each) | Per-instance: {vertexOffset, indexOffset, normalOffset, uvOffset} |
| Instance mat offset buffer | `uint[]` (4 bytes each) | Per-instance offset into material index buffer |

### Material & Light Buffers (HOST_VISIBLE)

| Buffer | Format | Purpose |
|--------|--------|---------|
| Material buffer | `GPUMaterial[]` (80 bytes each, std430) | PBR material parameters |
| Light buffer | 16-byte header + `GPUTriangleLight[]` (32 bytes each) | Emissive triangle list for NEE |

### Texture Array (Bindless)

| Resource | Format | Purpose |
|----------|--------|---------|
| Texture array | `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` (partially bound, variable count, max 4096) | All scene textures + env map |
| Env map CDF (marginal) | `R32_SFLOAT` texture | Row CDF for env map importance sampling |
| Env map CDF (conditional) | `R32_SFLOAT` texture | Per-row column CDF |
| Fallback texture | `R8G8B8A8_UNORM` 1x1 white | Used for missing/failed texture slots |

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
| gDiffRadiance | `R16G16B16A16_SFLOAT` | — (RT write) | NRD diffuse radiance input + normalized hitT |
| gSpecRadiance | `R16G16B16A16_SFLOAT` | — (RT write) | NRD specular radiance input + normalized hitT |
| gNrdDiffOut | `R16G16B16A16_SFLOAT` | — (NRD write) | NRD denoised diffuse output |
| gNrdSpecOut | `R16G16B16A16_SFLOAT` | — (NRD write) | NRD denoised specular output |
| Depth | `D32_SFLOAT` | — | Depth buffer for raster pass |

### ReSTIR DI Reservoir Buffers

| Buffer | Size | Purpose |
|--------|------|---------|
| Reservoir history | `width × height × 32 bytes` | Previous frame reservoirs (2× uvec4 per pixel) |
| Reservoir scratch | `width × height × 32 bytes` | Temporal output / spatial input |
| Surface history | `width × height × 32 bytes` | Temporal validation metadata (oct normal, viewZ, matID, worldPos, validity) |

### ReSTIR GI Buffer (binding 11)

One monolithic storage buffer with four 16-byte-aligned regions, owned by
`ReservoirGIResources`. 160 bytes per full-resolution pixel.

| Region | Size | Purpose |
|--------|------|---------|
| Reservoir A | `width × height × 48 bytes` | `SIGIReservoir` — one-bounce GI sample |
| Reservoir B | `width × height × 48 bytes` | Ping-pong pair of A |
| Receiver history prev | `width × height × 32 bytes` | `SISurfaceHistory` — previous-frame receiver metadata |
| Receiver history cur | `width × height × 32 bytes` | `SISurfaceHistory` — current-frame receiver metadata |

Reservoir A/B and receiver-history prev/cur ping-pong by frame parity. Two
history regions are required because the temporal dispatch reads previous
history while a separate `restir_gi_history.comp` dispatch writes current
history after all temporal reads finish — a single region would race.

While GI is disabled, a 16-byte dummy buffer is bound so the descriptor set
is legal without paying the full cost. A zeroed `SIGIReservoir` decodes as
invalid, so `vkCmdFillBuffer(..., 0)` clears produce invalid reservoirs.

### Camera & NRD UBOs

| Buffer | Size | Purpose |
|--------|------|---------|
| Camera UBO | 496 bytes | Position, directions, jitter, matrices (current + prev), viewport, SPP |
| NRD UBO | 16 bytes | NRD enabled flag, lobe dither mode, ReSTIR GI enabled flag, GI reservoir index |

---

## Memory Budget Analysis

For a large scene (San Miguel low-poly: 5.6M triangles, 17.5M vertices):

| Category | Size |
|----------|------|
| CPU: MeshRegistry | 622 MB |
| CPU: GPUSceneData | 400 MB (per-vertex normals/UVs as vec4) |
| GPU: Per-mesh vertex/index (DEVICE_LOCAL) | 533 MB (vec4 stride) |
| GPU: Per-mesh normal/UV (DEVICE_LOCAL) | 355 MB (vec4 stride) |
| GPU: BLAS structures (DEVICE_LOCAL) | 393 MB |
| GPU: Raster mega vertex (DEVICE_LOCAL) | ~400 MB |
| **Total HOST_VISIBLE (peak)** | **~70 MB** (one staging buffer at a time) |
| **Total DEVICE_LOCAL** | ~1681 MB |
| **Total peak (CPU+GPU)** | **~1.4 GB** |

HOST_VISIBLE memory usage is minimal — only transient staging buffers,
allocated and freed per upload. DEVICE_LOCAL memory is abundant on modern GPUs
(RTX 3090: 24 GB).

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

Textures use `AsyncTextureLoader` with a `StagingArena` (bump allocator) for
batched async upload via a dedicated command buffer + fence.

---

## Future: Shared Vertex Buffers

The raster pass currently builds its own "mega vertex buffer" (interleaved
`{vec3 pos, vec2 uv, vec3 tangent}`, per-triangle non-indexed). The RT path
uses indexed per-mesh buffers. These could be unified:

- Use the same indexed vertex/index buffers for both raster and RT
- Raster vertex shader fetches pos/uv from the indexed buffers (add a normal
  fetch too for the fragment shader)
- Eliminates the raster mega vertex buffer entirely (~400 MB saved)
- Requires updating `RasterPass` to use indexed draws + vertex buffer binding
