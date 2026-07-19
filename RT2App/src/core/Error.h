#pragma once

#ifndef RT2_CORE_ERROR_H
#define RT2_CORE_ERROR_H

#include <string>
#include <utility>

// ============================================================================
// Error — the engine's diagnostic return type.
//
// Engine code paths do not throw. Operations that can fail return bool and
// fill an Error out-param with a code, the path involved, and a human-readable
// detail string. Callers check IsOk() or the code directly.
//
// Keep this type free of Vulkan/ImGui/Walnut dependencies so it links into
// RT2Tests, RT2SliceRunner, and RT2App alike.
//
// ============================================================================

namespace rt2::core {

struct Error
{
    enum Code
    {
        None = 0,
        // File/IO
        Io,
        Parse,
        // Scene/schema
        SchemaVersion,
        DuplicateUuid,
        MissingParent,
        UnknownPrimitive,
        MissingAsset,
        InvalidEntity,
        InvalidHierarchy,
        HierarchyCycle,
        InvalidTransform,
        LockedEntity,
        ClipboardStale,
        // Runtime
        InvalidRuntimeState,
    };

    Code        code = None;
    std::string path;     // file or asset path involved (may be empty)
    std::string detail;   // human-readable context

    bool IsOk() const { return code == None; }

    // Human-readable one-liner for logs and CLI output.
    // Format: "code=<name> path=<path> detail=<detail>"
    std::string Format() const;

    // Code name for Format and tests.
    static const char* CodeName(Code c);
};

// Result<T> — value-or-error return type for APIs that can fail with a value
// in the success case. Engine code paths do not throw; this is the typed
// equivalent of the (bool, Error out-param) pattern for APIs like
// SceneManager::CountCanonicalSubtreeEntities that return a count on success.
template<typename T>
struct Result
{
    T       value{};
    Error   error;

    bool IsOk() const { return error.IsOk(); }

    static Result Ok(T v) { Result r; r.value = std::move(v); return r; }
    static Result Fail(Error::Code code, std::string path, std::string detail)
    {
        Result r;
        r.error.code = code;
        r.error.path = std::move(path);
        r.error.detail = std::move(detail);
        return r;
    }
};

} // namespace rt2::core

#endif // RT2_CORE_ERROR_H
