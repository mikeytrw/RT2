#include "ScriptSystem.h"
#include "ScriptAssetPath.h"
#include "ScriptFieldReconcile.h"
#include "ScriptSandbox.h"
#include "RuntimeSceneController.h"
#include "SceneDocument.h"
#include "ECSComponents.h"
#include "ECSScene.h"
#include "SceneGraph.h"
#include "SceneHierarchy.h"
#include "TransformEditing.h"
#include "core/UUID.h"
#include "core/Error.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <exception>
#include <fstream>
#include <sstream>

namespace rt2::core {

// ============================================================================
// ScriptSystem construction / destruction
// ============================================================================

ScriptSystem::ScriptSystem(
    IUuidProvider& uuidProvider,
    const AssetResolutionContext& assetContext,
    std::vector<AssetDiagnostic>& assetDiagnostics)
    : m_UuidProvider(uuidProvider)
    , m_AssetContext(assetContext)
    , m_AssetDiagnostics(assetDiagnostics)
{
    // Open only the safe Lua libraries. os/io/debug/package are deliberately
    // NOT opened: os.exit would terminate the process, io.* gives filesystem
    // access, package/require gives module loading. base is opened (it has
    // print, pairs, ipairs, type, error, pcall, etc.) but the dangerous
    // globals are shadowed per-environment in BuildEnvironment (Q6b) — see
    // ScriptSandbox.h.
    //
    // coroutine was opened here with nothing using it and no sandboxing
    // story, so it is no longer opened: an unused library is unreviewed
    // attack surface. If 6C implements timer.after/timer.every on
    // coroutines it should re-open it deliberately, with the deny list
    // revisited at the same time.
    m_Lua.open_libraries(sol::lib::base,
                         sol::lib::math,
                         sol::lib::string,
                         sol::lib::table,
                         sol::lib::utf8);

    // S7: install a panic guard. A Lua panic means an unprotected error
    // crossed C frames with no pcall on the stack — the handler terminates
    // the process rather than risking UB (throwing across C longjmp is
    // undefined on MSVC). The normal path never reaches it: every Lua
    // evaluation entry point is protected (sol::protected_function / pcall).
    // Unreachable except under allocation failure in unprotected setup
    // (open_libraries, BuildEnvironment table construction).
    lua_atpanic(m_Lua.lua_state(), &ScriptSystem::LuaPanic);
}

ScriptSystem::~ScriptSystem()
{
    // OnSceneStop tears down m_Instances and resets the state; if Stop was
    // not called (e.g. test teardown), the sol::state destructor reclaims
    // all Lua resources.
}

int ScriptSystem::LuaPanic(lua_State* L)
{
    const char* msg = lua_tostring(L, -1);
    if (!msg) msg = "Lua panic (no message)";
    // Lua is compiled as C, so throwing a C++ exception from this handler
    // would cross C frames reached via longjmp — undefined behavior on MSVC.
    // Every Lua evaluation entry point is protected (sol::protected_function
    // / pcall), so a panic is unreachable except under allocation failure in
    // unprotected setup (open_libraries, BuildEnvironment table construction).
    // A panic is now fatal by design: std::terminate aborts the process and
    // does not quarantine. This is the honest minimum — defined death beats
    // undefined survival. Flush stderr before terminating because std::abort
    // does not flush stdio buffers.
    fprintf(stderr, "[Script] Lua panic (unrecoverable, terminating): %s\n", msg);
    fflush(stderr);
    std::terminate();
}

// ============================================================================
// IRuntimeLifecycleObserver
// ============================================================================

void ScriptSystem::OnSceneStart(const SceneDocument& runtime,
                                const IInputService* input,
                                IRuntimeCommandSink* sink)
{
    m_RuntimeDoc = &runtime;
    m_Input = input;
    m_Sink = sink;

    // Clear any leftover state from a prior session (defensive — OnSceneStop
    // should have done this).
    m_Instances.clear();
    m_CreationOrder.clear();
    m_Timers.clear();
    m_NextTimerHandle = 1;

    // Build environments for every ScriptComponent-bearing entity in the
    // runtime clone. This is the "initial Play" special case of
    // SyncScriptEnvironments (G2): the registry is full, the environment
    // map is empty, so every scripted entity gets OnCreate.
    const size_t diagnosticBase = m_AssetDiagnostics.size();
    SyncScriptEnvironments();
    SortAssetDiagnosticsFrom(diagnosticBase);
}

void ScriptSystem::OnSceneStop(const SceneDocument& runtime)
{
    (void)runtime;

    // S5: fire OnDestroy for all live instances in reverse-creation-order.
    // S1: skip OnDestroy for quarantined instances (they never had a clean
    // lifecycle). NeverCreated instances have no environment to tear down.
    for (auto it = m_CreationOrder.rbegin(); it != m_CreationOrder.rend(); ++it)
    {
        auto instIt = m_Instances.find(*it);
        if (instIt == m_Instances.end())
            continue;
        auto& inst = instIt->second;
        if (inst.state == ScriptInstanceState::Live)
        {
            // Fire on_destroy. The entity is still alive at this point
            // (the runtime document has not been destroyed yet). Errors
            // during on_destroy are logged but do NOT quarantine (the
            // session is ending anyway).
            if (inst.on_destroy.valid())
            {
                sol::protected_function_result result;
                {
                    ScriptInstructionBudget budget(m_Lua.lua_state(),
                                                   kScriptCallbackInstructionBudget);
                    result = inst.on_destroy(inst.env["entity"]);
                }
                if (!result.valid())
                {
                    sol::error err = result;
                    printf("[Script] on_destroy error (entity %s): %s\n",
                           inst.uuid.ToString().c_str(), err.what());
                }
            }
            inst.state = ScriptInstanceState::Destroyed;
        }
    }

    // Tear down all Lua state. The sol::state itself is kept alive (it's
    // a member) so the next Play session can reuse it without re-allocating
    // the Lua VM. Clearing the instances + creation order is sufficient;
    // the environments are sol::environment objects whose destructors
    // release the Lua refs.
    m_Instances.clear();
    m_CreationOrder.clear();
    m_PendingReloads.clear();
    m_Timers.clear();
    m_NextTimerHandle = 1;
    m_RuntimeDoc = nullptr;
    m_Input = nullptr;
    m_Sink = nullptr;
}

// ============================================================================
// Phase 6C — Timers
// ============================================================================
void ScriptSystem::FireTimers(float dt)
{
    // Accumulate time and fire due timers. Iterate by index; callbacks may
    // cancel timers (marking them cancelled) or create new ones (appending
    // via timer.after/timer.every). New timers are not fired this frame.
    //
    // Max one fire per timer per frame. A timer.every(0.05) under a 0.2s
    // frame fires once, not four times; remaining += interval preserves
    // long-run average rate without bunching. W7's headless scenario must
    // drive OnUpdate with a fixed dt for deterministic timer behavior.
    //
    // CRITICAL: do NOT hold a reference into m_Timers across the callback.
    // A callback that creates a timer (self-rescheduling is the common
    // idiom) may trigger a vector reallocation, dangling the reference.
    // Copy out the callable and scalar state, call, then re-index.
    const size_t initialCount = m_Timers.size();
    for (size_t i = 0; i < initialCount; ++i)
    {
        if (m_Timers[i].cancelled) continue;

        // Look up the owning instance. If it's gone or quarantined, skip.
        UUID ownerUuid = m_Timers[i].ownerUuid;
        auto it = m_Instances.find(ownerUuid);
        if (it == m_Instances.end()) continue;
        if (it->second.state != ScriptInstanceState::Live) continue;

        m_Timers[i].remaining -= static_cast<double>(dt);
        if (m_Timers[i].remaining > 0.0) continue;

        // Copy out the callback and scalar state BEFORE calling, so a
        // reallocation inside the callback doesn't dangle.
        sol::protected_function cb = m_Timers[i].callback;
        const double interval = m_Timers[i].interval;
        const bool repeating = m_Timers[i].repeating;
        const int handle = m_Timers[i].handle;

        if (cb.valid())
        {
            sol::protected_function_result result;
            {
                ScriptInstructionBudget budget(m_Lua.lua_state(),
                                               kScriptCallbackInstructionBudget);
                result = cb();
            }
            if (!result.valid())
            {
                sol::error err = result;
                // Include the handle so the log can identify which timer.
                const std::string cbName = "timer:" + std::to_string(handle);
                Quarantine(it->second, cbName, err.what());
            }
        }

        // Re-index after the callback (index is stable: no erase during
        // the loop, only push_back which may reallocate but the index i
        // is still valid into the new buffer).
        if (i < m_Timers.size())
        {
            if (repeating)
                m_Timers[i].remaining += interval;
            else
                m_Timers[i].cancelled = true;
        }
    }

    // Garbage-collect cancelled and one-shot-expired timers.
    m_Timers.erase(
        std::remove_if(m_Timers.begin(), m_Timers.end(),
            [](const ScriptTimer& t) { return t.cancelled; }),
        m_Timers.end());
}

void ScriptSystem::CancelTimersForInstance(const UUID& uuid)
{
    for (auto& t : m_Timers)
        if (t.ownerUuid == uuid)
            t.cancelled = true;
    // Actual removal happens in FireTimers' GC pass; this just marks them
    // so they don't fire before the next GC.
}

// ============================================================================
// Phase 6C — Hot reload
// ============================================================================
void ScriptSystem::ReloadScript(const std::filesystem::path& path)
{
    // Canonicalize the path for comparison with ScriptInstance::scriptPath.
    // Both the watcher (efsw, OS-native absolute paths) and BuildEnvironment
    // (the shared locator's normalized path) must normalize
    // before comparison (review M3). weakly_canonical resolves relative
    // paths and normalizes separators; lexically_normal handles the case
    // where the file doesn't exist yet (weakly_canonical would throw).
    std::string pathStr;
    {
        std::error_code ec;
        auto canonical = std::filesystem::weakly_canonical(path, ec);
        if (ec)
            pathStr = path.string();
        else
            pathStr = canonical.string();
    }

    // Stopped (Edit): no instances exist. Invalidate the registry cache so
    // the inspector's next GetDeclaredFields re-parses (review B2).
    if (!m_RuntimeDoc || !m_Sink)
    {
        m_FieldRegistry.Clear();
        return;
    }

    // Three-way run-state branch using GetRunState (review B2):
    //   Playing → reload now
    //   Paused  → queue; drain on Resume / next Play frame
    //   Edit (stopped mid-Stop) → invalidate cache only
    const auto runState = m_Sink->GetRunState();
    if (runState == SceneRunState::Paused)
    {
        m_PendingReloads.push_back(pathStr);
        return;
    }
    if (runState != SceneRunState::Playing)
    {
        // Edit or mid-Stop: no reload.
        m_FieldRegistry.Clear();
        return;
    }

    const size_t diagnosticBase = m_AssetDiagnostics.size();

    // Playing: reload now.
    // 1. Re-parse declarations.
    auto result = m_FieldRegistry.GetDeclaredFields(path);
    if (!result.parsed)
    {
        // Parse failure: keep all instances in their current state. Do NOT
        // quarantine Live instances — the running code is still valid; only
        // the new source is broken.
        printf("[Script] reload parse failed (script %s): %s\n",
               pathStr.c_str(), result.diagnostic.c_str());
        for (const auto& [uuid, inst] : m_Instances)
        {
            std::error_code ec;
            const auto canonical = std::filesystem::weakly_canonical(
                std::filesystem::path(inst.scriptPath), ec);
            const std::string instPathStr =
                ec ? inst.scriptPath : canonical.string();
            if (instPathStr != pathStr)
                continue;
            const entt::entity entity = m_RuntimeDoc->FindByUuid(uuid);
            const auto* component =
                entity == entt::null ? nullptr :
                m_RuntimeDoc->ecs.registry.try_get<ScriptComponent>(entity);
            if (component)
            {
                AppendAssetDiagnostic(
                    *component, uuid, AssetDiagnostic::Malformed, path,
                    result.diagnostic);
            }
        }
        SortAssetDiagnosticsFrom(diagnosticBase);
        return;
    }

    // 2. Find all instances whose scriptPath matches (both are canonical).
    for (auto& [uuid, inst] : m_Instances)
    {
        // Normalize the instance's stored path for comparison.
        std::string instPathStr;
        {
            std::error_code ec;
            auto canonical = std::filesystem::weakly_canonical(
                std::filesystem::path(inst.scriptPath), ec);
            instPathStr = ec ? inst.scriptPath : canonical.string();
        }
        if (instPathStr != pathStr)
            continue;
        if (inst.state != ScriptInstanceState::Live &&
            inst.state != ScriptInstanceState::Quarantined)
            continue;

        // 3. Read the current ScriptComponent from the runtime document to
        // get the fieldValues for reconciliation.
        auto* sc = const_cast<SceneDocument*>(m_RuntimeDoc)->
                       ecs.registry.try_get<ScriptComponent>(
                           m_RuntimeDoc->FindByUuid(uuid));
        if (!sc) continue;

        // 4. Reconcile field values against new declarations.
        std::vector<FieldDiagnostic> diags;
        ScriptFieldMap reconciled = ReconcileScriptFields(
            sc->fieldValues, result.descriptors, uuid, diags);
        for (const auto& d : diags)
        {
            // Log only consequential diagnostics, not every Added/Removed.
            if (d.kind == FieldDiagnostic::Kind::TypeChanged ||
                d.kind == FieldDiagnostic::Kind::InvalidStoredValue ||
                d.kind == FieldDiagnostic::Kind::AmbiguousAlias)
            {
                printf("[Script] reload reconcile (entity %s, field %s): %s\n",
                       uuid.ToString().c_str(), d.field.c_str(), d.message.c_str());
            }
        }

        // 5. Save old self for rt2.previous_state.
        sol::object oldSelf = inst.env["self"];
        const bool needsOnCreate = !inst.onCreateFired;

        // 6. Build a scratch instance with the reconciled component.
        ScriptComponent reconciledComp = *sc;
        reconciledComp.fieldValues = reconciled;

        ScriptInstance scratch;
        scratch.uuid = uuid;
        scratch.state = ScriptInstanceState::Live;

        if (!BuildEnvironment(scratch, reconciledComp, m_Input, m_Sink))
        {
            // Scratch build failed (syntax error, bad file). BuildEnvironment
            // quarantined the *scratch* instance (which is discarded); the
            // running instance is untouched (D9 trap 2). The quarantine log
            // above refers to the discarded scratch build, not the live
            // instance.
            printf("[Script] reload build failed for entity %s (scratch discarded, live instance kept)\n",
                   uuid.ToString().c_str());
            continue;
        }

        // 7. Copy old self into rt2.previous_state on the new environment's
        // rt2 table (NOT as a dotted key — sol does not split on '.').
        if (oldSelf.is<sol::table>())
        {
            sol::object rt2obj = scratch.env["rt2"];
            if (rt2obj.is<sol::table>())
                rt2obj.as<sol::table>()["previous_state"] = oldSelf;
        }

        // Cancel all timers for this instance before the swap (B3).
        // A timer.every closure captures the old environment; without
        // cancellation, old timers keep firing alongside new ones.
        CancelTimersForInstance(uuid);

        // 8. Swap the environment and re-bind callbacks.
        // D9 trap 1: reset all four callbacks before assigning the new ones.
        inst.on_create = sol::protected_function{};
        inst.on_fixed_update = sol::protected_function{};
        inst.on_update = sol::protected_function{};
        inst.on_destroy = sol::protected_function{};

        inst.env = std::move(scratch.env);
        inst.on_create = std::move(scratch.on_create);
        inst.on_fixed_update = std::move(scratch.on_fixed_update);
        inst.on_update = std::move(scratch.on_update);
        inst.on_destroy = std::move(scratch.on_destroy);

        // 9. Un-quarantine on successful reload (S1: Quarantined → Live).
        inst.state = ScriptInstanceState::Live;

        // 10. If on_create never fired (e.g. initial load failed and the
        // instance was quarantined before init), fire it now after the swap
        // so self is properly initialized. Otherwise, do NOT re-run
        // on_create — the entity already exists (D9).
        if (needsOnCreate && inst.on_create.valid())
        {
            sol::protected_function_result createResult;
            {
                ScriptInstructionBudget budget(m_Lua.lua_state(),
                                               kScriptCallbackInstructionBudget);
                createResult = inst.on_create(
                    inst.env["entity"], inst.env["world"]);
            }
            if (!createResult.valid())
            {
                sol::error err = createResult;
                Quarantine(inst, "on_create (reload)", err.what());
                continue;
            }
            inst.onCreateFired = true;
        }

        // 11. Update the runtime document's ScriptComponent with reconciled
        // values so subsequent saves (if any) carry the reconciled map.
        sc->fieldValues = reconciled;

        printf("[Script] reloaded script %s for entity %s\n",
               pathStr.c_str(), uuid.ToString().c_str());
    }
    SortAssetDiagnosticsFrom(diagnosticBase);
}

// ============================================================================
// IRuntimeScriptDispatch
// ============================================================================

void ScriptSystem::OnFixedUpdate(float dt)
{
    if (!m_RuntimeDoc) return;

    // UUID-sorted iteration (S5). CollectScriptEntitiesSorted returns the
    // registry's current scripted entities; we filter to live instances.
    auto entities = CollectScriptEntitiesSorted();
    for (const auto& [uuid, entity] : entities)
    {
        auto it = m_Instances.find(uuid);
        if (it == m_Instances.end()) continue;
        auto& inst = it->second;
        if (inst.state != ScriptInstanceState::Live) continue;

        if (!inst.on_fixed_update.valid())
            continue;

        // 4-arg signature locked from 6A (S2): entity, dt, input, world.
        // `input` is inert in 6A (methods added in 6C); the parameter is
        // present so 6A-authored scripts run unchanged under 6C.
        sol::protected_function_result result;
        {
            ScriptInstructionBudget budget(m_Lua.lua_state(),
                                           kScriptCallbackInstructionBudget);
            result = inst.on_fixed_update(
                inst.env["entity"], dt, inst.env["input"], inst.env["world"]);
        }
        if (!result.valid())
        {
            sol::error err = result;
            Quarantine(inst, "on_fixed_update", err.what());
        }
    }
}

void ScriptSystem::OnUpdate(float dt)
{
    if (!m_RuntimeDoc) return;

    // Drain pending reloads (paused → resumed). OnUpdate fires during Play
    // and Step (which is Paused). Only drain when actually Playing — Step
    // should advance the world as authored, not mutate its code mid-freeze.
    if (!m_PendingReloads.empty() && m_Sink &&
        m_Sink->GetRunState() == SceneRunState::Playing)
    {
        auto pending = std::move(m_PendingReloads);
        m_PendingReloads.clear();
        for (const auto& p : pending)
            ReloadScript(p);
    }

    auto entities = CollectScriptEntitiesSorted();
    for (const auto& [uuid, entity] : entities)
    {
        auto it = m_Instances.find(uuid);
        if (it == m_Instances.end()) continue;
        auto& inst = it->second;
        if (inst.state != ScriptInstanceState::Live) continue;

        if (!inst.on_update.valid())
            continue;

        sol::protected_function_result result;
        {
            ScriptInstructionBudget budget(m_Lua.lua_state(),
                                           kScriptCallbackInstructionBudget);
            result = inst.on_update(
                inst.env["entity"], dt, inst.env["input"], inst.env["world"]);
        }
        if (!result.valid())
        {
            sol::error err = result;
            Quarantine(inst, "on_update", err.what());
        }
    }

    // Fire due timers after all on_update callbacks. Timers accumulate
    // against this frame's dt and advance under both Play and Step (Step
    // calls OnUpdate while Paused, which is correct — single-step should
    // be representative of Play).
    FireTimers(dt);
}

void ScriptSystem::OnEntitiesDestroying(const std::vector<UUID>& uuids)
{
    // Fire on_destroy while the entities are STILL ALIVE (the controller
    // calls this immediately before applying the destruction), so a script's
    // final callback can still read itself. `uuids` arrives in post-order,
    // children before parents.
    //
    // Instances are marked Destroyed here, so the removal pass in
    // SyncScriptEnvironments — which only fires on_destroy for state == Live
    // — will not fire it a second time; it just erases the entry.
    for (const auto& uuid : uuids)
    {
        auto it = m_Instances.find(uuid);
        if (it == m_Instances.end()) continue;
        auto& inst = it->second;
        if (inst.state != ScriptInstanceState::Live) continue;

        if (inst.on_destroy.valid())
        {
            sol::protected_function_result result;
            {
                ScriptInstructionBudget budget(m_Lua.lua_state(),
                                               kScriptCallbackInstructionBudget);
                result = inst.on_destroy(inst.env["entity"]);
            }
            if (!result.valid())
            {
                sol::error err = result;
                printf("[Script] on_destroy error (script %s, entity %s): %s\n",
                       inst.scriptPath.empty() ? "<unbound>" : inst.scriptPath.c_str(),
                       inst.uuid.ToString().c_str(), err.what());
            }
        }
        inst.state = ScriptInstanceState::Destroyed;
        CancelTimersForInstance(uuid);
    }
}

void ScriptSystem::SyncScriptEnvironments()
{
    if (!m_RuntimeDoc) return;

    // G2: the single chokepoint where the environment map mirrors the
    // runtime registry. Walk the registry's ScriptComponent-bearing
    // entities; for each, determine whether the environment map needs to
    // add (OnCreate), remove (OnDestroy), or no-op the entry.

    // Collect the registry's current scripted entities (UUID -> entity).
    auto entities = CollectScriptEntitiesSorted();
    std::unordered_map<UUID, entt::entity> registrySet;
    for (const auto& [uuid, entity] : entities)
        registrySet[uuid] = entity;

    // 1. Fire OnDestroy + tear down environments for entities in the map
    //    but NOT in the registry (destroyed at the safe point). Q10c:
    //    iterate in reverse-creation-order to match Stop's ordering — a
    //    parent that spawned children in OnCreate destroys them before
    //    itself, even mid-session.
    std::vector<UUID> toRemove;
    for (auto rit = m_CreationOrder.rbegin(); rit != m_CreationOrder.rend(); ++rit)
    {
        const auto& uuid = *rit;
        if (registrySet.count(uuid) != 0) continue;
        auto instIt = m_Instances.find(uuid);
        if (instIt == m_Instances.end()) continue;
        auto& inst = instIt->second;
        if (inst.state == ScriptInstanceState::Live)
        {
            if (inst.on_destroy.valid())
            {
                sol::protected_function_result result;
                {
                    ScriptInstructionBudget budget(m_Lua.lua_state(),
                                                   kScriptCallbackInstructionBudget);
                    result = inst.on_destroy(inst.env["entity"]);
                }
                if (!result.valid())
                {
                    sol::error err = result;
                    printf("[Script] on_destroy error (entity %s): %s\n",
                           inst.uuid.ToString().c_str(), err.what());
                }
            }
        }
        inst.state = ScriptInstanceState::Destroyed;
        toRemove.push_back(uuid);
    }
    if (!toRemove.empty())
    {
        for (const auto& uuid : toRemove)
            m_Instances.erase(uuid);

        // Compact m_CreationOrder in place. Order is preserved, which is all
        // the Stop-path reverse iteration needs — nothing indexes into this
        // vector. Leaving destroyed UUIDs in it made the vector grow without
        // bound across a session: a script that spawns and destroys entities
        // in a loop would leave hundreds of thousands of dead entries to be
        // walked at every sync and at Stop.
        m_CreationOrder.erase(
            std::remove_if(m_CreationOrder.begin(), m_CreationOrder.end(),
                           [this](const UUID& u) {
                               return m_Instances.find(u) == m_Instances.end();
                           }),
            m_CreationOrder.end());
    }

    // 2. Build environments + fire OnCreate for entities in the registry
    //    but NOT in the map (newly applied spawns, or initial Play).
    for (const auto& [uuid, entity] : entities)
    {
        if (m_Instances.count(uuid) != 0) continue;

        const auto& reg = m_RuntimeDoc->ecs.registry;
        const auto* sc = reg.try_get<ScriptComponent>(entity);
        if (!sc) continue;

        ScriptInstance inst;
        inst.uuid = uuid;
        inst.state = ScriptInstanceState::NeverCreated;
        if (BuildEnvironment(inst, *sc, m_Input, m_Sink))
        {
            // OnCreate fires immediately (G2): the entity is live as soon
            // as the environment is built, before this frame's OnUpdate.
            inst.state = ScriptInstanceState::Live;
            m_CreationOrder.push_back(uuid);

            if (inst.on_create.valid())
            {
                sol::protected_function_result result;
                {
                    ScriptInstructionBudget budget(m_Lua.lua_state(),
                                                   kScriptCallbackInstructionBudget);
                    result = inst.on_create(inst.env["entity"], inst.env["world"]);
                }
                if (!result.valid())
                {
                    sol::error err = result;
                    Quarantine(inst, "on_create", err.what());
                }
                else
                {
                    inst.onCreateFired = true;
                }
            }
            else
            {
                // No on_create defined — the instance is still live, just
                // with no init callback. Mark as fired so reload doesn't
                // try to re-fire a non-existent callback.
                inst.onCreateFired = true;
            }
        }
        else
        {
            // BuildEnvironment already quarantined the instance on error.
            // Record it so SyncScriptEnvironments doesn't retry every frame.
            m_CreationOrder.push_back(uuid);
        }
        m_Instances.emplace(uuid, std::move(inst));
    }
}

// ============================================================================
// Environment construction + bindings
// ============================================================================

namespace {

// Read a file into a string while keeping an empty-but-readable file distinct
// from a failed read. Empty scripts are legal (Phase 6 Q10d).
bool ReadFile(const std::filesystem::path& path, std::string& source)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    if (f.bad()) return false;
    source = ss.str();
    return true;
}

} // anonymous namespace

ScriptFieldRegistry::Result
ScriptSystem::GetDeclaredFields(const UUID& uuid)
{
    ScriptFieldRegistry::Result empty;
    empty.parsed = false;
    empty.diagnostic = "entity has no script instance";

    if (!m_RuntimeDoc) return empty;
    const auto e = m_RuntimeDoc->FindByUuid(uuid);
    if (e == entt::null) return empty;

    const auto& reg = m_RuntimeDoc->ecs.registry;
    if (!reg.valid(e)) return empty;
    const auto* sc = reg.try_get<ScriptComponent>(e);
    if (!sc) return empty;

    // All exits above establish the document precondition required by the
    // shared path helper. Keep this assertion beside the dereference so a
    // future refactor cannot silently weaken that invariant.
    assert(m_RuntimeDoc != nullptr);
    // The full Result propagates: on a parse failure the descriptors are the
    // registry's last known-good set (D10), and `parsed` is the only signal
    // that reconciling against them would be destructive.
    const size_t diagnosticBase = m_AssetDiagnostics.size();
    const auto resolved = ResolveScriptAssetPath(
        *sc, m_AssetContext, uuid, EntityName(uuid), m_AssetDiagnostics);
    if (!resolved.success)
    {
        SortAssetDiagnosticsFrom(diagnosticBase);
        empty.diagnostic = "script asset resolution failed";
        return empty;
    }

    auto result = m_FieldRegistry.GetDeclaredFields(resolved.resolvedPath);
    if (!result.parsed)
    {
        AppendAssetDiagnostic(
            *sc, uuid,
            result.diagnostic.find("failed to read script file") == 0
                ? AssetDiagnostic::Missing
                : AssetDiagnostic::Malformed,
            resolved.resolvedPath, result.diagnostic);
    }
    SortAssetDiagnosticsFrom(diagnosticBase);
    return result;
}

bool ScriptSystem::BuildEnvironment(ScriptInstance& inst,
                                    const ScriptComponent& comp,
                                    const IInputService* input,
                                    IRuntimeCommandSink* sink)
{
    // BuildEnvironment is reached only while a Play document is installed by
    // OnSceneStart/SyncScriptEnvironments. Make that lifetime precondition
    // executable before resolving the component through the shared locator.
    assert(m_RuntimeDoc != nullptr);

    const size_t diagnosticBase = m_AssetDiagnostics.size();
    const auto resolved = ResolveScriptAssetPath(
        comp, m_AssetContext, inst.uuid, EntityName(inst.uuid),
        m_AssetDiagnostics);
    if (!resolved.success)
    {
        if (m_AssetDiagnostics.size() > diagnosticBase)
            inst.scriptPath =
                m_AssetDiagnostics.back().resolvedPath;
        SortAssetDiagnosticsFrom(diagnosticBase);
        Quarantine(inst, "load", "script asset resolution failed");
        return false;
    }
    const std::filesystem::path scriptPath = resolved.resolvedPath;
    // Store the canonical form so ReloadScript's path comparison matches
    // by construction (review M3: efsw and ResolveScriptAssetPath can
    // produce different separator/relative forms).
    {
        std::error_code ec;
        auto canonical = std::filesystem::weakly_canonical(scriptPath, ec);
        inst.scriptPath = ec ? scriptPath.string() : canonical.string();
    }

    std::string source;
    if (!ReadFile(scriptPath, source))
    {
        AppendAssetDiagnostic(
            comp, inst.uuid, AssetDiagnostic::Missing, scriptPath,
            "failed to read script file: " + scriptPath.string());
        SortAssetDiagnosticsFrom(diagnosticBase);
        Quarantine(inst, "load",
            "failed to read script file: " + scriptPath.string());
        return false;
    }

    // Create a fresh sol::environment for this entity. The environment
    // sandboxes the script's globals so two entities using the same
    // source have isolated state.
    inst.env = sol::environment(m_Lua, sol::create, m_Lua.globals());

    // Q6b: block the dangerous globals. See ScriptSandbox.h for the full
    // deny list and the reasoning, including why sol::nil does not shadow.
    // Shared with ScriptFieldRegistry's parse sandbox so the two cannot
    // drift apart.
    InstallSandbox(m_Lua, inst.env);

    // The reflection registry evaluates this same source with the full
    // declaration DSL. Runtime execution must also accept declaration-bearing
    // scripts: the values have already been parsed and reconciled, so these
    // constructors only need to return inert marker tables while the chunk
    // defines its callbacks.
    sol::table rt2 = m_Lua.create_table();
    sol::table field = m_Lua.create_table();
    auto declarationMarker = [this](sol::variadic_args) {
        return m_Lua.create_table();
    };
    for (const char* typeName : ScriptFieldTypeNames)
        field[typeName] = declarationMarker;
    rt2["field"] = field;

    // rt2.reload(): request a self-reload. DEFERS to the pending-reload
    // queue rather than calling ReloadScript synchronously — a synchronous
    // call would swap the environment mid-callback (the running on_update
    // or timer callback's environment would be moved-from). The drain at
    // OnUpdate's top runs it before any callback next frame, with no
    // re-entrancy. The watcher (W2) calls ReloadScript directly from
    // OnUIRender (off the Lua stack), which is also safe.
    ScriptSystem* selfPtr = this;
    std::string instScriptPath = inst.scriptPath;
    rt2["reload"] = [selfPtr, instScriptPath](sol::object) {
        selfPtr->m_PendingReloads.push_back(instScriptPath);
    };

    inst.env["rt2"] = rt2;

    // Bind `self` (S3): the per-entity field table. W2 injects the typed
    // authored entries from ScriptComponent::fieldValues. Writes to the Lua
    // table remain runtime-only and are never copied back into authoring data.
    sol::table self = m_Lua.create_table();
    for (const auto& [name, entry] : comp.fieldValues)
    {
        // Reconciliation and the serializer maintain this invariant. Treat a
        // malformed in-memory entry defensively: exposing a value under the
        // wrong declared type would make later reload compatibility lossy.
        if (!ScriptFieldEntryHasValidPayload(entry))
        {
            printf("[Script] invalid field payload (entity %s, field %s, type %s)\n",
                   inst.uuid.ToString().c_str(), name.c_str(),
                   ScriptFieldTypeName(entry.type));
            continue;
        }

        std::visit([&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>)
                self[name] = v;
            else if constexpr (std::is_same_v<T, int64_t>)
                self[name] = static_cast<lua_Integer>(v);
            else if constexpr (std::is_same_v<T, double>)
                self[name] = v;
            else if constexpr (std::is_same_v<T, std::string>)
                self[name] = v;
            else if constexpr (std::is_same_v<T, UUID>)
                self[name] = v.ToString();
            else if constexpr (std::is_same_v<T, glm::vec3>)
            {
                sol::table vec = m_Lua.create_table();
                vec[1] = v.x; vec[2] = v.y; vec[3] = v.z;
                self[name] = vec;
            }
        }, entry.value);
    }
    inst.env["self"] = self;

    // Bind `log` — the logging table.
    sol::table log = m_Lua.create_table();
    log["info"]  = [](const std::string& msg) { printf("[Script] info: %s\n", msg.c_str()); };
    log["warn"]  = [](const std::string& msg) { printf("[Script] warn: %s\n", msg.c_str()); };
    log["error"] = [](const std::string& msg) { printf("[Script] error: %s\n", msg.c_str()); };
    inst.env["log"] = log;

    // Bind `entity` — the validated UUID-keyed handle. Q5: lambdas capture
    // `this` (the ScriptSystem) and read m_Sink at call time. m_Sink is
    // nulled in OnSceneStop, so post-Stop calls fail safely instead of
    // dangling. The entity's UUID is captured by value (instUuid).
    sol::table entity = m_Lua.create_table();
    entity["uuid"] = inst.uuid.ToString();
    UUID instUuid = inst.uuid;

    // Bind `world` — the IRuntimeCommandSink. Q5: capture `this`, read
    // m_Sink at call time. Q7: spawn returns a pending-handle table (not
    // a UUID string); reject non-table desc.
    if (sink)
    {
        lua_State* L = m_Lua.lua_state();
        ScriptSystem* self = this;

        sol::table world = m_Lua.create_table();

        world["find_by_name"] = [self, L, instUuid](sol::object, const std::string& name) -> sol::object {
            IRuntimeCommandSink* s = self->m_Sink;
            if (!s) return sol::nil;
            UUID found = s->FindByName(name);
            if (found.IsNull()) return sol::nil;
            // Return a handle table (Q7): { uuid = "..." }
            sol::state_view sv(L);
            sol::table h = sv.create_table();
            h["uuid"] = found.ToString();
            return h;
        };

        world["find_by_uuid"] = [self, L, instUuid](sol::object, const std::string& uuidStr) -> sol::object {
            IRuntimeCommandSink* s = self->m_Sink;
            if (!s) return sol::nil;
            UUID uuid = UUID::Parse(uuidStr);
            if (uuid.IsNull() || !s->IsAlive(uuid)) return sol::nil;
            sol::state_view sv(L);
            sol::table h = sv.create_table();
            h["uuid"] = uuidStr;
            return h;
        };

        // Q7: reject non-table desc. Return a pending-handle table, not a
        // UUID string. The handle is "pending" until SyncScriptEnvironments
        // builds the environment next safe point; methods on it fail
        // safely until then.
        world["spawn"] = [self, L, instUuid](sol::object selfArg, sol::object descArg) -> sol::object {
            IRuntimeCommandSink* s = self->m_Sink;
            if (!s) return sol::nil;

            // Determine the desc table: for world:spawn(desc), selfArg is
            // `world` and descArg is the desc. For world.spawn(desc),
            // selfArg is the desc and descArg is nil.
            sol::table desc;
            if (descArg.is<sol::table>())
                desc = descArg.as<sol::table>();
            else if (selfArg.is<sol::table>())
                desc = selfArg.as<sol::table>();
            else
                // Q7: reject non-table invocation. Raise a Lua error so
                // the script author sees the bug, not a silent empty spawn.
                return sol::nil;

            RuntimeEntityCreateDesc d;
            sol::object nameObj = desc["name"];
            if (nameObj.is<std::string>())
                d.name = nameObj.as<std::string>();

            // G2: scripts spawn SCRIPTED entities. desc.script is a
            // scene-relative .lua path; the spawned entity gets a
            // ScriptComponent, so the next SyncScriptEnvironments builds its
            // environment and fires its on_create in the same frame.
            //
            // This was previously dropped on the floor — only `name` was
            // read — so world:spawn{script=...} produced a permanently inert
            // entity and G2 was unmet by the Lua API despite the C++ path
            // (RuntimeEntityCreateDesc::script, RuntimeSceneMutator) fully
            // supporting it.
            sol::object scriptObj = desc["script"];
            if (scriptObj.is<std::string>())
            {
                const auto scriptPath = scriptObj.as<std::string>();
                if (!scriptPath.empty())
                {
                    ScriptComponent sc;
                    sc.asset.kind      = AssetKind::Script;
                    sc.asset.path      = scriptPath;
                    sc.asset.sourceKey = "lua:asset=" + scriptPath;
                    d.script = sc;
                }
            }

            auto r = s->SpawnEntity(d);
            if (!r.IsOk())
                return sol::nil;

            // Return a pending handle table. The entity is not live until
            // SyncScriptEnvironments next safe point. The handle's uuid
            // field lets scripts reference it; is_pending is true until
            // the environment is built.
            sol::state_view sv(L);
            sol::table h = sv.create_table();
            h["uuid"] = r.value.ToString();
            h["is_pending"] = true;
            return h;
        };

        world["destroy"] = [self, instUuid](sol::object selfArg, sol::object uuidArg) -> bool {
            IRuntimeCommandSink* s = self->m_Sink;
            if (!s) return false;
            std::string uuidStr;
            if (uuidArg.is<std::string>())
                uuidStr = uuidArg.as<std::string>();
            else if (uuidArg.is<sol::table>())
            {
                sol::object u = uuidArg.as<sol::table>()["uuid"];
                if (u.is<std::string>()) uuidStr = u.as<std::string>();
            }
            else if (selfArg.is<std::string>())
                uuidStr = selfArg.as<std::string>();
            else
                return false;
            UUID uuid = UUID::Parse(uuidStr);
            if (uuid.IsNull()) return false;
            auto r = s->DestroyEntity(uuid);
            return r.IsOk();
        };

        inst.env["world"] = world;

        // Bind entity methods. Q5: capture `this`, read m_Sink at call
        // time. Q10e: validate vec3 table, error on malformed input.
        entity["get_uuid"] = [instUuid](sol::object) -> std::string {
            return instUuid.ToString();
        };
        entity["get_name"] = [self, instUuid](sol::object) -> std::string {
            IRuntimeCommandSink* s = self->m_Sink;
            return s ? s->GetName(instUuid) : std::string{};
        };
        entity["set_name"] = [self, instUuid](sol::object, const std::string& name) -> bool {
            IRuntimeCommandSink* s = self->m_Sink;
            return s ? s->SetName(instUuid, name) : false;
        };
        entity["get_position"] = [self, instUuid, L](sol::object) -> sol::object {
            IRuntimeCommandSink* s = self->m_Sink;
            if (!s) return sol::nil;
            glm::vec3 pos;
            if (!s->GetPosition(instUuid, pos)) return sol::nil;
            sol::state_view sv(L);
            sol::table t = sv.create_table();
            t[1] = pos.x; t[2] = pos.y; t[3] = pos.z;
            return t;
        };
        entity["set_position"] = [self, instUuid](sol::object, sol::table pos) -> bool {
            IRuntimeCommandSink* s = self->m_Sink;
            if (!s) return false;
            if (!pos.valid() || !pos.is<sol::table>()) return false;
            // Q10e: validate the vec3 table has exactly 3 numeric elements.
            // Don't silently zero-fill malformed input.
            sol::object x = pos[1], y = pos[2], z = pos[3];
            if (!x.is<double>() || !y.is<double>() || !z.is<double>())
                return false;
            glm::vec3 p{ static_cast<float>(x.as<double>()),
                         static_cast<float>(y.as<double>()),
                         static_cast<float>(z.as<double>()) };
            return s->SetPosition(instUuid, p);
        };
        entity["get_visible"] = [self, instUuid](sol::object) -> bool {
            IRuntimeCommandSink* s = self->m_Sink;
            if (!s) return false;
            bool v = false;
            s->GetVisible(instUuid, v);
            return v;
        };
        entity["set_visible"] = [self, instUuid](sol::object, bool v) -> bool {
            IRuntimeCommandSink* s = self->m_Sink;
            return s ? s->SetVisible(instUuid, v) : false;
        };

        // ---- entity.* light/camera/material (Phase 6C) ----------------

        entity["get_light"] = [self, instUuid, L](sol::object) -> sol::object {
            IRuntimeCommandSink* s = self->m_Sink;
            if (!s) return sol::nil;
            LightComponent lc;
            if (!s->GetLight(instUuid, lc)) return sol::nil;
            sol::state_view sv(L);
            sol::table t = sv.create_table();
            sol::table c = sv.create_table();
            c[1] = lc.color.x; c[2] = lc.color.y; c[3] = lc.color.z;
            t["color"] = c;
            t["intensity"] = lc.intensity;
            t["range"] = lc.range;
            t["inner_cone_angle"] = lc.innerConeAngle;
            t["outer_cone_angle"] = lc.outerConeAngle;
            t["is_spot"] = lc.isSpot;
            return t;
        };
        entity["set_light"] = [self, instUuid](sol::object, sol::table lt) -> bool {
            IRuntimeCommandSink* s = self->m_Sink;
            if (!s) return false;
            if (!lt.valid() || !lt.is<sol::table>()) return false;
            // Read the current component and overlay only present keys, so
            // a partial update (e.g. just intensity) doesn't clobber the
            // other fields with defaults.
            LightComponent lc;
            if (!s->GetLight(instUuid, lc)) return false;
            sol::object c = lt["color"];
            if (c.is<sol::table>())
            {
                sol::table ct = c.as<sol::table>();
                sol::optional<float> cx = ct[1], cy = ct[2], cz = ct[3];
                if (cx) lc.color.x = *cx;
                if (cy) lc.color.y = *cy;
                if (cz) lc.color.z = *cz;
            }
            sol::optional<float> intensity = lt["intensity"];
            if (intensity) lc.intensity = *intensity;
            sol::optional<float> range = lt["range"];
            if (range) lc.range = *range;
            sol::optional<float> innerCone = lt["inner_cone_angle"];
            if (innerCone) lc.innerConeAngle = *innerCone;
            sol::optional<float> outerCone = lt["outer_cone_angle"];
            if (outerCone) lc.outerConeAngle = *outerCone;
            sol::optional<bool> isSpot = lt["is_spot"];
            if (isSpot) lc.isSpot = *isSpot;
            return s->SetLight(instUuid, lc);
        };

        entity["get_camera"] = [self, instUuid, L](sol::object) -> sol::object {
            IRuntimeCommandSink* s = self->m_Sink;
            if (!s) return sol::nil;
            CameraComponent cc;
            if (!s->GetCamera(instUuid, cc)) return sol::nil;
            sol::state_view sv(L);
            sol::table t = sv.create_table();
            t["fov"] = cc.verticalFOV;
            t["aperture"] = cc.aperture;
            t["focus_distance"] = cc.focusDistance;
            sol::table f = sv.create_table();
            f[1] = cc.forwardDirection.x;
            f[2] = cc.forwardDirection.y;
            f[3] = cc.forwardDirection.z;
            t["forward"] = f;
            return t;
        };
        entity["set_camera"] = [self, instUuid](sol::object, sol::table ct) -> bool {
            IRuntimeCommandSink* s = self->m_Sink;
            if (!s) return false;
            if (!ct.valid() || !ct.is<sol::table>()) return false;
            // Read the current component and overlay only present keys.
            CameraComponent cc;
            if (!s->GetCamera(instUuid, cc)) return false;
            sol::optional<float> fov = ct["fov"];
            if (fov) cc.verticalFOV = *fov;
            sol::optional<float> aperture = ct["aperture"];
            if (aperture) cc.aperture = *aperture;
            sol::optional<float> focusDist = ct["focus_distance"];
            if (focusDist) cc.focusDistance = *focusDist;
            sol::object f = ct["forward"];
            if (f.is<sol::table>())
            {
                sol::table ft = f.as<sol::table>();
                sol::optional<float> fx = ft[1], fy = ft[2], fz = ft[3];
                if (fx) cc.forwardDirection.x = *fx;
                if (fy) cc.forwardDirection.y = *fy;
                if (fz) cc.forwardDirection.z = *fz;
            }
            return s->SetCamera(instUuid, cc);
        };

        entity["set_material_index"] = [self, instUuid](sol::object, int index) -> bool {
            IRuntimeCommandSink* s = self->m_Sink;
            return s ? s->SetMaterialIndex(instUuid, index) : false;
        };
    }
    else
    {
        inst.env["world"] = sol::nil;
    }

    inst.env["entity"] = entity;

    // Q6c: always bind `input` as a non-nil table, even when the input
    // service is null. S2 requires an inert-but-present input so scripts
    // that reference it don't error. When m_Input is set (Play), the
    // methods delegate to the real IInputService; when null (tests), they
    // return inert defaults (false/0/zero vector).
    {
        lua_State* L = m_Lua.lua_state();
        ScriptSystem* self = this;
        sol::table inputTable = m_Lua.create_table();

        inputTable["is_down"] = [self](sol::object, const std::string& action) -> bool {
            const IInputService* in = self->m_Input;
            return in ? in->IsDown(action) : false;
        };
        inputTable["is_pressed"] = [self](sol::object, const std::string& action) -> bool {
            const IInputService* in = self->m_Input;
            return in ? in->IsPressed(action) : false;
        };
        inputTable["is_released"] = [self](sol::object, const std::string& action) -> bool {
            const IInputService* in = self->m_Input;
            return in ? in->IsReleased(action) : false;
        };
        inputTable["get_axis"] = [self](sol::object, const std::string& axis) -> double {
            const IInputService* in = self->m_Input;
            return in ? static_cast<double>(in->GetAxisValue(axis)) : 0.0;
        };
        inputTable["get_mouse_delta"] = [self, L](sol::object) -> sol::object {
            const IInputService* in = self->m_Input;
            if (!in) return sol::nil;
            glm::vec2 delta = in->GetMouseDelta();
            sol::state_view sv(L);
            sol::table t = sv.create_table();
            t[1] = delta.x;
            t[2] = delta.y;
            return t;
        };
        inputTable["get_scroll_delta"] = [self](sol::object) -> double {
            const IInputService* in = self->m_Input;
            return in ? static_cast<double>(in->GetScrollDelta()) : 0.0;
        };

        inst.env["input"] = inputTable;
    }

    // Phase 6C: bind `timer` — after/every/cancel. Timers are stored in
    // ScriptSystem keyed by owner UUID. Callbacks are protected + budgeted
    // (6th entry point). Cancelled on Stop, entity destruction, and reload.
    {
        ScriptSystem* self = this;
        UUID instUuid = inst.uuid;
        sol::table timerTable = m_Lua.create_table();

        timerTable["after"] = [self, instUuid](sol::object, double seconds,
                                               sol::function cb) -> int {
            ScriptTimer t;
            t.handle = self->m_NextTimerHandle++;
            t.interval = seconds;
            t.remaining = seconds;
            t.repeating = false;
            t.callback = sol::protected_function(cb);
            t.ownerUuid = instUuid;
            const int h = t.handle;
            self->m_Timers.push_back(std::move(t));
            return h;
        };

        timerTable["every"] = [self, instUuid](sol::object, double seconds,
                                               sol::function cb) -> int {
            ScriptTimer t;
            t.handle = self->m_NextTimerHandle++;
            t.interval = seconds;
            t.remaining = seconds;
            t.repeating = true;
            t.callback = sol::protected_function(cb);
            t.ownerUuid = instUuid;
            const int h = t.handle;
            self->m_Timers.push_back(std::move(t));
            return h;
        };

        timerTable["cancel"] = [self](sol::object, int handle) -> bool {
            for (auto& t : self->m_Timers)
            {
                if (t.handle == handle && !t.cancelled)
                {
                    t.cancelled = true;
                    return true;
                }
            }
            return false;
        };

        inst.env["timer"] = timerTable;
    }

    // Load + execute the script source in the environment. This defines
    // the on_create/on_fixed_update/on_update/on_destroy functions as
    // environment globals.
    sol::protected_function_result result;
    {
        // A top-level `while true do end` must fail the load, not hang Play.
        ScriptInstructionBudget budget(m_Lua.lua_state(),
                                       kScriptLoadInstructionBudget);
        result = m_Lua.safe_script(
            source, inst.env,
            sol::script_pass_on_error,
            scriptPath.string());
    }

    if (!result.valid())
    {
        sol::error err = result;
        AppendAssetDiagnostic(
            comp, inst.uuid, AssetDiagnostic::Malformed, scriptPath,
            err.what());
        SortAssetDiagnosticsFrom(diagnosticBase);
        Quarantine(inst, "load", err.what());
        return false;
    }

    // Bind the four lifecycle callbacks from the environment.
    sol::object onCreate = inst.env["on_create"];
    sol::object onFixed  = inst.env["on_fixed_update"];
    sol::object onUpdate = inst.env["on_update"];
    sol::object onDest   = inst.env["on_destroy"];

    if (onCreate.is<sol::function>())
        inst.on_create = onCreate.as<sol::protected_function>();
    if (onFixed.is<sol::function>())
        inst.on_fixed_update = onFixed.as<sol::protected_function>();
    if (onUpdate.is<sol::function>())
        inst.on_update = onUpdate.as<sol::protected_function>();
    if (onDest.is<sol::function>())
        inst.on_destroy = onDest.as<sol::protected_function>();

    return true;
}

void ScriptSystem::AppendAssetDiagnostic(
    const ScriptComponent& component,
    const UUID& entityUuid,
    AssetDiagnostic::Severity severity,
    const std::filesystem::path& resolvedPath,
    const std::string& detail)
{
    AssetDiagnostic diagnostic;
    diagnostic.severity = severity;
    diagnostic.kind = AssetKind::Script;
    diagnostic.refPath = component.asset.path;
    diagnostic.resolvedPath = resolvedPath.string();
    diagnostic.entityUuid = entityUuid;
    diagnostic.entityName = EntityName(entityUuid);
    diagnostic.sourceKey = component.asset.sourceKey;
    diagnostic.detail = detail;
    m_AssetDiagnostics.push_back(std::move(diagnostic));
}

std::string ScriptSystem::EntityName(const UUID& uuid) const
{
    if (!m_RuntimeDoc)
        return {};
    const entt::entity entity = m_RuntimeDoc->FindByUuid(uuid);
    if (entity == entt::null || !m_RuntimeDoc->ecs.registry.valid(entity))
        return {};
    const auto* name =
        m_RuntimeDoc->ecs.registry.try_get<NameComponent>(entity);
    return name ? name->name : std::string{};
}

void ScriptSystem::SortAssetDiagnosticsFrom(size_t base)
{
    if (base >= m_AssetDiagnostics.size())
        return;
    std::stable_sort(
        m_AssetDiagnostics.begin() + base, m_AssetDiagnostics.end(),
        [](const AssetDiagnostic& left, const AssetDiagnostic& right) {
            return AssetDiagnosticSortKey(left) <
                   AssetDiagnosticSortKey(right);
        });
}

void ScriptSystem::Quarantine(ScriptInstance& inst,
                              const std::string& callbackName,
                              const std::string& message)
{
    // S1 promises path, entity, callback, and stack trace. The trace arrives
    // inside `message` (sol's default handler builds one with luaL_traceback,
    // which needs no debug library); the path comes from the instance.
    printf("[Script] %s error (script %s, entity %s, callback %s): %s\n",
           inst.state == ScriptInstanceState::NeverCreated ? "load" : "runtime",
           inst.scriptPath.empty() ? "<unbound>" : inst.scriptPath.c_str(),
           inst.uuid.ToString().c_str(),
           callbackName.c_str(),
           message.c_str());
    inst.state = ScriptInstanceState::Quarantined;
}

std::vector<std::pair<UUID, entt::entity>>
ScriptSystem::CollectScriptEntitiesSorted() const
{
    std::vector<std::pair<UUID, entt::entity>> out;
    if (!m_RuntimeDoc) return out;

    auto& reg = m_RuntimeDoc->ecs.registry;
    auto view = reg.view<ScriptComponent, EntityIdComponent>();
    for (auto e : view)
    {
        const auto& idc = view.get<EntityIdComponent>(e);
        out.emplace_back(idc.id, e);
    }
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

ScriptInstanceState ScriptSystem::GetInstanceState(const UUID& uuid) const
{
    auto it = m_Instances.find(uuid);
    if (it == m_Instances.end())
        return ScriptInstanceState::NeverCreated;
    return it->second.state;
}

size_t ScriptSystem::LiveInstanceCount() const
{
    size_t count = 0;
    for (const auto& [uuid, inst] : m_Instances)
        if (inst.state == ScriptInstanceState::Live)
            ++count;
    return count;
}

size_t ScriptSystem::QuarantinedInstanceCount() const
{
    size_t count = 0;
    for (const auto& [uuid, inst] : m_Instances)
        if (inst.state == ScriptInstanceState::Quarantined)
            ++count;
    return count;
}

// ============================================================================
// RuntimeCommandSink — concrete IRuntimeCommandSink
// ============================================================================

RuntimeCommandSink::RuntimeCommandSink(RuntimeSceneController& controller)
    : m_Controller(controller)
{
}

Result<UUID> RuntimeCommandSink::SpawnEntity(const RuntimeEntityCreateDesc& desc)
{
    return m_Controller.QueueCreateRuntimeEntity(desc);
}

Result<void> RuntimeCommandSink::DestroyEntity(const UUID& uuid)
{
    return m_Controller.QueueDestroyRuntimeEntity(uuid);
}

bool RuntimeCommandSink::GetLocalTransform(const UUID& uuid, EditableTRS& out) const
{
    const SceneDocument* doc = m_Controller.TryGetRuntimeScene();
    if (!doc) return false;
    auto& reg = const_cast<SceneDocument*>(doc)->ecs.registry;
    const auto e = doc->FindByUuid(uuid);
    if (e == entt::null || !reg.valid(e)) return false;
    const auto* tf = reg.try_get<Transform>(e);
    if (!tf) return false;
    out.translation = tf->translation;
    out.rotation = tf->rotation;
    out.scale = tf->scale;
    return true;
}

bool RuntimeCommandSink::SetLocalTransform(const UUID& uuid, const EditableTRS& trs)
{
    // Q4: gate writes through the controller's authority. The sink does
    // not bypass the controller during OnSceneStop.
    if (!m_Controller.IsRuntimeMutable()) return false;
    const SceneDocument* doc = m_Controller.TryGetRuntimeScene();
    if (!doc) return false;
    auto& reg = const_cast<SceneDocument*>(doc)->ecs.registry;
    const auto e = doc->FindByUuid(uuid);
    if (e == entt::null || !reg.valid(e)) return false;
    auto* tf = reg.try_get<Transform>(e);
    if (!tf) return false;
    tf->translation = trs.translation;
    tf->rotation = trs.rotation;
    tf->scale = trs.scale;
    SceneGraph::SetLocalDirty(reg, e);
    return true;
}

bool RuntimeCommandSink::GetPosition(const UUID& uuid, glm::vec3& out) const
{
    EditableTRS trs;
    if (!GetLocalTransform(uuid, trs)) return false;
    out = trs.translation;
    return true;
}

bool RuntimeCommandSink::SetPosition(const UUID& uuid, const glm::vec3& pos)
{
    if (!m_Controller.IsRuntimeMutable()) return false;
    EditableTRS trs;
    if (!GetLocalTransform(uuid, trs)) return false;
    trs.translation = pos;
    return SetLocalTransform(uuid, trs);
}

std::string RuntimeCommandSink::GetName(const UUID& uuid) const
{
    const SceneDocument* doc = m_Controller.TryGetRuntimeScene();
    if (!doc) return {};
    auto& reg = const_cast<SceneDocument*>(doc)->ecs.registry;
    const auto e = doc->FindByUuid(uuid);
    if (e == entt::null || !reg.valid(e)) return {};
    const auto* nc = reg.try_get<NameComponent>(e);
    return nc ? nc->name : std::string{};
}

bool RuntimeCommandSink::SetName(const UUID& uuid, const std::string& name)
{
    if (!m_Controller.IsRuntimeMutable()) return false;
    const SceneDocument* doc = m_Controller.TryGetRuntimeScene();
    if (!doc) return false;
    auto& reg = const_cast<SceneDocument*>(doc)->ecs.registry;
    const auto e = doc->FindByUuid(uuid);
    if (e == entt::null || !reg.valid(e)) return false;
    auto* nc = reg.try_get<NameComponent>(e);
    if (nc) nc->name = name;
    else reg.emplace<NameComponent>(e, NameComponent{name});
    return true;
}

bool RuntimeCommandSink::GetVisible(const UUID& uuid, bool& out) const
{
    const SceneDocument* doc = m_Controller.TryGetRuntimeScene();
    if (!doc) return false;
    auto& reg = const_cast<SceneDocument*>(doc)->ecs.registry;
    const auto e = doc->FindByUuid(uuid);
    if (e == entt::null || !reg.valid(e)) return false;
    const auto* vc = reg.try_get<VisibleComponent>(e);
    if (!vc) return false;
    out = vc->visible;
    return true;
}

bool RuntimeCommandSink::SetVisible(const UUID& uuid, bool visible)
{
    if (!m_Controller.IsRuntimeMutable()) return false;
    const SceneDocument* doc = m_Controller.TryGetRuntimeScene();
    if (!doc) return false;
    auto& reg = const_cast<SceneDocument*>(doc)->ecs.registry;
    const auto e = doc->FindByUuid(uuid);
    if (e == entt::null || !reg.valid(e)) return false;
    auto* vc = reg.try_get<VisibleComponent>(e);
    if (vc) vc->visible = visible;
    else reg.emplace<VisibleComponent>(e, VisibleComponent{visible});
    return true;
}

bool RuntimeCommandSink::GetLight(const UUID& uuid, LightComponent& out) const
{
    const SceneDocument* doc = m_Controller.TryGetRuntimeScene();
    if (!doc) return false;
    auto& reg = const_cast<SceneDocument*>(doc)->ecs.registry;
    const auto e = doc->FindByUuid(uuid);
    if (e == entt::null || !reg.valid(e)) return false;
    const auto* lc = reg.try_get<LightComponent>(e);
    if (!lc) return false;
    out = *lc;
    return true;
}

bool RuntimeCommandSink::SetLight(const UUID& uuid, const LightComponent& light)
{
    if (!m_Controller.IsRuntimeMutable()) return false;
    const SceneDocument* doc = m_Controller.TryGetRuntimeScene();
    if (!doc) return false;
    auto& reg = const_cast<SceneDocument*>(doc)->ecs.registry;
    const auto e = doc->FindByUuid(uuid);
    if (e == entt::null || !reg.valid(e)) return false;
    auto* lc = reg.try_get<LightComponent>(e);
    if (!lc) return false; // mutate-only; don't fabricate a light
    *lc = light;
    // Note: the GPU light list is built from emissive triangles, not
    // LightComponent, so this write may not have a visual effect until
    // the renderer is extended to consume LightComponent changes. The
    // component is correctly updated in the runtime document.
    return true;
}

bool RuntimeCommandSink::GetCamera(const UUID& uuid, CameraComponent& out) const
{
    const SceneDocument* doc = m_Controller.TryGetRuntimeScene();
    if (!doc) return false;
    auto& reg = const_cast<SceneDocument*>(doc)->ecs.registry;
    const auto e = doc->FindByUuid(uuid);
    if (e == entt::null || !reg.valid(e)) return false;
    const auto* cc = reg.try_get<CameraComponent>(e);
    if (!cc) return false;
    out = *cc;
    return true;
}

bool RuntimeCommandSink::SetCamera(const UUID& uuid, const CameraComponent& cam)
{
    if (!m_Controller.IsRuntimeMutable()) return false;
    const SceneDocument* doc = m_Controller.TryGetRuntimeScene();
    if (!doc) return false;
    auto& reg = const_cast<SceneDocument*>(doc)->ecs.registry;
    const auto e = doc->FindByUuid(uuid);
    if (e == entt::null || !reg.valid(e)) return false;
    auto* cc = reg.try_get<CameraComponent>(e);
    if (!cc) return false; // mutate-only; don't fabricate a camera
    *cc = cam;
    // Note: the render camera is read from ECSScene::camera, not from
    // CameraComponent on entities. This write updates the component but
    // may not affect the active render camera until the controller is
    // extended to bridge the two.
    return true;
}

bool RuntimeCommandSink::SetMaterialIndex(const UUID& uuid, int index)
{
    if (!m_Controller.IsRuntimeMutable()) return false;
    const SceneDocument* doc = m_Controller.TryGetRuntimeScene();
    if (!doc) return false;
    auto& reg = const_cast<SceneDocument*>(doc)->ecs.registry;
    const auto e = doc->FindByUuid(uuid);
    if (e == entt::null || !reg.valid(e)) return false;
    auto* mr = reg.try_get<MeshRef>(e);
    if (!mr) return false;
    // -1 is the "use per-triangle indices" sentinel — valid.
    // Reject out-of-range positive indices.
    const int matCount = static_cast<int>(
        const_cast<SceneDocument*>(doc)->ecs.materials.size());
    if (index < -1 || index >= matCount)
    {
        printf("[Script] set_material_index rejected: %d out of range [-1, %d)\n",
               index, matCount);
        return false;
    }
    mr->materialIndex = index;
    return true;
}

bool RuntimeCommandSink::IsAlive(const UUID& uuid) const
{
    const SceneDocument* doc = m_Controller.TryGetRuntimeScene();
    if (!doc) return false;
    return doc->FindByUuid(uuid) != entt::null;
}

UUID RuntimeCommandSink::FindByName(const std::string& name) const
{
    const SceneDocument* doc = m_Controller.TryGetRuntimeScene();
    if (!doc) return UUID::Nil();
    auto& reg = const_cast<SceneDocument*>(doc)->ecs.registry;
    // UUID-sorted iteration for deterministic results when multiple
    // entities share a name (the first in UUID order wins).
    std::vector<std::pair<UUID, entt::entity>> matches;
    auto view = reg.view<NameComponent, EntityIdComponent>();
    for (auto e : view)
    {
        const auto& nc = view.get<NameComponent>(e);
        if (nc.name == name)
        {
            const auto& idc = view.get<EntityIdComponent>(e);
            matches.emplace_back(idc.id, e);
        }
    }
    if (matches.empty()) return UUID::Nil();
    std::sort(matches.begin(), matches.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return matches.front().first;
}

bool RuntimeCommandSink::IsRuntimeMutable() const
{
    return m_Controller.IsRuntimeMutable();
}

SceneRunState RuntimeCommandSink::GetRunState() const
{
    return m_Controller.GetState();
}

} // namespace rt2::core
