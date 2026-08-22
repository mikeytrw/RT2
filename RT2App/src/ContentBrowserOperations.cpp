#include "ContentBrowserOperations.h"

#include "SceneAssetReferenceVisitor.h"
#include "SceneDocument.h"

#include <algorithm>
#include <cctype>
#include <system_error>

#ifdef _WIN32
#  include <windows.h>
#endif

namespace rt2::core {
namespace {

std::string PathString(const std::filesystem::path& path)
{
    return path.u8string();
}

std::string Fold(std::string value)
{
#ifdef _WIN32
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
#endif
    return value;
}

bool HasDotDotComponent(const std::filesystem::path& path)
{
    for (const auto& component : path)
    {
        if (component == std::filesystem::path(".."))
            return true;
    }
    return false;
}

std::filesystem::path AbsoluteLexical(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(path, ec);
    return (ec ? path : absolute).lexically_normal();
}

bool IsContained(const std::filesystem::path& root,
                 const std::filesystem::path& candidate)
{
    const auto absoluteRoot = AbsoluteLexical(root);
    const auto absoluteCandidate = AbsoluteLexical(candidate);
    if (absoluteRoot.root_name() != absoluteCandidate.root_name())
        return false;

    const auto relative = absoluteCandidate.lexically_relative(absoluteRoot);
    if (relative.empty())
        return true;
    return !HasDotDotComponent(relative);
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

bool HasReparsePointInExistingPath(const std::filesystem::path& root,
                                   const std::filesystem::path& candidate)
{
    auto current = AbsoluteLexical(candidate);
    const auto absoluteRoot = AbsoluteLexical(root);
    while (true)
    {
        std::error_code ec;
        if (std::filesystem::exists(current, ec) &&
            !ec && IsReparsePoint(current))
            return true;
        if (current == absoluteRoot)
            return false;
        const auto parent = current.parent_path();
        if (parent == current)
            return false;
        current = parent;
    }
}

bool IsRegularFile(const std::filesystem::path& path, Error& error)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec))
    {
        error.code = ec ? Error::Io : Error::MissingAsset;
        error.path = PathString(path);
        error.detail = ec ? "failed to inspect asset file: " + ec.message()
                          : "asset file is missing or not regular";
        return false;
    }
    return true;
}

bool IsDirectoryOrMissing(const std::filesystem::path& path,
                          Error& error)
{
    std::error_code ec;
    if (std::filesystem::exists(path, ec) && !ec)
    {
        if (std::filesystem::is_directory(path, ec) && !ec)
            return true;
        error.code = ec ? Error::Io : Error::InvalidArgument;
        error.path = PathString(path);
        error.detail = ec ? "failed to inspect destination directory: " +
                                  ec.message()
                          : "destination path is not a directory";
        return false;
    }
    if (ec)
    {
        error.code = Error::Io;
        error.path = PathString(path);
        error.detail = "failed to inspect destination directory: " +
                       ec.message();
        return false;
    }
    return true;
}

void AddDiagnostic(ContentBrowserOperationReport& report,
                   AssetDiagnostic::Severity severity,
                   AssetKind kind,
                   const std::filesystem::path& refPath,
                   const std::filesystem::path& resolvedPath,
                   std::string detail)
{
    AssetDiagnostic diagnostic;
    diagnostic.severity = severity;
    diagnostic.kind = kind;
    diagnostic.refPath = PathString(refPath);
    diagnostic.resolvedPath = PathString(resolvedPath);
    diagnostic.detail = std::move(detail);
    report.diagnostics.push_back(std::move(diagnostic));
}

bool Fail(ContentBrowserOperationReport& report,
          Error& error,
          Error::Code code,
          const std::filesystem::path& path,
          std::string detail)
{
    error.code = code;
    error.path = PathString(path);
    error.detail = std::move(detail);
    return false;
}

bool ValidateAssetPair(const std::filesystem::path& assetRoot,
                       const AssetRecord& record,
                       std::filesystem::path& source,
                       std::filesystem::path& sidecar,
                       ContentBrowserOperationReport& report,
                       Error& error)
{
    error = Error{};
    if (assetRoot.empty() || !assetRoot.is_absolute() ||
        record.sourcePath.empty())
        return Fail(report, error, Error::InvalidArgument, assetRoot,
                    "content-browser asset root and source path are required");

    const auto relative = std::filesystem::u8path(record.sourcePath);
    if (relative.is_absolute() || HasDotDotComponent(relative))
        return Fail(report, error, Error::InvalidArgument, relative,
                    "asset source path must be a contained portable path");

    source = AbsoluteLexical(assetRoot / relative);
    sidecar = AssetSidecarPath(source);
    if (!IsContained(assetRoot, source) || !IsContained(assetRoot, sidecar))
        return Fail(report, error, Error::InvalidArgument, source,
                    "asset source and sidecar must remain inside the asset root");
    if (HasReparsePointInExistingPath(assetRoot, source) ||
        HasReparsePointInExistingPath(assetRoot, sidecar))
        return Fail(report, error, Error::InvalidArgument, source,
                    "asset source and sidecar may not traverse links/reparse points");
    if (!IsRegularFile(source, error) || !IsRegularFile(sidecar, error))
        return false;
    return true;
}

bool ValidateDestination(const std::filesystem::path& assetRoot,
                         const std::filesystem::path& source,
                         const std::filesystem::path& destination,
                         ContentBrowserOperationReport& report,
                         Error& error)
{
    if (destination.empty() || !destination.is_absolute() ||
        HasDotDotComponent(destination) ||
        !IsContained(assetRoot, destination))
        return Fail(report, error, Error::InvalidArgument, destination,
                    "destination directory must be absolute and contained in the asset root");
    if (source.root_name() != destination.root_name())
        return Fail(report, error, Error::InvalidArgument, destination,
                    "cross-volume asset moves are not supported");
    if (HasReparsePointInExistingPath(assetRoot, destination))
        return Fail(report, error, Error::InvalidArgument, destination,
                    "destination directory may not traverse a link/reparse point");
    if (!IsDirectoryOrMissing(destination, error))
        return false;
    if (AbsoluteLexical(source.parent_path()) == AbsoluteLexical(destination))
        return Fail(report, error, Error::InvalidArgument, destination,
                    "destination directory is the asset's current directory");
    return true;
}

bool DestinationCollides(const std::filesystem::path& source,
                         const std::filesystem::path& sidecar,
                         const std::filesystem::path& newSource,
                         const std::filesystem::path& newSidecar,
                         ContentBrowserOperationReport& report,
                         Error& error)
{
    std::error_code ec;
    if (std::filesystem::exists(newSource, ec) ||
        std::filesystem::exists(newSidecar, ec))
        return Fail(report, error, Error::InvalidArgument, newSource,
                    "destination source or sidecar already exists (Conflict)");
    if (ec)
        return Fail(report, error, Error::Io, newSource,
                    "failed to inspect destination collision: " + ec.message());
    if (newSource == source || newSidecar == sidecar)
        return Fail(report, error, Error::InvalidArgument, newSource,
                    "destination is the existing asset pair");
    return true;
}

bool MoveFile(const ContentBrowserIoHooks& hooks,
              const std::filesystem::path& from,
              const std::filesystem::path& to,
              Error& error)
{
    if (hooks.moveFile)
        return hooks.moveFile(from, to, error);
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    if (ec)
    {
        error.code = Error::Io;
        error.path = PathString(from);
        error.detail = "failed to move asset: " + ec.message();
        return false;
    }
    return true;
}

bool RemoveFile(const ContentBrowserIoHooks& hooks,
                const std::filesystem::path& path,
                Error& error)
{
    if (hooks.removeFile)
        return hooks.removeFile(path, error);
    std::error_code ec;
    if (!std::filesystem::remove(path, ec))
    {
        error.code = ec ? Error::Io : Error::MissingAsset;
        error.path = PathString(path);
        error.detail = ec ? "failed to delete asset: " + ec.message()
                          : "asset was not present";
        return false;
    }
    return true;
}

bool CreateDirectories(const ContentBrowserIoHooks& hooks,
                       const std::filesystem::path& path,
                       Error& error)
{
    if (hooks.createDirectories)
        return hooks.createDirectories(path, error);
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec)
    {
        error.code = Error::Io;
        error.path = PathString(path);
        error.detail = "failed to create destination directory: " +
                       ec.message();
        return false;
    }
    return true;
}

bool ExecutePairMove(const std::filesystem::path& oldSource,
                     const std::filesystem::path& oldSidecar,
                     const std::filesystem::path& newSource,
                     const std::filesystem::path& newSidecar,
                     ContentBrowserOperationReport& report,
                     Error& error,
                     const ContentBrowserIoHooks& hooks)
{
    if (!MoveFile(hooks, oldSource, newSource, error))
        return false;
    report.changed = true;
    if (MoveFile(hooks, oldSidecar, newSidecar, error))
        return true;

    report.partialFailure = true;
    AddDiagnostic(report, AssetDiagnostic::Conflict, AssetKind::Unknown,
                  oldSidecar, oldSidecar,
                  "source moved but sidecar move failed; old sidecar remains and no rollback was attempted");
    return false;
}

std::string ReferenceKey(const std::string& referencePath,
                         const std::filesystem::path& assetRoot)
{
    const auto path = std::filesystem::u8path(referencePath);
    if (path.is_absolute() && IsContained(assetRoot, path))
        return Fold(path.lexically_relative(AbsoluteLexical(assetRoot)).generic_string());
    return Fold(path.lexically_normal().generic_string());
}

} // namespace

bool ContentBrowserCanOperate(bool projectActive)
{
    return projectActive;
}

bool DispatchContentBrowserAssetDrop(
    std::string_view path,
    const ContentBrowserDropCallbacks& callbacks,
    Error& error)
{
    error = Error{};
    if (path.empty())
    {
        error.code = Error::InvalidArgument;
        error.detail = "content-browser asset drop path is required";
        return false;
    }

    const std::string pathString(path);
    const std::string extension =
        Fold(std::filesystem::u8path(pathString).extension().u8string());
    if (extension == ".obj")
    {
        if (!callbacks.importObj)
        {
            error.code = Error::InvalidArgument;
            error.path = pathString;
            error.detail = "OBJ drop has no import callback";
            return false;
        }
        callbacks.importObj(pathString, ImportSettings{});
        return true;
    }
    if (extension == ".glb" || extension == ".gltf")
    {
        if (!callbacks.importGltf)
        {
            error.code = Error::InvalidArgument;
            error.path = pathString;
            error.detail = "glTF drop has no import callback";
            return false;
        }
        callbacks.importGltf(pathString);
        return true;
    }

    error.code = Error::InvalidArgument;
    error.path = pathString;
    error.detail = "unsupported content-browser asset drop extension";
    return false;
}

bool ContentBrowserDeleteAllowed(bool confirmed, size_t dependantCount)
{
    (void)dependantCount;
    // Dependants are a confirmation warning, not a hard prohibition. The
    // explicit confirmation is what lets the user intentionally remove an
    // asset and repair its references later.
    return confirmed;
}

std::vector<AssetRecord> SearchContentBrowserAssets(
    const AssetDatabase& database, std::string_view query)
{
    const std::string foldedQuery = Fold(std::string(query));
    std::vector<AssetRecord> result;
    for (const auto& record : database.AllRecordsSorted())
    {
        if (foldedQuery.empty() ||
            Fold(record.sourcePath).find(foldedQuery) != std::string::npos ||
            Fold(record.assetId.ToString()).find(foldedQuery) != std::string::npos)
            result.push_back(record);
    }
    return result;
}

std::vector<ContentBrowserDependant> FindContentBrowserDependants(
    const SceneDocument& document,
    const AssetRecord& record,
    const std::filesystem::path& assetRoot)
{
    std::vector<ContentBrowserDependant> result;
    const std::string recordPath =
        ReferenceKey(record.sourcePath, assetRoot);
    for (const auto& slot : CollectSceneAssetReferences(document))
    {
        if (!slot.reference)
            continue;
        const bool matchesId = !record.assetId.IsNull() &&
                               !slot.reference->assetId.IsNull() &&
                               slot.reference->assetId == record.assetId;
        // This is an advisory safety query, not the resolver: when IDs are
        // absent or disagree, the same source path is still worth reporting
        // so the delete confirmation cannot under-report a dependant.
        const bool matchesPath = !matchesId &&
            ReferenceKey(slot.reference->path, assetRoot) == recordPath;
        if (!matchesId && !matchesPath)
            continue;
        result.push_back(ContentBrowserDependant{
            slot.entityUuid,
            slot.entityName,
            slot.reference->kind,
            slot.reference->sourceKey,
            slot.reference->path});
    }
    return result;
}

bool RenameContentBrowserAsset(
    const std::filesystem::path& assetRoot,
    const AssetRecord& record,
    std::string_view newName,
    ContentBrowserOperationReport& report,
    Error& error,
    const ContentBrowserIoHooks& hooks)
{
    report = ContentBrowserOperationReport{};
    std::filesystem::path source;
    std::filesystem::path sidecar;
    if (!ValidateAssetPair(assetRoot, record, source, sidecar, report, error))
        return false;

    const std::string name(newName);
    if (name.empty() || name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos || name.find("..") != std::string::npos)
        return Fail(report, error, Error::InvalidArgument,
                    std::filesystem::u8path(name),
                    "new asset name must be a single filename without separators or '..'");
    const auto newNamePath = std::filesystem::u8path(name);
    if (newNamePath.filename() != newNamePath ||
        Fold(newNamePath.extension().u8string()) !=
            Fold(source.extension().u8string()))
        return Fail(report, error, Error::InvalidArgument, newNamePath,
                    "renaming an asset may not change its extension");

    const auto newSource = source.parent_path() / newNamePath;
    const auto newSidecar = AssetSidecarPath(newSource);
    if (!DestinationCollides(source, sidecar, newSource, newSidecar,
                             report, error))
        return false;
    return ExecutePairMove(source, sidecar, newSource, newSidecar,
                           report, error, hooks);
}

bool MoveContentBrowserAsset(
    const std::filesystem::path& assetRoot,
    const AssetRecord& record,
    const std::filesystem::path& destinationDirectory,
    ContentBrowserOperationReport& report,
    Error& error,
    const ContentBrowserIoHooks& hooks)
{
    report = ContentBrowserOperationReport{};
    std::filesystem::path source;
    std::filesystem::path sidecar;
    if (!ValidateAssetPair(assetRoot, record, source, sidecar, report, error))
        return false;
    if (!ValidateDestination(assetRoot, source, destinationDirectory,
                             report, error))
        return false;

    const auto newSource = destinationDirectory / source.filename();
    const auto newSidecar = AssetSidecarPath(newSource);
    if (!DestinationCollides(source, sidecar, newSource, newSidecar,
                             report, error))
        return false;
    if (!std::filesystem::exists(destinationDirectory))
    {
        if (!CreateDirectories(hooks, destinationDirectory, error))
            return false;
    }
    return ExecutePairMove(source, sidecar, newSource, newSidecar,
                           report, error, hooks);
}

bool DeleteContentBrowserAsset(
    const std::filesystem::path& assetRoot,
    const AssetRecord& record,
    ContentBrowserOperationReport& report,
    Error& error,
    const ContentBrowserIoHooks& hooks)
{
    report = ContentBrowserOperationReport{};
    std::filesystem::path source;
    std::filesystem::path sidecar;
    if (!ValidateAssetPair(assetRoot, record, source, sidecar, report, error))
        return false;

    // W6-A1: source first leaves a clean abort when the source is locked, and
    // makes a sidecar-delete failure visibly stale instead of identity loss.
    if (!RemoveFile(hooks, source, error))
        return false;
    report.changed = true;
    if (RemoveFile(hooks, sidecar, error))
        return true;

    report.partialFailure = true;
    AddDiagnostic(report, AssetDiagnostic::Conflict, AssetKind::Unknown,
                  sidecar, sidecar,
                  "source deleted but orphaned sidecar remains at " +
                      PathString(sidecar) + "; clean it up before reusing this path");
    return false;
}

bool ReimportContentBrowserAsset(
    const std::filesystem::path& assetRoot,
    const AssetRecord& record,
    const ContentBrowserReimportCallback& reimport,
    ContentBrowserOperationReport& report,
    Error& error)
{
    report = ContentBrowserOperationReport{};
    if (!reimport)
        return Fail(report, error, Error::InvalidArgument, assetRoot,
                    "reimport callback is required");

    std::filesystem::path source;
    std::filesystem::path sidecar;
    if (!ValidateAssetPair(assetRoot, record, source, sidecar, report, error))
        return false;
    Error readError;
    const UUID originalId = ReadSidecarId(sidecar, readError);
    if (!readError.IsOk() || originalId.IsNull())
    {
        error = readError;
        if (error.IsOk())
        {
            error.code = Error::Parse;
            error.path = PathString(sidecar);
            error.detail = "reimport requires a valid non-nil sidecar ID";
        }
        return false;
    }
    const std::string extension = Fold(source.extension().u8string());
    if (extension != ".glb" && extension != ".gltf" && extension != ".obj")
    {
        if (extension != ".rt2prefab")
        return Fail(report, error, Error::InvalidArgument, source,
                    "content-browser reimport supports only .glb, .gltf and .obj");
    }

    if (!reimport(record, source, report.diagnostics, error))
        return false;

    Error afterError;
    const UUID afterId = ReadSidecarId(sidecar, afterError);
    if (!afterError.IsOk() || afterId != originalId)
    {
        Error restoreError;
        const bool restored = WriteSidecarId(sidecar, originalId, restoreError);
        error.code = Error::InvalidArgument;
        error.path = PathString(sidecar);
        error.detail = "reimport changed the durable asset ID";
        if (!restored)
            error.detail += "; failed to restore the original sidecar ID: " +
                             restoreError.detail;
        AddDiagnostic(report, AssetDiagnostic::Conflict, record.observedKinds.empty()
                          ? AssetKind::Unknown : record.observedKinds.front(),
                      sidecar, sidecar, error.detail);
        return false;
    }

    if (!report.noOp)
        report.changed = true;
    return true;
}

} // namespace rt2::core
