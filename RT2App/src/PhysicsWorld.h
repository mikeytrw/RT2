#pragma once

#ifndef RT2_PHYSICS_WORLD_H
#define RT2_PHYSICS_WORLD_H

#include "core/Error.h"
#include "core/UUID.h"

#include <btBulletDynamicsCommon.h>
#include <BulletCollision/CollisionDispatch/btGhostObject.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// ============================================================================
// PhysicsWorld — RT2-owned Bullet rigid-body world (Bullet integration, T3
// world lifecycle).
//
// Ownership (plan section 2): RuntimeSceneController owns at most one
// committed PhysicsWorld for the lifetime of one Play session. The world is
// staged privately as a local candidate (PhysicsWorld::Create) and committed
// to controller ownership only after complete construction; it is destroyed
// before m_Runtime.reset() on Stop and on any candidate-construction failure.
// Never touches the authoring document; never survives Stop; never global.
//
// T3 scope: an empty simulated world. No body creation, no collision-asset
// decoding, no constraints, no events, no Lua, no debug drawing — those are
// T4+. What T3 owns:
//   - one btDiscreteDynamicsWorld with the plan gravity (0,-9.81,0),
//   - the outer-RT2-accumulator-as-sole-substepper contract: Step(kFixedDt)
//     performs exactly one Bullet step via stepSimulation(dt, 0),
//   - the handle census (bodies/constraints/shapes/ghosts) that Stop and the
//     leak assertion observe; all zero in T3, carried by real storage so T4
//     fills the same counters,
//   - Play-time validation of the early invariants (ValidatePhysicsForPlay),
//   - ordered teardown (constraints, then ghosts/bodies, then shapes, then
//     the world) that T4 bodies will ride along.
//
// CPU-only: this header pulls Bullet core headers only (proven Vulkan/ImGui/
// Walnut-free by the T1 RED_NoVulkanInPhysicsIncludes gate). It links into
// RT2Tests and RT2SliceRunner through the shared CPU source closure.
// ============================================================================

namespace rt2::core {

class SceneDocument;
class IPhysicsCollisionAssetProvider;

class PhysicsWorld final
{
public:
    // Construct a complete private candidate world (broadphase, dispatcher,
    // solver, configuration, dynamics world, ghost-pair callback, gravity).
    // Returns a typed Error on failure and never a half-built world. The
    // test-injected failure (SetTestInjectCreateFailure) exercises the
    // controller's candidate-commit rollback without needing a late body
    // failure that only T4 can produce naturally.
    static Result<std::unique_ptr<PhysicsWorld>> Create();

    ~PhysicsWorld();

    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    // Advance the simulation by exactly one Bullet step. The outer RT2 fixed-
    // step accumulator (kFixedDt, kMaxFrameTime clamp, kMaxSubsteps cap) is
    // the only substepper: callers pass kFixedDt once per RT2 fixed tick and
    // this performs stepSimulation(dt, 0) — never an inner substep loop.
    void Step(float dt);

    // Handle census for tests and the Stop leak assertion. All zero in T3
    // (no bodies are created yet); T4 backfills the same storage.
    size_t BodyCount() const { return m_Bodies.size(); }
    size_t ConstraintCount() const { return m_Constraints.size(); }
    size_t ShapeCount() const { return m_Shapes.size(); }
    size_t GhostCount() const { return m_Ghosts.size(); }
    size_t TotalHandles() const
    {
        return m_Bodies.size() + m_Constraints.size() +
               m_Shapes.size() + m_Ghosts.size();
    }
    bool IsEmpty() const { return TotalHandles() == 0; }

    // Number of Step() calls served. Tests use this to prove Step-mode runs
    // exactly one kFixedDt tick and Update() honors the five-tick cap.
    uint64_t StepCount() const { return m_StepCount; }

    // Test-only late-failure injection for RED_PhysicsPlayConstructionIsAtomic.
    // When set, the next Create() returns a typed failure instead of a world.
    // Default off; tests must clear it after use. Never set outside tests.
    static void SetTestInjectCreateFailure(bool fail);
    static bool TestInjectCreateFailure();

private:
    PhysicsWorld();

    // Bullet teardown order: constraints first, then ghosts/bodies, then
    // shapes, then the world itself (Bullet requirement; plan section 3).
    void Shutdown();

    static bool s_TestInjectCreateFailure;

    btDefaultCollisionConfiguration m_CollisionConfiguration;
    btCollisionDispatcher m_Dispatcher;
    btDbvtBroadphase m_Broadphase;
    btSequentialImpulseConstraintSolver m_Solver;
    btGhostPairCallback m_GhostCallback;
    btDiscreteDynamicsWorld m_World;

    // T3: empty by construction. T4 owns bodies/shapes/constraints/ghosts
    // here so the census above and Shutdown() cover them without change.
    std::vector<std::unique_ptr<btCollisionShape>> m_Shapes;
    std::vector<std::unique_ptr<btDefaultMotionState>> m_MotionStates;
    std::vector<std::unique_ptr<btRigidBody>> m_Bodies;
    std::vector<std::unique_ptr<btPairCachingGhostObject>> m_Ghosts;
    std::vector<std::unique_ptr<btTypedConstraint>> m_Constraints;

    uint64_t m_StepCount = 0;
};

// T3 Play-time validation of the early invariants (plan sections 3-4,
// ticket "early invariants"). Runs on the authoring document BEFORE
// CloneInMemory; every refusal is a typed Error whose path is the offending
// entity's Authoring-context UUID string and whose detail names that UUID.
// Checks, in order:
//   - hinge/slider identity via ValidatePhysicsConstraintReferences (dangling
//     or body-less otherBody, self-constraint, missing owner body),
//   - hinge/slider owner must be Dynamic or Kinematic (never Static),
//   - hinge/slider axes must be finite and non-zero (singular frames refused;
//     axes are normalized later at T5 build, never silently),
//   - per body (UUID order): MotionComponent clash, parented body, dynamic
//     triangle mesh, zero/unknown layer or mask bits, non-uniform/
//     non-positive/non-finite scale (bodies are roots, so local == world),
//   - active collision refs (ConvexHull hull / StaticTriMesh triMesh with a
//     non-empty path) require a collision provider; Play with refs and no
//     provider refuses loudly.
// Mass/inertia validation belongs to the T4 units contract and is not here.
bool ValidatePhysicsForPlay(const SceneDocument& doc,
                            const IPhysicsCollisionAssetProvider* provider,
                            Error& err);

} // namespace rt2::core

#endif // RT2_PHYSICS_WORLD_H
