#pragma once

#ifndef RT2_PREFAB_PROPAGATION_SERVICE_H
#define RT2_PREFAB_PROPAGATION_SERVICE_H

#include "PrefabPropagationContracts.h"
#include "PrefabPropagationDiscovery.h"
#include "SceneAssetResolver.h"

#include <cstddef>
#include <functional>
#include <vector>

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

// Applies only durable component operations to a temporary parsed document.
// The caller invokes this before the single normal ResolveAll pass.  All
// source preparation is completed before any write, so a global source failure
// leaves the document byte-for-byte unchanged.
struct PrefabPropagationLoadReport
{
    bool changed = false;
    std::size_t propagatedInstances = 0;
    std::size_t noOpInstances = 0;
    std::size_t quarantinedInstances = 0;
    std::vector<PrefabPropagationDiagnostic> diagnostics;
};

struct PrefabPropagationLoadHooks
{
    std::function<Result<PrefabPropagationPlan>(
        const PrefabPropagationDiscoveryRequest&)> prepare;
    std::function<bool(SceneDocument&, const AssetResolutionContext&,
                       std::vector<AssetDiagnostic>&, Error&)> resolveAll;
};

Result<PrefabPropagationLoadReport> ReconcilePrefabPropagationForLoad(
    SceneDocument& document, const AssetResolutionContext& assets,
    const PrefabPropagationLoadHooks& hooks = {});

// Host-load orchestration seam: reconcile the temporary document, append
// quarantine diagnostics, then invoke exactly one normal asset resolver.
Result<PrefabPropagationLoadReport> RunPrefabPropagationLoadIntegration(
    SceneDocument& document, const AssetResolutionContext& assets,
    std::vector<AssetDiagnostic>& diagnostics, Error& err,
    const PrefabPropagationLoadHooks& hooks = {});

void AppendPrefabPropagationDiagnostics(
    const PrefabPropagationLoadReport& report,
    std::vector<AssetDiagnostic>& diagnostics);

} // namespace rt2::core

#endif // RT2_PREFAB_PROPAGATION_SERVICE_H
