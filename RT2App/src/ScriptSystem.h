#pragma once

#ifndef RT2_CORE_SCRIPT_SYSTEM_H
#define RT2_CORE_SCRIPT_SYSTEM_H

#include "RuntimeLifecycleObserver.h"
#include "IRuntimeScriptDispatch.h"
#include "IRuntimeCommandSink.h"
#include "InputTypes.h"
#include "ECSComponents.h"
#include "ScriptFieldValue.h"
#include "core/UUID.h"

#include <sol/sol.hpp>

#include <unordered_map>
#include <vector>
#include <string>
#include <filesystem>

// ============================================================================
// ScriptSystem — Phase 6A Lua embedding and lifecycle dispatch.
//
// ScriptSystem is the single owner of the engine's Lua state and the per-
// entity environment map. It implements both IRuntimeLifecycleObserver
// (const-observe Play/Stop hooks) and IRuntimeScriptDispatch (per-frame
// mutation-driving callbacks). See the Phase 6 plan in
// docs/game-engine-development-plan.md for the full design.
//
// CPU-only: links into RT2Tests and (eventually) RT2SliceRunner without
// Vulkan/ImGui/Walnut. The only non-CPU-only dependency is sol2 + Lua,
// which are gated per-target by premake.
//
// Per-instance state machine (S1):
//
//   NeverCreated --OnCreate--> Live --runtime error--> Quarantined
//                                  |                      |
//                                  |                      +--(reload, 6C)--> Live
//                                  |
//                                  +--OnDestroy (Stop or safe point)--> Destroyed
//
// Quarantined instances receive no further callbacks this Play session
// until a successful reload (6C). OnDestroy is NOT called on a quarantined
// instance at Stop (it never had a clean lifecycle).
//
// Protected-call discipline (S7): every Lua entry point (rt2.fields eval,
// all four callbacks, sink-invoked calls) runs through
// sol::protected_function with a bound error handler. A lua_atpanic guard
// is installed on the sol::state at construction. Any uncaught error or
// panic transitions the instance to Quarantined.
// ============================================================================

namespace rt2::core {

class SceneDocument;
class RuntimeSceneController;

// Per-instance state (S1).
enum class ScriptInstanceState : uint8_t
{
    NeverCreated = 0,
    Live         = 1,
    Quarantined  = 2,
    Destroyed    = 3,
};

// One entry per entity that has a live or pending script environment.
struct ScriptInstance
{
    UUID                    uuid;
    sol::environment        env;
    sol::protected_function on_create;
    sol::protected_function on_fixed_update;
    sol::protected_function on_update;
    sol::protected_function on_destroy;
    ScriptInstanceState     state = ScriptInstanceState::NeverCreated;
    // Creation order (S5): the index in ScriptSystem::m_CreationOrder at
    // the time OnCreate fired. Used for reverse-creation-order OnDestroy
    // at Stop.
    size_t                  creationOrderIndex = 0;
};

class ScriptSystem : public IRuntimeLifecycleObserver,
                     public IRuntimeScriptDispatch
{
public:
    explicit ScriptSystem(IUuidProvider& uuidProvider);
    ~ScriptSystem() override;

    ScriptSystem(const ScriptSystem&) = delete;
    ScriptSystem& operator=(const ScriptSystem&) = delete;

    // ---- IRuntimeLifecycleObserver --------------------------------------

    void OnSceneStart(const SceneDocument& runtime,
                      const IInputService* input,
                      IRuntimeCommandSink* sink) override;
    void OnSceneStop(const SceneDocument& runtime) override;

    // ---- IRuntimeScriptDispatch -----------------------------------------

    void OnFixedUpdate(float dt) override;
    void OnUpdate(float dt) override;
    void SyncScriptEnvironments() override;

    // ---- Phase 6C hot reload (declared now, stubbed in 6A) --------------

    virtual void ReloadScript(const std::filesystem::path& path) { (void)path; }

    // ---- Test/inspector helpers (6B will extend; 6A provides the seam) --

    // Returns the current instance state for an entity, or NeverCreated if
    // the entity has no script environment.
    ScriptInstanceState GetInstanceState(const UUID& uuid) const;

    // Returns the number of live (Live only) script instances.
    size_t LiveInstanceCount() const;

    // Returns the number of quarantined script instances.
    size_t QuarantinedInstanceCount() const;

private:
    // Build a fresh sol::environment for one entity, load the script source
    // from disk, evaluate rt2.fields (empty in 6A), bind callbacks. Returns
    // false and quarantines the instance on any load/syntax error.
    bool BuildEnvironment(ScriptInstance& inst,
                          const ScriptComponent& comp,
                          const IInputService* input,
                          IRuntimeCommandSink* sink);

    // Fire a protected callback. On any error, quarantines the instance and
    // returns false. A missing (nil) callback is a no-op (returns true).
    bool FireProtected(ScriptInstance& inst,
                       sol::protected_function& fn,
                       const std::string& callbackName,
                       const std::function<sol::protected_function_result(sol::protected_function&)>& invoke);

    // Transition an instance to Quarantined and log the error with path,
    // entity UUID, callback name, and stack trace.
    void Quarantine(ScriptInstance& inst,
                    const std::string& callbackName,
                    const std::string& message);

    // Collect ScriptComponent-bearing entities from the runtime registry,
    // sorted by UUID for deterministic iteration.
    std::vector<std::pair<UUID, entt::entity>>
    CollectScriptEntitiesSorted() const;

    // The single engine Lua state. Rebuilt at OnSceneStart, torn down at
    // OnSceneStop.
    sol::state m_Lua;

    // Per-entity environment map, keyed by UUID. This is the per-frame-
    // maintained mirror of the runtime registry (G2): SyncScriptEnvironments
    // adds entries for newly applied entities and removes entries for
    // destroyed ones.
    std::unordered_map<UUID, ScriptInstance> m_Instances;

    // Creation order (S5): UUIDs in the order OnCreate fired. Stop iterates
    // this in reverse for OnDestroy.
    std::vector<UUID> m_CreationOrder;

    // Non-owning pointers, valid for the duration of a Play session (set
    // at OnSceneStart, cleared at OnSceneStop).
    const SceneDocument*      m_RuntimeDoc = nullptr;
    const IInputService*      m_Input = nullptr;
    IRuntimeCommandSink*      m_Sink = nullptr;
    IUuidProvider&            m_UuidProvider;

    // Lua panic handler (S7). Installed via lua_atpanic on m_Lua. Returns
    // a longjmp-style error so the offending protected_call sees it as a
    // runtime error rather than a process abort.
    static int LuaPanic(lua_State* L);
};

// ============================================================================
// RuntimeCommandSink — the concrete IRuntimeCommandSink handed to scripts.
//
// Holds a non-owning pointer to the controller (for queue ops) and the
// runtime document (for direct transform/vis writes). The document pointer
// is const, but the sink writes through it via const_cast — this is the
// same pattern RuntimeSceneController uses for gpuCache. The writes are
// safe because the sink is only live during Play, when the authoring
// document is not the active render document and the runtime document is
// exclusively accessed by the controller + script system.
// ============================================================================

class RuntimeCommandSink final : public IRuntimeCommandSink
{
public:
    // The sink holds a non-owning pointer to the controller and resolves
    // the runtime document lazily at call time via
    // TryGetRuntimeScene(). This avoids the chicken-and-egg problem where
    // the sink must be set BEFORE Play (so scripts get it at OnSceneStart)
    // but the runtime doc doesn't exist until Play constructs it.
    explicit RuntimeCommandSink(RuntimeSceneController& controller);
    ~RuntimeCommandSink() override = default;

    Result<UUID> SpawnEntity(const RuntimeEntityCreateDesc& desc) override;
    Result<void> DestroyEntity(const UUID& uuid) override;

    bool GetLocalTransform(const UUID& uuid, EditableTRS& out) const override;
    bool SetLocalTransform(const UUID& uuid, const EditableTRS& trs) override;
    bool GetPosition(const UUID& uuid, glm::vec3& out) const override;
    bool SetPosition(const UUID& uuid, const glm::vec3& pos) override;
    std::string GetName(const UUID& uuid) const override;
    bool SetName(const UUID& uuid, const std::string& name) override;
    bool GetVisible(const UUID& uuid, bool& out) const override;
    bool SetVisible(const UUID& uuid, bool visible) override;
    bool IsAlive(const UUID& uuid) const override;
    UUID FindByName(const std::string& name) const override;

private:
    RuntimeSceneController&  m_Controller;
};

} // namespace rt2::core

#endif // RT2_CORE_SCRIPT_SYSTEM_H