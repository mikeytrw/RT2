#pragma once

#ifndef RT2_CORE_SCENE_SYNC_IMPACT_H
#define RT2_CORE_SCENE_SYNC_IMPACT_H

// Renderer-neutral CPU vocabulary for scene mutations.  Keeping this in a
// leaf header lets scene contracts and CPU-only targets communicate impact
// without importing GPUSceneData, shader interfaces, or the render bridge.
namespace rt2::core {

enum class SyncImpact
{
    None,
    Transform,
    Material,
    Structural,
};

} // namespace rt2::core

#endif // RT2_CORE_SCENE_SYNC_IMPACT_H
