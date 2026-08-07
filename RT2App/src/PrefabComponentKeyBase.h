#pragma once

#ifndef RT2_PREFAB_COMPONENT_KEY_BASE_H
#define RT2_PREFAB_COMPONENT_KEY_BASE_H

#include <string>
#include <string_view>

// ============================================================================
// PrefabComponentKey — the bare class (Phase 8 W3, D3).
//
// Split out of PrefabComponentKey.h into this dependency-free header so
// ECSComponents.h can include it at the top without pulling in
// PersistedComponents.h (whose include of ECSComponents.h at the top would
// otherwise re-enter a guarded PrefabComponentKey.h before PersistedComponents
//::Count is declared). This header depends only on the standard library; the
// wire-key constants, the frozen classification table and the
// PrefabComponentKeyFor specializations live in PrefabComponentKey.h.
//
// One identity per persisted component. Each table entry carries the wire
// string the scene codec already writes for that component (the top-level
// JSON member names in EntityRecordToJson, SceneSerializer.cpp) and an
// `overridable` bit. The wire string IS the component identity; there is no
// parallel numeric enum, because a renumbering would silently remap overrides
// onto the wrong component.
//
// A key's m_wire points into static constexpr storage (the table), never at a
// transient buffer. The class still hard-rejects construction from std::string
// so the realistic accident — an implicit std::string -> string_view
// conversion firing on a parse-local string, leaving a key that dangles once
// the buffer dies — is a compile error rather than a landmine.
// ============================================================================

class PrefabComponentKey
{
public:
    constexpr PrefabComponentKey() = default;

    constexpr PrefabComponentKey(std::string_view wire, bool overridable) noexcept
        : m_wire(wire), m_overridable(overridable)
    {
    }

    // Deleted: a std::string (lvalue or rvalue) binds to these instead of
    // implicitly converting to std::string_view. Constructing a key from a
    // function-local std::string would leave m_wire dangling once the buffer
    // dies — deleting the conversions makes the realistic accident (W3-D3's
    // "parse a string, build a key") a compile error rather than a landmine
    // that passes every test before crashing elsewhere. Keys are only ever
    // built from static constexpr storage via kPrefabTable / the
    // PrefabComponentKeyFor specializations, or resolved through
    // FindComponentByWire.
    PrefabComponentKey(const std::string&, bool) = delete;
    PrefabComponentKey(std::string&&, bool) = delete;

    constexpr std::string_view wire() const noexcept { return m_wire; }
    constexpr bool overridable() const noexcept { return m_overridable; }
    constexpr bool valid() const noexcept { return !m_wire.empty(); }

    friend bool operator==(const PrefabComponentKey& a, const PrefabComponentKey& b) noexcept
    {
        return a.m_wire == b.m_wire && a.m_overridable == b.m_overridable;
    }
    friend bool operator!=(const PrefabComponentKey& a, const PrefabComponentKey& b) noexcept
    {
        return !(a == b);
    }

private:
    std::string_view m_wire;
    bool m_overridable = false;
};

#endif // RT2_PREFAB_COMPONENT_KEY_BASE_H
