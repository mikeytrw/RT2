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
	// Default promotion (2026-09-10): RR and all presets are ordinary
	// selections; no experimental opt-in exists. Bare compat switch
	// requests RR+Quality through the mode only.
	const auto legacy = ResolveDenoiserSelectionFromCLI(true, false, "", "");
	CHECK(legacy.mode == DenoiserMode::RayReconstruction);
	CHECK(legacy.quality == DlssQualityMode::Quality);
	CHECK(legacy.modeValid);
	CHECK(legacy.qualityValid);
	CHECK(legacy.rejection.empty());
	// R3 killer: explicit Off wins over a stale developer switch, so
	// selecting Off always stops RR — no renderer-side override remains.
	const auto explicitOff = ResolveDenoiserSelectionFromCLI(true, false, "off", "");
	CHECK(explicitOff.mode == DenoiserMode::Off);
	CHECK(explicitOff.modeValid);
	// Explicit NRD likewise wins; the incompatible input combination cannot
	// be reintroduced by the switch.
	const auto explicitNrd = ResolveDenoiserSelectionFromCLI(true, true, "nrd", "");
	CHECK(explicitNrd.mode == DenoiserMode::NRD);
	CHECK(explicitNrd.modeValid);
	// Compat --nrd alone maps to NRD; the CLI default stays NRD here (the
	// implicit RR default resolves later, once NGX support is known).
	const auto compatNrd = ResolveDenoiserSelectionFromCLI(false, true, "", "");
	CHECK(compatNrd.mode == DenoiserMode::NRD);
	CHECK(compatNrd.modeValid);
	// Explicit RR with a non-Quality preset is admitted as stated.
	const auto preset = ResolveDenoiserSelectionFromCLI(false, false, "rr", "performance");
	CHECK(preset.mode == DenoiserMode::RayReconstruction);
	CHECK(preset.quality == DlssQualityMode::Performance);
	CHECK(preset.modeValid);
	CHECK(preset.qualityValid);
	const auto balanced = ResolveDenoiserSelectionFromCLI(false, false, "rr", "balanced");
	CHECK(balanced.mode == DenoiserMode::RayReconstruction);
	CHECK(balanced.quality == DlssQualityMode::Balanced);
	CHECK(balanced.modeValid);
	CHECK(balanced.qualityValid);
	// Typos stay loud and keep rendering defaults.
	const auto bad = ResolveDenoiserSelectionFromCLI(false, false, "turbo", "ultra");
	CHECK_FALSE(bad.modeValid);
	CHECK_FALSE(bad.qualityValid);
	CHECK(bad.mode == DenoiserMode::NRD);
	CHECK(bad.quality == DlssQualityMode::Quality);
	CHECK(bad.rejection.empty());
}

TEST_CASE("DenoiserMode: implicit startup default promotes RR once supported")
{
	// No explicit selection + supported/eligible => RR Quality.
	const auto def = ResolveImplicitStartupDenoiser(false, false, true, true, true);
	REQUIRE(def.has_value());
	CHECK(def->mode == DenoiserMode::RayReconstruction);
	CHECK(def->quality == DlssQualityMode::Quality);
	// Unsupported or ineligible => NRD.
	const auto unsup = ResolveImplicitStartupDenoiser(false, false, false, true, true);
	REQUIRE(unsup.has_value());
	CHECK(unsup->mode == DenoiserMode::NRD);
	const auto inelig = ResolveImplicitStartupDenoiser(false, false, true, false, true);
	REQUIRE(inelig.has_value());
	CHECK(inelig->mode == DenoiserMode::NRD);
	// NRD unavailable => loud Off (reason carried for the session log).
	const auto off = ResolveImplicitStartupDenoiser(false, false, false, true, false);
	REQUIRE(off.has_value());
	CHECK(off->mode == DenoiserMode::Off);
	CHECK(std::string(off->reason).size() > 0);
	const auto offInelig = ResolveImplicitStartupDenoiser(false, false, true, false, false);
	REQUIRE(offInelig.has_value());
	CHECK(offInelig->mode == DenoiserMode::Off);
	// Explicit CLI or UI owns the session: resolver holds (nullopt).
	CHECK_FALSE(ResolveImplicitStartupDenoiser(true, false, true, true, true).has_value());
	CHECK_FALSE(ResolveImplicitStartupDenoiser(false, true, true, true, true).has_value());
	CHECK_FALSE(ResolveImplicitStartupDenoiser(false, true, false, true, true).has_value());
}

TEST_CASE("DenoiserMode: every renderer-init path applies the CLI selection")
{
	// Silent-failure repair: the public selector must reach the renderer
	// whether it initializes headless (pre/post-parse) or interactively.
	// Count application sites against runtime-init sites; a removed call
	// silently renders native while the setting claims RR.
	const std::string host = ReadSource("RT2App/src/WalnutApp.cpp");
	REQUIRE(!host.empty());
	size_t applies = 0, inits = 0, pos = 0;
	while ((pos = host.find("ApplyCLIDenoiserSelection(m_Settings)", pos)) != std::string::npos)
	{
		++applies;
		++pos;
	}
	pos = 0;
	while ((pos = host.find("m_RendererGPU.SetNgxRuntime(m_Ngx.get())", pos)) != std::string::npos)
	{
		++inits;
		++pos;
	}
	CHECK(inits == 2);
	CHECK(applies == 4);
	CHECK(applies > inits);
}

TEST_CASE("DenoiserMode: no unconditional NRD overwrite of explicit CLI")
{
	// Sol review (range 558913c..15d290f, major 1): the interactive
	// first-frame fixup unconditionally reset denoiserMode to NRD after
	// ProcessCLIArgs, silently discarding explicit --denoiser-mode. The
	// overwrite statement must stay gone; the fixup routes through the
	// single ApplyCLIDenoiserSelection authority instead.
	const std::string host = ReadSource("RT2App/src/WalnutApp.cpp");
	REQUIRE(!host.empty());
	CHECK(host.find("m_Settings.denoiserMode = DenoiserMode::NRD;") == std::string::npos);
	const std::string gate = "if (!g_CLI.headless && m_RendererGPU.IsAvailable())";
	const size_t at = host.find(gate);
	REQUIRE(at != std::string::npos);
	const size_t applied = host.find("m_RendererGPU.ApplySettings(m_Settings);", at);
	REQUIRE(applied != std::string::npos);
	const std::string fixup = host.substr(at, applied - at);
	CHECK(fixup.find("ApplyCLIDenoiserSelection(m_Settings)") != std::string::npos);
}

TEST_CASE("DenoiserMode: empty CLI keeps the interactive NRD default")
{
	// No-flag launches must keep rendering exactly as before the overwrite
	// removal: default NRD, valid, Quality.
	const auto def = ResolveDenoiserSelectionFromCLI(false, false, "", "");
	CHECK(def.mode == DenoiserMode::NRD);
	CHECK(def.quality == DlssQualityMode::Quality);
	CHECK(def.modeValid);
	CHECK(def.qualityValid);
	// Explicit Off still wins over the default (interactive-explicit case).
	const auto off = ResolveDenoiserSelectionFromCLI(false, false, "off", "");
	CHECK(off.mode == DenoiserMode::Off);
	CHECK(off.modeValid);
}

TEST_CASE("DenoiserMode: UI admits RR on support alone, all presets selectable")
{
	// Default promotion: the combo gates RR only on runtime support (no
	// experimental opt-in) and no preset is force-disabled. The old gated
	// statements must stay gone.
	const std::string host = ReadSource("RT2App/src/WalnutApp.cpp");
	REQUIRE(!host.empty());
	CHECK(host.find("const bool rrAdmitted = rrSupported;") != std::string::npos);
	CHECK(host.find("rrAdmitted = rrSupported && g_CLI.experimentalRR") == std::string::npos);
	CHECK(host.find("const bool gated = (i != 0);") == std::string::npos);
	CHECK(host.find("Balanced/Performance are disabled") == std::string::npos);
	CHECK(host.find("RR requires --experimental-rr") == std::string::npos);
}

TEST_CASE("DenoiserMode: completed frame never reports RR and NRD together")
{
	// Exclusive completed-frame backend presentation is preserved by the
	// promotion: a violated rr+nrd invariant resolves to None, never to a
	// valid backend.
	CHECK(ResolveCompletedDenoiser(true, true, RRBackend::ActiveRR) == CompletedDenoiser::None);
	CHECK(ResolveCompletedDenoiser(true, true, RRBackend::NativeNRD) == CompletedDenoiser::None);
	CHECK(ResolveCompletedDenoiser(true, false, RRBackend::ActiveRR) == CompletedDenoiser::RayReconstruction);
	CHECK(ResolveCompletedDenoiser(false, true, RRBackend::ActiveNativeNRD) == CompletedDenoiser::NRDFallback);
	CHECK(ResolveCompletedDenoiser(false, true, RRBackend::NativeNRD) == CompletedDenoiser::NRD);
	CHECK(ResolveCompletedDenoiser(false, false, RRBackend::NativeNRD) == CompletedDenoiser::Off);
}
