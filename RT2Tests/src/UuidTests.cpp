#include <doctest/doctest.h>

#include "SceneManager.h"
#include "ECSComponents.h"
#include "ECSScene.h"
#include "core/UUID.h"
#include "core/Error.h"
#include "SceneDocument.h"

#include <entt/entt.hpp>
#include <unordered_set>

using namespace rt2::core;

// ============================================================================
// VS-1: EntityIdComponent assignment and UUID index tests
//
// Every entity created through SceneManager must carry an EntityIdComponent
// with a stable, unique UUID, and the SceneDocument's UUID index must be
// consistent with the registry.
// ============================================================================

TEST_CASE("VS-1 UUID: SceneManager default-constructs with a UUID provider")
{
	SceneManager mgr;
	// No crash; the document has a provider set.
	CHECK(mgr.AuthoringDoc().GetUuidProvider() != nullptr);
}

TEST_CASE("VS-1 UUID: AddObject assigns EntityIdComponent")
{
	SceneManager mgr;
	auto e = mgr.AddObject("Cube", {1, 2, 3}, {0, 0, 0}, 1.0f, 0);
	CHECK(e.IsValid());

	auto& reg = mgr.GetECS().registry;
	CHECK(reg.all_of<EntityIdComponent>(e.id));

	const auto& idc = reg.get<EntityIdComponent>(e.id);
	CHECK_FALSE(idc.id.IsNull());
	CHECK(idc.id.Version() == 4);
	CHECK(idc.id.Variant() == 2);
}

TEST_CASE("VS-1 UUID: AddObjectWithGeometry assigns EntityIdComponent")
{
	SceneManager mgr;
	MeshData mesh;
	mesh.vertices = {0, 0, 0, 1, 0, 0, 0, 1, 0};
	mesh.indices = {0, 1, 2};
	mesh.name = "tri";
	auto e = mgr.AddObjectWithGeometry("Tri", std::move(mesh), {}, {}, 1.0f, 0);

	auto& reg = mgr.GetECS().registry;
	CHECK(reg.all_of<EntityIdComponent>(e.id));
	CHECK_FALSE(reg.get<EntityIdComponent>(e.id).id.IsNull());
}

TEST_CASE("VS-1 UUID: AddLight assigns EntityIdComponent")
{
	SceneManager mgr;
	auto e = mgr.AddLight("Lamp", {5, 10, 2}, {1, 0, 0}, 50.0f, LightType::Point);

	auto& reg = mgr.GetECS().registry;
	CHECK(reg.all_of<EntityIdComponent>(e.id));
	CHECK_FALSE(reg.get<EntityIdComponent>(e.id).id.IsNull());
}

TEST_CASE("VS-1 UUID: multiple entities get unique UUIDs")
{
	SceneManager mgr;
	std::unordered_set<UUID> seen;
	for (int i = 0; i < 100; ++i)
	{
		auto e = mgr.AddObject("Obj" + std::to_string(i));
		auto& reg = mgr.GetECS().registry;
		const auto& idc = reg.get<EntityIdComponent>(e.id);
		CHECK(seen.insert(idc.id).second);
	}
	CHECK(seen.size() == 100);
}

TEST_CASE("VS-1 UUID: FindEntityByUuid returns the entity")
{
	SceneManager mgr;
	auto e = mgr.AddObject("FindMe");
	auto& reg = mgr.GetECS().registry;
	UUID uuid = reg.get<EntityIdComponent>(e.id).id;

	entt::entity found = mgr.FindEntityByUuid(uuid);
	CHECK(found == e.id);
}

TEST_CASE("VS-1 UUID: FindEntityByUuid returns null for unknown UUID")
{
	SceneManager mgr;
	UUID unknown = UUID::Parse("00000000-0000-4000-8000-000000000001");
	CHECK(mgr.FindEntityByUuid(unknown) == static_cast<entt::entity>(entt::null));
}

TEST_CASE("VS-1 UUID: RemoveEntity erases from UUID index")
{
	SceneManager mgr;
	auto e = mgr.AddObject("ToDelete");
	auto& reg = mgr.GetECS().registry;
	UUID uuid = reg.get<EntityIdComponent>(e.id).id;

	CHECK(mgr.FindEntityByUuid(uuid) == e.id);
	mgr.RemoveEntity(e);
	CHECK(mgr.FindEntityByUuid(uuid) == static_cast<entt::entity>(entt::null));
}

TEST_CASE("VS-1 UUID: RemoveEntity with children erases all from index")
{
	SceneManager mgr;
	auto parent = mgr.AddObject("Parent");
	auto& reg = mgr.GetECS().registry;

	// Create a child manually parented to 'parent'.
	auto child = reg.create();
	reg.emplace<Transform>(child);
	reg.emplace<NameComponent>(child, "Child");
	reg.emplace<VisibleComponent>(child);
	mgr.AuthoringDoc().AssignNewUuid(child);

	Hierarchy& ph = reg.emplace<Hierarchy>(parent.id);
	ph.children.push_back(child);
	Hierarchy& ch = reg.emplace<Hierarchy>(child);
	ch.parent = parent.id;

	UUID parentUuid = reg.get<EntityIdComponent>(parent.id).id;
	UUID childUuid  = reg.get<EntityIdComponent>(child).id;

	CHECK(mgr.FindEntityByUuid(parentUuid) == parent.id);
	CHECK(mgr.FindEntityByUuid(childUuid) == child);

	mgr.RemoveEntity(parent);

	CHECK(mgr.FindEntityByUuid(parentUuid) == static_cast<entt::entity>(entt::null));
	CHECK(mgr.FindEntityByUuid(childUuid) == static_cast<entt::entity>(entt::null));
	CHECK_FALSE(reg.valid(parent.id));
	CHECK_FALSE(reg.valid(child));
}

TEST_CASE("VS-1 UUID: Clear resets the UUID index")
{
	SceneManager mgr;
	mgr.AddObject("A");
	mgr.AddObject("B");
	mgr.AddLight("C");
	CHECK(mgr.AuthoringDoc().uuidIndex.Size() == 3);

	mgr.Clear();
	CHECK(mgr.AuthoringDoc().uuidIndex.Size() == 0);
	CHECK(mgr.GetEntityCount() == 0);
}

TEST_CASE("VS-1 UUID: ValidateUniqueUuids passes on clean scene")
{
	SceneManager mgr;
	mgr.AddObject("A");
	mgr.AddObject("B");
	mgr.AddLight("C");

	Error err;
	CHECK(mgr.AuthoringDoc().ValidateUniqueUuids(err));
	CHECK(err.IsOk());
}

TEST_CASE("VS-1 UUID: deterministic provider yields predictable IDs")
{
	DeterministicUuidProvider provider;
	SceneManager mgr;
	mgr.SetUuidProvider(&provider);

	auto e = mgr.AddObject("Det");
	auto& reg = mgr.GetECS().registry;
	UUID first = reg.get<EntityIdComponent>(e.id).id;

	// A second SceneManager with the same deterministic provider should
	// produce the same first UUID.
	DeterministicUuidProvider provider2;
	SceneManager mgr2;
	mgr2.SetUuidProvider(&provider2);
	auto e2 = mgr2.AddObject("Det");
	UUID second = mgr2.GetECS().registry.get<EntityIdComponent>(e2.id).id;

	CHECK(first == second);
}

TEST_CASE("VS-1 UUID: EntityIdComponent survives SetTransform")
{
	SceneManager mgr;
	auto e = mgr.AddObject("Box", {0, 0, 0});
	auto& reg = mgr.GetECS().registry;
	UUID before = reg.get<EntityIdComponent>(e.id).id;

	mgr.SetTransform(e, {5, 5, 5}, {45, 0, 0}, 3.0f);

	UUID after = reg.get<EntityIdComponent>(e.id).id;
	CHECK(before == after);
	CHECK(mgr.FindEntityByUuid(after) == e.id);
}

TEST_CASE("VS-1 UUID: hierarchy edits preserve UUID index")
{
	SceneManager mgr;
	auto parent = mgr.AddObject("Parent");
	auto child  = mgr.AddObject("Child");

	auto& reg = mgr.GetECS().registry;
	UUID parentUuid = reg.get<EntityIdComponent>(parent.id).id;
	UUID childUuid  = reg.get<EntityIdComponent>(child.id).id;

	// Manually parent child under parent.
	Hierarchy& ph = reg.emplace<Hierarchy>(parent.id);
	ph.children.push_back(child.id);
	Hierarchy& ch = reg.emplace<Hierarchy>(child.id);
	ch.parent = parent.id;

	// Index should still resolve both.
	CHECK(mgr.FindEntityByUuid(parentUuid) == parent.id);
	CHECK(mgr.FindEntityByUuid(childUuid) == child.id);
}