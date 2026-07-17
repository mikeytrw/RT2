#pragma once

#ifndef RT2_PHASE1A_FIXTURE_GENERATOR_H
#define RT2_PHASE1A_FIXTURE_GENERATOR_H

#include "SceneSerializer.h"
#include "SceneDocument.h"
#include "SceneAssetResolver.h"
#include "ECSComponents.h"
#include "ECSScene.h"
#include "SceneTypes.h"
#include "MeshRegistry.h"
#include "PrimitiveGeometry.h"
#include "core/UUID.h"
#include "core/Error.h"

#include <filesystem>
#include <fstream>
#include <vector>
#include <cstdint>
#include <cstring>

// tinyexr (implementation is compiled by TinyEXRLoader.cpp; here we only
// need the header declarations).
#define NOMINMAX
#include <tinyexr.h>

// tinygltf for GLB generation. Do NOT define TINYGLTF_IMPLEMENTATION here —
// SceneLoader.cpp already does. Match the same defines to avoid symbol
// mismatches.
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include "tiny_gltf.h"

namespace rt2::core {

// Generate a tiny 4x2 EXR environment map (RGBA float) into `path`.
// Deterministic content: a horizontal gradient useful for verifying env
// reload without committing a large file.
inline bool GenerateTinyExrEnv(const std::filesystem::path& path, Error& err)
{
    err = Error{};
    const int w = 4, h = 2;
    std::vector<float> rgba((size_t)w * h * 4, 0.0f);
    for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
    {
        size_t i = ((size_t)y * w + x) * 4;
        rgba[i + 0] = float(x) / float(w - 1); // R gradient
        rgba[i + 1] = float(y) / float(h - 1); // G gradient
        rgba[i + 2] = 0.25f;                   // B constant
        rgba[i + 3] = 1.0f;                    // A
    }

    const char* exrErr = nullptr;
    int ret = SaveEXR(rgba.data(), w, h, 4, /*fp16=*/0, path.string().c_str(), &exrErr);
    if (ret != TINYEXR_SUCCESS)
    {
        err.code = Error::Io;
        err.path = path.string();
        err.detail = std::string("SaveEXR failed: ") + (exrErr ? exrErr : "unknown");
        if (exrErr) FreeEXRErrorMessage(const_cast<char*>(exrErr));
        return false;
    }
    return true;
}

// Generate a tiny GLB with a single textured triangle (positions, UVs,
// indices, one material with a base color texture). Writes a small embedded
// PNG texture. Uses tinygltf directly.
inline bool GenerateTinyTexturedGlb(const std::filesystem::path& path, Error& err)
{
    err = Error{};

    tinygltf::Model model;
    model.asset.version = "2.0";
    model.asset.generator = "RT2 Phase1A Fixture Generator";

    // 8x8 RGBA8 solid-color texture (a magenta swatch).
    const int tw = 8, th = 8;
    std::vector<unsigned char> texPixels((size_t)tw * th * 4, 0);
    for (size_t i = 0; i < texPixels.size(); i += 4)
    {
        texPixels[i + 0] = 255;
        texPixels[i + 1] = 0;
        texPixels[i + 2] = 255;
        texPixels[i + 3] = 255;
    }
    tinygltf::Image img;
    img.width = tw;
    img.height = th;
    img.component = 4;
    img.image = texPixels;
    model.images.push_back(img);

    tinygltf::Texture gtext;
    gtext.source = 0;
    model.textures.push_back(gtext);

    // Material with base color texture.
    tinygltf::Material mat;
    mat.pbrMetallicRoughness.baseColorFactor = { 0.8, 0.2, 0.2, 1.0 };
    mat.pbrMetallicRoughness.metallicFactor = 0.0;
    mat.pbrMetallicRoughness.roughnessFactor = 0.7;
    mat.pbrMetallicRoughness.baseColorTexture.index = 0;
    mat.pbrMetallicRoughness.baseColorTexture.texCoord = 0;
    model.materials.push_back(mat);

    // Geometry: a single triangle with positions + UVs + indices.
    auto appendBuffer = [&](const std::vector<unsigned char>& data) -> int {
        tinygltf::Buffer b;
        b.data = data;
        model.buffers.push_back(b);
        return (int)model.buffers.size() - 1;
    };
    auto addVec3Accessor = [&](const std::vector<float>& data) -> int {
        std::vector<unsigned char> bytes(data.size() * sizeof(float));
        std::memcpy(bytes.data(), data.data(), bytes.size());
        int buf = appendBuffer(bytes);
        tinygltf::BufferView bv;
        bv.buffer = buf; bv.byteOffset = 0; bv.byteLength = bytes.size();
        bv.byteStride = 0; bv.target = 34962;
        model.bufferViews.push_back(bv);
        int bvIdx = (int)model.bufferViews.size() - 1;
        tinygltf::Accessor acc;
        acc.bufferView = bvIdx; acc.byteOffset = 0;
        acc.componentType = 5126; acc.count = data.size() / 3; acc.type = 3;
        model.accessors.push_back(acc);
        return (int)model.accessors.size() - 1;
    };
    auto addVec2Accessor = [&](const std::vector<float>& data) -> int {
        std::vector<unsigned char> bytes(data.size() * sizeof(float));
        std::memcpy(bytes.data(), data.data(), bytes.size());
        int buf = appendBuffer(bytes);
        tinygltf::BufferView bv;
        bv.buffer = buf; bv.byteOffset = 0; bv.byteLength = bytes.size();
        bv.byteStride = 0; bv.target = 34962;
        model.bufferViews.push_back(bv);
        int bvIdx = (int)model.bufferViews.size() - 1;
        tinygltf::Accessor acc;
        acc.bufferView = bvIdx; acc.byteOffset = 0;
        acc.componentType = 5126; acc.count = data.size() / 2; acc.type = 2;
        model.accessors.push_back(acc);
        return (int)model.accessors.size() - 1;
    };

    std::vector<float> positions = {
        -1.0f, -1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
         0.0f,  1.0f, 0.0f,
    };
    std::vector<float> uvs = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.5f, 1.0f,
    };
    std::vector<uint32_t> indices = { 0, 1, 2 };
    std::vector<unsigned char> idxBytes(indices.size() * sizeof(uint32_t));
    std::memcpy(idxBytes.data(), indices.data(), idxBytes.size());
    int posAcc = addVec3Accessor(positions);
    int uvAcc  = addVec2Accessor(uvs);
    int idxBuf = appendBuffer(idxBytes);
    tinygltf::BufferView idxBv;
    idxBv.buffer = idxBuf; idxBv.byteOffset = 0; idxBv.byteLength = idxBytes.size();
    idxBv.byteStride = 0; idxBv.target = 34963;
    model.bufferViews.push_back(idxBv);
    int idxBvIdx = (int)model.bufferViews.size() - 1;
    tinygltf::Accessor idxAcc;
    idxAcc.bufferView = idxBvIdx; idxAcc.byteOffset = 0;
    idxAcc.componentType = 5125; idxAcc.count = indices.size(); idxAcc.type = 65;
    model.accessors.push_back(idxAcc);
    int idxAccIdx = (int)model.accessors.size() - 1;

    tinygltf::Primitive prim;
    prim.attributes["POSITION"] = posAcc;
    prim.attributes["TEXCOORD_0"] = uvAcc;
    prim.indices = idxAccIdx;
    prim.material = 0;
    prim.mode = 4;
    tinygltf::Mesh mesh;
    mesh.primitives.push_back(prim);
    model.meshes.push_back(mesh);

    tinygltf::Node node;
    node.mesh = 0;
    node.name = "TexturedTriangle";
    model.nodes.push_back(node);

    tinygltf::Scene scene;
    scene.nodes = { 0 };
    model.scenes.push_back(scene);
    model.defaultScene = 0;

    tinygltf::TinyGLTF loader;
    std::string gerr, gwarn;
    bool ok = loader.WriteGltfSceneToFile(&model, path.string(),
                                          true,  // embedImages
                                          true,  // embedBuffers
                                          true,  // prettyPrint
                                          true); // writeBinary (GLB)
    if (!ok)
    {
        err.code = Error::Io;
        err.path = path.string();
        err.detail = "WriteGltfSceneToFile failed for GLB fixture";
        return false;
    }
    return true;
}

} // namespace rt2::core

#endif // RT2_PHASE1A_FIXTURE_GENERATOR_H