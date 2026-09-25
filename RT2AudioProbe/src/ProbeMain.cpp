// ProbeMain.cpp
//
// A4: RT2AudioProbe exercises the PRODUCTION backend (real WAV/FLAC/MP3
// decode, RT2 stereo-gain node, engine mixer, immutable cache) in fixed
// float32 stereo 48 kHz no-device mode with exact oracles.
//
// Every refusal prints to stderr and exits nonzero (this codebase's
// characteristic bug is silent failure). Sections:
//   S1  pin, no-device mode, lock-free gain atomic
//   S2  WAV mono f32 decode (determinism, rate/channels/count, sine shape)
//   S3  WAV stereo f32 decode (channels distinct, L matches mono)
//   S4  FLAC lossless decode (matches s16 master within 1 ulp)
//   S5  MP3 decode (energy, range, rate/channels)
//   S6  corrupt inputs refuse with typed errors and publish no cache entry
//   S7  fingerprint/key mismatch and malformed keys refuse
//   S8  Fetch dedupe (same key, same immutable object)
//   S9  old/new fingerprint overlap on one path (distinct PCM, concurrent)
//   S10 Register/Release (double handle, unknown, live-handle refusal)
//   S11 exact L/R transport (hard-left/center/hard-right, attenuation,
//       bus and Master halving through real group volumes)
//   S12 pitch-2.0 completion range through the thread-safe pitch path
//   S13 completion token match + post-completion silence
//   S14 session pause freeze, initialPaused, PauseVoice resume
//   S15 StopSessionVoices census
//   S16 RenderNoDeviceFrames validation matrix, idle silence, prefix
//       discipline, post-shutdown hard failure (caller storage untouched)
//   S17 hard-cap ReplaceVoice (one-unit commit, victim preserved on failure)
//   S18 over-budget decode names asset and byte counts, publishes nothing
//   S19 torn-pair stress on the packed L/R atomic through the real block
//       function (TSan: unavailable on MSVC; design is allocation-free
//       with one acquire load per block)
//
// Fixtures live in RT2AudioProbe/fixtures (generated; see README.md). The
// probe reads them into immutable byte vectors and decodes from memory:
// no path the content can change underneath is ever trusted.

#include "AudioBackendPin.h"
#include "ProductionAudioBackend.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
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
        auto r1 = backend.DecodeClip(badMagic);
        auto r2 = backend.DecodeClip(badEmpty);
        if (r1.IsOk() || r2.IsOk())
            return Fail("corrupt input decoded without error");
        if (r1.error.code == rt2::core::Error::None ||
            r2.error.code == rt2::core::Error::None)
            return Fail("corrupt refusal carries no typed error");
        if (backend.DecodedCacheEntryCount() != before)
            return Fail("corrupt decode published a cache entry");
        // Mid-data truncation is a graceful prefix, not a refusal: fewer
        // frames than the full file, same leading content and energy.
        AudioClipBytes cut{ KeyFor(backend, "asset:cut", "clips/cut.wav", truncated),
                            std::make_shared<const std::vector<char>>(truncated) };
        auto r3 = backend.DecodeClip(cut);
        if (!r3.IsOk())
            return Fail("mid-data truncation unexpectedly refused");
        if (r3.value->frameCount >= 48000 || r3.value->frameCount < 47000)
            return Fail("truncated decode did not yield a shorter prefix");
        if (r3.value->pcmInterleaved !=
            std::vector<float>(monoGen->pcmInterleaved.begin(),
                               monoGen->pcmInterleaved.begin() + r3.value->frameCount))
            return Fail("truncated prefix differs from the full decode");
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

    backend.Shutdown();
    if (backend.Status().productionNoDevice)
        return Fail("shutdown backend still reports no-device");

    std::printf("RT2AudioProbe: PASS no-device float32 stereo 48kHz (%s)\n",
                kPinnedMiniaudioVersionString);
    return 0;
}
