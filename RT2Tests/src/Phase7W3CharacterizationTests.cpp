#include <doctest/doctest.h>

#include "ECSComponents.h"
#include "Phase1AFixtureGenerator.h"
#include "SceneAssetResolver.h"
#include "SceneDocument.h"
#include "SceneLoader.h"
#include "SceneSerializer.h"
#include "ScriptAssetPath.h"
#include "ScriptFieldRegistry.h"
#include "core/Error.h"
#include "core/UUID.h"

#include "json.hpp"

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
    CHECK(diagnostics.empty());
    CHECK(doc.ecs.registry.all_of<MeshRef>(entity));
    CHECK(doc.ecs.meshRegistry.GetCount() == 1);
    CHECK(doc.ecs.textures.size() == 1);
}

TEST_CASE("Phase7 W3 characterization: missing model emits file and entity diagnostics")
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
    REQUIRE(diagnostics.size() == 2);
    CHECK(CountSeverity(diagnostics, AssetDiagnostic::Missing) == 2);
    CHECK(diagnostics[0].entityUuid.IsNull());
    CHECK_FALSE(diagnostics[1].entityUuid.IsNull());
    CHECK_FALSE(doc.ecs.registry.all_of<MeshRef>(entity));
}

TEST_CASE("Phase7 W3 characterization: malformed model is followed by a Missing entity diagnostic")
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
    REQUIRE(diagnostics.size() == 2);
    CHECK(CountSeverity(diagnostics, AssetDiagnostic::Malformed) == 1);
    CHECK(CountSeverity(diagnostics, AssetDiagnostic::Missing) == 1);
    CHECK_FALSE(doc.ecs.registry.all_of<MeshRef>(entity));
}

TEST_CASE("Phase7 W3 characterization: unresolved model source key mutates resources before hard failure")
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
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].severity == AssetDiagnostic::Unresolved);
    CHECK_FALSE(doc.ecs.registry.all_of<MeshRef>(entity));

    // Transitional characterization of the false transactionality contract:
    // the resolver returns a hard failure after already appending resources.
    CHECK(doc.ecs.meshRegistry.GetCount() == 1);
    CHECK(doc.ecs.materials.size() == 1);
    CHECK(doc.ecs.textures.size() == 1);
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
        doc.environment.path = "environment.exr";
        std::vector<AssetDiagnostic> diagnostics;
        Error error;
        CHECK(SceneAssetResolver::ResolveEnvironment(
            doc, fixture.Path(), diagnostics, error));
        CHECK(error.IsOk());
        CHECK(diagnostics.empty());
        CHECK(doc.environment.width == 4);
        CHECK(doc.environment.height == 2);
        CHECK_FALSE(doc.environment.floatPixels.empty());
    }

    SUBCASE("missing is Missing and clears stale decoded data")
    {
        TempDirectory fixture;
        SceneDocument doc;
        doc.environment.path = "missing.exr";
        doc.environment.width = 8;
        doc.environment.height = 4;
        doc.environment.floatPixels.assign(8 * 4 * 4, 1.0f);

        std::vector<AssetDiagnostic> diagnostics;
        Error error;
        CHECK_FALSE(SceneAssetResolver::ResolveEnvironment(
            doc, fixture.Path(), diagnostics, error));
        CHECK(error.code == Error::MissingAsset);
        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0].severity == AssetDiagnostic::Missing);
        CHECK(diagnostics[0].kind == AssetKind::Environment);
        CHECK(doc.environment.path == "missing.exr");
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
        doc.environment.path = "malformed.exr";
        std::vector<AssetDiagnostic> diagnostics;
        Error error;
        CHECK_FALSE(SceneAssetResolver::ResolveEnvironment(
            doc, fixture.Path(), diagnostics, error));
        CHECK(error.code == Error::MissingAsset);
        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0].severity == AssetDiagnostic::Malformed);
        CHECK(diagnostics[0].kind == AssetKind::Environment);
        CHECK_FALSE(diagnostics[0].resolvedPath.empty());
    }
}

TEST_CASE("Phase7 W3 characterization: script path resolution is lexical and sourceKey-blind")
{
    TempDirectory fixture;
    SceneDocument doc;
    doc.metadata.sourcePath = fixture.Path() / "scene.rt2scene";

    SUBCASE("success resolves relative to the scene and parses declarations")
    {
        REQUIRE(WriteText(fixture.Path() / "script.lua",
            "rt2.fields = { speed = rt2.field.float(1.0) }\n"));
        const ScriptComponent script = ScriptAt("script.lua");
        const fs::path resolved = ResolveScriptAssetPath(doc, script);
        CHECK(resolved == fixture.Path() / "script.lua");

        ScriptFieldRegistry registry;
        const auto fields = registry.GetDeclaredFields(resolved);
        CHECK(fields.parsed);
        CHECK(fields.descriptors.size() == 1);
    }

    SUBCASE("missing still returns a non-empty candidate path")
    {
        const ScriptComponent script = ScriptAt("missing.lua");
        const fs::path resolved = ResolveScriptAssetPath(doc, script);
        CHECK(resolved == fixture.Path() / "missing.lua");
        CHECK_FALSE(resolved.empty());

        ScriptFieldRegistry registry;
        const auto fields = registry.GetDeclaredFields(resolved);
        CHECK_FALSE(fields.parsed);
        CHECK_FALSE(fields.diagnostic.empty());
    }

    SUBCASE("malformed path resolution succeeds and parsing fails separately")
    {
        REQUIRE(WriteText(fixture.Path() / "malformed.lua",
                          "rt2.fields = {\n"));
        const ScriptComponent script = ScriptAt("malformed.lua");
        const fs::path resolved = ResolveScriptAssetPath(doc, script);
        CHECK(resolved == fixture.Path() / "malformed.lua");

        ScriptFieldRegistry registry;
        const auto fields = registry.GetDeclaredFields(resolved);
        CHECK_FALSE(fields.parsed);
        CHECK_FALSE(fields.diagnostic.empty());
    }

    SUBCASE("a stale sourceKey does not participate in resolution")
    {
        REQUIRE(WriteText(fixture.Path() / "actual.lua", "\n"));
        ScriptComponent script = ScriptAt("actual.lua");
        script.asset.sourceKey = "lua:asset=other.lua";

        const fs::path resolved = ResolveScriptAssetPath(doc, script);
        CHECK(resolved == fixture.Path() / "actual.lua");
        ScriptFieldRegistry registry;
        CHECK(registry.GetDeclaredFields(resolved).parsed);
    }
}

TEST_CASE("Phase7 W3 transitional characterization: script assetId is dropped by serialization")
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
    REQUIRE(SceneSerializer::Save(authored, scenePath, saveError));

    SceneDocument loaded;
    DeterministicUuidProvider loadedIds;
    loaded.SetUuidProvider(&loadedIds);
    Error loadError;
    REQUIRE(SceneSerializer::Load(loaded, scenePath, loadError));

    const auto view = loaded.ecs.registry.view<ScriptComponent>();
    REQUIRE(view.size() == 1);
    CHECK(view.get<ScriptComponent>(*view.begin()).asset.assetId.IsNull());
}

TEST_CASE("Phase7 W3 characterization: OBJ texture failures do not fail valid geometry")
{
    SUBCASE("success attaches the decoded texture")
    {
        TempDirectory fixture;
        REQUIRE(WritePpm(fixture.Path() / "valid.ppm"));
        const fs::path obj = WriteTexturedObj(fixture.Path(), "valid.ppm");

        ECSScene scene;
        REQUIRE(SceneLoader::LoadObjIntoECS(scene, obj.string()));
        REQUIRE(scene.materials.size() == 1);
        CHECK(scene.meshRegistry.GetCount() == 1);
        CHECK(scene.textures.size() == 1);
        CHECK(scene.materials[0].baseColorTextureIndex == 0);
    }

    SUBCASE("missing texture is silently dropped")
    {
        TempDirectory fixture;
        const fs::path obj = WriteTexturedObj(fixture.Path(), "missing.ppm");

        ECSScene scene;
        REQUIRE(SceneLoader::LoadObjIntoECS(scene, obj.string()));
        REQUIRE(scene.materials.size() == 1);
        CHECK(scene.meshRegistry.GetCount() == 1);
        CHECK(scene.textures.empty());
        CHECK(scene.materials[0].baseColorTextureIndex == -1);
    }

    SUBCASE("malformed texture is logged and dropped")
    {
        TempDirectory fixture;
        REQUIRE(WriteText(fixture.Path() / "malformed.png", "not an image"));
        const fs::path obj =
            WriteTexturedObj(fixture.Path(), "malformed.png");

        ECSScene scene;
        REQUIRE(SceneLoader::LoadObjIntoECS(scene, obj.string()));
        REQUIRE(scene.materials.size() == 1);
        CHECK(scene.meshRegistry.GetCount() == 1);
        CHECK(scene.textures.empty());
        CHECK(scene.materials[0].baseColorTextureIndex == -1);
    }
}

TEST_CASE("Phase7 W3 characterization: glTF texture failures have three different outcomes")
{
    SUBCASE("success decodes the external image")
    {
        TempDirectory fixture;
        const fs::path gltf = WriteExternalTextureGltf(
            fixture.Path(), GltfImageCase::ValidExternal);

        ECSScene scene;
        REQUIRE(SceneLoader::LoadIntoECS(scene, gltf.string()));
        REQUIRE(scene.materials.size() == 1);
        REQUIRE(scene.textures.size() == 1);
        CHECK(scene.meshRegistry.GetCount() == 1);
        CHECK_FALSE(scene.textures[0].pixels.empty());
        CHECK(scene.materials[0].baseColorTextureIndex == 0);
    }

    SUBCASE("missing external image preserves an empty texture slot")
    {
        TempDirectory fixture;
        const fs::path gltf = WriteExternalTextureGltf(
            fixture.Path(), GltfImageCase::MissingExternal);

        ECSScene scene;
        REQUIRE(SceneLoader::LoadIntoECS(scene, gltf.string()));
        REQUIRE(scene.materials.size() == 1);
        REQUIRE(scene.textures.size() == 1);
        CHECK(scene.meshRegistry.GetCount() == 1);
        CHECK(scene.textures[0].filepath == "missing.ppm");
        CHECK(scene.textures[0].pixels.empty());
        CHECK(scene.materials[0].baseColorTextureIndex == 0);
    }

    SUBCASE("malformed external image fails the whole model")
    {
        TempDirectory fixture;
        const fs::path gltf = WriteExternalTextureGltf(
            fixture.Path(), GltfImageCase::MalformedExternal);

        ECSScene scene;
        CHECK_FALSE(SceneLoader::LoadIntoECS(scene, gltf.string()));
        CHECK(scene.meshRegistry.GetCount() == 0);
        CHECK(scene.materials.empty());
        CHECK(scene.textures.empty());
    }

    SUBCASE("invalid texture source preserves an unresolved empty slot")
    {
        TempDirectory fixture;
        const fs::path gltf = WriteExternalTextureGltf(
            fixture.Path(), GltfImageCase::InvalidTextureSource);

        ECSScene scene;
        REQUIRE(SceneLoader::LoadIntoECS(scene, gltf.string()));
        REQUIRE(scene.materials.size() == 1);
        REQUIRE(scene.textures.size() == 1);
        CHECK(scene.meshRegistry.GetCount() == 1);
        CHECK(scene.textures[0].filepath.empty());
        CHECK(scene.textures[0].pixels.empty());
        CHECK(scene.materials[0].baseColorTextureIndex == 0);
    }
}
