#pragma once

#include "Error.h"
#include "UUID.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace rt2::core {

struct PrefabFileSnapshot
{
    bool exists = false;
    std::vector<uint8_t> bytes;
};

struct PrefabPairSnapshot
{
    PrefabFileSnapshot asset;
    PrefabFileSnapshot sidecar;
};

struct TransactionCommitOutcome
{
    std::optional<Error> recoveryWarning;
};

// Deterministic native-operation fault seam used by RT2Tests. Production
// leaves the point at None; tests inject one failure and then clear it.
enum class PathTransactionFaultPoint
{
    None,
    CaptureOpen,
    CaptureRead,
    QuarantineRename,
    StageCreate,
    StageWrite,
    SidecarInstall,
    AssetInstall,
    RollbackRestore,
    ManifestFlush,
    ManifestCleanup,
};

void SetPathTransactionFaultForTests(PathTransactionFaultPoint point);
void ClearPathTransactionFaultForTests();

// CPU-only, Windows/NTFS-scoped transaction coordinator for one prefab asset
// and its identity sidecar. The implementation deliberately has no renderer,
// UI, JSON, or scene dependency so it links into RT2Tests and RT2SliceRunner.
class PrefabFileTransaction final
{
public:
    static Result<std::unique_ptr<PrefabFileTransaction>> Begin(
        std::filesystem::path assetPath,
        std::filesystem::path sidecarPath = {},
        bool manageSidecar = true);

    ~PrefabFileTransaction() noexcept;
    PrefabFileTransaction(PrefabFileTransaction&&) noexcept;
    PrefabFileTransaction& operator=(PrefabFileTransaction&&) noexcept;
    PrefabFileTransaction(const PrefabFileTransaction&) = delete;
    PrefabFileTransaction& operator=(const PrefabFileTransaction&) = delete;

    Result<PrefabPairSnapshot> InspectCurrent() const;
    Result<PrefabPairSnapshot> CapturePair();
    Result<void> Stage(const std::optional<std::vector<uint8_t>>& sidecarBytes,
                       const std::optional<std::vector<uint8_t>>& assetBytes);
    Result<void> InstallSidecarThenAsset();
    Result<TransactionCommitOutcome> Finalize();
    Result<void> Rollback();

    // Convenience for the asset-only command path. It preserves the same
    // capture/identity/no-replace protocol while leaving the sidecar untouched.
    static Result<TransactionCommitOutcome> ApplySingle(
        const std::filesystem::path& path,
        bool expectedExists,
        const std::vector<uint8_t>& expectedBytes,
        bool desiredExists,
        const std::vector<uint8_t>& desiredBytes,
        bool manageSidecar = false);

    // Recovery is deliberately scoped to a touched directory. Startup/load
    // callers may scan project asset roots; arbitrary external directories
    // are discovered only when a transaction touches them.
    static Result<void> RecoverDirectory(const std::filesystem::path& directory);

private:
    struct Impl;
    explicit PrefabFileTransaction(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_Impl;
};

} // namespace rt2::core
