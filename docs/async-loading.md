# Async scene loading

How RT2 loads scenes, imports models, and decodes environment maps without
freezing the UI. Covers the worker-thread wrapper, the two-phase loading
modal, and the async GPU-upload APIs.

Related: [scene-management.md](scene-management.md) for what loading
produces, [gpu-memory.md](gpu-memory.md) for the buffers involved.

---

## Why it exists

Loading a large glTF/OBJ is seconds of pure CPU work — parsing, decoding
textures, building meshes — followed by seconds of GPU work uploading
buffers and building acceleration structures. Doing either on the main
thread inside a frame blocks `OnUpdate`, so the window stops redrawing and
Windows marks the app "not responding".

The split is:

- **CPU work** (parse, decode, build ECS) runs on a **worker thread**.
- **GPU work** (uploads, AS build) stays on the **main thread** — Vulkan
  queue submission here is not thread-safe — but is broken into steps that
  each yield back to the frame loop, so the modal keeps painting.

---

## BackgroundWork

`RT2App/src/BackgroundWork.h`. A minimal `std::thread` wrapper with atomic
completion and a mutex-guarded status string.

```cpp
auto work = std::make_unique<BackgroundWork>();
work->Run("Loading scene...", [](BackgroundWork& self) {
    self.SetStatus("Parsing file...");
    // ... heavy CPU work, no Vulkan, no ImGui ...
    return true;                       // false = failed
});
// main thread, each frame:
if (!work->IsBusy()) { bool ok = work->GetResult(); }
```

Rules:

- `SetStatus` is the only thread-safe call from inside the work function.
- The work function must **never** touch Vulkan, ImGui, or the renderer.
  It parses into CPU-side structures; the main thread does the uploading.
- The thread is joined on destruction, so the work must always terminate.
- **Only one may be active at a time.** The host enforces this with
  `IsBackgroundBusy()`; every entry point that starts work checks it first
  and returns early. The loading modal is modal precisely so a second load
  cannot be started underneath the first.

---

## The loading modal is a two-phase state machine

`WalnutApp::DrawLoadingModal()` drives everything. Three pieces of state:

| Member | Meaning |
|---|---|
| `m_BackgroundWork` | non-null while **phase 1** (worker thread) is running |
| `m_GpuSyncPending` | true while **phase 2** (main-thread GPU sync) is running |
| `m_LoadingModalOpen` | whether the ImGui popup is currently open |

```
frame N      : m_BackgroundWork set, popup opened          -> phase 1
   ...       : worker runs; modal paints work->GetStatus()
frame N+k    : work done -> join, run completion callback  -> phase 1 end
               callback sets m_GpuSyncPending = true
frame N+k+1..: one GPU sub-step per frame                  -> phase 2
frame N+m    : m_GpuSyncPending = false, popup closed
```

Three details here are load-bearing and easy to break:

**The early-out must not fire while the popup is still open.**

```cpp
if (!m_BackgroundWork && !m_GpuSyncPending && !m_LoadingModalOpen) return;
```

`m_LoadingModalOpen` is in that condition so the function still runs for
one final frame after the work finishes, giving `CloseCurrentPopup()` a
frame to execute. Returning as soon as the work is done leaves an ImGui
popup open forever with nothing drawing it.

**The close must happen inside the popup's own Begin/End scope.**
`ImGui::CloseCurrentPopup()` is only meaningful between
`BeginPopupModal()` and `EndPopup()`; calling it outside silently does
nothing.

**Every GPU sub-step announces itself for one frame before blocking.**

```cpp
static const char* kStatus = "Building acceleration structures...";
if (m_GpuSyncStatus != kStatus) { m_GpuSyncStatus = kStatus; return; }
// ... now do the blocking work ...
```

The `return` lets the frame complete and paint the new label. Without it
the user stares at the *previous* step's label for the entire duration of
this one — which is exactly how "uploading to GPU" appeared to hang.

---

## Phase 2: the async GPU APIs

Each of these replaces a formerly blocking call with a begin/poll pair, so
the modal can yield between frames.

| Begin | Poll | Query |
|---|---|---|
| (implicit, at upload) | `RendererGPU::PollTextureUpload()` | — |
| `RendererGPU::BeginRebuildAccelerationStructures()` | `PollASRebuild()` | `IsASRebuildPending()`, `NeedsASRebuild()` |

`BeginRebuildAccelerationStructures` records and submits the AS build, then
returns immediately with a fence outstanding. `PollASRebuild()` checks the
fence with `vkGetFenceStatus` and returns true when signalled; the caller
then calls `UpdateDescriptorSetAfterAS()`. If `Begin...` returns false the
modal falls back to the synchronous `RebuildAccelerationStructures()` — a
slow frame is better than a scene that never loads.

`PollTextureUpload()` is not a thin forwarder: when the async textures are
adopted it must also refresh the path-trace descriptor set, but only when
the scene is valid and an AS rebuild is not about to happen (which would
rewrite the descriptors anyway).

---

## Failure handling

The historical bugs here were all *hangs and crashes on the unhappy path*,
so the rules are explicit:

- **Never wait unbounded on a fence that may never signal.**
  `AsyncTextureLoader::Adopt` used to log a failed `vkQueueSubmit` and then
  `vkWaitForFences(..., UINT64_MAX)` on a fence that could not possibly be
  signalled, hanging the app forever. It now skips the wait entirely when
  submit failed, and otherwise waits in 30 slices of one second with
  per-second progress output.
- **On submit failure, do not hand the textures to the renderer.** They are
  destroyed (safe — the GPU never saw them). On *timeout* they are
  deliberately leaked instead, because the GPU may still be reading them
  and freeing would be a use-after-free.
- **Value-initialise every Vulkan struct.** The batched attribute upload in
  `BuildAttributeBuffers` default-initialised its `VkBufferCopy`, leaving
  `srcOffset` as garbage; `vkCmdCopyBuffer` then read far outside the
  staging allocation and lost the device on the first flush. Each staging
  buffer holds one chunk at offset 0, so `srcOffset` is always 0 — it is
  now both value-initialised and set explicitly.

---

## Adding a new async operation

1. Check `IsBackgroundBusy()` and return early if something is running.
2. Create the `BackgroundWork`, do all CPU work in its lambda, never
   touching Vulkan or ImGui.
3. Set `m_OnBackgroundComplete` to a callback that runs on the **main
   thread** after the join. Do main-thread-only work there (installing
   results into `SceneManager`, kicking off the GPU upload), and set
   `m_GpuSyncPending = true` if GPU work follows.
4. If phase 2 has several steps, give each its own status string and the
   announce-then-block pattern above.

## Known gaps

- `SceneResources::BeginRebuildAccelerationStructures` busy-waits on the
  texture loader with no timeout — the one remaining unbounded wait.
- The async-loading path is **not covered by RT2Tests**: these files pull
  in Vulkan and so fall outside the CPU-only test boundary. Changes here
  must be validated by loading a large scene by hand.
