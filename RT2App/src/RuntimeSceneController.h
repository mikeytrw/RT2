#pragma once

#ifndef RT2_CORE_RUNTIME_SCENE_CONTROLLER_H
#define RT2_CORE_RUNTIME_SCENE_CONTROLLER_H

#include "SceneDocument.h"
#include "ISceneRenderBridge.h"
#include "RuntimeLifecycleObserver.h"
#include "RuntimeSceneMutator.h"
#include "IRuntimeScriptDispatch.h"
#include "IRuntimeCommandSink.h"
#include "InputTypes.h"
#include "core/Error.h"
#include "core/UUID.h"
#include "ECSComponents.h"

#include <memory>
#include <optional>
#include <unordered_set>
#include <variant>
#include <vector>

// ============================================================================
// RuntimeSceneController — owns the runtime scene clone and the Edit/Play/
// Pause lifecycle.
//
// The controller does NOT call RendererGPU or any Vulkan type directly. It
// communicates with the renderer through ISceneRenderBridge, so it links
// cleanly into RT2Tests and RT2SliceRunner (which supply a null/recording
// bridge) while RT2App supplies a real bridge backed by RendererGPU.
//
// Lifecycle (Phase 4 completion):
//   Play(authoring):
//     1. Construct runtime document, set UUID provider, CloneInMemory.
//     2. InitPrevTransforms.
//     3. Bridge FullSync + ResetTemporalState.
//     4. Set m_State = Playing.
//     5. Fire OnSceneStart(runtime).
//   Pause:
//     Clear the accumulator so stale wall-clock time cannot become queued
//     simulation on resume. No simulation runs while paused. Queue
//     submission remains allowed while Paused.
//   Step:
//     Valid only while Paused. Runs exactly one fixed tick (MotionSystem +
//     deferred structural changes + SceneGraph + one batched sync) plus one
//     presentation pass. Drains the deferred queue at the safe point. The
//     accumulator is NOT advanced. Returns false if not paused.
//   Stop:
//     1. Set m_Stopping (queue submission disabled).
//     2. Fire OnSceneStop(runtime).
//     3. Clear m_PendingOperations.
//     4. m_Runtime.reset().
//     5. Bridge FullSync + ResetTemporalState on the authoring document.
//     6. Set m_State = Edit.
//
// Deferred structural operations:
//   QueueCreateRuntimeEntity allocates a fresh UUID at queue time (returns
//   Result<UUID>) so later ops in the same tick can reference the new entity.
//   QueueDestroyRuntimeEntity enqueues a destroy. Both are rejected when
//   the controller is in Edit or stopping. The queue is one FIFO of
//   std::variant<Create, Destroy>, drained in exact enqueue order at the
//   safe point in Update/Step. The drain validates the complete batch first
//   (duplicate UUID, missing UUID, parent resolution, ancestor of a later
//   create) and applies atomically; on any validation failure the queue
//   is left intact and the runtime document is unchanged.
//
// The host (WalnutApp or RT2SliceRunner) calls Update(frameDt) each frame
// while Playing. The controller runs the fixed-step accumulator, performs
// one batched transform sync per rendered frame, and calls RequestRender.
//
// ============================================================================

namespace rt2::core {

// Fixed timestep and accumulator limits. Defined here so tests, the slice
// runner, and the interactive app share the same values. Canonical reference
// for these is docs/game-loop.md.
constexpr float kFixedDt       = 1.0f / 60.0f;
constexpr float kMaxFrameTime  = 0.25f;   // clamp large stalls
constexpr int   kMaxSubsteps   = 5;       // prevent spiral of death

enum class SceneRunState
{
    Edit,
    Playing,
    Paused,
};

// Deferred structural operations (Phase 4 §3). One FIFO queue, drained in
// exact enqueue order at the safe point.
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

class RuntimeSceneController
{
public:
    RuntimeSceneController() = default;
    ~RuntimeSceneController() = default;

    // Clone the authoring document into a runtime clone and activate it.
    // Returns false on clone failure (fills err). Only valid from Edit.
    bool Play(const SceneDocument& authoring, ISceneRenderBridge& bridge, Error& err);

    // Resume simulation from Paused. Returns false if not paused.
    bool Resume();

    // Pause simulation. Clears the accumulator.
    void Pause();

    // Advance the paused world by exactly one fixed tick + one presentation
    // pass. Returns false if not paused.
    bool Step(ISceneRenderBridge& bridge);

    // Stop the runtime, destroy the runtime clone, and re-activate the
    // authoring document for rendering.
    void Stop(const SceneDocument& authoring, ISceneRenderBridge& bridge);

    // Per-frame update while Playing. Runs the fixed-step accumulator and
    // one batched transform sync. No-op if not Playing.
    void Update(float frameDt, ISceneRenderBridge& bridge);

    SceneRunState GetState() const { return m_State; }

    // Returns the runtime document if Playing/Paused, null if Edit.
    const SceneDocument* TryGetRuntimeScene() const { return m_Runtime.get(); }

    // Non-const access for systems that need to mutate the runtime scene
    // (e.g. MotionSystem). Null if Edit.
    SceneDocument* TryGetRuntimeSceneMut() { return m_Runtime.get(); }

    // ---- Phase 4 completion API -----------------------------------------

    // Injectable runtime UUID provider. Stored non-owning, like
    // SceneManager::m_UuidProvider. The host (WalnutApp) injects the
    // production OsUuidProvider; tests inject a DeterministicUuidProvider
    // seeded for reproducibility. Play() sets the provider on the freshly
    // constructed runtime document BEFORE CloneInMemory, so the clone
    // preserves it (see SceneSerializer.cpp:1132-1137). Without this,
    // runtime UUID generation is impossible.
    void SetRuntimeUuidProvider(IUuidProvider* provider) { m_RuntimeUuidProvider = provider; }
    IUuidProvider* GetRuntimeUuidProvider() const { return m_RuntimeUuidProvider; }

    // Injectable lifecycle observer. Stored non-owning. OnSceneStart fires
    // after m_State = Playing with the runtime document by const reference
    // and (Phase 6) the read-only input service + runtime command sink;
    // OnSceneStop fires before the runtime is destroyed but after queue
    // submission is disabled.
    void SetLifecycleObserver(IRuntimeLifecycleObserver* observer) { m_LifecycleObserver = observer; }
    IRuntimeLifecycleObserver* GetLifecycleObserver() const { return m_LifecycleObserver; }

    // ---- Phase 6 script dispatch ----------------------------------------

    // Injectable script dispatch (G1). Stored non-owning. Distinct from the
    // lifecycle observer because OnFixedUpdate/OnUpdate/SyncScriptEnvironments
    // drive mutation through the sink and are not const-observe. The
    // controller calls these every frame (Update/Step) at the documented
    // frame-order slots. WalnutApp injects the ScriptSystem; tests and the
    // slice runner inject null or a recording spy.
    void SetScriptDispatch(IRuntimeScriptDispatch* dispatch) { m_ScriptDispatch = dispatch; }
    IRuntimeScriptDispatch* GetScriptDispatch() const { return m_ScriptDispatch; }

    // The read-only input service handed to scripts via OnSceneStart. Stored
    // non-owning. Set by WalnutApp to the application's InputService; tests
    // inject a null input service (scripts see a real-but-empty input). Null
    // is valid: OnSceneStart receives nullptr for `input` and the script
    // system treats input.* as inert (per S2: the 4-arg callback signature
    // is locked from 6A; input methods are added in 6C).
    void SetInputService(IInputService* input) { m_InputService = input; }
    IInputService* GetInputService() const { return m_InputService; }

    // The runtime command sink handed to scripts via OnSceneStart. Built by
    // the host (or ScriptSystem) per Play session; the controller does not
    // own it. Null is valid: OnSceneStart receives nullptr for `sink` and
    // the script system's entity/world bindings fail safely.
    void SetRuntimeCommandSink(IRuntimeCommandSink* sink) { m_CommandSink = sink; }
    IRuntimeCommandSink* GetRuntimeCommandSink() const { return m_CommandSink; }

    // Allocate a fresh UUID from the runtime provider, enqueue a create,
    // return the UUID so later operations in the same tick can reference
    // the new entity. Returns Failure if the controller is not
    // Playing/Paused (or is stopping), or if the provider is null, or if
    // the allocated UUID is already present (defensive — the provider
    // should not produce duplicates).
    Result<UUID> QueueCreateRuntimeEntity(const RuntimeEntityCreateDesc& desc);

    // Enqueue a destroy. Returns Failure if the controller is not
    // Playing/Paused (or is stopping). Does NOT validate the UUID here —
    // validation happens at drain time so a queued destroy of a not-yet-
    // created entity is a meaningful error rather than a silent drop.
    Result<void> QueueDestroyRuntimeEntity(const UUID& uuid);

    // Test-only accessors: number of pending operations and the queue
    // contents (by value, so tests can inspect without aliasing).
    size_t PendingOperationCount() const { return m_PendingOperations.size(); }
    std::vector<RuntimeStructuralOperation> PendingOperations() const
    {
        return m_PendingOperations;
    }

private:
    // Initialize prevWorldMatrix = worldMatrix for all transforms in the
    // runtime document. Called once at Play to prevent invalid motion vectors
    // on the first frame.
    void InitPrevTransforms();

    // Snapshot current world matrices into prevWorldMatrix. Called before
    // each simulation step.
    void SnapshotPrevTransforms();

    // Run one fixed update tick (MotionSystem). Does NOT drain the queue —
    // the queue is drained at the safe point AFTER the fixed-step loop.
    void RunFixedTick(float dt);

    // Apply deferred structural changes at the safe point (after the fixed-
    // step loop, before SceneGraph::UpdateWorldTransforms and the batched
    // sync). Batch-validate-then-apply: on any validation failure the queue
    // is left intact and the runtime document is unchanged. On success,
    // `createdUuids` is filled with the UUIDs of every entity created by
    // this batch so the caller can finalize prevWorldMatrix = worldMatrix
    // after the next UpdateWorldTransforms pass. Returns true if any
    // structural operation was applied this frame (so the caller picks
    // FullSync instead of TransformSync).
    bool ApplyDeferredStructuralChanges(Error& err,
                                        std::vector<UUID>& createdUuids);

    // Helper: collect UUIDs of pending create operations so a second create
    // with a provider-duplicate UUID does not collide with a queued-but-
    // undrained create.
    std::unordered_set<UUID> PendingCreateUuids() const;

    std::unique_ptr<SceneDocument> m_Runtime;
    SceneRunState m_State = SceneRunState::Edit;
    float m_Accumulator = 0.0f;

    // Phase 4: injectable UUID provider, lifecycle observer, FIFO queue.
    // Phase 6: script dispatch + input service + command sink.
    IUuidProvider* m_RuntimeUuidProvider = nullptr;
    IRuntimeLifecycleObserver* m_LifecycleObserver = nullptr;
    IRuntimeScriptDispatch* m_ScriptDispatch = nullptr;
    IInputService* m_InputService = nullptr;
    IRuntimeCommandSink* m_CommandSink = nullptr;
    std::vector<RuntimeStructuralOperation> m_PendingOperations;
    RuntimeSceneMutator m_Mutator;
    bool m_Stopping = false;
};

} // namespace rt2::core

#endif // RT2_CORE_RUNTIME_SCENE_CONTROLLER_H