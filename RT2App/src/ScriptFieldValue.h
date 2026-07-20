#pragma once

#ifndef RT2_CORE_SCRIPT_FIELD_VALUE_H
#define RT2_CORE_SCRIPT_FIELD_VALUE_H

#include "core/UUID.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <variant>

// ============================================================================
// ScriptFieldValue — the supported value space for script public fields.
//
// This header is deliberately CPU-only (no sol2/Lua/ImGui/Walnut/Vulkan) so
// it can be shared by:
//   - ECSComponents.h (the ScriptComponent struct lives here via include),
//   - the serializer (SceneSerializer v3, Phase 6B),
//   - the reflection layer (SceneEditorUI::RenderScriptEditor, Phase 6B),
//   - ScriptSystem (Phase 6A/6B/6C),
//   - RT2Tests (CPU-only).
//
// The variant is fixed from Phase 6A (per design review S6: adding a variant
// arm later re-touches the serializer + compatibility-rule surface, so
// vec3/color are in the initial set). color shares the glm::vec3 arm and is
// distinguished at the inspector-widget layer, not at the value layer.
//
// Supported types:
//   bool, int64_t, double, std::string, rt2::core::UUID, glm::vec3
//
// `color` is a semantic label on top of glm::vec3, not a separate variant
// arm. ScriptFieldDescriptor (Phase 6B) carries the type label that selects
// the inspector widget.
// ============================================================================

namespace rt2::core {

enum class ScriptFieldType : uint8_t
{
    Bool    = 0,
    Int     = 1,
    Float   = 2,
    String  = 3,
    Uuid    = 4,
    Vec3    = 5,
    Color   = 6,    // glm::vec3 arm, color-picker widget
};

// Variant of supported field value types. Default-constructed state is
// bool/false so a ScriptFieldValue is never valueless-by-exception.
using ScriptFieldValue = std::variant<
    bool,
    int64_t,
    double,
    std::string,
    UUID,
    glm::vec3
>;

} // namespace rt2::core

#endif // RT2_CORE_SCRIPT_FIELD_VALUE_H