// ============================================================================
// PhysicsWorld — RT2-owned Bullet rigid-body world (T3 world lifecycle).
//
// T3 owns the empty world, the single-step contract, the handle census, the
// early Play-time invariants, and ordered teardown. Body/shape/constraint
// construction and collision-asset decoding arrive in T4 and backfill the
// storage and census declared in PhysicsWorld.h without changing this file's
// ownership or teardown order.
// ============================================================================

#include "PhysicsWorld.h"

#include "ECSComponents.h"
#include "EntityReferenceRemapper.h"
#include "IPhysicsCollisionAssetProvider.h"
#include "SceneDocument.h"
#include "SceneGraph.h"
#include "SceneHierarchy.h"

#include <algorithm>
#include <cassert>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <new>

#include <glm/gtc/quaternion.hpp>

namespace rt2::core {

// T5 drive constants (declared early: PreStepSync servos the drive setpoints
// every fixed tick, ahead of the staging section that also uses them).
namespace {
constexpr float kT5MinAxisLength = 1e-6f;
// Proportional speed gain (1/s) for the slider target law.
constexpr float kT5SliderTargetGain = 10.0f;
} // namespace

bool PhysicsWorld::s_TestInjectCreateFailure = false;
bool PhysicsWorld::s_TestPoseProbe = false;
bool PhysicsWorld::s_StagingTestThrow = false;
PhysicsWorld::RebuildAllocThrowPhase PhysicsWorld::s_RebuildAllocThrowPhase =
    PhysicsWorld::RebuildAllocThrowPhase::None;
bool PhysicsWorld::s_TeardownOrderLog = false;
std::vector<std::string> PhysicsWorld::s_TeardownOrder;
bool PhysicsWorld::s_ResetCollectTestThrow = false;
PhysicsWorld::CandidateThrowPoint PhysicsWorld::s_CandidateThrowPoint = PhysicsWorld::CandidateThrowPoint::None;
bool PhysicsWorld::s_EscapeTestThrow = false;
size_t PhysicsWorld::s_LiveWorlds = 0;
std::vector<PhysicsWorld::ConstructionPose> PhysicsWorld::s_RecordedPoses;
std::function<void()> PhysicsWorld::s_TestDestroyProbe;

PhysicsWorld::PhysicsWorld()
    : m_Dispatcher(&m_CollisionConfiguration)
    , m_World(&m_Dispatcher, &m_Broadphase, &m_Solver,
              &m_CollisionConfiguration)
{
    // Plan units: Y-up, 1 unit = 1 metre, world gravity (0,-9.81,0).
    m_World.setGravity(btVector3(0.0f, -9.81f, 0.0f));
    m_Broadphase.getOverlappingPairCache()->setInternalGhostPairCallback(
        &m_GhostCallback);
    ++s_LiveWorlds;
}

PhysicsWorld::~PhysicsWorld()
{
    Shutdown();
    // Destruction-boundary probe (test-only, null in production): observes
    // the actual teardown point. In Stop() the runtime clone is still alive
    // here iff the world is destroyed before the clone reset.
    if (s_TestDestroyProbe)
        s_TestDestroyProbe();
    --s_LiveWorlds;
}

void PhysicsWorld::Shutdown()
{
    // Bullet teardown order (plan section 3): constraints first, then
    // ghosts/bodies, then shapes, then the world itself. All four stores are
    // empty in T3; the loops are real so T4 bodies ride the same order. T5
    // constraints ride the first loop; reversing the constraint/body order
    // strands live Bullet constraints over removed bodies, which the
    // teardown-order lifetime test observes through the order log.
    for (auto it = m_Constraints.rbegin(); it != m_Constraints.rend(); ++it)
    {
        m_World.removeConstraint(it->get());
        if (s_TeardownOrderLog)
            s_TeardownOrder.emplace_back("constraint");
    }
    m_Constraints.clear();
    m_ConstraintIndex.clear();
    for (auto it = m_Ghosts.rbegin(); it != m_Ghosts.rend(); ++it)
    {
        m_World.removeCollisionObject(it->get());
        if (s_TeardownOrderLog)
            s_TeardownOrder.emplace_back("body");
    }
    m_Ghosts.clear();
    for (auto it = m_Bodies.rbegin(); it != m_Bodies.rend(); ++it)
    {
        m_World.removeCollisionObject(it->get());
        if (s_TeardownOrderLog)
            s_TeardownOrder.emplace_back("body");
    }
    m_Bodies.clear();
    m_MotionStates.clear();
    m_TriangleMeshes.clear();
    m_BodyIndex.clear();
    m_Shapes.clear();
    // T6: overlap history dies with the world. A world that survived Stop
    // would otherwise report a stale Exit on the next session's first tick;
    // Stop destroys the world, and ClearEventHistory covers the explicit
    // reset path before that.
    m_PrevOverlaps.clear();
}

Result<std::unique_ptr<PhysicsWorld>> PhysicsWorld::Create(
    const SceneDocument& runtime)
{
    return Create(runtime, nullptr);
}

Result<std::unique_ptr<PhysicsWorld>> PhysicsWorld::Create(
    const SceneDocument& runtime,
    const IPhysicsCollisionAssetProvider* provider)
{
    // Single allocation-translation boundary over the WHOLE candidate
    // construction sequence (narrow follow-up): the PhysicsWorld allocation
    // with Bullet member construction, test-pose preparation, and body
    // staging all run inside one try. Any resource exhaustion surfaces as a
    // typed Error::Io; a half-built candidate still dies with its local
    // through the complete Shutdown() teardown (the destructor runs exactly
    // when construction completed — never a pre-construction early-out, never
    // a leak). Error::Io is the resource-exhaustion code.
    //
    // The factory-escape hook fires BEFORE the boundary so the throw escapes
    // Create entirely: it proves the Play handoff fallback, not this
    // boundary. The phased hook below fires inside it at independently
    // reachable points.
    if (s_EscapeTestThrow)
        throw std::bad_alloc(); // factory escape (tests only, pre-boundary)
    try
    {
        if (s_CandidateThrowPoint == CandidateThrowPoint::BeforeNew)
            throw std::bad_alloc(); // pre-candidate injection (tests only)
        // The candidate is FULLY constructed first: broadphase, dispatcher,
        // solver, configuration, dynamics world, ghost-pair callback,
        // gravity. A failure below therefore rolls back a live Bullet world
        // through the complete Shutdown() teardown.
        std::unique_ptr<PhysicsWorld> world(new PhysicsWorld());
        if (s_CandidateThrowPoint == CandidateThrowPoint::AfterNew)
            throw std::bad_alloc(); // post-construction: teardown proof

    // Construction pose probe (test-only, gated): record what the candidate
    // observes in the runtime clone at this exact point in the Play sequence.
    // The controller calls Create() after InitPrevTransforms, so the probe
    // sees refreshed world matrices; moving construction earlier observes
    // stale clone state and the ordering test goes red.
    if (s_TestPoseProbe)
    {
        std::vector<std::pair<UUID, entt::entity>> bodies;
        auto bodyView = runtime.ecs.registry.view<PhysicsBodyComponent>();
        for (auto e : bodyView)
        {
            const auto* idc =
                runtime.ecs.registry.try_get<EntityIdComponent>(e);
            if (idc != nullptr && !idc->id.IsNull())
                bodies.emplace_back(idc->id, e);
        }
        std::sort(bodies.begin(), bodies.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& [uuid, entity] : bodies)
        {
            const auto* tf =
                runtime.ecs.registry.try_get<Transform>(entity);
            if (tf == nullptr)
                continue;
            ConstructionPose pose;
            pose.id = uuid;
            pose.translation = glm::vec3(tf->worldMatrix[3]);
            s_RecordedPoses.push_back(pose);
        }
    }

    // T4: stage every authored body from the runtime clone (statics, then
    // kinematics, then dynamics, then ghosts; UUID order within each group).
    // Any failure returns a typed UUID-named Error and the half-staged
    // candidate dies with the local through the full Shutdown() teardown.
    // (Covered by the single outer boundary: no inner island remains.)
    {
        Error buildErr;
        if (!StageBodies(*world, runtime, provider, buildErr))
        {
            return Result<std::unique_ptr<PhysicsWorld>>::Fail(
                buildErr.code, buildErr.path, buildErr.detail);
        }
    }

    // T5: stage every authored hinge/slider after all bodies and ghosts, in
    // stable owner-UUID order across both kinds. Frames resolve once at
    // build; any failure (dangling/self reference, singular frame, invalid
    // limit range, body-less owner/other) rolls the whole candidate back.
    // Same outer allocation boundary: resource exhaustion surfaces as
    // Error::Io with no committed world.
    {
        Error buildErr;
        if (!StageConstraints(*world, runtime, buildErr))
        {
            return Result<std::unique_ptr<PhysicsWorld>>::Fail(
                buildErr.code, buildErr.path, buildErr.detail);
        }
    }

    // Late-failure probe: fires only after the live world above exists, so
    // the returned local dies here and runs the real destructor rollback.
    // The controller's catch (destroy candidate, reset clone, Edit state,
    // zero accumulator, no bridge call, no script callback) is what
    // RED_PhysicsPlayConstructionIsAtomic observes.
    if (s_TestInjectCreateFailure)
    {
        return Result<std::unique_ptr<PhysicsWorld>>::Fail(
            Error::InvalidRuntimeState, "",
            "PhysicsWorld::Create test-injected candidate failure "
            "(live candidate rolled back; no world committed)");
    }

    return Result<std::unique_ptr<PhysicsWorld>>::Ok(std::move(world));
    }
    catch (const std::bad_alloc&)
    {
        return Result<std::unique_ptr<PhysicsWorld>>::Fail(
            Error::Io, "",
            "PhysicsWorld::Create allocation failure during candidate "
            "construction (rolled back; no world committed)");
    }
}

size_t PhysicsWorld::LiveWorldCount()
{
    return s_LiveWorlds;
}

void PhysicsWorld::FastSphereCcdPreset(float radius, float& thresholdOut,
                                    float& sweptOut)
{
    thresholdOut = 0.5f * radius;
    sweptOut = 0.8f * radius;
}

const PhysicsBodyRecord* PhysicsWorld::FindBody(const UUID& id) const
{
    for (const auto& rec : m_BodyIndex)
    {
        if (rec.id == id)
            return &rec;
    }
    return nullptr;
}

const btCollisionShape* PhysicsWorld::FindBodyShape(const UUID& id) const
{
    const PhysicsBodyRecord* rec = FindBody(id);
    return rec != nullptr ? rec->shape : nullptr;
}

static PhysicsBodyRecord* FindBodyMut(std::vector<PhysicsBodyRecord>& index,
                                        const UUID& id)
{
    for (auto& rec : index)
    {
        if (rec.id == id)
            return &rec;
    }
    return nullptr;
}

bool PhysicsWorld::BodyWorldPosition(const UUID& id, glm::vec3& out) const
{
    const PhysicsBodyRecord* rec = FindBody(id);
    if (rec == nullptr)
        return false;
    const btTransform* t = nullptr;
    if (rec->body != nullptr)
        t = &rec->body->getWorldTransform();
    else if (rec->ghost != nullptr)
        t = &rec->ghost->getWorldTransform();
    else
        return false;
    const btVector3& o = t->getOrigin();
    out = glm::vec3(o.x(), o.y(), o.z());
    return true;
}

bool PhysicsWorld::SetBodyLinearVelocity(const UUID& id,
                                         const glm::vec3& velocity)
{
    PhysicsBodyRecord* rec = FindBodyMut(m_BodyIndex, id);
    if (rec == nullptr || rec->body == nullptr ||
        rec->kind == PhysicsBodyKind::Static)
        return false;
    rec->body->setLinearVelocity(
        btVector3(velocity.x, velocity.y, velocity.z));
    rec->body->activate();
    return true;
}

// ============================================================================
// T7 bounded Lua physics controls
// ============================================================================

bool PhysicsWorld::GetBodyLinearVelocity(const UUID& id,
                                         glm::vec3& out) const
{
    const PhysicsBodyRecord* rec = FindBody(id);
    if (rec == nullptr || rec->body == nullptr)
        return false;
    const btVector3& v = rec->body->getLinearVelocity();
    out = glm::vec3(v.x(), v.y(), v.z());
    return true;
}

bool PhysicsWorld::GetBodyAngularVelocity(const UUID& id,
                                          glm::vec3& out) const
{
    const PhysicsBodyRecord* rec = FindBody(id);
    if (rec == nullptr || rec->body == nullptr)
        return false;
    const btVector3& w = rec->body->getAngularVelocity();
    out = glm::vec3(w.x(), w.y(), w.z());
    return true;
}

bool PhysicsWorld::ApplyBodyImpulse(const UUID& id, const glm::vec3& impulse)
{
    if (!std::isfinite(impulse.x) || !std::isfinite(impulse.y) ||
        !std::isfinite(impulse.z))
        return false;
    PhysicsBodyRecord* rec = FindBodyMut(m_BodyIndex, id);
    // Dynamic only: Static bodies never move and Kinematic bodies have
    // effectively infinite mass, so an impulse on either refuses loudly
    // instead of vanishing into the solver.
    if (rec == nullptr || rec->body == nullptr ||
        rec->kind != PhysicsBodyKind::Dynamic)
        return false;
    rec->body->activate();
    rec->body->applyCentralImpulse(
        btVector3(impulse.x, impulse.y, impulse.z));
    return true;
}

bool PhysicsWorld::ResetBodyPose(SceneDocument& runtime, const UUID& id,
                                 const glm::vec3& position,
                                 const glm::quat& rotation,
                                 bool hasLinearVelocity,
                                 const glm::vec3& linearVelocity,
                                 bool hasAngularVelocity,
                                 const glm::vec3& angularVelocity)
{
    // ---- Phase 1: validate EVERYTHING before mutating anything ------------
    // A failed reset changes no Bullet state, no ECS state, and no overlap
    // history (the ticket's "no partial state change" failure check).
    if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z) ||
        std::fabs(position.x) > (float)FLT_MAX ||
        std::fabs(position.y) > (float)FLT_MAX ||
        std::fabs(position.z) > (float)FLT_MAX)
        return false;
    if (!std::isfinite(rotation.x) || !std::isfinite(rotation.y) ||
        !std::isfinite(rotation.z) || !std::isfinite(rotation.w))
        return false;
    // Overflow-safe norm in double precision (same discipline as the sink's
    // transform gate): finite near-FLT_MAX components must not overflow the
    // norm, and the normalized output is re-validated.
    const double dx = (double)rotation.x;
    const double dy = (double)rotation.y;
    const double dz = (double)rotation.z;
    const double dw = (double)rotation.w;
    const double norm =
        std::sqrt(dx * dx + dy * dy + dz * dz + dw * dw);
    if (!std::isfinite(norm) || !(norm > 1e-6) || norm > (double)FLT_MAX)
        return false;
    // glm::quat stores (w, x, y, z).
    const glm::quat unitRotation{(float)(dw / norm), (float)(dx / norm),
                                 (float)(dy / norm), (float)(dz / norm)};
    if (!std::isfinite(unitRotation.x) || !std::isfinite(unitRotation.y) ||
        !std::isfinite(unitRotation.z) || !std::isfinite(unitRotation.w))
        return false;
    if (hasLinearVelocity &&
        (!std::isfinite(linearVelocity.x) ||
         !std::isfinite(linearVelocity.y) ||
         !std::isfinite(linearVelocity.z)))
        return false;
    if (hasAngularVelocity &&
        (!std::isfinite(angularVelocity.x) ||
         !std::isfinite(angularVelocity.y) ||
         !std::isfinite(angularVelocity.z)))
        return false;

    auto& reg = runtime.ecs.registry;
    const auto entity = runtime.FindByUuid(id);
    if (entity == entt::null || !reg.valid(entity))
        return false;
    auto* tf = reg.try_get<Transform>(entity);
    if (tf == nullptr)
        return false;
    const auto* bodyComp = reg.try_get<PhysicsBodyComponent>(entity);
    if (bodyComp == nullptr ||
        bodyComp->kind == PhysicsBodyKind::Static)
        return false;
    // T7 re-review P1(2): reset is a rigid-body operation. Ghost-only
    // entities (triggers, including valid Kinematic triggers) have no solver
    // body to move: they refuse here instead of teleporting a trigger. The
    // controller queue gate enforces the same rule before enqueueing, so
    // reaching this refusal from the drain is unexpected (and loud there).
    PhysicsBodyRecord* rec = FindBodyMut(m_BodyIndex, id);
    if (rec == nullptr || rec->body == nullptr)
        return false;

    // T7 re-review P2(alloc): stage the previous-transform repair set BEFORE
    // the first Bullet or ECS mutation. The subtree walk allocates; under
    // exhaustion the reset must refuse loudly with nothing moved (the same
    // translation discipline as the staging/rebuild boundaries), never move
    // the body and then fail the descendant repair. The test throw below
    // proves the seam without needing real memory pressure.
    std::vector<entt::entity> repairSubtree;
    try
    {
        if (s_ResetCollectTestThrow)
            throw std::bad_alloc();
        SceneHierarchy::CollectSubtreePreOrder(reg, entity, repairSubtree);
    }
    catch (const std::bad_alloc&)
    {
        printf("[Physics] reset_body_pose refused for %s "
               "(subtree collection exhausted resources; body unmoved)\n",
               id.ToString().c_str());
        return false;
    }

    const glm::vec3 newLinear =
        hasLinearVelocity ? linearVelocity : glm::vec3{0.0f, 0.0f, 0.0f};
    const glm::vec3 newAngular =
        hasAngularVelocity ? angularVelocity : glm::vec3{0.0f, 0.0f, 0.0f};
    const btTransform newPose(
        btQuaternion(unitRotation.x, unitRotation.y, unitRotation.z,
                     unitRotation.w),
        btVector3(position.x, position.y, position.z));

    // ---- Phase 2: atomic apply (rigid body only) --------------------------
    {
        btRigidBody* body = rec->body;
        if (rec->motion != nullptr)
            rec->motion->setWorldTransform(newPose);
        body->setWorldTransform(newPose);
        // Interpolation must restart at the new pose: otherwise the next
        // step interpolates from the stale pre-reset transform and the body
        // visibly sweeps through the teleport for one frame.
        body->setInterpolationWorldTransform(newPose);
        body->setInterpolationLinearVelocity(
            btVector3(newLinear.x, newLinear.y, newLinear.z));
        body->setInterpolationAngularVelocity(
            btVector3(newAngular.x, newAngular.y, newAngular.z));
        body->clearForces();
        body->setLinearVelocity(
            btVector3(newLinear.x, newLinear.y, newLinear.z));
        body->setAngularVelocity(
            btVector3(newAngular.x, newAngular.y, newAngular.z));
        body->activate();
        // Broadphase/contact refresh: drop the proxy's stale pairs (they
        // describe the pre-reset AABB) and clear every manifold touching
        // this body, so the next scrape cannot report a Contact for a
        // collision that no longer exists. Pairs re-form on the next step
        // iff the body still overlaps.
        btBroadphaseProxy* proxy = body->getBroadphaseHandle();
        if (proxy != nullptr)
        {
            m_World.getBroadphase()->getOverlappingPairCache()
                ->cleanProxyFromPairs(proxy, &m_Dispatcher);
        }
        for (int i = m_Dispatcher.getNumManifolds() - 1; i >= 0; --i)
        {
            btPersistentManifold* manifold =
                m_Dispatcher.getManifoldByIndexInternal(i);
            if (manifold != nullptr &&
                (manifold->getBody0() == body ||
                 manifold->getBody1() == body))
                m_Dispatcher.clearManifold(manifold);
        }
        m_World.updateSingleAabb(body);
    }
    // Overlap-history purge: pairs mentioning this UUID describe the
    // pre-reset location. Without this the next tick would fabricate a
    // TriggerExit for an overlap that ended by teleport, not by motion.
    for (auto it = m_PrevOverlaps.begin(); it != m_PrevOverlaps.end();)
    {
        if (it->first == id || it->second == id)
            it = m_PrevOverlaps.erase(it);
        else
            ++it;
    }

    // ECS authority write-back (roots-only: local == world): the same pose,
    // marked dirty, with world matrices refreshed. Kinematic scale is
    // untouched (collision scale stays baked at Play).
    // T7 re-review P1(3): previous-transform repair covers the whole
    // transformed subtree, not just the body root. UpdateWorldTransforms
    // propagates the dirty parent into descendants and rewrites their
    // worldMatrix; without re-snapping theirs too, a legal visual child of
    // a reset body keeps a pre-teleport prevWorldMatrix and produces a
    // one-frame motion-vector spike.
    tf->translation = position;
    tf->rotation = unitRotation;
    SceneGraph::SetLocalDirty(reg, entity);
    SceneGraph::UpdateWorldTransforms(reg);
    // Staged in Phase 1 (before any mutation), so this walk cannot fail
    // partway: every entry was collected while the world was still intact.
    for (const auto child : repairSubtree)
    {
        if (auto* childTf = reg.try_get<Transform>(child))
            childTf->prevWorldMatrix = childTf->worldMatrix;
    }
    return true;
}

void PhysicsWorld::PreStepSync(SceneDocument& runtime)
{
    auto& reg = runtime.ecs.registry;
    // Refresh world matrices after OnFixedUpdate/Motion writes so the
    // kinematic push observes current poses (bodies are roots: world==local,
    // but the matrix is what Create baked from, so read it, not the TRS).
    SceneGraph::UpdateWorldTransforms(reg);
    for (auto& rec : m_BodyIndex)
    {
        if (rec.kind != PhysicsBodyKind::Kinematic)
            continue;
        if (!reg.valid(rec.entity))
            continue;
        const auto* tf = reg.try_get<Transform>(rec.entity);
        if (tf == nullptr)
            continue;
        const glm::vec3 t(tf->worldMatrix[3]);
        const glm::quat r =
            glm::quat_cast(glm::mat3(tf->worldMatrix) / rec.bakedScale);
        const btTransform btT(
            btQuaternion(r.x, r.y, r.z, r.w), btVector3(t.x, t.y, t.z));
        if (rec.body != nullptr)
        {
            // Publish the commanded pose through the motion state ONLY.
            // Bullet's saveKinematicState pulls motion-state -> world and
            // derives linear/angular velocity from previous-interpolation ->
            // new-world, which is exactly the platform motion contacts must
            // feel. Writing the world or interpolation transform here would
            // collapse that difference to zero velocity (or be overwritten
            // by the motion-state pull).
            if (rec.motion != nullptr)
                rec.motion->setWorldTransform(btT);
            rec.body->activate();
        }
        else if (rec.ghost != nullptr)
        {
            rec.ghost->setWorldTransform(btT);
        }
    }

    // T5 drive servo (every fixed tick, before the step): powered slider
    // targets and hinge returns are closed-loop setpoints, not one-shot
    // velocities. Re-servoing each tick makes the authored target the
    // attractor — larger displacements still map monotonically to higher
    // speeds (same proportional law as the drive entry points), but the
    // mechanism settles AT the target instead of blowing past it on a
    // constant initial velocity. Drive commands therefore take effect on
    // the immediately following step (at most one tick of latency).
    for (auto& rec : m_ConstraintIndex)
    {
        if (rec.isHinge && rec.hinge != nullptr && rec.returning)
        {
            const float delta = rec.restPosition - rec.hinge->getHingeAngle();
            // Disengage inside half a degree: the motor did its job, and a
            // one-shot velocity would orbit the setpoint forever. Relax
            // both endpoints exactly like an explicit release (T5 fixup
            // re-review P2): bodies ReturnHingeToRest pinned to
            // DISABLE_DEACTIVATION must return to ACTIVE_TAG once the
            // return completes, or the island never sleeps again. The
            // shared-endpoint-aware policy keeps an endpoint awake while
            // another enabled constraint still touches it.
            if (std::abs(delta) < 0.0087f)
            {
                rec.hinge->enableAngularMotor(false, 0.0f, 0.0f);
                rec.returning = false;
                const UUID ownerId = rec.ownerId;
                RelaxConstraintEndpoints(ownerId);
                continue;
            }
            float speed = rec.driveSpeed;
            if (!(speed > 0.0f))
                speed = 1.0f;
            rec.hinge->enableAngularMotor(true,
                                          delta > 0.0f ? speed : -speed,
                                          rec.driveMax);
        }
        else if (!rec.isHinge && rec.slider != nullptr &&
                 rec.slider->getPoweredLinMotor())
        {
            const float delta =
                rec.restPosition - rec.slider->getLinearPos();
            float speed = kT5SliderTargetGain * delta;
            speed = std::clamp(speed, -rec.driveSpeed, rec.driveSpeed);
            rec.slider->setTargetLinMotorVelocity(speed);
        }
    }
}

void PhysicsWorld::PostStepSync(SceneDocument& runtime)
{
    auto& reg = runtime.ecs.registry;
    // Dynamic Bullet -> ECS after step. Static/Kinematic/ghost poses are
    // never written back (single authority per kind). Local TRS is written
    // and marked dirty; the controller's one batched SceneGraph +
    // TransformSync pass per presentation frame carries it to the GPU.
    for (auto& rec : m_BodyIndex)
    {
        if (rec.kind != PhysicsBodyKind::Dynamic || rec.body == nullptr)
            continue;
        if (!reg.valid(rec.entity))
            continue;
        auto* tf = reg.try_get<Transform>(rec.entity);
        if (tf == nullptr)
            continue;
        const btTransform& wt = rec.body->getWorldTransform();
        const btVector3& o = wt.getOrigin();
        const btQuaternion& q = wt.getRotation();
        tf->translation = glm::vec3(o.x(), o.y(), o.z());
        tf->rotation = glm::normalize(glm::quat(q.w(), q.x(), q.y(), q.z()));
        SceneGraph::SetLocalDirty(reg, rec.entity);
    }
}

// ============================================================================
// T6 sync + introspection (member definitions at rt2::core scope)
// ============================================================================

void PhysicsWorld::Step(float dt)
{
    // The outer RT2 fixed-step accumulator is the ONLY substepper (plan
    // section 2): exactly one Bullet step per RT2 fixed tick. maxSubSteps 0
    // performs a single stepSimulation(dt) without an inner substep loop,
    // which keeps paused-Step (exactly one kFixedDt tick, accumulator
    // untouched) trivially correct and Lua on_fixed_update ordering stable.
    m_World.stepSimulation(dt, 0);
    ++m_StepCount;
}

namespace {

// T6 scrape helper: map a live Bullet collision object back to its
// Authoring UUID through the companion map (bodies XOR ghosts). Linear scan:
// N is toy-scale, and scan order never affects output because every
// collection point below sorts by UUID before emitting.
const UUID* T6UuidForObject(const std::vector<PhysicsBodyRecord>& index,
                            const btCollisionObject* object)
{
    for (const auto& rec : index)
    {
        if (rec.body == object || rec.ghost == object)
            return &rec.id;
    }
    return nullptr;
}

glm::vec3 T6ToGlm(const btVector3& v)
{
    return glm::vec3(v.x(), v.y(), v.z());
}

// T6 re-review P1(2): narrowphase confirmation for one ghost-cache
// candidate pair. Reports touching (distance <= 0) narrowphase points only.
struct T6OverlapConfirmCallback : public btCollisionWorld::ContactResultCallback
{
    bool confirmed = false;

    btScalar addSingleResult(
        btManifoldPoint& cp, const btCollisionObjectWrapper* colObj0Wrap,
        int partId0, int index0,
        const btCollisionObjectWrapper* colObj1Wrap, int partId1,
        int index1) override
    {
        (void)colObj0Wrap;
        (void)partId0;
        (void)index0;
        (void)colObj1Wrap;
        (void)partId1;
        (void)index1;
        if (cp.getDistance() <= btScalar(0))
            confirmed = true;
        return btScalar(0);
    }
};

// The ghost pair cache is broadphase-AABB-based, so a cached pair is only a
// candidate: a rotated thin trigger reports pairs whose shapes never touch.
// contactPairTest runs the narrowphase directly (bypassing the
// no-contact-response flags, exactly like the collision query path), and the
// pair joins the overlap set only when at least one touching point exists.
bool T6NarrowphaseOverlapConfirmed(btDiscreteDynamicsWorld& world,
                                   btCollisionObject* a, btCollisionObject* b)
{
    if (a == nullptr || b == nullptr || a == b)
        return false;
    T6OverlapConfirmCallback cb;
    cb.m_closestDistanceThreshold = btScalar(0);
    world.contactPairTest(a, b, cb);
    return cb.confirmed;
}

void T6LiveObjectFor(std::vector<PhysicsBodyRecord>& index, const UUID& id,
                     btCollisionObject*& out)
{
    out = nullptr;
    for (auto& rec : index)
    {
        if (!(rec.id == id))
            continue;
        out = (rec.body != nullptr)
                  ? static_cast<btCollisionObject*>(rec.body)
                  : static_cast<btCollisionObject*>(rec.ghost);
        return;
    }
}

} // namespace

void PhysicsWorld::ClearEventHistory()
{
    m_PrevOverlaps.clear();
}

PhysicsEvent PhysicsWorld::CoalesceContactPoints(
    const UUID& bodyA, const UUID& bodyB,
    const std::vector<ContactCoalescePoint>& points, uint32_t tickIndex)
{
    PhysicsEvent e;
    e.kind = PhysicsEventKind::Contact;
    e.bodyA = bodyA;
    e.bodyB = bodyB;
    e.tickIndex = tickIndex;
    if (points.empty())
        return e;
    glm::vec3 posSum = points.front().position;
    glm::vec3 nrmSum = points.front().normal;
    float impSum = points.front().impulse;
    for (size_t k = 1; k < points.size(); ++k)
    {
        posSum += points[k].position;
        nrmSum += points[k].normal;
        impSum += points[k].impulse;
    }
    const float count = (float)points.size();
    glm::vec3 nrm = nrmSum;
    const float len = glm::length(nrm);
    // Antiparallel manifold normals summing near zero are degenerate;
    // fall back to the run's first normal rather than emitting NaN.
    e.normal = (len > 1e-9f) ? (nrm / len) : points.front().normal;
    e.position = posSum / count;
    e.impulse = impSum;
    return e;
}

void PhysicsWorld::AppendTickEvents(std::vector<PhysicsEvent>& out,
                                    uint32_t tickIndex)
{
    // ---- Contacts: one coalesced event per unordered pair per tick ------
    // Manifolds iterate in Bullet solver order (not UUID order), so raw
    // points are collected, sorted by canonical pair, then coalesced through
    // CoalesceContactPoints: the stored impulse is the tick's summed applied
    // impulse, the position the mean, the normal the normalized mean.
    // Qualification is touch, not impact (re-review P1(3)): the solver acting
    // qualifies by impulse, and touching/penetrating points qualify by
    // distance even at zero impulse. Only speculative separating points the
    // solver ignored carry no event.
    struct RawPoint
    {
        UUID a;
        UUID b;
        glm::vec3 pos;
        glm::vec3 nrm;
        float imp = 0.0f;
    };
    std::vector<RawPoint> raw;
    const int manifoldCount = m_Dispatcher.getNumManifolds();
    for (int i = 0; i < manifoldCount; ++i)
    {
        btPersistentManifold* manifold =
            m_Dispatcher.getManifoldByIndexInternal(i);
        if (manifold == nullptr)
            continue;
        const btCollisionObject* o0 = manifold->getBody0();
        const btCollisionObject* o1 = manifold->getBody1();
        const UUID* id0 = T6UuidForObject(m_BodyIndex, o0);
        const UUID* id1 = T6UuidForObject(m_BodyIndex, o1);
        if (id0 == nullptr || id1 == nullptr)
            continue;
        // Ghosts carry no contact response and never own manifolds; skip
        // defensively so a trigger can never produce a Contact event.
        const PhysicsBodyRecord* r0 = FindBody(*id0);
        const PhysicsBodyRecord* r1 = FindBody(*id1);
        if (r0 == nullptr || r1 == nullptr || r0->isTrigger || r1->isTrigger)
            continue;
        const int pointCount = manifold->getNumContacts();
        for (int j = 0; j < pointCount; ++j)
        {
            const btManifoldPoint& pt = manifold->getContactPoint(j);
            const float impulse = pt.getAppliedImpulse();
            const float distance = pt.getDistance();
            // T6 re-review P1(3): Contact is narrowphase-confirmed touch,
            // not impact. Points the solver acted on qualify by impulse;
            // touching/penetrating points qualify by distance even when the
            // solver left the applied impulse at zero (e.g. a
            // static-vs-kinematic manifold the solver does not act on).
            // Speculative separating points the solver ignored carry no
            // event. The stored impulse keeps its real value, including 0.
            if (!(impulse > 0.0f) && !(distance <= 0.0f))
                continue;
            UUID a;
            UUID b;
            glm::vec3 n;
            CanonicalizePhysicsPair(*id0, *id1,
                                    T6ToGlm(pt.m_normalWorldOnB), a, b, n);
            RawPoint p;
            p.a = a;
            p.b = b;
            p.pos = T6ToGlm(pt.getPositionWorldOnB());
            p.nrm = n;
            p.imp = impulse;
            raw.push_back(p);
        }
    }
    std::sort(raw.begin(), raw.end(),
              [](const RawPoint& x, const RawPoint& y) {
                  if (x.a != y.a)
                      return x.a < y.a;
                  return x.b < y.b;
              });
    for (size_t i = 0; i < raw.size();)
    {
        size_t j = i + 1;
        while (j < raw.size() && raw[j].a == raw[i].a && raw[j].b == raw[i].b)
            ++j;
        // One canonical pair's run routes through the shared aggregation
        // (the discriminator's seam), never an inline copy of it.
        std::vector<ContactCoalescePoint> run;
        run.reserve(j - i);
        for (size_t k = i; k < j; ++k)
        {
            ContactCoalescePoint p;
            p.position = raw[k].pos;
            p.normal = raw[k].nrm;
            p.impulse = raw[k].imp;
            run.push_back(p);
        }
        out.push_back(
            CoalesceContactPoints(raw[i].a, raw[i].b, run, tickIndex));
        i = j;
    }

    // ---- Ghosts: narrowphase-confirmed overlap set-diff ------------------
    // Ghost records are visited in UUID order; the overlap sets below are
    // std::set (UUID order), so Enter/Stay/Exit emission is canonical with
    // no dependence on broadphase pair order. Emission is grouped by kind
    // (re-review P2(4)): all enters in pair order, then all stays, then all
    // exits — never interleaved by pair.
    std::vector<const PhysicsBodyRecord*> ghosts;
    for (const auto& rec : m_BodyIndex)
    {
        if (rec.ghost != nullptr)
            ghosts.push_back(&rec);
    }
    std::sort(ghosts.begin(), ghosts.end(),
              [](const PhysicsBodyRecord* x, const PhysicsBodyRecord* y) {
                  return x->id < y->id;
              });
    std::set<std::pair<UUID, UUID>> candidates;
    for (const PhysicsBodyRecord* rec : ghosts)
    {
        btHashedOverlappingPairCache* cache =
            rec->ghost->getOverlappingPairCache();
        if (cache == nullptr)
            continue;
        btBroadphasePairArray& pairs = cache->getOverlappingPairArray();
        const int pairCount = cache->getNumOverlappingPairs();
        for (int k = 0; k < pairCount; ++k)
        {
            const auto* proxy0 = pairs[k].m_pProxy0;
            const auto* proxy1 = pairs[k].m_pProxy1;
            if (proxy0 == nullptr || proxy1 == nullptr)
                continue;
            const auto* c0 =
                static_cast<const btCollisionObject*>(proxy0->m_clientObject);
            const auto* c1 =
                static_cast<const btCollisionObject*>(proxy1->m_clientObject);
            const btCollisionObject* other =
                (c0 == rec->ghost) ? c1 : c0;
            if (other == nullptr || other == rec->ghost)
                continue;
            const UUID* otherId = T6UuidForObject(m_BodyIndex, other);
            if (otherId == nullptr || *otherId == rec->id)
                continue;
            const UUID lo = (*otherId < rec->id) ? *otherId : rec->id;
            const UUID hi = (*otherId < rec->id) ? rec->id : *otherId;
            candidates.emplace(lo, hi);
        }
    }
    // T6 re-review P1(2): confirm every broadphase candidate with the
    // narrowphase before it joins the overlap set, so AABB-only pairs
    // (rotated thin triggers, near misses) never emit Enter/Stay.
    std::set<std::pair<UUID, UUID>> current;
    for (const auto& p : candidates)
    {
        btCollisionObject* a = nullptr;
        btCollisionObject* b = nullptr;
        T6LiveObjectFor(m_BodyIndex, p.first, a);
        T6LiveObjectFor(m_BodyIndex, p.second, b);
        if (T6NarrowphaseOverlapConfirmed(m_World, a, b))
            current.insert(p);
    }
    // Trigger payload: pair midpoint as position, canonical bodyA -> bodyB
    // direction as normal (+Y when coincident or unresolvable), impulse 0.
    auto emitTrigger = [this, tickIndex, &out](
                           PhysicsEventKind kind, const UUID& a, const UUID& b) {
        PhysicsEvent e;
        e.kind = kind;
        e.bodyA = a;
        e.bodyB = b;
        glm::vec3 pa{0.0f, 0.0f, 0.0f};
        glm::vec3 pb{0.0f, 0.0f, 0.0f};
        const bool oka = BodyWorldPosition(a, pa);
        const bool okb = BodyWorldPosition(b, pb);
        e.position = (oka && okb) ? (pa + pb) * 0.5f : (oka ? pa : pb);
        const glm::vec3 d = pb - pa;
        const float len = glm::length(d);
        e.normal = (oka && okb && len > 1e-9f)
                       ? (d / len)
                       : glm::vec3{0.0f, 1.0f, 0.0f};
        e.impulse = 0.0f;
        e.tickIndex = tickIndex;
        out.push_back(e);
    };
    for (const auto& p : current)
    {
        if (m_PrevOverlaps.count(p) == 0)
            emitTrigger(PhysicsEventKind::TriggerEnter, p.first, p.second);
    }
    for (const auto& p : current)
    {
        if (m_PrevOverlaps.count(p) != 0)
            emitTrigger(PhysicsEventKind::TriggerStay, p.first, p.second);
    }
    for (const auto& p : m_PrevOverlaps)
    {
        if (current.count(p) == 0)
            emitTrigger(PhysicsEventKind::TriggerExit, p.first, p.second);
    }
    m_PrevOverlaps = current;
}

void PhysicsWorld::SetTestInjectCreateFailure(bool fail)
{
    s_TestInjectCreateFailure = fail;
}

void PhysicsWorld::SetStagingTestThrow(bool fail)
{
    s_StagingTestThrow = fail;
}

bool PhysicsWorld::StagingTestThrow()
{
    return s_StagingTestThrow;
}

void PhysicsWorld::SetRebuildAllocThrowPhase(RebuildAllocThrowPhase phase)
{
    s_RebuildAllocThrowPhase = phase;
}

PhysicsWorld::RebuildAllocThrowPhase PhysicsWorld::GetRebuildAllocThrowPhase()
{
    return s_RebuildAllocThrowPhase;
}

void PhysicsWorld::SetCandidateThrowPoint(CandidateThrowPoint point)
{
    s_CandidateThrowPoint = point;
}

PhysicsWorld::CandidateThrowPoint PhysicsWorld::GetCandidateThrowPoint()
{
    return s_CandidateThrowPoint;
}

void PhysicsWorld::SetEscapeTestThrow(bool fail)
{
    s_EscapeTestThrow = fail;
}

bool PhysicsWorld::EscapeTestThrow()
{
    return s_EscapeTestThrow;
}

bool PhysicsWorld::TestInjectCreateFailure()
{
    return s_TestInjectCreateFailure;
}

void PhysicsWorld::SetTestDestroyProbe(std::function<void()> probe)
{
    s_TestDestroyProbe = std::move(probe);
}

void PhysicsWorld::SetTestPoseProbe(bool enabled)
{
    s_TestPoseProbe = enabled;
    if (!enabled)
        s_RecordedPoses.clear();
}

std::vector<PhysicsWorld::ConstructionPose> PhysicsWorld::TestRecordedPoses()
{
    return s_RecordedPoses;
}

void PhysicsWorld::ClearTestRecordedPoses()
{
    s_RecordedPoses.clear();
}

// ============================================================================
// BuildBodies — T4 candidate body staging driver
// ============================================================================

namespace {

std::string T4EntityName(const entt::registry& registry, entt::entity e)
{
    const auto* name = registry.try_get<NameComponent>(e);
    return name != nullptr ? name->name : std::string{};
}

bool T4Fail(Error& err, const UUID& owner, const std::string& name,
            const std::string& detail)
{
    err.code = Error::InvalidArgument;
    err.path = owner.ToString();
    err.detail = "entity " + owner.ToString() +
                 (name.empty() ? "" : " ('" + name + "') ") + detail;
    return false;
}

bool T4FailCode(Error& err, Error::Code code, const UUID& owner,
                const std::string& name, const std::string& detail)
{
    err.code = code;
    err.path = owner.ToString();
    err.detail = "entity " + owner.ToString() +
                 (name.empty() ? "" : " ('" + name + "') ") + detail;
    return false;
}

bool IsValidBodyNumerics(const PhysicsBodyComponent& body)
{
    return std::isfinite(body.mass) && std::isfinite(body.friction) &&
           std::isfinite(body.restitution) &&
           std::isfinite(body.linearDamping) &&
           std::isfinite(body.angularDamping) &&
           std::isfinite(body.ccdMotionThreshold) &&
           std::isfinite(body.ccdSweptRadius);
}

// Decompose a refreshed world matrix under the known uniform scale.
void DecomposeUniform(const glm::mat4& world, float scale, glm::vec3& t,
                      glm::quat& r)
{
    t = glm::vec3(world[3]);
    r = glm::quat_cast(glm::mat3(world) / scale);
}

btTransform ToBtTransform(const glm::vec3& t, const glm::quat& r)
{
    return btTransform(btQuaternion(r.x, r.y, r.z, r.w),
                       btVector3(t.x, t.y, t.z));
}

// Trust boundary for injected providers: a successful Result carries a
// pointer the consumer must validate before any Bullet allocation or read.
// The production decoder guarantees these invariants, but the interface is
// explicitly usable by CPU targets with alternate providers, so staging
// re-checks: non-null, triplet/non-empty/capped vertices, finite vertices,
// triplet/capped indices, and every index inside the vertex range.
bool ValidateProviderPayload(const CollisionGeometry* geom, const UUID& uuid,
                             const std::string& name, const char* wire,
                             Error& err)
{
    auto fail = [&](Error::Code code, const std::string& detail) {
        err.code = code;
        err.path = uuid.ToString();
        err.detail = "entity " + uuid.ToString() +
                     (name.empty() ? "" : " ('" + name + "') ") + detail;
        return false;
    };
    if (geom == nullptr)
    {
        return fail(Error::Parse,
                    std::string(wire) +
                        " provider returned success with null geometry");
    }
    if (geom->vertices.empty() || geom->vertices.size() % 3 != 0)
    {
        return fail(Error::Parse,
                    std::string(wire) +
                        " provider geometry has no xyz-triplet vertices");
    }
    const size_t points = geom->vertices.size() / 3;
    if (points > kMaxCollisionVertices)
    {
        return fail(Error::InvalidArgument,
                    std::string(wire) + " provider geometry has " +
                        std::to_string(points) + " vertices (cap " +
                        std::to_string(kMaxCollisionVertices) + ")");
    }
    for (float v : geom->vertices)
    {
        if (!std::isfinite(v))
        {
            return fail(Error::Parse,
                        std::string(wire) +
                            " provider geometry has non-finite vertices");
        }
    }
    if (geom->indices.empty() || geom->indices.size() % 3 != 0)
    {
        return fail(Error::Parse,
                    std::string(wire) +
                        " provider geometry has no triangle-triplet indices");
    }
    if (geom->indices.size() > kMaxCollisionIndices)
    {
        return fail(Error::InvalidArgument,
                    std::string(wire) + " provider geometry has " +
                        std::to_string(geom->indices.size()) +
                        " indices (cap " +
                        std::to_string(kMaxCollisionIndices) + ")");
    }
    for (uint32_t vi : geom->indices)
    {
        if ((size_t)vi >= points)
        {
            return fail(Error::Parse,
                        std::string(wire) +
                            " provider geometry has an out-of-range index");
        }
    }
    return true;
}

} // namespace

// ============================================================================
// PhysicsWorld::StageOneBody / StageBodies — member definitions at
// rt2::core scope (lambdas are illegal inside the anonymous namespace above)
// ============================================================================

// Stage one authored body. Appends records/shapes/motion/bodies/ghosts on
// success; on failure returns false with a UUID-named typed Error and leaves
// the candidate for the caller to roll back wholesale.
bool PhysicsWorld::StageOneBody(PhysicsWorld& world,
                                const entt::registry& registry,
                                const UUID& uuid, entt::entity entity,
                                const IPhysicsCollisionAssetProvider* provider,
                                Error& err)
{
    const std::string name = T4EntityName(registry, entity);
    const auto& body = registry.get<PhysicsBodyComponent>(entity);
    const auto* shape = registry.try_get<PhysicsShapeComponent>(entity);
    const auto* tf = registry.try_get<Transform>(entity);

    if (shape == nullptr)
    {
        return T4FailCode(err, Error::MissingAsset, uuid, name,
                          "carries PhysicsBodyComponent but no PhysicsShapeComponent "
                          "(missing collider; attach a shape before Play)");
    }
    if (tf == nullptr)
    {
        return T4FailCode(err, Error::InvalidTransform, uuid, name,
                          "carries PhysicsBodyComponent but has no Transform");
    }
    if (!IsValidBodyNumerics(body))
    {
        return T4Fail(err, uuid, name,
                      "has non-finite body parameters (mass/friction/restitution/"
                      "damping/CCD must all be finite)");
    }

    // Enum boundary: raw registry mutation can forge values the authoring
    // API and deserializer refuse, so Play staging re-validates both enums
    // with UUID-bearing errors (an unknown shape must never reach the shape
    // switch as a null dereference).
    if (body.kind != PhysicsBodyKind::Static &&
        body.kind != PhysicsBodyKind::Dynamic &&
        body.kind != PhysicsBodyKind::Kinematic)
    {
        return T4Fail(err, uuid, name,
                      "has an unknown body kind (Static/Dynamic/Kinematic only)");
    }
    if (shape->shape != PhysicsShapeKind::Sphere &&
        shape->shape != PhysicsShapeKind::Box &&
        shape->shape != PhysicsShapeKind::ConvexHull &&
        shape->shape != PhysicsShapeKind::StaticTriMesh)
    {
        return T4Fail(err, uuid, name,
                      "has an unknown shape kind "
                      "(Sphere/Box/ConvexHull/StaticTriMesh only)");
    }
    // Coherent trigger authority: Static ghosts bake once, Kinematic ghosts
    // push before the step; Dynamic triggers are refused (massless ghosts
    // are never simulated, so no authority exists for them).
    if (shape->isTrigger && body.kind == PhysicsBodyKind::Dynamic)
    {
        return T4Fail(err, uuid, name,
                      "is a dynamic trigger (ghost triggers are never "
                      "simulated; use a Static or Kinematic host)");
    }

    // T4 units contract: Dynamic requires mass > 0 (kilograms);
    // Static/Kinematic require mass == 0. Inertia is computed only for
    // positive mass, after the uniform scale is applied.
    const bool isDynamic = body.kind == PhysicsBodyKind::Dynamic;
    if (isDynamic)
    {
        if (!(body.mass > 0.0f))
        {
            return T4Fail(err, uuid, name,
                          "is Dynamic with mass " + std::to_string(body.mass) +
                              " (Dynamic requires mass > 0 kg)");
        }
    }
    else if (body.mass != 0.0f)
    {
        return T4Fail(err, uuid, name,
                      "is non-dynamic with mass " + std::to_string(body.mass) +
                          " (Static/Kinematic require mass == 0)");
    }
    if (body.friction < 0.0f)
        return T4Fail(err, uuid, name, "has negative friction");
    if (body.restitution < 0.0f || body.restitution > 1.0f)
        return T4Fail(err, uuid, name, "has restitution outside [0,1]");
    if (body.linearDamping < 0.0f || body.angularDamping < 0.0f)
        return T4Fail(err, uuid, name, "has negative damping");
    if (body.ccdEnabled &&
        (!(body.ccdMotionThreshold > 0.0f) ||
         !(body.ccdSweptRadius > 0.0f)))
    {
        return T4Fail(err, uuid, name,
                      "has CCD enabled with a non-positive motion threshold or "
                      "swept radius (world units, post-scale)");
    }

    // Single scale owner re-check at build (T3 validated the authoring doc;
    // this guards the runtime clone the candidate actually stages from).
    const glm::vec3& s = tf->scale;
    if (!std::isfinite(s.x) || !std::isfinite(s.y) || !std::isfinite(s.z) ||
        s.x <= 0.0f || s.y <= 0.0f || s.z <= 0.0f || s.x != s.y || s.y != s.z)
    {
        return T4FailCode(err, Error::InvalidTransform, uuid, name,
                          "has a non-uniform, non-positive, or non-finite "
                          "physics scale (single uniform-positive world scale "
                          "required)");
    }
    const float scale = s.x;

    // Safe-margin rule (review P2): Bullet preserves a box's outer dimensions
    // by subtracting the margin from its implicit (margin-free) support, so
    // a margin at or above the smallest final scaled half-extent inverts the
    // core and invalidates GJK. Margins must be strictly below it (zero is
    // always safe: every staged half-extent is positive).
    if (shape->shape == PhysicsShapeKind::Box)
    {
        const float minHalf =
            std::min({shape->halfExtents.x, shape->halfExtents.y,
                      shape->halfExtents.z}) *
            scale;
        if (!(shape->collisionMargin < minHalf))
        {
            return T4Fail(err, uuid, name,
                          "has a box margin at or above the smallest final "
                          "scaled half-extent (margin must be strictly smaller)");
        }
    }

    // Shape staging (world units, post-scale). One shape per entity.
    std::unique_ptr<btCollisionShape> owned;
    const CollisionGeometry* collision = nullptr;
    switch (shape->shape)
    {
        case PhysicsShapeKind::Sphere:
        {
            if (!(shape->radius > 0.0f) || !std::isfinite(shape->radius))
                return T4Fail(err, uuid, name, "has a non-positive sphere radius");
            owned = std::make_unique<btSphereShape>(shape->radius * scale);
            break;
        }
        case PhysicsShapeKind::Box:
        {
            const glm::vec3& h = shape->halfExtents;
            if (!std::isfinite(h.x) || !std::isfinite(h.y) ||
                !std::isfinite(h.z) || h.x <= 0.0f || h.y <= 0.0f ||
                h.z <= 0.0f)
            {
                return T4Fail(err, uuid, name,
                              "has non-positive box half-extents");
            }
            owned = std::make_unique<btBoxShape>(
                btVector3(h.x * scale, h.y * scale, h.z * scale));
            break;
        }
        case PhysicsShapeKind::ConvexHull:
        {
            if (shape->hull.path.empty())
            {
                return T4FailCode(err, Error::MissingAsset, uuid, name,
                                  "physicsShape.hull has an empty path "
                                  "(convex hulls reference authored simplified hull "
                                  "assets, never the render mesh)");
            }
            if (provider == nullptr)
            {
                return T4FailCode(err, Error::MissingAsset, uuid, name,
                                  "physicsShape.hull references '" +
                                      shape->hull.path +
                                      "' but no collision provider was injected");
            }
            Result<const CollisionGeometry*> got =
                const_cast<IPhysicsCollisionAssetProvider*>(provider)
                    ->GetCollisionGeometry(shape->hull, uuid, name);
            if (!got.IsOk())
            {
                err = got.error;
                return false;
            }
            if (!ValidateProviderPayload(got.value, uuid, name,
                                         "physicsShape.hull", err))
                return false;
            // Equivalent hull rule: the margin must stay strictly below the
            // smallest final scaled AABB half-extent of the decoded hull.
            if (!(shape->collisionMargin <
                  CollisionMinHalf(*got.value) * scale))
            {
                return T4Fail(err, uuid, name,
                              "has a convex-hull margin at or above the "
                              "smallest final scaled half-extent (margin must "
                              "be strictly smaller)");
            }
            collision = got.value;
            const size_t points = collision->vertices.size() / 3;
            if (points < 4)
            {
                return T4FailCode(err, Error::Parse, uuid, name,
                                  "convex hull geometry has fewer than 4 points");
            }
            auto* hull = new btConvexHullShape();
            for (size_t i = 0; i < points; ++i)
            {
                hull->addPoint(btVector3(collision->vertices[i * 3 + 0] * scale,
                                         collision->vertices[i * 3 + 1] * scale,
                                         collision->vertices[i * 3 + 2] * scale));
            }
            hull->recalcLocalAabb();
            owned.reset(hull);
            break;
        }
        case PhysicsShapeKind::StaticTriMesh:
        {
            // Bullet restriction: never a dynamic (or kinematic) triangle
            // mesh — static bodies only.
            if (body.kind != PhysicsBodyKind::Static)
            {
                return T4Fail(err, uuid, name,
                              "is a non-static body with a StaticTriMesh shape "
                              "(dynamic/kinematic triangle mesh is refused)");
            }
            if (shape->triMesh.path.empty())
            {
                return T4FailCode(err, Error::MissingAsset, uuid, name,
                                  "physicsShape.triMesh has an empty path "
                                  "(static triangle meshes reference authored "
                                  "collision-only geometry)");
            }
            if (provider == nullptr)
            {
                return T4FailCode(err, Error::MissingAsset, uuid, name,
                                  "physicsShape.triMesh references '" +
                                      shape->triMesh.path +
                                      "' but no collision provider was injected");
            }
            Result<const CollisionGeometry*> got =
                const_cast<IPhysicsCollisionAssetProvider*>(provider)
                    ->GetCollisionGeometry(shape->triMesh, uuid, name);
            if (!got.IsOk())
            {
                err = got.error;
                return false;
            }
            if (!ValidateProviderPayload(got.value, uuid, name,
                                         "physicsShape.triMesh", err))
                return false;
            collision = got.value;
            if (collision->indices.size() % 3 != 0 ||
                collision->indices.empty())
            {
                return T4FailCode(err, Error::Parse, uuid, name,
                                  "static triangle mesh has no triangles");
            }
            auto mesh = std::make_unique<btTriangleMesh>();
            const size_t tris = collision->indices.size() / 3;
            for (size_t t = 0; t < tris; ++t)
            {
                btVector3 v[3];
                for (int k = 0; k < 3; ++k)
                {
                    const uint32_t vi = collision->indices[t * 3 + (size_t)k];
                    v[k] = btVector3(
                        collision->vertices[(size_t)vi * 3 + 0] * scale,
                        collision->vertices[(size_t)vi * 3 + 1] * scale,
                        collision->vertices[(size_t)vi * 3 + 2] * scale);
                }
                mesh->addTriangle(v[0], v[1], v[2]);
            }
            auto* triShape =
                new btBvhTriangleMeshShape(mesh.get(), true);
            world.m_TriangleMeshes.push_back(std::move(mesh));
            owned.reset(triShape);
            break;
        }
        default:
        {
            // Defensive: the enum boundary above makes this unreachable, but
            // the switch must never fall through with a null shape (which
            // would then be dereferenced by setMargin below).
            return T4Fail(err, uuid, name,
                          "has an unknown shape kind "
                          "(Sphere/Box/ConvexHull/StaticTriMesh only)");
        }
    }

    // Margin range hygiene in final world units, post-scale (accepted for
    // every shape, including spheres, for uniformity).
    if (!std::isfinite(shape->collisionMargin) ||
        shape->collisionMargin < 0.0f || shape->collisionMargin > 1.0f)
    {
        return T4Fail(err, uuid, name,
                      "has a collision margin outside [0,1] world units");
    }
    // Sphere policy (review P2): pinned Bullet's btSphereShape::getMargin
    // returns its radius regardless of the base-class value, so an
    // independent sphere margin has no Bullet meaning — spheres skip
    // setMargin (radius IS the margin) while every other shape applies the
    // authored value. The field stays accepted for uniformity and is labeled
    // accordingly in the Inspector.
    if (shape->shape != PhysicsShapeKind::Sphere)
        owned->setMargin(shape->collisionMargin);

    glm::vec3 origin;
    glm::quat rotation;
    DecomposeUniform(tf->worldMatrix, scale, origin, rotation);
    const btTransform start = ToBtTransform(origin, rotation);
    btCollisionShape* rawShape = owned.get();
    world.m_Shapes.push_back(std::move(owned));

    PhysicsBodyRecord rec;
    rec.id = uuid;
    rec.entity = entity;
    rec.kind = body.kind;
    rec.isTrigger = shape->isTrigger;
    rec.bakedScale = scale;
    rec.shape = rawShape;
    // T5 safe-point removal needs the mesh entry: StaticTriMesh bodies own
    // one btTriangleMesh in m_TriangleMeshes (pushed just above for this
    // shape), so removal erases the matching entry instead of leaking it.
    if (shape->shape == PhysicsShapeKind::StaticTriMesh &&
        !world.m_TriangleMeshes.empty())
        rec.triangleMesh = world.m_TriangleMeshes.back().get();

    const int group = (int)body.layer;
    const int mask = (int)body.mask;
    if (shape->isTrigger)
    {
        // Ghost trigger: overlaps, gives no response. Follows its host body
        // kind for sync (kinematic ghosts push, static ghosts bake).
        auto ghost = std::make_unique<btPairCachingGhostObject>();
        ghost->setCollisionShape(rawShape);
        ghost->setWorldTransform(start);
        ghost->setCollisionFlags(ghost->getCollisionFlags() |
                                 btCollisionObject::CF_NO_CONTACT_RESPONSE);
        world.m_World.addCollisionObject(ghost.get(), group, mask);
        rec.ghost = ghost.get();
        world.m_Ghosts.push_back(std::move(ghost));
    }
    else
    {
        btVector3 inertia(0, 0, 0);
        if (isDynamic)
            rawShape->calculateLocalInertia(body.mass, inertia);
        auto motion = std::make_unique<btDefaultMotionState>(start);
        btRigidBody::btRigidBodyConstructionInfo info(
            isDynamic ? body.mass : 0.0f, motion.get(), rawShape, inertia);
        auto rigid = std::make_unique<btRigidBody>(info);
        rigid->setFriction(body.friction);
        rigid->setRestitution(body.restitution);
        rigid->setDamping(body.linearDamping, body.angularDamping);
        if (body.ccdEnabled)
        {
            rigid->setCcdMotionThreshold(body.ccdMotionThreshold);
            rigid->setCcdSweptSphereRadius(body.ccdSweptRadius);
        }
        if (body.kind == PhysicsBodyKind::Kinematic)
        {
            rigid->setCollisionFlags(rigid->getCollisionFlags() |
                                     btCollisionObject::CF_KINEMATIC_OBJECT);
            rigid->setActivationState(DISABLE_DEACTIVATION);
        }
        else if (body.startAsleep && isDynamic)
        {
            rigid->setActivationState(ISLAND_SLEEPING);
        }
        world.m_World.addRigidBody(rigid.get(), group, mask);
        rec.body = rigid.get();
        rec.motion = motion.get();
        world.m_MotionStates.push_back(std::move(motion));
        world.m_Bodies.push_back(std::move(rigid));
    }
    world.m_BodyIndex.push_back(rec);
    return true;
}

bool PhysicsWorld::StageBodies(PhysicsWorld& world,
                               const SceneDocument& runtime,
                               const IPhysicsCollisionAssetProvider* provider,
                               Error& err)
{
    const auto& registry = runtime.ecs.registry;
    std::vector<std::pair<UUID, entt::entity>> ordered;
    {
        auto view = registry.view<PhysicsBodyComponent, EntityIdComponent>();
        for (auto e : view)
        {
            const UUID id = view.get<EntityIdComponent>(e).id;
            if (!id.IsNull())
                ordered.emplace_back(id, e);
        }
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    // Build order: statics, then kinematics, then dynamics, then ghosts
    // (triggers); UUID order within each group. Constraints (T5) build last.
    // Deterministic injection hook for the allocation boundary (tests only).
    if (s_StagingTestThrow)
        throw std::bad_alloc();
    for (int pass = 0; pass < 4; ++pass)
    {
        for (const auto& [uuid, entity] : ordered)
        {
            const auto& body = registry.get<PhysicsBodyComponent>(entity);
            const auto* shape = registry.try_get<PhysicsShapeComponent>(entity);
            const bool isTrigger = shape != nullptr && shape->isTrigger;
            int want = -1;
            if (isTrigger)
                want = 3;
            else if (body.kind == PhysicsBodyKind::Static)
                want = 0;
            else if (body.kind == PhysicsBodyKind::Kinematic)
                want = 1;
            else
                want = 2;
            if (want != pass)
                continue;
            if (!StageOneBody(world, registry, uuid, entity, provider, err))
                return false;
        }
    }
    return true;
}

// ============================================================================
// StageConstraints — T5 driven hinge/slider construction
// ============================================================================
//
// Build order: every hinge/slider from the runtime clone, stable owner-UUID
// order across both kinds, after all bodies/ghosts. Owner is the component
// entity (must stage to a rigid body — Dynamic or Kinematic, never a ghost);
// otherBody is another rigid body UUID or nil = world/static anchor.
//
// Frame resolution (exact, once at build, no teleport):
//   - Hinge body-body: Bullet's pivot/axis constructor consumes the authored
//     owner-local pivot+axis and other-local pivot+axis verbatim (axes
//     normalized here; Bullet builds the frames).
//   - Hinge world anchor: the single-body Bullet frame consumes the authored
//     owner-local pivot+axis. The authored world otherPivot/otherAxis are the
//     same frame expressed in world space and must agree with that owner frame
//     at Play construction. Refusing disagreement is deliberate: selecting
//     just one frame would silently discard authored data and move the anchor.
//   - Slider body-body: frameA is the owner-local frame (origin at the owner
//     body center, X along the owner-local axis); frameB is the same world
//     frame expressed in the other body's local space, so both frames
//     coincide at build and the initial linear position is zero.
//   - Slider world anchor: the single-body frame is the owner-local frame
//     above (joint centered on the owner body at build).
//
// Drive (applied at build when motorEnabled, re-applied by the C++ drive
// entry points below):
//   - Hinge: enableAngularMotor(motorTargetVelocity, motorMaxImpulse) with
//     the authored limits. Release cuts the motor (free swing in limits);
//     return drives back toward restAngle with the staged impulse cap.
//   - Slider: powered linear motor toward targetPosition with the
//     proportional speed law v = clamp(kGain * (target - current),
//     +/-motorTargetVelocity) capped by motorMaxForce, so larger
//     displacements map monotonically to higher speeds. Release cuts the
//     motor and applies an axis impulse; limits still bind afterwards.
// Bodies joined by an enabled motor are set DISABLE_DEACTIVATION (spike
// precedent: a sleeping body would swallow the drive).
//
// Validation at staging (defense in depth behind ValidatePhysicsForPlay and
// the authoring APIs, all UUID-named): owner/other stage to rigid bodies,
// finite pivots/axes/limits/motor params, driveMode == 0, hinge min <= max,
// slider lower <= upper, slider target and hinge rest inside limits.

namespace {

bool T5FiniteVec3(const glm::vec3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

btVector3 ToBt(const glm::vec3& v) { return btVector3(v.x, v.y, v.z); }
glm::vec3 FromBt(const btVector3& v)
{
    return glm::vec3(v.x(), v.y(), v.z());
}

// Uniform Play-time scale of a constraint endpoint (bodies are roots, so
// the local scale IS the world scale). Non-uniform/non-positive/non-finite
// scales refuse loudly — frames cannot resolve without one scale owner.
bool T5UniformScale(const entt::registry& registry, entt::entity entity,
                    const UUID& owner, const std::string& name, Error& err,
                    float& scaleOut)
{
    const auto* tf = registry.try_get<Transform>(entity);
    if (tf == nullptr)
        return T4FailCode(err, Error::InvalidTransform, owner, name,
                          "constraint endpoint has no Transform "
                          "(frames resolve from the Play-time world transform)");
    const glm::vec3& s = tf->scale;
    if (!T5FiniteVec3(s) || s.x <= 0.0f || s.x != s.y || s.y != s.z)
        return T4FailCode(err, Error::InvalidTransform, owner, name,
                          "constraint endpoint has a non-uniform, "
                          "non-positive, or non-finite scale (frames need one "
                          "uniform scale)");
    scaleOut = s.x;
    return true;
}

// Owner world transform (translation + rotation) with the uniform scale
// stripped. Bodies are roots, so the refreshed world matrix is exact.
bool T5OwnerWorldFrame(const entt::registry& registry, entt::entity entity,
                       const UUID& owner, const std::string& name, Error& err,
                       glm::vec3& originOut, glm::quat& rotationOut)
{
    const auto* tf = registry.try_get<Transform>(entity);
    if (tf == nullptr)
        return T4FailCode(err, Error::InvalidTransform, owner, name,
                          "constraint owner has no Transform "
                          "(frames resolve from the Play-time world transform)");
    const glm::vec3& s = tf->scale;
    if (!T5FiniteVec3(s) || s.x <= 0.0f || s.x != s.y || s.y != s.z)
        return T4FailCode(err, Error::InvalidTransform, owner, name,
                          "constraint owner has a non-uniform, non-positive, "
                          "or non-finite scale (frames need one uniform scale)");
    DecomposeUniform(tf->worldMatrix, s.x, originOut, rotationOut);
    if (!T5FiniteVec3(originOut) || !std::isfinite(rotationOut.x) ||
        !std::isfinite(rotationOut.y) || !std::isfinite(rotationOut.z) ||
        !std::isfinite(rotationOut.w))
        return T4FailCode(err, Error::InvalidTransform, owner, name,
                          "constraint owner world transform is non-finite");
    return true;
}

// Rigid body staged for a constraint endpoint. Null when the UUID stages to
// a ghost trigger or to no record at all — both refuse loudly (a constraint
// over a ghost has no simulated body to drive).
btRigidBody* T5RigidEndpoint(PhysicsWorld& world, const UUID& id)
{
    const PhysicsBodyRecord* rec = world.FindBody(id);
    if (rec == nullptr || rec->body == nullptr)
        return nullptr;
    return rec->body;
}

// Orthonormal basis with X along the (already normalized) axis.
btMatrix3x3 T5BasisFromX(const btVector3& x)
{
    btVector3 up(0, 1, 0);
    if (std::abs(x.dot(up)) > 0.95f)
        up = btVector3(1, 0, 0);
    const btVector3 z = (x.cross(up)).normalized();
    const btVector3 y = (z.cross(x)).normalized();
    return btMatrix3x3(x.x(), y.x(), z.x(), x.y(), y.y(), z.y(), x.z(), y.z(),
                       z.z());
}

bool T5ValidateHingeValues(const PhysicsHingeComponent& hinge,
                           const UUID& owner, const std::string& name,
                           Error& err)
{
    if (!T5FiniteVec3(hinge.ownerPivot) || !T5FiniteVec3(hinge.otherPivot))
        return T4Fail(err, owner, name,
                      "hinge has a non-finite pivot (frames must be finite)");
    if (!T5FiniteVec3(hinge.ownerAxis) ||
        glm::length(hinge.ownerAxis) <= kT5MinAxisLength ||
        !T5FiniteVec3(hinge.otherAxis) ||
        glm::length(hinge.otherAxis) <= kT5MinAxisLength)
        return T4Fail(err, owner, name,
                      "hinge has a non-finite or zero-length pivot axis "
                      "(singular frame)");
    if (!std::isfinite(hinge.minAngleLimit) ||
        !std::isfinite(hinge.maxAngleLimit) ||
        hinge.minAngleLimit > hinge.maxAngleLimit)
        return T4Fail(err, owner, name,
                      "hinge has an invalid angle limit range "
                      "(finite min <= max required, radians)");
    if (hinge.driveMode != 0)
        return T4Fail(err, owner, name,
                      "hinge has an unknown drive mode (0 = velocity motor)");
    if (!std::isfinite(hinge.motorTargetVelocity) ||
        !std::isfinite(hinge.motorMaxImpulse) || hinge.motorMaxImpulse < 0.0f)
        return T4Fail(err, owner, name,
                      "hinge has non-finite motor params or a negative max "
                      "impulse");
    if (!std::isfinite(hinge.restAngle) || hinge.restAngle < hinge.minAngleLimit ||
        hinge.restAngle > hinge.maxAngleLimit)
        return T4Fail(err, owner, name,
                      "hinge rest angle is non-finite or outside its limits");
    return true;
}

bool T5ValidateSliderValues(const PhysicsSliderComponent& slider,
                            const UUID& owner, const std::string& name,
                            Error& err)
{
    if (!T5FiniteVec3(slider.axis) ||
        glm::length(slider.axis) <= kT5MinAxisLength)
        return T4Fail(err, owner, name,
                      "slider has a non-finite or zero-length axis "
                      "(singular frame)");
    if (!std::isfinite(slider.lowerLimit) || !std::isfinite(slider.upperLimit) ||
        slider.lowerLimit > slider.upperLimit)
        return T4Fail(err, owner, name,
                      "slider has an invalid limit range "
                      "(finite lower <= upper required, world units)");
    if (!std::isfinite(slider.targetPosition) ||
        slider.targetPosition < slider.lowerLimit ||
        slider.targetPosition > slider.upperLimit)
        return T4Fail(err, owner, name,
                      "slider target is non-finite or outside its limits");
    if (!std::isfinite(slider.motorTargetVelocity) ||
        slider.motorTargetVelocity < 0.0f ||
        !std::isfinite(slider.motorMaxForce) || slider.motorMaxForce < 0.0f)
        return T4Fail(err, owner, name,
                      "slider has non-finite or negative motor params");
    return true;
}

} // namespace

const PhysicsConstraintRecord* PhysicsWorld::FindConstraint(
    const UUID& ownerId) const
{
    for (const auto& rec : m_ConstraintIndex)
    {
        if (rec.ownerId == ownerId)
            return &rec;
    }
    return nullptr;
}

static PhysicsConstraintRecord* FindConstraintMut(
    std::vector<PhysicsConstraintRecord>& index, const UUID& ownerId)
{
    for (auto& rec : index)
    {
        if (rec.ownerId == ownerId)
            return &rec;
    }
    return nullptr;
}

std::vector<UUID> PhysicsWorld::ConstraintsForBody(const UUID& bodyId) const
{
    std::vector<UUID> owners;
    for (const auto& rec : m_ConstraintIndex)
    {
        if (rec.ownerId == bodyId || rec.otherId == bodyId)
            owners.push_back(rec.ownerId);
    }
    std::sort(owners.begin(), owners.end());
    return owners;
}

bool PhysicsWorld::BuildHinge(PhysicsWorld& world,
                                 const SceneDocument& runtime,
                                 const UUID& owner, entt::entity entity,
                                 Error& err, BuiltHingeConstraint& out)
{
    if (s_StagingTestThrow)
        throw std::bad_alloc();
    const auto& registry = runtime.ecs.registry;
    const std::string name = T4EntityName(registry, entity);
    const auto& hinge = registry.get<PhysicsHingeComponent>(entity);
    if (!T5ValidateHingeValues(hinge, owner, name, err))
        return false;

    // Single scale owner: authored pivots are unscaled body-local points;
    // the staged Bullet frames carry scaled geometry, so each pivot composes
    // once with its endpoint's uniform Play-time scale (axes are
    // scale-invariant after normalization).
    float ownerScale = 1.0f;
    if (!T5UniformScale(registry, entity, owner, name, err, ownerScale))
        return false;

    btRigidBody* ownerBody = T5RigidEndpoint(world, owner);
    if (ownerBody == nullptr)
        return T4Fail(err, owner, name,
                      "hinge owner stages to no rigid body (constraint owners "
                      "must be non-trigger Dynamic or Kinematic bodies)");
    btRigidBody* otherBody = nullptr;
    float otherScale = 1.0f;
    if (!hinge.otherBody.IsNull())
    {
        otherBody = T5RigidEndpoint(world, hinge.otherBody);
        if (otherBody == nullptr)
            return T4Fail(err, owner, name,
                          "hinge otherBody " + hinge.otherBody.ToString() +
                              " stages to no rigid body (ghost triggers and "
                              "missing bodies cannot anchor a hinge); owner " +
                              owner.ToString());
        const entt::entity otherEntity =
            runtime.FindByUuid(hinge.otherBody);
        if (otherEntity == entt::null ||
            !T5UniformScale(registry, otherEntity, owner, name, err,
                            otherScale))
            return T4Fail(err, owner, name,
                          "hinge otherBody " + hinge.otherBody.ToString() +
                              " has no valid scaled endpoint; owner " +
                              owner.ToString());
    }

    const btVector3 pivotA = ToBt(hinge.ownerPivot) * ownerScale;
    const btVector3 axisA =
        ToBt(glm::normalize(hinge.ownerAxis));
    std::unique_ptr<btHingeConstraint> owned;
    if (otherBody != nullptr)
    {
        const btVector3 pivotB = ToBt(hinge.otherPivot) * otherScale;
        const btVector3 axisB =
            ToBt(glm::normalize(hinge.otherAxis));
        owned = std::make_unique<btHingeConstraint>(
            *ownerBody, *otherBody, pivotA, pivotB, axisA, axisB, false);
    }
    else
    {
        // World anchor: otherPivot is a WORLD position. Resolve it into the
        // owner body frame (which already carries scaled geometry) through
        // the rigid Play-time world transform — pure Bullet math, no scale
        // residue. The hinge axis stays the owner's local axis.
        glm::vec3 origin;
        glm::quat rotation;
        if (!T5OwnerWorldFrame(registry, entity, owner, name, err, origin,
                               rotation))
            return false;
        const btTransform ownerWorld = ToBtTransform(origin, rotation);
        const btVector3 expectedWorldPivot = ownerWorld * pivotA;
        const btVector3 expectedWorldAxis =
            (ownerWorld.getBasis() * axisA).normalized();
        const btVector3 authoredWorldAxis =
            ToBt(glm::normalize(hinge.otherAxis));
        constexpr btScalar kT5WorldFramePositionToleranceSquared =
            btScalar(1.0e-8f);
        constexpr btScalar kT5WorldFrameAxisDotTolerance = btScalar(0.9999f);
        if ((expectedWorldPivot - ToBt(hinge.otherPivot)).length2() >
                kT5WorldFramePositionToleranceSquared ||
            expectedWorldAxis.dot(authoredWorldAxis) <
                kT5WorldFrameAxisDotTolerance)
        {
            return T4Fail(
                err, owner, name,
                "hinge world anchor frame disagrees with owner-local pivot/axis "
                "(world otherPivot/otherAxis must express the same frame)");
        }
        owned = std::make_unique<btHingeConstraint>(*ownerBody, pivotA, axisA,
                                                     false);
    }
    btHingeConstraint* hingePtr = owned.get();
    hingePtr->setLimit(hinge.minAngleLimit, hinge.maxAngleLimit);
    if (hinge.motorEnabled)
    {
        hingePtr->enableAngularMotor(true, hinge.motorTargetVelocity,
                                     hinge.motorMaxImpulse);
    }

    PhysicsConstraintRecord rec;
    rec.ownerId = owner;
    rec.otherId = hinge.otherBody;
    rec.isHinge = true;
    rec.constraint = hingePtr;
    rec.hinge = hingePtr;
    rec.slider = nullptr;
    rec.driveSpeed = hinge.motorTargetVelocity;
    rec.driveMax = hinge.motorMaxImpulse;
    rec.restPosition = hinge.restAngle;
    rec.lowerLimit = hinge.minAngleLimit;
    rec.upperLimit = hinge.maxAngleLimit;
    out.owned = std::move(owned);
    out.rec = rec;
    return true;
}

void PhysicsWorld::CommitBuiltHinge(PhysicsWorld& world,
                                    BuiltHingeConstraint& built)
{
    btHingeConstraint* hingePtr = built.owned.get();
    world.m_World.addConstraint(hingePtr, true);
    if (hingePtr->getEnableAngularMotor())
    {
        if (btRigidBody* ownerBody = T5RigidEndpoint(world, built.rec.ownerId))
            ownerBody->setActivationState(DISABLE_DEACTIVATION);
        if (!built.rec.otherId.IsNull())
        {
            if (btRigidBody* otherBody =
                    T5RigidEndpoint(world, built.rec.otherId))
                otherBody->setActivationState(DISABLE_DEACTIVATION);
        }
    }
    world.m_Constraints.push_back(std::move(built.owned));
    world.m_ConstraintIndex.push_back(built.rec);
}

bool PhysicsWorld::StageOneHinge(PhysicsWorld& world,
                                  const SceneDocument& runtime,
                                  const UUID& owner, entt::entity entity,
                                  Error& err)
{
    BuiltHingeConstraint built;
    if (!BuildHinge(world, runtime, owner, entity, err, built))
        return false;
    CommitBuiltHinge(world, built);
    return true;
}

bool PhysicsWorld::BuildSlider(PhysicsWorld& world,
                                  const SceneDocument& runtime,
                                  const UUID& owner, entt::entity entity,
                                  Error& err, BuiltSliderConstraint& out)
{
    if (s_StagingTestThrow)
        throw std::bad_alloc();
    const auto& registry = runtime.ecs.registry;
    const std::string name = T4EntityName(registry, entity);
    const auto& slider = registry.get<PhysicsSliderComponent>(entity);
    if (!T5ValidateSliderValues(slider, owner, name, err))
        return false;

    btRigidBody* ownerBody = T5RigidEndpoint(world, owner);
    if (ownerBody == nullptr)
        return T4Fail(err, owner, name,
                      "slider owner stages to no rigid body (constraint "
                      "owners must be non-trigger Dynamic or Kinematic "
                      "bodies)");
    btRigidBody* otherBody = nullptr;
    if (!slider.otherBody.IsNull())
    {
        otherBody = T5RigidEndpoint(world, slider.otherBody);
        if (otherBody == nullptr)
            return T4Fail(err, owner, name,
                          "slider otherBody " + slider.otherBody.ToString() +
                              " stages to no rigid body (ghost triggers and "
                              "missing bodies cannot anchor a slider); owner " +
                              owner.ToString());
    }

    const btVector3 axisA =
        ToBt(glm::normalize(slider.axis)).normalized();
    const btTransform frameA(T5BasisFromX(axisA), btVector3(0, 0, 0));
    std::unique_ptr<btSliderConstraint> owned;
    if (otherBody != nullptr)
    {
        // Same world frame in the other body's local space, so both frames
        // coincide at build (initial linear position zero). Pure Bullet
        // math: the owner world frame carries the staged axis, mapped into
        // the other body's local space through its staged world transform
        // (exact; bodies carry uniform scale only, which translations and
        // orthonormal bases are invariant to).
        glm::vec3 origin;
        glm::quat rotation;
        if (!T5OwnerWorldFrame(registry, entity, owner, name, err, origin,
                               rotation))
            return false;
        const btTransform ownerWorld(ToBtTransform(origin, rotation));
        const btVector3 axisWorld = ownerWorld.getBasis() * axisA;
        const btTransform frameWorld(T5BasisFromX(axisWorld),
                                     ownerWorld.getOrigin());
        const btTransform otherWorld = otherBody->getWorldTransform();
        const btTransform frameB = otherWorld.inverse() * frameWorld;
        // Anchor-first body order (spike convention): with the static/other
        // body as A and the owner as B, a positive target velocity drives
        // the owner along +axis and getLinearPos measures travel from the
        // build pose outward. Owner-first order mirrors the sign (proven by
        // a debug probe: the plunger traveled -X for a +X target).
        owned = std::make_unique<btSliderConstraint>(*otherBody, *ownerBody,
                                                     frameB, frameA, true);
    }
    else
    {
        owned = std::make_unique<btSliderConstraint>(*ownerBody, frameA, true);
    }
    btSliderConstraint* sliderPtr = owned.get();
    sliderPtr->setLowerLinLimit(slider.lowerLimit);
    sliderPtr->setUpperLinLimit(slider.upperLimit);
    if (slider.motorEnabled)
    {
        sliderPtr->setMaxLinMotorForce(slider.motorMaxForce);
        const float current = sliderPtr->getLinearPos();
        const float delta = slider.targetPosition - current;
        float speed = kT5SliderTargetGain * delta;
        speed = std::clamp(speed, -slider.motorTargetVelocity,
                           slider.motorTargetVelocity);
        sliderPtr->setTargetLinMotorVelocity(speed);
        sliderPtr->setPoweredLinMotor(true);
    }

    PhysicsConstraintRecord rec;
    rec.ownerId = owner;
    rec.otherId = slider.otherBody;
    rec.isHinge = false;
    rec.constraint = sliderPtr;
    rec.hinge = nullptr;
    rec.slider = sliderPtr;
    rec.driveSpeed = slider.motorTargetVelocity;
    rec.driveMax = slider.motorMaxForce;
    rec.restPosition = slider.targetPosition;
    rec.lowerLimit = slider.lowerLimit;
    rec.upperLimit = slider.upperLimit;
    // Staged local axis (normalized) for the release-impulse direction.
    rec.localAxis = FromBt(axisA);
    out.owned = std::move(owned);
    out.rec = rec;
    return true;
}

void PhysicsWorld::CommitBuiltSlider(PhysicsWorld& world,
                                     BuiltSliderConstraint& built)
{
    btSliderConstraint* sliderPtr = built.owned.get();
    world.m_World.addConstraint(sliderPtr, true);
    if (sliderPtr->getPoweredLinMotor())
    {
        if (btRigidBody* ownerBody = T5RigidEndpoint(world, built.rec.ownerId))
            ownerBody->setActivationState(DISABLE_DEACTIVATION);
        if (!built.rec.otherId.IsNull())
        {
            if (btRigidBody* otherBody =
                    T5RigidEndpoint(world, built.rec.otherId))
                otherBody->setActivationState(DISABLE_DEACTIVATION);
        }
    }
    world.m_Constraints.push_back(std::move(built.owned));
    world.m_ConstraintIndex.push_back(built.rec);
}

bool PhysicsWorld::StageOneSlider(PhysicsWorld& world,
                                   const SceneDocument& runtime,
                                   const UUID& owner, entt::entity entity,
                                   Error& err)
{
    BuiltSliderConstraint built;
    if (!BuildSlider(world, runtime, owner, entity, err, built))
        return false;
    CommitBuiltSlider(world, built);
    return true;
}

bool PhysicsWorld::StageConstraints(PhysicsWorld& world,
                                    const SceneDocument& runtime, Error& err)
{
    const auto& registry = runtime.ecs.registry;
    std::vector<std::pair<UUID, entt::entity>> hinges;
    for (auto e : registry.view<PhysicsHingeComponent>())
    {
        const auto* idc = registry.try_get<EntityIdComponent>(e);
        if (idc != nullptr && !idc->id.IsNull())
            hinges.emplace_back(idc->id, e);
    }
    std::vector<std::pair<UUID, entt::entity>> sliders;
    for (auto e : registry.view<PhysicsSliderComponent>())
    {
        const auto* idc = registry.try_get<EntityIdComponent>(e);
        if (idc != nullptr && !idc->id.IsNull())
            sliders.emplace_back(idc->id, e);
    }
    // Stable UUID order across both kinds: constraints build last as one
    // group, deterministic regardless of component kind.
    std::vector<std::tuple<UUID, entt::entity, bool>> ordered;
    for (const auto& [uuid, entity] : hinges)
        ordered.emplace_back(uuid, entity, true);
    for (const auto& [uuid, entity] : sliders)
        ordered.emplace_back(uuid, entity, false);
    std::sort(ordered.begin(), ordered.end(),
              [](const auto& a, const auto& b) {
                  return std::get<0>(a) < std::get<0>(b);
              });

    if (s_StagingTestThrow)
        throw std::bad_alloc();
    for (const auto& [uuid, entity, isHinge] : ordered)
    {
        const bool ok = isHinge
                            ? StageOneHinge(world, runtime, uuid, entity, err)
                            : StageOneSlider(world, runtime, uuid, entity,
                                             err);
        if (!ok)
            return false;
    }
    return true;
}

bool PhysicsWorld::ConstraintMotorEnabled(
    const PhysicsConstraintRecord& rec) const
{
    if (rec.isHinge)
        return rec.hinge != nullptr && rec.hinge->getEnableAngularMotor();
    return rec.slider != nullptr && rec.slider->getPoweredLinMotor();
}

void PhysicsWorld::WakeConstraintEndpoints(const UUID& owner)
{
    // Enable path: both endpoints activate now and pin
    // DISABLE_DEACTIVATION, exactly matching staged enabled motors, so a
    // sleeping island moves on the immediately following fixed tick.
    const PhysicsConstraintRecord* rec = FindConstraint(owner);
    if (rec == nullptr)
        return;
    auto wake = [this](const UUID& id) {
        const PhysicsBodyRecord* body = FindBody(id);
        if (body != nullptr && body->body != nullptr)
        {
            body->body->activate(true);
            body->body->setActivationState(DISABLE_DEACTIVATION);
        }
    };
    wake(rec->ownerId);
    if (!rec->otherId.IsNull())
        wake(rec->otherId);
}

void PhysicsWorld::RelaxConstraintEndpoints(const UUID& owner)
{
    // Release path: an endpoint no other enabled constraint touches returns
    // to ACTIVE_TAG — awake now, free to sleep later — instead of staying
    // pinned awake forever.
    const PhysicsConstraintRecord* rec = FindConstraint(owner);
    if (rec == nullptr)
        return;
    auto relax = [this](const UUID& id) {
        for (const auto& other : m_ConstraintIndex)
        {
            if ((other.ownerId == id || other.otherId == id) &&
                ConstraintMotorEnabled(other))
                return;
        }
        const PhysicsBodyRecord* body = FindBody(id);
        // Vendored Bullet gate (btCollisionObject.cpp:61-65): plain
        // setActivationState is a no-op while the body sits at
        // DISABLE_DEACTIVATION, so it can never undo the wake pin. The
        // unconditional forceActivationState is the only way back to
        // ACTIVE_TAG (awake now, free to sleep later).
        if (body != nullptr && body->body != nullptr)
            body->body->forceActivationState(ACTIVE_TAG);
    };
    relax(rec->ownerId);
    if (!rec->otherId.IsNull())
        relax(rec->otherId);
}

// ---- T5 drive entry points (immediate; next fixed tick moves) ----

bool PhysicsWorld::SetHingeDrive(const UUID& owner, float velocity,
                                 float maxImpulse)
{
    PhysicsConstraintRecord* rec = FindConstraintMut(m_ConstraintIndex, owner);
    if (rec == nullptr || !rec->isHinge || rec->hinge == nullptr)
        return false;
    if (!std::isfinite(velocity) || !std::isfinite(maxImpulse) ||
        maxImpulse < 0.0f)
        return false;
    rec->hinge->enableAngularMotor(true, velocity, maxImpulse);
    rec->driveSpeed = velocity;
    rec->driveMax = maxImpulse;
    rec->returning = false;
    WakeConstraintEndpoints(owner);
    return true;
}

bool PhysicsWorld::ReleaseHingeDrive(const UUID& owner)
{
    PhysicsConstraintRecord* rec = FindConstraintMut(m_ConstraintIndex, owner);
    if (rec == nullptr || !rec->isHinge || rec->hinge == nullptr)
        return false;
    // Motor off: free swing within limits. Poses are untouched — release
    // never teleports the constrained body.
    rec->hinge->enableAngularMotor(false, 0.0f, 0.0f);
    rec->returning = false;
    RelaxConstraintEndpoints(owner);
    return true;
}

bool PhysicsWorld::ReturnHingeToRest(const UUID& owner)
{
    PhysicsConstraintRecord* rec = FindConstraintMut(m_ConstraintIndex, owner);
    if (rec == nullptr || !rec->isHinge || rec->hinge == nullptr)
        return false;
    const float current = rec->hinge->getHingeAngle();
    const float delta = rec->restPosition - current;
    if (delta == 0.0f)
    {
        rec->hinge->enableAngularMotor(false, 0.0f, 0.0f);
        rec->returning = false;
        RelaxConstraintEndpoints(owner);
        return true;
    }
    float speed = rec->driveSpeed;
    if (!(speed > 0.0f))
        speed = 1.0f; // sane return rate when the staged drive is idle
    const float velocity = delta > 0.0f ? speed : -speed;
    rec->hinge->enableAngularMotor(true, velocity, rec->driveMax);
    // Closed-loop return: the pre-step servo keeps driving toward rest
    // until it disengages at the setpoint (a one-shot velocity alone would
    // orbit past it).
    rec->returning = true;
    WakeConstraintEndpoints(owner);
    return true;
}

float PhysicsWorld::HingeAngle(const UUID& owner, bool& ok)
{
    PhysicsConstraintRecord* rec = FindConstraintMut(m_ConstraintIndex, owner);
    if (rec == nullptr || !rec->isHinge || rec->hinge == nullptr)
    {
        ok = false;
        return 0.0f;
    }
    ok = true;
    return rec->hinge->getHingeAngle();
}

bool PhysicsWorld::HingeMotorEnabled(const UUID& owner, bool& ok)
{
    PhysicsConstraintRecord* rec = FindConstraintMut(m_ConstraintIndex, owner);
    if (rec == nullptr || !rec->isHinge || rec->hinge == nullptr)
    {
        ok = false;
        return false;
    }
    ok = true;
    return rec->hinge->getEnableAngularMotor();
}

bool PhysicsWorld::HingeMotorParams(const UUID& owner, float& velocityOut,
                                    float& maxImpulseOut)
{
    PhysicsConstraintRecord* rec = FindConstraintMut(m_ConstraintIndex, owner);
    if (rec == nullptr || !rec->isHinge || rec->hinge == nullptr)
        return false;
    velocityOut = rec->hinge->getMotorTargetVelocity();
    maxImpulseOut = rec->hinge->getMaxMotorImpulse();
    return true;
}

bool PhysicsWorld::SetSliderTarget(const UUID& owner, float target)
{
    PhysicsConstraintRecord* rec = FindConstraintMut(m_ConstraintIndex, owner);
    if (rec == nullptr || rec->isHinge || rec->slider == nullptr)
        return false;
    // Outside the limits is a loud refusal, never a clamp: the slider must
    // not move outside its limits through this entry point.
    if (!std::isfinite(target) || target < rec->lowerLimit ||
        target > rec->upperLimit)
        return false;
    const float current = rec->slider->getLinearPos();
    float speed = kT5SliderTargetGain * (target - current);
    speed = std::clamp(speed, -rec->driveSpeed, rec->driveSpeed);
    rec->slider->setTargetLinMotorVelocity(speed);
    rec->slider->setMaxLinMotorForce(rec->driveMax);
    rec->slider->setPoweredLinMotor(true);
    rec->restPosition = target;
    WakeConstraintEndpoints(owner);
    return true;
}

bool PhysicsWorld::ReleaseSlider(const UUID& owner, float impulse)
{
    PhysicsConstraintRecord* rec = FindConstraintMut(m_ConstraintIndex, owner);
    if (rec == nullptr || rec->isHinge || rec->slider == nullptr)
        return false;
    if (!std::isfinite(impulse))
        return false;
    rec->slider->setPoweredLinMotor(false);
    const PhysicsBodyRecord* body = FindBody(owner);
    if (body == nullptr || body->body == nullptr)
        return false;
    // Impulse along the staged axis in its current world direction. Limits
    // still bind afterwards; poses are never teleported.
    const btVector3 axisWorld =
        body->body->getWorldTransform().getBasis() * ToBt(rec->localAxis);
    body->body->activate(true);
    body->body->applyCentralImpulse(axisWorld * impulse);
    RelaxConstraintEndpoints(owner);
    return true;
}

float PhysicsWorld::SliderPosition(const UUID& owner, bool& ok) const
{
    const PhysicsConstraintRecord* rec = FindConstraint(owner);
    if (rec == nullptr || rec->isHinge || rec->slider == nullptr)
    {
        ok = false;
        return 0.0f;
    }
    ok = true;
    return rec->slider->getLinearPos();
}

bool PhysicsWorld::SliderMotorEnabled(const UUID& owner, bool& ok) const
{
    const PhysicsConstraintRecord* rec = FindConstraint(owner);
    if (rec == nullptr || rec->isHinge || rec->slider == nullptr)
    {
        ok = false;
        return false;
    }
    ok = true;
    return rec->slider->getPoweredLinMotor();
}

// ---- T5 safe-point teardown participation ----

void PhysicsWorld::RemoveSubtreePhysics(const std::vector<UUID>& uuids)
{
    // Constraints FIRST (every live constraint touching the set, as owner
    // or as otherBody), then bodies. Reversing this strands Bullet
    // constraints over removed bodies.
    for (size_t i = 0; i < m_ConstraintIndex.size();)
    {
        const auto& rec = m_ConstraintIndex[i];
        const bool ownerGone =
            std::find(uuids.begin(), uuids.end(), rec.ownerId) != uuids.end();
        const bool otherGone =
            !rec.otherId.IsNull() &&
            std::find(uuids.begin(), uuids.end(), rec.otherId) != uuids.end();
        if (!ownerGone && !otherGone)
        {
            ++i;
            continue;
        }
        m_World.removeConstraint(rec.constraint);
        if (s_TeardownOrderLog)
            s_TeardownOrder.emplace_back("constraint");
        auto it = m_Constraints.begin();
        for (; it != m_Constraints.end(); ++it)
        {
            if (it->get() == rec.constraint)
                break;
        }
        if (it != m_Constraints.end())
            m_Constraints.erase(it);
        m_ConstraintIndex.erase(m_ConstraintIndex.begin() + (ptrdiff_t)i);
    }
    for (const auto& uuid : uuids)
    {
        Error ignored;
        RemoveRuntimeBody(uuid, ignored);
    }
    // T6: purge the overlap history of removed UUIDs. Without this, a body
    // destroyed while overlapping its ghost would leave a stale pair behind
    // and fabricate a TriggerExit on a later tick for a UUID that no longer
    // resolves.
    for (auto it = m_PrevOverlaps.begin(); it != m_PrevOverlaps.end();)
    {
        if (std::find(uuids.begin(), uuids.end(), it->first) != uuids.end() ||
            std::find(uuids.begin(), uuids.end(), it->second) != uuids.end())
            it = m_PrevOverlaps.erase(it);
        else
            ++it;
    }
}

bool PhysicsWorld::RemoveRuntimeBody(const UUID& bodyId, Error& err)
{
    // Orphan guard: a body still referenced by a live constraint cannot be
    // removed first. Callers remove dependent constraints (or prove the
    // batch rejects them) before bodies — a reversed order fails here with
    // the UUIDs named instead of dangling silently. The index walk allocates;
    // translate exhaustion to the typed Error instead of letting bad_alloc
    // escape this bool+Error API.
    std::vector<UUID> dependents;
    try
    {
        dependents = ConstraintsForBody(bodyId);
    }
    catch (const std::bad_alloc&)
    {
        err.code = Error::Io;
        err.path = bodyId.ToString();
        err.detail = "body removal for " + bodyId.ToString() +
                     " exhausted resources while collecting dependents "
                     "(live set unchanged)";
        return false;
    }
    if (!dependents.empty())
    {
        err.code = Error::InvalidArgument;
        err.path = bodyId.ToString();
        err.detail = "entity " + bodyId.ToString() +
                     " is still referenced by live constraint owner " +
                     dependents.front().ToString() +
                     " (remove dependent constraints before bodies)";
        return false;
    }
    for (size_t i = 0; i < m_BodyIndex.size(); ++i)
    {
        if (!(m_BodyIndex[i].id == bodyId))
            continue;
        PhysicsBodyRecord rec = m_BodyIndex[i];
        if (rec.ghost != nullptr)
        {
            m_World.removeCollisionObject(rec.ghost);
            for (auto it = m_Ghosts.begin(); it != m_Ghosts.end(); ++it)
            {
                if (it->get() == rec.ghost)
                {
                    m_Ghosts.erase(it);
                    break;
                }
            }
        }
        else if (rec.body != nullptr)
        {
            m_World.removeCollisionObject(rec.body);
            for (auto it = m_Bodies.begin(); it != m_Bodies.end(); ++it)
            {
                if (it->get() == rec.body)
                {
                    m_Bodies.erase(it);
                    break;
                }
            }
            for (auto it = m_MotionStates.begin(); it != m_MotionStates.end();
                 ++it)
            {
                if (it->get() == rec.motion)
                {
                    m_MotionStates.erase(it);
                    break;
                }
            }
        }
        if (rec.shape != nullptr)
        {
            for (auto it = m_Shapes.begin(); it != m_Shapes.end(); ++it)
            {
                if (it->get() == rec.shape)
                {
                    m_Shapes.erase(it);
                    break;
                }
            }
        }
        if (rec.triangleMesh != nullptr)
        {
            for (auto it = m_TriangleMeshes.begin();
                 it != m_TriangleMeshes.end(); ++it)
            {
                if (it->get() == rec.triangleMesh)
                {
                    m_TriangleMeshes.erase(it);
                    break;
                }
            }
        }
        m_BodyIndex.erase(m_BodyIndex.begin() + (ptrdiff_t)i);
        if (s_TeardownOrderLog)
            s_TeardownOrder.emplace_back("body");
        err = Error{};
        return true;
    }
    err = Error{};
    return true; // no live body for this UUID: nothing to remove
}

bool PhysicsWorld::RebuildConstraintsForBody(SceneDocument& runtime,
                                             const UUID& bodyId, Error& err)
{
    // Atomic rebuild (T5 review fixup F2, re-review P1): every replacement
    // is fully validated, frame-resolved, and allocated while the live set
    // is untouched, then the live set is removed and the replacements
    // commit. Any failure — values, endpoints, uniform scale, owner world
    // frame, world-anchor agreement, or allocation — leaves the previous
    // constraints live and stepping. The collection walk, the reserve, and
    // the Build loop share one bad_alloc translation boundary so no
    // allocation site escapes this bool+Error API.
    struct BuiltReplacement
    {
        bool isHinge = true;
        BuiltHingeConstraint hinge;
        BuiltSliderConstraint slider;
    };
    std::vector<UUID> affected;
    std::vector<BuiltReplacement> replacements;
    try
    {
        if (s_RebuildAllocThrowPhase == RebuildAllocThrowPhase::Collect)
            throw std::bad_alloc();
        affected = ConstraintsForBody(bodyId);
        if (affected.empty())
        {
            err = Error{};
            return true;
        }
        auto& registry = runtime.ecs.registry;
        for (const auto& owner : affected)
        {
            const entt::entity entity = runtime.FindByUuid(owner);
            if (entity == entt::null || !registry.valid(entity))
            {
                err.code = Error::InvalidEntity;
                err.path = owner.ToString();
                err.detail = "constraint owner " + owner.ToString() +
                             " does not resolve to a live runtime entity "
                             "(rebuild refused; live set unchanged)";
                return false;
            }
            const bool wantHinge = registry.all_of<PhysicsHingeComponent>(entity);
            const bool wantSlider =
                registry.all_of<PhysicsSliderComponent>(entity);
            const PhysicsConstraintRecord* live = FindConstraint(owner);
            const bool liveHinge = live != nullptr && live->isHinge;
            if ((wantHinge == wantSlider) || (wantHinge != liveHinge))
            {
                err.code = Error::InvalidArgument;
                err.path = owner.ToString();
                err.detail = "constraint owner " + owner.ToString() +
                             " changed kind or lost its component "
                             "(rebuild refused; live set unchanged)";
                return false;
            }
            // Dry-run validation of values + endpoints before touching live
            // state. Mirrors the staging checks without constructing.
            const std::string name = T4EntityName(registry, entity);
            if (wantHinge)
            {
                if (!T5ValidateHingeValues(registry.get<PhysicsHingeComponent>(entity),
                                           owner, name, err))
                    return false;
            }
            else
            {
                if (!T5ValidateSliderValues(
                        registry.get<PhysicsSliderComponent>(entity), owner, name,
                        err))
                    return false;
            }
            const auto& comp = wantHinge
                                   ? registry.get<PhysicsHingeComponent>(entity)
                                         .otherBody
                                   : registry.get<PhysicsSliderComponent>(entity)
                                         .otherBody;
            if (!comp.IsNull() && T5RigidEndpoint(*this, comp) == nullptr)
            {
                err.code = Error::InvalidArgument;
                err.path = owner.ToString();
                err.detail = "constraint owner " + owner.ToString() +
                             " references body " + comp.ToString() +
                             " with no live rigid body "
                             "(rebuild refused; live set unchanged)";
                return false;
            }
            if (T5RigidEndpoint(*this, owner) == nullptr)
            {
                err.code = Error::InvalidArgument;
                err.path = owner.ToString();
                err.detail = "constraint owner " + owner.ToString() +
                             " has no live rigid body "
                             "(rebuild refused; live set unchanged)";
                return false;
            }
        }
        // Phase B: build every replacement detached from the world and both
        // indexes. Late frame/scale/anchor failures — and allocation
        // exhaustion, translated to a typed Error — return here with the
        // live set intact.
        if (s_RebuildAllocThrowPhase == RebuildAllocThrowPhase::Reserve)
            throw std::bad_alloc();
        replacements.reserve(affected.size());
        for (const auto& owner : affected)
        {
            const entt::entity entity = runtime.FindByUuid(owner);
            const bool wantHinge =
                registry.all_of<PhysicsHingeComponent>(entity);
            BuiltReplacement replacement;
            replacement.isHinge = wantHinge;
            const bool ok =
                wantHinge
                    ? BuildHinge(*this, runtime, owner, entity, err,
                                 replacement.hinge)
                    : BuildSlider(*this, runtime, owner, entity, err,
                                  replacement.slider);
            if (!ok)
            {
                err.detail += " (rebuild refused; live set unchanged)";
                return false;
            }
            replacements.push_back(std::move(replacement));
        }
    }
    catch (const std::bad_alloc&)
    {
        err.code = Error::Io;
        err.path = bodyId.ToString();
        err.detail = "constraint rebuild for body " + bodyId.ToString() +
                     " exhausted resources during staging "
                     "(rebuild refused; live set unchanged)";
        return false;
    }
    // Phase C: all replacements built — remove the old set (constraints
    // before anything else), then commit the new set from the detached
    // builds. Commit cannot fail.
    for (const auto& owner : affected)
    {
        const PhysicsConstraintRecord* live = FindConstraint(owner);
        m_World.removeConstraint(live->constraint);
        if (s_TeardownOrderLog)
            s_TeardownOrder.emplace_back("constraint");
        for (auto it = m_Constraints.begin(); it != m_Constraints.end(); ++it)
        {
            if (it->get() == live->constraint)
            {
                m_Constraints.erase(it);
                break;
            }
        }
        for (auto it = m_ConstraintIndex.begin(); it != m_ConstraintIndex.end();
             ++it)
        {
            if (it->ownerId == owner)
            {
                m_ConstraintIndex.erase(it);
                break;
            }
        }
    }
    for (auto& replacement : replacements)
    {
        if (replacement.isHinge)
            CommitBuiltHinge(*this, replacement.hinge);
        else
            CommitBuiltSlider(*this, replacement.slider);
    }
    err = Error{};
    return true;
}

void PhysicsWorld::SetTeardownOrderLog(bool enabled)
{
    s_TeardownOrderLog = enabled;
}

std::vector<std::string> PhysicsWorld::TakeTeardownOrderLog()
{
    return s_TeardownOrder;
}

void PhysicsWorld::ClearTeardownOrderLog() { s_TeardownOrder.clear(); }

void PhysicsWorld::SetResetCollectTestThrow(bool fail)
{
    s_ResetCollectTestThrow = fail;
}

bool PhysicsWorld::ResetCollectTestThrow()
{
    return s_ResetCollectTestThrow;
}

// ============================================================================
// ValidatePhysicsForPlay — T3 early invariants
// ============================================================================

namespace {

constexpr uint16_t kKnownPhysicsLayers =
    PhysicsLayer::Dynamic | PhysicsLayer::WorldStatic |
    PhysicsLayer::Mechanism | PhysicsLayer::Trigger;

constexpr float kMinAxisLength = 1e-6f;

bool IsFiniteVec3(const glm::vec3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// UUID-ordered (uuid, entity) pairs for every entity carrying T, so the first
// reported failure is deterministic across runs.
template <typename T>
std::vector<std::pair<UUID, entt::entity>> CollectOrdered(
    const entt::registry& registry)
{
    std::vector<std::pair<UUID, entt::entity>> out;
    auto view = registry.view<T, EntityIdComponent>();
    for (auto e : view)
        out.emplace_back(view.template get<EntityIdComponent>(e).id, e);
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

bool FailWith(Error& err, Error::Code code, const UUID& owner,
              const std::string& detail)
{
    err.code = code;
    err.path = owner.ToString();
    err.detail = detail;
    return false;
}

// Pass-0 identity gate: the component must sit on an entity carrying an
// authored EntityIdComponent. Iterates the bare component view — never
// filtered on identity — so a malformed entity cannot evade the check and
// silently vanish from the runtime clone (which collects from the ID view).
// The path names the component wire (no UUID exists to name); the detail
// identifies the registry entity and the required component.
template <typename T>
bool CheckComponentIds(const entt::registry& registry, const char* wire,
                       Error& err)
{
    for (auto e : registry.view<T>())
    {
        const auto* idc = registry.try_get<EntityIdComponent>(e);
        if (idc != nullptr && !idc->id.IsNull())
            continue;
        err.code = Error::InvalidEntity;
        err.path = wire;
        err.detail = std::string(wire) + " on scene entity " +
                     std::to_string(static_cast<uint32_t>(e)) +
                     " has no authored UUID (EntityIdComponent required; "
                     "the entity would be omitted from the runtime clone)";
        return false;
    }
    return true;
}

} // namespace

bool ValidatePhysicsForPlay(const SceneDocument& doc,
                            const IPhysicsCollisionAssetProvider* provider,
                            Error& err)
{
    err = Error{};
    const auto& registry = doc.ecs.registry;

    // Pass 0: every physics component must carry an authored UUID. Without
    // one no diagnostic can name the entity and the clone collector (which
    // starts from the ID view) would silently omit it — the characteristic
    // swallowed-data failure. Refuse loudly before anything is staged.
    if (!CheckComponentIds<PhysicsBodyComponent>(registry, "PhysicsBodyComponent", err))
        return false;
    if (!CheckComponentIds<PhysicsShapeComponent>(registry, "PhysicsShapeComponent", err))
        return false;
    if (!CheckComponentIds<PhysicsHingeComponent>(registry, "PhysicsHingeComponent", err))
        return false;
    if (!CheckComponentIds<PhysicsSliderComponent>(registry, "PhysicsSliderComponent", err))
        return false;

    // Constraint identity first: dangling or body-less otherBody,
    // self-constraint, and missing owner body refuse here with both UUIDs
    // named (T2 ValidatePhysicsConstraintReferences, reused verbatim).
    if (!ValidatePhysicsConstraintReferences(doc, err))
        return false;

    // Hinge/slider owners must be Dynamic or Kinematic — never Static — and
    // their authored axes must be finite and non-zero. Axes are normalized at
    // T5 build; a singular frame is refused now, never converted silently.
    for (const auto& [owner, entity] : CollectOrdered<PhysicsHingeComponent>(registry))
    {
        const auto& body = registry.get<PhysicsBodyComponent>(entity);
        if (body.kind == PhysicsBodyKind::Static)
        {
            return FailWith(err, Error::InvalidArgument, owner,
                            "PhysicsHingeComponent owner " + owner.ToString() +
                            " is Static (constraint owners must be Dynamic or Kinematic)");
        }
        const auto& hinge = registry.get<PhysicsHingeComponent>(entity);
        if (!IsFiniteVec3(hinge.ownerAxis) ||
            glm::length(hinge.ownerAxis) <= kMinAxisLength ||
            !IsFiniteVec3(hinge.otherAxis) ||
            glm::length(hinge.otherAxis) <= kMinAxisLength)
        {
            return FailWith(err, Error::InvalidArgument, owner,
                            "PhysicsHingeComponent owner " + owner.ToString() +
                            " has a non-finite or zero-length pivot axis (singular frame)");
        }
    }
    for (const auto& [owner, entity] : CollectOrdered<PhysicsSliderComponent>(registry))
    {
        const auto& body = registry.get<PhysicsBodyComponent>(entity);
        if (body.kind == PhysicsBodyKind::Static)
        {
            return FailWith(err, Error::InvalidArgument, owner,
                            "PhysicsSliderComponent owner " + owner.ToString() +
                            " is Static (constraint owners must be Dynamic or Kinematic)");
        }
        const auto& slider = registry.get<PhysicsSliderComponent>(entity);
        if (!IsFiniteVec3(slider.axis) ||
            glm::length(slider.axis) <= kMinAxisLength)
        {
            return FailWith(err, Error::InvalidArgument, owner,
                            "PhysicsSliderComponent owner " + owner.ToString() +
                            " has a non-finite or zero-length axis (singular frame)");
        }
    }

    // T5 constraint value ranges (defense in depth behind the authoring
    // APIs; staging re-checks verbatim). Limit ranges, pivots, and motor
    // params must be finite and ordered; unknown drive modes refuse.
    for (const auto& [owner, entity] : CollectOrdered<PhysicsHingeComponent>(registry))
    {
        const auto& hinge = registry.get<PhysicsHingeComponent>(entity);
        if (!IsFiniteVec3(hinge.ownerPivot) || !IsFiniteVec3(hinge.otherPivot))
        {
            return FailWith(err, Error::InvalidArgument, owner,
                            "PhysicsHingeComponent owner " + owner.ToString() +
                            " has a non-finite pivot (frames must be finite)");
        }
        if (!std::isfinite(hinge.minAngleLimit) ||
            !std::isfinite(hinge.maxAngleLimit) ||
            hinge.minAngleLimit > hinge.maxAngleLimit)
        {
            return FailWith(err, Error::InvalidArgument, owner,
                            "PhysicsHingeComponent owner " + owner.ToString() +
                            " has an invalid angle limit range (finite min <= "
                            "max required, radians)");
        }
        if (hinge.driveMode != 0)
        {
            return FailWith(err, Error::InvalidArgument, owner,
                            "PhysicsHingeComponent owner " + owner.ToString() +
                            " has an unknown drive mode (0 = velocity motor)");
        }
        if (!std::isfinite(hinge.motorTargetVelocity) ||
            !std::isfinite(hinge.motorMaxImpulse) ||
            hinge.motorMaxImpulse < 0.0f)
        {
            return FailWith(err, Error::InvalidArgument, owner,
                            "PhysicsHingeComponent owner " + owner.ToString() +
                            " has non-finite motor params or a negative max "
                            "impulse");
        }
        if (!std::isfinite(hinge.restAngle) ||
            hinge.restAngle < hinge.minAngleLimit ||
            hinge.restAngle > hinge.maxAngleLimit)
        {
            return FailWith(err, Error::InvalidArgument, owner,
                            "PhysicsHingeComponent owner " + owner.ToString() +
                            " has a non-finite rest angle or one outside its "
                            "limits");
        }
    }
    for (const auto& [owner, entity] : CollectOrdered<PhysicsSliderComponent>(registry))
    {
        const auto& slider = registry.get<PhysicsSliderComponent>(entity);
        if (!std::isfinite(slider.lowerLimit) ||
            !std::isfinite(slider.upperLimit) ||
            slider.lowerLimit > slider.upperLimit)
        {
            return FailWith(err, Error::InvalidArgument, owner,
                            "PhysicsSliderComponent owner " +
                            owner.ToString() +
                            " has an invalid limit range (finite lower <= "
                            "upper required, world units)");
        }
        if (!std::isfinite(slider.targetPosition) ||
            slider.targetPosition < slider.lowerLimit ||
            slider.targetPosition > slider.upperLimit)
        {
            return FailWith(err, Error::InvalidArgument, owner,
                            "PhysicsSliderComponent owner " +
                            owner.ToString() +
                            " has a non-finite target or one outside its "
                            "limits");
        }
        if (!std::isfinite(slider.motorTargetVelocity) ||
            slider.motorTargetVelocity < 0.0f ||
            !std::isfinite(slider.motorMaxForce) ||
            slider.motorMaxForce < 0.0f)
        {
            return FailWith(err, Error::InvalidArgument, owner,
                            "PhysicsSliderComponent owner " +
                            owner.ToString() +
                            " has non-finite or negative motor params");
        }
    }

    // T5 review fixup F3: one constraint per owner (hinge XOR slider). The
    // runtime index is owner-keyed, so a co-located pair is unaddressable —
    // refuse before anything stages. Reports the first offender in UUID
    // order for determinism.
    for (const auto& [owner, entity] : CollectOrdered<PhysicsHingeComponent>(registry))
    {
        if (registry.all_of<PhysicsSliderComponent>(entity))
        {
            return FailWith(err, Error::InvalidArgument, owner,
                            "PhysicsHingeComponent owner " + owner.ToString() +
                            " also carries a PhysicsSliderComponent "
                            "(one constraint per owner: hinge XOR slider)");
        }
    }

    // Per-body early invariants, UUID order.
    for (const auto& [uuid, entity] : CollectOrdered<PhysicsBodyComponent>(registry))
    {
        // Contradictory authorities: MotionComponent integrates Transform
        // while a physics body would own the same transform. Never silently
        // ignored, never double-integrated — refuse Play with the UUID named.
        if (registry.all_of<MotionComponent>(entity))
        {
            return FailWith(err, Error::InvalidArgument, uuid,
                            "entity " + uuid.ToString() +
                            " carries both MotionComponent and PhysicsBodyComponent "
                            "(contradictory transform authorities; remove one before Play)");
        }

        // Hierarchy rule: every physics-owned body must be an unparented
        // root. No exact parent-inverse conversion is defined yet; a silent
        // approximate conversion would corrupt hinge/slider frames (T5).
        if (const auto* hierarchy = registry.try_get<Hierarchy>(entity))
        {
            if (hierarchy->parent != entt::null)
            {
                return FailWith(err, Error::InvalidHierarchy, uuid,
                                "entity " + uuid.ToString() +
                                " carries PhysicsBodyComponent but is parented "
                                "(physics bodies must be unparented roots)");
            }
        }

        const auto& body = registry.get<PhysicsBodyComponent>(entity);
        const auto* shape = registry.try_get<PhysicsShapeComponent>(entity);

        // Bullet restriction: never a dynamic triangle mesh.
        if (shape != nullptr && body.kind == PhysicsBodyKind::Dynamic &&
            shape->shape == PhysicsShapeKind::StaticTriMesh)
        {
            return FailWith(err, Error::InvalidArgument, uuid,
                            "entity " + uuid.ToString() +
                            " is a Dynamic body with a StaticTriMesh shape "
                            "(dynamic triangle mesh is refused)");
        }

        // Settled collision policy (review fixup): layer is exactly one
        // known bit — group bitsets are refused, never silently narrowed.
        if (body.layer == 0 || (body.layer & (body.layer - 1)) != 0 ||
            (body.layer & ~kKnownPhysicsLayers) != 0)
        {
            return FailWith(err, Error::InvalidArgument, uuid,
                            "entity " + uuid.ToString() +
                            " has physics layer " + std::to_string(body.layer) +
                            " (layer must be exactly one of Dynamic|WorldStatic|"
                            "Mechanism|Trigger)");
        }

        // Mask is any nonzero subset of the known bits. Zero (collides with
        // nothing) and unknown bits are loud refusals, never silent defaults.
        if (body.mask == 0 || (body.mask & ~kKnownPhysicsLayers) != 0)
        {
            return FailWith(err, Error::InvalidArgument, uuid,
                            "entity " + uuid.ToString() +
                            " has physics mask " + std::to_string(body.mask) +
                            " (mask must be a nonzero subset of Dynamic|"
                            "WorldStatic|Mechanism|Trigger)");
        }

        // Trigger/layer consistency: ghost (trigger) shapes live on the
        // Trigger layer so the solver and the event scrape agree on what is
        // a trigger; solid shapes must not claim the Trigger layer.
        if (shape != nullptr)
        {
            if (shape->isTrigger && body.layer != PhysicsLayer::Trigger)
            {
                return FailWith(err, Error::InvalidArgument, uuid,
                                "entity " + uuid.ToString() +
                                " has a trigger shape on a non-Trigger layer "
                                "(trigger shapes require the Trigger layer)");
            }
            if (!shape->isTrigger && body.layer == PhysicsLayer::Trigger)
            {
                return FailWith(err, Error::InvalidArgument, uuid,
                                "entity " + uuid.ToString() +
                                " has a non-trigger shape on the Trigger layer "
                                "(only trigger shapes may use it)");
            }
        }

        // Single scale owner: the entity's uniform world scale composes once
        // at build. Bodies are roots (proven above), so local scale IS world
        // scale here. Non-uniform, non-positive, or non-finite scale is a
        // loud refusal; mid-Play scale writes are a T4 authority rule.
        const auto* transform = registry.try_get<Transform>(entity);
        if (transform == nullptr)
        {
            return FailWith(err, Error::InvalidTransform, uuid,
                            "entity " + uuid.ToString() +
                            " carries PhysicsBodyComponent but has no Transform "
                            "(bodies are placed from their world transform)");
        }
        const glm::vec3& s = transform->scale;
        if (!IsFiniteVec3(s) || s.x <= 0.0f || s.y <= 0.0f || s.z <= 0.0f ||
            s.x != s.y || s.y != s.z)
        {
            return FailWith(err, Error::InvalidTransform, uuid,
                            "entity " + uuid.ToString() +
                            " has a non-uniform, non-positive, or non-finite "
                            "physics scale (single uniform-positive world scale required)");
        }

        // Active collision refs require a provider. T3 ships the injection
        // seam only (no decoder): refs plus no provider refuse loudly; refs
        // plus a provider proceed with zero handles (T4 decodes them).
        if (shape != nullptr)
        {
            const char* wire = nullptr;
            const std::string* path = nullptr;
            if (shape->shape == PhysicsShapeKind::ConvexHull &&
                !shape->hull.path.empty())
            {
                wire = "physicsShape.hull";
                path = &shape->hull.path;
            }
            else if (shape->shape == PhysicsShapeKind::StaticTriMesh &&
                     !shape->triMesh.path.empty())
            {
                wire = "physicsShape.triMesh";
                path = &shape->triMesh.path;
            }
            if (wire != nullptr && provider == nullptr)
            {
                return FailWith(err, Error::MissingAsset, uuid,
                                "entity " + uuid.ToString() + " " + wire +
                                " references '" + *path +
                                "' but no collision provider was injected "
                                "(Play with physics collision refs and no provider is refused)");
            }
        }
    }

    return true;
}

} // namespace rt2::core
