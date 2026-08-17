#pragma once

#ifndef RT2_PREFAB_COMPONENT_VALUE_EQUALITY_H
#define RT2_PREFAB_COMPONENT_VALUE_EQUALITY_H

#include "ECSComponents.h"

#include <cmath>
#include <optional>
#include <type_traits>
#include <utility>

// CPU-only canonical comparisons shared by prefab planning and local edit
// code.  These deliberately compare durable authored fields only: Primitive
// has no derived mesh index, while optional comparison treats absence as a
// first-class value rather than as a structural instruction.
namespace rt2::core {

// NaN is malformed authored data, so it never compares equal—even to itself.
// This explicit rule prevents a stale-value check from treating two malformed
// payloads as a safe match while preserving ordinary signed-zero equality.
inline bool PrefabCanonicalFloatEqual(float a, float b) noexcept
{ return !std::isnan(a) && !std::isnan(b) && a == b; }

inline bool PrefabCanonicalDoubleEqual(double a, double b) noexcept
{ return !std::isnan(a) && !std::isnan(b) && a == b; }

inline bool PrefabCanonicalVec3Equal(const glm::vec3& a,
                                    const glm::vec3& b) noexcept
{
    return PrefabCanonicalFloatEqual(a.x, b.x) &&
           PrefabCanonicalFloatEqual(a.y, b.y) &&
           PrefabCanonicalFloatEqual(a.z, b.z);
}

inline bool PrefabCanonicalQuatEqual(const glm::quat& a,
                                     const glm::quat& b) noexcept
{
    return PrefabCanonicalFloatEqual(a.w, b.w) &&
           PrefabCanonicalFloatEqual(a.x, b.x) &&
           PrefabCanonicalFloatEqual(a.y, b.y) &&
           PrefabCanonicalFloatEqual(a.z, b.z);
}

inline bool PrefabCanonicalAssetReferenceEqual(const AssetReference& a,
                                               const AssetReference& b) noexcept
{
    return a.kind == b.kind && a.path == b.path &&
           a.importSettings == b.importSettings && a.sourceKey == b.sourceKey &&
           a.assetId == b.assetId;
}

inline bool PrefabCanonicalMaterialEqual(const SceneMaterial& a,
                                         const SceneMaterial& b) noexcept
{
    return a.type == b.type && PrefabCanonicalVec3Equal(a.baseColor, b.baseColor) &&
           PrefabCanonicalFloatEqual(a.baseAlpha, b.baseAlpha) &&
           PrefabCanonicalFloatEqual(a.metallic, b.metallic) &&
           PrefabCanonicalFloatEqual(a.roughness, b.roughness) &&
           PrefabCanonicalFloatEqual(a.ior, b.ior) &&
           PrefabCanonicalFloatEqual(a.transmissionFactor, b.transmissionFactor) &&
           PrefabCanonicalVec3Equal(a.emissiveColor, b.emissiveColor) &&
           PrefabCanonicalFloatEqual(a.emissiveIntensity, b.emissiveIntensity) &&
           a.baseColorTextureIndex == b.baseColorTextureIndex &&
           a.normalTextureIndex == b.normalTextureIndex &&
           a.emissiveTextureIndex == b.emissiveTextureIndex &&
           a.metallicRoughnessTextureIndex == b.metallicRoughnessTextureIndex &&
           a.alphaMode == b.alphaMode &&
           PrefabCanonicalFloatEqual(a.alphaCutoff, b.alphaCutoff) &&
           a.sourceKey == b.sourceKey;
}

inline bool PrefabCanonicalScriptFieldEqual(const ScriptFieldEntry& a,
                                            const ScriptFieldEntry& b) noexcept
{
    if (a.type != b.type || a.value.index() != b.value.index()) return false;
    return std::visit([](const auto& x, const auto& y) -> bool {
        using X = std::decay_t<decltype(x)>;
        using Y = std::decay_t<decltype(y)>;
        if constexpr (!std::is_same_v<X, Y>) return false;
        else if constexpr (std::is_same_v<X, float>)
            return PrefabCanonicalFloatEqual(x, y);
        else if constexpr (std::is_same_v<X, double>)
            return PrefabCanonicalDoubleEqual(x, y);
        else if constexpr (std::is_same_v<X, glm::vec3>)
            return PrefabCanonicalVec3Equal(x, y);
        else return x == y;
    }, a.value, b.value);
}

inline bool PrefabCanonicalScriptFieldsEqual(const ScriptFieldMap& a,
                                             const ScriptFieldMap& b) noexcept
{
    if (a.size() != b.size()) return false;
    for (const auto& [name, value] : a)
    {
        const auto it = b.find(name);
        if (it == b.end() || !PrefabCanonicalScriptFieldEqual(value, it->second))
            return false;
    }
    return true;
}

inline bool PrefabCanonicalComponentEqual(const NameComponent& a,
                                          const NameComponent& b) noexcept
{ return a.name == b.name; }

inline bool PrefabCanonicalComponentEqual(const Transform& a,
                                          const Transform& b) noexcept
{
    return PrefabCanonicalVec3Equal(a.translation, b.translation) &&
           PrefabCanonicalQuatEqual(a.rotation, b.rotation) &&
           PrefabCanonicalVec3Equal(a.scale, b.scale);
}

inline bool PrefabCanonicalComponentEqual(const VisibleComponent& a,
                                          const VisibleComponent& b) noexcept
{ return a.visible == b.visible; }

inline bool PrimitiveComponentCanonicalEqual(const PrimitiveComponent& a,
                                             const PrimitiveComponent& b) noexcept
{
    return a.kind == b.kind && PrefabCanonicalFloatEqual(a.size, b.size) &&
           a.segments == b.segments && a.rings == b.rings;
}

inline bool PrefabCanonicalComponentEqual(const PrimitiveComponent& a,
                                          const PrimitiveComponent& b) noexcept
{ return PrimitiveComponentCanonicalEqual(a, b); }

inline bool PrefabCanonicalComponentEqual(const ImportedMeshSourceComponent& a,
                                          const ImportedMeshSourceComponent& b) noexcept
{ return PrefabCanonicalAssetReferenceEqual(a.model, b.model); }

inline bool PrefabCanonicalComponentEqual(const MaterialOverrideComponent& a,
                                          const MaterialOverrideComponent& b) noexcept
{ return PrefabCanonicalMaterialEqual(a.material, b.material) &&
         a.authored == b.authored && a.sourceMaterialKey == b.sourceMaterialKey; }

inline bool PrefabCanonicalComponentEqual(const LightComponent& a,
                                          const LightComponent& b) noexcept
{ return PrefabCanonicalVec3Equal(a.color, b.color) &&
         PrefabCanonicalFloatEqual(a.intensity, b.intensity) &&
         PrefabCanonicalFloatEqual(a.range, b.range) &&
         PrefabCanonicalFloatEqual(a.innerConeAngle, b.innerConeAngle) &&
         PrefabCanonicalFloatEqual(a.outerConeAngle, b.outerConeAngle) &&
         a.type == b.type; }

inline bool PrefabCanonicalComponentEqual(const CameraComponent& a,
                                          const CameraComponent& b) noexcept
{ return PrefabCanonicalFloatEqual(a.verticalFOV, b.verticalFOV) &&
         PrefabCanonicalFloatEqual(a.aperture, b.aperture) &&
         PrefabCanonicalFloatEqual(a.focusDistance, b.focusDistance) &&
         PrefabCanonicalVec3Equal(a.forwardDirection, b.forwardDirection); }

inline bool PrefabCanonicalComponentEqual(const MotionComponent& a,
                                          const MotionComponent& b) noexcept
{ return PrefabCanonicalVec3Equal(a.linearVelocity, b.linearVelocity); }

inline bool PrefabCanonicalComponentEqual(const ScriptComponent& a,
                                          const ScriptComponent& b) noexcept
{ return PrefabCanonicalAssetReferenceEqual(a.asset, b.asset) &&
         PrefabCanonicalScriptFieldsEqual(a.fieldValues, b.fieldValues); }

inline bool OptionalPrimitiveComponentCanonicalEqual(
    const std::optional<PrimitiveComponent>& a,
    const std::optional<PrimitiveComponent>& b) noexcept
{
    if (a.has_value() != b.has_value()) return false;
    return !a || PrimitiveComponentCanonicalEqual(*a, *b);
}

// Exact optional equality for typed durable values.  Callers supply the
// component's canonical comparator, so this helper cannot silently fall back
// to a partial or bytewise comparison when a component later gains transient
// fields.  Both absence and presence are compared atomically.
template<typename T, typename Equal>
inline bool OptionalComponentCanonicalEqual(const std::optional<T>& a,
                                            const std::optional<T>& b,
                                            Equal&& equal)
{
    if (a.has_value() != b.has_value()) return false;
    return !a || static_cast<bool>(std::forward<Equal>(equal)(*a, *b));
}

// Convenience for components that provide a complete value operator==.
// Prefer the comparator overload for ECS components whose structs also carry
// runtime/cache state.
template<typename T>
inline bool OptionalComponentExactEqual(const std::optional<T>& a,
                                        const std::optional<T>& b)
{
    if (a.has_value() != b.has_value()) return false;
    return !a || (*a == *b);
}

} // namespace rt2::core

#endif // RT2_PREFAB_COMPONENT_VALUE_EQUALITY_H
