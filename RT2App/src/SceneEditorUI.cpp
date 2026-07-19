#include "SceneEditorUI.h"
#include "PrimitiveGeometry.h"
#include "FileDialog.h"
#include "ECSComponents.h"
#include "ECSScene.h"
#include "EditorCommands.h"
#include "RTLog.h"
#include "imgui.h"
#include <cstdio>
#include <algorithm>
#include <cctype>
#include <cstring>

void SceneEditorUI::RenderPanels()
{
	RenderOutliner();
	RenderInspector();
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
	if (!m_CommandHistory) return;
	DiscardTransformEditSession();
	const auto result = m_CommandHistory->Undo(*m_SceneMgr);
	ApplyMutation(result);
}

void SceneEditorUI::Redo()
{
	if (!m_CommandHistory) return;
	DiscardTransformEditSession();
	const auto result = m_CommandHistory->Redo(*m_SceneMgr);
	ApplyMutation(result);
}

void SceneEditorUI::BeginTransformEditSession(const rt2::core::UUID& target,
                                              const EditableTRS& beforeLocal)
{
	m_TransformEditSession.target = target;
	m_TransformEditSession.beforeLocal = beforeLocal;
	m_TransformEditSession.open = true;
	m_TransformEditSession.owningWidgetId = ImGui::GetID("");
}

void SceneEditorUI::DiscardTransformEditSession()
{
	m_TransformEditSession.open = false;
	m_TransformEditSession.target = rt2::core::UUID{};
	m_TransformEditSession.owningWidgetId = 0;
}

void SceneEditorUI::CloseTransformEditSessionIfOwning(const rt2::core::UUID& target,
                                                      const EditableTRS& afterLocal)
{
	if (!m_TransformEditSession.open) return;
	if (!(m_TransformEditSession.target == target)) return;

	// Defensive guards per spec: target no longer the inspected entity,
	// entity dead, or m_Editable false (Play started mid-drag) -> discard.
	const auto entity = m_SceneMgr->FindEntityByUuid(target);
	if (entity == entt::null || !m_Editable)
	{
		DiscardTransformEditSession();
		return;
	}

	// Record-on-release via RecordApplied. The before/after comparison is
	// the sole authority (handled inside MakeTransformCommandIfEffective,
	// which returns null for a no-op).
	EditableTRS after = afterLocal;
	if (m_CommandHistory)
	{
		auto cmd = MakeTransformCommandIfEffective(target,
			m_TransformEditSession.beforeLocal, after);
		if (cmd)
		{
			// The mutation was already applied incrementally by the
			// per-frame SetLocalTransform/TrySetWorldTransform. Record it
			// without re-applying.
			EditorMutationResult applied;
			applied.success = true;
			applied.syncImpact = rt2::core::SyncImpact::Transform;
			applied.affectedEntities.push_back(target);
			m_CommandHistory->RecordApplied(std::move(cmd), *m_SceneMgr, applied);
		}
	}
	DiscardTransformEditSession();
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
	auto result = m_CommandHistory->Execute(std::move(cmd), *m_SceneMgr);
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
			ApplyMutation(m_SceneMgr->CreateEmpty(), true);
		const auto selectedParent = m_State.Selection().Primary();
		ImGui::BeginDisabled(selectedParent.IsNull() || m_State.IsLocked(selectedParent));
		if (ImGui::MenuItem("Child Empty"))
			ApplyMutation(m_SceneMgr->CreateEmpty("Empty", selectedParent), true);
		ImGui::EndDisabled();
		ImGui::Separator();
		if (ImGui::MenuItem("Emissive Light"))
		{
			SceneMaterial mat;
			mat.emissiveColor = {1.0f, 1.0f, 1.0f};
			mat.emissiveIntensity = 10.0f;
			int matIdx = m_SceneMgr->AddMaterial(mat);
			auto id = m_SceneMgr->AddObjectWithGeometry("Light",
				PrimitiveGeometry::CreateSphere(0.2f), {0, 3, 0}, {0, 0, 0}, 1.0f, matIdx);
			m_SceneMgr->GetECS().registry.emplace_or_replace<PrimitiveComponent>(id.id,
				PrimitiveComponent{PrimitiveComponent::Sphere, 0.4f, 24, 16});
			SelectEntity(id);
			NotifySceneChanged();
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Cube"))
		{
			int matIdx = m_SceneMgr->AddMaterial(SceneMaterial{});
			auto id = m_SceneMgr->AddObjectWithGeometry("Cube",
				PrimitiveGeometry::CreateCube(1.0f), {0, 0.5f, 0}, {0, 0, 0}, 1.0f, matIdx);
			// Attach PrimitiveComponent so the entity can be saved to .rt2scene
			m_SceneMgr->GetECS().registry.emplace_or_replace<PrimitiveComponent>(id.id,
				PrimitiveComponent{PrimitiveComponent::Cube, 1.0f, 24, 16});
			SelectEntity(id);
			NotifySceneChanged();
		}
		if (ImGui::MenuItem("Sphere"))
		{
			int matIdx = m_SceneMgr->AddMaterial(SceneMaterial{});
			auto id = m_SceneMgr->AddObjectWithGeometry("Sphere",
				PrimitiveGeometry::CreateSphere(0.5f), {0, 0.5f, 0}, {0, 0, 0}, 1.0f, matIdx);
			m_SceneMgr->GetECS().registry.emplace_or_replace<PrimitiveComponent>(id.id,
				PrimitiveComponent{PrimitiveComponent::Sphere, 1.0f, 24, 16});
			SelectEntity(id);
			NotifySceneChanged();
		}
		if (ImGui::MenuItem("Plane"))
		{
			int matIdx = m_SceneMgr->AddMaterial(SceneMaterial{});
			auto id = m_SceneMgr->AddObjectWithGeometry("Plane",
				PrimitiveGeometry::CreatePlane(5.0f), {0, 0, 0}, {0, 0, 0}, 1.0f, matIdx);
			m_SceneMgr->GetECS().registry.emplace_or_replace<PrimitiveComponent>(id.id,
				PrimitiveComponent{PrimitiveComponent::Plane, 5.0f, 24, 16});
			SelectEntity(id);
			NotifySceneChanged();
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Import Scene..."))
		{
			const auto initialDirectory = m_DialogInitialDirectory
				? m_DialogInitialDirectory() : std::filesystem::path{};
			std::string path = FileDialog::OpenFile(
				L"glTF Binary (*.glb)\0*.glb\0glTF JSON (*.gltf)\0*.gltf\0OBJ Files (*.obj)\0*.obj\0All Files (*.*)\0*.*\0",
				initialDirectory);
			if (!path.empty() && m_OnImportGltf)
			{
				auto id = m_OnImportGltf(path);
				if (id.IsValid())
				{
					SelectEntity(id);
					NotifySceneChanged();
				}
			}
		}
		if (ImGui::MenuItem("Load Mesh File..."))
		{
			const auto initialDirectory = m_DialogInitialDirectory
				? m_DialogInitialDirectory() : std::filesystem::path{};
			std::string path = FileDialog::OpenFile(
				L"OBJ Files (*.obj)\0*.obj\0glTF Binary (*.glb)\0*.glb\0glTF JSON (*.gltf)\0*.gltf\0All Files (*.*)\0*.*\0",
				initialDirectory);
			if (!path.empty() && m_OnLoadMeshFile)
			{
				auto id = m_OnLoadMeshFile(path);
				if (id.IsValid())
				{
					SelectEntity(id);
					NotifySceneChanged();
				}
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
		{
			ApplyMutation(m_SceneMgr->RemoveSubtrees(m_State.Selection().Ordered()));
			m_State.Selection().Clear();
			m_TreeDirty = true;
		}
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
		ApplyMutation(m_State.Paste(*m_SceneMgr), true);
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(!m_Editable || m_State.Selection().Empty());
	if (ImGui::Button("Duplicate"))
		ApplyMutation(m_SceneMgr->DuplicateSubtrees(m_State.Selection().Ordered()), true);
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
			ApplyMutation(m_State.Paste(*m_SceneMgr), true);
		if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D))
			ApplyMutation(m_SceneMgr->DuplicateSubtrees(m_State.Selection().Ordered()), true);
		if (ImGui::IsKeyPressed(ImGuiKey_Delete))
		{
			std::string reason;
			if (MutationSelectionAllowed(reason))
			{
				ApplyMutation(m_SceneMgr->RemoveSubtrees(m_State.Selection().Ordered()));
				m_State.Selection().Clear();
				m_TreeDirty = true;
			}
			else m_MutationError = reason;
		}
	}

	ImGui::End();
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
						ApplyMutation(m_SceneMgr->Reparent(sources, uuid));
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
				ApplyMutation(m_SceneMgr->SetVisibility({ uuid }, !directlyVisible));
			if (ImGui::MenuItem("Create Child"))
				ApplyMutation(m_SceneMgr->CreateEmpty("Empty", uuid), true);
			if (m_SceneMgr->GetParent(entity).IsValid() && ImGui::MenuItem("Move to Scene Root"))
				ApplyMutation(m_SceneMgr->Reparent({ uuid }, std::nullopt));
			if (ImGui::MenuItem("Paste as Child", nullptr, false, m_State.HasClipboard()))
				ApplyMutation(m_State.Paste(*m_SceneMgr, uuid), true);
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
				ApplyMutation(m_SceneMgr->DuplicateSubtrees({ uuid }), true);
			ImGui::BeginDisabled(directlyLocked);
			if (ImGui::MenuItem("Delete"))
			{
				if (IsSelected(entity))
					m_State.Selection().Remove(m_SceneMgr->GetEntityUuid(entity));
				ApplyMutation(m_SceneMgr->RemoveSubtrees({ uuid }));
				m_TreeDirty = true;
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
				ApplyMutation(m_SceneMgr->SetVisibility({ uuid }, !directlyVisible));
			if (ImGui::MenuItem("Create Child"))
				ApplyMutation(m_SceneMgr->CreateEmpty("Empty", uuid), true);
			if (m_SceneMgr->GetParent(entity).IsValid() && ImGui::MenuItem("Move to Scene Root"))
				ApplyMutation(m_SceneMgr->Reparent({ uuid }, std::nullopt));
			if (ImGui::MenuItem("Paste as Child", nullptr, false, m_State.HasClipboard()))
				ApplyMutation(m_State.Paste(*m_SceneMgr, uuid), true);
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
				ApplyMutation(m_SceneMgr->DuplicateSubtrees({ uuid }), true);
			ImGui::BeginDisabled(directlyLocked);
			if (ImGui::MenuItem("Delete"))
			{
				if (IsSelected(entity))
					m_State.Selection().Remove(m_SceneMgr->GetEntityUuid(entity));
				ApplyMutation(m_SceneMgr->RemoveSubtrees({ uuid }));
				m_TreeDirty = true;
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
	const bool directlyLocked = m_State.IsLocked(m_SceneMgr->GetEntityUuid(entity));
	if (directlyLocked)
		ImGui::TextDisabled("Directly locked in the editor");
	ImGui::BeginDisabled(directlyLocked);

	// Name field
	char nameBuf[128];
	snprintf(nameBuf, sizeof(nameBuf), "%s", name.c_str());
	ImGui::Text("Name:");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(200.0f);
	ImGui::BeginDisabled(!m_Editable);
	if (ImGui::InputText("##EntityName", nameBuf, sizeof(nameBuf), ImGuiInputTextFlags_EnterReturnsTrue))
	{
		m_SceneMgr->SetEntityName(entity, nameBuf);
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
				ImGui::DragFloat3("Linear Velocity", &mc.linearVelocity[0], 0.1f);
				ImGui::EndDisabled();
				ImGui::SameLine();
				ImGui::BeginDisabled(!m_Editable);
				if (ImGui::Button("Remove Motion"))
				{
					reg.remove<MotionComponent>(entity.id);
					NotifySceneChanged();
				}
				ImGui::EndDisabled();
			}
			else
			{
				ImGui::BeginDisabled(!m_Editable);
				if (ImGui::Button("Add Motion"))
				{
					reg.emplace<MotionComponent>(entity.id, MotionComponent{});
					NotifySceneChanged();
				}
				ImGui::EndDisabled();
			}
		}
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
	ImGui::BeginDisabled(!m_Editable);

	// Phase 3A: record-on-release transform edit session. Per-widget
	// IsItemActivated/IsItemDeactivatedAfterEdit checks immediately after
	// EACH of the three DragFloat3s (ImGui's last-item rule). The first
	// activation while no session is open captures beforeLocal BEFORE any
	// mutation that frame and opens the session, owned by that widget. The
	// owning widget's IsItemDeactivatedAfterEdit closes the session and
	// records via RecordApplied. IsItemDeactivated without AfterEdit
	// (Escape cancel — ImGui reverts the value itself) discards the session
	// and records nothing.
	EditableTRS beforeLocalCapture;
	bool hasLocalForCapture = m_SceneMgr->GetLocalTransform(entity, beforeLocalCapture);
	const unsigned int owningWidgetId = m_TransformEditSession.open
		? m_TransformEditSession.owningWidgetId : 0;

	ImGui::SetNextItemWidth(180.0f);
	if (ImGui::DragFloat3("Position", &pos[0], 0.1f))
	{
		if (m_TransformSnap.enabled) pos = SnapValues(pos, m_TransformSnap.translation);
		changed = true;
	}
	const unsigned int posWidgetId = ImGui::GetID("Position");
	if (ImGui::IsItemActivated())
	{
		if (!m_TransformEditSession.open && hasLocalForCapture && m_Editable)
		{
			BeginTransformEditSession(targetUuid, beforeLocalCapture);
			m_TransformEditSession.owningWidgetId = posWidgetId;
		}
	}
	if (ImGui::IsItemDeactivatedAfterEdit())
	{
		if (m_TransformEditSession.open && owningWidgetId == posWidgetId)
		{
			EditableTRS afterLocal;
			if (m_SceneMgr->GetLocalTransform(entity, afterLocal))
				CloseTransformEditSessionIfOwning(targetUuid, afterLocal);
		}
	}
	else if (ImGui::IsItemDeactivated() && m_TransformEditSession.open &&
	         owningWidgetId == posWidgetId)
	{
		// Escape cancel: ImGui reverted the value; discard the session.
		DiscardTransformEditSession();
	}

	ImGui::SetNextItemWidth(180.0f);
	if (ImGui::DragFloat3("Rotation", &rot[0], 1.0f))
	{
		if (m_TransformSnap.enabled) rot = SnapValues(rot, m_TransformSnap.rotationDegrees);
		changed = true;
	}
	const unsigned int rotWidgetId = ImGui::GetID("Rotation");
	if (ImGui::IsItemActivated())
	{
		if (!m_TransformEditSession.open && hasLocalForCapture && m_Editable)
		{
			BeginTransformEditSession(targetUuid, beforeLocalCapture);
			m_TransformEditSession.owningWidgetId = rotWidgetId;
		}
	}
	if (ImGui::IsItemDeactivatedAfterEdit())
	{
		if (m_TransformEditSession.open && owningWidgetId == rotWidgetId)
		{
			EditableTRS afterLocal;
			if (m_SceneMgr->GetLocalTransform(entity, afterLocal))
				CloseTransformEditSessionIfOwning(targetUuid, afterLocal);
		}
	}
	else if (ImGui::IsItemDeactivated() && m_TransformEditSession.open &&
	         owningWidgetId == rotWidgetId)
	{
		DiscardTransformEditSession();
	}

	ImGui::SetNextItemWidth(180.0f);
	if (ImGui::DragFloat3("Scale", &scale[0], 0.05f))
	{
		if (m_TransformSnap.enabled) scale = SnapValues(scale, m_TransformSnap.scale);
		changed = true;
	}
	const unsigned int scaleWidgetId = ImGui::GetID("Scale");
	if (ImGui::IsItemActivated())
	{
		if (!m_TransformEditSession.open && hasLocalForCapture && m_Editable)
		{
			BeginTransformEditSession(targetUuid, beforeLocalCapture);
			m_TransformEditSession.owningWidgetId = scaleWidgetId;
		}
	}
	if (ImGui::IsItemDeactivatedAfterEdit())
	{
		if (m_TransformEditSession.open && owningWidgetId == scaleWidgetId)
		{
			EditableTRS afterLocal;
			if (m_SceneMgr->GetLocalTransform(entity, afterLocal))
				CloseTransformEditSessionIfOwning(targetUuid, afterLocal);
		}
	}
	else if (ImGui::IsItemDeactivated() && m_TransformEditSession.open &&
	         owningWidgetId == scaleWidgetId)
	{
		DiscardTransformEditSession();
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

	ImGui::BeginDisabled(!m_Editable);
	// Material index combo
	const auto& materials = m_SceneMgr->GetMaterials();
	int current = matIdx;
	ImGui::SetNextItemWidth(180.0f);
	if (ImGui::Combo("Material", &current, [](void* data, int idx, const char** out_text) -> bool {
		auto* mats = static_cast<const std::vector<SceneMaterial>*>(data);
		if (idx < 0 || idx >= (int)mats->size()) return false;
		static char buf[64];
		snprintf(buf, sizeof(buf), "Material %d", idx);
		*out_text = buf;
		return true;
	}, (void*)&materials, (int)materials.size()))
	{
		m_SceneMgr->SetMaterial(entity, current);
		NotifySceneChanged();
	}

	ImGui::SameLine();
	if (ImGui::Button("Duplicate"))
	{
		// Clone current material so this entity gets its own independent copy
		if (current >= 0 && current < (int)materials.size())
		{
			SceneMaterial copy = m_SceneMgr->GetMaterial(current);
			int newIdx = m_SceneMgr->AddMaterial(copy);
			m_SceneMgr->SetMaterial(entity, newIdx);
			NotifySceneChanged();
		}
	}

	// Inline material editor (edits the material that this entity references).
	// Edits go through SetMaterialProperties so dirty tracking, the correct
	// GPU sync path, and durable MaterialOverrideComponent recording on
	// imported entities all fire.
	if (current >= 0 && current < (int)materials.size())
	{
		SceneMaterial mat = m_SceneMgr->GetMaterial(current);
		bool matChanged = false;

		ImGui::Indent();
		if (ImGui::ColorEdit3("Base Color", &mat.baseColor[0]))
			matChanged = true;
		if (ImGui::DragFloat("Metallic", &mat.metallic, 0.01f, 0.0f, 1.0f, "%.2f"))
			matChanged = true;
		if (ImGui::DragFloat("Roughness", &mat.roughness, 0.01f, 0.0f, 1.0f, "%.2f"))
			matChanged = true;
		if (ImGui::DragFloat("IOR", &mat.ior, 0.01f, 1.0f, 3.0f, "%.2f"))
			matChanged = true;
		if (ImGui::ColorEdit3("Emissive", &mat.emissiveColor[0]))
			matChanged = true;
		if (ImGui::DragFloat("Emissive Intensity", &mat.emissiveIntensity, 0.1f, 0.0f, 100.0f, "%.1f"))
			matChanged = true;
		ImGui::Unindent();

		if (matChanged)
		{
			m_SceneMgr->SetMaterialProperties(current, mat);
			NotifySceneChanged();
		}
	}
	ImGui::EndDisabled();
}

void SceneEditorUI::RenderLightEditor(SceneManager::EntityId entity)
{
	ImGui::Text("Light");

	glm::vec3 color;
	float intensity;
	bool isSpot;
	if (!m_SceneMgr->GetLightProperties(entity, color, intensity, isSpot))
		return;

	bool changed = false;
	ImGui::PushID("Light");
	ImGui::BeginDisabled(!m_Editable);
	if (ImGui::ColorEdit3("Color", &color[0]))
		changed = true;
	if (ImGui::DragFloat("Intensity", &intensity, 0.1f, 0.0f, 1000.0f, "%.1f"))
		changed = true;
	if (ImGui::Checkbox("Spot", &isSpot))
		changed = true;
	ImGui::EndDisabled();
	ImGui::PopID();

	if (changed)
	{
		m_SceneMgr->SetLightProperties(entity, color, intensity, isSpot);
		NotifySceneChanged();
	}
}

void SceneEditorUI::RenderCameraEditor(SceneManager::EntityId entity)
{
	ImGui::Text("Camera");

	// Access camera component through SceneManager's ECS
	auto& reg = const_cast<entt::registry&>(m_SceneMgr->GetECS().registry);
	if (!reg.valid(entity.id)) return;
	auto* cam = reg.try_get<CameraComponent>(entity.id);
	if (!cam) return;

	float verticalFOV = cam->verticalFOV;
	float aperture = cam->aperture;
	float focusDistance = cam->focusDistance;
	ImGui::BeginDisabled(!m_Editable);
	bool changed = ImGui::DragFloat("FOV", &verticalFOV, 1.0f, 10.0f, 170.0f, "%.1f");
	changed |= ImGui::DragFloat("Aperture", &aperture, 0.001f, 0.0f, 5.0f, "%.3f");
	changed |= ImGui::DragFloat("Focus Distance", &focusDistance, 0.1f, 0.1f, 1000.0f, "%.1f");
	if (changed)
		m_SceneMgr->SetCameraProperties(entity, verticalFOV, aperture, focusDistance);
	const auto uuid = m_SceneMgr->GetEntityUuid(entity);
	if (ImGui::Button("View Through Camera") && m_OnViewThroughCamera)
		m_OnViewThroughCamera(uuid);
	ImGui::SameLine();
	if (ImGui::Button("Align Camera to View") && m_OnAlignCameraToView)
		m_OnAlignCameraToView(uuid);
	ImGui::EndDisabled();
}
