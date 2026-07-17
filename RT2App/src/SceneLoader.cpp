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
#include <unordered_map>
#include <cmath>
#include <unordered_map>

#include "SceneGraph.h"
#include "ECSScene.h"

namespace fs = std::filesystem;

// ============================================================================
// Save: ECSScene -> tinygltf::Model -> file
// ============================================================================

bool SceneLoader::Save(const ECSScene& ecsScene, const std::string& filepath)
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
    for (const auto& tex : ecsScene.textures)
    {
        tinygltf::Image image;
        image.uri = tex.filepath;
        model.images.push_back(image);

        tinygltf::Texture gtext;
        gtext.source = static_cast<int>(model.images.size()) - 1;
        model.textures.push_back(gtext);
    }

    // --- Materials ---
    for (const auto& mat : ecsScene.materials)
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

        if (mat.metallicRoughnessTextureIndex >= 0 && mat.metallicRoughnessTextureIndex < (int)model.textures.size())
        {
            gmat.pbrMetallicRoughness.metallicRoughnessTexture.index = mat.metallicRoughnessTextureIndex;
            gmat.pbrMetallicRoughness.metallicRoughnessTexture.texCoord = 0;
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

        gmat.alphaMode = mat.alphaMode;
        gmat.alphaCutoff = mat.alphaCutoff;

        if (mat.transmissionFactor > 0.0f)
        {
            tinygltf::Value::Object transExt;
            transExt["transmissionFactor"] = tinygltf::Value(static_cast<double>(mat.transmissionFactor));
            gmat.extensions["KHR_materials_transmission"] = tinygltf::Value(transExt);
            if (std::find(model.extensionsUsed.begin(), model.extensionsUsed.end(),
                          "KHR_materials_transmission") == model.extensionsUsed.end())
                model.extensionsUsed.push_back("KHR_materials_transmission");
        }

        tinygltf::Value::Object matExtras;
        matExtras["materialType"] = tinygltf::Value(static_cast<int>(mat.type));
        matExtras["ior"] = tinygltf::Value(static_cast<double>(mat.ior));
        matExtras["baseColorTextureIndex"] = tinygltf::Value(mat.baseColorTextureIndex);
        matExtras["normalTextureIndex"] = tinygltf::Value(mat.normalTextureIndex);
        matExtras["emissiveTextureIndex"] = tinygltf::Value(mat.emissiveTextureIndex);
        matExtras["metallicRoughnessTextureIndex"] = tinygltf::Value(mat.metallicRoughnessTextureIndex);
        matExtras["emissiveColor"] = tinygltf::Value(tinygltf::Value::Array({
            tinygltf::Value(static_cast<double>(mat.emissiveColor.r)),
            tinygltf::Value(static_cast<double>(mat.emissiveColor.g)),
            tinygltf::Value(static_cast<double>(mat.emissiveColor.b))
        }));
        matExtras["emissiveIntensity"] = tinygltf::Value(static_cast<double>(mat.emissiveIntensity));
        gmat.extras = tinygltf::Value(matExtras);

        model.materials.push_back(gmat);
    }

    // --- Nodes (from entities with MeshRef) ---
    std::vector<int> nodeIndices;

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

    auto meshView = ecsScene.registry.view<MeshRef, Transform>();
    for (auto entity : meshView)
    {
        const auto& ref = meshView.get<MeshRef>(entity);
        const auto& tf = meshView.get<Transform>(entity);

        if (ref.meshIndex >= ecsScene.meshRegistry.GetCount())
            continue;

        const auto& meshData = ecsScene.meshRegistry.GetMesh(ref.meshIndex);

        tinygltf::Mesh gmesh;

        if (!meshData.vertices.empty() && !meshData.indices.empty())
        {
            size_t vertCount = meshData.vertices.size() / 3;
            tinygltf::Primitive prim;
            prim.mode = 4;

            // POSITION
            {
                std::vector<unsigned char> posData(meshData.vertices.size() * sizeof(float));
                std::memcpy(posData.data(), meshData.vertices.data(), posData.size());
                int bufIdx = addBuffer(posData);
                int bvIdx = addBufferView(bufIdx, posData.size(), 34962);
                int accIdx = addAccessor(bvIdx, 5126, vertCount, 3);
                prim.attributes["POSITION"] = accIdx;
            }

            // NORMAL
            if (!meshData.normals.empty())
            {
                size_t normCount = meshData.normals.size() / 3;
                std::vector<unsigned char> normData(meshData.normals.size() * sizeof(float));
                std::memcpy(normData.data(), meshData.normals.data(), normData.size());
                int bufIdx = addBuffer(normData);
                int bvIdx = addBufferView(bufIdx, normData.size(), 34962);
                int accIdx = addAccessor(bvIdx, 5126, normCount, 3);
                prim.attributes["NORMAL"] = accIdx;
            }

            // TEXCOORD_0
            if (!meshData.uvs.empty())
            {
                size_t uvCount = meshData.uvs.size() / 2;
                std::vector<unsigned char> uvData(meshData.uvs.size() * sizeof(float));
                std::memcpy(uvData.data(), meshData.uvs.data(), uvData.size());
                int bufIdx = addBuffer(uvData);
                int bvIdx = addBufferView(bufIdx, uvData.size(), 34962);
                int accIdx = addAccessor(bvIdx, 5126, uvCount, 2);
                prim.attributes["TEXCOORD_0"] = accIdx;
            }

            // INDICES
            {
                std::vector<unsigned char> idxData(meshData.indices.size() * sizeof(uint32_t));
                std::memcpy(idxData.data(), meshData.indices.data(), idxData.size());
                int bufIdx = addBuffer(idxData);
                int bvIdx = addBufferView(bufIdx, idxData.size(), 34963);
                int accIdx = addAccessor(bvIdx, 5125, meshData.indices.size(), 65);
                prim.indices = accIdx;
            }

            if (ref.materialIndex >= 0 && ref.materialIndex < (int)model.materials.size())
                prim.material = ref.materialIndex;

            gmesh.primitives.push_back(prim);
        }

        gmesh.name = meshData.name;
        model.meshes.push_back(gmesh);
        int meshIdx = static_cast<int>(model.meshes.size()) - 1;

        tinygltf::Node node;
        node.mesh = meshIdx;
        node.translation = {tf.translation.x, tf.translation.y, tf.translation.z};
        glm::quat q = tf.rotation;
        node.rotation = {q.x, q.y, q.z, q.w};
        node.scale = {tf.scale.x, tf.scale.y, tf.scale.z};

        tinygltf::Value::Object nodeExtras;
        nodeExtras["materialIndex"] = tinygltf::Value(ref.materialIndex);
        node.extras = tinygltf::Value(nodeExtras);

        model.nodes.push_back(node);
        nodeIndices.push_back(static_cast<int>(model.nodes.size()) - 1);
    }

    // --- Lights ---
    if (!ecsScene.lights.empty())
    {
        tinygltf::Value::Object lightsExtObj;
        std::vector<tinygltf::Value> lightsArray;

        for (const auto& light : ecsScene.lights)
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
    const auto& cam = ecsScene.camera;
    {
        tinygltf::Camera gcam;
        gcam.type = "perspective";
        tinygltf::PerspectiveCamera persp;
        persp.yfov = glm::radians(cam.verticalFOV);
        persp.aspectRatio = 16.0 / 9.0;
        persp.znear = 0.1;
        persp.zfar = 10000.0;
        gcam.perspective = persp;

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
// LoadIntoECS — ECS-based scene loading
//
// Populates an ECSScene with object-space meshes + per-entity transforms.
// Meshes are stored in the MeshRegistry (unique geometry, deduplicated by
// glTF mesh index). Entities get Transform, MeshRef, and Hierarchy components.
// The SceneGraph system resolves the hierarchy to world matrices.
//
// ============================================================================

bool SceneLoader::LoadIntoECS(ECSScene& ecsScene, const std::string& filepath)
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

    if (!ret)
        return false;

    ecsScene.Clear();
    // --- Textures (same as Load) ---
    for (const auto& gtext : model.textures)
    {
        SceneTexture tex;
        if (gtext.source >= 0 && gtext.source < (int)model.images.size())
        {
            const auto& img = model.images[gtext.source];
            tex.filepath = img.uri;
            if (!img.image.empty() && img.width > 0 && img.height > 0)
            {
                tex.width = img.width;
                tex.height = img.height;
                tex.channels = 4;
                tex.pixels.assign(img.image.begin(), img.image.end());
            }
        }
        ecsScene.textures.push_back(tex);
    }

    // --- Materials (same as Load) ---
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
        if (gmat.pbrMetallicRoughness.metallicRoughnessTexture.index >= 0)
            mat.metallicRoughnessTextureIndex = gmat.pbrMetallicRoughness.metallicRoughnessTexture.index;
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

        mat.alphaMode = gmat.alphaMode;
        mat.alphaCutoff = (float)gmat.alphaCutoff;

        auto transmissionIt = gmat.extensions.find("KHR_materials_transmission");
        if (transmissionIt != gmat.extensions.end() && transmissionIt->second.IsObject())
        {
            const tinygltf::Value& transExt = transmissionIt->second;
            if (transExt.Has("transmissionFactor"))
            {
                float transFactor = (float)transExt.Get("transmissionFactor").GetNumberAsDouble();
                if (transFactor > 0.0f)
                {
                    mat.transmissionFactor = transFactor;
                    if (mat.transmissionFactor > 1.0f) mat.transmissionFactor = 1.0f;
                }
            }
        }

        if (gmat.extras.Has("ior"))
            mat.ior = (float)gmat.extras.Get("ior").GetNumberAsDouble();

        if (gmat.extras.Has("materialType"))
            mat.type = static_cast<MaterialType>((int)gmat.extras.Get("materialType").GetNumberAsDouble());

        if (gmat.extras.Has("baseColorTextureIndex"))
            mat.baseColorTextureIndex = (int)gmat.extras.Get("baseColorTextureIndex").GetNumberAsDouble();
        if (gmat.extras.Has("normalTextureIndex"))
            mat.normalTextureIndex = (int)gmat.extras.Get("normalTextureIndex").GetNumberAsDouble();
        if (gmat.extras.Has("emissiveTextureIndex"))
            mat.emissiveTextureIndex = (int)gmat.extras.Get("emissiveTextureIndex").GetNumberAsDouble();
        if (gmat.extras.Has("metallicRoughnessTextureIndex"))
            mat.metallicRoughnessTextureIndex = (int)gmat.extras.Get("metallicRoughnessTextureIndex").GetNumberAsDouble();

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
            mat.emissiveIntensity = (float)gmat.extras.Get("emissiveIntensity").GetNumberAsDouble();

        ecsScene.materials.push_back(mat);
    }

    // If no materials, add a default so index 0 is always valid
    if (ecsScene.materials.empty())
        ecsScene.materials.push_back(SceneMaterial{});

    // Mark sRGB textures — base color and emissive are perceptual color data.
    // Normal, metallicRoughness, and other data textures stay linear.
    for (const auto& mat : ecsScene.materials)
    {
        if (mat.baseColorTextureIndex >= 0 && mat.baseColorTextureIndex < (int)ecsScene.textures.size())
            ecsScene.textures[mat.baseColorTextureIndex].isSRGB = true;
        if (mat.emissiveTextureIndex >= 0 && mat.emissiveTextureIndex < (int)ecsScene.textures.size())
            ecsScene.textures[mat.emissiveTextureIndex].isSRGB = true;
    }

    // --- Lights (KHR_lights_punctual) ---
    auto lightsIt = model.extensions.find("KHR_lights_punctual");
    if (lightsIt != model.extensions.end() && lightsIt->second.IsObject())
    {
        const tinygltf::Value& lightsExt = lightsIt->second;
        if (lightsExt.Has("lights") && lightsExt.Get("lights").IsArray())
        {
            const tinygltf::Value& lightsArray = lightsExt.Get("lights");
            for (size_t i = 0; i < lightsArray.ArrayLen(); i++)
            {
                const tinygltf::Value& lightObj = lightsArray.Get(i);
                if (!lightObj.IsObject())
                    continue;

                SceneLight light;
                std::string typeStr = lightObj.Has("type") ? lightObj.Get("type").Get<std::string>() : "point";
                if (typeStr == "spot")
                    light.type = LightType::Spot;
                else
                    light.type = LightType::Point;

                if (lightObj.Has("color") && lightObj.Get("color").IsArray())
                {
                    const tinygltf::Value& colArr = lightObj.Get("color");
                    if (colArr.ArrayLen() >= 3)
                    {
                        light.color = {
                            (float)colArr.Get(0).GetNumberAsDouble(),
                            (float)colArr.Get(1).GetNumberAsDouble(),
                            (float)colArr.Get(2).GetNumberAsDouble()
                        };
                    }
                }
                if (lightObj.Has("intensity"))
                    light.intensity = (float)lightObj.Get("intensity").GetNumberAsDouble();
                if (lightObj.Has("range"))
                    light.range = (float)lightObj.Get("range").GetNumberAsDouble();

                if (lightObj.Has("position") && lightObj.Get("position").IsArray())
                {
                    const tinygltf::Value& posArr = lightObj.Get("position");
                    if (posArr.ArrayLen() >= 3)
                    {
                        light.position = {
                            (float)posArr.Get(0).GetNumberAsDouble(),
                            (float)posArr.Get(1).GetNumberAsDouble(),
                            (float)posArr.Get(2).GetNumberAsDouble()
                        };
                    }
                }
                if (lightObj.Has("direction") && lightObj.Get("direction").IsArray())
                {
                    const tinygltf::Value& dirArr = lightObj.Get("direction");
                    if (dirArr.ArrayLen() >= 3)
                    {
                        light.direction = {
                            (float)dirArr.Get(0).GetNumberAsDouble(),
                            (float)dirArr.Get(1).GetNumberAsDouble(),
                            (float)dirArr.Get(2).GetNumberAsDouble()
                        };
                    }
                }

                if (light.type == LightType::Spot && lightObj.Has("spot") && lightObj.Get("spot").IsObject())
                {
                    const tinygltf::Value& spotObj = lightObj.Get("spot");
                    if (spotObj.Has("innerConeAngle"))
                        light.innerConeAngle = glm::degrees((float)spotObj.Get("innerConeAngle").GetNumberAsDouble());
                    if (spotObj.Has("outerConeAngle"))
                        light.outerConeAngle = glm::degrees((float)spotObj.Get("outerConeAngle").GetNumberAsDouble());
                }
                if (lightObj.Has("innerConeAngleDeg"))
                    light.innerConeAngle = (float)lightObj.Get("innerConeAngleDeg").GetNumberAsDouble();
                if (lightObj.Has("outerConeAngleDeg"))
                    light.outerConeAngle = (float)lightObj.Get("outerConeAngleDeg").GetNumberAsDouble();

                ecsScene.lights.push_back(light);
            }
        }
    }

    // --- Helper: extract local TRS from a glTF node ---
    auto extractLocalTRS = [](const tinygltf::Node& node, glm::vec3& outT, glm::quat& outR, glm::vec3& outS) {
        outT = {0.0f, 0.0f, 0.0f};
        outR = {1.0f, 0.0f, 0.0f, 0.0f};  // identity
        outS = {1.0f, 1.0f, 1.0f};

        if (node.matrix.size() == 16)
        {
            glm::mat4 m = glm::make_mat4(node.matrix.data());
            // Decompose matrix into TRS
            outT = glm::vec3(m[3]);
            float scaleX = glm::length(glm::vec3(m[0]));
            float scaleY = glm::length(glm::vec3(m[1]));
            float scaleZ = glm::length(glm::vec3(m[2]));
            outS = {scaleX, scaleY, scaleZ};

            glm::mat3 rotMat(
                glm::vec3(m[0]) / scaleX,
                glm::vec3(m[1]) / scaleY,
                glm::vec3(m[2]) / scaleZ
            );
            outR = glm::quat_cast(rotMat);
        }
        else
        {
            if (node.translation.size() >= 3)
                outT = {(float)node.translation[0], (float)node.translation[1], (float)node.translation[2]};
            if (node.rotation.size() >= 4)
            {
                // glTF quaternion is [x, y, z, w]; GLM wants (w, x, y, z)
                outR = glm::quat((float)node.rotation[3], (float)node.rotation[0],
                                 (float)node.rotation[1], (float)node.rotation[2]);
            }
            if (node.scale.size() >= 3)
                outS = {(float)node.scale[0], (float)node.scale[1], (float)node.scale[2]};
        }
    };

    // --- Helper: read accessor data (shared with Load) ---
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
        if (stride < 0) return out;
        const unsigned char* data = buf.data.data() + bv.byteOffset + acc.byteOffset;
        out.resize(acc.count);
        for (size_t i = 0; i < acc.count; i++)
        {
            const float* f = reinterpret_cast<const float*>(data + i * stride);
            out[i] = glm::vec3(f[0], f[1], f[2]);
        }
        return out;
    };

    auto readVec4Accessor = [](const tinygltf::Model& model, int accessorIdx) -> std::vector<glm::vec4> {
        std::vector<glm::vec4> out;
        if (accessorIdx < 0 || accessorIdx >= (int)model.accessors.size()) return out;
        const tinygltf::Accessor& acc = model.accessors[accessorIdx];
        if (acc.bufferView < 0 || acc.bufferView >= (int)model.bufferViews.size()) return out;
        const tinygltf::BufferView& bv = model.bufferViews[acc.bufferView];
        if (bv.buffer < 0 || bv.buffer >= (int)model.buffers.size()) return out;
        const tinygltf::Buffer& buf = model.buffers[bv.buffer];
        int stride = acc.ByteStride(bv);
        if (stride < 0 || acc.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) return out;
        const unsigned char* data = buf.data.data() + bv.byteOffset + acc.byteOffset;
        out.resize(acc.count);
        for (size_t i = 0; i < acc.count; i++)
        {
            const float* f = reinterpret_cast<const float*>(data + i * stride);
            out[i] = glm::vec4(f[0], f[1], f[2], f[3]);
        }
        return out;
    };

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
        if (stride < 0) return out;
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
                out[i] = acc.normalized ? glm::vec2(b[0] / 255.0f, b[1] / 255.0f)
                                        : glm::vec2((float)b[0], (float)b[1]);
            }
            else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
            {
                const uint16_t* s = reinterpret_cast<const uint16_t*>(p);
                out[i] = acc.normalized ? glm::vec2(s[0] / 65535.0f, s[1] / 65535.0f)
                                        : glm::vec2((float)s[0], (float)s[1]);
            }
            else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_SHORT)
            {
                const int16_t* s = reinterpret_cast<const int16_t*>(p);
                out[i] = acc.normalized
                            ? glm::vec2(std::max(-1.0f, s[0] / 32767.0f), std::max(-1.0f, s[1] / 32767.0f))
                            : glm::vec2((float)s[0], (float)s[1]);
            }
        }
        return out;
    };

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
        if (stride < 0) return out;
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

    // --- Helper: create a MeshData from a glTF primitive (object space) ---
    auto createMeshData = [&](const tinygltf::Model& model, const tinygltf::Primitive& prim) -> MeshData {
        MeshData mesh;

        // POSITION (required)
        auto posIt = prim.attributes.find("POSITION");
        if (posIt == prim.attributes.end())
            return mesh;
        std::vector<glm::vec3> positions = readVec3Accessor(model, posIt->second);
        if (positions.empty())
            return mesh;

        // Object-space vertices (NO world transform applied)
        mesh.vertices.reserve(positions.size() * 3);
        for (const auto& p : positions)
        {
            mesh.vertices.push_back(p.x);
            mesh.vertices.push_back(p.y);
            mesh.vertices.push_back(p.z);
        }

        // NORMAL (optional, object space)
        auto nrmIt = prim.attributes.find("NORMAL");
        if (nrmIt != prim.attributes.end())
        {
            std::vector<glm::vec3> normals = readVec3Accessor(model, nrmIt->second);
            mesh.normals.reserve(normals.size() * 3);
            for (const auto& n : normals)
            {
                mesh.normals.push_back(n.x);
                mesh.normals.push_back(n.y);
                mesh.normals.push_back(n.z);
            }
        }

        // TANGENT (optional glTF vec4: xyz tangent, w bitangent handedness)
        auto tangentIt = prim.attributes.find("TANGENT");
        if (tangentIt != prim.attributes.end())
        {
            std::vector<glm::vec4> tangents = readVec4Accessor(model, tangentIt->second);
            mesh.tangents.reserve(tangents.size() * 4);
            for (const auto& tangent : tangents)
            {
                mesh.tangents.push_back(tangent.x);
                mesh.tangents.push_back(tangent.y);
                mesh.tangents.push_back(tangent.z);
                mesh.tangents.push_back(tangent.w);
            }
        }

        // TEXCOORD_0 (optional)
        auto uvIt = prim.attributes.find("TEXCOORD_0");
        if (uvIt != prim.attributes.end())
        {
            std::vector<glm::vec2> uvs = readVec2Accessor(model, uvIt->second);
            mesh.uvs.reserve(uvs.size() * 2);
            for (const auto& uv : uvs)
            {
                mesh.uvs.push_back(uv.x);
                mesh.uvs.push_back(uv.y);
            }
        }

        // INDICES
        if (prim.indices >= 0)
        {
            mesh.indices = readIndices(model, prim.indices);
        }
        else
        {
            // Non-indexed: generate sequential
            size_t vertCount = positions.size();
            size_t triCount = vertCount / 3;
            mesh.indices.resize(triCount * 3);
            for (size_t i = 0; i < triCount * 3; i++)
                mesh.indices[i] = static_cast<uint32_t>(i);
        }

        return mesh;
    };

    // --- Node traversal: create entities with Transform + MeshRef ---
    // Cache: glTF mesh index → MeshRegistry index (one BLAS per unique mesh)
    std::unordered_map<int, std::vector<uint32_t>> meshIndexCache;
    // glTF mesh index → list of MeshRegistry indices (one per primitive)

    // Capture the glTF default scene index for durable source keys so a
    // directly-loaded glTF scene can be saved and reopened as .rt2scene.
    int gltfSceneIdx = model.defaultScene;
    if (gltfSceneIdx < 0 && !model.scenes.empty())
        gltfSceneIdx = 0;

    std::function<entt::entity(int, entt::entity)> traverseNodeECS =
        [&](int nodeIdx, entt::entity parentEntity) -> entt::entity {
        if (nodeIdx < 0 || nodeIdx >= (int)model.nodes.size())
            return entt::null;

        const tinygltf::Node& node = model.nodes[nodeIdx];

        // Create entity with Transform
        entt::entity entity = ecsScene.registry.create();

        Transform& tf = ecsScene.registry.emplace<Transform>(entity);
        extractLocalTRS(node, tf.translation, tf.rotation, tf.scale);
        tf.dirty = true;

        // Hierarchy: link to parent
        if (parentEntity != entt::null)
        {
            Hierarchy& hier = ecsScene.registry.emplace<Hierarchy>(entity);
            hier.parent = parentEntity;

            // Register this entity as a child of the parent
            auto* parentHier = ecsScene.registry.try_get<Hierarchy>(parentEntity);
            if (parentHier)
                parentHier->children.push_back(entity);
            else
            {
                Hierarchy& ph = ecsScene.registry.emplace<Hierarchy>(parentEntity);
                ph.children.push_back(entity);
            }
        }

        // Name
        if (!node.name.empty())
        {
            auto& name = ecsScene.registry.emplace<NameComponent>(entity);
            name.name = node.name;
        }

        // Handle camera nodes
        if (node.camera >= 0 && node.camera < (int)model.cameras.size())
        {
            const tinygltf::Camera& gcam = model.cameras[node.camera];
            SceneCamera& cam = ecsScene.camera;
            cam.verticalFOV = glm::degrees((float)gcam.perspective.yfov);

            CameraComponent camComp;
            camComp.verticalFOV = cam.verticalFOV;

            // Camera position/forward will be computed from world matrix
            // after SceneGraph::UpdateWorldTransforms
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
                    camComp.forwardDirection = cam.forwardDirection;
                }
            }
            if (gcam.extras.Has("aperture"))
            {
                cam.aperture = (float)gcam.extras.Get("aperture").GetNumberAsDouble();
                camComp.aperture = cam.aperture;
            }
            if (gcam.extras.Has("focusDistance"))
            {
                cam.focusDistance = (float)gcam.extras.Get("focusDistance").GetNumberAsDouble();
                camComp.focusDistance = cam.focusDistance;
            }
            if (gcam.extras.Has("verticalFOV"))
            {
                cam.verticalFOV = (float)gcam.extras.Get("verticalFOV").GetNumberAsDouble();
                camComp.verticalFOV = cam.verticalFOV;
            }

            // Attach CameraComponent to the entity so it appears in the outliner
            ecsScene.registry.emplace<CameraComponent>(entity, camComp);

            // If no explicit position/forward in extras, we'll compute from
            // the world matrix after SceneGraph::UpdateWorldTransforms
            // (handled by caller)
        }

        // Handle mesh nodes
        if (node.mesh >= 0 && node.mesh < (int)model.meshes.size())
        {
            const tinygltf::Mesh& gmesh = model.meshes[node.mesh];

            if (!gmesh.primitives.empty())
            {
                // Get or create MeshRegistry entries for this glTF mesh
                auto cacheIt = meshIndexCache.find(node.mesh);
                if (cacheIt == meshIndexCache.end())
                {
                    // First time seeing this mesh — create MeshData for each primitive
                    std::vector<uint32_t> meshIndices;
                    for (const auto& prim : gmesh.primitives)
                    {
                        int mode = (prim.mode >= 0) ? prim.mode : 4;
                        if (mode != 4) // TINYGLTF_MODE_TRIANGLES
                        {
                            printf("[SceneLoader] Skipping non-triangle primitive (mode %d)\n", mode);
                            meshIndices.push_back(0xFFFFFFFF);  // invalid marker
                            continue;
                        }

                        MeshData meshData = createMeshData(model, prim);
                        if (meshData.vertices.empty())
                        {
                            meshIndices.push_back(0xFFFFFFFF);
                            continue;
                        }

                        uint32_t meshIdx = ecsScene.meshRegistry.AddMesh(std::move(meshData));
                        meshIndices.push_back(meshIdx);
                    }
                    cacheIt = meshIndexCache.emplace(node.mesh, std::move(meshIndices)).first;
                }

                // Create a MeshRef entity for each primitive
                for (size_t p = 0; p < cacheIt->second.size(); p++)
                {
                    uint32_t meshIdx = cacheIt->second[p];
                    if (meshIdx == 0xFFFFFFFF)
                        continue;

                    const auto& prim = gmesh.primitives[p];
                    int matIdx = (prim.material >= 0) ? prim.material : 0;

                    std::string sourceKey =
                        "gltf:scene=" + std::to_string(gltfSceneIdx) +
                        ":node=" + std::to_string(nodeIdx) +
                        ":mesh=" + std::to_string(node.mesh) +
                        ":primitive=" + std::to_string(p);

                    if (p == 0)
                    {
                        // First primitive goes on the node entity itself
                        MeshRef& ref = ecsScene.registry.emplace<MeshRef>(entity);
                        ref.meshIndex = meshIdx;
                        ref.materialIndex = matIdx;

                        ImportedMeshSourceComponent src;
                        src.model.kind      = AssetKind::Model;
                        src.model.sourceKey = sourceKey;
                        ecsScene.registry.emplace<ImportedMeshSourceComponent>(entity, src);
                    }
                    else
                    {
                        // Additional primitives: create child entities (same transform)
                        entt::entity primEntity = ecsScene.registry.create();
                        Transform& primTf = ecsScene.registry.emplace<Transform>(primEntity);
                        primTf.dirty = true;

                        Hierarchy& hier = ecsScene.registry.emplace<Hierarchy>(primEntity);
                        hier.parent = entity;

                        // Register this primitive as a child of the node entity
                        // so RemoveEntity can recursively destroy it.
                        auto* parentHier = ecsScene.registry.try_get<Hierarchy>(entity);
                        if (parentHier)
                            parentHier->children.push_back(primEntity);
                        else
                        {
                            Hierarchy& ph = ecsScene.registry.emplace<Hierarchy>(entity);
                            ph.children.push_back(primEntity);
                        }

                        MeshRef& ref = ecsScene.registry.emplace<MeshRef>(primEntity);
                        ref.meshIndex = meshIdx;
                        ref.materialIndex = matIdx;

                        ImportedMeshSourceComponent src;
                        src.model.kind      = AssetKind::Model;
                        src.model.sourceKey = sourceKey;
                        ecsScene.registry.emplace<ImportedMeshSourceComponent>(primEntity, src);
                    }
                }
            }
            else if (!gmesh.name.empty())
            {
                // RT2 custom format: mesh.name is an external OBJ filepath
                // This is a legacy path — the OBJ would need to be loaded separately.
                // For now, just create a MeshRef with an invalid mesh index.
                // The caller can handle OBJ loading after ECS population.
                printf("[SceneLoader] RT2 custom mesh format (OBJ filepath): %s\n", gmesh.name.c_str());
            }
        }

        // Recurse into children
        for (int childIdx : node.children)
            traverseNodeECS(childIdx, entity);

        return entity;
    };

    // Start traversal from scene root nodes
    if (model.defaultScene >= 0 && model.defaultScene < (int)model.scenes.size())
    {
        const tinygltf::Scene& gltfScene = model.scenes[model.defaultScene];
        for (int rootIdx : gltfScene.nodes)
            traverseNodeECS(rootIdx, entt::null);
    }
    else if (!model.scenes.empty())
    {
        for (int rootIdx : model.scenes[0].nodes)
            traverseNodeECS(rootIdx, entt::null);
    }
    else
    {
        for (int i = 0; i < (int)model.nodes.size(); i++)
            traverseNodeECS(i, entt::null);
    }

    // Resolve world transforms
    SceneGraph::UpdateWorldTransforms(ecsScene.registry);

    // If camera position wasn't set from extras, compute from camera entity's world matrix
    // Find entity with camera (we stored camera data on the node entity)
    // For now, leave camera as-is — the caller can update it from the entity world matrix

    // Count entities with Transform component
    size_t entityCount = ecsScene.registry.view<Transform>().size();

    printf("[SceneLoader] ECS load: %zu entities, %d meshes, %d materials, %d textures\n",
           entityCount,
           (int)ecsScene.meshRegistry.GetCount(),
           (int)ecsScene.materials.size(),
           (int)ecsScene.textures.size());

    return true;
}

entt::entity SceneLoader::ImportIntoECS(ECSScene& ecsScene, const std::string& filepath)
{
    if (filepath.empty() || !fs::exists(filepath))
        return entt::null;

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
        printf("[SceneLoader] Import Error: %s\n", err.c_str());
    if (!warn.empty())
        printf("[SceneLoader] Import Warning: %s\n", warn.c_str());

    if (!ret)
        return entt::null;

    // Record base offsets for merging into existing scene
    size_t texBase = ecsScene.textures.size();
    size_t matBase = ecsScene.materials.size();

    // --- Textures (append to existing) ---
    for (const auto& gtext : model.textures)
    {
        SceneTexture tex;
        if (gtext.source >= 0 && gtext.source < (int)model.images.size())
        {
            const auto& img = model.images[gtext.source];
            tex.filepath = img.uri;
            if (!img.image.empty() && img.width > 0 && img.height > 0)
            {
                tex.width = img.width;
                tex.height = img.height;
                tex.channels = 4;
                tex.pixels.assign(img.image.begin(), img.image.end());
            }
        }
        ecsScene.textures.push_back(tex);
    }

    // --- Materials (append with index offset) ---
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
            mat.baseColorTextureIndex = gmat.pbrMetallicRoughness.baseColorTexture.index + (int)texBase;
        if (gmat.pbrMetallicRoughness.metallicRoughnessTexture.index >= 0)
            mat.metallicRoughnessTextureIndex = gmat.pbrMetallicRoughness.metallicRoughnessTexture.index + (int)texBase;
        if (gmat.normalTexture.index >= 0)
            mat.normalTextureIndex = gmat.normalTexture.index + (int)texBase;
        if (gmat.emissiveTexture.index >= 0)
            mat.emissiveTextureIndex = gmat.emissiveTexture.index + (int)texBase;

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

        mat.alphaMode = gmat.alphaMode;
        mat.alphaCutoff = (float)gmat.alphaCutoff;

        auto transmissionIt = gmat.extensions.find("KHR_materials_transmission");
        if (transmissionIt != gmat.extensions.end() && transmissionIt->second.IsObject())
        {
            const tinygltf::Value& transExt = transmissionIt->second;
            if (transExt.Has("transmissionFactor"))
            {
                float transFactor = (float)transExt.Get("transmissionFactor").GetNumberAsDouble();
                if (transFactor > 0.0f)
                {
                    mat.transmissionFactor = transFactor;
                    if (mat.transmissionFactor > 1.0f) mat.transmissionFactor = 1.0f;
                }
            }
        }

        if (gmat.extras.Has("ior"))
            mat.ior = (float)gmat.extras.Get("ior").GetNumberAsDouble();

        ecsScene.materials.push_back(mat);
    }

    // Mark sRGB textures for imported materials
    for (size_t i = matBase; i < ecsScene.materials.size(); i++)
    {
        const auto& mat = ecsScene.materials[i];
        if (mat.baseColorTextureIndex >= 0 && mat.baseColorTextureIndex < (int)ecsScene.textures.size())
            ecsScene.textures[mat.baseColorTextureIndex].isSRGB = true;
        if (mat.emissiveTextureIndex >= 0 && mat.emissiveTextureIndex < (int)ecsScene.textures.size())
            ecsScene.textures[mat.emissiveTextureIndex].isSRGB = true;
    }

    // --- Helpers (same as LoadIntoECS) ---
    auto extractLocalTRS = [](const tinygltf::Node& node, glm::vec3& outT, glm::quat& outR, glm::vec3& outS) {
        outT = {0.0f, 0.0f, 0.0f};
        outR = {1.0f, 0.0f, 0.0f, 0.0f};
        outS = {1.0f, 1.0f, 1.0f};

        if (node.matrix.size() == 16)
        {
            glm::mat4 m = glm::make_mat4(node.matrix.data());
            outT = glm::vec3(m[3]);
            float scaleX = glm::length(glm::vec3(m[0]));
            float scaleY = glm::length(glm::vec3(m[1]));
            float scaleZ = glm::length(glm::vec3(m[2]));
            outS = {scaleX, scaleY, scaleZ};
            glm::mat3 rotMat(
                glm::vec3(m[0]) / scaleX,
                glm::vec3(m[1]) / scaleY,
                glm::vec3(m[2]) / scaleZ
            );
            outR = glm::quat_cast(rotMat);
        }
        else
        {
            if (node.translation.size() >= 3)
                outT = {(float)node.translation[0], (float)node.translation[1], (float)node.translation[2]};
            if (node.rotation.size() >= 4)
            {
                outR = glm::quat((float)node.rotation[3], (float)node.rotation[0],
                                 (float)node.rotation[1], (float)node.rotation[2]);
            }
            if (node.scale.size() >= 3)
                outS = {(float)node.scale[0], (float)node.scale[1], (float)node.scale[2]};
        }
    };

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
        if (stride < 0) return out;
        const unsigned char* data = buf.data.data() + bv.byteOffset + acc.byteOffset;
        out.resize(acc.count);
        for (size_t i = 0; i < acc.count; i++)
        {
            const unsigned char* p = data + i * stride;
            if (acc.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT)
                out[i] = {((float*)p)[0], ((float*)p)[1], ((float*)p)[2]};
        }
        return out;
    };

    auto readVec4Accessor = [](const tinygltf::Model& model, int accessorIdx) -> std::vector<glm::vec4> {
        std::vector<glm::vec4> out;
        if (accessorIdx < 0 || accessorIdx >= (int)model.accessors.size()) return out;
        const tinygltf::Accessor& acc = model.accessors[accessorIdx];
        if (acc.bufferView < 0 || acc.bufferView >= (int)model.bufferViews.size()) return out;
        const tinygltf::BufferView& bv = model.bufferViews[acc.bufferView];
        if (bv.buffer < 0 || bv.buffer >= (int)model.buffers.size()) return out;
        const tinygltf::Buffer& buf = model.buffers[bv.buffer];
        int stride = acc.ByteStride(bv);
        if (stride < 0 || acc.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) return out;
        const unsigned char* data = buf.data.data() + bv.byteOffset + acc.byteOffset;
        out.resize(acc.count);
        for (size_t i = 0; i < acc.count; i++)
        {
            const float* f = reinterpret_cast<const float*>(data + i * stride);
            out[i] = glm::vec4(f[0], f[1], f[2], f[3]);
        }
        return out;
    };

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
        if (stride < 0) return out;
        const unsigned char* data = buf.data.data() + bv.byteOffset + acc.byteOffset;
        out.resize(acc.count);
        for (size_t i = 0; i < acc.count; i++)
        {
            const unsigned char* p = data + i * stride;
            if (acc.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT)
                out[i] = {((float*)p)[0], ((float*)p)[1]};
            else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
            {
                const uint16_t* s = reinterpret_cast<const uint16_t*>(p);
                out[i] = acc.normalized ? glm::vec2(s[0] / 65535.0f, s[1] / 65535.0f)
                                        : glm::vec2((float)s[0], (float)s[1]);
            }
        }
        return out;
    };

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
        if (stride < 0) return out;
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

    auto createMeshData = [&](const tinygltf::Model& model, const tinygltf::Primitive& prim) -> MeshData {
        MeshData mesh;
        auto posIt = prim.attributes.find("POSITION");
        if (posIt == prim.attributes.end())
            return mesh;
        std::vector<glm::vec3> positions = readVec3Accessor(model, posIt->second);
        if (positions.empty())
            return mesh;

        mesh.vertices.reserve(positions.size() * 3);
        for (const auto& p : positions)
        {
            mesh.vertices.push_back(p.x);
            mesh.vertices.push_back(p.y);
            mesh.vertices.push_back(p.z);
        }

        auto nrmIt = prim.attributes.find("NORMAL");
        if (nrmIt != prim.attributes.end())
        {
            std::vector<glm::vec3> normals = readVec3Accessor(model, nrmIt->second);
            mesh.normals.reserve(normals.size() * 3);
            for (const auto& n : normals)
            {
                mesh.normals.push_back(n.x);
                mesh.normals.push_back(n.y);
                mesh.normals.push_back(n.z);
            }
        }

        auto tangentIt = prim.attributes.find("TANGENT");
        if (tangentIt != prim.attributes.end())
        {
            std::vector<glm::vec4> tangents = readVec4Accessor(model, tangentIt->second);
            mesh.tangents.reserve(tangents.size() * 4);
            for (const auto& tangent : tangents)
            {
                mesh.tangents.push_back(tangent.x);
                mesh.tangents.push_back(tangent.y);
                mesh.tangents.push_back(tangent.z);
                mesh.tangents.push_back(tangent.w);
            }
        }

        auto uvIt = prim.attributes.find("TEXCOORD_0");
        if (uvIt != prim.attributes.end())
        {
            std::vector<glm::vec2> uvs = readVec2Accessor(model, uvIt->second);
            mesh.uvs.reserve(uvs.size() * 2);
            for (const auto& uv : uvs)
            {
                mesh.uvs.push_back(uv.x);
                mesh.uvs.push_back(uv.y);
            }
        }

        if (prim.indices >= 0)
            mesh.indices = readIndices(model, prim.indices);
        else
        {
            size_t vertCount = positions.size();
            size_t triCount = vertCount / 3;
            mesh.indices.resize(triCount * 3);
            for (size_t i = 0; i < triCount * 3; i++)
                mesh.indices[i] = static_cast<uint32_t>(i);
        }

        return mesh;
    };

    // --- Create wrapper root entity for the imported glTF ---
    auto& reg = ecsScene.registry;
    entt::entity wrapperRoot = reg.create();
    {
        Transform& tf = reg.emplace<Transform>(wrapperRoot);
        tf.translation = {0.0f, 0.0f, 0.0f};
        tf.dirty = true;
        reg.emplace<NameComponent>(wrapperRoot, fs::path(filepath).stem().string());
        reg.emplace<VisibleComponent>(wrapperRoot);
        Hierarchy& rootHier = reg.emplace<Hierarchy>(wrapperRoot);
        rootHier.parent = entt::null;
    }

    // --- Node traversal (same as LoadIntoECS but with material offset) ---
    std::unordered_map<int, std::vector<uint32_t>> meshIndexCache;

    // Capture the glTF default scene index for durable source keys. The
    // importer attaches ImportedMeshSourceComponent so the native .rt2scene
    // can rebuild the same mesh/material association after reopen.
    int gltfSceneIdx = model.defaultScene;
    if (gltfSceneIdx < 0 && !model.scenes.empty())
        gltfSceneIdx = 0;

    std::function<entt::entity(int, entt::entity)> traverseNodeECS =
        [&](int nodeIdx, entt::entity parentEntity) -> entt::entity {
        if (nodeIdx < 0 || nodeIdx >= (int)model.nodes.size())
            return entt::null;

        const tinygltf::Node& node = model.nodes[nodeIdx];

        entt::entity entity = reg.create();

        Transform& tf = reg.emplace<Transform>(entity);
        extractLocalTRS(node, tf.translation, tf.rotation, tf.scale);
        tf.dirty = true;

        if (parentEntity != entt::null)
        {
            Hierarchy& hier = reg.emplace<Hierarchy>(entity);
            hier.parent = parentEntity;
            auto* parentHier = reg.try_get<Hierarchy>(parentEntity);
            if (parentHier)
                parentHier->children.push_back(entity);
            else
            {
                Hierarchy& ph = reg.emplace<Hierarchy>(parentEntity);
                ph.children.push_back(entity);
            }
        }

        if (!node.name.empty())
        {
            auto& name = reg.emplace<NameComponent>(entity);
            name.name = node.name;
        }

        if (node.mesh >= 0 && node.mesh < (int)model.meshes.size())
        {
            const tinygltf::Mesh& gmesh = model.meshes[node.mesh];

            if (!gmesh.primitives.empty())
            {
                auto cacheIt = meshIndexCache.find(node.mesh);
                if (cacheIt == meshIndexCache.end())
                {
                    std::vector<uint32_t> meshIndices;
                    for (const auto& prim : gmesh.primitives)
                    {
                        int mode = (prim.mode >= 0) ? prim.mode : 4;
                        if (mode != 4)
                        {
                            meshIndices.push_back(0xFFFFFFFF);
                            continue;
                        }

                        MeshData meshData = createMeshData(model, prim);
                        if (meshData.vertices.empty())
                        {
                            meshIndices.push_back(0xFFFFFFFF);
                            continue;
                        }

                        uint32_t meshIdx = ecsScene.meshRegistry.AddMesh(std::move(meshData));
                        meshIndices.push_back(meshIdx);
                    }
                    cacheIt = meshIndexCache.emplace(node.mesh, std::move(meshIndices)).first;
                }

                for (size_t p = 0; p < cacheIt->second.size(); p++)
                {
                    uint32_t meshIdx = cacheIt->second[p];
                    if (meshIdx == 0xFFFFFFFF)
                        continue;

                    const auto& prim = gmesh.primitives[p];
                    // Offset material index by matBase to reference imported materials
                    int matIdx = (prim.material >= 0) ? (prim.material + (int)matBase) : 0;

                    // Durable source key for this primitive. Uses the glTF
                    // scene/node/mesh/primitive indices so the native scene
                    // can rebuild the same association after reopen.
                    std::string sourceKey =
                        "gltf:scene=" + std::to_string(gltfSceneIdx) +
                        ":node=" + std::to_string(nodeIdx) +
                        ":mesh=" + std::to_string(node.mesh) +
                        ":primitive=" + std::to_string(p);

                    if (p == 0)
                    {
                        MeshRef& ref = reg.emplace<MeshRef>(entity);
                        ref.meshIndex = meshIdx;
                        ref.materialIndex = matIdx;

                        ImportedMeshSourceComponent src;
                        src.model.kind      = AssetKind::Model;
                        src.model.sourceKey = sourceKey;
                        reg.emplace<ImportedMeshSourceComponent>(entity, src);
                    }
                    else
                    {
                        entt::entity primEntity = reg.create();
                        Transform& primTf = reg.emplace<Transform>(primEntity);
                        primTf.dirty = true;

                        Hierarchy& hier = reg.emplace<Hierarchy>(primEntity);
                        hier.parent = entity;

                        auto* parentHier = reg.try_get<Hierarchy>(entity);
                        if (parentHier)
                            parentHier->children.push_back(primEntity);
                        else
                        {
                            Hierarchy& ph = reg.emplace<Hierarchy>(entity);
                            ph.children.push_back(primEntity);
                        }

                        MeshRef& ref = reg.emplace<MeshRef>(primEntity);
                        ref.meshIndex = meshIdx;
                        ref.materialIndex = matIdx;

                        ImportedMeshSourceComponent src;
                        src.model.kind      = AssetKind::Model;
                        src.model.sourceKey = sourceKey;
                        reg.emplace<ImportedMeshSourceComponent>(primEntity, src);
                    }
                }
            }
        }

        for (int childIdx : node.children)
            traverseNodeECS(childIdx, entity);

        return entity;
    };

    // Traverse from glTF scene root nodes, parent them under wrapper root
    if (model.defaultScene >= 0 && model.defaultScene < (int)model.scenes.size())
    {
        const tinygltf::Scene& gltfScene = model.scenes[model.defaultScene];
        for (int rootIdx : gltfScene.nodes)
            traverseNodeECS(rootIdx, wrapperRoot);
    }
    else if (!model.scenes.empty())
    {
        for (int rootIdx : model.scenes[0].nodes)
            traverseNodeECS(rootIdx, wrapperRoot);
    }
    else
    {
        for (int i = 0; i < (int)model.nodes.size(); i++)
            traverseNodeECS(i, wrapperRoot);
    }

    SceneGraph::SetLocalDirty(reg, wrapperRoot);
    SceneGraph::UpdateWorldTransforms(reg);

    printf("[SceneLoader] Import: %zu textures, %zu materials, %d meshes\n",
           ecsScene.textures.size() - texBase,
           ecsScene.materials.size() - matBase,
           (int)ecsScene.meshRegistry.GetCount());

    return wrapperRoot;
}

// ============================================================================
// OBJ loading (tinyobjloader → ECS)
// ============================================================================

#ifndef TINYOBJLOADER_STREAM_READER_MAX_BYTES
#define TINYOBJLOADER_STREAM_READER_MAX_BYTES (size_t(2048) * size_t(1024) * size_t(1024))
#endif

#define TINYOBJLOADER_IMPLEMENTATION
#include "tinyobjloader/tiny_obj_loader.h"

bool SceneLoader::LoadObjIntoECS(ECSScene& ecsScene, const std::string& filepath)
{
    if (filepath.empty() || !fs::exists(filepath))
        return false;

    fs::path fpath(filepath);
    std::string baseDir = fpath.parent_path().string();

    tinyobj::ObjReader reader;
    tinyobj::ObjReaderConfig config;
    config.mtl_search_path = baseDir;
    config.triangulate = true;
    config.vertex_color = false;

    printf("[SceneLoader] OBJ: parsing '%s' (%.1fMB)...\n", filepath.c_str(),
           (double)fs::file_size(filepath) / (1024.0 * 1024.0));
    fflush(stdout);

    if (!reader.ParseFromFile(filepath, config))
    {
        if (!reader.Error().empty())
            printf("[SceneLoader] OBJ error: %s\n", reader.Error().c_str());
        fflush(stdout);
        return false;
    }
    printf("[SceneLoader] OBJ: parse done\n");
    fflush(stdout);
    if (!reader.Warning().empty())
        printf("[SceneLoader] OBJ warning: %s\n", reader.Warning().c_str());

    const auto& attrib = reader.GetAttrib();
    const auto& shapes = reader.GetShapes();
    const auto& materials = reader.GetMaterials();

    printf("[SceneLoader] OBJ: %d verts, %d shapes, %d materials\n",
           (int)attrib.vertices.size() / 3, (int)shapes.size(), (int)materials.size());
    fflush(stdout);

    // Convert MTL materials to SceneMaterial
    int matBase = (int)ecsScene.materials.size();
    for (const auto& mtl : materials)
    {
        SceneMaterial mat;
        mat.baseColor = {mtl.diffuse[0], mtl.diffuse[1], mtl.diffuse[2]};
        mat.baseAlpha = (mtl.dissolve > 0.0f) ? mtl.dissolve : 1.0f;
        mat.metallic = 0.0f;
        if (mtl.roughness > 0.0f)
        {
            // Exocortex's Pr extension already stores perceptual roughness.
            mat.roughness = std::clamp(static_cast<float>(mtl.roughness), 0.0f, 1.0f);
        }
        else
        {
            // Legacy MTL stores a Blinn-Phong exponent (Ns).  Matching its
            // lobe to GGX gives alpha=sqrt(2/(Ns+2)); RT2's material value is
            // perceptual roughness (alpha=roughness^2), hence the extra sqrt.
            const float shininess = std::max(static_cast<float>(mtl.shininess), 0.0f);
            mat.roughness = std::sqrt(std::sqrt(2.0f / (shininess + 2.0f)));
        }
        mat.ior = mtl.ior;
        mat.emissiveColor = {mtl.emission[0], mtl.emission[1], mtl.emission[2]};
        mat.emissiveIntensity = (mtl.emission[0] + mtl.emission[1] + mtl.emission[2]) > 0.0f ? 1.0f : 0.0f;
        if (mtl.dissolve < 1.0f)
        {
            mat.alphaMode = "BLEND";
            mat.baseAlpha = (mtl.dissolve > 0.0f) ? mtl.dissolve : 1.0f;
        }
        ecsScene.materials.push_back(mat);
    }

    // If no materials, add a default
    if (ecsScene.materials.empty())
    {
        SceneMaterial mat;
        ecsScene.materials.push_back(mat);
        matBase = 0;
    }

    // Load textures
    int texBase = (int)ecsScene.textures.size();
    auto loadTexture = [&](const std::string& texName, bool isSRGB) -> int {
        if (texName.empty()) return -1;
        fs::path texPath = fs::path(baseDir) / texName;
        if (!fs::exists(texPath)) return -1;

        int w, h, channels;
        unsigned char* pixels = stbi_load(texPath.string().c_str(), &w, &h, &channels, 4);
        if (!pixels)
        {
            printf("[SceneLoader] Failed to load texture: %s\n", texPath.string().c_str());
            return -1;
        }

        SceneTexture tex;
        tex.filepath = texPath.string();
        tex.width = w;
        tex.height = h;
        tex.channels = 4;
        tex.pixels.assign(pixels, pixels + (size_t)w * h * 4);
        // Color textures are decoded through an sRGB image format. Data
        // textures must preserve their stored numeric values.
        tex.isSRGB = isSRGB;
        stbi_image_free(pixels);

        int idx = (int)ecsScene.textures.size();
        ecsScene.textures.push_back(tex);
        return idx;
    };

    // Assign textures to materials
    int texCount = 0;
    for (int mi = 0; mi < (int)materials.size(); mi++)
    {
        int matIdx = matBase + mi;
        if (matIdx >= (int)ecsScene.materials.size()) break;

        auto& mat = ecsScene.materials[matIdx];

        if (!materials[mi].diffuse_texname.empty())
            { int ti = loadTexture(materials[mi].diffuse_texname, true); if (ti >= 0) { mat.baseColorTextureIndex = ti; texCount++; } }
        if (!materials[mi].normal_texname.empty())
            { int ti = loadTexture(materials[mi].normal_texname, false); if (ti >= 0) { mat.normalTextureIndex = ti; texCount++; } }
        if (!materials[mi].emissive_texname.empty())
            { int ti = loadTexture(materials[mi].emissive_texname, true); if (ti >= 0) { mat.emissiveTextureIndex = ti; texCount++; } }
        if (!materials[mi].roughness_texname.empty())
            { int ti = loadTexture(materials[mi].roughness_texname, false); if (ti >= 0) { mat.metallicRoughnessTextureIndex = ti; texCount++; } }
    }
    printf("[SceneLoader] OBJ: %d textures loaded\n", texCount);
    fflush(stdout);

    // Merge all shapes into a single mega-mesh (one BLAS)
    std::vector<float>    megaVertices;
    std::vector<uint32_t> megaIndices;
    std::vector<float>    megaNormals;
    std::vector<float>    megaUVs;
    std::vector<uint32_t> megaMaterialIds;

    // OBJ uses separate indices for position, normal, and texture coordinate.
    // Build one indexed vertex for each unique attribute tuple instead of
    // expanding every triangle corner into a new vertex. Large architectural
    // scenes otherwise become multi-gigabyte triangle-soup buffers and make
    // the raster-first G-buffer pass vertex-bound.
    struct ObjVertexKey
    {
        int vertex;
        int normal;
        int texcoord;

        bool operator==(const ObjVertexKey& other) const
        {
            return vertex == other.vertex && normal == other.normal && texcoord == other.texcoord;
        }
    };
    struct ObjVertexKeyHash
    {
        size_t operator()(const ObjVertexKey& key) const
        {
            size_t h = std::hash<int>{}(key.vertex);
            h ^= std::hash<int>{}(key.normal) + size_t(0x9e3779b9u) + (h << 6) + (h >> 2);
            h ^= std::hash<int>{}(key.texcoord) + size_t(0x9e3779b9u) + (h << 6) + (h >> 2);
            return h;
        }
    };

    std::unordered_map<ObjVertexKey, uint32_t, ObjVertexKeyHash> vertexCache;
    vertexCache.max_load_factor(0.8f);
    vertexCache.reserve(attrib.vertices.size() / 3);

    size_t cornerCount = 0;
    size_t triangleCount = 0;
    for (const auto& shape : shapes)
    {
        cornerCount += shape.mesh.indices.size();
        triangleCount += shape.mesh.num_face_vertices.size();
    }
    megaIndices.reserve(cornerCount);
    megaMaterialIds.reserve(triangleCount);

    printf("[SceneLoader] OBJ: merging %d shapes...\n", (int)shapes.size());
    fflush(stdout);

    for (size_t s = 0; s < shapes.size(); s++)
    {
        const auto& shape = shapes[s];
        int shapeMatIdx = 0;

        for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); f++)
        {
            size_t fv = shape.mesh.num_face_vertices[f];
            if (fv != 3) continue;

            int matId = shape.mesh.material_ids[f];
            if (matId >= 0 && matId < (int)materials.size())
                shapeMatIdx = matId;

            for (size_t v = 0; v < fv; v++)
            {
                tinyobj::index_t idx = shape.mesh.indices[f * fv + v];

                ObjVertexKey key = { idx.vertex_index, idx.normal_index, idx.texcoord_index };
                auto cached = vertexCache.find(key);
                if (cached != vertexCache.end())
                {
                    megaIndices.push_back(cached->second);
                    continue;
                }

                uint32_t unifiedIndex = static_cast<uint32_t>(megaVertices.size() / 3);
                vertexCache.emplace(key, unifiedIndex);

                int vi = idx.vertex_index;
                if (vi >= 0)
                {
                    megaVertices.push_back(attrib.vertices[vi * 3 + 0]);
                    megaVertices.push_back(attrib.vertices[vi * 3 + 1]);
                    megaVertices.push_back(attrib.vertices[vi * 3 + 2]);
                }
                else
                {
                    megaVertices.push_back(0.0f);
                    megaVertices.push_back(0.0f);
                    megaVertices.push_back(0.0f);
                }

                int ni = idx.normal_index;
                if (ni >= 0 && ni * 3 + 2 < (int)attrib.normals.size())
                {
                    megaNormals.push_back(attrib.normals[ni * 3 + 0]);
                    megaNormals.push_back(attrib.normals[ni * 3 + 1]);
                    megaNormals.push_back(attrib.normals[ni * 3 + 2]);
                }
                else
                {
                    megaNormals.push_back(0.0f);
                    megaNormals.push_back(0.0f);
                    megaNormals.push_back(0.0f);
                }

                int ti = idx.texcoord_index;
                if (ti >= 0 && ti * 2 + 1 < (int)attrib.texcoords.size())
                {
                    megaUVs.push_back(attrib.texcoords[ti * 2 + 0]);
                    // OBJ texture coordinates use a bottom-left V origin,
                    // while stb_image and Vulkan sampling use top-left here.
                    megaUVs.push_back(1.0f - attrib.texcoords[ti * 2 + 1]);
                }
                else
                {
                    megaUVs.push_back(0.0f);
                    megaUVs.push_back(0.0f);
                }

                megaIndices.push_back(unifiedIndex);
            }

            megaMaterialIds.push_back(static_cast<uint32_t>(shapeMatIdx));
        }
    }

    printf("[SceneLoader] OBJ: merge done, %d indexed verts, %d corners (%.2fx reuse)\n",
           (int)megaVertices.size() / 3, (int)megaIndices.size(),
           megaVertices.empty() ? 0.0 : double(megaIndices.size()) / double(megaVertices.size() / 3));
    fflush(stdout);

    // Create one MeshData for the mega-mesh
    MeshData meshData;
    meshData.vertices = std::move(megaVertices);
    meshData.indices = std::move(megaIndices);
    meshData.normals = std::move(megaNormals);
    meshData.uvs = std::move(megaUVs);
    meshData.materialIndices = std::move(megaMaterialIds);

    std::string name = fpath.stem().string();
    meshData.name = name;

    uint32_t meshIdx = ecsScene.meshRegistry.AddMesh(std::move(meshData));

    // Create one entity for the mega-mesh
    auto entity = ecsScene.registry.create();
    Transform tf;
    tf.translation = {0, 0, 0};
    tf.scale = {1, 1, 1};
    ecsScene.registry.emplace<Transform>(entity, tf);
    ecsScene.registry.emplace<MeshRef>(entity, meshIdx, -1); // use per-triangle materials
    ecsScene.registry.emplace<NameComponent>(entity, name);
    ecsScene.registry.emplace<VisibleComponent>(entity);

    // Durable provenance: OBJ whole-model mega-mesh. The importer profile is
    // persisted by the serializer so the resolver can rebuild the same mesh.
    {
        ImportedMeshSourceComponent src;
        src.model.kind      = AssetKind::Model;
        src.model.sourceKey = "obj:whole-model";
        src.model.importSettings.triangulate     = true;
        src.model.importSettings.mergeMegaMesh   = true;
        src.model.importSettings.generateNormals = false;
        ecsScene.registry.emplace<ImportedMeshSourceComponent>(entity, src);
    }

    printf("[SceneLoader] OBJ loaded: %d verts, %d tris, %d textures, %d materials\n",
           (int)ecsScene.meshRegistry.GetMesh(meshIdx).vertices.size() / 3,
           (int)ecsScene.meshRegistry.GetMesh(meshIdx).indices.size() / 3,
           (int)ecsScene.textures.size() - texBase,
           (int)ecsScene.materials.size() - matBase);
    fflush(stdout);

    return true;
}
