#include "RuntimeSceneMutator.h"
#include "SceneDocument.h"
#include "ECSComponents.h"
#include "ECSScene.h"
#include "SceneGraph.h"
#include "SceneHierarchy.h"

#include <algorithm>
#include <vector>

namespace rt2::core {

namespace {

void RemoveChildFromParent(entt::registry& registry, entt::entity parent, entt::entity child)
{
    if (parent == entt::null)
        return;
    if (auto* hierarchy = registry.try_get<Hierarchy>(parent))
    {
        hierarchy->children.erase(
            std::remove(hierarchy->children.begin(), hierarchy->children.end(), child),
            hierarchy->children.end());
    }
}

} // anonymous namespace

Result<UUID> RuntimeSceneMutator::CreateEntity(SceneDocument& doc,
                                                const UUID& uuid,
                                                const RuntimeEntityCreateDesc& desc) const
{
    auto& registry = doc.ecs.registry;

    if (doc.uuidIndex.Contains(uuid))
        return Result<UUID>::Fail(Error::DuplicateUuid,
            uuid.ToString(),
            "RuntimeSceneMutator::CreateEntity: UUID already present in the runtime document");

    entt::entity parent = entt::null;
    if (desc.parentUuid)
    {
        parent = doc.FindByUuid(*desc.parentUuid);
        if (parent == entt::null || !registry.valid(parent))
            return Result<UUID>::Fail(Error::InvalidEntity,
                desc.parentUuid->ToString(),
                "RuntimeSceneMutator::CreateEntity: parent UUID does not resolve in the runtime document");
    }

    const auto entity = registry.create();

    Transform tf;
    if (desc.translation) tf.translation = *desc.translation;
    if (desc.rotation)    tf.rotation    = *desc.rotation;
    if (desc.scale)       tf.scale       = *desc.scale;
    tf.dirty = true;
    registry.emplace<Transform>(entity, tf);

    registry.emplace<NameComponent>(entity,
        desc.name.empty() ? "Empty" : desc.name);

    registry.emplace<VisibleComponent>(entity);

    if (!doc.AssignKnownUuid(entity, uuid))
    {
        // Defensive: AssignKnownUuid only fails on duplicate, which we checked
        // above. Treat as a fatal mutator bug.
        registry.destroy(entity);
        return Result<UUID>::Fail(Error::DuplicateUuid,
            uuid.ToString(),
            "RuntimeSceneMutator::CreateEntity: failed to assign known UUID");
    }

    if (parent != entt::null)
    {
        registry.emplace<Hierarchy>(entity).parent = parent;
        auto* parentHierarchy = registry.try_get<Hierarchy>(parent);
        if (!parentHierarchy)
            parentHierarchy = &registry.emplace<Hierarchy>(parent);
        parentHierarchy->children.push_back(entity);
    }

    // Phase 6: emplace the optional ScriptComponent verbatim. The live sol2
    // environment is built by ScriptSystem::SyncScriptEnvironments at the
    // next safe point (after ApplyDeferredStructuralChanges returns).
    if (desc.script)
        registry.emplace<ScriptComponent>(entity, *desc.script);

    SceneGraph::MarkDirty(registry, entity);

    return Result<UUID>::Ok(uuid);
}

Result<void> RuntimeSceneMutator::DestroySubtree(SceneDocument& doc,
                                                  const UUID& uuid) const
{
    auto& registry = doc.ecs.registry;

    const auto root = doc.FindByUuid(uuid);
    if (root == entt::null || !registry.valid(root))
        return Result<void>::Fail(Error::InvalidEntity,
            uuid.ToString(),
            "RuntimeSceneMutator::DestroySubtree: UUID does not resolve in the runtime document");

    std::vector<entt::entity> subtree;
    SceneHierarchy::CollectSubtreePostOrder(registry, root, subtree);

    // Unlink root from its parent BEFORE destroying, so the parent's children
    // list stays consistent.
    if (const auto* hierarchy = registry.try_get<Hierarchy>(root))
        RemoveChildFromParent(registry, hierarchy->parent, root);

    for (const auto entity : subtree)
    {
        if (const auto* identity = registry.try_get<EntityIdComponent>(entity))
            doc.uuidIndex.Erase(identity->id);
        registry.destroy(entity);
    }

    return Result<void>::Ok();
}

} // namespace rt2::core