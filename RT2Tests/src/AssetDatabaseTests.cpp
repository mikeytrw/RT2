#include <doctest/doctest.h>

#include "AssetDatabase.h"
#include "core/UUID.h"

#include <algorithm>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace rt2::core;

// ============================================================================
// Phase 7 W2/W3 foundation: CPU-only authoritative AssetDatabase tests.
// ============================================================================

namespace {

AssetRecord MakeRecord(const std::string& path, AssetKind kind,
                       const UUID& id = UUID::Nil())
{
    AssetRecord record;
    record.sourcePath = path;
    record.assetId = id;
    record.identityAuthority = id.IsNull()
        ? AssetIdentityAuthority::Reference
        : AssetIdentityAuthority::Sidecar;
    if (kind != AssetKind::Unknown)
        record.observedKinds.push_back(kind);
    return record;
}

AssetDependencyRecord MakeDependency(const std::string& sourceKey,
                                     const std::string& path,
                                     AssetKind kind,
                                     const UUID& id = UUID::Nil())
{
    AssetDependencyRecord dependency;
    dependency.sourceKey = sourceKey;
    dependency.sourcePath = path;
    dependency.kind = kind;
    dependency.assetId = id;
    return dependency;
}

std::string Snapshot(
    const AssetDatabase& database,
    const std::vector<AssetDatabaseDiagnostic>& diagnostics,
    const std::vector<UUID>& lookupIds)
{
    std::ostringstream out;
    for (const auto& record : database.AllRecordsSorted())
    {
        out << "record|" << record.sourcePath
            << "|" << record.assetId.ToString()
            << "|" << static_cast<int>(record.identityAuthority)
            << "|" << record.importSettings.triangulate
            << record.importSettings.generateNormals
            << record.importSettings.mergeMegaMesh;
        for (AssetKind kind : record.observedKinds)
            out << "|kind:" << static_cast<int>(kind);
        for (const UUID& entity : record.dependentEntities)
            out << "|entity:" << entity.ToString();
        for (const auto& dependency : record.dependencies)
        {
            out << "|dep:" << dependency.sourceKey
                << ":" << dependency.sourcePath
                << ":" << dependency.assetId.ToString()
                << ":" << static_cast<int>(dependency.kind);
        }
        out << "\n";
    }

    for (const UUID& id : lookupIds)
    {
        const auto lookup = database.LookupById(id);
        out << "lookup|" << id.ToString()
            << "|" << static_cast<int>(lookup.status);
        for (const auto& path : lookup.candidatePaths)
            out << "|" << path;
        out << "\n";
    }

    for (const auto& diagnostic : diagnostics)
    {
        out << "diagnostic|" << static_cast<int>(diagnostic.kind)
            << "|" << diagnostic.assetId.ToString()
            << "|" << diagnostic.sourcePath;
        for (const auto& path : diagnostic.candidatePaths)
            out << "|" << path;
        out << "|" << diagnostic.detail << "\n";
    }
    return out.str();
}

} // namespace

TEST_CASE("Phase7 W3 database: unique ID lookup returns its only claimant")
{
    AssetDatabase database;
    std::vector<AssetDatabaseDiagnostic> diagnostics;
    const UUID id =
        UUID::Parse("11111111-2222-4333-8444-555555555555");

    database.AddOrUpdate(
        MakeRecord("assets/cube.glb", AssetKind::Model, id),
        diagnostics);
    CHECK(diagnostics.empty());
    CHECK(database.Size() == 1);

    const AssetRecord* byPath = database.FindByPath("assets/cube.glb");
    REQUIRE(byPath);
    CHECK(byPath->assetId == id);
    REQUIRE(byPath->observedKinds.size() == 1);
    CHECK(byPath->observedKinds[0] == AssetKind::Model);

    const auto lookup = database.LookupById(id);
    CHECK(lookup.status == AssetIdLookupResult::Status::Unique);
    REQUIRE(lookup.record);
    CHECK(lookup.record->sourcePath == "assets/cube.glb");
    CHECK(lookup.candidatePaths ==
          std::vector<std::string>{ "assets/cube.glb" });
}

TEST_CASE("Phase7 W3 database: nil and unclaimed ID lookup are Missing")
{
    AssetDatabase database;
    const auto nil = database.LookupById(UUID::Nil());
    CHECK(nil.status == AssetIdLookupResult::Status::Missing);
    CHECK(nil.record == nullptr);
    CHECK(nil.candidatePaths.empty());

    const auto unknown = database.LookupById(
        UUID::Parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"));
    CHECK(unknown.status == AssetIdLookupResult::Status::Missing);
    CHECK(unknown.record == nullptr);
    CHECK(unknown.candidatePaths.empty());
}

TEST_CASE("Phase7 W3 database: nil update preserves an existing ID")
{
    AssetDatabase database;
    std::vector<AssetDatabaseDiagnostic> diagnostics;
    const UUID id =
        UUID::Parse("11111111-2222-4333-8444-555555555555");
    database.AddOrUpdate(
        MakeRecord("assets/cube.glb", AssetKind::Model, id),
        diagnostics);
    database.AddOrUpdate(
        MakeRecord("assets/cube.glb", AssetKind::Texture),
        diagnostics);

    const AssetRecord* record =
        database.FindByPath("assets/cube.glb");
    REQUIRE(record);
    CHECK(record->assetId == id);
    REQUIRE(record->observedKinds.size() == 2);
    CHECK(record->observedKinds[0] == AssetKind::Model);
    CHECK(record->observedKinds[1] == AssetKind::Texture);
    CHECK(diagnostics.empty());
}

TEST_CASE("Phase7 W3 database: sidecar record assigns an ID to a placeholder")
{
    AssetDatabase database;
    std::vector<AssetDatabaseDiagnostic> diagnostics;
    const UUID entity =
        UUID::Parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
    const UUID id =
        UUID::Parse("11111111-2222-4333-8444-555555555555");

    database.AddEntityDependency("assets/cube.glb", entity);
    REQUIRE(database.FindByPath("assets/cube.glb")->assetId.IsNull());
    database.AddOrUpdate(
        MakeRecord("assets/cube.glb", AssetKind::Model, id),
        diagnostics);

    CHECK(diagnostics.empty());
    const auto lookup = database.LookupById(id);
    CHECK(lookup.status == AssetIdLookupResult::Status::Unique);
    REQUIRE(lookup.record);
    REQUIRE(lookup.record->dependentEntities.size() == 1);
    CHECK(lookup.record->dependentEntities[0] == entity);
}

TEST_CASE("Phase7 W3 database: conflicting IDs on one path remain loud")
{
    AssetDatabase database;
    std::vector<AssetDatabaseDiagnostic> diagnostics;
    const UUID stored =
        UUID::Parse("11111111-2222-4333-8444-555555555555");
    const UUID conflicting =
        UUID::Parse("99999999-9999-4999-8999-999999999999");

    database.AddOrUpdate(
        MakeRecord("assets/cube.glb", AssetKind::Model, stored),
        diagnostics);
    database.AddOrUpdate(
        MakeRecord("assets/cube.glb", AssetKind::Model, conflicting),
        diagnostics);

    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].kind ==
          AssetDatabaseDiagnostic::Kind::ConflictingId);
    CHECK(diagnostics[0].assetId == conflicting);
    CHECK(database.FindByPath("assets/cube.glb")->assetId == stored);
    CHECK(database.LookupById(stored).status ==
          AssetIdLookupResult::Status::Unique);
    CHECK(database.LookupById(conflicting).status ==
          AssetIdLookupResult::Status::Missing);
}

TEST_CASE("Phase7 W3 database: sidecar identity wins a same-path conflict in either input order")
{
    const UUID referenceId =
        UUID::Parse("11111111-2222-4333-8444-555555555555");
    const UUID sidecarId =
        UUID::Parse("99999999-9999-4999-8999-999999999999");

    AssetRecord reference =
        MakeRecord("assets/cube.glb", AssetKind::Model, referenceId);
    reference.identityAuthority =
        AssetIdentityAuthority::Reference;
    AssetRecord sidecar =
        MakeRecord("assets/cube.glb", AssetKind::Model, sidecarId);
    sidecar.identityAuthority =
        AssetIdentityAuthority::Sidecar;

    std::vector<AssetDatabaseDiagnostic> forwardDiagnostics;
    std::vector<AssetDatabaseDiagnostic> reverseDiagnostics;
    const AssetDatabase forward = BuildAssetDatabase(
        { reference, sidecar }, forwardDiagnostics);
    const AssetDatabase reverse = BuildAssetDatabase(
        { sidecar, reference }, reverseDiagnostics);

    REQUIRE(forward.FindByPath("assets/cube.glb"));
    REQUIRE(reverse.FindByPath("assets/cube.glb"));
    CHECK(forward.FindByPath("assets/cube.glb")->assetId == sidecarId);
    CHECK(reverse.FindByPath("assets/cube.glb")->assetId == sidecarId);
    CHECK(forward.LookupById(referenceId).status ==
          AssetIdLookupResult::Status::Missing);
    CHECK(forward.LookupById(sidecarId).status ==
          AssetIdLookupResult::Status::Unique);
    CHECK(Snapshot(forward, forwardDiagnostics,
                   { referenceId, sidecarId }) ==
          Snapshot(reverse, reverseDiagnostics,
                   { referenceId, sidecarId }));
}

TEST_CASE("Phase7 W3 database: duplicate ID preserves every claimant and is Ambiguous")
{
    AssetDatabase database;
    std::vector<AssetDatabaseDiagnostic> diagnostics;
    const UUID id =
        UUID::Parse("11111111-2222-4333-8444-555555555555");

    database.AddOrUpdate(
        MakeRecord("assets/z.glb", AssetKind::Model, id),
        diagnostics);
    database.AddOrUpdate(
        MakeRecord("assets/a.glb", AssetKind::Model, id),
        diagnostics);

    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].kind ==
          AssetDatabaseDiagnostic::Kind::DuplicateId);
    CHECK(diagnostics[0].candidatePaths ==
          std::vector<std::string>{ "assets/a.glb", "assets/z.glb" });

    // Neither claimant is sanitized and no winner is exposed.
    CHECK(database.FindByPath("assets/a.glb")->assetId == id);
    CHECK(database.FindByPath("assets/z.glb")->assetId == id);
    const auto lookup = database.LookupById(id);
    CHECK(lookup.status == AssetIdLookupResult::Status::Ambiguous);
    CHECK(lookup.record == nullptr);
    CHECK(lookup.candidatePaths ==
          std::vector<std::string>{ "assets/a.glb", "assets/z.glb" });
}

TEST_CASE("Phase7 W3 database: reversed duplicate insertion has identical lookup and diagnostics")
{
    const UUID id =
        UUID::Parse("11111111-2222-4333-8444-555555555555");
    const std::vector<AssetRecord> forward = {
        MakeRecord("assets/a.glb", AssetKind::Model, id),
        MakeRecord("assets/z.glb", AssetKind::Model, id),
    };
    const std::vector<AssetRecord> reverse = {
        MakeRecord("assets/z.glb", AssetKind::Model, id),
        MakeRecord("assets/a.glb", AssetKind::Model, id),
    };

    std::vector<AssetDatabaseDiagnostic> forwardDiagnostics;
    std::vector<AssetDatabaseDiagnostic> reverseDiagnostics;
    const AssetDatabase forwardDatabase =
        BuildAssetDatabase(forward, forwardDiagnostics);
    const AssetDatabase reverseDatabase =
        BuildAssetDatabase(reverse, reverseDiagnostics);

    CHECK(Snapshot(forwardDatabase, forwardDiagnostics, { id }) ==
          Snapshot(reverseDatabase, reverseDiagnostics, { id }));
}

TEST_CASE("Phase7 W3 database: entity dependency creates a placeholder")
{
    AssetDatabase database;
    const UUID entity =
        UUID::Parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
    database.AddEntityDependency("assets/cube.glb", entity);

    CHECK(database.Size() == 1);
    const AssetRecord* record =
        database.FindByPath("assets/cube.glb");
    REQUIRE(record);
    CHECK(record->assetId.IsNull());
    REQUIRE(record->dependentEntities.size() == 1);
    CHECK(record->dependentEntities[0] == entity);
}

TEST_CASE("Phase7 W3 database: entity dependency is idempotent")
{
    AssetDatabase database;
    const UUID entity =
        UUID::Parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
    database.AddEntityDependency("assets/cube.glb", entity);
    database.AddEntityDependency("assets/cube.glb", entity);

    const AssetRecord* record =
        database.FindByPath("assets/cube.glb");
    REQUIRE(record);
    CHECK(record->dependentEntities.size() == 1);
}

TEST_CASE("Phase7 W3 database: entity dependencies are sorted by UUID")
{
    AssetDatabase database;
    const UUID high =
        UUID::Parse("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    const UUID low =
        UUID::Parse("11111111-2222-4333-8444-555555555555");
    database.AddEntityDependency("assets/cube.glb", high);
    database.AddEntityDependency("assets/cube.glb", low);

    const AssetRecord* record =
        database.FindByPath("assets/cube.glb");
    REQUIRE(record);
    REQUIRE(record->dependentEntities.size() == 2);
    CHECK(record->dependentEntities[0] == low);
    CHECK(record->dependentEntities[1] == high);
}

TEST_CASE("Phase7 W3 database: cross-asset dependency is keyed by stable source key")
{
    AssetDatabase database;
    std::vector<AssetDatabaseDiagnostic> diagnostics;
    const UUID modelId =
        UUID::Parse("11111111-2222-4333-8444-555555555555");
    const UUID textureId =
        UUID::Parse("22222222-3333-4444-8555-666666666666");
    database.AddOrUpdate(
        MakeRecord("assets/model.glb", AssetKind::Model, modelId),
        diagnostics);
    database.AddAssetDependency(
        "assets/model.glb",
        MakeDependency("gltf:image=0", "assets/albedo.png",
                       AssetKind::Texture, textureId),
        diagnostics);

    CHECK(diagnostics.empty());
    const auto dependencies = database.FindDependenciesBySourceKey(
        "assets/model.glb", "gltf:image=0");
    REQUIRE(dependencies.size() == 1);
    CHECK(dependencies[0].assetId == textureId);
    CHECK(dependencies[0].sourcePath == "assets/albedo.png");
    CHECK(dependencies[0].kind == AssetKind::Texture);

    const auto target = database.LookupById(textureId);
    CHECK(target.status == AssetIdLookupResult::Status::Unique);
    REQUIRE(target.record);
    REQUIRE(target.record->observedKinds.size() == 1);
    CHECK(target.record->observedKinds[0] == AssetKind::Texture);
}

TEST_CASE("Phase7 W3 database: exact cross-asset dependency is idempotent")
{
    AssetDatabase database;
    std::vector<AssetDatabaseDiagnostic> diagnostics;
    const auto dependency = MakeDependency(
        "gltf:image=0", "assets/albedo.png", AssetKind::Texture);
    database.AddAssetDependency(
        "assets/model.glb", dependency, diagnostics);
    database.AddAssetDependency(
        "assets/model.glb", dependency, diagnostics);

    const auto dependencies = database.FindDependenciesBySourceKey(
        "assets/model.glb", "gltf:image=0");
    CHECK(dependencies.size() == 1);
}

TEST_CASE("Phase7 W3 database: conflicting claims at one source key are preserved and sorted")
{
    AssetDatabase database;
    std::vector<AssetDatabaseDiagnostic> diagnostics;
    database.AddAssetDependency(
        "assets/model.glb",
        MakeDependency("gltf:image=0", "assets/z.png",
                       AssetKind::Texture),
        diagnostics);
    database.AddAssetDependency(
        "assets/model.glb",
        MakeDependency("gltf:image=0", "assets/a.png",
                       AssetKind::Texture),
        diagnostics);

    const auto dependencies = database.FindDependenciesBySourceKey(
        "assets/model.glb", "gltf:image=0");
    REQUIRE(dependencies.size() == 2);
    CHECK(dependencies[0].sourcePath == "assets/a.png");
    CHECK(dependencies[1].sourcePath == "assets/z.png");
}

TEST_CASE("Phase7 W3 database: one physical file accumulates multiple observed uses")
{
    AssetDatabase database;
    std::vector<AssetDatabaseDiagnostic> diagnostics;
    const UUID id =
        UUID::Parse("11111111-2222-4333-8444-555555555555");
    database.AddOrUpdate(
        MakeRecord("assets/lighting.hdr", AssetKind::Environment, id),
        diagnostics);
    database.AddAssetDependency(
        "assets/model.glb",
        MakeDependency("gltf:image=2", "assets/lighting.hdr",
                       AssetKind::Texture, id),
        diagnostics);

    CHECK(diagnostics.empty());
    const AssetRecord* record =
        database.FindByPath("assets/lighting.hdr");
    REQUIRE(record);
    REQUIRE(record->observedKinds.size() == 2);
    CHECK(record->observedKinds[0] == AssetKind::Texture);
    CHECK(record->observedKinds[1] == AssetKind::Environment);
}

TEST_CASE("Phase7 W3 database: BuildAssetDatabase random permutations are byte-identical")
{
    const UUID duplicateId =
        UUID::Parse("11111111-2222-4333-8444-555555555555");
    const UUID uniqueId =
        UUID::Parse("22222222-3333-4444-8555-666666666666");
    const UUID entityLow =
        UUID::Parse("33333333-4444-4333-8444-555555555555");
    const UUID entityHigh =
        UUID::Parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");

    AssetRecord a =
        MakeRecord("assets/a.glb", AssetKind::Model, duplicateId);
    a.dependentEntities = { entityHigh, entityLow, entityLow };
    a.dependencies = {
        MakeDependency("gltf:image=1", "assets/z.png",
                       AssetKind::Texture),
        MakeDependency("gltf:image=0", "assets/a.png",
                       AssetKind::Texture),
        MakeDependency("gltf:image=0", "assets/a.png",
                       AssetKind::Texture),
    };
    AssetRecord z =
        MakeRecord("assets/z.glb", AssetKind::Model, duplicateId);
    AssetRecord middle =
        MakeRecord("assets/middle.hdr", AssetKind::Environment, uniqueId);
    AssetRecord middleSecondUse =
        MakeRecord("assets/middle.hdr", AssetKind::Texture);
    middleSecondUse.importSettings.generateNormals = true;

    const std::vector<AssetRecord> source = {
        a, z, middle, middleSecondUse,
    };
    const std::vector<UUID> lookupIds = { duplicateId, uniqueId };

    std::vector<AssetDatabaseDiagnostic> baselineDiagnostics;
    const AssetDatabase baselineDatabase =
        BuildAssetDatabase(source, baselineDiagnostics);
    const std::string baseline = Snapshot(
        baselineDatabase, baselineDiagnostics, lookupIds);

    std::mt19937 random(0x7a53d19u);
    for (int iteration = 0; iteration < 64; ++iteration)
    {
        std::vector<AssetRecord> shuffled = source;
        std::shuffle(shuffled.begin(), shuffled.end(), random);
        for (auto& record : shuffled)
        {
            std::shuffle(record.observedKinds.begin(),
                         record.observedKinds.end(), random);
            std::shuffle(record.dependentEntities.begin(),
                         record.dependentEntities.end(), random);
            std::shuffle(record.dependencies.begin(),
                         record.dependencies.end(), random);
        }

        std::vector<AssetDatabaseDiagnostic> diagnostics;
        const AssetDatabase database =
            BuildAssetDatabase(std::move(shuffled), diagnostics);
        CAPTURE(iteration);
        CHECK(Snapshot(database, diagnostics, lookupIds) == baseline);
    }
}

TEST_CASE("Phase7 W3 database: AllRecordsSorted is sorted by source path")
{
    AssetDatabase database;
    std::vector<AssetDatabaseDiagnostic> diagnostics;
    database.AddOrUpdate(
        MakeRecord("assets/zebra.glb", AssetKind::Model), diagnostics);
    database.AddOrUpdate(
        MakeRecord("assets/apple.glb", AssetKind::Model), diagnostics);
    database.AddOrUpdate(
        MakeRecord("assets/mango.glb", AssetKind::Model), diagnostics);

    const auto records = database.AllRecordsSorted();
    REQUIRE(records.size() == 3);
    CHECK(records[0].sourcePath == "assets/apple.glb");
    CHECK(records[1].sourcePath == "assets/mango.glb");
    CHECK(records[2].sourcePath == "assets/zebra.glb");
}

TEST_CASE("Phase7 W3 database: empty database has no records or dependencies")
{
    AssetDatabase database;
    CHECK(database.Size() == 0);
    CHECK(database.AllRecordsSorted().empty());
    CHECK(database.FindByPath("anything") == nullptr);
    CHECK(database.LookupById(UUID::Nil()).status ==
          AssetIdLookupResult::Status::Missing);
    CHECK(database.FindDependenciesBySourceKey(
              "anything", "gltf:image=0").empty());
}
