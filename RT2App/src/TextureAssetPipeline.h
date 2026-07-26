#pragma once

#ifndef RT2_CORE_TEXTURE_ASSET_PIPELINE_H
#define RT2_CORE_TEXTURE_ASSET_PIPELINE_H

#include "AssetResolver.h"
#include "SceneTypes.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace tinygltf {
struct Image;
class Model;
}

namespace tinyobj {
struct material_t;
}

namespace rt2::core {

enum class TextureIdentityMode : uint8_t
{
    ReadOnly,
    ExplicitImport,
};

struct TextureAssetLoadContext
{
    AssetResolutionContext resolution;
    AssetReference         ownerModel;
    std::filesystem::path  resolvedOwnerPath;
    UUID                   effectiveOwnerId;
    UUID                   entityUuid;
    std::string            entityName;
    TextureIdentityMode    identityMode = TextureIdentityMode::ReadOnly;
    IUuidProvider*         uuidProvider = nullptr;
};

// Build the context for an OS-dialog/CLI/direct import. The host must already
// have an absolute path; this helper normalizes it but never consults CWD or
// assigns identity. Returns false and appends Model/Malformed on invalid
// input.
bool BuildExplicitImportTextureContext(
    const std::filesystem::path& ownerPath,
    IUuidProvider* uuidProvider,
    TextureAssetLoadContext& context,
    std::vector<AssetDiagnostic>& diagnostics);

enum class TexturePayloadKind : uint8_t
{
    External,
    Embedded,
    Invalid,
};

enum class ObjTextureRole : uint8_t
{
    Diffuse,
    Normal,
    Emissive,
    Roughness,
};

// Neutral deterministic input to the shared resolver/decode stage. Format
// adapters fill these records in source order; the pipeline owns identity,
// decode, containment, and diagnostic policy.
struct TextureManifestEntry
{
    size_t                     outputSlot = 0;
    AssetReference             ref;
    TexturePayloadKind         payloadKind = TexturePayloadKind::Invalid;
    std::string                externalUri;
    std::vector<unsigned char> encodedBytes;
    int                        materialIndex = -1;
    ObjTextureRole             objRole = ObjTextureRole::Diffuse;
    bool                       isSRGB = false;
};

using TextureManifest = std::vector<TextureManifestEntry>;
using GltfTextureManifest = TextureManifest;
using ObjTextureManifest = TextureManifest;

// TinyGLTF callback state. Payloads are indexed by glTF image index so
// callback order cannot alter the manifest.
struct GltfImageCapture
{
    std::vector<std::vector<unsigned char>> encodedByImage;
};

std::string GltfImageSourceKey(size_t imageIndex);
std::string GltfInvalidTextureSourceKey(size_t textureIndex);
std::string ObjTextureSourceKey(size_t materialIndex, ObjTextureRole role);

// Stable source ordering used by the format adapters and by tests. This
// sorts by output slot, then canonical source key, then material binding.
void SortTextureManifest(TextureManifest& manifest);

// Capture encoded bytes without decoding them. Malformed image content is
// deliberately accepted here and is contained during the later CPU decode
// stage, so it cannot turn a structurally valid glTF into a model failure.
bool CaptureGltfImageData(tinygltf::Image* image,
                          int imageIndex,
                          std::string* error,
                          std::string* warning,
                          int requestedWidth,
                          int requestedHeight,
                          const unsigned char* bytes,
                          int size,
                          void* userData);

GltfTextureManifest EnumerateGltfTextureDependencies(
    const tinygltf::Model& model,
    const GltfImageCapture& capture,
    const TextureAssetLoadContext& context);

ObjTextureManifest EnumerateObjTextureDependencies(
    const std::vector<tinyobj::material_t>& materials,
    const TextureAssetLoadContext& context);

// Sole W3 texture containment value.
SceneTexture MakeMissingTexturePlaceholder(const AssetReference& ref);
bool IsMissingTexturePlaceholder(const SceneTexture& texture);

// Implemented as loader formats cut over in W3 steps 6.3 and 6.4.
std::vector<SceneTexture> ResolveAndDecodeTextures(
    const TextureManifest& manifest,
    const TextureAssetLoadContext& context,
    std::vector<AssetDiagnostic>& diagnostics);

} // namespace rt2::core

#endif // RT2_CORE_TEXTURE_ASSET_PIPELINE_H
