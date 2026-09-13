// ============================================================================
// PhysicsCollisionBodiesTests — Bullet T4 collision + bodies + authoring.
//
// Permanent discriminating RED/GREEN coverage for the T4 outcome (ticket
// t4-collision-bodies-authoring). CPU-only by design: no Vulkan, ImGui or
// Walnut (same guards as PhysicsWorldLifecycleTests).
// ============================================================================

#include "PhysicsWorld.h"

#ifdef IMGUI_VERSION
#error "T4 boundary: physics collision tests must not import ImGui"
#endif

#ifdef VK_HEADER_VERSION
#error "T4 boundary: physics collision tests must not import Vulkan"
#endif

#if __has_include("imgui.h")
#error "T4 boundary: imgui.h must not be reachable from physics collision units"
#endif

#if __has_include("vulkan/vulkan.h")
#error "T4 boundary: vulkan headers must not be reachable from physics collision units"
#endif

#if __has_include("Walnut/Application.h")
#error "T4 boundary: Walnut headers must not be reachable from physics collision units"
#endif

// Full T4 RED/GREEN cases land with the bodies/authoring workstreams below.
