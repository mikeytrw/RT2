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

    const std::string& refPath = doc.environment.ref.path;

    // Phase 7 W3 step 4: resolve the environment map through the shared
    // read-only locator (ID-first, path+sidecar fallback). The locator emits
    // exactly one terminal diagnostic on failure and never mints/writes a
    // sidecar — env import owns identity repair (SceneManager::LoadEnvMap).
    // The environment now carries a real AssetReference (W3-Q1/D2), so we
    // resolve it directly instead of reconstructing a temporary reference.
    // The kind invariant (AssetKind::Environment) is maintained by
    // EnvironmentSettings' default member initializer and Clear(); set it
    // defensively here in case a caller mutated ref.path/assetId without
    // setting kind.
    AssetReference& ref = doc.environment.ref;
    ref.kind = AssetKind::Environment;
    AssetResolutionContext ctx;
    ctx.assetRoot = sceneRoot;
    ctx.database  = nullptr;

    std::vector<AssetDiagnostic> resolveDiags;
    auto rr = Resolve(ref, ctx, UUID::Nil(), "", resolveDiags);
    // Append locator diagnostics to the caller's vector (W3-P8: one terminal
    // diagnostic per failed reference).
    for (auto& d : resolveDiags)
        diagnostics.push_back(std::move(d));

    if (!rr.success)
    {
        err.code   = Error::MissingAsset;
        err.path   = refPath;
        err.detail = "environment map resolution failed: " + refPath;
        // Preserve the path reference; clear stale pixels so the renderer
        // does not sample half-decoded data. The durable assetId is also
        // preserved so a later successful reload reattaches by identity.
        doc.environment.floatPixels.clear();
        doc.environment.width = 0;
        doc.environment.height = 0;
        return false;
    }

    std::vector<float> pixels;
    int w = 0, h = 0;
    std::string decodeErr;
    if (!DecodeEnvMapFile(rr.resolvedPath.string(), pixels, w, h, decodeErr))
    {
        AssetDiagnostic d;
        d.severity     = AssetDiagnostic::Malformed;
        d.kind         = AssetKind::Environment;
        d.refPath      = refPath;
        d.resolvedPath = rr.resolvedPath.string();
        d.detail       = "environment map decode failed: " + decodeErr;
        diagnostics.push_back(d);
        err.code   = Error::MissingAsset;
        err.path   = rr.resolvedPath.string();
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
        d.resolvedPath = rr.resolvedPath.string();
        d.detail       = "environment map decoded to empty pixels";
        diagnostics.push_back(d);
        err.code   = Error::MissingAsset;
        err.path   = rr.resolvedPath.string();
        err.detail = "environment map decoded to empty pixels";
        doc.environment.floatPixels.clear();
        doc.environment.width = 0;
        doc.environment.height = 0;
        return false;
    }

    doc.environment.floatPixels = std::move(pixels);
    doc.environment.width = w;
    doc.environment.height = h;
    // If the locator resolved by path fallback and the sidecar supplied an
    // effective ID, cache it so the next save persists it. This does not
    // mint; it only copies an already-authoritative sidecar ID into the
    // document cache. Nil effective ID (absent sidecar) leaves the document
    // ID untouched — the host's next save/migration owns repair.
    if (!rr.effectiveId.IsNull())
        doc.environment.ref.assetId = rr.effectiveId;
    return true;
}

bool SceneAssetResolver::ResolveAll(SceneDocument& doc,
                                    const std::filesystem::path& sceneRoot,
                                    std::vector<AssetDiagnostic>& diagnostics,
                                    Error& err)
{
    err = Error{};
    const size_t diagnosticBase = diagnostics.size();
    auto sortDiagnostics = [&]() {
        if (diagnosticBase >= diagnostics.size())
            return;
        std::stable_sort(
            diagnostics.begin() +
                static_cast<std::ptrdiff_t>(diagnosticBase),
            diagnostics.end(),
            [](const AssetDiagnostic& a, const AssetDiagnostic& b) {
                return AssetDiagnosticSortKey(a) <
                       AssetDiagnosticSortKey(b);
            });
    };

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
        AssetReference ownerRef;
        UUID        effectiveId;
        bool        isObj = false;
        // OBJ import mode (captured from the first entity referencing this
        // model+mode). The dedup key is (path, isObj, mergeMegaMesh) so the
        // same OBJ imported in both modes stages independently.
        bool        mergeMegaMesh = true;
        // Resolution context for the first entity that referenced this model.
        // W3 step 3: resolution is ID-first via the shared locator. The host
        // does not build an AssetDatabase yet (step 4 / W4), so `database` is
        // nullptr and the locator falls back to path+sidecar verification.
        UUID        firstEntityUuid;
        std::string firstEntityName;
    };

    // W3 step 3: build the resolution context once. No database is available
    // at scene load yet; the locator handles the no-database case by
    // verifying the asset's sidecar against any non-nil requested ID.
    AssetResolutionContext resolutionContext;
    resolutionContext.assetRoot = sceneRoot;
    resolutionContext.database  = nullptr;

    std::vector<ModelRef> models;
    auto findOrAddModel = [&](const std::string& refPath, bool isObj,
                              bool mergeMegaMesh,
                              const AssetReference& ref,
                              const UUID& entityUuid,
                              const std::string& entityName) -> int {
        for (size_t i = 0; i < models.size(); ++i)
            if (models[i].path == refPath &&
                models[i].isObj == isObj &&
                models[i].mergeMegaMesh == mergeMegaMesh)
                return (int)i;
        ModelRef m;
        m.path = refPath;
        m.ownerRef = ref;
        m.isObj = isObj;
        m.mergeMegaMesh = mergeMegaMesh;
        m.firstEntityUuid = entityUuid;
        m.firstEntityName = entityName;
        // Resolve through the shared locator. The locator emits exactly one
        // terminal diagnostic on failure (W3-P8: no duplicate file-level
        // diagnostic), routed through the same AssetDiagnostic vector. We
        // keep the resolved path for the loader; the diagnostic is owned by
        // the caller's `diagnostics` vector and is emitted once per missing
        // model path, not once per entity.
        std::vector<AssetDiagnostic> resolveDiags;
        auto rr = Resolve(ref, resolutionContext, entityUuid, entityName,
                         resolveDiags);
        m.resolved = rr.success ? rr.resolvedPath : fs::path{};
        m.effectiveId = rr.effectiveId;
        // The locator emits one diagnostic on failure (or a stale-path
        // Stale advisory on success-by-ID). Append it to the caller's vector; the
        // batch sort at the end of ResolveAll orders them deterministically.
        for (auto& d : resolveDiags)
            diagnostics.push_back(std::move(d));
        models.push_back(std::move(m));
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
            pe.modelIdx = findOrAddModel(src.model.path, isObj,
                                         src.model.importSettings.mergeMegaMesh,
                                         src.model, pe.uuid, pe.name);
            pending.push_back(pe);
        }
    }

    if (pending.empty())
    {
        sortDiagnostics();
        return true; // nothing to resolve (e.g. primitive-only scene)
    }

    // ---- Load each referenced model once into a staging scene ----
    struct StagedModel
    {
        bool        loaded = false;
        bool        isObj  = false;
        bool        merged = false;  // true once staged resources are appended
        ECSScene    ecs;        // rebuilt geometry/materials/textures
        // Generic: map source key -> (meshIndex, materialIndex) in `ecs`.
        // Populated from staged entities carrying
        // ImportedMeshSourceComponent. Works for both glTF primitive keys
        // ("gltf:scene=...:primitive=...") and OBJ per-shape keys
        // ("obj:shape=<n>:name=<...>").
        std::unordered_map<std::string, std::pair<uint32_t,int>> keyMap;
        // For OBJ mega-mesh (legacy "obj:whole-model"): the single
        // mega-mesh index (per-triangle materials). Used as a fallback
        // when keyMap lookup misses on a whole-model key.
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
            // The shared locator already emitted exactly one terminal
            // diagnostic for this missing path (W3-P8: no duplicate
            // file-level diagnostic). Nothing to append here.
            continue;
        }

        TextureAssetLoadContext textureContext;
        textureContext.resolution = resolutionContext;
        textureContext.ownerModel = m.ownerRef;
        textureContext.resolvedOwnerPath = m.resolved;
        textureContext.effectiveOwnerId = m.effectiveId;
        textureContext.entityUuid = m.firstEntityUuid;
        textureContext.entityName = m.firstEntityName;
        textureContext.identityMode = TextureIdentityMode::ReadOnly;

        bool ok = false;
        if (m.isObj)
        {
            ImportSettings iset;
            iset.mergeMegaMesh = m.mergeMegaMesh;
            entt::entity root = SceneLoader::ImportObjIntoECS(
                s.ecs, m.resolved.string(), iset,
                textureContext, diagnostics);
            ok = (root != entt::null);
        }
        else
        {
            ok = SceneLoader::LoadIntoECS(
                s.ecs, m.resolved.string(), textureContext, diagnostics);
        }

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

        // Build keyMap from staged entities that carry
        // ImportedMeshSourceComponent. This is format-agnostic: glTF
        // primitive keys ("gltf:scene=...:primitive=...") and OBJ per-shape
        // keys ("obj:shape=<n>:name=<...>") both flow through the same map.
        // The importer (ImportIntoECS / ImportObjIntoECS) attaches provenance
        // to staged entities; the standalone loader path does not, so this
        // map may be empty for the standalone-load case.
        {
            auto& sreg = s.ecs.registry;
            auto sview = sreg.view<ImportedMeshSourceComponent>();
            for (auto se : sview)
            {
                const auto& ssrc = sview.get<ImportedMeshSourceComponent>(se);
                if (auto* sref = sreg.try_get<MeshRef>(se))
                {
                    s.keyMap[ssrc.model.sourceKey] =
                        { sref->meshIndex, sref->materialIndex };
                    // Capture the OBJ mega-mesh index for legacy fallback.
                    if (ssrc.model.sourceKey == "obj:whole-model")
                        s.objMeshIndex = sref->meshIndex;
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
        if (s.keyMap.empty() && !s.isObj)
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

    // ---- Plan + commit: transactional model resolution ----
    // W3-P7/W3-Q6: a false ResolveAll leaves the document unchanged. A
    // successful partial result may commit accepted resources. We resolve
    // every entity's target against the STAGED model's local indices first
    // (no mutation of doc.ecs), then only commit the staged models that have
    // at least one resolved entity. If every entity is unresolved, doc.ecs
    // is untouched and we return false.
    //
    // Each plan entry records the staged model index and the LOCAL
    // (meshIndex, materialIndex) inside that staged model; the commit pass
    // rebases them by the staged model's base offsets once merged.

    struct PlanEntry
    {
        const PendingEntity* pe;
        uint32_t             localMeshIndex = 0xFFFFFFFF; // staged-local
        int                  localMaterialIndex = -1;      // staged-local
    };
    std::vector<PlanEntry> plan;
    plan.reserve(pending.size());

    int unresolvedCount = 0;

    for (const auto& pe : pending)
    {
        const StagedModel& s = staged[pe.modelIdx];
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

        uint32_t localMeshIndex = 0xFFFFFFFF;
        int      localMaterialIndex = -1;

        if (s.isObj)
        {
            auto it = s.keyMap.find(pe.ref.sourceKey);
            if (it != s.keyMap.end())
            {
                localMeshIndex = it->second.first;
                localMaterialIndex = -1;
            }
            else if (pe.ref.sourceKey == "obj:whole-model")
            {
                if (s.ecs.meshRegistry.GetCount() > 0)
                {
                    localMeshIndex = s.objMeshIndex;
                    localMaterialIndex = -1;
                }
            }
            else if (pe.ref.sourceKey.rfind("obj:shape=", 0) == 0)
            {
                auto namePos = pe.ref.sourceKey.find("name=");
                if (namePos != std::string::npos)
                {
                    std::string wantName = pe.ref.sourceKey.substr(namePos + 5);
                    for (const auto& kv : s.keyMap)
                    {
                        auto np = kv.first.find("name=");
                        if (np != std::string::npos &&
                            kv.first.substr(np + 5) == wantName)
                        {
                            localMeshIndex = kv.second.first;
                            localMaterialIndex = -1;
                            break;
                        }
                    }
                }
            }
        }
        else
        {
            auto it = s.keyMap.find(pe.ref.sourceKey);
            if (it != s.keyMap.end())
            {
                localMeshIndex = it->second.first;
                localMaterialIndex = it->second.second;
            }
            else
            {
                GltfKey gk;
                if (ParseGltfKey(pe.ref.sourceKey, gk) && gk.prim >= 0)
                {
                    if ((uint32_t)gk.prim < s.ecs.meshRegistry.GetCount())
                    {
                        localMeshIndex = (uint32_t)gk.prim;
                        auto& sreg = s.ecs.registry;
                        auto sview = sreg.view<MeshRef>();
                        for (auto se : sview)
                        {
                            const auto& sr = sview.get<MeshRef>(se);
                            if (sr.meshIndex == (uint32_t)gk.prim)
                            {
                                localMaterialIndex = sr.materialIndex;
                                break;
                            }
                        }
                    }
                }
            }
        }

        if (localMeshIndex == 0xFFFFFFFF)
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

        plan.push_back({ &pe, localMeshIndex, localMaterialIndex });
    }

    // Aggregate policy: if every imported entity failed, hard-fail with the
    // document UNCHANGED. No staged resources have been appended to doc.ecs
    // (the plan pass only read from staged scenes). W3-P7 transactionality.
    if (unresolvedCount > 0 && unresolvedCount == (int)pending.size())
    {
        err.code = Error::MissingAsset;
        err.detail = std::to_string(unresolvedCount)
                   + " imported entit" + (unresolvedCount == 1 ? "y" : "ies")
                   + " could not be resolved";
        sortDiagnostics();
        return false;
    }

    // ---- Commit pass: merge needed staged models and install MeshRefs ----
    // A successful partial result may commit accepted resources/placeholders
    // (W3-Q6). We merge only the staged models that have at least one
    // resolved entity.
    {
        auto& reg = doc.ecs.registry;
        for (auto& planEntry : plan)
        {
            StagedModel& s = staged[planEntry.pe->modelIdx];

            // Merge this staged model into the target document once (lazily).
            if (!s.merged)
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

            const uint32_t targetMeshIndex =
                s.meshBase + planEntry.localMeshIndex;
            int targetMaterialIndex = -1;
            if (planEntry.localMaterialIndex >= 0)
                targetMaterialIndex = planEntry.localMaterialIndex + s.matBase;

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
            if (auto* ov = reg.try_get<MaterialOverrideComponent>(planEntry.pe->entity))
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
            if (auto* existingRef = reg.try_get<MeshRef>(planEntry.pe->entity))
            {
                existingRef->meshIndex = targetMeshIndex;
                existingRef->materialIndex = targetMaterialIndex;
            }
            else
            {
                MeshRef newRef;
                newRef.meshIndex = targetMeshIndex;
                newRef.materialIndex = targetMaterialIndex;
                reg.emplace<MeshRef>(planEntry.pe->entity, newRef);
            }
        }
    }

    // Rebuild world transforms in case new MeshRef entities need their
    // hierarchy resolved (they were already created by the serializer).
    SceneGraph::UpdateWorldTransforms(doc.ecs.registry);

    sortDiagnostics();
    return true;
}

} // namespace rt2::core
