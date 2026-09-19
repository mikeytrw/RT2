#pragma once

#ifndef RT2_PHYSICS_DEBUG_CAPTURE_H
#define RT2_PHYSICS_DEBUG_CAPTURE_H

#include "PhysicsDebugLines.h"

#include <btBulletDynamicsCommon.h>
#include <BulletCollision/CollisionDispatch/btGhostObject.h>

#include <cstddef>

// ============================================================================
// PhysicsDebugCapture — minimal btIDebugDraw capture (Bullet T8, CPU-only).
//
// The drawer below is the only btIDebugDraw in the tree: it captures
// drawLine calls into a PhysicsDebugLines DTO with per-object ownership set
// by the caller before each debugDrawObject. Bullet's debugDrawWorld is NOT
// used directly because it carries no owner identity — attribution would be
// lost. PhysicsWorld::CaptureDebugLines iterates its UUID-ordered companion
// map, sets (owner, kind) per body/ghost, and calls debugDrawObject per
// shape, so every segment names its owning body.
//
// Constraint handling (T5 independence): T5 hinge/slider Bullet constraints
// are built on another branch and must not be duplicated here. The
// constraint-debug path therefore consumes the ALREADY-PERSISTED hinge/slider
// components (PhysicsComponents.h, authored in T2, validated in T3) through
// AppendConstraintAdapterLines — a mergeable adapter that draws deterministic
// axis/pivot segments from authored frames + runtime world positions. Final
// T5 integration point (documented for the merge): when PhysicsWorld owns
// real btTypedConstraints, extend PhysicsWorld::CaptureDebugLines to set the
// constraint owner context and call debugDrawConstraint (DBG_DrawConstraints)
// per constraint in UUID order, then keep or retire the adapter as the
// world-anchor fallback. Current tests use synthetic/persisted hinge/slider
// fixtures through this adapter; no T5 build/teardown order is touched.
//
// CPU-only: Bullet + glm + UUID only. No Vulkan/ImGui/Walnut. Safe for
// RT2Tests/RT2SliceRunner. Drawing lives in PhysicsDebugOverlay (RT2App-only).
// ISceneRenderBridge carries no debug geometry (plan section 2).
// ============================================================================

namespace rt2::core {

class SceneDocument;

// Minimal capture drawer: drawLine appends one owned segment; every other
// virtual is a deterministic no-op (contacts/text/AABB route through
// drawLine only when the caller enables those modes — T8 enables wireframe
// only). The caller sets ownership per object; segments captured with a null
// owner are dropped rather than misattributed (loud-misattribution is worse
// than a missing line; empty capture is itself a test-visible signal).
class PhysicsDebugDrawer final : public btIDebugDraw
{
public:
    PhysicsDebugDrawer() = default;

    void SetOwner(const UUID& owner, PhysicsDebugLineKind kind)
    {
        m_HasOwner = !owner.IsNull();
        m_Owner = owner;
        m_Kind = kind;
    }
    void ClearOwner() { m_HasOwner = false; }

    void BeginCapture(PhysicsDebugLines* out)
    {
        m_Out = out;
        ClearOwner();
    }

    // Test-only deterministic allocation-failure seam. The next accepted
    // drawLine throws before appending; CaptureDebugLines translates it to a
    // typed Error and restores Bullet's previous drawer through RAII.
    static void SetTestThrowOnNextLine(bool enabled);
    static bool TestThrowOnNextLine();

    // btIDebugDraw interface.
    void drawLine(const btVector3& from, const btVector3& to,
                  const btVector3& color) override;
    void drawContactPoint(const btVector3& /*point*/,
                          const btVector3& /*normal*/, btScalar /*dist*/,
                          int /*lifeTime*/, const btVector3& /*color*/) override {}
    void reportErrorWarning(const char* /*warning*/) override {}
    void draw3dText(const btVector3& /*loc*/, const char* /*text*/) override {}
    void setDebugMode(int mode) override { m_DebugMode = mode; }
    int getDebugMode() const override { return m_DebugMode; }

private:
    PhysicsDebugLines* m_Out = nullptr;
    UUID m_Owner;
    PhysicsDebugLineKind m_Kind = PhysicsDebugLineKind::Static;
    bool m_HasOwner = false;
    int m_DebugMode = btIDebugDraw::DBG_DrawWireframe;
    static bool s_TestThrowOnNextLine;
};

// Adapter over already-persisted hinge/slider components: appends one
// Constraint-kind segment group per hinge/slider in owner-UUID order. Reads
// the runtime document only (never Authoring); never builds, steps, or tears
// down Bullet constraints. Deterministic: UUID order, fixed axis length,
// guarded axis normalization (degenerate axes still emit a pivot cross so a
// present constraint is never silently invisible).
void AppendConstraintAdapterLines(const SceneDocument& runtime,
                                  PhysicsDebugLines& out);

} // namespace rt2::core

#endif // RT2_PHYSICS_DEBUG_CAPTURE_H
