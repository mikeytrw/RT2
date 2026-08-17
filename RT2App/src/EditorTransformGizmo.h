#pragma once

#include "EditorSelection.h"
#include "TransformEditing.h"

#include <glm/glm.hpp>

#include <string>
#include <optional>
#include <vector>
#include <utility>

class Camera;

enum class TransformGizmoOperation
{
	Translate,
	Rotate,
	Scale,
};

struct TransformGizmoResult
{
	bool consumesMouse = false;
	bool changed = false;
	bool active = false;
	bool pickThrough = false;
	std::string error;
	bool dragJustStarted = false;
	bool dragCancelled = false;
	// Phase 3B1: drag-end reporting. When a drag that produced changes ends,
	// `dragJustEnded` is true and `draggedUuids` + `dragStartLocal` carry
	// the before-drag local TRS for each dragged entity. The host builds a
	// multi-entity TransformCommand and calls RecordApplied.
	bool dragJustEnded = false;
	std::uint64_t interactionSequence = 0;
	std::vector<rt2::core::UUID> draggedUuids;
	std::vector<EditableTRS> dragStartLocal;
	// Intent-only output: the host stages these desired world matrices in one
	// validate-only batch before publishing a composite preview.
	std::vector<std::pair<rt2::core::UUID, glm::mat4>> desiredWorld;
};

// ImGui-free input to the exact release classifier used by Draw. Keeping the
// moved bit inside this seam makes never-moved press/release a first-class end
// fact rather than a fabricated test output.
struct TransformGizmoReleaseInput
{
	bool active = false;
	bool leftMouseDown = false;
	bool moved = false;
	std::uint64_t interactionSequence = 0;
	std::vector<rt2::core::UUID> draggedUuids;
	std::vector<EditableTRS> dragStartLocal;
};

inline std::optional<TransformGizmoResult> ClassifyTransformGizmoRelease(
	const TransformGizmoReleaseInput& input)
{
	if (!input.active || input.leftMouseDown) return std::nullopt;
	TransformGizmoResult result;
	result.pickThrough = !input.moved;
	result.dragJustEnded = true;
	result.interactionSequence = input.interactionSequence;
	result.draggedUuids = input.draggedUuids;
	result.dragStartLocal = input.dragStartLocal;
	return result;
}

// Lightweight intent-only viewport transform gizmo. It deliberately owns
// editor-only interaction state; the host stages its UUID/world intents
// through the transform preview session.
class EditorTransformGizmo
{
public:
	TransformGizmoResult Draw(const std::optional<EditorWorldTransformSnapshot>& snapshot,
		const EditorSelection& selection,
		const Camera& camera, const glm::vec2& imageMin, const glm::vec2& imageSize,
		bool imageHovered, bool editable, TransformSpace space, TransformPivot pivot,
		const TransformSnapSettings& snap, bool uniformScale = false);

	TransformGizmoOperation Operation() const { return m_Operation; }
	void SetOperation(TransformGizmoOperation operation) { m_Operation = operation; }
	void Cancel();

private:
	struct DragState
	{
		bool active = false;
		bool moved = false;
		int axis = -1;
		TransformGizmoOperation operation = TransformGizmoOperation::Translate;
		TransformSpace space = TransformSpace::World;
		TransformPivot pivot = TransformPivot::Primary;
		glm::vec2 startMouse{ 0.0f };
		glm::vec2 dragScreenDirection{ 1.0f, 0.0f };
		glm::vec3 axisWorld{ 1.0f, 0.0f, 0.0f };
		glm::quat pivotRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
		glm::mat4 startPivot{ 1.0f };
		float worldHandleLength = 1.0f;
		float pixelHandleLength = 1.0f;
		std::vector<rt2::core::UUID> uuids;
		std::vector<glm::mat4> startWorld;
		std::vector<EditableTRS> startLocal;  // Phase 3B1: before-drag local TRS
		std::size_t primaryIndex = 0;
		std::uint64_t interactionSequence = 0;
	};

	TransformGizmoOperation m_Operation = TransformGizmoOperation::Translate;
	DragState m_Drag;
	std::uint64_t m_NextInteractionSequence = 0;
};

// Host-side authorization seams are deliberately pure so delayed viewport
// facts and Inspector publisher ownership can be tested without ImGui/GPU.
inline bool TransformGizmoEventMatches(std::uint64_t activeSequence,
	std::uint64_t eventSequence)
{
	return activeSequence != 0 && eventSequence != 0 &&
		activeSequence == eventSequence;
}

inline bool InspectorTransformPublishAllowed(unsigned int ownerWidgetId,
	unsigned int changedWidgetId, bool tokenValid)
{
	return tokenValid && ownerWidgetId != 0 &&
		ownerWidgetId == changedWidgetId;
}
