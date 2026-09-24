// AudioBackendPin.h
//
// A1: miniaudio-free pin constants for RT2AudioBackend.
//
// This header deliberately includes no miniaudio type and no RT2 header, so
// CPU-only targets (RT2Tests) can include it to pin the vendored version
// without importing miniaudio. Byte identity of the vendored sources is
// pinned separately in RT2AudioBackend/VENDORING.md and enforced by
// run_audio_a1_gates.ps1.

#pragma once

#include <cstdint>

namespace rt2::audio::backend
{

// Pinned upstream release; must match MA_VERSION_* in vendor/miniaudio/miniaudio.h.
constexpr int kPinnedMiniaudioMajor = 0;
constexpr int kPinnedMiniaudioMinor = 11;
constexpr int kPinnedMiniaudioRevision = 25;
constexpr const char* kPinnedMiniaudioVersionString = "0.11.25";
constexpr const char* kPinnedMiniaudioCommit = "9634bedb5b5a2ca38c1ee7108a9358a4e233f14d";

// Production no-device PCM format contract (READY plan: fixed two-channel
// 48 kHz float32 no-device fallback; full oracles land in A4).
constexpr uint32_t kNoDeviceSampleRate = 48000;
constexpr uint32_t kNoDeviceChannels = 2;
constexpr const char* kNoDeviceFormatName = "float32";
constexpr uint32_t kNoDeviceMaxFramesPerRender = 4096;

} // namespace rt2::audio::backend
