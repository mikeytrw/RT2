#include <doctest/doctest.h>
#include <string>

#include "NgxSupport.h"

TEST_CASE("NGX support state names and gating are explicit")
{
	const NgxSupportState states[] = {
		NgxSupportState::NotProbed,
		NgxSupportState::Supported,
		NgxSupportState::NeedsApplicationId,
		NgxSupportState::RuntimeMissing,
		NgxSupportState::MissingVulkanRequirement,
		NgxSupportState::UnsupportedGpuVendor,
		NgxSupportState::UnsupportedHardware,
		NgxSupportState::DriverUpdateRequired,
		NgxSupportState::RequirementQueryFailure,
		NgxSupportState::InitializationFailure,
		NgxSupportState::ParameterFailure,
		NgxSupportState::ShutdownFailure,
		NgxSupportState::ApplicationDataPathFailure,
	};
	for (const NgxSupportState state : states)
		CHECK(std::string(NgxSupportStateName(state)) != "Unknown");

	NgxSupportSnapshot snapshot;
	CHECK_FALSE(snapshot.IsSupported());
	snapshot.state = NgxSupportState::Supported;
	CHECK_FALSE(snapshot.IsSupported()); // init is a separate gate
	snapshot.initialized = true;
	CHECK_FALSE(snapshot.IsSupported()); // params, availability and driver are separate gates
	snapshot.capabilityParametersOwned = true;
	snapshot.availabilityKnown = true;
	snapshot.available = true;
	snapshot.driverQuerySucceeded = true;
	CHECK(snapshot.IsSupported());
	snapshot.rrFeatureCreated = true;
	CHECK_FALSE(snapshot.IsSupported()); // W1 must never claim a created RR feature
}

TEST_CASE("NGX support masks preserve driver and adapter diagnostics")
{
	CHECK(NgxSupportStateFromMask(0) == NgxSupportState::Supported);
	CHECK(NgxSupportStateFromMask(2) == NgxSupportState::DriverUpdateRequired);
	CHECK(NgxSupportStateFromMask(4) == NgxSupportState::UnsupportedGpuVendor);
	CHECK(NgxSupportStateFromMask(8) == NgxSupportState::UnsupportedHardware);
	CHECK(NgxSupportStateFromMask(16) == NgxSupportState::UnsupportedHardware);
	CHECK(std::string(NgxSupportReasonFromMask(2)) == "driver version unsupported");
	CHECK(std::string(NgxSupportReasonFromMask(4)) == "adapter unsupported");
	CHECK(std::string(NgxSupportReasonFromMask(0)) == "supported");
}

TEST_CASE("NGX support formatting is stable and retains exact results")
{
	NgxSupportSnapshot snapshot;
	snapshot.state = NgxSupportState::RuntimeMissing;
	snapshot.reason = "instance discovery failed: missing runtime";
	snapshot.ngxResult = static_cast<int32_t>(0xBAD00012u);
	snapshot.vulkanResult = -7;
	snapshot.supportMask = 16;
	snapshot.gpuName = "NVIDIA GeForce RTX 3090";
	const std::string expected =
		"state=RuntimeMissing reason=\"instance discovery failed: missing runtime\" sdk=v310.7.0 runtime=unavailable gpu=\"NVIDIA GeForce RTX 3090\" ngx_result=0xBAD00012 vk_result=-7 support_mask=16 initialized=0 parameters_owned=0 availability_known=0 available=0 driver_query_succeeded=0 rr_feature_created=0";
	CHECK(snapshot.Format() == expected);
	CHECK(snapshot.Format() == snapshot.Format());
}
