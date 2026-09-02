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
	if (hooks.resetHistory) hooks.resetHistory();
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

bool RRFeatureLifecycle::Reconcile(const OutputExtent& output, const RRFeatureHooks& hooks)
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
	if (!hooks.queryOptimalSettings || !hooks.create)
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
		ResetHistory(hooks);
	}
	m_State.backend = RRBackend::RequestedRR;
	if (!hooks.create(tuple, detail))
	{
		LatchFallback(Failure(RRFeatureOperation::Create, detail), 0, hooks);
		return false;
	}
	m_Tuple = tuple;
	m_HasTuple = true;
	m_State.quality = tuple.quality;
	m_State.render = tuple.render;
	m_State.featureOwned = true;
	m_State.rrOutputValid = false;
	m_State.evaluationBegun = false;
	++m_State.featureGeneration;
	ResetHistory(hooks);
	m_State.backend = RRBackend::ActiveRR;
	return true;
}

bool RRFeatureLifecycle::InvalidateResources(const RRFeatureHooks& hooks)
{
	if (!ReleaseFeature(hooks, m_State.fallbackReason))
	{
		LatchFallback(m_State.fallbackReason, 0, hooks);
		return false;
	}
	m_HasTuple = false;
	ResetHistory(hooks);
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
