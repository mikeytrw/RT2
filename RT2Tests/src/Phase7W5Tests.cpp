#include <doctest/doctest.h>

#include "AssetIdentity.h"
#include "ECSComponents.h"
#include "SceneAssetMigration.h"
#include "SceneAssetReferenceVisitor.h"
#include "SceneRecoveryService.h"
#include "SceneSerializer.h"
#include "core/UUID.h"
#include "json.hpp"

#include <filesystem>
#include <fstream>
#include <string>

using namespace rt2::core;

namespace {

struct CountingUuidProvider final : IUuidProvider
{
    DeterministicUuidProvider inner;
    int calls = 0;

    UUID CreateV4() override
    {
        ++calls;
        return inner.CreateV4();
    }
};

struct TempTree
{
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        "rt2_phase7_w5_tests";

    TempTree()
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root / "Assets" / "Scenes");
        std::ofstream(root / "Assets" / "model.glb") << "model";
        std::ofstream(root / "Assets" / "move.lua") << "return {}";
        std::ofstream(root / "Assets" / "night.exr") << "env";
    }

    ~TempTree()
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

SceneDocument MakeLegacyDocument(const TempTree& tree,
                                 IUuidProvider* provider,
                                 const UUID& firstId,
                                 const UUID& secondId)
{
    SceneDocument document;
    document.SetUuidProvider(provider);
    document.metadata.schemaVersion = 3;
    document.metadata.sourcePath = tree.root / "Assets" / "Scenes" /
        "legacy.rt2scene";

    auto addModel = [&](const UUID& id) {
        const auto entity = document.ecs.registry.create();
        REQUIRE(document.AssignKnownUuid(entity, id));
        document.ecs.registry.emplace<Transform>(entity);
        document.ecs.registry.emplace<VisibleComponent>(entity);
        ImportedMeshSourceComponent imported;
        imported.model.kind = AssetKind::Model;
        imported.model.path = "../model.glb";
        imported.model.sourceKey = "gltf:scene=0";
        document.ecs.registry.emplace<ImportedMeshSourceComponent>(
            entity, imported);
    };
    addModel(firstId);
    addModel(secondId);

    const auto scriptEntity = document.ecs.registry.create();
    REQUIRE(document.AssignKnownUuid(
        scriptEntity, UUID::Parse("550e8400-e29b-41d4-a716-446655440003")));
    document.ecs.registry.emplace<Transform>(scriptEntity);
    document.ecs.registry.emplace<VisibleComponent>(scriptEntity);
    ScriptComponent script;
    script.asset.kind = AssetKind::Script;
    script.asset.path = "../move.lua";
    script.asset.sourceKey = "lua:asset=../move.lua";
    document.ecs.registry.emplace<ScriptComponent>(scriptEntity, script);

    document.environment.ref.kind = AssetKind::Environment;
    document.environment.ref.path = "../night.exr";
    return document;
}

} // namespace

TEST_CASE("Phase7 W5 migration assigns once per physical source and writes v4")
{
    TempTree tree;
    CountingUuidProvider provider;
    const UUID first = UUID::Parse("550e8400-e29b-41d4-a716-446655440001");
    const UUID second = UUID::Parse("550e8400-e29b-41d4-a716-446655440002");
    SceneDocument source = MakeLegacyDocument(tree, &provider, first, second);

    Error err;
    std::vector<AssetDiagnostic> saveDiagnostics;
    REQUIRE(SceneSerializer::SaveTo(
        source, source.metadata.sourcePath, source.metadata.sourcePath,
        saveDiagnostics, err));

    SceneDocument loaded;
    loaded.SetUuidProvider(&provider);
    SceneLoadReport loadReport;
    REQUIRE(SceneSerializer::Load(
        loaded, source.metadata.sourcePath, loadReport, err));
    CHECK(loadReport.sourceVersion == 3);
    CHECK(loadReport.requiresAssetMigration);

    SceneDocument staged;
    SceneAssetMigrationReport migration;
    const UUID projectId = UUID::Parse(
        "550e8400-e29b-41d4-a716-446655440010");
    const SceneAssetMigrationOptions options{
        tree.root / "Assets", projectId, &provider, nullptr};
    REQUIRE(MigrateSceneAssetReferences(
        loaded, staged, options, migration, err));
    CHECK(migration.requiresPersistence);
    CHECK_FALSE(migration.incomplete);
    CHECK(migration.createdSidecarCount == 3);
    CHECK(provider.calls == 3);
    CHECK(staged.metadata.schemaVersion == SceneSerializer::SchemaVersion);
    CHECK(staged.metadata.projectId == projectId);
    CHECK(staged.metadata.assetRoot == tree.root / "Assets");

    auto slots = CollectSceneAssetReferences(staged);
    REQUIRE(slots.size() == 4);
    CHECK(slots[0].reference->path == "night.exr");
    CHECK(slots[1].reference->path == "model.glb");
    CHECK(slots[2].reference->path == "model.glb");
    CHECK(slots[3].reference->path == "move.lua");
    CHECK(slots[1].reference->assetId == slots[2].reference->assetId);

    const auto migratedPath = tree.root / "Assets" / "Scenes" /
        "migrated.rt2scene";
    REQUIRE(SceneSerializer::Save(
        staged, migratedPath, saveDiagnostics, err));
    nlohmann::json saved;
    { std::ifstream input(migratedPath); input >> saved; }
    CHECK(saved["version"].get<uint32_t>() == 4);
    CHECK(saved["metadata"]["projectId"].get<std::string>() ==
          projectId.ToString());

    // A retry sees the durable sidecars and does not mint again.
    SceneDocument retried;
    SceneAssetMigrationReport retryReport;
    REQUIRE(MigrateSceneAssetReferences(
        loaded, retried, options, retryReport, err));
    CHECK(retryReport.createdSidecarCount == 0);
    CHECK(provider.calls == 3);
}

TEST_CASE("Phase7 W5 v4 rejects malformed asset identity")
{
    const auto path = std::filesystem::temp_directory_path() /
        "rt2_phase7_w5_bad_id.rt2scene";
    std::ofstream output(path);
    output << R"({
      "version":4,
      "metadata":{"projectId":"550e8400-e29b-41d4-a716-446655440010"},
      "entities":[{"uuid":"550e8400-e29b-41d4-a716-446655440001",
        "name":"Model","parent":"","visible":true,
        "transform":{"translation":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
        "importedSource":{"kind":"model","path":"model.glb",
          "sourceKey":"gltf:scene=0","assetId":"not-a-uuid"}}],
      "materials":[],"textures":[],
      "camera":{"position":[0,0,10],"forward":[0,0,-1],"fov":45,
        "aperture":0,"focusDist":1},"envMap":{"path":"","width":0,"height":0}
    })";
    output.close();

    SceneDocument document;
    SceneLoadReport report;
    Error err;
    CHECK_FALSE(SceneSerializer::Load(document, path, report, err));
    CHECK(err.code == Error::Parse);
    CHECK(report.sourceVersion == 4);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("Phase7 W5 migration rejects sidecar conflict without mutating source")
{
    TempTree tree;
    CountingUuidProvider provider;
    const UUID first = UUID::Parse("550e8400-e29b-41d4-a716-446655440001");
    const UUID second = UUID::Parse("550e8400-e29b-41d4-a716-446655440002");
    SceneDocument source = MakeLegacyDocument(tree, &provider, first, second);
    const UUID authoritative = UUID::Parse(
        "550e8400-e29b-41d4-a716-446655440020");
    Error err;
    REQUIRE(WriteSidecarId(
        AssetSidecarPath(tree.root / "Assets" / "model.glb"),
        authoritative, err));

    auto sourceSlots = CollectSceneAssetReferences(source);
    REQUIRE(sourceSlots.size() == 4);
    sourceSlots[1].reference->assetId = UUID::Parse(
        "550e8400-e29b-41d4-a716-446655440021");
    const auto originalPath = sourceSlots[1].reference->path;
    const auto originalId = sourceSlots[1].reference->assetId;

    SceneDocument staged;
    SceneAssetMigrationReport report;
    const SceneAssetMigrationOptions options{
        tree.root / "Assets", UUID::Nil(), &provider, nullptr};
    CHECK_FALSE(MigrateSceneAssetReferences(
        source, staged, options, report, err));
    CHECK(err.code == Error::InvalidArgument);
    CHECK(sourceSlots[1].reference->path == originalPath);
    CHECK(sourceSlots[1].reference->assetId == originalId);
}

TEST_CASE("Phase7 W5 recovery preserves v3 without assigning asset identity")
{
    TempTree tree;
    CountingUuidProvider provider;
    SceneDocument document = MakeLegacyDocument(
        tree, &provider,
        UUID::Parse("550e8400-e29b-41d4-a716-446655440001"),
        UUID::Parse("550e8400-e29b-41d4-a716-446655440002"));
    document.metadata.dirty = true;

    SceneRecoveryService recovery(
        tree.root / "Recovery", [] { return int64_t(1000); }, 8, 0.0);
    AssetMigrationPersistenceGate migrationGate;
    migrationGate.Adopt(true); // the host adopts this after loading v3
    CHECK(migrationGate.Pending());
    CHECK_FALSE(migrationGate.SuppressAutosave());
    CHECK(ShouldCaptureRecoverySnapshot(false, migrationGate));
    CHECK_FALSE(ShouldCaptureRecoverySnapshot(true, migrationGate));
    Error err;
    std::vector<AssetDiagnostic> diagnostics;
    CHECK_FALSE(recovery.MaybeSnapshot(
        document, 1, "unused", tree.root / "Assets", diagnostics, err));
    REQUIRE(recovery.MaybeSnapshot(
        document, 1, "unused", tree.root / "Assets", diagnostics, err));

    const auto records = recovery.Discover(err);
    REQUIRE(records.size() == 1);
    nlohmann::json snapshot = nlohmann::json::parse(records[0].snapshotJson);
    CHECK(snapshot["version"].get<uint32_t>() == 3);
    CHECK_FALSE(snapshot["entities"][0].contains("assetId"));
    CHECK_FALSE(std::filesystem::exists(
        AssetSidecarPath(tree.root / "Assets" / "model.glb")));
    CHECK(provider.calls == 0);
}
