#pragma once

#ifndef RT2_CORE_SCENE_RUN_STATE_H
#define RT2_CORE_SCENE_RUN_STATE_H

namespace rt2::core {

enum class SceneRunState
{
    Edit,
    Playing,
    Paused,
};

} // namespace rt2::core

#endif // RT2_CORE_SCENE_RUN_STATE_H