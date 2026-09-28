# Phase 9 — Jolt physics grounding findings

Grounded 2026-08-04 against commit
`0df3175acfb150cdc411e1ddac79d3a3b509bcf8`. This is a
read-only grounding report, not the Phase 9 implementation spec. Recommendations
are deliberately not recorded as settled decisions.

## Evidence labels

- **Code-verified** — established from the grounded tree. Each finding cites a
  `file:line`.
- **Inferred / recommendation** — proposed from the code-verified constraints;
  it still needs design review.
- **Jolt-doc-dependent** — an exact answer depends on the Jolt revision that RT2
  pins. No external Jolt documentation was consulted for this pass, so these
  points must be verified against primary sources for that revision before code.

## Headline findings

1. **There is no Jolt dependency or physics implementation today.** The complete
   submodule list contains Walnut, EnTT, Lua, sol2, and efsw only
   (`.gitmodules:1-16`); RT2App's include/library lists contain no Jolt entry
   (`RT2App/premake5.lua:200-239`), and the generated app project has neither a
   Jolt include nor library (`RT2App/RT2App.vcxproj:82,92`). The only physics
   material in the tree is future prose/placeholders
   (`docs/game-loop.md:378-411`; `docs/future-extensions.md:31-125`).
2. **The existing runtime controller, not `PhysicsSystem`, must continue to own
   fixed time.** It already owns the single accumulator, 1/60 s tick, 0.25 s
   clamp, five-substep cap, residual-drop policy, Pause/Resume reset, and exact
   Step tick (`RT2App/src/RuntimeSceneController.h:76-81,116-126,251-264`;
   `RT2App/src/RuntimeSceneController.cpp:85-105,112-181,242-328`). A second
   physics accumulator would split script and physics time.
3. **The advertised `Transform` sync is not cheap and its written contract is
   false.** The interface says “instance transform buffer only (no AS rebuild)”
   (`RT2App/src/ISceneRenderBridge.h:17-21,48-50`), but the implementation
   performs `vkDeviceWaitIdle`, synchronously destroys/recreates the TLAS,
   rebuilds transform and both light buffers, updates descriptors, and
   invalidates ReSTIR history (`RT2App/src/RendererGPU.cpp:563-575`;
   `RT2App/src/SceneResources.cpp:596-644`;
   `RT2App/src/AccelerationStructure.cpp:824-950`). The CPU DTO path also
   re-walks every emissive triangle to rebuild triangle lights
   (`RT2App/src/GPUSceneData.cpp:444-496,504-594`).
4. **That costly path runs every Playing frame even when nothing moved.** The
   runtime controller unconditionally calls `UpdateInstancesFromECS` and
   `TransformSync` on every non-structural frame
   (`RT2App/src/RuntimeSceneController.cpp:304-325`). Phase 9 must add measured
   dirty/impact ownership or explicitly accept this cost; it cannot treat the
   current path as a solved foundation.
5. **Authored physics configuration and live Jolt state must be separate.** The
   plan already requires physics bodies/handles to be runtime-only and never
   copied back (`docs/game-engine-development-plan.md:153-167`). The existing
   pattern keeps durable component data in the document and live Lua state off
   it (`RT2App/src/ECSComponents.h:272-303`), while `CloneInMemory` explicitly
   excludes physics/runtime state (`RT2App/src/SceneSerializer.h:58-64`). Jolt
   body IDs, shape handles, contact state, interpolation history, and query
   scratch therefore must not appear in a persisted ECS component.
6. **The singular lifecycle seam is already occupied by scripting.** The
   controller stores one `IRuntimeLifecycleObserver*`
   (`RT2App/src/RuntimeSceneController.h:163-180,257-263`), and the host installs
   `ScriptSystem` there (`RT2App/src/WalnutApp.cpp:2490-2495`). Physics needs a
   distinct ordered runtime-system seam (or a reviewed composite dispatcher),
   not a second call to `SetLifecycleObserver` that silently replaces scripts.
7. **The Phase 9 stub is under-scoped.** It does not address the persisted
   `MotionComponent` that already writes transforms in the fixed slot
   (`RT2App/src/ECSComponents.h:175-182`;
   `RT2App/src/RuntimeSceneController.cpp:565-600`), the schema-version
   handoff from Phase 8,
   Phase-8 prefab component codecs, physics-aware runtime spawning, lifecycle
   teardown hooks, interpolation storage, or the real TLAS cost. Those are
   prerequisites, not polish.

## Prominent contradictions and stale claims

| Conflict | Code-grounded resolution for the Phase 9 spec |
|---|---|
| `docs/future-extensions.md` recommends starting with a built-in physics engine (`docs/future-extensions.md:82-101`), while the roadmap requires Jolt (`docs/game-engine-development-plan.md:620-645`). | Treat the roadmap's Jolt requirement as current intent; retire or supersede the older future-extension recommendation when the spec is written. |
| The canonical runtime is 1/60 s with one controller accumulator (`RT2App/src/RuntimeSceneController.h:76-81,251-264`), while the physics extension sketch proposes a separate accumulator and “e.g. 1/120 s” (`docs/game-loop.md:400-411`). | Recommend one controller-owned 1/60 s clock initially. A multi-rate solver is a separate, measured decision, not the default. |
| `SyncImpact::Transform` promises no TLAS rebuild (`RT2App/src/ISceneRenderBridge.h:17-21`), while `RebuildTLASOnly` destroys and rebuilds the TLAS (`RT2App/src/AccelerationStructure.cpp:845-950`). | Correct the contract and measure the actual transform presentation path before setting dynamic-body budgets. |
| `docs/scene-management.md` describes schema v3 (`docs/scene-management.md:285-347`), but the grounded code writes v4 and reads v3-v4 (`RT2App/src/SceneSerializer.h:85-126`). Phase 8 assigns the v4→v5 decision to the first serialized prefab-reference work (`docs/game-engine-development-plan.md:12782-12786,13060-13062`), and the active W1 integration has selected v5. | Reverify the merged Phase 8 schema before Phase 9 starts, then allocate the **next** schema version for physics (expected v6 if W1 lands as designed). Do not reuse v5 for two independently evolving formats. |
| The roadmap's current Test baseline says 700/700 (`docs/game-engine-development-plan.md:6170-6196`), while `AGENTS.md` says 554/554 and eight Debug failures (`AGENTS.md:69-81`). | Both are stale at this commit. Direct builds/runs measured **772/772, 146,801 assertions** in both Release and Debug. See “Verification performed” below. |
| `Transform` sync timings exposed in the Performance window are populated only by full/async AS builds (`RT2App/src/SceneResources.cpp:278-331,420-513`; `RT2App/src/WalnutApp.cpp:683-691`), not the per-frame TLAS-only path (`RT2App/src/SceneResources.cpp:596-644`). | Add separate transform-sync CPU wait, TLAS-only GPU/CPU, DTO/light rebuild, and buffer-upload metrics before physics acceptance. |
| `RebuildTLASOnly` returns `bool` (`RT2App/src/AccelerationStructure.cpp:824-833,949-950`), but `UpdateInstances` ignores it (`RT2App/src/SceneResources.cpp:635-637`). | This is an existing swallowed-return example. Phase 9 must not repeat it with Jolt body/shape creation, add/remove, step, or cache writes. |
| The Phase-8 prefab serializer currently rejects every non-empty record list (`RT2App/src/PrefabSerializer.h:13-38`; `RT2App/src/PrefabSerializer.cpp:78-99`). | Physics component persistence must join the shared scene/subtree/prefab codec after the Phase-8 record codec exists; do not create a physics-only divergent serializer. |

## Recovered commitments deferred into or constraining Phase 9

- The exact `grep -n "Phase 9"` recovery yields the roadmap entry and one
  explicit Phase-4 deferral: physics was excluded while the runtime lifecycle
  and mutation queue were built for later systems
  (`docs/game-engine-development-plan.md:620-669,2573-2584`).
- Cross-phase rules require stable UUID/asset identity rather than EnTT values
  or vector indices, centralized scene mutations, narrow runtime services, and
  CPU-only physics mapping tests
  (`docs/game-engine-development-plan.md:104-119,128-167`).
- Fixed-step systems must define stable iteration and event ordering and must
  not inherit unordered-container or scheduler order
  (`docs/game-engine-development-plan.md:169-175`).
- Phase 6 explicitly deferred “later physics queries” to the validated script
  surface and forbids exposing EnTT, raw pointers, renderer, or window
  (`docs/game-engine-development-plan.md:488-506`).
- Phase 6 deliberately installed fixed script dispatch before the future
  physics slot and variable dispatch after the structural safe point
  (`docs/game-engine-development-plan.md:3520-3528,3848-3869`).
- Phase 5 did not defer a collision feature by name. Its relevant commitment is
  the CPU-only `IInputService` already passed through runtime lifecycle
  (`RT2App/src/InputTypes.h:250-277`); physics gameplay must continue to consume
  logical actions through scripts/services, never GLFW.
- The roadmap requires dynamic rigid motion to use the transform/TLAS path once
  per rendered frame, CPU collision data for correctness, no silent RT-quality
  reduction, and a measured TLAS cost before large dynamic counts
  (`docs/game-engine-development-plan.md:633-645`).

## 1. Dependency and build grounding

### Code-verified

- The workspace includes only three projects from the root Premake file:
  RT2App, RT2Tests, and RT2SliceRunner (`premake5.lua:1-12`). There is no
  physics library target.
- RT2App glob-compiles its own source/vendor C/C++ files and manually excludes
  selected third-party trees (`RT2App/premake5.lua:8-42`). RT2Tests and the
  slice runner manually enumerate the RT2 core `.cpp` files they compile
  (`RT2Tests/premake5.lua:28-66`; `RT2SliceRunner/premake5.lua:9-52`). Adding a
  source only to RT2App would silently leave the CPU gates without it.
- RT2SliceRunner explicitly forbids Walnut, ImGui, GLFW, Vulkan, NRD, and NRI
  (`RT2SliceRunner/premake5.lua:9-11`). Jolt is CPU-only in the proposed
  architecture and must remain usable inside this boundary.
- There is no physics compile definition in the platform/configuration filters
  (`RT2App/premake5.lua:244-297`; `RT2Tests/premake5.lua:71-92`;
  `RT2SliceRunner/premake5.lua:91-112`). Exact Jolt definitions, SIMD choices,
  floating-point options, and CRT compatibility remain **Jolt-doc-dependent**.

### Recommendation (unsettled)

Pin Jolt as a Git submodule and build it once as a dedicated static-library
project. Put the RT2-owned CPU layer in an `RT2Physics` static library (or an
equivalent strictly CPU-only target) linked by all three executables. This
avoids compiling Jolt independently with potentially different flags in each
target and makes the forbidden renderer dependency direction enforceable.
Update Premake and the checked-in/generated project representation together;
the current project files carry hand-maintained source/include lists
(`RT2Tests/RT2Tests.vcxproj:82,254-355`).

Before choosing exact flags, pin a Jolt revision and verify its primary build
documentation for MSVC runtime, exception/RTTI assumptions, SIMD, deterministic
floating-point constraints, asserts, object layers, job system, and allocator
hooks. Those facts are intentionally not guessed here.

## 2. Game-loop timing and insertion point

### Current order (code-verified)

1. Walnut polls events, computes and clamps the outer frame time to 0.25 s,
   then calls layer `OnUpdate` before UI/present
   (`Walnut/Walnut/src/Walnut/Application.cpp:745-779,800-871`).
2. RT2 samples logical input before camera/runtime work
   (`RT2App/src/WalnutApp.cpp:2220-2289`).
3. While Playing, `RuntimeSceneController::Update` runs before autosave and
   render (`RT2App/src/WalnutApp.cpp:2290-2319,2376-2390`).
4. Each fixed tick currently runs UUID-sorted fixed scripts, then UUID-sorted
   `MotionComponent` integration (`RT2App/src/RuntimeSceneController.cpp:565-600`).
5. After all substeps, the controller drains structural operations, syncs
   script environments, runs variable scripts, updates world transforms, does
   one GPU sync, and requests render
   (`RT2App/src/RuntimeSceneController.cpp:267-328`).

Pause and Resume both clear the accumulator; Pause executes no ordinary frame
updates. Step runs exactly one fixed tick without advancing the accumulator,
then drains structural changes, runs one variable script callback at `kFixedDt`,
syncs, and presents (`RT2App/src/RuntimeSceneController.cpp:85-181`). At five
substeps, all residual time is discarded (`RT2App/src/RuntimeSceneController.cpp:247-265`).
Motion and script iteration are UUID-sorted, but the future physics/event order
is not yet defined (`RT2App/src/RuntimeSceneController.cpp:572-600`;
`RT2App/src/ScriptSystem.cpp:469-526`).

### Recommended insertion (unsettled)

`RuntimeSceneController` should remain the only accumulator owner and invoke
physics from `RunFixedTick(kFixedDt)` after fixed scripts. The unresolved
`MotionComponent` policy must be decided first: silently allowing both legacy
motion and a dynamic body creates two transform authorities. Recommend rejecting
that combination and deprecating `MotionComponent` after the primitive drop
slice proves physics; do not silently choose last-writer-wins.

Recommended per-fixed-tick order:

```text
publish exact dynamic poses needed by fixed gameplay
fixed script callbacks (UUID order)
apply queued teleports / kinematic targets (UUID order)
legacy MotionComponent only for entities without a dynamic body
Jolt step
copy exact dynamic results into runtime pose history
collect, canonicalize, sort, and dispatch physics events
```

The existing after-loop structural safe point remains. Physics bodies for new
entities must be created after the ECS batch commits but before scripted
`on_create`; bodies for destroyed entities must be removed after scripted
`on_destroy` but before EnTT destruction. This requires an explicit ordered
physics-dispatch seam because the current safe point knows only scripts and the
mutator (`RT2App/src/RuntimeSceneController.cpp:384-531`).

No interpolation exists today: the accumulator is private and there is no alpha
or physics pose history anywhere (`RT2App/src/RuntimeSceneController.h:220-264`).
Recommend `alpha = accumulator / kFixedDt` remain controller-owned while
`PhysicsSystem` owns previous/current exact body poses. The final spec must
settle whether interpolation writes the runtime ECS presentation transform or
only the renderer DTO; both have script-observation consequences.

## 3. ECS, authored data, schema, and runtime state

### Existing boundaries (code-verified)

- `Transform` is local TRS plus derived current/previous world matrices and a
  dirty bit; `Hierarchy::parent`/`children` use transient EnTT identities
  (`RT2App/src/ECSComponents.h:38-66`). Stable entity identity is the UUID in
  `EntityIdComponent` (`RT2App/src/ECSComponents.h:144-152`).
- The canonical authored-component visitor contains 11 components and is meant
  to make duplication/serialization drift fail together
  (`RT2App/src/PersistedComponents.h:10-36`). In practice, the serializer and
  subtree codec also have explicit per-component record/copy/compare code
  (`RT2App/src/SceneSerializer.cpp:510-644,647-788,1087-1207`;
  `RT2App/src/SubtreeSnapshot.h:43-83`;
  `RT2App/src/SceneManager.cpp:1623-1947`). Physics must update every surface.
- Scene schema is currently v4 and v3 is the minimum readable version
  (`RT2App/src/SceneSerializer.h:123-126`). Unknown future entity blocks would
  be ignored by the v4 reader because it only probes known keys
  (`RT2App/src/SceneSerializer.cpp:825-921`), so writing physics under v4 risks
  old-build data loss.
- `CloneInMemory` copies persisted records but initializes derived/transient
  state afresh (`RT2App/src/SceneSerializer.cpp:1080-1095`), exactly the seam
  physics needs.

### Proposed authored model (recommendation, unsettled)

Use pure-data, handle-free components with stable JSON keys:

| Type | Suggested authored fields | Suggested stable key |
|---|---|---|
| `RigidBodyComponent` | `motionType` (`static`/`kinematic`/`dynamic`), mass policy/value, linear/angular damping, gravity factor, initial linear/angular velocity, sleep and continuous-collision policy | `rigidBody` |
| `ColliderComponent` | shape kind, local offset/rotation, primitive dimensions or durable model/subresource reference, trigger flag, collision layer and mask | `collider` |
| `PhysicsMaterialComponent` | friction, restitution, density (only if density is the chosen mass policy) | `physicsMaterial` |
| `PhysicsSettings` on `SceneDocument` | gravity and the reviewed fixed-step policy | top-level `physics` |

Recommend the next schema version after the merged Phase 8 result (expected
v6 because active W1 has selected v5), retaining the final Phase 8 readable
range with absent physics defaults. The version bump makes a pre-physics binary
reject the file instead of opening it,
discarding unknown physics keys, and resaving. `SceneMetadata` currently embeds
the v4 default and would need to move with the codec
(`RT2App/src/SceneDocument.h:79-89`).

Keep these runtime-only in a `PhysicsSession`/`PhysicsWorldState` owned by
`PhysicsSystem`: Jolt world interfaces, body IDs, shape references, UUID↔body
maps, active contact pairs, pending events, interpolation poses, query scratch,
job/temp allocators, and diagnostics. None belongs in `ECSComponents.h`,
`PersistedComponents`, `SubtreeSnapshot`, prefab records, recovery, or scene
JSON. This follows the explicit no-handle component contract
(`docs/game-engine-development-plan.md:153-167`) and avoids copying stale body
IDs during Play clone, duplicate, undo, or prefab instantiation.

Inline material values are recommended for the first slice. A shared physics
material asset would introduce another Asset→Authoring→Scene translation and
identity scheme before profiling proves reuse is needed.

## 4. Transform authority, hierarchy, scale, teleport, and rendering

### Authority recommendation (unsettled)

| Body mode | Authority while Playing | ECS/Jolt synchronization |
|---|---|---|
| Static | Authored runtime-clone transform at Play start | Build once. Reject runtime movement or require an explicit remove/recreate operation; never silently leave the collider stale. |
| Kinematic | Validated runtime command/script target | Apply target to Jolt at the fixed boundary; publish resulting presentation pose to ECS. |
| Dynamic | Jolt exact pose | Publish Jolt results to ECS/presentation once per rendered frame; generic Transform writes become explicit physics teleports or fail loudly. |

The current script sink writes local TRS directly and only marks ECS dirty
(`RT2App/src/IRuntimeCommandSink.h:74-90`;
`RT2App/src/ScriptSystem.cpp:1465-1513`). Phase 9 must interpose a
physics-aware runtime mutation service for bodies. Recommend an explicit
teleport command with a reviewed velocity policy (`preserve`, `clear`, or
set-explicitly); a plain `set_position` on a dynamic body must not update only
the render transform while leaving Jolt behind.

### Hierarchy and scale (code-verified constraints, recommended policy)

Scene world transforms are derived from local TRS and cached child links
(`RT2App/src/SceneGraph.cpp:21-78`). World-to-local conversion already rejects
singular parents and shear (`RT2App/src/TransformEditing.cpp:36-100`). A parent
with non-uniform scale plus rotation can therefore produce a world transform a
rigid-body backend cannot represent as position/rotation/scale without a policy.

Recommend for the first usable slice:

- dynamic and kinematic bodies are root entities;
- parented static colliders are accepted only when their resolved world matrix
  is finite, non-singular, and shear-free at Play construction;
- all collider scale is strictly positive and validated, never `abs`-repaired;
- sphere scale is uniform; capsule radial axes match; box uses explicit per-axis
  extents; convex/triangle scale policy is deferred to the pinned Jolt shape
  rules and the cooking decision;
- every rejected entity reports UUID, entity name, component/field, and the
  violated restriction, and blocks transactional Play rather than disappearing
  from physics.

Exact Jolt support for scaled/offset/rotated shapes and dynamic concave meshes
is **Jolt-doc-dependent**. The roadmap itself requires parent/non-uniform-scale
restrictions to be validated clearly (`docs/game-engine-development-plan.md:647-656`),
not silently approximated.

### Interpolation and renderer path

`Transform` already carries `worldMatrix`/`prevWorldMatrix` for motion vectors
(`RT2App/src/ECSComponents.h:45-48`), and the GPU DTO copies both
(`RT2App/src/GPUSceneData.cpp:456-480`). Physics interpolation still needs its
own exact previous/current pose history so presentation does not feed an
interpolated dynamic pose back into simulation.

Before choosing ECS-write versus renderer-only interpolation, prototype both
against script semantics:

- ECS-write makes variable scripts/camera follow observe the smooth pose but
  requires exact poses to be republished before the next fixed callback.
- Renderer-only preserves exact ECS state but requires a new CPU-only instance
  transform override seam because `UpdateInstancesFromECS` currently reads
  `Transform::worldMatrix` directly (`RT2App/src/GPUSceneData.cpp:444-480`).

The renderer work is exactly one per rendered frame, not one per substep, but
“`Transform` impact” currently means: rebuild all visible instance DTOs and
emissive triangle lights, wait for the whole device, recreate TLAS and related
buffers, upload transform/light buffers, update descriptors, and invalidate
ReSTIR (`RT2App/src/GPUSceneData.cpp:444-594`;
`RT2App/src/RendererGPU.cpp:563-575`;
`RT2App/src/SceneResources.cpp:596-644`). Phase 9 acceptance must measure each
part separately and must add a no-dirty-frame fast path.

## 5. Runtime and scene lifecycle

### Recommended ordered lifecycle (unsettled)

| Transition | Required physics ownership/action | Existing evidence |
|---|---|---|
| Native open / recovery / document adoption | Parse and validate authored physics only; create no Jolt world. Adoption stays transactional. | `ReplaceAuthoringDocument` adopts a prepared document and advances generations (`RT2App/src/SceneManager.h:70-77`; `RT2App/src/SceneManager.cpp:372-392`). Recovery builds a temporary document before adoption (`docs/scene-management.md:621-635`). |
| Play | Clone, validate every physics record, construct a temporary physics session in UUID order, and commit runtime + physics together. Only then full-sync/render and fire script `OnSceneStart`, so `on_create` queries see bodies. | Current Play clones, full-syncs, enters Playing, then fires the singular observer (`RT2App/src/RuntimeSceneController.cpp:21-78`). This order must gain a failure-capable physics construction slot before activation. |
| Pause / Resume | No simulation while Paused; clear accumulator on both transitions. Preserve physics state. | `RT2App/src/RuntimeSceneController.cpp:85-105`. |
| Step | Run exactly one fixed physics tick, event delivery, safe point, presentation sync, and render; do not consume wall-clock time. | `RT2App/src/RuntimeSceneController.cpp:112-181`. |
| Runtime create | After the ECS batch commits, validate/create body before `SyncScriptEnvironments` fires `on_create`. A failed body creation must roll back the entity batch or fail the whole safe point atomically. | Current order is batch → script environment sync (`RT2App/src/RuntimeSceneController.cpp:267-285`). `RuntimeEntityCreateDesc` currently supports transform/name/visibility/script only (`RT2App/src/RuntimeSceneMutator.h:36-60`). |
| Runtime destroy | Script `on_destroy` while entity/body are queryable; remove body and purge maps/pairs/events; then erase UUID and destroy EnTT subtree. | Current script callback is deliberately before ECS destruction (`RT2App/src/RuntimeSceneController.cpp:483-519`), while the mutator erases UUIDs and registry entities immediately (`RT2App/src/RuntimeSceneMutator.cpp:94-120`). Physics needs an insertion between them. |
| Stop | Disable queues; fire script `OnSceneStop` while physics queries remain valid; destroy the entire physics session; destroy runtime document; restore authoring/full sync. | Current Stop observer precedes runtime reset (`RT2App/src/RuntimeSceneController.cpp:190-236`). |
| Authoring duplicate / undo / redo / prefab | Copy only authored physics values; never copy runtime handles or active contacts. Update exact-state comparison and restore codecs. | `CopyAuthoredComponents` uses the canonical visitor (`RT2App/src/SceneManager.cpp:77-87`), while snapshots still explicitly capture/apply/compare every payload (`RT2App/src/SceneManager.cpp:1623-1947`). |
| Scene clear / replace | No live physics world should be attached to authoring. If a host ever permits replacement while Playing, Stop must complete first. | `SceneManager::Clear` clears the document and advances generations (`RT2App/src/SceneManager.cpp:4091-4097`); editor Play is non-editable (`RT2App/src/WalnutApp.cpp:4015-4041`). |

The `PhysicsSession` should be RAII-owned by `PhysicsSystem`, with one explicit
destroy path used by failed Play construction, normal Stop, host shutdown, and
test teardown. UUID→body and body→UUID mappings must be inserted/erased in the
same checked operation as Jolt body creation/destruction. Never key long-lived
state by raw `entt::entity`; scene entities are transient while UUIDs are the
durable/runtime correlation identity (`docs/glossary.md:27-41`).

Script hot reload must not recreate physics. Successful reload swaps script
environments and cancels timers (`docs/scripting.md:232-285`); event callbacks
in the new environment should receive only events dispatched after the swap.
Model/prefab file watching currently refreshes only the asset database, while
only `.lua` triggers live reload (`RT2App/src/AssetWatchPolicy.cpp:21-30`;
`RT2App/src/WalnutApp.cpp:3747-3770`). Recommend no live collider recook in the
first slice: invalidate derived cache identity on source change and rebuild on
the next Play, with an explicit diagnostic that the current Play session still
uses its committed shapes.

### Silent-failure policy

Physics world construction should be validate-all/build-temporary/commit, like
the existing runtime structural batch. Every Jolt result and cache write must
be checked and translated into the dependency-free `Error`/`Result<T>` channel
(`RT2App/src/core/Error.h:9-17,23-104`). Distinguish “query succeeded with no
hit” from “query could not run.” Do not use the existing script-setter pattern
of returning a frequently ignored `false` for physics mutations; that pattern
is documented as a common silent no-op (`docs/scripting.md:64-111`).

## 6. Collision, trigger, query, and script surface

### Neutral DTO recommendation (unsettled)

Keep Jolt types behind `PhysicsSystem`. A CPU-only public surface should use
RT2 values:

- `PhysicsHit`: entity UUID, point, normal, distance/fraction, subshape index,
  trigger flag, and optional material identifier;
- `PhysicsContactEvent`: `Enter`/`Stay`/`Exit`, canonical UUID pair, trigger
  flag, copied contact points/normals and reviewed impulse/depth fields;
- `PhysicsQueryFilter`: layer/mask, ignored UUIDs, trigger inclusion;
- explicit `Result<optional<PhysicsHit>>` and `Result<vector<...>>` so no-hit is
  not an error.

Do not expose Jolt body/subshape handles to Lua, scene JSON, EnTT, or renderer.
A UUID is 128-bit (`RT2App/src/core/UUID.h:35-61`), so do not assume it fits a
backend user-data integer. Maintain checked maps keyed by the full backend body
identity/generation and validate the mapping on every callback. The exact Jolt
`BodyID` reuse and listener-thread rules are **Jolt-doc-dependent**.

### Deterministic event policy recommendation (unsettled)

Backend contact callbacks may arrive in backend/thread order, which is not the
engine contract. Copy them into an RT2-owned per-step buffer, canonicalize each
pair as `(min(UUID), max(UUID))`, coalesce backend manifold churn into pair-level
state, then sort by pair, event kind, and copied contact key before dispatch.
Compute Enter/Stay/Exit from an RT2 active-pair set so backend callback order is
not observable. Events are immutable values valid for the dispatch only; Lua
receives copied tables, never pointers.

Recommend dispatch immediately after each fixed physics step and before the next
substep. Event callbacks may queue structural operations, but the existing
safe-point rule still defers application until the fixed loop completes
(`RT2App/src/IRuntimeScriptDispatch.h:52-64`;
`RT2App/src/RuntimeSceneController.cpp:267-280`). The final spec must settle
whether later substeps suppress events for UUIDs already pending destruction
and whether destruction emits Exit before `on_destroy`.

Queries belong on a narrow `IRuntimePhysics`/`IPhysicsQueryService` exposed
through the existing validated `world` binding, fulfilling Phase 6's “later
physics queries” commitment without widening `IRuntimeCommandSink` into a Jolt
adapter (`docs/game-engine-development-plan.md:496-502`;
`RT2App/src/IRuntimeCommandSink.h:19-47`). Collision callbacks require either a
neutral `OnPhysicsEvents` addition to the per-frame script dispatcher or a
separate event-consumer interface; both preserve the current fixed callback
signature.

## 7. Primitive shapes, mesh cooking, assets, and context translations

### Current CPU geometry (code-verified)

`MeshData` stores object-space positions/indices/normals/UVs/tangents and
per-triangle material indices in the Scene `MeshRegistry`
(`RT2App/src/MeshRegistry.h:14-44`). `meshIndex` is a transient Scene-context
slot, not durable identity (`RT2App/src/ECSComponents.h:68-75,187-203`). Durable
model identity is `AssetReference.assetId + path + sourceKey + importSettings`
(`RT2App/src/AssetReference.h:102-129`), and the resolver translates source keys
into staged/current mesh slots (`RT2App/src/SceneAssetResolver.cpp:273-310,785-818`).

### Required boundary translations

| Context | Physics meaning | Required translation |
|---|---|---|
| Asset | Model file and subresource identity | Persist `AssetReference` plus `sourceKey`; never persist a cook path or mesh slot. |
| Authoring | Entity UUID + body/collider/material settings | Serializer/prefab/undo own stable values. A collider may reference a model subresource durably. |
| Scene | Resolved `MeshRef::meshIndex` / `MeshData` object-space arrays | At load/Play, resolve durable collider source to the current Scene mesh slot, validate geometry, and cook/build the Jolt shape. The slot is invalidated by import/delete/compaction. |
| GPU | BLAS/TLAS instance indices and device addresses | Never a source of physics correctness. Physics consumes CPU `MeshData`; render sync separately translates ECS presentation transforms to GPU instances. |

These are the glossary's four distinct identity/lifetime contexts
(`docs/glossary.md:27-48`). A collider cache key should include at least the
durable asset ID, source key, import settings, geometry/cook settings, RT2 cook
format version, and pinned Jolt revision. Cache files are generated,
replaceable contents under `cacheRoot`, not new source assets; the project model
already distinguishes durable assetRoot from derived cacheRoot
(`docs/glossary.md:121-136`). Every cache read validates its header/key and
falls back to checked recook; every cache write is atomic and checked.

Recommended rollout: box, sphere, capsule from authored dimensions first;
convex from CPU vertices second; static triangle mesh last. Imported mesh
colliders must be tested after a second import into a non-empty scene because
load-at-index-zero hides missed Scene-index rebases — the exact defect class
recorded by the glossary (`docs/glossary.md:171-186`). Concave/dynamic mesh,
degenerate triangle, vertex-limit, winding, scale, and serialized cooked-shape
constraints are **Jolt-doc-dependent**.

## 8. Testing, diagnostics, profiling, and rollout

### Smallest vertical slice

The first end-to-end increment should be CPU-only and contain exactly:

1. a pinned Jolt static library linked into RT2App, RT2Tests, and
   RT2SliceRunner;
2. next-version handle-free body/collider/material data for root box shapes
   (expected schema v6 after Phase 8's v5);
3. transactional Play construction of one static floor and one dynamic box in
   stable UUID order;
4. one controller-owned 1/60 s fixed step, dynamic pose publication, Pause,
   exact Step, and Stop restoration;
5. a headless N-step drop test proving final pose within tolerance and zero
   surviving bodies/maps after Stop;
6. no collision events, queries, mesh cooking, inspector, or debug draw yet.

This proves the risky dependency/build/lifecycle/authority seam without making
it depend on unbuilt UI. The roadmap's larger drop-and-collision acceptance
then layers on the same CPU route (`docs/game-engine-development-plan.md:647-663`).

### Required test surfaces

- Pure validation/codecs: enum/key stability, pre-physics→physics defaults,
  malformed-field
  isolation versus hard body/collider errors, deterministic output, full
  scene/recovery/subtree/prefab round-trip, duplicate/undo/redo, and proof that
  runtime handles never serialize. Existing component coverage is split across
  the visitor, serializer, and snapshot code and needs fault tests at all three
  (`RT2App/src/PersistedComponents.h:10-36`;
  `RT2App/src/SceneSerializer.cpp:510-644`;
  `RT2App/src/SceneManager.cpp:1623-1947`).
- CPU Jolt integration: stable UUID insertion, filters/masks, primitive shape
  conversion, invalid parent/scale, Pause/Resume/Step, catch-up cap, teleport
  velocity policy, interpolation endpoints, create/destroy in one FIFO batch,
  stale body callback rejection, world cleanup after failed Play/Stop, and
  failure injection for every backend/cache result.
- Determinism: single-thread backend mode first, stable insertion/event order,
  fixed seed/config, N exact controller ticks, documented position/rotation/
  velocity tolerances. Do not claim cross-platform bitwise determinism; the
  engine policy already calls for tolerances (`docs/game-loop.md:239-245`).
- Script API: no-hit versus error, UUID mapping, query filters, enter/stay/exit
  ordering, trigger behavior, callback quarantine, event-triggered deferred
  destruction, and reload while contacts persist.
- Asset/cooking: primitive, convex, static mesh; cache hit/miss/corruption/write
  failure; same model imported twice into a non-empty scene; source change
  invalidation; Scene-slot compaction; no GPU dependency.
- Renderer/interactive: stacked boxes, kinematic platform, trigger, debug draw;
  dynamic-body counts (1/10/100/1000) with separate DTO/light rebuild, device
  wait, TLAS-only, transform/light upload, descriptor, and total frame cost.
  Confirm no BLAS rebuild and no RT-quality downgrade. The current transform
  path is Vulkan-only and cannot be covered by CPU tests
  (`AGENTS.md:79-81`; `RT2App/src/SceneResources.cpp:596-644`).

### Risk-ordered workstreams

1. **Decisions + dependency spike:** pin Jolt, settle flags/threading/determinism,
   build a CPU-only static target into all executables, and prove init/teardown.
2. **Renderer truth/instrumentation:** correct the `Transform` contract, add
   dirty-frame suppression and TLAS-only metrics before physics makes the path
   hot. Preserve current behavior until measurements justify optimization.
3. **Authored data and shared codecs:** settle the next schema version after
   Phase 8, components, material,
   shape source, `MotionComponent` conflict, and Phase-8 prefab integration;
   land fault-driven persistence/duplicate/undo/recovery tests.
4. **Primitive runtime vertical slice:** transactional Play/Step/Stop, root
   static/dynamic boxes, UUID maps, pose authority, cleanup, headless drop.
5. **Transform semantics:** kinematic targets, dynamic teleports, hierarchy and
   scale validation, interpolation, one renderer sync per dirty rendered frame.
6. **Structural lifecycle:** physics-aware runtime spawn/destroy safe point,
   rollback/failure paths, stale callback defense, scene clear/adoption audits.
7. **Filters, queries, triggers, events:** neutral DTOs, stable ordering, Lua
   surface, diagnostics; prove single-thread correctness before optional jobs.
8. **Cooking:** capsule, convex, then static triangle meshes; durable cache
   identity and all Asset/Authoring/Scene translations.
9. **Editor/debug/acceptance:** inspector commands, backend-independent overlay,
   profiling scenes, both build modes, CPU gates, script gate, interactive
   acceptance, and a Phase 9 verification report.

## Open decisions that must be settled before implementation

| ID | Decision and options | Recommendation (not settled) | Evidence / claim class |
|---|---|---|---|
| P9-D1 | Dependency form: pinned submodule/static lib vs package/prebuilt. | Pinned submodule + one static Jolt target + CPU-only RT2Physics wrapper. | Build lists are manual and all three targets need the same core (`.gitmodules:1-16`; `RT2Tests/premake5.lua:28-66`; `RT2SliceRunner/premake5.lua:9-52`). Exact flags are Jolt-doc-dependent. |
| P9-D2 | Fixed clock: reuse 1/60 controller tick, separate 1/120 physics accumulator, or deliberate multi-rate integer subdivision. | Reuse the sole 1/60 controller tick first. | Controller already owns pause/step/catch-up semantics (`RT2App/src/RuntimeSceneController.h:76-81,251-264`). |
| P9-D3 | `MotionComponent` with rigid bodies: allow ordering, translate to kinematic drive, or reject/deprecate. | Reject dynamic+Motion, retain Motion only for non-dynamic legacy fixtures, then deprecate. | Both would write Transform in the same fixed tick (`RT2App/src/ECSComponents.h:175-182`; `RT2App/src/RuntimeSceneController.cpp:565-600`). |
| P9-D4 | Runtime integration: replace singular lifecycle observer, composite it, or add explicit physics dispatch. | Add explicit ordered `IRuntimePhysicsDispatch`/`PhysicsSystem` injection; keep script observer semantics intact. | Singular observer is occupied (`RT2App/src/RuntimeSceneController.h:163-180,257-263`; `RT2App/src/WalnutApp.cpp:2490-2495`). |
| P9-D5 | Scene schema: reuse Phase 8's output version or allocate the next version for physics. | Reverify the merged Phase 8 constants, then allocate the next version (expected v6 after W1's v5), retain the final supported read range, and default absent physics. | Grounded v4 readers ignore unknown component blocks (`RT2App/src/SceneSerializer.cpp:825-921,1628-1645`), while Phase 8 makes its own v4→v5 decision when prefab references first serialize (`docs/game-engine-development-plan.md:12782-12786,13060-13062`). |
| P9-D6 | Physics material: inline component vs shared resource/asset. | Inline values first; assetize only when reuse/workflow is proven. | Every new index crosses four contexts and needs explicit translation (`docs/glossary.md:27-48`). |
| P9-D7 | Multiple colliders/compound bodies: one collider per body, child colliders, or an authored collider array. | One body + one collider per entity for the vertical slice; decide compound representation before convex/mesh work. | Existing ECS uses one component type per entity and hierarchy is already authoritative (`RT2App/src/ECSComponents.h:38-75`). Product requirement is under-specified. |
| P9-D8 | Parented bodies and non-uniform scale. | Root dynamic/kinematic; parented static only after strict world-TRS validation; loud per-shape scale rules. | SceneGraph/local conversion constraints (`RT2App/src/SceneGraph.cpp:21-78`; `RT2App/src/TransformEditing.cpp:36-100`). Exact backend support is Jolt-doc-dependent. |
| P9-D9 | Dynamic Transform write: ignore, teleport, force/impulse, or error. | Generic setter routes to an explicit queued teleport with visible velocity policy; forces/impulses are separate APIs. | Current setter mutates ECS only (`RT2App/src/ScriptSystem.cpp:1480-1513`), which would create stale Jolt state. |
| P9-D10 | Interpolation location: ECS presentation transform vs renderer-only override. | Prototype both; prefer the smallest design that preserves exact fixed-script observations and smooth variable/render observations. | Current GPU DTO reads ECS directly and no alpha exists (`RT2App/src/GPUSceneData.cpp:444-480`; `RT2App/src/RuntimeSceneController.h:220-264`). |
| P9-D11 | Event delivery: per-step callbacks, per-frame batch, or polling. | RT2-owned per-step immutable batch, stable-sorted, dispatched after physics; optionally expose polling over the same DTO later. | Fixed callbacks are per-substep and structural mutation is deferred (`RT2App/src/IRuntimeScriptDispatch.h:52-94`). Listener ordering is Jolt-doc-dependent. |
| P9-D12 | Threading: single-thread, Jolt job system, or configurable. | Single-thread for first slice/tests; add jobs only after event/order and performance measurements. | Engine determinism forbids scheduler-dependent order (`docs/game-engine-development-plan.md:169-175`). Exact Jolt facilities are doc-dependent. |
| P9-D13 | Invalid authored body at Play: skip body with warning vs fail Play transactionally. | Fail Play with structured UUID/component/field diagnostics. | Partial silent simulation conflicts with the established transactional load/queue pattern (`RT2App/src/SceneSerializer.h:47-64`; `RT2App/src/RuntimeSceneController.cpp:397-480`). |
| P9-D14 | Mesh source/cook cache identity and live recook policy. | Durable model `AssetReference`+`sourceKey`, cacheRoot-derived key, rebuild next Play; no live recook initially. | Durable versus transient mesh identities (`RT2App/src/AssetReference.h:102-129`; `RT2App/src/MeshRegistry.h:14-44`); current watcher refreshes database only (`RT2App/src/AssetWatchPolicy.cpp:21-30`). |
| P9-D15 | Exit/destruction events: emit Exit before teardown, suppress it, and handle pending-destroy pairs. | Specify one pair-state rule before event code; never infer from backend callback order. | Current `on_destroy` must observe the entity alive (`RT2App/src/IRuntimeScriptDispatch.h:66-76`), while destruction is deferred until after fixed steps (`RT2App/src/RuntimeSceneController.cpp:267-280`). |

## Verification performed for this grounding pass

No engine code was changed. The ignored/generated solution file was absent, so
the checked-in `RT2Tests.vcxproj` was built directly with Visual Studio 2022
MSBuild. Both executables were run from the repository root, as required by
fixture path handling (`AGENTS.md:76-78`).

| Configuration | Command | Result at grounded commit |
|---|---|---|
| Release x64 | `MSBuild.exe RT2Tests/RT2Tests.vcxproj -p:Configuration=Release -p:Platform=x64 -m`, then `bin/Release-windows-x86_64/RT2Tests/RT2Tests.exe` | Build succeeded; **772/772 tests, 146,801/146,801 assertions, 0 failed, 0 skipped** |
| Debug x64 | `MSBuild.exe RT2Tests/RT2Tests.vcxproj -p:Configuration=Debug -p:Platform=x64 -m`, then `bin/Debug-windows-x86_64/RT2Tests/RT2Tests.exe` | Build succeeded; **772/772 tests, 146,801/146,801 assertions, 0 failed, 0 skipped** |

The new Phase 9 spec should supersede the stale baseline figures with a dated
append-only note rather than rewriting the historical sections
(`AGENTS.md:12-35`).

## Bottom line

Jolt can fit RT2 cleanly only if it is treated as a CPU-only runtime service
behind stable RT2 DTOs, with `RuntimeSceneController` retaining time and
ordering authority. The first implementation risk is not collision math; it is
the combination of a singular lifecycle seam, dual transform authorities,
handle-free persistence across scene/prefab/undo, and a “transform-only” render
path that currently blocks and rebuilds far more than its name promises. Settle
P9-D1 through P9-D15, review the resulting Phase 9 spec, then implement the
primitive drop slice before queries, events, cooking, or UI.
