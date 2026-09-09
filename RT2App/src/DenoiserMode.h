#pragma once

#include <cstdint>
#include <optional>
#include <string>

// DenoiserMode — the single authored denoiser selection (amendment
// 2026-09-09, step 2). Exactly one of Off / NRD / RayReconstruction is
// active; there are no independent NRD and RR booleans anywhere.
//
// Vocabulary (glossary-adjacent, approved by the amendment):
// - authored mode: RenderSettings::denoiserMode, the session selection.
// - active backend: RRFeatureLifecycle::Backend(), what the renderer runs.
// - completed snapshot: CompletedDenoiserFrame, what the last submitted
//   frame actually used (Performance UI reads this, never the setting).
enum class DenoiserMode : uint8_t
{
	Off,               // no NRD and no RR
	NRD,               // native-resolution NRD path
	RayReconstruction, // NGX RR plus DLSS upscaling
};

// DlssQualityMode — the authored RR upscaling preset, shared with the
// RR feature lifecycle (tuple identity, optimal-settings query, feature
// creation). NGX remains authoritative for each preset's recommended
// render extent; the setting only selects which preset is queried.
enum class DlssQualityMode : uint8_t
{
	Quality,
	Balanced,
	Performance,
};

// CompletedDenoiser — the backend used by a frame. Derived from the checked
// frame outcome plus the lifecycle backend, never from the requested setting.
// NativeDiagnosticBypass reports a G-buffer debug frame as itself (R7); it
// must never display as Off.
enum class CompletedDenoiser : uint8_t
{
	None,               // no completed frame yet
	Off,                // completed frame ran neither NRD nor RR
	NRD,                // completed frame recorded native NRD
	RayReconstruction,  // completed frame evaluated RR successfully
	NRDFallback,        // completed native NRD after requested RR fell back
	NativeDiagnosticBypass, // G-buffer debug view; no denoiser ran
};

inline bool IsNrdAuthored(DenoiserMode mode) { return mode == DenoiserMode::NRD; }
inline bool IsRRRequested(DenoiserMode mode)
{
	return mode == DenoiserMode::RayReconstruction;
}

// Effective per-frame NRD state: authored NRD or the approved automatic
// native-NRD fallback (requested RR settled on native NRD). One authority;
// the bool overload below delegates to it.
inline bool EffectiveNrdEnabled(DenoiserMode authored, bool automaticFallback)
{
	return IsNrdAuthored(authored) || automaticFallback;
}

// ResolveCompletedDenoiser lives with the lifecycle authority
// (RRFeatureLifecycle.h): it needs the backend for the diagnostic-bypass
// identity (R7).

inline const char* DenoiserModeName(DenoiserMode mode)
{
	switch (mode)
	{
	case DenoiserMode::Off: return "Off";
	case DenoiserMode::NRD: return "NRD";
	case DenoiserMode::RayReconstruction: return "DLSS Ray Reconstruction";
	}
	return "Unknown";
}

inline const char* DlssQualityModeName(DlssQualityMode quality)
{
	switch (quality)
	{
	case DlssQualityMode::Quality: return "Quality";
	case DlssQualityMode::Balanced: return "Balanced";
	case DlssQualityMode::Performance: return "Performance";
	}
	return "Unknown";
}

inline const char* CompletedDenoiserName(CompletedDenoiser denoiser)
{
	switch (denoiser)
	{
	case CompletedDenoiser::None: return "None";
	case CompletedDenoiser::Off: return "Off";
	case CompletedDenoiser::NRD: return "NRD";
	case CompletedDenoiser::RayReconstruction: return "DLSS Ray Reconstruction";
	case CompletedDenoiser::NRDFallback: return "NRD (RR fallback)";
	case CompletedDenoiser::NativeDiagnosticBypass: return "Diagnostic bypass";
	}
	return "Unknown";
}

// Shared CLI/config spelling; compat flags map onto the same authority.
inline std::optional<DenoiserMode> ParseDenoiserMode(const std::string& text)
{
	if (text == "off" || text == "Off" || text == "OFF")
		return DenoiserMode::Off;
	if (text == "nrd" || text == "NRD")
		return DenoiserMode::NRD;
	if (text == "rr" || text == "RR" || text == "reconstruction" ||
		text == "dlss-rr" || text == "ray-reconstruction")
		return DenoiserMode::RayReconstruction;
	return std::nullopt;
}

inline std::optional<DlssQualityMode> ParseDlssQuality(const std::string& text)
{
	if (text == "quality" || text == "Quality")
		return DlssQualityMode::Quality;
	if (text == "balanced" || text == "Balanced")
		return DlssQualityMode::Balanced;
	if (text == "performance" || text == "Performance")
		return DlssQualityMode::Performance;
	return std::nullopt;
}
