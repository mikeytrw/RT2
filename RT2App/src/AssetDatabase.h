#pragma once

#ifndef RT2_CORE_ASSET_DATABASE_H
#define RT2_CORE_ASSET_DATABASE_H

#include "ECSComponents.h"
#include "core/UUID.h"
#include "core/Error.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// AssetDatabase — the in-memory record store for source-asset identity
// (Phase 7 W2).
//
// This is the database the Phase 7 roadmap calls for: stable asset IDs,
// source paths, asset kinds, import settings, and dependency records. Per
// D8, the *source of truth* for identity is per-asset sidecar files
// (.rt2meta) committed alongside the asset; this database is an in-memory
// index/cache built from those sidecars plus the scene's AssetReferences.
//
// CPU-only by design (no Vulkan, ImGui, Walnut, filesystem-write access):
// it links cleanly into RT2Tests and RT2SliceRunner, following the
// ScriptFieldReconcile / ScriptScenarioCompare precedent of keeping logic
// pure and putting host wiring elsewhere. Scanning the asset tree for
// sidecars is done by the caller (who owns the filesystem); the database
// itself only indexes records handed to it.
//
// Determinism: every builder path sorts by source path before recording so
// the resulting database never depends on directory enumeration order. This
// is the same precedent as ReconcileScriptFields (ScriptFieldReconcile.cpp:
// sort before emit). Two scans of the same tree produce byte-identical
// databases.
// ============================================================================

namespace rt2::core {

// One record per known asset. A record exists when a sidecar claims an ID
// for a path, or when a scene's AssetReference references a path (even with
// a nil ID — the database records the reference so dependency tracking and
// missing-asset diagnostics work before the ID is assigned).
struct AssetRecord
{
    UUID                 assetId;          // nil if not yet assigned
    AssetKind            kind = AssetKind::Unknown;
    std::string          sourcePath;       // portable, project-relative UTF-8
    ImportSettings       importSettings;

    // Dependency graph (recorded from scene references in W2). An entry in
    // `dependents` means some scene entity references this asset. Cross-
    // asset dependencies (a model depends on its textures) land in W3 when
    // resolution by ID unifies the import paths.
    std::vector<UUID>    dependents;       // entities that reference this asset
};

// A diagnostic produced while building or querying the database. Routed
// through the existing AssetDiagnostic channel by the host; kept separate
// here so the database is CPU-only and testable without the resolver.
struct AssetDatabaseDiagnostic
{
    enum class Kind
    {
        DuplicateId,         // two sidecars claim the same ID for different paths
        DuplicatePath,       // two records map to the same path
        MissingSidecarId,    // a sidecar exists but contains no valid ID
        ConflictingId,       // a scene reference's assetId disagrees with the sidecar
    };
    Kind        kind = Kind::DuplicateId;
    UUID        assetId;        // the ID in dispute (nil for MissingSidecarId)
    std::string sourcePath;     // the path involved
    std::string detail;
};

class AssetDatabase
{
public:
    // Add or merge a record. If a record for sourcePath already exists, the
    // kind/importSettings are updated and the existing assetId is preserved
    // unless the new one is non-nil and conflicts (a ConflictingId diagnostic
    // is emitted and the sidecar's ID wins). If two different paths claim the
    // same non-nil ID, a DuplicateId diagnostic is emitted and the first path
    // keeps the ID; the second is left with a nil ID in its record.
    //
    // Returns false only on a hard internal error (none today); diagnostics
    // are always appended to `diagnostics` regardless.
    void AddOrUpdate(const AssetRecord& record,
                     std::vector<AssetDatabaseDiagnostic>& diagnostics);

    // Record that an entity (by UUID) depends on an asset at sourcePath.
    // Creates a placeholder record if the path is not yet known (nil ID),
    // so dependency tracking works before sidecars are scanned.
    void AddDependency(const std::string& sourcePath, const UUID& dependentEntity);

    // Look up by path. Returns nullptr if not present.
    const AssetRecord* FindByPath(const std::string& sourcePath) const;

    // Look up by asset ID. Returns nullptr if not present (or if the ID is
    // nil — nil IDs are not in the ID index by design).
    const AssetRecord* FindById(const UUID& assetId) const;

    // All records, sorted by sourcePath for deterministic output. The
    // returned vector is a snapshot; subsequent mutations do not affect it.
    std::vector<AssetRecord> AllRecordsSorted() const;

    size_t Size() const { return m_ByPath.size(); }

private:
    // sourcePath -> record (owns the records).
    std::unordered_map<std::string, AssetRecord> m_ByPath;
    // assetId -> sourcePath (index into m_ByPath; nil IDs are not indexed).
    std::unordered_map<UUID, std::string> m_ById;
};

// Build a database from a sorted, deduplicated list of records. The caller
// is responsible for scanning sidecars in deterministic order (sorted by
// path); this helper just inserts them. Diagnostics surface duplicate IDs
// and paths. This is the pure core that tests exercise; the host owns the
// filesystem scan.
//
// `records` is consumed by move. Sort it by sourcePath before calling for
// deterministic results; this function does NOT re-sort (the caller has the
// context to do so and may have a stronger ordering requirement).
AssetDatabase BuildAssetDatabase(std::vector<AssetRecord> records,
                                  std::vector<AssetDatabaseDiagnostic>& diagnostics);

} // namespace rt2::core

#endif // RT2_CORE_ASSET_DATABASE_H