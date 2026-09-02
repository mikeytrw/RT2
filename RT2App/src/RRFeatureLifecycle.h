#pragma once

#include "RenderExtents.h"

#include <cstdint>
#include <functional>
#include <string>

// W4's temporary static RR mode is deliberately represented by a small,
// Vulkan-free state machine.  The application supplies hooks for NGX and
// resource operations; RT2Tests and RT2SliceRunner therefore exercise the
// exact production transition rules without importing the SDK.
enum class RRBackend
{
	NativeNRD,
	RequestedRR,
	ActiveRR,
	FallbackPending,
	ActiveNativeNRD,
};

enum class RRQualityMode
{
	Quality,
};

struct RRQualityTuple
{
	RRQualityMode quality = RRQualityMode::Quality;
	OutputExtent output;
	RenderExtent render;

	bool operator==(const RRQualityTuple& other) const
	{
		return quality == other.quality && output == other.output && render == other.render;
	}
	bool operator!=(const RRQualityTuple& other) const { return !(*this == other); }
};

struct RROptimalSettings
{
	RenderExtent render;
	RenderExtent minimum;
	RenderExtent maximum;
	float sharpness = 0.0f;
};

enum class RRFeatureOperation
{
	OptimalSettings,
	WaitIdle,
	Release,
	Create,
	Evaluate,
	ResetHistory,
};

struct RRFeatureHooks
{
	// All hooks are called on the render thread.  Failure text must identify
	// the exact SDK/Vulkan operation; the authority preserves it verbatim.
	std::function<bool(OutputExtent, RRQualityMode, RROptimalSettings&, std::string&)> queryOptimalSettings;
	std::function<bool(const RRQualityTuple&, std::string&)> create;
	std::function<bool(std::string&)> evaluate;
	std::function<bool(std::string&)> waitIdle;
	std::function<bool(std::string&)> release;
	std::function<void()> resetHistory;
};

struct RRFeatureState
{
	RRBackend backend = RRBackend::NativeNRD;
	RRQualityMode quality = RRQualityMode::Quality;
	OutputExtent output;
	RenderExtent render;
	bool requested = false;
	bool featureOwned = false;
	bool evaluationBegun = false;
	bool rrOutputValid = false;
	bool failureLatched = false;
	uint64_t featureGeneration = 0;
	uint64_t historyResetGeneration = 0;
	int32_t lastResult = 0;
	std::string fallbackReason;
};

struct RRFrameDecision
{
	bool recorded = false;
	bool useRR = false;
	bool preserveDisplay = false;
	bool fallbackLatched = false;
	std::string reason;
};

class RRFeatureLifecycle final
{
public:
	RRFeatureLifecycle() = default;

	// Request state is intentionally separate from active state.  Calling
	// SetRequested(false) never destroys a live feature in the middle of a
	// command buffer; Reconcile performs the safe idle-boundary transition.
	void SetRequested(bool requested) { m_State.requested = requested; }
	bool IsRequested() const { return m_State.requested; }

	// Explicit user/developer retry boundary.  Evaluation failure itself is
	// one-way and cannot cause a per-frame retry.
	void ClearFailureLatch();

	// Query Quality dimensions, validate the SDK result and create/recreate a
	// feature exactly once for the current tuple.  Disabled/default mode never
	// calls an NGX hook.
	bool Reconcile(const OutputExtent& output, const RRFeatureHooks& hooks);
	// Called by the renderer immediately before application-owned images are
	// destroyed.  It is the only legal way to forget a feature outside
	// Reconcile, and enforces idle -> release ordering.
	bool InvalidateResources(const RRFeatureHooks& hooks);

	// Begin/complete are intentionally separate: Begin marks that NGX work has
	// entered the command buffer, while Complete controls whether the output
	// may be selected for tone mapping.
	RRFrameDecision BeginEvaluation();
	RRFrameDecision CompleteEvaluation(bool success, std::string reason = {},
		int32_t result = 0, const RRFeatureHooks& hooks = {});

	const RRFeatureState& State() const { return m_State; }
	RRBackend Backend() const { return m_State.backend; }
	const std::string& FallbackReason() const { return m_State.fallbackReason; }

private:
	static bool ValidateOptimal(const OutputExtent& output,
		const RROptimalSettings& settings, std::string& reason);
	void ResetHistory(const RRFeatureHooks& hooks);
	bool ReleaseFeature(const RRFeatureHooks& hooks, std::string& reason);
	void LatchFallback(std::string reason, int32_t result, const RRFeatureHooks& hooks);

	RRFeatureState m_State;
	RRQualityTuple m_Tuple;
	bool m_HasTuple = false;
};

using RRFeatureLifecycleAuthority = RRFeatureLifecycle;
