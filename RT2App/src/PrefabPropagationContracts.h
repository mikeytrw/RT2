#pragma once

#ifndef RT2_PREFAB_PROPAGATION_CONTRACTS_H
#define RT2_PREFAB_PROPAGATION_CONTRACTS_H

#include "AssetResolver.h"
#include "ECSComponents.h"
#include "ISceneRenderBridge.h"
#include "PrefabComponentKey.h"
#include "PrefabComponentValueEquality.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// CPU-only vocabulary shared by the future W4 discovery/planning and command
// layers.  This header intentionally contains no scene host, renderer,
// watcher, or filesystem mutation API; it is a durable plan/apply boundary.
namespace rt2::core {

struct PrefabSourceFingerprint
{
    std::filesystem::path normalizedPath;
    UUID assetId;
    // Canonical content digest supplied by the asset loader.  A timestamp is
    // deliberately not part of identity: unchanged bytes must coalesce.
    std::string contentDigest;

    bool IsValid() const noexcept
    {
        return !normalizedPath.empty() && !assetId.IsNull() &&
               !contentDigest.empty();
    }

    friend bool operator==(const PrefabSourceFingerprint& a,
                           const PrefabSourceFingerprint& b) noexcept
    {
        return a.normalizedPath == b.normalizedPath &&
               a.assetId == b.assetId && a.contentDigest == b.contentDigest;
    }
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

// A complete durable component payload.  std::nullopt means explicit
// component absence; link metadata and derived MeshRef are intentionally not
// legal values in this operation variant.
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

struct PrefabPropagationComponentOperation
{
    UUID entityUuid;
    UUID templateId;
    PrefabComponentKey key;
    std::optional<PrefabPropagationComponentValue> before;
    std::optional<PrefabPropagationComponentValue> after;

    bool IsValid() const noexcept
    {
        return !entityUuid.IsNull() && !templateId.IsNull() && key.valid() &&
               key.overridable();
    }
};

enum class PrefabPropagationResourceKind : std::uint8_t
{
    Mesh,
    Material,
    Texture,
};

struct PrefabPropagationResourceOwnership
{
    PrefabPropagationResourceKind kind = PrefabPropagationResourceKind::Mesh;
    // Append-only slots owned by this plan.  Redo reuses these exact slots;
    // undo only restores references and never compacts the tables.
    std::vector<std::uint32_t> ownedSlots;
    std::vector<std::uint32_t> sourceSlots;

    bool IsValid() const noexcept
    {
        return ownedSlots.size() == sourceSlots.size();
    }
};

struct PrefabPropagationDiagnostic
{
    AssetDiagnostic::Severity severity = AssetDiagnostic::Malformed;
    std::filesystem::path prefabPath;
    UUID prefabAssetId;
    UUID instanceId;
    UUID rootUuid;
    UUID templateId;
    std::string reason;

    // Stable ordering independent of EnTT traversal or unordered containers.
    std::string SortKey() const
    {
        return prefabPath.generic_string() + "|" + prefabAssetId.ToString() +
               "|" + instanceId.ToString() + "|" + rootUuid.ToString() +
               "|" + templateId.ToString() + "|" +
               std::to_string(static_cast<unsigned>(severity)) + "|" + reason;
    }
};

struct PrefabPropagationInstancePlan
{
    UUID instanceId;
    UUID rootUuid;
    PrefabPropagationInstanceDisposition disposition =
        PrefabPropagationInstanceDisposition::NoOp;
    std::vector<UUID> affectedEntities;
    std::vector<PrefabPropagationDiagnostic> diagnostics;
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
    bool anyStateChange = false;

    bool IsNoOp() const noexcept
    { return !anyStateChange && componentOperations.empty(); }
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
};

} // namespace rt2::core

#endif // RT2_PREFAB_PROPAGATION_CONTRACTS_H
