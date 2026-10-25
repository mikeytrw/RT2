#include "EditorActionQueue.h"

namespace rt2::core {

const char* EditorActionName(EditorAction action)
{
    switch (action)
    {
    case EditorAction::Save: return "Save";
    case EditorAction::Undo: return "Undo";
    case EditorAction::Redo: return "Redo";
    case EditorAction::AddEmpty: return "AddEmpty";
    case EditorAction::AddChildEmpty: return "AddChildEmpty";
    case EditorAction::AddLightPoint: return "AddLightPoint";
    case EditorAction::AddLightSpot: return "AddLightSpot";
    case EditorAction::AddLightDirectional: return "AddLightDirectional";
    case EditorAction::AddLightEmissiveSphere: return "AddLightEmissiveSphere";
    case EditorAction::AddCube: return "AddCube";
    case EditorAction::AddSphere: return "AddSphere";
    case EditorAction::AddPlane: return "AddPlane";
    case EditorAction::AddImportScene: return "AddImportScene";
    case EditorAction::AddLoadMesh: return "AddLoadMesh";
    case EditorAction::Play: return "Play";
    case EditorAction::Resume: return "Resume";
    case EditorAction::Pause: return "Pause";
    case EditorAction::Step: return "Step";
    case EditorAction::Stop: return "Stop";
    case EditorAction::None: break;
    }
    return "None";
}

void EditorActionQueue::Push(EditorAction action, std::string payload)
{
    if (action == EditorAction::None) return;
    for (const auto& existing : m_Requests)
    {
        if (existing.action == action && existing.payload == payload)
        {
            ++m_SuppressedDuplicates; // same activation twice: keep the first
            return;
        }
    }
    EditorActionRequest request;
    request.action = action;
    request.payload = std::move(payload);
    m_Requests.push_back(std::move(request));
}

namespace {

bool IsAddKind(EditorAction action)
{
    switch (action)
    {
    case EditorAction::AddLightPoint:
    case EditorAction::AddLightSpot:
    case EditorAction::AddLightDirectional:
    case EditorAction::AddLightEmissiveSphere:
    case EditorAction::AddCube:
    case EditorAction::AddSphere:
    case EditorAction::AddPlane:
    case EditorAction::AddImportScene:
    case EditorAction::AddLoadMesh:
        return true;
    default:
        return false;
    }
}

void NoteWiringFailure(EditorActionDispatchReport& report, EditorAction action)
{
    if (report.error.IsOk())
    {
        report.error.code = Error::InvalidArgument;
        report.error.detail = std::string("editor action has no owner callback: ") +
            EditorActionName(action);
    }
}

} // anonymous namespace

EditorActionDispatchReport DispatchEditorActions(
    EditorActionQueue& queue,
    const EditorActionAvailability& availability,
    const EditorActionCallbacks& callbacks)
{
    EditorActionDispatchReport report;
    report.suppressedDuplicates = queue.SuppressedDuplicates();
    for (const auto& request : queue.Requests())
    {
        const EditorAction action = request.action;
        bool stale = false;
        // Revalidate against the dispatch-time snapshot: a state change
        // between emit and dispatch refuses loudly instead of misfiring.
        switch (action)
        {
        case EditorAction::Save: stale = !availability.canSave; break;
        case EditorAction::Undo: stale = !availability.canUndo; break;
        case EditorAction::Redo: stale = !availability.canRedo; break;
        case EditorAction::AddEmpty: stale = !availability.canAdd; break;
        case EditorAction::AddChildEmpty:
            stale = !availability.canAdd || !availability.canAddChild ||
                request.payload.empty() ||
                request.payload != availability.addChildParentUuid;
            break;
        case EditorAction::Play: stale = !availability.canPlay; break;
        case EditorAction::Resume: stale = !availability.canResume; break;
        case EditorAction::Pause: stale = !availability.canPause; break;
        case EditorAction::Step: stale = !availability.canStep; break;
        case EditorAction::Stop: stale = !availability.canStop; break;
        default:
            if (IsAddKind(action)) stale = !availability.canAdd;
            else stale = true; // None or unknown: never dispatch
            break;
        }
        if (stale)
        {
            ++report.rejectedStale;
            continue;
        }
        bool wired = true;
        switch (action)
        {
        case EditorAction::Save:
            if (callbacks.onSave) callbacks.onSave(); else wired = false;
            break;
        case EditorAction::Undo:
            if (callbacks.onUndo) callbacks.onUndo(); else wired = false;
            break;
        case EditorAction::Redo:
            if (callbacks.onRedo) callbacks.onRedo(); else wired = false;
            break;
        case EditorAction::AddEmpty:
            if (callbacks.onAddEmpty) callbacks.onAddEmpty(); else wired = false;
            break;
        case EditorAction::AddChildEmpty:
            if (callbacks.onAddChild) callbacks.onAddChild(request.payload);
            else wired = false;
            break;
        case EditorAction::Play:
            if (callbacks.onPlay) callbacks.onPlay(); else wired = false;
            break;
        case EditorAction::Resume:
            if (callbacks.onResume) callbacks.onResume(); else wired = false;
            break;
        case EditorAction::Pause:
            if (callbacks.onPause) callbacks.onPause(); else wired = false;
            break;
        case EditorAction::Step:
            if (callbacks.onStep) callbacks.onStep(); else wired = false;
            break;
        case EditorAction::Stop:
            if (callbacks.onStop) callbacks.onStop(); else wired = false;
            break;
        default:
            if (IsAddKind(action))
            {
                if (callbacks.onAddKind) callbacks.onAddKind(action);
                else wired = false;
            }
            else wired = false;
            break;
        }
        if (!wired)
        {
            // A valid request with no owner callback is a wiring bug: loud,
            // counted apart from stale rejections.
            NoteWiringFailure(report, action);
            ++report.rejectedStale;
            continue;
        }
        ++report.accepted;
    }
    queue.Clear(); // consumed requests never replay next frame
    return report;
}

} // namespace rt2::core
