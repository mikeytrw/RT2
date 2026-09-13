#pragma once

#ifndef RT2_PHYSICS_COLLISION_GEOMETRY_H
#define RT2_PHYSICS_COLLISION_GEOMETRY_H

#include "AssetReference.h"
#include "core/Error.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// ============================================================================
// PhysicsCollisionGeometry — shared CPU collision decode (Bullet T4).
//
// Factored from SceneLoader's tinyobj/tinygltf parse paths (not the staging-
// ECSScene path, which would drag render/material linkage): OBJ uses the
// tinyobj merged mega-geometry walk; glTF uses the primitive-attribute
// (POSITION + INDEX) extraction. Render, material, and texture code are
// untouched and this translation unit links into the CPU-only targets.
//
// Identity (glossary Asset context): the caller resolves AssetReference.path
// to an absolute file first (ID-first via AssetResolver); this seam takes the
// resolved absolute path + exact sourceKey + geometry ImportSettings subset.
//   - OBJ:  sourceKey must be "obj:whole-model" (merged mega-geometry).
//   - glTF: sourceKey must be "gltf:scene=<s>:node=<n>:mesh=<m>:primitive=<p>"
//           selecting that primitive's POSITION/INDEX by exact match.
// Mismatch, malformed data, and unsupported extensions are typed Errors
// (existing core/Error.h codes), never empty/false silence.
//
// Units: decoded vertices are authoring-scale (file units = metres by the T4
// units contract); the entity's single uniform world scale composes once at
// PhysicsWorld build. contentHash is FNV-1a over the decoded floats/indices
// plus the sourceKey, used by the provider for stale detection alongside
// mtime/size.
// ============================================================================

namespace rt2::core {

// Hard caps: authored collision-only geometry. Oversize input is a loud
// Play-construction refusal, never a silent truncation.
inline constexpr size_t kMaxCollisionVertices = 250000;
inline constexpr size_t kMaxCollisionIndices = 1000000;

struct CollisionGeometry
{
    // xyz triplets, authoring scale.
    std::vector<float> vertices;
    std::vector<uint32_t> indices;
    uint64_t contentHash = 0;
};

// Decode collision geometry from an already-resolved absolute asset file.
// extension selects the parser (.obj vs .gltf/.glb); sourceKey must match
// exactly per the formats above. settings carries the geometry-affecting
// subset (triangulate for OBJ).
Result<CollisionGeometry> DecodeCollisionGeometry(
    const std::filesystem::path& absolutePath,
    const std::string& sourceKey,
    const ImportSettings& settings);

// FNV-1a 64 over raw bytes (content hashing primitive).
uint64_t FnV1a64(const void* data, size_t bytes, uint64_t seed = 1469598103934665603ULL);

} // namespace rt2::core

#endif // RT2_PHYSICS_COLLISION_GEOMETRY_H
