#pragma once

#ifndef RT2_PERSISTED_COMPONENTS_H
#define RT2_PERSISTED_COMPONENTS_H

#include "ECSComponents.h"

#include <cstddef>

// Canonical list of authored value components. EntityIdComponent and
// Hierarchy are deliberately handled separately because duplication remaps
// identity and relationships. Add a component here when it becomes authored;
// serializer and duplication coverage tests then fail together rather than
// silently drifting.
namespace PersistedComponents
{
template<typename T>
struct Tag { using Type = T; };

constexpr size_t Count = 13;

template<typename Visitor>
void ForEach(Visitor&& visitor)
{
    visitor(Tag<NameComponent>{});
    visitor(Tag<Transform>{});
    visitor(Tag<VisibleComponent>{});
    visitor(Tag<MeshRef>{});
    visitor(Tag<PrimitiveComponent>{});
    visitor(Tag<ImportedMeshSourceComponent>{});
    visitor(Tag<MaterialOverrideComponent>{});
    visitor(Tag<LightComponent>{});
    visitor(Tag<CameraComponent>{});
    visitor(Tag<MotionComponent>{});
    visitor(Tag<ScriptComponent>{});
    visitor(Tag<PrefabInstanceComponent>{});
    visitor(Tag<PrefabMemberComponent>{});
}
}

#endif
