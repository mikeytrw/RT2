#pragma once

#ifndef RT2_CORE_ASSET_REFERENCE_H
#define RT2_CORE_ASSET_REFERENCE_H

#include "core/UUID.h"

#include <cstdint>
#include <string>

// ============================================================================
// AssetReference — neutral, CPU-only asset identity types.
//
// Phase 7 W3 extracts AssetKind, ImportSettings, and AssetReference into this
// neutral header so that leaf runtime types (SceneTexture, EnvironmentSettings)
// can carry an AssetReference without including ECSComponents.h (which would
// re-introduce the SceneTypes.h / ECSComponents.h include cycle documented in
// W3-P2).
//
// This header depends only on core/UUID.h and the standard library. It links
// cleanly into RT2Tests and RT2SliceRunner with no Vulkan/ImGui/Walnut/entt.
//
// ECSComponents.h includes this header and the names remain in the global
// namespace to match the pre-extraction source layout. Existing callers are
// unchanged. (rt2::core already aliases these via `using` in ECSComponents.h
// if/when needed; the engine's component layer is not namespaced.)
// ============================================================================

enum class AssetKind : uint8_t
{
    Unknown     = 0,
    Model       = 1,   // .gltf / .glb / .obj
    Texture     = 2,   // image referenced by a material
    Environment = 3,   // .hdr / .exr environment map
    Script      = 4,   // .lua script asset (Phase 6)
};

// Settings that affect how an asset is rebuilt on load. Only values that
// change the resulting geometry/material/texture should be persisted; display
// and runtime-only options are not stored here.
struct ImportSettings
{
    // OBJ importer profile.
    bool triangulate       = true;
    bool generateNormals   = false; // true if flat normals were generated
    bool mergeMegaMesh     = true;  // OBJ-specific: merge all shapes into one BLAS

    // glTF import profile. Geometry is byte-faithful; this knob only affects
    // material factors.
    //
    // glTF defines an absent metallicFactor/roughnessFactor as 1.0. A material
    // that ships neither a metallicRoughness texture nor explicit factors is
    // therefore, by spec, a fully metallic and fully rough surface — a rough
    // mirror. Exporters hit this constantly by omitting the values while
    // assuming a dielectric default, so the spec-correct import renders a
    // grey, non-converging patch (Intel Sponza's `dirt_decal` is one).
    //
    // When true, such a material is imported as a dielectric (metallic 0)
    // and a diagnostic records the correction. Default false: spec behaviour,
    // and existing assets keep their current identity.
    bool assumeDielectricWithoutMetalRough = false;

    bool operator==(const ImportSettings& o) const
    {
        return triangulate == o.triangulate
            && generateNormals == o.generateNormals
            && mergeMegaMesh == o.mergeMegaMesh
            && assumeDielectricWithoutMetalRough == o.assumeDielectricWithoutMetalRough;
    }
    bool operator!=(const ImportSettings& o) const { return !(*this == o); }
};

// A durable reference to an external source asset. The path is a portable,
// scene-relative UTF-8 path (forward slashes, normalized). It is resolved
// relative to the .rt2scene file at load time. Native save retains a
// normalized absolute path only when it cannot be relativized (for example,
// across Windows volumes) and reports that exceptional persistence as a
// NonPortable advisory.
//
// assetId is the stable identity of the source asset (Phase 7 W1, per D1/D2).
// It is the durable form of identity; path is a human-readable fallback for
// diagnostics and hand-editing. On a v3 scene the field is absent on read and
// left nil (additive migration, per D5); the first v4 save assigns it from
// the asset's sidecar (.rt2meta) or mints a fresh v4 and writes the sidecar
// (per D8). A nil assetId means "not yet assigned".
//
// W3 step 2 introduces the generic read-only locator (AssetResolver) that
// resolves an AssetReference against an explicit root and an AssetDatabase.
// Resolution is ID-first; path is fallback. The locator never mints, writes,
// or remaps — import/save/migration own identity repair.
struct AssetReference
{
    AssetKind               kind = AssetKind::Unknown;
    std::string             path;        // portable, scene-relative UTF-8 path
    ImportSettings          importSettings;
    std::string             sourceKey;   // stable source subresource identity
    rt2::core::UUID         assetId;     // stable source-asset identity (Phase 7)

    bool IsValid() const { return kind != AssetKind::Unknown && !path.empty(); }
};

#endif // RT2_CORE_ASSET_REFERENCE_H
