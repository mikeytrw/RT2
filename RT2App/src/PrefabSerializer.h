#pragma once

#ifndef RT2_CORE_PREFAB_SERIALIZER_H
#define RT2_CORE_PREFAB_SERIALIZER_H

#include "SubtreeSnapshot.h"
#include "core/Error.h"

#include <filesystem>
#include <vector>

// ============================================================================
// PrefabSerializer — .rt2prefab native format (Phase 8 W0).
//
// A prefab is an entity subtree saved as an asset. W0 delivers the asset kind
// and the file ENVELOPE: header + version + an empty record list. Subtree
// capture and entity records are W1.
//
// Format summary (version 1):
//   {
//     "header":   "rt2prefab",
//     "version":  1,
//     "entities": []
//   }
//
// - The version constant is INDEPENDENT of SceneSerializer::SchemaVersion
//   (D5): changes to the .rt2prefab format must not force a .rt2scene schema
//   bump, and vice versa.
// - The record shape reuses SubtreeEntityRecord (SubtreeSnapshot.h) so W1's
//   record codec shares the component codecs with scene serialization (D4).
// - HARD RULE: prefab files must never contain resource-table indices.
//   MeshRef::meshIndex and MaterialOverrideComponent::materialIndex are
//   transient by design, and override texture indices are repaired only from
//   a currently-staged material. The W1 record codec must strip them; the W0
//   envelope contains no such fields at all.
// - W0 boundary: Save refuses a non-empty record list and Load refuses to
//   decode one, loudly — never silently drop or invent records. W1 replaces
//   those rejections with the real codec.
//
// Atomic save: write to "<path>.tmp" then replace, so a crash never leaves a
// half-written prefab.
// ============================================================================

namespace rt2::core {

// Independent file-format version for .rt2prefab (0 = none reserved).
inline constexpr uint32_t kPrefabFormatVersion = 1;

// A prefab asset document. For W0 the record list is always empty; W1 fills
// it from a captured SubtreeSnapshot.
struct PrefabDocument
{
    uint32_t version = kPrefabFormatVersion;
    std::vector<SubtreeEntityRecord> entities;
};

class PrefabSerializer
{
public:
    // The .rt2prefab format version, independent of .rt2scene's schema.
    static constexpr uint32_t FormatVersion = kPrefabFormatVersion;

    // Write a prefab file. W0 writes the envelope with an empty record list;
    // a non-empty record list is rejected loudly (the codec is W1's).
    static bool Save(const PrefabDocument& doc,
                     const std::filesystem::path& path,
                     Error& err);

    // Read a prefab file transactionally. Dest is replaced only on a fully
    // valid header+version; a wrong header or version is a hard error. A
    // non-empty record list is rejected loudly (decode is W1's).
    static bool Load(PrefabDocument& doc,
                     const std::filesystem::path& path,
                     Error& err);
};

} // namespace rt2::core

#endif // RT2_CORE_PREFAB_SERIALIZER_H
