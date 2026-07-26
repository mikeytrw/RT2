#pragma once

#ifndef RT2_CORE_SCRIPT_COMPONENT_VALIDATION_H
#define RT2_CORE_SCRIPT_COMPONENT_VALIDATION_H

#include "ECSComponents.h"

#include <cmath>
#include <optional>
#include <string>

namespace rt2::core {

// Normalize redundant ScriptComponent metadata and validate the authored
// payload without touching a registry, Lua, ImGui, or the renderer. The
// returned component is safe for both the mutation boundary and persistence.
inline bool NormalizeAndValidateScriptComponent(const ScriptComponent& input,
                                                ScriptComponent& output,
                                                std::string& detail,
                                                std::string* field = nullptr)
{
    output = input;
    detail.clear();
    if (field) field->clear();

    if (output.asset.kind != AssetKind::Script)
    {
        detail = "ScriptComponent asset kind must be Script";
        return false;
    }

    // Script assets never use import settings — v3 does not serialize them and
    // no script code consumes them. Reset to defaults so commands cannot
    // preserve hidden state that Save would discard.
    output.asset.importSettings = ImportSettings{};

    if (output.asset.path.empty())
    {
        output.asset.sourceKey.clear();
        if (!output.fieldValues.empty())
        {
            detail = "an unbound ScriptComponent cannot contain authored fields";
            return false;
        }
        return true;
    }

    output.asset.sourceKey = "lua:asset=" + output.asset.path;

    for (const auto& [name, entry] : output.fieldValues)
    {
        if (!IsValidScriptFieldName(name))
        {
            if (field) *field = name;
            detail = name.empty()
                   ? "script field names must not be empty"
                   : "script field names must be valid UTF-8";
            return false;
        }

        bool knownType = false;
        switch (entry.type)
        {
        case ScriptFieldType::Bool:
        case ScriptFieldType::Int:
        case ScriptFieldType::Float:
        case ScriptFieldType::String:
        case ScriptFieldType::Uuid:
        case ScriptFieldType::Vec3:
        case ScriptFieldType::Color:
            knownType = true;
            break;
        }
        if (!knownType || !ScriptFieldEntryHasValidPayload(entry))
        {
            if (field) *field = name;
            detail = "script field '" + name + "' has an invalid type/payload pairing";
            return false;
        }

        if (entry.type == ScriptFieldType::Float)
        {
            if (!std::isfinite(std::get<double>(entry.value)))
            {
                if (field) *field = name;
                detail = "script field '" + name + "' must contain a finite number";
                return false;
            }
        }
        else if (entry.type == ScriptFieldType::String)
        {
            if (!IsWellFormedUtf8(std::get<std::string>(entry.value)))
            {
                if (field) *field = name;
                detail = "script field '" + name + "' must contain valid UTF-8";
                return false;
            }
        }
        else if (entry.type == ScriptFieldType::Vec3 ||
                 entry.type == ScriptFieldType::Color)
        {
            const auto& value = std::get<glm::vec3>(entry.value);
            if (!std::isfinite(value.x) || !std::isfinite(value.y) ||
                !std::isfinite(value.z))
            {
                if (field) *field = name;
                detail = "script field '" + name + "' must contain finite vector values";
                return false;
            }
        }
    }

    return true;
}

// Canonical equality for two optional ScriptComponents. Both must already be
// canonical (path → derived sourceKey, cleared import settings, validated
// fields) for this to be meaningful; the manager and command factory both
// canonicalize before comparing. Order-independent over the field map.
//   - both absent → equal (no-op)
//   - one absent → not equal
//   - both present → equal iff kind, path, sourceKey and typed field map match
inline bool ScriptComponentCanonicalEqual(
    const std::optional<ScriptComponent>& a,
    const std::optional<ScriptComponent>& b)
{
    const bool aHas = a.has_value();
    const bool bHas = b.has_value();
    if (!aHas && !bHas) return true;
    if (aHas != bHas) return false;
    if (a->asset.assetId != b->asset.assetId) return false;
    if (a->asset.kind != b->asset.kind) return false;
    if (a->asset.path != b->asset.path) return false;
    if (a->asset.sourceKey != b->asset.sourceKey) return false;
    return a->fieldValues == b->fieldValues;
}

} // namespace rt2::core

#endif // RT2_CORE_SCRIPT_COMPONENT_VALIDATION_H
