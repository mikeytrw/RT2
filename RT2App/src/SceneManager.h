#pragma once

#ifndef SCENE_MANAGER_H
#define SCENE_MANAGER_H

#include "Scene.h"
#include "ECSScene.h"
#include "GPUSceneData.h"
#include "Mesh.h"
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

// ============================================================================
// SceneManager — owns all scene state + provides entity manipulation APIs.
//
// Owns:
//   - Legacy Scene (flat mesh array — used by CPU renderer + OBJ fallback)
//   - ECSScene (entity-component model — used by GPU renderer)
//   - GPU scene data (last uploaded to RendererGPU)
//   - Environment map (HDR/EXR float pixels + dimensions)
//   - CPU-side meshes (for the CPU ray tracer)
//
// Entity manipulation APIs (for P4.2 + future scene outliner):
//   - AddObject: register mesh geometry + create entity with transform
//   - AddLight: attach LightComponent to an entity
//   - RemoveEntity: destroy entity + its hierarchy
//   - SetTransform: update local TRS (marks dirty for SceneGraph)
//   - SetMaterial: change material on an entity's MeshRef
//   - GetEntityName / SetEntityName
//
// The manager calls a sync callback (provided by the app) to push the
// rebuilt GPUSceneData to RendererGPU::SetScene() or UpdateSceneInstances().
// This keeps SceneManager decoupled from the renderer.
//
// ============================================================================
class SceneManager
{
public:
	using SyncCallback = std::function<void(const GPUSceneData&)>;

	SceneManager() = default;
	~SceneManager() = default;

	// ---- Sync callback ----
	// Called after scene changes that require a full GPU re-upload
	// (SetScene path: textures, meshes, AS rebuild).
	void SetSyncCallback(SyncCallback cb) { m_SyncCallback = std::move(cb); }

	// ---- Scene loading ----
	bool LoadScene(const std::string& filepath);
	bool LoadEnvMap(const std::string& filepath);
	void ClearEnvMap();

	// ---- Full GPU re-upload (rebuilds GPUSceneData from scene state) ----
	void SyncToGPU();

	// ---- Entity manipulation ----
	// EntityId is a thin wrapper around entt::entity for type safety.
	struct EntityId
	{
		entt::entity id = entt::null;
		bool IsValid() const { return id != entt::null; }
	};

	// Add a mesh object to the scene. Returns the new entity.
	// If meshData is empty and filepath is set, the mesh will be loaded
	// from file on the next SyncToGPU() (legacy OBJ path).
	EntityId AddObject(const std::string& name,
	                   const glm::vec3& position = {0, 0, 0},
	                   const glm::vec3& rotation = {0, 0, 0},
	                   float scale = 1.0f,
	                   int materialIndex = 0);

	// Add a mesh with explicit geometry data (inline mesh — no file).
	EntityId AddObjectWithGeometry(const std::string& name,
	                               MeshData&& meshData,
	                               const glm::vec3& position = {0, 0, 0},
	                               const glm::vec3& rotation = {0, 0, 0},
	                               float scale = 1.0f,
	                               int materialIndex = 0);

	// Add a light entity with the given properties.
	EntityId AddLight(const std::string& name,
	                  const glm::vec3& position = {0, 0, 0},
	                  const glm::vec3& color = {1, 1, 1},
	                  float intensity = 1.0f,
	                  bool isSpot = false);

	// Remove an entity and its children. Safe to call with invalid EntityId.
	void RemoveEntity(EntityId entity);

	// Update an entity's local transform (marks it dirty for SceneGraph).
	void SetTransform(EntityId entity,
	                  const glm::vec3& position,
	                  const glm::vec3& rotation = {0, 0, 0},
	                  float scale = 1.0f);

	// Update an entity's material index (which material from the materials array).
	void SetMaterial(EntityId entity, int materialIndex);

	// Get/set entity name (for UI outliner).
	std::string GetEntityName(EntityId entity) const;
	void SetEntityName(EntityId entity, const std::string& name);

	// Get entity count (for UI iteration).
	size_t GetEntityCount() const;
	EntityId GetEntityByIndex(size_t index) const;

	// ---- Material + texture management ----
	int AddMaterial(const SceneMaterial& material);
	SceneMaterial& GetMaterial(int index);
	const std::vector<SceneMaterial>& GetMaterials() const { return m_EcsScene.materials; }

	// ---- Accessors ----
	const Scene& GetScene() const { return m_Scene; }
	Scene& GetScene() { return m_Scene; }
	const ECSScene& GetECS() const { return m_EcsScene; }
	ECSScene& GetECS() { return m_EcsScene; }
	const GPUSceneData& GetCurrentGpuScene() const { return m_CurrentGpuScene; }

	// Environment map
	bool HasEnvMap() const { return !m_EnvMapFloatPixels.empty(); }
	const std::string& GetEnvMapPath() const { return m_EnvMapPath; }
	int GetEnvMapWidth() const { return m_EnvMapWidth; }
	int GetEnvMapHeight() const { return m_EnvMapHeight; }

	// CPU-side mesh count (for CPU renderer stats)
	uint32_t GetTriangleCount() const;
	uint32_t GetBVHNodeCount() const;
	int GetBVHMaxDepth() const;

	// Clear all scene state.
	void Clear();

private:
	void UpdateWorldTransforms();

	Scene              m_Scene;
	ECSScene           m_EcsScene;
	GPUSceneData       m_CurrentGpuScene;

	// Environment map
	std::string        m_EnvMapPath;
	std::vector<float> m_EnvMapFloatPixels;
	int                m_EnvMapWidth = 0;
	int                m_EnvMapHeight = 0;

	// CPU-side meshes (for CPU ray tracer)
	std::vector<Mesh>  m_CpuMeshes;

	SyncCallback       m_SyncCallback;

	// Cache of entity list (for GetEntityByIndex — rebuilt on demand)
	mutable std::vector<entt::entity> m_EntityCache;
	mutable bool m_EntityCacheDirty = true;
};

#endif // SCENE_MANAGER_H