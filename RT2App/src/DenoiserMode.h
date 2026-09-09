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

// CompletedDenoiser — the backend used by the latest completed
// (submitted) frame. Derived from the checked frame outcome, never from
// the requested setting.
enum class CompletedDenoiser : uint8_t
{
	None,               // no completed frame yet
	Off,                // completed frame ran neither NRD nor RR
	NRD,                // completed frame recorded native NRD
	RayReconstruction,  // completed frame evaluated RR successfully
	NRDFallback,        // completed native NRD after requested RR fell back
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

// Resolve the completed-frame label from the checked outcome. rrEvaluated
// and nrdRecorded must never both be true for one successful frame; that
// combination resolves to None so a violated invariant cannot present as a
// valid backend.
inline CompletedDenoiser ResolveCompletedDenoiser(bool rrEvaluated,
	bool nrdRecorded, bool rrFallbackLatched)
{
	if (rrEvaluated && nrdRecorded)
		return CompletedDenoiser::None;
	if (rrEvaluated)
		return CompletedDenoiser::RayReconstruction;
	if (nrdRecorded)
		return rrFallbackLatched ? CompletedDenoiser::NRDFallback : CompletedDenoiser::NRD;
	return CompletedDenoiser::Off;
}

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
