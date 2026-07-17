#include "RuntimeSceneController.h"
#include "SceneSerializer.h"
#include "SceneGraph.h"
#include "ECSComponents.h"
#include "GPUSceneData.h"

#include <algorithm>
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

    // Deep-clone the authoring document into the runtime document.
    m_Runtime = std::make_unique<SceneDocument>();
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
    // For the vertical slice, we do a full sync on Play and Stop. Keep-
    // textures optimization can come later.
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

    m_State = SceneRunState::Playing;
    m_Accumulator = 0.0f;
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

    // Update world transforms (batched — once after simulation).
    SceneGraph::UpdateWorldTransforms(m_Runtime->ecs.registry);

    // One batched transform sync for the presentation pass.
    // The slice only has motion (no structural changes during Play), so
    // the sync impact is always Transform.
    GPUSceneData gpuData = m_Runtime->gpuCache;
    UpdateInstancesFromECS(gpuData, m_Runtime->ecs);
    m_Runtime->gpuCache = gpuData;
    bridge.TransformSync(gpuData);

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

    // Destroy the runtime document and all runtime-only state.
    m_Runtime.reset();

    // Re-activate the authoring document for rendering: full upload + temporal
    // reset. The authoring document was never mutated during Play, so this
    // restores the exact pre-Play visual state.
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
    // Update the authoring document's gpuCache (it may have been stale).
    const_cast<SceneDocument&>(authoring).gpuCache = gpuData;
    bridge.FullSync(gpuData);
    bridge.ResetTemporalState();

    m_State = SceneRunState::Edit;
    m_Accumulator = 0.0f;
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
    // (Alternative: carry it to next frame, but that can cause cascading
    // catch-up. The drop policy is documented in game-loop.md.)
    if (substeps == kMaxSubsteps)
        m_Accumulator = 0.0f;

    // Batched: update world transforms once after all substeps.
    SceneGraph::UpdateWorldTransforms(m_Runtime->ecs.registry);

    // One batched transform-only sync per rendered frame.
    GPUSceneData gpuData = m_Runtime->gpuCache;
    UpdateInstancesFromECS(gpuData, m_Runtime->ecs);
    m_Runtime->gpuCache = gpuData;
    bridge.TransformSync(gpuData);

    bridge.RequestRender();
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

    // Deferred structural changes would be applied here at the defined safe
    // point. The vertical slice has no structural changes during Play.
}

} // namespace rt2::core