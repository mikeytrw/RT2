#include "NgxLifecycle.h"

#include <sstream>
#include <utility>

namespace
{
constexpr uint32_t kSuccess = 0x1u;
constexpr uint32_t kInvalidParameter = 0xBAD00005u;
constexpr uint32_t kOutOfDate = 0xBAD0000Cu;
constexpr uint32_t kPlatformError = 0xBAD00002u;
constexpr uint32_t kNotImplemented = 0xBAD00012u;
constexpr uint32_t kUnableToInitialize = 0xBAD0000Bu;

bool Failed(int32_t result)
{
	return static_cast<uint32_t>(result) != kSuccess;
}

bool Is(uint32_t result, uint32_t expected)
{
	return result == expected;
}

const char* OperationName(NgxLifecycleOperation operation)
{
	switch (operation)
	{
	case NgxLifecycleOperation::RequirementQuery: return "requirement query";
	case NgxLifecycleOperation::Initialize: return "initialization";
	case NgxLifecycleOperation::CapabilityParameters: return "capability parameters";
	case NgxLifecycleOperation::Availability: return "availability query";
	case NgxLifecycleOperation::Driver: return "driver query";
	case NgxLifecycleOperation::WaitIdle: return "Vulkan idle wait";
	case NgxLifecycleOperation::DestroyParameters: return "capability-parameter destruction";
	case NgxLifecycleOperation::Shutdown: return "NGX shutdown";
	}
	return "NGX operation";
}
}

NgxLifecycleAuthority::NgxLifecycleAuthority(NgxSupportSnapshot& snapshot)
	: m_Snapshot(snapshot)
{
}

void NgxLifecycleAuthority::SetState(NgxSupportState state, std::string reason,
	int32_t ngxResult, int32_t vulkanResult)
{
	m_Snapshot.state = state;
	m_Snapshot.reason = std::move(reason);
	m_Snapshot.ngxResult = ngxResult;
	m_Snapshot.vulkanResult = vulkanResult;
}

void NgxLifecycleAuthority::ExternalFailure(NgxSupportState state,
	std::string reason, int32_t ngxResult, int32_t vulkanResult)
{
	SetState(state, std::move(reason), ngxResult, vulkanResult);
}

void NgxLifecycleAuthority::OperationFailure(NgxLifecycleOperation operation,
	const NgxLifecycleCall& call, std::string detail)
{
	Failure(operation, call, std::move(detail));
}

void NgxLifecycleAuthority::SetGpuName(std::string name)
{
	m_Snapshot.gpuName = std::move(name);
}

void NgxLifecycleAuthority::SetRuntimeVersion(std::string version)
{
	m_Snapshot.runtimeVersion = std::move(version);
}

void NgxLifecycleAuthority::Failure(NgxLifecycleOperation operation,
	const NgxLifecycleCall& call, std::string detail)
{
	const uint32_t result = static_cast<uint32_t>(call.result);
	NgxSupportState state = NgxSupportState::RequirementQueryFailure;
	if (Is(result, kInvalidParameter))
		state = NgxSupportState::NeedsApplicationId;
	else if (Is(result, kOutOfDate))
		state = NgxSupportState::DriverUpdateRequired;
	else if (Is(result, kNotImplemented) ||
		(operation == NgxLifecycleOperation::RequirementQuery && Is(result, kPlatformError)))
		state = NgxSupportState::RuntimeMissing;
	else if (operation == NgxLifecycleOperation::Initialize)
		state = NgxSupportState::InitializationFailure;
	else if (operation == NgxLifecycleOperation::CapabilityParameters ||
		operation == NgxLifecycleOperation::Availability ||
		operation == NgxLifecycleOperation::Driver)
		state = NgxSupportState::ParameterFailure;
	else if (operation == NgxLifecycleOperation::WaitIdle ||
		operation == NgxLifecycleOperation::DestroyParameters ||
		operation == NgxLifecycleOperation::Shutdown)
		state = NgxSupportState::ShutdownFailure;

	std::ostringstream reason;
	reason << OperationName(operation) << " failed: "
		<< (call.detail.empty() ? detail : call.detail);
	SetState(state, reason.str(), call.result, call.vulkanResult);
}

bool NgxLifecycleAuthority::Probe(const NgxLifecycleHooks& hooks)
{
	if (!hooks.invoke)
	{
		ExternalFailure(NgxSupportState::InitializationFailure,
			"NGX lifecycle hook is unavailable");
		return false;
	}

	const NgxLifecycleCall requirement = hooks.invoke(NgxLifecycleOperation::RequirementQuery);
	if (Failed(requirement.result))
	{
		Failure(NgxLifecycleOperation::RequirementQuery, requirement,
			"SDK result");
		return false;
	}
	m_Snapshot.supportMask = requirement.supportMask;
	if (requirement.supportMask != 0)
	{
		SetState(NgxSupportStateFromMask(requirement.supportMask),
			NgxSupportReasonFromMask(requirement.supportMask), requirement.result,
			requirement.vulkanResult);
		return false;
	}

	const NgxLifecycleCall initialization = hooks.invoke(NgxLifecycleOperation::Initialize);
	if (Failed(initialization.result))
	{
		Failure(NgxLifecycleOperation::Initialize, initialization, "SDK result");
		return false;
	}
	m_Snapshot.initialized = true;

	const NgxLifecycleCall parameters = hooks.invoke(NgxLifecycleOperation::CapabilityParameters);
	if (parameters.parametersOwned)
		m_Snapshot.capabilityParametersOwned = true;
	if (Failed(parameters.result) || !parameters.parametersOwned)
	{
		NgxLifecycleCall failed = parameters;
		if (!Failed(failed.result)) failed.result = 0;
		Failure(NgxLifecycleOperation::CapabilityParameters, failed,
			parameters.parametersOwned ? "SDK result" : "ownership was not returned");
		return false;
	}
	m_Snapshot.capabilityParametersOwned = true;

	const NgxLifecycleCall availability = hooks.invoke(NgxLifecycleOperation::Availability);
	m_Snapshot.availabilityKnown = true;
	m_Snapshot.available = availability.available;
	if (Failed(availability.result))
	{
		Failure(NgxLifecycleOperation::Availability, availability, "SDK result");
		return false;
	}
	if (!availability.available)
	{
		SetState(availability.unsupportedVendor ? NgxSupportState::UnsupportedGpuVendor
			: NgxSupportState::UnsupportedHardware,
			availability.unsupportedVendor ? "Ray Reconstruction unavailable for selected GPU vendor"
				: "Ray Reconstruction unavailable on selected hardware",
			availability.result, availability.vulkanResult);
		return false;
	}

	const NgxLifecycleCall driver = hooks.invoke(NgxLifecycleOperation::Driver);
	m_Snapshot.driverQuerySucceeded = !Failed(driver.result);
	if (Failed(driver.result))
	{
		Failure(NgxLifecycleOperation::Driver, driver, "SDK result");
		return false;
	}
	if (driver.needsUpdatedDriver)
	{
		SetState(NgxSupportState::DriverUpdateRequired,
			"driver update required", driver.result, driver.vulkanResult);
		return false;
	}

	m_Snapshot.state = NgxSupportState::Supported;
	m_Snapshot.reason = "supported";
	m_Snapshot.ngxResult = driver.result;
	return true;
}

bool NgxLifecycleAuthority::Shutdown(const NgxLifecycleHooks& hooks)
{
	if (m_CleanupAttempted)
		return m_Snapshot.state != NgxSupportState::ShutdownFailure;
	m_CleanupAttempted = true;
	if (!hooks.invoke)
	{
		ExternalFailure(NgxSupportState::ShutdownFailure,
			"NGX teardown hook is unavailable");
		return false;
	}

	const NgxLifecycleCall idle = hooks.invoke(NgxLifecycleOperation::WaitIdle);
	if (Failed(idle.result))
	{
		Failure(NgxLifecycleOperation::WaitIdle, idle, "Vulkan result");
		return false;
	}
	if (m_Snapshot.capabilityParametersOwned)
	{
		const NgxLifecycleCall parameters = hooks.invoke(NgxLifecycleOperation::DestroyParameters);
		if (Failed(parameters.result))
		{
			Failure(NgxLifecycleOperation::DestroyParameters, parameters, "SDK result");
			// Continue to Shutdown exactly once; the failed ownership remains
			// visible, and the terminal flag prevents a destructor retry.
		}
		else
			m_Snapshot.capabilityParametersOwned = false;
	}
	if (m_Snapshot.initialized)
	{
		const NgxLifecycleCall shutdown = hooks.invoke(NgxLifecycleOperation::Shutdown);
		if (Failed(shutdown.result))
		{
			Failure(NgxLifecycleOperation::Shutdown, shutdown, "SDK result");
			return false;
		}
		m_Snapshot.initialized = false;
	}
	if (m_Snapshot.state != NgxSupportState::ShutdownFailure)
		m_Snapshot.reason = "shutdown complete";
	return m_Snapshot.state != NgxSupportState::ShutdownFailure;
}
