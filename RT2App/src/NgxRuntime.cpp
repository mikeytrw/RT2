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
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

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

NVSDK_NGX_FeatureCommonInfo MakeCommonInfo(
	const std::vector<const wchar_t*>& featurePathPointers)
{
	NVSDK_NGX_FeatureCommonInfo common{};
	common.PathListInfo.Path = featurePathPointers.empty() ? nullptr : featurePathPointers.data();
	common.PathListInfo.Length = static_cast<unsigned int>(featurePathPointers.size());
	return common;
}

}

void NgxRuntime::ObserveRuntimeVersion()
{
#ifdef _WIN32
	HMODULE module = GetModuleHandleW(L"nvngx_dlssd.dll");
	if (!module)
		module = GetModuleHandleW(L"nvngx_dlss.dll");
	if (!module)
	{
		m_Lifecycle.SetRuntimeVersion("unavailable");
		return;
	}
	wchar_t modulePath[MAX_PATH] = {};
	const DWORD pathLength = GetModuleFileNameW(module, modulePath, MAX_PATH);
	if (pathLength == 0 || pathLength >= MAX_PATH)
	{
		m_Lifecycle.SetRuntimeVersion("unavailable");
		return;
	}
	DWORD ignored = 0;
	const DWORD versionSize = GetFileVersionInfoSizeW(modulePath, &ignored);
	if (versionSize == 0)
	{
		m_Lifecycle.SetRuntimeVersion("unavailable");
		return;
	}
	std::vector<unsigned char> versionData(versionSize);
	if (!GetFileVersionInfoW(modulePath, 0, versionSize, versionData.data()))
	{
		m_Lifecycle.SetRuntimeVersion("unavailable");
		return;
	}
	VS_FIXEDFILEINFO* info = nullptr;
	UINT infoSize = 0;
	if (!VerQueryValueW(versionData.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize) ||
		!info || infoSize < sizeof(VS_FIXEDFILEINFO))
	{
		m_Lifecycle.SetRuntimeVersion("unavailable");
		return;
	}
	m_Lifecycle.SetRuntimeVersion(std::to_string(HIWORD(info->dwFileVersionMS)) + "." +
		std::to_string(LOWORD(info->dwFileVersionMS)) + "." +
		std::to_string(HIWORD(info->dwFileVersionLS)) + "." +
		std::to_string(LOWORD(info->dwFileVersionLS)));
#else
	m_Lifecycle.SetRuntimeVersion("unavailable");
#endif
}

NgxRuntime::NgxRuntime(std::string projectId, std::filesystem::path featurePath)
	: m_ProjectId(std::move(projectId)), m_FeaturePath(std::move(featurePath)),
	  m_Lifecycle(m_Snapshot)
{
	if (m_ProjectId.empty())
		m_Lifecycle.ExternalFailure(NgxSupportState::NeedsApplicationId,
			"NGX project/application ID is missing");
	if (!m_FeaturePath.empty())
	{
		m_FeaturePathNative = m_FeaturePath.wstring();
		m_FeaturePathPointers.push_back(m_FeaturePathNative.c_str());
	}
}

NgxRuntime::~NgxRuntime()
{
	(void)Shutdown();
}

void NgxRuntime::SetGpuName(VkPhysicalDevice physicalDevice)
{
	if (physicalDevice == VK_NULL_HANDLE)
		return;
	VkPhysicalDeviceProperties properties{};
	vkGetPhysicalDeviceProperties(physicalDevice, &properties);
	m_Lifecycle.SetGpuName(properties.deviceName);
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
		{
			m_Lifecycle.ExternalFailure(NgxSupportState::ApplicationDataPathFailure,
				"LOCALAPPDATA is missing or empty");
			return false;
		}
	}
	std::error_code error;
	std::filesystem::create_directories(m_ApplicationDataPath, error);
	if (error)
	{
		m_Lifecycle.ExternalFailure(NgxSupportState::ApplicationDataPathFailure,
			"cannot create NGX application-data path '" + m_ApplicationDataPath.u8string() + "': " + error.message());
		return false;
	}
	const auto marker = m_ApplicationDataPath / ".rt2-ngx-write-test";
	{
		std::ofstream writable(marker, std::ios::binary | std::ios::trunc);
		if (!writable)
		{
			m_Lifecycle.ExternalFailure(NgxSupportState::ApplicationDataPathFailure,
				"NGX application-data path is not writable: '" + m_ApplicationDataPath.u8string() + "'");
			return false;
		}
	}
	std::filesystem::remove(marker, error);
	if (error)
	{
		m_Lifecycle.ExternalFailure(NgxSupportState::ApplicationDataPathFailure,
			"cannot remove NGX application-data write probe: " + error.message());
		return false;
	}
	m_ApplicationDataPathNative = m_ApplicationDataPath.wstring();
	return true;
}

Walnut::Result<Walnut::OptionalVulkanFeatureRequirements> NgxRuntime::DiscoverInstanceRequirements()
{
	if (m_ProjectId.empty())
	{
		m_Lifecycle.ExternalFailure(NgxSupportState::NeedsApplicationId, "NGX project/application ID is missing");
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
		m_Lifecycle.OperationFailure(NgxLifecycleOperation::RequirementQuery,
			{ static_cast<int32_t>(result) }, NgxResultText(result).c_str());
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
		m_Lifecycle.ExternalFailure(NgxSupportState::NeedsApplicationId, "NGX project/application ID is missing");
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
		m_Lifecycle.OperationFailure(NgxLifecycleOperation::RequirementQuery,
			{ static_cast<int32_t>(result) }, NgxResultText(result).c_str());
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
				m_Lifecycle.ExternalFailure(NgxSupportState::MissingVulkanRequirement,
					diagnostic.message.empty() ? "one or more NGX Vulkan requirements are unavailable" : diagnostic.message,
					0, static_cast<int32_t>(diagnostic.vkResult));
				return;
			}
		}
		if (m_Snapshot.state != NgxSupportState::NotProbed &&
			m_Snapshot.state != NgxSupportState::Supported)
			return;
		m_Lifecycle.ExternalFailure(NgxSupportState::RequirementQueryFailure,
			"Walnut disabled NGX optional Vulkan requirements");
		return;
	}
	if (m_ProjectId.empty())
	{
		m_Lifecycle.ExternalFailure(NgxSupportState::NeedsApplicationId, "NGX project/application ID is missing");
		return;
	}
	if (m_Instance == VK_NULL_HANDLE || m_PhysicalDevice == VK_NULL_HANDLE || m_Device == VK_NULL_HANDLE)
	{
		m_Lifecycle.ExternalFailure(NgxSupportState::InitializationFailure, "Walnut did not provide a complete Vulkan device");
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
	NgxLifecycleHooks hooks;
	hooks.invoke = [this, &discovery, &common, &requirement](NgxLifecycleOperation operation) {
		NgxLifecycleCall call;
		switch (operation)
		{
		case NgxLifecycleOperation::RequirementQuery:
			{
				const NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_GetFeatureRequirements(
					m_Instance, m_PhysicalDevice, &discovery, &requirement);
				call.result = static_cast<int32_t>(result);
				call.detail = NgxResultText(result);
			}
			call.supportMask = static_cast<uint32_t>(requirement.FeatureSupported);
			break;
		case NgxLifecycleOperation::Initialize:
			{
				const NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_Init_with_ProjectID(
				m_ProjectId.c_str(), NVSDK_NGX_ENGINE_TYPE_CUSTOM, m_EngineVersion.c_str(),
				m_ApplicationDataPathNative.c_str(), m_Instance, m_PhysicalDevice, m_Device,
				vkGetInstanceProcAddr, vkGetDeviceProcAddr, &common, NVSDK_NGX_Version_API);
				call.result = static_cast<int32_t>(result);
				call.detail = NgxResultText(result);
			}
			m_Initialized = static_cast<uint32_t>(call.result) == 0x1u;
			break;
		case NgxLifecycleOperation::CapabilityParameters:
			{
				const NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&m_Parameters);
				call.result = static_cast<int32_t>(result);
				call.detail = NgxResultText(result);
			}
			call.parametersOwned = m_Parameters != nullptr;
			break;
		case NgxLifecycleOperation::Availability:
		{
			unsigned int available = 0;
			const NVSDK_NGX_Result result = m_Parameters->Get(
				NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &available);
			call.result = static_cast<int32_t>(result);
			call.detail = NgxResultText(result);
			call.available = available != 0;
			VkPhysicalDeviceProperties properties{};
			vkGetPhysicalDeviceProperties(m_PhysicalDevice, &properties);
			call.unsupportedVendor = properties.vendorID != 0x10DE;
			break;
		}
		case NgxLifecycleOperation::Driver:
		{
			unsigned int needsUpdatedDriver = 0;
			const NVSDK_NGX_Result result = m_Parameters->Get(
				NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver,
				&needsUpdatedDriver);
			call.result = static_cast<int32_t>(result);
			call.detail = NgxResultText(result);
			call.needsUpdatedDriver = needsUpdatedDriver != 0;
			break;
		}
		default:
			break;
		}
		return call;
	};
	if (m_Lifecycle.Probe(hooks))
		ObserveRuntimeVersion();
}

bool NgxRuntime::Shutdown()
{
	if ((!m_Initialized && !m_Parameters) || m_Lifecycle.CleanupAttempted())
		return m_Snapshot.state != NgxSupportState::ShutdownFailure;
	NgxLifecycleHooks hooks;
	hooks.invoke = [this](NgxLifecycleOperation operation) {
		NgxLifecycleCall call;
		switch (operation)
		{
		case NgxLifecycleOperation::WaitIdle:
			if (m_Device == VK_NULL_HANDLE)
			{
				call.result = 1;
				call.vulkanResult = 0;
			}
			else
			{
				const VkResult result = vkDeviceWaitIdle(m_Device);
				call.result = result == VK_SUCCESS ? 1 : 0;
				call.vulkanResult = static_cast<int32_t>(result);
			}
			call.detail = "Vulkan result";
			break;
		case NgxLifecycleOperation::DestroyParameters:
			{
				const NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_DestroyParameters(m_Parameters);
				call.result = static_cast<int32_t>(result);
				call.detail = NgxResultText(result);
			}
			if (static_cast<uint32_t>(call.result) == 0x1u) m_Parameters = nullptr;
			break;
		case NgxLifecycleOperation::Shutdown:
			{
				const NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_Shutdown1(m_Device);
				call.result = static_cast<int32_t>(result);
				call.detail = NgxResultText(result);
			}
			if (static_cast<uint32_t>(call.result) == 0x1u) m_Initialized = false;
			break;
		default:
			break;
		}
		return call;
	};
	const bool shutdownOk = m_Lifecycle.Shutdown(hooks);
	const std::string report = m_Snapshot.Format();
	printf("[NGX] %s\n", report.c_str());
	RT_LOG("[NGX] %s", report.c_str());
	return shutdownOk;
}
