#include "EditorToolbar.h"

#include "EditorIcons.h"
#include "EditorTheme.h"

namespace rt2 {

namespace {

enum class ToolbarTone { Ghost, Primary, Play };

// Pushes the colors for one toolbar button; pair with PopToolbarTone().
void PushToolbarTone(ToolbarTone tone)
{
    using namespace EditorPalette;
    switch (tone)
    {
    case ToolbarTone::Primary:
        ImGui::PushStyleColor(ImGuiCol_Button, Indigo);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.36f, 0.33f, 0.93f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.26f, 0.23f, 0.82f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        break;
    case ToolbarTone::Play:
        ImGui::PushStyleColor(ImGuiCol_Button, Play);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.71f, 0.27f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.86f, 0.57f, 0.12f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.10f, 0.07f, 0.02f, 1.0f));
        break;
    case ToolbarTone::Ghost:
    default:
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Hover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Selection);
        ImGui::PushStyleColor(ImGuiCol_Text, TextSecondary);
        break;
    }
}

void PopToolbarTone() { ImGui::PopStyleColor(4); }

void ToolbarTooltip(const char* text)
{
    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(text);
        ImGui::EndTooltip();
    }
}

void NoteToolbarItem(const EditorToolbarProbeHooks* hooks, const char* label,
    bool enabled)
{
    if (hooks != nullptr && hooks->onItem)
        hooks->onItem(label, enabled,
            ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

// name: the stable item name probes and dispatch counters use.
// label: what is drawn (icon and/or text, with a ## id suffix).
bool ToolbarActionButton(const char* name, const char* label, const char* tooltip,
    bool enabled, core::EditorActionQueue& queue, core::EditorAction action,
    const EditorToolbarProbeHooks* hooks, ToolbarTone tone = ToolbarTone::Ghost,
    const ImVec2& size = ImVec2(0.0f, 0.0f))
{
    PushToolbarTone(tone);
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::Button(label, size);
    ImGui::EndDisabled();
    PopToolbarTone();
    ToolbarTooltip(tooltip);
    NoteToolbarItem(hooks, name, enabled);
    if (pressed && enabled)
        queue.Push(action);
    return pressed && enabled;
}

bool ToolbarActionMenuItem(const char* label, const char* shortcut,
    bool enabled, core::EditorActionQueue& queue, core::EditorAction action,
    const EditorToolbarProbeHooks* hooks, const std::string& payload = {})
{
    const bool pressed =
        ImGui::MenuItem(label, shortcut, false, enabled);
    NoteToolbarItem(hooks, label, enabled);
    if (pressed && enabled)
        queue.Push(action, payload);
    return pressed && enabled;
}

float ToolbarButtonWidth(const char* label)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    return ImGui::CalcTextSize(label, nullptr, true).x + style.FramePadding.x * 2.0f;
}

void ToolbarDivider()
{
    ImGui::SameLine();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight();
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(p.x, p.y + h * 0.2f), ImVec2(p.x, p.y + h * 0.8f),
        ImGui::GetColorU32(EditorPalette::Border));
    ImGui::Dummy(ImVec2(1.0f, h));
    ImGui::SameLine();
}

} // anonymous namespace

void RenderSharedAddMenuContents(const core::SharedAddMenuContext& context,
    const EditorToolbarProbeHooks* hooks)
{
    if (context.queue == nullptr) return;
    const EditorToolbarProbeHooks* itemHooks =
        (hooks != nullptr) ? hooks : context.hooks;
    core::EditorActionQueue& queue = *context.queue;
    const bool editable = context.editable;
    if (ImGui::MenuItem(RT2_ICON_CIRCLE_DOT "  Empty"))
        queue.Push(core::EditorAction::AddEmpty);
    NoteToolbarItem(itemHooks, "Empty", editable);
    const bool childOk =
        editable && context.hasParent && !context.parentLocked;
    ImGui::BeginDisabled(!childOk);
    if (ImGui::MenuItem(RT2_ICON_LIST_TREE "  Child Empty"))
        queue.Push(core::EditorAction::AddChildEmpty, context.parentUuid);
    ImGui::EndDisabled();
    NoteToolbarItem(itemHooks, "Child Empty", childOk);
    ImGui::Separator();
    if (ImGui::BeginMenu(RT2_ICON_SUN "  Light"))
    {
        if (ImGui::MenuItem("Point"))
            queue.Push(core::EditorAction::AddLightPoint);
        NoteToolbarItem(itemHooks, "Point", editable);
        if (ImGui::MenuItem("Spot"))
            queue.Push(core::EditorAction::AddLightSpot);
        NoteToolbarItem(itemHooks, "Spot", editable);
        if (ImGui::MenuItem("Directional"))
            queue.Push(core::EditorAction::AddLightDirectional);
        NoteToolbarItem(itemHooks, "Directional", editable);
        ImGui::Separator();
        if (ImGui::MenuItem("Emissive Sphere"))
            queue.Push(core::EditorAction::AddLightEmissiveSphere);
        NoteToolbarItem(itemHooks, "Emissive Sphere", editable);
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem(RT2_ICON_BOX "  Cube"))
        queue.Push(core::EditorAction::AddCube);
    NoteToolbarItem(itemHooks, "Cube", editable);
    if (ImGui::MenuItem(RT2_ICON_CIRCLE "  Sphere"))
        queue.Push(core::EditorAction::AddSphere);
    NoteToolbarItem(itemHooks, "Sphere", editable);
    if (ImGui::MenuItem(RT2_ICON_SQUARE "  Plane"))
        queue.Push(core::EditorAction::AddPlane);
    NoteToolbarItem(itemHooks, "Plane", editable);
    ImGui::Separator();
    if (ImGui::MenuItem(RT2_ICON_LAYERS "  Import Scene..."))
        queue.Push(core::EditorAction::AddImportScene);
    NoteToolbarItem(itemHooks, "Import Scene...", editable);
    if (ImGui::MenuItem(RT2_ICON_IMPORT "  Load Mesh File..."))
        queue.Push(core::EditorAction::AddLoadMesh);
    NoteToolbarItem(itemHooks, "Load Mesh File...", editable);
}

void RenderEditorToolbar(
    const EditorToolbarSnapshot& snapshot, core::EditorActionQueue& queue,
    const EditorToolbarProbeHooks* hooks)
{
    using namespace EditorPalette;
    const auto& avail = snapshot.availability;
    const bool paused =
        snapshot.runState == core::SceneRunState::Paused;
    const bool playing =
        snapshot.runState == core::SceneRunState::Playing;
    const bool running = playing || paused;
    const char* playName = paused ? "Resume" : "Play";
    const char* playLabel = paused ? RT2_ICON_PLAY "  Resume##Play" : RT2_ICON_PLAY "  Play##Play";

    // Strip geometry: a chrome band the full host width, buttons slightly
    // taller than ordinary frames, a hairline underneath. Play mode tints
    // the band amber so the state is unmistakable from anywhere.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
    const ImGuiStyle& style = ImGui::GetStyle();
    const float frameH = ImGui::GetFrameHeight();
    const float padY = 6.0f;
    const float padX = 10.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float availWidth = ImGui::GetContentRegionAvail().x;
    const ImVec2 bandMax(origin.x + availWidth, origin.y + frameH + padY * 2.0f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, bandMax, ImGui::GetColorU32(
        running ? ImVec4(0.082f, 0.071f, 0.047f, 1.0f) : Chrome));
    draw->AddLine(ImVec2(origin.x, bandMax.y - 1.0f), ImVec2(bandMax.x, bandMax.y - 1.0f),
        ImGui::GetColorU32(running ? ImVec4(0.23f, 0.16f, 0.06f, 1.0f) : Border));
    ImGui::SetCursorScreenPos(ImVec2(origin.x + padX, origin.y + padY));

    const ImVec2 square(frameH, frameH);
    const float spacing = style.ItemSpacing.x;
    const float transportWidth =
        ToolbarButtonWidth(playLabel) + spacing + frameH * 3.0f + spacing * 3.0f;
    // Measured overflow: Save + transport stay; Undo/Redo/Add collapse.
    const float fullWidth =
        padX * 2.0f + frameH * 3.0f + spacing * 3.0f + 9.0f +
        ToolbarButtonWidth(RT2_ICON_PLUS "  Add  " RT2_ICON_CHEVRON_DOWN) + spacing +
        transportWidth + ImGui::CalcTextSize("MMMMMMMMMMMM").x;
    const bool overflow = fullWidth > availWidth;

    ToolbarActionButton("Save", RT2_ICON_SAVE "##Save", "Save scene (Ctrl+S)",
        avail.canSave, queue, core::EditorAction::Save, hooks,
        ToolbarTone::Ghost, square);
    ImGui::SameLine();
    if (!overflow)
    {
        ToolbarActionButton("Undo", RT2_ICON_UNDO_2 "##Undo", "Undo (Ctrl+Z)",
            avail.canUndo, queue, core::EditorAction::Undo, hooks,
            ToolbarTone::Ghost, square);
        ImGui::SameLine();
        ToolbarActionButton("Redo", RT2_ICON_REDO_2 "##Redo",
            "Redo (Ctrl+Shift+Z / Ctrl+Y)", avail.canRedo, queue,
            core::EditorAction::Redo, hooks, ToolbarTone::Ghost, square);
        ToolbarDivider();
        PushToolbarTone(ToolbarTone::Ghost);
        ImGui::BeginDisabled(!avail.canAdd);
        if (ImGui::Button(RT2_ICON_PLUS "  Add  " RT2_ICON_CHEVRON_DOWN "##Add"))
            ImGui::OpenPopup("ToolbarAddEntity");
        ImGui::EndDisabled();
        PopToolbarTone();
        ToolbarTooltip("Add entity or import asset");
        NoteToolbarItem(hooks, "Add", avail.canAdd);
        // Own popup scope: no PushID/popup wrapping around this pair
        // (glossary popup rule).
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 6.0f));
        if (ImGui::BeginPopup("ToolbarAddEntity"))
        {
            core::SharedAddMenuContext ctx;
            ctx.editable = avail.canAdd;
            ctx.hasParent = avail.canAddChild;
            ctx.parentLocked = false;
            ctx.parentUuid = avail.addChildParentUuid;
            ctx.queue = &queue;
            ImGui::BeginDisabled(!avail.canAdd);
            RenderSharedAddMenuContents(ctx, hooks);
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
        ImGui::SameLine();
    }

    // Transport, centered on the band when there is room.
    const float centeredX = origin.x + (availWidth - transportWidth) * 0.5f;
    if (centeredX > ImGui::GetCursorScreenPos().x)
        ImGui::SetCursorScreenPos(ImVec2(centeredX, origin.y + padY));
    if (running)
    {
        // While running, Play is the resume affordance and Stop is the
        // amber primary: the way back to authoring.
        ToolbarActionButton(playName, playLabel,
            paused ? "Resume from Paused" : "Playing",
            avail.canPlay || avail.canResume, queue,
            paused ? core::EditorAction::Resume : core::EditorAction::Play,
            hooks, ToolbarTone::Ghost);
    }
    else
    {
        ToolbarActionButton(playName, playLabel, "Play from Edit (runs a copy of the scene)",
            avail.canPlay || avail.canResume, queue, core::EditorAction::Play,
            hooks, ToolbarTone::Primary);
    }
    ImGui::SameLine();
    ToolbarActionButton("Pause", RT2_ICON_PAUSE "##Pause", "Pause", avail.canPause,
        queue, core::EditorAction::Pause, hooks, ToolbarTone::Ghost, square);
    ImGui::SameLine();
    ToolbarActionButton("Step", RT2_ICON_STEP_FORWARD "##Step", "Step one frame (Paused only)",
        avail.canStep, queue, core::EditorAction::Step, hooks, ToolbarTone::Ghost, square);
    ImGui::SameLine();
    ToolbarActionButton("Stop", RT2_ICON_SQUARE "##Stop",
        "Stop (restores the authored scene)", avail.canStop, queue,
        core::EditorAction::Stop, hooks,
        running ? ToolbarTone::Play : ToolbarTone::Ghost, square);
    if (overflow)
    {
        ImGui::SameLine();
        PushToolbarTone(ToolbarTone::Ghost);
        if (ImGui::Button(RT2_ICON_ELLIPSIS "##Overflow", square))
            ImGui::OpenPopup("ToolbarOverflow");
        PopToolbarTone();
        ToolbarTooltip("More tools");
        NoteToolbarItem(hooks, ">>", true);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 6.0f));
        const bool overflowOpen = ImGui::BeginPopup("ToolbarOverflow");
        ImGui::PopStyleVar();
        if (overflowOpen)
        {
            ToolbarActionMenuItem("Undo", "Ctrl+Z", avail.canUndo, queue,
                core::EditorAction::Undo, hooks);
            ToolbarActionMenuItem("Redo", "Ctrl+Shift+Z", avail.canRedo, queue,
                core::EditorAction::Redo, hooks);
            ImGui::Separator();
            core::SharedAddMenuContext ctx;
            ctx.editable = avail.canAdd;
            ctx.hasParent = avail.canAddChild;
            ctx.parentLocked = false;
            ctx.parentUuid = avail.addChildParentUuid;
            ctx.queue = &queue;
            ImGui::BeginDisabled(!avail.canAdd);
            RenderSharedAddMenuContents(ctx, hooks);
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }
    }
    // Read-only document/runtime indicator, right-aligned (Session panel
    // stays authoritative). The dot is green when saved, indigo when dirty.
    const char* stateText =
        playing ? "PLAYING" : (paused ? "PAUSED" : nullptr);
    const ImVec2 docSize = ImGui::CalcTextSize(snapshot.documentLabel.c_str());
    const float stateW = stateText ? ImGui::CalcTextSize(stateText).x + 18.0f : 0.0f;
    const float dotR = 3.5f;
    const float rightW = stateW + docSize.x + dotR * 2.0f + 10.0f;
    const float rightX = bandMax.x - padX - rightW;
    ImGui::SameLine();
    if (rightX > ImGui::GetCursorScreenPos().x + 12.0f)
    {
        const float textY = origin.y + padY + (frameH - docSize.y) * 0.5f;
        float x = rightX;
        if (stateText)
        {
            draw->AddCircleFilled(ImVec2(x + 4.0f, textY + docSize.y * 0.5f), dotR,
                ImGui::GetColorU32(Play));
            draw->AddText(ImVec2(x + 12.0f, textY), ImGui::GetColorU32(Play), stateText);
            x += stateW;
        }
        draw->AddText(ImVec2(x, textY), ImGui::GetColorU32(TextSecondary),
            snapshot.documentLabel.c_str());
        draw->AddCircleFilled(ImVec2(x + docSize.x + 10.0f, textY + docSize.y * 0.5f), dotR,
            ImGui::GetColorU32(snapshot.documentDirty ? IndigoLine : Success));
    }
    ImGui::NewLine();
    ImGui::PopStyleVar(3);
    // Leave the cursor below the band so the DockSpace starts under it.
    ImGui::SetCursorScreenPos(ImVec2(origin.x, bandMax.y));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

} // namespace rt2
