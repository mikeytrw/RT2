#pragma once

#ifndef RT2_CORE_IRUNTIME_COMMAND_SINK_H
#define RT2_CORE_IRUNTIME_COMMAND_SINK_H

#include "RuntimeSceneMutator.h"   // RuntimeEntityCreateDesc
#include "SceneRunState.h"         // SceneRunState
#include "TransformEditing.h"      // EditableTRS
#include "PhysicsEvents.h"         // PhysicsEvent (CPU-only, no Bullet)
#include "core/Error.h"
#include "core/UUID.h"
#include "ECSComponents.h"         // ScriptComponent

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <optional>
#include <string>
#include <vector>

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

// T7 bounded pose-reset parameters (entity:reset_body_pose). Rotation is a
// unit-intent quaternion (x, y, z, w); the controller normalizes before
// enqueueing and refuses degenerate input. Velocities are optional: absent
// means "rest" (zero), matching the trigger/event -> reset -> impulse reuse
// flow where the reset body starts still.
struct PhysicsPoseReset
{
    glm::vec3 position = {0.0f, 0.0f, 0.0f};
    glm::quat rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
    bool hasLinearVelocity = false;
    glm::vec3 linearVelocity = {0.0f, 0.0f, 0.0f};
    bool hasAngularVelocity = false;
    glm::vec3 angularVelocity = {0.0f, 0.0f, 0.0f};
};

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

    // ---- entity.* light/camera/material (Phase 6C) ----------------------

    // Light. Get returns false if the entity has no LightComponent. Set
    // gates on IsRuntimeMutable. Writes the full component (6 fields).
    virtual bool GetLight(const UUID& uuid, LightComponent& out) const = 0;
    virtual bool SetLight(const UUID& uuid, const LightComponent& light) = 0;

    // Camera. Get returns false if the entity has no CameraComponent. Set
    // gates on IsRuntimeMutable. Writes the full component (4 fields,
    // including forwardDirection).
    virtual bool GetCamera(const UUID& uuid, CameraComponent& out) const = 0;
    virtual bool SetCamera(const UUID& uuid, const CameraComponent& cam) = 0;

    // Material index. Gates on IsRuntimeMutable. Returns false if the
    // entity has no MeshRef. Rejects index < -1 or index >= materialCount
    // (-1 is the "use per-triangle indices" sentinel).
    virtual bool SetMaterialIndex(const UUID& uuid, int index) = 0;

    // ---- entity.* bounded physics controls (Bullet T7) --------------------
    //
    // Reads observe live Bullet state directly (no queueing): linear and
    // angular velocity for simulated rigid bodies. Each returns false
    // (writing nothing) for unknown UUIDs and ghost-only entities.
    //
    // Writes never touch Bullet inline: they enqueue a validated command on
    // the controller, applied at the next pre-step boundary (at most one
    // fixed tick of latency: same-tick when queued from OnFixedUpdate, next
    // frame's first tick when queued from OnUpdate). Every write returns
    // false (enqueueing nothing) when the session is not mutable (silent),
    // when the target lies in the frozen destroy set (loud warn, the T6
    // refusal), or when the UUID/body/kind/arguments are invalid (loud
    // warn). No Bullet pointer ever enters Lua; no registry mutates during
    // script iteration.
    virtual bool GetLinearVelocity(const UUID& uuid, glm::vec3& out) const = 0;
    virtual bool GetAngularVelocity(const UUID& uuid, glm::vec3& out) const = 0;
    // Linear-velocity write for Dynamic/Kinematic bodies (Static refuses).
    virtual bool SetLinearVelocity(const UUID& uuid,
                                   const glm::vec3& velocity) = 0;
    // Central impulse (J = m*dv) for Dynamic bodies only
    // (Static/Kinematic refuse).
    virtual bool ApplyImpulse(const UUID& uuid, const glm::vec3& impulse) = 0;
    // Hinge drive (angular-velocity motor) / release (free swing) for the
    // entity owning the hinge constraint. Unknown/non-hinge owners and
    // bad parameters refuse.
    virtual bool SetHingeDrive(const UUID& owner, float velocity,
                               float maxImpulse) = 0;
    virtual bool ReleaseHingeDrive(const UUID& owner) = 0;
    // Slider target (refused outside [lower,upper], never clamped) /
    // release (motor off + impulse along the axis) for the entity owning
    // the slider constraint.
    virtual bool SetSliderTarget(const UUID& owner, float target) = 0;
    virtual bool ReleaseSlider(const UUID& owner, float impulse) = 0;
    // Bounded pose reset for Dynamic/Kinematic bodies only (Static,
    // missing bodies, and destroying UUIDs refuse with no partial state
    // change). Applied atomically at the next pre-step boundary: pose,
    // velocities (or zero when absent), force/torque clear, wake,
    // broadphase/contact/ghost refresh, and ECS previous-transform update.
    virtual bool ResetBodyPose(const UUID& uuid,
                               const PhysicsPoseReset& reset) = 0;

    // ---- world.* physics events (Bullet T7) --------------------------------
    //
    // A copy of the controller's immutable per-frame snapshot (the T6
    // publication: canonical pairs, coalesced contacts, grouped trigger
    // transitions, destroy-filtered, exact-dup collapsed). Non-consuming:
    // every call in the frame returns the same contents regardless of
    // UUID-sorted callback order. Empty outside OnUpdate (pre-publication
    // callbacks always observe empty), when zero ticks ran, and after Stop.
    virtual std::vector<PhysicsEvent> GetPhysicsEvents() const = 0;

    // Drop all queued-but-unapplied physics commands. Called by
    // ScriptSystem on quarantine and reload (no stale command outlives its
    // issuing environment); the controller's Stop clears its own queue
    // directly. Silent.
    virtual void ClearQueuedPhysicsCommands() = 0;
    // Test seam: commands waiting for the next pre-step boundary.
    virtual size_t QueuedPhysicsCommandCount() const = 0;

    // ---- lookup ----------------------------------------------------------

    // Find by UUID. Returns true if the UUID resolves in the runtime
    // document (the handle is live). Returns false for pending spawns (not
    // yet applied) and destroyed entities.
    virtual bool IsAlive(const UUID& uuid) const = 0;

    // Find by name. Returns the first matching UUID in UUID-sorted order,
    // or UUID::Nil() if none. Name lookup is O(n) over the registry.
    virtual UUID FindByName(const std::string& name) const = 0;

    // ---- run-state query (Phase 6C) --------------------------------------

    // The current run state. Used by ReloadScript to distinguish Playing
    // (reload now) from Paused (queue) from Stopped (invalidate cache only).
    // Delegates to RuntimeSceneController::GetState.
    virtual SceneRunState GetRunState() const = 0;

    // True when Playing or Paused and not mid-Stop. Delegates to
    // RuntimeSceneController::IsRuntimeMutable (which checks m_Stopping).
    virtual bool IsRuntimeMutable() const = 0;
};

} // namespace rt2::core

#endif // RT2_CORE_IRUNTIME_COMMAND_SINK_H