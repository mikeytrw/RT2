#pragma once

#ifndef RT2_CORE_IRUNTIME_SCRIPT_DISPATCH_H
#define RT2_CORE_IRUNTIME_SCRIPT_DISPATCH_H

#include "core/UUID.h"

#include <vector>

// ============================================================================
// IRuntimeScriptDispatch — per-frame, mutation-driving script dispatch seam.
//
// This is the companion to IRuntimeLifecycleObserver. The lifecycle observer
// is const-observe (OnSceneStart/OnSceneStop receive a const SceneDocument&),
// added in Phase 4 to give systems a clean observation hook for Play/Stop.
// Phase 6 scripting needs the runtime controller to call into the script
// system every frame (OnFixedUpdate, OnUpdate) and once per frame to sync
// script environments with the runtime registry (SyncScriptEnvironments).
// Those calls drive mutation through the IRuntimeCommandSink and are NOT
// const-observe, so they do not belong on IRuntimeLifecycleObserver.
//
// G1 resolution (Phase 6 design review): folding per-frame dispatch into the
// lifecycle observer would pollute its documented const-observe contract.
// IRuntimeScriptDispatch is a separate interface the controller holds via a
// new SetScriptDispatch/GetScriptDispatch pair — the one structural addition
// to RuntimeSceneController the Phase 6 plan previously hand-waved.
//
// CPU-only (no Vulkan/ImGui/Walnut/GLFW), so it links into RT2Tests and
// RT2SliceRunner. ScriptSystem implements both IRuntimeLifecycleObserver and
// IRuntimeScriptDispatch.
//
// Frame-order contract (canonical reference: docs/game-loop.md):
//   RunFixedTick(dt):
//       m_ScriptDispatch->OnFixedUpdate(dt)     // BEFORE motion integration
//       <inline motion integration>             // (existing)
//   Update(frameDt) / Step:
//       <fixed-step loop calling RunFixedTick>
//       ApplyDeferredStructuralChanges()        // existing safe-point drain
//       m_ScriptDispatch->SyncScriptEnvironments()  // G2: build/teardown envs
//       m_ScriptDispatch->OnUpdate(frameDt)     // AFTER safe point
//       SceneGraph::UpdateWorldTransforms()
//       <one batched GPU sync>
// ============================================================================

namespace rt2::core {

class IRuntimeScriptDispatch
{
public:
    virtual ~IRuntimeScriptDispatch() = default;

    // Fixed-step script callbacks. Called once per fixed substep, BEFORE the
    // inline motion integration in RunFixedTick, in UUID-sorted entity order.
    // Spawns queued here do NOT resolve mid-loop — they resolve at the safe
    // point after the fixed-step loop.
    virtual void OnFixedUpdate(float dt) = 0;

    // Variable-step script callbacks. Called once per rendered frame, AFTER
    // ApplyDeferredStructuralChanges AND SyncScriptEnvironments, BEFORE
    // SceneGraph::UpdateWorldTransforms, in UUID-sorted entity order. A
    // scripted spawn queued during OnFixedUpdate this frame is visible to
    // OnUpdate this frame (it resolved at SyncScriptEnvironments). A spawn
    // queued during OnUpdate resolves next frame.
    virtual void OnUpdate(float dt) = 0;

    // Called from inside ApplyDeferredStructuralChanges, immediately BEFORE
    // a queued subtree destruction is applied, with the subtree's UUIDs in
    // post-order (children first). The entities are STILL ALIVE and still
    // resolve in the runtime document.
    //
    // Why this exists: the drain used to apply the destruction and only then
    // call SyncScriptEnvironments, which fired on_destroy. The environment
    // was still alive but the UUID no longer resolved, so a script's final
    // callback saw entity:get_name() return empty and get_position() fail —
    // contradicting the contract that a script observes itself alive during
    // its own teardown. Firing here restores it.
    //
    // Defaulted to a no-op so non-script dispatch implementations and older
    // tests are unaffected.
    virtual void OnEntitiesDestroying(const std::vector<UUID>& uuids)
    {
        (void)uuids;
    }

    // G2: the single chokepoint where the script system's per-entity
    // environment map mirrors the runtime registry. Called once per frame
    // (Update and Step) AFTER ApplyDeferredStructuralChanges and BEFORE
    // OnUpdate. For entities in the registry but not in the environment map
    // (newly applied spawns, or initial Play): construct a fresh sol2
    // environment, load the script, evaluate rt2.fields, bind callbacks, and
    // fire OnCreate immediately. For entities in the environment map but not
    // in the registry (destroyed at the safe point): fire OnDestroy, then
    // tear down the environment. For entities in both: no-op.
    virtual void SyncScriptEnvironments() = 0;
};

} // namespace rt2::core

#endif // RT2_CORE_IRUNTIME_SCRIPT_DISPATCH_H