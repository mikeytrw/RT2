# Task: Implement Phase 1 of the Descriptor Refactor — TextureSlot Contract

## Context

You are working on RT2, a Vulkan ray tracer. Read the refactor documentation first:

```
C:\Users\mikey\source\repos\RT2\docs\descriptor_refactor.md
```

Read the **entire** document, then implement **Phase 1 only** (sections 1a through 1e). Do not implement Phases 2-5.

## Problem

Two bugs corrupt texture indices:

1. **Env map extraction shifts indices** — `SceneResources::SetScene()` moves the env map's float pixels out of `sceneData.textures[envMapIndex]`, then erases that entry from the list. All textures after `envMapIndex` shift down by 1. Material texture indices (which reference original positions) become wrong.

2. **Descriptor update skips no-view textures** — `RendererGPU::UpdatePathTraceDescriptorSet()` skips `GpuImage` entries with `!gt.view`, compacting the descriptor array. Descriptor slot N no longer maps to material texture index N.

## What to Implement

### 1a. Stop extracting env map from texture list

**File: `RT2App/src/SceneResources.cpp` — `SetScene()` (line 84)**

Remove the entire env-map extraction block (lines 102-125):
- Remove `envMapFloat`, `envMapW`, `envMapH` variables and the block that moves `envTex.floatPixels`
- Remove `baseTextures` and the `erase(envMapIndex)` logic
- Pass `sceneData.textures` intact to `m_TextureLoader.Begin()`
- Pass `sceneData.envMapIndex` as a parameter to the loader

Update the "no textures" check (line 129): check `sceneData.textures.empty()` instead of `baseTextures.empty() && envMapFloat.empty()`.

Update the `m_TextureLoader.Begin()` call (line 150) to use the new API (see below).

Keep the CDF extraction (`marginalCDF`, `conditionalCDF`, `cdfW`, `cdfH` from `sceneData`) — that stays the same.

**File: `RT2App/src/AsyncTextureLoader.h` — `Begin()` signature (line 49)**

Change from:
```cpp
bool Begin(const GpuDevice& dev,
           const std::vector<SceneTexture>& textures,
           const std::vector<float>& envMapFloatPixels,
           int envMapWidth, int envMapHeight,
           const std::vector<float>& marginalCDF,
           const std::vector<float>& conditionalCDF,
           int cdfWidth, int cdfHeight);
```

To:
```cpp
bool Begin(const GpuDevice& dev,
           const std::vector<SceneTexture>& textures,
           int envMapIndex,
           const std::vector<float>& marginalCDF,
           const std::vector<float>& conditionalCDF,
           int cdfWidth, int cdfHeight);
```

Remove the `m_EnvMapFloatPixels`, `m_EnvMapWidth`, `m_EnvMapHeight` member variables (lines 108-110). Add `int m_EnvMapIndex = -1` member.

Update `WorkerThread` signature: remove `std::vector<float> envMapFloat` parameter, add `int envMapIndex`.

**File: `RT2App/src/AsyncTextureLoader.cpp` — `Begin()` (line 14)**

Update to match new signature. Store `m_EnvMapIndex = envMapIndex`. Pass it to `WorkerThread`.

Update the "no textures" check: `textures.empty()` only (no env float check).

**File: `RT2App/src/AsyncTextureLoader.cpp` — `WorkerThread()` (line 175)**

Remove section 1 (lines 187-198) — the env-map-append step. The env map is already in `textures[]` at its original index. Set `envMapIndex` from the parameter (not by appending).

The existing per-texture loop already handles HDR via `tex.isHDR && !tex.floatPixels.empty()` — no change needed there.

The CDF `hasCDF` check (line 472) uses `envMapIndex >= 0` — this still works since `envMapIndex` is now the pass-through value.

CDF indices: `marginalCDFIndex = (int)textures.size()`, `conditionalCDFIndex = (int)textures.size() + 1` — same as before since CDFs are still appended after all textures.

Set `m_ResultEnvMapIndex = envMapIndex` (the passed-through value).

### 1b. Don't skip textures with no view in descriptor update

**File: `RT2App/src/RendererGPU.cpp` — `UpdatePathTraceDescriptorSet()` (line 212)**

Replace the texture loop (lines 237-246):
```cpp
// CURRENT (broken — skips entries, shifts indices):
for (const auto& gt : m_Scene.GetTextures())
{
    if (!gt.view) continue;  // BUG: compacts the array
    ...
    textureImageInfos.push_back(imgInfo);
}
```

With:
```cpp
// FIXED — one descriptor per slot, fallback for missing views:
for (const auto& gt : m_Scene.GetTextures())
{
    VkDescriptorImageInfo imgInfo = {};
    imgInfo.sampler = m_Scene.GetTextureSampler();
    imgInfo.imageView = gt.view ? gt.view : m_FallbackTexture.view;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    textureImageInfos.push_back(imgInfo);
}
```

### 1c. Create a fallback texture

**File: `RT2App/src/RendererGPU.h`**

Add member: `GpuImage m_FallbackTexture;`

Add method declaration (if not inline): `void CreateFallbackTexture();`

**File: `RT2App/src/RendererGPU.cpp`**

In `Init()` (after device setup, before pipeline creation), create a 1x1 opaque-white R8G8B8A8_UNORM texture:
```cpp
void RendererGPU::CreateFallbackTexture()
{
    uint8_t white[4] = {255, 255, 255, 255};
    GpuResources::CreateImage(m_Device, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_FallbackTexture);
    // Upload white pixel + transition to SHADER_READ_ONLY_OPTIMAL
    // Use ImmediateSubmit + a staging buffer + vkCmdCopyBufferToImage + barrier
}
```

Call `CreateFallbackTexture()` in `Init()` after `m_Scene.InitSamplers()`.
Destroy `m_FallbackTexture` in `Destroy()` via `GpuResources::DestroyImage(m_Device, m_FallbackTexture)`.

### 1d. AsyncTextureLoader API change

This is covered by 1a above. The key points:
- `Begin()` takes `int envMapIndex` instead of separate float/dimensions
- `WorkerThread()` takes `int envMapIndex` instead of `std::vector<float> envMapFloat`
- `Adopt()` returns the same `envMapIndex` (unchanged pass-through)
- No env-map append step in the worker

### 1e. Add bounds checks before descriptor update

**File: `RT2App/src/RendererGPU.cpp` — `UpdatePathTraceDescriptorSet()`**

Before the `m_PathTracePass.UpdateDescriptorSet()` call, add:

```cpp
// Bounds check: texture count must not exceed layout capacity
const uint32_t MAX_TEXTURES = 1000;  // matches PathTracePass layout
if (textureImageInfos.size() > MAX_TEXTURES)
{
    RT_LOG("[UpdateDS] ERROR: texture count %zu exceeds layout capacity %u — skipping descriptor update",
           textureImageInfos.size(), MAX_TEXTURES);
    return;
}

// Bounds check: material texture indices must be < texture count
uint32_t texCount = (uint32_t)m_Scene.GetTextures().size();
const auto& scene = m_Scene.GetScene();
for (size_t i = 0; i < scene.materials.size(); i++)
{
    const auto& mat = scene.materials[i];
    // Check baseColor, normal, emissive, metallicRoughness texture indices
    int indices[] = { mat.textureIndices.x, mat.textureIndices.y, mat.textureIndices.z, mat.extraIndices.x };
    const char* names[] = { "baseColor", "normal", "emissive", "metallicRoughness" };
    for (int j = 0; j < 4; j++)
    {
        if (indices[j] >= 0 && (uint32_t)indices[j] >= texCount)
        {
            RT_LOG("[UpdateDS] ERROR: material %zu %s texIdx=%d >= texCount=%u — skipping descriptor update",
                   i, names[j], indices[j], texCount);
            return;
        }
    }
}

// Bounds check: env/CDF indices
if (m_Scene.GetEnvMapIndex() >= 0 && (uint32_t)m_Scene.GetEnvMapIndex() >= texCount)
{
    RT_LOG("[UpdateDS] ERROR: envMapIndex=%d >= texCount=%u — skipping", m_Scene.GetEnvMapIndex(), texCount);
    return;
}
```

## Files to Modify

| File | Changes |
|------|---------|
| `RT2App/src/SceneResources.cpp` | Remove env-map extraction block in `SetScene()`; pass textures + envMapIndex to loader |
| `RT2App/src/AsyncTextureLoader.h` | Change `Begin()` signature; remove env float members; add `m_EnvMapIndex` |
| `RT2App/src/AsyncTextureLoader.cpp` | Update `Begin()`, `WorkerThread()`; remove env-map append; pass-through envMapIndex |
| `RT2App/src/RendererGPU.cpp` | Fix texture loop (no skip); create fallback texture; add bounds checks |
| `RT2App/src/RendererGPU.h` | Add `m_FallbackTexture` member + `CreateFallbackTexture()` |

## What NOT to Change

- Do NOT change `shader_interface.h` or any shader files
- Do NOT change `PathTracePass.cpp` (descriptor layout stays at binding 11, count 1000)
- Do NOT remove diagnostic logging (that's Phase 5, after validation)
- Do NOT remove dead code (that's Phase 4)
- Do NOT change `SceneManager.cpp` — it already appends env map to `gpuData.textures` correctly at `envMapIndex = textures.size() - 1`

## Build & Test

Build command:
```
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" "C:\Users\mikey\source\repos\RT2\RT2App.sln" /p:Configuration=Release /p:Platform=x64 /t:RT2App /m /v:minimal
```

Test command:
```
& "C:\Users\mikey\source\repos\RT2\bin\Release-windows-x86_64\RT2Tests\RT2Tests.exe"
```

All 141 tests must pass. After build, the user will run a validation-layer glTF scene load to verify texture identity and check for device loss.

## Key Code Paths to Understand

- `SceneManager::SyncToGPU()` (SceneManager.cpp:171) — builds `GPUSceneData`, appends env map as a `SceneTexture` at `textures.size()-1`, sets `envMapIndex`. This is correct and should NOT change.
- `SceneResources::SetScene()` (SceneResources.cpp:84) — currently extracts env map (BUG), passes to loader. This is what you're fixing.
- `AsyncTextureLoader::WorkerThread()` (AsyncTextureLoader.cpp:175) — currently appends env map at end (BUG). Already has per-entry `isHDR` check for format selection. Fix: remove append, use pass-through index.
- `RendererGPU::UpdatePathTraceDescriptorSet()` (RendererGPU.cpp:212) — currently skips no-view textures (BUG). Fix: use fallback.
- `GPUSceneData` (GPUSceneData.h:115) — `textures[]` array, `envMapIndex` into it, `marginalCDF`/`conditionalCDF` as float vectors (not textures).

## Important Notes

- The env map is a `SceneTexture` with `isHDR=true` and `floatPixels` populated. The loader already handles this per-entry via `tex.isHDR && !tex.floatPixels.empty()` at line 205 and 309. No special handling needed — just don't extract it.
- `SetSceneKeepTextures()` (SceneResources.cpp:44) preserves existing textures and sets `copy.envMapIndex = m_EnvMapIndex`. This path is unaffected since it doesn't touch the loader.
- The `Adopt()` signature does NOT change — it still returns `envMapIndex`, `marginalCDFIndex`, `conditionalCDFIndex`. Only the values change (envMapIndex is now pass-through, not appended).
- `PollTextureUpload()` (SceneResources.cpp:157) calls `Adopt()` and stores results. No changes needed there — it already stores the returned indices.