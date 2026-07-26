#include <doctest/doctest.h>

#include "TextureAssetPipeline.h"

#include <array>
#include <string>
#include <vector>

using namespace rt2::core;

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

    CHECK(texture.filepath == ref.path);
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
