#include <doctest/doctest.h>

#include "ContentBrowserOperations.h"
#include "AssetIdentity.h"
#include "ECSComponents.h"
#include "ProjectAssetScanner.h"
#include "SceneAssetReferenceVisitor.h"
#include "SceneDocument.h"
#include "SceneSerializer.h"
#include "core/UUID.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace rt2::core;

namespace {

const UUID kModelId = UUID::Parse("550e8400-e29b-41d4-a716-446655440101");
const UUID kScriptId = UUID::Parse("550e8400-e29b-41d4-a716-446655440102");
const UUID kEntityId = UUID::Parse("550e8400-e29b-41d4-a716-446655440103");

struct TempTree
{
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        "rt2_phase7_w6_tests";
    std::filesystem::path assets = root / "Assets";

    TempTree()
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        REQUIRE(std::filesystem::create_directories(assets / "models"));
        REQUIRE(std::filesystem::create_directories(assets / "scripts"));
        REQUIRE(std::filesystem::create_directories(assets / "Scenes"));
    }

    ~TempTree()
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    std::filesystem::path AddAsset(const std::filesystem::path& relative,
                                   const UUID& id)
    {
        const auto source = assets / relative;
        std::ofstream(source) << "asset";
        Error error;
        REQUIRE(WriteSidecarId(AssetSidecarPath(source), id, error));
        return source;
    }
};

AssetRecord Record(const std::string& path, const UUID& id)
{
    AssetRecord record;
    record.assetId = id;
    record.sourcePath = path;
    record.identityAuthority = AssetIdentityAuthority::Sidecar;
    return record;
}

bool HasDiagnosticDetail(const ContentBrowserOperationReport& report,
                         const std::string& text)
{
    return std::any_of(report.diagnostics.begin(), report.diagnostics.end(),
        [&](const AssetDiagnostic& diagnostic) {
            return diagnostic.detail.find(text) != std::string::npos;
        });
}

} // namespace

TEST_CASE("Phase7 W6 search is snapshot based and matches path or ID")
{
    AssetDatabase database;
    std::vector<AssetDatabaseDiagnostic> diagnostics;
    database.AddOrUpdate(Record("z/model.glb", kModelId), diagnostics);
    database.AddOrUpdate(Record("a/hero.obj", kScriptId), diagnostics);

    const auto all = SearchContentBrowserAssets(database, "");
    REQUIRE(all.size() == 2);
    CHECK(all[0].sourcePath == "a/hero.obj");
    CHECK(all[1].sourcePath == "z/model.glb");
    CHECK(SearchContentBrowserAssets(database, "model").size() == 1);
    CHECK(SearchContentBrowserAssets(database, kScriptId.ToString()).size() == 1);
}

TEST_CASE("Phase7 W6 rename moves source and sidecar without rewriting scene")
{
    TempTree tree;
    const auto source = tree.AddAsset("models/hero.glb", kModelId);
    const auto scene = tree.assets / "Scenes" / "main.rt2scene";
    const std::string sceneBytes = "scene bytes that must not change";
    std::ofstream(scene) << sceneBytes;
    std::error_code timeError;
    const auto sceneTime = std::filesystem::last_write_time(scene, timeError);
    REQUIRE_FALSE(timeError);

    ContentBrowserOperationReport report;
    Error error;
    REQUIRE(RenameContentBrowserAsset(
        tree.assets, Record("models/hero.glb", kModelId),
        "hero-renamed.glb", report, error));
    CHECK(report.changed);
    CHECK_FALSE(report.partialFailure);
    CHECK_FALSE(std::filesystem::exists(source));
    CHECK_FALSE(std::filesystem::exists(AssetSidecarPath(source)));
    const auto renamed = tree.assets / "models" / "hero-renamed.glb";
    REQUIRE(std::filesystem::exists(renamed));
    CHECK(ReadSidecarId(AssetSidecarPath(renamed), error) == kModelId);

    std::ifstream input(scene);
    CHECK(std::string((std::istreambuf_iterator<char>(input)), {}) == sceneBytes);
    CHECK(std::filesystem::last_write_time(scene, timeError) == sceneTime);

    ProjectAssetScanResult scan;
    REQUIRE(ScanProjectAssets(tree.assets, scan, error));
    REQUIRE(scan.database->FindByPath("models/hero-renamed.glb") != nullptr);
    CHECK(scan.database->FindByPath("models/hero-renamed.glb")->assetId == kModelId);
}

TEST_CASE("Phase7 W6 move keeps the pair together and rejects containment violations")
{
    TempTree tree;
    tree.AddAsset("models/hero.glb", kModelId);
    ContentBrowserOperationReport report;
    Error error;
    const auto destination = tree.assets / "moved" / "nested";
    REQUIRE(MoveContentBrowserAsset(
        tree.assets, Record("models/hero.glb", kModelId),
        destination, report, error));
    CHECK(std::filesystem::exists(destination / "hero.glb"));
    CHECK(std::filesystem::exists(destination / "hero.glb.rt2meta"));
    CHECK_FALSE(std::filesystem::exists(tree.assets / "models" / "hero.glb"));

    ContentBrowserIoHooks hooks;
    int calls = 0;
    hooks.moveFile = [&](const auto& from, const auto& to, Error& hookError) {
        ++calls;
        if (calls == 1)
        {
            std::filesystem::rename(from, to);
            return true;
        }
        hookError.code = Error::Io;
        hookError.path = to.u8string();
        hookError.detail = "injected sidecar move failure";
        return false;
    };
    tree.AddAsset("models/partial.glb", kScriptId);
    CHECK_FALSE(RenameContentBrowserAsset(
        tree.assets, Record("models/partial.glb", kScriptId),
        "partial-renamed.glb", report, error, hooks));
    CHECK(report.partialFailure);
    CHECK(std::filesystem::exists(tree.assets / "models" / "partial-renamed.glb"));
    CHECK(std::filesystem::exists(tree.assets / "models" / "partial.glb.rt2meta"));
    CHECK(HasDiagnosticDetail(report, "no rollback"));

    tree.AddAsset("models/boundary.glb", kModelId);
    const auto outside = tree.assets / ".." / "outside";
    CHECK_FALSE(MoveContentBrowserAsset(
        tree.assets, Record("models/boundary.glb", kModelId),
        outside, report, error));
    CHECK(error.code == Error::InvalidArgument);
}

TEST_CASE("Phase7 W6 delete is source first and names an orphan sidecar")
{
    TempTree tree;
    tree.AddAsset("models/locked.glb", kModelId);
    ContentBrowserOperationReport report;
    Error error;
    int removeCalls = 0;
    ContentBrowserIoHooks sourceFailure;
    sourceFailure.removeFile = [&](const auto&, Error& hookError) {
        ++removeCalls;
        hookError.code = Error::Io;
        hookError.detail = "injected locked source";
        return false;
    };
    CHECK_FALSE(DeleteContentBrowserAsset(
        tree.assets, Record("models/locked.glb", kModelId),
        report, error, sourceFailure));
    CHECK(removeCalls == 1);
    CHECK_FALSE(report.changed);
    CHECK(std::filesystem::exists(tree.assets / "models" / "locked.glb"));
    CHECK(std::filesystem::exists(tree.assets / "models" / "locked.glb.rt2meta"));

    tree.AddAsset("models/orphan.glb", kScriptId);
    removeCalls = 0;
    ContentBrowserIoHooks sidecarFailure;
    sidecarFailure.removeFile = [&](const auto& path, Error& hookError) {
        ++removeCalls;
        if (removeCalls == 1)
        {
            std::error_code ec;
            REQUIRE(std::filesystem::remove(path, ec));
            return true;
        }
        hookError.code = Error::Io;
        hookError.detail = "injected sidecar delete failure";
        return false;
    };
    CHECK_FALSE(DeleteContentBrowserAsset(
        tree.assets, Record("models/orphan.glb", kScriptId),
        report, error, sidecarFailure));
    CHECK(report.changed);
    CHECK(report.partialFailure);
    CHECK_FALSE(std::filesystem::exists(tree.assets / "models" / "orphan.glb"));
    const auto orphanSidecar = tree.assets / "models" / "orphan.glb.rt2meta";
    CHECK(std::filesystem::exists(orphanSidecar));
    CHECK(HasDiagnosticDetail(report, orphanSidecar.u8string()));

    ProjectAssetScanResult scan;
    REQUIRE(ScanProjectAssets(tree.assets, scan, error));
    CHECK(std::any_of(scan.diagnostics.begin(), scan.diagnostics.end(),
        [](const AssetDiagnostic& diagnostic) {
            return diagnostic.severity == AssetDiagnostic::Stale &&
                   diagnostic.sourceKey == "models/orphan.glb.rt2meta";
        }));
}

TEST_CASE("Phase7 W6 dependants come from live scene references")
{
    TempTree tree;
    SceneDocument document;
    const auto entity = document.ecs.registry.create();
    REQUIRE(document.AssignKnownUuid(entity, kEntityId));
    document.ecs.registry.emplace<NameComponent>(entity, "Hero");
    ImportedMeshSourceComponent imported;
    imported.model.kind = AssetKind::Model;
    imported.model.path = "models/hero.glb";
    imported.model.assetId = kModelId;
    imported.model.sourceKey = "gltf:scene=0";
    document.ecs.registry.emplace<ImportedMeshSourceComponent>(entity, imported);

    ScriptComponent script;
    script.asset.kind = AssetKind::Script;
    script.asset.path = "scripts/main.lua";
    script.asset.sourceKey = "lua:asset=scripts/main.lua";
    document.ecs.registry.emplace<ScriptComponent>(entity, script);
    document.environment.ref.kind = AssetKind::Environment;
    document.environment.ref.path = "models/hero.glb";
    document.environment.ref.assetId = kModelId;

    auto modelDependants = FindContentBrowserDependants(
        document, Record("models/hero.glb", kModelId), tree.assets);
    REQUIRE(modelDependants.size() == 2);
    const bool includesEnvironment = modelDependants[0].entityUuid.IsNull() ||
                                     modelDependants[1].entityUuid.IsNull();
    CHECK(includesEnvironment);
    CHECK(std::all_of(modelDependants.begin(), modelDependants.end(),
        [](const ContentBrowserDependant& dependant) {
            return dependant.sourcePath == "models/hero.glb";
        }));

    auto scriptDependants = FindContentBrowserDependants(
        document, Record("scripts/main.lua", UUID::Nil()), tree.assets);
    REQUIRE(scriptDependants.size() == 1);
    CHECK(scriptDependants[0].entityUuid == kEntityId);
    CHECK(scriptDependants[0].entityName == "Hero");
    CHECK(scriptDependants[0].kind == AssetKind::Script);
}

TEST_CASE("Phase7 W6 reimport dispatch preserves sidecar identity")
{
    TempTree tree;
    tree.AddAsset("models/reimport.glb", kModelId);
    ContentBrowserOperationReport report;
    Error error;
    std::filesystem::path callbackPath;
    REQUIRE(ReimportContentBrowserAsset(
        tree.assets, Record("models/reimport.glb", kModelId),
        [&](const AssetRecord& record, const auto& source,
            auto&, Error&) {
            CHECK(record.assetId == kModelId);
            callbackPath = source;
            return true;
        }, report, error));
    CHECK(report.changed);
    CHECK(callbackPath.filename() == "reimport.glb");
    CHECK(ReadSidecarId(AssetSidecarPath(callbackPath), error) == kModelId);

    const UUID wrongId = UUID::Parse("550e8400-e29b-41d4-a716-446655440199");
    CHECK_FALSE(ReimportContentBrowserAsset(
        tree.assets, Record("models/reimport.glb", kModelId),
        [&](const AssetRecord&, const auto&, auto&, Error& callbackError) {
            CHECK(WriteSidecarId(
                AssetSidecarPath(tree.assets / "models" / "reimport.glb"),
                wrongId, callbackError));
            return true;
        }, report, error));
    CHECK(error.code == Error::InvalidArgument);
    CHECK(HasDiagnosticDetail(report, "changed the durable asset ID"));
    CHECK(ReadSidecarId(
        AssetSidecarPath(tree.assets / "models" / "reimport.glb"), error) ==
          kModelId);
}

TEST_CASE("Phase7 W6 host policy disables standalone operations and requires confirmation")
{
    CHECK_FALSE(ContentBrowserCanOperate(false));
    CHECK(ContentBrowserCanOperate(true));
    CHECK_FALSE(ContentBrowserDeleteAllowed(false, 0));
    CHECK(ContentBrowserDeleteAllowed(true, 1));
    CHECK(ContentBrowserDeleteAllowed(true, 0));
}

TEST_CASE("Phase7 W6 acceptance: moving referenced mesh and script preserves IDs and scene bytes")
{
    TempTree tree;
    tree.AddAsset("models/hero.glb", kModelId);
    tree.AddAsset("scripts/main.lua", kScriptId);

    SceneDocument document;
    document.metadata.schemaVersion = SceneSerializer::SchemaVersion;
    document.metadata.projectId = UUID::Parse(
        "550e8400-e29b-41d4-a716-446655440110");
    document.metadata.assetRoot = tree.assets;
    document.metadata.sourcePath = tree.assets / "Scenes" / "main.rt2scene";
    const auto entity = document.ecs.registry.create();
    REQUIRE(document.AssignKnownUuid(entity, kEntityId));
    document.ecs.registry.emplace<NameComponent>(entity, "Hero");
    ImportedMeshSourceComponent imported;
    imported.model.kind = AssetKind::Model;
    imported.model.path = "models/hero.glb";
    imported.model.assetId = kModelId;
    imported.model.sourceKey = "gltf:scene=0";
    document.ecs.registry.emplace<ImportedMeshSourceComponent>(entity, imported);
    ScriptComponent script;
    script.asset.kind = AssetKind::Script;
    script.asset.path = "scripts/main.lua";
    script.asset.assetId = kScriptId;
    script.asset.sourceKey = "lua:asset=scripts/main.lua";
    document.ecs.registry.emplace<ScriptComponent>(entity, script);

    Error error;
    std::vector<AssetDiagnostic> diagnostics;
    REQUIRE(SceneSerializer::Save(
        document, document.metadata.sourcePath, diagnostics, error));
    std::ifstream beforeInput(document.metadata.sourcePath, std::ios::binary);
    const std::string before(
        (std::istreambuf_iterator<char>(beforeInput)), {});

    ContentBrowserOperationReport report;
    REQUIRE(RenameContentBrowserAsset(
        tree.assets, Record("models/hero.glb", kModelId),
        "hero-renamed.glb", report, error));
    REQUIRE(MoveContentBrowserAsset(
        tree.assets, Record("scripts/main.lua", kScriptId),
        tree.assets / "moved", report, error));

    std::ifstream afterInput(document.metadata.sourcePath, std::ios::binary);
    CHECK(std::string((std::istreambuf_iterator<char>(afterInput)), {}) == before);

    ProjectAssetScanResult scan;
    REQUIRE(ScanProjectAssets(tree.assets, scan, error));
    const auto* movedMesh = scan.database->FindByPath("models/hero-renamed.glb");
    const auto* movedScript = scan.database->FindByPath("moved/main.lua");
    REQUIRE(movedMesh != nullptr);
    REQUIRE(movedScript != nullptr);
    CHECK(movedMesh->assetId == kModelId);
    CHECK(movedScript->assetId == kScriptId);

    SceneDocument reloaded;
    SceneLoadReport loadReport;
    REQUIRE(SceneSerializer::Load(
        reloaded, document.metadata.sourcePath, loadReport, error));
    const auto references = CollectSceneAssetReferences(reloaded);
    REQUIRE(references.size() == 2);
    CHECK(std::any_of(references.begin(), references.end(),
        [](const auto& slot) {
            return slot.reference && slot.reference->assetId == kModelId &&
                   slot.reference->path == "models/hero.glb";
        }));
    CHECK(std::any_of(references.begin(), references.end(),
        [](const auto& slot) {
            return slot.reference && slot.reference->assetId == kScriptId &&
                   slot.reference->path == "scripts/main.lua";
        }));

    AssetReference staleReference;
    staleReference.kind = AssetKind::Model;
    staleReference.path = "models/hero.glb";
    staleReference.assetId = kModelId;
    std::vector<AssetDiagnostic> resolveDiagnostics;
    const auto resolved = Resolve(
        staleReference, AssetResolutionContext{tree.assets, scan.database.get()},
        kEntityId, "Hero", resolveDiagnostics);
    REQUIRE(resolved.success);
    CHECK(resolved.resolvedPath == tree.assets / "models" / "hero-renamed.glb");
}
