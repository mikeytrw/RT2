#include "SceneVisibility.h"
#include "ECSComponents.h"

#include <unordered_map>
#include <unordered_set>

namespace
{
bool ResolveVisibility(const entt::registry& registry, entt::entity entity,
                       std::unordered_map<entt::entity, bool>& cache,
                       std::unordered_set<entt::entity>& resolving)
{
    const auto cached = cache.find(entity);
    if (cached != cache.end())
        return cached->second;
    if (!registry.valid(entity) || !resolving.insert(entity).second)
        return false;
    bool visible = true;
    if (const auto* component = registry.try_get<VisibleComponent>(entity))
        visible = component->visible;
    if (visible)
    {
        if (const auto* hierarchy = registry.try_get<Hierarchy>(entity);
            hierarchy && hierarchy->parent != entt::null)
            visible = ResolveVisibility(registry, hierarchy->parent, cache, resolving);
    }
    resolving.erase(entity);
    cache[entity] = visible;
    return visible;
}
}

namespace SceneVisibility
{
bool IsEffectivelyVisible(const entt::registry& registry, entt::entity entity)
{
    std::unordered_map<entt::entity, bool> cache;
    std::unordered_set<entt::entity> resolving;
    return ResolveVisibility(registry, entity, cache, resolving);
}

std::vector<entt::entity> CollectVisibleRenderables(const ECSScene& scene)
{
    std::vector<entt::entity> entities;
    std::unordered_map<entt::entity, bool> cache;
    std::unordered_set<entt::entity> resolving;
    auto view = scene.registry.view<MeshRef, Transform>();
    for (const auto entity : view)
        if (ResolveVisibility(scene.registry, entity, cache, resolving))
            entities.push_back(entity);
    return entities;
}
}
