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
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>

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
		VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
		VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
	for (size_t formatIndex = 0; formatIndex < RR_GUIDE_DEDICATED_COUNT; ++formatIndex)
	{
		VkFormat format = kFormats[formatIndex];
		VkFormatProperties formatProperties{};
		vkGetPhysicalDeviceFormatProperties(device.physicalDevice, format, &formatProperties);
		m_FormatFeatures[formatIndex] = formatProperties.optimalTilingFeatures;
		if ((formatProperties.optimalTilingFeatures & required) != required)
		{
			RT_LOG("[RRGuides] selected device lacks required storage/sampled/transfer format support: %d", (int)format);
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
	for (auto& features : m_FormatFeatures) features = 0;
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

std::string JsonString(const std::string& value)
{
	std::ostringstream escaped;
	for (const char c : value)
	{
		switch (c)
		{
		case '\\': escaped << "\\\\"; break;
		case '"': escaped << "\\\""; break;
		case '\n': escaped << "\\n"; break;
		case '\r': escaped << "\\r"; break;
		case '\t': escaped << "\\t"; break;
		default: escaped << c; break;
		}
	}
	return escaped.str();
}
}

bool RRGuideResources::WriteReport(const std::string& path, const GpuImage& sharedDepth,
	const GpuImage& sharedMotion, const GpuImage& sharedEmission,
	const GpuImage& sharedAlbedoF0, const GpuImage& sharedPrimHit,
	const GpuImage& canonicalOutput,
	const RRGuideReportMetadata& metadata) const
{
	if (!m_Device || !IsValid() || !WithinBudget() || !sharedDepth.IsValid() ||
		!sharedMotion.IsValid() || !sharedEmission.IsValid() || !sharedAlbedoF0.IsValid() ||
		!sharedPrimHit.IsValid() || !canonicalOutput.IsValid())
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
	uint64_t sentinelRemainingByGuide[7]{};
	std::vector<float> decodedByGuide[7];
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
		decodedByGuide[e].resize(componentCount);
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
			decodedByGuide[e][i] = value;
			const bool sentinel = (e == 0 && value >= 60000.0f) ||
				((e == 1 || e == 2) && (i % 4u) == 3u && value == 0.0f) ||
				(e == 3 && !std::isfinite(value)) ||
				(e == 4 && !std::isfinite(value)) ||
				(e == 5 && !std::isfinite(value)) || (e == 6 && value < 0.0f);
			if (sentinel) ++sentinelRemainingByGuide[e];
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
	const uint64_t pixelCount = uint64_t(m_Extent.Width()) * m_Extent.Height();
	uint64_t depthMissCount = 0;
	uint64_t missWithNonzeroHitCount = 0;
	uint64_t emissivePixelCount = 0;
	uint64_t directEmissionProvenanceCount = 0;
	uint64_t skyMotionPixelCount = 0;
	uint64_t geometryMotionPixelCount = 0;
	uint64_t emissiveMotionPixelCount = 0;
	// Direct emission is a shared producer-side class mask, read from its
	// actual GPU image rather than inferred from noisy radiance. It is kept out
	// of the seven-guide schema because it remains an existing G-buffer row.
	std::vector<float> directEmission;
	{
		const VkDeviceSize size = VkDeviceSize(sharedEmission.width) * sharedEmission.height * 8u;
		GpuBuffer staging;
		if (!GpuResources::CreateBuffer(*m_Device, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging)) return false;
		CommandUtils::ImmediateSubmit(*m_Device, [&](VkCommandBuffer cmd) {
			GpuResources::TransitionImage(cmd, sharedEmission.image,
				VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
				VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT);
			VkBufferImageCopy copy{}; copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			copy.imageSubresource.layerCount = 1; copy.imageExtent = { sharedEmission.width, sharedEmission.height, 1 };
			vkCmdCopyImageToBuffer(cmd, sharedEmission.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				staging.buffer, 1, &copy);
			GpuResources::TransitionImage(cmd, sharedEmission.image,
				VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
		});
		void* mapped = nullptr;
		if (vkMapMemory(m_Device->device, staging.memory, 0, size, 0, &mapped) != VK_SUCCESS || !mapped)
		{ GpuResources::DestroyBuffer(*m_Device, staging); return false; }
		const uint8_t* bytes = static_cast<const uint8_t*>(mapped);
		directEmission.resize(pixelCount * 4u);
		for (uint64_t i = 0; i < directEmission.size(); ++i)
			directEmission[i] = HalfToFloat(ReadU16(bytes + i * 2u));
		for (uint64_t p = 0; p < pixelCount; ++p)
			if (directEmission[p * 4u + 3u] > 0.5f) ++directEmissionProvenanceCount;
		vkUnmapMemory(m_Device->device, staging.memory);
		GpuResources::DestroyBuffer(*m_Device, staging);
	}
	// Cross-resource material validation reads the producer inputs from the
	// actual GPU G-buffer and compares decoded guide pixels against the same
	// production C++ authority used by CPU contract tests.
	auto readbackBytes = [&](const GpuImage& image, VkDeviceSize bytes, std::vector<uint8_t>& out) -> bool {
		GpuBuffer staging;
		if (!GpuResources::CreateBuffer(*m_Device, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging)) return false;
		CommandUtils::ImmediateSubmit(*m_Device, [&](VkCommandBuffer cmd) {
			GpuResources::TransitionImage(cmd, image.image, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
				VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
			VkBufferImageCopy copy{}; copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			copy.imageSubresource.layerCount = 1; copy.imageExtent = { image.width, image.height, 1 };
			vkCmdCopyImageToBuffer(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1, &copy);
			GpuResources::TransitionImage(cmd, image.image, VK_ACCESS_TRANSFER_READ_BIT,
				VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
		});
		void* mapped = nullptr;
		if (vkMapMemory(m_Device->device, staging.memory, 0, bytes, 0, &mapped) != VK_SUCCESS || !mapped)
		{ GpuResources::DestroyBuffer(*m_Device, staging); return false; }
		out.resize(static_cast<size_t>(bytes));
		std::memcpy(out.data(), mapped, static_cast<size_t>(bytes));
		vkUnmapMemory(m_Device->device, staging.memory);
		GpuResources::DestroyBuffer(*m_Device, staging);
		return true;
	};
	std::vector<uint8_t> albedoBytes, primHitBytes;
	if (!readbackBytes(sharedAlbedoF0, VkDeviceSize(pixelCount) * 8u, albedoBytes) ||
		!readbackBytes(sharedPrimHit, VkDeviceSize(pixelCount) * 16u, primHitBytes))
		return false;
	uint64_t materialGpuSamples = 0;
	float maxMaterialDiffuseError = 0.0f;
	float maxMaterialSpecularError = 0.0f;
	uint64_t dielectricSamples = 0, metallicSamples = 0, grazingSamples = 0, emissiveSamples = 0;
	for (uint64_t p = 0; p < pixelCount; ++p)
	{
		float hitW = 0.0f;
		std::memcpy(&hitW, primHitBytes.data() + p * 16u + 12u, sizeof(float));
		if (!std::isfinite(hitW) || hitW < 0.5f) continue;
		float albedo[4]{};
		for (uint32_t c = 0; c < 4; ++c) albedo[c] = HalfToFloat(ReadU16(albedoBytes.data() + p * 8u + c * 2u));
		float world[3]{};
		for (uint32_t c = 0; c < 3; ++c) std::memcpy(&world[c], primHitBytes.data() + p * 16u + c * 4u, sizeof(float));
		const glm::vec3 normal(decodedByGuide[3][p * 4u], decodedByGuide[3][p * 4u + 1u], decodedByGuide[3][p * 4u + 2u]);
		const float roughness = std::clamp(decodedByGuide[3][p * 4u + 3u], 0.0f, 1.0f);
		const glm::vec3 worldPosition(world[0], world[1], world[2]);
		const float noV = std::abs(glm::dot(glm::normalize(normal), glm::normalize(metadata.cameraPosition - worldPosition)));
		const RRGuideMaterialValues expected = EvaluateRRGuideMaterial(glm::vec3(albedo[0], albedo[1], albedo[2]), albedo[3], roughness, noV);
		const glm::vec3 actualDiffuse(decodedByGuide[1][p * 4u], decodedByGuide[1][p * 4u + 1u], decodedByGuide[1][p * 4u + 2u]);
		const glm::vec3 actualSpec(decodedByGuide[2][p * 4u], decodedByGuide[2][p * 4u + 1u], decodedByGuide[2][p * 4u + 2u]);
		const glm::vec3 diffuseError = glm::abs(actualDiffuse - expected.diffuseReflectance);
		const glm::vec3 specularError = glm::abs(actualSpec - expected.specularAlbedo);
		maxMaterialDiffuseError = std::max(maxMaterialDiffuseError, std::max(diffuseError.x, std::max(diffuseError.y, diffuseError.z)));
		maxMaterialSpecularError = std::max(maxMaterialSpecularError, std::max(specularError.x, std::max(specularError.y, specularError.z)));
		++materialGpuSamples;
		if (albedo[3] < 0.1f) ++dielectricSamples;
		if (albedo[3] > 0.9f) ++metallicSamples;
		if (noV < 0.5f) ++grazingSamples;
		if (directEmission[p * 4u + 3u] > 0.5f) ++emissiveSamples;
	}
	const bool controlledMaterialCase = metadata.scenario == RRGuideScenario::ControlledMaterialMotion;
	const bool materialGpuValid = materialGpuSamples > 0 && maxMaterialDiffuseError <= 0.08f &&
		maxMaterialSpecularError <= 0.08f && (!controlledMaterialCase ||
		(dielectricSamples > 0 && metallicSamples > 0 && grazingSamples > 0 && emissiveSamples > 0));
	const bool budgetValid = WithinBudget();
	const bool finiteValid = [&]() { for (size_t i = 0; i < 7; ++i)
		if (statsByGuide[i].nonfinite != 0 || statsByGuide[i].finite != componentCountByGuide[i]) return false; return true; }();
	const bool diffuseRange = statsByGuide[1].min >= 0.0f && statsByGuide[1].max <= 1.0f;
	const bool specRange = statsByGuide[2].min >= 0.0f && statsByGuide[2].max <= 1.0f;
	const bool normalRange = statsByGuide[3].min >= -1.001f && statsByGuide[3].max <= 1.001f;
	const bool depthRange = statsByGuide[4].min >= 0.0f;
	const bool motionFinite = statsByGuide[5].nonfinite == 0;
	const bool hitRange = statsByGuide[6].min >= 0.0f;
	uint64_t sentinelRemaining = 0;
	for (uint64_t count : sentinelRemainingByGuide) sentinelRemaining += count;
	const bool allPixelsOverwritten = sentinelRemaining == 0;
	float maxNormalLengthError = 0.0f;
	for (uint64_t p = 0; p < pixelCount; ++p)
	{
		const glm::vec3 normal(decodedByGuide[3][p * 4u + 0u], decodedByGuide[3][p * 4u + 1u], decodedByGuide[3][p * 4u + 2u]);
		if (std::isfinite(normal.x) && std::isfinite(normal.y) && std::isfinite(normal.z))
			maxNormalLengthError = std::max(maxNormalLengthError, std::abs(glm::length(normal) - 1.0f));
	}
	for (uint64_t p = 0; p < pixelCount; ++p)
	{
		const bool depthMiss = decodedByGuide[4][p] >= 999999.0f;
		if (depthMiss)
		{
			++depthMissCount;
			if (decodedByGuide[6][p] > 0.000001f) ++missWithNonzeroHitCount;
		}
		const bool terminal = decodedByGuide[6][p] <= 0.000001f;
		const bool emissive = !depthMiss && directEmission[p * 4u + 3u] > 0.5f;
		if (emissive) ++emissivePixelCount;
	}
	uint64_t motionNonzeroPixelCount = 0;
	float maxMotionMagnitude = 0.0f;
	float motionSumX = 0.0f;
	for (uint64_t p = 0; p < pixelCount; ++p)
	{
		const float mx = decodedByGuide[5][p * 2u + 0u];
		const float my = decodedByGuide[5][p * 2u + 1u];
		const float magnitude = std::sqrt(mx * mx + my * my);
		maxMotionMagnitude = std::max(maxMotionMagnitude, magnitude);
		if (magnitude > 0.25f) ++motionNonzeroPixelCount;
		if (magnitude > 0.25f) motionSumX += mx;
		const bool depthMiss = decodedByGuide[4][p] >= 999999.0f;
		const bool emissive = !depthMiss && decodedByGuide[6][p] <= 0.000001f &&
			(decodedByGuide[0][p * 3u] + decodedByGuide[0][p * 3u + 1u] + decodedByGuide[0][p * 3u + 2u]) > 0.001f;
		if (magnitude > 0.25f)
		{
			if (depthMiss) ++skyMotionPixelCount;
			else ++geometryMotionPixelCount;
			if (emissive) ++emissiveMotionPixelCount;
		}
	}
	// Reclassify using the shared direct-emission GPU image, so emissive
	// coverage cannot be inferred from a guide value or an NRD sentinel.
	emissivePixelCount = 0;
	skyMotionPixelCount = geometryMotionPixelCount = emissiveMotionPixelCount = 0;
	for (uint64_t p = 0; p < pixelCount; ++p)
	{
		const bool sky = decodedByGuide[4][p] >= 999999.0f;
		const bool emissive = !sky && directEmission[p * 4u + 3u] > 0.5f;
		if (emissive) ++emissivePixelCount;
		const float mx = decodedByGuide[5][p * 2u];
		const float my = decodedByGuide[5][p * 2u + 1u];
		if (std::sqrt(mx * mx + my * my) > 0.25f)
		{
			if (sky) ++skyMotionPixelCount; else ++geometryMotionPixelCount;
			if (emissive) ++emissiveMotionPixelCount;
		}
	}
	const bool movingCase = std::abs(metadata.cameraSweepAmplitude) > 0.000001f ||
		std::getenv("RT2_RR_GUIDE_INJECT_DENSE_WEAK_MOTION") != nullptr ||
		std::getenv("RT2_RR_GUIDE_INJECT_BAD_MOTION_SCALE") != nullptr;
	// Independently derive current->previous render-pixel vectors from the
	// captured camera transforms and the actual GPU world-hit/depth mask. Sky
	// misses use a finite point along the reconstructed current camera ray.
	glm::vec2 expectedMotionMean(0.0f), observedMotionMean(0.0f);
	uint64_t expectedMotionSamples = 0;
	float expectedMotionMaxError = 0.0f;
	float expectedSkyMaxError = 0.0f, expectedGeometryMaxError = 0.0f;
	const glm::mat4 currentViewToClip = metadata.expectedCurrentViewToClip;
	const glm::mat4 currentWorldToView = metadata.expectedCurrentWorldToView;
	const glm::mat4 previousViewToClip = metadata.expectedPreviousViewToClip;
	const glm::mat4 previousWorldToView = metadata.expectedPreviousWorldToView;
	const glm::mat4 inverseCurrentProjection = glm::inverse(currentViewToClip);
	const glm::mat4 inverseCurrentView = glm::inverse(currentWorldToView);
	const glm::vec3 currentCameraPosition = glm::vec3(inverseCurrentView[3]);
	const glm::vec2 renderExtent(float(m_Extent.Width()), float(m_Extent.Height()));
	auto projectPixels = [&](const glm::mat4& projection, const glm::mat4& view,
		const glm::vec3& world, glm::vec2& result) -> bool {
		const glm::vec4 clip = projection * view * glm::vec4(world, 1.0f);
		if (!std::isfinite(clip.w) || std::abs(clip.w) < 1e-6f) return false;
		result = (glm::vec2(clip) / clip.w * 0.5f + 0.5f) * renderExtent;
		return std::isfinite(result.x) && std::isfinite(result.y);
	};
	for (uint64_t p = 0; p < pixelCount && metadata.expectedMotionTransformsValid; ++p)
	{
		const uint32_t x = uint32_t(p % m_Extent.Width());
		const uint32_t y = uint32_t(p / m_Extent.Width());
		const glm::vec2 currentPixel(float(x) + 0.5f, float(y) + 0.5f);
		const bool sky = decodedByGuide[4][p] >= 999999.0f;
		glm::vec2 expectedCurrent = currentPixel, expectedPrevious{};
		bool projected = false;
		if (!sky)
		{
			float hitW = 0.0f;
			std::memcpy(&hitW, primHitBytes.data() + p * 16u + 12u, sizeof(float));
			if (std::isfinite(hitW) && hitW >= 0.5f)
			{
				glm::vec3 world{};
				for (uint32_t c = 0; c < 3; ++c)
					std::memcpy(&world[c], primHitBytes.data() + p * 16u + c * 4u, sizeof(float));
				projected = projectPixels(currentViewToClip, currentWorldToView, world, expectedCurrent) &&
					projectPixels(previousViewToClip, previousWorldToView, world, expectedPrevious);
			}
		}
		else
		{
			const glm::vec2 uv = currentPixel / renderExtent;
			const glm::vec4 viewTarget = inverseCurrentProjection *
				glm::vec4(uv * 2.0f - 1.0f, 1.0f, 1.0f);
			if (std::isfinite(viewTarget.w) && std::abs(viewTarget.w) > 1e-6f)
			{
				const glm::vec3 viewDirection = glm::normalize(glm::vec3(viewTarget) / viewTarget.w);
				const glm::vec3 skyDirection = glm::normalize(glm::vec3(inverseCurrentView * glm::vec4(viewDirection, 0.0f)));
				projected = projectPixels(previousViewToClip, previousWorldToView,
					currentCameraPosition + skyDirection * 1000.0f, expectedPrevious);
			}
		}
		if (!projected) continue;
		const glm::vec2 expected = expectedPrevious - expectedCurrent;
		const glm::vec2 observed(decodedByGuide[5][p * 2u], decodedByGuide[5][p * 2u + 1u]);
		const float error = glm::length(observed - expected);
		expectedMotionMean += expected;
		observedMotionMean += observed;
		++expectedMotionSamples;
		expectedMotionMaxError = std::max(expectedMotionMaxError, error);
		if (sky) expectedSkyMaxError = std::max(expectedSkyMaxError, error);
		else expectedGeometryMaxError = std::max(expectedGeometryMaxError, error);
	}
	if (expectedMotionSamples > 0)
	{
		expectedMotionMean /= float(expectedMotionSamples);
		observedMotionMean /= float(expectedMotionSamples);
	}
	const float expectedMotionMeanError = glm::length(observedMotionMean - expectedMotionMean);
	const bool motionExpectedVectorValid = !movingCase && !metadata.expectedMotionTransformsValid
		? true : (metadata.expectedMotionTransformsValid && expectedMotionSamples > 0 &&
			expectedMotionMaxError <= 0.25f && expectedMotionMeanError <= 0.25f &&
			expectedSkyMaxError <= 0.25f && expectedGeometryMaxError <= 0.25f);
	// Motion faults are producer-side GPU clears in FrameRenderer; no semantic
	// counter is rewritten here.
	// A strict bound plus a density floor is intentional: an all-zero moving
	// producer must fail even when its lower-bound error equals the tolerance.
	const RRGuideMotionValidation motionValidation = ValidateRRGuideMotion(
		maxMotionMagnitude, motionNonzeroPixelCount, pixelCount, movingCase);
	const uint64_t minimumMovingPixels = motionValidation.minimumNonzeroPixels;
	const float expectedMotionMinimum = motionValidation.expectedMinimumPixels;
	const float motionExpectedObservedError = motionValidation.expectedObservedErrorPixels;
	const bool motionDensityValid = motionValidation.densityValid;
	const bool motionToleranceValid = motionValidation.toleranceValid;
	const float expectedMotionSignX = metadata.cameraMode == "yaw" ? 1.0f : 0.0f;
	const float observedMotionMeanX = motionNonzeroPixelCount > 0 ? motionSumX / float(motionNonzeroPixelCount) : 0.0f;
	const bool motionSignValid = !movingCase || expectedMotionSignX == 0.0f ||
		observedMotionMeanX * expectedMotionSignX > 0.0f;
	const bool normalToleranceValid = maxNormalLengthError <= 0.001f;
	const bool hitDepthCorrelationValid = missWithNonzeroHitCount == 0;
	const bool materialNumericsValid = ValidateRRGuideMaterialNumerics() && materialGpuValid;
	const bool emissiveRequired = controlledMaterialCase ||
		std::getenv("RT2_RR_GUIDE_REQUIRE_EMISSIVE") != nullptr;
	const bool classCoverageValid = !movingCase ||
		(depthMissCount > 0 && (pixelCount - depthMissCount) > 0 &&
		 (!emissiveRequired || (emissivePixelCount > 0 && emissiveMotionPixelCount > 0)) &&
		 skyMotionPixelCount > 0 && geometryMotionPixelCount > 0);
	bool semanticValid = finiteValid && diffuseRange && specRange && normalRange &&
		depthRange && motionFinite && hitRange && allPixelsOverwritten &&
		normalToleranceValid && hitDepthCorrelationValid && motionToleranceValid &&
		motionDensityValid && materialNumericsValid && classCoverageValid;
	semanticValid = semanticValid && motionExpectedVectorValid;
	const bool fixtureValid = metadata.fixtureHashValid && metadata.fixtureBytes > 0;
	const bool scenarioValid = metadata.scenarioDeclarationValid;
	const bool identityValid = !controlledMaterialCase || metadata.fixtureIdentityValid;
	bool valid = budgetValid && semanticValid && motionSignValid && fixtureValid && scenarioValid && identityValid;
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
	const bool canonicalReadbackStable = !metadata.canonicalBaselineValid || metadata.canonicalBaselineChecksum == checksum;
	const bool canonicalPairMatch = metadata.pairedBaselineValid &&
		metadata.pairedBaselineExitCode == 0 && metadata.pairedBaselineChecksum == checksum;
	const bool runtimeExitValid = metadata.expectedExitCode == 0;
	valid = valid && canonicalReadbackStable && canonicalPairMatch && runtimeExitValid;
	json << "],\"rt2_owned_allocation_bytes\":" << m_AllocationBytes << ",\"rt2_owned_allocation_count\":" << m_AllocationCount
		<< ",\"budget_valid\":" << (budgetValid ? "true" : "false")
		<< ",\"case\":\"" << JsonString(metadata.caseName) << "\",\"valid\":" << (valid ? "true" : "false")
		<< ",\"failures\":[";
	bool firstFailure = true;
	auto failure = [&](const char* name, bool ok) { if (!ok) { if (!firstFailure) json << ','; firstFailure = false; json << '\"' << name << '\"'; } };
	failure("budget", budgetValid); failure("all_pixels_finite", finiteValid);
	failure("all_pixels_overwritten", allPixelsOverwritten);
	failure("diffuse_range", diffuseRange); failure("specular_range", specRange);
	failure("normal_range", normalRange); failure("depth_nonnegative", depthRange);
	failure("motion_finite", motionFinite); failure("hit_distance_nonnegative", hitRange);
	failure("normal_tolerance", normalToleranceValid); failure("hit_depth_correlation", hitDepthCorrelationValid);
	failure("motion_expected_observed_tolerance", motionToleranceValid);
	failure("motion_density", motionDensityValid);
	failure("motion_expected_sign", motionSignValid);
	failure("motion_expected_vector", motionExpectedVectorValid);
	failure("fixture_hash", fixtureValid);
	failure("scenario_declaration", scenarioValid);
	failure("fixture_identity", identityValid);
	failure("material_contract", materialNumericsValid);
	failure("motion_class_coverage", classCoverageValid);
	failure("canonical_readback_changed", canonicalReadbackStable);
	failure("canonical_pair_missing_or_changed", canonicalPairMatch);
	failure("runtime_exit_code", runtimeExitValid);
	json << "],\"semantic\":{\"pixel_count\":" << pixelCount
		<< ",\"all_pixels_overwritten\":" << (allPixelsOverwritten ? "true" : "false")
		<< ",\"sentinel_remaining_count\":" << sentinelRemaining
		<< ",\"sentinel_remaining_by_guide\":[" << sentinelRemainingByGuide[0]
		<< "," << sentinelRemainingByGuide[1] << "," << sentinelRemainingByGuide[2]
		<< "," << sentinelRemainingByGuide[3] << "," << sentinelRemainingByGuide[4]
		<< "," << sentinelRemainingByGuide[5] << "," << sentinelRemainingByGuide[6] << "]"
		<< ",\"motion_nonzero_component_count\":" << statsByGuide[5].nonzero
		<< ",\"motion_nonzero_pixel_count\":" << motionNonzeroPixelCount
		<< ",\"motion_max_magnitude_px\":" << maxMotionMagnitude
		<< ",\"motion_expected_minimum_px\":" << expectedMotionMinimum
		<< ",\"motion_expected_observed_error_px\":" << motionExpectedObservedError
		<< ",\"motion_expected_sign_x\":" << expectedMotionSignX
		<< ",\"motion_observed_mean_x_px\":" << observedMotionMeanX
		<< ",\"motion_expected_mean_x_px\":" << expectedMotionMean.x
		<< ",\"motion_expected_mean_y_px\":" << expectedMotionMean.y
		<< ",\"motion_observed_mean_y_px\":" << observedMotionMean.y
		<< ",\"motion_expected_mean_error_px\":" << expectedMotionMeanError
		<< ",\"motion_expected_max_error_px\":" << expectedMotionMaxError
		<< ",\"motion_sky_max_error_px\":" << expectedSkyMaxError
		<< ",\"motion_geometry_max_error_px\":" << expectedGeometryMaxError
		<< ",\"motion_expected_vector_valid\":" << (motionExpectedVectorValid ? "true" : "false")
		<< ",\"motion_sign_valid\":" << (motionSignValid ? "true" : "false")
		<< ",\"motion_minimum_nonzero_pixel_count\":" << minimumMovingPixels
		<< ",\"motion_density_valid\":" << (motionDensityValid ? "true" : "false")
		<< ",\"sky_pixel_count\":" << depthMissCount
		<< ",\"geometry_pixel_count\":" << (pixelCount - depthMissCount)
		<< ",\"emissive_pixel_count\":" << emissivePixelCount
		<< ",\"emissive_provenance_pixel_count\":" << directEmissionProvenanceCount
		<< ",\"sky_motion_pixel_count\":" << skyMotionPixelCount
		<< ",\"geometry_motion_pixel_count\":" << geometryMotionPixelCount
		<< ",\"emissive_motion_pixel_count\":" << emissiveMotionPixelCount
		<< ",\"emissive_required\":" << (emissiveRequired ? "true" : "false")
		<< ",\"motion_class_coverage_valid\":" << (classCoverageValid ? "true" : "false")
		<< ",\"hit_distance_zero_default_count\":" << statsByGuide[6].zero
		<< ",\"depth_miss_count\":" << depthMissCount
		<< ",\"miss_with_nonzero_hit_count\":" << missWithNonzeroHitCount
		<< ",\"motion_tolerance_px\":0.25,\"normal_tolerance\":0.001"
		<< ",\"normal_max_length_error\":" << maxNormalLengthError
		<< ",\"material_numeric_cases\":4,\"material_gpu_samples\":" << materialGpuSamples
		<< ",\"material_dielectric_samples\":" << dielectricSamples
		<< ",\"material_metallic_samples\":" << metallicSamples
		<< ",\"material_grazing_samples\":" << grazingSamples
		<< ",\"material_emissive_samples\":" << emissiveSamples
		<< ",\"material_required_classes\":" << (controlledMaterialCase ? "true" : "false")
		<< ",\"material_max_diffuse_error\":" << maxMaterialDiffuseError
		<< ",\"material_max_specular_error\":" << maxMaterialSpecularError
		<< ",\"material_gpu_numeric_valid\":" << (materialGpuValid ? "true" : "false")
		<< ",\"material_numeric_valid\":" << (materialNumericsValid ? "true" : "false")
		<< "},\"format_feature_check\":{\"format\":\"R11G11B10F\",\"storage_image\":"
		<< ((m_FormatFeatures[0] & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0 ? "true" : "false")
		<< ",\"sampled_image\":" << ((m_FormatFeatures[0] & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0 ? "true" : "false")
		<< ",\"transfer_src\":" << ((m_FormatFeatures[0] & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) != 0 ? "true" : "false")
		<< ",\"transfer_dst\":" << ((m_FormatFeatures[0] & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) != 0 ? "true" : "false")
		<< ",\"pinned_sdk\":\"v310.7.0\"}"
		<< ",\"canonical_readback_checksum_fnv1a64\":\"" << std::hex << checksum << std::dec
		<< "\",\"canonical_readback_baseline_checksum_fnv1a64\":\"" << std::hex << metadata.canonicalBaselineChecksum << std::dec
		<< "\",\"canonical_readback_stable\":" << (canonicalReadbackStable ? "true" : "false")
		<< ",\"canonical_pair_checksum_fnv1a64\":\"" << std::hex << metadata.pairedBaselineChecksum << std::dec
		<< "\",\"canonical_pair_match\":" << (canonicalPairMatch ? "true" : "false")
		<< ",\"canonical_pair_manifest\":\"" << JsonString(metadata.pairedBaselinePath)
		<< "\",\"canonical_pair_command\":\"" << JsonString(metadata.pairedBaselineCommand)
		<< "\",\"fixture\":{\"path\":\"" << JsonString(metadata.fixturePath)
		<< "\",\"fnv1a64\":\"" << std::hex << metadata.fixtureFNV1a64 << std::dec
		<< "\",\"bytes\":" << metadata.fixtureBytes
		<< ",\"identity_valid\":" << (metadata.fixtureIdentityValid ? "true" : "false")
		<< "},\"runtime\":{\"scenario\":\"" << RRGuideScenarioName(metadata.scenario)
		<< "\",\"scenario_declaration_valid\":" << (metadata.scenarioDeclarationValid ? "true" : "false")
		<< ",\"nrd_enabled\":" << (metadata.nrdEnabled ? "true" : "false")
		<< ",\"frames\":" << metadata.frameCount << ",\"camera_mode\":\"" << JsonString(metadata.cameraMode)
		<< "\",\"camera_sweep_amplitude\":" << metadata.cameraSweepAmplitude
		<< ",\"camera_sweep_warmup\":" << metadata.cameraSweepWarmup
		<< ",\"camera_sweep_period\":" << metadata.cameraSweepPeriod
		<< "},\"command\":\"" << JsonString(metadata.commandLine) << "\",\"exit_code\":" << metadata.expectedExitCode
		<< ",\"provenance\":{\"source\":\"actual GPU vkCmdCopyImageToBuffer after vkDeviceWaitIdle\",\"extent\":\"RenderExtent\",\"motion_space\":\"render_pixels\",\"scene\":\"" << JsonString(metadata.scenePath) << "\"}}";
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
