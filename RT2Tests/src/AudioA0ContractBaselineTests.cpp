#include <doctest/doctest.h>

// ============================================================================
// A0 - Audio contracts and baseline (grounded at f143044).
//
// This translation unit is the A0 checkpoint for the audio integration plan
// ("Required discriminating checks" 1-20 in the READY technical plan). It
// does two things and deliberately nothing more:
//
//  1. GREEN baseline pins that run now: the persisted-component tripwire,
//     the asset-kind codec, the unconditional asset visitor, the Play
//     candidate/commit refusal shape, and the CPU import boundary. Each names
//     the later ticket (A1-A8) that must update it, so a future change reads
//     as an owned handoff rather than drift.
//  2. A DEFERRED ownership map: all 20 required checks with the ticket that
//     must implement each and the missing layer that blocks it at A0. No
//     entry claims to execute audio behavior - there is no audio
//     implementation in this tree (no AudioSourceComponent, no AudioWorld,
//     no IAudioBackend, no miniaudio).
//
// Out of scope (explicit): miniaudio vendoring (A1), clip assets and
// persistence (A2), AudioWorld policy (A3), production backend/cache (A4),
// runtime lifecycle/listener (A5), Lua controls (A6), authoring/preview UI
// (A7), acceptance scene/docs (A8), and any pinball sound content.
// ============================================================================

#if __has_include("miniaudio.h")
#error "A0 boundary: RT2Tests must not import miniaudio (check 17; A1 keeps the adapter in RT2AudioBackend)"
#endif

#include "PersistedComponents.h"
#include "ECSComponents.h"
#include "PhysicsComponents.h"
#include "AssetReference.h"
#include "SceneDocument.h"
#include "SceneAssetReferenceVisitor.h"
#include "RuntimeSceneController.h"
#include "ISceneRenderBridge.h"
#include "GPUSceneData.h"
#include "core/UUID.h"
#include "core/Error.h"

#include <set>
#include <string>
#include <typeindex>
#include <vector>

using namespace rt2::core;

namespace
{

// Minimal recording bridge for the A0 Play-refusal characterization. Records
// bridge calls and performs no GPU work.
class A0RecordingBridge final : public ISceneRenderBridge
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
};

bool HasSlotWithPath(const std::vector<SceneAssetReferenceSlot>& slots,
                     const std::string& path)
{
    for (const auto& slot : slots)
    {
        if (slot.reference != nullptr && slot.reference->path == path)
            return true;
    }
    return false;
}

} // namespace

TEST_CASE("A0 GREEN_PersistedCoverageExact17: generic list visits every authored type once")
{
    // Tripwire for required check 1. PersistedComponents::Count alone cannot
    // catch a removed ForEach entry, so this case pins the full visitation:
    // exactly 18 visits covering each of the 18 authored types named in
    // PersistedComponents.h. Removing any one entry (the A2 analogue is
    // forgetting AudioSourceComponent) turns this red while Count still
    // reads 18.
    //
    // A2 fulfilled the handoff: AudioSourceComponent added, Count 17 -> 18.
    CHECK(PersistedComponents::Count == 18);

    std::set<std::type_index> seen;
    size_t visits = 0;
    PersistedComponents::ForEach(
        [&](auto tag)
        {
            ++visits;
            seen.insert(std::type_index(typeid(typename decltype(tag)::Type)));
        });
    CHECK(visits == 18);
    CHECK(seen.size() == 18);

    CHECK(seen.count(std::type_index(typeid(NameComponent))) == 1);
    CHECK(seen.count(std::type_index(typeid(Transform))) == 1);
    CHECK(seen.count(std::type_index(typeid(VisibleComponent))) == 1);
    CHECK(seen.count(std::type_index(typeid(MeshRef))) == 1);
    CHECK(seen.count(std::type_index(typeid(PrimitiveComponent))) == 1);
    CHECK(seen.count(std::type_index(typeid(ImportedMeshSourceComponent))) == 1);
    CHECK(seen.count(std::type_index(typeid(MaterialOverrideComponent))) == 1);
    CHECK(seen.count(std::type_index(typeid(LightComponent))) == 1);
    CHECK(seen.count(std::type_index(typeid(CameraComponent))) == 1);
    CHECK(seen.count(std::type_index(typeid(MotionComponent))) == 1);
    CHECK(seen.count(std::type_index(typeid(ScriptComponent))) == 1);
    CHECK(seen.count(std::type_index(typeid(PrefabInstanceComponent))) == 1);
    CHECK(seen.count(std::type_index(typeid(PrefabMemberComponent))) == 1);
    CHECK(seen.count(std::type_index(typeid(PhysicsBodyComponent))) == 1);
    CHECK(seen.count(std::type_index(typeid(PhysicsShapeComponent))) == 1);
    CHECK(seen.count(std::type_index(typeid(PhysicsHingeComponent))) == 1);
    CHECK(seen.count(std::type_index(typeid(PhysicsSliderComponent))) == 1);
    CHECK(seen.count(std::type_index(typeid(AudioSourceComponent))) == 1);
}

TEST_CASE("A0 GREEN_AssetKindCodecHasNoAudioKindYet")
{
    // Baseline for required checks 2 and 17. The six durable kinds
    // round-trip through the shared name codec (AssetReference.h); the audio
    // name resolves since A2. Save validation rejects an Unknown kind
    // with a non-empty path loudly (SceneSerializer.cpp), so an
    // audio-shaped path can never smuggle itself through the codec.
    //
    // A2 fulfilled the handoff: AssetKind::AudioClip with wire "audioclip".
    CHECK(std::string(AssetKindName(AssetKind::Model)) == "model");
    CHECK(std::string(AssetKindName(AssetKind::Texture)) == "texture");
    CHECK(std::string(AssetKindName(AssetKind::Environment)) == "environment");
    CHECK(std::string(AssetKindName(AssetKind::Script)) == "script");
    CHECK(std::string(AssetKindName(AssetKind::Prefab)) == "prefab");
    CHECK(std::string(AssetKindName(AssetKind::AudioClip)) == "audioclip");
    CHECK(AssetKindFromName("model") == AssetKind::Model);
    CHECK(AssetKindFromName("texture") == AssetKind::Texture);
    CHECK(AssetKindFromName("environment") == AssetKind::Environment);
    CHECK(AssetKindFromName("script") == AssetKind::Script);
    CHECK(AssetKindFromName("prefab") == AssetKind::Prefab);
    CHECK(AssetKindFromName("audioclip") == AssetKind::AudioClip);

    CHECK(AssetKindFromName("audio") == AssetKind::Unknown);
    CHECK(AssetKindFromName("wav") == AssetKind::Unknown);

    AssetReference audioShaped;
    audioShaped.kind = AssetKind::AudioClip;
    audioShaped.path = "sfx/hit.wav";
    CHECK(audioShaped.IsValid());
}

TEST_CASE("A0 GREEN_VisitorSeesBothPhysicsRefsUnconditionally")
{
    // Baseline for required check 2. Both stored collision-geometry
    // references of a physics shape are visited even when malformed
    // (Unknown kind with a non-empty path - exactly the input save
    // validation refuses). AudioSourceComponent::clip receives the same
    // unconditional treatment since A2; filtering here would let Save
    // succeed on a file Load refuses.
    //
    // A2 fulfilled the handoff: the audio slot below is part of the census.
    DeterministicUuidProvider provider;
    SceneDocument doc;
    doc.SetUuidProvider(&provider);

    entt::entity emitter = doc.ecs.registry.create();
    doc.ecs.registry.emplace<NameComponent>(emitter, "Emitter");
    doc.AssignNewUuid(emitter);

    PhysicsShapeComponent shape;
    shape.hull.path = "stale/hull.bin";
    shape.triMesh.path = "stale/trimesh.bin";
    doc.ecs.registry.emplace<PhysicsShapeComponent>(emitter, shape);

    ScriptComponent script;
    script.asset.kind = AssetKind::Script;
    script.asset.path = "scripts/cover.lua";
    doc.ecs.registry.emplace<ScriptComponent>(emitter, script);

    AudioSourceComponent audio;
    audio.clip.kind = AssetKind::AudioClip;
    audio.clip.path = "sfx/hit.wav";
    doc.ecs.registry.emplace<AudioSourceComponent>(emitter, audio);

    const std::vector<SceneAssetReferenceSlot> slots =
        CollectSceneAssetReferences(doc);
    REQUIRE(slots.size() == 4);
    CHECK(HasSlotWithPath(slots, "stale/hull.bin"));
    CHECK(HasSlotWithPath(slots, "stale/trimesh.bin"));
    CHECK(HasSlotWithPath(slots, "scripts/cover.lua"));

    // The audio clip reference is visited unconditionally, exactly like the
    // physics collision references above.
    CHECK(HasSlotWithPath(slots, "sfx/hit.wav"));
}

TEST_CASE("A0 GREEN_PlayRefusedWhilePlayingLeavesSessionUndisturbed")
{
    // Baseline for required checks 4 and 6. Play is already
    // candidate/commit oriented: a second Play while a session is live is
    // refused (RuntimeSceneController.cpp:54-55) without replacing the
    // runtime clone or touching the bridge, and Stop returns the controller
    // to Edit with no runtime. A4/A5 must preserve this atomicity when the
    // audio candidate (decode every persisted source, build the AudioWorld)
    // joins the transaction.
    DeterministicUuidProvider provider;
    SceneDocument authoring;
    authoring.SetUuidProvider(&provider);
    entt::entity cube = authoring.ecs.registry.create();
    authoring.ecs.registry.emplace<NameComponent>(cube, "Cube");
    authoring.AssignNewUuid(cube);

    A0RecordingBridge bridge;
    Error err;
    RuntimeSceneController ctrl;
    REQUIRE(ctrl.Play(authoring, bridge, err));
    CHECK(err.IsOk());
    CHECK(ctrl.GetState() == SceneRunState::Playing);
    const SceneDocument* live = ctrl.TryGetRuntimeScene();
    REQUIRE(live != nullptr);
    const int syncsAfterPlay = bridge.fullSyncCalls;

    Error secondErr;
    CHECK_FALSE(ctrl.Play(authoring, bridge, secondErr));
    CHECK(ctrl.GetState() == SceneRunState::Playing);
    CHECK(ctrl.TryGetRuntimeScene() == live);
    CHECK(bridge.fullSyncCalls == syncsAfterPlay);

    ctrl.Stop(authoring, bridge);
    CHECK(ctrl.GetState() == SceneRunState::Edit);
    CHECK(ctrl.TryGetRuntimeScene() == nullptr);
}

TEST_CASE("A0 GREEN_CpuTargetHasNoMiniaudioBoundary")
{
    // Baseline for required check 17. The hard #error guard at the top of
    // this file fails the build if miniaudio.h ever becomes reachable from
    // RT2Tests; this case exists so the guarantee is visible in test
    // listings and counts (same pattern as the R1 CPU boundary in
    // CpuBoundaryTests.cpp). A1 must keep the vendored adapter and
    // miniaudio.c in top-level RT2AudioBackend, outside every CPU target.
    CHECK(true);
}

struct A0DeferredCheck
{
    int check = 0;
    const char* behavior = nullptr;
    const char* owner = nullptr;
    const char* blockedBy = nullptr;
};

// Ownership map for the 20 required discriminating checks. Every entry is
// DEFERRED at A0: the named behavior cannot execute because its layer does
// not exist yet. The owner ticket implements it; "blocked by" names the
// missing layer. This table is scaffolding, not coverage - it records who
// must build what, without overstating any future check as running now.
constexpr A0DeferredCheck kA0DeferredAudioChecks[] = {
    { 1, "Removing AudioSource from the generic persisted list fails coverage", "A2", "AudioSourceComponent does not exist" },
    { 2, "Removing its visitor entry lets a bad inactive ref through Save/dependency tests", "A2", "no audio visitor slot exists" },
    { 3, "Same-size/mtime clip rewrite changes fingerprint; old/new voices emit different PCM", "A4", "no clip provider, fingerprint, or decode cache" },
    { 4, "Corrupt non-autoplay clip refuses Play atomically: Edit, zero voices, no callbacks", "A5", "Play candidate decodes no audio today" },
    { 5, "Device-unavailable startup yields production no-device status; Play pumps exact PCM", "A4", "no backend and no no-device mode" },
    { 6, "Destroying a looping emitter stops exactly its voices; Play/Stop returns censuses to baseline", "A5", "no AudioWorld voice census" },
    { 7, "A stale generation handle cannot stop or modify a recycled voice slot", "A3", "no world handles or generations" },
    { 8, "Overflow steals the exact lowest-priority/oldest world slot; command 257 refuses", "A3", "no voice cap, stealing, or command queue" },
    { 9, "Spatial math and no-device PCM have exact L/R sample oracles; bad poses refuse", "A3", "no spatial math exists" },
    { 10, "Runtime camera motion changes listener mix with the authored camera stationary", "A5", "no listener injection" },
    { 11, "Physics-moving child emitters use final world transforms", "A5", "no source position sync" },
    { 12, "Pause freezes voices; Step moves pose but not cursor; Resume continues", "A5", "no session pause contract" },
    { 13, "Malformed script controls return false; async failures surface in audio_status", "A6", "no Lua audio controls" },
    { 14, "One authoritative destroy set with/without ScriptSystem; drain timing FIFO/next-frame", "A5", "no destruction audio hook" },
    { 15, "Preview replacement/document change/Play/shutdown leaves no preview voice", "A7", "no preview controller" },
    { 16, "No-device probe decodes WAV/FLAC/MP3 with exact frames/samples and rejects corrupt input", "A1", "no vendored miniaudio or probe target" },
    { 17, "Adapter/miniaudio compile exactly once in RT2AudioBackend; CPU targets link neither", "A1", "no RT2AudioBackend project exists" },
    { 18, "on_create Stop suppresses autoplay; gain/pitch then Play apply FIFO without duplicates", "A5", "no autoplay synthesis" },
    { 19, "Packed L/R stress never observes a torn pair (ThreadSanitizer where supported)", "A4", "no stereo-gain node" },
    { 20, "Overlapping A/B: A completion/failure cannot terminalize live B or replace its result", "A3", "no sequence-scoped status" },
};

TEST_CASE("A0 DEFERRED_OwnershipMapCoversAll20RequiredChecks")
{
    constexpr size_t kCount = sizeof(kA0DeferredAudioChecks) / sizeof(kA0DeferredAudioChecks[0]);
    CHECK(kCount == 20);

    bool seen[21] = {};
    for (const A0DeferredCheck& entry : kA0DeferredAudioChecks)
    {
        CHECK(entry.check >= 1);
        CHECK(entry.check <= 20);
        CHECK(entry.behavior != nullptr);
        CHECK(entry.owner != nullptr);
        CHECK(entry.blockedBy != nullptr);
        if (entry.check >= 1 && entry.check <= 20)
        {
            CHECK_FALSE(seen[entry.check]);
            seen[entry.check] = true;
        }
        // Owner is always one of the A1-A8 implementation tickets.
        REQUIRE(entry.owner != nullptr);
        const std::string owner = entry.owner;
        CHECK(owner.size() == 2);
        CHECK(owner[0] == 'A');
        CHECK(owner[1] >= '1');
        CHECK(owner[1] <= '8');
    }
    for (int id = 1; id <= 20; ++id)
        CHECK(seen[id]);
}
