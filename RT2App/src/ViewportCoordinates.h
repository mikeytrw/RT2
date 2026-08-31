#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include "RenderExtents.h"

struct ViewportImageRect
{
	glm::vec2 screenMin{ 0.0f };
	glm::vec2 displaySize{ 0.0f };
	OutputExtent outputExtent;
	RenderExtent renderExtent;
};

struct CameraRay
{
	glm::vec3 origin{ 0.0f };
	glm::vec3 direction{ 0.0f, 0.0f, -1.0f };
};

// Convert a top-left-origin desktop mouse coordinate to a top-left-origin
// render pixel. The right and bottom edges are exclusive.
std::optional<glm::uvec2> ScreenToRenderPixel(
	const glm::vec2& screenPosition,
	const ViewportImageRect& viewport);

std::optional<glm::uvec2> ScreenToOutputPixel(
	const glm::vec2& screenPosition,
	const ViewportImageRect& viewport);

// Deterministic pinhole ray used for editor picking. This intentionally does
// not share the stochastic aperture/lens sampling used by path-traced rays.
CameraRay BuildPickingRay(
	const glm::mat4& inverseProjection,
	const glm::mat4& inverseView,
	const glm::vec3& cameraPosition,
	const glm::vec2& normalizedViewportPosition);
