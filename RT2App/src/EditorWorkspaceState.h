#pragma once

#ifndef RT2_CORE_EDITOR_WORKSPACE_STATE_H
#define RT2_CORE_EDITOR_WORKSPACE_STATE_H

#include "core/Error.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// ============================================================================
// EditorWorkspaceState — CPU-only workspace preference policy and checked
// persistence for the native editor workspace (plan package "Persistence
// foundation").
//
// This module knows file paths, startup classification, view_config
// parsing/serialization, atomic file replacement, backups, debounce/retry
// policy, and durable in-memory diagnostic status. It does NOT know ImGui:
// window/dock node IDs, DockBuilder, style, and frame lifecycle live in the
// UI adapter (package 2) and the RT2 host layer. Geometry is transported as
// opaque ini bytes: the host reads bytes here and hands them to
// LoadIniSettingsFromMemory, and hands SaveIniSettingsToMemory output back
// for checked persistence.
//
// Boundaries (glossary: Asset / Authoring / Scene / GPU do not apply; the
// boundary here is interactive-preferences vs everything else):
//   - Layout/geometry (imgui.ini) and visibility (view_config.txt) stay
//     per-user files under the app-data root. They never move into project
//     or scene documents and never merge into EditorSettingsStore's schema.
//   - The executable-directory portable imgui.ini is a read-only seed. This
//     module never writes to it; subsequent settings go to the user file.
//   - Headless operation must not rewrite interactive preferences: when
//     headless is set, every write below is a checked no-op success.
//
// Failure policy (the codebase's characteristic bug is silent failure, so
// every fallible operation returns bool + typed Error; nothing returns
// empty/false without a diagnostic):
//   - Existence/read errors mean PROTECTED existing state, never absence.
//   - Empty means present-but-empty: user ini stays protected until explicit
//     Reset; readable empty view_config uses legacy defaults and is backed
//     up before rewrite; empty portable seed stays protected until Reset.
//   - The two files are independently atomic, not a cross-file transaction.
//     A partial save reports "workspace changes not fully saved", retains
//     both backups, and retries only unsaved parts.
//   - Backups (<file>.bak) are taken before the first rewrite of a file
//     that had content. Reset is blocked until checked backups succeed.
//
// Keep this header free of ImGui/Walnut/Vulkan so it links into RT2Tests,
// RT2SliceRunner-class closures, RT2ImGuiProbe, and RT2App alike.
// ============================================================================

namespace rt2::core {

inline constexpr const char* kWorkspaceIniFileName = "imgui.ini";
inline constexpr const char* kWorkspaceViewConfigFileName = "view_config.txt";
inline constexpr const char* kWorkspaceBackupSuffix = ".bak";

// Normal saves are debounced after the last snapshot change; automatic
// retries after a failure wait for the bounded retry delay. An explicit
// user Retry bypasses the delay. The clock is injected (steady_clock)
// so policy is testable without sleeps.
inline constexpr double kWorkspaceSaveDebounceSeconds = 1.0;
inline constexpr double kWorkspaceRetryDelaySeconds = 5.0;

// Performance detail bounds (RT2Layer kPerfLevelBasic..kPerfLevelEverything).
inline constexpr int kWorkspacePerfDetailMin = 0;
inline constexpr int kWorkspacePerfDetailMax = 2;

// ---- File probing ----------------------------------------------------------
// Presence includes empty files. An existence/read error (probeError set)
// means protected existing state, never absence.

struct WorkspaceFileProbe
{
    bool exists = false;    // confirmed present as a regular file
    bool readable = false;  // confirmed openable for reading
    bool empty = false;     // zero bytes (meaningful only if readable)
    Error probeError;       // typed error when presence could not be decided
};

WorkspaceFileProbe ProbeWorkspaceFile(const std::filesystem::path& path);

// ---- Startup classification ------------------------------------------------
// A profile is new only when all three paths are confirmed absent: user ini
// (U), portable ini (P), and view_config (V). Any probe error forces the
// protected path, never the "absent" column.

enum class WorkspaceGeometrySource
{
    NewDefault,      // no ini anywhere: session default (package-2 builder later)
    UserIni,         // readable existing user ini (never auto-rebuilt)
    PortableSeed,    // no user ini; readable executable-directory seed
    SessionDefault,  // protected: existing user ini is empty/unreadable
};

enum class WorkspaceVisibilityBasis
{
    NewProfile,  // no view_config anywhere: new-profile defaults
    Legacy,      // view_config present (or protected): legacy member defaults
                 // for omitted keys, valid stored keys applied on top
};

struct WorkspaceStartupClassification
{
    WorkspaceFileProbe userIni;
    WorkspaceFileProbe portableIni;
    WorkspaceFileProbe viewConfig;
    bool isNewProfile = false;
    WorkspaceGeometrySource geometrySource = WorkspaceGeometrySource::NewDefault;
    WorkspaceVisibilityBasis visibilityBasis = WorkspaceVisibilityBasis::Legacy;
    // True when the corresponding file exists but is empty or unreadable and
    // must not be overwritten except through the guarded Reset path.
    bool userIniProtected = false;
    bool portableSeedProtected = false;
    bool viewConfigProtected = false;
};

// ---- Visibility ------------------------------------------------------------

struct WorkspaceVisibility
{
    int perfDetail = kWorkspacePerfDetailMin;
    bool showCamera = true;
    bool showPerformance = true;
    bool showRenderSettings = true;
    bool showScene = true;
    bool showSession = true;
    bool showInputBindings = false;
    bool showContentBrowser = false;
    bool showInspector = true;
    bool showOutliner = true;

    bool operator==(const WorkspaceVisibility& other) const;
    bool operator!=(const WorkspaceVisibility& other) const { return !(*this == other); }

    // Legacy member defaults: the RT2Layer initializers at the grounding
    // commit (camera/perf/render/scene/session/inspector/outliner on;
    // input bindings/content browser off). Applied for omitted keys on
    // existing profiles.
    static WorkspaceVisibility LegacyDefaults() { return WorkspaceVisibility{}; }
    // New-profile defaults (accepted plan, with one interim migration
    // exception noted below): Outliner, Inspector, Content Browser,
    // Session, Performance on; Camera, Render Settings, Input Bindings off.
    // Viewport has no flag (always shown).
    //
    // INTERIM (persistence package): showScene stays ON for genuinely new
    // profiles so transport/environment remain discoverable until the
    // package-3 persistent toolbar ships; the toolbar package flips Scene
    // to the final off default. Existing profiles are unaffected (legacy
    // member defaults for omitted keys either way).
    static WorkspaceVisibility NewProfileDefaults();
};

struct WorkspaceViewConfigParse
{
    WorkspaceVisibility visibility;
    // Unknown key lines retained verbatim (without line terminator) in file
    // order so forward-compatible keys survive a rewrite. Blank lines are
    // dropped; lines without '=' are retained verbatim.
    std::vector<std::string> unknownLines;
    bool hadMalformedKnownValue = false;
};

WorkspaceViewConfigParse ParseWorkspaceViewConfig(
    const std::string& bytes, const WorkspaceVisibility& base);
std::string SerializeWorkspaceViewConfig(
    const WorkspaceVisibility& visibility,
    const std::vector<std::string>& unknownLines);

// ---- Checked file IO -------------------------------------------------------
// Small preference-file writer (the prefab PathTransaction protocol is
// deliberately not a dependency). All writers keep the old destination
// intact on failure and report typed read/directory/write/flush/replacement
// errors.

bool ReadFileBytesChecked(
    const std::filesystem::path& path, std::string& outBytes, Error& err);
bool WriteFileBytesCheckedAtomic(
    const std::filesystem::path& path, const std::string& bytes, Error& err);
bool BackupFileChecked(
    const std::filesystem::path& path, std::filesystem::path& backupPath, Error& err);

// ---- Durable session status ------------------------------------------------

enum class WorkspaceFileState
{
    Unknown,              // not yet classified this session
    Loaded,               // successfully loaded (or created-new) profile
    NewDefault,           // confirmed-absent source; session default in use
    SessionOnlyProtected, // existing file is empty/unreadable; session-only
                          // state, persistence visibly unavailable
    UnsavedChanges,       // snapshot differs from last successful write
    SaveFailed,           // last write failed; original intact, retry pending
    LoadFailed,           // last (re)load failed; session/original untouched
};

struct WorkspaceFileStatus
{
    WorkspaceFileState state = WorkspaceFileState::Unknown;
    Error lastError;  // last typed failure for this file (sticky until success)
    std::filesystem::path backupPath;  // set once a checked backup succeeds
    bool backupDone = false;
    int failureCount = 0;  // coalesced repeats of the same failure
    std::chrono::steady_clock::time_point nextRetryAt{};
};

struct WorkspacePersistenceStatus
{
    WorkspaceFileStatus geometry;    // imgui.ini side
    WorkspaceFileStatus visibility;  // view_config.txt side
    bool headless = false;

    bool HasUnsaved() const;
    // Exactly one side saved while the other still needs writing.
    bool PartiallySaved() const;
    const char* SummaryMessage() const;
};

// ---- Workspace preference state --------------------------------------------

class EditorWorkspaceState
{
public:
    using Clock = std::chrono::steady_clock;

    explicit EditorWorkspaceState(
        std::filesystem::path appDataRoot,
        std::filesystem::path executableDir = {});

    void SetHeadless(bool headless);
    bool IsHeadless() const { return m_Headless; }

    std::filesystem::path UserIniPath() const;
    std::filesystem::path PortableIniPath() const;
    std::filesystem::path ViewConfigPath() const;
    static std::filesystem::path BackupPathFor(const std::filesystem::path& path);

    // Probe U/P/V, decide new-vs-existing, and seed the durable status.
    // Reconstructs protected state from file checks on every call, so a
    // Retry Load path can re-run it after access is repaired.
    WorkspaceStartupClassification ClassifyStartup();

    struct GeometryLoad
    {
        WorkspaceGeometrySource source = WorkspaceGeometrySource::NewDefault;
        std::string bytes;  // empty when there is nothing loadable
        Error error;
        bool IsOk() const { return error.IsOk(); }
    };
    // Checked read of the classified geometry source. Updates geometry
    // status (Loaded / NewDefault / SessionOnlyProtected). Never falls back
    // to the portable seed when the user ini is protected.
    GeometryLoad LoadGeometryBytes();

    struct VisibilityLoad
    {
        WorkspaceVisibility visibility = WorkspaceVisibility::LegacyDefaults();
        std::vector<std::string> unknownLines;
        Error error;
        bool IsOk() const { return error.IsOk(); }
    };
    // Checked read+parse of view_config.txt over `base` defaults. Unknown
    // keys are retained; malformed known values keep the base value and are
    // reported through the parse flag + visibility status (never silent).
    VisibilityLoad LoadVisibility(const WorkspaceVisibility& base);

    // Snapshot compare drivers. The host calls these when its live flags or
    // ini bytes change (close buttons mutate flags directly, so compare —
    // don't hook — every frame).
    void MarkVisibilitySnapshot(const WorkspaceVisibility& visibility,
        const std::vector<std::string>& unknownLines, Clock::time_point now);
    void MarkGeometryDirty(Clock::time_point now);
    void MarkGeometrySnapshot(const std::string& iniBytes, Clock::time_point now);

    bool VisibilityDirty() const { return m_VisibilityDirtySince.has_value(); }
    bool GeometryDirty() const { return m_GeometryDirtySince.has_value(); }
    // Automatic-save gates: dirty, past the debounce interval, and past the
    // bounded retry delay after a failure. The host calls these per frame
    // and passes force=false; explicit Retry/Reset/flush pass force=true.
    bool VisibilitySaveDue(Clock::time_point now) const;
    bool GeometrySaveDue(Clock::time_point now) const;

    // Checked saves. Headless: checked no-op success (no bytes rewritten).
    // `force` bypasses debounce AND the retry delay (explicit Retry/Reset/
    // exit flush); automatic per-frame saves pass force=false. A save of a
    // protected file is refused loudly unless a Reset has cleared the
    // protection. First rewrite of a file that had content takes a checked
    // backup first; backup failure blocks the save and reports path/error.
    bool SaveVisibility(Clock::time_point now, bool force, Error& err);
    bool SaveGeometry(Clock::time_point now, bool force, const std::string& iniBytes, Error& err);
    // Force-save every dirty part. Returns true only when both requested
    // writes succeed; on a partial save the status reads "workspace changes
    // not fully saved" and only unsaved parts are retried later.
    // `geometryBytesOrNull` supplies fresh SaveIniSettingsToMemory output
    // when geometry is dirty; dirty geometry with null bytes is a loud
    // InvalidArgument, never a silent skip.
    bool Flush(Clock::time_point now,
        const WorkspaceVisibility& visibility,
        const std::vector<std::string>& unknownLines,
        const std::string* geometryBytesOrNull, Error& err);

    // Re-probe and re-read both files (Retry Load). On success the protected
    // session-only state is replaced by the file state and dirty snapshots
    // are cleared (the host discards session geometry after applying the
    // bytes — with a confirmation while its own edits are unsaved, owned by
    // the UI layer). On failure the session and originals are untouched.
    bool ReloadAll(Clock::time_point now, Error& err);

    // Guarded reset: checked backups of both existing files must succeed
    // first (an unreadable file fails its backup, so Reset stays unavailable
    // until access is repaired). Then visibility becomes new-profile
    // defaults (unknown lines retained) and both files are force-saved
    // without debounce. `geometryBytes` is fresh SaveIniSettingsToMemory
    // output supplied by the host: the file is brought into agreement with
    // the session, never cleared behind ImGui's back (1.87 exposes no
    // public call to drop loaded settings, so a file/memory divergence
    // would be silently healed by the next save). The package-2 default
    // builder upgrades what Reset installs geometrically.
    // Never touches the portable seed.
    bool ResetToDefaults(Clock::time_point now, const std::string& geometryBytes, Error& err);
    bool CanReset(Error& err) const;
    // Bounded visibility-only reset: the accurately named action the UI
    // offers in the persistence package. Checked backup of view_config must
    // succeed first; then visibility becomes new-profile defaults (unknown
    // lines harvested from the current file and retained) and the file is
    // force-saved without debounce. Geometry files are never touched, and
    // geometry protection is never cleared. The full two-file Reset Layout
    // (geometry rebuild) waits for the package-2 default builder; its
    // contract is NOT complete in this package.
    bool ResetVisibilityToDefaults(Clock::time_point now, Error& err);
    bool CanResetVisibility(Error& err) const;

    // Restore a file from its readable paired backup through the same
    // checked replacement + error handling. The caller reloads afterwards.
    bool RestoreBackupGeometry(Error& err);
    bool RestoreBackupVisibility(Error& err);

    const WorkspacePersistenceStatus& Status() const { return m_Status; }
    const WorkspaceStartupClassification& Classification() const { return m_Classification; }
    const WorkspaceVisibility& LastSavedVisibility() const { return m_LastSavedVisibility; }
    const std::vector<std::string>& LastSavedUnknownLines() const { return m_LastSavedUnknownLines; }
    const std::string& LastSavedGeometry() const { return m_LastSavedGeometry; }
    bool ResetPerformed() const { return m_ResetPerformed; }

private:
    bool CanSaveGeometry(Error& err) const;
    bool CanSaveVisibility(Error& err) const;
    bool EnsureBackup(const std::filesystem::path& path, WorkspaceFileStatus& status, Error& err);
    static bool RetryDue(const WorkspaceFileStatus& status, Clock::time_point now);
    static void NoteFailure(WorkspaceFileStatus& status, const Error& err, Clock::time_point now);
    static void NoteSuccess(WorkspaceFileStatus& status);

    std::filesystem::path m_AppDataRoot;
    std::filesystem::path m_ExecutableDir;
    bool m_Headless = false;
    WorkspaceStartupClassification m_Classification;
    bool m_Classified = false;
    WorkspacePersistenceStatus m_Status;
    WorkspaceVisibility m_PendingVisibility = WorkspaceVisibility::LegacyDefaults();
    std::vector<std::string> m_PendingUnknownLines;
    WorkspaceVisibility m_LastSavedVisibility = WorkspaceVisibility::LegacyDefaults();
    std::vector<std::string> m_LastSavedUnknownLines;
    std::string m_LastSavedGeometry;
    std::optional<Clock::time_point> m_VisibilityDirtySince;
    std::optional<Clock::time_point> m_GeometryDirtySince;
    bool m_ResetPerformed = false;
};

} // namespace rt2::core

#endif // RT2_CORE_EDITOR_WORKSPACE_STATE_H
