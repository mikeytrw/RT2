#include <btBulletDynamicsCommon.h>
#include <BulletCollision/CollisionDispatch/btGhostObject.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr btScalar kStep = btScalar(1.0 / 240.0);

class World final {
public:
    World()
        : m_Dispatcher(&m_CollisionConfiguration),
          m_World(&m_Dispatcher, &m_Broadphase, &m_Solver, &m_CollisionConfiguration) {
        m_Broadphase.getOverlappingPairCache()->setInternalGhostPairCallback(&m_GhostCallback);
    }

    ~World() {
        for (auto it = m_Constraints.rbegin(); it != m_Constraints.rend(); ++it)
            m_World.removeConstraint(it->get());
        for (auto it = m_Objects.rbegin(); it != m_Objects.rend(); ++it)
            m_World.removeCollisionObject(it->get());
    }

    btDiscreteDynamicsWorld& Get() { return m_World; }

    template <typename Shape, typename... Args>
    Shape* MakeShape(Args&&... args) {
        auto shape = std::make_unique<Shape>(std::forward<Args>(args)...);
        Shape* result = shape.get();
        m_Shapes.push_back(std::move(shape));
        return result;
    }

    btRigidBody* AddBody(btScalar mass, btCollisionShape* shape, const btTransform& transform) {
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

    btPairCachingGhostObject* AddTrigger(btCollisionShape* shape, const btTransform& transform) {
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
    Constraint* AddConstraint(Args&&... args) {
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

btTransform At(btScalar x, btScalar y, btScalar z) {
    btTransform transform;
    transform.setIdentity();
    transform.setOrigin(btVector3(x, y, z));
    return transform;
}

struct Check {
    std::string name;
    bool passed = false;
    std::string evidence;
};

btScalar RunThinWallShot(bool enableCcd) {
    World fixture;
    fixture.Get().setGravity(btVector3(0, 0, 0));
    auto* wallShape = fixture.MakeShape<btBoxShape>(btVector3(0.01f, 1.0f, 1.0f));
    fixture.AddBody(0, wallShape, At(0, 0, 0));
    auto* ballShape = fixture.MakeShape<btSphereShape>(0.05f);
    auto* ball = fixture.AddBody(1, ballShape, At(-1, 0, 0));
    ball->setLinearVelocity(btVector3(240, 0, 0));
    ball->setRestitution(0);
    if (enableCcd) {
        ball->setCcdMotionThreshold(0.01f);
        ball->setCcdSweptSphereRadius(0.045f);
    }
    fixture.Get().stepSimulation(btScalar(1.0 / 60.0), 0);
    return ball->getWorldTransform().getOrigin().x();
}

Check CheckCcdBall() {
    const btScalar discreteX = RunThinWallShot(false);
    const btScalar ccdX = RunThinWallShot(true);
    const bool passed = discreteX > btScalar(0.25) && ccdX < btScalar(0.10);
    return {"ccd_ball", passed,
            "discrete_x=" + std::to_string(discreteX) + ", ccd_x=" + std::to_string(ccdX)};
}

Check CheckMotorizedFlipper() {
    World fixture;
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
        fixture.Get().stepSimulation(kStep, 0);

    const btScalar angle = hinge->getHingeAngle();
    const btScalar pivotError = (flipper->getWorldTransform() * frameB).getOrigin().length();
    const bool passed = angle > SIMD_RADS_PER_DEG * 35.0f &&
                        angle <= SIMD_RADS_PER_DEG * 56.0f && pivotError < 0.02f;
    return {"motorized_flipper", passed,
            "angle_deg=" + std::to_string(angle / SIMD_RADS_PER_DEG) +
                ", pivot_error=" + std::to_string(pivotError)};
}

Check CheckPlungerConstraint() {
    World fixture;
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
        fixture.Get().stepSimulation(kStep, 0);

    const btScalar translation = slider->getLinearPos();
    const btScalar ballX = ball->getWorldTransform().getOrigin().x();
    const btScalar ballSpeed = ball->getLinearVelocity().x();
    const bool passed = translation >= -0.01f && translation <= 0.31f &&
                        ballX > 0.55f && ballSpeed > 0.2f;
    return {"plunger_constraint", passed,
            "translation=" + std::to_string(translation) + ", ball_x=" +
                std::to_string(ballX) + ", ball_vx=" + std::to_string(ballSpeed)};
}

Check CheckRampCollision() {
    auto triangleMesh = std::make_unique<btTriangleMesh>();
    World fixture;
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
    for (int i = 0; i < 300; ++i) {
        fixture.Get().stepSimulation(kStep, 0);
        const int manifoldCount = fixture.Get().getDispatcher()->getNumManifolds();
        for (int m = 0; m < manifoldCount; ++m) {
            const auto* manifold = fixture.Get().getDispatcher()->getManifoldByIndexInternal(m);
            if (manifold->getNumContacts() > 0) {
                contactSeen = true;
                contactHeight = std::max(contactHeight, ball->getWorldTransform().getOrigin().y());
            }
        }
    }
    const btVector3 position = ball->getWorldTransform().getOrigin();
    const bool passed = contactSeen && contactHeight > 0.45f && position.x() < 1.0f;
    return {"ramp_collision", passed,
            "contact=" + std::to_string(contactSeen) + ", contact_height=" +
                std::to_string(contactHeight) + ", final_x=" + std::to_string(position.x())};
}

Check CheckTrigger() {
    World fixture;
    fixture.Get().setGravity(btVector3(0, 0, 0));
    auto* triggerShape = fixture.MakeShape<btBoxShape>(btVector3(0.20f, 0.50f, 0.50f));
    auto* trigger = fixture.AddTrigger(triggerShape, At(0, 0, 0));
    auto* ballShape = fixture.MakeShape<btSphereShape>(0.10f);
    auto* ball = fixture.AddBody(1, ballShape, At(-1, 0, 0));
    ball->setLinearVelocity(btVector3(3, 0, 0));
    ball->setCcdMotionThreshold(0.02f);
    ball->setCcdSweptSphereRadius(0.09f);

    bool overlapSeen = false;
    for (int i = 0; i < 160; ++i) {
        fixture.Get().stepSimulation(kStep, 0);
        overlapSeen = overlapSeen || trigger->getNumOverlappingObjects() > 0;
    }
    const btScalar ballX = ball->getWorldTransform().getOrigin().x();
    const bool passed = overlapSeen && ballX > 0.50f;
    return {"trigger", passed,
            "overlap=" + std::to_string(overlapSeen) + ", ball_x=" + std::to_string(ballX)};
}

} // namespace

int main() {
    std::vector<Check> checks;
    checks.push_back(CheckCcdBall());
    checks.push_back(CheckMotorizedFlipper());
    checks.push_back(CheckPlungerConstraint());
    checks.push_back(CheckRampCollision());
    checks.push_back(CheckTrigger());

    bool allPassed = true;
    std::cout << "RT2 core-only Bullet pinball spike\n";
    std::cout << "bullet_revision=3.25@2c204c49e56ed15ec5fcfa71d199ab6d6570b3f5\n";
    std::cout << "linked_modules=LinearMath,BulletCollision,BulletDynamics\n";
    for (const Check& check : checks) {
        std::cout << check.name << '=' << (check.passed ? "PASS" : "FAIL")
                  << " (" << check.evidence << ")\n";
        allPassed = allPassed && check.passed;
    }
    std::cout << "overall=" << (allPassed ? "PASS" : "FAIL") << '\n';
    return allPassed ? 0 : 1;
}
