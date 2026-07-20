#pragma once

#ifndef RT2_CORE_IRUNTIME_COMMAND_SINK_H
#define RT2_CORE_IRUNTIME_COMMAND_SINK_H

#include "RuntimeSceneMutator.h"   // RuntimeEntityCreateDesc
#include "TransformEditing.h"      // EditableTRS
#include "core/Error.h"
#include "core/UUID.h"
#include "ECSComponents.h"         // ScriptComponent

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <optional>
#include <string>

// ============================================================================
// IRuntimeCommandSink — the controlled mutation channel handed to Lua
// scripts alongside the const runtime document.
//
// ScriptSystem::OnSceneStart receives const SceneDocument& (from
// IRuntimeLifecycleObserver) and IRuntimeCommandSink* (from the controller).
// The Lua `entity` and `world` bindings hold a pointer to the sink; every
// script-driven mutation routes through it. Scripts never see the
// SceneDocument, the entt::registry, SceneManager, or the render bridge.
//
// S4 resolution (Phase 6 design review): the narrative's "write directly to
// the runtime SceneDocument's Transform" is reconciled here — the script
// calls entity.set_position(vec3), which calls sink->SetRuntimeTransform,
// which writes the runtime document's Transform via the non-const path the
// sink owns. The script never sees the document.
//
// The sink wraps:
//   - RuntimeSceneController::QueueCreateRuntimeEntity (world.spawn)
//   - RuntimeSceneController::QueueDestroyRuntimeEntity (world.destroy)
//   - Direct writes to the runtime document's Transform / NameComponent /
//     VisibleComponent (entity.set_position, entity.set_visible, etc.)
//
// No raw entt::registry, no SceneManager access, no render bridge. The
// implementation (a concrete RuntimeCommandSink in ScriptSystem.cpp) holds a
// non-owning pointer to the controller and the runtime document, set at
// OnSceneStart and cleared at OnSceneStop. After OnDestroy for an entity,
// the Lua entity handle's sink pointer is nulled so all methods fail safely.
//
// CPU-only header (no Vulkan/ImGui/Walnut/GLFW).
// ============================================================================

namespace rt2::core {

class RuntimeSceneController;

class IRuntimeCommandSink
{
public:
    virtual ~IRuntimeCommandSink() = default;

    // ---- world.* (deferred structural operations) ------------------------

    // Queue a spawn. The UUID is allocated at queue time from the runtime
    // UUID provider; the new entity is created at the next
    // SyncScriptEnvironments (the safe point after ApplyDeferredStructural-
    // Changes). Returns the allocated UUID so the caller can reference the
    // pending entity this frame; the Lua handle returned to the script is
    // "pending" and fails safely until the environment is built.
    virtual Result<UUID> SpawnEntity(const RuntimeEntityCreateDesc& desc) = 0;

    // Queue a destroy. OnDestroy fires at the next SyncScriptEnvironments
    // (before the environment is torn down); the entity is alive for the
    // rest of this frame's callbacks and gone next frame.
    virtual Result<void> DestroyEntity(const UUID& uuid) = 0;

    // ---- entity.* (direct runtime-document writes) ----------------------

    // Returns true and out-transform if the UUID resolves in the runtime
    // document. Returns false if the entity does not exist (destroyed,
    // pending spawn, or unknown UUID). Reads the local TRS.
    virtual bool GetLocalTransform(const UUID& uuid, EditableTRS& out) const = 0;

    // Writes the local TRS on the runtime document. Returns false if the
    // UUID does not resolve. Marks the transform dirty; the batched
    // transform sync at end of frame picks it up.
    virtual bool SetLocalTransform(const UUID& uuid, const EditableTRS& trs) = 0;

    // Convenience: get/set position only. Position is in local space; world
    // position requires walking the parent chain (deferred to 6C unless
    // profiling proves it's needed in 6A).
    virtual bool GetPosition(const UUID& uuid, glm::vec3& out) const = 0;
    virtual bool SetPosition(const UUID& uuid, const glm::vec3& pos) = 0;

    // Name. Returns an empty string if the UUID does not resolve.
    virtual std::string GetName(const UUID& uuid) const = 0;
    virtual bool SetName(const UUID& uuid, const std::string& name) = 0;

    // Visibility.
    virtual bool GetVisible(const UUID& uuid, bool& out) const = 0;
    virtual bool SetVisible(const UUID& uuid, bool visible) = 0;

    // ---- lookup ----------------------------------------------------------

    // Find by UUID. Returns true if the UUID resolves in the runtime
    // document (the handle is live). Returns false for pending spawns (not
    // yet applied) and destroyed entities.
    virtual bool IsAlive(const UUID& uuid) const = 0;

    // Find by name. Returns the first matching UUID in UUID-sorted order,
    // or UUID::Nil() if none. Name lookup is O(n) over the registry.
    virtual UUID FindByName(const std::string& name) const = 0;
};

} // namespace rt2::core

#endif // RT2_CORE_IRUNTIME_COMMAND_SINK_H