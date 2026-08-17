#pragma once

#ifndef RT2_PREFAB_COMPONENT_VALUE_EQUALITY_H
#define RT2_PREFAB_COMPONENT_VALUE_EQUALITY_H

#include "ECSComponents.h"

#include <optional>
#include <utility>

// CPU-only canonical comparisons shared by prefab planning and local edit
// code.  These deliberately compare durable authored fields only: Primitive
// has no derived mesh index, while optional comparison treats absence as a
// first-class value rather than as a structural instruction.
namespace rt2::core {

inline bool PrimitiveComponentCanonicalEqual(const PrimitiveComponent& a,
                                             const PrimitiveComponent& b) noexcept
{
    // A NaN is never a valid canonical recipe, so ordinary == intentionally
    // makes two such values unequal and keeps malformed input observable.
    return a.kind == b.kind && a.size == b.size &&
           a.segments == b.segments && a.rings == b.rings;
}

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
