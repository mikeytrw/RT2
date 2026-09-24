// ProbeMain.cpp
//
// A1: RT2AudioProbe shell. Links RT2AudioBackend and exercises the real
// pinned miniaudio engine in fixed float32 stereo 48 kHz no-device mode:
// initialize, prove the idle no-voice pump reports zero frames, start an
// RT2-synthesized memory tone (no decoder), render exact frame counts
// through the production ma_engine_read_pcm_frames() path, observe
// completion, shut down.
//
// Loud failure policy (this codebase's characteristic bug is silent
// failure): every refusal prints to stderr and exits nonzero. Full
// decoder/mixer/sample oracles belong to A4; this probe proves the pin,
// the link, and the no-device render path only.

#include "AudioBackendPin.h"
#include "MiniaudioNoDeviceAdapter.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{

int Fail(const char* what)
{
    std::fprintf(stderr, "RT2AudioProbe FAIL: %s\n", what);
    return 1;
}

float MaxAbs(const std::vector<float>& samples)
{
    float peak = 0.0f;
    for (float s : samples)
    {
        const float a = std::fabs(s);
        if (a > peak)
            peak = a;
    }
    return peak;
}

} // namespace

int main()
{
    using namespace rt2::audio::backend;

    std::printf("RT2AudioProbe: miniaudio %s @ %s\n", kPinnedMiniaudioVersionString, kPinnedMiniaudioCommit);
    std::printf("RT2AudioProbe: requesting no-device %s %u ch %u Hz\n",
                kNoDeviceFormatName, kNoDeviceChannels, kNoDeviceSampleRate);

    MiniaudioNoDeviceAdapter adapter;
    std::string errorText;
    if (!adapter.Initialize(errorText))
    {
        std::fprintf(stderr, "RT2AudioProbe FAIL: initialize: %s\n",
                     errorText.empty() ? "unknown" : errorText.c_str());
        return 1;
    }
    if (!adapter.IsInitialized())
        return Fail("initialized adapter reports not-initialized");

    // Idle pump with no attached voices: the endpoint has no attachments to
    // mix, so the production path succeeds with zero frames. This documents
    // the idle rule explicitly rather than letting it hide.
    constexpr uint32_t kSmallFrames = 480;
    std::vector<float> idle(static_cast<size_t>(kSmallFrames) * kNoDeviceChannels, 1.0f);
    const NoDeviceRenderOutcome idleOutcome =
        adapter.RenderFrames(idle.data(), static_cast<uint32_t>(idle.size()), kSmallFrames);
    if (!idleOutcome.ok || idleOutcome.framesRendered != 0)
    {
        std::fprintf(stderr, "RT2AudioProbe FAIL: idle render: ok=%d rendered=%u error=%s\n",
                     idleOutcome.ok ? 1 : 0, idleOutcome.framesRendered,
                     idleOutcome.error != nullptr ? idleOutcome.error : "none");
        return 1;
    }
    std::printf("RT2AudioProbe: idle pump reports 0 frames\n");

    if (!adapter.StartProbeTone(errorText))
    {
        std::fprintf(stderr, "RT2AudioProbe FAIL: start probe tone: %s\n",
                     errorText.empty() ? "unknown" : errorText.c_str());
        return 1;
    }

    // Exact-count small render through caller-owned stereo storage.
    std::vector<float> small(static_cast<size_t>(kSmallFrames) * kNoDeviceChannels, 0.0f);
    const NoDeviceRenderOutcome smallOutcome =
        adapter.RenderFrames(small.data(), static_cast<uint32_t>(small.size()), kSmallFrames);
    if (!smallOutcome.ok || smallOutcome.framesRendered != kSmallFrames)
    {
        std::fprintf(stderr, "RT2AudioProbe FAIL: 480-frame render: ok=%d rendered=%u error=%s\n",
                     smallOutcome.ok ? 1 : 0, smallOutcome.framesRendered,
                     smallOutcome.error != nullptr ? smallOutcome.error : "none");
        return 1;
    }
    if (MaxAbs(small) < 0.25f)
        return Fail("480-frame render carries no tone energy");
    std::printf("RT2AudioProbe: rendered %u/%u frames (peak %.3f)\n",
                smallOutcome.framesRendered, kSmallFrames,
                static_cast<double>(MaxAbs(small)));

    // Exact-count maximum render (kNoDeviceMaxFramesPerRender per contract).
    std::vector<float> large(
        static_cast<size_t>(kNoDeviceMaxFramesPerRender) * kNoDeviceChannels, 0.0f);
    const NoDeviceRenderOutcome largeOutcome = adapter.RenderFrames(
        large.data(), static_cast<uint32_t>(large.size()), kNoDeviceMaxFramesPerRender);
    if (!largeOutcome.ok || largeOutcome.framesRendered != kNoDeviceMaxFramesPerRender)
        return Fail("4096-frame no-device render did not report 4096 frames");
    std::printf("RT2AudioProbe: rendered %u/%u frames\n",
                largeOutcome.framesRendered, kNoDeviceMaxFramesPerRender);

    // Pump the 1-second tone to completion. The crossing read is padded
    // with silence to the requested size (miniaudio device-underrun
    // behavior), so the total may exceed the tone length by less than one
    // chunk; everything past the tone end must be silent. The reported
    // counts must still be exact and the voice must report at-end.
    constexpr uint32_t kToneFrames = 48000;
    uint32_t totalRendered = kSmallFrames + kNoDeviceMaxFramesPerRender;
    std::vector<float> chunk(
        static_cast<size_t>(kNoDeviceMaxFramesPerRender) * kNoDeviceChannels, 0.0f);
    bool crossedEnd = false;
    std::vector<float> crossingChunk;
    uint32_t crossingValidFrames = 0;
    uint32_t crossingChunkFrames = 0;
    for (;;)
    {
        const NoDeviceRenderOutcome outcome = adapter.RenderFrames(
            chunk.data(), static_cast<uint32_t>(chunk.size()), kNoDeviceMaxFramesPerRender);
        if (!outcome.ok)
            return Fail("completion pump render failed");
        if (!crossedEnd && totalRendered + outcome.framesRendered >= kToneFrames)
        {
            crossedEnd = true;
            crossingChunkFrames = outcome.framesRendered;
            crossingValidFrames = kToneFrames - totalRendered;
            crossingChunk.assign(chunk.begin(),
                                 chunk.begin() + static_cast<size_t>(outcome.framesRendered) *
                                                     kNoDeviceChannels);
        }
        totalRendered += outcome.framesRendered;
        if (adapter.IsProbeToneAtEnd())
            break;
        if (outcome.framesRendered == 0)
            return Fail("completion pump stalled before at-end");
        if (totalRendered > kToneFrames + kNoDeviceMaxFramesPerRender)
            return Fail("completion pump overran the tone length");
    }
    if (!crossedEnd)
        return Fail("completion pump never reached the tone end");
    if (totalRendered < kToneFrames || totalRendered >= kToneFrames + kNoDeviceMaxFramesPerRender)
    {
        std::fprintf(stderr, "RT2AudioProbe FAIL: tone total %u, want [%u, %u)\n",
                     totalRendered, kToneFrames, kToneFrames + kNoDeviceMaxFramesPerRender);
        return 1;
    }
    for (uint32_t frame = crossingValidFrames; frame < crossingChunkFrames; ++frame)
    {
        for (uint32_t ch = 0; ch < kNoDeviceChannels; ++ch)
        {
            if (std::fabs(crossingChunk[static_cast<size_t>(frame) * kNoDeviceChannels + ch]) > 1e-6f)
                return Fail("tone tail padding is not silent");
        }
    }
    std::printf("RT2AudioProbe: tone completed at %u frames (%u silent tail frames)\n",
                totalRendered, crossingChunkFrames - crossingValidFrames);

    // Loud refusal checks on the probe path itself: null buffer and
    // out-of-range requests must fail without touching caller storage.
    const NoDeviceRenderOutcome nullOutcome =
        adapter.RenderFrames(nullptr, 0, kSmallFrames);
    if (nullOutcome.ok || nullOutcome.error != MiniaudioNoDeviceAdapter::kRenderErrorNullBuffer)
        return Fail("null buffer did not refuse loudly");
    const NoDeviceRenderOutcome zeroOutcome =
        adapter.RenderFrames(small.data(), static_cast<uint32_t>(small.size()), 0);
    if (zeroOutcome.ok ||
        zeroOutcome.error != MiniaudioNoDeviceAdapter::kRenderErrorRequestOutOfRange)
        return Fail("zero-frame request did not refuse loudly");

    adapter.Shutdown();
    if (adapter.IsInitialized())
        return Fail("shutdown adapter still reports initialized");
    // Post-shutdown render must refuse loudly, never touch the device path.
    const NoDeviceRenderOutcome deadOutcome =
        adapter.RenderFrames(small.data(), static_cast<uint32_t>(small.size()), kSmallFrames);
    if (deadOutcome.ok ||
        deadOutcome.error != MiniaudioNoDeviceAdapter::kRenderErrorNotInitialized)
        return Fail("post-shutdown render did not refuse loudly");

    std::printf("RT2AudioProbe: PASS no-device float32 stereo 48kHz (%s)\n",
                kPinnedMiniaudioVersionString);
    return 0;
}
