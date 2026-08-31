#pragma once

#include "GpuResources.h"
#include "RenderExtents.h"
#include "RRGuideContract.h"
#include <string>

struct GpuDevice;

class RRGuideResources
{
public:
	~RRGuideResources() { Destroy(); }
	RRGuideResources() = default;
	RRGuideResources(const RRGuideResources&) = delete;
	RRGuideResources& operator=(const RRGuideResources&) = delete;

	bool Create(const GpuDevice& device, const RenderExtent& extent);
	void Destroy();
	bool IsValid() const;
	RenderExtent GetExtent() const { return m_Extent; }
	const GpuImage& Get(RRGuideKind kind) const { return m_Images[DedicatedIndex(kind)]; }
	GpuImage& Get(RRGuideKind kind) { return m_Images[DedicatedIndex(kind)]; }
	uint32_t GetImageCount() const { return RR_GUIDE_DEDICATED_COUNT; }
	uint32_t GetAllocationCount() const { return m_AllocationCount; }
	VkDeviceSize GetAllocationBytes() const { return m_AllocationBytes; }
	bool WithinBudget() const;
	// Synchronously reads all five dedicated guides plus shared depth/motion and
	// emits a checked machine-readable report. This path is diagnostic-only.
	bool WriteReport(const std::string& path, const GpuImage& sharedDepth,
		const GpuImage& sharedMotion, const GpuImage& canonicalOutput) const;

private:
	static size_t DedicatedIndex(RRGuideKind kind);
	GpuImage m_Images[RR_GUIDE_DEDICATED_COUNT]{};
	const GpuDevice* m_Device = nullptr;
	RenderExtent m_Extent;
	uint32_t m_AllocationCount = 0;
	VkDeviceSize m_AllocationBytes = 0;
};
