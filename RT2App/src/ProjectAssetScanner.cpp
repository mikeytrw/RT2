#include "ProjectAssetScanner.h"

#include "AssetIdentity.h"

#include <algorithm>
#include <system_error>

#ifdef _WIN32
#  include <windows.h>
#endif

namespace rt2::core {

namespace {

constexpr const char* kSidecarSuffix = ".rt2meta";

std::string RelativeUtf8(const std::filesystem::path& path,
                         const std::filesystem::path& root)
{
    std::error_code ec;
    const auto relative = std::filesystem::relative(path, root, ec);
    return (ec ? path.lexically_relative(root) : relative).
        lexically_normal().generic_u8string();
}

bool DiagnosticLess(const AssetDiagnostic& a,
                    const AssetDiagnostic& b)
{
    if (a.severity != b.severity) return a.severity < b.severity;
    if (a.refPath != b.refPath) return a.refPath < b.refPath;
    if (a.sourceKey != b.sourceKey) return a.sourceKey < b.sourceKey;
    return a.detail < b.detail;
}

AssetDiagnostic AdaptDatabaseDiagnostic(
    const AssetDatabaseDiagnostic& source)
{
    AssetDiagnostic result;
    result.severity = AssetDiagnostic::Conflict;
    result.kind = AssetKind::Unknown;
    result.refPath = source.sourcePath;
    result.detail = source.detail;
    if (!source.candidatePaths.empty())
    {
        result.detail += " candidates=";
        for (size_t i = 0; i < source.candidatePaths.size(); ++i)
        {
            if (i) result.detail += ",";
            result.detail += source.candidatePaths[i];
        }
    }
    return result;
}

bool IsReparsePoint(const std::filesystem::path& path)
{
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    std::error_code ec;
    return std::filesystem::is_symlink(
        std::filesystem::symlink_status(path, ec));
#endif
}

} // namespace

bool ScanProjectAssets(const std::filesystem::path& assetRoot,
                       ProjectAssetScanResult& out,
                       Error& err)
{
    err = Error{};
    if (assetRoot.empty() || !assetRoot.is_absolute())
    {
        err.code = Error::InvalidArgument;
        err.path = assetRoot.u8string();
        err.detail = "project asset root must be absolute";
        return false;
    }

    std::error_code ec;
    const auto root = std::filesystem::weakly_canonical(assetRoot, ec);
    if (ec || !std::filesystem::is_directory(root, ec))
    {
        err.code = Error::MissingAsset;
        err.path = assetRoot.u8string();
        err.detail = "project asset root does not exist or is not a directory";
        return false;
    }
    if (IsReparsePoint(assetRoot))
    {
        err.code = Error::InvalidArgument;
        err.path = assetRoot.u8string();
        err.detail = "project asset root may not be a link/reparse point";
        return false;
    }

    std::vector<std::filesystem::path> sidecars;
    std::vector<AssetDiagnostic> scanDiagnostics;
    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    if (ec)
    {
        err.code = Error::Io;
        err.path = root.u8string();
        err.detail = "failed to enumerate asset root: " + ec.message();
        return false;
    }

    while (iterator != end)
    {
        const auto entry = *iterator;
        const auto status = entry.symlink_status(ec);
        if (ec)
        {
            err.code = Error::Io;
            err.path = entry.path().u8string();
            err.detail = "failed to inspect asset entry: " + ec.message();
            return false;
        }
        const bool directory = entry.is_directory(ec);
        if (ec)
        {
            err.code = Error::Io;
            err.path = entry.path().u8string();
            err.detail = "failed to classify asset entry: " + ec.message();
            return false;
        }
        if ((std::filesystem::is_symlink(status) ||
             IsReparsePoint(entry.path())) && directory)
        {
            iterator.disable_recursion_pending();
            AssetDiagnostic diagnostic;
            diagnostic.severity = AssetDiagnostic::Stale;
            diagnostic.kind = AssetKind::Unknown;
            diagnostic.refPath = RelativeUtf8(entry.path(), root);
            diagnostic.detail = "directory link/reparse point was not traversed";
            scanDiagnostics.push_back(std::move(diagnostic));
        }
        else if (entry.is_regular_file(ec) &&
                 entry.path().filename().u8string().size() >
                     std::char_traits<char>::length(kSidecarSuffix))
        {
            const std::string filename = entry.path().filename().u8string();
            if (filename.size() >= std::char_traits<char>::length(kSidecarSuffix) &&
                filename.compare(filename.size() -
                                     std::char_traits<char>::length(kSidecarSuffix),
                                 std::char_traits<char>::length(kSidecarSuffix),
                                 kSidecarSuffix) == 0)
                sidecars.push_back(entry.path());
        }
        iterator.increment(ec);
        if (ec)
        {
            err.code = Error::Io;
            err.path = root.u8string();
            err.detail = "failed while enumerating asset root: " + ec.message();
            return false;
        }
    }

    std::sort(sidecars.begin(), sidecars.end(),
              [&](const auto& a, const auto& b) {
                  return RelativeUtf8(a, root) < RelativeUtf8(b, root);
              });

    std::vector<AssetRecord> records;
    for (const auto& sidecar : sidecars)
    {
        std::string sourceString = sidecar.u8string();
        sourceString.resize(sourceString.size() -
                            std::char_traits<char>::length(kSidecarSuffix));
        const auto source = std::filesystem::u8path(sourceString);
        const std::string sourceRelative = RelativeUtf8(source, root);
        const std::string sidecarRelative = RelativeUtf8(sidecar, root);
        if (!std::filesystem::is_regular_file(source, ec) || ec)
        {
            AssetDiagnostic diagnostic;
            diagnostic.severity = AssetDiagnostic::Stale;
            diagnostic.kind = AssetKind::Unknown;
            diagnostic.refPath = sourceRelative;
            diagnostic.sourceKey = sidecarRelative;
            diagnostic.detail = "sidecar has no regular source asset";
            scanDiagnostics.push_back(std::move(diagnostic));
            ec.clear();
            continue;
        }

        Error readError;
        const UUID id = ReadSidecarId(sidecar, readError);
        if (!readError.IsOk())
        {
            AssetDiagnostic diagnostic;
            diagnostic.severity = AssetDiagnostic::Malformed;
            diagnostic.kind = AssetKind::Unknown;
            diagnostic.refPath = sourceRelative;
            diagnostic.sourceKey = sidecarRelative;
            diagnostic.detail = readError.detail;
            scanDiagnostics.push_back(std::move(diagnostic));
            continue;
        }
        if (id.IsNull())
        {
            AssetDiagnostic diagnostic;
            diagnostic.severity = AssetDiagnostic::Malformed;
            diagnostic.kind = AssetKind::Unknown;
            diagnostic.refPath = sourceRelative;
            diagnostic.sourceKey = sidecarRelative;
            diagnostic.detail = "sidecar contains no usable asset ID";
            scanDiagnostics.push_back(std::move(diagnostic));
            continue;
        }

        AssetRecord record;
        record.assetId = id;
        record.sourcePath = sourceRelative;
        record.identityAuthority = AssetIdentityAuthority::Sidecar;
        records.push_back(std::move(record));
    }

    std::vector<AssetDatabaseDiagnostic> databaseDiagnostics;
    AssetDatabase database = BuildAssetDatabase(
        std::move(records), databaseDiagnostics);
    for (const auto& diagnostic : databaseDiagnostics)
        scanDiagnostics.push_back(AdaptDatabaseDiagnostic(diagnostic));
    std::sort(scanDiagnostics.begin(), scanDiagnostics.end(), DiagnosticLess);

    ProjectAssetScanResult result;
    result.database = std::make_shared<const AssetDatabase>(std::move(database));
    result.diagnostics = std::move(scanDiagnostics);
    out = std::move(result);
    return true;
}

} // namespace rt2::core
