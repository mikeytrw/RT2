#pragma once

#ifndef RT2_PREFAB_PROPAGATION_CONTRACTS_H
#define RT2_PREFAB_PROPAGATION_CONTRACTS_H

#include "AssetResolver.h"
#include "ECSComponents.h"
#include "MeshRegistry.h"
#include "PrefabComponentKey.h"
#include "PrefabComponentValueEquality.h"
#include "PrefabPropagationComponentAdapter.h"
#include "SceneTypes.h"
#include "SceneSyncImpact.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>

// CPU-only vocabulary shared by W4 discovery/planning and command
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

// The decoded values are copied into a private const allocation. Callers can
// inspect the prepared block but cannot retain a mutable alias to its storage.
struct PrefabPropagationResourceBlock
{
    PrefabPropagationResourceKind kind = PrefabPropagationResourceKind::Mesh;

    static PrefabPropagationResourceBlock FromDecoded(
        PrefabPropagationResourceKind kind,
        const std::vector<PrefabPropagationResourcePayload>& values)
    {
        PrefabPropagationResourceBlock result;
        result.kind = kind;
        result.entries_ = std::make_shared<const std::vector<PrefabPropagationResourcePayload>>(
            values);
        return result;
    }

    const std::vector<PrefabPropagationResourcePayload>& Entries() const noexcept
    {
        static const std::vector<PrefabPropagationResourcePayload> empty;
        return entries_ ? *entries_ : empty;
    }

    bool IsValid() const noexcept
    {
        const auto& entries = Entries();
        if (entries.empty()) return false;
        return std::all_of(entries.begin(), entries.end(),
                           [&](const auto& value) {
                               return value.IsValid() &&
                                   PrefabPropagationResourceKindMatches(
                                       kind, value.decoded);
                           });
    }

    friend bool operator==(const PrefabPropagationResourceBlock& a,
                           const PrefabPropagationResourceBlock& b) noexcept
    { return a.kind == b.kind &&
             a.Entries() == b.Entries(); }

private:
    std::shared_ptr<const std::vector<PrefabPropagationResourcePayload>> entries_;
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
            sourceSlots.size() != owned.Entries().size() || sourceSlots.empty())
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

// Derived geometry is deliberately separate from the durable component
// operation.  MeshRef is a scene-global index and may never be copied from a
// prefab source record; the command installs the rebased value only after the
// owned mesh block has been appended.
struct PrefabPropagationMeshRefOperation
{
    UUID entityUuid;
    UUID templateId;
    std::optional<MeshRef> before;
    std::optional<MeshRef> after;

    bool IsValid() const noexcept
    { return !entityUuid.IsNull() && !templateId.IsNull() &&
             (before.has_value() || after.has_value()); }

    friend bool operator==(const PrefabPropagationMeshRefOperation& a,
                           const PrefabPropagationMeshRefOperation& b) noexcept
    { return a.entityUuid == b.entityUuid && a.templateId == b.templateId &&
             a.before.has_value() == b.before.has_value() &&
             a.after.has_value() == b.after.has_value() &&
             (!a.before || a.before->meshIndex == b.before->meshIndex &&
                a.before->materialIndex == b.before->materialIndex) &&
             (!a.after || a.after->meshIndex == b.after->meshIndex &&
                a.after->materialIndex == b.after->materialIndex); }
};

// The links and marker vector are part of the commit precondition.  Keeping
// them in the prepared plan prevents an entity that was re-linked between
// prepare and commit from receiving a valid-looking value delta.
struct PrefabPropagationMemberSnapshot
{
    UUID entityUuid;
    UUID instanceId;
    UUID templateId;
    std::vector<PrefabComponentKey> overrides;

    friend bool operator==(const PrefabPropagationMemberSnapshot& a,
                           const PrefabPropagationMemberSnapshot& b) noexcept
    { return a.entityUuid == b.entityUuid && a.instanceId == b.instanceId &&
             a.templateId == b.templateId && a.overrides == b.overrides; }
};

inline bool PrefabPropagationAssetReferenceEqual(const AssetReference& a,
                                                 const AssetReference& b) noexcept
{
    return a.kind == b.kind && a.path == b.path &&
           a.importSettings == b.importSettings && a.sourceKey == b.sourceKey &&
           a.assetId == b.assetId;
}

// The root link is a separate stale precondition from member links.  A root
// can be relinked while all member markers and values remain byte-identical,
// so the command must carry the exact durable prefab reference and instance id
// observed during discovery.
struct PrefabPropagationRootSnapshot
{
    UUID rootUuid;
    UUID instanceId;
    AssetReference prefab;

    bool IsValid() const noexcept
    { return !rootUuid.IsNull() && !instanceId.IsNull() && prefab.IsValid(); }

    friend bool operator==(const PrefabPropagationRootSnapshot& a,
                           const PrefabPropagationRootSnapshot& b) noexcept
    {
        // Plan equality is used for deterministic discovery comparison. Path
        // spelling is normalized for that comparison, while the command's
        // stale check above deliberately uses exact durable bytes.
        auto normalize = [](const std::filesystem::path& path) {
            return path.lexically_normal().generic_string();
        };
        return a.rootUuid == b.rootUuid && a.instanceId == b.instanceId &&
               a.prefab.kind == b.prefab.kind &&
               normalize(a.prefab.path) == normalize(b.prefab.path) &&
               a.prefab.importSettings == b.prefab.importSettings &&
               a.prefab.sourceKey == b.prefab.sourceKey &&
               a.prefab.assetId == b.prefab.assetId;
    }
};

inline SyncImpact PrefabPropagationImpactForResource(
    PrefabPropagationResourceKind kind) noexcept
{
    if (kind == PrefabPropagationResourceKind::Mesh ||
        kind == PrefabPropagationResourceKind::Texture)
        return SyncImpact::Structural;
    return SyncImpact::Material;
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

// The source snapshot is captured once by the host/load boundary and then
// borrowed by discovery.  Stage/history never reopens these bytes.
struct CapturedPrefabSource
{
    PrefabSourceFingerprint fingerprint;
    std::string prefabBytes;
    std::string sidecarBytes;

    bool IsValid() const noexcept
    { return fingerprint.IsValid() && !prefabBytes.empty() && !sidecarBytes.empty(); }
};

struct DiscoveredPropagationPlan
{
    PrefabSourceFingerprint source;
    CapturedPrefabSource capturedSource;
    std::uint64_t documentGeneration = 0;
    std::uint64_t resourceGeneration = 0;
    std::uint64_t authoringRevision = 0;
    // Presence is separate from the value: revision zero is valid on the
    // first clean loaded document.
    bool authoringRevisionCaptured = false;
    std::uint32_t sourceSchemaVersion = 0;
    std::uint32_t meshTableExtent = 0;
    std::uint32_t materialTableExtent = 0;
    std::uint32_t textureTableExtent = 0;
    std::vector<PrefabPropagationInstancePlan> instances;
    std::vector<PrefabPropagationComponentDelta> componentOperations;
    std::vector<PrefabPropagationMeshRefOperation> meshRefOperations;
    std::vector<PrefabPropagationMemberSnapshot> memberSnapshots;
    std::vector<PrefabPropagationRootSnapshot> rootSnapshots;
    std::vector<PrefabPropagationResourceOwnership> resourceOwnership;
    std::vector<PrefabPropagationDiagnostic> diagnostics;
    std::vector<UUID> affectedEntities;
    SyncImpact syncImpact = SyncImpact::None;

    std::vector<UUID> DerivedAffectedEntities() const
    {
        std::vector<UUID> derived;
        derived.reserve(componentOperations.size());
        for (const auto& operation : componentOperations)
        {
            if (operation.IsNoOp()) continue;
            if (std::find(derived.begin(), derived.end(), operation.EntityUuid()) ==
                derived.end())
                derived.push_back(operation.EntityUuid());
        }
        for (const auto& operation : meshRefOperations)
        {
            if (operation.before.has_value() == operation.after.has_value() &&
                operation.before && operation.before->meshIndex == operation.after->meshIndex &&
                operation.before->materialIndex == operation.after->materialIndex)
                continue;
            if (std::find(derived.begin(), derived.end(), operation.entityUuid) ==
                derived.end())
                derived.push_back(operation.entityUuid);
        }
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
        {
            if (operation.IsNoOp()) continue;
            if (static_cast<int>(operation.Impact()) >
                static_cast<int>(derived))
                derived = operation.Impact();
        }
        for (const auto& operation : meshRefOperations)
        {
            const bool changed = operation.before.has_value() != operation.after.has_value() ||
                (operation.before && operation.after &&
                 (operation.before->meshIndex != operation.after->meshIndex ||
                  operation.before->materialIndex != operation.after->materialIndex));
            if (changed && static_cast<int>(SyncImpact::Structural) > static_cast<int>(derived))
                derived = SyncImpact::Structural;
        }
        for (const auto& ownership : resourceOwnership)
            if (static_cast<int>(PrefabPropagationImpactForResource(
                    ownership.rebase.kind)) > static_cast<int>(derived))
                derived = PrefabPropagationImpactForResource(ownership.rebase.kind);
        return derived;
    }

    bool ResourceOwnershipConsumed() const noexcept
    {
        auto owns = [&](PrefabPropagationResourceKind kind, std::uint32_t slot) {
            for (const auto& ownership : resourceOwnership)
                if (ownership.rebase.kind == kind &&
                    slot >= ownership.rebase.sceneBeforeExtent &&
                    slot < ownership.rebase.sceneAfterExtent)
                    return true;
            return false;
        };
        for (const auto& ownership : resourceOwnership)
        {
            bool consumed = false;
            const auto kind = ownership.rebase.kind;
            for (const auto& operation : meshRefOperations)
            {
                if (operation.after)
                {
                    if (kind == PrefabPropagationResourceKind::Mesh &&
                        owns(kind, operation.after->meshIndex)) consumed = true;
                    if (kind == PrefabPropagationResourceKind::Material &&
                        operation.after->materialIndex >= 0 &&
                        owns(kind, static_cast<std::uint32_t>(operation.after->materialIndex)))
                        consumed = true;
                }
            }
            for (const auto& operation : componentOperations)
            {
                const auto after = operation.AfterValue();
                if (!after) continue;
                const auto* material = std::get_if<MaterialOverrideComponent>(after.operator->());
                if (!material) continue;
                if (kind == PrefabPropagationResourceKind::Material &&
                    material->materialIndex >= 0 &&
                    owns(kind, static_cast<std::uint32_t>(material->materialIndex)))
                    consumed = true;
                if (kind == PrefabPropagationResourceKind::Texture)
                {
                    const int indices[] = {material->material.baseColorTextureIndex,
                        material->material.normalTextureIndex,
                        material->material.emissiveTextureIndex,
                        material->material.metallicRoughnessTextureIndex};
                    for (const int index : indices)
                        if (index >= 0 && owns(kind, static_cast<std::uint32_t>(index)))
                            consumed = true;
                }
            }
            if (!consumed && kind == PrefabPropagationResourceKind::Material)
                for (const auto& meshOwnership : resourceOwnership)
                    if (meshOwnership.rebase.kind == PrefabPropagationResourceKind::Mesh)
                        for (const auto& payload : meshOwnership.rebase.owned.Entries())
                            for (const auto index : std::get<MeshData>(payload.decoded).materialIndices)
                                if (owns(kind, index)) consumed = true;
            if (!consumed && kind == PrefabPropagationResourceKind::Texture)
                for (const auto& materialOwnership : resourceOwnership)
                    if (materialOwnership.rebase.kind == PrefabPropagationResourceKind::Material)
                        for (const auto& payload : materialOwnership.rebase.owned.Entries())
                        {
                            const auto& material = std::get<SceneMaterial>(payload.decoded);
                            const int indices[] = {material.baseColorTextureIndex,
                                material.normalTextureIndex, material.emissiveTextureIndex,
                                material.metallicRoughnessTextureIndex};
                            for (const int index : indices)
                                if (index >= 0 && owns(kind, static_cast<std::uint32_t>(index)))
                                    consumed = true;
                        }
            if (!consumed) return false;
        }
        return true;
    }

    bool IsValid() const noexcept
    {
        std::set<std::pair<UUID, std::string>> componentKeys;
        std::set<UUID> meshEntities;
        std::set<UUID> snapshotEntities;
        std::set<UUID> roots;
        for (const auto& operation : componentOperations)
            if (!operation.IsValid() ||
                !componentKeys.emplace(operation.EntityUuid(), operation.Key().wire()).second)
                return false;
        for (const auto& operation : meshRefOperations)
            if (!operation.IsValid() || !meshEntities.emplace(operation.entityUuid).second)
                return false;
        for (const auto& snapshot : memberSnapshots)
            if (snapshot.entityUuid.IsNull() || snapshot.instanceId.IsNull() ||
                snapshot.templateId.IsNull() ||
                !snapshotEntities.emplace(snapshot.entityUuid).second)
                return false;
        for (const auto& snapshot : rootSnapshots)
            if (!snapshot.IsValid() || !roots.emplace(snapshot.rootUuid).second)
                return false;
        for (const auto& instance : instances)
        {
            const auto root = std::find_if(rootSnapshots.begin(), rootSnapshots.end(),
                [&](const auto& snapshot) { return snapshot.rootUuid == instance.rootUuid; });
            if (instance.disposition != PrefabPropagationInstanceDisposition::Quarantined &&
                rootSnapshots.size() != 0 &&
                (root == rootSnapshots.end() || root->instanceId != instance.instanceId))
                return false;
        }
        std::array<bool, 3> ownershipKinds{};
        std::array<std::uint32_t, 3> ownedBefore{};
        for (const auto& ownership : resourceOwnership)
        {
            if (!ownership.IsValid()) return false;
            const auto kind = static_cast<std::size_t>(ownership.rebase.kind);
            if (kind >= ownershipKinds.size() || ownershipKinds[kind]) return false;
            ownershipKinds[kind] = true;
            ownedBefore[kind] = ownership.rebase.sceneBeforeExtent;
            const auto expectedExtent = kind == static_cast<std::size_t>(PrefabPropagationResourceKind::Mesh)
                ? meshTableExtent
                : kind == static_cast<std::size_t>(PrefabPropagationResourceKind::Material)
                    ? materialTableExtent : textureTableExtent;
            const bool extentsCaptured = meshTableExtent != 0 || materialTableExtent != 0 ||
                                         textureTableExtent != 0 || !rootSnapshots.empty();
            if (extentsCaptured && ownership.rebase.sceneBeforeExtent != expectedExtent)
                return false;
        }
        auto ownsSlot = [&](PrefabPropagationResourceKind kind, std::uint32_t slot) {
            const auto index = static_cast<std::size_t>(kind);
            for (const auto& ownership : resourceOwnership)
                if (static_cast<std::size_t>(ownership.rebase.kind) == index)
                    return slot >= ownership.rebase.sceneBeforeExtent &&
                           slot < ownership.rebase.sceneAfterExtent;
            return false;
        };
        const bool extentsCaptured = meshTableExtent != 0 || materialTableExtent != 0 ||
                                     textureTableExtent != 0 || !rootSnapshots.empty();
        auto validTableIndex = [&](PrefabPropagationResourceKind kind,
                                   std::uint32_t index) {
            if (!extentsCaptured) return true;
            const auto extent = kind == PrefabPropagationResourceKind::Mesh
                ? meshTableExtent
                : kind == PrefabPropagationResourceKind::Material
                    ? materialTableExtent : textureTableExtent;
            return index < extent || ownsSlot(kind, index);
        };
        for (const auto& ownership : resourceOwnership)
        {
            const auto kind = ownership.rebase.kind;
            for (const auto& payload : ownership.rebase.owned.Entries())
            {
                if (kind == PrefabPropagationResourceKind::Mesh)
                {
                    const auto& mesh = std::get<MeshData>(payload.decoded);
                    for (const auto index : mesh.materialIndices)
                        if (!validTableIndex(PrefabPropagationResourceKind::Material, index))
                            return false;
                }
                else if (kind == PrefabPropagationResourceKind::Material)
                {
                    const auto& material = std::get<SceneMaterial>(payload.decoded);
                    const int indices[] = { material.baseColorTextureIndex,
                        material.normalTextureIndex, material.emissiveTextureIndex,
                        material.metallicRoughnessTextureIndex };
                    for (const int index : indices)
                        if (index < -1 || (index >= 0 &&
                            !validTableIndex(PrefabPropagationResourceKind::Texture,
                                             static_cast<std::uint32_t>(index))))
                            return false;
                }
            }
        }
        auto validMeshRef = [&](const std::optional<MeshRef>& ref) {
            if (!ref) return true;
            if (extentsCaptured && !ownsSlot(PrefabPropagationResourceKind::Mesh, ref->meshIndex) &&
                ref->meshIndex >= meshTableExtent)
                return false;
            if (ref->materialIndex < -1) return false;
            if (extentsCaptured && ref->materialIndex >= 0 &&
                !ownsSlot(PrefabPropagationResourceKind::Material,
                          static_cast<std::uint32_t>(ref->materialIndex)) &&
                static_cast<std::uint32_t>(ref->materialIndex) >= materialTableExtent)
                return false;
            return true;
        };
        for (const auto& operation : meshRefOperations)
            if (!validMeshRef(operation.before) || !validMeshRef(operation.after))
                return false;
        if (!ResourceOwnershipConsumed()) return false;
        if (affectedEntities != DerivedAffectedEntities()) return false;
        if (syncImpact != DerivedSyncImpact()) return false;
        return true;
    }

    bool IsNoOp() const noexcept
    {
        if (!IsValid()) return false;
        for (const auto& op : componentOperations)
            if (!op.IsNoOp()) return false;
        if (!resourceOwnership.empty()) return false;
        for (const auto& operation : meshRefOperations)
            if (operation.before.has_value() != operation.after.has_value() ||
                operation.before && operation.after &&
                (operation.before->meshIndex != operation.after->meshIndex ||
                 operation.before->materialIndex != operation.after->materialIndex))
                return false;
        return true;
    }
    bool IsEffective() const noexcept { return IsValid() && !IsNoOp(); }

    friend bool operator==(const DiscoveredPropagationPlan& a,
                           const DiscoveredPropagationPlan& b) noexcept
    { return a.source == b.source && a.documentGeneration == b.documentGeneration &&
             a.resourceGeneration == b.resourceGeneration &&
             a.authoringRevision == b.authoringRevision &&
             a.authoringRevisionCaptured == b.authoringRevisionCaptured &&
             a.sourceSchemaVersion == b.sourceSchemaVersion &&
             a.meshTableExtent == b.meshTableExtent &&
             a.materialTableExtent == b.materialTableExtent &&
             a.textureTableExtent == b.textureTableExtent &&
             a.instances == b.instances && a.componentOperations == b.componentOperations &&
             a.meshRefOperations == b.meshRefOperations &&
             a.memberSnapshots == b.memberSnapshots &&
             a.rootSnapshots == b.rootSnapshots &&
             a.resourceOwnership == b.resourceOwnership && a.diagnostics == b.diagnostics &&
             a.affectedEntities == b.affectedEntities && a.syncImpact == b.syncImpact; }
};

enum class StageOutcomeKind : std::uint8_t
{
    NoOp,
    Executable,
};

struct StageOutcome;

// Stage is the sole production constructor.  The data is exposed read-only
// through the command's const Plan() accessor; no raw source bytes live here.
class ExecutablePropagationPlan final : public DiscoveredPropagationPlan
{
    friend struct StageOutcome;

    explicit ExecutablePropagationPlan(DiscoveredPropagationPlan&& discovered)
        : DiscoveredPropagationPlan(std::move(discovered))
    { capturedSource = {}; }

    static ExecutablePropagationPlan Make(DiscoveredPropagationPlan&& discovered)
    { return ExecutablePropagationPlan(std::move(discovered)); }

public:
    ExecutablePropagationPlan() = delete;
};

// StageOutcome intentionally retains the discovered summary as its base so
// existing discovery/load diagnostics can be inspected without manufacturing
// a command-ready plan.  Only the Executable alternative contains command
// evidence and it is built privately by Stage.
struct StageOutcome final : public DiscoveredPropagationPlan
{
    StageOutcomeKind kind = StageOutcomeKind::NoOp;
    std::optional<ExecutablePropagationPlan> executable;

    static StageOutcome NoOp(DiscoveredPropagationPlan&& discovered)
    {
        StageOutcome result;
        static_cast<DiscoveredPropagationPlan&>(result) = std::move(discovered);
        result.capturedSource = {};
        result.kind = StageOutcomeKind::NoOp;
        return result;
    }

    static StageOutcome Effective(DiscoveredPropagationPlan&& discovered)
    {
        StageOutcome result;
        static_cast<DiscoveredPropagationPlan&>(result) = discovered;
        result.kind = StageOutcomeKind::Executable;
        result.executable = ExecutablePropagationPlan::Make(std::move(discovered));
        result.capturedSource = {};
        return result;
    }

    static StageOutcome FromDiscovered(DiscoveredPropagationPlan discovered)
    {
        return discovered.IsNoOp() ? NoOp(std::move(discovered))
                                   : Effective(std::move(discovered));
    }

    bool IsNoOp() const noexcept { return kind == StageOutcomeKind::NoOp; }
    bool IsEffective() const noexcept
    { return kind == StageOutcomeKind::Executable && executable.has_value(); }
};

} // namespace rt2::core

#endif // RT2_PREFAB_PROPAGATION_CONTRACTS_H
