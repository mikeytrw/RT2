// ============================================================================
// T1: Pinned Bullet core vendoring + build-isolation smoke tests.
//
// Links the three vendored StaticLib projects (LinearMath, BulletCollision,
// BulletDynamics) from a CPU-only test target and re-proves the five spike
// mechanics against the vendored revision. Deliberately introduces no RT2
// PhysicsWorld behavior: bodies, constraints and worlds here are local test
// fixtures only.
//
// Boundaries pinned by this file:
// - RED_NoVulkanInPhysicsIncludes: including the Bullet headers must not pull
//   in Vulkan, ImGui or Walnut. Enforced at compile time below (same pattern
//   as CpuBoundaryTests.cpp); a leak fails the build, not just a case.
// - GREEN_Cpp17TargetsBuild: the Bullet projects and this consumer compile as
//   C++17 (static_assert below; premake sets cppdialect C++17 on all four).
// - Link anchors: the fixtures reference btVector3 (LinearMath),
//   btBoxShape (BulletCollision) and btDiscreteDynamicsWorld (BulletDynamics),
//   so removing any of the three explicit consumer links fails the link.
// - Pin identity: the vendored revision must read back as the pinned commit
//   with intact upstream zlib bytes (fails loudly on re-pin drift or a
//   wildcard change that shadows these sources).
//
// Mechanics ports (tools/BulletPinballProbe/main.cpp @ spike 2a60816):
// fast CCD sphere, motorized hinge flipper, driven slider plunger, static
// triangle-mesh ramp, non-blocking ghost trigger. Parameters and pass bands
// are the spike's verbatim regression anchors.
// ============================================================================

#include <doctest/doctest.h>

#include <btBulletDynamicsCommon.h>
#include <BulletCollision/CollisionDispatch/btGhostObject.h>

#ifdef IMGUI_VERSION
#error "T1 boundary: Bullet physics headers must not import ImGui"
#endif

#ifdef VK_HEADER_VERSION
#error "T1 boundary: Bullet physics headers must not import Vulkan"
#endif

#if __has_include("imgui.h")
#error "T1 boundary: imgui.h must not be reachable from physics test units"
#endif

#if __has_include("vulkan/vulkan.h")
#error "T1 boundary: vulkan headers must not be reachable from physics test units"
#endif

#if __has_include("Walnut/Application.h")
#error "T1 boundary: Walnut headers must not be reachable from physics test units"
#endif

// MSVC reports __cplusplus as 199711L unless /Zc:__cplusplus is set (RT2 does
// not set it); _MSVC_LANG carries the actual language mode there.
#if defined(_MSC_VER)
static_assert(_MSVC_LANG >= 201703L, "T1 GREEN_Cpp17TargetsBuild: Bullet consumers build as C++17");
#else
static_assert(__cplusplus >= 201703L, "T1 GREEN_Cpp17TargetsBuild: Bullet consumers build as C++17");
#endif

#include <algorithm>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

// Spike-verbatim world harness: owns Bullet objects for one fixture and tears
// down constraints before bodies before the world (Bullet requirement).
class VendoredBulletWorld final {
public:
    VendoredBulletWorld()
        : m_Dispatcher(&m_CollisionConfiguration)
        , m_World(&m_Dispatcher, &m_Broadphase, &m_Solver, &m_CollisionConfiguration)
    {
        m_Broadphase.getOverlappingPairCache()->setInternalGhostPairCallback(&m_GhostCallback);
    }

    ~VendoredBulletWorld()
    {
        for (auto it = m_Constraints.rbegin(); it != m_Constraints.rend(); ++it)
            m_World.removeConstraint(it->get());
        for (auto it = m_Objects.rbegin(); it != m_Objects.rend(); ++it)
            m_World.removeCollisionObject(it->get());
    }

    btDiscreteDynamicsWorld& Get() { return m_World; }

    template <typename Shape, typename... Args>
    Shape* MakeShape(Args&&... args)
    {
        auto shape = std::make_unique<Shape>(std::forward<Args>(args)...);
        Shape* result = shape.get();
        m_Shapes.push_back(std::move(shape));
        return result;
    }

    btRigidBody* AddBody(btScalar mass, btCollisionShape* shape, const btTransform& transform)
    {
        btVector3 inertia(0, 0, 0);
        if (mass > 0)
            shape->calculateLocalInertia(mass, inertia);

        auto motionState = std::make_unique<btDefaultMotionState>(transform);
        btRigidBody::btRigidBodyConstructionInfo info(mass, motionState.get(), shape, inertia);
        auto body = std::make_unique<btRigidBody>(info);
        btRigidBody* result = body.get();
        m_World.addRigidBody(result);
        m_MotionStates.push_back(std::move(motionState));
        m_Objects.push_back(std::move(body));
        return result;
    }

    btPairCachingGhostObject* AddTrigger(btCollisionShape* shape, const btTransform& transform)
    {
        auto ghost = std::make_unique<btPairCachingGhostObject>();
        ghost->setWorldTransform(transform);
        ghost->setCollisionShape(shape);
        ghost->setCollisionFlags(ghost->getCollisionFlags() | btCollisionObject::CF_NO_CONTACT_RESPONSE);
        btPairCachingGhostObject* result = ghost.get();
        m_World.addCollisionObject(result);
        m_Objects.push_back(std::move(ghost));
        return result;
    }

    template <typename Constraint, typename... Args>
    Constraint* AddConstraint(Args&&... args)
    {
        auto constraint = std::make_unique<Constraint>(std::forward<Args>(args)...);
        Constraint* result = constraint.get();
        m_World.addConstraint(result, true);
        m_Constraints.push_back(std::move(constraint));
        return result;
    }

private:
    btDefaultCollisionConfiguration m_CollisionConfiguration;
    btCollisionDispatcher m_Dispatcher;
    btDbvtBroadphase m_Broadphase;
    btSequentialImpulseConstraintSolver m_Solver;
    btDiscreteDynamicsWorld m_World;
    btGhostPairCallback m_GhostCallback;
    std::vector<std::unique_ptr<btCollisionShape>> m_Shapes;
    std::vector<std::unique_ptr<btMotionState>> m_MotionStates;
    std::vector<std::unique_ptr<btCollisionObject>> m_Objects;
    std::vector<std::unique_ptr<btTypedConstraint>> m_Constraints;
};

btTransform At(btScalar x, btScalar y, btScalar z)
{
    btTransform transform;
    transform.setIdentity();
    transform.setOrigin(btVector3(x, y, z));
    return transform;
}

constexpr btScalar kProbeStep = btScalar(1.0 / 240.0);

btScalar RunThinWallShot(bool enableCcd)
{
    VendoredBulletWorld fixture;
    fixture.Get().setGravity(btVector3(0, 0, 0));
    auto* wallShape = fixture.MakeShape<btBoxShape>(btVector3(0.01f, 1.0f, 1.0f));
    fixture.AddBody(0, wallShape, At(0, 0, 0));
    auto* ballShape = fixture.MakeShape<btSphereShape>(0.05f);
    auto* ball = fixture.AddBody(1, ballShape, At(-1, 0, 0));
    ball->setLinearVelocity(btVector3(240, 0, 0));
    ball->setRestitution(0);
    if (enableCcd)
    {
        ball->setCcdMotionThreshold(0.01f);
        ball->setCcdSweptSphereRadius(0.045f);
    }
    fixture.Get().stepSimulation(btScalar(1.0 / 60.0), 0);
    return ball->getWorldTransform().getOrigin().x();
}

std::string ReadVendoredFile(const char* relativePath, bool& found)
{
    // RT2Tests runs from the repository root (AGENTS.md); the vendored pin
    // files resolve by relative path from there.
    std::ifstream in(relativePath, std::ios::binary);
    found = in.good();
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

TEST_CASE("T1 RED_NoVulkanInPhysicsIncludes: physics test unit stays CPU-only")
{
    // The boundary is enforced at compile time by the guards above; this case
    // exists so the guarantee is visible in test listings and counts.
    CHECK(true);
}

TEST_CASE("T1 pin identity: vendored Bullet reads back as 3.25 @ 2c204c49 with zlib bytes")
{
    bool pinFound = false;
    const std::string pin = ReadVendoredFile("RT2App/vendor/bullet/README.md", pinFound);
    REQUIRE_MESSAGE(pinFound, "Run RT2Tests from the repository root; RT2App/vendor/bullet/README.md not found");
    CHECK_MESSAGE(pin.find("2c204c49e56ed15ec5fcfa71d199ab6d6570b3f5") != std::string::npos,
        "Vendored Bullet pin drifted: expected commit 2c204c49e56ed15ec5fcfa71d199ab6d6570b3f5 in pin README");

    bool licenseFound = false;
    const std::string license = ReadVendoredFile("RT2App/vendor/bullet/LICENSE.txt", licenseFound);
    REQUIRE_MESSAGE(licenseFound, "RT2App/vendor/bullet/LICENSE.txt missing: upstream license notice must be preserved verbatim");
    CHECK_MESSAGE(license.find("zlib") != std::string::npos,
        "Vendored LICENSE.txt does not carry the upstream zlib notice");

    bool premakeFound = false;
    const std::string premake = ReadVendoredFile("RT2App/vendor/bullet/premake5.lua", premakeFound);
    REQUIRE_MESSAGE(premakeFound, "RT2App/vendor/bullet/premake5.lua missing");
    for (const char* project : { "project \"LinearMath\"", "project \"BulletCollision\"", "project \"BulletDynamics\"" })
    {
        CHECK_MESSAGE(premake.find(project) != std::string::npos,
            "Expected exactly the three Bullet library projects; missing ", project);
    }
    CHECK_MESSAGE(premake.find("StaticLib") != std::string::npos, "Bullet projects must be StaticLib");
}

TEST_CASE("T1 smoke: fast CCD sphere stops at a thin wall while discrete tunnels")
{
    const btScalar discreteX = RunThinWallShot(false);
    const btScalar ccdX = RunThinWallShot(true);
    CHECK_MESSAGE(discreteX > btScalar(0.25), "Discrete step tunneled less than the spike anchor, discrete_x=", discreteX);
    CHECK_MESSAGE(ccdX < btScalar(0.10), "CCD failed to stop the fast sphere, ccd_x=", ccdX);
}

TEST_CASE("T1 smoke: motorized hinge flipper reaches its limit without pivot drift")
{
    VendoredBulletWorld fixture;
    fixture.Get().setGravity(btVector3(0, 0, 0));
    auto* anchorShape = fixture.MakeShape<btBoxShape>(btVector3(0.05f, 0.05f, 0.05f));
    auto* flipperShape = fixture.MakeShape<btBoxShape>(btVector3(0.50f, 0.06f, 0.10f));
    auto* anchor = fixture.AddBody(0, anchorShape, At(0, 0, 0));
    auto* flipper = fixture.AddBody(1, flipperShape, At(0.50f, 0, 0));
    flipper->setActivationState(DISABLE_DEACTIVATION);

    btTransform frameA = At(0, 0, 0);
    btTransform frameB = At(-0.50f, 0, 0);
    auto* hinge = fixture.AddConstraint<btHingeConstraint>(*anchor, *flipper, frameA, frameB, true);
    hinge->setLimit(0, SIMD_RADS_PER_DEG * 55.0f);
    hinge->enableAngularMotor(true, 18.0f, 8.0f);
    for (int i = 0; i < 60; ++i)
        fixture.Get().stepSimulation(kProbeStep, 0);

    const btScalar angle = hinge->getHingeAngle();
    const btScalar pivotError = (flipper->getWorldTransform() * frameB).getOrigin().length();
    CHECK_MESSAGE(angle > SIMD_RADS_PER_DEG * 35.0f, "Hinge motor stalled, angle_deg=", angle / SIMD_RADS_PER_DEG);
    CHECK_MESSAGE(angle <= SIMD_RADS_PER_DEG * 56.0f, "Hinge blew past its limit, angle_deg=", angle / SIMD_RADS_PER_DEG);
    CHECK_MESSAGE(pivotError < 0.02f, "Hinge pivot drifted, pivot_error=", pivotError);
}

TEST_CASE("T1 smoke: driven slider plunger launches its ball")
{
    VendoredBulletWorld fixture;
    fixture.Get().setGravity(btVector3(0, 0, 0));
    auto* anchorShape = fixture.MakeShape<btBoxShape>(btVector3(0.05f, 0.05f, 0.05f));
    auto* plungerShape = fixture.MakeShape<btBoxShape>(btVector3(0.12f, 0.12f, 0.12f));
    auto* ballShape = fixture.MakeShape<btSphereShape>(0.10f);
    auto* anchor = fixture.AddBody(0, anchorShape, At(0, 0, 0));
    auto* plunger = fixture.AddBody(2, plungerShape, At(0, 0, 0));
    auto* ball = fixture.AddBody(1, ballShape, At(0.34f, 0, 0));
    plunger->setActivationState(DISABLE_DEACTIVATION);

    btTransform frameA = At(0, 0, 0);
    btTransform frameB = At(0, 0, 0);
    auto* slider = fixture.AddConstraint<btSliderConstraint>(*anchor, *plunger, frameA, frameB, true);
    slider->setLowerLinLimit(0);
    slider->setUpperLinLimit(0.30f);
    slider->setPoweredLinMotor(true);
    slider->setTargetLinMotorVelocity(8.0f);
    slider->setMaxLinMotorForce(80.0f);
    for (int i = 0; i < 120; ++i)
        fixture.Get().stepSimulation(kProbeStep, 0);

    const btScalar translation = slider->getLinearPos();
    const btScalar ballX = ball->getWorldTransform().getOrigin().x();
    const btScalar ballSpeed = ball->getLinearVelocity().x();
    CHECK_MESSAGE(translation >= -0.01f, "Slider below its travel, translation=", translation);
    CHECK_MESSAGE(translation <= 0.31f, "Slider past its travel, translation=", translation);
    CHECK_MESSAGE(ballX > 0.55f, "Plunger failed to move the ball, ball_x=", ballX);
    CHECK_MESSAGE(ballSpeed > 0.2f, "Plunger transferred no impulse, ball_vx=", ballSpeed);
}

TEST_CASE("T1 smoke: static triangle-mesh ramp deflects a falling ball")
{
    auto triangleMesh = std::make_unique<btTriangleMesh>();
    VendoredBulletWorld fixture;
    fixture.Get().setGravity(btVector3(0, -9.81f, 0));

    triangleMesh->addTriangle(btVector3(-1, 0, -1), btVector3(2, 0.75f, -1), btVector3(2, 0.75f, 1));
    triangleMesh->addTriangle(btVector3(-1, 0, -1), btVector3(2, 0.75f, 1), btVector3(-1, 0, 1));
    auto* rampShape = fixture.MakeShape<btBvhTriangleMeshShape>(triangleMesh.get(), true);
    fixture.AddBody(0, rampShape, At(0, 0, 0));

    auto* ballShape = fixture.MakeShape<btSphereShape>(0.12f);
    auto* ball = fixture.AddBody(1, ballShape, At(1.5f, 1.3f, 0));
    ball->setCcdMotionThreshold(0.02f);
    ball->setCcdSweptSphereRadius(0.10f);
    ball->setFriction(0.4f);

    bool contactSeen = false;
    btScalar contactHeight = 0;
    for (int i = 0; i < 300; ++i)
    {
        fixture.Get().stepSimulation(kProbeStep, 0);
        const int manifoldCount = fixture.Get().getDispatcher()->getNumManifolds();
        for (int m = 0; m < manifoldCount; ++m)
        {
            const auto* manifold = fixture.Get().getDispatcher()->getManifoldByIndexInternal(m);
            if (manifold->getNumContacts() > 0)
            {
                contactSeen = true;
                contactHeight = std::max(contactHeight, ball->getWorldTransform().getOrigin().y());
            }
        }
    }
    const btVector3 position = ball->getWorldTransform().getOrigin();
    CHECK_MESSAGE(contactSeen, "Ball never touched the static triangle ramp");
    CHECK_MESSAGE(contactHeight > 0.45f, "Ramp contact happened too low, contact_height=", contactHeight);
    CHECK_MESSAGE(position.x() < 1.0f, "Ramp failed to deflect the ball, final_x=", position.x());
}

TEST_CASE("T1 smoke: ghost trigger overlaps without blocking")
{
    VendoredBulletWorld fixture;
    fixture.Get().setGravity(btVector3(0, 0, 0));
    auto* triggerShape = fixture.MakeShape<btBoxShape>(btVector3(0.20f, 0.50f, 0.50f));
    auto* trigger = fixture.AddTrigger(triggerShape, At(0, 0, 0));
    auto* ballShape = fixture.MakeShape<btSphereShape>(0.10f);
    auto* ball = fixture.AddBody(1, ballShape, At(-1, 0, 0));
    ball->setLinearVelocity(btVector3(3, 0, 0));
    ball->setCcdMotionThreshold(0.02f);
    ball->setCcdSweptSphereRadius(0.09f);

    bool overlapSeen = false;
    for (int i = 0; i < 160; ++i)
    {
        fixture.Get().stepSimulation(kProbeStep, 0);
        overlapSeen = overlapSeen || trigger->getNumOverlappingObjects() > 0;
    }
    const btScalar ballX = ball->getWorldTransform().getOrigin().x();
    CHECK_MESSAGE(overlapSeen, "Ghost trigger never reported the passing ball");
    CHECK_MESSAGE(ballX > 0.50f, "Trigger blocked the ball, ball_x=", ballX);
}
