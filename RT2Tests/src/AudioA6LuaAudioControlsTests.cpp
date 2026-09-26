// AudioA6LuaAudioControlsTests — audio A6 Lua controls and status.
//
// Typed entity audio_play/audio_play_at/audio_stop/audio_pause/
// audio_set_gain/audio_set_pitch plus audio_status through the portable
// IRuntimeCommandSink boundary: validated deferred admission into the
// bounded 256-command FIFO (`true` means accepted only), sequence-scoped
// asynchronous results, frozen-drain FIFO/re-entrancy, destroying-target
// refusal, and reload/quarantine/Stop/teardown queue cleanup.
//
// Production path: Lua-surface cases drive a REAL ScriptSystem +
// RuntimeCommandSink through RuntimeSceneController::Play/Update/Stop with
// scene-relative .lua files on disk (the T7 harness pattern), so the tests
// observe the binding parse gates, the controller queue, the
// presentation-frame drain, and the recording fake — never a helper in
// isolation. Refusal paths that cannot be reached from Lua (unknown UUIDs,
// Edit/Stop gates, queue-full) go through the sink/controller directly
// with the same assertions.
//
// Out of scope (explicit): automatic physics-event sound mapping, sound
// banks, editor/preview UI (A7), production miniaudio decode/device (A4
// probe), acceptance scene and durable docs (A8), pinball sound content.
//
// CPU boundary: this file includes only portable engine headers plus the
// standard library. The hard #error guard fails the build if miniaudio.h
// ever becomes reachable from RT2Tests.

#include <doctest/doctest.h>

#if __has_include("miniaudio.h")
#error "A6 boundary: RT2Tests must not import miniaudio (the adapter stays in RT2AudioBackend)"
#endif

#include "RuntimeSceneController.h"
#include "RuntimeLifecycleObserver.h"
#include "IRuntimeScriptDispatch.h"
#include "ScriptSystem.h"
#include "SceneGraph.h"
#include "SceneDocument.h"
#include "ECSComponents.h"
#include "ECSScene.h"
#include "ISceneRenderBridge.h"
#include "GPUSceneData.h"
#include "AudioBackend.h"
#include "AudioComponents.h"
#include "AudioWorld.h"
#include "FakeAudioBackend.h"
#include "core/Error.h"
#include "core/UUID.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace rt2::core;
using namespace rt2::audio;

namespace {

uint64_t g_A6UuidCounter = 0;

UUID A6NextUuid()
{
    ++g_A6UuidCounter;
    std::array<uint8_t, 16> bytes{};
    uint64_t n = g_A6UuidCounter;
    for (int i = 0; i < 8; ++i)
        bytes[15 - i] = static_cast<uint8_t>(n >> (8 * i));
    return UUID(bytes);
}

class A6NullBridge final : public ISceneRenderBridge
{
public:
    void FullSync(GPUSceneData&) override      {}
    void MaterialSync(GPUSceneData&) override  {}
    void TransformSync(GPUSceneData&) override {}
    void ResetTemporalState() override         {}
    void RequestRender() override              {}
};

// Lazily created temp dir for scene-relative .lua files (the T7 pattern:
// a silently-empty temp dir would resolve every path against CWD instead
// of failing, so creation is eager and fatal on failure).
const std::filesystem::path& A6TempDir()
{
    static const std::filesystem::path dir = [] {
        auto d = std::filesystem::temp_directory_path() / "rt2_a6_lua_tests";
        std::filesystem::remove_all(d);
        std::filesystem::create_directories(d);
        return d;
    }();
    return dir;
}

std::filesystem::path A6WriteScript(const std::string& name,
                                    const std::string& source)
{
    auto path = A6TempDir() / name;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << source;
    f.close();
    return path;
}

struct A6Harness
{
    DeterministicUuidProvider uuidProv;
    A6NullBridge bridge;
    // Declared before ctrl so a test case that aborts before Stop still
    // tears down the live session (AudioWorld destructor) while the
    // backend is alive — never backend-first (the A5 finding-2 class).
    RecordingFakeAudioBackend fake;
    RuntimeSceneController ctrl;
    AssetResolutionContext assetContext;
    std::vector<AssetDiagnostic> assetDiagnostics;
    ScriptSystem scriptSys;
    RuntimeCommandSink sink;

    A6Harness()
        : scriptSys(uuidProv, assetContext, assetDiagnostics)
        , sink(ctrl)
    {
        ctrl.SetRuntimeUuidProvider(&uuidProv);
        ctrl.SetLifecycleObserver(&scriptSys);
        ctrl.SetScriptDispatch(&scriptSys);
        ctrl.SetInputService(nullptr);
        ctrl.SetRuntimeCommandSink(&sink);
        ctrl.SetAudioBackend(&fake);
        ctrl.SetAudioClipProvider(&fake);
    }

    bool Play(const SceneDocument& doc, Error& err)
    {
        assetContext.assetRoot = doc.metadata.sourcePath.parent_path();
        assetContext.database = nullptr;
        return ctrl.Play(doc, bridge, err);
    }

    bool Play(const SceneDocument& doc)
    {
        Error err;
        const bool ok = Play(doc, err);
        if (!ok)
            printf("[A6] Play refused: %s\n", err.Format().c_str());
        return ok;
    }

    void Update(float dt = 1.0f / 60.0f) { ctrl.Update(dt, bridge); }
    void Stop(const SceneDocument& doc) { ctrl.Stop(doc, bridge); }
};

// Raw SceneDocument builder for Lua-driven audio scenes (the T7 shape:
// explicit Transform/Name/Script/AudioSource emplaces, UUIDs assigned at
// creation, the scene file path roots script resolution at the temp dir).
struct A6DocBuilder
{
    A6Harness& h;
    SceneDocument doc;

    explicit A6DocBuilder(A6Harness& harness)
        : h(harness)
    {
        doc.metadata.sourcePath = A6TempDir() / "a6_fixture.rt2scene";
        doc.SetUuidProvider(&h.uuidProv);
    }

    entt::entity Create(const char* name)
    {
        entt::entity e = doc.ecs.registry.create();
        doc.ecs.registry.emplace<NameComponent>(e, NameComponent{name});
        Transform& tf = doc.ecs.registry.emplace<Transform>(e);
        tf.dirty = true;
        doc.AssignNewUuid(e);
        return e;
    }

    void AttachScript(entt::entity e, const std::string& scriptName)
    {
        ScriptComponent sc;
        sc.asset.kind = AssetKind::Script;
        sc.asset.path = scriptName;
        sc.asset.sourceKey = "lua:asset=" + scriptName;
        doc.ecs.registry.emplace<ScriptComponent>(e, sc);
    }

    // Bound authored source (the A5 fixture shape): valid clip identity,
    // Effects bus, authored-range mix, deterministic default key.
    void AttachBoundSource(entt::entity e, const std::string& clipPath,
                           bool autoplay = false, bool loop = false,
                           bool spatial = false)
    {
        AudioSourceComponent source;
        source.clip.kind = AssetKind::AudioClip;
        source.clip.path = clipPath;
        source.clip.assetId = A6NextUuid();
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
        doc.ecs.registry.emplace<AudioSourceComponent>(e, source);
    }

    void AttachUnboundSource(entt::entity e)
    {
        doc.ecs.registry.emplace<AudioSourceComponent>(e,
            AudioSourceComponent{});
    }

    UUID UuidOf(entt::entity e) const
    {
        return doc.ecs.registry.get<EntityIdComponent>(e).id;
    }
};

std::string A6Key(const std::string& path)
{
    // Mirrors the controller's default clip-key builder exactly.
    return "audioclip:" + path;
}

std::shared_ptr<const DecodedAudioGeneration> A6MonoGeneration()
{
    auto generation = std::make_shared<DecodedAudioGeneration>();
    generation->channels = 1;
    generation->sampleRate = 48000;
    generation->frameCount = 8;
    generation->pcmInterleaved.assign(
        static_cast<size_t>(generation->frameCount), 0.25f);
    return generation;
}

Error A6StartError()
{
    Error error;
    error.code = Error::InvalidRuntimeState;
    error.path = "audio backend voice";
    error.detail = "A6 fixture: injected start failure";
    return error;
}

std::string A6RuntimeName(A6Harness& h, const UUID& uuid)
{
    const SceneDocument* rt = h.ctrl.TryGetRuntimeScene();
    if (!rt)
        return {};
    const auto e = rt->FindByUuid(uuid);
    if (e == entt::null)
        return {};
    const auto* nc = rt->ecs.registry.try_get<NameComponent>(e);
    return nc ? nc->name : std::string{};
}

glm::vec3 A6RuntimePos(A6Harness& h, const UUID& uuid)
{
    const SceneDocument* rt = h.ctrl.TryGetRuntimeScene();
    if (!rt)
        return glm::vec3{0.0f, 0.0f, 0.0f};
    const auto e = rt->FindByUuid(uuid);
    if (e == entt::null)
        return glm::vec3{0.0f, 0.0f, 0.0f};
    const auto* tf = rt->ecs.registry.try_get<Transform>(e);
    return tf ? tf->translation : glm::vec3{0.0f, 0.0f, 0.0f};
}

size_t A6LiveVoices(A6Harness& h, const UUID& source)
{
    AudioWorld* world = h.ctrl.TryGetAudioWorld();
    if (!world)
        return 0;
    return world->LiveVoicesForSource(source).size();
}

// C++ probe dispatch for the destroying-UUID refusal case: runs mid-drain
// while the AudioWorld destroying mark is active, so sink audio calls
// there must refuse loudly without mutation or sequence consumption.
struct A6DestroyProbeDispatch final : public IRuntimeScriptDispatch
{
    RuntimeSceneController* ctrl = nullptr;
    RuntimeCommandSink* sink = nullptr;
    UUID dying;
    bool allRefused = false;

    void OnFixedUpdate(float) override {}
    void OnUpdate(float) override {}
    void SyncScriptEnvironments() override {}
    void OnEntitiesDestroying(const std::vector<UUID>& uuids) override
    {
        bool sawDying = false;
        for (const UUID& id : uuids)
            if (id == dying)
                sawDying = true;
        if (!sawDying || sink == nullptr || ctrl == nullptr)
            return;
        AudioWorld* world = ctrl->TryGetAudioWorld();
        const uint64_t newestBefore =
            world ? world->GetSourceStatus(dying).newestAcceptedSequence : 0;
        const bool play = sink->AudioPlay(dying);
        const bool playAt =
            sink->AudioPlayAt(dying, glm::vec3{1.0f, 0.0f, 0.0f});
        const bool stop = sink->AudioStop(dying);
        const bool pause = sink->AudioPause(dying, true);
        const bool gain = sink->AudioSetGain(dying, 0.5f);
        const bool pitch = sink->AudioSetPitch(dying, 1.0f);
        const uint64_t newestAfter =
            world ? world->GetSourceStatus(dying).newestAcceptedSequence : 0;
        allRefused = !play && !playAt && !stop && !pause && !gain && !pitch &&
            (newestAfter == newestBefore);
    }
};

} // namespace

TEST_CASE("A6_LuaMalformedControls_ReturnFalse_KeepScriptLive")
{
    // Required check 13 (Lua half): nil, wrong-type, NaN, infinity, and
    // overflow arguments return false without quarantining the script or
    // changing audio state; well-formed calls in the same frame still
    // return true and drain FIFO. The obvious single mutation (a binding
    // that throws/raises instead of returning false) quarantines the
    // instance and turns this red.
    A6Harness h;
    A6DocBuilder b(h);
    const entt::entity e = b.Create("Probe");
    b.AttachBoundSource(e, "audio/probe.wav");
    const UUID uuid = b.UuidOf(e);
    h.fake.ScriptGeneration(A6Key("audio/probe.wav"), A6MonoGeneration());

    A6WriteScript("a6_malformed.lua", R"lua(
bad = 0
function on_update(entity, dt, input, world)
    if entity:audio_play_at(nil) then bad = bad + 1 end
    if entity:audio_play_at(42) then bad = bad + 1 end
    if entity:audio_play_at("x") then bad = bad + 1 end
    if entity:audio_play_at({1.0, 2.0}) then bad = bad + 1 end
    if entity:audio_play_at({0/0, 0.0, 0.0}) then bad = bad + 1 end
    if entity:audio_play_at({math.huge, 0.0, 0.0}) then bad = bad + 1 end
    if entity:audio_play_at({1e300, 0.0, 0.0}) then bad = bad + 1 end
    if entity:audio_set_gain(nil) then bad = bad + 1 end
    if entity:audio_set_gain("x") then bad = bad + 1 end
    if entity:audio_set_gain(0/0) then bad = bad + 1 end
    if entity:audio_set_gain(math.huge) then bad = bad + 1 end
    if entity:audio_set_gain(5.0) then bad = bad + 1 end
    if entity:audio_set_gain(-1.0) then bad = bad + 1 end
    if entity:audio_set_pitch(nil) then bad = bad + 1 end
    if entity:audio_set_pitch("x") then bad = bad + 1 end
    if entity:audio_set_pitch(0/0) then bad = bad + 1 end
    if entity:audio_set_pitch(0.1) then bad = bad + 1 end
    if entity:audio_set_pitch(5.0) then bad = bad + 1 end
    if entity:audio_pause(nil) then bad = bad + 1 end
    if entity:audio_pause("yes") then bad = bad + 1 end
    if entity:audio_pause(1) then bad = bad + 1 end
    -- Well-formed controls in the same frame must still accept.
    if not entity:audio_play() then bad = bad + 1 end
    if not entity:audio_play_at({1.0, 2.0, 3.0}) then bad = bad + 1 end
    if not entity:audio_pause(true) then bad = bad + 1 end
    if not entity:audio_set_gain(0.5) then bad = bad + 1 end
    if not entity:audio_set_pitch(1.1) then bad = bad + 1 end
    if bad > 0 then entity:set_position({999.0, 0.0, 0.0}) end
end
)lua");
    b.AttachScript(e, "a6_malformed.lua");

    REQUIRE(h.Play(b.doc));
    h.Update();

    // No quarantine, no sentinel write, both well-formed plays drained.
    // The script pauses its own voices, so the truthful aggregate is
    // Paused with both voices paused — not Playing.
    CHECK(h.scriptSys.LiveInstanceCount() == 1);
    CHECK(h.scriptSys.QuarantinedInstanceCount() == 0);
    const glm::vec3 pos = A6RuntimePos(h, uuid);
    CHECK(pos.x == doctest::Approx(0.0f));
    CHECK(A6LiveVoices(h, uuid) == 2);
    AudioSourceStatus status = h.sink.GetAudioStatus(uuid);
    CHECK(status.aggregate == AudioSourceAggregate::Paused);
    CHECK(status.liveVoiceCount == 2);
    CHECK(status.pausedVoiceCount == 2);
    CHECK(status.newestAcceptedSequence == 5);
    CHECK(status.hasResult);
    CHECK(status.lastResultOk);
    CHECK(status.lastResultSequence == 5);
    h.Stop(b.doc);
}

TEST_CASE("A6_SinkRefusals_MissingSource_EditStop_QueueFull")
{
    // Required check 13 (sink half): unknown UUIDs, component-less
    // entities, unbound Play, Edit/Stop gates, and queue-full all return
    // false without consuming a sequence or mutating audio state. The
    // single mutation (accept-then-fail inside the queue) consumes
    // sequences and turns the newestAcceptedSequence checks red.
    A6Harness h;
    A6DocBuilder b(h);
    const entt::entity bound = b.Create("Bound");
    b.AttachBoundSource(bound, "audio/bound.wav");
    const UUID boundUuid = b.UuidOf(bound);
    const entt::entity plain = b.Create("Plain");
    const UUID plainUuid = b.UuidOf(plain);
    const entt::entity unbound = b.Create("Unbound");
    b.AttachUnboundSource(unbound);
    const UUID unboundUuid = b.UuidOf(unbound);
    h.fake.ScriptGeneration(A6Key("audio/bound.wav"), A6MonoGeneration());

    // Edit gate: no session is committed yet.
    const UUID unknown = A6NextUuid();
    CHECK_FALSE(h.sink.AudioPlay(unknown));
    CHECK(h.sink.GetAudioStatus(unknown).aggregate == AudioSourceAggregate::Idle);

    REQUIRE(h.Play(b.doc));

    // Unknown UUID: all six refuse, no sequence consumed.
    CHECK_FALSE(h.sink.AudioPlay(unknown));
    CHECK_FALSE(h.sink.AudioPlayAt(unknown, glm::vec3{0.0f, 0.0f, 0.0f}));
    CHECK_FALSE(h.sink.AudioStop(unknown));
    CHECK_FALSE(h.sink.AudioPause(unknown, true));
    CHECK_FALSE(h.sink.AudioSetGain(unknown, 0.5f));
    CHECK_FALSE(h.sink.AudioSetPitch(unknown, 1.0f));
    CHECK(h.sink.GetAudioStatus(unknown).newestAcceptedSequence == 0);

    // Component-less entity: all six refuse.
    CHECK_FALSE(h.sink.AudioPlay(plainUuid));
    CHECK_FALSE(h.sink.AudioPlayAt(plainUuid, glm::vec3{0.0f, 0.0f, 0.0f}));
    CHECK_FALSE(h.sink.AudioStop(plainUuid));
    CHECK_FALSE(h.sink.AudioPause(plainUuid, false));
    CHECK_FALSE(h.sink.AudioSetGain(plainUuid, 0.5f));
    CHECK_FALSE(h.sink.AudioSetPitch(plainUuid, 1.0f));

    // Unbound source: Play/PlayAt refuse (no clip authored); Stop stays an
    // accepted idempotent no-op and records its sequence.
    CHECK_FALSE(h.sink.AudioPlay(unboundUuid));
    CHECK_FALSE(h.sink.AudioPlayAt(unboundUuid, glm::vec3{0.0f, 0.0f, 0.0f}));
    CHECK(h.sink.AudioStop(unboundUuid));
    CHECK(h.sink.GetAudioStatus(unboundUuid).newestAcceptedSequence == 1);

    // Bound control still accepts after the refusals above.
    CHECK(h.sink.AudioPlay(boundUuid));
    CHECK(h.sink.GetAudioStatus(boundUuid).newestAcceptedSequence == 1);

    // Queue-full: 256 Stop admissions succeed, the 257th refuses loudly
    // without mutation or sequence consumption. The controller's
    // op-named pre-check absorbs the overflow (the world-level overflow
    // census is A3-covered), so the observable contract is: still 256
    // queued, newest sequence still 256.
    A6Harness h2;
    A6DocBuilder b2(h2);
    const entt::entity e2 = b2.Create("Full");
    b2.AttachBoundSource(e2, "audio/full.wav");
    const UUID fullUuid = b2.UuidOf(e2);
    h2.fake.ScriptGeneration(A6Key("audio/full.wav"), A6MonoGeneration());
    REQUIRE(h2.Play(b2.doc));
    for (uint32_t i = 0; i < kAudioCommandQueueCapacity; ++i)
        CHECK(h2.sink.AudioStop(fullUuid));
    CHECK(h2.ctrl.AudioQueuedCommandCount() == kAudioCommandQueueCapacity);
    CHECK_FALSE(h2.sink.AudioStop(fullUuid));
    CHECK(h2.ctrl.AudioQueuedCommandCount() == kAudioCommandQueueCapacity);
    CHECK(h2.sink.GetAudioStatus(fullUuid).newestAcceptedSequence ==
          kAudioCommandQueueCapacity);
    h2.Update();
    CHECK(h2.ctrl.AudioQueuedCommandCount() == 0);
    CHECK(A6LiveVoices(h2, fullUuid) == 0);
    h2.Stop(b2.doc);

    // Stop gate: post-Stop calls refuse and read Idle.
    h.Stop(b.doc);
    CHECK_FALSE(h.sink.AudioPlay(boundUuid));
    CHECK_FALSE(h.sink.AudioStop(boundUuid));
    CHECK(h.sink.GetAudioStatus(boundUuid).aggregate == AudioSourceAggregate::Idle);
}

TEST_CASE("A6_SameFrameAdmission_FIFOOrder")
{
    // Required check 12/14 (admission half): commands queued before the
    // post-transform audio slot execute in that same presentation frame,
    // in FIFO order — Stop-then-Play leaves one voice, Play-then-Stop
    // leaves none. Reversing drain order turns this red.
    A6Harness h;
    A6DocBuilder b(h);
    const entt::entity a = b.Create("A");
    b.AttachBoundSource(a, "audio/a.wav");
    const UUID aUuid = b.UuidOf(a);
    const entt::entity c = b.Create("C");
    b.AttachBoundSource(c, "audio/c.wav");
    const UUID cUuid = b.UuidOf(c);
    h.fake.ScriptGeneration(A6Key("audio/a.wav"), A6MonoGeneration());
    h.fake.ScriptGeneration(A6Key("audio/c.wav"), A6MonoGeneration());
    REQUIRE(h.Play(b.doc));

    // Source A: Play then Stop. Source C: Stop then Play.
    CHECK(h.sink.AudioPlay(aUuid));
    CHECK(h.sink.AudioStop(aUuid));
    CHECK(h.sink.AudioStop(cUuid));
    CHECK(h.sink.AudioPlay(cUuid));
    CHECK(h.sink.GetAudioStatus(aUuid).aggregate == AudioSourceAggregate::Queued);
    h.Update();
    CHECK(A6LiveVoices(h, aUuid) == 0);
    CHECK(A6LiveVoices(h, cUuid) == 1);
    CHECK(h.fake.starts.size() == 2);
    h.Stop(b.doc);
}

TEST_CASE("A6_ReentrantSubmission_WaitsNextFrame")
{
    // Frozen-drain FIFO timing: a command submitted re-entrantly from
    // inside the backend start callback lands in the next frame's queue —
    // the just-started voice stays live through frame return and dies on
    // the following frame. Draining re-entrant work inline turns this red.
    A6Harness h;
    A6DocBuilder b(h);
    const entt::entity e = b.Create("Reentrant");
    b.AttachBoundSource(e, "audio/re.wav");
    const UUID uuid = b.UuidOf(e);
    h.fake.ScriptGeneration(A6Key("audio/re.wav"), A6MonoGeneration());
    REQUIRE(h.Play(b.doc));

    h.fake.onStartVoice = [&](BackendVoiceToken) {
        CHECK(h.sink.AudioStop(uuid));
    };
    CHECK(h.sink.AudioPlay(uuid));
    h.Update();
    // The re-entrant Stop waited: voice live, one command queued.
    CHECK(A6LiveVoices(h, uuid) == 1);
    CHECK(h.ctrl.AudioQueuedCommandCount() == 1);
    h.fake.onStartVoice = nullptr;
    h.Update();
    CHECK(A6LiveVoices(h, uuid) == 0);
    CHECK(h.ctrl.AudioQueuedCommandCount() == 0);
    h.Stop(b.doc);
}

TEST_CASE("A6_BackendFailureAfterTrue_StatusFailed")
{
    // Required check 13 (async half): `true` is admission only — an
    // injected backend start failure after a `true` return surfaces as a
    // typed sequence-scoped Failed result without changing the historical
    // return. Reporting audible success on backend failure turns this red.
    A6Harness h;
    A6DocBuilder b(h);
    const entt::entity e = b.Create("Failing");
    b.AttachBoundSource(e, "audio/fail.wav");
    const UUID uuid = b.UuidOf(e);
    h.fake.ScriptGeneration(A6Key("audio/fail.wav"), A6MonoGeneration());
    REQUIRE(h.Play(b.doc));

    const bool admitted = h.sink.AudioPlay(uuid);
    h.fake.FailNextStart(A6StartError());
    h.Update();

    CHECK(admitted);
    CHECK(A6LiveVoices(h, uuid) == 0);
    const AudioSourceStatus status = h.sink.GetAudioStatus(uuid);
    CHECK(status.aggregate == AudioSourceAggregate::Failed);
    CHECK(status.newestAcceptedSequence == 1);
    CHECK_FALSE(status.newestQueued);
    CHECK(status.hasResult);
    CHECK_FALSE(status.lastResultOk);
    CHECK(status.lastResultSequence == 1);
    CHECK(status.lastError.detail == "A6 fixture: injected start failure");
    h.Stop(b.doc);
}

TEST_CASE("A6_OverlapAB_OlderOutcomeCannotReplaceNewer")
{
    // Required overlap checks: B's failure while A remains audible keeps
    // aggregate Playing with B's failure visible in lastResult; A's later
    // completion cannot replace B's newer sequence-scoped result; and once
    // every voice is gone the newest terminal outcome (B's failure) still
    // governs over B's own older successful completion. A last-write-wins
    // status turns every CHECK below red.
    A6Harness h;
    A6DocBuilder b(h);
    const entt::entity e = b.Create("Overlap");
    b.AttachBoundSource(e, "audio/overlap.wav");
    const UUID uuid = b.UuidOf(e);
    h.fake.ScriptGeneration(A6Key("audio/overlap.wav"), A6MonoGeneration());
    REQUIRE(h.Play(b.doc));

    CHECK(h.sink.AudioPlay(uuid)); // seq 1 -> voice A
    h.Update();
    REQUIRE(A6LiveVoices(h, uuid) == 1);
    CHECK(h.sink.AudioPlay(uuid)); // seq 2 -> voice B overlaps A
    h.Update();
    REQUIRE(A6LiveVoices(h, uuid) == 2);
    AudioSourceStatus bothLive = h.sink.GetAudioStatus(uuid);
    CHECK(bothLive.aggregate == AudioSourceAggregate::Playing);
    CHECK(bothLive.lastResultSequence == 2);
    CHECK(bothLive.lastResultOk);

    // B's start fails while A remains audible: Playing + B's typed failure.
    h.fake.FailNextStart(A6StartError());
    CHECK(h.sink.AudioPlay(uuid)); // seq 3 fails at the backend
    h.Update();
    CHECK(A6LiveVoices(h, uuid) == 2);
    AudioSourceStatus failedB = h.sink.GetAudioStatus(uuid);
    CHECK(failedB.aggregate == AudioSourceAggregate::Playing);
    CHECK(failedB.newestAcceptedSequence == 3);
    CHECK(failedB.hasResult);
    CHECK_FALSE(failedB.lastResultOk);
    CHECK(failedB.lastResultSequence == 3);
    CHECK(failedB.lastError.detail == "A6 fixture: injected start failure");

    // A's natural completion cannot replace B's newer failed result.
    BackendVoiceToken tokenA = h.fake.starts[0].token;
    h.fake.CompleteToken(tokenA, BackendCompletionReason::Completed);
    h.Update();
    CHECK(A6LiveVoices(h, uuid) == 1);
    AudioSourceStatus afterA = h.sink.GetAudioStatus(uuid);
    CHECK(afterA.aggregate == AudioSourceAggregate::Playing);
    CHECK(afterA.lastResultSequence == 3);
    CHECK_FALSE(afterA.lastResultOk);

    // B completes too: no live/queued voice remains, and the newest
    // terminal outcome is still seq-3's failure — not B's own completion.
    BackendVoiceToken tokenB = h.fake.starts[1].token;
    h.fake.CompleteToken(tokenB, BackendCompletionReason::Completed);
    h.Update();
    CHECK(A6LiveVoices(h, uuid) == 0);
    AudioSourceStatus settled = h.sink.GetAudioStatus(uuid);
    CHECK(settled.aggregate == AudioSourceAggregate::Failed);
    CHECK(settled.lastResultSequence == 3);
    CHECK_FALSE(settled.lastResultOk);
    h.Stop(b.doc);
}

TEST_CASE("A6_ReloadClearsQueue_QuarantineClearsQueue")
{
    // Every queue-clear site beside Stop: a successful reload replacement
    // and a quarantine both drop queued-but-unapplied audio commands, so
    // no stale command outlives its issuing environment. Clearing only
    // physics (the pre-A6 behavior) leaves voices starting post-reload
    // and turns this red.
    A6Harness h;
    A6DocBuilder b(h);
    const entt::entity e = b.Create("Reloadable");
    b.AttachBoundSource(e, "audio/reload.wav");
    const UUID uuid = b.UuidOf(e);
    h.fake.ScriptGeneration(A6Key("audio/reload.wav"), A6MonoGeneration());
    const std::filesystem::path scriptPath =
        A6WriteScript("a6_reloadable.lua", "function on_update(e, dt, i, w) end\n");
    b.AttachScript(e, "a6_reloadable.lua");
    REQUIRE(h.Play(b.doc));
    REQUIRE(h.scriptSys.LiveInstanceCount() == 1);

    // Reload replacement clears.
    CHECK(h.sink.AudioPlay(uuid));
    REQUIRE(h.ctrl.AudioQueuedCommandCount() == 1);
    h.scriptSys.ReloadScript(scriptPath);
    CHECK(h.ctrl.AudioQueuedCommandCount() == 0);
    CHECK(h.scriptSys.LiveInstanceCount() == 1);
    h.Update();
    CHECK(A6LiveVoices(h, uuid) == 0);

    // Quarantine clears: queue, then fail on_update in the same Update
    // (the error path runs before the audio slot drains).
    const entt::entity q = b.Create("Quarantined");
    (void)q;
    h.Stop(b.doc);

    A6Harness h2;
    A6DocBuilder b2(h2);
    const entt::entity e2 = b2.Create("Failing2");
    b2.AttachBoundSource(e2, "audio/q.wav");
    const UUID uuid2 = b2.UuidOf(e2);
    h2.fake.ScriptGeneration(A6Key("audio/q.wav"), A6MonoGeneration());
    A6WriteScript("a6_boom.lua", "function on_update(e, dt, i, w) error('boom') end\n");
    b2.AttachScript(e2, "a6_boom.lua");
    REQUIRE(h2.Play(b2.doc));
    CHECK(h2.sink.AudioPlay(uuid2));
    REQUIRE(h2.ctrl.AudioQueuedCommandCount() == 1);
    h2.Update();
    CHECK(h2.scriptSys.QuarantinedInstanceCount() == 1);
    CHECK(h2.ctrl.AudioQueuedCommandCount() == 0);
    CHECK(A6LiveVoices(h2, uuid2) == 0);
    h2.Stop(b2.doc);
}

TEST_CASE("A6_StopIsolation_NoCrossSessionLeak")
{
    // Stop returns every census to baseline and the next Play starts from
    // Idle sequences — a re-Play inherits neither queued commands nor
    // source state. Leaking the world across sessions turns this red.
    A6Harness h;
    A6DocBuilder b(h);
    const entt::entity e = b.Create("Session");
    b.AttachBoundSource(e, "audio/session.wav");
    const UUID uuid = b.UuidOf(e);
    h.fake.ScriptGeneration(A6Key("audio/session.wav"), A6MonoGeneration());
    REQUIRE(h.Play(b.doc));
    CHECK(h.sink.AudioPlay(uuid));
    h.Update();
    REQUIRE(A6LiveVoices(h, uuid) == 1);
    h.Stop(b.doc);
    CHECK(h.ctrl.TryGetAudioWorld() == nullptr);
    CHECK(h.ctrl.AudioLiveVoiceCount() == 0);
    CHECK(h.ctrl.AudioQueuedCommandCount() == 0);
    CHECK(h.fake.LiveTokenCount() == 0);

    REQUIRE(h.Play(b.doc));
    const AudioSourceStatus fresh = h.sink.GetAudioStatus(uuid);
    CHECK(fresh.aggregate == AudioSourceAggregate::Idle);
    CHECK(fresh.newestAcceptedSequence == 0);
    CHECK_FALSE(fresh.hasResult);
    CHECK(h.sink.AudioPlay(uuid));
    h.Update();
    CHECK(A6LiveVoices(h, uuid) == 1);
    h.Stop(b.doc);
}

TEST_CASE("A6_LuaPlayStatusChain_QueuedPlayingIdle")
{
    // Truthful asynchrony end to end: `true` from audio_play reads back
    // as Queued (not Playing) in the same callback, Playing once the
    // presentation slot drains, and Idle after audio_stop drains. A
    // synchronous-execution implementation reads Playing on frame 1 and
    // turns this red.
    A6Harness h;
    A6DocBuilder b(h);
    const entt::entity e = b.Create("Chained");
    b.AttachBoundSource(e, "audio/chain.wav");
    const UUID uuid = b.UuidOf(e);
    h.fake.ScriptGeneration(A6Key("audio/chain.wav"), A6MonoGeneration());
    A6WriteScript("a6_chain.lua", R"lua(
n = 0
function on_update(entity, dt, input, world)
    n = n + 1
    if n == 1 then entity:audio_play() end
    if n == 2 then entity:audio_stop() end
    local s = entity:audio_status()
    entity:set_name("f" .. n .. ":" .. s.aggregate .. ":" .. s.live_voice_count)
end
)lua");
    b.AttachScript(e, "a6_chain.lua");
    REQUIRE(h.Play(b.doc));

    h.Update();
    CHECK(A6RuntimeName(h, uuid) == "f1:queued:0");
    h.Update();
    CHECK(A6RuntimeName(h, uuid) == "f2:queued:1");
    h.Update();
    CHECK(A6RuntimeName(h, uuid) == "f3:idle:0");
    h.Stop(b.doc);
}

TEST_CASE("A6_DestroyingTargetRefuses_SurvivorDrainsSameFrame")
{
    // Required check 14 (audio half): with a script dispatch installed,
    // on_destroy-time audio commands targeting the dying UUID refuse
    // without consuming a sequence, while an unrelated pre-drain command
    // executes at the same frame's audio slot. Refusing the survivor or
    // admitting the dying target turns this red.
    A6Harness h;
    A6DocBuilder b(h);
    const entt::entity dyingEnt = b.Create("Dying");
    b.AttachBoundSource(dyingEnt, "audio/dying.wav");
    const UUID dyingUuid = b.UuidOf(dyingEnt);
    const entt::entity survivorEnt = b.Create("Survivor");
    b.AttachBoundSource(survivorEnt, "audio/survivor.wav");
    const UUID survivorUuid = b.UuidOf(survivorEnt);
    h.fake.ScriptGeneration(A6Key("audio/dying.wav"), A6MonoGeneration());
    h.fake.ScriptGeneration(A6Key("audio/survivor.wav"), A6MonoGeneration());

    A6DestroyProbeDispatch probe;
    probe.ctrl = &h.ctrl;
    probe.sink = &h.sink;
    probe.dying = dyingUuid;
    h.ctrl.SetScriptDispatch(&probe);

    REQUIRE(h.Play(b.doc));
    // Pre-drain command for the survivor, then destroy the other source.
    CHECK(h.sink.AudioPlay(survivorUuid));
    CHECK(h.ctrl.QueueDestroyRuntimeEntity(dyingUuid).IsOk());
    h.Update();

    CHECK(probe.allRefused);
    CHECK(A6LiveVoices(h, survivorUuid) == 1);
    CHECK(A6LiveVoices(h, dyingUuid) == 0);
    CHECK(h.sink.GetAudioStatus(dyingUuid).aggregate == AudioSourceAggregate::Idle);
    h.Stop(b.doc);
}

TEST_CASE("A6_GainPitchThenPlay_InitialMixAndRetrigger")
{
    // Review F1: queued SetGain/SetPitch merge into the following Play's
    // start mix in FIFO order (and into every later retrigger), instead
    // of the Play restoring authored scalars. Overwriting state.component
    // from the authored command yields unity gain/pitch here and turns
    // every CHECK red.
    A6Harness h;
    A6DocBuilder b(h);
    const entt::entity e = b.Create("Scalar");
    b.AttachBoundSource(e, "audio/scalar.wav");
    const UUID uuid = b.UuidOf(e);
    h.fake.ScriptGeneration(A6Key("audio/scalar.wav"), A6MonoGeneration());
    REQUIRE(h.Play(b.doc));

    CHECK(h.sink.AudioSetGain(uuid, 0.2f));
    CHECK(h.sink.AudioSetPitch(uuid, 1.5f));
    CHECK(h.sink.AudioPlay(uuid));
    h.Update();
    REQUIRE(h.fake.starts.size() == 1);
    // Non-spatial center: gain * cos(pi/4) per channel, explicit pitch.
    CHECK(h.fake.starts[0].start.initialLeft ==
          doctest::Approx(0.2f * 0.70710678f));
    CHECK(h.fake.starts[0].start.initialRight ==
          doctest::Approx(0.2f * 0.70710678f));
    CHECK(h.fake.starts[0].start.pitch == doctest::Approx(1.5f));

    // A later retrigger keeps the retained scalars, not authored unity.
    CHECK(h.sink.AudioPlay(uuid));
    h.Update();
    REQUIRE(h.fake.starts.size() == 2);
    CHECK(h.fake.starts[1].start.initialLeft ==
          doctest::Approx(0.2f * 0.70710678f));
    CHECK(h.fake.starts[1].start.pitch == doctest::Approx(1.5f));

    // A fresh SetGain overrides again; bus/loop stay authored.
    CHECK(h.sink.AudioSetGain(uuid, 0.4f));
    CHECK(h.sink.AudioPlay(uuid));
    h.Update();
    REQUIRE(h.fake.starts.size() == 3);
    CHECK(h.fake.starts[2].start.initialLeft ==
          doctest::Approx(0.4f * 0.70710678f));
    CHECK(h.fake.starts[2].start.pitch == doctest::Approx(1.5f));
    CHECK(h.fake.starts[2].start.loop == false);
    h.Stop(b.doc);
}

TEST_CASE("A6_PlayAtSpatialOverride_SurvivesRefresh_OverlapDistinct")
{
    // Review F2: a PlayAt override rides its voice through final-pose
    // landing and per-voice mix refresh (the entity sits at the origin,
    // which would read center if the override were lost), overlapping
    // one-shots keep distinct positions, and PlayAt on a looping source
    // refuses instead of pinning or fighting the loop.
    A6Harness h;
    A6DocBuilder b(h);
    const entt::entity e = b.Create("Panned");
    b.AttachBoundSource(e, "audio/pan.wav", false, false, true);
    const UUID uuid = b.UuidOf(e);
    h.fake.ScriptGeneration(A6Key("audio/pan.wav"), A6MonoGeneration());
    AudioListenerPose listener;
    listener.position[0] = 0.0f;
    listener.position[1] = 0.0f;
    listener.position[2] = 0.0f;
    listener.forward[0] = 0.0f;
    listener.forward[1] = 0.0f;
    listener.forward[2] = -1.0f;
    listener.up[0] = 0.0f;
    listener.up[1] = 1.0f;
    listener.up[2] = 1.0f;
    h.ctrl.SetAudioListenerPose(listener);
    REQUIRE(h.Play(b.doc));

    // Opposite-side overrides: hard-panned by equal-power law at
    // distance 10 (min 1, max 30, rolloff 1 -> gain 20/29).
    CHECK(h.sink.AudioPlayAt(uuid, glm::vec3{10.0f, 0.0f, 0.0f}));
    CHECK(h.sink.AudioPlayAt(uuid, glm::vec3{-10.0f, 0.0f, 0.0f}));
    h.Update();
    REQUIRE(A6LiveVoices(h, uuid) == 2);
    AudioWorld* world = h.ctrl.TryGetAudioWorld();
    REQUIRE(world != nullptr);
    bool sawRight = false;
    bool sawLeft = false;
    for (const AudioWorldVoiceHandle& voice : world->LiveVoicesForSource(uuid))
    {
        uint64_t seq = 0;
        REQUIRE(world->GetVoicePlaySequence(voice, seq));
        BackendVoiceMix mix;
        REQUIRE(world->GetVoiceMix(voice, mix));
        if (seq == 1)
        {
            CHECK(std::fabs(mix.left) < 1e-5f);
            CHECK(mix.right == doctest::Approx(20.0f / 29.0f).epsilon(1e-5));
            sawRight = true;
        }
        else if (seq == 2)
        {
            CHECK(mix.left == doctest::Approx(20.0f / 29.0f).epsilon(1e-5));
            CHECK(std::fabs(mix.right) < 1e-5f);
            sawLeft = true;
        }
    }
    CHECK(sawRight);
    CHECK(sawLeft);

    // A second frame refreshes mixes from final poses: the entity is at
    // the origin (center 0.7071), so unchanged hard-panned mixes prove
    // the per-voice override survived the refresh.
    h.Update();
    REQUIRE(A6LiveVoices(h, uuid) == 2);
    for (const AudioWorldVoiceHandle& voice : world->LiveVoicesForSource(uuid))
    {
        uint64_t seq = 0;
        REQUIRE(world->GetVoicePlaySequence(voice, seq));
        BackendVoiceMix mix;
        REQUIRE(world->GetVoiceMix(voice, mix));
        if (seq == 1)
        {
            CHECK(std::fabs(mix.left) < 1e-5f);
            CHECK(mix.right == doctest::Approx(20.0f / 29.0f).epsilon(1e-5));
        }
        else if (seq == 2)
        {
            CHECK(mix.left == doctest::Approx(20.0f / 29.0f).epsilon(1e-5));
            CHECK(std::fabs(mix.right) < 1e-5f);
        }
    }
    h.Stop(b.doc);
}

TEST_CASE("A6_ClearedStopDoesNotSuppressAutoplay_Quarantine")
{
    // Review F3 (quarantine half): an autoplay source whose on_create
    // queues Stop and then errors is quarantined before the first audio
    // slot; the dropped Stop must not suppress authored autoplay.
    // Queue-time suppression leaves zero voices and turns this red.
    A6Harness h;
    A6DocBuilder b(h);
    const entt::entity e = b.Create("AutoplayQ");
    b.AttachBoundSource(e, "audio/autoq.wav", true);
    const UUID uuid = b.UuidOf(e);
    h.fake.ScriptGeneration(A6Key("audio/autoq.wav"), A6MonoGeneration());
    A6WriteScript("a6_autoplay_quarantine.lua", R"lua(
function on_create(entity, world)
    entity:audio_stop()
    error("boom-create")
end
function on_update(entity, dt, input, world) end
)lua");
    b.AttachScript(e, "a6_autoplay_quarantine.lua");
    REQUIRE(h.Play(b.doc));
    CHECK(h.scriptSys.QuarantinedInstanceCount() == 1);
    h.Update();
    CHECK(A6LiveVoices(h, uuid) == 1);
    h.Stop(b.doc);
}

TEST_CASE("A6_ClearedStopDoesNotSuppressAutoplay_Reload")
{
    // Review F3 (reload half): an autoplay source whose on_create queues
    // Stop, cleared by a successful reload before the first audio slot,
    // still autoplays. Queue-time suppression leaves zero voices here.
    A6Harness h;
    A6DocBuilder b(h);
    const entt::entity e = b.Create("AutoplayR");
    b.AttachBoundSource(e, "audio/autor.wav", true);
    const UUID uuid = b.UuidOf(e);
    h.fake.ScriptGeneration(A6Key("audio/autor.wav"), A6MonoGeneration());
    const std::filesystem::path scriptPath =
        A6WriteScript("a6_autoplay_reload.lua", R"lua(
function on_create(entity, world)
    entity:audio_stop()
end
function on_update(entity, dt, input, world) end
)lua");
    b.AttachScript(e, "a6_autoplay_reload.lua");
    REQUIRE(h.Play(b.doc));
    REQUIRE(h.ctrl.AudioQueuedCommandCount() == 1);
    h.scriptSys.ReloadScript(scriptPath);
    CHECK(h.ctrl.AudioQueuedCommandCount() == 0);
    h.Update();
    CHECK(A6LiveVoices(h, uuid) == 1);
    h.Stop(b.doc);
}

TEST_CASE("A6_KeyBuilderFailureAfterPlay_StopStillReachesVoice")
{
    // Review F4: only Play/PlayAt resolve the clip key. A builder that
    // succeeds through Play and then fails (clip removed or asset record
    // unresolved mid-session) refuses later Plays but must not trap live
    // voices beyond Stop's reach. Building keys for Stop would refuse it
    // and leave the voice live.
    A6Harness h;
    A6DocBuilder b(h);
    const entt::entity e = b.Create("Keyed");
    b.AttachBoundSource(e, "audio/key.wav");
    const UUID uuid = b.UuidOf(e);
    h.fake.ScriptGeneration(A6Key("audio/key.wav"), A6MonoGeneration());
    int keyBuilds = 0;
    h.ctrl.SetAudioClipKeyBuilder(
        [&](const AssetReference& clip, const UUID&, const std::string&)
            -> Result<std::string> {
            ++keyBuilds;
            if (keyBuilds <= 2)
                return Result<std::string>::Ok(A6Key(clip.path));
            return Result<std::string>::Fail(Error::InvalidArgument,
                "audio clip",
                "A6 fixture: clip record unresolved after Play");
        });
    REQUIRE(h.Play(b.doc));
    CHECK(keyBuilds == 1);

    CHECK(h.sink.AudioPlay(uuid)); // second build: still healthy
    h.Update();
    REQUIRE(A6LiveVoices(h, uuid) == 1);

    CHECK_FALSE(h.sink.AudioPlay(uuid)); // third build fails: refused
    CHECK(h.sink.GetAudioStatus(uuid).newestAcceptedSequence == 1);
    CHECK(keyBuilds == 3);

    CHECK(h.sink.AudioStop(uuid)); // no key needed: accepted
    CHECK(keyBuilds == 3);
    h.Update();
    CHECK(A6LiveVoices(h, uuid) == 0);
    h.Stop(b.doc);
}

TEST_CASE("A6_PlayAtOnLoopSource_IndependentOneShot")
{
    // Re-review finding: PlayAt on a looping source starts an independent
    // non-looping one-shot at the override — it never refreshes the live
    // loop, never drags it to the override, and preserves the authored
    // loop flag so a later plain Play still refreshes instead of
    // duplicating. Refusing the PlayAt (or absorbing it into the loop)
    // turns this red.
    A6Harness h;
    A6DocBuilder b(h);
    const entt::entity e = b.Create("LoopWithOneShot");
    b.AttachBoundSource(e, "audio/loopshot.wav", false, true, true);
    const UUID uuid = b.UuidOf(e);
    h.fake.ScriptGeneration(A6Key("audio/loopshot.wav"), A6MonoGeneration());
    AudioListenerPose listener;
    listener.position[0] = 0.0f;
    listener.position[1] = 0.0f;
    listener.position[2] = 0.0f;
    listener.forward[0] = 0.0f;
    listener.forward[1] = 0.0f;
    listener.forward[2] = -1.0f;
    listener.up[0] = 0.0f;
    listener.up[1] = 1.0f;
    listener.up[2] = 1.0f;
    h.ctrl.SetAudioListenerPose(listener);
    REQUIRE(h.Play(b.doc));

    // The authored loop starts through plain Play.
    CHECK(h.sink.AudioPlay(uuid)); // seq 1: loop voice
    h.Update();
    REQUIRE(A6LiveVoices(h, uuid) == 1);
    REQUIRE(h.fake.starts.size() == 1);
    CHECK(h.fake.starts[0].start.loop == true);

    // PlayAt fires an independent one-shot at the override, not a loop
    // refresh: two voices, and the backend started a non-looping voice.
    CHECK(h.sink.AudioPlayAt(uuid, glm::vec3{10.0f, 0.0f, 0.0f})); // seq 2
    h.Update();
    REQUIRE(A6LiveVoices(h, uuid) == 2);
    REQUIRE(h.fake.starts.size() == 2);
    CHECK(h.fake.starts[1].start.loop == false);

    // The one-shot (seq 2) is hard-right at the override; the loop
    // (seq 1) still spatializes from the entity at the origin (center),
    // proving the loop was neither refreshed to nor dragged along.
    AudioWorld* world = h.ctrl.TryGetAudioWorld();
    REQUIRE(world != nullptr);
    bool sawLoop = false;
    bool sawOneShot = false;
    for (const AudioWorldVoiceHandle& voice : world->LiveVoicesForSource(uuid))
    {
        uint64_t seq = 0;
        REQUIRE(world->GetVoicePlaySequence(voice, seq));
        BackendVoiceMix mix;
        REQUIRE(world->GetVoiceMix(voice, mix));
        if (seq == 1)
        {
            CHECK(mix.left == doctest::Approx(0.70710678f).epsilon(1e-5));
            CHECK(mix.right == doctest::Approx(0.70710678f).epsilon(1e-5));
            sawLoop = true;
        }
        else if (seq == 2)
        {
            CHECK(std::fabs(mix.left) < 1e-5f);
            CHECK(mix.right == doctest::Approx(20.0f / 29.0f).epsilon(1e-5));
            sawOneShot = true;
        }
    }
    CHECK(sawLoop);
    CHECK(sawOneShot);

    // A later plain Play still refreshes the loop (no third voice, no new
    // backend start): the authored loop flag survived the override.
    CHECK(h.sink.AudioPlay(uuid)); // seq 3: loop refresh
    h.Update();
    CHECK(A6LiveVoices(h, uuid) == 2);
    CHECK(h.fake.starts.size() == 2);
    h.Stop(b.doc);
}
