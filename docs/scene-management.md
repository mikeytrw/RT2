# Scene Management

How scenes are represented, loaded, and synced to the GPU.

---

## Scene Representation

### ECSScene (sole representation, post-refactor)

```
ECSScene
  ├─ entt::registry registry       — entity storage
  ├─ MeshRegistry meshRegistry     — unique mesh geometry (object space)
  ├─ vector<SceneMaterial> materials
  ├─ vector<SceneTexture>  textures
  └─ SceneCamera           camera
```

The legacy `Scene` class (flat `SceneMesh` array) is being removed. ECSScene
is the only scene representation. `SceneMaterial`, `SceneTexture`, and
`SceneCamera` are shared POD structs defined in `Scene.h` (to be moved to
their own header during refactor).

### ECS Components

| Component | Fields | Purpose |
|-----------|--------|---------|
| `Transform` | translation (vec3), rotation (quat), scale (vec3), worldMatrix (mat4), prevWorldMatrix (mat4), dirty (bool) | Local TRS + computed world matrix |
| `Hierarchy` | parent (entity), children (vector<entity>) | Parent-child relationship for scene graph |
| `MeshRef` | meshIndex (uint32), materialIndex (int) | Reference to MeshRegistry entry + material |
| `LightComponent` | color, intensity, range, direction, innerCone, outerCone | Point/spot light (CPU-side, not emissive triangles) |
| `CameraComponent` | verticalFOV, aperture, focusDistance, forwardDirection | Camera metadata for scene file round-tripping |
| `NameComponent` | name (string) | Entity name for outliner display |
| `VisibleComponent` | (empty) | Marker: entity should be rendered |

### MeshRegistry

Stores unique mesh geometry in object space. Multiple entities (instances)
reference the same mesh by index — this is how instancing works. One BLAS is
built per registered mesh.

```
MeshData
  ├─ vector<float>    vertices   — position.xyz, stride 3
  ├─ vector<uint32_t> indices    — triangle indices
  ├─ vector<float>    normals    — normal.xyz, stride 3 (may be empty)
  ├─ vector<float>    uvs        — texcoord.xy, stride 2 (may be empty)
  └─ string           name       — for debugging
```

Tangents are NOT stored — they are computed in the shader from UV gradients
(or in `GPUSceneData` pre-processing, post-refactor: computed in shader).

---

## Scene Loading

### glTF Path (.gltf / .glb)

```
SceneLoader::LoadIntoECS(ecsScene, filepath)
  ├─ tinygltf::LoadASCIIFromFile / LoadBinaryFromFile
  ├─ Parse textures (embedded or external, decoded via stb_image)
  ├─ Parse materials (PBR metallic-roughness, emissive, alpha mode, transmission, IOR)
  ├─ Traverse node hierarchy:
  │    ├─ Create entity per node with Transform (TRS from glTF node)
  │    ├─ Create MeshData per primitive (POSITION, NORMAL, TEXCOORD_0, INDICES)
  │    ├─ Add MeshRef component (meshIndex → MeshRegistry, materialIndex)
  │    └─ Recurse into children (Hierarchy component)
  ├─ Handle camera nodes (SceneCamera from perspective + extras)
  └─ SceneGraph::UpdateWorldTransforms (resolve hierarchy)
```

### OBJ Path (.obj + .mtl)

```
SceneLoader::LoadObjIntoECS(ecsScene, filepath)
  ├─ tinyobj::ParseFromFile (triangulate=true)
  ├─ Parse MTL materials → SceneMaterial:
  │    ├─ Kd → baseColor, Ks → metallic (heuristic), Ns → roughness
  │    ├─ Ke → emissive color + intensity
  │    ├─ map_Kd → baseColorTexture, map_Bump → normalTexture
  │    └─ map_Ke → emissiveTexture
  ├─ Load external textures via stb_image (PNG/JPG from MTL directory)
  ├─ Merge all geometry into a single MeshData (mega-mesh):
  │    ├─ Deduplicate vertices (position + normal + texcoord key)
  │    ├─ Generate flat normals if OBJ has no normals
  │    └─ All triangles in one mesh (1 BLAS, minimal memory)
  ├─ Create root entity + single mesh entity with MeshRef
  └─ SceneGraph::UpdateWorldTransforms
```

**Note**: OBJ merging all geometry into one mesh eliminates per-material BLAS
proliferation (San Miguel: 1722 shapes → 1 BLAS). Per-triangle material
assignment is lost (all geometry uses material 0). Future improvement: add a
per-triangle material index buffer so the shader can look up materials per
primitive without needing separate BLASes.

### Import (merge into existing scene)

```
SceneLoader::ImportIntoECS(ecsScene, filepath)
  — Like LoadIntoECS but does NOT clear the scene
  — Offsets material and texture indices to avoid collisions
  — Creates a wrapper root entity parented to the existing scene
```

---

## Scene Graph

`SceneGraph::UpdateWorldTransforms(registry)` resolves the Transform hierarchy:

```
For each root entity (no Hierarchy or parent == null):
  worldMatrix = localMatrix()  // TRS
  For each child:
    child.worldMatrix = parent.worldMatrix * child.localMatrix()
    recurse
```

Called after scene load, after entity transforms change (drag in outliner),
and (in future) after physics step. Marks `dirty = false` after resolution.

Previous-frame world matrices (`prevWorldMatrix`) are preserved for motion
vector computation. They are updated at the start of each frame (copy current
→ prev) before new transforms are applied.

---

## Scene-to-GPU Sync

### Full sync (SyncToGPU)

Called when the scene changes structurally (load, import, entity add/remove,
material edit, texture change):

```
SceneManager::SyncToGPU()
  ├─ UpdateWorldTransforms()
  ├─ BuildGPUSceneDataFromECS(ecsScene)  → GPUSceneData
  │    ├─ Copy textures + materials (convert to GPUMaterial)
  │    ├─ For each mesh: copy vertices + indices
  │    │   (post-refactor: also copy normals + UVs as per-vertex buffers)
  │    ├─ For each entity with MeshRef: create GPUInstance (worldMatrix, meshIndex, materialIndex)
  │    └─ Collect emissive triangles → GPUTriangleLight list
  ├─ Append env map texture + build CDFs
  └─ m_SyncCallback(gpuData)  → RendererGPU::SetScene(gpuData)
       ├─ SceneResources::SetScene — kicks async texture upload
       ├─ Flag AS rebuild (BLAS + TLAS on next Render())
       └─ ResetAccumulation()
```

### Transform-only sync (SyncTransformsToGPU)

Called when only transforms change (entity moved in outliner, animation):

```
SceneManager::SyncTransformsToGPU()
  ├─ UpdateWorldTransforms()
  ├─ UpdateInstancesFromECS(ecsScene, existingGpuScene)  — update world matrices only
  └─ m_InstanceSyncCallback(gpuData)  → RendererGPU::UpdateSceneInstances(gpuData)
       ├─ vkCmdUpdateBuffer (instance transform buffer)
       └─ ResetAccumulation()
       (no BLAS/TLAS rebuild, no texture re-upload)
```

### Keep-textures sync (SyncToGPUKeepTextures)

Called when materials change but textures are unchanged (material parameter
edit, roughness slider):

```
SceneManager::SyncToGPUKeepTextures()
  — Rebuilds GPUSceneData but skips texture upload
  — Faster than full sync (no async texture loader round-trip)
```

---

## Entity Management (SceneManager API)

| Method | Purpose |
|--------|---------|
| `LoadScene(filepath)` | Clear scene + load file (glTF or OBJ) |
| `ImportGltf(filepath)` | Import glTF into existing scene (merge) |
| `AddMaterial(mat)` | Add material, return index |
| `AddObjectWithGeometry(name, meshData, pos, rot, scale, matIdx)` | Create entity with inline geometry |
| `RemoveEntity(id)` | Destroy entity + children + orphaned meshes |
| `CompactMeshRegistry()` | Remove unreferenced meshes (after deletions) |
| `SetEnvMap(path)` | Load HDR env map (stbi_loadf / tinyexr) |
| `DumpInstanceTransforms()` | Debug: print all instance matrices |
| `DumpNEEBuffers()` | Debug: print light buffer contents |

---

## Camera

The `Camera` class (`Camera.h/.cpp`) is an FPS-style controller:
- Right-click + drag to look, WASD/QE to move
- Projection: `glm::perspectiveFov` with `GLM_FORCE_DEPTH_ZERO_TO_ONE` (Vulkan depth range)
- Near plane: 0.1, far plane: 10000
- Jitter: Halton(2,3) sequence, applied in raster vertex shader (clip-space offset)
- When ReSTIR is enabled, NRD jitter is disabled via a greyed-out UI toggle
  (jitter causes temporal wobble in ReSTIR reprojection)

The scene camera (`SceneCamera` in ECSScene) stores position/forward/FOV from
the scene file. On load, `RT2Layer` copies these into the active `Camera`.