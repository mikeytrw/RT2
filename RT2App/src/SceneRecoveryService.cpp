#include "SceneRecoveryService.h"

#include "SceneSerializer.h"
#include "json.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#  define NOMINMAX
#  include <windows.h>
#endif

using json = nlohmann::json;

namespace rt2::core {
namespace {

std::string FoldWindowsPath(std::string value)
{
#ifdef _WIN32
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(::tolower(c)); });
#endif
    return value;
}

// Stable deterministic key. The full identity remains inside the envelope
// and is validated on read; unlike truncating a path, distinct long prefixes
// do not alias in normal operation.
uint64_t Fnv1a64(const std::string& value)
{
    uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : value)
    {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string HexKey(uint64_t value)
{
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << value;
    return out.str();
}

bool WriteAtomicText(const std::filesystem::path& target,
                     const std::string& content,
                     Error& err)
{
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec)
    {
        err.code = Error::Io;
        err.path = target.parent_path().string();
        err.detail = "failed to create recovery directory: " + ec.message();
        return false;
    }

    std::filesystem::path temp = target;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            err.code = Error::Io;
            err.path = temp.string();
            err.detail = "failed to open recovery temp file";
            return false;
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        if (!out)
        {
            err.code = Error::Io;
            err.path = temp.string();
            err.detail = "failed while writing recovery temp file";
            out.close();
            std::filesystem::remove(temp, ec);
            return false;
        }
    }

#ifdef _WIN32
    std::wstring wTarget = target.wstring();
    std::wstring wTemp = temp.wstring();
    BOOL ok = FALSE;
    if (std::filesystem::exists(target))
    {
        ok = ReplaceFileW(wTarget.c_str(), wTemp.c_str(), nullptr,
                          REPLACEFILE_WRITE_THROUGH, nullptr, nullptr);
        if (!ok)
            ok = MoveFileExW(wTemp.c_str(), wTarget.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
    else
    {
        ok = MoveFileExW(wTemp.c_str(), wTarget.c_str(), MOVEFILE_WRITE_THROUGH);
    }
    if (!ok)
    {
        err.code = Error::Io;
        err.path = target.string();
        err.detail = "failed to atomically replace recovery envelope";
        std::filesystem::remove(temp, ec);
        return false;
    }
#else
    std::filesystem::rename(temp, target, ec);
    if (ec)
    {
        err.code = Error::Io;
        err.path = target.string();
        err.detail = "failed to atomically rename recovery envelope: " + ec.message();
        std::filesystem::remove(temp, ec);
        return false;
    }
#endif
    return true;
}

} // namespace

SceneRecoveryService::SceneRecoveryService(std::filesystem::path recoveryRoot,
                                           ClockNow clock,
                                           size_t maxRecords,
                                           double intervalSeconds)
    : m_RecoveryRoot(std::move(recoveryRoot))
    , m_Clock(std::move(clock))
    , m_MaxRecords(maxRecords)
    , m_IntervalSeconds(intervalSeconds)
{
    if (!m_Clock)
    {
        m_Clock = []() -> int64_t {
            return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        };
    }
}

int64_t SceneRecoveryService::Now() const
{
    return m_NowOverride ? *m_NowOverride : m_Clock();
}

void SceneRecoveryService::ResetSchedule()
{
    m_LastSnapshotTime = 0;
    m_FirstDirtyObservedAt = 0;
    m_HasDirtyObservation = false;
    m_HasWrittenSnapshot = false;
    m_LastWrittenRevision = 0;
    m_ObservedRevision = 0;
}

std::string SceneRecoveryService::DocIdFor(const SceneDocument& doc,
                                           const std::string& untitledRecoveryId)
{
    if (doc.metadata.sourcePath.empty())
        return "untitled:" + (untitledRecoveryId.empty() ? std::string("missing-id")
                                                          : untitledRecoveryId);

    std::error_code ec;
    auto absolute = std::filesystem::weakly_canonical(doc.metadata.sourcePath, ec);
    if (ec) absolute = std::filesystem::absolute(doc.metadata.sourcePath, ec);
    if (ec) absolute = doc.metadata.sourcePath;
    return FoldWindowsPath(absolute.generic_u8string());
}

std::filesystem::path SceneRecoveryService::RecordPath(const std::string& docId) const
{
    return m_RecoveryRoot / (HexKey(Fnv1a64(docId)) + ".rt2recovery");
}

bool SceneRecoveryService::EnsureUntitledRecoveryAssetRoot(
    const std::filesystem::path& localAppData,
    std::filesystem::path& outRoot,
    Error& err)
{
    outRoot.clear();
    err = Error{};
    if (localAppData.empty() || !localAppData.is_absolute())
    {
        err.code = Error::InvalidArgument;
        err.path = localAppData.string();
        err.detail = "LOCALAPPDATA root must be absolute";
        return false;
    }

    const auto root =
        (localAppData / "RT2" / "recovery").lexically_normal();
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec)
    {
        err.code = Error::Io;
        err.path = root.string();
        err.detail = "failed to create untitled recovery asset root: " +
                     ec.message();
        return false;
    }
    outRoot = root;
    return true;
}

bool SceneRecoveryService::MaybeSnapshot(const SceneDocument& doc,
                                         uint64_t currentRevision,
                                         const std::string& untitledRecoveryId,
                                         const std::filesystem::path& logicalAssetRoot,
                                         std::vector<AssetDiagnostic>& diagnostics,
                                         Error& err,
                                         const std::optional<ProjectBinding>& project)
{
    err = Error{};
    if (!doc.metadata.dirty)
    {
        m_FirstDirtyObservedAt = 0;
        m_HasDirtyObservation = false;
        m_ObservedRevision = currentRevision;
        return false;
    }

    const int64_t now = Now();
    if (!m_HasDirtyObservation)
    {
        m_FirstDirtyObservedAt = now;
        m_HasDirtyObservation = true;
        m_ObservedRevision = currentRevision;
        return false;
    }
    if (currentRevision != m_ObservedRevision)
        m_ObservedRevision = currentRevision;

    const int64_t interval = std::max<int64_t>(0, static_cast<int64_t>(m_IntervalSeconds));
    const int64_t referenceTime = m_LastSnapshotTime != 0
                                ? m_LastSnapshotTime : m_FirstDirtyObservedAt;
    if (now - referenceTime < interval)
        return false;
    if (m_HasWrittenSnapshot && currentRevision == m_LastWrittenRevision)
        return false;

    if (doc.metadata.sourcePath.empty() &&
        (logicalAssetRoot.empty() || !logicalAssetRoot.is_absolute()))
    {
        err.code = Error::InvalidArgument;
        err.path = logicalAssetRoot.string();
        err.detail =
            "untitled recovery requires an absolute logical asset root";
        return false;
    }

    const std::string docId = DocIdFor(doc, untitledRecoveryId);
    if (!WriteRecord(
            docId, doc, logicalAssetRoot, currentRevision, now,
            diagnostics, err, project))
        return false;

    const auto keepPath = RecordPath(docId);
    EvictExcess(keepPath);
    CleanupStaleTemporaryFiles();
    m_LastSnapshotTime = now;
    m_LastWrittenRevision = currentRevision;
    m_HasWrittenSnapshot = true;
    return true;
}

bool SceneRecoveryService::WriteRecord(const std::string& docId,
                                       const SceneDocument& doc,
                                       const std::filesystem::path& logicalAssetRoot,
                                       uint64_t revision,
                                       int64_t createdAt,
                                       std::vector<AssetDiagnostic>& diagnostics,
                                       Error& err,
                                       const std::optional<ProjectBinding>& project)
{
    std::error_code ec;
    std::filesystem::create_directories(m_RecoveryRoot, ec);
    if (ec)
    {
        err.code = Error::Io;
        err.path = m_RecoveryRoot.string();
        err.detail = "failed to create recovery root: " + ec.message();
        return false;
    }

    const auto target = RecordPath(docId);
    auto capturePath = target;
    capturePath += ".capture.rt2scene";

    std::filesystem::path assetRoot;
    std::filesystem::path logicalScenePath;
    if (!doc.metadata.sourcePath.empty())
    {
        logicalScenePath = doc.metadata.sourcePath;
        assetRoot = project ? logicalAssetRoot : logicalScenePath.parent_path();
    }
    else
    {
        assetRoot = logicalAssetRoot;
        logicalScenePath = assetRoot / "__untitled__.rt2scene";
    }

    if (!SceneSerializer::SaveTo(
            doc, capturePath, logicalScenePath, diagnostics, err))
        return false;

    json snapshot;
    try
    {
        std::ifstream in(capturePath, std::ios::binary);
        if (!in) throw std::runtime_error("capture file is unreadable");
        in >> snapshot;
    }
    catch (const std::exception& e)
    {
        err.code = Error::Parse;
        err.path = capturePath.string();
        err.detail = std::string("failed to build recovery envelope: ") + e.what();
        std::filesystem::remove(capturePath, ec);
        return false;
    }
    std::filesystem::remove(capturePath, ec);

    json envelope;
    envelope["version"] = ManifestVersion;
    envelope["docId"] = docId;
    envelope["untitled"] = doc.metadata.sourcePath.empty();
    envelope["originalSourcePath"] = doc.metadata.sourcePath.generic_u8string();
    if (!project)
        envelope["assetRoot"] = assetRoot.generic_u8string();
    else
    {
        envelope["projectId"] = project->projectId.ToString();
        envelope["projectFile"] = project->projectFile.generic_u8string();
        envelope["sceneLocator"] = project->sceneLocator;
    }
    envelope["revision"] = revision;
    envelope["createdAt"] = createdAt;
    envelope["snapshot"] = std::move(snapshot);

    return WriteAtomicText(target, envelope.dump(2), err);
}

bool SceneRecoveryService::ParseRecord(const std::filesystem::path& path,
                                       RecoveryRecord& out) const
{
    out = RecoveryRecord{};
    out.recordPath = path;
    json root;
    try
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("recovery envelope is unreadable");
        in >> root;
    }
    catch (const std::exception& e)
    {
        out.diagnostic = std::string("recovery JSON parse: ") + e.what();
        return false;
    }

    if (!root.contains("version") || !root["version"].is_number_unsigned())
    {
        out.diagnostic = "recovery envelope missing version";
        return false;
    }
    out.version = root["version"].get<uint32_t>();
    if (out.version < 2 || out.version > ManifestVersion)
    {
        out.diagnostic = "unsupported recovery version " + std::to_string(out.version);
        return false;
    }
    if (!root.contains("docId") || !root["docId"].is_string() ||
        !root.contains("snapshot") || !root["snapshot"].is_object())
    {
        out.diagnostic = "recovery envelope missing docId or scene snapshot";
        return false;
    }

    out.docId = root["docId"].get<std::string>();
    if (RecordPath(out.docId).filename() != path.filename())
    {
        out.diagnostic = "recovery identity does not match its file name";
        return false;
    }
    out.untitled = root.value("untitled", false);
    if (root.contains("originalSourcePath") && root["originalSourcePath"].is_string())
        out.originalSourcePath = std::filesystem::u8path(root["originalSourcePath"].get<std::string>());
    if (root.contains("assetRoot") && root["assetRoot"].is_string())
        out.assetRoot = std::filesystem::u8path(root["assetRoot"].get<std::string>());
    if (root.contains("projectId") && root["projectId"].is_string())
        out.projectId = UUID::Parse(root["projectId"].get<std::string>());
    if (root.contains("projectFile") && root["projectFile"].is_string())
        out.projectFile = std::filesystem::u8path(
            root["projectFile"].get<std::string>());
    if (root.contains("sceneLocator") && root["sceneLocator"].is_string())
        out.sceneLocator = root["sceneLocator"].get<std::string>();
    if (out.version >= 3 && root.contains("projectId") &&
        (out.projectId.IsNull() || out.projectFile.empty()))
    {
        out.diagnostic = "project recovery has invalid project identity";
        return false;
    }
    out.revision = root.value("revision", uint64_t(0));
    out.createdAtUnix = root.value("createdAt", int64_t(0));
    out.snapshotJson = root["snapshot"].dump(2);
    out.valid = true;
    return true;
}

std::vector<SceneRecoveryService::RecoveryRecord>
SceneRecoveryService::Discover(Error& err) const
{
    err = Error{};
    std::vector<RecoveryRecord> records;
    std::error_code ec;
    if (!std::filesystem::exists(m_RecoveryRoot, ec)) return records;

    for (std::filesystem::directory_iterator it(m_RecoveryRoot, ec), end;
         !ec && it != end; it.increment(ec))
    {
        const auto& entry = *it;
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".rt2recovery")
            continue;
        RecoveryRecord record;
        ParseRecord(entry.path(), record);
        records.push_back(std::move(record));
    }
    if (ec)
    {
        err.code = Error::Io;
        err.path = m_RecoveryRoot.string();
        err.detail = "failed while enumerating recovery records: " + ec.message();
    }
    std::sort(records.begin(), records.end(), [](const RecoveryRecord& a, const RecoveryRecord& b) {
        if (a.createdAtUnix != b.createdAtUnix) return a.createdAtUnix > b.createdAtUnix;
        return a.docId < b.docId;
    });
    return records;
}

bool SceneRecoveryService::Restore(const RecoveryRecord& record,
                                   SceneDocument& outDoc,
                                   std::vector<AssetDiagnostic>& diagnostics,
                                   Error& err) const
{
    SceneLoadReport ignored;
    return Restore(record, outDoc, diagnostics, ignored, err);
}

bool SceneRecoveryService::Restore(const RecoveryRecord& record,
                                   SceneDocument& outDoc,
                                   std::vector<AssetDiagnostic>& diagnostics,
                                   SceneLoadReport& loadReport,
                                   Error& err) const
{
	if (!record.projectId.IsNull())
	{
		err.code = Error::InvalidArgument;
		err.path = record.projectFile.u8string();
		err.detail =
			"project-bound recovery requires a reloaded project context";
		return false;
	}
	const AssetResolutionContext context{ record.assetRoot, nullptr };
	return Restore(record, context, outDoc, diagnostics, loadReport, err);
}

bool SceneRecoveryService::Restore(const RecoveryRecord& record,
                                   const AssetResolutionContext& context,
                                   SceneDocument& outDoc,
                                   std::vector<AssetDiagnostic>& diagnostics,
                                   SceneLoadReport& loadReport,
                                   Error& err) const
{
    err = Error{};
    if (!record.projectId.IsNull() && context.database == nullptr)
    {
        err.code = Error::InvalidArgument;
        err.path = record.projectFile.u8string();
        err.detail =
            "project-bound recovery requires a database snapshot";
        return false;
    }
    if (!record.valid || record.snapshotJson.empty())
    {
        err.code = Error::Parse;
        err.path = record.recordPath.string();
        err.detail = "cannot restore invalid recovery record: " + record.diagnostic;
        return false;
    }

    auto restoreTemp = record.recordPath;
    restoreTemp += ".restore.tmp.rt2scene";
    {
        std::ofstream out(restoreTemp, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            err.code = Error::Io;
            err.path = restoreTemp.string();
            err.detail = "failed to materialize recovery snapshot";
            return false;
        }
        out.write(record.snapshotJson.data(),
                  static_cast<std::streamsize>(record.snapshotJson.size()));
        if (!out)
        {
            err.code = Error::Io;
            err.path = restoreTemp.string();
            err.detail = "failed while materializing recovery snapshot";
            return false;
        }
    }

    SceneDocument temp;
    temp.SetUuidProvider(outDoc.GetUuidProvider());
    const bool loaded = SceneSerializer::Load(temp, restoreTemp, loadReport, err);
    std::error_code ec;
    std::filesystem::remove(restoreTemp, ec);
    if (!loaded) return false;

    // Script resolution uses metadata.sourcePath, so restore the logical path
    // before either resolver runs. Asset resolution still receives assetRoot
    // explicitly for untitled recovery records.
    temp.metadata.sourcePath = !record.projectId.IsNull() &&
        !record.sceneLocator.empty()
        ? (context.assetRoot / std::filesystem::u8path(record.sceneLocator)).
            lexically_normal()
        : record.originalSourcePath;
    temp.metadata.projectId = record.projectId;
    temp.metadata.assetRoot = context.assetRoot;
    if (!SceneAssetResolver::ResolveAll(temp, context, diagnostics, err))
        return false;

    temp.metadata.dirty = true;
    outDoc = std::move(temp);
    return true;
}

bool SceneRecoveryService::IsContainedRecordPath(const std::filesystem::path& path) const
{
    std::error_code ec;
    auto root = std::filesystem::weakly_canonical(m_RecoveryRoot, ec);
    if (ec) root = std::filesystem::absolute(m_RecoveryRoot, ec);
    auto candidate = std::filesystem::weakly_canonical(path, ec);
    if (ec) candidate = std::filesystem::absolute(path, ec);
    if (root.empty() || candidate.empty()) return false;
    return candidate.parent_path() == root && candidate.extension() == ".rt2recovery";
}

bool SceneRecoveryService::Discard(const RecoveryRecord& record, Error& err) const
{
    err = Error{};
    if (!IsContainedRecordPath(record.recordPath))
    {
        err.code = Error::Io;
        err.path = record.recordPath.string();
        err.detail = "refusing to discard a path outside the recovery root";
        return false;
    }
    std::error_code ec;
    std::filesystem::remove(record.recordPath, ec);
    if (ec)
    {
        err.code = Error::Io;
        err.path = record.recordPath.string();
        err.detail = "failed to remove recovery record: " + ec.message();
        return false;
    }
    return true;
}

void SceneRecoveryService::DiscardForDoc(const std::string& docId)
{
    if (docId.empty()) return;
    std::error_code ec;
    std::filesystem::remove(RecordPath(docId), ec);
}

void SceneRecoveryService::OnSaveAs(const std::string& oldDocId,
                                    const std::string& newDocId)
{
    DiscardForDoc(oldDocId);
    DiscardForDoc(newDocId);
}

void SceneRecoveryService::EvictExcess(const std::filesystem::path& keepPath) const
{
    Error ignored;
    auto records = Discover(ignored);
    std::vector<RecoveryRecord> valid;
    for (auto& record : records)
        if (record.valid) valid.push_back(std::move(record));
    if (valid.size() <= m_MaxRecords) return;

    std::sort(valid.begin(), valid.end(), [](const RecoveryRecord& a, const RecoveryRecord& b) {
        if (a.createdAtUnix != b.createdAtUnix) return a.createdAtUnix < b.createdAtUnix;
        return a.docId < b.docId;
    });

    size_t remainingToRemove = valid.size() - m_MaxRecords;
    std::error_code ec;
    for (const auto& record : valid)
    {
        if (remainingToRemove == 0) break;
        if (record.recordPath == keepPath) continue;
        std::filesystem::remove(record.recordPath, ec);
        if (!ec) --remainingToRemove;
        ec.clear();
    }
}

void SceneRecoveryService::CleanupStaleTemporaryFiles() const
{
    std::error_code ec;
    if (!std::filesystem::exists(m_RecoveryRoot, ec)) return;
    for (const auto& entry : std::filesystem::directory_iterator(m_RecoveryRoot, ec))
    {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const auto name = entry.path().filename().string();
        if (name.find(".tmp") != std::string::npos ||
            name.find(".capture.rt2scene") != std::string::npos)
            std::filesystem::remove(entry.path(), ec);
        ec.clear();
    }
}

} // namespace rt2::core
