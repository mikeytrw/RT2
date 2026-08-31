#include "RRGuideContract.h"

#include <array>

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
