#include <doctest/doctest.h>

#include "EditorSettings.h"
#include "InputConfig.h"
#include "Project.h"
#include "ProjectAssetScanner.h"
#include "ProjectContext.h"
#include "AssetIdentity.h"
#include "SceneRecoveryService.h"
#include "SceneSerializer.h"
#include "SceneAssetResolver.h"
#include "Phase1AFixtureGenerator.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace rt2::core;

namespace {

std::filesystem::path TempDir(const std::string& tag)
{
    const auto path = std::filesystem::temp_directory_path() /
        (tag + "_" + std::to_string(std::rand()));
    std::filesystem::create_directories(path);
    return path;
}

void Write(const std::filesystem::path& path, const std::string& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << bytes;
    REQUIRE(output.good());
}

std::string Read(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    std::stringstream stream;
    stream << input.rdbuf();
    return stream.str();
}

InputMapping KeyAction(const std::string& name, uint16_t code)
{
    InputMapping mapping;
    mapping.name = name;
    ActionBinding binding;
    binding.device = InputDeviceKind::KeyboardKey;
    binding.code = code;
    mapping.actions.push_back(binding);
    return mapping;
}

InputContextRecord Context(const std::string& id,
                           std::initializer_list<InputMapping> mappings)
{
    InputContextRecord record;
    record.contextId = id;
    record.mappings.assign(mappings.begin(), mappings.end());
    return record;
}

std::string InputSnapshot(const std::vector<InputContextRecord>& records)
{
    return InputContextRecordsToJson(records).dump();
}

std::string DatabaseSnapshot(const AssetDatabase& database)
{
    std::ostringstream output;
    for (const auto& record : database.AllRecordsSorted())
        output << record.sourcePath << "|" << record.assetId.ToString() << "\n";
    return output.str();
}

std::string DiagnosticSnapshot(
    const std::vector<AssetDiagnostic>& diagnostics)
{
    std::ostringstream output;
    for (const auto& diagnostic : diagnostics)
        output << static_cast<int>(diagnostic.severity) << "|"
               << diagnostic.refPath << "|" << diagnostic.sourceKey << "|"
               << diagnostic.detail << "\n";
    return output.str();
}

} // namespace

TEST_CASE("Phase7 W4 input: composition order and explicit unbind are deterministic")
{
    std::vector<InputContextRecord> builtIns = {
        Context("editor", { KeyAction("undo", 90) }),
        Context("runtime", { KeyAction("jump", 32), KeyAction("fire", 70) }),
    };
    std::vector<InputContextRecord> project = {
        Context("runtime", { KeyAction("jump", 74) }),
    };
    InputMapping unbind;
    unbind.name = "fire";
    std::vector<InputContextRecord> user = {
        Context("runtime", { KeyAction("jump", 75), unbind }),
    };

    std::vector<InputContextRecord> composed;
    Error err;
    REQUIRE(ComposeInputContexts(builtIns, project, user, composed, err));
    REQUIRE(composed.size() == 2);
    const auto runtime = std::find_if(composed.begin(), composed.end(),
        [](const auto& record) { return record.contextId == "runtime"; });
    REQUIRE(runtime != composed.end());
    REQUIRE(runtime->mappings.size() == 1);
    CHECK(runtime->mappings[0].name == "jump");
    REQUIRE(runtime->mappings[0].actions.size() == 1);
    CHECK(runtime->mappings[0].actions[0].code == 75);

    std::reverse(builtIns.begin(), builtIns.end());
    std::reverse(user.front().mappings.begin(), user.front().mappings.end());
    std::vector<InputContextRecord> permuted;
    REQUIRE(ComposeInputContexts(builtIns, project, user, permuted, err));
    CHECK(InputSnapshot(permuted) == InputSnapshot(composed));
}

TEST_CASE("Phase7 W4 input: project cannot replace editor-owned contexts")
{
    std::vector<InputContextRecord> result;
    Error err;
    CHECK_FALSE(ComposeInputContexts(
        { Context("editor", { KeyAction("undo", 90) }) },
        { Context("editor", { KeyAction("undo", 89) }) }, {}, result, err));
    CHECK(err.code == Error::Parse);
    CHECK(err.detail.find("editor-owned") != std::string::npos);
}

TEST_CASE("Phase7 W4 settings: v2 inert editor mappings are dropped loudly")
{
    const auto directory = TempDir("w4_settings_migration");
    Write(directory / "settings.json", R"JSON({
  "version": 2,
  "projectRoot": "C:/LegacyBrowse",
  "recentScenes": [],
  "inputContexts": [
    {"contextId":"editor","mappings":[{"name":"undo","isAxis":false,"actions":[{"device":0,"code":89,"modifiers":0,"gamepadSlot":-1}],"axes":[]}]},
    {"contextId":"viewport","mappings":[{"name":"gizmo_translate","isAxis":false,"actions":[{"device":0,"code":84,"modifiers":0,"gamepadSlot":-1}],"axes":[]}]},
    {"contextId":"viewport.look","mappings":[{"name":"look","isAxis":false,"actions":[{"device":1,"code":1,"modifiers":0,"gamepadSlot":-1}],"axes":[]}]},
    {"contextId":"runtime","mappings":[{"name":"jump","isAxis":false,"actions":[{"device":0,"code":74,"modifiers":0,"gamepadSlot":-1}],"axes":[]}]}
  ]
})JSON");

    EditorSettingsStore settings(directory);
    EditorSettingsLoadReport report;
    Error err;
    REQUIRE(settings.Load(report, err));
    CHECK(report.sourceVersion == 2);
    CHECK(report.migrated);
    REQUIRE(settings.GetInputOverrides().size() == 1);
    CHECK(settings.GetInputOverrides()[0].contextId == "runtime");
    CHECK(report.diagnostics.size() == 4);
    CHECK(report.diagnostics[0].kind ==
          EditorSettingsMigrationDiagnostic::Kind::DroppedReservedContext);
    CHECK(report.diagnostics.back().kind ==
          EditorSettingsMigrationDiagnostic::Kind::PromotedOverride);

    REQUIRE(settings.Save(err));
    const std::string saved = Read(directory / "settings.json");
    CHECK(saved.find("lastBrowseDirectory") != std::string::npos);
    CHECK(saved.find("inputOverrides") != std::string::npos);
    CHECK(saved.find("projectRoot") == std::string::npos);
    CHECK(saved.find("inputContexts") == std::string::npos);
    CHECK(saved.find("\"editor\"") == std::string::npos);
    std::filesystem::remove_all(directory);
}

TEST_CASE("Phase7 W4 project: round trip and relocation keep portable locators")
{
    const auto first = TempDir("w4_project_first");
    const auto second = TempDir("w4_project_second");
    ProjectDocument project;
    project.projectId = UUID::Parse(
        "11111111-2222-4333-8444-555555555555");
    project.assetRootLocator = "Assets";
    project.cacheRootLocator = ".rt2/cache";
    project.startupSceneLocator = "Scenes/main.rt2scene";
    project.inputContexts = {
        Context("runtime", { KeyAction("jump", 74) }),
    };
    Error err;
    REQUIRE(ProjectStore::Save(project, first / "game.rt2proj", err));
    std::filesystem::copy_file(first / "game.rt2proj",
                               second / "game.rt2proj");

    ProjectDocument loadedFirst;
    ProjectDocument loadedSecond;
    REQUIRE(ProjectStore::Load(first / "game.rt2proj", loadedFirst, err));
    REQUIRE(ProjectStore::Load(second / "game.rt2proj", loadedSecond, err));
    CHECK(loadedFirst.projectId == loadedSecond.projectId);
    CHECK(loadedFirst.assetRootLocator == loadedSecond.assetRootLocator);
    CHECK(loadedFirst.startupSceneLocator == loadedSecond.startupSceneLocator);
    CHECK(loadedFirst.assetRoot != loadedSecond.assetRoot);
    CHECK(loadedFirst.startupScene == loadedFirst.assetRoot /
          "Scenes/main.rt2scene");
    CHECK(loadedSecond.startupScene == loadedSecond.assetRoot /
          "Scenes/main.rt2scene");
    std::filesystem::remove_all(first);
    std::filesystem::remove_all(second);
}

TEST_CASE("Phase7 W4 project: invalid roots IDs and startup paths fail loudly")
{
    const auto directory = TempDir("w4_project_invalid");
    ProjectDocument project;
    project.projectId = UUID::Parse(
        "11111111-2222-4333-8444-555555555555");
    Error err;

    project.assetRootLocator = "C:/Absolute";
    CHECK_FALSE(ProjectStore::ValidateAndResolve(
        project, directory / "game.rt2proj", err));
    project.assetRootLocator = "Assets";
    project.cacheRootLocator = "Assets/cache";
    CHECK_FALSE(ProjectStore::ValidateAndResolve(
        project, directory / "game.rt2proj", err));
    project.cacheRootLocator = ".rt2/cache";
    project.startupSceneLocator = "../outside.rt2scene";
    CHECK_FALSE(ProjectStore::ValidateAndResolve(
        project, directory / "game.rt2proj", err));
    project.startupSceneLocator.clear();
    project.projectId = UUID::Nil();
    CHECK_FALSE(ProjectStore::ValidateAndResolve(
        project, directory / "game.rt2proj", err));
    std::filesystem::remove_all(directory);
}

TEST_CASE("Phase7 W4 scanner: sidecar order is deterministic and defects are loud")
{
    const UUID duplicate = UUID::Parse(
        "11111111-2222-4333-8444-555555555555");
    const UUID unique = UUID::Parse(
        "22222222-3333-4444-8555-666666666666");

    auto makeTree = [&](const std::filesystem::path& root, bool reverse) {
        const std::vector<std::pair<std::string, UUID>> assets = reverse
            ? std::vector<std::pair<std::string, UUID>>{
                { "z.glb", duplicate }, { "middle.lua", unique },
                { "a.glb", duplicate }}
            : std::vector<std::pair<std::string, UUID>>{
                { "a.glb", duplicate }, { "middle.lua", unique },
                { "z.glb", duplicate }};
        for (const auto& [name, id] : assets)
        {
            Write(root / name, "asset");
            Error sidecarError;
            REQUIRE(WriteSidecarId(AssetSidecarPath(root / name), id,
                                   sidecarError));
        }
        Write(root / "broken.obj", "asset");
        Write(AssetSidecarPath(root / "broken.obj"), "not-a-uuid\n");
        Write(root / "orphan.exr.rt2meta",
              "33333333-4444-4333-8444-555555555555\n");
    };

    const auto first = TempDir("w4_scan_first");
    const auto second = TempDir("w4_scan_second");
    makeTree(first, false);
    makeTree(second, true);
    ProjectAssetScanResult firstResult;
    ProjectAssetScanResult secondResult;
    Error err;
    REQUIRE(ScanProjectAssets(first, firstResult, err));
    REQUIRE(ScanProjectAssets(second, secondResult, err));
    REQUIRE(firstResult.database);
    REQUIRE(secondResult.database);
    CHECK(DatabaseSnapshot(*firstResult.database) ==
          DatabaseSnapshot(*secondResult.database));
    CHECK(DiagnosticSnapshot(firstResult.diagnostics) ==
          DiagnosticSnapshot(secondResult.diagnostics));
    REQUIRE(firstResult.diagnostics.size() == 3);
    CHECK(firstResult.diagnostics[0].severity == AssetDiagnostic::Stale);
    CHECK(firstResult.diagnostics[1].severity == AssetDiagnostic::Malformed);
    CHECK(firstResult.diagnostics[2].severity == AssetDiagnostic::Conflict);
    CHECK(firstResult.database->LookupById(duplicate).status ==
          AssetIdLookupResult::Status::Ambiguous);

    const auto external = TempDir("w4_scan_external");
    Write(external / "escaped.glb", "asset");
    std::error_code linkError;
    std::filesystem::create_directory_symlink(
        external, first / "linked", linkError);
    if (!linkError)
    {
        ProjectAssetScanResult linkedResult;
        REQUIRE(ScanProjectAssets(first, linkedResult, err));
        CHECK(std::any_of(
            linkedResult.diagnostics.begin(), linkedResult.diagnostics.end(),
            [](const AssetDiagnostic& diagnostic) {
                return diagnostic.severity == AssetDiagnostic::Stale &&
                       diagnostic.refPath == "linked" &&
                       diagnostic.detail.find("not traversed") !=
                           std::string::npos;
            }));
        CHECK(linkedResult.database->FindByPath("escaped.glb") == nullptr);
    }
    else
    {
        MESSAGE("directory-link assertion skipped: ", linkError.message());
    }
    std::filesystem::remove_all(first);
    std::filesystem::remove_all(second);
    std::filesystem::remove_all(external);
}

TEST_CASE("Phase7 W4 project context: failed replacement preserves the live snapshot")
{
    const auto directory = TempDir("w4_context_transaction");
    std::filesystem::create_directories(directory / "Assets");
    ProjectDocument project;
    project.projectId = UUID::Parse(
        "11111111-2222-4333-8444-555555555555");
    Error err;
    REQUIRE(ProjectStore::Save(project, directory / "game.rt2proj", err));

    ProjectContext live;
    REQUIRE(LoadProjectContext(directory / "game.rt2proj", live, err));
    const auto originalId = live.project.projectId;
    const auto* originalDatabase = live.database.get();
    Write(directory / "broken.rt2proj", "{ not json");
    CHECK_FALSE(LoadProjectContext(
        directory / "broken.rt2proj", live, err));
    CHECK(live.project.projectId == originalId);
    CHECK(live.database.get() == originalDatabase);
    CHECK(live.Assets().database == originalDatabase);

    ProjectContext invalid;
    CHECK_THROWS_AS(invalid.Assets(), std::logic_error);
    std::filesystem::remove_all(directory);
}

TEST_CASE("Phase7 W4 recovery: project records require the reloaded database context")
{
    const auto directory = TempDir("w4_project_recovery");
    const auto projectFile = directory / "game.rt2proj";
    const auto sceneFile = directory / "Assets" / "Scenes" / "main.rt2scene";
    std::filesystem::create_directories(sceneFile.parent_path());

    ProjectDocument project;
    project.projectId = UUID::Parse(
        "11111111-2222-4333-8444-555555555555");
    project.startupSceneLocator = "Scenes/main.rt2scene";
    Error err;
    REQUIRE(ProjectStore::Save(project, projectFile, err));
    ProjectContext context;
    REQUIRE(LoadProjectContext(projectFile, context, err));

    OsUuidProvider provider;
    SceneDocument document;
    document.SetUuidProvider(&provider);
    document.metadata.sourcePath = sceneFile;
    document.metadata.dirty = true;

    SceneRecoveryService recovery(
        directory / "Recovery", [] { return int64_t(1000); }, 8, 0.0);
    SceneRecoveryService::ProjectBinding binding;
    binding.projectId = project.projectId;
    binding.projectFile = projectFile;
    binding.sceneLocator = "Scenes/main.rt2scene";
    std::vector<AssetDiagnostic> diagnostics;
    CHECK_FALSE(recovery.MaybeSnapshot(
        document, 1, "unused", context.project.assetRoot,
        diagnostics, err, binding));
    REQUIRE(recovery.MaybeSnapshot(
        document, 1, "unused", context.project.assetRoot,
        diagnostics, err, binding));
    const auto records = recovery.Discover(err);
    REQUIRE(records.size() == 1);
    CHECK(records[0].projectId == project.projectId);
    CHECK(records[0].projectFile == projectFile);
    CHECK(records[0].sceneLocator == "Scenes/main.rt2scene");
    CHECK(records[0].assetRoot.empty());

    SceneDocument restored;
    restored.SetUuidProvider(&provider);
    SceneLoadReport report;
    CHECK_FALSE(recovery.Restore(
        records[0], restored, diagnostics, report, err));
    CHECK(err.detail.find("reloaded project context") != std::string::npos);
    CHECK_FALSE(recovery.Restore(
        records[0], AssetResolutionContext{ context.project.assetRoot, nullptr },
        restored, diagnostics, report, err));
    CHECK(err.detail.find("database snapshot") != std::string::npos);
    REQUIRE(recovery.Restore(
        records[0], context.Assets(), restored, diagnostics, report, err));
    CHECK(restored.metadata.sourcePath == sceneFile);
    std::filesystem::remove_all(directory);
}

TEST_CASE("Phase7 W4 resolver: aggregate resolution keeps the caller database")
{
    const auto directory = TempDir("w4_explicit_resolver_context");
    const auto modelPath = directory / "actual" / "model.glb";
    std::filesystem::create_directories(modelPath.parent_path());
    Error err;
    REQUIRE(GenerateTinyTexturedGlb(modelPath, err));
    const UUID assetId = UUID::Parse(
        "11111111-2222-4333-8444-555555555555");
    REQUIRE(WriteSidecarId(AssetSidecarPath(modelPath), assetId, err));

    AssetRecord record;
    record.assetId = assetId;
    record.sourcePath = "actual/model.glb";
    record.identityAuthority = AssetIdentityAuthority::Sidecar;
    std::vector<AssetDatabaseDiagnostic> databaseDiagnostics;
    const AssetDatabase database = BuildAssetDatabase(
        { record }, databaseDiagnostics);
    REQUIRE(databaseDiagnostics.empty());

    DeterministicUuidProvider provider;
    SceneDocument document;
    document.SetUuidProvider(&provider);
    const auto entity = document.ecs.registry.create();
    document.ecs.registry.emplace<NameComponent>(entity, "Imported");
    document.ecs.registry.emplace<Transform>(entity);
    document.ecs.registry.emplace<VisibleComponent>(entity);
    ImportedMeshSourceComponent imported;
    imported.model.kind = AssetKind::Model;
    imported.model.path = "stale/model.glb";
    imported.model.assetId = assetId;
    imported.model.sourceKey =
        SceneAssetResolver::GltfSourceKey(0, 0, 0, 0);
    document.ecs.registry.emplace<ImportedMeshSourceComponent>(
        entity, imported);
    document.AssignNewUuid(entity);

    std::vector<AssetDiagnostic> diagnostics;
    REQUIRE(SceneAssetResolver::ResolveAll(
        document, AssetResolutionContext{ directory, &database },
        diagnostics, err));
    CHECK(document.ecs.registry.all_of<MeshRef>(entity));
    CHECK(document.ecs.meshRegistry.GetCount() == 1);
    CHECK(std::any_of(diagnostics.begin(), diagnostics.end(),
        [](const AssetDiagnostic& diagnostic) {
            return diagnostic.severity == AssetDiagnostic::Stale;
        }));
    std::filesystem::remove_all(directory);
}
