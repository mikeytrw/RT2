#include "RRFeatureLifecycle.h"

#include <cmath>

namespace
{
const char* OpName(RRFeatureOperation op)
{
	switch (op)
	{
	case RRFeatureOperation::OptimalSettings: return "optimal-settings query";
	case RRFeatureOperation::WaitIdle: return "device idle wait";
	case RRFeatureOperation::Release: return "feature release";
	case RRFeatureOperation::Create: return "feature creation";
	case RRFeatureOperation::Evaluate: return "feature evaluation";
	case RRFeatureOperation::ResetHistory: return "history reset";
	}
	return "RR operation";
}

std::string Failure(RRFeatureOperation operation, const std::string& detail)
{
	return std::string(OpName(operation)) + " failed: " +
		(detail.empty() ? "unspecified failure" : detail);
}
}

const char* RREligibilityDecision::Reason() const
{
	switch (kind)
	{
	case RREligibility::Uninitialized: return "RR eligibility has not been evaluated";
	case RREligibility::Eligible: return "eligible";
	case RREligibility::DeveloperDisabled: return "RR developer mode is disabled";
	case RREligibility::NgxUnavailable: return "NGX Ray Reconstruction is unavailable";
	case RREligibility::RequiresRasterFirst: return "RR requires raster-first mode";
	case RREligibility::NativeDiagnosticBypass: return "RR is unsupported for G-buffer debug mode";
	case RREligibility::DepthOfField: return "RR is unsupported with depth of field/aperture";
	case RREligibility::UnsupportedCamera: return "RR is unsupported for the current camera projection";
	}
	return "RR eligibility is unknown";
}

bool ShouldRecordNativeNRD(RRBackend backend, bool gbufferDebug, bool rasterFirst,
	bool nrdEnabled, bool nrdAvailable, bool automaticFallback)
{
	if (gbufferDebug || !nrdAvailable)
		return false;
	if (backend == RRBackend::ActiveNativeNRD)
		// Approved automatic fallback: requested RR settled on native NRD.
		// Bypasses raster-first gating so pure-path and DOF requests still
		// denoise natively. The ordinary switch-off path below is unchanged.
		return nrdEnabled || automaticFallback;
	if (!rasterFirst)
		return false;
	return backend == RRBackend::NativeNRD && nrdEnabled;
}

bool ShouldCommitHeadlessOutput(bool requested, bool rendererAvailable,
	bool submitted, bool captureAllowed, bool renderFailure)
{
	return requested && rendererAvailable && submitted && captureAllowed && !renderFailure;
}

bool RequiredRenderStagesAvailable(bool rendererAvailable, bool rrActive,
	bool tonemapAvailable, bool rrTonemapAvailable)
{
	return rendererAvailable && tonemapAvailable && (!rrActive || rrTonemapAvailable);
}

RREligibilityDecision ClassifyRREligibility(bool developerSwitch, bool ngxSupported,
	bool rasterFirst, bool gbufferDebug, float aperture, bool projectionValid)
{
	if (!developerSwitch) return {RREligibility::DeveloperDisabled};
	if (gbufferDebug) return {RREligibility::NativeDiagnosticBypass};
	if (!ngxSupported) return {RREligibility::NgxUnavailable};
	if (!rasterFirst) return {RREligibility::RequiresRasterFirst};
	if (aperture > 0.0f) return {RREligibility::DepthOfField};
	if (!projectionValid) return {RREligibility::UnsupportedCamera};
	return {RREligibility::Eligible};
}

void RRFeatureLifecycle::ClearFailureLatch()
{
	m_State.failureLatched = false;
	m_State.fallbackReason.clear();
	m_State.lastResult = 0;
	if (m_State.backend == RRBackend::FallbackPending ||
		m_State.backend == RRBackend::ActiveNativeNRD)
		m_State.backend = m_State.requested ? RRBackend::RequestedRR : RRBackend::NativeNRD;
}

bool RRFeatureLifecycle::ValidateOptimal(const OutputExtent& output,
	const RROptimalSettings& settings, std::string& reason)
{
	if (!output.IsValid() || !settings.render.IsValid() ||
		!settings.minimum.IsValid() || !settings.maximum.IsValid())
	{
		reason = "NGX returned a zero extent";
		return false;
	}
	const auto inBounds = [](const RenderExtent& e) {
		return e.Width() <= 16384u && e.Height() <= 16384u;
	};
	if (!inBounds(settings.render) || !inBounds(settings.minimum) ||
		!inBounds(settings.maximum))
	{
		reason = "NGX returned dimensions outside the RT2 contract";
		return false;
	}
	if (settings.minimum.Width() > settings.render.Width() ||
		settings.minimum.Height() > settings.render.Height() ||
		settings.render.Width() > settings.maximum.Width() ||
		settings.render.Height() > settings.maximum.Height())
	{
		reason = "NGX returned inverted Quality dimensions";
		return false;
	}
	if (settings.render.Width() > output.Width() || settings.render.Height() > output.Height() ||
		settings.maximum.Width() > output.Width() || settings.maximum.Height() > output.Height())
	{
		reason = "NGX returned a render extent larger than the output extent";
		return false;
	}
	return true;
}

void RRFeatureLifecycle::ResetHistory(const RRFeatureHooks& hooks)
{
	++m_State.historyResetGeneration;
	m_State.rrResetPending = true;
	if (hooks.resetHistory) hooks.resetHistory();
}

void RRFeatureLifecycle::RequestHistoryReset(const RRFeatureHooks& hooks)
{
	// Coalesce duplicate host ownership: SetScene/SetSceneKeepTextures run
	// immediately before ResetAccumulation (WalnutApp scene-changed handler,
	// EditorSyncRouter full/material sync plus router reset), and both request.
	// Internal transitions (Activate, LatchFallback, SetIneligible) keep
	// their own ResetHistory edges; only repeated scene/cut requests merge.
	if (m_State.rrResetPending)
		return;
	ResetHistory(hooks);
}

bool RRFeatureLifecycle::ReleaseFeature(const RRFeatureHooks& hooks, std::string& reason)
{
	if (!m_State.featureOwned) return true;
	if (!hooks.waitIdle || !hooks.release)
	{
		reason = "safe release hooks are unavailable";
		return false;
	}
	std::string detail;
	if (!hooks.waitIdle(detail))
	{
		reason = Failure(RRFeatureOperation::WaitIdle, detail);
		return false;
	}
	if (!hooks.release(detail))
	{
		reason = Failure(RRFeatureOperation::Release, detail);
		return false;
	}
	m_State.featureOwned = false;
	m_State.rrOutputValid = false;
	m_State.evaluationBegun = false;
	if (m_State.failureLatched)
		m_State.backend = RRBackend::FallbackPending;
	return true;
}

void RRFeatureLifecycle::LatchFallback(std::string reason, int32_t result,
	const RRFeatureHooks& hooks)
{
	m_State.failureLatched = true;
	m_State.fallbackReason = std::move(reason);
	m_State.lastResult = result;
	m_State.backend = RRBackend::FallbackPending;
	m_State.rrOutputValid = false;
	m_State.evaluationBegun = false;
	ResetHistory(hooks);
}

bool RRFeatureLifecycle::SelectTuple(const OutputExtent& output, const RRFeatureHooks& hooks)
{
	m_State.output = output;
	if (!m_State.requested)
	{
		if (m_State.featureOwned)
		{
			std::string reason;
			if (!ReleaseFeature(hooks, reason))
			{
				LatchFallback(reason, 0, hooks);
				return false;
			}
			ResetHistory(hooks);
		}
		m_State.backend = RRBackend::NativeNRD;
		m_State.rrOutputValid = false;
		m_HasTuple = false;
		return true;
	}
	if (m_State.failureLatched)
	{
		m_State.backend = RRBackend::ActiveNativeNRD;
		return false;
	}
	if (m_HasTuple && m_Tuple.output == output &&
		(m_State.backend == RRBackend::RequestedRR || m_State.backend == RRBackend::ActiveRR))
		return true;
	if (!hooks.queryOptimalSettings)
	{
		LatchFallback("RR lifecycle hooks are unavailable", 0, hooks);
		return false;
	}

	RROptimalSettings optimal;
	std::string detail;
	if (!hooks.queryOptimalSettings(output, RRQualityMode::Quality, optimal, detail) ||
		!ValidateOptimal(output, optimal, detail))
	{
		LatchFallback(Failure(RRFeatureOperation::OptimalSettings, detail), 0, hooks);
		return false;
	}
	const RRQualityTuple tuple{RRQualityMode::Quality, output, optimal.render};
	if (m_HasTuple && tuple == m_Tuple && m_State.featureOwned)
	{
		m_State.backend = RRBackend::ActiveRR;
		return true;
	}
	if (m_State.featureOwned)
	{
		if (!ReleaseFeature(hooks, detail))
		{
			LatchFallback(detail, 0, hooks);
			return false;
		}
	}
	m_State.backend = RRBackend::RequestedRR;
	m_Tuple = tuple;
	m_HasTuple = true;
	m_State.quality = tuple.quality;
	m_State.render = tuple.render;
	m_State.fallbackReason.clear();
	m_State.featureOwned = false;
	m_State.rrOutputValid = false;
	m_State.evaluationBegun = false;
	return true;
}

bool RRFeatureLifecycle::Activate(const RRFeatureHooks& hooks)
{
	if (m_State.backend == RRBackend::ActiveRR && m_State.featureOwned)
		return true;
	if (m_State.backend != RRBackend::RequestedRR || !m_HasTuple || !hooks.create)
	{
		LatchFallback("RR activation has no selected tuple or create hook", 0, hooks);
		return false;
	}
	std::string detail;
	if (!hooks.create(m_Tuple, detail))
	{
		LatchFallback(Failure(RRFeatureOperation::Create, detail), 0, hooks);
		return false;
	}
	m_State.featureOwned = true;
	m_State.rrOutputValid = false;
	m_State.evaluationBegun = false;
	++m_State.featureGeneration;
	ResetHistory(hooks);
	m_State.backend = RRBackend::ActiveRR;
	return true;
}

bool RRFeatureLifecycle::SetIneligible(const OutputExtent& output, std::string reason,
	const RRFeatureHooks& hooks, RRIneligibleMode mode)
{
	m_State.output = output;
	const bool wasActive = m_State.featureOwned || m_State.backend == RRBackend::ActiveRR;
	if (m_State.featureOwned)
	{
		std::string detail;
		if (!ReleaseFeature(hooks, detail))
		{
			LatchFallback(detail, 0, hooks);
			return false;
		}
	}
	m_HasTuple = false;
	m_State.featureOwned = false;
	m_State.rrOutputValid = false;
	m_State.evaluationBegun = false;
	m_State.backend = mode == RRIneligibleMode::ActiveNativeNRD ? RRBackend::ActiveNativeNRD :
		mode == RRIneligibleMode::NativeDiagnosticBypass ? RRBackend::NativeDiagnosticBypass : RRBackend::NativeNRD;
	m_State.fallbackReason = std::move(reason);
	if (wasActive) ResetHistory(hooks);
	return true;
}

bool RRFeatureLifecycle::Reconcile(const OutputExtent& output, const RRFeatureHooks& hooks)
{
	if (!SelectTuple(output, hooks)) return false;
	if (!m_State.requested) return true;
	return Activate(hooks);
}

bool RRFeatureLifecycle::InvalidateResources(const RRFeatureHooks& hooks)
{
	if (!ReleaseFeature(hooks, m_State.fallbackReason))
	{
		LatchFallback(m_State.fallbackReason, 0, hooks);
		return false;
	}
	m_HasTuple = false;
	return true;
}

RRFrameDecision RRFeatureLifecycle::BeginEvaluation()
{
	RRFrameDecision decision;
	if (m_State.backend != RRBackend::ActiveRR || !m_State.featureOwned)
	{
		decision.reason = m_State.fallbackReason;
		return decision;
	}
	m_State.evaluationBegun = true;
	m_State.rrOutputValid = false;
	decision.recorded = true;
	decision.useRR = true;
	return decision;
}

RRFrameDecision RRFeatureLifecycle::CompleteEvaluation(bool success, std::string reason,
	int32_t result, const RRFeatureHooks& hooks)
{
	RRFrameDecision decision;
	if (!m_State.evaluationBegun)
	{
		decision.reason = "RR evaluation completion without a begun evaluation";
		return decision;
	}
	m_State.evaluationBegun = false;
	if (success)
	{
		m_State.lastResult = result;
		m_State.rrOutputValid = true;
		decision.recorded = true;
		decision.useRR = true;
		return decision;
	}
	LatchFallback(Failure(RRFeatureOperation::Evaluate, reason), result, hooks);
	decision.recorded = false;
	decision.preserveDisplay = true;
	decision.fallbackLatched = true;
	decision.reason = m_State.fallbackReason;
	return decision;
}
