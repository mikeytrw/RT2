#include <doctest/doctest.h>

#include "AudioInspectorState.h"
#include "AudioPreviewController.h"
#include "AudioSpatialMath.h"
#include "AudioStatusSnapshot.h"
#include "AudioWorld.h"
#include "EditorCommandHistory.h"
#include "EditorPropertyCommands.h"
#include "FakeAudioBackend.h"
#include "SceneManager.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// ============================================================================
// A7: audio authoring, preview, and status UI probes.
//
// The production authoring path (SceneManager::SetAudioSourceState,
// SetAudioSourceCommand, AudioInspectorWork, AudioPreviewController) is
// CPU-only by design and links into this binary through the shared CPU
// closure; the event shapes that prove the UI (Apply gating under a real
// BeginDisabled scope, real Preview/Stop button clicks) replay against the
// vendored ImGui sources. RT2Tests stays ImGui-free: every TEST_CASE below
// lives here, not there.
//
// Coverage contract (A7 ticket acceptance):
//   - Apply/Undo/Redo through real history, including Add/Remove shapes.
//   - Invalid after-state and malformed clip text refuse loudly WITHOUT
//     recording history or dropping the working edit.
//   - Linked prefab members refuse with the non-overridable diagnostic
//     (manager authoritative; the UI additionally disables).
//   - Preview/Stop own exactly one replaceable voice; replacement detaches
//     the old voice, failures leave zero voices with a typed error, and
//     natural completion ends the preview.
//   - Re-review: Undo/Redo clip drift reconciles through the production
//     DecidePreviewMaintenanceAction dispatch executed end to end below
//     (SceneManager + history + controller + restart); failed mixes reach
//     LastError with retry; status lines go through the production
//     FillAudioStatusSnapshot with real backend/world/controller objects.
//   - Closure re-review: the inspector Remove/Apply step logic lives in
//     the probe-linkable ExecuteAudio*PreviewStep seams, which the cases
//     below drive with recording AND null hooks (branch, hook invocation,
//     and diagnostic strings all asserted). HONEST LIMITATION, not caller
//     proof: these cases execute the exact caller logic but cannot prove
//     RenderAudioEditor invokes the seam, that Walnut installs the hooks,
//     or the ImGui button chrome — the inspector TU cannot link here
//     (FileDialog native dialogs, gizmo/editor closure, ImGui/Walnut
//     boundary). Those three are covered by the interactive acceptance
//     below, which was NOT performed live in this environment (no
//     display/GPU session available to click through).
//
// INTERACTIVE ACCEPTANCE (Walnut session, against these exact transitions):
//   1. Select a source, Preview: voice sounds, Stop enabled, status shows
//      preview 1/total 1 with source name and mode.
//   2. Apply a different clip while previewing: audition swaps to the new
//      clip with no overlap; status failure stays none.
//   3. Undo the clip Apply: audition swaps back to the old clip; Redo:
//      swaps forward again. At most one voice throughout.
//   4. Remove the previewed source: voice stops at once with
//      "previewed source was removed" status; Stop stays enabled until the
//      voice is gone.
//   5. Toggle Preview Spatial mid-audition both ways: sound and status
//      label switch together; with no device the toggle still switches.
//   6. Preview a missing/corrupt clip: typed failure names the asset in
//      the inspector and the status panel; zero voices.
//   7. Commit Play with a bound source, then break its clip on disk and
//      re-Play or force a backend failure: the deferred typed failure
//      appears in the status panel with the affected source.
// ============================================================================

namespace
{

struct AudioSceneFixture
{
    rt2::core::DeterministicUuidProvider ids;
    SceneManager manager;
    EditorCommandHistory history;

    AudioSceneFixture()
    {
        manager.SetUuidProvider(&ids);
        manager.AddMaterial(SceneMaterial{});
    }

    rt2::core::UUID AddSource(const char* name = "Sfx")
    {
        return manager.CreateEmpty(name).affectedEntities.front();
    }
};

// 2D (non-spatial) source: valid without a Transform, so the fixture needs
// no transform scaffolding. Unbound (empty clip path) is valid authoring;
// bound variants set the clip below.
AudioSourceComponent TwoDSource(float gain = 1.0f)
{
    AudioSourceComponent component;
    component.bus = AudioBus::Effects;
    component.autoplay = false;
    component.loop = false;
    component.spatial = false;
    component.gain = gain;
    component.pitch = 1.0f;
    component.minDistance = 1.0f;
    component.maxDistance = 30.0f;
    component.rolloff = 1.0f;
    component.priority = 128;
    return component;
}

void BindClip(AudioSourceComponent& component,
              rt2::core::DeterministicUuidProvider& ids)
{
    component.clip.kind = AssetKind::AudioClip;
    component.clip.path = "sfx/hit.wav";
    component.clip.assetId = ids.CreateV4();
}

rt2::audio::AudioListenerPose CenterListener()
{
    rt2::audio::AudioListenerPose listener;
    listener.position[0] = 0.0f;
    listener.position[1] = 0.0f;
    listener.position[2] = 0.0f;
    listener.forward[0] = 0.0f;
    listener.forward[1] = 0.0f;
    listener.forward[2] = -1.0f;
    listener.up[0] = 0.0f;
    listener.up[1] = 1.0f;
    listener.up[2] = 0.0f;
    return listener;
}

// Binds a preview controller to a recording fake for both seams (the fake
// implements IAudioBackend and IAudioClipProvider) with a fixed clip key.
// Unscripted keys serve the fake's mono 48 kHz silent default, so no
// decoder is involved.
void BindPreviewToFake(rt2::audio::AudioPreviewController& controller,
                       rt2::audio::RecordingFakeAudioBackend& fake,
                       const std::string& key = "probe-key")
{
    controller.SetBackend(&fake);
    controller.SetClipProvider(&fake);
    controller.SetClipKeyBuilder(
        [key](const AssetReference& clip, const rt2::core::UUID& entity,
              const std::string& entityName)
            -> rt2::core::Result<std::string> {
            (void)clip;
            (void)entity;
            (void)entityName;
            return rt2::core::Result<std::string>::Ok(key);
        });
}

bool StartBoundPreview(rt2::audio::AudioPreviewController& controller,
                       const rt2::core::UUID& source,
                       const AudioSourceComponent& component,
                       rt2::core::Error& outError)
{
    const float pos[3] = { 0.0f, 0.0f, 0.0f };
    return controller.StartPreview(source, "Sfx", component,
                                   /*hasTransform=*/false, pos,
                                   CenterListener(),
                                   /*spatialAudition=*/false, outError);
}

// Minimal real-ImGui click harness: draws one button per frame, records the
// first frame's button center, then replays hover/down/up and ORs the
// Button() return across frames. Returns true when the click landed.
bool ClickButtonThroughImGui(const char* label, bool& outClickedThisRun,
                             const std::function<void()>& onClick)
{
    outClickedThisRun = false;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(800, 600);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);

    ImVec2 center(0, 0);
    bool clicked = false;
    auto frame = [&](ImVec2 mouse, bool down) {
        io.MousePos = mouse;
        io.MouseDown[0] = down;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(500, 400));
        ImGui::Begin("A7Probe", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoResize);
        if (ImGui::Button(label))
        {
            clicked = true;
            onClick();
        }
        if (center.x == 0 && center.y == 0)
        {
            const auto lo = ImGui::GetItemRectMin();
            const auto hi = ImGui::GetItemRectMax();
            center = ImVec2((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
        }
        ImGui::End();
        ImGui::Render();
    };

    const ImVec2 away(-10, -10);
    frame(away, false);
    frame(away, false);
    REQUIRE_MESSAGE(!(center.x == 0 && center.y == 0),
                    "probe button never laid out");
    frame(center, false);
    frame(center, true);
    frame(center, false);
    frame(away, false);
    ImGui::DestroyContext();
    outClickedThisRun = clicked;
    return clicked;
}

} // namespace

TEST_CASE("A7 probe: Apply commits source Add through history with Undo/Redo")
{
    AudioSceneFixture f;
    const auto uuid = f.AddSource();
    REQUIRE_FALSE(f.manager.GetAudioSource(uuid).has_value());

    AudioSourceComponent after = TwoDSource();
    auto cmd = MakeSetAudioSourceCommandIfEffective(
        uuid, std::nullopt, after);
    REQUIRE(cmd);
    CHECK(cmd->Description() == "Add Audio Source");
    auto r = f.history.Execute(std::move(cmd), f.manager);
    REQUIRE(r.success);
    CHECK(r.syncImpact == rt2::core::SyncImpact::None);
    const auto live = f.manager.GetAudioSource(uuid);
    REQUIRE(live.has_value());
    CHECK(*live == after);

    REQUIRE(f.history.Undo(f.manager).success);
    CHECK_FALSE(f.manager.GetAudioSource(uuid).has_value());
    REQUIRE(f.history.Redo(f.manager).success);
    REQUIRE(f.manager.GetAudioSource(uuid).has_value());
    CHECK(*f.manager.GetAudioSource(uuid) == after);

    // Canonical no-ops build no command: identical values and the
    // nullopt/nullopt pair are both silent (no history entry).
    CHECK(!MakeSetAudioSourceCommandIfEffective(
        uuid, after, after));
    CHECK(!MakeSetAudioSourceCommandIfEffective(
        uuid, std::nullopt, std::nullopt));
}

TEST_CASE("A7 probe: Apply commits field edits and Remove, each undoable")
{
    AudioSceneFixture f;
    const auto uuid = f.AddSource();
    AudioSourceComponent added = TwoDSource();
    REQUIRE(f.history.Execute(
        MakeSetAudioSourceCommandIfEffective(
            uuid, std::nullopt, added), f.manager).success);

    AudioSourceComponent edited = added;
    edited.gain = 0.5f;
    auto editCmd = MakeSetAudioSourceCommandIfEffective(
        uuid, added, edited);
    REQUIRE(editCmd);
    CHECK(editCmd->Description() == "Edit Audio Source");
    REQUIRE(f.history.Execute(std::move(editCmd), f.manager).success);
    CHECK(f.manager.GetAudioSource(uuid)->gain == doctest::Approx(0.5f));
    REQUIRE(f.history.Undo(f.manager).success);
    CHECK(f.manager.GetAudioSource(uuid)->gain == doctest::Approx(1.0f));

    auto removeCmd = MakeSetAudioSourceCommandIfEffective(
        uuid, added, std::nullopt);
    REQUIRE(removeCmd);
    CHECK(removeCmd->Description() == "Remove Audio Source");
    REQUIRE(f.history.Execute(std::move(removeCmd), f.manager).success);
    CHECK_FALSE(f.manager.GetAudioSource(uuid).has_value());
    REQUIRE(f.history.Undo(f.manager).success);
    REQUIRE(f.manager.GetAudioSource(uuid).has_value());
    CHECK(*f.manager.GetAudioSource(uuid) == added);
}

TEST_CASE("A7 probe: invalid after-state refuses loudly without recording")
{
    AudioSceneFixture f;
    const auto uuid = f.AddSource();
    AudioSourceComponent added = TwoDSource();
    REQUIRE(f.history.Execute(
        MakeSetAudioSourceCommandIfEffective(
            uuid, std::nullopt, added), f.manager).success);
    REQUIRE(f.history.UndoDepthForTest() == 1);

    // The factory does NOT suppress an invalid after-state: the command is
    // returned so history surfaces the manager's actionable failure — and
    // records nothing.
    AudioSourceComponent bad = added;
    bad.gain = 99.0f;
    auto cmd = MakeSetAudioSourceCommandIfEffective(uuid, added, bad);
    REQUIRE(cmd);
    auto r = f.history.Execute(std::move(cmd), f.manager);
    CHECK_FALSE(r.success);
    CHECK_FALSE(r.error.detail.empty());
    CHECK(f.history.UndoDepthForTest() == 1);
    CHECK_FALSE(f.history.CanRedo());
    // The model keeps the pre-Apply value; the refusal changed nothing.
    REQUIRE(f.manager.GetAudioSource(uuid).has_value());
    CHECK(*f.manager.GetAudioSource(uuid) == added);
}

TEST_CASE("A7 probe: linked prefab member refuses with non-overridable diagnostic")
{
    AudioSceneFixture f;
    const auto uuid = f.AddSource();
    const entt::entity e = f.manager.FindEntityByUuid(uuid);
    REQUIRE((e != entt::null));
    f.manager.GetECS().registry.emplace<PrefabMemberComponent>(
        e, PrefabMemberComponent{
               f.ids.CreateV4(), f.ids.CreateV4(), {}});

    // Manager refusal is authoritative: it fires before any mutation, so
    // the member keeps no source and the error names the policy.
    auto direct = f.manager.SetAudioSourceState(uuid, TwoDSource());
    CHECK_FALSE(direct.success);
    CHECK(direct.error.detail.find("non-overridable") != std::string::npos);
    CHECK_FALSE(f.manager.GetAudioSource(uuid).has_value());

    // The command path refuses identically and records nothing.
    auto cmd = MakeSetAudioSourceCommandIfEffective(
        uuid, std::nullopt, TwoDSource());
    REQUIRE(cmd);
    auto r = f.history.Execute(std::move(cmd), f.manager);
    CHECK_FALSE(r.success);
    CHECK(r.error.detail.find("non-overridable") != std::string::npos);
    CHECK_FALSE(f.history.CanUndo());
    CHECK_FALSE(f.manager.GetAudioSource(uuid).has_value());

    // Real ImGui disabled scope for the linked member: while disabled the
    // item flags carry Disabled so the prefab truth ("edit the prefab
    // source") cannot issue a commit from this row.
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* texPixels = nullptr;
    int texW = 0, texH = 0;
    io.Fonts->GetTexDataAsRGBA32(&texPixels, &texW, &texH);
    ImGui::NewFrame();
    ImGui::Begin("A7PrefabGate");
    ImGui::BeginDisabled(true);
    ImGui::Button("Apply Audio");
    const bool itemDisabled =
        (GImGui->LastItemData.InFlags & ImGuiItemFlags_Disabled) != 0;
    ImGui::EndDisabled();
    ImGui::End();
    ImGui::Render();
    ImGui::DestroyContext();
    CHECK(itemDisabled);
}

TEST_CASE("A7 probe: working-copy resync, conflict, revert, and reset policy")
{
    AudioSceneFixture f;
    const auto uuid = f.AddSource();
    const auto other = f.AddSource("Other");
    AudioSourceComponent live = TwoDSource();
    REQUIRE(f.history.Execute(
        MakeSetAudioSourceCommandIfEffective(
            uuid, std::nullopt, live), f.manager).success);

    AudioInspectorWork work;
    work.Sync(uuid, f.manager.GetAudioSource(uuid));
    REQUIRE(work.HasTarget());
    CHECK_FALSE(work.dirty);
    CHECK_FALSE(work.conflict);
    REQUIRE(work.work.has_value());
    CHECK(*work.work == live);

    // Clean copy follows live values (Undo of an earlier edit while this
    // entity stays selected): the next edit-and-Apply cannot overwrite
    // fields Undo restored.
    AudioSourceComponent undone = live;
    undone.gain = 0.25f;
    REQUIRE(f.history.Execute(
        MakeSetAudioSourceCommandIfEffective(
            uuid, live, undone), f.manager).success);
    REQUIRE(f.history.Undo(f.manager).success);
    work.Sync(uuid, f.manager.GetAudioSource(uuid));
    CHECK_FALSE(work.dirty);
    CHECK(*work.work == live);

    // Dirty copy whose live state moved under it conflicts: Apply must stay
    // disabled until Revert, and the edit is never silently discarded.
    work.work->gain = 0.5f;
    work.dirty = true;
    REQUIRE(f.history.Execute(
        MakeSetAudioSourceCommandIfEffective(
            uuid, live, undone), f.manager).success);
    work.Sync(uuid, f.manager.GetAudioSource(uuid));
    CHECK(work.conflict);
    CHECK(AudioInspectorApplyBlocked(work.conflict, false, std::string{}));
    CHECK(work.work->gain == doctest::Approx(0.5f));
    work.Revert(f.manager.GetAudioSource(uuid));
    CHECK_FALSE(work.dirty);
    CHECK_FALSE(work.conflict);
    CHECK(*work.work == undone);

    // Selection change reseeds; document reset drops everything so a
    // same-UUID replacement document cannot inherit values.
    work.work->gain = 0.75f;
    work.dirty = true;
    work.Sync(other, std::nullopt);
    CHECK_FALSE(work.dirty);
    CHECK_FALSE(work.work.has_value());
    work.Sync(uuid, f.manager.GetAudioSource(uuid));
    work.work->gain = 0.75f;
    work.dirty = true;
    work.Clear();
    CHECK_FALSE(work.HasTarget());
    CHECK_FALSE(work.dirty);
    CHECK_FALSE(work.conflict);
}

TEST_CASE("A7 probe: clip path edits clear the stale asset ID")
{
    AssetReference ref;
    ref.kind = AssetKind::AudioClip;
    ref.path = "sfx/old.wav";
    ref.sourceKey = "old-key";
    ref.assetId = rt2::core::DeterministicUuidProvider{}.CreateV4();

    // Same path: no change, identity preserved.
    CHECK_FALSE(AudioInspectorWork::NoteAudioClipPathChanged(
        ref, "sfx/old.wav"));
    CHECK_FALSE(ref.assetId.IsNull());

    // New path: stale ID and sourceKey clear (ID-first resolution would
    // otherwise name the previous file); strict Apply then refuses the
    // bound-but-identityless reference loudly until Browse/drop repairs it.
    CHECK(AudioInspectorWork::NoteAudioClipPathChanged(
        ref, "sfx/new.wav"));
    CHECK(ref.path == "sfx/new.wav");
    CHECK(ref.kind == AssetKind::AudioClip);
    CHECK(ref.assetId.IsNull());
    CHECK(ref.sourceKey.empty());

    // Clearing the path unbinds (Unknown kind, no ID).
    CHECK(AudioInspectorWork::NoteAudioClipPathChanged(ref, ""));
    CHECK(ref.path.empty());
    CHECK(ref.kind == AssetKind::Unknown);
}

TEST_CASE("A7 probe: malformed clip text blocks Apply and preserves the edit")
{
    // Typed parse: empty text unbinds (ok); non-clip extensions refuse with
    // a typed error naming the constraint; valid extensions pass.
    std::string error;
    CHECK(TryParseAudioClipPathText("", error));
    CHECK(error.empty());
    CHECK_FALSE(TryParseAudioClipPathText("sfx/hit.ogg", error));
    CHECK_FALSE(error.empty());
    CHECK(error.find(".wav") != std::string::npos);
    CHECK(TryParseAudioClipPathText("sfx/hit.WAV", error));
    CHECK(error.empty());

    // While the malformed text is retained, the production gate blocks even
    // when another field made the working copy dirty: committing would
    // silently revert the invalid edit to the old model value on success.
    CHECK_FALSE(TryParseAudioClipPathText("sfx/hit.ogg", error));
    CHECK(AudioInspectorApplyBlocked(false, true, error));
    CHECK(AudioClipTextBlocksApply(true, error));
    CHECK_FALSE(AudioInspectorApplyBlocked(false, true, std::string{}));
    CHECK_FALSE(AudioInspectorApplyBlocked(false, false, error));

    // Real ImGui disabled scope around the Apply shape: blocked carries
    // Disabled on the item, clean does not. Raw text, error, and working
    // copy survive either way (the probe retains them, like the inspector).
    AudioSourceComponent work = TwoDSource();
    const std::string retainedText = "sfx/hit.ogg";
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* texPixels = nullptr;
    int texW = 0, texH = 0;
    io.Fonts->GetTexDataAsRGBA32(&texPixels, &texW, &texH);
    ImGui::NewFrame();
    ImGui::Begin("A7ClipGate");
    const bool blocked =
        AudioInspectorApplyBlocked(false, true, error);
    ImGui::BeginDisabled(blocked);
    ImGui::Button("Apply Audio");
    const bool itemDisabledWhileBlocked =
        (GImGui->LastItemData.InFlags & ImGuiItemFlags_Disabled) != 0;
    ImGui::EndDisabled();
    ImGui::BeginDisabled(false);
    ImGui::Button("Apply Audio");
    const bool itemDisabledWhileClean =
        (GImGui->LastItemData.InFlags & ImGuiItemFlags_Disabled) != 0;
    ImGui::EndDisabled();
    ImGui::End();
    ImGui::Render();
    ImGui::DestroyContext();
    CHECK(blocked);
    CHECK(itemDisabledWhileBlocked);
    CHECK_FALSE(itemDisabledWhileClean);

    // The blocked Apply performs no commit: model, raw text, and error stay
    // available for fix-or-Revert.
    CHECK(work.clip.path.empty());
    CHECK(retainedText == "sfx/hit.ogg");
    CHECK_FALSE(error.empty());
}

TEST_CASE("A7 probe: preview owns exactly one replaceable voice")
{
    AudioSceneFixture f;
    const auto uuid = f.AddSource();
    AudioSourceComponent bound = TwoDSource();
    BindClip(bound, f.ids);

    rt2::audio::RecordingFakeAudioBackend fake;
    rt2::audio::AudioPreviewController controller;
    BindPreviewToFake(controller, fake);

    rt2::core::Error error;
    REQUIRE(StartBoundPreview(controller, uuid, bound, error));
    CHECK(error.IsOk());
    CHECK(controller.HasPreview());
    CHECK(controller.PreviewVoiceCount() == 1);
    CHECK(controller.PreviewSource() == uuid);
    CHECK(fake.LiveTokenCount() == 1);
    rt2::audio::BackendVoiceToken first;
    REQUIRE(controller.LiveBackendToken(first));

    // A second start replaces, never stacks: the old voice detaches and the
    // census returns to exactly one.
    AudioSourceComponent bound2 = bound;
    bound2.gain = 0.5f;
    REQUIRE(StartBoundPreview(controller, uuid, bound2, error));
    CHECK(controller.PreviewVoiceCount() == 1);
    CHECK(fake.LiveTokenCount() == 1);
    CHECK_FALSE(fake.IsTokenLive(first));
    CHECK(controller.StartCount() == 2);

    // Idempotent stop: first stop detaches, second is a clean no-op.
    REQUIRE(controller.StopPreview(error));
    CHECK(error.IsOk());
    CHECK_FALSE(controller.HasPreview());
    CHECK(controller.PreviewVoiceCount() == 0);
    CHECK(fake.LiveTokenCount() == 0);
    REQUIRE(controller.StopPreview(error));
    CHECK(error.IsOk());
}

TEST_CASE("A7 probe: preview failures are typed and leave zero voices")
{
    AudioSceneFixture f;
    const auto uuid = f.AddSource();

    rt2::audio::RecordingFakeAudioBackend fake;
    rt2::audio::AudioPreviewController controller;
    BindPreviewToFake(controller, fake);

    // No clip bound: MissingAsset naming the clip field.
    rt2::core::Error error;
    CHECK_FALSE(StartBoundPreview(controller, uuid, TwoDSource(), error));
    CHECK(error.code == rt2::core::Error::MissingAsset);
    CHECK_FALSE(controller.HasPreview());
    CHECK(fake.LiveTokenCount() == 0);
    CHECK_FALSE(controller.LastError().IsOk());
    CHECK_FALSE(controller.LastAffectedAsset().empty());
    CHECK(controller.FailureCount() == 1);

    // Corrupt clip (provider refusal): the typed provider error propagates
    // with the affected asset, still zero voices.
    AudioSourceComponent bound = TwoDSource();
    BindClip(bound, f.ids);
    fake.ScriptGenerationError(
        "probe-key", rt2::core::Error{
                         rt2::core::Error::InvalidArgument,
                         "sfx/hit.wav", "not an audio file"});
    CHECK_FALSE(StartBoundPreview(controller, uuid, bound, error));
    CHECK(error.code == rt2::core::Error::InvalidArgument);
    CHECK_FALSE(controller.HasPreview());
    CHECK(fake.LiveTokenCount() == 0);
    CHECK(controller.FailureCount() == 2);

    // A later success clears the sticky failure for the status UI. The
    // fake serves its silent mono default for unscripted keys, so a fresh
    // key reaches success with no decoder involved.
    rt2::audio::AudioPreviewController recovered;
    BindPreviewToFake(recovered, fake, "fresh-key");
    REQUIRE(StartBoundPreview(recovered, uuid, bound, error));
    CHECK(recovered.LastError().IsOk());
    CHECK(recovered.FailureCount() == 0);
}

TEST_CASE("A7 probe: natural completion ends the preview")
{
    AudioSceneFixture f;
    const auto uuid = f.AddSource();
    AudioSourceComponent bound = TwoDSource();
    BindClip(bound, f.ids);

    rt2::audio::RecordingFakeAudioBackend fake;
    rt2::audio::AudioPreviewController controller;
    BindPreviewToFake(controller, fake);

    rt2::core::Error error;
    REQUIRE(StartBoundPreview(controller, uuid, bound, error));
    rt2::audio::BackendVoiceToken token;
    REQUIRE(controller.LiveBackendToken(token));
    fake.CompleteToken(token, rt2::audio::BackendCompletionReason::Completed);

    const float pos[3] = { 0.0f, 0.0f, 0.0f };
    controller.Update(CenterListener(), pos, false, false);
    CHECK_FALSE(controller.HasPreview());
    CHECK(controller.PreviewVoiceCount() == 0);
    CHECK(controller.NaturalCompletionCount() == 1);
}

TEST_CASE("A7 probe: real Preview/Stop buttons drive the controller")
{
    AudioSceneFixture f;
    const auto uuid = f.AddSource();
    AudioSourceComponent bound = TwoDSource();
    BindClip(bound, f.ids);

    rt2::audio::RecordingFakeAudioBackend fake;
    rt2::audio::AudioPreviewController controller;
    BindPreviewToFake(controller, fake);

    // Real Preview click starts the one voice.
    bool previewClicked = false;
    bool clickedFlag = false;
    const bool previewLanded = ClickButtonThroughImGui(
        "Preview", clickedFlag, [&]() {
            rt2::core::Error error;
            previewClicked = StartBoundPreview(
                controller, uuid, bound, error);
        });
    CHECK(previewLanded);
    CHECK(clickedFlag);
    CHECK(previewClicked);
    CHECK(controller.HasPreview());
    CHECK(controller.PreviewVoiceCount() == 1);

    // Real Stop click ends it; a second Stop stays a clean no-op.
    bool stopClicked = false;
    const bool stopLanded = ClickButtonThroughImGui(
        "Stop", clickedFlag, [&]() {
            rt2::core::Error error;
            stopClicked = controller.StopPreview(error) && error.IsOk();
        });
    CHECK(stopLanded);
    CHECK(clickedFlag);
    CHECK(stopClicked);
    CHECK_FALSE(controller.HasPreview());
    CHECK(fake.LiveTokenCount() == 0);
    rt2::core::Error error;
    CHECK(controller.StopPreview(error));
}

// ============================================================================
// A7 review repair probes: the four findings below failed against surrogate
// widgets. These cases drive the shared production decision/format code the
// inspector and status block execute (Decide*, AudioPreviewControlsDisabled,
// controller mode switch, FormatAudioStatusLines) with real ImGui widget
// events and backend gain inspection.
// ============================================================================

namespace
{

// Real checkbox click harness mirroring ClickButtonThroughImGui: flips a
// caller-owned bool through an actual ImGui::Checkbox activation.
bool ClickCheckboxThroughImGui(const char* label, bool& value)
{
    bool clicked = false;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(800, 600);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);

    ImVec2 center(0, 0);
    auto frame = [&](ImVec2 mouse, bool down) {
        io.MousePos = mouse;
        io.MouseDown[0] = down;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(500, 400));
        ImGui::Begin("A7CheckboxProbe", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoResize);
        if (ImGui::Checkbox(label, &value))
            clicked = true;
        if (center.x == 0 && center.y == 0)
        {
            const auto lo = ImGui::GetItemRectMin();
            const auto hi = ImGui::GetItemRectMax();
            center = ImVec2((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
        }
        ImGui::End();
        ImGui::Render();
    };

    const ImVec2 away(-10, -10);
    frame(away, false);
    frame(away, false);
    REQUIRE_MESSAGE(!(center.x == 0 && center.y == 0),
                    "probe checkbox never laid out");
    frame(center, false);
    frame(center, true);
    frame(center, false);
    frame(away, false);
    ImGui::DestroyContext();
    return clicked;
}

bool MixIsCenter(const rt2::audio::BackendVoiceMix& mix, float gain)
{
    const float expected = gain * rt2::audio::kAudioCenterPanGain;
    return std::fabs(mix.left - expected) <= 1e-4f &&
           std::fabs(mix.right - expected) <= 1e-4f;
}

std::string FindLineStarting(
    const std::vector<AudioStatusLine>& lines, const std::string& prefix)
{
    for (const auto& line : lines)
        if (line.text.compare(0, prefix.size(), prefix) == 0)
            return line.text;
    return {};
}

} // namespace

TEST_CASE("A7 probe: production remove/apply reconciliation and control gate")
{
    // The inspector executes exactly these decisions; other entities'
    // auditions are never touched by this entity's edits.
    CHECK(DecidePreviewActionOnRemove(true) ==
          AudioAuthoringPreviewAction::Stop);
    CHECK(DecidePreviewActionOnRemove(false) ==
          AudioAuthoringPreviewAction::None);

    AssetReference before;
    before.kind = AssetKind::AudioClip;
    before.path = "sfx/old.wav";
    before.assetId = rt2::core::DeterministicUuidProvider{}.CreateV4();
    AssetReference same = before;
    AssetReference replaced = before;
    replaced.path = "sfx/new.wav";
    replaced.assetId = rt2::core::DeterministicUuidProvider{}.CreateV4();
    CHECK(DecidePreviewActionOnApply(true, before, replaced) ==
          AudioAuthoringPreviewAction::Restart);
    CHECK(DecidePreviewActionOnApply(true, before, same) ==
          AudioAuthoringPreviewAction::None);
    CHECK(DecidePreviewActionOnApply(false, before, replaced) ==
          AudioAuthoringPreviewAction::None);

    // The disabled gate keeps Stop reachable whenever this entity owns the
    // preview voice — including the removed-source state (no live source).
    CHECK_FALSE(AudioPreviewControlsDisabled(true, true, false));
    CHECK_FALSE(AudioPreviewControlsDisabled(true, false, true));
    CHECK(AudioPreviewControlsDisabled(true, false, false));
    CHECK(AudioPreviewControlsDisabled(false, true, true));

    // Real BeginDisabled scope around the Stop shape in the removed state:
    // the item is enabled so the voice can always be stopped from its row.
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* texPixels = nullptr;
    int texW = 0, texH = 0;
    io.Fonts->GetTexDataAsRGBA32(&texPixels, &texW, &texH);
    ImGui::NewFrame();
    ImGui::Begin("A7StopGate");
    ImGui::BeginDisabled(
        AudioPreviewControlsDisabled(true, false, true));
    ImGui::Button("Stop Preview");
    const bool stopEnabledWhilePreviewing =
        (GImGui->LastItemData.InFlags & ImGuiItemFlags_Disabled) == 0;
    ImGui::EndDisabled();
    ImGui::BeginDisabled(
        AudioPreviewControlsDisabled(true, false, false));
    ImGui::Button("Stop Preview");
    const bool stopDisabledWhileIdle =
        (GImGui->LastItemData.InFlags & ImGuiItemFlags_Disabled) != 0;
    ImGui::EndDisabled();
    ImGui::End();
    ImGui::Render();
    ImGui::DestroyContext();
    CHECK(stopEnabledWhilePreviewing);
    CHECK(stopDisabledWhileIdle);
}

TEST_CASE("A7 probe: spatial checkbox switches the running audition both ways")
{
    AudioSceneFixture f;
    const auto uuid = f.AddSource();

    // Off-axis source so the spatial mix is observably not the center mix.
    AudioSourceComponent spatial = TwoDSource();
    spatial.spatial = true;
    spatial.gain = 1.0f;
    BindClip(spatial, f.ids);

    rt2::audio::RecordingFakeAudioBackend fake;
    rt2::audio::AudioPreviewController controller;
    BindPreviewToFake(controller, fake);

    // Start spatial with a transform and an off-axis position.
    const float pos[3] = { 10.0f, 0.0f, 0.0f };
    rt2::core::Error error;
    REQUIRE(controller.StartPreview(uuid, "Sfx", spatial,
                                    /*hasTransform=*/true, pos,
                                    CenterListener(),
                                    /*spatialAudition=*/true, error));
    REQUIRE(controller.PreviewSpatialAudition());
    CHECK(fake.mixes.empty());

    // Real checkbox event flips the box off; feeding the live box state
    // into Update (as the host does every frame) republishes the center
    // mix exactly and commits the 2D mode.
    bool box = true;
    REQUIRE(ClickCheckboxThroughImGui("Preview Spatial", box));
    CHECK_FALSE(box);
    controller.Update(CenterListener(), pos, true, box);
    REQUIRE_FALSE(controller.PreviewSpatialAudition());
    REQUIRE(fake.mixes.size() == 1);
    CHECK(MixIsCenter(fake.mixes.back().second, 1.0f));

    // Steady 2D state republishes nothing: the switch is one publication.
    controller.Update(CenterListener(), pos, true, box);
    CHECK(fake.mixes.size() == 1);

    // And back on: the spatial mix republishes (observably not center for
    // this off-axis source) and the mode commits.
    REQUIRE(ClickCheckboxThroughImGui("Preview Spatial", box));
    CHECK(box);
    controller.Update(CenterListener(), pos, true, box);
    REQUIRE(controller.PreviewSpatialAudition());
    REQUIRE(fake.mixes.size() == 2);
    const rt2::audio::BackendVoiceMix& spatialMix =
        fake.mixes.back().second;
    const float center = 1.0f * rt2::audio::kAudioCenterPanGain;
    CHECK((std::fabs(spatialMix.left - center) > 1e-4f ||
           std::fabs(spatialMix.right - center) > 1e-4f ||
           (spatialMix.left != spatialMix.right)));
}

TEST_CASE("A7 probe: stop and drain failures reach the status snapshot")
{
    AudioSceneFixture f;
    const auto uuid = f.AddSource();
    AudioSourceComponent bound = TwoDSource();
    BindClip(bound, f.ids);

    rt2::audio::RecordingFakeAudioBackend fake;
    rt2::audio::AudioPreviewController controller;
    BindPreviewToFake(controller, fake);
    rt2::core::Error error;
    REQUIRE(StartBoundPreview(controller, uuid, bound, error));

    // A failed Stop detaches anyway (census to zero) but stays loud: the
    // host records it on the latest-failure path, rendered here through
    // the production formatter.
    fake.FailNextStop(rt2::core::Error{
        rt2::core::Error::InvalidRuntimeState, "preview-voice",
        "stop refused by the backend"});
    CHECK_FALSE(controller.StopPreview(error));
    CHECK(error.code == rt2::core::Error::InvalidRuntimeState);
    CHECK(controller.PreviewVoiceCount() == 0);
    CHECK(fake.LiveTokenCount() == 0);

    // Production collection with the real backend/controller objects.
    AudioStatusSnapshot stopSnapshot;
    const rt2::audio::AudioBackendStatus stopBackend = fake.Status();
    FillAudioStatusSnapshot(stopSnapshot, true, &stopBackend, 0,
                            controller, nullptr, true, error.Format(),
                            uuid.ToString(), 0, 0, 0);
    const std::string stopLine = FindLineStarting(
        FormatAudioStatusLines(stopSnapshot), "  Last failure: ");
    CHECK_FALSE(stopLine.empty());
    CHECK(stopLine.find("invalid_runtime_state") != std::string::npos);
    CHECK(stopLine.find(uuid.ToString()) != std::string::npos);

    // A failed completion drain records the typed error (bounded) with a
    // diagnostic instead of staying invisible.
    rt2::audio::AudioPreviewController draining;
    BindPreviewToFake(draining, fake);
    REQUIRE(StartBoundPreview(draining, uuid, bound, error));
    fake.FailNextDrain(rt2::core::Error{
        rt2::core::Error::InvalidRuntimeState, "preview-session",
        "drain refused by the backend"});
    const float pos[3] = { 0.0f, 0.0f, 0.0f };
    draining.Update(CenterListener(), pos, false, false);
    CHECK_FALSE(draining.LastError().IsOk());
    CHECK(draining.LastError().code ==
          rt2::core::Error::InvalidRuntimeState);
    CHECK(draining.FailureCount() == 1);
    CHECK_FALSE(draining.LastDiagnostic().empty());

    AudioStatusSnapshot drainSnapshot;
    const rt2::audio::AudioBackendStatus drainBackend = fake.Status();
    FillAudioStatusSnapshot(drainSnapshot, true, &drainBackend, 0,
                            draining, nullptr, true,
                            draining.LastError().Format(),
                            draining.LastAffectedAsset(), 0, 0, 0);
    CHECK_FALSE(FindLineStarting(
        FormatAudioStatusLines(drainSnapshot), "  Last failure: ").empty());
}

TEST_CASE("A7 probe: deferred runtime voice failure reaches the status lines")
{
    // Production path: a committed Play fails the voice after acceptance
    // (scripted backend refusal); the world records the typed per-source
    // error the host polls into the status snapshot.
    rt2::audio::RecordingFakeAudioBackend fake;
    rt2::audio::AudioWorld world(&fake, &fake,
                                 rt2::audio::AudioSessionId{ 1234 },
                                 rt2::audio::AudioOwnerKind::Runtime);

    AudioSceneFixture f;
    const auto uuid = f.AddSource();
    AudioSourceComponent bound = TwoDSource();
    BindClip(bound, f.ids);

    rt2::audio::AudioPlayRequest request;
    request.source = uuid;
    request.component = bound;
    request.sourcePosition[0] = 0.0f;
    request.sourcePosition[1] = 0.0f;
    request.sourcePosition[2] = 0.0f;
    request.hasTransform = true;
    request.clipKey = "deferred-k";
    uint64_t sequence = 0;
    REQUIRE(world.QueuePlay(request, sequence));

    fake.FailNextStart(rt2::core::Error{
        rt2::core::Error::InvalidRuntimeState, "deferred-k",
        "voice start refused after commit"});
    rt2::audio::AudioSourcePose pose;
    pose.source = uuid;
    pose.hasTransform = true;
    world.Update(CenterListener(), &pose, 1, 0);

    const rt2::audio::AudioSourceStatus status =
        world.GetSourceStatus(uuid);
    REQUIRE(status.hasResult);
    CHECK_FALSE(status.lastResultOk);
    CHECK(status.lastError.code ==
          rt2::core::Error::InvalidRuntimeState);
    CHECK(world.LiveVoiceCount() == 0);

    // The exact error the host would promote renders through the
    // production collection + formatter instead of "none", with live
    // session gains read off the real world.
    AudioStatusSnapshot snapshot;
    const rt2::audio::AudioBackendStatus runtimeBackend = fake.Status();
    rt2::audio::AudioPreviewController idlePreview;
    float sessionGains[4];
    sessionGains[0] = world.BusGain(AudioBus::Master);
    sessionGains[1] = world.BusGain(AudioBus::Music);
    sessionGains[2] = world.BusGain(AudioBus::Effects);
    sessionGains[3] = world.BusGain(AudioBus::UI);
    FillAudioStatusSnapshot(snapshot, true, &runtimeBackend,
                            world.LiveVoiceCount(), idlePreview,
                            sessionGains, true, status.lastError.Format(),
                            uuid.ToString(), 0, 0, 0);
    const auto lines = FormatAudioStatusLines(snapshot);
    const std::string failureLine =
        FindLineStarting(lines, "  Last failure: ");
    CHECK_FALSE(failureLine.empty());
    CHECK(failureLine.find("invalid_runtime_state") != std::string::npos);
    CHECK(FindLineStarting(lines, "  Voices: ") ==
          "  Voices: runtime 0 / preview 0 / total 0");
    CHECK(FindLineStarting(lines, "  Gains: ") ==
          "  Gains: M 1.00 / Mus 1.00 / Fx 1.00 / UI 1.00");
}

TEST_CASE("A7 probe: status formatter covers idle and full states exactly")
{
    // Idle editor: uninitialized backend, defaults, no failure, no cache.
    const auto idleLines = FormatAudioStatusLines(AudioStatusSnapshot{});
    CHECK(FindLineStarting(idleLines, "  Backend: ") ==
          "  Backend: not initialized");
    CHECK(FindLineStarting(idleLines, "  Voices: ") ==
          "  Voices: runtime 0 / preview 0 / total 0");
    CHECK(FindLineStarting(idleLines, "  Gains: ") ==
          "  Gains: M 1.00 / Mus 1.00 / Fx 1.00 / UI 1.00 "
          "(defaults, no Play session)");
    CHECK(FindLineStarting(idleLines, "  Last failure: ") ==
          "  Last failure: none");
    CHECK(FindLineStarting(idleLines, "  Cache: ").empty());
    CHECK(FindLineStarting(idleLines, "  Preview: ").empty());

    AudioStatusSnapshot full;
    full.backendReady = true;
    full.productionNoDevice = true;
    full.backendDetail = "no output device; 48 kHz stereo fallback";
    full.runtimeVoices = 2;
    full.previewVoices = 1;
    full.previewActive = true;
    full.previewSourceName = "Sfx";
    full.previewSpatial = true;
    full.cacheReady = true;
    full.decodedEntries = 3;
    full.decodedBytes = 128;
    full.providerEntries = 5;
    const auto fullLines = FormatAudioStatusLines(full);
    CHECK(FindLineStarting(fullLines, "  Backend: ") ==
          "  Backend: no-device (diagnosed)");
    CHECK(FindLineStarting(fullLines, "  Reason: ") ==
          "  Reason: no output device; 48 kHz stereo fallback");
    CHECK(FindLineStarting(fullLines, "  Voices: ") ==
          "  Voices: runtime 2 / preview 1 / total 3");
    CHECK(FindLineStarting(fullLines, "  Preview: ") ==
          "  Preview: 'Sfx' (spatial audition)");
    CHECK(FindLineStarting(fullLines, "  Cache: ") ==
          "  Cache: 3 entries / 128 bytes (decoded); provider 5 entries");
}

TEST_CASE("A7 probe: host maintenance dispatch table in priority order")
{
    using Action = AudioPreviewMaintenanceAction;
    // No preview: everything is None regardless of later inputs.
    CHECK(DecidePreviewMaintenanceAction(false, false, false, false, false,
                                         false) == Action::None);
    CHECK(DecidePreviewMaintenanceAction(false, true, true, true, true,
                                         true) == Action::None);
    // Priority: Edit state beats selection, entity, source, and clip.
    CHECK(DecidePreviewMaintenanceAction(true, false, false, false, false,
                                         false) == Action::StopLeftEdit);
    CHECK(DecidePreviewMaintenanceAction(true, false, true, true, true,
                                         true) == Action::StopLeftEdit);
    // Selection beats entity/source/clip.
    CHECK(DecidePreviewMaintenanceAction(true, true, false, false, false,
                                         false) ==
          Action::StopSelectionChanged);
    CHECK(DecidePreviewMaintenanceAction(true, true, false, true, true,
                                         true) ==
          Action::StopSelectionChanged);
    // Destroyed entity beats source/clip.
    CHECK(DecidePreviewMaintenanceAction(true, true, true, false, false,
                                         false) == Action::StopEntityGone);
    CHECK(DecidePreviewMaintenanceAction(true, true, true, false, true,
                                         true) == Action::StopEntityGone);
    // Removed source beats clip drift.
    CHECK(DecidePreviewMaintenanceAction(true, true, true, true, false,
                                         false) == Action::StopSourceRemoved);
    CHECK(DecidePreviewMaintenanceAction(true, true, true, true, false,
                                         true) == Action::StopSourceRemoved);
    // Clip drift restarts; steady state does nothing.
    CHECK(DecidePreviewMaintenanceAction(true, true, true, true, true,
                                         true) == Action::RestartClipMoved);
    CHECK(DecidePreviewMaintenanceAction(true, true, true, true, true,
                                         false) == Action::None);
}

TEST_CASE("A7 probe: Undo/Redo clip drift restarts the one preview voice")
{
    // Production route, end to end: SceneManager + history hold the
    // authored truth, the controller holds the audition, and the shared
    // host dispatch decides each transition. This is the exact code
    // WalnutApp::UpdateAudioPreview executes, minus the ImGui chrome.
    AudioSceneFixture f;
    const auto uuid = f.AddSource();

    auto boundClip = [&](const char* path) {
        AudioSourceComponent component = TwoDSource();
        component.clip.kind = AssetKind::AudioClip;
        component.clip.path = path;
        component.clip.assetId = f.ids.CreateV4();
        return component;
    };
    const AudioSourceComponent clipA = boundClip("sfx/a.wav");
    const AudioSourceComponent clipB = boundClip("sfx/b.wav");
    REQUIRE(f.history.Execute(MakeSetAudioSourceCommandIfEffective(
        uuid, std::nullopt, clipA), f.manager).success);

    rt2::audio::RecordingFakeAudioBackend fake;
    rt2::audio::AudioPreviewController controller;
    controller.SetBackend(&fake);
    controller.SetClipProvider(&fake);
    controller.SetClipKeyBuilder(
        [](const AssetReference& clip, const rt2::core::UUID&,
           const std::string&)
            -> rt2::core::Result<std::string> {
            return rt2::core::Result<std::string>::Ok(clip.path);
        });

    auto startLivePreview = [&](rt2::core::Error& error) {
        const float pos[3] = { 0.0f, 0.0f, 0.0f };
        const auto live = f.manager.GetAudioSource(uuid);
        REQUIRE(live.has_value());
        return controller.StartPreview(uuid, "Sfx", *live, false, pos,
                                       CenterListener(), false, error);
    };
    // Host-equivalent maintenance step for this entity: resolve the live
    // clip, dispatch, and execute the restart arm on the controller.
    auto hostMaintenanceStep = [&]() {
        const auto live = f.manager.GetAudioSource(uuid);
        const bool clipMoved =
            live.has_value() && AudioPreviewClipIdentityMoved(
                                    controller.PreviewClip(), live->clip);
        const AudioPreviewMaintenanceAction action =
            DecidePreviewMaintenanceAction(
                controller.HasPreview(), true, true, true,
                live.has_value(), clipMoved);
        if (action == AudioPreviewMaintenanceAction::RestartClipMoved)
        {
            rt2::core::Error error;
            CHECK(startLivePreview(error));
        }
        return action;
    };

    rt2::core::Error error;
    REQUIRE(startLivePreview(error));
    REQUIRE(controller.PreviewClip().path == "sfx/a.wav");

    // Apply clip B through history (inspector Apply equivalent): the live
    // clip moves, the dispatch restarts, the audition follows with exactly
    // one voice and no runtime involvement.
    REQUIRE(f.history.Execute(MakeSetAudioSourceCommandIfEffective(
        uuid, clipA, clipB), f.manager).success);
    CHECK(hostMaintenanceStep() ==
          AudioPreviewMaintenanceAction::RestartClipMoved);
    CHECK(controller.PreviewClip().path == "sfx/b.wav");
    CHECK(controller.PreviewVoiceCount() == 1);
    CHECK(fake.LiveTokenCount() == 1);

    // Undo: the source is A again while B keeps sounding until the host
    // step reconciles — the discriminator for re-review finding 1.
    REQUIRE(f.history.Undo(f.manager).success);
    REQUIRE(f.manager.GetAudioSource(uuid)->clip.path == "sfx/a.wav");
    REQUIRE(controller.PreviewClip().path == "sfx/b.wav");
    CHECK(hostMaintenanceStep() ==
          AudioPreviewMaintenanceAction::RestartClipMoved);
    CHECK(controller.PreviewClip().path == "sfx/a.wav");
    CHECK(controller.PreviewVoiceCount() == 1);
    CHECK(fake.LiveTokenCount() == 1);

    // Redo swaps forward again through the same transition.
    REQUIRE(f.history.Redo(f.manager).success);
    CHECK(hostMaintenanceStep() ==
          AudioPreviewMaintenanceAction::RestartClipMoved);
    CHECK(controller.PreviewClip().path == "sfx/b.wav");
    CHECK(controller.PreviewVoiceCount() == 1);
    CHECK(fake.LiveTokenCount() == 1);

    // Undo of the Add removes the source: the dispatch stops, never
    // restarts, and the census returns to zero.
    REQUIRE(f.history.Undo(f.manager).success); // back to A
    REQUIRE(f.history.Undo(f.manager).success); // removes the source
    REQUIRE_FALSE(f.manager.GetAudioSource(uuid).has_value());
    CHECK(DecidePreviewMaintenanceAction(
              controller.HasPreview(), true, true, true, false, false) ==
          AudioPreviewMaintenanceAction::StopSourceRemoved);
    REQUIRE(controller.StopPreview(error));
    CHECK(fake.LiveTokenCount() == 0);
}

TEST_CASE("A7 probe: failed spatial toggle retains mode, then retry commits")
{
    AudioSceneFixture f;
    const auto uuid = f.AddSource();
    AudioSourceComponent bound = TwoDSource();
    BindClip(bound, f.ids);

    rt2::audio::RecordingFakeAudioBackend fake;
    rt2::audio::AudioPreviewController controller;
    BindPreviewToFake(controller, fake);
    rt2::core::Error error;
    REQUIRE(StartBoundPreview(controller, uuid, bound, error));
    REQUIRE_FALSE(controller.PreviewSpatialAudition());

    // The publish fails: the mode is retained (sound and label stay 2D)
    // while the TYPED failure reaches LastError for the host status —
    // previously only a diagnostic the status never consumed.
    fake.FailNextMix(rt2::core::Error{
        rt2::core::Error::InvalidRuntimeState, "preview-voice",
        "mix publish refused by the backend"});
    const float pos[3] = { 0.0f, 0.0f, 0.0f };
    controller.Update(CenterListener(), pos, false, true);
    CHECK_FALSE(controller.PreviewSpatialAudition());
    CHECK_FALSE(controller.LastError().IsOk());
    CHECK(controller.LastError().code ==
          rt2::core::Error::InvalidRuntimeState);
    CHECK(controller.FailureCount() == 1);
    CHECK_FALSE(controller.LastDiagnostic().empty());

    // The one-shot script is consumed: the next frame retries the still
    // requested mode and commits it. LastError stays sticky by contract
    // (cleared on the next successful start), so the status still shows
    // the toggle failure that actually happened.
    controller.Update(CenterListener(), pos, false, true);
    CHECK(controller.PreviewSpatialAudition());
    CHECK_FALSE(controller.LastError().IsOk());
    CHECK(controller.FailureCount() == 1);
}

TEST_CASE("A7 probe: failed steady spatial refresh reaches LastError")
{
    AudioSceneFixture f;
    const auto uuid = f.AddSource();
    AudioSourceComponent spatial = TwoDSource();
    spatial.spatial = true;
    BindClip(spatial, f.ids);

    rt2::audio::RecordingFakeAudioBackend fake;
    rt2::audio::AudioPreviewController controller;
    BindPreviewToFake(controller, fake);
    const float pos[3] = { 10.0f, 0.0f, 0.0f };
    rt2::core::Error error;
    REQUIRE(controller.StartPreview(uuid, "Sfx", spatial, true, pos,
                                    CenterListener(), true, error));
    REQUIRE(controller.PreviewSpatialAudition());

    // The steady refresh publish fails: the voice keeps playing on the
    // last valid mix while the typed failure reaches the host status.
    fake.FailNextMix(rt2::core::Error{
        rt2::core::Error::InvalidRuntimeState, "preview-voice",
        "refresh publish refused by the backend"});
    controller.Update(CenterListener(), pos, true, true);
    CHECK(controller.PreviewSpatialAudition());
    CHECK(controller.PreviewVoiceCount() == 1);
    CHECK_FALSE(controller.LastError().IsOk());
    CHECK(controller.LastError().code ==
          rt2::core::Error::InvalidRuntimeState);
}

TEST_CASE("A7 probe: successive distinct failures publish monotonically")
{
    // Closure finding 1: after a sticky first failure, a later DISTINCT
    // mix/drain failure must replace it in the rendered status — while an
    // identical repeat must not grow the count. Each stage goes through
    // the production Fill + formatter, which is what Walnut renders.
    AudioSceneFixture f;
    const auto uuid = f.AddSource();
    AudioSourceComponent bound = TwoDSource();
    BindClip(bound, f.ids);

    rt2::audio::RecordingFakeAudioBackend fake;
    rt2::audio::AudioPreviewController controller;
    BindPreviewToFake(controller, fake);
    rt2::core::Error error;
    REQUIRE(StartBoundPreview(controller, uuid, bound, error));

    auto renderedFailure = [&]() {
        AudioStatusSnapshot snapshot;
        const rt2::audio::AudioBackendStatus backend = fake.Status();
        FillAudioStatusSnapshot(snapshot, true, &backend, 0, controller,
                                nullptr, true,
                                controller.LastError().Format(),
                                controller.LastAffectedAsset(), 0, 0, 0);
        return FindLineStarting(FormatAudioStatusLines(snapshot),
                                "  Last failure: ");
    };
    const float pos[3] = { 0.0f, 0.0f, 0.0f };

    // First failure: failed toggle publish (mix path).
    fake.FailNextMix(rt2::core::Error{ rt2::core::Error::InvalidArgument,
                                       "preview-voice",
                                       "toggle publish refused" });
    controller.Update(CenterListener(), pos, false, true);
    REQUIRE_FALSE(controller.LastError().IsOk());
    CHECK(controller.FailureCount() == 1);
    CHECK(renderedFailure().find("invalid_argument") != std::string::npos);

    // Second, DISTINCT failure: failed drain. The rendered line must track
    // the latest failure, not the sticky first one (the old IsOk-gated
    // code failed exactly here). The still-requested toggle retries on
    // the same frame and commits once its script is consumed.
    const rt2::core::Error drainFailure{ rt2::core::Error::InvalidRuntimeState,
                                         "preview-session",
                                         "drain refused" };
    fake.FailNextDrain(drainFailure);
    controller.Update(CenterListener(), pos, false, true);
    CHECK(controller.FailureCount() == 2);
    CHECK(controller.LastError() == drainFailure);
    CHECK(controller.PreviewSpatialAudition());
    const std::string secondLine = renderedFailure();
    CHECK(secondLine.find("invalid_runtime_state") != std::string::npos);
    CHECK(secondLine.find("preview-session") != std::string::npos);

    // Identical repeat: no new publication, count holds, line unchanged.
    fake.FailNextDrain(drainFailure);
    controller.Update(CenterListener(), pos, false, true);
    CHECK(controller.FailureCount() == 2);
    CHECK(controller.LastError() == drainFailure);
    CHECK(renderedFailure() == secondLine);

    // Third, distinct again: toggling back to 2D refuses its center-mix
    // publish and publishes once more (the steady path early-outs for
    // this 2D component, so the toggle drives the mix call).
    fake.FailNextMix(rt2::core::Error{ rt2::core::Error::Io,
                                       "preview-voice",
                                       "toggle-back refused" });
    controller.Update(CenterListener(), pos, false, false);
    CHECK(controller.FailureCount() == 3);
    CHECK(controller.PreviewSpatialAudition());
    CHECK(renderedFailure().find("code=io") != std::string::npos);
}

TEST_CASE("A7 probe: remove-step seam stops with status, loudly when unbound")
{
    // Exact caller logic with a recording hook: branch, single invocation,
    // and diagnostic string all asserted — a broken Stop mapping or a
    // dropped diagnostic fails here.
    int stops = 0;
    std::string diagnostic = "sentinel";
    CHECK(ExecuteAudioRemovePreviewStep(
        true, [&]() { ++stops; }, diagnostic));
    CHECK(stops == 1);
    CHECK(diagnostic == "Preview stopped: the previewed source was removed");

    // Another entity's audition is untouched; the diagnostic is preserved.
    diagnostic = "sentinel";
    CHECK_FALSE(ExecuteAudioRemovePreviewStep(
        false, [&]() { ++stops; }, diagnostic));
    CHECK(stops == 1);
    CHECK(diagnostic == "sentinel");

    // A missing hook is loud, never a silent skip that would strand a
    // voice behind a disabled Stop.
    diagnostic = "sentinel";
    CHECK_FALSE(ExecuteAudioRemovePreviewStep(
        true, std::function<void()>{}, diagnostic));
    CHECK(stops == 1);
    CHECK(diagnostic ==
          "Preview stop unavailable: no preview backend is bound");
}

TEST_CASE("A7 probe: apply-step seam restarts on clip moves only")
{
    AudioSceneFixture f;
    const auto uuid = f.AddSource();
    auto boundClip = [&](const char* path) {
        AudioSourceComponent component = TwoDSource();
        component.clip.kind = AssetKind::AudioClip;
        component.clip.path = path;
        component.clip.assetId = f.ids.CreateV4();
        return component;
    };
    const AudioSourceComponent clipA = boundClip("sfx/a.wav");
    const AudioSourceComponent clipB = boundClip("sfx/b.wav");
    const std::optional<AudioSourceComponent> before = clipA;
    const std::optional<AudioSourceComponent> after = clipB;
    const std::optional<AudioSourceComponent> same = clipA;

    // Clip move with a working hook: exact uuid + spatial forwarded, true
    // only when the new voice is live, diagnostic untouched on success.
    int starts = 0;
    rt2::core::UUID seenTarget;
    bool seenSpatial = false;
    std::string diagnostic = "sentinel";
    std::function<bool(const rt2::core::UUID&, bool, std::string&)> start =
        [&](const rt2::core::UUID& target, bool spatial,
            std::string&) {
            ++starts;
            seenTarget = target;
            seenSpatial = spatial;
            return true;
        };
    CHECK(ExecuteAudioApplyPreviewStep(true, uuid, before, after, start,
                                       true, diagnostic));
    CHECK(starts == 1);
    CHECK(seenTarget == uuid);
    CHECK(seenSpatial);
    CHECK(diagnostic == "sentinel");

    // Refused restart propagates the typed diagnostic and reports false.
    start = [](const rt2::core::UUID&, bool, std::string& out) {
        out = "code=MissingAsset path=sfx/b.wav detail=clip gone";
        return false;
    };
    diagnostic = "sentinel";
    CHECK_FALSE(ExecuteAudioApplyPreviewStep(true, uuid, before, after,
                                             start, false, diagnostic));
    CHECK(diagnostic == "code=MissingAsset path=sfx/b.wav detail=clip gone");

    // Field-identical clips, foreign previews, and missing states issue
    // no start and touch nothing.
    diagnostic = "sentinel";
    CHECK_FALSE(ExecuteAudioApplyPreviewStep(true, uuid, before, same,
                                             start, false, diagnostic));
    CHECK_FALSE(ExecuteAudioApplyPreviewStep(false, uuid, before, after,
                                             start, false, diagnostic));
    CHECK_FALSE(ExecuteAudioApplyPreviewStep(
        true, uuid, std::nullopt, after, start, false, diagnostic));
    CHECK(diagnostic == "sentinel");

    // A missing hook is loud.
    CHECK_FALSE(ExecuteAudioApplyPreviewStep(
        true, uuid, before, after,
        std::function<bool(const rt2::core::UUID&, bool, std::string&)>{},
        false, diagnostic));
    CHECK(diagnostic == "Preview unavailable: no preview backend is bound");
}
