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

	// Add menu
	if (ImGui::Button("Add"))
		ImGui::OpenPopup("AddEntity");

	if (ImGui::BeginPopup("AddEntity"))
	{
		if (ImGui::MenuItem("Point Light"))
		{
			auto id = m_SceneMgr->AddLight("Point Light", {0, 5, 0}, {1, 1, 1}, 10.0f, false);
			m_SelectedEntity = id;
			NotifySceneChanged();
		}
		if (ImGui::MenuItem("Spot Light"))
		{
			auto id = m_SceneMgr->AddLight("Spot Light", {0, 5, 0}, {1, 1, 1}, 10.0f, true);
			m_SelectedEntity = id;
			NotifySceneChanged();
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Cube"))
		{
			auto id = m_SceneMgr->AddObjectWithGeometry("Cube",
				PrimitiveGeometry::CreateCube(1.0f), {0, 0.5f, 0});
			m_SelectedEntity = id;
			NotifySceneChanged();
		}
		if (ImGui::MenuItem("Sphere"))
		{
			auto id = m_SceneMgr->AddObjectWithGeometry("Sphere",
				PrimitiveGeometry::CreateSphere(0.5f), {0, 0.5f, 0});
			m_SelectedEntity = id;
			NotifySceneChanged();
		}
		if (ImGui::MenuItem("Plane"))
		{
			auto id = m_SceneMgr->AddObjectWithGeometry("Plane",
				PrimitiveGeometry::CreatePlane(5.0f), {0, 0, 0});
			m_SelectedEntity = id;
			NotifySceneChanged();
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Load Mesh File..."))
		{
			std::string path = FileDialog::OpenFile(
				"OBJ Files (*.obj)\0*.obj\0glTF Binary (*.glb)\0*.glb\0glTF JSON (*.gltf)\0*.gltf\0All Files (*.*)\0*.*\0");
			if (!path.empty() && m_OnLoadMeshFile)
			{
				auto id = m_OnLoadMeshFile(path);
				m_SelectedEntity = id;
				NotifySceneChanged();
			}
		}
		ImGui::EndPopup();
	}

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

	if (m_SelectedEntity.IsValid() && ImGui::Button("Delete Selected"))
	{
		m_SceneMgr->RemoveEntity(m_SelectedEntity);
		m_SelectedEntity = SceneManager::EntityId{};
		NotifySceneChanged();
		m_TreeDirty = true;
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
	else if (m_SceneMgr->HasLight(entity))
		label = "[Light] " + name;
	else if (m_SceneMgr->HasMeshRef(entity))
		label = "[Mesh] " + name;
	else
		label = "[Entity] " + name;

	ImGui::PushID((int)entity.id);

	bool isSelected = (m_SelectedEntity.id == entity.id);
	bool hasChildren = m_SceneMgr->HasChildren(entity);

	if (hasChildren)
	{
		bool open = ImGui::TreeNodeEx(label.c_str(),
			ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
			ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen |
			(isSelected ? ImGuiTreeNodeFlags_Selected : 0));

		// Click on the tree node label selects it
		if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
			m_SelectedEntity = entity;

		// Right-click context menu for deletion
		if (ImGui::BeginPopupContextItem("EntityCtx"))
		{
			if (ImGui::MenuItem("Delete"))
			{
				if (m_SelectedEntity.id == entity.id)
					m_SelectedEntity = SceneManager::EntityId{};
				m_SceneMgr->RemoveEntity(entity);
				NotifySceneChanged();
				m_TreeDirty = true;
				ImGui::EndPopup();
				ImGui::PopID();
				return;
			}
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
			m_SelectedEntity = entity;

		if (ImGui::BeginPopupContextItem("EntityCtx"))
		{
			if (ImGui::MenuItem("Delete"))
			{
				if (m_SelectedEntity.id == entity.id)
					m_SelectedEntity = SceneManager::EntityId{};
				m_SceneMgr->RemoveEntity(entity);
				NotifySceneChanged();
				m_TreeDirty = true;
			}
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

	if (!m_SceneMgr || !m_SelectedEntity.IsValid() || !m_SceneMgr->IsEntityAlive(m_SelectedEntity))
	{
		ImGui::TextDisabled("Select an entity in the Outliner");
		ImGui::End();
		return;
	}

	auto entity = m_SelectedEntity;
	std::string name = m_SceneMgr->GetEntityName(entity);

	// Name field
	char nameBuf[128];
	snprintf(nameBuf, sizeof(nameBuf), "%s", name.c_str());
	ImGui::Text("Name:");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(200.0f);
	if (ImGui::InputText("##EntityName", nameBuf, sizeof(nameBuf), ImGuiInputTextFlags_EnterReturnsTrue))
	{
		m_SceneMgr->SetEntityName(entity, nameBuf);
	}
	ImGui::Separator();

	RenderTransformEditor(entity);
	ImGui::Separator();

	if (m_SceneMgr->HasMeshRef(entity))
		RenderMaterialEditor(entity);

	if (m_SceneMgr->HasLight(entity))
		RenderLightEditor(entity);

	if (m_SceneMgr->HasCamera(entity))
		RenderCameraEditor(entity);

	ImGui::End();
}

void SceneEditorUI::RenderTransformEditor(SceneManager::EntityId entity)
{
	ImGui::Text("Transform");

	glm::vec3 pos, rot;
	float scale;
	if (!m_SceneMgr->GetTransform(entity, pos, rot, scale))
		return;

	bool changed = false;
	ImGui::PushID("Transform");
	ImGui::SetNextItemWidth(180.0f);
	if (ImGui::DragFloat3("Position", &pos[0], 0.1f))
		changed = true;
	ImGui::SetNextItemWidth(180.0f);
	if (ImGui::DragFloat3("Rotation", &rot[0], 1.0f))
		changed = true;
	ImGui::SetNextItemWidth(180.0f);
	if (ImGui::DragFloat("Scale", &scale, 0.05f, 0.01f, 100.0f))
		changed = true;
	ImGui::PopID();

	if (changed)
	{
		m_SceneMgr->SetTransform(entity, pos, rot, scale);
		NotifyTransformChanged();
	}
}

void SceneEditorUI::RenderMaterialEditor(SceneManager::EntityId entity)
{
	ImGui::Text("Material");

	uint32_t meshIdx;
	int matIdx;
	if (!m_SceneMgr->GetMeshRef(entity, meshIdx, matIdx))
		return;

	ImGui::Text("Mesh Index: %u", meshIdx);

	// Material index combo
	const auto& materials = m_SceneMgr->GetMaterials();
	int current = matIdx;
	ImGui::SetNextItemWidth(180.0f);
	if (ImGui::Combo("Material", &current, [](void* data, int idx, const char** out_text) -> bool {
		auto* mats = static_cast<const std::vector<SceneMaterial>*>(data);
		if (idx < 0 || idx >= (int)mats->size()) return false;
		*out_text = "Material"; // Could show a name if we had one
		return true;
	}, (void*)&materials, (int)materials.size()))
	{
		m_SceneMgr->SetMaterial(entity, current);
		NotifySceneChanged();
	}

	// Inline material editor (edits the material that this entity references)
	if (current >= 0 && current < (int)materials.size())
	{
		auto& mat = m_SceneMgr->GetMaterial(current);
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
			NotifySceneChanged();
	}
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
	if (ImGui::ColorEdit3("Color", &color[0]))
		changed = true;
	if (ImGui::DragFloat("Intensity", &intensity, 0.1f, 0.0f, 1000.0f, "%.1f"))
		changed = true;
	if (ImGui::Checkbox("Spot", &isSpot))
		changed = true;
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

	ImGui::DragFloat("FOV", &cam->verticalFOV, 1.0f, 10.0f, 170.0f, "%.1f");
	ImGui::DragFloat("Aperture", &cam->aperture, 0.001f, 0.0f, 5.0f, "%.3f");
	ImGui::DragFloat("Focus Distance", &cam->focusDistance, 0.1f, 0.1f, 1000.0f, "%.1f");

	// Camera edits need accumulation reset but no GPU scene rebuild
	NotifyTransformChanged();
}