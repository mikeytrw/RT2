#include "EditorTransformGizmo.h"

#include "Camera.h"
#include "SceneManager.h"

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

} // namespace

void EditorTransformGizmo::Cancel()
{
	m_Drag = {};
}

TransformGizmoResult EditorTransformGizmo::Draw(SceneManager& scene,
	const EditorSelection& selection, const Camera& camera,
	const glm::vec2& imageMin, const glm::vec2& imageSize, bool imageHovered,
	bool editable, TransformSpace space, TransformPivot pivot,
	const TransformSnapSettings& snap)
{
	TransformGizmoResult result;
	if (!editable || selection.Empty() || imageSize.x <= 1.0f || imageSize.y <= 1.0f)
	{
		Cancel();
		return result;
	}

	// Operation toolbar. W/E/R also switch modes while the viewport is hovered.
	const ImVec2 savedCursor = ImGui::GetCursorScreenPos();
	ImGui::SetCursorScreenPos(ImVec2(imageMin.x + 10.0f, imageMin.y + 10.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(3.0f, 0.0f));
	if (ImGui::Button(m_Operation == TransformGizmoOperation::Translate ? "[W] Move" : "W Move"))
		m_Operation = TransformGizmoOperation::Translate;
	result.consumesMouse |= ImGui::IsItemHovered();
	ImGui::SameLine();
	if (ImGui::Button(m_Operation == TransformGizmoOperation::Rotate ? "[E] Rotate" : "E Rotate"))
		m_Operation = TransformGizmoOperation::Rotate;
	result.consumesMouse |= ImGui::IsItemHovered();
	ImGui::SameLine();
	if (ImGui::Button(m_Operation == TransformGizmoOperation::Scale ? "[R] Scale" : "R Scale"))
		m_Operation = TransformGizmoOperation::Scale;
	result.consumesMouse |= ImGui::IsItemHovered();
	ImGui::PopStyleVar();
	ImGui::SetCursorScreenPos(savedCursor);
	if (imageHovered && !m_Drag.active && !ImGui::GetIO().WantTextInput)
	{
		if (ImGui::IsKeyPressed(ImGuiKey_W)) m_Operation = TransformGizmoOperation::Translate;
		if (ImGui::IsKeyPressed(ImGuiKey_E)) m_Operation = TransformGizmoOperation::Rotate;
		if (ImGui::IsKeyPressed(ImGuiKey_R)) m_Operation = TransformGizmoOperation::Scale;
	}

	std::vector<rt2::core::UUID> liveUuids;
	std::vector<glm::mat4> liveWorld;
	liveUuids.reserve(selection.Size());
	liveWorld.reserve(selection.Size());
	std::size_t primaryIndex = 0;
	for (const auto& uuid : selection.Ordered())
	{
		const entt::entity raw = scene.FindEntityByUuid(uuid);
		if (raw == entt::null) continue;
		EditableTRS world;
		if (!scene.GetWorldTransform({ raw }, world)) continue;
		if (uuid == selection.Primary()) primaryIndex = liveWorld.size();
		liveUuids.push_back(uuid);
		liveWorld.push_back(world.Matrix());
	}
	if (liveWorld.empty())
	{
		Cancel();
		return result;
	}

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
		m_Drag.uuids = std::move(liveUuids);
		m_Drag.startWorld = std::move(liveWorld);
	}

	if (m_Drag.active && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
	{
		// Treat a handle press/release without a drag as an ordinary viewport
		// selection click. This keeps the handles from masking small or nearby
		// objects underneath them.
		result.pickThrough = !m_Drag.moved;
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
			AxisScale(m_Drag.pivotRotation, m_Drag.axis, operationAmount));
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
					AxisScale(orientation, m_Drag.axis, operationAmount));
			editedWorld.push_back(delta * startWorld);
		}
	}
	else if (!TryApplySharedPivotDelta(m_Drag.startPivot,
		sharedDelta * m_Drag.startPivot, m_Drag.startWorld, editedWorld))
	{
		result.error = "Gizmo edit rejected: the pivot transform is singular.";
		return result;
	}
	std::vector<std::pair<SceneManager::EntityId, glm::mat4>> edits;
	edits.reserve(editedWorld.size());
	for (std::size_t i = 0; i < editedWorld.size(); ++i)
	{
		const entt::entity raw = scene.FindEntityByUuid(m_Drag.uuids[i]);
		if (raw == entt::null)
		{
			Cancel();
			result.error = "Gizmo edit cancelled because the selection changed.";
			return result;
		}
		edits.push_back({ { raw }, editedWorld[i] });
	}
	if (!scene.TrySetWorldTransforms(edits))
	{
		result.error = "Gizmo edit rejected: a parent is singular or the result contains shear.";
		return result;
	}
	result.changed = true;
	return result;
}
