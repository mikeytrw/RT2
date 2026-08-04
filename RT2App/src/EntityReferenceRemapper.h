#pragma once

#ifndef RT2_ENTITY_REFERENCE_REMAPPER_H
#define RT2_ENTITY_REFERENCE_REMAPPER_H

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

using EntityUuidRemap = std::unordered_map<UUID, UUID>;

// Rewrite only UUID-typed script fields whose value is present in remap.
// References outside the copied set, stale UUIDs, nil values, and all other
// field types are preserved exactly.
void RemapEntityReferences(const EntityUuidRemap& remap,
                           const std::vector<ScriptComponent*>& components);

} // namespace rt2::core

#endif // RT2_ENTITY_REFERENCE_REMAPPER_H
