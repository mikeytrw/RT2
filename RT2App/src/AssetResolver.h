#pragma once

#ifndef RT2_CORE_ASSET_RESOLVER_H
#define RT2_CORE_ASSET_RESOLVER_H

#include "AssetDatabase.h"
#include "AssetReference.h"
#include "AssetIdentity.h"
#include "core/Error.h"
#include "core/UUID.h"

#include <filesystem>
#include <string>
#include <vector>

// ============================================================================
// AssetResolver — the neutral, CPU-only, read-only asset locator.
//
// Phase 7 W3 step 2 lands this header and its implementation with no
// production consumers. Step 3 onward cuts each asset kind over to it.
//
// Contract (approved 2026-07-25, W3-Q1..Q9):
//
//   - The locator answers only "which file?". It does not load, decode, or
//     stage geometry/textures. Kind-specific CPU loaders keep their policies
//     but emit every failure through AssetDiagnostic.
//   - The locator is read-only. It calls AssetIdentity::ReadSidecarId when
//     identity verification is needed; it NEVER mints, writes a sidecar,
//     rewrites a scene, or mutates the database. Import/save/migration own
//     identity repair.
//   - The caller supplies an explicit resolution context (asset root plus a
//     non-owning AssetDatabase). No global resolver/database and no database
//     embedded in SceneDocument (W3-Q3).
//   - Process-CWD fallback is removed (W3-Q8). Legacy absolute paths are
//     accepted in memory, but the successful result is normalized and the
//     locator never persists a new absolute path.
//
// Resolution order and disagreement policy (the eight cases are encoded in
// Resolve() and exhaustively tested):
//
//   1. A non-nil ID is authoritative and looked up first.
//   2. A unique ID whose file exists wins. A stale/missing reference path is
//      observable but does not defeat successful ID resolution.
//   3. If the database is stale/missing but the path exists and the path's
//      sidecar claims the same ID, path fallback succeeds and reports stale
//      database state.
//   4. If the ID does not locate a file, the path exists, and the sidecar is
//      absent, fallback succeeds with a sidecar Missing diagnostic and
//      identityRepairRequired=true; explicit save/migration performs remap.
//   5. If the path's sidecar claims a different ID, resolution fails with
//      Conflict; it never silently substitutes one identity for the other.
//   6. If neither ID nor path locates a regular file, resolution fails Missing.
//   7. If more than one asset claims the ID, lookup is Ambiguous and fails
//      Conflict, even when one candidate matches the fallback path. No
//      insertion-order winner is chosen.
//   8. A nil ID uses path fallback. Missing/malformed sidecar state is
//      observable; later schema/migration work persists the assigned identity.
//
// Diagnostic ordering: batch APIs collect diagnostics locally and sort before
// appending by (kind, refPath, entityUuid, sourceKey, severity, detail).
// Results never depend on EnTT traversal, directory enumeration, or
// unordered_map order. Terminal failure never returns an empty path without
// a diagnostic.
//
// AssetDiagnostic is defined here (neutral) and re-exported from
// SceneAssetResolver.h, which now includes this header. Severity gains
// Conflict (W3-Q5); the Walnut formatter must be made exhaustive in the same
// change that adds a Conflict-emitting code path.
// ============================================================================

namespace rt2::core {

// One diagnostic produced while resolving an asset. The structured locator
// fills every applicable field; consumers must not assume a single non-empty
// field identifies the failure. `detail` is the human-readable context.
struct AssetDiagnostic
{
    enum Severity
    {
        Missing,     // file not found / unreadable
        Malformed,   // file found but failed to parse
        Unresolved,   // source key not present in the rebuilt asset
        Conflict,    // ID/path disagreement; identity not substituted (W3-Q5)
    };
    Severity        severity = Missing;
    AssetKind       kind     = AssetKind::Unknown;
    std::string     refPath;        // the AssetReference::path that failed
    std::string     resolvedPath;   // absolute path the resolver tried
    UUID            entityUuid;     // referring entity (nil if env)
    std::string     entityName;
    std::string     sourceKey;      // subresource identity, if applicable
    std::string     detail;         // human-readable context
};

// The explicit resolution context owned by the current scene/recovery host.
// W4 replaces its root with the project asset root (W3-Q3).
struct AssetResolutionContext
{
    // Directory used to resolve relative AssetReference::path values. Must be
    // absolute. The locator does NOT fall back to process CWD (W3-Q8).
    std::filesystem::path assetRoot;

    // Non-owning. May be nullptr when no database is available (nil-ID path
    // fallback still works; ID-first lookup is skipped). The locator never
    // mutates this pointer.
    const AssetDatabase* database = nullptr;
};

// Where the successful path came from. Id = ID-first lookup located a file;
// PathFallback = ID did not locate a file (or was nil) and the path resolved.
enum class AssetResolutionSource : uint8_t
{
    Id           = 0,
    PathFallback = 1,
};

// Structured result returned by Resolve(). A successful resolution has
// `success=true`, a non-empty `resolvedPath`, and (when the ID was non-nil
// and matched) `effectiveId` equal to the authoritative ID. A failed
// resolution has `success=false`, an empty `resolvedPath`, and at least one
// diagnostic in the caller's diagnostic sink.
struct AssetResolutionResult
{
    bool                   success = false;
    std::filesystem::path  resolvedPath;        // empty on failure
    AssetResolutionSource  source = AssetResolutionSource::PathFallback;
    // The authoritative ID the locator converged on. On Id resolution this is
    // the database record's ID; on PathFallback with a matching sidecar it is
    // the sidecar's ID; otherwise nil.
    UUID                   effectiveId;
    // True when the locator resolved by path but the durable identity needs
    // explicit repair (missing or stale sidecar/database). Import/save/
    // migration owns the actual write; this flag is the signal.
    bool                   identityRepairRequired = false;
};

// Resolve a single AssetReference against an explicit context. Pure: no
// filesystem mutation, no sidecar write, no database mutation. Diagnostics
// (zero or one terminal entry on failure; zero on success) are appended to
// `diagnostics` and are not sorted by this entry point — batch APIs sort.
//
// `entityUuid`/`entityName` are optional context used only to fill the
// diagnostic; pass nil/empty for non-entity references (e.g. environment).
AssetResolutionResult Resolve(const AssetReference& ref,
                              const AssetResolutionContext& ctx,
                              const UUID& entityUuid,
                              const std::string& entityName,
                              std::vector<AssetDiagnostic>& diagnostics);

// Resolve a batch of references and sort the appended diagnostics
// deterministically by (kind, refPath, entityUuid, sourceKey, severity,
// detail). Each entry is resolved independently; the batch does not stage
// or commit any resource. Returns true iff every entry resolved.
struct AssetBatchEntry
{
    AssetReference  ref;
    UUID            entityUuid;     // nil if non-entity
    std::string     entityName;
};
bool ResolveBatch(const std::vector<AssetBatchEntry>& entries,
                  const AssetResolutionContext& ctx,
                  std::vector<AssetDiagnostic>& diagnostics);

// Deterministic sort key for diagnostics. Exposed so callers and tests share
// one ordering rule.
std::string AssetDiagnosticSortKey(const AssetDiagnostic& d);

} // namespace rt2::core

#endif // RT2_CORE_ASSET_RESOLVER_H