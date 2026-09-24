#pragma once

#ifndef RT2_AUDIO_CLIP_ASSET_PROVIDER_H
#define RT2_AUDIO_CLIP_ASSET_PROVIDER_H

#include "AssetResolver.h"
#include "AudioComponents.h"
#include "core/UUID.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// AudioClipAssetProvider — app-owned immutable clip-byte cache (audio A2).
//
// Lifetime: the app owns one provider for the app session plus a value-held
// AssetResolutionContext refreshed from the current context immediately
// before Play/Preview. Callers borrow the provider pointer for one session
// only. The provider never decodes audio; decoding remains the injected
// backend's responsibility (A4).
//
// Resolution: an AudioSourceComponent::clip AssetReference resolves to a
// ResolvedAudioClip holding the stable identity (effective asset ID),
// canonical path, content fingerprint, and an immutable source-byte owner.
// ResolvedAudioClip owns everything needed after resolution; neither the
// session nor the backend retains a borrowed AssetResolutionContext or
// project-database pointer.
//
// Fingerprint: FNV-1a over the file bytes, recomputed on every resolve. There
// is no (mtime, size) early return — those values are hints, never acceptance
// authorities. A same-size/same-mtime byte rewrite therefore yields a new
// fingerprint. A refresh invalidates only future resolves; immutable byte
// owners already handed out are never mutated, so an old generation held by
// a voice stays valid while new resolves see the new bytes.
//
// CPU-only: AssetResolver + filesystem only. Links into RT2Tests and
// RT2SliceRunner unchanged. No miniaudio, device, Vulkan, ImGui, or Walnut.
// ============================================================================

namespace rt2::core {

struct ResolvedAudioClip
{
    // Immutable source bytes. Shared ownership: the provider retains one
    // reference in its cache entry while every holder keeps its own, so a
    // later rewrite cannot mutate bytes already handed out.
    std::shared_ptr<const std::vector<char>> bytes;
    std::filesystem::path canonicalPath;
    UUID effectiveId;
    // FNV-1a over the exact bytes above.
    uint64_t fingerprint = 0;

    bool IsValid() const noexcept
    { return bytes != nullptr && !bytes->empty(); }
};

class AudioClipAssetProvider final
{
public:
    AudioClipAssetProvider() = default;

    // Value-copy borrow rule: the provider holds the context by value, never
    // by reference into host internals. The host refreshes it from the
    // current AssetResolutionContext immediately before Play/Preview.
    void SetContext(const AssetResolutionContext& ctx)
    {
        m_Context = ctx;
    }
    const AssetResolutionContext& Context() const { return m_Context; }

    Result<ResolvedAudioClip> ResolveClip(
        const AssetReference& ref, const UUID& entityUuid,
        const std::string& entityName);

    size_t CacheEntryCount() const { return m_Cache.size(); }
    size_t ReadCount() const { return m_ReadCount; }

private:
    struct CacheEntry
    {
        std::shared_ptr<const std::vector<char>> bytes;
        // Raw-byte content fingerprint of the file the bytes were read from,
        // plus the canonical path that produced them. A hit requires both to
        // match current state: same-size/same-mtime rewrites and ID retargets
        // therefore rebuild instead of serving stale bytes.
        uint64_t contentHash = 0;
        std::string canonicalPath;
    };

    AssetResolutionContext m_Context;
    std::unordered_map<std::string, CacheEntry> m_Cache;
    size_t m_ReadCount = 0;
};

} // namespace rt2::core

#endif // RT2_AUDIO_CLIP_ASSET_PROVIDER_H
