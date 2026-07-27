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

	// Deterministic sampling seed (0 = renderer default). Set from CLI
	// --seed; consumed by the path tracer to initialize per-pixel RNG.
	uint32_t sceneSeed   = 0;

	// Render path
	bool  rasterFirst   = true;
	bool  accumulate    = true;  // temporal accumulation (non-NRD path only)

	// ReSTIR DI (Reservoir-based Resampling for Direct Illumination)
	// Replaces RIS — raster-first mode only. Temporal + spatial reuse.
	bool  restirEnabled       = true;
	uint32_t restirFreshCandidates = 4;    // M: fresh candidates per pixel per frame
	uint32_t restirSpatialNeighbors = 4;  // neighbor count for spatial reuse
	uint32_t restirSpatialRadius    = 16; // pixel radius for neighbor sampling
	uint32_t restirTemporalMCap     = 40; // max M from temporal (10 * freshCandidates)
	uint32_t restirSpatialMCap      = 20; // max M from spatial neighbors
	float restirDepthThreshold     = 0.1f; // relative depth difference for validation
	float restirNormalThreshold    = 0.98f; // normal dot product for validation (cos angle)
	float restirWorldPosThreshold  = 0.5f; // world-position distance for temporal validation
	uint32_t restirMaxTemporalAge  = 20;  // max temporal reuse age before rejection
	bool  restirTemporalReuse     = true;  // enable temporal reuse
	bool  restirSpatialReuse      = true;  // enable spatial reuse

	// ReSTIR GI (Reservoir-based Resampling for one-bounce Global Illumination)
	// One diffuse indirect bounce, temporal reuse first, raster-first only.
	// Independent of ReSTIR DI — requires raster-first G-buffer but not DI.
	bool     restirGIEnabled         = true;
	bool     restirGITemporalEnabled = true;
	uint32_t restirGIFreshCandidates = 1;    // M: fresh GI candidates per pixel
	uint32_t restirGITemporalMCap    = 20;   // capped M from temporal history
	uint32_t restirGIMaxTemporalAge  = 4;    // reject history above this age
	float    restirGIDepthThreshold  = 0.10f; // relative depth difference for validation
	float    restirGINormalThreshold  = 0.90f; // normal dot product for validation
	float    restirGIWorldPosThreshold = 0.10f; // world-position distance for temporal validation

	// NRD denoiser
	bool  nrdEnabled        = false;
	// 0 = off (white noise), 1 = Bayer 4x4, 2 = Interleaved Gradient Noise
	int   nrdLobeDither     = 1;
	float nrdMaxBlurRadius = 30.0f;
	int   nrdMaxAccumFrames = 63; // REBLUR_MAX_HISTORY_FRAME_NUM
	float nrdResponsiveRoughnessThreshold = 0.0f; // 0 disables shortened glossy history
	int   nrdResponsiveMinAccumFrames = 3;        // valid range: 0..historyFixFrameNum (3)
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

// Whether the viewport background is actually drawn this frame.
//
// `showBackground` is authored state: it says the user wants a background in
// the editor viewport, where it helps you tell "nothing is there" from "the
// light is not reaching it". It is editor scaffolding, so Play must not show
// it whatever the flag says — a ray that hits nothing there should read as
// the game's own background.
//
// Kept as a free function, and out of RendererGPU, for two reasons: the host
// reads RendererGPU's settings back into its authored copy, so the derived
// value must never live in the struct where that readback could overwrite the
// user's choice; and the rule is then testable without a GPU.
inline bool IsBackgroundVisible(const RenderSettings& settings, bool editorPresentation)
{
	return settings.showBackground && editorPresentation;
}
