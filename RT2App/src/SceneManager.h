#pragma once

#ifndef SCENE_MANAGER_H
#define SCENE_MANAGER_H

#include "SceneTypes.h"
#include "ECSScene.h"
#include "GPUSceneData.h"
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

// ============================================================================
// SceneManager — owns all scene state + provides entity manipulation APIs.
//
// Owns:
//   - ECSScene (entity-component model — sole scene representation)
//   - GPU scene data (last uploaded to RendererGPU)
//   - Environment map (HDR/EXR float pixels + dimensions)
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
	using SyncCallback = std::function<void(GPUSceneData&)>;

	SceneManager() = default;
	~SceneManager() = default;

	// Set callback for full re-upload (SetScene path: textures + AS rebuild).
	void SetSyncCallback(SyncCallback cb) { m_SyncCallback = std::move(cb); }

	// Set callback for no-texture re-upload (SetSceneKeepTextures path).
	void SetSyncKeepTexturesCallback(SyncCallback cb) { m_SyncKeepTexturesCallback = std::move(cb); }

	// ---- Scene loading ----
	bool LoadScene(const std::string& filepath);
	bool LoadEnvMap(const std::string& filepath);
	void ClearEnvMap();

	// ---- Entity manipulation ----
	// EntityId is a thin wrapper around entt::entity for type safety.
	struct EntityId
	{
		entt::entity id = entt::null;
		bool IsValid() const { return id != entt::null; }
	};

	// Import a glTF file into the EXISTING scene (merges meshes/materials/
	// textures, creates a wrapper root entity). Does NOT clear the scene.
	// Returns the wrapper root entity, or invalid EntityId on failure.
	EntityId ImportGltf(const std::string& filepath);

	// ---- Full GPU re-upload (rebuilds GPUSceneData from scene state) ----
	void SyncToGPU();

	// ---- GPU re-upload without texture re-upload ----
	// Use when only entities/transforms/materials changed (add/delete entity,
	// material edit) but textures are unchanged. Much cheaper than SyncToGPU.
	void SyncToGPUKeepTextures();

	// Check if an entity is still alive in the registry.
	bool IsEntityAlive(EntityId entity) const;

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

	// Get root entities (no parent) for tree view outliner.
	std::vector<EntityId> GetRootEntities() const;

	// ---- Entity queries (for inspector UI) ----
	bool HasMeshRef(EntityId entity) const;
	bool HasLight(EntityId entity) const;
	bool HasCamera(EntityId entity) const;
	bool HasTransform(EntityId entity) const;

	// Read transform as euler degrees (for UI sliders). Returns false if no Transform.
	bool GetTransform(EntityId entity, glm::vec3& outPosition, glm::vec3& outRotationEuler, float& outScale) const;

	// Read/write light properties.
	bool GetLightProperties(EntityId entity, glm::vec3& outColor, float& outIntensity, bool& outIsSpot) const;
	void SetLightProperties(EntityId entity, const glm::vec3& color, float intensity, bool isSpot);

	// Read/write mesh ref (meshIndex + materialIndex).
	bool GetMeshRef(EntityId entity, uint32_t& outMeshIndex, int& outMaterialIndex) const;
	void SetMeshRefMeshIndex(EntityId entity, uint32_t meshIndex);

	// Material count (for combo dropdowns)
	size_t GetMaterialCount() const { return m_EcsScene.materials.size(); }

	// Mesh registry count (for info display)
	uint32_t GetMeshRegistryCount() const { return m_EcsScene.meshRegistry.GetCount(); }

	// ---- Hierarchy queries (for tree view outliner) ----
	bool HasChildren(EntityId entity) const;
	std::vector<EntityId> GetChildren(EntityId entity) const;
	EntityId GetParent(EntityId entity) const;

	// ---- Instance-only GPU sync ----
	// Rebuilds GPUSceneData instances + lights from ECS transforms, then
	// calls the instance sync callback (RendererGPU::UpdateSceneInstances).
	// Much cheaper than SyncToGPU() — no BLAS rebuild, no texture re-upload.
	void SetInstanceSyncCallback(SyncCallback cb) { m_InstanceSyncCallback = std::move(cb); }
	void SyncTransformsToGPU();

	// ---- Material + texture management ----
	int AddMaterial(const SceneMaterial& material);
	SceneMaterial& GetMaterial(int index);
	const std::vector<SceneMaterial>& GetMaterials() const { return m_EcsScene.materials; }

	// ---- Accessors ----
	const ECSScene& GetECS() const { return m_EcsScene; }
	ECSScene& GetECS() { return m_EcsScene; }
	const GPUSceneData& GetCurrentGpuScene() const { return m_CurrentGpuScene; }

	// Environment map
	bool HasEnvMap() const { return !m_EnvMapFloatPixels.empty(); }
	const std::string& GetEnvMapPath() const { return m_EnvMapPath; }
	int GetEnvMapWidth() const { return m_EnvMapWidth; }
	int GetEnvMapHeight() const { return m_EnvMapHeight; }

	// Clear all scene state.
	void Clear();

	// Remove unreferenced meshes/materials/textures and remap all
	// references to the compacted indices. Call after entity
	// deletion to prevent deleted resources from lingering in GPU scene.
	// Returns true if any compaction occurred (requires full re-sync).
	bool CompactMeshRegistry();

private:
	void UpdateWorldTransforms();

	ECSScene           m_EcsScene;
	GPUSceneData       m_CurrentGpuScene;

	// Environment map
	std::string        m_EnvMapPath;
	std::vector<float> m_EnvMapFloatPixels;
	int                m_EnvMapWidth = 0;
	int                m_EnvMapHeight = 0;

	SyncCallback       m_SyncCallback;
	SyncCallback       m_InstanceSyncCallback;
	SyncCallback       m_SyncKeepTexturesCallback;

	// Cache of entity list (for GetEntityByIndex — rebuilt on demand)
	mutable std::vector<entt::entity> m_EntityCache;
	mutable bool m_EntityCacheDirty = true;
};

#endif // SCENE_MANAGER_H