#pragma once

#ifndef ECS_COMPONENTS_H
#define ECS_COMPONENTS_H

#include "AssetReference.h"
#include "core/UUID.h"
#include "SceneTypes.h"
#include "ScriptFieldValue.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/constants.hpp>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <unordered_map>
#include <vector>
#include <string>
#include <cstdint>

// Forward declaration only. PrefabComponentKey is defined in
// PrefabComponentKey.h, which is included at the bottom of this header (after
// every persisted component struct is defined) to break the include cycle:
// PrefabComponentKey.h -> PersistedComponents.h -> ECSComponents.h. The vector
// member below needs only the declaration here; its special members are
// instantiated later, by which point the header is complete.
class PrefabComponentKey;

// ============================================================================
// ECS Components
//
// Components are plain data structs stored in the entt registry. Systems
// operate on entities that have the relevant components. An entity is just
// an entt::entity (uint32_t ID).
//
// Design principles:
// - Components are POD (plain old data) — no logic, no Vulkan/CPU-renderer deps
// - Transforms use TRS (translation/rotation/scale) for local space
// - World transforms are computed by the SceneGraph system
// - Mesh data (vertices, indices, UVs, tangents) lives in a separate
//   MeshRegistry keyed by mesh index, and MeshRef components point into it
//
// ============================================================================

// Transform — local TRS relative to parent entity (or world if no parent)
struct Transform
{
    glm::vec3 translation = {0.0f, 0.0f, 0.0f};
    glm::quat rotation = {1.0f, 0.0f, 0.0f, 0.0f};  // identity = no rotation
    glm::vec3 scale = {1.0f, 1.0f, 1.0f};

    // Computed world matrix (updated by SceneGraph system)
    glm::mat4 worldMatrix = glm::mat4(1.0f);
    glm::mat4 prevWorldMatrix = glm::mat4(1.0f);  // for motion vectors
    bool dirty = true;  // needs world matrix recomputation

    // Convenience: compute local TRS matrix
    glm::mat4 localMatrix() const
    {
        glm::mat4 t = glm::translate(glm::mat4(1.0f), translation);
        glm::mat4 r = glm::mat4(rotation);
        glm::mat4 s = glm::scale(glm::mat4(1.0f), scale);
        return t * r * s;
    }
};

// Parent-child relationship for scene graph hierarchy.
// The Hierarchy component is attached to entities that have children.
struct Hierarchy
{
    entt::entity parent = entt::null;
    std::vector<entt::entity> children;
};

// Reference to a unique mesh stored in the MeshRegistry.
// Multiple entities can reference the same meshIndex (instancing).
struct MeshRef
{
    uint32_t meshIndex = 0;      // index into MeshRegistry
    int materialIndex = -1;      // -1 = use per-triangle material indices from mesh,
                                 // >=0 = override all triangles with this material
};

// Light component for point/spot lights (CPU-side, not emissive triangles)
struct LightComponent
{
    glm::vec3 color = {1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 50.0f;
    float innerConeAngle = 30.0f;
    float outerConeAngle = 45.0f;
    // Replaces the former `bool isSpot`. Point and Spot behave as before;
    // Directional is a parallel-ray light whose position is ignored and
    // whose direction comes from the entity's world rotation.
    LightType type = LightType::Point;
};

// A light entity's aim lives in its Transform's rotation, not in the light
// component — that is the point of making lights entities, so moving or
// parenting one just works. glTF punctual lights emit along local -Z, so
// these convert between that convention and a rotation.
//
// Round-tripping direction -> rotation -> direction is exact for any unit
// direction; the reverse (rotation -> direction -> rotation) discards roll,
// which a light has no use for.
inline glm::quat LightDirectionToRotation(const glm::vec3& direction)
{
    const glm::vec3 forward(0.0f, 0.0f, -1.0f);
    const float len = glm::length(direction);
    if (len < 1e-8f)
        return glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // degenerate: keep identity

    const glm::vec3 d = direction / len;
    const float dot = glm::dot(forward, d);
    if (dot > 1.0f - 1e-6f)
        return glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // already pointing down -Z
    if (dot < -1.0f + 1e-6f)
        // Exactly reversed: the rotation axis is ambiguous, so pick one.
        return glm::angleAxis(glm::pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));

    return glm::angleAxis(std::acos(dot),
                          glm::normalize(glm::cross(forward, d)));
}

inline glm::vec3 LightRotationToDirection(const glm::quat& rotation)
{
    return glm::normalize(rotation * glm::vec3(0.0f, 0.0f, -1.0f));
}

// Camera component (only one entity should have this)
struct CameraComponent
{
    float verticalFOV = 45.0f;
    float aperture = 0.0f;
    float focusDistance = 1.0f;
    glm::vec3 forwardDirection = {0.0f, 0.0f, -1.0f};
};

// Name tag for debugging/UI
struct NameComponent
{
    std::string name;
};

// Marks an entity as visible for rendering
struct VisibleComponent
{
    bool visible = true;
};

// Stable authored identity. Every entity that participates in scene
// persistence, runtime cloning, or cross-entity references carries one.
// The UUID is generated by rt2::core::IUuidProvider and indexed by
// SceneDocument::uuidIndex. Runtime clones preserve UUIDs; duplicated
// subtrees receive fresh v4 UUIDs.
struct EntityIdComponent
{
    rt2::core::UUID id;
};

// Persisted primitive mesh source. The vertical slice serializer only
// round-trips entities with a PrimitiveComponent — non-primitive (glTF/OBJ)
// meshes are rejected with Error{UnknownPrimitive}. On load, the serializer
// reconstructs MeshData via PrimitiveGeometry::Create* and registers it in
// MeshRegistry, then creates/updates the MeshRef.
struct PrimitiveComponent
{
    enum Kind : uint8_t
    {
        None    = 0,
        Cube    = 1,
        Sphere  = 2,
        Plane   = 3,
    };

    Kind  kind       = None;
    float size       = 1.0f;   // cube edge / sphere diameter / plane edge
    int   segments   = 24;     // sphere longitude segments
    int   rings      = 16;     // sphere latitude rings
};

// Vertical-slice test behavior: constant linear velocity applied by
// MotionSystem::FixedUpdate during Play. This is a temporary vehicle for
// validating persistence, runtime cloning, lifecycle, and transform
// propagation — it must not become the long-term scripting API.
struct MotionComponent
{
    glm::vec3 linearVelocity{0.0f, 0.0f, 0.0f};
};

// ============================================================================
// Phase 1A — durable asset-reference layer
//
// These components persist durable source provenance for imported content so
// a saved .rt2scene can rebuild the same meshes/materials/textures after
// restart. They store ONLY durable references — never decoded vertex buffers,
// pixel data, GPU handles, BLAS/TLAS state, or transient MeshRegistry
// indices. SceneAssetResolver reads these on load and rebuilds runtime state.
//
// Invariants:
//   - MeshRef::meshIndex is transient. Do not persist it as identity.
//   - Imported geometry must have enough provenance to rebuild the same
//     mesh/material/texture association after reopening.
//   - glTF node/primitive identity is durable (scene/node/mesh/primitive
//     indices in the source file); it must not depend on EnTT entity values
//     or current MeshRegistry ordering.
//   - The merged OBJ importer has a deterministic source identity and
//     persisted import profile sufficient to recreate its geometry.
//   - Procedural PrimitiveComponent entities remain directly serializable and
//     do NOT carry an ImportedMeshSourceComponent.
//   - No Phase 7 global asset UUIDs or asset database is introduced here.
//
// AssetKind, ImportSettings, and AssetReference live in AssetReference.h
// (neutral, CPU-only) so runtime types such as SceneTexture and
// EnvironmentSettings can carry an AssetReference without an
// ECSComponents.h/entt dependency. ECSComponents.h re-exports them unchanged.
// ============================================================================

// Durable provenance for a single imported mesh entity. Identifies which
// subresource of a source model produced this entity's geometry so the
// resolver can rebuild the same MeshRef after reopening.
//
// sourceKey formats (deterministic, importer-defined):
//   - glTF:  "gltf:scene=<s>:node=<n>:mesh=<m>:primitive=<p>"
//            (indices into the source glTF file, stable across loads)
//   - OBJ:   "obj:whole-model"  (the merged OBJ mega-mesh is a single
//            subresource; the importer profile is persisted alongside)
struct ImportedMeshSourceComponent
{
    AssetReference model;
};

// Authored material override for a single imported entity. The resolver
// rebuilds source-imported materials first, then applies any serialized
// authored override on top. Precedence: authored override wins over source
// material.
//
// The override stores a FULL material value snapshot so it survives reopen
// without depending on a transient material-slot index. The `materialIndex`
// field is a transient post-resolution index filled by the resolver (it is
// NOT serialized as identity — the serializer writes `material` and
// `sourceMaterialKey`).
//
// Editor mutation paths (SceneManager::SetMaterial /
// SetMaterialProperties) create or update this component on any entity that
// has an ImportedMeshSourceComponent, so saved UI edits to imported
// materials are never discarded by re-import on reopen.
struct MaterialOverrideComponent
{
    // The durable authored material value. When `authored` is true, the
    // resolver appends this material to doc.ecs.materials and points the
    // entity's MeshRef at it, overriding the source-imported material.
    SceneMaterial material;

    // True if this override was author-edited and must be preserved verbatim
    // (not regenerated from the source asset). When false, the resolver is
    // free to reuse a source-generated material slot and ignore `material`.
    bool authored = false;

    // The durable source material identity this override applies to, so the
    // resolver can match it against a rebuilt source material. Minted by
    // RecordMaterialOverride (SceneManager.cpp) from the material's own
    // loader-surfaced identity (SceneMaterial::sourceKey): "gltf:material:
    // name=<n>" / "gltf:material:index=<i>" / "obj:material:name=<n>" /
    // "obj:material:index=<i>". Empty for author-created materials (the
    // resolver falls back to slot position with no diagnostic). Legacy
    // "<meshSourceKey>:material" values from before Phase 8 pre-work 2 are
    // rewritten to the new form at resolve time (D4). The resolver matches
    // this key against the rebuilt staged materials by key and only falls
    // back to the resolved slot when the key does not match, raising a Stale
    // diagnostic in that case (D3).
    std::string sourceMaterialKey;

    // Transient: index into doc.ecs.materials of the override slot the
    // resolver installed. Filled by ResolveAll; not persisted as identity.
    int materialIndex = -1;
};

// ============================================================================
// Phase 6 — Script component
//
// ScriptComponent is the persisted, authored data that binds a Lua script to
// an entity. It is pure data: which script asset this entity runs, plus the
// user-authored field values. The live sol2 environment/table is NOT stored
// here — it lives in ScriptSystem, rebuilt on Play and torn down on Stop.
// This mirrors the MaterialOverrideComponent::materialIndex precedent
// (transient post-resolution state is off-document) and keeps SceneDocument
// free of Lua state, exactly as SceneDocument.h documents ("script VM state"
// is explicitly NOT part of the document).
//
// The fieldValues map is the Phase 6B seam: the script's `rt2.fields` DSL
// declares types/defaults, W2 reconciles authored values against those
// declarations, and the runtime clone injects the typed values into the
// per-entity sol2 environment as `self`. W3 adds the on-disk v3 form and W5
// adds inspector editing; no Lua objects ever enter the document.
// ============================================================================

struct ScriptComponent
{
    // Durable reference to the .lua script asset. path is a portable,
    // scene-relative UTF-8 path (forward slashes, normalized). sourceKey is
    // "lua:asset=<path>" (a stable subresource identity, ready for the
    // future asset-UUID form). kind must be AssetKind::Script.
    AssetReference asset;

    // User-authored public field values, keyed by the field name declared in
    // the script's rt2.fields block. The explicit tag preserves distinctions
    // such as vec3 versus color even though they share a variant payload arm.
    rt2::core::ScriptFieldMap fieldValues;
};

// ============================================================================
// Phase 8 — prefab instance components
//
// D1 (Phase 8 spec): an instance maps each of its entities back to the
// template entity it came from, or overrides cannot reattach. Identity lives
// in the ECS, serializes through the per-component machinery, and needs no
// side table that can desync.
//
// PrefabInstanceComponent sits on the instance ROOT only: the durable
// AssetReference to the .rt2prefab asset plus the per-instance instanceId
// (fresh per instantiate, never shared between instances).
//
// PrefabMemberComponent sits on EVERY entity of the instance: instanceId
// (grouped with the root's) plus templateId — the entity's identity inside
// the prefab asset. templateId is minted once when the prefab asset is
// created and frozen in the file; it is never regenerated and never derived
// from a scene UUID at instantiate time (amendment A1).
//
// W1 scope note: an instance is a faithful copy plus a link. Nothing yet
// distinguishes an overridden component from an inherited one (W3).
// ============================================================================

struct PrefabInstanceComponent
{
    // Durable reference to the .rt2prefab asset. kind must be
    // AssetKind::Prefab. assetId is the sidecar identity (ResolveOrAssign).
    AssetReference prefab;

    // Per-instance identity, fresh per instantiate.
    rt2::core::UUID instanceId;
};

struct PrefabMemberComponent
{
    // Groups this entity with the instance root's PrefabInstanceComponent.
    rt2::core::UUID instanceId;

    // The entity's identity inside the prefab asset — matches the
    // PrefabEntityRecord::templateId frozen in the .rt2prefab file.
    rt2::core::UUID templateId;

    // Sorted, unique set of the components whose values this member diverges
    // from the template (W3-D2). Empty means fully inherited. Every entry is
    // a PrefabComponentKey resolved through the frozen table — the keys live
    // in static constexpr storage and are safe to hold indefinitely. Never
    // construct a key from a transient buffer (e.g. a parsed JSON string): a
    // key built from a short-lived string_view dangles once the buffer dies,
    // and would very likely pass every test before crashing somewhere
    // unrelated. The scene codec builds these only from FindComponentByWire,
    // which resolves through kPrefabTable.
    std::vector<PrefabComponentKey> overrides;
};

#include "PrefabComponentKey.h"

#endif // ECS_COMPONENTS_H
