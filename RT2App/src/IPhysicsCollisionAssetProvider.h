#pragma once

#ifndef RT2_PHYSICS_COLLISION_ASSET_PROVIDER_H
#define RT2_PHYSICS_COLLISION_ASSET_PROVIDER_H

// ============================================================================
// IPhysicsCollisionAssetProvider — host-owned, CPU-only collision-geometry
// seam (Bullet integration, T3 world lifecycle).
//
// T3 ships the injection seam and the presence check only: the controller
// borrows a provider pointer for one Play session, and Play with physics
// collision refs (PhysicsShapeComponent hull/triMesh with a non-empty path)
// and no provider is a loud refusal. No geometry decoding happens here.
//
// T4 extends this interface with the factored CPU decoder seam
// (DecodeCollisionGeometry -> Result<CollisionGeometry>, selected by file
// extension with exact sourceKey match) and the immutable session cache the
// provider owns while PhysicsWorld borrows views for one Play session.
// See the technical plan sections 2 (provider lifecycle binding) and 3
// (collision-asset pipeline).
//
// Lifetime (plan, Sol B2): the host owns the provider for the host session;
// the controller borrows the pointer for the Play session only. CPU-only
// targets (RT2Tests, RT2SliceRunner) inject an explicit test provider through
// the same setter seam. Stop never destroys the provider; only Play-session
// borrows and Bullet shapes end.
//
// This header is CPU-only (AssetResolver + core types; no Bullet, Vulkan,
// ImGui, Walnut, entt) so it links into RT2Tests and RT2SliceRunner unchanged.
// ============================================================================

#include "AssetReference.h"
#include "AssetResolver.h"
#include "PhysicsCollisionGeometry.h"
#include "core/Error.h"
#include "core/UUID.h"

#include <cstddef>
#include <string>

namespace rt2::core {

class IPhysicsCollisionAssetProvider
{
public:
    virtual ~IPhysicsCollisionAssetProvider() = default;

    // Host-session context, held by value-copy (never by reference into host
    // internals). The host refreshes it immediately before Play beside the
    // script context; CPU-only targets inject an explicit test value.
    virtual void SetContext(const AssetResolutionContext& ctx) = 0;

    // Resolve + decode-or-reuse the immutable geometry for one authored
    // collision ref. Returns a borrowed view owned by the provider's
    // host-session cache (valid until provider destruction or a stale-file
    // rebuild of that entry); the caller must not mutate or retain it past
    // the Play session. Typed loud failure naming the entity UUID.
    virtual Result<const CollisionGeometry*> GetCollisionGeometry(
        const AssetReference& ref, const UUID& entityUuid,
        const std::string& entityName) = 0;

    // Cache census for tests: entries held and total successful decodes.
    virtual size_t CacheEntryCount() const = 0;
    virtual size_t DecodeCount() const = 0;
};

} // namespace rt2::core

#endif // RT2_PHYSICS_COLLISION_ASSET_PROVIDER_H
