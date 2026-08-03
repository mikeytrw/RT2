#pragma once

#ifndef RT2_CORE_SCENE_ASSET_RESOLVER_H
#define RT2_CORE_SCENE_ASSET_RESOLVER_H

#include "SceneDocument.h"
#include "ECSComponents.h"
#include "AssetResolver.h"
#include "core/Error.h"
#include "core/UUID.h"

#include <filesystem>
#include <string>
#include <vector>

// ============================================================================
// SceneAssetResolver — resolves durable asset references into a SceneDocument.
//
// The serializer parses/writes schema and durable references only. This
// service rebuilds runtime mesh/texture/material/environment state from those
// references after a v2 .rt2scene is loaded structurally. It is the single
// place that calls SceneLoader (glTF/OBJ) and the environment loader, so the
// serializer itself never depends on the importer.
//
// Layering (must remain true):
//   - SceneSerializer: schema + durable refs only, no SceneLoader/RendererGPU.
//   - SceneAssetResolver: depends on SceneLoader (CPU importer) and the EXR/
//     HDR loader. Does NOT depend on RendererGPU, Vulkan, Walnut, ImGui,
//     GLFW, NRD, or NRI.
//   - SceneManager/app integration: invokes resolution transactionally and
//     then performs the existing GPU sync.
//   - RT2SliceRunner: uses a CPU resolver (this one) since it stays Vulkan-
//     free; the importer and EXR/HDR loaders are CPU-only.
//
// Resolution contract:
//   1. The caller hands in a SceneDocument that already has its entity
//      hierarchy, UUIDs, transforms, visibility, camera, and authored
//      components from the serializer. Imported entities carry
//      ImportedMeshSourceComponent; materials may include both source-
//      generated placeholders and authored overrides.
//   2. ResolveAll walks entities with ImportedMeshSourceComponent, loads the
//      referenced model through SceneLoader into a temporary ECSScene, maps
//      durable source keys to rebuilt mesh/material/texture indices, and
//      installs MeshRef components on the target document's entities. It
//      then re-applies authored MaterialOverrideComponent data.
//   3. ResolveEnvironment reads the environment map file (HDR/EXR) and fills
//      EnvironmentSettings::floatPixels/dimensions. A missing or unreadable
//      file is a distinct diagnostic: the document stays structurally valid
//      and the missing reference is recorded, not fatal.
//
// Transactionality:
//   - On any schema/parse/resolution failure, the input document is left
//     unchanged (resolution builds into a staging ECSScene and only copies
//     forward on success).
//   - Missing external assets are NOT fatal: the resolver records a
//     diagnostic, leaves the affected entity without a MeshRef (or with a
//     placeholder), and preserves the UUID/entity hierarchy.
//
// Path resolution is supplied explicitly by the host through one
// AssetResolutionContext. Project hosts provide the project asset root and
// database snapshot; standalone hosts provide the scene parent and no DB.
// ============================================================================

namespace rt2::core {

// AssetDiagnostic is now defined in AssetResolver.h (neutral, CPU-only) and
// re-exported here by include. Phase 7 W3 step 2 moved it so EnvironmentSettings
// and SceneTexture can carry references without a SceneTypes.h / ECSComponents.h
// include cycle (W3-P2). Severity gained Conflict (W3-Q5).

class SceneAssetResolver
{
public:
    // Resolve all imported model references and the environment map in `doc`.
    // `context` owns the path base and optional database snapshot. On success,
    // returns true
    // and `doc` has rebuilt MeshRef/material/texture state. On failure,
    // returns false and fills `err`; `doc` is unchanged on hard failure.
    // Missing assets produce diagnostics but do not cause failure unless
    // every imported entity is unresolvable (callers decide how to surface).
    //
    // `diagnostics` always receives one entry per failing reference even when
    // the function returns true (partial success).
    static bool ResolveAll(SceneDocument& doc,
                           const AssetResolutionContext& context,
                           std::vector<AssetDiagnostic>& diagnostics,
                           Error& err);

    // Resolve only the environment map referenced by doc.environment.path.
    // Fills floatPixels/width/height. Returns false and adds a diagnostic on
    // missing/unreadable files; does NOT clear the existing environment
    // reference (path/dims are preserved so a later successful reload can
    // reattach). Pixels are cleared on failure.
    static bool ResolveEnvironment(SceneDocument& doc,
                                   const AssetResolutionContext& context,
                                   std::vector<AssetDiagnostic>& diagnostics,
                                   Error& err);

    // Build a deterministic glTF source key.
    static std::string GltfSourceKey(int sceneIdx, int nodeIdx,
                                     int meshIdx, int primIdx);
    // Build a deterministic OBJ whole-model source key.
    static std::string ObjSourceKey();
};

} // namespace rt2::core

#endif // RT2_CORE_SCENE_ASSET_RESOLVER_H
