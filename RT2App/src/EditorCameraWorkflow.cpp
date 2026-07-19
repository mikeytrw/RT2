#include "EditorCameraWorkflow.h"

#include "ECSComponents.h"
#include "ISceneRenderBridge.h"
#include "SceneGraph.h"
#include "SceneHierarchy.h"
#include "TransformEditing.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <glm/gtx/quaternion.hpp>

namespace
{
constexpr float kEpsilon = 1e-6f;
constexpr float kFallbackHalfExtent = 0.25f;

bool IsFinite(float value)
{
    return std::isfinite(value);
}

bool IsFinite(const glm::vec3& value)
{
    return IsFinite(value.x) && IsFinite(value.y) && IsFinite(value.z);
}

bool TryNormalize(const glm::vec3& value, glm::vec3& normalized)
{
    if (!IsFinite(value)) return false;
    const float lengthSquared = glm::dot(value, value);
    if (!IsFinite(lengthSquared) || lengthSquared <= kEpsilon * kEpsilon)
        return false;
    normalized = value * glm::inversesqrt(lengthSquared);
    return IsFinite(normalized);
}

void IncludePoint(EditorSelectionBounds& bounds, const glm::vec3& point)
{
    if (!IsFinite(point)) return;
    if (!bounds.valid)
    {
        bounds.minimum = point;
        bounds.maximum = point;
        bounds.valid = true;
        return;
    }
    bounds.minimum = glm::min(bounds.minimum, point);
    bounds.maximum = glm::max(bounds.maximum, point);
}

void IncludeFallback(EditorSelectionBounds& bounds, const glm::vec3& centre)
{
    const glm::vec3 extent(kFallbackHalfExtent);
    IncludePoint(bounds, centre - extent);
    IncludePoint(bounds, centre + extent);
}

void IncludeMeshBounds(EditorSelectionBounds& bounds, const MeshData& mesh,
                       const glm::mat4& world)
{
    for (int x = 0; x < 2; ++x)
        for (int y = 0; y < 2; ++y)
            for (int z = 0; z < 2; ++z)
            {
                const glm::vec3 local(
                    x ? mesh.boundsMax.x : mesh.boundsMin.x,
                    y ? mesh.boundsMax.y : mesh.boundsMin.y,
                    z ? mesh.boundsMax.z : mesh.boundsMin.z);
                const glm::vec4 transformed = world * glm::vec4(local, 1.0f);
                if (IsFinite(transformed.w) && std::abs(transformed.w) > kEpsilon)
                    IncludePoint(bounds, glm::vec3(transformed) / transformed.w);
            }
}

bool BuildCameraBasis(const glm::vec3& requestedForward,
                      glm::vec3& forward, glm::vec3& right, glm::vec3& up)
{
    if (!TryNormalize(requestedForward, forward))
        return false;
    constexpr glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
    glm::vec3 rightCandidate = glm::cross(forward, worldUp);
    if (!TryNormalize(rightCandidate, right))
    {
        rightCandidate = glm::cross(forward, glm::vec3(0.0f, 0.0f, 1.0f));
        if (!TryNormalize(rightCandidate, right))
            return false;
    }
    return TryNormalize(glm::cross(right, forward), up);
}
}

bool EditorSelectionBounds::Contains(const glm::vec3& point) const
{
    if (!valid || !IsFinite(point)) return false;
    return point.x >= minimum.x && point.x <= maximum.x &&
           point.y >= minimum.y && point.y <= maximum.y &&
           point.z >= minimum.z && point.z <= maximum.z;
}

bool IsValidEditorCameraPose(const EditorCameraPose& pose)
{
    return IsFinite(pose.position) && IsFinite(pose.forward) &&
           glm::dot(pose.forward, pose.forward) > kEpsilon * kEpsilon &&
           IsFinite(pose.verticalFOV) && pose.verticalFOV > 1.0f &&
           pose.verticalFOV < 179.0f &&
           IsFinite(pose.aperture) && pose.aperture >= 0.0f &&
           IsFinite(pose.focusDistance) && pose.focusDistance > 0.0f &&
           IsFinite(pose.farClip) && pose.farClip > 0.0f;
}

bool TryNormalizeEditorCameraPose(EditorCameraPose& pose)
{
    glm::vec3 forward;
    if (!TryNormalize(pose.forward, forward)) return false;
    pose.forward = forward;
    return IsValidEditorCameraPose(pose);
}

bool TryCameraRotationFromForward(const glm::vec3& requestedForward,
                                  glm::quat& rotation)
{
    glm::vec3 forward, right, up;
    if (!BuildCameraBasis(requestedForward, forward, right, up))
        return false;
    const glm::mat3 basis(right, up, -forward);
    rotation = glm::normalize(glm::quat_cast(basis));
    return IsFinite(rotation.x) && IsFinite(rotation.y) &&
           IsFinite(rotation.z) && IsFinite(rotation.w);
}

bool ApplyEditorCameraCut(
    const EditorCameraPose& requested,
    rt2::core::ISceneRenderBridge& bridge,
    const std::function<bool(const EditorCameraPose&)>& applyPose)
{
    EditorCameraPose normalized = requested;
    if (!applyPose || !TryNormalizeEditorCameraPose(normalized) ||
        !applyPose(normalized))
        return false;
    bridge.ResetTemporalState();
    return true;
}

rt2::core::UUID FindDeterministicCameraEntity(
    const rt2::core::SceneDocument& document)
{
    rt2::core::UUID selected = rt2::core::UUID::Nil();
    const auto view = document.ecs.registry.view<CameraComponent, EntityIdComponent>();
    for (const entt::entity entity : view)
    {
        const auto& uuid = view.get<EntityIdComponent>(entity).id;
        if (selected.IsNull() || uuid < selected)
            selected = uuid;
    }
    return selected;
}

bool TryGetCameraEntityPose(rt2::core::SceneDocument& document,
                            const rt2::core::UUID& cameraEntity,
                            const EditorCameraPose& fallback,
                            EditorCameraPose& pose)
{
    const entt::entity entity = document.FindByUuid(cameraEntity);
    auto& registry = document.ecs.registry;
    if (entity == entt::null || !registry.valid(entity)) return false;
    const auto* camera = registry.try_get<CameraComponent>(entity);
    const auto* transform = registry.try_get<Transform>(entity);
    if (!camera || !transform) return false;

    SceneGraph::UpdateWorldTransforms(registry);
    EditableTRS world;
    if (!TryDecomposeEditableTRS(transform->worldMatrix, world)) return false;

    pose = fallback;
    pose.position = world.translation;
    pose.forward = glm::normalize(world.rotation * glm::vec3(0.0f, 0.0f, -1.0f));
    pose.verticalFOV = camera->verticalFOV;
    pose.aperture = camera->aperture;
    pose.focusDistance = camera->focusDistance;
    return TryNormalizeEditorCameraPose(pose);
}

bool ComputeEditorSelectionBounds(
    rt2::core::SceneDocument& document,
    const std::vector<rt2::core::UUID>& selectedRoots,
    EditorSelectionBounds& bounds)
{
    bounds = {};
    if (selectedRoots.empty()) return false;

    auto& registry = document.ecs.registry;
    SceneGraph::UpdateWorldTransforms(registry);

    std::vector<entt::entity> selected;
    selected.reserve(selectedRoots.size());
    for (const auto& uuid : selectedRoots)
    {
        const entt::entity entity = document.FindByUuid(uuid);
        if (entity != entt::null && registry.valid(entity))
            selected.push_back(entity);
    }

    std::vector<entt::entity> roots;
    for (const entt::entity candidate : selected)
    {
        bool coveredByAncestor = false;
        for (const entt::entity possibleAncestor : selected)
        {
            if (possibleAncestor != candidate &&
                SceneHierarchy::IsDescendant(registry, possibleAncestor, candidate))
            {
                coveredByAncestor = true;
                break;
            }
        }
        if (!coveredByAncestor)
            roots.push_back(candidate);
    }

    std::unordered_set<entt::entity> visited;
    for (const entt::entity root : roots)
    {
        std::vector<entt::entity> subtree;
        SceneHierarchy::CollectSubtreePreOrder(registry, root, subtree);
        for (const entt::entity entity : subtree)
        {
            if (!visited.insert(entity).second) continue;
            const auto* transform = registry.try_get<Transform>(entity);
            if (!transform) continue;

            bool includedGeometry = false;
            if (const auto* meshRef = registry.try_get<MeshRef>(entity))
            {
                if (meshRef->meshIndex < document.ecs.meshRegistry.GetCount())
                {
                    const MeshData& mesh =
                        document.ecs.meshRegistry.GetMesh(meshRef->meshIndex);
                    if (mesh.boundsValid)
                    {
                        IncludeMeshBounds(bounds, mesh, transform->worldMatrix);
                        includedGeometry = true;
                    }
                }
            }
            if (!includedGeometry)
            {
                const glm::vec4 worldOrigin =
                    transform->worldMatrix * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
                if (IsFinite(worldOrigin.w) && std::abs(worldOrigin.w) > kEpsilon)
                    IncludeFallback(bounds, glm::vec3(worldOrigin) / worldOrigin.w);
            }
        }
    }
    return bounds.valid;
}

bool TryFocusEditorCamera(const EditorCameraPose& current,
                          const EditorSelectionBounds& bounds,
                          float nearClip,
                          EditorCameraPose& focused)
{
    if (!bounds.valid || !IsFinite(nearClip) || nearClip <= 0.0f)
        return false;
    focused = current;
    if (!TryNormalizeEditorCameraPose(focused)) return false;

    const glm::vec3 toCentre = bounds.Center() - current.position;
    const float distanceSquared = glm::dot(toCentre, toCentre);
    if (IsFinite(distanceSquared) && distanceSquared > kEpsilon * kEpsilon)
    {
        focused.forward = toCentre * glm::inversesqrt(distanceSquared);
        focused.focusDistance = std::max(std::sqrt(distanceSquared), nearClip);
    }
    else
    {
        focused.focusDistance = std::max(current.focusDistance, nearClip);
    }
    return TryNormalizeEditorCameraPose(focused);
}

bool TryFrameEditorCamera(const EditorCameraPose& current,
                          const EditorSelectionBounds& bounds,
                          const EditorFrameSettings& settings,
                          EditorCameraPose& framed)
{
    if (!bounds.valid || !IsFinite(settings.viewportAspect) ||
        settings.viewportAspect <= 0.0f || !IsFinite(settings.nearClip) ||
        settings.nearClip <= 0.0f || !IsFinite(settings.margin) ||
        settings.margin < 1.0f)
        return false;

    framed = current;
    if (!TryNormalizeEditorCameraPose(framed)) return false;

    const glm::vec3 centre = bounds.Center();
    glm::vec3 desiredForward = framed.forward;
    if (bounds.Contains(current.position))
    {
        glm::vec3 towardCentre;
        if (TryNormalize(centre - current.position, towardCentre))
            desiredForward = towardCentre;
    }

    glm::vec3 forward, right, up;
    if (!BuildCameraBasis(desiredForward, forward, right, up)) return false;

    float halfWidth = 0.0f;
    float halfHeight = 0.0f;
    float halfDepth = 0.0f;
    for (int x = 0; x < 2; ++x)
        for (int y = 0; y < 2; ++y)
            for (int z = 0; z < 2; ++z)
            {
                const glm::vec3 corner(
                    x ? bounds.maximum.x : bounds.minimum.x,
                    y ? bounds.maximum.y : bounds.minimum.y,
                    z ? bounds.maximum.z : bounds.minimum.z);
                const glm::vec3 relative = corner - centre;
                halfWidth = std::max(halfWidth, std::abs(glm::dot(relative, right)));
                halfHeight = std::max(halfHeight, std::abs(glm::dot(relative, up)));
                halfDepth = std::max(halfDepth, std::abs(glm::dot(relative, forward)));
            }

    const float verticalRadians = glm::radians(framed.verticalFOV);
    const float verticalTangent = std::tan(verticalRadians * 0.5f);
    const float horizontalTangent = verticalTangent * settings.viewportAspect;
    if (!IsFinite(verticalTangent) || !IsFinite(horizontalTangent) ||
        verticalTangent <= kEpsilon || horizontalTangent <= kEpsilon)
        return false;

    const float distanceX = halfWidth / horizontalTangent;
    const float distanceY = halfHeight / verticalTangent;
    float distance = (std::max(distanceX, distanceY) + halfDepth) * settings.margin;
    distance = std::max(distance, settings.nearClip + halfDepth + 0.01f);
    if (!IsFinite(distance) || distance <= 0.0f) return false;

    framed.forward = forward;
    framed.position = centre - forward * distance;
    framed.focusDistance = distance;
    return TryNormalizeEditorCameraPose(framed);
}
