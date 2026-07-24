#pragma once

#ifndef RT2_CORE_SCENE_SERIALIZER_H
#define RT2_CORE_SCENE_SERIALIZER_H

#include "SceneDocument.h"
#include "ScriptFieldReconcile.h"
#include "core/Error.h"
#include "core/UUID.h"

#include <filesystem>
#include <string>
#include <vector>

// ============================================================================
// SceneSerializer — native .rt2scene JSON format (schema version 3).
//
// Operates on SceneDocument (not bare ECSScene) so it can persist the
// environment map path, scene metadata, and UUID index alongside ECS data.
//
// Schema version 3 (Phase 6B W3 — typed Lua field persistence):
//   - Reads and writes v3 only. v1/v2 are rejected deliberately.
//   - Serializes durable asset references (ImportedMeshSourceComponent) and
//     authored material overrides (MaterialOverrideComponent). Does NOT
//     serialize decoded vertex buffers, pixel data, GPU handles, or
//     transient MeshRegistry indices.
//   - Environment: path only; pixels are re-read on load via
//     SceneAssetResolver::ResolveEnvironment.
//   - Primitive meshes (PrimitiveComponent) remain directly serializable.
//   - Paths are stored as portable, scene-relative UTF-8 where possible.
//
// Components serialized (v3):
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
//   - Schema version check: only v3 is accepted; every other version fails
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

struct SceneLoadReport
{
    std::vector<FieldDiagnostic> fieldDiagnostics;
    bool normalizedScriptMetadata = false;
    bool normalizedScriptFieldData = false;
    bool droppedScriptFieldData = false;
};

class SceneSerializer
{
public:
    // Save a document to a .rt2scene file. Atomic on Windows via
    // ReplaceFileW/MoveFileExW. On failure, leaves the existing file intact.
    // Saves as schema v3. Paths in asset references are rebased against
    // the save `path`'s parent directory where possible.
    static bool Save(const SceneDocument& doc, const std::filesystem::path& path, Error& err);

    // Save a document to `outPath`, but relativize asset references against
    // `logicalScenePath`'s parent directory instead of `outPath`. This is the
    // recovery-snapshot path: bytes land under the recovery directory, but
    // durable asset references remain resolvable against the original
    // authoring scene's logical root. Does NOT mutate doc.metadata.sourcePath.
    static bool SaveTo(const SceneDocument& doc,
                       const std::filesystem::path& outPath,
                       const std::filesystem::path& logicalScenePath,
                       Error& err);

    // Load a v3 .rt2scene transactionally. The destination is replaced only
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

    // Current schema version (written by Save).
    static constexpr uint32_t SchemaVersion = 3;
    // Lowest readable schema version (equal to SchemaVersion by hard cutover).
    static constexpr uint32_t MinReadVersion = 3;
};

} // namespace rt2::core

#endif // RT2_CORE_SCENE_SERIALIZER_H
