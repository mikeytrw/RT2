#pragma once

#include "NgxSupport.h"

#include <cstdint>
#include <functional>
#include <string>

// CPU-only lifecycle vocabulary. The production owner supplies lambdas that
// call the SDK/Vulkan APIs; tests supply deterministic hooks instead.
enum class NgxLifecycleOperation
{
	RequirementQuery,
	Initialize,
	CapabilityParameters,
	Availability,
	Driver,
	WaitIdle,
	DestroyParameters,
	Shutdown,
};

struct NgxLifecycleCall
{
	int32_t result = 1;
	int32_t vulkanResult = 0;
	uint32_t supportMask = 0;
	bool parametersOwned = false;
	bool available = false;
	bool needsUpdatedDriver = false;
	bool unsupportedVendor = false;
	// Optional producer-supplied SDK/Vulkan text.  Keeping it on the call lets
	// the CPU authority publish the exact production failure without importing
	// NGX headers or result-string helpers into test targets.
	std::string detail;
};

struct NgxLifecycleHooks
{
	std::function<NgxLifecycleCall(NgxLifecycleOperation)> invoke;
};

class NgxLifecycleAuthority final
{
public:
	explicit NgxLifecycleAuthority(NgxSupportSnapshot& snapshot);

	// Runs the complete requirement/init/parameter/capability proof. Every
	// result is classified with its operation context and retained verbatim.
	bool Probe(const NgxLifecycleHooks& hooks);

	// Performs exactly one pre-device-destruction cleanup attempt. Once this
	// returns, the authority is terminal and a destructor cannot retry calls.
	bool Shutdown(const NgxLifecycleHooks& hooks);

	void ExternalFailure(NgxSupportState state, std::string reason,
		int32_t ngxResult = 0, int32_t vulkanResult = 0);
	void OperationFailure(NgxLifecycleOperation operation,
	const NgxLifecycleCall& call, std::string detail);
	void SetGpuName(std::string name);
	void SetRuntimeVersion(std::string version);
	bool CleanupAttempted() const { return m_CleanupAttempted; }

private:
	void Failure(NgxLifecycleOperation operation, const NgxLifecycleCall& call,
		std::string detail);
	void SetState(NgxSupportState state, std::string reason,
		int32_t ngxResult, int32_t vulkanResult);

	NgxSupportSnapshot& m_Snapshot;
	bool m_CleanupAttempted = false;
};
