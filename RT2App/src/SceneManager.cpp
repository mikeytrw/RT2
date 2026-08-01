#include "SceneManager.h"
#include "SceneLoader.h"
#include "SceneGraph.h"
#include "SceneHierarchy.h"
#include "EditorCameraWorkflow.h"
#include "PersistedComponents.h"
#include "PrimitiveGeometry.h"
#include "RTLog.h"
#include "ScriptComponentValidation.h"
#include "ScriptAssetPath.h"
#include "AssetIdentity.h"
#include "stb_image.h"
#include <tinyexr.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstdio>
#include <set>
#include <map>
#include <string>
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

void LogAssetDiagnostics(
	const std::vector<rt2::core::AssetDiagnostic>& diagnostics,
	size_t base,
	const char* context)
{
	for (size_t i = base; i < diagnostics.size(); ++i)
	{
		const auto& diagnostic = diagnostics[i];
		printf("[Asset] %s %s: path=%s source=%s detail=%s\n",
		       context,
		       rt2::core::AssetDiagnosticSeverityName(
			       diagnostic.severity),
		       diagnostic.refPath.c_str(),
		       diagnostic.sourceKey.c_str(),
		       diagnostic.detail.c_str());
	}
}

// All resource indices stored in ECSScene pass through this walk. Import
// merging uses bases; compaction supplies old-to-new maps. Keeping both
// operations on the same field list makes a new index-bearing field visible
// at the one place that must be updated for both operations.
enum class IndexRebaseMode
{
	None,
	Base,
	Remap,
};

template <typename Index>
class IndexRebaseAxis
{
public:
	void SetBase(Index base)
	{
		m_mode = IndexRebaseMode::Base;
		m_base = base;
		m_remap = nullptr;
	}

	void SetRemap(const std::map<Index, Index>& remap)
	{
		m_mode = IndexRebaseMode::Remap;
		m_remap = &remap;
	}

	bool IsActive() const { return m_mode != IndexRebaseMode::None; }

	Index Apply(Index index, Index unmapped) const
	{
		switch (m_mode)
		{
			case IndexRebaseMode::None:
				return index;
			case IndexRebaseMode::Base:
				return index + m_base;
			case IndexRebaseMode::Remap:
			{
				const auto it = m_remap->find(index);
				return it != m_remap->end() ? it->second : unmapped;
			}
		}
		return index;
	}

private:
	IndexRebaseMode m_mode = IndexRebaseMode::None;
	Index m_base = 0;
	const std::map<Index, Index>* m_remap = nullptr;
};

struct IndexRebase
{
	IndexRebaseAxis<uint32_t> mesh;
	IndexRebaseAxis<int> material;
	IndexRebaseAxis<int> texture;

	uint32_t Mesh(uint32_t index) const
	{
		return mesh.Apply(index, index);
	}

	int Material(int index) const
	{
		if (index < 0)
			return index;
		// Ordinary material references historically remain unchanged when a
		// compaction map has no entry for them; preserve that behavior.
		return material.Apply(index, index);
	}

	int MaterialOverride(int index) const
	{
		if (index < 0)
			return index;
		// The old compaction pass invalidated a transient override slot when
		// its material was removed; this deliberate asymmetry must remain.
		return material.Apply(index, -1);
	}

	int Texture(int index) const
	{
		if (index < 0)
			return index;
		// Compaction historically invalidated orphaned texture references.
		return texture.Apply(index, -1);
	}
};

void RebaseIndices(ECSScene& scene, const IndexRebase& rebase)
{
	// This is the complete list of scene-resource index fields. Keep all
	// additions here so merge and compaction cannot silently diverge.
	auto rebaseMaterialTextures = [&](SceneMaterial& material)
	{
		material.baseColorTextureIndex =
			rebase.Texture(material.baseColorTextureIndex);
		material.normalTextureIndex =
			rebase.Texture(material.normalTextureIndex);
		material.emissiveTextureIndex =
			rebase.Texture(material.emissiveTextureIndex);
		material.metallicRoughnessTextureIndex =
			rebase.Texture(material.metallicRoughnessTextureIndex);
	};

	if (rebase.material.IsActive())
	{
		for (uint32_t meshIndex = 0;
		     meshIndex < scene.meshRegistry.GetCount();
		     ++meshIndex)
		{
			auto& mesh = scene.meshRegistry.GetMesh(meshIndex);
			for (auto& materialIndex : mesh.materialIndices)
			{
				const int remapped = rebase.Material(static_cast<int>(materialIndex));
				if (remapped >= 0)
					materialIndex = static_cast<uint32_t>(remapped);
			}
		}
	}

	if (rebase.texture.IsActive())
		for (auto& material : scene.materials)
			rebaseMaterialTextures(material);

	if (rebase.mesh.IsActive() || rebase.material.IsActive())
	{
		auto meshRefView = scene.registry.view<MeshRef>();
		for (const auto entity : meshRefView)
		{
			auto& ref = meshRefView.get<MeshRef>(entity);
			if (rebase.mesh.IsActive())
				ref.meshIndex = rebase.Mesh(ref.meshIndex);
			if (rebase.material.IsActive())
				ref.materialIndex = rebase.Material(ref.materialIndex);
		}
	}

	if (rebase.material.IsActive() || rebase.texture.IsActive())
	{
		auto overrideView = scene.registry.view<MaterialOverrideComponent>();
		for (const auto entity : overrideView)
		{
			auto& materialOverride = overrideView.get<MaterialOverrideComponent>(entity);
			if (rebase.texture.IsActive())
				rebaseMaterialTextures(materialOverride.material);
			if (rebase.material.IsActive())
				materialOverride.materialIndex =
					rebase.MaterialOverride(materialOverride.materialIndex);
		}
	}
}
}

// Fill an imported entity's source path (if empty) and assign it a stable
// asset ID from the per-asset sidecar (.rt2meta), minting+writing the sidecar
// when absent (Phase 7 W1, per D8). Resolution by path is unchanged; the ID
// is plumbed but not yet authoritative. A minted ID is logged so a missing or
// malformed sidecar is observable, not silent. Errors do not block import:
// the scene still gets an ID for this session; the next save retries.
void FillImportedSourcePathAndId(entt::registry& reg,
                                 const std::string& filepath,
                                 rt2::core::IUuidProvider& provider)
{
	auto mv = reg.view<ImportedMeshSourceComponent>();
	for (auto e : mv)
	{
		auto& src = reg.get<ImportedMeshSourceComponent>(e);
		if (src.model.path.empty())
			src.model.path = filepath;
		if (!src.model.assetId.IsNull())
			continue; // already has a stable ID (e.g. from a loaded .rt2scene)

		bool minted = false;
		rt2::core::Error idErr;
		const rt2::core::UUID id =
			rt2::core::ResolveOrAssign(filepath, provider, minted, idErr);
		src.model.assetId = id;
		if (minted)
		{
			if (!idErr.IsOk())
			{
				printf("[Asset] %s: sidecar issue, assigned new id %s: %s\n",
				       filepath.c_str(), id.ToString().c_str(),
				       idErr.Format().c_str());
			}
			else
			{
				printf("[Asset] %s: assigned new id %s\n",
				       filepath.c_str(), id.ToString().c_str());
			}
			fflush(stdout);
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

bool SceneManager::LoadScene(
	const std::string& filepath,
	std::vector<rt2::core::AssetDiagnostic>* diagnostics)
{
	printf("[Scene] LoadScene: '%s'\n", filepath.c_str());
	fflush(stdout);

	std::string ext = filepath.substr(filepath.find_last_of('.') + 1);
	std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
	bool isObj = (ext == "obj");
	std::vector<rt2::core::AssetDiagnostic> localDiagnostics;
	auto& diagnosticSink =
		diagnostics ? *diagnostics : localDiagnostics;
	const size_t diagnosticBase = diagnosticSink.size();
	rt2::core::AssetResolutionContext importContext = m_AssetResolutionContext;
	if (importContext.assetRoot.empty())
		importContext.assetRoot = std::filesystem::u8path(filepath).parent_path();

	if (isObj)
	{
		rt2::core::TextureAssetLoadContext textureContext;
		if (!rt2::core::BuildExplicitImportTextureContext(
			    std::filesystem::u8path(filepath), m_UuidProvider,
			    importContext,
			    textureContext, diagnosticSink))
		{
			if (!diagnostics)
				LogAssetDiagnostics(
					diagnosticSink, diagnosticBase, "LoadScene");
			return false;
		}
		if (!SceneLoader::LoadObjIntoECS(
			    m_EcsScene, textureContext, diagnosticSink))
		{
			if (!diagnostics)
				LogAssetDiagnostics(
					diagnosticSink, diagnosticBase, "LoadScene");
			printf("[Scene] LoadObjIntoECS failed!\n");
			return false;
		}
		printf("[Scene] LoadObjIntoECS succeeded\n");
		fflush(stdout);
		// Fall through to UUID assignment + wrapper-root + validation below.
	}
	else
	{
		rt2::core::TextureAssetLoadContext textureContext;
		if (!rt2::core::BuildExplicitImportTextureContext(
			    std::filesystem::u8path(filepath), m_UuidProvider,
			    importContext,
			    textureContext, diagnosticSink))
		{
			if (!diagnostics)
				LogAssetDiagnostics(
					diagnosticSink, diagnosticBase, "LoadScene");
			return false;
		}
		if (!SceneLoader::LoadIntoECS(
			    m_EcsScene, textureContext, diagnosticSink))
		{
			if (!diagnostics)
				LogAssetDiagnostics(
					diagnosticSink, diagnosticBase, "LoadScene");
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
	// .rt2scene serializer can persist a durable reference, and assign each
	// a stable asset ID from its sidecar (Phase 7 W1).
	FillImportedSourcePathAndId(m_EcsScene.registry, filepath, *m_UuidProvider);

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
	       (int)m_EcsScene.registry.view<const LightComponent>().size(),
	       (int)m_EcsScene.textures.size());

	m_EntityCacheDirty = true;
	++m_DocumentGeneration;
	++m_ResourceGeneration;
	if (!diagnostics)
		LogAssetDiagnostics(diagnosticSink, diagnosticBase, "LoadScene");
	return true;
}

bool SceneManager::LoadEnvMap(const std::string& filepath,
                                rt2::core::Error* envImportErr)
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

	m_Authoring.environment.ref.kind = AssetKind::Environment;
	m_Authoring.environment.ref.path = filepath;
	m_Authoring.environment.width = w;
	m_Authoring.environment.height = h;
	m_Authoring.environment.floatPixels = std::move(pixels);

	// Phase 7 W3 step 4: assign a stable env-asset ID via the per-asset
	// sidecar, paralleling model import. The sidecar is the durable source
	// of truth; env.ref.assetId is a cache of it. ResolveEnvironment reads
	// the sidecar through the shared locator and never mints.
	if (m_UuidProvider)
	{
		bool minted = false;
		rt2::core::Error idErr;
		const rt2::core::UUID id = rt2::core::ResolveOrAssign(
			filepath, *m_UuidProvider, minted, idErr);
		m_Authoring.environment.ref.assetId = id;
		if (minted)
		{
			printf("[Asset] %s: assigned new id %s%s%s\n",
			       filepath.c_str(), id.ToString().c_str(),
			       idErr.IsOk() ? "" : ": ",
			       idErr.IsOk() ? "" : idErr.Format().c_str());
			fflush(stdout);
		}
		// Retain a structured diagnostic for sidecar read/write errors
		// (item 4): the load still succeeds, but the caller can surface
		// the error instead of relying on console output.
		if (envImportErr)
			*envImportErr = idErr;
	}
	return true;
}

void SceneManager::ClearEnvMap()
{
	m_Authoring.environment.Clear();
}

void SceneManager::SetEnvMapData(const std::string& filepath, int w, int h,
                                 std::vector<float> pixels,
                                 rt2::core::Error* envImportErr)
{
	m_Authoring.environment.ref.kind = AssetKind::Environment;
	m_Authoring.environment.ref.path = filepath;
	m_Authoring.environment.width = w;
	m_Authoring.environment.height = h;
	m_Authoring.environment.floatPixels = std::move(pixels);

	// Phase 7 W3 step 4: assign a stable env-asset ID via the sidecar. This
	// is the async-load completion path (WalnutApp background decode); it
	// must keep env.ref.assetId in lockstep with LoadEnvMap so a save/reopen
	// round-trip resolves by the same identity.
	if (m_UuidProvider)
	{
		bool minted = false;
		rt2::core::Error idErr;
		const rt2::core::UUID id = rt2::core::ResolveOrAssign(
			filepath, *m_UuidProvider, minted, idErr);
		m_Authoring.environment.ref.assetId = id;
		if (minted)
		{
			printf("[Asset] %s: assigned new id %s%s%s\n",
			       filepath.c_str(), id.ToString().c_str(),
			       idErr.IsOk() ? "" : ": ",
			       idErr.IsOk() ? "" : idErr.Format().c_str());
			fflush(stdout);
		}
		// Retain a structured diagnostic for sidecar read/write errors
		// (item 4).
		if (envImportErr)
			*envImportErr = idErr;
	}
}

SceneManager::EntityId SceneManager::ImportGltf(
	const std::string& filepath,
	std::vector<rt2::core::AssetDiagnostic>* diagnostics)
{
	std::vector<rt2::core::AssetDiagnostic> localDiagnostics;
	auto& diagnosticSink =
		diagnostics ? *diagnostics : localDiagnostics;
	const size_t diagnosticBase = diagnosticSink.size();
	rt2::core::AssetResolutionContext importContext = m_AssetResolutionContext;
	if (importContext.assetRoot.empty())
		importContext.assetRoot = std::filesystem::u8path(filepath).parent_path();
	rt2::core::TextureAssetLoadContext textureContext;
	if (!rt2::core::BuildExplicitImportTextureContext(
		    std::filesystem::u8path(filepath), m_UuidProvider,
		    importContext,
		    textureContext, diagnosticSink))
	{
		if (!diagnostics)
			LogAssetDiagnostics(
				diagnosticSink, diagnosticBase, "ImportGltf");
		return EntityId{};
	}
	entt::entity root = SceneLoader::ImportIntoECS(
		m_EcsScene, textureContext, diagnosticSink);
	if (root == entt::null)
	{
		if (!diagnostics)
			LogAssetDiagnostics(
				diagnosticSink, diagnosticBase, "ImportGltf");
		return EntityId{};
	}

	// Assign UUIDs to any imported entity that lacks one.
	auto& reg = m_EcsScene.registry;
	auto view = reg.view<Transform>();
	for (auto entity : view)
	{
		if (!reg.all_of<EntityIdComponent>(entity))
			m_Authoring.AssignNewUuid(entity);
	}

	// Record the source model path on every imported mesh entity so the
	// native .rt2scene serializer can persist a durable reference, and
	// assign each a stable asset ID from its sidecar (Phase 7 W1).
	{
		auto& reg = m_EcsScene.registry;
		FillImportedSourcePathAndId(reg, filepath, *m_UuidProvider);
	}

	m_EntityCacheDirty = true;
	if (!diagnostics)
		LogAssetDiagnostics(
			diagnosticSink, diagnosticBase, "ImportGltf");
	return EntityId{ root };
}

SceneManager::EntityId SceneManager::ImportObj(
	const std::string& filepath,
	const ImportSettings& settings,
	std::vector<rt2::core::AssetDiagnostic>* diagnostics)
{
	std::vector<rt2::core::AssetDiagnostic> localDiagnostics;
	auto& diagnosticSink =
		diagnostics ? *diagnostics : localDiagnostics;
	const size_t diagnosticBase = diagnosticSink.size();
	rt2::core::AssetResolutionContext importContext = m_AssetResolutionContext;
	if (importContext.assetRoot.empty())
		importContext.assetRoot = std::filesystem::u8path(filepath).parent_path();
	rt2::core::TextureAssetLoadContext textureContext;
	if (!rt2::core::BuildExplicitImportTextureContext(
		    std::filesystem::u8path(filepath), m_UuidProvider,
		    importContext,
		    textureContext, diagnosticSink))
	{
		if (!diagnostics)
			LogAssetDiagnostics(
				diagnosticSink, diagnosticBase, "ImportObj");
		return EntityId{};
	}
	entt::entity root = SceneLoader::ImportObjIntoECS(
		m_EcsScene, settings, textureContext, diagnosticSink);
	if (root == entt::null)
	{
		if (!diagnostics)
			LogAssetDiagnostics(
				diagnosticSink, diagnosticBase, "ImportObj");
		return EntityId{};
	}

	// Assign UUIDs to any imported entity that lacks one.
	auto& reg = m_EcsScene.registry;
	auto view = reg.view<Transform>();
	for (auto entity : view)
	{
		if (!reg.all_of<EntityIdComponent>(entity))
			m_Authoring.AssignNewUuid(entity);
	}

	// Record the source model path on every imported mesh entity so the
	// native .rt2scene serializer can persist a durable reference, and
	// assign each a stable asset ID from its sidecar (Phase 7 W1).
	{
		FillImportedSourcePathAndId(reg, filepath, *m_UuidProvider);
	}

	m_EntityCacheDirty = true;
	if (!diagnostics)
		LogAssetDiagnostics(
			diagnosticSink, diagnosticBase, "ImportObj");
	return EntityId{ root };
}

SceneManager::EntityId SceneManager::MergeImportedECS(ECSScene&& src,
                                                       entt::entity srcRoot,
                                                       const std::string& sourcePath)
{
	if (srcRoot == entt::null || !src.registry.valid(srcRoot))
		return EntityId{};

	auto& dst = m_EcsScene;
	auto& dstReg = dst.registry;
	auto& srcReg = src.registry;

	// Record base offsets in the destination scene.
	const uint32_t meshBase = dst.meshRegistry.GetCount();
	const int matBase = (int)dst.materials.size();
	const int texBase = (int)dst.textures.size();
	IndexRebase rebase;
	rebase.mesh.SetBase(meshBase);
	rebase.material.SetBase(matBase);
	rebase.texture.SetBase(texBase);
	RebaseIndices(src, rebase);

	// Append meshes after the complete source scene has been rebased.
	for (uint32_t i = 0; i < src.meshRegistry.GetCount(); ++i)
		dst.meshRegistry.AddMesh(src.meshRegistry.GetMesh(i));

	// Append materials after their texture indices have been rebased.
	for (const auto& sm : src.materials)
		dst.materials.push_back(sm);

	// Append textures.
	for (auto& st : src.textures)
		dst.textures.push_back(std::move(st));

	// Map src entities to dst entities.
	std::unordered_map<entt::entity, entt::entity> entityMap;

	// First pass: create all dst entities and copy simple components.
	{
		auto view = srcReg.view<Transform>();
		for (auto e : view)
		{
			entt::entity dstE = dstReg.create();
			entityMap[e] = dstE;

			const auto& srcTf = view.get<Transform>(e);
			Transform& dstTf = dstReg.emplace<Transform>(dstE);
			dstTf.translation = srcTf.translation;
			dstTf.rotation = srcTf.rotation;
			dstTf.scale = srcTf.scale;
			dstTf.dirty = true;
		}
	}

	// Copy MeshRef after the complete source index walk. -1 remains the
	// "use per-triangle indices" sentinel.
	{
		auto view = srcReg.view<MeshRef>();
		for (auto e : view)
		{
			auto it = entityMap.find(e);
			if (it == entityMap.end()) continue;
			const auto& srcRef = view.get<MeshRef>(e);
			dstReg.emplace<MeshRef>(it->second, srcRef);
		}
	}

	// Copy NameComponent.
	{
		auto view = srcReg.view<NameComponent>();
		for (auto e : view)
		{
			auto it = entityMap.find(e);
			if (it == entityMap.end()) continue;
			dstReg.emplace<NameComponent>(it->second, view.get<NameComponent>(e));
		}
	}

	// Copy VisibleComponent.
	{
		auto view = srcReg.view<VisibleComponent>();
		for (auto e : view)
		{
			auto it = entityMap.find(e);
			if (it == entityMap.end()) continue;
			dstReg.emplace<VisibleComponent>(it->second, view.get<VisibleComponent>(e));
		}
	}

	// Material overrides are authored data rather than loader output today,
	// but they are valid ECSScene components and carry the same resource
	// indices. Copy the already-rebased value so this path cannot lose it if a
	// temporary import scene contains an override.
	{
		auto view = srcReg.view<MaterialOverrideComponent>();
		for (auto e : view)
		{
			auto it = entityMap.find(e);
			if (it == entityMap.end()) continue;
			dstReg.emplace<MaterialOverrideComponent>(
				it->second, view.get<MaterialOverrideComponent>(e));
		}
	}

	// Copy Hierarchy (remap parent + children via entityMap).
	{
		auto view = srcReg.view<Hierarchy>();
		for (auto e : view)
		{
			auto it = entityMap.find(e);
			if (it == entityMap.end()) continue;
			const auto& srcHier = view.get<Hierarchy>(e);
			Hierarchy dstHier;
			if (srcHier.parent != entt::null)
			{
				auto pit = entityMap.find(srcHier.parent);
				dstHier.parent = (pit != entityMap.end()) ? pit->second : entt::null;
			}
			else
				dstHier.parent = entt::null;
			for (auto child : srcHier.children)
			{
				auto cit = entityMap.find(child);
				if (cit != entityMap.end())
					dstHier.children.push_back(cit->second);
			}
			dstReg.emplace<Hierarchy>(it->second, std::move(dstHier));
		}
	}

	// Copy ImportedMeshSourceComponent + fill source path + assign sidecar ID.
	{
		auto view = srcReg.view<ImportedMeshSourceComponent>();
		for (auto e : view)
		{
			auto it = entityMap.find(e);
			if (it == entityMap.end()) continue;
			auto srcComp = view.get<ImportedMeshSourceComponent>(e);
			if (srcComp.model.path.empty())
				srcComp.model.path = sourcePath;
			if (srcComp.model.assetId.IsNull())
			{
				bool minted = false;
				rt2::core::Error idErr;
				const rt2::core::UUID id = rt2::core::ResolveOrAssign(
					sourcePath, *m_UuidProvider, minted, idErr);
				srcComp.model.assetId = id;
				if (minted)
				{
					printf("[Asset] %s: assigned new id %s%s%s\n",
					       sourcePath.c_str(), id.ToString().c_str(),
					       idErr.IsOk() ? "" : ": ",
					       idErr.IsOk() ? "" : idErr.Format().c_str());
					fflush(stdout);
				}
			}
			dstReg.emplace<ImportedMeshSourceComponent>(it->second, std::move(srcComp));
		}
	}

	// Assign UUIDs to all imported entities that lack one.
	{
		auto view = dstReg.view<Transform>();
		for (auto entity : view)
		{
			if (!dstReg.all_of<EntityIdComponent>(entity))
				m_Authoring.AssignNewUuid(entity);
		}
	}

	// Find the wrapper root in the destination.
	entt::entity dstRoot = entt::null;
	auto rootIt = entityMap.find(srcRoot);
	if (rootIt != entityMap.end())
		dstRoot = rootIt->second;

	// Update world transforms for the imported hierarchy.
	if (dstRoot != entt::null)
	{
		SceneGraph::SetLocalDirty(dstReg, dstRoot);
		SceneGraph::UpdateWorldTransforms(dstReg);
	}

	m_EntityCacheDirty = true;
	return EntityId{ dstRoot };
}

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

	// NOTE: this references mesh 0 even when the registry is empty, which is
	// a dangling reference until geometry arrives. It is deliberate and
	// depended upon: the production caller (WalnutApp's model-load path)
	// calls this straight after SceneLoader::LoadIntoECS, when index 0 is
	// valid, and test fixtures treat AddObject as "add a renderable object"
	// and read the MeshRef back. Making it conditional breaks both.
	//
	// The reference is inert because every consumer bounds-checks it before
	// indexing: CompactMeshRegistry skips out-of-range indices, and
	// GPUSceneData guards at :292 and :451. Anything new that indexes
	// meshRegistry by a MeshRef must do the same.
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
                                              LightType type)
{
	auto entity = m_EcsScene.registry.create();

	Transform tf;
	tf.translation = position;
	m_EcsScene.registry.emplace<Transform>(entity, tf);

	LightComponent light;
	light.color = color;
	light.intensity = intensity;
	light.type = type;
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

// ============================================================================
// Phase 3B1 structural command APIs
// ============================================================================

namespace
{

// Build a SubtreeEntityRecord from a live entity. Reads authored component
// state only — never derived world matrices, GPU caches, or other transient
// state. This is the snapshot-side mirror of the serializer's
// BuildEntityRecord, kept in SceneManager.cpp so the structural command
// APIs and the serializer stay aligned by construction (a mismatch would
// cause Undo/Redo to restore different state than what was captured).
SubtreeEntityRecord BuildSubtreeRecord(const entt::registry& reg, entt::entity e)
{
	SubtreeEntityRecord r;
	rt2::core::UUID uuid;
	if (const auto* idc = reg.try_get<EntityIdComponent>(e))
		uuid = idc->id;
	r.uuid = uuid;

	if (const auto* nc = reg.try_get<NameComponent>(e))
		r.name = nc->name;

	r.parentUuid = rt2::core::UUID::Nil();
	if (const auto* h = reg.try_get<Hierarchy>(e))
	{
		if (h->parent != entt::null && reg.valid(h->parent))
		{
			if (const auto* pidc = reg.try_get<EntityIdComponent>(h->parent))
				r.parentUuid = pidc->id;
		}
	}

	if (const auto* tf = reg.try_get<Transform>(e))
	{
		r.translation = tf->translation;
		r.rotation    = tf->rotation;
		r.scale       = tf->scale;
	}

	if (const auto* vc = reg.try_get<VisibleComponent>(e))
		r.visible = vc->visible;

	if (const auto* ref = reg.try_get<MeshRef>(e))
	{
		r.hasMeshRef    = true;
		r.meshIndex     = ref->meshIndex;
		r.materialIndex = ref->materialIndex;
	}

	if (const auto* pc = reg.try_get<PrimitiveComponent>(e))
	{
		r.hasPrimitive = true;
		r.primitive    = *pc;
	}

	if (const auto* isrc = reg.try_get<ImportedMeshSourceComponent>(e))
	{
		r.hasImportedSource = true;
		r.importedSource    = *isrc;
	}

	if (const auto* mov = reg.try_get<MaterialOverrideComponent>(e))
	{
		r.hasMaterialOverride = true;
		r.materialOverride    = *mov;
	}

	if (const auto* lc = reg.try_get<LightComponent>(e))
	{
		r.hasLight = true;
		r.light    = *lc;
	}

	if (const auto* cc = reg.try_get<CameraComponent>(e))
	{
		r.hasCamera = true;
		r.camera    = *cc;
	}

	if (const auto* mc = reg.try_get<MotionComponent>(e))
	{
		r.hasMotion = true;
		r.motion    = *mc;
	}

	if (const auto* sc = reg.try_get<ScriptComponent>(e))
	{
		r.hasScript = true;
		r.script    = *sc;
	}

	return r;
}

// Restore an entity's authored component state from a SubtreeEntityRecord.
// Emplaces/overwrites every persisted component the record carries. Does NOT
// touch derived world matrices, GPU caches, or other transient state — those
// are recomputed by SceneGraph after restoration.
void ApplySubtreeRecord(const SubtreeEntityRecord& record, entt::registry& reg,
                        entt::entity e)
{
	if (!record.name.empty())
		reg.emplace_or_replace<NameComponent>(e, NameComponent{record.name});

	if (auto* tf = reg.try_get<Transform>(e))
	{
		tf->translation = record.translation;
		tf->rotation    = glm::normalize(record.rotation);
		tf->scale       = record.scale;
		tf->dirty       = true;
	}
	else
	{
		Transform fresh;
		fresh.translation = record.translation;
		fresh.rotation    = record.rotation;
		fresh.scale       = record.scale;
		reg.emplace<Transform>(e, fresh);
	}

	reg.emplace_or_replace<VisibleComponent>(e, VisibleComponent{record.visible});

	if (record.hasMeshRef)
		reg.emplace_or_replace<MeshRef>(e, MeshRef{record.meshIndex, record.materialIndex});
	else
		reg.remove<MeshRef>(e);

	if (record.hasPrimitive)
		reg.emplace_or_replace<PrimitiveComponent>(e, record.primitive);
	else
		reg.remove<PrimitiveComponent>(e);

	if (record.hasImportedSource)
		reg.emplace_or_replace<ImportedMeshSourceComponent>(e, record.importedSource);
	else
		reg.remove<ImportedMeshSourceComponent>(e);

	if (record.hasMaterialOverride)
		reg.emplace_or_replace<MaterialOverrideComponent>(e, record.materialOverride);
	else
		reg.remove<MaterialOverrideComponent>(e);

	if (record.hasLight)
		reg.emplace_or_replace<LightComponent>(e, record.light);
	else
		reg.remove<LightComponent>(e);

	if (record.hasCamera)
		reg.emplace_or_replace<CameraComponent>(e, record.camera);
	else
		reg.remove<CameraComponent>(e);

	if (record.hasMotion)
		reg.emplace_or_replace<MotionComponent>(e, record.motion);
	else
		reg.remove<MotionComponent>(e);

	if (record.hasScript)
		reg.emplace_or_replace<ScriptComponent>(e, record.script);
	else
		reg.remove<ScriptComponent>(e);
}

// Compare authored component state on an entity against a record. Returns
// true if every persisted component matches exactly. Transient state
// (worldMatrix, prevWorldMatrix, dirty, selection, clipboard) is NOT
// compared — only authoritative authored state.
bool EntityMatchesRecord(const entt::registry& reg, entt::entity e,
                         const SubtreeEntityRecord& record)
{
	if (const auto* idc = reg.try_get<EntityIdComponent>(e))
	{
		if (!(idc->id == record.uuid)) return false;
	}
	else if (!record.uuid.IsNull()) return false;

	if (const auto* nc = reg.try_get<NameComponent>(e))
	{
		if (nc->name != record.name) return false;
	}
	else if (!record.name.empty()) return false;

	// Parent UUID
	rt2::core::UUID liveParent = rt2::core::UUID::Nil();
	if (const auto* h = reg.try_get<Hierarchy>(e))
	{
		if (h->parent != entt::null && reg.valid(h->parent))
		{
			if (const auto* pidc = reg.try_get<EntityIdComponent>(h->parent))
				liveParent = pidc->id;
		}
	}
	if (!(liveParent == record.parentUuid)) return false;

	if (const auto* tf = reg.try_get<Transform>(e))
	{
		constexpr float eps = 1e-5f;
		auto vEq = [eps](const glm::vec3& a, const glm::vec3& b) {
			return std::fabs(a.x - b.x) <= eps &&
			       std::fabs(a.y - b.y) <= eps &&
			       std::fabs(a.z - b.z) <= eps;
		};
		auto qEq = [eps](const glm::quat& a, const glm::quat& b) {
			glm::quat na = a; if (na.w < 0.0f) na = -na;
			glm::quat nb = b; if (nb.w < 0.0f) nb = -nb;
			return std::fabs(na.x - nb.x) <= eps &&
			       std::fabs(na.y - nb.y) <= eps &&
			       std::fabs(na.z - nb.z) <= eps &&
			       std::fabs(na.w - nb.w) <= eps;
		};
		if (!vEq(tf->translation, record.translation) ||
		    !qEq(tf->rotation, record.rotation) ||
		    !vEq(tf->scale, record.scale))
			return false;
	}
	else
	{
		// record always carries a TRS; if the live entity has no Transform,
		// it cannot match unless the record's TRS is identity — but a
		// structural command snapshot always carries a Transform, so treat
		// absence as a mismatch.
		return false;
	}

	bool liveVisible = true;
	if (const auto* vc = reg.try_get<VisibleComponent>(e))
		liveVisible = vc->visible;
	if (liveVisible != record.visible) return false;

	auto checkRef = [&](bool has, const MeshRef* ref) {
		if (has != record.hasMeshRef) return false;
		if (has && ref &&
		    (ref->meshIndex != record.meshIndex ||
		     ref->materialIndex != record.materialIndex))
			return false;
		return true;
	};
	if (!checkRef(reg.all_of<MeshRef>(e), reg.try_get<MeshRef>(e))) return false;

	// Per-component exact compare. Each persisted component is plain data;
	// we compare fields explicitly because PrimitiveComponent,
	// AssetReference, and SceneMaterial do not define operator==.
	if (reg.all_of<PrimitiveComponent>(e) != record.hasPrimitive) return false;
	if (record.hasPrimitive)
	{
		const auto& live = *reg.try_get<PrimitiveComponent>(e);
		if (live.kind != record.primitive.kind ||
		    std::fabs(live.size - record.primitive.size) > 1e-5f ||
		    live.segments != record.primitive.segments ||
		    live.rings != record.primitive.rings)
			return false;
	}

	if (reg.all_of<ImportedMeshSourceComponent>(e) != record.hasImportedSource) return false;
	if (record.hasImportedSource)
	{
		const auto& live = *reg.try_get<ImportedMeshSourceComponent>(e);
		if (!(live.model.kind == record.importedSource.model.kind &&
		      live.model.path == record.importedSource.model.path &&
		      live.model.sourceKey == record.importedSource.model.sourceKey &&
		      live.model.importSettings == record.importedSource.model.importSettings))
			return false;
	}

	if (reg.all_of<MaterialOverrideComponent>(e) != record.hasMaterialOverride) return false;
	if (record.hasMaterialOverride)
	{
		const auto& live = *reg.try_get<MaterialOverrideComponent>(e);
		constexpr float eps = 1e-5f;
		if (live.authored != record.materialOverride.authored) return false;
		if (live.sourceMaterialKey != record.materialOverride.sourceMaterialKey) return false;
		const auto& a = live.material;
		const auto& b = record.materialOverride.material;
		if (a.type != b.type) return false;
		if (glm::length(a.baseColor - b.baseColor) > eps) return false;
		if (std::fabs(a.baseAlpha - b.baseAlpha) > eps) return false;
		if (std::fabs(a.metallic - b.metallic) > eps) return false;
		if (std::fabs(a.roughness - b.roughness) > eps) return false;
		if (std::fabs(a.ior - b.ior) > eps) return false;
		if (std::fabs(a.transmissionFactor - b.transmissionFactor) > eps) return false;
		if (glm::length(a.emissiveColor - b.emissiveColor) > eps) return false;
		if (std::fabs(a.emissiveIntensity - b.emissiveIntensity) > eps) return false;
		if (a.baseColorTextureIndex != b.baseColorTextureIndex) return false;
		if (a.normalTextureIndex != b.normalTextureIndex) return false;
		if (a.emissiveTextureIndex != b.emissiveTextureIndex) return false;
		if (a.metallicRoughnessTextureIndex != b.metallicRoughnessTextureIndex) return false;
		if (a.alphaMode != b.alphaMode) return false;
		if (std::fabs(a.alphaCutoff - b.alphaCutoff) > eps) return false;
	}

	if (reg.all_of<LightComponent>(e) != record.hasLight) return false;
	if (record.hasLight)
	{
		const auto& live = *reg.try_get<LightComponent>(e);
		constexpr float eps = 1e-5f;
		if (glm::length(live.color - record.light.color) > eps) return false;
		if (std::fabs(live.intensity - record.light.intensity) > eps) return false;
		if (std::fabs(live.range - record.light.range) > eps) return false;
		if (std::fabs(live.innerConeAngle - record.light.innerConeAngle) > eps) return false;
		if (std::fabs(live.outerConeAngle - record.light.outerConeAngle) > eps) return false;
		if (live.type != record.light.type) return false;
	}

	if (reg.all_of<CameraComponent>(e) != record.hasCamera) return false;
	if (record.hasCamera)
	{
		const auto& live = *reg.try_get<CameraComponent>(e);
		constexpr float eps = 1e-5f;
		if (std::fabs(live.verticalFOV - record.camera.verticalFOV) > eps) return false;
		if (std::fabs(live.aperture - record.camera.aperture) > eps) return false;
		if (std::fabs(live.focusDistance - record.camera.focusDistance) > eps) return false;
		if (glm::length(live.forwardDirection - record.camera.forwardDirection) > eps) return false;
	}

	if (reg.all_of<MotionComponent>(e) != record.hasMotion) return false;
	if (record.hasMotion)
	{
		const auto& live = *reg.try_get<MotionComponent>(e);
		constexpr float eps = 1e-5f;
		if (glm::length(live.linearVelocity - record.motion.linearVelocity) > eps) return false;
	}

	// Phase 6: script component comparison. The asset reference + field
	// values must match exactly for a record to be considered consistent.
	// Field-value comparison is by structural equality (the variant and the
	// map both define operator==).
	if (reg.all_of<ScriptComponent>(e) != record.hasScript) return false;
	if (record.hasScript)
	{
		const auto& live = *reg.try_get<ScriptComponent>(e);
		if (!(live.asset.path == record.script.asset.path)) return false;
		if (live.asset.kind != record.script.asset.kind) return false;
		if (live.fieldValues.size() != record.script.fieldValues.size()) return false;
		for (const auto& [k, v] : record.script.fieldValues)
		{
			auto it = live.fieldValues.find(k);
			if (it == live.fieldValues.end()) return false;
			if (!(it->second == v)) return false;
		}
	}

	return true;
}

// Read the sibling anchor for a root entity: the prev/next sibling UUID
// among the parent's children (or the root-entity list when parent is null),
// plus the child index for diagnostic cross-check.
RootSiblingAnchor BuildSiblingAnchor(const entt::registry& reg, entt::entity root)
{
	RootSiblingAnchor anchor;
	entt::entity parent = entt::null;
	if (const auto* h = reg.try_get<Hierarchy>(root))
		parent = h->parent;

	std::vector<entt::entity> siblings;
	if (parent != entt::null)
	{
		if (const auto* ph = reg.try_get<Hierarchy>(parent))
			siblings = ph->children;
	}
	else
	{
		// Root entities: registry iteration order (unspecified).
		auto view = reg.view<EntityIdComponent>();
		for (auto e : view)
		{
			const auto* h = reg.try_get<Hierarchy>(e);
			if (!h || h->parent == entt::null)
				siblings.push_back(e);
		}
	}

	for (std::size_t i = 0; i < siblings.size(); ++i)
	{
		if (siblings[i] == root)
		{
			anchor.childIndex = i;
			if (i > 0)
			{
				if (const auto* idc = reg.try_get<EntityIdComponent>(siblings[i - 1]))
					anchor.prevSibling = idc->id;
			}
			if (i + 1 < siblings.size())
			{
				if (const auto* idc = reg.try_get<EntityIdComponent>(siblings[i + 1]))
					anchor.nextSibling = idc->id;
			}
			break;
		}
	}
	return anchor;
}

// Validate a root's anchor against the current parent's children list (or
// the root-entity list). Returns true if the anchor points to a position
// the root can be restored to consistently. The root itself need not be
// present (it was just removed).
// Validate a root's anchor against the current parent's children list.
// Returns true if the anchor points to a position the root can be restored
// to consistently. The root itself need not be present (it was just
// removed). Only called for parented roots — nil-parent roots skip anchor
// validation (root ordering is unspecified).
bool AnchorIsConsistent(const entt::registry& reg, const rt2::core::UUID& rootUuid,
                        const rt2::core::UUID& parentUuid, const RootSiblingAnchor& anchor)
{
	std::vector<entt::entity> siblings;
	// Find the parent entity by UUID.
	entt::entity found = entt::null;
	auto view = reg.view<EntityIdComponent>();
	for (auto e : view)
	{
		if (const auto* idc = reg.try_get<EntityIdComponent>(e);
		    idc && idc->id == parentUuid)
		{
			found = e;
			break;
		}
	}
	if (found == entt::null) return false;
	if (const auto* ph = reg.try_get<Hierarchy>(found))
		siblings = ph->children;

	// Convert siblings to UUIDs.
	std::vector<rt2::core::UUID> siblingUuids;
	siblingUuids.reserve(siblings.size());
	for (auto s : siblings)
	{
		if (const auto* idc = reg.try_get<EntityIdComponent>(s))
			siblingUuids.push_back(idc->id);
	}

	// Find the insertion position: prevSibling must appear immediately
	// before the gap, nextSibling immediately after.
	if (anchor.prevSibling.IsNull() && anchor.nextSibling.IsNull())
	{
		// First-and-last: only valid if the list is empty (the root was the
		// only child/root).
		return siblingUuids.empty();
	}

	if (anchor.prevSibling.IsNull())
	{
		// Root was the first child; nextSibling must now be the first.
		if (siblingUuids.empty()) return false;
		return siblingUuids.front() == anchor.nextSibling;
	}

	if (anchor.nextSibling.IsNull())
	{
		// Root was the last child; prevSibling must now be the last.
		if (siblingUuids.empty()) return false;
		return siblingUuids.back() == anchor.prevSibling;
	}

	// Middle: prevSibling and nextSibling must be adjacent in the current
	// list (the root fit between them).
	for (std::size_t i = 0; i + 1 < siblingUuids.size(); ++i)
	{
		if (siblingUuids[i] == anchor.prevSibling &&
		    siblingUuids[i + 1] == anchor.nextSibling)
			return true;
	}
	return false;
}

} // namespace

EditorMutationResult SceneManager::RemoveSubtreesNoCompact(
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
	// NO CompactMeshRegistry() — Phase 3B1 invariant.
	NotifyAuthoringChanged();
	m_EntityCacheDirty = true;
	result.syncImpact = removesRenderable
		? rt2::core::SyncImpact::Structural : rt2::core::SyncImpact::None;
	return result;
}

EditorMutationResult SceneManager::RemoveSubtreesExact(const SubtreeSnapshot& snapshot)
{
	auto& registry = m_EcsScene.registry;

	// Phase 1: validate every expected UUID exists and authored state
	// matches the snapshot. Any mismatch => zero mutation, Failure.
	std::vector<entt::entity> toDestroy;
	toDestroy.reserve(snapshot.entities.size());
	for (const auto& record : snapshot.entities)
	{
		const auto entity = m_Authoring.FindByUuid(record.uuid);
		if (entity == entt::null || !registry.valid(entity))
			return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
				record.uuid.ToString(),
				"RemoveSubtreesExact: expected entity is not present in the scene");
		if (!EntityMatchesRecord(registry, entity, record))
			return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
				record.uuid.ToString(),
				"RemoveSubtreesExact: authored state does not match the snapshot");
		toDestroy.push_back(entity);
	}

	// Phase 2: validate no unexpected descendants. Every entity that is a
	// descendant of a snapshot root must appear in the snapshot. This
	// catches out-of-band edits that added children after the snapshot was
	// captured.
	std::unordered_set<rt2::core::UUID> snapshotUuids;
	for (const auto& record : snapshot.entities)
		snapshotUuids.insert(record.uuid);

	std::vector<entt::entity> roots;
	roots.reserve(snapshot.rootUuids.size());
	for (const auto& rootUuid : snapshot.rootUuids)
	{
		const auto root = m_Authoring.FindByUuid(rootUuid);
		if (root == entt::null || !registry.valid(root))
			return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
				rootUuid.ToString(),
				"RemoveSubtreesExact: snapshot root is not present in the scene");
		roots.push_back(root);
	}

	for (const auto root : roots)
	{
		std::vector<entt::entity> subtree;
		SceneHierarchy::CollectSubtreePostOrder(registry, root, subtree);
		for (const auto entity : subtree)
		{
			const auto* idc = registry.try_get<EntityIdComponent>(entity);
			if (!idc || snapshotUuids.find(idc->id) == snapshotUuids.end())
				return EditorMutationResult::Failure(rt2::core::Error::InvalidHierarchy,
					idc ? idc->id.ToString() : std::string{},
					"RemoveSubtreesExact: subtree contains an entity not in the snapshot");
		}
	}

	// Phase 3: all validation passed. Remove without compaction.
	EditorMutationResult result;
	bool removesRenderable = false;
	for (const auto root : roots)
	{
		std::vector<entt::entity> subtree;
		SceneHierarchy::CollectSubtreePostOrder(registry, root, subtree);
		for (const auto entity : subtree)
		{
			removesRenderable = removesRenderable || registry.all_of<MeshRef>(entity);
			if (const auto* identity = registry.try_get<EntityIdComponent>(entity))
			{
				result.affectedEntities.push_back(identity->id);
				m_Authoring.uuidIndex.Erase(identity->id);
			}
		}
		if (const auto* hierarchy = registry.try_get<Hierarchy>(root))
			RemoveChild(registry, hierarchy->parent, root);
	}
	for (const auto entity : toDestroy)
		registry.destroy(entity);

	NotifyAuthoringChanged();
	m_EntityCacheDirty = true;
	result.syncImpact = removesRenderable
		? rt2::core::SyncImpact::Structural : rt2::core::SyncImpact::None;
	return result;
}

EditorMutationResult SceneManager::RestoreSubtrees(const SubtreeSnapshot& snapshot)
{
	auto& registry = m_EcsScene.registry;

	// Phase 1: validate every stored UUID is absent from the document
	// (Undo of a creation) or present with matching authored state (Undo of
	// a deletion). For Undo-of-creation, the entities were just removed by
	// the command's Execute; for Undo-of-deletion, the entities are still
	// absent and we re-create them. The anchor check happens in phase 2.
	for (const auto& record : snapshot.entities)
	{
		if (m_Authoring.uuidIndex.Contains(record.uuid))
		{
			// Entity already exists — this must be a no-op-safe restore
			// (the snapshot matches live state). Treat as success without
			// re-mutating to keep Redo idempotent when the entity is
			// already present.
			const auto existing = m_Authoring.FindByUuid(record.uuid);
			if (existing == entt::null || !registry.valid(existing))
				return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
					record.uuid.ToString(),
					"RestoreSubtrees: UUID index inconsistent with registry");
			if (!EntityMatchesRecord(registry, existing, record))
				return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
					record.uuid.ToString(),
					"RestoreSubtrees: existing entity does not match the snapshot");
		}
	}

	// Phase 2: validate root sibling anchors against the current parent's
	// children list. An inconsistent anchor fails atomically — restoration
	// never silently appends. Per the spec, root-entity ordering is
	// unspecified (registry-iteration order, no explicit authored ordering
	// vector); anchor validation is skipped for nil-parent roots (the
	// root-entity list is not a stable ordering authority) and kept
	// strict for parented roots (the parent's children list is authored
	// state).
	for (std::size_t i = 0; i < snapshot.rootUuids.size(); ++i)
	{
		const auto& rootUuid = snapshot.rootUuids[i];
		const auto& anchor = snapshot.rootAnchors[i];
		// Find the root's parent UUID from the record.
		rt2::core::UUID parentUuid;
		for (const auto& record : snapshot.entities)
		{
			if (record.uuid == rootUuid)
			{
				parentUuid = record.parentUuid;
				break;
			}
		}
		// If the root is already present, the anchor was already validated
		// by EntityMatchesRecord above (parent UUID matches). Skip the
		// anchor check in that case.
		if (m_Authoring.uuidIndex.Contains(rootUuid)) continue;
		// Skip anchor validation for nil-parent roots: root-entity
		// ordering is unspecified and the entt pool iteration order is not
		// a stable authority (it changes on every destroy/create via
		// swap-and-pop). Validating against it would cause legitimate Undo
		// to fail nondeterministically.
		if (parentUuid.IsNull()) continue;
		if (!AnchorIsConsistent(registry, rootUuid, parentUuid, anchor))
			return EditorMutationResult::Failure(rt2::core::Error::InvalidHierarchy,
				rootUuid.ToString(),
				"RestoreSubtrees: sibling anchor is inconsistent with the current parent's children");
	}

	// Phase 3: create entities in pre-order (parents before children) so
	// Hierarchy wiring resolves. Assign known UUIDs.
	bool addsRenderable = false;
	EditorMutationResult result;
	std::unordered_map<rt2::core::UUID, entt::entity> created;
	std::unordered_set<rt2::core::UUID> newlyCreated;
	for (const auto& record : snapshot.entities)
	{
		if (m_Authoring.uuidIndex.Contains(record.uuid))
		{
			// Already present and matches — skip (idempotent Redo).
			created[record.uuid] = m_Authoring.FindByUuid(record.uuid);
			continue;
		}
		const auto entity = registry.create();
		if (!m_Authoring.AssignKnownUuid(entity, record.uuid))
		{
			// Rollback: destroy everything we created so far.
			for (const auto& [uuid, e] : created)
			{
				if (const auto* idc = registry.try_get<EntityIdComponent>(e))
					m_Authoring.uuidIndex.Erase(idc->id);
				registry.destroy(e);
			}
			return EditorMutationResult::Failure(rt2::core::Error::DuplicateUuid,
				record.uuid.ToString(),
				"RestoreSubtrees: failed to assign known UUID");
		}
		ApplySubtreeRecord(record, registry, entity);
		// Reset derived transform state — recomputed by SceneGraph.
		if (auto* tf = registry.try_get<Transform>(entity))
		{
			tf->worldMatrix = glm::mat4(1.0f);
			tf->prevWorldMatrix = glm::mat4(1.0f);
			tf->dirty = true;
		}
		addsRenderable = addsRenderable || registry.all_of<MeshRef>(entity);
		created[record.uuid] = entity;
		newlyCreated.insert(record.uuid);
		result.affectedEntities.push_back(record.uuid);
	}

	// Phase 4: wire Hierarchy. Each entity's parentUuid points to either
	// nil (root) or another entity in the snapshot. Insert the entity at
	// the anchored sibling position. Skip already-present entities (the
	// idempotent path) to avoid double-inserting into the parent's children
	// list.
	for (const auto& record : snapshot.entities)
	{
		// If the entity was already present (not newly created), its
		// Hierarchy wiring is already correct — skip to avoid corruption.
		if (newlyCreated.find(record.uuid) == newlyCreated.end())
			continue;
		const auto entity = created[record.uuid];
		if (record.parentUuid.IsNull())
		{
			// Root entity — no Hierarchy parent, but may gain a Hierarchy
			// component if it has children. Skip; children wire it.
			continue;
		}
		const auto parentIt = created.find(record.parentUuid);
		const auto parent = parentIt != created.end()
			? parentIt->second : m_Authoring.FindByUuid(record.parentUuid);
		if (parent == entt::null || !registry.valid(parent))
		{
			// Parent not in snapshot and not in document — rollback.
			for (const auto& [uuid, e] : created)
			{
				if (const auto* idc = registry.try_get<EntityIdComponent>(e))
					m_Authoring.uuidIndex.Erase(idc->id);
				registry.destroy(e);
			}
			return EditorMutationResult::Failure(rt2::core::Error::MissingParent,
				record.parentUuid.ToString(),
				"RestoreSubtrees: parent UUID is not present");
		}
		auto* hierarchy = registry.try_get<Hierarchy>(entity);
		if (!hierarchy)
			hierarchy = &registry.emplace<Hierarchy>(entity);
		hierarchy->parent = parent;
		auto* parentHierarchy = registry.try_get<Hierarchy>(parent);
		if (!parentHierarchy)
			parentHierarchy = &registry.emplace<Hierarchy>(parent);
		// Find the anchored position. The anchor was validated; insert
		// between prevSibling and nextSibling.
		const auto& anchor = [&]() -> const RootSiblingAnchor& {
			for (std::size_t i = 0; i < snapshot.rootUuids.size(); ++i)
				if (snapshot.rootUuids[i] == record.uuid)
					return snapshot.rootAnchors[i];
			static RootSiblingAnchor empty;
			return empty;
		}();
		std::size_t insertPos = parentHierarchy->children.size();
		for (std::size_t i = 0; i < parentHierarchy->children.size(); ++i)
		{
			const auto* idc = registry.try_get<EntityIdComponent>(parentHierarchy->children[i]);
			if (idc && idc->id == anchor.nextSibling)
			{
				insertPos = i;
				break;
			}
		}
		parentHierarchy->children.insert(parentHierarchy->children.begin() + insertPos, entity);
	}

	// Phase 5: mark dirty and refresh camera forward directions.
	for (const auto& record : snapshot.entities)
	{
		const auto entity = created[record.uuid];
		SceneGraph::MarkDirty(registry, entity);
	}
	std::vector<entt::entity> changedEntities;
	changedEntities.reserve(snapshot.entities.size());
	for (const auto& [uuid, e] : created)
		changedEntities.push_back(e);
	RefreshCameraForwardDirections(changedEntities);

	NotifyAuthoringChanged();
	m_EntityCacheDirty = true;
	result.syncImpact = addsRenderable
		? rt2::core::SyncImpact::Structural : rt2::core::SyncImpact::None;
	return result;
}

SubtreeSnapshot SceneManager::CaptureSubtreeSnapshot(
	const std::vector<rt2::core::UUID>& rootUuids) const
{
	SubtreeSnapshot snapshot;
	if (rootUuids.empty()) return snapshot;

	rt2::core::Error error;
	auto roots = ResolveCanonicalRoots(m_Authoring, rootUuids, error);
	if (!error.IsOk())
		return snapshot;

	auto& registry = m_EcsScene.registry;
	for (const auto root : roots)
	{
		std::vector<entt::entity> subtree;
		SceneHierarchy::CollectSubtreePreOrder(registry, root, subtree);
		for (const auto entity : subtree)
			snapshot.entities.push_back(BuildSubtreeRecord(registry, entity));
		snapshot.rootUuids.push_back(GetEntityUuid({ root }));
		snapshot.rootAnchors.push_back(BuildSiblingAnchor(registry, root));
	}
	return snapshot;
}

void SceneManager::CompactMeshRegistryNow()
{
	// The host contract forbids compaction while any Undo or Redo entry
	// references resource slots. The host is responsible for calling this
	// only at history.Clear(), document adoption, or save/reload. We do
	// not have access to the history here (SceneManager never depends on
	// the command layer), so we trust the host contract. The debug assert
	// lives in the host (WalnutApp) at the call site.
	CompactMeshRegistry();
}

rt2::core::UUID SceneManager::ReserveKnownUuid()
{
	return m_UuidProvider ? m_UuidProvider->CreateV4() : rt2::core::UUID::Nil();
}

std::vector<rt2::core::UUID> SceneManager::ReserveKnownUuids(size_t count)
{
	std::vector<rt2::core::UUID> uuids;
	uuids.reserve(count);
	for (size_t i = 0; i < count; ++i)
		uuids.push_back(ReserveKnownUuid());
	return uuids;
}

rt2::core::Result<size_t> SceneManager::CountCanonicalSubtreeEntities(
	const std::vector<rt2::core::UUID>& rootUuids) const
{
	if (rootUuids.empty())
		return rt2::core::Result<size_t>::Ok(0);
	rt2::core::Error error;
	auto roots = ResolveCanonicalRoots(m_Authoring, rootUuids, error);
	if (!error.IsOk())
		return rt2::core::Result<size_t>::Fail(error.code, error.path, error.detail);

	size_t count = 0;
	auto& registry = m_EcsScene.registry;
	for (const auto root : roots)
	{
		std::vector<entt::entity> subtree;
		SceneHierarchy::CollectSubtreePreOrder(registry, root, subtree);
		count += subtree.size();
	}
	return rt2::core::Result<size_t>::Ok(count);
}

EditorMutationResult SceneManager::CreateEmptyWithUuid(
	const rt2::core::UUID& uuid,
	const std::string& name,
	const std::optional<rt2::core::UUID>& parentUuid,
	std::optional<std::size_t> siblingPosition)
{
	auto& registry = m_EcsScene.registry;
	if (m_Authoring.uuidIndex.Contains(uuid))
		return EditorMutationResult::Failure(rt2::core::Error::DuplicateUuid,
			uuid.ToString(), "CreateEmptyWithUuid: UUID already present in the document");

	entt::entity parent = entt::null;
	if (parentUuid)
	{
		parent = m_Authoring.FindByUuid(*parentUuid);
		if (parent == entt::null || !registry.valid(parent))
			return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
				parentUuid->ToString(),
				"CreateEmptyWithUuid: parent UUID is not present in the authoring scene");
	}

	const auto entity = registry.create();
	registry.emplace<Transform>(entity);
	registry.emplace<NameComponent>(entity, name.empty() ? "Empty" : name);
	registry.emplace<VisibleComponent>(entity);
	if (!m_Authoring.AssignKnownUuid(entity, uuid))
	{
		registry.destroy(entity);
		return EditorMutationResult::Failure(rt2::core::Error::DuplicateUuid,
			uuid.ToString(), "CreateEmptyWithUuid: failed to assign known UUID");
	}
	if (parent != entt::null)
	{
		registry.emplace<Hierarchy>(entity).parent = parent;
		auto* parentHierarchy = registry.try_get<Hierarchy>(parent);
		if (!parentHierarchy)
			parentHierarchy = &registry.emplace<Hierarchy>(parent);
		std::size_t insertPos = siblingPosition
			? std::min(*siblingPosition, parentHierarchy->children.size())
			: parentHierarchy->children.size();
		parentHierarchy->children.insert(parentHierarchy->children.begin() + insertPos, entity);
	}
	else if (siblingPosition)
	{
		// Root-entity ordering is unspecified; siblingPosition for a root
		// is informational only. We still honor it best-effort by leaving
		// the entity in registry-iteration order (no explicit root list).
	}
	SceneGraph::MarkDirty(registry, entity);
	NotifyAuthoringChanged();
	m_EntityCacheDirty = true;
	EditorMutationResult result;
	result.affectedEntities.push_back(uuid);
	result.syncImpact = rt2::core::SyncImpact::None; // empty entity adds no renderable
	return result;
}

EditorMutationResult SceneManager::CreatePrimitiveEntity(
	const rt2::core::UUID& uuid,
	const std::string& name,
	PrimitiveComponent::Kind kind,
	float size,
	const EditableTRS& localTRS,
	int materialIndex,
	const std::optional<rt2::core::UUID>& parentUuid)
{
	auto& registry = m_EcsScene.registry;
	if (m_Authoring.uuidIndex.Contains(uuid))
		return EditorMutationResult::Failure(rt2::core::Error::DuplicateUuid,
			uuid.ToString(), "CreatePrimitiveEntity: UUID already present in the document");

	entt::entity parent = entt::null;
	if (parentUuid)
	{
		parent = m_Authoring.FindByUuid(*parentUuid);
		if (parent == entt::null || !registry.valid(parent))
			return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
				parentUuid->ToString(),
				"CreatePrimitiveEntity: parent UUID is not present in the authoring scene");
	}

	// Build the mesh geometry and register it. The mesh slot is stable
	// while no compaction runs (3B1 invariant).
	MeshData meshData;
	switch (kind)
	{
		case PrimitiveComponent::Cube:   meshData = PrimitiveGeometry::CreateCube(size); break;
		case PrimitiveComponent::Sphere:  meshData = PrimitiveGeometry::CreateSphere(size * 0.5f); break;
		case PrimitiveComponent::Plane:  meshData = PrimitiveGeometry::CreatePlane(size); break;
		default:
			return EditorMutationResult::Failure(rt2::core::Error::UnknownPrimitive,
				uuid.ToString(), "CreatePrimitiveEntity: unknown primitive kind");
	}
	const uint32_t meshIdx = m_EcsScene.meshRegistry.AddMesh(std::move(meshData));

	const auto entity = registry.create();
	Transform tf;
	tf.translation = localTRS.translation;
	tf.rotation = localTRS.rotation;
	tf.scale = localTRS.scale;
	registry.emplace<Transform>(entity, tf);
	registry.emplace<MeshRef>(entity, meshIdx, materialIndex);
	registry.emplace<PrimitiveComponent>(entity, PrimitiveComponent{kind, size, 24, 16});
	if (!name.empty())
		registry.emplace<NameComponent>(entity, name);
	registry.emplace<VisibleComponent>(entity);
	if (!m_Authoring.AssignKnownUuid(entity, uuid))
	{
		registry.destroy(entity);
		return EditorMutationResult::Failure(rt2::core::Error::DuplicateUuid,
			uuid.ToString(), "CreatePrimitiveEntity: failed to assign known UUID");
	}
	if (parent != entt::null)
	{
		registry.emplace<Hierarchy>(entity).parent = parent;
		auto* parentHierarchy = registry.try_get<Hierarchy>(parent);
		if (!parentHierarchy)
			parentHierarchy = &registry.emplace<Hierarchy>(parent);
		parentHierarchy->children.push_back(entity);
	}
	SceneGraph::MarkDirty(registry, entity);
	NotifyAuthoringChanged();
	m_EntityCacheDirty = true;
	EditorMutationResult result;
	result.affectedEntities.push_back(uuid);
	result.syncImpact = rt2::core::SyncImpact::Structural;
	return result;
}

EditorMutationResult SceneManager::CreateLightEntity(
	const rt2::core::UUID& uuid,
	const std::string& name,
	const EditableTRS& localTRS,
	const glm::vec3& color,
	float intensity,
	LightType type,
	const std::optional<rt2::core::UUID>& parentUuid)
{
	auto& registry = m_EcsScene.registry;
	if (m_Authoring.uuidIndex.Contains(uuid))
		return EditorMutationResult::Failure(rt2::core::Error::DuplicateUuid,
			uuid.ToString(), "CreateLightEntity: UUID already present in the document");

	entt::entity parent = entt::null;
	if (parentUuid)
	{
		parent = m_Authoring.FindByUuid(*parentUuid);
		if (parent == entt::null || !registry.valid(parent))
			return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
				parentUuid->ToString(),
				"CreateLightEntity: parent UUID is not present in the authoring scene");
	}

	const auto entity = registry.create();
	Transform tf;
	tf.translation = localTRS.translation;
	tf.rotation = localTRS.rotation;
	tf.scale = localTRS.scale;
	registry.emplace<Transform>(entity, tf);
	LightComponent light;
	light.color = color;
	light.intensity = intensity;
	light.type = type;
	registry.emplace<LightComponent>(entity, light);
	if (!name.empty())
		registry.emplace<NameComponent>(entity, name);
	registry.emplace<VisibleComponent>(entity);
	if (!m_Authoring.AssignKnownUuid(entity, uuid))
	{
		registry.destroy(entity);
		return EditorMutationResult::Failure(rt2::core::Error::DuplicateUuid,
			uuid.ToString(), "CreateLightEntity: failed to assign known UUID");
	}
	if (parent != entt::null)
	{
		registry.emplace<Hierarchy>(entity).parent = parent;
		auto* parentHierarchy = registry.try_get<Hierarchy>(parent);
		if (!parentHierarchy)
			parentHierarchy = &registry.emplace<Hierarchy>(parent);
		parentHierarchy->children.push_back(entity);
	}
	SceneGraph::MarkDirty(registry, entity);
	NotifyAuthoringChanged();
	m_EntityCacheDirty = true;
	EditorMutationResult result;
	result.affectedEntities.push_back(uuid);
	result.syncImpact = rt2::core::SyncImpact::Structural;
	return result;
}

SceneManager::DuplicationResult SceneManager::DuplicateSubtreesWithUuids(
	const std::vector<rt2::core::UUID>& sourceRoots,
	const std::vector<rt2::core::UUID>& knownDuplicateUuids)
{
	DuplicationResult out;
	if (sourceRoots.empty()) return out;
	rt2::core::Error error;
	auto roots = ResolveCanonicalRoots(m_Authoring, sourceRoots, error);
	if (!error.IsOk())
	{
		out.mutation.success = false;
		out.mutation.error = error;
		return out;
	}
	auto& registry = m_EcsScene.registry;

	// Walk each canonical subtree in deterministic pre-order and collect
	// the source entities. This is the SAME pre-order the manager uses
	// internally to assign UUIDs positionally.
	std::vector<entt::entity> sources;
	for (const auto root : roots)
		SceneHierarchy::CollectSubtreePreOrder(registry, root, sources);

	// Validate the UUID count exactly matches the resulting entity count.
	if (knownDuplicateUuids.size() != sources.size())
	{
		out.mutation = EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
			{}, "DuplicateSubtreesWithUuids: known UUID count does not match the canonical subtree size");
		return out;
	}

	// Validate all supplied UUIDs are valid/unique/absent from the document.
	std::unordered_set<rt2::core::UUID> seen;
	for (const auto& uuid : knownDuplicateUuids)
	{
		if (uuid.IsNull() || m_Authoring.uuidIndex.Contains(uuid) || !seen.insert(uuid).second)
		{
			out.mutation = EditorMutationResult::Failure(rt2::core::Error::DuplicateUuid,
				uuid.ToString(), "DuplicateSubtreesWithUuids: known UUID is nil, duplicate, or already present");
			return out;
		}
	}

	// Build the complete duplication plan before mutating.
	std::unordered_map<entt::entity, entt::entity> remap;
	std::vector<std::pair<rt2::core::UUID, rt2::core::UUID>> sourceToDuplicate;
	sourceToDuplicate.reserve(sources.size());
	for (std::size_t i = 0; i < sources.size(); ++i)
	{
		const auto source = sources[i];
		const auto duplicate = registry.create();
		remap.emplace(source, duplicate);
		CopyAuthoredComponents(registry, source, registry, duplicate);
		if (!m_Authoring.AssignKnownUuid(duplicate, knownDuplicateUuids[i]))
		{
			// Rollback: destroy everything we created so far.
			for (const auto& [s, d] : remap)
			{
				if (const auto* idc = registry.try_get<EntityIdComponent>(d))
					m_Authoring.uuidIndex.Erase(idc->id);
				registry.destroy(d);
			}
			out.mutation = EditorMutationResult::Failure(rt2::core::Error::DuplicateUuid,
				knownDuplicateUuids[i].ToString(),
				"DuplicateSubtreesWithUuids: failed to assign known UUID");
			return out;
		}
		const auto* sourceIdc = registry.try_get<EntityIdComponent>(source);
		sourceToDuplicate.emplace_back(sourceIdc ? sourceIdc->id : rt2::core::UUID{},
		                               knownDuplicateUuids[i]);
	}

	// Wire Hierarchy among duplicates.
	bool duplicatesRenderable = false;
	for (const auto source : sources)
	{
		const auto duplicate = remap.at(source);
		duplicatesRenderable = duplicatesRenderable || registry.all_of<MeshRef>(duplicate);
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

	// Append " Copy" to duplicate root names and collect created roots.
	for (const auto root : roots)
	{
		const auto duplicate = remap.at(root);
		if (auto* name = registry.try_get<NameComponent>(duplicate))
			name->name += " Copy";
		out.createdRoots.push_back(GetEntityUuid({ duplicate }));
		SceneGraph::MarkDirty(registry, duplicate);
	}

	NotifyAuthoringChanged();
	m_EntityCacheDirty = true;
	out.mutation.syncImpact = duplicatesRenderable
		? rt2::core::SyncImpact::Structural : rt2::core::SyncImpact::None;
	out.mutation.success = true;
	for (const auto root : roots)
		out.mutation.affectedEntities.push_back(GetEntityUuid({ remap.at(root) }));
	out.sourceToDuplicate = std::move(sourceToDuplicate);
	return out;
}

SceneManager::DuplicationResult SceneManager::PasteSubtreesWithUuids(
	const rt2::core::SceneDocument& clipboard,
	const std::vector<rt2::core::UUID>& clipboardRoots,
	const std::optional<rt2::core::UUID>& parentUuid,
	const std::vector<rt2::core::UUID>& knownPastedUuids)
{
	DuplicationResult out;
	if (clipboardRoots.empty()) return out;
	rt2::core::Error error;
	auto roots = ResolveCanonicalRoots(clipboard, clipboardRoots, error);
	if (!error.IsOk())
	{
		out.mutation.success = false;
		out.mutation.error = error;
		return out;
	}
	auto& destination = m_EcsScene.registry;
	entt::entity destinationParent = entt::null;
	if (parentUuid)
	{
		destinationParent = m_Authoring.FindByUuid(*parentUuid);
		if (destinationParent == entt::null || !destination.valid(destinationParent))
		{
			out.mutation = EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
				parentUuid->ToString(), "paste parent is not present in the authoring scene");
			return out;
		}
	}

	// Validate clipboard mesh/material resources still match this document.
	std::vector<entt::entity> sources;
	for (const auto root : roots)
		SceneHierarchy::CollectSubtreePreOrder(clipboard.ecs.registry, root, sources);
	for (const auto source : sources)
	{
		if (const auto* mesh = clipboard.ecs.registry.try_get<MeshRef>(source))
		{
			if (mesh->meshIndex >= m_EcsScene.meshRegistry.GetCount())
			{
				out.mutation = EditorMutationResult::Failure(rt2::core::Error::ClipboardStale,
					{}, "clipboard mesh resources no longer match this document");
				return out;
			}
			if (mesh->materialIndex >= static_cast<int>(m_EcsScene.materials.size()))
			{
				out.mutation = EditorMutationResult::Failure(rt2::core::Error::ClipboardStale,
					{}, "clipboard material resources no longer match this document");
				return out;
			}
		}
	}

	// Validate the UUID count exactly matches the resulting entity count.
	if (knownPastedUuids.size() != sources.size())
	{
		out.mutation = EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
			{}, "PasteSubtreesWithUuids: known UUID count does not match the canonical subtree size");
		return out;
	}

	// Validate all supplied UUIDs are valid/unique/absent from the document.
	std::unordered_set<rt2::core::UUID> seen;
	for (const auto& uuid : knownPastedUuids)
	{
		if (uuid.IsNull() || m_Authoring.uuidIndex.Contains(uuid) || !seen.insert(uuid).second)
		{
			out.mutation = EditorMutationResult::Failure(rt2::core::Error::DuplicateUuid,
				uuid.ToString(), "PasteSubtreesWithUuids: known UUID is nil, duplicate, or already present");
			return out;
		}
	}

	// Build the complete paste plan before mutating.
	std::unordered_map<entt::entity, entt::entity> remap;
	std::vector<std::pair<rt2::core::UUID, rt2::core::UUID>> sourceToDuplicate;
	sourceToDuplicate.reserve(sources.size());
	bool pastesRenderable = false;
	for (std::size_t i = 0; i < sources.size(); ++i)
	{
		const auto source = sources[i];
		const auto pasted = destination.create();
		remap.emplace(source, pasted);
		CopyAuthoredComponents(clipboard.ecs.registry, source, destination, pasted);
		if (!m_Authoring.AssignKnownUuid(pasted, knownPastedUuids[i]))
		{
			// Rollback.
			for (const auto& [s, d] : remap)
			{
				if (const auto* idc = destination.try_get<EntityIdComponent>(d))
					m_Authoring.uuidIndex.Erase(idc->id);
				destination.destroy(d);
			}
			out.mutation = EditorMutationResult::Failure(rt2::core::Error::DuplicateUuid,
				knownPastedUuids[i].ToString(),
				"PasteSubtreesWithUuids: failed to assign known UUID");
			return out;
		}
		pastesRenderable = pastesRenderable || destination.all_of<MeshRef>(pasted);
		const auto* sourceIdc = clipboard.ecs.registry.try_get<EntityIdComponent>(source);
		sourceToDuplicate.emplace_back(sourceIdc ? sourceIdc->id : rt2::core::UUID{},
		                                knownPastedUuids[i]);
	}

	// Wire Hierarchy among pastes and to the destination parent.
	for (const auto source : sources)
	{
		const auto pasted = remap.at(source);
		const auto* sourceHierarchy = clipboard.ecs.registry.try_get<Hierarchy>(source);
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

	// Append " Copy" to paste root names and collect created roots.
	for (const auto root : roots)
	{
		const auto pasted = remap.at(root);
		if (auto* name = destination.try_get<NameComponent>(pasted))
			name->name += " Copy";
		out.createdRoots.push_back(GetEntityUuid({ pasted }));
		SceneGraph::MarkDirty(destination, pasted);
	}

	NotifyAuthoringChanged();
	m_EntityCacheDirty = true;
	out.mutation.syncImpact = pastesRenderable
		? rt2::core::SyncImpact::Structural : rt2::core::SyncImpact::None;
	out.mutation.success = true;
	for (const auto root : roots)
		out.mutation.affectedEntities.push_back(GetEntityUuid({ remap.at(root) }));
	out.sourceToDuplicate = std::move(sourceToDuplicate);
	return out;
}

EditorMutationResult SceneManager::SetLocalTransformStates(
	const std::vector<std::pair<rt2::core::UUID, EditableTRS>>& states)
{
	if (states.empty()) return {};
	auto& registry = m_EcsScene.registry;

	// Validate ALL UUIDs resolve first. Any failure => zero mutation.
	std::vector<std::pair<entt::entity, EditableTRS>> resolved;
	resolved.reserve(states.size());
	for (const auto& [uuid, trs] : states)
	{
		const auto entity = m_Authoring.FindByUuid(uuid);
		if (entity == entt::null || !registry.valid(entity))
			return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
				uuid.ToString(),
				"SetLocalTransformStates: entity UUID is not present in the authoring scene");
		resolved.emplace_back(entity, trs);
	}

	// Apply all local TRS in one pass.
	for (const auto& [entity, trs] : resolved)
	{
		if (auto* tf = registry.try_get<Transform>(entity))
		{
			tf->translation = trs.translation;
			tf->rotation = glm::normalize(trs.rotation);
			tf->scale = trs.scale;
			SceneGraph::MarkDirty(registry, entity);
		}
	}

	std::vector<entt::entity> changedEntities;
	changedEntities.reserve(resolved.size());
	EditorMutationResult result;
	result.syncImpact = rt2::core::SyncImpact::Transform;
	for (const auto& [entity, trs] : resolved)
	{
		changedEntities.push_back(entity);
		if (const auto* idc = registry.try_get<EntityIdComponent>(entity))
			result.affectedEntities.push_back(idc->id);
	}
	RefreshCameraForwardDirections(changedEntities);
	NotifyAuthoringChanged();
	return result;
}

EditorMutationResult SceneManager::ReparentBatch(
	const std::vector<ReparentEdit>& edits, ReparentMode mode)
{
	if (edits.empty()) return {};
	auto& registry = m_EcsScene.registry;

	// Phase 1: validate all entities and all new parents resolve, and no
	// cycles. Any failure => zero mutation.
	std::vector<entt::entity> entities;
	std::vector<entt::entity> newParents;
	entities.reserve(edits.size());
	newParents.reserve(edits.size());
	for (const auto& edit : edits)
	{
		const auto entity = m_Authoring.FindByUuid(edit.entity);
		if (entity == entt::null || !registry.valid(entity))
			return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
				edit.entity.ToString(),
				"ReparentBatch: entity UUID is not present in the authoring scene");
		entt::entity newParent = entt::null;
		if (!edit.newParent.IsNull())
		{
			newParent = m_Authoring.FindByUuid(edit.newParent);
			if (newParent == entt::null || !registry.valid(newParent))
				return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
					edit.newParent.ToString(),
					"ReparentBatch: new parent UUID is not present in the authoring scene");
		}
		if (newParent != entt::null && SceneHierarchy::IsDescendant(registry, entity, newParent))
			return EditorMutationResult::Failure(rt2::core::Error::HierarchyCycle,
				edit.entity.ToString(),
				"ReparentBatch: cannot parent an entity beneath itself or a descendant");
		entities.push_back(entity);
		newParents.push_back(newParent);
	}

	// Phase 1b: batch-cycle validation. The per-edit check above validates
	// each entity against the PRE-batch hierarchy. A batch like {A→under B,
	// B→under A} passes per-edit validation (neither is a descendant of the
	// other yet) but creates a cycle. Build the planned parent map and
	// check that following the planned parent chain from each entity never
	// revisits an entity in the batch.
	{
		std::unordered_map<entt::entity, entt::entity> plannedParent;
		for (std::size_t i = 0; i < edits.size(); ++i)
			plannedParent[entities[i]] = newParents[i];
		for (std::size_t i = 0; i < edits.size(); ++i)
		{
			std::unordered_set<entt::entity> visited;
			visited.insert(entities[i]);
			entt::entity cursor = newParents[i];
			while (cursor != entt::null)
			{
				if (!visited.insert(cursor).second)
				{
					// Cycle detected through the planned parent chain.
					return EditorMutationResult::Failure(rt2::core::Error::HierarchyCycle,
						edits[i].entity.ToString(),
						"ReparentBatch: batch creates a cycle through the planned parent map");
				}
				// Follow the planned parent if this entity is in the batch;
				// otherwise follow the live hierarchy.
				auto it = plannedParent.find(cursor);
				if (it != plannedParent.end())
					cursor = it->second;
				else if (const auto* h = registry.try_get<Hierarchy>(cursor))
					cursor = h->parent;
				else
					cursor = entt::null;
			}
		}
	}

	// Phase 2: for PreserveWorld, convert each desired world matrix to
	// local against the NEW parent (singular/shear => fail all). Uses the
	// world matrix stored in the ReparentEdit (captured at construction
	// time) so the command is self-contained and not dependent on live
	// state.
	std::vector<EditableTRS> newLocals;
	if (mode == ReparentMode::PreserveWorld)
	{
		UpdateWorldTransforms();
		newLocals.reserve(edits.size());
		for (std::size_t i = 0; i < edits.size(); ++i)
		{
			const auto entity = entities[i];
			const auto newParent = newParents[i];
			glm::mat4 parentWorld(1.0f);
			if (newParent != entt::null)
			{
				const auto* parentTransform = registry.try_get<Transform>(newParent);
				if (!parentTransform)
					return EditorMutationResult::Failure(rt2::core::Error::InvalidTransform,
						edits[i].newParent.ToString(),
						"ReparentBatch: new parent has no transform");
				parentWorld = parentTransform->worldMatrix;
			}
			// Use the world matrix stored in the edit (captured at
			// construction time) rather than the live transform's
			// worldMatrix. This keeps the command self-contained.
			(void)entity; // entity validity already validated in Phase 1.
			EditableTRS local;
			if (!TryWorldToLocalTRS(parentWorld, edits[i].worldMatrix, local))
				return EditorMutationResult::Failure(rt2::core::Error::InvalidTransform,
					edits[i].entity.ToString(),
					"ReparentBatch: preserve-world reparent produced a singular or sheared transform");
			newLocals.push_back(local);
		}
	}
	else
	{
		newLocals.reserve(edits.size());
		for (const auto& edit : edits)
			newLocals.push_back(edit.localTRS);
	}

	// Phase 3: apply all reparents atomically. Remove each entity from its
	// old parent's children list, set the new parent, and insert at the
	// anchored sibling position (using ReparentEdit.anchor). For
	// after-edits the anchor is typically empty (append to the end); for
	// before-edits (Undo) the anchor restores the exact original position.
	for (std::size_t i = 0; i < edits.size(); ++i)
	{
		const auto entity = entities[i];
		const auto newParent = newParents[i];
		const auto& anchor = edits[i].anchor;
		auto* hierarchy = registry.try_get<Hierarchy>(entity);
		RemoveChild(registry, hierarchy ? hierarchy->parent : entt::null, entity);
		if (!hierarchy)
			hierarchy = &registry.emplace<Hierarchy>(entity);
		hierarchy->parent = newParent;
		if (newParent != entt::null)
		{
			auto* parentHierarchy = registry.try_get<Hierarchy>(newParent);
			if (!parentHierarchy)
				parentHierarchy = &registry.emplace<Hierarchy>(newParent);
			// Find the insertion position from the anchor's nextSibling.
			// Insert before nextSibling when resolvable; otherwise append.
			std::size_t insertPos = parentHierarchy->children.size();
			if (!anchor.nextSibling.IsNull())
			{
				for (std::size_t j = 0; j < parentHierarchy->children.size(); ++j)
				{
					const auto* idc = registry.try_get<EntityIdComponent>(parentHierarchy->children[j]);
					if (idc && idc->id == anchor.nextSibling)
					{
						insertPos = j;
						break;
					}
				}
			}
			parentHierarchy->children.insert(parentHierarchy->children.begin() + insertPos, entity);
		}
	}

	// Phase 4: apply the new local TRS.
	for (std::size_t i = 0; i < edits.size(); ++i)
	{
		auto& transform = registry.get<Transform>(entities[i]);
		transform.translation = newLocals[i].translation;
		transform.rotation = glm::normalize(newLocals[i].rotation);
		transform.scale = newLocals[i].scale;
		SceneGraph::MarkDirty(registry, entities[i]);
	}

	std::vector<entt::entity> changedEntities(entities.begin(), entities.end());
	RefreshCameraForwardDirections(changedEntities);
	NotifyAuthoringChanged();
	m_EntityCacheDirty = true;
	EditorMutationResult result;
	result.syncImpact = rt2::core::SyncImpact::Transform;
	for (const auto entity : entities)
		result.affectedEntities.push_back(GetEntityUuid({ entity }));
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
	// SetMaterialIndexState returns an authoritative EditorMutationResult and
	// can legitimately fail — an out-of-range index is the common case, since
	// it is bounds-checked against the material list. Dropping that result
	// made a rejected assignment indistinguishable from an applied one.
	// This wrapper is void by design (the inspector calls it fire-and-forget),
	// so surface the failure rather than returning it.
	const auto result = SetMaterialIndexState(GetEntityUuid(entity), materialIndex);
	if (!result.success)
		printf("[SceneManager] SetMaterial rejected: %s\n",
		       result.error.Format().c_str());
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
	SetEntityNameState(GetEntityUuid(entity), name);
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

bool SceneManager::HasScript(EntityId entity) const
{
	if (!entity.IsValid()) return false;
	if (!m_EcsScene.registry.valid(entity.id)) return false;
	return m_EcsScene.registry.try_get<ScriptComponent>(entity.id) != nullptr;
}

std::optional<ScriptComponent>
SceneManager::GetScriptState(const rt2::core::UUID& entity) const
{
	const auto e = m_Authoring.FindByUuid(entity);
	if (e == entt::null || !m_EcsScene.registry.valid(e))
		return std::nullopt;
	if (auto* sc = m_EcsScene.registry.try_get<ScriptComponent>(e))
		return *sc;
	return std::nullopt;
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

bool SceneManager::GetLightProperties(EntityId entity, glm::vec3& outColor, float& outIntensity, LightType& outType) const
{
	if (!entity.IsValid()) return false;
	if (!m_EcsScene.registry.valid(entity.id)) return false;
	auto* light = m_EcsScene.registry.try_get<LightComponent>(entity.id);
	if (!light) return false;
	outColor = light->color;
	outIntensity = light->intensity;
	outType = light->type;
	return true;
}

void SceneManager::SetLightProperties(EntityId entity, const glm::vec3& color, float intensity, LightType type)
{
	if (!entity.IsValid()) return;
	LightComponent value;
	if (auto* light = m_EcsScene.registry.try_get<LightComponent>(entity.id))
		value = *light;
	value.color = color;
	value.intensity = intensity;
	value.type = type;
	SetLightPropertiesState(GetEntityUuid(entity), value);
}

bool SceneManager::GetLightComponent(EntityId entity, LightComponent& outValue) const
{
	if (!entity.IsValid()) return false;
	if (!m_EcsScene.registry.valid(entity.id)) return false;
	auto* light = m_EcsScene.registry.try_get<LightComponent>(entity.id);
	if (!light) return false;
	outValue = *light;
	return true;
}

void SceneManager::SetLightComponent(EntityId entity, const LightComponent& value)
{
	if (!entity.IsValid()) return;
	SetLightPropertiesState(GetEntityUuid(entity), value);
}

bool SceneManager::SetCameraProperties(EntityId entity, float verticalFOV,
	float aperture, float focusDistance)
{
	if (!entity.IsValid() || !m_EcsScene.registry.valid(entity.id)) return false;
	CameraComponent value;
	if (auto* camera = m_EcsScene.registry.try_get<CameraComponent>(entity.id))
		value = *camera;
	else
		return false;
	value.verticalFOV = verticalFOV;
	value.aperture = aperture;
	value.focusDistance = focusDistance;
	const auto result = SetCameraPropertiesState(GetEntityUuid(entity), value);
	return result.success;
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
	SetMaterialPropertiesState(index, props);
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
// Phase 3B2 atomic property state APIs. Each applies the after-value, bumps
// the revision ONCE, and returns an authoritative EditorMutationResult.
// Material APIs also capture/restore MaterialOverrideComponent atomically so
// Undo of an imported-entity material assignment does not leave a stale
// override that save/reopen would resurrect.
// ============================================================================

EditorMutationResult SceneManager::SetEntityNameState(const rt2::core::UUID& entity,
                                                      const std::string& name)
{
	const auto e = m_Authoring.FindByUuid(entity);
	if (e == entt::null || !m_EcsScene.registry.valid(e))
		return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
			entity.ToString(), "SetEntityNameState: entity not present");
	if (auto* nc = m_EcsScene.registry.try_get<NameComponent>(e))
		nc->name = name;
	else
		m_EcsScene.registry.emplace<NameComponent>(e, name);
	NotifyAuthoringChanged();
	EditorMutationResult result;
	result.success = true;
	result.syncImpact = rt2::core::SyncImpact::None;
	result.affectedEntities.push_back(entity);
	return result;
}

EditorMutationResult SceneManager::SetLightPropertiesState(const rt2::core::UUID& entity,
                                                           const LightComponent& value)
{
	const auto e = m_Authoring.FindByUuid(entity);
	if (e == entt::null || !m_EcsScene.registry.valid(e))
		return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
			entity.ToString(), "SetLightPropertiesState: entity not present");
	auto* light = m_EcsScene.registry.try_get<LightComponent>(e);
	if (!light)
		return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
			entity.ToString(), "SetLightPropertiesState: entity has no LightComponent");
	*light = value;
	NotifyAuthoringChanged();
	EditorMutationResult result;
	result.success = true;
	result.syncImpact = rt2::core::SyncImpact::Material; // keep-textures path
	result.affectedEntities.push_back(entity);
	return result;
}

EditorMutationResult SceneManager::SetCameraPropertiesState(const rt2::core::UUID& entity,
                                                            const CameraComponent& value)
{
	const auto e = m_Authoring.FindByUuid(entity);
	if (e == entt::null || !m_EcsScene.registry.valid(e))
		return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
			entity.ToString(), "SetCameraPropertiesState: entity not present");
	auto* camera = m_EcsScene.registry.try_get<CameraComponent>(e);
	if (!camera)
		return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
			entity.ToString(), "SetCameraPropertiesState: entity has no CameraComponent");
	*camera = value;
	NotifyAuthoringChanged();
	EditorMutationResult result;
	result.success = true;
	result.syncImpact = rt2::core::SyncImpact::None;
	result.affectedEntities.push_back(entity);
	return result;
}

EditorMutationResult SceneManager::SetMaterialPropertiesState(int slotIndex,
                                                              const SceneMaterial& value)
{
	if (slotIndex < 0 || slotIndex >= (int)m_EcsScene.materials.size())
		return EditorMutationResult::Failure(rt2::core::Error::InvalidArgument,
			std::to_string(slotIndex), "SetMaterialPropertiesState: slot index out of range");
	m_EcsScene.materials[slotIndex] = value;

	// Propagate the edit into durable MaterialOverrideComponent on every
	// imported entity whose MeshRef points at this material slot, so saved
	// material edits survive reopen.
	{
		auto& reg = m_EcsScene.registry;
		auto view = reg.view<ImportedMeshSourceComponent>();
		for (auto e : view)
		{
			auto* ref = reg.try_get<MeshRef>(e);
			if (ref && ref->materialIndex == slotIndex)
				RecordMaterialOverride(e, slotIndex);
		}
	}

	NotifyAuthoringChanged();
	EditorMutationResult result;
	result.success = true;
	result.syncImpact = rt2::core::SyncImpact::Material;
	return result;
}

EditorMutationResult SceneManager::SetMaterialIndexState(const rt2::core::UUID& entity,
                                                         int afterIndex)
{
	const auto e = m_Authoring.FindByUuid(entity);
	if (e == entt::null || !m_EcsScene.registry.valid(e))
		return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
			entity.ToString(), "SetMaterialIndexState: entity not present");
	auto* ref = m_EcsScene.registry.try_get<MeshRef>(e);
	if (!ref)
		return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
			entity.ToString(), "SetMaterialIndexState: entity has no MeshRef");
	if (afterIndex < 0 || afterIndex >= (int)m_EcsScene.materials.size())
		return EditorMutationResult::Failure(rt2::core::Error::InvalidArgument,
			std::to_string(afterIndex), "SetMaterialIndexState: material index out of range");
	ref->materialIndex = afterIndex;
	if (m_EcsScene.registry.all_of<ImportedMeshSourceComponent>(e))
		RecordMaterialOverride(e, afterIndex);
	NotifyAuthoringChanged();
	EditorMutationResult result;
	result.success = true;
	result.syncImpact = rt2::core::SyncImpact::Material;
	result.affectedEntities.push_back(entity);
	return result;
}

EditorMutationResult SceneManager::SetMotionState(const rt2::core::UUID& entity,
                                                  const std::optional<MotionComponent>& value)
{
	const auto e = m_Authoring.FindByUuid(entity);
	if (e == entt::null || !m_EcsScene.registry.valid(e))
		return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
			entity.ToString(), "SetMotionState: entity not present");
	if (value.has_value())
		m_EcsScene.registry.emplace_or_replace<MotionComponent>(e, *value);
	else
	{
		if (m_EcsScene.registry.all_of<MotionComponent>(e))
			m_EcsScene.registry.remove<MotionComponent>(e);
	}
	NotifyAuthoringChanged();
	EditorMutationResult result;
	result.success = true;
	result.syncImpact = rt2::core::SyncImpact::None;
	result.affectedEntities.push_back(entity);
	return result;
}

EditorMutationResult SceneManager::SetScriptState(const rt2::core::UUID& entity,
                                                  const std::optional<ScriptComponent>& value)
{
	const auto e = m_Authoring.FindByUuid(entity);
	if (e == entt::null || !m_EcsScene.registry.valid(e))
		return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
			entity.ToString(), "SetScriptState: entity not present");
	if (value.has_value())
	{
		ScriptComponent canonical;
		std::string detail;
		std::string field;
		if (!rt2::core::NormalizeAndValidateScriptComponent(
				*value, canonical, detail, &field))
		{
			return EditorMutationResult::Failure(
				rt2::core::Error::InvalidArgument,
				field.empty() ? entity.ToString()
				              : entity.ToString() + ":" + field,
				"SetScriptState: " + detail);
		}

		// Suppress canonical no-ops: present→same-present must not dirty the
		// document, bump the revision, or notify observers (W4-F1). The
		// caller's value has already been canonicalized above, so compare
		// against the currently stored component.
		std::optional<ScriptComponent> current;
		if (m_EcsScene.registry.all_of<ScriptComponent>(e))
			current = m_EcsScene.registry.get<ScriptComponent>(e);

		// Binding a script is its explicit import operation. Assign/reuse the
		// per-asset sidecar ID here, matching model/environment import while
		// keeping the shared locator read-only (W3-Q9). A changed path never
		// carries the previous file's ID. Missing paths remain bindable so the
		// Phase 6 quarantine and watcher-recovery behavior is preserved.
		if (canonical.asset.path.empty())
		{
			canonical.asset.assetId = rt2::core::UUID::Nil();
		}
		else
		{
			const bool sameBinding = current.has_value() &&
				current->asset.path == canonical.asset.path;
			if (!sameBinding)
				canonical.asset.assetId = rt2::core::UUID::Nil();
			else if (canonical.asset.assetId.IsNull())
				canonical.asset.assetId = current->asset.assetId;

			rt2::core::AssetResolutionContext context = m_AssetResolutionContext;
			if (context.assetRoot.empty() &&
				!m_Authoring.metadata.sourcePath.empty())
				context.assetRoot = m_Authoring.metadata.sourcePath.
					parent_path().lexically_normal();
			std::vector<rt2::core::AssetDiagnostic> diagnostics;
			const auto resolved = rt2::core::ResolveScriptAssetPath(
				canonical, context, entity,
				GetEntityName({ e }), diagnostics);

			const bool conflict = std::any_of(
				diagnostics.begin(), diagnostics.end(),
				[](const rt2::core::AssetDiagnostic& diagnostic) {
					return diagnostic.severity ==
						rt2::core::AssetDiagnostic::Conflict;
				});
			if (conflict)
			{
				return EditorMutationResult::Failure(
					rt2::core::Error::InvalidArgument,
					canonical.asset.path,
					"SetScriptState: script asset identity conflict");
			}

			if (resolved.success &&
				!resolved.effectiveId.IsNull())
				canonical.asset.assetId = resolved.effectiveId;

			if (resolved.success &&
				resolved.identityRepairRequired &&
				canonical.asset.assetId.IsNull())
			{
				bool minted = false;
				rt2::core::Error idError;
				canonical.asset.assetId = rt2::core::ResolveOrAssign(
					resolved.resolvedPath, *m_UuidProvider,
					minted, idError);
				if (minted || !idError.IsOk())
				{
					printf("[Asset] %s: assigned script id %s%s%s\n",
					       resolved.resolvedPath.string().c_str(),
					       canonical.asset.assetId.ToString().c_str(),
					       idError.IsOk() ? "" : ": ",
					       idError.IsOk() ? "" : idError.Format().c_str());
					fflush(stdout);
				}
			}
		}

		if (rt2::core::ScriptComponentCanonicalEqual(
				current, std::optional<ScriptComponent>{canonical}))
		{
			EditorMutationResult result;
			result.success = true;
			result.effective = false;
			result.syncImpact = rt2::core::SyncImpact::None;
			return result;
		}

		m_EcsScene.registry.emplace_or_replace<ScriptComponent>(e,
			std::move(canonical));
	}
	else
	{
		// Suppress absent→absent removal (W4-F1).
		if (!m_EcsScene.registry.all_of<ScriptComponent>(e))
		{
			EditorMutationResult result;
			result.success = true;
			result.effective = false;
			result.syncImpact = rt2::core::SyncImpact::None;
			return result;
		}
		m_EcsScene.registry.remove<ScriptComponent>(e);
	}
	NotifyAuthoringChanged();
	EditorMutationResult result;
	result.success = true;
	// D8: script bindings and field values never reach the GPU scene.
	result.syncImpact = rt2::core::SyncImpact::None;
	result.affectedEntities.push_back(entity);
	return result;
}

EditorMutationResult SceneManager::SetCameraPoseState(const rt2::core::UUID& entity,
                                                      const EditableTRS& local,
                                                      const CameraComponent& props)
{
	const auto e = m_Authoring.FindByUuid(entity);
	if (e == entt::null || !m_EcsScene.registry.valid(e))
		return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
			entity.ToString(), "SetCameraPoseState: entity not present");
	if (!m_EcsScene.registry.all_of<Transform, CameraComponent>(e))
		return EditorMutationResult::Failure(rt2::core::Error::InvalidEntity,
			entity.ToString(), "SetCameraPoseState: entity is not a camera");

	auto& tf = m_EcsScene.registry.get<Transform>(e);
	tf.translation = local.translation;
	tf.rotation = glm::normalize(local.rotation);
	tf.scale = local.scale;
	SceneGraph::MarkDirty(m_EcsScene.registry, e);

	auto& camera = m_EcsScene.registry.get<CameraComponent>(e);
	camera = props;

	RefreshCameraForwardDirections({ e });
	NotifyAuthoringChanged();

	EditorMutationResult result;
	result.success = true;
	result.syncImpact = rt2::core::SyncImpact::Transform;
	result.affectedEntities.push_back(entity);
	return result;
}

std::optional<MaterialOverrideComponent> SceneManager::GetMaterialOverride(
	const rt2::core::UUID& entity) const
{
	const auto e = m_Authoring.FindByUuid(entity);
	if (e == entt::null || !m_EcsScene.registry.valid(e))
		return std::nullopt;
	const auto* ov = m_EcsScene.registry.try_get<MaterialOverrideComponent>(e);
	if (!ov)
		return std::nullopt;
	return *ov;
}

void SceneManager::InstallMaterialOverride(
	const rt2::core::UUID& entity,
	const std::optional<MaterialOverrideComponent>& override)
{
	const auto e = m_Authoring.FindByUuid(entity);
	if (e == entt::null || !m_EcsScene.registry.valid(e))
		return;
	if (override.has_value())
		m_EcsScene.registry.emplace_or_replace<MaterialOverrideComponent>(e, *override);
	else if (m_EcsScene.registry.all_of<MaterialOverrideComponent>(e))
		m_EcsScene.registry.remove<MaterialOverrideComponent>(e);
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

	// Find which mesh indices are still referenced by alive entities.
	//
	// A MeshRef may name an index the registry does not have — a stale
	// reference left by an entity built before its mesh was added, or one
	// that outlived a registry shrink. Such an index must NOT be treated as
	// live: it would flow into GetMesh() below, which is an unchecked
	// m_Meshes[index], and read out of bounds (SIGSEGV in Release, a
	// __fastfail on Debug's iterator checks). Skipping it is also correct
	// on the merits — a mesh that does not exist cannot be keeping anything
	// alive. GPUSceneData applies the same guard before indexing meshes.
	std::set<uint32_t> referenced;
	auto view = reg.view<MeshRef>();
	for (auto entity : view)
	{
		if (!reg.valid(entity)) continue;
		const auto& ref = view.get<MeshRef>(entity);
		if (ref.meshIndex >= meshReg.GetCount()) continue;
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

		IndexRebase rebase;
		rebase.mesh.SetRemap(remap);
		RebaseIndices(m_EcsScene, rebase);

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

		IndexRebase rebase;
		rebase.material.SetRemap(matRemap);
		RebaseIndices(m_EcsScene, rebase);

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

		IndexRebase rebase;
		rebase.texture.SetRemap(texRemap);
		RebaseIndices(m_EcsScene, rebase);

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
