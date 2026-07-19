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

Programmatic editor-camera changes are atomic `EditorCameraPose` cuts. Frame,
focus, bookmark recall, View Through Camera, and numeric pose/optics edits all
use the same application path. A successful cut resets accumulation/NRD and
invalidates ReSTIR DI and GI history exactly once; it does not dirty the scene
or invoke a scene GPU sync.

Frame Selected uses cached object-space `MeshData` bounds transformed to world
space. It unions selected hierarchy roots and all descendants even when hidden.
Entities without render geometry contribute a 0.5 m cube at their world origin,
so an empty-only selection remains finite. Frame fits both projection axes;
Focus rotates in place. Shortcuts are `F` / `Shift+F`.

Nine editor-only bookmark slots store position, forward, vertical FOV,
aperture, focus distance, and far clip. `Ctrl+1..9` recalls and
`Ctrl+Shift+1..9` stores. They are cleared on document adoption and are not
part of `.rt2scene`; camera movement speed is also session-only.

Authored camera entities use `Transform` as the authoritative pose.
`CameraComponent::forwardDirection` remains serialized for v2 compatibility
but is maintained as a derived world-space mirror after generic transform and
hierarchy mutations. On document adoption, legacy files are reconciled once
from the stored direction and then Transform is authoritative. View Through
Camera copies an entity pose into the editor camera; Align Camera to View writes
the editor pose back through the entity's parent. A singular or sheared
parent-relative conversion is rejected atomically.

On Play, RT2 snapshots the current editor pose. The runtime chooses the
`CameraComponent` with the lowest UUID deterministically; if none exists, the
legacy scene-level `ECSScene::camera` pose remains the fallback. Stop restores
the exact pre-Play editor pose.

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

---

## Editor settings, recovery, and session lifecycle (Phase 1B)

### EditorSettingsStore

Per-user editor preferences stored as a versioned JSON file under
`<appDataRoot>/settings.json` (production: `%LOCALAPPDATA%\RT2\Editor`).
Tests inject a temporary directory so they never touch the developer's
LocalAppData.

Schema (version 1):

```json
{
  "version": 1,
  "projectRoot": "<absolute path or empty>",
  "recentScenes": ["<absolute .rt2scene path>", ...]
}
```

- Atomic write: tmp + MoveFileExW/ReplaceFileW. A failed write leaves the
  previous valid file intact.
- Recents: native `.rt2scene` files only, most-recent-first, bounded
  (default 10), normalized + deduplicated case-insensitively on Windows.
  Updated only after a successful native open or save. Failed/cancelled
  operations do not change the list.
- Project root: optional editor preference used as an initial file-dialog
  location. Does NOT reinterpret the Phase 1A scene-relative asset-reference
  contract.
- Unsupported versions and malformed files produce useful diagnostics and
  safe defaults. Unknown optional fields are ignored.

### SceneRecoveryService

Crash-safe authoring recovery. Recovery is crash protection, NOT version
history. One atomically-replaced record per logical document. At most
`MaxRecords` (default 8) document records exist globally; the oldest are
evicted only AFTER a new record commits successfully. The just-written
record is never evicted.

Storage layout: one `<fnv1a64(docId)>.rt2recovery` JSON envelope directly
under `recoveryRoot`. Hashing the complete normalized document identity avoids
long-path truncation collisions; discovery validates that the stored `docId`
hash matches the filename. The file contains both the recovery metadata and a
nested schema-v2 scene snapshot. Asset references are still relativized
against the ORIGINAL logical scene root via `SceneSerializer::SaveTo`, not
against the recovery directory.

Envelope fields (the nested `snapshot` is the complete `.rt2scene` JSON
object, not a second file):

```json
{
  "version": 1,
  "docId": "<normalized case-folded absolute path or untitled:id>",
  "untitled": false,
  "originalSourcePath": "<empty when untitled>",
  "assetRoot": "<directory asset references were relativized against>",
  "revision": 0,
  "createdAt": 0,
  "snapshot": { "schemaVersion": 2, "...": "..." }
}
```

Autosave scheduling (`MaybeSnapshot`):

- Called once per frame from the host's `OnUpdate` while in Edit state.
- Only snapshots the authoring document — never the runtime Play clone.
- Guards: `doc.metadata.dirty == true`, interval elapsed (default 60s,
  injectable) after the first dirty observation, and revision changed since
  the last snapshot. The first dirty frame starts the timer; it does not write
  immediately. No work occurs on clean frames or unchanged revisions.
- Does NOT clear dirty state, change `sourcePath`, update recents, or reset
  UUIDs. Never overwrites the explicit `.rt2scene`.
- Writes one temp sibling and atomically replaces the envelope with
  `ReplaceFileW`/`MoveFileExW`. A failed write leaves the previous valid
  recovery intact.
- Autosave is deliberately synchronous on the main thread for Phase 1B.
  Success reports elapsed milliseconds; failures and writes above the 10 ms
  guardrail surface non-modal diagnostics without terminating the app.

Restore (`Restore(record, outDoc, diagnostics, err)`):

- Transactional: loads + resolves into a temporary document using the
  recorded `assetRoot` (NOT the recovery directory). Only on success does
  it swap into `outDoc`.
- The restored document is marked dirty.
- Preserves `metadata.sourcePath` from the record (empty when untitled).
- UUIDs, hierarchy, components, camera, lights, environment references,
  imported model references, and material overrides are preserved.
- On failure (parse, resolution, or IO), the live document is untouched and
  the recovery record remains available.

Discard:

- `Discard(record)` deletes only a directly contained `.rt2recovery` file;
  path-containment validation prevents deletion outside the recovery root.
  The explicit scene file is untouched.
- `DiscardForDoc(docId)` after successful Save/Save As/Discard.
- `OnSaveAs(oldId, newId)` retires both identities so a post-Save-As crash
  does not resurrect the pre-Save-As state.

Doc identity:

- Titled documents: normalized case-folded absolute `sourcePath`.
- Untitled documents: a stable per-session recovery UUID string.

Authoring revision counter:

- `SceneManager::AuthoringRevision()` bumps on every authoring mutation via
  `NotifyAuthoringChanged()`. Used by the recovery service to skip
  rewriting identical snapshots. Not serialized into `.rt2scene`.

### UnsavedChangesCoordinator

Pure state machine for New/Open/Recent/Exit requests against a document that may
have unsaved authoring changes. No ImGui dependency. The host wires
`IsDirtyQuery`, one unified `SaveGate`, `ExecuteGate`, and
`DiscardRecoveryGate` callbacks.

- `Request(action)` when clean → execute immediately.
- `Request(action)` when dirty → queue one pending action; a second request
  while pending is rejected.
- `ResolveSave()` → unified save gate; the host chooses Save or Save As from
  the document's source path. On success it executes the pending action; on
  failure or dialog cancellation it retains that action.
- `ResolveDiscard()` → execute pending + clear recovery for the abandoned
  document.
- `ResolveCancel()` → clear pending, no mutations.

### Window-close integration

Walnut's `Application::RequestClose()` is an interactive, cancelable close
that fires a `CloseRequestCallback`. The OS title-bar close button / Alt+F4
is routed through a `glfwSetWindowCloseCallback` that cancels the GLFW-level
close and calls `RequestClose()` instead. Headless/internal completion
still uses `Application::Close()` directly.

### Production user-data locations

- Settings: `%LOCALAPPDATA%\RT2\Editor\settings.json`
- Recovery: `%LOCALAPPDATA%\RT2\Editor\Recovery\<doc-id-hash>.rt2recovery`

Tests override both by constructing the stores/services with a temporary
directory. No core logic hardcodes these paths.

## Editor selection and viewport picking

Selection is transient editor state and uses stable `UUID` values rather than
`entt::entity`. `EditorSelection` preserves ordered multi-selection; its final
entry is primary. The Inspector resolves that UUID through the active
authoring `SceneDocument` each frame, and stale IDs are pruned. Selection is
not part of `.rt2scene`, runtime cloning, autosave, recovery, or `GPUSceneData`.

### Coordinate and ray contract

`ViewportImageRect` records the ImGui image's desktop-space top-left,
displayed size, and actual render extent. `ScreenToRenderPixel` treats right
and bottom edges as exclusive and returns a top-left-origin render pixel.
The pixel centre is converted to normalized UV before calling
`Camera::GetPickingRay`.

Picking rays are deterministic pinhole rays built from the camera inverse
projection and inverse view. They intentionally do not call
`GetRayOriginAndDirection`, because that path samples the aperture for depth
of field and would make editor selection stochastic.

### Instance identity boundary

`BuildGPUSceneDataFromECS` and `UpdateInstancesFromECS` optionally emit a
`RenderInstanceMap` alongside the GPU DTO. Entry `i` is the entity UUID for
GPU instance `i`. The map is passed through the SceneManager sync callback to
RendererGPU but remains outside `GPUSceneData`; shaders never consume UUIDs.

This boundary must remain atomic: code that changes GPU instance ordering must
change map construction in the same iteration. Never reconstruct the mapping
later from a second registry traversal.

### Frame-safe GPU result

`GpuPickingPass` dispatches one compute invocation using `GL_EXT_ray_query`
against the renderer's existing TLAS. It confirms opaque intersections and
applies the same MASK alpha cutoff; BLEND uses deterministic 0.5 coverage for
selection. The result contains hit state, instance custom index, hit distance,
and world position.

Each frame-in-flight slot owns a host-coherent result buffer and the exact
`RenderInstanceMap` snapshot captured when its query was recorded. RendererGPU
reads the slot only after `FrameContext::WaitForFence` for that same slot, then
resolves the instance index against the captured map. This adds no explicit
GPU stall. A monotonically increasing request serial discards older results
after a newer click. Resize or scene/instance updates cancel outstanding
requests.

Viewport selection is accepted only while the runtime controller is in Edit
state. A live UUID lookup is still required before adopting the result, because
an entity may have been deleted while the GPU query was in flight. A miss
clears selection.

## Hierarchy mutation and visibility contract

`Hierarchy::parent` is persisted and authoritative. `Hierarchy::children` is
an eager traversal cache only. `SceneHierarchy::RebuildChildren` clears and
reconstructs that cache after load/adoption; `SceneHierarchy::Validate` rejects
missing parents, self-parenting, cycles, duplicate cache entries, and any cache
entry that disagrees with the child's parent. Runtime traversal, dirtiness
propagation, subtree collection, and Outliner expansion consume the cache.

Editor hierarchy mutations are UUID-keyed and atomic. `CreateEmpty`, `Reparent`,
`RemoveSubtrees`, `SetVisibility`, `DuplicateSubtrees`, and `PasteSubtreesFrom`
return `EditorMutationResult`, containing a diagnostic, affected UUIDs, and the
single renderer `SyncImpact`. Callers must perform that sync once after success;
they must not infer a second sync from the affected UUID list.

Reparent defaults to preserve-world. All sources are resolved and reduced to
topmost selected roots before cycle and transform validation. Local transforms
are calculated before relationship changes; if any conversion is singular or
contains shear, nothing is committed. Selecting both an ancestor and descendant
therefore never moves or deletes the descendant twice.

Visibility is effective through the ancestor chain: an entity renders only if
its own `VisibleComponent` and every parent's component are visible. Both
`BuildGPUSceneDataFromECS` and `UpdateInstancesFromECS` use
`SceneVisibility::CollectVisibleRenderables`; this is the required ordering
boundary for `gpu.instances[i]` and `RenderInstanceMap[i]`. Hidden mesh emitters
are absent from emissive-triangle light extraction as a consequence.

Locks, selection, search text, and clipboard are editor-only state. Locks are
direct-only: an operation targeting a locked entity is rejected, while moving
or deleting an unlocked ancestor may include locked descendants. Clipboard
copy deep-clones the document in memory, records selected root UUIDs and
document/resource generations, and paste creates fresh UUIDs. New/open/recovery
document adoption clears this state; resource compaction makes an older
clipboard stale rather than risking transient-index corruption.

## Editor transform contract

`EditableTRS` is the editor-facing affine transform type. Its decomposition is
strict: singular scale, non-finite values, and shear are rejected. World-space
editing converts the desired world matrix through the parent world matrix and
only commits when the resulting local matrix is representable as TRS.

`SceneManager::TrySetWorldTransforms` validates an entire selection first. If
every desired world transform converts successfully, all local transforms are
committed and the hierarchy is marked dirty once; otherwise none are changed.
This is the mutation path used by the viewport gizmo.

The Inspector and gizmo share local/world space, primary/median/individual
pivot, and translation/rotation/scale snapping settings. Shared-pivot edits use
`D = currentPivot * inverse(startPivot)` followed by `world[i]' = D * world[i]`.
Individual mode applies an equivalent delta around each entity's own pivot.

Gizmo handles use a four-pixel drag threshold. Press/release without crossing
that threshold is forwarded to GPU picking, so an axis overlay never prevents
selection of the object underneath. Crossing the threshold starts manipulation
and cancels outstanding pick work. Successful edits invoke only
`SyncTransformsToGPU`; they do not rebuild BLAS geometry or upload textures.
