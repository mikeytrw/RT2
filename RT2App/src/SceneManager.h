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
#include "SubtreeSnapshot.h"
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
	// Load an environment map and assign/match its stable asset ID via the
	// per-asset sidecar (ResolveOrAssign). The load succeeds even when the
	// sidecar write fails — the scene gets a session ID and the next save
	// retries the sidecar — but `envImportErr` (if non-null) carries the
	// structured error so callers can surface it instead of relying on
	// console output (Phase 7 W3 step 4 remediation, item 4: silent failure
	// is this codebase's characteristic bug class).
	bool LoadEnvMap(const std::string& filepath,
	                rt2::core::Error* envImportErr = nullptr);
	void ClearEnvMap();

	// Set pre-decoded environment map data (used by the async load path
	// where the decode runs on a worker thread). Takes ownership of pixels.
	// Like LoadEnvMap, assigns/matches the sidecar ID and reports any
	// sidecar-write error through `envImportErr` (if non-null).
	void SetEnvMapData(const std::string& filepath, int w, int h,
	                   std::vector<float> pixels,
	                   rt2::core::Error* envImportErr = nullptr);

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

	// Import an OBJ file into the EXISTING scene (merges meshes/materials/
	// textures, creates a wrapper root entity with one child per shape when
	// settings.mergeMegaMesh is false, or a single mega-mesh child when true).
	// Does NOT clear the scene. Returns the wrapper root entity, or invalid
	// EntityId on failure.
	EntityId ImportObj(const std::string& filepath, const ImportSettings& settings);

	// Merge a temporary ECSScene (produced by SceneLoader::ImportObjIntoECS
	// or ImportIntoECS on a fresh ECSScene) into the live scene. Used by the
	// async import path: the worker thread parses into a temp scene, then
	// the main thread merges it here. Appends meshes (offsetting per-triangle
	// material indices by matBase), materials (remapping texture indices),
	// textures, and entities (re-parenting the wrapper root under the live
	// scene root). Assigns UUIDs to imported entities and fills source paths.
	// Returns the wrapper root entity in the live scene, or invalid on failure.
	EntityId MergeImportedECS(ECSScene&& src, entt::entity srcRoot,
	                          const std::string& sourcePath);

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

	// ---- Phase 3B1 structural command APIs ----
	// These APIs back undoable structural editor commands. They NEVER touch
	// the command layer; the host constructs commands and routes them through
	// EditorCommandHistory. The existing non-command APIs above stay unchanged
	// for non-undoable paths (RT2SliceRunner, host-driven non-undoable flows).

	// Remove subtrees WITHOUT running CompactMeshRegistry. Structural
	// commands use this so a snapshot's stored MeshRef::meshIndex stays
	// valid across Undo/Redo. The public RemoveSubtrees (with compaction)
	// stays for non-command paths.
	EditorMutationResult RemoveSubtreesNoCompact(
		const std::vector<rt2::core::UUID>& roots);

	// Remove the exact entities recorded in a SubtreeSnapshot. Validates
	// every expected UUID exists, validates authored component state and
	// hierarchy topology against the snapshot, rejects unexpected
	// descendants, performs all validation before destroying anything,
	// removes without resource compaction. Returns failure with no scene
	// mutation if validation fails. "Exact" compares authoritative
	// authored state only — not derived world matrices, GPU caches,
	// selection state, or other transient editor/runtime data.
	EditorMutationResult RemoveSubtreesExact(const SubtreeSnapshot& snapshot);

	// Restore every entity in a SubtreeSnapshot with its stored UUID, name,
	// parent, local TRS, visibility, and full-value component payloads.
	// Root sibling anchors are validated against the current parent's
	// children list (or the root-entity list); an inconsistent anchor fails
	// atomically (zero mutation). Re-creates with the SAME stored UUIDs so
	// Redo is idempotent.
	EditorMutationResult RestoreSubtrees(const SubtreeSnapshot& snapshot);

	// Capture the affected subtree(s) for command storage. Captures ONLY
	// the roots and their descendants — never the whole scene. Roots are
	// canonicalized (descendants of other roots are dropped) and ordered
	// in caller-supplied order.
	SubtreeSnapshot CaptureSubtreeSnapshot(
		const std::vector<rt2::core::UUID>& roots) const;

	// Explicit, safe compaction entry point. The Phase 3B1 invariant
	// forbids compaction while any Undo or Redo entry references resource
	// slots. The host is responsible for calling this only at
	// history.Clear(), document adoption, or save/reload. In debug builds
	// asserts history is clear; in release builds no-ops when history is
	// non-empty (host contract violation would silently corrupt a
	// snapshot's resource references otherwise).
	void CompactMeshRegistryNow();

	// Reserve known UUIDs for transactional creation commands. The host
	// reserves the exact count via CountCanonicalSubtreeEntities, then
	// passes the reserved UUIDs to the creation API. The manager's
	// internal count validation is mandatory protection against stale
	// input.
	rt2::core::UUID ReserveKnownUuid();
	std::vector<rt2::core::UUID> ReserveKnownUuids(size_t count);

	// Return the exact canonical entity count for a multi-root selection
	// including nested descendants. Uses the same root canonicalization
	// and deterministic traversal as duplication. Missing/invalid roots
	// return a failure result.
	rt2::core::Result<size_t> CountCanonicalSubtreeEntities(
		const std::vector<rt2::core::UUID>& roots) const;

	// Create an empty entity with a caller-supplied UUID at a known sibling
	// position. The host reserves the UUID, calls this, captures the
	// resulting SubtreeSnapshot, and constructs the creation command. If
	// creation fails the host must roll back. siblingPosition is optional;
	// when omitted the entity is appended as the last child (or last root).
	EditorMutationResult CreateEmptyWithUuid(
		const rt2::core::UUID& uuid,
		const std::string& name = "Empty",
		const std::optional<rt2::core::UUID>& parent = std::nullopt,
		std::optional<std::size_t> siblingPosition = std::nullopt);

	// Create a primitive entity (cube/sphere/plane) with a caller-supplied
	// UUID. The material is added to the material list and the mesh is
	// registered in the mesh registry; both resource slots are stable
	// while no compaction runs (3B1 invariant).
	EditorMutationResult CreatePrimitiveEntity(
		const rt2::core::UUID& uuid,
		const std::string& name,
		PrimitiveComponent::Kind kind,
		float size,
		const EditableTRS& localTRS,
		int materialIndex,
		const std::optional<rt2::core::UUID>& parent = std::nullopt);

	// Create a light entity with a caller-supplied UUID.
	EditorMutationResult CreateLightEntity(
		const rt2::core::UUID& uuid,
		const std::string& name,
		const EditableTRS& localTRS,
		const glm::vec3& color,
		float intensity,
		bool isSpot,
		const std::optional<rt2::core::UUID>& parent = std::nullopt);

	// Structured result for duplication/paste operations. `createdRoots`
	// are the new root UUIDs in canonical order; `sourceToDuplicate` maps
	// each source entity UUID to its duplicate's UUID (for paste, source
	// UUIDs are clipboard-document UUIDs, not destination-scene entities).
	struct DuplicationResult
	{
		EditorMutationResult mutation;
		std::vector<rt2::core::UUID> createdRoots;
		std::vector<std::pair<rt2::core::UUID, rt2::core::UUID>> sourceToDuplicate;
	};

	// Duplicate subtrees with caller-supplied UUIDs. The manager
	// canonicalizes the roots (preserving caller order), walks each
	// canonical subtree in deterministic pre-order, validates the UUID
	// count exactly matches the resulting entity count, validates all
	// supplied UUIDs are valid/unique/absent from the document, builds and
	// validates the complete duplication plan before mutating, assigns
	// UUIDs positionally in that internal pre-order, and returns the
	// created root UUIDs plus the complete source-to-duplicate mapping.
	DuplicationResult DuplicateSubtreesWithUuids(
		const std::vector<rt2::core::UUID>& sourceRoots,
		const std::vector<rt2::core::UUID>& knownDuplicateUuids);

	// Paste subtrees from a clipboard document with caller-supplied UUIDs.
	// Same flat-UUID-list contract as DuplicateSubtreesWithUuids. The
	// source UUIDs in the returned mapping are clipboard-document UUIDs,
	// not entities currently present in the destination scene.
	DuplicationResult PasteSubtreesWithUuids(
		const rt2::core::SceneDocument& clipboard,
		const std::vector<rt2::core::UUID>& clipboardRoots,
		const std::optional<rt2::core::UUID>& parent,
		const std::vector<rt2::core::UUID>& knownPastedUuids);

	// Atomic multi-entity local-transform edit. Validates ALL UUIDs
	// resolve, applies all local TRS in one pass, marks dirty once,
	// refreshes affected camera subtrees once, bumps the revision ONCE,
	// and returns Transform impact with affected UUIDs. One missing
	// target => no mutation, Failure.
	EditorMutationResult SetLocalTransformStates(
		const std::vector<std::pair<rt2::core::UUID, EditableTRS>>& states);

	// Atomic batch reparent. Validates all entities and all new parents
	// resolve and no cycles, then applies all reparents atomically. For
	// PreserveWorld, converts each desired world matrix to local against
	// the NEW parent (singular/shear => fail all). Bumps the revision
	// once. Undo of a multi-source reparent where the sources originally
	// had different parents requires restoring each source to its own
	// original parent — ReparentCommand stores before/after ReparentEdit
	// lists and Undo always uses PreserveLocal with the stored before-local
	// TRS.
	EditorMutationResult ReparentBatch(
		const std::vector<ReparentEdit>& edits, ReparentMode mode);

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
	// Phase 6B/W0: script component presence + read access for the inspector.
	bool HasScript(EntityId entity) const;
	// Returns nullopt when the entity is invalid or carries no ScriptComponent.
	std::optional<ScriptComponent> GetScriptState(const rt2::core::UUID& entity) const;

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

	// ---- Phase 3B2 atomic property state APIs ----
	//
	// Each applies the after-value, captures/restore side effects
	// (MaterialOverrideComponent on imported entities for the material
	// APIs), bumps the revision ONCE, and returns an authoritative
	// EditorMutationResult. Sync impact is set authoritatively:
	//   - Name / Camera-props / Motion: None
	//   - Light-props / Material-index / Material-props: Material
	//   - Camera-pose (composite local TRS + camera props): Transform
	//
	// The existing void-returning property APIs delegate to these so there
	// is one implementation; they remain for non-command paths
	// (RT2SliceRunner, host-driven non-undoable flows).
	//
	// Signatures are after-value-only, consistent with the 3A property
	// command precedent (SetLocalTransformStates, SetVisibilityStates).
	// Before-state lives in the command alone.
	EditorMutationResult SetEntityNameState(const rt2::core::UUID& entity,
	                                        const std::string& name);
	EditorMutationResult SetLightPropertiesState(const rt2::core::UUID& entity,
	                                             const LightComponent& value);
	EditorMutationResult SetCameraPropertiesState(const rt2::core::UUID& entity,
	                                              const CameraComponent& value);
	EditorMutationResult SetMaterialPropertiesState(int slotIndex,
	                                                const SceneMaterial& value);
	EditorMutationResult SetMaterialIndexState(const rt2::core::UUID& entity,
	                                           int afterIndex);
	EditorMutationResult SetMotionState(const rt2::core::UUID& entity,
	                                    const std::optional<MotionComponent>& value);
	// Phase 6B/W0: add, remove, or replace an entity's ScriptComponent.
	// nullopt removes. SyncImpact is None — script bindings and field values
	// are authored/runtime state that never touches the GPU scene (see the
	// Phase 6B plan, D8; mirrors SetMotionState exactly).
	EditorMutationResult SetScriptState(const rt2::core::UUID& entity,
	                                    const std::optional<ScriptComponent>& value);
	EditorMutationResult SetCameraPoseState(const rt2::core::UUID& entity,
	                                        const EditableTRS& local,
	                                        const CameraComponent& props);

	// ---- Phase 3B2 MaterialOverrideComponent capture/restore helpers ----
	//
	// The material commands (SetMaterialIndex, SetMaterialProperties) must
	// capture and restore durable MaterialOverrideComponent state atomically
	// with the index/material-value edit, so Undo of an imported-entity
	// material assignment does not leave a stale override that save/reopen
	// would resurrect. The command stores before/after override snapshots
	// (std::optional<MaterialOverrideComponent>; nullopt = absent) and uses
	// these helpers.
	//
	// GetMaterialOverride returns the current override or nullopt if the
	// entity has no MaterialOverrideComponent (or does not exist).
	//
	// InstallMaterialOverride installs the supplied override verbatim (no
	// re-derivation from source) when has_value, or removes the component
	// when nullopt. Does NOT bump the revision or notify — pair with a
	// state-API call that does. Safe to call on a non-imported entity (the
	// component is still installed/removed as requested).
	std::optional<MaterialOverrideComponent> GetMaterialOverride(
		const rt2::core::UUID& entity) const;
	void InstallMaterialOverride(
		const rt2::core::UUID& entity,
		const std::optional<MaterialOverrideComponent>& override);

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
	const std::string& GetEnvMapPath() const { return m_Authoring.environment.ref.path; }
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
