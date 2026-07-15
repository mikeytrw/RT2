# Shader Bindings

Descriptor set layout reference for the path tracer and compute passes.
Constants are defined in `shader_interface.h` (shared between C++ and GLSL).

---

## Set 0 — Scene-Global (Path Tracer + Compute Passes)

Owned by `PathTracePass`. Bound for RT dispatches and compute passes
(ReSTIR, compose, G-buffer debug). Uses a dedicated descriptor pool with
`VARIABLE_DESCRIPTOR_COUNT` for the texture array at binding 18.

| # | Constant | Type | Points to |
|---|----------|------|-----------|
| 0 | `SI_BINDING_OUTPUT_IMAGE` | Storage image (rgba32f) | Output beauty image |
| 1 | `SI_BINDING_CAMERA_UBO` | Uniform buffer | Camera data (496 bytes) |
| 2 | `SI_BINDING_MATERIAL_BUFFER` | Storage buffer (readonly) | `GPUMaterial[]` |
| 3 | `SI_BINDING_VERTEX_BUFFER` | Storage buffer (readonly) | `vec4 vertices[]` (per-mesh, concatenated, .xyz = pos, .w = 1.0) |
| 4 | `SI_BINDING_TLAS` | Acceleration structure | TLAS handle |
| 5 | `SI_BINDING_INDEX_BUFFER` | Storage buffer (readonly) | `uint indices[]` (per-mesh, concatenated) |
| 6 | `SI_BINDING_NORMAL_BUFFER` | Storage buffer (readonly) | `vec4 normals[]` (per-mesh, .xyz = normal, .w = 0.0) |
| 7 | `SI_BINDING_UV_BUFFER` | Storage buffer (readonly) | `vec4 uvs[]` (per-mesh, .xy = UV, .zw = 0.0) |
| 8 | `SI_BINDING_INSTANCE_MESH_INFO` | Storage buffer (readonly) | `uvec4 instanceMeshInfo[]` (per instance: {vertexOffset, indexOffset, normalOffset, uvOffset}) |
| 9 | `SI_BINDING_LIGHT_BUFFER` | Storage buffer (readonly) | 16-byte header + `TriangleLight[]` |
| 10 | `SI_BINDING_INSTANCE_TRANSFORMS` | Storage buffer (readonly) | `mat4 instanceTransforms[]` |
| 11 | `SI_BINDING_GI_DATA` | Storage buffer | ReSTIR GI monolithic buffer (4 regions: reservoir A/B + receiver history prev/cur — see ReSTIR GI section below) |
| 12 | `SI_BINDING_INSTANCE_TRANSFORMS_PREV` | Storage buffer (readonly) | `mat4 instanceTransformsPrev[]` |
| 13 | `SI_BINDING_INSTANCE_MATERIAL_INDICES` | Storage buffer (readonly) | `uint instanceMaterialIndices[]` (per-triangle) |
| 14 | `SI_BINDING_RESERVOIR_HISTORY` | Storage buffer | ReSTIR reservoir history (32 bytes/pixel) |
| 15 | `SI_BINDING_RESERVOIR_SCRATCH` | Storage buffer | ReSTIR reservoir scratch (32 bytes/pixel) |
| 16 | `SI_BINDING_SURFACE_HISTORY` | Storage buffer | ReSTIR surface history (32 bytes/pixel: oct normal, viewZ, matID, worldPos, validity) |
| 17 | `SI_BINDING_INSTANCE_MAT_OFFSETS` | Storage buffer (readonly) | `uint instanceMatOffsets[]` (per-instance offset into material index buffer) |
| 18 | `SI_BINDING_TEXTURE_ARRAY` | Combined image samplers (partially bound, variable count, max 4096) | Bindless texture array |

**ABI contract**: All attribute buffers use `vec4` (16-byte stride) in std430
layout. This avoids the scalar-layout alignment pitfall where `vec3` in
std430 has a 16-byte array stride but C++ `sizeof(vec3)` is 12 bytes. Using
vec4 with explicit padding ensures C++ and GLSL agree on layout without
requiring `GL_EXT_scalar_block_layout`.

**TextureSlot contract**: Every texture slot has a descriptor — no skipping,
no compaction. Missing/failed textures use a 1x1 white fallback. The env map
stays at its original index in the texture array (no extraction/re-append).
CDF textures are appended after all scene textures.

**Compute stage visibility**: The TLAS descriptor (binding 4) and scene
attribute buffers are visible to `VK_SHADER_STAGE_COMPUTE_BIT` in addition to
ray-tracing stages, so ReSTIR DI/GI compute passes can perform ray queries
against the existing TLAS.

---

## Set 0 — ReSTIR GI Monolithic Buffer (Binding 11)

`SI_BINDING_GI_DATA` is one storage buffer with four 16-byte-aligned regions,
owned by `ReservoirGIResources`. Reservoir A/B and receiver-history prev/cur
ping-pong by frame parity (`giFrameIndex & 1` selects current, `^1` selects
previous).

| Region | Size | Contents |
|---|---|---|
| reservoir A | `pixelCount × 48 bytes` | `SIGIReservoir` — one-bounce GI sample |
| reservoir B | `pixelCount × 48 bytes` | `SIGIReservoir` — ping-pong pair |
| receiver history prev | `pixelCount × 32 bytes` | `SISurfaceHistory` — previous-frame receiver metadata |
| receiver history cur | `pixelCount × 32 bytes` | `SISurfaceHistory` — current-frame receiver metadata |

**160 bytes per full-resolution pixel** (2×48 reservoir + 2×32 history).
1080p ≈ 316.4 MiB; 4K ≈ 1.24 GiB. Two receiver-history regions are required
because the temporal dispatch reads previous history while a separate
history-write dispatch (`restir_gi_history.comp`) writes current history
after all temporal reads finish — this avoids a read/write race that a single
combined dispatch would create.

A zeroed `SIGIReservoir` decodes as invalid (`flags bit 0 == 0`, `M == 0`),
so `vkCmdFillBuffer(..., 0)` clears produce invalid reservoirs by construction.
While GI is disabled, `ReservoirGIResources::CreateDummy` allocates a 16-byte
buffer so binding 11 points at a legal descriptor without paying the full cost.

---

## Set 1 — G-Buffer + NRD

Owned by `RendererGPU`. Bound for the raster-first raygen pass, NRD, compose,
and G-buffer debug.

| # | Constant | Type | Points to |
|---|----------|------|-----------|
| 0 | `SI_BINDING_G_NORMAL_ROUGHNESS` | Storage image (rgb10_a2) | NRD oct-packed normal + roughness |
| 1 | `SI_BINDING_G_VIEWZ` | Storage image (r32f) | View-space Z |
| 2 | `SI_BINDING_G_MOTION` | Storage image (rg16f) | Screen-space motion vector |
| 3 | `SI_BINDING_G_DIFF_RADIANCE` | Storage image (rgba16f) | Diffuse radiance + hitT (NRD input) |
| 4 | `SI_BINDING_G_SPEC_RADIANCE` | Storage image (rgba16f) | Specular radiance + hitT (NRD input) |
| 5 | `SI_BINDING_G_ALBEDO_F0` | Storage image (rgba16f) | Demodulated albedo + F0 |
| 6 | `SI_BINDING_NRD_UBO` | Uniform buffer | NRD uniform data (`SINRDUniformData`, 16 bytes — see below) |
| 7 | `SI_BINDING_G_DIRECT_EMISSION` | Storage image (rgba16f) | Direct emission (bypasses NRD) |
| 8 | `SI_BINDING_G_PRIM_HIT` | Storage image (rgba32f) | World pos (xyz) + packed matIdx (w) |
| 9 | `SI_BINDING_G_PRIM_GEO_NORMAL` | Storage image (rgba8) | Geometric normal (0.5+0.5 encode) |
| 10 | `SI_BINDING_G_PRIM_UV` | Storage image (rg16f) | UV at primary hit |

---

## Closest-Hit Attribute Fetch

The closest-hit shader fetches vertex attributes via the index buffer:

```glsl
uvec4 meshInfo = instanceMeshInfo[gl_InstanceID];
uint idxOffset = meshInfo.y + gl_PrimitiveID * 3;
uint i0 = indices[idxOffset + 0];
uint i1 = indices[idxOffset + 1];
uint i2 = indices[idxOffset + 2];

uint vertOffset = meshInfo.x;
vec3 pos0 = vertices[vertOffset + i0].xyz;  // .w = 1.0 (padding)
vec3 pos1 = vertices[vertOffset + i1].xyz;
vec3 pos2 = vertices[vertOffset + i2].xyz;

uint normOffset = meshInfo.z;
vec3 nrm0 = normals[normOffset + i0].xyz;  // .w = 0.0 (padding)

uint uvOffset = meshInfo.w;
vec2 uv0 = uvs[uvOffset + i0].xy;  // .zw = 0.0 (padding)

// Tangent computed from UV gradients (no buffer needed)
vec3 tangent = computeTangentFromUV(pos0, pos1, pos2, uv0, uv1, uv2);
```

The `instanceMeshInfo` buffer (binding 8) provides per-instance offsets into
the concatenated vertex/index/normal/UV buffers.

---

## ReSTIR GI Reservoir (`SIGIReservoir`, 48 bytes)

Per-pixel one-bounce GI reservoir. 3 × `uvec4`, std430. Stores the evaluated
outgoing radiance `Lo` along the selected primary-scatter direction so final
shading does NOT retrace the selected direction. The root sample seed is
stored so temporal re-evaluation can replay stochastic choices (alpha
traversal, secondary NEE, shadow alpha) deterministically.

```glsl
struct SIGIReservoir {
    uvec4 data0;  // xyz = floatBitsToUint(direction.xyz), w = floatBitsToUint(hitT)
    uvec4 data1;  // xyz = floatBitsToUint(Lo.xyz),       w = floatBitsToUint(weightSum)
    uvec4 data2;  // x   = floatBitsToUint(targetPdf)
                  // y   = M (uint, accumulated candidate count)
                  // z   = packed (age << 16) | flags
                  //        flags bit 0 = valid, bit 1 = environment miss,
                  //        bit 2 = geometry hit, bit 3 = history-derived,
                  //        bit 4 = secondary NEE valid
                  // w   = root sample seed (or compact sample key)
};
```

Flag constants: `SI_GI_FLAG_VALID=1`, `SI_GI_FLAG_ENV_MISS=2`,
`SI_GI_FLAG_GEOMETRY_HIT=4`, `SI_GI_FLAG_HISTORY=8`, `SI_GI_FLAG_NEE_VALID=16`.

## ReSTIR GI Push Constants (`SIGIPushConstants`, 48 bytes)

Separate from `SIReSTIRPushConstants` — the DI and GI compute pipelines are
independent. Drives both the temporal and history-write GI dispatches.

```glsl
struct SIGIPushConstants {
    uint  freshCandidateCount;    // M: fresh GI candidates per pixel
    uint  temporalMCap;           // capped M from temporal history
    uint  maxTemporalAge;         // reject history above this age
    uint  flags;                  // bit 0: temporal reuse enabled

    float depthThreshold;         // relative depth difference threshold
    float normalThreshold;        // normal similarity threshold (dot product)
    float worldPosThreshold;      // world-position difference threshold
    uint  frameIndex;             // GI frame index (drives reservoir parity)

    vec4  jitter;                 // xy = current, zw = previous jitter in pixel units
};
```

## NRD UBO (`SINRDUniformData`, 16 bytes)

The two spare fields after `lobeDither` are repurposed for ReSTIR GI control
without growing the UBO or adding a push-constant range to the RT pipeline.

```glsl
struct SINRDUniformData {
    uint nrdEnabled;              // 1 = NRD mode (1 spp, no temporal accum, write G-buffer)
    uint lobeDither;              // 0=off (white noise), 1=Bayer 4x4, 2=Interleaved Gradient Noise
    uint restirGIEnabled;         // 1 = consume stored GI sample in raygen
    uint restirGIReservoirIndex;  // current GI reservoir region index (frame parity)
};
```

---

## Shader File Reference

### Ray Tracing Shaders

| File | Stage | Entry | Purpose |
|------|-------|-------|---------|
| `raygen.rgen` | Ray-gen | `main()` | RT-primary: traces full camera rays |
| `secondary_raygen.rgen` | Ray-gen | `main()` | Raster-first: reads G-buffer, traces secondary rays |
| `closesthit.rchit` | Closest-hit | `main()` | Material eval, scatter, NEE, recursive trace |
| `anyhit.rahit` | Any-hit | `main()` | Alpha-tested transparency (MASK materials) |
| `miss.rmiss` | Miss | `main()` | Environment map sampling |
| `shadow.rmiss` | Miss | `main()` | Shadow ray miss (unoccluded) |
| `shadow.rahit` | Any-hit | `main()` | Shadow ray hit (occluded) |

### Compute Shaders

| File | Entry | Purpose |
|------|-------|---------|
| `restir_temporal.comp` | `main()` | ReSTIR DI temporal reuse |
| `restir_spatial.comp` | `main()` | ReSTIR DI spatial reuse |
| `restir_gi_temporal.comp` | `main()` | ReSTIR GI fresh candidate + temporal reuse (prev reservoir + prev receiver history → current reservoir) |
| `restir_gi_history.comp` | `main()` | ReSTIR GI receiver-history write (G-buffer → current receiver history); runs after temporal dispatch |
| `compose.comp` | `main()` | NRD output remodulation + direct emission (no tonemap — tonemap is a separate pass) |
| `tonemap.comp` | `main()` | Tone map linear HDR output to display |
| `gbuffer_debug.comp` | `main()` | G-buffer / ReSTIR reservoir / NRD-input visualization (modes 0-21) |

### Graphics Shaders

| File | Stage | Purpose |
|------|-------|---------|
| `raster.vert` | Vertex | G-buffer vertex transform + jitter |
| `raster.frag` | Fragment | G-buffer fill (normal, roughness, viewZ, motion, albedo, emission) |

### Shared Includes

| File | Included by | Purpose |
|------|-------------|---------|
| `shader_interface.h` | All C++ + GLSL | Struct definitions, binding constants, static_asserts |
| `pathtracer_shared.glsl` | All RT shaders | Set 0/1 declarations, Material/TriangleLight/RayPayload, BRDF, NEE, NRD helpers, temporal accumulation |
| `scatter_shared.glsl` | closesthit, secondary_raygen | Scatter logic, ReSTIR DI sampling, computeNEE |
| `restir_shared.glsl` | ReSTIR compute + scatter_shared | Reservoir struct + operations, oct encoding, target density |
| `restir_bindings.glsl` | ReSTIR DI compute passes | Compute pass bindings + RNG |
| `restir_gi_shared.glsl` | ReSTIR GI compute + raygen | GI reservoir access, streaming, target and validation helpers |
| `restir_gi_bindings.glsl` | ReSTIR GI compute passes | GI buffer, TLAS, scene and G-buffer declarations |
| `ray_query_scene.glsl` | ReSTIR GI compute | Shared ray-query hit reconstruction and alpha traversal for compute ray queries |

---

## Push Constants

### ReSTIR (`SIReSTIRPushConstants`, 60 bytes)

```glsl
struct SIReSTIRPushConstants {
    uint   freshCandidateCount;     // initial RIS candidates per pixel
    uint   temporalMCap;            // max M for temporal reuse
    uint   spatialMCap;             // max M for spatial reuse
    uint   spatialNeighborCount;    // neighbors for spatial reuse
    uint   spatialRadius;           // pixel radius for neighbor search
    float  depthThreshold;          // reject if relative depth delta exceeds
    float  normalThreshold;         // reject if normal dot product below
    float  worldPosThreshold;       // reject if world-position distance exceeds
    uint   maxTemporalAge;          // reject history above this age
    uint   flags;                   // bit 0: temporal enabled, bit 1: spatial enabled
    uint   frameIndex;              // independent ReSTIR frame counter
    vec4   jitter;                  // .xy = current jitter, .zw = previous jitter
};
```

### Camera UBO (`SICameraData`, 496 bytes)

```glsl
struct SICameraData {
    vec4  position;         // .xyz = camera pos, .w = frame index
    vec4  forward;          // .xyz = forward dir, .w = jitter.x
    vec4  right;            // .xyz = right dir, .w = jitter.y
    vec4  up;               // .xyz = up dir, .w = restirEnabled flag
    vec4  viewportSPP;      // .xy = viewport, .z = spp, .w = maxBounces
    vec4  apertureFocal;    // .x = aperture, .y = focus dist, .w = emissive boost
    vec4  envMap;           // .x = env map tex idx, .y = env intensity, .zw = CDF tex indices
    mat4  inverseProjection;
    mat4  inverseView;
    mat4  viewToClip;       // current projection
    mat4  viewToClipPrev;   // previous projection (for motion vectors)
    mat4  worldToView;      // current view
    mat4  worldToViewPrev;  // previous view (for motion vectors)
};
```
