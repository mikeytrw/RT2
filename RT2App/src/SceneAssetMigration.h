#pragma once

#ifndef RT2_CORE_SCENE_ASSET_MIGRATION_H
#define RT2_CORE_SCENE_ASSET_MIGRATION_H

#include "AssetResolver.h"
#include "SceneDocument.h"
#include "core/Error.h"

#include <filesystem>
#include <memory>
#include <vector>

namespace rt2::core {

struct SceneAssetMigrationOptions
{
    std::filesystem::path assetRoot;
    UUID projectId;
    IUuidProvider* uuidProvider = nullptr;
    const AssetDatabase* existingDatabase = nullptr;
};

struct SceneAssetMigrationReport
{
    bool requiresPersistence = false;
    bool incomplete = false;
    uint32_t sourceVersion = 0;
    size_t referenceCount = 0;
    size_t assignedReferenceCount = 0;
    size_t unresolvedReferenceCount = 0;
    size_t createdSidecarCount = 0;
    std::vector<std::filesystem::path> createdSidecars;
    std::vector<AssetDiagnostic> diagnostics;
    std::shared_ptr<const AssetDatabase> database;
};

// A v3 load or an incomplete v4 identity repair keeps explicit migration in
// front of acknowledgement. Recovery is still allowed to snapshot the
// unmigrated document through SaveTo; only a successful migration plus atomic
// scene save clears the gate.
class AssetMigrationPersistenceGate
{
public:
    void Adopt(bool pending) { m_Pending = pending; }
    void OnPersistedOrReset() { m_Pending = false; }
    bool Pending() const { return m_Pending; }
    // Pending migration is not destructive-load loss. Recovery must continue
    // to preserve the legacy document while explicit Save performs migration.
    bool SuppressAutosave() const { return false; }

private:
    bool m_Pending = false;
};

// CPU-only host policy seam. Migration pending is safe to recover; destructive
// script-repair loss remains the condition that suppresses autosave.
inline bool ShouldCaptureRecoverySnapshot(
    bool scriptRepairPending,
    const AssetMigrationPersistenceGate& migrationGate)
{
    return !scriptRepairPending && !migrationGate.SuppressAutosave();
}

// Stage a v3/v4 document into a canonical v4 document. Sidecars are assigned
// once per physical source asset in sorted order. The live document is never
// mutated; sidecars successfully written before a later failure are retained
// so retry can reuse them.
bool MigrateSceneAssetReferences(
    const SceneDocument& source,
    SceneDocument& staged,
    const SceneAssetMigrationOptions& options,
    SceneAssetMigrationReport& report,
    Error& err);

} // namespace rt2::core

#endif // RT2_CORE_SCENE_ASSET_MIGRATION_H
