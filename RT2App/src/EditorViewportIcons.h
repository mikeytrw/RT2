#pragma once

#include "core/UUID.h"

#include <glm/glm.hpp>

#include <vector>

class SceneManager;
class EditorSelection;

// ============================================================================
// Editor viewport icons (Phase 8 follow-up)
//
// Lights and cameras have no geometry, so the renderer draws nothing for them
// and the GPU picker — which traces a ray against the acceleration structure —
// can never hit one. Before this, an unselected light was invisible in the
// viewport and unreachable except through the outliner.
//
// This module projects those entities to screen space and draws a 2D glyph per
// entity over the rendered image, with a matching screen-space hit test so a
// click selects the icon. The hit test must run before the GPU pick request:
// an icon sits in front of whatever geometry is behind it, and the GPU pick is
// asynchronous, so letting both fire would select the wall behind the light a
// frame later.
//
// Everything here is editor-only and must not draw in Play — same category as
// the viewport background and the transform gizmo.
//
// Projection and hit testing are free functions taking plain matrices and
// vectors so they can be tested without ImGui, a camera, or a Vulkan device.
// Only the drawing needs ImGui.
// ============================================================================

// What an icon stands for. Picks the glyph and its tint.
enum class EditorIconKind
{
	PointLight,
	SpotLight,
	DirectionalLight,
	Camera,
};

// One projected icon, positioned in viewport-screen pixels.
struct EditorIconPlacement
{
	rt2::core::UUID entity;
	EditorIconKind  kind = EditorIconKind::PointLight;
	glm::vec2       screenPos{ 0.0f };
	// Clip-space w, i.e. distance along the camera's forward axis. Used to
	// sort far-to-near for drawing and near-to-far for hit testing, so the
	// icon that looks nearest is the one that gets clicked.
	float           viewDepth = 0.0f;
	bool            selected = false;
	// Screen-space direction the light emits along, for the spot/directional
	// glyphs. Zero when the projected aim point is behind the camera, in
	// which case the glyph falls back to an undirected one.
	glm::vec2       screenAim{ 0.0f };
};

struct EditorIconHit
{
	bool            hit = false;
	rt2::core::UUID entity;
};

// Screen-space radius of an icon's clickable disc, in pixels. The drawn glyph
// is smaller than this so that a near-miss still selects.
inline constexpr float kEditorIconRadius = 13.0f;

// Project a world point into viewport-screen pixels. Returns false when the
// point is at or behind the camera plane, or when the projection produced a
// non-finite result.
bool ProjectToViewport(const glm::vec3& worldPos, const glm::mat4& viewProj,
	const glm::vec2& imageMin, const glm::vec2& imageSize,
	glm::vec2& outScreen, float& outDepth);

// Nearest-to-camera icon whose disc contains `mouse`. Ties on depth resolve to
// whichever comes first in `icons`.
EditorIconHit HitTestEditorIcons(const std::vector<EditorIconPlacement>& icons,
	const glm::vec2& mouse, float radius = kEditorIconRadius);

// Collect every light and camera entity in the scene and project it. Entities
// whose icon lands off-screen or behind the camera are dropped. The result is
// sorted far-to-near, i.e. in draw order.
std::vector<EditorIconPlacement> BuildEditorIconPlacements(
	const SceneManager& scene, const EditorSelection& selection,
	const glm::mat4& viewProj, const glm::vec2& imageMin,
	const glm::vec2& imageSize);

struct EditorIconOverlayResult
{
	// True while the mouse is over an icon. The host must suppress its own
	// viewport pick for that click.
	bool            consumesMouse = false;
	bool            clicked = false;
	rt2::core::UUID clickedEntity;
};

// Draw the icons over the viewport image and hit-test the mouse against them.
// `imageHovered` gates interaction; `mouse` is in screen pixels. Call between
// the ImGui::Image call and the host's pick handling.
EditorIconOverlayResult DrawEditorViewportIcons(
	const std::vector<EditorIconPlacement>& icons,
	const glm::vec2& mouse, bool imageHovered, bool clickPressed);
