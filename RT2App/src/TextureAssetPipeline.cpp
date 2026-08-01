#include "TextureAssetPipeline.h"

#include "stb_image.h"
#include "tiny_gltf.h"
#include "tinyobjloader/tiny_obj_loader.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>
#include <tuple>

namespace rt2::core {

bool BuildExplicitImportTextureContext(
    const std::filesystem::path& ownerPath,
    IUuidProvider* uuidProvider,
    const AssetResolutionContext& resolution,
    TextureAssetLoadContext& context,
    std::vector<AssetDiagnostic>& diagnostics)
{
    context = {};
    const auto normalized = ownerPath.lexically_normal();
    if (normalized.empty() || !normalized.is_absolute())
    {
        AssetDiagnostic diagnostic;
        diagnostic.severity = AssetDiagnostic::Malformed;
        diagnostic.kind = AssetKind::Model;
        diagnostic.refPath = ownerPath.generic_string();
        diagnostic.detail = "direct model path must be absolute";
        diagnostics.push_back(std::move(diagnostic));
        return false;
    }
    if (uuidProvider == nullptr)
    {
        AssetDiagnostic diagnostic;
        diagnostic.severity = AssetDiagnostic::Malformed;
        diagnostic.kind = AssetKind::Model;
        diagnostic.refPath = normalized.filename().generic_string();
        diagnostic.resolvedPath = normalized.string();
        diagnostic.detail = "explicit model import requires a UUID provider";
        diagnostics.push_back(std::move(diagnostic));
        return false;
    }
    if (resolution.assetRoot.empty() || !resolution.assetRoot.is_absolute())
    {
        AssetDiagnostic diagnostic;
        diagnostic.severity = AssetDiagnostic::Malformed;
        diagnostic.kind = AssetKind::Model;
        diagnostic.refPath = normalized.generic_string();
        diagnostic.detail = "explicit import requires an absolute asset root";
        diagnostics.push_back(std::move(diagnostic));
        return false;
    }

    context.resolvedOwnerPath = normalized;
    context.resolution = resolution;
    context.ownerModel.kind = AssetKind::Model;
    const auto relative = normalized.lexically_relative(
        resolution.assetRoot.lexically_normal());
    context.ownerModel.path = relative.empty() ||
        (!relative.empty() && *relative.begin() == "..")
        ? normalized.generic_u8string()
        : relative.generic_u8string();
    context.identityMode = TextureIdentityMode::ExplicitImport;
    context.uuidProvider = uuidProvider;
    return true;
}

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

AssetDiagnostic MakeTextureDiagnostic(
    AssetDiagnostic::Severity severity,
    const AssetReference& ref,
    const TextureAssetLoadContext& context,
    const std::filesystem::path& resolvedPath,
    std::string detail)
{
    AssetDiagnostic diagnostic;
    diagnostic.severity = severity;
    diagnostic.kind = AssetKind::Texture;
    diagnostic.refPath = ref.path;
    diagnostic.resolvedPath = resolvedPath.string();
    diagnostic.entityUuid = context.entityUuid;
    diagnostic.entityName = context.entityName;
    diagnostic.sourceKey = ref.sourceKey;
    diagnostic.detail = std::move(detail);
    return diagnostic;
}

bool IsDataUri(const std::string& uri)
{
    return uri.rfind("data:", 0) == 0;
}

std::string PortablePathFor(const std::filesystem::path& physicalPath,
                            const std::filesystem::path& assetRoot)
{
    const auto relative = physicalPath.lexically_normal().lexically_relative(
        assetRoot.lexically_normal());
    if (!relative.empty())
        return relative.generic_string();
    return physicalPath.lexically_normal().generic_string();
}

std::filesystem::path PhysicalPathFor(
    const AssetReference& ref,
    const AssetResolutionContext& resolution)
{
    const std::filesystem::path stored =
        std::filesystem::u8path(ref.path).lexically_normal();
    return stored.is_absolute()
        ? stored
        : (resolution.assetRoot / stored).lexically_normal();
}

bool ReadFileBytes(const std::filesystem::path& path,
                   std::vector<unsigned char>& bytes)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    bytes.assign(std::istreambuf_iterator<char>(input),
                 std::istreambuf_iterator<char>());
    return input.good() || input.eof();
}

bool DecodeTextureBytes(const std::vector<unsigned char>& encoded,
                        SceneTexture& texture)
{
    if (encoded.empty())
        return false;

    int width = 0;
    int height = 0;
    int sourceChannels = 0;
    unsigned char* decoded = stbi_load_from_memory(
        encoded.data(), static_cast<int>(encoded.size()),
        &width, &height, &sourceChannels, 4);
    if (!decoded)
        return false;

    texture.width = width;
    texture.height = height;
    texture.channels = 4;
    texture.pixels.assign(
        decoded, decoded + static_cast<size_t>(width * height * 4));
    stbi_image_free(decoded);
    texture.isHDR = false;
    texture.floatPixels.clear();
    texture.isSRGB = false;
    return true;
}

void SortAppendedDiagnostics(std::vector<AssetDiagnostic>& diagnostics,
                             size_t base)
{
    if (base >= diagnostics.size())
        return;
    std::stable_sort(diagnostics.begin() + static_cast<std::ptrdiff_t>(base),
                     diagnostics.end(),
        [](const AssetDiagnostic& a, const AssetDiagnostic& b) {
            return AssetDiagnosticSortKey(a) < AssetDiagnosticSortKey(b);
        });
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

GltfTextureManifest EnumerateGltfTextureDependencies(
    const tinygltf::Model& model,
    const GltfImageCapture& capture,
    const TextureAssetLoadContext& context)
{
    GltfTextureManifest manifest;
    manifest.reserve(model.textures.size());

    for (size_t textureIndex = 0;
         textureIndex < model.textures.size(); ++textureIndex)
    {
        const auto& sourceTexture = model.textures[textureIndex];
        TextureManifestEntry entry;
        entry.outputSlot = textureIndex;
        entry.ref.kind = AssetKind::Texture;

        if (sourceTexture.source < 0 ||
            sourceTexture.source >= static_cast<int>(model.images.size()))
        {
            entry.payloadKind = TexturePayloadKind::Invalid;
            entry.ref.sourceKey =
                GltfInvalidTextureSourceKey(textureIndex);
            entry.ref.path = context.ownerModel.path;
            entry.ref.assetId = context.effectiveOwnerId;
            manifest.push_back(std::move(entry));
            continue;
        }

        const size_t imageIndex =
            static_cast<size_t>(sourceTexture.source);
        const auto& image = model.images[imageIndex];
        entry.ref.sourceKey = GltfImageSourceKey(imageIndex);

        if (image.uri.empty() || IsDataUri(image.uri))
        {
            entry.payloadKind = TexturePayloadKind::Embedded;
            entry.ref.path = context.ownerModel.path;
            entry.ref.assetId = context.effectiveOwnerId;
            if (imageIndex < capture.encodedByImage.size())
                entry.encodedBytes = capture.encodedByImage[imageIndex];
        }
        else
        {
            entry.payloadKind = TexturePayloadKind::External;
            entry.externalUri = image.uri;
            const auto physical =
                (context.resolvedOwnerPath.parent_path() /
                 std::filesystem::u8path(image.uri)).lexically_normal();
            entry.ref.path =
                PortablePathFor(physical, context.resolution.assetRoot);
        }

        manifest.push_back(std::move(entry));
    }

    SortTextureManifest(manifest);
    return manifest;
}

ObjTextureManifest EnumerateObjTextureDependencies(
    const std::vector<tinyobj::material_t>& materials,
    const TextureAssetLoadContext& context)
{
    ObjTextureManifest manifest;
    size_t outputSlot = 0;

    auto append = [&](size_t materialIndex,
                      ObjTextureRole role,
                      const std::string& textureName,
                      bool isSRGB) {
        if (textureName.empty())
            return;

        TextureManifestEntry entry;
        entry.outputSlot = outputSlot++;
        entry.ref.kind = AssetKind::Texture;
        entry.ref.sourceKey = ObjTextureSourceKey(materialIndex, role);
        entry.payloadKind = TexturePayloadKind::External;
        entry.externalUri = textureName;
        entry.materialIndex = static_cast<int>(materialIndex);
        entry.objRole = role;
        entry.isSRGB = isSRGB;

        const auto physical =
            (context.resolvedOwnerPath.parent_path() /
             std::filesystem::u8path(textureName)).lexically_normal();
        entry.ref.path =
            PortablePathFor(physical, context.resolution.assetRoot);
        manifest.push_back(std::move(entry));
    };

    for (size_t materialIndex = 0;
         materialIndex < materials.size(); ++materialIndex)
    {
        const auto& material = materials[materialIndex];
        append(materialIndex, ObjTextureRole::Diffuse,
               material.diffuse_texname, true);
        append(materialIndex, ObjTextureRole::Normal,
               material.normal_texname, false);
        append(materialIndex, ObjTextureRole::Emissive,
               material.emissive_texname, true);
        append(materialIndex, ObjTextureRole::Roughness,
               material.roughness_texname, false);
    }

    SortTextureManifest(manifest);
    return manifest;
}

SceneTexture MakeMissingTexturePlaceholder(const AssetReference& ref)
{
    SceneTexture texture;
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

bool IsMissingTexturePlaceholder(const SceneTexture& texture)
{
    static const std::vector<unsigned char> expected = {
        0xff, 0x00, 0xff, 0xff,
        0x00, 0x00, 0x00, 0xff,
        0x00, 0x00, 0x00, 0xff,
        0xff, 0x00, 0xff, 0xff,
    };
    return texture.width == 2 &&
           texture.height == 2 &&
           texture.channels == 4 &&
           texture.pixels == expected &&
           !texture.isHDR &&
           texture.floatPixels.empty() &&
           !texture.isSRGB;
}

std::vector<SceneTexture> ResolveAndDecodeTextures(
    const TextureManifest& manifest,
    const TextureAssetLoadContext& context,
    std::vector<AssetDiagnostic>& diagnostics)
{
    const size_t diagnosticBase = diagnostics.size();
    std::vector<SceneTexture> textures;
    textures.reserve(manifest.size());

    const bool invalidContext =
        context.resolution.assetRoot.empty() ||
        !context.resolution.assetRoot.is_absolute() ||
        context.resolvedOwnerPath.empty() ||
        !context.resolvedOwnerPath.is_absolute() ||
        (context.identityMode == TextureIdentityMode::ExplicitImport &&
         context.uuidProvider == nullptr);

    for (const auto& entry : manifest)
    {
        AssetReference ref = entry.ref;
        ref.kind = AssetKind::Texture;

        if (invalidContext)
        {
            diagnostics.push_back(MakeTextureDiagnostic(
                AssetDiagnostic::Malformed, ref, context, {},
                "invalid texture load context"));
            textures.push_back(MakeMissingTexturePlaceholder(ref));
            continue;
        }

        if (entry.payloadKind == TexturePayloadKind::Invalid)
        {
            diagnostics.push_back(MakeTextureDiagnostic(
                AssetDiagnostic::Unresolved, ref, context,
                context.resolvedOwnerPath,
                "glTF texture source index is invalid"));
            textures.push_back(MakeMissingTexturePlaceholder(ref));
            continue;
        }

        if (entry.payloadKind == TexturePayloadKind::Embedded)
        {
            SceneTexture texture;
            texture.ref = ref;
            if (!DecodeTextureBytes(entry.encodedBytes, texture))
            {
                const auto severity = entry.encodedBytes.empty()
                    ? AssetDiagnostic::Unresolved
                    : AssetDiagnostic::Malformed;
                diagnostics.push_back(MakeTextureDiagnostic(
                    severity, ref, context, context.resolvedOwnerPath,
                    entry.encodedBytes.empty()
                        ? "embedded image payload is missing"
                        : "embedded image payload failed to decode"));
                texture = MakeMissingTexturePlaceholder(ref);
            }
            textures.push_back(std::move(texture));
            continue;
        }

        // A database dependency claim, when supplied, is authoritative over
        // the URI-derived fallback. Multiple distinct claims are a conflict.
        if (context.resolution.database)
        {
            const auto claims =
                context.resolution.database->FindDependenciesBySourceKey(
                    context.ownerModel.path, ref.sourceKey);
            if (claims.size() > 1)
            {
                diagnostics.push_back(MakeTextureDiagnostic(
                    AssetDiagnostic::Conflict, ref, context, {},
                    "multiple dependency claims for texture source key"));
                textures.push_back(MakeMissingTexturePlaceholder(ref));
                continue;
            }
            if (claims.size() == 1)
            {
                if (claims[0].kind != AssetKind::Texture)
                {
                    diagnostics.push_back(MakeTextureDiagnostic(
                        AssetDiagnostic::Conflict, ref, context, {},
                        "dependency claim kind is not Texture"));
                    textures.push_back(MakeMissingTexturePlaceholder(ref));
                    continue;
                }
                ref.path = claims[0].sourcePath;
                ref.assetId = claims[0].assetId;
            }
        }

        std::vector<AssetDiagnostic> locatorDiagnostics;
        auto resolution = Resolve(
            ref, context.resolution, context.entityUuid,
            context.entityName, locatorDiagnostics);
        if (!resolution.success)
        {
            for (auto& diagnostic : locatorDiagnostics)
                diagnostics.push_back(std::move(diagnostic));
            textures.push_back(MakeMissingTexturePlaceholder(ref));
            continue;
        }

        std::filesystem::path physicalPath = resolution.resolvedPath;
        ref.assetId = resolution.effectiveId;
        if (ref.assetId.IsNull() && !entry.ref.assetId.IsNull())
            ref.assetId = entry.ref.assetId;

        if (context.identityMode == TextureIdentityMode::ExplicitImport)
        {
            bool minted = false;
            Error identityError;
            const UUID assigned = ResolveOrAssign(
                physicalPath, *context.uuidProvider, minted, identityError);
            if (!assigned.IsNull())
                ref.assetId = assigned;
            ref.path = PortablePathFor(
                physicalPath, context.resolution.assetRoot);

            if (!identityError.IsOk())
            {
                diagnostics.push_back(MakeTextureDiagnostic(
                    AssetDiagnostic::Stale, ref, context, physicalPath,
                    "texture sidecar repair: " + identityError.Format()));
            }

            // Successful explicit repair supersedes the pre-repair
            // missing-sidecar advisory. Preserve unrelated locator advisories.
            for (auto& diagnostic : locatorDiagnostics)
            {
                const bool repairAdvisory =
                    diagnostic.severity == AssetDiagnostic::Stale &&
                    resolution.identityRepairRequired;
                if (!repairAdvisory)
                    diagnostics.push_back(std::move(diagnostic));
            }
        }
        else
        {
            for (auto& diagnostic : locatorDiagnostics)
                diagnostics.push_back(std::move(diagnostic));
        }

        std::vector<unsigned char> encoded;
        SceneTexture texture;
        texture.ref = ref;
        if (!ReadFileBytes(physicalPath, encoded) ||
            !DecodeTextureBytes(encoded, texture))
        {
            diagnostics.push_back(MakeTextureDiagnostic(
                AssetDiagnostic::Malformed, ref, context, physicalPath,
                "external image failed to decode"));
            texture = MakeMissingTexturePlaceholder(ref);
        }
        else
        {
            texture.isSRGB = entry.isSRGB;
        }
        textures.push_back(std::move(texture));
    }

    SortAppendedDiagnostics(diagnostics, diagnosticBase);
    return textures;
}

size_t ApplyDielectricDefaultCorrection(
    std::vector<SceneMaterial>& materials,
    size_t firstMaterial,
    const AssetReference& owner,
    std::vector<AssetDiagnostic>& diagnostics)
{
    size_t corrected = 0;
    for (size_t i = firstMaterial; i < materials.size(); ++i)
    {
        SceneMaterial& m = materials[i];

        // Only the exact spec-default shape: fully metallic with nothing to
        // modulate it. A material that authored metallic deliberately, or
        // that ships a metallicRoughness texture, is left alone.
        if (m.metallicRoughnessTextureIndex >= 0) continue;
        if (m.metallic < 0.9f) continue;

        m.metallic = 0.0f;

        AssetDiagnostic d;
        d.severity = AssetDiagnostic::Stale;
        d.kind     = AssetKind::Model;
        d.refPath  = owner.path;
        d.sourceKey = owner.sourceKey;
        d.detail =
            "material " + std::to_string(i) +
            ": no metallicRoughness texture and no authored metallicFactor; "
            "imported as dielectric instead of glTF's default of 1.0";
        diagnostics.push_back(std::move(d));
        ++corrected;
    }
    return corrected;
}

} // namespace rt2::core
