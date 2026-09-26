// ProbeMain.cpp
//
// A4: RT2AudioProbe exercises the PRODUCTION backend (real WAV/FLAC/MP3
// decode, RT2 stereo-gain node, engine mixer, immutable cache) in fixed
// float32 stereo 48 kHz no-device mode with exact oracles.
//
// Every refusal prints to stderr and exits nonzero (this codebase's
// characteristic bug is silent failure). Sections:
//   S1  pin, no-device mode, lock-free gain atomic
//   S1b forced hardware-open failure falls back to diagnosed no-device
//   S1c per-group init failure unwinds owned groups only (injected)
//   S2  WAV mono f32 decode (determinism, rate/channels/count, sine shape)
//   S3  WAV stereo f32 decode (channels distinct, L matches mono)
//   S4  FLAC lossless decode (matches s16 master within 1 ulp)
//   S4b zero-total FLAC is valid unknown length (identical PCM)
//   S5  MP3 decode (energy, range, rate/channels)
//   S6  corrupt inputs refuse with typed errors and publish no cache entry
//   S7  fingerprint/key mismatch and malformed keys refuse
//   S8  Fetch dedupe (same key, same immutable object)
//   S9  old/new fingerprint overlap on one path (distinct PCM, concurrent)
//   S10 Register/Release (double handle, unknown, live-handle refusal)
//   S10b registration budget enforcement (10 MiB RED, identity dedupe,
//       evict-only-unpinned)
//   S11 exact L/R transport (hard-left/center/hard-right, attenuation,
//       bus and Master halving through real group volumes)
//   S12 pitch-2.0 completion range through the thread-safe pitch path
//   S13 completion token match + post-completion silence
//   S14 session pause freeze, initialPaused, PauseVoice resume
//   S15 StopSessionVoices census
//   S16 RenderNoDeviceFrames validation matrix, idle silence, prefix
//       discipline, post-shutdown hard failure (caller storage untouched)
//   S16b injected short/hard render results with caller-buffer proof
//   S17 hard-cap ReplaceVoice (one-unit commit, victim preserved on failure)
//   S17b concurrent paused-replacement rendering stays silent
//   S18 over-budget decode names asset and byte counts, publishes nothing
//   S18b pinned (registered) generations count against the budget
//   S19 torn-pair stress on the packed L/R atomic through the real block
//       function (TSan: unavailable on MSVC; design is allocation-free
//       with one acquire load per block)
//   S19b SetVoiceMix stress through a real rendering voice
//   S20 A5 semantic/render/accounting phase order through AudioWorld:
//       first-frame autoplay renders audible PCM, same-frame Stop renders
//       silence, cursors advance by exactly what rendered
//   S21 A8 shipped acceptance scene through the production path: the
//       checked-in scene file drives production Resolve (path+sidecar),
//       real-file decode, AudioWorld autoplay/play, exact no-device PCM,
//       natural completion, corrupt-file refusal, failure modes for
//       broken path/sidecar/bytes, and a 20-cycle production
//       session/cache/generation census with explicit purge baseline.
//
// Fixtures live in RT2AudioProbe/fixtures (generated; see README.md). The
// probe reads them into immutable byte vectors and decodes from memory:
// no path the content can change underneath is ever trusted. S21
// additionally reads the shipped acceptance scene and clips under
// RT2App/assets (generated copies; see RT2App/assets/audio/README.md)
// from the repository root.

#include "AudioBackendPin.h"
#include "ProductionAudioBackend.h"
#include "AudioWorld.h"
#include "AssetResolver.h"
#include "json.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{

using namespace rt2::audio;
using namespace rt2::audio::backend;

int Fail(const char* what)
{
    std::fprintf(stderr, "RT2AudioProbe FAIL: %s\n", what);
    return 1;
}

int FailDetail(const std::string& what)
{
    std::fprintf(stderr, "RT2AudioProbe FAIL: %s\n", what.c_str());
    return 1;
}

void Pass(const char* what)
{
    std::printf("RT2AudioProbe: PASS %s\n", what);
}

std::vector<char> ReadFile(const std::string& path, bool& ok)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        ok = false;
        return {};
    }
    in.seekg(0, std::ios::end);
    const std::streampos end = in.tellg();
    if (end < 0)
    {
        ok = false;
        return {};
    }
    in.seekg(0, std::ios::beg);
    std::vector<char> bytes(static_cast<size_t>(end));
    if (!bytes.empty())
        in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ok = static_cast<bool>(in);
    return bytes;
}

float MaxAbs(const float* samples, size_t count)
{
    float peak = 0.0f;
    for (size_t i = 0; i < count; ++i)
    {
        const float a = std::fabs(samples[i]);
        if (a > peak)
            peak = a;
    }
    return peak;
}

float MaxAbsChannel(const float* stereo, size_t frames, int channel)
{
    float peak = 0.0f;
    for (size_t f = 0; f < frames; ++f)
    {
        const float a = std::fabs(stereo[f * 2 + channel]);
        if (a > peak)
            peak = a;
    }
    return peak;
}

// Counts sign changes (zero crossings) of one channel.
size_t ZeroCrossings(const float* stereo, size_t frames, int channel)
{
    size_t crossings = 0;
    float prev = stereo[channel];
    for (size_t f = 1; f < frames; ++f)
    {
        const float cur = stereo[f * 2 + channel];
        if ((prev <= 0.0f && cur > 0.0f) || (prev >= 0.0f && cur < 0.0f))
            ++crossings;
        prev = cur;
    }
    return crossings;
}

bool AllZero(const float* samples, size_t count)
{
    for (size_t i = 0; i < count; ++i)
    {
        if (samples[i] != 0.0f)
            return false;
    }
    return true;
}

bool AllSentinel(const float* samples, size_t count, float sentinel)
{
    for (size_t i = 0; i < count; ++i)
    {
        if (samples[i] != sentinel)
            return false;
    }
    return true;
}

std::string KeyFor(ProductionAudioBackend& backend, const std::string& id,
                   const std::string& path, const std::vector<char>& bytes)
{
    const uint64_t fp = ProductionAudioBackend::FingerprintBytes(
        bytes.data(), bytes.size());
    return ProductionAudioBackend::BuildClipKey(id, path, fp, "f32le");
}

BackendVoiceStart MakeStart(AudioSessionId session, AudioBus bus, float l, float r)
{
    BackendVoiceStart start;
    start.session = session;
    start.owner = AudioOwnerKind::Runtime;
    start.loop = false;
    start.initialPaused = false;
    start.bus = bus;
    start.initialLeft = l;
    start.initialRight = r;
    start.pitch = 1.0f;
    return start;
}

// Renders exactly `frames` engine frames (bounded 4096 chunks), asserting
// every chunk reports success. Returns total rendered (short reads at
// idle/completion may make it smaller).
uint32_t RenderAll(ProductionAudioBackend& backend, std::vector<float>& out,
                   uint32_t frames, bool& ok)
{
    out.clear();
    out.resize(static_cast<size_t>(frames) * kNoDeviceChannels, 0.0f);
    uint32_t total = 0;
    std::vector<float> chunk(
        static_cast<size_t>(kNoDeviceMaxFramesPerRender) * kNoDeviceChannels, 0.0f);
    while (total < frames)
    {
        const uint32_t want = (frames - total > kNoDeviceMaxFramesPerRender)
                                  ? kNoDeviceMaxFramesPerRender
                                  : (frames - total);
        AudioPcmWriteBuffer buf{ chunk.data(),
                                 static_cast<size_t>(want) * kNoDeviceChannels };
        // Fill with sentinel so a short read's untouched tail is visible.
        std::fill(chunk.begin(), chunk.end(), -1234.5f);
        rt2::core::Result<uint32_t> result = backend.RenderNoDeviceFrames(buf, want);
        if (!result.IsOk())
        {
            ok = false;
            return total;
        }
        if (result.value > want)
        {
            ok = false;
            return total;
        }
        std::memcpy(out.data() + static_cast<size_t>(total) * kNoDeviceChannels,
                    chunk.data(),
                    static_cast<size_t>(result.value) * kNoDeviceChannels * sizeof(float));
        total += result.value;
        if (result.value < want)
            break; // successful-prefix short read: remainder untouched
    }
    out.resize(static_cast<size_t>(total) * kNoDeviceChannels);
    ok = true;
    return total;
}

// Builds a synthetic constant (DC) generation. DC is pipeline-invariant:
// any linear mixer stage maps a constant to a constant, so gain transport
// is provable bit-exactly despite engine block-cache latency, while sine
// fixtures prove real content flow separately.
std::shared_ptr<const DecodedAudioGeneration> MakeDC(float value, uint32_t frames = 48000)
{
    auto gen = std::make_shared<DecodedAudioGeneration>();
    gen->channels = 1;
    gen->sampleRate = 48000;
    gen->frameCount = frames;
    gen->pcmInterleaved.assign(frames, value);
    return gen;
}

// Synthesizes canonical float32 mono WAV bytes (44-byte header + data) for
// budget-sized decode inputs. Mirrors fixtures/generate_fixtures.py.
std::vector<char> BuildWavF32MonoDC(float value, uint32_t frames)
{
    std::vector<char> bytes;
    bytes.resize(44 + static_cast<size_t>(frames) * 4);
    auto put32 = [&](size_t at, uint32_t v) {
        bytes[at + 0] = static_cast<char>(v & 0xFF);
        bytes[at + 1] = static_cast<char>((v >> 8) & 0xFF);
        bytes[at + 2] = static_cast<char>((v >> 16) & 0xFF);
        bytes[at + 3] = static_cast<char>((v >> 24) & 0xFF);
    };
    auto put16 = [&](size_t at, uint32_t v) {
        bytes[at + 0] = static_cast<char>(v & 0xFF);
        bytes[at + 1] = static_cast<char>((v >> 8) & 0xFF);
    };
    const uint32_t dataBytes = frames * 4;
    std::memcpy(bytes.data() + 0, "RIFF", 4);
    put32(4, 36 + dataBytes);
    std::memcpy(bytes.data() + 8, "WAVE", 4);
    std::memcpy(bytes.data() + 12, "fmt ", 4);
    put32(16, 16);
    put16(20, 3); // IEEE float
    put16(22, 1); // mono
    put32(24, 48000);
    put32(28, 48000 * 4);
    put16(32, 4);
    put16(34, 32);
    std::memcpy(bytes.data() + 36, "data", 4);
    put32(40, dataBytes);
    float* samples = reinterpret_cast<float*>(bytes.data() + 44);
    for (uint32_t f = 0; f < frames; ++f)
        samples[f] = value;
    return bytes;
}

// Renders and discards `frames` engine frames (pipeline settle after a
// voice start/stop/mix change: the block-cached mixer needs a few frames
// to flush stale content and latency).
bool Settle(ProductionAudioBackend& backend, uint32_t frames = 8192)
{
    std::vector<float> tmp;
    bool ok = false;
    RenderAll(backend, tmp, frames, ok);
    return ok;
}

// Renders up to 4 chunks; passes when a full chunk is exactly silent.
// Stale block-cache content after a stop is finite, so one silent chunk
// proves the mixer reached silence.
bool RenderReachedSilence(ProductionAudioBackend& backend)
{
    for (int i = 0; i < 4; ++i)
    {
        std::vector<float> chunk;
        bool ok = false;
        RenderAll(backend, chunk, 4096, ok);
        if (!ok || chunk.size() != 4096 * kNoDeviceChannels)
            return false;
        if (AllZero(chunk.data(), chunk.size()))
            return true;
    }
    return false;
}

} // namespace

int main()
{
    std::printf("RT2AudioProbe: miniaudio %s @ %s\n", kPinnedMiniaudioVersionString,
                kPinnedMiniaudioCommit);

    // ---- S1: mode, pin, lock-free ----
    if (!ProductionAudioBackend::StereoGainAtomicIsLockFree())
        return Fail("packed stereo-gain atomic is not lock-free on this target");
    ProductionBackendConfig config;
    config.forceNoDevice = true;
    ProductionAudioBackend backend;
    {
        std::string errorText;
        if (!backend.Initialize(config, errorText))
        {
            std::fprintf(stderr, "RT2AudioProbe FAIL: initialize: %s\n",
                         errorText.empty() ? "unknown" : errorText.c_str());
            return 1;
        }
    }
    {
        const AudioBackendStatus status = backend.Status();
        if (!status.productionNoDevice)
            return Fail("forced no-device backend does not report production no-device");
        if (status.detail.find("no-device") == std::string::npos ||
            status.detail.find("48") == std::string::npos)
            return Fail("no-device status detail does not name the mode");
    }
    Pass("S1 no-device mode + lock-free gain atomic");

    // ---- S1b: injected hardware-open failure falls back ----
    {
        ProductionAudioBackend fallback;
        fallback.TestHook_FailHardwareOpenOnce();
        ProductionBackendConfig hwConfig; // forceNoDevice == false
        std::string errorText;
        if (!fallback.Initialize(hwConfig, errorText))
        {
            std::fprintf(stderr, "RT2AudioProbe FAIL: fallback init: %s\n",
                         errorText.empty() ? "unknown" : errorText.c_str());
            return 1;
        }
        const AudioBackendStatus status = fallback.Status();
        if (!status.productionNoDevice)
            return Fail("fallback backend does not report production no-device");
        if (status.detail.find("fallback") == std::string::npos ||
            status.detail.find("injected") == std::string::npos)
            return Fail("fallback diagnostic does not name the injected failure");
        fallback.Shutdown();
    }
    Pass("S1b injected hardware-open failure falls back to diagnosed no-device");

    // ---- S1c: per-group init failure unwinds owned groups only ----
    {
        const char* groups[] = { "master", "music", "effects", "ui" };
        for (const char* group : groups)
        {
            ProductionAudioBackend partial;
            partial.TestHook_FailGroupInit(group);
            std::string errorText;
            if (partial.Initialize(config, errorText))
                return FailDetail(std::string("group-init injection at ") + group +
                                  " unexpectedly succeeded");
            if (errorText.find(group) == std::string::npos)
                return FailDetail(std::string("group-init diagnostic at ") + group +
                                  " does not name the failed group");
            if (partial.IsInitialized())
                return FailDetail(std::string("partial init at ") + group +
                                  " reports initialized");
            if (partial.Status().productionNoDevice)
                return FailDetail(std::string("failed init at ") + group +
                                  " reports no-device status");
            if (partial.LiveVoiceCount() != 0 || partial.DecodedCacheEntryCount() != 0)
                return FailDetail(std::string("failed init at ") + group + " left residue");
            // A clean retry on the same object proves the unwind left no
            // half-initialized group behind (an uninitialized-group uninit
            // would crash instead of failing loudly).
            std::string retryError;
            if (!partial.Initialize(config, retryError))
                return FailDetail(std::string("clean retry after ") + group +
                                  " failure failed: " + retryError);
            partial.Shutdown();
        }
    }
    Pass("S1c per-group init failure unwinds owned groups only");

    // ---- Fixtures ----
    bool ok = false;
    const std::vector<char> monoF32 = ReadFile("RT2AudioProbe/fixtures/tone440_mono_f32.wav", ok);
    if (!ok || monoF32.empty())
        return Fail("cannot read fixtures/tone440_mono_f32.wav (run from repository root)");
    const std::vector<char> stereoF32 =
        ReadFile("RT2AudioProbe/fixtures/tone660_stereo_f32.wav", ok);
    if (!ok || stereoF32.empty())
        return Fail("cannot read fixtures/tone660_stereo_f32.wav");
    const std::vector<char> stereoS16 =
        ReadFile("RT2AudioProbe/fixtures/tone660_stereo_s16.wav", ok);
    if (!ok || stereoS16.empty())
        return Fail("cannot read fixtures/tone660_stereo_s16.wav");
    const std::vector<char> flac =
        ReadFile("RT2AudioProbe/fixtures/tone660_stereo_s16.flac", ok);
    if (!ok || flac.empty())
        return Fail("cannot read fixtures/tone660_stereo_s16.flac");
    const std::vector<char> mp3 =
        ReadFile("RT2AudioProbe/fixtures/tone660_stereo_48k.mp3", ok);
    if (!ok || mp3.empty())
        return Fail("cannot read fixtures/tone660_stereo_48k.mp3");
    const std::vector<char> truncated =
        ReadFile("RT2AudioProbe/fixtures/corrupt_truncated.wav", ok);
    if (!ok || truncated.empty())
        return Fail("cannot read fixtures/corrupt_truncated.wav");
    const std::vector<char> magic =
        ReadFile("RT2AudioProbe/fixtures/corrupt_magic.bin", ok);
    if (!ok || magic.empty())
        return Fail("cannot read fixtures/corrupt_magic.bin");

    std::unordered_map<std::string, AudioClipBytes> byteMap;
    const std::string kMono = KeyFor(backend, "asset:mono440", "clips/mono440.wav", monoF32);
    const std::string kStereo = KeyFor(backend, "asset:stereo660", "clips/stereo660.wav", stereoF32);
    const std::string kStereoS16 =
        KeyFor(backend, "asset:stereo660s16", "clips/stereo660s16.wav", stereoS16);
    const std::string kFlac = KeyFor(backend, "asset:stereoflac", "clips/stereo.flac", flac);
    const std::string kMp3 = KeyFor(backend, "asset:stereomp3", "clips/stereo.mp3", mp3);
    byteMap[kMono] = AudioClipBytes{ kMono,
                                     std::make_shared<const std::vector<char>>(monoF32) };
    byteMap[kStereo] = AudioClipBytes{ kStereo,
                                       std::make_shared<const std::vector<char>>(stereoF32) };
    byteMap[kStereoS16] = AudioClipBytes{ kStereoS16,
                                          std::make_shared<const std::vector<char>>(stereoS16) };
    byteMap[kFlac] = AudioClipBytes{ kFlac,
                                     std::make_shared<const std::vector<char>>(flac) };
    byteMap[kMp3] = AudioClipBytes{ kMp3,
                                    std::make_shared<const std::vector<char>>(mp3) };
    backend.SetClipByteResolver(
        [&byteMap](const std::string& key) -> rt2::core::Result<AudioClipBytes> {
            auto it = byteMap.find(key);
            if (it == byteMap.end())
                return rt2::core::Result<AudioClipBytes>::Fail(
                    rt2::core::Error::MissingAsset, key, "probe has no bytes for this key");
            return rt2::core::Result<AudioClipBytes>::Ok(it->second);
        });

    auto fetchRegister = [&](const std::string& key, BackendClipHandle& handle,
                             std::shared_ptr<const DecodedAudioGeneration>& gen) -> bool {
        auto fetched = backend.FetchDecodedGeneration(key);
        if (!fetched.IsOk())
            return false;
        gen = fetched.value;
        auto registered = backend.RegisterDecodedGeneration(gen);
        if (!registered.IsOk())
            return false;
        handle = registered.value;
        return true;
    };

    const AudioSessionId kSession{ 1 };

    // ---- S2: WAV mono f32 ----
    std::shared_ptr<const DecodedAudioGeneration> monoGen;
    {
        auto decoded = backend.FetchDecodedGeneration(kMono);
        if (!decoded.IsOk())
            return Fail("WAV mono decode failed");
        monoGen = decoded.value;
        if (monoGen->channels != 1 || monoGen->sampleRate != 48000 ||
            monoGen->frameCount != 48000)
            return Fail("WAV mono generation has wrong format/count");
        // Determinism: a second decode of the same bytes is bit-identical.
        AudioClipBytes same{ kMono,
                             std::make_shared<const std::vector<char>>(monoF32) };
        auto decoded2 = backend.DecodeClip(same);
        if (!decoded2.IsOk() ||
            decoded2.value->pcmInterleaved != monoGen->pcmInterleaved)
            return Fail("WAV mono re-decode is not bit-identical");
        // Sine shape without libm coupling: 440 Hz -> 880 crossings/s.
        std::vector<float> stereo(monoGen->frameCount * 2);
        for (uint32_t f = 0; f < monoGen->frameCount; ++f)
        {
            stereo[f * 2 + 0] = monoGen->pcmInterleaved[f];
            stereo[f * 2 + 1] = monoGen->pcmInterleaved[f];
        }
        const size_t crossings = ZeroCrossings(stereo.data(), monoGen->frameCount, 0);
        if (crossings < 878 || crossings > 882)
            return FailDetail("WAV mono crossings=" + std::to_string(crossings) +
                              ", want 880+-2");
        const float peak = MaxAbs(monoGen->pcmInterleaved.data(),
                                  monoGen->pcmInterleaved.size());
        if (peak < 0.49f || peak > 0.51f)
            return Fail("WAV mono peak is not 0.5");
        // Near-exact sample oracle against double-precision sine (libm may
        // differ by 1 ulp; tolerance is 1e-6, far below any content error).
        float worst = 0.0f;
        for (uint32_t f = 0; f < monoGen->frameCount; ++f)
        {
            const double t = static_cast<double>(f) / 48000.0;
            const float want =
                static_cast<float>(0.5 * std::sin(2.0 * 3.141592653589793 * 440.0 * t));
            const float diff = std::fabs(monoGen->pcmInterleaved[f] - want);
            if (diff > worst)
                worst = diff;
        }
        if (worst > 1e-6f)
            return Fail("WAV mono samples deviate from the sine oracle");
    }
    Pass("S2 WAV mono decode (deterministic, 48k x1, 440 Hz)");

    // ---- S3: WAV stereo f32 ----
    std::shared_ptr<const DecodedAudioGeneration> stereoGen;
    {
        auto decoded = backend.FetchDecodedGeneration(kStereo);
        if (!decoded.IsOk())
            return Fail("WAV stereo decode failed");
        stereoGen = decoded.value;
        if (stereoGen->channels != 2 || stereoGen->sampleRate != 48000 ||
            stereoGen->frameCount != 48000)
            return Fail("WAV stereo generation has wrong format/count");
        const float* pcm = stereoGen->pcmInterleaved.data();
        // L carries the 440 Hz tone: matches the mono fixture within 1 ulp.
        float worstL = 0.0f;
        for (uint32_t f = 0; f < stereoGen->frameCount; ++f)
        {
            const float diff = std::fabs(pcm[f * 2 + 0] - monoGen->pcmInterleaved[f]);
            if (diff > worstL)
                worstL = diff;
        }
        if (worstL > 1e-6f)
            return Fail("WAV stereo L does not match the mono 440 Hz content");
        // R carries 660 Hz: 1320 crossings/s and differs from L.
        const size_t crossingsR = ZeroCrossings(pcm, stereoGen->frameCount, 1);
        if (crossingsR < 1318 || crossingsR > 1322)
            return FailDetail("WAV stereo R crossings=" + std::to_string(crossingsR) +
                              ", want 1320+-2");
        bool differs = false;
        for (uint32_t f = 0; f < stereoGen->frameCount; ++f)
        {
            if (pcm[f * 2 + 0] != pcm[f * 2 + 1])
            {
                differs = true;
                break;
            }
        }
        if (!differs)
            return Fail("WAV stereo channels are unexpectedly identical");
    }
    Pass("S3 WAV stereo decode (distinct 440 Hz L / 660 Hz R)");

    // ---- S4: FLAC lossless ----
    {
        auto decoded = backend.FetchDecodedGeneration(kFlac);
        if (!decoded.IsOk())
            return Fail("FLAC decode failed");
        auto s16 = backend.FetchDecodedGeneration(kStereoS16);
        if (!s16.IsOk())
            return Fail("s16 master decode failed");
        if (decoded.value->channels != 2 || decoded.value->frameCount != 48000)
            return Fail("FLAC generation has wrong channels/count");
        if (decoded.value->pcmInterleaved.size() != s16.value->pcmInterleaved.size())
            return Fail("FLAC/s16 PCM sizes differ");
        float worst = 0.0f;
        for (size_t i = 0; i < decoded.value->pcmInterleaved.size(); ++i)
        {
            const float diff = std::fabs(decoded.value->pcmInterleaved[i] -
                                         s16.value->pcmInterleaved[i]);
            if (diff > worst)
                worst = diff;
        }
        if (worst > 1e-7f)
            return FailDetail("FLAC decode differs from s16 master");
    }
    Pass("S4 FLAC lossless decode matches s16 master");

    // ---- S4b: zero-total FLAC is valid unknown length ----
    //
    // A STREAMINFO total of zero means unknown (RFC 9639), not corrupt:
    // the same audio bytes with zeroed total must decode to identical PCM
    // while a nonzero advertised total still refuses truncation (S6).
    {
        if (flac.size() < 26 || std::memcmp(flac.data(), "fLaC", 4) != 0 ||
            (flac[4] & 0x7F) != 0)
            return Fail("FLAC fixture has no leading STREAMINFO");
        std::vector<char> flacUnknown = flac;
        auto* patched = reinterpret_cast<unsigned char*>(flacUnknown.data());
        patched[21] &= 0xF0; // keep bps bits, clear total-high nibble
        patched[22] = patched[23] = patched[24] = patched[25] = 0;
        const std::string kFlacUnknown = ProductionAudioBackend::BuildClipKey(
            "asset:flacunknown", "clips/unknown.flac",
            ProductionAudioBackend::FingerprintBytes(flacUnknown.data(),
                                                     flacUnknown.size()),
            "f32le");
        AudioClipBytes unknownBytes{ kFlacUnknown,
                                     std::make_shared<const std::vector<char>>(flacUnknown) };
        auto decodedUnknown = backend.DecodeClip(unknownBytes);
        if (!decodedUnknown.IsOk())
            return Fail("zero-total FLAC refused as corrupt");
        if (decodedUnknown.value->channels != 2 || decodedUnknown.value->frameCount != 48000)
            return Fail("zero-total FLAC generation has wrong format/count");
        auto decodedKnown = backend.FetchDecodedGeneration(kFlac);
        if (!decodedKnown.IsOk() ||
            decodedKnown.value->pcmInterleaved != decodedUnknown.value->pcmInterleaved)
            return Fail("zero-total FLAC PCM differs from the advertised-total decode");
    }
    Pass("S4b zero-total FLAC is valid unknown length");

    // ---- S5: MP3 ----
    {
        auto decoded = backend.FetchDecodedGeneration(kMp3);
        if (!decoded.IsOk())
            return Fail("MP3 decode failed");
        if (decoded.value->channels != 2 || decoded.value->sampleRate != 48000)
            return Fail("MP3 generation has wrong channels/rate");
        if (decoded.value->frameCount < 40000 || decoded.value->frameCount > 54000)
            return FailDetail("MP3 frame count out of range");
        const float peak = MaxAbs(decoded.value->pcmInterleaved.data(),
                                  decoded.value->pcmInterleaved.size());
        if (peak < 0.1f)
            return Fail("MP3 decode carries no energy");
    }
    Pass("S5 MP3 decode (energy, range, 48k stereo)");

    // ---- S6: corrupt inputs refuse ----
    //
    // The READY Play contract refuses any corrupt persisted clip
    // atomically. A header that promises more frames than the bytes
    // deliver is corruption, not a shorter clip: the checked-in truncated
    // WAV (header claims the full second, data is cut) must refuse, as
    // must a mid-stream FLAC cut (STREAMINFO-advertised vs decoded).
    {
        const size_t before = backend.DecodedCacheEntryCount();
        AudioClipBytes badMagic{ KeyFor(backend, "asset:bad", "clips/bad.bin", magic),
                                 std::make_shared<const std::vector<char>>(magic) };
        const std::vector<char> emptyData =
            ReadFile("RT2AudioProbe/fixtures/corrupt_empty_data.wav", ok);
        if (!ok || emptyData.empty())
            return Fail("cannot read fixtures/corrupt_empty_data.wav");
        AudioClipBytes badEmpty{ KeyFor(backend, "asset:bad", "clips/empty.wav", emptyData),
                                 std::make_shared<const std::vector<char>>(emptyData) };
        AudioClipBytes badCut{ KeyFor(backend, "asset:bad", "clips/cut.wav", truncated),
                               std::make_shared<const std::vector<char>>(truncated) };
        auto r1 = backend.DecodeClip(badMagic);
        auto r2 = backend.DecodeClip(badEmpty);
        auto r3 = backend.DecodeClip(badCut);
        if (r1.IsOk() || r2.IsOk() || r3.IsOk())
            return Fail("corrupt input decoded without error");
        if (r1.error.code == rt2::core::Error::None ||
            r2.error.code == rt2::core::Error::None ||
            r3.error.code == rt2::core::Error::None)
            return Fail("corrupt refusal carries no typed error");
        if (r3.error.detail.find("advertises") == std::string::npos &&
            r3.error.detail.find("decoded") == std::string::npos &&
            r3.error.detail.find("size") == std::string::npos)
            return Fail("truncated refusal does not name the length mismatch");
        // Truncated FLAC: STREAMINFO advertises the full second while the
        // bytes stop mid-stream.
        const std::vector<char> flacCut(flac.begin(),
                                        flac.begin() + static_cast<ptrdiff_t>(flac.size() / 2));
        AudioClipBytes badFlac{ KeyFor(backend, "asset:bad", "clips/cut.flac", flacCut),
                                std::make_shared<const std::vector<char>>(flacCut) };
        auto r4 = backend.DecodeClip(badFlac);
        if (r4.IsOk())
            return Fail("truncated FLAC decoded without error");
        if (r4.error.code == rt2::core::Error::None)
            return Fail("truncated FLAC refusal carries no typed error");
        if (backend.DecodedCacheEntryCount() != before)
            return Fail("corrupt decode published a cache entry");
    }
    Pass("S6 corrupt inputs refuse with typed errors");

    // ---- S7: key mismatch / malformed keys refuse ----
    {
        AudioClipBytes swapped{ kStereo,
                                std::make_shared<const std::vector<char>>(monoF32) };
        if (backend.DecodeClip(swapped).IsOk())
            return Fail("fingerprint-mismatched bytes decoded without error");
        AudioClipBytes malformed{ "not-a-key",
                                  std::make_shared<const std::vector<char>>(monoF32) };
        if (backend.DecodeClip(malformed).IsOk())
            return Fail("malformed clip key decoded without error");
        AudioClipBytes empty{ kMono, std::make_shared<const std::vector<char>>() };
        if (backend.DecodeClip(empty).IsOk())
            return Fail("empty clip bytes decoded without error");
    }
    Pass("S7 fingerprint/key validation refuses loudly");

    // ---- S8: fetch dedupe ----
    {
        auto a = backend.FetchDecodedGeneration(kMono);
        auto b = backend.FetchDecodedGeneration(kMono);
        if (!a.IsOk() || !b.IsOk() || a.value.get() != b.value.get())
            return Fail("same-key fetch did not dedupe to one immutable object");
    }
    Pass("S8 same-key fetch dedupes");

    // ---- S9: old/new overlap on one path ----
    {
        const std::vector<char> monoS16file =
            ReadFile("RT2AudioProbe/fixtures/tone440_mono_s16.wav", ok);
        if (!ok || monoS16file.empty())
            return Fail("cannot read fixtures/tone440_mono_s16.wav");
        const std::string kOld = kMono; // f32 bytes, same id+path below
        const std::string kNew =
            KeyFor(backend, "asset:mono440", "clips/mono440.wav", monoS16file);
        if (kOld == kNew)
            return Fail("same-content keys unexpectedly collide");
        byteMap[kNew] = AudioClipBytes{ kNew,
                                        std::make_shared<const std::vector<char>>(monoS16file) };
        auto oldGen = backend.FetchDecodedGeneration(kOld);
        auto newGen = backend.FetchDecodedGeneration(kNew);
        if (!oldGen.IsOk() || !newGen.IsOk())
            return Fail("overlap fetch failed");
        if (oldGen.value.get() == newGen.value.get())
            return Fail("old/new generations unexpectedly share one object");
        if (oldGen.value->pcmInterleaved == newGen.value->pcmInterleaved)
            return Fail("old/new PCM unexpectedly identical");
        BackendClipHandle hOld, hNew;
        std::shared_ptr<const DecodedAudioGeneration> gOld, gNew;
        if (!fetchRegister(kOld, hOld, gOld) || !fetchRegister(kNew, hNew, gNew))
            return Fail("overlap register failed");
        if (hOld == hNew)
            return Fail("old/new handles unexpectedly identical");
        auto tOld = backend.StartVoice(hOld, MakeStart(kSession, AudioBus::Effects, 0.5f, 0.5f));
        auto tNew = backend.StartVoice(hNew, MakeStart(kSession, AudioBus::Effects, 0.5f, 0.5f));
        if (!tOld.IsOk() || !tNew.IsOk())
            return Fail("concurrent old/new voices failed to start");
        if (backend.ActiveVoicesForKey(kOld) != 1 || backend.ActiveVoicesForKey(kNew) != 1)
            return Fail("per-key live counts wrong during overlap");
        std::vector<float> mixed;
        bool renderOk = false;
        RenderAll(backend, mixed, 480, renderOk);
        if (!renderOk || MaxAbs(mixed.data(), mixed.size()) < 0.05f)
            return Fail("overlapped voices carry no energy");
        rt2::core::Error error;
        if (!backend.StopVoice(tOld.value, error))
            return Fail("old voice stop failed");
        // The old generation stays valid after its voice ends.
        if (gOld->frameCount != 48000 || gOld->pcmInterleaved != oldGen.value->pcmInterleaved)
            return Fail("old generation mutated after voice stop");
        if (!backend.ReleaseDecodedGeneration(hOld, error))
            return Fail("old handle release failed");
        RenderAll(backend, mixed, 480, renderOk);
        if (!renderOk || MaxAbs(mixed.data(), mixed.size()) < 0.05f)
            return Fail("new voice died with the old generation");
        if (!backend.StopVoice(tNew.value, error) ||
            !backend.ReleaseDecodedGeneration(hNew, error))
            return Fail("new voice cleanup failed");
    }
    Pass("S9 old/new fingerprint generations overlap then release");

    // ---- S10: register/release ----
    {
        auto fetched = backend.FetchDecodedGeneration(kMono);
        if (!fetched.IsOk())
            return Fail("S10 fetch failed");
        auto r1 = backend.RegisterDecodedGeneration(fetched.value);
        auto r2 = backend.RegisterDecodedGeneration(fetched.value);
        if (!r1.IsOk() || !r2.IsOk() || r1.value == r2.value)
            return Fail("double register must yield distinct handles");
        rt2::core::Error error;
        if (!backend.ReleaseDecodedGeneration(r1.value, error))
            return Fail("first handle release failed");
        if (backend.ReleaseDecodedGeneration(r1.value, error))
            return Fail("double release unexpectedly succeeded");
        BackendClipHandle garbage{ 0xDEADBEEF };
        if (backend.ReleaseDecodedGeneration(garbage, error))
            return Fail("unknown handle release unexpectedly succeeded");
        auto started = backend.StartVoice(r2.value, MakeStart(kSession, AudioBus::Effects,
                                                              0.5f, 0.5f));
        if (!started.IsOk())
            return Fail("S10 voice start failed");
        if (backend.ReleaseDecodedGeneration(r2.value, error))
            return Fail("live-handle release unexpectedly succeeded");
        if (error.code == rt2::core::Error::None)
            return Fail("live-handle refusal carries no typed error");
        if (!backend.StopVoice(started.value, error) ||
            !backend.ReleaseDecodedGeneration(r2.value, error))
            return Fail("S10 cleanup failed");
    }
    Pass("S10 register/release incl. live-handle refusal");

    // ---- S10b: registration budget enforcement ----
    //
    // Direct registrations count against the same bound as decoded ones
    // (the A3 provider seam can supply generations independently of
    // Fetch). A 10 MiB supplied generation under a 1 MiB budget fails
    // typed Io and pins nothing; an already-held object adds nothing
    // (identity dedupe); a fitting registration evicts only unpinned
    // cache entries.
    {
        // 10 MiB RED under a 1 MiB budget.
        ProductionBackendConfig regConfig;
        regConfig.forceNoDevice = true;
        regConfig.decodedCacheBudgetBytes = 1048576;
        ProductionAudioBackend reg;
        std::string errorText;
        if (!reg.Initialize(regConfig, errorText))
            return Fail("registration-budget backend init failed");
        auto big = MakeDC(0.25f, 2621440); // mono f32 = exactly 10 MiB
        auto rBig = reg.RegisterDecodedGeneration(big);
        if (rBig.IsOk())
            return Fail("10 MiB direct registration unexpectedly fit a 1 MiB budget");
        if (rBig.error.code != rt2::core::Error::Io)
            return Fail("over-budget registration is not typed Io");
        if (rBig.error.detail.find("requires") == std::string::npos ||
            rBig.error.detail.find("10485760") == std::string::npos ||
            rBig.error.detail.find("resident") == std::string::npos ||
            rBig.error.detail.find("budget") == std::string::npos)
            return Fail("over-budget registration does not name byte counts");
        if (reg.DecodedCacheResidentBytes() != 0 || reg.LiveVoiceCount() != 0)
            return Fail("refused registration pinned memory");
        reg.Shutdown();
    }
    {
        // Identity dedupe + evict-only-unpinned on registration.
        ProductionBackendConfig evConfig;
        evConfig.forceNoDevice = true;
        evConfig.decodedCacheBudgetBytes = 200000;
        ProductionAudioBackend ev;
        std::string errorText;
        if (!ev.Initialize(evConfig, errorText))
            return Fail("eviction backend init failed");
        ev.SetClipByteResolver(
            [&byteMap](const std::string& key) -> rt2::core::Result<AudioClipBytes> {
                auto it = byteMap.find(key);
                if (it == byteMap.end())
                    return rt2::core::Result<AudioClipBytes>::Fail(
                        rt2::core::Error::MissingAsset, key, "probe has no bytes for this key");
                return rt2::core::Result<AudioClipBytes>::Ok(it->second);
            });
        BackendClipHandle hA;
        {
            auto fetchedA = ev.FetchDecodedGeneration(kMono); // 192000 B
            if (!fetchedA.IsOk())
                return Fail("eviction fetch A failed");
            auto regA = ev.RegisterDecodedGeneration(fetchedA.value);
            if (!regA.IsOk())
                return Fail("eviction register A failed");
            hA = regA.value;
            // Same object twice: distinct handle, residency counted once.
            auto regA2 = ev.RegisterDecodedGeneration(fetchedA.value);
            if (!regA2.IsOk() || regA2.value == hA)
                return Fail("same-object re-register must yield a distinct handle");
            if (ev.DecodedCacheResidentBytes() != 192000)
                return Fail("residency double-counted one object");
            rt2::core::Error error;
            if (!ev.ReleaseDecodedGeneration(regA2.value, error))
                return Fail("second handle release failed");
        } // fetchedA dies here; hA alone pins A.
        // A fitting registration that needs eviction while A is pinned
        // must fail instead of exceeding the bound (192000 + 16000 > 200000).
        auto smallT = MakeDC(0.25f, 4000); // 16000 B
        if (ev.RegisterDecodedGeneration(smallT).IsOk())
            return Fail("registration unexpectedly evicted the pinned A");
        // Releasing the pin lets the same registration evict A and fit.
        rt2::core::Error error;
        if (!ev.ReleaseDecodedGeneration(hA, error))
            return Fail("eviction release A failed");
        auto rT = ev.RegisterDecodedGeneration(smallT);
        if (!rT.IsOk())
            return Fail("registration after unpin failed");
        if (ev.DecodedCacheEntryCount() != 0)
            return Fail("evicted entry census wrong");
        if (ev.DecodedCacheResidentBytes() != 16000)
            return Fail("post-eviction residency wrong");
        const AudioSessionId evSession{ 41 };
        auto tT = ev.StartVoice(rT.value, MakeStart(evSession, AudioBus::Effects, 0.5f, 0.5f));
        if (!tT.IsOk())
            return Fail("voice from registered generation failed");
        if (!ev.StopVoice(tT.value, error) ||
            !ev.ReleaseDecodedGeneration(rT.value, error))
            return Fail("S10b cleanup failed");
        ev.Shutdown();
    }
    Pass("S10b registration budget enforcement");

    // ---- S11: exact L/R transport + bus/master gains ----
    //
    // Gain transport is proven with a DC source (pipeline-invariant exact):
    // hard-left, center, attenuation, and bus/master halving through the
    // real engine groups. Sine fixtures prove real content flow in S2-S5
    // and steady-state energy elsewhere.
    {
        auto dcRegistered = backend.RegisterDecodedGeneration(MakeDC(0.25f));
        if (!dcRegistered.IsOk())
            return Fail("S11 DC register failed");
        const BackendClipHandle handle = dcRegistered.value;
        std::vector<float> out;
        bool renderOk = false;
        // Hard-left through the real engine path: R must be exactly zero,
        // L must equal the DC content exactly.
        auto tLeft = backend.StartVoice(handle, MakeStart(kSession, AudioBus::Effects, 1.0f, 0.0f));
        if (!tLeft.IsOk())
            return Fail("hard-left voice start failed");
        if (!Settle(backend))
            return Fail("hard-left settle failed");
        RenderAll(backend, out, 1024, renderOk);
        if (!renderOk || out.size() != 2048)
            return Fail("hard-left render failed");
        for (uint32_t f = 0; f < 1024; ++f)
        {
            if (out[f * 2 + 1] != 0.0f)
                return Fail("hard-left R is not exactly zero");
            if (out[f * 2 + 0] != 0.25f)
                return Fail("hard-left L does not equal the DC content");
        }
        // Hard-right mirror.
        BackendVoiceMix hardRight{ 0.0f, 1.0f, 1.0f };
        rt2::core::Error error;
        if (!backend.SetVoiceMix(tLeft.value, hardRight, error))
            return Fail("hard-right mix failed");
        if (!Settle(backend))
            return Fail("hard-right settle failed");
        RenderAll(backend, out, 1024, renderOk);
        if (!renderOk)
            return Fail("hard-right render failed");
        for (uint32_t f = 0; f < 1024; ++f)
        {
            if (out[f * 2 + 0] != 0.0f || out[f * 2 + 1] != 0.25f)
                return Fail("hard-right is not exactly mirrored");
        }
        backend.StopVoice(tLeft.value, error);
        // Center: L equals R per frame, both exactly half the DC content
        // (0.5 is a power of two: scaling is exact).
        auto tCenter =
            backend.StartVoice(handle, MakeStart(kSession, AudioBus::Effects, 0.5f, 0.5f));
        if (!tCenter.IsOk())
            return Fail("center voice start failed");
        if (!Settle(backend))
            return Fail("center settle failed");
        RenderAll(backend, out, 1024, renderOk);
        if (!renderOk)
            return Fail("center render failed");
        for (uint32_t f = 0; f < 1024; ++f)
        {
            if (out[f * 2 + 0] != out[f * 2 + 1] || out[f * 2 + 0] != 0.125f)
                return Fail("center is not exactly half the DC content");
        }
        // Attenuated distance-style mix (0.25/0.25): exact sixteenth.
        BackendVoiceMix atten{ 0.25f, 0.25f, 1.0f };
        if (!backend.SetVoiceMix(tCenter.value, atten, error))
            return Fail("attenuated mix failed");
        if (!Settle(backend))
            return Fail("attenuated settle failed");
        RenderAll(backend, out, 512, renderOk);
        if (!renderOk)
            return Fail("attenuated render failed");
        for (uint32_t f = 0; f < 512; ++f)
        {
            if (out[f * 2 + 0] != 0.0625f || out[f * 2 + 1] != 0.0625f)
                return Fail("attenuated content is not exactly 0.0625");
        }
        backend.StopVoice(tCenter.value, error);
        // Bus gain halves through the real group volume. Window-matched
        // DC voices render the same settled frames, one at bus 1.0 and one
        // at bus 0.5 (0.5 scalings are exact powers of two).
        auto tBusFull =
            backend.StartVoice(handle, MakeStart(kSession, AudioBus::Effects, 0.5f, 0.5f));
        if (!tBusFull.IsOk())
            return Fail("bus-gain voice start failed");
        if (!Settle(backend))
            return Fail("bus-gain settle failed");
        RenderAll(backend, out, 512, renderOk);
        if (!renderOk)
            return Fail("bus-gain reference render failed");
        for (uint32_t f = 0; f < 512; ++f)
        {
            if (out[f * 2 + 0] != 0.125f || out[f * 2 + 1] != 0.125f)
                return Fail("bus-gain reference is not exactly 0.125");
        }
        backend.StopVoice(tBusFull.value, error);
        if (!backend.SetBusGain(AudioBus::Effects, 0.5f, error))
            return Fail("bus gain set failed");
        auto tBusHalf =
            backend.StartVoice(handle, MakeStart(kSession, AudioBus::Effects, 0.5f, 0.5f));
        if (!tBusHalf.IsOk())
            return Fail("halved-bus voice start failed");
        if (!Settle(backend))
            return Fail("halved-bus settle failed");
        RenderAll(backend, out, 512, renderOk);
        if (!renderOk)
            return Fail("bus-gain render failed");
        backend.StopVoice(tBusHalf.value, error);
        if (!backend.SetBusGain(AudioBus::Effects, 1.0f, error))
            return Fail("bus gain restore failed");
        for (uint32_t f = 0; f < 512; ++f)
        {
            if (out[f * 2 + 0] != 0.0625f || out[f * 2 + 1] != 0.0625f)
                return Fail("bus gain did not halve exactly through the group");
        }
        // Master gain halves exactly once (child volumes untouched).
        if (!backend.SetBusGain(AudioBus::Master, 0.5f, error))
            return Fail("master gain set failed");
        auto tMaster =
            backend.StartVoice(handle, MakeStart(kSession, AudioBus::Effects, 0.5f, 0.5f));
        if (!tMaster.IsOk())
            return Fail("master-gain voice start failed");
        if (!Settle(backend))
            return Fail("master-gain settle failed");
        RenderAll(backend, out, 512, renderOk);
        if (!renderOk)
            return Fail("master-gain render failed");
        backend.StopVoice(tMaster.value, error);
        if (!backend.SetBusGain(AudioBus::Master, 1.0f, error))
            return Fail("master gain restore failed");
        for (uint32_t f = 0; f < 512; ++f)
        {
            if (out[f * 2 + 0] != 0.0625f || out[f * 2 + 1] != 0.0625f)
                return Fail("master gain did not halve exactly once");
        }
        // Invalid gains refuse loudly.
        auto tMixCheck =
            backend.StartVoice(handle, MakeStart(kSession, AudioBus::Effects, 0.5f, 0.5f));
        if (!tMixCheck.IsOk())
            return Fail("mix-check voice start failed");
        BackendVoiceMix bad{ std::numeric_limits<float>::quiet_NaN(), 0.5f, 1.0f };
        if (backend.SetVoiceMix(tMixCheck.value, bad, error) || error.code == rt2::core::Error::None)
            return Fail("NaN mix unexpectedly accepted");
        if (backend.SetBusGain(AudioBus::Effects, std::numeric_limits<float>::infinity(), error) ||
            error.code == rt2::core::Error::None)
            return Fail("infinite bus gain unexpectedly accepted");
        if (!backend.StopVoice(tMixCheck.value, error) ||
            !backend.ReleaseDecodedGeneration(handle, error))
            return Fail("S11 cleanup failed");
    }
    Pass("S11 exact L/R, attenuation, bus/master gains");

    // ---- S12: pitch path ----
    {
        BackendClipHandle handle;
        std::shared_ptr<const DecodedAudioGeneration> gen;
        if (!fetchRegister(kMono, handle, gen))
            return Fail("S12 register failed");
        BackendVoiceStart start = MakeStart(kSession, AudioBus::Effects, 0.5f, 0.5f);
        start.pitch = 2.0f;
        auto token = backend.StartVoice(handle, start);
        if (!token.IsOk())
            return Fail("pitched voice start failed");
        uint32_t engineFrames = 0;
        bool done = false;
        for (int i = 0; i < 40 && !done; ++i)
        {
            std::vector<float> chunk;
            bool chunkOk = false;
            const uint32_t got = RenderAll(backend, chunk, 4096, chunkOk);
            if (!chunkOk)
                return Fail("pitched pump failed");
            engineFrames += got;
            auto completions = backend.DrainCompletions(kSession);
            if (!completions.IsOk())
                return Fail("pitched drain failed");
            for (const auto& c : completions.value)
            {
                if (c.token == token.value &&
                    c.reason == BackendCompletionReason::Completed)
                    done = true;
            }
        }
        if (!done)
            return Fail("pitched voice never completed");
        if (engineFrames < 20000 || engineFrames > 28000)
            return FailDetail("pitch-2.0 engine frames out of range");
        rt2::core::Error error;
        if (!backend.ReleaseDecodedGeneration(handle, error))
            return Fail("S12 cleanup failed");
    }
    Pass("S12 pitch-2.0 completion range");

    // ---- S13: completion token + post-completion silence ----
    {
        BackendClipHandle handle;
        std::shared_ptr<const DecodedAudioGeneration> gen;
        if (!fetchRegister(kMono, handle, gen))
            return Fail("S13 register failed");
        auto token =
            backend.StartVoice(handle, MakeStart(kSession, AudioBus::Effects, 0.5f, 0.5f));
        if (!token.IsOk())
            return Fail("S13 voice start failed");
        bool completed = false;
        bool renderOk = false;
        for (int i = 0; i < 40 && !completed; ++i)
        {
            std::vector<float> chunk;
            RenderAll(backend, chunk, 4096, renderOk);
            if (!renderOk)
                return Fail("completion pump failed");
            auto completions = backend.DrainCompletions(kSession);
            if (!completions.IsOk())
                return Fail("completion drain failed");
            for (const auto& c : completions.value)
            {
                if (c.token != token.value)
                    return Fail("foreign completion in session drain");
                if (c.reason != BackendCompletionReason::Completed)
                    return Fail("unexpected completion reason");
                completed = true;
            }
        }
        if (!completed)
            return Fail("voice never completed");
        if (backend.LiveVoiceCount() != 0)
            return Fail("completed voice still live");
        if (!RenderReachedSilence(backend))
            return Fail("post-completion mixer never reached silence");
        rt2::core::Error error;
        if (!backend.ReleaseDecodedGeneration(handle, error))
            return Fail("S13 cleanup failed");
    }
    Pass("S13 completion token match + post-completion silence");

    // ---- S14: session pause / initialPaused / PauseVoice ----
    {
        BackendClipHandle handle;
        std::shared_ptr<const DecodedAudioGeneration> gen;
        if (!fetchRegister(kMono, handle, gen))
            return Fail("S14 register failed");
        auto token =
            backend.StartVoice(handle, MakeStart(kSession, AudioBus::Effects, 0.5f, 0.5f));
        if (!token.IsOk())
            return Fail("S14 voice start failed");
        std::vector<float> out;
        bool renderOk = false;
        RenderAll(backend, out, 480, renderOk);
        if (!renderOk || MaxAbs(out.data(), out.size()) < 0.05f)
            return Fail("pre-pause voice carries no energy");
        rt2::core::Error error;
        if (!backend.SetSessionPaused(kSession, true, error))
            return Fail("session pause failed");
        if (!RenderReachedSilence(backend))
            return Fail("paused session never reached sample-frozen silence");
        auto idle = backend.DrainCompletions(kSession);
        if (!idle.IsOk() || !idle.value.empty())
            return Fail("paused session produced completions");
        // A voice created while paused is initially frozen.
        BackendVoiceStart frozen = MakeStart(kSession, AudioBus::Effects, 0.5f, 0.5f);
        frozen.initialPaused = true;
        auto tFrozen = backend.StartVoice(handle, frozen);
        if (!tFrozen.IsOk())
            return Fail("initially-paused voice start failed");
        if (!RenderReachedSilence(backend))
            return Fail("paused-session render never reached silence");
        if (!backend.SetSessionPaused(kSession, false, error))
            return Fail("session resume failed");
        // The initialPaused voice stays frozen until explicitly resumed.
        RenderAll(backend, out, 480, renderOk);
        if (!renderOk || MaxAbs(out.data(), out.size()) < 0.05f)
            return Fail("resumed session carries no energy");
        if (!backend.PauseVoice(tFrozen.value, false, error))
            return Fail("explicit voice resume failed");
        RenderAll(backend, out, 480, renderOk);
        if (!renderOk || MaxAbs(out.data(), out.size()) < 0.05f)
            return Fail("explicitly resumed voice carries no energy");
        if (!backend.StopVoice(token.value, error) ||
            !backend.StopVoice(tFrozen.value, error) ||
            !backend.ReleaseDecodedGeneration(handle, error))
            return Fail("S14 cleanup failed");
    }
    Pass("S14 session pause freeze + initialPaused + resume");

    // ---- S15: StopSessionVoices census ----
    {
        BackendClipHandle handle;
        std::shared_ptr<const DecodedAudioGeneration> gen;
        if (!fetchRegister(kMono, handle, gen))
            return Fail("S15 register failed");
        auto t1 = backend.StartVoice(handle, MakeStart(kSession, AudioBus::Music, 0.5f, 0.5f));
        auto t2 = backend.StartVoice(handle, MakeStart(kSession, AudioBus::UI, 0.5f, 0.5f));
        if (!t1.IsOk() || !t2.IsOk())
            return Fail("S15 voice starts failed");
        if (backend.LiveVoiceCount() != 2)
            return Fail("S15 census wrong before stop");
        rt2::core::Error error;
        if (!backend.StopSessionVoices(kSession, error))
            return Fail("StopSessionVoices failed");
        if (backend.LiveVoiceCount() != 0)
            return Fail("session voices outlive StopSessionVoices");
        if (backend.StopVoice(t1.value, error))
            return Fail("stopped session token unexpectedly valid");
        if (!RenderReachedSilence(backend))
            return Fail("post-stop mixer never reached silence");
        if (!backend.ReleaseDecodedGeneration(handle, error))
            return Fail("S15 cleanup failed");
    }
    Pass("S15 StopSessionVoices census");

    // ---- S16: render validation matrix ----
    {
        constexpr float kSentinel = -1234.5f;
        std::vector<float> guarded(kNoDeviceMaxFramesPerRender * kNoDeviceChannels, kSentinel);
        // Uninitialized backend: hard failure, nothing published.
        {
            ProductionAudioBackend cold;
            AudioPcmWriteBuffer buf{ guarded.data(), guarded.size() };
            if (cold.RenderNoDeviceFrames(buf, 480).IsOk())
                return Fail("uninitialized render unexpectedly succeeded");
            if (!AllSentinel(guarded.data(), guarded.size(), kSentinel))
                return Fail("uninitialized render mutated caller storage");
        }
        // Null / zero / oversized / undersized: typed errors, no mutation.
        {
            AudioPcmWriteBuffer nullBuf{ nullptr, 0 };
            if (backend.RenderNoDeviceFrames(nullBuf, 480).IsOk())
                return Fail("null buffer unexpectedly accepted");
            AudioPcmWriteBuffer buf{ guarded.data(), guarded.size() };
            if (backend.RenderNoDeviceFrames(buf, 0).IsOk())
                return Fail("zero-frame request unexpectedly accepted");
            if (backend.RenderNoDeviceFrames(buf, kNoDeviceMaxFramesPerRender + 1).IsOk())
                return Fail("oversized request unexpectedly accepted");
            AudioPcmWriteBuffer small{ guarded.data(), 100 };
            if (backend.RenderNoDeviceFrames(small, 480).IsOk())
                return Fail("undersized buffer unexpectedly accepted");
            if (!AllSentinel(guarded.data(), guarded.size(), kSentinel))
                return Fail("refused render mutated caller storage");
        }
        // Idle production render: the bus groups keep the graph attached,
        // so an idle production engine renders full silent frames (unlike
        // the bare A1 adapter, which reports a 0-frame prefix with no
        // attachments). Zero-progress output is exact silence.
        {
            AudioPcmWriteBuffer buf{ guarded.data(), guarded.size() };
            auto idle = backend.RenderNoDeviceFrames(buf, 480);
            if (!idle.IsOk() || idle.value != 480)
                return Fail("idle pump must render full silent frames");
            for (uint32_t f = 0; f < 480; ++f)
            {
                if (guarded[f * 2 + 0] != 0.0f || guarded[f * 2 + 1] != 0.0f)
                    return Fail("idle production render is not silent");
            }
            if (!AllSentinel(guarded.data() + 480 * 2,
                             guarded.size() - 480 * 2, kSentinel))
                return Fail("idle render touched beyond its reported prefix");
        }
        // Post-shutdown: hard failure, nothing published.
        {
            ProductionBackendConfig scratchConfig;
            scratchConfig.forceNoDevice = true;
            ProductionAudioBackend scratch;
            std::string errorText;
            if (!scratch.Initialize(scratchConfig, errorText))
                return Fail("scratch backend init failed");
            scratch.Shutdown();
            std::fill(guarded.begin(), guarded.end(), kSentinel);
            AudioPcmWriteBuffer buf{ guarded.data(), guarded.size() };
            if (scratch.RenderNoDeviceFrames(buf, 480).IsOk())
                return Fail("post-shutdown render unexpectedly succeeded");
            if (!AllSentinel(guarded.data(), guarded.size(), kSentinel))
                return Fail("post-shutdown render mutated caller storage");
        }
    }
    Pass("S16 render validation, short-read prefix, hard-failure silence");

    // ---- S16b: injected short/hard render results ----
    //
    // The engine pads with silence instead of short-reading, so these
    // branches are forced with the render fault hook. ShortOnce performs
    // the real engine render and truncates only the reported prefix.
    // FailOnce is labeled simulation: it returns the typed failure before
    // the engine read, covering the production failure branch and
    // caller-buffer discipline — valid engine state cannot force a
    // miniaudio-internal read failure.
    {
        auto dcRegistered = backend.RegisterDecodedGeneration(MakeDC(0.25f));
        if (!dcRegistered.IsOk())
            return Fail("S16b DC register failed");
        const BackendClipHandle handle = dcRegistered.value;
        auto token =
            backend.StartVoice(handle, MakeStart(kSession, AudioBus::Effects, 0.5f, 0.5f));
        if (!token.IsOk())
            return Fail("S16b voice start failed");
        if (!Settle(backend))
            return Fail("S16b settle failed");
        constexpr float kSentinel = -1234.5f;
        std::vector<float> guarded(kNoDeviceMaxFramesPerRender * kNoDeviceChannels, kSentinel);
        // Injected short render: the real engine output is produced, but
        // only the first 100 frames are reported and published; the caller
        // tail stays untouched.
        backend.TestHook_SetRenderFault(ProductionAudioBackend::TestRenderFault::ShortOnce, 100);
        {
            AudioPcmWriteBuffer buf{ guarded.data(), guarded.size() };
            auto shortRes = backend.RenderNoDeviceFrames(buf, 480);
            if (!shortRes.IsOk() || shortRes.value != 100)
                return Fail("injected short render did not report its prefix");
            for (uint32_t f = 0; f < 100; ++f)
            {
                if (guarded[f * 2 + 0] != 0.125f || guarded[f * 2 + 1] != 0.125f)
                    return Fail("injected short prefix is not the settled stream");
            }
            if (!AllSentinel(guarded.data() + 100 * 2, guarded.size() - 100 * 2, kSentinel))
                return Fail("injected short render touched beyond its prefix");
        }
        // Injected hard failure: typed error, caller storage untouched.
        backend.TestHook_SetRenderFault(ProductionAudioBackend::TestRenderFault::FailOnce);
        {
            std::fill(guarded.begin(), guarded.end(), kSentinel);
            AudioPcmWriteBuffer buf{ guarded.data(), guarded.size() };
            auto hardRes = backend.RenderNoDeviceFrames(buf, 480);
            if (hardRes.IsOk())
                return Fail("injected hard failure unexpectedly succeeded");
            if (hardRes.error.code == rt2::core::Error::None)
                return Fail("injected hard failure carries no typed error");
            if (!AllSentinel(guarded.data(), guarded.size(), kSentinel))
                return Fail("injected hard failure mutated caller storage");
        }
        // Faults are single-shot: the next render is normal again.
        {
            std::fill(guarded.begin(), guarded.end(), kSentinel);
            AudioPcmWriteBuffer buf{ guarded.data(), guarded.size() };
            auto normal = backend.RenderNoDeviceFrames(buf, 480);
            if (!normal.IsOk() || normal.value != 480 || guarded[0] != 0.125f)
                return Fail("post-fault render did not resume normally");
        }
        rt2::core::Error error;
        if (!backend.StopVoice(token.value, error) ||
            !backend.ReleaseDecodedGeneration(handle, error))
            return Fail("S16b cleanup failed");
    }
    Pass("S16b injected short/hard render results");

    // ---- S17: hard-cap ReplaceVoice ----
    {
        ProductionBackendConfig capConfig;
        capConfig.forceNoDevice = true;
        capConfig.maxVoices = 1;
        ProductionAudioBackend capped;
        std::string errorText;
        if (!capped.Initialize(capConfig, errorText))
            return Fail("capped backend init failed");
        capped.SetClipByteResolver(
            [&byteMap](const std::string& key) -> rt2::core::Result<AudioClipBytes> {
                auto it = byteMap.find(key);
                if (it == byteMap.end())
                    return rt2::core::Result<AudioClipBytes>::Fail(
                        rt2::core::Error::MissingAsset, key, "probe has no bytes for this key");
                return rt2::core::Result<AudioClipBytes>::Ok(it->second);
            });
        auto dcRegistered = capped.RegisterDecodedGeneration(MakeDC(0.25f));
        if (!dcRegistered.IsOk())
            return Fail("capped DC register failed");
        const BackendClipHandle handle = dcRegistered.value;
        const AudioSessionId capSession{ 7 };
        auto tA = capped.StartVoice(handle, MakeStart(capSession, AudioBus::Effects, 0.5f, 0.5f));
        if (!tA.IsOk())
            return Fail("capped first start failed");
        if (capped.StartVoice(handle, MakeStart(capSession, AudioBus::Effects, 0.5f, 0.5f))
                .IsOk())
            return Fail("over-cap start unexpectedly succeeded");
        // Atomic steal within one capacity unit: peak never exceeds 1.
        auto tB = capped.ReplaceVoice(
            tA.value, handle, MakeStart(capSession, AudioBus::Effects, 0.25f, 0.75f));
        if (!tB.IsOk())
            return Fail("capped ReplaceVoice failed");
        if (capped.PeakLiveVoices() != 1 || capped.LiveVoiceCount() != 1)
            return Fail("ReplaceVoice exceeded the hard cap");
        // Audible start + audible replace = two sound starts.
        if (capped.SoundStartCallCount() != 2)
            return Fail("audible replace did not start its sound exactly once");
        rt2::core::Error error;
        if (capped.StopVoice(tA.value, error))
            return Fail("victim token unexpectedly valid after replace");
        std::vector<float> out;
        bool renderOk = false;
        if (!Settle(capped))
            return Fail("post-replace settle failed");
        RenderAll(capped, out, 512, renderOk);
        if (!renderOk)
            return Fail("post-replace render failed");
        // Replacement carries the committed asymmetric DC mix: L is exactly
        // 0.0625, R exactly 0.1875 (same multiplies the voice performs).
        bool audible = MaxAbs(out.data(), out.size()) > 0.02f;
        if (!audible)
            return Fail("replacement voice is not audible");
        for (uint32_t f = 0; f < 512; ++f)
        {
            if (out[f * 2 + 0] != 0.0625f || out[f * 2 + 1] != 0.1875f)
                return Fail("replacement mix is not the committed asymmetric pair");
        }
        // Failed preparation preserves the victim: unknown clip, bad gains.
        BackendClipHandle garbage{ 0xBEEF };
        if (capped.ReplaceVoice(tB.value, garbage,
                                MakeStart(capSession, AudioBus::Effects, 0.5f, 0.5f))
                .IsOk())
            return Fail("replace with unknown clip unexpectedly succeeded");
        BackendVoiceMix badMix{ -1.0f, 0.5f, 1.0f };
        if (capped.SetVoiceMix(tB.value, badMix, error) || error.code == rt2::core::Error::None)
            return Fail("negative mix unexpectedly accepted");
        RenderAll(capped, out, 256, renderOk);
        if (!renderOk || MaxAbs(out.data(), out.size()) < 0.02f)
            return Fail("victim did not survive failed preparation");
        if (!capped.StopVoice(tB.value, error))
            return Fail("replacement stop failed");
        capped.Shutdown();
    }
    Pass("S17 hard-cap ReplaceVoice (one-unit commit, victim preserved)");

    // ---- S17b: paused replacement stays silent under concurrent render ----
    //
    // The hardware callback can render between any two main-thread
    // operations, so a paused ReplaceVoice that published audible gain
    // before stopping its prepared sound would leak audible frames. A
    // reader thread renders continuously while the main thread chains
    // paused replacements; every frame must be exactly silent. (This
    // mirrors standard device usage: the engine mixes on the reader while
    // the main thread starts/stops/destroys sounds.)
    {
        ProductionBackendConfig pauseConfig;
        pauseConfig.forceNoDevice = true;
        pauseConfig.maxVoices = 4;
        ProductionAudioBackend paused;
        std::string errorText;
        if (!paused.Initialize(pauseConfig, errorText))
            return Fail("paused backend init failed");
        auto dcRegistered = paused.RegisterDecodedGeneration(MakeDC(0.25f));
        if (!dcRegistered.IsOk())
            return Fail("S17b DC register failed");
        const BackendClipHandle handle = dcRegistered.value;
        const AudioSessionId pauseSession{ 21 };
        rt2::core::Error error;
        if (!paused.SetSessionPaused(pauseSession, true, error))
            return Fail("S17b session pause failed");
        BackendVoiceStart first = MakeStart(pauseSession, AudioBus::Effects, 0.5f, 0.5f);
        first.initialPaused = false; // session pause alone must freeze it
        auto current = paused.StartVoice(handle, first);
        if (!current.IsOk())
            return Fail("S17b first start failed");
        // Frozen at creation: the session pause means no sound start.
        if (paused.SoundStartCallCount() != 0)
            return Fail("paused start unexpectedly started its sound");

        std::atomic<bool> stopReader{ false };
        std::atomic<uint64_t> badFrames{ 0 };
        std::atomic<uint64_t> renderedFrames{ 0 };
        std::thread reader([&]() {
            std::vector<float> chunk(256 * kNoDeviceChannels);
            for (;;)
            {
                AudioPcmWriteBuffer buf{ chunk.data(), chunk.size() };
                auto res = paused.RenderNoDeviceFrames(buf, 256);
                if (!res.IsOk() || res.value > 256)
                {
                    badFrames.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                for (uint32_t f = 0; f < res.value; ++f)
                {
                    if (chunk[f * 2 + 0] != 0.0f || chunk[f * 2 + 1] != 0.0f)
                        badFrames.fetch_add(1, std::memory_order_relaxed);
                }
                renderedFrames.fetch_add(res.value, std::memory_order_relaxed);
                if (stopReader.load(std::memory_order_acquire))
                    return;
            }
        });
        bool replaceOk = true;
        for (int i = 0; i < 200 && replaceOk; ++i)
        {
            BackendVoiceStart next =
                MakeStart(pauseSession, AudioBus::Effects, 0.5f, 0.5f);
            next.initialPaused = (i % 2 == 0); // alternate both pause paths
            auto replaced = paused.ReplaceVoice(current.value, handle, next);
            if (!replaced.IsOk())
                replaceOk = false;
            else
                current = replaced;
        }
        stopReader.store(true, std::memory_order_release);
        reader.join();
        if (!replaceOk)
            return Fail("paused replacement chain failed");
        // Never-start proof (finding R1): 200 paused replacements chained
        // under concurrent rendering without a single sound start, so no
        // audio callback could have been in flight when gains published.
        // Combined with the all-silence render census above and the exact
        // resumed gains below, the Pause/Step blip is excluded by
        // construction, not just unobserved.
        if (paused.SoundStartCallCount() != 0)
            return Fail("paused replacement started its sound");
        if (badFrames.load() != 0)
            return Fail("paused replacement leaked audible frames under concurrent render");
        if (renderedFrames.load() == 0)
            return Fail("concurrent reader rendered nothing");
        if (paused.LiveVoiceCount() != 1)
            return Fail("paused replacement chain census wrong");
        // The surviving voice is frozen: deterministically silent, then
        // audible after resume (proves it is a real voice, not dead).
        std::vector<float> out;
        bool renderOk = false;
        RenderAll(paused, out, 1024, renderOk);
        if (!renderOk || !AllZero(out.data(), out.size()))
            return Fail("surviving paused voice is not frozen");
        if (!paused.SetSessionPaused(pauseSession, false, error))
            return Fail("S17b resume failed");
        // Resume starts the one surviving frozen voice exactly once.
        if (paused.SoundStartCallCount() != 1)
            return Fail("resume did not start the surviving voice exactly once");
        if (!Settle(paused))
            return Fail("S17b resume settle failed");
        RenderAll(paused, out, 512, renderOk);
        if (!renderOk || MaxAbs(out.data(), out.size()) < 0.05f)
            return Fail("resumed replacement carries no energy");
        if (!paused.StopVoice(current.value, error))
            return Fail("S17b cleanup stop failed");
        paused.Shutdown();
    }
    Pass("S17b paused replacement stays silent under concurrent render");

    // ---- S18: over-budget decode ----
    {
        ProductionBackendConfig tinyConfig;
        tinyConfig.forceNoDevice = true;
        tinyConfig.decodedCacheBudgetBytes = 1024;
        ProductionAudioBackend tiny;
        std::string errorText;
        if (!tiny.Initialize(tinyConfig, errorText))
            return Fail("tiny backend init failed");
        tiny.SetClipByteResolver(
            [&byteMap](const std::string& key) -> rt2::core::Result<AudioClipBytes> {
                auto it = byteMap.find(key);
                if (it == byteMap.end())
                    return rt2::core::Result<AudioClipBytes>::Fail(
                        rt2::core::Error::MissingAsset, key, "probe has no bytes for this key");
                return rt2::core::Result<AudioClipBytes>::Ok(it->second);
            });
        auto over = tiny.FetchDecodedGeneration(kMono);
        if (over.IsOk())
            return Fail("over-budget decode unexpectedly succeeded");
        if (over.error.code != rt2::core::Error::Io)
            return Fail("over-budget failure is not typed Io");
        const std::string detail = over.error.detail;
        if (detail.find(kMono) == std::string::npos ||
            detail.find("requires") == std::string::npos ||
            detail.find("resident") == std::string::npos ||
            detail.find("budget") == std::string::npos)
            return Fail("over-budget failure does not name asset and byte counts");
        if (tiny.DecodedCacheEntryCount() != 0)
            return Fail("over-budget decode published a cache entry");
        tiny.Shutdown();
    }
    Pass("S18 over-budget decode names asset and byte counts");

    // ---- S18b: pinned generations count against the budget ----
    //
    // Two 1 MiB clips under a 1 MiB budget. Fetching and registering A
    // pins its full PCM; fetching B must then fail loudly instead of
    // evicting A (whose bytes stay resident through the handle) and
    // exceeding the bound. After the registration is released, B fits by
    // evicting the now-unpinned A, and the stale A handle refuses loudly.
    {
        constexpr uint32_t kBigFrames = 262144; // mono f32 = exactly 1 MiB
        const std::vector<char> wavA = BuildWavF32MonoDC(0.25f, kBigFrames);
        const std::vector<char> wavB = BuildWavF32MonoDC(0.5f, kBigFrames);
        const std::string kBigA = ProductionAudioBackend::BuildClipKey(
            "asset:bigA", "clips/bigA.wav",
            ProductionAudioBackend::FingerprintBytes(wavA.data(), wavA.size()), "f32le");
        const std::string kBigB = ProductionAudioBackend::BuildClipKey(
            "asset:bigB", "clips/bigB.wav",
            ProductionAudioBackend::FingerprintBytes(wavB.data(), wavB.size()), "f32le");
        ProductionBackendConfig mibConfig;
        mibConfig.forceNoDevice = true;
        mibConfig.decodedCacheBudgetBytes = 1048576;
        ProductionAudioBackend mib;
        std::string errorText;
        if (!mib.Initialize(mibConfig, errorText))
            return Fail("MiB backend init failed");
        mib.SetClipByteResolver(
            [&wavA, &wavB, &kBigA, &kBigB](const std::string& key)
                -> rt2::core::Result<AudioClipBytes> {
                if (key == kBigA)
                    return rt2::core::Result<AudioClipBytes>::Ok(
                        AudioClipBytes{ kBigA,
                                        std::make_shared<const std::vector<char>>(wavA) });
                if (key == kBigB)
                    return rt2::core::Result<AudioClipBytes>::Ok(
                        AudioClipBytes{ kBigB,
                                        std::make_shared<const std::vector<char>>(wavB) });
                return rt2::core::Result<AudioClipBytes>::Fail(
                    rt2::core::Error::MissingAsset, key, "probe has no bytes for this key");
            });
        BackendClipHandle hA;
        {
            auto fetchedA = mib.FetchDecodedGeneration(kBigA);
            if (!fetchedA.IsOk() || fetchedA.value->frameCount != kBigFrames)
                return Fail("MiB fetch A failed");
            auto registeredA = mib.RegisterDecodedGeneration(fetchedA.value);
            if (!registeredA.IsOk())
                return Fail("MiB register A failed");
            hA = registeredA.value;
        } // fetchedA reference dies here; the registration pins A.
        if (mib.DecodedCacheResidentBytes() != 1048576)
            return Fail("MiB residency does not count the pinned generation");
        auto overB = mib.FetchDecodedGeneration(kBigB);
        if (overB.IsOk())
            return Fail("MiB fetch B unexpectedly evicted the pinned A");
        if (overB.error.code != rt2::core::Error::Io)
            return Fail("MiB pinned-budget failure is not typed Io");
        if (overB.error.detail.find(kBigB) == std::string::npos ||
            overB.error.detail.find("1048576") == std::string::npos)
            return Fail("MiB failure does not name asset and byte counts");
        if (mib.DecodedCacheEntryCount() != 1 ||
            mib.DecodedCacheResidentBytes() != 1048576)
            return Fail("MiB failed fetch mutated the cache census");
        // A voice still starts from the pinned handle while B is refused.
        const AudioSessionId mibSession{ 31 };
        auto tA = mib.StartVoice(hA, MakeStart(mibSession, AudioBus::Effects, 0.5f, 0.5f));
        if (!tA.IsOk())
            return Fail("MiB voice from pinned handle failed");
        rt2::core::Error error;
        if (!mib.StopVoice(tA.value, error))
            return Fail("MiB voice stop failed");
        // Releasing the pin lets B fit by evicting A; the stale A handle
        // then refuses loudly instead of addressing B's bytes.
        if (!mib.ReleaseDecodedGeneration(hA, error))
            return Fail("MiB release A failed");
        auto fetchedB = mib.FetchDecodedGeneration(kBigB);
        if (!fetchedB.IsOk())
            return Fail("MiB fetch B after unpin failed");
        if (mib.DecodedCacheEntryCount() != 1 ||
            mib.DecodedCacheResidentBytes() != 1048576)
            return Fail("MiB post-eviction census wrong");
        if (mib.StartVoice(hA, MakeStart(mibSession, AudioBus::Effects, 0.5f, 0.5f)).IsOk())
            return Fail("evicted handle unexpectedly started a voice");
        auto registeredB = mib.RegisterDecodedGeneration(fetchedB.value);
        if (!registeredB.IsOk())
            return Fail("MiB register B failed");
        auto tB = mib.StartVoice(registeredB.value,
                                 MakeStart(mibSession, AudioBus::Effects, 0.5f, 0.5f));
        if (!tB.IsOk())
            return Fail("MiB voice from B failed");
        if (!mib.StopVoice(tB.value, error) ||
            !mib.ReleaseDecodedGeneration(registeredB.value, error))
            return Fail("S18b cleanup failed");
        mib.Shutdown();
    }
    Pass("S18b pinned generations count against the budget");

    // ---- S19: torn-pair stress through the real block function ----
    {
        // Asymmetric pairs: any torn combination (A.L,B.R or B.L,A.R) is
        // observably distinct from both complete pairs.
        constexpr float kAL = 0.25f;
        constexpr float kAR = 0.75f;
        constexpr float kBL = 0.75f;
        constexpr float kBR = 0.125f;
        const uint64_t kPackA = PackStereoGain(kAL, kAR);
        const uint64_t kPackB = PackStereoGain(kBL, kBR);
        std::atomic<uint64_t> published{ kPackA };
        std::atomic<bool> writerDone{ false };
        constexpr int kBlocks = 60000;
        constexpr uint32_t kBlockFrames = 128;
        // Mono DC input of 1.0: block output equals the published pair
        // bit-exactly, so any torn pair is caught by exact comparison.
        std::vector<float> dcInput(kBlockFrames, 1.0f);
        uint64_t torn = 0;
        uint64_t countA = 0;
        uint64_t countB = 0;
        std::thread writer([&]() {
            for (int i = 0; i < 200000; ++i)
            {
                published.store((i % 2 == 0) ? kPackA : kPackB,
                                std::memory_order_release);
            }
            writerDone.store(true, std::memory_order_release);
        });
        std::vector<float> blockOut(kBlockFrames * 2);
        for (int b = 0; b < kBlocks; ++b)
        {
            // One acquire load per render block, exactly like the
            // audio-thread gain stage.
            float l = 0.0f;
            float r = 0.0f;
            UnpackStereoGain(published.load(std::memory_order_acquire), l, r);
            ProductionAudioBackend::ProcessStereoGainBlock(
                l, r, dcInput.data(), 1, blockOut.data(), kBlockFrames);
            const bool isA = (l == kAL && r == kAR);
            const bool isB = (l == kBL && r == kBR);
            if (!isA && !isB)
                ++torn;
            else
            {
                if (isA)
                    ++countA;
                else
                    ++countB;
                for (uint32_t f = 0; f < kBlockFrames; ++f)
                {
                    if (blockOut[f * 2 + 0] != l || blockOut[f * 2 + 1] != r)
                        ++torn;
                }
            }
            if (writerDone.load(std::memory_order_acquire) && (countA > 0 && countB > 0))
            {
                // Keep rendering a little past the writer to catch stragglers.
                if (b > kBlocks / 2)
                    break;
            }
        }
        writer.join();
        if (torn != 0)
            return FailDetail("torn L/R pair observed: " + std::to_string(torn));
        if (countA == 0 || countB == 0)
            return Fail("stress never observed both gain generations");
        std::printf("RT2AudioProbe: torn-pair stress saw A=%llu B=%llu torn=0 (TSan n/a on MSVC)\n",
                    (unsigned long long)countA, (unsigned long long)countB);
    }
    Pass("S19 packed L/R atomic never tears");

    // ---- S19b: SetVoiceMix stress through a real rendering voice ----
    //
    // S19 proves the primitive; this drives the production path: a writer
    // thread alternates two asymmetric pairs via SetVoiceMix on a live
    // voice while the main thread renders no-device blocks. Each audio
    // block loads the pair once, so with DC-1.0 input every rendered frame
    // must equal pair A or pair B bit-exactly — never a torn combination.
    // (TSan is unavailable on MSVC; the design performs one acquire load
    // per block and no allocation, I/O, or locking in the audio stage.)
    {
        auto dcRegistered = backend.RegisterDecodedGeneration(MakeDC(1.0f));
        if (!dcRegistered.IsOk())
            return Fail("S19b DC register failed");
        const BackendClipHandle handle = dcRegistered.value;
        constexpr float kSAL = 0.25f;
        constexpr float kSAR = 0.75f;
        constexpr float kSBL = 0.75f;
        constexpr float kSBR = 0.125f;
        auto token = [&]() {
            // Looping: the stress renders an unbounded frame count while
            // the writer runs, so a one-shot voice would exhaust into
            // post-completion silence (indistinguishable zeros, not tears).
            BackendVoiceStart looped = MakeStart(kSession, AudioBus::Effects, kSAL, kSAR);
            looped.loop = true;
            return backend.StartVoice(handle, looped);
        }();
        if (!token.IsOk())
            return Fail("S19b voice start failed");
        if (!Settle(backend))
            return Fail("S19b settle failed");
        std::atomic<bool> writerDone{ false };
        std::atomic<uint64_t> mixCalls{ 0 };
        std::thread writer([&]() {
            for (int i = 0; i < 20000; ++i)
            {
                BackendVoiceMix mix{ (i % 2 == 0) ? kSAL : kSBL,
                                     (i % 2 == 0) ? kSAR : kSBR, 1.0f };
                rt2::core::Error mixError;
                if (backend.SetVoiceMix(token.value, mix, mixError))
                    mixCalls.fetch_add(1, std::memory_order_relaxed);
            }
            writerDone.store(true, std::memory_order_release);
        });
        uint64_t torn = 0;
        uint64_t countA = 0;
        uint64_t countB = 0;
        std::vector<float> chunk(512 * kNoDeviceChannels);
        bool renderFailed = false;
        for (int guard = 0; guard < 20000 && !renderFailed; ++guard)
        {
            AudioPcmWriteBuffer buf{ chunk.data(), chunk.size() };
            auto res = backend.RenderNoDeviceFrames(buf, 512);
            if (!res.IsOk() || res.value != 512)
            {
                renderFailed = true;
                break;
            }
            for (uint32_t f = 0; f < 512; ++f)
            {
                const float l = chunk[f * 2 + 0];
                const float r = chunk[f * 2 + 1];
                const bool isA = (l == kSAL && r == kSAR);
                const bool isB = (l == kSBL && r == kSBR);
                if (isA)
                    ++countA;
                else if (isB)
                    ++countB;
                else
                {
                    ++torn;
                    if (torn <= 5)
                    {
                        std::fprintf(stderr,
                                     "RT2AudioProbe DIAG: torn frame l=%.9g r=%.9g\n",
                                     (double)l, (double)r);
                    }
                }
            }
            if (writerDone.load(std::memory_order_acquire) && countA > 0 && countB > 0)
                break;
        }
        writer.join();
        if (renderFailed)
            return Fail("S19b render failed");
        if (torn != 0)
            return FailDetail("torn L/R pair through real voice: " + std::to_string(torn));
        if (countA == 0 || countB == 0 || mixCalls.load() != 20000)
            return Fail("real-voice stress never observed both generations");
        std::printf("RT2AudioProbe: real-voice stress saw A=%llu B=%llu torn=0\n",
                    (unsigned long long)countA, (unsigned long long)countB);
        rt2::core::Error error;
        if (!backend.StopVoice(token.value, error) ||
            !backend.ReleaseDecodedGeneration(handle, error))
            return Fail("S19b cleanup failed");
    }
    Pass("S19b SetVoiceMix stress through a real rendering voice");

    // ---- S20: A5 semantic/render/accounting phase order, real PCM ----
    //
    // Drives AudioWorld's split phases against the production no-device
    // engine in controller order (semantic, render, advance). The semantic
    // phase drains first-frame autoplay and publishes mixes; only then is
    // PCM rendered; cursors advance by exactly what rendered. Rendering
    // before the semantic phase (the pre-fixup controller order) would
    // leave this first block silent while still advancing the cursor.
    // Runs on a fresh backend so earlier sections' voices cannot leak
    // into the silence oracle.
    {
        ProductionBackendConfig a5Config;
        a5Config.forceNoDevice = true;
        ProductionAudioBackend a5backend;
        std::string a5Error;
        if (!a5backend.Initialize(a5Config, a5Error))
            return Fail("S20 backend init failed");
        a5backend.SetClipByteResolver(
            [&byteMap](const std::string& key) -> rt2::core::Result<AudioClipBytes> {
                auto it = byteMap.find(key);
                if (it == byteMap.end())
                    return rt2::core::Result<AudioClipBytes>::Fail(
                        rt2::core::Error::MissingAsset, key,
                        "probe has no bytes for this key");
                return rt2::core::Result<AudioClipBytes>::Ok(it->second);
            });
        const AudioSessionId a5Session{ 77 };
        AudioWorld a5world(&a5backend, &a5backend, a5Session,
                           AudioOwnerKind::Runtime, AudioWorldConfig{});
        std::array<uint8_t, 16> a5IdBytes{};
        a5IdBytes[15] = 7;
        const rt2::core::UUID a5src(a5IdBytes);
        AudioPlayRequest staged;
        staged.source = a5src;
        staged.component.bus = AudioBus::Effects;
        staged.component.autoplay = true;
        staged.component.loop = false;
        staged.component.spatial = false;
        staged.component.gain = 1.0f;
        staged.component.pitch = 1.0f;
        staged.component.minDistance = 1.0f;
        staged.component.maxDistance = 30.0f;
        staged.component.rolloff = 1.0f;
        staged.component.priority = 128;
        staged.hasTransform = false;
        staged.clipKey = kMono;
        a5world.StageAutoplay(staged);
        if (a5world.StagedAutoplayCount() != 1)
            return Fail("S20 autoplay staging lost");
        AudioListenerPose listener; // identity: origin, -Z forward, +Y up
        a5world.UpdateSemantic(listener, nullptr, 0);
        if (a5world.LiveVoiceCount() != 1)
            return Fail("S20 first-frame autoplay produced no voice");
        if (a5world.StagedAutoplayCount() != 0)
            return Fail("S20 staging not consumed");
        std::vector<float> first(800 * kNoDeviceChannels, -1234.5f);
        AudioPcmWriteBuffer firstBuf{ first.data(), first.size() };
        auto firstRender = a5backend.RenderNoDeviceFrames(firstBuf, 800);
        if (!firstRender.IsOk() || firstRender.value != 800)
            return Fail("S20 first-block render failed");
        if (MaxAbs(first.data(), first.size()) < 0.05f)
            return Fail("S20 first autoplay block is silent (render ran before semantic)");
        a5world.AdvanceCursors(800);
        {
            auto voices = a5world.LiveVoicesForSource(a5src);
            if (voices.size() != 1)
                return Fail("S20 voice lost after first block");
            double cursor = 0.0;
            if (!a5world.GetVoiceCursor(voices[0], cursor) || cursor != 800.0)
                return Fail("S20 cursor did not advance by exactly what rendered");
        }
        // Same-frame Stop: the semantic phase stops the voice, so the
        // next rendered block carries no tone. The engine flushes a
        // single residual frame after a mid-stream sound destroy (observed
        // peak 0.33 on one stereo frame, then exact zeros), so the oracle
        // is energy-based with an exact-silence tail: under the pre-fixup
        // render-first order this block would carry the full tone (~100%
        // of the first block's energy).
        uint64_t stopSeq = 0;
        if (!a5world.QueueStop(a5src, stopSeq))
            return Fail("S20 stop queue refused");
        a5world.UpdateSemantic(listener, nullptr, 0);
        if (a5world.LiveVoiceCount() != 0)
            return Fail("S20 stop did not take effect in its own frame");
        std::vector<float> second(800 * kNoDeviceChannels, -1234.5f);
        AudioPcmWriteBuffer secondBuf{ second.data(), second.size() };
        auto secondRender = a5backend.RenderNoDeviceFrames(secondBuf, 800);
        if (!secondRender.IsOk() || secondRender.value != 800)
            return Fail("S20 post-stop render failed");
        double firstEnergy = 0.0;
        for (float s : first)
            firstEnergy += (double)s * (double)s;
        double secondEnergy = 0.0;
        for (float s : second)
            secondEnergy += (double)s * (double)s;
        if (!(secondEnergy < 0.01 * firstEnergy))
            return Fail("S20 post-stop block carries tone energy (stop not effective same frame)");
        if (MaxAbs(second.data() + 2, second.size() - 2) != 0.0f)
            return Fail("S20 post-stop tail is not exactly silent");
        a5world.AdvanceCursors(800); // no live voices: must not crash
        rt2::core::Error shutdownError;
        if (!a5world.Shutdown(shutdownError))
            return Fail("S20 world shutdown failed");
        if (a5world.LiveVoiceCount() != 0 ||
            a5world.CachedClipGenerationCount() != 0)
            return Fail("S20 shutdown census not at baseline");
        a5backend.Shutdown();
    }
    Pass("S20 A5 semantic/render/accounting phase order with real PCM");

    // ---- S20b: short one-shot settles same-frame, survivor advances ----
    //
    // A 100-frame one-shot plus a looping tone rendered in one 800-frame
    // no-device block: post-render reconciliation reaps the finished voice
    // (Completed status, zero census for its key) before the frame
    // returns, while the survivor advances by exactly the rendered block.
    // A second semantic pass (the next frame's script window) observes the
    // same settled state with no resurrection.
    {
        ProductionBackendConfig shortConfig;
        shortConfig.forceNoDevice = true;
        ProductionAudioBackend shortBackend;
        std::string shortError;
        if (!shortBackend.Initialize(shortConfig, shortError))
            return Fail("S20b backend init failed");
        const std::vector<char> wavShort = BuildWavF32MonoDC(0.25f, 100);
        const std::string kShort = ProductionAudioBackend::BuildClipKey(
            "asset:short100", "clips/short100.wav",
            ProductionAudioBackend::FingerprintBytes(wavShort.data(), wavShort.size()),
            "f32le");
        shortBackend.SetClipByteResolver(
            [&wavShort, &kShort, &byteMap](
                const std::string& key) -> rt2::core::Result<AudioClipBytes> {
                if (key == kShort)
                    return rt2::core::Result<AudioClipBytes>::Ok(
                        AudioClipBytes{ kShort,
                                        std::make_shared<const std::vector<char>>(wavShort) });
                auto it = byteMap.find(key);
                if (it == byteMap.end())
                    return rt2::core::Result<AudioClipBytes>::Fail(
                        rt2::core::Error::MissingAsset, key,
                        "probe has no bytes for this key");
                return rt2::core::Result<AudioClipBytes>::Ok(it->second);
            });
        const AudioSessionId shortSession{ 78 };
        AudioWorld shortWorld(&shortBackend, &shortBackend, shortSession,
                              AudioOwnerKind::Runtime, AudioWorldConfig{});
        std::array<uint8_t, 16> shortId{};
        shortId[15] = 8;
        const rt2::core::UUID shortSrc(shortId);
        std::array<uint8_t, 16> longId{};
        longId[15] = 9;
        const rt2::core::UUID longSrc(longId);
        auto stageAuto = [&](const rt2::core::UUID& src, const std::string& key,
                             bool loop) {
            AudioPlayRequest req;
            req.source = src;
            req.component.bus = AudioBus::Effects;
            req.component.autoplay = true;
            req.component.loop = loop;
            req.component.spatial = false;
            req.component.gain = 1.0f;
            req.component.pitch = 1.0f;
            req.component.minDistance = 1.0f;
            req.component.maxDistance = 30.0f;
            req.component.rolloff = 1.0f;
            req.component.priority = 128;
            req.hasTransform = false;
            req.clipKey = key;
            shortWorld.StageAutoplay(req);
        };
        stageAuto(shortSrc, kShort, false);
        stageAuto(longSrc, kMono, true);
        AudioListenerPose shortListener;
        shortWorld.UpdateSemantic(shortListener, nullptr, 0);
        if (shortWorld.LiveVoiceCount() != 2)
            return Fail("S20b autoplay did not start both voices");
        std::vector<float> block(800 * kNoDeviceChannels, -1234.5f);
        AudioPcmWriteBuffer blockBuf{ block.data(), block.size() };
        auto blockRender = shortBackend.RenderNoDeviceFrames(blockBuf, 800);
        if (!blockRender.IsOk() || blockRender.value != 800)
            return Fail("S20b block render failed");
        shortWorld.ReconcilePostRender();
        shortWorld.AdvanceCursors(800);
        if (!shortWorld.LiveVoicesForSource(shortSrc).empty())
            return Fail("S20b finished one-shot still live after reconcile");
        if (shortWorld.GetSourceStatus(shortSrc).aggregate !=
            AudioSourceAggregate::Completed)
            return Fail("S20b finished one-shot status is not Completed");
        if (shortBackend.ActiveVoicesForKey(kShort) != 0 ||
            shortBackend.LiveVoiceCount() != 1)
            return Fail("S20b backend census did not settle to the survivor");
        {
            auto survivors = shortWorld.LiveVoicesForSource(longSrc);
            if (survivors.size() != 1)
                return Fail("S20b survivor loop lost");
            double cursor = 0.0;
            if (!shortWorld.GetVoiceCursor(survivors[0], cursor) || cursor != 800.0)
                return Fail("S20b survivor did not advance by the rendered block");
        }
        // Next frame: settled state persists, survivor continues.
        shortWorld.UpdateSemantic(shortListener, nullptr, 0);
        if (!shortWorld.LiveVoicesForSource(shortSrc).empty() ||
            shortWorld.GetSourceStatus(shortSrc).aggregate !=
                AudioSourceAggregate::Completed)
            return Fail("S20b settled state did not persist to the next frame");
        shortWorld.AdvanceCursors(800);
        {
            auto survivors = shortWorld.LiveVoicesForSource(longSrc);
            double cursor = 0.0;
            if (survivors.size() != 1 ||
                !shortWorld.GetVoiceCursor(survivors[0], cursor) || cursor != 1600.0)
                return Fail("S20b survivor did not continue to 1600");
        }
        rt2::core::Error shortShutdownError;
        if (!shortWorld.Shutdown(shortShutdownError))
            return Fail("S20b world shutdown failed");
        shortBackend.Shutdown();
    }
    Pass("S20b short one-shot settles same-frame, survivor advances");

    // ---- S21: shipped acceptance scene through the production path ----
    //
    // P1/P2 closure for A8: every value below (clip paths, asset IDs,
    // buses, autoplay/loop/spatial/gain, poses) is parsed from the
    // checked-in RT2App/assets/audio-acceptance.rt2scene. The clips resolve
    // through production AssetResolver::Resolve (real path+sidecar
    // verification, no database), decode from their real on-disk bytes
    // through ProductionAudioBackend, play through AudioWorld, and render
    // exact no-device PCM. Removing/corrupting a WAV, breaking a sidecar,
    // or editing the scene's audio blocks turns this section red; the
    // path-scripted fake suites cannot observe any of those mutations.
    {
        bool sceneFileOk = false;
        const std::vector<char> sceneBytes =
            ReadFile("RT2App/assets/audio-acceptance.rt2scene", sceneFileOk);
        if (!sceneFileOk)
            return Fail("S21 cannot read shipped acceptance scene (run from repository root)");
        nlohmann::json scene;
        try
        {
            scene = nlohmann::json::parse(sceneBytes.begin(), sceneBytes.end());
        }
        catch (...)
        {
            return Fail("S21 shipped scene is not valid JSON");
        }
        uint32_t sceneVersion = 0;
        try
        {
            sceneVersion = scene.at("version").get<uint32_t>();
            const nlohmann::json& entities = scene.at("entities");
            if (!entities.is_array() || entities.size() != 2)
                return Fail("S21 shipped scene must carry exactly 2 entities");
        }
        catch (...)
        {
            return Fail("S21 shipped scene has no version/entities array");
        }
        if (sceneVersion != 9)
            return Fail("S21 shipped scene is not schema v9");

        struct ShippedSource
        {
            std::string name;
            rt2::core::UUID uuid;
            rt2::core::UUID clipId;
            std::string clipPath;
            AudioBus bus = AudioBus::Effects;
            bool autoplay = false;
            bool loop = false;
            bool spatial = false;
            float gain = 1.0f;
            float pitch = 1.0f;
            float minDistance = 1.0f;
            float maxDistance = 30.0f;
            float rolloff = 1.0f;
            uint8_t priority = 128;
            float translation[3] = { 0.0f, 0.0f, 0.0f };
        };
        auto parseSource = [&](const char* wantName, ShippedSource& out) -> bool {
            try
            {
                for (const auto& e : scene.at("entities"))
                {
                    if (e.at("name").get<std::string>() != wantName)
                        continue;
                    out.name = wantName;
                    out.uuid = rt2::core::UUID::Parse(
                        e.at("uuid").get<std::string>());
                    const auto& a = e.at("audioSource");
                    const auto& clip = a.at("clip");
                    if (clip.at("kind").get<std::string>() != "audioclip")
                        return false;
                    out.clipPath = clip.at("path").get<std::string>();
                    out.clipId = rt2::core::UUID::Parse(
                        clip.at("assetId").get<std::string>());
                    if (!AudioBusFromName(a.at("bus").get<std::string>(), out.bus))
                        return false;
                    out.autoplay = a.at("autoplay").get<bool>();
                    out.loop = a.at("loop").get<bool>();
                    out.spatial = a.at("spatial").get<bool>();
                    out.gain = a.at("gain").get<float>();
                    out.pitch = a.at("pitch").get<float>();
                    out.minDistance = a.at("minDistance").get<float>();
                    out.maxDistance = a.at("maxDistance").get<float>();
                    out.rolloff = a.at("rolloff").get<float>();
                    out.priority =
                        static_cast<uint8_t>(a.at("priority").get<int>());
                    const auto& t = e.at("transform").at("translation");
                    out.translation[0] = t.at(0).get<float>();
                    out.translation[1] = t.at(1).get<float>();
                    out.translation[2] = t.at(2).get<float>();
                    return true;
                }
            }
            catch (...)
            {
            }
            return false;
        };
        ShippedSource loopSrc;
        ShippedSource musicSrc;
        if (!parseSource("LoopEmitter", loopSrc))
            return Fail("S21 shipped scene has no parseable LoopEmitter audioSource");
        if (!parseSource("MusicBed", musicSrc))
            return Fail("S21 shipped scene has no parseable MusicBed audioSource");
        // Shape the walk asserts on comes from the file, not constants:
        // exactly one looping spatial autoplay source and one idle
        // non-spatial source on distinct clips with distinct identities.
        if (!(loopSrc.autoplay && loopSrc.loop && loopSrc.spatial))
            return Fail("S21 LoopEmitter is not an autoplay spatial loop");
        if (musicSrc.autoplay || musicSrc.loop || musicSrc.spatial)
            return Fail("S21 MusicBed is not an idle non-spatial one-shot");
        if (musicSrc.bus != AudioBus::Music)
            return Fail("S21 MusicBed is not on the Music bus");
        if (loopSrc.clipPath == musicSrc.clipPath ||
            loopSrc.clipId.IsNull() || musicSrc.clipId.IsNull() ||
            loopSrc.clipId == musicSrc.clipId || loopSrc.uuid == musicSrc.uuid)
            return Fail("S21 shipped sources lack distinct clip identity");

        // Production resolution context: absolute asset root, no database.
        // Per the resolver contract this is the healthy path+sidecar case:
        // the real sidecar must agree with the scene asset ID and no
        // diagnostic may be emitted.
        rt2::core::AssetResolutionContext prodCtx;
        prodCtx.assetRoot = std::filesystem::absolute("RT2App/assets");
        prodCtx.database = nullptr;
        struct ResolvedClip
        {
            std::string key;
            std::vector<char> bytes;
            std::string canonical;
        };
        auto resolveShipped = [&](const ShippedSource& src,
                                  ResolvedClip& out) -> bool {
            AssetReference ref;
            ref.kind = AssetKind::AudioClip;
            ref.path = src.clipPath;
            ref.sourceKey = "";
            ref.assetId = src.clipId;
            std::vector<rt2::core::AssetDiagnostic> diags;
            rt2::core::AssetResolutionResult res =
                rt2::core::Resolve(ref, prodCtx, src.uuid, src.name, diags);
            if (!res.success)
                return false;
            if (!diags.empty())
                return false;
            if (res.effectiveId != src.clipId)
                return false;
            bool bytesOk = false;
            out.bytes = ReadFile(res.resolvedPath.string(), bytesOk);
            if (!bytesOk || out.bytes.empty())
                return false;
            out.canonical = res.resolvedPath.lexically_normal().generic_string();
            const uint64_t fp = ProductionAudioBackend::FingerprintBytes(
                out.bytes.data(), out.bytes.size());
            out.key = ProductionAudioBackend::BuildClipKey(
                res.effectiveId.ToString(), out.canonical, fp, "f32le");
            return true;
        };
        ResolvedClip loopClip;
        ResolvedClip musicClip;
        if (!resolveShipped(loopSrc, loopClip))
            return Fail("S21 production Resolve failed for the shipped loop clip");
        if (!resolveShipped(musicSrc, musicClip))
            return Fail("S21 production Resolve failed for the shipped music clip");

        ProductionBackendConfig s21Config;
        s21Config.forceNoDevice = true;
        ProductionAudioBackend s21backend;
        std::string s21Error;
        if (!s21backend.Initialize(s21Config, s21Error))
            return Fail("S21 backend init failed");
        std::unordered_map<std::string, AudioClipBytes> s21bytes;
        s21bytes[loopClip.key] = AudioClipBytes{
            loopClip.key,
            std::make_shared<const std::vector<char>>(loopClip.bytes)
        };
        s21bytes[musicClip.key] = AudioClipBytes{
            musicClip.key,
            std::make_shared<const std::vector<char>>(musicClip.bytes)
        };
        s21backend.SetClipByteResolver(
            [&s21bytes](const std::string& key)
                -> rt2::core::Result<AudioClipBytes> {
                auto it = s21bytes.find(key);
                if (it == s21bytes.end())
                    return rt2::core::Result<AudioClipBytes>::Fail(
                        rt2::core::Error::MissingAsset, key,
                        "S21 has no bytes for this key");
                return rt2::core::Result<AudioClipBytes>::Ok(it->second);
            });

        // Real-file decode oracle: mono loop and stereo music generations
        // with the exact fixture geometry (48 kHz, 1 s, 440 Hz / 440+660).
        // Scoped so no oracle holder pins the generations past this point:
        // the world below holds its own references and the purge oracle
        // must observe exactly those.
        {
            std::shared_ptr<const DecodedAudioGeneration> loopGen;
            std::shared_ptr<const DecodedAudioGeneration> musicGen;
        {
            auto fetched = s21backend.FetchDecodedGeneration(loopClip.key);
            if (!fetched.IsOk())
                return Fail("S21 shipped loop bytes refused by production decode");
            loopGen = fetched.value;
            auto fetchedMusic = s21backend.FetchDecodedGeneration(musicClip.key);
            if (!fetchedMusic.IsOk())
                return Fail("S21 shipped music bytes refused by production decode");
            musicGen = fetchedMusic.value;
        }
        if (loopGen->channels != 1 || loopGen->sampleRate != 48000 ||
            loopGen->frameCount != 48000)
            return Fail("S21 shipped loop generation has wrong geometry");
        if (musicGen->channels != 2 || musicGen->sampleRate != 48000 ||
            musicGen->frameCount != 48000)
            return Fail("S21 shipped music generation has wrong geometry");
        {
            // Interleaved stereo view for the oracle (ZeroCrossings
            // strides f*2 like S2): 440 Hz -> ~880 crossings/s.
            std::vector<float> stereo(loopGen->frameCount * 2);
            for (uint32_t f = 0; f < loopGen->frameCount; ++f)
            {
                stereo[f * 2 + 0] = loopGen->pcmInterleaved[f];
                stereo[f * 2 + 1] = loopGen->pcmInterleaved[f];
            }
            if (ZeroCrossings(stereo.data(), loopGen->frameCount, 0) < 800)
                return Fail("S21 shipped loop PCM has no 440 Hz content");
        }
        } // oracle holders released: only the world may pin generations now

        // Authored-to-audible walk through AudioWorld: autoplay spatial
        // loop renders hard-right PCM, same-frame Stop renders silence,
        // then host-played music renders center and its full second
        // completes and settles. Each PCM oracle measures one isolated
        // voice.
        const AudioSessionId s21Session{ 21 };
        AudioWorld s21world(&s21backend, &s21backend, s21Session,
                            AudioOwnerKind::Runtime, AudioWorldConfig{});
        auto makeShippedRequest = [&](const ShippedSource& src,
                                      const std::string& key) {
            AudioPlayRequest req;
            req.source = src.uuid;
            req.component.bus = src.bus;
            req.component.clip.kind = AssetKind::AudioClip;
            req.component.clip.path = src.clipPath;
            req.component.clip.assetId = src.clipId;
            req.component.autoplay = src.autoplay;
            req.component.loop = src.loop;
            req.component.spatial = src.spatial;
            req.component.gain = src.gain;
            req.component.pitch = src.pitch;
            req.component.minDistance = src.minDistance;
            req.component.maxDistance = src.maxDistance;
            req.component.rolloff = src.rolloff;
            req.component.priority = src.priority;
            req.sourcePosition[0] = src.translation[0];
            req.sourcePosition[1] = src.translation[1];
            req.sourcePosition[2] = src.translation[2];
            req.hasTransform = true;
            req.clipKey = key;
            return req;
        };
        s21world.StageAutoplay(makeShippedRequest(loopSrc, loopClip.key));
        AudioListenerPose s21Listener; // origin, -Z forward, +Y up
        AudioSourcePose loopPose;
        loopPose.source = loopSrc.uuid;
        loopPose.position[0] = loopSrc.translation[0];
        loopPose.position[1] = loopSrc.translation[1];
        loopPose.position[2] = loopSrc.translation[2];
        loopPose.hasTransform = true;
        AudioSourcePose musicPose;
        musicPose.source = musicSrc.uuid;
        musicPose.position[0] = musicSrc.translation[0];
        musicPose.position[1] = musicSrc.translation[1];
        musicPose.position[2] = musicSrc.translation[2];
        musicPose.hasTransform = true;
        const AudioSourcePose s21Poses[2] = { loopPose, musicPose };
        s21world.UpdateSemantic(s21Listener, s21Poses, 2);
        if (s21world.LiveVoiceCount() != 1)
            return Fail("S21 shipped autoplay loop produced no voice");
        // Exactly hard-right equal-power mix at distance gain ~0.931:
        // the right channel carries the loop tone while the left is
        // exactly silent (pan-law clamp; unclamped cos(pi/2) rounds to
        // -4.37e-8 and production StartVoice refuses it). Render totals
        // are required exact: every read below is sized by what rendered,
        // so a short read fails loudly instead of over-reading.
        std::vector<float> loopBlock;
        bool loopRenderOk = false;
        const uint32_t loopRendered =
            RenderAll(s21backend, loopBlock, 4800, loopRenderOk);
        if (!loopRenderOk || loopRendered != 4800)
            return Fail("S21 loop block render failed");
        s21world.AdvanceCursors(loopRendered);
        {
            const float rightPeak = MaxAbsChannel(loopBlock.data(), loopRendered, 1);
            const float leftPeak = MaxAbsChannel(loopBlock.data(), loopRendered, 0);
            if (rightPeak < 0.3f || leftPeak != 0.0f)
                return Fail("S21 shipped loop PCM is not exactly hard-right");
        }
        // Live mix update across the singularity: the playing loop
        // follows final poses through exact-right without refusal or
        // stale panning (the pre-fixup SetVoiceMix refusal path). Each
        // move renders hard-panned PCM with an exactly silent channel
        // on the settled tail; moving back restores hard-right on the
        // same voice, which never drops.
        AudioSourcePose leftPose = loopPose;
        leftPose.position[0] = -3.0f;
        leftPose.position[1] = 0.0f;
        leftPose.position[2] = 0.0f;
        const AudioSourcePose leftPoses[2] = { leftPose, musicPose };
        s21world.UpdateSemantic(s21Listener, leftPoses, 2);
        if (s21world.LiveVoicesForSource(loopSrc.uuid).size() != 1)
            return Fail("S21 live move across the singularity lost the loop");
        {
            // Settling observation (documented, bounded): the first engine
            // period after a live SetVoiceMix can still deliver old-mix
            // frames (measured switch ~323 frames in), so exactness is
            // asserted on the settled tail while the full block proves
            // the voice stays audible through the transition.
            std::vector<float> leftBlock;
            bool leftOk = false;
            const uint32_t leftRendered =
                RenderAll(s21backend, leftBlock, 1600, leftOk);
            if (!leftOk || leftRendered != 1600)
                return Fail("S21 moved-loop render failed");
            s21world.AdvanceCursors(leftRendered);
            if (MaxAbsChannel(leftBlock.data(), leftRendered, 0) < 0.3f)
                return Fail("S21 moved loop went silent through the move");
            const float* leftTail = leftBlock.data() + 800 * 2;
            if (MaxAbsChannel(leftTail, 800, 0) < 0.3f ||
                MaxAbsChannel(leftTail, 800, 1) != 0.0f)
                return Fail("S21 moved loop PCM is not exactly hard-left");
        }
        s21world.UpdateSemantic(s21Listener, s21Poses, 2);
        if (s21world.LiveVoicesForSource(loopSrc.uuid).size() != 1)
            return Fail("S21 move back across the singularity lost the loop");
        {
            std::vector<float> backBlock;
            bool backOk = false;
            const uint32_t backRendered =
                RenderAll(s21backend, backBlock, 1600, backOk);
            if (!backOk || backRendered != 1600)
                return Fail("S21 moved-back render failed");
            s21world.AdvanceCursors(backRendered);
            if (MaxAbsChannel(backBlock.data(), backRendered, 1) < 0.3f)
                return Fail("S21 moved-back loop went silent");
            const float* backTail = backBlock.data() + 800 * 2;
            if (MaxAbsChannel(backTail, 800, 1) < 0.3f ||
                MaxAbsChannel(backTail, 800, 0) != 0.0f)
                return Fail("S21 moved-back PCM is not exactly hard-right");
        }
        // Host plays the idle music bed after the loop stopped: each PCM
        // oracle below measures one isolated voice, so the loop's right
        // energy cannot swamp the music's center ratio. Non-spatial center
        // mix at gain 0.8, then the full second completes and settles.
        uint64_t loopStopSeq = 0;
        if (!s21world.QueueStop(loopSrc.uuid, loopStopSeq))
            return Fail("S21 loop stop queue refused");
        s21world.UpdateSemantic(s21Listener, s21Poses, 2);
        if (s21world.LiveVoiceCount() != 0)
            return Fail("S21 loop stop did not take effect in its own frame");
        {
            std::vector<float> afterStop(800 * kNoDeviceChannels, -1234.5f);
            AudioPcmWriteBuffer stopBuf{ afterStop.data(), afterStop.size() };
            auto stopRender = s21backend.RenderNoDeviceFrames(stopBuf, 800);
            if (!stopRender.IsOk() || stopRender.value != 800)
                return Fail("S21 post-stop render failed");
            double firstEnergy = 0.0;
            for (float s : loopBlock)
                firstEnergy += (double)s * (double)s;
            double stopEnergy = 0.0;
            for (float s : afterStop)
                stopEnergy += (double)s * (double)s;
            if (!(stopEnergy < 0.01 * firstEnergy))
                return Fail("S21 post-stop block carries loop energy");
            if (MaxAbs(afterStop.data() + 2, afterStop.size() - 2) != 0.0f)
                return Fail("S21 post-stop tail is not exactly silent");
        }
        uint64_t musicSeq = 0;
        if (!s21world.QueuePlay(makeShippedRequest(musicSrc, musicClip.key),
                                musicSeq))
            return Fail("S21 shipped music QueuePlay refused");
        s21world.UpdateSemantic(s21Listener, s21Poses, 2);
        if (s21world.LiveVoiceCount() != 1)
            return Fail("S21 shipped music did not start alone");
        {
            std::vector<float> musicHead;
            bool headOk = false;
            const uint32_t headRendered =
                RenderAll(s21backend, musicHead, 4800, headOk);
            if (!headOk || headRendered != 4800)
                return Fail("S21 music head render failed");
            s21world.AdvanceCursors(headRendered);
            const float lPeak = MaxAbsChannel(musicHead.data(), headRendered, 0);
            const float rPeak = MaxAbsChannel(musicHead.data(), headRendered, 1);
            if (lPeak < 0.2f || rPeak < 0.2f)
                return Fail("S21 shipped music head is not center-audible");
            double lEnergy = 0.0;
            double rEnergy = 0.0;
            for (uint32_t f = 0; f < headRendered; ++f)
            {
                lEnergy += (double)musicHead[f * 2 + 0] *
                           (double)musicHead[f * 2 + 0];
                rEnergy += (double)musicHead[f * 2 + 1] *
                           (double)musicHead[f * 2 + 1];
            }
            if (!(lEnergy > 0.9 * rEnergy) || !(lEnergy < 1.12 * rEnergy))
                return Fail("S21 shipped music head is not center-panned");
        }
        {
            std::vector<float> musicTail;
            bool tailOk = false;
            const uint32_t tailRendered = RenderAll(
                s21backend, musicTail, 48000 - 4800, tailOk);
            if (!tailOk || tailRendered != 48000 - 4800)
                return Fail("S21 music full-length render failed");
            s21world.AdvanceCursors(tailRendered);
            s21world.ReconcilePostRender();
            s21world.UpdateSemantic(s21Listener, s21Poses, 2);
            if (!s21world.LiveVoicesForSource(musicSrc.uuid).empty())
                return Fail("S21 finished music still live after reconcile");
            if (s21world.GetSourceStatus(musicSrc.uuid).aggregate !=
                AudioSourceAggregate::Completed)
                return Fail("S21 finished music status is not Completed");
            if (s21world.LiveVoiceCount() != 0)
                return Fail("S21 world not empty after music completion");
        }
        rt2::core::Error s21ShutdownError;
        if (!s21world.Shutdown(s21ShutdownError))
            return Fail("S21 world shutdown failed");
        if (s21backend.LiveVoiceCount() != 0)
            return Fail("S21 backend voices live after world shutdown");
        if (s21backend.ActiveVoicesForKey(loopClip.key) != 0 ||
            s21backend.ActiveVoicesForKey(musicClip.key) != 0)
            return Fail("S21 backend key census live after world shutdown");
        // Decoded generations outlive the world for reuse (2 entries,
        // exact resident bytes); the explicit purge restores the
        // documented zero baseline and the next fetch re-decodes.
        if (s21backend.DecodedCacheEntryCount() != 2 ||
            s21backend.DecodedCacheResidentBytes() !=
                (48000 * 4 + 48000 * 2 * 4))
            return Fail("S21 retained cache census wrong after shutdown");
        if (s21backend.EvictZeroReferenceGenerations() != 2)
            return Fail("S21 explicit purge did not evict both idle entries");
        if (s21backend.DecodedCacheEntryCount() != 0 ||
            s21backend.DecodedCacheResidentBytes() != 0)
            return Fail("S21 cache baseline not restored by purge");
        {
            auto refetch = s21backend.FetchDecodedGeneration(loopClip.key);
            if (!refetch.IsOk() ||
                s21backend.DecodedCacheEntryCount() != 1)
                return Fail("S21 fetch after purge did not re-decode");
        } // refetch holder released: the re-decoded entry is idle again
        if (s21backend.EvictZeroReferenceGenerations() != 1 ||
            s21backend.DecodedCacheEntryCount() != 0)
            return Fail("S21 second purge did not restore baseline");
        s21backend.Shutdown();
    }
    Pass("S21 shipped acceptance scene through production resolve/decode/render");

    // ---- S21b: corrupt file refuses the non-autoplay branch ----
    //
    // Real corrupt bytes (checked-in corrupt_truncated.wav) through the
    // production decoder: typed refusal, no cache entry, and an
    // AudioWorld play against the corrupt key starts no voice while the
    // backend census stays at zero. This is the file-backed form of the
    // check-4 candidate refusal the fake suites assert with scripted
    // errors.
    {
        bool corruptOk = false;
        const std::vector<char> corruptBytes = ReadFile(
            "RT2AudioProbe/fixtures/corrupt_truncated.wav", corruptOk);
        if (!corruptOk || corruptBytes.empty())
            return Fail("S21b cannot read corrupt fixture");
        ProductionBackendConfig corruptConfig;
        corruptConfig.forceNoDevice = true;
        ProductionAudioBackend corruptBackend;
        std::string corruptError;
        if (!corruptBackend.Initialize(corruptConfig, corruptError))
            return Fail("S21b backend init failed");
        const uint64_t corruptFp = ProductionAudioBackend::FingerprintBytes(
            corruptBytes.data(), corruptBytes.size());
        const std::string kCorrupt = ProductionAudioBackend::BuildClipKey(
            "asset:corrupt", "clips/corrupt.wav", corruptFp, "f32le");
        corruptBackend.SetClipByteResolver(
            [&corruptBytes, &kCorrupt](
                const std::string& key) -> rt2::core::Result<AudioClipBytes> {
                if (key != kCorrupt)
                    return rt2::core::Result<AudioClipBytes>::Fail(
                        rt2::core::Error::MissingAsset, key,
                        "S21b has no bytes for this key");
                return rt2::core::Result<AudioClipBytes>::Ok(
                    AudioClipBytes{ kCorrupt,
                                    std::make_shared<const std::vector<char>>(
                                        corruptBytes) });
            });
        AudioClipBytes direct{ kCorrupt,
                               std::make_shared<const std::vector<char>>(
                                   corruptBytes) };
        if (corruptBackend.DecodeClip(direct).IsOk())
            return Fail("S21b corrupt file decoded without error");
        if (corruptBackend.FetchDecodedGeneration(kCorrupt).IsOk())
            return Fail("S21b corrupt file fetched without error");
        if (corruptBackend.DecodedCacheEntryCount() != 0 ||
            corruptBackend.LiveVoiceCount() != 0)
            return Fail("S21b corrupt file polluted backend census");
        const AudioSessionId corruptSession{ 22 };
        AudioWorld corruptWorld(&corruptBackend, &corruptBackend,
                                corruptSession, AudioOwnerKind::Runtime,
                                AudioWorldConfig{});
        std::array<uint8_t, 16> corruptId{};
        corruptId[15] = 21;
        AudioPlayRequest corruptReq;
        corruptReq.source = rt2::core::UUID(corruptId);
        corruptReq.component.bus = AudioBus::Music;
        corruptReq.component.autoplay = false;
        corruptReq.component.loop = false;
        corruptReq.component.spatial = false;
        corruptReq.component.gain = 0.8f;
        corruptReq.component.pitch = 1.0f;
        corruptReq.component.minDistance = 1.0f;
        corruptReq.component.maxDistance = 30.0f;
        corruptReq.component.rolloff = 1.0f;
        corruptReq.component.priority = 128;
        corruptReq.hasTransform = false;
        corruptReq.clipKey = kCorrupt;
        uint64_t corruptSeq = 0;
        if (corruptWorld.QueuePlay(corruptReq, corruptSeq))
        {
            AudioListenerPose corruptListener;
            corruptWorld.UpdateSemantic(corruptListener, nullptr, 0);
        }
        if (corruptWorld.LiveVoiceCount() != 0 ||
            corruptBackend.LiveVoiceCount() != 0)
            return Fail("S21b corrupt play started a voice");
        if (corruptWorld.GetSourceStatus(corruptReq.source).aggregate !=
            AudioSourceAggregate::Failed)
            return Fail("S21b corrupt play status is not Failed");
        rt2::core::Error corruptShutdownError;
        if (!corruptWorld.Shutdown(corruptShutdownError))
            return Fail("S21b world shutdown failed");
        corruptBackend.Shutdown();
    }
    Pass("S21b corrupt file refuses the non-autoplay branch");

    // ---- S21c: twenty Play/Stop sessions on the production census ----
    //
    // P2 repeated-session gate: every cycle constructs and destroys a
    // production AudioWorld with a fresh session ID on one shared
    // backend (the backend outlives sessions in production) and asserts
    // the voice/key/cache census plus the world's active generation
    // refs after the stops and again after shutdown. Voice-only cycles
    // inside a single world cannot catch resources retained across
    // world/session destruction. The explicit purge restores the
    // documented zero baseline; a retained holder pins its entry
    // across the purge (leak discrimination), and the next fetch
    // re-decodes afterwards.
    {
        bool loopFileOk = false;
        const std::vector<char> loopFileBytes = ReadFile(
            "RT2App/assets/audio/acceptance_loop_mono_s16.wav", loopFileOk);
        bool musicFileOk = false;
        const std::vector<char> musicFileBytes = ReadFile(
            "RT2App/assets/audio/acceptance_music_stereo_s16.wav",
            musicFileOk);
        if (!loopFileOk || !musicFileOk)
            return Fail("S21c cannot read shipped clips from repo root");
        const std::string kLoopCyc = ProductionAudioBackend::BuildClipKey(
            "asset:loop", "audio/acceptance_loop_mono_s16.wav",
            ProductionAudioBackend::FingerprintBytes(
                loopFileBytes.data(), loopFileBytes.size()),
            "f32le");
        const std::string kMusicCyc = ProductionAudioBackend::BuildClipKey(
            "asset:music", "audio/acceptance_music_stereo_s16.wav",
            ProductionAudioBackend::FingerprintBytes(
                musicFileBytes.data(), musicFileBytes.size()),
            "f32le");
        ProductionBackendConfig cycConfig;
        cycConfig.forceNoDevice = true;
        ProductionAudioBackend cycBackend;
        std::string cycError;
        if (!cycBackend.Initialize(cycConfig, cycError))
            return Fail("S21c backend init failed");
        cycBackend.SetClipByteResolver(
            [&loopFileBytes, &musicFileBytes, &kLoopCyc, &kMusicCyc](
                const std::string& key) -> rt2::core::Result<AudioClipBytes> {
                if (key == kLoopCyc)
                    return rt2::core::Result<AudioClipBytes>::Ok(
                        AudioClipBytes{ kLoopCyc,
                                        std::make_shared<const std::vector<char>>(
                                            loopFileBytes) });
                if (key == kMusicCyc)
                    return rt2::core::Result<AudioClipBytes>::Ok(
                        AudioClipBytes{ kMusicCyc,
                                        std::make_shared<const std::vector<char>>(
                                            musicFileBytes) });
                return rt2::core::Result<AudioClipBytes>::Fail(
                    rt2::core::Error::MissingAsset, key,
                    "S21c has no bytes for this key");
            });
        std::array<uint8_t, 16> loopCycId{};
        loopCycId[15] = 22;
        std::array<uint8_t, 16> musicCycId{};
        musicCycId[15] = 23;
        const rt2::core::UUID loopCycSrc(loopCycId);
        const rt2::core::UUID musicCycSrc(musicCycId);
        auto cycRequest = [&](const rt2::core::UUID& src,
                              const std::string& key, bool loop) {
            AudioPlayRequest req;
            req.source = src;
            req.component.bus =
                loop ? AudioBus::Effects : AudioBus::Music;
            req.component.autoplay = loop;
            req.component.loop = loop;
            req.component.spatial = false;
            req.component.gain = 1.0f;
            req.component.pitch = 1.0f;
            req.component.minDistance = 1.0f;
            req.component.maxDistance = 30.0f;
            req.component.rolloff = 1.0f;
            req.component.priority = 128;
            req.hasTransform = false;
            req.clipKey = key;
            return req;
        };
        size_t pinnedBytes = 0;
        for (int cycle = 0; cycle < 20; ++cycle)
        {
            // Fresh world and session per cycle; the scoped world is
            // destroyed (after explicit Shutdown) before the next cycle,
            // so cross-cycle retention is observable in the backend.
            AudioWorld cycWorld(
                &cycBackend, &cycBackend,
                AudioSessionId{ static_cast<uint64_t>(100 + cycle) },
                AudioOwnerKind::Runtime, AudioWorldConfig{});
            cycWorld.StageAutoplay(cycRequest(loopCycSrc, kLoopCyc, true));
            uint64_t playSeq = 0;
            if (!cycWorld.QueuePlay(
                    cycRequest(musicCycSrc, kMusicCyc, false), playSeq))
                return FailDetail(std::string("S21c music QueuePlay refused at cycle ") +
                                  std::to_string(cycle));
            AudioListenerPose cycListener;
            cycWorld.UpdateSemantic(cycListener, nullptr, 0);
            if (cycWorld.LiveVoiceCount() != 2 ||
                cycBackend.LiveVoiceCount() != 2)
                return FailDetail(std::string("S21c cycle did not start both voices at ") +
                                  std::to_string(cycle));
            // Both fetched generations are actively referenced by this
            // session's world.
            if (cycWorld.CachedClipGenerationCount() != 2)
                return FailDetail(std::string("S21c world generation refs wrong at cycle ") +
                                  std::to_string(cycle));
            std::vector<float> cycBlock;
            bool cycOk = false;
            const uint32_t cycRendered =
                RenderAll(cycBackend, cycBlock, 480, cycOk);
            if (!cycOk || cycRendered != 480)
                return FailDetail(std::string("S21c cycle render failed at ") +
                                  std::to_string(cycle));
            cycWorld.AdvanceCursors(cycRendered);
            uint64_t stopSeq = 0;
            if (!cycWorld.QueueStop(loopCycSrc, stopSeq) ||
                !cycWorld.QueueStop(musicCycSrc, stopSeq))
                return FailDetail(std::string("S21c cycle stop refused at ") +
                                  std::to_string(cycle));
            cycWorld.UpdateSemantic(cycListener, nullptr, 0);
            // Production session/voice/generation census at baseline.
            if (cycWorld.LiveVoiceCount() != 0 ||
                cycBackend.LiveVoiceCount() != 0 ||
                cycBackend.ActiveVoicesForKey(kLoopCyc) != 0 ||
                cycBackend.ActiveVoicesForKey(kMusicCyc) != 0)
                return FailDetail(std::string("S21c cycle census not at baseline at ") +
                                  std::to_string(cycle));
            rt2::core::Error cycShutdownError;
            if (!cycWorld.Shutdown(cycShutdownError))
                return FailDetail(std::string("S21c world shutdown failed at cycle ") +
                                  std::to_string(cycle));
            // Shutdown releases the session's cached generations; the
            // backend retains the idle decoded entries for reuse.
            if (cycWorld.CachedClipGenerationCount() != 0)
                return FailDetail(std::string("S21c world refs live after shutdown at ") +
                                  std::to_string(cycle));
            if (cycBackend.LiveVoiceCount() != 0)
                return FailDetail(std::string("S21c backend voices live after shutdown at ") +
                                  std::to_string(cycle));
            // Both decoded generations retained for reuse: exact stable
            // census. A retained-generation leak would grow entries or
            // bytes from here on.
            if (cycBackend.DecodedCacheEntryCount() != 2)
                return FailDetail(std::string("S21c cache entries drifted at cycle ") +
                                  std::to_string(cycle));
            if (cycle == 0)
                pinnedBytes = cycBackend.DecodedCacheResidentBytes();
            else if (cycBackend.DecodedCacheResidentBytes() != pinnedBytes)
                return FailDetail(std::string("S21c cache bytes drifted at cycle ") +
                                  std::to_string(cycle));
        }
        if (pinnedBytes != (48000 * 4 + 48000 * 2 * 4))
            return Fail("S21c retained cache bytes do not match the two shipped clips");
        // All twenty worlds shut down and destroyed above: the backend
        // holds no session voices, only the two idle decoded entries.
        if (cycBackend.LiveVoiceCount() != 0)
            return Fail("S21c backend voices live after twenty shutdowns");
        // Explicit purge restores the documented zero baseline.
        if (cycBackend.EvictZeroReferenceGenerations() != 2)
            return Fail("S21c purge did not evict both idle generations");
        if (cycBackend.DecodedCacheEntryCount() != 0 ||
            cycBackend.DecodedCacheResidentBytes() != 0)
            return Fail("S21c cache baseline not restored by purge");
        // A retained holder pins its entry across the purge (leak
        // discrimination); releasing it lets the next purge restore zero.
        {
            auto held = cycBackend.FetchDecodedGeneration(kLoopCyc);
            if (!held.IsOk())
                return Fail("S21c fetch after purge failed");
            if (cycBackend.EvictZeroReferenceGenerations() != 0 ||
                cycBackend.DecodedCacheEntryCount() != 1)
                return Fail("S21c purge evicted a pinned generation");
        }
        if (cycBackend.EvictZeroReferenceGenerations() != 1 ||
            cycBackend.DecodedCacheEntryCount() != 0 ||
            cycBackend.DecodedCacheResidentBytes() != 0)
            return Fail("S21c final purge did not restore baseline");
        cycBackend.Shutdown();
    }
    Pass("S21c twenty Play/Stop sessions hold the production census");

    // ---- S21d: broken path/sidecar/bytes fail loudly ----
    //
    // Production Resolve on a missing clip path (Missing), on a sidecar
    // claiming a foreign identity (Conflict), plus a same-size byte flip
    // that decodes to audibly different PCM (content sensitivity: the
    // fingerprint observes what mtime/size cannot).
    {
        rt2::core::AssetResolutionContext badCtx;
        badCtx.assetRoot = std::filesystem::absolute("RT2App/assets");
        badCtx.database = nullptr;
        std::array<uint8_t, 16> missingId{};
        missingId[15] = 24;
        AssetReference missingRef;
        missingRef.kind = AssetKind::AudioClip;
        missingRef.path = "audio/no-such-clip.wav";
        missingRef.sourceKey = "";
        missingRef.assetId = rt2::core::UUID(missingId);
        std::vector<rt2::core::AssetDiagnostic> missingDiags;
        if (rt2::core::Resolve(missingRef, badCtx, rt2::core::UUID(missingId),
                               "Missing", missingDiags)
                .success)
            return Fail("S21d missing clip path resolved");
        bool sawMissing = false;
        for (const auto& d : missingDiags)
            sawMissing = sawMissing ||
                         d.severity == rt2::core::AssetDiagnostic::Missing;
        if (!sawMissing)
            return Fail("S21d missing clip path has no Missing diagnostic");

        // Sidecar conflict in a uniquely owned scratch directory. The
        // directory is created exclusively: a name collision is retried
        // with the next suffix, never removed. Cleanup deletes only the
        // exact created path (verified parent and owned prefix), so a
        // fixed shared temp path can never destroy another process's or
        // user's directory.
        std::error_code tempEc;
        // Stripped explicitly: temp_directory_path may carry a trailing
        // separator that lexically_normal keeps on this STL, which would
        // make the ownership check below compare unequal against a
        // parent_path taken from a joined path (never strip a drive
        // root).
        std::string tempRaw =
            std::filesystem::temp_directory_path(tempEc).string();
        while (tempRaw.size() > 3 &&
               (tempRaw.back() == '\\' || tempRaw.back() == '/'))
            tempRaw.pop_back();
        const std::filesystem::path tempRoot =
            std::filesystem::path(tempRaw).lexically_normal();
        if (tempEc || tempRoot.empty())
            return Fail("S21d has no temp directory");
        const std::string ownerTag = "rt2_probe_s21_" +
            std::to_string(static_cast<unsigned long long>(
                std::hash<std::thread::id>{}(std::this_thread::get_id())));
        std::filesystem::path conflictDir;
        bool haveDir = false;
        for (int attempt = 0; attempt < 100 && !haveDir; ++attempt)
        {
            std::error_code createEc;
            const std::filesystem::path candidate =
                tempRoot / (ownerTag + "_" + std::to_string(attempt));
            if (std::filesystem::create_directory(candidate, createEc) &&
                !createEc)
            {
                conflictDir = candidate;
                haveDir = true;
            }
        }
        if (!haveDir)
            return Fail("S21d could not create an owned scratch directory");
        auto removeOwnedDir = [&]() -> bool {
            if (conflictDir.parent_path().lexically_normal() != tempRoot)
                return false;
            if (conflictDir.filename().string().rfind(ownerTag + "_", 0) != 0)
                return false;
            std::error_code removeEc;
            std::filesystem::remove_all(conflictDir, removeEc);
            if (removeEc)
                return false;
            return !std::filesystem::exists(conflictDir);
        };
        bool conflictWrote = false;
        {
            bool loopBytesOk = false;
            const std::vector<char> loopBytesForConflict = ReadFile(
                "RT2App/assets/audio/acceptance_loop_mono_s16.wav",
                loopBytesOk);
            if (loopBytesOk)
            {
                std::ofstream wavOut(
                    (conflictDir / "conflict.wav").string(), std::ios::binary);
                std::ofstream metaOut(
                    (conflictDir / "conflict.wav.rt2meta").string(),
                    std::ios::binary);
                // Foreign identity: the music bed's asset ID on the loop's
                // bytes. Production resolution must refuse to substitute.
                wavOut.write(loopBytesForConflict.data(),
                             static_cast<std::streamsize>(
                                 loopBytesForConflict.size()));
                metaOut << "1d7d49f3-3346-46b6-a9bb-02d0c9ee8e38";
                conflictWrote =
                    static_cast<bool>(wavOut) && static_cast<bool>(metaOut);
            }
        }
        if (!conflictWrote)
            return Fail("S21d could not stage the conflict fixture");
        rt2::core::AssetResolutionContext conflictCtx;
        conflictCtx.assetRoot = conflictDir;
        conflictCtx.database = nullptr;
        AssetReference conflictRef;
        conflictRef.kind = AssetKind::AudioClip;
        conflictRef.path = "conflict.wav";
        conflictRef.sourceKey = "";
        conflictRef.assetId =
            rt2::core::UUID::Parse("b3e7c60f-9925-43ba-b750-f35673d25026");
        std::vector<rt2::core::AssetDiagnostic> conflictDiags;
        if (rt2::core::Resolve(conflictRef, conflictCtx,
                               rt2::core::UUID(missingId), "Conflict",
                               conflictDiags)
                .success)
        {
            removeOwnedDir();
            return Fail("S21d sidecar conflict resolved");
        }
        bool sawConflict = false;
        for (const auto& d : conflictDiags)
            sawConflict = sawConflict ||
                          d.severity == rt2::core::AssetDiagnostic::Conflict;
        if (!removeOwnedDir())
            return Fail("S21d owned scratch directory was not removed");
        if (!sawConflict)
            return Fail("S21d sidecar conflict has no Conflict diagnostic");

        // Same-size byte flip: still decodes, but to different PCM with a
        // different fingerprint.
        bool flipBytesOk = false;
        std::vector<char> flipBytes = ReadFile(
            "RT2App/assets/audio/acceptance_loop_mono_s16.wav", flipBytesOk);
        if (!flipBytesOk || flipBytes.size() < 2000)
            return Fail("S21d cannot read shipped loop bytes");
        flipBytes[1000] = static_cast<char>(flipBytes[1000] ^ 0xFF);
        ProductionBackendConfig flipConfig;
        flipConfig.forceNoDevice = true;
        ProductionAudioBackend flipBackend;
        std::string flipError;
        if (!flipBackend.Initialize(flipConfig, flipError))
            return Fail("S21d backend init failed");
        const uint64_t flipFp = ProductionAudioBackend::FingerprintBytes(
            flipBytes.data(), flipBytes.size());
        const std::string kFlip = ProductionAudioBackend::BuildClipKey(
            "asset:flip", "audio/flip.wav", flipFp, "f32le");
        bool origOk = false;
        const std::vector<char> origBytes = ReadFile(
            "RT2App/assets/audio/acceptance_loop_mono_s16.wav", origOk);
        if (!origOk)
            return Fail("S21d cannot re-read shipped loop bytes");
        const uint64_t origFp = ProductionAudioBackend::FingerprintBytes(
            origBytes.data(), origBytes.size());
        if (flipFp == origFp)
            return Fail("S21d fingerprint blind to a same-size byte flip");
        const std::string kOrig = ProductionAudioBackend::BuildClipKey(
            "asset:orig", "audio/orig.wav", origFp, "f32le");
        flipBackend.SetClipByteResolver(
            [&flipBytes, &kFlip, &origBytes, &kOrig](
                const std::string& key) -> rt2::core::Result<AudioClipBytes> {
                if (key == kFlip)
                    return rt2::core::Result<AudioClipBytes>::Ok(
                        AudioClipBytes{ kFlip,
                                        std::make_shared<const std::vector<char>>(
                                            flipBytes) });
                if (key == kOrig)
                    return rt2::core::Result<AudioClipBytes>::Ok(
                        AudioClipBytes{ kOrig,
                                        std::make_shared<const std::vector<char>>(
                                            origBytes) });
                return rt2::core::Result<AudioClipBytes>::Fail(
                    rt2::core::Error::MissingAsset, key,
                    "S21d has no bytes for this key");
            });
        auto flipGen = flipBackend.FetchDecodedGeneration(kFlip);
        if (!flipGen.IsOk())
            return Fail("S21d flipped bytes refused decode (want PCM drift, not refusal)");
        auto origGen = flipBackend.FetchDecodedGeneration(kOrig);
        if (!origGen.IsOk() ||
            origGen.value->pcmInterleaved == flipGen.value->pcmInterleaved)
            return Fail("S21d flipped bytes decode to identical PCM");
        flipBackend.Shutdown();
    }
    Pass("S21d broken path/sidecar/bytes fail loudly");

    backend.Shutdown();
    if (backend.Status().productionNoDevice)
        return Fail("shutdown backend still reports no-device");

    std::printf("RT2AudioProbe: PASS no-device float32 stereo 48kHz (%s)\n",
                kPinnedMiniaudioVersionString);
    return 0;
}
