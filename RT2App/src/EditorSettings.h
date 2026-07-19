#pragma once

#ifndef RT2_CORE_EDITOR_SETTINGS_H
#define RT2_CORE_EDITOR_SETTINGS_H

#include "core/Error.h"
#include "InputTypes.h"

#include <filesystem>
#include <string>
#include <vector>
#include <cstdint>

// ============================================================================
// EditorSettingsStore — per-user editor preferences (project root + recent
// native scenes). Versioned JSON, atomic write, MRU recents.
//
// Production storage lives under <appDataRoot>/settings.json. Tests inject a
// temporary directory as appDataRoot so they never touch the developer's
// LocalAppData.
//
// CPU-only: no Vulkan/Walnut/ImGui/GLFW/NRD/NRI dependency. Links cleanly
// into RT2Tests and RT2SliceRunner.
//
// Recent-scenes rules:
//   - Native .rt2scene files only (updates happen on successful open/save).
//   - Most-recent-first.
//   - Bounded (default 10).
//   - Normalized + deduplicated case-insensitively on Windows.
//   - Failed/cancelled opens/saves do not change the list.
//
// Project-root rules:
//   - Optional editor preference used as an initial file-dialog location.
//   - Does NOT reinterpret the Phase 1A scene-relative asset-reference
//     contract. A saved .rt2scene still resolves durable asset references
//     using the documented scene-root semantics.
//
// Schema:
//   {
//     "version": 2,
//     "projectRoot": "<absolute path or empty>",
//     "recentScenes": [ "<absolute .rt2scene path>", ... ],
//     "inputContexts": [
//       {
//         "contextId": "editor",
//         "mappings": [
//           {
//             "name": "move_forward",
//             "isAxis": true,
//             "actions": [],
//             "axes": [
//               { "device": 0, "code": 0, "positive": 87, "negative": 83,
//                 "gamepadSlot": -1, "deadZone": 0.15, "invert": false }
//             ]
//           },
//           ...
//         ]
//       },
//       ...
//     ]
//   }
//
// Version 1 files (no inputContexts field) are migrated to v2 on load:
// the version field is updated and inputContexts is left empty, which
// signals to InputService to use built-in defaults (LoadDefaults()).
//
// Atomic write: write to settings.json.tmp, then MoveFileExW/ReplaceFileW.
// A failed write leaves the previous valid settings file intact.
// ============================================================================

namespace rt2::core {

class EditorSettingsStore
{
public:
    static constexpr uint32_t SettingsVersion = 2;
    static constexpr uint32_t kSupportedLegacyVersion = 1;
    static constexpr size_t   kDefaultMaxRecents = 10;

    explicit EditorSettingsStore(std::filesystem::path appDataRoot,
                                 size_t maxRecents = kDefaultMaxRecents);

    // Load settings from <appDataRoot>/settings.json. On missing file, leaves
    // defaults (empty project root, empty recents, empty input contexts) and
    // returns true. On malformed JSON, returns false with err and leaves
    // defaults. A v1 file (no inputContexts field) is migrated to v2:
    // version is updated, inputContexts is left empty (caller falls back
    // to InputService::LoadDefaults()). Unknown optional fields are
    // ignored safely.
    bool Load(Error& err);

    // Atomically save settings to <appDataRoot>/settings.json. Creates the
    // directory if needed. On failure, leaves the previous file intact.
    // Always writes the current SettingsVersion.
    bool Save(Error& err) const;

    // ---- Project root ----
    const std::filesystem::path& GetProjectRoot() const { return m_ProjectRoot; }
    void SetProjectRoot(std::filesystem::path p) { m_ProjectRoot = Normalize(p); }
    void ClearProjectRoot() { m_ProjectRoot.clear(); }

    // ---- Recent scenes ----
    const std::vector<std::filesystem::path>& GetRecentScenes() const { return m_RecentScenes; }

    // Add or promote a native .rt2scene path to the front of the recents
    // list. Normalizes and deduplicates case-insensitively on Windows. Trims
    // to maxRecents. Use only after a successful native open or save.
    void AddRecentScene(const std::filesystem::path& path);

    // Remove a recent entry explicitly (e.g. user clicks "remove" on a
    // missing file). No-op if not present.
    void RemoveRecentScene(const std::filesystem::path& path);

    // Clear all recents.
    void ClearRecentScenes() { m_RecentScenes.clear(); }

    size_t GetMaxRecents() const { return m_MaxRecents; }

    // ---- Input contexts (Phase 5) ----

    // Serialized input mappings grouped by context. Each entry is a
    // (contextId, vector<InputMapping>) pair. The InputService owns
    // the InputContext objects; this is the portable serialized form.
    struct InputContextRecord
    {
        std::string contextId;
        std::vector<InputMapping> mappings;
    };

    const std::vector<InputContextRecord>& GetInputContexts() const
    { return m_InputContexts; }
    void SetInputContexts(std::vector<InputContextRecord> v)
    { m_InputContexts = std::move(v); }
    void ClearInputContexts() { m_InputContexts.clear(); }

    // Static helpers exposed for tests.
    // Normalize a path to a stable absolute form with forward-slash generic
    // separators removed of trailing slashes for comparison/storage.
    static std::filesystem::path Normalize(const std::filesystem::path& p);

    // Case-folded comparison key for deduplication. On Windows this folds
    // case; elsewhere it leaves the string as-is.
    static std::string FoldKey(const std::filesystem::path& p);

private:
    std::filesystem::path              m_AppDataRoot;
    size_t                             m_MaxRecents;
    std::filesystem::path              m_ProjectRoot;
    std::vector<std::filesystem::path> m_RecentScenes;
    std::vector<InputContextRecord>    m_InputContexts;

    std::filesystem::path SettingsFilePath() const;
};

} // namespace rt2::core

#endif // RT2_CORE_EDITOR_SETTINGS_H
