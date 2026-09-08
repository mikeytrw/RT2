#pragma once

#include "NgxSupport.h"
#include "NgxLifecycle.h"
#include "RRFeatureLifecycle.h"

#include "Walnut/Application.h"
#include "vulkan/vulkan.h"

#include <filesystem>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct RRFeatureImage
{
	VkImageView view = VK_NULL_HANDLE;
	VkImage image = VK_NULL_HANDLE;
	VkFormat format = VK_FORMAT_UNDEFINED;
	VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
	uint32_t width = 0;
	uint32_t height = 0;
};

struct RRFeatureEvaluation
{
	RRFeatureImage noisyColor;
	RRFeatureImage diffuseAlbedo;
	RRFeatureImage specularAlbedo;
	RRFeatureImage normalRoughness;
	RRFeatureImage depth;
	RRFeatureImage motion;
	RRFeatureImage specularHitDistance;
	RRFeatureImage output;
	float jitterX = 0.0f;
	float jitterY = 0.0f;
	float preExposure = 1.0f;
	float exposureScale = 1.0f;
	int reset = 1;
	float mvScaleX = 1.0f;
	float mvScaleY = 1.0f;
	float* worldToView = nullptr;
	float* viewToClip = nullptr;
};

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

	// Direct Vulkan RR seam.  The NGX parameter map and feature handle remain
	// owned here, alongside initialization/teardown; RendererGPU only supplies
	// its command buffer and validated W3 images.
	bool QueryRROptimalSettings(const OutputExtent& output,
		RROptimalSettings& settings, std::string& reason) const;
	bool CreateRRFeature(VkCommandBuffer command, const RRQualityTuple& tuple,
		std::string& reason);
	bool EvaluateRRFeature(VkCommandBuffer command, const RRFeatureEvaluation& evaluation,
		std::string& reason, int32_t* resultCode = nullptr);
	bool WaitForRRDeviceIdle(std::string& reason) const;
	bool ReleaseRRFeature(std::string& reason);
	bool HasRRFeature() const { return m_RRFeature != nullptr; }

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
	struct NVSDK_NGX_Handle* m_RRFeature = nullptr;
	bool m_Initialized = false;
};
