#pragma once

#ifndef RT2_CORE_SCRIPT_FIELD_RECONCILE_H
#define RT2_CORE_SCRIPT_FIELD_RECONCILE_H

#include "ScriptFieldValue.h"
#include "core/UUID.h"

#include <string>
#include <vector>

namespace rt2::core {

struct FieldDiagnostic
{
    enum class Kind
    {
        Added,
        Removed,
        Renamed,
        AmbiguousAlias,
        TypeChanged,
        InvalidStoredValue,
        MissingEntityId,
        InvalidAssetKind,
        ParseFailed,
        UnknownSerializedType,
        MalformedSerializedValue,
        NormalizedScriptSourceKey,
        NormalizedSerializedUuid,
    };

    Kind        kind = Kind::Added;
    UUID        entity;
    std::string field;
    std::string fromField;
    std::string message;
};

// Reconcile authored values against the current declaration set. This is a
// pure, CPU-only operation: no Lua, filesystem, editor, or renderer access.
// The returned map contains exactly the declared fields. Diagnostics are
// deterministic: declaration-related entries are ordered by declared name,
// followed by removed authored fields ordered by name.
ScriptFieldMap ReconcileScriptFields(
    const ScriptFieldMap& persisted,
    const std::vector<ScriptFieldDescriptor>& declared,
    const UUID& entity,
    std::vector<FieldDiagnostic>& outDiagnostics);

} // namespace rt2::core

#endif // RT2_CORE_SCRIPT_FIELD_RECONCILE_H
