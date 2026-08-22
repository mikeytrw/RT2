#include <doctest/doctest.h>

#include "PrefabPropagationLive.h"
#include "EditorCommandHistory.h"
#include "SceneManager.h"

#include <algorithm>
#include <memory>

using namespace rt2::core;

namespace {

PrefabSourceFingerprint Fingerprint(const char* digest)
{
    return { std::filesystem::path("C:/assets/vehicle.rt2prefab"),
             UUID::Parse("00000000-0000-4000-8000-000000000601"), digest };
}

struct EffectiveLiveFixture
{
    SceneManager scene;
    EditorCommandHistory history;
    const UUID entity = UUID::Parse(
        "00000000-0000-4000-8000-000000000601");
    const UUID instance = UUID::Parse(
        "00000000-0000-4000-8000-000000000602");
    const UUID templateId = UUID::Parse(
        "00000000-0000-4000-8000-000000000603");
    AssetReference source{AssetKind::Prefab, "vehicle.rt2prefab", {}, {}, instance};

    EffectiveLiveFixture()
    {
        auto& doc = scene.AuthoringDoc();
        const auto handle = doc.ecs.registry.create();
        REQUIRE(doc.AssignKnownUuid(handle, entity));
        doc.ecs.registry.emplace<NameComponent>(handle, NameComponent{"vehicle"});
        doc.ecs.registry.emplace<Transform>(handle, Transform{});
        doc.ecs.registry.emplace<PrefabMemberComponent>(handle,
            PrefabMemberComponent{instance, templateId, {}});
        doc.ecs.registry.emplace<PrefabInstanceComponent>(handle,
            PrefabInstanceComponent{source, instance});
        scene.NotifyAuthoringChanged();
    }

    PrefabPropagationPlan Plan(const char* digest,
                               bool includeQuarantined = false) const
    {
        const auto handle = scene.FindEntityByUuid(entity);
        const auto before = scene.AuthoringDoc().ecs.registry.get<Transform>(handle);
        auto after = before;
        after.translation.x += 1.0f;
        PrefabPropagationPlan plan;
        plan.source = Fingerprint(digest);
        plan.documentGeneration = scene.DocumentGeneration();
        plan.resourceGeneration = scene.ResourceGeneration();
        plan.authoringRevision = scene.AuthoringRevision();
        plan.authoringRevisionCaptured = true;
        plan.meshTableExtent = scene.AuthoringDoc().ecs.meshRegistry.GetCount();
        plan.materialTableExtent = static_cast<std::uint32_t>(
            scene.AuthoringDoc().ecs.materials.size());
        plan.textureTableExtent = static_cast<std::uint32_t>(
            scene.AuthoringDoc().ecs.textures.size());
        plan.componentOperations.push_back({
            entity, templateId, PrefabComponentKeyFor<Transform>::value,
            PrefabPropagationComponentValue{before},
            PrefabPropagationComponentValue{after}});
        plan.memberSnapshots.push_back({entity, instance, templateId, {}});
        plan.rootSnapshots.push_back({entity, instance, source});
        plan.instances.push_back({instance, entity,
            PrefabPropagationInstanceDisposition::Propagate, {entity}, {}});
        if (includeQuarantined)
        {
            const auto bad = UUID::Parse(
                "00000000-0000-4000-8000-000000000604");
            plan.instances.push_back({bad, UUID::Nil(),
                PrefabPropagationInstanceDisposition::Quarantined, {},
                {PrefabPropagationDiagnostic{
                    AssetDiagnostic::Malformed, plan.source.normalizedPath,
                    plan.source.assetId, bad, UUID::Nil(), UUID::Nil(),
                    "synthetic quarantined sibling"}}});
        }
        plan.affectedEntities = {entity};
        plan.syncImpact = SyncImpact::Transform;
        plan.diagnostics = plan.instances.back().diagnostics;
        REQUIRE(plan.IsEffective());
        return plan;
    }
};

struct HostProbe
{
    int refreshes = 0;
    int acquires = 0;
    bool refreshResult = true;
    PrefabPropagationLiveContext context{
        std::filesystem::path("C:/assets"),
        std::make_shared<AssetDatabase>()};
    PrefabPropagationLiveContext refreshedContext = context;
    std::vector<EditorMutationResult> routed;
    std::vector<PrefabPropagationLiveReport> published;
    std::vector<std::string> statuses;

    PrefabPropagationLiveHostCallbacks Callbacks()
    {
        PrefabPropagationLiveHostCallbacks callbacks;
        callbacks.acquireContext = [&] {
            ++acquires;
            return Result<PrefabPropagationLiveContext>::Ok(context);
        };
        callbacks.refreshContext = [&] {
            ++refreshes;
            if (!refreshResult)
                return Result<PrefabPropagationLiveContext>::Fail(
                    Error::Io, "project-assets",
                    "project asset database refresh failed before queued prefab drain");
            context = refreshedContext;
            return Result<PrefabPropagationLiveContext>::Ok(context);
        };
        callbacks.publish = [&](const auto& report, const char*) {
            published.push_back(report);
        };
        callbacks.route = [&](const auto& mutation) { routed.push_back(mutation); };
        callbacks.status = [&](const auto& status) { statuses.push_back(status); };
        return callbacks;
    }
};

PrefabPropagationLiveHooks EffectiveHooks(EffectiveLiveFixture& fixture,
                                           std::string& digest,
                                           int& prepareCalls,
                                           int& fingerprintCalls)
{
    PrefabPropagationLiveHooks hooks;
    hooks.fingerprint = [&](const AssetReference&, const AssetResolutionContext&) {
        ++fingerprintCalls;
        return Result<PrefabSourceFingerprint>::Ok(Fingerprint(digest.c_str()));
    };
    hooks.prepare = [&](const PrefabPropagationDiscoveryRequest&) {
        ++prepareCalls;
        return Result<PrefabPropagationPlan>::Ok(fixture.Plan(digest.c_str()));
    };
    hooks.stage = [](const PrefabPropagationPlan& plan, const SceneDocument&,
                     const AssetResolutionContext&) {
        return Result<PrefabPropagationPlan>::Ok(plan);
    };
    return hooks;
}

PrefabPropagationPlan EmptyPlan(SceneManager& scene,
                                const PrefabSourceFingerprint& source)
{
    PrefabPropagationPlan plan;
    plan.source = source;
    plan.documentGeneration = scene.DocumentGeneration();
    plan.resourceGeneration = scene.ResourceGeneration();
    plan.authoringRevision = scene.AuthoringRevision();
    plan.authoringRevisionCaptured = true;
    plan.sourceSchemaVersion = PrefabSerializer::FormatVersion;
    plan.meshTableExtent = scene.AuthoringDoc().ecs.meshRegistry.GetCount();
    plan.materialTableExtent = static_cast<std::uint32_t>(scene.AuthoringDoc().ecs.materials.size());
    plan.textureTableExtent = static_cast<std::uint32_t>(scene.AuthoringDoc().ecs.textures.size());
    return plan;
}

} // namespace

TEST_CASE("S6 live queue coalesces newest fingerprint and drains only in Edit")
{
    SceneManager scene;
    EditorCommandHistory history;
    PrefabPropagationLiveQueue queue;
    AssetReference source;
    source.kind = AssetKind::Prefab;
    source.path = "vehicle.rt2prefab";
    source.assetId = UUID::Parse("00000000-0000-4000-8000-000000000601");

    std::string digest = "one";
    int prepareCalls = 0;
    PrefabPropagationLiveHooks hooks;
    hooks.fingerprint = [&](const AssetReference&, const AssetResolutionContext&) {
        return Result<PrefabSourceFingerprint>::Ok(Fingerprint(digest.c_str()));
    };
    hooks.prepare = [&](const PrefabPropagationDiscoveryRequest& request) {
        ++prepareCalls;
        return Result<PrefabPropagationPlan>::Ok(
            EmptyPlan(scene, Fingerprint(digest.c_str())));
    };

    const auto queued = queue.Submit(scene, history, source, {},
        SceneRunState::Playing, false, true,
        PrefabPropagationLiveTrigger::Watcher, hooks);
    CHECK(queued.accepted);
    CHECK(queued.queued);
    CHECK(queue.PendingCount() == 1);
    CHECK(prepareCalls == 0); // named mutant: applying while Playing must RED
    CHECK(history.UndoDepthForTest() == 0);

    const auto duplicate = queue.Submit(scene, history, source, {},
        SceneRunState::Playing, false, true,
        PrefabPropagationLiveTrigger::Watcher, hooks);
    CHECK(duplicate.noOp);
    CHECK(queue.PendingCount() == 1);

    digest = "two";
    const auto newest = queue.Submit(scene, history, source, {},
        SceneRunState::Playing, false, true,
        PrefabPropagationLiveTrigger::Watcher, hooks);
    CHECK(newest.queued);
    CHECK(queue.PendingCount() == 1);

    queue.Clear();
    const auto staleContext = queue.Submit(scene, history, source, {},
        SceneRunState::Edit, false, false,
        PrefabPropagationLiveTrigger::Explicit, hooks);
    CHECK(staleContext.queued);
    CHECK(prepareCalls == 0); // named mutant: dequeue before DB refresh must RED

    digest = "two";
    const auto refreshed = queue.Drain(scene, history, {}, SceneRunState::Edit,
                                       false, true, hooks);
    CHECK(refreshed.noOp);
    CHECK(prepareCalls == 1);
    CHECK(queue.PendingCount() == 0);
    CHECK(history.UndoDepthForTest() == 0);
}

TEST_CASE("S6 watcher selection ignores unrelated asset kinds")
{
    SceneManager scene;
    std::vector<AssetDiagnostic> diagnostics;
    const auto selected = CollectReferencedPrefabSources(
        scene.AuthoringDoc(), {},
        { std::filesystem::path("C:/assets/mesh.glb") }, false, diagnostics);
    CHECK(selected.empty());
}

TEST_CASE("S6 deferred explicit work carries refresh evidence and context clear")
{
    SceneManager scene;
    EditorCommandHistory history;
    PrefabPropagationLiveQueue queue;
    AssetReference source;
    source.kind = AssetKind::Prefab;
    source.path = "vehicle.rt2prefab";
    source.assetId = UUID::Parse("00000000-0000-4000-8000-000000000601");

    PrefabPropagationLiveHooks hooks;
    hooks.fingerprint = [](const AssetReference&, const AssetResolutionContext&) {
        return Result<PrefabSourceFingerprint>::Ok(Fingerprint("deferred"));
    };
    hooks.prepare = [&](const PrefabPropagationDiscoveryRequest&) {
        CHECK(false); // named RED mutant: dequeue before the refresh evidence
        return Result<PrefabPropagationPlan>::Fail(
            Error::InvalidRuntimeState, "s6", "prepared before refresh");
    };

    const auto queued = queue.Submit(
        scene, history, source, {}, SceneRunState::Edit, true, false,
        PrefabPropagationLiveTrigger::Explicit, hooks);
    CHECK(queued.accepted);
    CHECK(queued.queued);
    CHECK(queue.PendingNeedsRefresh());
    const auto stillQueued = queue.Drain(
        scene, history, {}, SceneRunState::Edit, false, false, hooks);
    CHECK(stillQueued.queued);
    CHECK(queue.PendingCount() == 1);
    queue.Clear();
    CHECK(queue.PendingCount() == 0);
    CHECK_FALSE(queue.PendingNeedsRefresh());
}

TEST_CASE("S6 executable host applies effective explicit and watcher work")
{
    EffectiveLiveFixture fixture;
    PrefabPropagationLiveQueue queue;
    PrefabPropagationLiveHost host(queue);
    HostProbe probe;
    std::string digest = "effective";
    int prepareCalls = 0;
    int fingerprintCalls = 0;
    const auto hooks = EffectiveHooks(fixture, digest, prepareCalls, fingerprintCalls);

    const auto revision = fixture.scene.AuthoringRevision();
    const auto resourceGeneration = fixture.scene.ResourceGeneration();
    const auto applied = host.Submit(
        fixture.scene, fixture.history, fixture.source, SceneRunState::Edit,
        false, false, PrefabPropagationLiveTrigger::Explicit, true,
        probe.Callbacks(), hooks);
    REQUIRE(applied.applied);
    CHECK(probe.refreshes == 1);
    CHECK(probe.acquires == 0);
    CHECK(prepareCalls == 1);
    CHECK(fixture.history.UndoDepthForTest() == 1);
    CHECK(fixture.scene.AuthoringRevision() == revision + 1);
    CHECK(fixture.scene.ResourceGeneration() == resourceGeneration);
    REQUIRE(probe.routed.size() == 1);
    CHECK(probe.routed.front().syncImpact == SyncImpact::Transform);
    REQUIRE(probe.routed.front().affectedEntities.size() == 1);
    CHECK(probe.routed.front().affectedEntities.front() == fixture.entity);
    REQUIRE(!probe.statuses.empty());
    CHECK(probe.statuses.back().find("1 applied") != std::string::npos);
    CHECK(probe.statuses.back().find(
        "Undo replays local scene state; a subsequent source event is independently re-evaluated") !=
        std::string::npos);

    const auto noOp = host.Submit(
        fixture.scene, fixture.history, fixture.source, SceneRunState::Edit,
        false, true, PrefabPropagationLiveTrigger::Watcher, false,
        probe.Callbacks(), hooks);
    CHECK(noOp.noOp);
    CHECK(probe.routed.size() == 1);
    const auto statusAfterNoOp = probe.statuses.back();
    const auto emptyDrain = host.Drain(
        fixture.scene, fixture.history, SceneRunState::Edit, false,
        probe.Callbacks(), hooks);
    CHECK_FALSE(emptyDrain.accepted);
    CHECK(probe.statuses.back() == statusAfterNoOp);

    // A genuine immediate watcher Submit uses the same host route and status
    // publication, rather than relying on a later empty drain.
    EffectiveLiveFixture watcherFixture;
    PrefabPropagationLiveQueue watcherQueue;
    PrefabPropagationLiveHost watcherHost(watcherQueue);
    HostProbe watcherProbe;
    std::string watcherDigest = "watcher";
    int watcherPrepare = 0;
    int watcherFingerprint = 0;
    const auto watcherHooks = EffectiveHooks(
        watcherFixture, watcherDigest, watcherPrepare, watcherFingerprint);
    const auto watcherApplied = watcherHost.Submit(
        watcherFixture.scene, watcherFixture.history, watcherFixture.source,
        SceneRunState::Edit, false, true,
        PrefabPropagationLiveTrigger::Watcher, false,
        watcherProbe.Callbacks(), watcherHooks);
    CHECK(watcherApplied.applied);
    CHECK(watcherPrepare == 1);
    REQUIRE(watcherProbe.routed.size() == 1);
    CHECK(watcherProbe.statuses.back().find("applied") != std::string::npos);
}

TEST_CASE("S6 host refresh failure retains queued work and formats diagnostics")
{
    EffectiveLiveFixture fixture;
    PrefabPropagationLiveQueue queue;
    PrefabPropagationLiveHost host(queue);
    HostProbe probe;
    std::string digest = "deferred";
    int prepareCalls = 0;
    int fingerprintCalls = 0;
    const auto hooks = EffectiveHooks(fixture, digest, prepareCalls, fingerprintCalls);

    const auto queued = host.Submit(
        fixture.scene, fixture.history, fixture.source, SceneRunState::Edit,
        true, false, PrefabPropagationLiveTrigger::Explicit, true,
        probe.Callbacks(), hooks);
    CHECK(queued.queued);
    CHECK(queue.PendingCount() == 1);
    CHECK(prepareCalls == 0);

    probe.refreshResult = false;
    const auto failedDrain = host.Drain(
        fixture.scene, fixture.history, SceneRunState::Edit, false,
        probe.Callbacks(), hooks);
    CHECK(failedDrain.queued);
    CHECK_FALSE(failedDrain.error.IsOk());
    CHECK(queue.PendingCount() == 1);
    CHECK(probe.statuses.back().find("code=io") != std::string::npos);
    CHECK(probe.statuses.back().find("failed before queued prefab drain") !=
          std::string::npos);

    probe.refreshResult = true;
    const auto applied = host.Drain(
        fixture.scene, fixture.history, SceneRunState::Edit, false,
        probe.Callbacks(), hooks);
    CHECK(applied.applied);
    CHECK(queue.PendingCount() == 0);
    CHECK(probe.refreshes == 2); // Failed drain, then successful drain.
    CHECK(probe.routed.size() == 1);
}

TEST_CASE("S6 host coalesces newest, quarantines siblings, and resets identity")
{
    EffectiveLiveFixture fixture;
    PrefabPropagationLiveQueue queue;
    PrefabPropagationLiveHost host(queue);
    HostProbe probe;
    std::string digest = "one";
    int prepareCalls = 0;
    int fingerprintCalls = 0;
    auto hooks = EffectiveHooks(fixture, digest, prepareCalls, fingerprintCalls);

    const auto first = host.Submit(
        fixture.scene, fixture.history, fixture.source, SceneRunState::Playing,
        false, true, PrefabPropagationLiveTrigger::Watcher, false,
        probe.Callbacks(), hooks);
    CHECK(first.queued);
    digest = "two";
    const auto newest = host.Submit(
        fixture.scene, fixture.history, fixture.source, SceneRunState::Paused,
        false, true, PrefabPropagationLiveTrigger::Watcher, false,
        probe.Callbacks(), hooks);
    CHECK(newest.queued);
    CHECK(queue.PendingCount() == 1);
    CHECK(prepareCalls == 0);
    CHECK(fixture.history.UndoDepthForTest() == 0);

    const auto drained = host.Drain(
        fixture.scene, fixture.history, SceneRunState::Edit, false,
        probe.Callbacks(), hooks);
    CHECK(drained.applied);
    CHECK(prepareCalls == 1);
    CHECK(probe.routed.size() == 1);

    // Context reset clears both pending work and the last-applied suppression.
    host.ResetContext();
    const auto again = host.Submit(
        fixture.scene, fixture.history, fixture.source, SceneRunState::Edit,
        false, true, PrefabPropagationLiveTrigger::Watcher, false,
        probe.Callbacks(), hooks);
    CHECK(again.applied);
    CHECK(probe.routed.size() == 2);

    // A valid effective sibling may commit while a quarantined sibling keeps
    // its diagnostic and count; the whole batch is not discarded.
    EffectiveLiveFixture quarantineFixture;
    PrefabPropagationLiveQueue quarantineQueue;
    PrefabPropagationLiveHost quarantineHost(quarantineQueue);
    HostProbe quarantineProbe;
    std::string quarantineDigest = "quarantine";
    int quarantinePrepare = 0;
    int quarantineFingerprint = 0;
    auto quarantineHooks = EffectiveHooks(
        quarantineFixture, quarantineDigest, quarantinePrepare, quarantineFingerprint);
    quarantineHooks.prepare = [&](const PrefabPropagationDiscoveryRequest&) {
        ++quarantinePrepare;
        return Result<PrefabPropagationPlan>::Ok(
            quarantineFixture.Plan(quarantineDigest.c_str(), true));
    };
    const auto quarantined = quarantineHost.Submit(
        quarantineFixture.scene, quarantineFixture.history,
        quarantineFixture.source, SceneRunState::Edit, false, true,
        PrefabPropagationLiveTrigger::Watcher, false,
        quarantineProbe.Callbacks(), quarantineHooks);
    CHECK(quarantined.applied);
    CHECK(quarantined.propagatedInstances == 1);
    CHECK(quarantined.quarantinedInstances == 1);
    REQUIRE(quarantineProbe.published.size() == 1);
    REQUIRE(quarantineProbe.published.front().diagnostics.size() == 1);
    CHECK(quarantineProbe.statuses.back().find("1 quarantined") != std::string::npos);
}

TEST_CASE("S6 queued aliases share durable identity in either trigger order")
{
    for (const bool explicitFirst : {true, false})
    {
        EffectiveLiveFixture fixture;
        PrefabPropagationLiveQueue queue;
        PrefabPropagationLiveHost host(queue);
        HostProbe probe;
        const AssetReference relative{
            AssetKind::Prefab, "vehicle.rt2prefab", {}, {}, fixture.instance};
        const AssetReference absolute{
            AssetKind::Prefab, "C:/assets/vehicle.rt2prefab", {}, {}, fixture.instance};
        std::string observedDigest;
        int fingerprintCalls = 0;
        int prepareCalls = 0;
        int stageCalls = 0;
        PrefabPropagationLiveHooks hooks;
        hooks.fingerprint = [&](const AssetReference& source,
                                const AssetResolutionContext&) {
            ++fingerprintCalls;
            observedDigest = source.path.find("C:/") == 0
                ? "watcher-newest" : "explicit-newest";
            return Result<PrefabSourceFingerprint>::Ok(
                Fingerprint(observedDigest.c_str()));
        };
        hooks.prepare = [&](const PrefabPropagationDiscoveryRequest&) {
            ++prepareCalls;
            return Result<PrefabPropagationPlan>::Ok(
                fixture.Plan(observedDigest.c_str()));
        };
        hooks.stage = [&](const PrefabPropagationPlan& plan,
                          const SceneDocument&,
                          const AssetResolutionContext&) {
            ++stageCalls;
            return Result<PrefabPropagationPlan>::Ok(plan);
        };

        if (explicitFirst)
        {
            CHECK(host.Submit(
                fixture.scene, fixture.history, relative, SceneRunState::Playing,
                false, false, PrefabPropagationLiveTrigger::Explicit, true,
                probe.Callbacks(), hooks).queued);
            CHECK(host.Submit(
                fixture.scene, fixture.history, absolute, SceneRunState::Paused,
                false, true, PrefabPropagationLiveTrigger::Watcher, false,
                probe.Callbacks(), hooks).queued);
        }
        else
        {
            CHECK(host.Submit(
                fixture.scene, fixture.history, absolute, SceneRunState::Paused,
                false, true, PrefabPropagationLiveTrigger::Watcher, false,
                probe.Callbacks(), hooks).queued);
            CHECK(host.Submit(
                fixture.scene, fixture.history, relative, SceneRunState::Edit,
                true, false, PrefabPropagationLiveTrigger::Explicit, true,
                probe.Callbacks(), hooks).queued);
        }
        CHECK(queue.PendingCount() == 1);
        CHECK(fingerprintCalls == 0);
        const auto drained = host.Drain(
            fixture.scene, fixture.history, SceneRunState::Edit, false,
            probe.Callbacks(), hooks);
        REQUIRE(drained.applied);
        CHECK(queue.PendingCount() == 0);
        CHECK(fingerprintCalls == 2); // drain read + command revalidation
        CHECK(prepareCalls == 1);
        CHECK(stageCalls == 1);
        CHECK(observedDigest == (explicitFirst
            ? "watcher-newest" : "explicit-newest"));
        CHECK(fixture.history.UndoDepthForTest() == 1);
        REQUIRE(probe.routed.size() == 1);
        REQUIRE(probe.published.size() >= 3);
        CHECK(probe.published.back().applied);
        CHECK(probe.published.back().quarantinedInstances == 0);
    }

    EffectiveLiveFixture nilFixture;
    PrefabPropagationLiveQueue nilQueue;
    PrefabPropagationLiveHost nilHost(nilQueue);
    HostProbe nilProbe;
    AssetReference nilSource{
        AssetKind::Prefab, "vehicle.rt2prefab", {}, {}, UUID::Nil()};
    const auto rejected = nilHost.Submit(
        nilFixture.scene, nilFixture.history, nilSource,
        SceneRunState::Playing, false, false,
        PrefabPropagationLiveTrigger::Explicit, true,
        nilProbe.Callbacks());
    CHECK_FALSE(rejected.accepted);
    CHECK(rejected.error.code == Error::InvalidArgument);
    CHECK(nilQueue.PendingCount() == 0);
}

TEST_CASE("S6 host refresh uses only the post-refresh owning context")
{
    EffectiveLiveFixture fixture;
    PrefabPropagationLiveQueue queue;
    PrefabPropagationLiveHost host(queue);
    HostProbe probe;
    auto databaseA = std::make_shared<AssetDatabase>();
    auto databaseB = std::make_shared<AssetDatabase>();
    const auto databaseBAddress = databaseB.get();
    probe.context = {std::filesystem::path("C:/project-A/assets"), databaseA};
    probe.refreshedContext = {
        std::filesystem::path("C:/project-B/assets"), databaseB};
    const auto weakDatabaseA = std::weak_ptr<const AssetDatabase>(databaseA);
    auto callbacks = probe.Callbacks();
    callbacks.refreshContext = [&] {
        ++probe.refreshes;
        databaseA.reset();
        probe.context = probe.refreshedContext;
        CHECK(weakDatabaseA.expired());
        return Result<PrefabPropagationLiveContext>::Ok(probe.context);
    };

    std::string digest = "context-refresh";
    int prepareCalls = 0;
    int fingerprintCalls = 0;
    int stageCalls = 0;
    int readsA = 0;
    auto hooks = EffectiveHooks(fixture, digest, prepareCalls, fingerprintCalls);
    hooks.fingerprint = [&](const AssetReference&, const AssetResolutionContext& assets) {
        ++fingerprintCalls;
        if (assets.database != databaseBAddress ||
            assets.assetRoot != std::filesystem::path("C:/project-B/assets"))
            ++readsA;
        return Result<PrefabSourceFingerprint>::Ok(Fingerprint(digest.c_str()));
    };
    hooks.prepare = [&](const PrefabPropagationDiscoveryRequest& request) {
        ++prepareCalls;
        if (request.assets.database != databaseBAddress ||
            request.assets.assetRoot != std::filesystem::path("C:/project-B/assets"))
            ++readsA;
        return Result<PrefabPropagationPlan>::Ok(fixture.Plan(digest.c_str()));
    };
    hooks.stage = [&](const PrefabPropagationPlan& plan,
                      const SceneDocument&, const AssetResolutionContext& assets) {
        ++stageCalls;
        if (assets.database != databaseBAddress ||
            assets.assetRoot != std::filesystem::path("C:/project-B/assets"))
            ++readsA;
        return Result<PrefabPropagationPlan>::Ok(plan);
    };

    const auto applied = host.Submit(
        fixture.scene, fixture.history, fixture.source, SceneRunState::Edit,
        false, false, PrefabPropagationLiveTrigger::Explicit, true,
        callbacks, hooks);
    REQUIRE(applied.applied);
    CHECK(probe.refreshes == 1);
    CHECK(readsA == 0);
    CHECK(fingerprintCalls == 2); // initial source check and command recheck
    CHECK(prepareCalls == 1);
    CHECK(stageCalls == 1);

    EffectiveLiveFixture queuedFixture;
    PrefabPropagationLiveQueue queuedQueue;
    PrefabPropagationLiveHost queuedHost(queuedQueue);
    HostProbe queuedProbe;
    auto queuedDatabaseA = std::make_shared<AssetDatabase>();
    queuedProbe.context = {
        std::filesystem::path("C:/project-A/assets"), queuedDatabaseA};
    queuedProbe.refreshedContext = {
        std::filesystem::path("C:/project-B/assets"), databaseB};
    const auto weakQueuedDatabaseA =
        std::weak_ptr<const AssetDatabase>(queuedDatabaseA);
    auto queuedCallbacks = queuedProbe.Callbacks();
    queuedCallbacks.refreshContext = [&] {
        ++queuedProbe.refreshes;
        queuedDatabaseA.reset();
        queuedProbe.context = queuedProbe.refreshedContext;
        CHECK(weakQueuedDatabaseA.expired());
        return Result<PrefabPropagationLiveContext>::Ok(queuedProbe.context);
    };
    std::string queuedDigest = "queued-context-refresh";
    int queuedReadsA = 0;
    int queuedPrepare = 0;
    int queuedStage = 0;
    int queuedFingerprint = 0;
    auto queuedHooks = EffectiveHooks(
        queuedFixture, queuedDigest, queuedPrepare, queuedFingerprint);
    queuedHooks.fingerprint = [&](const AssetReference&,
                                  const AssetResolutionContext& assets) {
        ++queuedFingerprint;
        if (assets.database != databaseBAddress ||
            assets.assetRoot != std::filesystem::path("C:/project-B/assets"))
            ++queuedReadsA;
        return Result<PrefabSourceFingerprint>::Ok(
            Fingerprint(queuedDigest.c_str()));
    };
    queuedHooks.prepare = [&](const PrefabPropagationDiscoveryRequest& request) {
        ++queuedPrepare;
        if (request.assets.database != databaseBAddress ||
            request.assets.assetRoot != std::filesystem::path("C:/project-B/assets"))
            ++queuedReadsA;
        return Result<PrefabPropagationPlan>::Ok(
            queuedFixture.Plan(queuedDigest.c_str()));
    };
    queuedHooks.stage = [&](const PrefabPropagationPlan& plan,
                            const SceneDocument&,
                            const AssetResolutionContext& assets) {
        ++queuedStage;
        if (assets.database != databaseBAddress ||
            assets.assetRoot != std::filesystem::path("C:/project-B/assets"))
            ++queuedReadsA;
        return Result<PrefabPropagationPlan>::Ok(plan);
    };
    const auto pending = queuedHost.Submit(
        queuedFixture.scene, queuedFixture.history, queuedFixture.source,
        SceneRunState::Edit, true, false,
        PrefabPropagationLiveTrigger::Explicit, true,
        queuedCallbacks, queuedHooks);
    CHECK(pending.queued);
    CHECK(queuedProbe.acquires == 0);
    CHECK(queuedFingerprint == 0);
    const auto drained = queuedHost.Drain(
        queuedFixture.scene, queuedFixture.history, SceneRunState::Edit, false,
        queuedCallbacks, queuedHooks);
    REQUIRE(drained.applied);
    CHECK(queuedProbe.refreshes == 1);
    CHECK(queuedReadsA == 0);
    CHECK(queuedPrepare == 1);
    CHECK(queuedStage == 1);
}

TEST_CASE("S6 same fingerprint re-evaluates after Undo for explicit and watcher")
{
    EffectiveLiveFixture fixture;
    PrefabPropagationLiveQueue queue;
    PrefabPropagationLiveHost host(queue);
    HostProbe probe;
    std::string digest = "undo-recheck";
    int prepareCalls = 0;
    int fingerprintCalls = 0;
    const auto hooks = EffectiveHooks(fixture, digest, prepareCalls, fingerprintCalls);

    const auto first = host.Submit(
        fixture.scene, fixture.history, fixture.source, SceneRunState::Edit,
        false, true, PrefabPropagationLiveTrigger::Explicit, false,
        probe.Callbacks(), hooks);
    REQUIRE(first.applied);
    CHECK(fixture.history.UndoDepthForTest() == 1);
    CHECK(fixture.scene.AuthoringDoc().ecs.registry
        .get<Transform>(fixture.scene.FindEntityByUuid(fixture.entity))
        .translation.x == doctest::Approx(1.0f));

    const auto duplicate = host.Submit(
        fixture.scene, fixture.history, fixture.source, SceneRunState::Edit,
        false, true, PrefabPropagationLiveTrigger::Watcher, false,
        probe.Callbacks(), hooks);
    CHECK(duplicate.noOp);
    CHECK_FALSE(duplicate.applied);
    CHECK(prepareCalls == 1);
    CHECK(probe.routed.size() == 1);

    const auto undone = fixture.history.Undo(fixture.scene);
    REQUIRE(undone.success);
    CHECK(fixture.history.UndoDepthForTest() == 0);
    CHECK(fixture.scene.AuthoringDoc().ecs.registry
        .get<Transform>(fixture.scene.FindEntityByUuid(fixture.entity))
        .translation.x == doctest::Approx(0.0f));

    const auto reapplied = host.Submit(
        fixture.scene, fixture.history, fixture.source, SceneRunState::Edit,
        false, true, PrefabPropagationLiveTrigger::Watcher, false,
        probe.Callbacks(), hooks);
    REQUIRE(reapplied.applied);
    CHECK(prepareCalls == 2);
    CHECK(fixture.history.UndoDepthForTest() == 1);
    CHECK(probe.routed.size() == 2);
    CHECK(fixture.scene.AuthoringDoc().ecs.registry
        .get<Transform>(fixture.scene.FindEntityByUuid(fixture.entity))
        .translation.x == doctest::Approx(1.0f));
}

TEST_CASE("S6 host debounce truncation covers empty prefab and mixed buffers")
{
    std::vector<std::string> scripts;
    std::vector<std::string> refresh;
    std::vector<std::string> prefabs{"a", "b", "c"};
    CHECK(PrefabPropagationLiveHost::TruncateDebounce(
        scripts, refresh, prefabs, 2));
    CHECK(prefabs.size() == 2);

    scripts = {"script"};
    refresh.clear();
    prefabs = {"a", "b"};
    CHECK(PrefabPropagationLiveHost::TruncateDebounce(
        scripts, refresh, prefabs, 2));
    CHECK(scripts.size() + prefabs.size() == 2);

    scripts.clear();
    refresh = {"refresh"};
    prefabs.clear();
    CHECK_FALSE(PrefabPropagationLiveHost::TruncateDebounce(
        scripts, refresh, prefabs, 2));
    CHECK(refresh.size() == 1);
}

TEST_CASE("S6 host rejects sidecar drift without routing or history")
{
    EffectiveLiveFixture fixture;
    PrefabPropagationLiveQueue queue;
    PrefabPropagationLiveHost host(queue);
    HostProbe probe;
    int fingerprintCalls = 0;
    int prepareCalls = 0;
    PrefabPropagationLiveHooks hooks;
    hooks.fingerprint = [&](const AssetReference&, const AssetResolutionContext&) {
        ++fingerprintCalls;
        return Result<PrefabSourceFingerprint>::Ok(
            Fingerprint(fingerprintCalls == 1 ? "before" : "drifted"));
    };
    hooks.prepare = [&](const PrefabPropagationDiscoveryRequest&) {
        ++prepareCalls;
        return Result<PrefabPropagationPlan>::Ok(
            fixture.Plan("before"));
    };
    hooks.stage = [](const PrefabPropagationPlan& plan, const SceneDocument&,
                     const AssetResolutionContext&) {
        return Result<PrefabPropagationPlan>::Ok(plan);
    };

    const auto failed = host.Submit(
        fixture.scene, fixture.history, fixture.source, SceneRunState::Edit,
        false, true, PrefabPropagationLiveTrigger::Watcher, false,
        probe.Callbacks(), hooks);
    CHECK_FALSE(failed.applied);
    CHECK_FALSE(failed.error.IsOk());
    CHECK(fingerprintCalls == 2);
    CHECK(probe.routed.empty());
    CHECK(fixture.history.UndoDepthForTest() == 0);
    CHECK(probe.statuses.back().find("code=invalid_argument") != std::string::npos);
}
