# Shader Bindings

Descriptor set layout reference for the path tracer and compute passes.
Constants are defined in `shader_interface.h` (shared between C++ and GLSL).

---

## Set 0 — Scene-Global (Path Tracer + Compute Passes)

Owned by `PathTracePass`. Bound for RT dispatches and compute passes
(ReSTIR, compose, G-buffer debug).

### Current Bindings (Pre-Refactor)

| # | Constant | Type | Points to |
|---|----------|------|-----------|
| 0 | `SI_BINDING_OUTPUT_IMAGE` | Storage image (rgba32f) | Output beauty image |
| 1 | `SI_BINDING_CAMERA_UBO` | Uniform buffer | Camera data (496 bytes) |
| 2 | `SI_BINDING_MATERIAL_BUFFER` | Storage buffer (readonly) | `GPUMaterial[]` |
| 3 | `SI_BINDING_NORMAL_BUFFER` | Storage buffer (readonly) | `vec4 triangleNormals[]` (1 per tri) |
| 4 | `SI_BINDING_TLAS` | Acceleration structure | TLAS handle |
| 5 | `SI_BINDING_INSTANCE_OFFSETS` | Storage buffer (readonly) | `uint normalOffsets[]` (per instance) |
| 6 | `SI_BINDING_TANGENT_BUFFER` | Storage buffer (readonly) | `vec4 triangleTangents[]` (3 per tri) |
| 7 | `SI_BINDING_UV_BUFFER` | Storage buffer (readonly) | `vec4 triangleUVs[]` (3 per tri) |
| 8 | `SI_BINDING_POSITION_BUFFER` | Storage buffer (readonly) | `vec4 trianglePositions[]` (3 per tri) |
| 9 | `SI_BINDING_LIGHT_BUFFER` | Storage buffer (readonly) | 16-byte header + `TriangleLight[]` |
| 10 | `SI_BINDING_INSTANCE_TRANSFORMS` | Storage buffer (readonly) | `mat4 instanceTransforms[]` |
| 11 | `SI_BINDING_TEXTURE_ARRAY` | Combined image samplers (partially bound, max 1000) | Bindless texture array |
| 12 | `SI_BINDING_INSTANCE_TRANSFORMS_PREV` | Storage buffer (readonly) | `mat4 instanceTransformsPrev[]` |
| 13 | `SI_BINDING_INSTANCE_MATERIAL_INDICES` | Storage buffer (readonly) | `uint instanceMaterialIndices[]` |
| 14 | `SI_BINDING_RESERVOIR_HISTORY` | Storage buffer | ReSTIR reservoir history |
| 15 | `SI_BINDING_RESERVOIR_SCRATCH` | Storage buffer | ReSTIR reservoir scratch |
| 16 | `SI_BINDING_SURFACE_HISTORY` | Storage buffer | ReSTIR surface history |

### Post-Refactor Bindings

Bindings 3, 5, 6, 7, 8 (combined per-triangle buffers + offsets) are replaced
with per-mesh indexed vertex attribute buffers:

| # | Constant | Type | Points to |
|---|----------|------|-----------|
| 0 | `SI_BINDING_OUTPUT_IMAGE` | Storage image (rgba32f) | Output beauty image |
| 1 | `SI_BINDING_CAMERA_UBO` | Uniform buffer | Camera data (496 bytes) |
| 2 | `SI_BINDING_MATERIAL_BUFFER` | Storage buffer (readonly) | `GPUMaterial[]` |
| 3 | `SI_BINDING_VERTEX_BUFFER` | Storage buffer (readonly) | `vec3 vertices[]` (per-mesh, concatenated) |
| 4 | `SI_BINDING_TLAS` | Acceleration structure | TLAS handle |
| 5 | `SI_BINDING_INDEX_BUFFER` | Storage buffer (readonly) | `uint indices[]` (per-mesh, concatenated) |
| 6 | `SI_BINDING_NORMAL_BUFFER` | Storage buffer (readonly) | `vec3 normals[]` (per-mesh, concatenated) |
| 7 | `SI_BINDING_UV_BUFFER` | Storage buffer (readonly) | `vec2 uvs[]` (per-mesh, concatenated) |
| 8 | `SI_BINDING_INSTANCE_MESH_INFO` | Storage buffer (readonly) | `uvec4 instanceMeshInfo[]` (per instance: {vertexOffset, indexOffset, normalOffset, uvOffset}) |
| 9 | `SI_BINDING_LIGHT_BUFFER` | Storage buffer (readonly) | 16-byte header + `TriangleLight[]` |
| 10 | `SI_BINDING_INSTANCE_TRANSFORMS` | Storage buffer (readonly) | `mat4 instanceTransforms[]` |
| 11 | `SI_BINDING_TEXTURE_ARRAY` | Combined image samplers (partially bound, max 1000) | Bindless texture array |
| 12 | `SI_BINDING_INSTANCE_TRANSFORMS_PREV` | Storage buffer (readonly) | `mat4 instanceTransformsPrev[]` |
| 13 | `SI_BINDING_INSTANCE_MATERIAL_INDICES` | Storage buffer (readonly) | `uint instanceMaterialIndices[]` |
| 14 | `SI_BINDING_RESERVOIR_HISTORY` | Storage buffer | ReSTIR reservoir history |
| 15 | `SI_BINDING_RESERVOIR_SCRATCH` | Storage buffer | ReSTIR reservoir scratch |
| 16 | `SI_BINDING_SURFACE_HISTORY` | Storage buffer | ReSTIR surface history |

**Key change**: Bindings 3/5/6/7/8 go from per-triangle vec4 arrays (normal,
position, UV, tangent + offset table) to per-vertex indexed buffers using
**vec4 storage** (16-byte array stride, matching std430 ABI). Position and
normal buffers use vec4 with .xyz = data, .w = padding (1.0 for positions,
0.0 for normals). UV buffers use vec4 with .xy = UV, .zw = 0. Tangents are
computed in-shader from UV gradients — no tangent buffer needed. An
`instanceMeshInfo` buffer (uvec4 per instance) provides offsets into the
concatenated vertex/index/normal/UV mega-buffers.

**ABI contract**: All attribute buffers use `vec4` (16-byte stride) in std430
layout. This avoids the scalar-layout alignment pitfall where `vec3` in
std430 has a 16-byte array stride but C++ `sizeof(vec3)` is 12 bytes. Using
vec4 with explicit padding ensures C++ and GLSL agree on layout without
requiring `GL_EXT_scalar_block_layout`.

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
| 6 | `SI_BINDING_NRD_UBO` | Uniform buffer | NRD uniform data (16 bytes) |
| 7 | `SI_BINDING_G_DIRECT_EMISSION` | Storage image (rgba16f) | Direct emission (bypasses NRD) |
| 8 | `SI_BINDING_G_PRIM_HIT` | Storage image (rgba32f) | World pos (xyz) + packed matIdx (w) |
| 9 | `SI_BINDING_G_PRIM_GEO_NORMAL` | Storage image (rgba8) | Geometric normal (0.5+0.5 encode) |
| 10 | `SI_BINDING_G_PRIM_UV` | Storage image (rg16f) | UV at primary hit |

---

## Closest-Hit Attribute Fetch (Post-Refactor)

The current closest-hit shader reads from combined per-triangle buffers:

```glsl
// CURRENT (pre-refactor) — per-triangle, 3 vec4s per attribute
uint offset = normalOffsets[gl_InstanceID] + gl_PrimitiveID * 3;
vec3 pos0 = trianglePositions[offset + 0].xyz;
vec3 pos1 = trianglePositions[offset + 1].xyz;
vec3 pos2 = trianglePositions[offset + 2].xyz;
// ... same for UVs, tangents
```

Post-refactor, it fetches via the index buffer:

```glsl
// POST-REFACTOR — indexed, vec4 storage (16-byte stride)
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
// ...

uint uvOffset = meshInfo.w;
vec2 uv0 = uvs[uvOffset + i0].xy;  // .zw = 0.0 (padding)
// ...

// Tangent computed from UV gradients (no buffer needed)
vec3 tangent = computeTangentFromUV(pos0, pos1, pos2, uv0, uv1, uv2);
```

The `instanceMeshInfo` buffer (binding 8) provides per-instance offsets into
the concatenated vertex/index/normal/UV buffers. This is the same pattern
used by vertex shaders for indexed draws.

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
| `compose.comp` | `main()` | NRD output remodulation + tone map |
| `gbuffer_debug.comp` | `main()` | G-buffer visualization |

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
| `restir_bindings.glsl` | ReSTIR compute passes | Compute pass bindings + RNG |

---

## Push Constants

### ReSTIR (`SIReSTIRPushConstants`, 48 bytes)

```glsl
struct SIReSTIRPushConstants {
    uint   freshCandidateCount;     // initial RIS candidates per pixel
    uint   temporalMCap;            // max M for temporal reuse
    uint   spatialNeighborCount;    // neighbors for spatial reuse
    uint   spatialRadius;           // pixel radius for neighbor search
    float  depthThreshold;          // reject if depth delta exceeds
    float  normalThreshold;         // reject if normal dot product below
    uint   flags;                   // bit 0: temporal enabled, bit 1: spatial enabled
    uint   frameIndex;              // accumulation frame counter
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