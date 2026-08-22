#include <doctest/doctest.h>

#include "PrefabPropagationLive.h"
#include "EditorCommandHistory.h"
#include "SceneManager.h"

using namespace rt2::core;

namespace {

PrefabSourceFingerprint Fingerprint(const char* digest)
{
    return { std::filesystem::path("C:/assets/vehicle.rt2prefab"),
             UUID::Parse("00000000-0000-4000-8000-000000000601"), digest };
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
