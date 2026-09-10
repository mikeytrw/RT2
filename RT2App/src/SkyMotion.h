#pragma once

#include <cmath>
#include <glm/glm.hpp>

// Infinite-sky rotation-only motion authority (sky shimmer repair ticket).
// An infinitely distant sky has no parallax: only camera ORIENTATION maps
// one frame's sky direction to the previous frame's. Translation, sampling
// jitter and finite projector points never enter: NGX receives jitter
// separately, and a finite point reintroduces translational parallax.
//
// Contract (all in render pixels, current->previous):
// - infinite sky ignores camera translation;
// - motion excludes current/previous sampling jitter (callers pass the
//   unjittered pixel centre; this function takes no jitter);
// - identical current/previous matrices yield exactly vec2(0) (bit-exact:
//   no float work happens on that path);
// - yaw motion agrees with unjittered previous-direction projection within
//   0.25 render pixel.
// The GLSL twin lives in pathtracer_shared.glsl (skyMotionPixels) and the
// RR guide reporter derives the same expectation independently.
struct SkyMotionResult
{
	glm::vec2 motionPixels{0.0f};
	bool degenerate = false; // behind-camera or non-finite projection
};

// Exact rotation equality (glm has no mat4 operator== here, and mat3 may
// pad; compare the nine rotation floats with ==). Translation is excluded
// by construction: equal orientations imply zero sky motion.
inline bool SameSkyOrientation(const glm::mat4& a, const glm::mat4& b)
{
	for (int c = 0; c < 3; ++c)
		for (int r = 0; r < 3; ++r)
			if (a[c][r] != b[c][r])
				return false;
	return true;
}

inline bool SameSkyProjection(const glm::mat4& a, const glm::mat4& b)
{
	for (int c = 0; c < 4; ++c)
		for (int r = 0; r < 4; ++r)
			if (a[c][r] != b[c][r])
				return false;
	return true;
}

inline SkyMotionResult ComputeInfiniteSkyMotion(glm::vec2 pixel,
	glm::vec2 extent, const glm::mat4& viewToClip, const glm::mat4& worldToView,
	const glm::mat4& viewToClipPrev, const glm::mat4& worldToViewPrev)
{
	// Bit-exact static/translation-only zero: equal orientations and
	// projections take no float path, so identical frames compare == 0.0f
	// bit-for-bit and translation alone cannot move the sky.
	if (SameSkyOrientation(worldToView, worldToViewPrev) &&
		SameSkyProjection(viewToClip, viewToClipPrev))
		return {};
	const glm::vec2 currUv = (pixel + glm::vec2(0.5f)) / extent;
	const glm::vec2 ndc = currUv * 2.0f - 1.0f;
	const glm::vec4 viewTarget = glm::inverse(viewToClip) * glm::vec4(ndc, 1.0f, 1.0f);
	if (!std::isfinite(viewTarget.w) || std::abs(viewTarget.w) < 1e-6f)
		return {glm::vec2(0.0f), true};
	const glm::vec3 viewDirection = glm::normalize(glm::vec3(viewTarget) / viewTarget.w);
	const glm::vec3 worldDirection = glm::normalize(
		glm::vec3(glm::inverse(glm::mat3(worldToView)) * viewDirection));
	const glm::vec3 prevViewDirection = glm::mat3(worldToViewPrev) * worldDirection;
	const glm::vec4 prevClip = viewToClipPrev * glm::vec4(prevViewDirection, 0.0f);
	if (!std::isfinite(prevClip.w) || std::abs(prevClip.w) < 1e-6f)
		return {glm::vec2(0.0f), true};
	const glm::vec2 prevUv = (glm::vec2(prevClip) / prevClip.w) * 0.5f + 0.5f;
	const glm::vec2 motion = (prevUv - currUv) * extent;
	if (!std::isfinite(motion.x) || !std::isfinite(motion.y))
		return {glm::vec2(0.0f), true};
	return {motion, false};
}
