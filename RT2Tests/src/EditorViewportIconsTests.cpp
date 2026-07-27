#include <doctest/doctest.h>

#include "ECSComponents.h"
#include "EditorSelection.h"
#include "EditorViewportIcons.h"
#include "SceneGraph.h"
#include "SceneManager.h"

#include <array>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// ============================================================================
// Editor viewport icons — projection and hit testing.
//
// Only the arithmetic is covered here; drawing lives in a separate TU that
// this project does not link, precisely so these can be tested at all.
//
// The behaviour worth pinning down is the depth ordering. Two lights along the
// same view ray project to the same pixel, and a hit test that returned the
// first match rather than the nearest would silently select the far one — a
// bug that looks like "clicking does nothing" because the selected entity is
// off-screen behind a wall.
// ============================================================================

namespace {

constexpr float kImageW = 800.0f;
constexpr float kImageH = 600.0f;
const glm::vec2 kImageMin{ 100.0f, 50.0f };
const glm::vec2 kImageSize{ kImageW, kImageH };

// Camera at the origin looking down -Z. This must reproduce
// Camera::RecalculateProjection exactly, including its Vulkan y-flip: without
// that flip the projection is y-up, screen space comes out mirrored, and every
// vertical assertion below silently tests the wrong convention.
glm::mat4 TestViewProj()
{
	const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f),
		glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
	glm::mat4 proj = glm::perspectiveFov(glm::radians(45.0f),
		kImageW, kImageH, 0.1f, 100.0f);
	proj[1][1] *= -1.0f;
	return proj * view;
}

rt2::core::UUID TestUuid(uint8_t tag)
{
	std::array<uint8_t, 16> bytes{};
	bytes[15] = tag;
	return rt2::core::UUID(bytes);
}

EditorIconPlacement MakeIcon(const glm::vec2& pos, float depth, uint8_t tag)
{
	EditorIconPlacement icon;
	icon.screenPos = pos;
	icon.viewDepth = depth;
	icon.entity = TestUuid(tag);
	return icon;
}

} // namespace

TEST_CASE("EditorViewportIcons: a point on the view axis projects to the image centre")
{
	glm::vec2 screen{ 0.0f };
	float depth = 0.0f;
	REQUIRE(ProjectToViewport({ 0.0f, 0.0f, -5.0f }, TestViewProj(),
		kImageMin, kImageSize, screen, depth));

	CHECK(screen.x == doctest::Approx(kImageMin.x + kImageW * 0.5f));
	CHECK(screen.y == doctest::Approx(kImageMin.y + kImageH * 0.5f));
	// Depth is clip-space w, which for this projection is the distance along
	// the camera's forward axis.
	CHECK(depth == doctest::Approx(5.0f));
}

TEST_CASE("EditorViewportIcons: points at or behind the camera are rejected")
{
	glm::vec2 screen{ 0.0f };
	float depth = 0.0f;
	const glm::mat4 vp = TestViewProj();

	CHECK_FALSE(ProjectToViewport({ 0.0f, 0.0f, 5.0f }, vp, kImageMin, kImageSize, screen, depth));
	CHECK_FALSE(ProjectToViewport({ 0.0f, 0.0f, 0.0f }, vp, kImageMin, kImageSize, screen, depth));
}

TEST_CASE("EditorViewportIcons: hit test misses outside the radius and hits inside it")
{
	std::vector<EditorIconPlacement> icons{ MakeIcon({ 400.0f, 300.0f }, 5.0f, 1) };

	CHECK(HitTestEditorIcons(icons, { 400.0f, 300.0f }).hit);
	// Just inside and just outside the disc, on the diagonal.
	const float inside = kEditorIconRadius * 0.7071f - 0.5f;
	const float outside = kEditorIconRadius * 0.7071f + 0.5f;
	CHECK(HitTestEditorIcons(icons, { 400.0f + inside, 300.0f + inside }).hit);
	CHECK_FALSE(HitTestEditorIcons(icons, { 400.0f + outside, 300.0f + outside }).hit);
	CHECK_FALSE(HitTestEditorIcons(icons, { 500.0f, 300.0f }).hit);
}

TEST_CASE("EditorViewportIcons: overlapping icons resolve to the nearest")
{
	const auto nearUuid = TestUuid(1);
	const auto farUuid = TestUuid(2);

	// Far one listed first: returning the first match would pick it.
	std::vector<EditorIconPlacement> icons{
		MakeIcon({ 400.0f, 300.0f }, 50.0f, 2),
		MakeIcon({ 402.0f, 301.0f }, 5.0f, 1),
	};
	CHECK(HitTestEditorIcons(icons, { 400.0f, 300.0f }).entity == nearUuid);

	// And the other way round, so the test cannot pass by list order alone.
	std::swap(icons[0], icons[1]);
	CHECK(HitTestEditorIcons(icons, { 400.0f, 300.0f }).entity == nearUuid);

	// Moving the mouse clear of the near icon falls through to the far one.
	std::vector<EditorIconPlacement> spread{
		MakeIcon({ 400.0f, 300.0f }, 50.0f, 2),
		MakeIcon({ 460.0f, 300.0f }, 5.0f, 1),
	};
	CHECK(HitTestEditorIcons(spread, { 400.0f, 300.0f }).entity == farUuid);
}

TEST_CASE("EditorViewportIcons: placements cover lights and cameras, sorted far to near")
{
	rt2::core::DeterministicUuidProvider ids;
	SceneManager manager;
	manager.SetUuidProvider(&ids);

	const auto nearLight = manager.GetEntityUuid(
		manager.AddLight("Near", { 0.0f, 0.0f, -4.0f }, { 1, 1, 1 }, 50.0f, LightType::Point));
	const auto farLight = manager.GetEntityUuid(
		manager.AddLight("Far", { 0.0f, 0.0f, -20.0f }, { 1, 1, 1 }, 50.0f, LightType::Spot));

	// A camera entity between the two, so the overlay is shown to cover both
	// component types and not just lights.
	const auto camUuid = manager.ReserveKnownUuid();
	REQUIRE(manager.CreateEmptyWithUuid(camUuid, "Camera", std::nullopt).success);
	{
		const auto camEntity = manager.FindEntityByUuid(camUuid);
		REQUIRE((camEntity != entt::null));
		auto& registry = manager.GetECS().registry;
		registry.get<Transform>(camEntity).translation = { 0.0f, 0.0f, -10.0f };
		registry.emplace<CameraComponent>(camEntity);
		SceneGraph::MarkDirty(registry, camEntity);
	}
	SceneGraph::UpdateWorldTransforms(manager.GetECS().registry);

	EditorSelection selection;
	selection.SelectOnly(nearLight);

	const auto icons = BuildEditorIconPlacements(manager, selection, TestViewProj(),
		kImageMin, kImageSize);
	REQUIRE(icons.size() == 3);

	// Draw order is far first so the nearest lands on top.
	CHECK(icons[0].entity == farLight);
	CHECK(icons[1].entity == camUuid);
	CHECK(icons[2].entity == nearLight);
	CHECK(icons[0].viewDepth > icons[1].viewDepth);
	CHECK(icons[1].viewDepth > icons[2].viewDepth);

	CHECK(icons[2].selected);
	CHECK_FALSE(icons[0].selected);
	CHECK(icons[0].kind == EditorIconKind::SpotLight);
	CHECK(icons[1].kind == EditorIconKind::Camera);
	CHECK(icons[2].kind == EditorIconKind::PointLight);
}

TEST_CASE("EditorViewportIcons: lights outside the frustum produce no placement")
{
	rt2::core::DeterministicUuidProvider ids;
	SceneManager manager;
	manager.SetUuidProvider(&ids);

	// Behind the camera, and far off to one side but in front.
	manager.AddLight("Behind", { 0.0f, 0.0f, 10.0f }, { 1, 1, 1 }, 50.0f, LightType::Point);
	manager.AddLight("OffLeft", { -500.0f, 0.0f, -4.0f }, { 1, 1, 1 }, 50.0f, LightType::Point);
	SceneGraph::UpdateWorldTransforms(manager.GetECS().registry);

	EditorSelection selection;
	const auto icons = BuildEditorIconPlacements(manager, selection, TestViewProj(),
		kImageMin, kImageSize);
	CHECK(icons.empty());
}

TEST_CASE("EditorViewportIcons: a spot light's screen aim follows its rotation")
{
	rt2::core::DeterministicUuidProvider ids;
	SceneManager manager;
	manager.SetUuidProvider(&ids);

	const auto uuid = manager.GetEntityUuid(
		manager.AddLight("Spot", { 0.0f, 0.0f, -10.0f }, { 1, 1, 1 }, 50.0f, LightType::Spot));

	// Aim straight down. Screen +y is down in ImGui coordinates, so the aim
	// vector must have a positive y — getting this backwards would draw every
	// spot cone pointing the opposite way to where it actually shines.
	const auto entity = manager.FindEntityByUuid(uuid);
	REQUIRE((entity != entt::null));
	auto& transform = manager.GetECS().registry.get<Transform>(entity);
	transform.rotation = LightDirectionToRotation({ 0.0f, -1.0f, 0.0f });
	SceneGraph::MarkDirty(manager.GetECS().registry, entity);
	SceneGraph::UpdateWorldTransforms(manager.GetECS().registry);

	EditorSelection selection;
	const auto icons = BuildEditorIconPlacements(manager, selection, TestViewProj(),
		kImageMin, kImageSize);
	REQUIRE(icons.size() == 1);
	CHECK(icons[0].screenAim.y > 0.0f);
	CHECK(std::abs(icons[0].screenAim.x) < 1e-3f);
}
