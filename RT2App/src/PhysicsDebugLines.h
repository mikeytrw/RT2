#pragma once

#ifndef RT2_PHYSICS_DEBUG_LINES_H
#define RT2_PHYSICS_DEBUG_LINES_H

#include "core/UUID.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

// ============================================================================
// PhysicsDebugLines — CPU-only world-space debug DTO (Bullet T8).
//
// A minimal, renderer-free snapshot of physics collision geometry: one
// segment list in world space, captured through a minimal btIDebugDraw
// implementation (see PhysicsDebugCapture.h). Read-only from the runtime
// controller during Play/Paused; cleared on Stop; retained on Pause.
//
// Context ownership (docs/glossary.md): `owner` is an Authoring-context UUID
// naming the body/constraint entity the segment was captured from. Segments
// never carry Scene-context handles (entt::entity, Bullet pointers) nor
// GPU-context indices. Sorting is by owner UUID string, then capture order,
// so headless dumps are deterministic across runs.
//
// CPU-only by design: this header pulls glm + UUID only — no Bullet, no
// Vulkan, no ImGui, no Walnut. It links into RT2Tests/RT2SliceRunner through
// the shared CPU source closure. Actual drawing lives in the RT2App-only
// PhysicsDebugOverlay TU, which is never added to CPU targets.
// Projection reuses the existing CPU seam: ProjectToViewport
// (EditorViewportIcons.h:74) — this DTO carries world space only and never
// projects itself.
// ============================================================================

namespace rt2::core {

// Debug line ownership kind. Static/Dynamic/Kinematic mirror the staged
// PhysicsBodyKind; Trigger marks ghost (no-response) shapes regardless of
// host kind; Constraint marks hinge/slider adapter segments (persisted
// components, see PhysicsDebugCapture.h T5 note).
enum class PhysicsDebugLineKind : uint8_t
{
    Static = 0,
    Dynamic = 1,
    Kinematic = 2,
    Trigger = 3,
    Constraint = 4,
};

inline const char* PhysicsDebugLineKindName(PhysicsDebugLineKind kind)
{
    switch (kind)
    {
    case PhysicsDebugLineKind::Static:     return "static";
    case PhysicsDebugLineKind::Dynamic:    return "dynamic";
    case PhysicsDebugLineKind::Kinematic:  return "kinematic";
    case PhysicsDebugLineKind::Trigger:    return "trigger";
    case PhysicsDebugLineKind::Constraint: return "constraint";
    }
    return "unknown";
}

struct PhysicsDebugSegment
{
    UUID owner;
    PhysicsDebugLineKind kind = PhysicsDebugLineKind::Static;
    glm::vec3 a{ 0.0f };
    glm::vec3 b{ 0.0f };
};

struct PhysicsDebugLines
{
    std::vector<PhysicsDebugSegment> segments;

    void Clear() { segments.clear(); }
    size_t Count() const { return segments.size(); }
    bool Empty() const { return segments.empty(); }

    // Stable UUID/segment order: sorts by (owner UUID string, kind) while
    // preserving capture order within one owner. Capture already emits in
    // UUID order; callers re-sort after any manual append so headless output
    // is deterministic even if a future producer appends out of order.
    void SortStable();

    // Deterministic headless dump, one line per segment:
    //   "<uuid> <kind> <ax> <ay> <az> <bx> <by> <bz>\n"
    // Floats print with %.6f. Empty snapshot dumps to "".
    std::string Dump() const;

    size_t CountByKind(PhysicsDebugLineKind kind) const;
};

} // namespace rt2::core

#endif // RT2_PHYSICS_DEBUG_LINES_H
