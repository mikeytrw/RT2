// Geometry half of the editor icon overlay: projection, hit testing, and
// collecting the entities that get an icon. Deliberately free of ImGui so the
// test project — which does not link ImGui — can cover the parts that have
// arithmetic in them. Drawing lives in EditorViewportIconsDraw.cpp.
#include "EditorViewportIcons.h"

#include "ECSComponents.h"
#include "EditorSelection.h"
#include "SceneManager.h"

#include <algorithm>
#include <cmath>

namespace {

EditorIconKind KindForLight(LightType type)
{
	switch (type)
	{
	case LightType::Spot:        return EditorIconKind::SpotLight;
	case LightType::Directional: return EditorIconKind::DirectionalLight;
	case LightType::Point:       break;
	}
	return EditorIconKind::PointLight;
}

} // namespace

bool ProjectToViewport(const glm::vec3& worldPos, const glm::mat4& viewProj,
	const glm::vec2& imageMin, const glm::vec2& imageSize,
	glm::vec2& outScreen, float& outDepth)
{
	const glm::vec4 clip = viewProj * glm::vec4(worldPos, 1.0f);
	if (clip.w <= 1e-5f) return false;
	const glm::vec2 ndc = glm::vec2(clip) / clip.w;
	if (!std::isfinite(ndc.x) || !std::isfinite(ndc.y)) return false;
	outScreen = imageMin + (ndc * 0.5f + 0.5f) * imageSize;
	outDepth = clip.w;
	return true;
}

EditorIconHit HitTestEditorIcons(const std::vector<EditorIconPlacement>& icons,
	const glm::vec2& mouse, float radius)
{
	EditorIconHit result;
	float bestDepth = 0.0f;
	for (const auto& icon : icons)
	{
		const glm::vec2 delta = icon.screenPos - mouse;
		if (glm::dot(delta, delta) > radius * radius) continue;
		if (!result.hit || icon.viewDepth < bestDepth)
		{
			result.hit = true;
			result.entity = icon.entity;
			bestDepth = icon.viewDepth;
		}
	}
	return result;
}

std::vector<EditorIconPlacement> BuildEditorIconPlacements(
	const SceneManager& scene, const EditorSelection& selection,
	const glm::mat4& viewProj, const glm::vec2& imageMin,
	const glm::vec2& imageSize)
{
	std::vector<EditorIconPlacement> icons;
	if (imageSize.x <= 1.0f || imageSize.y <= 1.0f) return icons;

	const auto& registry = scene.GetECS().registry;

	// An icon whose centre is just off the edge should still be clickable
	// where it overlaps the image, so cull with the hit radius as slack.
	const glm::vec2 lo = imageMin - glm::vec2(kEditorIconRadius);
	const glm::vec2 hi = imageMin + imageSize + glm::vec2(kEditorIconRadius);

	auto emit = [&](entt::entity entity, EditorIconKind kind,
		const glm::mat4& world, const glm::vec3& localAim) -> void
	{
		EditorIconPlacement icon;
		icon.kind = kind;
		const glm::vec3 worldPos(world[3]);
		if (!ProjectToViewport(worldPos, viewProj, imageMin, imageSize,
			icon.screenPos, icon.viewDepth))
			return;
		if (icon.screenPos.x < lo.x || icon.screenPos.x > hi.x ||
			icon.screenPos.y < lo.y || icon.screenPos.y > hi.y)
			return;

		// Aim is the screen-space delta to a point one unit along the entity's
		// forward axis. Projecting the far point rather than rotating the
		// direction keeps perspective foreshortening, so a light aimed at the
		// camera draws a short stub rather than a full-length arrow.
		const glm::vec3 aimWorld = worldPos + glm::mat3(world) * localAim;
		glm::vec2 aimScreen;
		float aimDepth = 0.0f;
		if (ProjectToViewport(aimWorld, viewProj, imageMin, imageSize, aimScreen, aimDepth))
			icon.screenAim = aimScreen - icon.screenPos;

		icon.entity = scene.GetEntityUuid(SceneManager::EntityId{ entity });
		if (icon.entity.IsNull()) return;
		icon.selected = selection.Contains(icon.entity);
		icons.push_back(icon);
	};

	// glTF punctual lights emit along local -Z; CameraComponent carries its own
	// forward direction, already in the entity's local frame.
	for (auto [entity, light, transform] :
		registry.view<const LightComponent, const Transform>().each())
		emit(entity, KindForLight(light.type), transform.worldMatrix,
			glm::vec3(0.0f, 0.0f, -1.0f));

	for (auto [entity, cam, transform] :
		registry.view<const CameraComponent, const Transform>().each())
		emit(entity, EditorIconKind::Camera, transform.worldMatrix, cam.forwardDirection);

	// Far-to-near: later draws land on top, so the nearest icon wins visually,
	// matching what the hit test picks.
	std::stable_sort(icons.begin(), icons.end(),
		[](const EditorIconPlacement& a, const EditorIconPlacement& b) {
			return a.viewDepth > b.viewDepth;
		});
	return icons;
}
