#include "SceneHierarchy.h"
#include "ECSComponents.h"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace
{
std::string EntityLabel(entt::entity entity)
{
    return std::to_string(static_cast<uint32_t>(entity));
}

bool ValidateParentChains(const entt::registry& registry, rt2::core::Error& error)
{
    std::unordered_map<entt::entity, uint8_t> state;
    auto view = registry.view<Hierarchy>();
    for (const auto start : view)
    {
        entt::entity current = start;
        std::vector<entt::entity> chain;
        while (current != entt::null)
        {
            const auto known = state.find(current);
            if (known != state.end())
            {
                if (known->second == 1)
                {
                    error.code = rt2::core::Error::HierarchyCycle;
                    error.path = EntityLabel(start);
                    error.detail = "hierarchy parent chain contains a cycle";
                    return false;
                }
                break;
            }
            if (!registry.valid(current))
            {
                error.code = rt2::core::Error::MissingParent;
                error.path = EntityLabel(start);
                error.detail = "hierarchy references a destroyed entity";
                return false;
            }
            state[current] = 1;
            chain.push_back(current);
            const auto* hierarchy = registry.try_get<Hierarchy>(current);
            if (!hierarchy || hierarchy->parent == entt::null)
                break;
            if (hierarchy->parent == current)
            {
                error.code = rt2::core::Error::HierarchyCycle;
                error.path = EntityLabel(current);
                error.detail = "entity cannot be its own parent";
                return false;
            }
            if (!registry.valid(hierarchy->parent))
            {
                error.code = rt2::core::Error::MissingParent;
                error.path = EntityLabel(current);
                error.detail = "parent entity is not alive";
                return false;
            }
            current = hierarchy->parent;
        }
        for (const auto entity : chain)
            state[entity] = 2;
    }
    return true;
}
}

namespace SceneHierarchy
{
bool Validate(const entt::registry& registry, rt2::core::Error& error)
{
    error = {};
    if (!ValidateParentChains(registry, error))
        return false;

    std::unordered_map<entt::entity, size_t> appearances;
    auto view = registry.view<Hierarchy>();
    for (const auto parent : view)
    {
        const auto& hierarchy = view.get<Hierarchy>(parent);
        std::unordered_set<entt::entity> local;
        for (const auto child : hierarchy.children)
        {
            if (!registry.valid(child) || !local.insert(child).second)
            {
                error.code = rt2::core::Error::InvalidHierarchy;
                error.path = EntityLabel(parent);
                error.detail = "children cache contains an invalid or duplicate entity";
                return false;
            }
            const auto* childHierarchy = registry.try_get<Hierarchy>(child);
            if (!childHierarchy || childHierarchy->parent != parent)
            {
                error.code = rt2::core::Error::InvalidHierarchy;
                error.path = EntityLabel(child);
                error.detail = "children cache disagrees with authoritative parent";
                return false;
            }
            ++appearances[child];
        }
    }
    for (const auto child : view)
    {
        const auto parent = view.get<Hierarchy>(child).parent;
        if (parent != entt::null && appearances[child] != 1)
        {
            error.code = rt2::core::Error::InvalidHierarchy;
            error.path = EntityLabel(child);
            error.detail = "parented entity must appear exactly once in the parent cache";
            return false;
        }
    }
    return true;
}

bool RebuildChildren(entt::registry& registry, rt2::core::Error& error)
{
    error = {};
    if (!ValidateParentChains(registry, error))
        return false;
    std::vector<std::pair<entt::entity, entt::entity>> parentLinks;
    {
        auto sourceView = registry.view<Hierarchy>();
        for (const auto child : sourceView)
        {
            const auto parent = sourceView.get<Hierarchy>(child).parent;
            if (parent != entt::null)
                parentLinks.emplace_back(child, parent);
        }
    }
    for (const auto& link : parentLinks)
        if (!registry.all_of<Hierarchy>(link.second))
            registry.emplace<Hierarchy>(link.second);
    auto view = registry.view<Hierarchy>();
    for (const auto entity : view)
        view.get<Hierarchy>(entity).children.clear();
    for (const auto& link : parentLinks)
        registry.get<Hierarchy>(link.second).children.push_back(link.first);
    return Validate(registry, error);
}

bool IsDescendant(const entt::registry& registry,
                  entt::entity ancestor, entt::entity candidate)
{
    if (!registry.valid(ancestor) || !registry.valid(candidate))
        return false;
    for (entt::entity current = candidate; current != entt::null;)
    {
        if (current == ancestor)
            return true;
        const auto* hierarchy = registry.try_get<Hierarchy>(current);
        current = hierarchy ? hierarchy->parent : entt::null;
    }
    return false;
}

void CollectSubtreePreOrder(const entt::registry& registry, entt::entity root,
                            std::vector<entt::entity>& entities)
{
    if (!registry.valid(root))
        return;
    entities.push_back(root);
    if (const auto* hierarchy = registry.try_get<Hierarchy>(root))
        for (const auto child : hierarchy->children)
            CollectSubtreePreOrder(registry, child, entities);
}

void CollectSubtreePostOrder(const entt::registry& registry, entt::entity root,
                             std::vector<entt::entity>& entities)
{
    if (!registry.valid(root))
        return;
    if (const auto* hierarchy = registry.try_get<Hierarchy>(root))
        for (const auto child : hierarchy->children)
            CollectSubtreePostOrder(registry, child, entities);
    entities.push_back(root);
}
}
