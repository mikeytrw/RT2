#include "RRGuideContract.h"

#include <array>
#include <algorithm>
#include <cmath>

namespace
{
constexpr std::array<RRGuideContract, static_cast<size_t>(RRGuideKind::Count)> kContracts = {{
	{ RRGuideKind::NoisyHdr, "RRGuideNoisyHdr", RRGuideFormat::R11G11B10F, RRGuideExtent::Render,
	  RRGuideSpace::LinearHdr, 12u, 0.0f, 0.0f, true, false },
	{ RRGuideKind::DiffuseAlbedo, "RRGuideDiffuseAlbedo", RRGuideFormat::RGBA8_UNORM, RRGuideExtent::Render,
	  RRGuideSpace::LinearRgb, 13u, 0.0f, 0.0f, true, false },
	{ RRGuideKind::SpecularAlbedo, "RRGuideSpecularAlbedo", RRGuideFormat::RGBA8_UNORM, RRGuideExtent::Render,
	  RRGuideSpace::LinearRgb, 14u, 0.5f, 0.5f, true, false },
	{ RRGuideKind::NormalRoughness, "RRGuideNormalRoughness", RRGuideFormat::RGBA16F, RRGuideExtent::Render,
	  RRGuideSpace::World, 15u, 1.0f, 1.0f, true, false },
	{ RRGuideKind::LinearDepth, "LinearDepth", RRGuideFormat::R32F, RRGuideExtent::Render,
	  RRGuideSpace::ViewDistance, 1u, 1.0e6f, 1.0e6f, false, true },
	{ RRGuideKind::Motion, "Motion", RRGuideFormat::RG16F, RRGuideExtent::Render,
	  RRGuideSpace::RenderPixels, 2u, 0.0f, 0.0f, false, true },
	{ RRGuideKind::SpecularHitDistance, "RRGuideSpecularHitDistance", RRGuideFormat::R32F, RRGuideExtent::Render,
	  RRGuideSpace::World, 16u, 0.0f, 0.0f, true, false },
}};

glm::vec3 EnvBRDFApprox2CPU(const glm::vec3& specularColor, float roughness, float noV)
{
	const glm::vec4 c0(-1.0f, -0.0275f, -0.572f, 0.022f);
	const glm::vec4 c1(1.0f, 0.0425f, 1.04f, -0.04f);
	const glm::vec4 r = roughness * c0 + c1;
	const float a004 = std::min(r.x * r.x, std::exp2(-9.28f * noV)) * r.x + r.y;
	const glm::vec2 ab = glm::vec2(-1.04f, 1.04f) * a004 + glm::vec2(r.z, r.w);
	return glm::max(specularColor * ab.x + glm::vec3(ab.y), glm::vec3(0.0f));
}
}

RRGuideMaterialValues EvaluateRRGuideMaterial(const glm::vec3& baseColor,
	float metallic, float roughness, float noV)
{
	const float m = std::clamp(metallic, 0.0f, 1.0f);
	const glm::vec3 clampedBase = glm::max(baseColor, glm::vec3(0.0f));
	RRGuideMaterialValues values;
	values.diffuseReflectance = clampedBase * (1.0f - m);
	values.f0 = glm::mix(glm::vec3(0.04f), clampedBase, m);
	values.specularAlbedo = EnvBRDFApprox2CPU(values.f0,
		std::clamp(roughness, 0.0f, 1.0f), std::clamp(std::abs(noV), 0.0f, 1.0f));
	return values;
}

bool ValidateRRGuideMaterialNumerics()
{
	const auto dielectric = EvaluateRRGuideMaterial(glm::vec3(0.8f), 0.0f, 0.5f, 1.0f);
	const auto metal = EvaluateRRGuideMaterial(glm::vec3(0.8f, 0.2f, 0.1f), 1.0f, 0.5f, 1.0f);
	const auto grazing = EvaluateRRGuideMaterial(glm::vec3(0.8f), 0.0f, 0.5f, 0.25f);
	const auto emissive = EvaluateRRGuideMaterial(glm::vec3(0.3f, 0.5f, 0.7f), 0.25f, 0.8f, 0.7f);
	return std::abs(dielectric.f0.x - 0.04f) < 1e-6f &&
		std::abs(metal.diffuseReflectance.x) < 1e-6f &&
		metal.f0.x > dielectric.f0.x &&
		glm::length(grazing.specularAlbedo - dielectric.specularAlbedo) > 1e-5f &&
		emissive.diffuseReflectance.x > 0.0f && emissive.f0.x > 0.0f;
}

RRGuideMotionValidation ValidateRRGuideMotion(float maxObservedMagnitudePixels,
	uint64_t nonzeroPixels, uint64_t pixelCount, bool movingCase)
{
	RRGuideMotionValidation result;
	// A moving render-pixel guide must carry a measurable displacement.  A
	// one-pixel floor rejects normalized-UV and dense 0.02px under-scaling
	// mutants while remaining far below the retained camera-sweep readings.
	result.expectedMinimumPixels = movingCase ? 1.0f : 0.0f;
	result.minimumNonzeroPixels = std::max<uint64_t>(1u, (pixelCount + 99u) / 100u);
	result.expectedObservedErrorPixels = movingCase
		? std::max(0.0f, result.expectedMinimumPixels - maxObservedMagnitudePixels)
		: maxObservedMagnitudePixels;
	result.densityValid = !movingCase || nonzeroPixels >= result.minimumNonzeroPixels;
	result.toleranceValid = result.expectedObservedErrorPixels <= 0.25f;
	result.valid = result.densityValid && result.toleranceValid;
	return result;
}

const RRGuideContract& GetRRGuideContract(RRGuideKind kind)
{
	return kContracts[static_cast<size_t>(kind)];
}

const std::array<RRGuideContract, static_cast<size_t>(RRGuideKind::Count)>& GetRRGuideContracts()
{
	return kContracts;
}

uint32_t RRGuideBytesPerPixel(RRGuideFormat format)
{
	switch (format)
	{
	case RRGuideFormat::RGBA16F: return 8;
	case RRGuideFormat::RGBA8_UNORM: return 4;
	case RRGuideFormat::R32F: return 4;
	case RRGuideFormat::RG16F: return 4;
	case RRGuideFormat::R11G11B10F: return 4;
	}
	return 0;
}

RRGuideContractCheck ValidateRRGuideContract()
{
	uint32_t dedicated = 0;
	uint32_t maxBinding = 0;
	uint32_t bytesPerPixel = 0;
	for (const auto& c : kContracts)
	{
		if (c.extent != RRGuideExtent::Render)
			return { false, "all initial guides must use RenderExtent" };
		if (c.binding == 19u)
			return { false, "guide binding collides with variable texture binding" };
		if (c.binding > maxBinding) maxBinding = c.binding;
		if (c.dedicated) { ++dedicated; bytesPerPixel += RRGuideBytesPerPixel(c.format); }
	}
	if (dedicated != RR_GUIDE_DEDICATED_COUNT)
		return { false, "dedicated guide count changed" };
	if (maxBinding >= 19u)
		return { false, "guide binding is above the variable texture binding" };
	if (bytesPerPixel != 24u)
		return { false, "dedicated guide payload format arithmetic changed" };
	if (kContracts[static_cast<size_t>(RRGuideKind::LinearDepth)].dedicated ||
		kContracts[static_cast<size_t>(RRGuideKind::Motion)].dedicated)
		return { false, "depth/motion must remain shared resources" };
	return { true, {} };
}
