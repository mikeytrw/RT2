#pragma once

#ifndef RT2_PHYSICS_DEBUG_OVERLAY_H
#define RT2_PHYSICS_DEBUG_OVERLAY_H

#include "PhysicsDebugLines.h"

#include <glm/glm.hpp>

// ============================================================================
// PhysicsDebugOverlay — RT2App-only viewport drawing for T8 debug lines.
//
// Draws the CPU-only PhysicsDebugLines DTO (world space) over the viewport
// image by projecting each segment through the existing CPU seam
// (ProjectToViewport, EditorViewportIcons.h:74). Segments with either
// endpoint behind the camera are skipped; off-screen-spanning segments draw
// clipped by ImGui automatically.
//
// RT2App-only by design: this TU includes <imgui.h> and must NEVER enter the
// RT2Tests / RT2SliceRunner / RT2ImGuiProbe CPU source closures (same split
// as EditorViewportIconsDraw.cpp vs EditorViewportIcons.cpp). Colors are a
// pure presentation choice (ticket: exact styling is implementation
// judgment): static grey, dynamic green, kinematic yellow, trigger magenta,
// constraint cyan.
// ============================================================================

namespace rt2::core {

void DrawPhysicsDebugLines(const PhysicsDebugLines& lines,
                           const glm::mat4& viewProj,
                           const glm::vec2& imageMin,
                           const glm::vec2& imageSize);

} // namespace rt2::core

#endif // RT2_PHYSICS_DEBUG_OVERLAY_H
