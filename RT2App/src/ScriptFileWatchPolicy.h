#pragma once

#ifndef RT2_CORE_SCRIPT_FILE_WATCH_POLICY_H
#define RT2_CORE_SCRIPT_FILE_WATCH_POLICY_H

#include "SceneRunState.h"

#include <filesystem>

// ============================================================================
// ScriptFileWatchPolicy — Phase 6C/W2 file-watcher dispatch decision.
//
// What the host does when the watcher reports a changed .lua file. Lifted
// out of WalnutApp::OnUIRender so the decision is reachable from RT2Tests;
// the caller keeps the debounce buffer, the mutex, and the actual calls.
//
// Not to be confused with ScriptFieldChangePolicy.h, which classifies field
// DATA changes found during scene load. This one is about FILE changes found
// on disk while the editor is open.
// ============================================================================

namespace rt2::core {

// The two effects are INDEPENDENT, not alternatives. The host owns two
// distinct caches: ScriptSystem::m_FieldRegistry (runtime) and WalnutApp's
// m_InspectorFieldRegistry (editor UI). A running reload refreshes the
// former; only an explicit invalidation refreshes the latter. Modelling
// this as an either/or is what left the inspector showing declarations
// parsed from the pre-edit file for the whole Play session.
struct ScriptFileChangeAction
{
    bool reloadScript = false;             // hand the path to ScriptSystem
    bool invalidateFieldRegistry = false;  // drop the inspector's cache

    bool operator==(const ScriptFileChangeAction& o) const
    {
        return reloadScript == o.reloadScript &&
               invalidateFieldRegistry == o.invalidateFieldRegistry;
    }
};

// Decide what a changed .lua file means for the current run state.
//
// Playing AND Paused both route to ReloadScript. ScriptSystem::ReloadScript
// owns the three-way run-state branch internally (Playing reloads now,
// Paused queues and drains on Resume, Edit invalidates its own cache), so
// gating on Playing here would make its Paused branch — and the whole
// pending-reload queue — unreachable from the watcher.
//
// The inspector cache is invalidated in EVERY state: the file backing the
// declarations it is displaying just changed, and that is true whether or
// not a runtime instance exists to swap.
inline ScriptFileChangeAction DecideScriptFileChange(SceneRunState state,
                                                     bool hasScriptSystem,
                                                     bool hasFieldRegistry)
{
    const bool running = (state == SceneRunState::Playing ||
                          state == SceneRunState::Paused);

    ScriptFileChangeAction action;
    action.reloadScript = running && hasScriptSystem;
    action.invalidateFieldRegistry = hasFieldRegistry;
    return action;
}

// A watcher must never interpret a relative diagnostic candidate against
// process CWD. Successful and missing candidates are both watchable when the
// resolver supplied an absolute physical path.
inline std::filesystem::path ScriptWatchDirectoryForCandidate(
    const std::filesystem::path& candidate)
{
    const auto normalized = candidate.lexically_normal();
    if (normalized.empty() || !normalized.is_absolute())
        return {};
    return normalized.parent_path();
}

} // namespace rt2::core

#endif // RT2_CORE_SCRIPT_FILE_WATCH_POLICY_H
