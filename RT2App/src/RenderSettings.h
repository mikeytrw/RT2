#pragma once

#include <cstdint>

// RenderSettings — all user-tunable rendering knobs in one POD struct.
//
// WalnutApp writes to these fields via the RendererGPU::GetSettings() /
// SetSettings() API. A dirty flag is set whenever any field changes so
// the renderer can trigger ResetAccumulation automatically (no more
// manual ResetAccumulation calls scattered through UI code).
//
// Fields NOT here (internal to RendererGPU):
//   - NRD jitter (m_NRDJitter / m_NRDJitterPrev) — computed each frame
//     from m_NRDJitterEnabled + m_NRDJitterScale
//   - Frame indices, prev matrices, output image, etc.
//
// Camera settings (aperture, focus, far clip) live on Camera and are
// NOT part of RenderSettings — they're per-frame inputs, not render mode.
struct RenderSettings
{
	// Ray tracing
	int   spp            = 5;
	int   maxBounces     = 8;
	bool  showBackground = false;
	bool  neeOnly        = false;
	float emissiveBoost  = 1.0f;
	float envIntensity   = 1.0f;

	// Render path
	bool  rasterFirst   = false;

	// NRD denoiser
	bool  nrdEnabled        = false;
	float nrdMaxBlurRadius = 30.0f;
	int   nrdMaxAccumFrames = 30;
	bool  nrdAntiFirefly    = true;
	float nrdSplitScreen    = 0.0f;
	bool  nrdJitterEnabled  = true;
	float nrdJitterScale    = 1.0f;

	// G-buffer debug view (-1 = off, 0-10 = view modes)
	int   gbufferDebugMode  = -1;

	// Dirty flag — set true by SetSettings() when any field changes.
	// RendererGPU::Render() checks this, calls ResetAccumulation, clears it.
	bool  dirty = false;
};