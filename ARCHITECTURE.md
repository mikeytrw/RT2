# RT2 — Architecture Overview

A minimal Vulkan ray-traced rendering engine with a raster-first hybrid path,
ReSTIR DI sampling, and NRD denoising. Designed as a foundation for a small
game engine with future scripting and physics extensions.

---

## Design Goals

- **Minimal**: One rendering path, one scene representation, no legacy duplication
- **Raster-first hybrid**: Rasterizer produces the G-buffer (primary visibility),
  ray tracing handles secondary rays and lighting. Cheaper than pure path tracing
  while still physically based.
- **Real-time path tracing quality**: ReSTIR DI for direct lighting, NRD REBLUR
  for denoising, 1 path per pixel
- **Data-driven**: ECS (entity-component-system) as the sole scene representation
- **Extensible**: Clear hooks for scripting, physics, and asset streaming

---

## System Diagram

```
┌─────────────────────────────────────────────────────────┐
│                    Application Layer                      │
│  Walnut (window, input, ImGui) ← RT2Layer (game loop)    │
└────────────────────────┬────────────────────────────────┘
                         │
          ┌──────────────┼──────────────┐
          ▼              ▼              ▼
   ┌─────────────┐ ┌───────────┐ ┌────────────┐
   │   Scene     │ │  Renderer │ │  (Future)  │
   │  Manager    │ │   GPU     │ │  Scripting │
   │             │ │           │ │  Physics   │
   │ ECSScene    │ │ RasterPass│ │            │
   │ MeshReg     │ │ PathTrace │ │            │
   │ SceneLoader │ │ ReSTIR    │ │            │
   │ SceneGraph  │ │ NRD       │ │            │
   │             │ │ Compose   │ │            │
   └──────┬──────┘ └─────┬─────┘ └────────────┘
          │              │
          ▼              ▼
   ┌─────────────┐ ┌───────────┐
   │ GPUSceneData│ │ GpuDevice │
   │ (bridge)    │ │ VkDevice  │
   │             │ │ VkQueue   │
   │ Materials   │ │ Buffers   │
   │ Instances   │ │ Images    │
   │ Meshes      │ │ Pipelines │
   └─────────────┘ └───────────┘
```

---

## Module List

### Scene Layer

| Module | File(s) | Responsibility |
|--------|---------|----------------|
| **SceneManager** | `SceneManager.h/.cpp` | Owns scene state, syncs to GPU via callbacks, handles loading |
| **ECSScene** | `ECSScene.h` | entt registry + MeshRegistry + material/texture arrays |
| **ECSComponents** | `ECSComponents.h` | Transform, MeshRef, Hierarchy, Light, Camera, Name, Visible components |
| **MeshRegistry** | `MeshRegistry.h` | Object-space mesh storage (vertices, indices, normals, UVs) |
| **SceneLoader** | `SceneLoader.h/.cpp` | glTF (.gltf/.glb) and OBJ (.obj+.mtl) loading into ECS |
| **SceneGraph** | `SceneGraph.h/.cpp` | Resolves Transform hierarchy to world matrices |
| **GPUSceneData** | `GPUSceneData.h/.cpp` | Bridge: ECS → POD structs for GPU upload (materials, instances, lights) |

### Rendering Layer

| Module | File(s) | Responsibility |
|--------|---------|----------------|
| **RendererGPU** | `RendererGPU.h/.cpp` | Top-level orchestrator: owns device, passes, frame ring, settings |
| **FrameRenderer** | `FrameRenderer.h/.cpp` | Static per-frame command buffer recorder (barriers, dispatch order) |
| **RasterPass** | `RasterPass.h/.cpp` | Graphics pipeline for G-buffer (8 MRT, depth, dynamic rendering) |
| **PathTracePass** | `PathTracePass.h/.cpp` | RT pipeline, SBT, descriptor set layout (set 0) |
| **ReSTIRPass** | `ReSTIRPass.h/.cpp` | ReSTIR DI temporal + spatial compute pipelines |
| **ComposePass** | `ComposePass.h/.cpp` | Compute shader: remodulates NRD output with material albedo/F0 |
| **GBufferDebugPass** | `GBufferDebugPass.h/.cpp` | Compute shader: visualizes G-buffer channels for debugging |
| **NRDIntegration** | `NRDIntegration.h/.cpp` | NVIDIA Real-time Denoisers wrapper (REBLUR diffuse+specular) |
| **GBufferTarget** | `GBufferTarget.h/.cpp` | Creates/resizes the 8 G-buffer images + depth |
| **SceneResources** | `SceneResources.h/.cpp` | Owns GPU scene resources: AS, materials, lights, transforms, textures |
| **AccelerationStructure** | `AccelerationStructure.h/.cpp` | BLAS/TLAS building, vertex/index buffer management |
| **ReservoirResources** | `ReservoirResources.h/.cpp` | ReSTIR reservoir buffers (history, scratch, surface history) |
| **GpuTimestampProfiler** | `GpuTimestampProfiler.h/.cpp` | GPU timestamp queries for per-region timing |

### Infrastructure

| Module | File(s) | Responsibility |
|--------|---------|----------------|
| **GpuDevice** | `GpuDevice.h/.cpp` | Vulkan device, queue, RT extension dispatch, buffer addresses |
| **GpuResources** | `GpuResources.h/.cpp` | Buffer/image creation helpers, memory allocation |
| **GpuImage** | `GpuImage.h` | Simple VkImage + memory + view wrapper |
| **FrameContext** | `FrameContext.h/.cpp` | Per-frame-in-flight command buffer, fence, submit |
| **CommandUtils** | `CommandUtils.h/.cpp` | ImmediateSubmit helper for one-off GPU commands |
| **AsyncTextureLoader** | `AsyncTextureLoader.h/.cpp` | Async texture upload via staging arena + fence |
| **StagingArena** | `StagingArena.h` | Bump-allocator for staging buffer uploads |
| **ShaderManager** | `ShaderManager.h` | Loads .spv shader files from disk |
| **Camera** | `Camera.h/.cpp` | FPS camera with keyboard/mouse input, projection/view matrices |
| **RenderSettings** | `RenderSettings.h` | All tunable parameters (NRD, ReSTIR, bounces, SPP, etc.) |
| **CLIArgs** | `CLIArgs.h` | Command-line argument parsing |

### Shaders

| File | Stage | Responsibility |
|------|-------|----------------|
| `raster.vert` | Vertex | G-buffer vertex transform + jitter + prev-position output |
| `raster.frag` | Fragment | G-buffer fill: normal, roughness, viewZ, motion, albedo, emission |
| `raygen.rgen` | Ray-gen | RT-primary path (traces full camera rays) |
| `secondary_raygen.rgen` | Ray-gen | Raster-first path (reads G-buffer, traces secondary rays only) |
| `closesthit.rchit` | Closest-hit | Material evaluation, GGX scatter, NEE, recursive trace |
| `anyhit.rahit` | Any-hit | Alpha-tested transparency (MASK materials) |
| `miss.rmiss` | Miss | Environment map sampling (sky) |
| `shadow.rmiss` | Miss | Shadow ray miss (unoccluded → lit) |
| `shadow.rahit` | Any-hit | Shadow ray hit (occluded → shadowed) |
| `pathtracer_shared.glsl` | Include | All set 0/1 declarations, Material/TriangleLight/RayPayload structs, BRDF, NEE, NRD helpers |
| `scatter_shared.glsl` | Include | Scatter logic, ReSTIR DI sampling, computeNEE |
| `restir_shared.glsl` | Include | ReSTIR reservoir struct + operations, oct encoding, target density |
| `restir_bindings.glsl` | Include | ReSTIR compute pass bindings + RNG |
| `restir_temporal.comp` | Compute | ReSTIR temporal reuse (initial candidates + history merge) |
| `restir_spatial.comp` | Compute | ReSTIR spatial reuse (neighbor merge) |
| `compose.comp` | Compute | NRD output remodulation + tone mapping |
| `gbuffer_debug.comp` | Compute | G-buffer visualization |
| `shader_interface.h` | Include | Shared C++/GLSL struct definitions + binding constants |

---

## What's Being Removed (Planned Refactor)

See `docs/refactor-plan.md` for the full ordered work list.

1. **CPU renderer** — `Renderer`, `Mesh`, `Hittable`, `BVHNode`, `Ray`, `Sphere`, `Triangle`, `AABB`, `Material` (CPU types). The GPU renderer is the only renderer.
2. **Legacy `Scene` class** — flat mesh arrays replaced by `ECSScene` as the sole scene representation.
3. **Combined per-triangle buffers** — `BuildCombinedBuffers` and its 4 SSBOs (normal/position/UV/tangent) eliminated. Closest-hit shader fetches attributes directly from per-mesh vertex/index/normal/UV buffers via `gl_PrimitiveID`.
4. **Per-triangle data in BLASData** — `triPositions`, `triUVs`, `triTangents` removed. BLAS only stores vertex/index buffers for AS construction.
5. **Per-triangle data in GPUMeshGeometry** — `vertexUVs` (6 floats/tri) and `tangents` (9 floats/tri) removed. UVs and normals stored per-vertex (as in MeshData). Tangents computed in shader.

This eliminates ~75% of duplicated geometry memory, enabling large scenes (San Miguel 5.6M tris, Bistro 3.8M tris) to load without OOM.

---

## What Stays

- **Raster-first hybrid path** — rasterizer for primary visibility, RT for secondary rays
- **RT-primary path** — `raygen.rgen` traces full camera rays (kept for flexibility, not default)
- **ReSTIR DI** — temporal + spatial reservoir resampling for direct lighting
- **NRD REBLUR** — diffuse + specular denoising
- **ECS scene** — entt registry with Transform, MeshRef, Hierarchy components
- **2 frames in flight** — fence-synced command buffer ring
- **Async texture loading** — staging arena + fence-based completion polling
- **GPU timestamp profiling** — per-region timing (raster, ReSTIR, RT, NRD, compose)

---

## Future Extension Points

See `docs/future-extensions.md` for detailed placeholder designs.

- **Scripting** — entity behavior scripts (OnUpdate/OnSpawn) integrated into the game loop
- **Physics** — rigid body simulation with ECS Transform sync
- **Scene serialization** — save/load ECS state
- **Asset streaming** — async mesh/texture loading with LOD
- **Upscaling** — FSR 2 / DLSS integration for high-resolution output