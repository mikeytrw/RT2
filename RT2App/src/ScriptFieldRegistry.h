#pragma once

#ifndef RT2_CORE_SCRIPT_FIELD_REGISTRY_H
#define RT2_CORE_SCRIPT_FIELD_REGISTRY_H

#include "ScriptFieldValue.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// ScriptFieldRegistry — parses a .lua script's `rt2.fields` block into
// ScriptFieldDescriptors, and caches the result per path.
//
// WHY THIS IS SEPARATE FROM ScriptSystem (Phase 6B plan, D1):
//
// ScriptSystem's Lua state and per-entity instance map exist only during a
// Play session (built at OnSceneStart, torn down at OnSceneStop). The
// inspector must show a script's declared fields while the editor is
// STOPPED, so declaration discovery cannot live there. The registry owns its
// own sol::state, is independent of Play, and is queried directly by the
// inspector. ScriptSystem::GetDeclaredFields delegates here so the
// Play-session path and the authoring path cannot diverge.
//
// SANDBOXING (D3). The editor parses arbitrary user .lua on selection, so
// the parse environment:
//   - opens base/math/string/table only (mirroring ScriptSystem's library
//     set); no io, os, debug, or package,
//   - nils dofile/loadfile/require/load in the environment,
//   - runs under a protected call, so a syntax or runtime error is a
//     diagnostic rather than a throw,
//   - installs a LUA_MASKCOUNT hook with an instruction budget, so a
//     top-level `while true do end` cannot hang the editor.
// Lifecycle callbacks are DEFINED by the chunk but never invoked.
//
// PARSE FAILURE IS NON-DESTRUCTIVE (D10). On a failed parse the registry
// returns the LAST KNOWN-GOOD descriptors for that path (empty if it never
// parsed cleanly) with parsed=false and a diagnostic. Callers must not
// reconcile against a parsed=false result: reconciling against zero
// declarations would treat every field as removed and delete the user's
// authored values on a transient syntax error.
//
// CPU-only: no ImGui, no Vulkan, no Walnut. Links into RT2Tests.
// ============================================================================

// Forward-declared so this header does not pull sol2/Lua into every TU that
// merely wants to hold a registry.
namespace sol { class state; }

namespace rt2::core {

class ScriptFieldRegistry
{
public:
    ScriptFieldRegistry();
    ~ScriptFieldRegistry();

    ScriptFieldRegistry(const ScriptFieldRegistry&) = delete;
    ScriptFieldRegistry& operator=(const ScriptFieldRegistry&) = delete;

    struct Result
    {
        std::vector<ScriptFieldDescriptor> descriptors;
        // False when this parse failed. `descriptors` then holds the last
        // known-good set for the path (possibly empty) — see D10. Callers
        // must not run reconciliation when this is false.
        bool                               parsed = false;
        // Human-readable reason when parsed == false; empty otherwise.
        std::string                        diagnostic;
    };

    // Parse (or return cached) declarations for a script path. The cache is
    // keyed by path and invalidated when the file's last-write time or size
    // changes, so an edited script re-parses on the next query.
    //
    // A missing or unreadable file yields parsed=false with a diagnostic;
    // an empty file is a legal script that declares no fields (parsed=true,
    // zero descriptors), matching ScriptSystem's Q10d treatment.
    Result GetDeclaredFields(const std::filesystem::path& path);

    // Drop all cached entries, including last-known-good descriptors. Called
    // on scene close and scene load so a long editing session cannot
    // accumulate entries for scripts no longer referenced.
    void Clear();

    // Number of cached paths. Test/diagnostic helper.
    size_t CachedCount() const { return m_Cache.size(); }

    // Maximum cached paths before the least-recently-used entry is evicted.
    static constexpr size_t kMaxCachedPaths = 64;

    // Instruction budget for one parse (D3). Generous for any plausible
    // declaration block, tiny compared to a runaway loop.
    static constexpr int kInstructionBudget = 2'000'000;

private:
    struct CacheEntry
    {
        std::vector<ScriptFieldDescriptor> descriptors;   // last known-good
        bool         everParsed = false;
        int64_t      mtime = 0;
        uint64_t     size = 0;
        uint64_t     hash = 0;      // FNV-1a of the source text
        uint64_t     lastUse = 0;   // LRU tick
    };

    // Parse already-read source, ignoring the cache. Fills `out` on success.
    // Must NOT call back into GetDeclaredFields: the parse sandbox has no
    // handle to the registry, and EvictIfNeeded assumes no reentrancy.
    bool ParseFile(const std::filesystem::path& path,
                   const std::string& source,
                   std::vector<ScriptFieldDescriptor>& out,
                   std::string& diagnostic);

    void EvictIfNeeded();

    // Owned by pointer so sol2/Lua headers stay out of this header.
    sol::state* m_Lua = nullptr;

    std::unordered_map<std::string, CacheEntry> m_Cache;
    uint64_t m_UseTick = 0;
};

} // namespace rt2::core

#endif // RT2_CORE_SCRIPT_FIELD_REGISTRY_H
