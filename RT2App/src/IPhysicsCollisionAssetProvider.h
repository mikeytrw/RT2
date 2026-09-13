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
// This header is dependency-free (no Bullet, Vulkan, ImGui, Walnut, entt)
// so it links into RT2Tests and RT2SliceRunner unchanged.
// ============================================================================

namespace rt2::core {

class IPhysicsCollisionAssetProvider
{
public:
    virtual ~IPhysicsCollisionAssetProvider() = default;
};

} // namespace rt2::core

#endif // RT2_PHYSICS_COLLISION_ASSET_PROVIDER_H
