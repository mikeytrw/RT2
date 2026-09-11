#include <doctest/doctest.h>

#include "EditorCommandHistory.h"
#include "EditorPropertyCommands.h"
#include "PrefabComponentKey.h"
#include "SceneManager.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <cmath>
#include <filesystem>
#include <optional>

// ============================================================================
// F1: scene-camera Combo selection and exposure reset commit through
// discrete whole-camera commands, not the drag-preview session lifecycle.
//
// A Combo popup opening deactivates the widget without an edit, and the
// later selection arrives with no activation; a Button release reports
// plain deactivation. The drag-session wrapper therefore canceled the
// eventual selection (Combo) or published-then-restored it (Reset), leaving
// zero commits. The inspector now routes both gestures through
// ResolveCameraDiscreteCommit + MakeSetCameraPresentationCommandIfEffective
// after closing any prior preview.
//
// These tests pin the repair at two levels: production factory/command
// behavior on real scenes (including prefab markers and exact Undo/Redo),
// and the real ImGui event shapes that discriminated the fault, replayed
// against the vendored ImGui sources through the production resolver.
// ============================================================================

namespace
{
struct SceneFixture
{
    rt2::core::DeterministicUuidProvider ids;
    SceneManager manager;
    EditorCommandHistory history;

    SceneFixture()
    {
        manager.SetUuidProvider(&ids);
        manager.AddMaterial(SceneMaterial{});
    }

    rt2::core::UUID AddCamera(const char* name = "Cam")
    {
        const auto uuid = manager.CreateEmpty(name).affectedEntities.front();
        const entt::entity e = manager.FindEntityByUuid(uuid);
        manager.GetECS().registry.emplace<CameraComponent>(e);
        return uuid;
    }

    CameraComponent GetCamera(const rt2::core::UUID& uuid)
    {
        const entt::entity e = manager.FindEntityByUuid(uuid);
        REQUIRE((e != entt::null));
        return *manager.GetECS().registry.try_get<CameraComponent>(e);
    }
};

bool CameraEqStrict(const CameraComponent& a, const CameraComponent& b)
{
    constexpr float eps = 1e-5f;
    return std::fabs(a.verticalFOV - b.verticalFOV) <= eps &&
           std::fabs(a.aperture - b.aperture) <= eps &&
           std::fabs(a.focusDistance - b.focusDistance) <= eps &&
           a.forwardDirection == b.forwardDirection &&
           a.presentation == b.presentation;
}

std::filesystem::path UniqueTempDir(const std::string& tag)
{
    static int counter = 0;
    auto dir = std::filesystem::temp_directory_path() /
               ("rt2_caminsp_" + tag + "_" + std::to_string(++counter));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

} // namespace

TEST_CASE("F1 discrete Combo selection commits exactly with Undo/Redo")
{
    SceneFixture f;
    const auto cam = f.AddCamera();
    const CameraComponent live = f.GetCamera(cam);
    REQUIRE(live.presentation.toneMap == ToneMapOperator::AgX);

    // Selecting ACES in the Combo builds an effective discrete command.
    auto cmd = MakeSetCameraPresentationCommandIfEffective(
        cam, live, ToneMapOperator::ACESFitted, std::nullopt);
    REQUIRE(cmd);
    auto r = f.history.Execute(std::move(cmd), f.manager);
    REQUIRE(r.success);
    CHECK(r.syncImpact == rt2::core::SyncImpact::None);
    CameraComponent after = f.GetCamera(cam);
    CHECK(after.presentation.toneMap == ToneMapOperator::ACESFitted);
    CHECK(after.presentation.exposureEV == doctest::Approx(live.presentation.exposureEV));
    CHECK(after.verticalFOV == doctest::Approx(live.verticalFOV));

    REQUIRE(f.history.Undo(f.manager).success);
    CHECK(CameraEqStrict(f.GetCamera(cam), live));
    REQUIRE(f.history.Redo(f.manager).success);
    CHECK(f.GetCamera(cam).presentation.toneMap == ToneMapOperator::ACESFitted);

    // Re-selecting the live operator is silent: no history entry.
    const CameraComponent current = f.GetCamera(cam);
    CHECK(!MakeSetCameraPresentationCommandIfEffective(
        cam, current, ToneMapOperator::ACESFitted, std::nullopt));
}

TEST_CASE("F1 reset commits EV zero and is silent when already zero")
{
    SceneFixture f;
    const auto cam = f.AddCamera();
    CameraComponent live = f.GetCamera(cam);
    live.presentation.toneMap = ToneMapOperator::Reinhard;
    live.presentation.exposureEV = 1.5f;
    {
        const entt::entity e = f.manager.FindEntityByUuid(cam);
        f.manager.GetECS().registry.emplace_or_replace<CameraComponent>(e, live);
    }

    auto cmd = MakeSetCameraPresentationCommandIfEffective(
        cam, live, std::nullopt, 0.0f);
    REQUIRE(cmd);
    REQUIRE(f.history.Execute(std::move(cmd), f.manager).success);
    CameraComponent after = f.GetCamera(cam);
    CHECK(after.presentation.exposureEV == doctest::Approx(0.0f));
    CHECK(after.presentation.toneMap == ToneMapOperator::Reinhard);
    REQUIRE(f.history.Undo(f.manager).success);
    CHECK(CameraEqStrict(f.GetCamera(cam), live));
    REQUIRE(f.history.Redo(f.manager).success);

    // Reset at 0 EV: factory silence, no history entry, live untouched.
    const CameraComponent atZero = f.GetCamera(cam);
    REQUIRE(atZero.presentation.exposureEV == doctest::Approx(0.0f));
    CHECK(!MakeSetCameraPresentationCommandIfEffective(
        cam, atZero, std::nullopt, 0.0f));
    CHECK(!MakeSetCameraPresentationCommandIfEffective(
        cam, atZero, std::nullopt, std::nullopt));
}

TEST_CASE("F1 discrete command canonicalizes and fails loudly on invalid input")
{
    SceneFixture f;
    const auto cam = f.AddCamera();
    CameraComponent live = f.GetCamera(cam);
    live.presentation.exposureEV = 1.0f;
    {
        const entt::entity e = f.manager.FindEntityByUuid(cam);
        f.manager.GetECS().registry.emplace_or_replace<CameraComponent>(e, live);
    }

    // -0.0f EV stores as +0.0f bit pattern.
    auto canon = MakeSetCameraPresentationCommandIfEffective(
        cam, live, std::nullopt, -0.0f);
    REQUIRE(canon);
    REQUIRE(f.history.Execute(std::move(canon), f.manager).success);
    uint32_t bits = 0xFFFFFFFFu;
    const float stored = f.GetCamera(cam).presentation.exposureEV;
    std::memcpy(&bits, &stored, sizeof(bits));
    CHECK(bits == 0u);

    // Out-of-range EV is returned (not suppressed) so Execute rejects it
    // loudly without recording or mutating.
    auto bad = MakeSetCameraPresentationCommandIfEffective(
        cam, live, std::nullopt, 20.0f);
    REQUIRE(bad);
    CHECK_FALSE(f.history.Execute(std::move(bad), f.manager).success);
    CHECK(f.GetCamera(cam).presentation.exposureEV == doctest::Approx(0.0f));
}

TEST_CASE("F1 discrete presentation edit marks the whole-camera prefab override")
{
    const auto dir = UniqueTempDir("instance");
    SceneFixture f;
    const auto root = f.manager.CreateEmpty("Root").affectedEntities.front();
    f.manager.CreateEmpty("Child", root);
    const entt::entity rootHandle = f.manager.FindEntityByUuid(root);
    CameraComponent templateCam;
    templateCam.verticalFOV = 60.0f;
    f.manager.GetECS().registry.emplace<CameraComponent>(rootHandle, templateCam);

    const auto prefabPath = dir / "cam.rt2prefab";
    REQUIRE(f.manager.CreatePrefabFromSubtree({ root }, prefabPath).ok);
    const auto uuids = f.manager.ReserveKnownUuids(2);
    std::vector<rt2::core::AssetDiagnostic> diags;
    REQUIRE(f.manager.InstantiatePrefabWithUuids(prefabPath, uuids, diags).mutation.success);

    const rt2::core::UUID member = uuids[0];
    const auto camKey = PrefabComponentKeyFor<CameraComponent>::value;
    REQUIRE_FALSE(f.manager.IsOverridden(member, camKey).value);
    const CameraComponent origin = f.GetCamera(member);

    // A Combo-style discrete selection records one entry and marks the wire.
    auto cmd = MakeSetCameraPresentationCommandIfEffective(
        member, origin, ToneMapOperator::ACESFitted, std::nullopt);
    REQUIRE(cmd);
    REQUIRE(f.history.Execute(std::move(cmd), f.manager).success);
    CHECK(f.GetCamera(member).presentation.toneMap == ToneMapOperator::ACESFitted);
    CHECK(f.manager.IsOverridden(member, camKey).value);

    REQUIRE(f.history.Undo(f.manager).success);
    CHECK(CameraEqStrict(f.GetCamera(member), origin));
    CHECK_FALSE(f.manager.IsOverridden(member, camKey).value);
    std::filesystem::remove_all(dir);
}

namespace
{
// Live ImGui event probe over the vendored sources. Replays the exact mouse
// gesture frames from the F1 fault (Combo popup open/select, Reset click)
// and records what the real widgets report. The removed drag-session wiring
// is modeled inline as the regression oracle: it begins a session on
// activation, publishes only while open, commits on after-edit and restores
// on plain deactivation.
struct WidgetEvents
{
    bool changed = false;
    bool activated = false;
    bool deactivated = false;
    bool afterEdit = false;
};

struct SessionModel
{
    bool open = false;
    int before = 0;
    int live = 0;
    int commits = 0;
};

struct ProbeFrame
{
    WidgetEvents combo;
    WidgetEvents reset;
};

std::vector<ProbeFrame> RunGestureProbe()
{
    std::vector<ProbeFrame> frames;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(800, 600);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);

    ImVec2 comboCenter(0, 0), resetCenter(0, 0);
    auto frame = [&](ImVec2 mouse, bool down) {
        io.MousePos = mouse;
        io.MouseDown[0] = down;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(500, 400));
        ImGui::Begin("Probe", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoResize);
        ProbeFrame f;
        {
            const char* items[] = { "AgX", "ACES Fitted", "Reinhard (Legacy)" };
            int edited = 0;
            f.combo.changed = ImGui::Combo("Tone Mapping", &edited, items, 3);
            const auto lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
            comboCenter = ImVec2((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
            f.combo.activated = ImGui::IsItemActivated();
            f.combo.deactivated = ImGui::IsItemDeactivated();
            f.combo.afterEdit = ImGui::IsItemDeactivatedAfterEdit();
        }
        {
            f.reset.changed = ImGui::Button("Reset Exposure to 0 EV");
            const auto lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
            resetCenter = ImVec2((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
            f.reset.activated = ImGui::IsItemActivated();
            f.reset.deactivated = ImGui::IsItemDeactivated();
            f.reset.afterEdit = ImGui::IsItemDeactivatedAfterEdit();
        }
        ImGui::End();
        ImGui::Render();
        frames.push_back(f);
    };

    const ImVec2 away(-10, -10);
    frame(away, false);
    frame(away, false);
    // Reset click: hover, down, up.
    frame(resetCenter, false);
    frame(resetCenter, true);
    frame(resetCenter, false);
    // Combo open: hover, down, up (popup opens on release).
    frame(comboCenter, false);
    frame(comboCenter, true);
    frame(comboCenter, false);
    frame(away, false);
    // Find the popup and click its second item (ACES Fitted).
    ImGuiWindow* popup = nullptr;
    for (auto* win : GImGui->Windows)
    {
        if (win->Flags & ImGuiWindowFlags_Popup)
        {
            popup = win;
            break;
        }
    }
    REQUIRE(popup != nullptr);
    const ImVec2 choice(popup->Pos.x + 40.0f,
                        popup->Pos.y + ImGui::GetStyle().WindowPadding.y +
                            ImGui::GetTextLineHeightWithSpacing() * 1.5f);
    frame(choice, false);
    frame(choice, true);
    frame(choice, false);
    ImGui::DestroyContext();
    return frames;
}

int OldSessionCommits(const std::vector<ProbeFrame>& frames, bool comboSide)
{
    // The removed wiring, modeled exactly: begin on activation, publish only
    // while open, commit on after-edit, restore on plain deactivation.
    SessionModel s;
    for (const ProbeFrame& f : frames)
    {
        const WidgetEvents& e = comboSide ? f.combo : f.reset;
        if (e.activated)
        {
            s.open = true;
            s.before = s.live;
        }
        if (e.changed && s.open)
            s.live = 1;
        if (e.afterEdit && s.open)
        {
            s.open = false;
            s.commits++;
        }
        else if (e.deactivated && s.open)
        {
            s.live = s.before;
            s.open = false;
        }
    }
    return s.commits;
}

} // namespace

TEST_CASE("F1 probe: real Combo/Reset events commit discretely, old wiring drops them")
{
    const std::vector<ProbeFrame> frames = RunGestureProbe();
    REQUIRE(frames.size() == 12);

    // The selection lands with no activation; the reset click lands with
    // plain deactivation and no after-edit. These shapes are what defeated
    // the activation-gated session.
    bool sawComboSelection = false;
    bool sawResetClick = false;
    for (const ProbeFrame& f : frames)
    {
        if (f.combo.changed)
        {
            sawComboSelection = true;
            CHECK_FALSE(f.combo.activated);
        }
        if (f.reset.changed)
        {
            sawResetClick = true;
            CHECK_FALSE(f.reset.afterEdit);
        }
    }
    REQUIRE(sawComboSelection);
    REQUIRE(sawResetClick);

    // Old wiring reproduces the fault on the same stream: zero commits.
    CHECK(OldSessionCommits(frames, true) == 0);
    CHECK(OldSessionCommits(frames, false) == 0);

    // New production policy commits both gestures discretely.
    CHECK(ResolveCameraDiscreteCommit(true) == CameraDiscreteCommit::Commit);
    CHECK(ResolveCameraDiscreteCommit(false) == CameraDiscreteCommit::None);

    // End to end through production commands: selection commits ACES with
    // exact Undo, reset commits EV zero with exact Undo.
    SceneFixture f;
    const auto cam = f.AddCamera();
    const CameraComponent live = f.GetCamera(cam);

    int commits = 0;
    for (const ProbeFrame& fr : frames)
    {
        if (ResolveCameraDiscreteCommit(fr.combo.changed) == CameraDiscreteCommit::Commit)
        {
            auto cmd = MakeSetCameraPresentationCommandIfEffective(
                cam, f.GetCamera(cam), ToneMapOperator::ACESFitted, std::nullopt);
            REQUIRE(cmd);
            REQUIRE(f.history.Execute(std::move(cmd), f.manager).success);
            commits++;
        }
        if (ResolveCameraDiscreteCommit(fr.reset.changed) == CameraDiscreteCommit::Commit)
        {
            CameraComponent cur = f.GetCamera(cam);
            cur.presentation.exposureEV = 1.5f;
            const entt::entity e = f.manager.FindEntityByUuid(cam);
            f.manager.GetECS().registry.emplace_or_replace<CameraComponent>(e, cur);
            auto cmd = MakeSetCameraPresentationCommandIfEffective(
                cam, f.GetCamera(cam), std::nullopt, 0.0f);
            REQUIRE(cmd);
            REQUIRE(f.history.Execute(std::move(cmd), f.manager).success);
            commits++;
        }
    }
    CHECK(commits == 2);
    CameraComponent finalState = f.GetCamera(cam);
    CHECK(finalState.presentation.toneMap == ToneMapOperator::ACESFitted);
    CHECK(finalState.presentation.exposureEV == doctest::Approx(0.0f));
}
