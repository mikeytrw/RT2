#include <doctest/doctest.h>

#include "AssetIdentity.h"
#include "SceneSerializer.h"
#include "SceneSerializerTestSupport.h"
#include "SceneDocument.h"
#include "ECSComponents.h"
#include "ECSScene.h"
#include "PrimitiveGeometry.h"
#include "core/UUID.h"
#include "core/Error.h"
#include "json.hpp"

#include <filesystem>
#include <fstream>

using namespace rt2::core;

// ============================================================================
// Phase 7 W1: per-asset sidecar identity (D8) and AssetReference.assetId
// plumbing (D1/D2). The sidecar is the source of truth; the field on
// AssetReference is a cache. Resolution still goes by path in W1 — the ID
// is plumbed but not authoritative.
// ============================================================================

namespace {

std::filesystem::path UniqueTempDir(const std::string& tag)
{
    auto base = std::filesystem::temp_directory_path();
    auto dir = base / (tag + "_" + std::to_string(std::rand()));
    std::filesystem::create_directories(dir);
    return dir;
}

} // namespace

// ---------------------------------------------------------------------------
// AssetSidecarPath
// ---------------------------------------------------------------------------

TEST_CASE("Phase7 W1: AssetSidecarPath appends .rt2meta to the full filename")
{
    CHECK(AssetSidecarPath("assets/cube.glb").string() == "assets/cube.glb.rt2meta");
    CHECK(AssetSidecarPath("cube.obj").string() == "cube.obj.rt2meta");
    CHECK(AssetSidecarPath("a/b/c/night.exr").string() == "a/b/c/night.exr.rt2meta");
    // A path with no extension still appends.
    CHECK(AssetSidecarPath("scripts/move").string() == "scripts/move.rt2meta");
}

// ---------------------------------------------------------------------------
// ReadSidecarId
// ---------------------------------------------------------------------------

TEST_CASE("Phase7 W1: ReadSidecarId returns nil for an absent sidecar (not an error)")
{
    auto dir = UniqueTempDir("rt2_w1_absent");
    Error err;
    UUID id = ReadSidecarId(dir / "missing.rt2meta", err);
    CHECK(id.IsNull());
    CHECK(err.IsOk()); // absent is normal
    std::filesystem::remove_all(dir);
}

TEST_CASE("Phase7 W1: ReadSidecarId parses a well-formed sidecar")
{
    auto dir = UniqueTempDir("rt2_w1_ok");
    const auto sidecar = dir / "asset.glb.rt2meta";
    const UUID written = UUID::Parse("550e8400-e29b-41d4-a716-446655440000");
    { std::ofstream out(sidecar); out << written.ToString() << "\n"; }
    Error err;
    UUID id = ReadSidecarId(sidecar, err);
    CHECK(err.IsOk());
    CHECK(id == written);
    std::filesystem::remove_all(dir);
}

TEST_CASE("Phase7 W1: ReadSidecarId tolerates trailing whitespace and no newline")
{
    auto dir = UniqueTempDir("rt2_w1_ws");
    const auto sidecar = dir / "asset.glb.rt2meta";
    const UUID written = UUID::Parse("550e8400-e29b-41d4-a716-446655440000");
    { std::ofstream out(sidecar); out << written.ToString() << "\r\n"; }
    Error err;
    UUID id = ReadSidecarId(sidecar, err);
    CHECK(err.IsOk());
    CHECK(id == written);

    { std::ofstream out(sidecar, std::ios::trunc); out << written.ToString(); } // no newline
    err = Error{};
    id = ReadSidecarId(sidecar, err);
    CHECK(err.IsOk());
    CHECK(id == written);
    std::filesystem::remove_all(dir);
}

TEST_CASE("Phase7 W1: ReadSidecarId reports a Parse error for a malformed sidecar")
{
    auto dir = UniqueTempDir("rt2_w1_bad");
    const auto sidecar = dir / "asset.glb.rt2meta";
    { std::ofstream out(sidecar); out << "not-a-uuid\n"; }
    Error err;
    UUID id = ReadSidecarId(sidecar, err);
    CHECK(id.IsNull());
    CHECK(err.code == Error::Parse);
    CHECK(err.path == sidecar.string());
    std::filesystem::remove_all(dir);
}

TEST_CASE("Phase7 W1: ReadSidecarId reports a Parse error for an empty sidecar")
{
    auto dir = UniqueTempDir("rt2_w1_empty");
    const auto sidecar = dir / "asset.glb.rt2meta";
    { std::ofstream out(sidecar); out << ""; }
    Error err;
    UUID id = ReadSidecarId(sidecar, err);
    CHECK(id.IsNull());
    CHECK(err.code == Error::Parse);
    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// WriteSidecarId
// ---------------------------------------------------------------------------

TEST_CASE("Phase7 W1: WriteSidecarId writes and is idempotent")
{
    auto dir = UniqueTempDir("rt2_w1_write");
    const auto sidecar = dir / "asset.glb.rt2meta";
    const UUID id = UUID::Parse("550e8400-e29b-41d4-a716-446655440000");
    Error err;
    REQUIRE(WriteSidecarId(sidecar, id, err));
    CHECK(err.IsOk());
    // Round-trip: the written file parses back to the same ID.
    CHECK(ReadSidecarId(sidecar, err) == id);
    // Writing again overwrites atomically with the same value.
    REQUIRE(WriteSidecarId(sidecar, id, err));
    CHECK(ReadSidecarId(sidecar, err) == id);
    std::filesystem::remove_all(dir);
}

TEST_CASE("Phase7 W1: WriteSidecarId refuses to write a nil ID")
{
    auto dir = UniqueTempDir("rt2_w1_nil");
    const auto sidecar = dir / "asset.glb.rt2meta";
    Error err;
    CHECK_FALSE(WriteSidecarId(sidecar, UUID::Nil(), err));
    CHECK(err.code == Error::InvalidArgument);
    CHECK_FALSE(std::filesystem::exists(sidecar));
    std::filesystem::remove_all(dir);
}

TEST_CASE("Phase7 W1: WriteSidecarId creates the parent directory")
{
    auto dir = UniqueTempDir("rt2_w1_parent");
    const auto sidecar = dir / "nested" / "deeper" / "asset.glb.rt2meta";
    const UUID id = UUID::Parse("550e8400-e29b-41d4-a716-446655440000");
    Error err;
    REQUIRE(WriteSidecarId(sidecar, id, err));
    CHECK(std::filesystem::exists(sidecar));
    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// ResolveOrAssign
// ---------------------------------------------------------------------------

TEST_CASE("Phase7 W1: ResolveOrAssign mints a fresh ID and writes a sidecar when absent")
{
    auto dir = UniqueTempDir("rt2_w1_mint");
    const auto asset = dir / "cube.glb";
    { std::ofstream out(asset); out << "stub"; } // asset need not be real
    DeterministicUuidProvider provider;
    bool minted = false;
    Error err;
    const UUID id = ResolveOrAssign(asset, provider, minted, err);
    CHECK(!id.IsNull());
    CHECK(minted == true);
    CHECK(err.IsOk());
    // The sidecar was written and round-trips to the same ID.
    CHECK(ReadSidecarId(AssetSidecarPath(asset), err) == id);
    std::filesystem::remove_all(dir);
}

TEST_CASE("Phase7 W1: ResolveOrAssign reuses an existing sidecar's ID and does not mint")
{
    auto dir = UniqueTempDir("rt2_w1_reuse");
    const auto asset = dir / "cube.glb";
    { std::ofstream out(asset); out << "stub"; }
    const UUID existing = UUID::Parse("11111111-2222-4333-8444-555555555555");
    REQUIRE(WriteSidecarId(AssetSidecarPath(asset), existing, Error{}));

    DeterministicUuidProvider provider;
    bool minted = true; // should be reset to false
    Error err;
    const UUID id = ResolveOrAssign(asset, provider, minted, err);
    CHECK(id == existing);
    CHECK(minted == false);
    CHECK(err.IsOk());
    std::filesystem::remove_all(dir);
}

TEST_CASE("Phase7 W1: ResolveOrAssign overwrites a malformed sidecar and reports the parse error")
{
    auto dir = UniqueTempDir("rt2_w1_repair");
    const auto asset = dir / "cube.glb";
    { std::ofstream out(asset); out << "stub"; }
    { std::ofstream out(AssetSidecarPath(asset)); out << "garbage"; }

    DeterministicUuidProvider provider;
    bool minted = false;
    Error err;
    const UUID id = ResolveOrAssign(asset, provider, minted, err);
    CHECK(!id.IsNull());
    CHECK(minted == true);
    // The parse failure is reported so it surfaces as a diagnostic...
    CHECK(err.code == Error::Parse);
    // ...but the sidecar was overwritten with a valid ID.
    Error readErr;
    CHECK(ReadSidecarId(AssetSidecarPath(asset), readErr) == id);
    CHECK(readErr.IsOk());
    std::filesystem::remove_all(dir);
}

TEST_CASE("Phase7 W1: ResolveOrAssign is stable across calls with a deterministic provider")
{
    // Two successive imports of the same asset on the same machine return the
    // same ID: the first mints+writes, the second reads.
    auto dir = UniqueTempDir("rt2_w1_stable");
    const auto asset = dir / "cube.glb";
    { std::ofstream out(asset); out << "stub"; }
    DeterministicUuidProvider provider;
    bool minted1 = false, minted2 = false;
    Error err1, err2;
    const UUID id1 = ResolveOrAssign(asset, provider, minted1, err1);
    const UUID id2 = ResolveOrAssign(asset, provider, minted2, err2);
    CHECK(id1 == id2);
    CHECK(minted1 == true);
    CHECK(minted2 == false);
    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Serialization round-trip (D5 additive: nil on read is valid, written on save)
// ---------------------------------------------------------------------------

TEST_CASE("Phase7 W1: assetId is serialized when non-nil and omitted when nil")
{
    // The serializer's AssetReferenceToJson/JsonToAssetReference are
    // internal (anonymous namespace), so exercise them via the public
    // Save API and inspect the emitted JSON. A document whose imported
    // source has a non-nil assetId writes the field; a nil one omits it.
    DeterministicUuidProvider provider;
    SceneDocument doc;
    doc.SetUuidProvider(&provider);

    const auto entity = doc.ecs.registry.create();
    doc.AssignNewUuid(entity);
    doc.ecs.registry.emplace<Transform>(entity);
    doc.ecs.registry.emplace<VisibleComponent>(entity);
    ImportedMeshSourceComponent imported;
    imported.model.kind = AssetKind::Model;
    imported.model.path = "assets/cube.glb";
    imported.model.sourceKey = "gltf:scene=0:node=0:mesh=0:prim=0";
    imported.model.assetId = UUID::Parse("550e8400-e29b-41d4-a716-446655440000");
    doc.ecs.registry.emplace<ImportedMeshSourceComponent>(entity, imported);

    const auto path = std::filesystem::temp_directory_path() / "rt2_w1_serialized.rt2scene";
    Error err;
    REQUIRE(SaveSceneForTest(doc, path, err));
    nlohmann::json saved;
    { std::ifstream in(path); in >> saved; }
    CHECK(saved["entities"][0]["importedSource"].contains("assetId"));
    CHECK(saved["entities"][0]["importedSource"]["assetId"] ==
          "550e8400-e29b-41d4-a716-446655440000");
    std::filesystem::remove(path);

    // A nil assetId is omitted (additive over v3: a v3 reader sees no field).
    imported.model.assetId = UUID::Nil();
    doc.ecs.registry.emplace_or_replace<ImportedMeshSourceComponent>(entity, imported);
    REQUIRE(SaveSceneForTest(doc, path, err));
    { std::ifstream in(path); in >> saved; }
    CHECK_FALSE(saved["entities"][0]["importedSource"].contains("assetId"));
    std::filesystem::remove(path);
}

TEST_CASE("Phase7 W1: a v3 scene (no assetId field) loads with nil assetIds")
{
    // A hand-authored v3 .rt2scene with no assetId field on importedSource.
    // The loader must treat absence as nil (additive migration per D5).
    const std::string v3Scene = R"({
      "version": 3,
      "metadata": {"name": "v3fixture"},
      "entities": [
        {
          "uuid": "550e8400-e29b-41d4-a716-446655440000",
          "name": "Imported",
          "parent": "",
          "visible": true,
          "transform": {"translation": [0,0,0], "rotation": [1,0,0,0], "scale": [1,1,1]},
          "importedSource": {
            "kind": "model",
            "path": "assets/cube.glb",
            "sourceKey": "gltf:scene=0:node=0:mesh=0:prim=0",
            "importSettings": {"triangulate": true, "generateNormals": false, "mergeMegaMesh": true}
          }
        }
      ],
      "materials": [],
      "textures": [],
      "camera": {"fov": 60, "aperture": 0, "focusDist": 1, "forward": [0,0,-1]},
      "envMap": {"path": "", "width": 0, "height": 0}
    })";
    const auto path = std::filesystem::temp_directory_path() / "rt2_w1_v3.rt2scene";
    { std::ofstream out(path); out << v3Scene; }

    DeterministicUuidProvider provider;
    SceneDocument loaded;
    loaded.SetUuidProvider(&provider);
    SceneLoadReport report;
    Error err;
    REQUIRE(SceneSerializer::Load(loaded, path, report, err));
    REQUIRE(err.IsOk());

    auto view = loaded.ecs.registry.view<ImportedMeshSourceComponent>();
    REQUIRE(std::distance(view.begin(), view.end()) == 1);
    const auto& ref = loaded.ecs.registry.get<ImportedMeshSourceComponent>(*view.begin());
    CHECK(ref.model.assetId.IsNull()); // additive: absent -> nil
    CHECK(ref.model.path == "assets/cube.glb");
    std::filesystem::remove(path);
}

TEST_CASE("Phase7 W1: assetId survives a save/load round-trip")
{
    DeterministicUuidProvider provider;
    SceneDocument doc;
    doc.SetUuidProvider(&provider);

    const auto entity = doc.ecs.registry.create();
    doc.AssignNewUuid(entity);
    doc.ecs.registry.emplace<Transform>(entity);
    doc.ecs.registry.emplace<VisibleComponent>(entity);
    ImportedMeshSourceComponent imported;
    imported.model.kind = AssetKind::Model;
    imported.model.path = "assets/cube.glb";
    imported.model.sourceKey = "gltf:scene=0:node=0:mesh=0:prim=0";
    imported.model.assetId = UUID::Parse("11111111-2222-4333-8444-555555555555");
    doc.ecs.registry.emplace<ImportedMeshSourceComponent>(entity, imported);

    const auto path = std::filesystem::temp_directory_path() / "rt2_w1_roundtrip.rt2scene";
    Error err;
    REQUIRE(SaveSceneForTest(doc, path, err));

    nlohmann::json saved;
    { std::ifstream in(path); in >> saved; }
    CHECK(saved["entities"][0]["importedSource"].contains("assetId"));
    CHECK(saved["entities"][0]["importedSource"]["assetId"] ==
          "11111111-2222-4333-8444-555555555555");

    SceneDocument loaded;
    loaded.SetUuidProvider(&provider);
    SceneLoadReport report;
    REQUIRE(SceneSerializer::Load(loaded, path, report, err));

    auto view = loaded.ecs.registry.view<ImportedMeshSourceComponent>();
    REQUIRE(std::distance(view.begin(), view.end()) == 1);
    const auto& roundTrip = loaded.ecs.registry.get<ImportedMeshSourceComponent>(*view.begin());
    CHECK(roundTrip.model.assetId == imported.model.assetId);

    std::filesystem::remove(path);
}
