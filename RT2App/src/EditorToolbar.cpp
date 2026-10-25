#include "EditorToolbar.h"

namespace rt2 {

namespace {

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

bool ToolbarActionButton(const char* label, const char* tooltip, bool enabled,
    core::EditorActionQueue& queue, core::EditorAction action,
    const EditorToolbarProbeHooks* hooks, const std::string& payload = {})
{
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::Button(label);
    ImGui::EndDisabled();
    ToolbarTooltip(tooltip);
    NoteToolbarItem(hooks, label, enabled);
    if (pressed && enabled)
        queue.Push(action, payload);
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
    return ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f;
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
    if (ImGui::MenuItem("Empty"))
        queue.Push(core::EditorAction::AddEmpty);
    NoteToolbarItem(itemHooks, "Empty", editable);
    const bool childOk =
        editable && context.hasParent && !context.parentLocked;
    ImGui::BeginDisabled(!childOk);
    if (ImGui::MenuItem("Child Empty"))
        queue.Push(core::EditorAction::AddChildEmpty, context.parentUuid);
    ImGui::EndDisabled();
    NoteToolbarItem(itemHooks, "Child Empty", childOk);
    ImGui::Separator();
    if (ImGui::BeginMenu("Light"))
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
    if (ImGui::MenuItem("Cube"))
        queue.Push(core::EditorAction::AddCube);
    NoteToolbarItem(itemHooks, "Cube", editable);
    if (ImGui::MenuItem("Sphere"))
        queue.Push(core::EditorAction::AddSphere);
    NoteToolbarItem(itemHooks, "Sphere", editable);
    if (ImGui::MenuItem("Plane"))
        queue.Push(core::EditorAction::AddPlane);
    NoteToolbarItem(itemHooks, "Plane", editable);
    ImGui::Separator();
    if (ImGui::MenuItem("Import Scene..."))
        queue.Push(core::EditorAction::AddImportScene);
    NoteToolbarItem(itemHooks, "Import Scene...", editable);
    if (ImGui::MenuItem("Load Mesh File..."))
        queue.Push(core::EditorAction::AddLoadMesh);
    NoteToolbarItem(itemHooks, "Load Mesh File...", editable);
}

void RenderEditorToolbar(
    const EditorToolbarSnapshot& snapshot, core::EditorActionQueue& queue,
    const EditorToolbarProbeHooks* hooks)
{
    const auto& avail = snapshot.availability;
    const bool paused =
        snapshot.runState == core::SceneRunState::Paused;
    const bool playing =
        snapshot.runState == core::SceneRunState::Playing;
    const char* playLabel = paused ? "Resume" : "Play";

    // Measured overflow: Save + transport stay; Undo/Redo/Add collapse.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float spacing = style.ItemSpacing.x;
    const float fullWidth =
        ToolbarButtonWidth("Save") + spacing +
        ToolbarButtonWidth("Undo") + spacing +
        ToolbarButtonWidth("Redo") + spacing +
        ToolbarButtonWidth("Add") + spacing +
        ToolbarButtonWidth(playLabel) + spacing +
        ToolbarButtonWidth("Pause") + spacing +
        ToolbarButtonWidth("Step") + spacing +
        ToolbarButtonWidth("Stop") + spacing +
        ImGui::CalcTextSize("MM").x; // state/dot slack
    const float availWidth = ImGui::GetContentRegionAvail().x;
    const bool overflow = fullWidth > availWidth;

    ToolbarActionButton("Save", "Save scene", avail.canSave, queue,
        core::EditorAction::Save, hooks);
    ImGui::SameLine();
    if (!overflow)
    {
        ToolbarActionButton("Undo", "Undo (Ctrl+Z)", avail.canUndo, queue,
            core::EditorAction::Undo, hooks);
        ImGui::SameLine();
        ToolbarActionButton("Redo", "Redo (Ctrl+Shift+Z / Ctrl+Y)", avail.canRedo,
            queue, core::EditorAction::Redo, hooks);
        ImGui::SameLine();
        ImGui::BeginDisabled(!avail.canAdd);
        if (ImGui::Button("Add"))
            ImGui::OpenPopup("ToolbarAddEntity");
        ImGui::EndDisabled();
        ToolbarTooltip("Add entity or import asset");
        NoteToolbarItem(hooks, "Add", avail.canAdd);
        // Own popup scope: no PushID/popup wrapping around this pair
        // (glossary popup rule).
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
        ImGui::SameLine();
    }
    ToolbarActionButton(playLabel,
        paused ? "Resume from Paused" : "Play from Edit",
        avail.canPlay || avail.canResume, queue,
        paused ? core::EditorAction::Resume : core::EditorAction::Play,
        hooks);
    ImGui::SameLine();
    ToolbarActionButton("Pause", "Pause", avail.canPause, queue,
        core::EditorAction::Pause, hooks);
    ImGui::SameLine();
    ToolbarActionButton("Step", "Step (Paused only)", avail.canStep, queue,
        core::EditorAction::Step, hooks);
    ImGui::SameLine();
    ToolbarActionButton("Stop", "Stop (restore authoring)", avail.canStop, queue,
        core::EditorAction::Stop, hooks);
    if (overflow)
    {
        ImGui::SameLine();
        if (ImGui::Button(">>"))
            ImGui::OpenPopup("ToolbarOverflow");
        ToolbarTooltip("More tools");
        NoteToolbarItem(hooks, ">>", true);
        if (ImGui::BeginPopup("ToolbarOverflow"))
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
    // Read-only document/runtime indicator (Session panel stays authoritative).
    ImGui::SameLine();
    const char* stateText =
        playing ? "Playing" : (paused ? "Paused" : "Edit");
    ImGui::TextDisabled("(%s)%s %s", stateText,
        snapshot.documentDirty ? "*" : "", snapshot.documentLabel.c_str());
    ImGui::Separator();
}

} // namespace rt2
