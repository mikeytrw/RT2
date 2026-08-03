#include <doctest/doctest.h>

#include "ECSComponents.h"
#include "SceneManager.h"
#include "SceneTypes.h"

// ============================================================================
// Phase 8 pre-work: override-aware compaction (texture pass only).
//
// CompactMeshRegistry marks live textures via m_EcsScene.materials; a
// MaterialOverrideComponent carries a full SceneMaterial snapshot of its own,
// so an unmirrored override is invisible to the sweep and its texture index
// is rebased to -1 with no diagnostic. The fix adds the override's four
// texture indices to the texture marking pass — and nothing else: the
// material-slot invalidation (SceneManager.cpp MaterialOverride, "the
// deliberate asymmetry must remain") is out of scope, and the material pass
// is untouched. These are the three discrimination proofs from the Phase 8
// pre-work spec.
//
// The fixtures build the scene directly on ECSScene tables and the registry
// (SceneManager.cpp compiles into RT2Tests), so no host or GPU is involved.
// ============================================================================

namespace
{

// Two textures, two materials, one entity. The entity's MeshRef points at
// material slot 1 (the live slot), while its MaterialOverrideComponent
// snapshot mirrors material slot 0 — which no entity references. After
// compaction slot 0 is swept and the override is unmirrored: the texture
// pass must keep the override's own texture alive.
struct UnmirroredOverrideFixture
{
	SceneManager mgr;
	entt::entity entity = entt::null;

	UnmirroredOverrideFixture()
	{
		auto& ecs = mgr.GetECS();

		SceneTexture albedo;
		albedo.ref.path = "albedo.png";
		albedo.width = 2;
		albedo.height = 2;
		SceneTexture metalness;
		metalness.ref.path = "metalness.png";
		metalness.width = 2;
		metalness.height = 2;
		ecs.textures.push_back(albedo);     // texture index 0
		ecs.textures.push_back(metalness);  // texture index 1

		SceneMaterial mirrored;  // material slot 0 — textured, referenced by nothing
		mirrored.baseColorTextureIndex = 0;
		SceneMaterial live;      // material slot 1 — the entity's actual slot
		live.baseColorTextureIndex = 1;
		ecs.materials.push_back(mirrored);
		ecs.materials.push_back(live);

		auto e = mgr.AddObject("E");
		REQUIRE(e.IsValid());
		entity = e.id;
		ecs.registry.get<MeshRef>(entity).materialIndex = 1;

		MaterialOverrideComponent ov;
		ov.material = mirrored;  // snapshot mirrors slot 0 (texture 0)
		ov.authored = true;
		ov.materialIndex = 0;    // transient slot field, mirrors slot 0
		ecs.registry.emplace<MaterialOverrideComponent>(entity, ov);
	}
};

// Three textures, one material, one entity. The entity's MeshRef points at
// the single slot and the override mirrors it exactly — the ordinary
// SetMaterial shape. The mirror's material index (0) deliberately differs
// from its texture index (2) so a faulty marking loop that leaks a
// non-texture value into the texture set is observable as a remap shift.
struct MirroredOverrideFixture
{
	SceneManager mgr;
	entt::entity entity = entt::null;

	MirroredOverrideFixture()
	{
		auto& ecs = mgr.GetECS();

		for (const char* path : {"x.png", "y.png", "z.png"})
		{
			SceneTexture tex;
			tex.ref.path = path;
			tex.width = 2;
			tex.height = 2;
			ecs.textures.push_back(tex);
		}

		SceneMaterial live;  // material slot 0
		live.baseColorTextureIndex = 2;
		ecs.materials.push_back(live);

		auto e = mgr.AddObject("E");
		REQUIRE(e.IsValid());
		entity = e.id;
		ecs.registry.get<MeshRef>(entity).materialIndex = 0;

		MaterialOverrideComponent ov;
		ov.material = live;  // mirror of slot 0
		ov.authored = true;
		ov.materialIndex = 0;
		ecs.registry.emplace<MaterialOverrideComponent>(entity, ov);
	}
};

} // namespace

TEST_CASE("Compaction: unmirrored override retains its texture")
{
	UnmirroredOverrideFixture f;
	auto& ecs = f.mgr.GetECS();

	REQUIRE(ecs.materials.size() == 2);
	f.mgr.CompactMeshRegistry();

	// The mirror slot is swept (referenced by no entity); the override's
	// texture must survive through the override's own marks.
	CHECK(ecs.materials.size() == 1);

	const auto& ov = ecs.registry.get<MaterialOverrideComponent>(f.entity);
	REQUIRE(ov.material.baseColorTextureIndex >= 0);
	REQUIRE(ov.material.baseColorTextureIndex < (int)ecs.textures.size());
	CHECK(ecs.textures[ov.material.baseColorTextureIndex].ref.path == "albedo.png");
}

TEST_CASE("Compaction: mirrored override compacts identically")
{
	MirroredOverrideFixture f;
	auto& ecs = f.mgr.GetECS();

	f.mgr.CompactMeshRegistry();

	// Ordinary SetMaterial mirror: the slot survives, so the sweep result
	// must be identical to the no-override case — one material, one texture,
	// and the override's indices remapped exactly like the slot's.
	CHECK(ecs.materials.size() == 1);
	CHECK(ecs.textures.size() == 1);
	CHECK(ecs.textures[0].ref.path == "z.png");
	CHECK(ecs.materials[0].baseColorTextureIndex == 0);

	const auto& ov = ecs.registry.get<MaterialOverrideComponent>(f.entity);
	CHECK(ov.material.baseColorTextureIndex == 0);
	CHECK(ov.materialIndex == 0);
}

TEST_CASE("Compaction: override material-slot invalidation is preserved")
{
	UnmirroredOverrideFixture f;
	auto& ecs = f.mgr.GetECS();

	f.mgr.CompactMeshRegistry();

	// The deliberate asymmetry (SceneManager.cpp MaterialOverride, "this
	// deliberate asymmetry must remain"): a swept slot maps the override's
	// transient materialIndex to -1 and the resolver refills it. The fix
	// must not mark overrides into the material pass.
	const auto& ov = ecs.registry.get<MaterialOverrideComponent>(f.entity);
	CHECK(ov.materialIndex == -1);
}
