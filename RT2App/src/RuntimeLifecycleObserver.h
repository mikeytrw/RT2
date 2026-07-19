#pragma once

#ifndef RT2_CORE_RUNTIME_LIFECYCLE_OBSERVER_H
#define RT2_CORE_RUNTIME_LIFECYCLE_OBSERVER_H

// ============================================================================
// IRuntimeLifecycleObserver — const observation seam for Play/Stop lifecycle.
//
// Phase 4 implements only the observer. OnSceneStart fires AFTER the runtime
// document is fully activated and m_State == Playing, so a callback that
// queries RuntimeSceneController::GetState() sees the post-Play state.
// OnSceneStop fires BEFORE the runtime document is destroyed, while queue
// submission is disabled (QueueCreateRuntimeEntity / QueueDestroyRuntimeEntity
// return Error::InvalidRuntimeState during OnSceneStop).
//
// The callback receives the runtime document by const reference — Phase 4
// does NOT provide a mutation channel. Phase 6 scripting will add a separate
// IRuntimeCommandSink (or similar) passed alongside the const document to
// OnSceneStart, giving scripts a controlled mutation channel that routes
// through the deferred structural-operation queue. This interface is added
// now so Phase 6 can hook scripting into it without further
// RuntimeSceneController structural changes.
//
// The interface is dependency-free (forward-declare SceneDocument) so it links
// into RT2Tests and RT2SliceRunner without Vulkan.
// ============================================================================

namespace rt2::core {

class SceneDocument;

class IRuntimeLifecycleObserver
{
public:
    virtual ~IRuntimeLifecycleObserver() = default;

    // Fires once per Play, after the runtime document is constructed,
    // activated via the bridge, and m_State == Playing. Receives the
    // runtime document by const reference.
    virtual void OnSceneStart(const SceneDocument& runtime) { (void)runtime; }

    // Fires once per Stop, BEFORE the runtime document is destroyed and
    // AFTER queue submission is disabled. Receives the runtime document
    // by const reference. A callback cannot queue structural operations
    // here (submission is rejected with Error::InvalidRuntimeState).
    virtual void OnSceneStop(const SceneDocument& runtime) { (void)runtime; }
};

} // namespace rt2::core

#endif // RT2_CORE_RUNTIME_LIFECYCLE_OBSERVER_H