#pragma once

#ifndef RT2_CORE_RUNTIME_SCENE_CONTROLLER_H
#define RT2_CORE_RUNTIME_SCENE_CONTROLLER_H

#include "SceneDocument.h"
#include "ISceneRenderBridge.h"
#include "IPhysicsCollisionAssetProvider.h"
#include "PhysicsWorld.h"
#include "PhysicsEvents.h"
#include "RuntimeLifecycleObserver.h"
#include "RuntimeSceneMutator.h"
#include "IRuntimeScriptDispatch.h"
#include "IRuntimeCommandSink.h"
#include "SceneRunState.h"
#include "InputTypes.h"
#include "core/Error.h"
#include "core/UUID.h"
#include "ECSComponents.h"

#include <memory>
#include <optional>
#include <string>
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
// Lifecycle (Phase 4 completion, T3 physics candidate-commit):
//   Play(authoring):
//     1. Validate physics early invariants on the authoring document (loud
//        typed Error naming the entity UUID; no mutation on failure).
//     2. Construct runtime document, set UUID provider, CloneInMemory.
//     3. InitPrevTransforms (rebuilds world transforms + snapshots prev).
//     4. Construct a complete private PhysicsWorld candidate; commit to
//        ownership only on success. On any candidate failure, destroy the
//        candidate, reset the clone, stay Edit with a zero accumulator, and
//        emit no bridge call and no script callback.
//     5. Bridge FullSync + ResetTemporalState.
//     6. Set m_State = Playing.
//     7. Fire OnSceneStart(runtime).
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
//     3. Destroy the PhysicsWorld (constraints, then ghosts/bodies, then
//        shapes, then the world; any surviving handle is a hard error).
//     4. Clear m_PendingOperations.
//     5. m_Runtime.reset().
//     6. Bridge FullSync + ResetTemporalState on the authoring document.
//     7. Set m_State = Edit.
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

// ---- T7 queued physics commands -------------------------------------------
//
// Bounded Lua physics mutations (velocity, impulse, hinge/slider drive,
// body-pose reset) never touch Bullet inline from a script callback. They
// enqueue here — validated at queue time (finite args, live body of a
// suitable kind, session mutable, target outside the frozen destroy set) —
// and the drain applies them in FIFO order at the next pre-step boundary
// inside RunFixedTick (after OnFixedUpdate + Motion, before PreStepSync),
// so:
//
//   - setters called from OnFixedUpdate affect the immediately following
//     step of the same tick;
//   - setters called from OnUpdate affect the next frame's first tick;
//   - no Bullet state mutates while script iteration (or the structural
//     drain) is running;
//   - reload, quarantine, and Stop clear the queue, so no stale command
//     outlives the script environment that issued it.
//
// Apply-time failures (the body died since queue time — possible only via
// the structural drain, which runs after the fixed ticks) skip silently:
// the queue-time validation already proved the command well-formed, so a
// missing handle at apply time is a raced teardown, not a script bug.
enum class QueuedPhysicsCommandKind : uint8_t
{
    SetLinearVelocity = 0,
    ApplyImpulse      = 1,
    SetHingeDrive     = 2,
    ReleaseHingeDrive = 3,
    SetSliderTarget   = 4,
    ReleaseSlider     = 5,
    ResetBodyPose     = 6,
};

struct QueuedPhysicsCommand
{
    QueuedPhysicsCommandKind kind = QueuedPhysicsCommandKind::SetLinearVelocity;
    UUID target; // Authoring UUID of the body (or constraint owner)
    glm::vec3 vector = {0.0f, 0.0f, 0.0f}; // velocity / impulse
    glm::vec3 position = {0.0f, 0.0f, 0.0f}; // reset pose translation
    glm::quat rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f}; // reset rotation
    glm::vec3 resetLinear = {0.0f, 0.0f, 0.0f}; // reset linear velocity
    glm::vec3 resetAngular = {0.0f, 0.0f, 0.0f}; // reset angular velocity
    bool hasResetLinear = false;
    bool hasResetAngular = false;
    float paramA = 0.0f; // hinge velocity / slider target / slider impulse
    float paramB = 0.0f; // hinge max impulse
};

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

    // Q4: a mutation gate the sink checks before writing to the runtime
    // document. Returns true only when Playing/Paused AND not stopping.
    // The sink's transform/vis/name setters call this before writing; a
    // false return means the write is rejected (the session is ending or
    // has ended). This centralizes the authority the controller already
    // has over the queue (QueueCreate/QueueDestroy check the same
    // conditions) and avoids the sink bypassing the controller via
    // const_cast during OnSceneStop.
    bool IsRuntimeMutable() const
    {
        return (m_State == SceneRunState::Playing ||
                m_State == SceneRunState::Paused) && !m_Stopping;
    }

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

    // T5 review fixup F1 + final re-review P1: frozen destroy-UUID set for
    // the active drain. While ApplyDeferredStructuralChanges iterates its
    // moved-to-local batch, every explicitly queued destroy UUID (plus its
    // registry subtree) is recorded here, and each destroy position merges
    // the recollected actual callback subtree before OnEntitiesDestroying —
    // which additionally covers descendants created earlier in the same
    // batch (invisible at precompute time). QueueCreate/QueueDestroy and
    // RuntimeCommandSink setters refuse these UUIDs for the remainder of
    // the drain (typed false + warn, no mutation); reads stay allowed.
    // Empty outside a drain. Callback-enqueued work lands in the emptied
    // m_PendingOperations (next safe point), never in the running batch.
    bool IsUuidInDestroyDrain(const UUID& uuid) const
    {
        return m_DestroyingUuids.count(uuid) != 0;
    }

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

    // ---- T7 queued physics commands (validated enqueue, pre-step drain) ---
    //
    // Every method validates before enqueueing and returns false (mutating
    // nothing, enqueueing nothing) when the session is not mutable, the
    // target lies in the frozen destroy set (T6 refusal, loud warn), no
    // physics world is committed, the arguments are non-finite/out-of-range,
    // or the target is not a live body (or constraint owner) of a suitable
    // kind. Loud printf + false follows docs/scripting.md:97-111; the
    // IsRuntimeMutable gate stays silent like the existing sink setters.
    bool QueueSetLinearVelocity(const UUID& target, const glm::vec3& velocity);
    bool QueueApplyImpulse(const UUID& target, const glm::vec3& impulse);
    bool QueueSetHingeDrive(const UUID& owner, float velocity,
                            float maxImpulse);
    bool QueueReleaseHingeDrive(const UUID& owner);
    bool QueueSetSliderTarget(const UUID& owner, float target);
    bool QueueReleaseSlider(const UUID& owner, float impulse);
    bool QueueResetBodyPose(const UUID& target, const glm::vec3& position,
                            const glm::quat& rotation, bool hasLinearVelocity,
                            const glm::vec3& linearVelocity,
                            bool hasAngularVelocity,
                            const glm::vec3& angularVelocity);

    // Drop all queued-but-unapplied physics commands (reload, quarantine,
    // and Stop call this: no stale command outlives its issuing
    // environment). Silent: the caller already diagnosed the reason.
    void ClearQueuedPhysicsCommands() { m_PhysicsCommands.clear(); }
    // Test seam: commands waiting for the next pre-step boundary. Zero in
    // Edit, after Stop, after a drain with no new script writes, and after
    // any reload/quarantine clear.
    size_t QueuedPhysicsCommandCount() const
    {
        return m_PhysicsCommands.size();
    }
    // T7 re-review P2: unexpected pre-step drain drops (a command that
    // passed queue-time validation but failed at apply time). The drain
    // prints UUID + op and continues; the count and the last message are
    // readable here so tests prove the loud-drop-and-continue contract.
    // Both reset on Play and Stop.
    size_t UnexpectedDrainDropCount() const { return m_DrainDropCount; }
    std::string LastDrainDrop() const { return m_LastDrainDrop; }

    // ---- T3 physics world lifecycle -------------------------------------

    // Injectable collision provider. Stored non-owning: the host owns the
    // provider for the host session and the controller borrows the pointer
    // for one Play session only (plan Sol B2). CPU-only targets inject an
    // explicit test provider through this same seam. Null (the default)
    // means no provider: Play with physics collision refs then refuses
    // loudly; Play without refs proceeds with an empty world.
    void SetCollisionProvider(IPhysicsCollisionAssetProvider* provider)
    {
        m_CollisionProvider = provider;
    }
    IPhysicsCollisionAssetProvider* GetCollisionProvider() const
    {
        return m_CollisionProvider;
    }

    // Committed physics world for the current Play session. Null in Edit and
    // after Stop; non-null while Playing/Paused after a successful Play.
    const PhysicsWorld* TryGetPhysicsWorld() const
    {
        return m_PhysicsWorld.get();
    }
    PhysicsWorld* TryGetPhysicsWorldMut() { return m_PhysicsWorld.get(); }

    // Handle census for tests and the Stop leak assertion. Zero when no
    // world is committed (Edit, or failed Play). T3 worlds are always empty;
    // T4 backfills bodies through the same counters.
    size_t PhysicsBodyCount() const
    {
        return m_PhysicsWorld ? m_PhysicsWorld->BodyCount() : 0;
    }
    size_t PhysicsConstraintCount() const
    {
        return m_PhysicsWorld ? m_PhysicsWorld->ConstraintCount() : 0;
    }
    size_t PhysicsShapeCount() const
    {
        return m_PhysicsWorld ? m_PhysicsWorld->ShapeCount() : 0;
    }
    size_t PhysicsGhostCount() const
    {
        return m_PhysicsWorld ? m_PhysicsWorld->GhostCount() : 0;
    }
    size_t PhysicsTotalHandles() const
    {
        return m_PhysicsWorld ? m_PhysicsWorld->TotalHandles() : 0;
    }
    uint64_t PhysicsStepCount() const
    {
        return m_PhysicsWorld ? m_PhysicsWorld->StepCount() : 0;
    }

    // ---- T6 deterministic physics events --------------------------------
    //
    // The immutable per-frame snapshot (PhysicsEvents.h rules): scraped
    // after every fixed tick, accumulated across the frame's 0-5 ticks,
    // filtered for drain-destroyed UUIDs, and published before OnUpdate.
    // Non-consuming: every script consumer in the frame reads these same
    // contents regardless of UUID-sorted callback order.
    //
    // T7 re-review P1(1): Lua visibility is phase-gated, not publication-
    // ordered. The C++ accessor below always returns the published snapshot,
    // but the Lua `world:physics_events()` binding (via the sink) observes
    // it ONLY while the controller is dispatching OnUpdate
    // (PhysicsEventsLuaVisible). SyncScriptEnvironments runs before OnUpdate
    // — so a runtime-spawned script's on_create and a destroyed script's
    // on_destroy observe an empty table even on frames with contacts — and
    // OnFixedUpdate observes empty via the T6 BeginPhysicsFrame clear.
    // Timer callbacks fire at the tail of OnUpdate dispatch, inside the
    // same visible window, so they observe the same snapshot the frame's
    // on_update callbacks saw.
    const std::vector<PhysicsEvent>& PhysicsEvents() const
    {
        return m_PhysicsSnapshot;
    }

    // T7 re-review P1(1): true only while dispatching OnUpdate (including
    // its timer tail). The sink's Lua event poll returns empty when false.
    bool PhysicsEventsLuaVisible() const { return m_PhysicsEventsLuaVisible; }
    void SetPhysicsEventsLuaVisible(bool visible)
    {
        m_PhysicsEventsLuaVisible = visible;
    }

    // Test-only accumulator read-out. Failed Play must leave it zero; Step
    // must not advance it.
    float DebugAccumulator() const { return m_Accumulator; }

    // Phase-1 batch validation as a standalone predicate (T5 destroy-policy
    // seam): duplicate-UUID, parent-resolution, destroy-target, AND the
    // constrained-body rule — a destroy batch that would orphan a surviving
    // hinge/slider (owner lives, referenced body dies) is rejected as a
    // whole unless the constraint owner dies in the same batch. False leaves
    // the queue and the world untouched; true means the drain may apply.
    // The drain calls this before mutating anything.
    bool ValidatePendingBatch(Error& err) const;

private:
    // Initialize prevWorldMatrix = worldMatrix for all transforms in the
    // runtime document. Called once at Play to prevent invalid motion vectors
    // on the first frame.
    void InitPrevTransforms();

    // Snapshot current world matrices into prevWorldMatrix. Called before
    // each simulation step.
    void SnapshotPrevTransforms();

    // Run one fixed update tick (MotionSystem + T4 kinematic pre-push +
    // exactly one PhysicsWorld step at kFixedDt + T4 dynamic write-back).
    // Does NOT drain the queue — the queue is drained at the safe point
    // AFTER the fixed-step loop.
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
    //
    // T6: `destroyedUuids` is filled with every UUID torn down by this batch
    // (each destroy op's recollected subtree at its apply position) so the
    // caller can filter the frame's physics event snapshot before OnUpdate.
    // Cleared on entry; empty when nothing was destroyed or validation
    // failed (no partial teardown exists on failure).
    bool ApplyDeferredStructuralChanges(Error& err,
                                        std::vector<UUID>& createdUuids,
                                        std::vector<UUID>& destroyedUuids);

    // Helper: collect UUIDs of pending create operations so a second create
    // with a provider-duplicate UUID does not collide with a queued-but-
    // undrained create.
    std::unordered_set<UUID> PendingCreateUuids() const;

    std::unique_ptr<SceneDocument> m_Runtime;
    // T3: at most one committed PhysicsWorld per Play session. Staged as a
    // local candidate in Play() and moved here only after complete
    // construction; destroyed in Stop() before m_Runtime.reset(). Never
    // global, never surviving Stop.
    std::unique_ptr<PhysicsWorld> m_PhysicsWorld;
    // T3: live-world baseline captured before candidate construction so Stop
    // can assert the committed world was destroyed, not pointer-discarded.
    size_t m_PhysicsLiveBaseline = 0;
    // T3: borrowed collision provider (host-owned; see SetCollisionProvider).
    IPhysicsCollisionAssetProvider* m_CollisionProvider = nullptr;
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
    // T5 review fixup F1: frozen destroy-UUID set, populated at drain start
    // from the moved-to-local batch and cleared when the drain returns.
    std::unordered_set<UUID> m_DestroyingUuids;
    // T6 frame event staging: per-tick scrape output accumulates here across
    // the frame's fixed ticks (BeginPhysicsFrame clears it), then
    // PublishPhysicsSnapshot filters destroy UUIDs, collapses exact dups,
    // and moves the result into m_PhysicsSnapshot before OnUpdate.
    std::vector<PhysicsEvent> m_FrameEventAccum;
    // T6 published snapshot (see the PhysicsEvents() accessor contract).
    std::vector<PhysicsEvent> m_PhysicsSnapshot;
    // T7 queued physics commands (validated at queue time, drained at the
    // next pre-step boundary; cleared on reload, quarantine, and Stop).
    std::vector<QueuedPhysicsCommand> m_PhysicsCommands;
    // T7 re-review P2: unexpected drain-drop observation (count + last
    // message). Reset on Play and Stop so sessions start at zero.
    size_t m_DrainDropCount = 0;
    std::string m_LastDrainDrop;
    // T7 re-review P1(1): Lua event-poll visibility window. Set around the
    // OnUpdate dispatch only (Update and Step); false everywhere else, so
    // on_create/on_destroy (SyncScriptEnvironments) and OnFixedUpdate can
    // never observe the published snapshot through the Lua binding.
    bool m_PhysicsEventsLuaVisible = false;
    // T6 frame-local fixed-tick sequence stamped on scraped events (0..4).
    uint32_t m_PhysicsTickIndex = 0;
    // T6: clear the per-tick staging AND the previously published snapshot,
    // then restart the tick sequence. Called at the top of every Update/Step
    // frame so zero-tick frames publish a fresh empty snapshot instead of
    // stale prior-frame data, and so pre-publication callbacks
    // (OnFixedUpdate, OnEntitiesDestroying) never observe the previous
    // frame's events (re-review P1(1)).
    void BeginPhysicsFrame();
    // T6: filter m_FrameEventAccum for destroyedUuids, collapse exact
    // duplicates keeping first occurrence, publish into m_PhysicsSnapshot,
    // and clear the staging. Called after the safe-point drain and before
    // SyncScriptEnvironments/OnUpdate on every Update/Step frame.
    void PublishPhysicsSnapshot(const std::vector<UUID>& destroyedUuids);
    // T7: apply every queued physics command in FIFO order at the pre-step
    // boundary (called from RunFixedTick after OnFixedUpdate + Motion,
    // before PreStepSync). Re-checks world/destroy-set/liveness per command
    // and skips raced teardowns silently; clears the queue even when the
    // world is gone.
    void DrainPhysicsCommands();
    // T7 queue-time guards shared by every Queue*Physics* entry point (see
    // the .cpp for the refusal contract). Null return from QueuePhysicsBody
    // means the caller already refused loudly.
    bool QueuePhysicsGuard(const UUID& target, const char* opName);
    const PhysicsBodyComponent* QueuePhysicsBody(const UUID& target,
                                                const char* opName);
    RuntimeSceneMutator m_Mutator;
    bool m_Stopping = false;
};

} // namespace rt2::core

#endif // RT2_CORE_RUNTIME_SCENE_CONTROLLER_H