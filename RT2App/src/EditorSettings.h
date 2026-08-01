#pragma once

#ifndef RT2_CORE_EDITOR_SETTINGS_H
#define RT2_CORE_EDITOR_SETTINGS_H

#include "InputConfig.h"
#include "core/Error.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Per-user editor preferences. `lastBrowseDirectory` is dialog state only;
// it never supplies asset, script, recovery, or project resolution semantics.
// The portable project model lives in Project.h.

namespace rt2::core {

struct EditorSettingsMigrationDiagnostic
{
    enum class Kind { DroppedReservedContext, PromotedOverride };
    Kind kind = Kind::DroppedReservedContext;
    std::string contextId;
    std::string mappingName;
};

struct EditorSettingsLoadReport
{
    uint32_t sourceVersion = 0;
    bool migrated = false;
    std::vector<EditorSettingsMigrationDiagnostic> diagnostics;
};

class EditorSettingsStore
{
public:
    static constexpr uint32_t SettingsVersion = 3;
    static constexpr uint32_t kOldestSupportedVersion = 1;
    static constexpr size_t kDefaultMaxRecents = 10;

    explicit EditorSettingsStore(std::filesystem::path appDataRoot,
                                 size_t maxRecents = kDefaultMaxRecents);

    bool Load(Error& err);
    bool Load(EditorSettingsLoadReport& report, Error& err);
    bool Save(Error& err) const;

    const std::filesystem::path& GetLastBrowseDirectory() const
    { return m_LastBrowseDirectory; }
    void SetLastBrowseDirectory(std::filesystem::path path)
    { m_LastBrowseDirectory = Normalize(path); }
    void ClearLastBrowseDirectory() { m_LastBrowseDirectory.clear(); }

    const std::vector<std::filesystem::path>& GetRecentScenes() const
    { return m_RecentScenes; }
    void AddRecentScene(const std::filesystem::path& path);
    void RemoveRecentScene(const std::filesystem::path& path);
    void ClearRecentScenes() { m_RecentScenes.clear(); }
    size_t GetMaxRecents() const { return m_MaxRecents; }

    const std::vector<InputContextRecord>& GetInputOverrides() const
    { return m_InputOverrides; }
    void SetInputOverrides(std::vector<InputContextRecord> value)
    { m_InputOverrides = std::move(value); }
    void ClearInputOverrides() { m_InputOverrides.clear(); }

    static std::filesystem::path Normalize(const std::filesystem::path& path);
    static std::string FoldKey(const std::filesystem::path& path);

private:
    std::filesystem::path m_AppDataRoot;
    size_t m_MaxRecents;
    std::filesystem::path m_LastBrowseDirectory;
    std::vector<std::filesystem::path> m_RecentScenes;
    std::vector<InputContextRecord> m_InputOverrides;

    std::filesystem::path SettingsFilePath() const;
};

} // namespace rt2::core

#endif // RT2_CORE_EDITOR_SETTINGS_H
