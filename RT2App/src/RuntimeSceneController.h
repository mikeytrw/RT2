#pragma once

#ifndef RT2_CORE_RUNTIME_SCENE_CONTROLLER_H
#define RT2_CORE_RUNTIME_SCENE_CONTROLLER_H

#include "SceneDocument.h"
#include "ISceneRenderBridge.h"
#include "core/Error.h"
#include "ECSComponents.h"

#include <memory>

// ============================================================================
// RuntimeSceneController — owns the runtime scene clone and the Edit/Play/
// Pause lifecycle.
//
// The controller does NOT call RendererGPU or any Vulkan type directly. It
// communicates with the renderer through ISceneRenderBridge, so it links
// cleanly into RT2Tests and RT2SliceRunner (which supply a null/recording
// bridge) while RT2App supplies a real bridge backed by RendererGPU.
//
// Lifecycle:
//   Play(authoring):
//     1. Deep-clone authoring → m_Runtime via SceneSerializer::CloneInMemory.
//        UUIDs are preserved; transient state (gpuCache, dirty, prevTransforms)
//        is NOT cloned.
//     2. Initialize runtime prevWorldMatrix = worldMatrix for all transforms
//        so the first Play frame does not produce invalid motion vectors.
//     3. Activate the runtime document for rendering via the bridge:
//        FullSync (full GPUSceneData upload) + ResetTemporalState.
//   Pause:
//     Clear the accumulator so stale wall-clock time cannot become queued
//     simulation on resume. No simulation runs while paused.
//   Step:
//     Valid only while Paused. Runs exactly one fixed tick (MotionSystem +
//     deferred structural changes + SceneGraph + one batched sync) plus one
//     presentation pass. The accumulator is NOT advanced. Returns false if
//     not paused.
//   Stop:
//     1. Activate the authoring document for rendering via the bridge:
//        FullSync + ResetTemporalState.
//     2. Destroy m_Runtime and its runtime-only components.
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

private:
    // Initialize prevWorldMatrix = worldMatrix for all transforms in the
    // runtime document. Called once at Play to prevent invalid motion vectors
    // on the first frame.
    void InitPrevTransforms();

    // Snapshot current world matrices into prevWorldMatrix. Called before
    // each simulation step.
    void SnapshotPrevTransforms();

    // Run one fixed update tick (MotionSystem + SceneGraph).
    void RunFixedTick(float dt);

    std::unique_ptr<SceneDocument> m_Runtime;
    SceneRunState m_State = SceneRunState::Edit;
    float m_Accumulator = 0.0f;
};

} // namespace rt2::core

#endif // RT2_CORE_RUNTIME_SCENE_CONTROLLER_H