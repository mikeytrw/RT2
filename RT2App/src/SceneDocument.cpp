#include "SceneDocument.h"
#include "core/Error.h"

#include <cassert>

namespace rt2::core {

void SceneDocument::Clear()
{
    ecs.Clear();
    environment.Clear();
    metadata = SceneMetadata{};
    uuidIndex.Clear();
    gpuCache = GPUSceneData{};
}

UUID SceneDocument::AssignNewUuid(entt::entity e)
{
    assert(m_UuidProvider && "SceneDocument::AssignNewUuid requires a UUID provider");
    UUID uuid = m_UuidProvider ? m_UuidProvider->CreateV4() : UUID::Nil();

    // Defensive uniqueness check: if the provider ever hands back a duplicate
    // (it should not for v4 from the OS RNG), keep generating until unique.
    while (uuidIndex.Contains(uuid) && m_UuidProvider)
        uuid = m_UuidProvider->CreateV4();

    ecs.registry.emplace_or_replace<EntityIdComponent>(e, EntityIdComponent{uuid});
    uuidIndex.Insert(uuid, e);
    return uuid;
}

bool SceneDocument::AssignKnownUuid(entt::entity e, const UUID& uuid)
{
    if (uuid.IsNull()) return false;
    if (uuidIndex.Contains(uuid)) return false;
    ecs.registry.emplace_or_replace<EntityIdComponent>(e, EntityIdComponent{uuid});
    uuidIndex.Insert(uuid, e);
    return true;
}

bool SceneDocument::ValidateUniqueUuids(Error& err) const
{
    // Walk the registry and confirm every EntityIdComponent UUID is in the
    // index exactly once. The index is the source of truth; this detects
    // deserialization bugs where a UUID was inserted twice or an entity
    // component was added without updating the index.
    std::unordered_map<UUID, int> seen;
    auto view = ecs.registry.view<EntityIdComponent>();
    for (auto entity : view)
    {
        const auto& idc = view.get<EntityIdComponent>(entity);
        if (idc.id.IsNull())
        {
            err.code = Error::DuplicateUuid;
            err.detail = "entity has nil UUID";
            return false;
        }
        if (++seen[idc.id] > 1)
        {
            err.code = Error::DuplicateUuid;
            err.path = idc.id.ToString();
            err.detail = "duplicate UUID in registry";
            return false;
        }
        if (uuidIndex.Find(idc.id) != entity)
        {
            err.code = Error::DuplicateUuid;
            err.path = idc.id.ToString();
            err.detail = "UUID index entry does not match registry entity";
            return false;
        }
    }

    // Index size must match the registry's EntityIdComponent count.
    if (uuidIndex.Size() != seen.size())
    {
        err.code = Error::DuplicateUuid;
        err.detail = "UUID index size does not match entity count";
        return false;
    }
    return true;
}

} // namespace rt2::core