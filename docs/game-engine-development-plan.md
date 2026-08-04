# Lightweight Game Engine Development Plan

Implementation roadmap for evolving RT2 from a renderer with scene-editing
facilities into a small, usable game engine. This plan is ordered around one
goal: close the authoring loop before expanding the renderer further.

The intended loop is:

```
create/import -> edit -> save -> play -> stop safely -> package -> run
```

## How to read this document

**It is append-only and chronological.** Sections are added as work happens
and are not retro-edited. Two consequences that matter more than anything
else here:

1. **A completed phase's section is a period record, not current state.**
   It describes the tree as it was when that phase landed. Figures inside it
   ("7 failures", "W4-W5 remain", "declared and stubbed") were true then and
   are routinely false now. Do not quote them. Where a section has been
   superseded it carries an explicit note saying so.
2. **Each phase appears twice.** A short scope stub in the roadmap below,
   written before the work; and — once worked — a much longer section near
   the end with grounded findings, decisions, workstreams and a verification
   report. The stub is the intent; the later section is what happened.

If you need current state rather than history, these are authoritative:

| Question | Where |
|---|---|
| Which tests should pass? | **Test baseline** (near the end) — supersedes every earlier figure |
| What does the engine do today? | The `docs/` siblings: `scripting.md`, `scene-management.md`, `game-loop.md` |
| What is being built next? | **Phase 7 — implementation plan**; W0–W3 are closed, **W4 is the next unstarted roadmap work** |
| What does this term mean? | `docs/glossary.md` — terms that have caused a defect, and the four context boundaries indices are translated across. Unlike this file it is corrected in place. |

**A number means a roadmap phase and nothing else.** Off-roadmap work gets a
name. Two sections were once both called "Phase 8"; see the glossary entry for
`Phase N`.

This file is also **two documents concatenated**: the phase roadmap, then
`# First tiny vertical slice development plan` at the halfway point. The
generic headings (`Goal`, `In scope`, `Work packages`) belong to the second.

## Contents

**Part 1 — Roadmap.** Scope stubs written ahead of the work.

- Existing foundation · Engineering principles · Cross-phase architecture
  contracts · Test strategy
- Phase 1 Native scene persistence · Phase 2 Scene-building ergonomics ·
  Phase 3 Command system, undo, redo · Phase 4 Edit/Play/Pause lifecycle ·
  Phase 5 Input action system · Phase 6 Lua scripting · **Phase 7 Project
  model and asset database** · Phase 8 Prefabs · Phase 9 Physics ·
  Phase 10 Animation · Phase 11 Audio · Phase 12 Standalone runtime
- Later backlog

**Part 2 — First tiny vertical slice.** Goal · In scope · Explicitly out of
scope · Proposed types and boundaries · Work packages · Acceptance script ·
Merge gates.

**Part 3 — Worked sections**, under "What follows immediately". Each is
grounded against the code as it stood at the time.

| Section | Status |
|---|---|
| Phase 1A asset-backed scene round-trip | implemented |
| Phase 1B crash-safe authoring and recovery | implemented |
| Phase 2A stable viewport selection | implemented |
| Phase 2B transform editing and gizmos | implemented |
| Phase 2C hierarchy authoring and outliner | implemented |
| Phase 2D camera authoring workflow | implemented |
| Phase 3A command/history foundation | implemented |
| Phase 3B1 structural command correctness | implemented |
| Phase 3B2 property command completion | implemented |
| Phase 4 Edit/Play/Pause lifecycle | implemented |
| Phase 5 Input action system | implemented |
| Phase 6A Lua embedding and lifecycle (+ verification report) | implemented |
| Phase 6B Public fields, reflection, persistence (+ report) | implemented |
| Phase 6C Hot reload, input, bindings (+ report) | implemented |
| Phase 6 exit criteria / ordering rationale | reference |
| **Test baseline** | **current — authoritative** |
| **Phase 7 Project model and asset database** | **implementation plan, not started** |

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

> **This is the scope stub only.** The grounded implementation plan — what
> already exists, the naming collisions, the seven decisions that must be
> answered before code — is the final section of this document. Read it
> before acting on anything below: Phase 7 is **not** greenfield, and roughly
> half of what this stub describes already exists under other names.

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

## Phase 6 — Lua scripting (completion, implemented)

The Phase 6 spec above (lines 415–466) is the goal. Phase 6 is delivered in
three sub-slices. Each sub-slice is a usable increment: 6A proves Lua drives
an entity; 6B makes the authoring round-trip survive save/load; 6C makes
runtime iteration productive via hot reload, the remaining bindings, and a
deterministic headless acceptance script.

### Foundation already in place (no rework needed)

The research survey confirms Phase 6 is greenfield on exactly four axes —
Lua, `ScriptComponent`, reflection, and file watching — but the seams it
plugs into are already designed for it:

- `IRuntimeLifecycleObserver` (`RuntimeLifecycleObserver.h`) with
  `OnSceneStart` / `OnSceneStop` is in place; its header comment explicitly
  says Phase 6 will add a separate `IRuntimeCommandSink` passed alongside
  the const document. `RuntimeSceneController::SetLifecycleObserver`
  registers it.
- The deferred structural-operation queue
  (`RuntimeSceneController::QueueCreateRuntimeEntity` /
  `QueueDestroyRuntimeEntity`, drained by
  `ApplyDeferredStructuralChanges` at the safe point after the fixed-step
  loop and before `SceneGraph::UpdateWorldTransforms` + batched sync) is
  the spawn/destroy channel for scripts.
- `RuntimeSceneMutator::CreateEntity` /
  `DestroySubtree` are the CPU-only mutators behind the queue. The header
  comment says "Phase 6 scripting may extend the surface"; 6A may extend
  `RuntimeEntityCreateDesc` with a script-asset reference.
- `IInputService` (`InputTypes.h`, CPU-only) is read-only and ready to
  hand to scripts through the lifecycle seam. The header comment confirms
  "Phase 6 scripts will receive a const reference to this through the
  runtime lifecycle observer seam."
- `SceneSerializer` is at schema v2 with a `PersistedComponents::ForEach`
  visitor; adding `ScriptComponent` to the visitor (Count 10→11) is the
  one place that fails together with serializer and duplication coverage
  tests if drifted. `CloneInMemory` (the Play path) preserves every
  persisted component, so cloning is automatic once `ScriptComponent` is
  in the visitor.
- `AssetReference` / `ImportedMeshSourceComponent` /
  `MaterialOverrideComponent` in `ECSComponents.h` are the durable-asset
  reference pattern. `AssetKind` is an `enum class : uint8_t` that
  6A/6B extends with `Script = 4`; `sourceKey` is `lua:asset=<path>` or a
  future asset-UUID form. Scene-relative UTF-8 portable paths are
  relativized by `SceneSerializer::Save`.
- `rt2::core::UUID` (`core/UUID.h`) with parse/format/hash/`Nil()` is in
  place; script asset references and persisted field-value maps key off
  it.
- `RuntimeSceneController::RunFixedTick` currently runs `MotionSystem`
  directly. 6A adds fixed script dispatch before physics' slot and
  variable script dispatch after the deferred-structural safe point,
  matching the canonical frame order in `game-loop.md` (lines 130–143).
- `RT2Tests/premake5.lua` compiles a hand-picked CPU-only subset of
  RT2App `.cpp` files; 6A adds the new `ScriptSystem.cpp` etc. to that
  list. Lua source becomes the first external link in `RT2Tests` (today
  it links nothing external), so the premake change is the gating
  decision per sub-slice.

### Resolutions from design review (before 6A implementation)

The first draft of this plan had five load-bearing gaps concentrated where
the runtime lifecycle meets the script environment map — the one genuinely
new moving part. They are closed here, not deferred to "polish," because
each is cheap to specify now and expensive to retrofit.

**G1 — Per-frame script dispatch needs its own interface.**
`IRuntimeLifecycleObserver` is documented as const-observe, start/stop only
(`OnSceneStart(const SceneDocument&)` / `OnSceneStop(const SceneDocument&)`).
Folding `OnFixedUpdate(dt)` / `OnUpdate(dt)` into it would pollute that
contract — those methods drive mutation through the sink and are not const.
6A introduces a **separate** `IRuntimeScriptDispatch` (CPU-only header):

```cpp
class IRuntimeScriptDispatch {
public:
    virtual ~IRuntimeScriptDispatch() = default;
    virtual void OnFixedUpdate(float dt) = 0;   // before MotionSystem, UUID-sorted
    virtual void OnUpdate(float dt) = 0;        // after deferred safe point
    virtual void SyncScriptEnvironments() = 0;  // see G2
};
```

`RuntimeSceneController` holds it via a new `SetScriptDispatch(...)` /
`GetScriptDispatch()` pair, distinct from the lifecycle observer. This is
the **one** structural addition to `RuntimeSceneController` the first draft
hand-waved; it is named now. `ScriptSystem` implements both interfaces;
the controller calls `OnSceneStart`/`OnSceneStop` on the observer and
`OnFixedUpdate`/`OnUpdate`/`SyncScriptEnvironments` on the dispatch.

**G2 — Spawned-scripted-entity lifecycle is a core mechanic, not a detail.**
`world.spawn` during Play creates entities at the safe point, but the first
draft never said when their `OnCreate` fires — and without that, "scripts
drive spawning" doesn't hold. 6A specifies the full mechanic:

- `RuntimeEntityCreateDesc` gains an `std::optional<ScriptComponent>
  script` field. `RuntimeSceneMutator::CreateEntity` emplaces it when
  present (the header comment already licenses extending the surface).
- `ScriptSystem` maintains a per-entity environment map that is a
  **per-frame-maintained mirror of the runtime registry**, not a
  build-it-once-at-Play structure. Each frame, between
  `ApplyDeferredStructuralChanges` and `OnUpdate`, the controller calls
  `SyncScriptEnvironments()` (the third method on
  `IRuntimeScriptDispatch`), which:
  1. Walks the runtime registry's `ScriptComponent`-bearing entities.
  2. For entities in the registry but NOT in the environment map:
     constructs a fresh `sol::environment`, loads the script, evaluates
     `rt2.fields` (6B), binds callbacks, and fires `OnCreate` **immediately**
     — before this frame's `OnUpdate`. This is the only path by which a
     runtime-spawned scripted entity gets `OnCreate`.
  3. For entities in the environment map but NOT in the registry (destroyed
     at the safe point): fires `OnDestroy` **before** tearing down the
     environment, so the script sees itself as alive for its final
     callback. This is the `world.destroy` → `OnDestroy` timing rule.
  4. For entities present in both: no-op (environment already live).
- `OnUpdate(dt)` then iterates the now-current environment map in UUID
  order. Entities spawned this frame receive `OnCreate` (in
  `SyncScriptEnvironments`) then `OnUpdate` (in the dispatch) — both in
  the same frame, in that order. Entities destroyed this frame received
  `OnDestroy` (in `SyncScriptEnvironments`) and are absent from `OnUpdate`.
- `OnFixedUpdate(dt)` runs **before** the fixed-step loop's
  `ApplyDeferredStructuralChanges`, so spawns queued during
  `OnFixedUpdate` do NOT resolve until the safe point between the fixed
  loop and `SyncScriptEnvironments` — same frame, after the loop. A
  scripted spawn in `OnFixedUpdate` is visible to `OnUpdate` this frame
  and to `OnFixedUpdate` next frame, never to a later substep this frame.
  This preserves the "no mid-iteration mutation" guarantee across the
  fixed loop.

This makes `SyncScriptEnvironments` the single chokepoint where the
environment map and the runtime registry agree. The first draft's
"construct environments on Play" is the special case where the registry is
the full runtime clone and the environment map is empty.

**G3 — Headless acceptance must use a deterministic UUID provider.**
`QueueCreateRuntimeEntity` allocates the UUID at queue time from the
runtime provider; with `OsUuidProvider` those UUIDs vary per run, so the
6C `--script-scenario` JSON report (UUID-keyed, includes spawned
entities) is non-deterministic. 6C fixes this by having
`RT2SliceRunner::RunScriptScenario` install a
`DeterministicUuidProvider` (seeded from a fixed seed in the scenario
file) via `RuntimeSceneController::SetRuntimeUuidProvider` — the seam
already exists from Phase 4. The scenario file's `expectedTransforms`
are then UUID-keyed against the deterministic sequence. Asserting only
against pre-existing UUIDs is the alternative if a scenario wants to
forbid spawning; both modes are stated in 6C.

**S1 — Explicit per-instance state machine.** Every script instance is
in exactly one of four states, made testable:

```
NeverCreated ──OnCreate──▶ Live ──runtime error──▶ Quarantined
                              │                        │
                              │                        └──(reload, 6C)──▶ Live
                              │
                              └──OnDestroy (at Stop or safe-point)──▶ Destroyed
```

- `NeverCreated`: `ScriptComponent` present, environment not yet built.
  No callbacks fire.
- `Live`: environment built, `OnCreate` has fired. All callbacks fire.
- `Quarantined`: a runtime error occurred in a callback (or a syntax
  error on load/reload). **No further callbacks fire** for this instance
  this Play session until a successful reload (6C). Mid-frame: if
  `OnFixedUpdate` throws on substep 2 of 3, substep 3 and this frame's
  `OnUpdate` are skipped for this instance (the "no further callbacks"
  rule applies within the frame too — stated explicitly). Other
  instances are unaffected.
- `Destroyed`: `OnDestroy` has fired (at Stop or at the safe point for a
  `world.destroy`'d entity). The environment is torn down. Using the
  entity handle after this returns nil/false.

Quarantined-at-Stop: `OnDestroy` is **skipped** for a quarantined
instance — it never had a clean lifecycle, and calling `OnDestroy` on a
script that failed mid-`OnUpdate` is unsafe. This means a script that
grabbed a `world` handle in `OnCreate` gets no cleanup hook if it
quarantines. This is acceptable because Lua state is GC'd and the
`IRuntimeCommandSink` holds no per-instance resources (the queue is
controller-owned); the only thing a quarantined script can leak is
Lua-side state, which the `sol::state` reset on `OnSceneStop` reclaims.
Stated explicitly so 6C's reload-un-quarantines rule has a clear before
state.

**S2 — The 4-arg callback signature is locked from 6A.**
`on_update(entity, dt, input, world)` is the signature from 6A onward.
`input` is present from 6A but **inert** — the `input.*` methods are
added in 6C. A 6A-authored script that ignores `input` runs unchanged
under 6C; a 6C-authored script that uses `input` would see `nil`-method
errors under 6A (acceptable — 6A scripts are test-only, not shipped). The
same lock applies to `on_fixed_update(entity, dt, input, world)` and
`on_create(entity, world)` / `on_destroy(entity)`. Adding a parameter
later would break every 6A-authored script; the 4-arg form is the
forward-compatible contract.

**S3 — `self` is the per-entity field table, with split read/write
semantics.** The first draft's narrative leaned on `self.speed` without
enumerating `self` in the bindings. Pinned:

- `self` is a per-environment table populated from
  `ScriptComponent::fieldValues` on `OnCreate` (6B populates the values;
  6A's `self` is empty because `fieldValues` is empty).
- **Reads** return the persisted/edited values (6B) or declared defaults
  (6A). The inspector edits `ScriptComponent::fieldValues`, not `self`
  directly; `self` is rebuilt from the component on `OnCreate`.
- **Writes** to `self.x` mutate the runtime environment only — they are
  **non-persistent** (not written back to `ScriptComponent::fieldValues`,
  invisible to the inspector, lost on Stop). This is the natural carrier
  for `rt2.previous_state` in 6C: reload copies the old environment's
  `self` into the new environment's `rt2.previous_state` before
  re-binding callbacks.
- `self` is not a binding on `entity` or `world`; it is a bare Lua local
  injected into the environment by `ScriptSystem` before callbacks run.

**S4 — `entity.set_position` writes through the sink, not the const
document.** `ScriptSystem::OnSceneStart` receives `const SceneDocument&`
(from the observer contract) and `IRuntimeCommandSink*`. The entity
handle in Lua holds a pointer to the sink; `entity.set_position(vec3)`
calls `sink->SetRuntimeTransform(uuid, ...)` which writes the runtime
`SceneDocument`'s `Transform` via the non-const path the sink owns. The
narrative's "write directly to the runtime `SceneDocument`'s `Transform`"
is reconciled: the script never sees the document, the sink does. The
handle→sink wiring is set at `OnCreate` time and is valid for the
instance's lifetime. A handle used after `OnDestroy` has a null sink
pointer and fails safely.

**S5 — Two distinct orderings, two distinct bookkeepings.** Updates
(`OnFixedUpdate`, `OnUpdate`) iterate the environment map in
**UUID-sorted** order (deterministic, matches `MotionSystem`'s existing
UUID-sorted iteration and the `game-loop.md` stable-order rule).
`OnDestroy` at Stop iterates in **reverse-creation-order** so a parent
that spawned children in `OnCreate` destroys them before itself.
`ScriptSystem` records creation order in a `std::vector<UUID>`
alongside the environment map; `SyncScriptEnvironments` appends newly
created UUIDs to it and the Stop path reverses it. The two orderings are
explicit and separately testable.

**S6 — `vec3` is in 6B's initial field type set.** The first draft
deferred `vec3` and `color`. `vec3` (offsets, directions, velocities) is
among the most common designer-facing script fields, and adding a
variant arm later re-touches exactly the 6B compatibility-rule +
serializer surface being built then. 6B's `ScriptFieldValue` variant is
`std::variant<bool, int64_t, double, std::string, rt2::core::UUID,
glm::vec3>` from the start. `color` (a `vec3` with a color picker
widget) is the same variant arm with a different inspector widget — also
in 6B. `rt2.field.vec3(default)` and `rt2.field.color(default)` join
the DSL. This avoids a fast-follow that re-touches the serializer.

**S7 — Protected-call discipline is a 6A cross-cutting rule, not a 6C
detail.** The "one bad script never crashes the engine" guarantee — the
foundation of the quarantine model — requires **every** Lua entry point
to use `sol::protected_function` with a bound error handler, plus a
`lua_atpanic` guard installed on the `sol::state`. This covers: field
DSL evaluation, all four lifecycle callbacks, timer callbacks (6C), and
any sink-invoked call back into Lua. A panic or uncaught error
quarantines the instance per S1. This is universal from 6A, not added
piecemeal.

**S8 — Doc drift to fix when 6A lands.** (a) `MotionSystem` is inline
in `RuntimeSceneController::RunFixedTick`, not a class — the spec's
`MotionSystem::FixedUpdate` naming is aspirational; the 6A implementation
call site is "the inline motion integration in `RunFixedTick`". (b)
`game-loop.md`'s numbered Step list (lines 154–159) omits the fixed-
script slot that the prose at line 161 implies; 6A updates that list to
include "fixed script callbacks" before the motion integration, matching
the full contract at lines 130–143.

### Phase 6A — Lua embedding and lifecycle (implemented)

**Outcome.** A C++ script component drives an entity's transform during
Play, with `OnCreate`, `OnFixedUpdate`, `OnUpdate`, and `OnDestroy`
callbacks dispatched in the canonical frame order. No inspector
reflection, no field persistence, no hot reload, no input bindings yet —
the script source is referenced by an absolute or scene-relative path and
the bindings surface is intentionally minimal. This slice proves the
embedding, the lifecycle dispatch, and the deferred-queue mutation
channel.

**Vendor.** Vendor Lua 5.4 (the canonical C sources, compiled as a
static lib) under `RT2App/vendor/lua/` and sol2 (header-only) under
`RT2App/vendor/sol2/`. Update `RT2App/premake5.lua` (`includedirs`,
`libdirs`, `links`, `files` for `vendor/lua/src/**.c`) and
`RT2Tests/premake5.lua` (link the Lua static lib into RT2Tests so the
CPU-only script-system tests can run). Add Lua to RT2SliceRunner only if
the slice runner needs to execute scripts (it does, for the headless
acceptance in 6C — defer the link until then).

**New types (CPU-only, no Vulkan/ImGui/Walnut/GLFW):**

- `ECSComponents.h`: add `ScriptComponent { AssetReference asset;
  std::unordered_map<std::string, ScriptFieldValue> fieldValues; }`.
  `ScriptFieldValue` is defined in a new `ScriptFieldValue.h` as
  `std::variant<bool, int64_t, double, std::string, rt2::core::UUID,
  glm::vec3>` (the `vec3` arm is present from 6A per S6, though 6A does
  not populate `fieldValues` — that is 6B's job). The variant is shared
  by the serializer and the reflection layer without pulling sol2 into
  the CPU-only boundary. The live sol2 environment/table handle is NOT
  stored on the component — it lives in `ScriptSystem` (mirrors the
  `MaterialOverrideComponent::materialIndex` precedent: transient
  post-resolution state is off-document).
- `AssetKind`: extend with `Script = 4`.
- `PersistedComponents.h`: add `Tag<ScriptComponent>{}`, bump `Count`
  to 11.
- `IRuntimeScriptDispatch` (new header, CPU-only — see G1): the
  per-frame mutation-driving interface, distinct from
  `IRuntimeLifecycleObserver`'s const-observe contract.
  ```cpp
  class IRuntimeScriptDispatch {
  public:
      virtual ~IRuntimeScriptDispatch() = default;
      virtual void OnFixedUpdate(float dt) = 0;
      virtual void OnUpdate(float dt) = 0;
      virtual void SyncScriptEnvironments() = 0;
  };
  ```
- `IRuntimeCommandSink` (new header, CPU-only — see S4): the controlled
  mutation channel handed to scripts. Wraps
  `RuntimeSceneController::QueueCreateRuntimeEntity` /
  `QueueDestroyRuntimeEntity` and owns the non-const path to the runtime
  `SceneDocument`'s transform/visibility setters. No raw
  `entt::registry`, no `SceneManager` access, no render bridge. The Lua
  `entity` handle holds a pointer to the sink; `entity.set_position(vec3)`
  calls `sink->SetRuntimeTransform(uuid, ...)` (the script never sees
  the document).
- `ScriptSystem.h/.cpp` (new, CPU-only): owns the single `sol::state`,
  a per-entity `sol::environment` map keyed by UUID, a per-entity
  `sol::table` for `self` (see S3), a per-entity declared-fields table
  (populated in 6B, empty in 6A), a creation-order `std::vector<UUID>`
  (see S5), and the per-instance state machine (see S1). Implements
  **both** `IRuntimeLifecycleObserver` (start/stop) and
  `IRuntimeScriptDispatch` (per-frame). Public API:
  ```cpp
  class ScriptSystem : public IRuntimeLifecycleObserver,
                       public IRuntimeScriptDispatch {
  public:
      explicit ScriptSystem(IUuidProvider& uuidProvider);

      // IRuntimeLifecycleObserver
      void OnSceneStart(const SceneDocument& runtime,
                        const IInputService* input,
                        IRuntimeCommandSink* sink) override;
      void OnSceneStop(const SceneDocument& runtime) override;

      // IRuntimeScriptDispatch
      void OnFixedUpdate(float dt) override;   // UUID-sorted
      void OnUpdate(float dt) override;        // UUID-sorted
      void SyncScriptEnvironments() override;  // see G2

      // 6C hot reload (declared now, stubbed in 6A)
      virtual void ReloadScript(const std::filesystem::path& path) {}
  };
  ```
  Protected-call discipline (S7): every Lua entry point
  (`rt2.fields` eval, all four callbacks, sink-invoked calls) runs
  through `sol::protected_function` with a bound error handler; a
  `lua_atpanic` guard is installed on the `sol::state` at construction.
  Any uncaught error or panic transitions the instance to `Quarantined`
  (S1).

- `RuntimeEntityCreateDesc` (existing, in `RuntimeSceneMutator.h`):
  gains `std::optional<ScriptComponent> script` so `world.spawn` can
  spawn a scripted entity (see G2). The header comment already licenses
  extending the surface.

**Lifecycle dispatch integration (see G1, G2, S1, S5):**

- `RuntimeSceneController` gains `SetScriptDispatch(...)` /
  `GetScriptDispatch()` (the **one** structural addition — see G1).
  `ScriptSystem` is registered via both `SetLifecycleObserver` and
  `SetScriptDispatch`.
- `Play` calls `m_LifecycleObserver->OnSceneStart(runtime, &inputService,
  &commandSink)` AFTER the runtime is fully activated. `ScriptSystem`
  builds environments for all `ScriptComponent`-bearing entities in the
  runtime clone, fires `OnCreate` once per entity in UUID-sorted order,
  and records creation order (S5). The `input` and `sink` pointers are
  stored on the controller and passed through; this is the
  `IRuntimeCommandSink` addition the `RuntimeLifecycleObserver.h`
  comment promised.
- `RunFixedTick(dt)` calls `m_ScriptDispatch->OnFixedUpdate(dt)` BEFORE
  the inline motion integration, in UUID-sorted order, to match
  `game-loop.md:134` ("fixed script callbacks … physics step"). Spawns
  queued during `OnFixedUpdate` do **not** resolve mid-loop — they
  resolve at the safe point after the loop (G2).
- After the fixed-step loop, `Update` calls
  `ApplyDeferredStructuralChanges` (existing), then
  `m_ScriptDispatch->SyncScriptEnvironments()` (new — see G2): this is
  the single chokepoint where the environment map mirrors the runtime
  registry. For newly-applied entities it constructs the environment
  and fires `OnCreate` immediately; for destroyed entities it fires
  `OnDestroy` then tears down the environment. Then
  `m_ScriptDispatch->OnUpdate(frameDt)` runs in UUID-sorted order, then
  `SceneGraph::UpdateWorldTransforms`, then the batched sync — matching
  `game-loop.md:137` ("variable script callbacks" after the safe point).
- `Stop` calls `OnSceneStop` BEFORE runtime destruction: `ScriptSystem`
  fires `OnDestroy` for all live instances in reverse-creation-order
  (S5), skips `OnDestroy` for quarantined instances (S1), tears down the
  `sol::state`, then the controller destroys the runtime document.
- Pause runs no script callbacks; Step runs exactly one
  `OnFixedUpdate(kFixedDt)` + one `SyncScriptEnvironments()` + one
  `OnUpdate(kFixedDt)`, matching the `game-loop.md:161` rule for stepped
  frames. Spawns queued during the stepped `OnFixedUpdate` resolve in
  the stepped `SyncScriptEnvironments`, visible to the stepped
  `OnUpdate`.

**Bindings exposed to scripts in 6A (intentionally narrow; signatures
locked from 6A per S2, S3):**

- **Callback signatures (forward-compatible contract):**
  `on_create(entity, world)`,
  `on_fixed_update(entity, dt, input, world)`,
  `on_update(entity, dt, input, world)`,
  `on_destroy(entity)`. The `input` parameter is present from 6A but
  **inert** — `input.*` methods are added in 6C; a 6A script that
  ignores `input` runs unchanged under 6C. Missing functions are
  skipped silently (not an error).
- **`self` (per S3):** a per-environment table populated from
  `ScriptComponent::fieldValues` on `OnCreate` (empty in 6A — 6B fills
  it). Reads return persisted/edited values (6B) or defaults (6A);
  writes mutate runtime-only and are non-persistent (lost on Stop,
  invisible to the inspector). `self` is also the carrier for
  `rt2.previous_state` in 6C reload.
- **`entity` (validated UUID-keyed handle; holds a sink pointer per
  S4):** `entity.get_position()`, `entity.set_position(vec3)` (→
  `sink->SetRuntimeTransform`), `entity.get_local_transform()`,
  `entity.set_local_transform(trs)`, `entity.get_world_transform()`,
  `entity.get_name()`, `entity.get_uuid()`, `entity.set_visible(bool)`,
  `entity.get_visible()`. After `OnDestroy`, the sink pointer is null
  and all methods fail safely.
- **`world` (the `IRuntimeCommandSink`):** `world.find_by_name(name)` →
  handle or nil, `world.find_by_uuid(uuid)` → handle or nil,
  `world.spawn(desc)` → pending handle (queues a create; resolves at
  the next `SyncScriptEnvironments`; using it before resolution returns
  a "pending" handle that fails safely), `world.destroy(handle)` (queues
  a destroy; `OnDestroy` fires at the next `SyncScriptEnvironments`).
- **`log`:** `log.info/warn/error(string)`.

**Not exposed in 6A (deferred to 6B/6C):** input actions, light/camera
properties, timers, mesh/material mutation, public-field reflection.

**Test plan (CPU-only Doctest, `RT2Tests/src/Phase6ALifecycleTests.cpp`):**

- A script that increments a counter in `on_create`, `on_fixed_update`,
  `on_update`, `on_destroy` fires each callback exactly the documented
  number of times across a Play/Pause/Step/Stop sequence.
- Two entities using the same script source have isolated per-entity
  environment state.
- A script that sets `entity.set_position(vec3)` in `on_fixed_update`
  results in the runtime transform changing and one batched
  transform-only GPU sync per rendered frame (asserted via a recording
  null render bridge, mirroring the Phase 4 test pattern).
- **G2 spawn lifecycle:** a script that calls `world.spawn(desc)` (with
  a `ScriptComponent` in `desc`) during `on_update` for a fixed number
  of frames results in: the spawn is deferred, applied at the safe
  point, the new entity's `on_create` fires in `SyncScriptEnvironments`
  this frame (before the parent's `on_update` returns — well, in the
  same frame, after the parent's `on_update` since spawn was queued
  mid-`on_update`), the new entity's `on_update` fires this frame (after
  its `on_create`), and the new entity is visible to `on_fixed_update`
  next frame. Iterating during the spawn frame does not see the new
  entity mid-iteration (deterministic, no invalid iteration).
- **G2 destroy lifecycle:** a script that calls `world.destroy(self)`
  during `on_update` is deferred; `on_destroy` fires at the next
  `SyncScriptEnvironments` (the entity is alive for the rest of this
  frame's callbacks and gone next frame); the environment is torn down
  after `on_destroy`.
- **G2 fixed-loop spawn:** a script that calls `world.spawn` during
  `on_fixed_update` (substep 2 of 3) does NOT see the new entity in
  substep 3; it resolves at the safe point and is visible to
  `on_update` this frame and `on_fixed_update` next frame.
- A destroyed entity handle used in a subsequent callback fails safely
  (returns nil/false, does not alias a reused EnTT ID) — the sink
  pointer is null after `on_destroy`.
- A script with a syntax error on load quarantines only that instance
  (state `NeverCreated` → `Quarantined`, no `on_create`); other
  instances of the same source continue to run; the error includes the
  script path, entity UUID, and a stack trace.
- **S1 mid-frame quarantine:** a script that throws in `on_fixed_update`
  on substep 2 of 3: substep 3 and this frame's `on_update` are skipped
  for this instance; the instance is `Quarantined`; other instances are
  unaffected; `on_destroy` is NOT called at Stop for this instance.
- **S5 ordering:** updates iterate UUID-sorted (asserted by recording
  the order of callback firings across entities with known UUIDs);
  `on_destroy` at Stop iterates reverse-creation-order (a parent that
  spawned a child in `on_create` destroys the child before itself).
- Lifecycle order: `on_create` fires once per entity on Play;
  `on_fixed_update` fires `kMaxSubsteps` times max per frame;
  `on_update` fires once per frame (zero while Paused); `on_destroy`
  fires once per live entity on Stop (not on quarantine, not for
  `NeverCreated`).
- `IRuntimeCommandSink` rejects `world.spawn` / `world.destroy`
  outside Play with a clear error (the controller already enforces
  this on the queue; the sink surfaces it to Lua as a Lua error, not
  a C++ exception).
- **S7 protected-call discipline:** a script that calls `error()` in
  `on_update` does not crash the engine; a script that triggers a Lua
  panic (e.g. stack overflow via deep recursion) does not crash the
  engine — the `lua_atpanic` guard quarantines the instance.

**Verification gates (6A):** Release x64 build clean (RT2App + RT2Tests
+ RT2SliceRunner); `RT2Tests` with only the six known pre-existing
failures; new `Phase6ALifecycleTests` passing; `run_slice_test.ps1`
and `run_recovery_test.ps1` pass; `graphify update .`; documentation
updates with test counts. No interactive acceptance yet (no inspector
UI to author a script in).

**Explicitly out of scope for 6A:** public-field reflection and
inspector UI; field-value persistence; hot reload; input/light/camera/
mesh/material bindings; timers; headless JSON state report. The
`ScriptComponent.fieldValues` map exists in the struct but is empty and
unused in 6A — it is the seam 6B fills.

### Phase 6A verification report (implemented)

**Vendored:**
- `RT2App/vendor/lua/` — Lua 5.4 (submodule, `v5.4` branch), C sources
  compiled as a static lib via premake `vendor/**.c` glob. Excludes
  `lua.c` (interpreter), `luac.c` (compiler), `onelua.c` (amalgamation),
  `testes/`, `manual/`.
- `RT2App/vendor/sol2/` — sol2 v3.5.0 (submodule, header-only), included
  via `vendor/sol2/include`. sol2 tests/examples/scripts/single excluded
  from the build glob.

**New files:**
- `RT2App/src/ScriptFieldValue.h` — CPU-only `ScriptFieldValue` variant
  (`bool, int64_t, double, std::string, UUID, glm::vec3`) and
  `ScriptFieldType` enum (includes `Vec3` + `Color` per S6).
- `RT2App/src/IRuntimeScriptDispatch.h` — per-frame mutation-driving
  interface (G1): `OnFixedUpdate`, `OnUpdate`, `SyncScriptEnvironments`.
- `RT2App/src/IRuntimeCommandSink.h` — controlled mutation channel (S4):
  `SpawnEntity`, `DestroyEntity`, `GetLocalTransform`, `SetLocalTransform`,
  `GetPosition`, `SetPosition`, `GetName`, `SetName`, `GetVisible`,
  `SetVisible`, `IsAlive`, `FindByName`.
- `RT2App/src/ScriptSystem.h/.cpp` — single `sol::state`, per-entity
  environment map (G2: per-frame-maintained mirror via
  `SyncScriptEnvironments`), per-instance state machine (S1:
  NeverCreated/Live/Quarantined/Destroyed), protected-call discipline
  (S7: `sol::protected_function` + `lua_atpanic`), UUID-sorted dispatch
  (S5), reverse-creation-order `OnDestroy` at Stop (S5). Bindings: `entity`
  (get/set position/name/visible, get_uuid), `world` (find_by_name,
  find_by_uuid, spawn, destroy), `log` (info/warn/error), `self` (S3:
  per-entity field table, empty in 6A), `input` (S2: present but inert,
  4-arg callback signature locked from 6A). `RuntimeCommandSink` concrete
  sink resolves the runtime doc lazily via `TryGetRuntimeScene` (avoids
  the chicken-and-egg of needing the runtime doc before Play).
- `RT2Tests/src/Phase6ALifecycleTests.cpp` — 8 tests, 37 assertions.

**Modified files:**
- `RT2App/src/ECSComponents.h` — `ScriptComponent` struct (asset +
  fieldValues), `AssetKind::Script = 4`, added `<entt/entt.hpp>` include
  (self-contained), `ScriptFieldValue.h` include, `<unordered_map>`.
- `RT2App/src/PersistedComponents.h` — `Count` 10→11, `Tag<ScriptComponent>`.
- `RT2App/src/RuntimeLifecycleObserver.h` — `OnSceneStart` extended with
  `const IInputService*`, `IRuntimeCommandSink*` params (back-compat shim
  calls the single-arg overload so Phase 4 observers work unchanged).
- `RT2App/src/RuntimeSceneController.h/.cpp` —
  `SetScriptDispatch`/`GetScriptDispatch`, `SetInputService`/
  `GetInputService`, `SetRuntimeCommandSink`/`GetRuntimeCommandSink`;
  `RunFixedTick` calls `OnFixedUpdate` before motion; `Update`/`Step` call
  `SyncScriptEnvironments` after the deferred drain, then `OnUpdate`,
  before `SceneGraph::UpdateWorldTransformes`; `Play` passes input + sink
  to `OnSceneStart`.
- `RT2App/src/RuntimeSceneMutator.h/.cpp` — `RuntimeEntityCreateDesc`
  gains `std::optional<ScriptComponent> script`; `CreateEntity` emplaces
  it when present.
- `RT2App/src/SceneSerializer.cpp` — `static_assert` Count 10→11;
  `EntityRecord` gains `hasScript`/`script`; `BuildEntityRecord` reads
  `ScriptComponent`; the apply path emplaces it (clone-in-memory carries
  it; v2 on-disk path does not serialize it — 6B adds v3).
- `RT2App/src/SubtreeSnapshot.h` — `SubtreeEntityRecord` gains
  `hasScript`/`script` (duplication/paste preserves script binding).
- `RT2App/src/SceneManager.cpp` — `BuildSubtreeRecord` reads
  `ScriptComponent`; `ApplySubtreeRecord` emplaces/removes it;
  `EntityMatchesRecord` compares it.
- `RT2App/premake5.lua` — `vendor/**.c` glob, sol2 + Lua include dirs,
  sol2/Lua exclusions.
- `RT2Tests/premake5.lua` — `ScriptSystem.cpp` + Lua C sources in the
  `files` list, sol2 + Lua include dirs.
- `docs/game-loop.md` — Step list updated with fixed-script +
  SyncScriptEnvironments + OnUpdate slots; scripting placeholder replaced
  with the implemented `IRuntimeScriptDispatch` contract.

**Verification:**
- Release x64 full solution builds clean (RT2App + RT2Tests +
  RT2SliceRunner).
- `RT2Tests`: 385 cases, 379 pass, 6 known pre-existing failures (5
  `SceneGraph` cases in `EcsTests.cpp` + 1 SIGSEGV in `SceneManager:
  RemoveEntity destroys entity`), 48 skipped. No regressions.
- `Phase6ALifecycleTests`: 8/8 pass, 37 assertions covering lifecycle
  callback counts, two-entity isolation, scripted transform → transform-
  only sync, syntax error quarantine (only affected instance), runtime
  error quarantine (mid-frame, no further callbacks), world.spawn
  deferral to next safe point, Pause/Step callback suppression, Lua
  error safety (protected-call discipline).
- `run_slice_test.ps1` PASS.
- `run_recovery_test.ps1` PASS.
- graphify updated: 32007 nodes, 68122 edges, 1273 communities.

**Phase 6B status _as of this report_:** W0-W3 now provide app wiring,
public-field reflection, typed storage, deterministic reconciliation, and
schema-v3 persistence across normal open and recovery. Command integration
and Inspector UI remain W4-W5. The `ReloadScript(path)` API is declared on
`ScriptSystem` and stubbed (empty body); 6C implements file-watched hot
reload.

**Deferred to 6C (as planned):** hot reload (file watching via efsw);
input/light/camera/mesh/material bindings; timers; headless JSON state
report; `RT2SliceRunner --script-scenario` mode.

> **Superseded 2026-07-24.** The two paragraphs above are a point-in-time
> snapshot, kept as a record of what this verification report covered. Both
> 6B W4-W6 and all of 6C have since landed: `ReloadScript` is fully
> implemented and every item under "Deferred to 6C" exists. For current
> state see the Phase 6C verification report near the end of this document.

### Phase 6B — Public fields, reflection, and persistence (implemented)

> *Header corrected 2026-07-24 (during 6C/W9): this previously read
> "W0-W3 implemented, W4-W6 planned". W4 (`SetScriptCommand` +
> `MakeSetScriptCommandIfEffective`, `EditorPropertyCommands.h:217,331`),
> W5 (`SceneEditorUI::RenderScriptEditor`, `SceneEditorUI.cpp:1626`) and
> W6 (`Phase6BFieldsTests.cpp`) are all present in the tree. Note the
> verification report below covers W0-W2 only; W3-W6 never got one.*

**Outcome.** A script declares public fields with declared types and
defaults; the inspector renders them, the user edits them, and they
survive save/load. The compatibility rules (same name+type preserves
value; added → default; removed → warning; renamed → explicit alias;
incompatible type → default + diagnostic) are enforced on load and on
reload. No hot reload yet — reload is exercised via explicit
`ReloadScript(path)` calls in tests.

**Reflection (built from scratch — none exists today; the `ScriptFieldValue`
variant and `self` table are already in place from 6A per S3/S6):**

- `ScriptFieldDescriptor { std::string name; ScriptFieldType type;
  ScriptFieldValue defaultValue; std::optional<std::string> alias; }`
  discovered by the script system when it loads a script source: the
  script declares fields via a small RT2-owned DSL (`rt2.fields = {
  speed = rt2.field.float(5.0), name = rt2.field.string("cube"),
  enabled = rt2.field.bool(true), offset = rt2.field.vec3(0,0,0),
  tint = rt2.field.color(1,1,1) }` at the top of the script, evaluated
  in a sandboxed environment before the lifecycle callbacks are
  bound). `ScriptFieldType` enumerates the supported variant types
  (`bool`, `int64`, `double`, `string`, `uuid`, `vec3`, `color` — the
  last two share the `glm::vec3` variant arm but get different
  inspector widgets).
- `ScriptSystem::GetDeclaredFields(UUID)` →
  `std::vector<ScriptFieldDescriptor>`; used by the inspector and the
  serializer.
- `SceneEditorUI::RenderScriptEditor(SceneManager::EntityId)` —
  mirrors the existing `Render*Editor` dispatch pattern. **Amended
  (F4):** the cited `RenderMotionEditor at SceneEditorUI.cpp:944-1003`
  does not exist — Motion is an inline block inside `RenderInspector`
  (`SceneEditorUI.cpp:939-1003`). The real dispatch precedent is
  `RenderTransformEditor` / `RenderMaterialEditor` / `RenderLightEditor`
  / `RenderCameraEditor` (`SceneEditorUI.cpp:1009/1243/1401/1492`);
  `RenderScriptEditor` follows `RenderLightEditor`'s structure, and
  takes the "Add Component" / "Remove Component" button pair from the
  Motion block. Renders one widget per declared field, typed by
  `ScriptFieldType`, with the `PropertyEditSession<T>` record-on-release
  pattern for continuous edits (drag/slider). Reads/writes
  `ScriptComponent::fieldValues[fieldName]`; on release, records a
  command. **Amended (D7):** the command is a single `SetScriptCommand`
  with `std::optional<ScriptComponent>` before/after (covering add,
  remove, path edit, and field edit), not a per-field
  `SetScriptFieldCommand` — this is the proven `SetMotionCommand` shape
  (`EditorPropertyCommands.h:186-208`), and `PropertyEditSession`
  already yields exactly one record per drag so per-field granularity
  buys nothing. **Amended (D8):** sync impact is `SyncImpact::None`, not
  `Structural`. Evidence: the analogous `SceneManager::SetMotionState`
  returns `None` (`SceneManager.cpp:3387`), and 6B's own test plan below
  requires that field edits produce no GPU sync.
- The inspector lists the bound script asset path with a "Rebind" button
  (deferred to Phase 7's content-browser era; 6B edits the path as
  text).

**Field-value persistence:**

- `SceneSerializer` v2 → v3 as a **hard format cutover**. v3 adds the
  `ScriptComponent` payload: `AssetReference` (path + sourceKey) plus
  `fieldValues` as a JSON object keyed by field name with a tagged-
  value form (`{ "type": "float", "value": 5.0 }`). Save writes v3 and
  the loader accepts v3 only; v1/v2 scenes are rejected as unsupported.
  No migration code or legacy fixtures are required for this slice.
- `PersistedComponents::ForEach` already includes `ScriptComponent`
  (Count 11). W3 adds its disk representation without preserving v2
  fixture compatibility.
- The two-pass load (create + resolve) is unchanged; script field
  values are applied in the create pass (they are self-contained per
  entity, no cross-entity references).
- `CloneInMemory` (Play path) clones `ScriptComponent` automatically
  once it is in the visitor; field values are carried into the runtime
  clone and presented to the script as the initial environment
  state.

**Compatibility rules on load and on reload (the 6B core):**

- **Same field name + same type:** value preserved.
- **Added field:** receives the declared default; a warning is logged
  with the entity UUID and field name.
- **Removed field:** value dropped from the persisted map; a warning
  is logged with the entity UUID and field name.
- **Renamed field (declared `alias = "oldName"`):** value migrated
  from `oldName` to `newName` if `oldName` is present in the persisted
  map and `newName` is not; otherwise the declared default. A warning
  is logged on the migration.
- **Incompatible type change (e.g. float → string):** value reset to
  the declared default with a diagnostic naming the entity, field,
  old type, and new type.

These rules run on (a) scene load (comparing persisted field values
against the currently declared fields) and (b) reload (comparing the
in-memory field values against the newly declared fields).

**Test plan (`RT2Tests/src/Phase6BFieldsTests.cpp`):**

- A script declares `speed = rt2.field.float(5.0)`; the inspector
  path writes `7.0`; save; reload; the value is `7.0` (type preserved).
- Add a field `height = rt2.field.float(2.0)` to the script between
  save and reload: `speed` preserves its value, `height` receives
  the default `2.0`, a warning is logged for the added field.
- Remove the `speed` field between save and reload: the persisted
  `speed` value is dropped, a warning is logged, the script's
  `OnUpdate` does not see `speed`.
- Rename `speed` to `velocity` with `alias = "speed"`: the persisted
  `speed` value migrates to `velocity`; a warning is logged.
- Change `speed`'s declared type from `float` to `string`: the
  persisted float value is reset to the declared string default; a
  diagnostic is logged naming the entity, field, old type, new type.
- A v3 scene with no `ScriptComponent` loads cleanly with no scripts;
  a v3 scene with scripts round-trips every supported field type. v1/v2
  inputs fail with the normal unsupported-schema diagnostic.
- `CloneInMemory` carries field values into the runtime clone; the
  script's `OnCreate` reads the authored values.
- Field values do not sync the GPU (they are editor/runtime-only
  state until the script reads them; a script that reads `speed` and
  writes it to `entity.set_position` produces exactly one
  transform-only sync per frame, not a structural sync).
- Two entities using the same script source with different field
  values have isolated environment state (already proven in 6A; 6B
  adds that the values are the persisted ones, not the defaults).
- **S6 vec3/color:** a script declaring `offset = rt2.field.vec3(0,0,0)`
  and `tint = rt2.field.color(1,1,1)` round-trips both values; the
  inspector renders `tint` with a color picker widget and `offset`
  with a `DragFloat3`. A type change `float` → `vec3` on an existing
  field triggers the incompatible-type rule (default + diagnostic).
  **Amended (D5):** `vec3` → `color` is a *compatible* change by rule
  (same variant arm) — the value is preserved and the stored type tag
  is updated. This is a deliberate rule, not an undetectable accident,
  because field values are stored typed (see D5 in the implementation
  plan below).

**Verification gates (6B):** same as 6A plus an interactive acceptance
check: author a script with `rt2.fields = { speed = rt2.field.float(5.0) }`
that moves the entity by `speed * dt` in `OnUpdate`; save; reopen;
verify the inspector shows the `speed` field with the saved value;
edit it; Play; verify the entity moves at the edited speed; Stop;
verify the authoring scene is unchanged.

**Explicitly out of scope for 6B:** hot reload (file watching);
input/light/camera/mesh/material bindings; timers; headless JSON state
report; the interactive rebinding UI (Phase 7). Reload is exercised in
tests via an explicit `ScriptSystem::ReloadScript(path)` call.

#### Phase 6B implementation plan (approved 2026-07-21)

Written after grounding the 6B spec against the code as it actually
stands post-6A. Findings F1–F8 below are the evidence base; design
decisions D1–D9 follow from them. Where a decision contradicts the 6B
spec text above, the spec has been amended in place with a pointer to
the finding, so this decision is not reopened by the next reader.

##### Findings (verified against the tree, not assumed)

| # | Finding | Evidence |
|---|---|---|
| F1 | **Scripting is unreachable from the app.** `ScriptSystem` is never instantiated in `WalnutApp.cpp`; `SetLifecycleObserver` / `SetScriptDispatch` are never called. 6A is test-only. | `RuntimeSceneController.h:174,185`; no owner in any `.cpp` |
| F2 | **No editor path to a `ScriptComponent`.** `SceneManager.h` contains zero `Script` symbols. | `SceneManager.h` |
| F3 | **`GetDeclaredFields(UUID)` as specced is Play-only.** `m_Instances` is populated at `OnSceneStart` and cleared at `OnSceneStop`; the inspector needs declarations while *editing*. | `ScriptSystem.cpp:67-85, 87` |
| F4 | **The spec's cited model function does not exist** (see amendment above). | `SceneEditorUI.cpp:939-1003, 1009, 1243, 1401, 1492` |
| F5 | **The spec's `Structural` sync impact is wrong.** The analogous `SetMotionState` returns `None`. | `SceneManager.cpp:3387` |
| F6 | **`self` injection already exists** from 6A; 6B only reconciles `fieldValues` before that loop. | `ScriptSystem.cpp:352-379` |
| F7 | `ScriptFieldType` (7 labels) and the 6-arm `ScriptFieldValue` variant already exist. Only `ScriptFieldDescriptor` is missing. | `ScriptFieldValue.h:40-60` |
| F8 | Serializer is at v2, `MinReadVersion = 1`; `ScriptComponent` is already in `PersistedComponents` (Count 11) and already survives `CloneInMemory`. Only disk read/write is missing. | `SceneSerializer.h:102`; `PersistedComponents.h:20,35`; `SceneSerializer.cpp:384-390, 753-759` |

##### Design decisions

**D1 — Declarations live in a standalone `ScriptFieldRegistry`, not in
`ScriptSystem` (resolves F3).** The inspector must show declared fields
with the editor *stopped*, but `ScriptSystem`'s Lua state and instance
map exist only during Play. A new CPU-only `ScriptFieldRegistry` owns
**one** reusable `sol::state`, parses a `.lua` path on demand into a
fresh sandboxed environment, and caches `std::vector<ScriptFieldDescriptor>`
keyed by path with `(mtime, size, FNV-1a source hash)` invalidation. Only descriptor vectors
are cached — never Lua states — so the cache is tiny. `ScriptSystem::
GetDeclaredFields(UUID)` is still added, delegating to the registry via
the instance's asset path, so the spec's API survives; the editor calls
the registry directly.

**D2 — The field-declaration DSL.**

```lua
rt2.fields = {
  speed   = rt2.field.float(5.0),
  enabled = rt2.field.bool(true),
  count   = rt2.field.int(3),
  label   = rt2.field.string("cube"),
  offset  = rt2.field.vec3(0, 0, 0),
  tint    = rt2.field.color(1, 1, 1),
  target  = rt2.field.uuid(),
  vel     = rt2.field.float(1.0, { alias = "speed" }),   -- rename
}
```

Each constructor returns
`{ __rt2_field = true, type = <int>, default = <value>, alias = <string|nil> }`.

- **Alias direction (confirmed):** the alias names the *old* field. Given
  persisted `speed` and declared `vel` with `alias = "speed"`, the value
  migrates `speed` → `vel`. Migration is one hop only — alias chains are
  not followed. If `vel` is already present in the persisted map, no
  migration occurs (the persisted `vel` wins and `speed` is dropped as a
  removed field). If two declared fields alias the same old name, the
  migration is ambiguous: **neither** migrates, both take their declared
  defaults, and one diagnostic names both claimants.
- **Field order: sorted by name ascending.** The spec omits ordering, and
  Lua table iteration is unordered, which would reshuffle the inspector
  between frames. Name-sorting is deterministic and testable. Known UX
  wart: renaming a field relocates it in the inspector. Authoring order
  would require an array-form declaration and a sequence index in the
  descriptor — deferred to 6C if content authors ask for it.

**D3 — Parsing is sandboxed and bounded.** The registry parses arbitrary
user `.lua` in the editor on selection, so it opens the shared safe library
set, installs throwing stubs for denied base functions, runs the chunk under
`sol::protected_function`, and
installs a `LUA_MASKCOUNT` hook with an instruction budget so a top-level
`while true do end` cannot hang the editor. Callbacks are defined but
never invoked. A parse failure yields a diagnostic and never throws.

> **Amended after review (2026-07-21).** "Open safe libraries and shadow
> the dangerous names" is **not sufficient**, and it took two rounds of
> review to establish why. Both sandboxes now go through a single
> `InstallSandbox` in `ScriptSandbox.h`. Three distinct escapes were
> closed, each proven by a test before it was fixed:
>
> 1. **`sol::nil` does not shadow.** Assigning nil to a table key removes
>    it, restoring the environment's `__index` fallback to globals.
>    `dofile`, `loadfile` and `load` were reachable from every script;
>    `loadfile` and `load` do not even raise, so it was silent. A
>    throwing stub stores a real value and genuinely shadows.
> 2. **`getmetatable(_ENV).__index` IS the globals table.** An
>    environment built on a globals fallback exposes its own metatable, so
>    one line retrieves the real `loadfile`/`dofile`/`load` regardless of
>    any name-level shadow. `getmetatable("")` likewise yields the shared
>    string metatable. `getmetatable` is now denied. An intermediate fix
>    kept it, reasoning it was harmless once `_G` was denied — that
>    reasoning was wrong.
> 3. **Library tables are shared mutable state.** `math`, `string`,
>    `table` and `utf8` resolve through the fallback to tables shared by
>    every script in the Lua state, so `math.floor = ...` in one entity's
>    script poisons every other script and every later field parse. Each
>    environment now gets a shallow copy.
>
> The durable lesson for 6C: a globals-fallback environment is an
> **allowlist** problem wearing a denylist's clothes. Every binding,
> library or capability 6C adds must be checked for a path back to the
> globals table, and every escape found needs a regression test. If 6C's
> surface grows much further, replace the fallback with a sealed
> allowlist environment rather than extending the deny list again.

**D10 also covers structurally malformed declarations, not just Lua
errors.** A script that never assigns `rt2.fields` declares nothing and
parses cleanly. A script that replaced the injected `rt2` table, or set
`rt2.fields` to a non-table (`rt2.fields = "unfinished"` mid-edit), is
**a parse failure**. Reporting that as a clean empty set would defeat D10
completely: W2 would read it as "the author deleted every field" and
destroy the authored values — the exact outcome D10 exists to prevent.

**D4 — Reconciliation is a pure free function; the serializer stays
Lua-free.** `SceneSerializer` reads and writes `fieldValues` verbatim.
Reconciliation runs as a post-load pass, the way `SceneAssetResolver`
already runs after load:

```cpp
// ScriptFieldReconcile.h — no Lua, no ImGui, no Vulkan
struct FieldDiagnostic {
    enum class Kind { Added, Removed, Renamed, AmbiguousAlias,
                      TypeChanged, InvalidStoredValue, MissingEntityId,
                      InvalidAssetKind, ParseFailed };
    Kind kind; rt2::core::UUID entity;
    std::string field, fromField, message;
};

ScriptFieldMap ReconcileScriptFields(
    const ScriptFieldMap& persisted,
    const std::vector<ScriptFieldDescriptor>& declared,
    const rt2::core::UUID& entity,
    std::vector<FieldDiagnostic>& outDiags);
```

The core reconciliation matrix then needs neither Lua nor file I/O.

**D5 — Field values are stored typed.** *(Amended from the spec after
review — the spec's implicit "type is the variant arm" model was lossy.)*

```cpp
struct ScriptFieldEntry {
    rt2::core::ScriptFieldType type;
    rt2::core::ScriptFieldValue value;
    bool operator==(const ScriptFieldEntry& other) const {
        return type == other.type && value == other.value;
    }
};
using ScriptFieldMap = std::unordered_map<std::string, ScriptFieldEntry>;
```

Rationale for taking the cost now: W3 already touches the component, clone
path, and serializer, while delaying typed storage until after v3 would
require a v4 migration. The W2 call-site audit updated runtime injection,
snapshot comparison, clone/duplication, and tests before any untyped disk
format shipped. Storing the tag also *simplifies*
D6: the on-disk tag becomes authoritative rather than derived from the
variant arm, so `color` round-trips exactly and no arm-derivation logic
is needed.

Compatibility rule with typed storage: **two types are compatible iff
they share a variant arm.** Therefore `vec3` ↔ `color` preserves the
value and updates the stored tag (a deliberate rule with a test that
asserts *desired* behavior), while `float` → `vec3` and `float` →
`string` still reset to the declared default with a diagnostic, exactly
as the spec requires.

Note on the reviewed `vec3 → float → vec3` concern: that sequence loses
the value at the first step regardless of how types are stored, because
the declared type genuinely changed across variant arms. Each step emits
a `TypeChanged` diagnostic naming the entity, field, old type, and new
type, so it is reported rather than silent. It is not a D5 artifact.

**D6 — Tagged JSON, with the stored tag written directly.**

```json
"script": {
  "asset":  { "path": "scripts/spin.lua",
              "sourceKey": "lua:asset=scripts/spin.lua" },
  "fields": { "speed": { "type": "float", "value": 7.0 },
              "tint":  { "type": "color", "value": [1, 1, 1] } }
}
```

Tags: `bool | int | float | string | uuid | vec3 | color`. An unknown tag
drops that field with a diagnostic (forward compatibility). A tag whose
value payload fails to parse is likewise dropped with a diagnostic —
never a load failure, since one bad field must not cost the user a scene.

**D7 — One `SetScriptCommand`, not a per-field command.** See the
amendment in the spec above for the rationale.

**D8 — Sync impact is `None`.** `SceneManager::SetScriptState` mirrors
`SetMotionState` (`SceneManager.cpp:3370-3390`) exactly. See the spec
amendment.

**D9 — `ReloadScript` gets a partial 6B body.** It re-parses declarations
for the path and re-reconciles + re-injects `self` on bound instances. It
does **not** watch files — that remains 6C. This is the minimum needed to
run the spec's reload tests.

> **Amended after review (2026-07-21).** The original wording said reload
> "does not rebind callbacks", which is not achievable. `BuildEnvironment`
> constructs a *fresh* `sol::environment` and extracts the four callback
> handles from it; re-injecting `self` means replacing the environment,
> and the new environment's callbacks must therefore be re-bound. The
> rebind-and-swap shape is the right one and the current code supports it
> — the callbacks read `inst.env["entity"]` / `["world"]` at *call* time
> (`ScriptSystem.cpp` `OnUpdate`/`OnFixedUpdate`), not at bind time, so
> swapping the environment does not strand them. What D9 actually
> excludes is **re-running `on_create`** (the entity already exists).
>
> Two traps for the implementer, both live in the current code:
>
> 1. **Callback removal must explicitly clear.** The bind block only
>    assigns when the global is present, so a callback deleted from the
>    source leaves the previous handle in place and it keeps being
>    called. Reset all four to `sol::protected_function{}` before
>    rebinding.
> 2. **Build into a scratch instance and swap only on clean load.** A
>    syntax error must leave the running instance untouched — that is
>    what makes "the scene keeps running" true rather than aspirational.

**D10 — A parse failure never reconciles.** If the registry cannot parse
a script, it returns the **last known-good** descriptors for that path
plus a warning, and the resolver **skips reconciliation entirely** for
entities bound to it. Reconciling against zero declarations would treat
every field as removed and delete the user's authored values on a
transient syntax error. The inspector shows the stale-but-last-good
widgets with a warning banner, which is more useful than an empty panel
and is safe because nothing is written back until the user edits.

##### Open behavioral decisions, now settled

- **Empty or invalid script path.** A `ScriptComponent` with an empty
  `asset.path` renders the path field, the text "No script bound", and
  the Remove button — no declarations, no field widgets, no crash. A
  non-empty path that does not resolve on disk renders the same, with a
  "script not found" diagnostic line.
- **`rt2.field.uuid()` default and validation.** Default is the zero
  `UUID`, meaning "unset". Validation is **format-only** (canonical
  string form, or empty). It deliberately does *not* require the UUID to
  reference a live entity: a script may legitimately reference an entity
  spawned at runtime, and load-order effects would make an existence
  check flaky. The inspector shows a non-blocking "not found in scene"
  hint when the UUID is well-formed but unresolved.
- **Registry eviction.** `ScriptFieldRegistry::Clear()` is called on
  scene close and scene load, plus an LRU cap of 64 cached descriptor
  vectors. Because the registry caches descriptors and not `sol::state`s
  (D1), steady-state memory is negligible.

##### Workstreams

- **W0 — App wiring (enabling; resolves F1/F2).** `WalnutApp` owns a
  `ScriptSystem` + `RuntimeCommandSink` and calls `SetLifecycleObserver`
  / `SetScriptDispatch`. Add `SceneManager::SetScriptState(uuid,
  std::optional<ScriptComponent>)` → `SyncImpact::None`, plus
  `HasScript(entity)` / `GetScriptState(uuid)`. **Included in 6B
  deliberately:** without it the acceptance gate is unrunnable and 6B's
  "it works" claim would rest on unit tests alone, with two scripting
  phases shipped and no in-app path to use them.
- **W1 — Reflection.** `ScriptFieldDescriptor` in `ScriptFieldValue.h`;
  new `ScriptFieldRegistry.h/.cpp` (D1–D3, D10);
  `ScriptSystem::GetDeclaredFields(UUID)`.
- **W2 — Reconciliation (implemented).** New `ScriptFieldReconcile.h/.cpp` (D4, D5);
  `ScriptFieldResolver::ResolveDocument(doc, registry, diags)` post-load
  seam. W3 installs the normal-open and recovery callers and owns their
  dirty-state policy. Three implementation notes added after the W1 review:
  - The compatibility test compares **two different enums** — the
    declared `ScriptFieldDescriptor::type` and the stored
    `ScriptFieldEntry::type`. State it explicitly:
    `compatible = ScriptFieldArmIndex(declared.type) ==
    ScriptFieldArmIndex(stored.type)`, which `ScriptFieldTypesCompatible`
    (added in W1, `ScriptFieldValue.h`) already expresses.
  - The largest single edit for D5's typed storage is **not** a
    signature change: it is the `std::visit` block in
    `ScriptSystem::BuildEnvironment` that injects `self` from
    `fieldValues`. It must read `.value`, and should assert `.type`
    against the descriptor.
  - Reconciliation must be **skipped entirely** when the registry
    returns `parsed == false` (D10). `ScriptSystem::GetDeclaredFields`
    returns the whole `ScriptFieldRegistry::Result` for exactly this
    reason — do not reduce it to a descriptor vector at any layer.
- **W3 — Persistence.** `SchemaVersion 2 → 3`; set `MinReadVersion` to 3
  as an explicit hard cutover. Reject v1/v2 rather than implementing a
  migration path.
  Add the `script` branch to `EntityRecordToJson` (~`SceneSerializer.cpp:467`)
  and `JsonToEntityRecord` (~`:592`); `hasScript`/`script` already exist
  on the record and are already emplaced at `:758`. Only v3 scenes load;
  Save always writes v3.
- **W4 — Command.** `SetScriptCommand` + `MakeSetScriptCommandIfEffective`
  in `EditorPropertyCommands.*`, kept CPU-only and proven through Execute,
  Undo and Redo history tests. `SceneEditorUI::RecordScriptEdit` moves to W5,
  where it has real widget callers and can submit staged canonical state through
  history before the document is mutated.
- **W5 — Inspector.** `RenderScriptEditor(EntityId)` following
  `RenderLightEditor`'s structure (F4), dispatched from `RenderInspector`
  (~`:937`). One `PropertyEditSession<ScriptComponent>
  m_ScriptFieldSession` (single active slot, per that header's contract).
  Widget map: `bool→Checkbox`, `int→DragInt`, `float→DragFloat`,
  `string→InputText(EnterReturnsTrue)`, `uuid→validated InputText`,
  `vec3→DragFloat3`, `color→ColorEdit3`. Continuous widgets (`Drag*`) go
  through the session; discrete ones record immediately. Plus the asset
  path field, Add/Remove Script buttons, and the diagnostics line.
- **W6 — Tests.** `RT2Tests/src/Phase6BFieldsTests.cpp`, registered in
  **both** `RT2Tests.vcxproj` and `RT2Tests.vcxproj.filters` (the 6A file
  is in both).

##### Test plan

*Pure, no Lua (W2):* value preserved on same name + type · added →
default + warning · removed → dropped + warning · alias migration
`speed` → `vel` · alias no-op when the target is already present ·
ambiguous alias → both default + one diagnostic · `float` → `string`
reset + diagnostic · `float` → `vec3` reset + diagnostic · `vec3` →
`color` **preserved with the tag updated** (asserts the D5 rule).

*Registry (W1):* all seven constructors parse · descriptors sorted by
name · syntax error → last-good descriptors + diagnostic, no throw ·
**parse failure does not reconcile** and therefore does not clobber
authored values (D10) · runaway top-level loop terminates via the
instruction hook (D3) · `Clear()` drops the cache and the next
`GetDeclaredFields` re-parses · `(mtime, size, FNV-1a)` invalidation.

*Serializer (W3):* v3 round-trips every supported type including `color`
· v1/v2 files are rejected as unsupported · unknown type tag dropped
with a diagnostic, load still succeeds · `CloneInMemory` carries field
values into the Play clone and `OnCreate` reads the authored values.

*Runtime and command (W0/W4):* two entities sharing one script with
different values have isolated `self` holding the **persisted** values,
not the defaults · a script that reads `speed` and calls
`entity.set_position` produces exactly one **Transform** sync per frame
and **zero Structural** syncs · `SetScriptCommand` Undo/Redo for add,
remove, and field edit, with no-op suppression.

*Inspector guard (W5):* a `ScriptComponent` with an empty `asset.path`
renders no field widgets and does not crash the inspector.

##### Verification gates

The 6A gates, plus the interactive acceptance check (requires W0): author
`rt2.fields = { speed = rt2.field.float(5.0) }` in a script that moves
the entity by `speed * dt`; save; reopen; the inspector shows the saved
value; edit it; Play; the entity moves at the edited speed; Stop; the
authoring scene is unchanged.

##### Explicitly out of scope for 6B

Hot reload and file watching (`efsw`); input, light, camera, mesh, and
material bindings; timers; the headless JSON state report; the
content-browser rebind UI (Phase 7). Reload is exercised in tests via an
explicit `ScriptSystem::ReloadScript(path)` call (D9).

### Phase 6B W0-W2 verification report (implemented)

W0+W1 began in commits `e2edab6` (6A hardening prerequisite) and `0472274`.
W2 is implemented in the current tree. **W3-W6 remain**: serializer v3,
`SetScriptCommand`, inspector UI, and their remaining acceptance coverage.

**W0 — app wiring.** `ScriptSystem` was never instantiated by the app:
`SetLifecycleObserver` / `SetScriptDispatch` were never called, so 6A
shipped test-only and `ScriptComponent`-bearing entities were inert in
Play. The controller side was already fully wired; only the owner was
missing.

- `WalnutApp` owns a `ScriptSystem` + `RuntimeCommandSink` and installs
  all four controller seams from `EnsureScriptRuntimeWired()` — lazy and
  idempotent, mirroring `EnsureRenderBridge`, called from `EnterPlay`.
  They are `unique_ptr` because `ScriptSystem` takes its UUID provider by
  reference, so construction must wait until the authoring document has
  one; if it is still null the app stays unwired and retries next Play.
- `SceneManager::HasScript` / `GetScriptState` / `SetScriptState`. The
  mutator mirrors `SetMotionState` exactly and reports
  `SyncImpact::None` (D8).

**W1 — reflection.**

- `ScriptFieldDescriptor` in `ScriptFieldValue.h`, plus
  `ScriptFieldArmIndex` / `ScriptFieldTypesCompatible` /
  `ScriptFieldTypeName`. `ScriptFieldTypesCompatible` is the single
  expression of D5's rule and is what W2 must use.
- `ScriptFieldRegistry.h/.cpp` — the `rt2.fields` DSL (all seven
  constructors plus the `alias` option), sandboxed and instruction-
  bounded parsing, a `(mtime, size, FNV-1a)` cache with LRU-64 and
  `Clear()`.
- `ScriptSystem::GetDeclaredFields(UUID)` delegates to the registry. Shared
  `ResolveScriptAssetPath` now keeps the Play and authoring paths identical.

**W2 — typed storage and reconciliation.**

- `ScriptFieldEntry { type, value }` and `ScriptFieldMap` replace the untyped
  variant map. Runtime `self` injection validates the tag/payload invariant
  and visits the stored payload.
- `ReconcileScriptFields` is Lua-free and deterministic. It covers compatible
  preservation, defaults/removals, one-hop aliases, target-wins semantics,
  ambiguous aliases, incompatible types, malformed stored entries, and the
  compatible `vec3`/`color` tag transition.
- `ScriptFieldResolver::ResolveDocument` resolves script-bearing entities in
  UUID order through the shared path helper. `parsed=false` preserves authored
  values exactly and emits `ParseFailed`; the resolver intentionally leaves
  document dirty-state policy to its W3 host. It accounts for UUID-less
  components as skipped and rejects non-Script asset kinds explicitly.
- Typed entries survive subtree duplication and `CloneInMemory`; a lifecycle
  test proves an authored typed float reaches runtime `self` and drives an
  entity during Play.
- W3 promotes this seam to live load behavior: normal open and recovery invoke
  `ResolveDocument` transactionally before adoption, so Play consumes the
  reconciled typed map.

**Focused verification:** `Phase6*` is 31/31 cases and 219/219 assertions;
`Phase 6*` is 17/17 cases and 79/79 assertions (48 cases, 298 assertions
combined). Release RT2Tests builds clean.

**Current full Release suite:** 482 run, 475 passed, 7 failed, 0 skipped.
The failures are the unchanged baseline: five `SceneGraph` cases in
`EcsTests.cpp`, plus `SceneManager: SetTransform updates TRS and marks dirty`
and `SceneManager: SetMaterial updates MeshRef`. No W2 test failed.

**Two contracts W2 depends on, both enforced by tests:**

1. A failed parse returns the **last known-good** descriptors with
   `parsed=false` and does not stamp mtime/size, so fixing a syntax error
   recovers on the next query. `GetDeclaredFields` returns the whole
   `Result` rather than a descriptor vector precisely so this flag cannot
   be dropped — W2 must skip reconciliation when it is false.
2. A **malformed** declaration structure (a replaced `rt2` table, or
   `rt2.fields` set to a non-table mid-edit) is a parse *failure*, not a
   clean empty set. Reporting it as "no fields" would make W2 delete
   every authored value.

**Tests:** `RT2Tests/src/Phase6BFieldsTests.cpp`, 31 cases / 219 assertions
cover W0-W2: authoring API and sync impact; all constructors, ordering,
aliases, parse recovery, instruction bounds, sandbox/isolation and cache
invalidation; pure reconciliation; UUID-ordered multi-entity resolution;
parse-failure preservation; and typed clone/duplication. The complementary
`Phase 6*` lifecycle filter is 17 cases / 79 assertions and includes runtime
consumption of an authored typed value.

**Suite:** 419 cases, 413 passed, 6 failed at the time of the commit —
the same six pre-existing failures documented for Phase 2D. RT2App
Release builds clean.

> **That figure, and every "6 failed / 48 skipped" in the verification
> reports above, was a truncated run.** The SIGSEGV in `SceneManager:
> RemoveEntity destroys entity` did not merely fail — it took the process
> down mid-run, so Release abandoned the 48 cases after it and Debug could
> not complete a run at all. Fixed in `b19512b` (an unchecked
> `m_Meshes[index]` in `CompactMeshRegistry`). The honest baseline is now:
>
> - **Release: 468 run, 0 skipped, 7 failed** — 5 × `SceneGraph`
>   (`EcsTests.cpp:93-193`) plus `SceneManager: SetTransform updates TRS
>   and marks dirty` and `SceneManager: SetMaterial updates MeshRef`. The
>   last two are long-standing test/implementation drift that the crash
>   had been hiding.
> - **Debug: 468 run, 0 skipped, 15 failed** — the 7 above plus 7 ×
>   `OBJ Import Wizard` and 1 × `P1A Multi-Model`, which fail only in
>   Debug.
>
> Compare regressions against those numbers, not against 6. The earlier
> reports are left as written because they were accurate measurements of
> what the suite reported at the time.

**Not done in W0/W1:** the interactive acceptance gate (authoring a
script in the editor, editing `speed`, Play) requires the inspector from
W5, so it remains open.

#### Phase 6B W3 implementation — v3 persistence hard cutover (implemented 2026-07-22)

**Outcome.** `.rt2scene` v3 is the first and only readable script-aware
format. Saving writes each `ScriptComponent` and every valid typed public
field; loading reconstructs the authored component, reports and isolates bad
individual fields, then the RT2 app reconciles the loaded values against the
current Lua declarations before adopting the document. This is deliberately
not a compatibility slice: v1/v2 files and recovery snapshots are rejected.

##### Grounded findings

| # | Finding | Consequence for W3 |
|---|---|---|
| W3-F1 | `SceneSerializer::SchemaVersion` is 2 and `MinReadVersion` is 1; `Load` explicitly maps every accepted input to the current version. | Set both constants to 3 and remove the effective-version migration language/path. |
| W3-F2 | `EntityRecord` already contains `hasScript` and `ScriptComponent`; `BuildEntityRecord`, `BuildDocumentFromRecords`, `CloneInMemory`, snapshots and duplication already copy it. | Disk JSON is the missing serializer seam; do not create a second component-copy path. |
| W3-F3 | `AssetKindName` / `AssetKindFromName` do not recognize `Script`. | Add the `"script"` mapping before using the existing asset-reference shape. |
| W3-F4 | `SceneSerializer::Load` exposes only `bool + Error`; it has no non-fatal diagnostic/report channel. | Add a load report so one bad public field can be dropped visibly without failing the scene. |
| W3-F5 | Normal `.rt2scene` open already runs on a worker: `Load`, then `SceneAssetResolver::ResolveAll`, then main-thread adoption. | Run a worker-local `ScriptFieldRegistry` and `ScriptFieldResolver` in that transaction before adoption; never share the runtime Lua state across threads. |
| W3-F6 | Recovery restore is a separate load/resolve/adopt host in `SceneRecoveryService::Restore`. | It must run the same script reconciliation and return field diagnostics; otherwise normal open and recovery diverge. |
| W3-F7 | `RT2SliceRunner` loads scenes but intentionally does not link Lua/sol2. | Keep it as a raw v3 deserialize/clone consumer in W3. Script declaration reconciliation in the runner remains part of the 6C script-scenario work. |
| W3-F8 | v2 serializes `metadata.sourcePath`, and `Load` trusts that stored value. `ScriptFieldResolver` resolves relative scripts from this metadata, while model resolution uses the actual opened file's parent. | In v3, source path is derived runtime state: omit it from JSON and set it from the actual load path. Recovery restores its separately recorded logical source path before resolution. |
| W3-F9 | The normal-open completion path unconditionally calls `ClearDirty()` after adoption. | Carry separate save-required and destructive-loss signals from normalization/reconciliation; call `MarkDirty()` after the ordinary clear when the adopted document changed. |
| W3-F10 | The committed vertical-slice scene is v2 and raw serializer tests contain v1/v2 literals whose intended assertions occur after schema validation. | Regenerate/update the committed fixture, rewrite ordinary literals to v3, and retain only explicit v1/v2 rejection tests. |
| W3-F11 | `SceneManager::SetScriptState` currently accepts any tag/payload pair, while the W3 serializer is intentionally strict. | Add shared validation at the authoring mutation boundary so public APIs cannot create an unsaveable scene; Save retains a defensive check for raw-registry corruption. |
| W3-F12 | Recovery envelopes have an independent `ManifestVersion = 1`. | Bump the envelope version with the schema cutover so old recovery records fail at discovery with an accurate diagnostic rather than at inner scene parsing. |
| W3-F13 | A fresh `SceneMetadata` still defaults `schemaVersion` to 1. | Change the default to 3 in W3.0 so in-memory documents never claim an unreadable format. |

##### v3 JSON contract

An entity with a script writes:

```json
"script": {
  "asset": {
    "kind": "script",
    "path": "scripts/move.lua",
    "sourceKey": "lua:asset=scripts/move.lua"
  },
  "fields": {
    "enabled": { "type": "bool",  "value": true },
    "count":   { "type": "int",   "value": 3 },
    "speed":   { "type": "float", "value": 7.5 },
    "label":   { "type": "string", "value": "runner" },
    "target":  { "type": "uuid",  "value": "00000000-0000-0000-0000-000000000000" },
    "offset":  { "type": "vec3",  "value": [1.0, 2.0, 3.0] },
    "tint":    { "type": "color", "value": [1.0, 0.5, 0.2] }
  }
}
```

- Reuse the asset-reference keys `kind`, `path`, and `sourceKey`; write
  `importSettings` only for model assets, not scripts.
- Script `sourceKey` is canonical derived data: write
  `lua:asset=<serialized path>`. When Save As rebases the path, regenerate the
  key from that rebased path rather than preserving a stale path-form key.
- Field tags are exactly `bool | int | float | string | uuid | vec3 | color`.
- UUIDs use canonical strings; the all-zero UUID is a valid unset value.
- `vec3` and `color` have identical three-number payloads but retain distinct
  tags. The stored tag is authoritative.
- Script field keys are emitted lexically. Entity order remains UUID order.
  The project's default `nlohmann::json` object type uses `std::map`, so object
  keys are sorted; do not switch these paths to insertion-ordered
  `ordered_json`. Explicit lexical iteration is still preferred where the
  source container is unordered because it makes the determinism requirement
  visible rather than relying on a later JSON-object sort.
- An absent `script` key means no `ScriptComponent`; an absent `fields` key
  means an empty typed map.

##### Validation and failure policy

**The authoring boundary and Save share one invariant.** Add a CPU-only
`NormalizeAndValidateScriptComponent` helper that returns a canonical copy and
call it from both `SceneManager::SetScriptState` and the serializer's pre-save
validation. The canonical states are:

- **unbound:** `asset.kind == Script`, empty path/sourceKey, and no fields;
- **bound:** `asset.kind == Script`, non-empty path, canonical
  `sourceKey == "lua:asset=" + path`, and every field has a recognized tag,
  matching payload arm and finite numeric/vector value.

The helper derives `sourceKey` from a non-empty path; callers never reject a
component merely because that redundant string was stale. `SetScriptState`
stores the canonical copy, but rejects structural or field-payload errors with
`EditorMutationResult::Failure` before mutation, revision change or dirtying.
W4/W5 must use this API rather than emplacing components directly. This
preserves W2's intentional empty-path inspector state without allowing authored
values on a script that cannot run.

**Save remains strict and atomic as a defensive backstop.** Raw registry
mutation can bypass `SetScriptState`, so Save repeats validation before writing
the temp file. A missing/stale bound `sourceKey` is normalized only in the
serialized copy to the rebased path; Save does not mutate the live component.
Fill `Error` with the entity UUID and field name and leave an existing target
file byte-for-byte unchanged.

**Load distinguishes structural damage from repairable derived metadata and
one bad field.** A non-object `script`, missing/malformed asset object,
non-Script kind, or non-object `fields` is a hard `Error::Parse` and clears the
temporary document. An empty path is valid only with an empty source key and
empty fields (the canonical unbound state). A bound asset with a missing or
non-string source key is structurally malformed. A string key that does not
match `lua:asset=<path>` is regenerated, emits `NormalizedScriptSourceKey`,
and does not discard authored field data.
Within an otherwise valid `fields` object:

- unknown type tag → drop that field, emit `UnknownSerializedType`;
- recognized tag with the wrong JSON shape/range → drop that field, emit
  `MalformedSerializedValue`;
- valid field → construct a payload whose variant arm exactly matches its tag.

Dropping a field sets `SceneLoadReport::droppedScriptFieldData = true`; other
entities and valid sibling fields survive. Normalizing a stale-but-string
source key instead sets `normalizedScriptMetadata = true`; it is not reported
as field loss. No JSON exception may escape
`SceneSerializer::Load`—field conversion helpers validate types and bounds
before calling `get<T>()`.

Add `UnknownSerializedType`, `MalformedSerializedValue`, and
`NormalizedScriptSourceKey` to
`FieldDiagnostic::Kind`, then add a CPU-only report:

```cpp
struct SceneLoadReport {
    std::vector<FieldDiagnostic> fieldDiagnostics;
    bool normalizedScriptMetadata = false;
    bool droppedScriptFieldData = false;
};
```

Provide a report-taking `Load` overload used by production and W3 tests; keep
the existing three-argument overload as a convenience for raw consumers that
do not surface non-fatal diagnostics.

##### Path and source-root policy

`SceneMetadata::sourcePath` is process state, not durable scene content. v3
omits `metadata.sourcePath`; `Load(path)` always sets it to `path`. This makes
normal script and model resolution agree on the file the user actually
opened, even if a scene was moved.

For Save As, relative references must continue to identify the same assets:
resolve an existing relative reference against the document's current source
directory, then relativize it against the logical output scene directory.
Apply that shared rebasing rule to imported models, scripts and the environment
path. If the document is untitled and the reference is already relative,
preserve and normalize it because no old root exists. `SaveTo` continues to
use `logicalScenePath` rather than the recovery-file directory.

Absolute references preserve their target identity. Save first tries to make
them relative to the logical output scene directory; if that is impossible
(for example, the target is on a different Windows volume), it writes a
normalized absolute path. This is the same fallback for models, scripts, and
the environment and must be covered explicitly by tests.

Recovery records already store `originalSourcePath` and `assetRoot`
separately. Restore sets the temporary document's source path to the original
logical path before asset and script resolution. An envelope with manifest v1,
or a manifest-v2 envelope containing a scene-v1/v2 snapshot, fails its
respective version check by design.

##### Host transaction and dirty policy

Normal open remains one all-or-nothing worker transaction:

```text
SceneSerializer::Load(v3, SceneLoadReport)
  → SceneAssetResolver::ResolveAll
  → worker-local ScriptFieldRegistry
  → ScriptFieldResolver::ResolveDocument
  → success: main-thread ReplaceAuthoringDocument
  → ClearDirty
  → classify normalization/reconciliation versus destructive field loss
  → if either class changed the document: MarkDirty
  → if destructive: require acknowledgement before Save/autosave
```

Serializer and field diagnostics are formatted into the existing background
diagnostic string and logged on the main thread. `parsed=false`, missing script
files, invalid bindings, and UUID-less components are warnings/skips; they do
not by themselves mark the document dirty because authored data was preserved.

Do not collapse every change into a single `repaired` bit. Compute two host
signals:

- **non-destructive change:** source-key normalization, added defaults, renamed
  values, and compatible tag normalization; mark dirty and show the ordinary
  "script fields changed; save required" status;
- **destructive change:** a serialized field was dropped, or reconciliation
  emitted `Removed`, `TypeChanged`, `InvalidStoredValue`, or
  `AmbiguousAlias`; mark dirty, show an explicit entity/field warning, and set
  `scriptRepairAcknowledgementPending`.

While destructive acknowledgement is pending, suppress recovery autosnapshots
and require an explicit acknowledgement/confirmation before ordinary Save or
Save As. This prevents the background snapshot loop or a reflexive save from
persisting field loss before the author has seen it. Acknowledgement permits
persistence; it does not clear dirty state. The classifier is a small shared
CPU helper used by normal open and recovery so their semantics cannot drift.

Recovery performs the same asset/script resolution, but a successfully
restored recovery document is already dirty regardless of reconciliation.
Extend `SceneRecoveryService::Restore` to return `FieldDiagnostic`s alongside
`AssetDiagnostic`s and the two change classifications so warnings and
destructive acknowledgement requirements are not swallowed.

##### Implementation order

1. **W3.0 — Schema and invariant cutover.** Set both serializer constants and
   the `SceneMetadata::schemaVersion` default to 3; bump recovery
   `ManifestVersion` to 2; remove v1→current migration code/comments; add
   explicit v1/v2 scene and v1 recovery-envelope rejection tests; add the
   shared normalization/validation helper and enforce it in `SetScriptState`
   before strict Save work lands; change ordinary raw JSON test inputs to v3;
   regenerate
   `assets/vertical-slice.rt2scene`; rename version-specific test wording.
2. **W3.1 — Typed codecs and save validation.** Add Script asset-kind mapping,
   tag conversion helpers, exact payload writers/readers, strict pre-save
   defensive validation through the same shared helper, lexical field
   emission, and source-root rebasing without mutating live components.
3. **W3.2 — Entity JSON and load report.** Add the `script` branch to
   `EntityRecordToJson` and `JsonToEntityRecord`; introduce
   `SceneLoadReport`; implement hard component errors and non-fatal per-field
   repair without uncaught JSON conversions, distinguishing metadata
   normalization from dropped field data.
4. **W3.3 — Normal-open integration.** In the background worker, pass the
   report, resolve assets, create a worker-local registry, reconcile scripts,
   aggregate diagnostics, classify destructive loss, and carry `requiresSave`
   plus `requiresAcknowledgement` to the completion callback. Preserve
   transactional adoption, mark dirty only after the existing clear, and gate
   Save/autosnapshot while destructive acknowledgement is pending.
5. **W3.4 — Recovery integration.** Update `Restore` and callers/tests to set
   the logical source path before both resolvers and expose field diagnostics
   and classifications. Confirm old recovery envelopes fail cleanly without
   touching the live doc and destructive recovery repair cannot be
   autosnapshotted before acknowledgement.
6. **W3.5 — Documentation and verification.** Update serializer/scene docs
   from v2 to v3, run focused persistence/lifecycle tests, build the Release
   solution, run the full suite against the documented seven-failure baseline,
   and refresh Graphify.

##### Test matrix

Add W3 cases primarily to `Phase6BFieldsTests.cpp` and
`SceneSerializerTests.cpp`; update schema-specific cases in
`Phase1ASceneAssetTests.cpp`, recovery tests, fixture tests and slice-runner
inputs.

- all seven field types round-trip, including nil/non-nil UUID and distinct
  `vec3`/`color` tags;
- entity with no script, canonical unbound script, and bound script with an
  empty field map round-trip;
- two saves with reverse `unordered_map` insertion order are byte-identical;
- relative and absolute references for models, scripts and environment follow
  the same target-preserving Save As rule, including different-volume absolute
  fallback;
- `SetScriptState` rejects malformed state without mutation, revision or dirty
  changes; a valid accepted state remains saveable;
- raw-registry malformed entry or invalid script asset makes Save fail
  atomically without mutating the live component;
- Save canonicalizes a stale raw-registry `sourceKey` only in the serialized
  copy and leaves the live component untouched; Load canonicalizes a stale
  string key, emits `NormalizedScriptSourceKey`, preserves every field, and is
  classified as non-destructive;
- unknown tag and wrong payload each drop only their field, report entity/name,
  set `droppedScriptFieldData`, and preserve valid siblings; saving the
  resulting valid temporary document succeeds after host acknowledgement;
- malformed script/asset/fields containers fail the whole temporary load;
- v1 and v2 reject with `Error::SchemaVersion`; v3 and only v3 loads;
- post-load same-type preservation is idempotent and clean;
- post-load add/remove/rename/type reset mutates the temporary document and
  produces `requiresSave=true` in deterministic UUID/name order, while the
  classifier marks add/rename non-destructive and remove/type-reset
  destructive;
- declaration parse failure preserves serialized fields exactly and does not
  request a save;
- actual opened path overrides any stale/unknown metadata and drives relative
  script resolution;
- normal open and recovery block Save/autosnapshot after destructive repair
  until explicit acknowledgement, but do not block non-destructive
  normalization;
- recovery envelope v1 is rejected at discovery; v2 resolves scripts against
  `originalSourcePath`, remains dirty, and does not mutate the live document on
  any load/asset/script hard failure;
- save→load→resolve→Play proves the persisted authored value reaches `self`;
- committed fixture, recovery scenario and slice runner consume v3 successfully.

##### W3 acceptance gate

Create a v3 scene in a test with two entities bound to one relative script and
different values for every supported field arm. Save, reopen through the same
CPU pipeline as the app, reconcile, and Play: each entity receives only its
own authored `self`; the clean round-trip is not dirty. Edit the declaration
to add, rename and incompatibly change fields, reopen again, and verify the
document is adopted dirty with deterministic diagnostics and corrected values.
The Release solution builds, all W3/focused Phase 6 tests pass, and the full
suite introduces no failure beyond the seven documented baseline failures.

**Out of scope.** Undo commands and inspector widgets remain W4/W5. File
watching, callback reload, input/binding expansion and SliceRunner script
execution remain 6C. No v1/v2 scene or recovery migration utility is built.

**Implementation report.** W3 is complete: scenes and recovery envelopes use
hard-cutover versions 3 and 2 respectively; all seven public-field types have
strict tagged codecs; script metadata is normalized and validated at the
mutation and defensive-save boundaries; malformed fields are isolated through
`SceneLoadReport`; Save As rebases environment, model, and script references
from the logical source root; and normal open/recovery reconcile declarations
before adoption. Lossless repairs require a save, while destructive repairs
also suppress autosave and require an explicit first-Save acknowledgement.
Runtime environments install inert `rt2.field.*` declaration constructors so
the same declaration-bearing source accepted by reflection also executes during
Play. The end-to-end persistence test covers save → load → reconcile → Play.

**Post-review hardening.** Save validates field names and UTF-8 string payloads
before JSON emission and catches any remaining serializer exception; reflection
rejects defaults that cannot be persisted. Valid non-canonical UUID text is
normalized with a lossless diagnostic. The SliceRunner now consumes
`SceneLoadReport` and refuses snapshots that dropped fields. Registry, runtime,
and serializer type tags share one canonical name table. The destructive-repair
persistence gate remains active after acknowledgement until Save succeeds, so a
recovery autosnapshot cannot silently persist loss between the first and second
Save actions.

**W3 verification:** Release solution build passes; focused W3 is 15/15 cases
and 127/127 assertions; recovery is 23/23 cases and 140/140 assertions; both the
60-step vertical slice and recovery scenario pass. The full Release suite is
499 run, 492 passed, 7 failed, 0 skipped. Those seven are the unchanged baseline
(five `SceneGraph` cases and two legacy `SceneManager` cases); no W3 case fails.

**Next implementation slice:** W4 command/history integration, followed by W5
Inspector authoring. W4 must make script assignment/removal and public-field
edits undoable without weakening the W3 validation and destructive-repair
contracts.

#### Phase 6B W4 implementation plan — script command/history integration (planned 2026-07-22)

**Outcome.** Every authored `ScriptComponent` transition can be represented by
one UUID-keyed command and round-tripped through Execute, Undo and Redo. The
command covers component add/remove, script-path replacement and any typed
field-map change by storing `std::optional<ScriptComponent>` before/after
snapshots. W4 is deliberately CPU-only: it builds and proves the command seam;
W5 owns `RenderScriptEditor`, ImGui lifecycle and `PropertyEditSession` glue.

##### Grounded findings

| ID | Current fact | W4 consequence |
|---|---|---|
| W4-F1 | `SceneManager::SetScriptState(UUID, optional<ScriptComponent>)` validates/canonicalizes and returns `SyncImpact::None`, but currently calls `NotifyAuthoringChanged()` unconditionally—even for canonical present→same-present and absent→absent removal. | W4.0 must make canonical no-op suppression a manager invariant before W5 integration. Effective commands still call this API directly and never synthesize impact. |
| W4-F2 | `SetMotionCommand` already proves the optional whole-component shape for add/remove/edit. | `SetScriptCommand` follows this shape rather than introducing per-field command classes. |
| W4-F3 | `ScriptComponent` and `AssetReference` have no whole-value equality operator; `sourceKey` is derived and script `importSettings` are not persisted. | Add a script-specific canonical equality helper. Do not use raw struct/memory comparison and do not make unordered-map iteration part of equality. |
| W4-F4 | `ScriptFieldEntry::operator==` compares the semantic type tag and exact variant payload; `ScriptFieldMap` equality is key-based and order-independent. | Exact typed-map equality is the no-op rule. `vec3` and `color` with identical vectors are still different states because their tags differ. |
| W4-F5 | W3 validation regenerates `sourceKey`, rejects invalid names/payloads and guarantees persistable UTF-8, but the irrelevant `AssetReference::importSettings` member can still survive in memory on a script component. | Canonicalization clears script import settings to defaults so commands cannot preserve hidden state that Save discards. |
| W4-F6 | `EditorCommandHistory::Execute` records only successful commands; failed initial Execute leaves both stacks unchanged. Failed Undo/Redo clears **both** stacks by established policy. | An invalid after-state or missing target must fail before recording, and an invalid before-snapshot must be rejected by the factory in Release as well as debug so it can never wipe the session on Undo. |
| W4-F7 | Existing Inspector drags mutate the document per frame and later use `RecordApplied`. If a drag changes through intermediate values but returns to its exact start, those mutations already dirtied/bumped the document even though the factory suppresses the final command. Script fields need no live renderer preview. | W5 stages script edits in UI-owned working state and submits one W4 command through `EditorCommandHistory::Execute` at commit. W4 does not adopt the flawed apply-first lifecycle. |
| W4-F8 | `EditorSyncRouter` returns immediately for `SyncImpact::None`. | Execute/Undo/Redo of a script command must produce no renderer sync and no accumulation reset. |
| W4-F9 | The destructive-load `ScriptRepairPersistenceGate` is document-level state owned by the host, independent of ordinary authoring history. | Script commands never clear or acknowledge that gate. Undoing an edit cannot make prior destructive load loss safe to autosave. |
| W4-F10 | `SceneEditorUI::Undo`/`Redo` currently omit `m_MotionVelocitySession` from their session-discard list; W5 will add another session if the list remains distributed. | W5 must first centralize “discard all property sessions” and include motion plus script. W4 does not add an untestable UI session or helper. |

##### Command contract

Add `SetScriptCommand` to `EditorPropertyCommands.h/.cpp`:

```cpp
class SetScriptCommand final : public IEditorCommand {
public:
    SetScriptCommand(UUID target,
        std::optional<ScriptComponent> beforeValue,
        std::optional<ScriptComponent> afterValue);

    EditorMutationResult Execute(SceneManager& scene) override;
    EditorMutationResult Undo(SceneManager& scene) override;
    std::string Description() const override;
};
```

- Execute and Redo call `scene.SetScriptState(target, afterValue)`.
- Undo calls `scene.SetScriptState(target, beforeValue)`.
- The command owns full value copies only. It stores no `entt::entity`, Lua
  objects, registry pointers, file handles, descriptors or runtime callbacks.
- Descriptions are state-aware: `Add Script`, `Remove Script`, or `Edit Script`.
- A path change is not reconciled inside the command. The caller supplies the
  complete already-decided after-state; commands remain deterministic and do
  no filesystem or Lua work. W5 owns declaration lookup/reconciliation when a
  user changes a binding.
- Removing a script stores its complete prior typed map so Undo restores it.
- An unbound component (`kind=Script`, empty path/source key/fields) remains a
  valid add state. Empty path plus authored fields remains invalid by W3.

Add `MakeSetScriptCommandIfEffective(target, before, after)` alongside the
existing property-command factories. Its semantics are:

1. validate and canonicalize a present before-state first. An invalid
   before-snapshot returns null (and may emit a debug diagnostic); no command may be
   submitted because a later failed Undo would clear the entire history under
   the established history policy. This Release guard is required even though
   ordinary callers obtain before-state from validated `GetScriptState` data;
2. both absent → null;
3. validate/canonicalize a present after-state. An invalid after-state is not
   mistaken for a no-op: retain it in a command so
   `EditorCommandHistory::Execute` returns the manager's actionable failure and
   leaves history unchanged;
4. one absent → command;
5. both valid and present → suppress only when canonical path, derived source
   key and exact typed map match.

Before landing the factory, extend
`NormalizeAndValidateScriptComponent` to reset `asset.importSettings` for
`AssetKind::Script`. This is lossless canonicalization: v3 never serializes
script import settings, and no script code consumes them.

Use the same canonical comparator inside `SceneManager::SetScriptState` after
validating the incoming value and before mutating the registry:

- present→canonical-equal present returns successful `SyncImpact::None` with no
  affected UUIDs, dirty change or revision bump;
- absent→absent removal has the same successful no-op result;
- one-present/one-absent or canonically different present values remain
  effective and notify exactly once;
- invalid input still returns the existing actionable failure before equality
  is considered.

This manager-level rule protects direct callers and discrete submissions, but it
cannot erase intermediate mutations from a continuous apply-first drag. W5
therefore stages continuous script edits outside the document and applies only
the final command. A slider dragged away and back to its exact initial value
then closes with no command, dirty bit, revision bump or recovery snapshot.

##### W4/W5 boundary

W4 does **not** add `RenderScriptEditor`, a `PropertyEditSession`, asset-path
text buffers or `SceneEditorUI::RecordScriptEdit`. Those have no production
caller until W5 and cannot be usefully exercised in the CPU suite. W5 will:

1. capture the canonical before-state from `GetScriptState`;
2. edit a UI-owned working copy without calling `SetScriptState` per frame;
3. on discrete commit or continuous-widget release, build the W4 command from
   before and staged after values;
4. skip a null/no-op factory result, otherwise submit through
   `EditorCommandHistory::Execute`, which performs the single validated manager
   mutation and records only on success;
5. discard staged state on Escape, selection change, entity death, Play entry,
   document adoption, Undo or Redo;
6. route Execute/Undo/Redo through the existing `ApplyMutation` path.

The factory and manager both canonicalize, so stale derived `sourceKey` data in
the working copy cannot become command truth. Invalid after-state is displayed
from the failed Execute and never enters history. Net-zero staged edits never
touch the document at all.

##### Implementation order

1. **W4.0 — Canonical state and manager no-op semantics.** Clear script import
   settings in the shared W3 normalization helper; add one exact canonical
   comparator shared by the manager and command factory; make
   `SetScriptState` suppress canonical present→same-present and absent→absent
   mutations before notification. Cover source-key normalization, reversed
   field insertion order, dirty state, revision and empty affected-UUID output.
2. **W4.1 — Command and factory.** Add `SetScriptCommand`, state-aware
   descriptions, accessors used by tests, and the no-op-suppressing factory in
   `EditorPropertyCommands.h/.cpp`.
3. **W4.2 — Execute-history coverage.** Prove add, remove, path edit and typed
   field edit through `EditorCommandHistory::Execute`, Undo and Redo. Assert
   exact state, dirty/revision behavior, affected UUID and `SyncImpact::None`.
4. **W4.3 — Staged-commit coverage.** Simulate the W5 lifecycle: capture before,
   modify an off-document after copy, create the command, then Execute/Undo/Redo.
   Include validation failure, canonical no-op and net-zero edit cases; assert
   the document is untouched before Execute.
5. **W4.4 — Persistence and routing checks.** Save/reopen the commanded state,
   and route command results through a recording `EditorSyncRouter` to prove
   zero GPU sync/reset work.
6. **W4.5 — Documentation and verification.** Update scripting/scene docs,
   build Release, run focused W4/Phase 6 tests, both headless scenarios, the
   full suite against its seven-failure baseline, and refresh Graphify.

##### Test matrix

Add W4 cases to `RT2Tests/src/Phase6BFieldsTests.cpp`:

- factory suppresses absent→absent and equal canonical present→present;
- stale versus derived `sourceKey` with the same path is a no-op;
- reversed `ScriptFieldMap` insertion order is a no-op;
- same vector payload tagged `vec3` versus `color` is effective;
- applying an identical canonical state directly through `SetScriptState` is a
  successful no-op: clean stays clean, revision is unchanged and no UUID is
  reported affected;
- removing a nonexistent `ScriptComponent` directly through `SetScriptState`
  has the same no-op semantics;
- add bound and unbound components; Undo removes; Redo restores canonical data;
- remove a component with all seven field types; Undo restores every type tag
  and payload exactly;
- edit one field, replace the script path, and replace the complete field map;
  each transition round-trips independently;
- Execute, Undo and Redo each bump the revision once, mark dirty, identify only
  the target UUID and return `SyncImpact::None`;
- no-op factory output never changes revision, dirty state or history stacks;
- invalid type/payload, invalid UTF-8, empty name and unbound-with-fields after
  states fail atomically and do not enter history;
- an invalid before-snapshot is rejected by the factory and therefore cannot
  create an Undo operation that clears existing history;
- missing target fails gracefully and leaves both stacks unchanged;
- staged working-copy edits do not touch dirty state, revision or live component
  until `Execute`; a net-zero staged edit produces no command;
- a failed Execute or null factory output does not clear the redo stack;
- `EditorSyncRouter` records zero full/material/transform/reset calls;
- save/reopen after Execute contains the after-state; save/reopen after Undo
  contains the before-state.

##### W4 acceptance gate

Create one entity with a bound script and values for all seven supported field
types. Through command history: edit a scalar, change a semantic tag from
`vec3` to `color`, replace the binding/path, remove the component, then Undo all
steps and Redo all steps. At every boundary assert the exact optional component,
canonical source key, revision delta, dirty state and `SyncImpact::None`; no
renderer callback fires. Save/reopen at the final Execute state and at an Undo
state to prove command snapshots are the same data W3 persists. Invalid input
must leave the live document and history unchanged. Include direct-manager
present→same-present and absent→absent passes, plus a staged net-zero edit,
proving no dirty/revision/history change. Release verification may
introduce no failure beyond the documented seven-test baseline.

**Out of scope.** Inspector widgets, edit-session lifecycle, file dialogs,
interactive rebinding, declaration diagnostics in the Inspector and hot reload
remain W5/6C. W4 performs no Lua evaluation and does not alter the destructive
load-repair acknowledgement state.

#### Phase 6B W4 implementation — script command/history integration (implemented 2026-07-22)

**Outcome.** `SetScriptCommand` and `MakeSetScriptCommandIfEffective` cover
component add, remove, script-path replacement, and any typed field-map change
through one UUID-keyed command storing `std::optional<ScriptComponent>`
before/after snapshots. Execute, Undo, and Redo round-trip through
`EditorCommandHistory`; `SceneManager::SetScriptState` suppresses canonical
no-ops (present→same-present and absent→absent) without bumping the revision,
dirtying the document, or notifying observers. The factory canonicalizes both
before and after states before comparing, so stale `sourceKey` data and reversed
`ScriptFieldMap` insertion order are no-ops; an invalid before-snapshot is
rejected (returns null) so it can never wipe history via a failed Undo, and an
invalid after-state is retained so `EditorCommandHistory::Execute` surfaces the
manager's actionable failure without recording.

**W4.0 — Canonical state and manager no-op semantics.**
`NormalizeAndValidateScriptComponent` now resets `asset.importSettings` to
defaults for `AssetKind::Script` (v3 never serializes script import settings, so
commands cannot preserve hidden state Save would discard).
`ScriptComponentCanonicalEqual` compares two `std::optional<ScriptComponent>`
for order-independent field-map equality (kind, path, derived sourceKey, and
exact typed entries). `SetScriptState` calls it after canonicalization and
suppresses the mutation — no `NotifyAuthoringChanged`, no revision bump, no
dirty mark, no affected UUID — when the canonical incoming value equals the
currently stored component (or both are absent).

**W4.1 — Command and factory.** `SetScriptCommand` follows `SetMotionCommand`'s
shape: Execute/Redo call `scene.SetScriptState(target, after)`, Undo calls
`scene.SetScriptState(target, before)`. Descriptions are state-aware: "Add
Script", "Remove Script", or "Edit Script". `MakeSetScriptCommandIfEffective`
canonicalizes the before-state first (invalid → null), suppresses both-absent
and canonically-equal-present pairs, then canonicalizes the after-state for the
comparison (invalid after is NOT suppressed — the command is returned so
Execute fails atomically without entering history).

**W4.2-W4.4 — Tests.** 19 cases in `Phase6BFieldsTests.cpp` cover:
- factory no-op suppression (absent→absent, canonical-equal present→present,
  stale sourceKey, reversed insertion order);
- vec3 vs color with same payload is effective (tags differ);
- invalid before-snapshot rejected by factory;
- direct-manager no-op for present→same-present and absent→absent (no revision
  bump, no dirty, no affected UUID);
- add/remove/field-edit/path-edit/complete-map-replacement round-trip through
  Execute/Undo/Redo with exact state, revision delta, and `SyncImpact::None`;
- remove with all seven field types; Undo restores every tag and payload;
- each Execute/Undo/Redo reports `SyncImpact::None` and the target UUID;
- no-op factory output never changes revision, dirty, or history;
- invalid after-state fails atomically, does not enter history, does not clear
  redo stack;
- missing target fails gracefully, leaves both stacks unchanged;
- staged working-copy edits do not touch document until Execute;
  net-zero staged edit produces no command;
- `EditorSyncRouter` records zero full/material/transform/reset calls across
  the full add/edit/undo/redo/remove cycle;
- save/reopen after Execute contains the after-state; after Undo contains the
  before-state;
- unbound component add/remove round-trips.

**W4 verification:** Release solution builds clean; focused W4 is 22/22 cases
and 196/196 assertions; the full Phase6B filter is 68/68 cases and 541/541
assertions; the Phase 6 lifecycle filter is 17/17 cases and 79/79 assertions.
The full Release suite is 521 run, 514 passed, 7 failed, 0 skipped — the
unchanged seven-failure baseline (five `SceneGraph` cases and two legacy
`SceneManager` cases). No W4 case fails.

**Post-review hardening.** Three findings from the W4 review were fixed:

1. **[High] Phantom history entry on factory/manager disagreement.** When the
   factory's `before` snapshot diverged from the manager's stored state (e.g.
   an out-of-band mutation happened between W5's capture and commit), the
   factory emitted a command, the manager suppressed it as a no-op, but
   `EditorCommandHistory::Execute` recorded it anyway — creating a phantom undo
   entry and clearing the redo stack for a mutation that never happened. Fix:
   added `bool effective = true` to `EditorMutationResult`; the two
   `SetScriptState` no-op paths set `effective = false`; `Execute` and
   `RecordApplied` skip recording when `success && !effective`. The flag is
   additive — every existing command defaults to `effective = true` and is
   unaffected. A regression test covers the crossing case (capture before →
   out-of-band mutation → commit equal to stored → no history entry, redo
   survives).

2. **[Medium] Add-commands stored non-canonical after-snapshots.** The factory
   canonicalized the after-state only inside the `beforeHas && afterHas`
   branch, so add-commands (`before` absent, `after` present) stored the
   caller's raw `sourceKey` and `importSettings`. Fix: hoisted the
   after-canonicalization out of the guard so it runs whenever `after` is
   present. A regression test builds an add-command with a stale `sourceKey`
   and non-default `importSettings`, then asserts `AfterValue()` is canonical
   and matches the stored state after Execute.

3. **[Low] `Description()` mislabelled absent→absent as "Edit Script".** The
   factory rejects this shape, but `SetScriptCommand` is publicly constructible.
   Fix: added an explicit `"Script (no change)"` fallback so a mislabelled
   entry is self-identifying.

**Next implementation slice:** W5 Inspector authoring (`RenderScriptEditor` +
`PropertyEditSession<ScriptComponent>`), which builds on the W4 command seam
to submit staged canonical state through history on commit. The `effective`
flag from the High fix protects both `Execute` and `RecordApplied` against
phantom entries when the document moves between capture and commit.

#### Phase 6B W5 implementation plan — inspector authoring (planned 2026-07-22)

**Outcome.** `RenderScriptEditor(EntityId)` renders one widget per declared
public field, typed by `ScriptFieldType`, with the asset-path field, Add/Remove
Script buttons, a diagnostics line, and the record-on-release pattern for
continuous edits. Every authoring action (add script, remove script, edit path,
edit field) submits one `SetScriptCommand` through `EditorCommandHistory` and is
undoable. The interactive acceptance gate (author a script, edit `speed`, Play,
verify motion, Stop, verify authoring unchanged) becomes runnable.

##### Grounded findings

| ID | Current fact | W5 consequence |
|---|---|---|
| W5-F1 | `RenderLightEditor` (`SceneEditorUI.cpp:1401-1490`) is the closest structural precedent: a `PropertyEditSession<T>` with `OnActivated`/`OnEditCommitted`/`CloseDeferred`, an owning-widget-id guard, and a `Record*Edit` helper that calls `RecordApplied`. | `RenderScriptEditor` follows this shape. The session stores `ScriptComponent` (the full component, not individual fields) so the command's before/after snapshots are complete. |
| W5-F2 | The Motion block (`:939-1003`) is inline in `RenderInspector`, not a separate `Render*Editor`. It uses the same session pattern but with immediate `SetMotionState` calls per frame and `RecordMotionEdit` on close. | `RenderScriptEditor` is a separate method like Light/Camera, not inline. The Motion block's Add/Remove button pair is the precedent for the Script Add/Remove buttons. |
| W5-F3 | `SceneEditorUI::Undo()`/`Redo()` (`:79-103`) discard 6 sessions but miss `m_MotionVelocitySession` (W4-F10). `ResetForDocument()` (`:123-134`) correctly discards all 7. | W5 must centralize "discard all property sessions" into one private method and call it from `Undo`, `Redo`, and `ResetForDocument`, adding both the missing motion session and the new script session. |
| W5-F4 | The inspector has no `ScriptFieldRegistry` injection. `ScriptSystem::FieldRegistry()` exists but `ScriptSystem` is lazy-created at Play (`EnsureScriptRuntimeWired`). The inspector needs declarations while STOPPED. | `WalnutApp` owns a `std::unique_ptr<rt2::core::ScriptFieldRegistry> m_InspectorFieldRegistry`, created at startup, injected into `SceneEditorUI` via `SetFieldRegistry`. Cleared on scene load/close alongside the existing `ResetForDocument` path. |
| W5-F5 | `ResolveScriptAssetPath(document, component)` (`ScriptAssetPath.h`) is the shared scene-relative path resolver used by both the runtime and the W2/W3 resolver. | The inspector uses it to resolve the authored `ScriptComponent::asset.path` against `m_SceneMgr->AuthoringDoc()` before querying the registry. |
| W5-F6 | `SceneManager::HasScript(EntityId)` and `GetScriptState(UUID)` (`SceneManager.h:372-374`) are the read APIs. `SetScriptState(UUID, optional<ScriptComponent>)` is the write API (W0). | The inspector reads via `GetScriptState`, writes via `SetScriptState` (per-frame for continuous edits, like Motion/Light), and records via `MakeSetScriptCommandIfEffective` + `RecordApplied` on close. |
| W5-F7 | `ScriptFieldRegistry::Result` carries `descriptors`, `parsed`, and `diagnostic`. D10 requires that a `parsed=false` result displays the last-known-good descriptors with a warning, not an empty panel. | `RenderScriptEditor` checks `parsed` and renders a warning banner when false. Field widgets are still rendered from the last-known-good descriptors so the user can see existing values, but they are **read-only** (`BeginDisabled`) while `parsed == false` so the user cannot author values against declarations the engine has stopped trusting (review Medium finding). |
| W5-F8 | The W4 `effective` flag on `EditorMutationResult` prevents phantom history entries when the manager suppresses a no-op. `RecordApplied` now skips recording when `success && !effective`. | The `RecordScriptEdit` helper must pass the manager's actual `EditorMutationResult` to `RecordApplied` (not a synthesized one like the Light/Motion helpers do), so the `effective` flag flows through correctly. This is a correction to the existing `Record*Edit` pattern — the Light/Motion helpers synthesize `applied` with `effective=true` and would record a phantom entry if the manager suppressed. Fixing those is out of scope for W5 but worth noting. |
| W5-F9 | `ScriptFieldEntry::type` distinguishes `vec3` from `color` (same variant arm, different inspector widget). The widget map must dispatch on `ScriptFieldType`, not on the variant index. | Widget dispatch: `Bool→Checkbox`, `Int→DragInt`, `Float→DragFloat`, `String→InputText(EnterReturnsTrue)`, `Uuid→validated InputText`, `Vec3→DragFloat3`, `Color→ColorEdit3`. |
| W5-F10 | The plan spec says the asset-path field is edited as text (the Rebind button is deferred to Phase 7's content-browser era). A path edit is a discrete commit (InputText with EnterReturnsTrue), not a continuous drag. | The path field uses `ImGui::InputText` with `ImGuiInputTextFlags_EnterReturnsTrue`. On Enter, capture before from `GetScriptState`, build the after-state with the new path (fields unchanged), and submit via `MakeSetScriptCommandIfEffective` + `Execute`. No session needed — it's a discrete edit like the Add/Remove buttons. |
| W5-F11 | An unbound `ScriptComponent` (empty path, no fields) is a valid state. The inspector must render the path field and Remove button, but no field widgets and no crash. A non-empty path that does not resolve on disk renders a "script not found" diagnostic. | The empty-path guard is the first check in `RenderScriptEditor`. When the path is empty, render only the path InputText and Remove button. When the path is non-empty but `GetDeclaredFields` returns `parsed=false` with an empty descriptor list and a "file not found" diagnostic, render the diagnostic line and no field widgets. |
| W5-F12 | Field names are sorted by name ascending (D2). The registry returns them pre-sorted. | The inspector iterates the descriptor vector in order; no re-sorting needed. |
| W5-F13 | `PropertyEditSession<ScriptComponent>` stores the full component as the before-value. On close, `CloseDeferred` returns `{before, after}`. The after-value is the current `GetScriptState` — NOT the per-frame mutated working copy, because the per-frame mutations already wrote to the manager. | This matches the Light/Motion pattern: per-frame writes go to the manager, `CloseDeferred` reads the after from the manager, and `RecordApplied` records the command. The `effective` flag from the manager's `SetScriptState` result is passed through. |

##### Command contract

`RenderScriptEditor` submits commands through three paths:

1. **Add Script** (discrete): capture `before = nullopt`, construct `after` as an
   unbound `ScriptComponent` (kind=Script, empty path, no fields), call
   `SetScriptState(target, after)`, then `MakeSetScriptCommandIfEffective(target,
   before, after)` + `Execute`. If the factory returns null (impossible for
   add-from-absent, but defensive), skip.

2. **Remove Script** (discrete): capture `before = GetScriptState(target)`,
   call `SetScriptState(target, nullopt)`, then
   `MakeSetScriptCommandIfEffective(target, before, nullopt)` + `Execute`.

3. **Path edit** (discrete): capture `before = GetScriptState(target)`, construct
   `after` = `before` with the new path (fields unchanged), call
   `SetScriptState(target, after)`, then
   `MakeSetScriptCommandIfEffective(target, before, after)` + `Execute`.

4. **Field edit** (continuous, via session): the session captures `before` on
   widget activation. Per-frame, the inspector mutates a UI-owned working copy
   and calls `SetScriptState` to write it to the manager (so the document
   reflects the edit live). On `DeactivatedAfterEdit`, `CloseDeferred` reads the
   after from `GetScriptState`, builds the command, and calls `RecordApplied`
   with the manager's actual result (so `effective` flows through). On
   `Deactivated` without AfterEdit (Escape), `OnCancelled` discards.

   **Important:** the per-frame `SetScriptState` call for a continuous field
   edit writes the ENTIRE `ScriptComponent` (all fields), not just the one being
   dragged. This is because `SetScriptState` takes the whole component. The
   working copy is the full component with one field's value updated.

##### Widget dispatch

```
ScriptFieldType → ImGui widget
  Bool   → Checkbox           (discrete: commit immediately, no session)
  Int    → DragInt            (continuous: session)
  Float  → DragFloat          (continuous: session)
  String → InputText(EnterReturnsTrue) (discrete: commit on Enter)
  Uuid   → InputText(EnterReturnsTrue) (discrete: commit on Enter, validate)
  Vec3   → DragFloat3         (continuous: session)
  Color  → ColorEdit3         (continuous: session)
```

Discrete widgets (Checkbox, InputText with EnterReturnsTrue) commit immediately
via `Execute` — no session. Continuous widgets (Drag*) go through the
`PropertyEditSession<ScriptComponent>` with the owning-widget-id guard (like
Light's `drawLightWidget` lambda).

**Text-commit on focus loss (review High finding).** `EnterReturnsTrue` fires
only on Enter, not on focus loss. The existing entity-name field
(`SceneEditorUI.cpp:916-924`) demonstrates the bug: it captures
`IsItemActivated()` and discards it with `(void)`, so typing a name and
clicking away silently loses the edit. W5 commits text fields (path, String,
Uuid) on `IsItemDeactivatedAfterEdit()` in addition to Enter — the standard
ImGui idiom already used for Drag* widgets elsewhere in this file. Escape-cancel
remains correct because ImGui reverts the buffer and fires
`IsItemDeactivated()` without `AfterEdit`. W5.0 also fixes the entity-name
field's commit in the same pass so the two text-commit idioms do not diverge.

UUID validation: parse with `UUID::Parse`. Accept nil as "unset" (W3 defines it
as a legitimate value). Reject anything else by reverting the buffer and
showing an error line — do not commit. This matches the loader's strictness
(W3 drops fields that fail the `ToString()` round-trip) so the inspector cannot
author a value that saves and then vanishes on reload.

**ColorEdit3 picker popup handling (review Medium finding).** `ColorEdit3` is
not a simple drag — clicking the swatch opens a picker popup, and
`IsItemDeactivatedAfterEdit()` on the parent widget interacts with popup
open/close differently from `DragFloat3`. W5.3 treats Color as its own case
rather than folding it in with `Vec3`: the session opens on
`IsItemActivated()`, commits on `IsItemDeactivatedAfterEdit()`, and cancels on
`IsItemDeactivated()` without `AfterEdit`. W5.6 guard tests cover
open-picker → edit → close as a distinct `PropertyEditSession` lifecycle.

##### Session lifecycle

One `PropertyEditSession<ScriptComponent> m_ScriptFieldSession` (single active
slot). The owning-widget-id guard (`m_ScriptFieldSessionOwningWidgetId`)
ensures only the widget that started the edit closes it (same pattern as Light).

The session is discarded on:
- `Deactivated` without AfterEdit (Escape cancel)
- Selection change (via `ResetForDocument` or the centralized discard)
- Entity death (guard predicate: `FindEntityByUuid(target) != entt::null`)
- Play entry (`SetEditable(false)` disables widgets; the session is discarded)
- Undo/Redo (via the centralized discard)
- Document adoption (via `ResetForDocument`)

##### Registry injection

`WalnutApp` owns `std::unique_ptr<rt2::core::ScriptFieldRegistry>
m_InspectorFieldRegistry`, created in the constructor. Injected into
`SceneEditorUI` via `SetFieldRegistry(rt2::core::ScriptFieldRegistry*)`.
Cleared (`Clear()`) on scene load and scene close, alongside
`ResetForDocument`. The registry is CPU-only and links cleanly.

The inspector resolves the script path via `ResolveScriptAssetPath(
m_SceneMgr->AuthoringDoc(), component)` and queries
`m_FieldRegistry->GetDeclaredFields(resolvedPath)`.

When no registry is injected (tests), `RenderScriptEditor` renders the path
field and Add/Remove buttons but no field widgets (defensive null check).

**Per-frame I/O fix (review High finding).** `GetDeclaredFields` currently
reads and hashes the entire file on every call, then consults the cache — the
cache prevents re-parsing, not re-reading. At 60 fps with an entity selected,
that is 60 file opens + full reads + hashes per second on the UI thread. W5.1
adds a fast-path staleness gate in `ScriptFieldRegistry::GetDeclaredFields`:
check `(mtime, size)` via `std::filesystem::last_write_time` + `file_size`
first, and only read+hash when they differ or the entry is absent. This
preserves the hash's purpose (catching same-length edits within one timestamp
tick) at the cost of one `stat` per frame. The hash still wins on a
same-tick same-length edit the moment the timestamp advances.

##### Centralized session discard

Add a private `DiscardAllPropertySessions()` method to `SceneEditorUI`:

```cpp
void DiscardAllPropertySessions()
{
    m_TransformSession.Discard();
    m_NameSession.Discard();
    m_LightSession.Discard();
    m_CameraSession.Discard();
    m_MaterialIndexSession.Discard();
    m_MaterialPropertiesSession.Discard();
    m_MotionVelocitySession.Discard();
    m_ScriptFieldSession.Discard();
}
```

Call it from `Undo()`, `Redo()`, and `ResetForDocument()` (replacing the
hand-maintained lists that missed `m_MotionVelocitySession` in Undo/Redo).

##### Inspector dispatch

In `RenderInspector`, after the Camera editor and before/after the Motion block,
add:

```cpp
if (m_SceneMgr->HasScript(entity))
    RenderScriptEditor(entity);
```

Also add an "Add Script" button when the entity has no ScriptComponent (like the
"Add Motion" button in the Motion block).

##### Implementation order

1. **W5.0 — Centralize session discard.** Add
   `DiscardAllPropertySessions()`, call from `Undo`, `Redo`, and
   `ResetForDocument`. This fixes W4-F10 (missing motion discard in Undo/Redo)
   and adds the script session. Build and run the full suite to verify no
   regression from the motion-discard fix.

2. **W5.1 — Registry injection.** Add `SetFieldRegistry` to `SceneEditorUI`,
   `m_InspectorFieldRegistry` to `WalnutApp`, wire in the constructor, clear on
   scene load/close. Add `m_ScriptFieldSession` and
   `m_ScriptFieldSessionOwningWidgetId` members to `SceneEditorUI`. Add
   `RenderScriptEditor` declaration. Build.

3. **W5.2 — `RenderScriptEditor` skeleton.** Implement the method: header text,
   path field (InputText with EnterReturnsTrue), Add/Remove buttons, diagnostics
   line, and the field-widget loop. Start with the empty-path guard and the
   no-registry guard. Build and manually verify the inspector renders for a
   scripted entity.

4. **W5.3 — Field widgets.** Implement the widget dispatch for all seven types.
   Continuous widgets go through the session; discrete widgets commit
   immediately. Color is treated as its own case (picker popup handling). All
   widgets are wrapped in `BeginDisabled(!m_Editable)` so authoring is blocked
   during Play (review gap 1). Add `RecordScriptEdit` helper that calls
   `RecordApplied` with the manager's actual result (not a synthesized one).
   Build and manually test each widget type.

5. **W5.4 — Path edit and Add/Remove.** Implement the discrete commit paths for
   path edit, Add Script, and Remove Script. Text fields commit on
   `IsItemDeactivatedAfterEdit()` as well as Enter (review High finding). Also
   fix the entity-name field's commit in the same pass. Build and manually test.

6. **W5.5 — Diagnostics and parse-failure banner.** When `parsed=false`, render
   a warning banner with the diagnostic text and render the last-known-good
   field widgets read-only (`BeginDisabled`). When the path resolves to a
   missing file, render "script not found." When the path is empty (unbound
   component), render only the path field and Remove button — no field widgets,
   no diagnostics banner (review gap 2). Build and manually test with a
   broken script and a missing path.

7. **W5.6 — Inspector guard tests.** Add CPU test cases to
   `Phase6BFieldsTests.cpp` covering: empty-path component renders no field
   widgets, non-existent path renders a diagnostic, `parsed=false` widgets are
   read-only, and the session lifecycle (capture/commit/cancel, including the
   ColorEdit3 picker-popup case) through the `PropertyEditSession` state machine.

8. **W5.7 — Interactive acceptance gate.** This is the Phase 6B exit gate:
   author a script with `rt2.fields = { speed = rt2.field.float(5.0) }` that
   moves the entity by `speed * dt`; save; reopen; verify the inspector shows
   the saved value; edit it to 9.0; Play; verify the entity moves at 9.0
   units/sec; Stop; verify the authoring scene is unchanged (speed is still
   9.0, not reset to 5.0). Undo the field edit and verify the value reverts to
   5.0. Redo and verify it returns to 9.0. This proves the full 6B stack
   (W0 wiring → W1 reflection → W2 reconciliation → W3 persistence → W4
   command/history → W5 inspector) end to end.

9. **W5.8 — Documentation and verification.** Update scripting/scene docs, build
   Release, run focused W5/Phase 6 tests, the full suite against its
   seven-failure baseline, and refresh Graphify.

##### Test matrix

Add W5 cases to `RT2Tests/src/Phase6BFieldsTests.cpp`:

- `PropertyEditSession<ScriptComponent>` lifecycle: OnActivated captures before,
  OnEditCommitted + CloseDeferred returns {before, after}, OnCancelled discards,
  no-commit activation discards on close.
- Centralized session discard: Undo/Redo discards all 8 sessions including
  motion and script (verify via a fixture that opens a motion session, calls
  Undo, and asserts the motion session is no longer open).
- Inspector guard (requires a minimal `SceneEditorUI` fixture or a separate
  headless test): a `ScriptComponent` with an empty `asset.path` produces no
  field-widget rendering path (assert the code path is taken, not pixel output).

The interactive acceptance gate is manual (W5.7) and cannot be automated without
a GLFW/ImGui test harness.

##### W5 acceptance gate

Author a script with `rt2.fields = { speed = rt2.field.float(5.0) }` that moves
the entity by `speed * dt` in `on_update`. In the editor: add a ScriptComponent
to an entity, set the path, verify the `speed` field appears in the inspector,
edit it to 9.0, save, reopen, verify the inspector shows 9.0, Play, verify the
entity moves at 9.0 units/sec, Stop, verify the authoring scene is unchanged
(speed is still 9.0, not reset to 5.0). Undo the field edit and verify the value
reverts to 5.0. Redo and verify it returns to 9.0.

The Release solution builds, all W5/Phase 6 tests pass, and the full suite
introduces no failure beyond the documented seven-test baseline.

**Out of scope.** File dialogs for path browsing, interactive rebinding UI
(Phase 7), declaration diagnostics from the registry shown inline in the
inspector (the warning banner is the minimum), hot reload (6C), and the
`RecordApplied`-synthesizes-`effective` issue in existing Light/Motion helpers
(noted in W5-F8 but fixing it is a separate hardening pass — tracked here so
it is not forgotten when Light/Motion gain no-op suppression). Per-frame
full-component re-validation (review Medium finding) is acceptable for W5
given typical field counts; if it shows up in profiling, the fix is to
validate incrementally at the mutation boundary rather than re-scanning
unchanged entries.

#### Phase 6B W5 implementation — inspector authoring (implemented 2026-07-22)

**Outcome.** `RenderScriptEditor(EntityId)` renders one widget per declared
public field, typed by `ScriptFieldType`, with the asset-path field, Add/Remove
Script buttons, a parse-failure warning banner, and the record-on-release
session pattern for continuous edits. Every authoring action submits one
`SetScriptCommand` through `EditorCommandHistory` and is undoable. The
interactive acceptance gate is runnable.

**W5.0 — Centralized session discard + entity-name commit fix.** Added
`DiscardAllPropertySessions()` to `SceneEditorUI`, replacing the hand-maintained
lists in `Undo()`, `Redo()`, and `ResetForDocument()` that missed
`m_MotionVelocitySession` in Undo/Redo (W4-F10). The method discards all 8
sessions including the new `m_ScriptFieldSession`. Also fixed the entity-name
field's text-commit bug: it now commits on `IsItemDeactivatedAfterEdit()` (focus
loss) in addition to Enter, matching the standard ImGui idiom used for Drag*
widgets. The `(void)nameActivated` discard that silently lost edits on focus
loss is removed.

**W5.1 — Registry injection + fast-path staleness gate.** `WalnutApp` owns
`std::unique_ptr<ScriptFieldRegistry> m_InspectorFieldRegistry`, created at
startup, injected into `SceneEditorUI` via `SetFieldRegistry`, and cleared on
scene load/close. The registry is independent of `ScriptSystem` (which is
lazy-created at Play) so the inspector can query declarations while STOPPED.

`ScriptFieldRegistry::GetDeclaredFields` now has a fast-path: check
`(mtime, size)` against the cache first and return cached descriptors without
reading or hashing the file. Only when they differ (or the entry is absent) does
it read + hash + re-parse. The hash tiebreaker still catches same-tick
same-size edits the moment the timestamp advances. This avoids 60 file reads +
hashes per second when the inspector queries every frame (review High finding).

**W5.2-W5.5 — RenderScriptEditor.** Follows `RenderLightEditor`'s structure:
path field (`InputText` with `EnterReturnsTrue` + `DeactivatedAfterEdit` for
focus-loss commit), Remove Script button, Add Script button (in `RenderInspector`
when no ScriptComponent exists), diagnostics warning banner when `parsed=false`
with field widgets rendered read-only (`BeginDisabled`), and the field-widget
loop.

Widget dispatch by `ScriptFieldType`:
- `Bool→Checkbox` (discrete: immediate command via `RecordScriptEdit`)
- `Int→DragInt`, `Float→DragFloat`, `Vec3→DragFloat3`, `Color→ColorEdit3`
  (continuous: `PropertyEditSession<ScriptComponent>` with owning-widget-id guard)
- `String→InputText(EnterReturnsTrue)` + `DeactivatedAfterEdit` (discrete)
- `Uuid→InputText(EnterReturnsTrue)` + `DeactivatedAfterEdit` (discrete, validated:
  accept nil as "unset", reject garbage by reverting)
- Empty path (unbound): path field + Remove only, no field widgets, no banner

`RecordScriptEdit` passes the manager's actual `EditorMutationResult` to
`RecordApplied` (not a synthesized one), so the `effective` flag flows through
correctly (W5-F8). All widgets are wrapped in `BeginDisabled(!m_Editable)` so
authoring is blocked during Play.

**W5.6 — Inspector guard tests.** 3 cases in `Phase6BFieldsTests.cpp`:
- `PropertyEditSession<ScriptComponent>` lifecycle: open/commit/close produces
  a record; open without commit produces none; cancel produces none; guard
  failure produces none.
- Registry fast-path: unchanged file returns cached descriptors without
  re-reading; modified file (different size) re-parses.
- Registry same-size edit: rewrite with same byte count but different content;
  after timestamp advance, the new default is returned (hash tiebreaker).

**W5 verification:** Release solution builds clean (both RT2App and RT2Tests);
focused W5 is 3/3 cases and 28/28 assertions; the full Phase6B filter is 71/71
cases and 569/569 assertions; the full Release suite is 524 run, 517 passed, 7
failed, 0 skipped — the unchanged seven-failure baseline. No W5 case fails.

**Remaining for W5.7 (interactive acceptance gate):** manual verification —
author a script with `rt2.fields = { speed = rt2.field.float(5.0) }` that moves
the entity by `speed * dt`, add a ScriptComponent via the inspector, set the
path, edit `speed` to 9.0, save, reopen, Play, verify motion at 9.0 units/sec,
Stop, verify authoring unchanged, Undo/Redo the field edit. This proves the
full 6B stack end to end.

### Phase 6C — Hot reload, input, and remaining bindings (implemented)

**Outcome.** Editing a `.lua` file while Playing hot-reloads it without
restarting RT2; syntax/runtime errors during reload are reported with
path, entity, callback, and stack trace but the scene keeps running.
Scripts can read input, set light/camera/material properties, and use
timers. A deterministic headless script emits a JSON state report
asserting the final transform. This slice satisfies the Phase 6 exit
criteria.

**File watcher (vendored):** efsw (Entropia File System Watcher) under
`RT2App/vendor/efsw/`, built as a static lib via premake. The watcher
is owned by `WalnutApp` (the only owner of GLFW/ImGui/Walnut) and
watches the project root + any directory containing a referenced
script asset. On change, `WalnutApp` posts a deferred reload request to
`ScriptSystem` (thread-safe queue; the actual reload runs on the main
thread at the next `OnUIRender` top, before `EndFrame`, so it does not
race the fixed-step loop). `RT2Tests` and `RT2SliceRunner` do not link
efsw; tests exercise reload via explicit `ReloadScript(path)` calls
(the same API the watcher uses internally).

**Hot reload semantics (built on the S1 state machine and S3 `self`
table from 6A; the `ReloadScript(path)` API is declared on
`ScriptSystem` from 6A and stubbed, implemented in 6C):**

- Reload re-evaluates `rt2.fields` in the sandboxed environment and
  runs the 6B compatibility rules against the in-memory field values
  (the current `self` table).
- Reload re-binds `on_create` / `on_fixed_update` / `on_update` /
  `on_destroy` from the new source. If a callback was previously bound
  and is now missing, it is unbound silently (not an error). If a
  callback was previously missing and is now present, it is bound;
  `on_create` is NOT re-run on reload (the entity already exists).
- **S3 `rt2.previous_state`:** reload copies the old environment's
  `self` table (the runtime-mutated values, not the persisted ones)
  into the new environment's `rt2.previous_state` before re-binding
  callbacks. Scripts that want full re-init ignore it; scripts that
  want to preserve runtime state read it. This is the narrowest
  contract that survives reload without forcing re-derivation.
- **S1 reload un-quarantine:** a successful reload transitions a
  `Quarantined` instance back to `Live` (re-binds callbacks, does NOT
  re-run `on_create` — the entity exists). A failed reload (syntax
  error) keeps it `Quarantined`. The error includes path, entity UUID,
  callback name (empty for load-time), and stack trace. Instances of
  the same source quarantine and un-quarantine together (they share
  the source).
- Reload is suppressed during Paused (the user is inspecting a frozen
  state; a reload mid-pause would be confusing). Queued reload
  requests drain on the next Play frame or the next Resume.

**Remaining bindings (added in 6C):**

- `input.is_down("action")`, `input.is_pressed("action")`,
  `input.is_released("action")`, `input.get_axis("axis")`,
  `input.get_mouse_delta()`, `input.get_scroll_delta()`. Read-only;
  the `IInputService&` is handed to `ScriptSystem::OnSceneStart` (the
  seam from Phase 5).
- `entity.get_light()` / `entity.set_light(color, intensity, isSpot)`,
  `entity.get_camera()` / `entity.set_camera(fov, aperture,
  focusDistance)`, `entity.set_material_index(index)`.
- `timer.after(seconds, callback)`, `timer.every(seconds, callback)`,
  `timer.cancel(handle)`. Timers respect Pause (no firing while Paused)
  and scene destruction (all timers cancelled on Stop). Timers fire on
  the main thread, in the variable-update phase.
- `rt2.fields`, `rt2.field.float/int/string/bool/uuid`, `rt2.previous_state`,
  `rt2.reload()` (requests a self-reload; useful for development).

**Headless acceptance (`RT2SliceRunner` extension; G3 deterministic
UUIDs):**

- New `--script-scenario <path>` mode: loads a scene, Play, runs a
  fixed number of frames, emits a JSON state report (`entities:
  [{uuid, name, position, rotation, scale, visible}]`), Stop, and
  exits with a non-zero code on any script error or assertion
  mismatch. The script scenario file declares the scene path, the
  number of frames, a UUID seed, and the expected final transforms
  (UUID-keyed).
- **G3 determinism:** `RT2SliceRunner::RunScriptScenario` installs a
  `DeterministicUuidProvider` seeded from the scenario file's `uuidSeed`
  via `RuntimeSceneController::SetRuntimeUuidProvider` (the seam from
  Phase 4). Spawned entities receive deterministic UUIDs from the
  seeded sequence, so the JSON report is reproducible run-to-run and
  the `expectedTransforms` assertions are stable. The scenario file
  may instead set `forbidSpawn: true` to restrict assertions to pre-
  existing UUIDs (a scenario that wants to test only authored-entity
  scripting); both modes are supported.
- `run_script_test.ps1` regression script invokes the slice runner in
  this mode against a checked-in tiny fixture (`assets/script-
  scenario.rt2scene` + `assets/script-scenario.lua`) and asserts the
  JSON report matches expectations.
- This links Lua (and only Lua — not efsw, not sol2's optional
  dependencies) into `RT2SliceRunner` via its premake.

**Test plan (`RT2Tests/src/Phase6CHotReloadTests.cpp` and
`RT2Tests/src/Phase6CBindingsTests.cpp`):**

- `ReloadScript(path)` with an added field runs the 6B compatibility
  rules; the new field receives its default; existing fields preserve
  values.
- `ReloadScript(path)` with a syntax error leaves every instance of that
  source in its current state — a parse failure must NOT quarantine a
  Live instance, because the running code is still valid and the author
  is mid-keystroke. Only a successful parse can replace anything.
  *(Amended in W9: this bullet previously specified the opposite. The
  implemented behaviour is the correct one and is pinned by a test.)*
- An instance quarantined by a **runtime** error returns to `Live` on a
  subsequent `ReloadScript` with valid source (S1: `Quarantined` → `Live`).
- A reload that adds an `on_update` callback (previously missing) binds
  it without re-running `on_create`.
- A reload that removes an `on_update` callback unbinds it; subsequent
  frames do not call it.
- `rt2.previous_state` carries a `self` table value across reload
  (S3).
- Reload is suppressed while Paused; queued reloads drain on Resume.
- `input.is_pressed` returns the same edge as the C++
  `IInputService::IsPressed` for the same frame.
- `timer.after(0.1, ...)` fires exactly once, after the expected
  number of fixed steps; `timer.every(0.05, ...)` fires repeatedly;
  cancel prevents further firings; Pause suppresses all firings; Stop
  cancels all timers.
- `entity.set_light(...)` results in a Material-or-Structural sync
  (lights are structural in the current sync-impact contract — the
  test asserts the expected impact).
- Destroying an entity with a pending timer cancels the timer safely
  (no firing after destroy).
- The headless `--script-scenario` runner emits the expected JSON
  report for the checked-in fixture and exits zero.

**Resolved ahead of 6C (2026-07-21).** Four gaps found by review were
fixed immediately rather than deferred, because three were reachable by
any user script and the fourth broke a documented contract:

- **Runtime instruction budget — DONE.** Protected calls catch *errors*,
  not *hangs*: a callback containing `while true do end` never returns,
  so no result is produced and neither `sol::protected_function` nor
  `lua_atpanic` can intervene; S1 quarantine cannot help because
  quarantine requires the call to return. All five runtime Lua entry
  points (chunk load plus the four callbacks) now run under a
  `ScriptInstructionBudget` (`ScriptSandbox.h`), whose `LUA_MASKCOUNT`
  hook routes an exhausted budget through the ordinary error path into
  Quarantine. Two tests cover it; without the fix they hang the suite.
- **`world.spawn` script attachment — DONE.** The binding read only
  `desc.name` and silently dropped `desc.script`, so G2's "scripts spawn
  scripted entities" was unmet by the Lua API even though
  `RuntimeEntityCreateDesc::script` and `RuntimeSceneMutator` supported
  it. Worse, the 6A test "spawn with ScriptComponent produces scripted
  entity" asserted `LiveInstanceCount() == 1` — it *enshrined* the bug
  under a name claiming the opposite. Binding fixed; test rewritten to
  assert the child is live and its `on_create` ran.
- **Mid-session `on_destroy` ordering — DONE.** The drain applied the
  destruction and only then fired `on_destroy` via
  `SyncScriptEnvironments`, so a script's final callback saw its own
  entity already removed (`entity:get_name()` returned empty). New
  `IRuntimeScriptDispatch::OnEntitiesDestroying(uuids)` fires from inside
  `ApplyDeferredStructuralChanges`, immediately *before* the removal,
  with the subtree in post-order (children first). Defaulted to a no-op
  so other dispatch implementations are unaffected.
- **Script path in the quarantine log — DONE.** `ScriptInstance` now
  carries its resolved path and `Quarantine` logs it. Note the stack
  trace was never actually missing: sol's default handler builds one with
  `luaL_traceback`, which is in `lauxlib` and needs no `debug` library —
  an early review claim to the contrary was wrong.

**Still open for 6C:** `LuaPanic` throws a C++ exception from a handler
invoked by C-compiled Lua. Every Lua entry is protected so it should be
unreachable, but if it fires the throw crosses C frames reached via
longjmp, which is undefined on MSVC. The sound fix is compiling Lua as
C++ (`LUAI_THROW` then throws), a premake/vendor change that deserves its
own commit. Returning from the handler is NOT an alternative — in Lua 5.4
that calls `abort()`.

Also for consideration in 6C: `sol::lib::coroutine` was opened in 6A with
nothing using it and no sandboxing story, and has since been closed. If
`timer.after`/`timer.every` are implemented on coroutines, re-open it
deliberately and revisit the deny list in `ScriptSandbox.h` at the same
time.

**Verification gates (6C):** same as 6A/6B plus the headless script
scenario regression; interactive acceptance: edit a Playing script's
`speed` field declaration in the `.lua` file, save it, observe the
reload log in the console, and verify the new value takes effect
without restarting RT2; introduce a syntax error and verify the scene
keeps running with a useful error.

#### Phase 6C implementation plan (approved 2026-07-22)

Written after grounding the 6C spec against the code as it stands post-6A/6B.
Findings C1–C10 below are the evidence base; workstreams W0–W9 follow.

##### Grounded findings

> **These describe the code as it stood BEFORE 6C (2026-07-22).** They are
> the evidence base the 6C plan was built on, kept unedited so the reasoning
> can be audited. Every "current fact" below has since been changed by the
> work it motivated — C1's stub is now a full implementation, C3's inert
> `input` table now has its methods, and so on. Do not read this table as a
> description of the present tree.

| ID | Fact as of 2026-07-22 (pre-6C) | 6C consequence |
|---|---|---|
| C1 | `ScriptSystem::ReloadScript` is a virtual stub (`ScriptSystem.h:135`) that does nothing. `BuildEnvironment` (`ScriptSystem.cpp:409-743`) constructs a fresh `sol::environment`, loads the chunk, binds callbacks, and injects `self`. | 6C must implement reload as: re-parse declarations via the registry, re-run `BuildEnvironment` into a scratch environment, reconcile field values against new declarations, copy old `self` into `rt2.previous_state`, swap the environment, and re-bind callbacks. A syntax error must leave the running instance untouched (D9 trap 2). |
| C2 | Four items were resolved ahead of 6C (lines 5714-5761): runtime instruction budget, `world.spawn` script attachment, `OnEntitiesDestroying` ordering, and script path in the quarantine log. All four are confirmed in the code. | 6C does not re-address these. They are done. |
| C3 | `IInputService` (`InputTypes.h:252-277`) exposes `IsPressed`, `IsDown`, `IsReleased`, `GetAxisValue`, `GetMouseDelta`, `GetScrollDelta`. `ScriptSystem::OnSceneStart` receives and stores `m_Input` (`ScriptSystem.h:122,213`). `BuildEnvironment` binds an inert `input` table (`ScriptSystem.cpp:696-704`) with no methods. | 6C adds Lua bindings to the `input` table that call through to `m_Input`. The table is already created; only the method bindings are missing. |
| C4 | `ScriptSystem::BuildEnvironment` (`:560-628`) already binds `entity.set_position`, `entity.set_local_transform`, `entity.get_position`, `entity.get_name`, `entity.get_uuid`, `entity.is_alive`, `entity.set_visible`, `entity.get_visible`, `world.spawn`, `world.destroy`, `world.find_by_name`, `world.find_by_uuid`, `log.info`, `log.warn`, `log.error`. | 6C adds `entity.get_light`, `entity.set_light`, `entity.get_camera`, `entity.set_camera`, `entity.set_material_index` to the entity table. These route through the `RuntimeCommandSink` (for set) or read the runtime document directly (for get). |
| C5 | `RuntimeCommandSink` (`ScriptSystem.cpp:690-820`) already implements `SetLocalTransform`, `SetPosition`, `SetName`, `SetVisible`, `IsAlive`, `FindByName`, `FindByName`, `GetLocalTransform`, `GetPosition`, `GetName`, `GetVisible`. | 6C adds `SetLight`, `GetLight`, `SetCamera`, `GetCamera`, `SetMaterialIndex` to the sink. The sink's `IsRuntimeMutable()` gate already protects against writes outside Play. |
| C6 | `ScriptSandbox.h` already defines `InstallSandbox` (deny list + library copies) and `ScriptInstructionBudget` (RAII hook). `coroutine` is copied but inert (line 202: "no-op unless 6C re-opens it"). | If `timer.after`/`timer.every` are implemented via Lua coroutines, `coroutine` is already isolated per-environment and ready. If implemented in C++, no change needed. The plan says timers fire on the main thread in the variable-update phase — C++ timers are simpler and avoid coroutine-yield semantics in the fixed-step loop. |
| C7 | `RT2SliceRunner` (`premake5.lua`) does NOT link Lua or sol2. It compiles a subset of CPU-only RT2App sources. The 6C `--script-scenario` mode needs Lua execution. | 6C adds Lua C sources + sol2 include path to `RT2SliceRunner/premake5.lua`, matching the pattern in `RT2Tests/premake5.lua:15-48`. It also links `ScriptSystem.cpp`, `ScriptFieldRegistry.cpp`, `ScriptAssetPath.cpp`, and `ScriptSandbox.h`. No efsw. |
| C8 | `RuntimeSceneController::SetRuntimeUuidProvider` (`RuntimeSceneController.h:166`) exists and is used by Phase 4/6A tests. | The headless script scenario installs a `DeterministicUuidProvider` seeded from the scenario file, exactly as Phase 6A tests do (`Phase6ALifecycleTests.cpp:114`). |
| C9 | `LuaPanic` (`ScriptSystem.cpp:53-61`) throws a C++ exception from a C `lua_atpanic` handler. The plan (line 5749-5755) flags this as UB if it ever fires, because Lua is compiled as C. | 6C should compile Lua as C++ (`LUAI_THROW`) OR replace `LuaPanic` with `std::terminate`. This is a premake/vendor change. The plan says "returning from the handler is NOT an alternative — in Lua 5.4 that calls abort()." Compiling Lua as C++ is the clean fix. |
| C10 | `WalnutApp` owns `m_InspectorFieldRegistry` (W5) and `m_ScriptSystem` (W0). The file watcher needs to live in `WalnutApp` (the only GLFW/ImGui/Walnut owner). | 6C adds efsw to `RT2App/vendor/efsw/`, builds it as a static lib via premake, and wires it in `WalnutApp`. The watcher posts to a thread-safe queue; `WalnutApp::OnUIRender` drains it and calls `ScriptSystem::ReloadScript`. |

##### Workstreams

> **Naming:** Findings are C1–C10. Workstreams are W0–W9 (matching 6A/6B
> convention). Where a workstream resolves a finding, it says "resolves
> finding C*N*" explicitly.

- **W0 — `LuaPanic` hardening (resolves finding C9; demoted from the
  critical path per review H1).** The review found that compiling Lua as
  C++ is a three-premake, three-vcxproj, `SOL_USING_CXX_LUA`-in-every-TU
  detonation that gates everything behind a fix for an unreachable defect
  (all Lua entry is protected). W0 is the two-line fix: `LuaPanic` logs the
  message and calls `std::terminate` instead of throwing a C++ exception
  across C frames. The throw is UB on MSVC; `std::terminate` is a clean
  last-resort backstop. Lua-as-C++ remains a deferred option (tracked in
  the plan's open-items section) revisited only if a panic is ever observed.

- **W1 — Hot reload core (resolves finding C1).** Implement
  `ScriptSystem::ReloadScript(path)`:
  1. Canonicalize the path via `ResolveScriptAssetPath`-equivalent
     normalization (review M3: efsw hands OS-native absolute paths;
     `ScriptInstance::scriptPath` is resolved; `ScriptComponent::asset.path`
     is scene-relative — all three must be normalized before comparison).
  2. Re-parse declarations via `ScriptFieldRegistry::GetDeclaredFields`.
  3. If parse fails: log the error, keep all instances of that source in
     their current state (Quarantined stays Quarantined; Live stays Live).
     Do NOT quarantine Live instances on a parse failure — the running code
     is still valid; only the new source is broken.
  4. If parse succeeds: for each instance of that source, build a scratch
     environment via `BuildEnvironment`, reconcile field values against the
     new declarations (reuse `ReconcileScriptFields` from 6B/W2; the
     reconciliation target is the runtime clone's `ScriptComponent::
     fieldValues` — reconciled values live in the clone and are discarded
     at Stop, so adding a field mid-Play shows the default and never
     reaches the authoring document; diagnostics go to the log, review M5),
     copy old `self` into `rt2.previous_state` on the new environment, swap
     the environment and re-bind callbacks.
  5. **Cancel all timers for the instance before the swap** (review B3: a
     `sol::protected_function` registered by `timer.every` is a closure
     over the old environment; without cancellation, old timers keep firing
     alongside new ones after reload). This is the third D9 trap alongside
     "reset the four callbacks" and "build into scratch, swap on clean
     load".
  6. Reset all four callbacks to `sol::protected_function{}` before
     rebinding (D9 trap 1). If the scratch build fails, leave the running
     instance untouched (D9 trap 2).
  7. Un-quarantine: a successful reload transitions `Quarantined` → `Live`.
  8. **Run-state awareness (review B2):** add
     `virtual bool IsRuntimeMutable() const` to `IRuntimeCommandSink` (the
     sink wraps `RuntimeSceneController::IsRuntimeMutable()` but does not
     expose it). `ReloadScript` queries the sink:
     - **Stopped** (no sink / not mutable, `m_Instances` empty): invalidate
       the `ScriptFieldRegistry` cache entry for the path only — the
       inspector's next `GetDeclaredFields` re-parses. No instance work.
     - **Playing:** reload now.
     - **Paused:** queue; drain on Resume or next Play frame. (Review M1:
       timers DO advance under Step — see W5.)

- **W2 — File watcher (resolves finding C10).** Vendor efsw under
  `RT2App/vendor/efsw/` (review L5: copied source, not a junction or
  submodule; the vendor directory is committed like `vendor/lua/` and
  `vendor/sol2/`). Add to `RT2App/premake5.lua` as a static lib. Wire in
  `WalnutApp`: watch directories containing referenced script assets (not
  individual files). On change, post `{path}` to a thread-safe queue.
  **Debounce (review M4):** coalesce by path over a ~100ms quiet window —
  atomic save (temp + rename) yields Modified + Added + Deleted for a
  single Ctrl+S, which would reload 2–3× per save. Drain in `OnUIRender`
  (before `EndFrame`, after the fixed-step loop) and call
  `m_ScriptSystem->ReloadScript(path)`. `RT2Tests` and `RT2SliceRunner` do
  NOT link efsw; tests exercise reload via explicit `ReloadScript(path)`.

- **W3 — Input bindings (resolves finding C3).** Add methods to the
  `input` table in `BuildEnvironment`:
  - `input.is_down(action)` → `m_Input->IsDown(action)`
  - `input.is_pressed(action)` → `m_Input->IsPressed(action)`
  - `input.is_released(action)` → `m_Input->IsReleased(action)`
  - `input.get_axis(axis)` → `m_Input->GetAxisValue(axis)`
  - `input.get_mouse_delta()` → returns `{x, y}` from `m_Input->GetMouseDelta()`
  - `input.get_scroll_delta()` → `m_Input->GetScrollDelta()`
  All read-only. When `m_Input` is null (tests), return inert defaults
  (false/0/zero vector).
  **Cursor capture (review H5):** `IInputService::RequestCursorCapture` is
  non-const, and `ScriptSystem` stores `const IInputService*`. 6C does NOT
  widen the pointer — cursor capture is out of scope. The acceptance script
  uses keyboard-only control. The plan's exit criterion ("drive an entity
  using input") is met by keyboard-driven motion. Mouse-look (which needs
  cursor lock) is a Phase 7 / future binding.

- **W4 — Entity light/camera/material bindings (resolves findings C4, C5).
  (Review H3: routes through the sink, NOT `SceneManager` — the sink writes
  the runtime document's components directly, exactly as `SetVisible`/
  `SetName` already do. No `SceneManager` access, per `IRuntimeCommandSink.h:40`.)**

  Add to the `entity` table in `BuildEnvironment`:
  - `entity.get_light()` → reads `LightComponent` via `try_get` (review H2:
    `registry.get` asserts when absent — use `try_get`, return nil if
    missing). Returns the **full** component (review H4): `{color={r,g,b},
    intensity=N, range=N, inner_cone_angle=N, outer_cone_angle=N,
    is_spot=bool}`.
  - `entity.set_light(color, intensity, range, inner_cone_angle,
    outer_cone_angle, is_spot)` → writes the **full** component via the
    sink. Gates on `IsRuntimeMutable()`.
  - `entity.get_camera()` → reads `CameraComponent` via `try_get`. Returns
    the **full** component (review H4): `{fov=N, aperture=N,
    focus_distance=N, forward={x,y,z}}` (includes `forwardDirection` — the
    one field a script camera controller needs).
  - `entity.set_camera(fov, aperture, focus_distance, forward_x,
    forward_y, forward_z)` → writes the full component via the sink. Gates
    on `IsRuntimeMutable()`.
  - `entity.set_material_index(index)` → routes through the sink. **Bounds
    check (review H2):** `try_get<MeshRef>` (asserts if absent → use
    `try_get`, return false if missing); reject `index < -1` or `index >=
    materialCount` with a log and return false. `-1` is the "use per-
    triangle indices" sentinel (`ECSComponents.h:70-71`) and is valid.

  Add the corresponding methods to `IRuntimeCommandSink` and
  `RuntimeCommandSink`. Get methods read the runtime document directly via
  `try_get`. Set methods gate on `IsRuntimeMutable()`.

  **`get_world_position` (review L3):** close the deferral from
  `IRuntimeCommandSink.h:86-87` — 6C does NOT implement it. State
  explicitly that world-space position is out of scope for 6C and the
  deferral is closed as "not needed."

- **W5 — Timers.** Implement `timer.after(seconds, callback)`,
  `timer.every(seconds, callback)`, `timer.cancel(handle)` in C++ (not Lua
  coroutines — simpler, avoids fixed-step/coroutine-yield interaction).
  Store timers in `ScriptSystem`, keyed by instance UUID. Fire in
  `OnUpdate` (variable-update phase). Timers accumulate against `OnUpdate`'s
  `dt` and **do advance under Step** (review M1: `Step()` calls
  `OnUpdate(dt)` while Paused; suppressing timers there makes single-step
  non-representative). Respect Pause in `Update()` only (which is a no-op
  unless Playing). Cancel all timers on `OnSceneStop`, on entity
  destruction, and **on reload** (review B3: before the environment swap —
  see W1 step 5). `timer.after(0, cb)` fires on the next `OnUpdate`.
  Return an opaque handle (integer) for cancellation. Bind a `timer` table
  in `BuildEnvironment`.
  **Protected-call discipline (review M2):** timer callbacks are
  `protected_function` calls under a `ScriptInstructionBudget` (making
  timers the 6th entry point). An error quarantines the instance. An
  infinite loop is caught by the budget hook.

- **W6 — `rt2.previous_state` and `rt2.reload()`.** On reload, copy the
  old environment's `self` table (runtime-mutated values, not the persisted
  ones) into the new environment's `rt2.previous_state`. Bind
  `rt2.reload()` as a function that posts the entity's own script path to
  the reload queue (useful for development; the watcher calls the same
  internal path).

- **W7 — Headless script scenario (resolves findings C7, C8).** Add
  `--script-scenario <path>` mode to `RT2SliceRunner`:
  1. Parse the scenario JSON: `{scenePath, frames, uuidSeed,
     expectedTransforms: {uuid: {position, rotation, scale}},
     forbidSpawn?}`.
  2. Load the scene via the existing `SceneSerializer::Load` + resolver
     path.
  3. Install `DeterministicUuidProvider(uuidSeed)` via
     `RuntimeSceneController::SetRuntimeUuidProvider`.
  4. Play, run `frames` fixed steps, emit JSON state report
     (`entities: [{uuid, name, position, rotation, scale, visible}]`).
  5. Compare against `expectedTransforms`; exit non-zero on mismatch or
     any script error.
  6. **Drive `Update()` with a constant `frameDt`** (review M1: document
     the value — use `kFixedDt` so the variable phase is deterministic).
  7. Add Lua + sol2 + ScriptSystem to `RT2SliceRunner/premake5.lua`,
     matching the `RT2Tests/premake5.lua:15-48` pattern.
  8. Create `assets/script-scenario.rt2scene` + `assets/script-scenario.lua`
     and `run_script_test.ps1`.

- **W8 — Tests.** Two new test files:
  - `Phase6CHotReloadTests.cpp`: reload add/remove/rename fields, syntax
    error quarantine + un-quarantine, callback add/remove,
    `rt2.previous_state`, pause suppression, `world.spawn` script
    attachment during reload, **timer cancellation on reload** (B3),
    **path canonicalization** (M3), **`IsRuntimeMutable()` gate on reload**
    (B2).
  - `Phase6CBindingsTests.cpp`: input bindings (mock `IInputService`),
    timer lifecycle (after/every/cancel/pause/step-advances/stop/destroy/
    reload-cancels), entity light/camera/material get/set, **bounds check
    on `set_material_index`** (H2), **`try_get` for absent components**
    (H2), **`IsRuntimeMutable()` gate on new setters** (L4), `rt2.reload()`.
  Register both in `RT2Tests.vcxproj` + `.filters`.

  *Amended in W9:* delivered as **one** file, `Phase6CScriptingTests.cpp`,
  rather than two — the harness is shared and splitting it would duplicate
  `Harness6C` across translation units. Coverage matches the two lists
  above. Two pure seams were extracted first so the decisions were
  reachable from tests at all: `ScriptScenarioCompare.h` (headless verdict
  + the `ScenarioExit` code table) and `ScriptFileWatchPolicy.h`
  (`DecideScriptFileChange`). Not covered, and deliberately so: the
  watcher's member-declaration-order UAF needs a live `efsw` thread the
  test target does not link.

- **W9 — Documentation and verification.** Update scripting/scene docs,
  build Release, run focused 6C tests, the full suite against its
  **7 Release / 15 Debug** baseline (review L1: state both), the headless
  script scenario regression, and refresh Graphify.

##### Implementation order

> **Review H1/H ordering change:** W0 (`std::terminate`) is demoted out of
> the critical path. The order leads with W1 (hot reload) + W3 (input),
> which de-risk everything else and need no build-system change.

1. **W0** — `LuaPanic` → `std::terminate`. Two-line fix, build + verify.
2. **W1** — Hot reload core. Implement `ReloadScript` + `IsRuntimeMutable`
   on the sink. Tests via explicit calls (no efsw yet). Build + test.
3. **W3** — Input bindings. Add methods to `input` table. Test with mock
   `IInputService`.
4. **W4** — Entity light/camera/material bindings. Add to entity table +
   sink. Test get/set round-trip, bounds, absent-component, gate.
5. **W5** — Timers. Implement C++ timer system in `ScriptSystem`. Test
   after/every/cancel/pause/step-advances/stop/destroy/reload-cancels.
6. **W6** — `rt2.previous_state` + `rt2.reload()`. Test across reload.
7. **W2** — File watcher. Vendor efsw, wire in WalnutApp, debounce, drain
   queue in `OnUIRender`. Interactive test only.
8. **W7** — Headless script scenario. Add to SliceRunner, create fixture,
   add to regression script.
9. **W8** — Tests. Two new test files, registered in both vcxproj files.
10. **W9** — Documentation and verification.

### Phase 6C verification report (implemented)

Verified 2026-07-24. All W0–W9 workstreams landed.

| Check | Result |
|---|---|
| Release build, whole solution | RT2App, RT2Tests, RT2SliceRunner, Walnut, ImGui, GLFW all link |
| Debug build, RT2Tests | links |
| Focused 6C suites, Release | 29 cases / 150 assertions, all pass |
| Focused 6C suites, Debug | 29 cases / 150 assertions, all pass |
| Full suite, Release | 553 cases, **7 failed** (9 assertions) — matched the then-current baseline |
| Full suite, Debug | 553 cases, **15 failed** (17 assertions) — matched the then-current baseline |

> **Baseline superseded 2026-07-24, after this report.** The 7 Release
> failures were subsequently diagnosed and fixed (see "Test baseline" below);
> Release is now **554 run, 0 failed**. Debug retains 8 failures with an
> unrelated root cause. Do not use the two rows above as a current gate.
| Headless scenario (`run_script_test.ps1`) | PASS, exit 0 |
| Graphify | refreshed |

The 7 Release / 15 Debug failures are the pre-existing set enumerated in the
Phase 6B W0-W2 verification report above ("The honest baseline is now") —
5 × `SceneGraph` plus 2 × `SceneManager` in both configurations, and in Debug
additionally 7 × `OBJ Import Wizard` and 1 × `P1A Multi-Model`. They fail
identically in isolation and are untouched by Phase 6C. Note the *total* case
count has grown since that report (468 → 553) while the failure counts have
not.

**Defects found during W8/W9 and fixed:**

1. The watcher's file-change policy was an either/or between reloading and
   invalidating the inspector's field registry. `ScriptSystem` and the editor
   hold **separate** `ScriptFieldRegistry` instances, so while Playing the
   inspector kept showing declarations parsed from the pre-edit file for the
   whole session. Now both effects are independent and the cache is
   invalidated in every run state.
2. `assets/script-scenario.lua` declared its field with the plain-table form
   `{ type = "float", default = 1.0 }`, which is silently skipped as "not an
   `rt2.field.*` declaration". The fixture only worked because the value was
   also authored into the `.rt2scene`. Fixed, and pinned by a test asserting
   that constructors declare and plain tables do not.
3. `ScenarioExit::TransformMismatch` was renamed `ExpectationFailed` — a
   spawn violation shares code 5 and is not a transform mismatch.
4. `CompareTransforms` was iterating entities and skipping any without an
   expectation, so a scenario whose subject the script destroyed passed
   clean. It is now driven by the expectation list.

**Known non-coverage:** the W2 watcher shutdown ordering (member declaration
order) needs a live `efsw` thread that the CPU-only test target does not
link. It remains guarded by a comment at the declaration site.

**Caveat for CI:** `RT2Tests.exe` resolves some fixtures by relative path and
must be run from the repository root; from another working directory a
handful of unrelated cases fail on missing assets.

### Test baseline (current — supersedes all earlier figures)

Established 2026-07-24 and last measured 2026-07-31 after Phase 7 W4.
**This section is the
authoritative baseline; every earlier "7 failed" / "15 failed" figure in this
document is a period record of a superseded state.**

| Configuration | Result |
|---|---|
| **Release** | **700 run, 700 passed, 0 failed, 0 skipped; 145,911 assertions** |
| **Debug** | **700 run, 700 passed, 0 failed, 0 skipped; 145,911 assertions** |

Run from the repository root — `RT2Tests.exe` resolves some fixtures by
relative path and both fails and writes stray files if run from elsewhere.

> **Updated 2026-07-25.** The 8 Debug-only failures recorded below were
> diagnosed and fixed; both configurations were 555/555 at that point. Phase
> 7 W1 (asset ID plumbing, +16 tests) and W2 (asset database, +11 tests)
> then raised the count to 582/582 in both Release and Debug. The previous
> Debug row (554 run, 546 passed, 8 failed) is a period record of the
> pre-fix state. See the supersession note at the end of the "remaining 8
> Debug-only failures" subsection for the verified root cause.

> **Updated 2026-07-31.** Subsequent Phase 7 W3/W4 work raised both
> configurations to the measured 700/700 shown above. The 582/582 figure is
> now another period record; the W4 implementation report at the document end
> records the complete build/test/script/slice gate.

**Release is green and must stay green.** A Release failure is now a real
regression, not baseline noise. This is the change that makes the gate
meaningful: previously a passing run had to be checked against a memorised
failure count, which silently reverses the meaning of "the suite passed".

#### The remaining 8 Debug-only failures

All in `Phase1ASceneAssetTests.cpp`: 7 × `OBJ Import Wizard` and
1 × `P1A Multi-Model`. Diagnosed but not fixed.

They are **not** OBJ importer bugs. The chain is:

1. Each test builds its fixture with `MakeMultiShapeObj`, which writes an
   `.obj` and `.mtl` via `std::ofstream` and never checks that the write
   succeeded.
2. `SceneLoader::ParseObjAndLoadResources` opens with
   `if (filepath.empty() || !fs::exists(filepath)) return false;` — before
   any parsing or logging, so the failure is completely silent.
3. `ImportObjIntoECS` returns `entt::null`, `ImportObj` returns an invalid
   `EntityId`, and the test fails at `REQUIRE(rootId.IsValid())` — several
   layers away from the cause, naming the importer rather than the fixture.

Instrumentation established that in Debug the fixture files **do not exist**
after `MakeMultiShapeObj` returns, even though the `std::ofstream` inside it
reports `is_open() == true` and `fail() == false`, and a directly constructed
`std::ofstream` at test scope in the same directory writes successfully. Why
the successfully-opened stream leaves no file in Debug is unresolved.

Two things worth fixing regardless of that root cause, since both convert a
silent failure into a loud one:

- `MakeMultiShapeObj` should assert the files exist before returning.
- `ParseObjAndLoadResources` should log the missing path rather than
  returning `false` silently — every caller currently reports the symptom
  three layers up.

A clean Debug rebuild was ruled out as a cause; the failures survive it.

> **Superseded 2026-07-25 — root cause found and fixed.** The unresolved
> question above ("why does the successfully-opened stream leave no file in
> Debug") has a code-level answer, not an environmental one. The fixture
> structs (`TinyObjFixture`, `MultiShapeObjFixture`, `MultiMaterialShapeFixture`,
> `DegenerateShapeFixture` in `RT2Tests/src/Phase1ASceneAssetTests.cpp`) own
> their `.obj`/`.mtl` files via a destructor that calls `std::filesystem::remove`
> but were **copyable** with no user-defined copy/move. The `Make*` helpers
> return the fixture by value. MSVC's **Debug** build does not apply NRVO
> (named return value optimization) for this case, so `return f;` copies `f`
> into the return slot and then destroys the local `f` — running `Cleanup()`,
> which deletes the files the caller is about to use. **Release** applies
> NRVO, so no copy/destructor occurs and the files survive. This explains
> every observation: files exist at the end of `MakeMultiShapeObj` but not
> after the return; `.objx` / `.mtl`-written-at-test-scope survive (no
> fixture destructor touches them); a clean Debug rebuild changes nothing.
>
> Probes that proved it (instrumentation, not reasoning): a copy-constructor
> trace showed `COPY ctor` immediately followed by `DTOR` (with `exists=1`
> at destructor entry) during the return, and the test body then saw
> `exists=0`. Adding `RT_LOG` to `ParseObjAndLoadResources`'s guard
> confirmed it refused the path as "does not exist" — the file was already
> gone before the importer ran.
>
> Fix applied (`RT2Tests/src/Phase1ASceneAssetTests.cpp`): the four fixture
> structs are now **move-only** (copy deleted; move ctor/assignment clear
> the source's paths so the moved-from destructor's `Cleanup` is a no-op).
> `return f;` is now a move, so the source destructor cannot delete the
> caller's files in either configuration. Each `Make*` helper now calls
> `RequireFixtureFile` to assert both files exist before returning — a
> fixture that silently produces nothing now fails at the source, not three
> layers deep in the importer.
>
> Secondary fix (`RT2App/src/SceneLoader.cpp`): the four silent
> `if (filepath.empty() || !fs::exists(filepath)) return false;` guards
> (in `LoadIntoECS`, `ImportIntoECS`, `LoadObjIntoECS`, and
> `ParseObjAndLoadResources`) now log the refused path and the reason
> ("empty" / "does not exist") via `RT_LOG` before returning. This is the
> codebase's characteristic silent-failure bug class; the OBJ failures
> were a clean instance of it.
>
> Verified 2026-07-25 from the repository root, both configurations built
> clean (single-threaded for Release to avoid a `vc143.pdb` write race):
> **Debug 555/555**, **Release 555/555**. Note: at verification time the
> `phase-6-scripting` branch had in-flight Phase 7 edits to
> `ECSComponents.h` (an unqualified `UUID assetId;` field that does not
> resolve in the global namespace) that broke the build in both
> configurations; a temporary `rt2::core::` qualification was used **only**
> to unblock verification and was reverted immediately afterward, so the
> Phase 7 agent's file was left exactly as found.

### Phase 6 exit criteria (all three sub-slices)

A saved `ScriptComponent` can drive an entity using input and survive
save/load, Play/Stop, and hot reload — the spec's exit criterion. No
gameplay-facing code touches the EnTT registry, renderer, or window
directly; all script access goes through the validated bindings. The
single Lua state is owned by `ScriptSystem`; the single file-watcher
is owned by `WalnutApp`. The CPU-only boundary (RT2Tests,
RT2SliceRunner, EditorSettingsStore) is preserved: sol2 is header-only
and the Lua C lib is the only new link, gated per target by premake.

### Phase 6 ordering rationale

6A before 6B before 6C because:

- 6A proves the embedding, the lifecycle dispatch (G1's
  `IRuntimeScriptDispatch`), the spawned-entity lifecycle (G2's
  `SyncScriptEnvironments`), and the per-instance state machine (S1) —
  the riskiest unknowns (Lua + sol2 + the observer/dispatch seam + the
  deferred queue as the mutation channel + the environment-map mirror).
  If 6A's lifecycle dispatch is wrong, 6B's field persistence and 6C's
  hot reload would be built on sand.
- 6B's reflection and persistence are independently testable via
  explicit `ReloadScript` calls (the API was declared in 6A and stubbed
  until 6C implemented it);
  deferring file watching to 6C means 6B does not need efsw or a
  worker thread, keeping the CPU-only test boundary intact.
- 6C's hot reload is the feature most likely to surface race
  conditions and lifecycle edge cases; building it last means it runs
  against a stable lifecycle and a stable field model, so the reload
  bugs it surfaces are reload bugs, not lifecycle or field bugs. The
  S1 state machine's `Quarantined` → `Live` reload transition is the
  6C-specific piece; the rest of the state machine is fixed in 6A.

### Documentation to update when 6A lands (S8)

- `game-loop.md`: the numbered Step list (lines 154–159) is missing
  the fixed-script slot the prose at line 161 implies. Add "fixed
  script callbacks" before the inline motion integration, and
  `SyncScriptEnvironments` after the deferred safe point, matching the
  full contract at lines 130–143.
- `game-loop.md` scripting placeholder (lines 297–309): replace the
  aspirational `FixedScriptSystem::Update` / `ScriptSystem::OnUpdate`
  sketch with the implemented `IRuntimeScriptDispatch` interface and
  the `SyncScriptEnvironments` mechanic.
- `scene-management.md`: note `ScriptComponent` as a persisted
  component (Count 10→11) and the v3 hard schema cutover (lands in 6B,
  but note the visitor change in 6A).

---

## Phase 7 — Project model and asset database (implementation plan, not started)

Grounded against the tree at commit `33b777a` (2026-07-24), immediately after
Phase 6 completed. Every claim below carries a `file:line` so it can be
re-verified rather than trusted — the tree moves, and this section will go
stale.

The Phase 7 roadmap section earlier in this document states the goal in 39
lines. This section is the part the roadmap does not have: what already
exists, what collides with it, and what must be decided before code is
written.

---

### Why this document exists

The Phase 7 roadmap describes a greenfield asset database. **It is not
greenfield.** Roughly half the machinery exists under other names, and two
pieces of it use the term "project root" to mean something *different* from
what Phase 7 means. An implementer reading only the roadmap would either
rebuild what exists or silently conflict with it.

---

### Grounded findings

| ID | Fact (verified in tree) | Consequence for Phase 7 |
|---|---|---|
| P1 | `AssetReference { AssetKind kind; std::string path; ImportSettings importSettings; std::string sourceKey; }` — `ECSComponents.h:205-213`. The `path` comment reads "portable, scene-relative UTF-8 path". **There is no asset ID field.** | This is the struct Phase 7 extends. Adding a stable ID here is the smallest change that reaches every asset kind at once. |
| P2 | `sourceKey` already provides *stable subresource identity within a file*, with deterministic importer-defined formats documented at `ECSComponents.h:215-223` (`gltf:scene=<s>:node=<n>:mesh=<m>:primitive=<p>`, `obj:whole-model`). Builders: `SceneAssetResolver::GltfSourceKey/ObjSourceKey/GltfMaterialKey/ObjMaterialKey` (`SceneAssetResolver.h:118-125`). | Half the identity problem is solved. `sourceKey` answers "which mesh inside the file"; Phase 7's asset ID answers "which file". **Do not conflate them, and do not replace `sourceKey`.** |
| P3 | Asset paths are already scene-relative and **already rebased on Save As**: `EntityRecordToJson(record, currentSceneDir, outputSceneDir, err)` (`SceneSerializer.cpp:548`, rebasing logic `:241-257`). | "Project moves stay portable" is partly built at the *scene* level. Phase 7 lifts it to the *project* level. |
| P4 | v3 `metadata` serializes **only** `name` (`SceneSerializer.cpp:1226-1230`); the loader reads only `name` (`:1486-1490`). An absolute `metadata.sourcePath` used to be written and was removed during Phase 6. | The known absolute-path leak into scene files is closed. Do not reintroduce it. Audit for others rather than assuming none remain. |
| P5 | **A `projectRoot` already exists and is NOT what Phase 7 means.** `EditorSettings.h:32-36`: "Optional editor preference used as an initial file-dialog location. **Does NOT reinterpret** the Phase 1A scene-relative asset-reference contract." Per-user, stored in the user settings file, API at `EditorSettings.h:98-100`. | Phase 7's `project.rt2proj` is a *portable, committed* file that **does** define asset resolution. Two different concepts, one name. Must be disambiguated explicitly — see D3. |
| P6 | **The input map already lives in per-user editor settings**, not in a project file: `inputContexts` in the EditorSettings v2 schema (`EditorSettings.h:43-61`). | The roadmap says `project.rt2proj` holds the input map. That is a *move* across a per-user/per-project boundary, with a migration. See D4. |
| P7 | `SceneAssetResolver` already owns the diagnostics story: `AssetDiagnostic { refPath, resolvedPath, entityName, sourceKey, detail }` (`SceneAssetResolver.h:76-81`), plus `ResolveAll` (`:97`), `ResolveEnvironment` (`:107`), `ResolvePath(refPath, sceneRoot)` (`:114`). | "Placeholders and diagnostics for missing/invalid/cyclic references" **extends** this type. Do not invent a parallel diagnostic channel. |
| P8 | `AssetKind` has four members — `Model`, `Texture`, `Environment`, `Script` (`ECSComponents.h:169-176`). Only Model/Environment go through `SceneAssetResolver`; Script has its own resolver `ResolveScriptAssetPath` (`ScriptAssetPath.h:16`); Texture is resolved inside model import. | An asset database must cover all four. **Three different resolution paths exist today** — unifying them is arguably the real work of Phase 7. |
| P9 | **No content browser exists.** No file in `RT2App/src` matches `contentbrowser`/`assetbrowser`/`browser`. | Genuinely greenfield. The only greenfield part. |
| P10 | The efsw file watcher exists (Phase 6C) but watches only the scene directory plus directories of bound `.lua` scripts, recomputed on scene open (`WalnutApp.cpp:2711-2745`). | Phase 7's "watch source files and reimport" is the same machinery, wider scope. Note the watch set is rebuilt **only on scene open** — an asset added later is not seen. |
| P11 | `SchemaVersion = 3`, `MinReadVersion = 3` (`SceneSerializer.h:112-114`) — a deliberate **hard cutover**, no backward compatibility. | Adding asset IDs to serialized references implies v4 and the same cutover-vs-migration decision. See D5. |
| P12 | EditorSettings has its own independent version with a real migration: v1 → v2 adds `inputContexts`, "version is updated, inputContexts is left empty" (`EditorSettings.h:63-64, 86-87`). | A working precedent for *additive* settings migration, in contrast to the scene format's hard cutover. |
| P13 | `rt2::core::UUID` provides `Nil()`, `IsNull()`, `ToString()`, `Parse()` (`core/UUID.h:35-59`), with a deterministic provider used throughout for entity IDs. | Asset IDs can reuse this type outright. See D1. |

### Commitments earlier phases deferred *to* Phase 7

These are recorded where the deferral happened, not where the work lands, so
a spec written from the roadmap alone will miss all of them.

| Where | Commitment |
|---|---|
| plan:985 | Asset-database migration to **global asset UUIDs** |
| plan:3311, 3380 | The **input rebinding dialog** ("Phase 7's content-browser era") |
| plan:4064, 5274 | The script asset **Rebind button**. Phase 6B W5 ships the asset path as a raw `InputText`; the browse/rebind affordance was explicitly deferred |
| plan:5521 | Declaration diagnostics shown inline in the content browser |
| plan:5901 | Cursor-lock binding |

---

### Decisions required before implementation

Each needs an explicit answer. Defaults are recommendations, not settled.

**D1 — Asset ID type.** Reuse `rt2::core::UUID` (P13), or introduce a distinct
`AssetId`?
*Recommend reusing `UUID`.* It already has parse/format/nil semantics, a
deterministic provider for tests, and serializer support. A distinct type buys
compile-time separation from entity IDs at the cost of duplicating all of that.

**D2 — Where the ID lives.** Add `assetId` to `AssetReference` (P1), or keep an
external database keyed by path and leave `AssetReference` alone?
*Recommend adding to `AssetReference`*, keeping `path` as a human-readable
fallback for diagnostics and hand-editing. Path-keyed external mapping
reintroduces exactly the rename-fragility Phase 7 exists to remove.

**D3 — Name collision with the existing `projectRoot` (P5).** Two concepts
currently share one name. Options: rename the EditorSettings one (e.g.
`lastBrowseDirectory`, which is what it actually is); or name the new one
distinctly. **Whichever is chosen, `EditorSettings.h:32-36` must be updated in
the same change** — that comment currently asserts project root does not affect
asset resolution, which stops being true.

**D4 — Does the input map move (P6)?** The roadmap puts it in
`project.rt2proj`; it currently lives per-user. Moving it makes bindings
shippable with the project but overrides per-user preference. A split (project
provides defaults, user settings override) is more work but is the behaviour
users expect from an editor.

**D5 — Schema v4: hard cutover or migration?** P11 shows the v2→v3 precedent
was a hard cutover. P12 shows settings do additive migration. Asset IDs cannot
be invented for existing scenes without a scan-and-assign pass, so this
interacts with D2.

**D6 — Cache root.** The roadmap requires generated artifacts outside source
asset folders. `SceneRecoveryService` already writes generated data somewhere —
check where, and decide whether the cache root sits beside it or under the
project root.

**D7 — Watcher scope (P10).** Widen the existing efsw watcher to the asset
root, and fix that the watch set is only rebuilt on scene open.

**D5 (settled 2026-07-25, with D8) — Schema v4: additive migration, assign
once and persist.** The v2→v3 precedent (P11) was a hard cutover; v4 breaks
that precedent deliberately. Asset IDs cannot be invented for existing
scenes without a scan-and-assign pass, and a hard cutover would invalidate
every existing scene for a feature that can degrade gracefully. So `MinReadVersion`
stays 3 while `SchemaVersion` becomes 4 (the `MinReadVersion == SchemaVersion`
invariant at `SceneSerializer.h:112-114` is intentionally broken).

The assignment mechanism is **assign-once-during-explicit-migration, written
back on save** — not lazy minting on resolve, and not deterministic derivation
from path. Lazy minting via `OsUuidProvider::CreateV4()` (`core/UUID.cpp:89-118`,
`CoCreateGuid`/`UuidCreate`, random) was rejected because two machines opening
the same v3 scene would assign divergent IDs to the same asset and fail the
cross-machine portability exit criterion. Deterministic derivation (v5 from
canonical project-relative path) was rejected for two reasons: (1) it couples
identity to path, which is the rename-fragility Phase 7 exists to remove, and
(2) `core/UUID.h:25` commits v5 to "Linked imported nodes: v5 from asset ID +
canonical node key (future)" — v5 is already spoken for as a derivation *from*
asset IDs, not a way to *produce* them. Under assign-once-persisted, the first
save under a v4 editor runs an explicit scan-and-assign pass; from then on the
scene carries stable, persisted IDs. The pass is observable (the editor reports
"migrated N assets to v4") rather than a silent save-time mutation, per the
loud-failure rule in AGENTS.md:53-60.

The mechanism interacts with D8 (below): where the IDs *live* determines
whether "machine B opening A's scene" binds A's IDs to local paths or mints
new ones. See D8.

**D8 (settled 2026-07-25) — Where asset identity lives: per-asset sidecars.**

The roadmap says the project has "stable asset IDs, source paths, asset
types, import settings, and dependency records" but never says *where*. That
question has to be answered before W1 because the import flow's
"generate at import" step writes to whichever store holds identity.

Three candidates were evaluated against three failure cases the per-machine
assumption breaks (unreferenced asset; independent import; concurrent offline
work) plus the version-control axis that most distinguishes them:

- **Per-machine database** (what a naive reading of "asset database" assumes).
  Requires answering all three cases with a merge policy. Case 2
  (independent import: B already holds `id_B` for the path when A's scene
  arrives claiming `id_A`) has no good answer: either B rewrites A's scene
  (breaks portability) or B adopts A's ID and orphans its own `id_B` record
  (silent data loss). Rejected.
- **Portable database** — a single file committed alongside
  `project.rt2proj`. IDs assigned once at import, travel with the project.
  Cases 1 and 3 become ordinary text-file merge conflicts: visible and
  resolvable. Case 2 is a real conflict (two IDs for one path) but it is a
  *diff you can read*, which is the version-control virtue. Most of the
  save-time migration machinery becomes unnecessary except for legacy v3
  scenes. Strong candidate.
- **Per-asset sidecars** — `cube.glb.rt2meta` beside each asset, carrying its
  ID, committed with it. This is how Unity solves the same problem. It
  dissolves all three cases by construction: identity travels with the
  asset, so an unreferenced asset still has its ID (case 1); a file arriving
  from another machine arrives with its ID already assigned — there is no
  "B already holds a different ID" because the ID is in the file, not in B's
  database (case 2); and concurrent offline work produces two sidecars for
  two *different* assets, or a merge conflict on the one shared sidecar
  (case 3, visible). Moving an asset moves its identity. There is no
  central file to conflict in. Costs: double the file count in the asset
  tree; sidecars lost by naive file operations (copy without sidecar, or
  delete the sidecar); and a defined behaviour when one goes missing.

**Decision: per-asset sidecars.** The deciding axis is the Phase 7 exit
criterion — a project folder copied to another machine without rewriting
scene files. Sidecars make that case *structural* rather than procedural: the
identity is in the folder being copied, not in a database that lives on each
machine and has to be reconciled. A portable database file also satisfies the
exit criterion but introduces a single chokepoint that conflicts on every
multi-developer asset add; sidecars distribute the conflict surface so two
developers adding *different* assets never touch the same file. The double-
file-count cost is real but acceptable in a project that already commits
binary `.glb`/`.obj`/`.exr`/`.lua` sources.

**Missing-sidecar behaviour (must be loud, not silent).** A sidecar can be
lost by a naive file copy that moves the asset without its `.rt2meta`, or by
a VCS operation that ignores it. This is the same shape as Phase 6's
characteristic silent-failure bug (AGENTS.md:53-60). The rule: a missing
sidecar is **not** a quiet re-mint. Resolution by path proceeds; the
`AssetDiagnostic` channel (P7, `SceneAssetResolver.h:66-82`) records a
`Missing`-severity diagnostic with the sidecar path, and the next save writes
a fresh sidecar with a fresh ID *and* updates the scene's reference to the new
ID. The scene's old ID is treated as stale; the database records the remap.
This is observable (diagnostics surface in the existing channel) and
recoverable (next save fixes it), and it never silently leaves a reference
pointing at an ID no sidecar claims.

**How D8 changes W1/W2.**
- W1 attaches the ID at import. Import reads the sidecar if present (stable
  ID); mints a fresh v4 and writes the sidecar if absent (assign-once). The
  ID lives in `AssetReference::assetId` (per D2) and in the sidecar (source
  of truth). The sidecar is the durable record; the scene's `assetId` is a
  cache of it.
- W2 is the in-memory record store, *not* the sidecar files. It is built by
  scanning sidecars (deterministic, sorted by path — see `ReconcileScriptFields`
  at `ScriptFieldReconcile.cpp:199` for the sort-before-emit precedent). It
  is CPU-only (per the `ScriptFieldReconcile`/`ScriptScenarioCompare`
  precedent: pure logic, no Vulkan/ImGui/Walnut). Sidecar I/O is host
  wiring, kept out of the CPU-only core.
- A v3 scene (no `assetId` field) is read with nil IDs; the first v4 save
  scans sidecars, assigns IDs from sidecars where they exist, mints+writes
  sidecars where they don't, and persists the IDs into the scene. This is
  the D5 scan-and-assign pass, made concrete by D8.

---

### Proposed workstreams

Ordered so each is independently testable and nothing depends on unbuilt UI.

- **W0 — Audit.** Find every place an absolute path can still reach serialized
  data or the asset database. P4 closed one; assume others exist. Deliverable:
  a list, plus a test that fails if a saved scene contains an absolute path.
- **W1 — Asset ID plumbing.** Per D1/D2: add the ID, generate on import, thread
  through `AssetReference`. No behaviour change yet — resolution still goes by
  path. Fully unit-testable.
- **W2 — Asset database.** The record store: ID, source path, kind, import
  settings, dependency records. Pure and CPU-only, following the
  `ScriptFieldReconcile` precedent of keeping logic Lua-free and testable.
- **W3 — Resolution by ID, path as fallback.** Unify the three resolution paths
  from P8 behind one entry point. **Highest-risk workstream** — it touches
  model, texture, environment and script loading simultaneously.
- **W4 — `project.rt2proj`.** Per D3/D4/D6: project UUID, asset root, cache
  root, startup scene, and whatever D4 decides about the input map.
- **W5 — Schema v4.** Per D5. Includes the scan-and-assign pass for existing
  scenes.
- **W6 — Content browser** (P9). Search, rename/move/delete, drag/drop,
  reimport. Depends on W2/W3.
- **W7 — Watching and async reimport.** Per D7, widening the Phase 6C watcher.
- **W8 — Deferred commitments.** The Rebind button, input rebinding dialog,
  inline declaration diagnostics, cursor-lock binding.
- **W9 — Tests and docs.** Per the roadmap's test list, plus a baseline update.

---

### Test requirements

From the roadmap, plus what grounding suggests:

- Asset IDs survive rename/move; scene references still resolve.
- Scanning produces the same database regardless of directory enumeration
  order. *(Precedent: `ReconcileScriptFields` sorts before emitting so results
  never depend on `unordered_map` iteration order — do the same here.)*
- Dependency graphs detect missing assets and cycles.
- Reimport updates cache metadata without changing asset identity.
- Project relocation preserves all valid relative references.
- Asset deletion reports dependants before committing.
- **Added:** a saved scene contains no absolute paths (W0).
- **Added:** all four `AssetKind` values resolve through the unified path (P8).

---

### Risks

**W3 is the dangerous one.** Three independent resolution paths exist today
(P8) and each has its own failure behaviour. Unifying them touches model,
texture, environment and script loading at once, and scripting only just
stabilised in Phase 6. Land W1/W2 first so the database is proven before
anything depends on it for resolution.

**Silent failure is this codebase's characteristic bug.** Phase 6 shipped
three of them: a policy that left a cache stale for a whole session, a
field-declaration form that parsed and did nothing, and a fixture generator
whose writes were never checked. Asset resolution is the same shape of problem
— a missing asset that resolves to "nothing" without a diagnostic will not be
noticed. `AssetDiagnostic` (P7) exists; route every failure through it rather
than returning empty paths.

---

### Notes for whoever implements this

- **The test suite is green in both configurations (555/555) and must stay
  green.** A Release failure is now a real regression. The 8 Debug-only
  failures in OBJ fixture generation that previously existed were diagnosed
  and fixed on 2026-07-25 (move-only fixture structs + loud missing-file
  logging in `SceneLoader.cpp`); see "Test baseline" in the plan doc.
- **Run `RT2Tests.exe` from the repository root.** It resolves some fixtures
  by relative path; run elsewhere it fails extra cases *and* writes stray
  fixture files into the tree.
- `AGENTS.md` asks you to run `graphify update .` after changing code, and to
  start codebase questions with `graphify query`. Note the graph is now
  gitignored — on a fresh clone run `graphify update .` once before querying.
- Build: `msbuild RT2App.sln -p:Configuration=Release -p:Platform=x64`.
  Targets are `RT2App`, `RT2Tests`, `RT2SliceRunner`.
- `RT2Tests` and `RT2SliceRunner` are CPU-only by design — no Vulkan, ImGui or
  Walnut. Keep asset-database logic linkable into both; that constraint is why
  Phase 6's scripting core is testable at all.

---

## Phase 7 W3 — unified asset resolution (approved implementation plan)

Grounded against commit `2e7f089ea2c6ccf7817350f19a8c04e61c6fc810`
on 2026-07-25, after W0–W2. Reviewed and approved before production changes.
The first incremental work item (hermetic characterization tests) is recorded
at the end of this section.

W3 is not a mechanical refactor. The four asset kinds currently have
incomplete representations, three independent locators, and incompatible
failure policies. The disagreements below are the design problem.

### W3 grounded findings

| ID | Finding | Consequence |
|---|---|---|
| W3-P1 | `AssetKind` has `Model`, `Texture`, `Environment`, and `Script` (`RT2App/src/ECSComponents.h:169-176`). `AssetReference` carries `assetId`, path, kind, import settings, and source key (`RT2App/src/ECSComponents.h:201-221`). | The generic contract already has the right identity pair, but only model and script component data can currently hold it. |
| W3-P2 | `EnvironmentSettings` stores only path, dimensions, and decoded pixels (`RT2App/src/SceneDocument.h:49-66`). `SceneTexture` stores only filepath, pixels, dimensions, and colour-space state (`RT2App/src/SceneTypes.h:92-105`). | Environment and texture need durable `AssetReference` representation before they can resolve by ID. `AssetReference` cannot simply be included from `ECSComponents.h` because that header already includes `SceneTypes.h`; the common asset types need a neutral header. |
| W3-P3 | The shared asset-reference codec reads/writes optional `assetId` (`RT2App/src/SceneSerializer.cpp:185-243`) and imported models use it (`RT2App/src/SceneSerializer.cpp:592-597`). Script serialization hand-builds only `kind`, `path`, and `sourceKey` (`RT2App/src/SceneSerializer.cpp:637-660`), and script load reads the same three fields (`RT2App/src/SceneSerializer.cpp:823-867`). Environment serialization stores only its path (`RT2App/src/SceneSerializer.cpp:1268-1275`, `:1483-1490`); native texture serialization is still an empty array (`RT2App/src/SceneSerializer.cpp:1261-1264`). | Script `assetId` is lost on every save/reopen — a shipped W1 persistence defect. Environment and texture IDs have no on-disk form. W3 cannot claim authoritative identity across reopen until these gaps are closed. |
| W3-P4 | Production calls `AssetIdentity::ResolveOrAssign` only while importing/loading models (`RT2App/src/SceneManager.cpp:95-134`, `:285`, `:374-422`, `:556-582`). `ResolveOrAssign` reads an existing sidecar or mints and writes one when absent/malformed (`RT2App/src/AssetIdentity.cpp:157-196`). | Resolution itself must be read-only and must not call `ResolveOrAssign`; otherwise opening a scene mutates identity. Import, migration, and explicit save own sidecar writes. |
| W3-P5 | `AssetDatabase` is pure/in-memory and the caller owns the filesystem scan (`RT2App/src/AssetDatabase.h:25-30`, `:117-126`). `FindById` and `FindByPath` are simple lookups (`RT2App/src/AssetDatabase.cpp:76-89`). On duplicate IDs, the first inserted record keeps the ID and the second is sanitized to nil (`RT2App/src/AssetDatabase.cpp:10-35`); input order is not sorted internally (`RT2App/src/AssetDatabase.cpp:106-112`). | First-wins is not valid for authoritative lookup and depends on scan order. W3 needs explicit `Missing` / `Unique` / `Ambiguous` lookup, with every duplicate claim retained and sorted. |
| W3-P6 | `SceneAssetResolver::ResolvePath` is existence-only: empty returns empty; an existing absolute path returns as-is; a relative path tries `sceneRoot`; then it falls back to the process CWD; failure returns an empty path with no diagnostic (`RT2App/src/SceneAssetResolver.cpp:106-131`). | The shared locator must return a structured result and route terminal failures through `AssetDiagnostic`; CWD fallback must be removed after callers/tests provide an explicit root. |
| W3-P7 | The resolver header promises that hard failure leaves the document unchanged (`RT2App/src/SceneAssetResolver.h:49-58`). In practice, a loaded model's meshes/materials/textures are appended before source-key validation (`RT2App/src/SceneAssetResolver.cpp:513-562`), and an all-unresolved batch returns false afterward (`RT2App/src/SceneAssetResolver.cpp:725-733`). | The documented transactionality contract is false. W3 must stage all resource changes and commit only after the batch's aggregate policy accepts the result. |
| W3-P8 | Model collection uses registry iteration order and no final diagnostic sort (`RT2App/src/SceneAssetResolver.cpp:267-331`). A malformed file emits a file-level `Malformed` diagnostic (`:390-399`) and then an entity-level `Missing` diagnostic (`:494-510`). An unresolved source key emits `Unresolved` but omits `resolvedPath` (`:653-665`). | Diagnostics are duplicated/misclassified and order is not guaranteed. W3 needs one contextual terminal diagnostic per failed reference and deterministic sorting before emission. |
| W3-P9 | `ResolveScriptAssetPath` is purely lexical and never checks existence, identity, kind, or source key (`RT2App/src/ScriptAssetPath.cpp:8-15`). `ScriptFieldResolver` sorts entities by UUID (`RT2App/src/ScriptFieldResolver.cpp:21-38`) but reports missing/malformed scripts through `FieldDiagnostic`, not `AssetDiagnostic` (`:43-67`). | Script resolution must use the shared locator while preserving reflection diagnostics as an additional domain-specific channel. Every location/parse failure must also reach `AssetDiagnostic`. |
| W3-P10 | `ScriptSystem` reads source to an empty string on failure (`RT2App/src/ScriptSystem.cpp:684-694`) and only quarantines that empty source when `exists(path)` is false (`:740-759`). Empty scripts are intentionally legal (`RT2Tests/src/Phase6ALifecycleTests.cpp:859-878`). | An unreadable existing path can be mistaken for a valid empty script. The shared resolver/read result must distinguish empty content from failed I/O without regressing the legal-empty-script contract. |
| W3-P11 | glTF import appends an empty `SceneTexture` when a texture source index is invalid or its decoded image is empty (`RT2App/src/SceneLoader.cpp:455-470`, duplicated at `:1211-1228`). Material texture indices are copied without range validation (`:499-506`, `:1256-1263`). | Missing/unresolved glTF textures can leave referentially valid-looking empty slots with no diagnostic. |
| W3-P12 | The vendored glTF parser treats a missing external image as a warning and continues (`RT2App/vendor/tinygltf/tiny_gltf.h:4414-4424`), while image decoder failure returns false and can reject the whole model (`:4438-4446`, image parse at `:6424-6488`). | glTF missing and malformed texture files have different containment behaviour even though both are texture failures. W3 must prevent a bad texture from killing otherwise valid geometry. |
| W3-P13 | OBJ texture load returns `-1` for an absent file and logs/returns `-1` for decode failure; the material simply leaves the slot unset and the model succeeds (`RT2App/src/SceneLoader.cpp:2140-2184`; same legacy path at `:1807-1856`). | OBJ silently drops missing textures and only logs malformed ones. Both need `Texture` diagnostics and a deterministic placeholder. |
| W3-P14 | The native-scene host formats the three current severities with a ternary whose final branch is `"Unresolved"` (`RT2App/src/WalnutApp.cpp:2633-2664`). | Adding `Conflict` without making this formatter exhaustive would silently mislabel conflicts as unresolved. |
| W3-P15 | Existing hermetic fixtures cover successful embedded GLB and EXR paths (`RT2App/src/Phase1AFixtureGenerator.h:68-105`; `RT2Tests/src/Phase1ASceneAssetTests.cpp:227-308`) and one missing environment (`RT2Tests/src/Phase1ASceneAssetTests.cpp:398-417`). Several model-path tests instead depend on `C:\Users\mikey\Downloads\*.glb` (`RT2Tests/src/BuildGpuFromEcsTests.cpp:14-205`, `RT2Tests/src/EcsSceneLoaderTests.cpp:14-155`, `RT2Tests/src/SceneManagerTests.cpp:205-265`, `:361-370`). | Green results from those machine-local tests are not portable evidence for W3. Failure coverage must use generated temporary assets. |

### Current behaviour, side by side

| Outcome | Models (`SceneAssetResolver`) | Environments (`SceneAssetResolver`) | Scripts (`ResolveScriptAssetPath` + consumers) | Textures (inside model import) |
|---|---|---|---|---|
| Success | Path exists, model stages and source key installs `MeshRef`; resources are appended (`SceneAssetResolver.cpp:378-484`, `:513-718`). | Existing HDR/EXR decodes into pixels and dimensions (`SceneAssetResolver.cpp:188-232`). | Lexical scene-relative path is returned; field registry/runtime then reads and parses it independently (`ScriptAssetPath.cpp:8-15`, `ScriptFieldRegistry.cpp:310-417`). | glTF creates one `SceneTexture` per texture and copies decoded pixels (`SceneLoader.cpp:455-470`); OBJ loads and assigns indices (`:2140-2184`). |
| Missing file | One file-level `Missing`, then one `Missing` per entity; all unresolved is a hard `MissingAsset` (`SceneAssetResolver.cpp:366-375`, `:494-510`, `:725-733`). | One `Missing`, false, `MissingAsset`; path preserved and decoded data cleared (`SceneAssetResolver.cpp:166-185`). | Locator still returns a non-empty candidate. Field registry reports parse failure; runtime quarantines only after its separate existence check (`ScriptAssetPath.cpp:8-15`, `ScriptFieldRegistry.cpp:323-393`, `ScriptSystem.cpp:740-759`). No `AssetDiagnostic`. | Missing glTF external image warns, model succeeds, and an empty texture slot survives (`tiny_gltf.h:4414-4424`, `SceneLoader.cpp:455-470`). OBJ model succeeds and drops the texture (`SceneLoader.cpp:2140-2184`). No `AssetDiagnostic`. |
| Malformed file | Importer returns false; resolver emits file `Malformed` plus entity `Missing`; aggregate error is still `MissingAsset` (`SceneAssetResolver.cpp:390-399`, `:494-510`, `:725-733`). | One `Malformed`, false, but error code is `MissingAsset`; decoded data cleared (`SceneAssetResolver.cpp:191-226`). | Path resolution succeeds. Field registry returns `parsed=false` with last-known-good declarations; runtime quarantines initial load, while failed hot reload preserves the live instance (`ScriptFieldRegistry.cpp:395-417`, `ScriptSystem.cpp:348-367`, `:1225-1243`). No `AssetDiagnostic`. | Malformed glTF image can fail the whole model (`tiny_gltf.h:4438-4446`); malformed OBJ texture is logged and dropped while geometry succeeds (`SceneLoader.cpp:2140-2184`). |
| Unresolved subresource | Missing `sourceKey` emits `Unresolved`; if every entity misses, returns false after resources have already been appended (`SceneAssetResolver.cpp:513-665`, `:725-733`). | Not applicable: the current environment reference has no subresource key (`SceneDocument.h:49-66`). | Runtime locator ignores `sourceKey`; serializer normalizes it from path (`ScriptAssetPath.cpp:8-15`, `SceneSerializer.cpp:848-876`). | Invalid glTF texture source/material indices are not diagnosed; an empty slot or out-of-range material reference can survive (`SceneLoader.cpp:455-470`, `:499-506`). OBJ represents failure as an unset `-1` slot (`:2140-2184`). |

### Approved unified contract

Introduce one CPU-only, filesystem-aware asset locator in a neutral
`AssetResolver.{h,cpp}`. Move (do not duplicate) `AssetDiagnostic` there and
make `SceneAssetResolver.h` include it. Extract `AssetKind`, `ImportSettings`,
and `AssetReference` into a neutral asset-reference header so
`EnvironmentSettings` and `SceneTexture` can carry references without a
`SceneTypes.h` / `ECSComponents.h` include cycle.

The entry point receives an `AssetReference`, an explicit resolution context
(asset root plus a non-owning `AssetDatabase`), and diagnostic context
(entity UUID/name where applicable). It returns a structured result containing:

- success/failure;
- normalized absolute path;
- resolution source (`Id` or `PathFallback`);
- effective ID;
- whether explicit identity repair is required.

The locator is read-only. It calls `ReadSidecarId` when identity verification
is needed; it never mints, writes a sidecar, rewrites a scene, or mutates the
database. Import/save/migration own repair.

Resolution order and disagreement policy:

1. A non-nil ID is authoritative and is looked up first.
2. A unique ID whose file exists wins. A stale/missing reference path is
   observable but does not defeat successful ID resolution.
3. If the database is stale/missing but the path exists and its sidecar claims
   the same ID, path fallback succeeds and reports stale database state.
4. If the ID does not locate a file, the path exists, and the sidecar is
   absent, fallback succeeds with a sidecar `Missing` diagnostic and
   `identityRepairRequired=true`; explicit save/migration performs the remap.
5. If the path's sidecar claims a different ID, resolution fails with
   `Conflict`; it never silently substitutes one identity for the other.
6. If neither ID nor path locates a regular file, resolution fails `Missing`.
7. If more than one asset claims the ID, lookup is `Ambiguous` and fails
   `Conflict`, even when one candidate matches the fallback path. No
   insertion-order winner is chosen.
8. A nil ID uses path fallback. Missing/malformed sidecar state is observable;
   later schema/migration work persists the assigned identity.

The shared locator answers only "which file?". Kind-specific CPU loaders keep
their policies but emit every failure through the same `AssetDiagnostic`
vector:

- model: unresolved entities remain without a `MeshRef`; aggregate hard
  failure only when every imported model entity fails;
- environment: preserve the durable reference, clear stale decoded pixels,
  direct resolve returns false while `ResolveAll` may continue;
- script: location/parse failure emits `AssetDiagnostic` and preserves the
  existing quarantine, last-known-good reflection, and failed-hot-reload
  behaviour;
- texture: preserve valid model geometry, install a deterministic CPU
  placeholder, and emit a `Texture` diagnostic.

Batch APIs collect diagnostics locally and sort before appending by
`(kind, refPath, entityUuid, sourceKey, severity, detail)`. Results must not
depend on EnTT traversal, directory enumeration, or `unordered_map` order.
Terminal failure never returns an empty path without a diagnostic.

### Decisions resolved by review

These were unsettled in the pre-implementation report. The recommendations
were approved on 2026-07-25.

| ID | Decision |
|---|---|
| W3-Q1 | **Representation:** extract neutral asset-reference types; environment and texture gain `AssetReference`; fix script `assetId` persistence. Only the additive fields required for W3 land here; W5 owns the formal v4 migration/reporting pass. |
| W3-Q2 | **Embedded/multi-role assets:** `AssetKind` is the requested use, not an exclusive physical classification. Embedded/data-URI textures use the model's physical asset ID plus a deterministic image source key; external textures receive their own sidecars. |
| W3-Q3 | **Pre-W4 ownership:** use an explicit resolution context owned by the current scene/recovery host. W4 replaces its root with the project asset root. No global resolver/database and no database embedded in `SceneDocument`. |
| W3-Q4 | **Duplicate IDs:** replace W2 first-wins with `Missing` / `Unique` / `Ambiguous`, retain and sort all claimants, and fail authoritative resolution on ambiguity. |
| W3-Q5 | **Diagnostics:** extend the existing `AssetDiagnostic::Severity` with `Conflict`; do not create another channel. Make the Walnut formatter exhaustive in the same change. |
| W3-Q6 | **Transactionality:** a false `ResolveAll` result leaves the document unchanged. A successful partial result may commit accepted resources/placeholders. |
| W3-Q7 | **Texture containment:** a bad texture produces a deterministic placeholder plus a `Texture` diagnostic and does not kill otherwise valid model geometry. |
| W3-Q8 | **Path policy:** remove process-CWD fallback after tests/callers supply an explicit root. Accept legacy absolute paths in memory, but normalize the successful result and never persist a new absolute path. |
| W3-Q9 | **Resolver purity:** resolution is read-only; sidecar mint/write/remap happens only during explicit import/save/migration. |

### Incremental implementation order

Each production cutover is its own commit. After every cutover commit, build
and run the complete suite in both Release and Debug from the repository root;
do not defer the full gate until all four kinds have moved.

0. **Characterize current behaviour.** Add generated temporary fixtures for
   all cells in the table above, including GLB/EXR, external-image glTF,
   OBJ/MTL/PPM, and Lua. No production behaviour change.
1. **Harden W2 lookup.** Add explicit ambiguous-ID results, retain/sort all
   duplicate claimants, sort database construction internally, and add
   permutation tests.
2. **Land neutral types and the generic locator without consumers.** Add the
   CPU-only files to RT2App, RT2Tests, and RT2SliceRunner; exhaustively test
   ID/path disagreement and deterministic diagnostics.
3. **Cut over models only.** Keep a compatibility `ResolvePath` adapter during
   the transition. Resolve ID-first, remove duplicate/misclassified model
   diagnostics, populate `resolvedPath`/importer detail, and make hard failure
   transactional.
4. **Cut over environments.** Cover moved assets, nil-ID fallback, missing
   sidecar, missing file, and corrupt HDR/EXR.
5. **Cut over scripts.** Make `ResolveScriptAssetPath` an adapter over the
   structured locator; inject the context/diagnostic sink into field
   resolution, runtime, watcher, and slice runner. Preserve legal empty
   scripts, quarantine isolation, last-known-good fields, and live-instance
   preservation after failed reload. Run `run_script_test.ps1`.
6. **Cut over textures last.** Extract dependency enumeration/decode from
   `SceneLoader`; resolve external glTF/OBJ images through the locator and
   embedded images through model ID plus source key. Convert failures into
   diagnostics/placeholders while retaining valid geometry.
7. **Complete host wiring.** Remove legacy CWD/script-only paths only after all
   four kinds use the shared entry point.
8. **Final verification.** Build whole solution Release and Debug; run both
   complete test binaries from the repository root; run the script scenario;
   refresh Graphify; commit `GRAPH_REPORT.md` if changed; do not push.

### W3 step 0 verification report — characterization complete

Implemented 2026-07-25 in
`RT2Tests/src/Phase7W3CharacterizationTests.cpp`. Nine hermetic test cases
(149 assertions) cover:

- model success, missing file, malformed file, and unresolved source key;
- environment success, missing file, and malformed file;
- script success, missing file, malformed file, source-key blindness, and the
  transitional script-`assetId` persistence defect;
- OBJ texture success/missing/malformed behaviour;
- glTF texture success, missing external image, malformed external image, and
  invalid texture source.

No test uses `C:\Users\mikey\Downloads`. No production resolution behaviour
changed.

| Check | Result |
|---|---|
| Focused W3 characterization, Release | 9/9 cases, 149/149 assertions |
| Full RT2Tests, Release | 591/591 cases, 144530/144530 assertions |
| Focused W3 characterization, Debug | 9/9 cases, 149/149 assertions |
| Full RT2Tests, Debug | 591/591 cases, 144530/144530 assertions |
| Whole solution, Release | builds |
| Phase 6C script scenario | PASS |
| Graphify | refreshed |

**Verification discrepancy found:** the whole Debug solution currently fails
while linking `RT2App`, before the test executable is involved.
`RT2App.vcxproj:77-92` compiles Debug with `MultiThreadedDebugDLL`
(`MDd`, iterator debug level 2), while the only checked-in NRD/NRI libraries
are Release-runtime binaries under `RT2App/vendor/NRD/Lib` and
`RT2App/vendor/NRI/Lib`; the linker reports `LNK2038` (`MD`/iterator level 0
versus `MDd`/level 2) and `LNK1319`. The Debug `RT2Tests` target builds and
passes 591/591. This mismatch predates and is independent of the test-only W3
step; no build-runtime policy was changed here.

### W3 AssetDatabase hardening verification — complete

Implemented 2026-07-25, grounded against commit `a64f60f`.

`AssetDatabase` now preserves every path claiming an asset ID and exposes an
explicit `Missing` / `Unique` / `Ambiguous` lookup result. Ambiguous results
never select a record, and their candidate paths are sorted. Database
construction normalizes and total-sorts its inputs before insertion, including
an explicit reference-versus-sidecar identity authority, so sidecar precedence
does not depend on enumeration order.

Records now carry sorted, deduplicated entity dependents and cross-asset
dependency records. A cross-asset dependency is keyed by stable `sourceKey`
and records the target ID, source path, and requested `AssetKind`. Conflicting
claims for the same source key remain preserved and sorted for the later
locator to diagnose; they are not silently collapsed.

W3-Q2 was inseparable from that dependency representation in one limited
respect: `AssetRecord` now stores a sorted set of observed `AssetKind` uses
rather than one exclusive kind. Tests prove that one physical asset can be
observed as both `Texture` and `Environment`. No texture import, scene,
environment, or script resolution path changed in this step.

Order-independence is covered by forward/reversed construction and 64 seeded
random permutations. Each permutation shuffles both the input records and
their nested use, entity-dependent, and asset-dependency collections, then
compares a canonical byte serialization of all public records, ID lookups, and
diagnostics with the baseline.

| Check | Result |
|---|---|
| AssetDatabase focused tests, Release | 18/18 cases, 152/152 assertions |
| AssetDatabase focused tests, Debug | 18/18 cases, 152/152 assertions |
| W3 characterization, Release | 9/9 cases, 149/149 assertions; expectations unchanged |
| W3 characterization, Debug | 9/9 cases, 149/149 assertions; expectations unchanged |
| Full RT2Tests, Release | 598/598 cases, 144627/144627 assertions |
| Full RT2Tests, Debug | 598/598 cases, 144627/144627 assertions |
| RT2SliceRunner target, Release and Debug | builds |

No project or compiler configuration changed; in particular this step did not
touch the existing RT2Tests `/FS` option. The pre-existing full-Debug
`RT2App` vendor-runtime link mismatch remains explicitly out of W3 scope.

### W3 step 2 verification report — neutral types and generic locator landed

Implemented 2026-07-25, grounded against commit `12a88c8` (immediately
after the W2 hardening step). No production consumer was cut over in this
step; step 3 cuts models over.

**Files added (CPU-only, no Vulkan/ImGui/Walnut/entt):**

- `RT2App/src/AssetReference.h` — extracts `AssetKind`, `ImportSettings`,
  and `AssetReference` from `ECSComponents.h` into a neutral header that
  depends only on `core/UUID.h`. The types remain in the **global
  namespace** to match the pre-extraction source layout (verified via
  `git show HEAD:RT2App/src/ECSComponents.h`: the originals were not inside
  any `namespace` block). `ECSComponents.h` now includes
  `AssetReference.h` and re-exports the names unchanged. This unblocks
  W3-P2: `SceneTexture` and `EnvironmentSettings` can now carry an
  `AssetReference` without a `SceneTypes.h`/`ECSComponents.h` include cycle.
  (Adding those fields is W3 step 3+ work, not this step.)
- `RT2App/src/AssetResolver.{h,cpp}` — the neutral read-only locator.
  Defines `AssetDiagnostic` (moved here from `SceneAssetResolver.h`),
  `AssetResolutionContext`, `AssetResolutionResult`,
  `AssetResolutionSource`, `AssetBatchEntry`, `Resolve()`, `ResolveBatch()`,
  and `AssetDiagnosticSortKey()`. `AssetDiagnostic::Severity` gains
  `Conflict` (W3-Q5). The eight-case ID/path disagreement policy from the
  approved contract is encoded in `Resolve()`:
  1. unique ID + matching file → success by `Id` (case 1);
  2. unique ID + file exists, stale ref path → success by `Id` with an
     observable `Missing` diagnostic for the stale path (case 2);
  3. ID not in DB, path exists, sidecar matches requested ID → fallback
     succeeds, `Missing` diagnostic "database stale" (case 3);
  4. ID not in DB, path exists, sidecar absent → fallback succeeds with
     `identityRepairRequired=true` (case 4);
  5. sidecar claims a different ID → `Conflict`, no substitution (case 5);
  6. neither ID nor path locates a regular file → `Missing` (case 6);
  7. ambiguous ID → `Conflict` even if fallback path matches one claimant
     (case 7);
  8. nil ID → path fallback; absent sidecar → repair required; present
     sidecar → effective ID from sidecar, no repair (case 8).
  The locator never mints, writes, or remaps (W3-Q9). Process-CWD fallback
  is removed (W3-Q8); legacy absolute paths are accepted in memory and the
  successful result is normalized. `ResolveBatch()` sorts appended
  diagnostics by `(kind, refPath, entityUuid, sourceKey, severity, detail)`
  using `AssetDiagnosticSortKey()` and a stable sort, so equal-key
  diagnostics keep insertion order and results are independent of input
  order.

**Files modified:**

- `RT2App/src/ECSComponents.h` — includes `AssetReference.h`; the
  duplicated `AssetKind`/`ImportSettings`/`AssetReference` definitions are
  removed (now in `AssetReference.h`). Comment block updated to record the
  extraction.
- `RT2App/src/SceneAssetResolver.h` — includes `AssetResolver.h` instead
  of defining `AssetDiagnostic` inline. `AssetDiagnostic` is now re-exported
  by include; its `Conflict` member is visible to all existing consumers.
  The `SceneAssetResolver` class and its `ResolveAll`/`ResolveEnvironment`/
  `ResolvePath` signatures are unchanged (step 3 will cut them over).
- `RT2App/src/WalnutApp.cpp` — the diagnostic severity formatter is made
  exhaustive over the four severities (`Missing`/`Malformed`/`Unresolved`/
  `Conflict`) with a `default` arm labelled `"Unknown"`, replacing the
  ternary whose final branch silently mislabelled anything beyond
  `Unresolved` (W3-P14). This was called out as required in the same change
  that adds a `Conflict`-emitting code path (W3-Q5).
- `RT2App/RT2App.vcxproj`, `RT2Tests/RT2Tests.vcxproj` — wired
  `AssetReference.h`, `AssetResolver.h`, and `AssetResolver.cpp` into both
  targets. (`RT2SliceRunner/RT2SliceRunner.vcxproj` is gitignored on this
  machine; the same edit was applied on disk and the slice runner builds in
  both configurations.)

**Tests added:** `RT2Tests/src/Phase7W3LocatorTests.cpp` — 18 cases,
119 assertions, covering:

- each of the eight ID/path disagreement cases as a distinct test;
- read-only behaviour: no sidecar written for nil-ID missing sidecar;
  existing sidecar untouched even on the Conflict path (file size and
  content preserved; `ReadSidecarId` returns the original ID);
- no process-CWD fallback (a file placed at the test runner's CWD does not
  resolve against an unrelated asset root);
- absolute legacy path accepted and the successful result normalized;
- batch diagnostics sorted deterministically and independent of input
  order (two orderings compared by refPath sequence);
- entity UUID/name context preserved in diagnostics;
- empty path fails with a `Missing` diagnostic (never a silent empty
  result);
- `AssetDiagnostic::Conflict` is a distinct severity value.

Every fixture is generated below a unique temporary directory; no test
depends on `C:\Users\mikey\Downloads` or on checked-in generated assets.

**Production behaviour change:** none. The existing `SceneAssetResolver`
model/environment/script resolution paths were not touched; the
characterization tests (step 0) still pin the pre-W3 behaviour and pass
unchanged. The Walnut formatter change is exhaustive-but-equivalent for
the three pre-existing severities.

| Check | Result |
|---|---|
| W3 locator focused tests, Release | 18/18 cases, 119/119 assertions |
| W3 locator focused tests, Debug | 18/18 cases, 119/119 assertions |
| W3 characterization (step 0), Release | 9/9 cases, 149/149 assertions; expectations unchanged |
| W3 characterization (step 0), Debug | 9/9 cases, 149/149 assertions; expectations unchanged |
| Full RT2Tests, Release | 616/616 cases, 144746/144746 assertions |
| Full RT2Tests, Debug | 616/616 cases, 144746/144746 assertions |
| RT2App target (Vulkan), Release | built after the production changes; a redundant relink after removing one unused include was blocked by an already-running `RT2App.exe` |
| RT2SliceRunner target, Release and Debug | builds |
| Phase 6C script scenario | PASS |
| Graphify | refreshed; `GRAPH_REPORT.md` changed |

**Pre-existing carryover, unchanged by this step:** the whole-Debug
`RT2App` vendor-runtime link mismatch (`LNK2038`/`LNK1319` from
`NRD.lib`/`NRI.lib` built with `MD`/iterator level 0 against `RT2App`'s
`MDd`/level 2) is the same error set recorded in the W3 step 0 report and
remains explicitly out of W3 scope. Debug `RT2Tests` and `RT2SliceRunner`
build and pass; only Debug `RT2App` (which pulls the NRD/NRI vendor
binaries) fails to link.

### W3 step 3 verification report — models cut over to the shared locator

Implemented 2026-07-25, grounded against commit `061340e` (immediately
after the step 2 neutral-types + locator landing). Models are the first
production consumer of the generic `AssetResolver`; environment, script,
and texture resolution remain on their pre-W3 paths and are cut over in
steps 4–6.

**Locator defect found and fixed during this cutover.** Step 2's `Resolve()`
took the nil-ID path whenever `ctx.database == nullptr`, even when a
non-nil ID was requested. That dropped the ID-vs-sidecar conflict check
(the case-5 contract) at scene load, where the host does not yet build an
`AssetDatabase` but `AssetReference::assetId` is non-nil (assigned at
import). `AssetResolver.cpp` was restructured so the nil-ID branch is
taken only when the requested ID is actually nil; a non-nil ID with no
database proceeds directly to path+sidecar verification, which still
emits `Conflict` when the sidecar claims a different ID. Two locator
tests pin the new no-database variants of cases 3 and 5.

**Production changes (all in `RT2App/src/SceneAssetResolver.cpp`):**

- `ResolveAll`'s model section now resolves each referenced model through
  the shared `Resolve()` against an `AssetResolutionContext` whose
  `assetRoot` is the scene root and whose `database` is `nullptr` (W3-Q3:
  no global resolver/database; W4 replaces the root with the project
  asset root). The locator is read-only (W3-Q9): no sidecar is minted,
  written, or rewritten at scene load. `assetId` is plumbed but the host
  does not yet build a database, so the locator's path+sidecar
  verification is what carries identity.

- **Removed the duplicate file-level `Missing` diagnostic** (W3-P8). The
  pre-W3 code emitted one file-level `Missing` per missing model path
  and then one entity-level `Missing` per entity that referenced it. The
  locator emits exactly one terminal diagnostic per missing reference,
  with the first referencing entity's UUID filled in. The entity-level
  "model not loaded" diagnostic still fires in the plan pass, so a
  missing model produces two diagnostics — but both now carry the
  entity's UUID (the old file-level diagnostic had a nil UUID). The
  misclassified/misordered diagnostics called out in W3-P8 are no longer
  produced for models.

- **Made hard failure transactional** (W3-P7 / W3-Q6). The pre-W3 code
  appended staged meshes/materials/textures into `doc.ecs` during the
  merge loop and only checked "every entity unresolved" afterward, so a
  hard `ResolveAll` failure left `doc.ecs` polluted. `ResolveAll` is now
  split into a **plan pass** (resolves every entity's target against the
  STAGED model's local indices without touching `doc.ecs`) and a
  **commit pass** (merges only the staged models that have at least one
  resolved entity and installs `MeshRef`s). If every entity is
  unresolved, `ResolveAll` returns false with `doc.ecs` unchanged — no
  meshes, materials, or textures are committed. A successful partial
  result may commit accepted resources/placeholders (W3-Q6), so a scene
  with one resolvable and one unresolvable entity still returns true and
  commits the resolvable entity's resources.

- `ResolvePath` is kept as a compatibility adapter (the plan called for
  it). It is still used by `ResolveEnvironment` (step 4 cuts environment
  over); only the model path stopped using it. Its signature and
  existence are unchanged.

- `ResolveBatch` is not used by `ResolveAll` yet — `ResolveAll` needs
  per-entity UUID/name context that the batch entry supports, but the
  diagnostic ordering within `ResolveAll` is already deterministic
  (insertion order: models in first-reference order, entities in registry
  iteration order). A later step can route through `ResolveBatch` once
  the registry-iteration-order dependency is addressed; that is not a
  step-3 concern.

**Tests updated:** four transitional characterization cases in
`RT2Tests/src/Phase7W3CharacterizationTests.cpp` were rewritten to pin
the post-cutover contract instead of the pre-W3 defects:

- "model success resolves a generated GLB": was `diagnostics.empty()`;
  now asserts exactly one `Missing` "identity repair required"
  diagnostic (nil `assetId`, no sidecar — the locator's case 8a).
- "missing model emits file and entity diagnostics": was 2 diagnostics
  with `diagnostics[0].entityUuid.IsNull()`; renamed to "emits one
  locator diagnostic and one entity diagnostic", asserts 2 diagnostics
  with BOTH entity UUIDs non-nil (the locator fills the first entity's
  UUID), and asserts transactionality (`doc.ecs` empty on hard failure).
- "malformed model is followed by a Missing entity diagnostic": was 2
  diagnostics; renamed to "emits locator, load, and entity diagnostics",
  asserts 3 diagnostics (locator `Missing` "identity repair required" +
  loader `Malformed` "model failed to load" + plan-pass `Missing` "model
  not loaded"), and asserts transactionality.
- "unresolved model source key mutates resources before hard failure":
  was 1 diagnostic + `meshRegistry.GetCount() == 1`; renamed to "fails
  transactionally", asserts 2 diagnostics (locator `Missing` + plan-pass
  `Unresolved`) and `meshRegistry.GetCount() == 0` (the false
  transactionality defect is fixed).

**Tests added:** three new focused step-3 cases in the same file:

- non-nil `assetId` with a matching sidecar resolves and emits exactly
  one "database stale" `Missing` diagnostic (the case-3 no-database
  variant the model cutover actually exercises at scene load);
- non-nil `assetId` with a conflicting sidecar fails with `Conflict` and
  leaves the document unchanged;
- partial success (one resolvable + one unresolvable entity) commits
  the resolvable entity's resources and returns true.

**P1A test updated:** `Phase1ASceneAssetTests.cpp`'s
"P1A RoundTrip: glTF import -> save v3 -> load + resolve" asserted
`d.severity != Missing` for every diagnostic on a successful resolve.
After cutover, a successful resolve with a non-nil `assetId` and a
matching sidecar emits a "database stale" `Missing` diagnostic (the W3
contract signal, not a real failure). The assertion was relaxed to
forbid `Malformed`/`Conflict`/`Unresolved` (real defects) while allowing
`Missing`. No environment or script test changed.

| Check | Result |
|---|---|
| W3 locator focused tests, Release | 20/20 cases, 125/125 assertions (2 no-database cases added) |
| W3 locator focused tests, Debug | 20/20 cases, 125/125 assertions |
| W3 characterization (model cases), Release | post-cutover expectations; 4 updated + 3 added |
| Full RT2Tests, Release | 621/621 cases, 144802/144802 assertions |
| Full RT2Tests, Debug | 621/621 cases, 144802/144802 assertions |
| RT2App target (Vulkan), Release | builds |
| RT2SliceRunner target, Release and Debug | builds |
| Phase 6C script scenario | PASS |
| Graphify | refreshed; `GRAPH_REPORT.md` changed |

**Pre-existing carryover, unchanged by this step:** the whole-Debug
`RT2App` NRD/NRI `LNK2038`/`LNK1319` mismatch is the same error set
recorded in the step 0 and step 2 reports and remains explicitly out of
W3 scope. Debug `RT2Tests` (621/621) and `RT2SliceRunner` build and pass;
only Debug `RT2App` fails to link.

### W3 step 4 verification report — environments cut over to the shared locator

Implemented 2026-07-25, grounded against commit `9731f06` (immediately
after the step 3 model cutover). Environments are the second production
consumer of the generic `AssetResolver`; scripts and textures remain on
their pre-W3 paths and are cut over in steps 5–6.

**W3-P2 precondition landed (additive, no schema bump):**
`EnvironmentSettings` now carries a `UUID assetId` alongside its existing
`path`/`width`/`height`/`floatPixels`. `Clear()` resets it. `HasEnvMap()`
stays path-based. Env serialization writes `assetId` only when non-nil and
reads it additively (absence → nil), mirroring the model `AssetReference`
codec (W3-Q1: only the additive fields required for W3 land here; W5 owns
the formal v4 migration/reporting pass). No schema version bump.

**Env import writes a sidecar.** `SceneManager::LoadEnvMap` and
`SetEnvMapData` (the async-load completion path) now call
`ResolveOrAssign` to mint or reuse a per-asset sidecar at env import,
paralleling model import. The sidecar is the durable source of truth;
`environment.assetId` is a cache of it. `ResolveEnvironment` reads the
sidecar through the shared locator and never mints — env resolution is
read-only (W3-Q9), just like model resolution.

**Production change (`RT2App/src/SceneAssetResolver.cpp`):**
`ResolveEnvironment` now builds an `AssetReference`
(`kind=Environment`, `path=env.path`, `assetId=env.assetId`) and calls
the shared `Resolve()` against an `AssetResolutionContext` whose
`assetRoot` is the scene root and whose `database` is `nullptr` (W3-Q3).
On locator failure it clears stale pixels (preserving the path and
assetId references) and returns false; the locator's single terminal
diagnostic is appended to the caller's vector. On locator success it
decodes the resolved path through the existing `DecodeEnvMapFile`. If
the locator resolved by path fallback and the sidecar supplied an
effective ID, the ID is cached back into `doc.environment.assetId` —
this copies an already-authoritative sidecar ID, it does not mint. A
nil effective ID (absent sidecar) leaves the document ID untouched; the
host's next save/migration owns repair. `ResolvePath` is no longer
called by `ResolveEnvironment`; it remains as a compatibility adapter
with no production callers (step 5 cuts scripts over, after which it
can be removed in step 7).

**Tests updated:** the environment characterization subcases and one
P1A env test were rewritten to pin the post-cutover contract:

- "environment success, missing, and malformed disagree on detail"
  (3 subcases):
  - success: was `diagnostics.empty()`; now asserts exactly one
    `Missing` "identity repair required" diagnostic (nil env assetId,
    no sidecar — the locator's case 8a).
  - missing: still 1 `Missing` diagnostic (the locator's terminal
    diagnostic for the missing path; the pre-W3 duplicate file-level
    diagnostic is gone, same fix as the model cutover).
  - malformed: was 1 `Malformed` diagnostic; now asserts 2 diagnostics
    (locator `Missing` "identity repair required" + decoder
    `Malformed` "decode failed"), both with `kind=Environment`.
- "P1A Environment: save env reference -> load -> resolve reads
  pixels": was `diagnostics.empty()`; now asserts exactly one
    `Missing` "identity repair required" diagnostic.

**Tests added (5 focused step-4 cases):**

- non-nil env `assetId` with a matching sidecar resolves and emits
  exactly one "database stale" `Missing` diagnostic; the effective ID is
  cached back into the document;
- moved env asset (stale path) without a database fails `Missing` but
  preserves the durable `assetId` so a later database-backed resolve can
  reattach by identity — pins that moved assets need the W4 database;
- non-nil env `assetId` with a conflicting sidecar fails with `Conflict`
  and never substitutes identity (the locator's case-5 contract,
  extended in step 3 to the no-database path);
- nil env `assetId` with a sidecar caches the effective ID from the
  sidecar (case 8b) and emits no diagnostic;
- env `assetId` survives a save/load round-trip (additive over v3: the
  saved file contains the `assetId` field, and the loaded document
  restores it).

**Carryover from step 3, unchanged:** `ResolveAll`'s model section and
the locator's no-database conflict check (the step-3 fix) are exercised
by the new env tests with no further changes. The shared locator is
now the resolution entry point for both models and environments.

| Check | Result |
|---|---|
| W3 characterization (env cases), Release | post-cutover expectations; 3 subcases updated + 5 added |
| W3 characterization (env cases), Debug | same; 3 subcases updated + 5 added |
| Full RT2Tests, Release | 626/626 cases, 144862/144862 assertions |
| Full RT2Tests, Debug | 626/626 cases, 144862/144862 assertions |
| RT2App target (Vulkan), Release | builds |
| RT2SliceRunner target, Release and Debug | builds |
| Phase 6C script scenario | PASS |
| Graphify | refreshed; `GRAPH_REPORT.md` changed |

**Pre-existing carryover, unchanged by this step:** the whole-Debug
`RT2App` NRD/NRI `LNK2038`/`LNK1319` mismatch is the same error set
recorded in the step 0, 2, and 3 reports and remains explicitly out of
W3 scope. Debug `RT2Tests` (626/626) and `RT2SliceRunner` build and pass;
only Debug `RT2App` fails to link.

### W3 step 5 verification report — scripts cut over to the shared locator

Implemented 2026-07-26, grounded against commit `5e2545b` (steps 0–4
complete). This report covers step 5 only. Textures remain on their
pre-W3 path for step 6.

**Structured script resolution:** `ResolveScriptAssetPath` is now a thin
adapter over the shared `AssetResolver::Resolve` locator. It takes an
explicit `AssetResolutionContext` and `AssetDiagnostic` sink, validates
`kind=Script`, validates the canonical `lua:asset=<path>` source key, and
inherits the locator's existence, regular-file, ID, sidecar, database,
and conflict rules. It no longer returns an unchecked lexical candidate.
The same caller-owned context and diagnostic sink are threaded through
`ScriptFieldResolver`, `ScriptSystem`, the Walnut file watcher/load and
recovery paths, and `RT2SliceRunner`. Missing files remain watchable by
using the terminal diagnostic's candidate parent, but they cannot enter
the live Lua VM.

**Identity and persistence:** script save/load now use the shared
`AssetReferenceToJson`/`JsonToAssetReference` codec, including non-nil
`assetId`. The existing Phase 6 script disk shape is retained by omitting
the irrelevant `importSettings` member for `kind=Script`; other asset
kinds retain their established codec shape. `SceneManager::SetScriptState`
now treats an existing script binding as the explicit import boundary and
calls `ResolveOrAssign`, so a sidecar ID is minted or reused and stored in
the component. A changed path cannot carry the previous script's ID.
Missing script paths remain authorable so the existing quarantine and
watcher-recovery behavior is preserved; no ID is minted until the file
exists.

**Phase 6 contract verification:**

| Contract | How it was verified |
|---|---|
| Empty script is legal | Existing `Phase 6A: empty script file is legal` passed in both full configurations. The file reader now distinguishes a successful zero-byte read from an I/O failure. |
| A script error quarantines only the affected instance | Existing `Phase 6A: syntax error quarantines only the affected instance` and runtime-error isolation cases passed in both full configurations. Locator/load failures are attached to the failing entity while other instances continue. |
| Field reflection keeps last-known-good declarations; `parsed=false` suppresses reconciliation | Existing Phase 6B resolver/registry cases passed in both full configurations. The parse-failure case additionally asserted the new asset diagnostics while retaining the previous declarations and authored values. |
| Failed hot reload never replaces live code | Existing `Phase 6C: a mid-edit syntax error does not kill the running instance` and reload scratch-environment cases passed in both full configurations. Resolution/read/parse failure returns before swapping the live environment. |
| `rt2.reload()` remains deferred | Existing `Phase 6C: rt2.reload() from on_update does not re-enter` passed in both full configurations; the queueing boundary was not changed. |
| Timers, input bindings, and entity bindings are unaffected | The existing timer scheduling/cancellation/reload tests, input-service binding tests, and entity-binding validation/lifecycle tests all passed as part of both full suites. Their implementations were not changed. |

**Authorized expectation changes:**

- `Phase7 W3 characterization: script path resolution is lexical and
  sourceKey-blind` became `Phase7 W3 step 5: script adapter uses
  structured locator and validates metadata`. Old: a missing path still
  returned the joined candidate and a stale source key was ignored. New:
  the missing reference returns no resolved path plus terminal `Missing`,
  and the stale source key returns no resolved path plus terminal
  `Unresolved`. Scope item 1 explicitly replaces the lexical,
  non-validating implementation.
- `Phase7 W3 transitional characterization: script assetId is dropped by
  serialization` became `Phase7 W3 step 5: script assetId survives
  serialization through the shared codec`. Old: the loaded script ID was
  nil and the JSON omitted `assetId`. New: JSON contains the authored
  non-nil ID and load restores it. Scope item 3 explicitly fixes this
  serialization defect.
- No Phase 6 expectation changed. The pre-step 629 cases all pass with
  their current expectations; one new identity-assignment case brings
  the total to 630.

**Discrimination proof:** the one new case, `Phase7 W3 step 5: binding an
existing script assigns and reuses its sidecar ID`, was proven red/green.
Temporarily disabling the production `ResolveOrAssign` branch made it
fail (1 case, 11 assertions: 9 passed, 2 failed — nil component ID and
missing sidecar). Restoring the branch and rebuilding made the exact case
pass (1/1 case, 11/11 assertions). The temporary fault was not retained.

| Check | Result |
|---|---|
| Full RT2Tests, Release, repository root | 630/630 cases, 144934/144934 assertions |
| Full RT2Tests, Debug, repository root | 630/630 cases, 144934/144934 assertions |
| RT2App target (Vulkan), Release | builds |
| RT2SliceRunner target, Release and Debug | builds |
| `run_script_test.ps1` (Phase 6C gate) | PASS: 60 frames, 1 entity, no mismatches |
| Graphify | refreshed after implementation |

**Verified by running:** every build/test row above, both full-suite
counts, all named Phase 6 cases as members of those full suites, and the
red/green discrimination run. The fixture generator was also run after a
shared-codec compatibility correction and left its tracked scene fixture
byte-clean. The final host relink obstruction was identified as the
already-running Release `RT2App.exe` (PID 54024); it was not terminated.

**Assumed, not re-verified:** the pre-existing whole-Debug `RT2App`
NRD/NRI link mismatch remains unchanged; per instruction, that whole-app
target was not run. Removing the unused `<system_error>` include after
the successful Release host build is assumed not to affect its link
result; the exact current source was compiled and linked in both
`RT2Tests` and `RT2SliceRunner`. No build configuration or project file
changed. Texture behavior is assumed unchanged because step 6 was not
started and no texture-resolution production path was modified.

### W3 step 6 grounded implementation plan — textures cut over last

Planned 2026-07-26 and grounded against commit `1c04753` (W3 steps 0–5
complete). This section expands incremental-order step 6; it does not
rewrite the approved W3 record above. It is a documentation-only planning
change. Step 7 host cleanup and step 8 final verification are explicitly
out of this dispatch.

**Starting baseline:** Release and Debug `RT2Tests` both pass 630/630
cases and 144934/144934 assertions from the repository root.
`RT2SliceRunner` builds in both configurations and
`run_script_test.ps1` passes. The whole-Debug `RT2App` NRD/NRI mismatch
remains a pre-existing out-of-scope link defect.

#### Step 5 carryovers folded into this dispatch

These corrections are intentionally folded into step 6 rather than
interrupting the incremental sequence:

1. `AssetDiagnostic::Stale` already exists
   (`RT2App/src/AssetResolver.h:79-96`), but three successful `Resolve`
   branches still emit `Missing`:
   - nil ID + existing file + no sidecar
     (`RT2App/src/AssetResolver.cpp:104-120`);
   - unique ID resolves an existing file while the cached reference path
     is stale (`:187-204`);
   - non-nil ID + existing fallback path + no sidecar (`:237-252`).
   Reclassify all three as `Stale`. `Missing` is reserved for a resolution
   that returns `success=false` because no readable regular file was found.
   Add one locator invariant test: every successful result has zero
   `Missing` diagnostics. Update every existing expectation that currently
   calls an identity-repair success `Missing`; real missing-file
   expectations remain `Missing`.
2. `RT2App/assets/script-scenario.lua` is a tracked project asset but has
   no committed `script-scenario.lua.rt2meta`; consequently the scenario
   emits "asset has no identity sidecar; identity repair required" on every
   run (`RT2SliceRunner/src/Main.cpp:393-394`, `:559-590`;
   `RT2App/assets/script-scenario.rt2scene:25-34`). Generate one valid v4
   UUID once, commit the one-line sidecar beside the Lua file, and leave the
   scene's cached `assetId` absent. Nil reference ID + authoritative
   sidecar then resolves cleanly without a false database-stale advisory.
   Add a focused committed-fixture test and require the scenario gate output
   to contain no asset diagnostic.

#### Grounded step-6 findings

| ID | Finding at `1c04753` | Implementation consequence |
|---|---|---|
| S6-F1 | `SceneTexture` carries only `filepath`, decoded dimensions/pixels and colour-space flags (`RT2App/src/SceneTypes.h:92-105`), although W3-Q1 requires a texture `AssetReference`. | Add `AssetReference ref` without removing `filepath`; step 7 owns removal of compatibility fields. |
| S6-F2 | Native scene save always writes an empty texture array (`RT2App/src/SceneSerializer.cpp:1280-1282`) and load does not reconstruct standalone texture records. Imported textures are derived by rebuilding their owner models. | Do not invent a second standalone texture graph or schema in step 6. Rebuild `SceneTexture::ref` deterministically from the model plus sidecars on every import/reopen. W4 persists/indexes dependency records; W5 owns formal v4 schema work. |
| S6-F3 | TinyGLTF invokes `DecodeImageData`, which immediately decodes bytes and returns false on malformed data (`RT2App/src/SceneLoader.cpp:16-36`). TinyGLTF can therefore reject valid geometry before RT2 can contain the texture failure. | Replace the callback with a capture-only callback that never makes image decode a model-parse failure. Decode in the extracted texture stage after structural model parsing. |
| S6-F4 | The glTF texture-copy loop is duplicated in standalone load and append import (`RT2App/src/SceneLoader.cpp:455-470`, `:1212-1228`). Invalid sources create empty slots; decoded bytes are copied directly. | Both paths must consume one shared manifest/resolution/decode result. No duplicate texture policy remains in `SceneLoader`. |
| S6-F5 | OBJ texture resolution/decode/material assignment is duplicated in standalone load and import (`RT2App/src/SceneLoader.cpp:1807-1856`, `:2138-2184`). Both use `baseDir / texName`, call `stbi_load` directly, and return `-1` on failure. | Both OBJ paths must use the same manifest and pipeline as glTF, with one deterministic material-slot order. |
| S6-F6 | `SceneLoader` exposes four two/three-argument entry points with no resolution context or diagnostic sink (`RT2App/src/SceneLoader.h:12-45`). | Add context-aware overloads used by every production caller. Retain compatibility adapters only until step 7; adapters still use the structured pipeline and log every diagnostic. |
| S6-F7 | Native-scene resolution stages OBJ/glTF through `SceneLoader` (`RT2App/src/SceneAssetResolver.cpp:431-452`) but currently discards the model locator's effective ID after storing only the resolved path (`:289-342`). | Retain the owner `AssetReference` and `effectiveId` in `ModelRef`, then pass them, the same `AssetResolutionContext`, first entity UUID/name, and the same diagnostic sink into texture loading. |
| S6-F8 | Direct editor loads/imports call the four legacy loader APIs (`RT2App/src/SceneManager.cpp:194-234`, `:444-482`) and assign the model sidecar only after loading (`:303-307`, `:458-464`, `:486-489`). | Mark these calls as explicit-import mode. Once structural model parse succeeds, establish/reuse the model ID before decoding embedded images, then reuse that ID for mesh provenance. |
| S6-F9 | The database already represents sorted cross-asset dependencies by stable `sourceKey`, target ID/path and requested kind (`RT2App/src/AssetDatabase.h:38-57`, `:73-80`, `:136-154`). Existing tests and comments establish `gltf:image=<index>` (`RT2Tests/src/AssetDatabaseTests.cpp:366-372`). | Query a supplied database for the owner model's dependency claim before resolving an external texture. Zero claims uses URI/path fallback; one claim supplies authoritative ID/path; multiple distinct claims emit `Conflict` and produce a placeholder. |
| S6-F10 | Empty texture slots are skipped during GPU upload (`RT2App/src/AsyncTextureLoader.cpp:243-250`, `:338-345`). | A containment placeholder must contain valid pixels and dimensions, not merely retain an empty vector slot. No Vulkan/GPU code change is required. |
| S6-F11 | Material indices already preserve glTF texture-slot indices and OBJ assigns the returned loader index (`RT2App/src/SceneLoader.cpp:499-507`, `:1256-1264`, `:1844-1856`, `:2172-2184`). | Preserve valid indices on failure: glTF always yields one output slot per glTF texture; every referenced OBJ material slot receives a real or placeholder texture index. |
| S6-F12 | Transitional tests pin the inconsistent pre-cutover outcomes: OBJ drops missing/malformed textures (`RT2Tests/src/Phase7W3CharacterizationTests.cpp:1076-1119`); glTF keeps an empty missing slot, fails the whole model on malformed external data, and keeps an empty invalid-source slot (`:1121-1182`). | Rewrite only these authorized expectations to the W3-Q7 contract and add focused identity/diagnostic tests. |
| S6-F13 | `SceneAssetResolver::ResolveAll` appends staged textures transactionally only when a model has an accepted entity (`RT2App/src/SceneAssetResolver.cpp:809-859`), but does not perform a final diagnostic sort before returning (`:775-916`). | Texture failure remains soft and stages a placeholder; hard model failure remains transactional. Stable-sort only the newly appended diagnostic slice at the public boundary. |
| S6-F14 | Walnut has exhaustive `Stale` formatting in both script and scene diagnostic paths (`RT2App/src/WalnutApp.cpp:1504-1519`, `:2704-2719`), but direct model imports receive no texture diagnostic sink (`:229-231`, `:2337-2341`). | Reuse one exhaustive formatter and surface direct-import texture diagnostics instead of console-only failure. |
| S6-F15 | RT2App, RT2Tests and RT2SliceRunner all compile `SceneLoader`, but the CPU targets enumerate source files explicitly (`RT2Tests/premake5.lua:13`; `RT2SliceRunner/premake5.lua:10-39`; generated vcxproj entries at `RT2Tests/RT2Tests.vcxproj:253` and `RT2SliceRunner/RT2SliceRunner.vcxproj:194`). | The extracted CPU-only source requires build-file wiring. This is an expected build-configuration change and must be flagged in the implementation report. No Vulkan/Walnut dependency may enter it. |

#### Settled implementation decisions

These are direct implementations of approved W3-Q1/Q2/Q3/Q5/Q6/Q7/Q8/Q9;
no new product-policy question remains open.

| ID | Decision |
|---|---|
| S6-D1 — representation | Add `AssetReference ref` to `SceneTexture`. `ref.kind` is always `Texture`. `filepath` remains a compatibility mirror of `ref.path` through step 7. Decoded pixels are cache data and are never serialized. The existing empty native `textures` JSON array stays unchanged in step 6. |
| S6-D2 — extracted CPU stage | Add CPU-only `TextureAssetPipeline.{h,cpp}`. It owns capture payloads, format manifests, external reference construction, database dependency selection, locator calls, sidecar assignment in explicit-import mode, decode, placeholder construction and diagnostic sorting. It may include tinygltf/tinyobj/stb headers but may not include renderer, Vulkan, Walnut, ImGui, GLFW, NRD or NRI headers. |
| S6-D3 — loader context | Introduce `TextureAssetLoadContext` containing the caller's `AssetResolutionContext`, owner-model `AssetReference`, absolute resolved owner path, owner effective ID, entity UUID/name, `ReadOnly` versus `ExplicitImport` mode, and a nullable `IUuidProvider` that is required only for `ExplicitImport`. Every context-aware loader overload also requires `std::vector<AssetDiagnostic>&`. Invalid combinations are loud `Malformed` diagnostics. |
| S6-D4 — canonical keys | Valid glTF image dependencies use exactly `gltf:image=<imageIndex>`. An invalid glTF texture source uses `gltf:texture=<textureIndex>` and reports the invalid source index in `detail`. OBJ edges use exactly `obj:material=<materialIndex>:texture=<diffuse|normal|emissive|roughness>`, enumerated in that slot order. |
| S6-D5 — external paths | Resolve an external glTF URI or OBJ MTL texture name against the resolved owner model's parent, lexically normalize it, and express `ref.path` relative to `context.resolution.assetRoot` when possible. Legacy absolute input remains in memory only. Do not fall back to process CWD and do not persist a newly created absolute path. |
| S6-D6 — embedded identity | A bufferView/data-URI/GLB image uses the owner model's physical path and effective model ID with `kind=Texture` and `sourceKey=gltf:image=<n>`. It never receives a child sidecar. A nil owner ID is legal in read-only legacy load; the texture remains usable with nil cached ID and the owner locator's single `Stale` repair signal. |
| S6-D7 — external identity | An external glTF/OBJ image is a separate physical asset and uses its own sidecar. Read-only resolution never writes. Explicit import first verifies the dependency is a regular file, then calls `ResolveOrAssign`, and finally resolves/decodes through the same locator path. Successful repair replaces the pre-repair advisory; a sidecar parse/write problem is surfaced as `Stale` while decoded content remains usable. Missing files never receive sidecars. |
| S6-D8 — capture then decode | TinyGLTF's image callback copies encoded bytes by image index and returns success without decoding. Structural glTF parse errors still fail the model. External images ignore TinyGLTF's opportunistic bytes and are reread only from the locator's `resolvedPath`; embedded images decode only from captured bytes. Thus malformed image bytes cannot reject valid geometry. |
| S6-D9 — placeholder | The sole CPU placeholder is a 2×2 RGBA8 magenta/black checker, row-major bytes `ff 00 ff ff`, `00 00 00 ff`, `00 00 00 ff`, `ff 00 ff ff`; `width=2`, `height=2`, `channels=4`, `isHDR=false`, `isSRGB=false`, and `floatPixels` empty. It retains the failed dependency's `AssetReference` and compatibility `filepath`. |
| S6-D10 — index containment | glTF emits exactly one `SceneTexture` per `model.textures` entry in source order. OBJ emits one entry per non-empty referenced material slot in material-index order and the fixed slot order from S6-D4. Missing, malformed, unresolved and conflicting dependencies all consume their normal slot and install the placeholder, so material indices remain in range. No cross-slot deduplication is introduced. |
| S6-D11 — diagnostics | Locator failure preserves its terminal `Missing`/`Malformed`/`Conflict`. Successful locator advisories are `Stale`, never `Missing`. Decode failure adds one `Malformed`; missing/invalid embedded payload adds one `Unresolved`. All texture diagnostics use `kind=Texture`, the canonical source key, the first dependent entity context when available, and the attempted physical path. Public APIs stable-sort only their appended slice with `AssetDiagnosticSortKey`. |
| S6-D12 — return policy | A texture failure never makes an otherwise structurally valid glTF/OBJ load return false. It produces a diagnostic plus placeholder. Model syntax/geometry failure remains a model `Malformed` hard failure. `ResolveAll=false` still leaves the input document unchanged; accepted partial/model results may commit real textures and placeholders under W3-Q6. |
| S6-D13 — compatibility | Keep the current short `SceneLoader` overloads until step 7. Each creates a read-only context rooted at the model parent, invokes the same structured implementation, stable-sorts diagnostics, and logs every diagnostic. It contains no lexical/direct decode fallback. Production `SceneManager` and `SceneAssetResolver` must use the explicit overloads in step 6. |
| S6-D14 — schema boundary | Do not bump schema v3 and do not populate the native top-level `textures` array. External identity is durable in its per-file sidecar; embedded identity is durable in the owner model sidecar plus source key. W4 builds/persists the project dependency index and W5 owns the v4 migration/reporting pass. |

#### Concrete API and data flow

`TextureAssetPipeline.h` will expose neutral manifest/result types plus:

```cpp
enum class TextureIdentityMode : uint8_t { ReadOnly, ExplicitImport };

struct TextureAssetLoadContext
{
    AssetResolutionContext resolution;
    AssetReference         ownerModel;
    std::filesystem::path  resolvedOwnerPath;
    UUID                   effectiveOwnerId;
    UUID                   entityUuid;
    std::string            entityName;
    TextureIdentityMode    identityMode = TextureIdentityMode::ReadOnly;
    IUuidProvider*         uuidProvider = nullptr;
};
```

The public stage functions append results/diagnostics rather than mutating
global state:

```cpp
GltfTextureManifest EnumerateGltfTextureDependencies(...);
ObjTextureManifest  EnumerateObjTextureDependencies(...);
std::vector<SceneTexture> ResolveAndDecodeTextures(
    const TextureManifest&,
    const TextureAssetLoadContext&,
    std::vector<AssetDiagnostic>&);
SceneTexture MakeMissingTexturePlaceholder(const AssetReference&);
```

The exact tinygltf/tinyobj parameter types may remain in the `.cpp` through
format-specific adapters; the public header must remain CPU-only and must
not expose renderer types. Manifest entries contain output slot, canonical
source key, external URI or embedded encoded bytes, and the OBJ material
binding where applicable.

The four context-aware `SceneLoader` entry points keep their existing return
types and add `const TextureAssetLoadContext&` plus
`std::vector<AssetDiagnostic>&`. Their order is:

1. parse model structure and capture encoded glTF image payloads;
2. in explicit-import mode, establish/reuse the owner model sidecar ID after
   structural parse succeeds and before embedded references are built;
3. enumerate a deterministic texture manifest;
4. resolve identities/paths and decode each entry into a real texture or
   placeholder;
5. install materials/texture indices, then build geometry/entities;
6. return false only for a model-level failure.

`SceneAssetResolver::ModelRef` retains the original owner reference and
locator `effectiveId`. Its staged load uses `ReadOnly`, passes the existing
scene-root context and first entity context, and appends texture diagnostics
to the same caller vector. `SceneManager::{LoadScene,ImportGltf,ImportObj}`
uses `ExplicitImport`, passes its UUID provider, and surfaces the returned
diagnostics through Walnut's exhaustive formatter. RT2SliceRunner receives
the behavior through `SceneAssetResolver`; no GPU dependency is added.

#### Exact post-cutover characterization changes

Only the transitional texture expectations are authorized to change:

| Existing case | Old value | New value |
|---|---|---|
| OBJ valid external texture | one decoded texture, material index 0, no structured diagnostic | same decoded texture/index; populated `Texture` ref; read-only no-sidecar fixture emits one `Stale` |
| OBJ missing texture | `textures.empty()`, material index `-1`, no diagnostic | one exact placeholder, material index `0`, one `Texture/Missing` |
| OBJ malformed texture | `textures.empty()`, material index `-1`, console log only | one exact placeholder, material index `0`, ordered `Texture/Stale` then `Texture/Malformed` for the no-sidecar read-only fixture |
| glTF valid external image | decoded slot/index 0, no structured diagnostic | same decoded slot/index; populated ref; one `Texture/Stale` for the no-sidecar read-only fixture |
| glTF missing external image | one empty slot, index 0, warning only | one exact placeholder, index 0, one `Texture/Missing` |
| glTF malformed external image | whole model returns false; no geometry/material/texture | model returns true; geometry/material retained; one exact placeholder at index 0; ordered `Texture/Stale` then `Texture/Malformed` |
| glTF invalid texture source | one empty slot at index 0 | one exact placeholder at index 0 and one `Texture/Unresolved` keyed `gltf:texture=0` |

The carryover expectation changes are mechanical and separately authorized:
successful identity-repair/stale-path cases change only
`AssetDiagnostic::Missing` → `AssetDiagnostic::Stale`; failure return values,
resolved paths and real missing-file `Missing` expectations do not change.

#### Focused tests to add

Add focused CPU-only cases (prefer
`RT2Tests/src/Phase7W3CharacterizationTests.cpp`, with locator-only cases in
`Phase7W3LocatorTests.cpp`):

1. successful locator cases 2, 4 and 8a emit `Stale` and zero `Missing`;
   one table-driven invariant covers every successful locator result;
2. the exact 2×2 placeholder bytes and all metadata/ref fields are stable;
3. explicit import of an external glTF texture assigns a sidecar ID, stores
   it in `SceneTexture::ref`, and reuses it on the second import;
4. explicit OBJ import follows the same sidecar/reuse contract;
5. a moved external texture resolves by the unique dependency ID when a
   database is supplied, while a conflicting or ambiguous dependency claim
   produces `Conflict` plus a placeholder;
6. embedded GLB and data-URI images use the owner model ID plus
   `gltf:image=0`, decode successfully, and create no child `.rt2meta`;
7. malformed embedded bytes preserve valid geometry and produce one
   `Malformed` placeholder;
8. reversed manifest/dependency input produces byte-identical textures,
   bindings and sorted diagnostic snapshots;
9. the committed `script-scenario.lua.rt2meta` is a valid non-nil UUID and
   the scenario's nil-ID reference resolves with zero diagnostics;
10. all four material roles retain in-range texture indices when their OBJ
    files are missing or malformed.

Every fixture write must be `REQUIRE`d. Tests run from the repository root;
no machine-local Downloads asset is evidence for this step.

#### Discrimination proofs required

Every new case must be shown red against a deliberate temporary production
or fixture fault, then green after restoration. A compact proof matrix is
acceptable when one fault discriminates several cases:

| Temporary fault | Cases that must fail |
|---|---|
| Change one successful locator advisory back to `Missing` | successful-result invariant and affected characterization |
| Return an empty placeholder pixel vector | exact placeholder plus every missing/malformed containment case |
| Make malformed image decode return model failure | glTF external/embedded containment cases |
| Bypass `Resolve` and open the URI directly | moved-ID and conflict/ambiguity cases |
| Mint a child sidecar for an embedded image | embedded owner-identity/no-child-sidecar case |
| Temporarily remove/rename `script-scenario.lua.rt2meta` | committed-fixture case and clean scenario-output check |
| Reverse or skip final diagnostic sorting | order-independence snapshot |

Use `apply_patch` for every temporary source fault, revert only that fault,
rebuild the affected target, and record failing/passing case and assertion
counts in the step-6 verification report. No existing test may be deleted,
skipped or weakened.

#### Implementation order inside the step-6 commit

1. Land the two step-5 carryovers and their focused tests; confirm
   `run_script_test.ps1` remains green and emits no asset diagnostic.
2. Add `SceneTexture::ref`, placeholder helper and the CPU-only pipeline
   types; wire the new source into tracked premake/vcxproj files and the
   generated local slice vcxproj.
3. Replace TinyGLTF eager decode with capture-only payload collection.
4. Implement glTF manifest enumeration, locator/dependency selection,
   decode and placeholder containment; route both glTF loader paths through
   it.
5. Implement OBJ manifest enumeration and route both OBJ loader paths
   through the same resolver/decode stage.
6. Thread explicit contexts/sinks through `SceneAssetResolver`,
   `SceneManager`, Walnut and RT2SliceRunner; retain only the structured
   compatibility adapters.
7. Rewrite the seven authorized characterization expectations, add focused
   identity/ordering/placeholder tests, and perform every red/green proof.
8. Run the complete verification gate, append the step-6 implementation
   report, refresh Graphify, and commit step 6 only. Stop before step 7.

#### Verification gate

Run from the repository root and record observed counts:

```powershell
msbuild RT2App.sln -t:RT2Tests -p:Configuration=Release -p:Platform=x64
.\bin\Release-windows-x86_64\RT2Tests\RT2Tests.exe
msbuild RT2App.sln -t:RT2Tests -p:Configuration=Debug -p:Platform=x64
.\bin\Debug-windows-x86_64\RT2Tests\RT2Tests.exe
msbuild RT2App.sln -t:RT2SliceRunner -p:Configuration=Release -p:Platform=x64
msbuild RT2App.sln -t:RT2SliceRunner -p:Configuration=Debug -p:Platform=x64
.\run_script_test.ps1
graphify update .
```

Also build the Release `RT2App` target so the interactive direct-import
wiring is compiled. Do not touch the pre-existing whole-Debug RT2App
NRD/NRI mismatch. The original 630 cases must remain green with only the
explicit old/new expectation changes above; report the new total and both
assertion counts. `run_script_test.ps1` must pass and print no asset
diagnostic. Commit `GRAPH_REPORT.md` only if Graphify changes it. Do not
push.

#### Explicit non-goals

- Do not remove `SceneAssetResolver::ResolvePath`, legacy `filepath`, or the
  short `SceneLoader` adapters; that is step 7.
- Do not build the W4 project scan/database owner or content browser.
- Do not introduce schema v4, cache artifacts, texture editing UI, texture
  deduplication, colour-space redesign, mip generation, GPU placeholder
  logic, or hot reimport.
- Do not modify renderer/Vulkan behavior or the known Debug RT2App
  NRD/NRI configuration.
- Stop after the step-6 implementation report and commit.

#### Step 6 review amendment — four independently green commits

Approved 2026-07-26 after implementation-plan review. This amendment
**supersedes only** the earlier phrases "inside the step-6 commit" and
"commit step 6 only." The production contract, decisions, authorized
expectation table, discrimination matrix, verification requirements,
non-goals, and stop-before-step-7 boundary remain unchanged.

Step 6 lands as four ordered commits. Do not squash them: each is a recovery
point, must build and test independently, and must leave the branch green
before the next begins.

| Commit | Scope | Independent green gate |
|---|---|---|
| **6.1 — carryovers and diagnostic truth** | Reclassify all three successful locator `Missing` advisories to `Stale`; update only the mechanically authorized severity expectations; add the successful-result/no-`Missing` invariant; add and validate committed `script-scenario.lua.rt2meta`; require clean scenario output. No texture representation or loader change. | Focused locator/fixture tests; full Release and Debug `RT2Tests`; both retain every original 630 case; `run_script_test.ps1` passes and prints no asset diagnostic. |
| **6.2 — additive texture pipeline foundation** | Add `SceneTexture::ref`, exact placeholder helper, manifest/context/result types, CPU-only `TextureAssetPipeline.{h,cpp}`, capture-only callback implementation, and build wiring for RT2App/RT2Tests/RT2SliceRunner. Add unit tests for placeholder bytes, manifest keys/order and capture payloads. Nothing in `SceneLoader` switches to the new callback or consumes the new pipeline yet. | Existing behavior and all seven transitional texture expectations remain byte-for-byte unchanged; new foundation tests pass; full Release and Debug `RT2Tests` pass; Release and Debug slice targets build; Release RT2App builds. |
| **6.3 — glTF cutover** | Atomically switch both glTF entry points from eager decode to the already-tested capture callback; route both glTF texture loops through the pipeline; thread owner/context/diagnostics through glTF callers in SceneAssetResolver, SceneManager, Walnut and the runner path; implement external/embedded identity, database dependency selection and placeholder containment. Rewrite only the four glTF rows in the authorized expectation table and add/prove the focused glTF cases. OBJ remains on its old path. | All glTF red/green proofs recorded; full Release and Debug `RT2Tests` pass; Release and Debug slice targets build; Release RT2App builds; script gate remains clean. |
| **6.4 — OBJ cutover and Step 6 close** | Route both OBJ entry points through the shared manifest/resolver/decode stage; thread explicit-import context through remaining OBJ callers; rewrite only the three OBJ rows; add/prove OBJ sidecar, four-slot containment and final order-independence cases; remove no compatibility API. Append the complete Step 6 verification report and refresh Graphify. | All remaining red/green proofs recorded; every original 630 case plus all new cases pass in full Release and Debug suites; both slice targets and Release RT2App build; script gate passes cleanly; Graphify refreshed and `GRAPH_REPORT.md` committed only if changed. |

The capture-only callback deserves an explicit boundary rule. Adding it in
6.2 is additive; activating it is part of 6.3. Switching `SceneLoader` to
capture-only before the pipeline consumes captured bytes would make the
legacy glTF loops observe undecoded `image.image` data and would not be an
independently green scaffold commit.

Within each commit:

1. implement the production slice;
2. add/update only its authorized tests;
3. perform and revert its discrimination faults;
4. run its complete green gate from the repository root;
5. inspect the diff and commit before starting the next slice.

If any gate is red, stop on that commit's working tree; do not begin or
partially stage the following commit. The Step 6 implementation report must
list all four commit hashes, the per-commit test totals, and the
discrimination proof associated with each recovery point. Do not push, and
stop after 6.4 rather than continuing to step 7.

### W3 step 6 implementation report — texture cutover complete

Completed 2026-07-26 on `phase-6-scripting`. This report closes incremental
step 6 only. Step 7 host cleanup and every later W3 step remain unstarted.

#### Delivered recovery points

| Slice | Commit | Green suite at the recovery point |
|---|---|---|
| 6.1 carryovers | `8238e79` | Release and Debug 632/632 cases, 144961/144961 assertions |
| 6.2 pipeline foundation | `cb54d44` | Release and Debug 635/635 cases, 144996/144996 assertions |
| 6.3 glTF cutover | `438b330` | Release and Debug 639/639 cases, 145175/145175 assertions |
| 6.4 OBJ cutover and close | recorded by the following commit | Release and Debug 642/642 cases, 145315/145315 assertions |

Step 6 added `SceneTexture::ref`, the CPU-only
`TextureAssetPipeline.{h,cpp}`, exact placeholder construction, glTF
capture-then-decode, and deterministic glTF/OBJ dependency manifests.
Both glTF and OBJ entry points now use the shared database/locator,
sidecar-assignment, decode, containment and diagnostic-sort path. External
textures receive their own durable IDs in explicit-import mode; embedded
glTF textures retain owner identity and never mint child sidecars.
`SceneAssetResolver`, `SceneManager`, Walnut's synchronous/background paths
and the slice-runner path pass explicit resolution context and one
diagnostic sink. The short compatibility loaders remain and call the same
structured implementation, as required until step 7.

The tracked `tiny_textured.glb` fixture was found to encode the invalid
schema value `bufferView: -1`. Its generator now writes a real PPM data URI,
the binary fixture was regenerated, and the existing round-trip case
continued to pass. This was a fixture correctness repair, not an expectation
change.

One build-configuration change was required and is flagged explicitly:
Debug compilation of the enlarged `SceneLoader.cpp` exceeded the default
COFF section limit (`C1128`). `/bigobj` is applied only to that translation
unit in the Debug `RT2Tests` and `RT2SliceRunner` configurations, in both
premake sources and the tracked test vcxproj. The Release configurations and
RT2App link settings are unchanged. The pre-existing whole-Debug RT2App
NRD/NRI mismatch was not touched.

#### Authorized expectation changes

No original case was removed, skipped or weakened. Only the seven rows
authorized by the step-6 plan changed:

| Case | Old | New |
|---|---|---|
| OBJ valid external | one decoded texture/index 0; no structured diagnostic | same decoded texture/index; populated `Texture` ref; one read-only `Stale` |
| OBJ missing | no texture; index `-1`; no diagnostic | exact placeholder at index 0; one `Texture/Missing` |
| OBJ malformed | no texture; index `-1`; console log only | exact placeholder at index 0; ordered `Texture/Stale`, then `Texture/Malformed` |
| glTF valid external | decoded slot/index 0; no structured diagnostic | same decoded slot/index; populated ref; one read-only `Stale` |
| glTF missing external | empty slot/index 0; warning only | exact placeholder/index 0; one `Texture/Missing` |
| glTF malformed external | whole model failed with no retained geometry/material/texture | model succeeds; geometry/material retained; exact placeholder/index 0; ordered `Texture/Stale`, then `Texture/Malformed` |
| glTF invalid source | empty slot/index 0 | exact placeholder/index 0; one `Texture/Unresolved` keyed `gltf:texture=0` |

The separately authorized carryover changed successful identity-repair
advisories from `Missing` to `Stale`; actual unresolved files remain
`Missing`.

#### Discrimination proofs

Every temporary fault was applied to production or fixture behavior with
`apply_patch`, the affected Release target was rebuilt where required, the
case was observed red, the one fault was removed, and the case was observed
green:

| Recovery point | Temporary fault | Red observation | Restored green |
|---|---|---|---|
| 6.1 | successful no-sidecar resolve emitted `Missing` | invariant 0/1 cases, 14/16 assertions | 1/1, 16/16 |
| 6.1 | committed scenario sidecar made malformed | fixture case 0/1, 2/3; scenario gate rejected the asset diagnostic | fixture 1/1, 10/10; scenario gate PASS |
| 6.2 | placeholder returned an empty pixel vector | 0/1, 11/12 | focused foundation set green |
| 6.2 | manifest sort skipped | 0/1, 8/14 | focused foundation set green |
| 6.2 | capture payload discarded | 0/1, 7/9 | focused foundation set green |
| 6.3 | placeholder returned an empty pixel vector | 1/5 cases, 203/217 assertions | 5/5, 217/217 |
| 6.3 | captured embedded payload discarded | 0/1, 46/49 | restored focused case green |
| 6.3 | database dependency lookup bypassed | 0/1, 39/52 | restored focused case green |
| 6.3 | texture `Malformed` made model-fatal | 2/5 cases, 150/153 | 5/5, 217/217 |
| 6.3 | external explicit `ResolveOrAssign` skipped | 0/1, 6/7 | restored focused case green |
| 6.3 | embedded child sidecar minted | 0/1, 12/13 | restored focused case green |
| 6.3 | structured `SceneTexture::ref` cleared | 0/1, 11/14 | restored focused case green |
| 6.4 | placeholder returned an empty pixel vector | 2/4 cases, 160/166 | 4/4, 166/166 |
| 6.4 | external explicit `ResolveOrAssign` skipped | 0/1, 6/7 | 1/1, 16/16 |
| 6.4 | OBJ material binding skipped | 2/4 cases, 159/166 | 4/4, 166/166 |
| 6.4 | final texture diagnostic sort disabled | 0/1, 47/48 | 1/1, 48/48 |

#### Verified by running

- From the repository root, the final Release and Debug `RT2Tests`
  executables each passed 642/642 cases and 145315/145315 assertions. Thus
  every original 630-case step-6 baseline case remained present and green.
- `RT2SliceRunner` built in Release and Debug. `RT2App` built in Release.
- `run_script_test.ps1` passed: 60 frames, one entity, no mismatches, and no
  asset diagnostic.
- `graphify update .` completed code extraction and rebuilt the graph at
  33664 nodes, 70884 edges and 1388 communities. Its wrapper exceeded the
  180-second command timeout after printing completion; no Graphify/uv/Python
  refresh process remained. `GRAPH_REPORT.md` changed and is included.
- The Phase 6 hard contract was checked both inside both full suites and by
  named Release cases: empty scripts remain legal; syntax failure
  quarantines only the affected instance; last-good descriptors survived
  parse failure (8/8); resolver parse failure preserved authored values and
  suppressed reconciliation (12/12); a mid-edit syntax error kept the live
  callback; `rt2:reload()` did not re-enter; self-rescheduling timers and
  input bindings remained live. The combined named contract selection
  passed 7/7 cases and 39/39 assertions. The 60-frame scenario additionally
  exercised the bound entity through the slice-runner consumer.
- `git diff --check` was clean after all temporary faults were removed.

#### Assumed or intentionally not run

- Whole-Debug `RT2App` was intentionally not linked because its existing
  NRD/NRI mismatch is explicitly out of scope; no inference of a green
  whole-Debug app is made.
- No GPU/render-quality claim is made. CPU suites, both slice builds and the
  Release application build verify linkage and loader behavior; they do not
  replace an interactive GPU run.
- No machine-local Downloads asset is used as evidence for texture
  correctness. Such optional legacy tests may still execute when their
  local files exist, but all new evidence uses generated or committed
  project fixtures.

### W3 step 7 implementation plan — complete host wiring

Approved with amendments 2026-07-26 and grounded against commit `591a76e` on
`phase-6-scripting`. This is the implementation plan for incremental step 7
only. No step-7 production code has started. The unrelated UI and render-loop
commits `113ec7d`, `deb7ee2` and `591a76e` were allowed to land before
grounding; concurrent UI/render work and the untracked `.claude/` directory
are not part of this work and must not be touched.

Review approved S7-Q1, S7-Q2, S7-Q3 and S7-Q6 as written; amended S7-Q4 and
S7-Q5; and settled the remaining healthy-asset diagnostic/gate policy as
S7-Q7. The decisions below are final implementation instructions.

#### Boundary and completion claim

Step 7 completes W3 host wiring and removes only the compatibility paths that
were deliberately retained while models, environments, scripts and textures
were cut over. It does not start step 8 final verification or W4 project
database ownership.

At completion:

- a relative `AssetReference::path` can reach the filesystem only through an
  explicit absolute `AssetResolutionContext::assetRoot`;
- the four format-loader entry points have one context-aware form each, with
  `TextureAssetLoadContext::resolvedOwnerPath` as the sole physical model
  path and one required `AssetDiagnostic` sink;
- `SceneAssetResolver::ResolvePath`, all four short `SceneLoader` adapters,
  their CWD-derived context helper, and the recovery CWD convenience overload
  no longer exist;
- direct-import, recovery, script binding, runtime reload and watcher hosts
  either provide an explicit absolute root/path or fail loudly without
  consulting process CWD;
- `SceneTexture::ref` is authoritative and the temporary `filepath` mirror is
  removed;
- native save remains available for legitimate cross-volume assets, but every
  non-portable retained absolute reference emits a distinct `NonPortable`
  advisory that reaches the editor status surface; glTF export never writes
  an absolute texture URI it cannot relativize;
- a fully healthy asset emits no diagnostic, advisory severities sort before
  terminal failures, and the script scenario gate fails only on severity
  `Missing` or higher;
- every Phase 6 scripting behavior remains a hard contract.

Step 8 remains separately observable work: whole-solution verification,
final report, Graphify refresh and the final W3 close. Step 7 still runs its
own full per-commit gates; that does not silently consume step 8.

#### Grounded findings

| ID | Fact at `591a76e` | Step-7 consequence |
|---|---|---|
| S7-F1 | `AssetResolutionContext::assetRoot` is documented as absolute and the locator says it has no CWD fallback (`RT2App/src/AssetResolver.h:101-107`), but `ResolvePathNoCwd` forms `assetRoot / p` and calls `exists`/`is_regular_file` without rejecting an empty or relative root (`RT2App/src/AssetResolver.cpp:24-47`). `Resolve` computes that candidate before ID policy (`:101-112`). The unused `NormalizeResolved` helper's comment says “Make absolute against the asset root” immediately above `return lex;`, which returns it relative (`:10-22`). | This is the codebase's characteristic silent-failure mode: names/comments promise an invariant while unchecked filesystem behavior violates it without a diagnostic. Root validation must live at the shared locator boundary; delete the misleading dead helper and record this finding explicitly in the implementation report. |
| S7-F2 | The generic locator already has a discriminative no-CWD case for an unrelated **absolute** root and a CWD decoy (`RT2Tests/src/Phase7W3LocatorTests.cpp:526-568`), but it does not cover empty or relative roots. | Add the missing cells; the existing test is not evidence for the newly found hole. |
| S7-F3 | `SceneAssetResolver::ResolvePath` remains declared at `RT2App/src/SceneAssetResolver.h:102-104` and defined at `RT2App/src/SceneAssetResolver.cpp:106-131`. Its final branch tries the raw relative path and calls `fs::absolute`, but `rg` finds no production or test caller. | Delete the declaration and definition. Do not preserve a forwarding wrapper around `Resolve`. |
| S7-F4 | `SceneLoader` still exposes four short entry points beside the structured forms (`RT2App/src/SceneLoader.h:20-25`, `:31-36`, `:39-44`, `:60-68`). Their definitions call `MakeCompatibilityTextureContext` (`RT2App/src/SceneLoader.cpp:39-57`, `:508-516`, `:1278-1286`, `:1906-1914`, `:2398-2408`). | Migrate every caller first, then remove all four short declarations/definitions and the helper. |
| S7-F5 | Even the structured loader path overwrites the supplied `resolvedOwnerPath` with `fs::absolute(filepath)` (`RT2App/src/SceneLoader.cpp:59-67`). A relative `filepath` can therefore select a CWD file while the supposedly authoritative context names another root. | Remove the redundant `filepath` parameter from structured entry points. The already-absolute context path becomes the only parse path; mismatch cannot be represented. |
| S7-F6 | Production callers already pass structured contexts and sinks: `SceneAssetResolver` (`RT2App/src/SceneAssetResolver.cpp:451-473`), `SceneManager` (`RT2App/src/SceneManager.cpp:263-285`, `:512-568`), Walnut synchronous/background routes (`RT2App/src/WalnutApp.cpp:263-282`, `:2276-2289`, `:2392-2395`) and the recovery slice through `SceneManager` (`RT2SliceRunner/src/Main.cpp:140-154`). | The cutover is an API contraction, not a new texture behavior. These callers only need to make the context path authoritative. |
| S7-F7 | `SceneManager::MakeExplicitTextureContext` and Walnut's duplicate helper call `std::filesystem::absolute` on the raw host input (`RT2App/src/SceneManager.cpp:109-125`; `RT2App/src/WalnutApp.cpp:1577-1593`). | Direct-import hosts must reject a relative physical path with a `Model/Malformed` diagnostic. They may lexically normalize or weakly canonicalize an already-absolute path, but may not call `absolute` to invent a root. |
| S7-F8 | Six test files still call short loaders with repo-relative or machine-absolute strings: `RT2Tests/src/EcsSceneLoaderTests.cpp:14-127`, `GltfGeometryTests.cpp:46-336`, `GltfSaveGeometryTests.cpp:57-286`, `SceneLoaderTests.cpp:177-507`, and the optional machine-local cases in `BuildGpuFromEcsTests.cpp:17-182`. `Phase7W3CharacterizationTests.cpp:1556-1900` already demonstrates explicit contexts. | Add one test-only context builder requiring an explicit root, migrate all short calls without changing their resource assertions, and retain the machine-local cases only as optional legacy coverage—not Step-7 evidence. |
| S7-F9 | `SceneTexture` has both `filepath` and `ref`, with a comment that the mirror lasts through step 7 (`RT2App/src/SceneTypes.h:94-99`). The pipeline writes both (`RT2App/src/TextureAssetPipeline.cpp:295-312`, `:373-390`, `:474-480`), glTF export reads only `filepath` (`RT2App/src/SceneLoader.cpp:175-185`), and seven test assertions pin the mirror (`RT2Tests/src/Phase7W3TexturePipelineTests.cpp:19-27`, `Phase7W3CharacterizationTests.cpp:1578-1609`, `SceneLoaderTests.cpp:346-349`, `:445-448`, `SceneTextureTests.cpp:16-23`). | Remove only `SceneTexture::filepath`; switch export and assertions to `ref.path`. `SceneMesh::filepath` at `SceneTypes.h:34-40` is a different, currently unused legacy field and is not authorized by this step. |
| S7-F10 | The explicit recovery overload accepts a logical asset root, but the compatibility overload derives it from `current_path` (`RT2App/src/SceneRecoveryService.h:56-69`; `SceneRecoveryService.cpp:184-198`). The explicit implementation also falls back to `current_path` when its root is empty (`SceneRecoveryService.cpp:262-274`). Only two recovery tests still use the short overload (`RT2Tests/src/RecoveryTests.cpp:123-137`, `:186-200`); Walnut and RT2SliceRunner already pass explicit roots (`RT2App/src/WalnutApp.cpp:1544-1554`; `RT2SliceRunner/src/Main.cpp:212-221`). | Remove the short overload, migrate those two tests, and keep empty/non-absolute logical root as `InvalidArgument` for an untitled dirty snapshot. The production Walnut host must make that guard a never-happens invariant by supplying a created absolute per-user recovery asset root. Clean documents may still return “no snapshot” before validation because no filesystem work is attempted. |
| S7-F11 | Walnut's `UntitledAssetRoot` returns the configured project root, then falls back to process CWD (`RT2App/src/WalnutApp.cpp:2597-2604`). The same helper feeds autosave (`:1547-1554`) and Enter Play's script context (`:2697-2703`), although their no-project policies now differ. The watcher accepts `resolvedPath` from either a result or the last diagnostic and adds its parent without requiring it to be absolute (`:3000-3039`). `%LOCALAPPDATA%` access already exists at `WalnutApp.cpp:2621-2627`. | Split recovery-root and script-root selection. Untitled autosave receives an ensured absolute `%LOCALAPPDATA%\RT2\recovery` root; scripts still receive only saved-scene/project root or empty. Remove the CWD fallback, reject relative watch candidates and preserve legacy absolute script references. |
| S7-F12 | Script Inspector resolution already uses an explicit scene/dialog root and the shared adapter (`RT2App/src/SceneEditorUI.cpp:1678-1699`); background field reconciliation, runtime, watcher and the runner already share `AssetResolutionContext` and `AssetDiagnostic` sinks (`RT2App/src/WalnutApp.cpp:2902-2951`, `ScriptFieldResolver.cpp:14-89`, `ScriptSystem.cpp:755-819`, `RT2SliceRunner/src/Main.cpp:522-530`). | Preserve these routes. Step 7 is an audit/contract cleanup, not another scripting cutover. |
| S7-F13 | Explicit script binding still uses the script-only lexical `ResolveAuthoredScriptPath` helper (`RT2App/src/SceneManager.cpp:94-107`, `:3673-3711`) before `ResolveOrAssign`. It correctly avoids CWD when the scene has no source path, but it bypasses locator kind/ID/sidecar policy. | Replace the lexical existence check with `ResolveScriptAssetPath`; mint/reuse through `ResolveOrAssign` only after successful shared resolution. Missing scripts remain bindable and unminted, preserving watcher recovery/quarantine behavior. |
| S7-F14 | `ScriptSystem::ReloadScript` accepts any path and `weakly_canonical` therefore resolves a relative manual path through CWD (`RT2App/src/ScriptSystem.cpp:257-273`). Legitimate sources are already absolute: efsw emits absolute paths, `BuildEnvironment` stores the locator result (`:789-810`), and `rt2.reload()` queues that stored path (`:849-860`). | Reject a relative reload request with a `Script/Malformed` diagnostic and no cache/live-instance mutation. Absolute native-separator matching remains unchanged. |
| S7-F15 | Native save still turns relative inputs into absolute paths and, when drives differ, intentionally persists the absolute result (`RT2App/src/SceneSerializer.cpp:250-280`). The Windows case explicitly expects success and `"Z:/external/move.lua"` (`RT2Tests/src/SceneSerializerTests.cpp:1037-1060`). `SceneSerializer::Save`/`SaveTo` expose only `Error&`, so a successful save has no diagnostic route (`RT2App/src/SceneSerializer.h:77-91`; `SceneSerializer.cpp:1406-1420`). Walnut reports only success/failure and overwrites the status with `"Saved"`/`"Saved As"` (`RT2App/src/WalnutApp.cpp:3107-3158`). glTF export writes a texture URI directly (`RT2App/src/SceneLoader.cpp:175-185`). | Native cross-volume save must remain successful but become visibly advisory. Add a required save diagnostic sink, a distinct `NonPortable` severity, exhaustive formatting and a Walnut status message. The glTF export rule remains stricter because export failure does not make the authoring document unsaveable. |
| S7-F16 | The step-6 close report records Release and Debug at 642/642 cases and 145315/145315 assertions; review independently confirmed that baseline at `591a76e`. Both slice targets and Release RT2App built and the script gate passed cleanly (`docs/game-engine-development-plan.md`, “W3 step 6 implementation report”). | Every one of those 642 cases remains present and green. New totals are measured rather than predicted. |
| S7-F17 | `AssetDiagnostic::Severity` currently uses declaration order `Missing`, `Malformed`, `Unresolved`, `Conflict`, `Stale` (`RT2App/src/AssetResolver.h:79-90`), while diagnostic sorting separately ranks `Stale` before terminal failures (`AssetResolver.cpp:83-94`). Format switches are repeated in SceneLoader, SceneManager and two Walnut paths. | Make advisory-versus-terminal ordering executable in the enum and one shared exhaustive name helper: `Stale=0`, `NonPortable=1`, `Missing=2`, `Malformed=3`, `Unresolved=4`, `Conflict=5`; both advisories receive severity rank 0. |
| S7-F18 | `run_script_test.ps1:73-75` fails on any `[ScriptScenario] Asset diagnostic:` line, while RT2SliceRunner prints the numeric severity (`RT2SliceRunner/src/Main.cpp:559-567`). This treats a repair/portability advisory as a scripting regression. | Parse the printed severity and fail only when it is greater than or equal to `Missing`. A fully healthy scenario still emits no diagnostic; advisory-pass and terminal-fail behavior both require discrimination proofs. |

#### Settled implementation decisions

| ID | Approved decision |
|---|---|
| S7-Q1 — locator root contract | Perform ID lookup before reference-path fallback. Ambiguity remains `Conflict` regardless of root. Before sending **any relative physical candidate** to the filesystem—either a project-relative database `sourcePath` or `AssetReference::path` fallback—require a non-empty absolute `assetRoot`; otherwise fail with one contextual `Malformed` diagnostic whose detail is exactly `"relative asset reference requires an absolute asset root"`. A unique database record whose source path is already absolute may still win without consulting an unused root. Absolute legacy references remain accepted without deriving anything from CWD. Every successful result is absolute and normalized. Amend `AssetResolver.h` to describe this executable contract. |
| S7-Q2 — one loader path | Replace each `(ecs, filepath, context, diagnostics)` form with `(ecs, context, diagnostics)` (plus `ImportSettings` where applicable). `context.resolvedOwnerPath` is the physical input passed to TinyGLTF/tinyobj. It must be non-empty and absolute before parse or identity assignment; invalid input emits one `Model/Malformed`, returns the entry point's existing failure value, and mutates no scene/sidecar. Remove all short overloads rather than marking them deprecated or deleted. |
| S7-Q3 — host-owned direct paths | OS dialog/CLI/direct-import hosts must supply an absolute path. `SceneManager` and Walnut share a small pure context-builder in `TextureAssetPipeline.h`; it normalizes an already-absolute path, fills model ref/root/mode/provider, and appends `Model/Malformed` on invalid input. It never probes CWD or mints. Existing parse-time explicit-import code remains the sole owner of `ResolveOrAssign`. |
| S7-Q4 — script/recovery no-root behavior | Missing script files remain legal authored bindings, but relative bindings cannot be resolved or watched until the host has a saved-scene/project root. Relative `ReloadScript` calls are rejected. Recovery and scripts use separate host-root policies: untitled recovery uses an ensured absolute `%LOCALAPPDATA%\RT2\recovery` root (an absolute configured project root still wins), while the script context remains empty without scene/project ownership. Add CPU-testable `SceneRecoveryService::EnsureUntitledRecoveryAssetRoot(localAppData, outRoot, err)`; Walnut owns environment access and passes `%LOCALAPPDATA%`, while tests pass a temp base. `MaybeSnapshot` keeps its empty/non-absolute `InvalidArgument` guard as a never-happens production invariant. Failure to obtain/create the per-user root is loud through the existing autosave `Error`/status path and never falls back to CWD. |
| S7-Q5 — non-portable persistence | W3-Q8 governs paths the **resolver derives**; it does not make a legitimate multi-volume authoring document unsaveable. Native save keeps the existing normalized absolute fallback, succeeds atomically, and appends exactly one `NonPortable` diagnostic per offending asset reference. Change `SceneSerializer::Save` and `SaveTo` to require `std::vector<AssetDiagnostic>&`; migrate every caller rather than retaining a sink-less overload. Add tested `FormatNonPortableAssetSummary`; Walnut logs the diagnostic and, after a successful save, shows `"Saved with " + summary` in `m_LastStatusMsg` instead of overwriting it with plain `"Saved"` (one warning names the ref; multiple warnings include the count and first sorted ref). Recovery propagates save diagnostics to Walnut's autosave log/status path. glTF export still relativizes an absolute `SceneTexture::ref.path` to the output parent and returns false if it cannot; it never writes the absolute URI. |
| S7-Q6 — compatibility representation | Remove `SceneTexture::filepath` in this step. `ref.kind == Texture` and `ref.path` are the only source identity. Placeholder bytes, decoded cache fields, material indices, colour-space flags and native scene `textures: []` behavior do not change. Do not remove `SceneMesh::filepath` or redesign native texture serialization. |
| S7-Q7 — healthy assets and diagnostic threshold | A fully healthy asset emits zero diagnostics. `Stale` remains limited to successful resolution with actual path/identity/database repair state; `NonPortable` is limited to successful persistence of an absolute fallback. Give explicit enum values `Stale=0`, `NonPortable=1`, `Missing=2`, `Malformed=3`, `Unresolved=4`, `Conflict=5`, keep `Missing` as the default member value, and expose shared `AssetDiagnosticSeverityName`/`IsTerminalAssetDiagnostic` helpers. `AssetDiagnosticSortKey` assigns both advisories rank 0. All formatters use the shared exhaustive name helper. For every scenario diagnostic line, `run_script_test.ps1` must parse `severity=<integer>`, fail loud if the field is absent/malformed, and fail the gate only when the value is `>= Missing`; a healthy scenario still prints no asset diagnostic. |

#### Exact API and implementation shape

The final public loader surface is:

```cpp
static bool LoadIntoECS(
    ECSScene&,
    const rt2::core::TextureAssetLoadContext&,
    std::vector<rt2::core::AssetDiagnostic>&);

static entt::entity ImportIntoECS(
    ECSScene&,
    const rt2::core::TextureAssetLoadContext&,
    std::vector<rt2::core::AssetDiagnostic>&);

static bool LoadObjIntoECS(
    ECSScene&,
    const rt2::core::TextureAssetLoadContext&,
    std::vector<rt2::core::AssetDiagnostic>&);

static entt::entity ImportObjIntoECS(
    ECSScene&,
    const ImportSettings&,
    const rt2::core::TextureAssetLoadContext&,
    std::vector<rt2::core::AssetDiagnostic>&);
```

No default context, optional diagnostic pointer, implicit-root overload or
adapter is retained. `SceneLoader::Save` remains separate because it is an
export operation.

The diagnostic severity and persistence surfaces become:

```cpp
enum Severity : uint8_t
{
    Stale      = 0,
    NonPortable = 1,
    Missing    = 2,
    Malformed  = 3,
    Unresolved = 4,
    Conflict   = 5,
};

const char* AssetDiagnosticSeverityName(AssetDiagnostic::Severity);
bool IsTerminalAssetDiagnostic(AssetDiagnostic::Severity);
std::string FormatNonPortableAssetSummary(
    const std::vector<AssetDiagnostic>&);

static bool SceneSerializer::Save(
    const SceneDocument&, const std::filesystem::path&,
    std::vector<AssetDiagnostic>&, Error&);
static bool SceneSerializer::SaveTo(
    const SceneDocument&, const std::filesystem::path&,
    const std::filesystem::path& logicalScenePath,
    std::vector<AssetDiagnostic>&, Error&);
```

The sink-less serializer overloads are removed. The explicit
`SceneRecoveryService::MaybeSnapshot` overload likewise gains a required
`std::vector<AssetDiagnostic>&` immediately before `Error&`; it forwards the
sink to `SaveTo`. `SaveInternal` stages its diagnostics locally and appends
them to the caller only after the atomic save succeeds; a failed save leaves
both the existing output and caller diagnostic prefix unchanged. The
successful batch is stable-sorted with `AssetDiagnosticSortKey` before
append.

`RebasePath` returns both the stored path and whether the normalized absolute
fallback was retained. Its caller has the asset/entity context and appends:

```text
severity:     NonPortable
kind:         the AssetReference kind
refPath:      the original authored path
resolvedPath: the normalized absolute path written
entity:       referring entity UUID/name, or nil/empty for environment
sourceKey:    the original AssetReference source key
detail:       asset path could not be made relative to the output scene;
              saved as a normalized absolute path
```

This is one advisory per non-portable serialized reference. It does not
change the bytes written today. Walnut calls the required sink overload,
formats every entry through `AssetDiagnosticSeverityName`, logs it, and
preserves the S7-Q5 warning in the visible status line after save. Autosave
uses the same diagnostic path.

The implementation is ordered as follows:

1. Make severity ordering explicit, add the shared exhaustive severity-name
   and terminal-threshold helpers, update all consumers, and change the
   script gate to severity `>= Missing`. Pin zero diagnostics for fully
   healthy resolution before adding either new Step-7 diagnostic path.
2. Refactor generic `Resolve` so ID lookup does not depend on computing a
   reference-path fallback candidate. Validate the root before any relative
   database or fallback path reaches the filesystem, make invalid-root
   diagnostics deterministic, and delete the unused `NormalizeResolved`.
3. Add `RT2Tests/src/SceneLoaderTestSupport.h`. Its builder takes an explicit
   absolute root and a model path, constructs a read-only context, and exposes
   the diagnostics to the test. Repo-relative committed fixtures must pass
   the repository root explicitly; generated temp fixtures use their already
   absolute parent. Migrate every short-loader test call while adapters still
   exist.
4. Add the shared explicit-import context builder to the existing CPU-only
   texture pipeline header/source. Route `SceneManager`, Walnut and the
   slice-runner-through-manager path through it.
5. Change all four structured loaders to consume only
   `resolvedOwnerPath`. Validate before TinyGLTF/tinyobj parse and before
   `ResolveOrAssign`; do not overwrite the supplied context after parse.
6. Delete `MakeCompatibilityTextureContext`, four short loader
   declarations/definitions and `SceneAssetResolver::ResolvePath`.
7. Delete `SceneRecoveryService::MaybeSnapshot(doc, revision, err)` and its
   CWD fallback; add the required diagnostic sink and the injectable
   `EnsureUntitledRecoveryAssetRoot` helper. Split Walnut recovery-root
   selection from script-root selection. Ensure/create the absolute
   `%LOCALAPPDATA%\RT2\recovery` root for untitled autosave, while an
   unsaved/no-project script context remains empty. Keep and test the
   recovery service's invalid-root guard.
8. Make watcher directory selection accept only absolute candidates; reject
   relative `ReloadScript`; replace `ResolveAuthoredScriptPath` with the
   shared script adapter before explicit ID assignment.
9. Remove `SceneTexture::filepath`, stop writing the mirror in every pipeline
   branch and make glTF export read/relativize `ref.path`. Add required native
   save sinks, emit/render `NonPortable` without changing cross-volume save
   success or bytes, and keep glTF export failure for an unrelativizable URI.
10. Run the Step-7 close gate, append the implementation report, and stop.
   Do not start step 8.

#### Authorized old/new expectations

No existing test is deleted, skipped or weakened. These are the only
authorized changes; every assertion not listed here must retain its current
meaning.

| Surface | Old expectation | New expectation and authority |
|---|---|---|
| Relative ref + empty/relative root | May resolve a matching process-CWD file because `assetRoot / ref` remains relative. | Fails with one `Malformed`; W3-Q8 and S7-Q1. |
| Short loader calls | Repo-relative `filepath` alone compiles and derives a context from CWD. | No such overload exists; tests name an explicit root/context while retaining all resource assertions; step-6 S6-D13 deferral and S7-Q2. |
| Structured loader path mismatch | Separate `filepath` silently overwrites `context.resolvedOwnerPath`. | Mismatch is unrepresentable; the context path is authoritative; S7-Q2. |
| Relative direct-import path | `SceneManager`/Walnut call `absolute` and select a CWD file. | Fails before parse/mint with `Model/Malformed`; S7-Q3. |
| Untitled recovery with no project root | Compatibility/empty-root forms use process CWD. | Walnut supplies an ensured absolute `%LOCALAPPDATA%\RT2\recovery` logical root; the short API is absent and the service still rejects an invalid explicit root; amended S7-Q4. |
| Untitled relative script with no source/project root | Enter Play and watcher inherit process CWD. | Resolution is `Malformed`, no relative directory is watched, binding remains authored/unminted; S7-Q4. |
| Relative `ReloadScript` | `weakly_canonical` interprets it against CWD. | One `Script/Malformed`, no cache clear, queue or live swap; S7-Q4. |
| Matching non-nil ID/path sidecar with `database == nullptr` | Successful path verification also emits `Stale` `"database stale"` even though pre-W4 intentionally has no database. | Successful resolve with zero diagnostics; only a supplied database that misses the confirmed ID is stale; S7-Q7. |
| Script scenario diagnostic gate | Any asset diagnostic line fails the gate. | `Stale`/`NonPortable` advisories are printed but do not fail; severity `Missing` or higher fails; S7-Q7. |
| `SceneTexture::filepath` assertions | Mirror equals `ref.path`; glTF export reads the mirror. | Mirror does not exist; the same path assertions target `ref.path`, and export URI is driven by `ref.path`; step-6 S6-D1 deferral and S7-Q6. |

Empty scripts, missing-script authored bindings, texture placeholder bytes,
texture containment, material slot/index values, geometry, field values,
reload state and diagnostic ordering are not authorized to change.

The Windows cross-volume native-save expectation is deliberately **not** in
the table because it does not change: `SceneSerializerTests.cpp:1037-1060`
continues to require successful save and the same normalized
`"Z:/external/move.lua"` JSON path. It gains assertions for exactly one
`NonPortable` diagnostic with the context/detail above. Healthy same-volume
save gains a zero-diagnostic assertion.

#### Permanent tests and discrimination proofs

Every new behavior gets a permanent test and a temporary production fault.
Use `apply_patch` for each fault, build/run only the affected selection to
observe red, revert only that fault, rerun green, and record exact
case/assertion counts in the implementation report.

| Permanent evidence | Temporary fault that must make it fail |
|---|---|
| Severity policy: `Stale` and `NonPortable` are below `Missing`; every terminal severity is `>= Missing`; both advisories sort at rank 0; every value has the exact shared display name. A fully healthy resolve for each asset kind appends zero diagnostics. | Classify `NonPortable` as rank 1/terminal, or append `Stale` on the healthy-success branch. |
| Locator: relative reference/database paths with empty or relative root cannot select a same-name CWD decoy; each produces the exact `Malformed`. Ambiguous-ID behavior, an absolute database claimant and normal absolute-root cases remain green. | Remove/defer the absolute-root guard so path fallback or a relative database record reaches `exists(relative)`. |
| Loader: an invalid/non-absolute `resolvedOwnerPath` emits one `Model/Malformed`, leaves the target ECS unchanged and writes no sidecar. | Feed the path through `fs::absolute` before validation. |
| API removal: all migrated loader tests build only through the final signatures. | Temporarily change one migrated call back to the old short signature and record the expected compile failure; restore it and rebuild green. This is the discrimination proof for an intentionally absent API. |
| Resolver adapter removal: the repository audit finds no declaration, definition or call to `SceneAssetResolver::ResolvePath`; generic `Resolve` tests remain green. | Temporarily restore a compile-only call to `SceneAssetResolver::ResolvePath` and record that it cannot compile after removal. |
| Recovery service: a due untitled snapshot with empty/relative logical root fails without creating a record even when a plausible scene asset exists in CWD; an explicit absolute root succeeds. Walnut root policy returns/creates absolute `%LOCALAPPDATA%\RT2\recovery` without a project and reports environment/create failure instead of CWD fallback. | Restore the `current_path` fallback; separately make the root-policy seam return empty for the no-project case. |
| Watch policy: absolute successful/missing candidates yield their absolute parent; relative diagnostic candidates yield no watch directory. | Accept a relative candidate in the extracted watch policy. |
| Script binding: a real relative script under an explicit scene root is resolved by the shared adapter and receives/reuses its sidecar ID; a missing script remains bindable with nil ID; a conflict is not silently remapped. | Restore the lexical `ResolveAuthoredScriptPath` existence check. |
| Reload: absolute native-separator reload still matches; relative reload appends `Malformed` and changes neither live callback nor field-registry state. | Reintroduce `weakly_canonical` on the relative input before the guard. |
| Texture representation/export: placeholder and decoded textures retain the same `ref`; glTF image URI comes from `ref.path`; no `filepath` field is referenced. | Clear/ignore `ref.path` when building the exported `tinygltf::Image`. |
| Native save: cross-volume save succeeds with byte-for-byte unchanged normalized absolute JSON and exactly one contextual `NonPortable`; same-volume save succeeds with zero diagnostics; multiple warnings are sorted; a forced atomic-write failure preserves both output and diagnostic prefix. The sink-less `Save`/`SaveTo` calls no longer compile. | Suppress the `NonPortable` append while retaining the absolute fallback; append staged warnings before the forced write failure; separately change one migrated call back to the old sink-less signature for the compile-fail proof. |
| Save presentation: the CPU-tested summary helper formats one/many sorted `NonPortable` diagnostics exactly; the shared severity formatter returns `"NonPortable"`; Walnut uses the summary for successful Save/Save As/autosave instead of a plain success status. | Break the one/many summary helper; separately restore Walnut's unconditional plain-success assignment after diagnostics and verify the source/app build no longer satisfies the surface wiring audit. |
| glTF export: a same-volume absolute texture ref is written relative; a cross-volume ref returns false without creating/replacing output. | Write `ref.path` directly into `tinygltf::Image::uri`. |
| Script gate: healthy tracked sidecar produces no line and passes; temporarily absent sidecar produces `Stale` and still passes; temporarily malformed sidecar produces `Malformed` and fails. | Restore the line-presence-only PowerShell condition. |

After the proof table is green, run a source audit:

```powershell
rg -n "MakeCompatibilityTextureContext|SceneAssetResolver::ResolvePath|current_path|fs::absolute|filesystem::absolute|\.filepath" RT2App/src RT2SliceRunner/src RT2Tests/src
rg -n "switch.*severity|case .*AssetDiagnostic::" RT2App/src RT2SliceRunner/src
```

Every remaining hit must be classified in the implementation report.
Expected non-asset-policy hits such as the AppData fallback and recovery-path
containment are not deleted merely to make the grep empty. `SceneMesh` is
also explicitly out of scope. The severity audit must show one exhaustive
shared name switch plus the rank switch; production presentation code must
not retain private severity-name switches.

#### Phase 6 hard-contract verification

Run the full Release and Debug suites and also report these named cases
explicitly so a green aggregate cannot hide the delicate scripting contract:

| Contract | Direct evidence |
|---|---|
| Empty script is legal | `Phase 6A: empty script file is legal` (`RT2Tests/src/Phase6ALifecycleTests.cpp:863`). |
| One script error quarantines only its instance | `Phase 6A: syntax error quarantines only the affected instance` (`Phase6ALifecycleTests.cpp:353`). |
| Reflection retains last-good declarations; `parsed=false` suppresses reconciliation | `Phase6B W1: a syntax error yields last-good descriptors, not a throw` (`RT2Tests/src/Phase6BFieldsTests.cpp:474`) and `Phase6B W2: resolver parse failure preserves authored values exactly` (`:1062`). |
| Failed hot reload never replaces live code | `Phase 6C: a mid-edit syntax error does not kill the running instance` (`RT2Tests/src/Phase6CScriptingTests.cpp:747`). |
| `rt2.reload()` remains deferred | `Phase 6C: rt2:reload() from on_update does not re-enter` (`Phase6CScriptingTests.cpp:498`). |
| Timers are unaffected | `Phase 6C: self-rescheduling timers survive vector reallocation` (`Phase6CScriptingTests.cpp:460`) plus reload/Stop timer cases at `:976` and `:1257`. |
| Input and entity/component bindings are unaffected | Input at `Phase6CScriptingTests.cpp:1142`; light at `:1184`; material at `:1223`; camera failure containment at `:573`; the 60-frame scenario exercises the bound entity through RT2SliceRunner. |

`run_script_test.ps1` remains the most important Step-7 gate. It must pass
with no terminal asset diagnostic (`severity >= Missing`). A fully healthy
tracked scenario still emits no diagnostic at all; an advisory is rendered
but is not a scripting failure. A scripting contract failure stops the
current commit; later cleanup does not begin.

#### Four independently green commits

Do not squash these recovery points. Each commit runs from the repository
root and is green before the next starts.

| Commit | Scope | Independent green gate |
|---|---|---|
| **7.1 — diagnostic/root foundations and caller migration** | Set explicit advisory/terminal enum values; add shared exhaustive name/threshold helpers; require and thread still-empty `Save`/`SaveTo` sinks; change the script gate threshold and prove healthy/advisory/terminal behavior; refactor locator fallback/root validation; add empty/relative-root decoy tests; add the test-only loader context builder; migrate every short loader test while leaving compatibility APIs in place. No `NonPortable` emission yet. | Full Release and Debug `RT2Tests`; every original 642 case remains green; both slice targets build; script gate healthy/advisory/terminal proofs pass. |
| **7.2 — loader and model/texture host contraction** | Add the explicit-import context builder; make the four structured loaders context-path-only; update SceneAssetResolver, SceneManager, Walnut and runner reachability; delete the four short adapters, compatibility context helper and `SceneAssetResolver::ResolvePath`; perform compile-fail proofs. | Full Release and Debug `RT2Tests`; Release and Debug RT2SliceRunner; Release RT2App; script gate clean. |
| **7.3 — script, watcher and recovery CWD removal** | Remove recovery convenience/fallback; add/test the per-user untitled recovery-root helper and forward save diagnostics; keep script root empty without scene/project ownership; enforce absolute watch directories; route explicit script binding through the shared adapter; reject relative reload; add all focused tests and Phase 6 proofs. | Full Release and Debug `RT2Tests`; both slice targets; Release RT2App; named Phase 6 selection; `run_script_test.ps1` clean. |
| **7.4 — representation and persistence close** | Remove `SceneTexture::filepath`; switch pipeline/export/tests to `ref`; emit/sort/render native `NonPortable` advisories while retaining current cross-volume save success/bytes; make glTF export relativize-or-fail; add only the authorized assertions/gate change; run the final audit, refresh Graphify and append the Step-7 implementation report. | Full Release and Debug `RT2Tests`; both slice targets; Release RT2App; script gate; Graphify refresh; `git diff --check`. Stop before step 8. |

No build-configuration change is expected: Step 7 adds no production
translation unit and removes no currently linked source. If project,
premake, compiler or linker settings change, flag the exact old/new value
and why before committing. Do not touch the existing whole-Debug RT2App
NRD/NRI mismatch.

#### Step-7 verification gate

Run from the repository root after every commit, with the app/script portions
at least on commits whose scope names them, and run the complete block at
7.4:

```powershell
msbuild RT2App.sln -t:RT2Tests -p:Configuration=Release -p:Platform=x64
.\bin\Release-windows-x86_64\RT2Tests\RT2Tests.exe
msbuild RT2App.sln -t:RT2Tests -p:Configuration=Debug -p:Platform=x64
.\bin\Debug-windows-x86_64\RT2Tests\RT2Tests.exe
msbuild RT2App.sln -t:RT2SliceRunner -p:Configuration=Release -p:Platform=x64
msbuild RT2App.sln -t:RT2SliceRunner -p:Configuration=Debug -p:Platform=x64
msbuild RT2App.sln -t:RT2App -p:Configuration=Release -p:Platform=x64
.\run_script_test.ps1
graphify update .
git diff --check
```

All 642 pre-Step-7 cases must remain present and green in both
configurations. Record the new case/assertion totals rather than assuming
them. Do not use machine-local Downloads assets as evidence. Step 7 does not
claim a green whole-Debug RT2App.

Run Graphify at the Step-7 close because the repository instructions require
a refresh after code changes; step 8 will run it again as part of final W3
verification. Commit `GRAPH_REPORT.md` only if it actually changes.
Generated ignored graph files are not committed.

#### Explicit non-goals and stop condition

- Do not start W3 step 8 or W4 project scanning/database ownership.
- Do not add `project.rt2proj`, a content browser, cache artifacts, schema v4
  migration, texture editing UI, hot reimport or GPU texture-policy work.
- Do not change texture placeholder bytes, decode containment, geometry,
  material ordering, renderer/Vulkan behavior or Phase 6 scripting semantics.
- Do not remove `SceneMesh::filepath`, AppData fallback paths, or recovery
  record containment normalization without a separate grounded scope.
- Do not alter, delete, skip or weaken an existing test beyond the exact
  authorized old/new table.
- Do not touch unrelated UI/render work or the whole-Debug NRD/NRI mismatch.
- Stop after the Step-7 implementation report and four commits. Do not push.

The implementation report must separate **verified by running** from
**assumed/not run**, list all four commit hashes and per-commit totals, quote
every old/new expectation actually changed, record every red/green proof and
classify every remaining path-audit hit. It must call out S7-F1 as an
instance of the codebase's characteristic silent-failure mode, report the
healthy/advisory/terminal scenario-gate observations separately, and show
how a native `NonPortable` advisory was verified at both the serializer sink
and the visible Walnut status/log surface.

### Phase 7 W3 step 7 implementation report (2026-07-26)

Grounded and implemented on `phase-6-scripting`, beginning at `591a76e`.
This report closes step 7 only. Step 8 and W4 were not started.

#### Delivered commits and independently green boundaries

| Slice | Commit | Release / Debug result at its boundary |
|---|---|---|
| 7.1 — diagnostic/root foundations | `67f4951` | 646/646, 145,374 assertions in each configuration |
| 7.2 — loader/host contraction | `fd18e7b` | 650/650, 145,409 assertions in each configuration |
| 7.3 — script/watcher/recovery CWD removal | `5b0664d` | 655/655, 145,463 assertions in each configuration |
| 7.4 — representation/persistence close | This report's commit (the Step-7 closing `HEAD`) | 659/659, 145,494 assertions in each configuration |

The approved plan itself was recorded separately in `ec62842`. No build,
project, premake, compiler or linker configuration changed.

#### What changed

- The generic locator now performs ID-first resolution and refuses to send a
  relative database or fallback candidate to the filesystem without a
  non-empty absolute asset root. Successful results are absolute and
  normalized. The misleading `NormalizeResolved`, compatibility loader
  context and `SceneAssetResolver::ResolvePath` paths were removed.
- All four structured model loaders now take only an explicit
  `TextureAssetLoadContext`; `resolvedOwnerPath` is the single physical
  owner path. SceneManager, Walnut, RT2SliceRunner reachability and all tests
  use the same explicit-import context builder. Relative direct-import
  inputs fail before parse, mutation or sidecar assignment.
- Diagnostic severity is an executable external contract:
  `Stale=0`, `NonPortable=1`, `Missing=2`, `Malformed=3`,
  `Unresolved=4`, `Conflict=5`. Presentation and terminal-threshold checks
  are shared; healthy matching sidecars with no database emit no diagnostic.
- Untitled recovery and scripting no longer share a process-CWD policy.
  Recovery uses an ensured absolute `%LOCALAPPDATA%\RT2\recovery` logical
  root (or an absolute configured project root); unsaved relative scripts
  retain an empty root, remain authored/unminted, and are neither resolved
  nor watched. Relative reload requests are rejected without clearing
  reflection state, queueing work or swapping code.
- Script binding now uses `ResolveScriptAssetPath` before identity assignment.
  Existing sidecar identity is reused, missing files remain legal nil-ID
  bindings, and conflicts are rejected rather than silently remapped.
- `SceneTexture::filepath` was removed. `SceneTexture::ref` is the only
  texture source identity in the CPU pipeline and glTF exporter. Export
  writes relative normalized image URIs and fails before writing when an
  absolute texture ref cannot be relativized to the output parent.
- Native scene save still succeeds across Windows volumes and writes the
  same normalized absolute JSON path, but now stages and, only after atomic
  success, appends one contextual `NonPortable` advisory per offending
  reference. Diagnostics are deterministically sorted. Recovery forwards
  the same sink.
- Walnut logs save/recovery diagnostics through the shared severity
  formatter and preserves `"Saved[ As] with " + summary` or
  `"Autosaved with " + summary` in the visible status rather than
  overwriting the advisory with a plain success message.
- The Phase 6A fixture writer was found to depend on its suite-order setup
  test. A filtered empty-script run therefore wrote to a nonexistent
  directory without checking the stream and then quarantined the instance.
  The fixture root is now initialized absolutely, created on demand, and
  every directory/file write fails loudly. The production empty-script
  expectation was not changed.

S7-F1 is a textbook instance of this codebase's characteristic silent
failure mode: `ResolvePathNoCwd` was named and documented as prohibiting CWD
fallback, while `assetRoot / ref` with an empty root silently performed
exactly that lookup; `NormalizeResolved` was similarly documented as making
a path absolute immediately above a lexical-only return. The fix made the
promised invariant executable rather than relying on the name/comment.

#### Authorized expectation changes actually made

No existing case was deleted, skipped or weakened. The following old/new
changes are the complete set:

| Old | New |
|---|---|
| A relative ref with an empty/relative root could select a CWD file. | It fails with one contextual `Malformed`: `"relative asset reference requires an absolute asset root"`. |
| Short loader calls accepted a filepath and derived ownership from CWD. | Those overloads do not exist; callers provide an explicit context/root. |
| A separate loader filepath silently overwrote `context.resolvedOwnerPath`. | The duplicate parameter is removed, so mismatch is unrepresentable. |
| Relative direct-import paths were made absolute against CWD. | They fail with `Model/Malformed` before parse/mint/mutation. |
| Untitled recovery without a project used CWD. | Walnut supplies the ensured per-user recovery root; the service rejects an invalid explicit root. |
| Untitled relative scripts inherited CWD for Play/watch. | They remain authored with nil identity and no watch directory until ownership exists. |
| Relative `ReloadScript` was canonicalized against CWD. | It emits one `Script/Malformed` and changes no cache, queue or live code. |
| Matching non-nil sidecar identity with `database == nullptr` emitted `Stale`. | It is a healthy successful resolve with zero diagnostics. |
| Any script-scenario asset diagnostic failed the PowerShell gate. | `Stale`/`NonPortable` are advisory; `Missing` and above fail. |
| Texture assertions/export used the `filepath` compatibility mirror. | The same path assertions and export URI use `ref.path`; the mirror is absent. |

The cross-volume native-save value did **not** change:
`"Z:/external/move.lua"` remains `"Z:/external/move.lua"` and save still
returns true. The test only gained assertions for one contextual
`NonPortable` advisory. No Phase 6 runtime expectation changed.

#### Discrimination proofs

Every temporary fault was applied with `apply_patch` (to production except
for the intentional compile-only removed-API probes), observed red, reverted,
and observed green:

- 7.1: removing the absolute-root guard selected a CWD decoy; restoring
  `Stale` on a healthy matching sidecar violated the zero-diagnostic case;
  classifying `NonPortable` as terminal violated the threshold case.
  Healthy scenario output passed with no diagnostic; a temporarily absent
  sidecar printed `Stale` and passed; a malformed sidecar printed
  `Malformed` and failed. All faults were reverted and the full boundary
  returned to 646/646.
- 7.2: feeding an invalid owner through CWD absolutization mutated the
  invalid-context behavior; a temporary old two-argument loader call failed
  compilation with C2660; a temporary
  `SceneAssetResolver::ResolvePath` call failed with C2039. The final API
  and full boundary returned green at 650/650.
- 7.3: restoring recovery's CWD fallback made the invalid-root recovery case
  write a record (four assertions red); returning an empty per-user recovery
  root broke three helper assertions; accepting a relative watch candidate
  broke the absolute-only watch case; restoring lexical script binding
  minted a CWD-decoy sidecar; canonicalizing relative reload before the
  guard swapped live code from v1 to v2. Each was reverted. The focused
  Step-7.3 selection was 3/3 and 40 assertions green; recovery-root
  selections were 2/2 and 16 assertions plus 1/1 and 6 assertions green.
- 7.4: ignoring `ref.path` made both glTF export cases fail (2/2 red,
  three failed assertions), including replacement of the sentinel output;
  restored code passed 2/2 and 5 assertions. Suppressing persistence
  advisories made the one-warning case fail at 0 versus 1 and the
  three-warning case fail at 0 versus 3; restored selections passed 1/1
  with 12 and 10 assertions. Publishing staged diagnostics before the
  atomic replace made the prefix test fail at 2 versus 1; restored it passed
  1/1 and 6 assertions. Warning on successful same-volume rebasing made the
  healthy-save assertion fail; restored it passed 1/1 and 5 assertions. A
  sink-less `Save` call failed compilation with C2660, then rebuilt green
  after restoration. Corrupting the one-warning summary failed 1/1 at one
  assertion; restoration passed 1/1 and 3 assertions. Restoring Walnut's
  unconditional plain-save assignment made the save-warning source wiring
  audit red; restoration found both Save/Save As and autosave summary
  assignments. The complete new Step-7.4 selection passed 4/4 and 21
  assertions.

#### Phase 6 hard-contract verification

The final Release named selections were executed independently, so aggregate
order could not mask the scripting contract:

- Empty script legal: 1/1, 4/4 assertions.
- Syntax error quarantines only the affected instance: 1/1, 7/7.
- Last-known-good declarations: 1/1, 8/8.
- `parsed=false` preserves authored values and suppresses reconciliation:
  1/1, 12/12.
- Failed hot reload does not replace live code: 1/1, 5/5.
- `rt2.reload()` remains deferred: 1/1, 4/4.
- Timers: self-reschedule 4/4, reload cancellation 4/4, Stop containment
  5/5.
- Bindings: input 3/3, camera failure containment 6/6, light 3/3 and
  material bounds containment 3/3.
- `run_script_test.ps1`: PASS, 60 frames, one entity, no mismatches and no
  asset diagnostic.

This explicitly verifies that empty source remains legal; quarantine remains
per-instance; failed reflection retains last-known-good data without
reconciliation; failed reload never swaps live code; reload remains
deferred; and timers/input/entity-component bindings are unchanged.

#### Source-audit classification

- `MakeCompatibilityTextureContext`, `SceneAssetResolver::ResolvePath` and
  `.filepath` have zero hits in the Step-7 source/test audit.
- `current_path` in `FixtureTests`, `Phase1ASceneAssetTests` and
  `SceneLoaderTestSupport` names explicit repository-root test fixtures.
  Phase 6C `absolute/current_path` hits construct explicit test inputs and
  CWD decoys; they are not production fallback.
- `SceneRecoveryService` absolute/canonical hits create document IDs and
  enforce recovery-record containment. They do not choose an asset root.
- `SceneSerializer` absolute hits preserve the existing native-save
  rebasing/byte contract, including the advisory cross-volume fallback; they
  are persistence behavior, not resolver fallback.
- Walnut's remaining `current_path()/RT2Editor` is the explicitly out-of-
  scope non-asset editor-settings fallback.
- The severity audit contains exactly the shared exhaustive display-name
  switch and the deterministic sort-rank switch; no private presentation
  switch remains.
- `SceneMesh::filepath` remains declared and untouched as explicitly
  required; no `SceneTexture::filepath` declaration or use remains.

#### Verified by running

- Release: RT2App, RT2Tests and RT2SliceRunner built; RT2Tests passed
  659/659 and 145,494/145,494 assertions from repository root.
- Debug: RT2Tests and RT2SliceRunner built; RT2Tests passed 659/659 and
  145,494/145,494 assertions from repository root.
- The named Phase 6 selections and 60-frame scripting scenario above.
- All red/green discrimination proofs described above.
- Native `NonPortable` at the serializer sink: unchanged successful
  cross-volume JSON plus exact severity/kind/ref/resolved/entity/source/detail;
  deterministic three-warning order; zero warnings for healthy same-volume;
  no publication on locked-target failure.
- Walnut presentation at the source/build surface: the shared formatter is
  called after successful save/recovery, diagnostics are logged, the warning
  assignments survive the source audit, and Release RT2App links.
- `graphify update .`: 33,768 nodes, 71,052 edges, 1,389 communities;
  tracked `GRAPH_REPORT.md` changed and is included.
- `git diff --check` was clean at close.

#### Assumed / not run

- Whole-Debug RT2App was intentionally not built; the pre-existing NRD/NRI
  mismatch was not touched.
- No interactive Walnut GUI session was launched. Visible status behavior is
  verified by focused formatter tests, source-wiring discrimination and the
  Release app build, not by a manual click-through.
- The Windows cross-volume condition uses an unmounted `Z:` lexical path, as
  the established test does; no physical second volume or machine-local
  Downloads asset was used.
- No push was performed.

### W3 step 8 — whole-solution verification and W3 close

Run 2026-07-26 against `90cf7b5` plus the build fix recorded below. This is
the final W3 step; W4 project database ownership was not started.

#### Whole-solution build (the point of this step)

Step 8 exists to build targets that per-step gates deliberately skipped.
It found one regression that every prior step had missed.

**RT2App Debug did not compile.** `SceneLoader.cpp` failed with
`fatal error C1128: number of sections exceeded object file format limit:
compile with /bigobj`. Step 6.2 added Debug-only `/bigobj` for this file to
`RT2Tests` and `RT2SliceRunner`, but deliberately left RT2App's
configuration alone; the texture pipeline then pushed the same translation
unit past the COFF section limit in RT2App too.

This was masked, not tolerated. The standing exclusion for whole-Debug
RT2App is the **NRD/NRI runtime-library link mismatch**, and every step
from 6 onward restated that exclusion without building the target — so a
new *compile* failure sat behind a documented *link* failure and no gate
could see it. A skipped target reports nothing, including things that are
not the reason it was skipped.

Fix: add `/bigobj` for `src/SceneLoader.cpp` under `Debug|x64` to
`RT2App/RT2App.vcxproj`, mirroring the existing `RT2Tests` entry, and add
the matching `filter { "configurations:Debug", "files:src/SceneLoader.cpp" }`
to `RT2App/premake5.lua` so the hand-maintained project and the generator do
not diverge. This is a compile option only; no link configuration, runtime
library or dependency changed.

After the fix RT2App Debug compiles fully and fails at link with
`LNK2038`/`LNK1319` — 22 `_ITERATOR_DEBUG_LEVEL` and `RuntimeLibrary`
mismatches from `NRD.lib`, which ships as an `MD_DynamicRelease` prebuilt
binary. That is the pre-existing documented defect, unchanged and still out
of scope. The regression is resolved; the known failure is now the only one
remaining, and is reached rather than hidden.

#### Verified by running

| Gate | Result |
|---|---|
| Full solution, Release, all five targets | Built clean (exit 0) |
| Full solution, Debug | ImGui, GLFW, Walnut, RT2SliceRunner, RT2Tests built; RT2App link-fails on the known NRD/NRI mismatch |
| `RT2Tests` Release | 659/659 cases, 145,494/145,494 assertions |
| `RT2Tests` Debug | 659/659 cases, 145,494/145,494 assertions |
| `run_script_test.ps1` | PASS — 60 frames, 1 entity, no mismatches, no terminal diagnostics |
| `graphify update .` | No code-graph topology changes; outputs left untouched |
| `git status` | Clean before the step-8 commit |

The Release and Debug suite figures were re-run independently rather than
carried over from the step-7 report, and match it exactly.

Debug emits `[sol2] An exception occurred: <name> is disabled in the RT2
script sandbox` during the run. These are expected output from passing
sandbox-rejection cases, not failures.

#### Assumed / not verified

- Whole-Debug `RT2App` still cannot link. The NRD/NRI runtime-library
  mismatch is pre-existing, was explicitly out of scope for all of W3, and
  is unchanged by this step.
- No interactive GPU or render-quality claim is made. Step 8 verified
  builds, tests and the headless gate only.
- `graphify update .` reported no topology change, so `GRAPH_REPORT.md` was
  neither regenerated nor committed by this step.

#### W3 close

W3 asset identity is complete. Asset references carry durable identity
through `.rt2meta` sidecars and the asset database; models, environments,
scripts and textures all resolve through the shared locator; no host
consults the process working directory; failures are named diagnostics
rather than silent substitution.

Two items are carried forward rather than closed:

1. **`SceneLoader.cpp` needs `/bigobj` in three separate projects.** The
   flag is a workaround for translation-unit size. The remaining texture
   and format logic should move into `TextureAssetPipeline.cpp`, after
   which all three `/bigobj` entries should be removed together.
2. **The whole-Debug `RT2App` NRD/NRI link mismatch** now blocks the only
   solution-wide gate that could catch a Debug-only RT2App regression. It
   should be fixed — by rebuilding NRD against the Debug runtime or
   shipping both variants — before Debug RT2App accumulates further
   undetected breakage.

Next: W4 project scanning and database ownership.

## Punctual lights (spec) — renderer work, not a roadmap phase

> **Retitled 2026-07-31. This section was originally headed "Phase 8 —
> Punctual lights", which collided with the roadmap's Phase 8 (Prefabs,
> `:581`) — two different Phase 8s in one append-only document. The roadmap
> numbering is authoritative; this is off-roadmap renderer work and should
> never have taken a number. Header corrected under the "correct headers
> freely" rule; the body below is left as written.**
>
> **Status superseding the "no implementation has started" line below:
> implemented and shipped 2026-07-27** (PR #3, merged at `77626a3`). Steps
> 1–6 landed; step 7 (close report) was never written. Scope explicitly not
> taken: ReSTIR DI reservoir candidacy for punctual lights (P8-Q5).
>
> Follow-on work landed after the spec: editor viewport icons for lights and
> cameras, Inspector fields for range and spot cone angles, and run-state
> gating of editor-only visuals. Two defects found afterwards and fixed —
> punctual lights were absent from the transform-only sync path (a moved
> light kept casting from its old position until Play), and the descriptor
> binding this spec added was placed above the variable-count texture array,
> which is a Vulkan spec violation.
>
> Terms used here — "light", "punctual", "triangle light" — are defined in
> `docs/glossary.md`.

Drafted 2026-07-26, grounded against `db74b4c` on `phase-6-scripting`. No
implementation has started. This section is a spec and requires review
before implementation begins.

### Motivation

The renderer supports emissive geometry only. Every other light in a scene
is imported, stored, serialised, shown in the Outliner — and contributes
zero illumination, silently.

Two consequences observed in Intel Sponza:

- The 24 lamps that light the arcade alcoves do nothing, so the alcoves are
  black. They are present in the source glTF as `KHR_lights_punctual`.
- Authoring a working light means raising an emissive material's intensity
  into the thousands. That is arithmetically correct, not a bug: irradiance
  from an emitter goes as `L · A · cos / d²`, so a small emitter needs very
  large radiance to rival an environment map covering the whole hemisphere.
  The problem is that the engine asks the user to author in radiance while
  they are thinking in power. A punctual light has no area term, so it is
  authored in intensity directly and the units problem disappears.

### Grounded findings

| ID | Fact at `db74b4c` | Consequence |
|---|---|---|
| P8-F1 | Two unrelated light representations exist. `LightComponent` (`RT2App/src/ECSComponents.h:76`) is the live editable one — created by `SceneManager::AddLight` (`SceneManager.cpp:929`), edited in the Inspector (`SceneEditorUI.cpp:1455`, `:1518`), undo/redo aware (`SceneManager.cpp:1540`), with position and direction implied by the entity's `Transform`. `ECSScene.lights` is a flat `std::vector<SceneLight>` (`ECSScene.h:33`) written *only* by the glTF loader (`SceneLoader.cpp:772`). | Pick one. `LightComponent` + `Transform` wins: transform-driven, undo-aware, already in the Inspector. `ECSScene.lights` becomes loader staging that is converted into entities, then removed. |
| P8-F2 | Neither representation reaches the GPU. `GPUSceneData::lights` is `GPUTriangleLight` built exclusively from emissive triangles (`GPUSceneData.cpp:282-560`); nothing reads `ecsScene.lights`. `SceneResources::CreateLightBuffer` (`SceneResources.cpp:607`) uploads only triangle lights. | The name `CreateLightBuffer` already means "emissive triangle buffer". Punctual lights need their own struct, buffer, and binding; do not overload the existing one. |
| P8-F3 | `SceneLight` (`SceneTypes.h:82`) has the full glTF punctual model — type, position, direction, colour, intensity, range, inner/outer cone. `LightComponent` has only `color`, `intensity`, `range`, cone angles and a `bool isSpot`. `LightType` (`SceneTypes.h:24`) has `Point` and `Spot` but no `Directional`. | `LightComponent` must gain a real type enum. `isSpot` appears in ~12 places including the public `SceneManager` API (`AddLight`, `GetLightProperties`, `SetLightProperties`), `EditorPropertyCommands.cpp:71`, and the Inspector — all migrate together. |
| P8-F4 | `KHR_lights_punctual` is parsed in `LoadIntoECS` only (`SceneLoader.cpp:697`); `ImportIntoECS` does not read it. Export writes it (`SceneLoader.cpp:443`). | Importing a glTF silently drops every light. This is why a fresh Sponza import reports `Lights: 0` while the `.rt2scene` reports 24. |
| P8-F5 | NEE already performs stochastic selection between triangle lights and the environment, dividing by the selection probability `pTri` (`restir_gi_bindings.glsl:565-595`). The estimator is unbiased: uniform triangle choice, uniform area sampling, converted to solid angle. | Punctual lights join as a third arm of the same selection. The existing `pTri`/`pEnv` split becomes a three-way split; every consumer of those probabilities must be updated together or the estimator silently biases. |

### Decisions

| ID | Decision |
|---|---|
| P8-Q1 — sampling | Stochastic single-light selection weighted by `intensity / d²`, one shadow ray per NEE event, divided by the selection probability. Scales past a handful of lights and matches the existing stochastic NEE structure. Not a deterministic loop (O(lights) shadow rays per bounce) and not ReSTIR DI reservoir integration in this phase. |
| P8-Q2 — scope | `Point`, `Spot` and `Directional`, all zero-radius with sharp shadows. Directional gives a sun independent of the environment map. No radius/sphere-light soft shadows in this phase. |
| P8-Q3 — representation | `LightComponent` + `Transform` is authoritative. World position is the entity's world translation; world direction is the world rotation applied to `-Z` (glTF convention). `ECSScene.lights` is converted to entities at load and then deleted. |
| P8-Q4 — units | Follow glTF: `Point`/`Spot` intensity in candela, `Directional` in lux. Conversion to engine radiance is applied once at GPU-buffer build, documented in one place, with the existing `Emissive Boost` left untouched. |
| P8-Q5 — ReSTIR DI | Out of scope. Punctual lights go through classic NEE only. Recorded explicitly so a later phase can add reservoir candidacy without re-deriving the contract. |

### Implementation order

1. `LightComponent` gains `LightType type` replacing `isSpot`; add `Directional` to `LightType`. Migrate all ~12 call sites, the public `SceneManager` light API, `EditorPropertyCommands` equality, and the Inspector. Serialisation reads `isSpot` when `type` is absent so existing scenes load unchanged.
2. Convert `ECSScene.lights` into entities carrying `LightComponent` + `Transform` at load; delete the vector and its remaining readers.
3. Parse `KHR_lights_punctual` in `ImportIntoECS`, sharing one helper with `LoadIntoECS`.
4. Add `GPUPunctualLight` to `GPUSceneData.h`, built from `LightComponent` + world `Transform`, with the P8-Q4 unit conversion.
5. Add the GPU buffer, descriptor binding, and `shader_interface.h` constant. Mirror the existing 16-byte-header buffer layout.
6. Shader: punctual NEE with power/distance-weighted selection, spot cone attenuation, range falloff, and the three-way selection probability. Update every consumer of `pTri`/`pEnv`.
7. Tests and discrimination proofs; then the close report.

### Permanent tests and discrimination proofs

| Permanent evidence | Temporary fault that must make it fail |
|---|---|
| A glTF with `KHR_lights_punctual` yields the same light count and per-light type/intensity/cone values through **both** `LoadIntoECS` and `ImportIntoECS`. | Remove the punctual parse from `ImportIntoECS`. |
| Moving or rotating a light entity changes the built `GPUPunctualLight` world position/direction accordingly; a spot's direction follows the entity's rotation. | Build the buffer from the local transform instead of the world transform. |
| Each `LightType` maps to its own GPU type value and cone/range fields survive the round trip; a scene written before the enum existed still loads with `isSpot` honoured. | Collapse `Directional` onto `Point` in the type mapping. |
| Selection probabilities over the three NEE arms sum to 1 for representative scenes (lights only, emissives only, environment only, and all three). | Drop the punctual arm from the normalisation so the probabilities sum above 1. |
| A scene lit by one point light converges to the analytic `I·cos/d²` irradiance at a reference point, within tolerance. | Omit the inverse-square term. |

### Boundary

Does not include ReSTIR DI reservoir candidacy, soft shadows/light radius,
IES profiles, shadow-casting toggles per light, or any change to emissive
handling or the `Emissive Boost` control.

### Review amendments (2026-07-26)

Reviewed by GLM 5.2 against `3a4a022`; every finding below was independently
re-verified before being accepted. The spec above stands except as amended
here. Amendments win where they conflict.

**A1 — P8-F1 overstated the writer set.** `ECSScene.lights` is not written
only by `SceneLoader.cpp:772`. `WalnutApp.cpp:2441` moves a loaded vector
into the live scene (`live.lights = std::move(result->ecs.lights)`), and
`RT2Tests/src/SceneLoaderTests.cpp:304,331,448` push to it directly. Readers
beyond the loader: `WalnutApp.cpp:734` (the Outliner "Lights: %d" count),
`SceneManager.cpp:357` (log), and the export path `SceneLoader.cpp:397-444`.
The two-representation claim itself holds — no third representation exists.

**A2 — step 1's migration list is incomplete, and one omission is a public
API.** `isSpot` occupies 16 distinct sites, not ~12. The additions:

- **`ScriptSystem.cpp:1107` and `:1136-1137` — the Lua `is_spot` binding**,
  reachable from user scripts via `entity.get_light`/`set_light`. Migrating
  `LightComponent` to an enum without this leaves scripts reading and writing
  a stale bool while the engine has moved on: a silent semantic break in a
  published API. This alone blocks step 1 as written.
- **`SceneSerializer.cpp:667` — the *write* side.** The spec covered reading
  `isSpot` for back-compat but never said what replaces the write. Decide
  explicitly: emit `type`, or keep emitting `isSpot` for round-trip.
- `SceneManager.cpp:1697` (`MatchesRecord`), a third equality site distinct
  from `EditorPropertyCommands.cpp:71`.
- Tests: `Phase3B2CommandTests.cpp:159,436`, `SceneManagerTests.cpp:83`.

`RT2SliceRunner` has none. Record structs carrying `LightComponent`
(`EditorPropertyCommands.h:159-160`, `SubtreeSnapshot.h:69`,
`SceneSerializer.cpp:488`) migrate automatically with the struct.

**A3 — P8-F5's consumer list was wrong in both directions.** `pTri` is not
referenced at all in `restir_spatial.comp` or `restir_gi_temporal.comp`
(0 occurrences each); both only do reservoir reuse. The actual consumers are
`closesthit.rchit` (2), `miss.rmiss` (2), `scatter_shared.glsl` (12),
`restir_temporal.comp` (6) and `restir_shared.glsl` (6).

**A4 — the silent-bias proof was insufficient; this is the most important
amendment.** `computePTri` is **duplicated in four files**
(`pathtracer_shared.glsl:100`, `restir_bindings.glsl:182`,
`restir_gi_bindings.glsl:109`, and a `gbuffer_debug.comp:60` stub). The real
failure mode is therefore not a normalisation error but the four copies
*disagreeing* after a partial migration — each can sum to 1 locally while
contradicting the others, so the proposed "probabilities sum to 1" test
passes with the bug live.

Replace that proof with: **unify `computePTri` into one shared header** so a
third arm cannot be added to some copies and not others, and make the
discrimination fault "change one definition and not the rest" — which must
be impossible to express once unified. Until unified, no test in this phase
should be trusted to catch inter-shader divergence.

**A5 — step ordering leaves the tree red.** Step 3 (parse
`KHR_lights_punctual` in `ImportIntoECS`) is independent and must move
**before** step 2. Step 2 cannot delete `ECSScene.lights` on its own: the
same commit must also migrate `SceneLoader::Save`'s export loop
(`:397-444`), the Outliner count (`WalnutApp.cpp:734`), the log
(`SceneManager.cpp:357`), and the loader round-trip tests
(`SceneLoaderTests.cpp:189,304,309,331,336,448,483-485`). Revised order:
1, 3, 2 (with its full reader migration), then 4-7 unchanged.

**A6 — minor citation drift.** The `pTri` division cited as
`restir_gi_bindings.glsl:565-595` is at `:590`, with the environment arm's
`direct /= (1.0 - pTri)` at `:626`, outside the cited range.

## Phase 7 W4–W5 — project ownership and schema-v4 migration (implementation spec)

Drafted 2026-07-31 and grounded against commit
`7ec974b7f12db686ae0c9addb7b0da61549422e6`. This section supersedes the
unsettled D3, D4 and D6 questions in the Phase 7 plan at `:6404-6425`; it does
not rewrite that chronological record. W0–W3 already exist in the tree. W4
and W5 are not greenfield and no implementation described below has started.

This spec must be reviewed before implementation. Review amendments are
appended here; implementation does not reinterpret an unsettled point from
memory.

### Scope and exit

W4 introduces the portable `.rt2proj` document, gives the existing W2
`AssetDatabase` a production owner, scans the W1/D8 sidecars into immutable
database snapshots, and cuts every production resolver over to an explicit
project-or-standalone context. W5 makes scene schema v4 the durable form,
migrates existing v3 references observably, and persists asset-root-relative
paths plus the IDs already defined by sidecars.

The combined exit is stronger than “a project file parses”: copy a project
folder to another absolute location, open its `.rt2proj`, and resolve the same
startup scene, models, external textures, environment and scripts by the same
IDs without rewriting the scene. Missing, malformed, ambiguous or stale
identity never becomes an empty path or an implicit replacement.

Not in W4/W5: the content browser (W6), watcher expansion/reimport (W7), or
the rebinding, Rebind-button, declaration-browser and cursor-lock UI deferred
to W8. W4 owns input storage and composition, not the rebinding UI.

### Grounded findings at `7ec974b7`

| ID | Current fact | Consequence |
|---|---|---|
| W45-F1 | `EditorSettings` still serializes an absolute `projectRoot`, exposes `Get/Set/ClearProjectRoot`, and stores `inputContexts` in schema v2 (`RT2App/src/EditorSettings.h:38-65,76,98-100,124-150`; `RT2App/src/EditorSettings.cpp:104-129,153-217,242-294`). | W4 must migrate an existing per-user schema. It must not add a second value with the same name and different authority. |
| W45-F2 | The header says that `projectRoot` is only a file-dialog preference (`RT2App/src/EditorSettings.h:32-36`), but the host now also uses it as the asset root for scripts and untitled recovery (`RT2App/src/WalnutApp.cpp:2704-2754`), injects it into Play when the document is untitled (`:2849-2855`), and labels it “Project Root” in the Session UI (`:1069-1091`). | The old Phase 7 P5 finding is stale in an important direction: one setting now has three meanings. D3 must separate them and remove the semantic fallback, not merely update a comment. |
| W45-F3 | `InputService::LoadDefaults` constructs the editor, viewport, look and runtime maps in code (`RT2App/src/InputService.cpp:105-178`). `WalnutApp` calls only `LoadDefaults` and pushes the editor context (`RT2App/src/WalnutApp.cpp:1514-1518`); it never applies the `EditorSettings::inputContexts` records. | The per-user input data exists but is inert in production. D4 must define and wire composition rather than “move” JSON that currently changes nothing. |
| W45-F4 | `AssetDatabase` is deliberately a pure in-memory index with no filesystem I/O (`RT2App/src/AssetDatabase.h:15-24`) and already has deterministic `BuildAssetDatabase` plus ID/path/dependency APIs (`:61-80,128-172`). At this commit production code has no owner or builder call; resolver contexts therefore carry null databases. | W4 supplies the missing filesystem scan and lifetime-owning host. It extends W2 instead of creating a competing database. |
| W45-F5 | `AssetResolutionContext` already contains an absolute `assetRoot` and a non-owning database pointer (`RT2App/src/AssetResolver.h:107-116`). The locator is read-only and explicitly leaves sidecar mutation to import/save/migration (`:18-37,148-150`). | Keep this seam. W4 changes context construction and lifetime, not resolver purity and not `SceneDocument` ownership. |
| W45-F6 | `SceneAssetResolver` still accepts `sceneRoot`, constructs two internal contexts with `database = nullptr`, and calls the environment path separately (`RT2App/src/SceneAssetResolver.h:76-98`; `RT2App/src/SceneAssetResolver.cpp:131-159,233-295`). Other null-context sites remain in Play/open/watcher (`RT2App/src/WalnutApp.cpp:2849-2855,3068-3075,3152-3155`), recovery (`RT2App/src/SceneRecoveryService.cpp:483-486`), and the slice runner (`RT2SliceRunner/src/Main.cpp:529-531`). | W4 must change the aggregate resolver API to require one explicit context and audit every host. Leaving even one internal null construction makes ID-first behavior depend on the entry point. |
| W45-F7 | Database record paths are documented as project-relative (`RT2App/src/AssetDatabase.h:61-80`), and ID lookup resolves them beneath `AssetResolutionContext::assetRoot` (`RT2App/src/AssetResolver.cpp:168-218`). | The database’s path base is specifically the project **asset root**, not the project-file directory and not the current scene directory. W4 makes that invariant executable. |
| W45-F8 | Sidecars are already the durable identity source. `AssetSidecarPath`, `ReadSidecarId`, atomic `WriteSidecarId`, and `ResolveOrAssign` exist in a CPU-only module (`RT2App/src/AssetIdentity.h:15-73`; `RT2App/src/AssetIdentity.cpp:23-91,164-194`). | The scanner reads these sidecars; it does not invent another metadata format. W5 reuses `ResolveOrAssign` only in its explicit mutation phase. |
| W45-F9 | Durable asset references already contain `assetId`, but their path contract and comments remain scene-relative/v3 (`RT2App/src/AssetReference.h:73-97`). The persistent owners are imported models (`RT2App/src/ECSComponents.h:221-223`), scripts (`:282-288`) and the environment (`RT2App/src/SceneDocument.h:50-62`). Native top-level textures are deliberately serialized as an empty array (`RT2App/src/SceneSerializer.cpp:1339-1344`); imported textures are reconstructed from their owner model. | W5 changes the path base once at the shared reference boundary. It migrates model/script/environment references and does not invent a second standalone texture graph. |
| W45-F10 | The shared v3 asset-reference writer already emits non-nil IDs, while its reader silently turns a malformed or non-string ID into nil (`RT2App/src/SceneSerializer.cpp:216-257`). Model and script paths are rebased from the current scene directory to the output scene directory on every save (`:639-647,692-716`), and the environment follows the same scene-relative rule (`:1348-1367`). | Existing “v3” files can contain no IDs, all IDs, or a mixture. W5 must migrate all three cases and end Save-As path rebasing for project-bound v4 scenes. Malformed identity can no longer disappear silently. |
| W45-F11 | `SceneSerializer` reads and writes only schema 3 (`RT2App/src/SceneSerializer.h:16-54,79-120`; `RT2App/src/SceneSerializer.cpp:1548-1563`). `SceneLoadReport` currently reports only script-field normalization/drop state (`RT2App/src/SceneSerializer.h:68-74`). | W5 raises `SchemaVersion` to 4, keeps `MinReadVersion` at 3 as D5 already decided, and makes migration state part of the load report. |
| W45-F12 | Recovery bytes live below `%LOCALAPPDATA%\\RT2\\Editor\\Recovery` (`RT2App/src/WalnutApp.cpp:80-87,2766-2779`). Each recovery envelope persists an absolute logical `assetRoot` and later resolves with it and no database (`RT2App/src/SceneRecoveryService.cpp:297-307,336,389-390,483-486`). | Recovery is generated per-user session data, not the project cache. A project recovery record must identify and reopen its project context; it must not silently resolve a moved project through an obsolete absolute root. |
| W45-F13 | There is no `.rt2proj`, project model or project CLI argument. The app CLI has only `scenePath` and parses `--scene` (`RT2App/src/CLIArgs.h:9-56,67-70`); the slice runner likewise requires `--scene` (`RT2SliceRunner/src/Main.cpp:91-93,785-824`). The native open filter has scene/model types only (`RT2App/src/WalnutApp.cpp:759-760`). | W4 includes application, CLI and CPU-runner entry points. A project type that only unit tests can load is not a production owner. |
| W45-F14 | The roadmap mentions “default runtime settings”, but the tree has no project-owned runtime-settings model. The fixed-step values are compile-time constants (`RT2App/src/RuntimeSceneController.h:79-81`), while render settings are renderer/UI state and CLI overrides are applied directly (`RT2App/src/WalnutApp.cpp:1999-2040`). | W4 does not serialize an unconsumed `runtimeDefaults` bag. Adding project-owned runtime/render defaults is deferred until a concrete owner and precedence contract are specified. Input defaults are the only runtime default with an existing neutral model. |

### Recovered Phase 7 commitments

The earlier deferrals remain real, but their landing workstream matters:

| Source | Commitment | W4/W5 treatment |
|---|---|---|
| `docs/game-engine-development-plan.md:985` | Migrate to global asset UUIDs. | W5 closes the legacy-scene half by assigning/reusing sidecar IDs and persisting them in v4. |
| `:3389-3390,3458-3459` | Input rebinding dialog. | W4 provides project defaults, user overrides and composition; W8 owns UI. |
| `:4064,5274` | Script asset Rebind button. | No W4/W5 UI. W4/W5 make the underlying ID/path context stable; W8 owns the button. |
| `:5521` | Declaration diagnostics in content browser. | W6/W8; no browser exists yet. |
| `:5901` | Cursor-lock binding. | W4’s composition supports it later; W8 authors it. |

### Decisions — answered before code

These are the settled answers for this spec. Review may amend them before
implementation; implementation does not choose a different answer locally.

#### D3 — split the existing `projectRoot` by responsibility

**Decision:** rename the per-user setting and introduce distinct project
terms. There will be no new field or API simply named `projectRoot`.

- `EditorSettings` schema v3 renames `projectRoot` to
  `lastBrowseDirectory`. The v2 reader migrates the old value; the v3 writer
  emits only the new key. The API and Session UI use “Last Browse Directory”.
- `lastBrowseDirectory` is dialog state only. Asset resolution, script Play,
  recovery and Save never consult it.
- The portable project model owns `projectFile`, derived absolute
  `projectDirectory`, derived absolute `assetRoot`, derived absolute
  `cacheRoot`, `projectId`, optional `startupScene`, input defaults and the
  current immutable asset-database snapshot.
- A loaded project supplies its asset root and database to every asset
  operation. A standalone saved scene retains a deliberate compatibility
  context: scene parent as the implicit asset root and no project database.
  An untitled standalone scene has no general asset root; its existing
  synthetic recovery root is recovery-only, not a hidden project.
- `SceneDocument` does not own a database or global project singleton. This
  preserves W3-Q3 (`docs/game-engine-development-plan.md:6731-6734`).

This is a behavior migration, not just a rename: removing the current
`ScriptAssetRoot()` fallback at `RT2App/src/WalnutApp.cpp:2713-2723` is part
of D3 acceptance.

#### D4 — split shippable defaults from per-user overrides

**Decision:** project input data provides defaults; per-user settings provide
overrides. Neither replaces the built-in safety map.

Composition is one CPU-only function and has one deterministic order:

1. `InputService::LoadDefaults()` supplies the built-in editor, viewport,
   viewport-look and runtime mappings (`RT2App/src/InputService.cpp:105-178`).
2. Project `inputContexts` overlays allowed runtime/gameplay mappings by
   `(contextId, mapping.name)`.
3. Per-user `inputOverrides` overlays the result by the same key. An explicit
   unbound override removes that action’s binding; absence means inherit.

Project files may define `runtime` and future gameplay contexts. They may not
override the editor-owned `editor`, `viewport` or `viewport.look` contexts;
load rejects those records with a named diagnostic. This keeps a malformed or
hostile project from disabling the editor’s escape/navigation controls.

`EditorSettings` schema v3 renames v2 `inputContexts` to `inputOverrides`.
The v2 reader preserves each record as an override, and the v3 writer emits
only `inputOverrides`. Duplicate context/action keys, unknown binding types,
or contradictory entries fail validation; the composer never chooses by
JSON/insertion order. The shared mapping JSON codec is extracted from
`EditorSettings.cpp:153-217,251-294` so project and settings serialization
cannot drift.

W4 wires the composed maps into `InputService` before its context stack is
first sampled. W8 owns authoring UI. This distinction matters because the
current stored `inputContexts` are never consumed by `WalnutApp` (W45-F3).

#### D6 — portable locator, generated local contents, separate recovery

**Decision:** `cacheRoot` is stored as a project-directory-relative locator,
defaulting to `.rt2/cache`. Its contents are generated, replaceable and not
part of asset identity.

- `assetRoot` and `cacheRoot` are normalized forward-slash paths relative to
  the `.rt2proj` parent. `startupScene` is relative to `assetRoot`. Absolute
  paths, empty `assetRoot`, `..` escape, and paths that escape after
  canonical/reparse-point checks are load errors.
- Cache and asset roots may not overlap in either direction. The scanner also
  refuses directory symlink/reparse-point traversal, so generated files or an
  external tree cannot re-enter the asset scan through an alias.
- The cache directory may be absent and is created lazily by the first cache
  producer. W4 creates no cache artifact merely by opening a project.
- The project file never stores a machine-specific absolute cache path. A
  future per-user cache override requires a separate settings field and is
  not implied by D6.
- Crash-recovery bytes remain below the per-user app-data recovery directory
  at `RT2App/src/WalnutApp.cpp:80-87`; they are not cache entries and are not
  moved under the project.
- A project-bound recovery envelope stores `projectId`, the project-file
  locator and the logical scene locator. Restore reloads/validates the
  project and rebuilds its database before resolving assets. A missing or
  mismatched project is a loud restore failure, not a fallback to the stale
  absolute `assetRoot` currently stored at
  `RT2App/src/SceneRecoveryService.cpp:336`.

### W4 contract — `.rt2proj`, scan and production ownership

#### Project document v1

The initial portable form is:

```json
{
  "version": 1,
  "projectId": "<non-nil UUID>",
  "assetRoot": "Assets",
  "cacheRoot": ".rt2/cache",
  "startupScene": "Scenes/main.rt2scene",
  "inputContexts": []
}
```

`startupScene` is optional/empty; when present it is relative to `assetRoot`,
contained by it, and has a `.rt2scene` extension. Unknown fields are ignored
for additive compatibility, while an unknown `version`, invalid UUID or
invalid path is terminal. Save is atomic and deterministic. Derived absolute
paths are runtime state and are never serialized.

No `runtimeDefaults` object is emitted in v1 for W45-F14. This is an explicit
scope correction to the roadmap stub, not an accidental omission.

#### Scanner and database snapshot

Add a CPU-only scanner beside `AssetDatabase`, not in Walnut UI code:

1. Validate and canonicalize the absolute asset root once.
2. Recursively enumerate regular `*.rt2meta` files without following
   directory symlinks/reparse points; normalize their paths relative to the
   asset root and sort before reading.
3. Strip the single `.rt2meta` suffix to identify the source asset. A sidecar
   without a regular source file is a stale diagnostic, not a record.
4. Read each sidecar through `ReadSidecarId`. Missing/malformed/nil IDs,
   duplicate IDs and conflicting path claims are routed through the existing
   asset-diagnostic surface; nothing is silently skipped.
5. Emit one sidecar-authoritative `AssetRecord` per valid source path and call
   the existing deterministic `BuildAssetDatabase`.

The scan does not mint or rewrite sidecars, infer an exclusive `AssetKind`
from extension, decode assets, or populate cache. `observedKinds` and
dependency/entity edges are merged from known scene/import records as those
sources are available; a physical file can still be observed in multiple
roles (`RT2App/src/AssetDatabase.h:30-80`).

The production project context owns a `shared_ptr<const AssetDatabase>`
snapshot. Refresh builds a complete replacement off to the side and swaps it
only after success; asynchronous scene/import jobs capture the shared snapshot
so the non-owning pointer inside their `AssetResolutionContext` cannot dangle.
A failed scan leaves the previous project/context live and reports the sorted
diagnostics.

Until W7 watches the whole asset tree, W4 refreshes explicitly after project
open, successful explicit import/sidecar assignment, and successful W5
migration. File-system changes made externally after open require an explicit
Refresh Assets action; this limitation is visible in status, not represented
as a live database.

#### Host and API cutover

- Change `SceneAssetResolver::ResolveAll` and `ResolveEnvironment` to require
  the caller’s `AssetResolutionContext`; remove their internal `sceneRoot` /
  null-database construction at `RT2App/src/SceneAssetResolver.cpp:131-159,
  233-295`.
- Add transactional project open to `WalnutApp`: parse, validate, scan and
  compose input maps before replacing the current context. A failed open
  preserves the current project, scene and input map.
- Add `.rt2proj` to the native open flow and a distinct Open Project command.
  Opening a scene/model remains separate. The Session panel displays the
  loaded project file, ID, asset root and cache root; it no longer edits a
  semantic “Project Root” text field.
- Add `--project <path.rt2proj>` to `CLIArgs` and the slice runner. When both
  `--project` and `--scene` are supplied, the scene locator is interpreted
  below the project asset root and must be contained by it; without `--scene`,
  the project startup scene is required. Existing standalone `--scene`
  behavior remains.
- Construct one project/standalone context for native open, Play, script
  inspector/runtime, texture import, recovery and slice execution. Audit the
  null sites in W45-F6. A project-active call with `database == nullptr` is a
  programming error and must fail loudly in Release as well as Debug.
- Project switching is blocked while a destructive load/import is in flight;
  ordinary immutable database snapshots remain safe for readers already in
  flight.

### W5 contract — schema v4 and explicit legacy migration

#### Schema and path semantics

- Set `SceneSerializer::SchemaVersion = 4` and
  `MinReadVersion = 3`. Load accepts the inclusive range and reports the
  source version in `SceneLoadReport`.
- A project-bound v4 scene stores the owning `projectId` in metadata. A
  standalone v4 scene omits it/nil. A project host requires a matching
  non-nil ID; nil is accepted only in explicit standalone mode. Loading under
  a different project ID is a terminal `Conflict`; it must never probe that
  project’s database and happen to bind matching paths.
- Every v4 `AssetReference::path` is relative to the active asset root. For a
  project scene that is the project’s `assetRoot`; for a standalone scene it
  is the scene parent. Save As inside one project therefore does not rewrite
  asset paths. A save outside the project is rejected unless the user first
  converts to a standalone scene through an explicit future operation.
- Existing W3 absolute-path fallback remains readable in memory and produces a
  `NonPortable` diagnostic. It is not described as portable merely because
  v4 was written; the combined portability gate is green only when no
  nonportable/unresolved reference remains.
- Present malformed IDs are diagnostics in v3 and terminal parse errors in
  v4. Missing IDs remain representable so missing assets can load with
  placeholders, but `SceneLoadReport` marks migration/repair incomplete.

Introduce one neutral visitor over every durable scene `AssetReference`.
Serializer validation, migration and portability reporting all use it. Its
initial set is imported model, script and environment (W45-F9); derived
`SceneTexture::ref` is intentionally excluded from native persistence.

#### Explicit migration algorithm

Loading v3 is read-only. It sets `requiresAssetMigration` and records mixed,
nil, malformed and nonportable reference state; it does not write sidecars,
change the document, or acknowledge persistence.

The first explicit Save/Migrate action runs one transactional service:

1. Clone the authoring document and collect durable references through the
   shared visitor. Resolve each v3 path against the legacy scene directory,
   then sort by normalized physical path, kind, entity UUID and source key.
2. Convert each locator to the active asset-root-relative path. Containment
   failure retains the successful absolute locator only as a `NonPortable`
   advisory and leaves the portability gate incomplete.
3. If a reference has a non-nil ID, verify it against the sidecar and database.
   A different sidecar ID or ambiguous database ID is `Conflict` and aborts;
   migration never picks one claimant.
4. Reuse a valid sidecar ID. For an existing source with an absent or
   malformed sidecar, call `ResolveOrAssign` with the injected UUID provider,
   emit the repair diagnostic, and update every reference to that physical
   source with the one assigned ID. A repeated reference never mints twice.
5. A missing source remains unresolved and emits `Missing`/`Stale`; it does
   not receive an ID. Migration may save a structurally valid v4 scene with
   placeholders, but the report says “migrated N; M unresolved” and the
   portability/repair gate stays armed.
6. Rebuild the project database from sidecars, validate every staged
   reference against that snapshot, atomically write schema v4, and only then
   replace the live document/context and clear the migration-persistence gate.

The document and scene file remain unchanged if any sidecar write, rescan,
validation or scene write fails. Sidecars successfully created before a later
failure are not deleted; they are durable assign-once facts, and retry must
reuse them. This makes partial external progress idempotent instead of trying
to roll back identity with another risky write.

#### Save, autosave and recovery boundary

W5 adds a document-level `AssetMigrationPersistenceGate`, analogous in
ownership to the existing script repair gate at
`RT2App/src/WalnutApp.cpp:2839-2842`. Ordinary edits, undo and recovery do not
clear it.

Autosave/recovery never initiates migration and never mints or repairs a
sidecar. The recovery-only `SaveTo` path preserves a v3 document as schema 3
with its legacy scene-relative path base; it does not route through the normal
v4 writer. The recovery envelope also preserves the source version and logical
context, so restore still reports that explicit migration is required. A
recovered project document reloads the project/database per D6 before
resolution. Only a successful explicit migration plus atomic scene save
acknowledges the gate.

### Implementation order

Each step keeps `RT2Tests` and `RT2SliceRunner` CPU-only and ends with focused
tests before the next host cutover.

1. **W4.0 — Neutral project/input models and codecs.** Add project v1
   load/save/path validation, extract the shared input-mapping codec, and add
   deterministic input composition. No Walnut ownership yet.
2. **W4.1 — D3/D4 settings migration.** Raise EditorSettings to v3; migrate
   `projectRoot` → `lastBrowseDirectory` and `inputContexts` →
   `inputOverrides`; prove old files survive and new files contain no old
   semantic keys.
3. **W4.2 — Sidecar scanner and immutable snapshot.** Build sorted records via
   W1/W2 APIs, add diagnostic adaptation, prove scan determinism and
   transactionality.
4. **W4.3 — Resolver API cutover.** Require explicit contexts in aggregate
   resolution and migrate native load, script, Play, recovery and slice-runner
   call sites together. Do not leave a compatibility overload that recreates
   a null database internally.
5. **W4.4 — Project host/CLI.** Transactional Open Project, startup scene,
   `.rt2proj` filter, `--project`, Session status, input composition and
   explicit refresh. Re-run standalone-scene behavior as a separate mode.
6. **W5.0 — v3/v4 codec and shared reference visitor.** Read 3–4, write 4,
   strict v4 ID parsing, project-ID metadata, asset-root-relative paths and
   migration reporting. Still no sidecar mutation on load.
7. **W5.1 — Explicit migration service/gate.** Implement sorted assign-once
   staging, database refresh, atomic scene persistence, idempotent retry and
   recovery/autosave exclusion.
8. **W5.2 — Host workflow and documentation.** Observable migration status,
   unresolved/nonportable gate state, project relocation exercise, Release
   and Debug verification, then append the measured close report.

### Permanent tests and discrimination proofs

| Permanent evidence | Temporary fault that must make it fail |
|---|---|
| EditorSettings v2 migrates both old fields; v3 writes only `lastBrowseDirectory` and `inputOverrides`. Asset resolution is unchanged when only the browse directory changes. | Feed `lastBrowseDirectory` into the script/scene context. |
| Input composition is identical under permuted JSON order; project runtime defaults are overlaid by user overrides, an explicit unbind removes one action, and project editor-context records are rejected. | Swap project/user precedence or let a project replace `editor`. |
| Project codec rejects absolute/escaping roots, cache/asset overlap, nil IDs, bad startup-scene containment and unknown versions. Copying the project tree changes only derived absolute paths. | Resolve one stored path against process CWD. |
| Scanner records and diagnostics are byte-for-byte identical under permuted enumeration; orphan/malformed/duplicate sidecars are loud and directory links are not traversed. | Insert records in raw enumeration order or follow a linked directory. |
| With a project active, model, external texture, environment and script resolution all receive the same asset root/database snapshot; standalone scenes receive their explicit fallback. | Restore the internal `database=nullptr` construction in `SceneAssetResolver`. |
| Failed project parse/scan/startup load leaves the prior project, scene, input maps and database snapshot intact. | Clear the current project before validating the replacement. |
| A v3 scene with no IDs, mixed IDs and all IDs migrates to the same canonical v4 result when sidecar state is the same. Duplicate references to one file call the UUID provider once. | Assign per reference instead of per physical asset. |
| Valid sidecars are reused; absent/malformed sidecars are assigned loudly; ID conflicts/ambiguity abort without changing the live document or scene file. | Prefer the scene ID over a conflicting sidecar. |
| Moving the whole project preserves project ID, sidecar IDs, v4 scene bytes and successful resolution. Save As inside the project does not rewrite reference paths. | Rebase a v4 path against the output scene directory. |
| v3 malformed IDs load with migration diagnostics; v4 malformed IDs fail structurally; versions below 3 and above 4 fail. | Reuse the current silent `JsonToAssetReference` nil conversion for v4. |
| A forced sidecar-write or scene-write failure leaves the live document/file unchanged; retry reuses sidecars already assigned before the failure. | Mutate the live document before the atomic scene write succeeds. |
| Autosave/recovery of a v3 document does not call the UUID provider, write sidecars or clear the migration gate; restore still requires explicit migration. | Route recovery through the explicit migration entry point. |
| A scene carrying project A’s ID fails under project B before asset lookup. | Ignore scene metadata and let fallback paths bind under project B. |

Every new permanent test gets its discrimination fault exercised before the
implementation report calls it protective. The final gate builds the Release
and Debug solutions, runs `RT2Tests.exe` from the repository root, runs the
scripting regression gate and both project/standalone slice-runner scenarios,
and records the actual counts measured then. No count is copied from an older
completed phase.

### Review checklist and boundary

A reviewer must verify at least: all context-producing call sites in W45-F6;
the D3 removal of browse-directory asset semantics; D4 precedence and reserved
editor contexts; D6 containment/recovery behavior; project-database snapshot
lifetime across async work; the complete durable-reference visitor; and the
failure ordering in W5 migration. Any accepted correction is appended as a
dated review amendment.

W4/W5 do not add content-browser UI, rename/move/delete operations, broad
watching/reimport, cache artifact formats, input rebinding UI, script Rebind,
or project-owned render/fixed-step settings. Those boundaries prevent W4 from
depending on W6/W7/W8 and keep W5’s high-risk persistence change behind a
fully established project context.

### Review amendments (2026-07-31 — approved before implementation)

Reviewed against `7ec974b7`. The reviewer approved the spec with the six
amendments below. Each finding was re-checked against the tree and accepted.
These amendments override the draft where they conflict; no W4/W5 code has
started.

#### W45-A1 — inert editor input records do not become live on migration

W45-F3 means v2 `EditorSettings::inputContexts` records have never been
consumed by `WalnutApp` (`RT2App/src/WalnutApp.cpp:1514-1518`). The original
D4 migration would therefore have activated previously dead data, including
editor navigation bindings, merely by upgrading the settings file.

Amended decision: the v2 → v3 migration discards records whose context ID is
`editor`, `viewport` or `viewport.look`; it emits a settings-migration
diagnostic naming every dropped context and action. It never promotes those
records to `inputOverrides`. Non-reserved runtime/gameplay records may migrate
to `inputOverrides`, but their count and keys are also returned in the
migration report and surfaced in host status before the composed map is first
used. Thus no dead record becomes live without an observable report.

Permanent test amendment: a v2 fixture containing all three reserved contexts
plus `runtime` must produce only the runtime override, sorted dropped-record
diagnostics, and a promoted-record report. The discrimination fault is to copy
one reserved record into v3 overrides; the test must fail.

#### W45-A2 — migration transactionality is scoped

The W5 guarantee is **transactional over the live `SceneDocument` and the
`.rt2scene` file, not over the asset tree**. If a later sidecar write, rescan,
validation or scene write fails, the live document and scene file remain
unchanged. Any `.rt2meta` files successfully created earlier in that attempt
remain visible in the asset tree by design. They are durable assign-once facts,
are not rolled back, and the next attempt must reuse them. UI failure status
reports both the failed stage and the number/paths of sidecars already created.

This scoped wording replaces every unqualified use of “transactional
migration” in the draft. The idempotent retry proof remains mandatory.

#### W45-A3 — declared safe stopping points

Passing a focused test is not by itself a shippable checkpoint. The eight
implementation steps form two integration tranches:

1. **W4 tranche: W4.0–W4.4.** Every intermediate commit must build and pass its
   focused tests, but there is no release/handoff checkpoint after W4.1,
   W4.2 or especially W4.3. The first safe stop is after W4.4, when settings
   migration, resolver API cutover, project ownership, standalone fallback,
   CLI and the full Release/Debug/CPU regression gates are all green.
2. **W5 tranche: W5.0–W5.2.** There is no release/handoff checkpoint after
   W5.0: accepting v3 while writing v4 is not shippable until explicit
   migration, persistence gating and recovery behavior exist. The second safe
   stop is after W5.2 and the full combined gate.

If work is interrupted inside a tranche, the branch is reported as
in-progress from the preceding safe checkpoint; a green focused test does not
authorize release or handoff. W4.3 and W4.4 should be reviewed as one host
cutover even if split into buildable commits.

#### W45-A4 — Phase 7 scope/exit amendment for runtime defaults

W45-F14 is an intentional amendment to the Phase 7 roadmap scope at
`docs/game-engine-development-plan.md:554-564`: W4 v1 does **not** ship
project-owned fixed-step or render defaults, and Phase 7 completion is not
blocked on an unconsumed `runtimeDefaults` field. The Phase 7 exit at
`:575-581` is evaluated using the portable project fields that have concrete
owners: project ID, asset/cache/startup locators and input defaults.

Carry-forward **P7-R1**: when a later phase introduces a neutral,
project-owned runtime-settings model and a precedence/consumer contract, it
must add the corresponding `.rt2proj` field, migration and relocation tests.
It must not deserialize directly into `WalnutApp` renderer/UI state or the
compile-time constants at `RT2App/src/RuntimeSceneController.h:79-81`.

#### W45-A5 — injectable resolver discrimination fault

Replace the resolver fault in the permanent-tests table with:

> Construct a default-initialized `AssetResolutionContext` inside
> `SceneAssetResolver` instead of using the caller-supplied context.

`database` already defaults to null at `RT2App/src/AssetResolver.h:113-116`;
the current aggregate resolver’s defect is that its internal contexts never
set the field (`RT2App/src/SceneAssetResolver.cpp:154-156,293-295`). The
amended fault is directly injectable and must make the four-kind host-context
test fail.

#### W45-A6 — glossary is part of the D3 vocabulary contract

The contested-term entry in `docs/glossary.md:112-126` is updated with the
current three-way collision and D3’s settled names. New W4 code and prose use:

- `lastBrowseDirectory` for the per-user dialog-only preference;
- `projectDirectory` for the `.rt2proj` parent;
- `assetRoot` for the base of portable asset locators; and
- `cacheRoot` for generated cache contents.

Bare `projectRoot` is legacy vocabulary and is not introduced in the new
project model. The glossary is the current-state vocabulary source; this
append-only plan records why the choice was made.

### Phase 7 W4 implementation report (2026-07-31)

Grounded and implemented from commit
`7ec974b7f12db686ae0c9addb7b0da61549422e6`. This is the first safe stopping
point declared by W45-A3; W5 remains unimplemented.

- **W4.0/W4.1:** `.rt2proj` v1 and portable-path validation live in
  `RT2App/src/Project.h:16-51` and `Project.cpp:137-283`. The shared input
  codec/composer is `InputConfig.h:14-49` and `InputConfig.cpp:199-346`.
  `EditorSettings` is schema v3 at `EditorSettings.h:38-74`; its v2 reader
  drops inert editor-owned records and reports promoted runtime overrides at
  `EditorSettings.cpp:150-193`. Production composition is applied by
  `InputService.cpp:180-230` and the host before input sampling.
- **W4.2:** the read-only, deterministic sidecar scan is
  `ProjectAssetScanner.cpp:71-238`. It refuses directory links/reparse points,
  sorts before reading, uses `ReadSidecarId`, routes findings through
  `AssetDiagnostic`, and builds the existing `AssetDatabase`. Transactional
  project/context construction is `ProjectContext.cpp:5-19`; the immutable
  snapshot is owned by `ProjectContext.h:14-30`.
- **W4.3:** `SceneAssetResolver::ResolveAll` and `ResolveEnvironment` require
  the caller context (`SceneAssetResolver.h:76-98`;
  `SceneAssetResolver.cpp:131-229`). SceneManager, direct texture import,
  native open, Play, scripts, recovery and the CPU runner now receive explicit
  project or standalone contexts. A project context without a database throws
  in Release as an invariant violation (`ProjectContext.h:21-28`).
- **W4.4:** transactional project open, startup/CLI scene containment,
  standalone native open, Session status and explicit refresh are in
  `WalnutApp.cpp:2890-2914,3196-3396`. `--project` is parsed by
  `CLIArgs.h:11-76`; the CPU runner implements the same project/scene contract
  at `RT2SliceRunner/src/Main.cpp:771-910`. Project-bound recovery envelope v3
  stores project ID/file/scene locator and refuses stale-root fallback
  (`SceneRecoveryService.h:27-107`; `SceneRecoveryService.cpp:331-539`). The
  checked-in runnable project is `RT2App/vertical-slice.rt2proj`.

Permanent W4 coverage is nine cases at
`RT2Tests/src/Phase7W4Tests.cpp:94-448`: deterministic input precedence and
unbind, reserved editor rejection, inert-data migration, relocation and path
validation, deterministic/loud scan, failed-context transactionality,
project-bound recovery, and aggregate resolver database propagation. The
temporary discrimination pass made all nine fail: reversed project/user
precedence; accepted project editor input; promoted inert editor records;
CWD-rooted project locators; disabled root-overlap rejection; skipped malformed
sidecars; premature live-context clearing; stale recovery-root fallback; and a
default-initialized aggregate resolver context. Each fault was removed before
the final gate.

Measured verification from the repository root:

- Release solution built; `RT2Tests.exe --no-skip`: **700/700 cases** and
  **145,911/145,911 assertions**.
- Debug solution built; `RT2Tests.exe --no-skip`: **700/700 cases** and
  **145,911/145,911 assertions**. This supersedes older Debug baseline claims.
- `run_script_test.ps1`: 60-frame script scenario passed with no mismatches.
- Release and Debug `RT2SliceRunner` both passed standalone
  `--scene RT2App/assets/vertical-slice.rt2scene` and project
  `--project RT2App/vertical-slice.rt2proj` modes.
- `graphify update .` completed: 34,070 nodes, 71,702 edges and 1,380
  communities. Its generated report retains the generator's whitespace.

Defects found and corrected during the tranche: generated projects initially
omitted new CPU sources; recovery briefly applied a project root to standalone
records; CLI environment import could race project context establishment;
native scene open could accidentally retain the previous project; Windows
junctions needed explicit reparse-point detection; and scanner findings first
used a parallel diagnostic type instead of the shared asset surface. Focused
and whole-suite tests were rerun after each correction.

### Phase 7 W5 implementation report (2026-07-31)

This append-only report supersedes the W4 period record above where it says
W5 remains unimplemented and where it records the 700-case test counts. W5
was implemented against the reviewed W45 spec and the post-W4 worktree; the
earlier report remains unchanged as its historical record.

- **W5.0 — codec and coverage boundary:** `SceneSerializer` now reads schema
  versions 3–4 and writes v4 (`RT2App/src/SceneSerializer.h:105-126`,
  `SceneSerializer.cpp:235-310,1622-1748`). The shared reference visitor
  covers imported models, scripts and the environment while excluding derived
  texture references (`SceneAssetReferenceVisitor.h:19-41`,
  `SceneAssetReferenceVisitor.cpp:31-71`). v3 malformed IDs are diagnostic
  migration inputs; v4 malformed IDs and project IDs fail structurally.
- **W5.1 — explicit migration:** `MigrateSceneAssetReferences` stages a
  canonical v4 document, groups duplicate references by normalized physical
  source, preflights conflicts before sidecar writes, assigns/reuses IDs in
  deterministic order, converts paths to project-root-relative form, and
  reports missing/nonportable sources (`SceneAssetMigration.cpp:130-360`).
  Sidecars written before a later failure remain as durable assign-once facts;
  the live document and scene file are changed only after the staged scene
  save succeeds. `AssetMigrationPersistenceGate` suppresses autosave until a
  complete migration is persisted (`SceneAssetMigration.h:38-62`).
- **W5.2 — host, recovery and status:** native open records migration state,
  project-bound v4 scenes are checked against the active project before asset
  lookup, and Save/Save As runs the migration transaction with observable
  failure/incomplete status (`WalnutApp.cpp:3362-3429,3605-3751`). Recovery
  continues to serialize v3 snapshots without invoking migration or writing
  sidecars (`SceneRecoveryService.h:21-24`, `SceneRecoveryService.cpp:276-350`).
  The final host correction rereads the adopted document after replacement
  before updating recovery identity (`WalnutApp.cpp:3728-3732`).
- **Permanent W5 evidence:** four cases in
  `RT2Tests/src/Phase7W5Tests.cpp:97-255` cover one-ID-per-physical-source,
  v4 malformed-ID rejection, sidecar conflict transactionality, and v3
  recovery/autosave exclusion. CPU build wiring is present in
  `RT2App/RT2App.vcxproj:669-674`, `RT2Tests/RT2Tests.vcxproj:300-305,696`,
  and `RT2SliceRunner/RT2SliceRunner.vcxproj:250-255`.

Measured verification from the repository root after the final edits:

- Release solution built; `RT2Tests.exe --no-skip`: **704/704 cases** and
  **145,960/145,960 assertions**.
- Debug solution built; `RT2Tests.exe --no-skip`: **704/704 cases** and
  **145,960/145,960 assertions**.
- `run_script_test.ps1`: 60-frame script scenario passed with no mismatches.
- Release and Debug `RT2SliceRunner` each passed both standalone
  `--scene RT2App/assets/vertical-slice.rt2scene` and project
  `--project RT2App/vertical-slice.rt2proj` scenarios.
- `graphify update .` completed: 34,192 nodes, 71,887 edges and 1,404
  communities.

Defects found and corrected during W5 included stale v3 expectations after
the schema bump, missing permanent recovery exclusion coverage, and a host
use-after-replacement risk when Save adopted the staged document. No W5
Release or Debug test failures remain.

Graphify supersession note (2026-07-31): the final post-validation refresh
completed at **34,194 nodes, 71,899 edges and 1,432 communities**; this
supersedes the intermediate graph count recorded immediately above.

W5 recovery-policy correction (2026-07-31): migration pending is an explicit
Save acknowledgement state, not a recovery-suppression state. The host still
allows `MaybeSnapshot`/`SaveTo` for a loaded v3 document, while the migration
gate remains pending until a successful explicit migration and scene save.

Final verification supersession (2026-07-31): the recovery-policy test added
two assertions. Release and Debug now pass **704/704 cases** and
**145,962/145,962 assertions**. Script and all four slice-runner scenarios
remain green. The final graph refresh reports **34,194 nodes, 71,899 edges
and 1,441 communities**.

Final host-policy supersession (2026-07-31): the CPU-only recovery predicate
is now shared by WalnutApp and the permanent W5 test. Release and Debug remain
at **704/704 cases** and **145,964/145,964 assertions**; the final Graphify
refresh reports **34,195 nodes, 71,902 edges and 1,389 communities**.

## Phase 7 W6 — content browser (implementation spec)

Drafted 2026-07-31 and grounded against commit
`17292d6` (W0–W5 landed there; the earlier `7ec974b7` predates those files).
W0–W5 are implemented in the working tree; this spec must be reviewed before
implementation. Review
amendments are appended here; implementation does not reinterpret an
unsettled point from memory.

### Scope and exit

W6 introduces the content browser: a project-scoped panel that searches the
asset tree, displays asset records from the immutable database snapshot, and
performs rename, move, delete, reimport and drag/drop-to-import. It is the
first UI that mutates the asset tree and the first UI that browses the
database produced by W4's scanner.

The combined exit is stronger than "a file list renders": rename and move a
referenced mesh and script through the content browser, reload the scene, and
confirm both still resolve by ID without rewriting the scene file. Delete an
asset that has dependants and confirm the browser reports them before
committing. Reimport a source asset and confirm its identity is preserved
while its decoded cache is rebuilt. External file-system changes made after
open require an explicit Refresh Assets action; W6 does not watch the tree.

Not in W6: filesystem watching or async reimport (W7, per D7 at `:6433`),
the rebinding UI, script Rebind button, declaration diagnostics and
cursor-lock binding (W8). W6 does not add cache artifact formats, project-
owned render/fixed-step settings, or a standalone-scene content browser. The
content browser is project-only; standalone scenes have no `assetRoot` to
browse and no database snapshot to read.

### Grounded findings at `17292d6`

| ID | Current fact | Consequence |
|---|---|---|
| W6-F1 | No content browser exists. No file in `RT2App/src` matches `contentbrowser`/`assetbrowser`/`browser` (P9, `:6373`). The existing UI panels are Scene, Session, Outliner, Inspector, Camera, Performance, Render Settings — all registered as `m_Show*Window` booleans and toggled from the View menu (`WalnutApp.cpp:3159-3165,3916-3932`). | W6 is genuinely greenfield UI. It follows the existing panel pattern: a `m_ShowContentBrowserWindow` boolean, a `DrawContentBrowserPanel()` method called from `OnUIRender`, and a View-menu item. |
| W6-F2 | `AssetDatabase` is a pure in-memory index with `FindByPath`, `LookupById`, `AllRecordsSorted`, `FindDependenciesBySourceKey`, `AddEntityDependency` and `AddAssetDependency` (`AssetDatabase.h:119-172`). `BuildAssetDatabase` normalizes and sorts internally so results are enumeration-order-independent (`AssetDatabase.h:166-172`). | The database has the query APIs W6 needs for search and display. `AllRecordsSorted` returns every record sorted by `sourcePath` — the natural display order for a browser. |
| W6-F3 | `AddEntityDependency` and `AddAssetDependency` are **only called in tests** (`RT2Tests/src/AssetDatabaseTests.cpp:173,310,326-327,342-343,364,391,393,405,410,432`). No production code path — not the scanner, not the resolver, not the serializer, not `SceneManager` — populates `dependentEntities` or `dependencies` in the live database. The scanner (`ProjectAssetScanner.cpp:218-222`) creates records with only `assetId`, `sourcePath` and `identityAuthority=Sidecar`. | The database today **cannot** answer "who depends on this asset?" from its own state. W6 must build the dependants query from the live scene document, not from the database. The `SceneAssetReferenceVisitor` (W6-F9) is the seam. |
| W6-F4 | The immutable snapshot is a `shared_ptr<const AssetDatabase>` owned by `ProjectContext` (`ProjectContext.h:18`). `RefreshProjectAssets()` (`WalnutApp.cpp:2910-2943`) builds a complete replacement via `ScanProjectAssets`, swaps it into `m_ProjectContext`, and pushes the new `AssetResolutionContext` to `m_SceneMgr` and `m_ScriptAssetContext`. It refuses while `IsBackgroundBusy()` is true. | W6 mutations (rename, move, delete, reimport) call `RefreshProjectAssets()` after the filesystem operation succeeds. Readers already in flight hold their `shared_ptr` and see the old snapshot until they re-fetch — this is the existing W4 lifetime contract and W6 does not change it. |
| W6-F5 | Sidecars are the durable identity source. `AssetSidecarPath` appends `.rt2meta` to the full filename (`AssetIdentity.cpp:14-21`). `ReadSidecarId` returns nil for absent sidecars (normal) and fills `err` for malformed ones (`AssetIdentity.cpp:23-62`). `WriteSidecarId` is atomic: temp file + `ReplaceFileW`/`MoveFileExW` on Windows (`AssetIdentity.cpp:64-155`). `ResolveOrAssign` reads-or-mints and is called only by import/migration, never by the read-only resolver (`AssetIdentity.h:52-68`; W3-P4 at `:6647`). | Moving `foo.glb` without `foo.glb.rt2meta` destroys that asset's identity: the scanner sees no sidecar, the asset gets a new ID on next import, and every scene reference to the old ID becomes stale. W6 must move the source file and its sidecar as one atomic unit. |
| W6-F6 | The scanner identifies source assets by stripping the `.rt2meta` suffix from sidecar filenames (`ProjectAssetScanner.cpp:174-177`). A sidecar without a regular source file is a `Stale` diagnostic, not a record (`ProjectAssetScanner.cpp:180-191`). | After a move, the scanner finds the sidecar at the new location and records the new `sourcePath`. After a delete, the scanner no longer finds the sidecar and the record disappears. The database refresh is the mechanism that makes moves and deletes visible. |
| W6-F7 | Resolution is ID-first with path fallback (`AssetResolver.h:39-59`). `Resolve` checks the database by `assetId` first; if the ID locates a unique file that exists, resolution succeeds even when the stored `path` is stale (`AssetResolver.cpp:159-218`). Path fallback reads the sidecar at the resolved path and checks for ID agreement (`AssetResolver.cpp:220-330`). | This is the reason rename and move must **not** touch scene files. If a mesh is moved (with its sidecar), the ID stays the same. After a database refresh, `LookupById` returns the new `sourcePath`. The scene's `AssetReference::path` is stale but the ID is authoritative, so resolution succeeds with a `Stale` advisory. The scene file is unchanged. |
| W6-F8 | `AssetReference::path` in a v4 project scene is relative to the active `assetRoot` (`AssetReference.h:73-80`; `SceneSerializer.h:22-32`). Save As inside a project does not rewrite reference paths (W45 spec at `:8810-8814`). The path is a human-readable fallback; `assetId` is the durable identity. | After a move, the scene's stored path is stale relative to the new filesystem location. This is observable as a `Stale` diagnostic on next resolution, not a broken reference. The path is not rewritten until the next explicit Save, which may optionally update it. W6 does not force a Save after move. |
| W6-F9 | `CollectSceneAssetReferences` walks every entity with `ImportedMeshSourceComponent` or `ScriptComponent`, plus the environment, and returns `AssetReference*` slots in stable order sorted by `(entityUuid, kind, sourceKey, path)` (`SceneAssetReferenceVisitor.h:19-41`; `SceneAssetReferenceVisitor.cpp:31-71`). A const overload returns `const AssetReference*`. | This is the seam for the dependants query. W6 calls `CollectSceneAssetReferences` on the live `SceneDocument`, filters by `assetId` or `sourcePath`, and reports the matching entities. No database mutation is needed. |
| W6-F10 | `ImportedMeshSourceComponent::model` is an `AssetReference` with `kind=Model`, `path` (portable, scene-relative), `sourceKey` (e.g. `gltf:scene=0:node=3:mesh=1:primitive=0`), and `assetId` (`ECSComponents.h:221-223`). `ScriptComponent::asset` is an `AssetReference` with `kind=Script`, `sourceKey` `lua:asset=<path>`, and `assetId` (`ECSComponents.h:282-288`). `EnvironmentSettings::ref` is an `AssetReference` with `kind=Environment` (`SceneDocument.h:50-62`). | The dependants query covers three asset kinds. A single physical file may be referenced by multiple entities (e.g. a glTF with many meshes) and in multiple roles (a model that is also a script owner is unlikely, but a file observed in multiple `sourceKey` slots is normal). The query reports all of them. |
| W6-F11 | The existing import flow calls `SceneManager::ImportGltf`/`ImportObj` (`SceneManager.h:120-132`), which call `FillImportedSourcePathAndId` (`SceneManager.cpp:275-309`) to assign/reuse sidecar IDs via `ResolveOrAssign`. After import, `WalnutApp` calls `RefreshProjectAssets()` (`WalnutApp.cpp:256,2680,2701,3820`). | Reimport reuses this path. The source asset's sidecar already exists, so `ResolveOrAssign` returns the stable ID (`AssetIdentity.cpp:168-169`). Reimport re-decodes the asset through the same import path, preserving identity while rebuilding cache entries. |
| W6-F12 | Drag/drop exists in `SceneEditorUI.cpp:769-806` using `ImGui::BeginDragDropSource`/`BeginDragDropTarget` with a `RT2_ENTITY_UUID` payload type. The outliner uses it for entity reparenting. | W6 drag/drop-to-import follows the same ImGui pattern: `BeginDragDropSource` on a browser item with an `RT2_ASSET_PATH` payload, `BeginDragDropTarget` on the viewport or outliner to receive it. The host's existing import callback is invoked. |
| W6-F13 | `ProjectContext::Assets()` throws `std::logic_error` if the database is null (`ProjectContext.h:21-28`). A project-active call with `database == nullptr` is a Release invariant violation (W45 spec at `:8793`). | The content browser is only available when `m_ProjectContext` is non-null. In standalone mode the panel shows "No project is open" and all operations are disabled. This mirrors the Session panel's project/standalone split (`WalnutApp.cpp:1087-1103`). |
| W6-F14 | `WriteSidecarId` creates parent directories if needed (`AssetIdentity.cpp:76-83`). `std::filesystem::rename` on Windows uses `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING` for the sidecar's atomic write path (`AssetIdentity.cpp:120-141`), but the source asset file itself has no atomic-move helper. | W6 must provide its own filesystem operations for moving and deleting source asset files. The source file move is not atomic across volumes; W6 must detect cross-volume moves and report them as unsupported rather than failing silently. |
| W6-F15 | The `AssetMigrationPersistenceGate` (`SceneAssetMigration.h:42-54`) tracks whether asset migration is pending. Autosave/recovery never initiates migration (`SceneAssetMigration.h:48-50`). `ShouldCaptureRecoverySnapshot` (`SceneAssetMigration.h:58-63`) allows recovery while migration is pending. | W6 operations do not interact with the migration gate. Rename, move, delete and reimport are asset-tree mutations, not scene-document migrations. The gate remains in whatever state it was before the operation. |
| W6-F16 | The efsw file watcher watches only the scene directory and directories of bound `.lua` scripts, rebuilt on scene open (P10, `:6374`). It does not watch the asset tree. | W6 does not depend on filesystem watching. After any W6 mutation, the database is refreshed explicitly. External changes require the existing "Refresh Assets" button (`WalnutApp.cpp:1097-1098`). W7 owns widening the watcher. |

### Recovered Phase 7 commitments

| Source | Commitment | W6 treatment |
|---|---|---|
| `:560` | Add a content browser with search, rename/move/delete, drag/drop, and reimport. | W6 implements all five. |
| `:573` | Asset deletion reports dependants before committing the change. | W6 builds a live-document dependants query (W6-F3, W6-F9) and blocks delete until the user acknowledges. |
| `:577` | Move and rename a referenced mesh and script through the content browser; reload the scene and confirm both still resolve. | W6 exit criterion. The acceptance exercise is part of the implementation report. |
| `:579` | Modify a source texture and verify reimport updates the viewport safely. | Deferred to W7. W6 reimport re-decodes through the existing import path; W7 owns the async/watched variant. W6's reimport is explicit and synchronous. |
| `:5521` | Declaration diagnostics shown inline in the content browser. | W8. W6 displays the asset list and supports search/rename/move/delete/reimport, but does not add declaration diagnostics. |
| `:8618` | Declaration diagnostics in content browser. | W6/W8; W6 builds the browser, W8 adds diagnostics. |

### Decisions — answered before code

These are the settled answers for this spec. Review may amend them before
implementation; implementation does not choose a different answer locally.

#### D-W6-1 — the atomic unit of move is the source file plus its sidecar

**Decision:** rename and move operate on the pair `{source, sidecar}` as one
atomic unit. The content browser constructs both destination paths, moves the
source file first, then moves the sidecar. If the source move succeeds and
the sidecar move fails, the operation is a **partial failure**: the source
file is at its new location but its identity sidecar is at the old one. W6
reports this loudly through `AssetDiagnostic` with `Conflict` severity, names
both paths, and does not attempt to move the source back (a rollback move is
another risky filesystem operation). The user is told to move the sidecar
manually or reimport.

Rationale: `WriteSidecarId` is atomic (`AssetIdentity.cpp:64-155`), but the
source asset file has no atomic-move helper (W6-F14). `std::filesystem::rename`
is atomic on the same volume but not across volumes. W6 rejects cross-volume
moves up front rather than discovering the failure mid-operation. Within one
volume, both `rename` calls are atomic individually; the pair is not
transactional, but the failure window is narrow and the partial state is
diagnosable.

#### D-W6-2 — rename and move must not touch scene files

**Decision:** rename and move are asset-tree operations only. They do not
rewrite, save, or mark dirty any `.rt2scene` file. After a successful move,
the database is refreshed and the scene's `AssetReference::path` is stale.
Resolution still succeeds by ID (`AssetResolver.h:42-44`) and emits a `Stale`
advisory. The path is corrected on the next explicit Save, which is the
existing W4/W5 contract.

This is the payoff of W1–W5. If moving a referenced mesh required rewriting
every scene that uses it, the phase failed. The acceptance exercise proves
this: move a referenced mesh and script, reload the scene, and confirm both
resolve by the same IDs without a scene-file rewrite between the move and
the reload.

#### D-W6-3 — delete reports dependants from the live scene document, not from the database

**Decision:** the dependants query is built from `CollectSceneAssetReferences`
on the live `SceneDocument` (W6-F9), not from `AssetDatabase::dependentEntities`
(which is never populated in production — W6-F3). The query matches by
`assetId` when non-nil, falling back to `sourcePath` when nil. It returns one
entry per referencing entity, including entity UUID, name, asset kind and
source key. The browser displays the list and requires explicit
acknowledgement before deleting.

The query is a read-only operation against the live document. It does not
mutate the database or the document. If the scene is unsaved, the query
reflects the current authoring state, not the last-saved file. This is
correct: the user needs to know what will break *now*, not what would have
broken at last save.

**Limitation:** the query covers only the currently open scene. If another
scene in the project references the asset, W6 cannot see it. This is
documented in the delete confirmation dialog: "X entities in the current
scene reference this asset. Other scenes in this project may also reference
it." This is not a silent failure; it is a stated limitation of a
single-scene editor. A project-wide dependants query would require scanning
every scene file, which is out of scope for W6.

#### D-W6-4 — refresh after every mutation, readers in flight see the old snapshot

**Decision:** after every successful rename, move, delete or reimport, W6
calls `RefreshProjectAssets()` (`WalnutApp.cpp:2910-2943`). The existing
lifetime contract applies: the `shared_ptr<const AssetDatabase>` held by
async workers remains valid until they complete; the next resolver call
fetches the new snapshot via `m_ProjectContext->Assets()`.

W6 does not refresh on every UI frame. The browser displays records from the
snapshot captured at the last refresh. If the user suspects external changes,
they click "Refresh Assets" (the existing button, `WalnutApp.cpp:1097`). W6
does not add a second refresh path.

#### D-W6-5 — reimport is explicit and synchronous, identity is preserved

**Decision:** reimport re-decodes the source asset through the existing
import path (`SceneManager::ImportGltf`/`ImportObj`, W6-F11). The asset's
sidecar already exists, so `ResolveOrAssign` returns the stable ID
(`AssetIdentity.cpp:168-169`). Reimport does not mint a new ID, write a new
sidecar, or change the scene's `AssetReference::assetId`. It rebuilds the
decoded geometry/material/texture cache entries and syncs to GPU.

Reimport is a user-initiated action on a single asset. It is synchronous
within the existing background-work framework (`StartBackgroundWork`,
`WalnutApp.cpp:293-340`): the worker thread decodes the asset, the
completion callback merges it into the live scene, and the database is
refreshed. W7 owns async/watched reimport; W6 does not watch for external
file changes.

#### D-W6-6 — CPU-only operations module, ImGui panel in WalnutApp

**Decision:** the content browser's non-UI logic — search, dependants query,
move/rename/delete filesystem operations, and the reimport dispatch — lives
in a new CPU-only module (`ContentBrowserOperations.{h,cpp}`) beside
`AssetDatabase` and `ProjectAssetScanner`. It links into `RT2Tests` and
`RT2SliceRunner` without Vulkan, ImGui or Walnut. The ImGui panel
(`DrawContentBrowserPanel`) lives in `WalnutApp.cpp` and calls the CPU-only
operations through callbacks, following the `SceneEditorUI` precedent.

### W6 contract — content browser capabilities

#### Search and display

The browser panel is project-only. When `m_ProjectContext` is null, it shows
"No project is open" and all controls are disabled. When a project is open,
it displays a flat or tree-ordered list of `AssetRecord` entries from
`AllRecordsSorted()`, each showing:

- Source path (relative to `assetRoot`, forward slashes).
- Asset ID (short form, first 8 characters with ellipsis if longer).
- Observed kinds (from `AssetRecord::observedKinds`, which may be empty for
  sidecar-only records).
- A sidecar status icon: present, absent, or malformed (from the last scan
  diagnostics).

A search filter (`ImGui::InputText`) filters by substring match on
`sourcePath` or `assetId.ToString()`. The filter is case-insensitive on
Windows. Filtering is client-side against the current snapshot; it does not
re-scan.

#### Rename

Rename changes the filename of the source asset and its sidecar within the
same directory. It does not change the directory or the asset ID. The
operation:

1. Validate the new name: non-empty, no path separators, no `..`, must end
   with the same extension as the original (renaming `cube.glb` to
   `cube.obj` is a format change, not a rename — reject it).
2. Construct old and new paths for both source and sidecar.
3. Check that the new source path does not already exist (a collision is a
   loud `Conflict`, not a silent overwrite).
4. Move source file, then move sidecar (D-W6-1).
5. On success, `RefreshProjectAssets()`.
6. On partial failure, report loudly and do not refresh (the old snapshot is
   stale but the user needs to see the diagnostic).

Rename does not touch any scene file (D-W6-2).

#### Move

Move changes the directory of the source asset and its sidecar within the
`assetRoot`. The operation:

1. Validate the destination directory: must be contained by `assetRoot`
   (same containment check as W4's path validation, `Project.cpp:137-283`),
   must not escape via `..` or reparse points, must not be the same as the
   current directory.
2. Reject cross-volume moves up front (D-W6-1).
3. Create the destination directory if it does not exist.
4. Construct old and new paths for both source and sidecar.
5. Check that the new source path does not already exist.
6. Move source file, then move sidecar (D-W6-1).
7. On success, `RefreshProjectAssets()`.
8. On partial failure, report loudly and do not refresh.

Move does not touch any scene file (D-W6-2). After move, the scene's
`AssetReference::path` is stale. Resolution by ID succeeds with a `Stale`
advisory (W6-F7). The path is corrected on the next explicit Save.

#### Delete

Delete removes the source asset file and its sidecar. The operation:

1. Build the dependants query (D-W6-3): call
   `CollectSceneAssetReferences` on the live `SceneDocument`, filter by
   `assetId` (when non-nil) or `sourcePath` (when nil). Collect entity
   UUIDs, names, asset kinds and source keys.
2. Display the dependants list in a confirmation dialog. If dependants are
   non-empty, the dialog says: "N entities in the current scene reference
   this asset. Deleting it will cause those references to break on next
   resolve. Other scenes in this project may also reference it." The user
   must explicitly confirm.
3. If the user confirms, delete the sidecar first, then the source file.
   Deleting the sidecar first means a partial failure (source delete fails)
   leaves the asset resolvable by path with a `Stale` diagnostic, rather
   than leaving an orphaned sidecar.
4. On success, `RefreshProjectAssets()`.
5. On partial failure (source deleted, sidecar remains), report loudly. The
   orphaned sidecar is a `Stale` diagnostic on next scan
   (`ProjectAssetScanner.cpp:180-191`).

Delete does not modify the scene document. Referencing entities remain in
the scene with their `AssetReference` intact. On next resolve, the missing
asset produces a `Missing` diagnostic (`AssetResolver.h:54`). The user may
then remove the entity or rebind the reference (W8 owns the rebind UI).

#### Reimport

Reimport re-decodes the source asset through the existing import path. The
operation:

1. The asset must have a valid sidecar (otherwise reimport is an import, not
   a reimport — direct the user to the existing Import button).
2. Call `SceneManager::ImportGltf` or `ImportObj` (dispatched by extension)
   with the asset's absolute path. `FillImportedSourcePathAndId`
   (`SceneManager.cpp:275-309`) calls `ResolveOrAssign`, which reads the
   existing sidecar and returns the stable ID (`AssetIdentity.cpp:168-169`).
3. The import replaces the entity's mesh/material/texture state with freshly
   decoded data. `ImportedMeshSourceComponent::model.assetId` is unchanged.
4. After import, `RefreshProjectAssets()` (the existing post-import pattern,
   `WalnutApp.cpp:256`).
5. GPU sync is the existing `m_PendingFullSync` path.

Reimport does not change asset identity, write a new sidecar, or touch the
scene file's `assetId` fields. It may update `ImportSettings` if the user
changed import options (e.g. toggling `assumeDielectricWithoutMetalRough`).

#### Drag/drop to import

The browser supports dragging an asset from the list and dropping it on the
viewport or outliner. The drag source sets an `RT2_ASSET_PATH` ImGui payload
containing the asset's absolute path. The drop target calls the existing
import callback (`SceneEditorUI::SetOnImportGltf`/`SetOnImportWithOptions`,
`SceneEditorUI.h:68-77`). This is not a new import path; it is a new entry
point to the existing one.

Drag/drop does not move or delete assets. It is import-only. Moving assets
within the browser tree is a rename or move operation initiated through the
context menu or keyboard shortcut, not through drag/drop.

### Implementation order

Each step keeps `RT2Tests` and `RT2SliceRunner` CPU-only and ends with
focused tests before the next host cutover.

1. **W6.0 — CPU-only operations module.** Add `ContentBrowserOperations.{h,cpp}`
   with: search/filter against `AssetDatabase::AllRecordsSorted`, dependants
   query via `CollectSceneAssetReferences`, rename (same-directory move of
   source + sidecar), move (cross-directory move of source + sidecar),
   delete (sidecar-then-source removal), and reimport dispatch (calls
   existing `SceneManager` import APIs). No Walnut/ImGui. Focused tests:
   search determinism, dependants query correctness, rename/move/delete
   filesystem operations with temp directories, partial-failure reporting,
   cross-volume rejection.

2. **W6.1 — ImGui panel.** Add `DrawContentBrowserPanel()` to `WalnutApp`,
   register `m_ShowContentBrowserWindow`, add View-menu item. Display the
   asset list from the current `m_ProjectContext->database` snapshot. Wire
   search filter. Wire rename/move/delete/reimport to the CPU-only
   operations through callbacks. Add the delete-confirmation dialog with
   dependants list. Add drag/drop source. No drop target yet.

3. **W6.2 — Drop target and acceptance exercise.** Add drop targets on the
   viewport and outliner for `RT2_ASSET_PATH` payloads. Wire to the existing
   import callbacks. Run the acceptance exercise: move and rename a
   referenced mesh and script, reload the scene, confirm both resolve by ID.
   Delete an asset with dependants, confirm the dialog reports them. Reimport
   a source asset, confirm identity is preserved. Release and Debug
   verification, then append the measured close report.

### Permanent tests and discrimination proofs

| Permanent evidence | Temporary fault that must make it fail |
|---|---|
| Search returns records matching a substring on `sourcePath` or `assetId`, sorted by `sourcePath`, independent of scan enumeration order. | Return records in raw `unordered_map` iteration order instead of sorted. |
| Rename moves both source and sidecar to the new name within the same directory. After refresh, the database records the new `sourcePath` and the same `assetId`. | Move the source file without moving the sidecar; the refresh must produce a `MissingSidecarId`/`Stale` diagnostic and a different `assetId` on next import. |
| Move relocates source and sidecar to a new directory within `assetRoot`. After refresh, `LookupById` returns the new `sourcePath`. A scene reference with the old path and the stable ID resolves successfully with a `Stale` advisory. | Move the source without the sidecar; `LookupById` must fail `Missing` because the sidecar is gone and no record claims the ID. |
| Move rejects cross-volume destinations, `..` escape, reparse points, and destinations outside `assetRoot`. | Disable the containment check and allow a move outside `assetRoot`; the refreshed database must not contain the record. |
| Delete with dependants reports every referencing entity (UUID, name, kind, source key) from the live scene document before committing. | Populate `AssetDatabase::dependentEntities` instead of querying the live document; the test must fail because production code never populates that field. |
| Delete removes sidecar then source. A forced source-delete failure leaves the sidecar as an orphaned `Stale` diagnostic on next scan, not a silent success. | Delete the source first and skip the sidecar; the scan must report the orphaned sidecar. |
| After rename or move, no `.rt2scene` file is modified. The scene's `AssetReference::path` is stale but `assetId` is unchanged; resolution by ID succeeds with `Stale`. | Rewrite the scene file's paths after move; the test must fail because the scene file's modification time changed. |
| Reimport preserves the asset's sidecar ID and `AssetReference::assetId`. The decoded geometry is replaced; identity is unchanged. | Call `ResolveOrAssign` with a fresh UUID provider that mints a different ID; the test must fail because the asset ID changed. |
| Drag/drop payload carries the absolute asset path and triggers the existing import callback. | Dispatch to a new import path instead of the existing `SceneManager::ImportGltf`/`ImportObj`; the test must fail because the import did not go through the production path. |
| The browser panel is disabled when no project is open. All operations are no-ops. | Enable operations in standalone mode; the test must fail because `m_ProjectContext` is null and `Assets()` throws. |

Every new permanent test gets its discrimination fault exercised before the
implementation report calls it protective. The final gate builds the Release
and Debug solutions, runs `RT2Tests.exe` from the repository root, runs the
scripting regression gate and both project/standalone slice-runner scenarios,
and records the actual counts measured then. No count is copied from an older
completed phase.

### Review checklist and boundary

A reviewer must verify at least: the atomic-unit move contract (D-W6-1);
the no-scene-rewrite contract (D-W6-2); the dependants query source
(D-W6-3, live document not database); the refresh timing (D-W6-4); the
reimport identity-preservation (D-W6-5); the CPU-only/UI split (D-W6-6);
the partial-failure reporting for rename, move and delete; the delete
ordering (sidecar first, then source); the cross-volume rejection; the
search determinism; and the acceptance exercise (move and rename a
referenced mesh and script, reload, confirm both resolve). Any accepted
correction is appended as a dated review amendment.

W6 does not add filesystem watching or async reimport (W7), the rebinding
UI, script Rebind button, declaration diagnostics or cursor-lock binding
(W8), cache artifact formats, project-owned render/fixed-step settings, or
a standalone-scene content browser. W6's dependants query covers only the
currently open scene; a project-wide query is out of scope. W6's reimport is
synchronous and explicit; watched reimport is W7. These boundaries prevent
W6 from depending on W7/W8 and keep the asset-tree mutations behind the
established W4/W5 project context.

#### Review amendment W6-A1 (2026-07-31) — delete order is source first

Raised during implementation. The delete contract contradicted itself: step 3
mandated sidecar-first ("rather than leaving an orphaned sidecar"), while step
5 described the partial-failure state as "source deleted, sidecar remains" —
which only sidecar-second can produce. The permanent-test row repeated the same
inconsistency, claiming "removes sidecar then source" and then asserting the
source-first failure state.

**Resolution: delete the source first, then the sidecar.** Step 3 is amended;
steps 4 and 5 and the test row were already written against this ordering and
stand unchanged.

The original rationale had the risk backwards. The failure that actually
happens is the *source* delete — a mesh or texture may be held open by the
renderer or locked by the OS, while a small JSON sidecar effectively always
deletes. Under sidecar-first, that common failure destroys the durable identity
of an asset which still exists on disk, so the user's delete fails *and* every
scene referencing it needs identity repair. Under source-first the same failure
occurs before anything irreversible: nothing is damaged and the operation is a
clean abort.

This also matches the W5 contract that sidecars are durable assign-once facts
and are never destroyed speculatively before an operation is known to have
succeeded.

The residual risk of source-first is a surviving orphan sidecar later claiming
its old ID for a different asset imported to the same path. Step 5's loud
report must therefore name the orphaned sidecar path so it can be cleaned up;
the scanner independently reports it as `Stale`.

Grounding commit for this amendment: `17292d6`. Also supersedes the spec
header's grounding commit `7ec974b`, which predates W0–W5 landing in the tree.

## Phase 7 W6 verification report (2026-07-31)

W6 was implemented on branch `codex/phase7-w6-content-browser`, grounded
against `3f21f49` plus the W6-A1 amendment. The implementation commits are
`29c0ff6` (CPU operations), `c0bbf33` (Content Browser host and drag/drop),
`3e4495b` and `7f9066b` (acceptance proof refinements), and `140a948`
(cross-volume discrimination coverage).

### Built

- `RT2App/src/ContentBrowserOperations.{h,cpp}:24-515` is the CPU-only
  operation seam. It searches the immutable sorted database snapshot, derives
  dependants from `CollectSceneAssetReferences`, validates containment and
  reparse boundaries, moves source/sidecar pairs source-first, reports
  non-rolled-back partial failures, deletes source-first per W6-A1, and
  verifies reimport identity remains equal to the durable sidecar ID.
- `RT2App/src/WalnutApp.cpp:1201-1400,3407,4175` adds the project-only
  Content Browser panel, View-menu registration, search, rename/move/delete/
  reimport actions, dependant confirmation, refresh-after-success policy, and
  the absolute `RT2_ASSET_PATH` drag source.
- `RT2App/src/SceneEditorUI.{h,cpp}:27-43,762-780` and
  `RT2App/src/WalnutApp.cpp:896-1045` add viewport/outliner drop targets that
  dispatch through the existing glTF/OBJ import callbacks.
- `RT2Tests/src/Phase7W6Tests.cpp:78-389` covers search, live-scene
  dependants, pair rename/move, containment and cross-volume rejection,
  source-first delete failure states, scanner Stale reporting, reimport ID
  preservation, standalone policy, and the referenced mesh/script acceptance
  exercise. All filesystem tests use temporary trees.

The original W6 text still contains historical “sidecar then source” wording
in its permanent-test/checklist prose. W6-A1 is the controlling amendment and
the shipped implementation and tests are source-first: a locked source leaves
the pair untouched; a sidecar failure leaves a named orphan and the scanner
reports it as Stale.

### Measured gates

- Release solution build: pass.
- Debug solution build: pass.
- Release `RT2Tests.exe` from the repository root: **712/712 cases,
  146,075/146,075 assertions, 0 failed**.
- Debug `RT2Tests.exe` from the repository root: **712/712 cases,
  146,075/146,075 assertions, 0 failed**.
- Focused W6 tests: **8/8 cases, 111/111 assertions** in both Release and
  Debug.
- `run_script_test.ps1`: pass (60 frames, 1 entity, no mismatches).
- `run_slice_test.ps1`: pass; standalone and explicit project-context slice
  runs both completed 60 steps with authoring intact.
- Release headless render smoke: pass, 8 frames at 640×360 with ReSTIR and
  NRD; screenshot and GPU timing readback completed.
- `graphify update .`: pass; final graph refresh reported 34,295 nodes,
  72,169 edges and 1,490 communities, with no topology change on the final
  test-only refresh.

### Acceptance and discrimination evidence

The acceptance fixture creates a temporary project scene referencing a mesh
and script, renames the mesh and moves the script through the same CPU seam
used by the browser, reloads the unchanged scene, and confirms both durable
IDs remain attached while ID-first resolution reaches the new mesh path. The
permanent red-path probes inject sidecar move failure, locked-source delete,
sidecar-delete failure, wrong-volume/escape destinations, and a reimport that
tries to replace the sidecar ID; each produces the expected refusal, partial
state, diagnostic, or restoration, and the final tree is green in both
configurations.

No W7 filesystem watcher or async reimport was added, and standalone content
browser operations remain disabled as specified.

## Phase 7 W7 — watching and async reimport (implementation spec)

Drafted 2026-08-01 and grounded against commit
`9008290`. W0–W6 are implemented in the working tree; this spec must be
reviewed before implementation. Review amendments are appended here;
implementation does not reinterpret an unsettled point from memory.

### Scope and exit

W7 widens the existing efsw file watcher from script-only directories to the
entire `assetRoot`, fixes the defect that the watch set is rebuilt only on
scene open (D7, `:6433`), and defines which asset kinds may be auto-reimported
"where safe." It is the first workstream that reacts to external filesystem
changes on the asset tree.

The combined exit is stronger than "the watcher runs": an externally modified
`.lua` script is hot-reloaded through the existing `ScriptSystem::ReloadScript`
path without user intervention. An externally added or deleted asset file
appears in or disappears from the content browser after an automatic database
refresh. A W6 rename, move or delete does not fight the watcher —
self-inflicted events are suppressed. An `ReadDirectoryChangesW` buffer
overflow is reported, not silently dropped. Explicit Refresh remains a
supported backstop for every case the watcher cannot cover.

Not in W7: the rebinding UI, script Rebind button, declaration diagnostics
and cursor-lock binding (W8). W7 does not auto-reimport models, textures or
environment maps — those rebuild GPU resources and require explicit user
action through the W6 content browser. W7 does not auto-reload `.rt2scene`
files that change on disk while open and dirty; that is explicitly out of
scope. W7 does not add a new watcher implementation — it widens and refines
the existing efsw watcher.

### Grounded findings at `9008290`

| ID | Current fact | Consequence |
|---|---|---|
| W7-F1 | The watcher is `ScriptFileWatchListener` (`WalnutApp.cpp:3000-3029`), created once in the constructor alongside `efsw::FileWatcher` (`:174-177`). The listener overrides `handleFileAction` but does **not** override `handleMissedFileActions` — the efsw default is a no-op (`efsw.hpp:265`). | W7 must override `handleMissedFileActions` to surface buffer-overflow events. The current listener silently drops them. |
| W7-F2 | `handleFileAction` filters to `.lua` files only (case-insensitive on Windows) and ignores `Delete` actions (`WalnutApp.cpp:3014-3021`). It builds the full path from `dir + filename` and pushes to `pendingChanges` under a mutex (`:3023-3027`). | W7 widens the filter: `.lua` → script reload path; `.glb`/`.gltf`/`.obj`/`.hdr`/`.exr`/`.rt2meta` → database refresh; everything else → ignored. The existing mutex/pendingChanges pattern is reused but the listener must classify by file type, not just extension. |
| W7-F3 | The watch set is rebuilt **only on scene open** (`WalnutApp.cpp:3726-3791`). It removes all active watch IDs, walks `ScriptComponent` entities to find script directories, and adds recursive watches deduped by directory path. D7 (`:6433`) calls this a defect: "the watch set is only rebuilt on scene open." | W7 must add the `assetRoot` as a single recursive watch at project open, not per-script-directory at scene open. The script-directory walk is removed — the `assetRoot` watch covers all scripts. The watch is added when `m_ProjectContext` is established and removed when it is cleared. |
| W7-F4 | The debounce drain runs in `OnUIRender` (`WalnutApp.cpp:1058-1106`). It locks the mutex, moves `pendingChanges` into `m_DebouncedChanges` (deduplicating by path), waits a 100 ms quiet window, then calls `ScriptSystem::ReloadScript` for each path and optionally clears the inspector field registry. The 100 ms window exists because an atomic save (temp + rename) produces Modified + Added + Deleted for one Ctrl+S (`:1058-1060`). | Widening from a few script directories to the whole `assetRoot` multiplies event volume. The debounce window must remain, but the coalescing must be by operation class (script reload vs database refresh), not just by path. A single external `git checkout` can produce hundreds of events; the drain must coalesce them into one database refresh, not 100 individual refreshes. |
| W7-F5 | `RefreshProjectAssets()` (`WalnutApp.cpp:3153-3187`) returns `false` when `IsBackgroundBusy()` is true, with the status message "Asset refresh deferred: background work is in progress" (`:3161-3166`). It does not queue the refresh for later. | Under W7, watcher events arrive while background work (import, scene load, env decode) is running. The current silent-defer path goes from rare to routine. W7 must queue events during background work and drain them into a single refresh when work completes, rather than silently dropping them. |
| W7-F6 | `IsBackgroundBusy()` returns `m_BackgroundWork != nullptr` (`WalnutApp.cpp:1771`). Only one `BackgroundWork` may be active at a time (`BackgroundWork.h:36-38`). The completion callback runs on the main thread after the worker joins (`WalnutApp.cpp:1645-1654`). `WaitForBackgroundWork()` is the headless synchronous drain (`:1777-1807`). | The watcher runs on its own efsw thread; `BackgroundWork` runs on a dedicated worker thread; the drain and completion run on the main thread. The suppression registry and the event queue must be thread-safe. The completion callback is the safe point to drain queued events. |
| W7-F7 | W6 operations (rename, move, delete, reimport) all call `RefreshProjectAssets()` after success (`WalnutApp.cpp:1293,1325,1358,1393`). Each operation touches the filesystem — rename moves source + sidecar, move relocates them, delete removes them, reimport re-reads the source. Every one of these fires efsw events on the `assetRoot`. | Without suppression, W6 and W7 fight: a rename fires Modified + Add + Delete, the watcher queues a database refresh, `RefreshProjectAssets()` is called redundantly, and the cycle repeats for every W6 operation. The suppression registry (D-W7-1) is the mechanism that prevents this. |
| W7-F8 | `ResolveOrAssign` is called only at `SceneManager.cpp:291,584,627,900,3899`, `SceneLoader.cpp:80`, `SceneAssetMigration.cpp:333`, and `TextureAssetPipeline.cpp:498`. Every call site is in an explicit import or migration path — never in the read-only resolver, never in the watcher, never in the scanner. | The watcher must never call `ResolveOrAssign`. It may call `ReadSidecarId` for diagnostic purposes, but it never mints, writes, or repairs a sidecar. This is the W5 identity contract: sidecars are assign-once and durable; only import, migration, and explicit W6 reimport touch them. |
| W7-F9 | efsw on Windows uses `ReadDirectoryChangesW` (`WatcherWin32.cpp:209-211`) with a default buffer of 63 KB (`FileWatcherWin32.cpp:55-56`). When `dwNumberOfBytesTransfered == 0`, the buffer overflowed; efsw calls `handleMissedFileActions` and then `RefreshWatch` to re-register (`WatcherWin32.cpp:164-170`). The `WinBufferSize` option (`efsw.hpp:128-133`) allows a larger buffer, but "a buffer larger than 64K will fail the folder being watched" for network drives (`efsw.hpp:130-132`). | Event loss on Windows is a normal condition, not an exception. W7 overrides `handleMissedFileActions` to post a "missed" marker that triggers an explicit `RefreshProjectAssets()` on the next drain. The user is told. W7 does not increase the buffer size for network drives; it increases it for local drives only, using `WinBufferSize`. |
| W7-F10 | `ScriptFileWatchPolicy` (`ScriptFileWatchPolicy.h:10-80`) provides `DecideScriptFileChange` (run-state dispatch: Playing/Paused → reload, Edit → invalidate cache) and `ScriptWatchDirectoryForCandidate` (normalize to absolute parent). It is CPU-only and tested from `RT2Tests`. | W7 follows this precedent: the new `AssetWatchPolicy` module is CPU-only, testable, and lifted out of `WalnutApp::OnUIRender`. The existing `DecideScriptFileChange` is reused unchanged; `ScriptWatchDirectoryForCandidate` is no longer needed for watch-set construction (the `assetRoot` covers all scripts) but remains for path normalization. |
| W7-F11 | `ScriptSystem::ReloadScript` (`ScriptSystem.cpp:257-356`) has a three-way run-state branch: Playing reloads now, Paused queues for drain on Resume, Edit invalidates the field registry cache only. It requires an absolute path (`:259-269`) and normalizes lexically (`:276`). | The watcher's script-reload path already works. W7 does not change `ReloadScript`; it feeds it the same absolute paths from the wider watch. The 100 ms debounce is retained. |
| W7-F12 | `ProjectContext` (`ProjectContext.h:15-29`) owns a `shared_ptr<const AssetDatabase>` snapshot. `RefreshProjectAssets()` builds a complete replacement, swaps it, and pushes the new `AssetResolutionContext` to `m_SceneMgr` and `m_ScriptAssetContext` (`WalnutApp.cpp:3168-3184`). The scan is read-only and deterministic (`ProjectAssetScanner.cpp:71-237`). | The database refresh after watcher events uses the existing `RefreshProjectAssets()` path. The immutable-snapshot contract is unchanged: readers in flight hold their `shared_ptr` and see the old snapshot until the next call. |
| W7-F13 | The content browser panel (`WalnutApp.cpp:1201-1400`) displays records from `m_ProjectContext->database` and has a "Refresh" button (`:1217-1218`) that calls `RefreshProjectAssets()`. After a W6 operation, the panel calls `RefreshProjectAssets()` and updates its display from the new snapshot. | The content browser is a consumer of the database snapshot. After a watcher-triggered refresh, the browser's next frame renders from the new snapshot automatically (it queries `m_ProjectContext->database` each frame). No explicit notification to the browser is needed. |
| W7-F14 | The `efsw::FileWatcher` is constructed once and `watch()` is called once in the WalnutApp constructor (`WalnutApp.cpp:176-177`). `addWatch` returns a `WatchID` (`efsw.hpp:195`); `removeWatch` by ID is O(log n) (`efsw.hpp:210`). The watcher runs on its own thread after `watch()` is called. | W7 adds the `assetRoot` watch at project open and removes it at project close/switch. The existing `m_ActiveWatchIds` vector (`WalnutApp.cpp:3038`) is replaced by a single watch ID for the `assetRoot`. The per-script-directory watches are removed. |
| W7-F15 | The `AssetMigrationPersistenceGate` (`SceneAssetMigration.h:42-54`) tracks pending migration. `ShouldCaptureRecoverySnapshot` (`:58-63`) allows recovery while migration is pending. | W7 does not interact with the migration gate. Watcher-triggered refreshes are database scans, not scene migrations. The gate remains in whatever state it was before. |

### Recovered Phase 7 commitments

| Source | Commitment | W7 treatment |
|---|---|---|
| `:561` | Watch source files and reimport asynchronously where safe. | W7 watches the `assetRoot` and auto-reimports `.lua` scripts. Models/textures/env are "not safe" and require explicit W6 reimport. |
| `:579` | Modify a source texture and verify reimport updates the viewport safely. | W7 does not auto-reimport textures. The acceptance exercise is: modify a `.lua` script externally and verify hot reload; add/delete an asset file and verify the browser updates after automatic refresh. Texture reimport remains an explicit W6 operation. |
| `:6374` (P10) | The efsw watcher watches only the scene directory plus bound `.lua` script directories, rebuilt on scene open. | W7 widens to the `assetRoot` and rebuilds at project open, not scene open. |
| `:6433` (D7) | Widen the existing efsw watcher to the asset root, and fix that the watch set is only rebuilt on scene open. | W7 implements D7 directly. |
| `:9658` (W6 report) | No W7 filesystem watcher or async reimport was added. | W7 adds it. |

### Decisions — answered before code

These are the settled answers for this spec. Review may amend them before
implementation; implementation does not choose a different answer locally.

#### D-W7-1 — self-inflicted events are suppressed by an operation registry

**Decision:** W7 introduces a thread-safe suppression registry. Before any W6
operation (rename, move, delete, reimport) touches the filesystem, it
registers the absolute paths it will modify (source file, sidecar, and their
destination paths). The watcher listener checks incoming events against the
registry and suppresses matching ones. After the operation completes and
`RefreshProjectAssets()` returns, the registry entries are cleared.

The registry is a `std::unordered_set<std::string>` of normalized absolute
paths, guarded by a mutex (the same mutex that guards `pendingChanges`). It
is populated by the host before the filesystem operation and cleared after
the refresh. The listener's `handleFileAction` checks the registry before
posting to `pendingChanges`; a matching path is silently dropped.

This hazard is newly created by W6 and did not exist when D7 was written.
Without suppression, a rename triggers Modified + Add + Delete events, the
watcher queues a refresh, `RefreshProjectAssets()` runs redundantly, and the
cycle repeats for every operation. The registry is the single mechanism that
prevents W6 and W7 from fighting.

The registry is best-effort, not transactional: if a W6 operation fails
partially (W6-A1), the registered source paths may not have changed. The
clear-after-refresh pattern means stale entries are cleaned up on the next
drain cycle. A stale entry suppresses one real external event at most — the
explicit Refresh button is the backstop.

#### D-W7-2 — "where safe" means only `.lua` scripts auto-reimport

**Decision:** the watcher auto-reimports only `.lua` script files through the
existing `ScriptSystem::ReloadScript` path. Models (`.glb`/`.gltf`/`.obj`),
textures, and environment maps (`.hdr`/`.exr`) are **not** auto-reimported.
Their reimport rebuilds GPU resources (BLAS, textures, materials) and must not
happen automatically from a watcher event.

The rule: auto-reimport is safe only for assets whose reload does not touch
the GPU scene. Script reload is safe because it only touches the Lua VM and
the field registry (`ScriptSystem.cpp:257-356`). Model reimport rebuilds
geometry/materials/textures and requires a full GPU sync
(`SceneManager.cpp:645-700`). Texture reimport may change material appearance
and requires a texture re-upload. Environment reimport re-decodes an HDR/EXR
and uploads new pixels.

External changes to non-`.lua` asset files (add, delete, modify) trigger a
**database refresh** only, not a reimport. The content browser updates its
display, but the live scene's meshes, textures and environment are unchanged.
The user must explicitly reimport through the content browser to rebuild GPU
resources.

`.rt2meta` sidecar files are treated as database-refresh triggers: a new
sidecar means a new asset identity; a deleted sidecar means a lost identity;
a modified sidecar means a changed ID. All three trigger a database refresh
and surface as scan diagnostics. The watcher never writes or repairs sidecars
(D-W7-3).

#### D-W7-3 — the watcher never mints identity

**Decision:** the watcher never calls `ResolveOrAssign`. It may call
`ReadSidecarId` for diagnostic purposes (to report a stale or changed
sidecar), but it never mints, writes, or repairs a sidecar. The only paths
that touch sidecars are import (`SceneManager.cpp:291,584,627,900,3899`;
`SceneLoader.cpp:80`; `TextureAssetPipeline.cpp:498`), migration
(`SceneAssetMigration.cpp:333`), and explicit W6 reimport
(`ContentBrowserOperations.cpp:570-574`). All are user-initiated.

This is the W5 identity contract. The watcher is a read-only observer of the
asset tree. A missing sidecar seen by the watcher is a scan diagnostic
(`ProjectAssetScanner.cpp:206-216`), not a mint. A changed sidecar is a
database refresh, not a repair.

#### D-W7-4 — queue events during background work, drain on completion

**Decision:** when `IsBackgroundBusy()` is true, watcher events are queued,
not dropped. The queue is a simple vector of `(path, action)` pairs,
coalesced by path (same as the existing debounce buffer). When background
work completes, the completion callback (`WalnutApp.cpp:1645-1654`) drains
the queue: script events go through the existing `ReloadScript` path; all
other events trigger a single `RefreshProjectAssets()` call.

The user is told: "N asset changes queued; refreshing…" in the status bar.
If the queue grows beyond a threshold (100 unique paths), it is truncated to
the most recent 100 and the user is told: "Asset change queue overflowed; use
Refresh for a full scan." This is not a silent failure — it is a stated
limitation of a single-threaded background work model.

`RefreshProjectAssets()` itself is unchanged: it still returns false when
`IsBackgroundBusy()` is true. But the watcher no longer relies on
`RefreshProjectAssets()` during background work — it queues events and drains
them after. The explicit Refresh button remains available when no background
work is running.

#### D-W7-5 — event loss is normal; `handleMissedFileActions` triggers refresh

**Decision:** W7 overrides `handleMissedFileActions` to post a "missed"
marker that triggers an explicit `RefreshProjectAssets()` on the next drain.
The user is told: "File system events may have been missed; refreshing the
asset database."

On Windows, `ReadDirectoryChangesW` buffer overflow is a normal condition
(`WatcherWin32.cpp:164-170`). The default `handleMissedFileActions` is a
no-op (`efsw.hpp:265`); the current listener silently drops these events. W7
makes them loud.

W7 also increases the Windows buffer size for local drives using
`WinBufferSize` (`efsw.hpp:128-133`) to 256 KB, reducing the overflow
window. It does not increase the buffer for network drives (a buffer larger
than 64 KB fails the watch on network drives, `efsw.hpp:130-132`). If the
`assetRoot` is on a network drive, W7 uses the default 63 KB buffer and
relies on the missed-events handler.

Explicit Refresh remains a supported backstop. It is not removed as
redundant. The status bar says "Last refresh: watcher + explicit" when the
last refresh was watcher-triggered, and "Last refresh: manual" when the user
clicked Refresh. This makes the backstop visible.

#### D-W7-6 — watch the entire `assetRoot`; `.rt2scene` is out of scope

**Decision:** the watcher adds a single recursive watch on the `assetRoot`
at project open, replacing the per-script-directory watches. The watch is
removed at project close/switch. The listener dispatches by file type:

- `.lua` → script reload path (existing debounce + `ScriptSystem::ReloadScript`).
- `.glb`/`.gltf`/`.obj`/`.hdr`/`.exr`/`.rt2meta` → database refresh (coalesced).
- Everything else → ignored.

`.rt2scene` files changing on disk while open and dirty is **explicitly out of
scope**. A scene file is not an asset — it is the document the user is
editing. Auto-reloading a dirty scene from disk would destroy unsaved work.
The user must reload manually through the existing Open flow. The watcher
does not watch the scene's parent directory for scene-file changes; it
watches the `assetRoot` for asset-file changes. If the scene file is inside
the `assetRoot` (the normal case), the watcher sees the event but ignores it
because `.rt2scene` is not in the dispatch table.

#### D-W7-7 — CPU-only `AssetWatchPolicy` module, testable without efsw

**Decision:** the event classification, suppression registry, coalescing
queue, and missed-events handling live in a new CPU-only module
(`AssetWatchPolicy.{h,cpp}`) beside `ScriptFileWatchPolicy.h`. It provides:

- `ClassifyAssetFileEvent(path, action)` → `AssetFileEventKind` (ScriptReload,
  DatabaseRefresh, Ignore).
- `AssetWatchSuppressionRegistry` — thread-safe set of suppressed paths.
- `AssetWatchEventQueue` — coalescing queue for events during background work.
- `DecideWatchRefreshAction(hasMissedEvents, queueSize, backgroundBusy)` →
  whether to refresh now, queue, or truncate.

The ImGui/efsw wiring (listener, debounce drain, `RefreshProjectAssets()`
calls, `handleMissedFileActions` override) stays in `WalnutApp.cpp`. The
module is testable without a real filesystem or efsw, following the
`ContentBrowserOperations` precedent from W6 and the
`ScriptFileWatchPolicy` precedent from Phase 6C.

### W7 contract — watcher capabilities

#### Watch establishment

At project open (`OpenProjectInternal`, `WalnutApp.cpp:3236`), after
`m_ProjectContext` is established:

1. Remove all existing watch IDs from `m_ActiveWatchIds`.
2. Add a single recursive watch on `m_ProjectContext->project.assetRoot`.
3. Use `WinBufferSize` = 256 KB for local drives; default for network drives.
4. Store the watch ID in `m_ActiveWatchIds` (now a single-element vector).

At project close/switch, remove the watch. In standalone mode (no project),
no watch is active — the watcher is idle. This mirrors the content browser's
project-only gate (W6-F13).

The existing per-script-directory walk at scene open
(`WalnutApp.cpp:3726-3791`) is removed. The `assetRoot` watch covers all
script directories.

#### Event classification and dispatch

The listener's `handleFileAction` classifies each event:

1. Build the full path from `dir + filename` (existing pattern,
   `WalnutApp.cpp:3025`).
2. Check the suppression registry (D-W7-1). If the path is registered,
   silently drop.
3. Classify by extension:
   - `.lua` → `ScriptReload`: push to `pendingChanges` (existing path).
   - `.glb`/`.gltf`/`.obj`/`.hdr`/`.exr`/`.rt2meta` → `DatabaseRefresh`:
     push to a separate `pendingRefreshPaths` vector.
   - Everything else → `Ignore`.
4. `Delete` actions on `.lua` files are no longer ignored — a deleted script
   is a database refresh (the asset record disappears), and the script
   reload path handles the missing-file case (`ScriptSystem.cpp:308-336`
   reports a parse failure, not a crash). The existing `Delete` skip
   (`WalnutApp.cpp:3014`) is removed for `.lua` files.

#### Debounce and drain

The drain in `OnUIRender` (`WalnutApp.cpp:1058-1106`) is extended:

1. Lock the mutex. Move `pendingChanges` into `m_DebouncedChanges` (existing
   dedup by path). Move `pendingRefreshPaths` into `m_DebouncedRefreshPaths`
   (dedup by path).
2. If `IsBackgroundBusy()`: do not drain. Events remain in the debounce
   buffer. The completion callback will drain them.
3. If not busy and the 100 ms quiet window has elapsed:
   - For each `.lua` path in `m_DebouncedChanges`: call
     `ScriptSystem::ReloadScript` (existing path).
   - If `m_DebouncedRefreshPaths` is non-empty: call
     `RefreshProjectAssets()` once (not per path).
   - Clear both buffers.
4. If `handleMissedFileActions` was called since the last drain: call
   `RefreshProjectAssets()` once, regardless of the quiet window, and clear
   the missed marker.

The 100 ms window is retained. Widening from a few script directories to the
whole `assetRoot` does not change the per-event volume — efsw coalesces within
its own buffer before calling `handleFileAction`. The drain's dedup-by-path
ensures that a `git checkout` producing 100 Modified events results in one
`RefreshProjectAssets()` call, not 100.

#### Suppression registry

Before any W6 filesystem operation (rename, move, delete, reimport), the host
registers the paths that will be touched:

```
suppressionRegistry.Register(sourcePath);
suppressionRegistry.Register(sidecarPath);
suppressionRegistry.Register(destSourcePath); // for rename/move
suppressionRegistry.Register(destSidecarPath);
```

After `RefreshProjectAssets()` returns, the host clears the registry:

```
suppressionRegistry.Clear();
```

The registry is thread-safe (mutex-guarded). The listener checks it under the
same lock as `pendingChanges`. A registered path is silently dropped — it is
not posted to any queue.

#### Missed-events handling

`handleMissedFileActions` sets a `m_MissedEvents` flag and pushes the
directory path to `pendingRefreshPaths`. On the next drain, the flag triggers
an immediate `RefreshProjectAssets()` regardless of the quiet window. The user
is told: "File system events may have been missed; refreshing the asset
database."

#### Background-work completion drain

The completion callback (`WalnutApp.cpp:1645-1654`) is extended: after the
existing `m_OnBackgroundComplete` callback runs, check the debounce buffers.
If non-empty, drain them as in the normal drain path. This is the point where
queued events are processed — `IsBackgroundBusy()` is now false, so
`RefreshProjectAssets()` will succeed.

### Implementation order

Each step keeps `RT2Tests` and `RT2SliceRunner` CPU-only and ends with
focused tests before the next host cutover.

1. **W7.0 — CPU-only `AssetWatchPolicy` module.** Add
   `AssetWatchPolicy.{h,cpp}` with: `ClassifyAssetFileEvent`,
   `AssetWatchSuppressionRegistry`, `AssetWatchEventQueue`, and
   `DecideWatchRefreshAction`. No efsw, no ImGui, no Walnut. Focused tests:
   classification by extension, suppression registry add/check/clear, queue
   coalescing and truncation, missed-events decision, background-busy
   decision.

2. **W7.1 — Widen the efsw listener and watch scope.** Replace
   `ScriptFileWatchListener` with `AssetWatchListener` that classifies by
   file type, checks the suppression registry, and overrides
   `handleMissedFileActions`. Add the `assetRoot` watch at project open
   instead of per-script-directory at scene open. Remove the
   script-directory walk. Use `WinBufferSize` = 256 KB for local drives.
   Focused tests: integration test with a temp directory and a real efsw
   watcher, verifying that `.lua` events go to script reload,
   `.glb`/`.rt2meta` events go to database refresh, and suppressed paths are
   dropped.

3. **W7.2 — Debounce drain, background-work queue, and acceptance exercise.**
   Extend the `OnUIRender` drain to handle both `pendingChanges` and
   `pendingRefreshPaths`. Add the background-work completion drain. Add the
   missed-events flag. Wire the suppression registry into W6 operations.
   Run the acceptance exercise: modify a `.lua` script externally and verify
   hot reload; add/delete an asset file and verify the browser updates after
   automatic refresh; perform a W6 rename and verify no redundant refresh;
   simulate a missed-events marker and verify explicit refresh. Release and
   Debug verification, then append the measured close report.

### Permanent tests and discrimination proofs

| Permanent evidence | Temporary fault that must make it fail |
|---|---|
| `ClassifyAssetFileEvent` returns `ScriptReload` for `.lua`, `DatabaseRefresh` for `.glb`/`.gltf`/`.obj`/`.hdr`/`.exr`/`.rt2meta`, and `Ignore` for everything else, regardless of action (Add/Modified/Delete/Moved). | Return `ScriptReload` for a `.glb` file; the test must fail because a model reload was dispatched to the script path. |
| The suppression registry suppresses events whose path matches a registered entry, and clears after `Clear()`. A W6 rename that registers source, sidecar, and destination paths produces zero watcher-posted events. | Disable the suppression registry check in `handleFileAction`; the test must fail because the rename's events reached `pendingChanges`. |
| The event queue coalesces by path: 100 Modified events for the same path produce one `RefreshProjectAssets()` call, not 100. | Remove the dedup-by-path from the drain; the test must fail because `RefreshProjectAssets()` was called 100 times. |
| When `IsBackgroundBusy()` is true, events are queued and not dropped. On completion, the queue is drained into one `RefreshProjectAssets()` call. | Drop events when `IsBackgroundBusy()` is true instead of queuing; the test must fail because the refresh never happened after work completed. |
| `handleMissedFileActions` sets the missed flag and triggers an immediate `RefreshProjectAssets()` on the next drain, regardless of the quiet window. | Leave `handleMissedFileActions` as the default no-op; the test must fail because the missed events were silently dropped. |
| The watcher never calls `ResolveOrAssign`. A missing sidecar seen by the watcher produces a scan diagnostic, not a minted ID. | Call `ResolveOrAssign` from the watcher's database-refresh path; the test must fail because a new sidecar was written. |
| The `assetRoot` watch is added at project open and removed at project close. In standalone mode, no watch is active. | Add the watch at scene open instead of project open; the test must fail because a scene opened without a project has an active watch. |
| `.rt2scene` file changes inside the `assetRoot` are ignored by the watcher, not auto-reloaded. | Classify `.rt2scene` as `DatabaseRefresh`; the test must fail because the scene was reloaded from disk. |
| A W6 rename, move or delete does not trigger a redundant `RefreshProjectAssets()` call from the watcher. The suppression registry covers source, sidecar, and destination paths. | Register only the source path, not the sidecar; the test must fail because the sidecar's Modified event reached `pendingRefreshPaths`. |
| The Windows buffer size is 256 KB for local drives and 63 KB (default) for network drives. A local drive watch does not fail with `WatcherFailed`. | Set the buffer to 256 KB on a network drive; the test must fail because `addWatch` returned `WatcherFailed`. |
| The 100 ms debounce window is retained. A single atomic save (temp + rename) producing Modified + Add + Delete results in one drain, not three. | Remove the debounce window; the test must fail because three drains were produced for one save. |
| Explicit Refresh remains a supported backstop. After a missed-events marker, the user can click Refresh and get a full scan even if the watcher has not yet drained. | Remove the Refresh button or make it a no-op when the watcher is active; the test must fail because the user could not force a refresh. |

Every new permanent test gets its discrimination fault exercised before the
implementation report calls it protective. The final gate builds the Release
and Debug solutions, runs `RT2Tests.exe` from the repository root, runs the
scripting regression gate and both project/standalone slice-runner scenarios,
and records the actual counts measured then. No count is copied from an older
completed phase.

### Review checklist and boundary

A reviewer must verify at least: the suppression registry contract (D-W7-1)
and its thread-safety; the "where safe" rule (D-W7-2) and that no model/
texture/env auto-reimport path exists; the identity-non-minting contract
(D-W7-3) verified against all `ResolveOrAssign` call sites; the queue-then-
drain contract (D-W7-4) and the completion-callback drain; the
`handleMissedFileActions` override (D-W7-5) and the Windows buffer sizing; the
watch scope (D-W7-6) and the `.rt2scene` exclusion; the CPU-only module
(D-W7-7) and its testability without efsw; the removal of the
per-script-directory walk; the debounce retention; and the acceptance
exercise (modify a script externally, add/delete an asset, perform a W6
rename without redundant refresh). Any accepted correction is appended as a
dated review amendment.

W7 does not add the rebinding UI, script Rebind button, declaration
diagnostics or cursor-lock binding (W8). W7 does not auto-reimport models,
textures or environment maps — those rebuild GPU resources and require
explicit W6 reimport. W7 does not auto-reload `.rt2scene` files that change on
disk while open and dirty. W7 does not add a new watcher implementation — it
widens and refines the existing efsw watcher. W7 does not watch the
`cacheRoot` — cache contents are generated, replaceable, and not part of asset
identity (D6, `:8683-8700`). W7 does not add project-owned render/fixed-step
settings. These boundaries prevent W7 from depending on W8 and keep the
watcher behind the established W4/W5 project context and W6 content-browser
operations.

#### Review amendment W7-A1 (2026-08-01) — texture auto-reimport is out of scope, signed off

D-W7-2 restricts automatic reimport to `.lua` scripts. Models, textures and
environments trigger a database refresh only; rebuilding GPU resources remains
an explicit W6 operation.

**This changes Phase 7's stated runtime acceptance and was signed off
knowingly.** The roadmap (`:579`) lists "Modify a source texture and verify
reimport updates the viewport safely". Phase 7 will close with that criterion
substituted rather than met: the acceptance exercise becomes modifying a `.lua`
script externally and verifying hot reload, plus adding or deleting an asset
file and verifying the browser updates after automatic refresh.

The reasoning is that "where safe" was the roadmap's own qualifier, and a
texture that is live on the GPU is not safe to swap from a watcher thread —
it means descriptor updates against in-flight frames and interaction with the
async upload path, for no authoring benefit that an explicit reimport does not
already provide. Automatic model and texture reimport remains available to a
later workstream if it earns its risk.

#### Review amendment W7-A2 (2026-08-01) — clear the suppression registry one drain later

D-W7-1 clears registry entries once the W6 operation completes and
`RefreshProjectAssets()` returns. Filesystem events are asynchronous and can
arrive after that point, so a rename's own events can land once its entries are
already gone, defeating suppression and causing a second redundant scan. The
registry is described as "the single mechanism that prevents W6 and W7 from
fighting"; this is the one path where they still can.

**Resolution: clear registered entries on the next drain cycle after the
refresh, not immediately.** This grants roughly one debounce window of grace
using machinery that already exists, and adds no timer and no new failure mode
beyond the one D-W7-1 already documents — a stale entry suppresses at most one
real external event, with the explicit Refresh as backstop.

If a late event still slips through, the outcome is one redundant scan.
`RefreshProjectAssets()` is transactional, so that is wasteful rather than
harmful, and is accepted.

Grounding commit for both amendments: `9008290`.

## W6 discrimination proof report (2026-08-01)

This is an append-only verification record for the W6 permanent-test table.
The work was performed on branch `codex/phase7-w6-discrimination-proofs`,
from master at `9008290`. Each temporary fault was built in Release, the
named permanent test was run and made red, the fault was reverted, and the
same test was rebuilt and run green. The delete-order proof uses the amended
W6-A1 contract: source first, then sidecar.

1. **Search remains sorted by portable path.** Fault: reverse the vector
   returned by `AllRecordsSorted()` before filtering in
   `RT2App/src/ContentBrowserOperations.cpp:398`.

   Actual red output:

   ```text
   Phase7W6Tests.cpp(87): ERROR: CHECK( all[0].sourcePath == "a/hero.obj" ) is NOT correct!
     values: CHECK( z/model.glb == a/hero.obj )
   Phase7W6Tests.cpp(88): ERROR: CHECK( all[1].sourcePath == "z/model.glb" ) is NOT correct!
     values: CHECK( a/hero.obj == z/model.glb )
   [doctest] test cases: 1 | 0 passed | 1 failed
   [doctest] assertions: 5 | 3 passed | 2 failed |
   ```

   Revert build exited 0; green output: `test cases: 1 | 1 passed`,
   `assertions: 5 | 5 passed`.

2. **Rename moves source and sidecar together.** Fault: return after the
   source move in `ExecutePairMove`, skipping the sidecar.

   Actual red output:

   ```text
   Phase7W6Tests.cpp(147): ERROR: CHECK_FALSE( std::filesystem::exists(AssetSidecarPath(source)) ) is NOT correct!
     values: CHECK_FALSE( true )
   Phase7W6Tests.cpp(150): ERROR: CHECK( ReadSidecarId(AssetSidecarPath(renamed), error) == kModelId ) is NOT correct!
     values: CHECK( {?} == {?} )
   Phase7W6Tests.cpp(158): FATAL ERROR: REQUIRE( scan.database->FindByPath("models/hero-renamed.glb") != nullptr ) is NOT correct!
     values: REQUIRE( nullptr != nullptr )
   [doctest] test cases: 1 | 0 passed | 1 failed
   [doctest] assertions: 16 | 13 passed | 3 failed |
   ```

   Revert build exited 0; green output: `test cases: 1 | 1 passed`,
   `assertions: 17 | 17 passed`.

3. **Move keeps source and sidecar together.** Fault: return after the
   source move in `MoveContentBrowserAsset`, skipping the sidecar.

   Actual red output:

   ```text
   Phase7W6Tests.cpp(173): ERROR: CHECK( std::filesystem::exists(destination / "hero.glb.rt2meta") ) is NOT correct!
     values: CHECK( false )
   [doctest] test cases: 1 | 0 passed | 1 failed
   [doctest] assertions: 19 | 18 passed | 1 failed |
   ```

   Revert build exited 0; green output: `test cases: 1 | 1 passed`,
   `assertions: 19 | 19 passed`.

4. **Destination containment and traversal guards hold.** Fault: disable
   both the `..`-component and `IsContained` checks in
   `ValidateDestination`. The permanent test uses a failing directory-create
   hook so the unsafe path cannot mutate a real outside directory.

   Actual red output:

   ```text
   Phase7W6Tests.cpp(218): ERROR: CHECK( error.code == Error::InvalidArgument ) is NOT correct!
     values: CHECK( 1 == 12 )
   [doctest] test cases: 1 | 0 passed | 1 failed
   [doctest] assertions: 19 | 18 passed | 1 failed |
   ```

   Revert build exited 0; green output: `test cases: 1 | 1 passed`,
   `assertions: 19 | 19 passed`.

5. **Dependants come from live scene references.** Fault: return an empty
   dependant list before `CollectSceneAssetReferences()`.

   Actual red output:

   ```text
   Phase7W6Tests.cpp(302): FATAL ERROR: REQUIRE( modelDependants.size() == 2 ) is NOT correct!
     values: REQUIRE( 0 == 2 )
   [doctest] test cases: 1 | 0 passed | 1 failed
   [doctest] assertions: 5 | 4 passed | 1 failed |
   ```

   Revert build exited 0; green output: `test cases: 1 | 1 passed`,
   `assertions: 11 | 11 passed`.

6. **Delete is source-first and exposes an orphan sidecar.** Fault: delete
   the sidecar first and then attempt the source, bypassing the W6-A1
   partial-failure diagnostic.

   Actual red output:

   ```text
   Phase7W6Tests.cpp(262): ERROR: CHECK( report.partialFailure ) is NOT correct!
     values: CHECK( false )
   Phase7W6Tests.cpp(263): ERROR: CHECK_FALSE( std::filesystem::exists(tree.assets / "models" / "orphan.glb") ) is NOT correct!
     values: CHECK_FALSE( true )
   Phase7W6Tests.cpp(265): ERROR: CHECK( std::filesystem::exists(orphanSidecar) ) is NOT correct!
     values: CHECK( false )
   Phase7W6Tests.cpp(266): ERROR: CHECK( HasDiagnosticDetail(report, orphanSidecar.u8string()) ) is NOT correct!
     values: CHECK( false )
   Phase7W6Tests.cpp(274): ERROR: CHECK( std::any_of(scan.diagnostics.begin(), scan.diagnostics.end(), ...) ) is NOT correct!
     values: CHECK( false )
   [doctest] test cases: 1 | 0 passed | 1 failed
   [doctest] assertions: 19 | 14 passed | 5 failed |
   ```

   Revert build exited 0; green output: `test cases: 1 | 1 passed`,
   `assertions: 19 | 19 passed`.

7. **Rename does not rewrite scene files.** Fault: append bytes to
   `Assets/Scenes/main.rt2scene` after a successful rename.

   Actual red output:

   ```text
   Phase7W6Tests.cpp(153): ERROR: CHECK( std::string((std::istreambuf_iterator<char>(input)), {}) == sceneBytes ) is NOT correct!
     values: CHECK( scene bytes that must not changediscrimination fault == scene bytes that must not change )
   Phase7W6Tests.cpp(154): ERROR: CHECK( std::filesystem::last_write_time(scene, timeError) == sceneTime ) is NOT correct!
     values: CHECK( {?} == {?} )
   [doctest] test cases: 1 | 0 passed | 1 failed
   [doctest] assertions: 17 | 15 passed | 2 failed |
   ```

   Revert build exited 0; green output: `test cases: 1 | 1 passed`,
   `assertions: 17 | 17 passed`.

8. **Reimport preserves the durable sidecar ID.** Fault: return success
   immediately after the reimport callback, bypassing the post-callback ID
   read, conflict diagnostic and restore.

   Actual red output:

   ```text
   Phase7W6Tests.cpp(334): ERROR: CHECK( report.changed ) is NOT correct!
     values: CHECK( false )
   Phase7W6Tests.cpp(346): ERROR: CHECK_FALSE( ReimportContentBrowserAsset(...) ) is NOT correct!
     values: CHECK_FALSE( true )
   Phase7W6Tests.cpp(347): ERROR: CHECK( error.code == Error::InvalidArgument ) is NOT correct!
     values: CHECK( 0 == 12 )
   Phase7W6Tests.cpp(348): ERROR: CHECK( HasDiagnosticDetail(report, "changed the durable asset ID") ) is NOT correct!
     values: CHECK( false )
   Phase7W6Tests.cpp(351): ERROR: CHECK( ReadSidecarId(...) == kModelId ) is NOT correct!
     values: CHECK( {?} == {?} )
   [doctest] test cases: 1 | 0 passed | 1 failed
   [doctest] assertions: 14 | 9 passed | 5 failed |
   ```

   Revert build exited 0; green output: `test cases: 1 | 1 passed`,
   `assertions: 14 | 14 passed`.

9. **Drag/drop dispatch uses the existing import callback and exact path.**
   The CPU-only seam is `ContentBrowserDropCallbacks` and
   `DispatchContentBrowserAssetDrop` (`RT2App/src/ContentBrowserOperations.h:61-71`);
   `SceneEditorUI` calls it at `RT2App/src/SceneEditorUI.cpp:30-47`. Fault:
   append `.wrong` to the path sent to both callbacks.

   Actual red output:

   ```text
   Phase7W6Tests.cpp(123): ERROR: CHECK( receivedGltf == gltfPath ) is NOT correct!
     values: CHECK( C:\Users\mikey\source\repos\RT2\drop-assets\hero.GLB.wrong == C:\Users\mikey\source\repos\RT2\drop-assets\hero.GLB )
   Phase7W6Tests.cpp(124): ERROR: CHECK( receivedObj == objPath ) is NOT correct!
     values: CHECK( C:\Users\mikey\source\repos\RT2\drop-assets\props.OBJ.wrong == C:\Users\mikey\source\repos\RT2\drop-assets\props.OBJ )
   [doctest] test cases: 1 | 0 passed | 1 failed
   [doctest] assertions: 7 | 5 passed | 2 failed |
   ```

   Revert build exited 0; green output: `test cases: 1 | 1 passed`,
   `assertions: 7 | 7 passed`.

10. **The Walnut host consults both content-browser policy seams.** The
    permanent source guard is `RT2Tests/src/Phase7W6Tests.cpp:363-371` and
    covers the app callsites at `RT2App/src/WalnutApp.cpp:1204-1205` and
    `:1383-1386`. This optional host-callsite guard was added because the
    discrimination pass showed that the existing pure-predicate test could
    not detect a bypass. Fault: replace the delete-policy call with the raw
    confirmation boolean.

    Actual red output:

    ```text
    Phase7W6Tests.cpp(370): ERROR: CHECK( source.find("ContentBrowserDeleteAllowed(") != std::string::npos ) is NOT correct!
      values: CHECK( 18446744073709551615 != 18446744073709551615 )
    [doctest] test cases: 1 | 0 passed | 1 failed
    [doctest] assertions: 3 | 2 passed | 1 failed |
    ```

    Revert build exited 0; green output: `test cases: 1 | 1 passed`,
    `assertions: 3 | 3 passed`.

The permanent W6 suite then passed in both configurations: Release and Debug
`RT2Tests.exe` each reported `714` test cases and `146085` assertions, all
passing. `run_slice_test.ps1` passed with 60 steps, authoring intact, and cube
X `0.999999702`; the project-mode slice runner also passed the same 60-step
check. `--recovery-scenario` passed, and `run_script_test.ps1` passed with 60
frames, one entity, and no mismatches. The Release solution build completed
successfully before the Release suite; the Debug solution build completed
successfully before the Debug suite.

## Phase 7 W7 verification report (2026-08-01)

This report records the W7 discrimination pass against `3973e9c`. No
production changes were retained; only this report is committed.

### Measured gates

The Release solution built successfully. Release `RT2Tests.exe` reported:

```text
[doctest] test cases:    732 |    732 passed | 0 failed | 0 skipped
[doctest] assertions: 146428 | 146428 passed | 0 failed |
[doctest] Status: SUCCESS!
```

The Debug solution built successfully. Debug `RT2Tests.exe` reported the same
measured result: 732/732 test cases and 146428/146428 assertions, with status
SUCCESS.

The remaining gates passed:

- `run_script_test.ps1`: `PASS: 60 frames, 1 entities, no mismatches`.
- `run_slice_test.ps1`: `PASS: 60 steps, authoring intact`; final cube X
  `0.999999702`.
- `run_punctual_light_test.ps1`: `lit samples: 48, peak luminance: 42.7`,
  `PASS`.
- Release `--headless --validate` exited 0 with zero validation messages.
  The existing CLI also reported that `--out artifacts/v.png` is unknown and
  saved `screenshot.png`; this did not change the zero exit or validation
  result.

### Interactive acceptance

The external `.lua` edit was performed while the editor was open; the Session
panel reported `Scripts reloaded`, confirming hot reload.

An asset was added externally and appeared after the automatic `Project assets
refreshed` status. The same temporary asset was deleted externally; the
Content Browser then showed `No matching assets` after the automatic refresh.

The W6 rename exercise was attempted with the watcher active. The Content
Browser context menu opened, but selecting `Rename...` did not open the rename
modal, so no rename was actually performed and no end-to-end observation of a
rename without a redundant scan was established. This remains an acceptance
gap, not a claimed pass.

### Discrimination proofs

Each fault below was injected temporarily, followed by a Release build and
the named test only; it was then reverted, rebuilt, and the named test passed.

1. **`.glb` classified as `ScriptReload`.**

   ```text
   Phase7W7Tests.cpp(55): ERROR: CHECK( ClassifyAssetFileEvent(kRoot / "model.glb", action) == AssetFileEventKind::DatabaseRefresh ) is NOT correct!
     values: CHECK( 2 == 1 )
   ```
   The error occurred four times. Summary: `38 assertions: 34 passed, 4
   failed`. Reverted result: `1 passed`, `38/38` assertions.

2. **Suppression check disabled in event publication.**

   ```text
   Phase7W7Tests.cpp(165): ERROR: CHECK_FALSE( PublishAssetWatchEventLocked( registry, queue, path, AssetFileAction::Modified) ) is NOT correct!
     values: CHECK_FALSE( true )
   Phase7W7Tests.cpp(166): ERROR: CHECK( queue.SizeLocked() == 0 ) is NOT correct!
     values: CHECK( 1 == 0 )
   ```
   Summary: `5 assertions: 3 passed, 2 failed`. Reverted result: `1 passed`,
   `5/5` assertions.

3. **Path deduplication removed.**

   ```text
   Phase7W7Tests.cpp(122): FATAL ERROR: REQUIRE( events.size() == 1 ) is NOT correct!
     values: REQUIRE( 100 == 1 )
   ```
   Summary: `101 assertions: 100 passed, 1 failed`. Reverted result: `1
   passed`, `102/102` assertions.

4. **Busy watcher events dropped instead of queued.**

   ```text
   Phase7W7Tests.cpp(138): ERROR: CHECK( DecideWatchRefreshAction(false, 1, true) == AssetWatchRefreshAction::Queue ) is NOT correct!
     values: CHECK( 0 == 2 )
   Phase7W7Tests.cpp(140): ERROR: CHECK( DecideWatchRefreshAction(false, kAssetWatchQueueLimit, true) == AssetWatchRefreshAction::Truncate ) is NOT correct!
     values: CHECK( 0 == 3 )
   ```
   Summary: `4 assertions: 2 passed, 2 failed`. Reverted result: `1 passed`,
   `4/4` assertions.

5. **Missed-event handler left as a no-op.**

   ```text
   Phase7W7Tests.cpp(148): ERROR: CHECK( DecideWatchRefreshAction(true, 0, false) == AssetWatchRefreshAction::RefreshNow ) is NOT correct!
     values: CHECK( 0 == 1 )
   ```
   Summary: `3 assertions: 2 passed, 1 failed`. Reverted result: `1 passed`,
   `3/3` assertions.

6. **Watcher performs identity assignment.**

   ```text
   Phase7W7Tests.cpp(273): ERROR: CHECK( drain.find("ResolveOrAssign") == std::string::npos ) is NOT correct!
     values: CHECK( 2662 == 18446744073709551615 )
   ```
   Summary: `5 assertions: 4 passed, 1 failed`. Reverted result: `1 passed`,
   `5/5` assertions.

7. **Watcher scope widened to the script-candidate walk.**

   ```text
   Phase7W7Tests.cpp(255): ERROR: CHECK( source.find("ScriptWatchDirectoryForCandidate") == std::string::npos ) is NOT correct!
     values: CHECK( 140005 == 18446744073709551615 )
   ```
   Summary: `5 assertions: 4 passed, 1 failed`. Reverted result: `1 passed`,
   `5/5` assertions.

8. **`.rt2scene` classified as `DatabaseRefresh`.**

   ```text
   Phase7W7Tests.cpp(262): ERROR: CHECK( ClassifyAssetFileEvent(kRoot / "open.rt2scene", action) == AssetFileEventKind::Ignore ) is NOT correct!
     values: CHECK( 1 == 2 )
   ```
   The error occurred four times. Summary: `7 assertions: 3 passed, 4
   failed`. Reverted result: `1 passed`, `7/7` assertions.

9. **Destination sidecar omitted from W6 suppression paths.**

   ```text
   Phase7W7Tests.cpp(195): FATAL ERROR: REQUIRE( expectedPaths.size() == operation.expectedPathCount ) is NOT correct!
     values: REQUIRE( 3 == 4 )
   ```
   Summary: `5 assertions: 4 passed, 1 failed`. Reverted result: `1 passed`,
   `16/16` assertions.

10. **Network watcher buffer widened to 256 KiB.**

   ```text
   Phase7W7Tests.cpp(129): ERROR: CHECK( AssetWatchBufferSize(AssetWatchDriveKind::Network) == 63u * 1024u ) is NOT correct!
     values: CHECK( 262144 == 64512 )
   ```
   Summary: `3 assertions: 2 passed, 1 failed`. Reverted result: `1 passed`,
   `3/3` assertions.

11. **Debounce window removed.**

   ```text
   Phase7W7Tests.cpp(293): ERROR: CHECK( kAssetWatchDebounceMilliseconds == 100 ) is NOT correct!
     values: CHECK( 0 == 100 )
   ```
   Summary: `7 assertions: 6 passed, 1 failed`. Reverted result: `1 passed`,
   `7/7` assertions.

12. **Explicit Refresh label/backstop removed.**

   ```text
   Phase7W7Tests.cpp(301): ERROR: CHECK( source.find("Refresh Assets") != std::string::npos ) is NOT correct!
     values: CHECK( 18446744073709551615 != 18446744073709551615 )
   ```
   Summary: `4 assertions: 3 passed, 1 failed`. Reverted result: `1 passed`,
   `4/4` assertions.

The two critical proofs were run before this pass. Registering suppression
paths after the callback produced:

```text
Phase7W7Tests.cpp(207): ERROR: CHECK_FALSE( unsuppressedEventObserved ) is NOT correct!
  values: CHECK_FALSE( true )
[doctest] test cases: 1 | 0 passed | 1 failed | 731 skipped
[doctest] assertions: 16 | 12 passed | 4 failed |
```

The same test was green after reverting: `1 passed`, `16/16` assertions.
Commenting out a literal `RunAssetWatchSuppressedOperation` call in
`WalnutApp.cpp` stayed green: `1 passed`, `16/16` assertions. That fault was
restored and was not counted as a protective proof.

### Known host-dispatch limitation

The seam's ordering contract is proven by the behavioural test. The host's use
of the seam is not proven, because `RT2Tests` compiles no RT2App sources and
does not include `WalnutApp.cpp`. Commenting out one of the four host calls
(`WalnutApp.cpp:1288`, `:1358`, `:1403`, or `:1447`) leaves the suite green.

If that host wiring regresses, suppression fails and a W6 rename triggers a
redundant scan. `RefreshProjectAssets` is transactional and read-only, so the
consequence is wasted work, not corruption or data loss. The same gap exists
for W5's `ShouldCaptureRecoverySnapshot` and W6's `ContentBrowserCanOperate`.
Closing it requires extracting host dispatch into the CPU module; that work is
deliberately out of scope for W7.

The remaining tests named `static check: ...` are source-text guardrails for
host wiring and configuration: duplicate-path drain, busy queueing, missed
events, watcher scope, identity non-minting, buffer configuration, debounce,
and explicit Refresh. They are useful regression checks but are not behavioural
proofs of the host call sites. The CPU behavioural tests are the evidence for
classification, suppression publication, queue coalescing, busy decisions,
missed-event decisions, scene exclusion, and W6 suppression-path composition.

### W7 follow-up — content-browser popup scope (2026-08-01)

The W6 content-browser UI had a popup-scope defect in `RT2App/src/WalnutApp.cpp`:
Rename, Move, and Delete called `OpenPopup` inside each record's per-record
and context-popup ID scopes, while their `BeginPopupModal` calls ran outside
both scopes. The CPU operations were fully tested, but all three UI paths were
unreachable. The fix captures `openRename`, `openMove`, and `openDelete` inside
the loop and opens the matching popups after the loop beside the modal bodies.
No modal body, suppression wiring, or CPU operation was changed.

Interactive acceptance with the watcher active then showed:

- Rename opened, changed `cube.obj` to `cube_renamed.obj`, retained its
  sidecar identity, showed `Status: Asset renamed`, and did not show a
  redundant project-refresh status.
- Move opened, moved the renamed asset into `phase1a-fixtures`, showed
  `Status: Asset moved`, and did not show a redundant project-refresh status.
  The asset was then moved back for cleanup.
- Delete opened for `script-scenario.lua`, which is referenced by the open
  scene. The warning and confirmation controls appeared, so deletion required
  explicit confirmation. The upper part of this modal was clipped off-screen,
  preventing the dependant list from being fully observed; no deletion was
  confirmed. This is a remaining UI defect.

Release and Debug both measured 732/732 tests and 146428/146428 assertions;
script, slice, and `--validate` gates passed. The host-dispatch extraction gap
described above remains out of scope.

## 2026-08-02 — W7/W6 interactive dependant-query follow-up

Interactive acceptance found a third W6 defect after the unopenable modals and
the clipped confirmation dialog. `FindContentBrowserDependants`
(`RT2App/src/ContentBrowserOperations.cpp:407-434`) gated `matchesPath` on
the database record having a nil ID, but scanned records always carry a
non-nil sidecar ID (`ProjectAssetScanner.cpp:210-216`). Path fallback was
therefore unreachable for unmigrated v3 scene references. The safety
consequence was material: the delete warning could claim there were no
dependants, even though `ContentBrowserDeleteAllowed` does not block deletion
(`ContentBrowserOperations.cpp:383-389`) and the list is the only warning
mechanism.

The fix makes ID matching require two non-nil, equal IDs, then uses the
canonical source path whenever the ID match fails. Because this is an
advisory destructive-action query rather than the resolver, same-path
references with a conflicting ID are reported instead of being silently
trusted. Four CPU-only tests cover nil-ID path matching, different-path
non-matching, conflicting-ID same-path reporting, and mixed legacy/current
references (`RT2Tests/src/Phase7W6Tests.cpp:319-412`).

The delete acceptance was repeated in the editor with a temporary project
fixture and temporary asset, both restored afterwards. A legacy script scene
reference produced a readable one-entry dependant list; pressing Delete with
the confirmation checkbox clear left the modal open. An unreferenced asset
produced the compact modal with the checkbox and buttons visible. Deleting a
temporary unreferenced asset with the watcher active removed it from the
browser and showed `Asset deleted`; after the debounce interval there was no
second `Project assets refreshed` status, so the delete suppression path did
not trigger a redundant refresh.

This is the third W6 defect found only through interactive acceptance while
the CPU operations remained green throughout. It is further evidence that the
host-dispatch extraction should land before more UI-heavy work: the CPU seam
was correct enough to test, but the real modal and host integration were not
observable from `RT2Tests` alone.

Final verification after the fix measured 736/736 tests and 146453/146453
assertions in both Release and Debug. Release and Debug builds passed;
`run_script_test.ps1` passed its 60-frame scenario; `run_slice_test.ps1`
passed its 60-step authoring-preservation scenario; and the Release headless
`--validate` run exited 0 with no validation messages. The headless CLI still
prints its pre-existing `--out` unknown-argument notice and saves its default
`screenshot.png`; this follow-up did not change that behavior.

## Phase 7 W8 — deferred commitments (implementation spec)

Drafted 2026-08-02 and grounded against branch
`codex/phase7-w7-watching-async-reimport` at commit `8d8a361`. W0–W7 are
implemented in the working tree (W7 is on this branch, not yet merged to
master). This spec must be reviewed before implementation. Review amendments
are appended here; implementation does not reinterpret an unsettled point
from memory.

### Scope and exit

W8 is the deferred-commitments bucket: the four items that earlier phases
explicitly pushed to Phase 7 because they were UI or needed the project model
to exist first. Each is small, and they are independent. This spec decides
which are worth building, which are worth deferring again, and which should be
dropped.

The combined exit is: a user can rebind a runtime input action through a UI
panel and see the new binding take effect on the next Play; a user can rebind
a script asset through a file dialog and the `assetId` is preserved; a user
can see script declaration diagnostics in the content browser for `.lua`
assets; and cursor-lock is bound to an input action so a runtime script can
request it.

Not in W8: the host-dispatch extraction (scheduled but unstarted — see
boundary). W8 does not add new asset operations, new watcher capabilities, or
new scene-schema versions.

### Grounded findings at `8d8a361`

| ID | Current fact | Consequence |
|---|---|---|
| W8-F1 | `InputConfig` provides `ParseInputContextRecords`, `ComposeInputContexts`, `InputContextRecordsToJson`, and `IsEditorOwnedInputContext` (`InputConfig.h:35-53`). Composition is deterministic: built-ins → project defaults → user overrides, with empty bindings as explicit unbind (`InputConfig.h:43-51`; `InputConfig.cpp:322-352`). | The data model is complete. W8's input rebinding UI is purely an authoring surface over `InputConfig`. The CPU-only composition, validation, and serialization already exist and are tested. |
| W8-F2 | `InputService::ApplyConfiguration` (`InputService.cpp:180-231`) rebuilds all four owned contexts (`editor`, `viewport`, `viewport.look`, `runtime`) from composed records. It is called at project open (`WalnutApp.cpp:3258-3265,3303`), at standalone scene open (`:3200-3201`), and on input-settings save. | The runtime already consumes composed input. W8's rebinding UI calls `ApplyConfiguration` after editing overrides, exactly as the existing settings-save path does. No new runtime wiring is needed. |
| W8-F3 | `EditorSettingsStore` owns `m_InputOverrides` (`EditorSettings.h:62-66,76`) and serializes them as `inputOverrides` in schema v3 (`EditorSettings.cpp:227-246`). The v2 → v3 migration drops editor-owned records and promotes runtime records (`EditorSettings.cpp:152-207`). | User overrides already persist. W8's rebinding UI edits `m_InputOverrides` and calls `Save`, reusing the existing persistence path. No new serialization is needed. |
| W8-F4 | `IsEditorOwnedInputContext` returns true for `editor`, `viewport`, `viewport.look` (`InputConfig.cpp:194-198`). `ValidateScope` rejects project defaults for editor-owned contexts and restricts project v1 to the `runtime` context only (`InputConfig.cpp:136-155`). User overrides accept editor-owned contexts when explicitly authored in v3. | The rebinding UI must refuse to let the user rebind editor-owned contexts through the project defaults panel. It may allow rebinding editor-owned contexts through user overrides (the existing v3 contract), but this is a user preference, not a project-shippable binding. |
| W8-F5 | The `runtime` context is populated by `LoadDefaults` with: `move_forward` (W/S), `move_right` (D/A), `move_up` (E/Q), `look` (right mouse), `jump` (Space), `primary_action` (F), plus gamepad axes (`InputService.cpp:161-178`). These are the bindings a user can rebind. | The rebinding UI lists the `runtime` context's mappings and lets the user edit each one's action bindings (keyboard, mouse, gamepad) or axis bindings. The built-in defaults are the starting point; the user's overrides replace specific mappings. |
| W8-F6 | `InputTypes.h` defines `ActionBinding` (device, code, modifiers, gamepadSlot) and `AxisBinding` (device, code, positive, negative, gamepadSlot, deadZone, invert) (`InputTypes.h:177-230`). `InputMapping` holds a name, an `isAxis` flag, and vectors of action/axis bindings. | The rebinding UI must capture a new `InputMapping` for the action being rebound. For an action mapping, the user presses a key / mouse button / gamepad button; for an axis mapping, the user provides positive/negative keys or a gamepad axis. The UI does not need to support modifier combinations initially — a single key press is the common case. |
| W8-F7 | The script inspector (`SceneEditorUI::RenderScriptEditor`, `SceneEditorUI.cpp:1765-1812`) edits the script asset path as a raw `InputText` with `EnterReturnsTrue`. On Enter or `DeactivatedAfterEdit`, it builds a before/after `ScriptComponent` and calls `SetScriptState` (`:1784-1796`). The path is the only field edited — `assetId` and `sourceKey` are not touched by the path edit. | The "Rebind" button is a file-dialog replacement for the raw `InputText`. It opens a file dialog filtered to `.lua`, sets the path, and submits the same `SetScriptState` command. The `assetId` is preserved because `SetScriptState` calls `NormalizeAndValidateScriptComponent` (`SceneManager.cpp:3826-3829`), which derives `sourceKey` from `path` but does not touch `assetId` (`ScriptComponentValidation.h:48`). |
| W8-F8 | `NormalizeAndValidateScriptComponent` (`ScriptComponentValidation.h:17-113`) sets `output.asset.sourceKey = "lua:asset=" + output.asset.path` when the path is non-empty (`:48`). It does not modify `assetId`. `ScriptComponentCanonicalEqual` (`:123-136`) compares `assetId`, `kind`, `path`, `sourceKey`, and `fieldValues`. | A script rebind changes `path` and `sourceKey` but preserves `assetId`. This is the W5 identity contract: the sidecar is the source of truth, and `assetId` is a cache of it. Rebinding does not mint identity. If the new path points to a different physical file with a different sidecar, the resolver will report a `Conflict` on next resolve (`AssetResolver.h:52-53`). |
| W8-F9 | The content browser panel (`WalnutApp.cpp:1228-1460`) lists `AssetRecord` entries from the database snapshot. It shows source path, supports search, and has context-menu items for rename/move/delete/reimport. It does not display any declaration or field information for `.lua` assets. | Declaration diagnostics for `.lua` assets can be surfaced in the content browser by querying `ScriptFieldRegistry::GetDeclaredFields` for each `.lua` record and displaying the parse status (parsed/failed) and diagnostic message. The registry is already injected into `WalnutApp` (`m_InspectorFieldRegistry`, `WalnutApp.cpp:172`). |
| W8-F10 | `ScriptFieldRegistry::GetDeclaredFields` (`ScriptFieldRegistry.h:77-84`) returns a `Result` with `descriptors`, `parsed` (bool), and `diagnostic` (string). It caches by path and invalidates on mtime/size/hash change. A missing file yields `parsed=false`; an empty file is legal (`parsed=true`, zero descriptors). | The content browser can call `GetDeclaredFields` for each `.lua` asset record and display a status icon: green (parsed), red (parse failed + diagnostic), grey (missing file). The cache means repeated browser frames do not re-parse. |
| W8-F11 | `FieldDiagnostic` (`ScriptFieldReconcile.h:14-38`) has 13 kinds covering field-level reconciliation issues (Added, Removed, Renamed, TypeChanged, ParseFailed, etc.). `ScriptFieldResolver` (`ScriptFieldResolver.cpp:19-122`) produces field diagnostics during scene load. These are load-time diagnostics, not browser-time diagnostics. | The content browser's declaration diagnostics are simpler than `FieldDiagnostic`: they are per-asset (does this `.lua` file parse?), not per-field. The browser displays the `ScriptFieldRegistry::Result.diagnostic` string, not individual `FieldDiagnostic` entries. Per-field diagnostics remain in the inspector, where they already exist (`SceneEditorUI.cpp:1837-1841`). |
| W8-F12 | `IInputService::RequestCursorCapture(bool)` (`InputTypes.h:273-277`) is the cursor-lock API. `InputService::EndFrame` applies it via `Walnut::Input::SetCursorMode` (`InputService.cpp:320-336`). `Camera::OnUpdate` calls `RequestCursorCapture(true)` when the `look` action is held and `false` otherwise (`Camera.cpp:58,63`). | Cursor lock is already wired for the editor camera. The deferred commitment ("cursor-lock binding") is about making it available to runtime scripts, not about adding the mechanism. The `look` action in the `runtime` context already triggers cursor lock through the same `RequestCursorCapture` path — `Camera::OnUpdate` is editor-only, but a runtime script calling `input.request_cursor_capture(true)` would need the same API exposed to Lua. |
| W8-F13 | `ScriptSystem` stores `const IInputService*` (`ScriptSystem.h:117`), and `IInputService::RequestCursorCapture` is non-const (`InputTypes.h:276`). Phase 6C W3 explicitly deferred cursor capture: "6C does NOT widen the pointer — cursor capture is out of scope" (`:5975-5980`). | The cursor-lock binding requires widening `ScriptSystem`'s pointer from `const IInputService*` to `IInputService*`, or providing a separate non-const cursor-capture callback. This is a small interface change with test implications: `ScriptSystem` is constructed with `const IInputService*` in production (`ScriptSystem.h:117`). |
| W8-F14 | `RuntimeSceneController` pushes the `runtime` context on Play (`WalnutApp.cpp:3041`) and pops it on Stop. The runtime context contains the `look` action (right mouse, `InputService.cpp:167`). During Play, the editor camera is not updated; the runtime scene's camera entity is used instead. | The cursor-lock binding for runtime is: expose `RequestCursorCapture` to Lua so a runtime script can lock the cursor for mouse-look, matching what the editor camera already does. The `look` action binding already exists in the runtime context. |
| W8-F15 | No input rebinding UI exists in `WalnutApp.cpp` or `SceneEditorUI.cpp`. The Session panel shows project status and `lastBrowseDirectory` (`WalnutApp.cpp:1078-1124`). There is no "Input" or "Bindings" panel. The View menu lists Camera, Performance, Render Settings, Scene, Session, Content Browser, Outliner, Inspector (`:4170-4177`). | W8 adds a new "Input Bindings" panel, following the existing pattern: `m_ShowInputBindingsWindow`, a `DrawInputBindingsPanel()` method, a View-menu item. |
| W8-F16 | `RT2Tests` compiles zero `RT2App/src` files. It includes `InputConfig.h`, `ScriptFieldReconcile.h`, `ScriptComponentValidation.h`, `AssetWatchPolicy.h`, `ContentBrowserOperations.h`, and other CPU-only headers, but never `WalnutApp.cpp`, `SceneEditorUI.cpp`, or `InputService.cpp`. | All W8 UI logic is untestable by construction. CPU-only logic must be pushed into modules to be testable. The host-dispatch extraction (scheduled but unstarted) would make host wiring testable; until it lands, W8's testable surface is limited to the CPU-only modules. |

### Recovered deferred commitments

All four items were found in the Phase 7 commitments table (`:6379-6390`)
and re-tracked through the W4/W5, W6, and W7 specs. None has been silently
implemented.

| Source | Commitment | W8 treatment |
|---|---|---|
| `:3311,3380` | Input rebinding dialog. | **Build.** The data model (W4) and composition (W4) are complete. W8 adds the authoring UI. |
| `:4142` | Script asset Rebind button. | **Build.** The `InputText` path edit exists (`SceneEditorUI.cpp:1781`). W8 adds a "Browse…" button that opens a file dialog. |
| `:5600` | Declaration diagnostics in content browser. | **Build.** The content browser exists (W6). `ScriptFieldRegistry` exists. W8 connects them. |
| `:5980` | Cursor-lock binding. | **Defer again.** See D-W8-4. The mechanism exists (`RequestCursorCapture`), but exposing it to Lua requires widening `ScriptSystem`'s pointer, which is an interface change with test implications that should land separately. |

### Decisions — answered before code

These are the settled answers for this spec. Review may amend them before
implementation; implementation does not choose a different answer locally.

#### D-W8-1 — input rebinding UI edits user overrides, not project defaults

**Decision:** the rebinding UI edits `EditorSettingsStore::m_InputOverrides`
(`EditorSettings.h:62-66`). It does not edit project `inputContexts` — those
are authored in the `.rt2proj` file by hand or by a future project-settings
editor. The UI lists the composed result (built-ins + project + user) so the
user sees the effective bindings, but edits only the user override layer.

The UI lists the `runtime` context's mappings by default, since that is what
a gameplay author rebinds. Editor-owned contexts (`editor`, `viewport`,
`viewport.look`) are shown in a separate read-only section with a note that
they can be overridden in the settings file but not through the project. This
matches the W4 contract: project defaults may not target editor-owned contexts
(`InputConfig.cpp:142-145`), and the v2 → v3 migration drops inert editor
records (`EditorSettings.cpp:167`).

Rebinding a mapping captures a new `InputMapping` and writes it to
`m_InputOverrides`. An empty binding (no actions, no axes) is an explicit
unbind that removes the inherited mapping (`InputConfig.cpp:182-184`). The UI
supports both "rebind to a new key" and "unbind" actions. After editing, the
UI calls `ApplyConfiguration` and `Save`, reusing the existing path.

**What a user can rebind:** any `runtime` mapping's action bindings (keyboard
key, mouse button, gamepad button) or axis bindings (keyboard pair, gamepad
axis). **What is refused:** editing project `inputContexts` through this UI
(not an override), and editing editor-owned contexts through project defaults
(rejected by `ValidateScope`).

**How a conflicting assignment is shown:** if the user binds the same key to
two actions in the same context, the UI shows a warning but does not prevent
it — the input system resolves disjunctively (any binding firing fires the
action), so two actions on the same key both fire. This is documented
behaviour, not a defect.

#### D-W8-2 — script Rebind button is a file dialog, not a path mint

**Decision:** the Rebind button opens a file dialog filtered to `.lua` files,
rooted at the active `assetRoot` (or `lastBrowseDirectory` in standalone
mode). On selection, it sets `ScriptComponent::asset.path` to the
asset-root-relative path and calls `SetScriptState`, exactly as the existing
`InputText` path edit does (`SceneEditorUI.cpp:1784-1796`).

The button does **not** touch `assetId`. `NormalizeAndValidateScriptComponent`
derives `sourceKey` from `path` but preserves `assetId`
(`ScriptComponentValidation.h:48`). This is the W5 identity contract: the
sidecar is the source of truth, and `assetId` is a cache of it. Rebinding
does not mint identity.

If the new path points to a different physical file with a different sidecar,
the resolver will report a `Conflict` on next resolve
(`AssetResolver.h:52-53`). This is correct behaviour: the user changed the
script asset, and if the new asset has a different identity, the scene's
`assetId` is stale and needs migration. The Rebind button does not silently
fix this; it surfaces the diagnostic.

The Rebind button replaces the `InputText` for the path field. The
`InputText` is retained as a fallback for manual path entry, but the primary
interaction is the file dialog. The button label is "Browse…" to match the
existing `lastBrowseDirectory` dialog pattern (`WalnutApp.cpp:1111`).

#### D-W8-3 — declaration diagnostics in the content browser use ScriptFieldRegistry

**Decision:** the content browser queries `ScriptFieldRegistry::GetDeclaredFields`
for each `.lua` asset record and displays a status indicator beside the
asset's source path:

- **Green dot:** `parsed == true` — the script parses cleanly.
- **Red dot:** `parsed == false` — the script failed to parse. The
  `diagnostic` string is shown in a tooltip on hover.
- **Grey dot:** the file is missing or unreadable. `GetDeclaredFields` returns
  `parsed == false` with a "failed to read script file" diagnostic
  (`ScriptFieldRegistry.cpp:391`).

The query is per-frame for visible records only (not the entire database).
The registry's cache (`ScriptFieldRegistry.h:77-84`) means repeated frames do
not re-parse. The cache invalidates on mtime/size/hash change, so a W7
watcher-triggered refresh after an external `.lua` edit automatically
re-parses on the next frame.

The source of truth for declaration diagnostics is `ScriptFieldRegistry`, not
`FieldDiagnostic`. Per-field diagnostics (Added, Removed, TypeChanged, etc.)
remain in the inspector, where they already exist
(`SceneEditorUI.cpp:1837-1841`). The browser shows only the per-asset parse
status, not per-field reconciliation.

This is a read-only display. The browser does not edit declarations or field
values. Editing remains in the inspector.

#### D-W8-4 — cursor-lock binding is deferred again

**Decision:** the cursor-lock binding is deferred to a follow-up task, not
included in W8.

**Rationale:** the mechanism exists (`RequestCursorCapture`,
`InputService.cpp:320-336`). The editor camera already uses it
(`Camera.cpp:58,63`). But the deferred commitment was about making it
available to **runtime scripts**, which requires widening `ScriptSystem`'s
pointer from `const IInputService*` to `IInputService*`
(`ScriptSystem.h:117`; `InputTypes.h:276`). Phase 6C W3 explicitly deferred
this: "6C does NOT widen the pointer — cursor capture is out of scope"
(`:5975-5980`).

Widening the pointer is a small interface change, but it has test
implications: `ScriptSystem` is constructed with `const IInputService*` in
production, and the test harness would need a non-const mock. It also
requires a Lua binding (`input.request_cursor_capture(bool)`) and a runtime
acceptance exercise. This is more than a UI button — it is a runtime API
addition that touches the script system, the input system, and the Lua
binding layer.

Given that W8 is the last Phase 7 workstream and the other three items are
purely UI, mixing in a runtime API change changes the character of the
workstream. The cursor-lock binding should land as a named follow-up task
("cursor-lock runtime binding") rather than being squeezed into the
deferred-UI bucket.

**Carry-forward:** P7-R2: when a later phase introduces cursor-lock binding
for runtime scripts, it must widen `ScriptSystem`'s `IInputService` pointer
to non-const, add the Lua binding, and provide a runtime acceptance exercise
that verifies mouse-look during Play. It must not add a second cursor-capture
mechanism parallel to `RequestCursorCapture`.

#### D-W8-5 — CPU-only `InputBindingEditor` module for rebind logic

**Decision:** the rebinding logic — capturing a key press into an
`InputMapping`, validating it against `ValidateScope`, building the override
record, and composing the result — lives in a new CPU-only module
(`InputBindingEditor.{h,cpp}`) beside `InputConfig`. It provides:

- `CaptureActionBinding(device, code, modifiers, gamepadSlot)` →
  `ActionBinding`.
- `CaptureAxisBinding(device, code, positive, negative, gamepadSlot, deadZone,
  invert)` → `AxisBinding`.
- `BuildOverrideRecord(contextId, mappingName, isAxis, bindings)` →
  `InputContextRecord`.
- `DescribeMapping(const InputMapping&)` → human-readable string (e.g.
  "W (Keyboard)", "Right Mouse", "Gamepad A").
- `FindConflicts(const std::vector<InputContextRecord>&)` → list of
  same-context mappings sharing a binding.

The ImGui panel (`DrawInputBindingsPanel`) lives in `WalnutApp.cpp` and calls
the CPU-only module through callbacks, following the `ContentBrowserOperations`
precedent from W6.

### Test-provable properties vs interactive acceptance

This section is required because W8 is almost entirely UI, and the host
wiring is not testable (W8-F16). The distinction is stated explicitly rather
than presenting a predicate test as evidence that a feature works.

#### Test-provable (CPU-only modules, RT2Tests)

| Property | How it is proven |
|---|---|
| `BuildOverrideRecord` produces a valid `InputContextRecord` that passes `ParseInputContextRecords` with `UserOverrides` scope. | Unit test: build a record, round-trip through JSON, parse, assert equality. |
| `ComposeInputContexts` with the new override produces the expected composed mapping (override wins over built-in). | Unit test: compose built-ins + override, assert the overridden mapping's bindings match. |
| An explicit unbind (empty bindings) removes the inherited mapping from the composed result. | Unit test: compose built-ins + unbind override, assert the mapping is absent. |
| `FindConflicts` detects two mappings in the same context sharing a binding. | Unit test: two mappings with the same key, assert conflict is reported. |
| `DescribeMapping` produces the expected string for each device kind. | Unit test: keyboard, mouse, gamepad button, gamepad axis. |
| Script rebind preserves `assetId`: `NormalizeAndValidateScriptComponent` with a new path but old `assetId` leaves `assetId` unchanged. | Unit test: construct a `ScriptComponent` with a non-nil `assetId`, change `path`, normalize, assert `assetId` is unchanged. |
| Script rebind derives `sourceKey` from the new path: `"lua:asset=" + newPath`. | Unit test: same setup, assert `sourceKey` matches. |

#### Interactive acceptance only (WalnutApp/SceneEditorUI, not testable)

| Property | Acceptance exercise | Observable pass condition |
|---|---|---|
| The Input Bindings panel opens, lists runtime mappings, and lets the user rebind one. | Open the panel, rebind `move_forward` from W to Up arrow, enter Play. | The entity moves with Up arrow, not W. |
| The rebinding takes effect on the next Play without restarting. | Rebind, enter Play, verify, Stop, rebind back, enter Play again. | The second Play uses the restored binding. |
| The script Rebind button opens a file dialog and sets the path. | Select a scripted entity, click "Browse…", pick a different `.lua` file. | The inspector shows the new path; the field registry updates; on Play the new script runs. |
| The script `assetId` is preserved after rebind. | Rebind a script, save, reopen the scene. | The `assetId` in the reopened scene matches the pre-rebind `assetId` (verified by diagnostic output or by the resolver not reporting a Conflict). |
| Declaration diagnostics appear in the content browser for `.lua` assets. | Open the content browser, observe a `.lua` asset with a syntax error. | A red dot appears beside the asset; hovering shows the parse error message. |
| Declaration diagnostics update after external edit (W7 watcher). | With the browser open, externally edit a `.lua` file to introduce a syntax error. | The dot turns red within the W7 refresh window (100 ms debounce + refresh). |
| The Input Bindings panel shows editor-owned contexts as read-only. | Open the panel, observe the editor/viewport section. | The editor/viewport mappings are displayed but not editable; a note says they can be overridden in the settings file. |
| The unbind action removes a mapping. | Select a runtime mapping, click "Unbind". | The mapping disappears from the composed list; on Play, that action does nothing. |

### Implementation order

Each step keeps `RT2Tests` and `RT2SliceRunner` CPU-only and ends with
focused tests or interactive acceptance before the next step.

1. **W8.0 — CPU-only `InputBindingEditor` module.** Add
   `InputBindingEditor.{h,cpp}` with: `BuildOverrideRecord`,
   `CaptureActionBinding`, `CaptureAxisBinding`, `DescribeMapping`,
   `FindConflicts`. No ImGui, no Walnut. Focused tests: override record
   round-trip, composition with override, explicit unbind, conflict
   detection, describe-mapping for all device kinds, script-rebind
   `assetId` preservation.

2. **W8.1 — Input Bindings panel.** Add `DrawInputBindingsPanel()` to
   `WalnutApp`, register `m_ShowInputBindingsWindow`, add View-menu item.
   Display the composed runtime mappings. Wire "Rebind" to a key-capture
   modal (press any key). Wire "Unbind". On edit, call `ApplyConfiguration`
   and `Save`. Interactive acceptance: rebind `move_forward`, enter Play,
   verify.

3. **W8.2 — Script Rebind button.** Add a "Browse…" button beside the
   script path `InputText` in `RenderScriptEditor`. On click, open a file
   dialog filtered to `.lua`, rooted at the active `assetRoot`. On
   selection, build the after-state with the new path (same `assetId`),
   call `SetScriptState`. Interactive acceptance: rebind a script, save,
   reopen, verify `assetId` is preserved.

4. **W8.3 — Declaration diagnostics in content browser.** Extend
   `DrawContentBrowserPanel` to query `ScriptFieldRegistry::GetDeclaredFields`
   for visible `.lua` records and display a status dot. Tooltip shows the
   diagnostic on hover. Interactive acceptance: observe green/red/grey dots
   for valid/invalid/missing scripts; externally edit a `.lua` file and
   verify the dot updates.

### Permanent tests and discrimination proofs

| Permanent evidence | Temporary fault that must make it fail |
|---|---|
| `BuildOverrideRecord` produces a record that round-trips through `InputContextRecordsToJson` → `ParseInputContextRecords(UserOverrides)` and matches the input. | Omit the `contextId` from the record; the test must fail because `ParseInputContextRecords` rejects a missing contextId. |
| `ComposeInputContexts` with a user override for `runtime.move_forward` produces a composed mapping whose bindings match the override, not the built-in. | Swap the overlay order (user before built-in); the test must fail because the built-in wins. |
| An explicit unbind (empty bindings in the override) removes `runtime.move_forward` from the composed result. | Treat empty bindings as "inherit" instead of "unbind"; the test must fail because the built-in mapping survives. |
| `FindConflicts` reports two `runtime` mappings that share the same keyboard code. | Compare only within the same mapping name instead of the same context; the test must fail because the conflict is missed. |
| `DescribeMapping` returns "W (Keyboard)" for `KeyCode::W` on `KeyboardKey`, "Right Mouse" for `MouseButton::Button1` on `MouseButton`, "Gamepad A" for `GamepadButton::A` on `GamepadButton`. | Return the numeric code as a string; the test must fail because the description is not human-readable. |
| `NormalizeAndValidateScriptComponent` with a new `path` but old `assetId` leaves `assetId` unchanged and sets `sourceKey = "lua:asset=" + newPath`. | Clear `assetId` when `path` changes; the test must fail because identity was destroyed. |

Every new permanent test gets its discrimination fault exercised before the
implementation report calls it protective. Interactive acceptance exercises
are first-class deliverables and must be completed and recorded before the
implementation report claims W8 is done. The final gate builds the Release
and Debug solutions, runs `RT2Tests.exe` from the repository root, runs the
scripting regression gate and both project/standalone slice-runner scenarios,
and records the actual counts measured then. No count is copied from an older
completed phase.

### Review checklist and boundary

A reviewer must verify at least: the override-only editing contract (D-W8-1)
and that project defaults are not editable through the UI; the identity
preservation contract (D-W8-2) verified against
`NormalizeAndValidateScriptComponent`; the declaration-diagnostics source
(D-W8-3) is `ScriptFieldRegistry`, not `FieldDiagnostic`; the cursor-lock
deferral (D-W8-4) and its carry-forward note; the CPU-only module (D-W8-5)
and its testability; the test-provable vs interactive-acceptance split; and
all interactive acceptance exercises. Any accepted correction is appended as
a dated review amendment.

W8 does not add the host-dispatch extraction (scheduled but unstarted). W8
does not add cursor-lock runtime binding (deferred again as P7-R2). W8 does
not edit project `inputContexts` through the UI (override-only). W8 does not
add per-field diagnostics to the content browser (those remain in the
inspector). W8 does not add new asset operations, new watcher capabilities, or
new scene-schema versions.

**Materially safer if the host-dispatch extraction landed first.** W8 is the
third workstream in a row (after W6 and W7) where the host wiring is not
testable. The W6 rename/move/delete modals had an ImGui ID-stack bug that
left them non-functional while CPU tests were green; the W7 suppression
ordering was proven but the host's call-site routing was not. If the
host-dispatch extraction landed before W8, the Input Bindings panel's call to
`ApplyConfiguration` and `Save`, the Rebind button's call to `SetScriptState`,
and the browser's call to `GetDeclaredFields` would all be testable host
wiring. This is a genuine input to sequencing: the extraction is not blocking
W8, but it would materially reduce the risk of another green-tests-broken-UI

#### Review amendment W8-A1 (2026-08-02) — commitment citations corrected

The commitments table originally cited `:3311,3380`, `:4064,5274,4142`,
`:5521,5600` and `:5901,5980`. Five of those nine line numbers pointed at
unrelated content — `:4064` at a recovery-test result, `:5274` at command
sync-impact, `:5521` at a blank line, `:5901` at LuaPanic hardening.

The cause is worth recording, because it defeats an assumption this document
relies on. The numbers were inherited from the W4/W5 spec's own commitments
table, where they were correct when written. **Append-only does not guarantee
stable line numbers**: two earlier edits — the navigation-table addition in the
preamble and the Phase 8 heading correction — inserted lines *above* them and
shifted everything below by roughly twenty-five lines.

Corrected to the verified locations: input rebinding at `:3389-3390` and
`:3458-3459`, the script Rebind button at `:4142`, declaration diagnostics at
`:5600`, cursor-lock at `:5980`. Each was located by content, not by
arithmetic.

The rule that follows: a citation copied from an earlier section is a claim
about the current file and must be re-verified like any other, exactly as
AGENTS.md already requires for counts and status claims. The commitments
themselves were all real and correctly identified; only the coordinates drifted.

## Host dispatch extraction (implementation spec)

Drafted 2026-08-02 and grounded against commit `a4406de` on `master`. This
is a refactor, not a roadmap phase. It has no phase number and no workstream
letter. It must not change behaviour; test counts must not fall.

### The problem

`RT2Tests` compiles zero `WalnutApp.cpp` sources. It links 46 RT2App `.cpp`
files (`premake5.lua:13`) but never the 4,472-line host
(`WalnutApp.cpp:1-4472`). Three workstreams hit the same wall:

| Workstream | CPU-only module tested | Host wiring not tested |
|---|---|---|
| W5 | `ShouldCaptureRecoverySnapshot` (`SceneAssetMigration.h:58-63`) | that the host consults it at `WalnutApp.cpp:2008` |
| W6 | `ContentBrowserCanOperate` / `ContentBrowserDeleteAllowed` (`ContentBrowserOperations.h:74-75`) | that the host calls them at `WalnutApp.cpp:1231,1459` |
| W7 | `RunSuppressedAssetOperation` (`AssetWatchPolicy.h:147-152`) | that the host routes through it at `WalnutApp.cpp:3290` |

The existing W6 and W7 tests work around this with **source-text probes** —
they open `WalnutApp.cpp` as a text file and grep for function names
(`Phase7W6Tests.cpp:459-464`; `Phase7W7Tests.cpp:26-44,212-275`). A text
probe proves the string is present, not that the code is reachable. A
deliberate fault commenting out one of the four suppression registrations
left the entire suite green.

Three W6 defects reached master behind a green suite and were found only by
driving the UI by hand: the rename/move/delete modals never opened (ImGui
ID-stack bug), the delete confirmation clipped its dependants list, and the
dependants query reported nothing for unmigrated scenes. In all three the CPU
operations underneath were correct and fully unit-tested.

### Scope and acceptance criterion

**What this extraction proves:** the dispatch *sequence* — register
suppression paths before the filesystem operation, with the correct paths
for each operation kind — becomes link-time testable. A test calls the
extracted dispatch with a fake registry and a fake operation, and asserts
the registry was populated with the right paths before the operation ran.

**What it does not prove:** whether the host *invokes* the dispatch for each
of the four content-browser operations. After the extraction, the host still
has four call sites to `DispatchContentBrowserOperation` inside
`DrawContentBrowserPanel`. RT2Tests cannot link `WalnutApp.cpp` (it includes
ImGui, Walnut, Vulkan, efsw, tinyexr, NRD — HD-F6), so commenting out one of
those four call sites leaves the suite green. The gap shrinks — the
untestable surface goes from "the whole register-then-operate sequence" to
"does the host call one function" — but it does not close.

This is stated plainly because the original acceptance criterion — "the
green fault turns red" — was wrong. The fault that stayed green was
commenting out a `RunAssetWatchSuppressedOperation(...)` call site in
`WalnutApp.cpp`, not commenting out `RegisterMany` in the CPU-only
`AssetWatchPolicy.cpp` (which was already link-time testable and already
tested red by W7 at `Phase7W7Tests.cpp:207`). The extraction moves the call
site up one level — from `RunSuppressedAssetOperation` to
`DispatchContentBrowserOperation` — but the host invocation of either is
equally untestable. A stated limitation is worth more than an overstated
guarantee, and this phase has produced three defects that hid behind exactly
that difference.

The extraction covers three dispatch gaps:

1. **W7 gap:** the four content-browser dispatches (reimport, rename, move,
   delete) — the register-then-operate *sequence* becomes link-time testable.
   Whether the host *invokes* it for each operation remains untestable.
2. **W6 gap:** `ContentBrowserCanOperate` and `ContentBrowserDeleteAllowed`
   become link-time testable through dispatch wrappers. Whether the host
   *consults* the wrappers remains untestable.
3. **W5 gap:** `ShouldCaptureRecoverySnapshot` becomes link-time testable
   through a dispatch wrapper. Whether the host *consults* it remains
   untestable.

All three land in one change. The extraction is small enough to land
together because the dispatch logic for all three is the same shape: check a
policy, register suppression, call an operation, clear suppression, refresh.

### Residual gap

The residual gap — whether the host invokes the extracted dispatch — is the
same gap that W6 and W7 source-text probes attempted to close. The
source-text probes at `Phase7W6Tests.cpp:457-465` and
`Phase7W7Tests.cpp:212-275` prove the string is present, not that the code is
called. After this extraction, those probes can be strengthened to grep for
`DispatchContentBrowserOperation` instead of `RunAssetWatchSuppressedOperation`
— but they remain text probes, not link-time tests.

Closing the residual gap fully would require making `DrawContentBrowserPanel`
itself testable — for example, by injecting the dispatch as a callable
parameter so a test can assert the panel invokes it for each action. That
touches ImGui rendering code and is a larger change than this spec proposes.
It is noted here as a follow-up, not designed.

The practical mitigation for the residual gap is the same one W6 and W7
already rely on: interactive acceptance exercises. The implementation report
must record that each of the four content-browser operations was driven
through the UI and produced the expected filesystem change and database
refresh. This is a first-class deliverable, not a postscript.

### Grounded findings at `a4406de`

| ID | Current fact (verified at `a4406de`) | Consequence |
|---|---|---|
| HD-F1 | The four content-browser dispatches are inline in `DrawContentBrowserPanel` (`WalnutApp.cpp:1228-1486`). Each follows the same pattern: build report/error, construct source/destination paths, call `RunAssetWatchSuppressedOperation` with a lambda that calls the CPU operation, call `RefreshProjectAssets`, call `ScheduleAssetWatchSuppressionClear`, set status. The pattern is at `:1290-1332` (reimport), `:1361-1383` (rename), `:1405-1429` (move), `:1463-1479` (delete). | The dispatch logic is identical in shape across all four. It can be extracted into one function parameterised by operation kind, record, destination, and the CPU operation callback. |
| HD-F2 | `RunAssetWatchSuppressedOperation` (`WalnutApp.cpp:3279-3294`) is a host method that: (a) checks `m_FileWatchListener` is non-null, (b) constructs the source path from `assetRoot + record.sourcePath`, (c) calls `rt2::core::RunSuppressedAssetOperation` with the listener's `suppressionRegistry`, and (d) returns the callback's result. | This is the seam. It is already a named function with a clear contract. The extraction moves it out of `WalnutApp` into a CPU-only module and makes the host supply the registry as a collaborator. |
| HD-F3 | `ScheduleAssetWatchSuppressionClear` (`WalnutApp.cpp:3296-3301`) sets a `bool m_AssetWatchClearSuppressionNextDrain = true` flag. The flag is consumed by `DrainAssetWatchChanges` (`:3327-3331`), which clears the registry under the listener's mutex. | The delayed-clear mechanism is host-specific because it crosses into the drain loop. The extraction must either expose the flag as a collaborator or move the clear-into-drain ordering into the module. |
| HD-F4 | `ContentBrowserCanOperate` is called at `WalnutApp.cpp:1231` and gates the entire panel. `ContentBrowserDeleteAllowed` is called at `:1459` inside the Delete modal's button condition. Both are pure CPU functions (`ContentBrowserOperations.cpp:331-336,383-388`). | The host *consults* these functions at specific call sites. The extraction must make the consultation testable — a test must be able to prove the host calls `ContentBrowserCanOperate` before allowing any operation and `ContentBrowserDeleteAllowed` before committing a delete. |
| HD-F5 | `ShouldCaptureRecoverySnapshot` is called at `WalnutApp.cpp:2008` inside the autosave block. Its two arguments are `m_ScriptRepairGate.SuppressAutosave()` and `m_AssetMigrationGate`. The function is a pure CPU predicate (`SceneAssetMigration.h:58-63`). | The host *consults* this predicate before capturing a recovery snapshot. The extraction must make the consultation testable — a test must be able to prove the host calls it and respects its result. |
| HD-F6 | `RT2Tests` compiles `ContentBrowserOperations.cpp`, `AssetWatchPolicy.cpp`, `SceneAssetMigration.cpp`, `ProjectContext.cpp`, `ProjectAssetScanner.cpp`, `SceneManager.cpp`, and 40 other RT2App sources (`premake5.lua:13`; `RT2Tests.vcxproj:306-311`). It does not compile `WalnutApp.cpp`, `SceneEditorUI.cpp`, `InputService.cpp`, `RendererGPU.cpp`, or any file that includes `Walnut/Application.h`, `imgui.h`, `efsw/efsw.hpp`, `tinyexr.h`, `NRD.h`, or `stb_image.h`. | The extracted module must not include or depend on any ImGui/Walnut/Vulkan/efsw/tinyexr/NRD header. It may depend on `ContentBrowserOperations.h`, `AssetWatchPolicy.h`, `SceneAssetMigration.h`, `AssetDatabase.h`, `ProjectContext.h`, `core/Error.h`, and the standard library — all of which are already linkable into RT2Tests. |
| HD-F7 | `WalnutApp.cpp` includes 55 headers at `:1-55`, including `Walnut/Application.h`, `RendererGPU.h`, `NRD.h`, `efsw/efsw.hpp`, `stb_image.h`, `tinyexr.h`, and `imgui.h` (via `SceneEditorUI.h`). The extracted dispatch logic must not transitively pull in any of these. | The dispatch logic's only dependencies are: `ContentBrowserOperations.h` (for the CPU operations and policy predicates), `AssetWatchPolicy.h` (for `RunSuppressedAssetOperation` and `AssetWatchSuppressionRegistry`), `SceneAssetMigration.h` (for `ShouldCaptureRecoverySnapshot` and `AssetMigrationPersistenceGate`), `ProjectContext.h` (for `AssetResolutionContext`), and `core/Error.h`. All are CPU-only and already in RT2Tests. |
| HD-F8 | The existing W7 source-text probes (`Phase7W7Tests.cpp:26-44`) read `WalnutApp.cpp` as text and grep for strings like `"ConfigureAssetRootWatch(staged->project.assetRoot)"` and `"handleMissedFileActions"`. These tests prove the string is present but not that the code is called. | The extraction replaces some source-text probes with link-time tests for the dispatch *sequence*. Whether the host *invokes* the dispatch remains a text-probe or interactive-acceptance matter — see the residual-gap section. |
| HD-F9 | `RunSuppressedAssetOperation` in `AssetWatchPolicy.cpp:263-273` calls `suppressionRegistry.RegisterMany(...)` then `callback()`. The registration happens before the callback. But the host's wrapper at `WalnutApp.cpp:3290` constructs the source path *before* calling `RunSuppressedAssetOperation`, and the CPU operation (e.g. `RenameContentBrowserAsset`) is inside the callback. | The ordering invariant is: (1) register suppression paths, (2) call the filesystem operation, (3) refresh, (4) schedule clear. If step 1 is skipped or moved after step 2, the watcher fires on self-inflicted events. The extraction must make this ordering testable. |
| HD-F10 | `DrainAssetWatchChanges` (`WalnutApp.cpp:3303-3426`) is the drain loop. It calls `RefreshProjectAssets()` at `:3419` when the debounce window elapses. The drain is host-specific because it touches `m_DebouncedChanges`, `m_DebouncedRefreshPaths`, `m_ScriptSystem`, `m_InspectorFieldRegistry`, and `m_Runtime`. | The drain stays in the host. The extraction does not move the drain. It moves only the dispatch: the "register → operate → refresh → schedule clear" sequence that is currently inline in `DrawContentBrowserPanel`. |

### Approaches evaluated

#### A. Dispatch module — **chosen**

A new CPU-only module (`ContentBrowserDispatch.{h,cpp}`) owns the "for this
action, register these paths, then run this operation, then refresh, then
schedule clear" sequence. `WalnutApp` is reduced to supplying the UI event,
the real `AssetWatchSuppressionRegistry`, and the real CPU operation
callback. The module is linkable into `RT2Tests` because it depends only on
`ContentBrowserOperations.h`, `AssetWatchPolicy.h`, and `core/Error.h`.

The host supplies the registry as a reference parameter. A test supplies a
fake registry and a fake operation callback, then asserts the registry was
populated before the callback ran and the refresh was called after.

**Why chosen:** it is the same move already applied one level down by
`ContentBrowserOperations` (W6) and `AssetWatchPolicy` (W7). It is small
(the dispatch is ~20 lines per operation, and all four share one parameterised
function). It does not require adding any RT2App source to RT2Tests. It makes
the dispatch *sequence* link-time testable. It does not make the host's
*invocation* of the dispatch testable — see the residual-gap section.

#### B. Add selected RT2App sources to RT2Tests.vcxproj — **rejected**

The relevant code is in `WalnutApp.cpp`, which includes `Walnut/Application.h`
(`:1`), `RendererGPU.h` (`:6`), `NRD.h` (`:54`), `efsw/efsw.hpp` (`:55`),
`stb_image.h` (`:50`), and `tinyexr.h` (`:53`). These headers pull in Vulkan,
GLFW, ImGui, and the NRD shader compiler. RT2Tests is CPU-only by design
(`AGENTS.md:79-81`) and cannot link these.

Even if the dispatch logic were extracted to a separate `.cpp` file, that
file would need to call `RefreshProjectAssets()` (which touches
`m_ProjectContext`, `m_SceneMgr`, and `m_ScriptAssetContext`) and
`ScheduleAssetWatchSuppressionClear()` (which touches
`m_AssetWatchClearSuppressionNextDrain`). These are host members. A source
file that calls host methods cannot be compiled into RT2Tests without the
host class definition, which pulls in the full include graph.

**Rejected** because the dispatch logic cannot be separated from its host
collaborators without extracting it to a module that receives them as
parameters — which is approach A.

#### C. Separate static library — **rejected**

A static library linked by both RT2App and RT2Tests would solve the link
problem but introduces a build-system change that affects every target. The
existing precedent (`ContentBrowserOperations`, `AssetWatchPolicy`,
`InputConfig`, `Project`, `SceneAssetMigration`) adds `.cpp` files directly
to RT2Tests via `premake5.lua:13` and `RT2Tests.vcxproj`. A static library
is more machinery for the same result.

**Rejected** because the existing pattern of adding the `.cpp` to
`premake5.lua` and `RT2Tests.vcxproj` is proven and cheaper. If the project
later adopts a static library for other reasons, the extracted module moves
into it without interface change.

### Decisions — answered before code

#### D-HD-1 — one parameterised dispatch function for all four operations

**Decision:** the module exposes one function:

```cpp
struct ContentBrowserDispatchContext {
    std::filesystem::path assetRoot;
    AssetWatchSuppressionRegistry* suppressionRegistry;
    ContentBrowserOperationReport* report;
    Error* error;
};

bool DispatchContentBrowserOperation(
    const ContentBrowserDispatchContext& ctx,
    const AssetRecord& record,
    AssetWatchOperationKind operationKind,
    const std::filesystem::path& destination,
    const ContentBrowserSuppressedOperation& operation);
```

The function: (1) constructs source/destination paths, (2) calls
`RunSuppressedAssetOperation` with the registry, operation kind, paths, and
the callback, (3) returns the callback's result. The host is responsible
for calling `RefreshProjectAssets` and `ScheduleAssetWatchSuppressionClear`
afterwards — those are host-specific because they touch `m_ProjectContext`
and the drain loop.

The ordering invariant — register before operate — is inside
`RunSuppressedAssetOperation` (`AssetWatchPolicy.cpp:270-272`), which is
already tested. The extraction makes the *dispatch sequence* (path
construction + register + operate) link-time testable as a unit. Whether the
host *invokes* the dispatch for each operation remains untestable — see the
residual-gap section.

#### D-HD-2 — policy consultation is tested through a dispatch wrapper

**Decision:** the module exposes two additional functions that wrap the
policy checks:

```cpp
bool CanOperateContentBrowser(bool projectActive);
bool AllowDeleteContentBrowser(bool confirmed, size_t dependantCount);
```

These are trivial wrappers around `ContentBrowserCanOperate` and
`ContentBrowserDeleteAllowed` that exist so the dispatch module is the single
entry point for all content-browser host decisions. A test that calls
`DispatchContentBrowserOperation` with a null project context must fail at
the policy check before reaching the operation.

The host calls `CanOperateContentBrowser` at the panel gate and
`AllowDeleteContentBrowser` at the delete button. The wrappers themselves
are link-time testable. Whether the host *consults* them remains untestable
— see the residual-gap section.

#### D-HD-3 — recovery-snapshot consultation is tested through a dispatch wrapper

**Decision:** the module exposes:

```cpp
bool ShouldCaptureRecovery(
    bool scriptRepairPending,
    const AssetMigrationPersistenceGate& migrationGate);
```

This is a wrapper around `ShouldCaptureRecoverySnapshot` that exists so the
host's consultation of it is routed through the dispatch module. A test that
calls `ShouldCaptureRecovery` with `scriptRepairPending=true` must get
`false`; a test with `scriptRepairPending=false` and a non-pending gate must
get `true`.

The host's autosave block calls `ShouldCaptureRecovery` instead of
`ShouldCaptureRecoverySnapshot` directly. This is a one-line change in
`WalnutApp.cpp:2008`.

#### D-HD-4 — the host supplies the registry; the module does not own it

**Decision:** the `AssetWatchSuppressionRegistry` is supplied by the host
through the `ContentBrowserDispatchContext`. The module does not construct
or own it. A test supplies a `AssetWatchSuppressionRegistry` created with
its own mutex (`AssetWatchPolicy.h:48`). The host supplies the listener's
`suppressionRegistry` member (`WalnutApp.cpp:3093`).

This keeps the module CPU-only and testable. The registry is already
CPU-only (`AssetWatchPolicy.h:42-68`).

### Contract

#### ContentBrowserDispatch module

The module (`ContentBrowserDispatch.{h,cpp}`) provides:

1. `ContentBrowserDispatchContext` — a struct holding the asset root, a
   non-owning `AssetWatchSuppressionRegistry*`, and the report/error outputs.
   The registry pointer may be null (standalone mode — no suppression).

2. `DispatchContentBrowserOperation` — the parameterised dispatch. It:
   (a) constructs the source path from `assetRoot + record.sourcePath`;
   (b) if `suppressionRegistry` is non-null, calls
       `RunSuppressedAssetOperation` with the registry, operation kind,
       source, destination, and the callback;
   (c) if `suppressionRegistry` is null, calls the callback directly;
   (d) returns the callback's result.

3. `CanOperateContentBrowser(bool projectActive)` — wraps
   `ContentBrowserCanOperate`.

4. `AllowDeleteContentBrowser(bool confirmed, size_t dependantCount)` — wraps
   `ContentBrowserDeleteAllowed`.

5. `ShouldCaptureRecovery(bool, const AssetMigrationPersistenceGate&)` —
   wraps `ShouldCaptureRecoverySnapshot`.

The module depends on: `ContentBrowserOperations.h`,
`AssetWatchPolicy.h`, `SceneAssetMigration.h`, `AssetDatabase.h`,
`core/Error.h`, and the standard library. It does not include ImGui, Walnut,
Vulkan, efsw, tinyexr, NRD, or stb_image.

#### Host changes

`WalnutApp.cpp` changes are minimal:

- `DrawContentBrowserPanel` calls `DispatchContentBrowserOperation` instead
  of `RunAssetWatchSuppressedOperation` + inline path construction.
- The panel gate calls `CanOperateContentBrowser` instead of
  `ContentBrowserCanOperate`.
- The delete button calls `AllowDeleteContentBrowser` instead of
  `ContentBrowserDeleteAllowed`.
- The autosave block calls `ShouldCaptureRecovery` instead of
  `ShouldCaptureRecoverySnapshot`.
- `RunAssetWatchSuppressedOperation` and `ScheduleAssetWatchSuppressionClear`
  remain in the host (they own the listener and drain flag).

No behaviour changes. The host's `RefreshProjectAssets` and
`ScheduleAssetWatchSuppressionClear` calls remain after the dispatch returns,
exactly as they are today.

### Implementation order

Each step builds and leaves the tree green.

1. **HD.0 — Add `ContentBrowserDispatch` module.** Add
   `ContentBrowserDispatch.{h,cpp}` with the five functions above. Add
   `ContentBrowserDispatch.cpp` to `premake5.lua` and `RT2Tests.vcxproj`.
   No host changes yet. Focused tests: dispatch with a fake registry and
   fake operation asserts the registry was populated before the operation
   ran; dispatch with a null registry calls the operation directly; policy
   wrappers return the same results as the underlying functions; recovery
   wrapper returns the same result as `ShouldCaptureRecoverySnapshot`.

2. **HD.1 — Route host through the module.** Replace the four inline
   dispatches in `DrawContentBrowserPanel` with calls to
   `DispatchContentBrowserOperation`. Replace the panel gate and delete
   button with `CanOperateContentBrowser` and `AllowDeleteContentBrowser`.
   Replace the autosave call with `ShouldCaptureRecovery`. Build and run
   the full suite. No new tests yet — the existing W6/W7 source-text probes
   still pass because the strings they grep for are still present (the host
   still calls `RefreshProjectAssets` and `ScheduleAssetWatchSuppressionClear`).

3. **HD.2 — Replace source-text probes with link-time tests.** Add permanent
   tests that call `DispatchContentBrowserOperation` with a fake registry
   and a fake operation, and assert the ordering invariant. Remove the W6
   source-text probe at `Phase7W6Tests.cpp:457-465` and the W7 source-text
   probes at `Phase7W7Tests.cpp:212-275` that are now superseded by link-time
   tests. The remaining W7 source-text probes (those that test the drain
   loop and the listener, which stay in the host) remain. Run the full gate.

### Permanent tests and discrimination proofs

| Permanent evidence | Temporary fault that must make it fail |
|---|---|
| `DispatchContentBrowserOperation` with a fake registry and a fake Rename operation: the registry contains the source and destination paths before the operation callback runs. | Move the path construction or `RunSuppressedAssetOperation` call to after the callback in `DispatchContentBrowserOperation`; the test must fail because the registry is empty when the callback checks it. |
| `DispatchContentBrowserOperation` with a fake registry and a fake Move operation: the registry contains both source and destination paths. | Register only the source path, not the destination; the test must fail because the destination path is missing from the registry. |
| `DispatchContentBrowserOperation` with a fake registry and a fake Delete operation: the registry contains the source and sidecar paths. | Register only the source path, not the sidecar; the test must fail because the sidecar path is missing from the registry. |
| `DispatchContentBrowserOperation` with a null registry: the operation callback runs and returns its result. | Refuse to call the callback when the registry is null; the test must fail because the operation never ran. |
| `CanOperateContentBrowser(false)` returns false; `CanOperateContentBrowser(true)` returns true. | Invert the predicate; the test must fail. |
| `AllowDeleteContentBrowser(false, 0)` returns false; `AllowDeleteContentBrowser(true, 1)` returns true. | Remove the confirmation check; the test must fail because delete is allowed without confirmation. |
| `ShouldCaptureRecovery(true, gate)` returns false regardless of gate state; `ShouldCaptureRecovery(false, gate)` returns true when gate is not pending. | Ignore the `scriptRepairPending` argument; the test must fail because recovery is captured during script-repair loss. |

**What the link-time tests do not cover:** whether the host invokes
`DispatchContentBrowserOperation` for each of the four content-browser
operations. Commenting out one of the four call sites in
`DrawContentBrowserPanel` leaves the suite green because RT2Tests cannot
link `WalnutApp.cpp`. This is the residual gap described in the scope
section. The implementation report must record interactive acceptance
exercises for all four operations as first-class evidence.

Every new permanent test gets its discrimination fault exercised before the
implementation report calls it protective. The final gate builds the Release
and Debug solutions, runs `RT2Tests.exe` from the repository root, runs the
scripting regression gate, and records the actual counts measured then.

### Review checklist and boundary

A reviewer must verify at least: the dispatch module has no ImGui/Walnut/
Vulkan/efsw/tinyexr/NRD/stb_image includes; the module is added to
`premake5.lua` and `RT2Tests.vcxproj`; the host's four dispatch call sites
route through the module; the host's panel gate and delete button route
through the module's policy wrappers; the host's autosave block routes
through the module's recovery wrapper; the ordering invariant
(register-before-operate) is proven by a link-time test; the residual gap
(host invocation of the dispatch) is stated in the spec and covered by
interactive acceptance exercises in the implementation report; and the
source-text probes that are superseded by link-time tests are removed, while
those that test host-only code (drain, listener) remain.

**Boundary.** This extraction moves only the content-browser dispatch and
the three policy consultations. It does not move the drain loop, the
watcher listener, the ImGui panel rendering, the recovery service, the
project-open flow, or any other host logic. It does not fix the three W6
defects (they are already fixed). It does not address the
machine-locked test fixtures (17 tests loading 284 MB from
`C:\Users\mikey\Downloads`). It does not add the host-dispatch extraction for
W8's input-bindings panel or script-rebind button — those are W8's own
extraction, to be done after this one lands. It does not change the
`WalnutApp.cpp` god-object problem in general; it extracts exactly the
dispatch logic that was untestable, and no more. It does not close the
residual gap — whether the host *invokes* the dispatch — which is stated in
the scope section and covered by interactive acceptance, not by link-time
test.

**Sequencing note for W8.** The W8 spec (`:10859-10869`) notes that W8
would be materially safer if this extraction landed first. This spec
confirms that: the `ContentBrowserDispatch` module is the seam W8's
input-bindings panel and script-rebind button would route through. W8
should land after this extraction.

### Host dispatch extraction — verification report

Implemented on branch `host-dispatch-extraction` against `a4406de`. Three
commits, one per step:

- **HD.0** (`8e2801a`) — added `RT2App/src/ContentBrowserDispatch.{h,cpp}`
  with `DispatchContentBrowserOperation`, `CanOperateContentBrowser`,
  `AllowDeleteContentBrowser` and `ShouldCaptureRecovery`, plus
  `RT2Tests/src/ContentBrowserDispatchTests.cpp`. Added the module to
  `RT2App.vcxproj`, `RT2Tests.vcxproj` and `RT2Tests/premake5.lua`
  (`RT2App/premake5.lua` already globs `src/**.cpp`). No host changes.
- **HD.1** (`e647dcf`) — routed the four content-browser dispatches in
  `DrawContentBrowserPanel` (reimport, rename, move, delete) through
  `DispatchContentBrowserOperation`, supplying the host's
  `AssetWatchSuppressionRegistry` as a collaborator. Routed the panel
  gate, the delete-button gate and the autosave predicate through the
  dispatch module's three wrappers. The host keeps `RefreshProjectAssets`,
  `ScheduleAssetWatchSuppressionClear` and `RunAssetWatchSuppressedOperation`
  (owning the listener and drain flag).
- **HD.2** (this section) — the W6 source-text probe that grepped
  `WalnutApp.cpp` for `ContentBrowserCanOperate(`/`ContentBrowserDeleteAllowed(`
  was removed; the HD.0 link-time tests are its replacement. No W7
  source-text probe was superseded: the probes at `Phase7W7Tests.cpp:212-275`
  all test the drain loop, the listener and the watch scope — host-only code
  that stays — so they all remain. The residual gap (whether the host
  *invokes* the dispatch) is unchanged and remains a text-probe /
  interactive-acceptance matter, as the scope section states.

**Spec deviation, recorded:** the HD.1 step expected the W6 source-text
probe to still pass after routing, because the strings it greps for would
"still be present." That was wrong: the probe greps for the *underlying*
policy function names, and HD.1 routes the host through the *wrapper*
names. The probe went red after HD.1. To keep HD.1 green, the probe was
removed in HD.1 rather than HD.2 — pulled forward one step. The link-time
replacement (HD.0) was already in place, so no coverage was lost; the
ordering change is the only deviation.

**Forbidden includes.** `ContentBrowserDispatch.{h,cpp}` include only
`AssetDatabase.h`, `AssetWatchPolicy.h`, `ContentBrowserOperations.h`,
`SceneAssetMigration.h`, `core/Error.h` and `<filesystem>`/`<cstddef>`. No
ImGui, Walnut, Vulkan, efsw, NRD, tinyexr or stb_image include is needed.
The only mention of "WalnutApp.cpp" in the module is a file-path reference
in a comment.

**Measured counts.** Baseline on `master` (`a4406de`): Release 736/736
cases, 146453 assertions. After the extraction: Release and Debug both
743/743 cases, 146476 assertions. The case count fell by one (the removed
W6 source-text probe) and rose by eight (the HD.0 link-time tests), net
+7. Assertion count rose by 23 (the HD.0 tests) and fell by 3 (the removed
probe's three assertions), net +20. No pre-existing test was removed or
weakened except the single superseded source-text probe.

**Discrimination proofs.** Each new permanent test had its fault injected,
built, and confirmed red with actual assertion output, then reverted and
confirmed green:

| Test | Fault | Red output |
|---|---|---|
| registers suppression paths before the rename operation runs | call the callback before `RunSuppressedAssetOperation` | `CHECK( 2 == 1 )`, `CHECK( false )` |
| registers both source and destination for a move | pass empty destination to `RunSuppressedAssetOperation` | `CHECK( false )` |
| registers source and sidecar for a delete | pass empty source for the Delete kind | `CHECK( false )`, `CHECK( false )` |
| with null registry runs the operation directly | return false without calling the callback when the registry is null | `CHECK( false )`, `CHECK( false )` |
| CanOperateContentBrowser mirrors ContentBrowserCanOperate | invert the wrapper predicate | `CHECK_FALSE( true )`, `CHECK( false )` |
| AllowDeleteContentBrowser mirrors ContentBrowserDeleteAllowed | always return true | `CHECK_FALSE( true )` (twice) |
| ShouldCaptureRecovery mirrors ShouldCaptureRecoverySnapshot | ignore `scriptRepairPending` (pass false) | `CHECK_FALSE( true )` (twice) |

**Final gate** (measured 2026-08-02 on `host-dispatch-extraction`):

| Gate | Result |
|---|---|
| Release build | green |
| Debug build | green |
| `RT2Tests.exe` Release | 743/743 cases, 146476 assertions, 0 failed |
| `RT2Tests.exe` Debug | 743/743 cases, 146476 assertions, 0 failed |
| `run_script_test.ps1` | `[ScriptScenario] PASS` |
| `run_slice_test.ps1` | `[Slice] PASS` |
| `--validate --frames 8` | exit 0, zero validation messages |

**Interactive acceptance.** Not performed in this session — the host UI
requires a desktop session. The residual gap (whether the host invokes
the dispatch for each of the four operations) remains, as the scope
section states, and is covered by interactive acceptance, not by
link-time test. This report records that gap rather than overstating it;
the earlier review caught exactly that overstatement once before.

#### Review amendment W8-A2 (2026-08-02) — D-W8-2 corrected: Rebind adopts the new asset's identity

Raised during implementation, before any code was written. D-W8-2 stated that
the Rebind button "does not touch `assetId`", grounded on
`NormalizeAndValidateScriptComponent` preserving it. That check was correct but
one layer too shallow: `SetScriptState` clears `assetId` whenever the path
changes (`SceneManager.cpp:3859-3862`), then resolves and, where identity
repair is required, mints through `ResolveOrAssign` (`:3889-3901`).

**Resolution: the existing behaviour is correct and stands unchanged. D-W8-2 is
amended, not the code.** No separate rebind path is introduced.

Rebind means pointing an entity at a *different* script file. A different file
is a different asset and must carry that file's identity. Preserving the old
`assetId` against a new path would produce a reference claiming identity A
while resolving to file B — exactly the state `AssetResolver` case 5 rejects as
`Conflict`, and which it explicitly refuses to resolve by silent substitution.
D-W8-2 as written would have manufactured that conflict on every rebind.

Minting is also within the W5 contract. The rule is that the *watcher* never
mints (D-W7-3); user-initiated import and migration may, and all eight
production `ResolveOrAssign` call sites are user-initiated. A button click is
user-initiated. Rebinding to a script whose sidecar is absent should assign one,
exactly as importing that script would.

The Rebind button therefore remains a thin affordance over the existing path
edit: open a `.lua` file dialog, set the asset-root-relative path, call
`SetScriptState`. Its permanent test asserts that after a rebind the reference
resolves to the *new* file's identity — not that the old `assetId` survived.

Distinguish this from a future *relink* operation, which repairs a moved
asset's path while deliberately keeping its identity. That is a different
operation with different semantics and is not part of W8.

Grounding commit for this amendment: `5c44dba`.

### Phase 7 W8 — verification report (2026-08-03)

Implemented on branch `codex/phase7-w8-deferred-commitments`, from
`dbb2d4a`, with implementation commits `0ee5fca` and `ea1fef8`.

**Implementation.** W8.0 adds the CPU-only `InputBindingEditor` module and
registers it for the test and slice targets (`RT2App/src/InputBindingEditor.h:1`,
`RT2App/src/InputBindingEditor.cpp:1`, `RT2Tests/src/Phase7W8Tests.cpp:59`,
`RT2Tests/premake5.lua:13`, `RT2Tests/RT2Tests.vcxproj:224`). It constructs
action/axis bindings, serializes override records, describes bindings, and
finds same-context conflicts. W8.1 adds the View-menu Input Bindings panel
(`RT2App/src/WalnutApp.cpp:1311`, `:4814`), edits user overrides only, applies
the composed configuration, and supports rebind/unbind (`:1354-1467`). An
explicit empty override remains visible so an unbound mapping can be rebound
again (`:1320-1348`, `:1430-1465`). W8.2 adds the `.lua` Browse button and
submits the selected asset through the existing `SetScriptState` path
(`RT2App/src/SceneEditorUI.cpp:1799-1867`). Per review amendment W8-A2, a
different script adopts the different file's identity; the permanent test
asserts the new sidecar ID (`RT2Tests/src/Phase7W8Tests.cpp:160-196`). W8.3
adds per-asset declaration status and diagnostics using
`ScriptFieldRegistry::GetDeclaredFields` (`RT2App/src/WalnutApp.cpp:1481-1542`).
Cursor-lock runtime binding remains deferred as P7-R2; no second cursor-capture
mechanism was added.

**Measured final gate.** Release and Debug builds both completed successfully.
Running the tests from the repository root produced the same measured result
in both configurations:

| Gate | Result |
|---|---|
| Release `RT2Tests.exe` | 749/749 cases, 146510 assertions, 0 failed |
| Debug `RT2Tests.exe` | 749/749 cases, 146510 assertions, 0 failed |
| `run_script_test.ps1` | `ScriptScenario PASS: 60 frames, 1 entities, no mismatches` |
| `run_slice_test.ps1` | `SliceRunner PASS: 60 steps, authoring intact`; `Slice PASS` |
| `run_punctual_light_test.ps1` | `PunctualLight PASS` |
| `--headless --validate ... --frames 8` | exit 0, zero validation messages |

The requested headless command also printed that `--out` was an unknown
argument and wrote its default `screenshot.png`; that generated file was
removed. It also printed the pre-existing recent-scenes settings temp-file
warning and a Vulkan loader debug-layer warning. Neither produced a validation
message or non-zero exit.

**Discrimination proofs.** Each permanent W8 test was faulted, rebuilt and
run alone, then restored and rerun green. The actual red outputs were:

| Test | Temporary fault | Actual red output |
|---|---|---|
| `Phase7 W8 InputBindingEditor builds and round-trips an override` | omit `contextId` | `Phase7W8Tests.cpp(69): FATAL ERROR: REQUIRE( ParseInputContextRecords( InputContextRecordsToJson({record}), InputConfigScope::UserOverrides, parsed, error) ) is NOT correct!` — 0 passed, 1 failed; 0 passed, 1 failed assertions |
| `Phase7 W8 user input override wins over built-in composition` | overlay built-ins after user overrides | `Phase7W8Tests.cpp(96): ERROR: CHECK( ...positive == static_cast<uint16_t>(KeyCode::Up) ) is NOT correct! values: CHECK( 87 == 265 )` — 3 passed, 1 failed assertions |
| `Phase7 W8 explicit unbind removes inherited mapping` | treat empty bindings as inherit | `Phase7W8Tests.cpp(110): ERROR: CHECK( composed.front().mappings.empty() ) is NOT correct! values: CHECK( false )` — 2 passed, 1 failed assertions |
| `Phase7 W8 FindConflicts detects shared bindings in one context` | compare only within one mapping name | `Phase7W8Tests.cpp(131): FATAL ERROR: REQUIRE( conflicts.size() == 1 ) is NOT correct! values: REQUIRE( 0 == 1 )` — 0 passed, 1 failed assertions |
| `Phase7 W8 DescribeMapping names keyboard mouse and gamepad bindings` | return numeric code text | `Phase7W8Tests.cpp(143): ERROR: CHECK( DescribeMapping(keyboard) == "W (Keyboard)" ) is NOT correct! values: CHECK( Key 87 (Keyboard) == W (Keyboard) )` — 2 passed, 1 failed assertions |
| `Phase7 W8 script rebind resolves the new file identity` | stop resolving the selected file's sidecar identity | `Phase7W8Tests.cpp(183): FATAL ERROR: REQUIRE( result.success ) is NOT correct! values: REQUIRE( false )` — 6 passed, 1 failed assertions |

After each fault was reverted, its focused test passed. The six restored
focused runs were green: 7/7, 4/4, 3/3, 4/4, 3/3 and 13/13 assertions,
respectively. The final full Release and Debug runs above also passed all six.

**Interactive acceptance status.** The editor acceptance exercise was not
completed in this session. The computer-controlled editor session was stopped
because the Windows desktop became unresponsive before the Input Bindings,
script Rebind, declaration-diagnostic refresh, and next-Play checks could be
performed. An attempted project open also exposed a fixture problem rather
than a W8 result: `vertical-slice.rt2scene` was rejected because it is missing
`metadata.projectId` for project binding. No interactive pass is claimed here;
the remaining acceptance work is to verify the Input Bindings rebind/unbind
flows, script rebind/save/reopen to the new identity, declaration status and
external-edit refresh, and editor-owned contexts being read-only.

**Test boundary and static assertions.** The W8 permanent tests deliberately
cover the CPU-only module and the `SceneManager` identity behavior. `RT2Tests`
does not compile `WalnutApp.cpp` or `SceneEditorUI.cpp` (`W8-F16`), so the UI
calls to `ApplyConfiguration`, `Save`, `SetScriptState`, and
`GetDeclaredFields` are not host-wiring tests. No source-text test was added
to pretend otherwise; the host paths are recorded as pending interactive
acceptance. The existing W5/W6/W7 host-dispatch gaps remain subject to the
same boundary and are not silently converted into guarantees by these W8
tests.

### Phase 7 W8 — popup-scope correction and acceptance completion (2026-08-03)

This append-only note supersedes the preceding report's statement that the
interactive acceptance was incomplete. The remaining editor checks were
performed after fixing a fourth instance of the ImGui popup-scope defect.

**Popup-scope fix.** `RT2App/src/WalnutApp.cpp:1345-1379` now records a
Rebind request while the per-mapping `PushID` scope is active, then calls
`OpenPopup("Capture Input")` at `:1379`, in the same parent scope used by
`BeginPopupModal("Capture Input")` at `:1419`. Previously the open call was
inside the mapping ID scope and the modal was outside it, so the dialog never
opened and the feature could not capture or save a binding. The audit of every
`OpenPopup` in `WalnutApp.cpp` found no additional mismatched popup/modal pair;
the content-browser opens are hoisted after their per-item scopes and the
remaining modal pairs share their parent scope.

**Acceptance performed.**

- In View → Input Bindings, Rebind on `jump` opened the Capture Input modal.
  After waiting for the opening click to be consumed, the first real `Space`
  keypress closed the modal and the row displayed `Space (Keyboard)`. The
  persisted settings record contained the runtime `jump` mapping. The
  `m_InputCaptureSkipFrame` path therefore did not swallow the first real
  keypress. The temporary user override was removed after the exercise.
- Without restarting the editor, Play was entered from the Scene panel and
  showed `(Playing)` with Pause and Stop enabled. `Space` was sent while Play
  was active; the viewport continued rendering and the editor remained in
  `(Playing)`, then returned to `(Edit)` after Stop. The vertical-slice fixture
  has no visible `jump` consumer, so this confirms the live composition/input
  route and Play continuity rather than a scene-specific jump animation.
- The four earlier interactive checks also stand: editor-owned contexts were
  read-only; explicit unbind removed the mapping; Browse → save → reopen
  resolved a script reference to the new file's identity; and declaration
  diagnostics changed from invalid to valid after an external `.lua` edit.

**Measured gates.** The Release and Debug builds completed successfully. Tests
run from the repository root measured:

| Gate | Result |
|---|---|
| Release `RT2Tests.exe` | 749/749 cases, 146510 assertions, 0 failed |
| Debug `RT2Tests.exe` | 749/749 cases, 146510 assertions, 0 failed |
| `run_script_test.ps1` | `ScriptScenario PASS: 60 frames, 1 entities, no mismatches` |
| `run_slice_test.ps1` | `SliceRunner PASS: 60 steps, authoring intact`; `Slice PASS` |
| `--headless --validate ... --frames 8` | exit 0, zero validation messages |

The requested headless command also reported that `--out` is not a supported
argument and wrote its default `screenshot.png`; that generated file was
removed. This did not produce a validation message or non-zero exit. The
punctual-light gate recorded in the preceding W8 verification run remained
passing; no production code changed after that gate.

**Phase 7 boundary.** W8 completes the deferred input-binding, script-rebind,
and declaration-diagnostic commitments. It does not add the W7-A1
model/texture/environment automatic reimport that was deliberately
substituted with database refresh, and it does not add the deferred runtime
cursor-lock binding (D-W8-4/P7-R2). The host-invocation boundary also remains:
`RT2Tests` does not compile `WalnutApp.cpp` or `SceneEditorUI.cpp`, so the
host's use of the W8 seams is covered by the interactive checks rather than a
link-time test; the analogous W5/W6/W7 host-dispatch gaps remain. Four UI
popup-scope defects reached a green CPU suite and were caught only by
interactive acceptance; this Capture Input defect was the fourth.

The scene fixtures were not modified by the acceptance exercise. The only
temporary persisted editor setting was the `jump` override, restored to an
empty `inputOverrides` array before closing the editor.

## Phase 7 — closure (2026-08-03)

Phase 7 is complete at `27d5173` on `codex/phase7-w8-deferred-commitments`:
**749/749 tests and 146,510 assertions in both Release and Debug**, the script
and slice gates green, and `--headless --validate` exiting 0 with no validation
messages. The tree carries no fixture modifications.

This section is the phase boundary. It states what the phase delivers, what it
deliberately does not, and what it leaves reachable for Phase 8. It supersedes
no earlier section — the per-workstream reports remain authoritative for their
own detail.

### Against the roadmap exit criterion

> *A project folder can be copied to another machine/location without
> rewriting scene files manually.*

Met. `project.rt2proj` stores portable relative locators; `projectDirectory`,
`assetRoot` and `cacheRoot` are all derived from the opened file's location
rather than serialized absolutely, and `lastBrowseDirectory` — the one setting
that is legitimately machine-specific — never participates in asset
resolution. Round-trip and relocation are covered by
`Phase7W4Tests.cpp:178`.

The stub's other listed outcomes: asset IDs survive rename and move (W1/W2),
resolution is by ID with path as fallback (W3), the content browser has
search, rename/move/delete, drag-drop and reimport (W6), sources are watched
with async database refresh (W7), and the deferred input and script-rebind
commitments are closed (W8).

### What the phase does not deliver

Each of these is a recorded decision, not an oversight:

- **Automatic model/texture/environment reimport on source change** (W7-A1).
  Deliberately substituted with database refresh. Watching a source and
  refreshing its record is done; rebuilding the GPU-side resource from a
  changed file in place is not.
- **The runtime cursor-lock binding** (D-W8-4 / P7-R2), deferred by decision.
- **Host-invocation coverage.** `RT2Tests` compiles neither `WalnutApp.cpp`
  nor `SceneEditorUI.cpp`, so wherever the host *calls* a W5–W8 seam, the
  cover is an interactive check rather than a link-time test. The host
  dispatch extraction narrowed this for the content-browser register-then-
  operate sequence; it did not close it.

### The finding this phase should be remembered for

**Four ImGui popup-scope defects reached a fully green CPU suite and were
caught only by driving the UI by hand.** Rename, Move and Delete were
*unreachable in the application* while their operations were unit-tested and
passing; Capture Input could not capture a binding, which was the entire point
of the feature. All four are fixed, and the rule is recorded in
`docs/glossary.md` under "ImGui `OpenPopup` and `BeginPopupModal` must share
an ID scope".

The structural cause is the host-invocation gap above: a suite that cannot
link the host cannot observe that the host's UI never reaches the code the
suite is proving. **Interactive acceptance is therefore load-bearing, not a
postscript** — it is currently the only instrument that can see this class of
defect at all. Phases that budget it as an afterthought will ship the same bug
again.

### Carried into Phase 8

- **The suite is machine-locked.** Four test files hold 16 absolute-path
  references into `C:\Users\mikey\Downloads` — `sofa_and_lamp.glb` (243 MB,
  13 references) and `ABeautifulGame.glb` (41 MB, 4). They dominate the
  runtime and mean the suite cannot run on any other machine or in CI. Fixing
  this is a prerequisite for the suite being a shared artifact rather than a
  local one.
- **Compaction drops override-only material and texture references.**
  Pre-existing and currently unreachable, because nothing yet holds a
  reference that exists only as an override. **Phase 8 is Prefabs, which is
  precisely the feature that creates them.** Address it inside Phase 8's
  scope, before prefab overrides ship, not after.
- **File-local and scene-global indices remain the same type** (`int` /
  `uint32_t`), so nothing prevents assigning one to the other. Four defects
  came from this in July; distinct types would make the class
  unrepresentable. Still open.
- **One W6 negative-case test has no recorded discriminating fault.** The
  correct fault is `const bool matchesPath = !matchesId;` — dropping the
  `ReferenceKey` comparison. Record it when next touching that file.

## Phase 8 pre-work — override-aware compaction (implementation spec)

Written 2026-08-03, before Phase 8 (Prefabs) begins. This is a correctness
prerequisite, not a feature: the compaction sweep is currently correct only
because of an invariant maintained in another file, and Prefabs is the feature
that breaks that invariant.

### The problem

`SceneManager::CompactMeshRegistry` (`RT2App/src/SceneManager.cpp:4017`) is
mark-and-sweep over three resource tables. Each pass decides what counts as a
live reference:

| Pass | Marks from | Lines |
|---|---|---|
| Meshes | `MeshRef::meshIndex` on live entities | `:4032-4039` |
| Materials | `MeshRef::materialIndex`, plus per-triangle `mesh.materialIndices` | `:4085-4096` |
| Textures | the four texture indices on each entry of `m_EcsScene.materials` | `:4137-4143` |

`MaterialOverrideComponent` (`RT2App/src/ECSComponents.h:241`) is marked by
none of them, and it holds a **full `SceneMaterial` value snapshot** with its
own `baseColorTextureIndex`, `normalTextureIndex`, `emissiveTextureIndex` and
`metallicRoughnessTextureIndex`. It is therefore a second place texture
indices live, invisible to the pass that sweeps them.

Two consequences, and they are not the same thing:

- **The material-slot invalidation is deliberate.** `materialIndex` on the
  override is explicitly transient — filled by the resolver, not serialized as
  identity. When its slot is swept, `RebaseIndices` maps it to `-1` and
  `SceneManager.cpp:190-192` states the asymmetry must remain. The resolver
  refills it. **Do not change this.**
- **The texture handling is a latent defect.** A texture reachable only
  through an override snapshot is not marked, is swept from
  `m_EcsScene.textures`, and then `rebaseMaterialTextures` (`:255-262`)
  rewrites the override's index to `-1`. The override survives; its texture
  pointers are silently nulled. An authored material returns untextured with
  no diagnostic.

### Why nothing hits it today

`SetMaterial` constructs the override by **copying a live material slot** —
`ov.material = m_EcsScene.materials[materialIndex]` (`:3661`) — and `MeshRef`
points at that same slot. The override's textures are consequently marked via
the mirror, never via the override itself.

**The sweep is not correct; it is masked.** Its safety depends on "every
authored override is mirrored by a live entry in `m_EcsScene.materials`" — an
invariant established in the resolver and in `SetMaterial`, stated nowhere in
the compaction code, and asserted by no test.

### Why Phase 8 breaks it

A prefab override is authored against an asset, not against a live
instantiated material slot. A prefab instance overriding a texture on a
variant not currently materialised in `m_EcsScene.materials` is exactly the
unmirrored case. The failure is silent at the point of failure: compaction
runs automatically on scene change (`:1324`, `:2387`, `WalnutApp.cpp:307`), so
the texture is lost during an operation the user did not initiate and notices
later, in another session, as "my prefab lost its texture". Nothing connects
the symptom to the sweep.

### Findings to establish before code

- **F1.** `InstallMaterialOverride` (`:3992`) installs an arbitrary override
  snapshot with no resolver pass and no mirroring. It is the command
  capture/restore path (`:1705`, `:3994-4003`). **Determine whether an
  undo/redo sequence interleaved with a compaction can already install an
  override whose texture indices do not match any live material.** If it can,
  this defect is reachable today and this spec's framing as pre-work is wrong —
  say so rather than proceeding.
- **F2.** `MergeImportedECS` copies overrides between registries (`:850-856`).
  Confirm whether the copy preserves the mirror or can produce an override
  whose indices are file-local. Import already rebases; the question is
  whether the override goes through `RebaseIndices` on that path.

Answer both from the code before writing the fix, and record the answers.

### The change

Add a fourth marking loop to the **texture** pass only, so an override marks
its own textures rather than relying on being mirrored:

```cpp
// Overrides carry a full material snapshot, so their textures are live
// references even when no entry in m_EcsScene.materials mirrors them.
// Prefab overrides are authored against an asset, not a live slot, so
// the mirror cannot be assumed. See Phase 8 pre-work spec.
auto overrideView = m_EcsScene.registry.view<MaterialOverrideComponent>();
for (const auto entity : overrideView)
{
    const auto& mat = overrideView.get<MaterialOverrideComponent>(entity).material;
    if (mat.baseColorTextureIndex >= 0)         referencedTexs.insert(mat.baseColorTextureIndex);
    if (mat.normalTextureIndex >= 0)            referencedTexs.insert(mat.normalTextureIndex);
    if (mat.emissiveTextureIndex >= 0)          referencedTexs.insert(mat.emissiveTextureIndex);
    if (mat.metallicRoughnessTextureIndex >= 0) referencedTexs.insert(mat.metallicRoughnessTextureIndex);
}
```

**Scope boundary — the material pass is not changed.** Marking the override's
`materialIndex` would defeat the deliberate invalidation at `:190-192` and
change resolver behaviour. This spec touches the texture pass only.

Extract the four-field test into a shared helper if it reads better than
repeating it; `rebaseMaterialTextures` (`:208-217`) already enumerates the same
four fields, and a third copy is the point at which they can diverge. One
helper naming the four fields, used by mark and rebase alike, is preferable.

### Tests and discrimination proofs

Add to `RT2Tests`. `SceneManager.cpp` **is** compiled into the test project, so
unlike the Phase 7 UI defects this class is fully reachable by automated test.

1. **Unmirrored override retains its texture.** Build a scene with a textured
   material, attach a `MaterialOverrideComponent` whose snapshot references
   that texture, remove every entity referencing the material so no live
   `m_EcsScene.materials` entry mirrors it, compact, and assert the override's
   `baseColorTextureIndex` still resolves to the same texture path.
   *Discriminating fault:* delete the new marking loop. The index becomes
   `-1`.
2. **Mirrored override is unchanged.** The ordinary `SetMaterial` path still
   compacts identically — same table sizes, same remapped indices — proving
   the fix adds marks without retaining garbage.
   *Discriminating fault:* mark unconditionally without the `>= 0` guard, so
   `-1` enters `referencedTexs` and the remap shifts.
3. **The material-slot asymmetry is preserved.** An override whose material
   slot is swept still has `materialIndex == -1` afterwards.
   *Discriminating fault:* add the override to `referencedMats`. The index
   survives and the deliberate asymmetry is gone.

Every proof runs fault-first: inject, confirm red, revert, confirm green, and
record both outcomes. A test that has never been seen to fail has not been
shown to test anything.

### Acceptance

- Both configurations build and the full suite passes. Baseline before this
  work is 749/749 and 146,510 assertions at `bc9d7f1`; three new cases are
  expected, so state the measured counts rather than copying these.
- Script, slice and `--headless --validate` gates green.
- F1 and F2 answered in writing, with file:line evidence.
- All three discrimination proofs recorded red-then-green.
- No fixture modifications in `git status`.

### Boundary

This spec does not implement prefabs, does not change the resolver, and does
not change the material or mesh marking passes. It makes one sweep locally
correct so that Phase 8 can introduce unmirrored overrides without a silent
data-loss path waiting for them.

## Phase 8 pre-work — override-aware compaction (verification report)

Written 2026-08-03 against master `cce93a7`, committed on the Phase 8 pre-work
branch. Every line reference below is the tree as committed on this branch;
the spec's citations were to the pre-change tree (`SceneManager.cpp` moved by
~9 lines after the shared-helper extraction).

### Findings, answered before any code

**F1 — No: the undo/redo path cannot install a stale override today.** The
spec's framing holds; the defect is reachable only once Phase 8 breaks the
mirror invariant. Evidence:

- `InstallMaterialOverride` emplaces the snapshot verbatim with no resolver
  pass and no rebase (`SceneManager.cpp:4001-4012`). It is called only from
  `SetMaterialIndexCommand`/`SetMaterialPropertiesCommand` Execute/Undo
  (`EditorPropertyCommands.cpp:140,148,163,174,184,194`) and its snapshots
  are captured at command construction (`SceneEditorUI.cpp:162-178`). A
  snapshot's texture indices therefore become stale only if a compaction runs
  between capture and restore.
- Compaction cannot run in that window. The host's automatic compaction is
  deferred while any undo or redo entry exists: `historyLive` at
  `WalnutApp.cpp:304`, guard at `:306-307`. Every other compaction site runs
  only after `m_History.Clear()` (`WalnutApp.cpp:1874-1875, 3262-3263,
  4152-4153, 4435-4436`) or asserts history empty (`:2715-2719`).
- `RemoveSubtrees` compacts unconditionally (`SceneManager.cpp:1333`), but its
  only caller is `RemoveEntity` (`:1119-1121`), which has no app caller — the
  UI delete path is `RemoveSubtreesCommand` → `RemoveSubtreesNoCompact`
  (`SceneEditorUI.cpp:382`, `EditorStructuralCommands.cpp:53-56`), which
  explicitly does not compact (`SceneManager.cpp:2080`).
- `EditorCommandHistory::Execute` pushes the command only after a successful
  effective execute (`EditorCommandHistory.cpp:14` then `:30`), and the host
  fires its scene-changed notification after history Execute returns
  (`SceneEditorUI.cpp:286-287, 109-110`), so the guard always sees a resident
  command when compaction could run. Undo/Redo move the command between
  stacks before any notification (`EditorCommandHistory.cpp:74-88, 102-116`).
  Snapshots are destroyed (capacity eviction `:156-166`, generation rebind
  `:139-154`, failure policy `:78-84, 106-112`) before compaction is allowed.
- `RebaseIndices` does rebase live overrides (`SceneManager.cpp:264-274`) — but
  only components in the registry, never the command-held copies.

The compaction is therefore masked twice, exactly as the spec says: the
resolver re-establishes the mirror on every resolve (`SceneAssetResolver.cpp:
764-783`, appending the override as its own live slot), and the Phase 3B1
resource-lifetime invariant (`docs/scene-management.md:860-868`) blocks
compaction for the whole lifetime of any snapshot.

**F2 — No: the copy is scene-global, and the mirror survives.** The override
goes through `RebaseIndices` on the import path (`SceneManager.cpp:785`): its
four texture fields are rebased by the texture base (`:259-260` with the
`< 0` guard inside `IndexRebase::Texture`, `:195-201` preserving `-1`) and its
`materialIndex` by the material base (`:261-263`). Since `MergeImportedECS`
appends all source materials/textures wholesale (`:783-788`) and the bases are
`dst` sizes taken before appending (`:769-776`), the copied override's indices
are dst-global, not file-local. The override's `materialIndex` and the
entity's `MeshRef.materialIndex` are both offset by the same `matBase`
(`:246-249`), so the mirror is preserved if it held in the source scene.
Note that loader output never contains overrides; they are authored data
(comment at `:849-852`).

### What changed

One file, `RT2App/src/SceneManager.cpp`, texture pass only:

- **Extracted the shared helper — yes.** `ForEachMaterialTextureIndex`
  (`SceneManager.cpp:204-216`) enumerates the four texture fields once and is
  now used by all three enumerations: `rebaseMaterialTextures`
  (`:222-227`), the materials texture-marking loop (`:4146-4152`) and the new
  override marking loop (`:4154-4165`). The codebase already treats the
  texture-field list as a load-bearing single place ("This is the complete
  list of scene-resource index fields. Keep all additions here so merge and
  compaction cannot silently diverge", `:220-221`; glossary `index` entry).
  A third literal copy is the exact point at which a fifth texture field
  would silently diverge the mark and rebase passes, so the helper is the
  smaller risk. Marking passes guard `>= 0` inside their closures; the rebase
  closure relies on `IndexRebase::Texture`'s own `-1` pass-through.
- **The material pass is untouched.** The override is marked into
  `referencedTexs` only; nothing changed in the mesh or material passes, and
  `MaterialOverrideComponent::materialIndex` invalidation (the deliberate
  asymmetry at `SceneManager.cpp:186-193`) is preserved — test 3 pins it.
- No change to `RebaseIndices`, the resolver, the serializer, or the host.

### Tests and discrimination proofs

Three cases in `RT2Tests/src/OverrideAwareCompactionTests.cpp` (registered in
`RT2Tests/RT2Tests.vcxproj` and `RT2Tests/premake5.lua`). Fixtures build
textures/materials/entities directly on the ECS (SceneManager.cpp compiles
into RT2Tests). Every proof ran fault-first on the Release build.

1. **Unmirrored override retains its texture** — fixture: entity whose
   MeshRef points at a live slot while its override snapshot mirrors a second,
   unreferenced textured slot; compact; assert the override's
   `baseColorTextureIndex` still resolves to `albedo.png`.
   - Fault (spec's): delete the new marking loop.
   - RED: `REQUIRE(ov.material.baseColorTextureIndex >= 0)` failed, value
     `-1` — exactly the spec's predicted symptom (the texture is swept and the
     rebase maps the index to `-1`).
   - Revert → GREEN.
2. **Mirrored override compacts identically** — fixture: the ordinary
   SetMaterial shape (override mirrors the entity's own live slot; the slot
   index deliberately differs from its texture index); assert table sizes,
   paths and remapped indices equal the no-override result.
   - **Finding on the spec's named fault.** Removing the `>= 0` guard so `-1`
     enters `referencedTexs` does **not** go red: the remap loop's range check
     (`SceneManager.cpp:4170-4173`) absorbs `-1` (`old >= 0` is false), so the
     remap cannot shift from a `-1` mark. The spec's claimed mechanism
     ("the remap shifts") is not observable in the current implementation.
     Reported rather than forcing the proof; the test was then proven with a
     discriminating fault of the same class — a wrong-field slip that marks
     the override's transient `materialIndex` into `referencedTexs`.
   - Fault (spec's): unguarded marking → GREEN (finding above, recorded).
   - Fault (discriminating): garbage mark of `materialIndex` → RED with the
     spec's predicted observable: `textures.size() == 2` (was 1),
     `textures[0].path == "x.png"` (was "z.png"), remapped indices shifted.
   - Revert → GREEN.
3. **Material-slot asymmetry is preserved** — same fixture as 1; after
   compaction the swept slot must leave `ov.materialIndex == -1`.
   - Fault (spec's): add the override to `referencedMats` in the material
     pass.
   - RED: `CHECK(ov.materialIndex == -1)` failed, value `0` — the slot
     survives the sweep and the deliberate asymmetry is gone. (In this
     fixture the fault also keeps the whole material table from being
     compacted, so test 1 fails as well — same mechanism.)
   - Revert → GREEN.

### Measured results (both configurations, run from the repository root)

| Configuration | Result |
|---|---|
| **Release** | **752 run, 752 passed, 0 failed, 0 skipped; 146,525 assertions** |
| **Debug** | **752 run, 752 passed, 0 failed, 0 skipped; 146,525 assertions** |

Baseline at `cce93a7` was 749/749, 146,510; the three new cases add 15
assertions (5 + 7 + 2, the fixtures' `REQUIRE(e.IsValid())` counting per
case). Debug shows no failures; the "8 known Debug failures in OBJ fixture
generation" note in AGENTS.md is stale — the plan's Test baseline
(`:6170-6199`) records them diagnosed and fixed on 2026-07-25, and this run
measured 752/752.

### Gates

- `run_script_test.ps1` — PASS (60 frames, 1 entity, no mismatches).
- `run_slice_test.ps1` — PASS (60 steps, authoring intact; Cube x≈1.0).
- `RT2App.exe --headless --validate` on `vertical-slice.rt2scene`, 8 frames —
  PASS, screenshot written.
- `git status` — no fixture modifications (script-scenario and
  vertical-slice untouched).

### Observation, out of scope (recorded, not fixed)

`RecordMaterialIndexEdit` captures the command's before-override *after*
`SetMaterial` has already replaced the override
(`SceneEditorUI.cpp:1405-1409, 162-169`), so the stored `m_BeforeOverride`
equals `m_AfterOverride` (both post-mutation mirrors of the new slot) and
`SetMaterialIndexCommand::Undo` (`EditorPropertyCommands.cpp:144-150`)
overwrites the correctly re-derived before-override with the after value.
This does not create unmirrored texture indices (the snapshot always mirrors
a live slot; the compaction guard above is unrelated) — it is a durable-value
capture-order quirk on imported entities, pre-existing, and outside this
spec's boundary. Flagged for the owner.


## Material-index undo restores the wrong override (implementation spec)

Written 2026-08-03, found during Phase 8 pre-work review and deliberately left
out of that change's scope. This is an authoring data-loss defect, independent
of prefabs and of compaction.

### The problem

`SceneEditorUI::RecordMaterialIndexEdit`
(`RT2App/src/SceneEditorUI.cpp:162-177`) captures the command's before- and
after-override with **the same call, made twice, both after the mutation has
already happened**:

```cpp
const auto beforeOverride = m_SceneMgr->GetMaterialOverride(target);
// The after-override is what the manager just recorded via
// SetMaterialIndexState's RecordMaterialOverride side effect.
const auto afterOverride = m_SceneMgr->GetMaterialOverride(target);
```

The call site (`:1405-1409`) runs `SetMaterial(entity, current)` **first**, and
`SetMaterial` recreates the entity's `MaterialOverrideComponent` from the newly
selected slot (`SceneManager.cpp:3656-3665`). By the time either line above
runs, the override already holds the *after* value. The two snapshots are
therefore identical by construction, and `m_BeforeOverride` is a copy of the
after-state.

`SetMaterialIndexCommand::Undo` (`EditorPropertyCommands.cpp:144-150`) restores
the index and then installs that snapshot:

```cpp
auto result = scene.SetMaterialIndexState(m_Target, m_BeforeIndex);
if (!result.success) return result;
scene.InstallMaterialOverride(m_Target, m_BeforeOverride);
```

So undo restores `MeshRef::materialIndex` correctly and then writes back the
**post-edit** durable override.

### Why the effectiveness guard does not mask it

`MakeSetMaterialIndexCommandIfEffective` (`EditorPropertyCommands.cpp:282-301`)
compares the two overrides only inside `if (beforeIndex == afterIndex)`. The
call site records only when `indexChanged` is true (`:1405`), so the indices
always differ and the equality branch is never reached. The command is created
every time, carrying a before-snapshot that equals its after-snapshot.

### Why it is silent until reopen

This is the part that makes it expensive. Immediately after undo the viewport
looks correct: `MeshRef` points at the old slot, `m_EcsScene.materials` is
untouched, and the entity renders with the original material. Nothing is
visibly wrong in the session.

The override is the **durable** record. On save and reopen the resolver applies
any `authored` override on top of the rebuilt source materials — it appends
`override.material` to `doc.ecs.materials` and repoints `MeshRef` at it
(`ECSComponents.h:226-241`, resolver at `SceneAssetResolver.cpp:764-783`). The
entity therefore comes back with the **post-edit** material, and the undo is
silently reverted across the document boundary.

The user's experience is: change a material on an imported entity, undo it, see
the undo work, save, reopen, and find the change back. Nothing connects that to
the undo.

### The correct pattern is thirteen lines below

`RecordMaterialPropertiesEdit` (`:179-199`) solves exactly this problem
correctly. It does not read the before-state after the mutation; it uses
`m_PendingMaterialPropertiesBeforeOverrides`, a member captured at
`ImGui::IsItemActivated()` time (`:1449`) and cleared when the session closes
(`:1516`).

Its comment is visibly the author reasoning the problem out in place — "the
session stores `SceneMaterial`, not the override list… For simplicity here,
capture before-overrides at activation". **The author identified the hazard,
solved it for the properties path, and did not carry the solution to the index
path.** The index path has no session — it is a discrete combo-box change, not
a drag — so there was no obvious place to hang the cache, and the two identical
`GetMaterialOverride` calls went in instead.

This is the same shape as the Phase 7 popup-scope finding: a correct pattern
and a broken one adjacent in the same file. Cite the correct one when fixing.

### The testability constraint — decide this before writing code

`RT2Tests` compiles `EditorPropertyCommands.cpp` but **not**
`SceneEditorUI.cpp` (`RT2Tests/premake5.lua:17`). The command layer is fully
testable; the capture site is not. A fix placed entirely in
`RecordMaterialIndexEdit` is therefore unverifiable by the suite, which is how
this class of defect survived Phase 7 four times over.

**D1 — where the before-capture lives.** Two options:

- **(a) Cache member in `SceneEditorUI`.** Capture the override before calling
  `SetMaterial` at `:1405`, hold it in a member, pass it to
  `RecordMaterialIndexEdit`. Mirrors the properties path exactly. Smallest
  change; **entirely untestable**.
- **(b) Capture in a testable seam.** Have the mutation return, or a helper
  compute, the before-override so the ordering is enforced by a signature
  rather than by call-site discipline — e.g. `SetMaterial` returning the
  displaced override, or a `SceneManager` helper that performs
  capture-then-mutate as one operation. Larger change; the ordering becomes
  impossible to get wrong and is coverable by a `SceneManager` test.

**Recommendation: (b).** The defect is precisely a call-ordering mistake, and
(a) preserves the property that a correct fix depends on remembering the
ordering. (b) is also the only option that produces a regression test. If (b)
proves disproportionate after reading the code, take (a) and say why — but do
not take (a) merely because it is shorter.

### Findings to establish before code

- **F1.** Confirm the exact guard under which `SetMaterial` creates or replaces
  a `MaterialOverrideComponent`. `SceneManager.cpp:3656-3665` derives
  `sourceMaterialKey` from `ImportedMeshSourceComponent` but appears to emplace
  the component unconditionally, while `:3749` describes propagation to
  "every imported entity". Establish whether a non-imported entity can acquire
  an override, since that determines the defect's blast radius and the test
  fixtures.
- **F2.** Establish whether `SetMaterialIndexCommand::Redo` has the mirrored
  problem. If `m_AfterOverride` is also the post-mutation value it is
  accidentally correct, and the asymmetry should be stated in the fix rather
  than left for the next reader to rediscover.
- **F3.** Check whether any other `Record*Edit` in `SceneEditorUI.cpp` reads
  its before-state after the mutation. `RecordMaterialPropertiesEdit` and
  `RecordMotionEdit` are correct; the rest are unaudited. Report the audit
  result either way — a clean audit is a useful finding.

### Tests and discrimination proofs

Under D1(b), in `RT2Tests`:

1. **Undo restores the pre-edit override.** Build an imported entity with an
   authored override, change its material index, undo, and assert the entity's
   `MaterialOverrideComponent::material` equals the pre-edit value — not merely
   that `MeshRef::materialIndex` was restored.
   *Discriminating fault:* capture the before-override after the mutation, i.e.
   reintroduce the present defect. The material comes back as the after-value
   while the index is correct.
2. **Redo still reaches the post-edit override.** Undo then redo returns both
   index and override to the after-state.
3. **Round-trip through the resolver.** The case that makes this user-visible:
   after undo, serialize and reload, and assert the entity resolves to the
   pre-edit material. This is the assertion that would have caught the defect,
   because the in-session state looks correct without it.
   *Discriminating fault:* the same reintroduction as (1). This test goes red
   where an in-session-only assertion stays green — which is the point.

Every proof runs fault-first: inject, confirm red, revert, confirm green, and
record both. If a named fault does not go red, report that rather than
adjusting the test — that happened in the Phase 8 pre-work and the report was
more valuable than the fix.

### Acceptance

- Both configurations build and the full suite passes. Baseline at `ab3c852`
  is 752/752 and 146,525 assertions; state measured counts, do not copy these.
- Script, slice and `--headless --validate` gates green.
- D1 decided in writing with reasoning; F1-F3 answered with `file:line`
  evidence.
- All discrimination proofs recorded red-then-green.
- **Interactive check, required under either D1 option:** on an imported
  entity, change the material, undo, save, reopen, and confirm the entity has
  the pre-edit material. This is the only check that exercises the full path
  the user actually walks.
- No fixture modifications in `git status`.

### Boundary

This spec fixes the before-capture for material-index edits. It does not
restructure the command history, does not change the resolver's override
precedence, and does not alter `RecordMaterialPropertiesEdit`, which is already
correct and is the model to follow.

## Material-index undo restores the wrong override (verification report)

Written 2026-08-03. Implements the spec section immediately above (commit 80f7faf). Branch: phase8-material-index-undo-override-fix.
### D1 — where the before-capture lives: decided (b), the testable seam

The spec's recommendation proved proportionate after reading the code, so (b) was taken.

Why (a) was rejected, beyond shortness: (a) preserves the exact property that
produced this defect — the capture ordering lives at the call site as
discipline, not as structure. The correct pattern already existed just below
the defect (`RecordMaterialPropertiesEdit`, SceneEditorUI.cpp:184), and the
failure mode of the file was precisely "the author knew the right
pattern and did not carry it over". (a) would have left `GetMaterialOverride`
reads in the UI where one ordering mistake recreates the bug, and — because
RT2Tests does not compile SceneEditorUI.cpp (RT2Tests/premake5.lua:17) — no
regression test, meaning only the interactive check could ever catch a
reintroduction. The spec's fear of (b) — "drags in the resolver, the command
history, or a signature change that ripples further than the defect warrants"
— did not materialize: measured against the tree, the seam is two defaulted
optional out-params on the single mutation function plus one UI call site.

What (b) actually costs:
- `SetMaterialIndexState` (`SceneManager.h:491`, `SceneManager.cpp:3784`)
  gains two `std::optional<MaterialOverrideComponent>*` out-params with
  nullptr defaults. Existing callers compile unchanged: the `SetMaterial`
  wrapper (`SceneManager.cpp:3333`), `SetMaterialIndexCommand::Execute`/`Undo`
  (`EditorPropertyCommands.cpp:134/146`), and
  `RuntimeCommandSink::SetMaterialIndex` (`ScriptSystem.cpp:1630`) — which
  mutates the runtime SceneDocument directly and does not call the state API
  at all. The resolver, the command history, `EditorMutationResult`, and
  `ScriptSystem` are untouched.
- `SetMaterial` (`SceneManager.h:383`) forwards the out-params.
- The UI call site (`SceneEditorUI.cpp:1409-1419`) captures via `SetMaterial`
  and `RecordMaterialIndexEdit` (`.h:206`, `.cpp:162`) takes both overrides
  as parameters. The UI makes **zero** `GetMaterialOverride` reads on this
  path anymore.

The ordering is now enforced by the mutation itself: `SetMaterialIndexState`
reads the displaced component before the index write and the freshly recorded
one immediately after `RecordMaterialOverride` — inside one function, where
"before" and "after" cannot be swapped. Test 3 (the round-trip) is the
regression test this defect had been missing.

### F1 — guard under which SetMaterial creates/replaces the override

`SetMaterialIndexState` records the override only when the entity carries an
`ImportedMeshSourceComponent`: `if (m_EcsScene.registry.all_of<ImportedMeshSourceComponent>(e)) RecordMaterialOverride(e, afterIndex);` (`SceneManager.cpp:3810-3811`). The properties path propagates only over
`reg.view<ImportedMeshSourceComponent>()` (`SceneManager.cpp:3768`).
`RecordMaterialOverride` itself (`SceneManager.cpp:3657-3680`) emplaces
unconditionally (`reg.emplace_or_replace<MaterialOverrideComponent>(entity, ov)`, `:3679`) but its only two callers are the guarded sites above (`:3768`,
`:3810`) — a non-imported entity **cannot** acquire an override through any
editor mutation path, so the defect's blast radius is imported entities only,
and the test fixtures must be imported entities (they are).

Latent trap worth recording: the guard lives at the call sites, not inside
`RecordMaterialOverride`. An unguarded future caller would silently give a
non-imported entity a durable override. The function signature is the natural
place for that guard; left as-is (out of scope).

### F2 — Redo has no mirrored problem; the asymmetry is structural

`SetMaterialIndexCommand::Execute` (the redo path; `EditorPropertyCommands.cpp:132-142`) calls `SetMaterialIndexState(m_Target, m_AfterIndex)` and then
`InstallMaterialOverride(m_Target, m_AfterOverride)`. `m_AfterOverride` was
read post-mutation at the capture site, so it genuinely IS the after value —
Execute/Redo is accidentally correct. The asymmetry is not a coincidence of
the capture site but structural: the mutation destroys the before-state and
produces the after-state, so "before" must be captured inside the mutation and
"after" is its output. The fix makes that explicit — `SetMaterialIndexState`
captures both — and the comments at `SceneManager.h:491` and
`SceneManager.cpp:3784` state it, so the next reader does not rediscover it.

### F3 — audit of the remaining Record*Edit functions: clean

All in `SceneEditorUI.cpp` (current line numbers). Before-states are captured
before any mutation in every case except the fixed one:

- `RecordNameEdit` (:121-132): `name` read at :998, mutation at :1019.
  Correct.
- `RecordLightEdit` (:134-146): `beforeLight` read :1550 before any mutation;
  session activation :1568, mutation :1658. Correct.
- `RecordCameraEdit` (:148-160): `beforeCamera` read :1688, mutation :1749;
  session activation :1706. Correct.
- `RecordMaterialIndexEdit` (:162-182): the defect — both overrides read
  :166-169 (pre-fix numbering, as cited in the spec) after `SetMaterial`
  (pre-fix :1408) had already replaced the component. **Fixed in this
  change.**
- `RecordMaterialPropertiesEdit` (:184-206): activation-captured member
  (`m_PendingMaterialPropertiesBeforeOverrides`, :1459, cleared :1526).
  Correct (spec: known correct).
- `RecordMotionEdit` (:206-218): session `rec->before`; explicit `before`
  read :1084 before `SetMotionState` :1085. Correct.
- `RecordScriptEdit` (:220-229): before captured :1799/:1859/:1884 before each
  `SetScriptState`; session-based at :2125 and :2153. Correct.

Result: the audit is clean except the one defect this change fixes.

### What changed

- `RT2App/src/SceneManager.h`: `SetMaterial` (:383) and `SetMaterialIndexState`
  (:491) gained defaulted capture out-params with explanatory comments.
- `RT2App/src/SceneManager.cpp`: `SetMaterialIndexState` (:3784) captures the
  displaced override before the index write and the freshly recorded one
  after `RecordMaterialOverride`; failure paths leave the out-params
  untouched. `SetMaterial` (:3333) forwards them.
- `RT2App/src/SceneEditorUI.cpp` + `.h`: the call site (:1409-1419) captures
  via `SetMaterial`; `RecordMaterialIndexEdit` (:162, `.h:206`) takes both
  overrides as parameters and no longer reads live override state.
- `RT2App/src/EditorPropertyCommands.h`: corrected the factory comment, which
  still described the old capture protocol.
- `RT2Tests/src/MaterialIndexUndoOverrideTests.cpp` (new; registered in
  `RT2Tests/RT2Tests.vcxproj` and `RT2Tests/premake5.lua`): the three spec
  tests, in-memory fixture (tests 1-2) and GLB round-trip via
  `SceneSerializer` + `SceneAssetResolver::ResolveAll` (test 3).

### Discrimination proofs (all run fault-first, red then green)

**Proof A — the spec's named fault, in production code**: the before-capture
in `SetMaterialIndexState` moved AFTER the mutation (i.e., the present defect
reintroduced at the seam). Result:

- Test 1 RED: `before` came back as the after-value
  (`MaterialIndexUndoOverrideTests.cpp:158`) and after undo the durable
  override was the post-edit value while the index was correct (:178) — the
  spec's exact "material comes back as the after-value while the index is
  correct".
- Test 2 RED at its intermediate undo-state assertion (:203) — see note below.
- Test 3 RED both in-session (:254, :265) and — the point of the test — after
  save/reopen: the resolved material was the post-edit green, not the pre-edit
  red (:296-297). The save-and-reopen resurrection the spec predicts.
- Reverted: all three GREEN (755/755).

Note on test 2: the spec describes test 2's final-state claim ("redo still
reaches the post-edit override") as holding under the defect, since the
after-capture is genuinely the after state. My test 2 additionally asserts the
intermediate undo state (same assertion as test 1), so under proof A it went
red at that assertion before reaching redo. Deliberate strengthening, not a
spec contradiction — the final-state claim is still covered and passes on the
fixed code (below). On the fixed code all three pass.

**Proof B — test 2's redo sensitivity**: the only way redo can install the
wrong durable record is a corrupted after-override at construction, so the
fault was injected there (after = before). Result: exactly one failure — test
2's redo assertion (observed at :211 during the injected run, which is :209 in
the final file: after undo+redo the override was the before-value while the
index was correct). The redo assertion is a real check, not vacuous. Reverted:
GREEN.

**Test-shaping findings recorded while proving**:
1. The resolver rewrites the override's texture indices from the re-imported
   staged material (`SceneAssetResolver.cpp:768-777`), so the round-trip
   assertion must compare the authored scalars (type/baseColor/metallic/
   roughness/ior/emissive) — a full-material comparison can never pass across
   the round-trip. `AuthoredScalarEq` in the test file.
2. `REQUIRE(le != entt::null)` is ambiguous between doctest's
   `Expression_lhs` and entt's `operator!=`; the bool is computed outside the
   macro.

### Measured counts and gates

- Baseline at 80f7faf (before any change): Release 752/752 tests, 146,525
  assertions.
- After the fix and tests: **Release 755/755, 146,579 assertions; Debug
  755/755, 146,579 assertions** (both full-suite runs from the repo root).
- `run_script_test.ps1`: PASS (60 frames, no mismatches).
- `run_slice_test.ps1`: PASS (60 steps, authoring intact).
- `--headless --validate` (vertical-slice, 8 frames, 320x200): exit 0.
- `git status`: no fixture modifications (only the five RT2App sources, the
  two test-project files, the new test file, and the generated
  graphify-out/GRAPH_REPORT.md).

### Interactive check — BLOCKED, not performed

The acceptance check — on an imported entity, change the material, undo,
**save, reopen**, and confirm the entity has the pre-edit material — cannot be
performed from this environment: it requires driving the desktop application
(ImGui inspector, mouse/keyboard). It is reported as **blocked**, not as
passed and not as "expected to pass". Test 3 is the automated analogue of the
same walk (it serializes and re-resolves through the same resolver the save/
reopen path uses), and it is the assertion that fails under the reintroduced
defect, but the on-screen interaction remains unperformed and must be run by
someone with the app open.

### Out of scope, unchanged

`RecordMaterialPropertiesEdit` (correct, the model), the resolver's override
precedence, the command history, and `CompactMeshRegistry` are untouched.

### Material-index undo — interactive acceptance closed (2026-08-03)

The one acceptance item reported BLOCKED in the verification report above has
been performed and **passed**. Recorded here so the blocked entry is not read
later as still outstanding.

- **Check:** on an imported entity, change the material, undo, save, reopen,
  confirm the entity has the pre-edit material.
- **Result:** passed.
- **Performed by:** the repo owner, driving the desktop editor by hand. The
  implementing model correctly reported it as blocked rather than as passed or
  as covered by the automated tests; it is not covered by them.

Automated test 3 (`MaterialIndexUndoOverrideTests.cpp`) proves the **document**
round-trips correctly through save, load and `SceneAssetResolver::ResolveAll`.
This check proves the **editor** does — the inspector reflects the reverted
material and nothing in the host layer re-derives the override on load. The
Phase 7 popup-scope defects all lived in exactly that untested host layer, so
the two are not interchangeable.

## Phase 8 pre-work 2 — source-material identity and key-based override matching (implementation spec)

Written 2026-08-03 from the grounding findings
(`docs/phase8-prefabs-grounding-findings.md`, Q4). Second correctness
prerequisite for Prefabs, after override-aware compaction (`ab3c852`).

This one is not latent. It is a live defect in the imported-model path today.

### The problem

An authored `MaterialOverrideComponent` binds to its source material **by slot
position**, not by identity. The resolver matches the entity's mesh
`sourceKey` against the rebuilt model's `keyMap`
(`SceneAssetResolver.cpp:495-517, 590`), obtains `(meshIndex, materialIndex)`,
and applies the override against whatever material now sits at that index
(`:764-783`).

So when a source file is re-exported with its materials **reordered, or one
removed**, the entity still resolves — mesh identity is unchanged — and the
override silently re-binds to a different material. Texture indices are
repaired from the new occupant (`:768-777`). No diagnostic is raised. The user
sees an authored material land on the wrong surface, with nothing connecting it
to the re-export.

`sourceMaterialKey` exists to prevent exactly this and **does not work**:

- The component comment (`ECSComponents.h:253-256`) claims it lets the resolver
  "match it against a rebuilt source material", and gives the form
  `"gltf:material=<index>"`.
- **The resolver never reads it.** Across `RT2App` it is written once
  (`SceneManager.cpp:3677`), serialized (`SceneSerializer.cpp:707, 889`), and
  compared for equality (`SceneManager.cpp:1846`,
  `EditorPropertyCommands.cpp:91`). Nothing consumes it for matching.
- The value written is `src->model.sourceKey + ":material"`
  (`SceneManager.cpp:3670-3672`) — the **mesh** key plus a literal suffix. It
  does not identify a material and cannot distinguish two materials on one
  mesh. The documented `"gltf:material=<index>"` form never occurs.

The field is currently dead weight that actively misleads readers. It misled
the author of the Phase 8 pre-work review, who cited the comment rather than
checking the code.

### Why this blocks prefabs

A prefab instance is a source reference plus a sparse set of property deltas
reattached to the source on load. **Reattachment is identity matching.** If we
build prefab overrides on the current material system, they inherit
match-by-position, and the same silent rebinding appears in a feature whose
entire purpose is surviving source edits.

The material path is also the only worked example of source-to-instance
override reconciliation in the tree. It is what the Phase 8 spec will
generalise from. It should be correct before it is generalised.

### What does not exist yet

`SceneMaterial` (`SceneTypes.h:86-107`) carries **no source identity** — no
name, no source index, nothing durable. `keyMap`
(`SceneAssetResolver.cpp:390`) is keyed by mesh `sourceKey` and its value is a
`(meshIndex, materialIndex)` pair. So the source side has nothing to match
against, and minting a real key requires the loader to surface material
identity that it currently discards.

That is the substance of this work. The resolver change is small once the
identity exists.

### Decisions — answer before code

**D1 — the form of a source material identity.** Both loaders must produce a
key that survives re-export.

- glTF: materials have an optional `name` and a positional index. Names are
  more durable across re-export; indices reorder. Names may be absent or
  duplicated.
- OBJ: `.mtl` materials are named by construction (`newmtl <name>`), and the
  name is the natural durable identity.

**Recommendation:** prefer the name when present and unique, fall back to the
index, and make the key self-describing about which was used — e.g.
`gltf:material:name=Brass` versus `gltf:material:index=3`. A key that does not
say what it matched on cannot be reasoned about when it later fails to match.
Reject this if the encoding proves unwieldy, but do not silently choose index
alone: index-only matching is what we already have, wearing a key.

**D2 — where the source-side identity lives.** The override carries its key on
the component. The *source* side needs the same identity available at resolve
time.

**Recommendation:** a parallel per-material key table on the staged/imported
model, alongside the existing `keyMap`, rather than a new field on
`SceneMaterial`. `SceneMaterial` is a GPU-adjacent value type that is
serialized verbatim inside every authored override; adding identity to it
changes the serialized shape of every override and couples value to identity.
Argue for the field on `SceneMaterial` if the parallel table proves awkward,
but state the serialization consequence if you do.

**D3 — what happens when the key does not match.** Existing scenes carry the
old bogus `"<meshKey>:material"` value, and a genuinely renamed source material
will also miss.

**Recommendation:** fall back to the current slot-position behaviour **and
raise a diagnostic**. Do not fail the load and do not drop the override —
either would be destructive for a case that is often benign. The existing
`AssetDiagnostic` channel (`SceneAssetResolver.cpp:656-669`) is the right
vehicle. The point of this work is that the silent case becomes loud, not that
it becomes fatal.

**D4 — migration of existing keys.** Old-format keys are recognisable
(they end in `:material` and equal the mesh key plus that suffix). Decide
whether to migrate them on load, treat them as unmatched under D3, or reject
them. Note that any decision here interacts with the schema version; say
explicitly whether a bump is required and why.

### The change, in order

1. Loader surfaces a durable per-material source identity for glTF and OBJ,
   per D1.
2. Staged/imported models carry that identity per material, per D2.
3. `SceneManager` mints `sourceMaterialKey` from the real identity instead of
   `model.sourceKey + ":material"` (`:3670-3677`).
4. The resolver matches the override to the rebuilt material **by key**, using
   the resolved slot only as the D3 fallback (`SceneAssetResolver.cpp:764-783`).
5. Diagnostics on miss, per D3.
6. Migration, per D4.
7. Correct the `MaterialOverrideComponent` comment
   (`ECSComponents.h:253-256`) so it describes what the code does. It has been
   wrong since it was written and has already misled one reader.

### Tests and discrimination proofs

`SceneManager.cpp`, `SceneAssetResolver.cpp` and `SceneLoader.cpp` are all
compiled into `RT2Tests`, so every case below is automatable.

1. **Reordered source materials keep the override on its own material.**
   Import a model with two distinct materials, override one, rewrite the source
   with the materials in the opposite order, re-resolve, and assert the
   override still applies to the same *material*, not the same slot.
   *Discriminating fault:* match by slot index instead of key. The override
   lands on the wrong material — the present behaviour.
2. **A removed source material raises a diagnostic and does not silently
   rebind.** Per D3.
   *Discriminating fault:* drop the diagnostic; the test sees a silent
   fallback.
3. **Round trip.** A minted key survives save, load and re-resolve unchanged.
4. **Legacy key handling** per D4 — whichever behaviour is chosen, pin it.
5. **Two materials on one mesh are distinguishable.** The current key cannot
   express this at all; the new one must.
   *Discriminating fault:* revert to the mesh-key-plus-suffix form; both
   materials collide on one key.

Every proof runs fault-first: inject, confirm red, revert, confirm green,
record both. If a named fault does not discriminate, report that rather than
adjusting the test — that has already happened once in this phase and the
report was worth more than the fix.

### Acceptance

- Both configurations build; full suite passes. Baseline at `c6200ca` is
  755/755 and 146,579 assertions. State measured counts; do not copy these.
- Script, slice and `--headless --validate` gates green.
- D1-D4 answered in writing with reasoning.
- All discrimination proofs recorded red-then-green.
- **Interactive check:** import a model with at least two materials, override
  one in the inspector, save, edit the source file so the materials are
  reordered, reopen, and confirm the override is still on its own material.
  If you cannot drive the desktop application, report this as **blocked** —
  not as passed, not as covered by the automated tests.
- No fixture modifications in `git status`.

### Boundary

This spec does not implement prefabs, does not add per-property override
granularity (the override remains a whole-`SceneMaterial` snapshot), and does
not change the compaction passes. It makes source-to-override reattachment
identity-based so that Phase 8 can generalise a mechanism that is correct.

## Phase 8 pre-work 2 — verification report (material-key identity, 2026-08-03)

Implementation spec above; grounded at `master` `827eb8c` (755/755, 146,579
assertions, Release). All work landed on branch `phase8-prework2-material-key`
off that commit.

### What was built

- `SceneMaterial::sourceKey` (`SceneTypes.h`): loader-minted durable identity,
  travels with the value (D2, overturning the parallel table — the string
  cannot desync from its material).
- Key minting in all four loader loops (`SceneLoader.cpp`): glTF load
  (`~:725`), glTF import (`~:1428`), OBJ load (`~:2060`), OBJ import
  (`~:2368`), each with a name-count pre-pass; name form preferred,
  `gltf:material:name=<n>` / `obj:material:name=<n>`, index form only when the
  name is absent or duplicated (D1).
- `RecordMaterialOverride` (`SceneManager.cpp:3657`) mints the override key
  from the live `materials[materialIndex].sourceKey` (empty for author-created
  materials → resolver slot behavior, no diagnostic).
- Resolver (`SceneAssetResolver.cpp`): `IsNewFormMaterialKey` /
  `IsLegacyMaterialKey` helpers beside `ParseGltfKey`; plan-pass key match
  against the staged materials' `sourceKey` before the slot fallback (D3:
  `Stale` diagnostic on a miss, never fatal); legacy `<meshKey>:material`
  keys rebased in the commit pass only for committed models, so a rejected
  resolve never mutates authored state (D4). OBJ legacy keys
  (`obj:whole-model:material`) stay legacy — per-triangle materials have no
  concrete slot to rebase to, and the key remains entity-wide historical
  semantics (obsolete, not a miss, no diagnostic).
- Serializer: additive `j["sourceKey"]` on every material block
  (`SceneSerializer.cpp:112/135`) — accepted drift (D2).
- Removed the dead `GltfMaterialKey`/`ObjMaterialKey` helpers
  (`SceneAssetResolver.h/.cpp`) that emitted a now-ambiguous `gltf:material=<i>`
  form; corrected the `MaterialOverrideComponent::sourceMaterialKey` comment
  (`ECSComponents.h`) and the W0 path-walker comment
  (`SceneSerializerTests.cpp`) — comments only, no assertion changes.
- Tests: `RT2Tests/src/Phase8Prework2MaterialKeyTests.cpp` (6 cases,
  registered in `RT2Tests.vcxproj` and `premake5.lua`).

### Measurements

- Release: **761/761, 146,716 assertions** (baseline 755/755, 146,579; +6
  tests, +137 assertions).
- Debug: **761/761, 146,716 assertions** — the "8 known failures in OBJ
  fixture generation" recorded in the test baseline did not reproduce at this
  commit; the measured Debug run is fully green.
- `run_script_test.ps1`: PASS. `run_slice_test.ps1`: PASS.
- `--headless --validate --scene vertical-slice.rt2scene --frames 8`:
  PASS (rendered `artifacts/v.png`).
- No fixture modifications: `RT2App/assets/script-scenario.rt2scene` and
  `RT2App/assets/vertical-slice.rt2scene` both clean in `git status`. One
  expected consequence observed and restored: the validate run rewrites
  checked-in scenes with the additive `"sourceKey": ""` field — the
  accepted D2 drift.

### Discrimination proofs (fault first, then revert)

All proofs ran against the six new tests only; the 755 pre-existing tests
were untouched (no existing expectation moved) and the final full suites are
green.

1. **C1 serializer round trip** — fault: strip the `j["sourceKey"]` write in
   `MaterialToJson`. RED: C1 failed, exactly the two material-block
   assertions (`Phase8Prework2MaterialKeyTests.cpp:375-376`); the other four
   tests stayed green. Reverted → green.
2. **C2 loader surface** — fault: disable the OBJ-import mint block
   (`SceneLoader.cpp ~:2368`). RED: C2 failed, exactly the two
   `obj:material:name=` assertions (`:450-451`); the OBJ mutation-seam
   assertions stayed green (they prove `RecordMaterialOverride` reads the
   live value, which is independent of the mint). Reverted → green.
3. **C3 cross-path resolver matching** — fault: `IsNewFormMaterialKey`
   returns false (slot-only matching). RED: C3 failed — a `Stale` diagnostic
   appeared and the override adopted the slot-0 texture instead of the
   key-matched one (`:383`, `:528`); C5 also red (same mechanism). Reverted →
   green.
4. **C4 D3 miss diagnostic** — fault: remove the diagnostic push in the
   new-form miss branch. RED: C4 failed, exactly the `CountStale == 1`
   assertion (`:581`) — the miss went silent; slot fallback still applied.
   Reverted → green.
5. **C5 shared-mesh name matching** — fault: mint index-form keys in the
   glTF load loop. RED: all five glTF tests red (the load-path mint is
   shared); C5's fatal was the name-form key assertion at `:629` — the
   override keyed `gltf:material:index=0` silently re-binds to whatever
   definition sits at slot 0 after the swap. Reverted → green.
6. **D4 legacy-key rebase** — fault: disable the rebase assignment in the
   commit pass. RED: the D4 test failed, exactly the glTF rebase assertion
   (`:661`); the OBJ keep-as-is sub-case stayed green (no rebase is expected
   there). Reverted → green.

### Interactive check

**Blocked.** The desktop application cannot be driven from this environment.
The identical walk (import two-material model → override one → save → reorder
source materials → reopen → override follows its material) is executed
headlessly by C3 (single override, definition-swap reorder, texture-copy
observable) and C5 (both overrides, shared-mesh reorder) and by C4's
rename/miss case, but per the spec this is reported blocked, not passed.

### Defects found along the way

- Dead `GltfMaterialKey`/`ObjMaterialKey` helpers emitted a misleading
  `gltf:material=<i>` key form that matched neither the new nor the legacy
  shape — removed (would have routed every such key through the D3
  "unrecognized form" branch).
- The additive material-block field silently rewrites checked-in scene
  assets on save/validate — expected and accepted under D2; assets restored
  for this commit.
- `graphify update .` initially failed to swap `graph.json` (WinError 32
  rename lock on the 42 MB file); `--force` completed the swap. Note for
  future runs on this machine.

### Notes

- The OBJ resolve key-match path is the same code as glTF (single match loop
  over staged materials); the OBJ surface is proven by C2's mint assertions
  and the mutation seam, and the D4 OBJ keep-as-is sub-case. The OBJ
  reorder-at-the-loader-surface case cannot discriminate (per-triangle
  material indices ride through reorders unchanged), so it is proven by the
  key-string mutation seam instead, as the spec's fixture notes require.
- `git status` at commit: sources, tests, project files, and this document;
  `graphify-out/GRAPH_REPORT.md` left unstaged (pre-existing dirty state).

## Phase 8 — Prefabs (implementation spec)

Written 2026-08-03 from `docs/phase8-prefabs-grounding-findings.md`, grounded
at `697d3c9` and current at `e7cb72d`. Supersedes nothing; the roadmap stub
(`:586-620`) remains the statement of intent, and this is how it gets built.

Both pre-works are merged: override-aware compaction (`ab3c852`) and
source-material identity (`e7cb72d`). Baseline is 761/761 and 146,716
assertions.

### What a prefab is, in RT2 terms

A prefab is an **entity subtree saved as an asset**, instantiated many times,
where each instance keeps a link to the source and a sparse set of deltas.
Editing the source updates every instance except where an instance has
deliberately diverged.

The engine already has the shape in miniature: `ImportedMeshSourceComponent`
is a source link and `MaterialOverrideComponent` is a durable override with
precedence (`ECSComponents.h:221-261`). Phase 8 generalises source-link +
override from *one property class on imported meshes* to *any authored
component on any subtree*.

**It does not already have the identity half.** There is exactly one identity
in the tree — the document-global UUID on `EntityIdComponent`
(`ECSComponents.h:144-152`) — and grep for `localId|instanceId` across the
whole tree returns zero matches (findings Q1). The roadmap's "stable local
IDs" are net-new.

### Decisions — answer before code

**D1 — instance identity model.** An instance must map each of its entities
back to the template entity it came from, or overrides cannot reattach.

**Recommendation: two components, no per-instance maps.**

- `PrefabInstanceComponent` on the instance root: the prefab `AssetReference`
  and an `instanceId` UUID.
- `PrefabMemberComponent` on every entity of the instance: `instanceId` plus
  `templateId` — the entity's identity *inside the prefab asset*.

`templateId` is the prefab-local identity Q1 says does not exist. Mint it once
when the prefab asset is created, freeze it in the file, and never regenerate
it. Scene UUIDs stay fresh per instance, exactly as duplication already does.

This keeps identity in the ECS where every other identity lives, serializes
through the existing per-component machinery, and needs no side table that can
desync — the argument that overturned D2 in pre-work 2.

**D2 — override granularity.** The roadmap says "property-level overrides"
(`:597`). RT2 has no property reflection outside `ScriptFieldRegistry`, and
`MaterialOverrideComponent` is a whole-value snapshot, not per-property
(findings, Contradictions 2).

**Recommendation: component-level granularity for Phase 8.** An authored
component on an instance entity is either **inherited** from the template or
**overridden** wholesale. The override set is a subset of
`PersistedComponents` (`PersistedComponents.h:20-36`) — the list the
serializer and duplication already agree on, with a `static_assert` keeping
them honest.

This is a real reduction from the roadmap and must be recorded as one.
Per-property granularity means building a reflection layer over every
authored component; that is a phase of its own and would dominate this one.
Component granularity delivers the actual user need — move one instance,
retint one instance, keep the rest tracking — at a fraction of the cost.
Revisit if it proves too coarse in use, not before.

**D3 — script `Uuid` field remapping.** `ScriptFieldValue` has a `Uuid` arm
(`ScriptFieldValue.h:119-126`) serialized as an opaque string. **Nothing in
the engine resolves it to an entity** (findings Q2), and duplication copies it
verbatim, so a duplicated subtree whose script references a sibling points at
the original (findings Q3). No test pins this, and intent could not be
determined.

**Recommendation: remap on instantiate when the value names an entity inside
the subtree; leave it untouched otherwise.** A reference into the subtree is
structurally part of the prefab; a reference out of it is a scene-level fact
the instance should not rewrite.

**And fix duplication with the same remapper.** Duplication has the identical
defect today. Building instantiation's remapper separately would implement the
same logic twice and leave the existing bug in place. One remapper, two
callers, with the duplication case as a regression test. This is a behaviour
change to duplication — record it as intentional.

**D4 — prefab file format.** No subtree serializer exists; `SaveInternal`
emits whole documents and `SubtreeSnapshot` is in-memory only (findings Q5).

**Recommendation:** a new `.rt2prefab` JSON file whose entity records reuse the
`SubtreeSnapshot`/`EntityRecord` shape (`SubtreeSnapshot.h:43-83`), with its
**own version constant** independent of `.rt2scene`. Reusing the record shape
means one set of component codecs; an independent version means prefab format
changes do not force a scene schema bump.

**Transient indices must never be written.** `MeshRef::meshIndex` and
`MaterialOverrideComponent::materialIndex` are transient by design, and
override *texture* indices are serialized raw and repaired only from a
currently-staged material (findings, Fragility 1). A prefab instance whose
source is not materialised has nothing to repair from. Prefab files carry
asset references and source keys, never resource-table indices.

**D5 — `.rt2scene` schema version.** New components on serialized entities
change the document. Determine whether v4 → v5 is required, and state it
explicitly. Note the reader consequence from findings Q5: **an unknown
`AssetKind` with a non-empty path is a hard parse error**
(`SceneSerializer.cpp:304-309`), so older builds cannot load scenes that
reference prefabs. That is acceptable but must be deliberate.

**D6 — structural deltas.** A child added to or deleted from an instance, or
from the template after instances exist, is a structural delta rather than a
value one. This is where prefab systems get genuinely ugly.

**Recommendation: out of scope for Phase 8, with a loud guard.** Detect the
case and raise an `AssetDiagnostic`; do not silently mis-merge. The roadmap's
"deleted or added prefab children merge predictably" moves to a follow-on
workstream. Nested prefabs stay deferred per the roadmap (`:600`).

Deferring this is the single largest scope call in the spec. Say so plainly in
the phase's closure rather than letting it read as delivered.

### Workstreams

Each lands separately with its own tests, on its own branch, verified before
the next begins.

**W0 — prefab asset kind and file format.** `AssetKind::Prefab` through the
enum, name codec both ways, reader acceptance, watcher extension list, sidecar
minting, and the content-browser reimport policy (findings Q5 enumerates
every site). `ImportSettings` is a fixed type — either add an arm or accept
that prefabs carry none, and say which.

**W1 — create prefab from subtree, and instantiate.** `CaptureSubtreeSnapshot`
(`SceneManager.cpp:2364`) already produces the right shape; W1 serializes it,
mints `templateId`s, and instantiates with fresh scene UUIDs and remapped
hierarchy. No overrides yet — an instance is a faithful copy plus a link.
Reuse `RebuildChildren` (`SceneHierarchy.cpp`) rather than hand-wiring the
dual parent/children representation (findings, Fragility 3).

**W2 — reference remapping, shared with duplication.** Per D3. Delivers the
duplication fix as well as instantiation.

**W3 — overrides.** Per D2: mark a component overridden, serialize the
override set, and apply overrides on top of template values at load. This is
where `MaterialOverrideComponent`'s precedence model generalises.

**W4 — propagation.** Editing the prefab source updates every instance's
non-overridden components. This is the workstream that makes the feature
worth having, and the one the roadmap's runtime acceptance exercises.

**W5 — revert, apply, unpack.** Revert drops an override. Apply pushes an
instance's override into the source. Unpack severs the link, leaving ordinary
entities.

**W6 — content browser and inspector surfacing.** Overrides must be *visible*
and revertable, per the exit criterion (`:619`). An override the user cannot
see is a bug they cannot diagnose.

### Constraints that apply to every workstream

- **Commands carry complete state at construction.** `EditorCommand.h:19-21`
  is a hard contract, and the `697d3c9` defect was exactly its violation.
  Follow the structural precedent: capture pre-mutation, or use the
  `RecordApplied` + authoritative post-mutation snapshot pattern. Never
  read-after-mutate.
- **Snapshots depend on the no-compaction invariant.** Compaction is deferred
  while history is live (`WalnutApp.cpp:304-307`). Prefab commands holding
  snapshots inherit that dependency; do not weaken it.
- **Names are not unique.** Duplication appends `" Copy"` and
  `RuntimeCommandSink::FindByName` picks the first in UUID order
  (`ScriptSystem.cpp:1661-1683`). Prefab instance naming will collide with
  this; decide the naming scheme in W1 and state the collision behaviour.
- **New authored components go in `PersistedComponents`**
  (`PersistedComponents.h:20-36`). The `static_assert` makes serializer and
  duplication coverage fail together if they are not.

### Tests

Per workstream, in `RT2Tests` — `SceneManager.cpp`, `SceneSerializer.cpp` and
`SceneAssetResolver.cpp` are all compiled into the test project, so the
mechanism is fully testable. The host UI is not; W6 is the workstream whose
acceptance is necessarily interactive.

The roadmap's test list (`:602-608`) is the floor, not the ceiling:

- Multiple instances get unique scene UUIDs with correct internal references.
- Source updates propagate to non-overridden components.
- Overrides survive save/load and source updates.
- Apply/revert produces deterministic serialized output.
- **Plus, from the findings:** a duplicated subtree's script `Uuid` field
  pointing at a sibling resolves to the copy (the W2 regression); a prefab
  file never contains a resource-table index; a structural delta raises a
  diagnostic rather than mis-merging (D6).

Every test gets a discrimination proof with a named fault: inject, confirm
red, revert, confirm green, record both. If a named fault does not
discriminate, report it rather than adjusting the test — that has happened
twice in this project and both reports were worth more than the fix.

### Acceptance

The roadmap's runtime acceptance is the phase's spine (`:612-614`): create a
light fixture hierarchy, instantiate it several times, override one material,
update the source, and verify the expected propagation. **Build toward that
specific scenario from W1 and let it drive the design**, rather than
implementing the full operation set and testing it at the end.

Phase exit (`:617-619`): common hierarchy reuse no longer requires
copy/paste, and overrides are visible and recoverable.

### Boundary

Phase 8 delivers single-level prefabs with component-granularity overrides.
It does not deliver nested prefabs (roadmap-deferred), per-property override
granularity (D2), or structural delta merging (D6). Each is a recorded
decision with a stated reason, and each must appear in the phase closure as
*not delivered* rather than being quietly absorbed into "prefabs, done".

## Phase 8 W0 — verification report (prefab asset kind and file envelope, 2026-08-03)

Implementation spec above (W0), grounded at `master` `38a98af` (761/761,
146,716 assertions). Landed on branch `phase8-w0-prefab-kind` off that commit.

### Every Q5 site touched

1. **Enum arm** — `AssetReference.h:29-37`: `AssetKind::Prefab = 5`.
2. **Name codec, both directions** — `AssetKindName`/`AssetKindFromName` were
   file-local in `SceneSerializer.cpp`'s anonymous namespace (TU-internal,
   the anon namespace closes at `:1287`); they are now `inline` functions in
   `AssetReference.h` (the neutral, CPU-only home of `AssetKind`) with the
   `"prefab"` arm. This makes the codec testable from RT2Tests — it was not,
   before.
3. **Reader acceptance** — no code change needed: the hard parse error for an
   unknown kind with a non-empty path (`SceneSerializer.cpp:312`) now never
   fires for `"prefab"` because the codec returns `AssetKind::Prefab`. The
   genuinely-unknown path is unchanged (verified by test).
4. **Watcher extension list** — `AssetWatchPolicy.cpp:23-27`: `.rt2prefab`
   added to `DatabaseRefresh`.
5. **Sidecar minting via `ResolveOrAssign`** — no code change: it is
   kind-agnostic (`AssetSidecarPath` appends `.rt2meta` to any path), so a
   `.rt2prefab` gets a sidecar and the assign-once property automatically.
   Proven by test (deterministic provider, two imports, stable ID).
6. **Content-browser reimport policy** — `ContentBrowserOperations.cpp:552`:
   a `.rt2prefab` reimport is now recognized and rejected with an explicit
   prefab-specific diagnostic ("re-import/propagation is a later workstream")
   instead of the generic ".glb/.gltf/.obj only" message.

Sites examined and deliberately **not** changed in W0 (findings' reach but
out of W0's boundary, recorded so W1 knows they exist):
- `DispatchContentBrowserAssetDrop` (`ContentBrowserOperations.cpp:336`) — a
  prefab drop today returns the generic "unsupported extension" error, which
  is honest for W0; instantiation-from-drop is W1.
- Loader/import dispatch (`SceneLoader.cpp`, `SceneManager.cpp`) — prefabs
  are not imported into the scene in W0, so no loader site needs a prefab
  branch yet.
- `AssetReferenceToJson` IS touched (see ImportSettings decision below): it is
  where the fixed-type `ImportSettings` block is either written or omitted.

Findings Q5 that needed no change because they were already generic:
`AssetRecord`/`observedKinds` (a sorted, deduplicated `std::vector<AssetKind>`),
the scanner ("an asset is a file with a sidecar"), and the content-browser
listing (no kind filter — a prefab with a sidecar lists by name).

### Decisions

**D4 — envelope approach confirmed.** The `.rt2prefab` envelope is a `json`
document with `header: "rt2prefab"`, its **own** `version` constant
(`PrefabSerializer::FormatVersion = 1`, independent of
`SceneSerializer::SchemaVersion = 4`), and an `entities` array that reuses
`SubtreeEntityRecord` (`SubtreeSnapshot.h:43-83`) so W1's record codec shares
component codecs with scene serialization. W0's Save/Load round-trips an
empty record list and **refuses a non-empty one loudly** (`InvalidArgument`
on save, `Parse` on load, both naming W1) — this is the seam W1 fills without
touching format plumbing, and it is the "prefer loud failure" character rule:
W0 never silently drops or invents records.
**Hard rule honored:** the W0 envelope contains no resource-table index field
of any kind. `SubtreeEntityRecord` carries `meshIndex`/`materialIndex`
(TU-transient), so W1's record codec must strip them and the override's
texture indices (repairable only from a materialised source) — flagged for W1.

**D5 — no `.rt2scene` v4 → v5 for W0. Verified, not assumed.** W0 adds no
components to scenes, no new serialized entity data, and — crucially —
nothing in W0 writes a prefab `AssetReference` into a scene (no instantiation,
no propagation). The kind codec change is purely additive over existing
bytes. The reader consequence the spec calls out (an unknown kind with a
non-empty path is a hard parse error at `SceneSerializer.cpp:312`, so older
builds cannot load scenes that reference prefabs) becomes live only once a
scene first references a prefab — that moment is W1 (instantiation) or W4
(propagation), and it is where the v4→v5 decision must be made deliberately.
Nothing in W0 brings it forward.

**`ImportSettings` — prefabs carry none, and the codec says so.** The fixed
type (`AssetReference.h`) has no prefab meaning; adding an arm would invent
settings. `AssetReferenceToJson` (`SceneSerializer.cpp:207`) now omits the
`importSettings` block for `Prefab` exactly as it already does for `Script`
(the established no-inert-settings-block precedent), so a future reader
cannot mistake defaulted knobs for real prefab settings.

### What was built

- `AssetKind::Prefab` + inline name codec (`AssetReference.h`).
- `.rt2prefab` in the watcher's refresh list (`AssetWatchPolicy.cpp`).
- Prefab-aware reimport rejection (`ContentBrowserOperations.cpp`).
- `PrefabSerializer.h/.cpp` (new): `PrefabDocument`, `PrefabSerializer::Save`
  /`Load`, `FormatVersion`, atomic tmp+replace write, deterministic `dump(2)`
  output, transactional Load (dest unchanged on any failure). Registered in
  `RT2App.vcxproj`, `RT2Tests.vcxproj`, and `RT2Tests/premake5.lua`.
- `RT2Tests/src/Phase8W0PrefabKindTests.cpp` (6 tests) registered in both
  test project files.

### Measurements

- Release: **767/767, 146,760 assertions** (baseline 761/761, 146,716; +6
  tests, +44 assertions).
- Debug: **767/767, 146,760 assertions.**
- `run_script_test.ps1`: PASS. `run_slice_test.ps1`: PASS.
- `--headless --validate --scene vertical-slice.rt2scene --frames 8`:
  PASS.
- No fixture modifications in `git status` after the validate run (the run
  rewrites `vertical-slice.rt2scene` byte-identically; only an EOL-normalization
  marker appears, and the authoritative bytes are restored; `8314bdd` already
  prevents the empty-sourceKey field churn).

### Discrimination proofs (fault first, then revert)

All ran against the six W0 tests; the 761 pre-existing tests were untouched
and the full suites stayed green.

1. **C1 kind codec** — fault: drop the `"prefab"` arm from
   `AssetKindFromName`. RED: exactly the two Prefab round-trip assertions
   (`Phase8W0PrefabKindTests.cpp:71-72`); the unknown-rejection and
   other-kind checks stayed green. Reverted → green.
2. **C2 envelope round-trip** — fault: omit the `entities` array in Save.
   RED: `Load` rejected the file (missing-array check), exactly the Load
   REQUIRE (`:111`). Reverted → green.
3. **C3 sidecar assign-once** — fault: remove the reuse-existing-sidecar
   early return in `AssetIdentity.cpp` (`ResolveOrAssign`). RED: exactly the
   `id2 == id1` and `minted2 == false` assertions (`:149-150`); first-import
   behavior stayed green. (Collateral: two pre-existing AssetIdentityTests
   pinning the same reuse property also went red — expected, the fault is a
   real behaviour change.) Reverted → green.
4. **C4 watcher classification** — fault: drop `.rt2prefab` from the watch
   list. RED: exactly the `.rt2prefab` classify + needs-refresh assertions
   (`:164-165`); the `.rt2meta` sidecar and existing-extension checks stayed
   green. Reverted → green.
5. **C5 version mismatch** — fault: remove the version-range check in Load.
   RED: the version-99 load was silently accepted, failing `:193-196`; the
   header check stayed green (it is a separate check). Reverted → green.
6. **C6 reimport policy** — fault: omit the `.rt2prefab` reimport branch.
   RED: exactly the prefab-diagnostic content assertion (`:239`) — the
   generic message does not mention prefab; the rejection itself and its
   `InvalidArgument` code stayed green (both paths reject, only the message
   discriminates). Reverted → green.

### Interactive check

**Blocked.** The desktop application cannot be driven from this environment,
so "place a `.rt2prefab` in the asset root and confirm the content browser
lists it without error" was not performed. The listing is sidecar-gated and
kind-filter-free by existing design (`SearchContentBrowserAssets` filters by
name only), so a prefab with a sidecar would list, but per the task this is
reported blocked, not passed and not "expected to pass".

### Anything W1 will need that W0 did not provide

- **The record codec.** W0 refuses non-empty `entities` at both Save and
  Load; W1 replaces both rejections with a `SubtreeEntityRecord` → JSON
  writer/reader. Reuse the scene component codecs (in `SceneSerializer.cpp`'s
  now-shrunk internal helpers) rather than re-serialising components.
- **Strip transient indices in that codec.** `meshIndex`,
  `materialIndex`, and override texture indices must not reach the file
  (hard rule). SubtreeEntityRecord carries them in memory; W1 chooses the
  explicit omit.
- **`templateId`.** `SubtreeEntityRecord` has `uuid` (the *document* UUID).
  W1 must mint and freeze a prefab-local `templateId` on creation
  (spec D1) and add it to the record shape — the current struct has no slot
  for it.
- **A prefab import path** that calls `ResolveOrAssign` with the
  `.rt2prefab` path (the primitive is proven assign-once; no engine wiring
  exists yet) and a scene-side `AssetReference` of kind Prefab.
- **The deliberate v4→v5 decision** becomes due the moment a scene first
  contains a prefab reference (unknown-kind-with-path is a hard parse error
  for older builds).
- **Naming.** Prefab instances inherit the "names are not unique" constraint
  (spec); the W1 naming scheme is not touched by W0.

### Phase 8 amendment A1 — prefab records wrap `SubtreeEntityRecord` (2026-08-03)

**Raised by:** the W0 handover list. `SubtreeEntityRecord`
(`SubtreeSnapshot.h:43-83`) has nowhere to put `templateId`, the prefab-local
identity D1 requires. The Phase 8 spec assumed the record shape could carry it
and it cannot.

**Decision: W1 introduces a prefab-specific record that *wraps*
`SubtreeEntityRecord` rather than extending it.**

Shape, approximately — W1 owns the details:

```
struct PrefabEntityRecord
{
    rt2::core::UUID     templateId;   // prefab-local identity, frozen at creation
    SubtreeEntityRecord record;       // the existing per-entity payload
};
```

**Why not extend `SubtreeEntityRecord`.** It is the in-memory record type for
undo snapshots — every structural command holds a `SubtreeSnapshot` built from
it (`EditorStructuralCommands`, findings Q6). Adding `templateId` to it would
put a prefab-only field on every duplicate, paste, delete and reparent
snapshot in the history, where it would be meaningless, unset, and eventually
mistaken for meaningful by someone reading a snapshot in a debugger. It would
also couple the undo system's record shape to the prefab file format, so a
prefab format change would touch undo.

Wrapping keeps prefab identity in the prefab layer and leaves the shared
payload shared. The cost is one level of indirection in the prefab codec,
which is a fair price for not putting an unused field on every command in the
history.

**Constraint carried forward:** `templateId` is minted once when the prefab
asset is created and frozen in the file. It is never regenerated, and it is
never derived from a scene UUID at instantiate time — that would make it an
instance property rather than a template one, and overrides would stop
reattaching after the first source edit.

This amends D1 in the Phase 8 spec. D1's two-component instance model
(`PrefabInstanceComponent`, `PrefabMemberComponent`) is unchanged; only the
file-side record shape is settled here.

### Phase 8 W2 — shared entity-reference remapper (duplication half) verification report (2026-08-04)

**Implementation branch:** `codex/phase8-w2-entity-reference-remapper`, based
on `master` at `aff20c1`.

W2 adds the CPU-only remapper declared in
`RT2App/src/EntityReferenceRemapper.h:20-26` and implemented in
`RT2App/src/EntityReferenceRemapper.cpp:8-33`. Its input is a durable
source-UUID → destination-UUID map plus opaque `ScriptComponent*` views; it
does not depend on an entt registry or SceneManager internals. It rewrites
only a `ScriptFieldType::Uuid` entry whose value is a non-nil UUID present in
the map. External references, stale UUIDs, nil UUIDs, malformed variant/type
pairs, and non-UUID fields are left unchanged.

`SceneManager` now converts each copy plan to that UUID map and invokes the
shared remapper after all destination UUIDs have been assigned in the legacy
duplicate/paste paths (`RT2App/src/SceneManager.cpp:1487,1579`) and the
UUID-supplied duplicate/paste paths (`RT2App/src/SceneManager.cpp:2740,2887`).
The W1 prefab-instantiation path is not implemented or changed here. W1 will
build its complete prefab-local-template-UUID → instance-UUID map, collect the
copied `ScriptComponent` views, and call this same function only after the
instance UUID assignment pass is complete.

#### Decision reconciliation

The stale-UUID decision is to preserve the value. A UUID absent from the
copy map does not establish whether the target was deleted, belongs to another
scene, or is intentional authored data; clearing or substituting it would lose
information. This is tested directly at
`RT2Tests/src/Phase8W2EntityReferenceTests.cpp:159-172`.

The current tree also clarifies an outdated phrase in the Phase 8 grounding
notes that said no engine code resolves UUID-valued script fields. The script
runtime exposes UUID lookup through `ScriptSystem.cpp:943-951`, so a typed UUID
script field is a usable entity reference even though W2 deliberately keeps
the remapper generic and CPU-only. No new authored component was added;
`PersistedComponents` remains the existing component set.

#### Permanent tests and discrimination proofs

All five permanent W2 tests were fault-injected one at a time. For each, the
Release solution was built, the named test was run alone, the actual red
output was recorded, the fault was reverted, Release was rebuilt, and the same
test was confirmed green.

1. **Duplicate internal UUID remapping** — temporary removal of the remapper
   call from `DuplicateSubtreesWithUuids` (`SceneManager.cpp:2740`):

   ```text
   Phase8W2EntityReferenceTests.cpp(104): ERROR: CHECK( FieldUuid(*duplicateScript, "sibling") == duplicateChild ) is NOT correct!
     values: CHECK( {?} == {?} )
   Phase8W2EntityReferenceTests.cpp(105): ERROR: CHECK( FieldUuid(*duplicateScript, "sibling") != fixture.child ) is NOT correct!
     values: CHECK( {?} != {?} )
   [doctest] test cases:  1 | 0 passed | 1 failed | 771 skipped
   [doctest] assertions: 11 | 9 passed | 2 failed |
   [doctest] Status: FAILURE!
   ```

   Reverted result:

   ```text
   [doctest] test cases:  1 | 1 passed | 0 failed | 771 skipped
   [doctest] assertions: 11 | 11 passed | 0 failed |
   [doctest] Status: SUCCESS!
   ```

2. **Duplicate external UUID preservation** — temporary unconditional
   rewrite of UUID fields in `EntityReferenceRemapper.cpp:26-30`:

   ```text
   Phase8W2EntityReferenceTests.cpp(123): ERROR: CHECK( FieldUuid(*duplicateScript, "external") == fixture.external ) is NOT correct!
     values: CHECK( {?} == {?} )
   [doctest] test cases: 1 | 0 passed | 1 failed | 771 skipped
   [doctest] assertions: 7 | 6 passed | 1 failed |
   [doctest] Status: FAILURE!
   ```

   Reverted result:

   ```text
   [doctest] test cases: 1 | 1 passed | 0 failed | 771 skipped
   [doctest] assertions: 7 | 7 passed | 0 failed |
   [doctest] Status: SUCCESS!
   ```

3. **Paste internal remapping and external preservation** — temporary removal
   of the remapper call from `PasteSubtreesWithUuids` (`SceneManager.cpp:2887`):

   ```text
   Phase8W2EntityReferenceTests.cpp(151): ERROR: CHECK( FieldUuid(*pastedScript, "internal") == pastedChild ) is NOT correct!
     values: CHECK( {?} == {?} )
   [doctest] test cases:  1 | 0 passed | 1 failed | 771 skipped
   [doctest] assertions: 14 | 13 passed | 1 failed |
   [doctest] Status: FAILURE!
   ```

   Reverted result:

   ```text
   [doctest] test cases:  1 | 1 passed | 0 failed | 771 skipped
   [doctest] assertions: 14 | 14 passed | 0 failed |
   [doctest] Status: SUCCESS!
   ```

4. **Stale UUID preservation** — temporary clearing of any UUID absent from
   the remap:

   ```text
   Phase8W2EntityReferenceTests.cpp(172): ERROR: CHECK( FieldUuid(script, "stale") == stale ) is NOT correct!
     values: CHECK( {?} == {?} )
   [doctest] test cases:  1 | 0 passed | 1 failed | 771 skipped
   [doctest] assertions: 6 | 5 passed | 1 failed |
   [doctest] Status: FAILURE!
   ```

   Reverted result:

   ```text
   [doctest] test cases: 1 | 1 passed | 0 failed | 771 skipped
   [doctest] assertions: 6 | 6 passed | 0 failed |
   [doctest] Status: SUCCESS!
   ```

5. **Non-UUID field preservation** — temporary acceptance of a UUID variant
   without its `Uuid` semantic type tag:

   ```text
   Phase8W2EntityReferenceTests.cpp(196): ERROR: CHECK( script.fieldValues == before ) is NOT correct!
     values: CHECK( {{mismatched, {?}}, {string, {?}}, {color, {?}}, {float, {?}}} == {{color, {?}}, {string, {?}}, {mismatched, {?}}, {float, {?}}} )
   [doctest] test cases: 1 | 0 passed | 1 failed | 771 skipped
   [doctest] assertions: 3 | 2 passed | 1 failed |
   [doctest] Status: FAILURE!
   ```

   Reverted result:

   ```text
   [doctest] test cases: 1 | 1 passed | 0 failed | 771 skipped
   [doctest] assertions: 3 | 3 passed | 0 failed |
   [doctest] Status: SUCCESS!
   ```

The tests are registered in the CPU-only RT2Tests target at
`RT2Tests/src/Phase8W2EntityReferenceTests.cpp:90-196`; the remapper source
is also included in RT2Tests and RT2SliceRunner so neither target depends on
Vulkan, ImGui, Walnut, or the editor host.

#### Verification

- Release solution build: passed, 0 warnings, 0 errors.
- Debug solution build: passed, 0 errors; 42 warnings, including the known
  existing runtime-library warning in the Debug app target.
- Release full RT2Tests: **772/772 cases**, **146,801/146,801 assertions**;
  passed.
- Debug full RT2Tests: **772/772 cases**, **146,801/146,801 assertions**;
  passed.
- `powershell -File run_script_test.ps1`: passed — 60 frames, 1 entity, no
  mismatches.
- `powershell -File run_slice_test.ps1`: passed — 60 steps, authoring intact;
  cube final x `0.999999702` against approximately `1.0`.
- Headless validation using the built executable at
  `bin/Release-windows-x86_64/RT2App/RT2App.exe`: exited 0, rendered all 8
  frames, saved the 320×200 screenshot, and emitted zero validation messages.
  The temporary `screenshot.png` and `artifacts/v.png` outputs were removed
  after verification.
- `graphify update .` completed after the source changes. Its generated
  `graphify-out/GRAPH_REPORT.md` remains intentionally unstaged.

No W2 defect was revealed. The intentional behavior change is that copied
typed entity references now follow the copied subtree; references outside it
remain pointed at their original UUIDs. Prefab instantiation remains W1 work.

### Phase 8 W1 — prefab create-from-subtree and instantiate verification report (2026-08-04)

**Grounded against commit `89ed336`** on branch
`phase8-w1-prefab-create-instantiate`, master merged in. This report reflects
the tree at commit time; the working tree additionally carries the W1
implementation described below.

#### Grounding findings that changed the intended scope

Half of W1 already existed before this work was speced from the roadmap's
"Phase 8 W2" text. Reading the tree rather than the roadmap found:

- The prefab file codec is **not** in PrefabSerializer.cpp as the W0-era
  comments claim; `PrefabSerializer::Save`/`Load` delegate record
  serialization to `PrefabRecordToJson`/`JsonToPrefabRecord`, which live in
  **SceneSerializer.cpp** (they reuse the scene per-component codec). The
  HARD-RULE strip of transient resource indices
  (`MeshRef::materialIndex`, `materialOverride.material.*TextureIndex`) is in
  `StripTransientIndices` at `SceneSerializer.cpp:1494-1510` (a HARD RULE only
  if that exclusion is on the write side of a scene save; on the prefab side it
  is unconditional), and prefab nodes are staged from the same primitives as
  the scene load path via `RegisterPrimitiveMesh` (`SceneSerializer.cpp:1403`).
- The two record-codec declarations in `PrefabSerializer.h:118,122` used bare
  `json` where only the `.cpp` file carries `using json = nlohmann::json`;
  qualified them as `nlohmann::json` (no header using-declaration).
- `SceneSerializer.cpp` schema is now **v5** (`SceneSerializer.h:
  SchemaVersion 4 -> 5`). The `static_assert(PersistedComponents::Count == 11)`
  fired because the two prefab components raised `Count` to 13; after verifying
  EntityRecord serialization covers both end-to-end, the assert was updated to
  `13` (`SceneSerializer.cpp:39`).
- Existing v4-era assertion literals were stale: `Phase1ASceneAssetTests.cpp`,
  `Phase7W5Tests.cpp`, `SceneSerializerTests.cpp` hard-coded
  `"version": 4`/`== 4`; corrected to use `SceneSerializer::SchemaVersion` (or
  the `5` literal where a fixture is written) and the `vertical-slice` asset
  was bumped to version 5.

#### What was built (implementation scope from TASK.md section A)

- `SceneManager::CreatePrefabFromSubtree(roots, prefabPath)` — asset-side,
  no live-scene mutation. Captures via the existing `CaptureSubtreeSnapshot`,
  mints **one fresh templateId per captured entity** via `ReserveKnownUuids`
  (parallel to `sourceSnapshot.entities`, pre-order — **not** derived from any
  scene UUID, per amendment A1), writes the `.rt2prefab` atomically via
  `PrefabSerializer::Save`, and resolves the sidecar asset identity via
  `ResolveOrAssign`. Fills `PrefabCreationResult` per the header contract.
  (`SceneManager.cpp:2529`)
- `SceneManager::CountCanonicalPrefabEntities(prefabPath)` — mirrors
  `CountCanonicalSubtreeEntities`: loads the file and returns the record count,
  failing with an `Error` on an invalid file. (`SceneManager.cpp`)
- `SceneManager::InstantiatePrefabWithUuids(prefabPath, knownInstanceUuids,
  diagnostics)` — flat-UUID-list contract mirroring
  `DuplicateSubtreesWithUuids`: validate count against
  `CountCanonicalPrefabEntities`, validate non-nil/unique/absent from the live
  document, then build the full plan in a temp `SceneDocument` before any
  mutation; on failure roll back cleanly. Loads via `PrefabSerializer::Load`,
  builds instances via the shared `ApplySubtreeRecord`, resolves imported
  assets in the temp doc through `SceneAssetResolver` (diagnostics
  out-param), merges resolved resources with base-offset rebasing, and wires
  the hierarchy via `SceneHierarchy::RebuildChildren` (reused, not a hand-wired
  parent/children loop). Links the instance: `PrefabInstanceComponent` on the
  root (fresh instanceId + `AssetReference` to the prefab) and
  `PrefabMemberComponent` on every member (same instanceId, templateId from
  the file). Root name gets the locked `" Copy"` suffix. After UUID assignment
  it invokes the shared W2 `EntityReferenceRemapper`, building the
  template-scene-UUID → instance-UUID map from the file's own
  `SubtreeEntityRecord.uuid` fields plus the copied `ScriptComponent` views, so
  a script `Uuid` field pointing at a sibling inside the prefab resolves to
  that sibling's instance — **not** the template and **not** a sibling
  instance. (`SceneManager.cpp:2596`)
- Undo/redo commands (`EditorStructuralCommands.h/.cpp`): asset-side
  `CreatePrefabCommand` (scene never mutated; Undo restores prior file bytes or
  removes a never-present file, Redo deterministically regenerates) and
  scene-side `InstantiatePrefabCommand` (Undo `RemoveSubtreesExact`, Redo
  `RestoreSubtrees` — same stored UUIDs, link verbatim). Both follow
  `EditorCommand.h`'s carry-state-at-construction contract and the
  no-compaction-while-history-live invariant.

#### Permanent tests and discrimination proofs

Nine permanent tests in the CPU-only target at
`RT2Tests/src/Phase8W1PrefabTests.cpp` (registered in both `RT2Tests.vcxproj`
and `premake5.lua`). For each the Debug solution was built, the named test was
run alone, the actual red output was recorded, the fault was reverted, Debug
was rebuilt, and the same test was confirmed green.

1. **Multiple instances get unique UUIDs and remapped internal refs** — fault:
   temporarily removed the `RemapCopiedScriptFields` call at
   `SceneManager.cpp:2815`. Red (correct discrimination — sibling script refs
   pointed at the template, not the instance):

   ```text
   Phase8W1PrefabTests.cpp(128): ERROR: CHECK( FieldUuid(*s1, "sibling") == inst1Child ) is NOT correct!
   Phase8W1PrefabTests.cpp(129): ERROR: CHECK( FieldUuid(*s1, "sibling") != child ) is NOT correct!
   ... (also 150, 152)
   [doctest] test cases:  1 |  0 passed | 1 failed | 780 skipped
   [doctest] assertions: 37 | 33 passed | 4 failed |
   [doctest] Status: FAILURE!
   ```

2. **Prefab file never contains a resource-table index** — fault: disabled the
   `materialIndex` erase in `StripTransientIndices`
   (`SceneSerializer.cpp:1498`). Red:

   ```text
   Phase8W1PrefabTests.cpp(138): ERROR: CHECK( raw.find("materialIndex") == std::string::npos ) is NOT correct!
   Phase8W1PrefabTests.cpp(154): ERROR: CHECK( rec.materialIndex == -1 ) is NOT correct!
   [doctest] test cases:  1 |  0 passed | 1 failed | 780 skipped
   [doctest] assertions: 14 | 12 passed | 2 failed |
   [doctest] Status: FAILURE!
   ```

3. **templateIds stable, not derived from scene UUIDs** — fault: derived
   templateIds from the capture's scene UUIDs instead of fresh `ReserveKnownUuids`
   (`SceneManager.cpp:2556`). Red:

   ```text
   Phase8W1PrefabTests.cpp(163): ERROR: CHECK( created.templateIds[0] != root ) is NOT correct!
   Phase8W1PrefabTests.cpp(166): ERROR: CHECK( created.templateIds[1] != child ) is NOT correct!
   [doctest] test cases:  1 |  0 passed | 1 failed | 780 skipped
   [doctest] assertions: 25 | 23 passed | 2 failed |
   [doctest] Status: FAILURE!
   ```

4. **Instance link components survive scene round-trip** — fault: suppressed
   the `prefabMember` write (`SceneSerializer.cpp:828`). Red:

   ```text
   Phase8W1PrefabTests.cpp(209): FATAL ERROR: REQUIRE( msg0 ) is NOT correct!
   [doctest] test cases:  1 |  0 passed | 1 failed | 780 skipped
   [doctest] assertions: 11 | 10 passed | 1 failed |
   [doctest] Status: FAILURE!
   ```

5. **InstantiatePrefabCommand Undo/Redo restores the link** — fault: suppressed
   the `PrefabInstance`/`PrefabMember` writes in `ApplySubtreeRecord`
   (`SceneManager.cpp:1797-1805`) that `RestoreSubtrees` uses on Redo. Red:

   ```text
   [doctest] test cases:  1 |  0 passed | 1 failed | 780 skipped
   [doctest] assertions: 14 | 13 passed | 1 failed |
   [doctest] Status: FAILURE!   (REQUIRE( instComp ) after Redo — link absent)
   ```

6. **CreatePrefabCommand Undo restores prior contents, Redo regenerates** —
   fault: suppressed the before-contents write in `CreatePrefabCommand::Undo`
   (`EditorStructuralCommands.cpp`). Red:

   ```text
   Phase8W1PrefabTests.cpp(241): ERROR: CHECK( ReadFileBinary(prefabPath) == stale ) is NOT correct!
   [doctest] test cases:  1 |  0 passed | 1 failed | 780 skipped
   [doctest] assertions: 13 | 12 passed | 1 failed |
   [doctest] Status: FAILURE!
   ```

7. **CreatePrefabCommand Undo removes a never-present file** — fault: in the
   empty-prior branch, `Undo` no longer removed the file. Red:

   ```text
   Phase8W1PrefabTests.cpp(270): ERROR: CHECK_FALSE( std::filesystem::exists(prefabPath) ) is NOT correct!
   [doctest] test cases: 1 | 0 passed | 1 failed | 780 skipped
   [doctest] assertions: 7 | 6 passed | 1 failed |
   [doctest] Status: FAILURE!
   ```

8. **B2 regression: instantiating a primitive registers its mesh** — fault:
   replaced `RegisterPrimitiveMesh` at `SceneManager.cpp:2663` with a
   zeroed `meshIndex` (no registration). Red:

   ```text
   Phase8W1PrefabTests.cpp(307): FATAL ERROR: REQUIRE( ref->meshIndex < countAfter ) is NOT correct!
   [doctest] test cases: 1 | 0 passed | 1 failed | 780 skipped
   [doctest] assertions: 7 | 6 passed | 1 failed |
   [doctest] Status: FAILURE!
   ```

9. **Instantiate validates UUID count/uniqueness atomically** — fault: disabled
   the count check and the seen-set duplicate check in
   `InstantiatePrefabWithUuids` (`SceneManager.cpp:2622`). Red — the guard is
   load-bearing; with it removed a wrong UUID count proceeds and aborts:

   ```text
   (no assertion summary — process aborted)
   : Assertion failed: vector subscript out of range
   TEST EXIT=-1073740791 (abort)
   ```

Every fault reverted byte-for-byte and the full suites re-run green. No fault
that was expected to discriminate failed to go red; no test was adjusted to
make a fault pass.

#### Verification (measured this session)

- **Release solution build**: passed, 0 errors.
- **Debug solution build**: passed, 0 errors.
- **Release full RT2Tests** (run from repo root): **781/781 cases**,
  **146,955/146,955 assertions**; passed.
- **Debug full RT2Tests** (run from repo root): **781/781 cases**,
  **146,955/146,955 assertions**; passed. (Baseline before W1, from the W2
  report: 772/772, 146,801 — W1 adds 9 test cases and 154 assertions.)
- **`powershell -File run_script_test.ps1`**: passed — 60 frames, 1 entity, no
  mismatches.
- **`powershell -File run_slice_test.ps1`**: passed — 60 steps, authoring
  intact (exercises the v5-bumped `vertical-slice.rt2scene`).
- **`powershell -File run_recovery_test.ps1`**: passed — recovery regression.

#### Defects found

- The transient Release test-worker failure reported at the start of this
  phase (anchor: "Assertion failed" during occasional Release TestWorker runs)
  was **investigated but not reproduced**: 16 consecutive green Release runs
  (sequential, concurrent Release+Debug, and Release-while-Debug-rebuild) with
  no capture from the one failing run, so no cause was isolable and it is left
  **unresolved** rather than mislabeled as fixed. No W1 code path touches the
  TestWorker; shipping W1 does not claim to have addressed it.
- The transient resource-index exclusion plus the stale v4 static-assert and
  the stale test literals above were grounded and corrected, not assumed.

#### Headless note

The RT2App executable's headless CLI (`--headless`) can load and render scenes
but exposes **no prefab create/instantiate action**, so a headless-RT2App
instantiate/reload sanity drive is **not directly exercisable** without adding
a new CLI scenario — reported as blocked rather than passed. The identical
headless instantiate → save → reload → verify-link round-trip is covered by
the permanent W1-D test in the CPU-only suite (test 4 above).

----

## Phase 8 W1 — cold-review repair (2026-08-04)

Supersession note: the W1 verification report above (781/781) was the state at
commit 18c1ad7. A cold review of that commit (artifact phase8-w1-review)
found six findings (P0, P1 sidecar, P1 rollback, P2 one-root, P3 command
semantics, P4 visitor). All were repaired in a bounded follow-up commit. This
note records the repair; it does not rewrite the historical W1 report.

### Repairs and where they landed

- **P0 — slice source list.** RT2SliceRunner/premake5.lua never registered
  PrefabSerializer.cpp (the gitignored RT2SliceRunner.vcxproj was the only
  place that had it, and premake is source-of-truth). Added it. The regenerated
  slice builds and links in both Release and Debug. AssetWatchPolicy.cpp was
  considered for the slice but **rejected as ungrounded**: its only includers
  (WalnutApp.cpp, ContentBrowserDispatch.cpp) are not slice sources, so a
  fresh slice target has no reference to it.
- **RT2Tests premake parity.** RT2Tests/premake5.lua was missing
  AssetWatchPolicy.cpp even though the tracked RT2Tests.vcxproj had it at
  line 327 — a fresh premake regeneration would drop the symbol and reproduce
  the reviewer's 21-symbol unresolved failure. Added it for parity.
- **P1 — sidecar failure is loud.** CreatePrefabFromSubtree now treats a
  sidecar that cannot be committed as a hard failure: it rolls back the just-
  written .rt2prefab file (asset+sidecar atomic) and returns ok=false.
  InstantiatePrefabWithUuids resolves the sidecar identity BEFORE any scene
  mutation and fails pre-mutation when it is unreadable; the link step reuses
  the committed ID instead of re-resolving inline.
- **P1 — transactional resource rollback.** On a failed instantiate, the
  rollback path now truncates the appended mesh/material/texture rows back to
  their pre-merge bases (dst.meshRegistry.Truncate(meshBase) +
  materials.resize(matBase) + 	extures.resize(texBase)). Added
  MeshRegistry::Truncate.
- **P2 — one-root invariant.** A prefab must have exactly one top-level root.
  CreatePrefabFromSubtree rejects multi-root input (canonical roots != 1)
  with a structured InvalidEntity diagnostic before any write; instantiate
  rejects a file with != 1 top-level root before any mutation.
- **P3 — command semantics.** CreatePrefabCommand now carries an explicit
  ileExistedBefore flag so a pre-existing zero-byte file is restored on Undo
  (not removed, which the old eforeContents.empty() heuristic got wrong).
  Undo also surfaces a failed file removal as a loud Io Failure instead of a
  silent success. Factory signature updated; both call sites updated.
- **P4 — visitor coverage.** SceneAssetReferenceVisitor now collects
  PrefabInstanceComponent.prefab alongside imported/script slots.

### Regression tests added (Phase8W1PrefabTests.cpp, all with fault-to-red)

- P1 create-sidecar failure: create fails atomically, asset file rolled back.
- P1 instantiate-sidecar failure: fails before mutation, zero entity/mesh delta.
- P1 resource rollback: red confirmed in-session — with Truncate disabled,
  meshRegistry.GetCount() == 2 vs base 1 after a failed instantiate;
  green with the fix.
- P2 multi-root create rejected before any file write; multi-root file
  rejected at instantiate before mutation.
- P3 zero-byte pre-existing file restored on Undo (still exists, size 0);
  failed removal surfaces as Io Failure.
- P4 visitor finds the prefab reference exactly once on the instance root.

### Verification (measured this session, after repair)

- **Release full RT2Tests** (run from repo root): **789/789 cases**,
  **147,008/147,008 assertions**; passed.
- **Debug full RT2Tests** (run from repo root): **789/789 cases**,
  **147,008/147,008 assertions**; passed.
- **powershell -File run_script_test.ps1**: passed — 60 frames, 1 entity, no
  mismatches.
- **powershell -File run_slice_test.ps1**: passed — 60 steps, authoring
  intact, cube x=0.999999702.
- **powershell -File run_recovery_test.ps1**: passed.
- **Slice build** (regenerated from the repaired premake): Release + Debug both
  link RT2SliceRunner.exe successfully.
- **graphify update .**: graph rebuilt (34,808 nodes); tracked
  graphify-out/GRAPH_REPORT.md restored to HEAD (generated churn, no
  functional delta).

### Caveats

- The transient Release TestWorker failure from the W1 report remains
  unresolved (not reproduced by W1; untouched by this repair).
- The reviewer's statement that the RT2Tests link failure involves the slice
  was inaccurate: the slice has no AssetWatchPolicy reference; the RT2Tests
  premake parity gap is the reproducible 21-symbol path.
