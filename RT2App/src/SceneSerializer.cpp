#include "SceneSerializer.h"
#include "ECSComponents.h"
#include "PrimitiveGeometry.h"
#include "SceneTypes.h"
#include "RTLog.h"
#include "SceneHierarchy.h"
#include "PersistedComponents.h"
#include "ScriptComponentValidation.h"
#include "AssetResolver.h"
#include "SceneAssetReferenceVisitor.h"
#include "PrefabSerializer.h"

#include "json.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <vector>
#include <set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using json = nlohmann::json;

namespace rt2::core {

static_assert(PersistedComponents::Count == 13,
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
    // Durable source identity (Phase 8 pre-work 2): the loader-minted
    // material key. Additive over v4; omitted entirely for author-created
    // materials, which have no source. Writing it unconditionally added
    // "sourceKey": "" to every material block, so any save rewrote every
    // committed scene asset — churn the fixtures cannot afford.
    if (!m.sourceKey.empty())
        j["sourceKey"]    = m.sourceKey;
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
    if (j.contains("sourceKey"))        m.sourceKey        = j["sourceKey"].get<std::string>();
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
    // Written only when set. The reader defaults to false (spec behaviour),
    // so omitting it keeps every existing scene byte-identical on re-save
    // instead of adding a no-op line to each one.
    if (s.assumeDielectricWithoutMetalRough)
        j["assumeDielectricWithoutMetalRough"] = true;
    return j;
}

ImportSettings JsonToImportSettings(const json& j)
{
    ImportSettings s;
    if (j.contains("triangulate"))     s.triangulate     = j["triangulate"].get<bool>();
    if (j.contains("generateNormals")) s.generateNormals = j["generateNormals"].get<bool>();
    if (j.contains("mergeMegaMesh"))   s.mergeMegaMesh   = j["mergeMegaMesh"].get<bool>();
    if (j.contains("assumeDielectricWithoutMetalRough"))
        s.assumeDielectricWithoutMetalRough =
            j["assumeDielectricWithoutMetalRough"].get<bool>();
    return s;
}

// The asset-kind name codec is inline in AssetReference.h (the neutral CPU-only
// home of AssetKind) so tests and the serializer share one source of truth.

json AssetReferenceToJson(const AssetReference& a)
{
    json j;
    j["kind"]    = AssetKindName(a.kind);
    j["path"]    = a.path;
    j["sourceKey"] = a.sourceKey;
    // Scripts deliberately retain their Phase 6 on-disk shape (no inert
    // settings block) while still using this shared codec for identity.
    // Prefabs likewise carry no import settings (Phase 8 W0 decision: the
    // fixed ImportSettings type has no prefab meaning; an inert block would
    // invite a future reader to treat defaulted knobs as meaningful).
    // Other kinds keep the established shared-codec shape.
    if (a.kind != AssetKind::Script && a.kind != AssetKind::Prefab)
        j["importSettings"] = ImportSettingsToJson(a.importSettings);
    // assetId (Phase 7 W1, additive over v3): written only when assigned.
    // A v3 scene has no assetId field; the loader treats absence as nil.
    if (!a.assetId.IsNull())
        j["assetId"] = a.assetId.ToString();
    return j;
}

AssetReference JsonToAssetReference(const json& j,
                                    uint32_t schemaVersion,
                                    SceneLoadReport* report,
                                    const UUID& entityUuid,
                                    const std::string& entityName,
                                    Error& err)
{
    AssetReference a;
    if (j.contains("kind") && j["kind"].is_string())
        a.kind = AssetKindFromName(j["kind"].get<std::string>());
    if (j.contains("path") && j["path"].is_string())
        a.path = j["path"].get<std::string>();
    if (j.contains("sourceKey") && j["sourceKey"].is_string())
        a.sourceKey = j["sourceKey"].get<std::string>();
    if (j.contains("importSettings"))
        a.importSettings = JsonToImportSettings(j["importSettings"]);
    // assetId is optional on legacy v3 input. A malformed v3 value is kept
    // observable as a migration diagnostic; the same malformed value in v4
    // is a structural parse failure because v4 identity is authoritative.
    if (j.contains("assetId") && j["assetId"].is_string())
        a.assetId = UUID::Parse(j["assetId"].get<std::string>());

    auto recordAssetDiagnostic = [&](AssetDiagnostic::Severity severity,
                                     const std::string& detail) {
        if (!report) return;
        AssetDiagnostic diagnostic;
        diagnostic.severity = severity;
        diagnostic.kind = a.kind;
        diagnostic.refPath = a.path;
        diagnostic.resolvedPath = a.path;
        diagnostic.entityUuid = entityUuid;
        diagnostic.entityName = entityName;
        diagnostic.sourceKey = a.sourceKey;
        diagnostic.detail = detail;
        report->assetDiagnostics.push_back(std::move(diagnostic));
    };

    if (!a.path.empty() && std::filesystem::u8path(a.path).is_absolute())
    {
        if (report) report->hasNonPortableAsset = true;
        recordAssetDiagnostic(AssetDiagnostic::NonPortable,
                              "asset reference uses an absolute path");
    }

    if (!j.contains("assetId") && !a.path.empty())
    {
        if (report) report->requiresAssetMigration = true;
        recordAssetDiagnostic(AssetDiagnostic::Stale,
                              "asset reference has no durable sidecar ID");
    }
    else if (j.contains("assetId"))
    {
        const bool valid = j["assetId"].is_string() &&
            !UUID::Parse(j["assetId"].get<std::string>()).IsNull();
        if (!valid)
        {
            const std::string detail =
                "assetId must be a valid UUID when present";
            if (schemaVersion >= SceneSerializer::SchemaVersion)
            {
                err.code = Error::Parse;
                err.detail = detail;
                return a;
            }
            if (report) report->requiresAssetMigration = true;
            recordAssetDiagnostic(AssetDiagnostic::Malformed, detail);
        }
    }

    if (a.kind == AssetKind::Unknown && !a.path.empty())
    {
        err.code = Error::Parse;
        err.detail = "unknown asset kind for path: " + a.path;
        return a;
    }
    return a;
}

struct RebasedPath
{
    std::string storedPath;
    bool retainedAbsolute = false;
};

// Relativize an absolute or relative path against the .rt2scene directory.
// Stores a portable UTF-8 path with forward slashes. If the path cannot be
// made relative (different drive on Windows), stores the normalized absolute
// path and reports that advisory state to the caller.
RebasedPath RebasePath(const std::string& stored,
                       const std::filesystem::path& currentSceneDir,
                       const std::filesystem::path& outputSceneDir)
{
    if (stored.empty()) return {};
    std::error_code ec;
    std::filesystem::path p(stored);
    p = p.lexically_normal();
    if (!p.is_absolute())
    {
        // Already relative — normalize separators and return.
        if (currentSceneDir.empty()) return { p.generic_string(), false };
        const auto combined = currentSceneDir / p;
        p = std::filesystem::absolute(combined, ec);
        if (ec) p = combined.lexically_normal();
    }
    if (outputSceneDir.empty())
        return { p.generic_string(), p.is_absolute() };
    std::filesystem::path base = outputSceneDir.lexically_normal();
    if (!base.is_absolute())
    {
        ec.clear();
        const auto absoluteBase = std::filesystem::absolute(base, ec);
        if (!ec) base = absoluteBase;
    }
    const std::filesystem::path rel = p.lexically_relative(base);
    if (rel.empty())
        return { p.generic_string(), p.is_absolute() };
    return { rel.lexically_normal().generic_string(), false };
}

void AppendNonPortableDiagnostic(
    const AssetReference& original,
    const RebasedPath& rebased,
    const UUID& entityUuid,
    const std::string& entityName,
    std::vector<AssetDiagnostic>& diagnostics)
{
    if (!rebased.retainedAbsolute)
        return;

    AssetDiagnostic diagnostic;
    diagnostic.severity = AssetDiagnostic::NonPortable;
    diagnostic.kind = original.kind;
    diagnostic.refPath = original.path;
    diagnostic.resolvedPath = rebased.storedPath;
    diagnostic.entityUuid = entityUuid;
    diagnostic.entityName = entityName;
    diagnostic.sourceKey = original.sourceKey;
    diagnostic.detail =
        "asset path could not be made relative to the output scene; "
        "saved as a normalized absolute path";
    diagnostics.push_back(std::move(diagnostic));
}

bool ScriptFieldTypeFromName(const std::string& name, ScriptFieldType& out)
{
    for (size_t i = 0; i < ScriptFieldTypeNames.size(); ++i)
    {
        if (name != ScriptFieldTypeNames[i]) continue;
        out = static_cast<ScriptFieldType>(i);
        return true;
    }
    return false;
}

json ScriptFieldValueToJson(const ScriptFieldEntry& entry)
{
    switch (entry.type)
    {
    case ScriptFieldType::Bool:   return std::get<bool>(entry.value);
    case ScriptFieldType::Int:    return std::get<int64_t>(entry.value);
    case ScriptFieldType::Float:  return std::get<double>(entry.value);
    case ScriptFieldType::String: return std::get<std::string>(entry.value);
    case ScriptFieldType::Uuid:   return std::get<UUID>(entry.value).ToString();
    case ScriptFieldType::Vec3:
    case ScriptFieldType::Color:  return Vec3ToJson(std::get<glm::vec3>(entry.value));
    }
    return nullptr;
}

bool JsonNumberToFiniteDouble(const json& value, double& out)
{
    if (!value.is_number()) return false;
    out = value.get<double>();
    return std::isfinite(out);
}

bool JsonToScriptFieldEntry(const json& serialized,
                            ScriptFieldEntry& out,
                            FieldDiagnostic::Kind& failureKind,
                            std::string& detail,
                            bool& normalized)
{
    normalized = false;
    failureKind = FieldDiagnostic::Kind::MalformedSerializedValue;
    if (!serialized.is_object() || !serialized.contains("type") ||
        !serialized["type"].is_string() || !serialized.contains("value"))
    {
        detail = "field entry must contain a string type and value";
        return false;
    }

    const std::string typeName = serialized["type"].get<std::string>();
    if (!ScriptFieldTypeFromName(typeName, out.type))
    {
        failureKind = FieldDiagnostic::Kind::UnknownSerializedType;
        detail = "unknown serialized script field type '" + typeName + "'";
        return false;
    }

    const auto& value = serialized["value"];
    switch (out.type)
    {
    case ScriptFieldType::Bool:
        if (value.is_boolean()) { out.value = value.get<bool>(); return true; }
        break;
    case ScriptFieldType::Int:
        if (value.is_number_unsigned())
        {
            const uint64_t number = value.get<uint64_t>();
            if (number <= static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()))
            {
                out.value = static_cast<int64_t>(number);
                return true;
            }
        }
        else if (value.is_number_integer())
        {
            out.value = value.get<int64_t>();
            return true;
        }
        break;
    case ScriptFieldType::Float:
    {
        double number = 0.0;
        if (JsonNumberToFiniteDouble(value, number))
        {
            out.value = number;
            return true;
        }
        break;
    }
    case ScriptFieldType::String:
        if (value.is_string()) { out.value = value.get<std::string>(); return true; }
        break;
    case ScriptFieldType::Uuid:
        if (value.is_string())
        {
            const std::string text = value.get<std::string>();
            const UUID parsed = UUID::Parse(text);
            std::string folded = text;
            std::transform(folded.begin(), folded.end(), folded.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const std::string canonical = parsed.ToString();
            if (text.size() == 36 && canonical == folded)
            {
                out.value = parsed;
                normalized = canonical != text;
                return true;
            }
        }
        break;
    case ScriptFieldType::Vec3:
    case ScriptFieldType::Color:
        if (value.is_array() && value.size() == 3)
        {
            double x = 0.0, y = 0.0, z = 0.0;
            if (JsonNumberToFiniteDouble(value[0], x) &&
                JsonNumberToFiniteDouble(value[1], y) &&
                JsonNumberToFiniteDouble(value[2], z) &&
                std::abs(x) <= (std::numeric_limits<float>::max)() &&
                std::abs(y) <= (std::numeric_limits<float>::max)() &&
                std::abs(z) <= (std::numeric_limits<float>::max)())
            {
                out.value = glm::vec3(static_cast<float>(x),
                                      static_cast<float>(y),
                                      static_cast<float>(z));
                return true;
            }
        }
        break;
    }

    detail = "serialized value does not match type '" + typeName + "'";
    return false;
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

    // Phase 6: script component. 6A carries it through clone-in-memory so
    // Play preserves ScriptComponent. 6B adds the v3 serialization (asset
    // path + typed fieldValues) and CloneInMemory uses the same in-memory
    // EntityRecord path, so the component also survives Play/Stop.
    bool hasScript = false;
    ScriptComponent script{};

    // Phase 8 W1: prefab instance link components (v5 schema). The instance
    // root carries PrefabInstanceComponent; every member carries
    // PrefabMemberComponent. Both are authored scene-side components — they
    // are NOT written into .rt2prefab files, whose records carry
    // PrefabEntityRecord::templateId instead.
    bool hasPrefabInstance = false;
    PrefabInstanceComponent prefabInstance{};

    bool hasPrefabMember = false;
    PrefabMemberComponent prefabMember{};
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

    // Phase 6: carry ScriptComponent through the in-memory clone path. 6A
    // serializes it in v3 with its asset path and typed fieldValues.
    if (auto* sc = reg.try_get<ScriptComponent>(e))
    {
        r.hasScript = true;
        r.script    = *sc;
    }

    // Phase 8 W1: carry the prefab instance link components.
    if (auto* pic = reg.try_get<PrefabInstanceComponent>(e))
    {
        r.hasPrefabInstance = true;
        r.prefabInstance    = *pic;
    }

    if (auto* pmc = reg.try_get<PrefabMemberComponent>(e))
    {
        r.hasPrefabMember = true;
        r.prefabMember    = *pmc;
    }

    return r;
}

std::optional<json> EntityRecordToJson(
                        const EntityRecord& r,
                        const std::filesystem::path& currentSceneDir,
                        const std::filesystem::path& outputSceneDir,
                        std::vector<AssetDiagnostic>& diagnostics,
                        Error& err)
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
        const auto rebased =
            RebasePath(relRef.path, currentSceneDir, outputSceneDir);
        AppendNonPortableDiagnostic(
            r.importedSource.model, rebased, r.uuid, r.name, diagnostics);
        relRef.path = rebased.storedPath;
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
        // `type` is authoritative. `isSpot` is still written so a scene saved
        // by this build still opens in one that predates the enum; readers
        // here prefer `type` and fall back to `isSpot`.
        l["type"]        = LightTypeName(r.light.type);
        l["isSpot"]      = (r.light.type == LightType::Spot);
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

    if (r.hasScript)
    {
        ScriptComponent canonical;
        std::string detail;
        std::string field;
        if (!NormalizeAndValidateScriptComponent(
                r.script, canonical, detail, &field))
        {
            err.code = Error::InvalidArgument;
            err.path = r.uuid.ToString();
            if (!field.empty()) err.path += ":" + field;
            err.detail = "invalid ScriptComponent while writing entity: " + detail;
            return std::nullopt;
        }

        AssetReference serializedAsset = canonical.asset;
        const auto rebased = RebasePath(
            serializedAsset.path, currentSceneDir, outputSceneDir);
        AppendNonPortableDiagnostic(
            r.script.asset, rebased, r.uuid, r.name, diagnostics);
        serializedAsset.path = rebased.storedPath;
        serializedAsset.sourceKey = serializedAsset.path.empty()
                                  ? std::string{}
                                  : "lua:asset=" + serializedAsset.path;
        json asset = AssetReferenceToJson(serializedAsset);

        json fields = json::object();
        std::vector<std::string> names;
        names.reserve(canonical.fieldValues.size());
        for (const auto& [name, value] : canonical.fieldValues)
        {
            (void)value;
            names.push_back(name);
        }
        std::sort(names.begin(), names.end());
        for (const auto& name : names)
        {
            const auto& entry = canonical.fieldValues.at(name);
            json serialized;
            serialized["type"] = ScriptFieldTypeName(entry.type);
            serialized["value"] = ScriptFieldValueToJson(entry);
            fields[name] = std::move(serialized);
        }

        json script;
        script["asset"] = std::move(asset);
        script["fields"] = std::move(fields);
        j["script"] = std::move(script);
    }

    // Phase 8 W1 (v5 schema): prefab instance link components. The prefab
    // reference is rebased like every other asset reference so the link
    // survives scene relocation.
    if (r.hasPrefabInstance)
    {
        AssetReference relRef = r.prefabInstance.prefab;
        const auto rebased =
            RebasePath(relRef.path, currentSceneDir, outputSceneDir);
        relRef.path = rebased.storedPath;
        json pi;
        pi["asset"]      = AssetReferenceToJson(relRef);
        pi["instanceId"] = r.prefabInstance.instanceId.ToString();
        j["prefabInstance"] = std::move(pi);
    }

    if (r.hasPrefabMember)
    {
        json pm;
        pm["instanceId"] = r.prefabMember.instanceId.ToString();
        pm["templateId"] = r.prefabMember.templateId.ToString();
        j["prefabMember"] = std::move(pm);
    }

    return j;
}

// Parse an entity from JSON into a record (pass 1 — no entity creation yet).
EntityRecord JsonToEntityRecord(const json& j, uint32_t schemaVersion,
                                Error& err,
                                SceneLoadReport* report)
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
        r.importedSource.model = JsonToAssetReference(
            j["importedSource"], schemaVersion, report, r.uuid, r.name, err);
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
        // Prefer `type`; fall back to the pre-enum `isSpot` so scenes written
        // before this field existed keep their point/spot distinction.
        if (l.contains("type"))
            r.light.type = LightTypeFromName(l["type"].get<std::string>().c_str());
        else if (l.contains("isSpot"))
            r.light.type = l["isSpot"].get<bool>() ? LightType::Spot : LightType::Point;
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

    if (j.contains("script"))
    {
        const auto& script = j["script"];
        if (!script.is_object() || !script.contains("asset") ||
            !script["asset"].is_object())
        {
            err.code = Error::Parse;
            err.path = r.uuid.ToString();
            err.detail = "script must contain an asset object";
            return r;
        }

        const auto& asset = script["asset"];
        if (!asset.contains("kind") || !asset["kind"].is_string() ||
            !asset.contains("path") || !asset["path"].is_string() ||
            !asset.contains("sourceKey") || !asset["sourceKey"].is_string())
        {
            err.code = Error::Parse;
            err.path = r.uuid.ToString();
            err.detail = "script asset requires string kind, path, and sourceKey";
            return r;
        }
        if (asset["kind"].get<std::string>() != "script")
        {
            err.code = Error::Parse;
            err.path = r.uuid.ToString();
            err.detail = "ScriptComponent asset kind must be script";
            return r;
        }
        if (script.contains("fields") && !script["fields"].is_object())
        {
            err.code = Error::Parse;
            err.path = r.uuid.ToString();
            err.detail = "script fields must be an object";
            return r;
        }

        Error assetError;
        AssetReference decodedAsset =
            JsonToAssetReference(asset, schemaVersion, report, r.uuid, r.name,
                                 assetError);
        if (!assetError.IsOk())
        {
            err = assetError;
            err.path = r.uuid.ToString();
            return r;
        }
        if (decodedAsset.kind != AssetKind::Script)
        {
            err.code = Error::Parse;
            err.path = r.uuid.ToString();
            err.detail = "ScriptComponent asset kind must be script";
            return r;
        }

        r.hasScript = true;
        r.script.asset = std::move(decodedAsset);
        const std::string storedSourceKey = asset["sourceKey"].get<std::string>();
        const std::string canonicalSourceKey = r.script.asset.path.empty()
                                             ? std::string{}
                                             : "lua:asset=" + r.script.asset.path;
        const json emptyFields = json::object();
        const json& fields = script.contains("fields")
                           ? script["fields"] : emptyFields;
        if (r.script.asset.path.empty() &&
            (!storedSourceKey.empty() || !fields.empty()))
        {
            err.code = Error::Parse;
            err.path = r.uuid.ToString();
            err.detail = "an unbound script must have an empty sourceKey and fields";
            return r;
        }

        r.script.asset.sourceKey = canonicalSourceKey;
        if (storedSourceKey != canonicalSourceKey && report)
        {
            report->normalizedScriptMetadata = true;
            FieldDiagnostic diagnostic;
            diagnostic.kind = FieldDiagnostic::Kind::NormalizedScriptSourceKey;
            diagnostic.entity = r.uuid;
            diagnostic.message = "entity " + r.uuid.ToString() +
                " script sourceKey was regenerated from its path";
            report->fieldDiagnostics.push_back(std::move(diagnostic));
        }

        for (auto it = fields.begin(); it != fields.end(); ++it)
        {
            ScriptFieldEntry entry;
            FieldDiagnostic::Kind failureKind;
            std::string detail;
            bool normalized = false;
            if (!IsValidScriptFieldName(it.key()))
            {
                failureKind = FieldDiagnostic::Kind::MalformedSerializedValue;
                detail = it.key().empty()
                       ? "field name must not be empty"
                       : "field name must be valid UTF-8";
            }
            else if (JsonToScriptFieldEntry(
                         it.value(), entry, failureKind, detail, normalized))
            {
                r.script.fieldValues.emplace(it.key(), std::move(entry));
                if (normalized && report)
                {
                    report->normalizedScriptFieldData = true;
                    FieldDiagnostic diagnostic;
                    diagnostic.kind = FieldDiagnostic::Kind::NormalizedSerializedUuid;
                    diagnostic.entity = r.uuid;
                    diagnostic.field = it.key();
                    diagnostic.message = "entity " + r.uuid.ToString() +
                        " field '" + it.key() + "' UUID text was normalized";
                    report->fieldDiagnostics.push_back(std::move(diagnostic));
                }
                continue;
            }

            if (report)
            {
                report->droppedScriptFieldData = true;
                FieldDiagnostic diagnostic;
                diagnostic.kind = failureKind;
                diagnostic.entity = r.uuid;
                diagnostic.field = it.key();
                diagnostic.message = "entity " + r.uuid.ToString() +
                    " field '" + it.key() + "' was dropped: " + detail;
                report->fieldDiagnostics.push_back(std::move(diagnostic));
            }
        }
    }

    // Phase 8 W1 (v5 schema): prefab instance link components.
    if (j.contains("prefabInstance"))
    {
        const auto& pi = j["prefabInstance"];
        if (!pi.is_object() || !pi.contains("asset") || !pi["asset"].is_object() ||
            !pi.contains("instanceId") || !pi["instanceId"].is_string())
        {
            err.code = Error::Parse;
            err.path = r.uuid.ToString();
            err.detail = "prefabInstance must contain an asset object and an "
                         "instanceId string";
            return r;
        }

        Error assetError;
        AssetReference decodedPrefab =
            JsonToAssetReference(pi["asset"], schemaVersion, report, r.uuid,
                                 r.name, assetError);
        if (!assetError.IsOk())
        {
            err = assetError;
            err.path = r.uuid.ToString();
            return r;
        }
        if (decodedPrefab.kind != AssetKind::Prefab || !decodedPrefab.IsValid())
        {
            err.code = Error::Parse;
            err.path = r.uuid.ToString();
            err.detail = "prefabInstance asset must be a valid prefab reference";
            return r;
        }

        r.hasPrefabInstance = true;
        r.prefabInstance.prefab = std::move(decodedPrefab);
        r.prefabInstance.instanceId =
            UUID::Parse(pi["instanceId"].get<std::string>());
        if (r.prefabInstance.instanceId.IsNull())
        {
            err.code = Error::Parse;
            err.path = r.uuid.ToString();
            err.detail = "prefabInstance has malformed instanceId";
            return r;
        }
    }

    if (j.contains("prefabMember"))
    {
        const auto& pm = j["prefabMember"];
        if (!pm.is_object() || !pm.contains("instanceId") ||
            !pm["instanceId"].is_string() || !pm.contains("templateId") ||
            !pm["templateId"].is_string())
        {
            err.code = Error::Parse;
            err.path = r.uuid.ToString();
            err.detail = "prefabMember must contain instanceId and templateId "
                         "strings";
            return r;
        }

        r.hasPrefabMember = true;
        r.prefabMember.instanceId =
            UUID::Parse(pm["instanceId"].get<std::string>());
        r.prefabMember.templateId =
            UUID::Parse(pm["templateId"].get<std::string>());
        if (r.prefabMember.instanceId.IsNull() ||
            r.prefabMember.templateId.IsNull())
        {
            err.code = Error::Parse;
            err.path = r.uuid.ToString();
            err.detail = "prefabMember has malformed instanceId or templateId";
            return r;
        }
    }

    return r;
}

// Reconstruct mesh geometry from a primitive and register it.
// Returns the mesh index in the registry.
uint32_t RegisterPrimitiveMeshImpl(MeshRegistry& meshReg, const PrimitiveComponent& prim)
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

        // Script component shared by v3 load and the in-memory Play clone.
        if (r.hasScript)
            doc.ecs.registry.emplace<ScriptComponent>(e, r.script);

        // Prefab instance link components (Phase 8 W1, v5 schema).
        if (r.hasPrefabInstance)
            doc.ecs.registry.emplace<PrefabInstanceComponent>(e, r.prefabInstance);

        if (r.hasPrefabMember)
            doc.ecs.registry.emplace<PrefabMemberComponent>(e, r.prefabMember);
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

// Public wrapper (declared in SceneSerializer.h): shares the exact primitive
// geometry rebuild between the scene load path and the prefab instantiate
// path in SceneManager.cpp.
uint32_t RegisterPrimitiveMesh(MeshRegistry& meshReg, const PrimitiveComponent& prim)
{
    return RegisterPrimitiveMeshImpl(meshReg, prim);
}

// ============================================================================
// Prefab record codec (Phase 8 W1)
//
// Defined here — not in PrefabSerializer.cpp — so it reuses the scene
// per-component codecs above (W0 handover: reuse, don't re-serialise
// components). The envelope plumbing (header/version/atomic write) stays in
// PrefabSerializer.cpp.
//
// HARD RULE: prefab files never contain resource-table indices. After the
// payload is written like a scene entity, the transient fields are stripped:
//   - "meshRef.materialIndex" — MeshRef::materialIndex is transient by design
//   - "materialOverride.material.*TextureIndex" — override texture indices
//     are repaired only from a currently-staged material; a prefab has none
// ("meshIndex" is never written by the scene codec either).
// ============================================================================

namespace {

// SubtreeEntityRecord <-> EntityRecord are field-for-field isomorphic payload
// records; the conversions are mechanical and keep the prefab file shape
// (PrefabEntityRecord wrapping SubtreeEntityRecord, per amendment A1) aligned
// with the shared scene codecs.

EntityRecord ToSceneRecord(const SubtreeEntityRecord& s)
{
    EntityRecord r;
    r.uuid           = s.uuid;
    r.name           = s.name;
    r.parentUuid     = s.parentUuid;
    r.translation    = s.translation;
    r.rotation       = s.rotation;
    r.scale          = s.scale;
    r.visible        = s.visible;
    r.hasMeshRef     = s.hasMeshRef;
    r.meshIndex      = s.meshIndex;
    r.materialIndex  = s.materialIndex;
    r.hasPrimitive   = s.hasPrimitive;
    r.primitive      = s.primitive;
    r.hasImportedSource = s.hasImportedSource;
    r.importedSource    = s.importedSource;
    r.hasMaterialOverride = s.hasMaterialOverride;
    r.materialOverride    = s.materialOverride;
    r.hasLight       = s.hasLight;
    r.light          = s.light;
    r.hasCamera      = s.hasCamera;
    r.camera         = s.camera;
    r.hasMotion      = s.hasMotion;
    r.motion         = s.motion;
    r.hasScript      = s.hasScript;
    r.script         = s.script;
    return r;
}

SubtreeEntityRecord ToSubtreeRecord(const EntityRecord& r)
{
    SubtreeEntityRecord s;
    s.uuid           = r.uuid;
    s.name           = r.name;
    s.parentUuid     = r.parentUuid;
    s.translation    = r.translation;
    s.rotation       = r.rotation;
    s.scale          = r.scale;
    s.visible        = r.visible;
    s.hasMeshRef     = r.hasMeshRef;
    s.meshIndex      = r.meshIndex;
    s.materialIndex  = r.materialIndex;
    s.hasPrimitive   = r.hasPrimitive;
    s.primitive      = r.primitive;
    s.hasImportedSource = r.hasImportedSource;
    s.importedSource    = r.importedSource;
    s.hasMaterialOverride = r.hasMaterialOverride;
    s.materialOverride    = r.materialOverride;
    s.hasLight       = r.hasLight;
    s.light          = r.light;
    s.hasCamera      = r.hasCamera;
    s.camera         = r.camera;
    s.hasMotion      = r.hasMotion;
    s.motion         = r.motion;
    s.hasScript      = r.hasScript;
    s.script         = r.script;
    return s;
}

// The scene codec's texture-index keys inside a material-override material.
void StripTransientIndices(json& recordJson)
{
    if (recordJson.contains("meshRef") && recordJson["meshRef"].is_object())
        recordJson["meshRef"].erase("materialIndex");

    if (!recordJson.contains("materialOverride") ||
        !recordJson["materialOverride"].is_object())
        return;
    auto& mat = recordJson["materialOverride"]["material"];
    if (!mat.is_object())
        return;
    mat.erase("baseColorTextureIndex");
    mat.erase("normalTextureIndex");
    mat.erase("emissiveTextureIndex");
    mat.erase("metallicRoughnessTextureIndex");
}

} // namespace

bool PrefabRecordToJson(const PrefabEntityRecord& record,
                        std::vector<AssetDiagnostic>& diagnostics,
                        Error& err,
                        json& out)
{
    err = Error{};
    if (record.templateId.IsNull())
    {
        err.code = Error::InvalidArgument;
        err.detail = "prefab entity record has a nil templateId";
        return false;
    }

    // The instance-link components are scene-side state. A template entity
    // inside a prefab file never carries them; refuse loudly rather than
    // inventing template semantics for instance data.
    if (record.record.hasPrefabInstance || record.record.hasPrefabMember)
    {
        err.code = Error::InvalidArgument;
        err.path = record.templateId.ToString();
        err.detail = "prefab entity record must not carry scene-side prefab "
                     "instance components";
        return false;
    }

    // Empty dirs: paths are stored verbatim (W1). Absolute paths still get
    // the advisory NonPortable diagnostic via the scene codec.
    const EntityRecord sceneRecord = ToSceneRecord(record.record);
    const auto payload = EntityRecordToJson(
        sceneRecord, std::filesystem::path{}, std::filesystem::path{},
        diagnostics, err);
    if (!payload)
    {
        if (err.path.empty())
            err.path = record.templateId.ToString();
        return false;
    }

    json recordJson = *payload;
    StripTransientIndices(recordJson);

    json j;
    j["templateId"] = record.templateId.ToString();
    j["record"]     = std::move(recordJson);
    out = std::move(j);
    return true;
}

bool JsonToPrefabRecord(const json& j, Error& err, PrefabEntityRecord& out)
{
    err = Error{};
    if (!j.is_object() || !j.contains("templateId") ||
        !j["templateId"].is_string())
    {
        err.code = Error::Parse;
        err.detail = "prefab entity missing templateId field";
        return false;
    }
    const UUID templateId = UUID::Parse(j["templateId"].get<std::string>());
    if (templateId.IsNull())
    {
        err.code = Error::Parse;
        err.detail = "prefab entity has malformed templateId";
        return false;
    }
    if (!j.contains("record") || !j["record"].is_object())
    {
        err.code = Error::Parse;
        err.path = templateId.ToString();
        err.detail = "prefab entity missing record object";
        return false;
    }

    // The scene reader at the current schema version. Stripped indices are
    // simply absent, so meshIndex/materialIndex default (0 / -1).
    const EntityRecord sceneRecord =
        JsonToEntityRecord(j["record"], SceneSerializer::SchemaVersion, err,
                           nullptr);
    if (!err.IsOk())
    {
        if (err.path.empty())
            err.path = templateId.ToString();
        return false;
    }

    PrefabEntityRecord parsed;
    parsed.templateId = templateId;
    parsed.record     = ToSubtreeRecord(sceneRecord);
    out = std::move(parsed);
    return true;
}

// ============================================================================
// Save
// ============================================================================

// Internal save: writes the document to outPath, but relativizes asset
// references against sceneDir (which may differ from outPath's parent when
// writing a recovery snapshot whose logical scene root is elsewhere).
// Pre-save validation + atomic replace are shared by Save and SaveTo.
static bool SaveInternal(const SceneDocument& doc,
                         const std::filesystem::path& outPath,
                         const std::filesystem::path& outputSceneDir,
                         std::vector<AssetDiagnostic>& diagnostics,
                         Error& err,
                         uint32_t outputVersion)
{
    err = Error{};
    std::vector<AssetDiagnostic> stagedDiagnostics;
    // Keep serializer validation on the same durable-reference coverage as
    // migration and portability reporting. A non-empty reference with no
    // kind would otherwise serialize into a scene that cannot be loaded back.
    for (const auto& slot : CollectSceneAssetReferences(doc))
    {
        if (!slot.reference || slot.reference->path.empty() ||
            slot.reference->kind != AssetKind::Unknown)
            continue;
        err.code = Error::InvalidArgument;
        err.path = slot.reference->path;
        err.detail = "asset reference has a path but no asset kind";
        return false;
    }
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

    // Script components are validated before the temp file is opened. The
    // helper canonicalizes redundant sourceKey data in a copy, so a stale key
    // never mutates the live authoring document and never makes it unsaveable.
    {
        const auto& reg = doc.ecs.registry;
        auto view = reg.view<ScriptComponent>();
        for (const auto entity : view)
        {
            ScriptComponent canonical;
            std::string detail;
            std::string field;
            if (NormalizeAndValidateScriptComponent(
                    view.get<ScriptComponent>(entity), canonical, detail, &field))
                continue;

            err.code = Error::InvalidArgument;
            if (const auto* id = reg.try_get<EntityIdComponent>(entity))
                err.path = id->id.ToString();
            else
                err.path = "registry-entity:" +
                    std::to_string(entt::to_integral(entity));
            if (!field.empty()) err.path += ":" + field;
            err.detail = "invalid ScriptComponent: " + detail;
            return false;
        }
    }

    const std::filesystem::path currentSceneDir =
        doc.metadata.sourcePath.empty()
            ? std::filesystem::path{}
            : doc.metadata.sourcePath.parent_path();
    const std::filesystem::path currentReferenceRoot =
        doc.metadata.schemaVersion >= SceneSerializer::SchemaVersion &&
        !doc.metadata.assetRoot.empty()
            ? doc.metadata.assetRoot : currentSceneDir;
    const std::filesystem::path outputReferenceRoot =
        outputVersion >= SceneSerializer::SchemaVersion &&
        !doc.metadata.projectId.IsNull() &&
        !doc.metadata.assetRoot.empty()
            ? doc.metadata.assetRoot : outputSceneDir;

    // Build the JSON document.
    json root;
    root["version"] = outputVersion;

    // Metadata
    {
        json meta;
        meta["name"]       = doc.metadata.name;
        if (outputVersion >= SceneSerializer::SchemaVersion &&
            !doc.metadata.projectId.IsNull())
            meta["projectId"] = doc.metadata.projectId.ToString();
        root["metadata"]   = meta;
    }

    // Entities (sorted by UUID for deterministic output)
    auto records = CollectRecords(doc);
    json entitiesArray = json::array();
    for (const auto& r : records)
    {
        auto serialized = EntityRecordToJson(
            r, currentReferenceRoot, outputReferenceRoot,
            stagedDiagnostics, err);
        if (!serialized) return false;
        entitiesArray.push_back(std::move(*serialized));
    }
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

    // Environment (AssetReference + decoded dimensions; relativize path against
    // scene dir). Phase 7 W3 step 4 remediation: the environment now carries a
    // real AssetReference (W3-Q1/D2), so the env block is serialized through
    // the shared AssetReferenceToJson codec (no duplicate assetId handling).
    // Additive over v3 exactly as before: the v3 fields path/width/height
    // remain, and assetId is still written only when non-nil. The
    // AssetReference-only fields (kind/sourceKey/importSettings) are added
    // by the shared codec; a v3 reader ignores unknown fields and a pre-
    // remediation reader reads only path/width/height/assetId.
    {
        AssetReference relRef = doc.environment.ref;
        relRef.kind = AssetKind::Environment;
        const AssetReference originalRef = relRef;
        const auto rebased =
            RebasePath(relRef.path, currentReferenceRoot,
                       outputReferenceRoot);
        AppendNonPortableDiagnostic(
            originalRef, rebased, UUID::Nil(), {},
            stagedDiagnostics);
        relRef.path = rebased.storedPath;
        json env = AssetReferenceToJson(relRef);
        env["width"]  = doc.environment.width;
        env["height"] = doc.environment.height;
        root["envMap"] = env;
    }

    // Serialize with sorted keys and fixed precision for deterministic output.
    std::string content;
    try
    {
        content = root.dump(2);
    }
    catch (const std::exception& e)
    {
        err.code = Error::InvalidArgument;
        err.path = outPath.string();
        err.detail = std::string("scene contains text that cannot be serialized: ") +
                     e.what();
        return false;
    }

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

    std::stable_sort(
        stagedDiagnostics.begin(), stagedDiagnostics.end(),
        [](const AssetDiagnostic& left, const AssetDiagnostic& right) {
            return AssetDiagnosticSortKey(left) <
                   AssetDiagnosticSortKey(right);
        });
    diagnostics.insert(
        diagnostics.end(),
        std::make_move_iterator(stagedDiagnostics.begin()),
        std::make_move_iterator(stagedDiagnostics.end()));
    return true;
}

bool SceneSerializer::Save(
    const SceneDocument& doc,
    const std::filesystem::path& path,
    std::vector<AssetDiagnostic>& diagnostics,
    Error& err)
{
    return SaveInternal(doc, path, path.parent_path(), diagnostics, err,
                        SceneSerializer::SchemaVersion);
}

bool SceneSerializer::SaveTo(const SceneDocument& doc,
                            const std::filesystem::path& outPath,
                            const std::filesystem::path& logicalScenePath,
                            std::vector<AssetDiagnostic>& diagnostics,
                            Error& err)
{
    // Relativize asset references against the logical scene's directory,
    // not the physical output path. This is the recovery-snapshot path:
    // bytes land under the recovery directory, but durable references
    // remain resolvable against the original authoring scene's root.
    return SaveInternal(
        doc, outPath, logicalScenePath.parent_path(), diagnostics, err,
        doc.metadata.schemaVersion < SceneSerializer::SchemaVersion
            ? doc.metadata.schemaVersion : SceneSerializer::SchemaVersion);
}

// ============================================================================
// Load
// ============================================================================

bool SceneSerializer::Load(SceneDocument& doc, const std::filesystem::path& path, Error& err)
{
    SceneLoadReport ignored;
    return Load(doc, path, ignored, err);
}

bool SceneSerializer::Load(SceneDocument& doc, const std::filesystem::path& path,
                           SceneLoadReport& report, Error& err)
{
    err = Error{};
    report = SceneLoadReport{};
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

    // W5 retains v3 as the explicit migration input and writes v4.
    if (!root.contains("version") || !root["version"].is_number_unsigned())
    {
        err.code = Error::Parse;
        err.path = path.string();
        err.detail = "missing or invalid version field";
        return false;
    }
    uint32_t version = root["version"].get<uint32_t>();
    report.sourceVersion = version;
    if (version < MinReadVersion || version > SchemaVersion)
    {
        err.code = Error::SchemaVersion;
        err.path = path.string();
        err.detail = "unsupported schema version " + std::to_string(version) +
                     " (supported " + std::to_string(MinReadVersion) +
                     " through " + std::to_string(SchemaVersion) + ")";
        return false;
    }
    // Parse entities into records (pass 0 — no entity creation).
    std::vector<EntityRecord> records;
    if (root.contains("entities") && root["entities"].is_array())
    {
        for (const auto& ej : root["entities"])
        {
            EntityRecord r = JsonToEntityRecord(ej, version, err, &report);
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

    // Parse environment (AssetReference + decoded dimensions; pixels are not
    // serialized). Phase 7 W3 step 4 remediation: the env block is parsed
    // through the shared JsonToAssetReference codec (no duplicate assetId
    // handling). Additive over v3: a v3 env block {path,width,height} parses
    // to a nil assetId; the AssetReference-only fields (kind/sourceKey/
    // importSettings) are optional and default sensibly.
    //
    // Item 3 (malformed env assetId must be observable): a present-but-non-
    // string assetId, or a string that parses to UUID::Nil(), is corrupt
    // scene identity. Previously this was silently ignored (non-string) or
    // parsed to nil (malformed string), then treated as a nil-ID fallback
    // by resolution, hiding the corruption. Now it sets Error, records a
    // SceneLoadReport field diagnostic, and fails the load loudly —
    // silent failure is this codebase's characteristic bug class.
    EnvironmentSettings env;
    if (root.contains("envMap"))
    {
        const auto& ej = root["envMap"];
        // Parse the AssetReference portion through the shared codec. A
        // malformed `kind` (unknown asset kind) is a hard parse error
        // already reported by JsonToAssetReference; for the env block the
        // kind is optional and defaults to Unknown, but the env invariant
        // forces Environment below.
        Error refErr;
        env.ref = JsonToAssetReference(
            ej, version, &report, UUID::Nil(), {}, refErr);
        if (!refErr.IsOk())
        {
            err = refErr;
            err.path = path.string();
            err.detail = "malformed envMap asset reference: " + err.detail;
            return false;
        }
        // Force the env invariant regardless of what was serialized.
        env.ref.kind = AssetKind::Environment;

        if (ej.contains("width"))  env.width  = ej["width"].get<int>();
        if (ej.contains("height")) env.height = ej["height"].get<int>();

    }

    // Parse metadata.
    std::string sceneName;
    UUID projectId;
    if (root.contains("metadata"))
    {
        const auto& mj = root["metadata"];
        if (mj.contains("name"))       sceneName  = mj["name"].get<std::string>();
        if (mj.contains("projectId"))
        {
            const bool valid = mj["projectId"].is_string() &&
                !UUID::Parse(mj["projectId"].get<std::string>()).IsNull();
            if (!valid && version >= SchemaVersion)
            {
                err.code = Error::Parse;
                err.path = path.string();
                err.detail = "metadata.projectId must be a valid UUID";
                return false;
            }
            if (valid) projectId = UUID::Parse(
                mj["projectId"].get<std::string>());
            else
                report.requiresAssetMigration = true;
        }
    }

    // Build into a temporary and adopt only after every pass succeeds. This
    // keeps an already-populated destination byte-for-byte intact on failure.
    SceneDocument temp;
    temp.SetUuidProvider(doc.GetUuidProvider());
    if (!BuildDocumentFromRecords(temp, records, materials, camera, env,
                                  version, path, err))
        return false;

    temp.metadata.name = sceneName;
    temp.metadata.projectId = projectId;
    temp.metadata.assetRoot = path.parent_path();
    doc = std::move(temp);
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
    dst.metadata.projectId = src.metadata.projectId;
    dst.metadata.assetRoot = src.metadata.assetRoot;

    // Do NOT copy:
    //   - gpuCache (per-document; runtime builds its own)
    //   - dirty flag (runtime starts clean)
    //   - prevWorldMatrix (initialized by RuntimeSceneController::Play)
    //   - any renderer temporal history
    // Those are initialized after Play() activates the clone.

    return true;
}

} // namespace rt2::core
