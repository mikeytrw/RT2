#pragma once

#include "CompositePreviewSession.h"
#include "EditorTransformGizmo.h"
#include "PreviewSessionClose.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

enum class TransformGizmoCloseIntent
{
	None,
	Restore,
	Finalize,
};

enum class AuthoringDocumentReplacementKind
{
	RecoveryRestore,
	OpenScene,
	AssetMigrationSave,
};

enum class AuthoringDocumentSelectionPolicy
{
	Reset,
	PreserveValid,
};

inline AuthoringDocumentSelectionPolicy SelectionPolicyForReplacement(
	AuthoringDocumentReplacementKind kind)
{
	return kind == AuthoringDocumentReplacementKind::AssetMigrationSave
		? AuthoringDocumentSelectionPolicy::PreserveValid
		: AuthoringDocumentSelectionPolicy::Reset;
}

// The exact production replacement boundary: adoption invalidates the old
// document first, then UI sessions/tokens/recovery are discarded, and only
// then may Walnut cancel its local gizmo/correlation.
template <typename Adopt, typename DiscardUi, typename CancelLocal>
void ApplyAuthoringDocumentReplacement(AuthoringDocumentReplacementKind kind,
	Adopt&& adopt, DiscardUi&& discardUi, CancelLocal&& cancelLocal)
{
	std::forward<Adopt>(adopt)();
	std::forward<DiscardUi>(discardUi)(SelectionPolicyForReplacement(kind));
	std::forward<CancelLocal>(cancelLocal)();
}

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
	template <typename CancelLocal>
	void CompleteAfterUiTransition(CancelLocal&& cancelLocal)
	{
		std::forward<CancelLocal>(cancelLocal)();
		Clear();
	}
	void CompleteAfterUiTransition() { Clear(); }

	// Consume a host-visible external transition. RecoveryTransferred is an
	// ownership transfer, not an invitation to retry: local correlation is
	// cancelled exactly as for Closed, while the UI retains recovery authority.
	template <typename CancelLocal>
	bool ConsumeExternalLifecycle(TransformGestureLifecycleState state,
		CancelLocal&& cancelLocal)
	{
		if (!m_LocalDragActive || state == TransformGestureLifecycleState::HealthyOpen)
			return false;
		CompleteAfterUiTransition(std::forward<CancelLocal>(cancelLocal));
		return true;
	}

	TransformGizmoCloseIntent CloseIntent(const TransformGizmoResult& event) const
	{
		if (!Matches(event.interactionSequence)) return TransformGizmoCloseIntent::None;
		if (event.dragCancelled) return TransformGizmoCloseIntent::Restore;
		if (event.dragJustEnded) return TransformGizmoCloseIntent::Finalize;
		return TransformGizmoCloseIntent::None;
	}

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

struct TransformGizmoCoordinatorCallbacks
{
	std::function<void()> cancelLocal;
	std::function<std::optional<TransformGestureToken>(
		const std::vector<rt2::core::UUID>&)> begin;
	std::function<EditorMutationResult(const TransformGestureToken&,
		const std::vector<std::pair<rt2::core::UUID, glm::mat4>>&)> preview;
	std::function<PreviewSessionCloseOutcome(bool,
		const TransformGestureToken&)> close;
};

struct TransformGizmoCoordinatorResult
{
	bool beginRejected = false;
	bool bindRejected = false;
	std::optional<EditorMutationResult> preview;
	std::optional<PreviewSessionCloseOutcome> close;
};

struct TransformGizmoCoordinatorFrameResult
{
	TransformGizmoResult event;
	TransformGizmoCoordinatorResult routed;
};

// ImGui-free coordinator for Walnut's exact ownership cadence. Walnut owns one
// instance and delegates every pre-Draw transition and every Draw result here;
// CPU tests drive the same callback order with real TransformPreviewSession
// close/preview operations and an observable real-cancel callback.
class TransformGizmoCoordinator
{
public:
	template <typename Adopt, typename DiscardUi, typename CancelLocal>
	void ReplaceDocument(AuthoringDocumentReplacementKind kind,
		Adopt&& adopt, DiscardUi&& discardUi, CancelLocal&& cancelLocal)
	{
		ApplyAuthoringDocumentReplacement(kind,
			std::forward<Adopt>(adopt), std::forward<DiscardUi>(discardUi),
			[this, cancel = std::forward<CancelLocal>(cancelLocal)]() mutable {
				m_Host.CompleteAfterUiTransition(cancel);
			});
	}

	bool BeforeDraw(TransformGestureLifecycleState state,
		const std::function<void()>& cancelLocal)
	{
		return m_Host.ConsumeExternalLifecycle(state, cancelLocal);
	}

	template <typename Draw>
	TransformGizmoCoordinatorFrameResult RunFrame(
		TransformGestureLifecycleState externalState,
		const std::function<void()>& cancelLocal, Draw&& draw,
		const TransformGizmoCoordinatorCallbacks& callbacks)
	{
		BeforeDraw(externalState, cancelLocal);
		TransformGizmoCoordinatorFrameResult result;
		result.event = std::forward<Draw>(draw)();
		result.routed = ProcessEvent(result.event, callbacks);
		return result;
	}

	TransformGizmoCoordinatorResult ProcessEvent(const TransformGizmoResult& event,
		const TransformGizmoCoordinatorCallbacks& callbacks)
	{
		TransformGizmoCoordinatorResult result;
		if (event.dragJustStarted && event.interactionSequence != 0 &&
			!m_Host.LocalDragActive() && !event.draggedUuids.empty())
		{
			if (!m_Host.OnLocalDragStarted(event.interactionSequence))
			{
				if (callbacks.cancelLocal) callbacks.cancelLocal();
				m_Host.RejectBeginAndCancel();
				result.beginRejected = true;
				return result;
			}
			const auto token = callbacks.begin
				? callbacks.begin(event.draggedUuids) : std::nullopt;
			if (!token)
			{
				if (callbacks.cancelLocal) callbacks.cancelLocal();
				m_Host.RejectBeginAndCancel();
				result.beginRejected = true;
				return result;
			}
			if (!m_Host.BindToken(*token))
			{
				if (callbacks.close) result.close = callbacks.close(false, *token);
				CompleteAfterUiTransition(callbacks.cancelLocal);
				result.bindRejected = true;
				return result;
			}
		}

		if (event.changed && !event.desiredWorld.empty() &&
			m_Host.Matches(event.interactionSequence) && m_Host.Token() &&
			callbacks.preview)
			result.preview = callbacks.preview(*m_Host.Token(), event.desiredWorld);

		const auto intent = m_Host.CloseIntent(event);
		if (intent != TransformGizmoCloseIntent::None)
		{
			if (m_Host.Token() && callbacks.close)
				result.close = callbacks.close(
					intent == TransformGizmoCloseIntent::Finalize, *m_Host.Token());
			CompleteAfterUiTransition(callbacks.cancelLocal);
		}
		return result;
	}

	PreviewSessionCloseOutcome RestoreActive(
		const TransformGizmoCoordinatorCallbacks& callbacks)
	{
		PreviewSessionCloseOutcome outcome;
		if (m_Host.Token() && callbacks.close)
			outcome = callbacks.close(false, *m_Host.Token());
		CompleteAfterUiTransition(callbacks.cancelLocal);
		return outcome;
	}

	void CompleteAfterUiTransition(const std::function<void()>& cancelLocal)
	{
		if (cancelLocal) m_Host.CompleteAfterUiTransition(cancelLocal);
		else m_Host.CompleteAfterUiTransition([]() {});
	}

	bool LocalDragActive() const { return m_Host.LocalDragActive(); }
	std::uint64_t InteractionSequence() const { return m_Host.InteractionSequence(); }
	const std::optional<TransformGestureToken>& Token() const { return m_Host.Token(); }
	bool Matches(std::uint64_t sequence) const { return m_Host.Matches(sequence); }

private:
	TransformGizmoHostLifecycle m_Host;
};
