#pragma once

#ifndef SCENE_EDITOR_UI_H
#define SCENE_EDITOR_UI_H

#include "SceneManager.h"
#include "EditorSceneState.h"
#include "EditorCommandHistory.h"
#include "EditorStructuralCommands.h"
#include "EditorPropertyCommands.h"
#include "PropertyEditSession.h"
#include "CompositePreviewSession.h"
#include "PreviewSessionClose.h"
#include "TransformEditing.h"
#include "core/UUID.h"
#include <functional>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace rt2::core { class ScriptFieldRegistry; }

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
	void SetScriptDialogInitialDirectoryProvider(
		std::function<std::filesystem::path()> provider)
	{ m_ScriptDialogInitialDirectory = std::move(provider); }

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

	void SetFieldRegistry(rt2::core::ScriptFieldRegistry* registry) { m_FieldRegistry = registry; }

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
	// read-only (bound to the runtime scene). Leaving editability finalizes or
	// compensates any open live-preview session so no authored preview
	// value/marker/schema is orphaned without a history entry (S6-C fixup,
	// P1 finding 4 — editability is never discard proof).
	void SetEditable(bool editable);
	bool IsEditable() const { return m_Editable; }

	void SelectUuid(const rt2::core::UUID& uuid) { m_State.Selection().SelectOnly(uuid); }
	void ClearSelection() { m_State.Selection().Clear(); }
	void ResetForDocument()
	{
		m_State.ResetForDocument();
		m_SearchBuffer[0] = '\0';
		DiscardAllPropertySessions();
	}
	EditorSelection& Selection() { return m_State.Selection(); }
	const EditorSelection& Selection() const { return m_State.Selection(); }
	bool SelectionHasDirectLock() const
	{ return m_State.AnyDirectlyLocked(m_State.Selection().Ordered()); }
	TransformSpace GetTransformSpace() const { return m_TransformSpace; }
	TransformPivot GetTransformPivot() const { return m_TransformPivot; }
	const TransformSnapSettings& GetTransformSnapSettings() const { return m_TransformSnap; }
	bool IsTransformGestureOpen() const { return m_TransformPreviewSession.IsOpen(); }
	std::optional<TransformGestureToken> BeginTransformGestureForGizmo(std::uint64_t owner,
		const std::vector<rt2::core::UUID>& uuids);
	EditorMutationResult PreviewTransformWorldIntent(
		const TransformGestureToken& token,
		const std::vector<std::pair<rt2::core::UUID, glm::mat4>>& worlds);
	PreviewSessionCloseOutcome CloseTransformGesture(bool finalize,
		const TransformGestureToken& token);
	bool GetUniformScale() const { return m_UniformScale; }
	bool CaptureCameraBookmark(size_t slot, const EditorCameraPose& pose)
	{ return m_State.CaptureCameraBookmark(slot, pose); }
	const EditorCameraPose* CameraBookmark(size_t slot) const
	{ return m_State.CameraBookmark(slot); }
	bool ClearCameraBookmark(size_t slot)
	{ return m_State.ClearCameraBookmark(slot); }

	void RenderPanels();

	// Dispatch a browser asset drop through the existing import callbacks.
	// The browser owns the payload; this class only owns the established
	// scene-import callback path.
	void ImportAssetPathFromDrop(const std::string& path);

	// Per-panel visibility (View menu toggles). The Outliner/Inspector can
	// be hidden without affecting the other panel.
	bool IsOutlinerVisible() const { return m_ShowOutliner; }
	void SetOutlinerVisible(bool v) { m_ShowOutliner = v; }
	bool IsInspectorVisible() const { return m_ShowInspector; }
	void SetInspectorVisible(bool v) { m_ShowInspector = v; }

	// Public hooks for async completion callbacks (host calls these after
	// a background load/import finishes).
	void OnImportComplete(SceneManager::EntityId importedRoot)
	{
		if (importedRoot.IsValid())
			SelectEntity(importedRoot);
		NotifySceneChanged();
	}

private:
	void RenderOutliner();
	void RenderInspector();
	void RenderAssetDropTarget();

	void RenderEntityTree(SceneManager::EntityId entity, int depth);
	void RenderTransformEditor(SceneManager::EntityId entity);
	void RenderMaterialEditor(SceneManager::EntityId entity);
	void RenderLightEditor(SceneManager::EntityId entity);
	void RenderCameraEditor(SceneManager::EntityId entity);
	void RenderScriptEditor(SceneManager::EntityId entity);
	void DrawImportOptionsModal();

	void NotifySceneChanged();
	void NotifyTransformChanged();
	void ApplyMutation(const EditorMutationResult& result, bool selectAffected = false);
	bool MutationSelectionAllowed(std::string& reason) const;
	bool MatchesSearch(SceneManager::EntityId entity) const;
	SceneManager::EntityId SelectedEntity() const;
	void SelectEntity(SceneManager::EntityId entity, bool toggle = false);
	bool IsSelected(SceneManager::EntityId entity) const;

	// Phase 3B2: property command helpers. The S6-B converted paths — name,
	// material index, material properties, motion add/remove, script
	// add/path/rebind/remove and discrete fields — use construct-then-Execute
	// inline at their call sites. The four S6-C live-preview sessions (light,
	// camera, motion velocity, Int/Float/Vec3/Color script fields) preview
	// each frame through the composite seam via CompositePreviewSession and
	// finalize through FinalizePreviewSession; no fabricated RecordApplied
	// outcome remains. PropertyEditSession handles deferred-close ordering and
	// defensive guards for the construct-then-Execute paths.
	void DiscardAllPropertySessions();

	// S6-C host glue. The two-phase close decision machine lives in the
	// CPU-linkable PreviewSessionClose core (FinalizePreviewSession /
	// RestorePreviewSession / BuildPreviewSessionCommand); the ImGui layer
	// below is thin glue that routes outcomes through ApplyMutation and keeps
	// owning-widget ownership alive while a session remains open:
	//   - the owning widget ID is cleared ONLY after the session actually
	//     closed (recorded/compensated/discarded) — a failed close keeps it,
	//     so the stranded session stays retryable and finalizable (P1 finding 2);
	//   - a close that failed against a live target surfaces a pending-recovery
	//     path that retries or discards while preserving target/origin/owner.
	// Close a live-preview session (finalize=true) or compensate it
	// (finalize=false) via the shared core, then update owning-widget /
	// pending-recovery state from the outcome. Clearing the owning widget ID
	// is conditional on the session actually closing.
	PreviewSessionCloseOutcome ClosePreviewSession(
		PreviewSessionKind kind, CompositePreviewSession& session,
		unsigned int& owningWidgetId, bool finalize);
	// Finalize or restore an open live-preview session BEFORE a discrete
	// command (camera Align, script path/rebind/remove, motion add/remove,
	// discrete script fields) acts on the same component. Returns true when no
	// open session remains (proceed); false when the close stayed pending and
	// the discrete action must be aborted (P1 finding 3).
	bool ClosePreviewBeforeDiscrete(PreviewSessionKind kind,
		CompositePreviewSession& session, unsigned int& owningWidgetId);
	// Finish every currently open live-preview session when leaving
	// editability. Editability is never discard proof (P1 finding 4).
	void FinalizeOpenPreviewSessions();
	// Close every open live-preview session (abandon/restore) BEFORE a
	// document-preserving global action (Undo/Redo) through the two-phase close
	// policy. Returns false (so the requested action is aborted) when any open
	// session stays pending, leaving recovery surfaced and never orphaning an
	// applied preview (S6-C re-review, P1 finding 2).
	bool CloseAllPreviewSessionsForGlobalAction();
	// Shared implementation used by both Undo/Redo abandonment (finalize=false)
	// and discrete/global authoring admission (finalize=true): close every open
	// preview session through the reducer and return whether all closed.
	bool CloseAllPreviewSessionsForAction(bool finalize);
	// Shared command-admission seam (S6-C final-verdict P1 finding 2): close
	// every open live-preview session (finalize/record) BEFORE any discrete or
	// global authoring/history mutation so no command can record ahead of an
	// open preview (chronological history, legal schema transitions). Returns
	// true only when everything closed; on false the requested mutation must be
	// ABORTED with the pending session's recovery owner/error unchanged.
	bool AdmitAuthoringMutation();
	// Submit a discrete/global authoring command through the shared admission
	// seam and route its result through ApplyMutation. Returns the (possibly
	// rejected) mutation result.
	EditorMutationResult SubmitAuthoringCommand(
		std::unique_ptr<IEditorCommand> cmd, bool selectAffected = false);
	// Render the pending-recovery banner (a closed-but-failed session awaiting
	// retry or discard).
	void RenderPreviewRecoveryBar();
	unsigned int& OwningWidgetIdFor(PreviewSessionKind kind);
	// Route a close outcome through the host sync path when the close mutated
	// the scene.
	void ApplyCloseOutcome(const PreviewSessionCloseOutcome& outcome);
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
	// Punctual light creation. `type` picks Point/Spot/Directional. A spot's
	// aim and a directional's direction both live in the entity's rotation,
	// not the light component, so `direction` is converted to a rotation on
	// the way in and is meaningless for a point light.
	void CreateLightCommand(const char* name, LightType type,
	                        const glm::vec3& position, const glm::vec3& direction,
	                        const glm::vec3& color, float intensity);
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
	std::function<std::filesystem::path()> m_ScriptDialogInitialDirectory;
	std::function<void()> m_OnDumpGPUTransforms;
	std::function<void()> m_OnDumpNEEBuffers;
	std::function<void(const rt2::core::UUID&)> m_OnViewThroughCamera;
	std::function<void(const rt2::core::UUID&)> m_OnAlignCameraToView;
	std::string m_ScriptRebindDiagnostic;

	// UI state for the "Add" popup
	bool m_ShowAddPopup = false;

	// Per-panel visibility flags (driven by the app's View menu). The
	// viewport is not toggleable and is not owned by SceneEditorUI.
	bool m_ShowOutliner = true;
	bool m_ShowInspector = true;

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
	//
	// S6-C: the four non-transform live-preview editors (light, camera,
	// motion velocity, Int/Float/Vec3/Color script fields) use
	// CompositePreviewSession instead — per-frame edits go through the
	// composite seam (immutable origin, rolling committed source, real
	// results) and finalize through FinalizePreviewSession / escape via
	// RestorePreviewSession. The remaining discrete construct-then-Execute
	// sessions continue to use PropertyEditSession.
	using NameSession = PropertyEditSession<std::string>;
	using LightSession = CompositePreviewSession;
	using CameraSession = CompositePreviewSession;
	using MaterialIndexSession = PropertyEditSession<int>;
	using MaterialPropertiesSession = PropertyEditSession<SceneMaterial>;
	using MotionSession = CompositePreviewSession;
	using ScriptSession = CompositePreviewSession;

	EditorCommandHistory* m_CommandHistory = nullptr;
	rt2::core::ScriptFieldRegistry* m_FieldRegistry = nullptr;

	TransformPreviewSession      m_TransformPreviewSession;
	NameSession                  m_NameSession;
	LightSession                 m_LightSession;
	CameraSession                m_CameraSession;
	MaterialIndexSession         m_MaterialIndexSession;
	MaterialPropertiesSession    m_MaterialPropertiesSession;
	MotionSession                m_MotionVelocitySession;
	ScriptSession                m_ScriptFieldSession;
	// ImGui widget IDs that opened each multi-widget session (so the
	// deactivation close only fires for the owning widget). ImGui IDs are
	// stored as unsigned int to avoid depending on ImGui headers here.
	unsigned int m_TransformSessionOwningWidgetId = 0;
	std::optional<TransformGestureToken> m_TransformGestureToken;
	unsigned int m_LightSessionOwningWidgetId = 0;
	unsigned int m_CameraSessionOwningWidgetId = 0;
	unsigned int m_MaterialPropertiesSessionOwningWidgetId = 0;
	unsigned int m_MotionVelocitySessionOwningWidgetId = 0;
	unsigned int m_ScriptFieldSessionOwningWidgetId = 0;

	// Pending-recovery surfacing (S6-C fixup P1 finding 2 / final closure P1
	// finding 2): a live-preview session whose close failed against a still-live
	// target. The owning widget ID above is NOT cleared while a session is
	// pending, so a retry through the persistent recovery bar keeps
	// target/origin/owner intact. State is kept PER KIND and derived from all
	// open pending sessions, so closing a different-kind session can never hide
	// a still-open pending one.
	struct PreviewRecoveryState
	{
		bool pending = false;
		PreviewSessionKind kind = PreviewSessionKind::Light;
		bool finalize = true; // close mode to retry (true = finalize, false = restore)
		std::string detail;
	};
	PreviewRecoveryState m_PreviewRecoveryByKind[5] = {};
	bool m_TransformRecoveryPending = false;
	bool m_TransformRecoveryFinalize = true;
	std::string m_TransformRecoveryDetail;
	PreviewRecoveryState& PendingRecoveryFor(PreviewSessionKind kind);
	void SetPendingRecovery(PreviewSessionKind kind, bool finalize,
		const std::string& detail);
	void ClearPendingRecovery(PreviewSessionKind kind);
	bool AnyPreviewRecoveryPending() const;
	// True when at least one of the four live-preview sessions is open (any
	// kind). Used with AdmitAuthoringMutation to enforce at most one open S6-C
	// preview: before a new Begin on a different kind/target, all existing
	// sessions are finalized in physical gesture order (S6-C closure-ordering
	// P1 finding 2).
	bool AnyPreviewSessionOpen() const;
	// Shared Begin-admission predicate (host-edge P1 finding 2): whether a new
	// live-preview gesture may begin on `target` for `kind`. Returns false when
	// recovery is pending (no implicit retry), when that kind's session already
	// owns `target` (the ongoing gesture), or when shared admission cannot
	// finalize an other-open (different-kind or same-kind-different-target)
	// session. On true the caller may Begin; the prior gesture is already
	// finalized in physical order.
	bool CanBeginPreview(PreviewSessionKind kind, const rt2::core::UUID& target);
	// The first kind with a pending (open, unresolved) session, or nullptr.
	const PreviewRecoveryState* FirstPendingRecovery() const;
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
	// Defaults off: glTF's metallicFactor default is 1.0, and importing
	// spec-correctly is the honest baseline. Opting in is a per-asset choice.
	bool        m_PendingImportAssumeDielectric = false;
};

#endif // SCENE_EDITOR_UI_H
