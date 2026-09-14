#pragma once

#ifndef RT2_PHYSICS_INSPECTOR_STATE_H
#define RT2_PHYSICS_INSPECTOR_STATE_H

#include "PhysicsComponents.h"
#include "core/UUID.h"

#include <optional>
#include <string>

// ============================================================================
// PhysicsInspectorState — CPU-testable working-copy policy for the T4 minimal
// physics Inspector (Bullet review fixup F).
//
// The ImGui layer edits working copies, not live components; Apply commits
// exactly one command per component (before = live read fresh at Apply time,
// after = working copy). This struct owns the resync/conflict/reset rules so
// they are unit-tested without ImGui:
//
//   - Reseed on selection (target) change. A clean copy also follows live
//     presence changes (component added/removed out-of-band); a dirty copy
//     instead conflicts and remains available for an explicit Revert.
//   - Clean-copy resync: a non-dirty copy whose live values drifted (Undo of
//     an earlier edit while this entity stayed selected) is resynced to live,
//     so the next edit-and-Apply cannot overwrite fields Undo restored.
//   - Dirty/live conflict: a dirty copy whose live state moved under it
//     (Undo/Redo of another command) raises a conflict flag. Policy is
//     explicit: Apply stays disabled until Revert (which reseeds from live).
//     The user's unapplied edits are never silently discarded AND never
//     applied over restored history state.
//   - Document reset clears target, copies, seeds, dirty, and conflict flags,
//     so a same-UUID replacement document cannot inherit values.
//   - Collision path changes clear the stale asset ID (ID-first resolution
//     would otherwise conflict with the new file); the W5 migration/repair
//     flow reassigns identity later. Unchanged paths preserve the ID.
//
// CPU-only: plain data + equality, no ImGui/Walnut/Vulkan/Bullet/entt.
// ============================================================================

struct PhysicsInspectorWork
{
    rt2::core::UUID target{};
    std::optional<PhysicsBodyComponent> body;
    std::optional<PhysicsShapeComponent> shape;
    std::optional<PhysicsHingeComponent> hinge;
    std::optional<PhysicsSliderComponent> slider;
    std::optional<PhysicsBodyComponent> seedBody;
    std::optional<PhysicsShapeComponent> seedShape;
    std::optional<PhysicsHingeComponent> seedHinge;
    std::optional<PhysicsSliderComponent> seedSlider;
    bool bodyDirty = false;
    bool shapeDirty = false;
    bool hingeDirty = false;
    bool sliderDirty = false;
    bool bodyConflict = false;
    bool shapeConflict = false;
    bool hingeConflict = false;
    bool sliderConflict = false;

    bool HasTarget() const { return !target.IsNull(); }

    // Reconcile working state with the live document for the selected target.
    // Live presence is derived from the optionals (nullopt = absent).
    // The T5 hinge/slider sides ride the exact T4 body/shape policy.
    void Sync(const rt2::core::UUID& selected,
              const std::optional<PhysicsBodyComponent>& liveBody,
              const std::optional<PhysicsShapeComponent>& liveShape,
              const std::optional<PhysicsHingeComponent>& liveHinge =
                  std::nullopt,
              const std::optional<PhysicsSliderComponent>& liveSlider =
                  std::nullopt)
    {
        if (target != selected)
        {
            target = selected;
            Reseed(liveBody, liveShape, liveHinge, liveSlider);
            return;
        }
        SyncSide(body, seedBody, liveBody, bodyDirty, bodyConflict);
        SyncSide(shape, seedShape, liveShape, shapeDirty, shapeConflict);
        SyncSide(hinge, seedHinge, liveHinge, hingeDirty, hingeConflict);
        SyncSide(slider, seedSlider, liveSlider, sliderDirty, sliderConflict);
    }

    // Applying one component must not discard pending edits to the other.
    void AppliedBody(const std::optional<PhysicsBodyComponent>& liveBody)
    {
        body = seedBody = liveBody;
        bodyDirty = false;
        bodyConflict = false;
    }
    void AppliedShape(const std::optional<PhysicsShapeComponent>& liveShape)
    {
        shape = seedShape = liveShape;
        shapeDirty = false;
        shapeConflict = false;
    }

    // Atomic pair apply: both sides advance to current live state together,
    // so solid <-> trigger conversions never strand a half-applied pair.
    // Hinge/slider working state is preserved (their own Apply commits it).
    void AppliedPair(const std::optional<PhysicsBodyComponent>& liveBody,
                     const std::optional<PhysicsShapeComponent>& liveShape)
    {
        body = seedBody = liveBody;
        shape = seedShape = liveShape;
        bodyDirty = shapeDirty = false;
        bodyConflict = shapeConflict = false;
    }
    void AppliedHinge(const std::optional<PhysicsHingeComponent>& liveHinge)
    {
        hinge = seedHinge = liveHinge;
        hingeDirty = false;
        hingeConflict = false;
    }
    void AppliedSlider(const std::optional<PhysicsSliderComponent>& liveSlider)
    {
        slider = seedSlider = liveSlider;
        sliderDirty = false;
        sliderConflict = false;
    }

    // Single-side revert (the other sides' working state is preserved).
    void RevertBody(const std::optional<PhysicsBodyComponent>& liveBody)
    {
        body = seedBody = liveBody;
        bodyDirty = false;
        bodyConflict = false;
    }
    void RevertShape(const std::optional<PhysicsShapeComponent>& liveShape)
    {
        shape = seedShape = liveShape;
        shapeDirty = false;
        shapeConflict = false;
    }
    void RevertHinge(const std::optional<PhysicsHingeComponent>& liveHinge)
    {
        hinge = seedHinge = liveHinge;
        hingeDirty = false;
        hingeConflict = false;
    }
    void RevertSlider(const std::optional<PhysicsSliderComponent>& liveSlider)
    {
        slider = seedSlider = liveSlider;
        sliderDirty = false;
        sliderConflict = false;
    }

    // Document reset (including same-UUID replacement): drop everything.
    void Clear()
    {
        target = rt2::core::UUID{};
        body.reset();
        shape.reset();
        hinge.reset();
        slider.reset();
        seedBody.reset();
        seedShape.reset();
        seedHinge.reset();
        seedSlider.reset();
        bodyDirty = shapeDirty = hingeDirty = sliderDirty = false;
        bodyConflict = shapeConflict = hingeConflict = sliderConflict = false;
    }

    // Collision-ref path edit: assign the new path (Model kind unless empty)
    // and clear any prior asset ID, which named the previous file. Returns
    // true when the path actually changed.
    static bool NoteCollisionPathChanged(AssetReference& ref,
                                         const std::string& newPath)
    {
        if (ref.path == newPath)
            return false;
        ref.path = newPath;
        ref.kind = newPath.empty() ? AssetKind::Unknown : AssetKind::Model;
        ref.assetId = rt2::core::UUID{};
        return true;
    }

private:
    template <typename T>
    static void SyncSide(std::optional<T>& work, std::optional<T>& seed,
                         const std::optional<T>& live, bool& dirty,
                         bool& conflict)
    {
        if (work.has_value() != live.has_value())
        {
            // Preserve a dirty edit when Undo/Redo removes its component.
            // The UI keeps rendering the working copy until explicit Revert.
            if (dirty)
            {
                conflict = true;
                return;
            }
            work = live;
            seed = live;
            conflict = false;
            return;
        }
        if (!dirty)
        {
            // Clean copy follows live values (Undo/Redo of value edits).
            if (!(work == live))
                work = live;
            seed = live;
            conflict = false;
            return;
        }
        // Dirty copy: conflict iff live moved away from the seed. The copy
        // is kept (nothing is silently discarded); Apply must stay disabled
        // until Revert reseeds. A dirty copy edited back to live values is
        // not dirty at all (return-to-start needs no history entry).
        if (work == live && live == seed)
        {
            dirty = false;
            conflict = false;
            return;
        }
        conflict = !(live == seed);
    }

    void Reseed(const std::optional<PhysicsBodyComponent>& liveBody,
                const std::optional<PhysicsShapeComponent>& liveShape,
                const std::optional<PhysicsHingeComponent>& liveHinge,
                const std::optional<PhysicsSliderComponent>& liveSlider)
    {
        body = seedBody = liveBody;
        shape = seedShape = liveShape;
        hinge = seedHinge = liveHinge;
        slider = seedSlider = liveSlider;
        bodyDirty = shapeDirty = hingeDirty = sliderDirty = false;
        bodyConflict = shapeConflict = hingeConflict = sliderConflict = false;
    }
};

#endif // RT2_PHYSICS_INSPECTOR_STATE_H
