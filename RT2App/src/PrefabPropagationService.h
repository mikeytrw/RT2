#pragma once

#ifndef RT2_PREFAB_PROPAGATION_SERVICE_H
#define RT2_PREFAB_PROPAGATION_SERVICE_H

#include "PrefabPropagationContracts.h"
#include "SceneAssetResolver.h"

namespace rt2::core {

// Resolves only on a private document clone.  The input document, its
// resource tables, and every generation counter remain untouched.  The
// returned plan owns every appended block and records all scene-global
// rebases needed by PrefabPropagationCommand.
Result<PrefabPropagationPlan> StagePrefabPropagationResources(
    const PrefabPropagationPlan& durablePlan,
    const SceneDocument& live,
    const AssetResolutionContext& assets);

inline Result<PrefabPropagationPlan> ResolvePrefabPropagationResources(
    const PrefabPropagationPlan& durablePlan,
    const SceneDocument& live,
    const AssetResolutionContext& assets)
{ return StagePrefabPropagationResources(durablePlan, live, assets); }

} // namespace rt2::core

#endif // RT2_PREFAB_PROPAGATION_SERVICE_H
