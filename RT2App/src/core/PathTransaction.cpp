#include "PathTransaction.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <atomic>
#include <sstream>
#include <string>
#include <system_error>
#include <iomanip>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace rt2::core {

namespace {
std::atomic<PathTransactionFaultPoint> g_faultPoint{PathTransactionFaultPoint::None};
bool Fault(PathTransactionFaultPoint point)
{
    auto expected = point;
    return g_faultPoint.compare_exchange_strong(expected, PathTransactionFaultPoint::None);
}
}

void SetPathTransactionFaultForTests(PathTransactionFaultPoint point)
{ g_faultPoint.store(point); }

void ClearPathTransactionFaultForTests()
{ g_faultPoint.store(PathTransactionFaultPoint::None); }

#ifdef _WIN32
namespace {

struct Handle
{
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { Reset(); }
    Handle(Handle&& other) noexcept : value(other.value)
    { other.value = INVALID_HANDLE_VALUE; }
    Handle& operator=(Handle&& other) noexcept
    {
        if (this != &other) { Reset(); value = other.value; other.value = INVALID_HANDLE_VALUE; }
        return *this;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    bool Valid() const { return value != INVALID_HANDLE_VALUE && value != nullptr; }
    void Reset() { if (Valid()) CloseHandle(value); value = INVALID_HANDLE_VALUE; }
    HANDLE Release() { HANDLE h = value; value = INVALID_HANDLE_VALUE; return h; }
};

struct FileKey
{
    ULONGLONG volume = 0;
    FILE_ID_128 id{};
};

struct ChainEntry
{
    std::filesystem::path path;
    Handle handle;
    FileKey key{};
};

struct Slot
{
    std::filesystem::path original;
    std::filesystem::path backup;
    std::filesystem::path stage;
    bool managed = false;
    bool existed = false;
    bool quarantined = false;
    bool staged = false;
    bool installed = false;
    Handle handle; // capture handle before quarantine
    Handle backupHandle;
    Handle stageHandle;
    Handle installedHandle;
    FileKey key{}; // capture key
    FileKey backupKey{};
    FileKey stageKey{};
    FileKey installedKey{};
    std::vector<uint8_t> bytes;
};

struct ManifestHeader
{
    uint32_t magic = 0x32544A52; // RT2J
    uint32_t version = 1;
    uint64_t generation = 0;
    uint32_t length = 0;
    uint32_t crc = 0;
};

constexpr DWORD kManifestSlotSize = 4096;

std::string WinMessage(DWORD code)
{
    LPSTR buffer = nullptr;
    const DWORD len = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                     FORMAT_MESSAGE_FROM_SYSTEM |
                                     FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr, code, 0,
                                     reinterpret_cast<LPSTR>(&buffer), 0, nullptr);
    std::string result = len ? std::string(buffer, len) : std::to_string(code);
    if (buffer) LocalFree(buffer);
    while (!result.empty() && (result.back() == '\r' || result.back() == '\n')) result.pop_back();
    return result;
}

Result<void> Fail(Error::Code code, const std::filesystem::path& path,
                  const std::string& detail)
{
    return Result<void>::Fail(code, path.string(), detail);
}

Result<PrefabPairSnapshot> FailPair(const std::filesystem::path& path,
                                    const std::string& detail)
{
    return Result<PrefabPairSnapshot>::Fail(Error::Io, path.string(), detail);
}

Error WinError(const std::filesystem::path& path, const char* operation)
{
    const DWORD code = GetLastError();
    Error e;
    e.code = Error::Io;
    e.path = path.string();
    e.detail = std::string(operation) + " failed: " + WinMessage(code) +
               " (win32=" + std::to_string(code) + ")";
    return e;
}

bool SameKey(const FileKey& a, const FileKey& b)
{
    return a.volume == b.volume && std::memcmp(&a.id, &b.id, sizeof(a.id)) == 0;
}

std::string KeyText(const FileKey& key)
{
    std::ostringstream out;
    out << std::hex << key.volume << ":";
    const auto* bytes = reinterpret_cast<const uint8_t*>(&key.id);
    for (size_t i = 0; i < sizeof(key.id); ++i)
        out << std::setw(2) << std::setfill('0') << static_cast<unsigned>(bytes[i]);
    return out.str();
}

bool ReadKey(HANDLE h, FileKey& out)
{
    FILE_ID_INFO info{};
    if (!GetFileInformationByHandleEx(h, FileIdInfo, &info, sizeof(info))) return false;
    out.volume = info.VolumeSerialNumber;
    out.id = info.FileId;
    return true;
}

bool IsReparse(HANDLE h)
{
    FILE_ATTRIBUTE_TAG_INFO info{};
    return GetFileInformationByHandleEx(h, FileAttributeTagInfo, &info, sizeof(info)) &&
           (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool IsLocalNtfs(const std::filesystem::path& path)
{
    const auto root = path.root_path();
    if (root.empty() || GetDriveTypeW(root.c_str()) != DRIVE_FIXED) return false;
    wchar_t fs[32]{};
    return GetVolumeInformationW(root.c_str(), nullptr, 0, nullptr, nullptr, nullptr,
                                 fs, static_cast<DWORD>(std::size(fs))) &&
           _wcsicmp(fs, L"NTFS") == 0;
}

bool BuildChain(const std::filesystem::path& parent,
                std::vector<ChainEntry>& out, Error& err)
{
    if (!IsLocalNtfs(parent))
    {
        err = Error{};
        err.code = Error::Io;
        err.path = parent.string();
        err.detail = "transaction requires a local fixed NTFS volume";
        return false;
    }

    std::filesystem::path current = parent.root_path();
    out.clear();
    auto holdDirectory = [&](const std::filesystem::path& directory) -> bool
    {
        HANDLE raw = CreateFileW(directory.wstring().c_str(),
                                 FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING,
                                 FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                                 nullptr);
        if (raw == INVALID_HANDLE_VALUE)
        {
            err = WinError(directory, "open ancestor directory");
            return false;
        }
        ChainEntry entry;
        entry.path = directory;
        entry.handle = Handle(raw);
        if (IsReparse(entry.handle.value) || !ReadKey(entry.handle.value, entry.key))
        {
            err = Error{};
            err.code = Error::Io;
            err.path = directory.string();
            err.detail = "ancestor is a reparse point or has no stable file identity";
            return false;
        }
        out.push_back(std::move(entry));
        return true;
    };
    if (!holdDirectory(current)) return false;
    for (const auto& component : parent.relative_path())
    {
        current /= component;
        if (!holdDirectory(current)) return false;
    }
    return true;
}

bool VerifyChain(const std::vector<ChainEntry>& chain, Error& err)
{
    for (const auto& entry : chain)
    {
        FileKey now{};
        if (!entry.handle.Valid() || IsReparse(entry.handle.value) ||
            !ReadKey(entry.handle.value, now) || !SameKey(now, entry.key))
        {
            err = Error{};
            err.code = Error::Io;
            err.path = entry.path.string();
            err.detail = "ancestor identity changed while transaction was active";
            return false;
        }
    }
    return true;
}

bool ParentContains(HANDLE parent, const std::filesystem::path& leaf,
                    const FileKey& expected)
{
    std::array<uint8_t, 64 * 1024> buffer{};
    if (!GetFileInformationByHandleEx(parent, FileIdBothDirectoryRestartInfo,
                                      buffer.data(), static_cast<DWORD>(buffer.size())))
        return false;
    const std::wstring wanted = leaf.filename().wstring();
    for (DWORD offset = 0; ; )
    {
        const auto* info = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(buffer.data() + offset);
        if (info->FileNameLength / sizeof(wchar_t) == wanted.size() &&
            _wcsnicmp(info->FileName, wanted.c_str(), wanted.size()) == 0)
        {
            // FILE_ID_BOTH_DIR_INFO exposes the NTFS file-id low 64 bits;
            // compare that directory binding and then retain the full
            // FILE_ID_INFO key from the opened handle for all ownership checks.
            uint64_t listedLow = 0;
            std::memcpy(&listedLow, &info->FileId, sizeof(listedLow));
            uint64_t expectedLow = 0;
            std::memcpy(&expectedLow, &expected.id, sizeof(expectedLow));
            return listedLow == expectedLow;
        }
        if (info->NextEntryOffset == 0) break;
        offset += info->NextEntryOffset;
        if (offset >= buffer.size()) break;
    }
    return false;
}

bool PathEntryExists(const std::filesystem::path& path, WIN32_FIND_DATAW* data)
{
    WIN32_FIND_DATAW local{};
    HANDLE h = FindFirstFileW(path.wstring().c_str(), &local);
    if (h == INVALID_HANDLE_VALUE) return false;
    FindClose(h);
    if (data) *data = local;
    return true;
}

Result<Slot> OpenAndCapture(const std::filesystem::path& path,
                            const std::vector<ChainEntry>& chain)
{
    if (Fault(PathTransactionFaultPoint::CaptureOpen))
        return Result<Slot>::Fail(Error::Io, path.string(), "fault seam: capture open");
    Slot slot;
    slot.original = path;
    WIN32_FIND_DATAW data{};
    if (!PathEntryExists(path, &data))
    {
        if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND)
            return Result<Slot>::Fail(Error::Io, path.string(), "failed to inspect target entry");
        return Result<Slot>::Ok(std::move(slot));
    }
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return Result<Slot>::Fail(Error::Io, path.string(), "target is a symlink/junction/reparse point");
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return Result<Slot>::Fail(Error::Io, path.string(), "target is not a regular file");

    HANDLE raw = CreateFileW(path.wstring().c_str(),
                             GENERIC_READ | DELETE | FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (raw == INVALID_HANDLE_VALUE)
        return Result<Slot>::Fail(Error::Io, path.string(), "target is inaccessible: " + WinMessage(GetLastError()));
    slot.handle = Handle(raw);
    if (IsReparse(slot.handle.value))
        return Result<Slot>::Fail(Error::Io, path.string(), "target is a reparse point");
    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandleEx(slot.handle.value, FileStandardInfo,
                                      &standard, sizeof(standard)) || standard.Directory)
        return Result<Slot>::Fail(Error::Io, path.string(), "target is not a regular file");
    if (!ReadKey(slot.handle.value, slot.key) || chain.empty() ||
        !ParentContains(chain.back().handle.value, path, slot.key))
        return Result<Slot>::Fail(Error::Io, path.string(), "opened entry is not bound to held parent");

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(slot.handle.value, &size) || size.QuadPart < 0 || size.QuadPart > 0x7fffffff)
        return Result<Slot>::Fail(Error::Io, path.string(), "invalid target size");
    const DWORD lockLength = size.QuadPart == 0 ? 1u : static_cast<DWORD>(size.QuadPart);
    OVERLAPPED ov{};
    if (!LockFileEx(slot.handle.value, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                    0, lockLength, 0, &ov))
        return Result<Slot>::Fail(Error::Io, path.string(), "target is locked");
    std::vector<uint8_t> bytes(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    size_t offset = 0;
    while (offset < bytes.size())
    {
        const DWORD want = static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, 1u << 20));
        if (Fault(PathTransactionFaultPoint::CaptureRead) ||
            !ReadFile(slot.handle.value, bytes.data() + offset, want, &read, nullptr) || read == 0)
        {
            UnlockFileEx(slot.handle.value, 0, lockLength, 0, &ov);
            return Result<Slot>::Fail(Error::Io, path.string(), "complete target read failed");
        }
        offset += read;
    }
    uint8_t terminal = 0;
    if (ReadFile(slot.handle.value, &terminal, 1, &read, nullptr) && read != 0)
    {
        UnlockFileEx(slot.handle.value, 0, lockLength, 0, &ov);
        return Result<Slot>::Fail(Error::Io, path.string(), "target changed during read");
    }
    LARGE_INTEGER after{};
    FileKey afterKey{};
    if (!GetFileSizeEx(slot.handle.value, &after) || after.QuadPart != size.QuadPart ||
        !ReadKey(slot.handle.value, afterKey) || !SameKey(afterKey, slot.key))
    {
        UnlockFileEx(slot.handle.value, 0, lockLength, 0, &ov);
        return Result<Slot>::Fail(Error::Io, path.string(), "target identity/size changed during read");
    }
    UnlockFileEx(slot.handle.value, 0, lockLength, 0, &ov);
    slot.existed = true;
    slot.bytes = std::move(bytes);
    return Result<Slot>::Ok(std::move(slot));
}

bool RenameHandle(HANDLE handle, const std::filesystem::path& destination)
{
    const std::wstring name = destination.wstring();
    const size_t bytes = sizeof(FILE_RENAME_INFO) + (name.size() ? (name.size() - 1) * sizeof(wchar_t) : 0);
    std::vector<uint8_t> buffer(bytes);
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    std::memset(info, 0, bytes);
    info->ReplaceIfExists = FALSE;
    info->RootDirectory = nullptr;
    info->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
    if (!name.empty()) std::memcpy(info->FileName, name.data(), name.size() * sizeof(wchar_t));
    return SetFileInformationByHandle(handle, FileRenameInfo, info, static_cast<DWORD>(bytes)) != FALSE;
}

bool DeleteHandle(HANDLE handle)
{
    FILE_DISPOSITION_INFO disposition{ TRUE };
    return SetFileInformationByHandle(handle, FileDispositionInfo,
                                      &disposition, sizeof(disposition)) != FALSE;
}

uint32_t Crc32(const uint8_t* data, size_t length)
{
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < length; ++i)
    {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

struct Manifest
{
    Handle handle;
    std::filesystem::path path;
    uint64_t generation = 0;
    std::string metadata;
    bool Create(const std::filesystem::path& p)
    {
        path = p;
        HANDLE raw = CreateFileW(p.wstring().c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
                                 FILE_SHARE_READ, nullptr, CREATE_NEW,
                                 FILE_ATTRIBUTE_HIDDEN, nullptr);
        if (raw == INVALID_HANDLE_VALUE) return false;
        handle = Handle(raw);
        LARGE_INTEGER size{}; size.QuadPart = static_cast<LONGLONG>(kManifestSlotSize * 2);
        if (!SetFilePointerEx(handle.value, size, nullptr, FILE_BEGIN) ||
            !SetEndOfFile(handle.value) || !FlushFileBuffers(handle.value)) return false;
        return true;
    }
    bool Write(const std::string& state)
    {
        if (Fault(PathTransactionFaultPoint::ManifestFlush)) return false;
        const std::string payload = metadata.empty() ? state : state + "\n" + metadata;
        if (!handle.Valid() || payload.size() > kManifestSlotSize - sizeof(ManifestHeader)) return false;
        ManifestHeader header{};
        header.generation = ++generation;
        header.length = static_cast<uint32_t>(payload.size());
        header.crc = Crc32(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
        const DWORD slot = static_cast<DWORD>((generation & 1u) * kManifestSlotSize);
        LARGE_INTEGER pos{}; pos.QuadPart = slot;
        if (!SetFilePointerEx(handle.value, pos, nullptr, FILE_BEGIN)) return false;
        DWORD wrote = 0;
        if (!WriteFile(handle.value, &header, sizeof(header), &wrote, nullptr) || wrote != sizeof(header)) return false;
        if (!payload.empty() && (!WriteFile(handle.value, payload.data(), static_cast<DWORD>(payload.size()), &wrote, nullptr) || wrote != payload.size())) return false;
        return FlushFileBuffers(handle.value) != FALSE;
    }
    bool Remove()
    {
        if (Fault(PathTransactionFaultPoint::ManifestCleanup)) return false;
        handle.Reset();
        return DeleteFileW(path.wstring().c_str()) != FALSE || GetLastError() == ERROR_FILE_NOT_FOUND;
    }
};

} // namespace

struct PrefabFileTransaction::Impl
{
    std::filesystem::path assetPath;
    std::filesystem::path sidecarPath;
    std::filesystem::path parent;
    bool manageSidecar = false;
    UUID transactionId;
    std::vector<ChainEntry> chain;
    Manifest manifest;
    Slot asset;
    Slot sidecar;
    bool captured = false;
    bool committed = false;
    bool logicalCommitted = false;

    bool WriteManifest(const std::string& state)
    {
        std::ostringstream metadata;
        metadata << "asset=" << assetPath.string() << "\nsidecar=" << sidecarPath.string();
        if (asset.key.volume != 0) metadata << "\nasset.capture=" << KeyText(asset.key);
        if (asset.backupKey.volume != 0) metadata << "\nasset.backup=" << KeyText(asset.backupKey);
        if (asset.stageKey.volume != 0) metadata << "\nasset.stage=" << KeyText(asset.stageKey);
        if (asset.installedKey.volume != 0) metadata << "\nasset.installed=" << KeyText(asset.installedKey);
        if (sidecar.key.volume != 0) metadata << "\nsidecar.capture=" << KeyText(sidecar.key);
        if (sidecar.backupKey.volume != 0) metadata << "\nsidecar.backup=" << KeyText(sidecar.backupKey);
        if (sidecar.stageKey.volume != 0) metadata << "\nsidecar.stage=" << KeyText(sidecar.stageKey);
        if (sidecar.installedKey.volume != 0) metadata << "\nsidecar.installed=" << KeyText(sidecar.installedKey);
        manifest.metadata = metadata.str();
        return manifest.Write(state);
    }

    Error Prepare()
    {
        Error error;
        assetPath = std::filesystem::absolute(assetPath).lexically_normal();
        parent = assetPath.parent_path();
        if (!IsLocalNtfs(parent))
        {
            error.code = Error::Io; error.path = assetPath.string();
            error.detail = "unsupported filesystem: transaction requires local fixed NTFS";
            return error;
        }
        if (manageSidecar)
        {
            sidecarPath = std::filesystem::absolute(sidecarPath).lexically_normal();
            if (sidecarPath.parent_path() != parent)
            {
                error.code = Error::Io; error.path = sidecarPath.string();
                error.detail = "asset and sidecar must share one parent directory";
                return error;
            }
        }
        if (!BuildChain(parent, chain, error)) return error;
        auto recovered = PrefabFileTransaction::RecoverDirectory(parent);
        if (!recovered.IsOk())
        {
            error = recovered.error;
            return error;
        }
        transactionId = OsUuidProvider{}.CreateV4();
        std::filesystem::path manifestPath = parent / (".rt2txn-" + transactionId.ToString() + ".manifest");
        if (!manifest.Create(manifestPath) || !WriteManifest("Prepared"))
        {
            error = WinError(manifestPath, "create transaction manifest");
            return error;
        }
        return {};
    }

    Error VerifyPathBinding(const std::filesystem::path& path, const FileKey& key) const
    {
        Error error;
        if (!VerifyChain(chain, error) || chain.empty() ||
            !ParentContains(chain.back().handle.value, path, key))
        {
            if (error.IsOk())
            {
                error.code = Error::Io;
                error.path = path.string();
                error.detail = "path entry identity is not bound to held parent";
            }
        }
        return error;
    }

    Error Quarantine(Slot& slot)
    {
        if (!slot.existed) return {};
        if (!slot.handle.Valid()) { Error e; e.code = Error::Io; e.path = slot.original.string(); e.detail = "missing capture handle"; return e; }
        Error binding = VerifyPathBinding(slot.original, slot.key);
        if (!binding.IsOk()) return binding;
        slot.backup = parent / (".rt2txn-" + transactionId.ToString() +
                                (slot.original == assetPath ? ".asset.bak" : ".sidecar.bak"));
        if (Fault(PathTransactionFaultPoint::QuarantineRename) || !RenameHandle(slot.handle.value, slot.backup))
            return WinError(slot.original, "quarantine entry");
        slot.backupHandle = std::move(slot.handle);
        slot.backupKey = slot.key;
        slot.quarantined = true;
        return {};
    }

    Error CreateStage(Slot& slot, const std::optional<std::vector<uint8_t>>& bytes)
    {
        if (!bytes.has_value()) return {};
        slot.stage = parent / (".rt2txn-" + transactionId.ToString() +
                              (slot.original == assetPath ? ".asset.stage" : ".sidecar.stage"));
        if (Fault(PathTransactionFaultPoint::StageCreate)) return Fail(Error::Io, slot.stage, "fault seam: stage create").error;
        HANDLE raw = CreateFileW(slot.stage.wstring().c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
                                 FILE_SHARE_READ, nullptr, CREATE_NEW,
                                 FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (raw == INVALID_HANDLE_VALUE) return WinError(slot.stage, "create transaction stage");
        slot.stageHandle = Handle(raw);
        slot.staged = true;
        if (!bytes->empty())
        {
            DWORD wrote = 0;
            if (Fault(PathTransactionFaultPoint::StageWrite) || !WriteFile(slot.stageHandle.value, bytes->data(), static_cast<DWORD>(bytes->size()), &wrote, nullptr) || wrote != bytes->size())
                return WinError(slot.stage, "write transaction stage");
        }
        if (!FlushFileBuffers(slot.stageHandle.value)) return WinError(slot.stage, "flush transaction stage");
        if (!ReadKey(slot.stageHandle.value, slot.stageKey)) return WinError(slot.stage, "identify transaction stage");
        Error binding = VerifyPathBinding(slot.stage, slot.stageKey);
        if (!binding.IsOk()) return binding;
        return {};
    }

    Error Install(Slot& slot)
    {
        if (!slot.staged) return {};
        Error binding = VerifyPathBinding(slot.stage, slot.stageKey);
        if (!binding.IsOk()) return binding;
        const auto installFault = slot.original == assetPath ? PathTransactionFaultPoint::AssetInstall : PathTransactionFaultPoint::SidecarInstall;
        if (Fault(installFault) || !RenameHandle(slot.stageHandle.value, slot.original))
            return WinError(slot.original, "install transaction entry");
        slot.installedHandle = std::move(slot.stageHandle);
        slot.installedKey = slot.stageKey;
        slot.installed = true;
        Error after = VerifyPathBinding(slot.original, slot.installedKey);
        if (!after.IsOk()) return after;
        return {};
    }

    Error RemoveSlotOwned(Slot& slot)
    {
        if (slot.installedHandle.Valid() && slot.installed)
        {
            if (!DeleteHandle(slot.installedHandle.value)) return WinError(slot.original, "remove owned installed entry");
            slot.installedHandle.Reset();
            slot.installed = false;
        }
        return {};
    }

    Error RestoreSlot(Slot& slot)
    {
        if (slot.installed)
        {
            Error binding = VerifyPathBinding(slot.original, slot.installedKey);
            if (!binding.IsOk()) return binding;
            if (slot.existed)
            {
                const std::filesystem::path residue = parent / (".rt2txn-" + transactionId.ToString() + ".rollback");
                if (!RenameHandle(slot.installedHandle.value, residue)) return WinError(slot.original, "quarantine installed entry for rollback");
                slot.installed = false;
                if (!DeleteHandle(slot.installedHandle.value)) return WinError(residue, "remove installed rollback residue");
                slot.installedHandle.Reset();
            }
            else
            {
                if (!DeleteHandle(slot.installedHandle.value)) return WinError(slot.original, "remove created entry during rollback");
                slot.installedHandle.Reset();
                slot.installed = false;
            }
        }
        if (slot.quarantined && slot.existed)
        {
            if (!slot.backupHandle.Valid())
            {
                HANDLE raw = CreateFileW(slot.backup.wstring().c_str(), GENERIC_READ | DELETE | FILE_READ_ATTRIBUTES,
                                         FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                if (raw == INVALID_HANDLE_VALUE) return WinError(slot.backup, "open transaction backup");
                slot.backupHandle = Handle(raw);
            }
            if (Fault(PathTransactionFaultPoint::RollbackRestore) || !RenameHandle(slot.backupHandle.value, slot.original)) return WinError(slot.original, "restore transaction backup");
            slot.backupHandle.Reset();
            slot.quarantined = false;
        }
        return {};
    }

    Error CleanupSlot(Slot& slot)
    {
        if (slot.staged && slot.stageHandle.Valid())
        {
            if (!DeleteHandle(slot.stageHandle.value)) return WinError(slot.stage, "remove transaction stage");
            slot.stageHandle.Reset(); slot.staged = false;
        }
        if (slot.quarantined && slot.backupHandle.Valid())
        {
            if (!DeleteHandle(slot.backupHandle.value)) return WinError(slot.backup, "remove transaction backup");
            slot.backupHandle.Reset(); slot.quarantined = false;
        }
        return {};
    }
};

PrefabFileTransaction::PrefabFileTransaction(std::unique_ptr<Impl> impl)
    : m_Impl(std::move(impl)) {}

PrefabFileTransaction::~PrefabFileTransaction() noexcept
{
    if (!m_Impl || m_Impl->committed) return;
    (void)Rollback();
}

PrefabFileTransaction::PrefabFileTransaction(PrefabFileTransaction&&) noexcept = default;
PrefabFileTransaction& PrefabFileTransaction::operator=(PrefabFileTransaction&&) noexcept = default;

Result<std::unique_ptr<PrefabFileTransaction>> PrefabFileTransaction::Begin(
    std::filesystem::path assetPath, std::filesystem::path sidecarPath, bool manageSidecar)
{
    auto impl = std::make_unique<Impl>();
    impl->assetPath = std::move(assetPath);
    impl->sidecarPath = std::move(sidecarPath);
    impl->manageSidecar = manageSidecar && !impl->sidecarPath.empty();
    Error error = impl->Prepare();
    if (!error.IsOk()) return Result<std::unique_ptr<PrefabFileTransaction>>::Fail(error.code, error.path, error.detail);
    return Result<std::unique_ptr<PrefabFileTransaction>>::Ok(
        std::unique_ptr<PrefabFileTransaction>(new PrefabFileTransaction(std::move(impl))));
}

Result<PrefabPairSnapshot> PrefabFileTransaction::InspectCurrent() const
{
    if (!m_Impl) return FailPair({}, "invalid transaction");
    Error error;
    auto asset = OpenAndCapture(m_Impl->assetPath, m_Impl->chain);
    if (!asset.IsOk()) return Result<PrefabPairSnapshot>::Fail(asset.error.code, asset.error.path, asset.error.detail);
    PrefabPairSnapshot pair;
    pair.asset.exists = asset.value.existed; pair.asset.bytes = asset.value.bytes;
    if (m_Impl->manageSidecar)
    {
        auto sidecar = OpenAndCapture(m_Impl->sidecarPath, m_Impl->chain);
        if (!sidecar.IsOk()) return Result<PrefabPairSnapshot>::Fail(sidecar.error.code, sidecar.error.path, sidecar.error.detail);
        pair.sidecar.exists = sidecar.value.existed; pair.sidecar.bytes = sidecar.value.bytes;
    }
    return Result<PrefabPairSnapshot>::Ok(std::move(pair));
}

Result<PrefabPairSnapshot> PrefabFileTransaction::CapturePair()
{
    if (!m_Impl) return FailPair({}, "invalid transaction");
    auto asset = OpenAndCapture(m_Impl->assetPath, m_Impl->chain);
    if (!asset.IsOk()) return Result<PrefabPairSnapshot>::Fail(asset.error.code, asset.error.path, asset.error.detail);
    m_Impl->asset = std::move(asset.value);
    if (m_Impl->manageSidecar)
    {
        auto sidecar = OpenAndCapture(m_Impl->sidecarPath, m_Impl->chain);
        if (!sidecar.IsOk()) return Result<PrefabPairSnapshot>::Fail(sidecar.error.code, sidecar.error.path, sidecar.error.detail);
        m_Impl->sidecar = std::move(sidecar.value);
    }
    if (!m_Impl->WriteManifest("Captured")) return Result<PrefabPairSnapshot>::Fail(Error::Io, m_Impl->assetPath.string(), "failed to flush Captured manifest state");
    Error error = m_Impl->Quarantine(m_Impl->asset);
    if (!error.IsOk()) return Result<PrefabPairSnapshot>::Fail(error.code, error.path, error.detail);
    if (m_Impl->manageSidecar)
    {
        error = m_Impl->Quarantine(m_Impl->sidecar);
        if (!error.IsOk()) return Result<PrefabPairSnapshot>::Fail(error.code, error.path, error.detail);
    }
    if (!m_Impl->WriteManifest("Quarantined")) return Result<PrefabPairSnapshot>::Fail(Error::Io, m_Impl->assetPath.string(), "failed to flush Quarantined manifest state");
    m_Impl->captured = true;
    PrefabPairSnapshot pair;
    pair.asset.exists = m_Impl->asset.existed; pair.asset.bytes = m_Impl->asset.bytes;
    pair.sidecar.exists = m_Impl->sidecar.existed; pair.sidecar.bytes = m_Impl->sidecar.bytes;
    return Result<PrefabPairSnapshot>::Ok(std::move(pair));
}

Result<void> PrefabFileTransaction::Stage(const std::optional<std::vector<uint8_t>>& sidecarBytes,
                                          const std::optional<std::vector<uint8_t>>& assetBytes)
{
    if (!m_Impl || !m_Impl->captured) return Fail(Error::Io, {}, "stage requires captured transaction");
    Error error;
    if (m_Impl->manageSidecar) { error = m_Impl->CreateStage(m_Impl->sidecar, sidecarBytes); if (!error.IsOk()) return Result<void>{error}; }
    error = m_Impl->CreateStage(m_Impl->asset, assetBytes);
    if (!error.IsOk()) return Result<void>{error};
    if (!m_Impl->WriteManifest("Staged")) return Fail(Error::Io, m_Impl->assetPath, "failed to flush Staged manifest state");
    return Result<void>::Ok();
}

Result<void> PrefabFileTransaction::InstallSidecarThenAsset()
{
    if (!m_Impl || !m_Impl->captured) return Fail(Error::Io, {}, "install requires captured transaction");
    Error error;
    if (m_Impl->manageSidecar)
    {
        error = m_Impl->Install(m_Impl->sidecar);
        if (!error.IsOk()) return Result<void>{error};
        if (!m_Impl->WriteManifest("SidecarInstalled")) return Fail(Error::Io, m_Impl->sidecarPath, "failed to flush SidecarInstalled manifest state");
    }
    error = m_Impl->Install(m_Impl->asset);
    if (!error.IsOk()) return Result<void>{error};
    if (!m_Impl->WriteManifest("BothInstalled")) return Fail(Error::Io, m_Impl->assetPath, "failed to flush BothInstalled manifest state");
    return Result<void>::Ok();
}

Result<TransactionCommitOutcome> PrefabFileTransaction::Finalize()
{
    if (!m_Impl) return Result<TransactionCommitOutcome>::Fail(Error::Io, {}, "invalid transaction");
    if (!m_Impl->WriteManifest("LogicalCommitted")) return Result<TransactionCommitOutcome>::Fail(Error::Io, m_Impl->assetPath.string(), "failed to flush logical commit");
    m_Impl->logicalCommitted = true;
    TransactionCommitOutcome outcome;
    Error error = m_Impl->CleanupSlot(m_Impl->asset);
    if (error.IsOk() && m_Impl->manageSidecar) error = m_Impl->CleanupSlot(m_Impl->sidecar);
    m_Impl->manifest.handle.Reset();
    if (error.IsOk() && !m_Impl->manifest.Remove()) error = WinError(m_Impl->manifest.path, "remove committed manifest");
    if (!error.IsOk()) outcome.recoveryWarning = error;
    m_Impl->committed = true;
    return Result<TransactionCommitOutcome>::Ok(std::move(outcome));
}

Result<void> PrefabFileTransaction::Rollback()
{
    if (!m_Impl || m_Impl->committed) return Result<void>::Ok();
    Error error = m_Impl->RestoreSlot(m_Impl->asset);
    if (error.IsOk() && m_Impl->manageSidecar) error = m_Impl->RestoreSlot(m_Impl->sidecar);
    if (error.IsOk()) error = m_Impl->CleanupSlot(m_Impl->asset);
    if (error.IsOk() && m_Impl->manageSidecar) error = m_Impl->CleanupSlot(m_Impl->sidecar);
    if (error.IsOk()) { m_Impl->manifest.handle.Reset(); m_Impl->manifest.Remove(); m_Impl->committed = true; return Result<void>::Ok(); }
    return Result<void>::Fail(error.code, error.path, "rollback failed; recovery manifest/residue preserved: " + error.detail);
}

Result<TransactionCommitOutcome> PrefabFileTransaction::ApplySingle(
    const std::filesystem::path& path, bool expectedExists,
    const std::vector<uint8_t>& expectedBytes, bool desiredExists,
    const std::vector<uint8_t>& desiredBytes, bool manageSidecar)
{
    auto tx = Begin(path, {}, manageSidecar);
    if (!tx.IsOk()) return Result<TransactionCommitOutcome>::Fail(tx.error.code, tx.error.path, tx.error.detail);
    auto current = tx.value->InspectCurrent();
    if (!current.IsOk()) return Result<TransactionCommitOutcome>::Fail(current.error.code, current.error.path, current.error.detail);
    if (current.value.asset.exists != expectedExists ||
        (expectedExists && current.value.asset.bytes != expectedBytes))
    {
        (void)tx.value->Rollback();
        return Result<TransactionCommitOutcome>::Fail(Error::Io, path.string(), "expected prefab state changed out-of-band");
    }
    if (current.value.asset.exists == desiredExists &&
        (!desiredExists || current.value.asset.bytes == desiredBytes))
        return tx.value->Finalize(); // logical no-op: effective caller remains true
    auto captured = tx.value->CapturePair();
    if (!captured.IsOk()) return Result<TransactionCommitOutcome>::Fail(captured.error.code, captured.error.path, captured.error.detail);
    std::optional<std::vector<uint8_t>> desired;
    if (desiredExists) desired = desiredBytes;
    auto staged = tx.value->Stage(std::nullopt, desired);
    if (!staged.IsOk()) { (void)tx.value->Rollback(); return Result<TransactionCommitOutcome>::Fail(staged.error.code, staged.error.path, staged.error.detail); }
    auto installed = tx.value->InstallSidecarThenAsset();
    if (!installed.IsOk()) { (void)tx.value->Rollback(); return Result<TransactionCommitOutcome>::Fail(installed.error.code, installed.error.path, installed.error.detail); }
    return tx.value->Finalize();
}

Result<void> PrefabFileTransaction::RecoverDirectory(const std::filesystem::path& directory)
{
    const auto root = std::filesystem::absolute(directory).lexically_normal();
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec) || ec)
        return Result<void>::Fail(Error::Io, root.string(), "recovery root is not accessible");
    for (const auto& entry : std::filesystem::directory_iterator(root, ec))
    {
        if (ec) return Result<void>::Fail(Error::Io, root.string(), "failed to enumerate transaction residues");
        const auto name = entry.path().filename().wstring();
        if (name.rfind(L".rt2txn-", 0) != 0 || entry.path().extension() != L".manifest") continue;
        HANDLE raw = CreateFileW(entry.path().wstring().c_str(), GENERIC_READ | DELETE,
                                 FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (raw == INVALID_HANDLE_VALUE)
            return Result<void>::Fail(Error::Io, entry.path().string(), "transaction manifest is inaccessible; residue preserved");
        Handle manifest(raw);
        std::array<uint8_t, kManifestSlotSize * 2> bytes{};
        DWORD read = 0;
        if (!ReadFile(manifest.value, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size())
            return Result<void>::Fail(Error::Io, entry.path().string(), "transaction manifest is truncated/corrupt; residue preserved");
        uint64_t bestGeneration = 0;
        std::string bestState;
        for (size_t offset = 0; offset < bytes.size(); offset += kManifestSlotSize)
        {
            ManifestHeader header{};
            std::memcpy(&header, bytes.data() + offset, sizeof(header));
            if (header.magic != ManifestHeader{}.magic || header.version != 1 ||
                header.length > kManifestSlotSize - sizeof(ManifestHeader)) continue;
            const auto* payload = bytes.data() + offset + sizeof(ManifestHeader);
            if (Crc32(payload, header.length) != header.crc || header.generation < bestGeneration) continue;
            bestGeneration = header.generation;
            bestState.assign(reinterpret_cast<const char*>(payload), header.length);
        }
        if (bestGeneration == 0)
            return Result<void>::Fail(Error::Io, entry.path().string(), "transaction manifest has no valid CRC slot; residue preserved");
        if (bestState.rfind("LogicalCommitted", 0) == 0)
        {
            manifest.Reset();
            if (!DeleteFileW(entry.path().wstring().c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
                return Result<void>::Fail(Error::Io, entry.path().string(), "committed transaction manifest cleanup blocked");
            continue;
        }
        return Result<void>::Fail(Error::Io, entry.path().string(),
            "incomplete prefab transaction requires recovery; manifest and owned residue preserved");
    }
    return Result<void>::Ok();
}

#else

struct PrefabFileTransaction::Impl {};
PrefabFileTransaction::PrefabFileTransaction(std::unique_ptr<Impl> impl) : m_Impl(std::move(impl)) {}
PrefabFileTransaction::~PrefabFileTransaction() noexcept = default;
PrefabFileTransaction::PrefabFileTransaction(PrefabFileTransaction&&) noexcept = default;
PrefabFileTransaction& PrefabFileTransaction::operator=(PrefabFileTransaction&&) noexcept = default;

static Result<std::unique_ptr<PrefabFileTransaction>> Unsupported()
{ return Result<std::unique_ptr<PrefabFileTransaction>>::Fail(Error::Io, {}, "prefab transactions require Windows NTFS"); }
Result<std::unique_ptr<PrefabFileTransaction>> PrefabFileTransaction::Begin(std::filesystem::path, std::filesystem::path, bool) { return Unsupported(); }
Result<PrefabPairSnapshot> PrefabFileTransaction::InspectCurrent() const { return Result<PrefabPairSnapshot>::Fail(Error::Io, {}, "prefab transactions require Windows NTFS"); }
Result<PrefabPairSnapshot> PrefabFileTransaction::CapturePair() { return Result<PrefabPairSnapshot>::Fail(Error::Io, {}, "prefab transactions require Windows NTFS"); }
Result<void> PrefabFileTransaction::Stage(const std::optional<std::vector<uint8_t>>&, const std::optional<std::vector<uint8_t>>&) { return Result<void>::Fail(Error::Io, {}, "prefab transactions require Windows NTFS"); }
Result<void> PrefabFileTransaction::InstallSidecarThenAsset() { return Result<void>::Fail(Error::Io, {}, "prefab transactions require Windows NTFS"); }
Result<TransactionCommitOutcome> PrefabFileTransaction::Finalize() { return Result<TransactionCommitOutcome>::Fail(Error::Io, {}, "prefab transactions require Windows NTFS"); }
Result<void> PrefabFileTransaction::Rollback() { return Result<void>::Ok(); }
Result<TransactionCommitOutcome> PrefabFileTransaction::ApplySingle(const std::filesystem::path&, bool, const std::vector<uint8_t>&, bool, const std::vector<uint8_t>&, bool)
{ return Result<TransactionCommitOutcome>::Fail(Error::Io, {}, "prefab transactions require Windows NTFS"); }
Result<void> PrefabFileTransaction::RecoverDirectory(const std::filesystem::path&)
{ return Result<void>::Fail(Error::Io, {}, "prefab transactions require Windows NTFS"); }

#endif

} // namespace rt2::core
