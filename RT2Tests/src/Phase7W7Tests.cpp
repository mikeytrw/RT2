#include <doctest/doctest.h>

#include "AssetWatchPolicy.h"

#include <algorithm>
#include <filesystem>
#include <mutex>
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
    CHECK(queue.EnqueueLocked(path, AssetFileAction::Modified));
    // The host checks suppression before enqueue; this queue call models the
    // subsequent publication while the same mutex is still held.
    CHECK(queue.SizeLocked() == 1);
}
