# Descriptor Set & Resource Management Refactor

## Problem Statement

RT2's descriptor set and texture management has several structural bugs and
design problems:

1. **Texture index identity not preserved (BUG — acid trip colors)** —
   `RendererGPU.cpp:238-246` skips `GpuImage` entries with no view while
   building the descriptor array. This compacts the descriptor list so
   descriptor slot N no longer maps to `GPUMaterial::textureIndices == N`.
   Any missing/failed texture shifts all subsequent indices.

2. **Env map extraction shifts texture indices (BUG)** —
   `SceneResources.cpp:116-125` removes the env map texture from
   `sceneData.textures` before passing to `AsyncTextureLoader`, which appends
   it at the end. If env map was at index 3, all textures after index 3 shift
   down by 1, but material texture indices still reference the original
   positions. No remap is applied to materials.

3. **Hard-coded binding counts** — `bindings[18]`, `bindingFlags[18]`,
   `writes[21]`, `bindingCount = 18` scattered across PathTracePass.cpp.
   Adding a binding requires manually updating 4+ locations with no
   compile-time checks.

4. **Fragile write packing** — `UpdateDescriptorSet` manually packs
   `VkWriteDescriptorSet` entries with fragile index arithmetic
   (`writeCount = 13`, `nextWrite`, `restirIdx`). Redundant initialization
   at `writes[17]` (line 567) then re-initialization at the packed location
   is dead code, not a gap bug, but the pattern is unmaintainable.

5. **Dead code** — `m_InstanceMaterialIndexBuffer` (constructed but never
   bound to any descriptor), `CreateTextures` (replaced by
   AsyncTextureLoader), `CreateEnvMapCDFTextures` (replaced by async CDF
   upload), `DestroyEnvMapCDFTextures` (only resets indices).

6. **Binding 13 naming confusion** — `SI_BINDING_INSTANCE_MATERIAL_INDICES`
   (binding 13) points to the per-triangle material index buffer from
   AccelerationStructure, not the per-instance material index buffer in
   SceneResources (which is dead code).

## Current Binding Layout (Set 0)

| Binding | Name | Type | Count | Owner |
|---------|------|------|-------|-------|
| 0 | OUTPUT_IMAGE | Storage image | 1 | RendererGPU |
| 1 | CAMERA_UBO | Uniform buffer | 1 | RendererGPU |
| 2 | MATERIAL_BUFFER | Storage buffer | 1 | SceneResources |
| 3 | VERTEX_BUFFER | Storage buffer | 1 | AccelerationStructure |
| 4 | TLAS | Accel structure | 1 | AccelerationStructure |
| 5 | INDEX_BUFFER | Storage buffer | 1 | AccelerationStructure |
| 6 | NORMAL_BUFFER | Storage buffer | 1 | AccelerationStructure |
| 7 | UV_BUFFER | Storage buffer | 1 | AccelerationStructure |
| 8 | INSTANCE_MESH_INFO | Storage buffer | 1 | AccelerationStructure |
| 9 | LIGHT_BUFFER | Storage buffer | 1 | SceneResources |
| 10 | INSTANCE_TRANSFORMS | Storage buffer | 1 | SceneResources |
| 11 | TEXTURE_ARRAY | Combined image sampler | 1000 | SceneResources |
| 12 | INSTANCE_TRANSFORMS_PREV | Storage buffer | 1 | SceneResources |
| 13 | INSTANCE_MATERIAL_INDICES | Storage buffer | 1 | AccelerationStructure |
| 14 | RESERVOIR_HISTORY | Storage buffer | 1 | ReservoirResources |
| 15 | RESERVOIR_SCRATCH | Storage buffer | 1 | ReservoirResources |
| 16 | SURFACE_HISTORY | Storage buffer | 1 | ReservoirResources |
| 17 | INSTANCE_MAT_OFFSETS | Storage buffer | 1 | AccelerationStructure |

## Architecture: TextureSlot Contract

The foundation of this refactor is a **TextureSlot contract** that guarantees
index identity from scene load through shader access.

### Invariants

1. **Material texture indices are stable.** `GPUMaterial::textureIndices.x/y/z`
   and `extraIndices.x` refer to scene texture slots as assigned at load time.
   No code path may compact, reorder, or shift these indices.

2. **Every slot has a valid descriptor.** If a texture's source data is absent
   or upload fails, its slot is filled with a 1x1 fallback image (opaque
   white or transparent black). The descriptor array has no gaps.

3. **Env and CDF descriptors occupy explicit non-material slots.** They are
   appended after all material textures, at indices returned by the loader
   and stored in `SceneResources`. Materials never reference these slots.

4. **Descriptor count = total slots.** The descriptor array size equals
   `materialTextureCount + envMapCount + cdfCount`. No skipping.

### Data Flow

```
SceneLoader → ECSScene.textures[N]
             → ECSScene.materials[M].baseColorTextureIndex ∈ [0, N)
             ↓
BuildGPUSceneDataFromECS → GPUSceneData.textures[N]
                         → GPUSceneData.materials[M].textureIndices ∈ [0, N)
                         → GPUSceneData.envMapIndex ∈ [0, N) or -1
                           (env map is a scene texture at its original slot)
                         ↓
SceneResources::SetScene → AsyncTextureLoader.Begin(textures[N],
                            envMapIndex, CDFs)
                          → Loader creates GpuImage[N] in-order
                          → Appends CDF textures at indices [N, N+cdfCount)
                          → Returns: envMapIdx = envMapIndex (unchanged),
                            margCDF = N, condCDF = N+1
                         ↓
PollTextureUpload → Adopt → m_Textures[N + cdfCount]
                  → m_EnvMapIndex = envMapIndex (unchanged)
                  → m_MarginalCDFIndex = N, m_ConditionalCDFIndex = N+1
                         ↓
RendererGPU::UpdatePathTraceDescriptorSet
  → for each m_Textures[i]: write descriptor slot i (no skipping)
  → descriptorCount = m_Textures.size()
  → validate: max material texture index < m_Textures.size()
  → validate: envMapIndex < m_Textures.size() or -1
  → validate: CDF indices < m_Textures.size() or -1
```

### Key Change: No Env Map Extraction

**Current (broken):** `SetScene` moves `envTex.floatPixels` out of
`sceneData.textures[envMapIndex]`, then erases that entry from the list,
shifting all subsequent indices. The extracted float pixels are passed
separately to the loader, which appends a new entry at the end. Material
texture indices are never remapped.

**Proposed:** `SetScene` passes `sceneData.textures` as-is to the loader —
no move, no erase. The env map entry stays at its original index with its
`floatPixels` intact. The loader checks `tex.isHDR` per-entry and handles
format accordingly. CDF textures are appended after all scene textures.

## Proposed Binding Layout (Set 0)

Move texture array from binding 11 to binding 18 (the new highest binding).
This enables `VARIABLE_DESCRIPTOR_COUNT_BIT_EXT` on the texture array.

**Important:** The binding array passed to `vkCreateDescriptorSetLayout` is
indexed by array position, not binding number. A sparse layout (bindings
0-10, 12-18 with 11 missing) must have 18 entries (not 19), and
`pBindingFlags` is indexed by array position, not binding number.

| Array Index | Binding # | Name | Type | Count | Flags |
|-------------|-----------|------|------|-------|-------|
| 0 | 0 | OUTPUT_IMAGE | Storage image | 1 | |
| 1 | 1 | CAMERA_UBO | Uniform buffer | 1 | |
| 2 | 2 | MATERIAL_BUFFER | Storage buffer | 1 | |
| 3 | 3 | VERTEX_BUFFER | Storage buffer | 1 | |
| 4 | 4 | TLAS | Accel structure | 1 | |
| 5 | 5 | INDEX_BUFFER | Storage buffer | 1 | |
| 6 | 6 | NORMAL_BUFFER | Storage buffer | 1 | |
| 7 | 7 | UV_BUFFER | Storage buffer | 1 | |
| 8 | 8 | INSTANCE_MESH_INFO | Storage buffer | 1 | |
| 9 | 9 | LIGHT_BUFFER | Storage buffer | 1 | |
| 10 | 10 | INSTANCE_TRANSFORMS | Storage buffer | 1 | |
| 11 | 12 | INSTANCE_TRANSFORMS_PREV | Storage buffer | 1 | |
| 12 | 13 | INSTANCE_MATERIAL_INDICES | Storage buffer | 1 | |
| 13 | 14 | RESERVOIR_HISTORY | Storage buffer | 1 | |
| 14 | 15 | RESERVOIR_SCRATCH | Storage buffer | 1 | |
| 15 | 16 | SURFACE_HISTORY | Storage buffer | 1 | |
| 16 | 17 | INSTANCE_MAT_OFFSETS | Storage buffer | 1 | |
| 17 | 18 | TEXTURE_ARRAY | Combined image sampler | MAX | VARIABLE_COUNT \| PARTIALLY_BOUND |

`bindingCount = 18` (array entries, not highest binding + 1).

## Implementation Plan

### Phase 1: Fix TextureSlot contract (fixes known texture-index corruption, removes one likely crash contributor)

**Goal:** Establish index identity from loader to shader. This is the
highest priority — it fixes the wrong-texture bug and removes the env-map
extraction as a crash contributor. A validation-layer glTF run is required
after Phase 1 to determine whether device loss is fully resolved (the
binding-count mismatch is an independent crash contributor).

**1a. Stop extracting env map from texture list**

`SceneResources.cpp` `SetScene()`:
- Remove the entire env-map extraction block (lines 102-125):
  - Remove the `envMapFloat` move from `sceneData.textures[envMapIndex]`
  - Remove the `baseTextures.erase(envMapIndex)` logic
  - Remove the separate `envMapFloat` / `envMapW` / `envMapH` variables
- Pass `sceneData.textures` intact to `AsyncTextureLoader::Begin()`
- Pass `sceneData.envMapIndex` as a parameter so the loader knows which
  entry is the env map (it still needs `isHDR`/`floatPixels` to choose
  the format, but does not move or re-append it)

`AsyncTextureLoader.h` `Begin()`:
- Replace `envMapFloatPixels` / `envMapWidth` / `envMapHeight` parameters
  with `int envMapIndex` (pass-through, no extraction)
- The loader creates GpuImages for all `textures[]` entries in-order,
  checking `tex.isHDR` per-entry for format selection
- CDF textures are appended after `textures.size()` entries
- `Adopt()` returns `envMapIndex` = the passed-through value (unchanged)

`AsyncTextureLoader.cpp` `WorkerThread()`:
- Remove the env-map-append step (section 1, lines 187-198)
- The env map is already in `textures[]` at its original index
- `envMapIndex` = the value passed through from `sceneData.envMapIndex`
- CDF textures appended at indices `[textures.size(), textures.size()+2)`

**1b. Don't skip textures with no view in descriptor update**

`RendererGPU.cpp` `UpdatePathTraceDescriptorSet()`:
- Replace the skip loop with: for every `m_Textures[i]`, write descriptor
  slot i. If `gt.view` is null, use a fallback 1x1 image.
- Create the fallback image once at init and reuse it.

**1c. Create a fallback texture**

`SceneResources` or `RendererGPU`:
- Create a 1x1 R8G8B8A8 white image at init
- Use it for any texture slot where the GpuImage has no view
- **Known limitation:** A single opaque-white fallback is not neutral for
  normal maps (sampling white as tangent-space normal produces a non-flat
  direction) or emissive textures (white instead of black). This is
  acceptable initially for slot-identity correctness. A follow-up can use
  role-aware fallbacks: white for base color, flat-normal (128,128,255) for
  normal maps, black for emissive, appropriate metallic/roughness defaults.

**1d. AsyncTextureLoader API change**

`AsyncTextureLoader::Begin()` signature changes from separate env map
float pixels/dimensions to a pass-through `int envMapIndex`. `Adopt()`
returns the same `envMapIndex` unchanged. CDF indices are
`textures.size()` and `textures.size()+1`.

**1e. Add bounds checks before descriptor update**

Before `vkUpdateDescriptorSets`, validate:
- `m_Textures.size() <= currentLayoutTextureCapacity` (1000 in current
  fixed layout)
- Every nonnegative material texture index in
  `GPUSceneData.materials[].textureIndices` and `extraIndices` is
  `< m_Textures.size()`
- `envMapIndex` is -1 or `< m_Textures.size()`
- `marginalCDFIndex` / `conditionalCDFIndex` are -1 or
  `< m_Textures.size()`

On violation: log scene/material/slot context and fail scene synchronization
cleanly (skip descriptor update, set `m_NeedsASRebuild = false` to avoid
retry loops). Do not submit an invalid write.

### Phase 2: Descriptor schema table (replaces hardcoded arrays)

**Goal:** Single source of truth for binding definitions. Derive layout,
flags, and writes from one table.

**2a. Define schema in PathTracePass.h or a new DescriptorSchema.h**

```cpp
struct BindingDef {
    uint32_t binding;
    VkDescriptorType type;
    uint32_t descriptorCount;  // 1 for non-array, MAX for texture array
    VkShaderStageFlags stageFlags;
    VkDescriptorBindingFlagsEXT flags;
};

static const std::vector<BindingDef> s_Set0Bindings = {
    {0,  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, allGraphicsRTFlags, 0},
    {1,  VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, allGraphicsRTFlags, 0},
    // ...
    {18, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_TEXTURES,
        allGraphicsRTFlags,
        VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT_EXT |
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT_EXT},
};
```

**2b. Derive layout from schema**

`PathTracePass::Init()`:
- Build `std::vector<VkDescriptorSetLayoutBinding>` from `s_Set0Bindings`
- Build `std::vector<VkDescriptorBindingFlagsEXT>` from `s_Set0Bindings`
- `bindingCount = s_Set0Bindings.size()` (18, not 19)

**2c. Derive writes from schema**

`PathTracePass::UpdateDescriptorSet()`:
- Use `std::vector<VkWriteDescriptorSet>`
- Declare `VkDescriptorBufferInfo` / `VkDescriptorImageInfo` as local
  variables (stable addresses)
- Build writes vector referencing them
- No gaps, no index arithmetic

### Phase 3: VARIABLE_DESCRIPTOR_COUNT (optional)

**Goal:** Allocate only the texture slots actually needed, not a fixed max.

**Requirements:**
- Layout binding 18 declares `descriptorCount = MAX_TEXTURES` (e.g. 4096)
  — this is the device-supported maximum, not the per-scene count
- Allocation uses
  `VkDescriptorSetVariableDescriptorCountAllocateInfoEXT` with
  `descriptorSetCount = 1` and `pDescriptorCounts = &actualTextureCount`
- **Descriptor set must be re-allocated** when texture count increases
  beyond the allocated count
- **GPU lifetime protocol:** Before freeing the old descriptor set,
  `vkDeviceWaitIdle` (current approach, simple) or defer retirement to
  frame fence (complex, deferred)

**Descriptor pool sizing:**
- Walnut's pool has 1000 COMBINED_IMAGE_SAMPLER descriptors — insufficient
  for large scenes with VARIABLE_DESCRIPTOR_COUNT
- RT2 should create its own descriptor pool for set 0, sized to
  `MAX_TEXTURES * MAX_SETS`, separate from ImGui/Walnut's pool
- `MAX_TEXTURES` must be bounded by both:
  - `VkPhysicalDeviceLimits::maxPerStageDescriptorSampledImages`
  - `VkPhysicalDeviceLimits::maxDescriptorSetSampledImages`
- If `UPDATE_AFTER_BIND` flags are added later, query the
  `...UpdateAfterBind...` variants instead. The current plan does not use
  `UPDATE_AFTER_BIND`, so the dedicated pool does not need
  `VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT`.

### Phase 4: Dead code removal

Only after Phase 1-3 are validated:

- Remove `m_InstanceMaterialIndexBuffer` / `m_InstanceMaterialIndexBufferMemory`
  from `SceneResources` (verified: never bound, `GetInstanceMaterialIndexBuffer()`
  has no callers)
- Remove `CreateTextures()`, `CreateEnvMapCDFTextures()`,
  `DestroyEnvMapCDFTextures()`
- Remove `GetInstanceMaterialIndexBuffer()` accessor

### Phase 5: Diagnostic logging removal

- Remove `[UpdateDS]` step-by-step logging + `fflush` from `PathTracePass.cpp`
- Remove `[UpdateDS]` logging + `fflush` from `RendererGPU.cpp`
- Remove `totalStaging`/`usePerTexture` logging from `AsyncTextureLoader.cpp`
  (already partially cleaned up)

## Risk Assessment

| Phase | Risk | Mitigation |
|-------|------|------------|
| 1 (TextureSlot) | Medium — changes loader + descriptor update | Validation-layer glTF run; check texture identity + device loss |
| 2 (Schema table) | Low — same data, cleaner structure | Compile + run existing tests |
| 3 (Variable count) | Medium — reallocation + pool sizing | Defer unless needed; fixed 4096 works for now |
| 4 (Dead code) | Low — verified no callers | Full repo grep before deletion |
| 5 (Logging) | Low — pure deletion | Only after Phase 1 validation run is clean |

## Implementation Order

1. **Phase 1a-1e:** Fix TextureSlot contract (fixes texture-index corruption, removes one crash contributor)
2. **Build + validation-layer glTF run** — verify texture identity, check for device loss
3. **Phase 4:** Remove dead code (after Phase 1 validates the path)
4. **Phase 5:** Remove diagnostic logging (only after validation run is clean)
5. **Phase 2:** Descriptor schema table (architectural improvement)
6. **Phase 3:** VARIABLE_DESCRIPTOR_COUNT (optional, only if needed)

## Files Changed

| File | Phase | Changes |
|------|-------|---------|
| `SceneResources.cpp` | 1a | Remove entire env-map extraction block (move + erase); pass textures intact to loader |
| `AsyncTextureLoader.cpp/.h` | 1a,1d | Replace env float/dimensions params with `int envMapIndex` pass-through; don't append env map; CDFs appended after all textures |
| `RendererGPU.cpp` | 1b,1e | Don't skip no-view textures; use fallback; add bounds checks before `vkUpdateDescriptorSets` |
| `SceneResources.h/.cpp` | 1c | Create fallback 1x1 texture at init |
| `shader_interface.h` | 2 | `SI_BINDING_TEXTURE_ARRAY` 11 → 18 |
| `PathTracePass.cpp/.h` | 2 | Schema-driven layout + vector writes |
| `SceneResources.h/.cpp` | 4 | Remove dead code |
| `RendererGPU.cpp` | 4 | Remove `GetInstanceMaterialIndexBuffer()` call |
| `PathTracePass.cpp` | 5 | Remove `[UpdateDS]` logging |
| `RendererGPU.cpp` | 5 | Remove `[UpdateDS]` logging |

## What NOT to Change

- Set 1 (G-buffer) descriptor layout — separate, stable, 11 bindings
- ComposePass descriptor layout — separate, 8 bindings
- ReSTIRPass / RasterPass / GBufferDebugPass — use set 0 + set 1 layouts,
  no own layouts