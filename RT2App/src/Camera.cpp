#include "Camera.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/random.hpp>
#include "Walnut/Random.h"
#include "InputTypes.h"
#include "InputService.h"


#include <iostream>


Camera::Camera(float verticalFOV, float nearClip, float farClip, float apeture, float focalDistance)
	: m_VerticalFOV(verticalFOV), m_NearClip(nearClip), m_FarClip(farClip), m_Aperture(apeture), m_FocusDistance(focalDistance)
{
	m_ForwardDirection = glm::vec3(0, 0, -1);
	m_Position = glm::vec3(0, 1, 10);
}

EditorCameraPose Camera::GetEditorPose() const
{
	EditorCameraPose pose;
	pose.position = m_Position;
	pose.forward = m_ForwardDirection;
	pose.verticalFOV = m_VerticalFOV;
	pose.aperture = m_Aperture;
	pose.focusDistance = m_FocusDistance;
	pose.farClip = m_FarClip;
	return pose;
}

bool Camera::SetEditorPose(const EditorCameraPose& requested)
{
	EditorCameraPose pose = requested;
	if (!TryNormalizeEditorCameraPose(pose))
		return false;
	m_Position = pose.position;
	m_ForwardDirection = pose.forward;
	m_VerticalFOV = pose.verticalFOV;
	m_Aperture = pose.aperture;
	m_FocusDistance = pose.focusDistance;
	m_FarClip = pose.farClip;
	RecalculateView();
	if (m_ViewportWidth > 0 && m_ViewportHeight > 0)
		RecalculateProjection();
	return true;
}

bool Camera::OnUpdate(float ts, rt2::core::IInputService& input)
{
	using namespace rt2::core;
	glm::vec2 delta = input.GetMouseDelta() * 0.002f;

	if (!input.IsDown("look"))
	{
		input.RequestCursorCapture(false);
		mHasMoved = false;
		return false;
	}

	input.RequestCursorCapture(true);

	bool moved = false;

	constexpr glm::vec3 upDirection(0.0f, 1.0f, 0.0f);
	m_RightDirection = glm::cross(m_ForwardDirection, upDirection);

	float speed = m_Speed;

	// Movement — driven by named axes from the input service.
	float forward = input.GetAxisValue("move_forward");
	float right   = input.GetAxisValue("move_right");
	float up      = input.GetAxisValue("move_up");

	if (forward != 0.0f)
	{
		m_Position += m_ForwardDirection * forward * speed * ts;
		moved = true;
	}
	if (right != 0.0f)
	{
		m_Position += m_RightDirection * right * speed * ts;
		moved = true;
	}
	if (up != 0.0f)
	{
		m_Position += upDirection * up * speed * ts;
		moved = true;
	}

	// Rotation
	if (delta.x != 0.0f || delta.y != 0.0f)
	{
		float pitchDelta = delta.y * GetRotationSpeed();
		float yawDelta = delta.x * GetRotationSpeed();

		glm::quat q = glm::normalize(glm::cross(glm::angleAxis(-pitchDelta, m_RightDirection),
			glm::angleAxis(-yawDelta, glm::vec3(0.f, 1.0f, 0.0f))));
		m_ForwardDirection = glm::rotate(q, m_ForwardDirection);

		moved = true;
	}

	if (moved)
	{
		mHasMoved = true;
		RecalculateView();
	}

	return moved;
}

void Camera::OnResize(uint32_t width, uint32_t height)
{
	if (width == m_ViewportWidth && height == m_ViewportHeight)
		return;

	m_ViewportWidth = width;
	m_ViewportHeight = height;

	RecalculateProjection();
}

float Camera::GetRotationSpeed()
{
	return 1.0f;
}

glm::vec3 Camera::RandomInUnitDisk() const {
	static thread_local std::mt19937 generator;
	std::uniform_real_distribution<float> distribution(-1.0f, 1.0f);

	glm::vec3 p;
	do {
		p = glm::vec3(distribution(generator), distribution(generator), 0.0f);
	} while (glm::dot(p, p) >= 1.0f);
	return p;
}

std::pair<glm::vec3, glm::vec3> Camera::GetRayOriginAndDirection(float u, float v) const {
	glm::vec2 coord = { u , v };
	coord = coord * 2.0f - 1.0f; // -1 -> 1

	glm::vec4 target = m_InverseProjection * glm::vec4(coord.x, coord.y, 1, 1);
	glm::vec3 rayDirection = glm::vec3(m_InverseView * glm::vec4(glm::normalize(glm::vec3(target) / target.w), 0)); // World space

	// Calculate lens offset for depth of field
	glm::vec3 lensOffset = m_Aperture / 2.0f * RandomInUnitDisk();
	glm::vec3 offset = m_RightDirection * lensOffset.x + glm::cross(m_ForwardDirection, m_RightDirection) * lensOffset.y;

	// Calculate new ray origin and direction
	glm::vec3 rayOrigin = m_Position + offset;
	rayDirection = rayDirection * m_FocusDistance - offset;

	return { rayOrigin, rayDirection };
}

void Camera::RecalculateProjection()
{
	m_Projection = glm::perspectiveFov(glm::radians(m_VerticalFOV), (float)m_ViewportWidth, (float)m_ViewportHeight, m_NearClip, m_FarClip);
	m_Projection[1][1] *= -1.0f;
	m_InverseProjection = glm::inverse(m_Projection);
}

void Camera::RecalculateView()
{
	m_View = glm::lookAt(m_Position, m_Position + m_ForwardDirection, glm::vec3(0, 1, 0));
	m_InverseView = glm::inverse(m_View);
}

CameraRay Camera::GetPickingRay(float u, float v) const
{
	return BuildPickingRay(m_InverseProjection, m_InverseView, m_Position,
	                       glm::vec2(u, v));
}