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

## Future Hooks

### Scripting (placeholder)

```
OnUpdate(ts):
  Camera::OnUpdate(ts)
  ScriptSystem::OnUpdate(ts)     // ← NEW: runs entity behavior scripts
  Render()
```

Scripts would be per-entity components (`ScriptComponent`) containing a
callback or Lua function. The script system iterates entities with
`ScriptComponent + Transform` and calls `OnUpdate(entity, ts)`. Scripts can
modify transforms, spawn/despawn entities, and set material parameters.

### Physics (placeholder)

```
OnUpdate(ts):
  Camera::OnUpdate(ts)
  PhysicsSystem::Step(ts)        // ← NEW: advances simulation
  ScriptSystem::OnUpdate(ts)     // ← scripts see post-physics transforms
  SceneGraph::UpdateWorldTransforms()  // ← sync ECS from physics
  Render()
```

Physics runs before scripting so scripts see post-step transforms. A
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