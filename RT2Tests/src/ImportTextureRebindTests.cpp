#include <doctest/doctest.h>

#include "ECSComponents.h"
#include "SceneManager.h"

#include <filesystem>
#include <fstream>
#include <string>

// ============================================================================
// Reported repro: import model A, delete its entity, import model B, and B
// renders with A's textures.
//
// Everything here stays on the ECS side -- import, delete, compact, import --
// so if the texture indices come out wrong the bug is in the scene's own
// bookkeeping. If they come out right, the mis-binding is downstream in the
// GPU texture array, and this file has ruled out the half that is cheap to
// test.
// ============================================================================

namespace {

// Minimal uncompressed 24-bit TGA. stb_image reads these, and the test project
// links the reader but no writer, so this is the cheapest real image file to
// produce.
void WriteTga(const std::filesystem::path& path,
              unsigned char r, unsigned char g, unsigned char b)
{
	std::ofstream file(path, std::ios::binary);
	unsigned char header[18] = {};
	header[2] = 2;    // uncompressed true-colour
	header[12] = 2;   // width  = 2
	header[14] = 2;   // height = 2
	header[16] = 24;  // bits per pixel
	file.write(reinterpret_cast<const char*>(header), sizeof(header));
	for (int i = 0; i < 4; ++i)
	{
		file.put(static_cast<char>(b));
		file.put(static_cast<char>(g));
		file.put(static_cast<char>(r));
	}
}

// One triangle, one material, one base-colour texture.
std::filesystem::path MakeTexturedObj(const std::filesystem::path& dir,
                                      const std::string& name,
                                      unsigned char r, unsigned char g, unsigned char b)
{
	std::filesystem::create_directories(dir);
	const std::string texName = name + "_tex.tga";
	WriteTga(dir / texName, r, g, b);

	{
		std::ofstream mtl(dir / (name + ".mtl"));
		mtl << "newmtl " << name << "_mat\n"
		    << "Kd 1 1 1\n"
		    << "map_Kd " << texName << "\n";
	}

	const auto objPath = dir / (name + ".obj");
	{
		std::ofstream obj(objPath);
		obj << "mtllib " << name << ".mtl\n"
		    << "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
		    << "vt 0 0\nvt 1 0\nvt 0 1\n"
		    << "usemtl " << name << "_mat\n"
		    << "f 1/1 2/2 3/3\n";
	}
	return objPath;
}

// Addressing a material through an entity differs between merged and
// per-shape imports, so assert on the scene's tables instead: those are what
// the GPU scene is built from, and where a stale index would show.
std::string DescribeTables(SceneManager& mgr)
{
	const auto& ecs = mgr.GetECS();
	std::string out = "materials=" + std::to_string(ecs.materials.size()) +
	                  " textures=" + std::to_string(ecs.textures.size());
	for (size_t i = 0; i < ecs.materials.size(); ++i)
		out += " | mat[" + std::to_string(i) + "].baseColorTex=" +
		       std::to_string(ecs.materials[i].baseColorTextureIndex);
	for (size_t i = 0; i < ecs.textures.size(); ++i)
		out += " | tex[" + std::to_string(i) + "]=" + ecs.textures[i].ref.path;
	return out;
}

} // namespace

TEST_CASE("Import: a model imported after a delete keeps its own textures")
{
	const auto dir = std::filesystem::temp_directory_path() / "rt2_import_texture_rebind";
	std::filesystem::remove_all(dir);

	const auto modelA = MakeTexturedObj(dir, "modelA", 255, 0, 0);
	const auto modelB = MakeTexturedObj(dir, "modelB", 0, 0, 255);

	SceneManager mgr;
	ImportSettings settings;
	settings.mergeMegaMesh = false;

	const auto rootA = mgr.ImportObj(modelA.string(), settings);
	REQUIRE(rootA.IsValid());
	MESSAGE("after import A: " << DescribeTables(mgr));

	// The app compacts on scene change whenever the undo history is empty,
	// which is the state this reproduces.
	mgr.RemoveEntity(rootA);
	mgr.CompactMeshRegistry();
	MESSAGE("after delete+compact: " << DescribeTables(mgr));

	const auto rootB = mgr.ImportObj(modelB.string(), settings);
	REQUIRE(rootB.IsValid());
	MESSAGE("after import B: " << DescribeTables(mgr));

	// Every material that names a texture must name one of B's.
	const auto& ecs = mgr.GetECS();
	int checked = 0;
	for (const auto& mat : ecs.materials)
	{
		if (mat.baseColorTextureIndex < 0) continue;
		REQUIRE(mat.baseColorTextureIndex < (int)ecs.textures.size());
		const std::string path = ecs.textures[mat.baseColorTextureIndex].ref.path;
		INFO("material resolved to: " << path);
		CHECK(path.find("modelB") != std::string::npos);
		CHECK(path.find("modelA") == std::string::npos);
		++checked;
	}
	CHECK(checked > 0);

	std::filesystem::remove_all(dir);
}

// ============================================================================
// The actual defect behind the report.
//
// A merged mega-mesh OBJ stores per-triangle material indices and sets
// MeshRef::materialIndex to -1, so those per-triangle values are the only
// thing addressing a material. They are written straight from tinyobj, which
// numbers materials from 0 within the file being read -- with no matBase
// added for materials already in the scene.
//
// So the second OBJ imported into a scene indexes the FIRST import's
// materials, and renders with its textures. The glTF path gets this right
// (SceneLoader.cpp: "Offset material index by matBase"); only OBJ does not.
//
// Deleting the first model's entity is what makes it obvious rather than
// necessary: the orphaned material stays in the table, so the survivor on
// screen is wearing the deleted model's texture.
// ============================================================================
TEST_CASE("Import: a second OBJ's triangles reference its own materials")
{
	const auto dir = std::filesystem::temp_directory_path() / "rt2_import_obj_matbase";
	std::filesystem::remove_all(dir);

	const auto modelA = MakeTexturedObj(dir, "modelA", 255, 0, 0);
	const auto modelB = MakeTexturedObj(dir, "modelB", 0, 0, 255);

	SceneManager mgr;
	ImportSettings settings;
	settings.mergeMegaMesh = true;

	REQUIRE(mgr.ImportObj(modelA.string(), settings).IsValid());
	const auto rootB = mgr.ImportObj(modelB.string(), settings);
	REQUIRE(rootB.IsValid());

	const auto& ecs = mgr.GetECS();
	auto& reg = const_cast<entt::registry&>(ecs.registry);
	MESSAGE("tables: " << DescribeTables(mgr));

	// Find B's mesh via its subtree.
	uint32_t meshIdx = UINT32_MAX;
	std::vector<entt::entity> stack{ rootB.id };
	while (!stack.empty())
	{
		const auto e = stack.back();
		stack.pop_back();
		if (!reg.valid(e)) continue;
		if (auto* mr = reg.try_get<MeshRef>(e)) { meshIdx = mr->meshIndex; break; }
		if (auto* h = reg.try_get<Hierarchy>(e))
			for (auto c : h->children) stack.push_back(c);
	}
	REQUIRE(meshIdx != UINT32_MAX);

	const auto& mesh = ecs.meshRegistry.GetMesh(meshIdx);
	REQUIRE_FALSE(mesh.materialIndices.empty());

	for (uint32_t matIdx : mesh.materialIndices)
	{
		REQUIRE(matIdx < ecs.materials.size());
		const int texIdx = ecs.materials[matIdx].baseColorTextureIndex;
		REQUIRE(texIdx >= 0);
		REQUIRE(texIdx < (int)ecs.textures.size());
		const std::string path = ecs.textures[texIdx].ref.path;
		INFO("B triangle -> material " << matIdx << " -> " << path);
		CHECK(path.find("modelB") != std::string::npos);
	}

	std::filesystem::remove_all(dir);
}
