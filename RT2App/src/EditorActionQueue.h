#pragma once

#ifndef RT2_CORE_EDITOR_ACTION_QUEUE_H
#define RT2_CORE_EDITOR_ACTION_QUEUE_H

#include "core/Error.h"

#include <functional>
#include <string>
#include <vector>

// ============================================================================
// Editor action seam — the single routing contract for relocated editor
// commands (workspace package 3).
//
// Menu, Scene transport, shortcuts, toolbar, and the shared Add contents all
// ENQUEUE stable action requests during the frame; RT2 dispatches the queue
// exactly once per frame, outside every menu/popup drawing stack. Requests
// carry stable payloads (a parent UUID string for AddChildEmpty, nothing
// renderer-specific), never renderer pointers or live scene handles.
//
// Availability is a plain snapshot the owner fills from live state each
// frame; dispatch revalidates every request against it, so a state change
// between enqueue and dispatch refuses loudly instead of misfiring.
// Duplicate (action, payload) pairs within a frame collapse to one; distinct
// requests keep their order. The queue always clears after dispatch, so the
// next frame cannot replay.
//
// Real effects stay in RT2 owner callbacks (existing EnterPlay, save,
// undo/redo, SceneEditorUI create commands, preview/audio admission). This
// file proves routing only.
//
// CPU-only: links into RT2Tests, RT2SliceRunner-class closures, the probe,
// and RT2App. No ImGui/Walnut/Vulkan.
// ============================================================================

namespace rt2 { struct EditorToolbarProbeHooks; } // ImGui side, EditorToolbar.h

namespace rt2::core {

enum class EditorAction
{
    None = 0,
    Save,
    Undo,
    Redo,
    AddEmpty,
    AddChildEmpty, // payload: parent UUID text captured at emit time
    AddLightPoint,
    AddLightSpot,
    AddLightDirectional,
    AddLightEmissiveSphere,
    AddCube,
    AddSphere,
    AddPlane,
    AddImportScene, // blocking chooser runs in the owner callback at dispatch
    AddLoadMesh,    // (outside the popup stack, same UX as today)
    Play,   // from Edit
    Resume, // from Paused
    Pause,
    Step,   // Paused only
    Stop,
};

struct EditorActionRequest
{
    EditorAction action = EditorAction::None;
    std::string payload; // AddChildEmpty parent UUID; empty otherwise
};

// One frame's enablement, mirroring today's per-route enablement exactly:
// Save always clickable; Undo/Redo by history (shortcuts keep their own
// Edit gate at emit time); transport by run state; Add by edit admission
// with the selection snapshot for Child Empty.
struct EditorActionAvailability
{
    bool canSave = false;
    bool canUndo = false;
    bool canRedo = false;
    bool canAdd = false;
    bool canAddChild = false;
    std::string addChildParentUuid;
    bool canPlay = false;
    bool canResume = false;
    bool canPause = false;
    bool canStep = false;
    bool canStop = false;
};

struct EditorActionCallbacks
{
    std::function<void()> onSave;
    std::function<void()> onUndo;
    std::function<void()> onRedo;
    std::function<void()> onAddEmpty;
    std::function<void(const std::string& parentUuid)> onAddChild;
    std::function<void(EditorAction kind)> onAddKind; // lights/primitives/imports
    std::function<void()> onPlay;
    std::function<void()> onResume;
    std::function<void()> onPause;
    std::function<void()> onStep;
    std::function<void()> onStop;
};

struct EditorActionDispatchReport
{
    int accepted = 0;
    int rejectedStale = 0;
    int suppressedDuplicates = 0;
    Error error; // first wiring failure (missing callback), if any
};

class EditorActionQueue
{
public:
    // Enqueue one request. EditorAction::None is ignored. An identical
    // (action, payload) pair already queued collapses (counted at dispatch
    // as suppressed); distinct requests keep emit order.
    void Push(EditorAction action, std::string payload = {});
    const std::vector<EditorActionRequest>& Requests() const { return m_Requests; }
    bool Empty() const { return m_Requests.empty(); }
    void Clear() { m_Requests.clear(); m_SuppressedDuplicates = 0; }
    int SuppressedDuplicates() const { return m_SuppressedDuplicates; }

private:
    std::vector<EditorActionRequest> m_Requests;
    int m_SuppressedDuplicates = 0;
};

// Shared Add-menu context: everything the common Add contents need, without
// any popup ownership. ImGui-free so SceneEditorUI can build it in its
// header; rendering lives in EditorToolbar (ImGui side). Both the Outliner
// popup and the toolbar's popups render from one function over this.
struct SharedAddMenuContext
{
    bool editable = false;
    bool hasParent = false;
    bool parentLocked = false;
    std::string parentUuid;
    EditorActionQueue* queue = nullptr; // non-null when rendering for emit
    // Null in production. The probe passes a recorder to capture widget
    // rects for synthesized clicks (repo seam idiom: recording AND null
    // hooks execute identical widget code).
    const rt2::EditorToolbarProbeHooks* hooks = nullptr;
};

// Drain the queue through the callbacks under the availability snapshot.
// Always clears the queue, accepted or not.
EditorActionDispatchReport DispatchEditorActions(
    EditorActionQueue& queue,
    const EditorActionAvailability& availability,
    const EditorActionCallbacks& callbacks);

const char* EditorActionName(EditorAction action);

} // namespace rt2::core

#endif // RT2_CORE_EDITOR_ACTION_QUEUE_H
