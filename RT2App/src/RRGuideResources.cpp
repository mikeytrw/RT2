#include "RRGuideResources.h"

#include "GpuDevice.h"
#include "CommandUtils.h"
#include "RTLog.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <cstdlib>

namespace
{
constexpr RRGuideKind kDedicated[] = {
	RRGuideKind::NoisyHdr, RRGuideKind::DiffuseAlbedo, RRGuideKind::SpecularAlbedo,
	RRGuideKind::NormalRoughness, RRGuideKind::SpecularHitDistance
};
constexpr VkFormat kFormats[] = {
	VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_R8G8B8A8_UNORM,
	VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT,
	VK_FORMAT_R32_SFLOAT
};
}

size_t RRGuideResources::DedicatedIndex(RRGuideKind kind)
{
	for (size_t i = 0; i < RR_GUIDE_DEDICATED_COUNT; ++i)
		if (kDedicated[i] == kind) return i;
	return 0;
}

bool RRGuideResources::Create(const GpuDevice& device, const RenderExtent& extent)
{
	Destroy();
	if (!extent.IsValid()) return false;
	// The pinned contract requires storage-image production and checked transfer
	// readback. Reject the selected device/format before allocating anything.
	const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
		VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
	for (VkFormat format : kFormats)
	{
		VkFormatProperties formatProperties{};
		vkGetPhysicalDeviceFormatProperties(device.physicalDevice, format, &formatProperties);
		if ((formatProperties.optimalTilingFeatures & required) != required)
		{
			RT_LOG("[RRGuides] selected device lacks required storage/transfer format support: %d", (int)format);
			return false;
		}
	}
	m_Device = &device;
	m_Extent = extent;
	const VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
		VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	for (size_t i = 0; i < RR_GUIDE_DEDICATED_COUNT; ++i)
	{
		if (!GpuResources::CreateImage(device, extent.Width(), extent.Height(), kFormats[i], usage,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_Images[i]))
		{
			RT_LOG("[RRGuides] failed to create guide %zu", i);
			Destroy();
			return false;
		}
		++m_AllocationCount;
		m_AllocationBytes += m_Images[i].allocationSize;
	}
	CommandUtils::ImmediateSubmit(device, [&](VkCommandBuffer cmd) {
		for (auto& image : m_Images)
		{
			VkImageMemoryBarrier barrier{};
			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
			barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = image.image;
			barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			barrier.subresourceRange.levelCount = 1;
			barrier.subresourceRange.layerCount = 1;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
				0, 0, nullptr, 0, nullptr, 1, &barrier);
		}
	});
	return true;
}

void RRGuideResources::Destroy()
{
	if (m_Device)
		for (auto& image : m_Images) GpuResources::DestroyImage(*m_Device, image);
	m_Device = nullptr;
	m_Extent = {};
	m_AllocationCount = 0;
	m_AllocationBytes = 0;
}

bool RRGuideResources::IsValid() const
{
	for (const auto& image : m_Images) if (!image.IsValid()) return false;
	return m_Device != nullptr;
}

bool RRGuideResources::WithinBudget() const
{
	return m_AllocationCount <= RR_GUIDE_MAX_ALLOCATIONS &&
		m_AllocationBytes <= RR_GUIDE_MAX_RT2_BYTES;
}

namespace
{
uint16_t ReadU16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
float HalfToFloat(uint16_t h)
{
	const uint32_t sign = (uint32_t(h & 0x8000u) << 16);
	uint32_t exp = (h >> 10) & 0x1Fu;
	uint32_t mant = h & 0x3FFu;
	uint32_t out;
	if (exp == 0)
	{
		if (!mant) out = sign;
		else
		{
			exp = 1;
			while ((mant & 0x400u) == 0) { mant <<= 1; --exp; }
			mant &= 0x3FFu;
			out = sign | ((exp + 112u) << 23) | (mant << 13);
		}
	}
	else if (exp == 31) out = sign | 0x7F800000u | (mant << 13);
	else out = sign | ((exp + 112u) << 23) | (mant << 13);
	float f; std::memcpy(&f, &out, sizeof(f)); return f;
}

float UFloatToFloat(uint32_t bits, uint32_t mantissaBits)
{
	const uint32_t mantissaMask = (1u << mantissaBits) - 1u;
	const uint32_t mantissa = bits & mantissaMask;
	const uint32_t exponent = (bits >> mantissaBits) & 0x1Fu;
	if (exponent == 0u)
		return std::ldexp(float(mantissa), -14 - int(mantissaBits));
	if (exponent == 31u)
		return (mantissa == 0u) ? std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN();
	return std::ldexp(1.0f + float(mantissa) / float(1u << mantissaBits), int(exponent) - 15);
}

struct Stats { uint64_t finite = 0, nonfinite = 0, zero = 0, nonzero = 0; float min = std::numeric_limits<float>::infinity(); float max = -std::numeric_limits<float>::infinity(); };
void Add(Stats& s, float v)
{
	if (std::isfinite(v)) { ++s.finite; s.min = std::min(s.min, v); s.max = std::max(s.max, v); }
	else ++s.nonfinite;
	if (v == 0.0f) ++s.zero; else ++s.nonzero;
}
}

bool RRGuideResources::WriteReport(const std::string& path, const GpuImage& sharedDepth,
	const GpuImage& sharedMotion, const GpuImage& canonicalOutput) const
{
	if (!m_Device || !IsValid() || !WithinBudget() || !sharedDepth.IsValid() ||
		!sharedMotion.IsValid() || !canonicalOutput.IsValid())
		return false;
	if (vkDeviceWaitIdle(m_Device->device) != VK_SUCCESS)
	{
		RT_LOG("[RRGuides] report vkDeviceWaitIdle failed"); return false;
	}
	struct Entry { RRGuideKind kind; const GpuImage* image; } entries[] = {
		{ RRGuideKind::NoisyHdr, &m_Images[0] }, { RRGuideKind::DiffuseAlbedo, &m_Images[1] },
		{ RRGuideKind::SpecularAlbedo, &m_Images[2] }, { RRGuideKind::NormalRoughness, &m_Images[3] },
		{ RRGuideKind::LinearDepth, &sharedDepth }, { RRGuideKind::Motion, &sharedMotion },
		{ RRGuideKind::SpecularHitDistance, &m_Images[4] }
	};
	VkPhysicalDeviceProperties properties{};
	vkGetPhysicalDeviceProperties(m_Device->physicalDevice, &properties);
	std::ostringstream json;
	Stats statsByGuide[7]{};
	uint64_t componentCountByGuide[7]{};
	json << "{\"schema\":\"rt2.rr-guide-report.v1\",\"gpu\":{\"name\":\"" << properties.deviceName
		<< "\",\"vendor_id\":" << properties.vendorID << ",\"device_id\":" << properties.deviceID
		<< ",\"api_version\":" << properties.apiVersion << ",\"driver_version\":" << properties.driverVersion
		<< "},\"render_extent\":{"
		<< "\"width\":" << m_Extent.Width() << ",\"height\":" << m_Extent.Height() << "},\"guides\":[";
	for (size_t e = 0; e < 7; ++e)
	{
		const GpuImage& image = *entries[e].image;
		const RRGuideContract& contract = GetRRGuideContract(entries[e].kind);
		uint32_t bpp = 4;
		switch (image.format) { case VK_FORMAT_R16G16B16A16_SFLOAT: bpp = 8; break; case VK_FORMAT_R16G16_SFLOAT: bpp = 4; break; case VK_FORMAT_R8G8B8A8_UNORM: bpp = 4; break; case VK_FORMAT_B10G11R11_UFLOAT_PACK32: bpp = 4; break; default: bpp = 4; break; }
		VkDeviceSize size = VkDeviceSize(image.width) * image.height * bpp;
		GpuBuffer staging;
		if (!GpuResources::CreateBuffer(*m_Device, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging)) return false;
		CommandUtils::ImmediateSubmit(*m_Device, [&](VkCommandBuffer cmd) {
			GpuResources::TransitionImage(cmd, image.image, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
				VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
			VkBufferImageCopy copy{}; copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; copy.imageSubresource.layerCount = 1;
			copy.imageExtent = { image.width, image.height, 1 };
			vkCmdCopyImageToBuffer(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1, &copy);
			GpuResources::TransitionImage(cmd, image.image, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
		});
		void* mapped = nullptr;
		if (vkMapMemory(m_Device->device, staging.memory, 0, size, 0, &mapped) != VK_SUCCESS || !mapped)
		{
			GpuResources::DestroyBuffer(*m_Device, staging); return false;
		}
		Stats stats;
		const uint8_t* bytes = static_cast<const uint8_t*>(mapped);
		const uint64_t pixelCount = uint64_t(image.width) * image.height;
		const uint32_t componentsPerPixel = bpp == 8 ? 4u :
			(image.format == VK_FORMAT_R16G16_SFLOAT ? 2u :
			 (image.format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 ? 3u :
			  (image.format == VK_FORMAT_R32_SFLOAT ? 1u : 4u)));
		const uint64_t componentCount = pixelCount * componentsPerPixel;
		componentCountByGuide[e] = componentCount;
		for (uint64_t i = 0; i < componentCount; ++i)
		{
			float value = 0.0f;
			if (image.format == VK_FORMAT_B10G11R11_UFLOAT_PACK32)
			{
				uint32_t packed = 0;
				std::memcpy(&packed, bytes + (i / 3u) * 4u, sizeof(packed));
				const uint32_t channel = uint32_t(i % 3u);
				value = channel == 0u ? UFloatToFloat((packed >> 0u) & 0x7FFu, 6u) :
					(channel == 1u ? UFloatToFloat((packed >> 11u) & 0x7FFu, 6u) : UFloatToFloat((packed >> 22u) & 0x3FFu, 5u));
			}
			else if (image.format == VK_FORMAT_R16G16B16A16_SFLOAT || image.format == VK_FORMAT_R16G16_SFLOAT) value = HalfToFloat(ReadU16(bytes + i * 2));
			else if (image.format == VK_FORMAT_R8G8B8A8_UNORM) value = float(bytes[i]) / 255.0f;
			else std::memcpy(&value, bytes + i * 4, sizeof(value));
			Add(stats, value);
		}
		statsByGuide[e] = stats;
		vkUnmapMemory(m_Device->device, staging.memory);
		GpuResources::DestroyBuffer(*m_Device, staging);
		if (e) json << ',';
		json << "{\"name\":\"" << contract.name << "\",\"format\":\"";
		switch (image.format) { case VK_FORMAT_R16G16B16A16_SFLOAT: json << "RGBA16F"; break; case VK_FORMAT_R8G8B8A8_UNORM: json << "RGBA8_UNORM"; break; case VK_FORMAT_R16G16_SFLOAT: json << "RG16F"; break; case VK_FORMAT_B10G11R11_UFLOAT_PACK32: json << "R11G11B10F"; break; default: json << "R32F"; break; }
		json << "\",\"width\":" << image.width << ",\"height\":" << image.height
			<< ",\"allocation_bytes\":" << image.allocationSize << ",\"finite_count\":" << stats.finite
			<< ",\"nonfinite_count\":" << stats.nonfinite << ",\"zero_count\":" << stats.zero
			<< ",\"nonzero_count\":" << stats.nonzero << ",\"min\":" << (std::isfinite(stats.min) ? stats.min : 0.0f)
			<< ",\"max\":" << (std::isfinite(stats.max) ? stats.max : 0.0f) << '}';
	}
	const bool budgetValid = WithinBudget();
	const uint64_t pixelCount = uint64_t(m_Extent.Width()) * m_Extent.Height();
	const bool finiteValid = [&]() { for (size_t i = 0; i < 7; ++i)
		if (statsByGuide[i].nonfinite != 0 || statsByGuide[i].finite != componentCountByGuide[i]) return false; return true; }();
	const bool diffuseRange = statsByGuide[1].min >= 0.0f && statsByGuide[1].max <= 1.0f;
	const bool specRange = statsByGuide[2].min >= 0.0f && statsByGuide[2].max <= 1.0f;
	const bool normalRange = statsByGuide[3].min >= -1.001f && statsByGuide[3].max <= 1.001f;
	const bool depthRange = statsByGuide[4].min >= 0.0f;
	const bool motionFinite = statsByGuide[5].nonfinite == 0;
	const bool hitRange = statsByGuide[6].min >= 0.0f;
	const bool semanticValid = finiteValid && diffuseRange && specRange && normalRange &&
		depthRange && motionFinite && hitRange;
	const bool valid = budgetValid && semanticValid;
	uint64_t checksum = 1469598103934665603ull;
	{
		const VkDeviceSize size = VkDeviceSize(canonicalOutput.width) * canonicalOutput.height * 16u;
		GpuBuffer staging;
		if (!GpuResources::CreateBuffer(*m_Device, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging))
			return false;
		CommandUtils::ImmediateSubmit(*m_Device, [&](VkCommandBuffer cmd) {
			GpuResources::TransitionImage(cmd, canonicalOutput.image,
				VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
				VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT);
			VkBufferImageCopy copy{}; copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			copy.imageSubresource.layerCount = 1; copy.imageExtent = { canonicalOutput.width, canonicalOutput.height, 1 };
			vkCmdCopyImageToBuffer(cmd, canonicalOutput.image,
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1, &copy);
			GpuResources::TransitionImage(cmd, canonicalOutput.image,
				VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
		});
		void* mapped = nullptr;
		if (vkMapMemory(m_Device->device, staging.memory, 0, size, 0, &mapped) != VK_SUCCESS || !mapped)
		{
			GpuResources::DestroyBuffer(*m_Device, staging); return false;
		}
		const uint8_t* bytes = static_cast<const uint8_t*>(mapped);
		for (VkDeviceSize i = 0; i < size; ++i) { checksum ^= bytes[i]; checksum *= 1099511628211ull; }
		vkUnmapMemory(m_Device->device, staging.memory);
		GpuResources::DestroyBuffer(*m_Device, staging);
	}
	json << "],\"rt2_owned_allocation_bytes\":" << m_AllocationBytes << ",\"rt2_owned_allocation_count\":" << m_AllocationCount
		<< ",\"budget_valid\":" << (budgetValid ? "true" : "false")
		<< ",\"case\":\"raster-first-production\",\"valid\":" << (valid ? "true" : "false")
		<< ",\"failures\":[";
	bool firstFailure = true;
	auto failure = [&](const char* name, bool ok) { if (!ok) { if (!firstFailure) json << ','; firstFailure = false; json << '\"' << name << '\"'; } };
	failure("budget", budgetValid); failure("all_pixels_finite", finiteValid);
	failure("diffuse_range", diffuseRange); failure("specular_range", specRange);
	failure("normal_range", normalRange); failure("depth_nonnegative", depthRange);
	failure("motion_finite", motionFinite); failure("hit_distance_nonnegative", hitRange);
	json << "],\"semantic\":{\"pixel_count\":" << pixelCount
		<< ",\"all_pixels_overwritten\":" << (finiteValid ? "true" : "false")
		<< ",\"motion_nonzero_component_count\":" << statsByGuide[5].nonzero
		<< ",\"hit_distance_zero_default_count\":" << statsByGuide[6].zero
		<< ",\"motion_tolerance_px\":0.25,\"normal_tolerance\":0.001"
		<< "},\"format_feature_check\":{\"format\":\"R11G11B10F\",\"storage_image\":true,\"transfer_src\":true,\"transfer_dst\":true,\"pinned_sdk\":\"v310.7.0\"}"
		<< ",\"canonical_output_checksum_fnv1a64\":\"" << std::hex << checksum << std::dec
		<< "\",\"provenance\":{\"source\":\"actual GPU vkCmdCopyImageToBuffer after vkDeviceWaitIdle\",\"extent\":\"RenderExtent\",\"motion_space\":\"render_pixels\"}}";
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out.is_open()) { RT_LOG("[RRGuides] report open failed: %s", path.c_str()); return false; }
	out << json.str();
	if (!out.good()) { RT_LOG("[RRGuides] report write failed: %s", path.c_str()); return false; }
	out.flush();
	if (!out.good()) { RT_LOG("[RRGuides] report flush failed: %s", path.c_str()); return false; }
	if (std::getenv("RT2_RR_GUIDE_INJECT_CLOSE_FAILURE") != nullptr)
		out.setstate(std::ios::failbit);
	out.close();
	if (out.fail()) { RT_LOG("[RRGuides] report close failed: %s", path.c_str()); return false; }
	return valid;
}
