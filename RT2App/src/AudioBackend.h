#pragma once

#ifndef RT2_AUDIO_BACKEND_H
#define RT2_AUDIO_BACKEND_H

#include "AudioComponents.h"
#include "core/Error.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// ============================================================================
// AudioBackend — CPU-only backend boundary (audio integration, A3 policy
// core).
//
// CPU-only: standard library + AudioComponents + core/Error only. No
// miniaudio, device, Vulkan, ImGui, or Walnut types. Links into RT2Tests and
// RT2SliceRunner unchanged. No `ma_*` name may appear in this header (READY
// plan, Backend boundary).
//
// Identity (READY plan, review finding 1):
//   - AudioWorld owns deterministic AudioWorldVoiceHandle slots and
//     generations (cap and steal policy).
//   - IAudioBackend owns opaque BackendVoiceToken values and no ECS policy.
//   - The per-slot mapping (generation -> backend token) lives in AudioWorld
//     and is removed only after explicit stop or a drained completion whose
//     token matches the slot's current generation. A stale script handle or
//     backend completion therefore cannot control or reclaim a recycled slot.
//   - A voice started while its session is paused carries initialPaused so
//     Step can never observe a briefly audible voice.
//
// A4 seams (deliberately declared but not implemented here):
//   - DecodeClip / RegisterDecodedGeneration / ReleaseDecodedGeneration and
//     the immutable DecodedAudioGeneration PCM they traffic in. A3 tests feed
//     synthetic generations through the recording fake; A4 wires the real
//     miniaudio decoder and bounded cache behind the same interface.
//   - RenderNoDeviceFrames (production 48 kHz stereo no-device rendering).
// ============================================================================

namespace rt2::audio {

enum class AudioOwnerKind : uint8_t
{
    Runtime = 0,
    Preview = 1,
};

struct AudioSessionId
{
    uint64_t value = 0;

    bool IsValid() const noexcept { return value != 0; }
    bool operator==(const AudioSessionId& o) const noexcept { return value == o.value; }
    bool operator!=(const AudioSessionId& o) const noexcept { return value != o.value; }
};

// Deterministic world-side voice identity. `slot` indexes the world's fixed
// voice array; `generation` is bumped every time the slot is recycled. A
// handle names a live voice only when both match the slot's current state.
struct AudioWorldVoiceHandle
{
    uint32_t slot = 0;
    uint32_t generation = 0;

    bool operator==(const AudioWorldVoiceHandle& o) const noexcept
    { return slot == o.slot && generation == o.generation; }
    bool operator!=(const AudioWorldVoiceHandle& o) const noexcept
    { return !(*this == o); }
};

// Opaque backend-side voice identity. The world never interprets these bits;
// it only maps them per slot and matches completions against the mapping.
struct BackendVoiceToken
{
    uint64_t opaque = 0;

    bool IsValid() const noexcept { return opaque != 0; }
    bool operator==(const BackendVoiceToken& o) const noexcept { return opaque == o.opaque; }
    bool operator!=(const BackendVoiceToken& o) const noexcept { return opaque != o.opaque; }
};

// Opaque backend-side clip identity for one immutable decoded generation.
// A4 keys these by (asset ID, canonical path, content fingerprint, decode
// format); A3 tests use synthetic generations through the fake.
struct BackendClipHandle
{
    uint64_t opaque = 0;

    bool IsValid() const noexcept { return opaque != 0; }
    bool operator==(const BackendClipHandle& o) const noexcept { return opaque == o.opaque; }
    bool operator!=(const BackendClipHandle& o) const noexcept { return opaque != o.opaque; }
};

// Immutable decoded PCM generation. Shared ownership: the backend retains
// one reference while any backend voice reads it, and existing world voices
// retain theirs, so old and new generations of one path overlap safely.
struct DecodedAudioGeneration
{
    std::vector<float> pcmInterleaved; // float32 interleaved frames
    uint32_t channels = 0;             // 1 = mono (required for spatial), 2 = stereo
    uint32_t sampleRate = 0;
    uint32_t frameCount = 0;           // pcmInterleaved.size() / channels
};

// Immutable resolved clip bytes handed to the decoder. The key stands in
// for the A4 (asset ID, canonical path, content fingerprint, decode format)
// tuple; the bytes are the exact content to decode, never a path that can
// change underneath the call. A3 tests synthesize both; A4 builds them from
// AudioClipAssetProvider snapshots.
struct AudioClipBytes
{
    std::string clipKey;
    std::shared_ptr<const std::vector<char>> bytes;
};
// Listener pose injected by the host from the actual rendered camera. There
// is deliberately no persisted listener component (READY plan).
struct AudioListenerPose
{
    float position[3] = { 0.0f, 0.0f, 0.0f };
    float forward[3] = { 0.0f, 0.0f, -1.0f };
    float up[3] = { 0.0f, 1.0f, 0.0f };
};

// Backend start descriptor. Bus/Master gains are applied exactly once by the
// backend sound groups; they are NOT baked into initialLeft/Right.
struct BackendVoiceStart
{
    AudioSessionId session;
    AudioOwnerKind owner = AudioOwnerKind::Runtime;
    bool loop = false;
    bool initialPaused = false;
    AudioBus bus = AudioBus::Effects;
    float initialLeft = 0.70710678f;
    float initialRight = 0.70710678f;
    float pitch = 1.0f;
};

// Explicit per-voice channel mix. RT2 owns the equal-power law; the backend
// must not re-derive gains from a scalar pan.
struct BackendVoiceMix
{
    float left = 0.70710678f;
    float right = 0.70710678f;
    float pitch = 1.0f;
};

enum class BackendCompletionReason : uint8_t
{
    Completed = 0, // reached the end of non-looping content
    Failed = 1,    // terminal device/decode failure with a typed error
    Stopped = 2,   // backend-initiated stop (world-initiated stops reclaim directly)
};

// Backend completion event. Carries session ID, backend token, terminal
// reason, and an optional typed error; never an ECS pointer.
struct BackendVoiceCompletion
{
    AudioSessionId session;
    BackendVoiceToken token;
    BackendCompletionReason reason = BackendCompletionReason::Completed;
    core::Error error;
};

// Caller-owned no-device PCM output. A4 implements the production path;
// the A3 fake refuses it loudly (A4 seam, not silent success).
struct AudioPcmWriteBuffer
{
    float* data = nullptr;
    size_t sampleCapacity = 0; // float32 samples, >= requestedFrames * 2
};

struct AudioBackendStatus
{
    bool productionNoDevice = false; // true only for the real fallback engine
    std::string detail;              // human-readable mode/reason
};

class IAudioBackend
{
public:
    virtual ~IAudioBackend() = default;

    virtual core::Result<std::shared_ptr<const DecodedAudioGeneration>> DecodeClip(
        const AudioClipBytes& clip) = 0;
    virtual core::Result<BackendClipHandle> RegisterDecodedGeneration(
        std::shared_ptr<const DecodedAudioGeneration> generation) = 0;
    virtual bool ReleaseDecodedGeneration(BackendClipHandle handle, core::Error& outError) = 0;

    virtual core::Result<BackendVoiceToken> StartVoice(
        BackendClipHandle clip, const BackendVoiceStart& start) = 0;
    virtual bool StopVoice(BackendVoiceToken token, core::Error& outError) = 0;
    virtual bool PauseVoice(BackendVoiceToken token, bool paused, core::Error& outError) = 0;
    virtual bool SetVoiceMix(BackendVoiceToken token, const BackendVoiceMix& mix,
                             core::Error& outError) = 0;

    // Non-blocking, main-thread completion drain. Tokens are matched against
    // the world's current per-slot mapping before reclamation.
    virtual core::Result<std::vector<BackendVoiceCompletion>> DrainCompletions(
        AudioSessionId session) = 0;

    virtual bool SetSessionPaused(AudioSessionId session, bool paused, core::Error& outError) = 0;
    virtual bool SetBusGain(AudioBus bus, float gain, core::Error& outError) = 0;
    virtual bool StopSessionVoices(AudioSessionId session, core::Error& outError) = 0;

    virtual core::Result<uint32_t> RenderNoDeviceFrames(
        AudioPcmWriteBuffer interleavedStereo, uint32_t requestedFrames) = 0;

    virtual AudioBackendStatus Status() const = 0;
};

} // namespace rt2::audio

#endif // RT2_AUDIO_BACKEND_H
