#pragma once

#ifndef RT2_CORE_SCRIPT_SANDBOX_H
#define RT2_CORE_SCRIPT_SANDBOX_H

#include <sol/sol.hpp>

#include <stdexcept>
#include <string>

// ============================================================================
// ScriptSandbox — the single definition of what a script environment may not
// reach.
//
// RT2 has two sandboxes: the runtime one (ScriptSystem::BuildEnvironment,
// per entity, per Play session) and the parse one
// (ScriptFieldRegistry::ParseFile, editor-time, for rt2.fields discovery).
// They previously maintained separate deny lists and drifted. Both now call
// InstallSandboxDenials, so a gap closed in one is closed in the other.
//
// WHY A THROWING STUB AND NOT sol::nil.
//
// Assigning sol::nil does NOT shadow anything: setting a table key to nil in
// Lua REMOVES the key, so the environment's __index fallback to the globals
// table kicks straight back in and the global stays reachable. That is a
// real bug this project shipped — dofile, loadfile and load (all defined by
// sol::lib::base) were "nilled" and stayed fully callable from every user
// script; loadfile and load do not even raise, so the escape was silent.
// Binding a stub stores a real non-nil value, which genuinely shadows the
// global. sol converts the C++ exception into an ordinary Lua error inside
// the enclosing protected call, so the offending instance quarantines with
// a clear message instead of escaping.
//
// WHAT IS DENIED, AND WHY.
//
//   dofile, loadfile, load, loadstring
//       Filesystem reads and arbitrary chunk loading. `load` in particular
//       compiles a chunk whose default environment is _G, not the entity's
//       sandbox, so it launders code straight out of the sandbox.
//
//   _G
//       The globals table is SHARED by every per-entity environment via the
//       __index fallback. Reachable, `_G.x = 1` in one entity's script is
//       visible to every other entity's script, which breaks the per-entity
//       isolation guarantee (S3) that the whole environment design exists
//       to provide.
//
//   rawset, rawget, rawequal, rawlen
//       Bypass metatables, so they defeat any protection installed on a
//       shared table and give raw access to table internals.
//
//   collectgarbage
//       Lets a script stall the frame or tamper with GC pacing.
//
//   getmetatable
//       THE environment escape. An environment created as
//       sol::environment(lua, sol::create, lua.globals()) is a table whose
//       metatable is { __index = <globals> }. So inside a chunk,
//       getmetatable(_ENV).__index IS the globals table, and
//       .loadfile / .dofile / .load off it are the real functions — every
//       name-level shadow above is bypassed in one line. getmetatable("")
//       likewise hands back the shared string metatable, whose __index is
//       the shared `string` table. An earlier iteration of this header
//       argued getmetatable was safe once _G was denied; that was wrong,
//       and a test now proves the escape
//       (Phase6BFieldsTests.cpp, "the environment metatable does not leak
//       the globals table").
//
// NOT denied, deliberately:
//
//   setmetatable — the standard Lua idiom for classes, heavily used in
//       ordinary game scripts. It is not an escape by itself: it WRITES a
//       metatable rather than reading one, and with _G, getmetatable and
//       the raw* family denied a script has no way to obtain a reference to
//       any privileged table to install.
//
//   string.dump — produces bytecode, but with no io, no os and no network
//       binding there is nowhere for the bytes to go, and `load` (the only
//       way to rehydrate them) is denied.
//
//   print, pairs, ipairs, type, error, pcall, tostring, tonumber, select,
//       assert, next, unpack — ordinary, safe, and needed by real scripts.
// ============================================================================

namespace rt2::core {

// ============================================================================
// Instruction budgets.
//
// Protected calls catch ERRORS, not HANGS. A chunk or callback containing
// `while true do end` never returns, so no result is ever produced and
// neither sol::protected_function nor lua_atpanic can intervene — the engine
// hangs and only killing the process recovers it. The S1 quarantine machine
// cannot help either, because quarantine requires the call to RETURN.
//
// A LUA_MASKCOUNT hook is the only mechanism that regains control from
// non-returning Lua. luaL_error from the hook longjmps to the enclosing
// pcall, which sol surfaces as an ordinary error — so an exhausted budget
// quarantines the instance through the existing error path with no special
// casing.
//
// The budgets are deliberately generous: they are a runaway backstop, not a
// performance policy. A callback doing real work uses a tiny fraction.
// ============================================================================

constexpr int kScriptLoadInstructionBudget     = 2'000'000;   // chunk evaluation
constexpr int kScriptCallbackInstructionBudget = 5'000'000;   // one callback

inline void ScriptInstructionHook(lua_State* L, lua_Debug*)
{
    luaL_error(L, "script exceeded its instruction budget "
                  "(an unterminated loop?)");
}

// RAII. The clear must be unconditional: safe_script and protected calls can
// propagate a C++ exception (a sol binding throw, std::bad_alloc during
// compilation), and a leaked count hook would then fire inside unrelated
// Lua work on the same shared state.
struct ScriptInstructionBudget
{
    lua_State* L;

    ScriptInstructionBudget(lua_State* state, int budget) : L(state)
    {
        lua_sethook(L, &ScriptInstructionHook, LUA_MASKCOUNT, budget);
    }
    ~ScriptInstructionBudget() { lua_sethook(L, nullptr, 0, 0); }

    ScriptInstructionBudget(const ScriptInstructionBudget&) = delete;
    ScriptInstructionBudget& operator=(const ScriptInstructionBudget&) = delete;
};

// Shadow one global inside a sandboxed environment with a stub that raises.
inline void DenySandboxGlobal(sol::environment& env, const char* name)
{
    const std::string message =
        std::string(name) + " is disabled in the RT2 script sandbox";
    env[name] = [message](sol::variadic_args) -> int {
        throw std::runtime_error(message);
    };
}

// Give the environment its OWN shallow copy of a standard library table.
//
// Without this, `math`, `string`, `table` and `utf8` resolve through the
// globals fallback to tables SHARED by every script in the Lua state, so
// `math.floor = function() return 999 end` in one entity's script silently
// poisons every other entity's script — and, in the editor, every later
// field parse. A shallow copy is enough: the functions themselves are
// immutable C closures, so only the table binding needs isolating. Method
// syntax on primitives ("abc":upper()) still routes through the original
// string table via the string metatable, which is fine — that path is
// read-only once getmetatable is denied.
inline void CopySandboxLibrary(sol::state_view lua,
                               sol::environment& env,
                               const char* name)
{
    sol::object orig = lua.globals()[name];
    if (!orig.is<sol::table>()) return;
    sol::table src = orig.as<sol::table>();
    sol::table dst = lua.create_table();
    src.for_each([&dst](sol::object k, sol::object v) { dst[k] = v; });
    env[name] = dst;
}

// Install the full deny list. Call on every freshly created script
// environment, in both the runtime and the parse sandbox.
inline void InstallSandboxDenials(sol::environment& env)
{
    // Filesystem / module / chunk loading.
    DenySandboxGlobal(env, "dofile");
    DenySandboxGlobal(env, "loadfile");
    DenySandboxGlobal(env, "require");
    DenySandboxGlobal(env, "load");
    DenySandboxGlobal(env, "loadstring");   // Lua 5.1 spelling; harmless if absent

    // Shared mutable state across every entity's environment.
    DenySandboxGlobal(env, "_G");

    // Metatable bypass, and the environment-metatable escape.
    DenySandboxGlobal(env, "getmetatable");
    DenySandboxGlobal(env, "rawset");
    DenySandboxGlobal(env, "rawget");
    DenySandboxGlobal(env, "rawequal");
    DenySandboxGlobal(env, "rawlen");

    // Frame-stalling / GC tampering.
    DenySandboxGlobal(env, "collectgarbage");
}

// The single entry point: deny the dangerous globals AND isolate the
// mutable library tables. Every freshly created script environment — the
// runtime one in ScriptSystem::BuildEnvironment and the parse one in
// ScriptFieldRegistry::ParseFile — must call this and nothing less.
inline void InstallSandbox(sol::state_view lua, sol::environment& env)
{
    InstallSandboxDenials(env);
    CopySandboxLibrary(lua, env, "math");
    CopySandboxLibrary(lua, env, "string");
    CopySandboxLibrary(lua, env, "table");
    CopySandboxLibrary(lua, env, "utf8");
    CopySandboxLibrary(lua, env, "coroutine");   // no-op unless 6C re-opens it
}

} // namespace rt2::core

#endif // RT2_CORE_SCRIPT_SANDBOX_H
