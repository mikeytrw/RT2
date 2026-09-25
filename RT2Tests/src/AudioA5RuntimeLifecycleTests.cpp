#include <doctest/doctest.h>

// ============================================================================
// A5 - Runtime audio lifecycle and listener (grounded at 249a5a7).
//
// Transactional Play-session audio over the real RuntimeSceneController with
// the recording fake behind both seams: frozen candidate resolve/decode of
// every persisted bound clip (autoplay or not), staged autoplay with
// on_create precedence and Stop/Pause suppression, host-injected listener
// poses from the actual rendered camera, post-GPU-sync/pre-render Update
// and sample-frozen Step slots, one authoritative post-order destroy
// subtree shared by audio/script/physics/ECS, and full Stop census reset.
//
// Every case is discriminating: each names the single mutation that must
// turn it red (rather than merely checking that a call happened).
//
// Out of scope (explicit): Lua controls (A6), inspector preview/status UI
// (A7), production miniaudio decode/device (A4 probe), acceptance scene
// and durable docs (A8), and any pinball sound content.
//
// CPU boundary: this file includes only portable engine headers plus the
// standard library. The hard #error guard fails the build if miniaudio.h
// ever becomes reachable from RT2Tests (required check 17).
// ============================================================================

#if __has_include("miniaudio.h")
#error "A5 boundary: RT2Tests must not import miniaudio (check 17; the adapter stays in RT2AudioBackend)"
#endif

#include "RuntimeSceneController.h"
#include "RuntimeLifecycleObserver.h"
#include "IRuntimeScriptDispatch.h"
#include "SceneManager.h"
#include "SceneDocument.h"
#include "SceneGraph.h"
#include "ECSComponents.h"
#include "ECSScene.h"
#include "PhysicsComponents.h"
#include "ISceneRenderBridge.h"
#include "GPUSceneData.h"
#include "AudioBackend.h"
#include "AudioComponents.h"
#include "AudioWorld.h"
#include "FakeAudioBackend.h"
#include "PhysicsWorld.h"
#include "core/UUID.h"
#include "core/Error.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

using namespace rt2::core;
using namespace rt2::audio;

namespace
{

constexpr float kCenter = 0.70710678f; // cos(pi/4): equal-power center

uint64_t g_A5UuidCounter = 0;

UUID A5NextUuid()
{
    ++g_A5UuidCounter;
    std::array<uint8_t, 16> bytes{};
    uint64_t n = g_A5UuidCounter;
    for (int i = 0; i < 8; ++i)
        bytes[15 - i] = static_cast<uint8_t>(n >> (8 * i));
    return UUID(bytes);
}

class A5RecordingBridge final : public ISceneRenderBridge
{
public:
    int fullSyncCalls = 0;
    int materialSyncCalls = 0;
    int transformSyncCalls = 0;
    int resetTemporalCalls = 0;
    int renderRequests = 0;

    void FullSync(GPUSceneData&) override { ++fullSyncCalls; }
    void MaterialSync(GPUSceneData&) override { ++materialSyncCalls; }
    void TransformSync(GPUSceneData&) override { ++transformSyncCalls; }
    void ResetTemporalState() override { ++resetTemporalCalls; }
    void RequestRender() override { ++renderRequests; }

    void Reset()
    {
        fullSyncCalls = 0;
        materialSyncCalls = 0;
        transformSyncCalls = 0;
        resetTemporalCalls = 0;
        renderRequests = 0;
    }

    bool Quiet() const
    {
        return fullSyncCalls == 0 && materialSyncCalls == 0 &&
               transformSyncCalls == 0 && resetTemporalCalls == 0 &&
               renderRequests == 0;
    }
};

class A5Observer final : public IRuntimeLifecycleObserver
{
public:
    int starts = 0;
    int stops = 0;

    void OnSceneStart(const SceneDocument&) override { ++starts; }
    void OnSceneStop(const SceneDocument&) override { ++stops; }
};

// Destroy probe: captures the exact OnEntitiesDestroying membership per
// destroy position and optionally exercises re-entrant audio commands
// against the controller's committed world while the destroying mark is
// active (must refuse) plus the audio-before-callback order (voices for
// the dying set must already be stopped when the callback runs).
class A5DestroyProbe final : public IRuntimeScriptDispatch
{
public:
    RuntimeSceneController* ctrl = nullptr;
    int destroyCallCount = 0;
    std::vector<std::vector<UUID>> destroyCalls;
    // Re-entrancy arms (one-shot semantics not needed: every destroy call
    // re-checks while the mark is active).
    bool refuseAudioArmed = false;
    bool audioRefusalsObserved = false;
    bool voicesStoppedAtCallback = true;

    void OnFixedUpdate(float) override {}
    void OnUpdate(float) override {}
    void SyncScriptEnvironments() override {}
    void OnEntitiesDestroying(const std::vector<UUID>& uuids) override
    {
        ++destroyCallCount;
        destroyCalls.push_back(uuids);
        if (ctrl == nullptr || ctrl->TryGetAudioWorld() == nullptr)
            return;
        AudioWorld* world = ctrl->TryGetAudioWorld();
        for (const UUID& id : uuids)
        {
            if (world->LiveVoicesForSource(id).size() != 0)
                voicesStoppedAtCallback = false;
        }
        if (refuseAudioArmed)
        {
            for (const UUID& id : uuids)
            {
                AudioPlayRequest req;
                req.source = id;
                req.component = AudioSourceComponent{};
                req.component.bus = AudioBus::Effects;
                req.clipKey = "audioclip:reentrant.wav";
                uint64_t seq = 0;
                if (!world->QueuePlay(req, seq) &&
                    !world->QueueStop(id, seq))
                    audioRefusalsObserved = true;
            }
        }
    }
};

// No-device order probe (A5 fixup, finding 1): an IAudioBackend /
// IAudioClipProvider that delegates everything to an inner recording
// fake but reports production no-device mode and turns every PCM render
// into an oracle for the controller's semantic-before-render order. Each
// render records the backend voice census and mix-publication state AT
// RENDER TIME and fills the caller buffer with the live-voice count, so
// the buffer content proves whether the semantic phase (drain, autoplay,
// final mixes) ran before the PCM block. Under the old render-first
// order the first-frame buffer reads 0.0 with an advanced cursor.
// Composition (not inheritance): RecordingFakeAudioBackend is final.
class A5NoDeviceOrderBackend final : public IAudioBackend,
                                     public IAudioClipProvider
{
public:
    RecordingFakeAudioBackend inner;

    struct RenderSight
    {
        uint32_t requested = 0;
        size_t liveVoices = 0;
        size_t mixPubs = 0;
        BackendVoiceMix lastMix{};
        bool hasMix = false;
        const float* buffer = nullptr;
    };
    std::vector<RenderSight> sights;

    size_t LiveTokenCount() const { return inner.LiveTokenCount(); }

    Result<std::shared_ptr<const DecodedAudioGeneration>>
    FetchDecodedGeneration(const std::string& clipKey) override
    {
        return inner.FetchDecodedGeneration(clipKey);
    }

    Result<std::shared_ptr<const DecodedAudioGeneration>> DecodeClip(
        const AudioClipBytes& clip) override
    {
        return inner.DecodeClip(clip);
    }
    Result<BackendClipHandle> RegisterDecodedGeneration(
        std::shared_ptr<const DecodedAudioGeneration> generation) override
    {
        return inner.RegisterDecodedGeneration(std::move(generation));
    }
    bool ReleaseDecodedGeneration(BackendClipHandle handle,
                                  Error& outError) override
    {
        return inner.ReleaseDecodedGeneration(handle, outError);
    }
    Result<BackendVoiceToken> StartVoice(
        BackendClipHandle clip, const BackendVoiceStart& start) override
    {
        return inner.StartVoice(clip, start);
    }
    Result<BackendVoiceToken> ReplaceVoice(
        BackendVoiceToken victim, BackendClipHandle clip,
        const BackendVoiceStart& start) override
    {
        return inner.ReplaceVoice(victim, clip, start);
    }
    bool StopVoice(BackendVoiceToken token, Error& outError) override
    {
        return inner.StopVoice(token, outError);
    }
    bool PauseVoice(BackendVoiceToken token, bool paused,
                    Error& outError) override
    {
        return inner.PauseVoice(token, paused, outError);
    }
    bool SetVoiceMix(BackendVoiceToken token, const BackendVoiceMix& mix,
                     Error& outError) override
    {
        return inner.SetVoiceMix(token, mix, outError);
    }
    Result<std::vector<BackendVoiceCompletion>> DrainCompletions(
        AudioSessionId session) override
    {
        return inner.DrainCompletions(session);
    }
    bool SetSessionPaused(AudioSessionId session, bool paused,
                          Error& outError) override
    {
        return inner.SetSessionPaused(session, paused, outError);
    }
    bool SetBusGain(AudioBus bus, float gain, Error& outError) override
    {
        return inner.SetBusGain(bus, gain, outError);
    }
    bool StopSessionVoices(AudioSessionId session,
                           Error& outError) override
    {
        return inner.StopSessionVoices(session, outError);
    }

    AudioBackendStatus Status() const override
    {
        AudioBackendStatus status;
        status.productionNoDevice = true;
        status.detail = "A5 order probe (no-device)";
        return status;
    }

    Result<uint32_t> RenderNoDeviceFrames(
        AudioPcmWriteBuffer interleavedStereo,
        uint32_t requestedFrames) override
    {
        ++inner.renderCalls;
        if (interleavedStereo.data == nullptr || requestedFrames == 0 ||
            interleavedStereo.sampleCapacity <
                static_cast<size_t>(requestedFrames) * 2)
            return Result<uint32_t>::Fail(
                Error::InvalidArgument, "audio no-device render",
                "A5 order probe: undersized or null caller buffer");
        RenderSight sight;
        sight.requested = requestedFrames;
        sight.liveVoices = inner.LiveTokenCount();
        sight.mixPubs = inner.mixes.size();
        if (!inner.mixes.empty())
        {
            sight.lastMix = inner.mixes.back().second;
            sight.hasMix = true;
        }
        sight.buffer = interleavedStereo.data;
        const float marker = static_cast<float>(sight.liveVoices);
        for (uint32_t f = 0; f < requestedFrames * 2; ++f)
            interleavedStereo.data[f] = marker;
        sights.push_back(sight);
        return Result<uint32_t>::Ok(requestedFrames);
    }

    void ResetSights()
    {
        sights.clear();
        inner.ClearRecords();
    }
};

// Budget-pinning provider (A5 fixup, finding 3): an IAudioClipProvider
// with production LRU/budget semantics — a tiny byte budget, LRU eviction
// of generations with no outside holder (use_count == 1), and loud
// over-budget refusal naming byte counts. The controller's candidate pins
// each validated generation as it is fetched, so a combined over-budget
// candidate refuses pre-commit instead of evicting silently.
class A5PinBudgetProvider final : public IAudioClipProvider
{
public:
    size_t budgetBytes = 0;
    std::map<std::string, size_t> scriptedBytes; // key -> PCM bytes
    struct Entry
    {
        std::shared_ptr<const DecodedAudioGeneration> generation;
        size_t bytes = 0;
        uint64_t lru = 0;
    };
    std::map<std::string, Entry> cache;
    std::vector<std::string> fetches;
    uint64_t clock = 0;

    Result<std::shared_ptr<const DecodedAudioGeneration>>
    FetchDecodedGeneration(const std::string& clipKey) override
    {
        using ResultGen =
            Result<std::shared_ptr<const DecodedAudioGeneration>>;
        fetches.push_back(clipKey);
        const auto hit = cache.find(clipKey);
        if (hit != cache.end())
        {
            hit->second.lru = ++clock;
            return ResultGen::Ok(hit->second.generation);
        }
        const auto sized = scriptedBytes.find(clipKey);
        const size_t need =
            (sized != scriptedBytes.end()) ? sized->second : 1048576;
        size_t resident = 0;
        for (const auto& entry : cache)
            resident += entry.second.bytes;
        // Evict LRU-first among generations with no outside holder.
        while (resident + need > budgetBytes)
        {
            const std::string* victim = nullptr;
            uint64_t oldest = 0;
            bool first = true;
            for (const auto& entry : cache)
            {
                if (entry.second.generation.use_count() != 1)
                    continue; // pinned by an outside holder: unevictable
                if (first || entry.second.lru < oldest)
                {
                    victim = &entry.first;
                    oldest = entry.second.lru;
                    first = false;
                }
            }
            if (victim == nullptr)
            {
                Error error;
                error.code = Error::Io;
                error.path = clipKey;
                error.detail = "A5 budget provider: clip '" + clipKey +
                               "' needs " + std::to_string(need) +
                               " bytes with " + std::to_string(resident) +
                               " resident under budget " +
                               std::to_string(budgetBytes);
                return ResultGen::Fail(error.code, error.path, error.detail);
            }
            resident -= cache[*victim].bytes;
            cache.erase(*victim);
        }
        auto generation = std::make_shared<DecodedAudioGeneration>();
        generation->channels = 1;
        generation->sampleRate = 48000;
        generation->frameCount = static_cast<uint32_t>(need / 4);
        generation->pcmInterleaved.assign(
            static_cast<size_t>(generation->frameCount), 0.25f);
        Entry entry;
        entry.generation = generation;
        entry.bytes = need;
        entry.lru = ++clock;
        cache[clipKey] = std::move(entry);
        return ResultGen::Ok(std::move(generation));
    }
};

struct A5Fixture
{
    DeterministicUuidProvider ids;
    SceneManager manager;

    A5Fixture() { manager.SetUuidProvider(&ids); }

    UUID Create(const char* name)
    {
        return manager.CreateEmpty(name).affectedEntities.front();
    }

    UUID CreateChild(const char* name, const UUID& parent)
    {
        return manager.CreateEmpty(name, parent).affectedEntities.front();
    }

    entt::entity Handle(const UUID& uuid) const
    {
        return manager.FindEntityByUuid(uuid);
    }

    entt::registry& Registry() { return manager.GetECS().registry; }
    const SceneDocument& Authoring() const { return manager.AuthoringDoc(); }
};

AudioSourceComponent A5BoundSource(const std::string& path, bool autoplay,
                                   bool loop, bool spatial)
{
    AudioSourceComponent source;
    source.clip.kind = AssetKind::AudioClip;
    source.clip.path = path;
    source.clip.assetId = A5NextUuid();
    source.bus = AudioBus::Effects;
    source.autoplay = autoplay;
    source.loop = loop;
    source.spatial = spatial;
    source.gain = 1.0f;
    source.pitch = 1.0f;
    source.minDistance = 1.0f;
    source.maxDistance = 30.0f;
    source.rolloff = 1.0f;
    source.priority = 128;
    return source;
}

std::string A5Key(const std::string& path)
{
    // Mirrors the controller's default clip-key builder exactly. A test
    // that scripts the wrong key observes zero fetches for its generation
    // and fails at Play, which is itself the key-derivation proof.
    return "audioclip:" + path;
}

std::shared_ptr<const DecodedAudioGeneration> A5MonoGeneration()
{
    auto generation = std::make_shared<DecodedAudioGeneration>();
    generation->channels = 1;
    generation->sampleRate = 48000;
    generation->frameCount = 8;
    generation->pcmInterleaved.assign(
        static_cast<size_t>(generation->frameCount), 0.25f);
    return generation;
}

std::shared_ptr<const DecodedAudioGeneration> A5StereoGeneration()
{
    auto generation = std::make_shared<DecodedAudioGeneration>();
    generation->channels = 2;
    generation->sampleRate = 48000;
    generation->frameCount = 8;
    generation->pcmInterleaved.assign(
        static_cast<size_t>(generation->frameCount) * 2, 0.25f);
    return generation;
}

AudioListenerPose A5Listener(float x, float y, float z)
{
    AudioListenerPose listener;
    listener.position[0] = x;
    listener.position[1] = y;
    listener.position[2] = z;
    listener.forward[0] = 0.0f;
    listener.forward[1] = 0.0f;
    listener.forward[2] = -1.0f;
    listener.up[0] = 0.0f;
    listener.up[1] = 1.0f;
    listener.up[2] = 0.0f;
    return listener;
}

Error A5CorruptError()
{
    Error error;
    error.code = Error::Io;
    error.path = "audio clip bytes";
    error.detail = "A5 fixture: corrupt non-autoplay content";
    return error;
}

void A5WireAudio(RuntimeSceneController& ctrl,
                 RecordingFakeAudioBackend& fake)
{
    ctrl.SetAudioBackend(&fake);
    ctrl.SetAudioClipProvider(&fake);
}

void A5SetTranslation(A5Fixture& f, const UUID& uuid, float x, float y,
                      float z)
{
    auto e = f.Handle(uuid);
    REQUIRE((e != entt::null));
    auto* tf = f.Registry().try_get<Transform>(e);
    REQUIRE(tf != nullptr);
    tf->translation = glm::vec3(x, y, z);
    SceneGraph::MarkDirty(f.Registry(), e);
}

BackendVoiceMix A5RequireMix(AudioWorld& world, const UUID& source)
{
    std::vector<AudioWorldVoiceHandle> voices =
        world.LiveVoicesForSource(source);
    REQUIRE(voices.size() == 1);
    BackendVoiceMix mix;
    REQUIRE(world.GetVoiceMix(voices[0], mix));
    return mix;
}

double A5RequireCursor(AudioWorld& world, const UUID& source)
{
    std::vector<AudioWorldVoiceHandle> voices =
        world.LiveVoicesForSource(source);
    REQUIRE(voices.size() == 1);
    double cursor = 0.0;
    REQUIRE(world.GetVoiceCursor(voices[0], cursor));
    return cursor;
}

} // namespace

TEST_CASE("A5_PlayRefusesCorruptNonAutoplay_ZeroSessionState")
{
    // Required check 4: one valid autoplay source plus one corrupt
    // NON-autoplay source refuses Play with zero session voices, handles,
    // callbacks, and bridge calls. The obvious single mutation (decode only
    // autoplay sources in the candidate) turns this green-then-red.
    A5Fixture f;
    const UUID good = f.Create("GoodAutoplay");
    f.Registry().emplace<AudioSourceComponent>(
        f.Handle(good), A5BoundSource("audio/good.wav", true, false, false));
    const UUID bad = f.Create("BadIdle");
    f.Registry().emplace<AudioSourceComponent>(
        f.Handle(bad), A5BoundSource("audio/bad.flac", false, false, false));

    RecordingFakeAudioBackend fake;
    fake.ScriptGeneration(A5Key("audio/good.wav"), A5MonoGeneration());
    fake.ScriptGenerationError(A5Key("audio/bad.flac"), A5CorruptError());

    A5RecordingBridge bridge;
    A5Observer obs;
    RuntimeSceneController ctrl;
    ctrl.SetLifecycleObserver(&obs);
    A5WireAudio(ctrl, fake);

    const size_t liveBefore = PhysicsWorld::LiveWorldCount();
    Error err;
    CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
    CHECK_FALSE(err.IsOk());
    const bool namesBadClip = err.detail.find("bad.flac") < err.detail.size();
    CHECK(namesBadClip);
    // Atomic refusal: Edit, zero accumulator, no runtime, no audio world,
    // no backend voices, no bridge traffic, no script callbacks.
    CHECK(ctrl.GetState() == SceneRunState::Edit);
    CHECK(ctrl.TryGetRuntimeScene() == nullptr);
    CHECK(ctrl.TryGetAudioWorld() == nullptr);
    CHECK(ctrl.AudioLiveVoiceCount() == 0);
    CHECK(fake.LiveTokenCount() == 0);
    CHECK(fake.starts.empty());
    CHECK(bridge.Quiet());
    CHECK(obs.starts == 0);
    CHECK(obs.stops == 0);
    CHECK(ctrl.DebugAccumulator() == doctest::Approx(0.0f));
    CHECK(PhysicsWorld::LiveWorldCount() == liveBefore);

    // Recoverable: the same scene with valid content Plays clean.
    A5Fixture g;
    const UUID good2 = g.Create("GoodAutoplay");
    g.Registry().emplace<AudioSourceComponent>(
        g.Handle(good2), A5BoundSource("audio/good.wav", true, false, false));
    const UUID idle2 = g.Create("IdleOk");
    g.Registry().emplace<AudioSourceComponent>(
        g.Handle(idle2), A5BoundSource("audio/ok.flac", false, false, false));
    RecordingFakeAudioBackend fake2;
    fake2.ScriptGeneration(A5Key("audio/good.wav"), A5MonoGeneration());
    fake2.ScriptGeneration(A5Key("audio/ok.flac"), A5MonoGeneration());
    RuntimeSceneController ctrl2;
    A5WireAudio(ctrl2, fake2);
    Error err2;
    REQUIRE(ctrl2.Play(g.Authoring(), bridge, err2));
    CHECK(err2.IsOk());
    CHECK(ctrl2.TryGetAudioWorld() != nullptr);
    ctrl2.Stop(g.Authoring(), bridge);
    CHECK(ctrl2.GetState() == SceneRunState::Edit);
}

TEST_CASE("A5_PlayRefusesSpatialStereo_ZeroSessionState")
{
    // Check 9 (runtime half): a spatial source whose decoded generation is
    // stereo refuses Play loudly. The mutation (skip the channel check in
    // the candidate) Plays clean and turns this red.
    A5Fixture f;
    const UUID emitter = f.Create("SpatialStereo");
    f.Registry().emplace<AudioSourceComponent>(
        f.Handle(emitter),
        A5BoundSource("audio/stereo.wav", true, false, true));

    RecordingFakeAudioBackend fake;
    fake.ScriptGeneration(A5Key("audio/stereo.wav"), A5StereoGeneration());

    A5RecordingBridge bridge;
    RuntimeSceneController ctrl;
    A5WireAudio(ctrl, fake);
    Error err;
    CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
    CHECK_FALSE(err.IsOk());
    const bool namesMonoRule = err.detail.find("mono") < err.detail.size();
    CHECK(namesMonoRule);
    CHECK(ctrl.GetState() == SceneRunState::Edit);
    CHECK(ctrl.TryGetAudioWorld() == nullptr);
    CHECK(fake.LiveTokenCount() == 0);
    CHECK(bridge.Quiet());
}

TEST_CASE("A5_PlayWithoutSeams_RefusesBoundSourcesButAllowsEmpty")
{
    // Loud-seam proof: bound sources with no installed backend/provider
    // refuse Play (silent no-audio would be the characteristic swallowed
    // failure), while a sourceless scene still Plays with no audio world.
    // The mutation (proceed without seams) Plays the bound scene clean.
    A5Fixture f;
    const UUID emitter = f.Create("Bound");
    f.Registry().emplace<AudioSourceComponent>(
        f.Handle(emitter),
        A5BoundSource("audio/bound.wav", true, false, false));

    A5RecordingBridge bridge;
    RuntimeSceneController ctrl;
    Error err;
    CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
    CHECK_FALSE(err.IsOk());
    CHECK(err.code == Error::InvalidRuntimeState);
    CHECK(ctrl.GetState() == SceneRunState::Edit);
    CHECK(bridge.Quiet());

    A5Fixture g;
    g.Create("Plain");
    RuntimeSceneController ctrl2;
    Error err2;
    REQUIRE(ctrl2.Play(g.Authoring(), bridge, err2));
    CHECK(ctrl2.TryGetAudioWorld() == nullptr);
    ctrl2.Update(kFixedDt, bridge);
    CHECK(bridge.renderRequests == 1);
    ctrl2.Stop(g.Authoring(), bridge);
    CHECK(ctrl2.GetState() == SceneRunState::Edit);
}

TEST_CASE("A5_AutoplayStagedThenSynthesized_AfterFirstUpdate")
{
    // Transactional Play: the candidate stages autoplay with no backend
    // voice started; the first post-transform Update synthesizes it. The
    // mutation (QueuePlay at candidate time, or synthesize before the
    // drain) shows starts at commit or misordered execution.
    A5Fixture f;
    const UUID emitter = f.Create("Autoplay");
    f.Registry().emplace<AudioSourceComponent>(
        f.Handle(emitter),
        A5BoundSource("audio/auto.wav", true, false, false));

    RecordingFakeAudioBackend fake;
    A5RecordingBridge bridge;
    RuntimeSceneController ctrl;
    A5WireAudio(ctrl, fake);
    Error err;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
    REQUIRE(ctrl.TryGetAudioWorld() != nullptr);
    // Staged, not started: no voice, no backend start, no mix publication.
    CHECK(ctrl.AudioStagedAutoplayCount() == 1);
    CHECK(ctrl.AudioLiveVoiceCount() == 0);
    CHECK(fake.starts.empty());
    CHECK(fake.LiveTokenCount() == 0);

    bridge.Reset();
    ctrl.Update(kFixedDt, bridge);
    CHECK(ctrl.AudioStagedAutoplayCount() == 0);
    CHECK(ctrl.AudioLiveVoiceCount() == 1);
    CHECK(fake.starts.size() == 1);
    CHECK(bridge.renderRequests == 1);
    ctrl.Stop(f.Authoring(), bridge);
    CHECK(ctrl.AudioLiveVoiceCount() == 0);
    CHECK(fake.LiveTokenCount() == 0);
}

TEST_CASE("A5_OnCreateStop_SuppressesPendingAutoplay")
{
    // Check 18 (first half): an accepted Stop before the first Update
    // suppresses pending autoplay even though no voice exists. The
    // mutation (queue autoplay as an ordinary staged command, or
    // synthesize before draining) starts a voice and turns this red.
    A5Fixture f;
    const UUID emitter = f.Create("Autoplay");
    f.Registry().emplace<AudioSourceComponent>(
        f.Handle(emitter),
        A5BoundSource("audio/auto.wav", true, false, false));

    RecordingFakeAudioBackend fake;
    A5RecordingBridge bridge;
    RuntimeSceneController ctrl;
    A5WireAudio(ctrl, fake);
    Error err;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
    REQUIRE(ctrl.AudioStagedAutoplayCount() == 1);

    // Simulates the on_create Stop accepted into the session queue before
    // the first post-transform audio slot runs.
    AudioWorld* world = ctrl.TryGetAudioWorld();
    REQUIRE(world != nullptr);
    uint64_t seq = 0;
    REQUIRE(world->QueueStop(emitter, seq));

    ctrl.Update(kFixedDt, bridge);
    CHECK(ctrl.AudioStagedAutoplayCount() == 0);
    CHECK(ctrl.AudioLiveVoiceCount() == 0);
    CHECK(fake.starts.empty());
    CHECK(fake.LiveTokenCount() == 0);
    CHECK(bridge.renderRequests == 1);
    ctrl.Stop(f.Authoring(), bridge);
}

TEST_CASE("A5_GainThenPlay_AppliesFifoWithoutDuplicate")
{
    // Check 18 (second half): gain accepted before synthesis merges into
    // the synthesized play in FIFO order, and an explicitly executed Play
    // consumes staging so no duplicate voice appears. The mutation
    // (synthesize from the staged snapshot without merging state, or
    // synthesize after an executed Play) yields unity gain or two voices.
    A5Fixture f;
    const UUID emitter = f.Create("Autoplay");
    f.Registry().emplace<AudioSourceComponent>(
        f.Handle(emitter),
        A5BoundSource("audio/auto.wav", true, false, false));

    RecordingFakeAudioBackend fake;
    A5RecordingBridge bridge;
    RuntimeSceneController ctrl;
    A5WireAudio(ctrl, fake);
    Error err;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
    AudioWorld* world = ctrl.TryGetAudioWorld();
    REQUIRE(world != nullptr);

    // Gain accepted after staging, before the first slot: synthesis must
    // observe it (FIFO), not the staged unity snapshot.
    uint64_t seq = 0;
    REQUIRE(world->QueueSetGain(emitter, 0.5f, seq));

    ctrl.Update(kFixedDt, bridge);
    CHECK(world->LiveVoicesForSource(emitter).size() == 1);
    CHECK(fake.starts.size() == 1);
    const BackendVoiceMix mix = A5RequireMix(*world, emitter);
    CHECK(mix.left == doctest::Approx(0.5f * kCenter));
    CHECK(mix.right == doctest::Approx(0.5f * kCenter));
    ctrl.Stop(f.Authoring(), bridge);

    // Explicit Play consumes staging: exactly one voice, one backend
    // start, no synthesized duplicate afterwards.
    A5Fixture g;
    const UUID emitter2 = g.Create("Autoplay");
    g.Registry().emplace<AudioSourceComponent>(
        g.Handle(emitter2),
        A5BoundSource("audio/auto.wav", true, false, false));
    RecordingFakeAudioBackend fake2;
    RuntimeSceneController ctrl2;
    A5WireAudio(ctrl2, fake2);
    Error err2;
    REQUIRE(ctrl2.Play(g.Authoring(), bridge, err2));
    AudioWorld* world2 = ctrl2.TryGetAudioWorld();
    REQUIRE(world2 != nullptr);
    AudioPlayRequest directPlay;
    directPlay.source = emitter2;
    directPlay.component = A5BoundSource("audio/auto.wav", true, false, false);
    directPlay.hasTransform = false;
    directPlay.clipKey = A5Key("audio/auto.wav");
    uint64_t seq2 = 0;
    REQUIRE(world2->QueuePlay(directPlay, seq2));
    ctrl2.Update(kFixedDt, bridge);
    CHECK(world2->LiveVoicesForSource(emitter2).size() == 1);
    CHECK(fake2.starts.size() == 1);
    CHECK(world2->StagedAutoplayCount() == 0);
    ctrl2.Stop(g.Authoring(), bridge);
}

TEST_CASE("A5_RuntimeCameraMotion_ChangesListenerMix")
{
    // Required check 10: moving the injected host pose changes the
    // listener mix while no camera entity moves, proving the controller
    // uses the actual rendered Camera. The mutation (derive the listener
    // from an authored camera entity, or freeze the pose at Play) leaves
    // the second mix identical.
    A5Fixture f;
    const UUID emitter = f.Create("RightEmitter");
    f.Registry().emplace<AudioSourceComponent>(
        f.Handle(emitter),
        A5BoundSource("audio/emit.wav", true, false, true));
    A5SetTranslation(f, emitter, 5.0f, 0.0f, 0.0f);

    RecordingFakeAudioBackend fake;
    A5RecordingBridge bridge;
    RuntimeSceneController ctrl;
    A5WireAudio(ctrl, fake);
    Error err;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
    AudioWorld* world = ctrl.TryGetAudioWorld();
    REQUIRE(world != nullptr);

    // Listener at the origin facing -Z: source at +X is hard right.
    ctrl.SetAudioListenerPose(A5Listener(0.0f, 0.0f, 0.0f));
    ctrl.Update(kFixedDt, bridge);
    REQUIRE(ctrl.AudioLiveVoiceCount() == 1);
    const BackendVoiceMix hardRight = A5RequireMix(*world, emitter);
    CHECK(hardRight.left < 1.0e-4f);
    CHECK(hardRight.right > 0.5f);

    // Same facing, moved past the source: now hard left. No entity moved.
    ctrl.SetAudioListenerPose(A5Listener(10.0f, 0.0f, 0.0f));
    ctrl.Update(kFixedDt, bridge);
    const BackendVoiceMix hardLeft = A5RequireMix(*world, emitter);
    CHECK(hardLeft.left > 0.5f);
    CHECK(hardLeft.right < 1.0e-4f);
    ctrl.Stop(f.Authoring(), bridge);
}

TEST_CASE("A5_PhysicsMovingChild_UsesFinalWorldTransform")
{
    // Required check 11: a child emitter moved by Bullet through its
    // parent uses the final world-matrix translation, not its local TRS.
    // The parent carries a live dynamic body driven by a queued velocity;
    // the mutation (pose from local translation) keeps the far mix at the
    // near gain even though the child local offset never changes.
    A5Fixture f;
    const UUID parent = f.Create("Parent");
    PhysicsBodyComponent body;
    body.kind = PhysicsBodyKind::Dynamic;
    body.mass = 1.5f;
    body.layer = PhysicsLayer::Dynamic;
    body.mask = PhysicsLayer::WorldStatic | PhysicsLayer::Mechanism;
    f.Registry().emplace<PhysicsBodyComponent>(f.Handle(parent), body);
    PhysicsShapeComponent shape;
    shape.shape = PhysicsShapeKind::Sphere;
    shape.radius = 0.5f;
    f.Registry().emplace<PhysicsShapeComponent>(f.Handle(parent), shape);
    const UUID child = f.CreateChild("ChildEmitter", parent);
    f.Registry().emplace<AudioSourceComponent>(
        f.Handle(child),
        A5BoundSource("audio/emit.wav", true, false, true));
    A5SetTranslation(f, child, 1.0f, 0.0f, 0.0f);

    RecordingFakeAudioBackend fake;
    A5RecordingBridge bridge;
    RuntimeSceneController ctrl;
    A5WireAudio(ctrl, fake);
    Error err;
    // No collision provider: the procedural sphere needs no asset decode.
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
    AudioWorld* world = ctrl.TryGetAudioWorld();
    REQUIRE(world != nullptr);
    ctrl.SetAudioListenerPose(A5Listener(0.0f, 0.0f, 0.0f));

    // Child world position is x=1 == minDistance: unity distance gain.
    ctrl.Update(kFixedDt, bridge);
    const BackendVoiceMix near = A5RequireMix(*world, child);
    CHECK(std::fabs(near.left) < 1.0e-4f);
    CHECK(near.right == doctest::Approx(1.0f));

    // Drive the parent through Bullet (queued velocity, committed world,
    // write-back into the runtime Transform) for one simulated second.
    REQUIRE(ctrl.QueueSetLinearVelocity(parent, glm::vec3(10.0f, 0.0f, 0.0f)));
    for (int i = 0; i < 60; ++i)
        ctrl.Update(kFixedDt, bridge);

    // Bullet write-back proof: the parent's runtime translation moved.
    const SceneDocument* runtime = ctrl.TryGetRuntimeScene();
    REQUIRE(runtime != nullptr);
    const auto parentEntity = runtime->FindByUuid(parent);
    REQUIRE((parentEntity != entt::null));
    const auto* parentTf =
        runtime->ecs.registry.try_get<Transform>(parentEntity);
    REQUIRE(parentTf != nullptr);
    CHECK(parentTf->translation.x > 5.0f);

    // The child's LOCAL offset never changed: only the final world matrix
    // can explain the new mix.
    const auto childEntity = runtime->FindByUuid(child);
    REQUIRE((childEntity != entt::null));
    const auto* childTf =
        runtime->ecs.registry.try_get<Transform>(childEntity);
    REQUIRE(childTf != nullptr);
    CHECK(childTf->translation.x == doctest::Approx(1.0f));
    const glm::vec3 finalWorld = glm::vec3(childTf->worldMatrix[3]);
    const float d = std::sqrt(finalWorld.x * finalWorld.x +
                              finalWorld.y * finalWorld.y +
                              finalWorld.z * finalWorld.z);
    REQUIRE(d > 1.0f);
    // READY-plan spatial oracle from the actual final translation:
    // distanceGain = (1 - (d-min)/(max-min))^rolloff, equal-power pan
    // from the +X side (listener faces -Z with +Y up, so right = +X).
    const float distGain = 1.0f - (d - 1.0f) / 29.0f;
    const float pan = std::max(-1.0f, std::min(1.0f, finalWorld.x / d));
    const float angle = (pan + 1.0f) * 3.14159265f / 4.0f;
    const BackendVoiceMix far = A5RequireMix(*world, child);
    CHECK(far.left == doctest::Approx(std::cos(angle) * distGain).epsilon(1.0e-3));
    CHECK(far.right == doctest::Approx(std::sin(angle) * distGain).epsilon(1.0e-3));
    // Motion took effect through the world matrix: the mix moved well
    // off the near/local prediction (unity gain, hard right).
    CHECK(std::fabs(far.right - near.right) > 0.05f);
    ctrl.Stop(f.Authoring(), bridge);
}

TEST_CASE("A5_PauseResumeStep_SampleFrozen")
{
    // Required check 12: session Pause freezes all runtime voices, a voice
    // started while paused is initially frozen, Step changes pose but not
    // the playback cursor, and Resume continues rather than restarts. The
    // mutation (skip SetSessionPaused, advance cursors on Step, or retrigger
    // on Resume) breaks the pause record, the cursor freeze, or the start
    // count.
    A5Fixture f;
    const UUID emitter = f.Create("Loop");
    f.Registry().emplace<AudioSourceComponent>(
        f.Handle(emitter),
        A5BoundSource("audio/loop.wav", true, true, false));
    // A second, non-autoplay source for the while-paused start below: the
    // emitter's live loop would idempotently absorb an explicit Play, so
    // the initially-paused start needs its own source.
    const UUID waiter = f.Create("Waiter");
    f.Registry().emplace<AudioSourceComponent>(
        f.Handle(waiter),
        A5BoundSource("audio/wait.wav", false, false, false));

    RecordingFakeAudioBackend fake;
    A5RecordingBridge bridge;
    RuntimeSceneController ctrl;
    A5WireAudio(ctrl, fake);
    Error err;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
    AudioWorld* world = ctrl.TryGetAudioWorld();
    REQUIRE(world != nullptr);
    ctrl.SetAudioListenerPose(A5Listener(0.0f, 0.0f, 0.0f));
    ctrl.Update(kFixedDt, bridge);
    REQUIRE(ctrl.AudioLiveVoiceCount() == 1);
    // One 1/60s frame at 48 kHz advances exactly 800 sample frames.
    CHECK(A5RequireCursor(*world, emitter) == doctest::Approx(800.0));
    CHECK(fake.renderCalls == 0);

    ctrl.Pause();
    REQUIRE(world->IsSessionPaused());
    REQUIRE(fake.sessionPauses.size() == 1);
    CHECK(fake.sessionPauses.back() == std::make_pair(world->Session(), true));

    // A voice started while paused carries initialPaused.
    AudioPlayRequest pausedPlay;
    pausedPlay.source = waiter;
    pausedPlay.component = A5BoundSource("audio/wait.wav", false, false, false);
    pausedPlay.hasTransform = false;
    pausedPlay.clipKey = A5Key("audio/wait.wav");
    uint64_t seq = 0;
    REQUIRE(world->QueuePlay(pausedPlay, seq));
    const double cursorBefore = A5RequireCursor(*world, emitter);
    REQUIRE(ctrl.Step(bridge));
    REQUIRE(fake.starts.size() == 2);
    CHECK(fake.starts.back().start.initialPaused);
    // Step is sample-frozen: neither cursor moved.
    CHECK(A5RequireCursor(*world, emitter) == doctest::Approx(cursorBefore));
    CHECK(A5RequireCursor(*world, waiter) == doctest::Approx(0.0));
    CHECK(fake.renderCalls == 0);

    const size_t startsBeforeResume = fake.starts.size();
    REQUIRE(ctrl.Resume());
    REQUIRE_FALSE(world->IsSessionPaused());
    CHECK(fake.sessionPauses.back() == std::make_pair(world->Session(), false));
    ctrl.Update(kFixedDt, bridge);
    // Continued, not restarted: no new backend start, cursor advanced.
    CHECK(fake.starts.size() == startsBeforeResume);
    CHECK(A5RequireCursor(*world, emitter) > cursorBefore);
    ctrl.Stop(f.Authoring(), bridge);
    CHECK(fake.LiveTokenCount() == 0);
}

TEST_CASE("A5_DestroyStopsVoices_BeforeCallbacksAndErase")
{
    // Required checks 6/14: destroying a looping emitter stops exactly its
    // voices before ECS removal, through one post-order [child, parent]
    // set shared by audio, script callbacks, physics, and ECS. The
    // mutation (per-subsystem recollection, or audio teardown after ECS
    // erase) misorders the callback observation or leaks a voice.
    A5Fixture f;
    const UUID parent = f.Create("ParentLoop");
    f.Registry().emplace<AudioSourceComponent>(
        f.Handle(parent),
        A5BoundSource("audio/parent.wav", true, true, false));
    const UUID child = f.CreateChild("ChildLoop", parent);
    f.Registry().emplace<AudioSourceComponent>(
        f.Handle(child),
        A5BoundSource("audio/child.wav", true, true, false));

    RecordingFakeAudioBackend fake;
    A5RecordingBridge bridge;
    A5Observer obs;
    A5DestroyProbe probe;
    RuntimeSceneController ctrl;
    ctrl.SetLifecycleObserver(&obs);
    probe.ctrl = &ctrl;
    probe.refuseAudioArmed = true;
    ctrl.SetScriptDispatch(&probe);
    A5WireAudio(ctrl, fake);
    Error err;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
    ctrl.Update(kFixedDt, bridge);
    REQUIRE(ctrl.AudioLiveVoiceCount() == 2);
    REQUIRE(fake.starts.size() == 2);

    REQUIRE(ctrl.QueueDestroyRuntimeEntity(parent).IsOk());
    ctrl.Update(kFixedDt, bridge);

    // One authoritative post-order set observed by the script callback.
    REQUIRE(probe.destroyCallCount == 1);
    REQUIRE(probe.destroyCalls.front().size() == 2);
    CHECK(probe.destroyCalls.front()[0] == child);
    CHECK(probe.destroyCalls.front()[1] == parent);
    // Audio stopped both voices BEFORE the callback ran (not after erase).
    CHECK(probe.voicesStoppedAtCallback);
    CHECK(ctrl.AudioLiveVoiceCount() == 0);
    CHECK(fake.stops.size() == 2);
    CHECK(fake.LiveTokenCount() == 0);
    // Re-entrant audio commands to the dying UUIDs refused.
    CHECK(probe.audioRefusalsObserved);
    // ECS erased the same membership.
    const SceneDocument* runtime = ctrl.TryGetRuntimeScene();
    REQUIRE(runtime != nullptr);
    CHECK_FALSE(runtime->uuidIndex.Contains(parent));
    CHECK_FALSE(runtime->uuidIndex.Contains(child));
    ctrl.Stop(f.Authoring(), bridge);
    CHECK(obs.stops == 1);
    CHECK(fake.LiveTokenCount() == 0);
}

TEST_CASE("A5_NoScriptSystem_SameBatchCreateDestroy_OneSubtree")
{
    // Required check 14 (no-ScriptSystem half): create parent, create
    // child under the parent, and destroy the parent in one frozen batch
    // with no ScriptSystem installed. The single destroy-position
    // recollection observes the real [child, parent] subtree; audio,
    // physics, and ECS all tear down that same membership. The mutation
    // (precomputed-only membership, or a script-branch-gated recollect)
    // leaves the batch-created child alive or crashes on the missing set.
    A5Fixture f;
    const UUID doomed = f.Create("DoomedLoop");
    f.Registry().emplace<AudioSourceComponent>(
        f.Handle(doomed),
        A5BoundSource("audio/doomed.wav", true, true, false));

    RecordingFakeAudioBackend fake;
    A5RecordingBridge bridge;
    RuntimeSceneController ctrl;
    DeterministicUuidProvider runtimeIds;
    ctrl.SetRuntimeUuidProvider(&runtimeIds);
    // No ScriptDispatch installed: this path must not depend on it.
    A5WireAudio(ctrl, fake);
    Error err;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
    ctrl.Update(kFixedDt, bridge);
    REQUIRE(ctrl.AudioLiveVoiceCount() == 1);

    RuntimeEntityCreateDesc descA;
    descA.name = "BatchParent";
    auto rA = ctrl.QueueCreateRuntimeEntity(descA);
    REQUIRE(rA.IsOk());
    RuntimeEntityCreateDesc descB;
    descB.name = "BatchChild";
    descB.parentUuid = rA.value;
    auto rB = ctrl.QueueCreateRuntimeEntity(descB);
    REQUIRE(rB.IsOk());
    REQUIRE(ctrl.QueueDestroyRuntimeEntity(rA.value).IsOk());
    REQUIRE(ctrl.QueueDestroyRuntimeEntity(doomed).IsOk());

    ctrl.Update(kFixedDt, bridge);

    // The same-batch subtree is gone, with no residue for the next drain.
    const SceneDocument* runtime = ctrl.TryGetRuntimeScene();
    REQUIRE(runtime != nullptr);
    CHECK_FALSE(runtime->uuidIndex.Contains(rA.value));
    CHECK_FALSE(runtime->uuidIndex.Contains(rB.value));
    CHECK_FALSE(runtime->uuidIndex.Contains(doomed));
    CHECK(ctrl.PendingOperationCount() == 0);
    // Audio tore down the doomed voice with the same destroy position.
    CHECK(ctrl.AudioLiveVoiceCount() == 0);
    CHECK(fake.LiveTokenCount() == 0);
    // A quiet follow-up frame proves no half-torn state survived.
    bridge.Reset();
    ctrl.Update(kFixedDt, bridge);
    CHECK(bridge.renderRequests == 1);
    ctrl.Stop(f.Authoring(), bridge);
    CHECK(ctrl.GetState() == SceneRunState::Edit);
}

TEST_CASE("A5_Stop_ZeroCensusBeforeCloneDestroy_TwentyCycles")
{
    // Session-lifetime gate: Stop clears commands and session voices
    // before the runtime clone is destroyed, and 20 repeated Play/Stop
    // cycles return every census to baseline. The mutation (audio
    // teardown after clone reset, or a leaked per-session voice) fails
    // the first Stop or drifts a later cycle.
    A5Fixture f;
    const UUID emitter = f.Create("Loop");
    f.Registry().emplace<AudioSourceComponent>(
        f.Handle(emitter),
        A5BoundSource("audio/loop.wav", true, true, false));

    RecordingFakeAudioBackend fake;
    A5RecordingBridge bridge;
    RuntimeSceneController ctrl;
    A5WireAudio(ctrl, fake);
    const size_t liveBefore = PhysicsWorld::LiveWorldCount();
    const AudioSessionId firstSession = ctrl.AudioSession();
    CHECK_FALSE(firstSession.IsValid());

    for (int cycle = 0; cycle < 20; ++cycle)
    {
        Error err;
        REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
        REQUIRE(ctrl.TryGetAudioWorld() != nullptr);
        REQUIRE(ctrl.TryGetAudioWorld()->Session().IsValid());
        ctrl.Update(kFixedDt, bridge);
        REQUIRE(ctrl.AudioLiveVoiceCount() == 1);
        ctrl.Stop(f.Authoring(), bridge);
        CHECK(ctrl.GetState() == SceneRunState::Edit);
        CHECK(ctrl.TryGetAudioWorld() == nullptr);
        CHECK(ctrl.TryGetRuntimeScene() == nullptr);
        CHECK(ctrl.AudioLiveVoiceCount() == 0);
        CHECK(ctrl.AudioQueuedCommandCount() == 0);
        CHECK(fake.LiveTokenCount() == 0);
        // Shutdown plus the world destructor's best-effort session stop
        // each record one session stop; the count must grow every cycle.
        CHECK(fake.stopSessions.size() >= static_cast<size_t>(cycle + 1));
        CHECK(PhysicsWorld::LiveWorldCount() == liveBefore);
    }
}

TEST_CASE("A5_NoDeviceRenderFollowsSemanticUpdate")
{
    // A5 fixup finding 1: the no-device PCM block must render AFTER the
    // semantic phase (drain, first-frame autoplay, final mixes), with the
    // cursor advancing by exactly what rendered. The mutation (render
    // before the semantic phase, the pre-fixup order) fills the first
    // block with the 0-voice marker while still advancing the cursor.
    A5Fixture f;
    const UUID emitter = f.Create("Autoplay");
    f.Registry().emplace<AudioSourceComponent>(
        f.Handle(emitter),
        A5BoundSource("audio/auto.wav", true, false, false));

    A5NoDeviceOrderBackend backend;
    A5RecordingBridge bridge;
    RuntimeSceneController ctrl;
    ctrl.SetAudioBackend(&backend);
    ctrl.SetAudioClipProvider(&backend);
    Error err;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
    AudioWorld* world = ctrl.TryGetAudioWorld();
    REQUIRE(world != nullptr);
    REQUIRE(world->LiveVoiceCount() == 0);

    ctrl.Update(kFixedDt, bridge);
    REQUIRE(backend.sights.size() == 1);
    const auto& sight = backend.sights.front();
    // The semantic phase started the autoplay voice before PCM rendered.
    CHECK(sight.liveVoices == 1);
    REQUIRE(sight.buffer != nullptr);
    CHECK(sight.buffer[0] == doctest::Approx(1.0f));
    CHECK(sight.buffer[799 * 2 + 1] == doctest::Approx(1.0f));
    // Cursor accounting matches exactly what rendered: one 1/60s block.
    CHECK(A5RequireCursor(*world, emitter) == doctest::Approx(800.0));
    ctrl.Stop(f.Authoring(), bridge);
    CHECK(backend.LiveTokenCount() == 0);
}

TEST_CASE("A5_NoDeviceSameFrameStopRendersSilence")
{
    // A5 fixup finding 1 (second leg): a Stop drained in the semantic
    // phase takes effect in the same frame's PCM block. The mutation
    // (render-first) renders the doomed voice for one more block.
    A5Fixture f;
    const UUID emitter = f.Create("Autoplay");
    f.Registry().emplace<AudioSourceComponent>(
        f.Handle(emitter),
        A5BoundSource("audio/auto.wav", true, false, false));

    A5NoDeviceOrderBackend backend;
    A5RecordingBridge bridge;
    RuntimeSceneController ctrl;
    ctrl.SetAudioBackend(&backend);
    ctrl.SetAudioClipProvider(&backend);
    Error err;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
    AudioWorld* world = ctrl.TryGetAudioWorld();
    REQUIRE(world != nullptr);
    ctrl.Update(kFixedDt, bridge);
    REQUIRE(world->LiveVoiceCount() == 1);

    uint64_t seq = 0;
    REQUIRE(world->QueueStop(emitter, seq));
    backend.ResetSights();
    ctrl.Update(kFixedDt, bridge);
    REQUIRE(backend.sights.size() == 1);
    CHECK(backend.sights.front().liveVoices == 0);
    CHECK(backend.sights.front().buffer[0] == doctest::Approx(0.0f));
    CHECK(world->LiveVoiceCount() == 0);
    CHECK(backend.LiveTokenCount() == 0);
    ctrl.Stop(f.Authoring(), bridge);
}

TEST_CASE("A5_NoDeviceRenderSeesRefreshedMix")
{
    // A5 fixup finding 1 (third leg): a listener move in the same frame
    // reaches the PCM block through the refreshed mix, not the stale one.
    // The mutation (render-first) publishes the previous frame's mix at
    // render time.
    A5Fixture f;
    const UUID emitter = f.Create("RightEmitter");
    f.Registry().emplace<AudioSourceComponent>(
        f.Handle(emitter),
        A5BoundSource("audio/emit.wav", true, false, true));
    A5SetTranslation(f, emitter, 5.0f, 0.0f, 0.0f);

    A5NoDeviceOrderBackend backend;
    A5RecordingBridge bridge;
    RuntimeSceneController ctrl;
    ctrl.SetAudioBackend(&backend);
    ctrl.SetAudioClipProvider(&backend);
    Error err;
    REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
    AudioWorld* world = ctrl.TryGetAudioWorld();
    REQUIRE(world != nullptr);
    ctrl.SetAudioListenerPose(A5Listener(0.0f, 0.0f, 0.0f));
    ctrl.Update(kFixedDt, bridge);
    REQUIRE(world->LiveVoiceCount() == 1);

    // Same facing, moved past the source: hard left at distance gain
    // (1 - (5-1)/(30-1)) = 25/29.
    ctrl.SetAudioListenerPose(A5Listener(10.0f, 0.0f, 0.0f));
    backend.ResetSights();
    const size_t mixesBefore = backend.inner.mixes.size();
    ctrl.Update(kFixedDt, bridge);
    REQUIRE(backend.sights.size() == 1);
    const auto& sight = backend.sights.front();
    CHECK(sight.liveVoices == 1);
    CHECK(sight.mixPubs > mixesBefore);
    REQUIRE(sight.hasMix);
    const float expected = 25.0f / 29.0f;
    CHECK(sight.lastMix.left == doctest::Approx(expected).epsilon(1.0e-4));
    CHECK(std::fabs(sight.lastMix.right) < 1.0e-4f);
    const BackendVoiceMix live = A5RequireMix(*world, emitter);
    CHECK(live.left == doctest::Approx(sight.lastMix.left));
    CHECK(live.right == doctest::Approx(sight.lastMix.right));
    ctrl.Stop(f.Authoring(), bridge);
}

TEST_CASE("A5_AppDetach_StopsSessionBeforeBackendShutdown")
{
    // A5 fixup finding 2: the app-detach sequence must Stop the active
    // runtime session while the bridge and backend are alive (OnSceneStop
    // plus explicit audio teardown and zero census) BEFORE the backend
    // shuts down. This mirrors WalnutApp::OnDetach's guarded EnterStop:
    // without it the world would still own voices at destruction and its
    // destructor would stop a session against a dead backend with no
    // OnSceneStop and no census assertion.
    A5Fixture f;
    const UUID emitter = f.Create("Loop");
    f.Registry().emplace<AudioSourceComponent>(
        f.Handle(emitter),
        A5BoundSource("audio/loop.wav", true, true, false));

    RecordingFakeAudioBackend backend;
    A5RecordingBridge bridge;
    A5Observer obs;
    {
        RuntimeSceneController ctrl;
        ctrl.SetLifecycleObserver(&obs);
        A5WireAudio(ctrl, backend);
        Error err;
        REQUIRE(ctrl.Play(f.Authoring(), bridge, err));
        ctrl.Update(kFixedDt, bridge);
        REQUIRE(ctrl.AudioLiveVoiceCount() == 1);

        // Detach with an active session: Stop first, backend alive.
        ctrl.Stop(f.Authoring(), bridge);
        CHECK(obs.stops == 1);
        CHECK(ctrl.TryGetAudioWorld() == nullptr);
        CHECK(ctrl.AudioLiveVoiceCount() == 0);
        CHECK(backend.LiveTokenCount() == 0);
        CHECK(backend.stopSessions.size() >= 1);
        // Leaving scope destroys a controller with no live world: no
        // further backend traffic may occur (the old order would stop a
        // live session here, silently skipping OnSceneStop and the
        // census asserts).
        const size_t stopsAtScopeExit = backend.stops.size();
        const size_t sessionStopsAtExit = backend.stopSessions.size();
        CHECK(backend.stops.size() == stopsAtScopeExit);
        CHECK(backend.stopSessions.size() == sessionStopsAtExit);
    }
    CHECK(obs.stops == 1);
}

TEST_CASE("A5_CandidatePinsGenerationsThroughCommit_BudgetRefusedPreCommit")
{
    // A5 fixup finding 3: two 1 MiB autoplay clips under a 1 MiB provider
    // budget refuse Play BEFORE commit (no voices, callbacks, or bridge
    // calls) because the candidate pins A while fetching B. The mutation
    // (fetch without retention) evicts A silently, commits, and fails a
    // voice only after callbacks and bridge sync have fired.
    A5Fixture f;
    const UUID clipA = f.Create("ClipA");
    f.Registry().emplace<AudioSourceComponent>(
        f.Handle(clipA),
        A5BoundSource("audio/a.wav", true, false, false));
    const UUID clipB = f.Create("ClipB");
    f.Registry().emplace<AudioSourceComponent>(
        f.Handle(clipB),
        A5BoundSource("audio/b.wav", true, false, false));

    A5PinBudgetProvider provider;
    provider.budgetBytes = 1048576;
    provider.scriptedBytes[A5Key("audio/a.wav")] = 1048576;
    provider.scriptedBytes[A5Key("audio/b.wav")] = 1048576;
    RecordingFakeAudioBackend backend;

    A5RecordingBridge bridge;
    A5Observer obs;
    RuntimeSceneController ctrl;
    ctrl.SetLifecycleObserver(&obs);
    ctrl.SetAudioBackend(&backend);
    ctrl.SetAudioClipProvider(&provider);
    Error err;
    CHECK_FALSE(ctrl.Play(f.Authoring(), bridge, err));
    CHECK_FALSE(err.IsOk());
    const bool namesBytes = err.detail.find("1048576") < err.detail.size();
    CHECK(namesBytes);
    CHECK(ctrl.GetState() == SceneRunState::Edit);
    CHECK(ctrl.TryGetRuntimeScene() == nullptr);
    CHECK(ctrl.TryGetAudioWorld() == nullptr);
    CHECK(ctrl.AudioPinnedGenerationCount() == 0);
    CHECK(backend.starts.empty());
    CHECK(backend.LiveTokenCount() == 0);
    CHECK(bridge.Quiet());
    CHECK(obs.starts == 0);
    CHECK(obs.stops == 0);

    // Adequate budget: both pin through commit and both voices start on
    // the first frame; pins release with the session.
    A5PinBudgetProvider roomy;
    roomy.budgetBytes = 2 * 1048576;
    roomy.scriptedBytes[A5Key("audio/a.wav")] = 1048576;
    roomy.scriptedBytes[A5Key("audio/b.wav")] = 1048576;
    RecordingFakeAudioBackend backend2;
    RuntimeSceneController ctrl2;
    ctrl2.SetAudioBackend(&backend2);
    ctrl2.SetAudioClipProvider(&roomy);
    Error err2;
    REQUIRE(ctrl2.Play(f.Authoring(), bridge, err2));
    CHECK(ctrl2.AudioPinnedGenerationCount() == 2);
    ctrl2.Update(kFixedDt, bridge);
    CHECK(ctrl2.AudioLiveVoiceCount() == 2);
    CHECK(backend2.starts.size() == 2);
    ctrl2.Stop(f.Authoring(), bridge);
    CHECK(ctrl2.AudioPinnedGenerationCount() == 0);
    CHECK(backend2.LiveTokenCount() == 0);
}
