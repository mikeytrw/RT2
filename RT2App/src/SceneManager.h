#pragma once

#ifndef SCENE_MANAGER_H
#define SCENE_MANAGER_H

#include "SceneTypes.h"
#include "ECSScene.h"
#include "GPUSceneData.h"
#include "SceneDocument.h"
#include "core/UUID.h"
#include "core/Error.h"
#include "TransformEditing.h"
#include "SceneMutation.h"
#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include <utility>
#include <optional>

struct EditorCameraPose;

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
		using SyncCallback = std::function<void(GPUSceneData&, const RenderInstanceMap&)>;

		SceneManager();
		~SceneManager() = default;

		// Inject a UUID provider for entity creation. Default is OsUuidProvider.
		// Tests and deterministic fixtures may inject DeterministicUuidProvider.
		void SetUuidProvider(rt2::core::IUuidProvider* provider);

		// Find an entity by its stable UUID. Returns entt::null if not found.
	entt::entity FindEntityByUuid(const rt2::core::UUID& uuid) const;

	// The authoring scene document (ECS + UUID index + metadata).
	rt2::core::SceneDocument& AuthoringDoc() { return m_Authoring; }
	const rt2::core::SceneDocument& AuthoringDoc() const { return m_Authoring; }

	// Atomically adopt a fully constructed authoring document. This is the
	// only supported path for transactional open/recovery commits: the
	// caller prepares and validates a temporary document, then transfers it
	// here after success. Reference aliases remain valid because m_Authoring
	// itself is assigned in place. Entity caches and the transient authoring
	// revision are reset deliberately without clearing the adopted data.
	void ReplaceAuthoringDocument(rt2::core::SceneDocument&& document,
	                              uint64_t authoringRevision = 0);

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
	rt2::core::UUID GetEntityUuid(EntityId entity) const;

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

	// UUID-keyed atomic authoring operations. Each operation performs all
	// validation before mutation, bumps the authoring revision at most once,
	// and reports the single renderer sync class required by the caller.
	EditorMutationResult CreateEmpty(
		const std::string& name = "Empty",
		const std::optional<rt2::core::UUID>& parent = std::nullopt);
	EditorMutationResult Reparent(
		const std::vector<rt2::core::UUID>& entities,
		const std::optional<rt2::core::UUID>& newParent,
		ReparentMode mode = ReparentMode::PreserveWorld);
	EditorMutationResult RemoveSubtrees(
		const std::vector<rt2::core::UUID>& roots);
	EditorMutationResult SetVisibility(
		const std::vector<rt2::core::UUID>& entities, bool visible);
	// Atomic multi-entity visibility with per-entity target states. Validates
	// ALL UUIDs first (any failure => zero mutation), deduplicates
	// (last-write-wins), skips entities already in the target state, applies
	// all, bumps the revision once, and returns one result (Structural if
	// anything changed, empty-success None otherwise). Mirrors SetVisibility.
	EditorMutationResult SetVisibilityStates(
		const std::vector<std::pair<rt2::core::UUID, bool>>& states);
	EditorMutationResult DuplicateSubtrees(
		const std::vector<rt2::core::UUID>& roots);
	EditorMutationResult PasteSubtreesFrom(
		const rt2::core::SceneDocument& snapshot,
		const std::vector<rt2::core::UUID>& roots,
		const std::optional<rt2::core::UUID>& parent = std::nullopt);

	// Update an entity's local transform (marks it dirty for SceneGraph).
	void SetTransform(EntityId entity,
	                  const glm::vec3& position,
	                  const glm::vec3& rotation = {0, 0, 0},
	                  float scale = 1.0f);
	void SetTransform(EntityId entity,
	                  const glm::vec3& position,
	                  const glm::vec3& rotation,
	                  const glm::vec3& scale);
	void SetLocalTransform(EntityId entity, const EditableTRS& transform);
	bool TrySetWorldTransform(EntityId entity, const glm::mat4& desiredWorld);
	bool TrySetWorldTransforms(
		const std::vector<std::pair<EntityId, glm::mat4>>& desiredWorldTransforms);
	EditorMutationResult AlignCameraEntityToView(
		const rt2::core::UUID& cameraEntity, const EditorCameraPose& pose);

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
	bool GetTransform(EntityId entity, glm::vec3& outPosition,
	                  glm::vec3& outRotationEuler, glm::vec3& outScale) const;
	bool GetLocalTransform(EntityId entity, EditableTRS& outTransform) const;
	bool GetWorldTransform(EntityId entity, EditableTRS& outTransform);

	// Read/write light properties.
	bool GetLightProperties(EntityId entity, glm::vec3& outColor, float& outIntensity, bool& outIsSpot) const;
	void SetLightProperties(EntityId entity, const glm::vec3& color, float intensity, bool isSpot);
	bool SetCameraProperties(EntityId entity, float verticalFOV,
		float aperture, float focusDistance);

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

	// Set material properties on an index and mark the scene dirty + flag
	// a material-sync GPU re-upload. Use this instead of mutating the
	// reference returned by GetMaterial() so dirty tracking and the
	// correct sync path are invoked.
	void SetMaterialProperties(int index, const SceneMaterial& props);

	// ---- Dirty tracking ----
	bool IsDirty() const { return m_Authoring.metadata.dirty; }
	void MarkDirty()
	{
		m_Authoring.metadata.dirty = true;
		++m_AuthoringRevision;
	}
	void ClearDirty() { m_Authoring.metadata.dirty = false; }

	// Authoring revision counter. Bumped on every authoring mutation via
	// NotifyAuthoringChanged(). Used by the recovery/autosave service to
	// skip rewriting an identical snapshot. Not serialized into .rt2scene.
	uint64_t AuthoringRevision() const { return m_AuthoringRevision; }
	uint64_t DocumentGeneration() const { return m_DocumentGeneration; }
	uint64_t ResourceGeneration() const { return m_ResourceGeneration; }

	// Centralized authoring-change notification. All editor mutations
	// (Add/Remove/SetTransform/SetMaterial/SetMaterialProperties) call
	// this. It marks the scene dirty and bumps the revision counter. The
	// host checks IsDirty() for unsaved-changes prompts.
	void NotifyAuthoringChanged();

	// ---- Accessors ----
	const ECSScene& GetECS() const { return m_EcsScene; }
	ECSScene& GetECS() { return m_EcsScene; }
	const GPUSceneData& GetCurrentGpuScene() const { return m_CurrentGpuScene; }

	// Environment map (delegates to authoring document)
	bool HasEnvMap() const { return m_Authoring.environment.HasEnvMap(); }
	const std::string& GetEnvMapPath() const { return m_Authoring.environment.path; }
	int GetEnvMapWidth() const { return m_Authoring.environment.width; }
	int GetEnvMapHeight() const { return m_Authoring.environment.height; }

	// Clear all scene state.
	void Clear();

	// Remove unreferenced meshes/materials/textures and remap all
	// references to the compacted indices. Call after entity
	// deletion to prevent deleted resources from lingering in GPU scene.
	// Returns true if any compaction occurred (requires full re-sync).
	bool CompactMeshRegistry();

private:
	void UpdateWorldTransforms();
	void RefreshCameraForwardDirections(const std::vector<entt::entity>& roots);
	void ReconcileStoredCameraDirections();

	// Record a durable MaterialOverrideComponent on an imported entity for the
	// material currently at `materialIndex`. Captures the material value
	// snapshot so the override survives save/reopen regardless of how the
	// source asset re-imports. Creates or replaces the component.
	void RecordMaterialOverride(entt::entity entity, int materialIndex);

	// Authoring scene document. m_EcsScene below is a reference alias so
	// existing code continues to work; both refer to m_Authoring.ecs.
	rt2::core::SceneDocument m_Authoring;
	ECSScene&                m_EcsScene;       // = m_Authoring.ecs
	GPUSceneData&            m_CurrentGpuScene; // = m_Authoring.gpuCache

	// UUID provider for entity creation. Default is an internal OsUuidProvider.
	rt2::core::OsUuidProvider      m_DefaultProvider;
	rt2::core::IUuidProvider*      m_UuidProvider = &m_DefaultProvider;

	SyncCallback       m_SyncCallback;
	SyncCallback       m_InstanceSyncCallback;
	SyncCallback       m_SyncKeepTexturesCallback;
	RenderInstanceMap  m_RenderInstanceMap;

	// Cache of entity list (for GetEntityByIndex — rebuilt on demand)
	mutable std::vector<entt::entity> m_EntityCache;
	mutable bool m_EntityCacheDirty = true;

	// Authoring revision counter (not serialized). Bumped by
	// NotifyAuthoringChanged(). See AuthoringRevision().
	uint64_t m_AuthoringRevision = 0;
	uint64_t m_DocumentGeneration = 1;
	uint64_t m_ResourceGeneration = 1;
};

#endif // SCENE_MANAGER_H
