#pragma once

#ifndef RT2_CORE_SCENE_SERIALIZER_H
#define RT2_CORE_SCENE_SERIALIZER_H

#include "SceneDocument.h"
#include "ScriptFieldReconcile.h"
#include "AssetResolver.h"
#include "core/Error.h"
#include "core/UUID.h"

#include <filesystem>
#include <string>
#include <vector>

// ============================================================================
// SceneSerializer — native .rt2scene JSON format (schema version 7).
//
// Operates on SceneDocument (not bare ECSScene) so it can persist the
// environment map path, scene metadata, and UUID index alongside ECS data.
//
// Schema version 7 (Phase 8 prefab propagation contracts):
//   - Reads v3 through v7; v1/v2 are rejected deliberately.
//   - Serializes durable asset references (ImportedMeshSourceComponent) and
//     authored material overrides (MaterialOverrideComponent). Does NOT
//     serialize decoded vertex buffers, pixel data, GPU handles, or
//     transient MeshRegistry indices.
//   - Environment: path only; pixels are re-read on load via
//     SceneAssetResolver::ResolveEnvironment.
//   - Primitive meshes (PrimitiveComponent) remain directly serializable.
//   - v4+ project paths are relative to the active asset root; standalone v4+
//     paths are relative to the scene parent where possible.
//
// Components serialized (v3+):
//   EntityIdComponent, NameComponent, Transform, Hierarchy (parent UUID),
//   VisibleComponent, MeshRef (materialIndex only; meshIndex is transient),
//   PrimitiveComponent, LightComponent, CameraComponent, MotionComponent,
//   ImportedMeshSourceComponent, MaterialOverrideComponent, ScriptComponent.
//
// Save:
//   - Atomic: write to path + ".tmp", then ReplaceFileW/MoveFileExW.
//   - Deterministic: entities sorted by UUID, fixed float precision, stable
//     key order for readable source-control diffs.
//   - Rejects scenes that cannot reopen: every entity with a MeshRef must
//     have either a PrimitiveComponent or an ImportedMeshSourceComponent.
//
// Load:
//   - Two-pass: create entities + components by UUID, then resolve parent
//     UUID references.
//   - Transactional: parse into a temporary document; only on success does
//     the caller swap it in as the live authoring scene. A parse/schema
//     failure cannot corrupt the live scene.
//   - Schema version check: only v3 through v7 are accepted; every other version fails
//     with Error{SchemaVersion}.
//   - Does NOT resolve external assets; the caller runs SceneAssetResolver
//     after a successful load to rebuild meshes/textures/environment.
//
// CloneInMemory:
//   - Reuses the same component visitor/two-pass reference resolution as load.
//   - Transactional: build into a temporary, validate, then replace dst.
//   - Preserves authored UUIDs exactly.
//   - Does NOT clone transient runtime state (gpuCache, dirty flag,
//     prevWorldMatrix, script VM, physics, audio, renderer history).
// ============================================================================

namespace rt2::core {

struct AssetDiagnostic;

// Reconstruct mesh geometry from a primitive and register it in the given
// mesh registry. Returns the new mesh index, or 0 on an unknown primitive
// kind. Shared by the scene load path (BuildDocumentFromRecords) and the
// prefab instantiate path so both rebuild primitive geometry identically.
uint32_t RegisterPrimitiveMesh(MeshRegistry& meshReg, const PrimitiveComponent& prim);

struct SceneLoadReport
{
    uint32_t sourceVersion = 0;
    std::vector<FieldDiagnostic> fieldDiagnostics;
    std::vector<AssetDiagnostic> assetDiagnostics;
    bool normalizedScriptMetadata = false;
    bool normalizedScriptFieldData = false;
    bool droppedScriptFieldData = false;
    bool requiresAssetMigration = false;
    bool hasNonPortableAsset = false;
};

class SceneSerializer
{
public:
    // Save a document to a .rt2scene file. Atomic on Windows via
    // ReplaceFileW/MoveFileExW. On failure, leaves the existing file intact.
    // Saves as schema v7. Project paths use the runtime asset root; standalone
    // the save `path`'s parent directory where possible.
    static bool Save(const SceneDocument& doc,
                     const std::filesystem::path& path,
                     std::vector<AssetDiagnostic>& diagnostics,
                     Error& err);

    // Save a document to `outPath`, but relativize asset references against
    // `logicalScenePath`'s parent directory instead of `outPath`. This is the
    // recovery-snapshot path: bytes land under the recovery directory, but
    // durable asset references remain resolvable against the original
    // authoring scene's logical root. Does NOT mutate doc.metadata.sourcePath.
    static bool SaveTo(const SceneDocument& doc,
                       const std::filesystem::path& outPath,
                       const std::filesystem::path& logicalScenePath,
                       std::vector<AssetDiagnostic>& diagnostics,
                       Error& err);

    // Load a v3+ .rt2scene transactionally. The destination is replaced only
    // after every parse/build pass succeeds and is unchanged on failure.
    // Does NOT resolve
    // external assets — call SceneAssetResolver::ResolveAll afterward.
    static bool Load(SceneDocument& doc, const std::filesystem::path& path, Error& err);

    // Production load path with non-fatal script-field diagnostics. Structural
    // scene/component failures still return false and preserve the destination.
    static bool Load(SceneDocument& doc, const std::filesystem::path& path,
                     SceneLoadReport& report, Error& err);

    // Deep-clone a document in memory. Reuses the same two-pass component
    // visitor as Load so persistence and Play share coverage by construction.
    // Preserves authored UUIDs exactly. Does NOT clone gpuCache, dirty flag,
    // or other transient runtime state — those are initialized after Play()
    // activates the clone.
    static bool CloneInMemory(const SceneDocument& src, SceneDocument& dst, Error& err);

    // Current schema version (written by Save). v7 adds primitive override
    // markers; v6's original eight override keys remain readable.
    static constexpr uint32_t SchemaVersion = 7;
    // v6 introduced prefab override vectors and project-owned identity. Keep
    // this boundary separate from the current version so v6 scenes retain
    // their existing semantics after v7 is introduced.
    static constexpr uint32_t PrefabOverrideSchemaVersion = 6;
    static constexpr uint32_t PrimitiveOverrideSchemaVersion = 7;
    static constexpr uint32_t ProjectBindingSchemaVersion = 6;
    static constexpr bool UsesProjectBinding(uint32_t version) noexcept
    { return version >= ProjectBindingSchemaVersion; }
    // Lowest readable schema version; v3 remains readable for migration.
    static constexpr uint32_t MinReadVersion = 3;

    // W3-D6: promote a document to the current schema the moment the first
    // override is added, as part of the same atomic mutation. Load records the
    // source schema into doc.metadata.schemaVersion, and SaveTo deliberately
    // preserves an older one (it passes min(doc.metadata.schemaVersion,
    // SchemaVersion)), which is how a recovery snapshot would otherwise be
    // written as v5 with the override set silently dropped. Calling this makes
    // the subsequent recovery capture write v6 so the override set survives.
    //
    // The marking path (S5/S6) calls this only when it is actually adding an
    // override, so an untouched older-schema document keeps writing older-
    // schema recovery snapshots exactly as today (pinned by Phase7W5Tests).
    // Pass the document's before/after schemaVersion through the command's
    // undo state so undo can restore the prior version.
    //
    // Returns true if the document was promoted (was below the current
    // schema), false if it was already current (a no-op, also used to detect
    // "no upgrade needed").
    static bool PromoteSchemaVersion(SceneDocument& doc);
};

} // namespace rt2::core

#endif // RT2_CORE_SCENE_SERIALIZER_H
