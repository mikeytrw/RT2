#include <doctest/doctest.h>

#include "AssetDatabase.h"
#include "AssetIdentity.h"
#include "AssetReference.h"
#include "AssetResolver.h"
#include "core/Error.h"
#include "core/UUID.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace rt2::core;
namespace fs = std::filesystem;

// ============================================================================
// Phase 7 W3 step 2 — generic read-only asset locator.
//
// These tests exhaustively cover the eight ID/path disagreement cases from
// the approved W3 contract (docs/game-engine-development-plan.md, "Approved
// unified contract" and "Decisions resolved by review", W3-Q1..Q9). The
// locator has NO production consumers yet; step 3 cuts models over.
//
// Every fixture is generated below a unique temporary directory. The locator
// is read-only: tests assert that no sidecar is written or mutated, that the
// database is not mutated, and that resolution never falls back to process
// CWD (W3-Q8). Diagnostics are deterministic: the batch API sorts by
// (kind, refPath, entityUuid, sourceKey, severity, detail).
// ============================================================================

namespace {

class TempDirectory
{
public:
    TempDirectory()
    {
        static uint64_t sequence = 0;
        const auto ticks = std::chrono::high_resolution_clock::now()
                               .time_since_epoch()
                               .count();
        m_Path = fs::temp_directory_path() /
            ("rt2_w3_locator_" + std::to_string(ticks) + "_" +
             std::to_string(++sequence));
        std::error_code ec;
        fs::create_directories(m_Path, ec);
        REQUIRE_MESSAGE(!ec, "failed to create temporary fixture directory");
    }
    ~TempDirectory()
    {
        std::error_code ec;
        fs::remove_all(m_Path, ec);
    }
    const fs::path& Path() const { return m_Path; }
private:
    fs::path m_Path;
};

bool WriteText(const fs::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    return out.good();
}

UUID MakeId(uint8_t a, uint8_t b)
{
    std::array<uint8_t, 16> bytes{};
    bytes[0] = a;
    bytes[1] = b;
    // Mark as v4 so ToString/Parse round-trip is unambiguous.
    bytes[6] = 0x40;
    bytes[8] = 0x80;
    return UUID(bytes);
}

// Create a regular file at <root>/<rel> with trivial content.
fs::path CreateAssetFile(const fs::path& root, const std::string& rel,
                         const std::string& content = "asset")
{
    const fs::path p = root / rel;
    fs::create_directories(p.parent_path(), std::error_code{});
    REQUIRE(WriteText(p, content));
    return p;
}

// Write a sidecar with the given ID next to <assetPath>.
void WriteSidecar(const fs::path& assetPath, const UUID& id)
{
    Error err;
    REQUIRE(WriteSidecarId(AssetSidecarPath(assetPath), id, err));
    REQUIRE(err.IsOk());
}

AssetReference MakeRef(AssetKind kind, const std::string& path,
                      const UUID& id = UUID::Nil(),
                      const std::string& sourceKey = "")
{
    AssetReference r;
    r.kind = kind;
    r.path = path;
    r.assetId = id;
    r.sourceKey = sourceKey;
    return r;
}

AssetRecord MakeDbRecord(const std::string& path, const UUID& id,
                         AssetKind kind = AssetKind::Model)
{
    AssetRecord record;
    record.sourcePath = path;
    record.assetId = id;
    record.identityAuthority = id.IsNull()
        ? AssetIdentityAuthority::Reference
        : AssetIdentityAuthority::Sidecar;
    record.observedKinds.push_back(kind);
    return record;
}

AssetResolutionContext MakeCtx(const fs::path& root,
                               const AssetDatabase* db)
{
    AssetResolutionContext ctx;
    ctx.assetRoot = root;
    ctx.database = db;
    return ctx;
}

size_t CountSeverity(const std::vector<AssetDiagnostic>& diags,
                     AssetDiagnostic::Severity sev)
{
    size_t n = 0;
    for (const auto& d : diags) if (d.severity == sev) ++n;
    return n;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Case 1 + 2: non-nil ID, unique DB record whose file exists -> success by ID.
// Stale reference path is observable but does not defeat ID resolution.
// ---------------------------------------------------------------------------

TEST_CASE("W3-Locator: unique ID with existing file wins (case 2)")
{
    TempDirectory tmp;
    const fs::path root = tmp.Path();
    const std::string dbRel = "assets/real.glb";
    const std::string refRel = "assets/old-name.glb"; // stale path
    CreateAssetFile(root, dbRel);
    // Reference path does NOT exist on disk.
    const UUID id = MakeId(0x11, 0x22);

    std::vector<AssetDatabaseDiagnostic> dbDiags;
    AssetDatabase db = BuildAssetDatabase({ MakeDbRecord(dbRel, id) }, dbDiags);
    REQUIRE(dbDiags.empty());

    auto ctx = MakeCtx(root, &db);
    std::vector<AssetDiagnostic> diags;
    auto r = Resolve(MakeRef(AssetKind::Model, refRel, id), ctx,
                     UUID::Nil(), "", diags);

    REQUIRE(r.success);
    REQUIRE(r.source == AssetResolutionSource::Id);
    REQUIRE(r.effectiveId == id);
    REQUIRE_FALSE(r.identityRepairRequired);
    REQUIRE(r.resolvedPath == (root / dbRel).lexically_normal());
    // Stale reference path produced an observable Missing diagnostic.
    REQUIRE(CountSeverity(diags, AssetDiagnostic::Missing) == 1);
    REQUIRE(diags[0].refPath == refRel);
    REQUIRE(diags[0].detail.find("stale") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Case 3: database stale/missing but path exists and sidecar matches ID ->
// fallback succeeds and reports stale database state.
// ---------------------------------------------------------------------------

TEST_CASE("W3-Locator: stale DB but matching sidecar (case 3)")
{
    TempDirectory tmp;
    const fs::path root = tmp.Path();
    const std::string rel = "assets/x.glb";
    const fs::path abs = CreateAssetFile(root, rel);
    const UUID id = MakeId(0x33, 0x44);
    WriteSidecar(abs, id);
    // Database is EMPTY (stale).
    std::vector<AssetDatabaseDiagnostic> dbDiags;
    AssetDatabase db = BuildAssetDatabase({}, dbDiags);
    auto ctx = MakeCtx(root, &db);
    std::vector<AssetDiagnostic> diags;
    auto r = Resolve(MakeRef(AssetKind::Model, rel, id), ctx,
                     UUID::Nil(), "", diags);
    REQUIRE(r.success);
    REQUIRE(r.source == AssetResolutionSource::PathFallback);
    REQUIRE(r.effectiveId == id);
    REQUIRE_FALSE(r.identityRepairRequired);
    REQUIRE(CountSeverity(diags, AssetDiagnostic::Missing) == 1);
    REQUIRE(diags[0].detail.find("database stale") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Case 4: ID not in DB, path exists, sidecar absent -> fallback succeeds with
// repair signal.
// ---------------------------------------------------------------------------

TEST_CASE("W3-Locator: ID missing, path exists, no sidecar (case 4)")
{
    TempDirectory tmp;
    const fs::path root = tmp.Path();
    const std::string rel = "assets/y.glb";
    CreateAssetFile(root, rel);
    const UUID requestedId = MakeId(0x55, 0x66);
    std::vector<AssetDatabaseDiagnostic> dbDiags;
    AssetDatabase db = BuildAssetDatabase({}, dbDiags);
    auto ctx = MakeCtx(root, &db);
    std::vector<AssetDiagnostic> diags;
    auto r = Resolve(MakeRef(AssetKind::Model, rel, requestedId), ctx,
                     UUID::Nil(), "", diags);
    REQUIRE(r.success);
    REQUIRE(r.source == AssetResolutionSource::PathFallback);
    REQUIRE(r.effectiveId.IsNull());
    REQUIRE(r.identityRepairRequired);
    REQUIRE(CountSeverity(diags, AssetDiagnostic::Missing) == 1);
    REQUIRE(diags[0].detail.find("identity repair required") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Case 5: sidecar claims a different ID -> Conflict, never substitute.
// ---------------------------------------------------------------------------

TEST_CASE("W3-Locator: sidecar disagrees -> Conflict (case 5)")
{
    TempDirectory tmp;
    const fs::path root = tmp.Path();
    const std::string rel = "assets/z.glb";
    const fs::path abs = CreateAssetFile(root, rel);
    const UUID requestedId = MakeId(0x77, 0x88);
    const UUID sidecarId = MakeId(0x99, 0xAA);
    WriteSidecar(abs, sidecarId);
    std::vector<AssetDatabaseDiagnostic> dbDiags;
    AssetDatabase db = BuildAssetDatabase({}, dbDiags);
    auto ctx = MakeCtx(root, &db);
    std::vector<AssetDiagnostic> diags;
    auto r = Resolve(MakeRef(AssetKind::Model, rel, requestedId), ctx,
                     UUID::Nil(), "", diags);
    REQUIRE_FALSE(r.success);
    REQUIRE(r.resolvedPath.empty());
    REQUIRE(CountSeverity(diags, AssetDiagnostic::Conflict) == 1);
    REQUIRE(diags[0].detail.find("disagrees") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Case 6: neither ID nor path locates a regular file -> Missing.
// ---------------------------------------------------------------------------

TEST_CASE("W3-Locator: nothing resolves -> Missing (case 6)")
{
    TempDirectory tmp;
    const fs::path root = tmp.Path();
    const UUID id = MakeId(0xBB, 0xCC);
    std::vector<AssetDatabaseDiagnostic> dbDiags;
    AssetDatabase db = BuildAssetDatabase({}, dbDiags);
    auto ctx = MakeCtx(root, &db);
    std::vector<AssetDiagnostic> diags;
    auto r = Resolve(MakeRef(AssetKind::Model, "nope.glb", id), ctx,
                     UUID::Nil(), "", diags);
    REQUIRE_FALSE(r.success);
    REQUIRE(r.resolvedPath.empty());
    REQUIRE(CountSeverity(diags, AssetDiagnostic::Missing) == 1);
}

// ---------------------------------------------------------------------------
// Case 7: ambiguous ID -> Conflict even if fallback path matches one claimant.
// ---------------------------------------------------------------------------

TEST_CASE("W3-Locator: ambiguous ID -> Conflict (case 7)")
{
    TempDirectory tmp;
    const fs::path root = tmp.Path();
    const std::string a = "a.glb";
    const std::string b = "b.glb";
    CreateAssetFile(root, a);
    CreateAssetFile(root, b);
    const UUID id = MakeId(0xDD, 0xEE);
    std::vector<AssetDatabaseDiagnostic> dbDiags;
    // Two records claim the same ID.
    AssetDatabase db = BuildAssetDatabase(
        { MakeDbRecord(a, id), MakeDbRecord(b, id) }, dbDiags);
    auto ctx = MakeCtx(root, &db);
    std::vector<AssetDiagnostic> diags;
    auto r = Resolve(MakeRef(AssetKind::Model, a, id), ctx,
                     UUID::Nil(), "", diags);
    REQUIRE_FALSE(r.success);
    REQUIRE(r.resolvedPath.empty());
    REQUIRE(CountSeverity(diags, AssetDiagnostic::Conflict) == 1);
    REQUIRE(diags[0].detail.find("multiple") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Case 8: nil ID -> path fallback. Missing sidecar -> repair required.
// Sidecar present -> effective ID from sidecar, no repair.
// ---------------------------------------------------------------------------

TEST_CASE("W3-Locator: nil ID path fallback with absent sidecar (case 8a)")
{
    TempDirectory tmp;
    const fs::path root = tmp.Path();
    const std::string rel = "assets/n.glb";
    CreateAssetFile(root, rel);
    std::vector<AssetDatabaseDiagnostic> dbDiags;
    AssetDatabase db = BuildAssetDatabase({}, dbDiags);
    auto ctx = MakeCtx(root, &db);
    std::vector<AssetDiagnostic> diags;
    auto r = Resolve(MakeRef(AssetKind::Model, rel, UUID::Nil()), ctx,
                     UUID::Nil(), "", diags);
    REQUIRE(r.success);
    REQUIRE(r.source == AssetResolutionSource::PathFallback);
    REQUIRE(r.effectiveId.IsNull());
    REQUIRE(r.identityRepairRequired);
    REQUIRE(CountSeverity(diags, AssetDiagnostic::Missing) == 1);
}

TEST_CASE("W3-Locator: nil ID path fallback with sidecar (case 8b)")
{
    TempDirectory tmp;
    const fs::path root = tmp.Path();
    const std::string rel = "assets/m.glb";
    const fs::path abs = CreateAssetFile(root, rel);
    const UUID sidecarId = MakeId(0x01, 0x02);
    WriteSidecar(abs, sidecarId);
    std::vector<AssetDatabaseDiagnostic> dbDiags;
    AssetDatabase db = BuildAssetDatabase({}, dbDiags);
    auto ctx = MakeCtx(root, &db);
    std::vector<AssetDiagnostic> diags;
    auto r = Resolve(MakeRef(AssetKind::Model, rel, UUID::Nil()), ctx,
                     UUID::Nil(), "", diags);
    REQUIRE(r.success);
    REQUIRE(r.source == AssetResolutionSource::PathFallback);
    REQUIRE(r.effectiveId == sidecarId);
    REQUIRE_FALSE(r.identityRepairRequired);
    REQUIRE(diags.empty());
}

// ---------------------------------------------------------------------------
// Case 1: unique ID matches reference path and file exists -> success by ID,
// no diagnostics.
// ---------------------------------------------------------------------------

TEST_CASE("W3-Locator: unique ID and path agree (case 1)")
{
    TempDirectory tmp;
    const fs::path root = tmp.Path();
    const std::string rel = "assets/agree.glb";
    CreateAssetFile(root, rel);
    const UUID id = MakeId(0x03, 0x04);
    std::vector<AssetDatabaseDiagnostic> dbDiags;
    AssetDatabase db = BuildAssetDatabase({ MakeDbRecord(rel, id) }, dbDiags);
    auto ctx = MakeCtx(root, &db);
    std::vector<AssetDiagnostic> diags;
    auto r = Resolve(MakeRef(AssetKind::Model, rel, id), ctx,
                     UUID::Nil(), "", diags);
    REQUIRE(r.success);
    REQUIRE(r.source == AssetResolutionSource::Id);
    REQUIRE(r.effectiveId == id);
    REQUIRE_FALSE(r.identityRepairRequired);
    REQUIRE(diags.empty());
}

// ---------------------------------------------------------------------------
// Read-only: resolution must not write or mutate any sidecar, and must not
// mutate the database. (W3-Q9.)
// ---------------------------------------------------------------------------

TEST_CASE("W3-Locator: read-only — no sidecar written for nil-ID missing sidecar")
{
    TempDirectory tmp;
    const fs::path root = tmp.Path();
    const std::string rel = "assets/ro.glb";
    const fs::path abs = CreateAssetFile(root, rel);
    const fs::path sidecar = AssetSidecarPath(abs);
    std::vector<AssetDatabaseDiagnostic> dbDiags;
    AssetDatabase db = BuildAssetDatabase({}, dbDiags);
    auto ctx = MakeCtx(root, &db);
    std::vector<AssetDiagnostic> diags;
    auto r = Resolve(MakeRef(AssetKind::Model, rel, UUID::Nil()), ctx,
                     UUID::Nil(), "", diags);
    REQUIRE(r.success);
    // No sidecar should exist after a read-only resolution.
    std::error_code ec;
    REQUIRE_FALSE(fs::exists(sidecar, ec));
}

TEST_CASE("W3-Locator: read-only — existing sidecar untouched")
{
    TempDirectory tmp;
    const fs::path root = tmp.Path();
    const std::string rel = "assets/ro2.glb";
    const fs::path abs = CreateAssetFile(root, rel);
    const UUID original = MakeId(0x10, 0x20);
    WriteSidecar(abs, original);
    const fs::path sidecar = AssetSidecarPath(abs);
    const auto before = fs::file_size(sidecar);
    std::vector<AssetDatabaseDiagnostic> dbDiags;
    AssetDatabase db = BuildAssetDatabase({}, dbDiags);
    auto ctx = MakeCtx(root, &db);
    std::vector<AssetDiagnostic> diags;
    // Request a DIFFERENT id to provoke the conflict path; even then, no
    // overwrite is permitted.
    auto r = Resolve(MakeRef(AssetKind::Model, rel, MakeId(0x30, 0x40)), ctx,
                     UUID::Nil(), "", diags);
    REQUIRE_FALSE(r.success);
    const auto after = fs::file_size(sidecar);
    REQUIRE(before == after);
    Error err;
    REQUIRE(ReadSidecarId(sidecar, err) == original);
}

// ---------------------------------------------------------------------------
// No CWD fallback (W3-Q8). A relative path that does not exist under the
// asset root must fail even if the file exists relative to process CWD.
// ---------------------------------------------------------------------------

TEST_CASE("W3-Locator: no process-CWD fallback (W3-Q8)")
{
    TempDirectory tmp;
    const fs::path root = tmp.Path();
    // Create a file at the process CWD (the test runner's CWD, which is the
    // repo root). We use a uniquely-named file to avoid collisions.
    const std::string cwdFile = "rt2_w3_locator_cwd_only_"
        + std::to_string(std::chrono::high_resolution_clock::now()
                             .time_since_epoch().count());
    REQUIRE(WriteText(cwdFile, "cwd-only"));
    struct CwdFileGuard {
        std::string name;
        CwdFileGuard(const std::string& n) : name(n) {}
        ~CwdFileGuard() { std::error_code ec; fs::remove(name, ec); }
    } guard(cwdFile);

    std::vector<AssetDatabaseDiagnostic> dbDiags;
    AssetDatabase db = BuildAssetDatabase({}, dbDiags);
    auto ctx = MakeCtx(root, &db);
    std::vector<AssetDiagnostic> diags;
    auto r = Resolve(MakeRef(AssetKind::Model, cwdFile, UUID::Nil()), ctx,
                     UUID::Nil(), "", diags);
    REQUIRE_FALSE(r.success);
    REQUIRE(r.resolvedPath.empty());
    REQUIRE(CountSeverity(diags, AssetDiagnostic::Missing) == 1);
}

// ---------------------------------------------------------------------------
// Absolute legacy paths are accepted in memory but the successful result is
// normalized (W3-Q8).
// ---------------------------------------------------------------------------

TEST_CASE("W3-Locator: absolute legacy path accepted and normalized")
{
    TempDirectory tmp;
    const fs::path root = tmp.Path();
    const std::string rel = "assets/abs.glb";
    const fs::path abs = CreateAssetFile(root, rel);
    std::vector<AssetDatabaseDiagnostic> dbDiags;
    AssetDatabase db = BuildAssetDatabase({}, dbDiags);
    auto ctx = MakeCtx(root, &db);
    std::vector<AssetDiagnostic> diags;
    auto r = Resolve(MakeRef(AssetKind::Model, abs.string(), UUID::Nil()),
                     ctx, UUID::Nil(), "", diags);
    REQUIRE(r.success);
    REQUIRE(r.resolvedPath.lexically_normal() == abs.lexically_normal());
}

// ---------------------------------------------------------------------------
// Deterministic batch diagnostics: sort by
// (kind, refPath, entityUuid, sourceKey, severity, detail). Stable for
// equal keys. Independent of input order.
// ---------------------------------------------------------------------------

TEST_CASE("W3-Locator: batch diagnostics sorted deterministically")
{
    TempDirectory tmp;
    const fs::path root = tmp.Path();
    // Two missing refs with different paths, and one conflict, plus one
    // success (no diagnostic). Construct entries so the natural input order is
    // NOT the sorted order.
    const UUID idA = MakeId(0xA0, 0x01);
    const UUID idB = MakeId(0xA0, 0x02); // different from sidecar
    CreateAssetFile(root, "z.glb");
    CreateAssetFile(root, "a.glb");
    const fs::path conflictAbs = CreateAssetFile(root, "m.glb");
    WriteSidecar(conflictAbs, idB);

    std::vector<AssetDatabaseDiagnostic> dbDiags;
    AssetDatabase db = BuildAssetDatabase({}, dbDiags);
    auto ctx = MakeCtx(root, &db);

    std::vector<AssetBatchEntry> entries;
    // z.glb missing (nil id)
    entries.push_back({ MakeRef(AssetKind::Model, "z.glb", UUID::Nil()),
                        UUID::Nil(), "eZ" });
    // a.glb missing (nil id)
    entries.push_back({ MakeRef(AssetKind::Model, "a.glb", UUID::Nil()),
                        UUID::Nil(), "eA" });
    // m.glb conflict (requested idA, sidecar idB)
    entries.push_back({ MakeRef(AssetKind::Model, "m.glb", idA),
                        UUID::Nil(), "eM" });

    std::vector<AssetDiagnostic> diags;
    const bool ok = ResolveBatch(entries, ctx, diags);
    REQUIRE_FALSE(ok);
    REQUIRE(diags.size() == 3);
    // Sorted by (kind, refPath, ...). All same kind; refPath ascending:
    // a.glb, m.glb, z.glb.
    REQUIRE(diags[0].refPath == "a.glb");
    REQUIRE(diags[1].refPath == "m.glb");
    REQUIRE(diags[1].severity == AssetDiagnostic::Conflict);
    REQUIRE(diags[2].refPath == "z.glb");
}

TEST_CASE("W3-Locator: batch order-independent")
{
    TempDirectory tmp;
    const fs::path root = tmp.Path();
    CreateAssetFile(root, "a.glb");
    CreateAssetFile(root, "b.glb");
    std::vector<AssetDatabaseDiagnostic> dbDiags;
    AssetDatabase db = BuildAssetDatabase({}, dbDiags);
    auto ctx = MakeCtx(root, &db);

    auto run = [&](const std::vector<std::string>& order) {
        std::vector<AssetBatchEntry> entries;
        for (const auto& p : order)
            entries.push_back({ MakeRef(AssetKind::Model, p, UUID::Nil()),
                                UUID::Nil(), "" });
        std::vector<AssetDiagnostic> diags;
        ResolveBatch(entries, ctx, diags);
        std::string key;
        for (const auto& d : diags) key += d.refPath + "|";
        return key;
    };

    auto k1 = run({ "a.glb", "b.glb" });
    auto k2 = run({ "b.glb", "a.glb" });
    REQUIRE(k1 == k2);
}

// ---------------------------------------------------------------------------
// entityUuid / entityName context is preserved in diagnostics.
// ---------------------------------------------------------------------------

TEST_CASE("W3-Locator: entity context preserved in diagnostic")
{
    TempDirectory tmp;
    const fs::path root = tmp.Path();
    const UUID entity = MakeId(0x70, 0x71);
    std::vector<AssetDatabaseDiagnostic> dbDiags;
    AssetDatabase db = BuildAssetDatabase({}, dbDiags);
    auto ctx = MakeCtx(root, &db);
    std::vector<AssetDiagnostic> diags;
    Resolve(MakeRef(AssetKind::Model, "missing.glb", UUID::Nil()),
            ctx, entity, "EntityName", diags);
    REQUIRE(diags.size() == 1);
    REQUIRE(diags[0].entityUuid == entity);
    REQUIRE(diags[0].entityName == "EntityName");
}

// ---------------------------------------------------------------------------
// empty path: terminal failure with a diagnostic, never a silent empty
// result.
// ---------------------------------------------------------------------------

TEST_CASE("W3-Locator: empty path fails with diagnostic")
{
    TempDirectory tmp;
    const fs::path root = tmp.Path();
    std::vector<AssetDatabaseDiagnostic> dbDiags;
    AssetDatabase db = BuildAssetDatabase({}, dbDiags);
    auto ctx = MakeCtx(root, &db);
    std::vector<AssetDiagnostic> diags;
    auto r = Resolve(MakeRef(AssetKind::Model, "", UUID::Nil()), ctx,
                     UUID::Nil(), "", diags);
    REQUIRE_FALSE(r.success);
    REQUIRE(r.resolvedPath.empty());
    REQUIRE(diags.size() == 1);
    REQUIRE(diags[0].severity == AssetDiagnostic::Missing);
}

// ---------------------------------------------------------------------------
// AssetDiagnostic::Conflict is a distinct severity and the exhaustive
// formatter surface (W3-Q5 / W3-P14) is exercised by the Walnut switch (built
// by the RT2App target). Here we only assert the enum value exists and is
// distinct from the other three.
// ---------------------------------------------------------------------------

TEST_CASE("W3-Locator: Conflict severity is distinct")
{
    CHECK(AssetDiagnostic::Missing    != AssetDiagnostic::Conflict);
    CHECK(AssetDiagnostic::Malformed  != AssetDiagnostic::Conflict);
    CHECK(AssetDiagnostic::Unresolved != AssetDiagnostic::Conflict);
}