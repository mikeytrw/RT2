#include "ScriptSystem.h"
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
    // Open standard Lua libraries. Scripts are sandboxed per-environment
    // (sol::environment), but the state itself has full stdlib access for
    // the engine's own setup code.
    m_Lua.open_libraries(sol::lib::base,
                         sol::lib::math,
                         sol::lib::string,
                         sol::lib::table,
                         sol::lib::coroutine,
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
                auto result = inst.on_destroy(inst.env["entity"]);
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
        auto result = inst.on_fixed_update(
            inst.env["entity"], dt, inst.env["input"], inst.env["world"]);
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

        auto result = inst.on_update(
            inst.env["entity"], dt, inst.env["input"], inst.env["world"]);
        if (!result.valid())
        {
            sol::error err = result;
            Quarantine(inst, "on_update", err.what());
        }
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
    //    but NOT in the registry (destroyed at the safe point). Iterate
    //    in creation order (not reverse — this is mid-frame, not Stop).
    std::vector<UUID> toRemove;
    for (const auto& uuid : m_CreationOrder)
    {
        if (registrySet.count(uuid) != 0) continue;
        auto it = m_Instances.find(uuid);
        if (it == m_Instances.end()) continue;
        auto& inst = it->second;
        if (inst.state == ScriptInstanceState::Live)
        {
            if (inst.on_destroy.valid())
            {
                auto result = inst.on_destroy(inst.env["entity"]);
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
    for (const auto& uuid : toRemove)
    {
        m_Instances.erase(uuid);
        // Note: we do NOT remove from m_CreationOrder (that would shift
        // indices). The UUID is simply absent from m_Instances, so the
        // Stop-path reverse iteration skips it.
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
            inst.creationOrderIndex = m_CreationOrder.size();
            m_CreationOrder.push_back(uuid);

            if (inst.on_create.valid())
            {
                auto result = inst.on_create(inst.env["entity"], inst.env["world"]);
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
            inst.creationOrderIndex = m_CreationOrder.size();
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

    // Resolve the script path relative to the runtime document's source
    // path (scene-relative). If the document has no source path, try the
    // path as-is (test fixtures use absolute paths).
    std::filesystem::path scriptPath;
    if (m_RuntimeDoc && !m_RuntimeDoc->metadata.sourcePath.empty())
    {
        scriptPath = m_RuntimeDoc->metadata.sourcePath.parent_path() / comp.asset.path;
    }
    else
    {
        scriptPath = comp.asset.path;
    }

    std::string source = ReadFile(scriptPath);
    if (source.empty())
    {
        Quarantine(inst, "load",
            "failed to read script file: " + scriptPath.string());
        return false;
    }

    // Create a fresh sol::environment for this entity. The environment
    // sandboxes the script's globals so two entities using the same
    // source have isolated state.
    inst.env = sol::environment(m_Lua, sol::create, m_Lua.globals());

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

    // Bind `entity` — the validated UUID-keyed handle. Holds the entity's
    // UUID and a pointer to the sink (S4). After OnDestroy, the sink
    // pointer is nulled so all methods fail safely.
    // The entity handle is a sol::table with metamethods that route to
    // the sink. We build it as a usertype-agnostic table to keep the
    // binding simple.
    sol::table entity = m_Lua.create_table();
    entity["uuid"] = inst.uuid.ToString();
    // The entity methods are bound in the script via the `world` sink and
    // a closure over the UUID. See the bindings below.

    // Bind `world` — the IRuntimeCommandSink. Exposes find_by_name,
    // find_by_uuid, spawn, destroy. The sink pointer is captured by value
    // (it's non-owning, valid for the Play session).
    if (sink)
    {
        IRuntimeCommandSink* sinkPtr = sink;
        lua_State* L = m_Lua.lua_state();
        UUID instUuid = inst.uuid;

        sol::table world = m_Lua.create_table();

        world["find_by_name"] = [sinkPtr, L](const std::string& name) -> sol::object {
            UUID found = sinkPtr->FindByName(name);
            if (found.IsNull())
                return sol::nil;
            // Return the UUID as a string. 6C will return a proper handle.
            return sol::make_object(L, found.ToString());
        };

        world["find_by_uuid"] = [sinkPtr, L](const std::string& uuidStr) -> sol::object {
            UUID uuid = UUID::Parse(uuidStr);
            if (uuid.IsNull() || !sinkPtr->IsAlive(uuid))
                return sol::nil;
            return sol::make_object(L, uuidStr);
        };

        world["spawn"] = [sinkPtr, L](sol::object selfOrDesc, sol::object descObj) -> sol::object {
            // Support both world.spawn(desc) and world:spawn(desc) (the
            // latter passes world as the first arg via Lua's : syntax).
            sol::table desc;
            if (descObj.is<sol::table>())
                desc = descObj.as<sol::table>();
            else if (selfOrDesc.is<sol::table>())
                desc = selfOrDesc.as<sol::table>();

            RuntimeEntityCreateDesc d;
            if (desc.valid())
            {
                sol::object nameObj = desc["name"];
                if (nameObj.is<std::string>())
                    d.name = nameObj.as<std::string>();
            }
            auto r = sinkPtr->SpawnEntity(d);
            if (!r.IsOk())
                return sol::nil;
            // Return the pending UUID as a string. The entity is not live
            // until SyncScriptEnvironments builds its environment next
            // safe point.
            return sol::make_object(L, r.value.ToString());
        };

        world["destroy"] = [sinkPtr](sol::object selfOrUuid, sol::object uuidObj) -> bool {
            // Support both world.destroy(uuid) and world:destroy(uuid).
            std::string uuidStr;
            if (uuidObj.is<std::string>())
                uuidStr = uuidObj.as<std::string>();
            else if (selfOrUuid.is<std::string>())
                uuidStr = selfOrUuid.as<std::string>();
            else
                return false;
            UUID uuid = UUID::Parse(uuidStr);
            if (uuid.IsNull()) return false;
            auto r = sinkPtr->DestroyEntity(uuid);
            return r.IsOk();
        };

        inst.env["world"] = world;

        // Bind entity methods as a table with closures over the sink + UUID.
        // All methods use the Lua : syntax, so the first parameter is the
        // self table (ignored).
        entity["get_uuid"] = [instUuid](sol::object) -> std::string {
            return instUuid.ToString();
        };
        entity["get_name"] = [sinkPtr, instUuid](sol::object) -> std::string {
            return sinkPtr->GetName(instUuid);
        };
        entity["set_name"] = [sinkPtr, instUuid](sol::object, const std::string& name) -> bool {
            return sinkPtr->SetName(instUuid, name);
        };
        entity["get_position"] = [sinkPtr, instUuid, L](sol::object) -> sol::object {
            glm::vec3 pos;
            if (!sinkPtr->GetPosition(instUuid, pos))
                return sol::nil;
            sol::state_view sv(L);
            sol::table t = sv.create_table();
            t[1] = pos.x; t[2] = pos.y; t[3] = pos.z;
            return t;
        };
        entity["set_position"] = [sinkPtr, instUuid](sol::object, sol::table pos) -> bool {
            if (!pos.valid() || !pos.is<sol::table>()) return false;
            glm::vec3 p;
            p.x = pos.get_or(1, 0.0f);
            p.y = pos.get_or(2, 0.0f);
            p.z = pos.get_or(3, 0.0f);
            return sinkPtr->SetPosition(instUuid, p);
        };
        entity["get_visible"] = [sinkPtr, instUuid](sol::object) -> bool {
            bool v = false;
            sinkPtr->GetVisible(instUuid, v);
            return v;
        };
        entity["set_visible"] = [sinkPtr, instUuid](sol::object, bool v) -> bool {
            return sinkPtr->SetVisible(instUuid, v);
        };
    }
    else
    {
        inst.env["world"] = sol::nil;
    }

    inst.env["entity"] = entity;

    // Bind `input` (S2): present from 6A but inert. The IInputService
    // methods are added in 6C. In 6A, input is a non-nil table with no
    // methods, so scripts that ignore it run unchanged.
    if (input)
    {
        sol::table inputTable = m_Lua.create_table();
        // 6C will add: is_down, is_pressed, is_released, get_axis,
        // get_mouse_delta, get_scroll_delta. 6A leaves it methodless.
        inst.env["input"] = inputTable;
    }
    else
    {
        inst.env["input"] = sol::nil;
    }

    // Load + execute the script source in the environment. This defines
    // the on_create/on_fixed_update/on_update/on_destroy functions as
    // environment globals.
    sol::protected_function_result result = m_Lua.safe_script(
        source, inst.env,
        sol::script_pass_on_error,
        scriptPath.string());

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
    printf("[Script] %s error (entity %s, callback %s): %s\n",
           inst.state == ScriptInstanceState::NeverCreated ? "load" : "runtime",
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
        if (inst.state == ScriptInstanceState::Live ||
            inst.state == ScriptInstanceState::Quarantined)
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