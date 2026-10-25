#include <doctest/doctest.h>

#include "EditorTheme.h"
#include "EditorWorkspaceDocking.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <cstring>
#include <filesystem>
#include <string>

// ============================================================================
// Editor workspace theme + docking probe — package-2 native cover.
//
// Real vendored ImGui 1.87, headless, no backend: the production theme and
// DockBuilder adapter execute exactly as the host calls them (theme in
// OnAttach, builder in the pre-dock hook before DockSpace submission).
// Dock-structure assertions use internal API probe-side only; production
// never reads dock internals and never assigns SelectedTabId.
// ============================================================================

namespace fs = std::filesystem;

namespace {

struct DockProbeContext
{
    DockProbeContext()
    {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.DisplaySize = ImVec2(1280, 720);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    }
    ~DockProbeContext() { ImGui::DestroyContext(); }

    static constexpr const char* kNames[10] = {
        "Viewport", "Scene Browser###Outliner", "Inspector", "Content Browser", "Session",
        "Performance", "Scene", "Camera", "Render Settings", "Input Bindings",
    };

    // One production-ordered frame: hook (optional builder) before DockSpace
    // submission, then every panel submits, like RT2's OnUIRender.
    void Frame(ImGuiID dockId, bool build)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("DockSpace Demo", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize);
        if (build)
            rt2::BuildDefaultDockLayout(dockId, ImGui::GetContentRegionAvail());
        ImGui::DockSpace(dockId);
        for (const char* name : kNames)
        {
            ImGui::Begin(name);
            ImGui::Text("panel");
            ImGui::End();
        }
        ImGui::End();
        ImGui::Render();
    }

    static ImGuiWindow* Find(const char* name)
    {
        return ImGui::FindWindowByName(name);
    }
};

} // anonymous namespace

TEST_CASE("WorkspaceDockingProbe: invalid avail builds nothing")
{
    DockProbeContext probe;
    CHECK_FALSE(rt2::BuildDefaultDockLayout(1234, ImVec2(0, 0)));
    CHECK_FALSE(rt2::BuildDefaultDockLayout(1234, ImVec2(-10, 720)));
    CHECK_FALSE(rt2::BuildDefaultDockLayout(0, ImVec2(1280, 720)));
    CHECK(ImGui::FindWindowByName("Viewport") == nullptr);
}

TEST_CASE("WorkspaceDockingProbe: default build docks every identity by role")
{
    DockProbeContext probe;
    constexpr ImGuiID dockId = 0xD0C117u; // fixed probe id (production reads it in-window)
    probe.Frame(dockId, true); // hook build, first submission
    probe.Frame(dockId, false); // settle: docked windows land in nodes

    ImGuiWindow* viewport = DockProbeContext::Find("Viewport");
    ImGuiWindow* outliner = DockProbeContext::Find("Scene Browser###Outliner");
    ImGuiWindow* inspector = DockProbeContext::Find("Inspector");
    ImGuiWindow* browser = DockProbeContext::Find("Content Browser");
    ImGuiWindow* session = DockProbeContext::Find("Session");
    ImGuiWindow* performance = DockProbeContext::Find("Performance");
    REQUIRE(viewport != nullptr);
    REQUIRE(outliner != nullptr);
    REQUIRE(inspector != nullptr);
    REQUIRE(browser != nullptr);

    // Every window docked somewhere (no floating leftovers).
    CHECK(viewport->DockId != 0);
    CHECK(outliner->DockId != 0);
    CHECK(inspector->DockId != 0);
    CHECK(browser->DockId != 0);
    // Role separation: center, left, right, and bottom are distinct nodes.
    CHECK(viewport->DockNode != outliner->DockNode);
    CHECK(viewport->DockNode != inspector->DockNode);
    CHECK(viewport->DockNode != browser->DockNode);
    CHECK(outliner->DockNode != inspector->DockNode);
    // Bottom tab group shares one node.
    CHECK(browser->DockNode == session->DockNode);
    CHECK(browser->DockNode == performance->DockNode);
    // Geometry follows fractions: center is widest, outliner narrowest.
    CHECK(viewport->Size.x > inspector->Size.x);
    CHECK(viewport->Size.x > outliner->Size.x);
    CHECK(inspector->Pos.x > viewport->Pos.x); // right side
    CHECK(outliner->Pos.x < viewport->Pos.x); // left side
    CHECK(browser->Pos.y > viewport->Pos.y); // bottom area
}

TEST_CASE("WorkspaceDockingProbe: loaded custom layout is untouched without the flag")
{
    DockProbeContext probe;
    const char* saved =
        "[Window][Viewport]\nPos=111,222\nSize=400,300\n";
    ImGui::LoadIniSettingsFromMemory(saved, std::strlen(saved));
    constexpr ImGuiID dockId = 0xD0C117u; // fixed probe id (production reads it in-window)
    probe.Frame(dockId, false);
    probe.Frame(dockId, false);
    ImGuiWindow* viewport = DockProbeContext::Find("Viewport");
    REQUIRE(viewport != nullptr);
    // No builder ran: saved placement survives, nothing docked by us.
    CHECK(viewport->DockId == 0);
    CHECK(viewport->Pos.x == doctest::Approx(111.0f));
    CHECK(viewport->Pos.y == doctest::Approx(222.0f));
}

TEST_CASE("WorkspaceDockingProbe: visible browser takes focus one-shot on build")
{
    DockProbeContext probe;
    constexpr ImGuiID dockId = 0xD0C117u; // fixed probe id (production reads it in-window)
    // Production order: hook builds, then the visible browser consumes the
    // one-shot SetNextWindowFocus at submission.
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("DockSpace Demo", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize);
    REQUIRE(rt2::BuildDefaultDockLayout(dockId, ImGui::GetContentRegionAvail()));
    ImGui::DockSpace(dockId);
    ImGui::Begin("Session");
    ImGui::End();
    ImGui::SetNextWindowFocus();
    ImGui::Begin("Content Browser");
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootWindow);
    ImGui::End();
    ImGui::End();
    ImGui::Render();
    CHECK(focused);
}

TEST_CASE("WorkspaceThemeProbe: Graphite tokens, absolute and idempotent")
{
    DockProbeContext probe;
    rt2::ApplyEditorTheme();
    ImGuiStyle first = ImGui::GetStyle();

    // Graphite surfaces: dark and near-neutral (a slight cool cast only).
    const ImVec4& bg = first.Colors[ImGuiCol_WindowBg];
    CHECK(bg.x < 0.12f);
    CHECK(bg.z - bg.x < 0.03f);
    const ImVec4& text = first.Colors[ImGuiCol_Text];
    CHECK(text.x > 0.85f); // bright readable text
    CHECK(first.Colors[ImGuiCol_TextDisabled].x > 0.5f); // readable labels
    CHECK(first.WindowRounding == doctest::Approx(0.0f));
    // Indigo on focus and selection.
    const ImVec4& nav = first.Colors[ImGuiCol_NavHighlight];
    CHECK(nav.z > nav.x);
    const ImVec4& sel = first.Colors[ImGuiCol_Header];
    CHECK(sel.z > sel.x);
    // The active tab joins its panel; inactive tabs sink into the chrome.
    CHECK(std::memcmp(&first.Colors[ImGuiCol_TabActive], &bg, sizeof(ImVec4)) == 0);
    CHECK(first.Colors[ImGuiCol_Tab].x < bg.x);

    // Idempotent: a second application changes nothing.
    rt2::ApplyEditorTheme();
    const ImGuiStyle& second = ImGui::GetStyle();
    CHECK(std::memcmp(first.Colors, second.Colors, sizeof(first.Colors)) == 0);
    CHECK(first.WindowRounding == doctest::Approx(second.WindowRounding));
    CHECK(first.FramePadding.x == doctest::Approx(second.FramePadding.x));

    // Scaling multiplies the base metrics once and never compounds.
    rt2::ApplyEditorTheme(1.5f);
    const float scaledPadding = ImGui::GetStyle().FramePadding.x;
    CHECK(scaledPadding == doctest::Approx(first.FramePadding.x * 1.5f));
    rt2::ApplyEditorTheme(1.5f);
    CHECK(ImGui::GetStyle().FramePadding.x == doctest::Approx(scaledPadding));
    rt2::ApplyEditorTheme();
}
