#pragma once
#include <glm/glm.hpp>
#include <cstdint>
#include <utility>

class Camera
{
public:

	Camera() {};
	Camera(float verticalFOV, float nearClip, float farClip, float apeture, float focalDistance);

	bool OnUpdate(float ts);
	void OnResize(uint32_t width, uint32_t height);

	const glm::mat4& GetProjection() const { return m_Projection; }
	const glm::mat4& GetInverseProjection() const { return m_InverseProjection; }
	const glm::mat4& GetView() const { return m_View; }
	const glm::mat4& GetInverseView() const { return m_InverseView; }

	const glm::vec3& GetPosition() const { return m_Position; }
	const glm::vec3& GetDirection() const { return m_ForwardDirection; }

	void SetPosition(const glm::vec3& pos) { m_Position = pos; RecalculateView(); }
	void SetForwardDirection(const glm::vec3& dir) { m_ForwardDirection = dir; RecalculateView(); }

	const bool checkHasMoved() { 
		if (mHasMoved) { 
			mHasMoved = false; 
			return true; 
		} 
		return false; 
	};

	glm::vec3 RandomInUnitDisk() const;
	std::pair<glm::vec3, glm::vec3> GetRayOriginAndDirection(float u, float v) const;

	float GetRotationSpeed();

	float m_Aperture = 0.0f;
	float m_FocusDistance = 1.0f;
private:
	void RecalculateProjection();
	void RecalculateView();
private:
	glm::mat4 m_Projection{ 1.0f };
	glm::mat4 m_View{ 1.0f };
	glm::mat4 m_InverseProjection{ 1.0f };
	glm::mat4 m_InverseView{ 1.0f };

	float m_VerticalFOV = 45.0f;
	float m_NearClip = 0.1f;
	float m_FarClip = 100.0f;

	glm::vec3 m_Position{ 0.0f, 0.0f, 0.0f };
	glm::vec3 m_ForwardDirection{ 0.0f, 0.0f, 0.0f };
	glm::vec3 m_RightDirection{ 1.0f, 0.0f, 0.0f };

	glm::vec2 m_LastMousePosition{ 0.0f, 0.0f };

	uint32_t m_ViewportWidth = 0, m_ViewportHeight = 0;

	bool mHasMoved = false;

	
};
