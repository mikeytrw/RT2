#pragma once

#ifndef RT2_CORE_RUNTIME_SCENE_MUTATOR_H
#define RT2_CORE_RUNTIME_SCENE_MUTATOR_H

#include "core/Error.h"
#include "core/UUID.h"
#include "ECSComponents.h"

#include <optional>
#include <string>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// ============================================================================
// RuntimeSceneMutator — owns the runtime-only structural invariants for the
// Phase 4 deferred structural-operation queue.
//
// The naive approach — a private RuntimeSceneController helper that emplaces
// directly into the runtime ECSScene registry — would duplicate SceneManager
// invariants inside the controller and silently break:
//   EntityIdComponent / uuidIndex consistency, hierarchy parent/children
//   invariants, transform dirtiness, subtree destruction semantics.
//
// RuntimeSceneMutator is a small, Vulkan-free class in rt2::core that operates
// on a SceneDocument and exposes only CreateEntity and DestroySubtree (no
// compaction, no command history, no sync callbacks, no material overrides).
// It reuses SceneHierarchy::CollectSubtreePostOrder and SceneGraph::MarkDirty,
// both of which are linked into RT2Tests without Vulkan.
//
// It does NOT call NotifyAuthoringChanged (no authoring revision to bump on the
// runtime document) and does NOT touch any SceneManager-private state. The
// runtime document has no compaction invariant; it is destroyed on Stop.
//
// Phase 4 supports only the empty + transform + name + visibility component
// set. No mesh, no light, no camera, no primitive. Phase 6 scripting may
// extend the surface; Phase 4 deliberately keeps it minimal so the invariants
// are tractable and testable.
// ============================================================================

namespace rt2::core {

class SceneDocument;

struct RuntimeEntityCreateDesc
{
    std::string name;
    std::optional<UUID> parentUuid;     // nullopt = root
    std::optional<glm::vec3> translation;
    std::optional<glm::quat> rotation;
    std::optional<glm::vec3> scale;
    // Phase 6 scripting: optional script component to emplace on the new
    // runtime entity. Present only when world.spawn(desc) included a script
    // binding. The mutator emplaces ScriptComponent verbatim (the caller
    // fills the asset reference + field values); ScriptSystem::SyncScript-
    // Environments constructs the live sol2 environment for the new entity
    // at the next safe point. Phase 4 deliberately omitted this; Phase 6A
    // adds it because scripted spawning must produce scripted entities.
    std::optional<ScriptComponent> script;
};

class RuntimeSceneMutator
{
public:
    // Create an entity with a caller-allocated UUID (the controller
    // allocates the UUID at queue time). Returns Failure if the UUID is
    // already present or the parent UUID does not resolve. Emplaces:
    // Transform, NameComponent, VisibleComponent, EntityIdComponent,
    // Hierarchy (if parent). Marks the transform dirty. Initializes
    // prevWorldMatrix = worldMatrix after the first SceneGraph evaluation
    // (the controller does this after the batch is applied — see Phase 4
    // spec §5).
    Result<UUID> CreateEntity(SceneDocument& doc,
                              const UUID& uuid,
                              const RuntimeEntityCreateDesc& desc) const;

    // Destroy the subtree rooted at `uuid` (post-order collect, parent
    // unlink, UUID erase from doc.uuidIndex, registry.destroy). Returns
    // Failure if the UUID does not resolve. No compaction (the runtime
    // document has no compaction invariant; it is destroyed on Stop).
    Result<void> DestroySubtree(SceneDocument& doc, const UUID& uuid) const;
};

} // namespace rt2::core

#endif // RT2_CORE_RUNTIME_SCENE_MUTATOR_H