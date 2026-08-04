#include "AssetWatchPolicy.h"

#include "AssetIdentity.h"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace rt2::core {
namespace {

std::string Fold(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
    return value;
}

AssetFileEventKind ClassifyExtension(const std::string& extension)
{
    if (extension == ".lua")
        return AssetFileEventKind::ScriptReload;
    if (extension == ".glb" || extension == ".gltf" ||
        extension == ".obj" || extension == ".hdr" ||
        extension == ".exr" || extension == ".rt2meta" ||
        extension == ".rt2prefab")
        return AssetFileEventKind::DatabaseRefresh;
    return AssetFileEventKind::Ignore;
}

AssetWatchEvent MakeEvent(const std::filesystem::path& path,
                          AssetFileAction action)
{
    AssetWatchEvent event;
    event.path = NormalizeWatchPath(path);
    event.action = action;
    event.kind = ClassifyAssetFileEvent(path, action);
    event.refreshDatabase = AssetFileNeedsDatabaseRefresh(path, action);
    return event;
}

} // namespace

AssetFileEventKind ClassifyAssetFileEvent(
    const std::filesystem::path& path,
    AssetFileAction action)
{
    (void)action;
    return ClassifyExtension(Fold(path.extension().u8string()));
}

bool AssetFileNeedsDatabaseRefresh(
    const std::filesystem::path& path,
    AssetFileAction action)
{
    const auto kind = ClassifyAssetFileEvent(path, action);
    return kind == AssetFileEventKind::DatabaseRefresh ||
           (kind == AssetFileEventKind::ScriptReload &&
            action == AssetFileAction::Delete);
}

std::string NormalizeWatchPath(const std::filesystem::path& path)
{
    std::filesystem::path normalized = path;
    if (!normalized.is_absolute())
    {
        std::error_code error;
        normalized = std::filesystem::absolute(normalized, error);
        if (error)
            normalized = path;
    }
    return Fold(normalized.lexically_normal().u8string());
}

AssetWatchSuppressionRegistry::AssetWatchSuppressionRegistry(
    std::mutex* sharedMutex)
    : m_Mutex(sharedMutex ? sharedMutex : &m_OwnMutex)
{
}

void AssetWatchSuppressionRegistry::Register(
    const std::filesystem::path& path)
{
    std::lock_guard<std::mutex> lock(*m_Mutex);
    RegisterLocked(path);
}

void AssetWatchSuppressionRegistry::RegisterMany(
    const std::vector<std::filesystem::path>& paths)
{
    std::lock_guard<std::mutex> lock(*m_Mutex);
    for (const auto& path : paths)
        RegisterLocked(path);
}

bool AssetWatchSuppressionRegistry::IsSuppressed(
    const std::filesystem::path& path) const
{
    std::lock_guard<std::mutex> lock(*m_Mutex);
    return IsSuppressedLocked(path);
}

void AssetWatchSuppressionRegistry::Clear()
{
    std::lock_guard<std::mutex> lock(*m_Mutex);
    ClearLocked();
}

size_t AssetWatchSuppressionRegistry::Size() const
{
    std::lock_guard<std::mutex> lock(*m_Mutex);
    return SizeLocked();
}

void AssetWatchSuppressionRegistry::RegisterLocked(
    const std::filesystem::path& path)
{
    const auto normalized = NormalizeWatchPath(path);
    if (normalized.empty())
        return;
    if (std::find(m_Paths.begin(), m_Paths.end(), normalized) == m_Paths.end())
        m_Paths.push_back(normalized);
}

void AssetWatchSuppressionRegistry::RegisterManyLocked(
    const std::vector<std::filesystem::path>& paths)
{
    for (const auto& path : paths)
        RegisterLocked(path);
}

bool AssetWatchSuppressionRegistry::IsSuppressedLocked(
    const std::filesystem::path& path) const
{
    const auto normalized = NormalizeWatchPath(path);
    return std::find(m_Paths.begin(), m_Paths.end(), normalized) != m_Paths.end();
}

void AssetWatchSuppressionRegistry::ClearLocked()
{
    m_Paths.clear();
}

size_t AssetWatchSuppressionRegistry::SizeLocked() const
{
    return m_Paths.size();
}

AssetWatchEventQueue::AssetWatchEventQueue(std::mutex* sharedMutex)
    : m_Mutex(sharedMutex ? sharedMutex : &m_OwnMutex)
{
}

bool AssetWatchEventQueue::Enqueue(const std::filesystem::path& path,
                                   AssetFileAction action)
{
    std::lock_guard<std::mutex> lock(*m_Mutex);
    return EnqueueLocked(path, action);
}

std::vector<AssetWatchEvent> AssetWatchEventQueue::Drain()
{
    std::lock_guard<std::mutex> lock(*m_Mutex);
    return DrainLocked();
}

bool AssetWatchEventQueue::TakeOverflowed()
{
    std::lock_guard<std::mutex> lock(*m_Mutex);
    return TakeOverflowedLocked();
}

size_t AssetWatchEventQueue::Size() const
{
    std::lock_guard<std::mutex> lock(*m_Mutex);
    return SizeLocked();
}

bool AssetWatchEventQueue::EnqueueLocked(
    const std::filesystem::path& path,
    AssetFileAction action)
{
    const auto event = MakeEvent(path, action);
    for (auto& existing : m_Events)
    {
        if (existing.path == event.path)
        {
            existing.action = event.action;
            existing.kind = event.kind;
            existing.refreshDatabase =
                existing.refreshDatabase || event.refreshDatabase;
            return true;
        }
    }

    if (m_Events.size() >= kAssetWatchQueueLimit)
    {
        m_Events.erase(m_Events.begin());
        m_Overflowed = true;
    }
    m_Events.push_back(event);
    return true;
}

std::vector<AssetWatchEvent> AssetWatchEventQueue::DrainLocked()
{
    auto drained = std::move(m_Events);
    m_Events.clear();
    return drained;
}

bool AssetWatchEventQueue::TakeOverflowedLocked()
{
    const bool overflowed = m_Overflowed;
    m_Overflowed = false;
    return overflowed;
}

size_t AssetWatchEventQueue::SizeLocked() const
{
    return m_Events.size();
}

bool PublishAssetWatchEventLocked(
    AssetWatchSuppressionRegistry& suppressionRegistry,
    AssetWatchEventQueue& pendingEvents,
    const std::filesystem::path& path,
    AssetFileAction action)
{
    if (suppressionRegistry.IsSuppressedLocked(path))
        return false;
    return pendingEvents.EnqueueLocked(path, action);
}

std::vector<std::filesystem::path> AssetWatchSuppressionPaths(
    AssetWatchOperationKind operation,
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& destinationPath)
{
    if (sourcePath.empty())
        return {};

    std::vector<std::filesystem::path> paths{
        sourcePath, AssetSidecarPath(sourcePath)};
    switch (operation)
    {
    case AssetWatchOperationKind::Rename:
    case AssetWatchOperationKind::Move:
        if (!destinationPath.empty())
        {
            paths.push_back(destinationPath);
            paths.push_back(AssetSidecarPath(destinationPath));
        }
        break;
    case AssetWatchOperationKind::Reimport:
    case AssetWatchOperationKind::Delete:
        break;
    }
    return paths;
}

bool RunSuppressedAssetOperation(
    AssetWatchSuppressionRegistry& suppressionRegistry,
    AssetWatchOperationKind operationKind,
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& destinationPath,
    const AssetWatchSuppressedOperation& callback)
{
    suppressionRegistry.RegisterMany(AssetWatchSuppressionPaths(
        operationKind, sourcePath, destinationPath));
    return callback ? callback() : false;
}

AssetWatchRefreshAction DecideWatchRefreshAction(
    bool hasMissedEvents,
    size_t queueSize,
    bool backgroundBusy)
{
    if (backgroundBusy)
        return queueSize >= kAssetWatchQueueLimit
            ? AssetWatchRefreshAction::Truncate
            : AssetWatchRefreshAction::Queue;
    if (hasMissedEvents || queueSize > 0)
        return AssetWatchRefreshAction::RefreshNow;
    return AssetWatchRefreshAction::NoOp;
}

} // namespace rt2::core
