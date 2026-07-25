#pragma once

#ifndef RT2_CORE_ASSET_IDENTITY_H
#define RT2_CORE_ASSET_IDENTITY_H

#include "core/UUID.h"
#include "core/Error.h"

#include <filesystem>
#include <string>

// ============================================================================
// AssetIdentity — stable source-asset identity via per-asset sidecars.
//
// Phase 7 D8 decided that asset identity lives in per-asset sidecar files
// (e.g. "cube.glb.rt2meta") committed alongside the asset, not in a
// per-machine database. The sidecar is the source of truth; the assetId
// stored in an AssetReference is a cache of it.
//
// This module is CPU-only: no Vulkan, ImGui, Walnut, or renderer access. It
// links cleanly into RT2Tests and RT2SliceRunner. File I/O is isolated here
// so the identity logic is testable with a deterministic provider.
//
// Missing-sidecar behaviour (loud, not silent): a missing sidecar is NOT a
// quiet re-mint that leaves the scene's old ID stale. ResolveOrAssign
// returns a fresh ID and sets `minted=true` so the caller can record an
// AssetDiagnostic and, on the next save, persist the new ID back into the
// scene. See D8 in docs/game-engine-development-plan.md.
// ============================================================================

namespace rt2::core {

// Build the sidecar path for a source asset. "cube.glb" -> "cube.glb.rt2meta".
// This is the single place that defines the sidecar naming convention; every
// reader and writer goes through it so the convention cannot drift.
std::filesystem::path AssetSidecarPath(const std::filesystem::path& assetPath);

// Read an existing sidecar's asset ID. Returns nil and leaves err empty
// when the sidecar does not exist (a normal condition, not an error). On a
// malformed sidecar, returns nil and fills err with a Parse error so the
// caller can route it through AssetDiagnostic rather than silently minting.
UUID ReadSidecarId(const std::filesystem::path& sidecarPath, Error& err);

// Atomically write a sidecar with the given ID. Creates the parent directory
// if needed. On failure, leaves the previous file (if any) intact and fills
// err. The sidecar format is a single line: the canonical UUID string,
// optionally followed by a newline. This keeps diffs trivial and merges
// obvious.
bool WriteSidecarId(const std::filesystem::path& sidecarPath,
                    const UUID& id, Error& err);

// The read-or-mint primitive used by the import flow. Given a source asset
// path and a UUID provider:
//   - if a sidecar exists and parses, return its ID (minted=false);
//   - if no sidecar exists, mint a fresh v4 from the provider, write the
//     sidecar, and return the new ID (minted=true);
//   - if a sidecar exists but is malformed, mint a fresh v4, OVERWRITE the
//     bad sidecar, and return the new ID with minted=true and a Parse error
//     in err so the caller can emit a diagnostic. A bad sidecar is a
//     repairable defect, not a silent failure.
//
// `minted` is always set; it is the signal the editor uses to decide whether
// to report "imported existing asset <id>" or "assigned new id <id> to
// <path> (sidecar was <absent|malformed>)".
UUID ResolveOrAssign(const std::filesystem::path& assetPath,
                      IUuidProvider& provider,
                      bool& minted,
                      Error& err);

} // namespace rt2::core

#endif // RT2_CORE_ASSET_IDENTITY_H