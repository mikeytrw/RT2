#include <doctest/doctest.h>

#include "DenoiserMode.h"
#include "RRFeatureLifecycle.h"
#include "CLIArgs.h"

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
	CHECK(ResolveCompletedDenoiser(false, false, false) == CompletedDenoiser::Off);
	CHECK(ResolveCompletedDenoiser(false, true, false) == CompletedDenoiser::NRD);
	CHECK(ResolveCompletedDenoiser(false, true, true) == CompletedDenoiser::NRDFallback);
	CHECK(ResolveCompletedDenoiser(true, false, false) == CompletedDenoiser::RayReconstruction);
	// RR+NRD overlap on one successful frame is not a valid backend: the
	// resolver refuses to present it rather than picking one.
	CHECK(ResolveCompletedDenoiser(true, true, false) == CompletedDenoiser::None);
	CHECK(ResolveCompletedDenoiser(true, true, true) == CompletedDenoiser::None);
	CHECK(std::string(CompletedDenoiserName(CompletedDenoiser::NRDFallback)) ==
		"NRD (RR fallback)");
	CHECK(std::string(DenoiserModeName(DenoiserMode::RayReconstruction)) ==
		"DLSS Ray Reconstruction");
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
