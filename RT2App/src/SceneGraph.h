#pragma once

#ifndef SCENE_GRAPH_H
#define SCENE_GRAPH_H

#include <entt/entt.hpp>
#include "ECSComponents.h"

// ============================================================================
// SceneGraph — system for resolving parent-child transform hierarchy.
//
// The SceneGraph walks the entity hierarchy and computes world matrices
// from local TRS (Transform component). Children inherit parent transforms.
//
// Usage:
//   SceneGraph::UpdateWorldTransforms(registry, rootEntities);
//   // Then read Transform::worldMatrix from each entity
//
// The system marks entities as dirty when their local TRS changes. A dirty
// entity propagates dirtiness to all descendants. UpdateWorldTransforms
// recomputes only dirty world matrices.
//
// For motion vectors, call SnapshotTransforms() after updating to stash
// the current world matrix into prevWorldMatrix before the next update.
//
// ============================================================================

class SceneGraph
{
public:
    // Mark an entity and all descendants as dirty (world matrix needs recompute)
    static void MarkDirty(entt::registry& registry, entt::entity entity);

    // Update world matrices for all entities with Transform components.
    // Walks the hierarchy from root entities (no parent) down to leaves.
    static void UpdateWorldTransforms(entt::registry& registry);

    // Snapshot current world matrices into prevWorldMatrix (for motion vectors)
    static void SnapshotTransforms(entt::registry& registry);

    // Get the world position of an entity (extracts translation from worldMatrix)
    static glm::vec3 GetWorldPosition(const entt::registry& registry, entt::entity entity);

    // Set local TRS dirty flag (call when Transform.translation/rotation/scale changes)
    static void SetLocalDirty(entt::registry& registry, entt::entity entity);

private:
    // Recursively update a node and its children
    static void UpdateNode(entt::registry& registry, entt::entity entity,
                          const glm::mat4& parentWorld, bool parentDirty);
};

#endif // SCENE_GRAPH_H