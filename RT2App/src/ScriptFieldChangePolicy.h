#pragma once

#ifndef RT2_CORE_SCRIPT_FIELD_CHANGE_POLICY_H
#define RT2_CORE_SCRIPT_FIELD_CHANGE_POLICY_H

#include "SceneSerializer.h"
#include "ScriptFieldResolver.h"

#include <vector>

namespace rt2::core {

struct ScriptFieldChangeClassification
{
    bool requiresSave = false;
    bool destructive = false;
};

// Host-side persistence gate for destructive load repair. The first manual
// Save acknowledges the loss but does not persist it; autosave remains blocked
// until a subsequent Save succeeds or the document is replaced/reset.
class ScriptRepairPersistenceGate
{
public:
    void Adopt(bool destructive)
    {
        m_Pending = destructive;
        m_Acknowledged = false;
    }

    bool SuppressAutosave() const { return m_Pending; }

    bool ConsumeSaveAcknowledgement()
    {
        if (!m_Pending || m_Acknowledged) return false;
        m_Acknowledged = true;
        return true;
    }

    void OnPersistedOrReset()
    {
        m_Pending = false;
        m_Acknowledged = false;
    }

    bool Pending() const { return m_Pending; }
    bool Acknowledged() const { return m_Acknowledged; }

private:
    bool m_Pending = false;
    bool m_Acknowledged = false;
};

inline bool IsDestructiveScriptFieldDiagnostic(FieldDiagnostic::Kind kind)
{
    switch (kind)
    {
    case FieldDiagnostic::Kind::Removed:
    case FieldDiagnostic::Kind::AmbiguousAlias:
    case FieldDiagnostic::Kind::TypeChanged:
    case FieldDiagnostic::Kind::InvalidStoredValue:
    case FieldDiagnostic::Kind::UnknownSerializedType:
    case FieldDiagnostic::Kind::MalformedSerializedValue:
        return true;
    default:
        return false;
    }
}

inline ScriptFieldChangeClassification ClassifyScriptFieldChanges(
    const SceneLoadReport& loadReport,
    const ScriptFieldResolutionResult& resolution,
    const std::vector<FieldDiagnostic>& diagnostics)
{
    ScriptFieldChangeClassification result;
    result.requiresSave = loadReport.normalizedScriptMetadata ||
                          loadReport.normalizedScriptFieldData ||
                          loadReport.droppedScriptFieldData ||
                          resolution.changed;
    result.destructive = loadReport.droppedScriptFieldData;
    for (const auto& diagnostic : diagnostics)
        result.destructive = result.destructive ||
                             IsDestructiveScriptFieldDiagnostic(diagnostic.kind);
    return result;
}

} // namespace rt2::core

#endif // RT2_CORE_SCRIPT_FIELD_CHANGE_POLICY_H
