// CPU-only T8 capture: drawer ownership + persisted-constraint adapter.
// No Vulkan/ImGui/Walnut. Links into RT2Tests/RT2SliceRunner.
#include "PhysicsDebugCapture.h"

#include "ECSComponents.h"
#include "PhysicsComponents.h"
#include "SceneDocument.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace rt2::core {

void PhysicsDebugDrawer::drawLine(const btVector3& from, const btVector3& to,
                                  const btVector3& /*color*/)
{
    if (m_Out == nullptr || !m_HasOwner)
        return;
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

glm::vec3 T8OwnerWorldPos(const SceneDocument& runtime, const UUID& owner)
{
    const entt::entity e = runtime.FindByUuid(owner);
    if (e == entt::null)
        return glm::vec3(0.0f);
    const auto* tf = runtime.ecs.registry.try_get<Transform>(e);
    if (tf == nullptr)
        return glm::vec3(0.0f);
    return glm::vec3(tf->worldMatrix[3]);
}

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
        const glm::vec3 base = T8OwnerWorldPos(runtime, owner) + hinge.ownerPivot;
        T8EmitCross(out, owner, base, kCrossHalf);
        bool ok = false;
        const glm::vec3 axis = T8SafeAxis(hinge.ownerAxis, ok);
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
        if (!hinge.otherBody.IsNull())
        {
            const glm::vec3 other = T8OwnerWorldPos(runtime, hinge.otherBody) +
                                    hinge.otherPivot;
            T8EmitCross(out, owner, other, kCrossHalf * 0.75f);
        }
    }

    for (const auto& [owner, entity] : sliders)
    {
        const auto& slider = runtime.ecs.registry.get<PhysicsSliderComponent>(entity);
        const glm::vec3 base = T8OwnerWorldPos(runtime, owner);
        T8EmitCross(out, owner, base, kCrossHalf);
        bool ok = false;
        const glm::vec3 axis = T8SafeAxis(slider.axis, ok);
        if (ok)
        {
            const float lo = std::isfinite(slider.lowerLimit) ? slider.lowerLimit : 0.0f;
            const float hi = std::isfinite(slider.upperLimit) ? slider.upperLimit : kAxisLen;
            const float span = (hi > lo && std::isfinite(hi - lo)) ? (hi - lo) : kAxisLen;
            PhysicsDebugSegment seg;
            seg.owner = owner;
            seg.kind = PhysicsDebugLineKind::Constraint;
            seg.a = base;
            seg.b = base + axis * span;
            out.segments.push_back(seg);
        }
    }
}

} // namespace rt2::core
