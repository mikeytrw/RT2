#include "PrefabPropagationLive.h"

#include "EditorCommandHistory.h"
#include "PrefabPropagationCommand.h"
#include "SceneManager.h"

#include <algorithm>

namespace rt2::core {
namespace {

PrefabPropagationLiveHooks Defaults(const PrefabPropagationLiveHooks& hooks)
{
    PrefabPropagationLiveHooks result = hooks;
    if (!result.fingerprint)
        result.fingerprint = [](const AssetReference& source,
                                const AssetResolutionContext& assets) {
            return ReadPrefabSourceFingerprint(source, assets);
        };
    if (!result.prepare)
        result.prepare = [](const PrefabPropagationDiscoveryRequest& request) {
            return PreparePrefabPropagation(request);
        };
    if (!result.stage)
        result.stage = [](const PrefabPropagationPlan& plan,
                          const SceneDocument& document,
                          const AssetResolutionContext& assets) {
            return StagePrefabPropagationResources(plan, document, assets);
        };
    return result;
}

void CountPlan(const PrefabPropagationPlan& plan,
               PrefabPropagationLiveReport& report)
{
    for (const auto& instance : plan.instances)
    {
        if (instance.disposition == PrefabPropagationInstanceDisposition::Propagate)
            ++report.propagatedInstances;
        else if (instance.disposition == PrefabPropagationInstanceDisposition::NoOp)
            ++report.noOpInstances;
        else
            ++report.quarantinedInstances;
    }
    report.diagnostics = plan.diagnostics;
}

} // namespace

std::string PrefabPropagationLiveQueue::Key(
    const PrefabSourceFingerprint& fingerprint)
{
    return fingerprint.normalizedPath.generic_string() + "|" +
           fingerprint.assetId.ToString();
}

std::string PrefabPropagationLiveQueue::SourceKey(
    const AssetReference& source)
{
    return IdentityKey(source.assetId);
}

std::string PrefabPropagationLiveQueue::IdentityKey(const UUID& assetId)
{
    if (assetId.IsNull()) return {};
    return "id|" + assetId.ToString();
}

PrefabPropagationLiveReport PrefabPropagationLiveQueue::Apply(
    SceneManager& scene, EditorCommandHistory& history,
    const AssetReference& source, const AssetResolutionContext& assets,
    const PrefabSourceFingerprint& fingerprint,
    const PrefabPropagationLiveHooks& inputHooks)
{
    const auto hooks = Defaults(inputHooks);
    PrefabPropagationDiscoveryRequest request;
    request.document = &scene.AuthoringDoc();
    request.assets = assets;
    request.changedSource = source;
    request.documentGeneration = scene.DocumentGeneration();
    request.resourceGeneration = scene.ResourceGeneration();
    request.authoringRevision = scene.AuthoringRevision();
    const auto prepared = hooks.prepare(request);
    if (!prepared.IsOk())
    {
        PrefabPropagationLiveReport report;
        report.error = prepared.error;
        return report;
    }
    const auto staged = hooks.stage(prepared.value, scene.AuthoringDoc(), assets);
    if (!staged.IsOk())
    {
        PrefabPropagationLiveReport report;
        report.error = staged.error;
        return report;
    }

    PrefabPropagationLiveReport report;
    CountPlan(staged.value, report);
    if (staged.value.IsNoOp())
    {
        report.accepted = true;
        report.noOp = true;
        m_LastApplied[Key(fingerprint)] = LastApplied{
            fingerprint, scene.AuthoringRevision()};
        return report;
    }

    const auto sourceReader = [hooks, source, assets]() {
        const auto current = hooks.fingerprint(source, assets);
        return current.IsOk() ? current.value : PrefabSourceFingerprint{};
    };
    auto mutation = history.Execute(
        std::make_unique<PrefabPropagationCommand>(staged.value, sourceReader),
        scene);
    if (!mutation.success)
    {
        report.error = mutation.error;
        return report;
    }
    report.accepted = true;
    report.applied = mutation.effective;
    if (!mutation.effective)
        report.noOp = true;
    else
    {
        report.mutations.push_back(mutation);
        m_LastApplied[Key(fingerprint)] = LastApplied{
            fingerprint, scene.AuthoringRevision()};
    }
    return report;
}

bool PrefabPropagationLiveQueue::PendingNeedsRefresh() const noexcept
{
    for (const auto& item : m_Pending)
        if (item.second.requiresRefresh)
            return true;
    return false;
}

PrefabPropagationLiveReport PrefabPropagationLiveQueue::Enqueue(
    const AssetReference& source, bool requiresRefresh)
{
    PrefabPropagationLiveReport report;
    const auto key = SourceKey(source);
    if (key.empty())
    {
        report.error = {Error::InvalidArgument, source.path,
            "prefab propagation requires a validated non-nil durable asset ID before queueing"};
        return report;
    }
    m_Pending[key] = Pending{
        source, PrefabSourceFingerprint{}, false, requiresRefresh};
    report.accepted = true;
    report.queued = true;
    return report;
}

void PrefabPropagationLiveHost::Publish(
    const PrefabPropagationLiveReport& report, const char* context,
    const PrefabPropagationLiveHostCallbacks& callbacks) const
{
    if (callbacks.publish)
        callbacks.publish(report, context);
    for (const auto& mutation : report.mutations)
        if (callbacks.route)
            callbacks.route(mutation);
    // An empty drain must not erase a meaningful immediate Submit status.
    // Every accepted Submit (including queued/no-op) and every failure has
    // meaningful status; an empty successful drain does not.
    if ((report.accepted || !report.error.IsOk()) && callbacks.status)
        callbacks.status(FormatStatus(report));
}

PrefabPropagationLiveReport PrefabPropagationLiveHost::Submit(
    SceneManager& scene, EditorCommandHistory& history,
    const AssetReference& source, SceneRunState state,
    bool backgroundBusy, bool refreshedContext,
    PrefabPropagationLiveTrigger trigger, bool refreshBeforeSubmit,
    const PrefabPropagationLiveHostCallbacks& callbacks,
    const PrefabPropagationLiveHooks& hooks)
{
    if (state != SceneRunState::Edit || backgroundBusy)
    {
        const auto report = m_Queue.Enqueue(
            source, refreshBeforeSubmit && !refreshedContext);
        Publish(report, trigger == PrefabPropagationLiveTrigger::Explicit
            ? "ExplicitReimport" : "WatcherSubmit", callbacks);
        return report;
    }

    Result<PrefabPropagationLiveContext> context;
    if (refreshBeforeSubmit && !backgroundBusy && !refreshedContext)
    {
        context = callbacks.refreshContext
            ? callbacks.refreshContext()
            : Result<PrefabPropagationLiveContext>::Fail(
                Error::Io, source.path,
                "project asset database refresh unavailable before prefab propagation");
    }
    else
    {
        context = callbacks.acquireContext
            ? callbacks.acquireContext()
            : Result<PrefabPropagationLiveContext>::Fail(
                Error::Io, source.path,
                "project asset context unavailable for prefab propagation");
    }
    if (!context.IsOk())
    {
        PrefabPropagationLiveReport report;
        report.error = context.error;
        Publish(report, trigger == PrefabPropagationLiveTrigger::Explicit
            ? "ExplicitReimport" : "WatcherSubmit", callbacks);
        return report;
    }
    const auto report = m_Queue.Submit(
        scene, history, source, context.value.View(), state, backgroundBusy,
        true, trigger, hooks);
    Publish(report, trigger == PrefabPropagationLiveTrigger::Explicit
        ? "ExplicitReimport" : "WatcherSubmit", callbacks);
    return report;
}

PrefabPropagationLiveReport PrefabPropagationLiveHost::Drain(
    SceneManager& scene, EditorCommandHistory& history,
    SceneRunState state, bool backgroundBusy,
    const PrefabPropagationLiveHostCallbacks& callbacks,
    const PrefabPropagationLiveHooks& hooks)
{
    // A busy explicit submit is deliberately retained with requiresRefresh.
    // Refresh is therefore part of this production drain seam, immediately
    // before the queue can dequeue or prepare anything.
    if (m_Queue.PendingCount() == 0)
    {
        const auto report = m_Queue.Drain(
            scene, history, AssetResolutionContext{}, state,
            backgroundBusy, true, hooks);
        Publish(report, "WatcherDrain", callbacks);
        return report;
    }

    Result<PrefabPropagationLiveContext> context;
    if (m_Queue.PendingNeedsRefresh())
    {
        context = callbacks.refreshContext
            ? callbacks.refreshContext()
            : Result<PrefabPropagationLiveContext>::Fail(
                Error::Io, "project-assets",
                "project asset database refresh unavailable before queued prefab drain");
    }
    else
    {
        context = callbacks.acquireContext
            ? callbacks.acquireContext()
            : Result<PrefabPropagationLiveContext>::Fail(
                Error::Io, "project-assets",
                "project asset context unavailable for queued prefab drain");
    }
    if (!context.IsOk())
    {
        PrefabPropagationLiveReport report;
        report.accepted = true;
        report.queued = true;
        report.error = context.error;
        Publish(report, "WatcherDrain", callbacks);
        return report;
    }
    const auto report = m_Queue.Drain(
        scene, history, context.value.View(), state, backgroundBusy, true, hooks);
    Publish(report, "WatcherDrain", callbacks);
    return report;
}

bool PrefabPropagationLiveHost::TruncateDebounce(
    std::vector<std::string>& scriptPaths,
    std::vector<std::string>& refreshPaths,
    std::vector<std::string>& prefabPaths,
    std::size_t limit)
{
    bool discarded = false;
    while (scriptPaths.size() + refreshPaths.size() + prefabPaths.size() > limit)
    {
        if (!refreshPaths.empty())
            refreshPaths.erase(refreshPaths.begin());
        else if (!prefabPaths.empty())
            prefabPaths.erase(prefabPaths.begin());
        else if (!scriptPaths.empty())
            scriptPaths.erase(scriptPaths.begin());
        else
            break;
        discarded = true;
    }
    return discarded;
}

std::string PrefabPropagationLiveHost::FormatStatus(
    const PrefabPropagationLiveReport& report)
{
    std::string status = "Prefab propagation ";
    if (report.queued)
        status += "queued";
    else if (!report.error.IsOk())
        status += "failed";
    else if (report.applied)
        status += "applied";
    else
        status += "no-op";
    status += " (" + std::to_string(report.propagatedInstances) +
        " applied, " + std::to_string(report.quarantinedInstances) +
        " quarantined, " + std::to_string(report.noOpInstances) +
        " no-op)";
    if (!report.error.IsOk())
        status += "; error: " + report.error.Format();
    if (report.applied)
        status += "; Undo replays local scene state; a subsequent source event is independently re-evaluated";
    return status;
}

PrefabPropagationLiveReport PrefabPropagationLiveQueue::Submit(
    SceneManager& scene, EditorCommandHistory& history,
    const AssetReference& source, const AssetResolutionContext& assets,
    SceneRunState state, bool backgroundBusy, bool refreshedContext,
    PrefabPropagationLiveTrigger, const PrefabPropagationLiveHooks& inputHooks)
{
    const auto hooks = Defaults(inputHooks);
    const auto fingerprint = hooks.fingerprint(source, assets);
    if (!fingerprint.IsOk())
    {
        PrefabPropagationLiveReport report;
        report.error = fingerprint.error;
        return report;
    }
    const auto key = Key(fingerprint.value);
    if (fingerprint.value.assetId.IsNull())
    {
        PrefabPropagationLiveReport report;
        report.error = {Error::InvalidArgument, source.path,
            "prefab propagation fingerprint has no validated durable asset ID"};
        return report;
    }
    const auto pendingKey = IdentityKey(fingerprint.value.assetId);
    const auto pending = m_Pending.find(pendingKey);
    if (pending != m_Pending.end() &&
        pending->second.hasFingerprint &&
        pending->second.fingerprint == fingerprint.value)
    {
        PrefabPropagationLiveReport report;
        report.accepted = true;
        report.queued = true;
        report.noOp = true;
        return report;
    }
    const auto applied = m_LastApplied.find(key);
    if (applied != m_LastApplied.end() &&
        applied->second.fingerprint == fingerprint.value &&
        applied->second.authoringRevision == scene.AuthoringRevision())
    {
        PrefabPropagationLiveReport report;
        report.accepted = true;
        report.noOp = true;
        return report;
    }
    if (state != SceneRunState::Edit || backgroundBusy || !refreshedContext)
    {
        const bool requiresRefresh = !refreshedContext ||
            (pending != m_Pending.end() && pending->second.requiresRefresh);
        m_Pending[pendingKey] = Pending{
            source, fingerprint.value, true, requiresRefresh};
        PrefabPropagationLiveReport report;
        report.accepted = true;
        report.queued = true;
        return report;
    }
    return Apply(scene, history, source, assets, fingerprint.value, hooks);
}

PrefabPropagationLiveReport PrefabPropagationLiveQueue::Drain(
    SceneManager& scene, EditorCommandHistory& history,
    const AssetResolutionContext& assets, SceneRunState state,
    bool backgroundBusy, bool refreshedContext,
    const PrefabPropagationLiveHooks& inputHooks)
{
    PrefabPropagationLiveReport aggregate;
    if (state != SceneRunState::Edit || backgroundBusy || !refreshedContext)
    {
        aggregate.queued = !m_Pending.empty();
        aggregate.accepted = aggregate.queued;
        return aggregate;
    }
    const auto hooks = Defaults(inputHooks);
    while (!m_Pending.empty())
    {
        auto it = m_Pending.begin();
        const Pending pending = it->second;
        m_Pending.erase(it);
        const auto fingerprint = hooks.fingerprint(pending.source, assets);
        PrefabPropagationLiveReport report;
        if (!fingerprint.IsOk())
            report.error = fingerprint.error;
        else
            report = Apply(scene, history, pending.source, assets,
                           fingerprint.value, hooks);
        aggregate.accepted = aggregate.accepted || report.accepted;
        aggregate.applied = aggregate.applied || report.applied;
        aggregate.noOp = aggregate.noOp || report.noOp;
        aggregate.propagatedInstances += report.propagatedInstances;
        aggregate.quarantinedInstances += report.quarantinedInstances;
        aggregate.noOpInstances += report.noOpInstances;
        aggregate.mutations.insert(aggregate.mutations.end(),
                                   report.mutations.begin(),
                                   report.mutations.end());
        aggregate.diagnostics.insert(aggregate.diagnostics.end(),
                                     report.diagnostics.begin(),
                                     report.diagnostics.end());
        if (!report.error.IsOk() && aggregate.error.IsOk())
            aggregate.error = report.error;
    }
    return aggregate;
}

std::vector<AssetReference> CollectReferencedPrefabSources(
    const SceneDocument& document, const AssetResolutionContext& assets,
    const std::vector<std::filesystem::path>& changedPaths, bool fullScan,
    std::vector<AssetDiagnostic>& diagnostics)
{
    std::vector<std::filesystem::path> normalized;
    normalized.reserve(changedPaths.size());
    for (const auto& path : changedPaths)
    {
        if (path.extension() != ".rt2prefab") continue;
        normalized.push_back(CanonicalAssetPath(path));
    }
    std::sort(normalized.begin(), normalized.end());
    normalized.erase(std::unique(normalized.begin(), normalized.end()), normalized.end());

    std::map<std::string, AssetReference> selected;
    const auto view = document.ecs.registry.view<PrefabInstanceComponent>();
    for (const auto entity : view)
    {
        const auto& link = view.get<PrefabInstanceComponent>(entity);
        std::vector<AssetDiagnostic> local;
        const auto resolved = Resolve(link.prefab, assets, UUID::Nil(), {}, local);
        diagnostics.insert(diagnostics.end(), local.begin(), local.end());
        if (!resolved.success || resolved.effectiveId.IsNull()) continue;
        const auto canonical = CanonicalAssetPath(resolved.resolvedPath);
        if (!fullScan && std::find(normalized.begin(), normalized.end(), canonical) == normalized.end())
            continue;
        AssetReference source = link.prefab;
        source.kind = AssetKind::Prefab;
        source.assetId = resolved.effectiveId;
        source.path = canonical.generic_string();
        const auto key = canonical.generic_string() + "|" + resolved.effectiveId.ToString();
        selected.emplace(key, std::move(source));
    }
    std::vector<AssetReference> result;
    result.reserve(selected.size());
    for (auto& item : selected) result.push_back(std::move(item.second));
    return result;
}

} // namespace rt2::core
