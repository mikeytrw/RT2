#include "ViewportCoordinates.h"

#include <algorithm>
#include <cmath>

std::optional<glm::uvec2> ScreenToRenderPixel(
	const glm::vec2& screenPosition,
	const ViewportImageRect& viewport)
{
	if (viewport.displaySize.x <= 0.0f || viewport.displaySize.y <= 0.0f ||
		viewport.renderExtent.x == 0u || viewport.renderExtent.y == 0u)
		return std::nullopt;

	const glm::vec2 local = screenPosition - viewport.screenMin;
	if (local.x < 0.0f || local.y < 0.0f ||
		local.x >= viewport.displaySize.x || local.y >= viewport.displaySize.y)
		return std::nullopt;

	const glm::vec2 normalized = local / viewport.displaySize;
	const uint32_t x = std::min(
		static_cast<uint32_t>(normalized.x * viewport.renderExtent.x),
		viewport.renderExtent.x - 1u);
	const uint32_t y = std::min(
		static_cast<uint32_t>(normalized.y * viewport.renderExtent.y),
		viewport.renderExtent.y - 1u);
	return glm::uvec2(x, y);
}

CameraRay BuildPickingRay(
	const glm::mat4& inverseProjection,
	const glm::mat4& inverseView,
	const glm::vec3& cameraPosition,
	const glm::vec2& normalizedViewportPosition)
{
	const glm::vec2 ndc = normalizedViewportPosition * 2.0f - 1.0f;
	const glm::vec4 target = inverseProjection * glm::vec4(ndc, 1.0f, 1.0f);
	const glm::vec3 viewDirection = glm::normalize(glm::vec3(target) / target.w);
	const glm::vec3 worldDirection = glm::normalize(
		glm::vec3(inverseView * glm::vec4(viewDirection, 0.0f)));
	return { cameraPosition, worldDirection };
}
