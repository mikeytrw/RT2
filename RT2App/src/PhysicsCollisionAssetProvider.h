#pragma once

#ifndef RT2_PHYSICS_COLLISION_ASSET_PROVIDER_IMPL_H
#define RT2_PHYSICS_COLLISION_ASSET_PROVIDER_IMPL_H

#include "IPhysicsCollisionAssetProvider.h"

#include "AssetResolver.h"
#include "PhysicsCollisionGeometry.h"
#include "core/UUID.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <unordered_map>

// ============================================================================
// PhysicsCollisionAssetProvider — host-owned immutable geometry cache (T4).
//
// Lifetime (plan section 2, Sol B2): the host owns one provider for the host
// session plus a value-held AssetResolutionContext refreshed immediately
// before Play beside the script context. The controller borrows the provider
// pointer for one Play session only; PhysicsWorld borrows cache views for one
// Play session and owns only the Bullet shapes built from them. Stop never
// destroys the provider; only Play-session borrows and Bullet shapes end.
//
// Cache key (plan section 3): effective asset ID AND canonical resolved
// path, sourceKey, and the geometry-affecting ImportSettings subset — never
// path alone, never a transient MeshRef::meshIndex. Same key decodes once;
// same-file/different-sourceKey entries are isolated; an ID retargeted to a
// different file naturally misses the old key. A changed file (observed via
// a raw-byte content fingerprint, not just mtime/size) rebuilds the entry;
// the running Play session keeps its borrowed views (Bullet shapes are
// already built), so the rebuild takes effect at the next Play — never
// mid-Play.
//
// CPU-only: AssetResolver + filesystem + decoder only. Links into RT2Tests
// and RT2SliceRunner unchanged.
// ============================================================================

namespace rt2::core {

class PhysicsCollisionAssetProvider final
    : public IPhysicsCollisionAssetProvider
{
public:
    PhysicsCollisionAssetProvider() = default;

    // Value-copy borrow rule: the provider holds the context by value, never
    // by reference into host internals. One stable value per Play session.
    void SetContext(const AssetResolutionContext& ctx) override
    {
        m_Context = ctx;
    }
    const AssetResolutionContext& Context() const { return m_Context; }

    Result<const CollisionGeometry*> GetCollisionGeometry(
        const AssetReference& ref, const UUID& entityUuid,
        const std::string& entityName) override;

    size_t CacheEntryCount() const override { return m_Cache.size(); }
    size_t DecodeCount() const override { return m_DecodeCount; }

private:
    struct CacheEntry
    {
        CollisionGeometry geometry;
        // Raw-byte content fingerprint of the file the geometry was decoded
        // from, plus the canonical path that produced it. A hit requires
        // both to match current state: same-size/same-mtime rewrites and
        // ID retargets therefore rebuild instead of serving stale geometry.
        uint64_t rawContentHash = 0;
        std::string canonicalPath;
    };

    AssetResolutionContext m_Context;
    std::unordered_map<std::string, CacheEntry> m_Cache;
    size_t m_DecodeCount = 0;
};

} // namespace rt2::core

#endif // RT2_PHYSICS_COLLISION_ASSET_PROVIDER_IMPL_H
