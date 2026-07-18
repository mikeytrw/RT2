#include "TransformEditing.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace {

float MaxAbsElement(const glm::mat4& matrix)
{
	float result = 0.0f;
	for (int column = 0; column < 4; ++column)
		for (int row = 0; row < 4; ++row)
			result = std::max(result, std::abs(matrix[column][row]));
	return result;
}

bool IsFinite(const glm::mat4& matrix)
{
	for (int column = 0; column < 4; ++column)
		for (int row = 0; row < 4; ++row)
			if (!std::isfinite(matrix[column][row])) return false;
	return true;
}

} // namespace

glm::mat4 EditableTRS::Matrix() const
{
	return glm::translate(glm::mat4(1.0f), translation) *
		glm::mat4_cast(rotation) * glm::scale(glm::mat4(1.0f), scale);
}

bool TryDecomposeEditableTRS(const glm::mat4& matrix, EditableTRS& out,
	float epsilon, float shearTolerance)
{
	if (!IsFinite(matrix) || std::abs(matrix[3][3] - 1.0f) > epsilon ||
		std::abs(matrix[0][3]) > epsilon || std::abs(matrix[1][3]) > epsilon ||
		std::abs(matrix[2][3]) > epsilon)
		return false;

	EditableTRS candidate;
	candidate.translation = glm::vec3(matrix[3]);
	glm::vec3 axes[] = {
		glm::vec3(matrix[0]), glm::vec3(matrix[1]), glm::vec3(matrix[2])
	};
	candidate.scale = {
		glm::length(axes[0]), glm::length(axes[1]), glm::length(axes[2])
	};
	if (candidate.scale.x <= epsilon || candidate.scale.y <= epsilon ||
		candidate.scale.z <= epsilon)
		return false;

	axes[0] /= candidate.scale.x;
	axes[1] /= candidate.scale.y;
	axes[2] /= candidate.scale.z;
	if (std::abs(glm::dot(axes[0], axes[1])) > shearTolerance ||
		std::abs(glm::dot(axes[0], axes[2])) > shearTolerance ||
		std::abs(glm::dot(axes[1], axes[2])) > shearTolerance)
		return false;

	float handedness = glm::dot(glm::cross(axes[0], axes[1]), axes[2]);
	if (std::abs(handedness) <= epsilon)
		return false;
	if (handedness < 0.0f)
	{
		int axis = 0;
		if (std::abs(candidate.scale.y) > std::abs(candidate.scale[axis])) axis = 1;
		if (std::abs(candidate.scale.z) > std::abs(candidate.scale[axis])) axis = 2;
		candidate.scale[axis] = -candidate.scale[axis];
		axes[axis] = -axes[axis];
	}

	glm::mat3 rotationMatrix(1.0f);
	rotationMatrix[0] = axes[0];
	rotationMatrix[1] = axes[1];
	rotationMatrix[2] = axes[2];
	candidate.rotation = glm::normalize(glm::quat_cast(rotationMatrix));

	const glm::mat4 reconstructed = candidate.Matrix();
	const float tolerance = std::max(epsilon, epsilon * (1.0f + MaxAbsElement(matrix)));
	for (int column = 0; column < 4; ++column)
		for (int row = 0; row < 4; ++row)
			if (std::abs(reconstructed[column][row] - matrix[column][row]) > tolerance)
				return false;

	out = candidate;
	return true;
}

bool TryWorldToLocalTRS(const glm::mat4& parentWorld,
	const glm::mat4& desiredWorld, EditableTRS& outLocal)
{
	if (!IsFinite(parentWorld) ||
		std::abs(glm::determinant(glm::mat3(parentWorld))) <= 1e-8f)
		return false;
	return TryDecomposeEditableTRS(glm::inverse(parentWorld) * desiredWorld, outLocal);
}

float SnapValue(float value, float increment)
{
	if (!std::isfinite(value) || !std::isfinite(increment) || increment <= 0.0f)
		return value;
	return std::round(value / increment) * increment;
}

glm::vec3 SnapValues(const glm::vec3& values, float increment)
{
	return { SnapValue(values.x, increment), SnapValue(values.y, increment),
	         SnapValue(values.z, increment) };
}

bool TryApplySharedPivotDelta(const glm::mat4& startPivot,
	const glm::mat4& currentPivot, const std::vector<glm::mat4>& startWorld,
	std::vector<glm::mat4>& outWorld)
{
	if (std::abs(glm::determinant(glm::mat3(startPivot))) <= 1e-8f)
		return false;
	const glm::mat4 delta = currentPivot * glm::inverse(startPivot);
	outWorld.clear();
	outWorld.reserve(startWorld.size());
	for (const glm::mat4& world : startWorld)
		outWorld.push_back(delta * world);
	return true;
}

glm::vec3 ComputeTransformPivot(const std::vector<glm::mat4>& worldMatrices,
	TransformPivot mode, std::size_t primaryIndex)
{
	if (worldMatrices.empty()) return glm::vec3(0.0f);
	if (mode == TransformPivot::Primary || mode == TransformPivot::Individual)
	{
		const std::size_t index = std::min(primaryIndex, worldMatrices.size() - 1);
		return glm::vec3(worldMatrices[index][3]);
	}
	glm::vec3 sum(0.0f);
	for (const glm::mat4& world : worldMatrices)
		sum += glm::vec3(world[3]);
	return sum / static_cast<float>(worldMatrices.size());
}

bool ExceedsTransformDragThreshold(const glm::vec2& start, const glm::vec2& current,
	float thresholdPixels)
{
	if (!std::isfinite(thresholdPixels) || thresholdPixels < 0.0f) return false;
	const glm::vec2 delta = current - start;
	return glm::dot(delta, delta) >= thresholdPixels * thresholdPixels;
}
