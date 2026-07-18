#include "SceneEditorUI.h"
#include "PrimitiveGeometry.h"
#include "FileDialog.h"
#include "ECSComponents.h"
#include "ECSScene.h"
#include "RTLog.h"
#include "imgui.h"
#include <cstdio>

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

SceneManager::EntityId SceneEditorUI::SelectedEntity() const
{
	if (!m_SceneMgr)
		return {};
	const auto uuid = m_Selection.Primary();
	return uuid.IsNull() ? SceneManager::EntityId{} :
		SceneManager::EntityId{ m_SceneMgr->FindEntityByUuid(uuid) };
}

void SceneEditorUI::SelectEntity(SceneManager::EntityId entity, bool toggle)
{
	if (!m_SceneMgr || !entity.IsValid())
	{
		if (!toggle) m_Selection.Clear();
		return;
	}
	const auto uuid = m_SceneMgr->GetEntityUuid(entity);
	if (toggle) m_Selection.Toggle(uuid);
	else m_Selection.SelectOnly(uuid);
}

bool SceneEditorUI::IsSelected(SceneManager::EntityId entity) const
{
	return m_SceneMgr && entity.IsValid() &&
		m_Selection.Contains(m_SceneMgr->GetEntityUuid(entity));
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

	// Add menu — disabled during Play
	ImGui::BeginDisabled(!m_Editable);
	if (ImGui::Button("Add"))
		ImGui::OpenPopup("AddEntity");

	if (ImGui::BeginPopup("AddEntity"))
	{
		if (ImGui::MenuItem("Emissive Light"))
		{
			SceneMaterial mat;
			mat.emissiveColor = {1.0f, 1.0f, 1.0f};
			mat.emissiveIntensity = 10.0f;
			int matIdx = m_SceneMgr->AddMaterial(mat);
			auto id = m_SceneMgr->AddObjectWithGeometry("Light",
				PrimitiveGeometry::CreateSphere(0.2f), {0, 3, 0}, {0, 0, 0}, 1.0f, matIdx);
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

	// Entity tree (root entities → children)
	size_t count = m_SceneMgr->GetEntityCount();
	ImGui::Text("Entities: %d", (int)count);

	if (count == 0)
	{
		ImGui::TextDisabled("  (empty — load a scene or add an entity)");
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
		const auto selected = m_Selection.Ordered();
		for (const auto& uuid : selected)
		{
			const entt::entity entity = m_SceneMgr->FindEntityByUuid(uuid);
			if (entity != entt::null)
				m_SceneMgr->RemoveEntity(SceneManager::EntityId{ entity });
		}
		m_Selection.Clear();
		NotifySceneChanged();
		m_TreeDirty = true;
	}
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

	ImGui::End();
}

void SceneEditorUI::RenderEntityTree(SceneManager::EntityId entity, int depth)
{
	if (!entity.IsValid()) return;

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

	ImGui::PushID((int)entity.id);

	bool isSelected = IsSelected(entity);
	bool hasChildren = m_SceneMgr->HasChildren(entity);

	if (hasChildren)
	{
		bool open = ImGui::TreeNodeEx(label.c_str(),
			ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
			ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen |
			(isSelected ? ImGuiTreeNodeFlags_Selected : 0));

		// Click on the tree node label selects it
		if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
			SelectEntity(entity, ImGui::GetIO().KeyCtrl);

		// Right-click context menu for deletion
		if (ImGui::BeginPopupContextItem("EntityCtx"))
		{
			ImGui::BeginDisabled(!m_Editable);
			if (ImGui::MenuItem("Delete"))
			{
				if (IsSelected(entity))
					m_Selection.Remove(m_SceneMgr->GetEntityUuid(entity));
				m_SceneMgr->RemoveEntity(entity);
				NotifySceneChanged();
				m_TreeDirty = true;
				ImGui::EndDisabled();
				ImGui::EndPopup();
				ImGui::PopID();
				return;
			}
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

		if (ImGui::BeginPopupContextItem("EntityCtx"))
		{
			ImGui::BeginDisabled(!m_Editable);
			if (ImGui::MenuItem("Delete"))
			{
				if (IsSelected(entity))
					m_Selection.Remove(m_SceneMgr->GetEntityUuid(entity));
				m_SceneMgr->RemoveEntity(entity);
				NotifySceneChanged();
				m_TreeDirty = true;
			}
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
		m_Selection.Prune(m_SceneMgr->AuthoringDoc());
	const auto selectedEntity = SelectedEntity();
	if (!m_SceneMgr || !selectedEntity.IsValid() || !m_SceneMgr->IsEntityAlive(selectedEntity))
	{
		ImGui::TextDisabled("Select an entity in the Outliner");
		ImGui::End();
		return;
	}

	auto entity = selectedEntity;
	std::string name = m_SceneMgr->GetEntityName(entity);

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
	ImGui::SetNextItemWidth(180.0f);
	if (ImGui::DragFloat3("Position", &pos[0], 0.1f))
	{
		if (m_TransformSnap.enabled) pos = SnapValues(pos, m_TransformSnap.translation);
		changed = true;
	}
	ImGui::SetNextItemWidth(180.0f);
	if (ImGui::DragFloat3("Rotation", &rot[0], 1.0f))
	{
		if (m_TransformSnap.enabled) rot = SnapValues(rot, m_TransformSnap.rotationDegrees);
		changed = true;
	}
	ImGui::SetNextItemWidth(180.0f);
	if (ImGui::DragFloat3("Scale", &scale[0], 0.05f))
	{
		if (m_TransformSnap.enabled) scale = SnapValues(scale, m_TransformSnap.scale);
		changed = true;
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

	ImGui::BeginDisabled(!m_Editable);
	ImGui::DragFloat("FOV", &cam->verticalFOV, 1.0f, 10.0f, 170.0f, "%.1f");
	ImGui::DragFloat("Aperture", &cam->aperture, 0.001f, 0.0f, 5.0f, "%.3f");
	ImGui::DragFloat("Focus Distance", &cam->focusDistance, 0.1f, 0.1f, 1000.0f, "%.1f");
	ImGui::EndDisabled();

	// Camera edits need accumulation reset but no GPU scene rebuild
	NotifyTransformChanged();
}
