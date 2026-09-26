#include <doctest/doctest.h>

// ============================================================================
// A8 - Audio acceptance scene and durable workflow (grounded at d01c763).
//
// Drives the checked-in acceptance scene
// (RT2App/assets/audio-acceptance.rt2scene, schema v9) through the real
// RuntimeSceneController with the recording fake behind both seams:
//
//   - file round-trip: Load the shipped bytes, assert the authored loop /
//     music payloads field-for-field, Save to temp, reload, exact equality;
//   - Play walk: autoplay spatial loop starts, follows the final world
//     transform across the listener, host-driven Music play/stop, destroy
//     cleanup of exactly the emitter's voices, Stop census reset;
//   - 20x Play/Stop census: session/voice/backend counts return to baseline
//     every cycle;
//   - corrupt music refusal: the real file with a poisoned provider entry
//     refuses Play atomically (check 4 shape on shipped content);
//   - preview: AudioPreviewController replaces loop with music and stops to
//     zero on the shipped components.
//
// Lua admission itself is A6's proven surface (AudioA6* + script gate); the
// host-level QueueAudioPlay/Stop seam used here is the same FIFO the Lua
// commands drain into. Production decode/device pacing is A4's probe
// (exact PCM oracles); the fake serves scripted mono/stereo generations so
// this file proves authored-to-policy wiring, not decoder bytes.
//
// Out of scope (explicit): pinball sound content/rules, streaming Music,
// HRTF/occlusion/reverb/Doppler, multi-device output.
//
// CPU boundary: portable engine headers + standard library only. The hard
// #error guard fails the build if miniaudio.h ever becomes reachable from
// RT2Tests (required check 17).
// ============================================================================

#if __has_include("miniaudio.h")
#error "A8 boundary: RT2Tests must not import miniaudio (check 17; the adapter stays in RT2AudioBackend)"
#endif

#include "RuntimeSceneController.h"
#include "ISceneRenderBridge.h"
#include "SceneDocument.h"
#include "SceneGraph.h"
#include "ECSComponents.h"
#include "ECSScene.h"
#include "GPUSceneData.h"
#include "AudioBackend.h"
#include "AudioClipProvider.h"
#include "AudioClipAssetProvider.h"
#include "AudioComponents.h"
#include "AudioWorld.h"
#include "FakeAudioBackend.h"
#include "AudioPreviewController.h"
#include "SceneSerializer.h"
#include "core/UUID.h"
#include "core/Error.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace rt2::core;
using namespace rt2::audio;

namespace
{

constexpr float kA8Dt = 1.0f / 60.0f;
constexpr float kA8Center = 0.70710678f; // cos(pi/4): equal-power center

const char* kA8ScenePath = "RT2App/assets/audio-acceptance.rt2scene";
const char* kA8LoopClip = "audio/acceptance_loop_mono_s16.wav";
const char* kA8MusicClip = "audio/acceptance_music_stereo_s16.wav";

UUID A8LoopUuid() { return UUID::Parse("9c50b5ff-11e2-49a5-bb9c-67809be6f579"); }
UUID A8MusicUuid() { return UUID::Parse("bcfa826e-939b-45ed-9b77-e75729800972"); }
UUID A8LoopClipId() { return UUID::Parse("b3e7c60f-9925-43ba-b750-f35673d25026"); }
UUID A8MusicClipId() { return UUID::Parse("1d7d49f3-3346-46b6-a9bb-02d0c9ee8e38"); }

std::string A8Key(const std::string& path)
{
    // Mirrors the controller's default clip-key builder exactly. Scripting
    // the wrong key observes zero fetches and fails at Play, which is
    // itself the key-derivation proof (same rationale as A5).
    return "audioclip:" + path;
}

std::shared_ptr<const DecodedAudioGeneration> A8MonoGeneration()
{
    auto generation = std::make_shared<DecodedAudioGeneration>();
    generation->channels = 1;
    generation->sampleRate = 48000;
    generation->frameCount = 8;
    generation->pcmInterleaved.assign(
        static_cast<size_t>(generation->frameCount), 0.25f);
    return generation;
}

std::shared_ptr<const DecodedAudioGeneration> A8StereoGeneration()
{
    auto generation = std::make_shared<DecodedAudioGeneration>();
    generation->channels = 2;
    generation->sampleRate = 48000;
    generation->frameCount = 8;
    generation->pcmInterleaved.assign(
        static_cast<size_t>(generation->frameCount) * 2, 0.25f);
    return generation;
}

AudioListenerPose A8Listener(float x, float y, float z)
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

class A8RecordingBridge final : public ISceneRenderBridge
{
public:
    int renderRequests = 0;

    void FullSync(GPUSceneData&) override {}
    void MaterialSync(GPUSceneData&) override {}
    void TransformSync(GPUSceneData&) override {}
    void ResetTemporalState() override {}
    void RequestRender() override { ++renderRequests; }
};

bool A8LoadAcceptance(SceneDocument& doc, Error& err)
{
    // Must run from the repository root (AGENTS.md): the path is
    // repo-relative, so a missing file fails loudly here rather than
    // silently testing an empty scene.
    return SceneSerializer::Load(doc, std::filesystem::path(kA8ScenePath),
                                 err);
}

const AudioSourceComponent* A8FindSource(const SceneDocument& doc,
                                         const UUID& uuid)
{
    const entt::entity e = doc.uuidIndex.Find(uuid);
    if (e == entt::null)
        return nullptr;
    return doc.ecs.registry.try_get<AudioSourceComponent>(e);
}

std::string A8ReadSidecar(const std::string& clipPath)
{
    std::ifstream in(std::string("RT2App/assets/") + clipPath + ".rt2meta",
                     std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    std::string out = text.str();
    while (!out.empty() &&
           (out.back() == '\n' || out.back() == '\r' || out.back() == ' ' ||
            out.back() == '\t'))
        out.pop_back();
    return out;
}

std::filesystem::path A8TempDir()
{
    static uint64_t counter = 0;
    ++counter;
    std::ostringstream name;
    name << "rt2_a8_acceptance_" << counter;
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / name.str();
    std::filesystem::create_directories(dir);
    return dir;
}

void A8ScriptAcceptanceClips(RecordingFakeAudioBackend& fake)
{
    fake.ScriptGeneration(A8Key(kA8LoopClip), A8MonoGeneration());
    fake.ScriptGeneration(A8Key(kA8MusicClip), A8StereoGeneration());
}

void A8WireAudio(RuntimeSceneController& ctrl,
                 RecordingFakeAudioBackend& fake)
{
    ctrl.SetAudioBackend(&fake);
    ctrl.SetAudioClipProvider(&fake);
}

BackendVoiceMix A8RequireMix(AudioWorld& world, const UUID& source)
{
    std::vector<AudioWorldVoiceHandle> voices =
        world.LiveVoicesForSource(source);
    REQUIRE(voices.size() == 1);
    BackendVoiceMix mix;
    REQUIRE(world.GetVoiceMix(voices[0], mix));
    return mix;
}

} // namespace

TEST_CASE("A8_AcceptanceSceneRoundTrip")
{
    // The shipped scene bytes carry exactly one looping spatial mono
    // Effects source and one idle non-spatial Music source, and survive a
    // Save/Load cycle with exact-value equality. The mutation (drop the
    // audioSource block, flip a bus/spatial flag, or retarget a clip)
    // turns the field checks red.
    SceneDocument doc;
    Error err;
    const bool loaded = A8LoadAcceptance(doc, err);
    INFO("audio-acceptance.rt2scene must load from the repo root: " << err.detail);
    REQUIRE(loaded);
    CHECK(err.IsOk());
    CHECK(doc.metadata.schemaVersion == SceneSerializer::SchemaVersion);

    REQUIRE(doc.uuidIndex.Contains(A8LoopUuid()));
    REQUIRE(doc.uuidIndex.Contains(A8MusicUuid()));

    const AudioSourceComponent* loop = A8FindSource(doc, A8LoopUuid());
    const AudioSourceComponent* music = A8FindSource(doc, A8MusicUuid());
    REQUIRE(loop != nullptr);
    REQUIRE(music != nullptr);

    CHECK(loop->clip.kind == AssetKind::AudioClip);
    CHECK(loop->clip.path == kA8LoopClip);
    CHECK(loop->clip.assetId == A8LoopClipId());
    CHECK(loop->bus == AudioBus::Effects);
    CHECK(loop->autoplay == true);
    CHECK(loop->loop == true);
    CHECK(loop->spatial == true);
    CHECK(loop->gain == doctest::Approx(1.0f));
    CHECK(loop->pitch == doctest::Approx(1.0f));
    CHECK(loop->minDistance == doctest::Approx(1.0f));
    CHECK(loop->maxDistance == doctest::Approx(30.0f));
    CHECK(loop->rolloff == doctest::Approx(1.0f));
    CHECK(loop->priority == 128);

    CHECK(music->clip.kind == AssetKind::AudioClip);
    CHECK(music->clip.path == kA8MusicClip);
    CHECK(music->clip.assetId == A8MusicClipId());
    CHECK(music->bus == AudioBus::Music);
    CHECK(music->autoplay == false);
    CHECK(music->loop == false);
    CHECK(music->spatial == false);
    CHECK(music->gain == doctest::Approx(0.8f));

    // Scene asset IDs match the sidecar-driven project identity: the
    // clip would not resolve in the editor if they drifted apart.
    CHECK(A8ReadSidecar(kA8LoopClip) == A8LoopClipId().ToString());
    CHECK(A8ReadSidecar(kA8MusicClip) == A8MusicClipId().ToString());

    const std::filesystem::path dir = A8TempDir();
    const std::filesystem::path tmp = dir / "audio-acceptance-rt.rt2scene";
    std::vector<AssetDiagnostic> diagnostics;
    Error saveErr;
    // SaveTo against the shipped logical path: asset references stay
    // relative to RT2App/assets (plain Save would rebase them against the
    // temp output directory, which is a different tested behavior).
    const bool saved = SceneSerializer::SaveTo(
        doc, tmp, std::filesystem::path(kA8ScenePath), diagnostics,
        saveErr);
    INFO("acceptance scene must Save: " << saveErr.detail);
    REQUIRE(saved);
    SceneDocument reloaded;
    Error reloadErr;
    const bool reloadedOk =
        SceneSerializer::Load(reloaded, tmp, reloadErr);
    INFO("acceptance scene must reload after Save: " << reloadErr.detail);
    REQUIRE(reloadedOk);
    const AudioSourceComponent* loop2 = A8FindSource(reloaded, A8LoopUuid());
    const AudioSourceComponent* music2 = A8FindSource(reloaded, A8MusicUuid());
    REQUIRE(loop2 != nullptr);
    REQUIRE(music2 != nullptr);
    CHECK(*loop2 == *loop);
    CHECK(*music2 == *music);
    std::filesystem::remove_all(dir);
}

TEST_CASE("A8_AcceptanceScenePlayWalk")
{
    // End-to-end authored-to-audible walk on the shipped scene: autoplay
    // spatial loop starts and tracks the final world transform across the
    // listener, host-driven Music play/stop is center-panned, destroying
    // the emitter stops exactly its voices, and Stop returns every census
    // to baseline.
    SceneDocument doc;
    Error loadErr;
    REQUIRE(A8LoadAcceptance(doc, loadErr));

    RecordingFakeAudioBackend fake;
    A8ScriptAcceptanceClips(fake);
    A8RecordingBridge bridge;
    RuntimeSceneController ctrl;
    A8WireAudio(ctrl, fake);
    ctrl.SetAudioListenerPose(A8Listener(0.0f, 0.0f, 0.0f));

    Error err;
    const bool played = ctrl.Play(doc, bridge, err);
    INFO("acceptance scene must Play: " << err.detail);
    REQUIRE(played);
    ctrl.Update(kA8Dt, bridge);

    // Autoplay loop is live after the first post-transform audio slot.
    REQUIRE(ctrl.TryGetAudioWorld() != nullptr);
    CHECK(ctrl.AudioLiveVoiceCount() == 1);
    CHECK(fake.starts.size() == 1);
    CHECK(fake.starts.front().start.loop == true);
    CHECK(fake.starts.front().start.bus == AudioBus::Effects);

    // Emitter authored near +X (3, 0, 0.5) with the listener at the
    // origin facing -Z: strongly right-dominant equal-power mix at
    // distance gain (1 - (|t|-1)/29) with |t| = sqrt(9.25). The half-unit
    // z offset keeps the left gain comfortably positive: exactly on-axis
    // geometry yields cos(pi/2) = -4.37e-8, which the production backend
    // loudly refuses (gains must be >= 0; proven by probe S21, invisible
    // to the path-scripted fake).
    BackendVoiceMix rightMix =
        A8RequireMix(*ctrl.TryGetAudioWorld(), A8LoopUuid());
    CHECK(rightMix.right > rightMix.left);
    CHECK(rightMix.right == doctest::Approx(0.92955447f).epsilon(0.001));
    CHECK(rightMix.left == doctest::Approx(0.00993377f).epsilon(0.01));
    CHECK(rightMix.left > 0.0f);

    // Final-world-transform tracking (check 11 shape): moving the runtime
    // emitter across the listener flips the mix without re-authoring.
    SceneDocument* runtime = ctrl.TryGetRuntimeSceneMut();
    REQUIRE(runtime != nullptr);
    {
        const entt::entity e = runtime->uuidIndex.Find(A8LoopUuid());
        REQUIRE((e != entt::null));
        Transform* tf = runtime->ecs.registry.try_get<Transform>(e);
        REQUIRE(tf != nullptr);
        tf->translation = glm::vec3(-3.0f, 0.0f, 0.5f);
        SceneGraph::MarkDirty(runtime->ecs.registry, e);
    }
    ctrl.Update(kA8Dt, bridge);
    BackendVoiceMix leftMix =
        A8RequireMix(*ctrl.TryGetAudioWorld(), A8LoopUuid());
    CHECK(leftMix.left > leftMix.right);
    CHECK(leftMix.left == doctest::Approx(0.92955447f).epsilon(0.001));
    CHECK(leftMix.right == doctest::Approx(0.00993377f).epsilon(0.01));

    // Host-driven Music one-shot: non-spatial center mix at gain 0.8.
    REQUIRE(ctrl.QueueAudioPlay(A8MusicUuid()));
    ctrl.Update(kA8Dt, bridge);
    CHECK(ctrl.AudioLiveVoiceCount() == 2);
    BackendVoiceMix musicMix =
        A8RequireMix(*ctrl.TryGetAudioWorld(), A8MusicUuid());
    CHECK(musicMix.left == doctest::Approx(kA8Center * 0.8f).epsilon(0.001));
    CHECK(musicMix.right == doctest::Approx(kA8Center * 0.8f).epsilon(0.001));
    REQUIRE(ctrl.QueueAudioStop(A8MusicUuid()));
    ctrl.Update(kA8Dt, bridge);
    CHECK(ctrl.TryGetAudioWorld()->LiveVoicesForSource(A8MusicUuid()).empty());

    // Destroying the looping emitter (check 6 shape) stops exactly its
    // voices; the already-stopped music bed stays silent.
    REQUIRE(ctrl.QueueDestroyRuntimeEntity(A8LoopUuid()).IsOk());
    ctrl.Update(kA8Dt, bridge);
    CHECK(ctrl.AudioLiveVoiceCount() == 0);
    CHECK(fake.LiveTokenCount() == 0);
    CHECK_FALSE(ctrl.TryGetRuntimeScene()->uuidIndex.Contains(A8LoopUuid()));

    ctrl.Stop(doc, bridge);
    CHECK(ctrl.GetState() == SceneRunState::Edit);
    CHECK(ctrl.TryGetAudioWorld() == nullptr);
    CHECK(ctrl.AudioLiveVoiceCount() == 0);
    CHECK(fake.LiveTokenCount() == 0);
}

TEST_CASE("A8_AcceptanceSceneTwentyPlayStopCensus")
{
    // Ticket census gate: twenty Play/Stop cycles on the shipped scene
    // return session/voice/backend counts to baseline every cycle. The
    // mutation (skip AudioWorld teardown or session-voice drain on Stop)
    // accumulates live tokens and turns a later cycle red.
    SceneDocument doc;
    Error loadErr;
    REQUIRE(A8LoadAcceptance(doc, loadErr));

    RecordingFakeAudioBackend fake;
    A8ScriptAcceptanceClips(fake);
    A8RecordingBridge bridge;
    RuntimeSceneController ctrl;
    A8WireAudio(ctrl, fake);
    ctrl.SetAudioListenerPose(A8Listener(0.0f, 0.0f, 0.0f));

    for (int cycle = 0; cycle < 20; ++cycle)
    {
        CAPTURE(cycle);
        Error err;
        const bool played = ctrl.Play(doc, bridge, err);
        INFO("cycle Play must succeed: " << err.detail);
        REQUIRE(played);
        ctrl.Update(kA8Dt, bridge);
        ctrl.Update(kA8Dt, bridge);
        REQUIRE(ctrl.AudioLiveVoiceCount() == 1);
        REQUIRE(fake.LiveTokenCount() == 1);
        ctrl.Stop(doc, bridge);
        CHECK(ctrl.GetState() == SceneRunState::Edit);
        CHECK(ctrl.TryGetAudioWorld() == nullptr);
        CHECK(ctrl.AudioLiveVoiceCount() == 0);
        CHECK(fake.LiveTokenCount() == 0);
    }
    CHECK(fake.LiveTokenCount() == 0);
}

TEST_CASE("A8_AcceptanceSceneCorruptMusicRefusesPlay")
{
    // Check 4 shape on shipped content: a corrupt NON-autoplay music clip
    // refuses the whole Play candidate atomically — Edit, zero voices,
    // zero session handles, no bridge traffic. Decoding only autoplay
    // sources would Play clean and turn this red.
    SceneDocument doc;
    Error loadErr;
    REQUIRE(A8LoadAcceptance(doc, loadErr));

    RecordingFakeAudioBackend fake;
    fake.ScriptGeneration(A8Key(kA8LoopClip), A8MonoGeneration());
    Error corrupt;
    corrupt.code = Error::Io;
    corrupt.path = kA8MusicClip;
    corrupt.detail = "A8 fixture: corrupt non-autoplay music content";
    fake.ScriptGenerationError(A8Key(kA8MusicClip), corrupt);

    A8RecordingBridge bridge;
    RuntimeSceneController ctrl;
    A8WireAudio(ctrl, fake);
    ctrl.SetAudioListenerPose(A8Listener(0.0f, 0.0f, 0.0f));

    Error err;
    CHECK_FALSE(ctrl.Play(doc, bridge, err));
    CHECK_FALSE(err.IsOk());
    CHECK(err.detail.find(kA8MusicClip) < err.detail.size());
    CHECK(ctrl.GetState() == SceneRunState::Edit);
    CHECK(ctrl.TryGetRuntimeScene() == nullptr);
    CHECK(ctrl.TryGetAudioWorld() == nullptr);
    CHECK(ctrl.AudioLiveVoiceCount() == 0);
    CHECK(fake.LiveTokenCount() == 0);
    CHECK(fake.starts.empty());
    CHECK(bridge.renderRequests == 0);
}

TEST_CASE("A8_AcceptanceSceneProductionProviderResolvesShippedClips")
{
    // P1 identity seam: the shipped scene's real clip references resolve
    // through the PRODUCTION AudioClipAssetProvider against the real
    // asset directory (no database: healthy path+sidecar case). The
    // probe's S21 consumes these same bytes through the production
    // decoder; this case proves the provider observed the real sidecars,
    // bytes, and fingerprints — a missing file or a foreign sidecar
    // refuses loudly here.
    SceneDocument doc;
    Error loadErr;
    REQUIRE(A8LoadAcceptance(doc, loadErr));
    const AudioSourceComponent* loop = A8FindSource(doc, A8LoopUuid());
    const AudioSourceComponent* music = A8FindSource(doc, A8MusicUuid());
    REQUIRE(loop != nullptr);
    REQUIRE(music != nullptr);

    AudioClipAssetProvider provider;
    AssetResolutionContext ctx;
    ctx.assetRoot = std::filesystem::absolute("RT2App/assets");
    ctx.database = nullptr;
    provider.SetContext(ctx);

    auto resolveShipped = [&](const AudioSourceComponent& source,
                              const UUID& uuid, const std::string& name,
                              const UUID& clipId) {
        auto result = provider.ResolveClip(source.clip, uuid, name);
        INFO("production provider must resolve " << name);
        REQUIRE(result.IsOk());
        CHECK(result.value.effectiveId == clipId);
        REQUIRE(result.value.bytes != nullptr);
        CHECK_FALSE(result.value.bytes->empty());
        CHECK(result.value.fingerprint != 0);
        return result.value;
    };
    const ResolvedAudioClip loopResolved =
        resolveShipped(*loop, A8LoopUuid(), "LoopEmitter", A8LoopClipId());
    const ResolvedAudioClip musicResolved =
        resolveShipped(*music, A8MusicUuid(), "MusicBed", A8MusicClipId());
    CHECK(provider.CacheEntryCount() == 2);
    // Immutable cache identity: re-resolving serves the same byte owner,
    // so a later rewrite cannot mutate bytes already handed out.
    const auto loopAgain = provider.ResolveClip(loop->clip, A8LoopUuid(),
                                                "LoopEmitter");
    REQUIRE(loopAgain.IsOk());
    CHECK(loopAgain.value.bytes.get() == loopResolved.bytes.get());
    CHECK(loopAgain.value.fingerprint == loopResolved.fingerprint);
    CHECK(provider.CacheEntryCount() == 2);

    // Missing file refuses with MissingAsset.
    AudioSourceComponent missingSource = *loop;
    missingSource.clip.path = "audio/no-such-clip.wav";
    const auto missing = provider.ResolveClip(missingSource.clip, A8LoopUuid(),
                                              "LoopEmitter");
    CHECK_FALSE(missing.IsOk());
    CHECK(missing.error.code == Error::MissingAsset);

    // Foreign sidecar refuses: the loop's bytes under a scratch sidecar
    // claiming the music identity must not resolve as the loop clip.
    const std::filesystem::path dir = A8TempDir();
    {
        std::ifstream wavIn("RT2App/assets/" + std::string(kA8LoopClip),
                            std::ios::binary);
        REQUIRE(static_cast<bool>(wavIn));
        std::ostringstream wavBytes;
        wavBytes << wavIn.rdbuf();
        const std::string bytes = wavBytes.str();
        REQUIRE_FALSE(bytes.empty());
        std::ofstream wavOut((dir / "conflict.wav").string(),
                             std::ios::binary);
        wavOut.write(bytes.data(),
                     static_cast<std::streamsize>(bytes.size()));
        wavOut.close();
        std::ofstream metaOut((dir / "conflict.wav.rt2meta").string(),
                              std::ios::binary);
        metaOut << A8MusicClipId().ToString();
        metaOut.close();
        REQUIRE(static_cast<bool>(wavOut));
    }
    AudioClipAssetProvider conflictProvider;
    AssetResolutionContext conflictCtx;
    conflictCtx.assetRoot = dir;
    conflictCtx.database = nullptr;
    conflictProvider.SetContext(conflictCtx);
    AudioSourceComponent conflictSource = *loop;
    conflictSource.clip.path = "conflict.wav";
    const auto conflict = conflictProvider.ResolveClip(
        conflictSource.clip, A8LoopUuid(), "LoopEmitter");
    INFO("foreign sidecar detail: " << conflict.error.detail);
    CHECK_FALSE(conflict.IsOk());
    std::filesystem::remove_all(dir);
}

TEST_CASE("A8_AcceptanceScenePreviewReplaceAndStop")
{
    // Edit-mode preview on the shipped components: starting music
    // replaces (never stacks with) the loop preview, and Stop leaves
    // zero preview voices for Play to inherit.
    SceneDocument doc;
    Error loadErr;
    REQUIRE(A8LoadAcceptance(doc, loadErr));
    const AudioSourceComponent* loop = A8FindSource(doc, A8LoopUuid());
    const AudioSourceComponent* music = A8FindSource(doc, A8MusicUuid());
    REQUIRE(loop != nullptr);
    REQUIRE(music != nullptr);

    RecordingFakeAudioBackend fake;
    A8ScriptAcceptanceClips(fake);
    AudioPreviewController preview;
    preview.SetBackend(&fake);
    preview.SetClipProvider(&fake);
    preview.SetClipKeyBuilder(
        [](const AssetReference& clip, const UUID&, const std::string&)
        {
            return Result<std::string>::Ok(A8Key(clip.path));
        });

    const AudioListenerPose listener = A8Listener(0.0f, 0.0f, 0.0f);
    const float loopPos[3] = { 3.0f, 0.0f, 0.5f };
    Error err;
    const bool loopPreview = preview.StartPreview(
        A8LoopUuid(), "LoopEmitter", *loop, true, loopPos, listener, false,
        err);
    INFO("loop preview must start: " << err.detail);
    REQUIRE(loopPreview);
    CHECK(preview.HasPreview());
    CHECK(preview.PreviewVoiceCount() == 1);
    CHECK(preview.PreviewSource() == A8LoopUuid());

    const float musicPos[3] = { 0.0f, 0.0f, 0.0f };
    const bool musicPreview = preview.StartPreview(
        A8MusicUuid(), "MusicBed", *music, true, musicPos, listener, false,
        err);
    INFO("music preview must replace: " << err.detail);
    REQUIRE(musicPreview);
    CHECK(preview.PreviewVoiceCount() == 1);
    CHECK(preview.PreviewSource() == A8MusicUuid());

    preview.Update(listener, musicPos, true, false);
    CHECK(preview.PreviewVoiceCount() == 1);

    const bool stopped = preview.StopPreview(err);
    INFO("preview stop must succeed: " << err.detail);
    REQUIRE(stopped);
    CHECK_FALSE(preview.HasPreview());
    CHECK(preview.PreviewVoiceCount() == 0);
    CHECK(fake.LiveTokenCount() == 0);
    Error shutErr;
    CHECK(preview.Shutdown(shutErr));
}
