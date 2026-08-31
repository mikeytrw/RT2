#pragma once

#include "NgxSupport.h"
#include "NgxLifecycle.h"

#include "Walnut/Application.h"
#include "vulkan/vulkan.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// RT2App-only owner.  Including this header from an RT2App translation unit
// is intentional; the CPU support contract lives in NgxSupport.h instead.
class NgxRuntime final
{
public:
	NgxRuntime(std::string projectId, std::filesystem::path featurePath = {});
	~NgxRuntime();

	NgxRuntime(const NgxRuntime&) = delete;
	NgxRuntime& operator=(const NgxRuntime&) = delete;

	Walnut::Result<Walnut::OptionalVulkanFeatureRequirements> DiscoverInstanceRequirements();
	Walnut::Result<std::vector<std::string>> DiscoverDeviceRequirements(
		VkInstance instance, VkPhysicalDevice physicalDevice);

	// Called after Walnut's device exists.  No NGX feature is created or
	// evaluated by this owner.
	void InitializeAfterVulkan(bool optionalFeatureEnabled,
		const std::vector<Walnut::OptionalVulkanFeatureDiagnostic>& walnutDiagnostics);
	bool Shutdown();

	const NgxSupportSnapshot& Snapshot() const { return m_Snapshot; }
	const std::string& ProjectId() const { return m_ProjectId; }

private:
	void SetGpuName(VkPhysicalDevice physicalDevice);
	bool PrepareApplicationDataPath();
	void ObserveRuntimeVersion();

	std::string m_ProjectId;
	std::filesystem::path m_ApplicationDataPath;
	std::filesystem::path m_FeaturePath;
	std::wstring m_ApplicationDataPathNative;
	std::wstring m_FeaturePathNative;
	std::vector<const wchar_t*> m_FeaturePathPointers;
	std::string m_EngineVersion = "RT2-W1";
	NgxSupportSnapshot m_Snapshot;
	NgxLifecycleAuthority m_Lifecycle;
	VkInstance m_Instance = VK_NULL_HANDLE;
	VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
	VkDevice m_Device = VK_NULL_HANDLE;
	struct NVSDK_NGX_Parameter* m_Parameters = nullptr;
	bool m_Initialized = false;
};
