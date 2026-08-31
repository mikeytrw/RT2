#include <doctest/doctest.h>

#include "NgxLifecycle.h"

#include <array>
#include <string>

namespace
{
constexpr int32_t kSuccess = 1;
constexpr int32_t kInvalidParameter = static_cast<int32_t>(0xBAD00005u);
constexpr int32_t kOutOfDate = static_cast<int32_t>(0xBAD0000Cu);
constexpr int32_t kNotImplemented = static_cast<int32_t>(0xBAD00012u);

struct FakeNgx
{
	NgxLifecycleCall requirement{};
	NgxLifecycleCall initialization{};
	NgxLifecycleCall parameters{};
	NgxLifecycleCall availability{};
	NgxLifecycleCall driver{};
	NgxLifecycleCall idle{};
	NgxLifecycleCall destroy{};
	NgxLifecycleCall shutdown{};
	std::array<int, 8> calls{};

	NgxLifecycleHooks Hooks()
	{
		return NgxLifecycleHooks{[this](NgxLifecycleOperation operation) {
			++calls[static_cast<size_t>(operation)];
			switch (operation)
			{
			case NgxLifecycleOperation::RequirementQuery: return requirement;
			case NgxLifecycleOperation::Initialize: return initialization;
			case NgxLifecycleOperation::CapabilityParameters: return parameters;
			case NgxLifecycleOperation::Availability: return availability;
			case NgxLifecycleOperation::Driver: return driver;
			case NgxLifecycleOperation::WaitIdle: return idle;
			case NgxLifecycleOperation::DestroyParameters: return destroy;
			case NgxLifecycleOperation::Shutdown: return shutdown;
			}
			return NgxLifecycleCall{};
		}};
	}

	void ConfigureSupported()
	{
		requirement.result = kSuccess;
		requirement.supportMask = 0;
		initialization.result = kSuccess;
		parameters.result = kSuccess;
		parameters.parametersOwned = true;
		availability.result = kSuccess;
		availability.available = true;
		driver.result = kSuccess;
	}
};
}

TEST_CASE("NGX lifecycle RED/GREEN supported requires the complete proof")
{
	FakeNgx fake;
	fake.ConfigureSupported();
	NgxSupportSnapshot snapshot;
	NgxLifecycleAuthority authority(snapshot);

	CHECK(authority.Probe(fake.Hooks()));
	CHECK(snapshot.IsSupported());
	CHECK(snapshot.initialized);
	CHECK(snapshot.capabilityParametersOwned);
	CHECK(snapshot.availabilityKnown);
	CHECK(snapshot.available);
	CHECK(snapshot.driverQuerySucceeded);
	CHECK(fake.calls[static_cast<size_t>(NgxLifecycleOperation::Driver)] == 1);

	NgxSupportSnapshot noParameters = snapshot;
	noParameters.capabilityParametersOwned = false;
	CHECK_FALSE(noParameters.IsSupported());
	NgxSupportSnapshot noAvailability = snapshot;
	noAvailability.available = false;
	CHECK_FALSE(noAvailability.IsSupported());
}

TEST_CASE("NGX lifecycle RED/GREEN unavailable capability and driver gates never become Supported")
{
	FakeNgx unavailable;
	unavailable.ConfigureSupported();
	unavailable.availability.available = false;
	NgxSupportSnapshot unavailableSnapshot;
	NgxLifecycleAuthority unavailableAuthority(unavailableSnapshot);
	CHECK_FALSE(unavailableAuthority.Probe(unavailable.Hooks()));
	CHECK(unavailableSnapshot.state == NgxSupportState::UnsupportedHardware);
	CHECK_FALSE(unavailableSnapshot.IsSupported());

	FakeNgx vendor;
	vendor.ConfigureSupported();
	vendor.availability.available = false;
	vendor.availability.unsupportedVendor = true;
	NgxSupportSnapshot vendorSnapshot;
	NgxLifecycleAuthority vendorAuthority(vendorSnapshot);
	CHECK_FALSE(vendorAuthority.Probe(vendor.Hooks()));
	CHECK(vendorSnapshot.state == NgxSupportState::UnsupportedGpuVendor);

	FakeNgx driver;
	driver.ConfigureSupported();
	driver.driver.needsUpdatedDriver = true;
	NgxSupportSnapshot driverSnapshot;
	NgxLifecycleAuthority driverAuthority(driverSnapshot);
	CHECK_FALSE(driverAuthority.Probe(driver.Hooks()));
	CHECK(driverSnapshot.state == NgxSupportState::DriverUpdateRequired);
	CHECK_FALSE(driverSnapshot.IsSupported());
}

TEST_CASE("NGX lifecycle RED/GREEN operation-sensitive failures retain exact results")
{
	struct FailureCase
	{
		NgxLifecycleOperation operation;
		int32_t result;
		NgxSupportState state;
		const char* operationName;
	};
	const FailureCase cases[] = {
		{ NgxLifecycleOperation::RequirementQuery, kInvalidParameter,
			NgxSupportState::RequirementQueryFailure, "requirement query" },
		{ NgxLifecycleOperation::RequirementQuery, kNotImplemented,
			NgxSupportState::RuntimeMissing, "requirement query" },
		{ NgxLifecycleOperation::Initialize, 77,
			NgxSupportState::InitializationFailure, "initialization" },
		{ NgxLifecycleOperation::CapabilityParameters, 77,
			NgxSupportState::ParameterFailure, "capability parameters" },
		{ NgxLifecycleOperation::Availability, 77,
			NgxSupportState::ParameterFailure, "availability query" },
		{ NgxLifecycleOperation::Driver, 77,
			NgxSupportState::ParameterFailure, "driver query" },
	};

	for (const FailureCase& failure : cases)
	{
		FakeNgx fake;
		fake.ConfigureSupported();
		switch (failure.operation)
		{
		case NgxLifecycleOperation::RequirementQuery: fake.requirement.result = failure.result; break;
		case NgxLifecycleOperation::Initialize: fake.initialization.result = failure.result; break;
		case NgxLifecycleOperation::CapabilityParameters: fake.parameters.result = failure.result; break;
		case NgxLifecycleOperation::Availability: fake.availability.result = failure.result; break;
		case NgxLifecycleOperation::Driver: fake.driver.result = failure.result; break;
		default: break;
		}
		NgxSupportSnapshot snapshot;
		NgxLifecycleAuthority authority(snapshot);
		CHECK_FALSE(authority.Probe(fake.Hooks()));
		CHECK(snapshot.state == failure.state);
		CHECK(snapshot.ngxResult == failure.result);
		CHECK(snapshot.reason.find(failure.operationName) != std::string::npos);
	}
}

TEST_CASE("NGX lifecycle RED/GREEN maps named results by every operation domain")
{
	struct MappingCase
	{
		NgxLifecycleOperation operation;
		int32_t result;
		NgxSupportState expectedState;
		const char* operationName;
	};
	const MappingCase cases[] = {
		{ NgxLifecycleOperation::RequirementQuery, kInvalidParameter,
			NgxSupportState::RequirementQueryFailure, "requirement query" },
		{ NgxLifecycleOperation::RequirementQuery, kNotImplemented,
			NgxSupportState::RuntimeMissing, "requirement query" },
		{ NgxLifecycleOperation::Initialize, kInvalidParameter,
			NgxSupportState::NeedsApplicationId, "initialization" },
		{ NgxLifecycleOperation::Initialize, kNotImplemented,
			NgxSupportState::RuntimeMissing, "initialization" },
		{ NgxLifecycleOperation::CapabilityParameters, kInvalidParameter,
			NgxSupportState::ParameterFailure, "capability parameters" },
		{ NgxLifecycleOperation::CapabilityParameters, kOutOfDate,
			NgxSupportState::ParameterFailure, "capability parameters" },
		{ NgxLifecycleOperation::Availability, kInvalidParameter,
			NgxSupportState::ParameterFailure, "availability query" },
		{ NgxLifecycleOperation::Availability, kNotImplemented,
			NgxSupportState::ParameterFailure, "availability query" },
		{ NgxLifecycleOperation::Driver, kInvalidParameter,
			NgxSupportState::ParameterFailure, "driver query" },
		{ NgxLifecycleOperation::Driver, kOutOfDate,
			NgxSupportState::ParameterFailure, "driver query" },
		{ NgxLifecycleOperation::WaitIdle, kInvalidParameter,
			NgxSupportState::ShutdownFailure, "Vulkan idle wait" },
		{ NgxLifecycleOperation::WaitIdle, kNotImplemented,
			NgxSupportState::ShutdownFailure, "Vulkan idle wait" },
		{ NgxLifecycleOperation::DestroyParameters, kInvalidParameter,
			NgxSupportState::ShutdownFailure, "capability-parameter destruction" },
		{ NgxLifecycleOperation::DestroyParameters, kOutOfDate,
			NgxSupportState::ShutdownFailure, "capability-parameter destruction" },
		{ NgxLifecycleOperation::Shutdown, kInvalidParameter,
			NgxSupportState::ShutdownFailure, "NGX shutdown" },
		{ NgxLifecycleOperation::Shutdown, kNotImplemented,
			NgxSupportState::ShutdownFailure, "NGX shutdown" },
	};

	for (const MappingCase& mapping : cases)
	{
		NgxSupportSnapshot snapshot;
		NgxLifecycleAuthority authority(snapshot);
		NgxLifecycleCall call;
		call.result = mapping.result;
		call.detail = "named result";
		authority.OperationFailure(mapping.operation, call, "fallback detail");
		CHECK(snapshot.state == mapping.expectedState);
		CHECK(snapshot.ngxResult == mapping.result);
		CHECK(snapshot.reason.find(mapping.operationName) != std::string::npos);
		CHECK(snapshot.reason.find("named result") != std::string::npos);
	}
}

TEST_CASE("NGX lifecycle RED/GREEN successful null capability parameters preserve SDK success")
{
	FakeNgx fake;
	fake.ConfigureSupported();
	// Match the production binding: it supplies a non-empty result string even
	// when the API violates the pointer-ownership postcondition.
	fake.parameters.detail = "NVSDK_NGX_Result_Success";
	fake.parameters.parametersOwned = false;
	NgxSupportSnapshot snapshot;
	NgxLifecycleAuthority authority(snapshot);

	CHECK_FALSE(authority.Probe(fake.Hooks()));
	CHECK(snapshot.state == NgxSupportState::ParameterFailure);
	CHECK(snapshot.ngxResult == kSuccess);
	CHECK_FALSE(snapshot.capabilityParametersOwned);
	CHECK(snapshot.reason.find("null pointer") != std::string::npos);
	CHECK(snapshot.reason.find("NVSDK_NGX_Result_Success") == std::string::npos);
}

TEST_CASE("NGX lifecycle RED/GREEN support masks preserve every bit with stable precedence")
{
	FakeNgx fake;
	fake.ConfigureSupported();
	fake.requirement.supportMask = 1u | 2u;
	NgxSupportSnapshot snapshot;
	NgxLifecycleAuthority authority(snapshot);

	CHECK_FALSE(authority.Probe(fake.Hooks()));
	CHECK(snapshot.supportMask == (1u | 2u));
	CHECK(snapshot.state == NgxSupportState::UnsupportedHardware);
	CHECK(std::string(NgxSupportReasonFromMask(snapshot.supportMask)) ==
		"required check not present");
	CHECK(fake.calls[static_cast<size_t>(NgxLifecycleOperation::Initialize)] == 0);

	FakeNgx allKnown;
	allKnown.ConfigureSupported();
	allKnown.requirement.supportMask = 2u | 4u | 8u | 16u;
	NgxSupportSnapshot allKnownSnapshot;
	NgxLifecycleAuthority allKnownAuthority(allKnownSnapshot);
	CHECK_FALSE(allKnownAuthority.Probe(allKnown.Hooks()));
	CHECK(allKnownSnapshot.supportMask == (2u | 4u | 8u | 16u));
	CHECK(allKnownSnapshot.state == NgxSupportState::DriverUpdateRequired);
	CHECK(std::string(NgxSupportReasonFromMask(allKnownSnapshot.supportMask)) ==
		"driver version unsupported");

	FakeNgx unknown;
	unknown.ConfigureSupported();
	unknown.requirement.supportMask = 32u;
	NgxSupportSnapshot unknownSnapshot;
	NgxLifecycleAuthority unknownAuthority(unknownSnapshot);
	CHECK_FALSE(unknownAuthority.Probe(unknown.Hooks()));
	CHECK(unknownSnapshot.supportMask == 32u);
	CHECK(unknownSnapshot.state == NgxSupportState::UnsupportedHardware);
	CHECK(std::string(NgxSupportReasonFromMask(unknownSnapshot.supportMask)) ==
		"unknown NGX support mask");
}

TEST_CASE("NGX lifecycle RED/GREEN shutdown is terminal and publishes teardown failures")
{
	FakeNgx fake;
	fake.ConfigureSupported();
	NgxSupportSnapshot snapshot;
	NgxLifecycleAuthority authority(snapshot);
	REQUIRE(authority.Probe(fake.Hooks()));
	fake.destroy.result = 99;
	fake.shutdown.result = 98;

	CHECK_FALSE(authority.Shutdown(fake.Hooks()));
	CHECK(snapshot.state == NgxSupportState::ShutdownFailure);
	CHECK(snapshot.ngxResult == 98);
	CHECK(snapshot.reason.find("NGX shutdown") != std::string::npos);
	const auto idleCalls = fake.calls[static_cast<size_t>(NgxLifecycleOperation::WaitIdle)];
	const auto destroyCalls = fake.calls[static_cast<size_t>(NgxLifecycleOperation::DestroyParameters)];
	const auto shutdownCalls = fake.calls[static_cast<size_t>(NgxLifecycleOperation::Shutdown)];
	CHECK_FALSE(authority.Shutdown(fake.Hooks()));
	CHECK(fake.calls[static_cast<size_t>(NgxLifecycleOperation::WaitIdle)] == idleCalls);
	CHECK(fake.calls[static_cast<size_t>(NgxLifecycleOperation::DestroyParameters)] == destroyCalls);
	CHECK(fake.calls[static_cast<size_t>(NgxLifecycleOperation::Shutdown)] == shutdownCalls);
	CHECK(authority.CleanupAttempted());

	FakeNgx parameterFailure;
	parameterFailure.ConfigureSupported();
	parameterFailure.parameters.parametersOwned = true;
	parameterFailure.parameters.result = 91;
	NgxSupportSnapshot parameterSnapshot;
	NgxLifecycleAuthority parameterAuthority(parameterSnapshot);
	CHECK_FALSE(parameterAuthority.Probe(parameterFailure.Hooks()));
	CHECK(parameterSnapshot.capabilityParametersOwned);
	CHECK(parameterSnapshot.state == NgxSupportState::ParameterFailure);
	CHECK(parameterAuthority.Shutdown(parameterFailure.Hooks()));
	CHECK(parameterFailure.calls[static_cast<size_t>(NgxLifecycleOperation::DestroyParameters)] == 1);

	FakeNgx missingOwnership;
	missingOwnership.ConfigureSupported();
	missingOwnership.parameters.parametersOwned = false;
	NgxSupportSnapshot missingOwnershipSnapshot;
	NgxLifecycleAuthority missingOwnershipAuthority(missingOwnershipSnapshot);
	CHECK_FALSE(missingOwnershipAuthority.Probe(missingOwnership.Hooks()));
	CHECK(missingOwnershipSnapshot.state == NgxSupportState::ParameterFailure);
	CHECK(missingOwnershipSnapshot.reason.find("null pointer") != std::string::npos);
}
