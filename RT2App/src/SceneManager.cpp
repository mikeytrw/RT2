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

// ============================================================================
// Scene loading
// ============================================================================

bool SceneManager::LoadScene(const std::string& filepath)
{
	printf("[Scene] LoadScene: '%s'\n", filepath.c_str());
	if (!SceneLoader::Load(m_Scene, filepath))
	{
		printf("[Scene] SceneLoader::Load failed!\n");
		return false;
	}
	printf("[Scene] SceneLoader::Load succeeded\n");

	if (!SceneLoader::LoadIntoECS(m_EcsScene, filepath))
	{
		printf("[Scene] SceneLoader::LoadIntoECS failed, GPU will use legacy path\n");
	}
	else
	{
		const auto& cam = m_Scene.GetCamera();
		printf("[Scene] Camera: pos=(%.1f,%.1f,%.1f), forward=(%.1f,%.1f,%.1f), fov=%.1f\n",
		       cam.position.x, cam.position.y, cam.position.z,
		       cam.forwardDirection.x, cam.forwardDirection.y, cam.forwardDirection.z,
		       cam.verticalFOV);
	}

	printf("[Scene] Loaded %d meshes, %d materials, %d lights, %d textures\n",
	       (int)m_Scene.GetMeshes().size(), (int)m_Scene.GetMaterials().size(),
	       (int)m_Scene.GetLights().size(), (int)m_Scene.GetTextures().size());

	// Rebuild CPU-side meshes for the CPU ray tracer
	m_CpuMeshes.clear();
	for (const auto& sceneMesh : m_Scene.GetMeshes())
	{
		Mesh mesh;
		bool meshLoaded = false;

		if (sceneMesh.HasGeometry())
		{
			auto material = std::make_shared<LambertianMaterial>(glm::vec3(0.7f));
			meshLoaded = mesh.LoadFromGeometry(sceneMesh.vertices, sceneMesh.normals,
			                                   sceneMesh.indices, sceneMesh.position,
			                                   sceneMesh.rotation, sceneMesh.scale, material);
		}
		else if (!sceneMesh.filepath.empty())
		{
			auto material = std::make_shared<LambertianMaterial>(glm::vec3(0.7f));
			meshLoaded = mesh.Load(sceneMesh.filepath, sceneMesh.position, sceneMesh.rotation,
			                       sceneMesh.scale, material);
		}

		if (meshLoaded)
			m_CpuMeshes.push_back(std::move(mesh));
	}

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

// ============================================================================
// GPU sync — build GPUSceneData from current scene state and push to renderer
// ============================================================================

void SceneManager::SyncToGPU()
{
	// Build GPUSceneData from ECS if available, otherwise from legacy Scene
	GPUSceneData gpuData;

	if (m_EcsScene.meshRegistry.GetCount() > 0)
	{
		UpdateWorldTransforms();
		gpuData = BuildGPUSceneDataFromECS(m_EcsScene);
	}
	else
	{
		gpuData = BuildGPUSceneData(m_Scene);
	}

	// If no scene meshes but we have CPU meshes, use the OBJ fallback
	if (gpuData.meshes.empty() && !m_CpuMeshes.empty())
	{
		for (auto& mesh : m_CpuMeshes)
		{
			auto [verts, indices] = mesh.GetRawVertexData();
			GPUMeshGeometry geo;
			geo.vertices = verts;
			geo.indices = indices;
			geo.materialIndex = 0;
			gpuData.meshes.push_back(std::move(geo));
		}
	}

	// Create identity instances if none exist
	if (gpuData.instances.empty() && !gpuData.meshes.empty())
	{
		for (uint32_t i = 0; i < gpuData.meshes.size(); i++)
		{
			GPUInstance inst;
			inst.meshIndex = i;
			inst.materialIndex = gpuData.meshes[i].materialIndex;
			inst.worldMatrix = glm::mat4(1.0f);
			inst.prevWorldMatrix = glm::mat4(1.0f);
			gpuData.instances.push_back(inst);
		}
	}

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

	m_CurrentGpuScene = gpuData;
	if (m_SyncCallback)
		m_SyncCallback(gpuData);
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

uint32_t SceneManager::GetTriangleCount() const
{
	uint32_t count = 0;
	for (const auto& mesh : m_CpuMeshes)
		count += static_cast<uint32_t>(mesh.GetTriangleCount());
	return count;
}

uint32_t SceneManager::GetBVHNodeCount() const
{
	uint32_t count = 0;
	for (const auto& mesh : m_CpuMeshes)
	{
		if (auto bvh = mesh.GetBvhNode())
		{
			uint32_t nodes = 0;
			int depth = 0;
			bvh->GetStats(nodes, depth);
			count += nodes;
		}
	}
	return count;
}

int SceneManager::GetBVHMaxDepth() const
{
	int maxDepth = 0;
	for (const auto& mesh : m_CpuMeshes)
	{
		if (auto bvh = mesh.GetBvhNode())
		{
			uint32_t nodes = 0;
			int depth = 0;
			bvh->GetStats(nodes, depth);
			maxDepth = std::max(maxDepth, depth);
		}
	}
	return maxDepth;
}

void SceneManager::Clear()
{
	m_Scene.Clear();
	m_EcsScene.Clear();
	m_CurrentGpuScene = GPUSceneData{};
	m_CpuMeshes.clear();
	ClearEnvMap();
	m_EntityCacheDirty = true;
}

// ============================================================================
// Internal
// ============================================================================

void SceneManager::UpdateWorldTransforms()
{
	SceneGraph::UpdateWorldTransforms(m_EcsScene.registry);
}