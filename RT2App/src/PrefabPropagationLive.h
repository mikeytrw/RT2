#pragma once

#ifndef RT2_PREFAB_PROPAGATION_LIVE_H
#define RT2_PREFAB_PROPAGATION_LIVE_H

#include "PrefabPropagationService.h"
#include "SceneMutation.h"
#include "SceneRunState.h"

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

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

// Owning context snapshot used by the live host. AssetResolutionContext holds
// a non-owning database pointer, so the host must keep the owning database
// snapshot alive across fingerprint, prepare, stage, and command execution.
struct PrefabPropagationLiveContext
{
    std::filesystem::path assetRoot;
    std::shared_ptr<const AssetDatabase> database;

    AssetResolutionContext View() const
    {
        return AssetResolutionContext{assetRoot, database.get()};
    }
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
    PrefabPropagationLiveReport Enqueue(
        const AssetReference& source, bool requiresRefresh);
    void Clear() { m_Pending.clear(); m_LastApplied.clear(); }

private:
    struct Pending
    {
        AssetReference source;
        PrefabSourceFingerprint fingerprint;
        bool hasFingerprint = false;
        bool requiresRefresh = false;
    };

    static std::string Key(const PrefabSourceFingerprint& fingerprint);
    static std::string IdentityKey(const UUID& assetId);
    static std::string SourceKey(const AssetReference& source);
    PrefabPropagationLiveReport Apply(
        SceneManager& scene, EditorCommandHistory& history,
        const AssetReference& source, const AssetResolutionContext& assets,
        const PrefabSourceFingerprint& fingerprint,
        const PrefabPropagationLiveHooks& hooks);

    std::map<std::string, Pending> m_Pending;
    struct LastApplied
    {
        PrefabSourceFingerprint fingerprint;
        std::uint64_t authoringRevision = 0;
    };
    std::map<std::string, LastApplied> m_LastApplied;
};

// CPU-only host orchestration shared by Walnut's explicit content-browser
// route and its watcher route.  The host owns the control-flow decisions
// around refresh, queueing, status publication, sync routing, bounded debounce
// storage, and context transitions; the queue remains the immutable-plan
// execution boundary.  Injected callbacks are deliberately narrow so tests
// exercise this production control flow rather than a parallel test harness.
struct PrefabPropagationLiveHostCallbacks
{
    std::function<Result<PrefabPropagationLiveContext>()> acquireContext;
    std::function<Result<PrefabPropagationLiveContext>()> refreshContext;
    std::function<void(const PrefabPropagationLiveReport&, const char*)> publish;
    std::function<void(const EditorMutationResult&)> route;
    std::function<void(const std::string&)> status;
};

class PrefabPropagationLiveHost
{
public:
    explicit PrefabPropagationLiveHost(PrefabPropagationLiveQueue& queue)
        : m_Queue(queue) {}

    PrefabPropagationLiveReport Submit(
        SceneManager& scene, EditorCommandHistory& history,
        const AssetReference& source, SceneRunState state,
        bool backgroundBusy, bool refreshedContext,
        PrefabPropagationLiveTrigger trigger, bool refreshBeforeSubmit,
        const PrefabPropagationLiveHostCallbacks& callbacks = {},
        const PrefabPropagationLiveHooks& hooks = {});

    PrefabPropagationLiveReport Drain(
        SceneManager& scene, EditorCommandHistory& history,
        SceneRunState state, bool backgroundBusy,
        const PrefabPropagationLiveHostCallbacks& callbacks = {},
        const PrefabPropagationLiveHooks& hooks = {});

    // Truncate all three main-thread debounce buffers without ever erasing
    // from an empty vector. Returns true when an event was discarded.
    static bool TruncateDebounce(
        std::vector<std::string>& scriptPaths,
        std::vector<std::string>& refreshPaths,
        std::vector<std::string>& prefabPaths,
        std::size_t limit);

    void ResetContext() { m_Queue.Clear(); }

    static std::string FormatStatus(const PrefabPropagationLiveReport& report);

private:
    void Publish(const PrefabPropagationLiveReport& report,
                 const char* context,
                 const PrefabPropagationLiveHostCallbacks& callbacks) const;

    PrefabPropagationLiveQueue& m_Queue;
};

// Returns only referenced prefab roots whose canonical durable source matches
// one of the changed .rt2prefab paths. `fullScan` is used for watcher overflow.
std::vector<AssetReference> CollectReferencedPrefabSources(
    const SceneDocument& document, const AssetResolutionContext& assets,
    const std::vector<std::filesystem::path>& changedPaths, bool fullScan,
    std::vector<AssetDiagnostic>& diagnostics);

} // namespace rt2::core

#endif // RT2_PREFAB_PROPAGATION_LIVE_H
