# Future Extensions

Placeholder designs for systems that will be added to RT2 as it evolves from
a rendering engine into a minimal game engine. These are not implemented yet —
this document defines where they would plug in and what the interfaces would
look like.

---

## Scripting

### Purpose

Allow per-entity behavior logic without hardcoding into the engine. Scripts
respond to events (spawn, update, destroy) and can modify entity transforms,
spawn/despawn entities, and adjust material parameters.

### Integration Point

In the game loop, scripting runs after camera input and before rendering
(see `docs/game-loop.md`):

```
OnUpdate(ts):
  Camera::OnUpdate(ts)
  ScriptSystem::OnUpdate(ts)     // ← scripting hook
  [PhysicsSystem::Step(ts)]      // ← physics (if present)
  SceneGraph::UpdateWorldTransforms()
  Render()
```

### ECS Component

```cpp
struct ScriptComponent
{
    // Callback-based (C++) or function pointer for native scripts
    std::function<void(entt::entity, float)> onUpdate;
    std::function<void(entt::entity)> onSpawn;
    std::function<void(entt::entity)> onDestroy;
};
```

### Script System

```cpp
class ScriptSystem
{
public:
    void OnUpdate(entt::registry& reg, float ts)
    {
        auto view = reg.view<ScriptComponent>();
        for (auto entity : view)
        {
            auto& script = view.get<ScriptComponent>(entity);
            if (script.onUpdate)
                script.onUpdate(entity, ts);
        }
    }
};
```

### Future: Lua scripting

For a data-driven workflow, embed Lua (or a subset) and expose:
- Entity access (get/set Transform, MeshRef, material parameters)
- Scene queries (find entities by name, raycast)
- Spawn/despawn entities
- Timer/coroutine support

Lua scripts would be loaded from `.lua` files referenced by a `LuaScriptComponent`:

```cpp
struct LuaScriptComponent
{
    std::string scriptPath;
    sol::table environment;  // per-entity Lua state
};
```

### UI

The outliner would show a "Add Script" option for selected entities, with a
file picker for `.lua` scripts or a dropdown of registered native script types.

---

## Physics

### Purpose

Rigid body dynamics (gravity, collisions, constraints) with ECS integration.
Transforms are driven by the physics simulation and synced to the render
transform.

### Integration Point

Physics steps with a fixed timestep (deterministic) using an accumulator
pattern (see `docs/game-loop.md`):

```
OnUpdate(ts):
  Camera::OnUpdate(ts)
  accumulator += ts
  while (accumulator >= FIXED_DT):
    PhysicsSystem::Step(FIXED_DT)
    accumulator -= FIXED_DT
  SceneGraph::UpdateWorldTransforms()
  Render()
```

### ECS Components

```cpp
struct RigidBodyComponent
{
    glm::vec3 velocity = {0, 0, 0};
    glm::vec3 angularVelocity = {0, 0, 0};
    float mass = 1.0f;
    float restitution = 0.5f;
    float friction = 0.5f;
    bool  isStatic = false;
    // Collider handle (index into collider registry)
    uint32_t colliderIndex = 0;
};

enum class ColliderShape { Sphere, Box, Capsule, ConvexMesh };

struct ColliderComponent
{
    ColliderShape shape = ColliderShape::Sphere;
    // Shape-specific params (union or separate fields)
    float radius = 0.5f;        // sphere
    glm::vec3 halfExtents;      // box
    // For convex mesh: reference to MeshData for hull generation
};
```

### Physics System

Two options:

**Option A: Built-in (minimal)**
- Broadphase: sweep-and-prune or simple grid
- Narrowphase: GJK + EPA for convex shapes
- Solver: sequential impulses (Box2D-style for 3D)
- Collision detection against scene BVH for triangle-mesh colliders
- Pros: no dependencies, full control, good for learning
- Cons: significant work, hard to get right

**Option B: Third-party (Jolt or JBX)**
- [Jolt Physics](https://github.com/jrouwe/JoltPhysics) — mature, MIT-licensed, used by Horizon Forbidden West
- Integrate as a submodule, wrap in `PhysicsSystem`
- Pros: production-quality, less code to maintain
- Cons: dependency, larger binary

**Recommendation**: Start with Option A (simple sphere/box broadphase + narrowphase)
for the minimal engine. Add Jolt if complex simulation is needed.

### Transform Sync

After `PhysicsSystem::Step`:
1. Physics writes to `RigidBodyComponent::velocity` and integrates position
2. Position is written to `Transform::translation`, `Transform::dirty = true`
3. `SceneGraph::UpdateWorldTransforms` resolves world matrices
4. `SceneManager::SyncTransformsToGPU` uploads to GPU (or the render loop
   picks up dirty transforms automatically)

### Collisions with Scene Geometry

For collision against ray-traced scene meshes (walls, floors, objects):
- Use the existing BLAS to do ray casts (GPU) or BVH traversal (CPU)
- Or build a separate physics BVH from MeshData (simplified/convex hulls)
- Physics queries: `raycast(origin, dir) → hit`, `sweep(shape, start, end) → hit`,
  `overlap(shape, pos) → entities`

### UI

- "Add RigidBody" and "Add Collider" in the entity outliner
- Physics settings panel: gravity, fixed timestep, debug visualization
- Debug draw: wireframe colliders, contact points, velocity arrows (rendered
  as overlay on the viewport, not through the RT pipeline)

---

## Scene Serialization

### Purpose

Save and load the full ECS scene state (entities, transforms, meshes,
materials, textures) to/from a file format. Enables scene editing workflows.

### Format

Use JSON (via tinygltf's JSON parser or nlohmann/json) for readability, or
binary for speed. A custom `.rt2scene` format:

```json
{
  "version": 1,
  "camera": { "position": [0,1,10], "forward": [0,0,-1], "fov": 45 },
  "materials": [ ... ],
  "textures": [ "path/to/texture.png", ... ],
  "meshes": [ { "name": "mesh0", "vertices": [...], "indices": [...], ... } ],
  "entities": [
    {
      "name": "Cube",
      "transform": { "t": [0,0,0], "r": [0,0,0,1], "s": [1,1,1] },
      "meshRef": { "meshIndex": 0, "materialIndex": 0 },
      "children": [ ... ]
    }
  ]
}
```

Alternatively, extend the glTF exporter (`SceneLoader::Save`) to write the
full ECS state as a `.gltf` file with custom extensions for RT2-specific data
(ReSTIR settings, NRD parameters, env map).

---

## Asset Streaming

### Purpose

Load textures and meshes asynchronously to avoid load-time stalls on large
scenes. The current `AsyncTextureLoader` handles textures; mesh streaming
would extend this to geometry.

### Design

- **Texture streaming**: Already implemented via `AsyncTextureLoader` + `StagingArena`
- **Mesh streaming**: Load `MeshData` on a background thread, upload to GPU
  via staging buffer, adopt when complete
- **LOD**: Store multiple resolution meshes per `MeshRegistry` entry. Select
  based on distance to camera. Lower LODs have fewer triangles → cheaper BLAS.
- **Virtual texturing**: For very large texture sets (> VRAM), use tiled
  virtual textures with on-demand page loading (like SVT/Megatexture)

### Integration

- `SceneManager` would track a "load queue" of pending assets
- Each frame, `PollAssetUpload()` checks for completed uploads and adopts them
- The renderer would handle "missing asset" gracefully (render a placeholder
  or skip the entity until its assets are ready)

---

## Upscaling (FSR 2 / DLSS)

### Purpose

Render at lower resolution (e.g., 1080p) and upscale to display resolution
(e.g., 4K) for higher effective FPS. This is critical for path tracing at
high resolutions.

### FSR 2 Integration

[FSR 2](https://github.com/GPUOpen-Tools/FidelityFX-FSR2) is MIT-licensed and
works on all GPUs. It requires:
- Current color buffer (the RT output image)
- Depth buffer (G-buffer viewZ or depth attachment)
- Motion vectors (G-buffer motion attachment)
- Previous color buffer (temporal history)
- Jitter offsets (already computed for NRD)

RT2 already produces all of these for NRD, so FSR 2 integration is
straightforward — add it as a compute pass between compose and output
transition.

### DLSS Integration

[DLSS](https://github.com/NVIDIA/DLSS) requires NVIDIA's Streamline SDK and
NDA. It provides higher quality than FSR 2 on RTX GPUs. Integration would be
optional (compile-time flag) since it's NVIDIA-only.

### Pipeline Change

```
... → NRD Denoise → Compose → [FSR 2 Upscale] → Output Transition
```

The render resolution (RT + NRD) would be 1080p; the display resolution
(FSR 2 output) would be 4K. Motion vectors and depth must be at render
resolution.