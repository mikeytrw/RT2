# Scene Management

How scenes are represented, loaded, and synced to the GPU.

---

## Scene Representation

### SceneDocument (authoring + runtime unit)

```
SceneDocument
  ├─ ECSScene ecs                   — entity-component scene
  ├─ EnvironmentSettings environment — env map path + cached HDR pixels + dims
  ├─ SceneMetadata metadata          — schema version, source path, dirty flag, name
  ├─ UuidIndex uuidIndex             — UUID -> entity lookup
  └─ GPUSceneData gpuCache           — per-document CPU cache (no Vulkan types)
```

`SceneDocument` is the serializable, cloneable unit of scene state. It unifies
what was historically split across `ECSScene` and `SceneManager`-private
environment state. The serializer, the runtime clone, and the slice runner all
operate on one self-contained `SceneDocument` instead of a raw `ECSScene` plus
loose manager state.

`ECSScene` remains the entity/mesh/material/texture/camera container inside
`SceneDocument`, but it is no longer described as the "sole representation" —
the document also owns the environment map, metadata, UUID index, and a CPU
GPU-scene cache.

### ECSScene

```
ECSScene
  ├─ entt::registry registry       — entity storage
  ├─ MeshRegistry meshRegistry     — unique mesh geometry (object space)
  ├─ vector<SceneMaterial> materials
  ├─ vector<SceneTexture>  textures
  └─ SceneCamera           camera
```

`SceneMaterial`, `SceneTexture`, and `SceneCamera` are shared POD structs
defined in `Scene.h`.

### ECS Components

| Component | Fields | Purpose |
|-----------|--------|---------|
| `Transform` | translation (vec3), rotation (quat), scale (vec3), worldMatrix (mat4), prevWorldMatrix (mat4), dirty (bool) | Local TRS + computed world matrix |
| `Hierarchy` | parent (entity), children (vector<entity>) | Parent-child relationship for scene graph |
| `MeshRef` | meshIndex (uint32), materialIndex (int, -1 = use per-triangle material indices) | Reference to MeshRegistry entry + material |
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

Tangents are NOT stored — they are computed in the shader from UV gradients.

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
indices are used so each primitive looks up its material independently —
the OBJ loader sets `materialIndex = -1` to indicate per-triangle materials.
glTF loaders set explicit material overrides per primitive.

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
  │    ├─ For each mesh: copy vertices + indices + normals + UVs as per-vertex buffers
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
- Projection: `glm::perspective` with `GLM_FORCE_DEPTH_ZERO_TO_ONE` (Vulkan depth range)
- Near plane: 0.1, far plane: 10000
- Jitter: Halton(2,3) sequence, applied in raster vertex shader (clip-space offset)
- When ReSTIR is enabled, NRD jitter is disabled via a greyed-out UI toggle
  (jitter causes temporal wobble in ReSTIR reprojection)

The scene camera (`SceneCamera` in ECSScene) stores position/forward/FOV from

---

## Native scene persistence (.rt2scene)

The `.rt2scene` format is RT2's native authoring format, distinct from glTF
(which remains the interchange/export path). It is a versioned JSON file
handled by `SceneSerializer`.

### Schema version 1 (vertical slice — read-only migration input)

- **Version field**: `{"version": 1, ...}`. v1 scenes are accepted by the
  loader and migrated in memory to the v2 representation without changing
  entity UUIDs, hierarchy UUID references, transforms, material identity, or
  camera. The serializer always writes v2 on save, so v1 inputs are migrated
  on the next save.
- v1 supports primitive meshes only (`PrimitiveComponent`). Non-primitive
  meshes were rejected with `Error{UnknownPrimitive}`.

### Schema version 2 (Phase 1A — asset-backed native scene round-trip)

- **Version field**: `{"version": 2, ...}`. Unsupported versions fail with
  `Error{SchemaVersion}`. Supported read range is `1..2`.
- **Entities**: serialized by UUID, sorted for deterministic output. Each
  entity carries: `uuid`, `name`, `parent` (UUID or empty), `transform`
  (translation, quaternion xyzw, scale), `visible`, and optional component
  blocks (`primitive`, `meshRef`, `importedSource`, `materialOverride`,
  `light`, `camera`, `motion`).
- **Procedural meshes**: `PrimitiveComponent` entities are directly
  serializable. The serializer rebuilds their geometry on load.
- **Imported meshes**: durable provenance is stored in an
  `importedSource` block containing an `AssetReference` (`kind`, portable
  scene-relative `path`, `sourceKey`, `importSettings`). The serializer does
  NOT write decoded vertex buffers, pixel data, GPU handles, or transient
  `MeshRef::meshIndex` values. `SceneAssetResolver` rebuilds meshes,
  materials, and textures from the durable references after a structural
  load.
- **Material overrides**: an optional `materialOverride` block records
  authored edits to an imported material. The block stores a FULL
  `SceneMaterial` value snapshot (`material`), an `authored` flag, and the
  durable `sourceMaterialKey`. The `materialIndex` field is transient and is
  not persisted as identity. Precedence: when `authored` is true, the
  resolver appends the override material to the document's materials array
  and points the entity's `MeshRef` at the new slot, overriding the re-
  imported source material. Editor mutation paths
  (`SceneManager::SetMaterial`, `SetMaterialProperties`, and the inline
  material editor in `SceneEditorUI`) create or update
  `MaterialOverrideComponent` on every imported entity they touch, so saved
  UI edits are never discarded by re-import on reopen.
- **MeshRef**: only `materialIndex` is serialized. `meshIndex` is transient
  runtime state and is repaired by the resolver on load.
- **Lights**: `LightComponent` entities only. The legacy `ECSScene::lights`
  array is the glTF interchange representation and is NOT serialized by the
  native format.
- **Materials**: full round-trip (PBR parameters, emissive, alpha mode,
  texture indices).
- **Camera**: `SceneCamera` (position, forward, FOV, aperture, focus).
- **Environment**: `envMap.path` is a portable, scene-relative UTF-8 path.
  Pixels are NOT serialized; `SceneAssetResolver::ResolveEnvironment` re-reads
  the file on load and verifies both dimensions and non-empty decoded float
  pixels before the GPU scene is built.
- **Textures**: the schema includes the array for forward-compatibility; the
  slice does not serialize texture pixel data. Textures are rebuilt from the
  source model by the resolver.
- **Paths**: asset reference paths and the environment path are stored as
  portable, scene-relative UTF-8 (forward slashes, normalized) wherever
  possible. They are resolved relative to the `.rt2scene` file's directory at
  load time. Absolute machine-specific paths are NOT persisted unless the
  asset is on a different drive and cannot be relativized.

### Save validation (v2)

The serializer rejects scenes that cannot reopen: every entity with a
`MeshRef` must have either a `PrimitiveComponent` (procedural) or an
`ImportedMeshSourceComponent` (durable asset reference). Entities with
neither produce `Error{UnknownPrimitive}` listing the offending UUIDs. This
prevents silently saving a native scene that cannot be loaded back.

### Load (two-pass, transactional, resolution-decoupled)

1. Parse JSON, check schema version (accepts v1 and v2).
2. Parse entity records (pass 0 — no entity creation).
3. Pass 1: create entities + components by UUID, build UUID index.
4. Pass 2: resolve parent UUIDs to `Hierarchy`. Missing parent →
   `Error{MissingParent}`. Duplicate UUID → `Error{DuplicateUuid}`.
5. Load into a temporary document; only on success does the caller swap it
   in as the live authoring scene. A parse/schema failure cannot corrupt the
   live scene.
6. The serializer does NOT resolve external assets. After a successful load,
   the caller runs `SceneAssetResolver::ResolveAll` to rebuild meshes,
   textures, and materials from durable references, and
   `SceneAssetResolver::ResolveEnvironment` to decode the environment map.
   Missing assets produce `AssetDiagnostic` entries but do not corrupt the
   UUID/entity hierarchy.

### Save (atomic, deterministic)

- Write to `path + ".tmp"`, then atomically replace the target via
  `ReplaceFileW` (or `MoveFileExW` on Windows). On failure, the existing
  file is left intact.
- Entities sorted by UUID, fixed float precision (`%.9g`), stable key order
  for readable source-control diffs.

### CloneInMemory

`SceneSerializer::CloneInMemory` reuses the same two-pass component visitor
as `Load` but without file I/O. It preserves authored UUIDs exactly and does
NOT clone transient runtime state (GPU cache, dirty flag, prevWorldMatrix,
renderer temporal history). `RuntimeSceneController::Play` uses this to deep-
clone the authoring `SceneDocument` into a runtime document.

### SceneDocument

`SceneDocument` unifies what was historically split across `ECSScene` and
`SceneManager`-private state:
- `ECSScene ecs` — entities, mesh registry, materials, textures, lights, camera
- `EnvironmentSettings environment` — env map path + cached pixels + dims
- `SceneMetadata metadata` — schema version, source path, dirty flag, name
- `UuidIndex uuidIndex` — UUID → entity lookup, maintained alongside the registry
- `GPUSceneData gpuCache` — per-document CPU cache (no Vulkan types)

### Dirty tracking

`SceneManager` exposes `IsDirty()`/`MarkDirty()`/`ClearDirty()`. All
authoring mutations (Add/Remove/SetTransform/SetMaterial/SetMaterialProperties/
SetEntityName/SetLightProperties/SetMeshRefMeshIndex/AddMaterial) call
`NotifyAuthoringChanged()` which marks the scene dirty. The editor UI checks
`IsDirty()` for unsaved-changes prompts. Material edits must go through
`SetMaterialProperties()` rather than mutating the reference returned by
`GetMaterial()` so dirty tracking and the correct sync path are invoked.

### ISceneRenderBridge

Scene code communicates with the renderer through `ISceneRenderBridge`, a
narrow interface with `FullSync`/`MaterialSync`/`TransformSync`/
`ResetTemporalState`/`RequestRender`. `SceneManager` and
`RuntimeSceneController` call this interface instead of `RendererGPU`
directly. RT2App provides `SceneRenderBridge` (backed by `RendererGPU`);
RT2Tests and RT2SliceRunner supply a null/recording implementation. This
keeps scene core code free of Vulkan/Walnut/ImGui dependencies.

### SceneAssetResolver (Phase 1A)

`SceneAssetResolver` resolves durable asset references into a `SceneDocument`
after a structural load. It is the single place that calls `SceneLoader`
(glTF/OBJ) and the EXR/HDR loader, so `SceneSerializer` never depends on the
importer. It remains Vulkan/Walnut/ImGui/GLFW/NRD/NRI-free and links cleanly
into RT2Tests and RT2SliceRunner.

- `ResolveAll(doc, sceneRoot, diagnostics, err)` walks entities with
  `ImportedMeshSourceComponent`, loads each referenced model once through
  `SceneLoader` into a staging `ECSScene`, maps durable source keys to
  rebuilt mesh/material/texture indices, merges staged resources into the
  target document, and installs/repairs `MeshRef` components. For each
  entity with an authored `MaterialOverrideComponent`, it appends the
  override material value to the document's materials array and points the
  entity's `MeshRef` at the new slot, so saved UI edits survive reopen.
- `ResolveEnvironment(doc, sceneRoot, diagnostics, err)` reads the
  environment map file (HDR/EXR) and fills `floatPixels`/dimensions. It
  verifies both dimensions and non-empty decoded pixels.
- Paths are resolved relative to `sceneRoot` (the `.rt2scene` directory).

### Missing-asset policy (Phase 1A)

Missing external assets are a distinct, user-visible diagnostic state, not a
crash and not silent data loss:

- The document stays structurally valid: the UUID/entity hierarchy,
  transforms, visibility, camera, and any resolved entities are preserved.
- The missing reference is recorded as an `AssetDiagnostic` (severity
  `Missing`, `Malformed`, or `Unresolved`) identifying the referring entity
  (UUID + name), the expected path, the resolved path attempted, the source
  key, and a human-readable detail.
- The affected entity is left without a resolved `MeshRef` (or with a
  placeholder). The renderer sees zero meshes for that entity rather than
  stale geometry.
- If every imported entity is unresolvable, `ResolveAll` returns false with
  `Error{MissingAsset}` so the caller can surface a clear message. Partial
  resolution returns true with diagnostics so the scene remains usable.
- Environment failure clears stale pixels (so the renderer does not sample
  half-decoded data) but preserves the path reference so a later successful
  reload can reattach.

### Import provenance (Phase 1A)

Durable source identity is attached at import time so the native scene can
rebuild the same mesh/material/texture association after reopening:

- **glTF**: `ImportedMeshSourceComponent::model.sourceKey` is
  `"gltf:scene=<s>:node=<n>:mesh=<m>:primitive=<p>"` (indices into the
  source glTF file, stable across loads). The key does NOT depend on EnTT
  entity values or current `MeshRegistry` ordering.
- **OBJ**: the merged mega-mesh uses `"obj:whole-model"` and a persisted
  importer profile (`ImportSettings`: triangulate, generateNormals,
  mergeMegaMesh) sufficient to recreate the geometry.
- `MeshRef::meshIndex` is transient runtime state. Its numeric value is
  never treated as persistent identity; the serializer writes only
  `materialIndex` and the resolver repairs `meshIndex` on load.

### v1 → v2 migration

v1 primitive-only scenes load and are migrated in memory to the v2
representation without changing entity UUIDs, hierarchy UUID references,
transforms, material identity, or camera. The only observable difference is
the version field written back on save (v2). v1 scenes carry no asset
references, so no resolution is needed for them.