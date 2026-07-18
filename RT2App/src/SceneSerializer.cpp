#include "SceneSerializer.h"
#include "ECSComponents.h"
#include "PrimitiveGeometry.h"
#include "SceneTypes.h"
#include "RTLog.h"
#include "SceneHierarchy.h"
#include "PersistedComponents.h"

#include "json.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>
#include <set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using json = nlohmann::json;

namespace rt2::core {

static_assert(PersistedComponents::Count == 10,
              "Update EntityRecord serialization when authored component coverage changes");

// ============================================================================
// Helpers
// ============================================================================

namespace {

// -- JSON conversions for glm types --

json Vec3ToJson(const glm::vec3& v)
{
    return json::array({ v.x, v.y, v.z });
}

glm::vec3 JsonToVec3(const json& j)
{
    if (!j.is_array() || j.size() < 3)
        return {};
    return { j[0].get<float>(), j[1].get<float>(), j[2].get<float>() };
}

json QuatToJson(const glm::quat& q)
{
    return json::array({ q.x, q.y, q.z, q.w });
}

glm::quat JsonToQuat(const json& j)
{
    if (!j.is_array() || j.size() < 4)
        return { 1.0f, 0.0f, 0.0f, 0.0f };
    return { j[3].get<float>(), j[0].get<float>(), j[1].get<float>(), j[2].get<float>() };
}

// -- Primitive kind mapping --

const char* PrimitiveKindName(PrimitiveComponent::Kind k)
{
    switch (k)
    {
        case PrimitiveComponent::Cube:   return "cube";
        case PrimitiveComponent::Sphere: return "sphere";
        case PrimitiveComponent::Plane:  return "plane";
        default:                         return "none";
    }
}

PrimitiveComponent::Kind PrimitiveKindFromName(const std::string& s)
{
    if (s == "cube")   return PrimitiveComponent::Cube;
    if (s == "sphere") return PrimitiveComponent::Sphere;
    if (s == "plane")  return PrimitiveComponent::Plane;
    return PrimitiveComponent::None;
}

// -- Material serialization --

json MaterialToJson(const SceneMaterial& m)
{
    json j;
    j["type"]             = static_cast<int>(m.type);
    j["baseColor"]        = Vec3ToJson(m.baseColor);
    j["baseAlpha"]        = m.baseAlpha;
    j["metallic"]         = m.metallic;
    j["roughness"]        = m.roughness;
    j["ior"]              = m.ior;
    j["transmission"]     = m.transmissionFactor;
    j["emissiveColor"]    = Vec3ToJson(m.emissiveColor);
    j["emissiveIntensity"]= m.emissiveIntensity;
    j["alphaMode"]        = m.alphaMode;
    j["alphaCutoff"]      = m.alphaCutoff;
    // Texture indices are schema-complete but unused in the slice (no textures).
    j["baseColorTex"]     = m.baseColorTextureIndex;
    j["normalTex"]        = m.normalTextureIndex;
    j["emissiveTex"]      = m.emissiveTextureIndex;
    j["metallicRoughTex"] = m.metallicRoughnessTextureIndex;
    return j;
}

SceneMaterial JsonToMaterial(const json& j)
{
    SceneMaterial m;
    if (j.contains("type"))             m.type             = static_cast<MaterialType>(j["type"].get<int>());
    if (j.contains("baseColor"))        m.baseColor        = JsonToVec3(j["baseColor"]);
    if (j.contains("baseAlpha"))        m.baseAlpha        = j["baseAlpha"].get<float>();
    if (j.contains("metallic"))         m.metallic         = j["metallic"].get<float>();
    if (j.contains("roughness"))        m.roughness        = j["roughness"].get<float>();
    if (j.contains("ior"))              m.ior              = j["ior"].get<float>();
    if (j.contains("transmission"))     m.transmissionFactor = j["transmission"].get<float>();
    if (j.contains("emissiveColor"))    m.emissiveColor    = JsonToVec3(j["emissiveColor"]);
    if (j.contains("emissiveIntensity"))m.emissiveIntensity= j["emissiveIntensity"].get<float>();
    if (j.contains("alphaMode"))        m.alphaMode        = j["alphaMode"].get<std::string>();
    if (j.contains("alphaCutoff"))      m.alphaCutoff      = j["alphaCutoff"].get<float>();
    if (j.contains("baseColorTex"))     m.baseColorTextureIndex = j["baseColorTex"].get<int>();
    if (j.contains("normalTex"))        m.normalTextureIndex    = j["normalTex"].get<int>();
    if (j.contains("emissiveTex"))      m.emissiveTextureIndex  = j["emissiveTex"].get<int>();
    if (j.contains("metallicRoughTex")) m.metallicRoughnessTextureIndex = j["metallicRoughTex"].get<int>();
    return m;
}

// -- Camera serialization --

json CameraToJson(const SceneCamera& c)
{
    json j;
    j["position"]   = Vec3ToJson(c.position);
    j["forward"]    = Vec3ToJson(c.forwardDirection);
    j["fov"]        = c.verticalFOV;
    j["aperture"]   = c.aperture;
    j["focusDist"]  = c.focusDistance;
    return j;
}

SceneCamera JsonToCamera(const json& j)
{
    SceneCamera c;
    if (j.contains("position"))  c.position          = JsonToVec3(j["position"]);
    if (j.contains("forward"))   c.forwardDirection  = JsonToVec3(j["forward"]);
    if (j.contains("fov"))       c.verticalFOV       = j["fov"].get<float>();
    if (j.contains("aperture"))  c.aperture          = j["aperture"].get<float>();
    if (j.contains("focusDist")) c.focusDistance     = j["focusDist"].get<float>();
    return c;
}

// -- Entity serialization --
// Collects all entities with an EntityIdComponent, sorts by UUID, serializes
// each entity's components. Returns the entities array + a UUID->index map
// for hierarchy resolution.

// -- Asset reference / import settings serialization (v2) --

json ImportSettingsToJson(const ImportSettings& s)
{
    json j;
    j["triangulate"]     = s.triangulate;
    j["generateNormals"] = s.generateNormals;
    j["mergeMegaMesh"]   = s.mergeMegaMesh;
    return j;
}

ImportSettings JsonToImportSettings(const json& j)
{
    ImportSettings s;
    if (j.contains("triangulate"))     s.triangulate     = j["triangulate"].get<bool>();
    if (j.contains("generateNormals")) s.generateNormals = j["generateNormals"].get<bool>();
    if (j.contains("mergeMegaMesh"))   s.mergeMegaMesh   = j["mergeMegaMesh"].get<bool>();
    return s;
}

const char* AssetKindName(AssetKind k)
{
    switch (k)
    {
        case AssetKind::Model:       return "model";
        case AssetKind::Texture:     return "texture";
        case AssetKind::Environment: return "environment";
        default:                     return "unknown";
    }
}

AssetKind AssetKindFromName(const std::string& s)
{
    if (s == "model")       return AssetKind::Model;
    if (s == "texture")     return AssetKind::Texture;
    if (s == "environment") return AssetKind::Environment;
    return AssetKind::Unknown;
}

json AssetReferenceToJson(const AssetReference& a)
{
    json j;
    j["kind"]    = AssetKindName(a.kind);
    j["path"]    = a.path;
    j["sourceKey"] = a.sourceKey;
    j["importSettings"] = ImportSettingsToJson(a.importSettings);
    return j;
}

AssetReference JsonToAssetReference(const json& j, Error& err)
{
    AssetReference a;
    if (j.contains("kind"))
        a.kind = AssetKindFromName(j["kind"].get<std::string>());
    if (j.contains("path"))
        a.path = j["path"].get<std::string>();
    if (j.contains("sourceKey"))
        a.sourceKey = j["sourceKey"].get<std::string>();
    if (j.contains("importSettings"))
        a.importSettings = JsonToImportSettings(j["importSettings"]);

    if (a.kind == AssetKind::Unknown && !a.path.empty())
    {
        err.code = Error::Parse;
        err.detail = "unknown asset kind for path: " + a.path;
        return a;
    }
    return a;
}

// Relativize an absolute or relative path against the .rt2scene directory.
// Stores a portable UTF-8 path with forward slashes. If the path cannot be
// made relative (different drive on Windows), stores it as-is.
std::string RelativizePath(const std::string& stored, const std::filesystem::path& sceneDir)
{
    if (stored.empty()) return {};
    std::error_code ec;
    std::filesystem::path p(stored);
    p.make_preferred();
    if (!p.is_absolute())
    {
        // Already relative — normalize separators and return.
        std::string s = p.generic_string();
        return s;
    }
    std::filesystem::path rel = std::filesystem::relative(p, sceneDir, ec);
    if (ec || rel.empty())
        return p.generic_string(); // fallback: keep absolute portable form
    return rel.generic_string();
}

struct SerializedEntity
{
    UUID uuid;
    entt::entity entity;
};

struct EntityRecord
{
    UUID uuid;
    std::string name;
    UUID parentUuid;        // nil = no parent
    glm::vec3 translation{};
    glm::quat rotation{1, 0, 0, 0};
    glm::vec3 scale{1, 1, 1};
    bool visible = true;

    bool hasMeshRef = false;
    uint32_t meshIndex = 0;       // transient; NOT serialized in v2
    int materialIndex = -1;

    bool hasPrimitive = false;
    PrimitiveComponent primitive{};

    bool hasImportedSource = false;
    ImportedMeshSourceComponent importedSource{};

    bool hasMaterialOverride = false;
    MaterialOverrideComponent materialOverride{};

    bool hasLight = false;
    LightComponent light{};

    bool hasCamera = false;
    CameraComponent camera{};

    bool hasMotion = false;
    MotionComponent motion{};
};

std::vector<SerializedEntity> CollectEntitiesSorted(const entt::registry& reg)
{
    std::vector<SerializedEntity> entities;
    auto view = reg.view<EntityIdComponent>();
    for (auto e : view)
    {
        const auto& idc = view.get<EntityIdComponent>(e);
        entities.push_back({ idc.id, e });
    }
    std::sort(entities.begin(), entities.end(),
              [](const SerializedEntity& a, const SerializedEntity& b) { return a.uuid < b.uuid; });
    return entities;
}

EntityRecord BuildEntityRecord(const entt::registry& reg, entt::entity e, const UUID& uuid)
{
    EntityRecord r;
    r.uuid = uuid;

    if (auto* nc = reg.try_get<NameComponent>(e))
        r.name = nc->name;

    // Parent UUID
    r.parentUuid = UUID::Nil();
    if (auto* h = reg.try_get<Hierarchy>(e))
    {
        if (h->parent != entt::null && reg.valid(h->parent))
        {
            if (auto* pidc = reg.try_get<EntityIdComponent>(h->parent))
                r.parentUuid = pidc->id;
        }
    }

    if (auto* tf = reg.try_get<Transform>(e))
    {
        r.translation = tf->translation;
        r.rotation    = tf->rotation;
        r.scale       = tf->scale;
    }

    if (auto* vc = reg.try_get<VisibleComponent>(e))
        r.visible = vc->visible;

    if (auto* ref = reg.try_get<MeshRef>(e))
    {
        r.hasMeshRef    = true;
        r.meshIndex     = ref->meshIndex;
        r.materialIndex = ref->materialIndex;
    }

    if (auto* pc = reg.try_get<PrimitiveComponent>(e))
    {
        r.hasPrimitive = true;
        r.primitive    = *pc;
    }

    if (auto* isrc = reg.try_get<ImportedMeshSourceComponent>(e))
    {
        r.hasImportedSource = true;
        r.importedSource    = *isrc;
    }

    if (auto* mov = reg.try_get<MaterialOverrideComponent>(e))
    {
        r.hasMaterialOverride = true;
        r.materialOverride    = *mov;
    }

    if (auto* lc = reg.try_get<LightComponent>(e))
    {
        r.hasLight = true;
        r.light    = *lc;
    }

    if (auto* cc = reg.try_get<CameraComponent>(e))
    {
        r.hasCamera = true;
        r.camera    = *cc;
    }

    if (auto* mc = reg.try_get<MotionComponent>(e))
    {
        r.hasMotion = true;
        r.motion    = *mc;
    }

    return r;
}

json EntityRecordToJson(const EntityRecord& r, const std::filesystem::path& sceneDir)
{
    json j;
    j["uuid"]       = r.uuid.ToString();
    j["name"]       = r.name;
    j["parent"]     = r.parentUuid.IsNull() ? "" : r.parentUuid.ToString();
    j["visible"]    = r.visible;

    {
        json t;
        t["translation"] = Vec3ToJson(r.translation);
        t["rotation"]    = QuatToJson(r.rotation);
        t["scale"]       = Vec3ToJson(r.scale);
        j["transform"]   = t;
    }

    if (r.hasMeshRef)
    {
        json m;
        m["materialIndex"] = r.materialIndex;
        j["meshRef"] = m;
    }

    if (r.hasPrimitive)
    {
        json p;
        p["kind"]    = PrimitiveKindName(r.primitive.kind);
        p["size"]    = r.primitive.size;
        p["segments"]= r.primitive.segments;
        p["rings"]   = r.primitive.rings;
        j["primitive"] = p;
    }

    if (r.hasImportedSource)
    {
        AssetReference relRef = r.importedSource.model;
        relRef.path = RelativizePath(relRef.path, sceneDir);
        j["importedSource"] = AssetReferenceToJson(relRef);
    }

    if (r.hasMaterialOverride)
    {
        json mo;
        mo["authored"]        = r.materialOverride.authored;
        mo["sourceMaterialKey"] = r.materialOverride.sourceMaterialKey;
        mo["material"]        = MaterialToJson(r.materialOverride.material);
        j["materialOverride"] = mo;
    }

    if (r.hasLight)
    {
        json l;
        l["color"]       = Vec3ToJson(r.light.color);
        l["intensity"]   = r.light.intensity;
        l["range"]       = r.light.range;
        l["innerCone"]   = r.light.innerConeAngle;
        l["outerCone"]   = r.light.outerConeAngle;
        l["isSpot"]      = r.light.isSpot;
        j["light"] = l;
    }

    if (r.hasCamera)
    {
        json c;
        c["fov"]         = r.camera.verticalFOV;
        c["aperture"]    = r.camera.aperture;
        c["focusDist"]   = r.camera.focusDistance;
        c["forward"]     = Vec3ToJson(r.camera.forwardDirection);
        j["camera"] = c;
    }

    if (r.hasMotion)
    {
        json m;
        m["velocity"] = Vec3ToJson(r.motion.linearVelocity);
        j["motion"] = m;
    }

    return j;
}

// Parse an entity from JSON into a record (pass 1 — no entity creation yet).
EntityRecord JsonToEntityRecord(const json& j, Error& err)
{
    EntityRecord r;

    if (!j.contains("uuid") || !j["uuid"].is_string())
    {
        err.code = Error::Parse;
        err.detail = "entity missing uuid field";
        return r;
    }
    r.uuid = UUID::Parse(j["uuid"].get<std::string>());
    if (r.uuid.IsNull())
    {
        err.code = Error::Parse;
        err.detail = "entity has malformed uuid";
        return r;
    }

    if (j.contains("name") && j["name"].is_string())
        r.name = j["name"].get<std::string>();

    if (j.contains("parent") && j["parent"].is_string())
    {
        const auto& ps = j["parent"].get<std::string>();
        if (!ps.empty())
            r.parentUuid = UUID::Parse(ps);
    }

    if (j.contains("visible"))
        r.visible = j["visible"].get<bool>();

    if (j.contains("transform"))
    {
        const auto& t = j["transform"];
        if (t.contains("translation")) r.translation = JsonToVec3(t["translation"]);
        if (t.contains("rotation"))    r.rotation    = JsonToQuat(t["rotation"]);
        if (t.contains("scale"))       r.scale       = JsonToVec3(t["scale"]);
    }

    if (j.contains("meshRef"))
    {
        r.hasMeshRef = true;
        const auto& m = j["meshRef"];
        if (m.contains("materialIndex")) r.materialIndex = m["materialIndex"].get<int>();
    }

    if (j.contains("primitive"))
    {
        r.hasPrimitive = true;
        const auto& p = j["primitive"];
        if (p.contains("kind"))    r.primitive.kind     = PrimitiveKindFromName(p["kind"].get<std::string>());
        if (p.contains("size"))    r.primitive.size     = p["size"].get<float>();
        if (p.contains("segments"))r.primitive.segments = p["segments"].get<int>();
        if (p.contains("rings"))   r.primitive.rings    = p["rings"].get<int>();

        if (r.primitive.kind == PrimitiveComponent::None)
        {
            err.code = Error::UnknownPrimitive;
            err.path = r.uuid.ToString();
            err.detail = "unknown primitive kind in entity";
            return r;
        }
    }

    if (j.contains("importedSource"))
    {
        r.hasImportedSource = true;
        r.importedSource.model = JsonToAssetReference(j["importedSource"], err);
        if (!err.IsOk())
        {
            err.path = r.uuid.ToString();
            return r;
        }
        if (!r.importedSource.model.IsValid())
        {
            err.code = Error::Parse;
            err.path = r.uuid.ToString();
            err.detail = "malformed importedSource reference on entity";
            return r;
        }
    }

    if (j.contains("materialOverride"))
    {
        r.hasMaterialOverride = true;
        const auto& mo = j["materialOverride"];
        if (mo.contains("authored"))          r.materialOverride.authored      = mo["authored"].get<bool>();
        if (mo.contains("sourceMaterialKey")) r.materialOverride.sourceMaterialKey = mo["sourceMaterialKey"].get<std::string>();
        if (mo.contains("material"))          r.materialOverride.material     = JsonToMaterial(mo["material"]);
        // materialIndex is transient — not read from the file as identity.
    }

    if (j.contains("light"))
    {
        r.hasLight = true;
        const auto& l = j["light"];
        if (l.contains("color"))      r.light.color           = JsonToVec3(l["color"]);
        if (l.contains("intensity"))  r.light.intensity       = l["intensity"].get<float>();
        if (l.contains("range"))      r.light.range           = l["range"].get<float>();
        if (l.contains("innerCone"))  r.light.innerConeAngle  = l["innerCone"].get<float>();
        if (l.contains("outerCone"))  r.light.outerConeAngle  = l["outerCone"].get<float>();
        if (l.contains("isSpot"))     r.light.isSpot          = l["isSpot"].get<bool>();
    }

    if (j.contains("camera"))
    {
        r.hasCamera = true;
        const auto& c = j["camera"];
        if (c.contains("fov"))       r.camera.verticalFOV     = c["fov"].get<float>();
        if (c.contains("aperture"))  r.camera.aperture        = c["aperture"].get<float>();
        if (c.contains("focusDist")) r.camera.focusDistance   = c["focusDist"].get<float>();
        if (c.contains("forward"))   r.camera.forwardDirection= JsonToVec3(c["forward"]);
    }

    if (j.contains("motion"))
    {
        r.hasMotion = true;
        const auto& m = j["motion"];
        if (m.contains("velocity")) r.motion.linearVelocity = JsonToVec3(m["velocity"]);
    }

    return r;
}

// Reconstruct mesh geometry from a primitive and register it.
// Returns the mesh index in the registry.
uint32_t RegisterPrimitiveMesh(MeshRegistry& meshReg, const PrimitiveComponent& prim)
{
    MeshData meshData;
    switch (prim.kind)
    {
        case PrimitiveComponent::Cube:
            meshData = PrimitiveGeometry::CreateCube(prim.size);
            meshData.name = "cube";
            break;
        case PrimitiveComponent::Sphere:
            meshData = PrimitiveGeometry::CreateSphere(prim.size * 0.5f, prim.segments, prim.rings);
            meshData.name = "sphere";
            break;
        case PrimitiveComponent::Plane:
            meshData = PrimitiveGeometry::CreatePlane(prim.size);
            meshData.name = "plane";
            break;
        default:
            return 0;
    }
    return meshReg.AddMesh(std::move(meshData));
}

// Build a complete document from a list of entity records.
// This is the shared core of both Load and CloneInMemory.
//
// When `preserveMeshIndices` is false (file Load), primitive meshes are re-
// registered from PrimitiveComponent data and imported entities get meshIndex=0
// (the resolver repairs it later). When true (CloneInMemory), the caller has
// already copied the source mesh registry and textures into `doc`, and record
// meshIndex values are used as-is so resolved imported meshes stay valid.
bool BuildDocumentFromRecords(SceneDocument& doc,
                              const std::vector<EntityRecord>& records,
                              const std::vector<SceneMaterial>& materials,
                              const SceneCamera& camera,
                              const EnvironmentSettings& env,
                              uint32_t schemaVersion,
                              const std::filesystem::path& sourcePath,
                              Error& err,
                              bool preserveMeshIndices = false)
{
    // The document should already be cleared by the caller.

    // --- Pass 1: create entities + components, build UUID index ---
    std::unordered_map<UUID, entt::entity> uuidToEntity;

    for (const auto& r : records)
    {
        if (uuidToEntity.count(r.uuid))
        {
            err.code = Error::DuplicateUuid;
            err.path = r.uuid.ToString();
            err.detail = "duplicate UUID in scene data";
            return false;
        }

        entt::entity e = doc.ecs.registry.create();

        // EntityIdComponent with the preserved UUID.
        doc.ecs.registry.emplace<EntityIdComponent>(e, EntityIdComponent{ r.uuid });
        uuidToEntity[r.uuid] = e;

        // Name
        if (!r.name.empty())
            doc.ecs.registry.emplace<NameComponent>(e, r.name);

        // Transform
        Transform tf;
        tf.translation = r.translation;
        tf.rotation    = r.rotation;
        tf.scale       = r.scale;
        tf.dirty       = true;
        doc.ecs.registry.emplace<Transform>(e, tf);

        // Visible
        doc.ecs.registry.emplace<VisibleComponent>(e, VisibleComponent{ r.visible });

        // Primitive + MeshRef
        if (r.hasPrimitive)
        {
            doc.ecs.registry.emplace<PrimitiveComponent>(e, r.primitive);

            MeshRef ref;
            if (preserveMeshIndices)
            {
                // Clone path: the source mesh registry was copied wholesale,
                // so the record's meshIndex is already valid in doc.
                ref.meshIndex     = r.meshIndex;
            }
            else
            {
                // File load path: rebuild primitive geometry from the record
                // and register it fresh.
                ref.meshIndex     = RegisterPrimitiveMesh(doc.ecs.meshRegistry, r.primitive);
            }
            ref.materialIndex = r.hasMeshRef ? r.materialIndex : 0;
            doc.ecs.registry.emplace<MeshRef>(e, ref);
        }
        else if (r.hasImportedSource)
        {
            // Imported geometry: attach durable provenance. The resolver
            // rebuilds MeshRef after loading the source asset. The
            // materialIndex from the record (if any) is preserved so the
            // resolver can reapply it.
            doc.ecs.registry.emplace<ImportedMeshSourceComponent>(e, r.importedSource);
            if (r.hasMeshRef)
            {
                // Preserve the authored materialIndex and the current
                // meshIndex. For a file load the meshIndex is 0 (transient)
                // and the resolver repairs it. For CloneInMemory the
                // meshIndex is the already-resolved index into the source
                // document's mesh registry, which CloneInMemory copies
                // verbatim, so the clone's MeshRef stays valid without
                // re-running the resolver.
                MeshRef ref;
                ref.meshIndex     = r.meshIndex;
                ref.materialIndex = r.materialIndex;
                doc.ecs.registry.emplace<MeshRef>(e, ref);
            }
        }
        else if (r.hasMeshRef)
        {
            // MeshRef with neither PrimitiveComponent nor
            // ImportedMeshSourceComponent — cannot reopen. This is the v1
            // rejection path, retained so v1 scenes that somehow lost their
            // primitive identity still fail clearly.
            err.code = Error::UnknownPrimitive;
            err.path = r.uuid.ToString();
            err.detail = "entity has MeshRef but no PrimitiveComponent or "
                         "importedSource (cannot reopen)";
            return false;
        }

        // Material override (authored edits to an imported material).
        if (r.hasMaterialOverride)
            doc.ecs.registry.emplace<MaterialOverrideComponent>(e, r.materialOverride);

        // Light
        if (r.hasLight)
            doc.ecs.registry.emplace<LightComponent>(e, r.light);

        // Camera
        if (r.hasCamera)
            doc.ecs.registry.emplace<CameraComponent>(e, r.camera);

        // Motion
        if (r.hasMotion)
            doc.ecs.registry.emplace<MotionComponent>(e, r.motion);
    }

    // --- Pass 2: resolve parent UUIDs to Hierarchy ---
    for (const auto& r : records)
    {
        if (r.parentUuid.IsNull())
            continue;

        auto it = uuidToEntity.find(r.parentUuid);
        if (it == uuidToEntity.end())
        {
            err.code = Error::MissingParent;
            err.path = r.uuid.ToString();
            err.detail = "parent UUID not found: " + r.parentUuid.ToString();
            return false;
        }

        entt::entity child  = uuidToEntity[r.uuid];
        entt::entity parent = it->second;

        auto& childHier  = doc.ecs.registry.emplace<Hierarchy>(child);
        childHier.parent = parent;

    }

    if (!SceneHierarchy::RebuildChildren(doc.ecs.registry, err))
        return false;

    // --- Materials ---
    doc.ecs.materials = materials;

    // --- Camera ---
    doc.ecs.camera = camera;

    // --- Environment ---
    doc.environment = env;

    // --- Metadata ---
    doc.metadata.schemaVersion = schemaVersion;
    doc.metadata.sourcePath    = sourcePath;
    doc.metadata.dirty         = false;

    // --- Build UUID index ---
    for (const auto& [uuid, entity] : uuidToEntity)
        doc.uuidIndex.Insert(uuid, entity);

    // --- Validate ---
    if (!doc.ValidateUniqueUuids(err))
        return false;

    return true;
}

// Collect all records from a source document (shared by Save and CloneInMemory).
std::vector<EntityRecord> CollectRecords(const SceneDocument& doc)
{
    auto entities = CollectEntitiesSorted(doc.ecs.registry);
    std::vector<EntityRecord> records;
    records.reserve(entities.size());
    for (const auto& se : entities)
        records.push_back(BuildEntityRecord(doc.ecs.registry, se.entity, se.uuid));
    return records;
}

} // anonymous namespace

// ============================================================================
// Save
// ============================================================================

// Internal save: writes the document to outPath, but relativizes asset
// references against sceneDir (which may differ from outPath's parent when
// writing a recovery snapshot whose logical scene root is elsewhere).
// Pre-save validation + atomic replace are shared by Save and SaveTo.
static bool SaveInternal(const SceneDocument& doc,
                         const std::filesystem::path& outPath,
                         const std::filesystem::path& sceneDir,
                         Error& err)
{
    // Pre-save validation: every entity with a MeshRef must have either a
    // PrimitiveComponent (procedural) or an ImportedMeshSourceComponent
    // (durable asset reference). Entities with neither cannot reopen —
    // never silently save a native scene that cannot be loaded back.
    {
        auto& reg = doc.ecs.registry;
        auto view = reg.view<MeshRef>();
        std::string offenders;
        int count = 0;
        for (auto e : view)
        {
            if (!reg.all_of<PrimitiveComponent>(e) &&
                !reg.all_of<ImportedMeshSourceComponent>(e))
            {
                ++count;
                if (auto* idc = reg.try_get<EntityIdComponent>(e))
                {
                    if (!offenders.empty()) offenders += ", ";
                    offenders += idc->id.ToString();
                    if (auto* nc = reg.try_get<NameComponent>(e))
                        offenders += " (" + nc->name + ")";
                }
            }
        }
        if (count > 0)
        {
            err.code = Error::UnknownPrimitive;
            err.path = outPath.string();
            err.detail = std::to_string(count) + " entit" +
                         (count == 1 ? "y" : "ies") +
                         " with mesh geometry but no PrimitiveComponent or "
                         "importedSource cannot be saved to .rt2scene: " + offenders +
                         ". Add a PrimitiveComponent or import the mesh so it "
                         "carries durable provenance.";
            return false;
        }
    }

    // Build the JSON document.
    json root;
    root["version"] = SceneSerializer::SchemaVersion;

    // Metadata
    {
        json meta;
        meta["sourcePath"] = doc.metadata.sourcePath.string();
        meta["name"]       = doc.metadata.name;
        root["metadata"]   = meta;
    }

    // Entities (sorted by UUID for deterministic output)
    auto records = CollectRecords(doc);
    json entitiesArray = json::array();
    for (const auto& r : records)
        entitiesArray.push_back(EntityRecordToJson(r, sceneDir));
    root["entities"] = entitiesArray;

    // Materials
    {
        json mats = json::array();
        for (const auto& m : doc.ecs.materials)
            mats.push_back(MaterialToJson(m));
        root["materials"] = mats;
    }

    // Textures (schema-complete; pixel data is never serialized)
    root["textures"] = json::array();

    // Camera
    root["camera"] = CameraToJson(doc.ecs.camera);

    // Environment (path only; relativize against scene dir)
    {
        json env;
        env["path"]   = RelativizePath(doc.environment.path, sceneDir);
        env["width"]  = doc.environment.width;
        env["height"] = doc.environment.height;
        root["envMap"] = env;
    }

    // Serialize with sorted keys and fixed precision for deterministic output.
    std::string content = root.dump(2);

    // --- Atomic save ---
    // Write to a temp sibling file, then atomically replace the target.
    std::filesystem::path tmpPath = outPath;
    tmpPath += ".tmp";

    {
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            err.code = Error::Io;
            err.path = tmpPath.string();
            err.detail = "failed to open temp file for writing";
            return false;
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!out)
        {
            err.code = Error::Io;
            err.path = tmpPath.string();
            err.detail = "failed while writing temp file";
            out.close();
            std::filesystem::remove(tmpPath);
            return false;
        }
        out.close();
    }

    // Replace the target atomically.
#ifdef _WIN32
    // Convert paths to wide strings for the Windows API.
    auto toWide = [](const std::filesystem::path& p) {
        return p.wstring();
    };
    std::wstring wTarget = toWide(outPath);
    std::wstring wTmp    = toWide(tmpPath);

    // If the target exists, use ReplaceFileW (atomic replace).
    if (std::filesystem::exists(outPath))
    {
        BOOL ok = ReplaceFileW(wTarget.c_str(), wTmp.c_str(), nullptr,
                               REPLACEFILE_WRITE_THROUGH, nullptr, nullptr);
        if (!ok)
        {
            // ReplaceFileW can fail if the target is read-only or locked.
            // Fall back to MoveFileExW with replace semantics.
            if (!MoveFileExW(wTmp.c_str(), wTarget.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                err.code = Error::Io;
                err.path = outPath.string();
                err.detail = "ReplaceFileW and MoveFileExW both failed";
                std::filesystem::remove(tmpPath);
                return false;
            }
        }
    }
    else
    {
        // Target doesn't exist — just rename.
        if (!MoveFileExW(wTmp.c_str(), wTarget.c_str(), MOVEFILE_WRITE_THROUGH))
        {
            err.code = Error::Io;
            err.path = outPath.string();
            err.detail = "MoveFileExW failed for new file";
            std::filesystem::remove(tmpPath);
            return false;
        }
    }
#else
    // Portable fallback (not the primary path on Windows).
    std::error_code ec;
    std::filesystem::rename(tmpPath, outPath, ec);
    if (ec)
    {
        err.code = Error::Io;
        err.path = outPath.string();
        err.detail = "filesystem::rename failed: " + ec.message();
        std::filesystem::remove(tmpPath);
        return false;
    }
#endif

    return true;
}

bool SceneSerializer::Save(const SceneDocument& doc, const std::filesystem::path& path, Error& err)
{
    return SaveInternal(doc, path, path.parent_path(), err);
}

bool SceneSerializer::SaveTo(const SceneDocument& doc,
                            const std::filesystem::path& outPath,
                            const std::filesystem::path& logicalScenePath,
                            Error& err)
{
    // Relativize asset references against the logical scene's directory,
    // not the physical output path. This is the recovery-snapshot path:
    // bytes land under the recovery directory, but durable references
    // remain resolvable against the original authoring scene's root.
    return SaveInternal(doc, outPath, logicalScenePath.parent_path(), err);
}

// ============================================================================
// Load
// ============================================================================

bool SceneSerializer::Load(SceneDocument& doc, const std::filesystem::path& path, Error& err)
{
    // Read the file.
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        err.code = Error::Io;
        err.path = path.string();
        err.detail = "failed to open file for reading";
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string content = ss.str();

    // Parse JSON.
    json root;
    try
    {
        root = json::parse(content);
    }
    catch (const std::exception& e)
    {
        err.code = Error::Parse;
        err.path = path.string();
        err.detail = std::string("JSON parse error: ") + e.what();
        return false;
    }

    // Schema version check. v1 and v2 are accepted; v1 is migrated in memory
    // to v2 (no UUID/transform/material changes). v1 scenes are primitive-
    // only and carry no asset references, so they load identically under
    // v2 semantics — the only difference is the version field written back
    // on save.
    if (!root.contains("version") || !root["version"].is_number_unsigned())
    {
        err.code = Error::Parse;
        err.path = path.string();
        err.detail = "missing or invalid version field";
        return false;
    }
    uint32_t version = root["version"].get<uint32_t>();
    if (version < MinReadVersion || version > SchemaVersion)
    {
        err.code = Error::SchemaVersion;
        err.path = path.string();
        err.detail = "unsupported schema version " + std::to_string(version) +
                     " (supported " + std::to_string(MinReadVersion) + ".." +
                     std::to_string(SchemaVersion) + ")";
        return false;
    }
    // Treat v1 as v2 for in-memory construction. The serialized output is
    // always v2 (see Save), so v1 inputs are migrated on save.
    const uint32_t effectiveVersion = SchemaVersion;

    // Parse entities into records (pass 0 — no entity creation).
    std::vector<EntityRecord> records;
    if (root.contains("entities") && root["entities"].is_array())
    {
        for (const auto& ej : root["entities"])
        {
            EntityRecord r = JsonToEntityRecord(ej, err);
            if (!err.IsOk())
            {
                err.path = path.string();
                return false;
            }
            records.push_back(r);
        }
    }

    // Parse materials.
    std::vector<SceneMaterial> materials;
    if (root.contains("materials") && root["materials"].is_array())
    {
        for (const auto& mj : root["materials"])
            materials.push_back(JsonToMaterial(mj));
    }

    // Parse camera.
    SceneCamera camera;
    if (root.contains("camera"))
        camera = JsonToCamera(root["camera"]);

    // Parse environment (path only; pixels are not serialized).
    EnvironmentSettings env;
    if (root.contains("envMap"))
    {
        const auto& ej = root["envMap"];
        if (ej.contains("path"))   env.path   = ej["path"].get<std::string>();
        if (ej.contains("width"))  env.width  = ej["width"].get<int>();
        if (ej.contains("height")) env.height = ej["height"].get<int>();
    }

    // Parse metadata.
    std::filesystem::path sourcePath = path;
    std::string sceneName;
    if (root.contains("metadata"))
    {
        const auto& mj = root["metadata"];
        if (mj.contains("sourcePath")) sourcePath = mj["sourcePath"].get<std::string>();
        if (mj.contains("name"))       sceneName  = mj["name"].get<std::string>();
    }

    // Build the document into a temporary first, then swap on success.
    // Since `doc` is already cleared by the caller, we build directly into
    // it. On failure, we clear it to avoid partial state.
    doc.Clear();

    if (!BuildDocumentFromRecords(doc, records, materials, camera, env,
                                  effectiveVersion, sourcePath, err))
    {
        doc.Clear();
        return false;
    }

    doc.metadata.name = sceneName;
    return true;
}

// ============================================================================
// CloneInMemory
// ============================================================================

bool SceneSerializer::CloneInMemory(const SceneDocument& src, SceneDocument& dst, Error& err)
{
    // Collect records from the source — same path as Save, but no file I/O.
    auto records = CollectRecords(src);

    // Preserve the UUID provider from dst (it may have been set by the caller
    // for the runtime document). Save it, clear dst, then restore.
    IUuidProvider* provider = dst.GetUuidProvider();

    dst.Clear();
    dst.SetUuidProvider(provider);

    // Clone path: copy the source mesh registry and textures verbatim so the
    // record meshIndex/textureIndex values stay valid without re-running the
    // resolver. BuildDocumentFromRecords is told to preserve mesh indices.
    for (uint32_t i = 0; i < src.ecs.meshRegistry.GetCount(); ++i)
        dst.ecs.meshRegistry.AddMesh(src.ecs.meshRegistry.GetMesh(i));
    dst.ecs.textures = src.ecs.textures;

    // Build into dst. On failure, clear to avoid partial state.
    if (!BuildDocumentFromRecords(dst, records, src.ecs.materials, src.ecs.camera,
                                  src.environment, src.metadata.schemaVersion,
                                  src.metadata.sourcePath, err,
                                  /*preserveMeshIndices=*/true))
    {
        dst.Clear();
        dst.SetUuidProvider(provider);
        return false;
    }

    // Copy the scene name (metadata.sourcePath is already set by BuildDocument).
    dst.metadata.name = src.metadata.name;

    // Do NOT copy:
    //   - gpuCache (per-document; runtime builds its own)
    //   - dirty flag (runtime starts clean)
    //   - prevWorldMatrix (initialized by RuntimeSceneController::Play)
    //   - any renderer temporal history
    // Those are initialized after Play() activates the clone.

    return true;
}

} // namespace rt2::core
