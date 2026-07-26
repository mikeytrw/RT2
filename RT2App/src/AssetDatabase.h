#pragma once

#ifndef RT2_CORE_ASSET_DATABASE_H
#define RT2_CORE_ASSET_DATABASE_H

#include "ECSComponents.h"
#include "core/UUID.h"
#include "core/Error.h"

#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// AssetDatabase — the in-memory record store for source-asset identity.
//
// Per Phase 7 D8, per-asset sidecars are the durable source of truth. This
// CPU-only database is a pure index/cache built from caller-supplied sidecar
// and reference records. It performs no filesystem I/O and links into
// RT2Tests and RT2SliceRunner without Vulkan, ImGui, or Walnut.
//
// BuildAssetDatabase normalizes and sorts its input internally, including
// nested dependency records. Results therefore do not depend on directory
// enumeration, insertion order, or unordered_map iteration.
// ============================================================================

namespace rt2::core {

// Identity precedence for records merged at one source path. Sidecar identity
// is authoritative per D8; a reference is a cached claim carried by a scene
// or dependency edge.
enum class AssetIdentityAuthority : uint8_t
{
    Reference = 0,
    Sidecar   = 1,
};

// One outgoing dependency from an asset to another asset. `sourceKey`
// identifies the dependency slot inside the owning asset (for example
// "gltf:image=0"); assetId identifies the target and sourcePath is its
// portable fallback. `kind` is the target's observed use on this edge, not an
// exclusive classification of the physical file.
struct AssetDependencyRecord
{
    std::string          sourceKey;
    UUID                 assetId;
    std::string          sourcePath;
    AssetKind            kind = AssetKind::Unknown;

    bool operator==(const AssetDependencyRecord& other) const
    {
        return sourceKey == other.sourceKey
            && assetId == other.assetId
            && sourcePath == other.sourcePath
            && kind == other.kind;
    }
};

// One record per known source path. A record may come from a sidecar or from
// an unresolved scene/dependency reference with a nil ID.
struct AssetRecord
{
    UUID                 assetId;          // nil if not yet assigned
    std::string          sourcePath;       // portable, project-relative UTF-8
    ImportSettings       importSettings;
    AssetIdentityAuthority identityAuthority =
        AssetIdentityAuthority::Reference;

    // A physical file may be observed in more than one role. Kept sorted and
    // deduplicated by AddOrUpdate/BuildAssetDatabase.
    std::vector<AssetKind> observedKinds;

    // Entities that reference this asset, sorted by UUID.
    std::vector<UUID> dependentEntities;

    // Outgoing cross-asset edges, sorted by stable source key and then target
    // identity/path. Distinct claims for one sourceKey are preserved so later
    // resolution can diagnose rather than choose by insertion order.
    std::vector<AssetDependencyRecord> dependencies;
};

// A diagnostic produced while building or querying the database. The host
// later routes it through AssetDiagnostic; this type keeps the database
// independent of SceneAssetResolver.
struct AssetDatabaseDiagnostic
{
    enum class Kind
    {
        DuplicateId,
        DuplicatePath,
        MissingSidecarId,
        ConflictingId,
    };

    Kind                     kind = Kind::DuplicateId;
    UUID                     assetId;
    std::string              sourcePath;
    // Every claimant when kind == DuplicateId, sorted by sourcePath.
    std::vector<std::string> candidatePaths;
    std::string              detail;
};

struct AssetIdLookupResult
{
    enum class Status
    {
        Missing,
        Unique,
        Ambiguous,
    };

    Status                    status = Status::Missing;
    // Non-null only for Unique.
    const AssetRecord*        record = nullptr;
    // Populated for Unique and Ambiguous; always sorted by sourcePath.
    std::vector<std::string>  candidatePaths;
};

class AssetDatabase
{
public:
    // Add or merge a record. Nested sets are unioned. A nil stored ID adopts
    // a later non-nil ID; two non-nil IDs on the same path emit ConflictingId
    // and preserve the claim with greater identityAuthority (sidecar over
    // reference). Equal-authority conflicts preserve the stored claim and
    // remain loud. Different paths claiming one ID all retain that ID and
    // emit DuplicateId with sorted candidate paths.
    void AddOrUpdate(const AssetRecord& record,
                     std::vector<AssetDatabaseDiagnostic>& diagnostics);

    // Record that an entity depends on an asset path. Creates a nil-ID
    // placeholder when the path is not known.
    void AddEntityDependency(const std::string& sourcePath,
                             const UUID& dependentEntity);

    // Record an outgoing cross-asset dependency keyed by sourceKey. Exact
    // duplicates are idempotent; distinct claims for one sourceKey are
    // retained in deterministic order. A target fallback path also creates
    // or updates a target record and observes dependency.kind there.
    void AddAssetDependency(
        const std::string& ownerSourcePath,
        const AssetDependencyRecord& dependency,
        std::vector<AssetDatabaseDiagnostic>& diagnostics);

    const AssetRecord* FindByPath(const std::string& sourcePath) const;

    // Nil/unclaimed IDs are Missing, one claimant is Unique, and more than
    // one claimant is Ambiguous. Ambiguous lookup never selects a record.
    AssetIdLookupResult LookupById(const UUID& assetId) const;

    // Sorted snapshot of every outgoing claim at one stable source key.
    std::vector<AssetDependencyRecord> FindDependenciesBySourceKey(
        const std::string& ownerSourcePath,
        const std::string& sourceKey) const;

    std::vector<AssetRecord> AllRecordsSorted() const;

    size_t Size() const { return m_ByPath.size(); }

private:
    std::unordered_map<std::string, AssetRecord> m_ByPath;
    // Every claiming path is retained and kept sorted. Nil IDs are omitted.
    std::unordered_map<UUID, std::vector<std::string>> m_ById;
};

// Build from records in any order. The function normalizes nested sets and
// sorts by a total deterministic key before insertion, so records,
// diagnostics, ambiguity candidates, observed uses, and dependency edges are
// independent of caller enumeration order.
AssetDatabase BuildAssetDatabase(
    std::vector<AssetRecord> records,
    std::vector<AssetDatabaseDiagnostic>& diagnostics);

} // namespace rt2::core

#endif // RT2_CORE_ASSET_DATABASE_H
