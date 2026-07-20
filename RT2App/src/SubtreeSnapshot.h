#pragma once

#ifndef RT2_SUBTREE_SNAPSHOT_H
#define RT2_SUBTREE_SNAPSHOT_H

#include "ECSComponents.h"
#include "TransformEditing.h"
#include "core/UUID.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstddef>
#include <string>
#include <vector>

// ============================================================================
// SubtreeSnapshot — CPU-only, serializable capture of a contiguous subtree
// for Phase 3B1 structural command Undo/Redo.
//
// A snapshot captures ONLY the affected subtree (never the whole scene). It
// reuses the serializer's per-entity component payload representation so it
// stays aligned with the persisted format. Each entity record carries its
// UUID, name, parent UUID (within the snapshot), local TRS, visibility, and
// optional full-value payloads for every persisted component (MeshRef,
// PrimitiveComponent, ImportedMeshSourceComponent, MaterialOverrideComponent
// with the full SceneMaterial + sourceMaterialKey, LightComponent,
// CameraComponent, MotionComponent).
//
// Root sibling anchors preserve exact Outliner position. Restoration fails
// atomically rather than silently appending if the anchors are inconsistent
// with the current parent's children list (or the root-entity list).
//
// Root-entity ordering is unspecified (registry-iteration order, no explicit
// authored ordering vector); Phase 3B1 does not introduce a root-order model.
//
// "Exact" state comparisons (RemoveSubtreesExact) compare authoritative
// authored state only — not derived world matrices, GPU caches, selection
// state, or other transient editor/runtime data.
//
// ============================================================================

struct SubtreeEntityRecord
{
	rt2::core::UUID uuid;
	std::string     name;
	rt2::core::UUID parentUuid;   // nil = root in this snapshot

	glm::vec3 translation{ 0.0f, 0.0f, 0.0f };
	glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
	glm::vec3 scale{ 1.0f, 1.0f, 1.0f };
	bool      visible = true;

	bool      hasMeshRef = false;
	uint32_t  meshIndex = 0;       // transient slot index; stable only while
	                               // no compaction occurs (3B1 invariant)
	int       materialIndex = -1;

	bool      hasPrimitive = false;
	PrimitiveComponent primitive{};

	bool      hasImportedSource = false;
	ImportedMeshSourceComponent importedSource{};

	bool      hasMaterialOverride = false;
	MaterialOverrideComponent materialOverride{};

	bool      hasLight = false;
	LightComponent light{};

	bool      hasCamera = false;
	CameraComponent camera{};

	bool      hasMotion = false;
	MotionComponent motion{};

	// Phase 6: script component. Carried through subtree snapshots so
	// duplicate/paste of scripted entities preserves the binding. 6A carries
	// the asset reference + (empty) field values; 6B adds field-value
	// persistence.
	bool      hasScript = false;
	ScriptComponent script{};
};

struct RootSiblingAnchor
{
	rt2::core::UUID prevSibling;   // nil = first child of parent (or first root)
	rt2::core::UUID nextSibling;   // nil = last child of parent (or last root)
	std::size_t     childIndex = 0; // diagnostic cross-check against live state
};

struct SubtreeSnapshot
{
	// Pre-order traversal of every entity in the captured subtree(s). Roots
	// appear first, in the caller-supplied order; descendants follow in
	// deterministic pre-order.
	std::vector<SubtreeEntityRecord> entities;

	// The canonical root UUIDs (parallel to rootAnchors). Each entry is a
	// root of the captured subtree — its parentUuid is either nil or a UUID
	// not present in this snapshot.
	std::vector<rt2::core::UUID> rootUuids;

	// Per-root sibling anchor in the parent's children list (or the root-
	// entity list when parentUuid is nil). Parallel to rootUuids.
	std::vector<RootSiblingAnchor> rootAnchors;
};

// ============================================================================
// ReparentEdit — one entry in an atomic batch reparent (ReparentBatch).
//
// For PreserveWorld, ReparentBatch converts `worldMatrix` to local against
// the new parent (singular/shear => fail all). For PreserveLocal, the local
// TRS is applied verbatim. ReparentCommand stores before/after ReparentEdit
// lists; Undo always uses PreserveLocal with the stored before-local TRS.
//
// `anchor` is the authoritative sibling position in the new parent's
// children list (or the root-entity list when newParent is nil). Restoration
// fails atomically if the anchor is inconsistent with live state.
//
// ============================================================================

struct ReparentEdit
{
	rt2::core::UUID  entity;
	rt2::core::UUID  newParent;           // nil = scene root
	EditableTRS      localTRS;            // used for PreserveLocal
	glm::mat4        worldMatrix = glm::mat4(1.0f); // used for PreserveWorld
	RootSiblingAnchor anchor;
};

#endif // RT2_SUBTREE_SNAPSHOT_H