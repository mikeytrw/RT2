// ============================================================================
// PhysicsCollisionAssetProvider — host-session immutable cache (T4).
// See header for lifetime/key rules. Loud typed failures name the entity
// UUID; the candidate Play world is destroyed by the caller per the atomic
// commit rule (no partial world on failure).
// ============================================================================

#include "PhysicsCollisionAssetProvider.h"

namespace rt2::core {
namespace {

std::string GeometrySettingsKey(const ImportSettings& s)
{
    // Geometry-affecting subset only: triangulation changes decoded topology;
    // mergeMegaMesh selects the OBJ subresource walk. Material-only knobs
    // (assumeDielectricWithoutMetalRough, generateNormals) do not key.
    return std::string("tri=") + (s.triangulate ? "1" : "0") +
           ";merge=" + (s.mergeMegaMesh ? "1" : "0");
}

Error::Code SeverityToCode(AssetDiagnostic::Severity severity)
{
    using S = AssetDiagnostic::Severity;
    switch (severity)
    {
        case S::Missing: return Error::MissingAsset;
        case S::Malformed: return Error::Parse;
        case S::Unresolved: return Error::InvalidArgument;
        case S::Conflict: return Error::InvalidArgument;
        default: return Error::MissingAsset;
    }
}

} // namespace

Result<const CollisionGeometry*> PhysicsCollisionAssetProvider::GetCollisionGeometry(
    const AssetReference& ref, const UUID& entityUuid,
    const std::string& entityName)
{
    const std::string uuidText = entityUuid.ToString();
    if (ref.kind != AssetKind::Model || ref.path.empty() ||
        ref.sourceKey.empty())
    {
        return Result<const CollisionGeometry*>::Fail(
            Error::InvalidArgument, uuidText,
            "collision asset for entity " + uuidText + " ('" + entityName +
                "') has an empty path/sourceKey or non-model kind "
                "(physics collision refs must be durable Model AssetReferences)");
    }

    std::vector<AssetDiagnostic> diagnostics;
    AssetResolutionResult resolved =
        Resolve(ref, m_Context, entityUuid, entityName, diagnostics);
    if (!resolved.success)
    {
        Error::Code code = Error::MissingAsset;
        std::string detail = "collision asset '" + ref.path + "' unresolved";
        if (!diagnostics.empty())
        {
            code = SeverityToCode(diagnostics.front().severity);
            detail = "collision asset '" + ref.path + "' (" +
                     diagnostics.front().detail + ")";
        }
        return Result<const CollisionGeometry*>::Fail(
            code, uuidText,
            "entity " + uuidText + " ('" + entityName + "') " + detail);
    }

    // Cache key: effective ID or canonical fallback path + sourceKey +
    // geometry settings. Never path alone.
    std::string key;
    if (!resolved.effectiveId.IsNull())
        key = "id:" + resolved.effectiveId.ToString();
    else
        key = "path:" + CanonicalAssetPath(resolved.resolvedPath).string();
    key += "|sk:" + ref.sourceKey + "|" + GeometrySettingsKey(ref.importSettings);

    std::error_code ec;
    auto currentSize =
        std::filesystem::file_size(resolved.resolvedPath, ec);
    if (ec)
        currentSize = 0;
    auto currentMtime =
        std::filesystem::last_write_time(resolved.resolvedPath, ec);
    if (ec)
        currentMtime = std::filesystem::file_time_type{};

    auto it = m_Cache.find(key);
    if (it != m_Cache.end() && it->second.size == currentSize &&
        it->second.mtime == currentMtime)
    {
        return Result<const CollisionGeometry*>::Ok(&it->second.geometry);
    }

    Result<CollisionGeometry> decoded = DecodeCollisionGeometry(
        resolved.resolvedPath, ref.sourceKey, ref.importSettings);
    if (!decoded.IsOk())
    {
        // Preserve the decoder's typed code; name the entity UUID per the
        // loud-failure rule (decoder paths carry file paths, not UUIDs).
        return Result<const CollisionGeometry*>::Fail(
            decoded.error.code, uuidText,
            "entity " + uuidText + " ('" + entityName + "'): " +
                decoded.error.detail);
    }

    CacheEntry entry;
    entry.geometry = std::move(decoded.value);
    entry.mtime = currentMtime;
    entry.size = currentSize;
    auto inserted = m_Cache.insert_or_assign(key, std::move(entry));
    ++m_DecodeCount;
    return Result<const CollisionGeometry*>::Ok(&inserted.first->second.geometry);
}

} // namespace rt2::core
