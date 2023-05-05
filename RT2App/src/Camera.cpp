#include "Camera.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/random.hpp>
#include "Walnut/Input/Input.h"
#include "Walnut/Random.h"



#include <iostream>
using namespace Walnut;


Camera::Camera(float verticalFOV, float nearClip, float farClip, float apeture, float focalDistance)
	: m_VerticalFOV(verticalFOV), m_NearClip(nearClip), m_FarClip(farClip), m_Aperture(apeture),m_FocusDistance(focalDistance)
{
	m_ForwardDirection = glm::vec3(0,0, -1);
	m_Position = glm::vec3(0, 1, 10);

	m_Aperture = 0.05f;
}

bool Camera::OnUpdate(float ts)
{
	glm::vec2 mousePos = Input::GetMousePosition();
	glm::vec2 delta = (mousePos - m_LastMousePosition) * 0.002f;
	m_LastMousePosition = mousePos;

	if (!Input::IsMouseButtonDown(MouseButton::Right))
	{
		Input::SetCursorMode(CursorMode::Normal);
		mHasMoved = false;
		return false;
	}

	Input::SetCursorMode(CursorMode::Locked);

	bool moved = false;

	constexpr glm::vec3 upDirection(0.0f, 1.0f, 0.0f);
	m_RightDirection = glm::cross(m_ForwardDirection, upDirection);

	float speed = 5.0f;

	// Movement
	if (Input::IsKeyDown(KeyCode::W))
	{
		m_Position += m_ForwardDirection * speed * ts;
		moved = true;
	}
	else if (Input::IsKeyDown(KeyCode::S))
	{
		m_Position -= m_ForwardDirection * speed * ts;
		moved = true;
	}
	if (Input::IsKeyDown(KeyCode::A))
	{
		m_Position -= m_RightDirection * speed * ts;
		moved = true;
	}
	else if (Input::IsKeyDown(KeyCode::D))
	{
		m_Position += m_RightDirection * speed * ts;
		moved = true;
	}
	if (Input::IsKeyDown(KeyCode::Q))
	{
		m_Position -= upDirection * speed * ts;
		moved = true;
	}
	else if (Input::IsKeyDown(KeyCode::E))
	{
		m_Position += upDirection * speed * ts;
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
		RecalculateRayDirections();
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
	//RecalculateRayDirections();
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

glm::vec3& Camera::getRayDirection(float u, float v) {
	
	glm::vec2 coord = { u , v };
	coord = coord * 2.0f - 1.0f; // -1 -> 1

	glm::vec4 target = m_InverseProjection * glm::vec4(coord.x, coord.y, 1, 1);
	glm::vec3 rayDirection = glm::vec3(m_InverseView * glm::vec4(glm::normalize(glm::vec3(target) / target.w), 0)); // World space
	
	return rayDirection;
	

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

void Camera::RecalculateRayDirections()
{
	return;

	/*
	m_RayDirections.resize(m_ViewportWidth * m_ViewportHeight);

	for (uint32_t y = 0; y < m_ViewportHeight; y++)
	{
		for (uint32_t x = 0; x < m_ViewportWidth; x++)
		{
			glm::vec2 coord = { (float)x / (float)m_ViewportWidth, (float)y / (float)m_ViewportHeight };
			coord = coord * 2.0f - 1.0f; // -1 -> 1

			glm::vec4 target = m_InverseProjection * glm::vec4(coord.x, coord.y, 1, 1);
			glm::vec3 rayDirection = glm::vec3(m_InverseView * glm::vec4(glm::normalize(glm::vec3(target) / target.w), 0)); // World space
			m_RayDirections[x + y * m_ViewportWidth] = rayDirection;
		}
	}
	*/
}