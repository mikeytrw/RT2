#include "EditorWorkspaceState.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#ifdef _WIN32
#  include <windows.h>
#endif

namespace rt2::core {

namespace {

bool SameError(const Error& a, const Error& b) { return a == b; }

std::string TrimAscii(const std::string& value)
{
    size_t begin = 0;
    while (begin < value.size() &&
           (value[begin] == ' ' || value[begin] == '\t' || value[begin] == '\r'))
        ++begin;
    size_t end = value.size();
    while (end > begin &&
           (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == '\r'))
        --end;
    return value.substr(begin, end - begin);
}

bool ParseBoolValue(const std::string& raw, bool& out)
{
    const std::string value = TrimAscii(raw);
    if (value == "1" || value == "true") { out = true; return true; }
    if (value == "0" || value == "false") { out = false; return true; }
    return false;
}

bool ParseIntValue(const std::string& raw, int& out)
{
    const std::string value = TrimAscii(raw);
    if (value.empty()) return false;
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0') return false;
    out = static_cast<int>(parsed);
    return true;
}

} // anonymous namespace

// ---- File probing ----------------------------------------------------------

WorkspaceFileProbe ProbeWorkspaceFile(const std::filesystem::path& path)
{
    WorkspaceFileProbe probe;
    if (path.empty()) return probe;
    std::error_code ec;
    // Order matters: a missing file reports ENOENT through the status
    // query itself, which must read as "absent", not as an error. Only a
    // failed existence check (e.g. a denied parent directory) is a probe
    // error. Anything that exists but is not a readable regular file —
    // directory at the path, denied read — is protected existing state.
    const bool exists = std::filesystem::exists(path, ec);
    if (ec)
    {
        probe.probeError.code = Error::Io;
        probe.probeError.path = path.u8string();
        probe.probeError.detail = "failed to check workspace file presence: " + ec.message();
        return probe;
    }
    if (!exists) return probe;
    probe.exists = true;
    const bool regular = std::filesystem::is_regular_file(path, ec);
    if (ec)
    {
        probe.probeError.code = Error::Io;
        probe.probeError.path = path.u8string();
        probe.probeError.detail = "failed to stat workspace file: " + ec.message();
        return probe;
    }
    if (!regular) return probe; // exists, not readable as a file: protected
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        return probe; // exists, unreadable: protected (no typed cause known)
    }
    in.seekg(0, std::ios::end);
    probe.readable = true;
    probe.empty = (in.tellg() == 0);
    return probe;
}

// ---- Visibility ------------------------------------------------------------

bool WorkspaceVisibility::operator==(const WorkspaceVisibility& other) const
{
    return perfDetail == other.perfDetail &&
           showCamera == other.showCamera &&
           showPerformance == other.showPerformance &&
           showRenderSettings == other.showRenderSettings &&
           showScene == other.showScene &&
           showSession == other.showSession &&
           showInputBindings == other.showInputBindings &&
           showContentBrowser == other.showContentBrowser &&
           showInspector == other.showInspector &&
           showOutliner == other.showOutliner;
}

WorkspaceVisibility WorkspaceVisibility::NewProfileDefaults()
{
    WorkspaceVisibility v;
    v.perfDetail = kWorkspacePerfDetailMin;
    v.showCamera = false;
    v.showPerformance = true;
    v.showRenderSettings = false;
    v.showScene = true; // INTERIM migration policy (see header): final is off.
    v.showSession = true;
    v.showInputBindings = false;
    v.showContentBrowser = true;
    v.showInspector = true;
    v.showOutliner = true;
    return v;
}

WorkspaceViewConfigParse ParseWorkspaceViewConfig(
    const std::string& bytes, const WorkspaceVisibility& base)
{
    WorkspaceViewConfigParse parsed;
    parsed.visibility = base;
    WorkspaceVisibility& v = parsed.visibility;
    size_t pos = 0;
    while (pos <= bytes.size())
    {
        size_t end = bytes.find('\n', pos);
        if (end == std::string::npos) end = bytes.size();
        std::string line = bytes.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue; // blank lines are dropped, not retained
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
        {
            parsed.unknownLines.push_back(line); // retained verbatim
            continue;
        }
        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);
        bool* flag = nullptr;
        if (key == "showCamera") flag = &v.showCamera;
        else if (key == "showPerformance") flag = &v.showPerformance;
        else if (key == "showRenderSettings") flag = &v.showRenderSettings;
        else if (key == "showScene") flag = &v.showScene;
        else if (key == "showSession") flag = &v.showSession;
        else if (key == "showInputBindings") flag = &v.showInputBindings;
        else if (key == "showContentBrowser") flag = &v.showContentBrowser;
        else if (key == "showInspector") flag = &v.showInspector;
        else if (key == "showOutliner") flag = &v.showOutliner;
        else if (key == "perfDetail")
        {
            int level = 0;
            if (!ParseIntValue(val, level) ||
                level < kWorkspacePerfDetailMin || level > kWorkspacePerfDetailMax)
                parsed.hadMalformedKnownValue = true;
            else
                v.perfDetail = level;
            continue;
        }
        else
        {
            parsed.unknownLines.push_back(line); // retained verbatim
            continue;
        }
        bool b = false;
        if (!ParseBoolValue(val, b))
            parsed.hadMalformedKnownValue = true; // keep the base value, loudly
        else
            *flag = b;
    }
    return parsed;
}

std::string SerializeWorkspaceViewConfig(
    const WorkspaceVisibility& visibility,
    const std::vector<std::string>& unknownLines)
{
    std::ostringstream out;
    out << "perfDetail=" << visibility.perfDetail << "\n";
    out << "showCamera=" << (visibility.showCamera ? 1 : 0) << "\n";
    out << "showPerformance=" << (visibility.showPerformance ? 1 : 0) << "\n";
    out << "showRenderSettings=" << (visibility.showRenderSettings ? 1 : 0) << "\n";
    out << "showScene=" << (visibility.showScene ? 1 : 0) << "\n";
    out << "showSession=" << (visibility.showSession ? 1 : 0) << "\n";
    out << "showInspector=" << (visibility.showInspector ? 1 : 0) << "\n";
    out << "showOutliner=" << (visibility.showOutliner ? 1 : 0) << "\n";
    out << "showInputBindings=" << (visibility.showInputBindings ? 1 : 0) << "\n";
    out << "showContentBrowser=" << (visibility.showContentBrowser ? 1 : 0) << "\n";
    for (const auto& line : unknownLines)
    {
        if (!line.empty()) out << line << "\n";
    }
    return out.str();
}

// ---- Checked file IO --------------------------------------------------------

bool ReadFileBytesChecked(
    const std::filesystem::path& path, std::string& outBytes, Error& err)
{
    err = Error{};
    outBytes.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        err.code = Error::Io;
        err.path = path.u8string();
        err.detail = "failed to open workspace file for reading";
        return false;
    }
    std::ostringstream stream;
    stream << in.rdbuf();
    if (in.bad())
    {
        err.code = Error::Io;
        err.path = path.u8string();
        err.detail = "failed while reading workspace file";
        return false;
    }
    outBytes = stream.str();
    return true;
}

bool WriteFileBytesCheckedAtomic(
    const std::filesystem::path& path, const std::string& bytes, Error& err)
{
    err = Error{};
    // Mirror EditorSettingsStore::Save: create the directory, write a temp
    // file, flush, then atomically replace. The old destination is never
    // truncated and stays intact on every failure below.
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
    {
        err.code = Error::Io;
        err.path = path.parent_path().u8string();
        err.detail = "failed to create workspace directory: " + ec.message();
        return false;
    }
    auto temp = path;
    temp += ".tmp";
    {
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            err.code = Error::Io;
            err.path = temp.u8string();
            err.detail = "failed to open workspace temp file for writing";
            return false;
        }
        if (!bytes.empty())
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output)
        {
            output.close();
            std::filesystem::remove(temp, ec);
            err.code = Error::Io;
            err.path = temp.u8string();
            err.detail = "failed while writing workspace temp file";
            return false;
        }
    }
#ifdef _WIN32
    const std::wstring target = path.wstring();
    const std::wstring source = temp.wstring();
    if (std::filesystem::exists(path))
    {
        if (!ReplaceFileW(target.c_str(), source.c_str(), nullptr,
                          REPLACEFILE_WRITE_THROUGH, nullptr, nullptr) &&
            !MoveFileExW(source.c_str(), target.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            std::filesystem::remove(temp, ec);
            err.code = Error::Io;
            err.path = path.u8string();
            err.detail = "failed to atomically replace workspace file";
            return false;
        }
    }
    else if (!MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH))
    {
        std::filesystem::remove(temp, ec);
        err.code = Error::Io;
        err.path = path.u8string();
        err.detail = "failed to atomically create workspace file";
        return false;
    }
#else
    std::filesystem::rename(temp, path, ec);
    if (ec)
    {
        std::filesystem::remove(temp, ec);
        err.code = Error::Io;
        err.path = path.u8string();
        err.detail = "failed to rename workspace temp file: " + ec.message();
        return false;
    }
#endif
    return true;
}

bool BackupFileChecked(
    const std::filesystem::path& path, std::filesystem::path& backupPath, Error& err)
{
    err = Error{};
    backupPath = EditorWorkspaceState::BackupPathFor(path);
    // Read first: an unreadable file fails its backup, which is exactly the
    // gate that keeps Reset unavailable until access is repaired.
    std::string original;
    if (!ReadFileBytesChecked(path, original, err)) return false;
    if (!WriteFileBytesCheckedAtomic(backupPath, original, err)) return false;
    return true;
}

// ---- Durable session status -------------------------------------------------

bool WorkspacePersistenceStatus::HasUnsaved() const
{
    return geometry.state == WorkspaceFileState::UnsavedChanges ||
           geometry.state == WorkspaceFileState::SaveFailed ||
           visibility.state == WorkspaceFileState::UnsavedChanges ||
           visibility.state == WorkspaceFileState::SaveFailed;
}

bool WorkspacePersistenceStatus::PartiallySaved() const
{
    const bool geometryNeeds =
        geometry.state == WorkspaceFileState::UnsavedChanges ||
        geometry.state == WorkspaceFileState::SaveFailed;
    const bool visibilityNeeds =
        visibility.state == WorkspaceFileState::UnsavedChanges ||
        visibility.state == WorkspaceFileState::SaveFailed;
    return geometryNeeds != visibilityNeeds;
}

const char* WorkspacePersistenceStatus::SummaryMessage() const
{
    if (headless) return "Workspace persistence disabled (headless)";
    if (PartiallySaved()) return "workspace changes not fully saved";
    if (HasUnsaved()) return "Workspace changes not saved";
    const bool unavailable =
        geometry.state == WorkspaceFileState::SessionOnlyProtected ||
        visibility.state == WorkspaceFileState::SessionOnlyProtected;
    if (unavailable) return "Workspace persistence unavailable";
    return "Workspace settings saved";
}

// ---- Workspace preference state ---------------------------------------------

EditorWorkspaceState::EditorWorkspaceState(
    std::filesystem::path appDataRoot, std::filesystem::path executableDir)
    : m_AppDataRoot(std::move(appDataRoot))
    , m_ExecutableDir(std::move(executableDir))
{
}

void EditorWorkspaceState::SetHeadless(bool headless)
{
    m_Headless = headless;
    m_Status.headless = headless;
}

std::filesystem::path EditorWorkspaceState::UserIniPath() const
{
    return m_AppDataRoot / kWorkspaceIniFileName;
}

std::filesystem::path EditorWorkspaceState::PortableIniPath() const
{
    if (m_ExecutableDir.empty()) return {};
    return m_ExecutableDir / kWorkspaceIniFileName;
}

std::filesystem::path EditorWorkspaceState::ViewConfigPath() const
{
    return m_AppDataRoot / kWorkspaceViewConfigFileName;
}

std::filesystem::path EditorWorkspaceState::BackupPathFor(const std::filesystem::path& path)
{
    auto backup = path;
    backup += kWorkspaceBackupSuffix;
    return backup;
}

WorkspaceStartupClassification EditorWorkspaceState::ClassifyStartup()
{
    WorkspaceStartupClassification c;
    c.userIni = ProbeWorkspaceFile(UserIniPath());
    const auto portable = PortableIniPath();
    c.portableIni = portable.empty() ? WorkspaceFileProbe{} : ProbeWorkspaceFile(portable);
    c.viewConfig = ProbeWorkspaceFile(ViewConfigPath());

    const auto confirmedAbsent = [](const WorkspaceFileProbe& p) {
        return !p.exists && p.probeError.IsOk();
    };
    c.isNewProfile =
        confirmedAbsent(c.userIni) && confirmedAbsent(c.portableIni) &&
        confirmedAbsent(c.viewConfig);

    // The user ini is protected unless it holds readable nonempty content:
    // empty and unreadable files (and undecidable probes) must never be
    // overwritten except through the guarded Reset path, and a protected
    // user ini never falls back to the portable seed.
    c.userIniProtected =
        (c.userIni.exists && (!c.userIni.readable || c.userIni.empty)) ||
        (!c.userIni.exists && !c.userIni.probeError.IsOk());
    c.portableSeedProtected =
        (c.portableIni.exists && (!c.portableIni.readable || c.portableIni.empty)) ||
        (!c.portableIni.exists && !c.portableIni.probeError.IsOk());
    c.viewConfigProtected =
        (c.viewConfig.exists && !c.viewConfig.readable) ||
        (!c.viewConfig.exists && !c.viewConfig.probeError.IsOk());

    if (c.userIni.exists && c.userIni.readable && !c.userIni.empty)
        c.geometrySource = WorkspaceGeometrySource::UserIni;
    else if (c.userIniProtected && (c.userIni.exists || !c.userIni.probeError.IsOk()))
        c.geometrySource = WorkspaceGeometrySource::SessionDefault;
    else if (c.portableIni.exists && c.portableIni.readable && !c.portableIni.empty)
        c.geometrySource = WorkspaceGeometrySource::PortableSeed;
    else if (!confirmedAbsent(c.portableIni))
        c.geometrySource = WorkspaceGeometrySource::SessionDefault;
    else
        c.geometrySource = WorkspaceGeometrySource::NewDefault;

    c.visibilityBasis = c.isNewProfile
        ? WorkspaceVisibilityBasis::NewProfile
        : WorkspaceVisibilityBasis::Legacy;

    m_Classification = c;
    m_Classified = true;

    // Seed the durable status. Pending loads below promote Unknown to
    // Loaded; protected and new-default states persist for the session
    // until resolution (never transient stdout).
    if (c.geometrySource == WorkspaceGeometrySource::SessionDefault)
    {
        m_Status.geometry.state = WorkspaceFileState::SessionOnlyProtected;
        if (!c.userIni.probeError.IsOk())
            m_Status.geometry.lastError = c.userIni.probeError;
        else if (!confirmedAbsent(c.portableIni) && c.userIniProtected)
            m_Status.geometry.lastError = c.portableIni.probeError;
    }
    else if (c.geometrySource == WorkspaceGeometrySource::NewDefault)
    {
        m_Status.geometry.state = WorkspaceFileState::NewDefault;
    }
    else
    {
        m_Status.geometry.state = WorkspaceFileState::Unknown;
    }
    if (c.viewConfigProtected)
    {
        m_Status.visibility.state = WorkspaceFileState::SessionOnlyProtected;
        m_Status.visibility.lastError = c.viewConfig.probeError;
    }
    else if (c.isNewProfile)
    {
        m_Status.visibility.state = WorkspaceFileState::NewDefault;
    }
    else
    {
        m_Status.visibility.state = WorkspaceFileState::Unknown;
    }
    return c;
}

EditorWorkspaceState::GeometryLoad EditorWorkspaceState::LoadGeometryBytes()
{
    GeometryLoad load;
    if (!m_Classified) ClassifyStartup();
    load.source = m_Classification.geometrySource;
    const std::filesystem::path source =
        (load.source == WorkspaceGeometrySource::PortableSeed)
        ? PortableIniPath() : UserIniPath();
    if (load.source == WorkspaceGeometrySource::NewDefault)
    {
        load.bytes.clear();
        m_LastSavedGeometry.clear();
        m_GeometryDirtySince.reset();
        NoteSuccess(m_Status.geometry);
        m_Status.geometry.state = WorkspaceFileState::NewDefault;
        return load;
    }
    if (load.source == WorkspaceGeometrySource::SessionDefault)
    {
        // Protected: session-only default, original untouched. Synthesize a
        // typed cause when the probe carried none (empty or non-file at the
        // path): protection without a diagnostic would be silent failure.
        load.bytes.clear();
        load.error = m_Status.geometry.lastError;
        if (load.error.IsOk())
        {
            load.error.code = Error::Io;
            load.error.path = UserIniPath().u8string();
            load.error.detail = "workspace layout file exists but is empty or "
                                "unreadable; session-only default in use";
            m_Status.geometry.lastError = load.error;
        }
        m_Status.geometry.state = WorkspaceFileState::SessionOnlyProtected;
        return load;
    }
    if (!ReadFileBytesChecked(source, load.bytes, load.error))
    {
        // Raced between probe and read (or the seed vanished): protect,
        // keep the session and original untouched.
        m_Status.geometry.state = WorkspaceFileState::SessionOnlyProtected;
        NoteFailure(m_Status.geometry, load.error, Clock::now());
        load.bytes.clear();
        return load;
    }
    m_LastSavedGeometry = load.bytes;
    m_GeometryDirtySince.reset();
    NoteSuccess(m_Status.geometry);
    m_Status.geometry.state = WorkspaceFileState::Loaded;
    return load;
}

EditorWorkspaceState::VisibilityLoad EditorWorkspaceState::LoadVisibility(
    const WorkspaceVisibility& base)
{
    VisibilityLoad load;
    load.visibility = base;
    if (!m_Classified) ClassifyStartup();
    if (m_Classification.viewConfigProtected)
    {
        load.error = m_Status.visibility.lastError;
        if (load.error.IsOk())
        {
            load.error.code = Error::Io;
            load.error.path = ViewConfigPath().u8string();
            load.error.detail = "view_config exists but is unreadable; "
                                "session-only visibility in use";
            m_Status.visibility.lastError = load.error;
        }
        m_Status.visibility.state = WorkspaceFileState::SessionOnlyProtected;
        m_PendingVisibility = base;
        m_PendingUnknownLines.clear();
        return load;
    }
    if (!m_Classification.viewConfig.exists)
    {
        // Confirmed absent: base defaults stand in; the first save creates
        // the file (with a backup only if content appears meanwhile).
        m_LastSavedVisibility = base;
        m_LastSavedUnknownLines.clear();
        m_PendingVisibility = base;
        m_PendingUnknownLines.clear();
        m_VisibilityDirtySince.reset();
        NoteSuccess(m_Status.visibility);
        m_Status.visibility.state = m_Classification.isNewProfile
            ? WorkspaceFileState::NewDefault
            : WorkspaceFileState::Loaded;
        return load;
    }
    std::string bytes;
    if (!ReadFileBytesChecked(ViewConfigPath(), bytes, load.error))
    {
        m_Status.visibility.state = WorkspaceFileState::SessionOnlyProtected;
        NoteFailure(m_Status.visibility, load.error, Clock::now());
        m_PendingVisibility = base;
        m_PendingUnknownLines.clear();
        return load;
    }
    // Readable empty view_config uses the base (legacy or new-profile)
    // defaults; the empty original is backed up before rewrite.
    const auto parsed = ParseWorkspaceViewConfig(bytes, base);
    load.visibility = parsed.visibility;
    load.unknownLines = parsed.unknownLines;
    m_LastSavedVisibility = parsed.visibility;
    m_LastSavedUnknownLines = parsed.unknownLines;
    m_PendingVisibility = parsed.visibility;
    m_PendingUnknownLines = parsed.unknownLines;
    m_VisibilityDirtySince.reset();
    NoteSuccess(m_Status.visibility);
    m_Status.visibility.state = WorkspaceFileState::Loaded;
    if (parsed.hadMalformedKnownValue)
    {
        // Loud, not silent: keep the base value but say so durably. This is
        // a warning on a Loaded file, not a failure (no retry scheduled).
        m_Status.visibility.lastError.code = Error::Parse;
        m_Status.visibility.lastError.path = ViewConfigPath().u8string();
        m_Status.visibility.lastError.detail =
            "malformed known view_config value(s) ignored; defaults kept";
    }
    return load;
}

void EditorWorkspaceState::MarkVisibilitySnapshot(
    const WorkspaceVisibility& visibility,
    const std::vector<std::string>& unknownLines, Clock::time_point now)
{
    const bool differs = visibility != m_LastSavedVisibility ||
                         unknownLines != m_LastSavedUnknownLines;
    m_PendingVisibility = visibility;
    m_PendingUnknownLines = unknownLines;
    if (differs)
    {
        if (!m_VisibilityDirtySince) m_VisibilityDirtySince = now;
        if (m_Status.visibility.state != WorkspaceFileState::SaveFailed)
            m_Status.visibility.state = WorkspaceFileState::UnsavedChanges;
    }
    else
    {
        m_VisibilityDirtySince.reset();
        if (m_Status.visibility.state == WorkspaceFileState::UnsavedChanges)
            m_Status.visibility.state = WorkspaceFileState::Loaded;
    }
}

void EditorWorkspaceState::MarkGeometryDirty(Clock::time_point now)
{
    if (!m_GeometryDirtySince) m_GeometryDirtySince = now;
    if (m_Status.geometry.state != WorkspaceFileState::SaveFailed)
        m_Status.geometry.state = WorkspaceFileState::UnsavedChanges;
}

bool EditorWorkspaceState::VisibilitySaveDue(Clock::time_point now) const
{
    if (!m_VisibilityDirtySince) return false;
    if (*m_VisibilityDirtySince + std::chrono::duration<double>(kWorkspaceSaveDebounceSeconds) > now)
        return false;
    return RetryDue(m_Status.visibility, now);
}

bool EditorWorkspaceState::GeometrySaveDue(Clock::time_point now) const
{
    if (!m_GeometryDirtySince) return false;
    if (*m_GeometryDirtySince + std::chrono::duration<double>(kWorkspaceSaveDebounceSeconds) > now)
        return false;
    return RetryDue(m_Status.geometry, now);
}

bool EditorWorkspaceState::CanSaveGeometry(Error& err) const
{
    err = Error{};
    if (m_Classification.userIniProtected && !m_ResetPerformed)
    {
        err.code = Error::Io;
        err.path = UserIniPath().u8string();
        err.detail = "workspace geometry save refused: user layout file is "
                     "empty or unreadable; repair access or Reset Layout";
        return false;
    }
    return true;
}

bool EditorWorkspaceState::CanSaveVisibility(Error& err) const
{
    err = Error{};
    if (m_Classification.viewConfigProtected && !m_ResetPerformed)
    {
        err.code = Error::Io;
        err.path = ViewConfigPath().u8string();
        err.detail = "workspace visibility save refused: view_config is "
                     "unreadable; repair access or Reset Layout";
        return false;
    }
    return true;
}

bool EditorWorkspaceState::EnsureBackup(
    const std::filesystem::path& path, WorkspaceFileStatus& status, Error& err)
{
    err = Error{};
    if (status.backupDone) return true;
    const auto probe = ProbeWorkspaceFile(path);
    if (!probe.exists)
    {
        if (!probe.probeError.IsOk())
        {
            err = probe.probeError;
            return false;
        }
        status.backupDone = true;
        status.backupPath.clear();
        return true;
    }
    // The file exists — including zero-byte originals, which materialize as
    // empty .bak files so post-Reset recovery is uniform. An unreadable file
    // fails here, which is the gate that blocks Reset until access is
    // repaired.
    std::filesystem::path backup;
    if (!BackupFileChecked(path, backup, err)) return false;
    status.backupDone = true;
    status.backupPath = backup;
    return true;
}

bool EditorWorkspaceState::RetryDue(const WorkspaceFileStatus& status, Clock::time_point now)
{
    if (status.failureCount == 0) return true;
    return now >= status.nextRetryAt;
}

void EditorWorkspaceState::NoteFailure(
    WorkspaceFileStatus& status, const Error& err, Clock::time_point now)
{
    if (SameError(status.lastError, err) && status.failureCount > 0)
        ++status.failureCount; // coalesce: count, never one error per frame
    else
    {
        status.lastError = err;
        status.failureCount = 1;
    }
    status.nextRetryAt =
        now + std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(kWorkspaceRetryDelaySeconds));
    status.state = WorkspaceFileState::SaveFailed;
}

void EditorWorkspaceState::NoteSuccess(WorkspaceFileStatus& status)
{
    status.lastError = Error{};
    status.failureCount = 0;
    status.nextRetryAt = Clock::time_point{};
}

bool EditorWorkspaceState::SaveVisibility(Clock::time_point now, bool force, Error& err)
{
    err = Error{};
    if (m_Headless)
    {
        // Checked no-op: headless operation must not rewrite interactive
        // preferences, but the in-memory snapshot still advances so the
        // session stays consistent.
        m_LastSavedVisibility = m_PendingVisibility;
        m_LastSavedUnknownLines = m_PendingUnknownLines;
        m_VisibilityDirtySince.reset();
        return true;
    }
    if (!m_VisibilityDirtySince && !force) return true;
    if (!force && !RetryDue(m_Status.visibility, now)) return true;
    if (!CanSaveVisibility(err))
    {
        NoteFailure(m_Status.visibility, err, now);
        return false;
    }
    const auto path = ViewConfigPath();
    if (!EnsureBackup(path, m_Status.visibility, err))
    {
        NoteFailure(m_Status.visibility, err, now);
        return false;
    }
    const std::string bytes =
        SerializeWorkspaceViewConfig(m_PendingVisibility, m_PendingUnknownLines);
    if (!WriteFileBytesCheckedAtomic(path, bytes, err))
    {
        NoteFailure(m_Status.visibility, err, now);
        return false;
    }
    m_LastSavedVisibility = m_PendingVisibility;
    m_LastSavedUnknownLines = m_PendingUnknownLines;
    m_VisibilityDirtySince.reset();
    NoteSuccess(m_Status.visibility);
    m_Status.visibility.state = WorkspaceFileState::Loaded;
    return true;
}

bool EditorWorkspaceState::SaveGeometry(
    Clock::time_point now, bool force, const std::string& iniBytes, Error& err)
{
    err = Error{};
    if (m_Headless)
    {
        m_LastSavedGeometry = iniBytes;
        m_GeometryDirtySince.reset();
        return true;
    }
    if (!m_GeometryDirtySince && !force) return true;
    if (!force && !RetryDue(m_Status.geometry, now)) return true;
    if (!CanSaveGeometry(err))
    {
        NoteFailure(m_Status.geometry, err, now);
        return false;
    }
    // Subsequent settings always go to the user directory, even when the
    // session was seeded from the portable file. The seed is never written.
    const auto path = UserIniPath();
    if (!EnsureBackup(path, m_Status.geometry, err))
    {
        NoteFailure(m_Status.geometry, err, now);
        return false;
    }
    if (!WriteFileBytesCheckedAtomic(path, iniBytes, err))
    {
        NoteFailure(m_Status.geometry, err, now);
        return false;
    }
    m_LastSavedGeometry = iniBytes;
    m_GeometryDirtySince.reset();
    NoteSuccess(m_Status.geometry);
    m_Status.geometry.state = WorkspaceFileState::Loaded;
    return true;
}

bool EditorWorkspaceState::Flush(
    Clock::time_point now,
    const WorkspaceVisibility& visibility,
    const std::vector<std::string>& unknownLines,
    const std::string* geometryBytesOrNull, Error& err)
{
    err = Error{};
    MarkVisibilitySnapshot(visibility, unknownLines, now);
    bool ok = true;
    Error first;
    if (m_VisibilityDirtySince && !SaveVisibility(now, true, err))
    {
        ok = false;
        first = err;
    }
    if (m_GeometryDirtySince)
    {
        if (geometryBytesOrNull == nullptr)
        {
            Error nullErr;
            nullErr.code = Error::InvalidArgument;
            nullErr.detail = "workspace flush refused: dirty geometry but no ini bytes supplied";
            NoteFailure(m_Status.geometry, nullErr, now);
            if (ok) first = nullErr;
            ok = false;
        }
        else if (!SaveGeometry(now, true, *geometryBytesOrNull, err))
        {
            ok = false;
            if (first.IsOk()) first = err;
        }
    }
    err = first;
    return ok;
}

bool EditorWorkspaceState::ReloadAll(Clock::time_point now, Error& err)
{
    err = Error{};
    (void)now;
    ClassifyStartup();
    const WorkspaceVisibility base =
        m_Classification.visibilityBasis == WorkspaceVisibilityBasis::NewProfile
        ? WorkspaceVisibility::NewProfileDefaults()
        : WorkspaceVisibility::LegacyDefaults();
    const GeometryLoad geometry = LoadGeometryBytes();
    const VisibilityLoad visibility = LoadVisibility(base);
    // Geometry bytes reach ImGui through the caller (which discards
    // session-only geometry after a successful read — confirmed in the UI
    // when its own edits are unsaved). Visibility applies directly.
    m_PendingVisibility = visibility.visibility;
    m_PendingUnknownLines = visibility.unknownLines;
    if (!geometry.IsOk() || !visibility.IsOk())
    {
        err = !geometry.IsOk() ? geometry.error : visibility.error;
        return false;
    }
    return true;
}

bool EditorWorkspaceState::CanReset(Error& err) const
{
    err = Error{};
    // Reset needs checked backups of both existing files to succeed. A
    // file that cannot even be opened for reading fails its backup, so
    // Reset stays unavailable until access is repaired — there is no
    // destructive bypass for unreadable files.
    const auto userProbe = ProbeWorkspaceFile(UserIniPath());
    if (userProbe.exists && !userProbe.readable)
    {
        err.code = Error::Io;
        err.path = UserIniPath().u8string();
        err.detail = "Reset Layout unavailable: user layout file is unreadable; "
                     "repair access, then Retry Load or Reset";
        return false;
    }
    if (!userProbe.probeError.IsOk())
    {
        err = userProbe.probeError;
        return false;
    }
    const auto viewProbe = ProbeWorkspaceFile(ViewConfigPath());
    if (viewProbe.exists && !viewProbe.readable)
    {
        err.code = Error::Io;
        err.path = ViewConfigPath().u8string();
        err.detail = "Reset Layout unavailable: view_config is unreadable; "
                     "repair access, then Retry Load or Reset";
        return false;
    }
    if (!viewProbe.probeError.IsOk())
    {
        err = viewProbe.probeError;
        return false;
    }
    return true;
}

bool EditorWorkspaceState::ResetToDefaults(
    Clock::time_point now, const std::string& geometryBytes, Error& err)
{
    err = Error{};
    if (m_Headless)
    {
        err.code = Error::InvalidArgument;
        err.detail = "Reset Layout refused: headless operation never rewrites preferences";
        return false;
    }
    if (!CanReset(err)) return false;
    // Back up both existing files BEFORE changing anything. Either backup
    // failure blocks the reset and reports path/error; nothing is rebuilt.
    if (ProbeWorkspaceFile(UserIniPath()).exists &&
        !EnsureBackup(UserIniPath(), m_Status.geometry, err))
    {
        NoteFailure(m_Status.geometry, err, now);
        return false;
    }
    if (ProbeWorkspaceFile(ViewConfigPath()).exists &&
        !EnsureBackup(ViewConfigPath(), m_Status.visibility, err))
    {
        NoteFailure(m_Status.visibility, err, now);
        return false;
    }
    // Install defaults. Unknown view_config lines are harvested from the
    // current file (best effort) and retained: forward data is never
    // dropped by a reset, even when the host never loaded it this session.
    // Geometry takes the host's fresh session bytes as the new baseline
    // (see header). The portable seed is never touched.
    m_ResetPerformed = true;
    m_Classification.userIniProtected = false;
    m_Classification.viewConfigProtected = false;
    {
        std::string current;
        Error readErr;
        if (ReadFileBytesChecked(ViewConfigPath(), current, readErr))
            m_PendingUnknownLines =
                ParseWorkspaceViewConfig(
                    current, WorkspaceVisibility::NewProfileDefaults()).unknownLines;
    }
    m_PendingVisibility = WorkspaceVisibility::NewProfileDefaults();
    m_VisibilityDirtySince = now;
    m_Status.visibility.state = WorkspaceFileState::UnsavedChanges;
    m_GeometryDirtySince = now;
    m_Status.geometry.state = WorkspaceFileState::UnsavedChanges;
    // Forced checked save of BOTH files: no debounce wait. Partial failure
    // keeps both backups and retries only unsaved parts.
    bool ok = true;
    Error first;
    if (!SaveVisibility(now, true, err))
    {
        ok = false;
        first = err;
    }
    if (!SaveGeometry(now, true, geometryBytes, err))
    {
        ok = false;
        if (first.IsOk()) first = err;
    }
    err = first;
    return ok;
}

bool EditorWorkspaceState::ResetVisibilityToDefaults(Clock::time_point now, Error& err)
{
    err = Error{};
    if (m_Headless)
    {
        err.code = Error::InvalidArgument;
        err.detail = "Reset Window Visibility refused: headless operation never rewrites preferences";
        return false;
    }
    if (!CanResetVisibility(err)) return false;
    if (ProbeWorkspaceFile(ViewConfigPath()).exists &&
        !EnsureBackup(ViewConfigPath(), m_Status.visibility, err))
    {
        NoteFailure(m_Status.visibility, err, now);
        return false;
    }
    m_Classification.viewConfigProtected = false;
    {
        std::string current;
        Error readErr;
        if (ReadFileBytesChecked(ViewConfigPath(), current, readErr))
            m_PendingUnknownLines =
                ParseWorkspaceViewConfig(
                    current, WorkspaceVisibility::NewProfileDefaults()).unknownLines;
    }
    m_PendingVisibility = WorkspaceVisibility::NewProfileDefaults();
    m_VisibilityDirtySince = now;
    m_Status.visibility.state = WorkspaceFileState::UnsavedChanges;
    return SaveVisibility(now, true, err);
}

bool EditorWorkspaceState::CanResetVisibility(Error& err) const
{
    err = Error{};
    const auto viewProbe = ProbeWorkspaceFile(ViewConfigPath());
    if (viewProbe.exists && !viewProbe.readable)
    {
        err.code = Error::Io;
        err.path = ViewConfigPath().u8string();
        err.detail = "Reset Window Visibility unavailable: view_config is unreadable; "
                     "repair access, then Retry Load or Reset";
        return false;
    }
    if (!viewProbe.probeError.IsOk())
    {
        err = viewProbe.probeError;
        return false;
    }
    return true;
}

bool EditorWorkspaceState::RestoreBackupGeometry(Error& err)
{
    err = Error{};
    const auto backup = BackupPathFor(UserIniPath());
    std::string bytes;
    if (!ReadFileBytesChecked(backup, bytes, err)) return false;
    if (!WriteFileBytesCheckedAtomic(UserIniPath(), bytes, err)) return false;
    NoteSuccess(m_Status.geometry);
    return true;
}

bool EditorWorkspaceState::RestoreBackupVisibility(Error& err)
{
    err = Error{};
    const auto backup = BackupPathFor(ViewConfigPath());
    std::string bytes;
    if (!ReadFileBytesChecked(backup, bytes, err)) return false;
    if (!WriteFileBytesCheckedAtomic(ViewConfigPath(), bytes, err)) return false;
    NoteSuccess(m_Status.visibility);
    return true;
}

} // namespace rt2::core
