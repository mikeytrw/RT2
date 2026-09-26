// MiniaudioNoDeviceAdapter.cpp
//
// A1 concrete adapter implementation. This is the ONLY translation unit in
// the solution (besides vendored miniaudio.c) that includes miniaudio.h.
// It must stay inside top-level RT2AudioBackend: RT2App's broad
// src/**.cpp and vendor/**.c globs must never compile it twice (review
// finding 7). A4 owns the engine/sound-group adapter, decode cache, and
// sample oracles; this file only proves the pinned no-device path links,
// renders, and completes a memory-backed voice.

#include "MiniaudioNoDeviceAdapter.h"
#include "AudioBackendPin.h"

#include "miniaudio.h"

#include <cmath>
#include <cstring>
#include <new>
#include <vector>

namespace rt2::audio::backend
{

namespace
{

// A1 probe tone: 1 second of stereo sine, 440 Hz left / 660 Hz right at
// half amplitude. RT2-synthesized PCM (no decoder); A4 replaces it with
// decoded clip fixtures and exact sample oracles.
constexpr uint32_t kProbeToneFrames = 48000;
constexpr float kProbeToneLeftHz = 440.0f;
constexpr float kProbeToneRightHz = 660.0f;
constexpr float kProbeToneAmplitude = 0.5f;
constexpr float kTwoPi = 6.283185307179586f;

} // namespace

struct MiniaudioNoDeviceAdapter::Impl
{
    ma_engine engine{};
    bool engineOwned = false;
    // Preallocated 4096-frame stereo scratch: the production render path
    // writes here first, so a short read publishes exactly its reported
    // prefix and a hard failure publishes nothing into caller storage.
    std::vector<float> scratch;
    // Probe-tone voice state (A1 scaffolding; A4 owns real voices).
    std::vector<float> tonePcm;
    ma_audio_buffer toneBuffer{};
    bool toneBufferOwned = false;
    ma_sound toneSound{};
    bool toneSoundOwned = false;
};

MiniaudioNoDeviceAdapter::MiniaudioNoDeviceAdapter()
    : m_impl(new (std::nothrow) Impl())
{
}

MiniaudioNoDeviceAdapter::~MiniaudioNoDeviceAdapter()
{
    Shutdown();
    delete m_impl;
    m_impl = nullptr;
}

bool MiniaudioNoDeviceAdapter::Initialize(std::string& errorText)
{
    if (m_impl == nullptr)
    {
        errorText = "RT2AudioBackend: adapter storage allocation failed";
        return false;
    }
    if (m_impl->engineOwned)
    {
        errorText = "";
        return true;
    }

    ma_engine_config config = ma_engine_config_init();
    config.noDevice = MA_TRUE;
    config.channels = kNoDeviceChannels;
    config.sampleRate = kNoDeviceSampleRate;

    const ma_result result = ma_engine_init(&config, &m_impl->engine);
    if (result != MA_SUCCESS)
    {
        errorText = "RT2AudioBackend: ma_engine_init (no-device) failed";
        return false;
    }
    if (m_impl->engine.sampleRate != kNoDeviceSampleRate)
    {
        errorText = "RT2AudioBackend: no-device engine sample rate mismatch";
        ma_engine_uninit(&m_impl->engine);
        return false;
    }
    m_impl->engineOwned = true;
    m_impl->scratch.assign(static_cast<size_t>(kNoDeviceMaxFramesPerRender) * kNoDeviceChannels, 0.0f);
    errorText = "";
    return true;
}

bool MiniaudioNoDeviceAdapter::IsInitialized() const
{
    return m_impl != nullptr && m_impl->engineOwned;
}

NoDeviceRenderOutcome MiniaudioNoDeviceAdapter::RenderFrames(float* stereoInterleaved,
                                                            uint32_t sampleCapacity,
                                                            uint32_t requestedFrames)
{
    NoDeviceRenderOutcome outcome;
    if (!IsInitialized())
    {
        outcome.error = kRenderErrorNotInitialized;
        return outcome;
    }
    if (stereoInterleaved == nullptr)
    {
        outcome.error = kRenderErrorNullBuffer;
        return outcome;
    }
    if (requestedFrames < 1 || requestedFrames > kNoDeviceMaxFramesPerRender)
    {
        outcome.error = kRenderErrorRequestOutOfRange;
        return outcome;
    }
    // Undersized check before touching caller storage: needs frames*2 floats.
    const uint64_t requiredSamples = static_cast<uint64_t>(requestedFrames) * kNoDeviceChannels;
    if (static_cast<uint64_t>(sampleCapacity) < requiredSamples)
    {
        outcome.error = kRenderErrorUndersizedBuffer;
        return outcome;
    }

    ma_uint64 framesRead = 0;
    const ma_result result = ma_engine_read_pcm_frames(
        &m_impl->engine, m_impl->scratch.data(), requestedFrames, &framesRead);
    if (result != MA_SUCCESS || framesRead > requestedFrames)
    {
        outcome.error = kRenderErrorEngineFailure;
        outcome.framesRendered = 0;
        return outcome;
    }
    // An idle engine (no attached voices) succeeds with zero frames: the
    // endpoint has no attachments to mix. Zero progress is a valid short
    // count here; the editor fallback treats it as a loud failure (READY
    // plan), but the adapter reports it truthfully rather than erroring.
    const uint32_t rendered = static_cast<uint32_t>(framesRead);
    if (rendered > 0)
    {
        std::memcpy(stereoInterleaved, m_impl->scratch.data(),
                    static_cast<size_t>(rendered) * kNoDeviceChannels * sizeof(float));
    }
    outcome.ok = true;
    outcome.framesRendered = rendered;
    return outcome;
}

bool MiniaudioNoDeviceAdapter::StartProbeTone(std::string& errorText)
{
    if (!IsInitialized())
    {
        errorText = "RT2AudioBackend: cannot start probe tone on an uninitialized adapter";
        return false;
    }
    if (m_impl->toneSoundOwned)
    {
        errorText = "RT2AudioBackend: probe tone already active";
        return false;
    }

    m_impl->tonePcm.resize(static_cast<size_t>(kProbeToneFrames) * kNoDeviceChannels);
    for (uint32_t frame = 0; frame < kProbeToneFrames; ++frame)
    {
        const float t = static_cast<float>(frame) / static_cast<float>(kNoDeviceSampleRate);
        m_impl->tonePcm[static_cast<size_t>(frame) * 2 + 0] =
            kProbeToneAmplitude * std::sin(kTwoPi * kProbeToneLeftHz * t);
        m_impl->tonePcm[static_cast<size_t>(frame) * 2 + 1] =
            kProbeToneAmplitude * std::sin(kTwoPi * kProbeToneRightHz * t);
    }

    ma_audio_buffer_config bufferConfig = ma_audio_buffer_config_init(
        ma_format_f32, kNoDeviceChannels,
        static_cast<ma_uint64>(kProbeToneFrames), m_impl->tonePcm.data(), nullptr);
    bufferConfig.sampleRate = kNoDeviceSampleRate;
    if (ma_audio_buffer_init_copy(&bufferConfig, &m_impl->toneBuffer) != MA_SUCCESS)
    {
        errorText = "RT2AudioBackend: probe tone buffer init failed";
        return false;
    }
    m_impl->toneBufferOwned = true;

    if (ma_sound_init_from_data_source(
            &m_impl->engine, &m_impl->toneBuffer, 0, nullptr, &m_impl->toneSound) != MA_SUCCESS)
    {
        errorText = "RT2AudioBackend: probe tone sound init failed";
        ma_audio_buffer_uninit(&m_impl->toneBuffer);
        m_impl->toneBufferOwned = false;
        return false;
    }
    m_impl->toneSoundOwned = true;

    if (ma_sound_start(&m_impl->toneSound) != MA_SUCCESS)
    {
        errorText = "RT2AudioBackend: probe tone sound start failed";
        ma_sound_uninit(&m_impl->toneSound);
        m_impl->toneSoundOwned = false;
        ma_audio_buffer_uninit(&m_impl->toneBuffer);
        m_impl->toneBufferOwned = false;
        return false;
    }

    errorText = "";
    return true;
}

bool MiniaudioNoDeviceAdapter::IsProbeToneAtEnd() const
{
    if (m_impl == nullptr || !m_impl->toneSoundOwned)
        return false;
    return ma_sound_at_end(&m_impl->toneSound) == MA_TRUE;
}

void MiniaudioNoDeviceAdapter::Shutdown()
{
    if (m_impl == nullptr)
        return;
    // Dependency order: sound before its data source before the engine.
    if (m_impl->toneSoundOwned)
    {
        ma_sound_uninit(&m_impl->toneSound);
        m_impl->toneSoundOwned = false;
    }
    if (m_impl->toneBufferOwned)
    {
        ma_audio_buffer_uninit(&m_impl->toneBuffer);
        m_impl->toneBufferOwned = false;
    }
    m_impl->tonePcm.clear();
    if (m_impl->engineOwned)
    {
        ma_engine_uninit(&m_impl->engine);
        m_impl->engineOwned = false;
    }
}

} // namespace rt2::audio::backend
