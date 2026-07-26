#include "RuntimeSceneController.h"
#include "SceneSerializer.h"
#include "SceneGraph.h"
#include "SceneHierarchy.h"
#include "ECSComponents.h"
#include "GPUSceneData.h"
#include "core/UUID.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <unordered_set>
#include <vector>

namespace rt2::core {

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
    InitPrevTransforms();

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

    // Snapshot prev transforms before the step.
    SnapshotPrevTransforms();

    // Run one fixed update tick.
    RunFixedTick(dt);

    // Safe point: drain the deferred queue. The queue is drained in both
    // Update and Step so a queued op is processed on the next simulation
    // tick regardless of Pause state.
    Error drainErr;
    std::vector<UUID> createdThisBatch;
    const bool structural = ApplyDeferredStructuralChanges(drainErr, createdThisBatch);
    if (!drainErr.IsOk())
        printf("[Runtime] Step deferred-queue validation failed: %s (queue left intact)\n",
               drainErr.Format().c_str());

    // Phase 6: sync script environments with the runtime registry after the
    // safe point (G2). Fires OnCreate for newly applied entities, OnDestroy
    // for destroyed ones, before OnUpdate runs.
    if (m_ScriptDispatch)
        m_ScriptDispatch->SyncScriptEnvironments();

    // Phase 6: variable script callbacks after the safe point + env sync,
    // before SceneGraph::UpdateWorldTransforms (game-loop.md:137). Step
    // runs exactly one OnUpdate at kFixedDt for determinism.
    if (m_ScriptDispatch)
        m_ScriptDispatch->OnUpdate(dt);

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

    // 3. Clear any pending operations (they are runtime-only).
    m_PendingOperations.clear();

    // 4. Destroy the runtime document and all runtime-only state.
    m_Runtime.reset();

    // 5. Re-activate the authoring document for rendering: full upload +
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

    // 6. Reset state.
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
    const bool structural = ApplyDeferredStructuralChanges(drainErr, createdThisBatch);
    if (!drainErr.IsOk())
        printf("[Runtime] Update deferred-queue validation failed: %s (queue left intact)\n",
               drainErr.Format().c_str());

    // Phase 6: sync script environments with the runtime registry after the
    // safe point (G2). Fires OnCreate for newly applied entities, OnDestroy
    // for destroyed ones, before OnUpdate runs.
    if (m_ScriptDispatch)
        m_ScriptDispatch->SyncScriptEnvironments();

    // Phase 6: variable script callbacks after the safe point + env sync,
    // before SceneGraph::UpdateWorldTransforms (game-loop.md:137).
    if (m_ScriptDispatch)
        m_ScriptDispatch->OnUpdate(dt);

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

    m_PendingOperations.push_back(DestroyRuntimeSubtreeOperation{ uuid });
    return Result<void>::Ok();
}

bool RuntimeSceneController::ApplyDeferredStructuralChanges(
    Error& err, std::vector<UUID>& createdUuids)
{
    createdUuids.clear();

    if (!m_Runtime)
        return false;

    if (m_PendingOperations.empty())
        return false;

    auto& doc = *m_Runtime;

    // Phase 1: validate the complete batch before any mutation. Walk the
    // queue in order, building the set of UUIDs that will exist after each
    // operation. The validation state starts from the current runtime
    // document UUID set.
    std::unordered_set<UUID> existingUuids;
    for (const auto& [uuid, entity] : doc.uuidIndex.All())
        existingUuids.insert(uuid);

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
            const auto root = doc.FindByUuid(destroy->uuid);
            if (root != entt::null)
            {
                std::vector<entt::entity> subtree;
                SceneHierarchy::CollectSubtreePostOrder(doc.ecs.registry, root, subtree);
                for (const auto e : subtree)
                {
                    if (const auto* id = doc.ecs.registry.try_get<EntityIdComponent>(e))
                        existingUuids.erase(id->id);
                }
            }
        }
    }

    // Phase 2: apply the batch atomically in enqueue order via the mutator.
    // Post-validation, mutator failures are bugs — the validation phase
    // already checked every precondition (duplicate UUID, missing parent,
    // missing destroy target). A mutator Failure here means the validation
    // logic and the mutator disagree, which is a code bug. We assert in
    // debug and surface the error + clear the queue in release (the runtime
    // document may be partially mutated, but leaving the queue intact
    // would retry the same bug every frame).
    for (const auto& op : m_PendingOperations)
    {
        if (auto* create = std::get_if<CreateRuntimeEntityOperation>(&op))
        {
            auto r = m_Mutator.CreateEntity(doc, create->uuid, create->desc);
            if (!r.IsOk())
            {
                // Post-validation mutator failure is a bug (validation and
                // mutator disagree). Assert in debug so it's caught in
                // testing; in release, surface the error and clear the
                // queue to avoid retrying the same bug every frame.
                assert(false && "RuntimeSceneMutator::CreateEntity failed post-validation");
                err = r.error;
                m_PendingOperations.clear();
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

                    if (!uuids.empty())
                        m_ScriptDispatch->OnEntitiesDestroying(uuids);
                }
            }

            auto r = m_Mutator.DestroySubtree(doc, destroy->uuid);
            if (!r.IsOk())
            {
                assert(false && "RuntimeSceneMutator::DestroySubtree failed post-validation");
                err = r.error;
                m_PendingOperations.clear();
                return false;
            }
        }
    }

    // Phase 3: clear the queue. The batch is committed.
    m_PendingOperations.clear();

    // Phase 4 (post-apply): the caller will run SceneGraph::UpdateWorldTransforms
    // next, then set prevWorldMatrix = worldMatrix for every created entity
    // so the first frame's motion vectors are zero. We hand the created set
    // back via the out-param so the controller can finalize prevWorldMatrix
    // after UpdateWorldTransforms.

    return true;
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
}

} // namespace rt2::core