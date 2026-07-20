# Game Loop

The main loop lifecycle — from application startup through per-frame execution
to shutdown. Includes where future scripting and physics systems hook in.

---

## Startup Sequence

```
main()
  └─ Walnut::Application::Run()
       ├─ glfwInit() + create window
       ├─ SetupVulkan()          — VkInstance, VkDevice, VkQueue, descriptor pool
       ├─ SetupVulkanWindow()    — VkSurface, swapchain, framebuffers
       ├─ ImGui::CreateContext() + init backends
       ├─ font upload (one-off vkQueueSubmit + vkDeviceWaitIdle)
       ├─ RT2Layer constructor
       │    ├─ Camera init (FOV, near/far, aperture)
       │    ├─ SceneEditorUI wiring (callbacks)
       │    └─ (CLI args processed on first OnUIRender, not here)
       └─ Enter main loop
```

`RendererGPU::Init()` is called lazily on the first frame where ray tracing is
available and the GPU renderer is selected. This happens inside
`ProcessCLIArgs()` which runs on the first `OnUIRender()` call.

---

## Main Loop

```
while (window not closed):
    ┌─────────────────────────────────────────────────────────┐
    │ 1. glfwPollEvents()                                       │
    │    - Keyboard, mouse, window resize events                │
    │    - ImGui input state updated                            │
    ├─────────────────────────────────────────────────────────┤
    │ 2. Timestep computation                                   │
    │    - dt = GetTime() - m_LastFrameTime                     │
    │    - m_TimeStep = min(dt, 0.25s)  // clamp for stalls     │
    │    - Computed at loop START (not end) to eliminate        │
    │      1-frame input latency                                │
    ├─────────────────────────────────────────────────────────┤
    │ 3. OnUpdate(ts)  — per-layer update                       │
    │    ├─ Camera::OnUpdate(ts)     — movement, rotation       │
    │    ├─ [FUTURE] ScriptSystem::OnUpdate(ts)                 │
    │    ├─ [FUTURE] PhysicsSystem::Step(ts)                    │
    │    ├─ [FUTURE] SceneGraph::UpdateWorldTransforms()        │
    │    └─ Render()                 — GPU render submission    │
    │       (async, non-blocking — see Rendering Pipeline)      │
    ├─────────────────────────────────────────────────────────┤
    │ 4. Swapchain resize check                                 │
    │    - If g_SwapChainRebuild: recreate swapchain + ImGui    │
    ├─────────────────────────────────────────────────────────┤
    │ 5. ImGui frame                                             │
    │    ├─ ImGui_ImplVulkan_NewFrame()                         │
    │    ├─ ImGui_ImplGlfw_NewFrame()                           │
    │    ├─ ImGui::NewFrame()                                   │
    │    ├─ Dockspace + UI panels                               │
    │    │   ├─ Info panel (FPS, timings, GPU profiler)         │
    │    │   ├─ Settings panel (NRD, ReSTIR, render settings)   │
    │    │   ├─ Scene hierarchy / outliner                      │
    │    │   ├─ Properties editor (transform, material)         │
    │    │   └─ Viewport (ImGui::Image of renderer output)      │
    │    ├─ OnUIRender() per layer                              │
    │    │   ├─ First frame: ProcessCLIArgs (scene/env load)    │
    │    │   └─ Viewport resize detection (OnResize)            │
    │    └─ ImGui::Render()                                     │
    ├─────────────────────────────────────────────────────────┤
    │ 6. FrameRender()  — ImGui command buffer                  │
    │    - vkAcquireNextImageKHR (blocks on vsync)               │
    │    - vkWaitForFences (ImGui frame slot)                    │
    │    - Record ImGui draw data into command buffer            │
    │    - vkQueueSubmit (ImGui fence)                           │
    ├─────────────────────────────────────────────────────────┤
    │ 7. FramePresent()                                         │
    │    - vkQueuePresentKHR (waits on ImGui semaphore)          │
    └─────────────────────────────────────────────────────────┘
```

### Key design decisions

- **Render() in OnUpdate, not OnUIRender**: GPU render work is submitted before
  ImGui draws the viewport. This eliminates 1-frame visual latency — the
  viewport shows the current frame's output, not the previous frame's.
- **First-frame guard**: `Render()` is skipped until `m_CLIProcessed` is true
  (set after `ProcessCLIArgs` in `OnUIRender`), because the GPU renderer and
  scene are not initialized until CLI args are processed.
- **Timestep at loop start**: Computed before `OnUpdate`, not after present.
  This gives camera movement the actual elapsed time for the current frame,
  not a stale measurement from the previous frame. Clamp is 250ms (not 33ms)
  to avoid discarding real elapsed time under normal frame-time variance.

---

## Runtime frame-order contract

This section is the canonical ordering contract for the Edit/Play/Pause
runtime lifecycle. `game-engine-development-plan.md` may state phase-specific
requirements but must link here rather than define a competing order.

### Implemented (vertical slice)

The vertical slice implements a subset of the full contract. Systems marked
absent are placeholders for later phases.

```text
sample input
accumulate clamped frame time (max 0.25s)
while a fixed step is available, up to kMaxSubsteps (5):
    MotionSystem::FixedUpdate (UUID-sorted entity order)
    [physics step — absent, Phase 9]
apply deferred structural changes at the defined safe point
[variable script callbacks — absent, Phase 6]
[animation evaluation — absent, Phase 10]
update world transforms (SceneGraph)
issue one batched transform-only GPU sync
[audio update — absent, Phase 11]
render
```

Constants: `kFixedDt = 1/60`, `kMaxFrameTime = 0.25s`, `kMaxSubsteps = 5`.
If the substep cap is reached, residual accumulator time is dropped to
prevent cascading catch-up.

### Full contract (Phase 4+)

```text
sample input
accumulate clamped frame time
while a fixed step is available, up to the configured maximum:
    fixed script callbacks, in stable entity order
    physics step
apply deferred structural changes at the defined safe point, in stable order
variable script callbacks
animation evaluation
update world transforms
issue one batched dirty-state GPU sync using the scene sync-impact contract
update audio
render
```

### Pause and Step

Pause executes none of the simulation stages and clears the accumulator so
stale wall-clock time cannot become queued simulation on resume.

Step is valid only while Paused. It means: advance the paused runtime world
by exactly one `kFixedDt` tick, then make that tick renderable once. It must
not run the accumulator or consume wall-clock time. Specifically:

1. Snapshot previous transforms.
2. Run one fixed update tick:
   a. Script fixed callbacks (`IRuntimeScriptDispatch::OnFixedUpdate`,
      UUID-sorted) — Phase 6A.
   b. MotionSystem (inline, UUID-sorted).
3. Apply deferred structural changes at the defined safe point.
4. Sync script environments (`IRuntimeScriptDispatch::SyncScriptEnviron-
   ments` — fires `OnCreate` for newly applied entities, `OnDestroy` for
   destroyed ones) — Phase 6A.
5. Script variable callbacks (`IRuntimeScriptDispatch::OnUpdate`,
   UUID-sorted) — Phase 6A.
6. Update world transforms (SceneGraph).
7. Issue one batched GPU sync (coalesced: structural > material > transform).
8. Request one render submission.

The accumulator is NOT advanced. Variable scripts (when present) run once
with `kFixedDt`, not an arbitrary frame duration, so a stepped frame is
deterministic. After the render consumes the transform pair, previous
transforms must be committed to current before the next paused render,
otherwise motion vectors persist while stationary.

### Lifecycle

The runtime controller (`RuntimeSceneController`) snapshots previous
transforms before simulation and owns the runtime-world lifecycle. Play
deep-clones the authoring `SceneDocument` into a runtime document
(preserving UUIDs, not cloning transient state), initializes
`prevWorldMatrix = worldMatrix`, and performs a full GPU sync. Stop
destroys runtime-only state, re-activates the authoring document with a
full GPU sync, and restores the editor camera. The authoring scene is
never mutated during Play.

The stable-order requirements are part of the determinism policy: fixed system
iteration, deferred creates/destroys, seeds, and event delivery must not depend
on hash-table or thread scheduling order. Floating-point comparisons use the
documented system tolerance rather than claiming bitwise equality across all
platforms.

### Editor camera cuts and temporal history

Interactive continuous movement is sampled by `Camera::OnUpdate`. Atomic
programmatic cuts (frame/focus selected, bookmark recall, View Through Camera,
and numeric pose/optics edits) route through `ApplyEditorCameraCut`. The pose is
validated and applied as one unit, then
`ISceneRenderBridge::ResetTemporalState()` is invoked exactly once. The real
bridge resets accumulation and NRD correspondence state and invalidates both
ReSTIR DI and ReSTIR GI histories. Editor-only navigation does not dirty the
authoring document and emits no scene synchronization impact.

Play snapshots whatever editor pose exists at the instant Play begins. A
runtime camera entity is selected deterministically by lowest UUID, with the
legacy scene-level camera as fallback. Stop restores the exact snapshot; View
Through Camera is a one-time pose copy, not a persistent mode.

### Autosave and recovery scheduling (Phase 1B)

Autosave runs synchronously on the editor main thread in `OnUpdate`, AFTER the
runtime step and BEFORE `Render()`, and only while the controller is in Edit.
It is authoring-only: the runtime Play clone is never captured.
The `SceneRecoveryService::MaybeSnapshot` call is guarded by:

1. `doc.metadata.dirty == true` — clean frames do nothing.
2. interval elapsed (default 60s, injectable clock for tests) since the first
   dirty observation or the previous successful snapshot. The first dirty
   frame starts the timer and does not write immediately.
3. `AuthoringRevision()` changed since the last snapshot — unchanged
   revisions skip the write.

The authoring revision counter is bumped by `SceneManager::NotifyAuthoringChanged()`
on every authoring mutation and is NOT serialized into `.rt2scene`. This
prevents rewriting an identical recovery snapshot every frame while still
capturing every distinct edit.

A successful write reports its elapsed main-thread time in the editor status;
writes above the current 10 ms guardrail emit a warning. Failure remains
non-fatal and preserves the previous valid recovery envelope.

### Close / recovery lifecycle

- OS window close (title-bar X / Alt+F4) is routed through
  `Walnut::Application::RequestClose()`, an interactive, cancelable close.
  The `glfwSetWindowCloseCallback` cancels the GLFW-level close and calls
  `RequestClose()`, which fires the host's `CloseRequestCallback`. If the
  document is dirty, the callback queues an Exit action through the
  `UnsavedChangesCoordinator` and returns false (do not close) so the
  Save/Discard/Cancel modal can resolve. If clean, it executes immediately
  and the app exits.
- Headless/internal completion uses `Application::Close()` directly — no
  prompt, immediate exit.
- On startup, `SceneRecoveryService::Discover()` finds pending records
  from a previous unclean exit. If any exist, a Restore/Discard/Skip modal
  is shown. Restore is transactional (loads + resolves into a temp doc, swaps
  only on success). Discard deletes the record. Skip leaves it intact.

---

## Frames in Flight

RT2 uses a 2-frame-in-flight ring (`MAX_FRAMES_IN_FLIGHT = 2`):

```
Frame N:     [submit RT2 work] ──────────────── [fence N]
Frame N+1:   [submit RT2 work] ──────────────── [fence N+1]
Frame N+2:   [wait fence N] [submit RT2 work] ── [fence N]
                ^
                CPU stalls here if GPU hasn't finished frame N
```

Each `FrameContext` owns:
- `VkCommandPool` + `VkCommandBuffer` (reset each frame)
- `VkFence` (signaled on submit, waited on next time slot is reused)

The ImGui swapchain render uses a separate fence/semaphore set managed by
`ImGui_ImplVulkan`. Both RT2 and ImGui submit to the same `VkQueue`, so
Vulkan serializes them in submission order.

---

## Shutdown Sequence

```
RT2Layer::OnDetach()
  └─ RendererGPU::Destroy()
       ├─ vkDeviceWaitIdle()
       ├─ NRD::Destroy()
       ├─ ReSTIRPass::Destroy()           — DI compute pipelines
       ├─ Reservoirs::Destroy()           — DI reservoir buffers
       ├─ ReSTIRGIPass::Destroy()         — GI compute pipelines (temporal + history)
       ├─ GIReservoirs::Destroy()         — GI monolithic buffer
       ├─ PathTracePass::Destroy()        — pipeline, SBT, descriptor sets
       ├─ ComposePass::Destroy()
       ├─ TonemapPass::Destroy()
       ├─ RasterPass::Destroy()
       ├─ GBufferDebugPass::Destroy()
       ├─ SceneResources::Destroy()       — AS, buffers, textures
       └─ FrameContext::Destroy() x2      — command pools, fences

Application::Shutdown()
  ├─ vkDeviceWaitIdle()
  ├─ ImGui_ImplVulkan_Shutdown()
  ├─ CleanupVulkanWindow()             — swapchain, framebuffers
  └─ CleanupVulkan()                   — descriptor pool, device, instance
```

---

## Extension notes

The sketches below identify integration points only. The planned runtime order
above is normative once Phase 4 is implemented.

### Scripting (Phase 6A — implemented)

```
runtime frame:
  IRuntimeScriptDispatch::OnFixedUpdate(fixedDt)   // before motion
  <inline motion integration>
  ApplyDeferredStructuralChanges()                  // safe point
  IRuntimeScriptDispatch::SyncScriptEnvironments()  // OnCreate/OnDestroy
  IRuntimeScriptDispatch::OnUpdate(frameDt)         // after safe point
  SceneGraph::UpdateWorldTransforms()
  <one batched GPU sync>
  Render()
```

Scripts are per-entity `ScriptComponent` (persisted data: asset path +
field values) with a live `sol::environment` built by `ScriptSystem` on
Play and torn down on Stop. `ScriptSystem` implements both
`IRuntimeLifecycleObserver` (const-observe Play/Stop) and
`IRuntimeScriptDispatch` (per-frame mutation-driving). The environment
map is a per-frame-maintained mirror of the runtime registry:
`SyncScriptEnvironments` (called between the deferred safe point and
`OnUpdate`) fires `OnCreate` for newly applied entities and `OnDestroy`
for destroyed ones, so scripted spawning produces scripted entities.
Scripts mutate the runtime world through `IRuntimeCommandSink` (world
spawn/destroy, entity get/set transform/name/visible); the const
`SceneDocument` is never exposed. Per-instance state machine
(NeverCreated / Live / Quarantined / Destroyed) with protected-call
discipline ensures one bad script never crashes the engine.

### Physics (placeholder)

```
runtime frame:
  FixedScriptSystem::Update(fixedDt)
  PhysicsSystem::Step(fixedDt)
  ScriptSystem::OnUpdate(frameDt) // scripts see post-physics transforms
  SceneGraph::UpdateWorldTransforms()
  Render()
```

Fixed scripts run before physics; variable scripts run after physics so they
see post-step transforms. A
`RigidBodyComponent` would hold simulation state (velocity, mass, collider
handle). The physics system writes to `Transform::translation` and marks
`Transform::dirty = true`. SceneGraph resolves dirty transforms before
the renderer reads them.

### Fixed timestep for physics

```
accumulator += ts
while (accumulator >= PHYSICS_FIXED_DT):
    PhysicsSystem::Step(PHYSICS_FIXED_DT)
    accumulator -= PHYSICS_FIXED_DT
```

Physics uses a fixed timestep (e.g., 1/120s) for deterministic simulation.
The accumulator pattern sub-steps physics within a variable-rate render loop.
