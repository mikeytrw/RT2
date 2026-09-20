#pragma once

#ifndef RT2_PHYSICS_WORLD_H
#define RT2_PHYSICS_WORLD_H

#include "core/Error.h"
#include "core/UUID.h"
#include "PhysicsComponents.h"

#include <btBulletDynamicsCommon.h>
#include <BulletCollision/CollisionDispatch/btGhostObject.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <entt/entt.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
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
// T4 (this file, second half): rigid-body construction from the authored
// components plus the per-kind transform authority and the fixed-tick sync
// hooks. Build order is statics, then kinematics, then dynamics, then ghosts
// (triggers), UUID order within each group; constraints (T5) build last.
// Units: 1 RT2 unit = 1 metre, Y-up gravity, kilograms, radians internally;
// dynamic inertia comes from calculateLocalInertia after the uniform scale is
// applied; margins and CCD thresholds are validated in final world units
// post-scale. One shape per entity; uniform-positive world scale only;
// dynamic/kinematic triangle mesh is refused (Bullet restriction).
//
// CPU-only: this header pulls Bullet core headers only (proven Vulkan/ImGui/
// Walnut-free by the T1 RED_NoVulkanInPhysicsIncludes gate). It links into
// RT2Tests and RT2SliceRunner through the shared CPU source closure.
// ============================================================================

namespace rt2::core {

class SceneDocument;
class IPhysicsCollisionAssetProvider;

// One runtime body: the Authoring-UUID-keyed companion record (plan section
// 2). Bullet pointers are Scene-context handles owned by the vectors below;
// the ECS components stay plain authored data and never cross into Authoring.
struct PhysicsBodyRecord
{
    UUID id;
    entt::entity entity = entt::null;
    PhysicsBodyKind kind = PhysicsBodyKind::Static;
    bool isTrigger = false;
    // Non-owning views into m_Shapes/m_Bodies/m_Ghosts (body XOR ghost set).
    btCollisionShape* shape = nullptr;
    btRigidBody* body = nullptr;
    btDefaultMotionState* motion = nullptr;
    btPairCachingGhostObject* ghost = nullptr;
    // Non-owning view into m_TriangleMeshes (set only for StaticTriMesh).
    btStridingMeshInterface* triangleMesh = nullptr;
    // Uniform world scale baked at build (single scale owner).
    float bakedScale = 1.0f;
};

// One runtime constraint: the body-to-constraint dependency index entry (T5).
// Owner is the Authoring UUID of the entity carrying the hinge/slider
// component; other is its otherBody (nil = world anchor). The Bullet pointer
// is a non-owning view into m_Constraints; hinge XOR slider is set.
struct PhysicsConstraintRecord
{
    UUID ownerId;
    UUID otherId; // nil = world/static frame anchor
    bool isHinge = true;
    btTypedConstraint* constraint = nullptr;
    btHingeConstraint* hinge = nullptr;
    btSliderConstraint* slider = nullptr;
    // Staged drive parameters (authored values, validated at build): hinge
    // uses driveSpeed = target velocity and driveMax = max impulse with
    // restPosition = rest angle; slider uses driveSpeed = target-velocity
    // cap, driveMax = max force, restPosition = target position.
    float driveSpeed = 0.0f;
    float driveMax = 0.0f;
    float restPosition = 0.0f;
    float lowerLimit = 0.0f;
    float upperLimit = 0.0f;
    // Staged normalized owner-local axis (slider release-impulse direction).
    glm::vec3 localAxis = {1.0f, 0.0f, 0.0f};
    // Hinge return-in-progress (ReturnHingeToRest): the pre-step servo
    // drives toward restPosition until it disengages inside half a degree.
    // Any explicit SetHingeDrive/ReleaseHingeDrive clears it.
    bool returning = false;
};

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

    // T4 candidate construction with collision geometry: bodies, ghosts, and
    // shapes are staged from the runtime clone through the borrowed provider
    // (host-owned; must outlive the call — the controller guarantees the
    // Play-session borrow). Null provider means no collision refs may exist
    // (ValidatePhysicsForPlay already refused that combination); any body
    // carrying an active hull/triMesh ref with a null provider fails here
    // with a UUID-named Error. Every other failure (missing/malformed/
    // oversize geometry, bad units, forbidden dynamic tri mesh) is likewise
    // a typed UUID-named Error and the candidate rolls back completely.
    static Result<std::unique_ptr<PhysicsWorld>> Create(
        const SceneDocument& runtime,
        const IPhysicsCollisionAssetProvider* provider);

    ~PhysicsWorld();

    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    // Advance the simulation by exactly one Bullet step. The outer RT2 fixed-
    // step accumulator (kFixedDt, kMaxFrameTime clamp, kMaxSubsteps cap) is
    // the only substepper: callers pass kFixedDt once per RT2 fixed tick and
    // this performs stepSimulation(dt, 0) — never an inner substep loop.
    void Step(float dt);

    // T4 fixed-tick authority sync (called by the controller around Step;
    // see RuntimeSceneController::RunFixedTick):
    //   - PreStepSync: refresh world transforms, then push every Kinematic
    //     body and kinematic ghost pose (ECS/script -> Bullet). Static and
    //     Dynamic bodies are untouched. T5 drive servo rides the same hook:
    //     powered slider targets and in-progress hinge returns are
    //     re-servoed toward their setpoints before the step.
    //   - PostStepSync: write every simulated Dynamic body pose back
    //     (Bullet -> ECS local TRS, marked dirty for the controller's single
    //     batched SceneGraph + TransformSync pass). Static, Kinematic, and
    //     ghost poses are untouched.
    void PreStepSync(SceneDocument& runtime);
    void PostStepSync(SceneDocument& runtime);

    // Test/preset helper: linear velocity write for simulated rigid bodies.
    // Returns false (mutating nothing) for Static bodies, ghosts, and unknown
    // UUIDs. Velocity (like impulse) is a legal Dynamic control — only pose
    // writes are authority-gated — so Dynamic accepts and Static refuses.
    // (The future T7 reset_body_pose arrives as a distinct explicit API; no
    // generic C++ pose-write path exists, so Lua can only move bodies through
    // the kind-gated RuntimeCommandSink.)
    bool SetBodyLinearVelocity(const UUID& id, const glm::vec3& velocity);

    // Bullet-side world position for tests (kinematic-push and CCD proofs).
    // Returns false for unknown UUIDs.
    bool BodyWorldPosition(const UUID& id, glm::vec3& out) const;

    // Spike-proven fast-sphere CCD calibration preset (plan section 3):
    // threshold = 0.5 * radius, swept radius = 0.8 * radius, in world units.
    // Explicitly authored per body — never inferred from a gameplay role.
    static void FastSphereCcdPreset(float radius, float& thresholdOut,
                                    float& sweptOut);

    // Companion-map lookup for tests and T5+ consumers. Null when absent.
    const PhysicsBodyRecord* FindBody(const UUID& id) const;
    // Bullet shape staged for a body (null when absent). Test seam for
    // margin/CCD/inertia proofs at the Bullet boundary.
    const btCollisionShape* FindBodyShape(const UUID& id) const;
    size_t BodyRecordCount() const { return m_BodyIndex.size(); }

    // ---- T5 driven-constraint surface ------------------------------------
    //
    // Constraints are built after all bodies/ghosts in stable owner-UUID
    // order (StageConstraints, called by Create after StageBodies). Frames
    // are resolved once at build from the authored owner-local pivots/axes
    // and the other-local (or world, when otherBody is nil) pivots/axes;
    // Bullet poses are never teleported to satisfy a constraint.
    //
    // Drive commands mutate the live Bullet constraint immediately, so the
    // very next fixed tick (at most one tick of latency) moves the bodies.
    // All setters return false (mutating nothing) for unknown owners, wrong
    // constraint kinds, and out-of-range input. No Lua/event/debug coupling:
    // T7 binds these same C++ entry points later.

    // Dependency index lookup. Null when the owner carries no constraint.
    const PhysicsConstraintRecord* FindConstraint(const UUID& ownerId) const;
    size_t ConstraintRecordCount() const { return m_ConstraintIndex.size(); }
    // Every constraint owner UUID whose constraint references bodyId as
    // owner or as otherBody, in stable UUID order. The body-to-constraint
    // dependency index the safe-point drain consults before tearing down.
    std::vector<UUID> ConstraintsForBody(const UUID& bodyId) const;

    // Hinge drive: enable the angular-velocity motor immediately with the
    // given target velocity (rad/s) and max impulse. Returns false for
    // unknown/non-hinge owners and non-finite/negative-impulse input.
    bool SetHingeDrive(const UUID& owner, float velocity, float maxImpulse);
    // Hinge release: disable the motor; the arm swings freely within its
    // limits. No pose is teleported.
    bool ReleaseHingeDrive(const UUID& owner);
    // Hinge return: drive toward the authored rest angle (within limits)
    // using the staged motor impulse cap, without teleporting.
    bool ReturnHingeToRest(const UUID& owner);
    // Live hinge angle in radians (getHingeAngle). ok=false when unknown or
    // not a hinge. Non-const: the underlying Bullet query is non-const.
    float HingeAngle(const UUID& owner, bool& ok);
    // Live hinge motor state (enabled flag + staged target velocity and max
    // impulse). ok=false when unknown or not a hinge.
    bool HingeMotorEnabled(const UUID& owner, bool& ok);
    bool HingeMotorParams(const UUID& owner, float& velocityOut,
                          float& maxImpulseOut);

    // Slider drive: power the linear motor toward target (world units along
    // the axis) with a proportional speed law — larger displacements map
    // monotonically to higher speeds, capped by the staged target velocity
    // and force. Targets outside [lower,upper] are refused (false), so the
    // slider never moves outside its limits through this entry point.
    bool SetSliderTarget(const UUID& owner, float target);
    // Slider release: cut the motor and apply an impulse along the axis to
    // the owner body. The limits still bind afterwards.
    bool ReleaseSlider(const UUID& owner, float impulse);
    // Live slider linear position (getLinearPos). ok=false when unknown or
    // not a slider.
    float SliderPosition(const UUID& owner, bool& ok) const;

    // Safe-point teardown participation (called by the controller drain at
    // each destroy position, after OnEntitiesDestroying while ECS and Bullet
    // state are still present):
    //   1. RemoveSubtreePhysics removes every live constraint whose owner
    //      or otherBody lies in uuids FIRST, then every live body/ghost in
    //      uuids. Constraints always die before referenced bodies.
    //   2. RemoveRuntimeBody removes one body/ghost, but loudly refuses
    //      (false + err) while a live constraint still references it — the
    //      orphan guard that turns a reversed teardown order red.
    void RemoveSubtreePhysics(const std::vector<UUID>& uuids);
    bool RemoveRuntimeBody(const UUID& bodyId, Error& err);

    // Atomically rebuild every constraint touching bodyId (as owner or
    // otherBody) from the current runtime document: replacements are fully
    // staged and validated before any live constraint is removed, so a
    // failure leaves the previous set untouched. Returns false with a
    // UUID-named Error on failure.
    bool RebuildConstraintsForBody(SceneDocument& runtime, const UUID& bodyId,
                                   Error& err);

    // Test-only teardown-order log. While enabled, every constraint/body
    // removal records "constraint" or "body" in removal order, so the
    // lifetime test proves constraints-first teardown (reversing the order
    // turns it red). Disabled by default (zero production effect); tests
    // enable, read, then disable and clear. Never set outside tests.
    static void SetTeardownOrderLog(bool enabled);
    static std::vector<std::string> TakeTeardownOrderLog();
    static void ClearTeardownOrderLog();

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

    // Test-only allocation-failure injection for the staging boundary.
    // When set, the next StageBodies throws std::bad_alloc so tests prove
    // the Create boundary translates it into a typed Error with a rolled
    // back candidate. Default off; tests must clear it after use. Never set
    // outside tests.
    static void SetStagingTestThrow(bool fail);
    static bool StagingTestThrow();

    // Test-only allocation-failure injection for the rebuild collection /
    // reserve boundary (T5 fixup re-review P1). Collect fires before
    // ConstraintsForBody materializes its vector; Reserve fires just before
    // replacements.reserve. Both must translate to the typed Io Error with
    // the live set untouched, exactly like a Build-path exhaustion. Default
    // None; tests must clear after use. Never set outside tests.
    enum class RebuildAllocThrowPhase : uint8_t
    {
        None = 0,
        Collect = 1,
        Reserve = 2,
    };
    static void SetRebuildAllocThrowPhase(RebuildAllocThrowPhase phase);
    static RebuildAllocThrowPhase GetRebuildAllocThrowPhase();

    // Test-only allocation-failure injection for the whole candidate
    // construction sequence (narrow follow-up). Explicit phases replace the
    // earlier shared boolean, whose single flag could never reach the
    // post-new point: BeforeNew fires before the PhysicsWorld allocation
    // (pre-candidate early-out), AfterNew fires right after it (proves the
    // constructed local still tears down). A separate factory-escape flag
    // fires before the translation boundary so the throw escapes Create and
    // exercises the Play handoff fallback instead. All default off; tests
    // must clear after use. Never set outside tests.
    enum class CandidateThrowPoint : uint8_t
    {
        None = 0,
        BeforeNew = 1,
        AfterNew = 2,
    };
    static void SetCandidateThrowPoint(CandidateThrowPoint point);
    static CandidateThrowPoint GetCandidateThrowPoint();
    static void SetEscapeTestThrow(bool fail);
    static bool EscapeTestThrow();

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

    // T4 candidate staging (defined in PhysicsWorld.cpp): ordered body/ghost
    // construction from the runtime clone. On failure fills err with a
    // UUID-named typed Error and returns false; the caller rolls the
    // half-staged candidate back wholesale.
    static bool StageBodies(PhysicsWorld& world, const SceneDocument& runtime,
                            const IPhysicsCollisionAssetProvider* provider,
                            Error& err);
    static bool StageOneBody(PhysicsWorld& world,
                             const entt::registry& registry, const UUID& uuid,
                             entt::entity entity,
                             const IPhysicsCollisionAssetProvider* provider,
                             Error& err);
    // T5 candidate staging: every authored hinge/slider from the runtime
    // clone, stable owner-UUID order across both kinds, after all bodies and
    // ghosts. Frames resolve once at build; any failure fills err with a
    // UUID-named typed Error and the caller rolls the candidate back.
    static bool StageConstraints(PhysicsWorld& world,
                                 const SceneDocument& runtime, Error& err);
    static bool StageOneHinge(PhysicsWorld& world,
                              const SceneDocument& runtime, const UUID& owner,
                              entt::entity entity, Error& err);
    static bool StageOneSlider(PhysicsWorld& world,
                               const SceneDocument& runtime,
                               const UUID& owner, entt::entity entity,
                               Error& err);
    // T5 review fixup F2: failure-atomic rebuild support. Build validates the
    // component, resolves frames, and allocates the Bullet constraint WITHOUT
    // touching the dynamics world or either index, so a late frame/scale/
    // world-anchor/allocation failure leaves the live set untouched. Commit
    // adds one built constraint to the world and both indices (plus the
    // staged motor-enabled deactivation policy) and cannot fail.
    // StageOneHinge/Slider are Build+Commit; RebuildConstraintsForBody Builds
    // every replacement first, then removes the live set, then Commits.
    struct BuiltHingeConstraint
    {
        std::unique_ptr<btHingeConstraint> owned;
        PhysicsConstraintRecord rec;
    };
    struct BuiltSliderConstraint
    {
        std::unique_ptr<btSliderConstraint> owned;
        PhysicsConstraintRecord rec;
    };
    static bool BuildHinge(PhysicsWorld& world,
                           const SceneDocument& runtime, const UUID& owner,
                           entt::entity entity, Error& err,
                           BuiltHingeConstraint& out);
    static bool BuildSlider(PhysicsWorld& world,
                            const SceneDocument& runtime, const UUID& owner,
                            entt::entity entity, Error& err,
                            BuiltSliderConstraint& out);
    static void CommitBuiltHinge(PhysicsWorld& world,
                                 BuiltHingeConstraint& built);
    static void CommitBuiltSlider(PhysicsWorld& world,
                                  BuiltSliderConstraint& built);
    // T5 review fixup F4: endpoint wake policy shared by the live drive
    // entry points. Enabling a motor (drive/return/target) activates both
    // endpoints and pins DISABLE_DEACTIVATION, matching staged enabled
    // motors; releasing restores ACTIVE_TAG on an endpoint no other enabled
    // constraint touches, so it may sleep again.
    void WakeConstraintEndpoints(const UUID& owner);
    void RelaxConstraintEndpoints(const UUID& owner);
    bool ConstraintMotorEnabled(const PhysicsConstraintRecord& rec) const;

    // Bullet teardown order: constraints first, then ghosts/bodies, then
    // shapes, then the world itself (Bullet requirement; plan section 3).
    void Shutdown();

    static bool s_TestInjectCreateFailure;
    static bool s_TestPoseProbe;
    static bool s_StagingTestThrow;
    static RebuildAllocThrowPhase s_RebuildAllocThrowPhase;
    static bool s_TeardownOrderLog;
    static std::vector<std::string> s_TeardownOrder;
    static CandidateThrowPoint s_CandidateThrowPoint;
    static bool s_EscapeTestThrow;
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
    std::vector<std::unique_ptr<btStridingMeshInterface>> m_TriangleMeshes;
    std::vector<std::unique_ptr<btDefaultMotionState>> m_MotionStates;
    std::vector<std::unique_ptr<btRigidBody>> m_Bodies;
    std::vector<std::unique_ptr<btPairCachingGhostObject>> m_Ghosts;
    std::vector<std::unique_ptr<btTypedConstraint>> m_Constraints;
    std::vector<PhysicsConstraintRecord> m_ConstraintIndex;
    // UUID-ordered companion map: Authoring UUID -> runtime entity -> Bullet
    // handle. Constraint build (T5) and event scrape (T6) read this; the
    // ECS components never hold Bullet pointers.
    std::vector<PhysicsBodyRecord> m_BodyIndex;

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
