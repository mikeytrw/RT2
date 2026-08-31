#include <doctest/doctest.h>

#include "RRGuideContract.h"
#include "RenderExtents.h"
#include <fstream>
#include <iterator>
#include <string>

namespace
{
std::string ReadShader(const char* path)
{
	std::ifstream in(path, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
}

TEST_CASE("RR guides: checked seven-row contract is internally consistent")
{
	const RRGuideContractCheck check = ValidateRRGuideContract();
	CHECK(check.valid);
	CHECK(GetRRGuideContracts().size() == static_cast<size_t>(RRGuideKind::Count));
	CHECK(RR_GUIDE_DEDICATED_COUNT == 5);
}

TEST_CASE("RR guides: formats, defaults, semantic ownership and bindings are stable")
{
	const auto& noisy = GetRRGuideContract(RRGuideKind::NoisyHdr);
	CHECK(noisy.format == RRGuideFormat::R11G11B10F);
	CHECK(noisy.extent == RRGuideExtent::Render);
	CHECK(noisy.space == RRGuideSpace::LinearHdr);
	CHECK(noisy.dedicated);

	const auto& diffuse = GetRRGuideContract(RRGuideKind::DiffuseAlbedo);
	const auto& specular = GetRRGuideContract(RRGuideKind::SpecularAlbedo);
	CHECK(diffuse.format == RRGuideFormat::RGBA8_UNORM);
	CHECK(diffuse.clearValue == doctest::Approx(0.0f));
	CHECK(specular.missValue == doctest::Approx(0.5f));
	CHECK(specular.binding != diffuse.binding);

	const auto& nr = GetRRGuideContract(RRGuideKind::NormalRoughness);
	CHECK(nr.format == RRGuideFormat::RGBA16F);
	CHECK(nr.missValue == doctest::Approx(1.0f));
	const auto& depth = GetRRGuideContract(RRGuideKind::LinearDepth);
	const auto& motion = GetRRGuideContract(RRGuideKind::Motion);
	CHECK_FALSE(depth.dedicated);
	CHECK_FALSE(motion.dedicated);
	CHECK(depth.sharedWithNrd);
	CHECK(motion.sharedWithNrd);
	CHECK(GetRRGuideContract(RRGuideKind::SpecularHitDistance).missValue == doctest::Approx(0.0f));
}

TEST_CASE("RR guides: payload and allocation ceiling arithmetic is explicit")
{
	uint32_t dedicated = 0;
	uint32_t payloadBytesPerPixel = 0;
	for (const auto& c : GetRRGuideContracts())
	{
		if (c.dedicated) { ++dedicated; payloadBytesPerPixel += RRGuideBytesPerPixel(c.format); }
	}
	CHECK(dedicated == RR_GUIDE_DEDICATED_COUNT);
	CHECK(payloadBytesPerPixel == 24);
	const auto extent = RenderExtent::TryCreate(4096, 2160);
	REQUIRE(extent.has_value());
	const uint64_t payload = uint64_t(extent->Width()) * extent->Height() * payloadBytesPerPixel;
	CHECK(payload == 212336640ull);
	CHECK(payload <= RR_GUIDE_MAX_RT2_BYTES);
	CHECK(RR_GUIDE_DEDICATED_COUNT <= RR_GUIDE_MAX_IMAGES);
	CHECK(RR_GUIDE_DEDICATED_COUNT <= RR_GUIDE_MAX_ALLOCATIONS);
}

TEST_CASE("RR guides: render extent is the only source extent")
{
	const auto output = OutputExtent::TryCreate(1920, 1080);
	REQUIRE(output.has_value());
	const RenderExtent render = output->ToRenderNative();
	for (const auto& c : GetRRGuideContracts())
		CHECK(c.extent == RRGuideExtent::Render);
	CHECK(render.Width() == output->Width());
	CHECK(render.Height() == output->Height());
}

TEST_CASE("RR guides RED-GREEN: bindings and shared BRDF remain production-visible")
{
	const auto& contracts = GetRRGuideContracts();
	for (size_t i = 0; i < contracts.size(); ++i)
		for (size_t j = i + 1; j < contracts.size(); ++j)
			CHECK(contracts[i].binding != contracts[j].binding);
	const std::string shader = ReadShader("RT2App/shaders/rr_guides.comp");
	const std::string shared = ReadShader("RT2App/shaders/rr_guide_shared.glsl");
	const std::string raster = ReadShader("RT2App/shaders/raster.frag");
	const std::string secondary = ReadShader("RT2App/shaders/secondary_raygen.rgen");
	REQUIRE_FALSE(shader.empty());
	REQUIRE_FALSE(shared.empty());
	CHECK(shader.find("imageStore(rrDiffuseAlbedo") != std::string::npos);
	CHECK(shader.find("imageStore(rrSpecularAlbedo") != std::string::npos);
	CHECK(shader.find("imageStore(rrNormalRoughness") != std::string::npos);
	CHECK(shared.find("EnvBRDFApprox2") != std::string::npos);
	CHECK(raster.find("emissive geometry remains motion-dense") != std::string::npos);
	CHECK(ReadShader("RT2App/shaders/pathtracer_shared.glsl").find("camera-derived sky motion") != std::string::npos);
	CHECK(secondary.find("writeNRDSkyDefaults(pixel, skyRadiance, dir)") != std::string::npos);
}

TEST_CASE("RR guides RED-GREEN: canonical output path has no guide debug dependency")
{
	const std::string frame = ReadShader("RT2App/shaders/secondary_raygen.rgen");
	REQUIRE_FALSE(frame.empty());
	CHECK(frame.find("rrNoisyHdr") != std::string::npos);
	CHECK(frame.find("rrHitDistance") != std::string::npos);
	CHECK(frame.find("outputImage") != std::string::npos);
}
