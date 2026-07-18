#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <vector>

enum class TransformSpace
{
	Local,
	World,
};

enum class TransformPivot
{
	Primary,
	Median,
	Individual,
};

struct TransformSnapSettings
{
	bool enabled = false;
	float translation = 0.5f;
	float rotationDegrees = 15.0f;
	float scale = 0.1f;
};

struct EditableTRS
{
	glm::vec3 translation{ 0.0f };
	glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
	glm::vec3 scale{ 1.0f };

	glm::mat4 Matrix() const;
};

// Strict affine TRS decomposition used by hierarchy/world-space editing.
// Singular matrices and matrices containing shear are rejected rather than
// silently converted into a different transform.
bool TryDecomposeEditableTRS(const glm::mat4& matrix, EditableTRS& out,
	float epsilon = 1e-5f, float shearTolerance = 1e-4f);

// Convert an authored world matrix to a local TRS relative to parentWorld.
// Returns false for a singular parent or a local result containing shear.
bool TryWorldToLocalTRS(const glm::mat4& parentWorld,
	const glm::mat4& desiredWorld, EditableTRS& outLocal);

float SnapValue(float value, float increment);
glm::vec3 SnapValues(const glm::vec3& values, float increment);

// D = currentPivot * inverse(startPivot); Wi' = D * Wi.
// Returns false when the starting pivot is singular.
bool TryApplySharedPivotDelta(const glm::mat4& startPivot,
	const glm::mat4& currentPivot, const std::vector<glm::mat4>& startWorld,
	std::vector<glm::mat4>& outWorld);

glm::vec3 ComputeTransformPivot(const std::vector<glm::mat4>& worldMatrices,
	TransformPivot mode, std::size_t primaryIndex);

// Gizmo handles only claim manipulation after intentional pointer movement;
// a press/release below this threshold remains a viewport selection click.
bool ExceedsTransformDragThreshold(const glm::vec2& start, const glm::vec2& current,
	float thresholdPixels = 4.0f);
