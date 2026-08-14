#pragma once

#include "CompositePreviewSession.h"

#include <cstdint>
#include <optional>

// CPU-linkable mirror of Walnut's local viewport correlation. The gizmo owns
// its drag independently from SceneEditorUI's TransformPreviewSession, so a
// host transition must release four facts together: local drag, UI token,
// interaction sequence, and the binding between them. Production calls
// CompleteAfterUiTransition only after the UI close/discard/pending-transfer
// has completed; tests drive the same state machine across replacement and
// editability transitions without linking ImGui/Walnut.
class TransformGizmoHostLifecycle
{
public:
	bool OnLocalDragStarted(std::uint64_t interactionSequence)
	{
		if (m_LocalDragActive || m_InteractionSequence != 0 ||
			m_Token.has_value() || interactionSequence == 0)
			return false;
		m_LocalDragActive = true;
		m_InteractionSequence = interactionSequence;
		return true;
	}

	bool BindToken(const TransformGestureToken& token)
	{
		if (!m_LocalDragActive || m_InteractionSequence == 0 ||
			m_Token.has_value() || !token.IsValid())
			return false;
		m_Token = token;
		return true;
	}

	bool Matches(std::uint64_t eventSequence) const
	{
		return m_InteractionSequence != 0 && eventSequence != 0 &&
			m_InteractionSequence == eventSequence;
	}

	bool LocalDragActive() const { return m_LocalDragActive; }
	std::uint64_t InteractionSequence() const { return m_InteractionSequence; }
	const std::optional<TransformGestureToken>& Token() const { return m_Token; }

	// Begin was rejected after the gizmo had already emitted dragJustStarted.
	// No UI authority exists, so cancellation is immediate and local.
	void RejectBeginAndCancel() { Clear(); }

	// Call only after SceneEditorUI has closed/discarded the matching session,
	// or has retained all recovery authority in PendingRetry.
	void CompleteAfterUiTransition() { Clear(); }

private:
	void Clear()
	{
		m_LocalDragActive = false;
		m_Token.reset();
		m_InteractionSequence = 0;
	}

	bool m_LocalDragActive = false;
	std::optional<TransformGestureToken> m_Token;
	std::uint64_t m_InteractionSequence = 0;
};
