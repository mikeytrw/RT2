#pragma once

#ifndef RT2_CORE_I_SCENE_RENDER_BRIDGE_H
#define RT2_CORE_I_SCENE_RENDER_BRIDGE_H

#include "GPUSceneData.h"
#include "SceneSyncImpact.h"

// ============================================================================
// ISceneRenderBridge — narrow, Vulkan-free interface between CPU scene code
// and the renderer.
//
// SceneManager and RuntimeSceneController call this interface instead of
// RendererGPU directly. This keeps the scene core linkable into RT2Tests and
// RT2SliceRunner (which supply a null/recording implementation) while RT2App
// supplies a real implementation backed by RendererGPU.
//
// SyncImpact reflects the cross-phase SceneManager sync-impact contract:
//   None       — no GPU work
//   Transform  — instance transform buffer only (no BLAS/TLAS rebuild)
//   Material   — material buffer rebuild, textures preserved
//   Structural — full AS + texture + material rebuild
//
// ============================================================================

namespace rt2::core {

class ISceneRenderBridge
{
public:
    virtual ~ISceneRenderBridge() = default;

    // Full scene upload: textures, materials, meshes, AS rebuild.
    // Corresponds to RendererGPU::SetScene.
    virtual void FullSync(GPUSceneData& data) = 0;

    // Material/scene-data rebuild without re-uploading textures.
    // Corresponds to RendererGPU::SetSceneKeepTextures.
    virtual void MaterialSync(GPUSceneData& data) = 0;

    // Instance transform buffer update only (no AS rebuild).
    // Corresponds to RendererGPU::UpdateSceneInstances.
    virtual void TransformSync(GPUSceneData& data) = 0;

    // Reset temporal renderer state: accumulation, NRD history, ReSTIR
    // reservoirs. Called on scene switch, Play, and Stop.
    virtual void ResetTemporalState() = 0;

    // Request a render submission for the current frame. The CPU slice runner
    // implements this as a no-op; the interactive app drives Render().
    virtual void RequestRender() = 0;
};

} // namespace rt2::core

#endif // RT2_CORE_I_SCENE_RENDER_BRIDGE_H
