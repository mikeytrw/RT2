#include <doctest/doctest.h>

#include "EditorSelection.h"
#include "ViewportCoordinates.h"
#include <type_traits>
#include "SceneDocument.h"
#include "ECSComponents.h"
#include "SceneManager.h"
#include "SceneGraph.h"
#include "PrimitiveGeometry.h"
#include "TransformEditing.h"

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
		*OutputExtent::TryCreate(1600u, 900u),
		*RenderExtent::TryCreate(1600u, 900u)
	};

	CHECK(ScreenToRenderPixel({100.0f, 50.0f}, viewport).value() == glm::uvec2(0u, 0u));
	CHECK(ScreenToRenderPixel({500.0f, 275.0f}, viewport).value() == glm::uvec2(800u, 450u));
	CHECK(ScreenToRenderPixel({899.9f, 499.9f}, viewport).value() == glm::uvec2(1599u, 899u));
	CHECK_FALSE(ScreenToRenderPixel({900.0f, 500.0f}, viewport).has_value());
	CHECK_FALSE(ScreenToRenderPixel({99.9f, 50.0f}, viewport).has_value());
}

TEST_CASE("Typed extents reject zero dimensions and native planning keeps equality")
{
	static_assert(!std::is_convertible_v<RenderExtent, OutputExtent>);
	static_assert(!std::is_convertible_v<OutputExtent, RenderExtent>);
	CHECK_FALSE(RenderExtent::TryCreate(0u, 1u).has_value());
	CHECK_FALSE(OutputExtent::TryCreate(1u, 0u).has_value());
	const auto plan = PlanNativeExtents(1920u, 1080u);
	REQUIRE(plan.has_value());
	CHECK(plan->output.Width() == 1920u);
	CHECK(plan->output.Height() == 1080u);
	CHECK(plan->render == plan->output.ToRenderNative());
}

TEST_CASE("Picking maps output coordinates to render pixels explicitly")
{
	const ViewportImageRect viewport{
		glm::vec2(0.0f), glm::vec2(1000.0f, 500.0f),
		*OutputExtent::TryCreate(1000u, 500u),
		*RenderExtent::TryCreate(500u, 250u)
	};
	CHECK(ScreenToOutputPixel({999.0f, 499.0f}, viewport).value() == glm::uvec2(999u, 499u));
	CHECK(ScreenToRenderPixel({999.0f, 499.0f}, viewport).value() == glm::uvec2(499u, 249u));
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

TEST_CASE("Phase 2B affine TRS decomposition round trips non-uniform and reflected scale")
{
	EditableTRS source;
	source.translation = { -3.0f, 2.0f, 8.0f };
	source.rotation = glm::quat(glm::radians(glm::vec3(20.0f, -35.0f, 70.0f)));
	source.scale = { 2.0f, -3.0f, 0.5f };
	EditableTRS decomposed;
	REQUIRE(TryDecomposeEditableTRS(source.Matrix(), decomposed));
	for (int column = 0; column < 4; ++column)
		for (int row = 0; row < 4; ++row)
			CHECK(decomposed.Matrix()[column][row] ==
			      doctest::Approx(source.Matrix()[column][row]).epsilon(0.0001));
}

TEST_CASE("Phase 2B affine TRS rejects singular and sheared matrices")
{
	EditableTRS output;
	const glm::mat4 singular = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 1.0f));
	CHECK_FALSE(TryDecomposeEditableTRS(singular, output));

	glm::mat4 sheared(1.0f);
	sheared[1][0] = 0.25f;
	CHECK_FALSE(TryDecomposeEditableTRS(sheared, output));
}

TEST_CASE("Phase 2B world to local conversion reconstructs desired world")
{
	EditableTRS parent;
	parent.translation = { 4.0f, -1.0f, 2.0f };
	parent.rotation = glm::quat(glm::radians(glm::vec3(0.0f, 30.0f, 0.0f)));
	parent.scale = glm::vec3(2.0f);
	EditableTRS desiredLocal;
	desiredLocal.translation = { 1.0f, 2.0f, -3.0f };
	desiredLocal.rotation = glm::quat(glm::radians(glm::vec3(15.0f, 0.0f, 5.0f)));
	desiredLocal.scale = { 0.5f, 1.5f, 2.0f };
	const glm::mat4 desiredWorld = parent.Matrix() * desiredLocal.Matrix();

	EditableTRS converted;
	REQUIRE(TryWorldToLocalTRS(parent.Matrix(), desiredWorld, converted));
	const glm::mat4 reconstructed = parent.Matrix() * converted.Matrix();
	for (int column = 0; column < 4; ++column)
		for (int row = 0; row < 4; ++row)
			CHECK(reconstructed[column][row] ==
			      doctest::Approx(desiredWorld[column][row]).epsilon(0.0001));

	parent.scale.y = 0.0f;
	CHECK_FALSE(TryWorldToLocalTRS(parent.Matrix(), desiredWorld, converted));
}

TEST_CASE("Phase 2B snapping is symmetric for negative values and supports non-uniform scale")
{
	CHECK(SnapValue(0.74f, 0.5f) == doctest::Approx(0.5f));
	CHECK(SnapValue(0.76f, 0.5f) == doctest::Approx(1.0f));
	CHECK(SnapValue(-0.74f, 0.5f) == doctest::Approx(-0.5f));
	CHECK(SnapValue(-0.76f, 0.5f) == doctest::Approx(-1.0f));
	CHECK(SnapValue(1.23f, 0.0f) == doctest::Approx(1.23f));
	CHECK(SnapValues({ -1.24f, 2.26f, 0.04f }, 0.1f) ==
	      glm::vec3(-1.2f, 2.3f, 0.0f));
}

TEST_CASE("Phase 2B gizmo distinguishes a selection click from a drag")
{
	const glm::vec2 start(100.0f, 50.0f);
	CHECK_FALSE(ExceedsTransformDragThreshold(start, start));
	CHECK_FALSE(ExceedsTransformDragThreshold(start, start + glm::vec2(3.9f, 0.0f)));
	CHECK(ExceedsTransformDragThreshold(start, start + glm::vec2(4.0f, 0.0f)));
	CHECK(ExceedsTransformDragThreshold(start, start + glm::vec2(3.0f, 3.0f)));
}

TEST_CASE("Phase 2B shared pivot applies D times each starting world matrix")
{
	const glm::mat4 startPivot = glm::translate(glm::mat4(1.0f), { 1.0f, 0.0f, 0.0f });
	const glm::mat4 currentPivot = glm::translate(glm::mat4(1.0f), { 4.0f, 2.0f, 0.0f });
	const std::vector<glm::mat4> starts = {
		glm::translate(glm::mat4(1.0f), { 1.0f, 0.0f, 0.0f }),
		glm::translate(glm::mat4(1.0f), { 3.0f, 0.0f, 0.0f })
	};
	std::vector<glm::mat4> results;
	REQUIRE(TryApplySharedPivotDelta(startPivot, currentPivot, starts, results));
	REQUIRE(results.size() == 2);
	CHECK(glm::vec3(results[0][3]) == glm::vec3(4.0f, 2.0f, 0.0f));
	CHECK(glm::vec3(results[1][3]) == glm::vec3(6.0f, 2.0f, 0.0f));
	CHECK(ComputeTransformPivot(starts, TransformPivot::Median, 0) ==
	      glm::vec3(2.0f, 0.0f, 0.0f));
}

TEST_CASE("Phase 2B SceneManager preserves non-uniform local transforms")
{
	SceneManager manager;
	const auto entity = manager.AddObject("Editable");
	const glm::vec3 position(-2.0f, 4.0f, 1.0f);
	const glm::vec3 rotation(15.0f, -30.0f, 75.0f);
	const glm::vec3 scale(0.5f, 2.0f, 3.5f);

	manager.SetTransform(entity, position, rotation, scale);
	glm::vec3 actualPosition, actualRotation, actualScale;
	REQUIRE(manager.GetTransform(entity, actualPosition, actualRotation, actualScale));
	CHECK(actualPosition == position);
	CHECK(actualScale == scale);
	CHECK(actualRotation.x == doctest::Approx(rotation.x).epsilon(0.0001));
	CHECK(actualRotation.y == doctest::Approx(rotation.y).epsilon(0.0001));
	CHECK(actualRotation.z == doctest::Approx(rotation.z).epsilon(0.0001));
}

TEST_CASE("Phase 2B SceneManager converts world edits through a parent")
{
	SceneManager manager;
	const auto parent = manager.AddObject("Parent", { 5.0f, 1.0f, -2.0f },
		{ 0.0f, 25.0f, 0.0f }, 2.0f);
	const auto child = manager.AddObject("Child");
	auto& registry = manager.GetECS().registry;
	registry.emplace<Hierarchy>(parent.id).children.push_back(child.id);
	registry.emplace<Hierarchy>(child.id).parent = parent.id;
	SceneGraph::MarkDirty(registry, child.id);

	EditableTRS desired;
	desired.translation = { -4.0f, 3.0f, 7.0f };
	desired.rotation = glm::quat(glm::radians(glm::vec3(10.0f, 40.0f, -15.0f)));
	desired.scale = { 1.25f, 0.75f, 2.0f };
	REQUIRE(manager.TrySetWorldTransform(child, desired.Matrix()));

	EditableTRS actual;
	REQUIRE(manager.GetWorldTransform(child, actual));
	for (int column = 0; column < 4; ++column)
		for (int row = 0; row < 4; ++row)
			CHECK(actual.Matrix()[column][row] ==
			      doctest::Approx(desired.Matrix()[column][row]).epsilon(0.0001));
}

TEST_CASE("Phase 2B SceneManager rejects world edits below a singular parent")
{
	SceneManager manager;
	const auto parent = manager.AddObject("Parent");
	const auto child = manager.AddObject("Child", { 1.0f, 2.0f, 3.0f });
	auto& registry = manager.GetECS().registry;
	registry.emplace<Hierarchy>(parent.id).children.push_back(child.id);
	registry.emplace<Hierarchy>(child.id).parent = parent.id;
	manager.SetTransform(parent, {}, {}, glm::vec3(1.0f, 0.0f, 1.0f));

	EditableTRS before;
	REQUIRE(manager.GetLocalTransform(child, before));
	const glm::mat4 desired = glm::translate(glm::mat4(1.0f), glm::vec3(8.0f));
	CHECK_FALSE(manager.TrySetWorldTransform(child, desired));
	EditableTRS after;
	REQUIRE(manager.GetLocalTransform(child, after));
	CHECK(after.translation == before.translation);
	CHECK(after.rotation == before.rotation);
	CHECK(after.scale == before.scale);
}

TEST_CASE("Phase 2B SceneManager applies parent and child world edits atomically")
{
	SceneManager manager;
	const auto parent = manager.AddObject("Parent", { 1.0f, 0.0f, 0.0f });
	const auto child = manager.AddObject("Child", { 2.0f, 0.0f, 0.0f });
	auto& registry = manager.GetECS().registry;
	registry.emplace<Hierarchy>(parent.id).children.push_back(child.id);
	registry.emplace<Hierarchy>(child.id).parent = parent.id;

	const glm::mat4 parentWorld = glm::translate(glm::mat4(1.0f), { 4.0f, 3.0f, 0.0f });
	const glm::mat4 childWorld = glm::translate(glm::mat4(1.0f), { 6.0f, 3.0f, 0.0f });
	REQUIRE(manager.TrySetWorldTransforms({
		{ parent, parentWorld }, { child, childWorld }
	}));
	EditableTRS actualParent, actualChild;
	REQUIRE(manager.GetWorldTransform(parent, actualParent));
	REQUIRE(manager.GetWorldTransform(child, actualChild));
	CHECK(actualParent.translation == glm::vec3(4.0f, 3.0f, 0.0f));
	CHECK(actualChild.translation == glm::vec3(6.0f, 3.0f, 0.0f));

	EditableTRS beforeParent, beforeChild;
	REQUIRE(manager.GetLocalTransform(parent, beforeParent));
	REQUIRE(manager.GetLocalTransform(child, beforeChild));
	glm::mat4 invalidChild = childWorld;
	invalidChild[1][0] = 0.5f;
	CHECK_FALSE(manager.TrySetWorldTransforms({
		{ parent, glm::translate(parentWorld, glm::vec3(10.0f, 0.0f, 0.0f)) },
		{ child, invalidChild }
	}));
	EditableTRS afterParent, afterChild;
	REQUIRE(manager.GetLocalTransform(parent, afterParent));
	REQUIRE(manager.GetLocalTransform(child, afterChild));
	CHECK(afterParent.translation == beforeParent.translation);
	CHECK(afterChild.translation == beforeChild.translation);
}

TEST_CASE("Phase 2B batch world edits account for unselected intermediate parents")
{
	SceneManager manager;
	const auto root = manager.AddObject("Root", { 1.0f, 0.0f, 0.0f });
	const auto middle = manager.AddObject("Middle", { 2.0f, 0.0f, 0.0f });
	const auto leaf = manager.AddObject("Leaf", { 3.0f, 0.0f, 0.0f });
	auto& registry = manager.GetECS().registry;
	registry.emplace<Hierarchy>(root.id).children.push_back(middle.id);
	registry.emplace<Hierarchy>(middle.id).parent = root.id;
	registry.get<Hierarchy>(middle.id).children.push_back(leaf.id);
	registry.emplace<Hierarchy>(leaf.id).parent = middle.id;

	EditableTRS rootStart, leafStart;
	REQUIRE(manager.GetWorldTransform(root, rootStart));
	REQUIRE(manager.GetWorldTransform(leaf, leafStart));
	const glm::mat4 delta = glm::translate(glm::mat4(1.0f), { 0.0f, 5.0f, 0.0f });
	REQUIRE(manager.TrySetWorldTransforms({
		{ root, delta * rootStart.Matrix() },
		{ leaf, delta * leafStart.Matrix() }
	}));

	EditableTRS rootAfter, middleAfter, leafAfter;
	REQUIRE(manager.GetWorldTransform(root, rootAfter));
	REQUIRE(manager.GetWorldTransform(middle, middleAfter));
	REQUIRE(manager.GetWorldTransform(leaf, leafAfter));
	CHECK(rootAfter.translation == glm::vec3(1.0f, 5.0f, 0.0f));
	CHECK(middleAfter.translation == glm::vec3(3.0f, 5.0f, 0.0f));
	CHECK(leafAfter.translation == glm::vec3(6.0f, 5.0f, 0.0f));
}
