#include <doctest/doctest.h>

#include "AssetWatchPolicy.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

using namespace rt2::core;

namespace {

const std::filesystem::path kRoot =
    std::filesystem::absolute("phase7-w7-watch-root");

AssetFileAction kActions[] = {
    AssetFileAction::Add,
    AssetFileAction::Modified,
    AssetFileAction::Delete,
    AssetFileAction::Moved,
};

std::string WalnutAppSource()
{
    std::ifstream input("RT2App/src/WalnutApp.cpp");
    REQUIRE(input.good());
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

std::string SourceSlice(const std::string& source,
                        const std::string& begin,
                        const std::string& end)
{
    const auto beginAt = source.find(begin);
    REQUIRE(beginAt != std::string::npos);
    const auto endAt = source.find(end, beginAt + begin.size());
    REQUIRE(endAt != std::string::npos);
    return source.substr(beginAt, endAt - beginAt);
}

} // namespace

TEST_CASE("Phase7 W7 classifies supported asset events by extension")
{
    for (const auto action : kActions)
    {
        CHECK(ClassifyAssetFileEvent(kRoot / "script.LUA", action) ==
              AssetFileEventKind::ScriptReload);
        CHECK(ClassifyAssetFileEvent(kRoot / "model.glb", action) ==
              AssetFileEventKind::DatabaseRefresh);
        CHECK(ClassifyAssetFileEvent(kRoot / "model.gltf", action) ==
              AssetFileEventKind::DatabaseRefresh);
        CHECK(ClassifyAssetFileEvent(kRoot / "model.obj", action) ==
              AssetFileEventKind::DatabaseRefresh);
        CHECK(ClassifyAssetFileEvent(kRoot / "sky.hdr", action) ==
              AssetFileEventKind::DatabaseRefresh);
        CHECK(ClassifyAssetFileEvent(kRoot / "sky.exr", action) ==
              AssetFileEventKind::DatabaseRefresh);
        CHECK(ClassifyAssetFileEvent(kRoot / "model.rt2meta", action) ==
              AssetFileEventKind::DatabaseRefresh);
        CHECK(ClassifyAssetFileEvent(kRoot / "scene.rt2scene", action) ==
              AssetFileEventKind::Ignore);
        CHECK(ClassifyAssetFileEvent(kRoot / "notes.txt", action) ==
              AssetFileEventKind::Ignore);
    }
    CHECK(AssetFileNeedsDatabaseRefresh(
        kRoot / "deleted.lua", AssetFileAction::Delete));
    CHECK_FALSE(AssetFileNeedsDatabaseRefresh(
        kRoot / "changed.lua", AssetFileAction::Modified));
}

TEST_CASE("Phase7 W7 suppression registry is normalized and clearable")
{
    AssetWatchSuppressionRegistry registry;
    const auto source = kRoot / "models" / "hero.glb";
    registry.Register(source);
    registry.Register(source.lexically_normal());
    CHECK(registry.Size() == 1);
    CHECK(registry.IsSuppressed(source));
    CHECK(registry.IsSuppressed(kRoot / "models" / "." / "hero.glb"));
    CHECK_FALSE(registry.IsSuppressed(kRoot / "models" / "other.glb"));
    registry.Clear();
    CHECK(registry.Size() == 0);
    CHECK_FALSE(registry.IsSuppressed(source));
}

TEST_CASE("Phase7 W7 event queue coalesces and truncates by path")
{
    AssetWatchEventQueue queue;
    for (size_t i = 0; i < kAssetWatchQueueLimit; ++i)
        REQUIRE(queue.Enqueue(kRoot / ("asset" + std::to_string(i) + ".glb"),
                              AssetFileAction::Modified));
    REQUIRE(queue.Size() == kAssetWatchQueueLimit);
    REQUIRE(queue.Enqueue(kRoot / "asset42.glb", AssetFileAction::Delete));
    CHECK(queue.Size() == kAssetWatchQueueLimit);
    CHECK_FALSE(queue.TakeOverflowed());
    REQUIRE(queue.Enqueue(kRoot / "newest.glb", AssetFileAction::Add));
    CHECK(queue.Size() == kAssetWatchQueueLimit);
    CHECK(queue.TakeOverflowed());
    const auto events = queue.Drain();
    REQUIRE(events.size() == kAssetWatchQueueLimit);
    CHECK(events.back().path == NormalizeWatchPath(kRoot / "newest.glb"));
    const auto updated = std::find_if(events.begin(), events.end(),
        [](const AssetWatchEvent& event) {
            return event.path.find("asset42.glb") != std::string::npos;
        });
    REQUIRE(updated != events.end());
    CHECK(updated->action == AssetFileAction::Delete);
}

TEST_CASE("Phase7 W7 repeated events for one path have one queued refresh")
{
    AssetWatchEventQueue queue;
    for (size_t i = 0; i < kAssetWatchQueueLimit; ++i)
        REQUIRE(queue.Enqueue(kRoot / "model.glb", AssetFileAction::Modified));
    const auto events = queue.Drain();
    REQUIRE(events.size() == 1);
    CHECK(events.front().refreshDatabase);
}

TEST_CASE("Phase7 W7 watcher buffer and debounce policy are explicit")
{
    CHECK(AssetWatchBufferSize(AssetWatchDriveKind::Local) == 256u * 1024u);
    CHECK(AssetWatchBufferSize(AssetWatchDriveKind::Network) == 63u * 1024u);
    CHECK(kAssetWatchDebounceMilliseconds == 100);
}

TEST_CASE("Phase7 W7 refresh decision queues while busy and refreshes after")
{
    CHECK(DecideWatchRefreshAction(false, 0, false) ==
          AssetWatchRefreshAction::NoOp);
    CHECK(DecideWatchRefreshAction(false, 1, true) ==
          AssetWatchRefreshAction::Queue);
    CHECK(DecideWatchRefreshAction(false, kAssetWatchQueueLimit, true) ==
          AssetWatchRefreshAction::Truncate);
    CHECK(DecideWatchRefreshAction(false, 1, false) ==
          AssetWatchRefreshAction::RefreshNow);
}

TEST_CASE("Phase7 W7 missed events force a refresh when idle")
{
    CHECK(DecideWatchRefreshAction(true, 0, false) ==
          AssetWatchRefreshAction::RefreshNow);
    CHECK(DecideWatchRefreshAction(true, 1, false) ==
          AssetWatchRefreshAction::RefreshNow);
    CHECK(DecideWatchRefreshAction(true, 0, true) ==
          AssetWatchRefreshAction::Queue);
}

TEST_CASE("Phase7 W7 shared mutex makes suppression and enqueue atomic")
{
    std::mutex mutex;
    AssetWatchSuppressionRegistry registry(&mutex);
    AssetWatchEventQueue queue(&mutex);
    const auto path = kRoot / "models" / "hero.glb";
    std::lock_guard<std::mutex> lock(mutex);
    registry.RegisterLocked(path);
    CHECK(registry.IsSuppressedLocked(path));
    CHECK_FALSE(PublishAssetWatchEventLocked(
        registry, queue, path, AssetFileAction::Modified));
    CHECK(queue.SizeLocked() == 0);
    registry.ClearLocked();
    CHECK(PublishAssetWatchEventLocked(
        registry, queue, path, AssetFileAction::Modified));
    CHECK(queue.SizeLocked() == 1);
}

TEST_CASE("Phase7 W7 W6 operations register paths before filesystem callbacks")
{
    const auto source = kRoot / "model.glb";
    const auto destination = kRoot / "moved" / "model.glb";
    struct OperationCase
    {
        AssetWatchOperationKind kind;
        std::filesystem::path destination;
        size_t expectedPathCount;
    };
    const std::vector<OperationCase> operations{
        {AssetWatchOperationKind::Reimport, {}, 2},
        {AssetWatchOperationKind::Rename, destination, 4},
        {AssetWatchOperationKind::Move, destination, 4},
        {AssetWatchOperationKind::Delete, {}, 2},
    };

    AssetWatchSuppressionRegistry registry;
    for (const auto& operation : operations)
    {
        const auto expectedPaths = AssetWatchSuppressionPaths(
            operation.kind, source, operation.destination);
        REQUIRE(expectedPaths.size() == operation.expectedPathCount);
        bool invoked = false;
        bool unsuppressedEventObserved = false;
        CHECK(RunSuppressedAssetOperation(
            registry, operation.kind, source, operation.destination, [&]() {
            invoked = true;
            for (const auto& path : expectedPaths)
                unsuppressedEventObserved =
                    unsuppressedEventObserved || !registry.IsSuppressed(path);
            return true;
        }));
        CHECK(invoked);
        CHECK_FALSE(unsuppressedEventObserved);
        registry.Clear();
    }
}

TEST_CASE("Phase7 W7 static check: host drains duplicate asset paths through one refresh")
{
    const auto source = WalnutAppSource();
    const auto drain = SourceSlice(
        source, "void DrainAssetWatchChanges", "bool IsNetworkWatchRoot");
    CHECK(drain.find("std::find(m_DebouncedRefreshPaths.begin()") !=
          std::string::npos);
    CHECK(drain.find("RefreshProjectAssets();") != std::string::npos);
    CHECK(drain.find("kAssetWatchQueueLimit") != std::string::npos);
}

TEST_CASE("Phase7 W7 static check: host queues watcher events during background work")
{
    const auto source = WalnutAppSource();
    const auto drain = SourceSlice(
        source, "void DrainAssetWatchChanges", "bool IsNetworkWatchRoot");
    CHECK(drain.find("DecideWatchRefreshAction") != std::string::npos);
    CHECK(drain.find("AssetWatchRefreshAction::Queue") != std::string::npos);
    CHECK(source.find("m_BackgroundWork.reset();") != std::string::npos);
    CHECK(source.find("DrainAssetWatchChanges(true);") != std::string::npos);
}

TEST_CASE("Phase7 W7 static check: missed watcher events force an immediate host refresh")
{
    const auto source = WalnutAppSource();
    const auto listener = SourceSlice(
        source, "class AssetWatchListener", "// Declaration order matters");
    const auto drain = SourceSlice(
        source, "void DrainAssetWatchChanges", "bool IsNetworkWatchRoot");
    CHECK(listener.find("handleMissedFileActions") != std::string::npos);
    CHECK(listener.find("missedEvents = true") != std::string::npos);
    CHECK(drain.find("File system events may have been missed") !=
          std::string::npos);
    CHECK(drain.find("m_AssetWatchMissedEvents") != std::string::npos);
}

TEST_CASE("Phase7 W7 static check: watcher scope is project assetRoot only")
{
    const auto source = WalnutAppSource();
    CHECK(source.find("ConfigureAssetRootWatch(staged->project.assetRoot)") !=
          std::string::npos);
    CHECK(source.find("std::optional<efsw::WatchID>") != std::string::npos);
    CHECK(source.find("m_ActiveWatchId") != std::string::npos);
    CHECK(source.find("ScriptWatchDirectoryForCandidate") == std::string::npos);
}

TEST_CASE("Phase7 W7 scene files stay outside automatic reload policy")
{
    for (const auto action : kActions)
        CHECK(ClassifyAssetFileEvent(kRoot / "open.rt2scene", action) ==
              AssetFileEventKind::Ignore);
    const auto source = WalnutAppSource();
    CHECK(source.find("ConfigureAssetRootWatch") != std::string::npos);
    CHECK(source.find("OpenRt2SceneInternal") != std::string::npos);
}

TEST_CASE("Phase7 W7 static check: watcher never performs identity assignment")
{
    const auto source = WalnutAppSource();
    const auto drain = SourceSlice(
        source, "void DrainAssetWatchChanges", "bool IsNetworkWatchRoot");
    CHECK(drain.find("ResolveOrAssign") == std::string::npos);
    CHECK(drain.find("RefreshProjectAssets") != std::string::npos);
}

TEST_CASE("Phase7 W7 static check: local and network watcher buffers are distinct")
{
    CHECK(AssetWatchBufferSize(AssetWatchDriveKind::Local) == 256u * 1024u);
    CHECK(AssetWatchBufferSize(AssetWatchDriveKind::Network) == 63u * 1024u);
    const auto source = WalnutAppSource();
    CHECK(source.find("DRIVE_REMOTE") != std::string::npos);
    CHECK(source.find("Options::WinBufferSize") != std::string::npos);
}

TEST_CASE("Phase7 W7 static check: atomic save events retain the debounce window")
{
    AssetWatchEventQueue queue;
    REQUIRE(queue.Enqueue(kRoot / "script.lua", AssetFileAction::Modified));
    REQUIRE(queue.Enqueue(kRoot / "script.lua", AssetFileAction::Add));
    REQUIRE(queue.Enqueue(kRoot / "script.lua", AssetFileAction::Delete));
    CHECK(queue.Drain().size() == 1);
    CHECK(kAssetWatchDebounceMilliseconds == 100);
    const auto source = WalnutAppSource();
    CHECK(source.find("kAssetWatchDebounceMilliseconds") != std::string::npos);
}

TEST_CASE("Phase7 W7 static check: explicit Refresh remains available as a backstop")
{
    const auto source = WalnutAppSource();
    CHECK(source.find("Refresh Assets") != std::string::npos);
    CHECK(source.find("if (ImGui::Button(\"Refresh\"))") !=
          std::string::npos);
    CHECK(source.find("Asset change queue overflowed; use Refresh") !=
          std::string::npos);
}
