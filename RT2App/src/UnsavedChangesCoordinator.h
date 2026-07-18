#pragma once

#ifndef RT2_CORE_UNSAVED_CHANGES_COORDINATOR_H
#define RT2_CORE_UNSAVED_CHANGES_COORDINATOR_H

#include "core/Error.h"

#include <filesystem>
#include <functional>
#include <string>
#include <optional>

// ============================================================================
// UnsavedChangesCoordinator — pure state machine for New/Open/Exit requests
// against a document that may have unsaved authoring changes.
//
// No ImGui dependency. The host wires callbacks for Save, SaveAs, Open(path),
// New, and Close. Tests inject fakes. Exactly one pending action may be
// queued; a second Request while a prompt is pending is rejected (returns
// false) so the user must resolve the current prompt first.
//
// Transitions:
//   Request(action) when clean  -> execute immediately, return true.
//   Request(action) when dirty  -> queue action, return false (host should
//                                  open the Save/Discard/Cancel modal).
//   ResolveSave()   -> SaveGate():
//                        success -> execute pending, clear pending.
//                        failure -> retain pending, host re-shows modal.
//                        cancel  -> retain pending (Save As cancelled).
//   ResolveDiscard() -> execute pending, clear pending, clear recovery.
//   ResolveCancel()  -> clear pending, no mutations.
//
// The host owns the actual scene/document/recovery services; this class
// only decides ordering and gating.
// ============================================================================

namespace rt2::core {

class UnsavedChangesCoordinator
{
public:
    enum class ActionKind
    {
        None,
        New,
        Open,
        Recent,
        Exit,
    };

    struct PendingAction
    {
        ActionKind            kind = ActionKind::None;
        std::filesystem::path path; // for Open / Recent
    };

    // Unified save gate. The host decides whether the current document uses
    // Save or Save As and returns true only after a successful explicit
    // native save. Cancellation and failure both return false.
    using SaveGate    = std::function<bool()>;
    using ExecuteGate = std::function<void(const PendingAction&)>;

    UnsavedChangesCoordinator() = default;

    // Query the current dirty state. The host owns the dirty flag; this
    // helper just queries it so the coordinator stays stateless re: scene.
    void SetIsDirtyQuery(std::function<bool()> q) { m_IsDirty = std::move(q); }
    void SetSaveGate(SaveGate g)         { m_Save = std::move(g); }
    void SetExecuteGate(ExecuteGate g)  { m_Execute = std::move(g); }

    // True when a prompt is pending (host should render the modal).
    bool NeedsPrompt() const { return m_Pending.has_value(); }
    const PendingAction& Pending() const { return *m_Pending; }

    // Request an action. Returns true if executed immediately (clean doc),
    // false if a prompt is now pending (dirty doc). Returns false (and does
    // nothing) if a prompt is already pending — the host must resolve it
    // first.
    bool Request(const PendingAction& action);

    // Resolve the pending prompt.
    void ResolveSave();
    void ResolveDiscard();
    void ResolveCancel();

    // Discard callback lets the host clear the recovery record for the
    // abandoned document. Set if you want auto-cleanup on Discard.
    void SetDiscardRecoveryGate(std::function<void()> g) { m_DiscardRecovery = std::move(g); }

private:
    std::function<bool()>        m_IsDirty;
    SaveGate                     m_Save;
    ExecuteGate                  m_Execute;
    std::function<void()>        m_DiscardRecovery;
    std::optional<PendingAction> m_Pending;

    bool IsDirty() const { return m_IsDirty ? m_IsDirty() : false; }
    void Execute(const PendingAction& a)
    {
        if (m_Execute) m_Execute(a);
    }
};

} // namespace rt2::core

#endif // RT2_CORE_UNSAVED_CHANGES_COORDINATOR_H
