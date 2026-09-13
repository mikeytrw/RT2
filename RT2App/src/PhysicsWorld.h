#pragma once

#ifndef RT2_PHYSICS_WORLD_H
#define RT2_PHYSICS_WORLD_H

#include "core/Error.h"
#include "core/UUID.h"

#include <btBulletDynamicsCommon.h>
#include <BulletCollision/CollisionDispatch/btGhostObject.h>

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
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
//   - the live-world census (LiveWorldCount) that proves a committed world is
//     constructed and destroyed rather than merely pointer-discarded,
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
    // A runtime body pose observed at candidate-construction time. Recorded
    // only while the test pose probe is enabled; the permanent ordering test
    // uses it to prove InitPrevTransforms ran before the candidate observed
    // the runtime clone (a reorder of the two is visible here).
    struct ConstructionPose
    {
        UUID id;
        glm::vec3 translation = {0.0f, 0.0f, 0.0f};
    };

    // Construct a complete private candidate world (broadphase, dispatcher,
    // solver, configuration, dynamics world, ghost-pair callback, gravity)
    // against the freshly cloned runtime document whose world transforms the
    // controller has already refreshed. Returns a typed Error on failure and
    // never a half-built world: the candidate dies with the local, running
    // the full Bullet teardown. The test-injected failure
    // (SetTestInjectCreateFailure) fires AFTER that construction so the
    // permanent atomicity test observes a real candidate being built and
    // rolled back — without needing a late body failure that only T4 can
    // produce naturally. Takes the runtime document (rather than nothing) so
    // T4 stages bodies from the same seam.
    static Result<std::unique_ptr<PhysicsWorld>> Create(
        const SceneDocument& runtime);

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

    // Live constructed-but-not-yet-destroyed worlds. Production-meaningful
    // leak baseline: failed Play and Stop must restore the pre-Play count,
    // and a world that is merely pointer-discarded (never destroyed) keeps
    // the count elevated where TotalHandles() on a null pointer reads zero.
    static size_t LiveWorldCount();

    // Test-only late-failure injection for RED_PhysicsPlayConstructionIsAtomic.
    // When set, the next Create() builds the full candidate world, records
    // the construction pose probe, then returns a typed failure instead of
    // committing — so rollback destroys a live Bullet world. Default off;
    // tests must clear it after use. Never set outside tests.
    static void SetTestInjectCreateFailure(bool fail);
    static bool TestInjectCreateFailure();

    // Test-only destruction probe, invoked by the REAL ~PhysicsWorld()
    // when set (after Bullet teardown, before the live count decrements).
    // Lets the Stop-order test observe the actual destruction boundary:
    // the probe runs while Stop() still holds the runtime clone iff the
    // world is destroyed before the clone reset. Null by default (zero
    // production effect); tests set, read, then clear. Never set outside
    // tests.
    static void SetTestDestroyProbe(std::function<void()> probe);

    // Test-only construction pose probe. While enabled, Create() records one
    // ConstructionPose per runtime physics body (UUID order) at the moment
    // the candidate observes the runtime clone. Disabled by default (zero
    // production cost); tests enable, read, then clear and disable.
    static void SetTestPoseProbe(bool enabled);
    static std::vector<ConstructionPose> TestRecordedPoses();
    static void ClearTestRecordedPoses();

private:
    PhysicsWorld();

    // Bullet teardown order: constraints first, then ghosts/bodies, then
    // shapes, then the world itself (Bullet requirement; plan section 3).
    void Shutdown();

    static bool s_TestInjectCreateFailure;
    static bool s_TestPoseProbe;
    static size_t s_LiveWorlds;
    static std::vector<ConstructionPose> s_RecordedPoses;
    static std::function<void()> s_TestDestroyProbe;

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
// ticket "early invariants", review-fixup settled policy). Runs on the
// authoring document BEFORE CloneInMemory; refusals are typed Errors.
// Checks, in order:
//   - pass 0: every PhysicsBody/Shape/Hinge/Slider component must sit on an
//     entity carrying EntityIdComponent (missing or null IDs refuse with
//     Error::InvalidEntity naming the component and registry entity —
//     iterated WITHOUT pre-filtering on identity so malformed entities
//     cannot evade validation and vanish from the clone),
//   - hinge/slider identity via ValidatePhysicsConstraintReferences (dangling
//     or body-less otherBody, self-constraint, missing owner body),
//   - hinge/slider owner must be Dynamic or Kinematic (never Static),
//   - hinge/slider axes must be finite and non-zero (singular frames refused;
//     axes are normalized later at T5 build, never silently),
//   - per body (UUID order): MotionComponent clash, parented body, dynamic
//     triangle mesh, settled layer/mask/trigger policy (layer is exactly one
//     known bit; mask is a nonzero subset of known bits; trigger shapes
//     require the Trigger layer and non-trigger shapes reject it),
//     non-uniform/non-positive/non-finite scale (bodies are roots, so local
//     == world),
//   - active collision refs (ConvexHull hull / StaticTriMesh triMesh with a
//     non-empty path) require a collision provider; Play with refs and no
//     provider refuses loudly.
// Mass/inertia validation belongs to the T4 units contract and is not here.
bool ValidatePhysicsForPlay(const SceneDocument& doc,
                            const IPhysicsCollisionAssetProvider* provider,
                            Error& err);

} // namespace rt2::core

#endif // RT2_PHYSICS_WORLD_H
