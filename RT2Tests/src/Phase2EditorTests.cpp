#include <doctest/doctest.h>

#include "EditorSelection.h"
#include "ViewportCoordinates.h"
#include "SceneDocument.h"
#include "ECSComponents.h"
#include "SceneManager.h"
#include "PrimitiveGeometry.h"

#include <glm/gtc/matrix_transform.hpp>

namespace {

rt2::core::UUID AddIdentifiedEntity(rt2::core::SceneDocument& document)
{
	const entt::entity entity = document.ecs.registry.create();
	return document.AssignNewUuid(entity);
}

} // namespace

TEST_CASE("Phase 2 editor selection preserves order and primary UUID")
{
	rt2::core::DeterministicUuidProvider ids;
	const auto a = ids.CreateV4();
	const auto b = ids.CreateV4();
	EditorSelection selection;

	selection.Add(a);
	selection.Add(b);
	selection.Add(a);
	CHECK(selection.Size() == 2);
	CHECK(selection.Ordered()[0] == a);
	CHECK(selection.Primary() == b);

	selection.Toggle(a);
	CHECK_FALSE(selection.Contains(a));
	CHECK(selection.Primary() == b);
	selection.SelectOnly(a);
	CHECK(selection.Size() == 1);
	CHECK(selection.Primary() == a);
}

TEST_CASE("Phase 2 editor selection prunes deleted authoring entities")
{
	rt2::core::DeterministicUuidProvider ids;
	rt2::core::SceneDocument document;
	document.SetUuidProvider(&ids);
	const auto live = AddIdentifiedEntity(document);
	const auto deleted = AddIdentifiedEntity(document);
	const entt::entity deletedEntity = document.FindByUuid(deleted);

	EditorSelection selection;
	selection.Add(live);
	selection.Add(deleted);
	document.uuidIndex.Erase(deleted);
	document.ecs.registry.destroy(deletedEntity);

	CHECK(selection.Prune(document));
	CHECK(selection.Size() == 1);
	CHECK(selection.Primary() == live);
	CHECK_FALSE(selection.Prune(document));
}

TEST_CASE("Phase 2 viewport coordinates account for displayed and render extents")
{
	const ViewportImageRect viewport{
		glm::vec2(100.0f, 50.0f), glm::vec2(800.0f, 450.0f),
		glm::uvec2(1600u, 900u)
	};

	CHECK(ScreenToRenderPixel({100.0f, 50.0f}, viewport).value() == glm::uvec2(0u, 0u));
	CHECK(ScreenToRenderPixel({500.0f, 275.0f}, viewport).value() == glm::uvec2(800u, 450u));
	CHECK(ScreenToRenderPixel({899.9f, 499.9f}, viewport).value() == glm::uvec2(1599u, 899u));
	CHECK_FALSE(ScreenToRenderPixel({900.0f, 500.0f}, viewport).has_value());
	CHECK_FALSE(ScreenToRenderPixel({99.9f, 50.0f}, viewport).has_value());
}

TEST_CASE("Phase 2 picking ray is deterministic and centered on camera forward")
{
	glm::mat4 projection = glm::perspectiveFov(
		glm::radians(60.0f), 1600.0f, 900.0f, 0.1f, 1000.0f);
	projection[1][1] *= -1.0f;
	const glm::vec3 position(3.0f, 2.0f, 5.0f);
	const glm::vec3 forward = glm::normalize(glm::vec3(0.25f, -0.1f, -1.0f));
	const glm::mat4 view = glm::lookAt(position, position + forward, glm::vec3(0, 1, 0));

	const CameraRay first = BuildPickingRay(glm::inverse(projection), glm::inverse(view),
	                                        position, glm::vec2(0.5f));
	const CameraRay second = BuildPickingRay(glm::inverse(projection), glm::inverse(view),
	                                         position, glm::vec2(0.5f));
	CHECK(first.origin == position);
	CHECK(first.direction == second.direction);
	CHECK(glm::dot(first.direction, forward) > 0.9999f);
	CHECK(doctest::Approx(glm::length(first.direction)) == 1.0f);
}

TEST_CASE("Phase 2 render instance UUID map matches build and transform update order")
{
	rt2::core::DeterministicUuidProvider ids;
	SceneManager manager;
	manager.SetUuidProvider(&ids);
	manager.AddMaterial(SceneMaterial{});
	const auto first = manager.AddObjectWithGeometry(
		"First", PrimitiveGeometry::CreateCube(1.0f),
		{ -1.0f, 0.0f, 0.0f }, {}, 1.0f, 0);
	const auto second = manager.AddObjectWithGeometry(
		"Second", PrimitiveGeometry::CreateSphere(0.5f),
		{ 1.0f, 0.0f, 0.0f }, {}, 1.0f, 0);

	RenderInstanceMap buildMap;
	GPUSceneData gpu = BuildGPUSceneDataFromECS(manager.GetECS(), &buildMap);
	REQUIRE(buildMap.size() == gpu.instances.size());
	CHECK(buildMap.size() == 2);
	RenderInstanceMap expected;
	const auto view = manager.GetECS().registry.view<MeshRef, Transform>();
	for (const entt::entity entity : view)
		expected.push_back(manager.GetECS().registry.get<EntityIdComponent>(entity).id);
	CHECK(buildMap == expected);

	manager.SetTransform(first, { -2.0f, 0.0f, 0.0f }, {}, 1.0f);
	RenderInstanceMap updateMap;
	UpdateInstancesFromECS(gpu, manager.GetECS(), &updateMap);
	CHECK(updateMap == buildMap);
}
