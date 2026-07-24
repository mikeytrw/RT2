#pragma once

#ifndef RT2_CORE_SCENE_RECOVERY_SERVICE_H
#define RT2_CORE_SCENE_RECOVERY_SERVICE_H

#include "SceneDocument.h"
#include "SceneAssetResolver.h"
#include "core/Error.h"

#include <filesystem>
#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <optional>

namespace rt2::core {

struct SceneLoadReport;

// Crash recovery is deliberately one atomic envelope per logical document,
// not version history. Each .rt2recovery file contains its manifest and the
// complete schema-v3 scene JSON. Tests inject the storage root and clock.
class SceneRecoveryService
{
public:
    static constexpr uint32_t ManifestVersion = 2;
    static constexpr size_t   kDefaultMaxRecords = 8;
    static constexpr double   kDefaultIntervalSeconds = 60.0;

    struct RecoveryRecord
    {
        std::filesystem::path recordPath;
        uint32_t              version = 0;
        std::string           docId;
        bool                  untitled = false;
        std::filesystem::path originalSourcePath;
        std::filesystem::path assetRoot;
        uint64_t              revision = 0;
        int64_t               createdAtUnix = 0;
        bool                  valid = false;
        std::string           diagnostic;

        // Owned serialized scene payload extracted from the envelope. It is
        // intentionally immutable from the caller's perspective.
        std::string           snapshotJson;
    };

    using ClockNow = std::function<int64_t()>;

    explicit SceneRecoveryService(std::filesystem::path recoveryRoot,
                                  ClockNow clock = nullptr,
                                  size_t maxRecords = kDefaultMaxRecords,
                                  double intervalSeconds = kDefaultIntervalSeconds);

    // The explicit overload is used by the editor. For an untitled document,
    // untitledRecoveryId must be stable for the current authoring session and
    // logicalAssetRoot is the selected project root (or another stable base).
    bool MaybeSnapshot(const SceneDocument& doc,
                       uint64_t currentRevision,
                       const std::string& untitledRecoveryId,
                       const std::filesystem::path& logicalAssetRoot,
                       Error& err);

    // Compatibility/test convenience: uses metadata.name as the untitled id
    // and current_path as the untitled logical asset root.
    bool MaybeSnapshot(const SceneDocument& doc,
                       uint64_t currentRevision,
                       Error& err);

    std::vector<RecoveryRecord> Discover(Error& err) const;

    // Transactional restore. outDoc is assigned only after scene parsing and
    // hard asset resolution succeed. Environment failure remains diagnostic,
    // matching the Phase 1A missing-environment policy.
    bool Restore(const RecoveryRecord& record,
                 SceneDocument& outDoc,
                 std::vector<AssetDiagnostic>& diagnostics,
                 Error& err) const;

    bool Restore(const RecoveryRecord& record,
                 SceneDocument& outDoc,
                 std::vector<AssetDiagnostic>& diagnostics,
                 SceneLoadReport& loadReport,
                 Error& err) const;

    bool Discard(const RecoveryRecord& record, Error& err) const;
    void DiscardForDoc(const std::string& docId);
    void OnSaveAs(const std::string& oldDocId, const std::string& newDocId);

    size_t   GetMaxRecords() const { return m_MaxRecords; }
    double   GetIntervalSeconds() const { return m_IntervalSeconds; }
    int64_t  GetLastSnapshotTime() const { return m_LastSnapshotTime; }
    uint64_t GetLastWrittenRevision() const { return m_LastWrittenRevision; }

    void ResetSchedule();

    static std::string DocIdFor(const SceneDocument& doc,
                                const std::string& untitledRecoveryId);

    void SetNowOverride(int64_t now) { m_NowOverride = now; }
    void ClearNowOverride() { m_NowOverride.reset(); }

private:
    std::filesystem::path m_RecoveryRoot;
    ClockNow              m_Clock;
    size_t                m_MaxRecords;
    double                m_IntervalSeconds;
    int64_t               m_LastSnapshotTime = 0;
    int64_t               m_FirstDirtyObservedAt = 0;
    bool                  m_HasDirtyObservation = false;
    bool                  m_HasWrittenSnapshot = false;
    uint64_t              m_LastWrittenRevision = 0;
    uint64_t              m_ObservedRevision = 0;
    std::optional<int64_t> m_NowOverride;

    int64_t Now() const;
    std::filesystem::path RecordPath(const std::string& docId) const;

    bool WriteRecord(const std::string& docId,
                     const SceneDocument& doc,
                     const std::filesystem::path& logicalAssetRoot,
                     uint64_t revision,
                     int64_t createdAt,
                     Error& err);
    bool ParseRecord(const std::filesystem::path& path,
                     RecoveryRecord& out) const;
    void EvictExcess(const std::filesystem::path& keepPath) const;
    void CleanupStaleTemporaryFiles() const;
    bool IsContainedRecordPath(const std::filesystem::path& path) const;
};

} // namespace rt2::core

#endif // RT2_CORE_SCENE_RECOVERY_SERVICE_H
