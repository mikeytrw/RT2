#pragma once

#ifndef RT2_PREFAB_PROPAGATION_CONTRACTS_H
#define RT2_PREFAB_PROPAGATION_CONTRACTS_H

#include "AssetResolver.h"
#include "ECSComponents.h"
#include "MeshRegistry.h"
#include "PrefabComponentKey.h"
#include "PrefabComponentValueEquality.h"
#include "SceneTypes.h"
#include "SceneSyncImpact.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>

// CPU-only vocabulary shared by future W4 discovery/planning and command
// layers. No scene host, renderer, watcher, or filesystem mutation API enters
// this boundary; all resource data is immutable once placed in a plan.
namespace rt2::core {

struct PrefabSourceFingerprint
{
    std::filesystem::path normalizedPath;
    UUID assetId;
    std::string contentDigest;

    bool IsValid() const noexcept
    { return !normalizedPath.empty() && !assetId.IsNull() && !contentDigest.empty(); }

    friend bool operator==(const PrefabSourceFingerprint& a,
                           const PrefabSourceFingerprint& b) noexcept
    { return std::tie(a.normalizedPath, a.assetId, a.contentDigest) ==
             std::tie(b.normalizedPath, b.assetId, b.contentDigest); }
    friend bool operator!=(const PrefabSourceFingerprint& a,
                           const PrefabSourceFingerprint& b) noexcept
    { return !(a == b); }
};

enum class PrefabPropagationInstanceDisposition : std::uint8_t
{
    Propagate,
    NoOp,
    Quarantined,
};

using PrefabPropagationComponentValue = std::variant<
    NameComponent,
    Transform,
    VisibleComponent,
    PrimitiveComponent,
    ImportedMeshSourceComponent,
    MaterialOverrideComponent,
    LightComponent,
    CameraComponent,
    MotionComponent,
    ScriptComponent>;

inline bool PrefabPropagationValueEqual(const PrefabPropagationComponentValue& a,
                                        const PrefabPropagationComponentValue& b) noexcept
{
    return std::visit([](const auto& x, const auto& y) -> bool {
        using X = std::decay_t<decltype(x)>;
        using Y = std::decay_t<decltype(y)>;
        if constexpr (!std::is_same_v<X, Y>) return false;
        else return PrefabCanonicalComponentEqual(x, y);
    }, a, b);
}

inline bool PrefabPropagationKeyAllowsValue(const PrefabComponentKey& key,
                                             const PrefabPropagationComponentValue& value) noexcept
{
    const auto canonical = FindComponentByWire(key.wire());
    if (!canonical || *canonical != key) return false;
    // importedSource is source-authoritative rather than user-overridable,
    // but it is a legal propagation operation. Links and derived MeshRef are
    // absent from the payload variant and therefore cannot enter this path.
    const bool sourceAuthoritative = key.wire() == PrefabWireKeys::kImportedSource;
    if (!key.overridable() && !sourceAuthoritative) return false;
    return std::visit([&](const auto& payload) -> bool {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, NameComponent>)
            return key.wire() == PrefabWireKeys::kName;
        else if constexpr (std::is_same_v<T, Transform>)
            return key.wire() == PrefabWireKeys::kTransform;
        else if constexpr (std::is_same_v<T, VisibleComponent>)
            return key.wire() == PrefabWireKeys::kVisible;
        else if constexpr (std::is_same_v<T, PrimitiveComponent>)
            return key.wire() == PrefabWireKeys::kPrimitive;
        else if constexpr (std::is_same_v<T, ImportedMeshSourceComponent>)
            return key.wire() == PrefabWireKeys::kImportedSource;
        else if constexpr (std::is_same_v<T, MaterialOverrideComponent>)
            return key.wire() == PrefabWireKeys::kMaterialOverride;
        else if constexpr (std::is_same_v<T, LightComponent>)
            return key.wire() == PrefabWireKeys::kLight;
        else if constexpr (std::is_same_v<T, CameraComponent>)
            return key.wire() == PrefabWireKeys::kCamera;
        else if constexpr (std::is_same_v<T, MotionComponent>)
            return key.wire() == PrefabWireKeys::kMotion;
        else
            return key.wire() == PrefabWireKeys::kScript;
    }, value);
}

struct PrefabPropagationComponentOperation
{
    UUID entityUuid;
    UUID templateId;
    PrefabComponentKey key;
    std::optional<PrefabPropagationComponentValue> before;
    std::optional<PrefabPropagationComponentValue> after;

    bool IsValid() const noexcept
    {
        if (entityUuid.IsNull() || templateId.IsNull() || !key.valid()) return false;
        if (!before && !after) return false;
        if (before && !PrefabPropagationKeyAllowsValue(key, *before)) return false;
        if (after && !PrefabPropagationKeyAllowsValue(key, *after)) return false;
        return true;
    }

    friend bool operator==(const PrefabPropagationComponentOperation& a,
                           const PrefabPropagationComponentOperation& b) noexcept
    {
        return a.entityUuid == b.entityUuid && a.templateId == b.templateId &&
               a.key == b.key &&
               (!a.before && !b.before || a.before && b.before &&
                   PrefabPropagationValueEqual(*a.before, *b.before)) &&
               (!a.after && !b.after || a.after && b.after &&
                   PrefabPropagationValueEqual(*a.after, *b.after));
    }
};

enum class PrefabPropagationResourceKind : std::uint8_t
{
    Mesh,
    Material,
    Texture,
};

struct PrefabPropagationSourceSlot
{
    std::uint32_t value = 0;
    friend bool operator==(const PrefabPropagationSourceSlot& a,
                           const PrefabPropagationSourceSlot& b) noexcept
    { return a.value == b.value; }
};

struct PrefabPropagationSceneSlot
{
    std::uint32_t value = 0;
    friend bool operator==(const PrefabPropagationSceneSlot& a,
                           const PrefabPropagationSceneSlot& b) noexcept
    { return a.value == b.value; }
};

using PrefabPropagationResourceValue = std::variant<
    MeshData,
    SceneMaterial,
    SceneTexture>;

inline bool PrefabPropagationFloatVectorEqual(const std::vector<float>& a,
                                              const std::vector<float>& b) noexcept
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (!PrefabCanonicalFloatEqual(a[i], b[i])) return false;
    return true;
}

inline bool PrefabPropagationMeshEqual(const MeshData& a,
                                       const MeshData& b) noexcept
{
    return PrefabPropagationFloatVectorEqual(a.vertices, b.vertices) &&
           a.indices == b.indices &&
           PrefabPropagationFloatVectorEqual(a.normals, b.normals) &&
           PrefabPropagationFloatVectorEqual(a.uvs, b.uvs) &&
           PrefabPropagationFloatVectorEqual(a.tangents, b.tangents) &&
           a.materialIndices == b.materialIndices && a.name == b.name &&
           PrefabCanonicalVec3Equal(a.boundsMin, b.boundsMin) &&
           PrefabCanonicalVec3Equal(a.boundsMax, b.boundsMax) &&
           a.boundsValid == b.boundsValid;
}

inline bool PrefabPropagationTextureEqual(const SceneTexture& a,
                                          const SceneTexture& b) noexcept
{
    if (!PrefabCanonicalAssetReferenceEqual(a.ref, b.ref) ||
        a.width != b.width || a.height != b.height || a.channels != b.channels ||
        a.isHDR != b.isHDR || a.isSRGB != b.isSRGB || a.pixels != b.pixels ||
        a.floatPixels.size() != b.floatPixels.size()) return false;
    for (std::size_t i = 0; i < a.floatPixels.size(); ++i)
        if (!PrefabCanonicalFloatEqual(a.floatPixels[i], b.floatPixels[i]))
            return false;
    return true;
}

inline bool PrefabPropagationResourceValueEqual(
    const PrefabPropagationResourceValue& a,
    const PrefabPropagationResourceValue& b) noexcept
{
    return std::visit([](const auto& x, const auto& y) -> bool {
        using X = std::decay_t<decltype(x)>;
        using Y = std::decay_t<decltype(y)>;
        if constexpr (!std::is_same_v<X, Y>) return false;
        else if constexpr (std::is_same_v<X, MeshData>)
            return PrefabPropagationMeshEqual(x, y);
        else if constexpr (std::is_same_v<X, SceneMaterial>)
            return PrefabCanonicalMaterialEqual(x, y);
        else return PrefabPropagationTextureEqual(x, y);
    }, a, b);
}

inline bool PrefabPropagationResourceKindMatches(
    PrefabPropagationResourceKind kind,
    const PrefabPropagationResourceValue& value) noexcept
{
    return std::visit([&](const auto& payload) -> bool {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, MeshData>)
            return kind == PrefabPropagationResourceKind::Mesh;
        else if constexpr (std::is_same_v<T, SceneMaterial>)
            return kind == PrefabPropagationResourceKind::Material;
        else
            return kind == PrefabPropagationResourceKind::Texture;
    }, value);
}

struct PrefabPropagationResourcePayload
{
    std::string sourceIdentity;
    std::string contentDigest;
    PrefabPropagationResourceValue decoded;

    bool IsValid() const noexcept
    {
        if (sourceIdentity.empty() || contentDigest.empty()) return false;
        return std::visit([](const auto& payload) -> bool {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, MeshData>)
                return !payload.vertices.empty() && !payload.indices.empty();
            else if constexpr (std::is_same_v<T, SceneMaterial>)
                return true;
            else
                return payload.width > 0 && payload.height > 0 &&
                    (payload.isHDR ? !payload.floatPixels.empty()
                                   : !payload.pixels.empty());
        }, decoded);
    }

    friend bool operator==(const PrefabPropagationResourcePayload& a,
                           const PrefabPropagationResourcePayload& b) noexcept
    { return a.sourceIdentity == b.sourceIdentity &&
             a.contentDigest == b.contentDigest &&
             PrefabPropagationResourceValueEqual(a.decoded, b.decoded); }
};

// The shared vector is const by construction: Undo/Redo can only rebind the
// recorded slots and cannot mutate the owned payload block underneath a plan.
struct PrefabPropagationResourceBlock
{
    PrefabPropagationResourceKind kind = PrefabPropagationResourceKind::Mesh;
    std::shared_ptr<const std::vector<PrefabPropagationResourcePayload>> entries;

    bool IsValid() const noexcept
    {
        if (!entries || entries->empty()) return false;
        return std::all_of(entries->begin(), entries->end(),
                           [&](const auto& value) {
                               return value.IsValid() &&
                                   PrefabPropagationResourceKindMatches(
                                       kind, value.decoded);
                           });
    }

    friend bool operator==(const PrefabPropagationResourceBlock& a,
                           const PrefabPropagationResourceBlock& b) noexcept
    { return a.kind == b.kind &&
             (a.entries == b.entries ||
              (a.entries && b.entries && *a.entries == *b.entries)); }
};

struct PrefabPropagationResourceRebase
{
    PrefabPropagationResourceKind kind = PrefabPropagationResourceKind::Mesh;
    std::uint32_t sourceBeforeExtent = 0;
    std::uint32_t sceneBeforeExtent = 0;
    std::uint32_t sceneAppendBase = 0;
    std::uint32_t sceneAfterExtent = 0;
    std::vector<PrefabPropagationSourceSlot> sourceSlots;
    std::vector<PrefabPropagationSceneSlot> sceneSlots;
    PrefabPropagationResourceBlock owned;

    bool IsValid() const noexcept
    {
        if (owned.kind != kind || !owned.IsValid() ||
            sourceSlots.size() != sceneSlots.size() ||
            sourceSlots.size() != owned.entries->size() || sourceSlots.empty())
            return false;
        if (sceneAppendBase != sceneBeforeExtent) return false;
        const auto count = sourceSlots.size();
        if (count > std::numeric_limits<std::uint32_t>::max()) return false;
        const auto count32 = static_cast<std::uint32_t>(count);
        if (sceneBeforeExtent > std::numeric_limits<std::uint32_t>::max() - count32)
            return false;
        if (sceneAfterExtent != sceneBeforeExtent + count32) return false;
        std::vector<std::uint32_t> sources;
        sources.reserve(sourceSlots.size());
        for (std::size_t i = 0; i < sourceSlots.size(); ++i)
        {
            if (sourceSlots[i].value >= sourceBeforeExtent ||
                sceneSlots[i].value != sceneAppendBase + static_cast<std::uint32_t>(i) ||
                sceneSlots[i].value < sceneBeforeExtent ||
                sceneSlots[i].value >= sceneAfterExtent)
                return false;
            sources.push_back(sourceSlots[i].value);
        }
        std::sort(sources.begin(), sources.end());
        return std::adjacent_find(sources.begin(), sources.end()) == sources.end();
    }

    friend bool operator==(const PrefabPropagationResourceRebase& a,
                           const PrefabPropagationResourceRebase& b) noexcept
    { return a.kind == b.kind && a.sourceBeforeExtent == b.sourceBeforeExtent &&
             a.sceneBeforeExtent == b.sceneBeforeExtent &&
             a.sceneAppendBase == b.sceneAppendBase &&
             a.sceneAfterExtent == b.sceneAfterExtent &&
             a.sourceSlots == b.sourceSlots && a.sceneSlots == b.sceneSlots &&
             a.owned == b.owned; }
};

struct PrefabPropagationResourceOwnership
{
    PrefabPropagationResourceRebase rebase;

    bool IsValid() const noexcept { return rebase.IsValid(); }
    friend bool operator==(const PrefabPropagationResourceOwnership& a,
                           const PrefabPropagationResourceOwnership& b) noexcept
    { return a.rebase == b.rebase; }
};

inline SyncImpact PrefabPropagationImpactForKey(const PrefabComponentKey& key) noexcept
{
    const auto wire = key.wire();
    if (wire == PrefabWireKeys::kTransform ||
        wire == PrefabWireKeys::kCamera)
        return SyncImpact::Transform;
    if (wire == PrefabWireKeys::kMaterialOverride ||
        wire == PrefabWireKeys::kLight)
        return SyncImpact::Material;
    if (wire == PrefabWireKeys::kVisible ||
        wire == PrefabWireKeys::kPrimitive ||
        wire == PrefabWireKeys::kImportedSource)
        return SyncImpact::Structural;
    return SyncImpact::None;
}

inline SyncImpact PrefabPropagationImpactForResource(
    PrefabPropagationResourceKind kind) noexcept
{
    return kind == PrefabPropagationResourceKind::Mesh
        ? SyncImpact::Structural : SyncImpact::Material;
}

struct PrefabPropagationDiagnostic
{
    AssetDiagnostic::Severity severity = AssetDiagnostic::Malformed;
    std::filesystem::path prefabPath;
    UUID prefabAssetId;
    UUID instanceId;
    UUID rootUuid;
    UUID templateId;
    std::string reason;

    friend bool operator==(const PrefabPropagationDiagnostic& a,
                           const PrefabPropagationDiagnostic& b) noexcept
    { return std::tie(a.severity, a.prefabPath, a.prefabAssetId, a.instanceId,
                      a.rootUuid, a.templateId, a.reason) ==
             std::tie(b.severity, b.prefabPath, b.prefabAssetId, b.instanceId,
                      b.rootUuid, b.templateId, b.reason); }

    friend bool operator<(const PrefabPropagationDiagnostic& a,
                          const PrefabPropagationDiagnostic& b) noexcept
    { return std::tie(a.severity, a.prefabPath, a.prefabAssetId, a.instanceId,
                      a.rootUuid, a.templateId, a.reason) <
             std::tie(b.severity, b.prefabPath, b.prefabAssetId, b.instanceId,
                      b.rootUuid, b.templateId, b.reason); }

    // Kept as a human-readable diagnostic key; structured operator< above is
    // the ordering authority and cannot collide on delimiter characters.
    std::string SortKey() const
    { return prefabPath.generic_string() + "|" + prefabAssetId.ToString() +
             "|" + instanceId.ToString() + "|" + rootUuid.ToString() +
             "|" + templateId.ToString() + "|" +
             std::to_string(static_cast<unsigned>(severity)) + "|" + reason; }
};

struct PrefabPropagationInstancePlan
{
    UUID instanceId;
    UUID rootUuid;
    PrefabPropagationInstanceDisposition disposition =
        PrefabPropagationInstanceDisposition::NoOp;
    std::vector<UUID> affectedEntities;
    std::vector<PrefabPropagationDiagnostic> diagnostics;

    friend bool operator==(const PrefabPropagationInstancePlan& a,
                           const PrefabPropagationInstancePlan& b) noexcept
    { return a.instanceId == b.instanceId && a.rootUuid == b.rootUuid &&
             a.disposition == b.disposition &&
             a.affectedEntities == b.affectedEntities &&
             a.diagnostics == b.diagnostics; }
};

struct PrefabPropagationPlan
{
    PrefabSourceFingerprint source;
    std::uint64_t documentGeneration = 0;
    std::uint64_t resourceGeneration = 0;
    std::uint32_t sourceSchemaVersion = 0;
    std::vector<PrefabPropagationInstancePlan> instances;
    std::vector<PrefabPropagationComponentOperation> componentOperations;
    std::vector<PrefabPropagationResourceOwnership> resourceOwnership;
    std::vector<PrefabPropagationDiagnostic> diagnostics;
    std::vector<UUID> affectedEntities;
    SyncImpact syncImpact = SyncImpact::None;

    std::vector<UUID> DerivedAffectedEntities() const
    {
        std::vector<UUID> derived;
        derived.reserve(componentOperations.size());
        for (const auto& operation : componentOperations)
            if (std::find(derived.begin(), derived.end(), operation.entityUuid) ==
                derived.end())
                derived.push_back(operation.entityUuid);
        std::sort(derived.begin(), derived.end(),
                  [](const UUID& a, const UUID& b) {
                      return a.ToString() < b.ToString();
                  });
        return derived;
    }

    SyncImpact DerivedSyncImpact() const noexcept
    {
        SyncImpact derived = SyncImpact::None;
        for (const auto& operation : componentOperations)
            if (static_cast<int>(PrefabPropagationImpactForKey(operation.key)) >
                static_cast<int>(derived))
                derived = PrefabPropagationImpactForKey(operation.key);
        for (const auto& ownership : resourceOwnership)
            if (static_cast<int>(PrefabPropagationImpactForResource(
                    ownership.rebase.kind)) > static_cast<int>(derived))
                derived = PrefabPropagationImpactForResource(ownership.rebase.kind);
        return derived;
    }

    bool IsValid() const noexcept
    {
        for (const auto& operation : componentOperations)
            if (!operation.IsValid()) return false;
        for (const auto& ownership : resourceOwnership)
            if (!ownership.IsValid()) return false;
        if (affectedEntities != DerivedAffectedEntities()) return false;
        if (syncImpact != DerivedSyncImpact()) return false;
        return true;
    }

    bool IsNoOp() const noexcept
    {
        if (!IsValid()) return false;
        for (const auto& op : componentOperations)
        {
            if (op.before.has_value() != op.after.has_value())
                return false;
            if (op.before && op.after &&
                !PrefabPropagationValueEqual(*op.before, *op.after))
                return false;
        }
        if (!resourceOwnership.empty()) return false;
        return true;
    }
    bool IsEffective() const noexcept { return IsValid() && !IsNoOp(); }

    friend bool operator==(const PrefabPropagationPlan& a,
                           const PrefabPropagationPlan& b) noexcept
    { return a.source == b.source && a.documentGeneration == b.documentGeneration &&
             a.resourceGeneration == b.resourceGeneration &&
             a.sourceSchemaVersion == b.sourceSchemaVersion &&
             a.instances == b.instances && a.componentOperations == b.componentOperations &&
             a.resourceOwnership == b.resourceOwnership && a.diagnostics == b.diagnostics &&
             a.affectedEntities == b.affectedEntities && a.syncImpact == b.syncImpact; }
};

struct PrefabPropagationResult
{
    bool success = false;
    bool effective = false;
    PrefabPropagationInstanceDisposition disposition =
        PrefabPropagationInstanceDisposition::NoOp;
    std::size_t propagatedInstances = 0;
    std::size_t noOpInstances = 0;
    std::size_t quarantinedInstances = 0;
    std::uint64_t documentGeneration = 0;
    std::uint64_t resourceGeneration = 0;
    SyncImpact syncImpact = SyncImpact::None;
    std::vector<UUID> affectedEntities;
    std::vector<PrefabPropagationDiagnostic> diagnostics;

    friend bool operator==(const PrefabPropagationResult& a,
                           const PrefabPropagationResult& b) noexcept
    { return a.success == b.success && a.effective == b.effective &&
             a.disposition == b.disposition &&
             a.propagatedInstances == b.propagatedInstances &&
             a.noOpInstances == b.noOpInstances &&
             a.quarantinedInstances == b.quarantinedInstances &&
             a.documentGeneration == b.documentGeneration &&
             a.resourceGeneration == b.resourceGeneration &&
             a.syncImpact == b.syncImpact && a.affectedEntities == b.affectedEntities &&
             a.diagnostics == b.diagnostics; }
};

} // namespace rt2::core

#endif // RT2_PREFAB_PROPAGATION_CONTRACTS_H
