#pragma once

#include "core/UUID.h"

#include <vector>

// CPU-side metadata for a render submission. Entry i identifies the authored
// entity represented by GPU instance i. It intentionally lives outside
// GPUSceneData: UUIDs are editor identity, not shader/render payload.
using RenderInstanceMap = std::vector<rt2::core::UUID>;
