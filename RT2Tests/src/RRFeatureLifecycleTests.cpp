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
// Smaller render fractions stand in for Balanced/Performance optimal
// settings: the lifecycle must treat each preset as a distinct tuple.
RROptimalSettings PresetSettings(uint32_t w, uint32_t h, DlssQualityMode quality)
{
	RROptimalSettings settings;
	const uint32_t rw = quality == DlssQualityMode::Performance ? w / 2 :
		(quality == DlssQualityMode::Balanced ? w * 3 / 5 : w * 2 / 3);
	const uint32_t rh = quality == DlssQualityMode::Performance ? h / 2 :
		(quality == DlssQualityMode::Balanced ? h * 3 / 5 : h * 2 / 3);
	settings.render = *RenderExtent::TryCreate(rw, rh);
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
	hooks.queryOptimalSettings = [&](OutputExtent, DlssQualityMode, RROptimalSettings&, std::string&) { ++queries; return true; };
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
	hooks.queryOptimalSettings = [&](OutputExtent extent, DlssQualityMode quality, RROptimalSettings& out, std::string&) {
		CHECK(quality == DlssQualityMode::Quality);
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
	lifecycle.MarkEvaluationSubmitted();
	CHECK_FALSE(lifecycle.ResetPending());
	REQUIRE(lifecycle.BeginEvaluation().useRR);
	CHECK(lifecycle.CompleteEvaluation(true, {}, 20, hooks).useRR);
	CHECK_FALSE(lifecycle.ResetPending());
	lifecycle.MarkEvaluationSubmitted();
	CHECK_FALSE(lifecycle.ResetPending());
}

TEST_CASE("RR presets select distinct tuples and recreate exactly once")
{
	RRFeatureLifecycle lifecycle;
	lifecycle.SetRequested(true);
	CHECK(lifecycle.RequestedQuality() == DlssQualityMode::Quality);
	const auto output = *OutputExtent::TryCreate(1920, 1080);
	int queries = 0, creates = 0, releases = 0, idles = 0, resets = 0;
	DlssQualityMode queried = DlssQualityMode::Quality;
	RRFeatureHooks hooks;
	hooks.queryOptimalSettings = [&](OutputExtent e, DlssQualityMode quality,
		RROptimalSettings& out, std::string&) {
		++queries; queried = quality;
		out = PresetSettings(e.Width(), e.Height(), quality);
		return true;
	};
	hooks.create = [&](const RRQualityTuple&, std::string&) { ++creates; return true; };
	hooks.waitIdle = [&](std::string&) { ++idles; return true; };
	hooks.release = [&](std::string&) { ++releases; return true; };
	hooks.resetHistory = [&] { ++resets; };
	REQUIRE(lifecycle.Reconcile(output, hooks));
	CHECK(queried == DlssQualityMode::Quality);
	CHECK(lifecycle.State().quality == DlssQualityMode::Quality);
	lifecycle.MarkEvaluationSubmitted();
	// Same output, same preset: no new query, no recreate.
	REQUIRE(lifecycle.Reconcile(output, hooks));
	CHECK(queries == 1);
	CHECK(creates == 1);
	// Preset change: one new query, one idle/release/recreate, one reset.
	lifecycle.SetRequestedQuality(DlssQualityMode::Balanced);
	REQUIRE(lifecycle.Reconcile(output, hooks));
	CHECK(queried == DlssQualityMode::Balanced);
	CHECK(lifecycle.State().quality == DlssQualityMode::Balanced);
	CHECK(queries == 2);
	CHECK(creates == 2);
	CHECK(releases == 1);
	CHECK(idles == 1);
	CHECK(resets == 2);
	CHECK(lifecycle.State().featureGeneration == 2);
	CHECK(lifecycle.Backend() == RRBackend::ActiveRR);
	lifecycle.MarkEvaluationSubmitted();
	// Steady on the new preset: nothing moves.
	REQUIRE(lifecycle.Reconcile(output, hooks));
	CHECK(queries == 2);
	CHECK(creates == 2);
	CHECK(releases == 1);
	CHECK(resets == 2);
}

TEST_CASE("RR preset query failure latches fallback without retry")
{
	RRFeatureLifecycle lifecycle;
	lifecycle.SetRequested(true);
	lifecycle.SetRequestedQuality(DlssQualityMode::Performance);
	const auto output = *OutputExtent::TryCreate(1920, 1080);
	int queries = 0, creates = 0;
	RRFeatureHooks hooks;
	hooks.queryOptimalSettings = [&](OutputExtent, DlssQualityMode quality,
		RROptimalSettings&, std::string& detail) {
		++queries;
		if (quality == DlssQualityMode::Performance)
		{
			detail = "Performance unsupported by this runtime";
			return false;
		}
		return true;
	};
	hooks.create = [&](const RRQualityTuple&, std::string&) { ++creates; return true; };
	hooks.waitIdle = [](std::string&) { return true; };
	hooks.release = [](std::string&) { return true; };
	CHECK_FALSE(lifecycle.Reconcile(output, hooks));
	CHECK(lifecycle.State().failureLatched);
	CHECK(lifecycle.FallbackReason().find("Performance unsupported") != std::string::npos);
	CHECK(creates == 0);
	CHECK_FALSE(lifecycle.Reconcile(output, hooks));
	CHECK(queries == 1); // no per-frame retry
	CHECK(lifecycle.Backend() == RRBackend::ActiveNativeNRD);
}

TEST_CASE("W4 RR lifecycle evaluates atomically and latches one fallback")
{
	RRFeatureLifecycle lifecycle;
	lifecycle.SetRequested(true);
	const auto output = *OutputExtent::TryCreate(1280, 720);
	int resets = 0, releaseCount = 0;
	RRFeatureHooks hooks;
	hooks.queryOptimalSettings = [](OutputExtent e, DlssQualityMode, RROptimalSettings& out, std::string&) {
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
	hooks.queryOptimalSettings = [](OutputExtent e, DlssQualityMode, RROptimalSettings& out, std::string&) {
		out = QualitySettings(e.Width(), e.Height()); return true;
	};
	hooks.create = [](const RRQualityTuple&, std::string&) { return true; };
	hooks.waitIdle = [](std::string&) { return true; };
	hooks.release = [](std::string&) { return true; };
	hooks.resetHistory = [&] { ++resets; };
	REQUIRE(lifecycle.Reconcile(output, hooks));
	lifecycle.MarkEvaluationSubmitted();
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
	hooks.queryOptimalSettings = [](OutputExtent e, DlssQualityMode, RROptimalSettings& out, std::string&) { out = QualitySettings(e.Width(), e.Height()); return true; };
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
	hooks.queryOptimalSettings = [&](OutputExtent e, DlssQualityMode, RROptimalSettings& out, std::string&) {
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
	lifecycle.MarkEvaluationSubmitted();
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
	// The temporary W4 static no-jitter policy is retired: the shared
	// FrameSampling authority (FrameSamplingTests) owns jitter on every
	// backend, including requested RR.
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

TEST_CASE("RR jitter runs only for frames that will evaluate RR")
{
	CHECK(ShouldJitterSampling(true, RRBackend::ActiveRR));
	CHECK_FALSE(ShouldJitterSampling(true, RRBackend::RequestedRR));
	CHECK_FALSE(ShouldJitterSampling(true, RRBackend::ActiveNativeNRD));
	CHECK_FALSE(ShouldJitterSampling(true, RRBackend::NativeNRD));
	CHECK_FALSE(ShouldJitterSampling(true, RRBackend::FallbackPending));
	CHECK_FALSE(ShouldJitterSampling(true, RRBackend::NativeDiagnosticBypass));
	CHECK_FALSE(ShouldJitterSampling(false, RRBackend::ActiveRR));
}

TEST_CASE("R1 ordinary camera motion resets accumulation only on the Off path")
{
	using DM = DenoiserMode;
	// Legacy Off path with motion: reset (the only behavior that changes).
	CHECK(ShouldResetAccumulationOnCameraMove(DM::Off, false, true, true, true));
	// NRD (authored or fallback), RR requested in any backend: never.
	CHECK_FALSE(ShouldResetAccumulationOnCameraMove(DM::NRD, false, true, true, true));
	CHECK_FALSE(ShouldResetAccumulationOnCameraMove(DM::Off, true, true, true, true));
	CHECK_FALSE(ShouldResetAccumulationOnCameraMove(DM::RayReconstruction, false, true, true, true));
	CHECK_FALSE(ShouldResetAccumulationOnCameraMove(DM::RayReconstruction, true, true, true, true));
	CHECK_FALSE(ShouldResetAccumulationOnCameraMove(DM::NRD, true, true, true, true));
	// Missing preconditions: never, on any mode.
	CHECK_FALSE(ShouldResetAccumulationOnCameraMove(DM::Off, false, false, true, true));
	CHECK_FALSE(ShouldResetAccumulationOnCameraMove(DM::Off, false, true, false, true));
	CHECK_FALSE(ShouldResetAccumulationOnCameraMove(DM::Off, false, true, true, false));
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
	lifecycle.MarkEvaluationSubmitted();
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
	lifecycle.MarkEvaluationSubmitted();
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
	hooks.queryOptimalSettings = [](OutputExtent, DlssQualityMode, RROptimalSettings& out, std::string&) { out = {}; return true; };
	hooks.create = [](const RRQualityTuple&, std::string&) { return true; };
	const auto output = *OutputExtent::TryCreate(1280, 720);
	CHECK_FALSE(lifecycle.Reconcile(output, hooks));
	CHECK(lifecycle.State().failureLatched);
	CHECK(lifecycle.FallbackReason().find("zero extent") != std::string::npos);
	CHECK_FALSE(lifecycle.State().featureOwned);
}

TEST_CASE("R4 session mirror keeps fallback dispatch, identity and reason")
{
	// Full first-frame -> host reaction -> next-frame sequence for a
	// pure-path fallback (the settled W4 ineligible path). The host mirrors
	// the session selector WITHOUT tearing down the renderer fallback; the
	// second frame must still denoise with identity and reason intact.
	// (A teardown-on-mirror mutant dispatches nothing on frame two.)
	RRFeatureLifecycle fallback;
	fallback.SetRequested(true);
	RRFeatureHooks hooks;
	int resets = 0;
	hooks.resetHistory = [&] { ++resets; };
	const auto output = *OutputExtent::TryCreate(1280, 720);
	REQUIRE(fallback.SetIneligible(output, "RR requires raster-first mode", hooks,
		RRIneligibleMode::ActiveNativeNRD));
	// Frame one: authored RR + fallback dispatches native NRD (pure path:
	// rasterFirst=false, authored NRD off).
	const bool first = ShouldRecordNativeNRD(fallback.Backend(), false, false,
		EffectiveNrdEnabled(DenoiserMode::RayReconstruction, true), true, true);
	CHECK(first);
	fallback.MarkEvaluationSubmitted();
	// Host reaction: resolve + mirror (request state only, no teardown).
	auto authored = ResolveSessionFallbackDenoiser(
		DenoiserMode::RayReconstruction, fallback.Backend(), false);
	REQUIRE(authored.has_value());
	CHECK(*authored == DenoiserMode::NRD);
	fallback.SetRequested(IsRRRequested(*authored));
	const uint64_t afterMirror = fallback.State().historyResetGeneration;
	CHECK(fallback.Backend() == RRBackend::ActiveNativeNRD);
	// Frame two: authored NRD + retained fallback still dispatches, with
	// the exact reason preserved and no extra edge from the mirror itself.
	const bool second = ShouldRecordNativeNRD(fallback.Backend(), false, false,
		EffectiveNrdEnabled(*authored, true), true, true);
	CHECK(second);
	CHECK(fallback.State().historyResetGeneration == afterMirror);
	CHECK(fallback.FallbackReason() == "RR requires raster-first mode");
	// Explicit leave (user deselects) is a different road and is covered by
	// the renderer teardown path, not by mirroring.
}

TEST_CASE("R4a frame context carries the lifecycle for retained fallback")
{
	using RB = RRBackend;
	// Requested RR always carries it (all backends reachable while asked).
	CHECK(ShouldProvideRRLifecycle(true, RB::RequestedRR));
	CHECK(ShouldProvideRRLifecycle(true, RB::ActiveRR));
	CHECK(ShouldProvideRRLifecycle(true, RB::ActiveNativeNRD));
	// Mirrored session (request withdrawn, fallback retained) still carries
	// it: without the pointer the dispatcher substitutes plain NativeNRD
	// and pure-path gating kills the second frame. Request-only gating
	// returns false here and is exactly the reported bug.
	CHECK(ShouldProvideRRLifecycle(false, RB::ActiveNativeNRD));
	// Plain native and diagnostic states carry nothing.
	CHECK_FALSE(ShouldProvideRRLifecycle(false, RB::NativeNRD));
	CHECK_FALSE(ShouldProvideRRLifecycle(false, RB::NativeDiagnosticBypass));
	CHECK_FALSE(ShouldProvideRRLifecycle(false, RB::RequestedRR));
	CHECK_FALSE(ShouldProvideRRLifecycle(false, RB::FallbackPending));
}

TEST_CASE("R4b explicit leave clears retained provenance on any non-RR pick")
{
	using DM = DenoiserMode;
	// NRD->Off after a mirror (previous already NRD) must clear: the old
	// previous==RR rule skipped cleanup and left authored Off running NRD.
	CHECK(ShouldClearRetainedFallback(DM::Off, true));
	CHECK(ShouldClearRetainedFallback(DM::NRD, true));
	// RR picks never clear; nothing retained clears nothing.
	CHECK_FALSE(ShouldClearRetainedFallback(DM::RayReconstruction, true));
	CHECK_FALSE(ShouldClearRetainedFallback(DM::Off, false));
	CHECK_FALSE(ShouldClearRetainedFallback(DM::NRD, false));
}
