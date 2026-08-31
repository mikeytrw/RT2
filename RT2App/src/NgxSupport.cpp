#include "NgxSupport.h"

#include <algorithm>

namespace
{
struct MaskDiagnostic
{
	NgxSupportState state;
	const char* reason;
};

MaskDiagnostic DecodeMask(uint32_t supportMask)
{
	// This is one ordered decision for both public decoders.  Check-not-present
	// is the primary platform/support failure, followed by driver, adapter,
	// OS, and implementation constraints. Unknown bits remain explicit
	// unsupported hardware rather than being treated as support.
	if (supportMask == 0)
		return { NgxSupportState::Supported, "supported" };
	if ((supportMask & 1u) != 0)
		return { NgxSupportState::UnsupportedHardware, "required check not present" };
	if ((supportMask & 2u) != 0)
		return { NgxSupportState::DriverUpdateRequired, "driver version unsupported" };
	if ((supportMask & 4u) != 0)
		return { NgxSupportState::UnsupportedGpuVendor, "adapter unsupported" };
	if ((supportMask & 8u) != 0)
		return { NgxSupportState::UnsupportedHardware, "OS version below minimum" };
	if ((supportMask & 16u) != 0)
		return { NgxSupportState::UnsupportedHardware, "feature not implemented" };
	return { NgxSupportState::UnsupportedHardware, "unknown NGX support mask" };
}
}

const char* NgxSupportStateName(NgxSupportState state)
{
	switch (state)
	{
	case NgxSupportState::NotProbed: return "NotProbed";
	case NgxSupportState::Supported: return "Supported";
	case NgxSupportState::NeedsApplicationId: return "NeedsApplicationId";
	case NgxSupportState::RuntimeMissing: return "RuntimeMissing";
	case NgxSupportState::MissingVulkanRequirement: return "MissingVulkanRequirement";
	case NgxSupportState::UnsupportedGpuVendor: return "UnsupportedGpuVendor";
	case NgxSupportState::UnsupportedHardware: return "UnsupportedHardware";
	case NgxSupportState::DriverUpdateRequired: return "DriverUpdateRequired";
	case NgxSupportState::RequirementQueryFailure: return "RequirementQueryFailure";
	case NgxSupportState::InitializationFailure: return "InitializationFailure";
	case NgxSupportState::ParameterFailure: return "ParameterFailure";
	case NgxSupportState::ShutdownFailure: return "ShutdownFailure";
	case NgxSupportState::ApplicationDataPathFailure: return "ApplicationDataPathFailure";
	}
	return "Unknown";
}

NgxSupportState NgxSupportStateFromMask(uint32_t supportMask)
{
	return DecodeMask(supportMask).state;
}

const char* NgxSupportReasonFromMask(uint32_t supportMask)
{
	return DecodeMask(supportMask).reason;
}

std::string NgxSupportSnapshot::Format() const
{
	std::ostringstream stream;
	stream << "state=" << NgxSupportStateName(state)
		   << " reason=\"" << reason << "\""
		   << " sdk=" << sdkVersion
		   << " runtime=" << runtimeVersion
		   << " gpu=\"" << gpuName << "\""
		   << " ngx_result=0x" << std::hex << std::uppercase
		   << static_cast<uint32_t>(ngxResult)
		   << " vk_result=" << std::dec << vulkanResult
		   << " support_mask=" << supportMask
		   << " initialized=" << (initialized ? 1 : 0)
		   << " parameters_owned=" << (capabilityParametersOwned ? 1 : 0)
		   << " availability_known=" << (availabilityKnown ? 1 : 0)
		   << " available=" << (available ? 1 : 0)
		   << " driver_query_succeeded=" << (driverQuerySucceeded ? 1 : 0)
		   << " rr_feature_created=" << (rrFeatureCreated ? 1 : 0);
	return stream.str();
}
