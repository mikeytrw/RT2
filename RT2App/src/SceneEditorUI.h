#pragma once

#ifndef SCENE_EDITOR_UI_H
#define SCENE_EDITOR_UI_H

#include "SceneManager.h"
#include <functional>
#include <string>

// ============================================================================
// SceneEditorUI — ImGui panels for scene editing (outliner + inspector).
//
// Two panels:
//   1. "Outliner" — lists all entities, allows selection, deletion, and
//      adding new entities (lights, primitive objects, mesh files).
//   2. "Inspector" — edits the selected entity's transform, material,
//      and light properties.
//
// The UI calls SceneManager APIs for all mutations. After any edit,
// it calls the provided OnSceneChanged callback so the host can sync
// to the GPU and reset accumulation.
//
// Usage:
//   SceneEditorUI editor;
//   editor.SetSceneMgr(&m_SceneMgr);
//   editor.SetOnSceneChanged([&]() { m_SceneMgr.SyncToGPU(); m_RendererGPU.ResetAccumulation(); });
//   editor.SetOnTransformChanged([&]() { m_SceneMgr.SyncTransformsToGPU(); m_RendererGPU.ResetAccumulation(); });
//   editor.SetOnLoadMeshFile([&](const std::string& path) { ... });
//   editor.RenderPanels();
//
// ============================================================================

class SceneEditorUI
{
public:
	SceneEditorUI() = default;

	void SetSceneMgr(SceneManager* mgr) { m_SceneMgr = mgr; }

	// Called when the scene structure changes (add/remove entity, material change).
	// Host should call SyncToGPU() + ResetAccumulation().
	void SetOnSceneChanged(std::function<void()> cb) { m_OnSceneChanged = std::move(cb); }

	// Called when only transforms change (move/rotate/scale).
	// Host should call SyncTransformsToGPU() + ResetAccumulation().
	void SetOnTransformChanged(std::function<void()> cb) { m_OnTransformChanged = std::move(cb); }

	// Called when user picks "Load Mesh File..." — host loads the OBJ/glTF
	// as geometry and adds it via AddObjectWithGeometry. Returns the entity ID.
	void SetOnLoadMeshFile(std::function<SceneManager::EntityId(const std::string&)> cb)
	{ m_OnLoadMeshFile = std::move(cb); }

	void RenderPanels();

private:
	void RenderOutliner();
	void RenderInspector();

	void RenderEntityTree(SceneManager::EntityId entity, int depth);
	void RenderTransformEditor(SceneManager::EntityId entity);
	void RenderMaterialEditor(SceneManager::EntityId entity);
	void RenderLightEditor(SceneManager::EntityId entity);
	void RenderCameraEditor(SceneManager::EntityId entity);

	void NotifySceneChanged();
	void NotifyTransformChanged();

	SceneManager* m_SceneMgr = nullptr;

	SceneManager::EntityId m_SelectedEntity;
	std::function<void()> m_OnSceneChanged;
	std::function<void()> m_OnTransformChanged;
	std::function<SceneManager::EntityId(const std::string&)> m_OnLoadMeshFile;

	// UI state for the "Add" popup
	bool m_ShowAddPopup = false;

	// Set when an entity was deleted during this frame's tree traversal.
	// The outliner checks this after each RenderEntityTree call and aborts
	// the remaining traversal (stale entity IDs would crash entt).
	bool m_TreeDirty = false;
};

#endif // SCENE_EDITOR_UI_H