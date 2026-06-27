#include "SceneLoader.h"

// Disable stb_image/stb_image_write in tinygltf — we handle image data
// ourselves via stb_image directly. But we still need to provide a
// LoadImageData callback so tinygltf can parse GLB files with embedded images.
#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE

#include "tiny_gltf.h"

// stb_image for decoding texture images (implementation provided by Walnut::Image.cpp)
#include "stb_image.h"

// Image loader: decodes raw image bytes (PNG/JPEG/etc) to RGBA8 using stb_image.
// Stores decoded pixels in image->image as unsigned char vector.
bool DecodeImageData(tinygltf::Image *image, const int image_idx,
                     std::string *err, std::string *warn,
                     int req_width, int req_height,
                     const unsigned char *bytes, int size, void *user_data)
{
    (void)req_width; (void)req_height; (void)user_data;

    int w, h, channels;
    unsigned char *decoded = stbi_load_from_memory(bytes, size, &w, &h, &channels, 4);
    if (!decoded)
    {
        if (err)
            *err += "Failed to decode image " + std::to_string(image_idx) + "\n";
        return false;
    }

    image->width = w;
    image->height = h;
    image->component = 4;
    image->image.assign(decoded, decoded + (size_t)(w * h * 4));
    stbi_image_free(decoded);
    return true;
}

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <fstream>
#include <filesystem>
#include <functional>
#include <cstring>
#include <algorithm>

namespace fs = std::filesystem;

// ============================================================================
// Save: Scene -> tinygltf::Model -> file
// ============================================================================

bool SceneLoader::Save(const Scene& scene, const std::string& filepath)
{
    if (filepath.empty())
        return false;

    fs::path p(filepath);
    if (p.has_parent_path())
    {
        fs::path parent = p.parent_path();
        if (!parent.empty() && !fs::exists(parent))
            return false;
    }

    tinygltf::Model model;
    tinygltf::Scene gltfScene;
    tinygltf::Asset asset;

    asset.version = "2.0";
    asset.generator = "RT2 Scene Exporter";
    model.asset = asset;

    // --- Textures ---
    const auto& textures = scene.GetTextures();
    for (const auto& tex : textures)
    {
        tinygltf::Image image;
        image.uri = tex.filepath;
        model.images.push_back(image);

        tinygltf::Texture gtext;
        gtext.source = static_cast<int>(model.images.size()) - 1;
        model.textures.push_back(gtext);
    }

    // --- Materials ---
    const auto& materials = scene.GetMaterials();
    for (const auto& mat : materials)
    {
        tinygltf::Material gmat;

        gmat.pbrMetallicRoughness.baseColorFactor = {
            mat.baseColor.r, mat.baseColor.g, mat.baseColor.b, mat.baseAlpha
        };
        gmat.pbrMetallicRoughness.metallicFactor = mat.metallic;
        gmat.pbrMetallicRoughness.roughnessFactor = mat.roughness;

        if (mat.baseColorTextureIndex >= 0 && mat.baseColorTextureIndex < (int)model.textures.size())
        {
            gmat.pbrMetallicRoughness.baseColorTexture.index = mat.baseColorTextureIndex;
            gmat.pbrMetallicRoughness.baseColorTexture.texCoord = 0;
        }

        gmat.emissiveFactor = {
            mat.emissiveIntensity > 0.0f ? mat.emissiveColor.r : 0.0f,
            mat.emissiveIntensity > 0.0f ? mat.emissiveColor.g : 0.0f,
            mat.emissiveIntensity > 0.0f ? mat.emissiveColor.b : 0.0f
        };

        if (mat.normalTextureIndex >= 0 && mat.normalTextureIndex < (int)model.textures.size())
        {
            gmat.normalTexture.index = mat.normalTextureIndex;
            gmat.normalTexture.texCoord = 0;
        }

        if (mat.emissiveTextureIndex >= 0 && mat.emissiveTextureIndex < (int)model.textures.size())
        {
            gmat.emissiveTexture.index = mat.emissiveTextureIndex;
            gmat.emissiveTexture.texCoord = 0;
        }

        if (mat.emissiveIntensity > 0.0f)
        {
            tinygltf::Value::Object strengthExt;
            strengthExt["emissiveStrength"] = tinygltf::Value(static_cast<double>(mat.emissiveIntensity));
            gmat.extensions["KHR_materials_emissive_strength"] = tinygltf::Value(strengthExt);
            if (std::find(model.extensionsUsed.begin(), model.extensionsUsed.end(),
                          "KHR_materials_emissive_strength") == model.extensionsUsed.end())
                model.extensionsUsed.push_back("KHR_materials_emissive_strength");
        }

        // glTF alpha mode and cutoff
        gmat.alphaMode = mat.alphaMode;
        gmat.alphaCutoff = mat.alphaCutoff;

        // Export transmission as KHR_materials_transmission if baseAlpha < 1
        if (mat.baseAlpha < 1.0f)
        {
            float transFactor = 1.0f - mat.baseAlpha;
            tinygltf::Value::Object transExt;
            transExt["transmissionFactor"] = tinygltf::Value(static_cast<double>(transFactor));
            gmat.extensions["KHR_materials_transmission"] = tinygltf::Value(transExt);
            if (std::find(model.extensionsUsed.begin(), model.extensionsUsed.end(),
                          "KHR_materials_transmission") == model.extensionsUsed.end())
                model.extensionsUsed.push_back("KHR_materials_transmission");
        }

        // Store custom material type, IOR, and texture indices in extras
        tinygltf::Value::Object matExtras;
        matExtras["materialType"] = tinygltf::Value(static_cast<int>(mat.type));
        matExtras["ior"] = tinygltf::Value(static_cast<double>(mat.ior));
        matExtras["baseColorTextureIndex"] = tinygltf::Value(mat.baseColorTextureIndex);
        matExtras["normalTextureIndex"] = tinygltf::Value(mat.normalTextureIndex);
        matExtras["emissiveTextureIndex"] = tinygltf::Value(mat.emissiveTextureIndex);
        matExtras["emissiveColor"] = tinygltf::Value(tinygltf::Value::Array({
            tinygltf::Value(static_cast<double>(mat.emissiveColor.r)),
            tinygltf::Value(static_cast<double>(mat.emissiveColor.g)),
            tinygltf::Value(static_cast<double>(mat.emissiveColor.b))
        }));
        matExtras["emissiveIntensity"] = tinygltf::Value(static_cast<double>(mat.emissiveIntensity));
        gmat.extras = tinygltf::Value(matExtras);

        model.materials.push_back(gmat);
    }

    // --- Nodes (meshes) ---
    const auto& meshes = scene.GetMeshes();
    std::vector<int> nodeIndices;

    // Helper lambdas for writing accessor/bufferView/buffer data
    auto addBuffer = [&model](const std::vector<unsigned char>& data) -> int {
        tinygltf::Buffer buf;
        buf.data = data;
        model.buffers.push_back(buf);
        return static_cast<int>(model.buffers.size()) - 1;
    };

    auto addBufferView = [&model](int bufIdx, size_t byteLen, int target) -> int {
        tinygltf::BufferView bv;
        bv.buffer = bufIdx;
        bv.byteOffset = 0;
        bv.byteLength = byteLen;
        bv.byteStride = 0;
        bv.target = target;
        model.bufferViews.push_back(bv);
        return static_cast<int>(model.bufferViews.size()) - 1;
    };

    auto addAccessor = [&model](int bvIdx, int componentType, size_t count, int type) -> int {
        tinygltf::Accessor acc;
        acc.bufferView = bvIdx;
        acc.byteOffset = 0;
        acc.componentType = componentType;
        acc.count = count;
        acc.type = type;
        model.accessors.push_back(acc);
        return static_cast<int>(model.accessors.size()) - 1;
    };

    for (const auto& mesh : meshes)
    {
        tinygltf::Mesh gmesh;

        if (mesh.HasGeometry())
        {
            // Real geometry: write POSITION, NORMAL, TEXCOORD, INDICES accessors
            size_t vertCount = mesh.vertices.size() / 3;
            tinygltf::Primitive prim;
            prim.mode = 4;  // TRIANGLES

            // POSITION
            if (!mesh.vertices.empty())
            {
                std::vector<unsigned char> posData(mesh.vertices.size() * sizeof(float));
                std::memcpy(posData.data(), mesh.vertices.data(), posData.size());
                int bufIdx = addBuffer(posData);
                int bvIdx = addBufferView(bufIdx, posData.size(), 34962);  // ARRAY_BUFFER
                int accIdx = addAccessor(bvIdx, 5126, vertCount, 3);  // FLOAT, VEC3
                prim.attributes["POSITION"] = accIdx;
            }

            // NORMAL
            if (!mesh.normals.empty())
            {
                size_t normCount = mesh.normals.size() / 3;
                std::vector<unsigned char> normData(mesh.normals.size() * sizeof(float));
                std::memcpy(normData.data(), mesh.normals.data(), normData.size());
                int bufIdx = addBuffer(normData);
                int bvIdx = addBufferView(bufIdx, normData.size(), 34962);
                int accIdx = addAccessor(bvIdx, 5126, normCount, 3);
                prim.attributes["NORMAL"] = accIdx;
            }

            // TEXCOORD_0
            if (!mesh.uvs.empty())
            {
                size_t uvCount = mesh.uvs.size() / 2;
                std::vector<unsigned char> uvData(mesh.uvs.size() * sizeof(float));
                std::memcpy(uvData.data(), mesh.uvs.data(), uvData.size());
                int bufIdx = addBuffer(uvData);
                int bvIdx = addBufferView(bufIdx, uvData.size(), 34962);
                int accIdx = addAccessor(bvIdx, 5126, uvCount, 2);  // VEC2
                prim.attributes["TEXCOORD_0"] = accIdx;
            }

            // INDICES
            if (!mesh.indices.empty())
            {
                std::vector<unsigned char> idxData(mesh.indices.size() * sizeof(uint32_t));
                std::memcpy(idxData.data(), mesh.indices.data(), idxData.size());
                int bufIdx = addBuffer(idxData);
                int bvIdx = addBufferView(bufIdx, idxData.size(), 34963);  // ELEMENT_ARRAY_BUFFER
                int accIdx = addAccessor(bvIdx, 5125, mesh.indices.size(), 65);  // UNSIGNED_INT, SCALAR
                prim.indices = accIdx;
            }

            // Material
            if (mesh.materialIndex >= 0 && mesh.materialIndex < (int)model.materials.size())
                prim.material = mesh.materialIndex;

            gmesh.primitives.push_back(prim);
        }
        else
        {
            // External filepath (RT2 custom format): store path in mesh name
            gmesh.name = mesh.filepath;
        }

        model.meshes.push_back(gmesh);
        int meshIdx = static_cast<int>(model.meshes.size()) - 1;

        tinygltf::Node node;
        node.mesh = meshIdx;
        node.translation = {mesh.position.x, mesh.position.y, mesh.position.z};
        node.rotation = {0.0, 0.0, 0.0, 1.0};
        node.scale = {mesh.scale, mesh.scale, mesh.scale};

        // Store Euler rotation and material index in extras (for RT2 custom format)
        tinygltf::Value::Object nodeExtras;
        nodeExtras["rotation"] = tinygltf::Value(tinygltf::Value::Array({
            tinygltf::Value(static_cast<double>(mesh.rotation.x)),
            tinygltf::Value(static_cast<double>(mesh.rotation.y)),
            tinygltf::Value(static_cast<double>(mesh.rotation.z))
        }));
        nodeExtras["materialIndex"] = tinygltf::Value(mesh.materialIndex);
        node.extras = tinygltf::Value(nodeExtras);

        model.nodes.push_back(node);
        nodeIndices.push_back(static_cast<int>(model.nodes.size()) - 1);
    }

    // --- Lights ---
    const auto& lights = scene.GetLights();
    if (!lights.empty())
    {
        tinygltf::Value::Object lightsExtObj;
        std::vector<tinygltf::Value> lightsArray;

        for (const auto& light : lights)
        {
            tinygltf::Value::Object lightObj;
            lightObj["color"] = tinygltf::Value(tinygltf::Value::Array({
                tinygltf::Value(light.color.r),
                tinygltf::Value(light.color.g),
                tinygltf::Value(light.color.b)
            }));

            std::string typeStr = (light.type == LightType::Spot) ? "spot" : "point";
            lightObj["type"] = tinygltf::Value(typeStr);
            lightObj["intensity"] = tinygltf::Value(light.intensity);
            lightObj["range"] = tinygltf::Value(light.range);

            lightObj["position"] = tinygltf::Value(tinygltf::Value::Array({
                tinygltf::Value(light.position.x),
                tinygltf::Value(light.position.y),
                tinygltf::Value(light.position.z)
            }));

            lightObj["direction"] = tinygltf::Value(tinygltf::Value::Array({
                tinygltf::Value(light.direction.x),
                tinygltf::Value(light.direction.y),
                tinygltf::Value(light.direction.z)
            }));

            if (light.type == LightType::Spot)
            {
                tinygltf::Value::Object spotObj;
                spotObj["innerConeAngle"] = tinygltf::Value(static_cast<double>(glm::radians(light.innerConeAngle)));
                spotObj["outerConeAngle"] = tinygltf::Value(static_cast<double>(glm::radians(light.outerConeAngle)));
                lightObj["spot"] = tinygltf::Value(spotObj);
            }

            lightObj["innerConeAngleDeg"] = tinygltf::Value(light.innerConeAngle);
            lightObj["outerConeAngleDeg"] = tinygltf::Value(light.outerConeAngle);

            lightsArray.push_back(tinygltf::Value(lightObj));
        }

        lightsExtObj["lights"] = tinygltf::Value(lightsArray);
        model.extensions["KHR_lights_punctual"] = tinygltf::Value(lightsExtObj);
        model.extensionsUsed.push_back("KHR_lights_punctual");
    }

    // --- Camera ---
    const auto& cam = scene.GetCamera();
    {
        tinygltf::Camera gcam;
        gcam.type = "perspective";
        tinygltf::PerspectiveCamera persp;
        persp.yfov = glm::radians(cam.verticalFOV);
        persp.aspectRatio = 16.0 / 9.0;
        persp.znear = 0.1;
        persp.zfar = 100.0;
        gcam.perspective = persp;

        // Store camera position, forward, aperture, focusDistance, FOV in extras
        tinygltf::Value::Object camExtras;
        camExtras["position"] = tinygltf::Value(tinygltf::Value::Array({
            tinygltf::Value(static_cast<double>(cam.position.x)),
            tinygltf::Value(static_cast<double>(cam.position.y)),
            tinygltf::Value(static_cast<double>(cam.position.z))
        }));
        camExtras["forward"] = tinygltf::Value(tinygltf::Value::Array({
            tinygltf::Value(static_cast<double>(cam.forwardDirection.x)),
            tinygltf::Value(static_cast<double>(cam.forwardDirection.y)),
            tinygltf::Value(static_cast<double>(cam.forwardDirection.z))
        }));
        camExtras["aperture"] = tinygltf::Value(static_cast<double>(cam.aperture));
        camExtras["focusDistance"] = tinygltf::Value(static_cast<double>(cam.focusDistance));
        camExtras["verticalFOV"] = tinygltf::Value(static_cast<double>(cam.verticalFOV));
        gcam.extras = tinygltf::Value(camExtras);

        model.cameras.push_back(gcam);

        tinygltf::Node camNode;
        camNode.camera = 0;
        model.nodes.push_back(camNode);
        nodeIndices.push_back(static_cast<int>(model.nodes.size()) - 1);
    }

    // --- Scene ---
    gltfScene.nodes = nodeIndices;
    model.scenes.push_back(gltfScene);
    model.defaultScene = 0;

    // --- Write file ---
    tinygltf::TinyGLTF loader;
    std::string err, warn;

    fs::path fpath(filepath);
    std::string ext = fpath.extension().string();
    bool isGLB = (ext == ".glb" || ext == ".GLB");

    bool ret = loader.WriteGltfSceneToFile(&model, filepath,
        false,  // embedImages
        isGLB,  // embedBuffers (needed for GLB)
        true,   // prettyPrint
        isGLB   // writeBinary
    );

    return ret;
}

// ============================================================================
// Load: file -> tinygltf::Model -> Scene
// ============================================================================

bool SceneLoader::Load(Scene& scene, const std::string& filepath)
{
    if (filepath.empty() || !fs::exists(filepath))
        return false;

    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    loader.SetImageLoader(DecodeImageData, nullptr);
    std::string err, warn;

    fs::path fpath(filepath);
    std::string ext = fpath.extension().string();
    bool isGLB = (ext == ".glb" || ext == ".GLB");

    bool ret;
    if (isGLB)
        ret = loader.LoadBinaryFromFile(&model, &err, &warn, filepath);
    else
        ret = loader.LoadASCIIFromFile(&model, &err, &warn, filepath);

    if (!err.empty())
        printf("[SceneLoader] Error: %s\n", err.c_str());
    if (!warn.empty())
        printf("[SceneLoader] Warning: %s\n", warn.c_str());
    printf("[SceneLoader] Load result: %s, meshes=%d, nodes=%d, materials=%d, accessors=%d\n",
           ret ? "true" : "false", (int)model.meshes.size(), (int)model.nodes.size(),
           (int)model.materials.size(), (int)model.accessors.size());

    if (!ret)
        return false;

    scene.Clear();

    // --- Textures ---
    for (const auto& gtext : model.textures)
    {
        SceneTexture tex;
        if (gtext.source >= 0 && gtext.source < (int)model.images.size())
        {
            const auto& img = model.images[gtext.source];
            tex.filepath = img.uri;

            // Copy decoded RGBA8 pixel data from the tinygltf image
            if (!img.image.empty() && img.width > 0 && img.height > 0)
            {
                tex.width = img.width;
                tex.height = img.height;
                tex.channels = 4;
                tex.pixels.assign(img.image.begin(), img.image.end());
            }
        }
        printf("[SceneLoader] Texture %d: %dx%d RGBA8 (%zu bytes)\n",
               (int)scene.GetTextures().size(), tex.width, tex.height,
               tex.pixels.size());
        scene.AddTexture(tex);
    }

    // --- Materials ---
    for (const auto& gmat : model.materials)
    {
        SceneMaterial mat;

        if (gmat.pbrMetallicRoughness.baseColorFactor.size() >= 4)
        {
            mat.baseColor = {
                (float)gmat.pbrMetallicRoughness.baseColorFactor[0],
                (float)gmat.pbrMetallicRoughness.baseColorFactor[1],
                (float)gmat.pbrMetallicRoughness.baseColorFactor[2]
            };
            mat.baseAlpha = (float)gmat.pbrMetallicRoughness.baseColorFactor[3];
        }
        else if (gmat.pbrMetallicRoughness.baseColorFactor.size() >= 3)
        {
            mat.baseColor = {
                (float)gmat.pbrMetallicRoughness.baseColorFactor[0],
                (float)gmat.pbrMetallicRoughness.baseColorFactor[1],
                (float)gmat.pbrMetallicRoughness.baseColorFactor[2]
            };
        }

        mat.metallic = (float)gmat.pbrMetallicRoughness.metallicFactor;
        mat.roughness = (float)gmat.pbrMetallicRoughness.roughnessFactor;

        if (gmat.pbrMetallicRoughness.baseColorTexture.index >= 0)
            mat.baseColorTextureIndex = gmat.pbrMetallicRoughness.baseColorTexture.index;

        if (gmat.normalTexture.index >= 0)
            mat.normalTextureIndex = gmat.normalTexture.index;

        if (gmat.emissiveTexture.index >= 0)
            mat.emissiveTextureIndex = gmat.emissiveTexture.index;

        if (gmat.emissiveFactor.size() >= 3)
        {
            float r = (float)gmat.emissiveFactor[0];
            float g = (float)gmat.emissiveFactor[1];
            float b = (float)gmat.emissiveFactor[2];
            float maxComp = std::max({r, g, b});
            if (maxComp > 0.0f)
            {
                mat.emissiveColor = {r / maxComp, g / maxComp, b / maxComp};
                mat.emissiveIntensity = maxComp;
            }
        }

        auto emissiveStrengthIt = gmat.extensions.find("KHR_materials_emissive_strength");
        if (emissiveStrengthIt != gmat.extensions.end() && emissiveStrengthIt->second.IsObject())
        {
            const tinygltf::Value& strengthExt = emissiveStrengthIt->second;
            if (strengthExt.Has("emissiveStrength"))
                mat.emissiveIntensity *= (float)strengthExt.Get("emissiveStrength").GetNumberAsDouble();
        }

        // glTF alpha mode and cutoff
        mat.alphaMode = gmat.alphaMode;
        mat.alphaCutoff = (float)gmat.alphaCutoff;

        // KHR_materials_transmission: physical glass (transmissionFactor 0..1)
        // Treat as BLEND with baseAlpha = 1 - transmissionFactor
        auto transmissionIt = gmat.extensions.find("KHR_materials_transmission");
        if (transmissionIt != gmat.extensions.end() && transmissionIt->second.IsObject())
        {
            const tinygltf::Value& transExt = transmissionIt->second;
            if (transExt.Has("transmissionFactor"))
            {
                float transFactor = (float)transExt.Get("transmissionFactor").GetNumberAsDouble();
                if (transFactor > 0.0f)
                {
                    mat.alphaMode = "BLEND";
                    mat.baseAlpha = 1.0f - transFactor;
                    if (mat.baseAlpha < 0.0f) mat.baseAlpha = 0.0f;
                    if (mat.baseAlpha > 1.0f) mat.baseAlpha = 1.0f;
                }
            }
        }

        if (gmat.extras.Has("materialType"))
        {
            int typeInt = (int)gmat.extras.Get("materialType").GetNumberAsInt();
            mat.type = static_cast<MaterialType>(typeInt);
        }
        if (gmat.extras.Has("ior"))
        {
            mat.ior = (float)gmat.extras.Get("ior").GetNumberAsDouble();
        }
        if (gmat.extras.Has("baseColorTextureIndex"))
        {
            mat.baseColorTextureIndex = (int)gmat.extras.Get("baseColorTextureIndex").GetNumberAsInt();
        }
        if (gmat.extras.Has("normalTextureIndex"))
        {
            mat.normalTextureIndex = (int)gmat.extras.Get("normalTextureIndex").GetNumberAsInt();
        }
        if (gmat.extras.Has("emissiveTextureIndex"))
        {
            mat.emissiveTextureIndex = (int)gmat.extras.Get("emissiveTextureIndex").GetNumberAsInt();
        }
        if (gmat.extras.Has("emissiveColor") && gmat.extras.Get("emissiveColor").IsArray())
        {
            const tinygltf::Value& emArr = gmat.extras.Get("emissiveColor");
            if (emArr.ArrayLen() >= 3)
            {
                mat.emissiveColor = {
                    (float)emArr.Get(0).GetNumberAsDouble(),
                    (float)emArr.Get(1).GetNumberAsDouble(),
                    (float)emArr.Get(2).GetNumberAsDouble()
                };
            }
        }
        if (gmat.extras.Has("emissiveIntensity"))
        {
            mat.emissiveIntensity = (float)gmat.extras.Get("emissiveIntensity").GetNumberAsDouble();
        }

        printf("[SceneLoader]   Material %d: baseColor=(%.2f,%.2f,%.2f) metallic=%.2f rough=%.2f ior=%.2f"
               " emissive=(%.2f,%.2f,%.2f)*%.2f"
               " texIdx: baseColor=%d normal=%d emissive=%d"
               " alpha=%s cutoff=%.2f\n",
               (int)scene.GetMaterials().size(),
               mat.baseColor.x, mat.baseColor.y, mat.baseColor.z,
               mat.metallic, mat.roughness, mat.ior,
               mat.emissiveColor.x, mat.emissiveColor.y, mat.emissiveColor.z,
               mat.emissiveIntensity,
               mat.baseColorTextureIndex, mat.normalTextureIndex, mat.emissiveTextureIndex,
               mat.alphaMode.c_str(), mat.alphaCutoff);

        scene.AddMaterial(mat);
    }

    // --- Lights ---
    auto lightsIt = model.extensions.find("KHR_lights_punctual");
    if (lightsIt != model.extensions.end() && lightsIt->second.IsObject())
    {
        const tinygltf::Value& lightsExt = lightsIt->second;
        if (lightsExt.Has("lights") && lightsExt.Get("lights").IsArray())
        {
            const tinygltf::Value& lightsArray = lightsExt.Get("lights");
            for (int i = 0; i < lightsArray.ArrayLen(); i++)
            {
                const tinygltf::Value& lightVal = lightsArray.Get(i);
                if (!lightVal.IsObject())
                    continue;

                SceneLight light;

                if (lightVal.Has("type") && lightVal.Get("type").IsString())
                {
                    std::string typeStr = lightVal.Get("type").Get<std::string>();
                    if (typeStr == "spot")
                        light.type = LightType::Spot;
                    else
                        light.type = LightType::Point;
                }

                if (lightVal.Has("color") && lightVal.Get("color").IsArray())
                {
                    const tinygltf::Value& colorArr = lightVal.Get("color");
                    if (colorArr.ArrayLen() >= 3)
                    {
                        light.color = {
                            (float)colorArr.Get(0).GetNumberAsDouble(),
                            (float)colorArr.Get(1).GetNumberAsDouble(),
                            (float)colorArr.Get(2).GetNumberAsDouble()
                        };
                    }
                }

                if (lightVal.Has("intensity"))
                    light.intensity = (float)lightVal.Get("intensity").GetNumberAsDouble();

                if (lightVal.Has("range"))
                    light.range = (float)lightVal.Get("range").GetNumberAsDouble();

                if (lightVal.Has("position") && lightVal.Get("position").IsArray())
                {
                    const tinygltf::Value& posArr = lightVal.Get("position");
                    if (posArr.ArrayLen() >= 3)
                    {
                        light.position = {
                            (float)posArr.Get(0).GetNumberAsDouble(),
                            (float)posArr.Get(1).GetNumberAsDouble(),
                            (float)posArr.Get(2).GetNumberAsDouble()
                        };
                    }
                }

                if (lightVal.Has("direction") && lightVal.Get("direction").IsArray())
                {
                    const tinygltf::Value& dirArr = lightVal.Get("direction");
                    if (dirArr.ArrayLen() >= 3)
                    {
                        light.direction = {
                            (float)dirArr.Get(0).GetNumberAsDouble(),
                            (float)dirArr.Get(1).GetNumberAsDouble(),
                            (float)dirArr.Get(2).GetNumberAsDouble()
                        };
                    }
                }

                if (lightVal.Has("innerConeAngleDeg"))
                    light.innerConeAngle = (float)lightVal.Get("innerConeAngleDeg").GetNumberAsDouble();
                else if (lightVal.Has("spot") && lightVal.Get("spot").IsObject())
                {
                    const tinygltf::Value& spotObj = lightVal.Get("spot");
                    if (spotObj.Has("innerConeAngle"))
                        light.innerConeAngle = glm::degrees((float)spotObj.Get("innerConeAngle").GetNumberAsDouble());
                    if (spotObj.Has("outerConeAngle"))
                        light.outerConeAngle = glm::degrees((float)spotObj.Get("outerConeAngle").GetNumberAsDouble());
                }

                if (lightVal.Has("outerConeAngleDeg"))
                    light.outerConeAngle = (float)lightVal.Get("outerConeAngleDeg").GetNumberAsDouble();

                scene.AddLight(light);
            }
        }
    }

    // --- Node traversal (meshes + camera) ---
    // Traverse from scene root nodes recursively, accumulating parent transforms

    // Helper: compute local transform matrix from a node (TRS or matrix)
    auto computeLocalMatrix = [](const tinygltf::Node& node) -> glm::mat4 {
        if (node.matrix.size() == 16)
        {
            return glm::make_mat4(node.matrix.data());
        }

        glm::vec3 T(0.0f);
        glm::quat R(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 S(1.0f);

        if (node.translation.size() >= 3)
            T = glm::vec3((float)node.translation[0], (float)node.translation[1], (float)node.translation[2]);
        if (node.rotation.size() >= 4)
        {
            // glTF quaternion is [x, y, z, w]; GLM wants (w, x, y, z)
            R = glm::quat((float)node.rotation[3], (float)node.rotation[0],
                          (float)node.rotation[1], (float)node.rotation[2]);
        }
        if (node.scale.size() >= 3)
            S = glm::vec3((float)node.scale[0], (float)node.scale[1], (float)node.scale[2]);

        return glm::translate(glm::mat4(1.0f), T) * glm::mat4(R) * glm::scale(glm::mat4(1.0f), S);
    };

    // Helper: extract a glm::vec3 array from an accessor
    auto readVec3Accessor = [](const tinygltf::Model& model, int accessorIdx) -> std::vector<glm::vec3> {
        std::vector<glm::vec3> out;
        if (accessorIdx < 0 || accessorIdx >= (int)model.accessors.size())
            return out;
        const tinygltf::Accessor& acc = model.accessors[accessorIdx];
        if (acc.bufferView < 0 || acc.bufferView >= (int)model.bufferViews.size())
            return out;
        const tinygltf::BufferView& bv = model.bufferViews[acc.bufferView];
        if (bv.buffer < 0 || bv.buffer >= (int)model.buffers.size())
            return out;
        const tinygltf::Buffer& buf = model.buffers[bv.buffer];
        int stride = acc.ByteStride(bv);
        if (stride < 0)
            return out;
        const unsigned char* data = buf.data.data() + bv.byteOffset + acc.byteOffset;
        out.resize(acc.count);
        for (size_t i = 0; i < acc.count; i++)
        {
            const float* f = reinterpret_cast<const float*>(data + i * stride);
            out[i] = glm::vec3(f[0], f[1], f[2]);
        }
        return out;
    };

    // Helper: extract a glm::vec2 array from an accessor (handles normalized integer types)
    auto readVec2Accessor = [](const tinygltf::Model& model, int accessorIdx) -> std::vector<glm::vec2> {
        std::vector<glm::vec2> out;
        if (accessorIdx < 0 || accessorIdx >= (int)model.accessors.size())
            return out;
        const tinygltf::Accessor& acc = model.accessors[accessorIdx];
        if (acc.bufferView < 0 || acc.bufferView >= (int)model.bufferViews.size())
            return out;
        const tinygltf::BufferView& bv = model.bufferViews[acc.bufferView];
        if (bv.buffer < 0 || bv.buffer >= (int)model.buffers.size())
            return out;
        const tinygltf::Buffer& buf = model.buffers[bv.buffer];
        int stride = acc.ByteStride(bv);
        if (stride < 0)
            return out;
        const unsigned char* data = buf.data.data() + bv.byteOffset + acc.byteOffset;
        out.resize(acc.count);
        for (size_t i = 0; i < acc.count; i++)
        {
            const unsigned char* p = data + i * stride;
            if (acc.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT)
            {
                const float* f = reinterpret_cast<const float*>(p);
                out[i] = glm::vec2(f[0], f[1]);
            }
            else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
            {
                const uint8_t* b = reinterpret_cast<const uint8_t*>(p);
                if (acc.normalized)
                {
                    out[i] = glm::vec2(b[0] / 255.0f, b[1] / 255.0f);
                }
                else
                {
                    out[i] = glm::vec2((float)b[0], (float)b[1]);
                }
            }
            else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
            {
                const uint16_t* s = reinterpret_cast<const uint16_t*>(p);
                if (acc.normalized)
                {
                    out[i] = glm::vec2(s[0] / 65535.0f, s[1] / 65535.0f);
                }
                else
                {
                    out[i] = glm::vec2((float)s[0], (float)s[1]);
                }
            }
            else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_SHORT)
            {
                const int16_t* s = reinterpret_cast<const int16_t*>(p);
                if (acc.normalized)
                {
                    out[i] = glm::vec2(std::max(-1.0f, s[0] / 32767.0f), std::max(-1.0f, s[1] / 32767.0f));
                }
                else
                {
                    out[i] = glm::vec2((float)s[0], (float)s[1]);
                }
            }
        }
        return out;
    };

    // Helper: extract uint32 indices from an accessor (widens u8/u16 to u32)
    auto readIndices = [](const tinygltf::Model& model, int accessorIdx) -> std::vector<uint32_t> {
        std::vector<uint32_t> out;
        if (accessorIdx < 0 || accessorIdx >= (int)model.accessors.size())
            return out;
        const tinygltf::Accessor& acc = model.accessors[accessorIdx];
        if (acc.bufferView < 0 || acc.bufferView >= (int)model.bufferViews.size())
            return out;
        const tinygltf::BufferView& bv = model.bufferViews[acc.bufferView];
        if (bv.buffer < 0 || bv.buffer >= (int)model.buffers.size())
            return out;
        const tinygltf::Buffer& buf = model.buffers[bv.buffer];
        int stride = acc.ByteStride(bv);
        if (stride < 0)
            return out;
        const unsigned char* data = buf.data.data() + bv.byteOffset + acc.byteOffset;
        out.resize(acc.count);
        for (size_t i = 0; i < acc.count; i++)
        {
            const unsigned char* p = data + i * stride;
            if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
                out[i] = *reinterpret_cast<const uint8_t*>(p);
            else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
                out[i] = *reinterpret_cast<const uint16_t*>(p);
            else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT)
                out[i] = *reinterpret_cast<const uint32_t*>(p);
            else
                out[i] = 0;
        }
        return out;
    };

    // Helper: decompose a mat4 into translation, Euler rotation (radians), and uniform scale
    auto decomposeMatrix = [](const glm::mat4& m, glm::vec3& outPos, glm::vec3& outRot, float& outScale) {
        outPos = glm::vec3(m[3]);  // translation from last column
        // Extract scale
        float scaleX = glm::length(glm::vec3(m[0]));
        float scaleY = glm::length(glm::vec3(m[1]));
        float scaleZ = glm::length(glm::vec3(m[2]));
        outScale = scaleX;  // use first component for uniform

        // Build rotation matrix (remove scale)
        glm::mat3 rotMat(
            glm::vec3(m[0]) / scaleX,
            glm::vec3(m[1]) / scaleY,
            glm::vec3(m[2]) / scaleZ
        );

        // Extract Euler angles (XYZ order) from rotation matrix
        outRot.y = glm::asin(-rotMat[0][2]);  // pitch (Y)
        if (glm::cos(outRot.y) > 0.0001f)
        {
            outRot.x = glm::atan(rotMat[1][2], rotMat[2][2]);  // roll (X)
            outRot.z = glm::atan(rotMat[0][1], rotMat[0][0]);  // yaw (Z)
        }
        else
        {
            outRot.x = glm::atan(-rotMat[2][0], rotMat[1][1]);
            outRot.z = 0.0f;
        }
    };

    // Recursive node traversal function
    std::function<void(int, const glm::mat4&)> traverseNode = [&](int nodeIdx, const glm::mat4& parentWorld) {
        if (nodeIdx < 0 || nodeIdx >= (int)model.nodes.size())
            return;
        const tinygltf::Node& node = model.nodes[nodeIdx];

        glm::mat4 localMat = computeLocalMatrix(node);
        glm::mat4 worldMat = parentWorld * localMat;

        // Handle camera nodes
        if (node.camera >= 0 && node.camera < (int)model.cameras.size())
        {
            const tinygltf::Camera& gcam = model.cameras[node.camera];
            SceneCamera& cam = scene.GetCamera();
            cam.verticalFOV = glm::degrees((float)gcam.perspective.yfov);

            // Extract camera position from world matrix
            glm::vec3 camPos = glm::vec3(worldMat[3]);
            cam.position = camPos;

            // Forward direction is -Z in glTF camera space
            glm::vec3 camForward = glm::normalize(glm::vec3(worldMat * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
            cam.forwardDirection = camForward;

            // Read extras for custom camera data
            if (gcam.extras.Has("position") && gcam.extras.Get("position").IsArray())
            {
                const tinygltf::Value& posArr = gcam.extras.Get("position");
                if (posArr.ArrayLen() >= 3)
                {
                    cam.position = {
                        (float)posArr.Get(0).GetNumberAsDouble(),
                        (float)posArr.Get(1).GetNumberAsDouble(),
                        (float)posArr.Get(2).GetNumberAsDouble()
                    };
                }
            }
            if (gcam.extras.Has("forward") && gcam.extras.Get("forward").IsArray())
            {
                const tinygltf::Value& fwdArr = gcam.extras.Get("forward");
                if (fwdArr.ArrayLen() >= 3)
                {
                    cam.forwardDirection = {
                        (float)fwdArr.Get(0).GetNumberAsDouble(),
                        (float)fwdArr.Get(1).GetNumberAsDouble(),
                        (float)fwdArr.Get(2).GetNumberAsDouble()
                    };
                }
            }
            if (gcam.extras.Has("aperture"))
                cam.aperture = (float)gcam.extras.Get("aperture").GetNumberAsDouble();
            if (gcam.extras.Has("focusDistance"))
                cam.focusDistance = (float)gcam.extras.Get("focusDistance").GetNumberAsDouble();
            if (gcam.extras.Has("verticalFOV"))
                cam.verticalFOV = (float)gcam.extras.Get("verticalFOV").GetNumberAsDouble();
        }

        // Handle mesh nodes
        if (node.mesh >= 0 && node.mesh < (int)model.meshes.size())
        {
            const tinygltf::Mesh& gmesh = model.meshes[node.mesh];

            // Decompose world matrix into TRS for SceneMesh
            glm::vec3 worldPos, worldRot;
            float worldScale;
            decomposeMatrix(worldMat, worldPos, worldRot, worldScale);

            // Check if this is an RT2 custom-format file (no primitives, just mesh.name as filepath)
            bool hasRealGeometry = !gmesh.primitives.empty();

            if (!hasRealGeometry)
            {
                // RT2 custom format: mesh.name is an external OBJ filepath
                SceneMesh mesh;
                mesh.filepath = gmesh.name;
                mesh.position = worldPos;
                mesh.scale = worldScale;

                // Check for custom Euler rotation in extras (RT2 format)
                if (node.extras.Has("rotation") && node.extras.Get("rotation").IsArray())
                {
                    const tinygltf::Value& rotArr = node.extras.Get("rotation");
                    if (rotArr.ArrayLen() >= 3)
                    {
                        mesh.rotation = {
                            (float)rotArr.Get(0).GetNumberAsDouble(),
                            (float)rotArr.Get(1).GetNumberAsDouble(),
                            (float)rotArr.Get(2).GetNumberAsDouble()
                        };
                    }
                }
                else
                {
                    mesh.rotation = worldRot;  // from matrix decomposition
                }

                if (node.extras.Has("materialIndex"))
                    mesh.materialIndex = (int)node.extras.Get("materialIndex").GetNumberAsInt();
                else
                    mesh.materialIndex = 0;

                scene.AddMesh(mesh);
            }
            else
            {
                // Real glTF: extract geometry from each primitive
                for (const tinygltf::Primitive& prim : gmesh.primitives)
                {
                    // Skip non-triangle primitives
                    int mode = (prim.mode >= 0) ? prim.mode : 4;  // default = TRIANGLES
                    // printf("[SceneLoader] Primitive: mode=%d, indices=%d, material=%d\n",
                    //        mode, prim.indices, prim.material);
                    if (mode != 4)  // TINYGLTF_MODE_TRIANGLES
                    {
                        printf("[SceneLoader]   Skipping non-triangle primitive (mode %d)\n", mode);
                        continue;
                    }

                    // POSITION is required
                    auto posIt = prim.attributes.find("POSITION");
                    if (posIt == prim.attributes.end())
                        continue;

                    std::vector<glm::vec3> positions = readVec3Accessor(model, posIt->second);
                    if (positions.empty())
                        continue;

                    // NORMAL (optional)
                    std::vector<glm::vec3> normals;
                    auto nrmIt = prim.attributes.find("NORMAL");
                    if (nrmIt != prim.attributes.end())
                        normals = readVec3Accessor(model, nrmIt->second);

                    // TEXCOORD_0 (optional)
                    std::vector<glm::vec2> uvs;
                    auto uvIt = prim.attributes.find("TEXCOORD_0");
                    if (uvIt != prim.attributes.end())
                        uvs = readVec2Accessor(model, uvIt->second);

                    // INDICES (optional — if missing, generate sequential)
                    std::vector<uint32_t> indices;
                    if (prim.indices >= 0)
                    {
                        indices = readIndices(model, prim.indices);
                    }
                    else
                    {
                        // Non-indexed: generate 0..N-1
                        size_t vertCount = positions.size();
                        // Must be divisible by 3 for triangles
                        size_t triCount = vertCount / 3;
                        indices.resize(triCount * 3);
                        for (size_t i = 0; i < triCount * 3; i++)
                            indices[i] = static_cast<uint32_t>(i);
                    }

                    // Build SceneMesh — vertices are pre-transformed to world space
                    SceneMesh mesh;
                    mesh.hasGeometry = true;
                    mesh.position = glm::vec3(0.0f);
                    mesh.rotation = glm::vec3(0.0f);
                    mesh.scale = 1.0f;
                    mesh.materialIndex = (prim.material >= 0) ? prim.material : 0;

                    // Transform positions to world space via the node's world matrix
                    mesh.vertices.reserve(positions.size() * 3);
                    for (const auto& p : positions)
                    {
                        glm::vec4 worldPos4 = worldMat * glm::vec4(p, 1.0f);
                        mesh.vertices.push_back(worldPos4.x);
                        mesh.vertices.push_back(worldPos4.y);
                        mesh.vertices.push_back(worldPos4.z);
                    }

                    // Transform normals to world space (inverse-transpose of 3x3)
                    if (!normals.empty())
                    {
                        glm::mat3 normalMat = glm::transpose(glm::inverse(glm::mat3(worldMat)));
                        mesh.normals.reserve(normals.size() * 3);
                        for (const auto& n : normals)
                        {
                            glm::vec3 wn = glm::normalize(normalMat * n);
                            mesh.normals.push_back(wn.x);
                            mesh.normals.push_back(wn.y);
                            mesh.normals.push_back(wn.z);
                        }
                    }

                    // Flatten UVs
                    if (!uvs.empty())
                    {
                        mesh.uvs.reserve(uvs.size() * 2);
                        for (const auto& uv : uvs)
                        {
                            mesh.uvs.push_back(uv.x);
                            mesh.uvs.push_back(uv.y);
                        }
                    }

                    // Indices
                    mesh.indices = std::move(indices);

                    scene.AddMesh(mesh);
                }
            }
        }

        // Recurse into children
        for (int childIdx : node.children)
            traverseNode(childIdx, worldMat);
    };

    // Start traversal from scene root nodes
    if (model.defaultScene >= 0 && model.defaultScene < (int)model.scenes.size())
    {
        const tinygltf::Scene& gltfScene = model.scenes[model.defaultScene];
        for (int rootIdx : gltfScene.nodes)
            traverseNode(rootIdx, glm::mat4(1.0f));
    }
    else if (!model.scenes.empty())
    {
        // Fallback: use scene 0
        for (int rootIdx : model.scenes[0].nodes)
            traverseNode(rootIdx, glm::mat4(1.0f));
    }
    else
    {
        // Fallback: no scenes, iterate all nodes flat (legacy)
        for (int i = 0; i < (int)model.nodes.size(); i++)
            traverseNode(i, glm::mat4(1.0f));
    }

    return true;
}
