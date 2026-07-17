#include "SceneRenderBridge.h"
#include "RendererGPU.h"

SceneRenderBridge::SceneRenderBridge(RendererGPU& renderer)
	: m_Renderer(renderer)
{
}

void SceneRenderBridge::FullSync(GPUSceneData& data)
{
	m_Renderer.SetScene(data);
}

void SceneRenderBridge::MaterialSync(GPUSceneData& data)
{
	m_Renderer.SetSceneKeepTextures(data);
}

void SceneRenderBridge::TransformSync(GPUSceneData& data)
{
	m_Renderer.UpdateSceneInstances(data);
}

void SceneRenderBridge::ResetTemporalState()
{
	m_Renderer.ResetAccumulation();
	m_Renderer.InvalidateReSTIRHistory();
	m_Renderer.InvalidateGIHistory();
}

void SceneRenderBridge::RequestRender()
{
	// The interactive app drives rendering from its own OnUpdate loop;
	// this is a no-op here. RT2SliceRunner's bridge records the call.
}