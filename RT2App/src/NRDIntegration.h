#pragma once

#include "vulkan/vulkan.h"
#include <cstdint>

// NRD needs NRD.h for Identifier typedef
#include "NRD.h"

namespace nri { struct Device; }

class NRDWrapper
{
public:
	NRDWrapper();
	~NRDWrapper();

	void Destroy();

	bool Init(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
	          VkQueue queue, uint32_t queueFamily, uint32_t width, uint32_t height);
	void OnResize(uint32_t width, uint32_t height);

	void NewFrame();
	void ResetHistory();

	void SetCommonSettings(const float* viewToClip, const float* viewToClipPrev,
	                       const float* worldToView, const float* worldToViewPrev,
	                       float jitterX, float jitterY,
	                       float jitterXPrev, float jitterYPrev,
	                       uint32_t frameIndex, bool reset, float splitScreen = 0.0f);

	void SetReblurSettings(float maxBlurRadius, uint32_t maxAccumulatedFrameNum,
	                       bool enableAntiFirefly, float splitScreen);

	void Denoise(VkCommandBuffer cmdBuffer,
	             VkImage inNormalRoughness, VkFormat normalRoughnessFmt,
	             VkImage inViewZ, VkFormat viewZFmt,
	             VkImage inMotion, VkFormat motionFmt,
	             VkImage inDiffRadianceHitDist, VkFormat diffFmt,
	             VkImage inSpecRadianceHitDist, VkFormat specFmt,
	             VkImage outDiffRadianceHitDist,
	             VkImage outSpecRadianceHitDist);

	bool IsAvailable() const { return m_Initialized; }
	uint32_t GetWidth() const { return m_Width; }
	uint32_t GetHeight() const { return m_Height; }

private:
	bool m_Initialized = false;
	uint32_t m_Width = 0;
	uint32_t m_Height = 0;

	nri::Device* m_NRIDevice = nullptr;
	nrd::Identifier m_DenoiserID = 0;
};