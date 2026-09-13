#pragma once

#ifndef RT2_PHYSICS_COMPONENTS_H
#define RT2_PHYSICS_COMPONENTS_H

#include "AssetReference.h"
#include "core/UUID.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>

// ============================================================================
// PhysicsComponents — authored rigid-body data (Bullet integration, T2
// persistence foundation).
//
// These components are plain authored data (the ECSComponents.h POD rule):
// what the user authors in Edit and what the .rt2scene v8 codec persists.
// They carry NO Bullet pointers (Scene-context runtime handles live only in
// the T3 PhysicsWorld companion map) and NO transient mesh-registry indices
// (collision geometry is referenced by durable AssetReference, resolved
// through the T4 collision provider; see the Asset-vs-Scene boundary in
// docs/glossary.md — MeshRef::meshIndex must never be persisted as identity).
//
// Context ownership reminder: `otherBody` below is an Authoring-context UUID
// (or nil = world/static anchor). It is remapped by EntityReferenceRemapper
// whenever entities are copied with fresh UUIDs, exactly like UUID-typed
// script fields.
//
// T2 ships persistence only: component definitions, exact equality, every
// manual codec (EntityRecord / scene JSON v8 / CloneInMemory /
// SubtreeEntityRecord / prefab records), v3-v7 migration (absent physics =
// no body), non-overridable prefab table entries, otherBody rebasing, and
// AssetReference visitation. Play-world construction, validation-at-Play,
// and simulation arrive in T3/T4 and must reuse these definitions verbatim.
// ============================================================================

enum class PhysicsBodyKind : uint8_t
{
    Static    = 0,
    Dynamic   = 1,
    Kinematic = 2,
};

enum class PhysicsShapeKind : uint8_t
{
    Sphere       = 0,
    Box          = 1,
    ConvexHull   = 2, // authored simplified hull asset (never the render mesh)
    StaticTriMesh = 3, // static bodies only; dynamic triangle mesh is refused
};

// Collision layers (uint16 pair per body). Decisive mapping from the plan:
// Dynamic(1), WorldStatic(2), Mechanism(4), Trigger(8: ghosts).
namespace PhysicsLayer
{
inline constexpr uint16_t Dynamic     = 1;
inline constexpr uint16_t WorldStatic = 2;
inline constexpr uint16_t Mechanism   = 4;
inline constexpr uint16_t Trigger     = 8;
} // namespace PhysicsLayer

// Authored rigid-body parameters. One scale owner only (plan): the entity's
// uniform world scale composes once at Play-time build; there is no second
// free-form shape scale. Units: 1 RT2 unit = 1 metre, mass in kilograms,
// angles in radians internally (degrees in the inspector).
struct PhysicsBodyComponent
{
    PhysicsBodyKind kind = PhysicsBodyKind::Static;
    float mass = 0.0f;              // Dynamic requires mass > 0 (T3 validates)
    float friction = 0.5f;
    float restitution = 0.0f;
    float linearDamping = 0.0f;
    float angularDamping = 0.0f;
    // CCD is explicitly authored, never inferred. Fast-sphere calibration:
    // ccdSweptRadius = 0.8 * sphere radius, ccdMotionThreshold = radius * 0.5.
    bool  ccdEnabled = false;
    float ccdMotionThreshold = 0.0f; // world units, post-scale (T3 validates)
    float ccdSweptRadius = 0.0f;     // world units, post-scale (T3 validates)
    bool  startAsleep = false;
    // Layer/mask pair. Default: a static world body colliding with dynamic
    // bodies, world statics, and mechanism bodies. T3 validates the pair.
    uint16_t layer = PhysicsLayer::WorldStatic;
    uint16_t mask = PhysicsLayer::Dynamic | PhysicsLayer::WorldStatic |
                    PhysicsLayer::Mechanism;

    bool operator==(const PhysicsBodyComponent& o) const
    {
        return kind == o.kind && mass == o.mass && friction == o.friction &&
               restitution == o.restitution &&
               linearDamping == o.linearDamping &&
               angularDamping == o.angularDamping &&
               ccdEnabled == o.ccdEnabled &&
               ccdMotionThreshold == o.ccdMotionThreshold &&
               ccdSweptRadius == o.ccdSweptRadius &&
               startAsleep == o.startAsleep && layer == o.layer &&
               mask == o.mask;
    }
    bool operator!=(const PhysicsBodyComponent& o) const { return !(*this == o); }
};

// Authored collision shape. One shape per entity for the slice (compound
// deferred). `hull` is read only when shape == ConvexHull; `triMesh` only
// when shape == StaticTriMesh. Both are durable AssetReferences
// (AssetKind::Model; sourceKey selects the subresource exactly like the
// scene collision decoder: "obj:whole-model" or
// "gltf:scene=..:node=..:mesh=..:primitive=.."); empty path = no reference.
struct PhysicsShapeComponent
{
    PhysicsShapeKind shape = PhysicsShapeKind::Sphere;
    float     radius = 0.5f;                      // Sphere only
    glm::vec3 halfExtents = {0.5f, 0.5f, 0.5f};   // Box only
    AssetReference hull;    // ConvexHull collision geometry (durable)
    AssetReference triMesh; // StaticTriMesh collision geometry (durable)
    bool  isTrigger = false; // ghost: overlaps, gives no response
    float collisionMargin = 0.04f; // Bullet default margin; world units (T3)

    bool operator==(const PhysicsShapeComponent& o) const
    {
        return shape == o.shape && radius == o.radius &&
               halfExtents == o.halfExtents && isTrigger == o.isTrigger &&
               collisionMargin == o.collisionMargin &&
               hull.kind == o.hull.kind && hull.path == o.hull.path &&
               hull.importSettings == o.hull.importSettings &&
               hull.sourceKey == o.hull.sourceKey &&
               hull.assetId == o.hull.assetId &&
               triMesh.kind == o.triMesh.kind &&
               triMesh.path == o.triMesh.path &&
               triMesh.importSettings == o.triMesh.importSettings &&
               triMesh.sourceKey == o.triMesh.sourceKey &&
               triMesh.assetId == o.triMesh.assetId;
    }
    bool operator!=(const PhysicsShapeComponent& o) const { return !(*this == o); }
};

// Driven hinge constraint (e.g. flipper). The owner body is the entity
// carrying this component (must also carry PhysicsBodyComponent, Dynamic or
// Kinematic — T3 refuses Play otherwise). `otherBody` is the Authoring UUID
// of the second constrained body, or nil = world/static frame anchor.
// Frames are authored as owner-local pivot+axis and other-local (or world,
// when otherBody is nil) pivot+axis; T5 resolves them once at build from
// each body's Play-time world transform. Angles in radians; axes are
// normalized at validation (T3).
struct PhysicsHingeComponent
{
    rt2::core::UUID otherBody; // nil = world anchor
    glm::vec3 ownerPivot = {0.0f, 0.0f, 0.0f};
    glm::vec3 ownerAxis = {0.0f, 0.0f, 1.0f};
    glm::vec3 otherPivot = {0.0f, 0.0f, 0.0f};
    glm::vec3 otherAxis = {0.0f, 0.0f, 1.0f};
    float minAngleLimit = 0.0f;
    float maxAngleLimit = 0.0f;
    // Drive: angular-velocity motor only in this slice (0 = velocity motor).
    uint8_t driveMode = 0;
    float motorTargetVelocity = 0.0f;
    float motorMaxImpulse = 0.0f;
    bool  motorEnabled = false;
    float restAngle = 0.0f;

    bool operator==(const PhysicsHingeComponent& o) const
    {
        return otherBody == o.otherBody && ownerPivot == o.ownerPivot &&
               ownerAxis == o.ownerAxis && otherPivot == o.otherPivot &&
               otherAxis == o.otherAxis &&
               minAngleLimit == o.minAngleLimit &&
               maxAngleLimit == o.maxAngleLimit && driveMode == o.driveMode &&
               motorTargetVelocity == o.motorTargetVelocity &&
               motorMaxImpulse == o.motorMaxImpulse &&
               motorEnabled == o.motorEnabled && restAngle == o.restAngle;
    }
    bool operator!=(const PhysicsHingeComponent& o) const { return !(*this == o); }
};

// Driven slider constraint (e.g. plunger/launcher). Identity semantics match
// PhysicsHingeComponent: owner-implied, optional otherBody (nil = world),
// axis authored in the owner frame and normalized at validation (T3).
// Limits and drive target are in world units along the axis.
struct PhysicsSliderComponent
{
    rt2::core::UUID otherBody; // nil = world anchor
    glm::vec3 axis = {1.0f, 0.0f, 0.0f};
    float lowerLimit = 0.0f;
    float upperLimit = 0.0f;
    float targetPosition = 0.0f;
    float motorTargetVelocity = 0.0f;
    float motorMaxForce = 0.0f;
    bool  motorEnabled = false;

    bool operator==(const PhysicsSliderComponent& o) const
    {
        return otherBody == o.otherBody && axis == o.axis &&
               lowerLimit == o.lowerLimit && upperLimit == o.upperLimit &&
               targetPosition == o.targetPosition &&
               motorTargetVelocity == o.motorTargetVelocity &&
               motorMaxForce == o.motorMaxForce &&
               motorEnabled == o.motorEnabled;
    }
    bool operator!=(const PhysicsSliderComponent& o) const { return !(*this == o); }
};

#endif // RT2_PHYSICS_COMPONENTS_H
