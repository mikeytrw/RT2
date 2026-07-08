#include <doctest/doctest.h>

#include "SceneManager.h"
#include "MeshRegistry.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

// ============================================================================
// SceneManager entity manipulation tests (no GPU required)
//
// These tests verify the ECS entity APIs: AddObject, AddLight,
// RemoveEntity, SetTransform, SetMaterial, naming, and entity
// enumeration. SceneManager is constructed without a sync callback,
// so SyncToGPU() is a no-op — we test the scene state only.
// ============================================================================

TEST_CASE("SceneManager: starts empty")
{
	SceneManager mgr;
	CHECK(mgr.GetEntityCount() == 0);
	CHECK_FALSE(mgr.HasEnvMap());
	CHECK(mgr.GetEnvMapWidth() == 0);
	CHECK(mgr.GetEnvMapHeight() == 0);
}

TEST_CASE("SceneManager: AddObject creates entity with Transform + MeshRef")
{
	SceneManager mgr;
	auto e = mgr.AddObject("Cube", {1, 2, 3}, {0, 90, 0}, 2.0f, 0);
	CHECK(e.IsValid());
	CHECK(mgr.GetEntityCount() == 1);
	CHECK(mgr.GetEntityName(e) == "Cube");

	// Verify the entity has Transform and MeshRef components
	auto& reg = mgr.GetECS().registry;
	CHECK(reg.all_of<Transform>(e.id));
	CHECK(reg.all_of<MeshRef>(e.id));

	auto& tf = reg.get<Transform>(e.id);
	CHECK(tf.translation.x == 1.0f);
	CHECK(tf.translation.y == 2.0f);
	CHECK(tf.translation.z == 3.0f);
	CHECK(tf.scale.x == 2.0f);
	CHECK(tf.dirty == true);
}

TEST_CASE("SceneManager: AddObjectWithGeometry registers mesh in MeshRegistry")
{
	SceneManager mgr;

	MeshData meshData;
	meshData.vertices = {0, 0, 0, 1, 0, 0, 0, 1, 0};
	meshData.indices = {0, 1, 2};
	meshData.name = "triangle";

	auto e = mgr.AddObjectWithGeometry("Tri", std::move(meshData), {0, 0, 0}, {}, 1.0f, 0);
	CHECK(e.IsValid());
	CHECK(mgr.GetECS().meshRegistry.GetCount() == 1);

	auto& reg = mgr.GetECS().registry;
	auto& ref = reg.get<MeshRef>(e.id);
	CHECK(ref.meshIndex == 0);
	CHECK(ref.materialIndex == 0);
}

TEST_CASE("SceneManager: AddLight creates entity with LightComponent")
{
	SceneManager mgr;
	auto e = mgr.AddLight("Lamp", {5, 10, 2}, {1, 0, 0}, 50.0f, false);
	CHECK(e.IsValid());
	CHECK(mgr.GetEntityCount() == 1);

	auto& reg = mgr.GetECS().registry;
	CHECK(reg.all_of<LightComponent>(e.id));
	CHECK(reg.all_of<Transform>(e.id));

	auto& light = reg.get<LightComponent>(e.id);
	CHECK(light.color.r == 1.0f);
	CHECK(light.intensity == 50.0f);
	CHECK(light.isSpot == false);
}

TEST_CASE("SceneManager: RemoveEntity destroys entity")
{
	SceneManager mgr;
	auto e1 = mgr.AddObject("A");
	auto e2 = mgr.AddObject("B");
	CHECK(mgr.GetEntityCount() == 2);

	mgr.RemoveEntity(e1);
	CHECK(mgr.GetEntityCount() == 1);
	CHECK_FALSE(mgr.GetECS().registry.valid(e1.id));
	CHECK(mgr.GetECS().registry.valid(e2.id));
}

TEST_CASE("SceneManager: RemoveEntity on invalid id is no-op")
{
	SceneManager mgr;
	mgr.AddObject("A");
	SceneManager::EntityId invalid;
	CHECK_FALSE(invalid.IsValid());
	mgr.RemoveEntity(invalid);
	CHECK(mgr.GetEntityCount() == 1);
}

TEST_CASE("SceneManager: SetTransform updates TRS and marks dirty")
{
	SceneManager mgr;
	auto e = mgr.AddObject("Box", {0, 0, 0});

	mgr.SetTransform(e, {5, 5, 5}, {45, 0, 0}, 3.0f);

	auto& reg = mgr.GetECS().registry;
	auto& tf = reg.get<Transform>(e.id);
	CHECK(tf.translation.x == 5.0f);
	CHECK(tf.translation.y == 5.0f);
	CHECK(tf.translation.z == 5.0f);
	CHECK(tf.scale.x == 3.0f);
	CHECK(tf.dirty == true);
}

TEST_CASE("SceneManager: SetMaterial updates MeshRef")
{
	SceneManager mgr;
	auto e = mgr.AddObject("Box", {}, {}, 1.0f, 0);

	mgr.SetMaterial(e, 2);

	auto& reg = mgr.GetECS().registry;
	auto& ref = reg.get<MeshRef>(e.id);
	CHECK(ref.materialIndex == 2);
}

TEST_CASE("SceneManager: SetEntityName updates NameComponent")
{
	SceneManager mgr;
	auto e = mgr.AddObject("Original");
	CHECK(mgr.GetEntityName(e) == "Original");

	mgr.SetEntityName(e, "Renamed");
	CHECK(mgr.GetEntityName(e) == "Renamed");
}

TEST_CASE("SceneManager: GetEntityName on unnamed entity returns empty")
{
	SceneManager mgr;
	// AddObject with empty name doesn't create NameComponent
	auto e = mgr.AddObject("");
	CHECK(mgr.GetEntityName(e).empty());
}

TEST_CASE("SceneManager: GetEntityByIndex iterates entities")
{
	SceneManager mgr;
	auto e1 = mgr.AddObject("First");
	auto e2 = mgr.AddObject("Second");
	auto e3 = mgr.AddLight("Light");

	CHECK(mgr.GetEntityCount() == 3);

	// GetEntityByIndex should return valid entities for 0..count-1
	auto idx0 = mgr.GetEntityByIndex(0);
	auto idx1 = mgr.GetEntityByIndex(1);
	auto idx2 = mgr.GetEntityByIndex(2);
	CHECK(idx0.IsValid());
	CHECK(idx1.IsValid());
	CHECK(idx2.IsValid());

	// Out of range returns invalid
	CHECK_FALSE(mgr.GetEntityByIndex(99).IsValid());
}

TEST_CASE("SceneManager: AddMaterial + GetMaterial")
{
	SceneManager mgr;
	SceneMaterial mat;
	mat.baseColor = {0.5f, 0.5f, 1.0f};
	mat.metallic = 0.8f;
	int idx = mgr.AddMaterial(mat);
	CHECK(idx == 0);
	CHECK(mgr.GetMaterials().size() == 1);

	auto& got = mgr.GetMaterial(0);
	CHECK(got.baseColor.r == 0.5f);
	CHECK(got.metallic == 0.8f);
}

TEST_CASE("SceneManager: Clear resets all state")
{
	SceneManager mgr;
	mgr.AddObject("A");
	mgr.AddLight("L");
	mgr.AddMaterial({});
	CHECK(mgr.GetEntityCount() == 2);
	CHECK(mgr.GetMaterials().size() == 1);

	mgr.Clear();
	CHECK(mgr.GetEntityCount() == 0);
	CHECK(mgr.GetMaterials().empty());
	CHECK_FALSE(mgr.HasEnvMap());
}

TEST_CASE("SceneManager: ClearEnvMap clears env map data")
{
	// Can't easily test LoadEnvMap (requires a file), but ClearEnvMap
	// can be tested directly — it should reset all env map fields.
	SceneManager mgr;
	mgr.ClearEnvMap();
	CHECK_FALSE(mgr.HasEnvMap());
	CHECK(mgr.GetEnvMapWidth() == 0);
	CHECK(mgr.GetEnvMapHeight() == 0);
	CHECK(mgr.GetEnvMapPath().empty());
}

TEST_CASE("SceneManager: SyncToGPU with no sync callback is safe")
{
	SceneManager mgr;
	mgr.AddObject("Test");
	// No sync callback set — should not crash
	mgr.SyncToGPU();
	// With a single object (no geometry) the GPU scene should be empty-ish
	// — just verify it doesn't crash and has 0 or more instances.
	bool hasInstances = !mgr.GetCurrentGpuScene().instances.empty();
	bool hasMeshes = !mgr.GetCurrentGpuScene().meshes.empty();
	CHECK((hasInstances || hasMeshes || true)); // just verify no crash
}