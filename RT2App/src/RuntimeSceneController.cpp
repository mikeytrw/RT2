#include "RuntimeSceneController.h"
#include "SceneSerializer.h"
#include "SceneGraph.h"
#include "SceneHierarchy.h"
#include "ECSComponents.h"
#include "GPUSceneData.h"
#include "core/UUID.h"

#include <algorithm>
#include <cassert>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <new>
#include <set>
#include <unordered_set>
#include <vector>

namespace rt2::core {

namespace {

// T7 re-review P1(1): RAII for the Lua event-poll window. Opens on
// construction (OnUpdate dispatch entry), closes on destruction — including
// destruction during stack unwinding, so an escaping failure inside script
// dispatch cannot leave the window open for a later lifecycle callback.
class T7EventPollWindow
{
public:
    explicit T7EventPollWindow(RuntimeSceneController& controller)
        : m_Controller(controller)
    {
        m_Controller.SetPhysicsEventsLuaVisible(true);
    }
    ~T7EventPollWindow() { m_Controller.SetPhysicsEventsLuaVisible(false); }

    T7EventPollWindow(const T7EventPollWindow&) = delete;
    T7EventPollWindow& operator=(const T7EventPollWindow&) = delete;

private:
    RuntimeSceneController& m_Controller;
};

} // namespace

// ============================================================================
// Play
// ============================================================================

bool RuntimeSceneController::Play(const SceneDocument& authoring,
                                   ISceneRenderBridge& bridge,
                                   Error& err)
{
    if (m_State != SceneRunState::Edit)
        return false;

    m_Stopping = false;
    m_PendingOperations.clear();
    // T7: a fresh Play session starts with no queued physics commands (a
    // previous session's Stop already cleared them; this is defense in
    // depth for the same invariant).
    m_PhysicsCommands.clear();

    // T3 candidate-commit step 1: validate the physics early invariants on
    // the authoring document BEFORE anything is staged. Loud typed Error
    // naming the entity UUID; no clone, no world, no bridge call, no script
    // callback, accumulator stays zero.
    if (!ValidatePhysicsForPlay(authoring, m_CollisionProvider, err))
    {
        m_Accumulator = 0.0f;
        return false;
    }

    // Construct the runtime document and set the UUID provider BEFORE
    // CloneInMemory. CloneInMemory preserves the destination provider (see
    // SceneSerializer.cpp:1132-1137), so this is the only place we need to
    // inject it. Without this, runtime UUID generation is impossible.
    m_Runtime = std::make_unique<SceneDocument>();
    if (m_RuntimeUuidProvider)
        m_Runtime->SetUuidProvider(m_RuntimeUuidProvider);
    if (!SceneSerializer::CloneInMemory(authoring, *m_Runtime, err))
    {
        m_Runtime.reset();
        return false;
    }

    // Initialize prevWorldMatrix = worldMatrix so the first frame's motion
    // vectors are zero (no spurious movement from uninitialized prev state).
    // This rebuilds world transforms first, so a non-identity authored
    // transform is refreshed BEFORE the physics candidate below observes it.
    InitPrevTransforms();

    // T3 candidate-commit step 2 (+T4 body staging): construct the
    // PhysicsWorld completely in private against the refreshed runtime clone
    // and commit to ownership only on success. The borrowed collision
    // provider rides the same candidate: geometry borrows live only for the
    // Play session while Bullet shapes commit with the world. Rollback on any
    // candidate failure: the candidate dies with the local (full Bullet
    // teardown of a live world), the clone is reset, state stays Edit with a
    // zero accumulator, and no bridge call and no script callback have
    // happened yet.
    // happened yet. Defense in depth (narrow follow-up): even though Create
    // is itself no-throw, the handoff translates any escaping resource
    // failure into the same atomic refusal rather than an exception.
    {
        m_PhysicsLiveBaseline = PhysicsWorld::LiveWorldCount();
        Result<std::unique_ptr<PhysicsWorld>> candidate;
        bool handoffThrew = false;
        try
        {
            candidate = PhysicsWorld::Create(*m_Runtime, m_CollisionProvider);
        }
        catch (const std::bad_alloc&)
        {
            handoffThrew = true;
        }
        if (handoffThrew)
        {
            err.code = Error::Io;
            err.path = "";
            err.detail =
                "RuntimeSceneController::Play allocation failure during "
                "physics candidate handoff (clone reset; still Edit)";
            m_Runtime.reset();
            m_Accumulator = 0.0f;
            return false;
        }
        if (!candidate.IsOk())
        {
            err = candidate.error;
            m_Runtime.reset();
            m_Accumulator = 0.0f;
            return false;
        }
        m_PhysicsWorld = std::move(candidate.value);
    }

    // Activate the runtime document for rendering: full GPU upload + temporal
    // reset. The bridge builds GPUSceneData from m_Runtime and hands it to
    // the renderer.
    GPUSceneData gpuData = BuildGPUSceneDataFromECS(m_Runtime->ecs);
    if (m_Runtime->environment.HasEnvMap())
    {
        SceneTexture envTex;
        envTex.isHDR = true;
        envTex.width = m_Runtime->environment.width;
        envTex.height = m_Runtime->environment.height;
        envTex.floatPixels = m_Runtime->environment.floatPixels;
        gpuData.textures.push_back(envTex);
        gpuData.envMapIndex = (int)gpuData.textures.size() - 1;
    }
    m_Runtime->gpuCache = gpuData;
    bridge.FullSync(gpuData);
    bridge.ResetTemporalState();

    // Set m_State = Playing BEFORE firing OnSceneStart so a callback that
    // queries GetState() sees the post-Play state. OnSceneStart is a clean
    // observation seam — it may call QueueCreateRuntimeEntity (the queued op
    // is NOT drained during OnSceneStart; it waits for the next Update/Step).
    // Phase 6: pass the input service + command sink so scripts can read
    // input and mutate the runtime world through the controlled channel.
    m_State = SceneRunState::Playing;
    m_Accumulator = 0.0f;
    // T6: a fresh Play session starts with no staged ticks and an empty
    // published snapshot. The committed PhysicsWorld is newly constructed,
    // so its ghost-overlap history is empty by construction (the first
    // overlap reports TriggerEnter, never a stale Exit).
    m_FrameEventAccum.clear();
    m_PhysicsSnapshot.clear();
    m_PhysicsTickIndex = 0;

    if (m_LifecycleObserver)
        m_LifecycleObserver->OnSceneStart(*m_Runtime, m_InputService, m_CommandSink);

    return true;
}

// ============================================================================
// Pause
// ============================================================================

void RuntimeSceneController::Pause()
{
    if (m_State != SceneRunState::Playing)
        return;
    m_State = SceneRunState::Paused;
    // Clear the accumulator so stale wall-clock time cannot become queued
    // simulation on resume.
    m_Accumulator = 0.0f;
}

// ============================================================================
// Resume — return from Paused to Playing
// ============================================================================

bool RuntimeSceneController::Resume()
{
    if (m_State != SceneRunState::Paused || !m_Runtime)
        return false;
    m_State = SceneRunState::Playing;
    m_Accumulator = 0.0f;
    return true;
}

// ============================================================================
// Step — exactly one fixed tick + one presentation pass (Paused only)
// ============================================================================

bool RuntimeSceneController::Step(ISceneRenderBridge& bridge)
{
    if (m_State != SceneRunState::Paused || !m_Runtime)
        return false;

    constexpr float dt = kFixedDt;

    // T6: restart the frame's event staging so this Step publishes exactly
    // this tick's snapshot.
    BeginPhysicsFrame();

    // Snapshot prev transforms before the step.
    SnapshotPrevTransforms();

    // Run one fixed update tick.
    RunFixedTick(dt);

    // Safe point: drain the deferred queue. The queue is drained in both
    // Update and Step so a queued op is processed on the next simulation
    // tick regardless of Pause state.
    Error drainErr;
    std::vector<UUID> createdThisBatch;
    std::vector<UUID> destroyedThisBatch;
    const bool structural = ApplyDeferredStructuralChanges(
        drainErr, createdThisBatch, destroyedThisBatch);
    if (!drainErr.IsOk())
        printf("[Runtime] Step deferred-queue validation failed: %s (queue left intact)\n",
               drainErr.Format().c_str());

    // T6: filter drain-destroyed UUIDs and publish the immutable snapshot
    // before any script observes it.
    PublishPhysicsSnapshot(destroyedThisBatch);

    // Phase 6: sync script environments with the runtime registry after the
    // safe point (G2). Fires OnCreate for newly applied entities, OnDestroy
    // for destroyed ones, before OnUpdate runs.
    if (m_ScriptDispatch)
        m_ScriptDispatch->SyncScriptEnvironments();

    // Phase 6: variable script callbacks after the safe point + env sync,
    // before SceneGraph::UpdateWorldTransforms (game-loop.md:137). Step
    // runs exactly one OnUpdate at kFixedDt for determinism.
    // T7 re-review P1(1): the Lua event-poll window opens ONLY around this
    // dispatch (RAII: an escaping allocation failure still closes it). The
    // sync above (on_create/on_destroy) and any timer tail inside OnUpdate
    // are covered by construction: sync runs closed, timers run open.
    if (m_ScriptDispatch)
    {
        const T7EventPollWindow window(*this);
        m_ScriptDispatch->OnUpdate(dt);
    }

    // Batched: update world transforms once after the tick + drain.
    SceneGraph::UpdateWorldTransforms(m_Runtime->ecs.registry);

    // For every entity created by this batch, set prevWorldMatrix = worldMatrix
    // so the first frame's motion vectors are zero (no spurious movement from
    // uninitialized prev state). This mirrors InitPrevTransforms, scoped to
    // the batch's created set.
    if (!createdThisBatch.empty())
    {
        auto& reg = m_Runtime->ecs.registry;
        for (const auto& uuid : createdThisBatch)
        {
            const auto e = m_Runtime->FindByUuid(uuid);
            if (e != entt::null)
                if (auto* tf = reg.try_get<Transform>(e))
                    tf->prevWorldMatrix = tf->worldMatrix;
        }
    }

    // One sync for the presentation pass: FullSync if the frame applied any
    // structural operation, otherwise TransformSync. Updated in place — see
    // the note in Update() on why copying GPUSceneData per frame is costly.
    UpdateInstancesFromECS(m_Runtime->gpuCache, m_Runtime->ecs);
    if (structural)
    {
        GPUSceneData gpuData = m_Runtime->gpuCache;
        bridge.FullSync(gpuData);
    }
    else
    {
        bridge.TransformSync(m_Runtime->gpuCache);
    }

    // Request a render submission for the presentation pass.
    bridge.RequestRender();

    return true;
}

// ============================================================================
// Stop — destroy runtime, re-activate authoring
// ============================================================================

void RuntimeSceneController::Stop(const SceneDocument& authoring,
                                   ISceneRenderBridge& bridge)
{
    if (m_State == SceneRunState::Edit)
        return;

    // 1. Disable queue submission so a callback cannot queue structural
    //    operations during OnSceneStop.
    m_Stopping = true;

    // 2. Fire OnSceneStop while the runtime document still exists and is
    //    observable.
    if (m_LifecycleObserver && m_Runtime)
        m_LifecycleObserver->OnSceneStop(*m_Runtime);

    // 3. Destroy the PhysicsWorld BEFORE the runtime clone goes away:
    //    constraints, then ghosts/bodies, then shapes, then the world. The
    //    live-world census must return to the pre-Play baseline: a world
    //    that is merely pointer-discarded (never destroyed) keeps the count
    //    elevated, which this assert catches in debug (tests observe the
    //    same census in release). The real destructor is the destruction
    //    boundary: it runs here, while m_Runtime is still alive — reversing
    //    these two resets is caught by the Stop-order test's destroy probe.
    //    T6: clear the ghost-overlap history explicitly first (no fabricated
    //    stale events can outlive the session) and drop the published
    //    snapshot so post-Stop observers never read prior-session data.
    if (m_PhysicsWorld)
        m_PhysicsWorld->ClearEventHistory();
    m_PhysicsWorld.reset();
    m_FrameEventAccum.clear();
    m_PhysicsSnapshot.clear();
    m_PhysicsTickIndex = 0;
    assert(PhysicsWorld::LiveWorldCount() == m_PhysicsLiveBaseline &&
           "PhysicsWorld destroyed on Stop must restore the live baseline");

    // 4. Clear any pending operations (they are runtime-only). T7: queued
    // physics commands are runtime-only too — a re-Play must never inherit
    // a stale velocity/impulse/reset from the previous session.
    m_PendingOperations.clear();
    m_PhysicsCommands.clear();

    // 5. Destroy the runtime document and all runtime-only state.
    m_Runtime.reset();

    // 6. Re-activate the authoring document for rendering: full upload +
    //    temporal reset. The authoring document's canonical serialized state
    //    was never mutated during Play, so this restores the exact pre-Play
    //    visual state. (The transient gpuCache IS mutated via const_cast
    //    below — see Phase 4 spec §8: "authoring unchanged" excludes
    //    gpuCache, which is a CPU cache the Stop path legitimately rebuilds.)
    GPUSceneData gpuData = BuildGPUSceneDataFromECS(authoring.ecs);
    if (authoring.environment.HasEnvMap())
    {
        SceneTexture envTex;
        envTex.isHDR = true;
        envTex.width = authoring.environment.width;
        envTex.height = authoring.environment.height;
        envTex.floatPixels = authoring.environment.floatPixels;
        gpuData.textures.push_back(envTex);
        gpuData.envMapIndex = (int)gpuData.textures.size() - 1;
    }
    const_cast<SceneDocument&>(authoring).gpuCache = gpuData;
    bridge.FullSync(gpuData);
    bridge.ResetTemporalState();

    // 7. Reset state.
    m_State = SceneRunState::Edit;
    m_Accumulator = 0.0f;
    m_Stopping = false;
}
// ============================================================================
// Update — per-frame while Playing
// ============================================================================

void RuntimeSceneController::Update(float frameDt, ISceneRenderBridge& bridge)
{
    if (m_State != SceneRunState::Playing || !m_Runtime)
        return;

    // Clamp frame time to avoid spiral-of-death after stalls.
    float dt = std::min(frameDt, kMaxFrameTime);

    // T6: restart the frame's event staging. A frame that runs zero fixed
    // ticks therefore publishes a fresh empty snapshot below, never stale
    // prior-frame data.
    BeginPhysicsFrame();

    // Snapshot prev transforms before simulation.
    SnapshotPrevTransforms();

    // Fixed-step accumulator with max substep guard.
    m_Accumulator += dt;
    int substeps = 0;
    while (m_Accumulator >= kFixedDt && substeps < kMaxSubsteps)
    {
        RunFixedTick(kFixedDt);
        m_Accumulator -= kFixedDt;
        ++substeps;
    }

    // If we hit the substep cap, drop residual time to avoid buildup.
    if (substeps == kMaxSubsteps)
        m_Accumulator = 0.0f;

    // Safe point: drain the deferred queue after the fixed-step loop, before
    // SceneGraph::UpdateWorldTransforms and the batched sync.
    Error drainErr;
    std::vector<UUID> createdThisBatch;
    std::vector<UUID> destroyedThisBatch;
    const bool structural = ApplyDeferredStructuralChanges(
        drainErr, createdThisBatch, destroyedThisBatch);
    if (!drainErr.IsOk())
        printf("[Runtime] Update deferred-queue validation failed: %s (queue left intact)\n",
               drainErr.Format().c_str());

    // T6: filter drain-destroyed UUIDs and publish the immutable snapshot
    // before SyncScriptEnvironments/OnUpdate observe it.
    PublishPhysicsSnapshot(destroyedThisBatch);

    // Phase 6: sync script environments with the runtime registry after the
    // safe point (G2). Fires OnCreate for newly applied entities, OnDestroy
    // for destroyed ones, before OnUpdate runs.
    if (m_ScriptDispatch)
        m_ScriptDispatch->SyncScriptEnvironments();

    // Phase 6: variable script callbacks after the safe point + env sync,
    // before SceneGraph::UpdateWorldTransforms (game-loop.md:137).
    // T7 re-review P1(1): same OnUpdate-only event-poll window as Step.
    if (m_ScriptDispatch)
    {
        const T7EventPollWindow window(*this);
        m_ScriptDispatch->OnUpdate(dt);
    }

    // Batched: update world transforms once after all substeps + drain.
    SceneGraph::UpdateWorldTransforms(m_Runtime->ecs.registry);

    // For every entity created by this batch, set prevWorldMatrix = worldMatrix
    // so the first frame's motion vectors are zero.
    if (!createdThisBatch.empty())
    {
        auto& reg = m_Runtime->ecs.registry;
        for (const auto& uuid : createdThisBatch)
        {
            const auto e = m_Runtime->FindByUuid(uuid);
            if (e != entt::null)
                if (auto* tf = reg.try_get<Transform>(e))
                    tf->prevWorldMatrix = tf->worldMatrix;
        }
    }

    // One sync per rendered frame: FullSync if any structural operation was
    // applied this frame, otherwise TransformSync. A frame with a failed
    // validation batch (no mutation) fires TransformSync — the runtime
    // document is unchanged, so a transform-only sync is correct.
    //
    // UpdateInstancesFromECS only rewrites instances[] and lights[], so the
    // cache is updated in place. Copying GPUSceneData here would deep-copy
    // every SceneTexture::pixels buffer and the env-map CDFs once per frame
    // — pure CPU cost, invisible in the GPU timings, and only while playing.
    UpdateInstancesFromECS(m_Runtime->gpuCache, m_Runtime->ecs);
    if (structural)
    {
        // FullSync reaches SetScene(GPUSceneData&), which moves the textures
        // out of the argument. Hand it a copy so the cache keeps its pixels.
        GPUSceneData gpuData = m_Runtime->gpuCache;
        bridge.FullSync(gpuData);
    }
    else
    {
        // TransformSync takes a const ref; no copy needed.
        bridge.TransformSync(m_Runtime->gpuCache);
    }

    bridge.RequestRender();
}

// ============================================================================
// Deferred structural-operation queue (Phase 4 §3, §4)
// ============================================================================

std::unordered_set<UUID> RuntimeSceneController::PendingCreateUuids() const
{
    std::unordered_set<UUID> set;
    for (const auto& op : m_PendingOperations)
    {
        if (auto* create = std::get_if<CreateRuntimeEntityOperation>(&op))
            set.insert(create->uuid);
    }
    return set;
}

Result<UUID> RuntimeSceneController::QueueCreateRuntimeEntity(
    const RuntimeEntityCreateDesc& desc)
{
    if (m_State != SceneRunState::Playing && m_State != SceneRunState::Paused)
        return Result<UUID>::Fail(Error::InvalidRuntimeState, "",
            "QueueCreateRuntimeEntity: not Playing/Paused");
    if (m_Stopping)
        return Result<UUID>::Fail(Error::InvalidRuntimeState, "",
            "QueueCreateRuntimeEntity: controller is stopping");
    if (!m_RuntimeUuidProvider)
        return Result<UUID>::Fail(Error::InvalidRuntimeState, "",
            "QueueCreateRuntimeEntity: no runtime UUID provider");
    if (!m_Runtime)
        return Result<UUID>::Fail(Error::InvalidRuntimeState, "",
            "QueueCreateRuntimeEntity: no runtime document");
    // T5 fixup re-review P1: a callback create parented under the frozen
    // destroy set would validate against an entity about to be torn down and
    // then poison the next queue (validation retains the bad batch forever).
    // Refuse loudly here so the next safe point stays usable.
    if (desc.parentUuid && m_DestroyingUuids.count(*desc.parentUuid) != 0)
        return Result<UUID>::Fail(Error::InvalidEntity, desc.parentUuid->ToString(),
            "QueueCreateRuntimeEntity: parent is being destroyed in this safe-point drain");

    UUID uuid = m_RuntimeUuidProvider->CreateV4();
    while (m_Runtime->uuidIndex.Contains(uuid) ||
           PendingCreateUuids().count(uuid) != 0)
        uuid = m_RuntimeUuidProvider->CreateV4();

    m_PendingOperations.push_back(
        CreateRuntimeEntityOperation{ uuid, desc });
    return Result<UUID>::Ok(uuid);
}

Result<void> RuntimeSceneController::QueueDestroyRuntimeEntity(const UUID& uuid)
{
    if (m_State != SceneRunState::Playing && m_State != SceneRunState::Paused)
        return Result<void>::Fail(Error::InvalidRuntimeState, "",
            "QueueDestroyRuntimeEntity: not Playing/Paused");
    if (m_Stopping)
        return Result<void>::Fail(Error::InvalidRuntimeState, "",
            "QueueDestroyRuntimeEntity: controller is stopping");
    // T5 fixup re-review P1: on_destroy(A) -> destroy(A) would otherwise be
    // accepted into the next-safe-point queue while A is still observable,
    // then rejected as a missing target on every later drain (validation
    // retains the queue), permanently blocking structural work. Refuse
    // targets anywhere inside the frozen dying subtree loudly instead.
    if (m_DestroyingUuids.count(uuid) != 0)
        return Result<void>::Fail(Error::InvalidEntity, uuid.ToString(),
            "QueueDestroyRuntimeEntity: entity is being destroyed in this safe-point drain");

    m_PendingOperations.push_back(DestroyRuntimeSubtreeOperation{ uuid });
    return Result<void>::Ok();
}

bool RuntimeSceneController::ValidatePendingBatch(Error& err) const
{
    if (!m_Runtime)
        return false;

    const auto& doc = *m_Runtime;

    // Phase 1: validate the complete batch before any mutation. Walk the
    // queue in order, building the set of UUIDs that will exist after each
    // operation. The validation state starts from the current runtime
    // document UUID set.
    std::unordered_set<UUID> existingUuids;
    for (const auto& [uuid, entity] : doc.uuidIndex.All())
        existingUuids.insert(uuid);
    // T5 destroy closure: every UUID the batch removes (explicit destroys
    // plus their registry subtrees), for the constrained-body rule below.
    std::unordered_set<UUID> destroyedUuids;

    for (const auto& op : m_PendingOperations)
    {
        if (auto* create = std::get_if<CreateRuntimeEntityOperation>(&op))
        {
            if (existingUuids.count(create->uuid) != 0)
            {
                err = Error{ Error::DuplicateUuid, create->uuid.ToString(),
                    "ApplyDeferredStructuralChanges: duplicate UUID in batch" };
                return false;
            }
            if (create->desc.parentUuid &&
                existingUuids.count(*create->desc.parentUuid) == 0)
            {
                err = Error{ Error::InvalidEntity, create->desc.parentUuid->ToString(),
                    "ApplyDeferredStructuralChanges: parent UUID does not resolve" };
                return false;
            }
            existingUuids.insert(create->uuid);
        }
        else if (auto* destroy = std::get_if<DestroyRuntimeSubtreeOperation>(&op))
        {
            if (existingUuids.count(destroy->uuid) == 0)
            {
                err = Error{ Error::InvalidEntity, destroy->uuid.ToString(),
                    "ApplyDeferredStructuralChanges: destroy UUID not present (already destroyed or missing)" };
                return false;
            }
            // Remove the destroyed UUID (and its subtree, if it already exists
            // in the registry) from the validation set. A destroy of an
            // entity created earlier in this same batch has no subtree yet
            // (children would have to be created later in the batch and
            // would have specified this entity as parentUuid — but the
            // destroy removes it from the projected set, so a later create
            // child-of-destroyed-parent is correctly rejected). A destroy of
            // an entity that existed before this batch has its subtree in
            // the registry, so we collect it post-order and remove every
            // descendant UUID.
            existingUuids.erase(destroy->uuid);
            destroyedUuids.insert(destroy->uuid);
            const auto root = doc.FindByUuid(destroy->uuid);
            if (root != entt::null)
            {
                std::vector<entt::entity> subtree;
                SceneHierarchy::CollectSubtreePostOrder(doc.ecs.registry, root, subtree);
                for (const auto e : subtree)
                {
                    if (const auto* id = doc.ecs.registry.try_get<EntityIdComponent>(e))
                    {
                        existingUuids.erase(id->id);
                        destroyedUuids.insert(id->id);
                    }
                }
            }
        }
    }

    // T5 constrained-body destroy policy (plan section 3): a destroy batch
    // that would leave a surviving hinge/slider referencing a destroyed body
    // is rejected as a whole (loud, batch preserved) unless the constraint
    // owner is destroyed in the same batch — then teardown rides along.
    // UUID order for determinism: the first orphaned constraint fails.
    std::vector<std::pair<UUID, entt::entity>> hingeOwners;
    for (auto e : doc.ecs.registry.view<PhysicsHingeComponent>())
    {
        const auto* idc = doc.ecs.registry.try_get<EntityIdComponent>(e);
        if (idc != nullptr && !idc->id.IsNull())
            hingeOwners.emplace_back(idc->id, e);
    }
    std::vector<std::pair<UUID, entt::entity>> sliderOwners;
    for (auto e : doc.ecs.registry.view<PhysicsSliderComponent>())
    {
        const auto* idc = doc.ecs.registry.try_get<EntityIdComponent>(e);
        if (idc != nullptr && !idc->id.IsNull())
            sliderOwners.emplace_back(idc->id, e);
    }
    std::sort(hingeOwners.begin(), hingeOwners.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::sort(sliderOwners.begin(), sliderOwners.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    auto checkOther = [&](const UUID& owner, const UUID& other,
                          const char* kind) -> bool {
        if (other.IsNull())
            return true; // world anchor: no body to orphan
        if (destroyedUuids.count(owner) != 0)
            return true; // owner dies with the batch: teardown rides along
        if (destroyedUuids.count(other) != 0)
        {
            err = Error{ Error::InvalidArgument, owner.ToString(),
                std::string("ApplyDeferredStructuralChanges: destroy batch would orphan surviving ") +
                kind + " owner " + owner.ToString() +
                " (otherBody " + other.ToString() +
                " destroyed without the constraint owner)" };
            return false;
        }
        return true;
    };
    for (const auto& [owner, entity] : hingeOwners)
    {
        const auto& hinge = doc.ecs.registry.get<PhysicsHingeComponent>(entity);
        if (!checkOther(owner, hinge.otherBody, "PhysicsHingeComponent"))
            return false;
    }
    for (const auto& [owner, entity] : sliderOwners)
    {
        const auto& slider = doc.ecs.registry.get<PhysicsSliderComponent>(entity);
        if (!checkOther(owner, slider.otherBody, "PhysicsSliderComponent"))
            return false;
    }

    err = Error{};
    return true;
}

bool RuntimeSceneController::ApplyDeferredStructuralChanges(
    Error& err, std::vector<UUID>& createdUuids,
    std::vector<UUID>& destroyedUuids)
{
    createdUuids.clear();
    destroyedUuids.clear();

    if (!m_Runtime)
        return false;

    if (m_PendingOperations.empty())
        return false;

    auto& doc = *m_Runtime;

    // Phase 1 (no mutation): the standalone validator covers duplicate UUID,
    // parent resolution, destroy targets, and the T5 constrained-body rule.
    // Any failure leaves the queue intact and the document unchanged.
    if (!ValidatePendingBatch(err))
        return false;

    // Phase 2: freeze the validated batch. Move the queue into one local
    // vector and iterate it exactly once in enqueue order (T5 review fixup
    // F1: the approved frozen-safe-point contract). From this moment the
    // member queue is the NEXT safe point's queue: OnEntitiesDestroying
    // callbacks that QueueCreate/QueueDestroy append there, so callback work
    // can neither reallocate the vector under the active iteration nor be
    // dropped by the end-of-drain clear.
    std::vector<RuntimeStructuralOperation> batch;
    batch.swap(m_PendingOperations);

    // Frozen destroy-UUID set for the destroying-UUID command refusal: every
    // explicitly queued destroy UUID plus its current registry subtree, so a
    // callback write aimed anywhere inside a dying subtree refuses loudly
    // instead of mutating an entity about to be torn down. Each destroy
    // position additionally merges the recollected actual subtree before
    // OnEntitiesDestroying (see below), closing the batch-created-descendant
    // gap. Cleared on every drain exit below.
    m_DestroyingUuids.clear();
    for (const auto& op : batch)
    {
        if (auto* destroy = std::get_if<DestroyRuntimeSubtreeOperation>(&op))
        {
            m_DestroyingUuids.insert(destroy->uuid);
            const auto root = doc.FindByUuid(destroy->uuid);
            if (root != entt::null && doc.ecs.registry.valid(root))
            {
                std::vector<entt::entity> subtree;
                SceneHierarchy::CollectSubtreePostOrder(
                    doc.ecs.registry, root, subtree);
                for (const auto e : subtree)
                {
                    if (const auto* idc =
                            doc.ecs.registry.try_get<EntityIdComponent>(e))
                        m_DestroyingUuids.insert(idc->id);
                }
            }
        }
    }

    // Apply the frozen batch atomically in enqueue order via the mutator.
    // Post-validation, mutator failures are bugs — the validation phase
    // already checked every precondition (duplicate UUID, missing parent,
    // missing destroy target). A mutator Failure here means the validation
    // logic and the mutator disagree, which is a code bug. We assert in
    // debug and surface the error in release. The member queue is NOT
    // cleared here: it holds only next-safe-point work submitted by
    // callbacks during this drain, which must survive to the next frame
    // (the failed local batch tail is dropped — it already passed
    // validation, so re-running it would hit the same bug every frame).
    for (const auto& op : batch)
    {
        if (auto* create = std::get_if<CreateRuntimeEntityOperation>(&op))
        {
            auto r = m_Mutator.CreateEntity(doc, create->uuid, create->desc);
            if (!r.IsOk())
            {
                // Post-validation mutator failure is a bug (validation and
                // mutator disagree). Assert in debug so it's caught in
                // testing; in release, surface the error and preserve the
                // next-safe-point queue for the following drain.
                assert(false && "RuntimeSceneMutator::CreateEntity failed post-validation");
                err = r.error;
                m_DestroyingUuids.clear();
                return false;
            }
            createdUuids.push_back(create->uuid);
        }
        else if (auto* destroy = std::get_if<DestroyRuntimeSubtreeOperation>(&op))
        {
            // Phase 6: give scripts their on_destroy BEFORE the entities go
            // away, so the final callback can still read the entity it is
            // tearing down. Post-order = children first, matching the
            // destruction order. Skipped silently if the root no longer
            // resolves (a prior op in this batch already removed it).
            if (m_ScriptDispatch)
            {
                const auto root = doc.FindByUuid(destroy->uuid);
                if (root != entt::null && doc.ecs.registry.valid(root))
                {
                    std::vector<entt::entity> subtree;
                    SceneHierarchy::CollectSubtreePostOrder(
                        doc.ecs.registry, root, subtree);

                    std::vector<UUID> uuids;
                    uuids.reserve(subtree.size());
                    for (auto e : subtree)
                        if (const auto* idc =
                                doc.ecs.registry.try_get<EntityIdComponent>(e))
                            uuids.push_back(idc->id);

                    // T5 final re-review P1: the precomputed frozen set
                    // cannot see descendants created earlier in this same
                    // one-pass batch (create A, create B parented to A,
                    // destroy A): at precompute time neither exists in the
                    // registry, yet the recollect above now observes the
                    // real dying subtree [B, A]. Merge those actual
                    // callback UUIDs into the frozen set BEFORE the
                    // callbacks run, so QueueDestroy/create-under-B and
                    // sink writes targeting the batch-created child
                    // refuse loudly instead of poisoning the next queue.
                    // The set stays frozen for the rest of the drain and
                    // is cleared on drain exit; enqueue order is untouched.
                    for (const auto& id : uuids)
                        m_DestroyingUuids.insert(id);

                    if (!uuids.empty())
                        m_ScriptDispatch->OnEntitiesDestroying(uuids);
                }
            }

            // T5 physics teardown participation at the OnEntitiesDestroying
            // seam: ECS entities and Bullet objects are still present here.
            // Dependent constraints die FIRST, then the bodies/ghosts they
            // reference (constraints-before-bodies, same op position, so a
            // half-torn constraint set is never stepped). Phase-1 validation
            // already rejected batches that would orphan a surviving
            // constraint, so every affected constraint owner dies in this
            // same subtree.
            if (m_PhysicsWorld)
            {
                const auto root = doc.FindByUuid(destroy->uuid);
                if (root != entt::null && doc.ecs.registry.valid(root))
                {
                    std::vector<entt::entity> subtree;
                    SceneHierarchy::CollectSubtreePostOrder(
                        doc.ecs.registry, root, subtree);
                    std::vector<UUID> subtreeUuids;
                    subtreeUuids.reserve(subtree.size());
                    for (auto e : subtree)
                        if (const auto* idc =
                                doc.ecs.registry.try_get<EntityIdComponent>(e))
                            subtreeUuids.push_back(idc->id);
                    if (!subtreeUuids.empty())
                        m_PhysicsWorld->RemoveSubtreePhysics(subtreeUuids);
                    // T6: record the recollected dying subtree for the
                    // snapshot destroy filter. Recollection (not the frozen
                    // precompute) is what sees descendants created earlier
                    // in this same one-pass batch, so a create-then-destroy
                    // UUID's tick events are filtered even though the UUID
                    // did not exist at drain start.
                    destroyedUuids.insert(destroyedUuids.end(),
                                          subtreeUuids.begin(),
                                          subtreeUuids.end());
                }
            }
            else
            {
                // T6: no physics world, but the ECS teardown below still
                // destroys these UUIDs — record them for the snapshot
                // filter from the same recollected subtree.
                const auto root = doc.FindByUuid(destroy->uuid);
                if (root != entt::null && doc.ecs.registry.valid(root))
                {
                    std::vector<entt::entity> subtree;
                    SceneHierarchy::CollectSubtreePostOrder(
                        doc.ecs.registry, root, subtree);
                    for (auto e : subtree)
                        if (const auto* idc =
                                doc.ecs.registry.try_get<EntityIdComponent>(e))
                            destroyedUuids.push_back(idc->id);
                }
            }

            auto r = m_Mutator.DestroySubtree(doc, destroy->uuid);
            if (!r.IsOk())
            {
                assert(false && "RuntimeSceneMutator::DestroySubtree failed post-validation");
                err = r.error;
                m_DestroyingUuids.clear();
                return false;
            }
        }
    }

    // Phase 3: the frozen batch is committed. The member queue is NOT
    // cleared: it holds only callback-submitted next-safe-point work, which
    // the following frame's drain will validate and apply.
    m_DestroyingUuids.clear();

    // Phase 4 (post-apply): the caller will run SceneGraph::UpdateWorldTransforms
    // next, then set prevWorldMatrix = worldMatrix for every created entity
    // so the first frame's motion vectors are zero. We hand the created set
    // back via the out-param so the controller can finalize prevWorldMatrix
    // after UpdateWorldTransforms.

    return true;
}

// ============================================================================
// T6 frame event snapshot (staging + publish)
// ============================================================================

void RuntimeSceneController::BeginPhysicsFrame()
{
    // T6 re-review P1(1): the published snapshot clears here, not only at
    // PublishPhysicsSnapshot. Otherwise frame N's events stay readable
    // through PhysicsEvents() during frame N+1's OnFixedUpdate and
    // OnEntitiesDestroying — before the drain filter runs — contradicting
    // the contract that events are visible exactly inside OnUpdate and that
    // on_destroy receives no physics events. The per-tick staging still
    // accumulates below; only the published (previous-frame) view resets.
    m_FrameEventAccum.clear();
    m_PhysicsSnapshot.clear();
    m_PhysicsTickIndex = 0;
}

void RuntimeSceneController::PublishPhysicsSnapshot(
    const std::vector<UUID>& destroyedUuids)
{
    // Destroy filter: tick events referencing a UUID torn down by this
    // frame's drain never reach OnUpdate (the bodies are gone; delivering
    // their last-tick contacts would observe a destroyed entity).
    std::unordered_set<UUID> dead;
    dead.reserve(destroyedUuids.size() * 2 + 1);
    for (const auto& uuid : destroyedUuids)
        dead.insert(uuid);
    std::vector<PhysicsEvent> snapshot;
    snapshot.reserve(m_FrameEventAccum.size());
    for (const auto& e : m_FrameEventAccum)
    {
        if (dead.count(e.bodyA) != 0 || dead.count(e.bodyB) != 0)
            continue;
        snapshot.push_back(e);
    }
    // Exact-dup collapse keeping first occurrence: order-preserving safety
    // net over the per-tick per-pair coalescing (PhysicsEvents.h rule 6).
    std::set<PhysicsEvent> seen;
    std::vector<PhysicsEvent> published;
    published.reserve(snapshot.size());
    for (const auto& e : snapshot)
    {
        if (seen.insert(e).second)
            published.push_back(e);
    }
    m_PhysicsSnapshot = std::move(published);
    m_FrameEventAccum.clear();
}

// ============================================================================
// T7 queued physics commands (validated enqueue, pre-step drain)
// ============================================================================

namespace {

// T7 queue-time finite/range gate shared by every vector argument: Bullet
// stores velocities/impulses/positions in float, so a non-finite or
// out-of-float-range double must refuse BEFORE the cast (same discipline as
// the sink's set_position gate) — math.huge, NaN, and 1e300 never reach the
// queue, the drain, or Bullet.
bool T7FiniteVector(const glm::vec3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
           std::fabs(v.x) <= (float)FLT_MAX &&
           std::fabs(v.y) <= (float)FLT_MAX &&
           std::fabs(v.z) <= (float)FLT_MAX;
}

} // namespace

// Queue-time guard shared by every T7 enqueue: session mutability (silent
// false, like the existing sink setters), frozen destroy set (loud false +
// warn, the T6 refusal), and a committed physics world (loud false + warn).
// Returns true when the caller may proceed to per-op validation.
bool RuntimeSceneController::QueuePhysicsGuard(const UUID& target,
                                              const char* opName)
{
    if (!IsRuntimeMutable())
        return false;
    if (IsUuidInDestroyDrain(target))
    {
        printf("[Physics] %s refused for %s "
               "(entity is being destroyed in this safe-point drain)\n",
               opName, target.ToString().c_str());
        return false;
    }
    if (!m_Runtime || !m_PhysicsWorld)
    {
        printf("[Physics] %s refused for %s (no live physics world)\n",
               opName, target.ToString().c_str());
        return false;
    }
    return true;
}

// Queue-time body resolution: the UUID must resolve in the runtime document
// to an entity carrying a PhysicsBodyComponent. Returns the component (or
// null after a loud refusal) so each op can apply its kind rule.
const PhysicsBodyComponent* RuntimeSceneController::QueuePhysicsBody(
    const UUID& target, const char* opName)
{
    const auto entity = m_Runtime->FindByUuid(target);
    if (entity == entt::null || !m_Runtime->ecs.registry.valid(entity))
    {
        printf("[Physics] %s refused for %s (unknown entity)\n",
               opName, target.ToString().c_str());
        return nullptr;
    }
    const auto* body = m_Runtime->ecs.registry.try_get<PhysicsBodyComponent>(entity);
    if (body == nullptr)
    {
        printf("[Physics] %s refused for %s (no physics body)\n",
               opName, target.ToString().c_str());
        return nullptr;
    }
    return body;
}

bool RuntimeSceneController::QueueSetLinearVelocity(
    const UUID& target, const glm::vec3& velocity)
{
    constexpr const char* kOp = "set_velocity";
    if (!QueuePhysicsGuard(target, kOp))
        return false;
    if (!T7FiniteVector(velocity))
    {
        printf("[Physics] %s refused for %s (non-finite velocity)\n",
               kOp, target.ToString().c_str());
        return false;
    }
    const auto* body = QueuePhysicsBody(target, kOp);
    if (body == nullptr)
        return false;
    if (body->kind == PhysicsBodyKind::Static)
    {
        printf("[Physics] %s refused for %s "
               "(Static bodies are simulation-owned)\n",
               kOp, target.ToString().c_str());
        return false;
    }
    // T7 re-review P1(2): a rigid-body command needs a live rigid body, not
    // merely a body record. Ghost-only entities (triggers, including valid
    // Kinematic triggers) carry no solver velocity: accepting here would
    // report true for a write the drain must refuse, so refuse loudly now
    // and enqueue nothing.
    const PhysicsBodyRecord* record = m_PhysicsWorld->FindBody(target);
    if (record == nullptr || record->body == nullptr)
    {
        printf("[Physics] %s refused for %s (no live rigid body)\n",
               kOp, target.ToString().c_str());
        return false;
    }
    QueuedPhysicsCommand cmd;
    cmd.kind = QueuedPhysicsCommandKind::SetLinearVelocity;
    cmd.target = target;
    cmd.vector = velocity;
    m_PhysicsCommands.push_back(cmd);
    return true;
}

bool RuntimeSceneController::QueueApplyImpulse(const UUID& target,
                                              const glm::vec3& impulse)
{
    constexpr const char* kOp = "apply_impulse";
    if (!QueuePhysicsGuard(target, kOp))
        return false;
    if (!T7FiniteVector(impulse))
    {
        printf("[Physics] %s refused for %s (non-finite impulse)\n",
               kOp, target.ToString().c_str());
        return false;
    }
    const auto* body = QueuePhysicsBody(target, kOp);
    if (body == nullptr)
        return false;
    // Dynamic only (the PhysicsWorld entry point enforces the same rule at
    // apply time; refusing here too keeps the refusal loud and immediate).
    if (body->kind != PhysicsBodyKind::Dynamic)
    {
        printf("[Physics] %s refused for %s "
               "(impulse applies to Dynamic bodies only)\n",
               kOp, target.ToString().c_str());
        return false;
    }
    // T7 re-review P1(2): same live-rigid-body rule as set_velocity (a
    // Dynamic component whose record is ghost-only has no solver body to
    // take the impulse).
    const PhysicsBodyRecord* record = m_PhysicsWorld->FindBody(target);
    if (record == nullptr || record->body == nullptr)
    {
        printf("[Physics] %s refused for %s (no live rigid body)\n",
               kOp, target.ToString().c_str());
        return false;
    }
    QueuedPhysicsCommand cmd;
    cmd.kind = QueuedPhysicsCommandKind::ApplyImpulse;
    cmd.target = target;
    cmd.vector = impulse;
    m_PhysicsCommands.push_back(cmd);
    return true;
}

bool RuntimeSceneController::QueueSetHingeDrive(const UUID& owner,
                                               float velocity,
                                               float maxImpulse)
{
    constexpr const char* kOp = "set_hinge_drive";
    if (!QueuePhysicsGuard(owner, kOp))
        return false;
    if (!std::isfinite(velocity) || !std::isfinite(maxImpulse) ||
        maxImpulse < 0.0f)
    {
        printf("[Physics] %s refused for %s (bad drive parameters)\n",
               kOp, owner.ToString().c_str());
        return false;
    }
    const auto* body = QueuePhysicsBody(owner, kOp);
    if (body == nullptr)
        return false;
    const PhysicsConstraintRecord* rec =
        m_PhysicsWorld->FindConstraint(owner);
    if (rec == nullptr || !rec->isHinge)
    {
        printf("[Physics] %s refused for %s (no hinge constraint)\n",
               kOp, owner.ToString().c_str());
        return false;
    }
    QueuedPhysicsCommand cmd;
    cmd.kind = QueuedPhysicsCommandKind::SetHingeDrive;
    cmd.target = owner;
    cmd.paramA = velocity;
    cmd.paramB = maxImpulse;
    m_PhysicsCommands.push_back(cmd);
    return true;
}

bool RuntimeSceneController::QueueReleaseHingeDrive(const UUID& owner)
{
    constexpr const char* kOp = "release_hinge";
    if (!QueuePhysicsGuard(owner, kOp))
        return false;
    if (QueuePhysicsBody(owner, kOp) == nullptr)
        return false;
    const PhysicsConstraintRecord* rec =
        m_PhysicsWorld->FindConstraint(owner);
    if (rec == nullptr || !rec->isHinge)
    {
        printf("[Physics] %s refused for %s (no hinge constraint)\n",
               kOp, owner.ToString().c_str());
        return false;
    }
    QueuedPhysicsCommand cmd;
    cmd.kind = QueuedPhysicsCommandKind::ReleaseHingeDrive;
    cmd.target = owner;
    m_PhysicsCommands.push_back(cmd);
    return true;
}

bool RuntimeSceneController::QueueSetSliderTarget(const UUID& owner,
                                                 float target)
{
    constexpr const char* kOp = "set_slider_target";
    if (!QueuePhysicsGuard(owner, kOp))
        return false;
    if (!std::isfinite(target))
    {
        printf("[Physics] %s refused for %s (non-finite target)\n",
               kOp, owner.ToString().c_str());
        return false;
    }
    if (QueuePhysicsBody(owner, kOp) == nullptr)
        return false;
    const PhysicsConstraintRecord* rec =
        m_PhysicsWorld->FindConstraint(owner);
    if (rec == nullptr || rec->isHinge)
    {
        printf("[Physics] %s refused for %s (no slider constraint)\n",
               kOp, owner.ToString().c_str());
        return false;
    }
    // Outside the limits is a loud refusal, never a clamp (same rule as the
    // immediate C++ entry point — the queue-time check keeps it immediate).
    if (target < rec->lowerLimit || target > rec->upperLimit)
    {
        printf("[Physics] %s refused for %s "
               "(target outside slider limits)\n",
               kOp, owner.ToString().c_str());
        return false;
    }
    QueuedPhysicsCommand cmd;
    cmd.kind = QueuedPhysicsCommandKind::SetSliderTarget;
    cmd.target = owner;
    cmd.paramA = target;
    m_PhysicsCommands.push_back(cmd);
    return true;
}

bool RuntimeSceneController::QueueReleaseSlider(const UUID& owner,
                                               float impulse)
{
    constexpr const char* kOp = "release_slider";
    if (!QueuePhysicsGuard(owner, kOp))
        return false;
    if (!std::isfinite(impulse))
    {
        printf("[Physics] %s refused for %s (non-finite impulse)\n",
               kOp, owner.ToString().c_str());
        return false;
    }
    if (QueuePhysicsBody(owner, kOp) == nullptr)
        return false;
    const PhysicsConstraintRecord* rec =
        m_PhysicsWorld->FindConstraint(owner);
    if (rec == nullptr || rec->isHinge)
    {
        printf("[Physics] %s refused for %s (no slider constraint)\n",
               kOp, owner.ToString().c_str());
        return false;
    }
    QueuedPhysicsCommand cmd;
    cmd.kind = QueuedPhysicsCommandKind::ReleaseSlider;
    cmd.target = owner;
    cmd.paramA = impulse;
    m_PhysicsCommands.push_back(cmd);
    return true;
}

bool RuntimeSceneController::QueueResetBodyPose(
    const UUID& target, const glm::vec3& position, const glm::quat& rotation,
    bool hasLinearVelocity, const glm::vec3& linearVelocity,
    bool hasAngularVelocity, const glm::vec3& angularVelocity)
{
    constexpr const char* kOp = "reset_body_pose";
    if (!QueuePhysicsGuard(target, kOp))
        return false;
    // Shape validation here is queue-time loudness only: ResetBodyPose
    // re-validates the same predicates atomically at apply time (no partial
    // state change on any failure), so a command that passes here cannot
    // half-apply there.
    if (!T7FiniteVector(position))
    {
        printf("[Physics] %s refused for %s (non-finite position)\n",
               kOp, target.ToString().c_str());
        return false;
    }
    if (!std::isfinite(rotation.x) || !std::isfinite(rotation.y) ||
        !std::isfinite(rotation.z) || !std::isfinite(rotation.w))
    {
        printf("[Physics] %s refused for %s (non-finite rotation)\n",
               kOp, target.ToString().c_str());
        return false;
    }
    // Degenerate (non-normalizable) rotations refuse here too: the apply
    // path re-validates, but the queue must never hold a command that is
    // known-bad at enqueue time (same overflow-safe norm discipline as the
    // sink transform gate and the apply path).
    {
        const double dx = (double)rotation.x;
        const double dy = (double)rotation.y;
        const double dz = (double)rotation.z;
        const double dw = (double)rotation.w;
        const double norm =
            std::sqrt(dx * dx + dy * dy + dz * dz + dw * dw);
        if (!std::isfinite(norm) || !(norm > 1e-6) || norm > (double)FLT_MAX)
        {
            printf("[Physics] %s refused for %s (degenerate rotation)\n",
                   kOp, target.ToString().c_str());
            return false;
        }
    }
    if ((hasLinearVelocity && !T7FiniteVector(linearVelocity)) ||
        (hasAngularVelocity && !T7FiniteVector(angularVelocity)))
    {
        printf("[Physics] %s refused for %s (non-finite velocity)\n",
               kOp, target.ToString().c_str());
        return false;
    }
    const auto* body = QueuePhysicsBody(target, kOp);
    if (body == nullptr)
        return false;
    // Dynamic/Kinematic only. Static refuses (baked at Play); pure ghosts
    // without a body component never reach here (QueuePhysicsBody).
    if (body->kind == PhysicsBodyKind::Static)
    {
        printf("[Physics] %s refused for %s "
               "(Static bodies cannot be reset)\n",
               kOp, target.ToString().c_str());
        return false;
    }
    // T7 re-review P1(2): reset needs the live rigid body, not merely the
    // record. Ghost-only entities (including valid Kinematic triggers)
    // refuse here instead of teleporting a trigger in the apply path.
    const PhysicsBodyRecord* record = m_PhysicsWorld->FindBody(target);
    if (record == nullptr || record->body == nullptr)
    {
        printf("[Physics] %s refused for %s (no live rigid body)\n",
               kOp, target.ToString().c_str());
        return false;
    }
    QueuedPhysicsCommand cmd;
    cmd.kind = QueuedPhysicsCommandKind::ResetBodyPose;
    cmd.target = target;
    cmd.position = position;
    cmd.rotation = rotation;
    cmd.hasResetLinear = hasLinearVelocity;
    cmd.resetLinear = linearVelocity;
    cmd.hasResetAngular = hasAngularVelocity;
    cmd.resetAngular = angularVelocity;
    m_PhysicsCommands.push_back(cmd);
    return true;
}

void RuntimeSceneController::DrainPhysicsCommands()
{
    if (m_PhysicsCommands.empty())
        return;
    // Move to a local batch: commands queued re-entrantly while draining
    // (only possible from future entry points — no script callback runs
    // inside this drain today) land in the next boundary, never in the
    // running batch. The queue clears even when the world is gone, so Stop
    // and failed sessions cannot strand commands.
    std::vector<QueuedPhysicsCommand> batch;
    batch.swap(m_PhysicsCommands);
    if (!m_Runtime || !m_PhysicsWorld)
        return;
    for (const auto& cmd : batch)
    {
        // Raced teardown since queue time (the structural drain runs after
        // the fixed ticks and may have removed the body): skip silently.
        // The queue-time validation already proved the command well-formed.
        if (IsUuidInDestroyDrain(cmd.target))
            continue;
        if (m_Runtime->FindByUuid(cmd.target) == entt::null)
            continue;
        // T7 re-review P1(2): any other apply-time failure is unexpected —
        // the queue gate proved a live rigid body — so it reports loudly
        // (UUID + op named) instead of vanishing. The drain continues with
        // the next command; the failed command already left no partial
        // state (every apply path validates before mutating).
        bool applied = false;
        const char* opName = "unknown";
        switch (cmd.kind)
        {
        case QueuedPhysicsCommandKind::SetLinearVelocity:
            opName = "set_velocity";
            applied =
                m_PhysicsWorld->SetBodyLinearVelocity(cmd.target, cmd.vector);
            break;
        case QueuedPhysicsCommandKind::ApplyImpulse:
            opName = "apply_impulse";
            applied =
                m_PhysicsWorld->ApplyBodyImpulse(cmd.target, cmd.vector);
            break;
        case QueuedPhysicsCommandKind::SetHingeDrive:
            opName = "set_hinge_drive";
            applied = m_PhysicsWorld->SetHingeDrive(cmd.target, cmd.paramA,
                                                   cmd.paramB);
            break;
        case QueuedPhysicsCommandKind::ReleaseHingeDrive:
            opName = "release_hinge";
            applied = m_PhysicsWorld->ReleaseHingeDrive(cmd.target);
            break;
        case QueuedPhysicsCommandKind::SetSliderTarget:
            opName = "set_slider_target";
            applied =
                m_PhysicsWorld->SetSliderTarget(cmd.target, cmd.paramA);
            break;
        case QueuedPhysicsCommandKind::ReleaseSlider:
            opName = "release_slider";
            applied =
                m_PhysicsWorld->ReleaseSlider(cmd.target, cmd.paramA);
            break;
        case QueuedPhysicsCommandKind::ResetBodyPose:
            opName = "reset_body_pose";
            applied = m_PhysicsWorld->ResetBodyPose(
                *m_Runtime, cmd.target, cmd.position, cmd.rotation,
                cmd.hasResetLinear, cmd.resetLinear, cmd.hasResetAngular,
                cmd.resetAngular);
            break;
        }
        if (!applied)
        {
            printf("[Physics] %s for %s failed at the pre-step drain "
                   "(unexpected: queue validation passed; command dropped)\n",
                   opName, cmd.target.ToString().c_str());
        }
    }
}

// ============================================================================
// Internal helpers
// ============================================================================

void RuntimeSceneController::InitPrevTransforms()
{
    if (!m_Runtime)
        return;
    auto& reg = m_Runtime->ecs.registry;
    SceneGraph::UpdateWorldTransforms(reg);
    auto view = reg.view<Transform>();
    for (auto e : view)
    {
        auto& tf = view.get<Transform>(e);
        tf.prevWorldMatrix = tf.worldMatrix;
    }
}

void RuntimeSceneController::SnapshotPrevTransforms()
{
    if (!m_Runtime)
        return;
    auto& reg = m_Runtime->ecs.registry;
    auto view = reg.view<Transform>();
    for (auto e : view)
    {
        auto& tf = view.get<Transform>(e);
        tf.prevWorldMatrix = tf.worldMatrix;
    }
}

void RuntimeSceneController::RunFixedTick(float dt)
{
    if (!m_Runtime)
        return;

    auto& reg = m_Runtime->ecs.registry;

    // Phase 6: fixed script callbacks BEFORE motion integration, in UUID-
    // sorted entity order (game-loop.md:134). Spawns queued here do NOT
    // resolve mid-loop; they resolve at the safe point after the loop.
    if (m_ScriptDispatch)
        m_ScriptDispatch->OnFixedUpdate(dt);

    // MotionSystem: iterate entities with MotionComponent + Transform,
    // sorted by UUID for deterministic iteration order.
    std::vector<entt::entity> entities;
    auto view = reg.view<MotionComponent, Transform>();
    for (auto e : view)
        entities.push_back(e);

    // Sort by EntityIdComponent UUID for stable cross-load iteration order.
    std::sort(entities.begin(), entities.end(),
              [&reg](entt::entity a, entt::entity b) {
                  auto* ia = reg.try_get<EntityIdComponent>(a);
                  auto* ib = reg.try_get<EntityIdComponent>(b);
                  if (ia && ib) return ia->id < ib->id;
                  return false;
              });

    for (auto e : entities)
    {
        auto& mc = reg.get<MotionComponent>(e);
        auto& tf = reg.get<Transform>(e);
        tf.translation += mc.linearVelocity * dt;
        SceneGraph::SetLocalDirty(reg, e);
    }

    // T3: exactly one Bullet step per RT2 fixed tick. The outer accumulator
    // is the sole substepper (PhysicsWorld::Step performs stepSimulation(dt,
    // 0)); an entity can never carry both MotionComponent and a physics body
    // (Play refuses it), so motion integration and the physics step never
    // fight over one transform.
    //
    // T4 authority around the step: kinematic bodies push (ECS/script ->
    // Bullet) before the step; simulated dynamic bodies write back (Bullet ->
    // ECS, marked dirty for the single batched SceneGraph + TransformSync
    // pass per presentation frame) after it. Static bodies are baked once at
    // Play and never touched here.
    //
    // T7: the queued physics-command drain runs here — after OnFixedUpdate
    // scripts and Motion have written, before PreStepSync pushes kinematics
    // and the solver steps. A command queued from OnFixedUpdate therefore
    // affects the immediately following step of this same tick; a command
    // queued from OnUpdate (or a timer) waits for the next frame's first
    // tick. Either way the latency is at most one fixed tick.
    DrainPhysicsCommands();
    if (m_PhysicsWorld)
    {
        m_PhysicsWorld->PreStepSync(*m_Runtime);
        m_PhysicsWorld->Step(dt);
        m_PhysicsWorld->PostStepSync(*m_Runtime);
        // T6: scrape this tick's contact manifolds and ghost overlaps into
        // the frame accumulator. The tick index stamps the frame-local
        // sequence (0..4); PublishPhysicsSnapshot (after the safe-point
        // drain) filters destroys and publishes before OnUpdate.
        m_PhysicsWorld->AppendTickEvents(m_FrameEventAccum,
                                         m_PhysicsTickIndex++);
    }
}

} // namespace rt2::core