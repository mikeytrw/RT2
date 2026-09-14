// ============================================================================
// PhysicsCollisionAssetProvider — host-session immutable cache (T4).
// See header for lifetime/key rules. Loud typed failures name the entity
// UUID; the candidate Play world is destroyed by the caller per the atomic
// commit rule (no partial world on failure).
// ============================================================================

#include "PhysicsCollisionAssetProvider.h"

#include <fstream>

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

// Raw-byte content fingerprint: FNV-1a over the file bytes. Cheap (no parse)
// and sensitive to every content change, including same-size rewrites with a
// preserved or coarse timestamp that mtime/size comparison alone would miss.
bool FingerprintFile(const std::filesystem::path& path, uint64_t& hashOut,
                     std::string& detailOut)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        detailOut = "collision asset '" + path.string() + "' is not readable";
        return false;
    }
    uint64_t hash = 1469598103934665603ULL;
    char chunk[65536];
    while (in.good())
    {
        in.read(chunk, sizeof(chunk));
        const std::streamsize n = in.gcount();
        if (n > 0)
            hash = FnV1a64(chunk, (size_t)n, hash);
    }
    if (in.bad())
    {
        detailOut = "collision asset '" + path.string() + "' failed while reading";
        return false;
    }
    hashOut = hash;
    return true;
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
    AssetResolutionResult resolved;
    try
    {
        resolved = Resolve(ref, m_Context, entityUuid, entityName, diagnostics);
    }
    catch (const std::bad_alloc&)
    {
        return Result<const CollisionGeometry*>::Fail(
            Error::Io, uuidText,
            "entity " + uuidText + " ('" + entityName +
                "'): allocation failure while resolving collision asset '" +
                ref.path + "'");
    }
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

    // Cache key: effective ID AND canonical resolved path + sourceKey +
    // geometry settings. An asset ID retargeted to a different file misses
    // the old key by construction instead of reusing its entry.
    const std::string canonical =
        CanonicalAssetPath(resolved.resolvedPath).generic_string();
    const std::string idPart = resolved.effectiveId.IsNull()
                                   ? "id:nil"
                                   : "id:" + resolved.effectiveId.ToString();
    const std::string key = idPart + "|path:" + canonical +
                            "|sk:" + ref.sourceKey + "|" +
                            GeometrySettingsKey(ref.importSettings);

    uint64_t rawHash = 0;
    std::string fpDetail;
    try
    {
        if (!FingerprintFile(resolved.resolvedPath, rawHash, fpDetail))
        {
            return Result<const CollisionGeometry*>::Fail(
                Error::MissingAsset, uuidText,
                "entity " + uuidText + " ('" + entityName + "'): " + fpDetail);
        }
    }
    catch (const std::bad_alloc&)
    {
        return Result<const CollisionGeometry*>::Fail(
            Error::Io, uuidText,
            "entity " + uuidText + " ('" + entityName +
                "'): allocation failure while fingerprinting collision asset");
    }

    auto it = m_Cache.find(key);
    if (it != m_Cache.end() && it->second.rawContentHash == rawHash &&
        it->second.canonicalPath == canonical)
    {
        return Result<const CollisionGeometry*>::Ok(&it->second.geometry);
    }

    Result<CollisionGeometry> decoded;
    try
    {
        decoded = DecodeCollisionGeometry(resolved.resolvedPath, ref.sourceKey,
                                          ref.importSettings);
    }
    catch (const std::bad_alloc&)
    {
        // The decoder translates its own failures; this guards the call
        // boundary itself (plus map insertion below).
        return Result<const CollisionGeometry*>::Fail(
            Error::Io, uuidText,
            "entity " + uuidText + " ('" + entityName +
                "'): allocation failure while decoding collision asset");
    }
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
    entry.rawContentHash = rawHash;
    entry.canonicalPath = canonical;
    try
    {
        auto inserted = m_Cache.insert_or_assign(key, std::move(entry));
        ++m_DecodeCount;
        return Result<const CollisionGeometry*>::Ok(
            &inserted.first->second.geometry);
    }
    catch (const std::bad_alloc&)
    {
        return Result<const CollisionGeometry*>::Fail(
            Error::Io, uuidText,
            "entity " + uuidText + " ('" + entityName +
                "'): allocation failure while caching collision geometry");
    }
}

} // namespace rt2::core
