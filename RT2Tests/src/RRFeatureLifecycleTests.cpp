#include <doctest/doctest.h>

#include "RRFeatureLifecycle.h"
#include "RenderSettings.h"

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
	int resets = 0;
	hooks.queryOptimalSettings = [](OutputExtent e, RRQualityMode, RROptimalSettings& out, std::string&) {
		out = QualitySettings(e.Width(), e.Height()); return true;
	};
	hooks.create = [](const RRQualityTuple&, std::string&) { return true; };
	hooks.waitIdle = [](std::string&) { return true; };
	hooks.release = [](std::string&) { return true; };
	hooks.resetHistory = [&] { ++resets; };
	REQUIRE(lifecycle.Reconcile(output, hooks));
	lifecycle.MarkEvaluationSubmitted(true);
	CHECK_FALSE(lifecycle.ResetPending());
	REQUIRE(lifecycle.SetIneligible(output, "unsupported camera projection", hooks));
	CHECK(lifecycle.Backend() == RRBackend::NativeNRD);
	CHECK(lifecycle.ResetPending());
	CHECK(resets == 2);
	CHECK(lifecycle.State().historyResetGeneration == 2);
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
	CHECK(ClassifyRREligibility(true, true, false, true, 0.0f, true).IsDiagnosticBypass());
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
	int queries = 0, creates = 0, resets = 0;
	RRFeatureHooks hooks;
	hooks.queryOptimalSettings = [&](OutputExtent e, RRQualityMode, RROptimalSettings& out, std::string&) {
		++queries; out = QualitySettings(e.Width(), e.Height()); return true;
	};
	hooks.create = [&](const RRQualityTuple&, std::string&) { ++creates; return true; };
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
	REQUIRE(lifecycle.InvalidateResources(hooks));
	CHECK(lifecycle.Backend() == RRBackend::FallbackPending);
	lifecycle.SetFallbackRecovered();
	CHECK(lifecycle.Backend() == RRBackend::ActiveNativeNRD);
	CHECK(lifecycle.State().failureLatched);
	CHECK(lifecycle.FallbackReason() == reason);
	CHECK(creates == 1);
	lifecycle.SetNativeNrdUnavailable();
	CHECK(lifecycle.Backend() == RRBackend::NativeNRD);
}

TEST_CASE("W4 production dispatch and headless persistence decisions are checked")
{
	// Production six-argument authority throughout (the old four-argument
	// overload is removed; dispatch-only booleans missed R1).
	CHECK(ShouldRecordNativeNRD(RRBackend::NativeNRD, false, true, true, true, false));
	CHECK(ShouldRecordNativeNRD(RRBackend::ActiveNativeNRD, false, true, true, true, false));
	CHECK_FALSE(ShouldRecordNativeNRD(RRBackend::ActiveNativeNRD, false, true, false, true, false));
	CHECK_FALSE(ShouldRecordNativeNRD(RRBackend::ActiveNativeNRD, false, true, true, false, false));
	CHECK_FALSE(ShouldRecordNativeNRD(RRBackend::NativeDiagnosticBypass, true, true, true, true, false));
	CHECK_FALSE(ShouldRecordNativeNRD(RRBackend::NativeNRD, true, true, true, true, false));
	CHECK(ShouldCommitHeadlessOutput(true, true, true, true, false));
	CHECK_FALSE(ShouldCommitHeadlessOutput(false, true, true, true, false));
	CHECK_FALSE(ShouldCommitHeadlessOutput(true, false, true, true, false));
	CHECK_FALSE(ShouldCommitHeadlessOutput(true, true, false, true, false));
	CHECK_FALSE(ShouldCommitHeadlessOutput(true, true, true, false, false));
	CHECK_FALSE(ShouldCommitHeadlessOutput(true, true, true, true, true));
	CHECK(RequiredRenderStagesAvailable(true, false, true, false));
	CHECK(RequiredRenderStagesAvailable(true, true, true, true));
	CHECK_FALSE(RequiredRenderStagesAvailable(false, false, true, true));
	CHECK_FALSE(RequiredRenderStagesAvailable(true, false, false, true));
	CHECK_FALSE(RequiredRenderStagesAvailable(true, true, true, false));
	CHECK(ShouldRecordNativeNRD(RRBackend::ActiveNativeNRD, false, true, false, true, true));
	// R2: approved fallback bypasses raster-first gating, so a pure-path
	// fallback (rasterFirst=false) still dispatches native NRD.
	CHECK(ShouldRecordNativeNRD(RRBackend::ActiveNativeNRD, false, false, false, true, true));
	CHECK_FALSE(ShouldRecordNativeNRD(RRBackend::NativeDiagnosticBypass, true, true, false, true, true));
	CHECK(ShouldForceStaticRRNoJitter(true, false, false));
	CHECK(ShouldForceStaticRRNoJitter(false, true, false));
	CHECK_FALSE(ShouldForceStaticRRNoJitter(false, false, false));
}

TEST_CASE("W4 production policy gives diagnostic bypass precedence over unavailable NGX")
{
	const auto decision = ClassifyRREligibility(true, false, true, true, 0.0f, true);
	CHECK(decision.kind == RREligibility::NativeDiagnosticBypass);
	CHECK(decision.IsDiagnosticBypass());
	CHECK(std::string(decision.Reason()) == "RR is unsupported for G-buffer debug mode");
	CHECK_FALSE(ShouldRecordNativeNRD(RRBackend::NativeDiagnosticBypass, true,
		true, false, true, false));
	CHECK(ShouldRecordNativeNRD(RRBackend::ActiveNativeNRD, false,
		true, false, true, true));
}

TEST_CASE("W4 zero-jitter policy reads the real non-ReSTIR production settings")
{
	RenderSettings settings;
	settings.restirEnabled = false;
	settings.restirGIEnabled = false;
	CHECK(ShouldForceStaticRRNoJitter(true, settings.restirEnabled, settings.restirGIEnabled));
	CHECK_FALSE(ShouldForceStaticRRNoJitter(false, settings.restirEnabled, settings.restirGIEnabled));
}

TEST_CASE("W4 production scene cuts request one RR history reset and steady frames do not")
{
	RRFeatureLifecycle lifecycle;
	lifecycle.SetRequested(true);
	int resets = 0;
	RRFeatureHooks hooks;
	hooks.resetHistory = [&] { ++resets; };
	const auto output = *OutputExtent::TryCreate(1280, 720);
	CHECK(lifecycle.SetIneligible(output, "native fallback", hooks,
		RRIneligibleMode::ActiveNativeNRD));
	lifecycle.MarkEvaluationSubmitted(false);
	const uint64_t before = lifecycle.State().historyResetGeneration;
	// Production host sequence: scene setter (SetScene/SetSceneKeepTextures)
	// immediately followed by ResetAccumulation (WalnutApp scene-changed
	// handler; EditorSyncRouter full/material sync plus router reset).
	// Both request; the pair must own exactly one generation edge.
	lifecycle.RequestHistoryReset(hooks);
	lifecycle.RequestHistoryReset(hooks);
	CHECK(lifecycle.State().historyResetGeneration == before + 1);
	CHECK(resets == 1);
	CHECK(lifecycle.ResetPending());
	lifecycle.MarkEvaluationSubmitted(true);
	CHECK_FALSE(lifecycle.ResetPending());
	CHECK(lifecycle.State().historyResetGeneration == before + 1);
	// A second host transition after consumption owns exactly one more edge.
	lifecycle.RequestHistoryReset(hooks);
	lifecycle.RequestHistoryReset(hooks);
	CHECK(lifecycle.State().historyResetGeneration == before + 2);
	CHECK(resets == 2);
}

TEST_CASE("W4 effective NRD authority drives fallback signal production")
{
	// R1: automatic fallback enables NRD inputs with authored NRD disabled.
	CHECK(EffectiveNrdEnabled(false, true));
	CHECK(EffectiveNrdEnabled(true, false));
	CHECK(EffectiveNrdEnabled(true, true));
	CHECK_FALSE(EffectiveNrdEnabled(false, false));
	// Effective state, not authored state, is what dispatch must observe:
	// authored-off fallback dispatches; authored-off switch-off does not.
	CHECK(ShouldRecordNativeNRD(RRBackend::ActiveNativeNRD, false, true, false, true, true));
	CHECK_FALSE(ShouldRecordNativeNRD(RRBackend::NativeNRD, false, true, false, true, false));
}

TEST_CASE("W4 automatic fallback dispatches native NRD for pure-path and DOF requests")
{
	// R2: pure-path fallback (rasterFirst=false, authored NRD off) denoises.
	CHECK(ShouldRecordNativeNRD(RRBackend::ActiveNativeNRD, false, false, false, true, true));
	// DOF/raster fallback (raster path, authored NRD off) denoises.
	CHECK(ShouldRecordNativeNRD(RRBackend::ActiveNativeNRD, false, true, false, true, true));
	// Unavailable denoiser still dispatches nothing, even on fallback.
	CHECK_FALSE(ShouldRecordNativeNRD(RRBackend::ActiveNativeNRD, false, false, false, false, true));
	// Diagnostic bypass never dispatches, even with the fallback flag set.
	CHECK_FALSE(ShouldRecordNativeNRD(RRBackend::NativeDiagnosticBypass, true, true, false, true, true));
	CHECK_FALSE(ShouldRecordNativeNRD(RRBackend::NativeDiagnosticBypass, false, true, false, true, true));
}

TEST_CASE("W4 switch-off pure-path/DOF routing stays undenoised without the developer switch")
{
	// Ordinary NativeNRD backend ignores the fallback flag: no developer
	// request means no fallback, so pure-path stays raw.
	CHECK_FALSE(ShouldRecordNativeNRD(RRBackend::NativeNRD, false, false, true, true, false));
	CHECK_FALSE(ShouldRecordNativeNRD(RRBackend::NativeNRD, false, false, true, true, true));
	CHECK_FALSE(ShouldRecordNativeNRD(RRBackend::NativeNRD, false, false, false, true, true));
	// Switch-off raster path is unchanged: authored NRD decides.
	CHECK(ShouldRecordNativeNRD(RRBackend::NativeNRD, false, true, true, true, false));
	CHECK_FALSE(ShouldRecordNativeNRD(RRBackend::NativeNRD, false, true, false, true, false));
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
