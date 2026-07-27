#include <doctest/doctest.h>

#include "SceneLoader.h"
#include "TextureAssetPipeline.h"

#include <array>
#include <filesystem>
#include <string>
#include <vector>

using namespace rt2::core;

TEST_CASE("Phase7 W3 step 7.2: explicit import context rejects implicit roots")
{
    DeterministicUuidProvider ids;
    for (const std::filesystem::path path :
         { std::filesystem::path{}, std::filesystem::path{"model.glb"} })
    {
        TextureAssetLoadContext context;
        std::vector<AssetDiagnostic> diagnostics;
        CHECK_FALSE(BuildExplicitImportTextureContext(
            path, &ids, context, diagnostics));
        CHECK(context.resolvedOwnerPath.empty());
        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0].severity == AssetDiagnostic::Malformed);
        CHECK(diagnostics[0].kind == AssetKind::Model);
        CHECK(diagnostics[0].detail == "direct model path must be absolute");
    }
}

TEST_CASE("Phase7 W3 step 7.2: explicit import context normalizes without probing or minting")
{
    DeterministicUuidProvider ids;
    const auto absolute =
        (std::filesystem::temp_directory_path() /
         "rt2-context-builder" / ".." / "model.glb").lexically_normal();
    TextureAssetLoadContext context;
    std::vector<AssetDiagnostic> diagnostics;

    REQUIRE(BuildExplicitImportTextureContext(
        absolute, &ids, context, diagnostics));
    CHECK(diagnostics.empty());
    CHECK(context.resolvedOwnerPath == absolute);
    CHECK(context.resolution.assetRoot == absolute.parent_path());
    CHECK(context.ownerModel.kind == AssetKind::Model);
    CHECK(context.ownerModel.path == "model.glb");
    CHECK(context.ownerModel.assetId.IsNull());
    CHECK(context.identityMode == TextureIdentityMode::ExplicitImport);
    CHECK(context.uuidProvider == &ids);
}

TEST_CASE("Phase7 W3 step 7.2: loader rejects a relative context before mutation")
{
    ECSScene scene;
    const entt::entity existing = scene.registry.create();
    TextureAssetLoadContext context;
    context.resolvedOwnerPath = "model.glb";
    context.ownerModel.kind = AssetKind::Model;
    context.ownerModel.path = "model.glb";
    std::vector<AssetDiagnostic> diagnostics;

    CHECK_FALSE(SceneLoader::LoadIntoECS(scene, context, diagnostics));
    CHECK(scene.registry.valid(existing));
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].severity == AssetDiagnostic::Malformed);
    CHECK(diagnostics[0].kind == AssetKind::Model);
    CHECK(diagnostics[0].detail ==
          "model load context requires an absolute resolved owner path");
}

TEST_CASE("Phase7 W3 step 6.2: missing texture placeholder is exact")
{
    AssetReference ref;
    ref.kind = AssetKind::Texture;
    ref.path = "textures/missing.png";
    ref.sourceKey = "gltf:image=3";
    ref.assetId = UUID::Parse("11223344-5566-4788-99aa-bbccddeeff00");

    const SceneTexture texture = MakeMissingTexturePlaceholder(ref);
    const std::vector<unsigned char> expected = {
        0xff, 0x00, 0xff, 0xff,
        0x00, 0x00, 0x00, 0xff,
        0x00, 0x00, 0x00, 0xff,
        0xff, 0x00, 0xff, 0xff,
    };

    CHECK(texture.ref.path == ref.path);
    CHECK(texture.ref.kind == AssetKind::Texture);
    CHECK(texture.ref.path == ref.path);
    CHECK(texture.ref.sourceKey == ref.sourceKey);
    CHECK(texture.ref.assetId == ref.assetId);
    CHECK(texture.width == 2);
    CHECK(texture.height == 2);
    CHECK(texture.channels == 4);
    CHECK(texture.pixels == expected);
    CHECK_FALSE(texture.isHDR);
    CHECK(texture.floatPixels.empty());
    CHECK_FALSE(texture.isSRGB);
}

TEST_CASE("Phase7 W3 step 6.2: manifest keys and ordering are canonical")
{
    CHECK(GltfImageSourceKey(0) == "gltf:image=0");
    CHECK(GltfImageSourceKey(17) == "gltf:image=17");
    CHECK(GltfInvalidTextureSourceKey(4) == "gltf:texture=4");

    const std::array<ObjTextureRole, 4> roles = {
        ObjTextureRole::Diffuse,
        ObjTextureRole::Normal,
        ObjTextureRole::Emissive,
        ObjTextureRole::Roughness,
    };
    const std::array<std::string, 4> expectedRoleKeys = {
        "obj:material=2:texture=diffuse",
        "obj:material=2:texture=normal",
        "obj:material=2:texture=emissive",
        "obj:material=2:texture=roughness",
    };
    for (size_t i = 0; i < roles.size(); ++i)
        CHECK(ObjTextureSourceKey(2, roles[i]) == expectedRoleKeys[i]);

    TextureManifest manifest(3);
    manifest[0].outputSlot = 2;
    manifest[0].ref.sourceKey = GltfImageSourceKey(2);
    manifest[1].outputSlot = 0;
    manifest[1].ref.sourceKey = GltfImageSourceKey(0);
    manifest[2].outputSlot = 1;
    manifest[2].ref.sourceKey = GltfImageSourceKey(1);

    SortTextureManifest(manifest);

    REQUIRE(manifest.size() == 3);
    for (size_t i = 0; i < manifest.size(); ++i)
    {
        CHECK(manifest[i].outputSlot == i);
        CHECK(manifest[i].ref.sourceKey == GltfImageSourceKey(i));
    }
}

TEST_CASE("Phase7 W3 step 6.2: capture callback preserves indexed encoded payloads")
{
    GltfImageCapture capture;
    const std::array<unsigned char, 4> later = { 0xde, 0xad, 0xbe, 0xef };
    const std::array<unsigned char, 3> earlier = { 0x00, 0xff, 0x13 };
    std::string error;
    std::string warning;

    CHECK(CaptureGltfImageData(nullptr, 2, &error, &warning, 0, 0,
                               later.data(), static_cast<int>(later.size()),
                               &capture));
    CHECK(CaptureGltfImageData(nullptr, 0, &error, &warning, 0, 0,
                               earlier.data(), static_cast<int>(earlier.size()),
                               &capture));
    CHECK(CaptureGltfImageData(nullptr, 1, &error, &warning, 0, 0,
                               nullptr, 0, &capture));

    REQUIRE(capture.encodedByImage.size() == 3);
    CHECK(capture.encodedByImage[0]
          == std::vector<unsigned char>(earlier.begin(), earlier.end()));
    CHECK(capture.encodedByImage[1].empty());
    CHECK(capture.encodedByImage[2]
          == std::vector<unsigned char>(later.begin(), later.end()));
    CHECK(error.empty());
    CHECK(warning.empty());
}

// ============================================================================
// Dielectric-default correction — opt-in workaround for glTF's metallicFactor
// ============================================================================

TEST_CASE("Dielectric correction only touches untextured fully-metallic materials")
{
    std::vector<SceneMaterial> materials;

    // 0: the target — spec default, no metallicRoughness texture.
    SceneMaterial defaulted;
    defaulted.metallic = 1.0f;
    defaulted.roughness = 1.0f;
    defaulted.metallicRoughnessTextureIndex = -1;
    materials.push_back(defaulted);

    // 1: fully metallic but textured — the texture modulates it, so the
    //    factor of 1.0 is meaningful and must be left alone.
    SceneMaterial textured;
    textured.metallic = 1.0f;
    textured.metallicRoughnessTextureIndex = 7;
    materials.push_back(textured);

    // 2: deliberately authored metal, untextured. Not the default shape.
    SceneMaterial authored;
    authored.metallic = 0.5f;
    authored.metallicRoughnessTextureIndex = -1;
    materials.push_back(authored);

    // 3: already dielectric — nothing to do.
    SceneMaterial dielectric;
    dielectric.metallic = 0.0f;
    dielectric.metallicRoughnessTextureIndex = -1;
    materials.push_back(dielectric);

    AssetReference owner;
    owner.kind = AssetKind::Model;
    owner.path = "models/sponza.gltf";

    std::vector<AssetDiagnostic> diagnostics;
    const size_t corrected =
        ApplyDielectricDefaultCorrection(materials, 0, owner, diagnostics);

    CHECK(corrected == 1);
    CHECK(materials[0].metallic == doctest::Approx(0.0f));
    CHECK(materials[1].metallic == doctest::Approx(1.0f));
    CHECK(materials[2].metallic == doctest::Approx(0.5f));
    CHECK(materials[3].metallic == doctest::Approx(0.0f));

    // A silent fix would be worse than the bug it fixes.
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].severity == AssetDiagnostic::Stale);
    CHECK(diagnostics[0].kind == AssetKind::Model);
    CHECK(diagnostics[0].refPath == "models/sponza.gltf");
    CHECK(diagnostics[0].detail.find("dielectric") != std::string::npos);
}

TEST_CASE("Dielectric correction skips materials before the import's first index")
{
    // Import appends to an existing scene, so the pass must not reach back
    // into materials that were already there and correct them retroactively.
    std::vector<SceneMaterial> materials;
    SceneMaterial preexisting;
    preexisting.metallic = 1.0f;
    preexisting.metallicRoughnessTextureIndex = -1;
    materials.push_back(preexisting);

    SceneMaterial imported;
    imported.metallic = 1.0f;
    imported.metallicRoughnessTextureIndex = -1;
    materials.push_back(imported);

    AssetReference owner;
    owner.kind = AssetKind::Model;
    owner.path = "models/added.gltf";

    std::vector<AssetDiagnostic> diagnostics;
    const size_t corrected =
        ApplyDielectricDefaultCorrection(materials, 1, owner, diagnostics);

    CHECK(corrected == 1);
    CHECK(materials[0].metallic == doctest::Approx(1.0f)); // untouched
    CHECK(materials[1].metallic == doctest::Approx(0.0f));
}
