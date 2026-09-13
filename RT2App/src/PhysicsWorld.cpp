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

PhysicsWorld::PhysicsWorld()
    : m_Dispatcher(&m_CollisionConfiguration)
    , m_World(&m_Dispatcher, &m_Broadphase, &m_Solver,
              &m_CollisionConfiguration)
{
    // Plan units: Y-up, 1 unit = 1 metre, world gravity (0,-9.81,0).
    m_World.setGravity(btVector3(0.0f, -9.81f, 0.0f));
    m_Broadphase.getOverlappingPairCache()->setInternalGhostPairCallback(
        &m_GhostCallback);
}

PhysicsWorld::~PhysicsWorld()
{
    Shutdown();
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

Result<std::unique_ptr<PhysicsWorld>> PhysicsWorld::Create()
{
    // Late-failure probe FIRST: no Bullet object is staged, so the candidate
    // fails before any observable construction. The controller's rollback
    // (destroy candidate, reset clone, Edit state, zero accumulator, no
    // bridge call, no script callback) is what RED_PhysicsPlayConstructionIsAtomic
    // observes.
    if (s_TestInjectCreateFailure)
    {
        return Result<std::unique_ptr<PhysicsWorld>>::Fail(
            Error::InvalidRuntimeState, "",
            "PhysicsWorld::Create test-injected candidate failure "
            "(atomicity probe; no world staged)");
    }

    std::unique_ptr<PhysicsWorld> world(new PhysicsWorld());
    return Result<std::unique_ptr<PhysicsWorld>>::Ok(std::move(world));
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

} // namespace

bool ValidatePhysicsForPlay(const SceneDocument& doc,
                            const IPhysicsCollisionAssetProvider* provider,
                            Error& err)
{
    err = Error{};
    const auto& registry = doc.ecs.registry;

    // Every physics entity must carry an authored UUID; without one no
    // diagnostic can name it, so refuse loudly rather than skipping.
    for (const auto& [uuid, entity] : CollectOrdered<PhysicsBodyComponent>(registry))
    {
        if (uuid.IsNull())
        {
            err.code = Error::InvalidEntity;
            err.detail =
                "physics body without authored UUID (cannot persist or diagnose)";
            return false;
        }
        (void)entity;
    }

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

        // Layers/masks: the four plan layers only (uint16 pair). A zero layer
        // or any unknown bit is a loud refusal, never a silent default.
        if (body.layer == 0 || (body.layer & ~kKnownPhysicsLayers) != 0 ||
            (body.mask & ~kKnownPhysicsLayers) != 0)
        {
            return FailWith(err, Error::InvalidArgument, uuid,
                            "entity " + uuid.ToString() +
                            " has an invalid physics layer/mask pair "
                            "(layers Dynamic|WorldStatic|Mechanism|Trigger only)");
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
