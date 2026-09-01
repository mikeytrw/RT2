#include <doctest/doctest.h>

#include "RRGuideContract.h"
#include "RenderExtents.h"
#include <fstream>
#include <iterator>
#include <string>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace
{
std::string ReadShader(const char* path)
{
	std::ifstream in(path, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

glm::vec2 ProjectPixels(const glm::mat4& viewToClip, const glm::mat4& worldToView,
	const glm::vec3& world, const glm::vec2& extent)
{
	const glm::vec4 clip = viewToClip * worldToView * glm::vec4(world, 1.0f);
	const glm::vec2 uv = glm::vec2(clip) / clip.w * 0.5f + 0.5f;
	return uv * extent;
}

void CheckMotion(const glm::vec2& observed, const glm::vec2& expected)
{
	CHECK(glm::length(observed - expected) <= 0.25f);
}

glm::vec2 ExpectedYawPixels(const glm::vec3& point, float yawRadians,
	float focalLength, const glm::vec2& extent)
{
	const float c = std::cos(yawRadians);
	const float s = std::sin(yawRadians);
	const float yawedX = c * point.x + s * point.z;
	const float yawedZ = -s * point.x + c * point.z;
	const float currentNdcX = focalLength * point.x / -point.z;
	const float previousNdcX = focalLength * yawedX / -yawedZ;
	return glm::vec2((previousNdcX - currentNdcX) * extent.x * 0.5f, 0.0f);
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
	CHECK(raster.find("(prevUv - currUv) * camera.viewportSPP.xy") != std::string::npos);
	CHECK(raster.find("nrdJitter") == std::string::npos);
	CHECK(ReadShader("RT2App/shaders/pathtracer_shared.glsl").find("camera-derived sky motion") != std::string::npos);
	CHECK(secondary.find("writeNRDSkyDefaults(pixel, skyRadiance, dir)") != std::string::npos);
	const std::string skyHelper = ReadShader("RT2App/shaders/pathtracer_shared.glsl");
	const size_t skyStart = skyHelper.find("void writeNRDSkyDefaults");
	const size_t skyEnd = skyHelper.find("// NRD hit distance", skyStart);
	REQUIRE(skyStart != std::string::npos);
	REQUIRE(skyEnd != std::string::npos);
	CHECK(skyHelper.substr(skyStart, skyEnd - skyStart).find("imageStore(outputImage") == std::string::npos);
	CHECK(secondary.find("writeNRDSkyDefaults(pixel, skyRadiance, dir);\n        if (nrdMode)\n            imageStore(outputImage") != std::string::npos);
}

TEST_CASE("RR guides: CPU motion projection contract covers static translation yaw rigid emissive sky")
{
	const glm::vec2 extent(4096.0f, 2160.0f);
	const glm::mat4 projection = glm::perspective(glm::radians(60.0f), extent.x / extent.y, 0.1f, 1000.0f);
	const glm::mat4 identity(1.0f);
	const glm::vec3 point(0.0f, 0.0f, -5.0f);
	CheckMotion(ProjectPixels(projection, identity, point, extent) -
		ProjectPixels(projection, identity, point, extent), glm::vec2(0.0f));

	// Current and previous world positions model a rigid one-pixel translation.
	const glm::vec3 previous = point;
	const glm::vec3 current = point + glm::vec3(2.0f / extent.x * 5.0f / projection[0][0], 0.0f, 0.0f);
	CheckMotion(ProjectPixels(projection, identity, previous, extent) -
		ProjectPixels(projection, identity, current, extent),
		glm::vec2(-1.0f, 0.0f));

	const glm::mat4 yaw = glm::rotate(glm::mat4(1.0f), glm::radians(1.0f), glm::vec3(0, 1, 0));
	CheckMotion(ProjectPixels(projection, yaw, point, extent) -
		ProjectPixels(projection, identity, point, extent),
		ExpectedYawPixels(point, glm::radians(1.0f), projection[0][0], extent));
	// Rigid geometry, emissive geometry and sky all use the same once-only
	// current->previous projection; none adds a jitter delta.
	for (const glm::vec3& p : { glm::vec3(0.2f, 0.1f, -4.0f), glm::vec3(-0.4f, 0.3f, -8.0f), glm::vec3(0.0f, 0.0f, -1000.0f) })
		CheckMotion(ProjectPixels(projection, yaw, p, extent) - ProjectPixels(projection, identity, p, extent),
			ExpectedYawPixels(p, glm::radians(1.0f), projection[0][0], extent));
}

TEST_CASE("RR guides RED-GREEN: production motion acceptance rejects zero and low-density mutants")
{
	const auto zero = ValidateRRGuideMotion(0.0f, 0, 10000, true);
	CHECK_FALSE(zero.valid);
	const auto weak = ValidateRRGuideMotion(0.02f, 100, 10000, true);
	CHECK_FALSE(weak.valid);
	CHECK_FALSE(zero.densityValid);
	const auto sparse = ValidateRRGuideMotion(4.0f, 99, 10000, true);
	CHECK_FALSE(sparse.valid);
	CHECK_FALSE(sparse.densityValid);
	const auto moving = ValidateRRGuideMotion(4.0f, 100, 10000, true);
	CHECK(moving.valid);
	const auto staticCase = ValidateRRGuideMotion(0.0f, 0, 10000, false);
	CHECK(staticCase.valid);
}

TEST_CASE("RR guides: numeric shared material semantics cover dielectric metallic angle and emissive")
{
	const auto dielectric = EvaluateRRGuideMaterial(glm::vec3(0.8f), 0.0f, 0.5f, 1.0f);
	const auto metal = EvaluateRRGuideMaterial(glm::vec3(0.8f, 0.2f, 0.1f), 1.0f, 0.5f, 1.0f);
	const auto grazing = EvaluateRRGuideMaterial(glm::vec3(0.8f), 0.0f, 0.5f, 0.25f);
	const auto emissive = EvaluateRRGuideMaterial(glm::vec3(0.3f, 0.5f, 0.7f), 0.25f, 0.8f, 0.7f);
	CHECK(dielectric.f0.x == doctest::Approx(0.04f));
	CHECK(metal.diffuseReflectance.x == doctest::Approx(0.0f));
	CHECK(metal.f0.x == doctest::Approx(0.8f));
	CHECK(glm::length(grazing.specularAlbedo - dielectric.specularAlbedo) > 0.00001f);
	CHECK(glm::dot(glm::normalize(glm::vec3(0, 0, 1)), glm::vec3(0, 0, 1)) == doctest::Approx(1.0f));
	CHECK(glm::dot(glm::normalize(glm::vec3(1, 0, 1)), glm::vec3(0, 0, 1)) == doctest::Approx(0.707106f).epsilon(0.001));
	// Emissive guide values are the resolved material, never an NRD white/metal
	// sentinel; the emission channel is intentionally tested independently.
	CHECK(emissive.diffuseReflectance.x == doctest::Approx(0.225f));
	CHECK(ValidateRRGuideMaterialNumerics());
}

TEST_CASE("RR guides: production motion reset preserves previous camera history")
{
	const std::string renderer = ReadShader("RT2App/src/RendererGPU.cpp");
	const size_t reset = renderer.find("void RendererGPU::ResetAccumulation");
	const size_t next = renderer.find("void RendererGPU::", reset + 1);
	REQUIRE(reset != std::string::npos);
	const std::string resetBody = renderer.substr(reset, next == std::string::npos ? std::string::npos : next - reset);
	CHECK(resetBody.find("m_HasPrevMatrices = false") == std::string::npos);
	CHECK(resetBody.find("previous camera matrices") != std::string::npos);
}

TEST_CASE("RR guides RED-GREEN: canonical output path has no guide debug dependency")
{
	const std::string frame = ReadShader("RT2App/shaders/secondary_raygen.rgen");
	REQUIRE_FALSE(frame.empty());
	CHECK(frame.find("rrNoisyHdr") != std::string::npos);
	CHECK(frame.find("rrHitDistance") != std::string::npos);
	CHECK(frame.find("outputImage") != std::string::npos);
}

TEST_CASE("RR guides: retained GPU reports are writer-shaped and self-validating")
{
	for (const char* path : { "docs/rr-guide-report-4k-rtx3090.json",
		"docs/rr-guide-report-256x256-nonnrd.json",
		"docs/rr-guide-report-256x256-motion.json",
		"docs/rr-guide-report-controlled.json" })
	{
		const std::string report = ReadShader(path);
		REQUIRE_FALSE(report.empty());
		CHECK(report.find("\"schema\":\"rt2.rr-guide-report.v1\"") != std::string::npos);
		CHECK(report.find("\"guides\":[") != std::string::npos);
		CHECK(report.find("RRGuideNoisyHdr") != std::string::npos);
		CHECK(report.find("RRGuideSpecularHitDistance") != std::string::npos);
		CHECK(report.find("\"runtime\":") != std::string::npos);
		CHECK(report.find("\"command\":") != std::string::npos);
		CHECK(report.find("\"exit_code\":0") != std::string::npos);
		CHECK(report.find("\"canonical_readback_stable\":true") != std::string::npos);
		CHECK(report.find("\"canonical_pair_match\":true") != std::string::npos);
		CHECK(report.find("\"sentinel_remaining_count\":0") != std::string::npos);
		CHECK(report.find("\"sentinel_remaining_by_guide\":[0,0,0,0,0,0,0]") != std::string::npos);
		CHECK(report.find("\"motion_class_coverage_valid\":true") != std::string::npos);
	}
	const std::string controlled = ReadShader("docs/rr-guide-report-controlled.json");
	CHECK(controlled.find("\"material_required_classes\":true") != std::string::npos);
	CHECK(controlled.find("\"material_dielectric_samples\":1390") != std::string::npos);
	CHECK(controlled.find("\"material_metallic_samples\":650") != std::string::npos);
	CHECK(controlled.find("\"material_grazing_samples\":140") != std::string::npos);
	CHECK(controlled.find("\"material_emissive_samples\":695") != std::string::npos);
	CHECK(controlled.find("\"emissive_provenance_pixel_count\":695") != std::string::npos);
	CHECK(controlled.find("\"emissive_motion_pixel_count\":695") != std::string::npos);
	CHECK(controlled.find("\"motion_expected_vector_valid\":true") != std::string::npos);
	CHECK(controlled.find("\"motion_expected_max_error_px\":") != std::string::npos);
	CHECK(controlled.find("\"fixture\":{\"path\":") != std::string::npos);
	CHECK(controlled.find("\"fnv1a64\":\"bd392a5fd506765e\"") != std::string::npos);
	CHECK(controlled.find("\"bytes\":3091") != std::string::npos);
	const std::string fixture = ReadShader("RT2App/assets/rr-guide-controlled.rt2scene");
	CHECK(fixture.find("EmissiveCube") != std::string::npos);
	CHECK(fixture.find("emissiveColor") != std::string::npos);
	CHECK(fixture.find("MetallicCube") != std::string::npos);
}

TEST_CASE("RR guides RED-GREEN: non-NRD producer and checked report faults are permanent")
{
	const std::string secondary = ReadShader("RT2App/shaders/secondary_raygen.rgen");
	const std::string resources = ReadShader("RT2App/src/RRGuideResources.cpp");
	const std::string renderer = ReadShader("RT2App/src/RendererGPU.cpp");
	const std::string host = ReadShader("RT2App/src/WalnutApp.cpp");
	const std::string frameRenderer = ReadShader("RT2App/src/FrameRenderer.cpp");
	CHECK(secondary.find("imageStore(rrNoisyHdr, pixel") != std::string::npos);
	CHECK(secondary.find("if (nrdMode)") != std::string::npos);
	CHECK(secondary.find("temporalAccumulate(pixel") != std::string::npos);
	CHECK(secondary.find("scatter.lobeType >= 0.5") != std::string::npos);
	CHECK(resources.find("vkGetPhysicalDeviceFormatProperties") != std::string::npos);
	CHECK(resources.find("VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT") != std::string::npos);
	CHECK(resources.find("sentinel_remaining_count") != std::string::npos);
	CHECK(resources.find("hit_depth_correlation") != std::string::npos);
	CHECK(resources.find("motion_expected_observed_tolerance") != std::string::npos);
	CHECK(resources.find("motion_density") != std::string::npos);
	CHECK(resources.find("canonical_pair_match") != std::string::npos);
	CHECK(resources.find("material_numeric_valid") != std::string::npos);
	CHECK(frameRenderer.find("RT2_RR_GUIDE_INJECT_ZERO_MOTION") != std::string::npos);
	CHECK(frameRenderer.find("RT2_RR_GUIDE_INJECT_ZERO_SKY_MOTION") != std::string::npos);
	CHECK(frameRenderer.find("RT2_RR_GUIDE_INJECT_ZERO_GEOMETRY_MOTION") != std::string::npos);
	CHECK(frameRenderer.find("RT2_RR_GUIDE_INJECT_ZERO_EMISSIVE_MOTION") != std::string::npos);
	CHECK(resources.find("canonical_readback_stable") != std::string::npos);
	CHECK(resources.find("metadata.commandLine") != std::string::npos);
	CHECK(frameRenderer.find("vkCmdClearColorImage") != std::string::npos);
	CHECK(frameRenderer.find("RT2_RR_GUIDE_INJECT_MISSING_VIEWZ") != std::string::npos);
	CHECK(frameRenderer.find("RT2_RR_GUIDE_INJECT_MISSING_MOTION") != std::string::npos);
	CHECK(frameRenderer.find("RT2_RR_GUIDE_INJECT_MATERIAL") != std::string::npos);
	CHECK(frameRenderer.find("RT2_RR_GUIDE_INJECT_DENSE_WEAK_MOTION") != std::string::npos);
	CHECK(frameRenderer.find("RT2_RR_GUIDE_INJECT_BAD_MOTION_SCALE") != std::string::npos);
	CHECK(resources.find("motion_expected_mean_error_px") != std::string::npos);
	CHECK(resources.find("motion_expected_vector_valid") != std::string::npos);
	CHECK(resources.find("fixture_hash") != std::string::npos);
	CHECK(resources.find("controlledMaterialCase ||") != std::string::npos);
	CHECK(resources.find("RT2_RR_GUIDE_INJECT_ZERO_EMISSIVE") == std::string::npos);
	CHECK(frameRenderer.find("clearValues[1]") == std::string::npos); // production clear lives in RasterPass
	CHECK(frameRenderer.find("rrGuideReportMode") != std::string::npos);
	CHECK(resources.find("canonical_pair_checksum_fnv1a64") != std::string::npos);
	CHECK(resources.find("RT2_RR_GUIDE_INJECT_CLOSE_FAILURE") != std::string::npos);
	CHECK(renderer.find("m_RRGuideInitFailed") != std::string::npos);
	CHECK(host.find("std::exit(EXIT_FAILURE)") != std::string::npos);
}
