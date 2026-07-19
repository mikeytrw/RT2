#pragma once

#ifndef SCENE_EDITOR_UI_H
#define SCENE_EDITOR_UI_H

#include "SceneManager.h"
#include "EditorSceneState.h"
#include <functional>
#include <filesystem>
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
	void SetOnMutation(std::function<void(rt2::core::SyncImpact)> cb)
	{ m_OnMutation = std::move(cb); }

	// Called when user picks "Load Mesh File..." — host loads the OBJ/glTF
	// as geometry and adds it via AddObjectWithGeometry. Returns the entity ID.
	void SetOnLoadMeshFile(std::function<SceneManager::EntityId(const std::string&)> cb)
	{ m_OnLoadMeshFile = std::move(cb); }

	// Called when user picks "Import glTF..." — host imports the glTF into
	// the existing scene (merge, not replace). Returns the wrapper root entity ID.
	void SetOnImportGltf(std::function<SceneManager::EntityId(const std::string&)> cb)
	{ m_OnImportGltf = std::move(cb); }

	void SetDialogInitialDirectoryProvider(
		std::function<std::filesystem::path()> provider)
	{ m_DialogInitialDirectory = std::move(provider); }

	// Called when user clicks "Dump GPU Transforms" — host should call
	// RendererGPU::DumpInstanceTransforms().
	void SetOnDumpGPUTransforms(std::function<void()> cb)
	{ m_OnDumpGPUTransforms = std::move(cb); }

	void SetOnDumpNEEBuffers(std::function<void()> cb)
	{ m_OnDumpNEEBuffers = std::move(cb); }
	void SetOnViewThroughCamera(
		std::function<void(const rt2::core::UUID&)> cb)
	{ m_OnViewThroughCamera = std::move(cb); }
	void SetOnAlignCameraToView(
		std::function<void(const rt2::core::UUID&)> cb)
	{ m_OnAlignCameraToView = std::move(cb); }

	// Set whether authoring edits are allowed. During Play, the UI is
	// read-only (bound to the runtime scene).
	void SetEditable(bool editable) { m_Editable = editable; }
	bool IsEditable() const { return m_Editable; }

	void SelectUuid(const rt2::core::UUID& uuid) { m_State.Selection().SelectOnly(uuid); }
	void ClearSelection() { m_State.Selection().Clear(); }
	void ResetForDocument() { m_State.ResetForDocument(); m_SearchBuffer[0] = '\0'; }
	EditorSelection& Selection() { return m_State.Selection(); }
	const EditorSelection& Selection() const { return m_State.Selection(); }
	bool SelectionHasDirectLock() const
	{ return m_State.AnyDirectlyLocked(m_State.Selection().Ordered()); }
	TransformSpace GetTransformSpace() const { return m_TransformSpace; }
	TransformPivot GetTransformPivot() const { return m_TransformPivot; }
	const TransformSnapSettings& GetTransformSnapSettings() const { return m_TransformSnap; }
	bool CaptureCameraBookmark(size_t slot, const EditorCameraPose& pose)
	{ return m_State.CaptureCameraBookmark(slot, pose); }
	const EditorCameraPose* CameraBookmark(size_t slot) const
	{ return m_State.CameraBookmark(slot); }
	bool ClearCameraBookmark(size_t slot)
	{ return m_State.ClearCameraBookmark(slot); }

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
	void ApplyMutation(const EditorMutationResult& result, bool selectAffected = false);
	bool MutationSelectionAllowed(std::string& reason) const;
	bool MatchesSearch(SceneManager::EntityId entity) const;
	SceneManager::EntityId SelectedEntity() const;
	void SelectEntity(SceneManager::EntityId entity, bool toggle = false);
	bool IsSelected(SceneManager::EntityId entity) const;

	SceneManager* m_SceneMgr = nullptr;

	EditorSceneState m_State;
	std::function<void()> m_OnSceneChanged;
	std::function<void()> m_OnTransformChanged;
	std::function<void(rt2::core::SyncImpact)> m_OnMutation;
	std::function<SceneManager::EntityId(const std::string&)> m_OnLoadMeshFile;
	std::function<SceneManager::EntityId(const std::string&)> m_OnImportGltf;
	std::function<std::filesystem::path()> m_DialogInitialDirectory;
	std::function<void()> m_OnDumpGPUTransforms;
	std::function<void()> m_OnDumpNEEBuffers;
	std::function<void(const rt2::core::UUID&)> m_OnViewThroughCamera;
	std::function<void(const rt2::core::UUID&)> m_OnAlignCameraToView;

	// UI state for the "Add" popup
	bool m_ShowAddPopup = false;

	// Set when an entity was deleted during this frame's tree traversal.
	// The outliner checks this after each RenderEntityTree call and aborts
	// the remaining traversal (stale entity IDs would crash entt).
	bool m_TreeDirty = false;

	// When false, authoring controls are disabled (during Play).
	bool m_Editable = true;

	// Shared Inspector/viewport transform editing state.
	TransformSpace m_TransformSpace = TransformSpace::Local;
	TransformPivot m_TransformPivot = TransformPivot::Primary;
	TransformSnapSettings m_TransformSnap;
	std::string m_TransformEditError;
	std::string m_MutationError;
	char m_SearchBuffer[128]{};
};

#endif // SCENE_EDITOR_UI_H
