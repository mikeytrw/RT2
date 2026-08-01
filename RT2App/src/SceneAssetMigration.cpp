#include "SceneAssetMigration.h"

#include "AssetIdentity.h"
#include "ProjectAssetScanner.h"
#include "SceneAssetReferenceVisitor.h"
#include "SceneSerializer.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <system_error>

namespace rt2::core {
namespace {

std::filesystem::path AbsoluteNormalized(const std::filesystem::path& path)
{
    std::error_code ec;
    auto absolute = std::filesystem::absolute(path, ec);
    if (ec) absolute = path;
    return absolute.lexically_normal();
}

std::filesystem::path PhysicalPathFor(
    const std::filesystem::path& base,
    const std::string& stored)
{
    const auto value = std::filesystem::u8path(stored);
    const auto candidate = value.is_absolute() ? value : base / value;
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec))
    {
        const auto canonical = std::filesystem::weakly_canonical(candidate, ec);
        if (!ec) return canonical.lexically_normal();
    }
    return AbsoluteNormalized(candidate);
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

std::string PathKey(const std::filesystem::path& path)
{
    return Fold(path.lexically_normal().generic_u8string());
}

bool IsContained(const std::filesystem::path& root,
                 const std::filesystem::path& candidate)
{
    auto rootIt = root.begin();
    auto candidateIt = candidate.begin();
    for (; rootIt != root.end(); ++rootIt, ++candidateIt)
    {
        if (candidateIt == candidate.end() ||
            Fold(rootIt->generic_u8string()) !=
                Fold(candidateIt->generic_u8string()))
            return false;
    }
    return true;
}

std::filesystem::path CanonicalRoot(const std::filesystem::path& root)
{
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(root, ec);
    return (ec ? AbsoluteNormalized(root) : canonical).lexically_normal();
}

AssetDiagnostic MakeDiagnostic(const SceneAssetReferenceSlot& slot,
                               AssetDiagnostic::Severity severity,
                               const std::filesystem::path& resolvedPath,
                               std::string detail)
{
    AssetDiagnostic diagnostic;
    diagnostic.severity = severity;
    diagnostic.kind = slot.reference ? slot.reference->kind : AssetKind::Unknown;
    diagnostic.refPath = slot.reference ? slot.reference->path : std::string{};
    diagnostic.resolvedPath = resolvedPath.generic_u8string();
    diagnostic.entityUuid = slot.entityUuid;
    diagnostic.entityName = slot.entityName;
    diagnostic.sourceKey = slot.reference ? slot.reference->sourceKey : std::string{};
    diagnostic.detail = std::move(detail);
    return diagnostic;
}

struct WorkItem
{
    std::string key;
    std::filesystem::path sourcePath;
    std::filesystem::path sidecarPath;
    std::vector<size_t> slotIndices;
    std::vector<UUID> referenceIds;
    UUID sidecarId;
    Error sidecarError;
};

bool SamePath(const std::filesystem::path& left,
              const std::filesystem::path& right)
{
    return PathKey(left) == PathKey(right);
}

bool FailConflict(const WorkItem& item, const std::string& detail, Error& err)
{
    err.code = Error::InvalidArgument;
    err.path = item.sourcePath.u8string();
    err.detail = detail;
    return false;
}

void SortDiagnostics(std::vector<AssetDiagnostic>& diagnostics)
{
    std::stable_sort(diagnostics.begin(), diagnostics.end(),
        [](const AssetDiagnostic& left, const AssetDiagnostic& right) {
            return AssetDiagnosticSortKey(left) <
                   AssetDiagnosticSortKey(right);
        });
}

bool ValidateStagedReferences(
    const std::vector<ConstSceneAssetReferenceSlot>& slots,
    const std::filesystem::path& assetRoot,
    const AssetDatabase& database,
    Error& err)
{
    for (const auto& slot : slots)
    {
        if (!slot.reference || slot.reference->path.empty())
            continue;

        const auto resolved = PhysicalPathFor(assetRoot, slot.reference->path);
        std::error_code sourceError;
        const bool sourceExists =
            std::filesystem::is_regular_file(resolved, sourceError) &&
            !sourceError;
        if (!sourceExists)
        {
            if (slot.reference->assetId.IsNull())
                continue; // Missing sources are an explicit incomplete state.
            err.code = Error::InvalidArgument;
            err.path = resolved.u8string();
            err.detail = "staged asset ID points at a missing source";
            return false;
        }

        if (!IsContained(assetRoot, resolved))
            continue; // NonPortable is already reported and remains incomplete.

        if (slot.reference->assetId.IsNull())
        {
            err.code = Error::InvalidArgument;
            err.path = resolved.u8string();
            err.detail = "staged existing asset has no durable ID";
            return false;
        }

        const auto lookup = database.LookupById(slot.reference->assetId);
        if (lookup.status != AssetIdLookupResult::Status::Unique ||
            !lookup.record)
        {
            err.code = Error::InvalidArgument;
            err.path = resolved.u8string();
            err.detail = "staged asset ID is absent or ambiguous in the "
                         "rebuilt database";
            return false;
        }

        const auto databasePath = assetRoot /
            std::filesystem::u8path(lookup.record->sourcePath);
        if (!SamePath(databasePath, resolved))
        {
            err.code = Error::InvalidArgument;
            err.path = resolved.u8string();
            err.detail = "staged asset ID resolves to a different database path";
            return false;
        }
    }
    return true;
}

} // namespace

bool MigrateSceneAssetReferences(
    const SceneDocument& source,
    SceneDocument& staged,
    const SceneAssetMigrationOptions& options,
    SceneAssetMigrationReport& report,
    Error& err)
{
    err = Error{};
    report = SceneAssetMigrationReport{};
    report.sourceVersion = source.metadata.schemaVersion;

    if (options.assetRoot.empty() || !options.assetRoot.is_absolute())
    {
        err.code = Error::InvalidArgument;
        err.path = options.assetRoot.u8string();
        err.detail = "asset migration requires an absolute asset root";
        return false;
    }
    if (!options.uuidProvider)
    {
        err.code = Error::InvalidArgument;
        err.detail = "asset migration requires an injected UUID provider";
        return false;
    }

    const auto assetRoot = CanonicalRoot(options.assetRoot);
    const auto legacyBase =
        source.metadata.schemaVersion >= SceneSerializer::SchemaVersion &&
        !source.metadata.assetRoot.empty()
            ? CanonicalRoot(source.metadata.assetRoot)
            : (source.metadata.sourcePath.empty()
                ? assetRoot
                : CanonicalRoot(source.metadata.sourcePath.parent_path()));

    const auto sourceSlots = CollectSceneAssetReferences(source);
    report.referenceCount = sourceSlots.size();
    std::map<std::string, WorkItem> workByPath;
    for (size_t index = 0; index < sourceSlots.size(); ++index)
    {
        const auto& slot = sourceSlots[index];
        if (!slot.reference || slot.reference->path.empty()) continue;
        const auto physical = PhysicalPathFor(
            legacyBase, slot.reference->path);
        const std::string key = PathKey(physical);
        auto [it, inserted] = workByPath.emplace(key, WorkItem{});
        WorkItem& work = it->second;
        if (inserted)
        {
            work.key = key;
            work.sourcePath = physical;
            work.sidecarPath = AssetSidecarPath(physical);
            work.sidecarId = ReadSidecarId(work.sidecarPath,
                                           work.sidecarError);
        }
        work.slotIndices.push_back(index);
        if (!slot.reference->assetId.IsNull() &&
            std::find(work.referenceIds.begin(), work.referenceIds.end(),
                      slot.reference->assetId) == work.referenceIds.end())
            work.referenceIds.push_back(slot.reference->assetId);
    }

    // Preflight every identity disagreement before the first sidecar write.
    for (const auto& [key, work] : workByPath)
    {
        (void)key;
        if (work.referenceIds.size() > 1)
            return FailConflict(work,
                "one physical asset has contradictory reference IDs", err);
        if (!work.sidecarId.IsNull() && !work.referenceIds.empty() &&
            work.referenceIds.front() != work.sidecarId)
            return FailConflict(work,
                "reference ID conflicts with authoritative sidecar ID", err);

        auto checkDatabaseId = [&](const UUID& id) -> bool {
            if (!options.existingDatabase || id.IsNull()) return true;
            const auto lookup = options.existingDatabase->LookupById(id);
            if (lookup.status == AssetIdLookupResult::Status::Ambiguous)
            {
                FailConflict(work, "database contains an ambiguous asset ID", err);
                return false;
            }
            if (lookup.status == AssetIdLookupResult::Status::Unique &&
                lookup.record)
            {
                const auto databasePath = assetRoot /
                    std::filesystem::u8path(lookup.record->sourcePath);
                if (!SamePath(databasePath, work.sourcePath))
                {
                    FailConflict(work,
                        "reference ID resolves to a different database path",
                        err);
                    return false;
                }
            }
            return true;
        };
        for (const auto& id : work.referenceIds)
            if (!checkDatabaseId(id)) return false;
        if (!work.sidecarId.IsNull() && !checkDatabaseId(work.sidecarId))
            return false;
    }

    if (!SceneSerializer::CloneInMemory(source, staged, err))
        return false;
    staged.metadata.schemaVersion = SceneSerializer::SchemaVersion;
    staged.metadata.projectId = options.projectId;
    staged.metadata.assetRoot = assetRoot;

    auto stagedSlots = CollectSceneAssetReferences(staged);
    if (stagedSlots.size() != sourceSlots.size())
    {
        err.code = Error::InvalidArgument;
        err.detail = "durable scene reference visitor changed coverage during migration";
        return false;
    }

    for (const auto& [key, work] : workByPath)
    {
        (void)key;
        UUID assigned = work.sidecarId;
        std::error_code sourceError;
        const bool sourceExists =
            std::filesystem::is_regular_file(work.sourcePath, sourceError) &&
            !sourceError;

        if (!sourceExists)
        {
            report.incomplete = true;
            for (const size_t index : work.slotIndices)
            {
                auto& slot = stagedSlots[index];
                slot.reference->assetId = UUID::Nil();
                ++report.unresolvedReferenceCount;
                report.diagnostics.push_back(MakeDiagnostic(
                    slot, AssetDiagnostic::Missing, work.sourcePath,
                    "source asset is missing; migration saved a placeholder"));
            }
        }
        else if (assigned.IsNull())
        {
            bool minted = false;
            Error assignError;
            assigned = ResolveOrAssign(
                work.sourcePath, *options.uuidProvider, minted, assignError);
            if (assigned.IsNull())
            {
                err = assignError;
                if (err.IsOk())
                {
                    err.code = Error::Io;
                    err.path = work.sourcePath.u8string();
                    err.detail = "asset identity assignment returned a nil ID";
                }
                return false;
            }
            Error verifyError;
            const UUID verified = ReadSidecarId(work.sidecarPath, verifyError);
            if (verified.IsNull() || !verifyError.IsOk())
            {
                err = verifyError;
                if (err.IsOk())
                {
                    err.code = Error::Io;
                    err.path = work.sidecarPath.u8string();
                    err.detail = "asset identity assignment was not persisted";
                }
                return false;
            }
            if (minted)
            {
                ++report.createdSidecarCount;
                report.createdSidecars.push_back(work.sidecarPath);
            }
            const auto severity = work.sidecarError.IsOk()
                ? AssetDiagnostic::Stale : AssetDiagnostic::Malformed;
            for (const size_t index : work.slotIndices)
            {
                auto& slot = stagedSlots[index];
                report.diagnostics.push_back(MakeDiagnostic(
                    slot, severity, work.sourcePath,
                    work.sidecarError.IsOk()
                        ? "assigned a new asset ID and wrote its sidecar"
                        : "repaired a malformed asset sidecar"));
            }
        }

        const bool contained = IsContained(assetRoot, work.sourcePath);
        const std::string portablePath = contained
            ? work.sourcePath.lexically_relative(assetRoot).generic_u8string()
            : work.sourcePath.generic_u8string();
        if (!contained)
        {
            report.incomplete = true;
            for (const size_t index : work.slotIndices)
                report.diagnostics.push_back(MakeDiagnostic(
                    stagedSlots[index], AssetDiagnostic::NonPortable,
                    work.sourcePath,
                    "asset is outside the active asset root; retained absolute path"));
        }

        for (const size_t index : work.slotIndices)
        {
            auto& slot = stagedSlots[index];
            if (sourceExists) slot.reference->assetId = assigned;
            slot.reference->path = portablePath;
            if (slot.reference->kind == AssetKind::Script)
                slot.reference->sourceKey = slot.reference->path.empty()
                    ? std::string{}
                    : "lua:asset=" + slot.reference->path;
            ++report.assignedReferenceCount;
        }
    }

    report.requiresPersistence = source.metadata.schemaVersion <
        SceneSerializer::SchemaVersion;
    for (size_t i = 0; i < sourceSlots.size(); ++i)
    {
        if (!sourceSlots[i].reference || !stagedSlots[i].reference) continue;
        if (sourceSlots[i].reference->path != stagedSlots[i].reference->path ||
            sourceSlots[i].reference->assetId != stagedSlots[i].reference->assetId)
            report.requiresPersistence = true;
    }

    ProjectAssetScanResult scan;
    if (!ScanProjectAssets(assetRoot, scan, err))
        return false;
    report.database = std::move(scan.database);
    report.diagnostics.insert(report.diagnostics.end(),
                              scan.diagnostics.begin(), scan.diagnostics.end());
    const auto validationSlots = CollectSceneAssetReferences(
        static_cast<const SceneDocument&>(staged));
    if (!report.database ||
        !ValidateStagedReferences(
            validationSlots, assetRoot, *report.database, err))
        return false;
    SortDiagnostics(report.diagnostics);
    return true;
}

} // namespace rt2::core
