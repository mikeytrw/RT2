#pragma once

#ifndef RT2_PREFAB_PROPAGATION_COMPONENT_ADAPTER_H
#define RT2_PREFAB_PROPAGATION_COMPONENT_ADAPTER_H

#include "PrefabComponentKey.h"
#include "PrefabComponentValueEquality.h"
#include "PrefabSerializer.h"
#include "SceneSyncImpact.h"

#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace rt2::core {

// This is deliberately a closed set. MeshRef and the prefab link/marker
// components are derived/evidence state and remain outside generic component
// propagation policy.
using PropagationComponentSet = std::variant<
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

template<typename T>
struct PropagationComponentDescriptor;

template<typename T>
inline constexpr SyncImpact PropagationComponentImpact =
    PropagationComponentDescriptor<T>::Impact;

#define RT2_PROPAGATION_DESCRIPTOR(T, IMPACT)                                  \
template<> struct PropagationComponentDescriptor<T>                             \
{                                                                                \
    using Type = T;                                                              \
    static constexpr PrefabComponentKey KeyValue =                               \
        PrefabComponentKeyFor<T>::value;                                         \
    static constexpr SyncImpact Impact = IMPACT;                                 \
    static constexpr bool Overrideable = KeyValue.overridable();                 \
};

RT2_PROPAGATION_DESCRIPTOR(NameComponent, SyncImpact::None)
RT2_PROPAGATION_DESCRIPTOR(Transform, SyncImpact::Transform)
RT2_PROPAGATION_DESCRIPTOR(VisibleComponent, SyncImpact::Structural)
RT2_PROPAGATION_DESCRIPTOR(PrimitiveComponent, SyncImpact::Structural)
RT2_PROPAGATION_DESCRIPTOR(ImportedMeshSourceComponent, SyncImpact::Structural)
RT2_PROPAGATION_DESCRIPTOR(MaterialOverrideComponent, SyncImpact::Material)
RT2_PROPAGATION_DESCRIPTOR(LightComponent, SyncImpact::Material)
RT2_PROPAGATION_DESCRIPTOR(CameraComponent, SyncImpact::None)
RT2_PROPAGATION_DESCRIPTOR(MotionComponent, SyncImpact::None)
RT2_PROPAGATION_DESCRIPTOR(ScriptComponent, SyncImpact::None)

#undef RT2_PROPAGATION_DESCRIPTOR

template<typename T>
struct TypedComponentDelta
{
    using Type = T;
    static TypedComponentDelta Make(const UUID& entity, const UUID& templ,
                                    std::optional<T> beforeValue,
                                    std::optional<T> afterValue)
    {
        return TypedComponentDelta(entity, templ, std::move(beforeValue),
                                   std::move(afterValue));
    }

    static constexpr PrefabComponentKey Key() noexcept
    { return PropagationComponentDescriptor<T>::KeyValue; }

    UUID EntityUuid() const noexcept { return m_EntityUuid; }
    UUID TemplateId() const noexcept { return m_TemplateId; }
    const std::optional<T>& Before() const noexcept { return m_Before; }
    const std::optional<T>& After() const noexcept { return m_After; }

private:
    TypedComponentDelta(const UUID& entity, const UUID& templ,
                        std::optional<T> beforeValue,
                        std::optional<T> afterValue)
        : m_EntityUuid(entity), m_TemplateId(templ),
          m_Before(std::move(beforeValue)), m_After(std::move(afterValue)) {}

    UUID m_EntityUuid;
    UUID m_TemplateId;
    std::optional<T> m_Before;
    std::optional<T> m_After;
};

using PropagationComponentDeltaSet = std::variant<
    TypedComponentDelta<NameComponent>,
    TypedComponentDelta<Transform>,
    TypedComponentDelta<VisibleComponent>,
    TypedComponentDelta<PrimitiveComponent>,
    TypedComponentDelta<ImportedMeshSourceComponent>,
    TypedComponentDelta<MaterialOverrideComponent>,
    TypedComponentDelta<LightComponent>,
    TypedComponentDelta<CameraComponent>,
    TypedComponentDelta<MotionComponent>,
    TypedComponentDelta<ScriptComponent>>;

inline bool PropagationComponentEqual(const PropagationComponentSet& a,
                                      const PropagationComponentSet& b) noexcept;

class PrefabPropagationComponentDelta final
{
public:
    template<typename T>
    static PrefabPropagationComponentDelta Make(
        const UUID& entity, const UUID& templ,
        std::optional<T> before, std::optional<T> after)
    {
        static_assert(std::is_same_v<T, typename PropagationComponentDescriptor<T>::Type>);
        return PrefabPropagationComponentDelta(
            TypedComponentDelta<T>::Make(entity, templ,
                                          std::move(before), std::move(after)));
    }

    const PropagationComponentDeltaSet& Variant() const noexcept { return m_Value; }

    template<typename Visitor>
    decltype(auto) Visit(Visitor&& visitor) const
    { return std::visit(std::forward<Visitor>(visitor), m_Value); }

    UUID EntityUuid() const noexcept
    { return Visit([](const auto& delta) { return delta.EntityUuid(); }); }
    UUID TemplateId() const noexcept
    { return Visit([](const auto& delta) { return delta.TemplateId(); }); }
    PrefabComponentKey Key() const noexcept
    { return Visit([](const auto& delta) { return std::decay_t<decltype(delta)>::Key(); }); }

    std::optional<PropagationComponentSet> BeforeValue() const
    {
        return Visit([](const auto& delta) -> std::optional<PropagationComponentSet> {
            if (!delta.Before()) return std::nullopt;
            return PropagationComponentSet{*delta.Before()};
        });
    }

    std::optional<PropagationComponentSet> AfterValue() const
    {
        return Visit([](const auto& delta) -> std::optional<PropagationComponentSet> {
            if (!delta.After()) return std::nullopt;
            return PropagationComponentSet{*delta.After()};
        });
    }

    bool IsValid() const noexcept
    {
        return !EntityUuid().IsNull() && !TemplateId().IsNull() &&
               (BeforeValue().has_value() || AfterValue().has_value()) &&
               Visit([](const auto& delta) {
                   return delta.Before().has_value() || delta.After().has_value();
               });
    }

    bool IsNoOp() const noexcept
    {
        return Visit([](const auto& delta) {
            return OptionalComponentCanonicalEqual(
                delta.Before(), delta.After(),
                [](const auto& a, const auto& b) {
                    return PrefabCanonicalComponentEqual(a, b);
                });
        });
    }

    bool IsOverrideable() const noexcept
    { return Key().overridable(); }

    SyncImpact Impact() const noexcept
    { return Visit([](const auto& delta) {
        using T = typename std::decay_t<decltype(delta)>::Type;
        return PropagationComponentDescriptor<T>::Impact;
    }); }

    template<typename T>
    PrefabPropagationComponentDelta WithAfter(std::optional<T> after) const
    {
        return Visit([&](const auto& delta) {
            using U = typename std::decay_t<decltype(delta)>::Type;
            if constexpr (std::is_same_v<T, U>)
                return Make<T>(delta.EntityUuid(), delta.TemplateId(),
                               delta.Before(), std::move(after));
            else
                return *this;
        });
    }

    friend bool operator==(const PrefabPropagationComponentDelta& a,
                           const PrefabPropagationComponentDelta& b) noexcept
    {
        if (a.EntityUuid() != b.EntityUuid() || a.TemplateId() != b.TemplateId() ||
            a.Key() != b.Key()) return false;
        const auto beforeA = a.BeforeValue();
        const auto beforeB = b.BeforeValue();
        const auto afterA = a.AfterValue();
        const auto afterB = b.AfterValue();
        return OptionalPropagationEqual(beforeA, beforeB) &&
               OptionalPropagationEqual(afterA, afterB);
    }

private:
    template<typename T>
    explicit PrefabPropagationComponentDelta(T value)
        : m_Value(std::move(value)) {}

    static bool OptionalPropagationEqual(
        const std::optional<PropagationComponentSet>& a,
        const std::optional<PropagationComponentSet>& b) noexcept
    {
        if (a.has_value() != b.has_value()) return false;
        return !a || PropagationComponentEqual(*a, *b);
    }

    PropagationComponentDeltaSet m_Value;
};

inline bool PropagationComponentEqual(const PropagationComponentSet& a,
                                      const PropagationComponentSet& b) noexcept
{
    return std::visit([](const auto& x, const auto& y) -> bool {
        using X = std::decay_t<decltype(x)>;
        using Y = std::decay_t<decltype(y)>;
        if constexpr (!std::is_same_v<X, Y>) return false;
        else return PrefabCanonicalComponentEqual(x, y);
    }, a, b);
}

template<typename T>
std::optional<T> ReadPropagationComponent(const entt::registry& registry,
                                          entt::entity entity)
{
    if (const auto* value = registry.try_get<T>(entity)) return *value;
    return std::nullopt;
}

template<typename T>
std::optional<T> ReadPropagationSource(const PrefabEntityRecord& record)
{
    if constexpr (std::is_same_v<T, NameComponent>)
        return NameComponent{record.record.name};
    else if constexpr (std::is_same_v<T, Transform>)
        return Transform{record.record.translation, record.record.rotation,
                         record.record.scale};
    else if constexpr (std::is_same_v<T, VisibleComponent>)
        return VisibleComponent{record.record.visible};
    else if constexpr (std::is_same_v<T, PrimitiveComponent>)
        return record.record.hasPrimitive ? std::optional<T>(record.record.primitive) : std::nullopt;
    else if constexpr (std::is_same_v<T, ImportedMeshSourceComponent>)
        return record.record.hasImportedSource ? std::optional<T>(record.record.importedSource) : std::nullopt;
    else if constexpr (std::is_same_v<T, MaterialOverrideComponent>)
        return record.record.hasMaterialOverride ? std::optional<T>(record.record.materialOverride) : std::nullopt;
    else if constexpr (std::is_same_v<T, LightComponent>)
        return record.record.hasLight ? std::optional<T>(record.record.light) : std::nullopt;
    else if constexpr (std::is_same_v<T, CameraComponent>)
        return record.record.hasCamera ? std::optional<T>(record.record.camera) : std::nullopt;
    else if constexpr (std::is_same_v<T, MotionComponent>)
        return record.record.hasMotion ? std::optional<T>(record.record.motion) : std::nullopt;
    else if constexpr (std::is_same_v<T, ScriptComponent>)
        return record.record.hasScript ? std::optional<T>(record.record.script) : std::nullopt;
}

template<typename T>
void WritePropagationComponent(entt::registry& registry, entt::entity entity,
                               const std::optional<T>& value)
{
    if (value) registry.emplace_or_replace<T>(entity, *value);
    else registry.remove<T>(entity);
}

inline std::optional<PropagationComponentSet> ReadPropagationComponent(
    const PrefabPropagationComponentDelta& delta,
    const entt::registry& registry, entt::entity entity)
{
    return delta.Visit([&](const auto& typed) -> std::optional<PropagationComponentSet> {
        using T = typename std::decay_t<decltype(typed)>::Type;
        const auto value = ReadPropagationComponent<T>(registry, entity);
        return value ? std::optional<PropagationComponentSet>(PropagationComponentSet{*value})
                     : std::nullopt;
    });
}

inline void WritePropagationComponent(
    const PrefabPropagationComponentDelta& delta,
    entt::registry& registry, entt::entity entity, bool after)
{
    delta.Visit([&](const auto& typed) {
        using T = typename std::decay_t<decltype(typed)>::Type;
        WritePropagationComponent<T>(registry, entity, after ? typed.After() : typed.Before());
    });
}

inline bool PreflightPropagationComponent(
    const PrefabPropagationComponentDelta& delta,
    const entt::registry& registry, entt::entity entity) noexcept
{
    if (!delta.IsValid()) return false;
    const auto current = ReadPropagationComponent(delta, registry, entity);
    const auto before = delta.BeforeValue();
    return current.has_value() == before.has_value() &&
           (!current || PropagationComponentEqual(*current, *before));
}

static_assert(std::variant_size_v<PropagationComponentSet> == 10,
              "PropagationComponentSet must remain the exact ten propagation payloads");
static_assert(std::variant_size_v<PropagationComponentDeltaSet> == 10,
              "PropagationComponentDeltaSet must remain the exact ten typed deltas");
static_assert(!std::is_constructible_v<PrefabPropagationComponentDelta,
                                      PrefabComponentKey>,
              "typed deltas cannot be constructed from an independent key");

} // namespace rt2::core

#endif // RT2_PREFAB_PROPAGATION_COMPONENT_ADAPTER_H
