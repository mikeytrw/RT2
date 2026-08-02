#include "ContentBrowserDispatch.h"

namespace rt2::core {

bool DispatchContentBrowserOperation(
    const ContentBrowserDispatchContext& ctx,
    const AssetRecord& record,
    AssetWatchOperationKind operationKind,
    const std::filesystem::path& destination,
    const AssetWatchSuppressedOperation& operation)
{
    if (!operation)
        return false;

    const auto source =
        (ctx.assetRoot / std::filesystem::u8path(record.sourcePath))
            .lexically_normal();

    if (ctx.suppressionRegistry != nullptr)
    {
        return RunSuppressedAssetOperation(
            *ctx.suppressionRegistry, operationKind, source, destination,
            operation);
    }

    return operation();
}

bool CanOperateContentBrowser(bool projectActive)
{
    return ContentBrowserCanOperate(projectActive);
}

bool AllowDeleteContentBrowser(bool confirmed, size_t dependantCount)
{
    return ContentBrowserDeleteAllowed(confirmed, dependantCount);
}

bool ShouldCaptureRecovery(
    bool scriptRepairPending,
    const AssetMigrationPersistenceGate& migrationGate)
{
    return ShouldCaptureRecoverySnapshot(scriptRepairPending, migrationGate);
}

} // namespace rt2::core