#pragma once

#ifndef RT2_CORE_ASSET_WATCH_POLICY_H
#define RT2_CORE_ASSET_WATCH_POLICY_H

#include <cstddef>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace rt2::core {

enum class AssetFileAction
{
    Add,
    Modified,
    Delete,
    Moved,
};

enum class AssetFileEventKind
{
    ScriptReload,
    DatabaseRefresh,
    Ignore,
};

AssetFileEventKind ClassifyAssetFileEvent(
    const std::filesystem::path& path,
    AssetFileAction action);

bool AssetFileNeedsDatabaseRefresh(
    const std::filesystem::path& path,
    AssetFileAction action);

// Watch paths are physical absolute paths. On Windows normalization also
// folds case, matching the filesystem's event identity rules.
std::string NormalizeWatchPath(const std::filesystem::path& path);

class AssetWatchSuppressionRegistry
{
public:
    // A host listener may provide its event mutex so suppression checks and
    // event publication occur in one critical section. Tests use the private
    // mutex supplied by the default constructor.
    explicit AssetWatchSuppressionRegistry(std::mutex* sharedMutex = nullptr);

    void Register(const std::filesystem::path& path);
    void RegisterMany(const std::vector<std::filesystem::path>& paths);
    bool IsSuppressed(const std::filesystem::path& path) const;
    void Clear();
    size_t Size() const;

    // These methods require the caller to hold the mutex passed to the
    // constructor. They are the listener's atomic check/update seam.
    void RegisterLocked(const std::filesystem::path& path);
    void RegisterManyLocked(const std::vector<std::filesystem::path>& paths);
    bool IsSuppressedLocked(const std::filesystem::path& path) const;
    void ClearLocked();
    size_t SizeLocked() const;

private:
    mutable std::mutex m_OwnMutex;
    std::mutex* m_Mutex;
    std::vector<std::string> m_Paths;
};

constexpr size_t kAssetWatchQueueLimit = 100;
constexpr int kAssetWatchDebounceMilliseconds = 100;

// Apply the generic main-thread debounce retention policy across script,
// refresh, and prefab buffers. Returns true when older events were discarded.
bool TruncateAssetWatchBuffers(
    std::vector<std::string>& scriptPaths,
    std::vector<std::string>& refreshPaths,
    std::vector<std::string>& prefabPaths,
    std::size_t limit);

enum class AssetWatchDriveKind
{
    Local,
    Network,
};

// efsw's Windows default is 63 KiB. A larger buffer is safe for local
// drives, but Windows rejects it for network shares.
constexpr size_t AssetWatchBufferSize(AssetWatchDriveKind drive)
{
    return drive == AssetWatchDriveKind::Local ? 256u * 1024u : 63u * 1024u;
}

struct AssetWatchEvent
{
    std::string path;
    AssetFileAction action = AssetFileAction::Modified;
    AssetFileEventKind kind = AssetFileEventKind::Ignore;
    bool refreshDatabase = false;
};

class AssetWatchEventQueue
{
public:
    explicit AssetWatchEventQueue(std::mutex* sharedMutex = nullptr);

    // Returns true when the event was accepted. A new event beyond the limit
    // drops the oldest entry, retaining the most recent 100 paths and setting
    // the overflow marker.
    bool Enqueue(const std::filesystem::path& path, AssetFileAction action);
    std::vector<AssetWatchEvent> Drain();
    bool TakeOverflowed();
    size_t Size() const;

    bool EnqueueLocked(const std::filesystem::path& path,
                       AssetFileAction action);
    std::vector<AssetWatchEvent> DrainLocked();
    bool TakeOverflowedLocked();
    size_t SizeLocked() const;

private:
    mutable std::mutex m_OwnMutex;
    std::mutex* m_Mutex;
    std::vector<AssetWatchEvent> m_Events;
    bool m_Overflowed = false;
};

// The watcher thread must make the suppression check and queue publication
// one operation under the host's shared mutex. No filesystem or scan work is
// performed by this seam.
bool PublishAssetWatchEventLocked(
    AssetWatchSuppressionRegistry& suppressionRegistry,
    AssetWatchEventQueue& pendingEvents,
    const std::filesystem::path& path,
    AssetFileAction action);

enum class AssetWatchOperationKind
{
    Reimport,
    Rename,
    Move,
    Delete,
};

std::vector<std::filesystem::path> AssetWatchSuppressionPaths(
    AssetWatchOperationKind operation,
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& destinationPath = {});

using AssetWatchSuppressedOperation = std::function<bool()>;

// Register the paths before invoking the operation, then leave the entries
// installed for the host's W7-A2 delayed clear. The callback is deliberately
// injected so CPU-only tests can observe the registry during the operation.
bool RunSuppressedAssetOperation(
    AssetWatchSuppressionRegistry& suppressionRegistry,
    AssetWatchOperationKind operationKind,
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& destinationPath,
    const AssetWatchSuppressedOperation& callback);

enum class AssetWatchRefreshAction
{
    NoOp,
    RefreshNow,
    Queue,
    Truncate,
};

AssetWatchRefreshAction DecideWatchRefreshAction(
    bool hasMissedEvents,
    size_t queueSize,
    bool backgroundBusy);

} // namespace rt2::core

#endif // RT2_CORE_ASSET_WATCH_POLICY_H
