#include <doctest/doctest.h>

#include "EditorToolbar.h"
#include "EditorActionQueue.h"

#include "imgui.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

// ============================================================================
// Editor toolbar probe — package-3 route cover.
//
// Real vendored ImGui 1.87 widgets, headless: the production toolbar and
// shared Add contents render, recorded rects drive synthesized mouse clicks,
// and the production CPU dispatcher routes the queue to recording owner
// callbacks. Widget code is identical with null hooks (production) and
// recording hooks (here).
// ============================================================================

using namespace rt2;
using namespace rt2::core;

namespace {

struct RecordedItem
{
    bool enabled = false;
    ImVec2 min{};
    ImVec2 max{};
    ImVec2 Center() const
    {
        return ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    }
};

struct ToolbarProbe
{
    EditorActionQueue queue;
    std::map<std::string, RecordedItem> items;
    EditorToolbarProbeHooks hooks;
    ImVec2 windowSize = ImVec2(1280, 200);

    ToolbarProbe()
    {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1280, 720);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        hooks.onItem = [this](const char* label, bool enabled,
                             const ImVec2& min, const ImVec2& max) {
            items[label] = RecordedItem{ enabled, min, max };
        };
    }
    ~ToolbarProbe() { ImGui::DestroyContext(); }

    static EditorToolbarSnapshot EditSnapshot()
    {
        EditorToolbarSnapshot snap;
        snap.availability.canSave = true;
        snap.availability.canUndo = true;
        snap.availability.canRedo = true;
        snap.availability.canAdd = true;
        snap.availability.canAddChild = true;
        snap.availability.addChildParentUuid = "probe-parent";
        snap.availability.canPlay = true;
        snap.runState = SceneRunState::Edit;
        snap.documentLabel = "(untitled)";
        return snap;
    }

    void Frame(const EditorToolbarSnapshot& snap, ImVec2 mouse, bool down)
    {
        items.clear();
        ImGuiIO& io = ImGui::GetIO();
        io.MousePos = mouse;
        io.MouseDown[0] = down;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(windowSize);
        ImGui::Begin("Bar", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize);
        RenderEditorToolbar(snap, queue, &hooks);
        ImGui::End();
        ImGui::Render();
    }

    void Click(const EditorToolbarSnapshot& snap, const ImVec2& at)
    {
        const ImVec2 away(-10, -10);
        Frame(snap, at, false);
        Frame(snap, at, true);
        Frame(snap, at, false);
        Frame(snap, away, false);
    }
};

struct CallbackCounter
{
    std::map<std::string, int> counts;
    EditorActionCallbacks Callbacks()
    {
        EditorActionCallbacks cb;
        cb.onSave = [this]() { ++counts["Save"]; };
        cb.onUndo = [this]() { ++counts["Undo"]; };
        cb.onRedo = [this]() { ++counts["Redo"]; };
        cb.onAddEmpty = [this]() { ++counts["AddEmpty"]; };
        cb.onAddChild = [this](const std::string&) { ++counts["AddChildEmpty"]; };
        cb.onAddKind = [this](EditorAction k) { ++counts[EditorActionName(k)]; };
        cb.onPlay = [this]() { ++counts["Play"]; };
        cb.onResume = [this]() { ++counts["Resume"]; };
        cb.onPause = [this]() { ++counts["Pause"]; };
        cb.onStep = [this]() { ++counts["Step"]; };
        cb.onStop = [this]() { ++counts["Stop"]; };
        return cb;
    }
};

} // anonymous namespace

TEST_CASE("ToolbarProbe: transport click emits Play, disabled Step stays silent")
{
    ToolbarProbe probe;
    const auto snap = ToolbarProbe::EditSnapshot();
    probe.Frame(snap, ImVec2(-10, -10), false);
    REQUIRE(probe.items.count("Play") == 1);
    CHECK(probe.items["Play"].enabled);
    REQUIRE(probe.items.count("Step") == 1);
    CHECK_FALSE(probe.items["Step"].enabled);

    probe.Click(snap, probe.items["Play"].Center());
    REQUIRE(probe.queue.Requests().size() == 1);
    CHECK(probe.queue.Requests()[0].action == EditorAction::Play);

    // Clicking the disabled Step emits nothing.
    probe.Click(snap, probe.items["Step"].Center());
    CHECK(probe.queue.Requests().size() == 1);

    // Production dispatch routes to exactly one owner callback.
    CallbackCounter counter;
    const auto report = DispatchEditorActions(
        probe.queue, snap.availability, counter.Callbacks());
    CHECK(report.accepted == 1);
    CHECK(counter.counts["Play"] == 1);
    CHECK(probe.queue.Empty());
}

TEST_CASE("ToolbarProbe: paused snapshot labels Resume")
{
    ToolbarProbe probe;
    auto snap = ToolbarProbe::EditSnapshot();
    snap.runState = SceneRunState::Paused;
    snap.availability.canPlay = false;
    snap.availability.canResume = true;
    snap.availability.canStep = true;
    probe.Frame(snap, ImVec2(-10, -10), false);
    CHECK(probe.items.count("Resume") == 1);
    CHECK(probe.items.count("Play") == 0);
    CHECK(probe.items["Resume"].enabled);
    probe.Click(snap, probe.items["Resume"].Center());
    REQUIRE(probe.queue.Requests().size() == 1);
    CHECK(probe.queue.Requests()[0].action == EditorAction::Resume);
}

TEST_CASE("ToolbarProbe: narrow width overflows Undo into the popup")
{
    ToolbarProbe probe;
    probe.windowSize = ImVec2(300, 200);
    const auto snap = ToolbarProbe::EditSnapshot();
    probe.Frame(snap, ImVec2(-10, -10), false);
    // Overflow mode: bar keeps Save/transport, Undo lives in the popup.
    CHECK(probe.items.count(">>") == 1);
    CHECK(probe.items.count("Undo") == 0);
    CHECK(probe.items.count("Save") == 1);

    probe.Click(snap, probe.items[">>"].Center());
    // Popup open this frame: Undo recorded from the overflow menu.
    CHECK(probe.items.count("Undo") == 1);
    probe.Click(snap, probe.items["Undo"].Center());
    bool foundUndo = false;
    for (const auto& r : probe.queue.Requests())
        foundUndo = foundUndo || r.action == EditorAction::Undo;
    CHECK(foundUndo);
}

TEST_CASE("ToolbarProbe: shared Add contents emit from the toolbar popup")
{
    ToolbarProbe probe;
    const auto snap = ToolbarProbe::EditSnapshot();
    probe.Frame(snap, ImVec2(-10, -10), false);
    REQUIRE(probe.items.count("Add") == 1);
    probe.Click(snap, probe.items["Add"].Center());
    // Add popup open: shared contents recorded, Child Empty enabled.
    CHECK(probe.items.count("Empty") == 1);
    REQUIRE(probe.items.count("Child Empty") == 1);
    CHECK(probe.items["Child Empty"].enabled);
    probe.Click(snap, probe.items["Empty"].Center());
    bool foundEmpty = false;
    for (const auto& r : probe.queue.Requests())
        foundEmpty = foundEmpty || r.action == EditorAction::AddEmpty;
    CHECK(foundEmpty);
}

TEST_CASE("ToolbarProbe: Child Empty disabled without a parent, dispatches payload")
{
    // Scoped separately: each probe owns one ImGui context; two live
    // contexts would confuse teardown (GImGui/current mismatch).
    {
        ToolbarProbe probe;
        auto snap = ToolbarProbe::EditSnapshot();
        snap.availability.canAddChild = false;
        probe.Frame(snap, ImVec2(-10, -10), false);
        REQUIRE(probe.items.count("Add") == 1);
        probe.Click(snap, probe.items["Add"].Center());
        REQUIRE(probe.items.count("Child Empty") == 1);
        CHECK_FALSE(probe.items["Child Empty"].enabled);
    }

    // With a parent, the payload round-trips through dispatch.
    ToolbarProbe probe2;
    const auto snap2 = ToolbarProbe::EditSnapshot();
    probe2.Frame(snap2, ImVec2(-10, -10), false);
    REQUIRE(probe2.items.count("Add") == 1);
    probe2.Click(snap2, probe2.items["Add"].Center());
    REQUIRE(probe2.items.count("Child Empty") == 1);
    probe2.Click(snap2, probe2.items["Child Empty"].Center());
    std::string payload;
    for (const auto& r : probe2.queue.Requests())
        if (r.action == EditorAction::AddChildEmpty) payload = r.payload;
    CHECK(payload == "probe-parent");
    CallbackCounter counter;
    const auto report = DispatchEditorActions(
        probe2.queue, snap2.availability, counter.Callbacks());
    CHECK(report.accepted == 1);
    CHECK(counter.counts["AddChildEmpty"] == 1);
}
