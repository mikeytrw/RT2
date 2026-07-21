#include "ScriptSystem.h"
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
#include <fstream>
#include <sstream>

namespace rt2::core {

// ============================================================================
// ScriptSystem construction / destruction
// ============================================================================

ScriptSystem::ScriptSystem(IUuidProvider& uuidProvider)
    : m_UuidProvider(uuidProvider)
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

    // S7: install a panic guard so a Lua panic (e.g. stack overflow via
    // deep recursion) quarantines the instance rather than aborting the
    // process. The panic handler is the last-resort backstop; the normal
    // path is sol::protected_function with a bound error handler.
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
    // Throw a C++ exception that sol2 catches in the protected_call frame.
    // This converts a panic into a runtime error that quarantines the
    // instance instead of aborting.
    throw std::runtime_error(std::string("Lua panic: ") + msg);
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

    // Build environments for every ScriptComponent-bearing entity in the
    // runtime clone. This is the "initial Play" special case of
    // SyncScriptEnvironments (G2): the registry is full, the environment
    // map is empty, so every scripted entity gets OnCreate.
    SyncScriptEnvironments();
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
    m_RuntimeDoc = nullptr;
    m_Input = nullptr;
    m_Sink = nullptr;
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

// Read a file into a string. Returns empty string on failure.
std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // anonymous namespace

std::filesystem::path
ScriptSystem::ResolveScriptPath(const ScriptComponent& comp) const
{
    // Scene-relative when the document has a source path; as-is otherwise
    // (test fixtures use absolute paths).
    if (m_RuntimeDoc && !m_RuntimeDoc->metadata.sourcePath.empty())
        return m_RuntimeDoc->metadata.sourcePath.parent_path() / comp.asset.path;
    return std::filesystem::path(comp.asset.path);
}

bool ScriptSystem::BuildEnvironment(ScriptInstance& inst,
                                    const ScriptComponent& comp,
                                    const IInputService* input,
                                    IRuntimeCommandSink* sink)
{
    if (!comp.asset.IsValid() || comp.asset.kind != AssetKind::Script)
    {
        Quarantine(inst, "load", "invalid or missing script asset reference");
        return false;
    }

    const std::filesystem::path scriptPath = ResolveScriptPath(comp);
    inst.scriptPath = scriptPath.string();

    std::string source = ReadFile(scriptPath);
    // Q10d: an empty file is a legal script (no callbacks defined). Only
    // a failed read (file missing) quarantines. ReadFile returns empty
    // on failure, so we distinguish via filesystem::exists.
    if (source.empty() && !std::filesystem::exists(scriptPath))
    {
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

    // Bind `self` (S3): the per-entity field table. In 6A this is empty
    // (fieldValues is unused). 6B populates it from ScriptComponent::
    // fieldValues on OnCreate. Writes to self are runtime-only and non-
    // persistent.
    sol::table self = m_Lua.create_table();
    for (const auto& [name, value] : comp.fieldValues)
    {
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
        }, value);
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
    }
    else
    {
        inst.env["world"] = sol::nil;
    }

    inst.env["entity"] = entity;

    // Q6c: always bind `input` as a non-nil table, even when the input
    // service is null. S2 requires an inert-but-present input so scripts
    // that reference it don't error. 6C adds the real methods.
    {
        sol::table inputTable = m_Lua.create_table();
        // 6C will add: is_down, is_pressed, is_released, get_axis,
        // get_mouse_delta, get_scroll_delta. 6A leaves it methodless.
        inst.env["input"] = inputTable;
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

} // namespace rt2::core