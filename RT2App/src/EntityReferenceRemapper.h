#pragma once

#ifndef RT2_ENTITY_REFERENCE_REMAPPER_H
#define RT2_ENTITY_REFERENCE_REMAPPER_H

#include "PhysicsComponents.h"
#include "core/Error.h"
#include "core/UUID.h"

#include <unordered_map>
#include <vector>

// Kept opaque here so the remapper's public surface does not depend on the
// SceneManager or registry. The implementation only needs ScriptComponent's
// authored field map; W1 can pass the same component view after building its
// prefab-local UUID mapping.
struct ScriptComponent;

namespace rt2::core
{

class SceneDocument;

using EntityUuidRemap = std::unordered_map<UUID, UUID>;

// Rewrite only UUID-typed script fields whose value is present in remap.
// References outside the copied set, stale UUIDs, nil values, and all other
// field types are preserved exactly.
void RemapEntityReferences(const EntityUuidRemap& remap,
                           const std::vector<ScriptComponent*>& components);

// T2 physics persistence foundation: rewrite hinge/slider `otherBody`
// references whose value is present in remap (intra-copy references point at
// the copy's fresh UUIDs). References outside the copied set (valid external
// bodies), dangling UUIDs, and nil world anchors are preserved exactly;
// validation of the result is a separate loud step
// (ValidatePhysicsConstraintReferences), never a silent fix-up.
void RemapPhysicsConstraintReferences(
    const EntityUuidRemap& remap,
    const std::vector<PhysicsHingeComponent*>& hinges,
    const std::vector<PhysicsSliderComponent*>& sliders);

// T2: loud validation of hinge/slider references against a document.
// For every entity carrying PhysicsHingeComponent/PhysicsSliderComponent:
//   - the owner must also carry PhysicsBodyComponent (missing owner body is
//     refused; err.path names the owner UUID);
//   - a nil otherBody (world anchor) is always valid;
//   - a non-nil otherBody must resolve to a live entity in the document that
//     also carries PhysicsBodyComponent (unknown or body-less target is
//     refused; err.path names the owner UUID and err.detail names both the
//     owner and the referenced UUID);
//   - otherBody must not equal the owner's own UUID (self-constraint refused,
//     both UUIDs in the diagnostic).
// Returns true when every constraint reference is valid. External references
// (pointing outside a copied set) are valid here as long as they resolve;
// copy-path scoping (remap-inside vs preserve-outside) is the caller's job.
bool ValidatePhysicsConstraintReferences(const SceneDocument& doc, Error& err);

} // namespace rt2::core

#endif // RT2_ENTITY_REFERENCE_REMAPPER_H
