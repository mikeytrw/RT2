#include "AssetDatabase.h"

#include <algorithm>
#include <tuple>

namespace rt2::core {

namespace {

bool DependencyLess(const AssetDependencyRecord& a,
                    const AssetDependencyRecord& b)
{
    if (a.sourceKey != b.sourceKey)
        return a.sourceKey < b.sourceKey;
    if (a.sourcePath != b.sourcePath)
        return a.sourcePath < b.sourcePath;
    if (a.assetId != b.assetId)
        return a.assetId < b.assetId;
    return static_cast<uint8_t>(a.kind) < static_cast<uint8_t>(b.kind);
}

void NormalizeRecord(AssetRecord& record)
{
    record.observedKinds.erase(
        std::remove(record.observedKinds.begin(),
                    record.observedKinds.end(),
                    AssetKind::Unknown),
        record.observedKinds.end());
    std::sort(record.observedKinds.begin(), record.observedKinds.end(),
              [](AssetKind a, AssetKind b) {
                  return static_cast<uint8_t>(a) < static_cast<uint8_t>(b);
              });
    record.observedKinds.erase(
        std::unique(record.observedKinds.begin(), record.observedKinds.end()),
        record.observedKinds.end());

    std::sort(record.dependentEntities.begin(), record.dependentEntities.end());
    record.dependentEntities.erase(
        std::unique(record.dependentEntities.begin(),
                    record.dependentEntities.end()),
        record.dependentEntities.end());

    std::sort(record.dependencies.begin(), record.dependencies.end(),
              DependencyLess);
    record.dependencies.erase(
        std::unique(record.dependencies.begin(), record.dependencies.end()),
        record.dependencies.end());
}

template<typename T, typename Less>
void MergeSortedUnique(std::vector<T>& destination,
                       const std::vector<T>& source,
                       Less less)
{
    destination.insert(destination.end(), source.begin(), source.end());
    std::sort(destination.begin(), destination.end(), less);
    destination.erase(
        std::unique(destination.begin(), destination.end()),
        destination.end());
}

std::string JoinCandidatePaths(const std::vector<std::string>& paths)
{
    std::string result;
    for (size_t i = 0; i < paths.size(); ++i)
    {
        if (i != 0)
            result += ", ";
        result += "\"" + paths[i] + "\"";
    }
    return result;
}

bool RecordLess(const AssetRecord& a, const AssetRecord& b)
{
    if (a.sourcePath != b.sourcePath)
        return a.sourcePath < b.sourcePath;
    if (a.identityAuthority != b.identityAuthority)
    {
        return static_cast<uint8_t>(a.identityAuthority) <
               static_cast<uint8_t>(b.identityAuthority);
    }
    if (a.assetId != b.assetId)
        return a.assetId < b.assetId;
    if (a.observedKinds != b.observedKinds)
    {
        return std::lexicographical_compare(
            a.observedKinds.begin(), a.observedKinds.end(),
            b.observedKinds.begin(), b.observedKinds.end(),
            [](AssetKind left, AssetKind right) {
                return static_cast<uint8_t>(left) <
                       static_cast<uint8_t>(right);
            });
    }

    const auto settingsA = std::tie(
        a.importSettings.triangulate,
        a.importSettings.generateNormals,
        a.importSettings.mergeMegaMesh);
    const auto settingsB = std::tie(
        b.importSettings.triangulate,
        b.importSettings.generateNormals,
        b.importSettings.mergeMegaMesh);
    if (settingsA != settingsB)
        return settingsA < settingsB;

    if (a.dependentEntities != b.dependentEntities)
    {
        return std::lexicographical_compare(
            a.dependentEntities.begin(), a.dependentEntities.end(),
            b.dependentEntities.begin(), b.dependentEntities.end());
    }

    return std::lexicographical_compare(
        a.dependencies.begin(), a.dependencies.end(),
        b.dependencies.begin(), b.dependencies.end(), DependencyLess);
}

void AppendDuplicateIdDiagnostic(
    const UUID& id,
    const std::vector<std::string>& claimants,
    std::vector<AssetDatabaseDiagnostic>& diagnostics)
{
    AssetDatabaseDiagnostic diagnostic;
    diagnostic.kind = AssetDatabaseDiagnostic::Kind::DuplicateId;
    diagnostic.assetId = id;
    diagnostic.sourcePath = claimants.empty()
        ? std::string{}
        : claimants.front();
    diagnostic.candidatePaths = claimants;
    diagnostic.detail = "asset ID " + id.ToString() +
                        " is claimed by multiple paths: " +
                        JoinCandidatePaths(claimants);
    diagnostics.push_back(std::move(diagnostic));
}

void InsertIdClaim(
    std::unordered_map<UUID, std::vector<std::string>>& byId,
    const UUID& id,
    const std::string& sourcePath,
    std::vector<AssetDatabaseDiagnostic>& diagnostics)
{
    auto& claimants = byId[id];
    auto claimantIt = std::lower_bound(
        claimants.begin(), claimants.end(), sourcePath);
    if (claimantIt == claimants.end() || *claimantIt != sourcePath)
        claimants.insert(claimantIt, sourcePath);

    if (claimants.size() > 1)
        AppendDuplicateIdDiagnostic(id, claimants, diagnostics);
}

void RemoveIdClaim(
    std::unordered_map<UUID, std::vector<std::string>>& byId,
    const UUID& id,
    const std::string& sourcePath)
{
    auto idIt = byId.find(id);
    if (idIt == byId.end())
        return;
    auto& claimants = idIt->second;
    auto claimantIt = std::lower_bound(
        claimants.begin(), claimants.end(), sourcePath);
    if (claimantIt != claimants.end() && *claimantIt == sourcePath)
        claimants.erase(claimantIt);
    if (claimants.empty())
        byId.erase(idIt);
}

} // namespace

void AssetDatabase::AddOrUpdate(
    const AssetRecord& record,
    std::vector<AssetDatabaseDiagnostic>& diagnostics)
{
    AssetRecord normalized = record;
    NormalizeRecord(normalized);

    auto pathIt = m_ByPath.find(normalized.sourcePath);
    if (pathIt == m_ByPath.end())
    {
        auto inserted = m_ByPath.emplace(
            normalized.sourcePath, std::move(normalized));
        AssetRecord& stored = inserted.first->second;

        if (!stored.assetId.IsNull())
            InsertIdClaim(
                m_ById, stored.assetId, stored.sourcePath, diagnostics);
        return;
    }

    AssetRecord& existing = pathIt->second;
    if (existing.assetId.IsNull() && !normalized.assetId.IsNull())
    {
        existing.assetId = normalized.assetId;
        existing.identityAuthority = normalized.identityAuthority;
        InsertIdClaim(
            m_ById, existing.assetId, existing.sourcePath, diagnostics);
    }
    else if (!normalized.assetId.IsNull() &&
             normalized.assetId != existing.assetId)
    {
        const bool incomingWins =
            normalized.identityAuthority >
            existing.identityAuthority;
        AssetDatabaseDiagnostic diagnostic;
        diagnostic.kind = AssetDatabaseDiagnostic::Kind::ConflictingId;
        diagnostic.assetId = normalized.assetId;
        diagnostic.sourcePath = normalized.sourcePath;
        diagnostic.detail = "assetId " + normalized.assetId.ToString() +
                            " disagrees with stored " +
                            (existing.assetId.IsNull()
                                ? std::string("nil")
                                : existing.assetId.ToString()) +
                            " for \"" + normalized.sourcePath + "\"; " +
                            (incomingWins
                                ? "using the authoritative incoming ID"
                                : "keeping the stored ID");
        diagnostics.push_back(std::move(diagnostic));

        if (incomingWins)
        {
            if (!existing.assetId.IsNull())
            {
                RemoveIdClaim(
                    m_ById, existing.assetId, existing.sourcePath);
            }
            existing.assetId = normalized.assetId;
            existing.identityAuthority =
                normalized.identityAuthority;
            InsertIdClaim(
                m_ById, existing.assetId,
                existing.sourcePath, diagnostics);
        }
    }
    else if (normalized.assetId == existing.assetId &&
             normalized.identityAuthority >
                 existing.identityAuthority)
    {
        existing.identityAuthority =
            normalized.identityAuthority;
    }

    // BuildAssetDatabase inserts records in a deterministic total order, so
    // the last settings value is deterministic even for duplicate paths.
    existing.importSettings = normalized.importSettings;
    MergeSortedUnique(
        existing.observedKinds, normalized.observedKinds,
        [](AssetKind a, AssetKind b) {
            return static_cast<uint8_t>(a) < static_cast<uint8_t>(b);
        });
    MergeSortedUnique(
        existing.dependentEntities, normalized.dependentEntities,
        [](const UUID& a, const UUID& b) { return a < b; });
    MergeSortedUnique(
        existing.dependencies, normalized.dependencies, DependencyLess);
}

void AssetDatabase::AddEntityDependency(
    const std::string& sourcePath,
    const UUID& dependentEntity)
{
    auto& record = m_ByPath[sourcePath];
    record.sourcePath = sourcePath;
    auto it = std::lower_bound(
        record.dependentEntities.begin(),
        record.dependentEntities.end(),
        dependentEntity);
    if (it == record.dependentEntities.end() || *it != dependentEntity)
        record.dependentEntities.insert(it, dependentEntity);
}

void AssetDatabase::AddAssetDependency(
    const std::string& ownerSourcePath,
    const AssetDependencyRecord& dependency,
    std::vector<AssetDatabaseDiagnostic>& diagnostics)
{
    auto& owner = m_ByPath[ownerSourcePath];
    owner.sourcePath = ownerSourcePath;
    auto dependencyIt = std::lower_bound(
        owner.dependencies.begin(), owner.dependencies.end(),
        dependency, DependencyLess);
    if (dependencyIt == owner.dependencies.end() ||
        !(*dependencyIt == dependency))
    {
        owner.dependencies.insert(dependencyIt, dependency);
    }

    if (!dependency.sourcePath.empty())
    {
        AssetRecord target;
        target.assetId = dependency.assetId;
        target.sourcePath = dependency.sourcePath;
        if (dependency.kind != AssetKind::Unknown)
            target.observedKinds.push_back(dependency.kind);
        AddOrUpdate(target, diagnostics);
    }
}

const AssetRecord* AssetDatabase::FindByPath(
    const std::string& sourcePath) const
{
    auto it = m_ByPath.find(sourcePath);
    return it == m_ByPath.end() ? nullptr : &it->second;
}

AssetIdLookupResult AssetDatabase::LookupById(const UUID& assetId) const
{
    AssetIdLookupResult result;
    if (assetId.IsNull())
        return result;

    auto idIt = m_ById.find(assetId);
    if (idIt == m_ById.end() || idIt->second.empty())
        return result;

    result.candidatePaths = idIt->second;
    if (result.candidatePaths.size() > 1)
    {
        result.status = AssetIdLookupResult::Status::Ambiguous;
        return result;
    }

    auto pathIt = m_ByPath.find(result.candidatePaths.front());
    if (pathIt == m_ByPath.end())
    {
        result.candidatePaths.clear();
        return result;
    }

    result.status = AssetIdLookupResult::Status::Unique;
    result.record = &pathIt->second;
    return result;
}

std::vector<AssetDependencyRecord>
AssetDatabase::FindDependenciesBySourceKey(
    const std::string& ownerSourcePath,
    const std::string& sourceKey) const
{
    std::vector<AssetDependencyRecord> result;
    const AssetRecord* owner = FindByPath(ownerSourcePath);
    if (!owner)
        return result;

    auto begin = std::lower_bound(
        owner->dependencies.begin(), owner->dependencies.end(), sourceKey,
        [](const AssetDependencyRecord& dependency,
           const std::string& key) {
            return dependency.sourceKey < key;
        });
    for (auto it = begin;
         it != owner->dependencies.end() && it->sourceKey == sourceKey;
         ++it)
    {
        result.push_back(*it);
    }
    return result;
}

std::vector<AssetRecord> AssetDatabase::AllRecordsSorted() const
{
    std::vector<AssetRecord> result;
    result.reserve(m_ByPath.size());
    for (const auto& entry : m_ByPath)
        result.push_back(entry.second);

    std::sort(result.begin(), result.end(),
              [](const AssetRecord& a, const AssetRecord& b) {
                  return a.sourcePath < b.sourcePath;
              });
    return result;
}

AssetDatabase BuildAssetDatabase(
    std::vector<AssetRecord> records,
    std::vector<AssetDatabaseDiagnostic>& diagnostics)
{
    for (auto& record : records)
        NormalizeRecord(record);
    std::sort(records.begin(), records.end(), RecordLess);

    AssetDatabase database;
    for (const auto& record : records)
        database.AddOrUpdate(record, diagnostics);
    return database;
}

} // namespace rt2::core
