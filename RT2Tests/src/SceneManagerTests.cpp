#include <doctest/doctest.h>

#include "SceneManager.h"
#include "MeshRegistry.h"
#include "PrimitiveGeometry.h"
#include "SceneGraph.h"
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

TEST_CASE("SceneManager: compaction tolerates a MeshRef with no backing mesh")
{
	// Regression. AddObject used to emplace MeshRef{meshIndex = 0}
	// unconditionally, so on an empty mesh registry every entity carried a
	// reference to a mesh that was never added. CompactMeshRegistry then fed
	// that index to MeshRegistry::GetMesh, an unchecked m_Meshes[index],
	// reading out of bounds: SIGSEGV in Release and a 0xC0000409 fastfail in
	// Debug. The Release crash aborted the remainder of the suite, hiding 48
	// other cases, so this is pinned directly rather than relied upon
	// incidentally.
	SceneManager mgr;
	auto a = mgr.AddObject("A");
	mgr.AddObject("B");
	REQUIRE(mgr.GetEntityCount() == 2);

	// AddObject deliberately still emplaces MeshRef{0}; with an empty
	// registry that is a reference to a mesh which does not exist.
	REQUIRE(mgr.GetECS().meshRegistry.GetCount() == 0);
	REQUIRE(mgr.HasMeshRef(a));

	// Must not crash, and must leave the scene intact.
	mgr.CompactMeshRegistry();
	CHECK(mgr.GetEntityCount() == 2);
	CHECK(mgr.GetECS().meshRegistry.GetCount() == 0);

	// An arbitrarily out-of-range index is skipped, not followed.
	auto& reg = mgr.GetECS().registry;
	reg.get<MeshRef>(a.id).meshIndex = 9999u;
	mgr.CompactMeshRegistry();
	CHECK(mgr.GetEntityCount() == 2);
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

TEST_CASE("SceneManager: SetTransform updates TRS and the world matrix")
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

	// `dirty` is NOT observable here. SetTransform raises it via
	// SetLocalDirty and then immediately consumes it: the same function
	// calls RefreshCameraForwardDirections, which runs
	// SceneGraph::UpdateWorldTransforms, which clears the flag once the
	// world matrix is recomputed. Asserting dirty == true here was testing
	// an intermediate state that no longer survives the call.
	//
	// The real, durable contract is that the world matrix reflects the new
	// TRS by the time SetTransform returns.
	CHECK_FALSE(tf.dirty);
	const glm::vec3 world = SceneGraph::GetWorldPosition(reg, e.id);
	CHECK(world.x == doctest::Approx(5.0f));
	CHECK(world.y == doctest::Approx(5.0f));
	CHECK(world.z == doctest::Approx(5.0f));
}

TEST_CASE("SceneManager: SetMaterial updates MeshRef")
{
	SceneManager mgr;
	// SetMaterialIndexState bounds-checks against the material list, which a
	// bare SceneManager starts empty. Without these the call is a legitimate
	// out-of-range rejection, not a bug — which is exactly how this test
	// used to fail.
	mgr.AddMaterial(SceneMaterial{});
	mgr.AddMaterial(SceneMaterial{});
	mgr.AddMaterial(SceneMaterial{});

	auto e = mgr.AddObject("Box", {}, {}, 1.0f, 0);

	mgr.SetMaterial(e, 2);

	auto& reg = mgr.GetECS().registry;
	auto& ref = reg.get<MeshRef>(e.id);
	CHECK(ref.materialIndex == 2);
}

TEST_CASE("SceneManager: SetMaterial rejects an out-of-range index")
{
	SceneManager mgr;
	mgr.AddMaterial(SceneMaterial{});
	auto e = mgr.AddObject("Box", {}, {}, 1.0f, 0);

	// Only index 0 is valid. The rejection must leave the existing index
	// intact rather than writing a dangling material reference.
	mgr.SetMaterial(e, 5);

	auto& ref = mgr.GetECS().registry.get<MeshRef>(e.id);
	CHECK(ref.materialIndex == 0);
}

TEST_CASE("SceneManager: Emissive sphere added to scene only creates lights for sphere")
{
	SceneManager mgr;
	std::string scenePath = "C:\\Users\\mikey\\Downloads\\sofa_and_lamp.glb";
	if (!mgr.LoadScene(scenePath))
		return;

	// Sync to GPU first to get the baseline light count from the scene
	mgr.SyncToGPU();
	int initialLights = (int)mgr.GetCurrentGpuScene().lights.size();
	printf("[Test] Initial lights: %d (from lamp shade)\n", initialLights);
	CHECK(initialLights > 0);

	// Add a sphere with its own material (index 4)
	int matIdx = mgr.AddMaterial(SceneMaterial{});
	printf("[Test] New material index: %d\n", matIdx);
	CHECK(matIdx == 4);

	MeshData sphereData = PrimitiveGeometry::CreateSphere(0.2f);
	auto e = mgr.AddObjectWithGeometry("Sphere", std::move(sphereData),
	                                   {0, 0.5f, 0}, {0, 0, 0}, 1.0f, matIdx);

	// Make it emissive
	auto& mat = mgr.GetMaterial(matIdx);
	mat.emissiveColor = {1, 1, 1};
	mat.emissiveIntensity = 10.0f;

	// Sync to GPU (no callback — just build the data)
	mgr.SyncToGPUKeepTextures();

	int newLights = (int)mgr.GetCurrentGpuScene().lights.size();
	int sphereTris = 24 * 16 * 2; // segments=24, rings=16, 2 tris per quad
	// UV sphere has degenerate triangles at the poles (all pole verts collapse
	// to a single point). Top ring = 24 degenerate tris, bottom ring = 24, but
	// only the triangles where ALL 3 verts are at the pole are degenerate.
	// The first ring's triangles share 2 verts with the pole (v0==v2), so
	// they're degenerate. There are 24 per pole = 48 total, but only 36 are
	// actually skipped (some near-pole tris have tiny but non-zero area).
	// Just check that we got fewer than the full count and more than baseline.
	printf("[Test] After emissive: %d lights (expected %d + %d = %d, degenerate skipped=%d)\n",
	       newLights, initialLights, sphereTris, initialLights + sphereTris,
	       initialLights + sphereTris - newLights);

	CHECK(newLights < initialLights + sphereTris);  // some degenerate tris skipped
	CHECK(newLights > initialLights);               // but most are kept

	// Verify the sphere's instance has the right world matrix
	auto& instances = mgr.GetCurrentGpuScene().instances;
	bool foundSphere = false;
	for (int i = 0; i < (int)instances.size(); i++)
	{
		if (instances[i].materialIndex == (uint32_t)matIdx)
		{
			glm::vec3 pos = glm::vec3(instances[i].worldMatrix[3]);
			printf("[Test] Sphere instance at (%.1f, %.1f, %.1f)\n", pos.x, pos.y, pos.z);
			CHECK(pos.y == doctest::Approx(0.5f).epsilon(0.001));
			foundSphere = true;
		}
	}
	CHECK(foundSphere);
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

TEST_CASE("SceneManager: Delete all mesh entities + compact produces empty GPU scene")
{
	SceneManager mgr;
	std::string scenePath = "C:\\Users\\mikey\\Downloads\\sofa_and_lamp.glb";

	if (!mgr.LoadScene(scenePath))
	{
		// Skip if file not available
		return;
	}

	// Should have loaded entities
	CHECK(mgr.GetEntityCount() > 0);
	printf("[Test] After load: %zu entities, %d meshes\n",
	       mgr.GetEntityCount(), (int)mgr.GetECS().meshRegistry.GetCount());

	// Delete all root entities (this recursively deletes children)
	auto roots = mgr.GetRootEntities();
	printf("[Test] Deleting %zu root entities\n", roots.size());
	for (auto root : roots)
		mgr.RemoveEntity(root);

	printf("[Test] After delete: %zu entities, %d meshes\n",
	       mgr.GetEntityCount(), (int)mgr.GetECS().meshRegistry.GetCount());

	// Compact the mesh registry
	mgr.CompactMeshRegistry();

	printf("[Test] After compact: %zu entities, %d meshes\n",
	       mgr.GetEntityCount(), (int)mgr.GetECS().meshRegistry.GetCount());

	// Sync to GPU (no callback — just build the data)
	mgr.SyncToGPUKeepTextures();

	printf("[Test] After sync: meshes=%d instances=%d materials=%d\n",
	       (int)mgr.GetCurrentGpuScene().meshes.size(),
	       (int)mgr.GetCurrentGpuScene().instances.size(),
	       (int)mgr.GetCurrentGpuScene().materials.size());

	// The GPU scene should have zero meshes and zero instances
	CHECK(mgr.GetCurrentGpuScene().meshes.empty());
	CHECK(mgr.GetCurrentGpuScene().instances.empty());
}
