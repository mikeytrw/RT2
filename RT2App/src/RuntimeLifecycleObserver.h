#pragma once

#ifndef RT2_CORE_RUNTIME_LIFECYCLE_OBSERVER_H
#define RT2_CORE_RUNTIME_LIFECYCLE_OBSERVER_H

// ============================================================================
// IRuntimeLifecycleObserver — const observation seam for Play/Stop lifecycle.
//
// Phase 4 introduced the observer with a const-observe start/stop contract.
// Phase 6 extends OnSceneStart to carry two additional non-const pointers
// (the read-only input service and the runtime command sink) so scripts can
// read input and mutate the runtime world through the controlled channel.
// The const SceneDocument& remains the first parameter; the new pointers
// are non-owning and are valid only for the duration of the Play session
// (until OnSceneStop). They are null when no script system is registered
// (the Phase 4 test observers receive null and ignore them).
//
// OnSceneStart fires AFTER the runtime document is fully activated and
// m_State == Playing, so a callback that queries
// RuntimeSceneController::GetState() sees the post-Play state.
// OnSceneStop fires BEFORE the runtime document is destroyed, while queue
// submission is disabled (QueueCreateRuntimeEntity / QueueDestroyRuntimeEntity
// return Error::InvalidRuntimeState during OnSceneStop).
//
// The interface is dependency-free (forward-declares SceneDocument,
// IInputService, IRuntimeCommandSink) so it links into RT2Tests and
// RT2SliceRunner without Vulkan or Lua.
// ============================================================================

namespace rt2::core {

class SceneDocument;
class IInputService;
class IRuntimeCommandSink;

class IRuntimeLifecycleObserver
{
public:
    virtual ~IRuntimeLifecycleObserver() = default;

    // Fires once per Play, after the runtime document is constructed,
    // activated via the bridge, and m_State == Playing. Receives the
    // runtime document by const reference. `input` and `sink` are non-
    // owning pointers valid until OnSceneStop; they are null when no
    // script system is registered. Phase 4 observers that do not override
    // this signature get the backward-compatible single-argument default.
    virtual void OnSceneStart(const SceneDocument& runtime,
                              const IInputService* input,
                              IRuntimeCommandSink* sink)
    {
        (void)runtime; (void)input; (void)sink;
        OnSceneStart(runtime);  // back-compat shim
    }

    // Backward-compatible single-argument overload. Called by the default
    // implementation of the three-argument OnSceneStart above so Phase 4
    // observers that override only this signature continue to work.
    virtual void OnSceneStart(const SceneDocument& runtime) { (void)runtime; }

    // Fires once per Stop, BEFORE the runtime document is destroyed and
    // AFTER queue submission is disabled. Receives the runtime document
    // by const reference. A callback cannot queue structural operations
    // here (submission is rejected with Error::InvalidRuntimeState).
    virtual void OnSceneStop(const SceneDocument& runtime) { (void)runtime; }
};

} // namespace rt2::core

#endif // RT2_CORE_RUNTIME_LIFECYCLE_OBSERVER_H