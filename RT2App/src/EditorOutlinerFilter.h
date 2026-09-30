#pragma once

#ifndef RT2_CORE_EDITOR_OUTLINER_FILTER_H
#define RT2_CORE_EDITOR_OUTLINER_FILTER_H

#include "SceneManager.h"

#include <cstdint>
#include <string>
#include <string_view>

// ============================================================================
// Editor outliner filter core — Scene Browser classification and matching
// (user-requested follow-up).
//
// Bitmask entity-type classification from the live registry plus a pure
// name/type/query predicate. The render side (SceneEditorUI, ImGui) keeps
// tree traversal, selection, drag-drop, and the parents-of-matches rule;
// this file decides per-entity type and match so both are unit-testable.
//
// An entity may carry several types (mesh + collider is the common case):
// chips match ANY set bit, while the row tag shows one primary by fixed
// priority (Camera, Light, Physics, Audio, Script, Motion, Mesh, Entity).
// An empty enabled mask matches everything (no stuck-empty browser).
//
// CPU-only: SceneManager + component headers only. No ImGui.
// ============================================================================

namespace rt2::core {

enum class EntityBrowserType : std::uint32_t
{
    None    = 0,
    Mesh    = 1u << 0,
    Light   = 1u << 1,
    Camera  = 1u << 2,
    Physics = 1u << 3, // any body/shape/hinge/slider component
    Audio   = 1u << 4,
    Script  = 1u << 5,
    Motion  = 1u << 6,
};

inline constexpr std::uint32_t kEntityBrowserAllTypes =
    static_cast<std::uint32_t>(EntityBrowserType::Mesh) |
    static_cast<std::uint32_t>(EntityBrowserType::Light) |
    static_cast<std::uint32_t>(EntityBrowserType::Camera) |
    static_cast<std::uint32_t>(EntityBrowserType::Physics) |
    static_cast<std::uint32_t>(EntityBrowserType::Audio) |
    static_cast<std::uint32_t>(EntityBrowserType::Script) |
    static_cast<std::uint32_t>(EntityBrowserType::Motion);

struct BrowserTypeToken
{
    const char* tag; // "[Mesh]", "[Light]", ..., "[Entity]"
    float r, g, b;   // tag text color (theme-consistent, neutral-tinted hues)
};

// Primary tag for a type mask (priority above); Plain mask yields Entity.
BrowserTypeToken TokenForBrowserTypes(std::uint32_t types);

// Classification from the live scene. Never fails: unknown/dead entities
// classify as None (shown as plain entities, never hidden by error).
std::uint32_t ClassifyEntityForBrowser(
    const SceneManager& manager, SceneManager::EntityId entity);

// Pure predicate: type-mask intersection (empty mask passes all) AND
// case-insensitive name substring (empty query passes all).
bool MatchesBrowserFilter(std::string_view name, std::uint32_t types,
    std::string_view query, std::uint32_t enabledMask);

} // namespace rt2::core

#endif // RT2_CORE_EDITOR_OUTLINER_FILTER_H
