#pragma once

#ifndef RT2_CORE_SCRIPT_FIELD_VALUE_H
#define RT2_CORE_SCRIPT_FIELD_VALUE_H

#include "core/UUID.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

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

// The variant arm a declared type occupies. Two ScriptFieldTypes are
// *compatible* (a value survives a declaration change) iff they share an
// arm — this is the single rule behind Phase 6B's incompatible-type
// handling, and it is why vec3 -> color preserves the value while
// float -> vec3 does not. Returns the std::variant index.
constexpr size_t ScriptFieldArmIndex(ScriptFieldType type)
{
    switch (type)
    {
    case ScriptFieldType::Bool:   return 0;   // bool
    case ScriptFieldType::Int:    return 1;   // int64_t
    case ScriptFieldType::Float:  return 2;   // double
    case ScriptFieldType::String: return 3;   // std::string
    case ScriptFieldType::Uuid:   return 4;   // UUID
    case ScriptFieldType::Vec3:   return 5;   // glm::vec3
    case ScriptFieldType::Color:  return 5;   // glm::vec3 (shared arm)
    }
    return 2;
}

constexpr bool ScriptFieldTypesCompatible(ScriptFieldType a, ScriptFieldType b)
{
    return ScriptFieldArmIndex(a) == ScriptFieldArmIndex(b);
}

// Canonical lowercase tag for a declared type. This is the on-disk tag
// written by SceneSerializer v3 and the label used in diagnostics.
constexpr const char* ScriptFieldTypeName(ScriptFieldType type)
{
    switch (type)
    {
    case ScriptFieldType::Bool:   return "bool";
    case ScriptFieldType::Int:    return "int";
    case ScriptFieldType::Float:  return "float";
    case ScriptFieldType::String: return "string";
    case ScriptFieldType::Uuid:   return "uuid";
    case ScriptFieldType::Vec3:   return "vec3";
    case ScriptFieldType::Color:  return "color";
    }
    return "float";
}

// ============================================================================
// ScriptFieldDescriptor — one public field as *declared by the script*.
//
// Produced by ScriptFieldRegistry from the script's `rt2.fields` block. The
// declaration is the source of truth for the field's type, its default, and
// (for a renamed field) the name it used to have. Persisted values are
// reconciled against these descriptors on load and on reload.
//
// `alias` names the OLD field, i.e. a descriptor {name="vel", alias="speed"}
// means "vel was previously called speed; migrate the persisted speed value
// into vel". Migration is one hop only — alias chains are not followed.
// ============================================================================

struct ScriptFieldDescriptor
{
    std::string                 name;
    ScriptFieldType             type = ScriptFieldType::Float;
    ScriptFieldValue            defaultValue;
    std::optional<std::string>  alias;
};

} // namespace rt2::core

#endif // RT2_CORE_SCRIPT_FIELD_VALUE_H