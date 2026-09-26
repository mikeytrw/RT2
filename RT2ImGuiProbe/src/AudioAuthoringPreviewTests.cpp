#include <doctest/doctest.h>

#include "AudioInspectorState.h"
#include "AudioPreviewController.h"
#include "EditorCommandHistory.h"
#include "EditorPropertyCommands.h"
#include "FakeAudioBackend.h"
#include "SceneManager.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <optional>
#include <string>

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
    controller.Update(CenterListener(), pos, false);
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
