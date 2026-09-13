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

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace rt2::core {

bool PhysicsWorld::s_TestInjectCreateFailure = false;
bool PhysicsWorld::s_TestPoseProbe = false;
size_t PhysicsWorld::s_LiveWorlds = 0;
std::vector<PhysicsWorld::ConstructionPose> PhysicsWorld::s_RecordedPoses;

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
    // The candidate is FULLY constructed first: broadphase, dispatcher,
    // solver, configuration, dynamics world, ghost-pair callback, gravity.
    // A failure below therefore rolls back a live Bullet world through the
    // complete Shutdown() teardown — never a pre-construction early-out.
    std::unique_ptr<PhysicsWorld> world(new PhysicsWorld());

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

size_t PhysicsWorld::LiveWorldCount()
{
    return s_LiveWorlds;
}

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

bool PhysicsWorld::TestInjectCreateFailure()
{
    return s_TestInjectCreateFailure;
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
