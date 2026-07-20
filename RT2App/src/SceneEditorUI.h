#pragma once

#ifndef SCENE_EDITOR_UI_H
#define SCENE_EDITOR_UI_H

#include "SceneManager.h"
#include "EditorSceneState.h"
#include "EditorCommandHistory.h"
#include "EditorStructuralCommands.h"
#include "EditorPropertyCommands.h"
#include "PropertyEditSession.h"
#include "TransformEditing.h"
#include "core/UUID.h"
#include <functional>
#include <filesystem>
#include <optional>
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

	// Called when the user confirms the Import Options modal. The host
	// dispatches by extension: OBJ -> SceneManager::ImportObj(path, settings);
	// glTF -> SceneManager::ImportGltf(path) (settings ignored for glTF).
	// Returns the wrapper root entity ID.
	void SetOnImportWithOptions(
		std::function<SceneManager::EntityId(const std::string&, const ImportSettings&)> cb)
	{ m_OnImportWithOptions = std::move(cb); }

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

	// Phase 3A: non-owning command history injection. WalnutApp owns the
	// history and injects a pointer so editor UI commands and the public
	// Undo/Redo below share one history instance.
	void SetCommandHistory(EditorCommandHistory* history) { m_CommandHistory = history; }

	// Phase 3A: public undo/redo entry points. Each runs the history and
	// routes the returned mutation result through the existing private
	// ApplyMutation() — one sync path, one m_MutationError display. These
	// bypass the editor lock gate (locks gate initiation in
	// MutationSelectionAllowed above the command layer) and the Play guard
	// (the host suppresses these calls during Play).
	void Undo();
	void Redo();

	bool CanUndo() const { return m_CommandHistory && m_CommandHistory->CanUndo(); }
	bool CanRedo() const { return m_CommandHistory && m_CommandHistory->CanRedo(); }
	std::string UndoDescription() const
	{ return m_CommandHistory ? m_CommandHistory->UndoDescription() : std::string{}; }
	std::string RedoDescription() const
	{ return m_CommandHistory ? m_CommandHistory->RedoDescription() : std::string{}; }

	// Set whether authoring edits are allowed. During Play, the UI is
	// read-only (bound to the runtime scene).
	void SetEditable(bool editable) { m_Editable = editable; }
	bool IsEditable() const { return m_Editable; }

	void SelectUuid(const rt2::core::UUID& uuid) { m_State.Selection().SelectOnly(uuid); }
	void ClearSelection() { m_State.Selection().Clear(); }
	void ResetForDocument()
	{
		m_State.ResetForDocument();
		m_SearchBuffer[0] = '\0';
		m_TransformSession.Discard();
		m_NameSession.Discard();
		m_LightSession.Discard();
		m_CameraSession.Discard();
		m_MaterialIndexSession.Discard();
		m_MaterialPropertiesSession.Discard();
		m_MotionVelocitySession.Discard();
	}
	EditorSelection& Selection() { return m_State.Selection(); }
	const EditorSelection& Selection() const { return m_State.Selection(); }
	bool SelectionHasDirectLock() const
	{ return m_State.AnyDirectlyLocked(m_State.Selection().Ordered()); }
	TransformSpace GetTransformSpace() const { return m_TransformSpace; }
	TransformPivot GetTransformPivot() const { return m_TransformPivot; }
	const TransformSnapSettings& GetTransformSnapSettings() const { return m_TransformSnap; }
	bool GetUniformScale() const { return m_UniformScale; }
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
	void DrawImportOptionsModal();

	void NotifySceneChanged();
	void NotifyTransformChanged();
	void ApplyMutation(const EditorMutationResult& result, bool selectAffected = false);
	bool MutationSelectionAllowed(std::string& reason) const;
	bool MatchesSearch(SceneManager::EntityId entity) const;
	SceneManager::EntityId SelectedEntity() const;
	void SelectEntity(SceneManager::EntityId entity, bool toggle = false);
	bool IsSelected(SceneManager::EntityId entity) const;

	// Phase 3B2: property command helpers. Each captures the before-state,
	// applies the per-frame mutation via the manager, records the command
	// via RecordApplied on close. The state machine (PropertyEditSession)
	// handles deferred-close ordering and defensive guards.
	void RecordNameEdit(const rt2::core::UUID& target,
	                    const std::string& before, const std::string& after);
	void RecordLightEdit(const rt2::core::UUID& target,
	                     const LightComponent& before, const LightComponent& after);
	void RecordCameraEdit(const rt2::core::UUID& target,
	                      const CameraComponent& before, const CameraComponent& after);
	void RecordMaterialIndexEdit(const rt2::core::UUID& target,
	                             int beforeIndex, int afterIndex);
	void RecordMaterialPropertiesEdit(int slotIndex,
	                                 const SceneMaterial& before,
	                                 const SceneMaterial& after);
	void RecordMotionEdit(const rt2::core::UUID& target,
	                      const std::optional<MotionComponent>& before,
	                      const std::optional<MotionComponent>& after);
	// Capture the current MaterialOverrideComponent state of every imported
	// entity referencing `slotIndex` (UUID -> override). Used to snapshot
	// the before-overrides when a material-properties session opens and the
	// after-overrides when it closes.
	SetMaterialPropertiesCommand::OverrideList CaptureMaterialOverrideListForSlot(int slotIndex) const;
	// Hide/Show the current selection as a single SetVisibilityCommand.
	void HideShowSelectionCommand(bool hide);

	// Phase 3B1: structural command helpers. Each reserves a known UUID,
	// applies the creation via the manager, captures the resulting
	// SubtreeSnapshot, constructs the command, and records it via
	// RecordApplied. On snapshot-capture failure the initial creation is
	// rolled back via RemoveSubtreesNoCompact.
	void CreateEmptyCommand(const std::optional<rt2::core::UUID>& parent);
	void CreatePrimitiveCommand(PrimitiveComponent::Kind kind, float size,
	                            const char* name, const glm::vec3& position);
	void CreateLightCommand(const glm::vec3& position, const glm::vec3& color,
	                        float intensity);
	// Delete the selection as a single RemoveSubtreesCommand.
	void DeleteSelectionCommand();
	// Duplicate the selection as a single DuplicateSubtreesCommand.
	void DuplicateSelectionCommand();
	// Paste the clipboard as a single PasteSubtreesCommand.
	void PasteCommand(const std::optional<rt2::core::UUID>& parent);
	// Reparent the selection as a single ReparentCommand.
	void ReparentCommand(const std::vector<rt2::core::UUID>& sources,
	                     const rt2::core::UUID& newParent);
	// Single-entity Hide/Show via SetVisibilityCommand (migrated from the
	// direct SetVisibility call).
	void SingleEntityHideShowCommand(const rt2::core::UUID& entity, bool hide);

	SceneManager* m_SceneMgr = nullptr;

	EditorSceneState m_State;
	std::function<void()> m_OnSceneChanged;
	std::function<void()> m_OnTransformChanged;
	std::function<void(rt2::core::SyncImpact)> m_OnMutation;
	std::function<SceneManager::EntityId(const std::string&)> m_OnLoadMeshFile;
	std::function<SceneManager::EntityId(const std::string&)> m_OnImportGltf;
	std::function<SceneManager::EntityId(const std::string&, const ImportSettings&)> m_OnImportWithOptions;
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
	bool m_UniformScale = false;
	std::string m_TransformEditError;
	std::string m_MutationError;
	char m_SearchBuffer[128]{};

	// Phase 3B2: record-on-release property edit sessions. The state
	// machine (PropertyEditSession<T>) is a pure template; the ImGui glue
	// (activation/deactivation detection + deferred close after the
	// mutation block) lives in the Render*Editor functions. Each session
	// is a single active slot per property kind.
	using TransformSession = PropertyEditSession<EditableTRS>;
	using NameSession = PropertyEditSession<std::string>;
	using LightSession = PropertyEditSession<LightComponent>;
	using CameraSession = PropertyEditSession<CameraComponent>;
	using MaterialIndexSession = PropertyEditSession<int>;
	using MaterialPropertiesSession = PropertyEditSession<SceneMaterial>;
	using MotionSession = PropertyEditSession<MotionComponent>;

	EditorCommandHistory* m_CommandHistory = nullptr;

	TransformSession             m_TransformSession;
	NameSession                  m_NameSession;
	LightSession                 m_LightSession;
	CameraSession                m_CameraSession;
	MaterialIndexSession         m_MaterialIndexSession;
	MaterialPropertiesSession    m_MaterialPropertiesSession;
	MotionSession                m_MotionVelocitySession;
	// ImGui widget IDs that opened each multi-widget session (so the
	// deactivation close only fires for the owning widget). ImGui IDs are
	// stored as unsigned int to avoid depending on ImGui headers here.
	unsigned int m_TransformSessionOwningWidgetId = 0;
	unsigned int m_LightSessionOwningWidgetId = 0;
	unsigned int m_CameraSessionOwningWidgetId = 0;
	unsigned int m_MaterialPropertiesSessionOwningWidgetId = 0;
	// Before-override snapshot captured when the material-properties session
	// opens. The after-overrides are read live at close time. The session
	// itself stores the SceneMaterial before/after; this stores the
	// per-entity override side effect.
	SetMaterialPropertiesCommand::OverrideList m_PendingMaterialPropertiesBeforeOverrides;

	// Import Options modal state. When m_ImportOptionsOpen is true, a file
	// path has been picked and the modal is shown. The modal collects the
	// import settings (currently just mergeMegaMesh for OBJ) and dispatches
	// via m_OnImportWithOptions on confirm, or clears the pending path on
	// cancel.
	bool        m_ImportOptionsOpen = false;
	std::string m_PendingImportPath;
	bool        m_PendingImportMergeMegaMesh = true;
};

#endif // SCENE_EDITOR_UI_H
