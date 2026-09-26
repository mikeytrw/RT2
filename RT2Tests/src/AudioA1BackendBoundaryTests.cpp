#include <doctest/doctest.h>

// ============================================================================
// A1 - Miniaudio build boundary (CPU side).
//
// This translation unit pins the A1 backend boundary from the CPU-only side
// and deliberately nothing more:
//
//  1. The vendored pin constants (version, commit, no-device PCM format) are
//     visible through the miniaudio-free AudioBackendPin.h. Including that
//     header must never import miniaudio: the hard #error guard below fails
//     the build if miniaudio.h ever becomes reachable from RT2Tests.
//  2. No backend translation unit may leak into this target: the gate script
//     (run_audio_a1_gates.ps1) proves the adapter and miniaudio.c compile
//     exactly once in RT2AudioBackend and that this project neither compiles
//     nor links them.
//
// Out of scope (explicit): no-device rendering, decode, mixer, and sample
// oracles (A4 owns those through RT2AudioProbe and the production adapter).
// ============================================================================

#if __has_include("miniaudio.h")
#error "A1 boundary: RT2Tests must not import miniaudio (check 17; the adapter stays in RT2AudioBackend)"
#endif

#include "AudioBackendPin.h"

#include <string>

TEST_CASE("A1 GREEN_BackendPinMatchesVendoredMiniaudio")
{
    // Pin identity for required checks 16 and 17. Must match the upstream
    // 0.11.25 release at commit 9634bedb5b5a2ca38c1ee7108a9358a4e233f14d
    // (VENDORING.md records the exact source URLs and byte hashes; the gate
    // script re-hashes the vendored bytes). A pin change updates the pin
    // header, the vendored bytes, and VENDORING.md together.
    CHECK(rt2::audio::backend::kPinnedMiniaudioMajor == 0);
    CHECK(rt2::audio::backend::kPinnedMiniaudioMinor == 11);
    CHECK(rt2::audio::backend::kPinnedMiniaudioRevision == 25);
    CHECK(std::string(rt2::audio::backend::kPinnedMiniaudioVersionString) == "0.11.25");
    CHECK(std::string(rt2::audio::backend::kPinnedMiniaudioCommit) ==
          "9634bedb5b5a2ca38c1ee7108a9358a4e233f14d");
}

TEST_CASE("A1 GREEN_NoDeviceFormatContractIsFixed")
{
    // Production no-device PCM contract from the READY plan: fixed
    // two-channel 48 kHz float32, bounded [1, 4096] render requests. A4
    // proves exact samples through this contract; A1 only pins it here so a
    // future format change reads as an owned handoff rather than drift.
    CHECK(rt2::audio::backend::kNoDeviceSampleRate == 48000);
    CHECK(rt2::audio::backend::kNoDeviceChannels == 2);
    CHECK(std::string(rt2::audio::backend::kNoDeviceFormatName) == "float32");
    CHECK(rt2::audio::backend::kNoDeviceMaxFramesPerRender == 4096);
}

TEST_CASE("A1 GREEN_CpuTargetHasNoBackendBoundary")
{
    // Counterpart of the A0 CPU boundary case for required check 17. The
    // hard #error guard at the top of this file fails the build if
    // miniaudio.h ever becomes reachable from RT2Tests; this case exists so
    // the guarantee is visible in test listings and counts.
    CHECK(true);
}
