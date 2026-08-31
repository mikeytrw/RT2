#pragma once

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

// CPU-only NGX support vocabulary.  This header deliberately contains no
// Vulkan or NVIDIA headers so RT2Tests and RT2SliceRunner can exercise every
// state/formatting/gating rule without importing the RT2App graphics stack.
enum class NgxSupportState
{
	NotProbed,
	Supported,
	NeedsApplicationId,
	RuntimeMissing,
	MissingVulkanRequirement,
	UnsupportedGpuVendor,
	UnsupportedHardware,
	DriverUpdateRequired,
	RequirementQueryFailure,
	InitializationFailure,
	ParameterFailure,
	ShutdownFailure,
	ApplicationDataPathFailure,
};

const char* NgxSupportStateName(NgxSupportState state);

// The values are intentionally plain integer snapshots.  NVSDK_NGX_Result is
// RT2App-only; preserving its underlying value keeps this contract stable and
// testable while retaining the exact SDK failure for diagnostics.
struct NgxSupportSnapshot
{
	NgxSupportState state = NgxSupportState::NotProbed;
	int32_t ngxResult = 0;
	int32_t vulkanResult = 0;
	uint32_t supportMask = 0;
	bool initialized = false;
	bool capabilityParametersOwned = false;
	bool availabilityKnown = false;
	bool available = false;
	bool driverQuerySucceeded = false;
	bool rrFeatureCreated = false;
	std::string reason = "not probed";
	std::string sdkVersion = "v310.7.0";
	std::string runtimeVersion = "unavailable";
	std::string gpuName = "(none)";

	bool IsSupported() const
	{
		return state == NgxSupportState::Supported && initialized &&
			capabilityParametersOwned && availabilityKnown && available &&
			driverQuerySucceeded && !rrFeatureCreated;
	}

	std::string Format() const;
};

// Translate the SDK's FeatureSupported bitfield without collapsing unknown
// bits into "supported".  Driver and adapter diagnostics have stable states;
// all other non-zero masks remain explicit unsupported hardware.
NgxSupportState NgxSupportStateFromMask(uint32_t supportMask);
const char* NgxSupportReasonFromMask(uint32_t supportMask);
