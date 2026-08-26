#include <doctest/doctest.h>

#include "ContentBrowserOperations.h"
#include "EditorCommandHistory.h"
#include "PrefabComponentKey.h"
#include "PrefabEditorActions.h"
#include "PrefabEditorPresentation.h"
#include "SceneManager.h"
#include "SceneSerializer.h"

#include <filesystem>

using namespace rt2::core;

namespace {

struct UiFixture
{
    DeterministicUuidProvider ids;
    SceneManager scene;

    UiFixture()
    {
        scene.SetUuidProvider(&ids);
        scene.AddMaterial(SceneMaterial{});
    }
};

struct TempPrefabDir
{
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        "rt2_phase8_prefab_ui_closure";
    TempPrefabDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        REQUIRE(std::filesystem::create_directories(path));
    }
    ~TempPrefabDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

} // namespace

TEST_CASE("Phase 8 C-UI: create prefab is asset-only and one undoable action")
{
    UiFixture f;
    TempPrefabDir temp;
    EditorCommandHistory history;
    const auto createdEntity = f.scene.CreateEmpty("Source Root");
    REQUIRE(createdEntity.success);
    const auto root = createdEntity.affectedEntities.front();
    const auto entityCount = f.scene.GetEntityCount();
    const auto revision = f.scene.AuthoringRevision();
    const auto prefabPath = temp.path / "source.rt2prefab";

    const auto result = CreatePrefabAssetFromRoot(
        f.scene, history, root, prefabPath, temp.path);
    REQUIRE(result.IsOk());
    CHECK(result.selectedRoot == root);
    CHECK(std::filesystem::exists(prefabPath));
    CHECK(std::filesystem::exists(AssetSidecarPath(prefabPath)));
    CHECK(f.scene.GetEntityCount() == entityCount);
    CHECK(f.scene.AuthoringRevision() == revision);
    const auto entity = f.scene.FindEntityByUuid(root);
    REQUIRE(static_cast<uint32_t>(entity) != static_cast<uint32_t>(entt::null));
    CHECK_FALSE(f.scene.GetECS().registry.all_of<PrefabInstanceComponent>(entity));
    CHECK_FALSE(f.scene.GetECS().registry.all_of<PrefabMemberComponent>(entity));
    CHECK(history.UndoDepthForTest() == 1);
    CHECK(history.UndoDescription() == "Create Prefab");

    REQUIRE(history.Undo(f.scene).success);
    CHECK_FALSE(std::filesystem::exists(prefabPath));
    REQUIRE(history.Redo(f.scene).success);
    CHECK(std::filesystem::exists(prefabPath));
}

TEST_CASE("Phase 8 C-UI: prefab drop instantiates linked root and records one history entry")
{
    UiFixture f;
    TempPrefabDir temp;
    const auto sourceResult = f.scene.CreateEmpty("Lamp");
    REQUIRE(sourceResult.success);
    const auto sourceRoot = sourceResult.affectedEntities.front();
    f.scene.SetTransform(SceneManager::EntityId{
        f.scene.FindEntityByUuid(sourceRoot)}, {1.0f, 2.0f, 3.0f});
    const auto prefabPath = temp.path / "lamp.rt2prefab";
    const auto created = f.scene.CreatePrefabFromSubtree({sourceRoot}, prefabPath);
    REQUIRE(created.ok);

    EditorCommandHistory history;
    const auto beforeCount = f.scene.GetEntityCount();
    const auto first = InstantiatePrefabAsset(f.scene, history, prefabPath);
    REQUIRE(first.IsOk());
    const auto second = InstantiatePrefabAsset(f.scene, history, prefabPath);
    REQUIRE(second.IsOk());
    CHECK_FALSE(first.selectedRoot.IsNull());
    CHECK_FALSE(second.selectedRoot.IsNull());
    CHECK(first.selectedRoot != second.selectedRoot);
    CHECK(f.scene.GetEntityCount() == beforeCount + 2);
    CHECK(history.UndoDepthForTest() == 2);
    CHECK(history.UndoDescription() == "Instantiate Prefab");

    const auto firstPresentation = DescribePrefabEntity(
        f.scene.AuthoringDoc(), first.selectedRoot);
    const auto secondPresentation = DescribePrefabEntity(
        f.scene.AuthoringDoc(), second.selectedRoot);
    CHECK(firstPresentation.kind == PrefabLinkPresentationKind::Root);
    CHECK(firstPresentation.hierarchyTag == "[Prefab]");
    CHECK(firstPresentation.hasSource);
    CHECK(firstPresentation.source.path == prefabPath.u8string());
    CHECK(firstPresentation.instanceId != secondPresentation.instanceId);
    const auto* firstTransform = f.scene.GetECS().registry.try_get<Transform>(
        f.scene.FindEntityByUuid(first.selectedRoot));
    const auto* secondTransform = f.scene.GetECS().registry.try_get<Transform>(
        f.scene.FindEntityByUuid(second.selectedRoot));
    REQUIRE(firstTransform != nullptr);
    REQUIRE(secondTransform != nullptr);
    CHECK(firstTransform->translation == glm::vec3(1.0f, 2.0f, 3.0f));
    CHECK(secondTransform->translation == firstTransform->translation);

    REQUIRE(history.Undo(f.scene).success);
    CHECK(static_cast<uint32_t>(f.scene.FindEntityByUuid(second.selectedRoot)) ==
        static_cast<uint32_t>(entt::null));
    CHECK(static_cast<uint32_t>(f.scene.FindEntityByUuid(first.selectedRoot)) !=
        static_cast<uint32_t>(entt::null));
    REQUIRE(history.Redo(f.scene).success);
    CHECK(static_cast<uint32_t>(f.scene.FindEntityByUuid(second.selectedRoot)) !=
        static_cast<uint32_t>(entt::null));
}

TEST_CASE("Phase 8 C-UI: create rejects a subtree containing prefab links before writing")
{
    UiFixture f;
    TempPrefabDir temp;
    EditorCommandHistory history;
    const auto rootResult = f.scene.CreateEmpty("Ordinary Root");
    const auto childResult = f.scene.CreateEmpty(
        "Linked Child", rootResult.affectedEntities.front());
    REQUIRE(rootResult.success);
    REQUIRE(childResult.success);
    const auto child = f.scene.FindEntityByUuid(childResult.affectedEntities.front());
    f.scene.GetECS().registry.emplace<PrefabMemberComponent>(
        child, PrefabMemberComponent{f.scene.ReserveKnownUuid(),
            f.scene.ReserveKnownUuid(), {}});

    const auto prefabPath = temp.path / "mixed.rt2prefab";
    const auto result = CreatePrefabAssetFromRoot(
        f.scene, history, rootResult.affectedEntities.front(), prefabPath,
        temp.path);
    CHECK_FALSE(result.IsOk());
    CHECK(result.mutation.error.code == Error::InvalidArgument);
    CHECK(result.mutation.error.detail.find("ordinary subtree") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(prefabPath));
    CHECK(history.UndoDepthForTest() == 0);
}

TEST_CASE("Phase 8 C-UI: create stays inside the active project asset root")
{
    UiFixture f;
    TempPrefabDir temp;
    EditorCommandHistory history;
    const auto rootResult = f.scene.CreateEmpty("Source");
    REQUIRE(rootResult.success);
    const auto assetRoot = temp.path / "Assets";
    REQUIRE(std::filesystem::create_directories(assetRoot));
    const auto outside = temp.path / "outside.rt2prefab";

    const auto result = CreatePrefabAssetFromRoot(
        f.scene, history, rootResult.affectedEntities.front(), outside, assetRoot);
    CHECK_FALSE(result.IsOk());
    CHECK(result.mutation.error.code == Error::InvalidArgument);
    CHECK(result.mutation.error.detail.find("asset root") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(outside));
    CHECK(history.UndoDepthForTest() == 0);

    const auto noProjectPath = temp.path / "no-project.rt2prefab";
    const auto noProject = CreatePrefabAssetFromRoot(f.scene, history,
        rootResult.affectedEntities.front(), noProjectPath, {});
    CHECK_FALSE(noProject.IsOk());
    CHECK(noProject.mutation.error.detail.find("asset root") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(noProjectPath));
    CHECK(history.UndoDepthForTest() == 0);
}

TEST_CASE("Phase 8 C-UI: presentation derives linked overrides and loud broken state")
{
    UiFixture f;
    TempPrefabDir temp;
    const auto rootResult = f.scene.CreateEmpty("Root");
    const auto childResult = f.scene.CreateEmpty("Child", rootResult.affectedEntities.front());
    REQUIRE(rootResult.success);
    REQUIRE(childResult.success);
    const auto rootUuid = rootResult.affectedEntities.front();
    const auto childUuid = childResult.affectedEntities.front();
    const auto root = f.scene.FindEntityByUuid(rootUuid);
    const auto child = f.scene.FindEntityByUuid(childUuid);
    const auto instanceId = f.scene.ReserveKnownUuid();
    const auto rootTemplate = f.scene.ReserveKnownUuid();
    const auto childTemplate = f.scene.ReserveKnownUuid();

    AssetReference source;
    source.kind = AssetKind::Prefab;
    source.path = "Prefabs/lamp.rt2prefab";
    source.assetId = f.scene.ReserveKnownUuid();
    f.scene.GetECS().registry.emplace<PrefabInstanceComponent>(
        root, PrefabInstanceComponent{source, instanceId});
    f.scene.GetECS().registry.emplace<PrefabMemberComponent>(
        root, PrefabMemberComponent{instanceId, rootTemplate, {}});
    f.scene.GetECS().registry.emplace<PrefabMemberComponent>(
        child, PrefabMemberComponent{instanceId, childTemplate,
            {PrefabComponentKeyFor<Transform>::value,
             PrefabComponentKeyFor<MaterialOverrideComponent>::value}});

    const auto linked = DescribePrefabEntity(f.scene.AuthoringDoc(), childUuid);
    CHECK(linked.kind == PrefabLinkPresentationKind::Member);
    CHECK(linked.hierarchyTag == "[Linked]");
    CHECK(linked.rootUuid == rootUuid);
    CHECK(linked.source.kind == source.kind);
    CHECK(linked.source.path == source.path);
    CHECK(linked.source.assetId == source.assetId);
    REQUIRE(linked.overrideLabels.size() == 2);
    CHECK(linked.overrideLabels[0] == "Material");
    CHECK(linked.overrideLabels[1] == "Transform");
    CHECK(PrefabOverrideTooltip(linked.overrideLabels[0]) ==
        "The complete Material component is protected from source updates.");

    f.scene.AuthoringDoc().metadata.schemaVersion = SceneSerializer::SchemaVersion;
    const auto scenePath = temp.path / "linked.rt2scene";
    std::vector<AssetDiagnostic> saveDiagnostics;
    Error saveError;
    REQUIRE(SceneSerializer::Save(
        f.scene.AuthoringDoc(), scenePath, saveDiagnostics, saveError));
    SceneDocument loaded;
    loaded.SetUuidProvider(&f.ids);
    Error loadError;
    REQUIRE(SceneSerializer::Load(loaded, scenePath, loadError));
    const auto reloaded = DescribePrefabEntity(loaded, childUuid);
    CHECK(reloaded.kind == PrefabLinkPresentationKind::Member);
    CHECK(reloaded.source.path == source.path);
    CHECK(reloaded.overrideLabels == linked.overrideLabels);

    f.scene.GetECS().registry.remove<PrefabInstanceComponent>(root);
    const auto broken = DescribePrefabEntity(f.scene.AuthoringDoc(), childUuid);
    CHECK(broken.kind == PrefabLinkPresentationKind::Broken);
    CHECK(broken.hierarchyTag == "[Prefab?]");
    CHECK_FALSE(broken.warning.empty());
}

TEST_CASE("Phase 8 C-UI: late history rejection compensates create and instantiate")
{
    SUBCASE("create restores the absent asset state")
    {
        UiFixture f;
        TempPrefabDir temp;
        EditorCommandHistory history;
        const auto source = f.scene.CreateEmpty("Source").affectedEntities.front();
        const auto path = temp.path / "late-create.rt2prefab";
        history.FailNextRecordAppliedForTest();
        const auto result = CreatePrefabAssetFromRoot(
            f.scene, history, source, path, temp.path);
        CHECK_FALSE(result.IsOk());
        CHECK_FALSE(std::filesystem::exists(path));
        CHECK_FALSE(std::filesystem::exists(AssetSidecarPath(path)));
        CHECK(static_cast<uint32_t>(f.scene.FindEntityByUuid(source)) !=
            static_cast<uint32_t>(entt::null));
        CHECK(history.UndoDepthForTest() == 0);
    }
    SUBCASE("instantiate removes the already-applied instance")
    {
        UiFixture f;
        TempPrefabDir temp;
        const auto source = f.scene.CreateEmpty("Source").affectedEntities.front();
        const auto sourceEntity = f.scene.FindEntityByUuid(source);
        f.scene.GetECS().registry.emplace<PrimitiveComponent>(
            sourceEntity, PrimitiveComponent{PrimitiveComponent::Cube, 1.0f, 24, 16});
        const auto path = temp.path / "late-instantiate.rt2prefab";
        REQUIRE(f.scene.CreatePrefabFromSubtree({source}, path).ok);
        const auto entityCount = f.scene.GetEntityCount();
        const auto meshCount = f.scene.GetECS().meshRegistry.GetCount();
        const auto materialCount = f.scene.GetECS().materials.size();
        const auto textureCount = f.scene.GetECS().textures.size();
        const auto revision = f.scene.AuthoringRevision();
        const auto resourceGeneration = f.scene.ResourceGeneration();
        const auto dirty = f.scene.IsDirty();
        EditorCommandHistory history;
        history.FailNextRecordAppliedForTest();
        const auto result = InstantiatePrefabAsset(f.scene, history, path);
        CHECK_FALSE(result.IsOk());
        CHECK(f.scene.GetEntityCount() == entityCount);
        CHECK(f.scene.GetECS().meshRegistry.GetCount() == meshCount);
        CHECK(f.scene.GetECS().materials.size() == materialCount);
        CHECK(f.scene.GetECS().textures.size() == textureCount);
        CHECK(f.scene.AuthoringRevision() == revision);
        CHECK(f.scene.ResourceGeneration() == resourceGeneration);
        CHECK(f.scene.IsDirty() == dirty);
        CHECK(history.UndoDepthForTest() == 0);
    }
    SUBCASE("invalid prefab is a visible zero-mutation failure")
    {
        UiFixture f;
        TempPrefabDir temp;
        EditorCommandHistory history;
        const auto entityCount = f.scene.GetEntityCount();
        const auto revision = f.scene.AuthoringRevision();
        const auto meshCount = f.scene.GetECS().meshRegistry.GetCount();
        const auto result = InstantiatePrefabAsset(
            f.scene, history, temp.path / "missing.rt2prefab");
        CHECK_FALSE(result.IsOk());
        CHECK_FALSE(result.mutation.error.IsOk());
        CHECK(f.scene.GetEntityCount() == entityCount);
        CHECK(f.scene.AuthoringRevision() == revision);
        CHECK(f.scene.GetECS().meshRegistry.GetCount() == meshCount);
        CHECK(history.UndoDepthForTest() == 0);
    }
}

TEST_CASE("Phase 8 C-UI: prefab drop dispatch and propagation presentation are exact")
{
    int prefabCalls = 0;
    int modelCalls = 0;
    std::string received;
    ContentBrowserDropCallbacks callbacks;
    callbacks.importGltf = [&](const std::string&) { ++modelCalls; };
    callbacks.instantiatePrefab = [&](const std::string& path) {
        ++prefabCalls;
        received = path;
    };
    Error error;
    REQUIRE(DispatchContentBrowserAssetDrop(
        "Assets/Prefabs/Lamp.RT2PREFAB", callbacks, error));
    CHECK(prefabCalls == 1);
    CHECK(modelCalls == 0);
    CHECK(received == "Assets/Prefabs/Lamp.RT2PREFAB");

    PrefabPropagationLiveReport report;
    report.accepted = true;
    report.applied = true;
    report.propagatedInstances = 2;
    report.quarantinedInstances = 1;
    PrefabPropagationDiagnostic diagnostic;
    diagnostic.prefabPath = "Assets/Prefabs/Lamp.rt2prefab";
    diagnostic.instanceId = UUID::Parse("11111111-2222-4333-8444-555555555555");
    diagnostic.reason = "broken sibling";
    report.diagnostics.push_back(diagnostic);
    const auto presentation = DescribePrefabPropagation(report);
    CHECK(presentation.visible);
    CHECK(presentation.warning);
    CHECK(presentation.summary.find("2 applied") != std::string::npos);
    CHECK(presentation.Warns(diagnostic.instanceId));
    REQUIRE(presentation.diagnostics.size() == 1);
    CHECK(presentation.diagnostics.front().reason == "broken sibling");

    const auto clean = DescribePrefabPropagation(PrefabPropagationLiveReport{});
    CHECK_FALSE(clean.visible);
    CHECK(clean.diagnostics.empty());

    PrefabPropagationPresentationState state;
    state.Replace(presentation);
    REQUIRE(state.Current().diagnostics.size() == 1);
    state.Replace(clean);
    CHECK_FALSE(state.Current().visible);
    CHECK(state.Current().diagnostics.empty());
    state.Replace(presentation);
    state.Clear();
    CHECK_FALSE(state.Current().visible);
    CHECK(state.Current().diagnostics.empty());

    PrefabPropagationLiveReport queuedReport;
    queuedReport.accepted = true;
    queuedReport.queued = true;
    CHECK(DescribePrefabPropagation(queuedReport).visible);
    PrefabPropagationLiveReport noOpReport;
    noOpReport.accepted = true;
    noOpReport.noOp = true;
    CHECK(DescribePrefabPropagation(noOpReport).visible);
    PrefabPropagationLiveReport failedReport;
    failedReport.error = Error{Error::Parse, "broken.rt2prefab", "invalid JSON"};
    const auto failedPresentation = DescribePrefabPropagation(failedReport);
    CHECK(failedPresentation.visible);
    CHECK(failedPresentation.warning);
}
