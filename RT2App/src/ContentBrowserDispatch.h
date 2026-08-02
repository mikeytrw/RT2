#pragma once

#ifndef RT2_CORE_CONTENT_BROWSER_DISPATCH_H
#define RT2_CORE_CONTENT_BROWSER_DISPATCH_H

#include "AssetDatabase.h"
#include "AssetWatchPolicy.h"
#include "ContentBrowserOperations.h"
#include "SceneAssetMigration.h"
#include "core/Error.h"

#include <cstddef>
#include <filesystem>

namespace rt2::core {

// Collaborators the host supplies to the dispatch so the module stays CPU-only
// and linkable into RT2Tests. The registry pointer may be null: the dispatch
// then calls the operation callback directly, mirroring the host's
// m_FileWatchListener-null branch (WalnutApp.cpp:3286-3287).
struct ContentBrowserDispatchContext
{
    std::filesystem::path assetRoot;
    AssetWatchSuppressionRegistry* suppressionRegistry = nullptr;
    ContentBrowserOperationReport* report = nullptr;
    Error* error = nullptr;
};

// Owns the register-then-operate sequence for reimport, rename, move and
// delete. Builds the source path from assetRoot + record.sourcePath, registers
// the operation's suppression paths through RunSuppressedAssetOperation (or
// calls the callback directly when the registry is null), and returns the
// callback's result. Refresh and the delayed suppression clear stay in the
// host because they touch m_ProjectContext and the drain loop.
bool DispatchContentBrowserOperation(
    const ContentBrowserDispatchContext& ctx,
    const AssetRecord& record,
    AssetWatchOperationKind operationKind,
    const std::filesystem::path& destination,
    const AssetWatchSuppressedOperation& operation);

// Link-time-reachable wrapper around ContentBrowserCanOperate so the host's
// panel gate routes through the dispatch module. Whether the host consults
// this wrapper remains a text-probe/interactive-acceptance matter.
bool CanOperateContentBrowser(bool projectActive);

// Link-time-reachable wrapper around ContentBrowserDeleteAllowed so the
// host's delete-button gate routes through the dispatch module. Whether the
// host consults this wrapper remains a text-probe/interactive-acceptance
// matter.
bool AllowDeleteContentBrowser(bool confirmed, size_t dependantCount);

// Link-time-reachable wrapper around ShouldCaptureRecoverySnapshot so the
// host's autosave block routes through the dispatch module. Whether the host
// consults this wrapper remains a text-probe/interactive-acceptance matter.
bool ShouldCaptureRecovery(
    bool scriptRepairPending,
    const AssetMigrationPersistenceGate& migrationGate);

} // namespace rt2::core

#endif // RT2_CORE_CONTENT_BROWSER_DISPATCH_H