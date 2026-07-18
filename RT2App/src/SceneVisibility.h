#pragma once

#ifndef RT2_SCENE_VISIBILITY_H
#define RT2_SCENE_VISIBILITY_H

#include "ECSScene.h"

#include <entt/entt.hpp>
#include <vector>

namespace SceneVisibility
{
bool IsEffectivelyVisible(const entt::registry& registry, entt::entity entity);
std::vector<entt::entity> CollectVisibleRenderables(const ECSScene& scene);
}

#endif
