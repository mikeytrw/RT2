#pragma once

#include "EditorSelection.h"
#include "TransformEditing.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

class Camera;
class SceneManager;

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
};

// Lightweight viewport transform gizmo. It deliberately owns editor-only
// interaction state while all authored mutations still pass through
// SceneManager's validated transform APIs.
class EditorTransformGizmo
{
public:
	TransformGizmoResult Draw(SceneManager& scene, const EditorSelection& selection,
		const Camera& camera, const glm::vec2& imageMin, const glm::vec2& imageSize,
		bool imageHovered, bool editable, TransformSpace space, TransformPivot pivot,
		const TransformSnapSettings& snap);

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
	};

	TransformGizmoOperation m_Operation = TransformGizmoOperation::Translate;
	DragState m_Drag;
};
