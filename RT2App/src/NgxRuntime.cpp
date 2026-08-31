#include "NgxRuntime.h"

#include "RTLog.h"

#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_defs_dlssd.h>
#include <nvsdk_ngx_helpers.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#include <filesystem>
#include <cstdlib>
#include <sstream>
#include <system_error>
#include <utility>

namespace
{
constexpr const char* kFeatureName = "DLSS-RR";

std::string Narrow(const wchar_t* value)
{
	if (!value)
		return "unknown NGX result";
#ifdef _WIN32
	const int length = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
	if (length <= 1)
		return "unknown NGX result";
	std::string result(static_cast<size_t>(length - 1), '\0');
	WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), length - 1, nullptr, nullptr);
	return result;
#else
	std::string result;
	while (*value)
	{
		result.push_back(static_cast<char>(*value));
		++value;
	}
	return result.empty() ? "unknown NGX result" : result;
#endif
}

std::string NgxResultText(NVSDK_NGX_Result result)
{
	return Narrow(GetNGXResultAsString(result));
}

bool IsRuntimeFailure(NVSDK_NGX_Result result)
{
	return result == NVSDK_NGX_Result_FAIL_NotImplemented ||
	       result == NVSDK_NGX_Result_FAIL_UnableToInitializeFeature ||
	       result == NVSDK_NGX_Result_FAIL_PlatformError;
}

NVSDK_NGX_FeatureCommonInfo MakeCommonInfo(
	const std::vector<const wchar_t*>& featurePathPointers)
{
	NVSDK_NGX_FeatureCommonInfo common{};
	common.PathListInfo.Path = featurePathPointers.empty() ? nullptr : featurePathPointers.data();
	common.PathListInfo.Length = static_cast<unsigned int>(featurePathPointers.size());
	return common;
}
}

NgxRuntime::NgxRuntime(std::string projectId, std::filesystem::path featurePath)
	: m_ProjectId(std::move(projectId)), m_FeaturePath(std::move(featurePath))
{
	if (m_ProjectId.empty())
		m_Snapshot.reason = "NGX project/application ID is missing";
	if (!m_FeaturePath.empty())
	{
		m_FeaturePathNative = m_FeaturePath.wstring();
		m_FeaturePathPointers.push_back(m_FeaturePathNative.c_str());
	}
}

NgxRuntime::~NgxRuntime()
{
	Shutdown();
}

void NgxRuntime::SetGpuName(VkPhysicalDevice physicalDevice)
{
	if (physicalDevice == VK_NULL_HANDLE)
		return;
	VkPhysicalDeviceProperties properties{};
	vkGetPhysicalDeviceProperties(physicalDevice, &properties);
	m_Snapshot.gpuName = properties.deviceName;
}

void NgxRuntime::SetFailure(NgxSupportState state, std::string reason,
	int32_t ngxResult, int32_t vulkanResult)
{
	m_Snapshot.state = state;
	m_Snapshot.reason = std::move(reason);
	m_Snapshot.ngxResult = ngxResult;
	m_Snapshot.vulkanResult = vulkanResult;
	m_Snapshot.initialized = m_Initialized;
	m_Snapshot.capabilityParametersOwned = m_Parameters != nullptr;
}

void NgxRuntime::SetFailureFromNgx(int32_t result, const char* operation)
{
	const NVSDK_NGX_Result ngxResult = static_cast<NVSDK_NGX_Result>(result);
	NgxSupportState state = NgxSupportState::RequirementQueryFailure;
	if (result == static_cast<int32_t>(NVSDK_NGX_Result_FAIL_InvalidParameter))
		state = NgxSupportState::NeedsApplicationId;
	else if (result == static_cast<int32_t>(NVSDK_NGX_Result_FAIL_OutOfDate))
		state = NgxSupportState::DriverUpdateRequired;
	else if (IsRuntimeFailure(ngxResult))
		state = NgxSupportState::RuntimeMissing;
	SetFailure(state, std::string(operation) + " failed: " + NgxResultText(ngxResult), result);
}

bool NgxRuntime::PrepareApplicationDataPath()
{
	if (m_ApplicationDataPath.empty())
	{
#ifdef _WIN32
		const wchar_t* localAppData = _wgetenv(L"LOCALAPPDATA");
		if (localAppData && *localAppData)
			m_ApplicationDataPath = std::filesystem::path(localAppData) / L"RT2" / L"NGX";
#else
		const char* localAppData = std::getenv("LOCALAPPDATA");
		if (localAppData && *localAppData)
			m_ApplicationDataPath = std::filesystem::path(localAppData) / "RT2" / "NGX";
#endif
		if (m_ApplicationDataPath.empty())
			m_ApplicationDataPath = std::filesystem::current_path() / "RT2" / "NGX";
	}
	std::error_code error;
	std::filesystem::create_directories(m_ApplicationDataPath, error);
	if (error)
	{
		SetFailure(NgxSupportState::InitializationFailure,
			"cannot create NGX application-data path '" + m_ApplicationDataPath.u8string() + "': " + error.message());
		return false;
	}
	m_ApplicationDataPathNative = m_ApplicationDataPath.wstring();
	return true;
}

Walnut::Result<Walnut::OptionalVulkanFeatureRequirements> NgxRuntime::DiscoverInstanceRequirements()
{
	if (m_ProjectId.empty())
	{
		SetFailure(NgxSupportState::NeedsApplicationId, "NGX project/application ID is missing");
		return Walnut::Result<Walnut::OptionalVulkanFeatureRequirements>::Failure(m_Snapshot.reason);
	}
	if (!PrepareApplicationDataPath())
		return Walnut::Result<Walnut::OptionalVulkanFeatureRequirements>::Failure(m_Snapshot.reason);

	const NVSDK_NGX_FeatureCommonInfo common = MakeCommonInfo(m_FeaturePathPointers);
	NVSDK_NGX_FeatureDiscoveryInfo discovery{};
	discovery.SDKVersion = NVSDK_NGX_Version_API;
	discovery.FeatureID = NVSDK_NGX_Feature_RayReconstruction;
	discovery.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
	discovery.Identifier.v.ProjectDesc.ProjectId = m_ProjectId.c_str();
	discovery.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
	discovery.Identifier.v.ProjectDesc.EngineVersion = m_EngineVersion.c_str();
	discovery.ApplicationDataPath = m_ApplicationDataPathNative.c_str();
	discovery.FeatureInfo = &common;

	uint32_t count = 0;
	VkExtensionProperties* properties = nullptr;
	const NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(
		&discovery, &count, &properties);
	if (NVSDK_NGX_FAILED(result))
	{
		SetFailureFromNgx(static_cast<int32_t>(result), "NGX instance-extension discovery");
		return Walnut::Result<Walnut::OptionalVulkanFeatureRequirements>::Failure(m_Snapshot.reason);
	}

	Walnut::OptionalVulkanFeatureRequirements requirements;
	requirements.featureName = kFeatureName;
	for (uint32_t i = 0; i < count; ++i)
		requirements.instanceExtensions.emplace_back(properties[i].extensionName);
	requirements.deviceExtensions = [this](VkInstance instance, VkPhysicalDevice physicalDevice) {
		return DiscoverDeviceRequirements(instance, physicalDevice);
	};
	m_Snapshot.reason = "Vulkan instance requirements discovered";
	return Walnut::Result<Walnut::OptionalVulkanFeatureRequirements>::Success(std::move(requirements));
}

Walnut::Result<std::vector<std::string>> NgxRuntime::DiscoverDeviceRequirements(
	VkInstance instance, VkPhysicalDevice physicalDevice)
{
	if (m_ProjectId.empty())
	{
		SetFailure(NgxSupportState::NeedsApplicationId, "NGX project/application ID is missing");
		return Walnut::Result<std::vector<std::string>>::Failure(m_Snapshot.reason);
	}
	const NVSDK_NGX_FeatureCommonInfo common = MakeCommonInfo(m_FeaturePathPointers);
	NVSDK_NGX_FeatureDiscoveryInfo discovery{};
	discovery.SDKVersion = NVSDK_NGX_Version_API;
	discovery.FeatureID = NVSDK_NGX_Feature_RayReconstruction;
	discovery.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
	discovery.Identifier.v.ProjectDesc.ProjectId = m_ProjectId.c_str();
	discovery.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
	discovery.Identifier.v.ProjectDesc.EngineVersion = m_EngineVersion.c_str();
	discovery.ApplicationDataPath = m_ApplicationDataPathNative.c_str();
	discovery.FeatureInfo = &common;

	uint32_t count = 0;
	VkExtensionProperties* properties = nullptr;
	const NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
		instance, physicalDevice, &discovery, &count, &properties);
	if (NVSDK_NGX_FAILED(result))
	{
		SetFailureFromNgx(static_cast<int32_t>(result), "NGX device-extension discovery");
		return Walnut::Result<std::vector<std::string>>::Failure(m_Snapshot.reason);
	}
	std::vector<std::string> extensions;
	extensions.reserve(count);
	for (uint32_t i = 0; i < count; ++i)
		extensions.emplace_back(properties[i].extensionName);
	return Walnut::Result<std::vector<std::string>>::Success(std::move(extensions));
}

void NgxRuntime::InitializeAfterVulkan(bool optionalFeatureEnabled,
	const std::vector<Walnut::OptionalVulkanFeatureDiagnostic>& walnutDiagnostics)
{
	m_Instance = Walnut::Application::GetInstance();
	m_PhysicalDevice = Walnut::Application::GetPhysicalDevice();
	m_Device = Walnut::Application::GetDevice();
	SetGpuName(m_PhysicalDevice);

	if (!optionalFeatureEnabled)
	{
		for (const auto& diagnostic : walnutDiagnostics)
		{
			if (diagnostic.reason == Walnut::OptionalVulkanFeatureDisableReason::MissingExtension)
			{
				SetFailure(NgxSupportState::MissingVulkanRequirement,
					diagnostic.message.empty() ? "one or more NGX Vulkan requirements are unavailable" : diagnostic.message,
					0, static_cast<int32_t>(diagnostic.vkResult));
				return;
			}
		}
		if (m_Snapshot.state != NgxSupportState::NotProbed &&
			m_Snapshot.state != NgxSupportState::Supported)
			return;
		SetFailure(NgxSupportState::RequirementQueryFailure,
			"Walnut disabled NGX optional Vulkan requirements");
		return;
	}
	if (m_ProjectId.empty())
	{
		SetFailure(NgxSupportState::NeedsApplicationId, "NGX project/application ID is missing");
		return;
	}
	if (m_Instance == VK_NULL_HANDLE || m_PhysicalDevice == VK_NULL_HANDLE || m_Device == VK_NULL_HANDLE)
	{
		SetFailure(NgxSupportState::InitializationFailure, "Walnut did not provide a complete Vulkan device");
		return;
	}
	if (!PrepareApplicationDataPath())
		return;

	const NVSDK_NGX_FeatureCommonInfo common = MakeCommonInfo(m_FeaturePathPointers);
	NVSDK_NGX_FeatureDiscoveryInfo discovery{};
	discovery.SDKVersion = NVSDK_NGX_Version_API;
	discovery.FeatureID = NVSDK_NGX_Feature_RayReconstruction;
	discovery.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
	discovery.Identifier.v.ProjectDesc.ProjectId = m_ProjectId.c_str();
	discovery.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
	discovery.Identifier.v.ProjectDesc.EngineVersion = m_EngineVersion.c_str();
	discovery.ApplicationDataPath = m_ApplicationDataPathNative.c_str();
	discovery.FeatureInfo = &common;
	NVSDK_NGX_FeatureRequirement requirement{};
	const NVSDK_NGX_Result requirementResult = NVSDK_NGX_VULKAN_GetFeatureRequirements(
		m_Instance, m_PhysicalDevice, &discovery, &requirement);
	if (NVSDK_NGX_FAILED(requirementResult))
	{
		SetFailureFromNgx(static_cast<int32_t>(requirementResult), "NGX Ray Reconstruction support query");
		return;
	}
	m_Snapshot.supportMask = static_cast<uint32_t>(requirement.FeatureSupported);
	if (m_Snapshot.supportMask != 0)
	{
		SetFailure(NgxSupportStateFromMask(m_Snapshot.supportMask),
			NgxSupportReasonFromMask(m_Snapshot.supportMask), static_cast<int32_t>(requirementResult));
		return;
	}

	const NVSDK_NGX_Result initResult = NVSDK_NGX_VULKAN_Init_with_ProjectID(
		m_ProjectId.c_str(), NVSDK_NGX_ENGINE_TYPE_CUSTOM, m_EngineVersion.c_str(),
		m_ApplicationDataPathNative.c_str(), m_Instance, m_PhysicalDevice, m_Device,
		vkGetInstanceProcAddr, vkGetDeviceProcAddr, &common, NVSDK_NGX_Version_API);
	if (NVSDK_NGX_FAILED(initResult))
	{
		SetFailureFromNgx(static_cast<int32_t>(initResult), "NVSDK_NGX_VULKAN_Init_with_ProjectID");
		return;
	}
	m_Initialized = true;
	m_Snapshot.initialized = true;

	const NVSDK_NGX_Result parametersResult = NVSDK_NGX_VULKAN_GetCapabilityParameters(&m_Parameters);
	if (NVSDK_NGX_FAILED(parametersResult) || !m_Parameters)
	{
		SetFailure(NgxSupportState::ParameterFailure,
			"NVSDK_NGX_VULKAN_GetCapabilityParameters failed: " + NgxResultText(parametersResult),
			static_cast<int32_t>(parametersResult));
		return;
	}
	m_Snapshot.capabilityParametersOwned = true;

	unsigned int available = 0;
	unsigned int needsUpdatedDriver = 0;
	const NVSDK_NGX_Result availableResult = m_Parameters->Get(
		NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &available);
	const NVSDK_NGX_Result driverResult = m_Parameters->Get(
		NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver, &needsUpdatedDriver);
	if (NVSDK_NGX_FAILED(availableResult))
	{
		SetFailure(NgxSupportState::ParameterFailure,
			"capability parameter SuperSamplingDenoising.Available failed: " + NgxResultText(availableResult),
			static_cast<int32_t>(availableResult));
		return;
	}
	if (NVSDK_NGX_FAILED(driverResult))
	{
		SetFailure(NgxSupportState::ParameterFailure,
			"capability parameter SuperSamplingDenoising.NeedsUpdatedDriver failed: " + NgxResultText(driverResult),
			static_cast<int32_t>(driverResult));
		return;
	}
	if (needsUpdatedDriver != 0)
	{
		SetFailure(NgxSupportState::DriverUpdateRequired,
			"driver update required (SuperSamplingDenoising.NeedsUpdatedDriver=" + std::to_string(needsUpdatedDriver) + ")");
		return;
	}
	if (available == 0)
	{
		SetFailure(NgxSupportState::UnsupportedGpuVendor,
			"Ray Reconstruction unavailable (SuperSamplingDenoising.Available=0)");
		return;
	}
	m_Snapshot.state = NgxSupportState::Supported;
	m_Snapshot.reason = "supported";
	m_Snapshot.ngxResult = static_cast<int32_t>(NVSDK_NGX_Result_Success);
	m_Snapshot.initialized = true;
	m_Snapshot.capabilityParametersOwned = true;
}

void NgxRuntime::Shutdown()
{
	if (!m_Initialized && !m_Parameters)
		return;
	if (m_Device != VK_NULL_HANDLE)
	{
		const VkResult idleResult = vkDeviceWaitIdle(m_Device);
		if (idleResult != VK_SUCCESS)
		{
			SetFailure(NgxSupportState::ShutdownFailure,
				"vkDeviceWaitIdle before NGX shutdown failed", m_Snapshot.ngxResult,
				static_cast<int32_t>(idleResult));
			return;
		}
	}
	bool parameterFailure = false;
	if (m_Parameters)
	{
		const NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_DestroyParameters(m_Parameters);
		if (NVSDK_NGX_FAILED(result))
		{
			parameterFailure = true;
			SetFailure(NgxSupportState::ShutdownFailure,
				"NVSDK_NGX_VULKAN_DestroyParameters failed: " + NgxResultText(result),
				static_cast<int32_t>(result));
		}
		else
		{
			m_Parameters = nullptr;
			m_Snapshot.capabilityParametersOwned = false;
		}
	}
	if (m_Initialized)
	{
		const NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_Shutdown1(m_Device);
		if (NVSDK_NGX_FAILED(result))
		{
			SetFailure(NgxSupportState::ShutdownFailure,
				"NVSDK_NGX_VULKAN_Shutdown1 failed: " + NgxResultText(result),
				static_cast<int32_t>(result));
		}
		else
		{
			m_Initialized = false;
			m_Snapshot.initialized = false;
			if (!parameterFailure)
				m_Snapshot.reason = "shutdown complete";
		}
	}
}
