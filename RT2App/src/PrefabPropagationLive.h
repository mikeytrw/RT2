#pragma once

#ifndef RT2_PREFAB_PROPAGATION_LIVE_H
#define RT2_PREFAB_PROPAGATION_LIVE_H

#include "PrefabPropagationService.h"
#include "SceneMutation.h"
#include "SceneRunState.h"

#include <cstddef>
#include <functional>
#include <map>
#include <string>

class EditorCommandHistory;
class SceneManager;

namespace rt2::core {

enum class PrefabPropagationLiveTrigger : std::uint8_t
{
    Explicit,
    Watcher,
};

struct PrefabPropagationLiveReport
{
    bool accepted = false;
    bool queued = false;
    bool applied = false;
    bool noOp = false;
    std::size_t propagatedInstances = 0;
    std::size_t quarantinedInstances = 0;
    std::size_t noOpInstances = 0;
    std::vector<PrefabPropagationDiagnostic> diagnostics;
    // Effective commands are returned intact so the host can route the
    // authoritative impact exactly once. Queued/no-op/error reports leave
    // this empty and therefore never reach the renderer sync path.
    std::vector<EditorMutationResult> mutations;
    Error error;
};

struct PrefabPropagationLiveHooks
{
    std::function<Result<PrefabSourceFingerprint>(
        const AssetReference&, const AssetResolutionContext&)> fingerprint;
    std::function<Result<PrefabPropagationPlan>(
        const PrefabPropagationDiscoveryRequest&)> prepare;
    std::function<Result<PrefabPropagationPlan>(
        const PrefabPropagationPlan&, const SceneDocument&,
        const AssetResolutionContext&)> stage;
};

// CPU-only queue shared by the explicit content-browser route and the file
// watcher route. It owns no scene state: all mutation is delegated to the
// existing immutable-plan command and EditorCommandHistory.
class PrefabPropagationLiveQueue
{
public:
    PrefabPropagationLiveReport Submit(
        SceneManager& scene, EditorCommandHistory& history,
        const AssetReference& source, const AssetResolutionContext& assets,
        SceneRunState state, bool backgroundBusy, bool refreshedContext,
        PrefabPropagationLiveTrigger trigger,
        const PrefabPropagationLiveHooks& hooks = {});

    PrefabPropagationLiveReport Drain(
        SceneManager& scene, EditorCommandHistory& history,
        const AssetResolutionContext& assets, SceneRunState state,
        bool backgroundBusy, bool refreshedContext,
        const PrefabPropagationLiveHooks& hooks = {});

    std::size_t PendingCount() const noexcept { return m_Pending.size(); }
    bool PendingNeedsRefresh() const noexcept;
    void Clear() { m_Pending.clear(); m_LastApplied.clear(); }

private:
    struct Pending
    {
        AssetReference source;
        PrefabSourceFingerprint fingerprint;
        bool requiresRefresh = false;
    };

    static std::string Key(const PrefabSourceFingerprint& fingerprint);
    PrefabPropagationLiveReport Apply(
        SceneManager& scene, EditorCommandHistory& history,
        const AssetReference& source, const AssetResolutionContext& assets,
        const PrefabSourceFingerprint& fingerprint,
        const PrefabPropagationLiveHooks& hooks);

    std::map<std::string, Pending> m_Pending;
    std::map<std::string, PrefabSourceFingerprint> m_LastApplied;
};

// Returns only referenced prefab roots whose canonical durable source matches
// one of the changed .rt2prefab paths. `fullScan` is used for watcher overflow.
std::vector<AssetReference> CollectReferencedPrefabSources(
    const SceneDocument& document, const AssetResolutionContext& assets,
    const std::vector<std::filesystem::path>& changedPaths, bool fullScan,
    std::vector<AssetDiagnostic>& diagnostics);

} // namespace rt2::core

#endif // RT2_PREFAB_PROPAGATION_LIVE_H
