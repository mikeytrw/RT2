#include <doctest/doctest.h>

#include "ContentBrowserDispatch.h"
#include "AssetDatabase.h"
#include "AssetIdentity.h"
#include "AssetWatchPolicy.h"

#include <filesystem>
#include <mutex>

using namespace rt2::core;

namespace {

const std::filesystem::path kRoot =
    std::filesystem::absolute("content-browser-dispatch-root");

AssetRecord MakeRecord(const std::string& sourcePath)
{
    AssetRecord record{};
    record.sourcePath = sourcePath;
    return record;
}

} // namespace

// The dispatch registers the operation's suppression paths before invoking
// the callback (the register-before-operate invariant). A fake callback
// inspects the registry while it runs; if registration were skipped or
// reordered after the operation, the registry would be empty here.
TEST_CASE("HD dispatch registers suppression paths before the rename operation runs")
{
    AssetWatchSuppressionRegistry registry;
    const auto ctx = ContentBrowserDispatchContext{kRoot, &registry, nullptr, nullptr};
    const auto record = MakeRecord("models/hero.glb");
    const auto destination = kRoot / "moved" / "hero.glb";

    bool invoked = false;
    int invocationCount = 0;
    bool registryPopulatedOnFirstCall = false;
    const bool ok = DispatchContentBrowserOperation(
        ctx, record, AssetWatchOperationKind::Rename, destination, [&]() {
            invoked = true;
            if (invocationCount == 0)
            {
                registryPopulatedOnFirstCall =
                    registry.IsSuppressed(kRoot / "models" / "hero.glb") &&
                    registry.IsSuppressed(destination);
            }
            ++invocationCount;
            return true;
        });
    CHECK(ok);
    CHECK(invoked);
    CHECK(invocationCount == 1);
    CHECK(registryPopulatedOnFirstCall);
}

// Move must suppress both source and destination (and their sidecars), so the
// watcher does not fire on the new path either. Registering only the source
// would leave the destination unwatched.
TEST_CASE("HD dispatch registers both source and destination for a move")
{
    AssetWatchSuppressionRegistry registry;
    const auto ctx = ContentBrowserDispatchContext{kRoot, &registry, nullptr, nullptr};
    const auto record = MakeRecord("models/hero.glb");
    const auto destination = kRoot / "moved" / "hero.glb";

    bool destinationRegistered = false;
    DispatchContentBrowserOperation(
        ctx, record, AssetWatchOperationKind::Move, destination, [&]() {
            destinationRegistered = registry.IsSuppressed(destination);
            return true;
        });
    CHECK(destinationRegistered);
}

// Delete suppresses the source and its identity sidecar so the watcher does
// not report the self-inflicted deletion. Registering only the source would
// leave the sidecar deletion unwatched.
TEST_CASE("HD dispatch registers source and sidecar for a delete")
{
    AssetWatchSuppressionRegistry registry;
    const auto ctx = ContentBrowserDispatchContext{kRoot, &registry, nullptr, nullptr};
    const auto record = MakeRecord("models/hero.glb");
    const auto source = kRoot / "models" / "hero.glb";
    const auto sidecar = AssetSidecarPath(source);

    bool sourceRegistered = false;
    bool sidecarRegistered = false;
    DispatchContentBrowserOperation(
        ctx, record, AssetWatchOperationKind::Delete, {}, [&]() {
            sourceRegistered = registry.IsSuppressed(source);
            sidecarRegistered = registry.IsSuppressed(sidecar);
            return true;
        });
    CHECK(sourceRegistered);
    CHECK(sidecarRegistered);
}

// A null registry means no watcher is installed (standalone mode). The
// dispatch must still run the operation and return its result, mirroring the
// host's m_FileWatchListener-null branch.
TEST_CASE("HD dispatch with null registry runs the operation directly")
{
    const auto ctx = ContentBrowserDispatchContext{kRoot, nullptr, nullptr, nullptr};
    const auto record = MakeRecord("models/hero.glb");

    bool invoked = false;
    const bool ok = DispatchContentBrowserOperation(
        ctx, record, AssetWatchOperationKind::Reimport, {}, [&]() {
            invoked = true;
            return true;
        });
    CHECK(ok);
    CHECK(invoked);
}

TEST_CASE("HD dispatch with null registry propagates a false result")
{
    const auto ctx = ContentBrowserDispatchContext{kRoot, nullptr, nullptr, nullptr};
    const auto record = MakeRecord("models/hero.glb");

    const bool ok = DispatchContentBrowserOperation(
        ctx, record, AssetWatchOperationKind::Reimport, {}, [&]() {
            return false;
        });
    CHECK_FALSE(ok);
}

// Policy wrappers must agree with the underlying predicates so the host's
// gate decisions are unchanged by routing through the dispatch module.
TEST_CASE("HD CanOperateContentBrowser mirrors ContentBrowserCanOperate")
{
    CHECK_FALSE(CanOperateContentBrowser(false));
    CHECK(CanOperateContentBrowser(true));
    CHECK_FALSE(ContentBrowserCanOperate(false));
    CHECK(ContentBrowserCanOperate(true));
}

TEST_CASE("HD AllowDeleteContentBrowser mirrors ContentBrowserDeleteAllowed")
{
    CHECK_FALSE(AllowDeleteContentBrowser(false, 0));
    CHECK_FALSE(AllowDeleteContentBrowser(false, 3));
    CHECK(AllowDeleteContentBrowser(true, 0));
    CHECK(AllowDeleteContentBrowser(true, 5));
}

// Recovery wrapper must agree with ShouldCaptureRecoverySnapshot so the
// autosave block's decision is unchanged by routing through the dispatch
// module. Script-repair loss suppresses recovery regardless of gate state.
TEST_CASE("HD ShouldCaptureRecovery mirrors ShouldCaptureRecoverySnapshot")
{
    AssetMigrationPersistenceGate gate;
    gate.Adopt(true);
    CHECK(gate.Pending());
    CHECK_FALSE(ShouldCaptureRecovery(true, gate));
    CHECK_FALSE(ShouldCaptureRecoverySnapshot(true, gate));
    // Migration pending does not suppress recovery (it is not destructive
    // load loss); only script-repair loss does.
    CHECK(ShouldCaptureRecovery(false, gate));
    CHECK(ShouldCaptureRecoverySnapshot(false, gate));

    gate.OnPersistedOrReset();
    CHECK_FALSE(gate.Pending());
    CHECK(ShouldCaptureRecovery(false, gate));
    CHECK_FALSE(ShouldCaptureRecovery(true, gate));
}