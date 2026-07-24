#pragma once

#ifndef RT2_CORE_SCRIPT_FIELD_VALUE_H
#define RT2_CORE_SCRIPT_FIELD_VALUE_H

#include "core/UUID.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
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

inline constexpr std::array<const char*, 7> ScriptFieldTypeNames{
    "bool", "int", "float", "string", "uuid", "vec3", "color"
};

// JSON persistence is strict UTF-8. Validate authored Lua byte strings before
// nlohmann::json::dump so invalid input becomes an actionable save error rather
// than an exception escaping the editor frame.
inline bool IsWellFormedUtf8(std::string_view text)
{
    size_t i = 0;
    while (i < text.size())
    {
        const auto lead = static_cast<unsigned char>(text[i]);
        if (lead <= 0x7f) { ++i; continue; }

        uint32_t codePoint = 0;
        size_t continuationCount = 0;
        if (lead >= 0xc2 && lead <= 0xdf)
        {
            codePoint = lead & 0x1f;
            continuationCount = 1;
        }
        else if (lead >= 0xe0 && lead <= 0xef)
        {
            codePoint = lead & 0x0f;
            continuationCount = 2;
        }
        else if (lead >= 0xf0 && lead <= 0xf4)
        {
            codePoint = lead & 0x07;
            continuationCount = 3;
        }
        else
        {
            return false;
        }

        if (i + continuationCount >= text.size()) return false;
        for (size_t offset = 1; offset <= continuationCount; ++offset)
        {
            const auto byte = static_cast<unsigned char>(text[i + offset]);
            if ((byte & 0xc0) != 0x80) return false;
            codePoint = (codePoint << 6) | (byte & 0x3f);
        }

        if ((continuationCount == 2 && codePoint < 0x800) ||
            (continuationCount == 3 && codePoint < 0x10000) ||
            (codePoint >= 0xd800 && codePoint <= 0xdfff) ||
            codePoint > 0x10ffff)
            return false;

        i += continuationCount + 1;
    }
    return true;
}

inline bool IsValidScriptFieldName(std::string_view name)
{
    return !name.empty() && IsWellFormedUtf8(name);
}

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

// A persisted/authored field value keeps its semantic declaration tag next
// to the variant payload. This is required because Vec3 and Color deliberately
// share the same glm::vec3 variant arm but select different inspector widgets
// and different on-disk type tags.
struct ScriptFieldEntry
{
    // Keep the default state internally consistent. Map insertion through
    // operator[] is therefore safe even before the caller assigns a value.
    ScriptFieldType  type = ScriptFieldType::Float;
    ScriptFieldValue value = 0.0;

    bool operator==(const ScriptFieldEntry& other) const
    {
        return type == other.type && value == other.value;
    }
    bool operator!=(const ScriptFieldEntry& other) const
    {
        return !(*this == other);
    }
};

using ScriptFieldMap = std::unordered_map<std::string, ScriptFieldEntry>;

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

inline bool ScriptFieldEntryHasValidPayload(const ScriptFieldEntry& entry)
{
    return entry.value.index() == ScriptFieldArmIndex(entry.type);
}

// Canonical lowercase tag for a declared type. This is the on-disk tag
// written by SceneSerializer v3 and the label used in diagnostics.
constexpr const char* ScriptFieldTypeName(ScriptFieldType type)
{
    const size_t index = static_cast<size_t>(type);
    return index < ScriptFieldTypeNames.size()
         ? ScriptFieldTypeNames[index] : ScriptFieldTypeNames[2];
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
