#pragma once

#ifndef RT2_PROPERTY_EDIT_SESSION_H
#define RT2_PROPERTY_EDIT_SESSION_H

#include "core/UUID.h"

#include <functional>
#include <optional>
#include <utility>

// ============================================================================
// PropertyEditSession<T> — pure state machine for record-on-release property
// edits in the Inspector.
//
// The 3A TransformEditSession was embedded in SceneEditorUI.cpp and tightly
// coupled to ImGui's IsItemActivated/IsItemDeactivatedAfterEdit. That made
// the deferred-close-after-mutation ordering and the defensive guards
// (target-changed, entity death, m_Editable false mid-edit) untestable —
// exactly where the keyboard-commit bug (0dcc490) lived, invisible to 183
// assertions.
//
// 3B2 extracts a pure state machine (NO ImGui dependency) so the lifecycle,
// deferred close, and guards are unit-testable. A thin ImGui glue layer in
// SceneEditorUI.cpp calls IsItemActivated/IsItemDeactivatedAfterEdit and
// forwards to the state machine.
//
// Lifecycle:
//   - OnActivated(beforeValue): called when a widget begins editing. Stores
//     the before-value and opens the session. The host has already read the
//     current (before) value from the manager BEFORE any per-frame mutation.
//   - OnEditCommitted(): the host has applied the per-frame mutation. The
//     state machine notes that a commit happened this activation (so a
//     subsequent CloseDeferred knows an actual edit occurred, vs an
//     activation that was cancelled with no edit).
//   - OnCancelled(): the widget was deactivated WITHOUT AfterEdit (Escape
//     cancel — ImGui reverts the value itself). Discards the session and
//     records nothing.
//   - CloseDeferred(afterValue, guards): called AFTER the per-frame mutation
//     block. Returns a SessionRecord<T> {before, after} when the session is
//     open, an edit was committed, and every guard predicate returns true.
//     Returns nullopt when the session was not open, no edit occurred, or a
//     guard failed — in all those cases the session is discarded and no
//     record is produced. The host turns a returned SessionRecord into a
//     command via the property-command factory (which applies its own no-op
//     suppression) and submits it via RecordApplied.
//
// Guards: the host injects predicate callbacks. Each is queried only at
// CloseDeferred time. Standard guards:
//   - TargetStillSelected: the inspected entity UUID still matches the
//     session target (selection switched mid-edit).
//   - TargetAlive: FindEntityByUuid(target) != entt::null.
//   - EditableNow: the editor's m_Editable flag is still true (Play did not
//     start mid-edit).
//   - (Material-properties only) SlotStillInRange: the session's slot index
//     is still a valid index into materials.
//
// Single active-session slot: the host owns one PropertyEditSession<T> per
// property kind and asserts no double-open. Cross-kind independence is
// sequential, not concurrent — a new activation while a different kind's
// session is open is a programming error and asserts in debug.
//
// ============================================================================

template <typename T>
class PropertyEditSession
{
public:
	struct SessionRecord
	{
		T before;
		T after;
	};

	using GuardPred = std::function<bool()>;

	PropertyEditSession() = default;

	bool IsOpen() const { return m_Open; }
	const rt2::core::UUID& Target() const { return m_Target; }
	const T& BeforeValue() const { return m_BeforeValue; }

	// Begin an edit. Stores the before-value and the target UUID. Asserts
	// (debug) that no session is already open — the host must close or
	// discard before opening a new one.
	void OnActivated(const rt2::core::UUID& target, const T& beforeValue)
	{
		(void)target; // stored; not queried by the state machine itself
		m_Target = target;
		m_BeforeValue = beforeValue;
		m_Open = true;
		m_CommittedThisActivation = false;
	}

	// Note that the host applied a mutation this activation. Required for
	// CloseDeferred to produce a record — an activation that was opened but
	// never had a commit is treated as a no-op.
	void OnEditCommitted()
	{
		m_CommittedThisActivation = true;
	}

	// Escape-cancel path: ImGui deactivated the widget without AfterEdit and
	// reverted the value itself. Discard the session; record nothing.
	void OnCancelled()
	{
		Discard();
	}

	// Called AFTER the per-frame mutation block. If the session is open, an
	// edit was committed, and every guard returns true, returns the
	// {before, after} record and discards the session. Otherwise discards
	// the session and returns nullopt.
	std::optional<SessionRecord> CloseDeferred(const T& afterValue,
	                                           const std::vector<GuardPred>& guards = {})
	{
		if (!m_Open)
		{
			Discard();
			return std::nullopt;
		}
		if (!m_CommittedThisActivation)
		{
			Discard();
			return std::nullopt;
		}
		for (const auto& g : guards)
		{
			if (!g || !g())
			{
				Discard();
				return std::nullopt;
			}
		}
		SessionRecord rec{ m_BeforeValue, afterValue };
		Discard();
		return rec;
	}

	void Discard()
	{
		m_Open = false;
		m_Target = rt2::core::UUID{};
		m_CommittedThisActivation = false;
	}

private:
	rt2::core::UUID m_Target;
	T               m_BeforeValue{};
	bool            m_Open = false;
	bool            m_CommittedThisActivation = false;
};

#endif // RT2_PROPERTY_EDIT_SESSION_H