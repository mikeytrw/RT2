#pragma once

#ifndef RT2_CORE_ERROR_H
#define RT2_CORE_ERROR_H

#include <string>

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

} // namespace rt2::core

#endif // RT2_CORE_ERROR_H