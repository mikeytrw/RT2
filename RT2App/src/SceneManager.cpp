#include "SceneManager.h"
#include "SceneLoader.h"
#include "SceneGraph.h"
#include "SceneHierarchy.h"
#include "EditorCameraWorkflow.h"
#include "PersistedComponents.h"
#include "RTLog.h"
#include "stb_image.h"
#include <tinyexr.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstdio>
#include <set>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>

namespace
{
std::vector<entt::entity> ResolveCanonicalRoots(
	const rt2::core::SceneDocument& document,
	const std::vector<rt2::core::UUID>& uuids,
	rt2::core::Error& error)
{
	std::vector<entt::entity> resolved;
	std::unordered_set<entt::entity> unique;
	for (const auto& uuid : uuids)
	{
		const auto entity = document.FindByUuid(uuid);
		if (entity == entt::null || !document.ecs.registry.valid(entity))
		{
			error.code = rt2::core::Error::InvalidEntity;
			error.path = uuid.ToString();
			error.detail = "entity UUID is not present in the authoring scene";
			return {};
		}
		if (unique.insert(entity).second)
			resolved.push_back(entity);
	}

	std::vector<entt::entity> canonical;
	for (const auto candidate : resolved)
	{
		bool covered = false;
		for (const auto possibleAncestor : resolved)
			if (candidate != possibleAncestor &&
				SceneHierarchy::IsDescendant(document.ecs.registry,
				                             possibleAncestor, candidate))
			{
				covered = true;
				break;
			}
		if (!covered)
			canonical.push_back(candidate);
	}
	return canonical;
}

void RemoveChild(entt::registry& registry, entt::entity parent, entt::entity child)
{
	if (parent == entt::null)
		return;
	if (auto* hierarchy = registry.try_get<Hierarchy>(parent))
		hierarchy->children.erase(
			std::remove(hierarchy->children.begin(), hierarchy->children.end(), child),
			hierarchy->children.end());
}

void CopyAuthoredComponents(const entt::registry& sourceRegistry,
	                       entt::entity source,
	                       entt::registry& destinationRegistry,
	                       entt::entity destination)
{
	PersistedComponents::ForEach([&](auto tag)
	{
		using Component = typename decltype(tag)::Type;
		if (const auto* component = sourceRegistry.try_get<Component>(source))
			destinationRegistry.emplace<Component>(destination, *component);
	});
	if (auto* transform = destinationRegistry.try_get<Transform>(destination))
	{
		transform->worldMatrix = glm::mat4(1.0f);
		transform->prevWorldMatrix = glm::mat4(1.0f);
		transform->dirty = true;
	}
}
}

SceneManager::SceneManager()
	: m_EcsScene(m_Authoring.ecs)
	, m_CurrentGpuScene(m_Authoring.gpuCache)
{
	m_Authoring.SetUuidProvider(&m_DefaultProvider);
}

void SceneManager::SetUuidProvider(rt2::core::IUuidProvider* provider)
{
	m_UuidProvider = provider ? provider : &m_DefaultProvider;
	m_Authoring.SetUuidProvider(m_UuidProvider);
}

entt::entity SceneManager::FindEntityByUuid(const rt2::core::UUID& uuid) const
{
	return m_Authoring.FindByUuid(uuid);
}

rt2::core::UUID SceneManager::GetEntityUuid(EntityId entity) const
{
	if (!entity.IsValid() || !m_EcsScene.registry.valid(entity.id))
		return rt2::core::UUID::Nil();
	const auto* identity = m_EcsScene.registry.try_get<EntityIdComponent>(entity.id);
	return identity ? identity->id : rt2::core::UUID::Nil();
}

void SceneManager::ReplaceAuthoringDocument(rt2::core::SceneDocument&& document,
	                                         uint64_t authoringRevision)
{
	// SceneDocument does not own its provider. Rebind it to the manager's
	// provider before and after assignment so future entity creation remains
	// valid even when the temporary document used a short-lived provider.
	document.SetUuidProvider(m_UuidProvider);
	m_Authoring = std::move(document);
	m_Authoring.SetUuidProvider(m_UuidProvider);
	rt2::core::Error hierarchyError;
	if (!SceneHierarchy::RebuildChildren(m_EcsScene.registry, hierarchyError))
		printf("[Scene] Adopted document hierarchy is invalid: %s\n",
		       hierarchyError.Format().c_str());

	m_EntityCache.clear();
	m_EntityCacheDirty = true;
	m_AuthoringRevision = authoringRevision;
	ReconcileStoredCameraDirections();
	++m_DocumentGeneration;
	++m_ResourceGeneration;
}

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
		// Fall through to UUID assignment + wrapper-root + validation below.
	}
	else
	{
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
	}

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

			m_Authoring.AssignNewUuid(rootEntity);
		}
	}

	// Rebuild the UUID index for all entities loaded by the scene loader.
	// SceneLoader creates entities directly on the registry without going
	// through SceneManager::Add*, so they do not yet have EntityIdComponent.
	// Assign UUIDs to any entity that lacks one, then validate.
	{
		auto& reg = m_EcsScene.registry;
		auto view = reg.view<Transform>();
		for (auto entity : view)
		{
			if (!reg.all_of<EntityIdComponent>(entity))
				m_Authoring.AssignNewUuid(entity);
		}
	}

	// Record the source model path on imported mesh entities so the native
	// .rt2scene serializer can persist a durable reference. The path is
	// stored as-is; the serializer relativizes it at save time.
	{
		auto& reg = m_EcsScene.registry;
		auto mv = reg.view<ImportedMeshSourceComponent>();
		for (auto e : mv)
		{
			auto& src = mv.get<ImportedMeshSourceComponent>(e);
			if (src.model.path.empty())
				src.model.path = filepath;
		}
	}

	rt2::core::Error hierarchyError;
	if (!SceneHierarchy::RebuildChildren(m_EcsScene.registry, hierarchyError))
	{
		printf("[Scene] Hierarchy validation failed after load: %s\n",
		       hierarchyError.Format().c_str());
		return false;
	}

	rt2::core::Error uuidErr;
	if (!m_Authoring.ValidateUniqueUuids(uuidErr))
	{
		printf("[Scene] UUID validation failed after load: %s\n", uuidErr.Format().c_str());
		fflush(stdout);
	}

	printf("[Scene] Loaded %d meshes, %d materials, %d lights, %d textures\n",
	       (int)m_EcsScene.meshRegistry.GetCount(), (int)m_EcsScene.materials.size(),
	       (int)m_EcsScene.lights.size(), (int)m_EcsScene.textures.size());

	m_EntityCacheDirty = true;
	++m_DocumentGeneration;
	++m_ResourceGeneration;
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

	m_Authoring.environment.path = filepath;
	m_Authoring.environment.width = w;
	m_Authoring.environment.height = h;
	m_Authoring.environment.floatPixels = std::move(pixels);
	return true;
}

void SceneManager::ClearEnvMap()
{
	m_Authoring.environment.Clear();
}

SceneManager::EntityId SceneManager::ImportGltf(const std::string& filepath)
{
	entt::entity root = SceneLoader::ImportIntoECS(m_EcsScene, filepath);
	if (root == entt::null)
		return EntityId{};

	// Assign UUIDs to any imported entity that lacks one.
	auto& reg = m_EcsScene.registry;
	auto view = reg.view<Transform>();
	for (auto entity : view)
	{
		if (!reg.all_of<EntityIdComponent>(entity))
			m_Authoring.AssignNewUuid(entity);
	}

	// Record the source model path on every imported mesh entity so the
	// native .rt2scene serializer can persist a durable reference. The path
	// is stored as-is (possibly absolute); the serializer relativizes it
	// against the .rt2scene location at save time.
	{
		auto mv = reg.view<ImportedMeshSourceComponent>();
		for (auto e : mv)
		{
			auto& src = mv.get<ImportedMeshSourceComponent>(e);
			if (src.model.path.empty())
				src.model.path = filepath;
		}
	}

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
	gpuData = BuildGPUSceneDataFromECS(m_EcsScene, &m_RenderInstanceMap);

	printf("[Scene] SyncToGPU: GPUSceneData built: meshes=%zu instances=%zu lights=%zu textures=%zu source_emissive=%u filtered_black=%u\n",
	       gpuData.meshes.size(), gpuData.instances.size(), gpuData.lights.size(), gpuData.textures.size(),
	       gpuData.sourceEmissiveTriangleCount, gpuData.filteredBlackEmissiveTriangleCount);
	fflush(stdout);

	// Add env map as an extra texture in the texture array
	if (HasEnvMap())
	{
		auto& env = m_Authoring.environment;
		SceneTexture envTex;
		envTex.isHDR = true;
		envTex.width = env.width;
		envTex.height = env.height;
		envTex.floatPixels = env.floatPixels;
		gpuData.textures.push_back(envTex);
		gpuData.envMapIndex = (int)gpuData.textures.size() - 1;

		BuildEnvMapCDF(env.floatPixels, env.width, env.height,
		               gpuData.marginalCDF, gpuData.conditionalCDF);
		gpuData.cdfWidth = env.width;
		gpuData.cdfHeight = env.height;

		printf("[Scene] Env map: idx=%d %dx%d, CDF built\n",
		       gpuData.envMapIndex, env.width, env.height);
	}

	if (m_SyncCallback)
	{
		printf("[Scene] SyncToGPU: calling sync callback (SetScene)...\n");
		fflush(stdout);
		m_SyncCallback(gpuData, m_RenderInstanceMap);
		printf("[Scene] SyncToGPU: sync callback done\n");
		fflush(stdout);
	}

	m_CurrentGpuScene = std::move(gpuData);
}

void SceneManager::SyncToGPUKeepTextures()
{
	GPUSceneData gpuData;

	UpdateWorldTransforms();
	gpuData = BuildGPUSceneDataFromECS(m_EcsScene, &m_RenderInstanceMap);

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
		m_SyncKeepTexturesCallback(gpuData, m_RenderInstanceMap);
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

	m_Authoring.AssignNewUuid(entity);
	NotifyAuthoringChanged();
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

	m_Authoring.AssignNewUuid(entity);
	NotifyAuthoringChanged();
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

	m_Authoring.AssignNewUuid(entity);
	NotifyAuthoringChanged();
	m_EntityCacheDirty = true;
	return {entity};
}

void SceneManager::RemoveEntity(EntityId entity)
{
	if (!entity.IsValid()) return;

	auto& reg = m_EcsScene.registry;
	if (!reg.valid(entity.id)) return;
	const auto stableId = GetEntityUuid(entity);
	if (!stableId.IsNull())
	{
		RemoveSubtrees({ stableId });
		return;
	}

	// Remove from UUID index before destruction.
	if (auto* idc = reg.try_get<EntityIdComponent>(entity.id))
		m_Authoring.uuidIndex.Erase(idc->id);

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
	NotifyAuthoringChanged();
	m_EntityCacheDirty = true;
}

EditorMutationResult SceneManager::CreateEmpty(
	const std::string& name,
	const std::optional<rt2::core::UUID>& parentUuid)
{
	auto& registry = m_EcsScene.registry;
	entt::entity parent = entt::null;
	if (parentUuid)
	{
		parent = m_Authoring.FindByUuid(*parentUuid);
		if (parent == entt::null || !registry.valid(parent))
			return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
				parentUuid->ToString(), "parent UUID is not present in the authoring scene");
	}
	const auto entity = registry.create();
	registry.emplace<Transform>(entity);
	registry.emplace<NameComponent>(entity, name.empty() ? "Empty" : name);
	registry.emplace<VisibleComponent>(entity);
	if (parent != entt::null)
	{
		registry.emplace<Hierarchy>(entity).parent = parent;
		auto* hierarchy = registry.try_get<Hierarchy>(parent);
		if (!hierarchy)
			hierarchy = &registry.emplace<Hierarchy>(parent);
		hierarchy->children.push_back(entity);
	}
	const auto uuid = m_Authoring.AssignNewUuid(entity);
	SceneGraph::MarkDirty(registry, entity);
	NotifyAuthoringChanged();
	m_EntityCacheDirty = true;
	EditorMutationResult result;
	result.affectedEntities.push_back(uuid);
	return result;
}

EditorMutationResult SceneManager::Reparent(
	const std::vector<rt2::core::UUID>& entityUuids,
	const std::optional<rt2::core::UUID>& newParentUuid,
	ReparentMode mode)
{
	if (entityUuids.empty()) return {};
	auto& registry = m_EcsScene.registry;
	rt2::core::Error error;
	auto roots = ResolveCanonicalRoots(m_Authoring, entityUuids, error);
	if (!error.IsOk())
	{
		EditorMutationResult result;
		result.success = false;
		result.error = error;
		return result;
	}
	entt::entity newParent = entt::null;
	if (newParentUuid)
	{
		newParent = m_Authoring.FindByUuid(*newParentUuid);
		if (newParent == entt::null || !registry.valid(newParent))
			return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
				newParentUuid->ToString(), "new parent UUID is not present in the authoring scene");
	}
	std::vector<entt::entity> changed;
	for (const auto root : roots)
	{
		if (newParent != entt::null && SceneHierarchy::IsDescendant(registry, root, newParent))
			return EditorMutationResult::Failure(rt2::core::Error::HierarchyCycle,
				GetEntityUuid({ root }).ToString(),
				"cannot parent an entity beneath itself or a descendant");
		const auto* hierarchy = registry.try_get<Hierarchy>(root);
		if ((hierarchy ? hierarchy->parent : entt::null) != newParent)
			changed.push_back(root);
	}
	if (changed.empty()) return {};

	UpdateWorldTransforms();
	std::vector<std::pair<entt::entity, EditableTRS>> newLocals;
	if (mode == ReparentMode::PreserveWorld)
	{
		glm::mat4 parentWorld(1.0f);
		if (newParent != entt::null)
		{
			const auto* parentTransform = registry.try_get<Transform>(newParent);
			if (!parentTransform)
				return EditorMutationResult::Failure(rt2::core::Error::InvalidTransform,
					newParentUuid->ToString(), "new parent has no transform");
			parentWorld = parentTransform->worldMatrix;
		}
		for (const auto root : changed)
		{
			const auto* transform = registry.try_get<Transform>(root);
			if (!transform)
				return EditorMutationResult::Failure(rt2::core::Error::InvalidTransform,
					GetEntityUuid({ root }).ToString(), "reparented entity has no transform");
			EditableTRS local;
			if (!TryWorldToLocalTRS(parentWorld, transform->worldMatrix, local))
				return EditorMutationResult::Failure(rt2::core::Error::InvalidTransform,
					GetEntityUuid({ root }).ToString(),
					"preserve-world reparent produced a singular or sheared transform");
			newLocals.emplace_back(root, local);
		}
	}

	for (const auto root : changed)
	{
		auto* hierarchy = registry.try_get<Hierarchy>(root);
		RemoveChild(registry, hierarchy ? hierarchy->parent : entt::null, root);
		if (!hierarchy)
			hierarchy = &registry.emplace<Hierarchy>(root);
		hierarchy->parent = newParent;
		if (newParent != entt::null)
		{
			auto* parentHierarchy = registry.try_get<Hierarchy>(newParent);
			if (!parentHierarchy)
				parentHierarchy = &registry.emplace<Hierarchy>(newParent);
			parentHierarchy->children.push_back(root);
		}
	}
	for (const auto& entry : newLocals)
	{
		auto& transform = registry.get<Transform>(entry.first);
		transform.translation = entry.second.translation;
		transform.rotation = glm::normalize(entry.second.rotation);
		transform.scale = entry.second.scale;
	}
	for (const auto root : changed)
		SceneGraph::MarkDirty(registry, root);
	RefreshCameraForwardDirections(changed);
	NotifyAuthoringChanged();
	m_EntityCacheDirty = true;
	EditorMutationResult result;
	result.syncImpact = rt2::core::SyncImpact::Transform;
	for (const auto root : changed)
		result.affectedEntities.push_back(GetEntityUuid({ root }));
	return result;
}

EditorMutationResult SceneManager::RemoveSubtrees(
	const std::vector<rt2::core::UUID>& rootUuids)
{
	if (rootUuids.empty()) return {};
	rt2::core::Error error;
	auto roots = ResolveCanonicalRoots(m_Authoring, rootUuids, error);
	if (!error.IsOk())
	{
		EditorMutationResult result;
		result.success = false;
		result.error = error;
		return result;
	}
	auto& registry = m_EcsScene.registry;
	std::vector<entt::entity> postOrder;
	bool removesRenderable = false;
	EditorMutationResult result;
	for (const auto root : roots)
	{
		std::vector<entt::entity> subtree;
		SceneHierarchy::CollectSubtreePostOrder(registry, root, subtree);
		for (const auto entity : subtree)
		{
			postOrder.push_back(entity);
			removesRenderable = removesRenderable || registry.all_of<MeshRef>(entity);
			if (const auto* identity = registry.try_get<EntityIdComponent>(entity))
				result.affectedEntities.push_back(identity->id);
		}
		if (const auto* hierarchy = registry.try_get<Hierarchy>(root))
			RemoveChild(registry, hierarchy->parent, root);
	}
	for (const auto entity : postOrder)
	{
		if (const auto* identity = registry.try_get<EntityIdComponent>(entity))
			m_Authoring.uuidIndex.Erase(identity->id);
		registry.destroy(entity);
	}
	const bool compacted = CompactMeshRegistry();
	NotifyAuthoringChanged();
	m_EntityCacheDirty = true;
	result.syncImpact = (removesRenderable || compacted)
		? rt2::core::SyncImpact::Structural : rt2::core::SyncImpact::None;
	return result;
}

EditorMutationResult SceneManager::SetVisibility(
	const std::vector<rt2::core::UUID>& entityUuids, bool visible)
{
	if (entityUuids.empty()) return {};
	auto& registry = m_EcsScene.registry;
	std::vector<entt::entity> entities;
	std::unordered_set<entt::entity> unique;
	for (const auto& uuid : entityUuids)
	{
		const auto entity = m_Authoring.FindByUuid(uuid);
		if (entity == entt::null || !registry.valid(entity))
			return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
				uuid.ToString(), "entity UUID is not present in the authoring scene");
		if (unique.insert(entity).second)
			entities.push_back(entity);
	}
	EditorMutationResult result;
	for (const auto entity : entities)
	{
		auto* component = registry.try_get<VisibleComponent>(entity);
		const bool current = component ? component->visible : true;
		if (current == visible) continue;
		if (!component)
			component = &registry.emplace<VisibleComponent>(entity);
		component->visible = visible;
		result.affectedEntities.push_back(GetEntityUuid({ entity }));
	}
	if (result.affectedEntities.empty()) return result;
	NotifyAuthoringChanged();
	result.syncImpact = rt2::core::SyncImpact::Structural;
	return result;
}

EditorMutationResult SceneManager::SetVisibilityStates(
	const std::vector<std::pair<rt2::core::UUID, bool>>& states)
{
	if (states.empty()) return {};
	auto& registry = m_EcsScene.registry;

	// Validate ALL UUIDs first. Any failure => zero mutation. Deduplicate
	// last-write-wins by walking in order and overwriting the per-entity
	// target slot.
	std::vector<std::pair<entt::entity, bool>> resolved;
	std::unordered_map<entt::entity, std::size_t> indexByEntity;
	for (const auto& [uuid, visible] : states)
	{
		const auto entity = m_Authoring.FindByUuid(uuid);
		if (entity == entt::null || !registry.valid(entity))
			return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
				uuid.ToString(), "entity UUID is not present in the authoring scene");
		const auto it = indexByEntity.find(entity);
		if (it == indexByEntity.end())
		{
			indexByEntity.emplace(entity, resolved.size());
			resolved.emplace_back(entity, visible);
		}
		else
		{
			resolved[it->second].second = visible; // last-write-wins
		}
	}

	EditorMutationResult result;
	for (const auto& [entity, visible] : resolved)
	{
		auto* component = registry.try_get<VisibleComponent>(entity);
		const bool current = component ? component->visible : true;
		if (current == visible) continue;
		if (!component)
			component = &registry.emplace<VisibleComponent>(entity);
		component->visible = visible;
		result.affectedEntities.push_back(GetEntityUuid({ entity }));
	}

	if (result.affectedEntities.empty())
	{
		// Empty-success: no entities actually changed state.
		return result;
	}

	NotifyAuthoringChanged();
	result.syncImpact = rt2::core::SyncImpact::Structural;
	return result;
}

EditorMutationResult SceneManager::DuplicateSubtrees(
	const std::vector<rt2::core::UUID>& rootUuids)
{
	if (rootUuids.empty()) return {};
	rt2::core::Error error;
	auto roots = ResolveCanonicalRoots(m_Authoring, rootUuids, error);
	if (!error.IsOk())
	{
		EditorMutationResult result;
		result.success = false;
		result.error = error;
		return result;
	}
	auto& registry = m_EcsScene.registry;
	std::vector<entt::entity> sources;
	for (const auto root : roots)
		SceneHierarchy::CollectSubtreePreOrder(registry, root, sources);

	std::unordered_map<entt::entity, entt::entity> remap;
	bool duplicatesRenderable = false;
	for (const auto source : sources)
	{
		const auto duplicate = registry.create();
		remap.emplace(source, duplicate);
		CopyAuthoredComponents(registry, source, registry, duplicate);
		m_Authoring.AssignNewUuid(duplicate);
		duplicatesRenderable = duplicatesRenderable || registry.all_of<MeshRef>(duplicate);
	}
	for (const auto source : sources)
	{
		const auto duplicate = remap.at(source);
		const auto* sourceHierarchy = registry.try_get<Hierarchy>(source);
		if (!sourceHierarchy || sourceHierarchy->parent == entt::null)
			continue;
		const auto mappedParent = remap.find(sourceHierarchy->parent);
		const auto duplicateParent = mappedParent != remap.end()
			? mappedParent->second : sourceHierarchy->parent;
		registry.emplace<Hierarchy>(duplicate).parent = duplicateParent;
		auto* parentHierarchy = registry.try_get<Hierarchy>(duplicateParent);
		if (!parentHierarchy)
			parentHierarchy = &registry.emplace<Hierarchy>(duplicateParent);
		parentHierarchy->children.push_back(duplicate);
	}

	EditorMutationResult result;
	for (const auto root : roots)
	{
		const auto duplicate = remap.at(root);
		if (auto* name = registry.try_get<NameComponent>(duplicate))
			name->name += " Copy";
		result.affectedEntities.push_back(GetEntityUuid({ duplicate }));
		SceneGraph::MarkDirty(registry, duplicate);
	}
	NotifyAuthoringChanged();
	m_EntityCacheDirty = true;
	result.syncImpact = duplicatesRenderable
		? rt2::core::SyncImpact::Structural : rt2::core::SyncImpact::None;
	return result;
}

EditorMutationResult SceneManager::PasteSubtreesFrom(
	const rt2::core::SceneDocument& snapshot,
	const std::vector<rt2::core::UUID>& rootUuids,
	const std::optional<rt2::core::UUID>& parentUuid)
{
	if (rootUuids.empty()) return {};
	rt2::core::Error error;
	auto roots = ResolveCanonicalRoots(snapshot, rootUuids, error);
	if (!error.IsOk())
	{
		EditorMutationResult result;
		result.success = false;
		result.error = error;
		return result;
	}
	auto& destination = m_EcsScene.registry;
	entt::entity destinationParent = entt::null;
	if (parentUuid)
	{
		destinationParent = m_Authoring.FindByUuid(*parentUuid);
		if (destinationParent == entt::null || !destination.valid(destinationParent))
			return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
				parentUuid->ToString(), "paste parent is not present in the authoring scene");
	}

	std::vector<entt::entity> sources;
	for (const auto root : roots)
		SceneHierarchy::CollectSubtreePreOrder(snapshot.ecs.registry, root, sources);
	for (const auto source : sources)
	{
		if (const auto* mesh = snapshot.ecs.registry.try_get<MeshRef>(source))
		{
			if (mesh->meshIndex >= m_EcsScene.meshRegistry.GetCount())
				return EditorMutationResult::Failure(rt2::core::Error::ClipboardStale,
					GetEntityUuid({ destinationParent }).ToString(),
					"clipboard mesh resources no longer match this document");
			if (mesh->materialIndex >= static_cast<int>(m_EcsScene.materials.size()))
				return EditorMutationResult::Failure(rt2::core::Error::ClipboardStale,
					{}, "clipboard material resources no longer match this document");
		}
	}

	std::unordered_map<entt::entity, entt::entity> remap;
	bool pastesRenderable = false;
	for (const auto source : sources)
	{
		const auto pasted = destination.create();
		remap.emplace(source, pasted);
		CopyAuthoredComponents(snapshot.ecs.registry, source, destination, pasted);
		m_Authoring.AssignNewUuid(pasted);
		pastesRenderable = pastesRenderable || destination.all_of<MeshRef>(pasted);
	}
	for (const auto source : sources)
	{
		const auto pasted = remap.at(source);
		const auto* sourceHierarchy = snapshot.ecs.registry.try_get<Hierarchy>(source);
		entt::entity pastedParent = destinationParent;
		if (sourceHierarchy)
		{
			const auto mappedParent = remap.find(sourceHierarchy->parent);
			if (mappedParent != remap.end())
				pastedParent = mappedParent->second;
		}
		if (pastedParent == entt::null)
			continue;
		destination.emplace<Hierarchy>(pasted).parent = pastedParent;
		auto* parentHierarchy = destination.try_get<Hierarchy>(pastedParent);
		if (!parentHierarchy)
			parentHierarchy = &destination.emplace<Hierarchy>(pastedParent);
		parentHierarchy->children.push_back(pasted);
	}

	EditorMutationResult result;
	for (const auto root : roots)
	{
		const auto pasted = remap.at(root);
		if (auto* name = destination.try_get<NameComponent>(pasted))
			name->name += " Copy";
		result.affectedEntities.push_back(GetEntityUuid({ pasted }));
		SceneGraph::MarkDirty(destination, pasted);
	}
	NotifyAuthoringChanged();
	m_EntityCacheDirty = true;
	result.syncImpact = pastesRenderable
		? rt2::core::SyncImpact::Structural : rt2::core::SyncImpact::None;
	return result;
}

void SceneManager::SetTransform(EntityId entity,
                                const glm::vec3& position,
                                const glm::vec3& rotation,
                                float scale)
{
	SetTransform(entity, position, rotation, glm::vec3(scale));
}

void SceneManager::SetTransform(EntityId entity,
	const glm::vec3& position, const glm::vec3& rotation, const glm::vec3& scale)
{
	if (!entity.IsValid()) return;
	auto& reg = m_EcsScene.registry;
	if (auto* tf = reg.try_get<Transform>(entity.id))
	{
		tf->translation = position;
		tf->rotation = glm::quat(glm::radians(rotation));
		tf->scale = scale;
		SceneGraph::SetLocalDirty(reg, entity.id);
		RefreshCameraForwardDirections({ entity.id });
		NotifyAuthoringChanged();
	}
}

void SceneManager::SetLocalTransform(EntityId entity, const EditableTRS& transform)
{
	if (!entity.IsValid() || !m_EcsScene.registry.valid(entity.id)) return;
	if (auto* tf = m_EcsScene.registry.try_get<Transform>(entity.id))
	{
		tf->translation = transform.translation;
		tf->rotation = glm::normalize(transform.rotation);
		tf->scale = transform.scale;
		SceneGraph::MarkDirty(m_EcsScene.registry, entity.id);
		RefreshCameraForwardDirections({ entity.id });
		NotifyAuthoringChanged();
	}
}

bool SceneManager::TrySetWorldTransform(EntityId entity, const glm::mat4& desiredWorld)
{
	return TrySetWorldTransforms({ { entity, desiredWorld } });
}

bool SceneManager::TrySetWorldTransforms(
	const std::vector<std::pair<EntityId, glm::mat4>>& desiredWorldTransforms)
{
	if (desiredWorldTransforms.empty()) return true;
	UpdateWorldTransforms();
	auto& registry = m_EcsScene.registry;
	std::unordered_map<entt::entity, glm::mat4> desiredByEntity;
	desiredByEntity.reserve(desiredWorldTransforms.size());
	for (const auto& edit : desiredWorldTransforms)
	{
		if (!edit.first.IsValid() || !registry.valid(edit.first.id) ||
			!registry.all_of<Transform>(edit.first.id) ||
			!desiredByEntity.emplace(edit.first.id, edit.second).second)
			return false;
	}
	std::unordered_map<entt::entity, glm::mat4> predictedWorldCache;
	std::unordered_set<entt::entity> resolving;
	std::function<bool(entt::entity, glm::mat4&)> resolvePredictedWorld;
	resolvePredictedWorld = [&](entt::entity entity, glm::mat4& outWorld) -> bool
	{
		const auto desired = desiredByEntity.find(entity);
		if (desired != desiredByEntity.end())
		{
			outWorld = desired->second;
			return true;
		}
		const auto cached = predictedWorldCache.find(entity);
		if (cached != predictedWorldCache.end())
		{
			outWorld = cached->second;
			return true;
		}
		if (!registry.valid(entity) || !registry.all_of<Transform>(entity) ||
			!resolving.insert(entity).second)
			return false;
		glm::mat4 parentWorld(1.0f);
		if (const auto* hierarchy = registry.try_get<Hierarchy>(entity);
			hierarchy && hierarchy->parent != entt::null)
		{
			if (!resolvePredictedWorld(hierarchy->parent, parentWorld))
			{
				resolving.erase(entity);
				return false;
			}
		}
		outWorld = parentWorld * registry.get<Transform>(entity).localMatrix();
		predictedWorldCache.emplace(entity, outWorld);
		resolving.erase(entity);
		return true;
	};

	std::vector<std::pair<entt::entity, EditableTRS>> locals;
	locals.reserve(desiredWorldTransforms.size());
	for (const auto& edit : desiredWorldTransforms)
	{
		glm::mat4 parentWorld(1.0f);
		const EntityId parent = GetParent(edit.first);
		if (parent.IsValid() && !resolvePredictedWorld(parent.id, parentWorld))
			return false;
		EditableTRS local;
		if (!TryWorldToLocalTRS(parentWorld, edit.second, local)) return false;
		locals.emplace_back(edit.first.id, local);
	}

	for (const auto& edit : locals)
	{
		auto& transform = registry.get<Transform>(edit.first);
		transform.translation = edit.second.translation;
		transform.rotation = glm::normalize(edit.second.rotation);
		transform.scale = edit.second.scale;
		SceneGraph::MarkDirty(registry, edit.first);
	}
	std::vector<entt::entity> changedEntities;
	changedEntities.reserve(locals.size());
	for (const auto& edit : locals)
		changedEntities.push_back(edit.first);
	RefreshCameraForwardDirections(changedEntities);
	NotifyAuthoringChanged();
	return true;
}

EditorMutationResult SceneManager::AlignCameraEntityToView(
	const rt2::core::UUID& cameraEntity, const EditorCameraPose& requested)
{
	EditorCameraPose pose = requested;
	if (!TryNormalizeEditorCameraPose(pose))
		return EditorMutationResult::Failure(rt2::core::Error::InvalidTransform,
			cameraEntity.ToString(), "editor camera pose is invalid");
	const entt::entity entity = m_Authoring.FindByUuid(cameraEntity);
	if (entity == entt::null || !m_EcsScene.registry.valid(entity) ||
		!m_EcsScene.registry.all_of<Transform, CameraComponent>(entity))
		return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
			cameraEntity.ToString(), "selected entity is not a camera");

	EditableTRS currentWorld;
	if (!GetWorldTransform({ entity }, currentWorld))
		return EditorMutationResult::Failure(rt2::core::Error::InvalidTransform,
			cameraEntity.ToString(), "camera world transform is not representable as TRS");
	glm::quat rotation;
	if (!TryCameraRotationFromForward(pose.forward, rotation))
		return EditorMutationResult::Failure(rt2::core::Error::InvalidTransform,
			cameraEntity.ToString(), "camera forward vector is invalid");
	EditableTRS desired = currentWorld;
	desired.translation = pose.position;
	desired.rotation = rotation;
	if (!TrySetWorldTransform({ entity }, desired.Matrix()))
		return EditorMutationResult::Failure(rt2::core::Error::InvalidTransform,
			cameraEntity.ToString(),
			"camera alignment was rejected; its parent may be singular or non-uniformly scaled");

	auto& component = m_EcsScene.registry.get<CameraComponent>(entity);
	component.verticalFOV = pose.verticalFOV;
	component.aperture = pose.aperture;
	component.focusDistance = pose.focusDistance;
	EditorMutationResult result;
	result.syncImpact = rt2::core::SyncImpact::Transform;
	result.affectedEntities.push_back(cameraEntity);
	return result;
}

void SceneManager::RefreshCameraForwardDirections(
	const std::vector<entt::entity>& roots)
{
	if (roots.empty()) return;
	auto& registry = m_EcsScene.registry;
	SceneGraph::UpdateWorldTransforms(registry);
	std::unordered_set<entt::entity> visited;
	for (const entt::entity root : roots)
	{
		if (!registry.valid(root)) continue;
		std::vector<entt::entity> subtree;
		SceneHierarchy::CollectSubtreePreOrder(registry, root, subtree);
		for (const entt::entity entity : subtree)
		{
			if (!visited.insert(entity).second ||
				!registry.all_of<CameraComponent, Transform>(entity))
				continue;
			EditableTRS world;
			if (!TryDecomposeEditableTRS(registry.get<Transform>(entity).worldMatrix, world))
				continue;
			const glm::vec3 forward =
				world.rotation * glm::vec3(0.0f, 0.0f, -1.0f);
			if (glm::dot(forward, forward) > 1e-8f)
				registry.get<CameraComponent>(entity).forwardDirection =
					glm::normalize(forward);
		}
	}
}

void SceneManager::ReconcileStoredCameraDirections()
{
	auto& registry = m_EcsScene.registry;
	SceneGraph::UpdateWorldTransforms(registry);
	std::vector<entt::entity> cameras;
	const auto view = registry.view<CameraComponent, Transform>();
	for (const entt::entity entity : view)
	{
		const auto& camera = view.get<CameraComponent>(entity);
		glm::quat rotation;
		if (!TryCameraRotationFromForward(camera.forwardDirection, rotation))
			continue;
		EditableTRS currentWorld;
		if (!TryDecomposeEditableTRS(view.get<Transform>(entity).worldMatrix,
			currentWorld))
			continue;
		currentWorld.rotation = rotation;
		glm::mat4 parentWorld(1.0f);
		if (const auto* hierarchy = registry.try_get<Hierarchy>(entity);
			hierarchy && hierarchy->parent != entt::null)
		{
			const auto* parentTransform = registry.try_get<Transform>(hierarchy->parent);
			if (!parentTransform) continue;
			parentWorld = parentTransform->worldMatrix;
		}
		EditableTRS local;
		if (!TryWorldToLocalTRS(parentWorld, currentWorld.Matrix(), local))
			continue;
		auto& transform = view.get<Transform>(entity);
		transform.translation = local.translation;
		transform.rotation = glm::normalize(local.rotation);
		transform.scale = local.scale;
		SceneGraph::MarkDirty(registry, entity);
		cameras.push_back(entity);
	}
	RefreshCameraForwardDirections(cameras);
}

void SceneManager::SetMaterial(EntityId entity, int materialIndex)
{
	if (!entity.IsValid()) return;
	auto& reg = m_EcsScene.registry;
	if (auto* ref = reg.try_get<MeshRef>(entity.id))
	{
		ref->materialIndex = materialIndex;
		// If this is an imported entity, record a durable override so the
		// assignment survives save/reopen. The override captures the material
		// value at the assigned index; the resolver re-appends it on reopen
		// rather than discarding the user's choice in favor of the re-imported
		// source material.
		if (reg.all_of<ImportedMeshSourceComponent>(entity.id))
		{
			RecordMaterialOverride(entity.id, materialIndex);
		}
		NotifyAuthoringChanged();
	}
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
	NotifyAuthoringChanged();
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

bool SceneManager::GetTransform(EntityId entity, glm::vec3& outPosition,
	glm::vec3& outRotationEuler, glm::vec3& outScale) const
{
	EditableTRS transform;
	if (!GetLocalTransform(entity, transform)) return false;
	outPosition = transform.translation;
	outRotationEuler = glm::degrees(glm::eulerAngles(transform.rotation));
	outScale = transform.scale;
	return true;
}

bool SceneManager::GetLocalTransform(EntityId entity, EditableTRS& outTransform) const
{
	if (!entity.IsValid() || !m_EcsScene.registry.valid(entity.id)) return false;
	const auto* transform = m_EcsScene.registry.try_get<Transform>(entity.id);
	if (!transform) return false;
	outTransform.translation = transform->translation;
	outTransform.rotation = transform->rotation;
	outTransform.scale = transform->scale;
	return true;
}

bool SceneManager::GetWorldTransform(EntityId entity, EditableTRS& outTransform)
{
	if (!entity.IsValid() || !m_EcsScene.registry.valid(entity.id)) return false;
	UpdateWorldTransforms();
	const auto* transform = m_EcsScene.registry.try_get<Transform>(entity.id);
	return transform && TryDecomposeEditableTRS(transform->worldMatrix, outTransform);
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
	NotifyAuthoringChanged();
}

bool SceneManager::SetCameraProperties(EntityId entity, float verticalFOV,
	float aperture, float focusDistance)
{
	if (!entity.IsValid() || !m_EcsScene.registry.valid(entity.id)) return false;
	auto* camera = m_EcsScene.registry.try_get<CameraComponent>(entity.id);
	if (!camera) return false;
	if (camera->verticalFOV == verticalFOV && camera->aperture == aperture &&
		camera->focusDistance == focusDistance)
		return false;
	camera->verticalFOV = verticalFOV;
	camera->aperture = aperture;
	camera->focusDistance = focusDistance;
	NotifyAuthoringChanged();
	return true;
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
	NotifyAuthoringChanged();
}

void SceneManager::SyncTransformsToGPU()
{
	if (m_EcsScene.meshRegistry.GetCount() == 0) return;

	UpdateWorldTransforms();

	GPUSceneData gpuData = m_CurrentGpuScene;
	UpdateInstancesFromECS(gpuData, m_EcsScene, &m_RenderInstanceMap);

	if (m_InstanceSyncCallback)
		m_InstanceSyncCallback(gpuData, m_RenderInstanceMap);

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
	NotifyAuthoringChanged();
	return idx;
}

SceneMaterial& SceneManager::GetMaterial(int index)
{
	if (index >= 0 && index < (int)m_EcsScene.materials.size())
		return m_EcsScene.materials[index];
	static SceneMaterial dummy;
	return dummy;
}

void SceneManager::SetMaterialProperties(int index, const SceneMaterial& props)
{
	if (index < 0 || index >= (int)m_EcsScene.materials.size())
		return;
	m_EcsScene.materials[index] = props;

	// Propagate the edit into durable MaterialOverrideComponent on every
	// imported entity whose MeshRef points at this material slot, so saved
	// material edits survive reopen. Without this, the resolver would
	// re-import the source material and discard the user's edits.
	{
		auto& reg = m_EcsScene.registry;
		auto view = reg.view<ImportedMeshSourceComponent>();
		for (auto e : view)
		{
			auto* ref = reg.try_get<MeshRef>(e);
			if (ref && ref->materialIndex == index)
				RecordMaterialOverride(e, index);
		}
	}

	NotifyAuthoringChanged();
}

void SceneManager::RecordMaterialOverride(entt::entity entity, int materialIndex)
{
	auto& reg = m_EcsScene.registry;
	if (!reg.valid(entity)) return;
	if (materialIndex < 0 || materialIndex >= (int)m_EcsScene.materials.size())
		return;

	// Derive the durable source material key from the imported source, if
	// available. For glTF primitives the sourceKey encodes the primitive; the
	// material key is separate and not currently recoverable from the loader
	// without deeper integration, so we use a generic stable key derived from
	// the model source key + the current material slot. This is durable
	// enough to match the override back to the rebuilt source material slot.
	std::string sourceMatKey;
	if (auto* src = reg.try_get<ImportedMeshSourceComponent>(entity))
		sourceMatKey = src->model.sourceKey + ":material";

	MaterialOverrideComponent ov;
	ov.material        = m_EcsScene.materials[materialIndex];
	ov.authored        = true;
	ov.sourceMaterialKey = sourceMatKey;
	ov.materialIndex   = materialIndex; // transient; repaired by resolver
	reg.emplace_or_replace<MaterialOverrideComponent>(entity, ov);
}

void SceneManager::NotifyAuthoringChanged()
{
	m_Authoring.metadata.dirty = true;
	++m_AuthoringRevision;
}

// ============================================================================
// Stats + misc
// ============================================================================

void SceneManager::Clear()
{
	m_Authoring.Clear();
	m_EntityCacheDirty = true;
	++m_DocumentGeneration;
	++m_ResourceGeneration;
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

		// The durable override value survives compaction, but its transient
		// material slot must follow the same remap as MeshRef.
		auto overrideView = reg.view<MaterialOverrideComponent>();
		for (const auto entity : overrideView)
		{
			auto& materialOverride = overrideView.get<MaterialOverrideComponent>(entity);
			const auto it = matRemap.find(materialOverride.materialIndex);
			materialOverride.materialIndex = it != matRemap.end() ? it->second : -1;
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
		auto overrideView = reg.view<MaterialOverrideComponent>();
		for (const auto entity : overrideView)
		{
			auto& mat = overrideView.get<MaterialOverrideComponent>(entity).material;
			remapTex(mat.baseColorTextureIndex);
			remapTex(mat.normalTextureIndex);
			remapTex(mat.emissiveTextureIndex);
			remapTex(mat.metallicRoughnessTextureIndex);
		}

		printf("[Scene] Compacted textures: %zu -> %zu\n",
		       texRemap.size() + (m_EcsScene.textures.size() - texRemap.size()),
		       m_EcsScene.textures.size());
	}

	const bool changed = meshesChanged || matsChanged || texsChanged;
	if (changed)
		++m_ResourceGeneration;
	return changed;
}

// ============================================================================
// Internal
// ============================================================================

void SceneManager::UpdateWorldTransforms()
{
	SceneGraph::UpdateWorldTransforms(m_EcsScene.registry);
}
