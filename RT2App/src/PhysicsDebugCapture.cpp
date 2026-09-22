// CPU-only T8 capture: drawer ownership + persisted-constraint adapter.
// No Vulkan/ImGui/Walnut. Links into RT2Tests/RT2SliceRunner.
#include "PhysicsDebugCapture.h"

#include "ECSComponents.h"
#include "PhysicsComponents.h"
#include "SceneDocument.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <vector>

namespace rt2::core {

bool PhysicsDebugDrawer::s_TestThrowOnNextLine = false;

void PhysicsDebugDrawer::SetTestThrowOnNextLine(bool enabled)
{
    s_TestThrowOnNextLine = enabled;
}

bool PhysicsDebugDrawer::TestThrowOnNextLine()
{
    return s_TestThrowOnNextLine;
}

void PhysicsDebugDrawer::drawLine(const btVector3& from, const btVector3& to,
                                  const btVector3& /*color*/)
{
    if (m_Out == nullptr || !m_HasOwner)
        return;
    if (s_TestThrowOnNextLine)
    {
        s_TestThrowOnNextLine = false;
        throw std::bad_alloc();
    }
    PhysicsDebugSegment seg;
    seg.owner = m_Owner;
    seg.kind = m_Kind;
    seg.a = glm::vec3(from.x(), from.y(), from.z());
    seg.b = glm::vec3(to.x(), to.y(), to.z());
    if (!std::isfinite(seg.a.x) || !std::isfinite(seg.a.y) ||
        !std::isfinite(seg.a.z) || !std::isfinite(seg.b.x) ||
        !std::isfinite(seg.b.y) || !std::isfinite(seg.b.z))
        return;
    m_Out->segments.push_back(seg);
}

namespace {

const Transform* T8FindTransform(const SceneDocument& runtime, const UUID& owner)
{
    const entt::entity e = runtime.FindByUuid(owner);
    if (e == entt::null)
        return nullptr;
    return runtime.ecs.registry.try_get<Transform>(e);
}

glm::vec3 T8WorldPoint(const glm::mat4& world, const glm::vec3& local)
{ return glm::vec3(world * glm::vec4(local, 1.0f)); }

glm::vec3 T8WorldDirection(const glm::mat4& world, const glm::vec3& local)
{ return glm::mat3(world) * local; }

glm::vec3 T8SafeAxis(glm::vec3 v, bool& ok)
{
    ok = std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    const float len = ok ? std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z) : 0.0f;
    if (!ok || !(len > 1e-6f) || !std::isfinite(len))
    {
        ok = false;
        return glm::vec3(1.0f, 0.0f, 0.0f);
    }
    ok = true;
    return v / len;
}

// Small pivot cross so a constraint with a degenerate axis is still visible.
void T8EmitCross(PhysicsDebugLines& out, const UUID& owner,
                 const glm::vec3& p, float half)
{
    const glm::vec3 dx(half, 0.0f, 0.0f);
    const glm::vec3 dy(0.0f, half, 0.0f);
    const glm::vec3 dz(0.0f, 0.0f, half);
    PhysicsDebugSegment sx, sy, sz;
    sx.owner = sy.owner = sz.owner = owner;
    sx.kind = sy.kind = sz.kind = PhysicsDebugLineKind::Constraint;
    sx.a = p - dx; sx.b = p + dx;
    sy.a = p - dy; sy.b = p + dy;
    sz.a = p - dz; sz.b = p + dz;
    out.segments.push_back(sx);
    out.segments.push_back(sy);
    out.segments.push_back(sz);
}

} // namespace

void AppendConstraintAdapterLines(const SceneDocument& runtime,
                                  PhysicsDebugLines& out)
{
    // UUID order within each component group; hinges before sliders, so the
    // merged constraint block is deterministic regardless of registry order.
    std::vector<std::pair<UUID, entt::entity>> hinges;
    for (auto e : runtime.ecs.registry.view<PhysicsHingeComponent>())
    {
        const auto* idc = runtime.ecs.registry.try_get<EntityIdComponent>(e);
        if (idc != nullptr && !idc->id.IsNull())
            hinges.emplace_back(idc->id, e);
    }
    std::sort(hinges.begin(), hinges.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<std::pair<UUID, entt::entity>> sliders;
    for (auto e : runtime.ecs.registry.view<PhysicsSliderComponent>())
    {
        const auto* idc = runtime.ecs.registry.try_get<EntityIdComponent>(e);
        if (idc != nullptr && !idc->id.IsNull())
            sliders.emplace_back(idc->id, e);
    }
    std::sort(sliders.begin(), sliders.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    constexpr float kAxisLen = 0.5f;
    constexpr float kCrossHalf = 0.08f;

    for (const auto& [owner, entity] : hinges)
    {
        const auto& hinge = runtime.ecs.registry.get<PhysicsHingeComponent>(entity);
        const Transform* ownerTf = T8FindTransform(runtime, owner);
        if (ownerTf == nullptr)
            continue;
        const glm::vec3 base = T8WorldPoint(ownerTf->worldMatrix, hinge.ownerPivot);
        T8EmitCross(out, owner, base, kCrossHalf);
        bool ok = false;
        const glm::vec3 axis = T8SafeAxis(
            T8WorldDirection(ownerTf->worldMatrix, hinge.ownerAxis), ok);
        if (ok)
        {
            PhysicsDebugSegment seg;
            seg.owner = owner;
            seg.kind = PhysicsDebugLineKind::Constraint;
            seg.a = base;
            seg.b = base + axis * kAxisLen;
            out.segments.push_back(seg);
        }
        // World/other anchor tick: when otherBody names a live body, draw the
        // authored other-pivot at that body's world position so a hinge
        // spanning two bodies shows both ends. Empty (world anchor) draws at
        // the authored other-pivot verbatim — the copy-path contract preserves
        // world anchors verbatim until re-authored.
        glm::vec3 otherPivot = hinge.otherPivot;
        glm::vec3 otherAxis = hinge.otherAxis;
        if (!hinge.otherBody.IsNull())
        {
            const Transform* otherTf = T8FindTransform(runtime, hinge.otherBody);
            if (otherTf == nullptr)
                continue;
            otherPivot = T8WorldPoint(otherTf->worldMatrix, hinge.otherPivot);
            otherAxis = T8WorldDirection(otherTf->worldMatrix, hinge.otherAxis);
        }
        // The nil other-body frame is authored in world space; a non-null
        // one is transformed from its body's local frame above.
        T8EmitCross(out, owner, otherPivot, kCrossHalf * 0.75f);
        const glm::vec3 resolvedOtherAxis = T8SafeAxis(otherAxis, ok);
        if (ok)
            out.segments.push_back({ owner, PhysicsDebugLineKind::Constraint,
                                     otherPivot, otherPivot + resolvedOtherAxis * kAxisLen });
    }

    for (const auto& [owner, entity] : sliders)
    {
        const auto& slider = runtime.ecs.registry.get<PhysicsSliderComponent>(entity);
        const Transform* ownerTf = T8FindTransform(runtime, owner);
        if (ownerTf == nullptr)
            continue;
        // Sliders persist no pivots: owner origin is its frame. A non-null
        // otherBody is shown at its implicit local-origin frame below; nil
        // has no stored world point this adapter may invent.
        const glm::vec3 base = T8WorldPoint(ownerTf->worldMatrix, glm::vec3(0.0f));
        T8EmitCross(out, owner, base, kCrossHalf);
        bool ok = false;
        const glm::vec3 axis = T8SafeAxis(
            T8WorldDirection(ownerTf->worldMatrix, slider.axis), ok);
        if (ok)
        {
            const float lo = std::isfinite(slider.lowerLimit) ? slider.lowerLimit : 0.0f;
            const float hi = std::isfinite(slider.upperLimit) ? slider.upperLimit : kAxisLen;
            const float end = (hi > lo && std::isfinite(hi - lo)) ? hi : lo + kAxisLen;
            PhysicsDebugSegment seg;
            seg.owner = owner;
            seg.kind = PhysicsDebugLineKind::Constraint;
            seg.a = base + axis * lo;
            seg.b = base + axis * end;
            out.segments.push_back(seg);
        }
        if (!slider.otherBody.IsNull())
        {
            const Transform* otherTf = T8FindTransform(runtime, slider.otherBody);
            if (otherTf != nullptr)
                T8EmitCross(out, owner,
                    T8WorldPoint(otherTf->worldMatrix, glm::vec3(0.0f)),
                    kCrossHalf * 0.75f);
        }
    }
}

} // namespace rt2::core
