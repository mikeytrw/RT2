#pragma once

#ifndef RT2_CORE_SCENE_DOCUMENT_H
#define RT2_CORE_SCENE_DOCUMENT_H

#include "ECSScene.h"
#include "GPUSceneData.h"
#include "AssetReference.h"
#include "core/UUID.h"
#include "core/Error.h"

#include <string>
#include <unordered_map>
#include <vector>
#include <filesystem>

// ============================================================================
// SceneDocument — the serializable, cloneable unit of scene state.
//
// RT2's active scene state was historically split across ECSScene (entities,
// meshes, materials, textures, lights, camera) and SceneManager-private
// environment state. SceneDocument unifies them so that the serializer, the
// runtime clone, and the slice runner operate on one self-contained object
// instead of a raw ECSScene plus loose manager state.
//
// Members:
//   ecs          — entity-component scene (registry, mesh registry, materials,
//                  textures, lights, camera).
//   environment  — env map path + cached HDR pixels + dimensions. Pixels are
//                  CPU-side float data; the renderer uploads them. The native
//                  .rt2scene format persists the path only and re-reads pixels
//                  on load, so the slice runner does not need to decode an
//                  EXR to validate scene lifecycle.
//   metadata     — schema version, source path, dirty flag, scene name.
//   uuidIndex    — UUID -> entt::entity map, maintained alongside ecs so it
//                  is never stale for a cloned or loaded document.
//   gpuCache     — last GPUSceneData built from this document. CPU DTO only;
//                  no Vulkan types. Cached so transform-only sync can update
//                  instances in place.
//
// Transient renderer state (prevWorldMatrix history, temporal reservoirs,
// script VM state, physics contacts, audio handles) is NOT part of the
// document. RuntimeSceneController initializes runtime-only state after
// cloning the authoring document into the runtime document on Play().
//
// ============================================================================

namespace rt2::core {

struct EnvironmentSettings
{
    // Stable source-asset identity (Phase 7 W3 step 4, converted to a real
    // AssetReference in the step-4 remediation per W3-Q1/D2). The reference's
    // `kind` is AssetKind::Environment; `path` is the env map path (absolute
    // or project-relative, empty = none); `assetId` is the durable source
    // identity (additive over the v3 env-map schema: absent on read, written
    // only when assigned). `width`/`height`/`floatPixels` are the decoded
    // pixel cache, never serialized. The host assigns the ID at env import
    // (SceneManager::LoadEnvMap/SetEnvMapData) via ResolveOrAssign,
    // paralleling model assetId. W5 owns the formal v4 migration/reporting
    // pass.
    AssetReference   ref{ AssetKind::Environment, {}, {}, {}, UUID::Nil() };
    int             width = 0;
    int             height = 0;
    std::vector<float> floatPixels;  // RGBA float, decoded; may be empty when
                                     // the document was loaded without re-reading
                                     // the env file (e.g. slice runner)
    bool HasEnvMap() const { return !ref.path.empty(); }

    void Clear()
    {
        ref = AssetReference{ AssetKind::Environment, {}, {}, {}, UUID::Nil() };
        width = 0;
        height = 0;
        floatPixels.clear();
    }
};

struct SceneMetadata
{
    uint32_t                    schemaVersion = 3;  // .rt2scene format version
    std::filesystem::path       sourcePath;         // file this document was loaded from / saved to
    std::string                 name;               // display name (defaults to filename stem)
    bool                        dirty = false;      // unsaved authoring changes
};

// UUID -> entt::entity lookup. Maintained by SceneDocument alongside the
// registry so it is always consistent with the ecs it accompanies.
class UuidIndex
{
public:
    void Clear() { m_Map.clear(); }

    void Insert(const UUID& uuid, entt::entity e) { m_Map[uuid] = e; }

    void Erase(const UUID& uuid) { m_Map.erase(uuid); }

    entt::entity Find(const UUID& uuid) const
    {
        auto it = m_Map.find(uuid);
        return it == m_Map.end() ? entt::null : it->second;
    }

    bool Contains(const UUID& uuid) const { return m_Map.count(uuid) != 0; }

    size_t Size() const { return m_Map.size(); }

    const std::unordered_map<UUID, entt::entity>& All() const { return m_Map; }

private:
    std::unordered_map<UUID, entt::entity> m_Map;
};

class IUuidProvider;

class SceneDocument
{
public:
    SceneDocument() = default;

    ECSScene             ecs;
    EnvironmentSettings  environment;
    SceneMetadata        metadata;
    UuidIndex            uuidIndex;
    GPUSceneData         gpuCache;   // per-document CPU cache

    // Clear all scene state including the UUID index and GPU cache. Does not
    // touch the injected UUID provider.
    void Clear();

    // Assign a fresh UUID to a newly created entity, add EntityIdComponent,
    // and insert into the index. Returns the UUID. The provider must be set
    // before calling; asserts otherwise.
    UUID AssignNewUuid(entt::entity e);

    // Attach a caller-supplied UUID to an entity, add EntityIdComponent, and
    // insert into the index. Phase 3B1 structural commands use this to
    // create entities with known UUIDs so Undo/Redo can restore the exact
    // same identity. Returns false (no mutation) if the UUID is nil or
    // already present in the index.
    bool AssignKnownUuid(entt::entity e, const UUID& uuid);

    // Attach a UUID provider used by AssignNewUuid. The document does not
    // own the provider; the caller (SceneManager/slice runner/tests) owns it.
    void SetUuidProvider(IUuidProvider* provider) { m_UuidProvider = provider; }
    IUuidProvider* GetUuidProvider() const { return m_UuidProvider; }

    // Look up an entity by UUID.
    entt::entity FindByUuid(const UUID& uuid) const { return uuidIndex.Find(uuid); }

    // Validate that all EntityIdComponent UUIDs are unique and match the
    // index. Fills `err` on failure. Used by the serializer after load.
    bool ValidateUniqueUuids(Error& err) const;

private:
    IUuidProvider* m_UuidProvider = nullptr;
};

} // namespace rt2::core

#endif // RT2_CORE_SCENE_DOCUMENT_H
