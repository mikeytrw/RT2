#include "EditorTransformGizmo.h"

#include "Camera.h"

#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr float kHandlePixels = 82.0f;
constexpr float kHitRadius = 9.0f;

glm::vec3 UnitAxis(int axis)
{
	glm::vec3 result(0.0f);
	result[axis] = 1.0f;
	return result;
}

float DistanceToSegment(const glm::vec2& point, const glm::vec2& start,
	const glm::vec2& end)
{
	const glm::vec2 segment = end - start;
	const float lengthSquared = glm::dot(segment, segment);
	if (lengthSquared <= 1e-6f) return glm::length(point - start);
	const float t = glm::clamp(glm::dot(point - start, segment) / lengthSquared,
		0.0f, 1.0f);
	return glm::length(point - (start + segment * t));
}

bool ProjectPoint(const glm::vec3& point, const Camera& camera,
	const glm::vec2& imageMin, const glm::vec2& imageSize, glm::vec2& out)
{
	const glm::vec4 clip = camera.GetProjection() * camera.GetView() * glm::vec4(point, 1.0f);
	if (clip.w <= 1e-5f) return false;
	const glm::vec2 ndc = glm::vec2(clip) / clip.w;
	if (!std::isfinite(ndc.x) || !std::isfinite(ndc.y)) return false;
	out = imageMin + (ndc * 0.5f + 0.5f) * imageSize;
	return true;
}

glm::mat4 AroundPivot(const glm::vec3& pivot, const glm::mat4& linear)
{
	return glm::translate(glm::mat4(1.0f), pivot) * linear *
		glm::translate(glm::mat4(1.0f), -pivot);
}

glm::mat4 AxisScale(const glm::quat& orientation, int axis, float factor)
{
	glm::vec3 values(1.0f);
	values[axis] = factor;
	const glm::mat4 basis = glm::mat4_cast(orientation);
	return basis * glm::scale(glm::mat4(1.0f), values) * glm::inverse(basis);
}

// Uniform scale about a pivot: scales all three axes by `factor` in the
// orientation's basis. Used when the host enables uniform-scale mode.
glm::mat4 UniformScale(const glm::quat& orientation, float factor)
{
	const glm::mat4 basis = glm::mat4_cast(orientation);
	return basis * glm::scale(glm::mat4(1.0f), glm::vec3(factor)) *
		glm::inverse(basis);
}

} // namespace

void EditorTransformGizmo::Cancel()
{
	m_Drag = {};
}

TransformGizmoResult EditorTransformGizmo::Draw(
	const std::optional<EditorWorldTransformSnapshot>& snapshot,
	const EditorSelection& selection, const Camera& camera,
	const glm::vec2& imageMin, const glm::vec2& imageSize, bool imageHovered,
	bool editable, TransformSpace space, TransformPivot pivot,
	const TransformSnapSettings& snap, bool uniformScale)
{
	TransformGizmoResult result;
	if (m_Drag.active && (ImGui::IsKeyPressed(ImGuiKey_Escape) ||
		imageSize.x <= 1.0f || imageSize.y <= 1.0f))
	{
		result.active = true;
		result.interactionSequence = m_Drag.interactionSequence;
		result.dragCancelled = true;
		Cancel();
		return result;
	}
	if ((!editable && !m_Drag.active) || imageSize.x <= 1.0f || imageSize.y <= 1.0f)
	{
		return result;
	}
	if (!m_Drag.active && selection.Empty()) return result;

	// Operation toolbar. W/E/R also switch modes while the viewport is hovered.
	const ImVec2 savedCursor = ImGui::GetCursorScreenPos();
	ImGui::SetCursorScreenPos(ImVec2(imageMin.x + 10.0f, imageMin.y + 10.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(3.0f, 0.0f));
	if (ImGui::Button(m_Operation == TransformGizmoOperation::Translate ? "[Z] Move" : "Z Move"))
		m_Operation = TransformGizmoOperation::Translate;
	result.consumesMouse |= ImGui::IsItemHovered();
	ImGui::SameLine();
	if (ImGui::Button(m_Operation == TransformGizmoOperation::Rotate ? "[X] Rotate" : "X Rotate"))
		m_Operation = TransformGizmoOperation::Rotate;
	result.consumesMouse |= ImGui::IsItemHovered();
	ImGui::SameLine();
	if (ImGui::Button(m_Operation == TransformGizmoOperation::Scale ? "[C] Scale" : "C Scale"))
		m_Operation = TransformGizmoOperation::Scale;
	result.consumesMouse |= ImGui::IsItemHovered();
	ImGui::PopStyleVar();
	ImGui::SetCursorScreenPos(savedCursor);
	if (imageHovered && !m_Drag.active && !ImGui::GetIO().WantTextInput)
	{
		if (ImGui::IsKeyPressed(ImGuiKey_Z)) m_Operation = TransformGizmoOperation::Translate;
		if (ImGui::IsKeyPressed(ImGuiKey_X)) m_Operation = TransformGizmoOperation::Rotate;
		if (ImGui::IsKeyPressed(ImGuiKey_C)) m_Operation = TransformGizmoOperation::Scale;
	}

	std::vector<rt2::core::UUID> liveUuids;
	std::vector<glm::mat4> liveWorld;
	std::size_t primaryIndex = 0;
	if (m_Drag.active)
	{
		liveUuids = m_Drag.uuids;
		liveWorld = m_Drag.startWorld;
		primaryIndex = std::min(m_Drag.primaryIndex,
			liveWorld.empty() ? std::size_t(0) : liveWorld.size() - 1);
	}
	else if (snapshot && !snapshot->Empty())
	{
		liveUuids = snapshot->uuids;
		liveWorld = snapshot->worldMatrices;
		primaryIndex = snapshot->primaryIndex;
	}
	else return result;
	if (liveWorld.empty() || liveWorld.size() != liveUuids.size()) return result;

	const glm::vec3 pivotPosition = ComputeTransformPivot(liveWorld, pivot, primaryIndex);
	EditableTRS primaryWorld;
	if (!TryDecomposeEditableTRS(liveWorld[primaryIndex], primaryWorld)) return result;
	const glm::quat pivotRotation = space == TransformSpace::Local
		? primaryWorld.rotation : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	const float cameraDistance = glm::length(camera.GetPosition() - pivotPosition);
	const float worldHandleLength = std::max(0.15f, cameraDistance * 0.12f);

	glm::vec2 pivotScreen;
	if (!ProjectPoint(pivotPosition, camera, imageMin, imageSize, pivotScreen)) return result;
	glm::vec2 axisEnds[3];
	bool projected[3]{};
	for (int axis = 0; axis < 3; ++axis)
	{
		const glm::vec3 axisWorld = pivotRotation * UnitAxis(axis);
		projected[axis] = ProjectPoint(pivotPosition + axisWorld * worldHandleLength,
			camera, imageMin, imageSize, axisEnds[axis]);
		if (projected[axis])
		{
			const glm::vec2 direction = axisEnds[axis] - pivotScreen;
			const float length = glm::length(direction);
			if (length > 1e-3f)
				axisEnds[axis] = pivotScreen + direction * (kHandlePixels / length);
		}
	}

	const glm::vec2 mouse(ImGui::GetMousePos().x, ImGui::GetMousePos().y);
	int hoveredAxis = -1;
	float bestDistance = kHitRadius;
	if (imageHovered && !result.consumesMouse && !m_Drag.active)
	{
		for (int axis = 0; axis < 3; ++axis)
		{
			if (!projected[axis]) continue;
			const float distance = DistanceToSegment(mouse, pivotScreen, axisEnds[axis]);
			if (distance < bestDistance)
			{
				bestDistance = distance;
				hoveredAxis = axis;
			}
		}
	}
	if (m_Drag.active) hoveredAxis = m_Drag.axis;
	result.interactionSequence = m_Drag.interactionSequence;

	ImDrawList* draw = ImGui::GetWindowDrawList();
	const ImU32 colors[] = {
		IM_COL32(230, 65, 65, 255), IM_COL32(70, 210, 90, 255),
		IM_COL32(70, 130, 245, 255)
	};
	for (int axis = 0; axis < 3; ++axis)
	{
		if (!projected[axis]) continue;
		const ImU32 color = axis == hoveredAxis ? IM_COL32(255, 220, 45, 255) : colors[axis];
		draw->AddLine(ImVec2(pivotScreen.x, pivotScreen.y),
			ImVec2(axisEnds[axis].x, axisEnds[axis].y), color,
			axis == hoveredAxis ? 4.0f : 3.0f);
		if (m_Operation == TransformGizmoOperation::Scale)
			draw->AddRectFilled(ImVec2(axisEnds[axis].x - 5.0f, axisEnds[axis].y - 5.0f),
				ImVec2(axisEnds[axis].x + 5.0f, axisEnds[axis].y + 5.0f), color);
		else
			draw->AddCircleFilled(ImVec2(axisEnds[axis].x, axisEnds[axis].y),
				m_Operation == TransformGizmoOperation::Rotate ? 6.0f : 4.0f, color);
	}
	result.consumesMouse |= hoveredAxis >= 0 || m_Drag.active;

	if (!m_Drag.active && hoveredAxis >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		m_Drag = {};
		m_Drag.interactionSequence = ++m_NextInteractionSequence;
		result.dragJustStarted = true;
		result.interactionSequence = m_Drag.interactionSequence;
		m_Drag.active = true;
		m_Drag.axis = hoveredAxis;
		m_Drag.operation = m_Operation;
		m_Drag.space = space;
		m_Drag.pivot = pivot;
		m_Drag.startMouse = mouse;
		m_Drag.axisWorld = pivotRotation * UnitAxis(hoveredAxis);
		m_Drag.pivotRotation = pivotRotation;
		m_Drag.startPivot = glm::translate(glm::mat4(1.0f), pivotPosition) *
			glm::mat4_cast(pivotRotation);
		m_Drag.worldHandleLength = worldHandleLength;
		m_Drag.pixelHandleLength = glm::max(1.0f,
			glm::length(axisEnds[hoveredAxis] - pivotScreen));
		m_Drag.dragScreenDirection = glm::normalize(axisEnds[hoveredAxis] - pivotScreen);
		if (m_Operation == TransformGizmoOperation::Rotate)
			m_Drag.dragScreenDirection = {
				-m_Drag.dragScreenDirection.y, m_Drag.dragScreenDirection.x
			};
		// Phase 3B1: capture the before-drag local TRS for each entity so
		// the host can build a multi-entity TransformCommand on drag end.
		// Exclude entities that fail GetLocalTransform — recording identity
		// as their "before" would cause Undo to slam them to identity.
		m_Drag.uuids = liveUuids;
		m_Drag.startWorld = liveWorld;
		m_Drag.startLocal.clear();
		m_Drag.primaryIndex = primaryIndex;
		result.draggedUuids = m_Drag.uuids;
	}

	const auto release = ClassifyTransformGizmoRelease({
		m_Drag.active,
		ImGui::IsMouseDown(ImGuiMouseButton_Left),
		m_Drag.moved,
		m_Drag.interactionSequence,
		m_Drag.uuids,
		m_Drag.startLocal,
	});
	if (release)
	{
		result = *release;
		Cancel();
	}
	if (!m_Drag.active) return result;

	result.active = true;
	const float pixels = glm::dot(mouse - m_Drag.startMouse, m_Drag.dragScreenDirection);
	if (!m_Drag.moved &&
		!ExceedsTransformDragThreshold(m_Drag.startMouse, mouse))
		return result;
	m_Drag.moved = true;
	const float worldDelta = pixels * m_Drag.worldHandleLength / m_Drag.pixelHandleLength;
	glm::mat4 sharedDelta(1.0f);
	float operationAmount = worldDelta;
	if (m_Drag.operation == TransformGizmoOperation::Translate)
	{
		if (snap.enabled) operationAmount = SnapValue(operationAmount, snap.translation);
		sharedDelta = glm::translate(glm::mat4(1.0f),
			m_Drag.axisWorld * operationAmount);
	}
	else if (m_Drag.operation == TransformGizmoOperation::Rotate)
	{
		operationAmount = pixels * 0.5f;
		if (snap.enabled) operationAmount = SnapValue(operationAmount, snap.rotationDegrees);
		sharedDelta = AroundPivot(glm::vec3(m_Drag.startPivot[3]),
			glm::rotate(glm::mat4(1.0f), glm::radians(operationAmount),
				m_Drag.axisWorld));
	}
	else
	{
		operationAmount = 1.0f + worldDelta / m_Drag.worldHandleLength;
		if (snap.enabled) operationAmount = SnapValue(operationAmount, snap.scale);
		if (std::abs(operationAmount) < 0.01f)
			operationAmount = operationAmount < 0.0f ? -0.01f : 0.01f;
		sharedDelta = AroundPivot(glm::vec3(m_Drag.startPivot[3]),
			uniformScale
				? UniformScale(m_Drag.pivotRotation, operationAmount)
				: AxisScale(m_Drag.pivotRotation, m_Drag.axis, operationAmount));
	}

	std::vector<glm::mat4> editedWorld;
	if (m_Drag.pivot == TransformPivot::Individual)
	{
		editedWorld.reserve(m_Drag.startWorld.size());
		for (const glm::mat4& startWorld : m_Drag.startWorld)
		{
			EditableTRS start;
			if (!TryDecomposeEditableTRS(startWorld, start))
			{
				result.error = "Gizmo edit rejected: an individual transform contains shear.";
				return result;
			}
			const glm::quat orientation = m_Drag.space == TransformSpace::Local
				? start.rotation : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			const glm::vec3 axisWorld = orientation * UnitAxis(m_Drag.axis);
			glm::mat4 delta(1.0f);
			if (m_Drag.operation == TransformGizmoOperation::Translate)
				delta = glm::translate(glm::mat4(1.0f), axisWorld * operationAmount);
			else if (m_Drag.operation == TransformGizmoOperation::Rotate)
				delta = AroundPivot(start.translation,
					glm::rotate(glm::mat4(1.0f), glm::radians(operationAmount), axisWorld));
			else
				delta = AroundPivot(start.translation,
					uniformScale
						? UniformScale(orientation, operationAmount)
						: AxisScale(orientation, m_Drag.axis, operationAmount));
			editedWorld.push_back(delta * startWorld);
		}
	}
	else if (!TryApplySharedPivotDelta(m_Drag.startPivot,
		sharedDelta * m_Drag.startPivot, m_Drag.startWorld, editedWorld))
	{
		result.error = "Gizmo edit rejected: the pivot transform is singular.";
		return result;
	}
	result.desiredWorld.reserve(editedWorld.size());
	for (std::size_t i = 0; i < editedWorld.size(); ++i)
		result.desiredWorld.emplace_back(m_Drag.uuids[i], editedWorld[i]);
	result.changed = true;
	return result;
}
