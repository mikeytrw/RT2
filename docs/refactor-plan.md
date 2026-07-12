# Refactor Plan

Ordered work items to eliminate data duplication, remove the CPU renderer,
and enable large scene loading. Each phase is independently testable.

---

## Phase 1: Remove CPU Renderer

**Goal**: Delete all CPU-only rendering code. The GPU renderer is the only renderer.

### 1a: Compile-migration checklist (before deleting any files)

The CPU renderer headers are still included from active code. These
dependencies must be resolved BEFORE file deletion or the build breaks:

| File to change | What to do first |
|----------------|-----------------|
| `WalnutApp.cpp:6,10` | Remove `#include "Renderer.h"` and `#include "Mesh.h"`. Remove `m_Renderer` member, `m_Mesh` member, `ReloadMesh()`, `BuildSceneFromCurrentState()`, `RebuildMaterial()`, legacy `LoadMeshFileAsEntity()` (OBJ branch only — the non-OBJ branch uses CPU `Mesh::Load`), all Mesh panel UI controls (Load Mesh button, material combo, position/rotation/scale sliders). Move GPU SPP/bounce defaults from `m_Renderer.m_SamplesPerPixel` / `m_Renderer.m_MaxBounceDepth` to hardcoded defaults or `RenderSettings` fields. |
| `SceneManager.h:9` | Remove `#include "Mesh.h"`. Remove `m_CpuMeshes` member and all references. Remove legacy OBJ fallback branch in `SyncToGPU()` (the `!m_EcsPopulated && gpuData.meshes.empty() && !m_CpuMeshes.empty()` block). |
| `SceneManager.cpp` | Remove `m_CpuMeshes` usage in `LoadScene()` and elsewhere. Remove `Mesh` includes. |
| `CLIArgs.h` | Remove `--renderer` flag (GPU is the only option). Keep `--scene`, `--env`, etc. |

### 1b: Files to delete

Once the includes and dependencies are removed:

| File | Reason |
|------|--------|
| `Renderer.h` | CPU ray tracer (not `RendererGPU`) |
| `Mesh.h` / `Mesh.cpp` | CPU mesh + BVH (tinyobj loading for CPU path) |
| `Hittable.h` | Abstract hittable interface |
| `HittableList.h` | Hittable container |
| `BVHNode.h` | CPU BVH construction + traversal |
| `AABB.h` | CPU bounding box |
| `Sphere.h` | CPU sphere primitive |
| `Triangle.h` | CPU triangle primitive |
| `Material.h` | CPU material types (Lambertian, Metal, Dielectric, Emissive) |
| `Colour.h` | CPU color utilities |
| `Utility.h` | CPU math utilities (nearZero, etc.) |
| `Ray.h` | CPU ray definition |

### 1c: Verification

- Build succeeds with no link errors
- glTF scenes load and render identically to pre-refactor
- OBJ scenes load via the OBJ-to-ECS loader (parity with current behavior —
  note: current OBJ path merges all geometry into one mesh with material 0,
  so "identically" means same merged-mesh behavior, not per-material parity)
- No references to deleted files anywhere in the codebase (`grep -r` check)

---

## Phase 2: Make ECSScene the Sole Scene Representation

**Goal**: Remove the legacy `Scene` class. ECSScene is the only scene data structure.

### Changes

1. **Move shared structs out of `Scene.h`**: `SceneMaterial`, `SceneTexture`, `SceneCamera`, `SceneLight`, `MaterialType`, `LightType` → new `SceneTypes.h` (no `Scene` class)

2. **Delete `Scene` class**: Remove `Scene::m_Meshes`, `Scene::m_Materials`, `Scene::m_Lights`, `Scene::m_Textures`, `Scene::m_Camera` and all methods. The `Scene` class itself is deleted.

3. **Update `SceneManager`**: Remove `m_Scene` (legacy `Scene`), remove `BuildGPUSceneData(Scene&)` path, always use `BuildGPUSceneDataFromECS(ECSScene&)`. Remove `m_EcsPopulated` flag (ECS is always populated).

4. **Update `SceneLoader`**: `Load()` (legacy Scene path) removed. Only `LoadIntoECS()`, `LoadObjIntoECS()`, and `ImportIntoECS()` remain. `Save()` can remain if needed (writes from ECS).

5. **Update `GPUSceneData.cpp`**: Remove `BuildGPUSceneData(const Scene&)`. Only `BuildGPUSceneDataFromECS(const ECSScene&)` and `UpdateInstancesFromECS()` remain.

6. **Update `WalnutApp.cpp`**: Remove all `m_SceneMgr.GetScene()` calls. Use `m_SceneMgr.GetECS()` instead. Remove camera-from-Scene fallback.

### Verification

- glTF and OBJ scenes load and render
- Entity outliner works
- Material editor works
- Save scene works (if implemented)

---

## Phase 3.0: Indexed-Buffer ABI Spike

**Goal**: Verify the SSBO layout contract before touching any production
buffers or shaders. This is a standalone spike — do NOT delete combined
buffers yet.

### 3.0a: SSBO layout decision

`std430` arrays of `vec3` have a 16-byte array stride unless the declaration
explicitly uses `layout(scalar, std430)`. Merely enabling `GL_EXT_scalar_block_layout`
does not change a `std430` declaration.

**Decision: use `vec4` storage (16-byte stride) for position and normal arrays.**
This is the safest option — it matches the existing combined-buffer ABI
(which uses `vec4`), requires no layout extension, and has no alignment
surprises. The .w component is unused (set to 1.0 for positions, 0.0 for
normals, matching current behavior).

For UVs, `vec2` in `std430` has an 8-byte array stride — this is safe without
scalar layout. However, for consistency and to avoid any ambiguity, use
`vec4` with .xy = UV, .zw = 0 (matching current combined-buffer format).

For indices, `uint` in `std430` has a 4-byte array stride — this is safe.

**Summary of buffer formats:**

| Buffer | GLSL type | Array stride | C++ type | C++ stride |
|--------|-----------|-------------|----------|------------|
| Vertex (position) | `vec4[]` | 16 bytes | `glm::vec4` | 16 bytes |
| Index | `uint[]` | 4 bytes | `uint32_t` | 4 bytes |
| Normal | `vec4[]` | 16 bytes | `glm::vec4` | 16 bytes |
| UV | `vec4[]` | 16 bytes | `glm::vec4` | 16 bytes |
| Instance mesh info | `uvec4[]` | 16 bytes | `glm::uvec4` | 16 bytes |

### 3.0b: Stride/offset assertions

Add compile-time and runtime assertions to catch ABI mismatches:

```cpp
// In shader_interface.h or a new GpuLayoutAssert.h:
static_assert(sizeof(glm::vec4) == 16, "vec4 must be 16 bytes");
static_assert(sizeof(glm::uvec4) == 16, "uvec4 must be 16 bytes");
static_assert(sizeof(uint32_t) == 4, "uint32 must be 4 bytes");

// Runtime: verify SSBO array stride matches C++ stride
// After descriptor set update, validate that the buffer size is an exact
// multiple of the expected element stride.
```

### 3.0c: Spike test

Create a minimal test: one mesh, two instances at different positions.
Validate:
- Indexed position fetch produces correct world-space positions
- Indexed UV fetch produces correct texture coordinates
- Normal fetch matches expected values
- Light reconstruction (emissive triangle positions) works with new buffers
- Compile and run with existing combined buffers still present (both paths
  coexist during the spike)

---

## Phase 3: Eliminate Combined Buffers

**Goal**: Replace per-triangle combined buffers with indexed per-vertex buffers.
Closest-hit and all other consumers fetch via `gl_PrimitiveID` → index buffer
→ vertex buffer. Remove all per-triangle data expansion.

### 3a: Complete consumer inventory

The following files read from the combined buffers (bindings 3/5/6/7/8) and
MUST be migrated together. Leaving any un-migrated causes compile failure or
silent data corruption.

| File | Lines | What it reads | New pattern |
|------|-------|---------------|-------------|
| `pathtracer_shared.glsl` | 54-79 | Declares `triangleNormals`, `trianglePositions`, `triangleUVs`, `triangleTangents`, `normalOffsets` | Replace declarations with `vertices[]`, `indices[]`, `normals[]`, `uvs[]`, `instanceMeshInfo[]` |
| `closesthit.rchit` | 40-88 | `hitTriPositions()`, `hitFaceNormal()`, `hitUV()`, `hitTangent()`, `hitShadingNormal()` | Indexed fetch via `gl_PrimitiveID` → `indices[]` → `vertices[]`/`normals[]`/`uvs[]`. Tangent computed inline from UV gradients. |
| `restir_shared.glsl` | 220-249, 314-317, 400-425 | Reconstructs emissive triangle positions + UVs for target density, proposal PDF, final lighting | Replace `normalOffsets[light.ids.x] + light.ids.y` + `trianglePositions[posIdx]` with indexed fetch using `instanceMeshInfo[light.ids.x]` + `indices[]` + `vertices[]` |
| `scatter_shared.glsl` | 386-432 | Same emissive triangle reconstruction for NEE | Same migration as restir_shared |
| `restir_bindings.glsl` | 54-68 | Declares combined buffer bindings for ReSTIR compute passes | Replace declarations |
| `gbuffer_debug.comp` | 27-32 | Reads `normalOffsets`, `triangleUVs`, `trianglePositions` for debug visualization | Indexed fetch |
| `ris.comp` | 66-102, 305-332 | Dead code but still compiles. Reads combined buffers. | **Delete `ris.comp` and `RISPass.h/.cpp` BEFORE removing binding definitions** — they are dead but still built. |

### 3b: Normal-mapping and shading contract

The current closest-hit uses **geometric face normals** (`cross(v1-v0, v2-v0)`)
for shading, not interpolated vertex normals. Tangents are precomputed per-triangle
from UV gradients and stored in the combined buffer. The refactor must preserve
this behavior unless explicitly changed.

**Shading contract (post-refactor):**

| Aspect | Current | Post-refactor | Rationale |
|--------|---------|---------------|-----------|
| Shading normal (no normal map) | Geometric face normal `cross(e1, e2)` | **Same**: geometric face normal computed from fetched positions | Preserves current look; vertex normals not needed |
| Shading normal (with normal map) | Face normal + tangent-space perturbation from texture | **Same**: face normal + tangent from UV gradient + normal map | Preserves current look |
| Tangent | Precomputed per-triangle from UV gradients, stored in buffer | **Computed inline** in shader from fetched positions + UVs | Eliminates tangent buffer; same formula |
| Ray offset normal | Geometric face normal | **Same**: geometric face normal | Used for `rayOrigin + N * 0.001` offset |
| Vertex normals | Available in MeshData but **not used** by closest-hit | **Not uploaded** (keep in MeshData for future use) | Saves memory; no current consumer |

**Tangent computation (inline in shader):**

```glsl
vec3 computeTangent(vec3 p0, vec3 p1, vec3 p2, vec2 uv0, vec2 uv1, vec2 uv2)
{
    vec3 edge1 = p1 - p0;
    vec3 edge2 = p2 - p0;
    vec2 dUV1 = uv1 - uv0;
    vec2 dUV2 = uv2 - uv0;
    float det = dUV1.x * dUV2.y - dUV1.y * dUV2.x;
    if (abs(det) < 1e-8) return vec3(1.0, 0.0, 0.0); // degenerate UV fallback
    float r = 1.0 / det;
    return normalize(r * (dUV2.y * edge1 - dUV1.y * edge2));
}
```

**Edge cases:**

| Case | Handling |
|------|----------|
| Degenerate UV triangle (det ≈ 0) | Fallback tangent `(1, 0, 0)` — same as current `BuildGPUSceneDataFromECS` behavior |
| Missing UVs (empty UV buffer) | Use `(0, 0)` for all vertices. Tangent fallback applies. Normal map will not work (expected — no UVs to sample texture) |
| Mirrored UV islands | Handedness is not stored or used. Current code does not track tangent handedness (no bitangent sign). Post-refactor is identical. |
| Non-uniform instance scaling | Tangent is computed in object space, then transformed by `mat3(instanceTransforms[InstanceID])` and normalized — same as current. Non-uniform scale distorts tangent direction, but this is a known limitation of flat per-triangle tangents. Not a regression. |
| Missing vertex normals | Not relevant — closest-hit uses geometric face normals, not vertex normals. The normal buffer is not uploaded if MeshData has no normals. |

### 3c: GPUSceneData changes

**`GPUSceneData.h`**:
- Remove `GPUMeshGeometry::vertexUVs` (per-tri, 6 floats/tri)
- Remove `GPUMeshGeometry::tangents` (per-tri, 9 floats/tri)
- Add `GPUMeshGeometry::normals` (per-vertex, `vector<float>` stride 3 — direct copy from MeshData)
- Add `GPUMeshGeometry::uvs` (per-vertex, `vector<float>` stride 2 — direct copy from MeshData)
- `vertices` and `indices` remain as-is

**`GPUSceneData.cpp`** (`BuildGPUSceneDataFromECS`):
- Copy `MeshData::normals` → `GPUMeshGeometry::normals` (direct, no expansion)
- Copy `MeshData::uvs` → `GPUMeshGeometry::uvs` (direct, no expansion)
- Remove per-tri UV reformatting loop
- Remove tangent computation loop

### 3d: AccelerationStructure changes

**`AccelerationStructure.h`**:
- Remove combined buffer members (`m_CombinedNormalBuffer/Memory`, etc.)
- Add: `m_VertexBuffer/Memory` (vec4 per vertex, positions as vec4(x,y,z,1))
- Add: `m_IndexBuffer/Memory` (uint32 per index)
- Add: `m_NormalBuffer/Memory` (vec4 per vertex, normals as vec4(x,y,z,0))
- Add: `m_UVBuffer/Memory` (vec4 per vertex, UVs as vec4(u,v,0,0))
- Add: `m_InstanceMeshInfoBuffer/Memory` (uvec4 per instance: {vertOffset, idxOffset, normOffset, uvOffset})
- Remove `BLASData::triPositions`, `triUVs`, `triTangents`
- Remove `BuildCombinedBuffers()` method
- Add `BuildAttributeBuffers()` method

**`AccelerationStructure.cpp`**:
- In `BuildBLASes()`: keep per-BLAS vertex/index buffer creation (for AS build).
  Remove per-tri data fill loop (lines 57-98). Record per-BLAS vertex/index
  counts for mega-buffer offset computation.
- Replace `BuildCombinedBuffers()` with `BuildAttributeBuffers()`:
  - Compute total vertex count and total index count across all BLASes
  - Create 4 DEVICE_LOCAL mega-buffers (vertex as vec4, index as uint32, normal as vec4, UV as vec4)
  - **Chunked upload** (see 3e below): upload one mesh at a time via staging
  - Create per-instance mesh info buffer (uvec4 per instance)
  - No per-triangle expansion anywhere

### 3e: Chunked upload strategy

To avoid recreating the OOM failure mode, the upload must be chunked:

```
For each mesh (0..N):
  1. Compute staging size = max(vertexBytes, indexBytes, normalBytes, uvBytes)
  2. Create one staging buffer (HOST_VISIBLE, that size)
  3. Map + fill vertex data (vec4 per vertex) + vkCmdCopyBuffer
  4. Map + fill index data + vkCmdCopyBuffer
  5. Map + fill normal data (if present) + vkCmdCopyBuffer
  6. Map + fill UV data (if present) + vkCmdCopyBuffer
  7. Destroy staging buffer
  → Peak HOST_VISIBLE per iteration = one staging buffer (max ~10-20MB per mesh)
```

For a single mega-mesh (OBJ path): the staging buffer will be large
(~70MB for 17.5M vec4 positions), but that is far below the 856MB that
caused OOM. If needed, split into sub-chunks (e.g., 1M vertices at a time).

**Memory accounting (peak vs steady-state):**

| Resource | Peak (during upload) | Steady-state (after upload) |
|----------|---------------------|---------------------------|
| CPU staging buffer | ~70MB (one mesh at a time) | 0 (freed) |
| CPU GPUSceneData | ~400MB (vertices+indices+normals+UVs) | ~400MB (kept as mirror) |
| CPU MeshRegistry | ~622MB | ~622MB (canonical source) |
| GPU DEVICE_LOCAL buffers | ~400MB (mega-buffers) | ~400MB |
| GPU BLAS structures | ~400MB | ~400MB |
| **Total CPU peak** | **~1.1 GB** | **~1.0 GB** (steady) |
| **Total GPU peak** | **~800 MB** | **~800 MB** (steady) |

### 3f: Shader migration

**`shader_interface.h`**:
- Remove: `SI_BINDING_NORMAL_BUFFER` (old per-tri #3), `SI_BINDING_INSTANCE_OFFSETS` (#5), `SI_BINDING_TANGENT_BUFFER` (#6), `SI_BINDING_UV_BUFFER` (old per-tri #7), `SI_BINDING_POSITION_BUFFER` (#8)
- Add: `SI_BINDING_VERTEX_BUFFER` (3), `SI_BINDING_INDEX_BUFFER` (5), `SI_BINDING_NORMAL_BUFFER` (6, new per-vertex), `SI_BINDING_UV_BUFFER` (7, new per-vertex), `SI_BINDING_INSTANCE_MESH_INFO` (8)

**`pathtracer_shared.glsl`**:
- Remove: `triangleNormals[]`, `trianglePositions[]`, `triangleUVs[]`, `triangleTangents[]`, `normalOffsets[]`
- Add: `vertices[]` (vec4), `indices[]` (uint), `normals[]` (vec4), `uvs[]` (vec4), `instanceMeshInfo[]` (uvec4)

**`closesthit.rchit`**:
- Replace all combined-buffer fetches with indexed fetch
- Add inline `computeTangent()` function (see 3b above)
- `hitShadingNormal()`: still uses geometric face normal + normal-map perturbation

**`restir_shared.glsl`** (lines 220-249, 314-317, 400-425):
- Replace `normalOffsets[light.ids.x] + light.ids.y` → indexed fetch:
  ```glsl
  uvec4 meshInfo = instanceMeshInfo[light.ids.x];
  uint idxBase = meshInfo.y + light.ids.y * 3u;
  uint i0 = indices[idxBase + 0u];
  uint i1 = indices[idxBase + 1u];
  uint i2 = indices[idxBase + 2u];
  vec3 lp0 = vertices[meshInfo.x + i0].xyz;
  // ... (transform via lightWorld, same as current)
  ```
- Same for UVs: `uvs[meshInfo.w + i0].xy` etc.

**`scatter_shared.glsl`** (lines 386-432):
- Same indexed-fetch migration as restir_shared

**`restir_bindings.glsl`** (lines 54-68):
- Replace combined buffer declarations with new buffer declarations

**`gbuffer_debug.comp`** (lines 27-32):
- Replace `normalOffsets`, `triangleUVs`, `trianglePositions` with indexed fetch

**`ris.comp`** and **`RISPass.h/.cpp`**:
- **Delete these files FIRST** (before removing binding definitions from `shader_interface.h`).
  They are dead code (never dispatched) but still compiled. Removing them first
  avoids a broken intermediate state.

**`secondary_raygen.rgen`**: No change (reads G-buffer images, not combined buffers)

**`RasterPass.cpp`**: No immediate change (uses its own mega vertex buffer). Future: share indexed buffers with RT path.

### 3g: PathTracePass and SceneResources updates

**`PathTracePass.cpp`**:
- Update `CreateDescriptorSetLayout()`: remove old bindings 3/5/6/7/8, add new bindings
- Update `UpdateDescriptorSet()`: bind new buffers

**`SceneResources.cpp`**:
- Update `RebuildAccelerationStructures()`: call `BuildAttributeBuffers()` instead of `BuildCombinedBuffers()`
- Add getters: `GetVertexBuffer()`, `GetIndexBuffer()`, `GetNormalBuffer()` (new), `GetUVBuffer()` (new), `GetInstanceMeshInfoBuffer()`
- Remove old getters: `GetNormalBuffer()` (old combined), `GetTangentBuffer()`, `GetPositionBuffer()`, `GetInstanceOffsetBuffer()`

### Verification

- `cube.obj` renders correctly (simple geometry, no textures)
- Existing glTF scenes render correctly (textured, normal-mapped)
- **Regression scenes**: test with scenes that have:
  - Normal-mapped surfaces (verify tangent computation is correct)
  - Mirrored UV islands (verify no handedness regression — current code does not track handedness, so this is a parity check)
  - Degenerate UV triangles (verify fallback tangent is used)
  - Hard edges (verify geometric face normals match)
  - Non-uniform instance scaling (verify tangent transform)
  - Emissive surfaces (verify ReSTIR light reconstruction works)
- G-buffer debug view shows correct geometry, normals, UVs
- Compare side-by-side screenshots with pre-refactor for at least one complex scene

---

## Phase 4: Test Large Scenes (Static Navigation)

**Goal**: Verify San Miguel and Bistro load and render without OOM.
This phase tests load-time stability and static rendering only.
Smooth navigation, animation, and live editing are deferred to Phase 6.

### Test scenes

| Scene | Source | Tris | Textures | Notes |
|-------|--------|------|----------|-------|
| San Miguel low-poly | McGuire Archive | 5.6M | 269 | Single OBJ, no instancing |
| Bistro Exterior | McGuire Archive | 2.8M | ~100 | Exterior only |
| Bistro Interior | McGuire Archive | 1.0M | ~50 | Interior only |

### Success criteria (static)

- Scene loads without crash (CPU peak memory < 2 GB, measured separately)
- AS builds without GPU OOM (DEVICE_LOCAL < 2 GB)
- Renders at >5 FPS at 1080p with NRD (quality not important, just stability)
- G-buffer debug view shows correct geometry
- Camera can move (WASD) and the scene renders — minor stutter acceptable
- Measure and report: peak CPU RAM, peak HOST_VISIBLE GPU memory, peak DEVICE_LOCAL GPU memory, steady-state totals

### Known issues to address if they arise

- **Texture staging arena overflow**: 269 textures exceed the current staging arena size. Fix: increase arena size or batch texture uploads.
- **BLAS build time**: 5.6M tris in a single BLAS may take several seconds. Acceptable for load time.
- **Shader divergence**: A single mega-BLAS with 281 materials means high material divergence. Consider SER as future optimization.

---

## Phase 5: Cleanup

After phases 1-4 are verified:

1. **Remove debug `fprintf(stderr)` calls** added during OBJ loader debugging
2. **Remove `RT_LOG` spam** in `BuildBLASes` (per-mesh logging)
3. **Update premake5.lua** to remove deleted files from build
4. **Update CLI help text** (`--renderer` removed)
5. **Consolidate `SceneTypes.h`** — ensure no remaining references to old `Scene.h`
6. **Remove stale `.spv` files** for deleted shaders
7. **Commit** all changes

---

## Phase 6: Asynchronous Update Path (Future)

**Goal**: Enable smooth camera navigation, live editing, and animation on
large scenes without stuttering. This is deferred from Phase 4 — the current
transform-only update path uses `vkDeviceWaitIdle` and synchronous submission,
which causes stalls.

### Work items

1. **Frame-fence-based resource retirement**: Instead of `vkDeviceWaitIdle`
   before destroying old BLASes/buffers, use the frame fence from 2 frames
   ago to know when old resources are safe to destroy.

2. **Non-blocking instance uploads**: Replace `vkDeviceWaitIdle` + immediate
   `vkCmdUpdateBuffer` with a per-frame staged upload (write to a HOST_VISIBLE
   staging buffer, `vkCmdCopyBuffer` in the frame's command buffer).

3. **TLAS update/refit**: Instead of full TLAS rebuild on transform change,
   use `vkCmdBuildAccelerationStructuresKHR` with update mode (refit).
   Only rebuild fully when instances are added/removed.

4. **Async BLAS rebuild**: For large meshes, rebuild BLAS on a background
   command queue while the render queue continues rendering the previous
   frame's AS.

### Success criteria

- Camera navigation at 30+ FPS on San Miguel with no stalls > 1 frame
- Entity transform edit in outliner does not block the render thread
- Adding/removing an entity triggers AS rebuild without freezing the viewport