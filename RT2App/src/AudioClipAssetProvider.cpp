// ============================================================================
// AudioClipAssetProvider — app-owned immutable clip-byte cache (audio A2).
// See header for lifetime/key rules. Loud typed failures name the entity
// UUID; the candidate Play world is destroyed by the caller per the atomic
// commit rule (no partial world on failure).
// ============================================================================

#include "AudioClipAssetProvider.h"

#include "PhysicsCollisionGeometry.h"

#include <fstream>

namespace rt2::core {
namespace {

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

// Read the whole file into memory. Bytes are hashed by the caller; there is
// no mtime/size shortcut on this path.
bool ReadClipBytes(const std::filesystem::path& path,
                   std::vector<char>& bytesOut, std::string& detailOut)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        detailOut = "audio clip '" + path.string() + "' is not readable";
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streampos end = in.tellg();
    if (end < 0)
    {
        detailOut = "audio clip '" + path.string() + "' has no readable size";
        return false;
    }
    in.seekg(0, std::ios::beg);
    bytesOut.resize(static_cast<size_t>(end));
    if (!bytesOut.empty())
    {
        in.read(bytesOut.data(),
                static_cast<std::streamsize>(bytesOut.size()));
        const std::streamsize got = in.gcount();
        if (got != static_cast<std::streamsize>(bytesOut.size()))
        {
            detailOut = "audio clip '" + path.string() +
                        "' failed while reading";
            return false;
        }
    }
    if (in.bad())
    {
        detailOut = "audio clip '" + path.string() +
                    "' failed while reading";
        return false;
    }
    return true;
}

} // namespace

Result<ResolvedAudioClip> AudioClipAssetProvider::ResolveClip(
    const AssetReference& ref, const UUID& entityUuid,
    const std::string& entityName)
{
    const std::string uuidText = entityUuid.ToString();
    if (ref.kind != AssetKind::AudioClip || ref.path.empty())
    {
        return Result<ResolvedAudioClip>::Fail(
            Error::InvalidArgument, uuidText,
            "audio clip for entity " + uuidText + " ('" + entityName +
                "') has an empty path or non-audioclip kind "
                "(audio clip refs must be durable AudioClip AssetReferences)");
    }
    if (!IsAudioClipPath(ref.path))
    {
        return Result<ResolvedAudioClip>::Fail(
            Error::InvalidArgument, uuidText,
            "entity " + uuidText + " ('" + entityName + "') audio clip '" +
                ref.path + "' must use a .wav, .flac, or .mp3 extension");
    }

    std::vector<AssetDiagnostic> diagnostics;
    AssetResolutionResult resolved =
        Resolve(ref, m_Context, entityUuid, entityName, diagnostics);
    if (!resolved.success)
    {
        Error::Code code = Error::MissingAsset;
        std::string detail = "audio clip '" + ref.path + "' unresolved";
        if (!diagnostics.empty())
        {
            code = SeverityToCode(diagnostics.front().severity);
            detail = "audio clip '" + ref.path + "' (" +
                     diagnostics.front().detail + ")";
        }
        return Result<ResolvedAudioClip>::Fail(
            code, uuidText,
            "entity " + uuidText + " ('" + entityName + "') " + detail);
    }

    // Cache key: effective ID AND canonical resolved path. An asset ID
    // retargeted to a different file misses the old key by construction
    // instead of reusing its entry.
    const std::string canonical =
        CanonicalAssetPath(resolved.resolvedPath).generic_string();
    const std::string idPart = resolved.effectiveId.IsNull()
                                   ? "id:nil"
                                   : "id:" + resolved.effectiveId.ToString();
    const std::string key = idPart + "|path:" + canonical;

    std::vector<char> fresh;
    std::string readDetail;
    if (!ReadClipBytes(resolved.resolvedPath, fresh, readDetail))
    {
        return Result<ResolvedAudioClip>::Fail(
            Error::MissingAsset, uuidText,
            "entity " + uuidText + " ('" + entityName + "'): " + readDetail);
    }
    // Fingerprint every byte on every resolve: mtime/size are hints, never
    // acceptance authorities, so same-size/same-mtime rewrites are observed.
    const uint64_t freshHash = fresh.empty()
        ? 1469598103934665603ULL
        : FnV1a64(fresh.data(), fresh.size());
    ++m_ReadCount;

    auto it = m_Cache.find(key);
    if (it != m_Cache.end() && it->second.contentHash == freshHash &&
        it->second.canonicalPath == canonical)
    {
        ResolvedAudioClip out;
        out.bytes = it->second.bytes;
        // The advertised path is the computed canonical path, not the raw
        // resolution spelling: an asset root or reference through a
        // junction/symlink (or any other alias spelling) must report the
        // same canonical identity the cache key — and the A4 decoded
        // generation key — is built from.
        out.canonicalPath = std::filesystem::path(canonical);
        out.effectiveId = resolved.effectiveId;
        out.fingerprint = freshHash;
        return Result<ResolvedAudioClip>::Ok(std::move(out));
    }

    auto stored = std::make_shared<const std::vector<char>>(std::move(fresh));
    CacheEntry entry;
    entry.bytes = stored;
    entry.contentHash = freshHash;
    entry.canonicalPath = canonical;
    m_Cache.insert_or_assign(key, std::move(entry));

    ResolvedAudioClip out;
    out.bytes = std::move(stored);
    out.canonicalPath = std::filesystem::path(canonical);
    out.effectiveId = resolved.effectiveId;
    out.fingerprint = freshHash;
    return Result<ResolvedAudioClip>::Ok(std::move(out));
}

} // namespace rt2::core
