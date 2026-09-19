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
#include "PhysicsDebugCapture.h"
#include "SceneDocument.h"
#include "SceneGraph.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <new>

#include <glm/gtc/quaternion.hpp>

namespace rt2::core {

bool PhysicsWorld::s_TestInjectCreateFailure = false;
bool PhysicsWorld::s_TestPoseProbe = false;
bool PhysicsWorld::s_StagingTestThrow = false;
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
    // empty in T3; the loops are real so T4 bodies ride the same order.
    for (auto it = m_Constraints.rbegin(); it != m_Constraints.rend(); ++it)
        m_World.removeConstraint(it->get());
    m_Constraints.clear();
    for (auto it = m_Ghosts.rbegin(); it != m_Ghosts.rend(); ++it)
        m_World.removeCollisionObject(it->get());
    m_Ghosts.clear();
    for (auto it = m_Bodies.rbegin(); it != m_Bodies.rend(); ++it)
        m_World.removeCollisionObject(it->get());
    m_Bodies.clear();
    m_MotionStates.clear();
    m_Shapes.clear();
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
// T8 debug capture (member definitions at rt2::core scope)
// ============================================================================

bool PhysicsWorld::CaptureDebugLines(const SceneDocument* runtime,
                                     PhysicsDebugLines& out, Error& err)
{
    // Capture must not publish partial geometry, and Bullet must never retain
    // a pointer to the stack drawer if vector growth throws. The guard restores
    // the exact prior drawer (normally null) before `drawer` is destroyed.
    struct DrawerRestore final
    {
        btDiscreteDynamicsWorld& world;
        btIDebugDraw* previous;
        DrawerRestore(btDiscreteDynamicsWorld& w, btIDebugDraw& installed)
            : world(w), previous(w.getDebugDrawer()) { world.setDebugDrawer(&installed); }
        ~DrawerRestore() { world.setDebugDrawer(previous); }
    };

    try
    {
        PhysicsDebugLines captured;
        PhysicsDebugDrawer drawer;
        drawer.setDebugMode(btIDebugDraw::DBG_DrawWireframe);
        drawer.BeginCapture(&captured);
        DrawerRestore restore(m_World, drawer);
        for (const auto& rec : m_BodyIndex)
        {
            PhysicsDebugLineKind kind = PhysicsDebugLineKind::Static;
            if (rec.isTrigger)
            kind = PhysicsDebugLineKind::Trigger;
            else if (rec.kind == PhysicsBodyKind::Dynamic)
            kind = PhysicsDebugLineKind::Dynamic;
            else if (rec.kind == PhysicsBodyKind::Kinematic)
            kind = PhysicsDebugLineKind::Kinematic;

            const btTransform* t = nullptr;
            if (rec.body != nullptr)
            t = &rec.body->getWorldTransform();
            else if (rec.ghost != nullptr)
            t = &rec.ghost->getWorldTransform();
            if (t == nullptr || rec.shape == nullptr)
                continue;
            drawer.SetOwner(rec.id, kind);
            m_World.debugDrawObject(*t, rec.shape, btVector3(1.0f, 1.0f, 1.0f));
        }
    // Persisted hinge/slider adapter (T5-independent): consumes already-read
    // runtime components, builds no Bullet constraints. See the T5 merge
    // point in PhysicsDebugCapture.h.
        if (runtime != nullptr)
            AppendConstraintAdapterLines(*runtime, captured);
        captured.SortStable();
        out.segments.swap(captured.segments); // no-throw publication
        err = {};
        return true;
    }
    catch (const std::bad_alloc&)
    {
        err.code = Error::Io;
        err.path.clear();
        err.detail = "PhysicsWorld::CaptureDebugLines allocation failure; previous snapshot retained";
        return false;
    }
}

// ============================================================================
// T4 sync + introspection (member definitions at rt2::core scope)
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
