#include <doctest/doctest.h>

#include "DenoiserMode.h"
#include "RRFeatureLifecycle.h"
#include "CLIArgs.h"
#include <fstream>
#include <iterator>

namespace
{
std::string ReadSource(const char* path)
{
	std::ifstream in(path, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
}

// ============================================================================
// Typed denoiser authority (amendment 2026-09-09, step 2).
//
// One authored enum, one effective rule, one completed-frame resolver. These
// tests compile against the production headers and fail if a second boolean
// flag is reintroduced alongside the mode, if RR and NRD can overlap on one
// frame, or if the completed label ever reports the requested setting.
// ============================================================================

TEST_CASE("DenoiserMode: CLI spelling resolves through one authority")
{
	CHECK(ParseDenoiserMode("off") == DenoiserMode::Off);
	CHECK(ParseDenoiserMode("nrd") == DenoiserMode::NRD);
	CHECK(ParseDenoiserMode("rr") == DenoiserMode::RayReconstruction);
	CHECK(ParseDenoiserMode("ray-reconstruction") == DenoiserMode::RayReconstruction);
	CHECK(ParseDenoiserMode("dlss-rr") == DenoiserMode::RayReconstruction);
	CHECK_FALSE(ParseDenoiserMode("nrd-rr-both").has_value());
	CHECK_FALSE(ParseDenoiserMode("").has_value());
	CHECK_FALSE(ParseDenoiserMode("ultra").has_value());
	CHECK(ParseDlssQuality("quality") == DlssQualityMode::Quality);
	CHECK(ParseDlssQuality("balanced") == DlssQualityMode::Balanced);
	CHECK(ParseDlssQuality("performance") == DlssQualityMode::Performance);
	CHECK_FALSE(ParseDlssQuality("ultra-performance").has_value());
	CHECK_FALSE(ParseDlssQuality("").has_value());
}

TEST_CASE("DenoiserMode: CLI parse carries selector and preset fields")
{
	const char* argv[] = { "RT2App", "--denoiser-mode", "rr", "--rr-quality", "balanced" };
	CLIArgs args = CLIArgs::Parse(5, const_cast<char**>(argv));
	CHECK(args.denoiserMode == "rr");
	CHECK(args.rrQuality == "balanced");
	CHECK(ParseDenoiserMode(args.denoiserMode) == DenoiserMode::RayReconstruction);
	CHECK(ParseDlssQuality(args.rrQuality) == DlssQualityMode::Balanced);
	const char* compat[] = { "RT2App", "--nrd", "--dev-rr-static" };
	CLIArgs compatArgs = CLIArgs::Parse(3, const_cast<char**>(compat));
	CHECK(compatArgs.nrd);
	CHECK(compatArgs.devRRStatic);
}

TEST_CASE("DenoiserMode: authored predicates and effective rule")
{
	CHECK(IsNrdAuthored(DenoiserMode::NRD));
	CHECK_FALSE(IsNrdAuthored(DenoiserMode::Off));
	CHECK_FALSE(IsNrdAuthored(DenoiserMode::RayReconstruction));
	CHECK(IsRRRequested(DenoiserMode::RayReconstruction));
	CHECK_FALSE(IsRRRequested(DenoiserMode::NRD));
	CHECK_FALSE(IsRRRequested(DenoiserMode::Off));
	CHECK(EffectiveNrdEnabled(DenoiserMode::NRD, false));
	CHECK(EffectiveNrdEnabled(DenoiserMode::Off, true));
	CHECK(EffectiveNrdEnabled(DenoiserMode::RayReconstruction, true));
	CHECK_FALSE(EffectiveNrdEnabled(DenoiserMode::Off, false));
	CHECK_FALSE(EffectiveNrdEnabled(DenoiserMode::RayReconstruction, false));
	// Legacy bool spelling delegates identically.
	CHECK(EffectiveNrdEnabled(true, false) == EffectiveNrdEnabled(DenoiserMode::NRD, false));
	CHECK(EffectiveNrdEnabled(false, true) == EffectiveNrdEnabled(DenoiserMode::Off, true));
}

TEST_CASE("DenoiserMode: completed label derives from the frame outcome")
{
	using RB = RRBackend;
	CHECK(ResolveCompletedDenoiser(false, false, RB::NativeNRD) == CompletedDenoiser::Off);
	CHECK(ResolveCompletedDenoiser(false, true, RB::NativeNRD) == CompletedDenoiser::NRD);
	CHECK(ResolveCompletedDenoiser(false, true, RB::ActiveNativeNRD) == CompletedDenoiser::NRDFallback);
	CHECK(ResolveCompletedDenoiser(true, false, RB::ActiveRR) == CompletedDenoiser::RayReconstruction);
	// RR+NRD overlap on one successful frame is not a valid backend: the
	// resolver refuses to present it rather than picking one.
	CHECK(ResolveCompletedDenoiser(true, true, RB::ActiveRR) == CompletedDenoiser::None);
	CHECK(ResolveCompletedDenoiser(true, true, RB::ActiveNativeNRD) == CompletedDenoiser::None);
	// R7: a debug frame reports the diagnostic bypass itself, never Off —
	// even though it records neither NRD nor RR.
	CHECK(ResolveCompletedDenoiser(false, false, RB::NativeDiagnosticBypass) ==
		CompletedDenoiser::NativeDiagnosticBypass);
	CHECK(ResolveCompletedDenoiser(false, false, RB::NativeDiagnosticBypass) ==
		CompletedDenoiser::NativeDiagnosticBypass);
	CHECK(std::string(CompletedDenoiserName(CompletedDenoiser::NRDFallback)) ==
		"NRD (RR fallback)");
	CHECK(std::string(CompletedDenoiserName(CompletedDenoiser::NativeDiagnosticBypass)) ==
		"Diagnostic bypass");
	CHECK(std::string(DenoiserModeName(DenoiserMode::RayReconstruction)) ==
		"DLSS Ray Reconstruction");
}

TEST_CASE("DenoiserMode: fence-gated tracker publishes only proven frames")
{
	CompletedSnapshotTracker<2> tracker;
	RRCompletedFrameSnapshot submitted;
	submitted.denoiser = CompletedDenoiser::NRD;
	// Nothing reaped before anything submitted.
	CHECK_FALSE(tracker.ReapCompleted(0).has_value());
	// Submission alone does not publish.
	tracker.Submit(0, submitted);
	// Reaping a different slot publishes nothing.
	CHECK_FALSE(tracker.ReapCompleted(1).has_value());
	// Fence completion of the submitted slot promotes exactly once.
	auto promoted = tracker.ReapCompleted(0);
	REQUIRE(promoted.has_value());
	CHECK(promoted->denoiser == CompletedDenoiser::NRD);
	CHECK_FALSE(tracker.ReapCompleted(0).has_value());
	// Wraparound: slot index is modulo capacity.
	RRCompletedFrameSnapshot later;
	later.denoiser = CompletedDenoiser::RayReconstruction;
	tracker.Submit(2, later);
	auto wrapped = tracker.ReapCompleted(0);
	REQUIRE(wrapped.has_value());
	CHECK(wrapped->denoiser == CompletedDenoiser::RayReconstruction);
}

TEST_CASE("DenoiserMode: active RR never records NRD on any flag combination")
{
	// Recording probe over the full dispatch grid: while the lifecycle owns
	// an RR feature, no gbuffer-debug/rasterFirst/availability/fallback
	// combination may select the NRD path. A violation would evaluate RR and
	// record NRD for the same frame.
	for (bool debug : {false, true})
		for (bool rasterFirst : {false, true})
			for (bool nrdEnabled : {false, true})
				for (bool nrdAvailable : {false, true})
					for (bool fallback : {false, true})
						CHECK_FALSE(ShouldRecordNativeNRD(RRBackend::ActiveRR,
							debug, rasterFirst, nrdEnabled, nrdAvailable, fallback));
	// Requested-but-not-active RR likewise records nothing until activation.
	for (bool fallback : {false, true})
		CHECK_FALSE(ShouldRecordNativeNRD(RRBackend::RequestedRR,
			false, true, false, true, fallback));
}

TEST_CASE("DenoiserMode: Off mode with no fallback never dispatches NRD")
{
	for (bool debug : {false, true})
		for (bool rasterFirst : {false, true})
			for (bool nrdAvailable : {false, true})
			{
				CHECK_FALSE(ShouldRecordNativeNRD(RRBackend::NativeNRD,
					debug, rasterFirst, false, nrdAvailable, false));
				CHECK_FALSE(ShouldRecordNativeNRD(RRBackend::ActiveNativeNRD,
					debug, rasterFirst, false, nrdAvailable, false));
			}
}

TEST_CASE("DenoiserMode: session fallback reacts once and stays session-local")
{
	using DM = DenoiserMode;
	using RB = RRBackend;
	// Requested-but-native covers every fallback road (latched fault,
	// ineligible mode, unavailable runtime): the session flips to NRD once.
	CHECK(ResolveSessionFallbackDenoiser(DM::RayReconstruction, RB::ActiveNativeNRD, false) == DM::NRD);
	// Second sighting: already applied, hold.
	CHECK_FALSE(ResolveSessionFallbackDenoiser(DM::RayReconstruction, RB::ActiveNativeNRD, true).has_value());
	// Anything else holds: successful RR, plain native after moving on,
	// diagnostic bypass, intermediate states.
	CHECK_FALSE(ResolveSessionFallbackDenoiser(DM::RayReconstruction, RB::ActiveRR, false).has_value());
	CHECK_FALSE(ResolveSessionFallbackDenoiser(DM::RayReconstruction, RB::NativeNRD, false).has_value());
	CHECK_FALSE(ResolveSessionFallbackDenoiser(DM::RayReconstruction, RB::NativeDiagnosticBypass, false).has_value());
	CHECK_FALSE(ResolveSessionFallbackDenoiser(DM::RayReconstruction, RB::RequestedRR, false).has_value());
	CHECK_FALSE(ResolveSessionFallbackDenoiser(DM::RayReconstruction, RB::FallbackPending, false).has_value());
	// Non-RR authorship never maps: NRD and Off sessions are untouched.
	CHECK_FALSE(ResolveSessionFallbackDenoiser(DM::NRD, RB::ActiveNativeNRD, false).has_value());
	CHECK_FALSE(ResolveSessionFallbackDenoiser(DM::Off, RB::ActiveNativeNRD, false).has_value());
	// The decision is pure: inputs are unchanged, only the mapped mode comes out.
	DM authored = DM::RayReconstruction;
	CHECK(ResolveSessionFallbackDenoiser(authored, RB::ActiveNativeNRD, false) == DM::NRD);
	CHECK(authored == DM::RayReconstruction);
}

TEST_CASE("DenoiserMode: session fallback cannot rewrite a durable file")
{
	// There is no render-settings persistence layer: the durable documents
	// (editor settings, project file) carry no denoiser state, and
	// RenderSettings itself has no save/load entry points. A fallback that
	// only writes the session struct therefore cannot reach disk.
	for (const char* path : { "RT2App/src/EditorSettings.h",
		"RT2App/src/Project.h" })
	{
		const std::string source = ReadSource(path);
		REQUIRE(!source.empty());
		CHECK(source.find("DenoiserMode") == std::string::npos);
		CHECK(source.find("RenderSettings") == std::string::npos);
		CHECK(source.find("nrdEnabled") == std::string::npos);
	}
	const std::string settings = ReadSource("RT2App/src/RenderSettings.h");
	REQUIRE(!settings.empty());
	CHECK(settings.find("Save") == std::string::npos);
	CHECK(settings.find("Load") == std::string::npos);
	CHECK(settings.find("serialize") == std::string::npos);
	CHECK(settings.find("to_json") == std::string::npos);
}

TEST_CASE("DenoiserMode: CLI resolution maps once onto the authored enum")
{
	// Bare compat switch requests RR+Quality through the mode only; without
	// the experimental opt-in the gated RR admission is invalid (loud
	// rejection, rendering defaults kept).
	const auto legacy = ResolveDenoiserSelectionFromCLI(true, false, "", "", false);
	CHECK(legacy.mode == DenoiserMode::RayReconstruction);
	CHECK(legacy.quality == DlssQualityMode::Quality);
	CHECK_FALSE(legacy.modeValid);
	CHECK(legacy.qualityValid);
	CHECK(!legacy.rejection.empty());
	// R3 killer: explicit Off wins over a stale developer switch, so
	// selecting Off always stops RR — no renderer-side override remains.
	// Off needs no opt-in.
	const auto explicitOff = ResolveDenoiserSelectionFromCLI(true, false, "off", "", false);
	CHECK(explicitOff.mode == DenoiserMode::Off);
	CHECK(explicitOff.modeValid);
	// Explicit NRD likewise wins; the incompatible input combination cannot
	// be reintroduced by the switch.
	const auto explicitNrd = ResolveDenoiserSelectionFromCLI(true, true, "nrd", "", false);
	CHECK(explicitNrd.mode == DenoiserMode::NRD);
	CHECK(explicitNrd.modeValid);
	// Compat --nrd alone maps to NRD; nothing maps to RR implicitly.
	const auto compatNrd = ResolveDenoiserSelectionFromCLI(false, true, "", "", false);
	CHECK(compatNrd.mode == DenoiserMode::NRD);
	CHECK(compatNrd.modeValid);
	// Explicit preset travels with explicit mode but stays gated.
	const auto preset = ResolveDenoiserSelectionFromCLI(false, false, "rr", "performance", false);
	CHECK(preset.mode == DenoiserMode::RayReconstruction);
	CHECK(preset.quality == DlssQualityMode::Performance);
	CHECK_FALSE(preset.modeValid);
	CHECK_FALSE(preset.qualityValid);
	// Opt-in admits RR and non-Quality presets for experimental use.
	const auto admitted = ResolveDenoiserSelectionFromCLI(false, false, "rr", "balanced", true);
	CHECK(admitted.mode == DenoiserMode::RayReconstruction);
	CHECK(admitted.quality == DlssQualityMode::Balanced);
	CHECK(admitted.modeValid);
	CHECK(admitted.qualityValid);
	const auto admittedLegacy = ResolveDenoiserSelectionFromCLI(true, false, "", "", true);
	CHECK(admittedLegacy.mode == DenoiserMode::RayReconstruction);
	CHECK(admittedLegacy.modeValid);
	// Typos stay loud and keep rendering defaults.
	const auto bad = ResolveDenoiserSelectionFromCLI(false, false, "turbo", "ultra", false);
	CHECK_FALSE(bad.modeValid);
	CHECK_FALSE(bad.qualityValid);
	CHECK(bad.mode == DenoiserMode::NRD);
	CHECK(bad.quality == DlssQualityMode::Quality);
	CHECK(bad.rejection.empty());
}
