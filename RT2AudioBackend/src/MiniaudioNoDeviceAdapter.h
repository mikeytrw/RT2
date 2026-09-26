// MiniaudioNoDeviceAdapter.h
//
// A1: minimal concrete miniaudio adapter. Owns one production ma_engine in
// explicit two-channel / 48 kHz no-device mode and renders caller-owned
// interleaved float32 stereo through the real ma_engine_read_pcm_frames()
// path. No decode, no mixer graph, no device: full decoder/mixer/sample
// oracles belong to A4.
//
// This header exposes no ma_* type (pimpl), so including it never imports
// miniaudio into a translation unit. Only MiniaudioNoDeviceAdapter.cpp
// includes vendor/miniaudio/miniaudio.h, and only the RT2AudioBackend
// project compiles that translation unit.

#pragma once

#include <cstdint>
#include <string>

namespace rt2::audio::backend
{

struct NoDeviceRenderOutcome
{
    bool ok = false;
    uint32_t framesRendered = 0;
    // Valid only when ok == false; one of the kRenderError* strings below.
    const char* error = nullptr;
};

class MiniaudioNoDeviceAdapter
{
public:
    MiniaudioNoDeviceAdapter();
    ~MiniaudioNoDeviceAdapter();

    MiniaudioNoDeviceAdapter(const MiniaudioNoDeviceAdapter&) = delete;
    MiniaudioNoDeviceAdapter& operator=(const MiniaudioNoDeviceAdapter&) = delete;
    MiniaudioNoDeviceAdapter(MiniaudioNoDeviceAdapter&&) = delete;
    MiniaudioNoDeviceAdapter& operator=(MiniaudioNoDeviceAdapter&&) = delete;

    // Opens the production engine with noDevice=true, 2 channels, 48 kHz.
    // Returns false with a diagnostic in errorText; safe to retry.
    bool Initialize(std::string& errorText);
    bool IsInitialized() const;

    // Renders up to requestedFrames through the production no-device path
    // into caller-owned interleaved stereo float32 storage (capacity must be
    // at least requestedFrames * 2 floats). requestedFrames must be in
    // [1, kNoDeviceMaxFramesPerRender]. On success only the reported prefix
    // is written; on hard failure nothing is written. Loud typed errors for
    // uninitialized use, null/undersized buffers, and out-of-range requests.
    NoDeviceRenderOutcome RenderFrames(float* stereoInterleaved,
                                       uint32_t sampleCapacity,
                                       uint32_t requestedFrames);

    void Shutdown();

    // A1 probe scaffolding: starts a one-shot RT2-synthesized stereo tone
    // (1 s, 440 Hz left / 660 Hz right) through the production engine path
    // so the no-device pump is observable without a decoder. Double-start
    // refuses loudly. A4 replaces synth PCM with decoded clip fixtures and
    // exact sample oracles; the render/count/completion plumbing stays.
    bool StartProbeTone(std::string& errorText);
    bool IsProbeToneAtEnd() const;

    static constexpr const char* kRenderErrorNotInitialized = "not-initialized";
    static constexpr const char* kRenderErrorNullBuffer = "null-buffer";
    static constexpr const char* kRenderErrorUndersizedBuffer = "undersized-buffer";
    static constexpr const char* kRenderErrorRequestOutOfRange = "request-out-of-range";
    static constexpr const char* kRenderErrorEngineFailure = "engine-failure";

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

} // namespace rt2::audio::backend
