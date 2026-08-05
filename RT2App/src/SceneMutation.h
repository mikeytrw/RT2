#pragma once

#ifndef RT2_SCENE_MUTATION_H
#define RT2_SCENE_MUTATION_H

#include "ISceneRenderBridge.h"
#include "core/Error.h"
#include "core/UUID.h"

#include <vector>
#include <optional>

enum class ReparentMode
{
    PreserveWorld,
    PreserveLocal,
};

struct EditorMutationResult
{
    bool success = true;
    // A successful mutation that changed nothing (canonical no-op suppression).
    // History uses this to avoid recording phantom undo entries: when the
    // manager accepts a value that is already the stored state, it returns
    // success=true but effective=false. Commands and callers never set this
    // to false for a real mutation.
    bool effective = true;
    rt2::core::Error error;
    std::optional<rt2::core::Error> recoveryWarning;
    rt2::core::SyncImpact syncImpact = rt2::core::SyncImpact::None;
    std::vector<rt2::core::UUID> affectedEntities;

    static EditorMutationResult Failure(rt2::core::Error::Code code,
                                        const std::string& path,
                                        const std::string& detail)
    {
        EditorMutationResult result;
        result.success = false;
        result.effective = false;
        result.error.code = code;
        result.error.path = path;
        result.error.detail = detail;
        return result;
    }
};

#endif
