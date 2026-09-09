#pragma once

#include <cstdint>
#include <glm/glm.hpp>

// FrameSamplingState — the single per-frame sampling authority (amendment
// 2026-09-09, step 3). Computed once per frame by the renderer and consumed
// consistently by:
//
// - raster projection and ray-generation subpixel sampling (UBO jitter),
// - motion-vector interpretation (motion stays an UNJITTERED render-pixel
//   UV delta per the W3 contract; consumers subtract this jitter exactly
//   once: ReSTIR via its jitterDelta path, NGX via InJitterOffset),
// - ReSTIR DI/GI reprojection (PC jitter pair),
// - NRD camera jitter pair,
// - NGX RR InJitterOffsetX/Y.
//
// Units are render pixels throughout. When sampling is not jittered (Off
// mode, jitter switch disabled), both offsets are exactly zero.
struct FrameSamplingState
{
	uint32_t frameIndex = 0; // continuously incrementing sample clock
	glm::vec2 jitter{0.0f};  // current subpixel offset, ~[-0.5, 0.5] x scale
	glm::vec2 jitterPrev{0.0f}; // previous frame offset; zero on reset/cut
	bool jitterActive = false;  // false => both offsets are exactly zero
	bool resetThisFrame = false; // history invalid: cut/resize/mode change
};

// Halton low-discrepancy sequence value for a 1-based index. Shared with
// every consumer so the sampling pattern cannot drift between them.
inline float FrameSamplingHalton(int index, int base)
{
	float f = 1.0f, r = 0.0f;
	int i = index;
	while (i > 0)
	{
		f /= static_cast<float>(base);
		r += f * static_cast<float>(i % base);
		i /= base;
	}
	return r;
}

// The 16-entry subpixel cycle historically driven by m_NRDFrameIndex.
// Preserved verbatim so previously jittered modes keep their pattern.
inline glm::vec2 FrameSamplingJitterForFrame(uint32_t frameClock, float scale)
{
	const int frame = static_cast<int>((frameClock - 1u) % 16u) + 1;
	return glm::vec2(FrameSamplingHalton(frame, 2) - 0.5f,
		FrameSamplingHalton(frame, 3) - 0.5f) * scale;
}

inline FrameSamplingState ComputeFrameSampling(uint32_t frameClock,
	bool jitterEnabled, float jitterScale, bool historyReset)
{
	FrameSamplingState state;
	state.frameIndex = frameClock;
	state.resetThisFrame = historyReset;
	state.jitterActive = jitterEnabled;
	if (!jitterEnabled)
		return state;
	state.jitter = FrameSamplingJitterForFrame(frameClock, jitterScale);
	// Previous offset is intentionally NOT reconstructed here: the renderer
	// carries it forward from the stored prior state, zeroing it on reset.
	return state;
}

// NRD boundary conversions. REBLUR documents its inputs in UV (0..1):
// "sampleUv = pixelUv + cameraJitter" and "pixelUvPrev = pixelUv + mv".
// Every other consumer (raster/ray sampling, ReSTIR, NGX, motion) works in
// render pixels, so the authority converts exactly once, here, at the NRD
// seam. Passing pixel values as UV overstates them by the extent (1280x at
// 720p) and destroys NRD history; the Sponza NRD case measured exactly that.
inline glm::vec2 FrameSamplingJitterToUv(glm::vec2 jitterPixels, glm::vec2 extent)
{
	return glm::vec2(jitterPixels.x / extent.x, jitterPixels.y / extent.y);
}

inline glm::vec3 FrameSamplingMotionScaleToUv(glm::vec2 extent)
{
	return glm::vec3(1.0f / extent.x, 1.0f / extent.y, 0.0f);
}
