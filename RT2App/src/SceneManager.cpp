#include "SceneManager.h"
#include "SceneLoader.h"
#include "SceneGraph.h"
#include "RTLog.h"
#include "stb_image.h"
#include <tinyexr.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstdio>
#include <set>
#include <map>

bool SceneManager::LoadScene(const std::string& filepath)
{
	printf("[Scene] LoadScene: '%s'\n", filepath.c_str());
	fflush(stdout);

	std::string ext = filepath.substr(filepath.find_last_of('.') + 1);
	std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
	bool isObj = (ext == "obj");

	if (isObj)
	{
		if (!SceneLoader::LoadObjIntoECS(m_EcsScene, filepath))
		{
			printf("[Scene] LoadObjIntoECS failed!\n");
			return false;
		}
		printf("[Scene] LoadObjIntoECS succeeded\n");
		fflush(stdout);
		return true;
	}

	if (!SceneLoader::LoadIntoECS(m_EcsScene, filepath))
	{
		printf("[Scene] SceneLoader::LoadIntoECS failed!\n");
		return false;
	}
	printf("[Scene] SceneLoader::LoadIntoECS succeeded\n");

	const auto& cam = m_EcsScene.camera;
	printf("[Scene] Camera: pos=(%.1f,%.1f,%.1f), forward=(%.1f,%.1f,%.1f), fov=%.1f\n",
	       cam.position.x, cam.position.y, cam.position.z,
	       cam.forwardDirection.x, cam.forwardDirection.y, cam.forwardDirection.z,
	       cam.verticalFOV);

	{
		auto& reg = m_EcsScene.registry;

		std::vector<entt::entity> roots;
		auto view = reg.view<Transform>();
		for (auto entity : view)
		{
			auto* h = reg.try_get<Hierarchy>(entity);
			if (!h || h->parent == entt::null)
				roots.push_back(entity);
		}

		if (!roots.empty())
		{
			std::string name = filepath;
			size_t lastSlash = name.find_last_of("/\\");
			if (lastSlash != std::string::npos)
				name = name.substr(lastSlash + 1);
			size_t lastDot = name.find_last_of('.');
			if (lastDot != std::string::npos)
				name = name.substr(0, lastDot);

			auto rootEntity = reg.create();
			Transform& tf = reg.emplace<Transform>(rootEntity);
			tf.dirty = true;
			reg.emplace<NameComponent>(rootEntity, name);
			reg.emplace<VisibleComponent>(rootEntity);
			Hierarchy& rootHier = reg.emplace<Hierarchy>(rootEntity);
			rootHier.parent = entt::null;

			for (auto child : roots)
			{
				auto* childHier = reg.try_get<Hierarchy>(child);
				if (!childHier)
					childHier = &reg.emplace<Hierarchy>(child);
				childHier->parent = rootEntity;
				rootHier.children.push_back(child);
				SceneGraph::SetLocalDirty(reg, child);
			}

			SceneGraph::SetLocalDirty(reg, rootEntity);
			SceneGraph::UpdateWorldTransforms(reg);
		}
	}

	printf("[Scene] Loaded %d meshes, %d materials, %d lights, %d textures\n",
	       (int)m_EcsScene.meshRegistry.GetCount(), (int)m_EcsScene.materials.size(),
	       (int)m_EcsScene.lights.size(), (int)m_EcsScene.textures.size());

	m_EntityCacheDirty = true;
	return true;
}

bool SceneManager::LoadEnvMap(const std::string& filepath)
{
	printf("[EnvMap] Loading '%s'\n", filepath.c_str());

	bool isEXR = filepath.size() >= 4 &&
	             (filepath.compare(filepath.size() - 4, 4, ".exr") == 0 ||
	              filepath.compare(filepath.size() - 4, 4, ".EXR") == 0);

	int w = 0, h = 0;
	std::vector<float> pixels;

	if (isEXR)
	{
		float* outRGBA = nullptr;
		const char* err = nullptr;
		int ret = LoadEXR(&outRGBA, &w, &h, filepath.c_str(), &err);
		if (ret != TINYEXR_SUCCESS || !outRGBA)
		{
			printf("[EnvMap] Failed to load EXR: %s\n", err ? err : "unknown");
			if (err) free((void*)err);
			return false;
		}
		pixels.assign(outRGBA, outRGBA + (size_t)w * h * 4);
		free(outRGBA);
		if (err) free((void*)err);
		printf("[EnvMap] Loaded %dx%d EXR\n", w, h);
	}
	else
	{
		int channels;
		float* data = stbi_loadf(filepath.c_str(), &w, &h, &channels, 4);
		if (!data)
		{
			printf("[EnvMap] Failed to load HDR file!\n");
			return false;
		}
		pixels.assign(data, data + (size_t)w * h * 4);
		stbi_image_free(data);
		printf("[EnvMap] Loaded %dx%d HDR\n", w, h);
	}

	m_EnvMapPath = filepath;
	m_EnvMapWidth = w;
	m_EnvMapHeight = h;
	m_EnvMapFloatPixels = std::move(pixels);
	return true;
}

void SceneManager::ClearEnvMap()
{
	m_EnvMapPath.clear();
	m_EnvMapFloatPixels.clear();
	m_EnvMapWidth = 0;
	m_EnvMapHeight = 0;
}

SceneManager::EntityId SceneManager::ImportGltf(const std::string& filepath)
{
	entt::entity root = SceneLoader::ImportIntoECS(m_EcsScene, filepath);
	if (root == entt::null)
		return EntityId{};

	m_EntityCacheDirty = true;
	return EntityId{ root };
}

// ============================================================================
// GPU sync — build GPUSceneData from current scene state and push to renderer
// ============================================================================

void SceneManager::SyncToGPU()
{
	printf("[Scene] SyncToGPU: building GPU scene data...\n");
	fflush(stdout);

	GPUSceneData gpuData;

	UpdateWorldTransforms();
	gpuData = BuildGPUSceneDataFromECS(m_EcsScene);

	printf("[Scene] SyncToGPU: GPUSceneData built: meshes=%zu instances=%zu lights=%zu textures=%zu source_emissive=%u filtered_black=%u\n",
	       gpuData.meshes.size(), gpuData.instances.size(), gpuData.lights.size(), gpuData.textures.size(),
	       gpuData.sourceEmissiveTriangleCount, gpuData.filteredBlackEmissiveTriangleCount);
	fflush(stdout);

	// Add env map as an extra texture in the texture array
	if (HasEnvMap())
	{
		SceneTexture envTex;
		envTex.isHDR = true;
		envTex.width = m_EnvMapWidth;
		envTex.height = m_EnvMapHeight;
		envTex.floatPixels = m_EnvMapFloatPixels;
		gpuData.textures.push_back(envTex);
		gpuData.envMapIndex = (int)gpuData.textures.size() - 1;

		BuildEnvMapCDF(m_EnvMapFloatPixels, m_EnvMapWidth, m_EnvMapHeight,
		               gpuData.marginalCDF, gpuData.conditionalCDF);
		gpuData.cdfWidth = m_EnvMapWidth;
		gpuData.cdfHeight = m_EnvMapHeight;

		printf("[Scene] Env map: idx=%d %dx%d, CDF built\n",
		       gpuData.envMapIndex, m_EnvMapWidth, m_EnvMapHeight);
	}

	if (m_SyncCallback)
	{
		printf("[Scene] SyncToGPU: calling sync callback (SetScene)...\n");
		fflush(stdout);
		m_SyncCallback(gpuData);
		printf("[Scene] SyncToGPU: sync callback done\n");
		fflush(stdout);
	}

	m_CurrentGpuScene = std::move(gpuData);
}

void SceneManager::SyncToGPUKeepTextures()
{
	GPUSceneData gpuData;

	UpdateWorldTransforms();
	gpuData = BuildGPUSceneDataFromECS(m_EcsScene);

	// Preserve env map data from current GPU scene (textures aren't re-uploaded)
	if (m_CurrentGpuScene.envMapIndex >= 0)
	{
		gpuData.envMapIndex = m_CurrentGpuScene.envMapIndex;
		gpuData.envIntensity = m_CurrentGpuScene.envIntensity;
		gpuData.marginalCDF = m_CurrentGpuScene.marginalCDF;
		gpuData.conditionalCDF = m_CurrentGpuScene.conditionalCDF;
		gpuData.cdfWidth = m_CurrentGpuScene.cdfWidth;
		gpuData.cdfHeight = m_CurrentGpuScene.cdfHeight;
	}

	m_CurrentGpuScene = gpuData;
	if (m_SyncKeepTexturesCallback)
		m_SyncKeepTexturesCallback(gpuData);
}

// ============================================================================
// Entity manipulation
// ============================================================================

SceneManager::EntityId SceneManager::AddObject(const std::string& name,
                                                const glm::vec3& position,
                                                const glm::vec3& rotation,
                                                float scale,
                                                int materialIndex)
{
	auto entity = m_EcsScene.registry.create();

	Transform tf;
	tf.translation = position;
	tf.rotation = glm::quat(glm::radians(rotation));
	tf.scale = {scale, scale, scale};
	m_EcsScene.registry.emplace<Transform>(entity, tf);
	m_EcsScene.registry.emplace<MeshRef>(entity, 0u, materialIndex);

	if (!name.empty())
		m_EcsScene.registry.emplace<NameComponent>(entity, name);
	m_EcsScene.registry.emplace<VisibleComponent>(entity);

	m_EntityCacheDirty = true;
	return {entity};
}

SceneManager::EntityId SceneManager::AddObjectWithGeometry(const std::string& name,
                                                           MeshData&& meshData,
                                                           const glm::vec3& position,
                                                           const glm::vec3& rotation,
                                                           float scale,
                                                           int materialIndex)
{
	uint32_t meshIdx = m_EcsScene.meshRegistry.AddMesh(std::move(meshData));

	auto entity = m_EcsScene.registry.create();

	Transform tf;
	tf.translation = position;
	tf.rotation = glm::quat(glm::radians(rotation));
	tf.scale = {scale, scale, scale};
	m_EcsScene.registry.emplace<Transform>(entity, tf);
	m_EcsScene.registry.emplace<MeshRef>(entity, meshIdx, materialIndex);

	if (!name.empty())
		m_EcsScene.registry.emplace<NameComponent>(entity, name);
	m_EcsScene.registry.emplace<VisibleComponent>(entity);

	m_EntityCacheDirty = true;
	return {entity};
}

SceneManager::EntityId SceneManager::AddLight(const std::string& name,
                                              const glm::vec3& position,
                                              const glm::vec3& color,
                                              float intensity,
                                              bool isSpot)
{
	auto entity = m_EcsScene.registry.create();

	Transform tf;
	tf.translation = position;
	m_EcsScene.registry.emplace<Transform>(entity, tf);

	LightComponent light;
	light.color = color;
	light.intensity = intensity;
	light.isSpot = isSpot;
	m_EcsScene.registry.emplace<LightComponent>(entity, light);

	if (!name.empty())
		m_EcsScene.registry.emplace<NameComponent>(entity, name);
	m_EcsScene.registry.emplace<VisibleComponent>(entity);

	m_EntityCacheDirty = true;
	return {entity};
}

void SceneManager::RemoveEntity(EntityId entity)
{
	if (!entity.IsValid()) return;

	auto& reg = m_EcsScene.registry;
	if (!reg.valid(entity.id)) return;

	// Remove from parent's children list if has Hierarchy
	if (auto* h = reg.try_get<Hierarchy>(entity.id))
	{
		if (h->parent != entt::null)
		{
			if (auto* parentH = reg.try_get<Hierarchy>(h->parent))
			{
				auto& children = parentH->children;
				children.erase(std::remove(children.begin(), children.end(), entity.id), children.end());
			}
		}
	}

	// Recursively destroy children
	if (auto* h = reg.try_get<Hierarchy>(entity.id))
	{
		auto children = h->children; // copy — registry modified during destroy
		for (auto child : children)
			RemoveEntity({child});
	}

	reg.destroy(entity.id);
	m_EntityCacheDirty = true;
}

void SceneManager::SetTransform(EntityId entity,
                                const glm::vec3& position,
                                const glm::vec3& rotation,
                                float scale)
{
	if (!entity.IsValid()) return;
	auto& reg = m_EcsScene.registry;
	if (auto* tf = reg.try_get<Transform>(entity.id))
	{
		tf->translation = position;
		tf->rotation = glm::quat(glm::radians(rotation));
		tf->scale = {scale, scale, scale};
		SceneGraph::SetLocalDirty(reg, entity.id);
	}
}

void SceneManager::SetMaterial(EntityId entity, int materialIndex)
{
	if (!entity.IsValid()) return;
	auto& reg = m_EcsScene.registry;
	if (auto* ref = reg.try_get<MeshRef>(entity.id))
		ref->materialIndex = materialIndex;
}

std::string SceneManager::GetEntityName(EntityId entity) const
{
	if (!entity.IsValid()) return "";
	if (!m_EcsScene.registry.valid(entity.id)) return "";
	auto& reg = m_EcsScene.registry;
	if (auto* name = reg.try_get<NameComponent>(entity.id))
		return name->name;
	return "";
}

void SceneManager::SetEntityName(EntityId entity, const std::string& name)
{
	if (!entity.IsValid()) return;
	auto& reg = m_EcsScene.registry;
	if (auto* nc = reg.try_get<NameComponent>(entity.id))
		nc->name = name;
	else
		reg.emplace<NameComponent>(entity.id, name);
}

size_t SceneManager::GetEntityCount() const
{
	// Use a component view (NameComponent or Transform) as a proxy.
	// Most entities have at least one of these. For an exact count we'd
	// need registry.storage<entt::entity>().size(), but that's not
	// public in this entt version. Use Transform as the common component.
	return m_EcsScene.registry.view<Transform>().size();
}

SceneManager::EntityId SceneManager::GetEntityByIndex(size_t index) const
{
	if (m_EntityCacheDirty)
	{
		m_EntityCache.clear();
		auto view = m_EcsScene.registry.view<Transform>();
		for (auto entity : view)
			m_EntityCache.push_back(entity);
		m_EntityCacheDirty = false;
	}
	if (index >= m_EntityCache.size()) return {entt::null};
	return {m_EntityCache[index]};
}

std::vector<SceneManager::EntityId> SceneManager::GetRootEntities() const
{
	std::vector<EntityId> roots;
	auto view = m_EcsScene.registry.view<Transform>();
	for (auto entity : view)
	{
		auto* h = m_EcsScene.registry.try_get<Hierarchy>(entity);
		if (!h || h->parent == entt::null)
			roots.push_back({entity});
	}
	return roots;
}

// ============================================================================
// Entity queries (for inspector UI)
// ============================================================================

bool SceneManager::HasMeshRef(EntityId entity) const
{
	if (!entity.IsValid()) return false;
	if (!m_EcsScene.registry.valid(entity.id)) return false;
	return m_EcsScene.registry.try_get<MeshRef>(entity.id) != nullptr;
}

bool SceneManager::HasLight(EntityId entity) const
{
	if (!entity.IsValid()) return false;
	if (!m_EcsScene.registry.valid(entity.id)) return false;
	return m_EcsScene.registry.try_get<LightComponent>(entity.id) != nullptr;
}

bool SceneManager::HasTransform(EntityId entity) const
{
	if (!entity.IsValid()) return false;
	if (!m_EcsScene.registry.valid(entity.id)) return false;
	return m_EcsScene.registry.try_get<Transform>(entity.id) != nullptr;
}

bool SceneManager::HasCamera(EntityId entity) const
{
	if (!entity.IsValid()) return false;
	if (!m_EcsScene.registry.valid(entity.id)) return false;
	return m_EcsScene.registry.try_get<CameraComponent>(entity.id) != nullptr;
}

bool SceneManager::IsEntityAlive(EntityId entity) const
{
	if (!entity.IsValid()) return false;
	return m_EcsScene.registry.valid(entity.id);
}

bool SceneManager::GetTransform(EntityId entity, glm::vec3& outPosition, glm::vec3& outRotationEuler, float& outScale) const
{
	if (!entity.IsValid()) return false;
	if (!m_EcsScene.registry.valid(entity.id)) return false;
	auto* tf = m_EcsScene.registry.try_get<Transform>(entity.id);
	if (!tf) return false;
	outPosition = tf->translation;
	outRotationEuler = glm::degrees(glm::eulerAngles(tf->rotation));
	outScale = tf->scale.x;
	return true;
}

bool SceneManager::GetLightProperties(EntityId entity, glm::vec3& outColor, float& outIntensity, bool& outIsSpot) const
{
	if (!entity.IsValid()) return false;
	if (!m_EcsScene.registry.valid(entity.id)) return false;
	auto* light = m_EcsScene.registry.try_get<LightComponent>(entity.id);
	if (!light) return false;
	outColor = light->color;
	outIntensity = light->intensity;
	outIsSpot = light->isSpot;
	return true;
}

void SceneManager::SetLightProperties(EntityId entity, const glm::vec3& color, float intensity, bool isSpot)
{
	if (!entity.IsValid()) return;
	auto* light = m_EcsScene.registry.try_get<LightComponent>(entity.id);
	if (!light) return;
	light->color = color;
	light->intensity = intensity;
	light->isSpot = isSpot;
}

bool SceneManager::GetMeshRef(EntityId entity, uint32_t& outMeshIndex, int& outMaterialIndex) const
{
	if (!entity.IsValid()) return false;
	if (!m_EcsScene.registry.valid(entity.id)) return false;
	auto* ref = m_EcsScene.registry.try_get<MeshRef>(entity.id);
	if (!ref) return false;
	outMeshIndex = ref->meshIndex;
	outMaterialIndex = ref->materialIndex;
	return true;
}

void SceneManager::SetMeshRefMeshIndex(EntityId entity, uint32_t meshIndex)
{
	if (!entity.IsValid()) return;
	auto* ref = m_EcsScene.registry.try_get<MeshRef>(entity.id);
	if (!ref) return;
	ref->meshIndex = meshIndex;
}

void SceneManager::SyncTransformsToGPU()
{
	if (m_EcsScene.meshRegistry.GetCount() == 0) return;

	UpdateWorldTransforms();

	GPUSceneData gpuData = m_CurrentGpuScene;
	UpdateInstancesFromECS(gpuData, m_EcsScene);

	if (m_InstanceSyncCallback)
		m_InstanceSyncCallback(gpuData);

	m_CurrentGpuScene = gpuData;
}

// ============================================================================
// Hierarchy queries (for tree view outliner)
// ============================================================================

bool SceneManager::HasChildren(EntityId entity) const
{
	if (!entity.IsValid()) return false;
	if (!m_EcsScene.registry.valid(entity.id)) return false;
	auto* h = m_EcsScene.registry.try_get<Hierarchy>(entity.id);
	return h && !h->children.empty();
}

std::vector<SceneManager::EntityId> SceneManager::GetChildren(EntityId entity) const
{
	std::vector<EntityId> result;
	if (!entity.IsValid()) return result;
	if (!m_EcsScene.registry.valid(entity.id)) return result;
	auto* h = m_EcsScene.registry.try_get<Hierarchy>(entity.id);
	if (!h) return result;
	result.reserve(h->children.size());
	for (auto child : h->children)
	{
		if (m_EcsScene.registry.valid(child))
			result.push_back({child});
	}
	return result;
}

SceneManager::EntityId SceneManager::GetParent(EntityId entity) const
{
	if (!entity.IsValid()) return {entt::null};
	if (!m_EcsScene.registry.valid(entity.id)) return {entt::null};
	auto* h = m_EcsScene.registry.try_get<Hierarchy>(entity.id);
	if (!h || h->parent == entt::null) return {entt::null};
	if (!m_EcsScene.registry.valid(h->parent)) return {entt::null};
	return {h->parent};
}

// ============================================================================
// Material + texture management
// ============================================================================

int SceneManager::AddMaterial(const SceneMaterial& material)
{
	int idx = (int)m_EcsScene.materials.size();
	m_EcsScene.materials.push_back(material);
	return idx;
}

SceneMaterial& SceneManager::GetMaterial(int index)
{
	if (index >= 0 && index < (int)m_EcsScene.materials.size())
		return m_EcsScene.materials[index];
	static SceneMaterial dummy;
	return dummy;
}

// ============================================================================
// Stats + misc
// ============================================================================

void SceneManager::Clear()
{
	m_EcsScene.Clear();
	m_CurrentGpuScene = GPUSceneData{};
	ClearEnvMap();
	m_EntityCacheDirty = true;
}

bool SceneManager::CompactMeshRegistry()
{
	auto& reg = m_EcsScene.registry;
	auto& meshReg = m_EcsScene.meshRegistry;

	// Find which mesh indices are still referenced by alive entities
	std::set<uint32_t> referenced;
	auto view = reg.view<MeshRef>();
	for (auto entity : view)
	{
		if (!reg.valid(entity)) continue;
		const auto& ref = view.get<MeshRef>(entity);
		referenced.insert(ref.meshIndex);
	}

	bool meshesChanged = false;

	// If all meshes are referenced, nothing to do
	if (referenced.size() == meshReg.GetCount())
	{
		// Meshes are fine, but still may need to compact materials/textures
	}
	// If no meshes referenced, clear the registry entirely
	else if (referenced.empty())
	{
		meshReg.Clear();
		meshesChanged = true;
	}
	else
	{
		// Build remap: old index -> new index
		std::map<uint32_t, uint32_t> remap;
		uint32_t newIndex = 0;
		for (uint32_t old : referenced)
			remap[old] = newIndex++;

		// Rebuild the mesh registry with only referenced meshes
		std::vector<MeshData> newMeshes;
		newMeshes.reserve(referenced.size());
		for (uint32_t old : referenced)
			newMeshes.push_back(std::move(meshReg.GetMesh(old)));

		meshReg.Clear();
		for (auto& mesh : newMeshes)
			meshReg.AddMesh(std::move(mesh));

		// Remap all MeshRef components
		for (auto entity : view)
		{
			if (!reg.valid(entity)) continue;
			auto& ref = view.get<MeshRef>(entity);
			auto it = remap.find(ref.meshIndex);
			if (it != remap.end())
				ref.meshIndex = it->second;
		}

		printf("[Scene] Compacted mesh registry: %d -> %d meshes\n",
		       (int)meshReg.GetCount(), (int)referenced.size());
		meshesChanged = true;
	}

	// ---- Compact materials ----
	// Collect all referenced material indices from MeshRef components
	// and per-triangle materialIndices in meshes.
	std::set<int> referencedMats;
	for (auto entity : view)
	{
		if (!reg.valid(entity)) continue;
		const auto& ref = view.get<MeshRef>(entity);
		referencedMats.insert(ref.materialIndex);
	}
	for (uint32_t m = 0; m < meshReg.GetCount(); m++)
	{
		const auto& mesh = meshReg.GetMesh(m);
		for (int idx : mesh.materialIndices)
			referencedMats.insert(idx);
	}

	// Always include a default material at index 0 if there are meshes
	// but no materials (safety net).
	if (meshReg.GetCount() > 0 && referencedMats.empty())
		referencedMats.insert(0);

	// Build material remap: old index -> new index
	std::map<int, int> matRemap;
	int newMatIdx = 0;
	for (int old : referencedMats)
	{
		if (old >= 0 && old < (int)m_EcsScene.materials.size())
			matRemap[old] = newMatIdx++;
	}

	bool matsChanged = (matRemap.size() < m_EcsScene.materials.size());

	if (matsChanged)
	{
		// Rebuild materials vector
		std::vector<SceneMaterial> newMaterials;
		newMaterials.reserve(matRemap.size());
		for (int old : referencedMats)
		{
			if (old >= 0 && old < (int)m_EcsScene.materials.size())
				newMaterials.push_back(m_EcsScene.materials[old]);
		}
		m_EcsScene.materials = std::move(newMaterials);

		// Remap MeshRef.materialIndex
		for (auto entity : view)
		{
			if (!reg.valid(entity)) continue;
			auto& ref = view.get<MeshRef>(entity);
			auto it = matRemap.find(ref.materialIndex);
			if (it != matRemap.end())
				ref.materialIndex = it->second;
		}

		// Remap per-triangle materialIndices in meshes
		for (uint32_t m = 0; m < meshReg.GetCount(); m++)
		{
			auto& mesh = meshReg.GetMesh(m);
			for (auto& idx : mesh.materialIndices)
			{
				auto it = matRemap.find(idx);
				if (it != matRemap.end())
					idx = it->second;
			}
		}

		printf("[Scene] Compacted materials: %zu -> %zu\n",
		       m_EcsScene.materials.size() + matRemap.size(), matRemap.size());
	}

	// ---- Compact textures ----
	// Collect all referenced texture indices from remaining materials.
	std::set<int> referencedTexs;
	for (const auto& mat : m_EcsScene.materials)
	{
		if (mat.baseColorTextureIndex >= 0)        referencedTexs.insert(mat.baseColorTextureIndex);
		if (mat.normalTextureIndex >= 0)          referencedTexs.insert(mat.normalTextureIndex);
		if (mat.emissiveTextureIndex >= 0)         referencedTexs.insert(mat.emissiveTextureIndex);
		if (mat.metallicRoughnessTextureIndex >= 0) referencedTexs.insert(mat.metallicRoughnessTextureIndex);
	}

	// Build texture remap: old index -> new index
	std::map<int, int> texRemap;
	int newTexIdx = 0;
	for (int old : referencedTexs)
	{
		if (old >= 0 && old < (int)m_EcsScene.textures.size())
			texRemap[old] = newTexIdx++;
	}

	bool texsChanged = (texRemap.size() < m_EcsScene.textures.size());

	if (texsChanged)
	{
		// Rebuild textures vector
		std::vector<SceneTexture> newTextures;
		newTextures.reserve(texRemap.size());
		for (int old : referencedTexs)
		{
			if (old >= 0 && old < (int)m_EcsScene.textures.size())
				newTextures.push_back(std::move(m_EcsScene.textures[old]));
		}
		m_EcsScene.textures = std::move(newTextures);

		// Remap texture indices in materials
		auto remapTex = [&texRemap](int& idx) {
			if (idx >= 0)
			{
				auto it = texRemap.find(idx);
				if (it != texRemap.end())
					idx = it->second;
				else
					idx = -1; // orphaned texture reference
			}
		};
		for (auto& mat : m_EcsScene.materials)
		{
			remapTex(mat.baseColorTextureIndex);
			remapTex(mat.normalTextureIndex);
			remapTex(mat.emissiveTextureIndex);
			remapTex(mat.metallicRoughnessTextureIndex);
		}

		printf("[Scene] Compacted textures: %zu -> %zu\n",
		       texRemap.size() + (m_EcsScene.textures.size() - texRemap.size()),
		       m_EcsScene.textures.size());
	}

	return meshesChanged || matsChanged || texsChanged;
}

// ============================================================================
// Internal
// ============================================================================

void SceneManager::UpdateWorldTransforms()
{
	SceneGraph::UpdateWorldTransforms(m_EcsScene.registry);
}
