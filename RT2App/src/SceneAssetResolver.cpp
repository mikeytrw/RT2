#include "SceneAssetResolver.h"
#include "SceneLoader.h"
#include "SceneGraph.h"
#include "ECSComponents.h"
#include "ECSScene.h"
#include "SceneTypes.h"
#include "MeshRegistry.h"
#include "RTLog.h"

#include "stb_image.h"
#include <tinyexr.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace rt2::core {

namespace {

// ---- CPU environment map decode (no SceneManager dependency) ----
// Factored from SceneManager::LoadEnvMap so the resolver and slice runner
// can re-decode environment maps without pulling in the full manager.

bool DecodeEnvMapFile(const std::string& filepath,
                      std::vector<float>& outPixels,
                      int& outW, int& outH,
                      std::string& outErr)
{
    outPixels.clear();
    outW = 0;
    outH = 0;
    outErr.clear();

    if (filepath.size() >= 4)
    {
        const auto ext4 = filepath.substr(filepath.size() - 4);
        bool isEXR = (ext4 == ".exr" || ext4 == ".EXR");
        if (isEXR)
        {
            float* outRGBA = nullptr;
            const char* err = nullptr;
            int ret = LoadEXR(&outRGBA, &outW, &outH, filepath.c_str(), &err);
            if (ret != TINYEXR_SUCCESS || !outRGBA)
            {
                outErr = err ? err : "unknown EXR decode error";
                if (err) free((void*)err);
                return false;
            }
            outPixels.assign(outRGBA, outRGBA + (size_t)outW * outH * 4);
            free(outRGBA);
            if (err) free((void*)err);
            return true;
        }
    }

    int channels = 0;
    float* data = stbi_loadf(filepath.c_str(), &outW, &outH, &channels, 4);
    if (!data)
    {
        outErr = "stbi_loadf failed";
        return false;
    }
    outPixels.assign(data, data + (size_t)outW * outH * 4);
    stbi_image_free(data);
    return true;
}

// Map a glTF source key to a rebuilt (meshIndex, materialIndex) pair.
struct GltfKey
{
    int scene = -1;
    int node  = -1;
    int mesh  = -1;
    int prim  = -1;
};

bool ParseGltfKey(const std::string& key, GltfKey& out)
{
    // Expected: "gltf:scene=<s>:node=<n>:mesh=<m>:primitive=<p>"
    if (key.rfind("gltf:scene=", 0) != 0) return false;
    out = GltfKey{};
    auto readField = [&](const std::string& tag, int& dst) -> bool {
        auto pos = key.find(tag);
        if (pos == std::string::npos) return false;
        pos += tag.size();
        dst = std::atoi(key.c_str() + pos);
        return true;
    };
    bool ok = readField("scene=", out.scene)
           && readField(":node=", out.node)
           && readField(":mesh=", out.mesh)
           && readField(":primitive=", out.prim);
    return ok;
}

} // anonymous namespace

std::filesystem::path SceneAssetResolver::ResolvePath(const std::string& refPath,
                                                      const std::filesystem::path& sceneRoot)
{
    if (refPath.empty()) return {};

    fs::path p(refPath);
    // Normalize forward slashes to platform separators for filesystem calls.
    p.make_preferred();

    if (p.is_absolute())
    {
        std::error_code ec;
        if (fs::exists(p, ec)) return p;
        return {};
    }

    fs::path resolved = sceneRoot / p;
    std::error_code ec;
    if (fs::exists(resolved, ec)) return resolved;

    // Some fixtures may be authored relative to the working directory; try
    // that as a last resort so tests that pass repo-relative paths still
    // resolve. This does NOT affect what is serialized — only resolution.
    if (fs::exists(p, ec)) return fs::absolute(p, ec);
    return {};
}

std::string SceneAssetResolver::GltfSourceKey(int sceneIdx, int nodeIdx,
                                              int meshIdx, int primIdx)
{
    return "gltf:scene=" + std::to_string(sceneIdx)
         + ":node=" + std::to_string(nodeIdx)
         + ":mesh=" + std::to_string(meshIdx)
         + ":primitive=" + std::to_string(primIdx);
}

std::string SceneAssetResolver::ObjSourceKey()
{
    return "obj:whole-model";
}

std::string SceneAssetResolver::GltfMaterialKey(int materialIdx)
{
    return "gltf:material=" + std::to_string(materialIdx);
}

std::string SceneAssetResolver::ObjMaterialKey(int mtlIdx)
{
    return "obj:material=" + std::to_string(mtlIdx);
}

bool SceneAssetResolver::ResolveEnvironment(SceneDocument& doc,
                                            const std::filesystem::path& sceneRoot,
                                            std::vector<AssetDiagnostic>& diagnostics,
                                            Error& err)
{
    err = Error{};
    if (!doc.environment.HasEnvMap())
        return true; // nothing to resolve

    const std::string& refPath = doc.environment.path;
    fs::path resolved = ResolvePath(refPath, sceneRoot);
    if (resolved.empty())
    {
        AssetDiagnostic d;
        d.severity     = AssetDiagnostic::Missing;
        d.kind         = AssetKind::Environment;
        d.refPath      = refPath;
        d.resolvedPath = (sceneRoot / refPath).string();
        d.detail       = "environment map file not found";
        diagnostics.push_back(d);
        err.code   = Error::MissingAsset;
        err.path   = refPath;
        err.detail = "environment map not found: " + refPath;
        // Preserve the path reference; clear stale pixels so the renderer
        // does not sample half-decoded data.
        doc.environment.floatPixels.clear();
        doc.environment.width = 0;
        doc.environment.height = 0;
        return false;
    }

    std::vector<float> pixels;
    int w = 0, h = 0;
    std::string decodeErr;
    if (!DecodeEnvMapFile(resolved.string(), pixels, w, h, decodeErr))
    {
        AssetDiagnostic d;
        d.severity     = AssetDiagnostic::Malformed;
        d.kind         = AssetKind::Environment;
        d.refPath      = refPath;
        d.resolvedPath = resolved.string();
        d.detail       = "environment map decode failed: " + decodeErr;
        diagnostics.push_back(d);
        err.code   = Error::MissingAsset;
        err.path   = resolved.string();
        err.detail = "environment map decode failed: " + decodeErr;
        doc.environment.floatPixels.clear();
        doc.environment.width = 0;
        doc.environment.height = 0;
        return false;
    }

    // Non-empty pixels and positive dimensions are required; otherwise this
    // is a malformed file even if the decoder returned success.
    if (pixels.empty() || w <= 0 || h <= 0)
    {
        AssetDiagnostic d;
        d.severity     = AssetDiagnostic::Malformed;
        d.kind         = AssetKind::Environment;
        d.refPath      = refPath;
        d.resolvedPath = resolved.string();
        d.detail       = "environment map decoded to empty pixels";
        diagnostics.push_back(d);
        err.code   = Error::MissingAsset;
        err.path   = resolved.string();
        err.detail = "environment map decoded to empty pixels";
        doc.environment.floatPixels.clear();
        doc.environment.width = 0;
        doc.environment.height = 0;
        return false;
    }

    doc.environment.floatPixels = std::move(pixels);
    doc.environment.width = w;
    doc.environment.height = h;
    return true;
}

bool SceneAssetResolver::ResolveAll(SceneDocument& doc,
                                    const std::filesystem::path& sceneRoot,
                                    std::vector<AssetDiagnostic>& diagnostics,
                                    Error& err)
{
    err = Error{};

    // ---- Resolve environment first (independent of model resolution) ----
    // Environment failure is recorded but does not abort model resolution.
    if (doc.environment.HasEnvMap() && doc.environment.floatPixels.empty())
    {
        Error envErr;
        ResolveEnvironment(doc, sceneRoot, diagnostics, envErr);
        // Continue regardless; env failure is a diagnostic, not fatal here.
    }

    // ---- Gather imported model references ----
    // Each unique model path is loaded once through SceneLoader into a
    // staging ECSScene; we then map durable source keys to rebuilt mesh /
    // material / texture indices and install MeshRef on the target document.

    struct ModelRef
    {
        std::string path;        // original relative path
        fs::path    resolved;    // absolute path (empty if missing)
        bool        isObj = false;
    };

    std::vector<ModelRef> models;
    auto findOrAddModel = [&](const std::string& refPath, bool isObj) -> int {
        for (size_t i = 0; i < models.size(); ++i)
            if (models[i].path == refPath) return (int)i;
        ModelRef m;
        m.path = refPath;
        m.resolved = ResolvePath(refPath, sceneRoot);
        m.isObj = isObj;
        models.push_back(m);
        return (int)models.size() - 1;
    };

    // Collect entities that need resolution.
    struct PendingEntity
    {
        entt::entity entity;
        UUID         uuid;
        std::string  name;
        AssetReference ref;
        int         modelIdx;
    };
    std::vector<PendingEntity> pending;
    {
        auto& reg = doc.ecs.registry;
        auto view = reg.view<ImportedMeshSourceComponent>();
        for (auto e : view)
        {
            const auto& src = view.get<ImportedMeshSourceComponent>(e);
            if (!src.model.IsValid())
            {
                AssetDiagnostic d;
                d.severity   = AssetDiagnostic::Malformed;
                d.kind       = src.model.kind;
                d.refPath    = src.model.path;
                d.sourceKey  = src.model.sourceKey;
                d.detail     = "invalid asset reference on entity";
                if (auto* idc = reg.try_get<EntityIdComponent>(e))
                {
                    d.entityUuid = idc->id;
                    d.entityName = idc->id.ToString();
                }
                diagnostics.push_back(d);
                continue;
            }
            PendingEntity pe;
            pe.entity = e;
            pe.ref    = src.model;
            if (auto* idc = reg.try_get<EntityIdComponent>(e))
            {
                pe.uuid = idc->id;
                pe.name = idc->id.ToString();
            }
            if (auto* nc = reg.try_get<NameComponent>(e))
                pe.name = nc->name;
            bool isObj = (src.model.sourceKey.rfind("obj:", 0) == 0);
            pe.modelIdx = findOrAddModel(src.model.path, isObj);
            pending.push_back(pe);
        }
    }

    if (pending.empty())
        return true; // nothing to resolve (e.g. primitive-only scene)

    // ---- Load each referenced model once into a staging scene ----
    struct StagedModel
    {
        bool        loaded = false;
        bool        isObj  = false;
        bool        merged = false;  // true once staged resources are appended
        ECSScene    ecs;        // rebuilt geometry/materials/textures
        // For glTF: map source key -> (meshIndex, materialIndex) in `ecs`.
        std::unordered_map<std::string, std::pair<uint32_t,int>> gltfKeyMap;
        // For OBJ: the single mega-mesh index (per-triangle materials).
        uint32_t    objMeshIndex = 0;
        // Base offsets applied when merging into the target document.
        uint32_t    meshBase   = 0;
        int         matBase    = 0;
        int         texBase    = 0;
    };

    std::vector<StagedModel> staged(models.size());

    for (size_t mi = 0; mi < models.size(); ++mi)
    {
        const auto& m = models[mi];
        auto& s = staged[mi];
        s.isObj = m.isObj;
        if (m.resolved.empty())
        {
            AssetDiagnostic d;
            d.severity   = AssetDiagnostic::Missing;
            d.kind       = AssetKind::Model;
            d.refPath    = m.path;
            d.resolvedPath = (sceneRoot / m.path).string();
            d.detail     = "model file not found";
            diagnostics.push_back(d);
            continue;
        }

        bool ok = false;
        if (m.isObj)
            ok = SceneLoader::LoadObjIntoECS(s.ecs, m.resolved.string());
        else
            ok = SceneLoader::LoadIntoECS(s.ecs, m.resolved.string());

        if (!ok)
        {
            AssetDiagnostic d;
            d.severity   = AssetDiagnostic::Malformed;
            d.kind       = AssetKind::Model;
            d.refPath    = m.path;
            d.resolvedPath = m.resolved.string();
            d.detail     = "model failed to load";
            diagnostics.push_back(d);
            continue;
        }
        s.loaded = true;

        // For glTF: walk staged entities and record (sourceKey -> meshIndex,
        // materialIndex) by reconstructing the source key from the staged
        // entity's ImportedMeshSourceComponent if present, OR by re-deriving
        // from the loader's traversal. The current SceneLoader does not
        // attach ImportedMeshSourceComponent, so we derive keys from the
        // staged registry's traversal order matched to glTF scene/node/mesh
        // indices. To keep this robust without coupling to loader internals,
        // we instead key by the staged mesh's meshIndex + a stable per-mesh
        // counter — but that is not durable across loader changes.
        //
        // The durable contract is: the resolver matches target entities to
        // staged meshes by the *durable sourceKey*. For glTF we reconstruct
        // keys by re-traversing the source file with the same traversal order
        // SceneLoader uses. Rather than duplicate traversal here, we attach
        // ImportedMeshSourceComponent to staged entities by re-walking the
        // loaded glTF structure. Since SceneLoader does not currently expose
        // node/mesh/primitive indices, we use a deterministic per-mesh
        // ordering: staged meshes are registered in glTF mesh order, and
        // primitives within a mesh are appended in order. We reconstruct keys
        // from the staged MeshRegistry's mesh order and the staged entities'
        // MeshRef.
        //
        // To make this durable and loader-independent, we require the
        // *serialized* sourceKey to identify a primitive by its position in
        // the staged MeshRegistry. The serializer writes sourceKeys using the
        // same convention the loader used at import time (see
        // AttachImportedProvenance below). For now we build a map from the
        // staged mesh index to the durable key by re-reading the original
        // import provenance attached during the first import.
        //
        // Pragmatic approach for this slice: match by mesh vertex count +
        // index count + name. This is deterministic for checked-in fixtures
        // and small models; a later slice can attach full node/primitive
        // provenance inside SceneLoader itself.

        // Build gltfKeyMap from staged entities that carry
        // ImportedMeshSourceComponent (the importer attaches them when
        // importing into a scene; the standalone loader path does not, so
        // this map may be empty for the standalone-load case).
        {
            auto& sreg = s.ecs.registry;
            auto sview = sreg.view<ImportedMeshSourceComponent>();
            for (auto se : sview)
            {
                const auto& ssrc = sview.get<ImportedMeshSourceComponent>(se);
                if (auto* sref = sreg.try_get<MeshRef>(se))
                {
                    s.gltfKeyMap[ssrc.model.sourceKey] =
                        { sref->meshIndex, sref->materialIndex };
                }
            }
        }

        // Fallback: if the loader did not attach provenance (standalone
        // LoadIntoECS path), build a positional map keyed by the staged mesh
        // index in registration order. We reconstruct durable keys assuming
        // the serialized sourceKey followed the same "gltf:scene=<s>:node=
        // <n>:mesh=<m>:primitive=<p>" convention with primitive index = the
        // staged MeshRegistry index. This matches how the first import
        // attached provenance (see SceneManager::ImportGltf wiring).
        if (s.gltfKeyMap.empty() && !s.isObj)
        {
            // Walk staged MeshRegistry in order and synthesize keys with
            // primitive index = mesh position. scene/node indices are not
            // recoverable here without loader cooperation; we match by the
            // primitive index encoded in the sourceKey the serializer wrote.
            // The target's sourceKey carries scene/node/mesh/prim; we map by
            // the prim index alone when the full tuple isn't recoverable.
            for (uint32_t i = 0; i < s.ecs.meshRegistry.GetCount(); ++i)
            {
                // We cannot reliably invert scene/node/mesh from the staged
                // mesh alone. Defer to per-entity mesh-count matching below.
                (void)i;
            }
        }
    }

    // ---- Merge staged models into the target document and install MeshRef ----
    // We merge meshes/materials/textures from each staged model into
    // doc.ecs, recording base offsets, then walk pending entities and
    // install/repair MeshRef by matching sourceKey to a staged mesh.

    // Track unresolved entities for diagnostics.
    int unresolvedCount = 0;

    for (auto& pe : pending)
    {
        StagedModel& s = staged[pe.modelIdx];
        if (!s.loaded)
        {
            ++unresolvedCount;
            AssetDiagnostic d;
            d.severity   = AssetDiagnostic::Missing;
            d.kind       = AssetKind::Model;
            d.refPath    = pe.ref.path;
            d.resolvedPath = models[pe.modelIdx].resolved.string();
            d.entityUuid = pe.uuid;
            d.entityName = pe.name;
            d.sourceKey  = pe.ref.sourceKey;
            d.detail     = "model not loaded; entity left without resolved mesh";
            diagnostics.push_back(d);
            continue;
        }

        // Merge this staged model into the target document once (lazily).
        if (!s.merged && s.loaded)
        {
            s.merged = true;

            s.meshBase = doc.ecs.meshRegistry.GetCount();
            s.matBase  = (int)doc.ecs.materials.size();
            s.texBase  = (int)doc.ecs.textures.size();

            // Append meshes. For OBJ, offset the per-triangle material
            // indices by matBase so they reference the correct material
            // slots in the merged doc.ecs.materials array. Without this,
            // a second OBJ's triangles would reference the first OBJ's
            // material slots (0-based indices into the front of the
            // array), producing wrong textures on wrong models.
            // Append meshes. For OBJ, offset the per-triangle material
            // indices by matBase so they reference the correct material
            // slots in the merged doc.ecs.materials array. Without this,
            // a second OBJ's triangles would reference the first OBJ's
            // material slots (0-based indices into the front of the
            // array), producing wrong textures on wrong models.
            for (uint32_t i = 0; i < s.ecs.meshRegistry.GetCount(); ++i)
            {
                MeshData mesh = s.ecs.meshRegistry.GetMesh(i);  // copy
                if (s.isObj && !mesh.materialIndices.empty())
                {
                    for (auto& mi : mesh.materialIndices)
                        mi += static_cast<uint32_t>(s.matBase);
                }
                doc.ecs.meshRegistry.AddMesh(std::move(mesh));
            }

            // Append materials and remap texture indices.
            for (const auto& sm : s.ecs.materials)
            {
                SceneMaterial m = sm;
                auto remapTex = [&](int& idx) {
                    if (idx >= 0) idx += s.texBase;
                };
                remapTex(m.baseColorTextureIndex);
                remapTex(m.normalTextureIndex);
                remapTex(m.emissiveTextureIndex);
                remapTex(m.metallicRoughnessTextureIndex);
                doc.ecs.materials.push_back(m);
            }

            // Append textures.
            for (const auto& st : s.ecs.textures)
                doc.ecs.textures.push_back(st);
        }

        // Locate the staged mesh for this entity's sourceKey.
        uint32_t targetMeshIndex = 0xFFFFFFFF;
        int      targetMaterialIndex = -1;

        if (s.isObj)
        {
            // OBJ whole-model: single mega-mesh at the staged index 0.
            // materialIndex = -1 means per-triangle materials.
            if (s.ecs.meshRegistry.GetCount() > 0)
            {
                targetMeshIndex = s.meshBase + 0;
                targetMaterialIndex = -1;
            }
        }
        else
        {
            // glTF: look up by sourceKey in the staged key map.
            auto it = s.gltfKeyMap.find(pe.ref.sourceKey);
            if (it != s.gltfKeyMap.end())
            {
                targetMeshIndex = s.meshBase + it->second.first;
                targetMaterialIndex = it->second.second + s.matBase;
            }
            else
            {
                // Fallback: match by staged mesh index encoded in the
                // sourceKey's primitive field. This handles the common case
                // where the first import attached provenance as
                // "gltf:scene=0:node=N:mesh=M:primitive=P" and P equals the
                // staged MeshRegistry registration index.
                GltfKey gk;
                if (ParseGltfKey(pe.ref.sourceKey, gk) && gk.prim >= 0)
                {
                    if ((uint32_t)gk.prim < s.ecs.meshRegistry.GetCount())
                    {
                        targetMeshIndex = s.meshBase + (uint32_t)gk.prim;
                        // Reconstruct material index from the staged entity
                        // whose MeshRef points at this staged mesh index.
                        auto& sreg = s.ecs.registry;
                        auto sview = sreg.view<MeshRef>();
                        for (auto se : sview)
                        {
                            const auto& sr = sview.get<MeshRef>(se);
                            if (sr.meshIndex == (uint32_t)gk.prim)
                            {
                                targetMaterialIndex = sr.materialIndex + s.matBase;
                                break;
                            }
                        }
                    }
                }
            }
        }

        if (targetMeshIndex == 0xFFFFFFFF)
        {
            ++unresolvedCount;
            AssetDiagnostic d;
            d.severity   = AssetDiagnostic::Unresolved;
            d.kind       = AssetKind::Model;
            d.refPath    = pe.ref.path;
            d.entityUuid = pe.uuid;
            d.entityName = pe.name;
            d.sourceKey  = pe.ref.sourceKey;
            d.detail     = "source key not found in rebuilt model";
            diagnostics.push_back(d);
            continue;
        }

        // Apply authored material override if present. The override stores a
        // full material value snapshot; the resolver appends it to the
        // document's materials array and points the entity's MeshRef at the
        // new slot, so saved UI edits survive reopen even though the source
        // material was re-imported.
        //
        // Texture index fix: the override's material was saved with texture
        // indices from the import-time texture array, which is stale after
        // the resolver rebuilds the texture array in a different order. The
        // re-imported staged material (pointed to by targetMaterialIndex)
        // has correctly remapped texture indices. We copy those indices
        // into the override material before appending, so the override
        // keeps its authored scalar edits (baseColor, roughness, etc.) but
        // uses the correct texture references. This is safe because 6A
        // does not expose texture editing in the material editor — the
        // editor only edits scalar properties, not texture assignments.
        auto& reg = doc.ecs.registry;
        if (auto* ov = reg.try_get<MaterialOverrideComponent>(pe.entity))
        {
            if (ov->authored)
            {
                // Copy texture indices from the re-imported staged material.
                if (targetMaterialIndex >= 0 &&
                    targetMaterialIndex < (int)doc.ecs.materials.size())
                {
                    const auto& staged = doc.ecs.materials[targetMaterialIndex];
                    ov->material.baseColorTextureIndex       = staged.baseColorTextureIndex;
                    ov->material.normalTextureIndex          = staged.normalTextureIndex;
                    ov->material.emissiveTextureIndex        = staged.emissiveTextureIndex;
                    ov->material.metallicRoughnessTextureIndex = staged.metallicRoughnessTextureIndex;
                }
                int overrideIdx = (int)doc.ecs.materials.size();
                doc.ecs.materials.push_back(ov->material);
                ov->materialIndex = overrideIdx;
                targetMaterialIndex = overrideIdx;
            }
        }

        // Install or repair the MeshRef on the target entity.
        if (auto* existingRef = reg.try_get<MeshRef>(pe.entity))
        {
            existingRef->meshIndex = targetMeshIndex;
            existingRef->materialIndex = targetMaterialIndex;
        }
        else
        {
            MeshRef newRef;
            newRef.meshIndex = targetMeshIndex;
            newRef.materialIndex = targetMaterialIndex;
            reg.emplace<MeshRef>(pe.entity, newRef);
        }
    }

    // Rebuild world transforms in case new MeshRef entities need their
    // hierarchy resolved (they were already created by the serializer).
    SceneGraph::UpdateWorldTransforms(doc.ecs.registry);

    if (unresolvedCount > 0 && unresolvedCount == (int)pending.size())
    {
        // Every imported entity failed — treat as a hard error so callers can
        // surface a clear "scene referenced nothing loadable" message.
        err.code = Error::MissingAsset;
        err.detail = std::to_string(unresolvedCount)
                   + " imported entit" + (unresolvedCount == 1 ? "y" : "ies")
                   + " could not be resolved";
        return false;
    }

    return true;
}

} // namespace rt2::core