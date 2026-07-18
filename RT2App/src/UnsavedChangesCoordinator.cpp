#include "UnsavedChangesCoordinator.h"

namespace rt2::core {

bool UnsavedChangesCoordinator::Request(const PendingAction& action)
{
    // Never allow a second pending request to clobber an unresolved one.
    if (m_Pending)
        return false;

    if (!IsDirty())
    {
        Execute(action);
        return true;
    }

    // Dirty: queue and let the host prompt.
    m_Pending = action;
    return false;
}

void UnsavedChangesCoordinator::ResolveSave()
{
    if (!m_Pending) return;

    // The host owns the Save-vs-Save-As decision because it owns the current
    // document path and file-dialog lifecycle.
    bool saved = m_Save ? m_Save() : false;

    if (saved)
    {
        PendingAction a = *m_Pending;
        m_Pending.reset();
        Execute(a);
    }
    // On failure or Save As cancel: retain the pending action so the host
    // can re-show the modal. Do NOT execute.
}

void UnsavedChangesCoordinator::ResolveDiscard()
{
    if (!m_Pending) return;

    PendingAction a = *m_Pending;
    m_Pending.reset();
    if (m_DiscardRecovery) m_DiscardRecovery();
    Execute(a);
}

void UnsavedChangesCoordinator::ResolveCancel()
{
    m_Pending.reset();
    // No mutations, no execution.
}

} // namespace rt2::core
