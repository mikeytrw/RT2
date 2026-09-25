// ProductionAudioBackend.h
//
// A4: production IAudioBackend + IAudioClipProvider over pinned miniaudio.
//
// One ma_engine (hardware device, or diagnosed 48 kHz stereo no-device
// fallback), one sound group per logical bus (Master/Music/Effects/UI),
// stable heap-owned voices with opaque tokens, completion polling, session
// pause/stop, and caller-buffer no-device rendering through the real
// ma_engine_read_pcm_frames() path.
//
// This header exposes no ma_* type, so including it never imports miniaudio
// into a translation unit. Only ProductionAudioBackend.cpp includes
// vendor/miniaudio/miniaudio.h, and only the RT2AudioBackend project
// compiles that translation unit (A1 build boundary, review finding 7).
//
// CPU isolation: RT2Tests and RT2SliceRunner must never include this header
// (it is not on their include path and they do not link RT2AudioBackend;
// enforced by run_audio_a1_gates.ps1). They use RecordingFakeAudioBackend.

#pragma once

#include "AudioBackend.h"
#include "AudioClipProvider.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

namespace rt2::audio::backend
{

// Production backend configuration. All fields have READY-plan defaults.
struct ProductionBackendConfig
{
    // Skip the hardware device attempt and open the no-device engine
    // directly. RT2AudioProbe always sets this for deterministic oracles;
    // the editor leaves it false so real hardware is used when present.
    bool forceNoDevice = false;
    // Immutable decoded-generation cache budget (READY plan: 256 MiB
    // default, configurable at startup). The budget counts every live PCM
    // object the backend holds — cache entries and registered handles
    // alike. Eviction is deterministic LRU among entries with no owners
    // outside the cache (no live voices, no registrations, no world or
    // provider holders); pinned generations are never evicted.
    size_t decodedCacheBudgetBytes = 256ULL * 1024ULL * 1024ULL;
    // Hard backend voice cap. StartVoice beyond the cap fails loudly;
    // ReplaceVoice commits within one capacity unit and is exempt.
    // Mirrors the AudioWorld default (64) so a legal world steal can
    // always commit.
    size_t maxVoices = 64;
};

// Resolves the immutable source bytes behind a clip key. The production
// host (A5+) wires this to the A2 AudioClipAssetProvider snapshot;
// RT2AudioProbe wires it to its fixture map. The backend never accepts a
// generation solely from mtime/size: DecodeClip fingerprints the exact
// bytes it decodes.
using ClipByteResolver = std::function<
    core::Result<rt2::audio::AudioClipBytes>(const std::string& clipKey)>;

// Packed RT2 stereo-gain publication. One instance per voice: SetVoiceMix
// performs one release store on the main thread; the audio-thread gain
// stage performs one acquire load once per render block. A block therefore
// observes either the complete old pair or the complete new pair, never
// one channel from each (READY plan, check 19).
//
// Layout: low 32 bits = left IEEE-754 bits, high 32 bits = right bits.
inline uint64_t PackStereoGain(float left, float right) noexcept
{
    uint32_t l = 0;
    uint32_t r = 0;
    static_assert(sizeof(float) == sizeof(uint32_t), "float must be 32 bits");
    std::memcpy(&l, &left, sizeof(float));
    std::memcpy(&r, &right, sizeof(float));
    return (static_cast<uint64_t>(r) << 32) | l;
}

inline void UnpackStereoGain(uint64_t packed, float& left, float& right) noexcept
{
    uint32_t l = static_cast<uint32_t>(packed & 0xFFFFFFFFULL);
    uint32_t r = static_cast<uint32_t>((packed >> 32) & 0xFFFFFFFFULL);
    std::memcpy(&left, &l, sizeof(float));
    std::memcpy(&right, &r, sizeof(float));
}

class ProductionAudioBackend final
    : public rt2::audio::IAudioBackend
    , public rt2::audio::IAudioClipProvider
{
public:
    ProductionAudioBackend();
    ~ProductionAudioBackend() override;

    ProductionAudioBackend(const ProductionAudioBackend&) = delete;
    ProductionAudioBackend& operator=(const ProductionAudioBackend&) = delete;
    ProductionAudioBackend(ProductionAudioBackend&&) = delete;
    ProductionAudioBackend& operator=(ProductionAudioBackend&&) = delete;

    // Opens the engine: hardware device first (unless forceNoDevice), then
    // diagnosed 48 kHz stereo no-device fallback on failure. Safe to retry
    // after failure; calling twice without Shutdown is a no-op success.
    bool Initialize(const ProductionBackendConfig& config, std::string& errorText);
    bool IsInitialized() const;
    void Shutdown();

    void SetClipByteResolver(ClipByteResolver resolver);

    // --- Test-only fault hooks (RT2AudioProbe-driven; inert unless armed) ---
    //
    // Each hook is single-shot: the Initialize/Render that observes it
    // consumes it. They exist so the probe can force paths no real input
    // reaches deterministically (hardware-open failure, per-group init
    // failure, short/hard render results). Production hosts never arm them.
    // Fires the hardware-open failure once on the next Initialize that
    // does not force no-device: the engine reopens diagnosed no-device.
    void TestHook_FailHardwareOpenOnce();
    // Fails one mixer-group init ("master", "music", "effects", or "ui") on
    // the next Initialize; "" disarms. Only owned groups are unwound.
    void TestHook_FailGroupInit(const std::string& group);
    enum class TestRenderFault : uint8_t
    {
        None = 0,
        // Next RenderNoDeviceFrames performs the real engine render but
        // reports only the first ShortFrames() frames (successful prefix).
        ShortOnce = 1,
        // Next RenderNoDeviceFrames returns the typed engine-failure error
        // and publishes nothing into caller storage.
        FailOnce = 2,
    };
    void TestHook_SetRenderFault(TestRenderFault fault, uint32_t shortFrames = 0);

    // --- IAudioClipProvider (immutable generations, fingerprint LRU) ---
    core::Result<std::shared_ptr<const rt2::audio::DecodedAudioGeneration>> FetchDecodedGeneration(
        const std::string& clipKey) override;

    // --- IAudioBackend ---
    core::Result<std::shared_ptr<const rt2::audio::DecodedAudioGeneration>> DecodeClip(
        const rt2::audio::AudioClipBytes& clip) override;
    core::Result<rt2::audio::BackendClipHandle> RegisterDecodedGeneration(
        std::shared_ptr<const rt2::audio::DecodedAudioGeneration> generation) override;
    bool ReleaseDecodedGeneration(rt2::audio::BackendClipHandle handle,
                                  core::Error& outError) override;
    core::Result<rt2::audio::BackendVoiceToken> StartVoice(
        rt2::audio::BackendClipHandle clip, const rt2::audio::BackendVoiceStart& start) override;
    core::Result<rt2::audio::BackendVoiceToken> ReplaceVoice(
        rt2::audio::BackendVoiceToken victim, rt2::audio::BackendClipHandle clip,
        const rt2::audio::BackendVoiceStart& start) override;
    bool StopVoice(rt2::audio::BackendVoiceToken token, core::Error& outError) override;
    bool PauseVoice(rt2::audio::BackendVoiceToken token, bool paused,
                    core::Error& outError) override;
    bool SetVoiceMix(rt2::audio::BackendVoiceToken token,
                     const rt2::audio::BackendVoiceMix& mix,
                     core::Error& outError) override;
    core::Result<std::vector<rt2::audio::BackendVoiceCompletion>> DrainCompletions(
        rt2::audio::AudioSessionId session) override;
    bool SetSessionPaused(rt2::audio::AudioSessionId session, bool paused,
                          core::Error& outError) override;
    bool SetBusGain(AudioBus bus, float gain,
                    core::Error& outError) override;
    bool StopSessionVoices(rt2::audio::AudioSessionId session, core::Error& outError) override;
    core::Result<uint32_t> RenderNoDeviceFrames(
        rt2::audio::AudioPcmWriteBuffer interleavedStereo, uint32_t requestedFrames) override;
    rt2::audio::AudioBackendStatus Status() const override;

    // --- A4 cache key helpers ---
    //
    // The AudioWorld clipKey stands in for the (effective asset ID,
    // canonical path, content fingerprint, decode format) tuple. The host
    // builds it with BuildClipKey; the backend keys its decoded cache on
    // the whole string and verifies the fingerprint embedded in it against
    // the FNV-1a of the exact bytes decoded. A same-size/same-mtime byte
    // rewrite therefore yields a new key and a distinct immutable
    // generation that overlaps the old one until its voices complete.
    static std::string BuildClipKey(const std::string& effectiveAssetId,
                                    const std::string& canonicalPath,
                                    uint64_t contentFingerprint,
                                    const std::string& decodeFormat);
    // FNV-1a/64 over raw bytes. Same algorithm as
    // PhysicsCollisionGeometry::FnV1a64 (same offset basis and prime), so
    // backend fingerprints agree with the A2 provider's by construction.
    static uint64_t FingerprintBytes(const void* data, size_t bytes) noexcept;

    // True when the packed stereo-gain atomic is lock-free on this target.
    // The Windows x64 production target requires this; Initialize refuses
    // to open the engine when it is false.
    static bool StereoGainAtomicIsLockFree();

    // Applies one already-published L/R pair to `frames` mono or stereo
    // float32 input frames, writing interleaved stereo output. This is the
    // exact block function the audio-thread gain stage runs after its one
    // acquire load per block (no allocation, no I/O, no locking); the
    // torn-pair stress oracle in RT2AudioProbe drives it directly with
    // asymmetric pairs. srcChannels is 1 (mono expanded) or 2.
    static void ProcessStereoGainBlock(float left, float right,
                                       const float* input, uint32_t srcChannels,
                                       float* stereoOutput, uint32_t frames) noexcept;

    // --- Observation (probe/diagnostics; main thread only) ---
    size_t DecodedCacheEntryCount() const;
    size_t DecodedCacheResidentBytes() const;
    size_t LiveVoiceCount() const;
    size_t PeakLiveVoices() const;
    // Number of live voices referencing `clipKey` (zero-reference LRU
    // observability for the probe).
    size_t ActiveVoicesForKey(const std::string& clipKey) const;

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

} // namespace rt2::audio::backend
