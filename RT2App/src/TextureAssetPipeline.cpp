#include "TextureAssetPipeline.h"

#include <algorithm>
#include <tuple>

namespace rt2::core {

namespace {

const char* ObjTextureRoleName(ObjTextureRole role)
{
    switch (role)
    {
        case ObjTextureRole::Diffuse:   return "diffuse";
        case ObjTextureRole::Normal:    return "normal";
        case ObjTextureRole::Emissive:  return "emissive";
        case ObjTextureRole::Roughness: return "roughness";
    }
    return "diffuse";
}

} // namespace

std::string GltfImageSourceKey(size_t imageIndex)
{
    return "gltf:image=" + std::to_string(imageIndex);
}

std::string GltfInvalidTextureSourceKey(size_t textureIndex)
{
    return "gltf:texture=" + std::to_string(textureIndex);
}

std::string ObjTextureSourceKey(size_t materialIndex, ObjTextureRole role)
{
    return "obj:material=" + std::to_string(materialIndex)
        + ":texture=" + ObjTextureRoleName(role);
}

void SortTextureManifest(TextureManifest& manifest)
{
    std::stable_sort(manifest.begin(), manifest.end(),
        [](const TextureManifestEntry& a, const TextureManifestEntry& b) {
            return std::tie(a.outputSlot, a.ref.sourceKey, a.materialIndex,
                            a.externalUri)
                 < std::tie(b.outputSlot, b.ref.sourceKey, b.materialIndex,
                            b.externalUri);
        });
}

bool CaptureGltfImageData(tinygltf::Image* image,
                          int imageIndex,
                          std::string* error,
                          std::string* warning,
                          int requestedWidth,
                          int requestedHeight,
                          const unsigned char* bytes,
                          int size,
                          void* userData)
{
    (void)image;
    (void)error;
    (void)warning;
    (void)requestedWidth;
    (void)requestedHeight;

    auto* capture = static_cast<GltfImageCapture*>(userData);
    if (!capture || imageIndex < 0)
        return false;

    const size_t index = static_cast<size_t>(imageIndex);
    if (capture->encodedByImage.size() <= index)
        capture->encodedByImage.resize(index + 1);

    auto& payload = capture->encodedByImage[index];
    if (bytes && size > 0)
        payload.assign(bytes, bytes + static_cast<size_t>(size));
    else
        payload.clear();
    // Byte validity is intentionally not decided here.
    return true;
}

SceneTexture MakeMissingTexturePlaceholder(const AssetReference& ref)
{
    SceneTexture texture;
    texture.filepath = ref.path;
    texture.ref = ref;
    texture.width = 2;
    texture.height = 2;
    texture.channels = 4;
    texture.pixels = {
        0xff, 0x00, 0xff, 0xff,
        0x00, 0x00, 0x00, 0xff,
        0x00, 0x00, 0x00, 0xff,
        0xff, 0x00, 0xff, 0xff,
    };
    texture.isHDR = false;
    texture.floatPixels.clear();
    texture.isSRGB = false;
    return texture;
}

} // namespace rt2::core
