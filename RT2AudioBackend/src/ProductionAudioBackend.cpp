// ProductionAudioBackend.cpp
//
// A4 production miniaudio backend + immutable decoded-generation cache.
//
// This is the ONLY translation unit in the solution (besides vendored
// miniaudio.c and the A1 MiniaudioNoDeviceAdapter.cpp) that includes
// miniaudio.h. It must stay inside top-level RT2AudioBackend: RT2App's
// broad src/**.cpp and vendor/**.c globs must never compile it twice
// (review finding 7). RT2Tests and RT2SliceRunner must never compile or
// link it (run_audio_a1_gates.ps1).
//
// Design (READY plan, Backend boundary + Clip asset path):
//   - One ma_engine: hardware device first, diagnosed 48 kHz stereo
//     no-device fallback on failure (forceNoDevice for the probe).
//   - Master group on the endpoint; Music/Effects/UI groups under Master.
//     Bus/Master gains are applied exactly once by these groups.
//   - Each voice is heap-owned (stable address; miniaudio objects must not
//     move) and reads its immutable generation through a custom RT2 gain
//     data source. That source publishes its L/R pair through one packed
//     std::atomic<uint64_t> (release store on the main thread, one acquire
//     load per onRead block on the audio thread). Pitch uses miniaudio's
//     thread-safe sound-pitch setter on the main thread; RT2 callback code
//     never touches pitch.
//   - The gain onRead performs no allocation, filesystem I/O, engine
//     callback, logging, or contended locking: pure arithmetic over the
//     immutable PCM plus one atomic load. All graph/voice mutation happens
//     on the main thread.
//   - Decode consumes immutable resolved bytes via ma_decoder_init_memory
//     (WAV/FLAC/MP3 auto-detected); miniaudio's filename-keyed resource
//     cache is never used for clip identity.
//   - The decoded cache is keyed by the whole AudioWorld clipKey, which the
//     host builds as (asset ID, canonical path, fingerprint, decode
//     format). DecodeClip verifies the FNV-1a of the exact bytes against
//     the fingerprint embedded in the key, so a same-size/same-mtime byte
//     rewrite yields a distinct overlapping generation. Eviction is
//     deterministic LRU among zero-voice-reference generations under the
//     configured budget (256 MiB default); over-budget inserts fail with
//     Error::Io naming the key and byte counts, before any voice starts.
//   - ReplaceVoice prepares the replacement fully (sound initialized and
//     started silent at zero gain) BEFORE the victim is touched, then stops
//     the victim and publishes the real gains: one capacity-unit commit,
//     victim preserved on any preparation failure, no transient extra
//     audible voice. Reentrant lifecycle is the world's job; the backend
//     documents that callbacks are not issued (all completion observation
//     is_poll based) so the world-side recheck sees stable state.

#include "ProductionAudioBackend.h"
#include "AudioBackendPin.h"

#include "miniaudio.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rt2::audio::backend
{
namespace
{

constexpr const char* kDecodeFormat = "f32le";
constexpr size_t kMaxDecodeBytes = 512ULL * 1024ULL * 1024ULL;

uint64_t FingerprintLocal(const void* data, size_t bytes) noexcept
{
    // Must match PhysicsCollisionGeometry::FnV1a64 byte-for-byte (same
    // offset basis and prime). Empty input yields the bare basis, matching
    // the A2 provider's empty-file rule.
    const auto* p = static_cast<const unsigned char*>(data);
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < bytes; ++i)
    {
        h ^= static_cast<uint64_t>(p[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

// Parses keys built by BuildClipKey ("id|path|fp:<hex>|fmt:<format>").
// Returns false when the shape is wrong; the fingerprint/format outputs
// are valid only on true.
bool ParseClipKey(const std::string& key, uint64_t& fingerprintOut, std::string& formatOut)
{
    const std::string fpTag = "|fp:";
    const std::string fmtTag = "|fmt:";
    const size_t fpPos = key.rfind(fpTag);
    const size_t fmtPos = key.rfind(fmtTag);
    if (fpPos == std::string::npos || fmtPos == std::string::npos || fmtPos <= fpPos)
        return false;
    const std::string fpHex = key.substr(fpPos + fpTag.size(), fmtPos - (fpPos + fpTag.size()));
    if (fpHex.empty() || fpHex.size() > 16)
        return false;
    uint64_t fp = 0;
    for (char c : fpHex)
    {
        fp <<= 4;
        if (c >= '0' && c <= '9')
            fp |= static_cast<uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f')
            fp |= static_cast<uint64_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            fp |= static_cast<uint64_t>(c - 'A' + 10);
        else
            return false;
    }
    fingerprintOut = fp;
    formatOut = key.substr(fmtPos + fmtTag.size());
    return !formatOut.empty();
}

bool IsValidGain(float v) noexcept
{
    return std::isfinite(v) && v >= 0.0f;
}

bool IsValidPitch(float v) noexcept
{
    return std::isfinite(v) && v >= 0.25f && v <= 4.0f;
}

bool IsValidBusGain(float v) noexcept
{
    return std::isfinite(v) && v >= 0.0f && v <= 4.0f;
}

core::Error MakeVoiceError(core::Error::Code code, const std::string& path,
                           const std::string& detail)
{
    core::Error error;
    error.code = code;
    error.path = path;
    error.detail = detail;
    return error;
}

uint16_t ReadU16LE(const unsigned char* p) noexcept
{
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t ReadU32LE(const unsigned char* p) noexcept
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Container-advertised length check (finding 2, amended by finding R3).
// WAV and FLAC carry their frame count in the container; a header that
// promises more frames than the bytes deliver is corruption, not a
// shorter clip, and must refuse rather than publish a prefix. A FLAC
// STREAMINFO total of zero means unknown length (valid per RFC 9639) and
// stays decoder-driven, as does MP3, which carries no trustworthy total.
struct ContainerCheck
{
    bool ok = true;            // false: malformed container (refuse with detail)
    bool enforce = false;      // true: frames holds the advertised count
    uint64_t frames = 0;
    const char* container = "";
    std::string detail;
};

ContainerCheck CheckWavLength(const unsigned char* bytes, size_t size)
{
    ContainerCheck out;
    out.container = "WAV";
    if (size < 12 || std::memcmp(bytes, "RIFF", 4) != 0 ||
        std::memcmp(bytes + 8, "WAVE", 4) != 0)
    {
        out.ok = false;
        out.detail = "malformed WAV header";
        return out;
    }
    const uint32_t riffSize = ReadU32LE(bytes + 4);
    if (static_cast<uint64_t>(riffSize) + 8 != size)
    {
        out.ok = false;
        out.detail = "WAV header size does not match the delivered bytes";
        return out;
    }
    bool fmtFound = false;
    bool dataFound = false;
    uint32_t dataBytes = 0;
    uint32_t blockAlign = 0;
    size_t pos = 12;
    while (pos + 8 <= size)
    {
        char id[5] = { 0, 0, 0, 0, 0 };
        std::memcpy(id, bytes + pos, 4);
        const uint32_t chunkSize = ReadU32LE(bytes + pos + 4);
        const size_t chunkStart = pos + 8;
        if (static_cast<uint64_t>(chunkStart) + chunkSize > size)
        {
            out.ok = false;
            out.detail = std::string("WAV chunk '") + id + "' overruns the delivered bytes";
            return out;
        }
        if (std::memcmp(id, "fmt ", 4) == 0)
        {
            if (chunkSize < 16)
            {
                out.ok = false;
                out.detail = "WAV fmt chunk is too small";
                return out;
            }
            const uint32_t channels = ReadU16LE(bytes + chunkStart + 2);
            const uint32_t sampleRate = ReadU32LE(bytes + chunkStart + 4);
            const uint32_t align = ReadU16LE(bytes + chunkStart + 12);
            const uint32_t bits = ReadU16LE(bytes + chunkStart + 14);
            if (channels != 1 && channels != 2)
            {
                out.ok = false;
                out.detail = "WAV fmt chunk names an unsupported channel count";
                return out;
            }
            if (sampleRate == 0 || align == 0 ||
                (bits != 8 && bits != 16 && bits != 24 && bits != 32) ||
                align != channels * bits / 8)
            {
                out.ok = false;
                out.detail = "WAV fmt chunk is inconsistent";
                return out;
            }
            blockAlign = align;
            fmtFound = true;
        }
        else if (std::memcmp(id, "data", 4) == 0 && !dataFound)
        {
            dataBytes = chunkSize;
            dataFound = true;
        }
        pos = chunkStart + chunkSize + (chunkSize & 1);
    }
    if (!fmtFound || !dataFound)
    {
        out.ok = false;
        out.detail = "WAV is missing its fmt or data chunk";
        return out;
    }
    if (dataBytes % blockAlign != 0 || dataBytes == 0)
    {
        out.ok = false;
        out.detail = "WAV data chunk is not a whole number of frames";
        return out;
    }
    out.enforce = true;
    out.frames = dataBytes / blockAlign;
    return out;
}

ContainerCheck CheckFlacLength(const unsigned char* bytes, size_t size)
{
    ContainerCheck out;
    out.container = "FLAC";
    if (size < 4 || std::memcmp(bytes, "fLaC", 4) != 0)
    {
        out.ok = false;
        out.detail = "malformed FLAC marker";
        return out;
    }
    size_t pos = 4;
    bool first = true;
    for (;;)
    {
        if (pos + 4 > size)
        {
            out.ok = false;
            out.detail = "FLAC metadata overruns the delivered bytes";
            return out;
        }
        const unsigned char header = bytes[pos];
        const unsigned char type = header & 0x7F;
        const bool last = (header & 0x80) != 0;
        const uint32_t length = (static_cast<uint32_t>(bytes[pos + 1]) << 16) |
                                (static_cast<uint32_t>(bytes[pos + 2]) << 8) |
                                static_cast<uint32_t>(bytes[pos + 3]);
        if (static_cast<uint64_t>(pos) + 4 + length > size)
        {
            out.ok = false;
            out.detail = "FLAC metadata block overruns the delivered bytes";
            return out;
        }
        if (first && type != 0)
        {
            out.ok = false;
            out.detail = "FLAC does not start with STREAMINFO";
            return out;
        }
        first = false;
        if (type == 0)
        {
            if (length != 34)
            {
                out.ok = false;
                out.detail = "FLAC STREAMINFO has an unexpected size";
                return out;
            }
            const unsigned char* s = bytes + pos + 4;
            const uint32_t channels = ((s[12] >> 1) & 7) + 1;
            if (channels != 1 && channels != 2)
            {
                out.ok = false;
                out.detail = "FLAC STREAMINFO names an unsupported channel count";
                return out;
            }
            const uint64_t total =
                (static_cast<uint64_t>(s[13] & 0x0F) << 32) |
                (static_cast<uint64_t>(s[14]) << 24) |
                (static_cast<uint64_t>(s[15]) << 16) |
                (static_cast<uint64_t>(s[16]) << 8) | static_cast<uint64_t>(s[17]);
            if (total == 0)
            {
                // Zero means the length is unknown, which is valid FLAC
                // (RFC 9639 STREAMINFO): the entry stays decoder-driven
                // and only non-end decoder errors can still refuse it.
                return out;
            }
            out.enforce = true;
            out.frames = total;
            return out;
        }
        if (last)
            break;
        pos += 4 + length;
    }
    out.ok = false;
    out.detail = "FLAC has no STREAMINFO block";
    return out;
}

ContainerCheck CheckContainerLength(const void* data, size_t size)
{
    ContainerCheck out;
    if (data == nullptr || size < 4)
        return out; // too small to identify: decoder-driven
    const auto* bytes = static_cast<const unsigned char*>(data);
    if (size >= 12 && std::memcmp(bytes, "RIFF", 4) == 0 &&
        std::memcmp(bytes + 8, "WAVE", 4) == 0)
        return CheckWavLength(bytes, size);
    if (std::memcmp(bytes, "fLaC", 4) == 0)
        return CheckFlacLength(bytes, size);
    return out; // MP3 and unidentified content stay decoder-driven
}

} // namespace

// ---------------------------------------------------------------------------
// GainVoiceSource: per-voice custom data source applying the packed L/R pair.
//
// ma_data_source_base must be the first member so a ma_data_source* can be
// reinterpreted to the owning struct. All PCM is immutable (owned by the
// generation shared_ptr held by Voice); the cursor is the only mutable
// audio-thread state besides the packed gains.

struct GainVoiceSource
{
    ma_data_source_base base{};
    std::atomic<uint64_t> packed{ PackStereoGain(0.0f, 0.0f) };
    const float* pcm = nullptr; // interleaved f32, immutable, non-owning
    uint64_t totalFrames = 0;
    uint32_t srcChannels = 0; // 1 = mono (expanded to stereo), 2 = stereo
    uint32_t sampleRate = 48000;
    std::atomic<uint64_t> cursor{ 0 };
    bool loop = false;
};

namespace
{

GainVoiceSource* ToGainSource(ma_data_source* ds) noexcept
{
    return reinterpret_cast<GainVoiceSource*>(ds);
}

} // namespace

void ProductionAudioBackend::ProcessStereoGainBlock(
    float left, float right, const float* input, uint32_t srcChannels,
    float* stereoOutput, uint32_t frames) noexcept
{
    if (srcChannels == 1)
    {
        for (uint32_t i = 0; i < frames; ++i)
        {
            const float m = input[i];
            stereoOutput[i * 2 + 0] = m * left;
            stereoOutput[i * 2 + 1] = m * right;
        }
    }
    else
    {
        for (uint32_t i = 0; i < frames; ++i)
        {
            stereoOutput[i * 2 + 0] = input[i * 2 + 0] * left;
            stereoOutput[i * 2 + 1] = input[i * 2 + 1] * right;
        }
    }
}

namespace
{

ma_result GainOnRead(ma_data_source* pDataSource, void* pFramesOut,
                     ma_uint64 frameCount, ma_uint64* pFramesRead)
{
    GainVoiceSource* self = ToGainSource(pDataSource);
    // One acquire load per render block: the whole block observes either
    // the complete old pair or the complete new pair (check 19).
    float left = 0.0f;
    float right = 0.0f;
    UnpackStereoGain(self->packed.load(std::memory_order_acquire), left, right);

    uint64_t cursor = self->cursor.load(std::memory_order_relaxed);
    uint64_t done = 0;
    if (pFramesOut == nullptr)
    {
        // Forward-seek contract: advance without producing output.
        uint64_t target = cursor + frameCount;
        if (self->loop && self->totalFrames > 0)
            target %= self->totalFrames;
        else if (target > self->totalFrames)
            target = self->totalFrames;
        self->cursor.store(target, std::memory_order_relaxed);
        if (pFramesRead != nullptr)
            *pFramesRead = frameCount;
        return MA_SUCCESS;
    }

    float* out = static_cast<float*>(pFramesOut);
    if (self->totalFrames == 0 || self->pcm == nullptr)
    {
        if (pFramesRead != nullptr)
            *pFramesRead = 0;
        return MA_SUCCESS;
    }
    while (done < frameCount)
    {
        if (cursor >= self->totalFrames)
        {
            if (self->loop && self->totalFrames > 0)
                cursor = 0;
            else
                break;
        }
        // Bounded chunking keeps stack use constant; no allocation.
        uint64_t remaining = frameCount - done;
        uint64_t available = self->totalFrames - cursor;
        uint64_t chunk = remaining < available ? remaining : available;
        // The block helper takes 32-bit counts; engine periods are small,
        // but clamp defensively so an unbounded request can never truncate.
        if (chunk > 4096)
            chunk = 4096;
        if (self->srcChannels == 1)
        {
            ProductionAudioBackend::ProcessStereoGainBlock(
                left, right, self->pcm + cursor, 1, out + done * 2,
                static_cast<uint32_t>(chunk));
        }
        else
        {
            ProductionAudioBackend::ProcessStereoGainBlock(
                left, right, self->pcm + cursor * 2, 2, out + done * 2,
                static_cast<uint32_t>(chunk));
        }
        cursor += chunk;
        done += chunk;
        if (!self->loop && cursor >= self->totalFrames)
            break;
    }
    self->cursor.store(cursor, std::memory_order_relaxed);
    if (pFramesRead != nullptr)
        *pFramesRead = done;
    return MA_SUCCESS;
}

ma_result GainOnSeek(ma_data_source* pDataSource, ma_uint64 frameIndex)
{
    GainVoiceSource* self = ToGainSource(pDataSource);
    uint64_t target = frameIndex;
    if (target > self->totalFrames)
        target = self->totalFrames;
    self->cursor.store(target, std::memory_order_relaxed);
    return MA_SUCCESS;
}

ma_result GainOnGetDataFormat(ma_data_source* pDataSource, ma_format* pFormat,
                              ma_uint32* pChannels, ma_uint32* pSampleRate,
                              ma_channel* pChannelMap, size_t channelMapCap)
{
    GainVoiceSource* self = ToGainSource(pDataSource);
    if (pFormat != nullptr)
        *pFormat = ma_format_f32;
    if (pChannels != nullptr)
        *pChannels = 2; // always stereo interleaved float32
    if (pSampleRate != nullptr)
        *pSampleRate = self->sampleRate;
    (void)pChannelMap;
    (void)channelMapCap;
    return MA_SUCCESS;
}

ma_result GainOnGetCursor(ma_data_source* pDataSource, ma_uint64* pCursor)
{
    if (pCursor == nullptr)
        return MA_INVALID_ARGS;
    *pCursor = ToGainSource(pDataSource)->cursor.load(std::memory_order_relaxed);
    return MA_SUCCESS;
}

ma_result GainOnGetLength(ma_data_source* pDataSource, ma_uint64* pLength)
{
    if (pLength == nullptr)
        return MA_INVALID_ARGS;
    *pLength = ToGainSource(pDataSource)->totalFrames;
    return MA_SUCCESS;
}

ma_result GainOnSetLooping(ma_data_source* pDataSource, ma_bool32 isLooping)
{
    ToGainSource(pDataSource)->loop = (isLooping != MA_FALSE);
    return MA_SUCCESS;
}

const ma_data_source_vtable s_gainVtable = {
    GainOnRead,
    GainOnSeek,
    GainOnGetDataFormat,
    GainOnGetCursor,
    GainOnGetLength,
    GainOnSetLooping,
    0, // flags
};

} // namespace

struct ProductionAudioBackend::Impl
{
    ma_engine engine{};
    bool engineOwned = false;
    bool noDeviceMode = false;
    std::string statusDetail = "uninitialized";

    ma_sound_group masterGroup{};
    bool masterOwned = false;
    ma_sound_group musicGroup{};
    bool musicOwned = false;
    ma_sound_group effectsGroup{};
    bool effectsOwned = false;
    ma_sound_group uiGroup{};
    bool uiOwned = false;
    float busGains[4] = { 1.0f, 1.0f, 1.0f, 1.0f }; // indexed by AudioBus

    ProductionBackendConfig config;
    ClipByteResolver resolver;

    // Test-only fault hooks (see header). Inert unless armed by the probe.
    bool failHardwareOpenOnce = false;
    std::string failGroupInit;
    TestRenderFault renderFault = TestRenderFault::None;
    uint32_t renderShortFrames = 0;

    // No-device reader quiescence (finding 3 follow-up). RenderNoDeviceFrames
    // sets readActive around the engine read; DestroyVoiceObjects waits for
    // an in-flight read to exit before freeing voice objects whose gain
    // stage the reader may be executing. Without this, a concurrent
    // no-device render could read a voice being destroyed (surfacing as
    // audible garbage in the S17b stress). The wait is bounded so a stuck
    // reader can never hang teardown; hardware device reads never set the
    // flag, so hardware teardown behavior is unchanged (stop, detach,
    // destroy — the standard miniaudio lifecycle).
    std::atomic<uint32_t> readActive{ 0 };

    void QuiesceNoDeviceReaders()
    {
        for (int i = 0; i < 100000 && readActive.load(std::memory_order_acquire) != 0; ++i)
            std::this_thread::yield();
    }

    struct Voice
    {
        rt2::audio::BackendVoiceToken token;
        rt2::audio::AudioSessionId session;
        rt2::audio::AudioOwnerKind owner = rt2::audio::AudioOwnerKind::Runtime;
        bool loop = false;
        AudioBus bus = AudioBus::Effects;
        rt2::audio::BackendClipHandle clipHandle;
        std::string clipKey;
        std::shared_ptr<const rt2::audio::DecodedAudioGeneration> generation;
        GainVoiceSource gain;
        bool gainOwned = false;
        ma_sound sound{};
        bool soundOwned = false;
        bool started = false; // sound currently started (audible unless session/paused)
        bool paused = false;  // explicit per-voice pause
        bool failed = false;  // terminal failure pending as a Failed completion
        core::Error failError;
    };

    std::unordered_map<uint64_t, std::unique_ptr<Voice>> voices; // token -> stable heap voice
    std::unordered_map<uint64_t, std::shared_ptr<const rt2::audio::DecodedAudioGeneration>> clips; // handle -> generation
    std::unordered_set<uint64_t> pausedSessions;

    struct CacheEntry
    {
        std::shared_ptr<const rt2::audio::DecodedAudioGeneration> generation;
        uint64_t fingerprint = 0;
        size_t byteSize = 0;
        uint64_t lastUsed = 0;
    };
    std::unordered_map<std::string, CacheEntry> decodeCache; // clipKey -> immutable generation
    std::unordered_map<std::string, size_t> liveKeyVoices;   // clipKey -> live voice count
    uint64_t lruClock = 0;

    static size_t GenerationBytes(
        const std::shared_ptr<const rt2::audio::DecodedAudioGeneration>& generation)
    {
        if (generation == nullptr)
            return 0;
        return generation->pcmInterleaved.size() * sizeof(float);
    }

    // Total live PCM held by the backend: decode-cache entries AND
    // registered handles, deduplicated by object identity (finding 4). A
    // generation pinned by RegisterDecodedGeneration — or retained by
    // AudioWorld, which keeps its fetched generations until Shutdown —
    // counts even with zero live voices, so repeating distinct clips in
    // one session cannot grow memory beyond the configured bound.
    size_t PinnedResidentBytes() const
    {
        std::vector<const rt2::audio::DecodedAudioGeneration*> seen;
        size_t total = 0;
        auto account =
            [&](const std::shared_ptr<const rt2::audio::DecodedAudioGeneration>& generation) {
                if (generation == nullptr)
                    return;
                const auto* raw = generation.get();
                for (const auto* known : seen)
                {
                    if (known == raw)
                        return;
                }
                seen.push_back(raw);
                total += GenerationBytes(generation);
            };
        for (const auto& entry : decodeCache)
            account(entry.second.generation);
        for (const auto& entry : clips)
            account(entry.second);
        return total;
    }

    // An entry is evictable only when this cache entry is its sole owner:
    // no live voices (which hold a reference each), no registered handles,
    // and no AudioWorld/provider holders. Anything else is pinned.
    static bool IsEvictable(const CacheEntry& entry)
    {
        return entry.generation != nullptr && entry.generation.use_count() == 1;
    }

    std::vector<float> scratch; // 4096-frame stereo no-device render area
    std::vector<float> discardBuffer; // ordinary-editor fallback area (kept for parity)

    uint64_t nextTokenId = 1;
    uint64_t nextHandleId = 1;
    size_t peakLiveVoices = 0;
    // Successful ma_sound_start calls. Test observability for the paused
    // never-start guarantee (finding R1): a paused ReplaceVoice must not
    // add to this count.
    uint64_t startCalls = 0;

    // Group selection and voice lifecycle live on Impl (defined here in
    // the .cpp) so no ma_* type and no private-type name leaks into the
    // public header or its free functions.
    ma_sound_group* GroupForBus(AudioBus bus) noexcept
    {
        switch (bus)
        {
            case AudioBus::Music: return &musicGroup;
            case AudioBus::Effects: return &effectsGroup;
            case AudioBus::UI: return &uiGroup;
            case AudioBus::Master: return nullptr; // mixer parent, never a voice bus
        }
        return nullptr;
    }

    void DestroyVoiceObjects(Voice* voice)
    {
        QuiesceNoDeviceReaders();
        if (voice->soundOwned)
        {
            ma_sound_stop(&voice->sound);
            ma_sound_uninit(&voice->sound);
            voice->soundOwned = false;
        }
        if (voice->gainOwned)
        {
            ma_data_source_uninit(&voice->gain.base);
            voice->gainOwned = false;
        }
    }

    // Fully prepares a voice: gain source initialized (silent when
    // startSilent), sound initialized stopped on its bus group, pitch
    // applied. No map entry is created and no other voice is touched, so
    // any failure here preserves the victim by construction (ReplaceVoice
    // contract).
    bool PrepareVoice(
        const std::shared_ptr<const rt2::audio::DecodedAudioGeneration>& generation,
        const std::string& clipKey, rt2::audio::BackendClipHandle clipHandle,
        const rt2::audio::BackendVoiceStart& start, bool startSilent,
        std::unique_ptr<Voice>& voiceOut, core::Error& errorOut)
    {
        auto voice = std::make_unique<Voice>();
        voice->session = start.session;
        voice->owner = start.owner;
        voice->loop = start.loop;
        voice->bus = start.bus;
        voice->clipHandle = clipHandle;
        voice->clipKey = clipKey;
        voice->generation = generation;

        voice->gain.pcm = generation->pcmInterleaved.data();
        voice->gain.totalFrames = generation->frameCount;
        voice->gain.srcChannels = generation->channels;
        voice->gain.sampleRate = generation->sampleRate;
        voice->gain.cursor.store(0, std::memory_order_relaxed);
        voice->gain.loop = start.loop;
        voice->gain.packed.store(
            PackStereoGain(startSilent ? 0.0f : start.initialLeft,
                           startSilent ? 0.0f : start.initialRight),
            std::memory_order_release);
        ma_data_source_config dsConfig = ma_data_source_config_init();
        dsConfig.vtable = &s_gainVtable;
        if (ma_data_source_init(&dsConfig, &voice->gain.base) != MA_SUCCESS)
        {
            errorOut = MakeVoiceError(core::Error::InvalidRuntimeState,
                                      "audio voice start",
                                      "gain data source init failed");
            return false;
        }
        voice->gainOwned = true;

        ma_sound_group* group = GroupForBus(start.bus);
        if (group == nullptr)
        {
            ma_data_source_uninit(&voice->gain.base);
            voice->gainOwned = false;
            errorOut = MakeVoiceError(core::Error::InvalidArgument,
                                      "audio voice start",
                                      "Master is a mixer parent and cannot be a voice bus");
            return false;
        }
        if (ma_sound_init_from_data_source(&engine, &voice->gain.base,
                                           MA_SOUND_FLAG_NO_SPATIALIZATION, group,
                                           &voice->sound) != MA_SUCCESS)
        {
            ma_data_source_uninit(&voice->gain.base);
            voice->gainOwned = false;
            errorOut = MakeVoiceError(core::Error::InvalidRuntimeState,
                                      "audio voice start",
                                      "miniaudio sound init failed");
            return false;
        }
        voice->soundOwned = true;
        // Looping lives at both levels: the gain source wraps its cursor,
        // and the sound seeks back instead of treating the known length as
        // end-of-stream (without the sound flag, a looping voice would go
        // silent after one pass while the source still has content).
        if (start.loop)
            ma_sound_set_looping(&voice->sound, MA_TRUE);
        ma_sound_set_pitch(&voice->sound, start.pitch);
        voiceOut = std::move(voice);
        return true;
    }
};

ProductionAudioBackend::ProductionAudioBackend()
    : m_impl(new (std::nothrow) Impl())
{
}

ProductionAudioBackend::~ProductionAudioBackend()
{
    Shutdown();
    delete m_impl;
    m_impl = nullptr;
}

bool ProductionAudioBackend::StereoGainAtomicIsLockFree()
{
    return std::atomic<uint64_t>::is_always_lock_free;
}

std::string ProductionAudioBackend::BuildClipKey(
    const std::string& effectiveAssetId, const std::string& canonicalPath,
    uint64_t contentFingerprint, const std::string& decodeFormat)
{
    char fpHex[17];
    static const char* kDigits = "0123456789abcdef";
    for (int i = 0; i < 16; ++i)
        fpHex[i] = kDigits[(contentFingerprint >> (60 - i * 4)) & 0xF];
    return effectiveAssetId + "|" + canonicalPath + "|fp:" +
           std::string(fpHex, 16) + "|fmt:" + decodeFormat;
}

uint64_t ProductionAudioBackend::FingerprintBytes(const void* data, size_t bytes) noexcept
{
    return FingerprintLocal(data, bytes);
}

bool ProductionAudioBackend::Initialize(const ProductionBackendConfig& config,
                                        std::string& errorText)
{
    if (m_impl == nullptr)
    {
        errorText = "RT2AudioBackend: production backend storage allocation failed";
        return false;
    }
    if (m_impl->engineOwned)
    {
        errorText = "";
        return true;
    }
    if (!StereoGainAtomicIsLockFree())
    {
        errorText = "RT2AudioBackend: packed stereo-gain atomic is not lock-free on this target";
        return false;
    }
    m_impl->config = config;
    if (m_impl->config.maxVoices == 0)
        m_impl->config.maxVoices = 64;
    if (m_impl->config.decodedCacheBudgetBytes == 0)
        m_impl->config.decodedCacheBudgetBytes = 256ULL * 1024ULL * 1024ULL;

    // Hardware attempt first (unless forced), then diagnosed no-device
    // fallback. The engine outlives every session; voices/groups are torn
    // down before it in Shutdown (dependency order).
    bool wantNoDevice = config.forceNoDevice;
    std::string fallbackReason;
    ma_engine_config engineConfig = ma_engine_config_init();
    engineConfig.channels = kNoDeviceChannels;
    engineConfig.sampleRate = kNoDeviceSampleRate;
    if (wantNoDevice)
    {
        engineConfig.noDevice = MA_TRUE;
    }
    ma_result result;
    bool injectedHardwareFailure = false;
    if (!wantNoDevice && m_impl->failHardwareOpenOnce)
    {
        // Test hook: simulate a hardware-open failure without touching a
        // real device, so the fallback path below is forced deterministically.
        m_impl->failHardwareOpenOnce = false;
        injectedHardwareFailure = true;
        result = MA_ERROR;
    }
    else
    {
        result = ma_engine_init(&engineConfig, &m_impl->engine);
    }
    if (result != MA_SUCCESS && !wantNoDevice)
    {
        fallbackReason = injectedHardwareFailure
                             ? "hardware device open failed (injected); reopening no-device"
                             : "hardware device open failed; reopening no-device";
        ma_engine_config fallbackConfig = ma_engine_config_init();
        fallbackConfig.noDevice = MA_TRUE;
        fallbackConfig.channels = kNoDeviceChannels;
        fallbackConfig.sampleRate = kNoDeviceSampleRate;
        result = ma_engine_init(&fallbackConfig, &m_impl->engine);
        wantNoDevice = true;
    }
    if (result != MA_SUCCESS)
    {
        errorText = "RT2AudioBackend: ma_engine_init failed (hardware and no-device)";
        return false;
    }
    if (wantNoDevice &&
        ma_engine_get_sample_rate(&m_impl->engine) != kNoDeviceSampleRate)
    {
        errorText = "RT2AudioBackend: no-device engine sample rate mismatch";
        ma_engine_uninit(&m_impl->engine);
        return false;
    }
    if (wantNoDevice &&
        ma_engine_get_channels(&m_impl->engine) != kNoDeviceChannels)
    {
        errorText = "RT2AudioBackend: no-device engine channel mismatch";
        ma_engine_uninit(&m_impl->engine);
        return false;
    }
    m_impl->engineOwned = true;
    m_impl->noDeviceMode = wantNoDevice;

    // Mixer groups: Master on the endpoint, children under Master. Bus and
    // Master gains apply exactly once via these groups; voice mixes carry
    // explicit L/R only.
    //
    // Partial-init unwind (finding 1): each Owned flag is set immediately
    // after its successful init, and the failure path uninitializes only
    // owned groups in reverse order. ma_sound_uninit dereferences an
    // uninitialized object, so touching a never-initialized group here
    // would crash instead of reporting the diagnosed error.
    if (m_impl->failGroupInit == "master")
    {
        m_impl->failGroupInit.clear();
        errorText = "RT2AudioBackend: master sound group init failed (injected)";
        ma_engine_uninit(&m_impl->engine);
        m_impl->engineOwned = false;
        return false;
    }
    if (ma_sound_group_init(&m_impl->engine, 0, nullptr, &m_impl->masterGroup) != MA_SUCCESS)
    {
        errorText = "RT2AudioBackend: master sound group init failed";
        ma_engine_uninit(&m_impl->engine);
        m_impl->engineOwned = false;
        return false;
    }
    m_impl->masterOwned = true;
    auto initChild = [&](ma_sound_group* group, const char* name, bool& owned) -> bool {
        if (m_impl->failGroupInit == name)
        {
            m_impl->failGroupInit.clear();
            errorText = std::string("RT2AudioBackend: sound group init failed (") + name +
                        ", injected)";
            return false;
        }
        if (ma_sound_group_init(&m_impl->engine, 0, &m_impl->masterGroup, group) != MA_SUCCESS)
        {
            errorText = std::string("RT2AudioBackend: sound group init failed (") + name + ")";
            return false;
        }
        owned = true;
        return true;
    };
    if (!initChild(&m_impl->musicGroup, "music", m_impl->musicOwned) ||
        !initChild(&m_impl->effectsGroup, "effects", m_impl->effectsOwned) ||
        !initChild(&m_impl->uiGroup, "ui", m_impl->uiOwned))
    {
        if (m_impl->uiOwned)
        {
            ma_sound_group_uninit(&m_impl->uiGroup);
            m_impl->uiOwned = false;
        }
        if (m_impl->effectsOwned)
        {
            ma_sound_group_uninit(&m_impl->effectsGroup);
            m_impl->effectsOwned = false;
        }
        if (m_impl->musicOwned)
        {
            ma_sound_group_uninit(&m_impl->musicGroup);
            m_impl->musicOwned = false;
        }
        if (m_impl->masterOwned)
        {
            ma_sound_group_uninit(&m_impl->masterGroup);
            m_impl->masterOwned = false;
        }
        ma_engine_uninit(&m_impl->engine);
        m_impl->engineOwned = false;
        return false;
    }

    m_impl->scratch.assign(
        static_cast<size_t>(kNoDeviceMaxFramesPerRender) * kNoDeviceChannels, 0.0f);
    m_impl->discardBuffer.assign(
        static_cast<size_t>(kNoDeviceMaxFramesPerRender) * kNoDeviceChannels, 0.0f);

    if (m_impl->noDeviceMode)
    {
        m_impl->statusDetail = "production no-device float32 stereo 48kHz";
        if (!fallbackReason.empty())
            m_impl->statusDetail += " (fallback: " + fallbackReason + ")";
        else if (!config.forceNoDevice)
            m_impl->statusDetail += " (fallback: hardware device unavailable)";
        else
            m_impl->statusDetail += " (explicit)";
    }
    else
    {
        m_impl->statusDetail = "production hardware device";
    }
    errorText = "";
    return true;
}

bool ProductionAudioBackend::IsInitialized() const
{
    return m_impl != nullptr && m_impl->engineOwned;
}

void ProductionAudioBackend::Shutdown()
{
    if (m_impl == nullptr || !m_impl->engineOwned)
        return;
    // Dependency order: stop every voice, uninit sounds and gain sources,
    // then groups, then the engine. In no-device mode rendering happens
    // only inside RenderNoDeviceFrames on the main thread, so no audio
    // thread can observe teardown; in hardware mode each sound is stopped
    // before its objects are destroyed.
    for (auto& entry : m_impl->voices)
    {
        Impl::Voice* voice = entry.second.get();
        if (voice->soundOwned)
        {
            ma_sound_stop(&voice->sound);
            ma_sound_uninit(&voice->sound);
            voice->soundOwned = false;
        }
        if (voice->gainOwned)
        {
            ma_data_source_uninit(&voice->gain.base);
            voice->gainOwned = false;
        }
    }
    m_impl->voices.clear();
    m_impl->liveKeyVoices.clear();
    m_impl->clips.clear();
    m_impl->decodeCache.clear();
    m_impl->pausedSessions.clear();
    if (m_impl->uiOwned)
    {
        ma_sound_group_uninit(&m_impl->uiGroup);
        m_impl->uiOwned = false;
    }
    if (m_impl->effectsOwned)
    {
        ma_sound_group_uninit(&m_impl->effectsGroup);
        m_impl->effectsOwned = false;
    }
    if (m_impl->musicOwned)
    {
        ma_sound_group_uninit(&m_impl->musicGroup);
        m_impl->musicOwned = false;
    }
    if (m_impl->masterOwned)
    {
        ma_sound_group_uninit(&m_impl->masterGroup);
        m_impl->masterOwned = false;
    }
    ma_engine_uninit(&m_impl->engine);
    m_impl->engineOwned = false;
    m_impl->noDeviceMode = false;
    m_impl->statusDetail = "uninitialized";
}

void ProductionAudioBackend::SetClipByteResolver(ClipByteResolver resolver)
{
    if (m_impl != nullptr)
        m_impl->resolver = std::move(resolver);
}

void ProductionAudioBackend::TestHook_FailHardwareOpenOnce()
{
    if (m_impl != nullptr)
        m_impl->failHardwareOpenOnce = true;
}

void ProductionAudioBackend::TestHook_FailGroupInit(const std::string& group)
{
    if (m_impl != nullptr)
        m_impl->failGroupInit = group;
}

void ProductionAudioBackend::TestHook_SetRenderFault(TestRenderFault fault,
                                                     uint32_t shortFrames)
{
    if (m_impl != nullptr)
    {
        m_impl->renderFault = fault;
        m_impl->renderShortFrames = shortFrames;
    }
}

core::Result<std::shared_ptr<const rt2::audio::DecodedAudioGeneration>>
ProductionAudioBackend::FetchDecodedGeneration(const std::string& clipKey)
{
    using ResultGen =
        core::Result<std::shared_ptr<const rt2::audio::DecodedAudioGeneration>>;
    if (m_impl == nullptr || !m_impl->engineOwned)
        return ResultGen::Fail(core::Error::InvalidRuntimeState, clipKey,
                               "production backend is not initialized");
    if (clipKey.empty())
        return ResultGen::Fail(core::Error::InvalidArgument, clipKey,
                               "clip key must not be empty");
    auto it = m_impl->decodeCache.find(clipKey);
    if (it != m_impl->decodeCache.end())
    {
        it->second.lastUsed = ++m_impl->lruClock;
        return ResultGen::Ok(it->second.generation);
    }
    if (!m_impl->resolver)
        return ResultGen::Fail(core::Error::MissingAsset, clipKey,
                               "no clip bytes registered for this key and no resolver is wired");
    core::Result<rt2::audio::AudioClipBytes> resolved = m_impl->resolver(clipKey);
    if (!resolved.IsOk())
        return ResultGen::Fail(resolved.error.code, clipKey, resolved.error.detail);
    if (resolved.value.clipKey.empty())
    {
        rt2::audio::AudioClipBytes withKey = resolved.value;
        withKey.clipKey = clipKey;
        return DecodeClip(withKey);
    }
    return DecodeClip(resolved.value);
}

core::Result<std::shared_ptr<const rt2::audio::DecodedAudioGeneration>>
ProductionAudioBackend::DecodeClip(const rt2::audio::AudioClipBytes& clip)
{
    using ResultGen =
        core::Result<std::shared_ptr<const rt2::audio::DecodedAudioGeneration>>;
    if (m_impl == nullptr || !m_impl->engineOwned)
        return ResultGen::Fail(core::Error::InvalidRuntimeState, clip.clipKey,
                               "production backend is not initialized");
    if (clip.clipKey.empty())
        return ResultGen::Fail(core::Error::InvalidArgument, clip.clipKey,
                               "clip key must not be empty");
    if (clip.bytes == nullptr || clip.bytes->empty())
        return ResultGen::Fail(core::Error::MissingAsset, clip.clipKey,
                               "audio clip has no readable bytes");
    const uint64_t actualFingerprint =
        FingerprintLocal(clip.bytes->data(), clip.bytes->size());

    // The key carries the content identity: a same-size/same-mtime rewrite
    // decodes under a new key and overlaps the old generation. Keys that
    // do not embed a fingerprint bypass version safety and are refused.
    uint64_t keyFingerprint = 0;
    std::string keyFormat;
    if (!ParseClipKey(clip.clipKey, keyFingerprint, keyFormat))
        return ResultGen::Fail(core::Error::InvalidArgument, clip.clipKey,
                               "clip key does not carry the required fingerprint/format identity");
    if (keyFingerprint != actualFingerprint)
        return ResultGen::Fail(core::Error::InvalidArgument, clip.clipKey,
                               "clip key fingerprint does not match the decoded bytes");
    if (keyFormat != kDecodeFormat)
        return ResultGen::Fail(core::Error::InvalidArgument, clip.clipKey,
                               "unsupported decode format '" + keyFormat + "'");

    // Same bytes under the same key deduplicate to the existing immutable
    // generation; changed bytes replace the entry while old voices retain
    // their shared reference (overlap-correct).
    auto existing = m_impl->decodeCache.find(clip.clipKey);
    if (existing != m_impl->decodeCache.end() &&
        existing->second.fingerprint == actualFingerprint)
    {
        existing->second.lastUsed = ++m_impl->lruClock;
        return ResultGen::Ok(existing->second.generation);
    }

    ma_decoder_config decoderConfig =
        ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_decoder decoder{};
    if (ma_decoder_init_memory(clip.bytes->data(), clip.bytes->size(),
                               &decoderConfig, &decoder) != MA_SUCCESS)
        return ResultGen::Fail(core::Error::Parse, clip.clipKey,
                               "audio clip failed to decode (WAV/FLAC/MP3 content required)");
    const uint32_t channels = decoder.outputChannels;
    const uint32_t sampleRate = decoder.outputSampleRate;
    if (channels != 1 && channels != 2)
    {
        ma_decoder_uninit(&decoder);
        return ResultGen::Fail(core::Error::InvalidArgument, clip.clipKey,
                               "audio clip decoded to an unsupported channel count");
    }
    if (sampleRate == 0)
    {
        ma_decoder_uninit(&decoder);
        return ResultGen::Fail(core::Error::Parse, clip.clipKey,
                               "audio clip decoded to an invalid sample rate");
    }
    std::vector<float> pcm;
    {
        float chunk[4096 * 2];
        for (;;)
        {
            ma_uint64 framesRead = 0;
            const ma_result readResult = ma_decoder_read_pcm_frames(
                &decoder, chunk, 4096, &framesRead);
            // A non-end decoder error refuses even when a valid prefix
            // arrived: a corrupt clip must never publish a shorter success.
            if (readResult != MA_SUCCESS && readResult != MA_AT_END)
            {
                ma_decoder_uninit(&decoder);
                return ResultGen::Fail(core::Error::Parse, clip.clipKey,
                                       "audio clip failed partway through decode");
            }
            if (framesRead > 0)
            {
                const size_t base = pcm.size();
                if ((base + static_cast<size_t>(framesRead) * channels) * sizeof(float) >
                    kMaxDecodeBytes)
                {
                    ma_decoder_uninit(&decoder);
                    return ResultGen::Fail(core::Error::Parse, clip.clipKey,
                                           "decoded audio clip exceeds the sanity bound");
                }
                pcm.resize(base + static_cast<size_t>(framesRead) * channels);
                std::memcpy(pcm.data() + base, chunk,
                            static_cast<size_t>(framesRead) * channels * sizeof(float));
            }
            if (readResult == MA_AT_END || framesRead == 0)
                break;
        }
    }
    ma_decoder_uninit(&decoder);
    // Container-advertised length enforcement: a header that promises more
    // frames than the bytes deliver is corruption, not a shorter clip.
    {
        const ContainerCheck container =
            CheckContainerLength(clip.bytes->data(), clip.bytes->size());
        if (!container.ok)
            return ResultGen::Fail(core::Error::Parse, clip.clipKey, container.detail);
        if (container.enforce)
        {
            const uint64_t decodedFrames = pcm.size() / channels;
            if (decodedFrames != container.frames)
            {
                return ResultGen::Fail(
                    core::Error::Parse, clip.clipKey,
                    std::string(container.container) + " header advertises " +
                        std::to_string(container.frames) + " frames but decoded " +
                        std::to_string(decodedFrames));
            }
        }
    }
    if (pcm.empty() || (pcm.size() % channels) != 0)
        return ResultGen::Fail(core::Error::Parse, clip.clipKey,
                               "audio clip decoded to zero frames");
    for (float s : pcm)
    {
        if (!std::isfinite(s))
            return ResultGen::Fail(core::Error::Parse, clip.clipKey,
                                   "audio clip decoded to non-finite PCM");
    }

    auto generation = std::make_shared<rt2::audio::DecodedAudioGeneration>();
    generation->pcmInterleaved = std::move(pcm);
    generation->channels = channels;
    generation->sampleRate = sampleRate;
    generation->frameCount = static_cast<uint32_t>(
        generation->pcmInterleaved.size() / channels);

    const size_t needBytes =
        generation->pcmInterleaved.size() * sizeof(float);
    // Pinned-memory LRU: evict least-recently-used entries with no owners
    // outside this cache until the candidate fits. Pinned generations
    // (live voices, registered handles, AudioWorld holders) are never
    // evicted; the insert then fails loudly instead of exceeding the
    // budget. Failure names the key and byte counts and publishes nothing.
    size_t resident = m_impl->PinnedResidentBytes();
    // The same key re-decoded (fingerprint change) replaces its own entry:
    // its old bytes stop counting once replaced, so exclude them from the
    // fit check to avoid a self-eviction failure when exactly at budget.
    {
        auto replaced = m_impl->decodeCache.find(clip.clipKey);
        if (replaced != m_impl->decodeCache.end() &&
            replaced->second.fingerprint != actualFingerprint &&
            replaced->second.generation != generation &&
            Impl::IsEvictable(replaced->second))
        {
            resident -= std::min(resident, replaced->second.byteSize);
        }
    }
    while (resident + needBytes > m_impl->config.decodedCacheBudgetBytes)
    {
        const Impl::CacheEntry* victim = nullptr;
        std::string victimKey;
        size_t victimBytes = 0;
        for (const auto& entry : m_impl->decodeCache)
        {
            if (entry.first == clip.clipKey)
                continue;
            if (!Impl::IsEvictable(entry.second))
                continue;
            if (victim == nullptr || entry.second.lastUsed < victim->lastUsed)
            {
                victim = &entry.second;
                victimKey = entry.first;
                victimBytes = entry.second.byteSize;
            }
        }
        if (victim == nullptr)
        {
            return ResultGen::Fail(
                core::Error::Io, clip.clipKey,
                "decoded cache budget exceeded for asset '" + clip.clipKey +
                    "': requires " + std::to_string(needBytes) + " bytes, resident " +
                    std::to_string(resident) + " of budget " +
                    std::to_string(m_impl->config.decodedCacheBudgetBytes));
        }
        m_impl->decodeCache.erase(victimKey);
        resident -= std::min(resident, victimBytes);
    }

    Impl::CacheEntry entry;
    entry.generation = generation;
    entry.fingerprint = actualFingerprint;
    entry.byteSize = needBytes;
    entry.lastUsed = ++m_impl->lruClock;
    m_impl->decodeCache.insert_or_assign(clip.clipKey, std::move(entry));
    return ResultGen::Ok(std::move(generation));
}

core::Result<rt2::audio::BackendClipHandle> ProductionAudioBackend::RegisterDecodedGeneration(
    std::shared_ptr<const rt2::audio::DecodedAudioGeneration> generation)
{
    using ResultHandle = core::Result<rt2::audio::BackendClipHandle>;
    if (m_impl == nullptr || !m_impl->engineOwned)
        return ResultHandle::Fail(core::Error::InvalidRuntimeState,
                                  "audio clip generation",
                                  "production backend is not initialized");
    if (generation == nullptr || generation->pcmInterleaved.empty() ||
        generation->frameCount == 0)
        return ResultHandle::Fail(core::Error::InvalidArgument,
                                  "audio clip generation",
                                  "cannot register an empty decoded generation");
    if (generation->channels != 1 && generation->channels != 2)
        return ResultHandle::Fail(core::Error::InvalidArgument,
                                  "audio clip generation",
                                  "decoded generation must be mono or stereo");
    if (generation->pcmInterleaved.size() !=
        static_cast<size_t>(generation->frameCount) * generation->channels)
        return ResultHandle::Fail(core::Error::InvalidArgument,
                                  "audio clip generation",
                                  "decoded generation frame count does not match its PCM");
    if (generation->sampleRate == 0)
        return ResultHandle::Fail(core::Error::InvalidArgument,
                                  "audio clip generation",
                                  "decoded generation has an invalid sample rate");
    // Budget enforcement (finding R2): a supplied generation counts against
    // the same bound as decoded ones — the A3 provider seam can supply
    // generations independently of FetchDecodedGeneration. An object
    // already held (cache entry or earlier registration) adds nothing
    // (residency dedupes by identity); otherwise unpinned cache entries
    // are evicted LRU until it fits. Handles stay per-registration; only
    // residency dedupes. Over-budget registration fails loudly and pins
    // nothing.
    const size_t needBytes = Impl::GenerationBytes(generation);
    bool alreadyCounted = false;
    for (const auto& entry : m_impl->decodeCache)
    {
        if (entry.second.generation.get() == generation.get())
        {
            alreadyCounted = true;
            break;
        }
    }
    if (!alreadyCounted)
    {
        for (const auto& entry : m_impl->clips)
        {
            if (entry.second.get() == generation.get())
            {
                alreadyCounted = true;
                break;
            }
        }
    }
    size_t resident = m_impl->PinnedResidentBytes();
    const size_t added = alreadyCounted ? 0 : needBytes;
    while (resident + added > m_impl->config.decodedCacheBudgetBytes)
    {
        const Impl::CacheEntry* victim = nullptr;
        std::string victimKey;
        size_t victimBytes = 0;
        for (const auto& entry : m_impl->decodeCache)
        {
            if (!Impl::IsEvictable(entry.second))
                continue;
            if (victim == nullptr || entry.second.lastUsed < victim->lastUsed)
            {
                victim = &entry.second;
                victimKey = entry.first;
                victimBytes = entry.second.byteSize;
            }
        }
        if (victim == nullptr)
        {
            return ResultHandle::Fail(
                core::Error::Io, "audio clip generation",
                "decoded cache budget exceeded for direct registration: requires " +
                    std::to_string(needBytes) + " bytes, resident " +
                    std::to_string(resident) + " of budget " +
                    std::to_string(m_impl->config.decodedCacheBudgetBytes));
        }
        m_impl->decodeCache.erase(victimKey);
        resident -= std::min(resident, victimBytes);
    }
    rt2::audio::BackendClipHandle handle;
    handle.opaque = m_impl->nextHandleId++;
    m_impl->clips[handle.opaque] = std::move(generation);
    return ResultHandle::Ok(handle);
}

bool ProductionAudioBackend::ReleaseDecodedGeneration(
    rt2::audio::BackendClipHandle handle, core::Error& outError)
{
    if (m_impl == nullptr || !m_impl->engineOwned)
    {
        outError.code = core::Error::InvalidRuntimeState;
        outError.path = "audio clip generation";
        outError.detail = "production backend is not initialized";
        return false;
    }
    auto it = m_impl->clips.find(handle.opaque);
    if (it == m_impl->clips.end() || !handle.IsValid())
    {
        outError.code = core::Error::InvalidArgument;
        outError.path = "audio clip generation";
        outError.detail = "unknown backend clip handle";
        return false;
    }
    for (const auto& voiceEntry : m_impl->voices)
    {
        if (voiceEntry.second->clipHandle == handle)
        {
            outError.code = core::Error::InvalidRuntimeState;
            outError.path = "audio clip generation";
            outError.detail = "backend clip handle still has live voices";
            return false;
        }
    }
    m_impl->clips.erase(it);
    outError = core::Error{};
    return true;
}

namespace
{

bool ValidateStartCommon(const rt2::audio::BackendVoiceStart& start, core::Error& errorOut)
{
    if (!start.session.IsValid())
    {
        errorOut = MakeVoiceError(core::Error::InvalidArgument,
                                  "audio voice start",
                                  "voice start requires a valid session");
        return false;
    }
    if (!IsValidGain(start.initialLeft) || !IsValidGain(start.initialRight))
    {
        errorOut = MakeVoiceError(core::Error::InvalidArgument,
                                  "audio voice start",
                                  "voice start gains must be finite and >= 0");
        return false;
    }
    if (!IsValidPitch(start.pitch))
    {
        errorOut = MakeVoiceError(core::Error::InvalidArgument,
                                  "audio voice start",
                                  "voice start pitch must be finite and in [0.25, 4]");
        return false;
    }
    if (start.bus != AudioBus::Music &&
        start.bus != AudioBus::Effects &&
        start.bus != AudioBus::UI)
    {
        errorOut = MakeVoiceError(core::Error::InvalidArgument,
                                  "audio voice start",
                                  "voice start bus must be music, effects, or ui");
        return false;
    }
    return true;
}

} // namespace

core::Result<rt2::audio::BackendVoiceToken> ProductionAudioBackend::StartVoice(
    rt2::audio::BackendClipHandle clip, const rt2::audio::BackendVoiceStart& start)
{
    using ResultToken = core::Result<rt2::audio::BackendVoiceToken>;
    if (m_impl == nullptr || !m_impl->engineOwned)
        return ResultToken::Fail(core::Error::InvalidRuntimeState,
                                 "audio voice start",
                                 "production backend is not initialized");
    core::Error validation;
    if (!ValidateStartCommon(start, validation))
        return ResultToken::Fail(validation.code, validation.path, validation.detail);
    auto clipIt = m_impl->clips.find(clip.opaque);
    if (!clip.IsValid() || clipIt == m_impl->clips.end())
        return ResultToken::Fail(core::Error::InvalidArgument,
                                 "audio voice start",
                                 "unknown backend clip handle");
    if (m_impl->voices.size() >= m_impl->config.maxVoices)
        return ResultToken::Fail(core::Error::InvalidRuntimeState,
                                 "audio voice start",
                                 "backend voice cap reached");

    const std::shared_ptr<const rt2::audio::DecodedAudioGeneration>& generation = clipIt->second;
    // Resolve the cache key owning this generation for live-reference
    // accounting (zero-ref LRU observability).
    std::string ownerKey;
    for (const auto& entry : m_impl->decodeCache)
    {
        if (entry.second.generation == generation)
        {
            ownerKey = entry.first;
            break;
        }
    }

    std::unique_ptr<Impl::Voice> voice;
    if (!m_impl->PrepareVoice(generation, ownerKey, clip, start, false, voice, validation))
        return ResultToken::Fail(validation.code, validation.path, validation.detail);

    const bool sessionPaused =
        (m_impl->pausedSessions.find(start.session.value) != m_impl->pausedSessions.end());
    const bool shouldStart = !start.initialPaused && !sessionPaused;
    if (shouldStart)
    {
        if (ma_sound_start(&voice->sound) != MA_SUCCESS)
        {
            m_impl->DestroyVoiceObjects(voice.get());
            return ResultToken::Fail(core::Error::InvalidRuntimeState,
                                     "audio voice start",
                                     "miniaudio sound start failed");
        }
        voice->started = true;
        ++m_impl->startCalls;
    }
    voice->paused = start.initialPaused;
    voice->token.opaque = m_impl->nextTokenId++;
    const rt2::audio::BackendVoiceToken token = voice->token;
    if (!ownerKey.empty())
        ++m_impl->liveKeyVoices[ownerKey];
    m_impl->voices[token.opaque] = std::move(voice);
    if (m_impl->voices.size() > m_impl->peakLiveVoices)
        m_impl->peakLiveVoices = m_impl->voices.size();
    return ResultToken::Ok(token);
}

core::Result<rt2::audio::BackendVoiceToken> ProductionAudioBackend::ReplaceVoice(
    rt2::audio::BackendVoiceToken victim, rt2::audio::BackendClipHandle clip,
    const rt2::audio::BackendVoiceStart& start)
{
    using ResultToken = core::Result<rt2::audio::BackendVoiceToken>;
    if (m_impl == nullptr || !m_impl->engineOwned)
        return ResultToken::Fail(core::Error::InvalidRuntimeState,
                                 "audio voice replace",
                                 "production backend is not initialized");
    core::Error validation;
    if (!ValidateStartCommon(start, validation))
        return ResultToken::Fail(validation.code, validation.path, validation.detail);
    auto victimIt = m_impl->voices.find(victim.opaque);
    if (!victim.IsValid() || victimIt == m_impl->voices.end())
        return ResultToken::Fail(core::Error::InvalidArgument,
                                 "audio voice replace",
                                 "unknown victim backend voice token");
    auto clipIt = m_impl->clips.find(clip.opaque);
    if (!clip.IsValid() || clipIt == m_impl->clips.end())
        return ResultToken::Fail(core::Error::InvalidArgument,
                                 "audio voice replace",
                                 "unknown backend clip handle");
    Impl::Voice* victimVoice = victimIt->second.get();
    if (victimVoice->session != start.session)
        return ResultToken::Fail(core::Error::InvalidArgument,
                                 "audio voice replace",
                                 "replacement session must match the victim session");

    const std::shared_ptr<const rt2::audio::DecodedAudioGeneration>& generation = clipIt->second;
    std::string ownerKey;
    for (const auto& entry : m_impl->decodeCache)
    {
        if (entry.second.generation == generation)
        {
            ownerKey = entry.first;
            break;
        }
    }

    // Preparation (fallible) precedes the commit. Whether the replacement
    // must end up audible is decided BEFORE any sound is started
    // (finding R1): a replacement that must remain paused is never
    // started at all, so no audio callback can be in flight for it when
    // its gains are published — stopping first would still leave a window
    // for a callback that already passed miniaudio's state check. Any
    // failure here leaves the victim live, mapped, and audible.
    const bool sessionPaused =
        (m_impl->pausedSessions.find(start.session.value) != m_impl->pausedSessions.end());
    const bool shouldBeStarted = !start.initialPaused && !sessionPaused;
    std::unique_ptr<Impl::Voice> replacement;
    if (!m_impl->PrepareVoice(generation, ownerKey, clip, start, true, replacement, validation))
        return ResultToken::Fail(validation.code, validation.path, validation.detail);
    if (shouldBeStarted)
    {
        if (ma_sound_start(&replacement->sound) != MA_SUCCESS)
        {
            m_impl->DestroyVoiceObjects(replacement.get());
            return ResultToken::Fail(core::Error::InvalidRuntimeState,
                                     "audio voice replace",
                                     "miniaudio replacement sound start failed");
        }
        replacement->started = true;
        ++m_impl->startCalls;
    }

    // Commit: stop the victim (silent from here), destroy its objects,
    // then publish the replacement's real gains and swap the map entry.
    // The peak live count never exceeds the cap: the victim leaves in the
    // same step the replacement enters it. A never-started replacement
    // stays inaudible through publication; an audible one follows a
    // stopped victim with no overlap.
    const std::string victimKey = victimVoice->clipKey;
    ma_sound_stop(&victimVoice->sound);
    m_impl->DestroyVoiceObjects(victimVoice);
    m_impl->voices.erase(victimIt);
    if (!victimKey.empty())
    {
        auto liveIt = m_impl->liveKeyVoices.find(victimKey);
        if (liveIt != m_impl->liveKeyVoices.end() && liveIt->second > 0)
        {
            if (--liveIt->second == 0)
                m_impl->liveKeyVoices.erase(liveIt);
        }
    }

    replacement->gain.packed.store(
        PackStereoGain(start.initialLeft, start.initialRight),
        std::memory_order_release);
    replacement->paused = start.initialPaused;
    replacement->token.opaque = m_impl->nextTokenId++;
    const rt2::audio::BackendVoiceToken token = replacement->token;
    if (!ownerKey.empty())
        ++m_impl->liveKeyVoices[ownerKey];
    m_impl->voices[token.opaque] = std::move(replacement);
    if (m_impl->voices.size() > m_impl->peakLiveVoices)
        m_impl->peakLiveVoices = m_impl->voices.size();
    return ResultToken::Ok(token);
}

bool ProductionAudioBackend::StopVoice(rt2::audio::BackendVoiceToken token,
                                       core::Error& outError)
{
    if (m_impl == nullptr || !m_impl->engineOwned)
    {
        outError = MakeVoiceError(core::Error::InvalidRuntimeState,
                                  "audio voice stop",
                                  "production backend is not initialized");
        return false;
    }
    auto it = m_impl->voices.find(token.opaque);
    if (!token.IsValid() || it == m_impl->voices.end())
    {
        // Nothing session-owned to detach for an unknown token: loud
        // failure, no state change.
        outError = MakeVoiceError(core::Error::InvalidArgument,
                                  "audio voice stop",
                                  "unknown backend voice token");
        return false;
    }
    // Teardown detaches even on error: the voice is destroyed and its token
    // invalidated before returning, so a failure cannot leave a
    // session-owned sound reachable.
    Impl::Voice* voice = it->second.get();
    const std::string key = voice->clipKey;
    m_impl->DestroyVoiceObjects(voice);
    m_impl->voices.erase(it);
    if (!key.empty())
    {
        auto liveIt = m_impl->liveKeyVoices.find(key);
        if (liveIt != m_impl->liveKeyVoices.end() && liveIt->second > 0)
        {
            if (--liveIt->second == 0)
                m_impl->liveKeyVoices.erase(liveIt);
        }
    }
    outError = core::Error{};
    return true;
}

bool ProductionAudioBackend::PauseVoice(rt2::audio::BackendVoiceToken token, bool paused,
                                        core::Error& outError)
{
    if (m_impl == nullptr || !m_impl->engineOwned)
    {
        outError = MakeVoiceError(core::Error::InvalidRuntimeState,
                                  "audio voice pause",
                                  "production backend is not initialized");
        return false;
    }
    auto it = m_impl->voices.find(token.opaque);
    if (!token.IsValid() || it == m_impl->voices.end())
    {
        outError = MakeVoiceError(core::Error::InvalidArgument,
                                  "audio voice pause",
                                  "unknown backend voice token");
        return false;
    }
    Impl::Voice* voice = it->second.get();
    if (voice->paused == paused)
    {
        outError = core::Error{};
        return true;
    }
    const bool sessionPaused =
        (m_impl->pausedSessions.find(voice->session.value) != m_impl->pausedSessions.end());
    voice->paused = paused;
    const bool shouldBeStarted = !voice->paused && !sessionPaused && !voice->failed;
    if (shouldBeStarted && !voice->started)
    {
        if (ma_sound_start(&voice->sound) != MA_SUCCESS)
        {
            voice->failed = true;
            voice->failError = MakeVoiceError(core::Error::InvalidRuntimeState,
                                              "audio voice pause",
                                              "miniaudio sound restart failed");
            outError = voice->failError;
            return false;
        }
        voice->started = true;
        ++m_impl->startCalls;
    }
    else if (!shouldBeStarted && voice->started)
    {
        if (ma_sound_stop(&voice->sound) != MA_SUCCESS)
        {
            outError = MakeVoiceError(core::Error::InvalidRuntimeState,
                                      "audio voice pause",
                                      "miniaudio sound stop failed");
            return false;
        }
        voice->started = false;
    }
    outError = core::Error{};
    return true;
}

bool ProductionAudioBackend::SetVoiceMix(rt2::audio::BackendVoiceToken token,
                                         const rt2::audio::BackendVoiceMix& mix,
                                         core::Error& outError)
{
    if (m_impl == nullptr || !m_impl->engineOwned)
    {
        outError = MakeVoiceError(core::Error::InvalidRuntimeState,
                                  "audio voice mix",
                                  "production backend is not initialized");
        return false;
    }
    auto it = m_impl->voices.find(token.opaque);
    if (!token.IsValid() || it == m_impl->voices.end())
    {
        outError = MakeVoiceError(core::Error::InvalidArgument,
                                  "audio voice mix",
                                  "unknown backend voice token");
        return false;
    }
    if (!IsValidGain(mix.left) || !IsValidGain(mix.right))
    {
        outError = MakeVoiceError(core::Error::InvalidArgument,
                                  "audio voice mix",
                                  "voice mix gains must be finite and >= 0");
        return false;
    }
    if (!IsValidPitch(mix.pitch))
    {
        outError = MakeVoiceError(core::Error::InvalidArgument,
                                  "audio voice mix",
                                  "voice mix pitch must be finite and in [0.25, 4]");
        return false;
    }
    Impl::Voice* voice = it->second.get();
    // One release store: the audio thread observes the complete old or new
    // pair, never a torn mix. Pitch travels miniaudio's thread-safe setter.
    voice->gain.packed.store(PackStereoGain(mix.left, mix.right),
                             std::memory_order_release);
    ma_sound_set_pitch(&voice->sound, mix.pitch);
    outError = core::Error{};
    return true;
}

core::Result<std::vector<rt2::audio::BackendVoiceCompletion>>
ProductionAudioBackend::DrainCompletions(rt2::audio::AudioSessionId session)
{
    using ResultVec = core::Result<std::vector<rt2::audio::BackendVoiceCompletion>>;
    if (m_impl == nullptr || !m_impl->engineOwned)
        return ResultVec::Fail(core::Error::InvalidRuntimeState,
                               "audio completion drain",
                               "production backend is not initialized");
    std::vector<rt2::audio::BackendVoiceCompletion> due;
    std::vector<uint64_t> reap;
    for (const auto& entry : m_impl->voices)
    {
        const Impl::Voice* voice = entry.second.get();
        if (voice->session != session)
            continue;
        const uint64_t cursor =
            voice->gain.cursor.load(std::memory_order_relaxed);
        const bool sourceExhausted =
            !voice->loop && cursor >= voice->gain.totalFrames;
        const bool deviceAtEnd =
            (ma_sound_at_end(&voice->sound) == MA_TRUE);
        if (voice->failed || sourceExhausted || deviceAtEnd)
        {
            rt2::audio::BackendVoiceCompletion completion;
            completion.session = session;
            completion.token = voice->token;
            if (voice->failed)
            {
                completion.reason = rt2::audio::BackendCompletionReason::Failed;
                completion.error = voice->failError;
            }
            else
            {
                completion.reason = rt2::audio::BackendCompletionReason::Completed;
            }
            due.push_back(completion);
            reap.push_back(entry.first);
        }
    }
    for (uint64_t tokenId : reap)
    {
        auto it = m_impl->voices.find(tokenId);
        if (it == m_impl->voices.end())
            continue;
        const std::string key = it->second->clipKey;
        m_impl->DestroyVoiceObjects(it->second.get());
        m_impl->voices.erase(it);
        if (!key.empty())
        {
            auto liveIt = m_impl->liveKeyVoices.find(key);
            if (liveIt != m_impl->liveKeyVoices.end() && liveIt->second > 0)
            {
                if (--liveIt->second == 0)
                    m_impl->liveKeyVoices.erase(liveIt);
            }
        }
    }
    return ResultVec::Ok(std::move(due));
}

bool ProductionAudioBackend::SetSessionPaused(rt2::audio::AudioSessionId session,
                                              bool paused, core::Error& outError)
{
    if (m_impl == nullptr || !m_impl->engineOwned)
    {
        outError = MakeVoiceError(core::Error::InvalidRuntimeState,
                                  "audio session pause",
                                  "production backend is not initialized");
        return false;
    }
    if (!session.IsValid())
    {
        outError = MakeVoiceError(core::Error::InvalidArgument,
                                  "audio session pause",
                                  "session pause requires a valid session");
        return false;
    }
    // Single atomic backend boundary: every session voice is frozen or
    // resumed under one main-thread pass; a voice created while paused
    // carries initialPaused so Step can never observe a briefly audible
    // voice.
    if (paused)
        m_impl->pausedSessions.insert(session.value);
    else
        m_impl->pausedSessions.erase(session.value);
    for (auto& entry : m_impl->voices)
    {
        Impl::Voice* voice = entry.second.get();
        if (voice->session != session || voice->failed)
            continue;
        const bool shouldBeStarted = !voice->paused && !paused;
        if (shouldBeStarted && !voice->started)
        {
            if (ma_sound_start(&voice->sound) != MA_SUCCESS)
            {
                voice->failed = true;
                voice->failError = MakeVoiceError(core::Error::InvalidRuntimeState,
                                                  "audio session pause",
                                                  "miniaudio sound resume failed");
                continue;
            }
            voice->started = true;
            ++m_impl->startCalls;
        }
        else if (!shouldBeStarted && voice->started)
        {
            if (ma_sound_stop(&voice->sound) != MA_SUCCESS)
            {
                outError = MakeVoiceError(core::Error::InvalidRuntimeState,
                                          "audio session pause",
                                          "miniaudio sound freeze failed");
                return false;
            }
            voice->started = false;
        }
    }
    outError = core::Error{};
    return true;
}

bool ProductionAudioBackend::SetBusGain(AudioBus bus, float gain,
                                        core::Error& outError)
{
    if (m_impl == nullptr || !m_impl->engineOwned)
    {
        outError = MakeVoiceError(core::Error::InvalidRuntimeState,
                                  "audio bus gain",
                                  "production backend is not initialized");
        return false;
    }
    if (!IsValidBusGain(gain))
    {
        outError = MakeVoiceError(core::Error::InvalidArgument,
                                  "audio bus gain",
                                  "bus gain must be finite and in [0, 4]");
        return false;
    }
    ma_sound_group* target = nullptr;
    size_t index = 0;
    switch (bus)
    {
        case AudioBus::Master: target = &m_impl->masterGroup; index = 0; break;
        case AudioBus::Music: target = &m_impl->musicGroup; index = 1; break;
        case AudioBus::Effects: target = &m_impl->effectsGroup; index = 2; break;
        case AudioBus::UI: target = &m_impl->uiGroup; index = 3; break;
        default:
            outError = MakeVoiceError(core::Error::InvalidArgument,
                                      "audio bus gain",
                                      "unknown audio bus");
            return false;
    }
    // Master is the explicit master operation: child groups keep their own
    // volumes and inherit Master through the hierarchy, so every bus gain
    // applies exactly once.
    ma_sound_group_set_volume(target, gain);
    m_impl->busGains[index] = gain;
    outError = core::Error{};
    return true;
}

bool ProductionAudioBackend::StopSessionVoices(rt2::audio::AudioSessionId session,
                                               core::Error& outError)
{
    if (m_impl == nullptr || !m_impl->engineOwned)
    {
        outError = MakeVoiceError(core::Error::InvalidRuntimeState,
                                  "audio session stop",
                                  "production backend is not initialized");
        return false;
    }
    // Teardown: every session voice is detached and invalidated even
    // though no fallible step exists; an error return can never leave a
    // session-owned sound running.
    std::vector<uint64_t> reap;
    for (const auto& entry : m_impl->voices)
    {
        if (entry.second->session == session)
            reap.push_back(entry.first);
    }
    for (uint64_t tokenId : reap)
    {
        auto it = m_impl->voices.find(tokenId);
        if (it == m_impl->voices.end())
            continue;
        const std::string key = it->second->clipKey;
        m_impl->DestroyVoiceObjects(it->second.get());
        m_impl->voices.erase(it);
        if (!key.empty())
        {
            auto liveIt = m_impl->liveKeyVoices.find(key);
            if (liveIt != m_impl->liveKeyVoices.end() && liveIt->second > 0)
            {
                if (--liveIt->second == 0)
                    m_impl->liveKeyVoices.erase(liveIt);
            }
        }
    }
    m_impl->pausedSessions.erase(session.value);
    outError = core::Error{};
    return true;
}

core::Result<uint32_t> ProductionAudioBackend::RenderNoDeviceFrames(
    rt2::audio::AudioPcmWriteBuffer interleavedStereo, uint32_t requestedFrames)
{
    using ResultFrames = core::Result<uint32_t>;
    if (m_impl == nullptr || !m_impl->engineOwned)
        return ResultFrames::Fail(core::Error::InvalidRuntimeState,
                                  "audio no-device render",
                                  "production backend is not initialized");
    if (!m_impl->noDeviceMode)
        return ResultFrames::Fail(core::Error::InvalidRuntimeState,
                                  "audio no-device render",
                                  "no-device rendering requires production no-device mode");
    if (interleavedStereo.data == nullptr)
        return ResultFrames::Fail(core::Error::InvalidArgument,
                                  "audio no-device render",
                                  "caller output buffer must not be null");
    if (requestedFrames < 1 || requestedFrames > kNoDeviceMaxFramesPerRender)
        return ResultFrames::Fail(core::Error::InvalidArgument,
                                  "audio no-device render",
                                  "requested frames must be in [1, 4096]");
    const uint64_t requiredSamples =
        static_cast<uint64_t>(requestedFrames) * kNoDeviceChannels;
    if (static_cast<uint64_t>(interleavedStereo.sampleCapacity) < requiredSamples)
        return ResultFrames::Fail(core::Error::InvalidArgument,
                                  "audio no-device render",
                                  "caller output buffer is undersized for the request");

    // Same production path as the editor fallback: render into the
    // preallocated scratch area, then copy only the successful prefix. A
    // short read leaves the caller remainder untouched; a hard failure
    // copies nothing. Note the idle rule differs from the bare A1 adapter:
    // the bus groups keep the production graph attached, so an idle
    // production engine renders full silent frames (a bare engine with no
    // attachments reports zero).
    //
    // Test-fault injection (finding 5): FailOnce simulates the hard engine
    // failure at the render boundary without touching the engine or caller
    // storage; ShortOnce performs the real render but reports a truncated
    // successful prefix. Both consume the armed fault. The FailOnce label
    // is honest simulation: it covers the production failure branch
    // (typed error + zero caller mutation), not a miniaudio-internal read
    // failure, which valid engine state cannot force.
    if (m_impl->renderFault == TestRenderFault::FailOnce)
    {
        m_impl->renderFault = TestRenderFault::None;
        return ResultFrames::Fail(core::Error::InvalidRuntimeState,
                                  "audio no-device render",
                                  "production engine render failed (injected)");
    }
    ma_uint64 framesRead = 0;
    m_impl->readActive.store(1, std::memory_order_release);
    const ma_result result = ma_engine_read_pcm_frames(
        &m_impl->engine, m_impl->scratch.data(), requestedFrames, &framesRead);
    m_impl->readActive.store(0, std::memory_order_release);
    if (result != MA_SUCCESS || framesRead > requestedFrames)
        return ResultFrames::Fail(core::Error::InvalidRuntimeState,
                                  "audio no-device render",
                                  "production engine render failed");
    uint32_t rendered = static_cast<uint32_t>(framesRead);
    if (m_impl->renderFault == TestRenderFault::ShortOnce)
    {
        m_impl->renderFault = TestRenderFault::None;
        if (rendered > m_impl->renderShortFrames)
            rendered = m_impl->renderShortFrames;
    }
    if (rendered > 0)
    {
        std::memcpy(interleavedStereo.data, m_impl->scratch.data(),
                    static_cast<size_t>(rendered) * kNoDeviceChannels * sizeof(float));
    }
    return ResultFrames::Ok(rendered);
}

rt2::audio::AudioBackendStatus ProductionAudioBackend::Status() const
{
    rt2::audio::AudioBackendStatus status;
    if (m_impl != nullptr && m_impl->engineOwned)
    {
        status.productionNoDevice = m_impl->noDeviceMode;
        status.detail = m_impl->statusDetail;
    }
    else
    {
        status.productionNoDevice = false;
        status.detail = "uninitialized";
    }
    return status;
}

size_t ProductionAudioBackend::DecodedCacheEntryCount() const
{
    return (m_impl != nullptr) ? m_impl->decodeCache.size() : 0;
}

size_t ProductionAudioBackend::DecodedCacheResidentBytes() const
{
    return (m_impl != nullptr) ? m_impl->PinnedResidentBytes() : 0;
}

size_t ProductionAudioBackend::LiveVoiceCount() const
{
    return (m_impl != nullptr) ? m_impl->voices.size() : 0;
}

size_t ProductionAudioBackend::PeakLiveVoices() const
{
    return (m_impl != nullptr) ? m_impl->peakLiveVoices : 0;
}

uint64_t ProductionAudioBackend::SoundStartCallCount() const
{
    return (m_impl != nullptr) ? m_impl->startCalls : 0;
}

size_t ProductionAudioBackend::ActiveVoicesForKey(const std::string& clipKey) const
{
    if (m_impl == nullptr)
        return 0;
    auto it = m_impl->liveKeyVoices.find(clipKey);
    return (it != m_impl->liveKeyVoices.end()) ? it->second : 0;
}

size_t ProductionAudioBackend::EvictZeroReferenceGenerations()
{
    if (m_impl == nullptr)
        return 0;
    size_t evicted = 0;
    for (auto it = m_impl->decodeCache.begin();
         it != m_impl->decodeCache.end();)
    {
        if (Impl::IsEvictable(it->second))
        {
            it = m_impl->decodeCache.erase(it);
            ++evicted;
        }
        else
        {
            ++it;
        }
    }
    return evicted;
}

} // namespace rt2::audio::backend
