#include "SceneEditorUI.h"
#include "PrimitiveGeometry.h"
#include "FileDialog.h"
#include "ECSComponents.h"
#include "ECSScene.h"
#include "EditorCommands.h"
#include "EditorStructuralCommands.h"
#include "EditorPropertyCommands.h"
#include "SceneHierarchy.h"
#include "ScriptAssetPath.h"
#include "ScriptFieldRegistry.h"
#include "ScriptFieldValue.h"
#include "ContentBrowserOperations.h"
#include "RTLog.h"
#include "imgui.h"
#include <cstdio>
#include <algorithm>
#include <cctype>
#include <cstring>

void SceneEditorUI::RenderPanels()
{
	if (m_ShowOutliner) RenderOutliner();
	if (m_ShowInspector) RenderInspector();
	RenderPreviewRecoveryBar();
	DrawImportOptionsModal();
}

void SceneEditorUI::RenderPreviewRecoveryBar()
{
	if (!m_PreviewRecovery.active) return;
	CompositePreviewSession* session = nullptr;
	unsigned int* owningWidgetId = nullptr;
	switch (m_PreviewRecovery.kind)
	{
	case PreviewSessionKind::Light:
		session = &m_LightSession; owningWidgetId = &m_LightSessionOwningWidgetId; break;
	case PreviewSessionKind::Camera:
		session = &m_CameraSession; owningWidgetId = &m_CameraSessionOwningWidgetId; break;
	case PreviewSessionKind::Motion:
		session = &m_MotionVelocitySession; owningWidgetId = &m_MotionVelocitySessionOwningWidgetId; break;
	case PreviewSessionKind::Script:
		session = &m_ScriptFieldSession; owningWidgetId = &m_ScriptFieldSessionOwningWidgetId; break;
	}
	if (!session || !session->IsOpen())
	{
		// The stranded session was closed elsewhere (document replacement,
		// target removal, direct discard): clear the stale banner.
		m_PreviewRecovery.active = false;
		return;
	}
	ImGui::Separator();
	const std::string detail = m_PreviewRecovery.detail.empty()
		? "recovery required" : m_PreviewRecovery.detail;
	ImGui::TextWrapped("[Preview] The live edit on %s could not be finalized: %s",
		session->Target().ToString().c_str(), detail.c_str());
	ImGui::TextDisabled("The edit is still applied to the document without a history entry.");
	ImGui::TextDisabled("Resolve the failure (or remove the target / load a new document) and retry.");
	if (ImGui::Button(m_PreviewRecovery.finalize ? "Retry Finalize" : "Retry Restore"))
		ClosePreviewSession(m_PreviewRecovery.kind, *session, *owningWidgetId,
			m_PreviewRecovery.finalize);
	ImGui::Separator();
}

void SceneEditorUI::ImportAssetPathFromDrop(const std::string& path)
{
    rt2::core::ContentBrowserDropCallbacks callbacks;
    if (m_OnImportWithOptions)
    {
        callbacks.importObj = [this](const std::string& droppedPath,
                                      const ImportSettings& settings) {
            (void)m_OnImportWithOptions(droppedPath, settings);
        };
    }
    if (m_OnImportGltf)
    {
        callbacks.importGltf = [this](const std::string& droppedPath) {
            (void)m_OnImportGltf(droppedPath);
        };
    }

    rt2::core::Error error;
    if (!rt2::core::DispatchContentBrowserAssetDrop(path, callbacks, error))
        RT_LOG("Content Browser asset drop rejected: %s", error.Format().c_str());
}

void SceneEditorUI::NotifySceneChanged()
{
	if (m_OnSceneChanged)
		m_OnSceneChanged();
}

void SceneEditorUI::NotifyTransformChanged()
{
	if (m_OnTransformChanged)
		m_OnTransformChanged();
}

void SceneEditorUI::ApplyMutation(const EditorMutationResult& result, bool selectAffected)
{
	if (!result.success)
	{
		m_MutationError = result.error.Format();
		return;
	}
	m_MutationError.clear();
	if (result.recoveryWarning)
		m_MutationError = "Warning: " + result.recoveryWarning->Format();
	if (selectAffected && !result.affectedEntities.empty())
	{
		m_State.Selection().Clear();
		for (const auto& uuid : result.affectedEntities)
			m_State.Selection().Add(uuid);
	}
	if (m_OnMutation)
	{
		m_OnMutation(result.syncImpact);
		return;
	}
	switch (result.syncImpact)
	{
		case rt2::core::SyncImpact::Transform: NotifyTransformChanged(); break;
		case rt2::core::SyncImpact::Material:
		case rt2::core::SyncImpact::Structural: NotifySceneChanged(); break;
		case rt2::core::SyncImpact::None: break;
	}
}

bool SceneEditorUI::MutationSelectionAllowed(std::string& reason) const
{
	if (!m_Editable)
	{
		reason = "Authoring is read-only while Play mode is active.";
		return false;
	}
	if (m_State.AnyDirectlyLocked(m_State.Selection().Ordered()))
	{
		reason = "The selection contains a directly locked entity.";
		return false;
	}
	return true;
}

void SceneEditorUI::Undo()
{
	if (!m_CommandHistory || !m_SceneMgr) return;
	// Two-phase-close every open live-preview session (abandon) before history
	// moves. If any stays pending, abort the Undo so an applied preview is
	// never orphaned — the persistent recovery bar stays surfaced (S6-C
	// re-review, P1 finding 2).
	if (!CloseAllPreviewSessionsForGlobalAction()) return;
	// Abandon the non-composite record-on-release sessions (uncommitted widget
	// edits) once the previews are safe to leave.
	m_TransformSession.Discard();
	m_NameSession.Discard();
	m_MaterialIndexSession.Discard();
	m_MaterialPropertiesSession.Discard();
	const auto result = m_CommandHistory->Undo(*m_SceneMgr);
	ApplyMutation(result);
}

void SceneEditorUI::Redo()
{
	if (!m_CommandHistory || !m_SceneMgr) return;
	if (!CloseAllPreviewSessionsForGlobalAction()) return;
	m_TransformSession.Discard();
	m_NameSession.Discard();
	m_MaterialIndexSession.Discard();
	m_MaterialPropertiesSession.Discard();
	const auto result = m_CommandHistory->Redo(*m_SceneMgr);
	ApplyMutation(result);
}

bool SceneEditorUI::CloseAllPreviewSessionsForGlobalAction()
{
	const auto closeOne = [&](PreviewSessionKind kind, CompositePreviewSession& session,
		unsigned int& owner) -> bool {
		if (!session.IsOpen()) return true;
		return ClosePreviewSession(kind, session, owner, /*finalize=*/false).result
			== PreviewSessionCloseOutcome::Result::Closed;
	};
	return closeOne(PreviewSessionKind::Light, m_LightSession, m_LightSessionOwningWidgetId)
		&& closeOne(PreviewSessionKind::Camera, m_CameraSession, m_CameraSessionOwningWidgetId)
		&& closeOne(PreviewSessionKind::Motion, m_MotionVelocitySession, m_MotionVelocitySessionOwningWidgetId)
		&& closeOne(PreviewSessionKind::Script, m_ScriptFieldSession, m_ScriptFieldSessionOwningWidgetId);
}

void SceneEditorUI::SetEditable(bool editable)
{
	// Leaving editability (Play entry): finalize/restore any open live-preview
	// session BEFORE authoring is locked so no previewed value/marker/schema is
	// orphaned in the authoring document without a history entry. Editability
	// is never discard proof (S6-C fixup, P1 finding 4); a failure surfaces as
	// pending recovery rather than silently discarding.
	if (!editable && m_Editable)
		FinalizeOpenPreviewSessions();
	m_Editable = editable;
}

unsigned int& SceneEditorUI::OwningWidgetIdFor(PreviewSessionKind kind)
{
	switch (kind)
	{
	case PreviewSessionKind::Light: return m_LightSessionOwningWidgetId;
	case PreviewSessionKind::Camera: return m_CameraSessionOwningWidgetId;
	case PreviewSessionKind::Motion: return m_MotionVelocitySessionOwningWidgetId;
	case PreviewSessionKind::Script: return m_ScriptFieldSessionOwningWidgetId;
	}
	return m_LightSessionOwningWidgetId;
}

void SceneEditorUI::ApplyCloseOutcome(const PreviewSessionCloseOutcome& outcome)
{
	if (!outcome.needsSyncApply) return;
	ApplyMutation(outcome.mutation);
}

PreviewSessionCloseOutcome SceneEditorUI::ClosePreviewSession(
	PreviewSessionKind kind, CompositePreviewSession& session,
	unsigned int& owningWidgetId, bool finalize)
{
	PreviewSessionCloseOutcome outcome;
	if (!m_SceneMgr || !session.IsOpen()) return outcome;
	if (finalize)
	{
		if (!m_CommandHistory)
		{
			// No history installed: the gesture cannot be recorded. Keep the
			// session open and surface recovery rather than silently
			// discarding the applied preview state.
			outcome.result = PreviewSessionCloseOutcome::Result::PendingRetry;
			outcome.lastError = EditorMutationResult::Failure(
				rt2::core::Error::InvalidRuntimeState, session.Target().ToString(),
				"command history is not installed; the preview edit cannot be recorded");
		}
		else
		{
			outcome = ::FinalizePreviewSession(*m_CommandHistory, *m_SceneMgr,
				kind, session);
		}
	}
	else
	{
		outcome = ::RestorePreviewSession(*m_SceneMgr, kind, session);
	}
	// Clear ownership ONLY after the session actually closed. A pending close
	// keeps the owning widget ID so the stranded session stays retryable and
	// finalizable (S6-C fixup, P1 finding 2) and surfaces recovery.
	if (outcome.result == PreviewSessionCloseOutcome::Result::Closed)
	{
		owningWidgetId = 0;
		m_PreviewRecovery.active = false;
	}
	else
	{
		m_PreviewRecovery.active = true;
		m_PreviewRecovery.kind = kind;
		m_PreviewRecovery.finalize = finalize;
		m_PreviewRecovery.detail = outcome.lastError.error.detail;
	}
	ApplyCloseOutcome(outcome);
	return outcome;
}

bool SceneEditorUI::ClosePreviewBeforeDiscrete(PreviewSessionKind kind,
	CompositePreviewSession& session, unsigned int& owningWidgetId)
{
	if (!session.IsOpen()) return true;
	const auto outcome = ClosePreviewSession(kind, session, owningWidgetId,
		/*finalize=*/true);
	// Abort the discrete action when the live-preview close could not be
	// completed (S6-C fixup, P1 finding 3): a discrete command issued against
	// a still-stranded session would make its compensation stale.
	return outcome.result == PreviewSessionCloseOutcome::Result::Closed;
}

void SceneEditorUI::FinalizeOpenPreviewSessions()
{
	if (!m_SceneMgr) return;
	ClosePreviewSession(PreviewSessionKind::Light, m_LightSession,
		m_LightSessionOwningWidgetId, /*finalize=*/true);
	ClosePreviewSession(PreviewSessionKind::Camera, m_CameraSession,
		m_CameraSessionOwningWidgetId, /*finalize=*/true);
	ClosePreviewSession(PreviewSessionKind::Motion, m_MotionVelocitySession,
		m_MotionVelocitySessionOwningWidgetId, /*finalize=*/true);
	ClosePreviewSession(PreviewSessionKind::Script, m_ScriptFieldSession,
		m_ScriptFieldSessionOwningWidgetId, /*finalize=*/true);
}

void SceneEditorUI::DiscardAllPropertySessions()
{
	m_TransformSession.Discard();
	m_NameSession.Discard();
	m_LightSession.Discard();
	m_CameraSession.Discard();
	m_MaterialIndexSession.Discard();
	m_MaterialPropertiesSession.Discard();
	m_MotionVelocitySession.Discard();
	m_ScriptFieldSession.Discard();
}

SetMaterialPropertiesCommand::OverrideList
SceneEditorUI::CaptureMaterialOverrideListForSlot(int slotIndex) const
{
	if (!m_SceneMgr) return {};
	return CaptureMaterialOverrideFanOut(m_SceneMgr->GetECS().registry, slotIndex);
}

void SceneEditorUI::HideShowSelectionCommand(bool hide)
{
	if (!m_SceneMgr || !m_CommandHistory) return;
	const auto ordered = m_State.Selection().Ordered();
	if (ordered.empty()) return;

	// Build per-entity before/after states. The "after" state for every
	// selected entity is `hide`. For the mixed-state path to be reachable,
	// each entity's prior state is read live.
	std::vector<std::pair<rt2::core::UUID, bool>> beforeStates;
	std::vector<std::pair<rt2::core::UUID, bool>> afterStates;
	beforeStates.reserve(ordered.size());
	afterStates.reserve(ordered.size());
	for (const auto& uuid : ordered)
	{
		const auto entity = m_SceneMgr->FindEntityByUuid(uuid);
		if (entity == entt::null) continue;
		const auto* vc = m_SceneMgr->GetECS().registry.try_get<VisibleComponent>(entity);
		const bool current = vc ? vc->visible : true;
		beforeStates.emplace_back(uuid, current);
		afterStates.emplace_back(uuid, hide);
	}

	auto cmd = MakeSetVisibilityCommandIfEffective(beforeStates, afterStates);
	if (!cmd) return;
	// Execute through history so it is recorded and routed.
	auto result = ExecuteCommandThroughHistory(m_CommandHistory, *m_SceneMgr, std::move(cmd));
	ApplyMutation(result);
}

// ============================================================================
// Phase 3B1 structural command helpers
// ============================================================================

void SceneEditorUI::CreateEmptyCommand(const std::optional<rt2::core::UUID>& parent)
{
	if (!m_SceneMgr || !m_CommandHistory) return;
	const auto uuid = m_SceneMgr->ReserveKnownUuid();
	auto applied = m_SceneMgr->CreateEmptyWithUuid(uuid, "Empty", parent);
	if (!applied.success)
	{
		ApplyMutation(applied);
		return;
	}
	auto snapshot = m_SceneMgr->CaptureSubtreeSnapshot({ uuid });
	if (snapshot.entities.empty())
	{
		// Rollback the creation.
		m_SceneMgr->RemoveSubtreesNoCompact({ uuid });
		return;
	}
	auto cmd = MakeCreateEmptyCommand(std::move(snapshot), uuid);
	if (!cmd) return;
	m_CommandHistory->RecordApplied(std::move(cmd), *m_SceneMgr, applied);
	ApplyMutation(applied, true);
}

void SceneEditorUI::CreatePrimitiveCommand(PrimitiveComponent::Kind kind, float size,
                                          const char* name, const glm::vec3& position)
{
	if (!m_SceneMgr || !m_CommandHistory) return;
	SceneMaterial mat;
	if (kind == PrimitiveComponent::Sphere && std::string(name) == "Light")
	{
		mat.emissiveColor = {1.0f, 1.0f, 1.0f};
		mat.emissiveIntensity = 10.0f;
	}
	const int matIdx = m_SceneMgr->AddMaterial(mat);
	const auto uuid = m_SceneMgr->ReserveKnownUuid();
	EditableTRS trs;
	trs.translation = position;
	auto applied = m_SceneMgr->CreatePrimitiveEntity(uuid, name, kind, size, trs, matIdx);
	if (!applied.success)
	{
		ApplyMutation(applied);
		return;
	}
	auto snapshot = m_SceneMgr->CaptureSubtreeSnapshot({ uuid });
	if (snapshot.entities.empty())
	{
		m_SceneMgr->RemoveSubtreesNoCompact({ uuid });
		return;
	}
	auto cmd = MakeCreatePrimitiveCommand(std::move(snapshot), uuid);
	if (!cmd) return;
	m_CommandHistory->RecordApplied(std::move(cmd), *m_SceneMgr, applied);
	ApplyMutation(applied, true);
}

void SceneEditorUI::CreateLightCommand(const char* name, LightType type,
                                      const glm::vec3& position, const glm::vec3& direction,
                                      const glm::vec3& color, float intensity)
{
	if (!m_SceneMgr || !m_CommandHistory) return;
	const auto uuid = m_SceneMgr->ReserveKnownUuid();
	EditableTRS trs;
	trs.translation = position;
	trs.rotation = LightDirectionToRotation(direction);
	auto applied = m_SceneMgr->CreateLightEntity(uuid, name, trs, color, intensity, type);
	if (!applied.success)
	{
		ApplyMutation(applied);
		return;
	}
	auto snapshot = m_SceneMgr->CaptureSubtreeSnapshot({ uuid });
	if (snapshot.entities.empty())
	{
		m_SceneMgr->RemoveSubtreesNoCompact({ uuid });
		return;
	}
	auto cmd = MakeCreateLightCommand(std::move(snapshot), uuid);
	if (!cmd) return;
	m_CommandHistory->RecordApplied(std::move(cmd), *m_SceneMgr, applied);
	ApplyMutation(applied, true);
}

void SceneEditorUI::DeleteSelectionCommand()
{
	if (!m_SceneMgr || !m_CommandHistory) return;
	const auto ordered = m_State.Selection().Ordered();
	if (ordered.empty()) return;
	auto snapshot = m_SceneMgr->CaptureSubtreeSnapshot(ordered);
	auto cmd = MakeRemoveSubtreesCommand(std::move(snapshot), ordered);
	if (!cmd) return;
	auto result = ExecuteCommandThroughHistory(m_CommandHistory, *m_SceneMgr, std::move(cmd));
	ApplyMutation(result);
	m_State.Selection().Clear();
	m_TreeDirty = true;
}

void SceneEditorUI::DuplicateSelectionCommand()
{
	if (!m_SceneMgr || !m_CommandHistory) return;
	const auto ordered = m_State.Selection().Ordered();
	if (ordered.empty()) return;
	auto countResult = m_SceneMgr->CountCanonicalSubtreeEntities(ordered);
	if (!countResult.IsOk()) return;
	auto knownUuids = m_SceneMgr->ReserveKnownUuids(countResult.value);
	auto dup = m_SceneMgr->DuplicateSubtreesWithUuids(ordered, knownUuids);
	if (!dup.mutation.success)
	{
		ApplyMutation(dup.mutation);
		return;
	}
	auto snapshot = m_SceneMgr->CaptureSubtreeSnapshot(dup.createdRoots);
	auto cmd = MakeDuplicateSubtreesCommand(std::move(snapshot), dup.createdRoots);
	if (!cmd) return;
	m_CommandHistory->RecordApplied(std::move(cmd), *m_SceneMgr, dup.mutation);
	ApplyMutation(dup.mutation, true);
}

void SceneEditorUI::PasteCommand(const std::optional<rt2::core::UUID>& parent)
{
	if (!m_SceneMgr || !m_CommandHistory) return;
	// All clipboard-generation validation lives in EditorSceneState. The UI
	// never duplicates generation checks: a stale clipboard is surfaced as a
	// mutation error BEFORE any snapshot, command, or history mutation.
	auto paste = m_State.PasteWithUuidsForCommand(*m_SceneMgr, parent);
	if (!paste.mutation.success)
	{
		ApplyMutation(paste.mutation);
		return;
	}
	auto snapshot = m_SceneMgr->CaptureSubtreeSnapshot(paste.createdRoots);
	auto cmd = MakePasteSubtreesCommand(std::move(snapshot), paste.createdRoots);
	if (!cmd) return;
	m_CommandHistory->RecordApplied(std::move(cmd), *m_SceneMgr, paste.mutation);
	ApplyMutation(paste.mutation, true);
}

void SceneEditorUI::ReparentCommand(const std::vector<rt2::core::UUID>& sources,
                                    const rt2::core::UUID& newParent)
{
	if (!m_SceneMgr || !m_CommandHistory) return;
	if (sources.empty()) return;

	// Capture before-edits: each source's current parent, local TRS, and
	// sibling anchor. The after-edits carry the source's current WORLD
	// matrix so ReparentBatch with PreserveWorld can derive the new local
	// TRS that preserves the world pose. Undo uses PreserveLocal with the
	// stored before-local TRS (set by the command, not here).
	std::vector<ReparentEdit> beforeEdits;
	std::vector<ReparentEdit> afterEdits;
	for (const auto& src : sources)
	{
		const auto entity = m_SceneMgr->FindEntityByUuid(src);
		if (entity == entt::null) continue;
		EditableTRS local;
		if (!m_SceneMgr->GetLocalTransform(SceneManager::EntityId{ entity }, local)) continue;
		EditableTRS world;
		if (!m_SceneMgr->GetWorldTransform(SceneManager::EntityId{ entity }, world)) continue;
		rt2::core::UUID parentUuid;
		const auto parent = m_SceneMgr->GetParent(SceneManager::EntityId{ entity });
		if (parent.IsValid())
			parentUuid = m_SceneMgr->GetEntityUuid(parent);
		auto snap = m_SceneMgr->CaptureSubtreeSnapshot({ src });
		RootSiblingAnchor anchor = snap.rootAnchors.empty() ? RootSiblingAnchor{} : snap.rootAnchors.front();
		beforeEdits.push_back({ src, parentUuid, local, world.Matrix(), anchor });
		afterEdits.push_back({ src, newParent, local, world.Matrix(), {} });
	}

	// Execute with PreserveWorld to preserve the world pose (matching
	// Phase 2C's drag-reparent semantics). Undo uses PreserveLocal with
	// the stored before-local TRS (handled by ReparentCommand::Undo).
	auto cmd = MakeReparentCommandIfEffective(beforeEdits, afterEdits, ReparentMode::PreserveWorld);
	if (!cmd) return;
	auto result = ExecuteCommandThroughHistory(m_CommandHistory, *m_SceneMgr, std::move(cmd));
	ApplyMutation(result);
}

void SceneEditorUI::SingleEntityHideShowCommand(const rt2::core::UUID& entity, bool hide)
{
	if (!m_SceneMgr || !m_CommandHistory) return;
	const auto e = m_SceneMgr->FindEntityByUuid(entity);
	if (e == entt::null) return;
	const auto* vc = m_SceneMgr->GetECS().registry.try_get<VisibleComponent>(e);
	const bool current = vc ? vc->visible : true;
	std::vector<std::pair<rt2::core::UUID, bool>> beforeStates = { {entity, current} };
	std::vector<std::pair<rt2::core::UUID, bool>> afterStates = { {entity, hide} };
	auto cmd = MakeSetVisibilityCommandIfEffective(beforeStates, afterStates);
	if (!cmd) return;
	auto result = ExecuteCommandThroughHistory(m_CommandHistory, *m_SceneMgr, std::move(cmd));
	ApplyMutation(result);
}

bool SceneEditorUI::MatchesSearch(SceneManager::EntityId entity) const
{
	const std::string query = m_State.SearchText();
	if (query.empty()) return true;
	std::string name = m_SceneMgr->GetEntityName(entity);
	std::string lowerQuery = query;
	std::transform(name.begin(), name.end(), name.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (name.find(lowerQuery) != std::string::npos) return true;
	for (const auto child : m_SceneMgr->GetChildren(entity))
		if (MatchesSearch(child)) return true;
	return false;
}

SceneManager::EntityId SceneEditorUI::SelectedEntity() const
{
	if (!m_SceneMgr)
		return {};
	const auto uuid = m_State.Selection().Primary();
	return uuid.IsNull() ? SceneManager::EntityId{} :
		SceneManager::EntityId{ m_SceneMgr->FindEntityByUuid(uuid) };
}

void SceneEditorUI::SelectEntity(SceneManager::EntityId entity, bool toggle)
{
	if (!m_SceneMgr || !entity.IsValid())
	{
		if (!toggle) m_State.Selection().Clear();
		return;
	}
	m_State.Prune(m_SceneMgr->AuthoringDoc());
	const auto uuid = m_SceneMgr->GetEntityUuid(entity);
	if (toggle) m_State.Selection().Toggle(uuid);
	else m_State.Selection().SelectOnly(uuid);
}

bool SceneEditorUI::IsSelected(SceneManager::EntityId entity) const
{
	return m_SceneMgr && entity.IsValid() &&
		m_State.Selection().Contains(m_SceneMgr->GetEntityUuid(entity));
}

// ============================================================================
// Outliner
// ============================================================================

void SceneEditorUI::RenderOutliner()
{
	ImGui::Begin("Outliner");

	if (!m_SceneMgr)
	{
		ImGui::Text("No scene manager");
		ImGui::End();
		return;
	}

	// Add menu â€” disabled during Play
	ImGui::BeginDisabled(!m_Editable);
	if (ImGui::Button("Add"))
		ImGui::OpenPopup("AddEntity");

	if (ImGui::BeginPopup("AddEntity"))
	{
		if (ImGui::MenuItem("Empty"))
			CreateEmptyCommand(std::nullopt);
		const auto selectedParent = m_State.Selection().Primary();
		ImGui::BeginDisabled(selectedParent.IsNull() || m_State.IsLocked(selectedParent));
		if (ImGui::MenuItem("Child Empty"))
			CreateEmptyCommand(selectedParent);
		ImGui::EndDisabled();
		ImGui::Separator();
		if (ImGui::BeginMenu("Light"))
		{
			// Punctual defaults. Point/spot intensity is candela and goes
			// through inverse-square falloff, so 50 cd at a few metres reads
			// as a normal room light. A directional light has no falloff at
			// all — its intensity is the arriving radiance — which is why it
			// needs a far smaller number to sit at the same exposure.
			if (ImGui::MenuItem("Point"))
			{
				CreateLightCommand("Point Light", LightType::Point,
				                   {0, 3, 0}, {0, 0, -1}, {1, 1, 1}, 50.0f);
			}
			if (ImGui::MenuItem("Spot"))
			{
				CreateLightCommand("Spot Light", LightType::Spot,
				                   {0, 3, 0}, {0, -1, 0}, {1, 1, 1}, 50.0f);
			}
			if (ImGui::MenuItem("Directional"))
			{
				CreateLightCommand("Directional Light", LightType::Directional,
				                   {0, 5, 0}, {0, -1, 0}, {1, 1, 1}, 3.0f);
			}
			ImGui::Separator();
			// Not a punctual light: an emissive sphere, which is real geometry
			// sampled by NEE as a triangle light. Kept because it is the only
			// light with visible area, and so the only one that casts soft
			// shadows.
			if (ImGui::MenuItem("Emissive Sphere"))
			{
				CreatePrimitiveCommand(PrimitiveComponent::Sphere, 0.4f, "Light", {0, 3, 0});
			}
			ImGui::EndMenu();
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Cube"))
		{
			CreatePrimitiveCommand(PrimitiveComponent::Cube, 1.0f, "Cube", {0, 0.5f, 0});
		}
		if (ImGui::MenuItem("Sphere"))
		{
			CreatePrimitiveCommand(PrimitiveComponent::Sphere, 1.0f, "Sphere", {0, 0.5f, 0});
		}
		if (ImGui::MenuItem("Plane"))
		{
			CreatePrimitiveCommand(PrimitiveComponent::Plane, 5.0f, "Plane", {0, 0, 0});
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Import Scene..."))
		{
			const auto initialDirectory = m_DialogInitialDirectory
				? m_DialogInitialDirectory() : std::filesystem::path{};
			std::string path = FileDialog::OpenFile(
				L"Model Files (*.glb;*.gltf;*.obj)\0*.glb;*.gltf;*.obj\0glTF Binary (*.glb)\0*.glb\0glTF JSON (*.gltf)\0*.gltf\0OBJ Files (*.obj)\0*.obj\0All Files (*.*)\0*.*\0",
				initialDirectory);
			if (!path.empty() && m_OnImportWithOptions)
			{
				m_PendingImportPath = path;
				m_PendingImportMergeMegaMesh = true;
				m_ImportOptionsOpen = true;
			}
		}
		if (ImGui::MenuItem("Load Mesh File..."))
		{
			const auto initialDirectory = m_DialogInitialDirectory
				? m_DialogInitialDirectory() : std::filesystem::path{};
			std::string path = FileDialog::OpenFile(
				L"Model Files (*.glb;*.gltf;*.obj)\0*.glb;*.gltf;*.obj\0OBJ Files (*.obj)\0*.obj\0glTF Binary (*.glb)\0*.glb\0glTF JSON (*.gltf)\0*.gltf\0All Files (*.*)\0*.*\0",
				initialDirectory);
			if (!path.empty() && m_OnImportWithOptions)
			{
				m_PendingImportPath = path;
				m_PendingImportMergeMegaMesh = true;
				m_ImportOptionsOpen = true;
			}
		}
		ImGui::EndPopup();
	}
	ImGui::EndDisabled();

	ImGui::Separator();
	if (ImGui::InputTextWithHint("##OutlinerSearch", "Search entities...",
		m_SearchBuffer, sizeof(m_SearchBuffer)))
		m_State.SearchText() = m_SearchBuffer;

	// Entity tree (root entities â†’ children)
	size_t count = m_SceneMgr->GetEntityCount();
	ImGui::Text("Entities: %d", (int)count);

	if (count == 0)
	{
		ImGui::TextDisabled("  (empty â€” load a scene or add an entity)");
		RenderAssetDropTarget();
		ImGui::End();
		return;
	}

	ImGui::Separator();

	auto roots = m_SceneMgr->GetRootEntities();
	m_TreeDirty = false;
	for (auto root : roots)
	{
		RenderEntityTree(root, 0);
		if (m_TreeDirty)
			break;
	}

	ImGui::Separator();

	ImGui::BeginDisabled(!m_Editable);
	if (ImGui::Button("Delete Selected"))
	{
		std::string reason;
		if (MutationSelectionAllowed(reason))
			DeleteSelectionCommand();
		else m_MutationError = reason;
	}
	ImGui::EndDisabled();

	ImGui::SameLine();
	if (ImGui::Button("Copy"))
	{
		rt2::core::Error error;
		if (!m_State.Copy(*m_SceneMgr, m_State.Selection().Ordered(), error))
			m_MutationError = error.Format();
		else m_MutationError.clear();
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(!m_Editable || !m_State.HasClipboard());
	if (ImGui::Button("Paste"))
		PasteCommand(std::nullopt);
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(!m_Editable || m_State.Selection().Empty());
	if (ImGui::Button("Duplicate"))
		DuplicateSelectionCommand();
	ImGui::EndDisabled();

	ImGui::SameLine();
	if (ImGui::Button("Dump GPU Transforms"))
	{
		if (m_OnDumpGPUTransforms)
			m_OnDumpGPUTransforms();
	}

	ImGui::SameLine();
	if (ImGui::Button("Dump NEE Buffers"))
	{
		if (m_OnDumpNEEBuffers)
			m_OnDumpNEEBuffers();
	}

	if (!m_MutationError.empty())
		ImGui::TextWrapped("%s", m_MutationError.c_str());
	RenderAssetDropTarget();

	const auto& io = ImGui::GetIO();
	if (m_Editable && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
		!io.WantTextInput)
	{
		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C))
		{
			rt2::core::Error error;
			if (!m_State.Copy(*m_SceneMgr, m_State.Selection().Ordered(), error))
				m_MutationError = error.Format();
		}
		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V))
			PasteCommand(std::nullopt);
		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D))
			DuplicateSelectionCommand();
		if (ImGui::IsKeyPressed(ImGuiKey_Delete))
		{
			std::string reason;
			if (MutationSelectionAllowed(reason))
				DeleteSelectionCommand();
			else m_MutationError = reason;
		}
	}

	ImGui::End();
}

void SceneEditorUI::RenderAssetDropTarget()
{
	ImGui::Separator();
	ImGui::TextDisabled("Drop a .glb, .gltf or .obj from Content Browser to import");
	if (!ImGui::BeginDragDropTarget())
		return;
	if (const ImGuiPayload* payload =
			ImGui::AcceptDragDropPayload("RT2_ASSET_PATH"))
	{
		if (payload->Data && payload->DataSize > 1)
		{
			const char* text = static_cast<const char*>(payload->Data);
			const size_t length = static_cast<size_t>(payload->DataSize - 1);
			ImportAssetPathFromDrop(std::string(text, length));
		}
	}
	ImGui::EndDragDropTarget();
}

void SceneEditorUI::RenderEntityTree(SceneManager::EntityId entity, int depth)
{
	if (!entity.IsValid()) return;
	if (!MatchesSearch(entity)) return;
	const auto uuid = m_SceneMgr->GetEntityUuid(entity);
	const bool directlyLocked = m_State.IsLocked(uuid);
	const auto* visibleComponent = m_SceneMgr->GetECS().registry.try_get<VisibleComponent>(entity.id);
	const bool directlyVisible = !visibleComponent || visibleComponent->visible;

	std::string name = m_SceneMgr->GetEntityName(entity);
	if (name.empty())
		name = "Entity " + std::to_string((uint32_t)entity.id);

	std::string label;
	if (m_SceneMgr->HasCamera(entity))
		label = "[Camera] " + name;
	else if (m_SceneMgr->HasMeshRef(entity))
		label = "[Mesh] " + name;
	else
		label = "[Entity] " + name;
	if (!directlyVisible) label = "[Hidden] " + label;
	if (directlyLocked) label = "[Locked] " + label;

	ImGui::PushID((int)entity.id);

	bool isSelected = IsSelected(entity);
	bool hasChildren = m_SceneMgr->HasChildren(entity);
	auto handleDragDrop = [&]()
	{
		if (!m_Editable) return;
		if (ImGui::BeginDragDropSource())
		{
			ImGui::SetDragDropPayload("RT2_ENTITY_UUID", uuid.bytes.data(), uuid.bytes.size());
			ImGui::Text("Move %s", name.c_str());
			ImGui::EndDragDropSource();
		}
		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("RT2_ENTITY_UUID"))
			{
				std::array<uint8_t, 16> bytes{};
				if (payload->DataSize == static_cast<int>(bytes.size()))
				{
					std::memcpy(bytes.data(), payload->Data, bytes.size());
					const rt2::core::UUID dragged(bytes);
					auto sources = m_State.Selection().Contains(dragged)
						? m_State.Selection().Ordered()
						: std::vector<rt2::core::UUID>{ dragged };
				if (directlyLocked || m_State.AnyDirectlyLocked(sources))
					m_MutationError = "Locked entities cannot be reparented or receive children.";
				else
					ReparentCommand(sources, uuid);
				}
			}
			ImGui::EndDragDropTarget();
		}
	};

	if (hasChildren)
	{
		bool open = ImGui::TreeNodeEx(label.c_str(),
			ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
			ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen |
			(isSelected ? ImGuiTreeNodeFlags_Selected : 0));
		handleDragDrop();

		// Click on the tree node label selects it
		if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
			SelectEntity(entity, ImGui::GetIO().KeyCtrl);

		// Right-click context menu for deletion
		if (ImGui::BeginPopupContextItem("EntityCtx"))
		{
			ImGui::BeginDisabled(!m_Editable);
			if (ImGui::MenuItem(directlyLocked ? "Unlock" : "Lock"))
				m_State.ToggleLocked(uuid);
			ImGui::BeginDisabled(directlyLocked);
			if (ImGui::MenuItem(directlyVisible ? "Hide" : "Show"))
				SingleEntityHideShowCommand(uuid, !directlyVisible);
			if (ImGui::MenuItem("Create Child"))
				CreateEmptyCommand(uuid);
			if (m_SceneMgr->GetParent(entity).IsValid() && ImGui::MenuItem("Move to Scene Root"))
				ReparentCommand({ uuid }, rt2::core::UUID::Nil());
			if (ImGui::MenuItem("Paste as Child", nullptr, false, m_State.HasClipboard()))
				PasteCommand(uuid);
			ImGui::EndDisabled();
			ImGui::Separator();
			// Phase 3A: selection-level Hide/Show, recorded through the
			// command history so a single Undo restores the mixed-state mix.
			ImGui::BeginDisabled(m_State.Selection().Empty());
			if (ImGui::MenuItem("Hide Selection"))
				HideShowSelectionCommand(true);
			if (ImGui::MenuItem("Show Selection"))
				HideShowSelectionCommand(false);
			ImGui::EndDisabled();
			ImGui::Separator();
			if (ImGui::MenuItem("Copy"))
			{
				rt2::core::Error error;
				if (!m_State.Copy(*m_SceneMgr, { uuid }, error)) m_MutationError = error.Format();
			}
			if (ImGui::MenuItem("Duplicate"))
			{
				m_State.Selection().SelectOnly(uuid);
				DuplicateSelectionCommand();
			}
			ImGui::BeginDisabled(directlyLocked);
			if (ImGui::MenuItem("Delete"))
			{
				m_State.Selection().SelectOnly(uuid);
				DeleteSelectionCommand();
				ImGui::EndDisabled();
				ImGui::EndDisabled();
				ImGui::EndPopup();
				if (open) ImGui::TreePop();
				ImGui::PopID();
				return;
			}
			ImGui::EndDisabled();
			ImGui::EndDisabled();
			ImGui::EndPopup();
		}

		if (open)
		{
			auto children = m_SceneMgr->GetChildren(entity);
			for (auto child : children)
			{
				RenderEntityTree(child, depth + 1);
				if (m_TreeDirty)
					break;
			}
			ImGui::TreePop();
		}
	}
	else
	{
		if (ImGui::Selectable(label.c_str(), isSelected, ImGuiTreeNodeFlags_SpanAvailWidth))
			SelectEntity(entity, ImGui::GetIO().KeyCtrl);
		handleDragDrop();

		if (ImGui::BeginPopupContextItem("EntityCtx"))
		{
			ImGui::BeginDisabled(!m_Editable);
			if (ImGui::MenuItem(directlyLocked ? "Unlock" : "Lock"))
				m_State.ToggleLocked(uuid);
			ImGui::BeginDisabled(directlyLocked);
			if (ImGui::MenuItem(directlyVisible ? "Hide" : "Show"))
				SingleEntityHideShowCommand(uuid, !directlyVisible);
			if (ImGui::MenuItem("Create Child"))
				CreateEmptyCommand(uuid);
			if (m_SceneMgr->GetParent(entity).IsValid() && ImGui::MenuItem("Move to Scene Root"))
				ReparentCommand({ uuid }, rt2::core::UUID::Nil());
			if (ImGui::MenuItem("Paste as Child", nullptr, false, m_State.HasClipboard()))
				PasteCommand(uuid);
			ImGui::EndDisabled();
			ImGui::Separator();
			ImGui::BeginDisabled(m_State.Selection().Empty());
			if (ImGui::MenuItem("Hide Selection"))
				HideShowSelectionCommand(true);
			if (ImGui::MenuItem("Show Selection"))
				HideShowSelectionCommand(false);
			ImGui::EndDisabled();
			ImGui::Separator();
			if (ImGui::MenuItem("Copy"))
			{
				rt2::core::Error error;
				if (!m_State.Copy(*m_SceneMgr, { uuid }, error)) m_MutationError = error.Format();
			}
			if (ImGui::MenuItem("Duplicate"))
			{
				m_State.Selection().SelectOnly(uuid);
				DuplicateSelectionCommand();
			}
			ImGui::BeginDisabled(directlyLocked);
			if (ImGui::MenuItem("Delete"))
			{
				m_State.Selection().SelectOnly(uuid);
				DeleteSelectionCommand();
			}
			ImGui::EndDisabled();
			ImGui::EndDisabled();
			ImGui::EndPopup();
		}
	}

	ImGui::PopID();
}

// ============================================================================
// Inspector
// ============================================================================

void SceneEditorUI::RenderInspector()
{
	ImGui::Begin("Inspector");

	if (m_SceneMgr)
		m_State.Selection().Prune(m_SceneMgr->AuthoringDoc());
	const auto selectedEntity = SelectedEntity();
	if (!m_SceneMgr || !selectedEntity.IsValid() || !m_SceneMgr->IsEntityAlive(selectedEntity))
	{
		ImGui::TextDisabled("Select an entity in the Outliner");
		ImGui::End();
		return;
	}

	auto entity = selectedEntity;
	std::string name = m_SceneMgr->GetEntityName(entity);
	const auto targetUuid = m_SceneMgr->GetEntityUuid(entity);
	const bool directlyLocked = m_State.IsLocked(m_SceneMgr->GetEntityUuid(entity));
	if (directlyLocked)
		ImGui::TextDisabled("Directly locked in the editor");
	ImGui::BeginDisabled(directlyLocked);

	// Name field — records on Enter or focus loss after edit (not per keystroke).
	char nameBuf[128];
	snprintf(nameBuf, sizeof(nameBuf), "%s", name.c_str());
	ImGui::Text("Name:");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(200.0f);
	ImGui::BeginDisabled(!m_Editable);
	const bool nameReturned = ImGui::InputText("##EntityName", nameBuf,
		sizeof(nameBuf), ImGuiInputTextFlags_EnterReturnsTrue);
	const bool nameDeactivatedAfterEdit = ImGui::IsItemDeactivatedAfterEdit();
	if (nameReturned || nameDeactivatedAfterEdit)
	{
		if (std::string(nameBuf) != name)
		{
			auto cmd = MakeSetNameCommandIfEffective(targetUuid, name, std::string(nameBuf));
			if (cmd)
			{
				const auto result = ExecuteCommandThroughHistory(m_CommandHistory, *m_SceneMgr, std::move(cmd));
				ApplyMutation(result);
			}
		}
	}
	ImGui::EndDisabled();
	ImGui::Separator();

	RenderTransformEditor(entity);
	ImGui::Separator();

	if (m_SceneMgr->HasMeshRef(entity))
		RenderMaterialEditor(entity);

	if (m_SceneMgr->HasLight(entity))
		RenderLightEditor(entity);

	if (m_SceneMgr->HasCamera(entity))
		RenderCameraEditor(entity);

	// Motion component editor (vertical-slice test behavior)
	{
		auto& reg = const_cast<entt::registry&>(m_SceneMgr->GetECS().registry);
		if (reg.valid(entity.id))
		{
			ImGui::Separator();
			bool hasMotion = reg.all_of<MotionComponent>(entity.id);
			if (hasMotion)
			{
				ImGui::Text("Motion");
				auto& mc = reg.get<MotionComponent>(entity.id);
				ImGui::BeginDisabled(!m_Editable);
				glm::vec3 vel = mc.linearVelocity;
				bool velChanged = false;
				if (ImGui::DragFloat3("Linear Velocity", &vel[0], 0.1f))
					velChanged = true;
				if (ImGui::IsItemActivated() && !m_MotionVelocitySession.IsOpen() && m_Editable)
				{
					if (m_MotionVelocitySession.Begin(*m_SceneMgr, targetUuid,
						PrefabValueKind::MotionState,
						PrefabComponentKeyFor<MotionComponent>::value,
						PrefabValuePayload{ std::optional<MotionComponent>(mc) },
						[this](const rt2::core::UUID& uuid,
							const PrefabValuePayload& raw) -> PrefabValuePayload {
							const auto entity = m_SceneMgr->FindEntityByUuid(uuid);
							if (entity == entt::null) return raw;
							const auto* mm = m_SceneMgr->GetECS().registry
								.try_get<MotionComponent>(entity);
							if (!mm) return raw;
							return PrefabValuePayload{
								std::optional<MotionComponent>(*mm) };
						}))
					{
						m_MotionVelocitySessionOwningWidgetId =
							ImGui::GetID("Linear Velocity");
					}
				}
				bool motionPendingClose = false;
				bool motionCancelPending = false;
				if (ImGui::IsItemDeactivatedAfterEdit() && m_MotionVelocitySession.IsOpen())
					motionPendingClose = true;
				else if (ImGui::IsItemDeactivated() && m_MotionVelocitySession.IsOpen())
					motionCancelPending = true;
				ImGui::EndDisabled();

				// S6-C live preview through the composite — no direct mutation
				// of `mc`; the working copy carries the edited velocity.
				if (velChanged && m_MotionVelocitySession.IsOpen())
				{
					MotionComponent target = mc;
					target.linearVelocity = vel;
					ApplyMutation(m_MotionVelocitySession.Preview(*m_SceneMgr,
						PrefabValuePayload{ std::optional<MotionComponent>(target) }));
				}

				// Deferred close AFTER the mutation block: commit (record one
				// command) or restore (Escape / returned-to-start, no history).
				// The owning widget ID is cleared only after the session
				// actually closed; a pending close keeps it and surfaces
				// recovery (S6-C fixup, P1 finding 2).
				if (motionPendingClose)
				{
					ClosePreviewSession(PreviewSessionKind::Motion,
						m_MotionVelocitySession, m_MotionVelocitySessionOwningWidgetId,
						/*finalize=*/true);
				}
				else if (motionCancelPending)
				{
					ClosePreviewSession(PreviewSessionKind::Motion,
						m_MotionVelocitySession, m_MotionVelocitySessionOwningWidgetId,
						/*finalize=*/false);
				}

				ImGui::SameLine();
				ImGui::BeginDisabled(!m_Editable);
				if (ImGui::Button("Remove Motion"))
				{
					// A discrete command on the same component must come only
					// after the live-preview session finishes; abort otherwise
					// (S6-C fixup, P1 finding 3).
					if (ClosePreviewBeforeDiscrete(PreviewSessionKind::Motion,
						m_MotionVelocitySession, m_MotionVelocitySessionOwningWidgetId))
					{
						MotionComponent before = mc;
						auto cmd = MakeSetMotionCommandIfEffective(targetUuid, before, std::nullopt);
						if (cmd)
						{
							const auto result = ExecuteCommandThroughHistory(m_CommandHistory, *m_SceneMgr, std::move(cmd));
							ApplyMutation(result);
						}
					}
				}
				ImGui::EndDisabled();
			}
			else
			{
				ImGui::BeginDisabled(!m_Editable);
				if (ImGui::Button("Add Motion"))
				{
					if (ClosePreviewBeforeDiscrete(PreviewSessionKind::Motion,
						m_MotionVelocitySession, m_MotionVelocitySessionOwningWidgetId))
					{
						MotionComponent after{};
						auto cmd = MakeSetMotionCommandIfEffective(targetUuid, std::nullopt, after);
						if (cmd)
						{
							const auto result = ExecuteCommandThroughHistory(m_CommandHistory, *m_SceneMgr, std::move(cmd));
							ApplyMutation(result);
						}
					}
				}
				ImGui::EndDisabled();
			}
		}
	}

	// Script component editor (Phase 6B/W5)
	if (m_SceneMgr->HasScript(entity))
		RenderScriptEditor(entity);
	else
	{
		ImGui::Separator();
		ImGui::BeginDisabled(!m_Editable);
		if (ImGui::Button("Add Script"))
		{
			// A discrete add on the same component must come only after the
			// live-preview session finishes (S6-C fixup, P1 finding 3).
			if (ClosePreviewBeforeDiscrete(PreviewSessionKind::Script,
				m_ScriptFieldSession, m_ScriptFieldSessionOwningWidgetId))
			{
				ScriptComponent unbound;
				unbound.asset.kind = AssetKind::Script;
				auto cmd = MakeSetScriptCommandIfEffective(targetUuid, std::nullopt, unbound);
				if (cmd)
				{
					const auto result = ExecuteCommandThroughHistory(m_CommandHistory, *m_SceneMgr, std::move(cmd));
					ApplyMutation(result);
				}
			}
		}
		ImGui::EndDisabled();
	}

	ImGui::EndDisabled();
	ImGui::End();
}

void SceneEditorUI::RenderTransformEditor(SceneManager::EntityId entity)
{
	EditableTRS transform;
	const bool hasTransform = m_TransformSpace == TransformSpace::Local
		? m_SceneMgr->GetLocalTransform(entity, transform)
		: m_SceneMgr->GetWorldTransform(entity, transform);
	if (!hasTransform)
	{
		ImGui::TextDisabled("Transform cannot be represented as affine TRS");
		return;
	}

	const auto targetUuid = m_SceneMgr->GetEntityUuid(entity);

	glm::vec3 pos = transform.translation;
	glm::vec3 rot = glm::degrees(glm::eulerAngles(transform.rotation));
	glm::vec3 scale = transform.scale;

	bool changed = false;
	ImGui::PushID("Transform");
	ImGui::Text("Transform");
	int space = m_TransformSpace == TransformSpace::Local ? 0 : 1;
	ImGui::SetNextItemWidth(100.0f);
	if (ImGui::Combo("Space", &space, "Local\0World\0"))
	{
		m_TransformSpace = space == 0 ? TransformSpace::Local : TransformSpace::World;
		m_TransformEditError.clear();
	}
	int pivot = static_cast<int>(m_TransformPivot);
	ImGui::SetNextItemWidth(100.0f);
	if (ImGui::Combo("Pivot", &pivot, "Primary\0Median\0Individual\0"))
		m_TransformPivot = static_cast<TransformPivot>(pivot);
	ImGui::Checkbox("Snap", &m_TransformSnap.enabled);
	if (m_TransformSnap.enabled)
	{
		ImGui::SetNextItemWidth(80.0f);
		ImGui::DragFloat("Move step", &m_TransformSnap.translation, 0.05f, 0.001f, 1000.0f, "%.3f");
		ImGui::SetNextItemWidth(80.0f);
		ImGui::DragFloat("Rotate step", &m_TransformSnap.rotationDegrees, 1.0f, 0.1f, 180.0f, "%.1f deg");
		ImGui::SetNextItemWidth(80.0f);
		ImGui::DragFloat("Scale step", &m_TransformSnap.scale, 0.01f, 0.001f, 100.0f, "%.3f");
	}
	ImGui::Checkbox("Uniform Scale", &m_UniformScale);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip(
			"When checked, scale edits apply equally to X, Y, Z.\n"
			"Also affects the viewport gizmo in Scale mode.");
	ImGui::BeginDisabled(!m_Editable);

	// Phase 3B2: record-on-release via PropertyEditSession<EditableTRS>.
	// Per-widget IsItemActivated/IsItemDeactivatedAfterEdit checks
	// immediately after EACH of the three DragFloat3s (ImGui's last-item
	// rule). The first activation while no session is open captures
	// beforeLocal BEFORE any mutation that frame and opens the session,
	// owned by that widget's ImGui ID (tracked locally). The owning
	// widget's IsItemDeactivatedAfterEdit closes the session and records
	// via RecordApplied. IsItemDeactivated without AfterEdit (Escape
	// cancel — ImGui reverts the value itself) discards the session and
	// records nothing.
	//
	// The close is deferred until AFTER the mutation block below: a
	// keyboard-committed edit (Ctrl+click, type, Enter) fires both
	// changed==true and IsItemDeactivatedAfterEdit() on the same frame,
	// and the after-state must be read AFTER SetLocalTransform/
	// TrySetWorldTransform has applied the committed value. Mouse-drag
	// releases carry no value change on the release frame, so deferring
	// is safe for them too.
	EditableTRS beforeLocalCapture;
	bool hasLocalForCapture = m_SceneMgr->GetLocalTransform(entity, beforeLocalCapture);
	unsigned int owningWidgetId = 0;
	if (m_TransformSession.IsOpen())
		owningWidgetId = m_TransformSessionOwningWidgetId;
	unsigned int pendingCloseWidgetId = 0;

	ImGui::SetNextItemWidth(180.0f);
	if (ImGui::DragFloat3("Position", &pos[0], 0.1f))
	{
		if (m_TransformSnap.enabled) pos = SnapValues(pos, m_TransformSnap.translation);
		changed = true;
	}
	const unsigned int posWidgetId = ImGui::GetID("Position");
	if (ImGui::IsItemActivated())
	{
		if (!m_TransformSession.IsOpen() && hasLocalForCapture && m_Editable)
		{
			m_TransformSession.OnActivated(targetUuid, beforeLocalCapture);
			m_TransformSessionOwningWidgetId = posWidgetId;
			owningWidgetId = posWidgetId;
		}
	}
	if (changed && m_TransformSession.IsOpen())
		m_TransformSession.OnEditCommitted();
	if (ImGui::IsItemDeactivatedAfterEdit())
	{
		if (m_TransformSession.IsOpen() && owningWidgetId == posWidgetId)
			pendingCloseWidgetId = posWidgetId;
	}
	else if (ImGui::IsItemDeactivated() && m_TransformSession.IsOpen() &&
	         owningWidgetId == posWidgetId)
	{
		m_TransformSession.OnCancelled();
	}

	ImGui::SetNextItemWidth(180.0f);
	const bool rotChangedThis = ImGui::DragFloat3("Rotation", &rot[0], 1.0f);
	if (rotChangedThis)
	{
		if (m_TransformSnap.enabled) rot = SnapValues(rot, m_TransformSnap.rotationDegrees);
		changed = true;
	}
	const unsigned int rotWidgetId = ImGui::GetID("Rotation");
	if (ImGui::IsItemActivated())
	{
		if (!m_TransformSession.IsOpen() && hasLocalForCapture && m_Editable)
		{
			m_TransformSession.OnActivated(targetUuid, beforeLocalCapture);
			m_TransformSessionOwningWidgetId = rotWidgetId;
			owningWidgetId = rotWidgetId;
		}
	}
	if (rotChangedThis && m_TransformSession.IsOpen())
		m_TransformSession.OnEditCommitted();
	if (ImGui::IsItemDeactivatedAfterEdit())
	{
		if (m_TransformSession.IsOpen() && owningWidgetId == rotWidgetId)
			pendingCloseWidgetId = rotWidgetId;
	}
	else if (ImGui::IsItemDeactivated() && m_TransformSession.IsOpen() &&
	         owningWidgetId == rotWidgetId)
	{
		m_TransformSession.OnCancelled();
	}

	ImGui::SetNextItemWidth(180.0f);
	bool scaleChangedThis = false;
	if (m_UniformScale)
	{
		float uniformScale = scale.x;
		scaleChangedThis = ImGui::DragFloat("Scale", &uniformScale, 0.05f);
		if (scaleChangedThis)
		{
			if (m_TransformSnap.enabled) uniformScale = SnapValue(uniformScale, m_TransformSnap.scale);
			scale = glm::vec3(uniformScale);
			changed = true;
		}
	}
	else
	{
		scaleChangedThis = ImGui::DragFloat3("Scale", &scale[0], 0.05f);
		if (scaleChangedThis)
		{
			if (m_TransformSnap.enabled) scale = SnapValues(scale, m_TransformSnap.scale);
			changed = true;
		}
	}
	const unsigned int scaleWidgetId = ImGui::GetID("Scale");
	if (ImGui::IsItemActivated())
	{
		if (!m_TransformSession.IsOpen() && hasLocalForCapture && m_Editable)
		{
			m_TransformSession.OnActivated(targetUuid, beforeLocalCapture);
			m_TransformSessionOwningWidgetId = scaleWidgetId;
			owningWidgetId = scaleWidgetId;
		}
	}
	if (scaleChangedThis && m_TransformSession.IsOpen())
		m_TransformSession.OnEditCommitted();
	if (ImGui::IsItemDeactivatedAfterEdit())
	{
		if (m_TransformSession.IsOpen() && owningWidgetId == scaleWidgetId)
			pendingCloseWidgetId = scaleWidgetId;
	}
	else if (ImGui::IsItemDeactivated() && m_TransformSession.IsOpen() &&
	         owningWidgetId == scaleWidgetId)
	{
		m_TransformSession.OnCancelled();
	}

	ImGui::EndDisabled();

	if (changed)
	{
		EditableTRS edited;
		edited.translation = pos;
		edited.rotation = glm::quat(glm::radians(rot));
		edited.scale = scale;
		const bool applied = m_TransformSpace == TransformSpace::Local
			? (m_SceneMgr->SetLocalTransform(entity, edited), true)
			: m_SceneMgr->TrySetWorldTransform(entity, edited.Matrix());
		if (applied)
		{
			m_TransformEditError.clear();
			NotifyTransformChanged();
		}
		else
		{
			m_TransformEditError =
				"World edit rejected: parent is singular or the result contains shear.";
		}
	}

	// Close the session AFTER the mutation block so the recorded after-state
	// reflects the committed value (keyboard-committed edits fire both
	// changed==true and IsItemDeactivatedAfterEdit() on the same frame).
	if (pendingCloseWidgetId != 0 && m_TransformSession.IsOpen() &&
	    m_TransformSessionOwningWidgetId == pendingCloseWidgetId)
	{
		EditableTRS afterLocal;
		if (m_SceneMgr->GetLocalTransform(entity, afterLocal))
		{
			auto rec = m_TransformSession.CloseDeferred(afterLocal,
				{ [this, targetUuid]() {
					return m_SceneMgr->FindEntityByUuid(targetUuid) != entt::null && m_Editable;
				} });
			if (rec && m_CommandHistory)
			{
				auto cmd = MakeTransformCommandIfEffective(targetUuid,
					rec->before, rec->after);
				if (cmd)
				{
					EditorMutationResult appliedResult;
					appliedResult.success = true;
					appliedResult.syncImpact = rt2::core::SyncImpact::Transform;
					appliedResult.affectedEntities.push_back(targetUuid);
					m_CommandHistory->RecordApplied(std::move(cmd), *m_SceneMgr, appliedResult);
				}
			}
		}
	}
	if (!m_TransformEditError.empty())
		ImGui::TextWrapped("%s", m_TransformEditError.c_str());
	ImGui::PopID();
}

void SceneEditorUI::RenderMaterialEditor(SceneManager::EntityId entity)
{
	ImGui::Text("Material");

	uint32_t meshIdx;
	int matIdx;
	if (!m_SceneMgr->GetMeshRef(entity, meshIdx, matIdx))
		return;

	ImGui::Text("Mesh Index: %u", meshIdx);
	const auto targetUuid = m_SceneMgr->GetEntityUuid(entity);

	ImGui::BeginDisabled(!m_Editable);
	// Material index combo — record-on-release via a discrete commit
	// (Combo returns true only on selection change, so one record per
	// selection is natural).
	const auto& materials = m_SceneMgr->GetMaterials();
	int current = matIdx;
	ImGui::SetNextItemWidth(180.0f);
	bool indexChanged = false;
	if (ImGui::Combo("Material", &current, [](void* data, int idx, const char** out_text) -> bool {
		auto* mats = static_cast<const std::vector<SceneMaterial>*>(data);
		if (idx < 0 || idx >= (int)mats->size()) return false;
		static char buf[64];
		snprintf(buf, sizeof(buf), "Material %d", idx);
		*out_text = buf;
		return true;
	}, (void*)&materials, (int)materials.size()))
	{
		indexChanged = true;
	}

	ImGui::SameLine();
	if (ImGui::Button("Duplicate"))
	{
		// Clone current material so this entity gets its own independent copy.
		// AddMaterial creates a new slot outside the command (orphaned until
		// history-clear compaction — consistent with the 3B1 leak-until-clear
		// policy). The SetMaterialIndexCommand records the index change.
		if (current >= 0 && current < (int)materials.size())
		{
			SceneMaterial copy = m_SceneMgr->GetMaterial(current);
			int newIdx = m_SceneMgr->AddMaterial(copy);
			current = newIdx;
			indexChanged = true;
		}
	}

	if (indexChanged)
	{
		const int beforeIndex = matIdx;
		// Read the displaced override live (before) and stage the canonical
		// after-override (no mutation), then let the command's composite
		// replay do the index write + marker insertion + override swap
		// atomically (construct-then-Execute; the 2026-08-03 material-index
		// undo defect shape — before can no longer be inverted with after).
		auto beforeOverride = m_SceneMgr->GetMaterialOverride(targetUuid);
		const auto staged = m_SceneMgr->StageMaterialIndex(targetUuid, current);
		if (!staged.IsOk())
		{
			ApplyMutation(ToEditorMutationResult(staged.error));
			return;
		}
		auto cmd = MakeSetMaterialIndexCommandIfEffective(targetUuid, beforeIndex, current,
			beforeOverride, staged.value.override);
		if (cmd)
		{
			const auto result = ExecuteCommandThroughHistory(m_CommandHistory, *m_SceneMgr, std::move(cmd));
			ApplyMutation(result);
		}
	}

	// Inline material editor (edits the material that this entity references).
	// Edits go through SetMaterialProperties so dirty tracking, the correct
	// GPU sync path, and durable MaterialOverrideComponent recording on
	// imported entities all fire. Record-on-release via the state machine
	// (PropertyEditSession<SceneMaterial>) with per-widget activation
	// tracking (ImGui's last-item rule requires checking IsItemActivated/
	// IsItemDeactivatedAfterEdit immediately after each widget).
	if (current >= 0 && current < (int)materials.size())
	{
		SceneMaterial mat = m_SceneMgr->GetMaterial(current);
		bool matChanged = false;

		unsigned int owningWidgetId = m_MaterialPropertiesSession.IsOpen()
			? m_MaterialPropertiesSessionOwningWidgetId : 0;
		unsigned int pendingCloseWidgetId = 0;
		SceneMaterial beforeMatCapture = mat;

		auto drawMaterialWidget = [&](const char* label, auto drawFn) -> unsigned int {
			ImGui::Indent();
			bool w = drawFn();
			ImGui::Unindent();
			if (w) matChanged = true;
			const unsigned int widgetId = ImGui::GetID(label);
			if (ImGui::IsItemActivated())
			{
				if (!m_MaterialPropertiesSession.IsOpen() && m_Editable)
				{
					m_MaterialPropertiesSession.OnActivated(targetUuid, beforeMatCapture);
					m_PendingMaterialPropertiesBeforeOverrides = CaptureMaterialOverrideListForSlot(current);
					m_MaterialPropertiesSessionOwningWidgetId = widgetId;
					owningWidgetId = widgetId;
				}
			}
			if (w && m_MaterialPropertiesSession.IsOpen())
				m_MaterialPropertiesSession.OnEditCommitted();
			if (ImGui::IsItemDeactivatedAfterEdit())
			{
				// `mat` holds this widget's live value on every frame it is
				// drawn, so on the deactivation frame it already carries the
				// final committed value passed to CloseDeferred below.
				if (m_MaterialPropertiesSession.IsOpen() && owningWidgetId == widgetId)
					pendingCloseWidgetId = widgetId;
			}
			else if (ImGui::IsItemDeactivated() && m_MaterialPropertiesSession.IsOpen() &&
			         owningWidgetId == widgetId)
			{
				m_MaterialPropertiesSession.OnCancelled();
			}
			return widgetId;
		};

		drawMaterialWidget("Base Color", [&]() {
			return ImGui::ColorEdit3("Base Color", &mat.baseColor[0]);
		});
		drawMaterialWidget("Metallic", [&]() {
			return ImGui::DragFloat("Metallic", &mat.metallic, 0.01f, 0.0f, 1.0f, "%.2f");
		});
		drawMaterialWidget("Roughness", [&]() {
			return ImGui::DragFloat("Roughness", &mat.roughness, 0.01f, 0.0f, 1.0f, "%.2f");
		});
		drawMaterialWidget("IOR", [&]() {
			return ImGui::DragFloat("IOR", &mat.ior, 0.01f, 1.0f, 3.0f, "%.2f");
		});
		drawMaterialWidget("Emissive", [&]() {
			return ImGui::ColorEdit3("Emissive", &mat.emissiveColor[0]);
		});
		drawMaterialWidget("Emissive Intensity", [&]() {
			return ImGui::DragFloat("Emissive Intensity", &mat.emissiveIntensity, 0.1f, 0.0f, 100.0f, "%.1f");
		});

		if (pendingCloseWidgetId != 0 && m_MaterialPropertiesSession.IsOpen() &&
		    m_MaterialPropertiesSessionOwningWidgetId == pendingCloseWidgetId)
		{
			// Construct-then-Execute: no mutation has happened yet — the
			// command's composite replay applies the material value write +
			// imported-member override fan-out + marker insertion atomically.
			auto rec = m_MaterialPropertiesSession.CloseDeferred(mat,
				{ [this, targetUuid, current]() {
					if (m_SceneMgr->FindEntityByUuid(targetUuid) == entt::null || !m_Editable)
						return false;
					return current >= 0 && current < (int)m_SceneMgr->GetMaterials().size();
				} });
			if (rec)
			{
				const auto staged = m_SceneMgr->StageMaterialSlot(current, rec->after);
				if (!staged.IsOk())
				{
					ApplyMutation(ToEditorMutationResult(staged.error));
				}
				else
				{
					auto cmd = MakeSetMaterialPropertiesCommandIfEffective(current,
						rec->before, rec->after,
						m_PendingMaterialPropertiesBeforeOverrides,
						staged.value.afterOverrides);
					if (cmd)
					{
						const auto result = ExecuteCommandThroughHistory(m_CommandHistory, *m_SceneMgr, std::move(cmd));
						ApplyMutation(result);
					}
				}
			}
			m_PendingMaterialPropertiesBeforeOverrides.clear();
		}
	}
	ImGui::EndDisabled();
}

void SceneEditorUI::RenderLightEditor(SceneManager::EntityId entity)
{
	ImGui::Text("Light");

	glm::vec3 color;
	float intensity;
	LightType lightType;
	if (!m_SceneMgr->GetLightProperties(entity, color, intensity, lightType))
		return;

	const auto targetUuid = m_SceneMgr->GetEntityUuid(entity);

	// Read the full LightComponent for the session (the Inspector only
	// exposes color/intensity/type, but the command stores the full
	// struct for forward compatibility).
	auto& reg = const_cast<entt::registry&>(m_SceneMgr->GetECS().registry);
	LightComponent beforeLight;
	if (auto* lc = reg.try_get<LightComponent>(entity.id))
		beforeLight = *lc;

	bool changed = false;
	bool cancelPending = false;
	ImGui::PushID("Light");
	ImGui::BeginDisabled(!m_Editable);

	unsigned int owningWidgetId = m_LightSession.IsOpen()
		? m_LightSessionOwningWidgetId : 0;
	unsigned int pendingCloseWidgetId = 0;

	auto drawLightWidget = [&](const char* label, auto drawFn) -> unsigned int {
		bool w = drawFn();
		if (w) changed = true;
		const unsigned int widgetId = ImGui::GetID(label);
		if (ImGui::IsItemActivated())
		{
			if (!m_LightSession.IsOpen() && m_Editable)
			{
				// Capture the immutable origin (value + override-set
				// membership + document schema) BEFORE any preview frame of
				// this gesture, and the durable-value reader for the rolling
				// source (falls back to the raw committed target if the live
				// read fails).
				if (m_LightSession.Begin(*m_SceneMgr, targetUuid,
					PrefabValueKind::LightProperties,
					PrefabComponentKeyFor<LightComponent>::value,
					beforeLight,
					[this](const rt2::core::UUID& uuid,
						const PrefabValuePayload& raw) -> PrefabValuePayload {
						const auto entity = m_SceneMgr->FindEntityByUuid(uuid);
						if (entity == entt::null) return raw;
						const auto* lc = m_SceneMgr->GetECS().registry
							.try_get<LightComponent>(entity);
						if (!lc) return raw;
						return PrefabValuePayload{ *lc };
					}))
				{
					m_LightSessionOwningWidgetId = widgetId;
					owningWidgetId = widgetId;
				}
			}
		}
		if (ImGui::IsItemDeactivatedAfterEdit())
		{
			if (m_LightSession.IsOpen() && owningWidgetId == widgetId)
				pendingCloseWidgetId = widgetId;
		}
		else if (ImGui::IsItemDeactivated() && m_LightSession.IsOpen() &&
		         owningWidgetId == widgetId)
		{
			cancelPending = true;
		}
		return widgetId;
	};

	// Range and the cone angles live on the component but were previously
	// unreachable from the Inspector; edit them through a working copy so a
	// single SetLightComponent carries every field.
	LightComponent edited = beforeLight;
	edited.color = color;
	edited.intensity = intensity;
	edited.type = lightType;

	drawLightWidget("Color", [&]() {
		return ImGui::ColorEdit3("Color", &color[0]);
	});
	drawLightWidget("Intensity", [&]() {
		return ImGui::DragFloat("Intensity", &intensity, 0.1f, 0.0f, 1000.0f, "%.1f");
	});
	drawLightWidget("Type", [&]() {
		// Order must match LightType's values (Point=0, Spot=1, Directional=2).
		const char* kTypeNames[] = { "Point", "Spot", "Directional" };
		int typeIndex = static_cast<int>(lightType);
		if (!ImGui::Combo("Type", &typeIndex, kTypeNames, IM_ARRAYSIZE(kTypeNames)))
			return false;
		lightType = static_cast<LightType>(typeIndex);
		return true;
	});
	edited.color = color;
	edited.intensity = intensity;
	edited.type = lightType;

	// Directional lights are parallel rays with no origin, so distance
	// falloff never applies and range is meaningless for them.
	if (lightType != LightType::Directional)
	{
		drawLightWidget("Range", [&]() {
			return ImGui::DragFloat("Range", &edited.range, 0.5f, 0.0f, 10000.0f,
				edited.range <= 0.0f ? "Unbounded" : "%.1f");
		});
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Distance at which the light fades to zero. 0 = unbounded.");
		edited.range = std::max(edited.range, 0.0f);
	}

	if (lightType == LightType::Spot)
	{
		drawLightWidget("InnerCone", [&]() {
			return ImGui::DragFloat("Inner Cone", &edited.innerConeAngle, 0.5f,
				0.0f, 90.0f, "%.1f deg");
		});
		drawLightWidget("OuterCone", [&]() {
			return ImGui::DragFloat("Outer Cone", &edited.outerConeAngle, 0.5f,
				0.0f, 90.0f, "%.1f deg");
		});
		// The GPU builder already clamps and orders these, so an inverted cone
		// renders as if swapped. Enforce the ordering here instead, otherwise
		// the authored value silently disagrees with what is drawn: push the
		// *other* angle so the one being dragged keeps the value typed into it.
		edited.innerConeAngle = glm::clamp(edited.innerConeAngle, 0.0f, 90.0f);
		edited.outerConeAngle = glm::clamp(edited.outerConeAngle, 0.0f, 90.0f);
		if (edited.innerConeAngle > edited.outerConeAngle)
		{
			if (owningWidgetId == ImGui::GetID("InnerCone"))
				edited.outerConeAngle = edited.innerConeAngle;
			else
				edited.innerConeAngle = edited.outerConeAngle;
		}
	}

	ImGui::EndDisabled();
	ImGui::PopID();

	if (changed && m_LightSession.IsOpen())
	{
		// S6-C live preview: stage the rolling committed source -> the edited
		// working copy and commit through the atomic composite (real result,
		// marker + schema handled there), instead of the old direct
		// SetLightComponent + fabricated RecordLightEdit.
		ApplyMutation(m_LightSession.Preview(*m_SceneMgr, PrefabValuePayload{ edited }));
	}

	// Deferred close AFTER the mutation block: commit (record one command)
	// or restore (Escape / returned-to-start, no history). The owning widget
	// ID is cleared only after the session actually closed; a pending close
	// keeps it and surfaces recovery (S6-C fixup, P1 finding 2).
	if (pendingCloseWidgetId != 0 && m_LightSession.IsOpen() &&
	    m_LightSessionOwningWidgetId == pendingCloseWidgetId)
	{
		ClosePreviewSession(PreviewSessionKind::Light, m_LightSession,
			m_LightSessionOwningWidgetId, /*finalize=*/true);
	}
	else if (cancelPending && m_LightSession.IsOpen())
	{
		ClosePreviewSession(PreviewSessionKind::Light, m_LightSession,
			m_LightSessionOwningWidgetId, /*finalize=*/false);
	}
}

void SceneEditorUI::RenderCameraEditor(SceneManager::EntityId entity)
{
	ImGui::Text("Camera");

	auto& reg = const_cast<entt::registry&>(m_SceneMgr->GetECS().registry);
	if (!reg.valid(entity.id)) return;
	auto* cam = reg.try_get<CameraComponent>(entity.id);
	if (!cam) return;

	const auto targetUuid = m_SceneMgr->GetEntityUuid(entity);
	CameraComponent beforeCamera = *cam;

	float verticalFOV = cam->verticalFOV;
	float aperture = cam->aperture;
	float focusDistance = cam->focusDistance;
	ImGui::BeginDisabled(!m_Editable);

	bool cancelPending = false;
	unsigned int owningWidgetId = m_CameraSession.IsOpen()
		? m_CameraSessionOwningWidgetId : 0;
	unsigned int pendingCloseWidgetId = 0;

	auto drawCameraWidget = [&](const char* label, auto drawFn) -> unsigned int {
		bool w = drawFn();
		const unsigned int widgetId = ImGui::GetID(label);
		if (ImGui::IsItemActivated())
		{
			if (!m_CameraSession.IsOpen() && m_Editable)
			{
				if (m_CameraSession.Begin(*m_SceneMgr, targetUuid,
					PrefabValueKind::CameraProperties,
					PrefabComponentKeyFor<CameraComponent>::value,
					beforeCamera,
					[this](const rt2::core::UUID& uuid,
						const PrefabValuePayload& raw) -> PrefabValuePayload {
						const auto entity = m_SceneMgr->FindEntityByUuid(uuid);
						if (entity == entt::null) return raw;
						const auto* cc = m_SceneMgr->GetECS().registry
							.try_get<CameraComponent>(entity);
						if (!cc) return raw;
						return PrefabValuePayload{ *cc };
					}))
				{
					m_CameraSessionOwningWidgetId = widgetId;
					owningWidgetId = widgetId;
				}
			}
		}
		if (ImGui::IsItemDeactivatedAfterEdit())
		{
			if (m_CameraSession.IsOpen() && owningWidgetId == widgetId)
				pendingCloseWidgetId = widgetId;
		}
		else if (ImGui::IsItemDeactivated() && m_CameraSession.IsOpen() &&
		         owningWidgetId == widgetId)
		{
			cancelPending = true;
		}
		return widgetId;
	};

	drawCameraWidget("FOV", [&]() {
		return ImGui::DragFloat("FOV", &verticalFOV, 1.0f, 10.0f, 170.0f, "%.1f");
	});
	drawCameraWidget("Aperture", [&]() {
		return ImGui::DragFloat("Aperture", &aperture, 0.001f, 0.0f, 5.0f, "%.3f");
	});
	drawCameraWidget("Focus Distance", [&]() {
		return ImGui::DragFloat("Focus Distance", &focusDistance, 0.1f, 0.1f, 1000.0f, "%.1f");
	});

	const bool viewPressed = ImGui::Button("View Through Camera");
	ImGui::SameLine();
	const bool alignPressed = ImGui::Button("Align Camera to View");

	ImGui::EndDisabled();

	bool changed = false;
	if (cam->verticalFOV != verticalFOV || cam->aperture != aperture ||
	    cam->focusDistance != focusDistance)
	{
		changed = true;
	}
	if (changed && m_CameraSession.IsOpen())
	{
		// S6-C live preview through the atomic composite (the working copy is
		// the full live camera with the edited FOV/aperture/focusDistance).
		CameraComponent editedCamera = *cam;
		editedCamera.verticalFOV = verticalFOV;
		editedCamera.aperture = aperture;
		editedCamera.focusDistance = focusDistance;
		ApplyMutation(m_CameraSession.Preview(*m_SceneMgr, PrefabValuePayload{ editedCamera }));
	}

	if (viewPressed && m_OnViewThroughCamera)
		m_OnViewThroughCamera(targetUuid);
	// A discrete command on the same component (Align Camera to View) must
	// come only after the live-preview session finishes; otherwise the align
	// command is recorded first, then the already-applied preview command
	// whose stored After no longer matches live state fails source validation
	// on Undo (S6-C fixup, P1 finding 3). Abort the align on a pending close.
	if (alignPressed && m_OnAlignCameraToView)
	{
		if (ClosePreviewBeforeDiscrete(PreviewSessionKind::Camera,
			m_CameraSession, m_CameraSessionOwningWidgetId))
			m_OnAlignCameraToView(targetUuid);
	}

	// Deferred close AFTER the mutation block: commit (record one command)
	// or restore (Escape / returned-to-start, no history). The owning widget
	// ID is cleared only after the session actually closed; a pending close
	// keeps it and surfaces recovery (S6-C fixup, P1 finding 2).
	if (pendingCloseWidgetId != 0 && m_CameraSession.IsOpen() &&
	    m_CameraSessionOwningWidgetId == pendingCloseWidgetId)
	{
		ClosePreviewSession(PreviewSessionKind::Camera, m_CameraSession,
			m_CameraSessionOwningWidgetId, /*finalize=*/true);
	}
	else if (cancelPending && m_CameraSession.IsOpen())
	{
		ClosePreviewSession(PreviewSessionKind::Camera, m_CameraSession,
			m_CameraSessionOwningWidgetId, /*finalize=*/false);
	}
}

// ----------------------------------------------------------------------------
// Phase 6B/W5 — Script component editor
// ----------------------------------------------------------------------------
void SceneEditorUI::RenderScriptEditor(SceneManager::EntityId entity)
{
	ImGui::Separator();
	ImGui::Text("Script");

	const auto targetUuid = m_SceneMgr->GetEntityUuid(entity);
	auto scriptState = m_SceneMgr->GetScriptState(targetUuid);
	if (!scriptState.has_value()) return;

	// Path field (InputText with EnterReturnsTrue + DeactivatedAfterEdit).
	char pathBuf[512];
	snprintf(pathBuf, sizeof(pathBuf), "%s", scriptState->asset.path.c_str());
	ImGui::Text("Path:");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(300.0f);
	ImGui::BeginDisabled(!m_Editable);
	const bool pathReturned = ImGui::InputText("##ScriptPath", pathBuf,
		sizeof(pathBuf), ImGuiInputTextFlags_EnterReturnsTrue);
	const bool pathDeactivatedAfterEdit = ImGui::IsItemDeactivatedAfterEdit();
	if (pathReturned || pathDeactivatedAfterEdit)
	{
		const std::string newPath(pathBuf);
		if (newPath != scriptState->asset.path)
		{
			// A discrete path commitment must come only after the live-preview
			// field session finishes; otherwise it runs before the
			// continuous-widget close and its source is stale (S6-C fixup,
			// P1 finding 3). Abort the path edit on a pending close.
			if (!ClosePreviewBeforeDiscrete(PreviewSessionKind::Script,
				m_ScriptFieldSession, m_ScriptFieldSessionOwningWidgetId))
				scriptState = m_SceneMgr->GetScriptState(targetUuid);
			else
			{
				auto before = *scriptState;
				auto after = *scriptState;
				after.asset.path = newPath;
				auto cmd = MakeSetScriptCommandIfEffective(targetUuid, before, after);
				if (cmd)
				{
					const auto result = ExecuteCommandThroughHistory(m_CommandHistory, *m_SceneMgr, std::move(cmd));
					ApplyMutation(result);
					scriptState = m_SceneMgr->GetScriptState(targetUuid);
				}
			}
		}
	}
	ImGui::EndDisabled();

	// W8: Rebind is a file-dialog affordance over the existing path edit. The
	// SceneManager remains the identity authority: changing path deliberately
	// adopts the selected file's sidecar ID (W8-A2), and may assign one when
	// the selected file has no sidecar.
	ImGui::SameLine();
	ImGui::BeginDisabled(!m_Editable);
	if (ImGui::Button("Browse..."))
	{
		m_ScriptRebindDiagnostic.clear();
		const auto initialDirectory = m_ScriptDialogInitialDirectory
			? m_ScriptDialogInitialDirectory() : std::filesystem::path{};
		const std::string selected = FileDialog::OpenFile(
			L"Lua Scripts (*.lua)\0*.lua\0", initialDirectory);
		if (!selected.empty())
		{
			const auto selectedPath = std::filesystem::u8path(selected);
			std::string extension = selectedPath.extension().u8string();
			std::transform(extension.begin(), extension.end(), extension.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (extension != ".lua")
			{
				m_ScriptRebindDiagnostic =
					"Rebind requires a .lua script";
			}
			else
			{
				const auto root = m_SceneMgr->AssetContext().assetRoot;
				std::filesystem::path relative = selectedPath;
				std::error_code containmentError;
				if (!root.empty())
				{
					const auto canonicalRoot = std::filesystem::weakly_canonical(
						root, containmentError);
					const auto canonicalSelected = std::filesystem::weakly_canonical(
						selectedPath, containmentError);
					const auto candidate = canonicalSelected.lexically_relative(
						canonicalRoot);
					if (containmentError || candidate.empty() ||
						candidate.is_absolute() ||
						(!candidate.empty() && *candidate.begin() == ".."))
					{
						m_ScriptRebindDiagnostic =
							"Selected script is outside the active assetRoot";
					}
					else
						relative = candidate;
				}

				if (m_ScriptRebindDiagnostic.empty())
				{
					// A discrete rebind must come only after the live-preview
					// field session finishes (S6-C fixup, P1 finding 3). On a
					// pending close the rebind is skipped and the session
					// state is re-read so the diagnostic can reflect it.
					if (!ClosePreviewBeforeDiscrete(PreviewSessionKind::Script,
						m_ScriptFieldSession, m_ScriptFieldSessionOwningWidgetId))
						m_ScriptRebindDiagnostic =
							"Rebind skipped: the active live-preview field edit could not be finalized";
					else
					{
						auto before = *scriptState;
						auto after = *scriptState;
						after.asset.path = relative.generic_u8string();
						auto cmd = MakeSetScriptCommandIfEffective(targetUuid, before, after);
						if (cmd)
						{
							const auto result = ExecuteCommandThroughHistory(m_CommandHistory, *m_SceneMgr, std::move(cmd));
							if (!result.success)
								m_ScriptRebindDiagnostic = result.error.Format();
							else
								scriptState = m_SceneMgr->GetScriptState(targetUuid);
							ApplyMutation(result);
						}
					}
				}
			}
		}
	}
	ImGui::EndDisabled();
	if (!m_ScriptRebindDiagnostic.empty())
		ImGui::TextDisabled("[Rebind] %s", m_ScriptRebindDiagnostic.c_str());

	// Remove Script button.
	ImGui::SameLine();
	ImGui::BeginDisabled(!m_Editable);
	if (ImGui::Button("Remove Script"))
	{
		// A discrete removal must come only after the live-preview field
		// session finishes; a removal while a preview is active leaves a
		// retained session with no component to restore (S6-C fixup,
		// P1 finding 3). Abort the removal on a pending close.
		if (ClosePreviewBeforeDiscrete(PreviewSessionKind::Script,
			m_ScriptFieldSession, m_ScriptFieldSessionOwningWidgetId))
		{
			auto before = *scriptState;
			auto cmd = MakeSetScriptCommandIfEffective(targetUuid, before, std::nullopt);
			if (cmd)
			{
				const auto result = ExecuteCommandThroughHistory(m_CommandHistory, *m_SceneMgr, std::move(cmd));
				ApplyMutation(result);
			}
		}
	}
	ImGui::EndDisabled();

	// Empty path → unbound component: no field widgets, no diagnostics.
	if (scriptState->asset.path.empty())
		return;

	// Query declared fields from the registry (if injected).
	if (!m_FieldRegistry) return;

	const auto& document = m_SceneMgr->AuthoringDoc();
	const auto& assetContext = m_SceneMgr->AssetContext();
	std::vector<rt2::core::AssetDiagnostic> assetDiagnostics;
	const auto resolved = rt2::core::ResolveScriptAssetPath(
		*scriptState, assetContext, targetUuid,
		m_SceneMgr->GetEntityName(entity), assetDiagnostics);
	if (!resolved.success)
	{
		const std::string detail = assetDiagnostics.empty()
			? "asset resolution failed"
			: assetDiagnostics.back().detail;
		ImGui::TextDisabled("[Warning] Script asset failed: %s",
			detail.c_str());
		return;
	}
	const auto result =
		m_FieldRegistry->GetDeclaredFields(resolved.resolvedPath);

	// Parse-failure warning banner (D10). Widgets are read-only while
	// parsed == false so the user cannot author against stale declarations.
	if (!result.parsed)
	{
		ImGui::TextDisabled("[Warning] Script parse failed: %s",
			result.diagnostic.c_str());
	}

	ImGui::BeginDisabled(!m_Editable || !result.parsed);

	unsigned int owningWidgetId = m_ScriptFieldSession.IsOpen()
		? m_ScriptFieldSessionOwningWidgetId : 0;
	unsigned int pendingCloseWidgetId = 0;
	bool cancelPending = false;

	// Before-state for a discrete construct-then-Execute edit: the current
	// document state. Continuous fields preview through the composite session
	// instead (immutable origin captured at activation); a discrete edit that
	// lands mid-gesture is therefore relative to the committed preview state.
	std::optional<ScriptComponent> sessionBefore = *scriptState;

	for (const auto& desc : result.descriptors)
	{
		ImGui::PushID(desc.name.c_str());
		ImGui::Text("%s:", desc.name.c_str());
		ImGui::SameLine();

		// Find the stored entry. Do NOT auto-insert defaults into the map —
		// only insert when the widget actually changes a field, so untouched
		// declared-but-unstored fields don't get persisted as authored data.
		auto& fieldMap = scriptState->fieldValues;
		auto it = fieldMap.find(desc.name);
		const bool hasStored = it != fieldMap.end();

		// Guard: if the stored type and declared type are incompatible (different
		// variant arms), don't touch the value — the user needs to reload/reconcile.
		// Compatible types (vec3 <-> color, same arm) are safe to read/write.
		if (hasStored && !rt2::core::ScriptFieldTypesCompatible(
		        it->second.type, desc.type))
		{
			ImGui::TextDisabled("(declaration type changed — reopen to reconcile)");
			ImGui::PopID();
			continue;
		}

		// Build a display value: the stored entry if present, or the declared
		// default. We use this for rendering without mutating the map.
		rt2::core::ScriptFieldEntry display;
		if (hasStored)
			display = it->second;
		else
			display = rt2::core::ScriptFieldEntry{ desc.type, desc.defaultValue };

		bool changed = false;
		const bool isContinuous =
			desc.type == rt2::core::ScriptFieldType::Int ||
			desc.type == rt2::core::ScriptFieldType::Float ||
			desc.type == rt2::core::ScriptFieldType::Vec3 ||
			desc.type == rt2::core::ScriptFieldType::Color;

		switch (desc.type)
		{
		case rt2::core::ScriptFieldType::Bool:
		{
			bool v = std::get<bool>(display.value);
			if (ImGui::Checkbox("##val", &v))
			{
				display.value = v;
				changed = true;
			}
			break;
		}
		case rt2::core::ScriptFieldType::Int:
		{
			int64_t v = std::get<int64_t>(display.value);
			if (ImGui::DragScalar("##val", ImGuiDataType_S64, &v, 1.0f))
			{
				display.value = v;
				changed = true;
			}
			break;
		}
		case rt2::core::ScriptFieldType::Float:
		{
			double v = std::get<double>(display.value);
			if (ImGui::DragScalar("##val", ImGuiDataType_Double, &v, 0.1f))
			{
				display.value = v;
				changed = true;
			}
			break;
		}
		case rt2::core::ScriptFieldType::String:
		{
			char buf[1024];
			snprintf(buf, sizeof(buf), "%s",
				std::get<std::string>(display.value).c_str());
			if (ImGui::InputText("##val", buf, sizeof(buf),
				ImGuiInputTextFlags_EnterReturnsTrue))
			{
				display.value = std::string(buf);
				changed = true;
			}
			if (ImGui::IsItemDeactivatedAfterEdit())
			{
				display.value = std::string(buf);
				changed = true;
			}
			break;
		}
		case rt2::core::ScriptFieldType::Uuid:
		{
			char buf[64];
			snprintf(buf, sizeof(buf), "%s",
				std::get<rt2::core::UUID>(display.value).ToString().c_str());
			const auto tryCommit = [&]() {
				const auto parsed = rt2::core::UUID::Parse(buf);
				if (!parsed.IsNull() ||
				    std::string(buf) == "00000000-0000-0000-0000-000000000000")
				{
					display.value = parsed;
					changed = true;
				}
			};
			if (ImGui::InputText("##val", buf, sizeof(buf),
				ImGuiInputTextFlags_EnterReturnsTrue))
				tryCommit();
			if (ImGui::IsItemDeactivatedAfterEdit())
				tryCommit();
			break;
		}
		case rt2::core::ScriptFieldType::Vec3:
		{
			glm::vec3 v = std::get<glm::vec3>(display.value);
			if (ImGui::DragFloat3("##val", &v[0], 0.1f))
			{
				display.value = v;
				changed = true;
			}
			break;
		}
		case rt2::core::ScriptFieldType::Color:
		{
			glm::vec3 v = std::get<glm::vec3>(display.value);
			if (ImGui::ColorEdit3("##val", &v[0]))
			{
				display.value = v;
				changed = true;
			}
			break;
		}
		}

		// Apply the changed field to the document. Only insert into the map
		// here, so untouched declared-but-unstored fields are not persisted.
		EditorMutationResult applied;
		applied.success = true;
		applied.effective = false;

		// Session management for continuous widgets. Activation MUST happen
		// before the first preview frame of the gesture so the per-frame
		// composite commit can open on the same frame the drag starts.
		if (isContinuous)
		{
			const unsigned int widgetId = ImGui::GetID("##val");
			if (ImGui::IsItemActivated())
			{
				if (!m_ScriptFieldSession.IsOpen() && m_Editable)
				{
					if (m_ScriptFieldSession.Begin(*m_SceneMgr, targetUuid,
						PrefabValueKind::ScriptState,
						PrefabComponentKeyFor<ScriptComponent>::value,
						PrefabValuePayload{
							std::optional<ScriptComponent>(*scriptState) },
						[this](const rt2::core::UUID& uuid,
							const PrefabValuePayload& raw) -> PrefabValuePayload {
							const auto live = m_SceneMgr->GetScriptState(uuid);
							if (!live.has_value()) return raw;
							return PrefabValuePayload{ live };
						}))
					{
						m_ScriptFieldSessionOwningWidgetId = widgetId;
						owningWidgetId = widgetId;
					}
				}
			}
			if (ImGui::IsItemDeactivatedAfterEdit())
			{
				if (m_ScriptFieldSession.IsOpen() && owningWidgetId == widgetId)
					pendingCloseWidgetId = widgetId;
			}
			else if (ImGui::IsItemDeactivated() && m_ScriptFieldSession.IsOpen() &&
			         owningWidgetId == widgetId)
			{
				cancelPending = true;
			}
		}

		if (changed)
		{
			if (isContinuous)
			{
				// Continuous widgets (Int/Float/Vec3/Color) publish a live
				// per-frame preview through the composite session (immutable
				// origin, rolling committed source, real result) and are
				// recorded once at session close (S6-C).
				auto target = *scriptState;
				target.fieldValues[desc.name] = display;
				if (m_ScriptFieldSession.IsOpen())
				{
					applied = m_ScriptFieldSession.Preview(*m_SceneMgr,
						PrefabValuePayload{ std::optional<ScriptComponent>(target) });
					if (applied.success)
						scriptState = m_SceneMgr->GetScriptState(targetUuid);
					ApplyMutation(applied);
				}
			}
			else
			{
				// Discrete widgets (Bool/String/Uuid): construct-then-Execute.
				// Build the after-state without mutating the live component so
				// the command's composite replay does the value write + marker
				// insertion + schema promotion atomically.
				//
				// A discrete edit must come only after any open continuous
				// field session finishes (S6-C fixup, P1 finding 3): issuing
				// it mid-gesture leaves the session's rolling source stale and
				// the close compensation out of sync. Abort the discrete edit
				// on a pending close.
				if (!ClosePreviewBeforeDiscrete(PreviewSessionKind::Script,
					m_ScriptFieldSession, m_ScriptFieldSessionOwningWidgetId))
					applied = EditorMutationResult::Failure(
						rt2::core::Error::InvalidRuntimeState, targetUuid.ToString(),
						"the active live-preview field edit could not be finalized");
				else
				{
					scriptState = m_SceneMgr->GetScriptState(targetUuid);
					sessionBefore = scriptState;
					auto after = *scriptState;
					after.fieldValues[desc.name] = display;
					auto cmd = MakeSetScriptCommandIfEffective(targetUuid,
						*sessionBefore, after);
					if (cmd)
					{
						applied = ExecuteCommandThroughHistory(m_CommandHistory, *m_SceneMgr, std::move(cmd));
						if (applied.success)
							scriptState = m_SceneMgr->GetScriptState(targetUuid);
						ApplyMutation(applied);
					}
				}
			}
		}

		ImGui::PopID();
	}

	ImGui::EndDisabled();

	// Deferred close AFTER the mutation block: commit (record one command)
	// or restore (Escape / returned-to-start, no history). The owning widget
	// ID is cleared only after the session actually closed; a pending close
	// keeps it and surfaces recovery (S6-C fixup, P1 finding 2).
	if (pendingCloseWidgetId != 0 && m_ScriptFieldSession.IsOpen() &&
	    m_ScriptFieldSessionOwningWidgetId == pendingCloseWidgetId)
	{
		ClosePreviewSession(PreviewSessionKind::Script, m_ScriptFieldSession,
			m_ScriptFieldSessionOwningWidgetId, /*finalize=*/true);
	}
	else if (cancelPending && m_ScriptFieldSession.IsOpen())
	{
		ClosePreviewSession(PreviewSessionKind::Script, m_ScriptFieldSession,
			m_ScriptFieldSessionOwningWidgetId, /*finalize=*/false);
	}
}

// ----------------------------------------------------------------------------
// Import Options modal
//
// Shown after the user picks a file from "Import Scene..." or "Load Mesh
// File...". Collects import settings (currently just mergeMegaMesh for OBJ)
// and dispatches via m_OnImportWithOptions. The modal is modal — the file
// dialog cannot open while it is up, so a second pick before dismissing is
// naturally blocked.
// ----------------------------------------------------------------------------
void SceneEditorUI::DrawImportOptionsModal()
{
	if (!m_ImportOptionsOpen || m_PendingImportPath.empty())
		return;

	// Detect OBJ by extension to decide whether to show the merge-mesh
	// checkbox. glTF has no import settings exposed yet.
	std::string ext;
	const auto& path = m_PendingImportPath;
	const auto dotPos = path.find_last_of('.');
	if (dotPos != std::string::npos)
	{
		ext = path.substr(dotPos + 1);
		std::transform(ext.begin(), ext.end(), ext.begin(),
		               [](unsigned char c) { return std::tolower(c); });
	}
	bool isObj = (ext == "obj");

	ImGui::OpenPopup("Import Options");
	if (ImGui::BeginPopupModal("Import Options", nullptr,
	                           ImGuiWindowFlags_AlwaysAutoResize))
	{
		// Filename (basename only, for readability).
		std::string basename = path;
		const auto slashPos = basename.find_last_of("/\\");
		if (slashPos != std::string::npos)
			basename = basename.substr(slashPos + 1);
		ImGui::Text("File: %s", basename.c_str());

		if (isObj)
		{
			ImGui::Separator();
			ImGui::Checkbox("Merge shapes into single mesh",
			                &m_PendingImportMergeMegaMesh);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(
					"When checked, all OBJ shapes are merged into one BLAS\n"
					"(faster ray traversal, no per-shape editing).\n"
					"When unchecked, each shape becomes its own entity.");
		}
		else
		{
			ImGui::Separator();
			ImGui::Checkbox("Treat untextured metals as dielectric",
			                &m_PendingImportAssumeDielectric);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(
					"glTF defines an absent metallicFactor as 1.0, so a material\n"
					"with no metallicRoughness texture and no authored factor is\n"
					"fully metallic and fully rough by spec — a rough mirror that\n"
					"renders as a grey patch which never converges.\n\n"
					"Exporters hit this constantly by omitting the value and\n"
					"assuming a dielectric default. When checked, such materials\n"
					"import as dielectric and each correction is recorded as a\n"
					"diagnostic. Stored per asset, so it survives a reload.");
		}

		ImGui::Separator();
		if (ImGui::Button("Import"))
		{
			ImportSettings settings;
			settings.mergeMegaMesh = m_PendingImportMergeMegaMesh;
			settings.assumeDielectricWithoutMetalRough =
				m_PendingImportAssumeDielectric;
			auto id = m_OnImportWithOptions(m_PendingImportPath, settings);
			if (id.IsValid())
			{
				SelectEntity(id);
				NotifySceneChanged();
			}
			m_PendingImportPath.clear();
			m_ImportOptionsOpen = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel"))
		{
			m_PendingImportPath.clear();
			m_ImportOptionsOpen = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}
