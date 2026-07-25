#include "AssetDatabase.h"

#include <algorithm>

namespace rt2::core {

void AssetDatabase::AddOrUpdate(const AssetRecord& record,
                                 std::vector<AssetDatabaseDiagnostic>& diagnostics)
{
    auto it = m_ByPath.find(record.sourcePath);
    if (it == m_ByPath.end())
    {
        // New record. Check for duplicate ID against an existing path.
        if (!record.assetId.IsNull())
        {
            auto idIt = m_ById.find(record.assetId);
            if (idIt != m_ById.end() && idIt->second != record.sourcePath)
            {
                AssetDatabaseDiagnostic d;
                d.kind = AssetDatabaseDiagnostic::Kind::DuplicateId;
                d.assetId = record.assetId;
                d.sourcePath = record.sourcePath;
                d.detail = "asset ID " + record.assetId.ToString() +
                           " already claimed by \"" + idIt->second +
                           "\"; leaving " + record.sourcePath + " with nil ID";
                diagnostics.push_back(std::move(d));
                AssetRecord sanitized = record;
                sanitized.assetId = UUID::Nil();
                auto inserted = m_ByPath.emplace(record.sourcePath, sanitized);
                (void)inserted;
                return;
            }
            m_ById[record.assetId] = record.sourcePath;
        }
        m_ByPath.emplace(record.sourcePath, record);
        return;
    }

    // Existing record: merge. Preserve the assigned ID unless the new one is
    // non-nil AND conflicts with what's stored.
    AssetRecord& existing = it->second;
    if (!record.assetId.IsNull() && record.assetId != existing.assetId)
    {
        // Conflict: the scene says idA but the sidecar (or an earlier record)
        // says idB. Per D8 the sidecar is the source of truth, so the existing
        // ID wins; record a diagnostic so a stale scene reference is visible.
        AssetDatabaseDiagnostic d;
        d.kind = AssetDatabaseDiagnostic::Kind::ConflictingId;
        d.assetId = record.assetId;
        d.sourcePath = record.sourcePath;
        d.detail = "scene reference assetId " + record.assetId.ToString() +
                   " disagrees with stored " + (existing.assetId.IsNull()
                       ? std::string("nil")
                       : existing.assetId.ToString()) +
                   " for \"" + record.sourcePath + "\"; keeping the stored ID";
        diagnostics.push_back(std::move(d));
    }
    // Update mutable fields (kind/importSettings) from the new record.
    existing.kind = record.kind;
    existing.importSettings = record.importSettings;
}

void AssetDatabase::AddDependency(const std::string& sourcePath,
                                   const UUID& dependentEntity)
{
    auto& rec = m_ByPath[sourcePath]; // creates a placeholder if absent
    rec.sourcePath = sourcePath;
    // Avoid duplicate dependent entries.
    if (std::find(rec.dependents.begin(), rec.dependents.end(),
                  dependentEntity) == rec.dependents.end())
    {
        rec.dependents.push_back(dependentEntity);
    }
}

const AssetRecord* AssetDatabase::FindByPath(const std::string& sourcePath) const
{
    auto it = m_ByPath.find(sourcePath);
    return it == m_ByPath.end() ? nullptr : &it->second;
}

const AssetRecord* AssetDatabase::FindById(const UUID& assetId) const
{
    if (assetId.IsNull()) return nullptr;
    auto idIt = m_ById.find(assetId);
    if (idIt == m_ById.end()) return nullptr;
    auto pathIt = m_ByPath.find(idIt->second);
    return pathIt == m_ByPath.end() ? nullptr : &pathIt->second;
}

std::vector<AssetRecord> AssetDatabase::AllRecordsSorted() const
{
    std::vector<AssetRecord> out;
    out.reserve(m_ByPath.size());
    for (const auto& [path, rec] : m_ByPath)
        out.push_back(rec);
    // Sort by sourcePath for deterministic output, independent of the
    // unordered_map iteration order. This is the ReconcileScriptFields
    // precedent (ScriptFieldReconcile.cpp:199): sort before emit.
    std::sort(out.begin(), out.end(),
              [](const AssetRecord& a, const AssetRecord& b)
              { return a.sourcePath < b.sourcePath; });
    return out;
}

AssetDatabase BuildAssetDatabase(std::vector<AssetRecord> records,
                                  std::vector<AssetDatabaseDiagnostic>& diagnostics)
{
    AssetDatabase db;
    for (auto& r : records)
        db.AddOrUpdate(r, diagnostics);
    return db;
}

} // namespace rt2::core