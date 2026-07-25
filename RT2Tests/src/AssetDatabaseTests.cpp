#include <doctest/doctest.h>

#include "AssetDatabase.h"
#include "core/UUID.h"

#include <algorithm>
#include <vector>

using namespace rt2::core;

// ============================================================================
// Phase 7 W2: AssetDatabase — the in-memory record store (CPU-only, per
// the ScriptFieldReconcile precedent). Per D8 the sidecar files are the
// source of truth; this database is an index/cache built from them plus the
// scene's AssetReferences. Determinism: builders sort by sourcePath before
// recording so the result never depends on directory enumeration order.
// ============================================================================

namespace {

AssetRecord MakeRecord(const std::string& path, AssetKind kind,
                        const UUID& id = UUID::Nil())
{
    AssetRecord r;
    r.sourcePath = path;
    r.kind = kind;
    r.assetId = id;
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// Basic insertion and lookup
// ---------------------------------------------------------------------------

TEST_CASE("Phase7 W2: AddOrUpdate inserts a new record and is findable by path and ID")
{
    AssetDatabase db;
    std::vector<AssetDatabaseDiagnostic> diags;
    const UUID id = UUID::Parse("11111111-2222-4333-8444-555555555555");
    db.AddOrUpdate(MakeRecord("assets/cube.glb", AssetKind::Model, id), diags);
    CHECK(diags.empty());
    CHECK(db.Size() == 1);

    const AssetRecord* byPath = db.FindByPath("assets/cube.glb");
    REQUIRE(byPath != nullptr);
    CHECK(byPath->assetId == id);
    CHECK(byPath->kind == AssetKind::Model);

    const AssetRecord* byId = db.FindById(id);
    REQUIRE(byId != nullptr);
    CHECK(byId->sourcePath == "assets/cube.glb");

    // A nil ID is not in the ID index.
    CHECK(db.FindById(UUID::Nil()) == nullptr);
}

TEST_CASE("Phase7 W2: AddOrUpdate merges an existing record, preserving the stored ID")
{
    AssetDatabase db;
    std::vector<AssetDatabaseDiagnostic> diags;
    const UUID id = UUID::Parse("11111111-2222-4333-8444-555555555555");
    db.AddOrUpdate(MakeRecord("assets/cube.glb", AssetKind::Model, id), diags);

    // A second record for the same path with a nil ID: kind/importSettings
    // update, ID is preserved.
    db.AddOrUpdate(MakeRecord("assets/cube.glb", AssetKind::Model, UUID::Nil()), diags);
    CHECK(db.Size() == 1);
    const AssetRecord* r = db.FindByPath("assets/cube.glb");
    REQUIRE(r);
    CHECK(r->assetId == id); // preserved
    CHECK(diags.empty());
}

TEST_CASE("Phase7 W2: AddOrUpdate with a conflicting ID reports a ConflictingId diagnostic and keeps the stored ID")
{
    AssetDatabase db;
    std::vector<AssetDatabaseDiagnostic> diags;
    const UUID stored = UUID::Parse("11111111-2222-4333-8444-555555555555");
    const UUID conflicting = UUID::Parse("99999999-9999-4999-8999-999999999999");
    db.AddOrUpdate(MakeRecord("assets/cube.glb", AssetKind::Model, stored), diags);

    db.AddOrUpdate(MakeRecord("assets/cube.glb", AssetKind::Model, conflicting), diags);
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].kind == AssetDatabaseDiagnostic::Kind::ConflictingId);
    CHECK(diags[0].assetId == conflicting);
    CHECK(diags[0].sourcePath == "assets/cube.glb");

    const AssetRecord* r = db.FindByPath("assets/cube.glb");
    REQUIRE(r);
    CHECK(r->assetId == stored); // stored wins (sidecar is source of truth)
}

// ---------------------------------------------------------------------------
// Duplicate detection
// ---------------------------------------------------------------------------

TEST_CASE("Phase7 W2: two different paths claiming the same ID emit a DuplicateId diagnostic")
{
    AssetDatabase db;
    std::vector<AssetDatabaseDiagnostic> diags;
    const UUID id = UUID::Parse("11111111-2222-4333-8444-555555555555");
    db.AddOrUpdate(MakeRecord("assets/a.glb", AssetKind::Model, id), diags);
    db.AddOrUpdate(MakeRecord("assets/b.glb", AssetKind::Model, id), diags);

    REQUIRE(diags.size() == 1);
    CHECK(diags[0].kind == AssetDatabaseDiagnostic::Kind::DuplicateId);
    CHECK(diags[0].assetId == id);
    CHECK(diags[0].sourcePath == "assets/b.glb"); // the second one loses

    // The first path keeps the ID; the second is left with nil.
    CHECK(db.FindByPath("assets/a.glb")->assetId == id);
    CHECK(db.FindByPath("assets/b.glb")->assetId.IsNull());
    CHECK(db.FindById(id)->sourcePath == "assets/a.glb");
}

// ---------------------------------------------------------------------------
// Dependencies
// ---------------------------------------------------------------------------

TEST_CASE("Phase7 W2: AddDependency creates a placeholder record for an unknown path")
{
    AssetDatabase db;
    const UUID entity = UUID::Parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
    db.AddDependency("assets/cube.glb", entity);
    CHECK(db.Size() == 1);
    const AssetRecord* r = db.FindByPath("assets/cube.glb");
    REQUIRE(r);
    CHECK(r->assetId.IsNull()); // placeholder until a sidecar is scanned
    REQUIRE(r->dependents.size() == 1);
    CHECK(r->dependents[0] == entity);
}

TEST_CASE("Phase7 W2: AddDependency is idempotent for the same entity")
{
    AssetDatabase db;
    const UUID entity = UUID::Parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
    db.AddDependency("assets/cube.glb", entity);
    db.AddDependency("assets/cube.glb", entity);
    const AssetRecord* r = db.FindByPath("assets/cube.glb");
    REQUIRE(r);
    CHECK(r->dependents.size() == 1);
}

TEST_CASE("Phase7 W2: AddDependency accumulates distinct entities")
{
    AssetDatabase db;
    const UUID e1 = UUID::Parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
    const UUID e2 = UUID::Parse("11111111-2222-4333-8444-555555555555");
    db.AddDependency("assets/cube.glb", e1);
    db.AddDependency("assets/cube.glb", e2);
    const AssetRecord* r = db.FindByPath("assets/cube.glb");
    REQUIRE(r);
    CHECK(r->dependents.size() == 2);
}

// ---------------------------------------------------------------------------
// Determinism (the core W2 requirement)
// ---------------------------------------------------------------------------

TEST_CASE("Phase7 W2: BuildAssetDatabase produces the same records regardless of input order")
{
    const UUID idA = UUID::Parse("11111111-2222-4333-8444-555555555555");
    const UUID idB = UUID::Parse("22222222-3333-4444-8555-666666666666");
    const UUID idC = UUID::Parse("33333333-4444-4333-8444-555555555555");

    auto buildForward = [&]() {
        std::vector<AssetDatabaseDiagnostic> diags;
        std::vector<AssetRecord> recs = {
            MakeRecord("assets/a.glb", AssetKind::Model, idA),
            MakeRecord("assets/b.glb", AssetKind::Model, idB),
            MakeRecord("assets/c.glb", AssetKind::Model, idC),
        };
        return BuildAssetDatabase(std::move(recs), diags).AllRecordsSorted();
    };
    auto buildReverse = [&]() {
        std::vector<AssetDatabaseDiagnostic> diags;
        std::vector<AssetRecord> recs = {
            MakeRecord("assets/c.glb", AssetKind::Model, idC),
            MakeRecord("assets/b.glb", AssetKind::Model, idB),
            MakeRecord("assets/a.glb", AssetKind::Model, idA),
        };
        return BuildAssetDatabase(std::move(recs), diags).AllRecordsSorted();
    };

    const auto fwd = buildForward();
    const auto rev = buildReverse();
    REQUIRE(fwd.size() == rev.size());
    for (size_t i = 0; i < fwd.size(); ++i)
    {
        CHECK(fwd[i].sourcePath == rev[i].sourcePath);
        CHECK(fwd[i].assetId == rev[i].assetId);
    }
    // And both are sorted by path.
    CHECK(fwd[0].sourcePath == "assets/a.glb");
    CHECK(fwd[2].sourcePath == "assets/c.glb");
}

TEST_CASE("Phase7 W2: AllRecordsSorted is always sorted by sourcePath")
{
    AssetDatabase db;
    std::vector<AssetDatabaseDiagnostic> diags;
    db.AddOrUpdate(MakeRecord("assets/zebra.glb", AssetKind::Model), diags);
    db.AddOrUpdate(MakeRecord("assets/apple.glb", AssetKind::Model), diags);
    db.AddOrUpdate(MakeRecord("assets/mango.glb", AssetKind::Model), diags);
    const auto recs = db.AllRecordsSorted();
    REQUIRE(recs.size() == 3);
    CHECK(recs[0].sourcePath == "assets/apple.glb");
    CHECK(recs[1].sourcePath == "assets/mango.glb");
    CHECK(recs[2].sourcePath == "assets/zebra.glb");
}

TEST_CASE("Phase7 W2: an empty database reports zero records and no diagnostics")
{
    AssetDatabase db;
    CHECK(db.Size() == 0);
    CHECK(db.AllRecordsSorted().empty());
    CHECK(db.FindByPath("anything") == nullptr);
    CHECK(db.FindById(UUID::Nil()) == nullptr);
}

TEST_CASE("Phase7 W2: BuildAssetDatabase surfaces duplicate IDs across records")
{
    const UUID id = UUID::Parse("11111111-2222-4333-8444-555555555555");
    std::vector<AssetDatabaseDiagnostic> diags;
    std::vector<AssetRecord> recs = {
        MakeRecord("assets/a.glb", AssetKind::Model, id),
        MakeRecord("assets/b.glb", AssetKind::Model, id),
    };
    AssetDatabase db = BuildAssetDatabase(std::move(recs), diags);
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].kind == AssetDatabaseDiagnostic::Kind::DuplicateId);
    CHECK(db.FindByPath("assets/a.glb")->assetId == id);
    CHECK(db.FindByPath("assets/b.glb")->assetId.IsNull());
}