#pragma once

#ifndef RT2_SCENE_MUTATION_H
#define RT2_SCENE_MUTATION_H

#include "ISceneRenderBridge.h"
#include "core/Error.h"
#include "core/UUID.h"

#include <vector>

enum class ReparentMode
{
    PreserveWorld,
    PreserveLocal,
};

struct EditorMutationResult
{
    bool success = true;
    rt2::core::Error error;
    rt2::core::SyncImpact syncImpact = rt2::core::SyncImpact::None;
    std::vector<rt2::core::UUID> affectedEntities;

    static EditorMutationResult Failure(rt2::core::Error::Code code,
                                        const std::string& path,
                                        const std::string& detail)
    {
        EditorMutationResult result;
        result.success = false;
        result.error.code = code;
        result.error.path = path;
        result.error.detail = detail;
        return result;
    }
};

#endif
