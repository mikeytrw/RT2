#include <doctest/doctest.h>

#include "ECSComponents.h"
#include "AssetIdentity.h"
#include "Phase1AFixtureGenerator.h"
#include "SceneAssetResolver.h"
#include "SceneDocument.h"
#include "SceneLoader.h"
#include "SceneManager.h"
#include "SceneSerializer.h"
#include "SceneSerializerTestSupport.h"
#include "ScriptAssetPath.h"
#include "ScriptFieldRegistry.h"
#include "core/Error.h"
#include "core/UUID.h"

#include "json.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace rt2::core;
namespace fs = std::filesystem;

// ============================================================================
// Phase 7 W3 characterization.
//
// These tests deliberately pin the three pre-W3 resolution paths before any
// production cutover:
//   - SceneAssetResolver for models and environments,
//   - ResolveScriptAssetPath + ScriptFieldRegistry for Lua,
//   - texture loading inside SceneLoader for glTF and OBJ.
//
// Some assertions describe known defects (duplicate/misclassified model
// diagnostics, false transactionality, and dropped script assetId). They are
// labelled as transitional characterization and must be replaced with the W3
// contract assertions as each consumer is cut over.
//
// Every fixture is generated below a unique temporary directory. No test
// depends on C:\Users\...\Downloads or on checked-in generated assets.
// ============================================================================

namespace {

class TempDirectory
{
public:
    TempDirectory()
    {
        static uint64_t sequence = 0;
        const auto ticks = std::chrono::high_resolution_clock::now()
                               .time_since_epoch()
                               .count();
        m_Path = fs::temp_directory_path() /
            ("rt2_w3_characterization_" + std::to_string(ticks) + "_" +
             std::to_string(++sequence));
        std::error_code ec;
        fs::create_directories(m_Path, ec);
        REQUIRE_MESSAGE(!ec, "failed to create temporary fixture directory");
    }

    ~TempDirectory()
    {
        std::error_code ec;
        fs::remove_all(m_Path, ec);
    }

    const fs::path& Path() const { return m_Path; }

private:
    fs::path m_Path;
};

bool WriteText(const fs::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    return out.good();
}

bool WriteBytes(const fs::path& path, const std::vector<unsigned char>& bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.close();
    return out.good();
}

bool WritePpm(const fs::path& path)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out << "P6\n1 1\n255\n";
    const unsigned char pixel[3] = { 128, 64, 32 };
    out.write(reinterpret_cast<const char*>(pixel), sizeof(pixel));
    out.close();
    return out.good();
}

entt::entity AddImportedEntity(SceneDocument& doc,
                               const std::string& path,
                               const std::string& sourceKey)
{
    const entt::entity entity = doc.ecs.registry.create();
    doc.ecs.registry.emplace<NameComponent>(entity, "Imported");
    doc.ecs.registry.emplace<Transform>(entity);
    doc.ecs.registry.emplace<VisibleComponent>(entity);

    ImportedMeshSourceComponent imported;
    imported.model.kind = AssetKind::Model;
    imported.model.path = path;
    imported.model.sourceKey = sourceKey;
    doc.ecs.registry.emplace<ImportedMeshSourceComponent>(
        entity, std::move(imported));
    doc.AssignNewUuid(entity);
    return entity;
}

size_t CountSeverity(const std::vector<AssetDiagnostic>& diagnostics,
                     AssetDiagnostic::Severity severity)
{
    size_t count = 0;
    for (const auto& diagnostic : diagnostics)
        if (diagnostic.severity == severity)
            ++count;
    return count;
}

enum class GltfImageCase
{
    ValidExternal,
    MissingExternal,
    MalformedExternal,
    InvalidTextureSource,
};

fs::path WriteExternalTextureGltf(const fs::path& directory,
                                  GltfImageCase imageCase)
{
    const std::array<float, 9> positions = {
        -1.0f, -1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
         0.0f,  1.0f, 0.0f,
    };
    const std::array<uint16_t, 3> indices = { 0, 1, 2 };

    // Keep the index buffer 4-byte aligned and advertise only the populated
    // bytes. This is a complete, tiny geometry fixture independent of any
    // machine-local model.
    std::vector<unsigned char> geometry(44, 0);
    std::memcpy(geometry.data(), positions.data(), sizeof(positions));
    std::memcpy(geometry.data() + 36, indices.data(), sizeof(indices));
    REQUIRE(WriteBytes(directory / "geometry.bin", geometry));

    std::string imageUri;
    switch (imageCase)
    {
        case GltfImageCase::ValidExternal:
            imageUri = "valid.ppm";
            REQUIRE(WritePpm(directory / imageUri));
            break;
        case GltfImageCase::MissingExternal:
            imageUri = "missing.ppm";
            break;
        case GltfImageCase::MalformedExternal:
            imageUri = "malformed.png";
            REQUIRE(WriteText(directory / imageUri, "not an image"));
            break;
        case GltfImageCase::InvalidTextureSource:
            break;
    }

    nlohmann::json gltf;
    gltf["asset"] = { { "version", "2.0" } };
    gltf["scene"] = 0;
    gltf["scenes"] = nlohmann::json::array({
        { { "nodes", nlohmann::json::array({ 0 }) } }
    });
    gltf["nodes"] = nlohmann::json::array({ { { "mesh", 0 } } });
    gltf["buffers"] = nlohmann::json::array({
        { { "uri", "geometry.bin" }, { "byteLength", geometry.size() } }
    });
    gltf["bufferViews"] = nlohmann::json::array({
        {
            { "buffer", 0 },
            { "byteOffset", 0 },
            { "byteLength", sizeof(positions) },
            { "target", 34962 },
        },
        {
            { "buffer", 0 },
            { "byteOffset", 36 },
            { "byteLength", sizeof(indices) },
            { "target", 34963 },
        },
    });
    gltf["accessors"] = nlohmann::json::array({
        {
            { "bufferView", 0 },
            { "componentType", 5126 },
            { "count", 3 },
            { "type", "VEC3" },
            { "min", nlohmann::json::array({ -1.0, -1.0, 0.0 }) },
            { "max", nlohmann::json::array({ 1.0, 1.0, 0.0 }) },
        },
        {
            { "bufferView", 1 },
            { "componentType", 5123 },
            { "count", 3 },
            { "type", "SCALAR" },
        },
    });
    gltf["meshes"] = nlohmann::json::array({
        {
            { "primitives", nlohmann::json::array({
                {
                    { "attributes", { { "POSITION", 0 } } },
                    { "indices", 1 },
                    { "material", 0 },
                }
            }) }
        }
    });

    if (imageCase == GltfImageCase::InvalidTextureSource)
    {
        gltf["textures"] = nlohmann::json::array({ { { "source", 7 } } });
    }
    else
    {
        gltf["images"] = nlohmann::json::array({ { { "uri", imageUri } } });
        gltf["textures"] = nlohmann::json::array({ { { "source", 0 } } });
    }
    gltf["materials"] = nlohmann::json::array({
        {
            { "pbrMetallicRoughness", {
                { "baseColorTexture", { { "index", 0 } } }
            } }
        }
    });

    const fs::path path = directory / "model.gltf";
    REQUIRE(WriteText(path, gltf.dump(2)));
    return path;
}

fs::path WriteDataUriTextureGltf(const fs::path& directory,
                                 bool malformed)
{
    const fs::path path = WriteExternalTextureGltf(
        directory, GltfImageCase::ValidExternal);
    nlohmann::json gltf;
    {
        std::ifstream input(path);
        REQUIRE(input.good());
        input >> gltf;
    }
    gltf["images"][0]["uri"] =
        std::string("data:application/octet-stream;base64,") +
        (malformed ? "bm90IGFuIGltYWdl"
                   : "UDYKMSAxCjI1NQqAQCA=");
    REQUIRE(WriteText(path, gltf.dump(2)));
    return path;
}

fs::path WriteEmbeddedTextureGlb(const fs::path& directory)
{
    const std::array<float, 9> positions = {
        -1.0f, -1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
         0.0f,  1.0f, 0.0f,
    };
    const std::array<uint16_t, 3> indices = { 0, 1, 2 };
    std::vector<unsigned char> binary(60, 0);
    std::memcpy(binary.data(), positions.data(), sizeof(positions));
    std::memcpy(binary.data() + 36, indices.data(), sizeof(indices));
    const std::string ppmHeader = "P6\n1 1\n255\n";
    std::memcpy(binary.data() + 44, ppmHeader.data(), ppmHeader.size());
    binary[44 + ppmHeader.size() + 0] = 128;
    binary[44 + ppmHeader.size() + 1] = 64;
    binary[44 + ppmHeader.size() + 2] = 32;

    nlohmann::json gltf;
    gltf["asset"] = { { "version", "2.0" } };
    gltf["scene"] = 0;
    gltf["scenes"] = nlohmann::json::array({
        { { "nodes", nlohmann::json::array({ 0 }) } }
    });
    gltf["nodes"] = nlohmann::json::array({ { { "mesh", 0 } } });
    gltf["buffers"] = nlohmann::json::array({
        { { "byteLength", 58 } }
    });
    gltf["bufferViews"] = nlohmann::json::array({
        {
            { "buffer", 0 }, { "byteOffset", 0 },
            { "byteLength", sizeof(positions) }, { "target", 34962 },
        },
        {
            { "buffer", 0 }, { "byteOffset", 36 },
            { "byteLength", sizeof(indices) }, { "target", 34963 },
        },
        {
            { "buffer", 0 }, { "byteOffset", 44 },
            { "byteLength", ppmHeader.size() + 3 },
        },
    });
    gltf["accessors"] = nlohmann::json::array({
        {
            { "bufferView", 0 }, { "componentType", 5126 },
            { "count", 3 }, { "type", "VEC3" },
            { "min", nlohmann::json::array({ -1.0, -1.0, 0.0 }) },
            { "max", nlohmann::json::array({ 1.0, 1.0, 0.0 }) },
        },
        {
            { "bufferView", 1 }, { "componentType", 5123 },
            { "count", 3 }, { "type", "SCALAR" },
        },
    });
    gltf["images"] = nlohmann::json::array({
        { { "bufferView", 2 },
          { "mimeType", "image/x-portable-pixmap" } }
    });
    gltf["textures"] = nlohmann::json::array({ { { "source", 0 } } });
    gltf["materials"] = nlohmann::json::array({
        {
            { "pbrMetallicRoughness", {
                { "baseColorTexture", { { "index", 0 } } }
            } }
        }
    });
    gltf["meshes"] = nlohmann::json::array({
        {
            { "primitives", nlohmann::json::array({
                {
                    { "attributes", { { "POSITION", 0 } } },
                    { "indices", 1 }, { "material", 0 },
                }
            }) }
        }
    });

    std::string jsonText = gltf.dump();
    while ((jsonText.size() % 4) != 0)
        jsonText.push_back(' ');
    while ((binary.size() % 4) != 0)
        binary.push_back(0);

    std::vector<unsigned char> glb;
    auto appendU32 = [&](uint32_t value) {
        glb.push_back(static_cast<unsigned char>(value & 0xff));
        glb.push_back(static_cast<unsigned char>((value >> 8) & 0xff));
        glb.push_back(static_cast<unsigned char>((value >> 16) & 0xff));
        glb.push_back(static_cast<unsigned char>((value >> 24) & 0xff));
    };
    const uint32_t totalLength = static_cast<uint32_t>(
        12 + 8 + jsonText.size() + 8 + binary.size());
    appendU32(0x46546c67);
    appendU32(2);
    appendU32(totalLength);
    appendU32(static_cast<uint32_t>(jsonText.size()));
    appendU32(0x4e4f534a);
    glb.insert(glb.end(), jsonText.begin(), jsonText.end());
    appendU32(static_cast<uint32_t>(binary.size()));
    appendU32(0x004e4942);
    glb.insert(glb.end(), binary.begin(), binary.end());

    const fs::path path = directory / "embedded.glb";
    REQUIRE(WriteBytes(path, glb));
    return path;
}

fs::path WriteTexturedObj(const fs::path& directory,
                          const std::string& textureName)
{
    REQUIRE(WriteText(directory / "material.mtl",
        "newmtl material\n"
        "Kd 1 1 1\n"
        "map_Kd " + textureName + "\n"));
    REQUIRE(WriteText(directory / "model.obj",
        "mtllib material.mtl\n"
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "vt 0 0\n"
        "vt 1 0\n"
        "vt 0 1\n"
        "usemtl material\n"
        "f 1/1 2/2 3/3\n"));
    return directory / "model.obj";
}

fs::path WriteFourRoleObj(const fs::path& directory,
                          const std::array<std::string, 4>& textureNames)
{
    REQUIRE(WriteText(directory / "material.mtl",
        "newmtl material\n"
        "Kd 1 1 1\n"
        "map_Kd " + textureNames[0] + "\n"
        "norm " + textureNames[1] + "\n"
        "map_Ke " + textureNames[2] + "\n"
        "map_Pr " + textureNames[3] + "\n"));
    REQUIRE(WriteText(directory / "model.obj",
        "mtllib material.mtl\n"
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "vt 0 0\n"
        "vt 1 0\n"
        "vt 0 1\n"
        "usemtl material\n"
        "f 1/1 2/2 3/3\n"));
    return directory / "model.obj";
}

TextureAssetLoadContext ReadOnlyTextureContext(const fs::path& modelPath)
{
    TextureAssetLoadContext context;
    context.resolution.assetRoot =
        fs::absolute(modelPath.parent_path()).lexically_normal();
    context.ownerModel.kind = AssetKind::Model;
    context.ownerModel.path = modelPath.filename().generic_string();
    context.resolvedOwnerPath =
        fs::absolute(modelPath).lexically_normal();
    return context;
}

TextureAssetLoadContext ExplicitTextureContext(
    const fs::path& modelPath,
    IUuidProvider& provider)
{
    auto context = ReadOnlyTextureContext(modelPath);
    context.identityMode = TextureIdentityMode::ExplicitImport;
    context.uuidProvider = &provider;
    return context;
}

void CheckExactTexturePlaceholder(const SceneTexture& texture)
{
    const std::vector<unsigned char> expected = {
        0xff, 0x00, 0xff, 0xff,
        0x00, 0x00, 0x00, 0xff,
        0x00, 0x00, 0x00, 0xff,
        0xff, 0x00, 0xff, 0xff,
    };
    CHECK(texture.width == 2);
    CHECK(texture.height == 2);
    CHECK(texture.channels == 4);
    CHECK(texture.pixels == expected);
    CHECK_FALSE(texture.isHDR);
    CHECK(texture.floatPixels.empty());
    CHECK_FALSE(texture.isSRGB);
}

ScriptComponent ScriptAt(const std::string& path)
{
    ScriptComponent script;
    script.asset.kind = AssetKind::Script;
    script.asset.path = path;
    script.asset.sourceKey = "lua:asset=" + path;
    return script;
}

} // namespace

TEST_CASE("Phase7 W3 characterization: model success resolves a generated GLB")
{
    TempDirectory fixture;
    Error fixtureError;
    REQUIRE(GenerateTinyTexturedGlb(
        fixture.Path() / "model.glb", fixtureError));

    DeterministicUuidProvider ids;
    SceneDocument doc;
    doc.SetUuidProvider(&ids);
    const entt::entity entity = AddImportedEntity(
        doc, "model.glb", SceneAssetResolver::GltfSourceKey(0, 0, 0, 0));

    std::vector<AssetDiagnostic> diagnostics;
    Error error;
    CHECK(SceneAssetResolver::ResolveAll(
        doc, fixture.Path(), diagnostics, error));
    CHECK(error.IsOk());
    // W3 step 3: a nil assetId with no sidecar resolves by path fallback and
    // emits exactly one "identity repair required" Stale diagnostic. The
    // locator is read-only; the host saves/migrates the assigned ID later.
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].severity == AssetDiagnostic::Stale);
    CHECK(diagnostics[0].detail.find("identity repair required") !=
          std::string::npos);
    CHECK(doc.ecs.registry.all_of<MeshRef>(entity));
    CHECK(doc.ecs.meshRegistry.GetCount() == 1);
    CHECK(doc.ecs.textures.size() == 1);
}

TEST_CASE("Phase7 W3 characterization: missing model emits one locator diagnostic and one entity diagnostic")
{
    TempDirectory fixture;
    DeterministicUuidProvider ids;
    SceneDocument doc;
    doc.SetUuidProvider(&ids);
    const entt::entity entity = AddImportedEntity(
        doc, "missing.glb", SceneAssetResolver::GltfSourceKey(0, 0, 0, 0));

    std::vector<AssetDiagnostic> diagnostics;
    Error error;
    CHECK_FALSE(SceneAssetResolver::ResolveAll(
        doc, fixture.Path(), diagnostics, error));
    CHECK(error.code == Error::MissingAsset);
    // W3 step 3 (W3-P8): the shared locator emits exactly one terminal
    // diagnostic per missing reference. The pre-W3 duplicate file-level
    // diagnostic is gone. The entity-level "model not loaded" diagnostic
    // still fires in the plan pass, so the total is 2 — but BOTH carry the
    // entity's UUID (the locator fills it from the first referencing
    // entity, not nil as in the pre-W3 file-level diagnostic).
    REQUIRE(diagnostics.size() == 2);
    CHECK(CountSeverity(diagnostics, AssetDiagnostic::Missing) == 2);
    CHECK_FALSE(diagnostics[0].entityUuid.IsNull());
    CHECK_FALSE(diagnostics[1].entityUuid.IsNull());
    // W3-P7 transactionality: hard failure leaves the document unchanged.
    CHECK(doc.ecs.meshRegistry.GetCount() == 0);
    CHECK(doc.ecs.materials.empty());
    CHECK(doc.ecs.textures.empty());
    CHECK_FALSE(doc.ecs.registry.all_of<MeshRef>(entity));
}

TEST_CASE("Phase7 W3 characterization: malformed model emits locator, load, and entity diagnostics")
{
    TempDirectory fixture;
    REQUIRE(WriteText(fixture.Path() / "malformed.glb", "not a GLB"));

    DeterministicUuidProvider ids;
    SceneDocument doc;
    doc.SetUuidProvider(&ids);
    const entt::entity entity = AddImportedEntity(
        doc, "malformed.glb", SceneAssetResolver::GltfSourceKey(0, 0, 0, 0));

    std::vector<AssetDiagnostic> diagnostics;
    Error error;
    CHECK_FALSE(SceneAssetResolver::ResolveAll(
        doc, fixture.Path(), diagnostics, error));
    CHECK(error.code == Error::MissingAsset);
    // W3 step 3: three diagnostics, each a distinct failure layer.
    //   1. Locator: nil ID + absent sidecar -> Stale "identity repair
    //      required" (the path resolves, so the file is not "missing").
    //   2. Loader: SceneLoader::LoadIntoECS fails -> Malformed "model
    //      failed to load".
    //   3. Plan pass: staged model not loaded -> Missing "model not loaded;
    //      entity left without resolved mesh".
    REQUIRE(diagnostics.size() == 3);
    CHECK(CountSeverity(diagnostics, AssetDiagnostic::Malformed) == 1);
    CHECK(CountSeverity(diagnostics, AssetDiagnostic::Stale) == 1);
    CHECK(CountSeverity(diagnostics, AssetDiagnostic::Missing) == 1);
    // W3-P7 transactionality: hard failure leaves the document unchanged.
    CHECK(doc.ecs.meshRegistry.GetCount() == 0);
    CHECK(doc.ecs.materials.empty());
    CHECK(doc.ecs.textures.empty());
    CHECK_FALSE(doc.ecs.registry.all_of<MeshRef>(entity));
}

TEST_CASE("Phase7 W3 characterization: unresolved model source key fails transactionally")
{
    TempDirectory fixture;
    Error fixtureError;
    REQUIRE(GenerateTinyTexturedGlb(
        fixture.Path() / "model.glb", fixtureError));

    DeterministicUuidProvider ids;
    SceneDocument doc;
    doc.SetUuidProvider(&ids);
    const entt::entity entity = AddImportedEntity(
        doc, "model.glb", SceneAssetResolver::GltfSourceKey(0, 0, 0, 99));

    std::vector<AssetDiagnostic> diagnostics;
    Error error;
    CHECK_FALSE(SceneAssetResolver::ResolveAll(
        doc, fixture.Path(), diagnostics, error));
    CHECK(error.code == Error::MissingAsset);
    // W3 step 3: two diagnostics.
    //   1. Locator: nil ID + absent sidecar -> Missing "identity repair
    //      required" (the path resolves, so the file is not "missing").
    //   2. Plan pass: source key not found in rebuilt model -> Unresolved.
    REQUIRE(diagnostics.size() == 2);
    CHECK(diagnostics[0].severity == AssetDiagnostic::Stale);
    CHECK(diagnostics[1].severity == AssetDiagnostic::Unresolved);
    CHECK_FALSE(doc.ecs.registry.all_of<MeshRef>(entity));

    // W3-P7/W3-Q6 transactionality: a false ResolveAll leaves the document
    // unchanged. The pre-W3 false-transactionality defect (resources
    // appended before the all-unresolved check) is fixed: no meshes,
    // materials, or textures are committed when every entity is unresolved.
    CHECK(doc.ecs.meshRegistry.GetCount() == 0);
    CHECK(doc.ecs.materials.empty());
    CHECK(doc.ecs.textures.empty());
}

// ---------------------------------------------------------------------------
// Phase 7 W3 step 3 — model cutover contract.
//
// These tests pin the post-cutover behaviour of SceneAssetResolver::ResolveAll
// for models. They exercise ID-first resolution through the shared locator,
// the removed duplicate file-level diagnostic, and transactionality on hard
// failure. Each fixture is generated; no machine-local assets are used.
// ---------------------------------------------------------------------------

TEST_CASE("Phase7 W3 step 3: non-nil assetId with matching sidecar resolves without diagnostics")
{
    TempDirectory fixture;
    Error fixtureError;
    REQUIRE(GenerateTinyTexturedGlb(
        fixture.Path() / "model.glb", fixtureError));

    // Write a sidecar so the locator's path-sidecar verification matches the
    // requested ID (case 3: database stale/missing but sidecar matches).
    const UUID id = UUID::Parse("11111111-2222-4333-8444-555555555555");
    Error sidecarErr;
    REQUIRE(WriteSidecarId(
        AssetSidecarPath(fixture.Path() / "model.glb"), id, sidecarErr));
    REQUIRE(sidecarErr.IsOk());

    DeterministicUuidProvider ids;
    SceneDocument doc;
    doc.SetUuidProvider(&ids);
    const entt::entity entity = doc.ecs.registry.create();
    doc.ecs.registry.emplace<NameComponent>(entity, "Imported");
    doc.ecs.registry.emplace<Transform>(entity);
    doc.ecs.registry.emplace<VisibleComponent>(entity);
    ImportedMeshSourceComponent imported;
    imported.model.kind = AssetKind::Model;
    imported.model.path = "model.glb";
    imported.model.sourceKey = SceneAssetResolver::GltfSourceKey(0, 0, 0, 0);
    imported.model.assetId = id; // non-nil, matches sidecar
    doc.ecs.registry.emplace<ImportedMeshSourceComponent>(entity, imported);
    doc.AssignNewUuid(entity);

    std::vector<AssetDiagnostic> diagnostics;
    Error error;
    CHECK(SceneAssetResolver::ResolveAll(
        doc, fixture.Path(), diagnostics, error));
    CHECK(error.IsOk());
    // A matching sidecar is fully healthy when no database was supplied.
    CHECK(diagnostics.empty());
    CHECK(doc.ecs.registry.all_of<MeshRef>(entity));
    CHECK(doc.ecs.meshRegistry.GetCount() == 1);
}

TEST_CASE("Phase7 W3 step 3: non-nil assetId with conflicting sidecar fails with Conflict")
{
    TempDirectory fixture;
    Error fixtureError;
    REQUIRE(GenerateTinyTexturedGlb(
        fixture.Path() / "model.glb", fixtureError));

    // Sidecar claims a DIFFERENT id than the reference carries.
    const UUID sidecarId = UUID::Parse("11111111-2222-4333-8444-555555555555");
    const UUID refId     = UUID::Parse("99999999-8888-4777-8666-555555555555");
    Error sidecarErr;
    REQUIRE(WriteSidecarId(
        AssetSidecarPath(fixture.Path() / "model.glb"), sidecarId, sidecarErr));

    DeterministicUuidProvider ids;
    SceneDocument doc;
    doc.SetUuidProvider(&ids);
    const entt::entity entity = doc.ecs.registry.create();
    doc.ecs.registry.emplace<NameComponent>(entity, "Imported");
    doc.ecs.registry.emplace<Transform>(entity);
    doc.ecs.registry.emplace<VisibleComponent>(entity);
    ImportedMeshSourceComponent imported;
    imported.model.kind = AssetKind::Model;
    imported.model.path = "model.glb";
    imported.model.sourceKey = SceneAssetResolver::GltfSourceKey(0, 0, 0, 0);
    imported.model.assetId = refId;
    doc.ecs.registry.emplace<ImportedMeshSourceComponent>(entity, imported);
    doc.AssignNewUuid(entity);

    std::vector<AssetDiagnostic> diagnostics;
    Error error;
    CHECK_FALSE(SceneAssetResolver::ResolveAll(
        doc, fixture.Path(), diagnostics, error));
    // The locator's Conflict diagnostic makes ResolveAll hard-fail (every
    // entity failed because the staged model was never loaded — the locator
    // refused to resolve the path). Transactionality: doc unchanged.
    CHECK_FALSE(doc.ecs.registry.all_of<MeshRef>(entity));
    CHECK(doc.ecs.meshRegistry.GetCount() == 0);
    CHECK(doc.ecs.materials.empty());
    CHECK(doc.ecs.textures.empty());
    bool sawConflict = false;
    for (const auto& d : diagnostics)
        if (d.severity == AssetDiagnostic::Conflict) sawConflict = true;
    CHECK(sawConflict);
}

TEST_CASE("Phase7 W3 step 3: partial success commits accepted resources and returns true")
{
    TempDirectory fixture;
    Error fixtureError;
    REQUIRE(GenerateTinyTexturedGlb(
        fixture.Path() / "model.glb", fixtureError));

    DeterministicUuidProvider ids;
    SceneDocument doc;
    doc.SetUuidProvider(&ids);

    // Entity A: resolvable (valid source key).
    const entt::entity a = AddImportedEntity(
        doc, "model.glb", SceneAssetResolver::GltfSourceKey(0, 0, 0, 0));
    // Entity B: unresolvable (bad primitive index). The model loads once;
    // B's source key miss produces an Unresolved diagnostic, but A still
    // resolves, so ResolveAll returns true and commits A's resources.
    const entt::entity b = AddImportedEntity(
        doc, "model.glb", SceneAssetResolver::GltfSourceKey(0, 0, 0, 99));

    std::vector<AssetDiagnostic> diagnostics;
    Error error;
    CHECK(SceneAssetResolver::ResolveAll(
        doc, fixture.Path(), diagnostics, error));
    CHECK(error.IsOk());
    // A has a MeshRef; B does not.
    CHECK(doc.ecs.registry.all_of<MeshRef>(a));
    CHECK_FALSE(doc.ecs.registry.all_of<MeshRef>(b));
    // Partial success committed the model's resources.
    CHECK(doc.ecs.meshRegistry.GetCount() == 1);
    // B's Unresolved diagnostic is present alongside A's locator diagnostic.
    bool sawUnresolved = false;
    for (const auto& d : diagnostics)
        if (d.severity == AssetDiagnostic::Unresolved) sawUnresolved = true;
    CHECK(sawUnresolved);
}

TEST_CASE("Phase7 W3 characterization: environment success, missing, and malformed disagree on detail")
{
    SUBCASE("success decodes a generated EXR")
    {
        TempDirectory fixture;
        Error fixtureError;
        REQUIRE(GenerateTinyExrEnv(fixture.Path() / "environment.exr",
                                   fixtureError));

        SceneDocument doc;
        doc.environment.ref.path = "environment.exr";
        std::vector<AssetDiagnostic> diagnostics;
        Error error;
        CHECK(SceneAssetResolver::ResolveEnvironment(
            doc, fixture.Path(), diagnostics, error));
        CHECK(error.IsOk());
        // W3 step 4: nil env assetId + absent sidecar -> locator resolves by
        // path fallback and emits one "identity repair required" Stale
        // diagnostic. The host's next save/migration persists the assigned ID.
        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0].severity == AssetDiagnostic::Stale);
        CHECK(diagnostics[0].kind == AssetKind::Environment);
        CHECK(diagnostics[0].detail.find("identity repair required") !=
              std::string::npos);
        CHECK(doc.environment.width == 4);
        CHECK(doc.environment.height == 2);
        CHECK_FALSE(doc.environment.floatPixels.empty());
    }

    SUBCASE("missing is Missing and clears stale decoded data")
    {
        TempDirectory fixture;
        SceneDocument doc;
        doc.environment.ref.path = "missing.exr";
        doc.environment.width = 8;
        doc.environment.height = 4;
        doc.environment.floatPixels.assign(8 * 4 * 4, 1.0f);

        std::vector<AssetDiagnostic> diagnostics;
        Error error;
        CHECK_FALSE(SceneAssetResolver::ResolveEnvironment(
            doc, fixture.Path(), diagnostics, error));
        CHECK(error.code == Error::MissingAsset);
        // W3 step 4: the locator emits exactly one terminal diagnostic for
        // the missing path. The pre-W3 duplicate file-level diagnostic is
        // gone (same fix as the model cutover).
        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0].severity == AssetDiagnostic::Missing);
        CHECK(diagnostics[0].kind == AssetKind::Environment);
        CHECK(doc.environment.ref.path == "missing.exr");
        CHECK(doc.environment.width == 0);
        CHECK(doc.environment.height == 0);
        CHECK(doc.environment.floatPixels.empty());
    }

    SUBCASE("malformed is diagnosed Malformed but still returns MissingAsset")
    {
        TempDirectory fixture;
        REQUIRE(WriteText(fixture.Path() / "malformed.exr",
                          "not an EXR image"));

        SceneDocument doc;
        doc.environment.ref.path = "malformed.exr";
        std::vector<AssetDiagnostic> diagnostics;
        Error error;
        CHECK_FALSE(SceneAssetResolver::ResolveEnvironment(
            doc, fixture.Path(), diagnostics, error));
        CHECK(error.code == Error::MissingAsset);
        // W3 step 4: two diagnostics. The locator resolves the path (the
        // malformed file exists) and emits a "identity repair required"
        // Stale diagnostic; the EXR decoder then fails and emits a
        // Malformed diagnostic.
        REQUIRE(diagnostics.size() == 2);
        CHECK(CountSeverity(diagnostics, AssetDiagnostic::Stale) == 1);
        CHECK(CountSeverity(diagnostics, AssetDiagnostic::Malformed) == 1);
        for (const auto& d : diagnostics)
            CHECK(d.kind == AssetKind::Environment);
        // The Malformed diagnostic carries the resolved path.
        bool sawMalformedWithResolvedPath = false;
        for (const auto& d : diagnostics)
            if (d.severity == AssetDiagnostic::Malformed &&
                !d.resolvedPath.empty())
                sawMalformedWithResolvedPath = true;
        CHECK(sawMalformedWithResolvedPath);
    }
}

// ---------------------------------------------------------------------------
// Phase 7 W3 step 4 — environment cutover contract.
//
// These tests pin the post-cutover behaviour of
// SceneAssetResolver::ResolveEnvironment. They cover moved assets (stale
// path but matching sidecar ID), nil-ID fallback, missing sidecar, missing
// file, and corrupt HDR/EXR — the cases the step-4 plan calls out.
// ---------------------------------------------------------------------------

TEST_CASE("Phase7 W3 step 4: non-nil env assetId with matching sidecar resolves without diagnostics")
{
    TempDirectory fixture;
    Error fixtureError;
    REQUIRE(GenerateTinyExrEnv(fixture.Path() / "environment.exr", fixtureError));

    // Write a sidecar matching the env reference's assetId. The path is also
    // valid, so resolution succeeds via path fallback with the sidecar
    // confirming the ID (case 3: database stale/missing but sidecar matches).
    const UUID id = UUID::Parse("22222222-3333-4444-8555-666666666666");
    Error sidecarErr;
    REQUIRE(WriteSidecarId(
        AssetSidecarPath(fixture.Path() / "environment.exr"), id, sidecarErr));
    REQUIRE(sidecarErr.IsOk());

    SceneDocument doc;
    doc.environment.ref.path = "environment.exr";
    doc.environment.ref.assetId = id;
    std::vector<AssetDiagnostic> diagnostics;
    Error error;
    CHECK(SceneAssetResolver::ResolveEnvironment(
        doc, fixture.Path(), diagnostics, error));
    CHECK(error.IsOk());
    // A matching sidecar is fully healthy when no database was supplied.
    CHECK(diagnostics.empty());
    CHECK(doc.environment.width > 0);
    CHECK(doc.environment.height > 0);
    CHECK_FALSE(doc.environment.floatPixels.empty());
    // The locator's effective ID is cached back into the document.
    CHECK(doc.environment.ref.assetId == id);
}

TEST_CASE("Phase7 W3 step 4: moved env asset (stale path) without database fails and preserves the ID")
{
    // Discriminating moved-asset test (item 5). The previous version of this
    // test expected failure against a stale path with no database, but the
    // asset and sidecar created under new/ were never consulted — a legacy
    // path-only resolver that just tried the stale path and failed would
    // satisfy it, so the test did not discriminate the W3 ID-aware path from
    // the pre-W3 path-only path.
    //
    // New setup: the stale path `old/env.exr` EXISTS (so a path-only resolver
    // would load it successfully — the wrong file), but its sidecar claims a
    // DIFFERENT ID than the scene's reference. The moved file `new/env.exr`
    // carries the matching sidecar. The W3 locator must read the sidecar at
    // the resolved path and refuse to substitute identity: sidecar ID != ref
    // ID -> Conflict (case 5), even with no database. A path-only resolver
    // would silently load `old/env.exr` and succeed. This pins that moved
    // assets need the W4 database to resolve by ID; the sidecar at the new
    // path is not enough when the reference path is stale and no database is
    // available.
    TempDirectory fixture;
    Error fixtureError;
    std::error_code mkdirEc;
    fs::create_directories(fixture.Path() / "old", mkdirEc);
    fs::create_directories(fixture.Path() / "new", mkdirEc);
    REQUIRE(!mkdirEc);
    REQUIRE(GenerateTinyExrEnv(fixture.Path() / "old/env.exr", fixtureError));
    REQUIRE(GenerateTinyExrEnv(fixture.Path() / "new/env.exr", fixtureError));

    const UUID requestedId = UUID::Parse("33333333-4444-4555-8666-777777777777");
    const UUID stalePathId  = UUID::Parse("AAAAAAAA-BBBB-4CCC-8DDD-EEEEEEEEEEEE");
    // The moved file's sidecar matches the requested ID.
    Error sidecarErrNew;
    REQUIRE(WriteSidecarId(
        AssetSidecarPath(fixture.Path() / "new/env.exr"),
        requestedId, sidecarErrNew));
    // The stale path's sidecar claims a DIFFERENT ID — so the locator must
    // refuse to substitute identity (Conflict), not silently load old/env.exr.
    Error sidecarErrOld;
    REQUIRE(WriteSidecarId(
        AssetSidecarPath(fixture.Path() / "old/env.exr"),
        stalePathId, sidecarErrOld));

    SceneDocument doc;
    doc.environment.ref.path = "old/env.exr"; // exists, but its sidecar disagrees
    doc.environment.ref.assetId = requestedId;
    std::vector<AssetDiagnostic> diagnostics;
    Error error;
    CHECK_FALSE(SceneAssetResolver::ResolveEnvironment(
        doc, fixture.Path(), diagnostics, error));
    CHECK(error.code == Error::MissingAsset);
    REQUIRE(diagnostics.size() == 1);
    // W3 case 5: the sidecar at the resolved path claims a different ID —
    // Conflict, never silently substitute. (A path-only resolver would have
    // succeeded here, so this assertion is what makes the test discriminate.)
    CHECK(diagnostics[0].severity == AssetDiagnostic::Conflict);
    CHECK(doc.environment.width == 0);
    CHECK(doc.environment.height == 0);
    CHECK(doc.environment.floatPixels.empty());
    // The durable assetId is preserved so a later database-backed resolve can
    // reattach by identity.
    CHECK(doc.environment.ref.assetId == requestedId);
}

TEST_CASE("Phase7 W3 step 4: non-nil env assetId with conflicting sidecar fails with Conflict")
{
    TempDirectory fixture;
    Error fixtureError;
    REQUIRE(GenerateTinyExrEnv(fixture.Path() / "environment.exr", fixtureError));

    const UUID sidecarId = UUID::Parse("44444444-5555-4666-8777-888888888888");
    const UUID refId     = UUID::Parse("99999999-8888-4777-8666-555555555555");
    Error sidecarErr;
    REQUIRE(WriteSidecarId(
        AssetSidecarPath(fixture.Path() / "environment.exr"), sidecarId, sidecarErr));

    SceneDocument doc;
    doc.environment.ref.path = "environment.exr";
    doc.environment.ref.assetId = refId;
    std::vector<AssetDiagnostic> diagnostics;
    Error error;
    CHECK_FALSE(SceneAssetResolver::ResolveEnvironment(
        doc, fixture.Path(), diagnostics, error));
    CHECK(error.code == Error::MissingAsset);
    bool sawConflict = false;
    for (const auto& d : diagnostics)
        if (d.severity == AssetDiagnostic::Conflict) sawConflict = true;
    CHECK(sawConflict);
    // No pixels decoded; path reference preserved.
    CHECK(doc.environment.floatPixels.empty());
    CHECK(doc.environment.ref.path == "environment.exr");
    // Requested assetId preserved (the locator never substitutes identity).
    CHECK(doc.environment.ref.assetId == refId);
}

TEST_CASE("Phase7 W3 step 4: nil env assetId with sidecar caches the effective ID")
{
    TempDirectory fixture;
    Error fixtureError;
    REQUIRE(GenerateTinyExrEnv(fixture.Path() / "environment.exr", fixtureError));

    const UUID sidecarId = UUID::Parse("55555555-6666-4777-8888-999999999999");
    Error sidecarErr;
    REQUIRE(WriteSidecarId(
        AssetSidecarPath(fixture.Path() / "environment.exr"), sidecarId, sidecarErr));

    SceneDocument doc;
    doc.environment.ref.path = "environment.exr";
    // assetId is nil; the sidecar supplies the effective ID.
    std::vector<AssetDiagnostic> diagnostics;
    Error error;
    CHECK(SceneAssetResolver::ResolveEnvironment(
        doc, fixture.Path(), diagnostics, error));
    CHECK(error.IsOk());
    // No diagnostic on the sidecar-supplied path fallback (case 8b).
    CHECK(diagnostics.empty());
    CHECK(doc.environment.ref.assetId == sidecarId);
    CHECK(doc.environment.width > 0);
}

TEST_CASE("Phase7 W3 step 4: env assetId survives a save/load round-trip")
{
    TempDirectory fixture;
    Error fixtureError;
    REQUIRE(GenerateTinyExrEnv(fixture.Path() / "environment.exr", fixtureError));
    const UUID id = UUID::Parse("66666666-7777-4888-9999-AAAAAAAAAAAA");
    // Write a sidecar so the env resolves successfully.
    Error sidecarErr;
    REQUIRE(WriteSidecarId(
        AssetSidecarPath(fixture.Path() / "environment.exr"), id, sidecarErr));

    // Build a source doc with the env assetId set.
    DeterministicUuidProvider provider;
    SceneDocument src;
    src.SetUuidProvider(&provider);
    src.environment.ref.path = "environment.exr";
    src.environment.ref.assetId = id;
    src.environment.width = 4;
    src.environment.height = 2;

    const auto scenePath = fixture.Path() / "env.rt2scene";
    Error saveErr;
    REQUIRE(SaveSceneForTest(src, scenePath, saveErr));
    REQUIRE(saveErr.IsOk());

    // The saved file must contain the env assetId (additive over v3).
    {
        std::ifstream in(scenePath, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
        in.close();
        CHECK(content.find(id.ToString()) != std::string::npos);
        CHECK(content.find("\"assetId\"") != std::string::npos);
    }

    // Load and verify the assetId round-trips.
    DeterministicUuidProvider provider2;
    SceneDocument loaded;
    loaded.SetUuidProvider(&provider2);
    Error loadErr;
    REQUIRE(SceneSerializer::Load(loaded, scenePath, loadErr));
    REQUIRE(loadErr.IsOk());
    CHECK(loaded.environment.ref.assetId == id);
    CHECK(loaded.environment.ref.path == "environment.exr");

    // ---- Item 5: nil-ID omission + absent-on-read semantics ----
    // A nil assetId MUST be omitted from the env block (additive over v3:
    // a v3 reader/scene sees no field). The shared AssetReferenceToJson
    // codec writes assetId only when non-nil; this asserts the env path
    // honours that contract — a regression that always writes the field
    // (e.g. a duplicated hand-rolled writer) would fail here.
    SceneDocument srcNil;
    srcNil.SetUuidProvider(&provider);
    srcNil.environment.ref.kind = AssetKind::Environment;
    srcNil.environment.ref.path = "environment.exr";
    srcNil.environment.ref.assetId = UUID::Nil(); // nil -> must be omitted
    srcNil.environment.width = 4;
    srcNil.environment.height = 2;
    const auto nilScenePath = fixture.Path() / "env_nil.rt2scene";
    Error nilSaveErr;
    REQUIRE(SaveSceneForTest(srcNil, nilScenePath, nilSaveErr));
    REQUIRE(nilSaveErr.IsOk());
    {
        nlohmann::json nilSaved;
        std::ifstream in(nilScenePath, std::ios::binary);
        in >> nilSaved;
        CHECK_FALSE(nilSaved["envMap"].contains("assetId"));
    }

    // Absent-on-read: a scene whose env block has NO assetId field (the v3
    // shape) loads to a nil assetId. This is the additive-read contract; a
    // regression that treats absent as an error (item 3 hardens the
    // present-but-malformed case, NOT the absent case) would fail here.
    SceneDocument loadedNil;
    DeterministicUuidProvider provider3;
    loadedNil.SetUuidProvider(&provider3);
    Error nilLoadErr;
    REQUIRE(SceneSerializer::Load(loadedNil, nilScenePath, nilLoadErr));
    REQUIRE(nilLoadErr.IsOk());
    CHECK(loadedNil.environment.ref.assetId.IsNull());

    fs::remove(nilScenePath);
}

TEST_CASE("Phase7 W3 step 5: script adapter uses structured locator and validates metadata")
{
    TempDirectory fixture;
    SceneDocument doc;
    doc.metadata.sourcePath = fixture.Path() / "scene.rt2scene";
    const AssetResolutionContext context{fixture.Path(), nullptr};
    const UUID entityUuid =
        UUID::Parse("550e8400-e29b-41d4-a716-446655440000");

    SUBCASE("success resolves relative to the scene and parses declarations")
    {
        REQUIRE(WriteText(fixture.Path() / "script.lua",
            "rt2.fields = { speed = rt2.field.float(1.0) }\n"));
        const ScriptComponent script = ScriptAt("script.lua");
        std::vector<AssetDiagnostic> diagnostics;
        const auto resolved = ResolveScriptAssetPath(
            script, context, entityUuid, "Scripted", diagnostics);
        REQUIRE(resolved.success);
        CHECK(resolved.resolvedPath == fixture.Path() / "script.lua");
        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0].severity == AssetDiagnostic::Stale);
        CHECK(diagnostics[0].entityUuid == entityUuid);
        CHECK(diagnostics[0].detail.find("identity repair required") !=
              std::string::npos);

        ScriptFieldRegistry registry;
        const auto fields =
            registry.GetDeclaredFields(resolved.resolvedPath);
        CHECK(fields.parsed);
        CHECK(fields.descriptors.size() == 1);
    }

    SUBCASE("missing fails with a structured terminal diagnostic")
    {
        const ScriptComponent script = ScriptAt("missing.lua");
        std::vector<AssetDiagnostic> diagnostics;
        const auto resolved = ResolveScriptAssetPath(
            script, context, entityUuid, "Scripted", diagnostics);
        CHECK_FALSE(resolved.success);
        CHECK(resolved.resolvedPath.empty());
        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0].severity == AssetDiagnostic::Missing);
        CHECK(diagnostics[0].resolvedPath ==
              (fixture.Path() / "missing.lua").string());
    }

    SUBCASE("malformed source locates successfully and parsing fails separately")
    {
        REQUIRE(WriteText(fixture.Path() / "malformed.lua",
                          "rt2.fields = {\n"));
        const ScriptComponent script = ScriptAt("malformed.lua");
        std::vector<AssetDiagnostic> diagnostics;
        const auto resolved = ResolveScriptAssetPath(
            script, context, entityUuid, "Scripted", diagnostics);
        REQUIRE(resolved.success);
        CHECK(resolved.resolvedPath == fixture.Path() / "malformed.lua");

        ScriptFieldRegistry registry;
        const auto fields =
            registry.GetDeclaredFields(resolved.resolvedPath);
        CHECK_FALSE(fields.parsed);
        CHECK_FALSE(fields.diagnostic.empty());
    }

    SUBCASE("a stale sourceKey fails as unresolved")
    {
        REQUIRE(WriteText(fixture.Path() / "actual.lua", "\n"));
        ScriptComponent script = ScriptAt("actual.lua");
        script.asset.sourceKey = "lua:asset=other.lua";

        std::vector<AssetDiagnostic> diagnostics;
        const auto resolved = ResolveScriptAssetPath(
            script, context, entityUuid, "Scripted", diagnostics);
        CHECK_FALSE(resolved.success);
        CHECK(resolved.resolvedPath.empty());
        REQUIRE(diagnostics.size() == 2);
        CHECK(diagnostics.back().severity == AssetDiagnostic::Unresolved);
        CHECK(diagnostics.back().resolvedPath ==
              (fixture.Path() / "actual.lua").string());
    }

    SUBCASE("a conflicting sidecar rejects authoritative script identity")
    {
        const fs::path scriptPath = fixture.Path() / "identified.lua";
        REQUIRE(WriteText(scriptPath, "\n"));
        const UUID requested =
            UUID::Parse("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
        const UUID conflicting =
            UUID::Parse("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
        Error sidecarError;
        REQUIRE(WriteSidecarId(
            AssetSidecarPath(scriptPath), conflicting, sidecarError));

        ScriptComponent script = ScriptAt("identified.lua");
        script.asset.assetId = requested;
        std::vector<AssetDiagnostic> diagnostics;
        const auto resolved = ResolveScriptAssetPath(
            script, context, entityUuid, "Scripted", diagnostics);
        CHECK_FALSE(resolved.success);
        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0].severity == AssetDiagnostic::Conflict);
    }
}

TEST_CASE("Phase7 W3 step 5: script assetId survives serialization through the shared codec")
{
    TempDirectory fixture;
    const fs::path scenePath = fixture.Path() / "scene.rt2scene";

    DeterministicUuidProvider ids;
    SceneDocument authored;
    authored.SetUuidProvider(&ids);
    authored.metadata.sourcePath = scenePath;

    const entt::entity entity = authored.ecs.registry.create();
    authored.ecs.registry.emplace<NameComponent>(entity, "Scripted");
    authored.ecs.registry.emplace<Transform>(entity);
    authored.ecs.registry.emplace<VisibleComponent>(entity);
    ScriptComponent script = ScriptAt("script.lua");
    script.asset.assetId =
        UUID::Parse("550e8400-e29b-41d4-a716-446655440000");
    REQUIRE_FALSE(script.asset.assetId.IsNull());
    authored.ecs.registry.emplace<ScriptComponent>(entity, script);
    authored.AssignNewUuid(entity);

    Error saveError;
    REQUIRE(SaveSceneForTest(authored, scenePath, saveError));

    SceneDocument loaded;
    DeterministicUuidProvider loadedIds;
    loaded.SetUuidProvider(&loadedIds);
    Error loadError;
    REQUIRE(SceneSerializer::Load(loaded, scenePath, loadError));

    const auto view = loaded.ecs.registry.view<ScriptComponent>();
    REQUIRE(view.size() == 1);
    CHECK(view.get<ScriptComponent>(*view.begin()).asset.assetId ==
          script.asset.assetId);

    nlohmann::json saved;
    {
        std::ifstream input(scenePath);
        input >> saved;
    }
    CHECK(saved["entities"][0]["script"]["asset"]["assetId"] ==
          script.asset.assetId.ToString());
}

TEST_CASE("Phase7 W3 step 5: binding an existing script assigns and reuses its sidecar ID")
{
    TempDirectory fixture;
    const fs::path scriptPath = fixture.Path() / "bound.lua";
    REQUIRE(WriteText(scriptPath, "\n"));

    DeterministicUuidProvider ids;
    SceneManager manager;
    manager.SetUuidProvider(&ids);
    manager.AuthoringDoc().metadata.sourcePath =
        fixture.Path() / "scene.rt2scene";
    const UUID entity =
        manager.CreateEmpty("Scripted").affectedEntities.front();

    REQUIRE(manager.SetScriptState(
        entity, ScriptAt("bound.lua")).success);
    const auto first = manager.GetScriptState(entity);
    REQUIRE(first.has_value());
    CHECK_FALSE(first->asset.assetId.IsNull());
    CHECK(fs::is_regular_file(AssetSidecarPath(scriptPath)));

    Error sidecarError;
    const UUID sidecarId =
        ReadSidecarId(AssetSidecarPath(scriptPath), sidecarError);
    REQUIRE(sidecarError.IsOk());
    CHECK(sidecarId == first->asset.assetId);

    ScriptComponent edited = *first;
    edited.fieldValues["speed"] = {
        ScriptFieldType::Float, ScriptFieldValue{2.0}};
    REQUIRE(manager.SetScriptState(entity, edited).success);
    const auto second = manager.GetScriptState(entity);
    REQUIRE(second.has_value());
    CHECK(second->asset.assetId == first->asset.assetId);
}

TEST_CASE("Phase7 W3 step 7.3: script binding respects explicit ownership roots")
{
    SUBCASE("a missing script remains a legal nil-ID authored binding")
    {
        TempDirectory fixture;
        DeterministicUuidProvider ids;
        SceneManager manager;
        manager.SetUuidProvider(&ids);
        manager.AuthoringDoc().metadata.sourcePath =
            fixture.Path() / "scene.rt2scene";
        const UUID entity =
            manager.CreateEmpty("MissingScript").affectedEntities.front();

        REQUIRE(manager.SetScriptState(
            entity, ScriptAt("missing.lua")).success);
        const auto stored = manager.GetScriptState(entity);
        REQUIRE(stored.has_value());
        CHECK(stored->asset.path == "missing.lua");
        CHECK(stored->asset.assetId.IsNull());
        std::error_code ec;
        CHECK_FALSE(fs::exists(
            AssetSidecarPath(fixture.Path() / "missing.lua"), ec));
    }

    SUBCASE("an untitled relative binding does not claim a CWD file")
    {
        const fs::path cwdScript = "rt2_w3_untitled_binding.lua";
        REQUIRE(WriteText(cwdScript, "\n"));
        struct CwdScriptGuard {
            fs::path path;
            ~CwdScriptGuard()
            {
                std::error_code ec;
                fs::remove(path, ec);
                fs::remove(AssetSidecarPath(path), ec);
            }
        } guard{ cwdScript };

        DeterministicUuidProvider ids;
        SceneManager manager;
        manager.SetUuidProvider(&ids);
        const UUID entity =
            manager.CreateEmpty("UntitledScript").affectedEntities.front();

        REQUIRE(manager.SetScriptState(
            entity, ScriptAt(cwdScript.generic_string())).success);
        const auto stored = manager.GetScriptState(entity);
        REQUIRE(stored.has_value());
        CHECK(stored->asset.assetId.IsNull());
        std::error_code ec;
        CHECK_FALSE(fs::exists(AssetSidecarPath(cwdScript), ec));
    }

    SUBCASE("a conflicting sidecar is not silently remapped")
    {
        TempDirectory fixture;
        const fs::path scriptPath = fixture.Path() / "conflict.lua";
        REQUIRE(WriteText(scriptPath, "\n"));

        DeterministicUuidProvider ids;
        SceneManager manager;
        manager.SetUuidProvider(&ids);
        manager.AuthoringDoc().metadata.sourcePath =
            fixture.Path() / "scene.rt2scene";
        const UUID entity =
            manager.CreateEmpty("ConflictScript").affectedEntities.front();
        REQUIRE(manager.SetScriptState(
            entity, ScriptAt("conflict.lua")).success);
        const auto original = manager.GetScriptState(entity);
        REQUIRE(original.has_value());
        REQUIRE_FALSE(original->asset.assetId.IsNull());

        const UUID conflictingId =
            UUID::Parse("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
        REQUIRE(conflictingId != original->asset.assetId);
        Error sidecarError;
        REQUIRE(WriteSidecarId(
            AssetSidecarPath(scriptPath), conflictingId, sidecarError));
        ScriptComponent binding = *original;
        binding.fieldValues["speed"] = {
            ScriptFieldType::Float, ScriptFieldValue{2.0}};

        CHECK_FALSE(manager.SetScriptState(entity, binding).success);
        REQUIRE(manager.GetScriptState(entity).has_value());
        CHECK(manager.GetScriptState(entity)->asset.assetId ==
              original->asset.assetId);
        CHECK(ReadSidecarId(
            AssetSidecarPath(scriptPath), sidecarError) == conflictingId);
    }
}

TEST_CASE("Phase7 W3 step 6.1: committed script scenario identity resolves cleanly")
{
    const fs::path assetRoot =
        fs::absolute("RT2App/assets").lexically_normal();
    const fs::path scriptPath = assetRoot / "script-scenario.lua";
    const fs::path sidecarPath = AssetSidecarPath(scriptPath);
    REQUIRE(fs::is_regular_file(scriptPath));
    REQUIRE(fs::is_regular_file(sidecarPath));

    Error sidecarError;
    const UUID sidecarId = ReadSidecarId(sidecarPath, sidecarError);
    REQUIRE(sidecarError.IsOk());
    REQUIRE_FALSE(sidecarId.IsNull());

    ScriptComponent script = ScriptAt("script-scenario.lua");
    REQUIRE(script.asset.assetId.IsNull());
    const AssetResolutionContext context{assetRoot, nullptr};
    std::vector<AssetDiagnostic> diagnostics;
    const auto resolved = ResolveScriptAssetPath(
        script, context, UUID::Nil(), "ScriptedCube", diagnostics);

    REQUIRE(resolved.success);
    CHECK(resolved.resolvedPath == scriptPath);
    CHECK(resolved.effectiveId == sidecarId);
    CHECK_FALSE(resolved.identityRepairRequired);
    CHECK(diagnostics.empty());
}

TEST_CASE("Phase7 W3 step 6.4: OBJ texture failures are contained")
{
    SUBCASE("success attaches the decoded texture")
    {
        TempDirectory fixture;
        REQUIRE(WritePpm(fixture.Path() / "valid.ppm"));
        const fs::path obj = WriteTexturedObj(fixture.Path(), "valid.ppm");

        ECSScene scene;
        std::vector<AssetDiagnostic> diagnostics;
        REQUIRE(SceneLoader::LoadObjIntoECS(
            scene, ReadOnlyTextureContext(obj), diagnostics));
        REQUIRE(scene.materials.size() == 1);
        CHECK(scene.meshRegistry.GetCount() == 1);
        CHECK(scene.textures.size() == 1);
        CHECK(scene.materials[0].baseColorTextureIndex == 0);
        CHECK(scene.textures[0].ref.kind == AssetKind::Texture);
        CHECK(scene.textures[0].ref.path == "valid.ppm");
        CHECK(scene.textures[0].ref.sourceKey ==
              "obj:material=0:texture=diffuse");
        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0].severity == AssetDiagnostic::Stale);
    }

    SUBCASE("missing texture installs a placeholder")
    {
        TempDirectory fixture;
        const fs::path obj = WriteTexturedObj(fixture.Path(), "missing.ppm");

        ECSScene scene;
        std::vector<AssetDiagnostic> diagnostics;
        REQUIRE(SceneLoader::LoadObjIntoECS(
            scene, ReadOnlyTextureContext(obj), diagnostics));
        REQUIRE(scene.materials.size() == 1);
        CHECK(scene.meshRegistry.GetCount() == 1);
        REQUIRE(scene.textures.size() == 1);
        CheckExactTexturePlaceholder(scene.textures[0]);
        CHECK(scene.materials[0].baseColorTextureIndex == 0);
        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0].severity == AssetDiagnostic::Missing);
        CHECK(diagnostics[0].kind == AssetKind::Texture);
        CHECK(diagnostics[0].sourceKey ==
              "obj:material=0:texture=diffuse");
    }

    SUBCASE("malformed texture installs a placeholder")
    {
        TempDirectory fixture;
        REQUIRE(WriteText(fixture.Path() / "malformed.png", "not an image"));
        const fs::path obj =
            WriteTexturedObj(fixture.Path(), "malformed.png");

        ECSScene scene;
        std::vector<AssetDiagnostic> diagnostics;
        REQUIRE(SceneLoader::LoadObjIntoECS(
            scene, ReadOnlyTextureContext(obj), diagnostics));
        REQUIRE(scene.materials.size() == 1);
        CHECK(scene.meshRegistry.GetCount() == 1);
        REQUIRE(scene.textures.size() == 1);
        CheckExactTexturePlaceholder(scene.textures[0]);
        CHECK(scene.materials[0].baseColorTextureIndex == 0);
        REQUIRE(diagnostics.size() == 2);
        CHECK(diagnostics[0].severity == AssetDiagnostic::Stale);
        CHECK(diagnostics[1].severity == AssetDiagnostic::Malformed);
        CHECK(diagnostics[0].sourceKey ==
              "obj:material=0:texture=diffuse");
        CHECK(diagnostics[1].sourceKey ==
              "obj:material=0:texture=diffuse");
    }
}

TEST_CASE("Phase7 W3 step 6.4: explicit OBJ import assigns and reuses texture identity")
{
    TempDirectory fixture;
    REQUIRE(WritePpm(fixture.Path() / "valid.ppm"));
    const fs::path obj =
        WriteTexturedObj(fixture.Path(), "valid.ppm");
    DeterministicUuidProvider ids;
    auto context = ExplicitTextureContext(obj, ids);
    ImportSettings settings;

    ECSScene scene;
    std::vector<AssetDiagnostic> diagnostics;
    const entt::entity first = SceneLoader::ImportObjIntoECS(
        scene, settings, context, diagnostics);
    REQUIRE(first != entt::entity{entt::null});
    REQUIRE(scene.textures.size() == 1);
    REQUIRE(diagnostics.empty());
    CHECK_FALSE(scene.textures[0].ref.assetId.IsNull());
    CHECK(scene.textures[0].ref.sourceKey ==
          "obj:material=0:texture=diffuse");

    const fs::path sidecar =
        AssetSidecarPath(fixture.Path() / "valid.ppm");
    REQUIRE(fs::is_regular_file(sidecar));
    Error sidecarError;
    const UUID textureId = ReadSidecarId(sidecar, sidecarError);
    REQUIRE(sidecarError.IsOk());
    CHECK(scene.textures[0].ref.assetId == textureId);

    const entt::entity second = SceneLoader::ImportObjIntoECS(
        scene, settings, context, diagnostics);
    REQUIRE(second != entt::entity{entt::null});
    REQUIRE(scene.textures.size() == 2);
    CHECK(diagnostics.empty());
    CHECK(scene.textures[1].ref.assetId == textureId);
}

TEST_CASE("Phase7 W3 step 6.4: all OBJ texture roles retain deterministic placeholder slots")
{
    TempDirectory fixture;
    REQUIRE(WriteText(fixture.Path() / "diffuse.bad", "not an image"));
    REQUIRE(WriteText(fixture.Path() / "emissive.bad", "not an image"));
    const fs::path obj = WriteFourRoleObj(
        fixture.Path(),
        { "diffuse.bad", "normal-missing.png",
          "emissive.bad", "roughness-missing.png" });

    ECSScene scene;
    std::vector<AssetDiagnostic> diagnostics;
    REQUIRE(SceneLoader::LoadObjIntoECS(
        scene, ReadOnlyTextureContext(obj), diagnostics));
    REQUIRE(scene.materials.size() == 1);
    REQUIRE(scene.textures.size() == 4);
    for (const auto& texture : scene.textures)
        CheckExactTexturePlaceholder(texture);

    const auto& material = scene.materials[0];
    CHECK(material.baseColorTextureIndex == 0);
    CHECK(material.normalTextureIndex == 1);
    CHECK(material.emissiveTextureIndex == 2);
    CHECK(material.metallicRoughnessTextureIndex == 3);
    CHECK(scene.textures[0].ref.sourceKey ==
          "obj:material=0:texture=diffuse");
    CHECK(scene.textures[1].ref.sourceKey ==
          "obj:material=0:texture=normal");
    CHECK(scene.textures[2].ref.sourceKey ==
          "obj:material=0:texture=emissive");
    CHECK(scene.textures[3].ref.sourceKey ==
          "obj:material=0:texture=roughness");
    CHECK(CountSeverity(diagnostics, AssetDiagnostic::Stale) == 2);
    CHECK(CountSeverity(diagnostics, AssetDiagnostic::Missing) == 2);
    CHECK(CountSeverity(diagnostics, AssetDiagnostic::Malformed) == 2);

    const std::vector<AssetDiagnostic::Severity> expectedOrder = {
        AssetDiagnostic::Stale,
        AssetDiagnostic::Malformed,
        AssetDiagnostic::Stale,
        AssetDiagnostic::Malformed,
        AssetDiagnostic::Missing,
        AssetDiagnostic::Missing,
    };
    std::vector<AssetDiagnostic::Severity> actualOrder;
    for (const auto& diagnostic : diagnostics)
        actualOrder.push_back(diagnostic.severity);
    CHECK(actualOrder == expectedOrder);
}

TEST_CASE("Phase7 W3 step 6.4: reversed OBJ manifest input has an identical sorted snapshot")
{
    TempDirectory fixture;
    REQUIRE(WriteText(fixture.Path() / "diffuse.bad", "not an image"));
    REQUIRE(WriteText(fixture.Path() / "emissive.bad", "not an image"));
    const fs::path owner = fixture.Path() / "model.obj";
    REQUIRE(WriteText(owner, "# manifest owner\n"));
    const auto context = ReadOnlyTextureContext(owner);

    TextureManifest canonical;
    const auto append = [&](size_t slot,
                            ObjTextureRole role,
                            const std::string& path) {
        TextureManifestEntry entry;
        entry.outputSlot = slot;
        entry.ref.kind = AssetKind::Texture;
        entry.ref.path = path;
        entry.ref.sourceKey = ObjTextureSourceKey(0, role);
        entry.payloadKind = TexturePayloadKind::External;
        entry.externalUri = path;
        entry.materialIndex = 0;
        entry.objRole = role;
        canonical.push_back(std::move(entry));
    };
    append(0, ObjTextureRole::Diffuse, "diffuse.bad");
    append(1, ObjTextureRole::Normal, "normal-missing.png");
    append(2, ObjTextureRole::Emissive, "emissive.bad");
    append(3, ObjTextureRole::Roughness, "roughness-missing.png");

    TextureManifest reversed = canonical;
    std::reverse(reversed.begin(), reversed.end());
    SortTextureManifest(canonical);
    SortTextureManifest(reversed);

    REQUIRE(canonical.size() == reversed.size());
    for (size_t i = 0; i < canonical.size(); ++i)
    {
        CHECK(canonical[i].outputSlot == reversed[i].outputSlot);
        CHECK(canonical[i].materialIndex == reversed[i].materialIndex);
        CHECK(canonical[i].objRole == reversed[i].objRole);
        CHECK(canonical[i].ref.sourceKey == reversed[i].ref.sourceKey);
    }

    std::vector<AssetDiagnostic> canonicalDiagnostics;
    std::vector<AssetDiagnostic> reversedDiagnostics;
    const auto canonicalTextures = ResolveAndDecodeTextures(
        canonical, context, canonicalDiagnostics);
    const auto reversedTextures = ResolveAndDecodeTextures(
        reversed, context, reversedDiagnostics);
    REQUIRE(canonicalTextures.size() == reversedTextures.size());
    for (size_t i = 0; i < canonicalTextures.size(); ++i)
    {
        CHECK(canonicalTextures[i].pixels == reversedTextures[i].pixels);
        CHECK(canonicalTextures[i].width == reversedTextures[i].width);
        CHECK(canonicalTextures[i].height == reversedTextures[i].height);
        CHECK(canonicalTextures[i].ref.kind ==
              reversedTextures[i].ref.kind);
        CHECK(canonicalTextures[i].ref.path ==
              reversedTextures[i].ref.path);
        CHECK(canonicalTextures[i].ref.sourceKey ==
              reversedTextures[i].ref.sourceKey);
    }

    const auto snapshot = [](const std::vector<AssetDiagnostic>& values) {
        std::vector<std::string> result;
        for (const auto& diagnostic : values)
        {
            result.push_back(
                std::to_string(static_cast<int>(diagnostic.severity)) +
                "|" + diagnostic.sourceKey +
                "|" + diagnostic.refPath);
        }
        return result;
    };
    CHECK(snapshot(canonicalDiagnostics) ==
          snapshot(reversedDiagnostics));

    const std::vector<AssetDiagnostic::Severity> expectedOrder = {
        AssetDiagnostic::Stale,
        AssetDiagnostic::Malformed,
        AssetDiagnostic::Stale,
        AssetDiagnostic::Malformed,
        AssetDiagnostic::Missing,
        AssetDiagnostic::Missing,
    };
    std::vector<AssetDiagnostic::Severity> actualOrder;
    for (const auto& diagnostic : canonicalDiagnostics)
        actualOrder.push_back(diagnostic.severity);
    CHECK(actualOrder == expectedOrder);
}

TEST_CASE("Phase7 W3 step 6.3: glTF texture failures are contained")
{
    SUBCASE("success decodes the external image")
    {
        TempDirectory fixture;
        const fs::path gltf = WriteExternalTextureGltf(
            fixture.Path(), GltfImageCase::ValidExternal);

        ECSScene scene;
        std::vector<AssetDiagnostic> diagnostics;
        REQUIRE(SceneLoader::LoadIntoECS(
            scene, ReadOnlyTextureContext(gltf), diagnostics));
        REQUIRE(scene.materials.size() == 1);
        REQUIRE(scene.textures.size() == 1);
        CHECK(scene.meshRegistry.GetCount() == 1);
        CHECK_FALSE(scene.textures[0].pixels.empty());
        CHECK(scene.materials[0].baseColorTextureIndex == 0);
        CHECK(scene.textures[0].ref.kind == AssetKind::Texture);
        CHECK(scene.textures[0].ref.path == "valid.ppm");
        CHECK(scene.textures[0].ref.sourceKey == "gltf:image=0");
        CHECK(CountSeverity(diagnostics, AssetDiagnostic::Stale) == 1);
    }

    SUBCASE("missing external image installs a placeholder")
    {
        TempDirectory fixture;
        const fs::path gltf = WriteExternalTextureGltf(
            fixture.Path(), GltfImageCase::MissingExternal);

        ECSScene scene;
        std::vector<AssetDiagnostic> diagnostics;
        REQUIRE(SceneLoader::LoadIntoECS(
            scene, ReadOnlyTextureContext(gltf), diagnostics));
        REQUIRE(scene.materials.size() == 1);
        REQUIRE(scene.textures.size() == 1);
        CHECK(scene.meshRegistry.GetCount() == 1);
        CHECK(scene.textures[0].filepath == "missing.ppm");
        CHECK(scene.textures[0].ref.kind == AssetKind::Texture);
        CHECK(scene.textures[0].ref.sourceKey == "gltf:image=0");
        CheckExactTexturePlaceholder(scene.textures[0]);
        CHECK(scene.materials[0].baseColorTextureIndex == 0);
        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0].severity == AssetDiagnostic::Missing);
        CHECK(diagnostics[0].kind == AssetKind::Texture);
        CHECK(diagnostics[0].sourceKey == "gltf:image=0");
    }

    SUBCASE("malformed external image preserves valid geometry")
    {
        TempDirectory fixture;
        const fs::path gltf = WriteExternalTextureGltf(
            fixture.Path(), GltfImageCase::MalformedExternal);

        ECSScene scene;
        std::vector<AssetDiagnostic> diagnostics;
        REQUIRE(SceneLoader::LoadIntoECS(
            scene, ReadOnlyTextureContext(gltf), diagnostics));
        CHECK(scene.meshRegistry.GetCount() == 1);
        REQUIRE(scene.materials.size() == 1);
        REQUIRE(scene.textures.size() == 1);
        CHECK(scene.textures[0].filepath == "malformed.png");
        CheckExactTexturePlaceholder(scene.textures[0]);
        CHECK(scene.materials[0].baseColorTextureIndex == 0);
        CHECK(CountSeverity(
            diagnostics, AssetDiagnostic::Stale) == 1);
        CHECK(CountSeverity(
            diagnostics, AssetDiagnostic::Malformed) == 1);
    }

    SUBCASE("invalid texture source installs an unresolved placeholder")
    {
        TempDirectory fixture;
        const fs::path gltf = WriteExternalTextureGltf(
            fixture.Path(), GltfImageCase::InvalidTextureSource);

        ECSScene scene;
        std::vector<AssetDiagnostic> diagnostics;
        REQUIRE(SceneLoader::LoadIntoECS(
            scene, ReadOnlyTextureContext(gltf), diagnostics));
        REQUIRE(scene.materials.size() == 1);
        REQUIRE(scene.textures.size() == 1);
        CHECK(scene.meshRegistry.GetCount() == 1);
        CHECK(scene.textures[0].ref.kind == AssetKind::Texture);
        CHECK(scene.textures[0].ref.sourceKey == "gltf:texture=0");
        CheckExactTexturePlaceholder(scene.textures[0]);
        CHECK(scene.materials[0].baseColorTextureIndex == 0);
        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0].severity == AssetDiagnostic::Unresolved);
        CHECK(diagnostics[0].kind == AssetKind::Texture);
        CHECK(diagnostics[0].sourceKey == "gltf:texture=0");
    }
}

TEST_CASE("Phase7 W3 step 6.3: explicit glTF import assigns and reuses external texture identity")
{
    TempDirectory fixture;
    const fs::path gltf = WriteExternalTextureGltf(
        fixture.Path(), GltfImageCase::ValidExternal);
    const fs::path texturePath = fixture.Path() / "valid.ppm";
    DeterministicUuidProvider ids;
    auto context = ExplicitTextureContext(gltf, ids);

    ECSScene scene;
    std::vector<AssetDiagnostic> diagnostics;
    const entt::entity firstRoot = SceneLoader::ImportIntoECS(
        scene, context, diagnostics);
    REQUIRE(firstRoot != entt::entity{entt::null});
    REQUIRE(scene.textures.size() == 1);
    REQUIRE(diagnostics.empty());
    CHECK_FALSE(scene.textures[0].ref.assetId.IsNull());
    CHECK(scene.textures[0].ref.kind == AssetKind::Texture);
    CHECK(scene.textures[0].ref.path == "valid.ppm");
    CHECK(scene.textures[0].ref.sourceKey == "gltf:image=0");
    REQUIRE(fs::is_regular_file(AssetSidecarPath(texturePath)));

    Error sidecarError;
    const UUID textureId = ReadSidecarId(
        AssetSidecarPath(texturePath), sidecarError);
    REQUIRE(sidecarError.IsOk());
    CHECK(textureId == scene.textures[0].ref.assetId);

    diagnostics.clear();
    const entt::entity secondRoot = SceneLoader::ImportIntoECS(
        scene, context, diagnostics);
    REQUIRE(secondRoot != entt::entity{entt::null});
    REQUIRE(scene.textures.size() == 2);
    CHECK(diagnostics.empty());
    CHECK(scene.textures[1].ref.assetId == textureId);
}

TEST_CASE("Phase7 W3 step 6.3: glTF dependencies resolve by database identity or conflict")
{
    SUBCASE("moved external image resolves by its unique dependency ID")
    {
        TempDirectory fixture;
        const fs::path gltf = WriteExternalTextureGltf(
            fixture.Path(), GltfImageCase::ValidExternal);
        const fs::path movedDirectory = fixture.Path() / "moved";
        std::error_code filesystemError;
        fs::create_directories(movedDirectory, filesystemError);
        REQUIRE_FALSE(filesystemError);
        const fs::path movedTexture = movedDirectory / "valid.ppm";
        fs::rename(fixture.Path() / "valid.ppm",
                   movedTexture, filesystemError);
        REQUIRE_FALSE(filesystemError);

        const UUID ownerId =
            UUID::Parse("10000000-0000-4000-8000-000000000001");
        const UUID textureId =
            UUID::Parse("20000000-0000-4000-8000-000000000002");
        Error sidecarError;
        REQUIRE(WriteSidecarId(
            AssetSidecarPath(movedTexture), textureId, sidecarError));
        REQUIRE(sidecarError.IsOk());

        AssetRecord owner;
        owner.assetId = ownerId;
        owner.sourcePath = "model.gltf";
        owner.observedKinds = { AssetKind::Model };
        owner.dependencies.push_back({
            "gltf:image=0", textureId, "moved/valid.ppm",
            AssetKind::Texture });
        AssetRecord target;
        target.assetId = textureId;
        target.sourcePath = "moved/valid.ppm";
        target.identityAuthority =
            AssetIdentityAuthority::Sidecar;
        target.observedKinds = { AssetKind::Texture };
        std::vector<AssetDatabaseDiagnostic> databaseDiagnostics;
        AssetDatabase database = BuildAssetDatabase(
            { owner, target }, databaseDiagnostics);
        REQUIRE(databaseDiagnostics.empty());

        auto context = ReadOnlyTextureContext(gltf);
        context.resolution.database = &database;
        context.ownerModel.assetId = ownerId;
        context.effectiveOwnerId = ownerId;
        ECSScene scene;
        std::vector<AssetDiagnostic> diagnostics;
        REQUIRE(SceneLoader::LoadIntoECS(
            scene, context, diagnostics));
        REQUIRE(scene.textures.size() == 1);
        CHECK_FALSE(scene.textures[0].pixels.empty());
        CHECK(scene.textures[0].ref.assetId == textureId);
        CHECK(scene.textures[0].ref.path == "moved/valid.ppm");
        CHECK(diagnostics.empty());
    }

    SUBCASE("two dependency claims produce Conflict and a placeholder")
    {
        TempDirectory fixture;
        const fs::path gltf = WriteExternalTextureGltf(
            fixture.Path(), GltfImageCase::ValidExternal);
        REQUIRE(WritePpm(fixture.Path() / "other.ppm"));
        const UUID first =
            UUID::Parse("30000000-0000-4000-8000-000000000003");
        const UUID second =
            UUID::Parse("40000000-0000-4000-8000-000000000004");

        AssetRecord owner;
        owner.sourcePath = "model.gltf";
        owner.observedKinds = { AssetKind::Model };
        owner.dependencies = {
            { "gltf:image=0", first, "valid.ppm",
              AssetKind::Texture },
            { "gltf:image=0", second, "other.ppm",
              AssetKind::Texture },
        };
        std::vector<AssetDatabaseDiagnostic> databaseDiagnostics;
        AssetDatabase database = BuildAssetDatabase(
            { owner }, databaseDiagnostics);
        auto context = ReadOnlyTextureContext(gltf);
        context.resolution.database = &database;

        ECSScene scene;
        std::vector<AssetDiagnostic> diagnostics;
        REQUIRE(SceneLoader::LoadIntoECS(
            scene, context, diagnostics));
        REQUIRE(scene.textures.size() == 1);
        CheckExactTexturePlaceholder(scene.textures[0]);
        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0].severity == AssetDiagnostic::Conflict);
        CHECK(diagnostics[0].sourceKey == "gltf:image=0");
    }

    SUBCASE("ambiguous dependency ID produces Conflict and a placeholder")
    {
        TempDirectory fixture;
        const fs::path gltf = WriteExternalTextureGltf(
            fixture.Path(), GltfImageCase::ValidExternal);
        REQUIRE(WritePpm(fixture.Path() / "other.ppm"));
        const UUID duplicate =
            UUID::Parse("50000000-0000-4000-8000-000000000005");
        Error sidecarError;
        REQUIRE(WriteSidecarId(
            AssetSidecarPath(fixture.Path() / "valid.ppm"),
            duplicate, sidecarError));
        REQUIRE(sidecarError.IsOk());
        REQUIRE(WriteSidecarId(
            AssetSidecarPath(fixture.Path() / "other.ppm"),
            duplicate, sidecarError));
        REQUIRE(sidecarError.IsOk());

        AssetRecord owner;
        owner.sourcePath = "model.gltf";
        owner.observedKinds = { AssetKind::Model };
        owner.dependencies = {
            { "gltf:image=0", duplicate, "valid.ppm",
              AssetKind::Texture },
        };
        AssetRecord first;
        first.assetId = duplicate;
        first.sourcePath = "valid.ppm";
        first.identityAuthority = AssetIdentityAuthority::Sidecar;
        first.observedKinds = { AssetKind::Texture };
        AssetRecord second;
        second.assetId = duplicate;
        second.sourcePath = "other.ppm";
        second.identityAuthority = AssetIdentityAuthority::Sidecar;
        second.observedKinds = { AssetKind::Texture };
        std::vector<AssetDatabaseDiagnostic> databaseDiagnostics;
        AssetDatabase database = BuildAssetDatabase(
            { owner, first, second }, databaseDiagnostics);
        REQUIRE_FALSE(databaseDiagnostics.empty());
        auto context = ReadOnlyTextureContext(gltf);
        context.resolution.database = &database;

        ECSScene scene;
        std::vector<AssetDiagnostic> diagnostics;
        REQUIRE(SceneLoader::LoadIntoECS(
            scene, context, diagnostics));
        REQUIRE(scene.textures.size() == 1);
        CheckExactTexturePlaceholder(scene.textures[0]);
        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0].severity == AssetDiagnostic::Conflict);
        CHECK(diagnostics[0].sourceKey == "gltf:image=0");
    }
}

TEST_CASE("Phase7 W3 step 6.3: embedded glTF images use owner identity and contain decode failure")
{
    SUBCASE("embedded GLB uses the explicitly assigned owner ID")
    {
        TempDirectory fixture;
        const fs::path glb = WriteEmbeddedTextureGlb(fixture.Path());
        DeterministicUuidProvider ids;
        auto context = ExplicitTextureContext(glb, ids);

        ECSScene scene;
        std::vector<AssetDiagnostic> diagnostics;
        REQUIRE(SceneLoader::LoadIntoECS(
            scene, context, diagnostics));
        REQUIRE(scene.textures.size() == 1);
        CHECK_FALSE(scene.textures[0].pixels.empty());
        CHECK(scene.textures[0].ref.kind == AssetKind::Texture);
        CHECK(scene.textures[0].ref.path == "embedded.glb");
        CHECK(scene.textures[0].ref.sourceKey == "gltf:image=0");
        CHECK_FALSE(scene.textures[0].ref.assetId.IsNull());
        CHECK(diagnostics.empty());

        Error sidecarError;
        const UUID ownerId = ReadSidecarId(
            AssetSidecarPath(glb), sidecarError);
        REQUIRE(sidecarError.IsOk());
        CHECK(scene.textures[0].ref.assetId == ownerId);
        size_t sidecarCount = 0;
        for (const auto& entry :
             fs::directory_iterator(fixture.Path()))
            if (entry.path().extension() == ".rt2meta")
                ++sidecarCount;
        CHECK(sidecarCount == 1);
    }

    SUBCASE("data URI uses a supplied owner ID and no child sidecar")
    {
        TempDirectory fixture;
        const fs::path gltf =
            WriteDataUriTextureGltf(fixture.Path(), false);
        const UUID ownerId =
            UUID::Parse("60000000-0000-4000-8000-000000000006");
        Error sidecarError;
        REQUIRE(WriteSidecarId(
            AssetSidecarPath(gltf), ownerId, sidecarError));
        REQUIRE(sidecarError.IsOk());
        auto context = ReadOnlyTextureContext(gltf);
        context.ownerModel.assetId = ownerId;
        context.effectiveOwnerId = ownerId;

        ECSScene scene;
        std::vector<AssetDiagnostic> diagnostics;
        REQUIRE(SceneLoader::LoadIntoECS(
            scene, context, diagnostics));
        REQUIRE(scene.textures.size() == 1);
        CHECK_FALSE(scene.textures[0].pixels.empty());
        CHECK(scene.textures[0].ref.assetId == ownerId);
        CHECK(scene.textures[0].ref.sourceKey == "gltf:image=0");
        CHECK(diagnostics.empty());
        CHECK_FALSE(fs::exists(
            fixture.Path() / "data-uri-image.rt2meta"));
    }

    SUBCASE("malformed data URI preserves geometry with a placeholder")
    {
        TempDirectory fixture;
        const fs::path gltf =
            WriteDataUriTextureGltf(fixture.Path(), true);
        auto context = ReadOnlyTextureContext(gltf);

        ECSScene scene;
        std::vector<AssetDiagnostic> diagnostics;
        REQUIRE(SceneLoader::LoadIntoECS(
            scene, context, diagnostics));
        CHECK(scene.meshRegistry.GetCount() == 1);
        REQUIRE(scene.materials.size() == 1);
        REQUIRE(scene.textures.size() == 1);
        CheckExactTexturePlaceholder(scene.textures[0]);
        CHECK(scene.materials[0].baseColorTextureIndex == 0);
        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0].severity == AssetDiagnostic::Malformed);
        CHECK(diagnostics[0].sourceKey == "gltf:image=0");
    }
}

TEST_CASE("Phase7 W3 step 6.3: SceneAssetResolver threads texture context and diagnostics")
{
    TempDirectory fixture;
    const fs::path gltf = WriteExternalTextureGltf(
        fixture.Path(), GltfImageCase::MalformedExternal);
    SceneDocument document;
    DeterministicUuidProvider ids;
    document.SetUuidProvider(&ids);
    const entt::entity entity = AddImportedEntity(
        document, gltf.filename().generic_string(),
        SceneAssetResolver::GltfSourceKey(0, 0, 0, 0));
    const UUID entityId =
        document.ecs.registry.get<EntityIdComponent>(entity).id;

    std::vector<AssetDiagnostic> diagnostics;
    Error resolveError;
    REQUIRE(SceneAssetResolver::ResolveAll(
        document, fixture.Path(), diagnostics, resolveError));
    REQUIRE(resolveError.IsOk());
    REQUIRE(document.ecs.textures.size() == 1);
    CheckExactTexturePlaceholder(document.ecs.textures[0]);

    bool sawTextureMalformed = false;
    bool sawTextureStale = false;
    for (const auto& diagnostic : diagnostics)
    {
        if (diagnostic.kind != AssetKind::Texture)
            continue;
        CHECK(diagnostic.entityUuid == entityId);
        CHECK(diagnostic.sourceKey == "gltf:image=0");
        sawTextureMalformed |=
            diagnostic.severity == AssetDiagnostic::Malformed;
        sawTextureStale |=
            diagnostic.severity == AssetDiagnostic::Stale;
    }
    CHECK(sawTextureMalformed);
    CHECK(sawTextureStale);
}

// ---------------------------------------------------------------------------
// Phase 7 W3 step 4 remediation (item 4) — exercise the real env import path.
//
// Both environment load paths (SceneManager::LoadEnvMap and SetEnvMapData)
// call ResolveOrAssign to mint/reuse a per-asset sidecar. The five step-4
// tests above hand-create the sidecar or assign the ID directly, so they
// would stay green even if those SceneManager lines were deleted. These
// tests drive the real import path so the ResolveOrAssign wiring is
// covered, and verify that sidecar read/write errors are surfaced through
// a structured out-param rather than only console output.
// ---------------------------------------------------------------------------

TEST_CASE("Phase7 W3 step 4 item 4: LoadEnvMap mints a sidecar via ResolveOrAssign")
{
    TempDirectory fixture;
    Error fixtureError;
    const fs::path exr = fixture.Path() / "env.exr";
    REQUIRE(GenerateTinyExrEnv(exr, fixtureError));

    // No sidecar exists yet. LoadEnvMap must call ResolveOrAssign, mint a
    // fresh v4, write the sidecar, and cache the ID into the document.
    SceneManager mgr;
    Error envImportErr;
    REQUIRE(mgr.LoadEnvMap(exr.string(), &envImportErr));

    const auto& env = mgr.AuthoringDoc().environment;
    CHECK(env.HasEnvMap());
    CHECK_FALSE(env.ref.assetId.IsNull());

    // The sidecar file MUST have been written by ResolveOrAssign. This is
    // the line that the hand-assigned tests do not exercise: a test that
    // only sets env.ref.assetId would not produce a sidecar on disk.
    const fs::path sidecar = AssetSidecarPath(exr);
    REQUIRE(fs::exists(sidecar));
    Error readErr;
    const UUID sidecarId = ReadSidecarId(sidecar, readErr);
    CHECK(readErr.IsOk());
    CHECK_FALSE(sidecarId.IsNull());
    CHECK(sidecarId == env.ref.assetId);

    // A second LoadEnvMap on the same file must reuse the committed sidecar
    // ID (minted=false), proving the identity is durable, not re-minted.
    const UUID firstId = env.ref.assetId;
    Error envImportErr2;
    REQUIRE(mgr.LoadEnvMap(exr.string(), &envImportErr2));
    CHECK(mgr.AuthoringDoc().environment.ref.assetId == firstId);
}

TEST_CASE("Phase7 W3 step 4 item 4: SetEnvMapData (async path) mints a sidecar via ResolveOrAssign")
{
    TempDirectory fixture;
    Error fixtureError;
    const fs::path exr = fixture.Path() / "env_async.exr";
    REQUIRE(GenerateTinyExrEnv(exr, fixtureError));

    // Decode pixels the way the async path would, then hand them to
    // SetEnvMapData. This is the completion path WalnutApp uses after a
    // background decode.
    std::vector<float> pixels;
    int w = 0, h = 0;
    {
        Error decodeErr;
        SceneDocument tmp;
        tmp.environment.ref.path = exr.string();
        std::vector<AssetDiagnostic> diags;
        REQUIRE(SceneAssetResolver::ResolveEnvironment(
            tmp, fixture.Path(), diags, decodeErr));
        w = tmp.environment.width;
        h = tmp.environment.height;
        pixels = std::move(tmp.environment.floatPixels);
    }
    REQUIRE(w > 0);
    REQUIRE(!pixels.empty());

    SceneManager mgr;
    Error envImportErr;
    mgr.SetEnvMapData(exr.string(), w, h, std::move(pixels), &envImportErr);

    const auto& env = mgr.AuthoringDoc().environment;
    CHECK(env.HasEnvMap());
    CHECK_FALSE(env.ref.assetId.IsNull());
    // The async path must also write the sidecar — this is the coverage gap
    // the reviewer identified (deleting the SetEnvMapData ResolveOrAssign
    // block left the step-4 tests green).
    const fs::path sidecar = AssetSidecarPath(exr);
    REQUIRE(fs::exists(sidecar));
    Error readErr;
    const UUID sidecarId = ReadSidecarId(sidecar, readErr);
    CHECK(readErr.IsOk());
    CHECK(sidecarId == env.ref.assetId);
}

TEST_CASE("Phase7 W3 step 4 item 4: sidecar read error is surfaced through the structured out-param")
{
    TempDirectory fixture;
    Error fixtureError;
    const fs::path exr = fixture.Path() / "env_bad_sidecar.exr";
    REQUIRE(GenerateTinyExrEnv(exr, fixtureError));

    // Pre-write a MALFORMED sidecar. ResolveOrAssign must overwrite it
    // with a fresh ID and report the parse failure through err (minted=true,
    // err is a Parse error). The load must still succeed (the asset gets a
    // session ID), but the error must be observable through the structured
    // out-param, not only the console.
    const fs::path sidecar = AssetSidecarPath(exr);
    REQUIRE(WriteText(sidecar, "not a uuid"));
    REQUIRE(fs::exists(sidecar));

    SceneManager mgr;
    Error envImportErr;
    REQUIRE(mgr.LoadEnvMap(exr.string(), &envImportErr));

    // The load succeeded and a fresh ID was assigned.
    CHECK(mgr.AuthoringDoc().environment.HasEnvMap());
    CHECK_FALSE(mgr.AuthoringDoc().environment.ref.assetId.IsNull());

    // The structured diagnostic must carry the malformed-sidecar parse
    // error. Without the out-param, this error was only printf'd.
    CHECK_FALSE(envImportErr.IsOk());
    CHECK(envImportErr.code == Error::Parse);

    // The sidecar must have been overwritten with a valid ID.
    Error readErr;
    const UUID repaired = ReadSidecarId(sidecar, readErr);
    CHECK(readErr.IsOk());
    CHECK(repaired == mgr.AuthoringDoc().environment.ref.assetId);
}

TEST_CASE("Phase7 W3 step 7.2: SceneManager rejects a relative direct model path before import")
{
    const fs::path relative = "rt2_w3_relative_direct_import.obj";
    REQUIRE(WriteText(
        relative,
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"));
    struct RelativeFixtureGuard {
        fs::path path;
        ~RelativeFixtureGuard()
        {
            std::error_code ec;
            fs::remove(path, ec);
            fs::remove(AssetSidecarPath(path), ec);
        }
    } guard{ relative };

    DeterministicUuidProvider ids;
    SceneManager manager;
    manager.SetUuidProvider(&ids);
    std::vector<AssetDiagnostic> diagnostics;

    CHECK_FALSE(manager.LoadScene(relative.string(), &diagnostics));
    CHECK(manager.GetECS().registry.view<Transform>().empty());
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].kind == AssetKind::Model);
    CHECK(diagnostics[0].severity == AssetDiagnostic::Malformed);
    CHECK(diagnostics[0].detail == "direct model path must be absolute");
    std::error_code ec;
    CHECK_FALSE(fs::exists(AssetSidecarPath(relative), ec));
}
