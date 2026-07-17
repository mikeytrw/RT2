#pragma once

#ifndef RT2_APP_SCENE_RENDER_BRIDGE_H
#define RT2_APP_SCENE_RENDER_BRIDGE_H

#include "ISceneRenderBridge.h"
#include "GPUSceneData.h"

class RendererGPU;

// ============================================================================
// SceneRenderBridge — adapts ISceneRenderBridge to RendererGPU.
//
// This is the app-side implementation of the narrow render bridge interface.
// RT2Tests and RT2SliceRunner supply a null/recording implementation instead.
// The bridge keeps scene code free of Vulkan/RendererGPU types while allowing
// the interactive app to drive the real renderer.
// ============================================================================

class SceneRenderBridge final : public rt2::core::ISceneRenderBridge
{
public:
	explicit SceneRenderBridge(RendererGPU& renderer);

	void FullSync(GPUSceneData& data) override;
	void MaterialSync(GPUSceneData& data) override;
	void TransformSync(GPUSceneData& data) override;
	void ResetTemporalState() override;
	void RequestRender() override;

private:
	RendererGPU& m_Renderer;
};

#endif // RT2_APP_SCENE_RENDER_BRIDGE_H