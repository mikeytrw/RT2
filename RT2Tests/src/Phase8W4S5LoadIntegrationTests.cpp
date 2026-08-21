#include <doctest/doctest.h>

#include "AssetIdentity.h"
#include "PrefabPropagationService.h"
#include "SceneRecoveryService.h"
#include "SceneSerializer.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

using namespace rt2::core;

namespace
{
UUID U(unsigned value)
{
    char text[48]{};
    std::snprintf(text, sizeof(text), "00000000-0000-4000-8000-%012u", value);
    return UUID::Parse(text);
}

const UUID kAsset = U(9000);
const UUID kInstanceA = U(9100);
const UUID kInstanceB = U(9200);

struct TempGuard
{
    std::filesystem::path directory;
    explicit TempGuard(std::filesystem::path value) : directory(std::move(value)) {}
    TempGuard(const TempGuard&) = delete;
    TempGuard& operator=(const TempGuard&) = delete;
    TempGuard(TempGuard&& other) noexcept : directory(std::move(other.directory))
    { other.directory.clear(); }
    ~TempGuard()
    {
        if (directory.empty()) return;
        std::error_code ec;
        std::filesystem::remove_all(directory, ec);
        if (ec || std::filesystem::exists(directory, ec)) std::abort();
    }
};

TempGuard Temp()
{
    static std::atomic<unsigned> sequence{0};
    TempGuard result{std::filesystem::temp_directory_path() /
        ("rt2_w4_s5_load_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
         "_" + std::to_string(sequence.fetch_add(1)))};
    std::error_code ec;
    if (!std::filesystem::create_directories(result.directory, ec) || ec)
        throw std::runtime_error("S5 temporary directory creation failed");
    return result;
}

std::filesystem::path WritePrefab(const TempGuard& temp, bool malformed = false)
{
    const auto source = temp.directory / "source.rt2prefab";
    Error error;
    if (malformed)
    {
        std::ofstream output(source, std::ios::binary);
        output << "not a prefab";
        output.close();
    }
    else
    {
        PrefabDocument prefab;
        PrefabEntityRecord root;
        root.templateId = U(1);
        root.record.uuid = U(100);
        root.record.name = "SourceRoot";
        root.record.translation = {1.0f, 2.0f, 3.0f};
        PrefabEntityRecord child;
        child.templateId = U(2);
        child.record.uuid = U(101);
        child.record.parentUuid = U(100);
        child.record.name = "SourceChild";
        child.record.translation = {4.0f, 5.0f, 6.0f};
        prefab.entities = {root, child};
        REQUIRE(PrefabSerializer::Save(prefab, source, error));
    }
    REQUIRE(WriteSidecarId(AssetSidecarPath(source), kAsset, error));
    return source;
}

entt::entity AddMember(SceneDocument& document, const UUID& entityUuid,
                       const UUID& instance, const UUID& templateId,
                       entt::entity parent)
{
    const auto entity = document.ecs.registry.create();
    REQUIRE(document.AssignKnownUuid(entity, entityUuid));
    document.ecs.registry.emplace<Hierarchy>(entity, parent,
                                               std::vector<entt::entity>{});
    document.ecs.registry.emplace<PrefabMemberComponent>(
        entity, PrefabMemberComponent{instance, templateId, {}});
    document.ecs.registry.emplace<NameComponent>(entity, NameComponent{"Old"});
    document.ecs.registry.emplace<Transform>(entity);
    document.ecs.registry.emplace<VisibleComponent>(entity);
    if (parent != entt::null)
        document.ecs.registry.get<Hierarchy>(parent).children.push_back(entity);
    return entity;
}

SceneDocument MakeDocument(const std::filesystem::path& source,
                           const UUID& instance = kInstanceA,
                           bool addMalformedSibling = false)
{
    SceneDocument document;
    document.metadata.schemaVersion = SceneSerializer::SchemaVersion;
    document.metadata.assetRoot = source.parent_path();
    const auto root = document.ecs.registry.create();
    REQUIRE(document.AssignKnownUuid(root, U(200)));
    document.ecs.registry.emplace<Hierarchy>(root);
    document.ecs.registry.emplace<PrefabInstanceComponent>(root,
        PrefabInstanceComponent{AssetReference{AssetKind::Prefab,
            source.filename().string(), {}, {}, kAsset}, instance});
    document.ecs.registry.emplace<PrefabMemberComponent>(root,
        PrefabMemberComponent{instance, U(1), {}});
    document.ecs.registry.emplace<NameComponent>(root, NameComponent{"OldRoot"});
    document.ecs.registry.emplace<Transform>(root);
    document.ecs.registry.emplace<VisibleComponent>(root);
    AddMember(document, U(201), instance, U(2), root);

    if (addMalformedSibling)
    {
        const auto badRoot = document.ecs.registry.create();
        REQUIRE(document.AssignKnownUuid(badRoot, U(300)));
        document.ecs.registry.emplace<Hierarchy>(badRoot);
        document.ecs.registry.emplace<PrefabInstanceComponent>(badRoot,
            PrefabInstanceComponent{AssetReference{AssetKind::Prefab,
                source.filename().string(), {}, {}, kAsset}, kInstanceB});
        document.ecs.registry.emplace<PrefabMemberComponent>(badRoot,
            PrefabMemberComponent{kInstanceB, U(1), {}});
        document.ecs.registry.emplace<NameComponent>(badRoot, NameComponent{"BadOld"});
        document.ecs.registry.emplace<Transform>(badRoot);
        document.ecs.registry.emplace<VisibleComponent>(badRoot);
        // The extra template ID deliberately quarantines only this instance.
        AddMember(document, U(301), kInstanceB, U(999), badRoot);
    }
    return document;
}

std::string Bytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
}

TEST_CASE("Phase 8 W4 S5: load reconciliation is deterministic, dirty, and sibling-safe")
{
    const auto temp = Temp();
    const auto source = WritePrefab(temp);
    auto document = MakeDocument(source, kInstanceA, true);
    const auto badBefore = document.ecs.registry.get<NameComponent>(
        document.FindByUuid(U(301))).name;

    const auto report = ReconcilePrefabPropagationForLoad(
        document, AssetResolutionContext{temp.directory, nullptr});
    REQUIRE(report.IsOk());
    CHECK(report.value.changed);
    CHECK(report.value.propagatedInstances == 1);
    CHECK(report.value.quarantinedInstances == 1);
    CHECK(document.metadata.dirty);
    CHECK(document.ecs.registry.get<NameComponent>(document.FindByUuid(U(201))).name ==
          "SourceChild");
    CHECK(document.ecs.registry.get<Transform>(document.FindByUuid(U(201))).translation.x ==
          doctest::Approx(4.0f));
    CHECK(document.ecs.registry.get<NameComponent>(document.FindByUuid(U(301))).name ==
          badBefore);
    CHECK(report.value.diagnostics.size() == 1);

    document.metadata.dirty = false;
    const auto noOp = ReconcilePrefabPropagationForLoad(
        document, AssetResolutionContext{temp.directory, nullptr});
    REQUIRE(noOp.IsOk());
    CHECK_FALSE(noOp.value.changed);
    CHECK_FALSE(document.metadata.dirty);
}

TEST_CASE("Phase 8 W4 S5: global source failure is transactional")
{
    const auto temp = Temp();
    const auto source = WritePrefab(temp, true);
    auto document = MakeDocument(source);
    document.metadata.dirty = false;
    const auto before = document.ecs.registry.get<NameComponent>(
        document.FindByUuid(U(201))).name;
    const auto result = ReconcilePrefabPropagationForLoad(
        document, AssetResolutionContext{temp.directory, nullptr});
    CHECK_FALSE(result.IsOk());
    CHECK_FALSE(document.metadata.dirty);
    CHECK(document.ecs.registry.get<NameComponent>(document.FindByUuid(U(201))).name ==
          before);
}

TEST_CASE("Phase 8 W4 S5: recovery restores and reconciles before asset resolution")
{
    const auto temp = Temp();
    const auto source = WritePrefab(temp);
    auto sourceDocument = MakeDocument(source);
    const auto scenePath = temp.directory / "scene.rt2scene";
    std::vector<AssetDiagnostic> saveDiagnostics;
    Error error;
    REQUIRE(SceneSerializer::Save(sourceDocument, scenePath, saveDiagnostics, error));

    const auto recoveryRoot = temp.directory / "Recovery";
    std::filesystem::create_directories(recoveryRoot);
    SceneRecoveryService service(recoveryRoot);
    SceneRecoveryService::RecoveryRecord record;
    record.recordPath = recoveryRoot / "scene.rt2recovery";
    record.originalSourcePath = scenePath;
    record.assetRoot = temp.directory;
    record.snapshotJson = Bytes(scenePath);
    record.valid = true;

    SceneDocument restored;
    std::vector<AssetDiagnostic> diagnostics;
    SceneLoadReport loadReport;
    const bool restoredOk = service.Restore(record,
        AssetResolutionContext{temp.directory, nullptr}, restored, diagnostics,
        loadReport, error);
    INFO(error.Format());
    REQUIRE(restoredOk);
    CHECK(restored.metadata.dirty);
    CHECK(restored.ecs.registry.get<NameComponent>(restored.FindByUuid(U(201))).name ==
          "SourceChild");
}
