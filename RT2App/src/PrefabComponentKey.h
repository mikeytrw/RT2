#pragma once

#ifndef RT2_PREFAB_COMPONENT_KEY_H
#define RT2_PREFAB_COMPONENT_KEY_H

#include "PersistedComponents.h"
#include "PrefabComponentKeyBase.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

// ============================================================================
// PrefabComponentKey and the frozen classification table (Phase 8 W3, D3).
//
// The bare PrefabComponentKey class lives in PrefabComponentKeyBase.h
// (dependency-free, included by ECSComponents.h at the top). This header holds
// everything that needs the persisted component types and
// PersistedComponents::Count: the wire constants, the frozen classification
// table, and the PrefabComponentKeyFor specializations.
//
// One identity per persisted component. Each table entry carries the wire
// string the scene codec already writes for that component (the top-level
// JSON member names in EntityRecordToJson, SceneSerializer.cpp:672-836) and an
// `overridable` bit. The wire string IS the component identity; there is no
// parallel numeric enum, because a renumbering would silently remap overrides
// onto the wrong component.
//
// The table is explicit and hand-maintained — never derived from typeid, EnTT
// storage order, or PersistedComponents::ForEach order at runtime. It must stay
// in the same order as PersistedComponents::ForEach (PersistedComponents.h:18-38);
// the two static_asserts below and the override test suite are what stop the
// table drifting out of sync (someone adds a 14th component, or reorders
// ForEach, and overrides would silently attach to the wrong component).
//
// Unknown input is loudly unresolvable: FindComponentByWire returns an empty
// optional for a name the table does not contain, never a plausible-looking
// default. Silently mapping an unrecognised name to a default key is exactly
// the swallowed-failure bug class this header exists to prevent — it would
// convert "this instance diverged" into "this instance tracks the source".
// ============================================================================

// Wire strings, one per component. These are the exact member names
// EntityRecordToJson writes (SceneSerializer.cpp). Never rename one without
// changing the codec; the classifications below share them.
namespace PrefabWireKeys
{
inline constexpr std::string_view kName             = "name";
inline constexpr std::string_view kTransform        = "transform";
inline constexpr std::string_view kVisible          = "visible";
inline constexpr std::string_view kMeshRef          = "meshRef";
inline constexpr std::string_view kPrimitive        = "primitive";
inline constexpr std::string_view kImportedSource   = "importedSource";
inline constexpr std::string_view kMaterialOverride = "materialOverride";
inline constexpr std::string_view kLight            = "light";
inline constexpr std::string_view kCamera           = "camera";
inline constexpr std::string_view kMotion           = "motion";
inline constexpr std::string_view kScript           = "script";
inline constexpr std::string_view kPrefabInstance   = "prefabInstance";
inline constexpr std::string_view kPrefabMember     = "prefabMember";
}

// The frozen classification table. One entry per persisted component, in the
// same order as PersistedComponents::ForEach. Hand-maintained: adding a
// component here requires extending PersistedComponents and specialising
// PrefabComponentKeyFor, and the size/overridable assertions below fail loudly
// if any of the three drifts.
inline constexpr std::array<PrefabComponentKey, PersistedComponents::Count> kPrefabTable = {
    PrefabComponentKey(PrefabWireKeys::kName,             true),  // NameComponent
    PrefabComponentKey(PrefabWireKeys::kTransform,        true),  // Transform
    PrefabComponentKey(PrefabWireKeys::kVisible,          true),  // VisibleComponent
    PrefabComponentKey(PrefabWireKeys::kMeshRef,          false), // MeshRef
    PrefabComponentKey(PrefabWireKeys::kPrimitive,        false), // PrimitiveComponent
    PrefabComponentKey(PrefabWireKeys::kImportedSource,   false), // ImportedMeshSourceComponent
    PrefabComponentKey(PrefabWireKeys::kMaterialOverride, true),  // MaterialOverrideComponent
    PrefabComponentKey(PrefabWireKeys::kLight,            true),  // LightComponent
    PrefabComponentKey(PrefabWireKeys::kCamera,           true),  // CameraComponent
    PrefabComponentKey(PrefabWireKeys::kMotion,           true),  // MotionComponent
    PrefabComponentKey(PrefabWireKeys::kScript,           true),  // ScriptComponent
    PrefabComponentKey(PrefabWireKeys::kPrefabInstance,   false), // PrefabInstanceComponent
    PrefabComponentKey(PrefabWireKeys::kPrefabMember,     false), // PrefabMemberComponent
};

// Required compile-time assertion 1: the table always covers every persisted
// component. Fires when a component is added to PersistedComponents without
// the table being extended (or when Count changes).
static_assert(kPrefabTable.size() == PersistedComponents::Count,
    "PrefabComponentKey table is out of sync with PersistedComponents::Count");

// Required compile-time assertion 2: exactly 8 components are overridable.
// Fires when any component's overridable bit is changed without that being
// considered, or when a new component is added with an unconsidered bit.
constexpr std::size_t CountOverridableEntries() noexcept
{
    std::size_t n = 0;
    for (const auto& key : kPrefabTable) if (key.overridable()) ++n;
    return n;
}
static_assert(CountOverridableEntries() == 8,
    "Expected exactly 8 overridable persisted components; "
    "re-examine each entry's overridable bit");

// Type -> key mapping, used by the override-vector mark/apply logic and by the
// suite to drive PersistedComponents::ForEach over the real classifications.
template<typename T> struct PrefabComponentKeyFor;

template<> struct PrefabComponentKeyFor<NameComponent>
    { static constexpr PrefabComponentKey value = PrefabComponentKey(PrefabWireKeys::kName, true); };
template<> struct PrefabComponentKeyFor<Transform>
    { static constexpr PrefabComponentKey value = PrefabComponentKey(PrefabWireKeys::kTransform, true); };
template<> struct PrefabComponentKeyFor<VisibleComponent>
    { static constexpr PrefabComponentKey value = PrefabComponentKey(PrefabWireKeys::kVisible, true); };
template<> struct PrefabComponentKeyFor<MeshRef>
    { static constexpr PrefabComponentKey value = PrefabComponentKey(PrefabWireKeys::kMeshRef, false); };
template<> struct PrefabComponentKeyFor<PrimitiveComponent>
    { static constexpr PrefabComponentKey value = PrefabComponentKey(PrefabWireKeys::kPrimitive, false); };
template<> struct PrefabComponentKeyFor<ImportedMeshSourceComponent>
    { static constexpr PrefabComponentKey value = PrefabComponentKey(PrefabWireKeys::kImportedSource, false); };
template<> struct PrefabComponentKeyFor<MaterialOverrideComponent>
    { static constexpr PrefabComponentKey value = PrefabComponentKey(PrefabWireKeys::kMaterialOverride, true); };
template<> struct PrefabComponentKeyFor<LightComponent>
    { static constexpr PrefabComponentKey value = PrefabComponentKey(PrefabWireKeys::kLight, true); };
template<> struct PrefabComponentKeyFor<CameraComponent>
    { static constexpr PrefabComponentKey value = PrefabComponentKey(PrefabWireKeys::kCamera, true); };
template<> struct PrefabComponentKeyFor<MotionComponent>
    { static constexpr PrefabComponentKey value = PrefabComponentKey(PrefabWireKeys::kMotion, true); };
template<> struct PrefabComponentKeyFor<ScriptComponent>
    { static constexpr PrefabComponentKey value = PrefabComponentKey(PrefabWireKeys::kScript, true); };
template<> struct PrefabComponentKeyFor<PrefabInstanceComponent>
    { static constexpr PrefabComponentKey value = PrefabComponentKey(PrefabWireKeys::kPrefabInstance, false); };
template<> struct PrefabComponentKeyFor<PrefabMemberComponent>
    { static constexpr PrefabComponentKey value = PrefabComponentKey(PrefabWireKeys::kPrefabMember, false); };

// Overridable predicate. The by-name form keeps a non-overridable name from
// ever being treated as one (the W3 boundary test asserts each excluded
// component is rejected by name, not merely that the runner answers false).
constexpr bool IsOverridable(const PrefabComponentKey& key) noexcept { return key.overridable(); }

template<typename T>
constexpr bool IsOverridable() noexcept { return PrefabComponentKeyFor<T>::value.overridable(); }

// Resolve a wire name to its classification. Returns an empty optional for any
// unrecognised wire — never a fabricated default — so an override referring to
// an unknown component is loudly unresolvable rather than silently mapped.
inline std::optional<PrefabComponentKey> FindComponentByWire(std::string_view wire) noexcept
{
    for (const auto& key : kPrefabTable) if (key.wire() == wire) return key;
    return std::nullopt;
}

#endif // RT2_PREFAB_COMPONENT_KEY_H