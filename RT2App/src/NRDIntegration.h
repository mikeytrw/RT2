#pragma once

#include "vulkan/vulkan.h"
#include <cstdint>
#include "RenderExtents.h"

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
	          VkQueue queue, uint32_t queueFamily, const RenderExtent& extent);
	void OnResize(const RenderExtent& extent);

	void NewFrame();
	void ResetHistory();

	void SetCommonSettings(const float* viewToClip, const float* viewToClipPrev,
	                       const float* worldToView, const float* worldToViewPrev,
	                       float jitterX, float jitterY,
	                       float jitterXPrev, float jitterYPrev,
	                       uint32_t frameIndex, bool reset, float splitScreen = 0.0f);

	void SetReblurSettings(float maxBlurRadius, uint32_t maxAccumulatedFrameNum,
	                       float responsiveRoughnessThreshold,
	                       uint32_t responsiveMinAccumulatedFrameNum,
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
	RenderExtent GetExtent() const { return m_Extent; }

private:
	bool m_Initialized = false;
	RenderExtent m_Extent;

	// Cached device handles for OnResize
	VkInstance      m_Instance       = VK_NULL_HANDLE;
	VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
	VkDevice        m_Device         = VK_NULL_HANDLE;
	VkQueue         m_Queue          = VK_NULL_HANDLE;
	uint32_t        m_QueueFamily    = 0;

	nri::Device* m_NRIDevice = nullptr;
	nrd::Identifier m_DenoiserID = 0;
};
