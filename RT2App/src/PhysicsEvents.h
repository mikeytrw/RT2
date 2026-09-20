#pragma once

#ifndef RT2_PHYSICS_EVENTS_H
#define RT2_PHYSICS_EVENTS_H

#include "core/UUID.h"

#include <glm/glm.hpp>

#include <cstdint>

// ============================================================================
// PhysicsEvents — deterministic per-frame physics event snapshot (Bullet
// integration, T6 events + safe point).
//
// What this is: plain Authoring-UUID-keyed event data scraped from Bullet
// after every fixed tick (contact manifolds, ghost overlaps) and accumulated
// into one immutable per-frame snapshot the controller publishes before
// OnUpdate. No Bullet types cross this header (CPU-only seam: RT2Tests and
// RT2SliceRunner include it without Bullet headers), no engine lifecycle
// callbacks are added, and no Lua/subscription surface lives here (T7).
//
// Snapshot rules (explicit and stable; the ticket requires them stated):
//   1. Canonical pairs: every event carries bodyA < bodyB (Authoring-UUID
//      order). Bullet reports manifolds/overlaps in solver order; when the
//      scrape swaps the pair into canonical order it negates the normal, so
//      the stored normal always points from bodyB toward bodyA.
//   2. Per-tick contact coalescing: one tick's manifold points for the same
//      unordered pair collapse into ONE Contact event (impulse = sum of the
//      tick's positive applied impulses, position = mean, normal =
//      normalized mean). A resting box pair therefore yields exactly one
//      Contact per tick no matter how many manifold points Bullet reports —
//      repeated manifold points can never create nondeterministic duplicates.
//   3. Trigger set-diff: ghost overlaps are diffed per tick against the
//      previous tick's overlap set — new pairs emit TriggerEnter, continuing
//      pairs TriggerStay, vanished pairs TriggerExit. Overlap history is
//      owned by PhysicsWorld and cleared on Stop/reset and purged on body
//      removal, so no stale Enter/Exit is ever fabricated.
//   4. Frame accumulation: the snapshot concatenates each tick's events in
//      tick order (tickIndex 0..4, the kMaxSubsteps cap). Zero ticks publish
//      a fresh empty snapshot — never stale prior-frame data. Five ticks
//      preserve all five ticks' distinct events.
//   5. Destroy filter: events referencing a UUID destroyed in the frame's
//      structural drain are filtered at snapshot build and never delivered
//      post-destroy.
//   6. Exact-dup collapse: at snapshot build, bitwise-identical events
//      collapse keeping first occurrence (order-preserving safety net; rule 2
//      already removes the structural duplicates).
//   7. Non-consuming: the published snapshot is a const vector every OnUpdate
//      consumer reads in full. A first script cannot consume events before a
//      later script regardless of UUID-sorted callback order.
//
// Field semantics: Contact carries the coalesced world position, the
// canonical normal (bodyB -> bodyA, Bullet's manifold convention preserved
// through the canonical swap), and the summed applied impulse (> 0).
// Trigger events carry the pair midpoint as position, the canonical
// bodyA -> bodyB direction as normal (or +Y when coincident), and impulse 0.
// ============================================================================

namespace rt2::core {

enum class PhysicsEventKind : uint8_t
{
    Contact      = 0,
    TriggerEnter = 1,
    TriggerStay  = 2,
    TriggerExit  = 3,
};

struct PhysicsEvent
{
    PhysicsEventKind kind = PhysicsEventKind::Contact;
    UUID bodyA; // canonical: bodyA < bodyB (nil never appears; both resolve)
    UUID bodyB;
    glm::vec3 position = {0.0f, 0.0f, 0.0f};
    glm::vec3 normal = {0.0f, 1.0f, 0.0f}; // canonical bodyB -> bodyA
    float impulse = 0.0f;
    uint32_t tickIndex = 0; // frame-local fixed-tick sequence (0..4)

    bool operator==(const PhysicsEvent& o) const
    {
        return kind == o.kind && bodyA == o.bodyA && bodyB == o.bodyB &&
               position == o.position && normal == o.normal &&
               impulse == o.impulse && tickIndex == o.tickIndex;
    }
    bool operator!=(const PhysicsEvent& o) const { return !(*this == o); }
    // Canonical frame order: kind, then pair, then tick, then payload. Used
    // by the exact-dup collapse set and by tests asserting determinism.
    bool operator<(const PhysicsEvent& o) const
    {
        if (kind != o.kind)
            return kind < o.kind;
        if (bodyA != o.bodyA)
            return bodyA < o.bodyA;
        if (bodyB != o.bodyB)
            return bodyB < o.bodyB;
        if (tickIndex != o.tickIndex)
            return tickIndex < o.tickIndex;
        if (position.x != o.position.x)
            return position.x < o.position.x;
        if (position.y != o.position.y)
            return position.y < o.position.y;
        if (position.z != o.position.z)
            return position.z < o.position.z;
        if (normal.x != o.normal.x)
            return normal.x < o.normal.x;
        if (normal.y != o.normal.y)
            return normal.y < o.normal.y;
        if (normal.z != o.normal.z)
            return normal.z < o.normal.z;
        return impulse < o.impulse;
    }
};

// Canonicalize one scraped pair: lower UUID first, negating the normal on
// swap so it always points from bodyB toward bodyA (rule 1).
inline void CanonicalizePhysicsPair(const UUID& first, const UUID& second,
                                    const glm::vec3& firstToSecondNormal,
                                    UUID& outA, UUID& outB, glm::vec3& outNormal)
{
    if (second < first)
    {
        outA = second;
        outB = first;
        outNormal = -firstToSecondNormal;
    }
    else
    {
        outA = first;
        outB = second;
        outNormal = firstToSecondNormal;
    }
}

} // namespace rt2::core

#endif // RT2_PHYSICS_EVENTS_H
