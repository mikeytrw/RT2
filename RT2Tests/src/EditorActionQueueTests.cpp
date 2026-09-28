#include <doctest/doctest.h>

#include "EditorActionQueue.h"

#include <string>
#include <vector>

// ============================================================================
// Editor action seam tests — package-3 routing contract.
//
// Availability matrices, stale-state rejection, exactly one callback per
// accepted request, duplicate suppression with order preservation, and
// clear-after-dispatch (no replay). Real engine effects stay in owner
// callbacks; recording lambdas stand in here.
// ============================================================================

using namespace rt2::core;

namespace {

struct Recorder
{
    std::vector<std::string> calls;
    std::string lastChildParent;
    EditorAction lastKind = EditorAction::None;

    EditorActionCallbacks Callbacks()
    {
        EditorActionCallbacks cb;
        cb.onSave = [this]() { calls.push_back("Save"); };
        cb.onUndo = [this]() { calls.push_back("Undo"); };
        cb.onRedo = [this]() { calls.push_back("Redo"); };
        cb.onAddEmpty = [this]() { calls.push_back("AddEmpty"); };
        cb.onAddChild = [this](const std::string& p) {
            calls.push_back("AddChildEmpty");
            lastChildParent = p;
        };
        cb.onAddKind = [this](EditorAction k) {
            calls.push_back(EditorActionName(k));
            lastKind = k;
        };
        cb.onPlay = [this]() { calls.push_back("Play"); };
        cb.onResume = [this]() { calls.push_back("Resume"); };
        cb.onPause = [this]() { calls.push_back("Pause"); };
        cb.onStep = [this]() { calls.push_back("Step"); };
        cb.onStop = [this]() { calls.push_back("Stop"); };
        return cb;
    }
};

EditorActionAvailability AllAvailable()
{
    EditorActionAvailability a;
    a.canSave = a.canUndo = a.canRedo = true;
    a.canAdd = a.canAddChild = true;
    a.addChildParentUuid = "parent-1";
    a.canPlay = a.canResume = a.canPause = a.canStep = a.canStop = true;
    return a;
}

} // anonymous namespace

TEST_CASE("ActionSeam: distinct requests dispatch in order, exactly once each")
{
    EditorActionQueue q;
    q.Push(EditorAction::Save);
    q.Push(EditorAction::Undo);
    q.Push(EditorAction::Play);
    q.Push(EditorAction::AddCube);
    Recorder r;
    const auto report = DispatchEditorActions(q, AllAvailable(), r.Callbacks());
    CHECK(report.accepted == 4);
    CHECK(report.rejectedStale == 0);
    REQUIRE(r.calls.size() == 4);
    CHECK(r.calls[0] == "Save");
    CHECK(r.calls[1] == "Undo");
    CHECK(r.calls[2] == "Play");
    CHECK(r.calls[3] == "AddCube");
    CHECK(q.Empty()); // consumed: next frame cannot replay
}

TEST_CASE("ActionSeam: duplicate activation within a frame collapses")
{
    EditorActionQueue q;
    q.Push(EditorAction::Play);
    q.Push(EditorAction::Play); // shortcut + toolbar same frame
    q.Push(EditorAction::Play);
    q.Push(EditorAction::Undo);
    q.Push(EditorAction::Undo);
    Recorder r;
    const auto report = DispatchEditorActions(q, AllAvailable(), r.Callbacks());
    CHECK(report.accepted == 2);
    CHECK(report.suppressedDuplicates == 3);
    REQUIRE(r.calls.size() == 2);
    CHECK(r.calls[0] == "Play");
    CHECK(r.calls[1] == "Undo");
    // Distinct payloads are distinct requests.
    EditorActionQueue q2;
    q2.Push(EditorAction::AddChildEmpty, "a");
    q2.Push(EditorAction::AddChildEmpty, "b");
    Recorder r2;
    EditorActionAvailability avail = AllAvailable();
    avail.addChildParentUuid = "a";
    const auto report2 = DispatchEditorActions(q2, avail, r2.Callbacks());
    CHECK(report2.accepted == 1); // "b" is stale (not the snapshot parent)
    CHECK(report2.rejectedStale == 1);
    CHECK(r2.lastChildParent == "a");
}

TEST_CASE("ActionSeam: stale requests refuse loudly, callbacks untouched")
{
    EditorActionQueue q;
    q.Push(EditorAction::Play);   // not in Edit
    q.Push(EditorAction::Resume); // not Paused
    q.Push(EditorAction::Step);   // not Paused
    q.Push(EditorAction::Undo);   // empty history
    q.Push(EditorAction::AddEmpty); // not editable
    Recorder r;
    EditorActionAvailability avail; // everything false
    const auto report = DispatchEditorActions(q, avail, r.Callbacks());
    CHECK(report.accepted == 0);
    CHECK(report.rejectedStale == 5);
    CHECK(r.calls.empty());
    CHECK(q.Empty());
}

TEST_CASE("ActionSeam: transport matrix follows run state")
{
    Recorder r;
    // Edit: only Play (+Stop refused, Pause/Step/Resume refused).
    {
        EditorActionQueue q;
        q.Push(EditorAction::Play);
        q.Push(EditorAction::Pause);
        q.Push(EditorAction::Resume);
        q.Push(EditorAction::Step);
        q.Push(EditorAction::Stop);
        EditorActionAvailability avail;
        avail.canPlay = true;
        const auto report = DispatchEditorActions(q, avail, r.Callbacks());
        CHECK(report.accepted == 1);
        CHECK(report.rejectedStale == 4);
        CHECK(r.calls.back() == "Play");
    }
    // Paused: Resume, Step, Stop go; Play and Pause refuse.
    {
        EditorActionQueue q;
        q.Push(EditorAction::Play);
        q.Push(EditorAction::Resume);
        q.Push(EditorAction::Pause);
        q.Push(EditorAction::Step);
        q.Push(EditorAction::Stop);
        EditorActionAvailability avail;
        avail.canResume = avail.canStep = avail.canStop = true;
        const auto report = DispatchEditorActions(q, avail, r.Callbacks());
        CHECK(report.accepted == 3);
        CHECK(report.rejectedStale == 2);
    }
}

TEST_CASE("ActionSeam: AddChild validates the snapshot parent")
{
    Recorder r;
    EditorActionAvailability avail = AllAvailable();
    avail.addChildParentUuid = "parent-9";
    // Current parent: accepted with the captured UUID.
    {
        EditorActionQueue q;
        q.Push(EditorAction::AddChildEmpty, "parent-9");
        const auto report = DispatchEditorActions(q, avail, r.Callbacks());
        CHECK(report.accepted == 1);
        CHECK(r.lastChildParent == "parent-9");
    }
    // Selection moved or lock engaged after emit: refused.
    {
        EditorActionQueue q;
        q.Push(EditorAction::AddChildEmpty, "parent-1");
        const auto report = DispatchEditorActions(q, avail, r.Callbacks());
        CHECK(report.accepted == 0);
        CHECK(report.rejectedStale == 1);
    }
    // Empty payload never dispatches.
    {
        EditorActionQueue q;
        q.Push(EditorAction::AddChildEmpty);
        const auto report = DispatchEditorActions(q, avail, r.Callbacks());
        CHECK(report.accepted == 0);
        CHECK(report.rejectedStale == 1);
    }
}

TEST_CASE("ActionSeam: missing owner callback is a loud wiring error")
{
    EditorActionQueue q;
    q.Push(EditorAction::Save);
    Recorder r;
    EditorActionCallbacks cb = r.Callbacks();
    cb.onSave = nullptr;
    const auto report = DispatchEditorActions(q, AllAvailable(), cb);
    CHECK(report.accepted == 0);
    CHECK(report.rejectedStale == 1);
    CHECK_FALSE(report.error.IsOk());
    CHECK(r.calls.empty());
    CHECK(q.Empty());
}

TEST_CASE("ActionSeam: None is ignored, queue clears even when all stale")
{
    EditorActionQueue q;
    q.Push(EditorAction::None);
    CHECK(q.Empty());
    q.Push(EditorAction::Stop);
    Recorder r;
    const auto report = DispatchEditorActions(q, EditorActionAvailability{}, r.Callbacks());
    CHECK(report.accepted == 0);
    CHECK(q.Empty());
}

TEST_CASE("ActionSeam: names cover every action for diagnostics")
{
    CHECK(std::string(EditorActionName(EditorAction::Save)) == "Save");
    CHECK(std::string(EditorActionName(EditorAction::AddChildEmpty)) == "AddChildEmpty");
    CHECK(std::string(EditorActionName(EditorAction::Resume)) == "Resume");
    CHECK(std::string(EditorActionName(EditorAction::None)) == "None");
}
