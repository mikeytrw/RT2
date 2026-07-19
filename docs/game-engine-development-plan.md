# Lightweight Game Engine Development Plan

Implementation roadmap for evolving RT2 from a renderer with scene-editing
facilities into a small, usable game engine. This plan is ordered around one
goal: close the authoring loop before expanding the renderer further.

The intended loop is:

```
create/import -> edit -> save -> play -> stop safely -> package -> run
```

## Existing foundation

RT2 already provides:

- an EnTT ECS with transforms, hierarchy, mesh references, materials, lights,
  cameras, names, and visibility;
- glTF/GLB and OBJ loading, scene import, primitive creation, and instancing;
- glTF/GLB scene saving with unit-tested geometry, material, texture, light,
  camera, and full-scene round trips;
- an outliner and inspector with transform, material, light, and camera editing;
- efficient full, material-only, and transform-only scene-to-GPU sync paths;
- a defined game loop and fixed-timestep integration point;
- headless rendering, regression scripts, and a Doctest-based `RT2Tests` target.

The glTF save path is useful for interchange and export. A native authoring
format is still needed because engine-only components and editor state should
not be forced into glTF extensions from the outset.

## Engineering principles

These apply across every phase.

1. **Editor and runtime state are distinct.** Playing a scene must not mutate
   the saved authoring scene unless the user explicitly applies a change.
2. **Stable identities cross persistence boundaries.** Serialized references
   use UUIDs or asset IDs, never raw `entt::entity` values or vector indices.
3. **Systems depend on narrow services.** Scripts do not access EnTT, GLFW,
   Vulkan, or ImGui directly.
4. **Scene mutations are centralized.** Editor operations use commands and the
   runtime uses a scene API, allowing validation, undo, dirty tracking, and the
   correct GPU synchronization path.
5. **CPU logic remains testable without a GPU.** Serialization, commands,
   input mapping, script bindings, asset resolution, prefab merging, physics
   mapping, and packaging should be separable from renderer initialization.
6. **Every phase leaves a usable increment.** Features are accepted through a
   small end-to-end workflow, not only isolated classes.

## Cross-phase architecture contracts

These contracts are prerequisites for work in later phases; they are not local
implementation choices.

### Scene mutation, synchronization, and dependency direction

All scene changes flow through the scene API. The editor reaches it through
commands; runtime systems reach it through narrow runtime services. The scene
API determines the only permitted renderer synchronization category:

| Impact | Required SceneManager path | Typical changes |
|---|---|---|
| `None` | No GPU sync | Editor selection, lock state, runtime-only counters |
| `Transform` | Transform-only sync | Rigid movement, hierarchy world-transform updates |
| `Material` | Material-only sync | Material assignment and editable material properties |
| `Structural` | Full scene/acceleration-structure rebuild | Entity, mesh, visibility, light, or topology changes |

```mermaid
flowchart LR
  UI[Editor UI] --> Commands --> SceneAPI[Scene API]
  Runtime[Runtime controller and systems] --> SceneAPI
  Scripts[Lua bindings] --> SceneAPI
  Assets[Asset database] --> Handles[Asset handles] --> SceneAPI
  SceneAPI --> SceneManager --> Sync[Sync-impact policy] --> Renderer
```

No UI panel, script, asset importer, physics backend, or animation backend may
call EnTT mutation paths or renderer synchronization callbacks directly.

### Component ownership and persistence

Every component must be classified before it is introduced:

- **Shared authoring/runtime, persisted:** transform, hierarchy, mesh/material
  references, lights, cameras, and script field values.
- **Editor-only, never persisted in a scene:** selection, editor locks, gizmo
  state, outliner expansion, and editor camera state.
- **Runtime-only, never copied back to authoring:** physics bodies/handles,
  audio voices, Lua environments/timers, animation evaluation caches, and
  transient simulation state.

Runtime cloning copies persisted shared data into a runtime world, constructs
runtime-only state there, and destroys it on Stop. Editor-only state remains
outside both serialized scene data and the runtime world.

### Determinism and performance budgets

Fixed-step systems must define stable entity iteration, deferred-command/spawn
ordering, seed assignment, and event ordering. They must not depend on
unordered container traversal or order-dependent parallel accumulation.
Cross-platform floating-point results are compared with documented per-system
tolerances; exact byte equality is required only where the format or algorithm
is explicitly deterministic.

The vertical slice establishes baselines for Release build time, `RT2Tests`
runtime, headless runtime checks, and render-regression duration. Each later
phase must add at least one automated regression check for each public behavior
it introduces, and must record its measured build/test deltas. Until a project
specific budget replaces them, the default guardrails are: Release build median
within `max(10%, 60 s)` of baseline, `RT2Tests` plus required headless checks
within `max(20%, 30 s)`, and no unexplained renderer benchmark median regression
above 5% on the affected manifest cases. A justified baseline update is allowed
only with the reason and before/after measurements recorded. Renderer
performance work additionally uses the benchmark manifests and frame-time
thresholds in the rendering docs.

`UUID` is a core value type owned by a dedicated engine header/source pair
(`core/UUID.h` and `core/UUID.cpp`), with parsing, formatting, generation,
comparison, and hashing defined there. It is not a SceneManager-local alias.

### UUID generation policy

Entity identity uses RFC 9562 UUID v4 from the OS cryptographic RNG
(`CoCreateGuid`/`UuidCreate` on Windows). Generation is injectable through
`IUuidProvider` so tests and deterministic fixtures use
`DeterministicUuidProvider` while production uses `OsUuidProvider`.

- **Authored entities**: v4 from OS cryptographic RNG. Never use timestamps,
  pointers, `rand()`, EnTT IDs, or render seeds.
- **Asset IDs**: separate stable IDs, persisted in the asset database. They
  survive file renames and moves.
- **Linked imported nodes**: if stable source-node identity is required,
  derive UUID v5 from the asset ID plus a canonical importer node key.
  Ordinary "import into scene" entities receive fresh v4 IDs and retain them
  in the native scene.
- **Runtime cloning**: preserve UUIDs, because the runtime clone represents
  the same logical entities.
- **Duplication**: generate fresh v4 IDs for the duplicated subtree and
  remap internal references.
- **Undo/delete restoration**: restore the original IDs.

The render sampling seed must not influence UUID generation. UUID generation
is a pure identity concern, not a rendering or simulation concern.

## Test strategy

Use four layers of tests throughout the roadmap.

### Unit tests

Add deterministic Doctest cases to `RT2Tests`. Keep engine logic in source
files that can be linked by the test target without initializing Vulkan or a
window. Tests should use temporary directories and tiny programmatically built
scenes where practical.

### Integration tests

Exercise several CPU systems together: save/load round trips, command stacks,
play-scene cloning, script lifecycle, asset dependency resolution, prefab
overrides, and package manifests. These belong in `RT2Tests` when they do not
require an interactive window.

### Headless runtime tests

Extend the existing headless command line for deterministic smoke scenarios.
Return a non-zero exit code on load, script, asset, or runtime assertion
failures. Prefer state assertions or compact JSON reports to image comparison
for gameplay logic. Retain the existing render regression harness for visual
changes.

### Interactive acceptance tests

Some editor behavior cannot be proven economically through unit tests. Each
feature therefore lists a short manual runtime checklist. Keep these checks
small and repeatable; automate them later only when their regression frequency
justifies UI automation.

For all phases, the baseline gate is:

1. build `RT2App` and `RT2Tests` in Release x64;
2. run all `RT2Tests`;
3. run applicable headless smoke/regression tests;
4. complete the phase-specific interactive checklist.

## Phase 1 - Native scene persistence and stable IDs

### Outcome

Users can create a scene, save it, reopen it, and retain all authored state and
cross-entity references. Existing glTF/GLB saving remains an interchange/export
path.

### Work

- Add a stable `EntityIdComponent` containing a generated UUID.
- Add UUID lookup and enforce uniqueness during create, clone, import, and load.
- Define a versioned `.rt2scene` JSON schema.
- Store entity hierarchy and component data by UUID.
- Store source/imported mesh, texture, environment, and future script references
  as project-relative paths or asset IDs.
- Initially permit inline primitive geometry, but do not serialize large imported
  vertex arrays into readable JSON by default.
- Serialize camera, environment, materials, entities, and relevant render/world
  settings. Keep transient GPU and editor selection state out of the scene.
- Implement New, Open, Save, Save As, recent scenes, dirty tracking, and
  save/discard/cancel prompts.
- Write atomically via a temporary sibling file followed by replacement.
- Add a schema migration entry point even while only version 1 exists.
- Add bounded autosave and recovery records after explicit saving is stable;
  recovery must never overwrite the last explicitly saved scene.

### Unit and integration tests

- UUID generation produces non-null unique IDs and survives entity moves.
- Duplicate IDs in an input file are rejected with a useful error.
- Empty, primitive-only, imported-asset, hierarchy, light, camera, material, and
  mixed scenes round-trip structurally.
- Parent and component references resolve after entity creation order changes.
- Unknown optional fields are ignored; unsupported schema versions fail clearly.
- Relative paths are normalized without escaping the project root silently.
- Failed writes leave the previous valid scene intact.
- Recovery discovers an interrupted unsaved session, offers restore/discard, and
  leaves the explicitly saved scene unchanged in both cases.
- Dirty state changes only after authoring commands and clears after successful
  save/load.
- Continue running all existing glTF/GLB round-trip tests.

### Runtime acceptance

- Build a scene containing a parented primitive, imported mesh, material, light,
  and camera; save, restart RT2, reopen, and compare the viewport and inspector.
- Attempt New/Open/Exit with unsaved changes and verify all three prompt choices.
- Temporarily remove a referenced asset and verify a visible placeholder/error,
  not a crash or silent data loss.
- Simulate an interrupted session and verify the recovery prompt restores the
  autosave only when the user chooses it.

### Exit criteria

A non-trivial authored scene survives restart, recovery is available without
overwriting explicit saves, missing assets produce actionable diagnostics, and
existing interchange saving still passes its tests.

## Phase 2 - Scene-building ergonomics and viewport tools

### Outcome

A small level can be assembled efficiently from the viewport and outliner.

### Work

- Implement viewport picking with an entity/instance ID result. Prefer a small
  raster ID attachment or a CPU/GPU ray query with one-frame-safe readback over
  coupling selection to path-tracing samples.
- Add translate, rotate, and scale gizmos.
- Support local/world space, pivot choice, grid/angle/scale snapping, and numeric
  inspector entry.
- Add create-empty, create-child, rename, copy/paste, duplicate hierarchy,
  delete, and drag/drop reparenting.
- Add multi-selection, focus/frame selected, visibility, editor lock, and
  outliner search.
- Validate hierarchy edits against cycles and preserve world transform when the
  chosen reparent mode requests it.
- Add camera bookmarks and align active camera/entity camera to the editor view.

### Unit and integration tests

- Screen-coordinate/picking math maps known IDs and handles background pixels.
- Reparent rejects self-parenting and descendant cycles.
- Reparent preserve-world mode reconstructs the expected local transform.
- Duplicate hierarchy creates new UUIDs and remaps internal references.
- Deleting a hierarchy removes all descendants without invalid iteration.
- Snapping handles negative values, non-uniform scale, and configured increments.
- Locked entities reject editor mutations but remain available to runtime systems.

### Runtime acceptance

- Select overlapping objects from the viewport and outliner.
- Build a simple room using only primitives, gizmos, duplication, snapping, and
  reparenting.
- Verify selection stays correct while the renderer is accumulating and while
  frames are in flight.
- Verify transform edits use the transform-only GPU sync path.

### Exit criteria

The simple room can be built without editing source files or relying primarily
on inspector numeric fields.

## Phase 3 - Command system, undo, and redo

### Outcome

Every normal editor mutation is reversible and drives a deliberate scene/GPU
synchronization category.

### Work

- Introduce `IEditorCommand` with `Execute`, `Undo`, optional merge/coalesce,
  description, and sync impact (`None`, `Transform`, `Material`, `Structural`).
- Route every impact through the cross-phase SceneManager contract: `None` has
  no renderer work, `Transform` uses transform-only sync, `Material` uses
  material-only sync, and `Structural` uses full scene/AS rebuild.
- Add a bounded command history and redo stack.
- Route transform, create, delete, duplicate, rename, reparent, add/remove
  component, material assignment, and property edits through commands.
- Snapshot only the affected subtree/component data rather than the whole scene.
- Coalesce continuous gizmo drags and sliders into one history entry.
- Clear or deliberately preserve history on scene load; do not retain commands
  that refer to a previous scene instance.

### Unit and integration tests

- Every command restores the exact relevant before/after state.
- A new command after Undo clears the redo branch.
- Coalescing creates one transform command per drag, not per frame.
- Delete/undo restores UUIDs, hierarchy, components, and asset references.
- Sync impact selects only the expected SceneManager callback using
  spies/counters; transform commands must not trigger material or structural
  work, and structural commands must not silently downgrade.
- History limit evicts safely without corrupting retained commands.

### Runtime acceptance

- Build and then undo/redo a mixed sequence of create, transform, reparent,
  material edit, and delete operations.
- Hold a gizmo drag for many frames and confirm one Undo restores the starting
  transform.

### Exit criteria

All exposed authoring changes are command-backed or explicitly documented as
non-undoable.

## Phase 4 - Edit/Play/Pause lifecycle

### Outcome

The editor can run a temporary runtime world and return safely to authored state.

### Work

- Add explicit `Edit`, `Playing`, and `Paused` application states.
- Clone/serialize the authoring scene into a runtime scene on Play.
- Keep selection and editor camera separate from the runtime primary camera.
- Add Play, Pause, Step, and Stop controls plus shortcuts.
- Define system lifecycle: scene start, fixed update, variable update, scene stop.
- Snapshot previous transforms before simulation, update hierarchy after systems,
  then issue one batched transform sync before render.
- Disable or clearly scope authoring operations during Play.
- Restore the unchanged authoring scene on Stop.

[`game-loop.md`](game-loop.md#planned-runtime-frame-order-contract-phase-4) is
the canonical runtime frame-order contract. Phase 4 must implement its named
stages, including the deferred structural-change safe point and one batched
sync before render. Changes to runtime ordering are proposed there first; this
plan deliberately does not duplicate the sequence. Lifecycle-order tests
exercise the same named stages recorded in that contract.

### Unit and integration tests

- Runtime clone has the same UUIDs and data but independent storage.
- Runtime create/delete/transform changes do not affect the authoring scene.
- Stop restores authoring data byte-for-byte at the serialization level.
- Pause runs no simulation updates; Step runs exactly one fixed update.
- Lifecycle callbacks fire once and in the documented order.
- A long frame is clamped and fixed substeps obey a configurable maximum.

### Runtime acceptance

- Press Play, move/spawn/delete runtime entities through a temporary test system,
  Pause, Step, and Stop; verify the edit scene is unchanged.
- Verify the primary runtime camera controls the viewport during Play and the
  editor camera returns on Stop.

### Exit criteria

Repeated Play/Stop cycles neither leak entities/resources nor alter saved scene
state.

## Phase 5 - Input action system

### Outcome

Gameplay code consumes stable named actions and axes, independent of GLFW and
editor shortcuts.

### Work

- Add action and axis definitions with keyboard, mouse, and controller bindings.
- Track pressed, held, released, axis value, and mouse delta per frame.
- Add editor and runtime input contexts and explicit cursor capture modes.
- Serialize mappings in project settings, not individual scenes.
- Expose a read-only input service to scripts.
- Add controller dead zones and focus-loss reset behavior.

### Unit and integration tests

- Synthetic input events produce correct edge/held states.
- Multiple bindings combine predictably and axes clamp as documented.
- Focus loss releases held inputs and prevents stuck movement.
- Context switching prevents runtime input from triggering editor commands.
- Mapping serialization round-trips and unknown device inputs are ignored safely.

### Runtime acceptance

- Rebind a movement action, enter Play, and verify the new binding immediately.
- Switch window focus while holding a key and confirm no stuck action on return.
- Verify editor shortcuts do not move the player while editing script fields.

### Exit criteria

No gameplay-facing code reads GLFW key or mouse state directly.

## Phase 6 - Lua scripting

### Outcome

Entities can have persistent, hot-reloadable behavior authored outside C++.

### Work

- Embed Lua and add a serializable script component referencing a script asset.
- Use one engine Lua state with isolated per-entity environments unless profiling
  proves a different model necessary.
- Implement `OnCreate`, `OnFixedUpdate`, `OnUpdate`, and `OnDestroy`.
- Expose validated entity handles, transform/light/camera/visibility access,
  find, spawn, destroy, logging, timers, input, and later physics queries.
- Do not expose the EnTT registry, raw component pointers, renderer, or window.
- Reflect declared public fields with supported types into the inspector and
  serialize their values in the scene.
- Add file watching and safe hot reload. Preserve compatible public fields and
  report syntax/runtime errors with path, entity, callback, and stack trace.
- Define public-field compatibility precisely: same field name and supported
  type preserves its value; added fields receive declared defaults; removed
  fields are dropped with a warning; renamed fields require an explicit alias;
  incompatible type changes reset to the declared default with a diagnostic.
- Queue structural ECS changes made during iteration and apply them at a defined
  safe point.

### Unit and integration tests

- Lifecycle order and callback counts match Play/Pause/Stop rules.
- Two entities using one script have isolated environment state.
- Public fields serialize, deserialize, retain types, and preserve compatible
  values across reload.
- Reload tests cover added, removed, explicitly renamed, and incompatible-type
  public fields, including the required warning/diagnostic for each lossy case.
- Destroyed entity handles fail safely rather than aliasing a reused EnTT ID.
- Spawn/destroy during update is deferred and deterministic.
- Script syntax/runtime errors disable or quarantine only the affected instance.
- Timers respect pause and scene destruction.
- A scripted transform results in one batched transform sync per rendered frame.

### Headless/runtime acceptance

- Run a deterministic script that moves an entity for a fixed number of frames;
  emit a JSON state report and assert the final transform.
- Edit the script while Playing and confirm hot reload without restarting RT2.
- Introduce a syntax error and verify the scene continues running with a useful
  error in the console.

### Exit criteria

A saved script component can drive an entity using input and survive save/load,
Play/Stop, and hot reload.

## Phase 7 - Project model and asset database

### Outcome

Scenes, scripts, meshes, textures, environments, and generated data belong to a
portable project with stable references.

### Work

- Define `project.rt2proj` with project UUID, asset root, cache root, startup
  scene, input map, and default runtime settings.
- Add stable asset IDs, source paths, asset types, import settings, and dependency
  records.
- Add a content browser with search, rename/move/delete, drag/drop, and reimport.
- Watch source files and reimport asynchronously where safe.
- Keep generated cache artifacts outside source asset folders.
- Add placeholders and diagnostics for missing, invalid, or cyclic references.
- Make project moves portable by storing paths relative to the project root.

### Unit and integration tests

- Asset IDs survive rename/move and scene references continue resolving.
- Scanning produces the same database independent of directory enumeration order.
- Dependency graphs detect missing assets and cycles.
- Reimport updates cache metadata without changing asset identity.
- Project relocation preserves all valid relative references.
- Asset deletion reports dependants before committing the change.

### Runtime acceptance

- Move and rename a referenced mesh and script through the content browser; reload
  the scene and confirm both still resolve.
- Modify a source texture and verify reimport updates the viewport safely.

### Exit criteria

A project folder can be copied to another machine/location without rewriting
scene files manually.

## Phase 8 - Prefabs

### Outcome

Reusable entity hierarchies can be instantiated and customized without losing
their relationship to the source.

### Work

- Define a prefab asset as a serialized entity subtree with stable local IDs.
- Add instantiate, unpack, apply overrides, and revert overrides.
- Track instance-to-prefab local-ID mapping and property-level overrides.
- Remap internal entity references per instance; preserve external references only
  when explicitly supported.
- Defer nested prefabs until the single-level model is stable and tested.

### Unit and integration tests

- Multiple instances receive unique scene UUIDs with correct internal references.
- Source updates propagate to non-overridden properties.
- Overrides survive save/load and source updates.
- Apply/revert produces deterministic serialized output.
- Deleted or added prefab children merge predictably.

### Runtime acceptance

- Create a light fixture hierarchy, instantiate it several times, override one
  material, update the source, and verify expected propagation.

### Exit criteria

Common hierarchy reuse no longer requires copy/paste, and overrides are visible
and recoverable.

## Phase 9 - Physics and collision queries

### Outcome

Scenes support reliable rigid-body gameplay and scriptable spatial queries.

### Work

- Integrate Jolt behind an RT2-owned `PhysicsSystem` abstraction.
- Add rigid-body and collider components with static, dynamic, and kinematic modes.
- Support box, sphere, capsule, convex, trigger, and static triangle-mesh shapes in
  that order.
- Add collision layers/masks, materials, gravity, fixed timestep, and interpolation.
- Synchronize authored transforms into physics at scene start and physics results
  back to ECS after each fixed step.
- Add raycast, overlap, and shape-cast APIs and collision enter/stay/exit events.
- Add collider/contact/velocity debug drawing independent of the RT pipeline.
- Use CPU collision data; do not make simulation correctness depend on asynchronous
  GPU BLAS queries.
- Treat rigid-body transform changes as `Transform` syncs: update the matching
  RT instance transform/TLAS path once per rendered frame so path tracing sees
  dynamic rigid bodies. Static triangle-mesh collider geometry stays static;
  deforming geometry is deferred to the explicit animation/BLAS policy in Phase
  10. Do not pause or silently cheapen RT rendering during simulation.
- Measure and document the dynamic-instance/TLAS update cost in the physics
  acceptance scene before enabling large dynamic-body counts by default.

### Unit and integration tests

- Component-to-Jolt shape/body conversion preserves scale and mode constraints.
- Collision mask filtering, raycasts, overlaps, and triggers return expected IDs.
- Fixed-step integration is repeatable within documented floating-point tolerance.
- Parent/child and non-uniform-scale restrictions are validated clearly.
- Destroying an entity removes its body and pending events safely.
- Collision events fire with correct enter/stay/exit transitions.
- Dynamic rigid-body motion triggers transform-only renderer sync and the
  corresponding RT instance update, not a full BLAS rebuild.

### Headless/runtime acceptance

- Run a deterministic drop-and-collision scene for a fixed number of steps and
  validate final position, sleep state, and event counts.
- Interactively test stacked boxes, a kinematic platform, a trigger, and collider
  debug visualization.

### Exit criteria

Scripts can build a basic character/object interaction without touching the
physics library directly. Lua (Phase 6) is a hard prerequisite for this
script-facing acceptance criterion, not for the underlying physics integration.

## Phase 10 - Animation

### Outcome

Imported assets can play and blend transform and skeletal animations in runtime.

### Work

- Import glTF animation clips, skin data, and inverse bind matrices.
- Add animation clip assets and animator components.
- Begin with clip playback and cross-fade; add a parameterized state machine next.
- Evaluate animation before world-transform propagation.
- Implement skinning with a path usable by both raster and ray-tracing geometry;
  define the RT acceleration-structure update cost explicitly.
- Add inspector controls, playback preview, speed, looping, and script API.
- Defer root motion, animation events, IK, and blend trees until clip/state-machine
  playback is sound.

### Unit and integration tests

- Keyframe interpolation handles boundary, loop, step, linear, and supported cubic
  cases.
- Skeleton hierarchy and inverse-bind evaluation match known fixtures.
- Cross-fade weights remain normalized and reach exact endpoints.
- Animator state serialization and parameter transitions round-trip.
- Malformed clips fail without corrupting the scene.

### Runtime acceptance

- Play, pause, scrub, loop, and blend a known glTF character.
- Verify raster and RT representations remain spatially consistent during motion.
- Use render regression captures for a fixed animation frame sequence.

### Exit criteria

A scripted entity can select and blend imported clips reproducibly. Lua (Phase
6) is a hard prerequisite for the script-API portion of this phase, not for
implementing animation playback itself.

## Phase 11 - Audio

### Outcome

Games can play 2D and spatial sounds through a small mixer model.

### Work

- Integrate miniaudio behind RT2 audio interfaces.
- Add audio clip assets, source components, and a listener bound to the active
  runtime camera.
- Support play/stop/pause, loop, volume, pitch, spatial attenuation, and autoplay.
- Add master, music, effects, and UI buses.
- Expose safe script controls and update spatial state after transforms.

### Unit and integration tests

- Attenuation, pan, bus gain, and state transitions use deterministic math tests.
- Source/listener component serialization round-trips.
- Destroying a playing entity releases the voice safely.
- Mock backend tests verify play/stop/pause calls without requiring an audio device.

### Runtime acceptance

- Walk around a looping spatial source and verify attenuation/orientation.
- Pause/Stop Play mode and confirm runtime voices stop and do not accumulate across
  repeated sessions.

### Exit criteria

Audio can be authored, saved, controlled by scripts, and cleaned up reliably.
Lua (Phase 6) is a hard prerequisite for the script-control acceptance portion,
not for implementing the audio backend itself.

## Phase 12 - Standalone runtime and packaging

### Outcome

A project can be built and run without editor UI or source asset assumptions.

### Work

- Separate reusable engine/runtime startup from editor startup.
- Add a runtime executable/configuration that loads a packaged project and startup
  scene without editor panels.
- Build a dependency collector from the startup scene and referenced scenes,
  prefabs, scripts, and assets.
- Copy or archive runtime-ready assets, shaders, project settings, and manifests.
- Package the exact SPIR-V/shader set required by the selected runtime renderer;
  fail packaging if the package manifest and shader inventory disagree.
- Add Build, Build and Run, Debug, and Release options.
- Add window title, resolution, fullscreen, vsync, and logging settings.
- Detect missing package dependencies before launch.
- Validate runtime device assumptions before launch: required Vulkan version,
  ray-tracing/descriptor extensions and features, required formats, and the
  documented fallback or clear failure when unsupported. Debug validation layers
  are optional diagnostics and must not be a Release package dependency.
- Make package output reproducible where possible.

### Unit and integration tests

- Dependency collection is complete, deduplicated, cycle-safe, and deterministic.
- Package manifest hashes and paths are stable for identical inputs.
- Missing startup scenes/assets fail packaging with clear dependency chains.
- Runtime configuration parsing handles defaults and invalid values.
- Editor-only assets/code are absent from Release packages.
- Package validation detects missing or stale SPIR-V/shader files before launch.
- A device-capability preflight reports unsupported required Vulkan features and
  validates the documented fallback/failure path.
- Debug and Release package manifests differ only where documented; Release does
  not require validation layers or source-tree shader paths.

### Headless/runtime acceptance

- Package the vertical-slice project, launch its runtime executable headlessly for
  a fixed frame count, and verify a successful report/exit code.
- Launch interactively on a clean directory or test machine without source-tree
  working-directory assumptions.
- Launch on a clean machine/configuration with the packaged shaders and verify
  capability diagnostics or the supported renderer fallback before scene load.

### Exit criteria

The vertical slice runs from a self-contained package without launching the RT2
editor.

## Later backlog

These should follow the core authoring/runtime loop unless a concrete game needs
one earlier:

- particles and decals;
- runtime UI/canvas and menus;
- navigation mesh and AI helpers;
- additive scenes and scene streaming;
- mesh LOD, visibility culling, and mesh streaming;
- terrain tooling;
- networking and visual scripting.

Renderer-specific upscaling and display performance work (for example FSR/DLSS)
belongs in the rendering roadmap and must use renderer benchmarks, not this
engine-feature backlog. Before Phase 5 input mappings become a stable public
gameplay API, write a networking feasibility note covering authority, replicated
state ownership, input-command serialization, prediction/reconciliation, and
the determinism policy above. Networking remains deferred until a concrete game
requires it, but its dependency direction must not be designed retroactively.

# First tiny vertical slice development plan

## Goal

Deliver the smallest end-to-end proof that RT2 can author and run behavior
without committing to the full asset, physics, prefab, or packaging designs.

The slice is:

> Create a cube and camera, save the scene, press Play, run a built-in test
> behavior that moves the cube, Pause/Step, Stop, and confirm the authored cube
> returns to its saved transform; reopen the scene and confirm persistence.

This intentionally uses a native C++ `MotionComponent`/test runtime system, not
Lua. The purpose is to validate persistence, stable IDs, runtime scene cloning,
lifecycle, transform propagation, and GPU synchronization before embedding a
scripting language. The component is a temporary vertical-slice vehicle or a
generic reusable movement component; it must not become the long-term scripting
API accidentally.

### Migration contract with Phase 1

The slice is a constrained Phase 1 implementation, not a throwaway serializer.
It must use the production UUID type, schema-version header, atomic save path,
two-pass UUID reference resolution, and migration entry point. Its primitive
mesh identifiers are explicitly a v1 subset: Phase 1 migrates them to
project-relative asset references or asset IDs without changing entity UUIDs,
hierarchy references, or authored material identity. Slice-only shortcuts may
not become the general serializer contract, and Phase 1 must add fixtures that
load every slice scene before expanding the schema.

## In scope

- UUID component and lookup, backed by the core UUID value type.
- Minimal version-1 `.rt2scene` JSON containing:
  - scene version;
  - entity UUID, name, transform, hierarchy, visibility;
  - primitive mesh identity/reference and material assignment;
  - light and camera components needed by the fixture;
  - `MotionComponent` velocity for the test behavior.
- New/Open/Save/Save As and dirty marker. Recent scenes are deferred; recovery
  autosave is required by the subsequent full Phase 1 persistence gate.
- Edit, Play, Paused states plus Play/Pause/Step/Stop controls.
- Deep cloning of authoring scene to runtime scene.
- Fixed timestep accumulator with maximum substep count.
- `MotionSystem::FixedUpdate` moving runtime transforms.
- One batched transform propagation/sync per rendered frame.
- A deterministic headless slice runner or CLI flags.
- Focused tests and a checked-in tiny fixture scene.

## Explicitly out of scope

- Lua, user-configurable input, physics, prefabs, asset database, gizmos, undo,
  audio, animation, and standalone packaging.
- General serialization of arbitrary components through reflection.
- Embedded imported geometry in `.rt2scene`; use built-in primitive identifiers in
  this slice.
- Applying runtime changes back to the authoring scene.

## Proposed types and boundaries

```cpp
struct EntityIdComponent { UUID id; };

struct MotionComponent
{
    glm::vec3 linearVelocity{0.0f};
};

enum class SceneRunState { Edit, Playing, Paused };

class SceneSerializer
{
public:
    static bool Save(const ECSScene&, const std::filesystem::path&, Error&);
    static bool Load(ECSScene&, const std::filesystem::path&, Error&);
};

class RuntimeSceneController
{
public:
    bool Play(const ECSScene& authoringScene);
    void Pause();
    void Step();
    void Stop();
    void Update(float frameDt);
};
```

`RuntimeSceneController` owns the runtime clone. The editor retains ownership of
the authoring scene. Renderer scene selection must be explicit when entering and
leaving Play so callbacks do not retain references to the wrong scene.

## Work packages

### VS-1 - UUID foundation

- Implement UUID parse/format/generation and `EntityIdComponent`.
- Assign IDs in every current SceneManager entity-creation path and during import.
- Add lookup and uniqueness validation.

Tests:

- parse/format round trip;
- generation uniqueness over a practical sample;
- all SceneManager creation paths receive IDs;
- lookup and duplicate rejection.

Done when every authored entity has a stable ID and existing ECS tests pass.

### VS-2 - Minimal scene serializer

- Define and document schema version 1.
- Serialize the in-scope components and hierarchy references by UUID.
- Load in two passes: create entities/components, then resolve references.
- Save atomically and report path plus reason on failure.
- Add the file-menu actions and dirty title marker.

Tests:

- empty and slice fixture round trips;
- hierarchy resolves independent of serialized order;
- malformed UUID, duplicate UUID, missing parent, and unsupported version errors;
- failed save does not destroy an existing file;
- loaded scene generates equivalent GPU scene instance/material counts.

Runtime check:

- author fixture, save, restart, reopen, and visually compare.

Done when the fixture survives restart and its serialized form is deterministic
enough for readable source control diffs.

### VS-3 - Runtime scene clone and state machine

- Implement a deep CPU clone, preferably through an in-memory serializer path so
  clone and persistence exercise the same component coverage.
- Add Play/Pause/Stop and make renderer scene switching explicit.
- On Stop, destroy runtime systems/resources and resync the untouched edit scene.

Tests:

- clone equivalence and storage independence;
- legal and illegal state transitions;
- repeated Play/Stop cycles;
- runtime entity changes cannot affect authoring data;
- Stop restores serialized authoring state exactly.

Runtime check:

- enter and stop Play repeatedly while editing transforms between sessions.

Done when runtime state can be discarded without modifying or leaking the edit
scene.

### VS-4 - Fixed update and motion behavior

- Add the fixed accumulator, maximum substep guard, and `MotionSystem`.
- Update runtime local transforms, mark them dirty, propagate hierarchy once after
  simulation, and issue the appropriate transform-only GPU sync.
- Implement Pause and single fixed Step.

Tests:

- known velocity and step count produce the expected transform;
- frame-time partitioning produces equivalent results within tolerance;
- Pause performs zero updates and Step performs exactly one;
- large frame time respects clamp/substep cap;
- sync callback spy observes no structural sync during motion.

Headless check:

- load the fixture, run 60 fixed steps, emit final cube transform, compare it with
  the expected value, Stop, and verify the authoring transform.

Done when the cube moves deterministically in Play and snaps back on Stop.

### VS-5 - End-to-end hardening

- Add the fixture and vertical-slice invocation to regression scripts.
- Verify useful error behavior for unreadable scenes and invalid fixture data.
- Update `scene-management.md`, `game-loop.md`, and `future-extensions.md` so their
  implemented/planned status and lifecycle order agree.
- Run full Release build, all unit tests, headless slice, and existing render
  regressions affected by scene switching.

Done when a clean checkout can execute the documented slice without manual data
repair or hidden working-directory assumptions.

## Vertical slice acceptance script

1. Launch RT2 and create a new scene.
2. Add a cube, camera, and light; set the cube velocity to `(1, 0, 0)`.
3. Save as `vertical-slice.rt2scene`; close and reopen it.
4. Record the authored cube transform.
5. Press Play and observe the cube move.
6. Pause and verify it stops; press Step and verify exactly one small movement.
7. Press Stop and verify the original authored transform returns.
8. Save/reopen again and verify no runtime transform was persisted.
9. Run the headless form for 60 fixed steps and require a zero exit code plus the
   expected final runtime and restored authoring transforms.

## Vertical slice merge gates

- `RT2App` Release x64 builds.
- `RT2Tests` Release x64 builds and all tests pass.
- Headless vertical-slice test passes deterministically in repeated runs.
- Existing glTF save/load and GPU scene-data tests remain green.
- Manual acceptance script passes.
- New public formats and lifecycle behavior are documented.

## What follows immediately

After the slice, implement the full Phase 1 persistence requirements, including
the migration of every slice fixture to the general asset-reference schema and
autosave/recovery, then Phase 2 scene-building tools and Phase 3 commands/undo.
General Lua scripting should begin only after the Play lifecycle and mutation
boundaries proven by this slice are stable.

### Phase 1A â€” asset-backed native scene round-trip (implemented)

Phase 1A extends the vertical slice so an imported model plus an HDR/EXR
environment map survive save, restart, and reopen. It delivers:

- Schema version 2 with durable asset references (`ImportedMeshSourceComponent`,
  `MaterialOverrideComponent`) and v1 read + in-memory migration.
- `SceneAssetResolver`: a CPU-side, Vulkan-free service that rebuilds meshes,
  textures, materials, and environment pixels from durable references after a
  structural load, with `AssetDiagnostic` reporting for missing/malformed/
  unresolved assets.
- Import provenance attached at load/import time (glTF scene/node/mesh/
  primitive keys; OBJ whole-model + importer profile).
- Scene-relative, normalized, portable UTF-8 asset paths; absolute machine-
  specific paths are not persisted.
- `SetEditable(false)` enforced across all authoring mutation paths during
  Play.
- Transactional load (parse/schema/resolution failure cannot partially
  replace the open authoring document) and atomic save semantics retained.
- Checked-in tiny fixtures (textured GLB, tiny EXR) and focused RT2Tests
  covering UUID coverage, v1â†’v2 migration, malformed references, deterministic
  v2 saves, imported-asset round trip, environment round trip,
  transactionality, and the runtime boundary.

What remains in full Phase 1 (intentionally deferred from 1A):

- Bounded autosave and recovery records after explicit saving is stable.
- Recent-scenes UI and project-root settings.
- Later asset-database migration (Phase 7 global asset UUIDs) â€” explicitly
  out of scope for 1A; the slice uses scene-relative paths, not asset UUIDs.

### Phase 1B â€” crash-safe authoring and session recovery (implemented)

Phase 1B closes the remaining Phase 1 requirements: bounded autosave,
recovery, Save/Discard/Cancel prompts, recent-scenes, and project-root
editor settings. It delivers:

- `EditorSettingsStore`: versioned JSON, atomic write, MRU recents (bounded
  10, deduplicated case-insensitively on Windows), optional project root.
  Production path: `%LOCALAPPDATA%\RT2\Editor\settings.json`. Tests inject
  a temporary directory.
- `SceneRecoveryService`: one atomically-replaced `.rt2recovery` JSON envelope
  per
  logical document, global cap of 8 records (oldest evicted only AFTER a
  new record commits). `MaybeSnapshot` is guarded by dirty + interval +
  revision; the first dirty observation starts the interval rather than
  writing immediately. No work happens on clean frames or unchanged revisions.
  Snapshots use `SceneSerializer::SaveTo` so asset references stay
  relativized against the original authoring scene root, NOT the recovery
  directory. The complete document identity is hashed into the record filename,
  and discard is constrained to direct child records. A recovered scene with
  imported meshes/materials/env resolves correctly. Transactional restore,
  discard, `DiscardForDoc`, `OnSaveAs`.
  Recovery manifest has its own version (independent of `.rt2scene` v2).
- `UnsavedChangesCoordinator`: pure state machine (no ImGui) for
  New/Open/Recent/Exit with Save/Discard/Cancel. One pending action; a second
  request while pending is rejected. `SaveGate`/`ExecuteGate`/
  `DiscardRecoveryGate` callbacks support test injection. The host
  exposes one unified save gate and decides Save versus Save As from whether
  the document already has a native source path.
- `SceneManager::AuthoringRevision()`: a transient counter bumped on every
  authoring mutation via `NotifyAuthoringChanged()`. Used by the recovery
  service to skip identical snapshots. Not serialized into `.rt2scene`.
- `Walnut::Application::RequestClose()`: interactive, cancelable close
  request. `glfwSetWindowCloseCallback` cancels the OS close and routes
  through the coordinator. Headless/internal completion still uses
  `Close()` directly.
- Unicode Windows file/folder dialogs with long-path-sized buffers and the
  active scene/project root as the initial location. `FileDialog::OpenFolder()`
  supports project-root selection.
- RT2SliceRunner `--recovery-scenario` mode + `run_recovery_test.ps1`: a
  deterministic CPU-only, asset-backed recovery regression (generate textured
  GLB + EXR â†’ load â†’ save â†’ edit transform/material â†’
  autosave â†’ drop session â†’ discover â†’ restore â†’ verify edit + UUIDs +
  imported assets + decoded environment + explicit file unchanged â†’ discard).
  Emits structured JSON; exits non-zero on
  failure.
- RT2Tests Phase 1B coverage: 44 tests (14 EditorSettings + 30 Recovery/
  Coordinator) covering defaults, round trip, malformed/unsupported
  settings, Unicode paths, atomic replacement failure, MRU order/dedup,
  bounded recents, autosave scheduling, dirty/sourcePath/
  explicit-file preservation, retention eviction, discovery, restore
  (UUIDs, imported GLB/texture/EXR, authored material override, dirty,
  untitled), document adoption, full-path identity, contained discard,
  corrupt envelope, failed-restore-preserves-live-doc, asset-root, Save As
  retirement, and all coordinator transitions.
- Verified baseline delta: the implementation report's 275 passing tests rose
  to 281 (+6 targeted regression cases added during review). Release x64,
  RT2Tests, the original slice runner, and the asset-backed recovery scenario
  all pass. No renderer benchmark baseline changes are expected because this
  slice affects editor/session code and the recovery runner is CPU-only.

**Phase 1 exit criteria are now satisfied.** A non-trivial authored scene
survives restart, recovery is available without overwriting explicit
saves, missing assets produce actionable diagnostics, and existing
interchange saving still passes its tests.

### Phase 2A â€” stable viewport selection (implemented)

The first Phase 2 slice establishes selection and picking without coupling
editor identity to transient ECS handles or path-tracing samples:

- `EditorSelection` stores an ordered set of authoring UUIDs. The final UUID
  is the primary selection used by the Inspector. Selection is editor-only,
  is never serialized, and prunes UUIDs that no longer resolve.
- Outliner clicks and viewport clicks share that selection state. Ctrl-click
  toggles Outliner membership; hierarchy deletion clears/prunes affected IDs.
- `ScreenToRenderPixel` defines the single top-left-origin conversion from
  ImGui screen coordinates through displayed viewport size to render pixels.
- `Camera::GetPickingRay` is a deterministic pinhole ray. It deliberately
  ignores the path tracer's stochastic aperture sampling.
- A one-invocation Vulkan compute pass uses `GL_EXT_ray_query` against the
  existing TLAS. It evaluates MASK alpha cutoffs and deterministic BLEND alpha
  coverage before confirming an intersection.
- `RenderInstanceMap` is submission metadata outside `GPUSceneData`. It is
  produced by the same ECS iteration as the GPU instance array, so instance
  index `i` always maps to the UUID at entry `i`.
- Picking has one result buffer per frame in flight. A slot captures its own
  UUID map when recorded and is read only after that slot's normal render
  fence signals. No queue-idle/device-idle wait is added. Monotonic request
  serials suppress superseded clicks; scene/transform/resize changes cancel
  outstanding requests.
- Picking is active only in Edit state. A background miss clears selection.

Verification:

- Release x64 full solution builds, including `picking.comp`.
- RT2Tests: 286/286 test cases and 141,627/141,627 assertions pass.
- Live Release-layout test: cube click selected Cube in Outliner/Inspector,
  sphere click switched both to Sphere, and a background click cleared both.

### Phase 2B â€” transform editing and gizmos (implemented)

The second Phase 2 slice adds a viewport-first transform workflow while
preserving the existing renderer synchronization boundary:

- `EditableTRS` provides strict affine decomposition and composition. Singular
  matrices and results containing shear are rejected instead of silently
  changing the authored transform.
- `SceneManager` supports non-uniform local transforms, validated world-space
  edits through parent transforms, and atomic world edits for a selection. A
  failed batch leaves every selected entity unchanged.
- The Inspector exposes numeric local/world position, rotation, and non-uniform
  scale; primary/median/individual pivot choice; and configurable translation,
  angle, and scale snapping.
- The viewport provides move, rotate, and scale handles with W/E/R shortcuts,
  local/world axes, all pivot modes, snapping, and ordered UUID multi-selection.
  Continuous manipulation uses the transform-only GPU update path.
- A four-pixel drag threshold separates manipulation from selection. A plain
  click on an axis selects the object underneath it, preventing handles from
  masking small or nearby objects. Ctrl-click toggles viewport selection.
- Shared-pivot edits apply `D = currentPivot * inverse(startPivot)` and
  `world[i]' = D * world[i]`. Individual mode builds the corresponding delta
  around each entity's own pivot and, in local mode, its own orientation.

Verification:

- Release x64 full solution builds.
- RT2Tests: 297/297 test cases pass; focused Phase 2B coverage passes 11/11
  cases and 103/103 assertions.
- The vertical-slice and recovery regression scripts pass.
- Live Release-layout test: viewport click selected `Bishop_W2`; a click
  through its horizontal gizmo handle selected neighboring `Knight_W2`; a
  deliberate drag of the same handle changed the selected transform.

### Phase 2C - hierarchy authoring and outliner workflow (implemented)

The third Phase 2 slice completes the hierarchy-building workflow without
introducing the Phase 3 command stack:

- `Hierarchy::parent` is the authoritative relationship. `children` is an
  eagerly maintained traversal cache, reconstructed and validated at load and
  document-adoption boundaries. Scene-graph recursion no longer scans the
  registry once per node.
- UUID-keyed create, reparent, batch-delete, visibility, duplicate, and paste
  operations use validate/calculate/commit semantics. They bump the authoring
  revision once and return one `SyncImpact`; a failed operation leaves the
  scene unchanged. Reparent defaults to preserve-world and rejects cycles,
  singular transforms, and shear.
- Batch deletion canonicalizes selected roots, collects complete post-order
  subtrees before destroying anything, detaches only roots from surviving
  parents, compacts resources once, and repairs transient mesh, material,
  material-override, and texture indices.
- Effective visibility is inherited through parent links. Full scene builds
  and transform-only instance updates share one visible-renderable enumerator,
  preserving the GPU-instance-to-UUID mapping and excluding hidden emissive
  geometry from light extraction.
- Subtree duplication uses a canonical authored-component registry, generates
  fresh UUIDs in two passes, remaps internal hierarchy links, and shares the
  same-document mesh resources. Clipboard copy holds an immutable in-memory
  scene snapshot, is document-scoped, and invalidates when resource generations
  change.
- Editor locks are direct-only editor state: directly locked entities reject
  inspector, gizmo, delete, visibility, and reparent edits; an unlocked ancestor
  may still carry or delete locked descendants. Copy/duplicate of locked sources
  is allowed and new copies are unlocked.
- The Outliner now supports create-empty/create-child, case-insensitive search,
  visibility and lock state, copy/paste/duplicate/delete shortcuts (suppressed
  while text input owns the keyboard), context actions, and preserve-world
  drag/drop reparenting. Structural operations use the full renderer sync path.

Verification:

- Release x64 full solution builds.
- RT2Tests: 307/307 test cases pass, including 10 focused Phase 2C cases for
  hierarchy rebuilding/cycles, inherited visibility and instance-map ordering,
  atomic reparent/delete, duplication, locks, and immutable clipboard behavior.
- The vertical-slice and recovery regression scripts pass.

### Phase 2D - camera authoring workflow (implemented)

The final Phase 2 slice completes viewport navigation and authored-camera
alignment without introducing the Phase 3 command stack:

- `MeshRegistry::AddMesh` caches finite object-space AABBs once. Selection
  bounds transform the eight AABB corners, include selected roots and every
  descendant without visibility filtering, and give non-renderable entities a
  deterministic 0.5 m fallback cube.
- Frame Selected fits both horizontal and vertical projection axes, uses the
  larger required distance plus a margin, updates focus distance, and has a
  deterministic fallback when the camera starts inside the bounds. Focus
  Selected rotates in place toward the bounds centre.
- The editor provides nine document-session camera bookmarks containing pose,
  FOV, aperture, focus distance, and far clip. Bookmarks and movement speed are
  editor state: they are not serialized, do not dirty the scene, and do not
  trigger scene synchronization. New/Open/recovery adoption clears bookmarks
  through `EditorSceneState::ResetForDocument()`.
- `CameraComponent` entity pose is authoritative in `Transform`; its stored
  `forwardDirection` is a compatibility mirror refreshed after generic
  transform and hierarchy mutations. Document adoption reconciles legacy
  stored directions, after which Transform remains authoritative.
- View Through Camera copies one authored camera pose into the editor view.
  Align Camera to View converts the editor pose through the entity's parent and
  commits exactly one Transform mutation/revision. Singular or sheared
  parent-relative results are rejected atomically with a diagnostic.
- Play snapshots the current editor view and restores that exact pose on Stop.
  A runtime `CameraComponent` is chosen by lowest UUID; legacy
  `ECSScene::camera` remains the fallback when no camera entity exists.
- Every programmatic editor-camera cut routes through
  `ApplyEditorCameraCut()` and the existing
  `ISceneRenderBridge::ResetTemporalState()` contract exactly once. The real
  bridge resets accumulation/NRD state and invalidates both ReSTIR DI and GI
  histories; editor navigation issues no Full/Material/Transform sync.
- Shortcuts are `F` frame, `Shift+F` focus, `Ctrl+1..9` recall, and
  `Ctrl+Shift+1..9` store. They are suppressed during text entry, active widget
  editing, and Play.

Verification:

- Release x64 full solution builds.
- RT2Tests: 318 total cases; focused Phase 2D coverage passes 11/11 cases and
  81/81 assertions for cached bounds, hierarchy/hidden/empty bounds, per-axis
  framing, focus, bookmarks, one-reset/no-sync camera cuts, deterministic
  runtime camera choice, atomic alignment/rejection, and native save/reload.
- The vertical-slice and recovery regression scripts pass.
- Interactive acceptance uses a Release-layout temporary deployment with the
  Release `imgui.ini` copied beside the executable. All five behavioural checks
  pass against `vertical-slice.rt2scene`: (1) `F` frames the selection and
  `Shift+F` rotates toward it without translating; (2) `Ctrl+Shift+1` store then
  `Ctrl+1` recall restores the full editor pose and optical values; (3) *View
  Through Camera* copies the camera-entity pose to the editor without dirtying,
  and *Align Camera to View* writes the editor pose back onto the camera entity
  and dirties the document (Transform sync, revision advances); (4) Play
  snapshots the editor pose and Stop restores it exactly; (5) frame/focus/recall
  never dirty the document while align does, and no camera cut leaves visible
  accumulation/NRD/ReSTIR smearing.

Known pre-existing test failures (unrelated to Phase 2D, tracked for a separate
fix): the full unfiltered `RT2Tests` run exits non-zero with six failures that
also reproduce on the Phase 2C checkpoint â€” five `SceneGraph` cases in
`EcsTests.cpp` (raw-registry hierarchy setups that omit the parent `children`
list `SceneGraph::UpdateNode` traverses) and one SIGSEGV in `SceneManager:
RemoveEntity destroys entity` via the UUID-backed `RemoveSubtrees` path. Phase
2D's sources are byte-identical to HEAD for all six, so none is attributable to
this slice.

**Phase 2 exit criteria are now satisfied.** The next vertical slice is Phase
3A: establish the command/history foundation and migrate a narrow set of
existing editor mutations to undo/redo without changing their sync-impact
contract.

### Phase 3A â€” command/history foundation (implemented)

Phase 3A is a narrow vertical slice of the Phase 3 command system: a CPU-only
command abstraction, a bounded history with redo, and exactly two
representative mutations migrated end-to-end. It deliberately excludes gizmo
migration, structural (create/delete/duplicate/paste/reparent) commands,
material/light/camera/name property commands, a history UI panel, and
save-point clean tracking.

Core design decisions:

- Commands are UUID-keyed, minimal-state, and CPU-only. A command stores
  `rt2::core::UUID` targets (never `entt::entity`) plus only the before/after
  data it touches. No whole-scene snapshots. `Execute`/`Undo` resolve UUIDs at
  run time and return `EditorMutationResult`; a missing entity is a graceful
  `Failure`, never a crash.
- Commands are constructed with complete before/after state. Construction is
  separated from application, so `Execute` and `Redo` are the same pure
  "apply after-states" call and can never recapture a stale "before".
- `result.syncImpact` from the executed mutation is the single sync-impact
  authority. There is no separate declared-impact channel; a command's
  expected impact is asserted in its tests.
- History never touches the render bridge. `EditorCommandHistory` runs
  commands against `SceneManager` and returns the mutation result; the caller
  routes it through the existing UI/host sync path. Dependency direction is
  `WalnutApp -> SceneEditorUI -> EditorCommandHistory -> IEditorCommand ->
  SceneManager`; SceneManager never depends on the command layer, and
  RT2SliceRunner does not link it.
- Two history entry points, both clearing redo only on a successful,
  effective submission: `Execute(cmd)` applies then records; `RecordApplied(
  cmd)` records a command whose effect was already applied incrementally
  (continuous edits). Failed initial Execute leaves both stacks unchanged.
- Conservative failure policy: a failed `Undo` or `Redo` surfaces the error
  and clears both stacks â€” a failed inverse means history's causal
  assumptions were violated by an out-of-band change, and older entries
  cannot be trusted.
- Generation guard on every public operation. `Execute`, `RecordApplied`,
  `Undo`, and `Redo` all compare the stored `DocumentGeneration` against the
  current one; on mismatch both stacks are cleared and history rebinds to the
  new generation (Execute/RecordApplied then proceed as the first entry).
  Host-side clears at document-adoption sites remain as redundancy only.
  History survives Play/Stop (document generation is unchanged) but undo/redo
  entry points are blocked while Play is active. History survives Save;
  undoing past the save point leaves the document dirty until save-point
  tracking is implemented (deliberate simplification).
- Undo/Redo bypass editor locks by construction: locks are editor-session UI
  state enforced at `MutationSelectionAllowed` when initiating commands; the
  command layer sits below that gate and SceneManager has no lock concept. A
  lock guards against new accidental edits, not against restoring states the
  user already authored.
- No-op suppression compares normalized state before recording: normalized
  local TRS (epsilon component compare, sign-canonicalized quaternion) for
  transforms; for visibility, pairs already in the target state are dropped
  at construction and an emptied command is never submitted. The no-op rule
  keeps history clean; it does not claim to un-dirty a document that
  per-frame direct edits already dirtied.

The two representative migrations:

- `TransformCommand` wraps Inspector transform editing. It stores
  `{UUID, beforeLocalTRS, afterLocalTRS}` â€” always local space, captured via
  `GetLocalTransform` regardless of whether the user edited in Local or World
  mode, so `TrySetWorldTransform`-based edits are fully covered. Undo/Redo
  restore via `SetLocalTransform`. Sync impact: Transform.
- `SetVisibilityCommand` wraps visibility toggling, built on a new atomic
  UUID-keyed API `SceneManager::SetVisibilityStates(
  const std::vector<std::pair<rt2::core::UUID, bool>>&)`, which follows the
  existing `SetVisibility` shape: validate every UUID first (any failure
  means no mutation), deduplicate (last write wins), skip entities already in
  the target state, apply all, bump the revision once, and return one result
  (Structural if anything changed, empty-success None otherwise). Undo is a
  single `SetVisibilityStates` call with inverted pairs. A "Hide/Show
  Selection" context action operating on `Selection().Ordered()` makes the
  multi-entity mixed-state path reachable from the UI.

Continuous-edit lifecycle (the coalescing seam): there is no runtime merge
API. One drag equals one history entry via record-on-release, and the seam
future slices reuse is the pattern `RecordApplied` + explicit session
boundaries (the gizmo's `DragState` already has begin/end). The Inspector
holds one `TransformEditSession { target UUID, beforeLocal, open }`:

- `ImGui::IsItemActivated()` is checked immediately after each of the three
  `DragFloat3` widgets (per-widget, honoring ImGui's last-item rule). The
  first activation while no session is open captures `beforeLocal` before any
  mutation that frame and opens the session, owned by that widget.
- `IsItemDeactivatedAfterEdit()` on the owning widget closes the session:
  current local TRS becomes `afterLocal`; normalized-equal means discard,
  otherwise `RecordApplied(TransformCommand)`.
- `IsItemDeactivated()` without AfterEdit (Escape cancel â€” ImGui reverts the
  value itself) discards the session and records nothing.
- Failed intermediate World edits cannot corrupt the session: a rejected
  `TrySetWorldTransform` leaves local TRS untouched, and the before/after
  comparison at close is the sole authority. A drag whose every frame failed
  collapses to a no-op.
- Defensive guards at close: target no longer the inspected entity, entity
  dead, or `m_Editable` false (Play started mid-drag) â€” discard.

Sync routing extraction: a CPU-only `EditorSyncRouter` takes the
`EditorMutationResult` plus injected callables for transform/material/full
sync and reset-accumulation, reproducing the current host mapping including
the resource-generation Structural downgrade check. The WalnutApp
`m_OnMutation` lambda body delegates to it, making the
`Transform -> transform-only`, `Material -> material-only`,
`Structural -> full`, `None -> nothing` contract testable.

Ownership and integration:

- WalnutApp owns `EditorCommandHistory` and injects a non-owning pointer via
  `SceneEditorUI::SetCommandHistory()`.
- SceneEditorUI exposes public `Undo()`/`Redo()`; each runs history and
  routes the result through the existing private `ApplyMutation()` â€” one
  sync path and one `m_MutationError` display.
- WalnutApp Edit-menu items and `Ctrl+Z` / `Ctrl+Shift+Z` (and `Ctrl+Y`
  alias) call those public methods, suppressed during text entry, active
  widget editing, and Play (same rules as the Phase 2D shortcuts). Menu
  labels come from `UndoDescription()`/`RedoDescription()`.
- History is cleared at every document-adoption site in addition to the lazy
  generation guard.

New files: `RT2App/src/EditorCommand.h`, `RT2App/src/EditorCommandHistory.h/
.cpp`, `RT2App/src/EditorCommands.h/.cpp`, `RT2App/src/EditorSyncRouter.h/
.cpp`, `RT2Tests/src/Phase3ACommandTests.cpp`. Modified: `SceneManager.h/
.cpp` (`SetVisibilityStates`), `SceneEditorUI.h/.cpp`, `WalnutApp.cpp`,
`RT2App.vcxproj`, `RT2Tests.vcxproj`, `RT2App/premake5.lua`,
`RT2Tests/premake5.lua`. RT2SliceRunner is deliberately untouched.

Test plan (all CPU, doctest):

- Execute/Undo restores the precise before-TRS / visibility flags; Redo
  restores the after-state. An execute/undo/redo/undo cycle asserts the
  second undo still restores the original before-states.
- A new effective command after Undo empties the redo stack; failed or no-op
  submissions do not.
- Bounded history (default 64, configurable) evicts oldest; every retained
  entry still undoes correctly.
- `SetVisibilityStates`: atomic validate-first failure (no partial
  mutation), single revision bump, mixed-state round trip, empty/no-change
  None result.
- Sync-impact spies: with counting `SyncCallback`s installed, Execute/Undo
  invoke zero sync callbacks; returned impact matches the expected impact
  per command; `TransformCommand` never reports Material/Structural.
- `EditorSyncRouter`: each impact triggers exactly its own sync path, `None`
  triggers nothing, and the resource-generation downgrade check is honored.
- Generation guard on all four public operations; explicit `Clear` empties
  both stacks.
- Failed Undo (target UUID no longer resolves) surfaces an error and clears
  both stacks; the scene is not further mutated.
- No-op suppression: identical before/after records no entry.
- `RecordApplied` records without re-mutating and still clears redo.
- Lock bypass: an entity locked after a transform edit is still restored by
  Undo.

Runtime acceptance:

- Numeric-edit a transform, `Ctrl+Z` restores exactly, `Ctrl+Shift+Z`
  reapplies.
- Hold one slider drag across many frames â€” exactly one history entry; one
  Undo returns to the pre-drag pose.
- Toggle visibility on a multi-selection with mixed prior states â€” one Undo
  restores the mix.
- Undo, make a new edit â€” Redo is unavailable.
- Open a different scene â€” history is empty. During Play, undo/redo are
  inert.

Verification gates: Release x64 build; focused Phase 3A tests; full RT2Tests
where the only permitted failures are exactly the six known pre-existing
cases listed above (five `SceneGraph` cases and `SceneManager: RemoveEntity
destroys entity`) â€” any new or different failure blocks the slice; slice and
recovery regression scripts; `graphify update .`; documentation updates with
new test counts.

Verification:

- Release x64 full solution builds clean.
- RT2Tests: 334 total cases; focused Phase 3A coverage passes 16/16 cases and
  183/183 assertions covering TransformCommand execute/undo/redo/undo cycles,
  effective-vs-no-op redo clearing, bounded-history eviction, atomic
  validate-first `SetVisibilityStates` failure, mixed-state round trips and
  None/empty results, dedupe last-write-wins, history-invokes-zero-sync-callbacks
  with authoritative impact, `EditorSyncRouter` per-impact routing plus the
  resource-generation Structural downgrade check, generation-guard on all four
  public operations, failed-Undo stack clearing, no-op suppression,
  `RecordApplied` without re-mutation, lock bypass, and `SetVisibilityCommand`
  multi-entity round trips.
- Full `RT2Tests` run: the failing set is exactly the six known pre-existing
  cases (five `SceneGraph` cases in `EcsTests.cpp` and one SIGSEGV in
  `SceneManager: RemoveEntity destroys entity`). No new or different failure.
- `run_slice_test.ps1` and `run_recovery_test.ps1` both pass.
- `graphify update .` completed (24095 nodes, 49842 edges, 896 communities).

Runtime acceptance (interactive): pending â€” to be performed by the user
against a Release deployment. The five behavioural checks: numeric-edit +
`Ctrl+Z`/`Ctrl+Shift+Z`; one drag = one history entry; mixed-state
multi-selection visibility toggle with one Undo restoring the mix; new edit
after Undo clears Redo; open-a-different-scene clears history and Play
inertness.

### Phase 3B1 â€” structural command correctness (implemented)

Phase 3B1 migrates structural editor mutations (create, delete, duplicate,
paste, reparent, primitive/light entity creation, and the viewport gizmo's
transform drag) onto the Phase 3A command/history foundation with exact-UUID,
exact-hierarchy, exact-resource-reference restoration on Undo/Redo. It is the
first of two Phase 3B sub-slices; 3B2 will follow with property commands. The
history panel and save-point clean tracking are deferred to a later slice.

Core design decisions:

- Resource-lifetime policy â€” no compaction while history references resources.
  `SceneManager::RemoveSubtrees` currently calls `CompactMeshRegistry()`
  immediately after deletion. Compaction rewrites every surviving entity's
  `MeshRef::meshIndex` and can delete unique meshes/materials/textures. A
  subtree snapshot that only stored the deleted entity's pre-deletion
  `MeshRef::meshIndex` would restore an index that now points to a different
  resource or nothing. Phase 3B1 enforces a hard invariant: compaction cannot
  run while any Undo or Redo entry references resource slots. A new
  `RemoveSubtreesNoCompact` is the deletion path used by structural commands;
  the existing `RemoveSubtrees` (with compaction) stays for non-command paths.
  `CompactMeshRegistry()` may run only at explicit `history.Clear()`, document
  adoption, or save/reload â€” never merely because the redo branch was cleared.
  A new explicit `CompactMeshRegistryNow()` is the only public compaction entry
  point and asserts (debug) / no-ops (release) when history is non-empty. All
  existing direct `CompactMeshRegistry()` call sites are audited. The eventual
  better design is a stable material/mesh ID decoupled from slot index; that is
  out of scope for 3B1 and is noted as future work.
- Known-UUID transactional create/restore â€” commands never touch the registry.
  Phase 3A's contract is that command before/after state is complete before
  `Execute()` and Redo never recaptures state. Creation commands resolve this
  with the `RecordApplied` seam. Creation pattern: host reserves known UUIDs,
  manager applies the mutation with those UUIDs, host captures the resulting
  snapshot, host constructs the command, host calls `RecordApplied`. If
  snapshot capture fails, the initial creation is rolled back. Redo calls
  `RestoreSubtrees(snapshot)` (re-creates with stored UUIDs). Undo calls
  `RemoveSubtreesExact(snapshot)`. Deletion pattern: host captures the
  snapshot at construction time (entity exists), command is executed via
  `history.Execute`; `Execute`/`Redo` call `RemoveSubtreesNoCompact(roots)`,
  `Undo` calls `RestoreSubtrees(snapshot)`. All creation paths route through a
  single host helper so no creation path can apply successfully and forget to
  record.
- `SubtreeSnapshot` captures only the affected subtree â€” never the whole scene.
  It reuses the serializer's per-entity component payload representation
  (`EntityRecord` shape) so it stays aligned with the persisted format. The
  snapshot carries per-entity UUID, name, parent UUID, local TRS, visibility,
  and optional full-value payloads for every persisted component (MeshRef,
  PrimitiveComponent, ImportedMeshSourceComponent, MaterialOverrideComponent
  with full SceneMaterial + sourceMaterialKey, LightComponent, CameraComponent,
  MotionComponent). Root sibling anchors record `prevSibling`/`nextSibling`
  UUIDs plus a child index for diagnostic cross-check; the anchors are the
  authority.
- Sibling-position restoration fails atomically, never silently appends.
  `RestoreSubtrees` validates each root's anchors against the parent's current
  children list (or the root-entity list). If the anchors are inconsistent,
  restoration fails atomically and Phase 3A's history-failure policy clears
  both stacks. Root-entity ordering is currently unspecified (registry-
  iteration order, no explicit authored ordering vector); 3B1 documents it as
  unspecified and does not introduce an explicit root-order model. A future
  slice may introduce one if stable root ordering becomes a requirement.
- Creation Undo must not absorb later descendants. Creation `Undo` uses the
  creation-time snapshot only and never recaptures. `SceneManager::RemoveSubtrees
  Exact(snapshot)` performs validation and removal as one transactional
  operation: validate every expected UUID exists, validate authored component
  state and hierarchy topology against the snapshot, reject unexpected
  descendants, perform all validation before destroying anything, remove without
  resource compaction, return failure with no scene mutation if validation
  fails. "Exact" compares authoritative authored state only â€” not derived
  world matrices, GPU caches, selection state, or other transient editor/
  runtime data. In a valid linear history, later descendant-creation commands
  are undone by their own commands first, so this validation normally passes.
  The validation catches out-of-band edits and refuses to silently absorb
  them.
- Atomic multi-transform for gizmos. A new UUID-keyed
  `SetLocalTransformStates(vector<pair<UUID, EditableTRS>>)` validates ALL
  UUIDs resolve, applies all local TRS in one pass, marks dirty once, refreshes
  affected camera subtrees once, bumps the revision ONCE, and returns
  `Transform` impact with affected UUIDs. One missing target => no mutation,
  `Failure`. `TransformCommand` is extended to store
  `std::vector<TransformTriple>` (where `TransformTriple = {UUID,
  beforeLocalTRS, afterLocalTRS}`). The single-entity factory stays for the
  Inspector; a new multi-entity factory handles the gizmo path and drops no-op
  entities. The host captures `beforeLocalTRS[]` at drag start (via `DragState`
  begin) and `afterLocalTRS[]` at drag end, builds the multi-entity
  `TransformCommand`, and calls `RecordApplied`. The gizmo's existing per-frame
  `TrySetWorldTransforms` calls continue (responsive direct edits); only the
  drag-end recording is new.
- Atomic batch reparent. `SceneManager::Reparent` accepts one destination
  parent for all sources. Undo of a multi-source reparent where the sources
  originally had different parents requires restoring each source to its own
  original parent. A new `ReparentBatch(vector<ReparentEdit>, ReparentMode)`
  validates all entities and all new parents resolve and no cycles, then
  applies all reparents atomically. For `PreserveWorld`, converts each desired
  world matrix to local against the NEW parent (singular/shear => fail all).
  Bumps the revision once. `ReparentCommand` stores before/after `ReparentEdit`
  lists; `Execute` calls `ReparentBatch(afterStates, mode)`; `Undo` calls
  `ReparentBatch(beforeStates, PreserveLocal)` â€” always restore local, since
  the command stored the exact before-local TRS. `ReparentEdit` carries
  `RootSiblingAnchor` so Undo restores the exact original sibling position.
- `DuplicateSubtreesWithUuids` / `PasteSubtreesWithUuids` use a flat UUID-list
  design with an internal pre-order walk. The manager canonicalizes the roots
  (preserving caller order), walks each canonical subtree in deterministic
  pre-order, validates the UUID count exactly matches the resulting entity
  count, validates all supplied UUIDs are valid/unique/absent from the
  document, builds and validates the complete duplication plan before
  mutating, assigns UUIDs positionally in that internal pre-order, and returns
  the created root UUIDs plus the complete source-to-duplicate UUID mapping.
  The host queries the exact canonical entity count via
  `CountCanonicalSubtreeEntities(roots)` (returning `Result<size_t>`),
  reserves exactly that many UUIDs, and calls the duplication API. The
  manager's internal count validation is mandatory protection against stale
  input. The command retains the resulting duplicate `SubtreeSnapshot`, so
  Redo restores the same entities with the same UUIDs rather than repeating
  the source walk or generating new IDs. `PasteSubtreesWithUuids` returns the
  same structured `DuplicationResult`; for paste, the source UUIDs in the
  mapping are clipboard-document UUIDs, not entities currently present in the
  destination scene (documented as such). The paste command retains only the
  pasted snapshot and created-root UUIDs for Undo/Redo; it does not need the
  source mapping after the initial operation.
- Single-entity visibility migration. Every Hide/Show entry point constructs
  the same `SetVisibilityCommand`, whether operating on one UUID or a multi-
  selection. The existing single-entity `ApplyMutation(m_SceneMgr->
  SetVisibility({uuid}, ...))` calls migrate to construct
  `SetVisibilityCommand` and route through `history.Execute`. The multi-entity
  "Hide/Show Selection" context action (Phase 3A) already uses this command.
- Impact assignments are authoritative from the manager, never synthesized.
  Structural commands call SceneManager atomic ops that already return
  `EditorMutationResult.syncImpact` (Structural for renderable-affecting
  create/remove/restore/duplicate/paste; Structural or Transform for reparent
  per the existing logic; Transform for `SetLocalTransformStates`). The
  commands return the manager's result unchanged. No synthesis. The expected
  impact is asserted in each command's tests.
- History semantics are unchanged from Phase 3A. History survives Play/Stop
  and Save; blocked during Play; cleared on document adoption. The generation
  guard on all four public ops is unchanged. The failure policy (failed
  Undo/Redo clears both stacks) is unchanged and now also covers the
  structural-consistency validation failures from `RemoveSubtreesExact` and
  `RestoreSubtrees` anchor checks. `RecordApplied` is the entry point for
  creation commands; `Execute` is the entry point for deletion commands. Both
  clear redo on a successful, effective submission.

New SceneManager APIs:

```
// Resource lifetime
EditorMutationResult RemoveSubtreesNoCompact(const std::vector<UUID>& roots);
EditorMutationResult RemoveSubtreesExact(const SubtreeSnapshot& snapshot);
void CompactMeshRegistryNow();  // explicit, asserts history clear in debug

// Known-UUID creation
UUID ReserveKnownUuid();
std::vector<UUID> ReserveKnownUuids(size_t count);
Result<size_t> CountCanonicalSubtreeEntities(const std::vector<UUID>& roots) const;
EditorMutationResult CreateEmptyWithUuid(UUID, name, parent, siblingPosition?);
EditorMutationResult CreatePrimitiveEntity(UUID, name, type, localTRS, matIdx, parent?);
EditorMutationResult CreateLightEntity(UUID, name, localTRS, color, intensity, isSpot, parent?);

struct DuplicationResult {
    EditorMutationResult mutation;
    std::vector<UUID> createdRoots;
    std::vector<std::pair<UUID, UUID>> sourceToDuplicate;
};
DuplicationResult DuplicateSubtreesWithUuids(sourceRoots, knownDuplicateUuids);
DuplicationResult PasteSubtreesWithUuids(clipboard, clipboardRoots, parent?, knownPastedUuids);

// Subtree snapshot + restore
SubtreeSnapshot CaptureSubtreeSnapshot(const std::vector<UUID>& roots);
EditorMutationResult RestoreSubtrees(const SubtreeSnapshot& snapshot);

// Atomic batch transform
EditorMutationResult SetLocalTransformStates(const std::vector<std::pair<UUID, EditableTRS>>& states);

// Atomic batch reparent
EditorMutationResult ReparentBatch(const std::vector<ReparentEdit>& edits, ReparentMode mode);
```

The existing `CreateEmpty`, `RemoveSubtrees`, `DuplicateSubtrees`,
`PasteSubtreesFrom`, `Reparent`, `SetVisibility`, `SetVisibilityStates`,
`TrySetWorldTransforms` all stay unchanged for non-command paths (RT2SliceRunner,
host-driven non-undoable flows).

New files: `RT2App/src/SubtreeSnapshot.h` (snapshot structs),
`RT2App/src/EditorStructuralCommands.h/.cpp` (the seven structural command
classes plus factories), `RT2Tests/src/Phase3B1CommandTests.cpp`. Modified:
`SceneManager.h/.cpp` (new APIs; existing ops unchanged), `EditorCommands.h/.cpp`
(multi-entity TransformCommand), `SceneEditorUI.h/.cpp` (command migration of
Add Primitive / Add Light / Delete / Duplicate / Paste / Reparent / single-entity
Hide/Show), `WalnutApp.cpp` (gizmo drag-end RecordApplied + creation helper),
`RT2App.vcxproj`, `RT2Tests.vcxproj`, `RT2Tests/premake5.lua`, both doc files.

Test plan (all CPU, doctest; 3A coverage stays green):

- `CreateEmptyCommand`: `RecordApplied` creates with a known UUID at a known
  sibling position; Undo removes via `RemoveSubtreesExact`; Redo re-creates
  via `RestoreSubtrees` with the SAME UUID at the SAME position.
- `RemoveSubtreesCommand`: multi-level subtree with a mesh entity (carrying
  MeshRef + ImportedMeshSourceComponent + MaterialOverrideComponent) and a
  light entity; captures the full snapshot; Undo restores UUIDs, hierarchy
  children order, all 10 persisted components, and resource references
  (MeshRef::meshIndex unchanged because no compaction); Redo removes again.
- Resource stability regression (critical): delete the sole user of a textured
  mesh while unrelated entities survive; Undo; verify the deleted entity's
  MeshRef::meshIndex still points to the original mesh (no compaction
  occurred); verify surviving entities' MeshRef::meshIndex values are
  unchanged across the full Undo/Redo cycle; verify the mesh still exists in
  the registry at the original index.
- `RemoveSubtreesExact` validation: after `RemoveSubtreesCommand::Execute`,
  add a child to the removed root out-of-band, then attempt Undo â€” the
  exact-state validation fails atomically (zero mutation), the command
  surfaces a history-consistency error, and Phase 3A's failure policy clears
  both stacks.
- `RemoveSubtreesExact` transient-state tolerance: verify that
  Transform::worldMatrix, Transform::prevWorldMatrix, Transform::dirty, and
  selection/clipboard state are NOT compared by the exact-state validation
  (only authored component state + hierarchy topology).
- `DuplicateSubtreesCommand`: captures original + duplicate UUIDs; the
  manager returns the source-to-duplicate mapping; Undo destroys the
  duplicates; Redo re-creates the duplicates with the SAME UUIDs (not fresh);
  verify hierarchy among duplicates is preserved and resource references are
  intact.
- `DuplicateSubtreesWithUuids` count validation: pass a `knownDuplicateUuids`
  list whose count does not match the canonical subtree size; verify the
  manager fails atomically with no mutation.
- `CountCanonicalSubtreeEntities`: returns the exact canonical entity count
  for a multi-root selection including nested descendants; missing/invalid
  roots return a failure result.
- `PasteSubtreesCommand`: captures the clipboard snapshot + pasted UUIDs;
  Undo destroys the pastes; Redo re-pastes with the same UUIDs; verify
  resource-reference invariants hold (no compaction while the command is in
  history).
- `ReparentCommand`: multi-source with different original parents;
  PreserveWorld and PreserveLocal modes; Undo restores the exact before-local
  TRS for each source at the exact sibling anchor; Redo re-applies; atomic
  failure on a cycle (one failure => no mutation).
- Sibling-anchor restoration: delete a middle child of a 3-child parent; Undo;
  verify the middle child is restored at the exact middle position (anchors
  validate); if the parent's children have been reordered out-of-band, Undo
  fails atomically rather than appending.
- `SetLocalTransformStates`: atomic validate-first (one missing UUID => no
  mutation, Failure); single revision bump; multi-entity round trip; atomic
  failure leaves all entities unchanged.
- Gizmo drag â†’ TransformCommand: 2-entity selection dragged along one axis;
  ONE history entry; Undo restores both pre-drag local TRS; Redo re-applies
  both. Verify the gizmo's per-frame TrySetWorldTransforms calls continue
  (responsive direct edits) and only the drag-end recording is new.
- `CreatePrimitiveEntityCommand` / `CreateLightEntityCommand`: `RecordApplied`
  creates with a known UUID; Undo removes via `RemoveSubtreesExact`; Redo
  re-creates with the SAME UUID; components and resource references intact.
- Single-entity visibility migration: the per-entity Hide/Show context-menu
  items construct `SetVisibilityCommand` and route through `history.Execute`;
  one Undo restores the prior visibility; verify the command instance is the
  same type as the multi-entity Hide/Show Selection path.
- Generation guard still fires on all four public ops (3A coverage stays green).
- Compaction audit: verify `CompactMeshRegistryNow()` no-ops (or asserts in
  debug) when history is non-empty; verify no `RemoveSubtrees`-with-compaction
  call remains on any command-backed path; verify compaction runs at
  `history.Clear()` and document adoption.
- Full-suite gate: the failing set stays exactly the six known pre-existing
  cases.

Verification gates: Release x64 build; focused Phase 3B1 tests; full RT2Tests
where the only permitted failures are exactly the six known pre-existing
cases; `run_slice_test.ps1` and `run_recovery_test.ps1` pass;
`graphify update .`; documentation updates with actual test counts.

Runtime acceptance (interactive, pending user):

- Create an empty entity, add a child to it, then `Ctrl+Z` the child creation
  followed by `Ctrl+Z` the empty creation â€” both undo in order; `Ctrl+Shift+Z`
  redoes the empty creation (with the SAME UUID), then `Ctrl+Shift+Z` redoes
  the child.
- Delete a textured mesh entity (sole user of its mesh) while unrelated
  entities survive; `Ctrl+Z` restores it with its texture intact; the
  unrelated entities never flicker (no compaction happened).
- Duplicate a multi-entity hierarchy; `Ctrl+Z` removes the duplicates;
  `Ctrl+Z` again (if no other edit) does nothing further; `Ctrl+Shift+Z`
  re-creates the duplicates with the same names + " Copy" suffix.
- Reparent two entities with different original parents into a common parent
  (PreserveWorld); `Ctrl+Z` restores each to its original parent at the
  original sibling position with the original local TRS.
- Hold a gizmo drag on a 2-entity selection across many frames â€” one `Ctrl+Z`
  restores both pre-drag transforms.
- Toggle visibility on a single entity via the context menu â€” one `Ctrl+Z`
  restores the prior state.

Verification report (CPU-only, doctest):

- Release x64 build succeeds for RT2App, RT2Tests, and RT2SliceRunner.
- `Phase3B1CommandTests.cpp`: 23 test cases, 308 assertions, all pass.
  Covers: CreateEmpty/Primitive/Light RecordApplied/Undo/Redo with same UUID;
  RemoveSubtreesCommand multi-level subtree with resource-reference
  stability; resource stability regression (no compaction while command in
  history); RemoveSubtreesExact out-of-band-descendant rejection;
  RemoveSubtreesExact transient-state tolerance (worldMatrix/prevWorldMatrix/
  dirty not compared); DuplicateSubtreesCommand same-UUID Redo;
  DuplicateSubtreesWithUuids count validation; CountCanonicalSubtreeEntities
  canonical + invalid; PasteSubtreesCommand same-UUID Redo; ReparentCommand
  multi-source PreserveLocal Undo/Redo; ReparentCommand sibling-position
  restoration via anchor; ReparentBatch PreserveWorld preserves world pose;
  ReparentBatch atomic cycle failure; ReparentBatch batch-cycle validation
  ({Aâ†’B, Bâ†’A}); SetLocalTransformStates atomic validate-first; multi-entity
  TransformCommand (gizmo path); single-entity visibility via history;
  sibling-anchor middle-position restoration (parented); multi-root
  middle-delete Undo restores entity (root ordering unspecified);
  generation guard clears structural commands; CompactMeshRegistryNow
  explicit entry point.
- `Phase3ACommandTests.cpp`: 16 test cases, 183 assertions, all pass
  (3A coverage stays green).
- Full `RT2Tests` run: 309 test cases, 303 passed, 6 failed, 48 skipped.
  The failing set is exactly the six known pre-existing cases (five
  `SceneGraph` cases in `EcsTests.cpp` and one SIGSEGV in
  `SceneManager: RemoveEntity destroys entity`). No new or different
  failure.
- `run_slice_test.ps1` passes (60 steps, authoring intact).
- `run_recovery_test.ps1` passes.
- `graphify update .` completed (24307 nodes, 50435 edges, 924 communities).
- Compaction invariant enforced: `OnSceneChanged` gates compaction on
  `!(m_History.CanUndo() || m_History.CanRedo())`; `CompactMeshRegistryNowAsserted()`
  debug-asserts history is empty at all four document-adoption call sites.
- Root-anchor validation skipped for nil-parent roots (root ordering
  unspecified); strict anchoring kept for parented roots.
- Outliner drag-reparent uses PreserveWorld on Execute (preserves world
  pose); Undo uses PreserveLocal with stored before-local TRS.
- ReparentBatch uses ReparentEdit.anchor for insertion position; batch-cycle
  validation against the planned parent map catches {Aâ†’B, Bâ†’A}.

Runtime acceptance (interactive): pending â€” to be performed by the user
against a Release deployment. The six behavioural checks listed above.

Explicitly out of scope for 3B1: property commands (name, material, light,
camera, motion) â€” Phase 3B2; record-on-release for continuous property widgets
â€” Phase 3B2; `AlignCameraCommand` â€” Phase 3B2; history panel â€” deferred
(requires `TravelTo(stateId)` with batched sync); save-point clean tracking â€”
deferred (requires unique history-state identity integrated with authoritative
dirty state; stack-size identity is invalid); coalescing/merge API â€” deferred
(`RecordApplied` + session boundaries handle the common cases); stable
material/mesh IDs decoupled from slot index â€” future work (the no-compaction
invariant makes slot-index reference safe for 3B1); explicit root-entity
ordering model â€” root ordering stays unspecified.

### Phase 3B2 â€” property command completion (implemented)

Phase 3B2 migrates every Inspector property edit onto the Phase 3A/3B1
command/history foundation with record-on-release, exact-value Undo/Redo
(including durable `MaterialOverrideComponent` side effects), and the same
impact/compaction invariants as 3B1. It covers: name, material index,
material properties, light, camera, motion (add/remove + velocity), and
`AlignCameraCommand`. After this slice, the only non-command authoring
mutations remaining are glTF import, Load Mesh File, and env-map load â€” the
Phase 3 exit criterion surface.

Core design decisions:

- `PropertyEditSession<T>` is a pure state machine, with a thin ImGui glue
  layer. The 3A `TransformEditSession` is UI-embedded and untestable â€” exactly
  where the keyboard-commit bug (0dcc490) lived, invisible to 183 assertions.
  3B2 splits the abstraction:
  - `PropertyEditSession<T>` (in `PropertyEditSession.h`, no ImGui dep): a
    pure state machine with `OnActivated(beforeValue)`, `OnEditCommitted()`,
    `OnCancelled()`, `CloseDeferred(afterValue) â†’ optional<SessionRecord<T>>`.
    Owns the defensive guards (target-changed, entity death, `m_Editable`
    false mid-edit) as predicate callbacks injected by the host.
  - A per-widget ImGui glue layer in `SceneEditorUI.cpp` calls
    `IsItemActivated`/`IsItemDeactivatedAfterEdit` and forwards to the state
    machine. The deferred-close-after-mutation ordering lives in the state
    machine, not the glue.
  - Tests drive the state machine directly, including deferred-close ordering,
    Escape-cancel, and keyboard-commit. This is the single change that makes
    the previously-unreachable layer verifiable.
- Single active-session slot, not concurrent sessions. ImGui has exactly one
  active item; concurrent sessions can't arise from real interaction. The
  host owns one `PropertyEditSession<T>` slot per property kind (asserting no
  double-open). Cross-kind independence is sequential, not concurrent â€” a new
  activation while a different kind's session is open is a programming error
  and asserts in debug.
- Material commands capture/restore `MaterialOverrideComponent` atomically,
  server-side in the state APIs. Both `SetMaterial` and
  `SetMaterialProperties` mutate durable `MaterialOverrideComponent` state on
  imported entities as a side effect (`SceneManager.cpp:2717-2720` and
  `SceneManager.cpp:3000-3013`). The command state must capture and restore
  override state (or its absence) atomically:
  - `SetMaterialIndexCommand` stores `{UUID entity, int beforeIndex,
    int afterIndex, std::optional<MaterialOverrideComponent> beforeOverride,
    std::optional<MaterialOverrideComponent> afterOverride}`. Execute/Redo
    apply the after-state (index + override); Undo restores the before-state
    (index + override, or removes the override if before was absent).
  - `SetMaterialPropertiesCommand` stores `{int slotIndex, SceneMaterial
    beforeMaterial, SceneMaterial afterMaterial, std::vector<std::pair<UUID,
    MaterialOverrideComponent>> beforeOverrides,
    std::vector<std::pair<UUID, MaterialOverrideComponent>> afterOverrides}`
    â€” the per-entity before-overrides of all imported entities referencing
    the slot at execute time. Undo restores all before-overrides.
  - The state APIs (`SetMaterialIndexState`, `SetMaterialPropertiesState`)
    perform the override capture/restore server-side so the command stays
    thin; the command stores the captured state.
- Impact classification is minimally correct, not rescued by the router
  downgrade:
  - `SetMaterialIndexState` â†’ **Material** (today's effective sync is
    `SetSceneKeepTextures`; no geometry/AS change).
  - `SetMaterialPropertiesState` â†’ **Material**.
  - `SetLightPropertiesState` â†’ **Material** (today's path is
    `NotifySceneChanged` â†’ keep-textures).
  - `SetCameraPropertiesState` â†’ **None** (no GPU sync today).
  - `SetNameState` â†’ **None**.
  - `SetMotionState` â†’ **None** (no GPU sync; motion is runtime-only).
  - `SetCameraPoseState` (align) â†’ **Transform**.
- State-API signatures are after-value-only, consistent with 3A precedent.
  3A property commands (`SetLocalTransform`, `SetVisibilityStates`) apply
  blindly; only 3B1's structural removes validate exactly. 3B2 follows the
  property-command precedent: signatures take the after-value only,
  before-state lives in the command alone.
  - `SetEntityNameState(UUID, const std::string& name) â†’ EditorMutationResult`
  - `SetLightPropertiesState(UUID, const LightComponent& value) â†’ EditorMutationResult`
  - `SetCameraPropertiesState(UUID, const CameraComponent& value) â†’ EditorMutationResult`
  - `SetMaterialPropertiesState(int slotIndex, const SceneMaterial& value) â†’ EditorMutationResult` (also captures/restores overrides)
  - `SetMaterialIndexState(UUID, int afterIndex) â†’ EditorMutationResult` (also captures/restores override)
  - `SetMotionState(UUID, const std::optional<MotionComponent>& value) â†’ EditorMutationResult`
  The existing void-returning APIs delegate to the new ones (single
  implementation); they stay for non-command paths.
- `AlignCameraCommand` uses one atomic API. Composing
  `SetLocalTransformStates` + `SetCameraPropertiesState` would bump the
  revision twice and require a synthesized combined impact â€” violating both
  the "revision bumps once" convention and "impact is authoritative from the
  manager, never synthesized." Instead, one atomic API:
  - `SetCameraPoseState(UUID, const EditableTRS& local, const CameraComponent&
    props) â†’ EditorMutationResult` â€” validates the entity exists and has a
    CameraComponent, applies the local TRS + camera props in one pass, bumps
    the revision once, returns one authoritative `Transform` impact. Used by
    Execute/Redo/Undo alike.
  - `AlignCameraCommand` stores `{UUID, EditableTRS beforeLocal, EditableTRS
    afterLocal, CameraComponent beforeCamera, CameraComponent afterCamera}`.
    Host applies `AlignCameraEntityToView`, captures the after-state, records
    via `RecordApplied`. Redo re-applies the stored after-state (NOT re-align
    to current view). Undo restores the before-state via `SetCameraPoseState`.
  - Wiring fix: the current `AlignCameraToView` (`WalnutApp.cpp:1157`) calls
    `SyncAuthoringTransforms` directly, bypassing the router. Migration
    routes through `ApplyMutation`/router so the accumulation reset fires
    exactly once.
- Single `SetMotionCommand` covers add, remove, and velocity edits.
  `SetMotionCommand` stores `{UUID, std::optional<MotionComponent> before,
  std::optional<MotionComponent> after}`. Execute/Redo emplace or remove to
  match `after`; Undo restores `before`. Add = {nullopt, some}; Remove =
  {some, nullopt}; velocity edit = {some, some}. One command class, three use
  cases.
- Material "Duplicate" button: AddMaterial (leak) + SetMaterialIndexCommand.
  `AddMaterial` creates a new slot outside the command (orphaned until
  history-clear compaction â€” consistent with the 3B1 leak-until-clear policy,
  documented here as expected behavior, not a defect). The
  `SetMaterialIndexCommand` records the index change. Undo restores the old
  index; the orphaned slot stays until `history.Clear()` +
  `CompactMeshRegistryNow()`.
- `SetLightCommand` stores the full `LightComponent` struct (color, intensity,
  isSpot, range, innerConeAngle, outerConeAngle). One command covers every
  light property. The Inspector only exposes color/intensity/isSpot today,
  but the command is forward-compatible.
- Name edit records on Enter/commit only, not per keystroke. The name
  `InputText` uses `ImGuiInputTextFlags_EnterReturnsTrue`. The command fires
  once on Enter (or defocus with commit). Per-keystroke edits mutate the live
  buffer only. Matches the 3A record-on-release pattern.

Defensive guards in `PropertyEditSession<T>`:
- Target-changed: the inspected entity UUID no longer matches the session's
  target (selection switched mid-edit). Discard.
- Entity death: `FindEntityByUuid(target) == entt::null`. Discard.
- `m_Editable` false mid-edit: Play started mid-drag. Discard.
- Slot-still-in-range (material-properties sessions only): the session's key
  is a slot index, not a UUID; the guard is `slotIndex < materials.size()`.
  If the slot was removed out-of-band (only possible at history-clear
  compaction, which clears sessions too), discard.

New SceneManager atomic APIs:

```
EditorMutationResult SetEntityNameState(const rt2::core::UUID& entity,
                                        const std::string& name);
EditorMutationResult SetLightPropertiesState(const rt2::core::UUID& entity,
                                             const LightComponent& value);
EditorMutationResult SetCameraPropertiesState(const rt2::core::UUID& entity,
                                              const CameraComponent& value);
EditorMutationResult SetMaterialPropertiesState(int slotIndex,
                                                const SceneMaterial& value);
EditorMutationResult SetMaterialIndexState(const rt2::core::UUID& entity,
                                           int afterIndex);
EditorMutationResult SetMotionState(const rt2::core::UUID& entity,
                                    const std::optional<MotionComponent>& value);
EditorMutationResult SetCameraPoseState(const rt2::core::UUID& entity,
                                        const EditableTRS& local,
                                        const CameraComponent& props);
```

The existing void-returning `SetEntityName`, `SetLightProperties`,
`SetCameraProperties`, `SetMaterialProperties`, `SetMaterial` all delegate to
the new state APIs (single implementation). They stay for non-command paths
(RT2SliceRunner, host-driven non-undoable flows).

New files: `RT2App/src/PropertyEditSession.h` (the pure state machine
template, no ImGui dep), `RT2App/src/EditorPropertyCommands.h/.cpp` (the 7
property command classes plus factories), `RT2Tests/src/Phase3B2CommandTests.cpp`.
Modified: `SceneManager.h/.cpp` (new state APIs; existing void APIs delegate),
`SceneEditorUI.h/.cpp` (migrate all property widgets to `PropertyEditSession<T>`
+ ImGui glue; replace `TransformEditSession` with the template; migrate Name,
Material, Light, Camera, Motion editors), `WalnutApp.cpp` (`AlignCameraToView`
records via `RecordApplied`; routes through `ApplyMutation`/router),
`RT2App.vcxproj`, `RT2Tests.vcxproj`, `RT2Tests/premake5.lua`, both doc files.

Test plan (all CPU, doctest; 3A/3B1 coverage stays green):

- `SetNameCommand`: Execute/Undo/Redo restores name; no-op suppression for
  identical names.
- `SetMaterialIndexCommand`: Execute/Undo/Redo restores slot index; Material
  impact; resource stability (slot index unchanged because no compaction).
- `SetMaterialIndexCommand` override restore: assign material on an imported
  entity â†’ Undo â†’ verify the `MaterialOverrideComponent` matches (or is
  absent as) before.
- `SetMaterialPropertiesCommand`: Execute/Undo/Redo restores full material
  value; Material impact; slot-keyed (not entity-keyed); affects all
  entities referencing the slot.
- `SetMaterialPropertiesCommand` override restore: edit a shared slot â†’ Undo
  â†’ verify per-entity overrides of all imported entities referencing the slot
  are restored.
- `SetMaterialPropertiesCommand` invariant interplay: slot-keyed material
  edit survives an unrelated command-delete in history (the 3B1 compaction
  invariant keeps the slot stable).
- `SetLightCommand`: Execute/Undo/Redo restores color/intensity/isSpot (full
  LightComponent); Material impact.
- `SetCameraCommand`: Execute/Undo/Redo restores FOV/aperture/focusDistance;
  None impact.
- `SetMotionCommand`: Add/Remove round trip; velocity edit round trip; Undo
  restores before-state (present/absent + value). All three use cases via one
  command class.
- `AlignCameraCommand`: RecordApplied records the composite after-state; Redo
  re-applies stored state (not re-align to current view); Undo restores
  before-localTRS + before-cameraProps; one revision bump; Transform impact
  authoritative.
- `PropertyEditSession<T>` state machine: activation/deactivation lifecycle;
  deferred-close-after-mutation ordering; Escape-cancel discards;
  keyboard-commit (Ctrl+click+Enter) records; defensive guards
  (target-changed, entity death, `m_Editable` false mid-edit,
  slot-out-of-range) all discard.
- No-op suppression for every command (not just name): identical before/after
  records no entry.
- Record-on-release for each property widget: one history entry per drag, not
  per frame.
- Generation guard covers the new commands.
- Full-suite gate: the failing set stays exactly the six known pre-existing
  cases.

Verification gates: Release x64 build; focused Phase 3B2 tests; full RT2Tests
where the only permitted failures are exactly the six known pre-existing
cases; `run_slice_test.ps1` and `run_recovery_test.ps1` pass;
`graphify update .`; documentation updates with actual test counts.

Runtime acceptance (interactive, pending user):

- Edit a material's base color via DragFloat; release â†’ one `Ctrl+Z` restores
  the prior color.
- Change a light's intensity via DragFloat; release â†’ one `Ctrl+Z` restores.
- Change a camera's FOV; release â†’ one `Ctrl+Z` restores.
- Type a new entity name + Enter â†’ one `Ctrl+Z` restores the old name.
- Add Motion, drag velocity, Remove Motion â†’ three Undo steps restore each.
- Align Camera to View â†’ one `Ctrl+Z` restores the prior transform + FOV.

Exit criterion (post-3B2 non-command surface): after this slice, the only
non-command authoring mutations are glTF import, Load Mesh File, and env-map
load. The `NotifySceneChanged` â†’ compaction-gated path shrinks to imports
only. This mirrors Phase 3's "command-backed or explicitly documented as
non-undoable" exit criterion.

Explicitly out of scope for 3B2: history panel â€” deferred (requires
`TravelTo(stateId)` with batched sync); save-point clean tracking â€” deferred
(requires unique history-state identity integrated with authoritative dirty
state; stack-size identity is invalid); coalescing/merge API â€” deferred
(`RecordApplied` + session boundaries handle the common cases); stable
material/mesh IDs decoupled from slot index â€” future work (the no-compaction
invariant makes slot-index reference safe for 3B1/3B2).

Verification report (implementation):

- New files: `RT2App/src/PropertyEditSession.h`,
  `RT2App/src/EditorPropertyCommands.h/.cpp`,
  `RT2Tests/src/Phase3B2CommandTests.cpp`.
- Modified: `SceneManager.h/.cpp` (7 new state APIs:
  `SetEntityNameState`, `SetLightPropertiesState`,
  `SetCameraPropertiesState`, `SetMaterialPropertiesState`,
  `SetMaterialIndexState`, `SetMotionState`, `SetCameraPoseState`; new
  `GetMaterialOverride`/`InstallMaterialOverride` helpers; void APIs
  delegate to the state APIs); `core/Error.h/.cpp` (new
  `InvalidArgument` code); `SceneEditorUI.h/.cpp` (replaced
  `TransformEditSession` with `PropertyEditSession<T>` sessions for
  transform, name, light, camera, material-index, material-properties,
  motion; all property widgets migrated to record-on-release via the
  state machine + command factories); `WalnutApp.cpp` (`AlignCameraToView`
  captures composite before/after state, records via `RecordApplied`,
  routes through `ApplyMutation`/router); `RT2App.vcxproj`,
  `RT2Tests.vcxproj`, `RT2Tests/premake5.lua` (new files registered).
- Build: Release x64 clean.
- Phase 3B2 tests: 22 test cases, 160 assertions, all passing.
- Full RT2Tests: 331 test cases, 325 passed, 6 failed (the 6 known
  pre-existing failures: 5 `SceneGraph` cases in `EcsTests.cpp` + 1
  SIGSEGV in `SceneManager: RemoveEntity destroys entity`), 48 skipped.
  No new failures introduced.
- `run_slice_test.ps1`: PASS (60 steps, authoring intact).
- `run_recovery_test.ps1`: PASS.
- graphify: 24491 nodes, 50899 edges, 913 communities.

Runtime acceptance (interactive): pending â€” to be performed by the user
against a Release deployment. The six behavioural checks listed above.

## Phase 4 â€” Edit/Play/Pause lifecycle (completion, implemented)

Phase 4's vertical slice is already implemented (RuntimeSceneController,
Play/Pause/Step/Stop, deep-clone, editor camera snapshot, runtime camera
selection by lowest UUID, 17 tests). This completion slice closes the
remaining Phase 4 work items so the exit criterion â€” "Repeated Play/Stop
cycles neither leak entities/resources nor alter saved scene state" â€” is
met by construction and verified by a dedicated test surface, not just by
the existing 100-cycle smoke test.

Verification: `RT2Tests/src/Phase4LifecycleTests.cpp` â€” 23 tests, 671
assertions, all passing. Full RT2Tests: 354 cases, 348 passed, 6
pre-existing failures (5 `SceneGraph` cases in `EcsTests.cpp` + 1 SIGSEGV
in `SceneManager: RemoveEntity destroys entity`), 48 skipped.
`run_slice_test.ps1` and `run_recovery_test.ps1` pass. WalnutApp injects
the production UUID provider via one line in `EnterPlay`. The interactive
app installs no observer and never queues a deferred operation.

The slice adds four things:

### 1. Injectable runtime UUID provider

`RuntimeSceneController::Play()` today constructs a default `SceneDocument`
(`m_Runtime = std::make_unique<SceneDocument>()`) with no UUID provider
set. `SceneSerializer::CloneInMemory` deliberately preserves the
destination provider (see `SceneSerializer.cpp:1132-1137`), which is
therefore null. Runtime UUID generation is impossible as specified.

Fix: add an injectable runtime UUID provider to the controller.

```
void RuntimeSceneController::SetRuntimeUuidProvider(
    rt2::core::IUuidProvider* provider);
```

The host (WalnutApp) injects the production `OsUuidProvider` (or reuses
the authoring document's provider â€” they are stateless and the UUID
spaces are disjoint because the runtime document's index is a fresh copy
at Play). Tests inject a `DeterministicUuidProvider` seeded for
reproducibility. The controller sets the provider on the freshly
constructed runtime document BEFORE `CloneInMemory`:

```
m_Runtime = std::make_unique<SceneDocument>();
if (m_RuntimeUuidProvider)
    m_Runtime->SetUuidProvider(m_RuntimeUuidProvider);
if (!SceneSerializer::CloneInMemory(authoring, *m_Runtime, err)) { ... }
```

The provider is stored on the controller (non-owning, like
`SceneManager::m_UuidProvider`), so a single `DeterministicUuidProvider`
injected by a test seeds every Play across 100 cycles consistently.

### 2. Runtime scene mutator (no registry mutation from the controller)

The naive approach â€” a private controller helper that emplaces directly
into the runtime `ECSScene` registry â€” duplicates SceneManager invariants
inside `RuntimeSceneController` and would silently break:
`EntityIdComponent` / `uuidIndex` consistency, hierarchy parent/children
invariants, transform dirtiness and `prevWorldMatrix`, component/resource
references, subtree destruction semantics (post-order collect, parent
unlink, UUID erase).

Fix: extract a small, Vulkan-free `RuntimeSceneMutator` that operates on
a `SceneDocument` and owns exactly those invariants for the runtime-only
operations Phase 4 needs. It is NOT a second SceneManager â€” it exposes
only `CreateEntity` and `DestroySubtree` (no compaction, no command
history, no sync callbacks, no material overrides). It lives in
`rt2::core` next to `SceneDocument` so both the controller and tests can
link it without Vulkan.

```
namespace rt2::core {

struct RuntimeEntityCreateDesc
{
    std::string name;
    std::optional<UUID> parentUuid;     // nullopt = root
    std::optional<glm::vec3> translation;
    std::optional<glm::quat> rotation;
    std::optional<glm::vec3> scale;
    // Phase 4 supports only the empty + transform + name + visibility
    // component set. No mesh, no light, no camera, no primitive. Phase 6
    // scripting may extend this; Phase 4 deliberately keeps the surface
    // minimal so the mutator invariants are tractable and testable.
};

class RuntimeSceneMutator
{
public:
    // Create an entity with a caller-allocated UUID (the controller
    // allocates the UUID at queue time â€” see Â§3). Returns Failure if the
    // UUID is already present or the parent UUID does not resolve.
    // Emplaces: Transform, NameComponent, VisibleComponent, EntityIdComponent,
    // Hierarchy (if parent). Marks the transform dirty. Initializes
    // prevWorldMatrix = worldMatrix after the first SceneGraph evaluation
    // (see Â§5).
    Result<UUID> CreateEntity(SceneDocument& doc,
                               const UUID& uuid,
                               const RuntimeEntityCreateDesc& desc) const;

    // Destroy the subtree rooted at `uuid` (post-order collect, parent
    // unlink, UUID erase from doc.uuidIndex, registry.destroy). Returns
    // Failure if the UUID does not resolve. No compaction (the runtime
    // document has no compaction invariant; it is destroyed on Stop).
    Result<void> DestroySubtree(SceneDocument& doc, const UUID& uuid) const;
};

} // namespace rt2::core
```

The mutator reuses `SceneHierarchy::CollectSubtreePostOrder` and
`SceneGraph::MarkDirty` (already linked into RT2Tests). It does NOT call
`NotifyAuthoringChanged` (no authoring revision to bump on the runtime
document) and does NOT touch any `SceneManager`-private state.

### 3. Single FIFO structural-operation queue

Separate create and destroy queues lose cross-operation ordering,
contrary to the stable-order game-loop contract. Use one FIFO queue of
tagged operations drained in exact enqueue order.

```
struct CreateRuntimeEntityOperation
{
    UUID uuid;                          // allocated at queue time
    RuntimeEntityCreateDesc desc;
};

struct DestroyRuntimeSubtreeOperation
{
    UUID uuid;
};

using RuntimeStructuralOperation =
    std::variant<CreateRuntimeEntityOperation,
                 DestroyRuntimeSubtreeOperation>;
```

Public controller API:

```
// Allocate a fresh UUID from the runtime provider, enqueue a create,
// return the UUID so later operations in the same tick can reference the
// new entity. Returns Failure if the controller is not Playing/Paused,
// or if the provider is null, or if the allocated UUID is already
// present (defensive â€” the provider should not produce duplicates).
Result<UUID> QueueCreateRuntimeEntity(const RuntimeEntityCreateDesc& desc);

// Enqueue a destroy. Returns Failure if the controller is not
// Playing/Paused. Does NOT validate the UUID here â€” validation happens
// at drain time so a queued destroy of a not-yet-created entity is a
// meaningful error rather than a silent drop. Returns Ok (nullopt
// payload) on enqueue success.
Result<void> QueueDestroyRuntimeEntity(const UUID& uuid);
```

Drain semantics (`ApplyDeferredStructuralChanges`, called at the safe
point in both `Update` and `Step`):

1. **Validate the complete batch before any mutation.** Walk the queue
   in order, building the set of UUIDs that will exist after each
   operation. A create adds its UUID; a destroy removes its UUID. The
   validation state starts from the current runtime document UUID set.
   Validation failures:
   - Create with a UUID already in the validation set â†’ `DuplicateUuid`.
   - Destroy with a UUID not in the validation set (missing, or already
     destroyed by an earlier op in this batch) â†’ `InvalidEntity`.
   - Create whose `parentUuid` does not resolve in the validation set
     (parent never existed, or was destroyed by an earlier op) â†’
     `InvalidEntity`.
   - Destroy of an entity that is an ancestor of an entity a LATER
     create in the batch targets as `parentUuid` â†’ `InvalidEntity`
     (would create an orphan). This is the "destroyâ†’create-child-of-
     destroyed-parent" case.
2. On ANY validation failure: drain nothing, leave the queue intact,
   surface the error via the bridge (or a controller error callback),
   and keep running. The runtime document is unchanged. The queue is
   NOT cleared â€” the host or test can inspect it. (A future Phase 6
   script error handler may clear or edit the queue; Phase 4 just
   reports.)
3. On validation success: apply the batch atomically in enqueue order
   via the `RuntimeSceneMutator`. Each operation either succeeds or the
   mutator returns Failure (which, post-validation, should not happen â€”
   if it does, it is a bug, and the controller treats it as a fatal
   queue error: clear the queue and log).
4. After successful application: clear the queue. Compute the frame's
   sync impact as `Structural` (any create or destroy was applied) or
   `Transform` (queue was empty or validation failed with no partial
   application).

This batch-validate-then-apply design matches the 3B1 structural
command precedent (`RemoveSubtreesExact`, `ReparentBatch` both validate
all then apply) and gives deterministic failure semantics: the runtime
document is never left in a partially-mutated state.

### 4. UUID allocation at queue time

`QueueCreateRuntimeEntity` allocates the UUID when called, not when
drained. This lets later operations in the same tick refer to the new
entity (e.g. create-then-destroy in one frame, or create a parent then
create a child of it) and makes deterministic testing possible (the test
sees the UUID immediately and can assert against it).

```
Result<UUID> QueueCreateRuntimeEntity(const RuntimeEntityCreateDesc& desc)
{
    if (m_State != SceneRunState::Playing && m_State != SceneRunState::Paused)
        return Result<UUID>::Fail(Error::InvalidRuntimeState, "",
            "QueueCreateRuntimeEntity: not Playing/Paused");
    if (!m_RuntimeUuidProvider)
        return Result<UUID>::Fail(Error::InvalidRuntimeState, "",
            "QueueCreateRuntimeEntity: no runtime UUID provider");

    UUID uuid = m_RuntimeUuidProvider->CreateV4();
    while (m_Runtime->uuidIndex.Contains(uuid) ||
           PendingCreateUuids().contains(uuid))
        uuid = m_RuntimeUuidProvider->CreateV4();

    m_PendingOperations.push_back(
        CreateRuntimeEntityOperation{ uuid, desc });
    return Result<UUID>::Ok(uuid);
}
```

`PendingCreateUuids()` is a helper that walks the queue and collects the
UUIDs of pending create operations, so a second create with a
provider-duplicate UUID does not collide with a queued-but-undrained
create.

### 5. Created-transform prevWorldMatrix initialization

A freshly created entity's `Transform::prevWorldMatrix` is
default-constructed (identity). After `ApplyDeferredStructuralChanges`
runs and `SceneGraph::UpdateWorldTransforms` evaluates the new
entity's `worldMatrix`, the controller sets
`prevWorldMatrix = worldMatrix` for every entity whose UUID was added by
this batch. This prevents first-frame motion-vector spikes for the new
entity. (Mirror of `RuntimeSceneController::InitPrevTransforms`, scoped
to the batch's created set.)

### 6. Lifecycle ordering and state

Precise ordering for Play:

1. Construct runtime document, set UUID provider, `CloneInMemory`.
2. `InitPrevTransforms`.
3. Bridge `FullSync` + `ResetTemporalState`.
4. Set `m_State = Playing`.
5. Fire `OnSceneStart(runtime)`.

So `OnSceneStart` observes a fully-activated runtime document while the
state is already `Playing` (a callback that queries `GetState()` sees
the post-Play state). This makes the callback a clean observation seam.

Precise ordering for Stop:

1. Set a `m_Stopping` flag (or check `m_State` in queue entry points)
   that disables further `QueueCreateRuntimeEntity` /
   `QueueDestroyRuntimeEntity` calls (return
   `Error::InvalidRuntimeState`).
2. Fire `OnSceneStop(runtime)` while the runtime document still exists
   and is observable.
3. Clear `m_PendingOperations`.
4. `m_Runtime.reset()`.
5. Bridge `FullSync` + `ResetTemporalState` on the authoring document.
6. Set `m_State = Edit`.

This means a callback cannot queue structural operations during
`OnSceneStop` (queueing is disabled before the callback fires) and
cannot observe a half-destroyed runtime document.

### 7. Lifecycle observer naming and the Phase 6 seam

The const callback is suitable as an observer but not yet a complete
Phase 6 scripting seam because script `OnCreate` commonly needs
mutation/spawning. Rename the interface to
`IRuntimeLifecycleObserver` to reflect that it is an observation seam,
not a mutation seam. Phase 6 will add a separate
`IRuntimeCommandSink` (or similar) passed alongside the const document
to `OnSceneStart`, giving scripts a controlled mutation channel that
routes through the deferred queue. Phase 4 implements only the observer.

```
namespace rt2::core {

class IRuntimeLifecycleObserver
{
public:
    virtual ~IRuntimeLifecycleObserver() = default;
    virtual void OnSceneStart(const SceneDocument& runtime) {}
    virtual void OnSceneStop(const SceneDocument& runtime) {}
};

} // namespace rt2::core
```

The controller stores an optional non-owning pointer and dispatches in
Play/Stop. Phase 4 tests install a recording spy that counts calls and
snapshots the runtime document state.

### 8. "Authoring unchanged" â€” precise definition

The current `Stop` path mutates the authoring document's transient
`gpuCache` through a `const_cast` (`RuntimeSceneController.cpp:149`).
So "the whole `SceneDocument` is literally unchanged" is false today.
The exit criterion is therefore defined precisely as: the authoring
document's **canonical serialized state** is unchanged across Play/Stop
cycles. That is:

- `metadata.dirty` unchanged.
- `metadata.sourcePath` / `metadata.name` / `metadata.schemaVersion`
  unchanged.
- `AuthoringRevision()` unchanged (Play/Stop do not call
  `NotifyAuthoringChanged`).
- `ECSScene` (registry entities, components, mesh registry, materials,
  textures, lights, camera) unchanged.
- `uuidIndex` unchanged (same UUID set, same entity mappings).
- `environment` unchanged.

The transient `gpuCache` is explicitly excluded (it is a CPU cache; the
Stop path legitimately rebuilds it). Tests assert via
`SceneSerializer::SerializeToString` (or equivalent canonical dump)
before Play and after Stop, comparing the strings. This is the
"byte-for-byte" claim, scoped to canonical serialized state.

### 9. Exit-criterion test surface

New `RT2Tests/src/Phase4LifecycleTests.cpp` with:

- **Lifecycle callback order and counts:** Play â†’ OnSceneStart(1) â†’ ...
  â†’ Stop â†’ OnSceneStop(1). 100 cycles â†’ 100 of each, no double-fire.
- **OnSceneStart receives a non-null runtime document while state is
  Playing; OnSceneStop receives a non-null runtime document.**
- **Injectable UUID provider:** a `DeterministicUuidProvider` injected
  via `SetRuntimeUuidProvider` produces the same UUID sequence across
  two identical Play/Stop/Play cycles (deterministic test).
- **Deferred create queued during a fixed tick:** after `Update`
  returns, the entity exists in the runtime scene with the UUID
  returned by `QueueCreateRuntimeEntity`; the bridge received exactly
  one `FullSync` for that frame; the new entity's
  `prevWorldMatrix == worldMatrix`.
- **Deferred destroy queued during a fixed tick:** after `Update`
  returns, the runtime entity is gone; the authoring entity with the
  same UUID is unchanged; one `FullSync` for that frame.
- **FIFO ordering:** queue a create (UUID A), then a create (UUID B
  with parentUuid A), then a destroy (UUID A) â€” the batch validates
  successfully (A exists after op 1, B exists and is a child of A after
  op 2, A is destroyed by op 3 which post-order-collects and also
  destroys B). Assert both A and B are gone after `Update`. This is
  the cross-operation ordering guarantee.
- **Createâ†’destroy in one frame:** queue create(A), then destroy(A).
  Validation: A exists after op 1, A is destroyed by op 2. After
  `Update`, A is gone. One `FullSync`.
- **Destroyâ†’create-child-of-destroyed-parent:** queue destroy(A), then
  create(B with parentUuid A). Validation fails (A's UUID is not in the
  validation set when op 2 runs). The batch is NOT applied; the queue
  remains intact; the runtime document is unchanged; the bridge
  receives no `FullSync` (or the controller surfaces an error â€” see
  Â§3.2). A test asserts the runtime document matches its pre-frame
  state.
- **Duplicate destroy:** queue destroy(A) twice. Validation fails on
  the second op (A is no longer in the validation set). Batch not
  applied.
- **UUID collision on create:** manually construct a
  `CreateRuntimeEntityOperation` with a UUID already in the runtime
  index (via a test-only backdoor, or by injecting a provider that
  returns a duplicate). Validation fails with `DuplicateUuid`.
- **Invalid queue calls in Edit:** `QueueCreateRuntimeEntity` and
  `QueueDestroyRuntimeEntity` return `Error::InvalidRuntimeState` when
  the controller is in `Edit`.
- **Callback reentrancy:** an observer whose `OnSceneStart` calls
  `QueueCreateRuntimeEntity` is rejected (queueing is allowed during
  `OnSceneStart` â€” it fires after `m_State = Playing` â€” but the queued
  op is NOT drained during `OnSceneStart`; it waits for the next
  `Update`/`Step`). Assert the op appears in the runtime document only
  after the next `Update`.
- **Pause/Resume preserves the queue:** queue a create while Paused,
  Resume, call `Update`; the create is drained. Queue a create while
  Paused, Step; the create is drained at Step's safe point.
- **Step drains the queue** at its safe point (same path as `Update`).
- **100-cycle stress with pending changes:** each cycle queues a create
  and a destroy (some cycles leave them pending by calling Stop
  mid-frame in the test); assert no leak, no authoring mutation (via
  canonical serialization compare), no crash.
- **Authoring-unchanged invariant:** serialize the authoring document
  to a string before the first Play; after N Play/Stop cycles with any
  sequence of queue calls, serialize again and assert the strings are
  equal (excluding `gpuCache`, which is not serialized).

### Core design decisions (carried from the original spec, amended)

- **No ImGui, no WalnutApp behavior change.** The Play/Pause/Step/Stop
  UI is unchanged. The observer and queue are exercised by tests; the
  interactive app installs no observer and never queues a deferred
  change. `WalnutApp` is modified ONLY to inject the production UUID
  provider via `SetRuntimeUuidProvider` (one line in the
  RuntimeSceneController wiring).
- **Coalesced sync, one per frame.** A frame with any applied
  structural operation fires one `FullSync`; a frame with only motion
  fires `TransformSync`; a frame with a failed validation batch fires
  neither (no mutation occurred). The controller picks the impact
  itself (it talks to `ISceneRenderBridge` directly, not the
  `EditorSyncRouter`).
- **Runtime-only mutation.** The authoring document is never mutated by
  a deferred change. Stop destroys the runtime document and any
  pending queue contents.
- **No new public SceneManager APIs.** The `RuntimeSceneMutator` is a
  separate, small class in `rt2::core`, not a SceneManager extension.

### Files

- New: `RT2App/src/RuntimeLifecycleObserver.h` (the
  `IRuntimeLifecycleObserver` interface, dependency-free),
  `RT2App/src/RuntimeSceneMutator.h/.cpp` (the
  `RuntimeEntityCreateDesc`, `RuntimeSceneMutator` class),
  `RT2Tests/src/Phase4LifecycleTests.cpp`.
- Modified: `RuntimeSceneController.h/.cpp` (injectable UUID provider
  storage + setter; observer storage + dispatch in Play/Stop with the
  precise ordering in Â§6; single FIFO `m_PendingOperations` queue +
  `QueueCreateRuntimeEntity` / `QueueDestroyRuntimeEntity` +
  `ApplyDeferredStructuralChanges` with batch-validate-then-apply;
  structural-impact coalescing in `Update`/`Step`; created-transform
  `prevWorldMatrix` initialization; `m_Stopping` queue-disable flag),
  `WalnutApp.cpp` (one line: inject the UUID provider),
  `RT2Tests/premake5.lua` and `RT2Tests.vcxproj` (new test file +
  `RuntimeSceneMutator.cpp` registered).

### Verification gates

Release x64 build; focused Phase 4 lifecycle tests; full RT2Tests where
the only permitted failures are exactly the six known pre-existing
cases; `run_slice_test.ps1` and `run_recovery_test.ps1` pass;
`graphify update .`; documentation updates with actual test counts.

### Runtime acceptance (interactive, pending user)

- Play, observe motion, Stop â€” authoring scene unchanged (existing
  behavior, now backed by an explicit canonical-serialization test).
- (No new interactive surface â€” the queue and observer are exercised by
  tests only this phase; Phase 6 wires them to scripting.)

### Exit criterion (post-Phase-4)

Repeated Play/Stop cycles â€” including cycles with deferred structural
operations pending, including cycles where a batch validation fails and
the queue is left intact â€” neither leak entities/resources nor alter
saved scene state. The authoring document's canonical serialized state
is identical across any number of Play/Stop cycles with any sequence of
runtime operations.

### Explicitly out of scope for this Phase 4 completion slice

Scripting (`OnCreate`/`OnFixedUpdate`/`OnUpdate`/`OnDestroy` â€” Phase 6);
the `IRuntimeCommandSink` mutation channel for `OnSceneStart` (Phase 6);
physics (Phase 9); variable-update script callbacks (Phase 6); exposing
the queue to Lua (Phase 6); content browser / asset database (Phase 7);
input action system (Phase 5); runtime creation of mesh/light/camera/
primitive entities (Phase 6 â€” Phase 4 supports only empty +
transform + name + visibility); runtime reparenting during Play (Phase
6). The observer interface and mutator are added now so Phase 6 can
hook scripting into them without further `RuntimeSceneController`
structural changes.

## Phase 5 — Input action system (completion, implemented)

### Survey summary (current state, post-Phase-4)

The RT2 codebase has two parallel, uncoordinated input paths today, and
no runtime/gameplay input layer whatsoever:

1. **`Walnut::Input` (GLFW polling, held-only).** Defined in
   `Walnut/Walnut/src/Walnut/Input/Input.cpp`. Exposes `IsKeyDown`,
   `IsMouseButtonDown`, `GetMousePosition`, `SetCursorMode`. No
   edge-triggered "pressed this frame" / "released this frame" state, no
   scroll, no modifiers, no joystick. The only consumer is
   `RT2App/src/Camera.cpp:52-126` (`Camera::OnUpdate`), which hardcodes
   W/S/A/D/Q/E + right-mouse-look.
2. **`ImGui::IsKeyPressed` / `IsMouseClicked` / `io.KeyCtrl` (edge-
   triggered).** Used by every editor shortcut (Undo/Redo, Delete,
   Copy/Paste/Duplicate, F to focus, Ctrl+number camera bookmarks),
   the gizmo mode hotkeys (W/E/R in `EditorTransformGizmo.cpp:101-103`),
   viewport mouse picking (`WalnutApp.cpp:733-743`), and gizmo dragging
   (`EditorTransformGizmo.cpp:153-237`). ImGui's GLFW backend
   (`ImGui_ImplGlfw_InitForVulkan` at `Walnut/Application.cpp:635`) is
   the only event source in the app — Walnut itself registers no GLFW
   key/mouse/scroll/joystick callbacks.

There is a latent W/E key conflict: gizmo mode-switch (viewport-hovered,
no right-mouse) vs. camera translate/up-down (right-mouse held). They do
not collide today only because the camera gate (right-mouse) and the
gizmo gate (viewport-hovered, no drag) happen to be disjoint.

The runtime Play path runs the *same* `Camera::OnUpdate` against
`m_RuntimeCam` (`WalnutApp.cpp:977-980`), so during Play the editor
flycam bindings are still active and there is no gameplay input
consumer. `RuntimeSceneController` reads no input. `RT2SliceRunner` is
headless and links no GLFW/Walnut/ImGui.

`EditorSettingsStore` (`RT2App/src/EditorSettings.h/.cpp`) is CPU-only
(no GLFW/ImGui/Walnut) and versioned (`SettingsVersion = 1`); its loader
ignores unknown optional fields, giving a clean v2 upgrade path for
serialized input bindings. `RT2SliceRunner` and `EditorSettingsStore`
both enforce a CPU-only boundary that any new input-binding types must
respect: action names are plain strings, key codes are plain integers
mirroring GLFW's numeric values (not `GLFW_KEY_*` macros).

No `InputAction` / `InputAxis` / `InputMapping` / `InputContext` /
`InputState` types exist. `KeyState { None, Pressed, Held, Released }`
is defined in `Walnut/Input/KeyCodes.h:143` but unused.

### Walnut frame ordering (load-bearing for this design)

Walnut's `Application::Run` (see `Walnut/Walnut/src/Walnut/Application.cpp:745-854`)
orders the frame as:

1. `glfwPollEvents()` (`:751`).
2. Timestep computed (`:759-762`).
3. `Layer::OnUpdate(ts)` called for each layer (`:765`).
4. `ImGui_ImplVulkan_NewFrame` / `ImGui_ImplGlfw_NewFrame` /
   `ImGui::NewFrame` (`:787-789`).
5. DockSpace + menubar built; `Layer::OnUIRender()` called for each
   layer (`:843`).
6. `ImGui::Render()` and submission (`:849+`).

**This means `OnUpdate` runs BEFORE `ImGui::NewFrame`**, and the
shortcuts / viewport hover / gizmo handling in `OnUIRender` run LATER in
the same frame. Any input frame that begins and ends entirely inside
`OnUpdate` would: (a) sample stale ImGui IO state from the previous
frame, (b) not yet know viewport-hover / capture state, (c) clear scroll
before UI consumers run, and (d) query `ImGui::IsKeyPressed` outside its
valid frame phase. The Phase 5 input frame is therefore split across
both stages — see §"Frame phasing" below.

### Outcome

Gameplay code (Phase 6 scripts, and the editor camera in the meantime)
consumes stable named actions and axes through a single input service,
independent of GLFW and ImGui. Editor and runtime input are isolated by
explicit contexts with consumption semantics. Mappings are serialized in
project settings. A read-only input service is exposed for future
scripting. Controller support uses GLFW's standardized gamepad API.
Focus-loss reset is first-class and does not compete with ImGui's
callbacks.

### Design constraints

- **Do not modify `Walnut` itself.** Walnut is a vendored upstream
  library. The Phase 5 input system lives in `RT2App/src` and consumes
  Walnut's existing `Input` polling API plus ImGui's `GetIO()` state.
  It does not patch `Walnut::Application`, does not install its own
  GLFW key/mouse/scroll/window-focus callbacks (those would conflict
  with ImGui's backend — see §"Focus loss" below), and does not add a
  `Walnut::Input::IsKeyPressed` edge API. The service samples ImGui's
  IO state each frame and derives its own edges from raw-down
  snapshots; it does NOT use `ImGui::IsKeyPressed` as an edge source.
- **CPU-only state machine, separate desktop backend.** The state
  machine (mappings, contexts, edges, axes) lives in a CPU-only
  `InputStateMachine` with no GLFW/ImGui/Walnut includes. The GLFW/
  ImGui/Walnut sampling lives in a separate `DesktopInputBackend`.
  `InputService` composes the two. This lets `RT2Tests` link and test
  the state machine directly without a new integration target; the
  desktop backend's GLFW/ImGui sampling is exercised by an
  `RT2AppIntegrationTests` target (see §"Test target decision" below).
- **No new public SceneManager or RuntimeSceneController APIs.** The
  input service is hosted by `WalnutApp` (RT2App), not scene core.
  Phase 6 scripting will receive a read-only `IInputService` reference
  alongside the runtime document via the existing
  `IRuntimeLifecycleObserver::OnSceneStart` seam (Phase 6 adds the
  mutation channel + input reference; Phase 5 only builds the service
  and the editor-runtime context switching).
- **No ImGui, no Walnut-app behavior change.** The Play/Pause/Step/Stop
  UI is unchanged. Existing editor shortcuts keep working. The camera
  movement is refactored to read from the input service, but its
  visible behavior (WASD + right-mouse-look) is identical.

### Core types (CPU-only, in `RT2App/src/InputTypes.h`)

```
namespace rt2::core {

// Integer key code mirroring GLFW numeric values (e.g. 'W' = 65).
// Stored as uint16_t so the schema is portable and CPU-only.
enum class KeyCode : uint16_t;
enum class MouseButton : uint16_t;   // GLFW mouse button values
enum class GamepadButton : uint8_t;  // GLFW_GAMEPAD_BUTTON_* values
enum class GamepadAxis : uint8_t;    // GLFW_GAMEPAD_AXIS_* values
enum class ModifierBits : uint8_t;   // Ctrl/Shift/Alt bitfield

// Which device a binding sources from. Required so that key=1 and
// mouseButton=1 are distinguishable in the serialized form.
enum class InputDeviceKind : uint8_t
{
    KeyboardKey,
    MouseButton,
    GamepadButton,
};

// Edge-triggered and held state for a logical action. Computed AFTER
// combining all of an action's bindings (see §"Edge computation").
enum class ActionState : uint8_t
{
    None      = 0,
    Pressed   = 1,   // edge: up -> down this frame
    Held      = 2,   // down both this frame and last frame
    Released  = 3,   // edge: down -> up this frame
    // "IsPressedThisFrame" == (state == Pressed)
    // "IsDown"            == (state == Pressed || state == Held)
};

struct ActionBinding
{
    InputDeviceKind device = InputDeviceKind::KeyboardKey;
    // For device == KeyboardKey:   KeyCode key
    // For device == MouseButton:   MouseButton button
    // For device == GamepadButton: GamepadButton button
    uint16_t code = 0;
    ModifierBits modifiers = ModifierBits(0);
    // Gamepad slot (see §"Gamepad handling"). -1 = "any connected
    // gamepad". Persisted as a logical player slot, not a raw jid.
    int gamepadSlot = -1;
};

struct AxisBinding
{
    InputDeviceKind device = InputDeviceKind::KeyboardKey;
    // Keyboard axis: positive/negative KeyCode.
    // Gamepad axis:  code = GamepadAxis, positive/negative ignored.
    uint16_t code = 0;
    uint16_t positive = 0;
    uint16_t negative = 0;
    int gamepadSlot = -1;
    float deadZone = 0.15f;
    bool  invert = false;
};

struct InputMapping
{
    std::string name;                        // e.g. "move_forward"
    bool isAxis = false;
    std::vector<ActionBinding> actions;
    std::vector<AxisBinding>   axes;
};

// A context owns a set of named mappings. Contexts are stacked; the
// resolution policy is defined in §"Context stack and consumption".
class InputContext
{
public:
    explicit InputContext(std::string id) : m_Id(std::move(id)) {}
    const std::string& Id() const { return m_Id; }
    void SetMapping(InputMapping m);
    const InputMapping* FindMapping(const std::string& name) const;
    bool HasMapping(const std::string& name) const;
private:
    std::string m_Id;
    std::unordered_map<std::string, InputMapping> m_Mappings;
};

// Read-only input service interface. Phase 6 scripts receive a const
// reference to this through the lifecycle observer seam (Phase 6 adds
// the reference; Phase 5 builds the service).
class IInputService
{
public:
    virtual ~IInputService() = default;
    virtual ActionState GetActionState(const std::string& name) const = 0;
    bool IsPressed(const std::string& name) const
    { return GetActionState(name) == ActionState::Pressed; }
    bool IsDown(const std::string& name) const
    {
        auto s = GetActionState(name);
        return s == ActionState::Pressed || s == ActionState::Held;
    }
    bool IsReleased(const std::string& name) const
    { return GetActionState(name) == ActionState::Released; }
    virtual float GetAxisValue(const std::string& name) const = 0;
    virtual glm::vec2 GetMouseDelta() const = 0;
    virtual float GetScrollDelta() const = 0;
    // Cursor capture is a host-controlled request, see §"Cursor
    // ownership". The service does NOT call glfwSetInputMode directly.
    virtual void RequestCursorCapture(bool locked) = 0;
    virtual bool IsCursorCaptureRequested() const = 0;
};

} // namespace rt2::core
```

### Architecture: state machine + desktop backend + service

```
RT2App/src/InputTypes.h           CPU-only types (above). No GLFW/ImGui/Walnut.
RT2App/src/InputStateMachine.h/.cpp
                                  CPU-only. Owns:
                                    - the context stack
                                    - per-binding previous/current down state
                                    - edge computation (§"Edge computation")
                                    - axis computation (§"Axis computation")
                                    - focus-loss state
                                  Test-only backdoor: SetSampleState(...)
                                  injects a frame's synthetic raw-down
                                  snapshot without any GLFW/ImGui call.
RT2App/src/DesktopInputBackend.h/.cpp
                                  Links Walnut/ImGui/GLFW. One method:
                                    RawInputSnapshot CaptureFrame()
                                  which polls Walnut::Input, ImGui::GetIO(),
                                  glfwGetGamepadState, and
                                  glfwGetWindowAttrib(GLFW_FOCUSED).
                                  No state, no context logic.
RT2App/src/InputService.h/.cpp
                                  Composes the two. Implements IInputService.
                                  Drives the frame phasing (§"Frame
                                  phasing"). Hosts the cursor-capture
                                  request and platform cursor calls.
```

`InputStateMachine` is linked into `RT2Tests` directly. `DesktopInputBackend`
and `InputService` are linked into `RT2App` and the new
`RT2AppIntegrationTests` target (§"Test target decision").

### Frame phasing

The input frame spans both `OnUpdate` and `OnUIRender`:

1. **`InputService::SampleRaw()` — called at the top of
   `WalnutApp::OnUpdate` (before `m_Cam.OnUpdate`).** Calls
   `DesktopInputBackend::CaptureFrame()` to snapshot:
   - `Walnut::Input::IsKeyDown` / `IsMouseButtonDown` for every key /
     button referenced by any context's bindings.
   - `ImGui::GetIO()`'s `KeyCtrl/KeyShift/KeyAlt`, `MouseWheel`,
     `MouseDown[]`, `MousePos` (ImGui's IO is valid to read here — it
     reflects the previous frame's `ImGui_ImplGlfw_NewFrame` data; the
     edge derivation is based on raw-down snapshots, NOT on
     `ImGui::IsKeyPressed`).
   - `glfwGetGamepadState` for each present gamepad slot.
   - `glfwGetWindowAttrib(window, GLFW_FOCUSED)` for focus state.
   The raw snapshot is handed to `InputStateMachine::BeginFrame(snapshot)`,
   which advances per-binding current/previous down state. At this
   point action edges and axis values are computed from the raw-down
   state only, WITHOUT consulting viewport-hover or widget-capture
   state. This lets the camera read `move_forward` / `look` etc. inside
   `OnUpdate`.
2. **`InputService::ResolveUI()` — called at the top of
   `WalnutApp::OnUIRender` (after `ImGui::NewFrame`, before any panel
   code).** Samples the UI capture state that was not available in
   `OnUpdate`:
   - `ImGui::GetIO().WantTextInput`, `WantCaptureKeyboard`,
     `WantCaptureMouse`.
   - `ImGui::IsWindowHovered()` / `ImGui::IsItemHovered()` for the
     viewport panel (viewport hover is known only inside `OnUIRender`).
   - Active-widget state (`ImGui::IsAnyItemActive()`).
   - Gizmo consumption state (queried from `EditorTransformGizmo`).
   The state machine applies an editor routing policy (§"Editor routing
   policy") that suppresses actions when ImGui wants text input, when a
   widget is active, when the gizmo is consuming mouse, etc. The
   suppressed actions are marked `None` for this frame. Context-stack
   transitions (pushing `"viewport"` / `"viewport.look"`) also happen
   here, since viewport hover is known only now.
3. **`InputService::EndFrame()` — called at the end of
   `WalnutApp::OnUIRender` (after all panel code, before
   `ImGui::Render`).** Commits the current down-state as next frame's
   "previous" state, clears per-frame deltas (scroll, mouse delta),
   and applies any pending cursor-capture request (§"Cursor ownership").
   After `EndFrame`, no further input queries are valid until the next
   frame's `SampleRaw`.

This phasing keeps the input frame open across both stages, derives
edges from raw-down snapshots (never from `ImGui::IsKeyPressed`), and
ensures scroll is not cleared before UI consumers run.

### Edge computation

Edges are computed PER ACTION after combining all of the action's
bindings, NOT per binding:

```
previousActionDown = any of this action's bindings was down last frame
currentActionDown  = any of this action's bindings is down this frame

state = (currentActionDown, previousActionDown) =>
    (true,  false) -> Pressed
    (true,  true ) -> Held
    (false, true ) -> Released
    (false, false) -> None
```

This avoids false Released edges when one binding is released while
another remains held.

### Axis computation

Keyboard axis value:
```
value = (down(negative) ? -1.0f : 0.0f) + (down(positive) ? 1.0f : 0.0f)
clamped to [-1, 1]
```

Gamepad axis value (sign-preserving dead zone):
```
float applyDeadZone(float raw, float dz) {
    if (std::abs(raw) <= dz) return 0.0f;
    float sign = (raw < 0.0f) ? -1.0f : 1.0f;
    return sign * (std::abs(raw) - dz) / (1.0f - dz);
}
value = applyDeadZone(raw, deadZone)
if (invert) value = -value
```

An axis's final value is the sum of its bindings' values, clamped to
[-1, 1].

### Context stack and consumption

`InputService` owns a `std::vector<InputContext*>` stack (non-owning
pointers; contexts are owned by the host). Push / pop / clear are
host-driven.

**Resolution policy: physical-source consumption with lower-context
blocking.** This replaces the original "fall through" design, which did
not actually resolve the W/E conflict (a `move_forward` query in the
`"editor"` context would still see W even when the `"viewport"` context
was active, because `"viewport"` mapped `gizmo_translate` but not
`move_forward`).

New policy:

1. For each physical source (key, mouse button, gamepad button/axis)
   referenced by any binding in the active stack, the topmost context
   that maps that physical source **claims** it.
2. A context's bindings only fire if their physical source has not been
   claimed by a higher context.
3. A context's action/axis queries are answered from the bindings that
   survived the claim check.

Concretely, when the `"viewport"` context is top and maps W →
`gizmo_translate`, W is claimed by `"viewport"`. The `"editor"` context
below also has a W → `move_forward` binding, but W is already claimed,
so `editor.move_forward` does not fire. Result: with viewport hovered
and no right-mouse, `IsPressed("gizmo_translate")` is true and
`GetAxisValue("move_forward")` is 0 — which is what the integration test
expects.

When the `"viewport.look"` context is pushed (right-mouse held) and
maps W → `move_forward`, W is claimed by `"viewport.look"`, and the
`"viewport"` context's W → `gizmo_translate` binding does not fire.
Result: right-mouse-held + W → camera moves forward, gizmo mode does
not toggle.

Context-stack transitions (driven by `ResolveUI`):

- **Edit state, viewport not hovered:** stack = `["editor"]`.
- **Edit state, viewport hovered, no right-mouse:** stack =
  `["editor", "viewport"]`. `"viewport"` maps W/E/R → gizmo mode
  actions; these claim W/E/R.
- **Edit state, viewport hovered, right-mouse held:** stack =
  `["editor", "viewport", "viewport.look"]`. `"viewport.look"` maps
  W/S/A/D/Q/E → camera axes and right-mouse → `look`; these claim
  their sources, so gizmo-mode bindings do not fire.
- **Play state:** stack = `["runtime"]`. The runtime context maps
  gameplay actions; editor shortcuts and gizmo actions are unmapped,
  so they do not fire. The viewport.look sub-context can still be
  pushed during Play for runtime flycam.
- **Stop:** pop `"runtime"`, re-push `"editor"`.

### Editor routing policy (ImGui suppression preserved)

Contexts alone do not replace ImGui's `io.WantTextInput`,
`WantCaptureKeyboard`, active-widget and gizmo-consumption checks.
`InputService::ResolveUI` applies an editor routing policy:

- If `io.WantTextInput || io.WantCaptureKeyboard`: suppress all
  keyboard-sourced actions for this frame (they become `None`). Mouse
  actions are unaffected unless `io.WantCaptureMouse` is also true.
- If `io.WantCaptureMouse && !viewportHovered`: suppress all
  mouse-sourced actions.
- If `ImGui::IsAnyItemActive()` and the active item is not the viewport
  image: suppress keyboard-sourced actions (the user is typing in a
  widget, dragging a slider, etc.).
- If `gizmo.consumesMouse` (queried from `EditorTransformGizmo`):
  suppress the viewport-pick action; allow gizmo-drag actions.
- The suppression is recorded per-action so `IsDown("move_forward")`
  does not return true while typing in a text box.

This preserves the existing behavior where Ctrl+Z, Delete, W/E/R and
viewport clicks do not activate while typing or manipulating widgets.

### Camera refactor

`Camera::OnUpdate(float ts)` is refactored to accept an
`IInputService&` instead of polling `Walnut::Input` directly. The
hardcoded W/S/A/D/Q/E reads become:

```
float forward = input.GetAxisValue("move_forward");
float right   = input.GetAxisValue("move_right");
float up      = input.GetAxisValue("move_up");
if (input.IsDown("look")) {
    input.RequestCursorCapture(true);
    glm::vec2 delta = input.GetMouseDelta();
    // … pitch/yaw …
} else {
    input.RequestCursorCapture(false);
    return;
}
// translate by (forward, right, up) * m_Speed * ts
```

`WalnutApp::OnUpdate` passes `m_InputService` to `m_Cam.OnUpdate(ts,
m_InputService)` and `m_RuntimeCam.OnUpdate(ts, m_InputService)`. The
visible behavior is unchanged: same keys, same speed, same right-mouse
gate. The camera no longer includes `Walnut/Input/Input.h`; it depends
only on `IInputService`. The camera no longer calls
`Input::SetCursorMode` directly — it calls
`input.RequestCursorCapture(true/false)` and the service applies the
request at `EndFrame` (§"Cursor ownership").

### Editor shortcut refactor

The shortcut blocks in `WalnutApp.cpp::HandleEditorCameraShortcuts`
(1092-1121), `HandleUndoRedoShortcuts` (1128-1144), the viewport pick
block (733-743), `SceneEditorUI.cpp` (678-695), and
`EditorTransformGizmo.cpp` (99-104) are rewritten to read from
`m_InputService` instead of `ImGui::IsKeyPressed` / `io.KeyCtrl`. The
mappings move into the `"editor"` and `"viewport"` contexts. This is a
mechanical translation — every `ImGui::IsKeyPressed(ImGuiKey_X)` under
`io.KeyCtrl` becomes `input.IsPressed("redo")` (or whatever the mapped
action name is). The ImGui IO state is still sampled by the desktop
backend (it's a raw-down source), but the shortcut code no longer
touches ImGui directly. Edge detection is the state machine's job, not
ImGui's.

### Gamepad handling

Phase 5 introduces gamepad support from scratch using GLFW's
standardized gamepad API:

- **`glfwGetGamepadState(int jid, GLFWgamepadstate* state)`** is the
  primary polling call. It returns `GLFW_TRUE` if `jid` is present AND
  has a known gamepad mapping; raw joystick fallback is NOT used.
- **Standardized controls:** `GLFW_GAMEPAD_BUTTON_*` (A/B/X/Y, bumpers,
  back/start/guide, thumb, dpad) and `GLFW_GAMEPAD_AXIS_*` (left/right
  X/Y, left/right trigger). The service does NOT use
  `glfwGetJoystickAxes` / `glfwGetJoystickButtons` directly.
- **Player slots, not raw jids.** `gamepadSlot` in `ActionBinding` /
  `AxisBinding` is a logical player slot (0..3). The service maintains
  a slot→jid map by enumerating `glfwJoystickIsGamepad(GLFW_JOYSTICK_1
  .. 16)` at the start of each `SampleRaw` and assigning slots in
  ascending jid order. Slot 0 = "first connected gamepad", slot 1 =
  "second", etc. A binding with `gamepadSlot = -1` matches "any
  connected gamepad" (resolved to slot 0). Raw jids are NOT persisted
  (they are not stable across reconnects or launches).
- **Connect/disconnect:** polled per frame via the slot→jid map. A
  disconnect zeroes the slot's axis values and releases its button
  bindings for one frame. No `glfwSetJoystickCallback` is registered
  (it would not conflict with ImGui, but polling is simpler and
  uniform with the rest of the design).
- **GUID persistence is NOT in scope** for Phase 5. The slot model is
  purely positional. GUID-based player assignment ("this controller is
  always Player 1") is a future convenience.

Default runtime context gamepad mappings:
- Left stick X → `move_right`, Y → `move_forward` (with `invert = true`
  on Y, since GLFW's Y is up = -1).
- Right stick X → `look_yaw`, Y → `look_pitch` (invert on Y).
- A/Cross → `jump`, X/Square → `primary_action`.

Dead-zone (sign-preserving) and inversion are per-binding, serialized
in the mapping (§"Edge computation" / §"Axis computation").

### Focus loss

ImGui's GLFW backend already installs
`glfwSetWindowFocusCallback` (`imgui_impl_glfw.cpp:430`) and chains
the previous callback via `PrevUserCallbackWindowFocus`. Installing a
competing callback would either clobber ImGui's or require careful
chaining and restoration — fragile. Phase 5 polls instead:

```
bool focused = glfwGetWindowAttrib(window, GLFW_FOCUSED);
```

`DesktopInputBackend::CaptureFrame` records `focused` in the raw
snapshot. The state machine detects transitions:

- `focused == false` (regardless of previous): mark all currently-down
  bindings as `Released` for one frame, zero all axis values, set
  `m_FocusLost = true`.
- While `m_FocusLost`: ignore raw-down samples (treat all keys / buttons
  / axes as up). This is belt-and-suspenders against GLFW reporting
  stale state while unfocused.
- `focused == true && m_FocusLost`: clear `m_FocusLost` on the next
  `BeginFrame`. Do NOT immediately treat the current down-state as
  `Pressed` — the first focused frame's down-state becomes the
  "previous" for the next frame's edge computation, so a key held
  through refocus is `Held`, not `Pressed`. This prevents accidental
  edge spikes on refocus.
- On focus loss / regain / context-stack transition: reset mouse
  position history so the first-frame mouse delta is zero, not a large
  jump from the pre-transition position.

### Cursor ownership

`Walnut::Input::SetCursorMode` calls `glfwSetInputMode(GLFW_CURSOR,
…)`. The camera today calls it directly. Phase 5 centralizes cursor
capture on `IInputService`:

- `IInputService::RequestCursorCapture(bool locked)` records the
  request. Multiple consumers can request; the service applies the
  most-recent request at `EndFrame` (so a later `RequestCursorCapture(
  false)` in `OnUIRender` overrides an earlier `true` from `OnUpdate`
  if the right-mouse was released mid-frame).
- `InputService::EndFrame` calls `Walnut::Input::SetCursorMode(locked
  ? CursorMode::Locked : CursorMode::Normal)`.
- The camera no longer calls `SetCursorMode` directly.
- On focus loss, focus regain, and context-stack transitions: the
  service forces `CursorMode::Normal` for one frame and resets mouse
  position history (§"Focus loss") to prevent a large first-frame look
  delta.

### Serialization

`EditorSettingsStore` schema bumps to v2. New optional field:

```json
{
  "version": 2,
  "projectRoot": "...",
  "recentScenes": ["..."],
  "inputContexts": [
    {
      "contextId": "editor",
      "mappings": [
        {
          "name": "move_forward",
          "isAxis": true,
          "axes": [
            { "device": 0, "code": 0, "positive": 87, "negative": 83,
              "gamepadSlot": -1, "deadZone": 0.15, "invert": false }
          ],
          "actions": []
        },
        {
          "name": "look",
          "isAxis": false,
          "actions": [
            { "device": 1, "code": 1, "modifiers": 0, "gamepadSlot": -1 }
          ],
          "axes": []
        }
      ]
    },
    {
      "contextId": "viewport",
      "mappings": [
        {
          "name": "gizmo_translate",
          "isAxis": false,
          "actions": [
            { "device": 0, "code": 87, "modifiers": 0, "gamepadSlot": -1 }
          ],
          "axes": []
        }
      ]
    },
    {
      "contextId": "runtime",
      "mappings": [ … ]
    }
  ]
}
```

Notes:

- **`device`** is the `InputDeviceKind` enum value (0 = KeyboardKey,
  1 = MouseButton, 2 = GamepadButton). This disambiguates `code = 1`
  for keyboard key 1 vs. mouse button 1.
- **`contextId`** groups mappings by context. A flat `inputMappings`
  array cannot represent `"editor"`, `"runtime"` and `"viewport"`
  contexts with overlapping action names (e.g. both `"editor"` and
  `"viewport.look"` map W). The new schema is a list of contexts, each
  with its own mapping list.
- The loader at `EditorSettings.cpp:64-148` already ignores unknown
  optional fields, so a v1 settings file with no `inputContexts` field
  loads cleanly and the service falls back to built-in defaults. A v2
  file on a v1-only loader is rejected by the version check (existing
  behavior).

Built-in defaults (used when `inputContexts` is absent or empty) are
constructed in code by `InputService::LoadDefaults()`, mirroring the
current hardcoded bindings (W/S/A/D/Q/E, right-mouse look, Ctrl+Z/Y,
Delete, F, Ctrl+C/V/D, Ctrl+1..N, W/E/R for gizmo) plus the default
runtime gamepad mappings (§"Gamepad handling"). This guarantees the
editor behaves identically before and after the migration.

### Runtime input routing

During Play, the `"runtime"` context is active. The runtime
`Camera::OnUpdate` call (against `m_RuntimeCam`) now reads from the
runtime context's mappings, not the editor context's. The editor
shortcuts (Undo/Redo/Delete/Copy/Paste) are NOT mapped in the runtime
context, so they are inert during Play. This matches the spec's exit
criterion: "Switch window focus while holding a key and confirm no
stuck action on return" and "Verify editor shortcuts do not move the
player while editing script fields."

Phase 6 will add `IInputService&` to the `OnSceneStart` callback
signature (alongside the `IRuntimeCommandSink` mutation channel). Phase
5 only builds the service and the context switching; scripts are not
yet a consumer.

### Test target decision

Two test targets:

1. **`RT2Tests` (CPU-only, existing).** Links `InputStateMachine.cpp`
   (no GLFW/ImGui/Walnut). Tests the state machine, context stack,
   edge computation, axis computation, focus-loss logic, and mapping
   serialization round-trip via the `SetSampleState` backdoor. This is
   the bulk of the test surface and runs in the existing CI path.
2. **`RT2AppIntegrationTests` (new target, links Walnut + ImGui + GLFW
   + RT2App source).** Tests `DesktopInputBackend::CaptureFrame`,
   `InputService`'s frame phasing across `OnUpdate` / `OnUIRender`,
   context-stack transitions driven by viewport hover, ImGui
   suppression policy, gamepad polling, and cursor capture. This is a
   small console executable that constructs a hidden GLFW window,
   drives `InputService` through one or more frames with synthetic
   GLFW state where possible, and asserts on the resulting
   `IInputService` queries. (Where GLFW state cannot be injected
   synthetically — e.g. viewport hover — the test uses ImGui's
   test-mode hooks or is marked as a manual acceptance step.)

The decision is made now: `RT2Tests` stays CPU-only and covers the
state machine; `RT2AppIntegrationTests` is the new integration target.
The spec no longer says "possibly a new target."

### Test surface

**`RT2Tests/src/InputStateMachineTests.cpp` (CPU-only):**

- Synthetic sample state via `SetSampleState` produces correct
  `Pressed` / `Held` / `Released` edges across multiple frames.
- **Edge computation combines bindings:** two bindings on one action,
  release one while the other stays held → action stays `Held`, NOT
  `Released`.
- **`Pressed` is up→down**, not down→up (comment correctness).
- Multiple bindings to one action combine disjunctively (any fires).
- Keyboard axis `(down(neg) ? -1 : 0) + (down(pos) ? 1 : 0)` clamps.
- Gamepad axis dead-zone is sign-preserving:
  `applyDeadZone(-0.5, 0.15) ≈ -0.4118`, `applyDeadZone(0.1, 0.15) ==
  0.0`, `applyDeadZone(1.0, 0.15) == 1.0`.
- Gamepad axis inversion is applied.
- **Context stack — physical-source consumption:**
  - `"viewport"` maps W → `gizmo_translate`; `"editor"` maps W →
    `move_forward`. With `"viewport"` on top, `IsPressed(
    "gizmo_translate")` is true, `GetAxisValue("move_forward")` is 0.
  - Push `"viewport.look"` (maps W → `move_forward`); now
    `GetAxisValue("move_forward")` is non-zero, `IsPressed(
    "gizmo_translate")` is false (W claimed by `"viewport.look"`).
- Focus-loss reset: all down bindings become `Released`, axes zero,
  subsequent frames ignore samples until refocus. First refocus frame
  does not produce `Pressed` edges (down-state becomes "previous").
- Mapping serialization round-trips through `EditorSettingsStore` v2
  with `device` and `contextId` fields.
- Unknown action names return `ActionState::None` and axis value 0
  safely (no throw, no assert).
- Empty mapping list → built-in defaults loaded.

**`RT2AppIntegrationTests/src/InputServiceFramePhaseTests.cpp`
(integration):**

- `OnUpdate` reads valid action state (raw-down derived) even though
  `ImGui::NewFrame` has not run yet.
- `OnUIRender` `ResolveUI` suppresses keyboard actions when
  `io.WantTextInput` is true.
- Viewport hover pushes `"viewport"` sub-context; W/E claim resolves
  as specified.
- Right-mouse held pushes `"viewport.look"`; gizmo-mode actions go
  `None`.
- Play pushes `"runtime"`, pops `"editor"`; `IsPressed("undo")` is
  false during Play.
- Stop re-activates `"editor"`; `IsPressed("undo")` works again.
- Focus loss while holding W: `IsDown("move_forward")` returns false
  on the next frame.
- `RequestCursorCapture(true)` from camera in `OnUpdate`;
  `EndFrame` applies `CursorMode::Locked`. A subsequent
  `RequestCursorCapture(false)` from `OnUIRender` overrides it.
- Gamepad connect mid-Play: axis `move_right` reads from the
  gamepad's left stick X within one frame (synthetic where possible,
  else manual acceptance).

### Files

- New: `RT2App/src/InputTypes.h` (CPU-only types, no GLFW/ImGui/Walnut).
- New: `RT2App/src/InputStateMachine.h/.cpp` (CPU-only state machine).
- New: `RT2App/src/DesktopInputBackend.h/.cpp` (GLFW/ImGui/Walnut
  snapshot collection).
- New: `RT2App/src/InputService.h/.cpp` (composes the two; implements
  `IInputService`; drives frame phasing and cursor capture).
- New: `RT2Tests/src/InputStateMachineTests.cpp` (CPU-only unit tests).
- New: `RT2AppIntegrationTests/` project (premake5.lua, vcxproj,
  `src/InputServiceFramePhaseTests.cpp`, `src/main.cpp`).
- Modified: `RT2App/src/Camera.h/.cpp` (refactored `OnUpdate` to take
  `IInputService&`; drop `Walnut/Input/Input.h` include; use
  `RequestCursorCapture`).
- Modified: `RT2App/src/WalnutApp.cpp` (own `InputService`, call
  `SampleRaw` / `ResolveUI` / `EndFrame` at the right frame phases,
  push/pop contexts in `EnterPlay`/`EnterStop` and viewport-hover,
  pass service to camera and shortcut handlers, rewrite shortcut
  handlers to read from service).
- Modified: `RT2App/src/EditorTransformGizmo.cpp` (gizmo mode hotkeys
  read from `IInputService`).
- Modified: `RT2App/src/SceneEditorUI.cpp` (hierarchy panel shortcuts
  read from `IInputService`).
- Modified: `RT2App/src/EditorSettings.h/.cpp` (schema v2, new
  `inputContexts` field, `LoadInputContexts`/`SaveInputContexts`).
- Modified: `RT2App/RT2App.vcxproj`, `RT2Tests/RT2Tests.vcxproj`,
  `RT2Tests/premake5.lua`, `RT2SliceRunner/premake5.lua` (link
  `InputStateMachine.cpp` into RT2Tests; link `InputService.cpp` +
  `DesktopInputBackend.cpp` into RT2App and RT2AppIntegrationTests;
  add `InputTypes.h` to all targets that need it).

### Verification gates

Release x64 build; `RT2Tests` (CPU-only) with the only permitted
failures being the six known pre-existing cases; new
`RT2AppIntegrationTests` passing; `run_slice_test.ps1` and
`run_recovery_test.ps1` pass; `graphify update .`; documentation
updates with actual test counts.

### Runtime acceptance (interactive, pending user)

- Rebind `move_forward` from W to Up arrow in the editor settings
  JSON, restart RT2, enter Play, and verify the new binding drives the
  camera immediately.
- Hold W, alt-tab away, alt-tab back: no stuck movement.
- Plug in a gamepad mid-Play: left stick drives `move_right` /
  `move_forward` within one frame.
- Enter Play with a runtime context mapping; verify Ctrl+Z does
  nothing (Undo is editor-only).
- Verify the gizmo W/E/R hotkeys still switch modes when the viewport
  is hovered and right-mouse is not held.
- Verify typing in a text widget suppresses W/E/R and Ctrl+Z.

### Exit criterion

No gameplay-facing code (and no editor camera or shortcut code) reads
GLFW key or mouse state directly, nor `ImGui::IsKeyPressed` /
`ImGui::IsMouseClicked` directly. All input consumption goes through
`IInputService`. The single GLFW-touching code path is
`DesktopInputBackend::CaptureFrame` (which polls `Walnut::Input`,
`ImGui::GetIO()`, `glfwGetGamepadState`, and
`glfwGetWindowAttrib(GLFW_FOCUSED)`). No new GLFW callbacks are
registered. The single `SetCursorMode` call is in
`InputService::EndFrame`.

### Explicitly out of scope for this Phase 5 completion slice

- Lua script access to `IInputService` (Phase 6 adds the reference to
  `OnSceneStart`).
- Per-entity input components (Phase 6 — gameplay scripts read the
  shared service, no per-entity mapping).
- Input-driven camera cuts / cinematic cameras (Phase 12 — standalone
  runtime).
- Touch input (later backlog).
- Hot-reload of input mappings while Playing (Phase 5 reloads on next
  Play/Stop cycle; live reload is a future convenience).
- Interactive rebinding UI (Phase 5 persists mappings and exposes
  load/save; the rebinding dialog is Phase 7's content-browser era —
  Phase 5 verification uses JSON editing of the settings file or the
  test-only `SetMapping` API).
- GUID-based gamepad persistence ("this controller is always Player
  1"). Phase 5 uses positional slots only.
- Multi-viewport input isolation (RT2 has one viewport; multi-viewport
  is a later UI phase).
- Raw-joystick fallback for non-gamepad controllers. Phase 5 only
  supports devices with a known GLFW gamepad mapping.

### Verification report

**Implemented (this slice):**
- `InputTypes.h` — CPU-only types (`KeyCode`, `MouseButton`,
  `GamepadButton`, `GamepadAxis`, `ModifierBits`, `InputDeviceKind`,
  `ActionState`, `ActionBinding`, `AxisBinding`, `InputMapping`,
  `InputContext`, `IInputService`). No GLFW/ImGui/Walnut includes.
- `InputStateMachine.h/.cpp` — CPU-only state machine: context stack,
  edge computation (per-action after combining bindings), axis
  computation (sign-preserving dead zone), focus-loss tracking via
  polled `windowFocused` flag, suppression. `SetSampleState` test
  backdoor.
- `DesktopInputBackend.h/.cpp` — single GLFW/ImGui/Walnut sampling
  path. Polls `Walnut::Input`, `ImGui::GetIO()`, `glfwGetGamepadState`,
  `glfwGetWindowAttrib(GLFW_FOCUSED)`. No GLFW callbacks registered.
- `InputService.h/.cpp` — composes the two; implements `IInputService`;
  drives frame phasing (`SampleRaw` / `ResolveUI` / `EndFrame`);
  cursor capture via `RequestCursorCapture`; `LoadDefaults()` populates
  editor + viewport + viewport.look + runtime contexts.
- `Camera.h/.cpp` — refactored `OnUpdate(float ts, IInputService&)`.
  WASD/QE reads replaced with `GetAxisValue`; right-mouse gate
  replaced with `IsDown("look")`; `SetCursorMode` replaced with
  `RequestCursorCapture`. Visible behavior unchanged.
- `WalnutApp.cpp` — owns `InputService`; `SampleRaw` at top of
  `OnUpdate`, `ResolveUI` at top of `OnUIRender`, `EndFrame` at end of
  `OnUIRender`; viewport sub-context push/pop based on viewport hover
  + right-mouse; `HandleEditorCameraShortcuts` and
  `HandleUndoRedoShortcuts` migrated to `IInputService`; viewport pick
  migrated to `IsPressed("viewport_pick")` / `IsDown("look")`.
- `EditorSettings.h/.cpp` — schema v2 with `inputContexts` field;
  v1 → v2 migration (v1 files load with empty `inputContexts`, caller
  falls back to `LoadDefaults()`).
- `InputStateMachineTests.cpp` — 18 tests, 60 assertions, all
  passing. Covers edge transitions, false-Released guard,
  disjunction, axis clamping, sign-preserving dead zone, inversion,
  physical-source consumption (W/E conflict resolved), focus-loss
  reset with refocus-no-spike, mouse delta, scroll, suppression
  (keyboard/mouse/named), modifier matching, context stack ordering.

**Deferred to a follow-up slice (not blocking Phase 6):**
- `EditorTransformGizmo.cpp` gizmo-mode hotkeys (W/E/R) — still use
  `ImGui::IsKeyPressed`. The context stack infrastructure is in place
  to absorb them; the migration is mechanical but the gizmo's
  `imageHovered && !m_Drag.active && !WantTextInput` gate needs to be
  expressed as a viewport sub-context push. The W/E conflict is
  resolved at the camera level (viewport.look claims W when
  right-mouse is held), but the gizmo W/E/R hotkeys still fire via
  ImGui when the viewport is hovered and right-mouse is NOT held.
- `SceneEditorUI.cpp` hierarchy shortcuts (Copy/Paste/Duplicate/
  Delete) — still use `ImGui::IsKeyPressed` + `io.KeyCtrl`. These
  have no conflicts and no gamepad equivalent; migration is
  mechanical.
- `RT2AppIntegrationTests` target — not yet created. The
  `InputStateMachine` is fully tested CPU-only via `RT2Tests`; the
  `DesktopInputBackend` and `InputService` frame phasing are
  exercised interactively by RT2App. An automated integration target
  that constructs a hidden GLFW window and drives `InputService`
  through synthetic frames is a follow-up.
- Interactive rebinding UI — Phase 5 persists mappings and loads
  them on startup; the rebinding dialog is Phase 7's content-browser
  era.

**Verification:**
- Release x64 build clean (RT2App + RT2Tests + RT2SliceRunner).
- `RT2Tests`: 372 cases, 366 passed, 6 pre-existing failures (5
  `SceneGraph` cases in `EcsTests.cpp` + 1 SIGSEGV in `SceneManager:
  RemoveEntity destroys entity`), 48 skipped. No regressions.
- `InputStateMachineTests`: 18/18 pass, 60 assertions.
- `run_slice_test.ps1` PASS.
- `run_recovery_test.ps1` PASS.
- graphify updated: 24825 nodes, 51458 edges, 945 communities.

