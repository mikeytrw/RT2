#pragma once

#ifndef RT2_SCENE_HIERARCHY_H
#define RT2_SCENE_HIERARCHY_H

#include "core/Error.h"

#include <entt/entt.hpp>
#include <vector>

// Hierarchy::parent is the authored source of truth. Hierarchy::children is
// an eagerly-maintained traversal cache rebuilt at scene boundaries and
// updated by editor mutations.
namespace SceneHierarchy
{
bool Validate(const entt::registry& registry, rt2::core::Error& error);
bool RebuildChildren(entt::registry& registry, rt2::core::Error& error);
bool IsDescendant(const entt::registry& registry,
                  entt::entity ancestor, entt::entity candidate);
void CollectSubtreePreOrder(const entt::registry& registry, entt::entity root,
                            std::vector<entt::entity>& entities);
void CollectSubtreePostOrder(const entt::registry& registry, entt::entity root,
                             std::vector<entt::entity>& entities);
}

#endif
