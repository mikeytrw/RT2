#pragma once

#ifndef RT2_CORE_PREFAB_SERIALIZER_H
#define RT2_CORE_PREFAB_SERIALIZER_H

#include "AssetResolver.h"
#include "SubtreeSnapshot.h"
#include "core/Error.h"
#include "json.hpp"

#include <filesystem>
#include <vector>

// ============================================================================
// PrefabSerializer — .rt2prefab native format (Phase 8).
//
// A prefab is an entity subtree saved as an asset. W0 delivered the asset kind
// and the file ENVELOPE (header + version + record list) with loud rejections
// for non-empty records; W1 replaces those rejections with the record codec.
//
// Format summary (version 1):
//   {
//     "header":   "rt2prefab",
//     "version":  1,
//     "entities": [
//       {
//         "templateId": "<uuid>",
//         "record": { ...scene component payload... }
//       }, ...
//     ]
//   }
//
// - The version constant is INDEPENDENT of SceneSerializer::SchemaVersion
//   (D5): changes to the .rt2prefab format must not force a .rt2scene schema
//   bump, and vice versa.
// - Per-record identity is PrefabEntityRecord::templateId — minted once when
//   the prefab asset is created and frozen in the file (amendment A1). It is
//   never regenerated and never derived from a scene UUID at instantiate time.
// - The record payload reuses SubtreeEntityRecord (SubtreeSnapshot.h); the
//   JSON codec is defined in SceneSerializer.cpp so it shares the per-
//   component codecs with scene serialization (D4).
// - HARD RULE: prefab files never contain resource-table indices.
//   MeshRef::meshIndex, MeshRef::materialIndex and
//   MaterialOverrideComponent::materialIndex are transient by design, and
//   override texture indices are repaired only from a currently-staged
//   material. The record codec strips them (verified by test).
// - The record payload NEVER carries PrefabInstanceComponent/
//   PrefabMemberComponent: those are scene-side components. A prefab file
//   holds template entities; template identity is the templateId field.
// - Record asset paths are stored verbatim (W1): no rebasing pass, and
//   instantiate resolves them relative to the destination scene.
//
// Atomic save: write to "<path>.tmp" then replace, so a crash never leaves a
// half-written prefab.
// ============================================================================

namespace rt2::core {

// Independent file-format version for .rt2prefab (0 = none reserved).
inline constexpr uint32_t kPrefabFormatVersion = 1;

// One entity in a prefab asset. templateId is the prefab-local identity
// (frozen at creation); record is the shared per-entity payload.
struct PrefabEntityRecord
{
    rt2::core::UUID     templateId;
    SubtreeEntityRecord record;
};

// A prefab asset document.
struct PrefabDocument
{
    uint32_t version = kPrefabFormatVersion;
    std::vector<PrefabEntityRecord> entities;
};

class PrefabSerializer
{
public:
    // The .rt2prefab format version, independent of .rt2scene's schema.
    static constexpr uint32_t FormatVersion = kPrefabFormatVersion;

    // Deterministically serialize a document to the exact bytes Save would
    // write (envelope + record codec + dump(2)). Shared by Save and by
    // callers that must compare against the file's current bytes before an
    // undo/redo or rollback (external-change guard). `content` is untouched
    // on failure.
    static bool Serialize(const PrefabDocument& doc,
                          std::string& content,
                          Error& err);

    // Atomically replace `path` with the given raw bytes (tmp + replace).
    // On failure the existing file is left intact (never truncated first).
    // Shared by Save and by the create rollback / undo restore paths so a
    // failed write cannot destroy recoverable state.
    static bool WriteBytesAtomic(const std::filesystem::path& path,
                                 const std::string& content,
                                 Error& err);

    // Write a prefab file (envelope + record codec). Atomic tmp+replace.
    static bool Save(const PrefabDocument& doc,
                     const std::filesystem::path& path,
                     Error& err);

    // Read a prefab file transactionally. Dest is replaced only on a fully
    // valid header+version+records; any failure leaves dest unchanged.
    static bool Load(PrefabDocument& doc,
                     const std::filesystem::path& path,
                     Error& err);
};

// ---- Record codec (defined in SceneSerializer.cpp) ----
//
// These reuse the scene per-component codecs. They are separate from
// PrefabSerializer's envelope plumbing because the component codecs live in
// SceneSerializer.cpp's internal helpers (W0 handover: reuse, don't
// re-serialise components).
//
// Write: the payload is written exactly like a scene entity record, then the
// transient resource-table indices are stripped (hard rule):
//   - "meshRef.materialIndex"      (MeshRef::materialIndex is transient)
//   - "materialOverride.material.*TextureIndex"  (repair needs a staged
//     material; a prefab has none)
// "meshIndex" is never written by the scene codec either.
//
// Read: the payload is decoded like a scene entity record. Stripped fields
// are absent, so they default (meshIndex=0, materialIndex=-1) and the
// instantiate path re-resolves them from source keys.

// Serialize one prefab entity record. On failure returns false with `err`
// filled; `out` is untouched on failure. `json` is nlohmann::json — the
// using-declaration lives only in .cpp files, so the header names it fully.
bool PrefabRecordToJson(const PrefabEntityRecord& record,
                        std::vector<AssetDiagnostic>& diagnostics,
                        Error& err,
                        nlohmann::json& out);

// Parse one prefab entity record. On failure returns false with `err` filled;
// `out` is untouched on failure.
bool JsonToPrefabRecord(const nlohmann::json& j,
                        Error& err,
                        PrefabEntityRecord& out);

} // namespace rt2::core

#endif // RT2_CORE_PREFAB_SERIALIZER_H
