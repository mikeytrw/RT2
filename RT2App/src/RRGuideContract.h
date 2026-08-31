#pragma once

// CPU-only RR guide contract. Keep this header free of Vulkan/NGX includes so
// RT2Tests and RT2SliceRunner can validate the semantic ledger in isolation.
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

enum class RRGuideKind : uint8_t
{
	NoisyHdr = 0,
	DiffuseAlbedo,
	SpecularAlbedo,
	NormalRoughness,
	LinearDepth,
	Motion,
	SpecularHitDistance,
	Count
};

enum class RRGuideFormat : uint8_t { RGBA16F, RGBA8_UNORM, R32F, RG16F, R11G11B10F };
enum class RRGuideExtent : uint8_t { Render, Output };
enum class RRGuideSpace : uint8_t { LinearHdr, LinearRgb, World, ViewDistance, RenderPixels };

struct RRGuideContract
{
	RRGuideKind kind;
	std::string_view name;
	RRGuideFormat format;
	RRGuideExtent extent;
	RRGuideSpace space;
	uint32_t binding;
	float clearValue;
	float missValue;
	bool dedicated;
	bool sharedWithNrd;
};

constexpr uint32_t RR_GUIDE_DEDICATED_COUNT = 5;
constexpr uint32_t RR_GUIDE_MAX_IMAGES = 6;
constexpr uint32_t RR_GUIDE_MAX_ALLOCATIONS = 8;
constexpr uint64_t RR_GUIDE_MAX_RT2_BYTES = 224ull * 1024ull * 1024ull;

const RRGuideContract& GetRRGuideContract(RRGuideKind kind);
const std::array<RRGuideContract, static_cast<size_t>(RRGuideKind::Count)>& GetRRGuideContracts();
uint32_t RRGuideBytesPerPixel(RRGuideFormat format);

struct RRGuideContractCheck
{
	bool valid = false;
	std::string_view error;
};

RRGuideContractCheck ValidateRRGuideContract();
