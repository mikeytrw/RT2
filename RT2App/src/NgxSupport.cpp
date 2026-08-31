#include "NgxSupport.h"

#include <algorithm>

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
	}
	return "Unknown";
}

NgxSupportState NgxSupportStateFromMask(uint32_t supportMask)
{
	if (supportMask == 0)
		return NgxSupportState::Supported;
	if ((supportMask & 2u) != 0)
		return NgxSupportState::DriverUpdateRequired;
	if ((supportMask & 4u) != 0)
		return NgxSupportState::UnsupportedGpuVendor;
	return NgxSupportState::UnsupportedHardware;
}

const char* NgxSupportReasonFromMask(uint32_t supportMask)
{
	if (supportMask == 0)
		return "supported";
	if ((supportMask & 1u) != 0)
		return "required check not present";
	if ((supportMask & 2u) != 0)
		return "driver version unsupported";
	if ((supportMask & 4u) != 0)
		return "adapter unsupported";
	if ((supportMask & 8u) != 0)
		return "OS version below minimum";
	if ((supportMask & 16u) != 0)
		return "feature not implemented";
	return "unknown NGX support mask";
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
		   << " rr_feature_created=" << (rrFeatureCreated ? 1 : 0);
	return stream.str();
}
