#include <doctest/doctest.h>

#include "RRFeatureLifecycle.h"

#include <array>

namespace
{
RROptimalSettings QualitySettings(uint32_t w, uint32_t h)
{
	RROptimalSettings settings;
	settings.render = *RenderExtent::TryCreate(w * 2 / 3, h * 2 / 3);
	settings.minimum = *RenderExtent::TryCreate(w / 2, h / 2);
	settings.maximum = *RenderExtent::TryCreate(w, h);
	return settings;
}
}

TEST_CASE("W4 RR lifecycle keeps default NRD completely NGX-free")
{
	RRFeatureLifecycle lifecycle;
	int queries = 0;
	RRFeatureHooks hooks;
	hooks.queryOptimalSettings = [&](OutputExtent, RRQualityMode, RROptimalSettings&, std::string&) { ++queries; return true; };
	const auto output = *OutputExtent::TryCreate(1920, 1080);
	CHECK(lifecycle.Reconcile(output, hooks));
	CHECK(lifecycle.Backend() == RRBackend::NativeNRD);
	CHECK(queries == 0);
	CHECK_FALSE(lifecycle.BeginEvaluation().useRR);
}

TEST_CASE("W4 RR lifecycle queries fixed Quality and creates one tuple")
{
	RRFeatureLifecycle lifecycle;
	lifecycle.SetRequested(true);
	const auto output = *OutputExtent::TryCreate(1920, 1080);
	int queries = 0, creates = 0, releases = 0, idles = 0, resets = 0;
	RRQualityTuple created;
	RRFeatureHooks hooks;
	hooks.queryOptimalSettings = [&](OutputExtent extent, RRQualityMode quality, RROptimalSettings& out, std::string&) {
		CHECK(quality == RRQualityMode::Quality);
		CHECK(extent == output);
		++queries; out = QualitySettings(extent.Width(), extent.Height()); return true;
	};
	hooks.create = [&](const RRQualityTuple& tuple, std::string&) { ++creates; created = tuple; return true; };
	hooks.waitIdle = [&](std::string&) { ++idles; return true; };
	hooks.release = [&](std::string&) { ++releases; return true; };
	hooks.resetHistory = [&] { ++resets; };
	REQUIRE(lifecycle.Reconcile(output, hooks));
	CHECK(lifecycle.Backend() == RRBackend::ActiveRR);
	CHECK(lifecycle.State().featureOwned);
	CHECK(created.output == output);
	CHECK(created.render == lifecycle.State().render);
	CHECK(lifecycle.State().featureGeneration == 1);
	CHECK(lifecycle.State().historyResetGeneration == 1);
	CHECK(lifecycle.ResetPending());
	REQUIRE(lifecycle.Reconcile(output, hooks));
	CHECK(creates == 1);
	CHECK(releases == 0);
	CHECK(queries == 1); // the selected Quality tuple is the sole lifecycle authority
	CHECK(resets == 1);
	REQUIRE(lifecycle.BeginEvaluation().useRR);
	CHECK(lifecycle.CompleteEvaluation(true, {}, 19, hooks).useRR);
	CHECK(lifecycle.State().lastResult == 19);
	CHECK(lifecycle.ResetPending());
	lifecycle.MarkEvaluationSubmitted(true);
	CHECK_FALSE(lifecycle.ResetPending());
	REQUIRE(lifecycle.BeginEvaluation().useRR);
	CHECK(lifecycle.CompleteEvaluation(true, {}, 20, hooks).useRR);
	CHECK_FALSE(lifecycle.ResetPending());
	lifecycle.MarkEvaluationSubmitted(false);
	CHECK_FALSE(lifecycle.ResetPending());
}

TEST_CASE("W4 RR lifecycle evaluates atomically and latches one fallback")
{
	RRFeatureLifecycle lifecycle;
	lifecycle.SetRequested(true);
	const auto output = *OutputExtent::TryCreate(1280, 720);
	int resets = 0, releaseCount = 0;
	RRFeatureHooks hooks;
	hooks.queryOptimalSettings = [](OutputExtent e, RRQualityMode, RROptimalSettings& out, std::string&) {
		out = QualitySettings(e.Width(), e.Height()); return true;
	};
	hooks.create = [](const RRQualityTuple&, std::string&) { return true; };
	hooks.waitIdle = [](std::string&) { return true; };
	hooks.release = [&](std::string&) { ++releaseCount; return true; };
	hooks.resetHistory = [&] { ++resets; };
	REQUIRE(lifecycle.Reconcile(output, hooks));
	REQUIRE(lifecycle.BeginEvaluation().useRR);
	const auto failed = lifecycle.CompleteEvaluation(false, "forced evaluate failure", 77, hooks);
	CHECK_FALSE(failed.recorded);
	CHECK(failed.preserveDisplay);
	CHECK(failed.fallbackLatched);
	CHECK(lifecycle.Backend() == RRBackend::FallbackPending);
	CHECK(lifecycle.State().failureLatched);
	CHECK(lifecycle.State().lastResult == 77);
	CHECK(resets == 2);
	CHECK_FALSE(lifecycle.Reconcile(output, hooks));
	CHECK(releaseCount == 0); // no retry or unsafe release on every frame
	CHECK(lifecycle.Backend() == RRBackend::ActiveNativeNRD);
	CHECK_FALSE(lifecycle.BeginEvaluation().useRR);
}

TEST_CASE("W4 RR reset is reasserted by resize and eligibility fallback")
{
	RRFeatureLifecycle lifecycle;
	lifecycle.SetRequested(true);
	const auto output = *OutputExtent::TryCreate(1280, 720);
	RRFeatureHooks hooks;
	hooks.queryOptimalSettings = [](OutputExtent e, RRQualityMode, RROptimalSettings& out, std::string&) {
		out = QualitySettings(e.Width(), e.Height()); return true;
	};
	hooks.create = [](const RRQualityTuple&, std::string&) { return true; };
	hooks.waitIdle = [](std::string&) { return true; };
	hooks.release = [](std::string&) { return true; };
	REQUIRE(lifecycle.Reconcile(output, hooks));
	lifecycle.MarkEvaluationSubmitted(true);
	CHECK_FALSE(lifecycle.ResetPending());
	REQUIRE(lifecycle.SetIneligible(output, "unsupported camera projection", hooks));
	CHECK(lifecycle.Backend() == RRBackend::NativeNRD);
	CHECK(lifecycle.ResetPending());
}

TEST_CASE("W4 RR lifecycle releases at idle before one resize recreate")
{
	RRFeatureLifecycle lifecycle;
	lifecycle.SetRequested(true);
	const auto first = *OutputExtent::TryCreate(1280, 720);
	const auto second = *OutputExtent::TryCreate(2560, 1440);
	std::array<const char*, 4> order{};
	size_t at = 0;
	int creates = 0, releases = 0, idles = 0;
	RRFeatureHooks hooks;
	hooks.queryOptimalSettings = [](OutputExtent e, RRQualityMode, RROptimalSettings& out, std::string&) { out = QualitySettings(e.Width(), e.Height()); return true; };
	hooks.create = [&](const RRQualityTuple&, std::string&) { ++creates; if (at < order.size()) order[at++] = "create"; return true; };
	hooks.waitIdle = [&](std::string&) { ++idles; if (at < order.size()) order[at++] = "idle"; return true; };
	hooks.release = [&](std::string&) { ++releases; if (at < order.size()) order[at++] = "release"; return true; };
	REQUIRE(lifecycle.Reconcile(first, hooks));
	REQUIRE(lifecycle.Reconcile(second, hooks));
	CHECK(creates == 2);
	CHECK(releases == 1);
	CHECK(idles == 1);
	CHECK(order[1] == "idle");
	CHECK(order[2] == "release");
	CHECK(lifecycle.State().featureGeneration == 2);
	CHECK(lifecycle.State().historyResetGeneration == 2);
}

TEST_CASE("W4 production eligibility policy is normalized and stable")
{
	const auto eligible = ClassifyRREligibility(true, true, true, false, 0.0f, true);
	CHECK(eligible.kind == RREligibility::Eligible);
	CHECK(eligible.IsEligible());
	CHECK(std::string(eligible.Reason()) == "eligible");
	const auto debug = ClassifyRREligibility(true, true, true, true, 0.0f, true);
	CHECK(debug.kind == RREligibility::NativeDiagnosticBypass);
	CHECK(debug.IsDiagnosticBypass());
	const auto pure = ClassifyRREligibility(true, true, false, false, 0.0f, true);
	CHECK(pure.kind == RREligibility::RequiresRasterFirst);
	CHECK_FALSE(pure.IsDiagnosticBypass());
	const auto dof = ClassifyRREligibility(true, true, true, false, 1.0f, true);
	CHECK(dof.kind == RREligibility::DepthOfField);
	const auto camera = ClassifyRREligibility(true, true, true, false, 0.0f, false);
	CHECK(camera.kind == RREligibility::UnsupportedCamera);
}

TEST_CASE("W4 production lifecycle keeps one generation and settles fallback truthfully")
{
	RRFeatureLifecycle lifecycle;
	lifecycle.SetRequested(true);
	const auto output = *OutputExtent::TryCreate(1920, 1080);
	int queries = 0, creates = 0, evaluates = 0, resets = 0;
	RRFeatureHooks hooks;
	hooks.queryOptimalSettings = [&](OutputExtent e, RRQualityMode, RROptimalSettings& out, std::string&) {
		++queries; out = QualitySettings(e.Width(), e.Height()); return true;
	};
	hooks.create = [&](const RRQualityTuple&, std::string&) { ++creates; return true; };
	hooks.evaluate = [&](std::string&) { ++evaluates; return true; };
	hooks.waitIdle = [](std::string&) { return true; };
	hooks.release = [](std::string&) { return true; };
	hooks.resetHistory = [&] { ++resets; };
	REQUIRE(lifecycle.Reconcile(output, hooks));
	CHECK(lifecycle.State().featureGeneration == 1);
	CHECK(resets == 1);
	REQUIRE(lifecycle.Reconcile(output, hooks));
	CHECK(queries == 1);
	CHECK(creates == 1);
	CHECK(lifecycle.State().featureGeneration == 1);
	REQUIRE(lifecycle.BeginEvaluation().useRR);
	REQUIRE(lifecycle.CompleteEvaluation(true, {}, 0, hooks).useRR);
	lifecycle.MarkEvaluationSubmitted(true);
	CHECK_FALSE(lifecycle.ResetPending());
	REQUIRE(lifecycle.BeginEvaluation().useRR);
	REQUIRE(lifecycle.CompleteEvaluation(false, "injected", 9, hooks).fallbackLatched);
	CHECK(lifecycle.Backend() == RRBackend::FallbackPending);
	CHECK(lifecycle.State().failureLatched);
	const std::string reason = lifecycle.FallbackReason();
	CHECK_FALSE(lifecycle.Reconcile(output, hooks));
	CHECK(lifecycle.Backend() == RRBackend::ActiveNativeNRD);
	CHECK(lifecycle.State().failureLatched);
	CHECK(lifecycle.FallbackReason() == reason);
	CHECK(creates == 1);
	CHECK(evaluates == 0);
}

TEST_CASE("W4 RR lifecycle rejects malformed optimal dimensions loudly")
{
	RRFeatureLifecycle lifecycle;
	lifecycle.SetRequested(true);
	RRFeatureHooks hooks;
	hooks.queryOptimalSettings = [](OutputExtent, RRQualityMode, RROptimalSettings& out, std::string&) { out = {}; return true; };
	hooks.create = [](const RRQualityTuple&, std::string&) { return true; };
	const auto output = *OutputExtent::TryCreate(1280, 720);
	CHECK_FALSE(lifecycle.Reconcile(output, hooks));
	CHECK(lifecycle.State().failureLatched);
	CHECK(lifecycle.FallbackReason().find("zero extent") != std::string::npos);
	CHECK_FALSE(lifecycle.State().featureOwned);
}
