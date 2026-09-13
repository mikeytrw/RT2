#include "EntityReferenceRemapper.h"

#include "ECSComponents.h"
#include "SceneDocument.h"

namespace rt2::core
{

void RemapEntityReferences(const EntityUuidRemap& remap,
                           const std::vector<ScriptComponent*>& components)
{
    for (auto* component : components)
    {
        if (!component)
            continue;

        for (auto& [name, entry] : component->fieldValues)
        {
            (void)name;
            if (entry.type != ScriptFieldType::Uuid)
                continue;

            const auto* referenced = std::get_if<UUID>(&entry.value);
            if (!referenced || referenced->IsNull())
                continue;

            const auto it = remap.find(*referenced);
            if (it == remap.end() || it->second.IsNull())
                continue;

            *std::get_if<UUID>(&entry.value) = it->second;
        }
    }
}

void RemapPhysicsConstraintReferences(
    const EntityUuidRemap& remap,
    const std::vector<PhysicsHingeComponent*>& hinges,
    const std::vector<PhysicsSliderComponent*>& sliders)
{
    for (auto* hinge : hinges)
    {
        if (!hinge || hinge->otherBody.IsNull())
            continue;
        const auto it = remap.find(hinge->otherBody);
        if (it == remap.end() || it->second.IsNull())
            continue;
        hinge->otherBody = it->second;
    }
    for (auto* slider : sliders)
    {
        if (!slider || slider->otherBody.IsNull())
            continue;
        const auto it = remap.find(slider->otherBody);
        if (it == remap.end() || it->second.IsNull())
            continue;
        slider->otherBody = it->second;
    }
}

bool ValidatePhysicsConstraintReferences(const SceneDocument& doc, Error& err)
{
    err = Error{};
    const auto& registry = doc.ecs.registry;

    // Index owner UUIDs for diagnostics. A missing EntityIdComponent cannot
    // name its owner; treat it as InvalidEntity loudly rather than skipping.
    auto ownerUuid = [&](entt::entity e, UUID& out) {
        const auto* idc = registry.try_get<EntityIdComponent>(e);
        if (!idc || idc->id.IsNull())
        {
            err.code = Error::InvalidEntity;
            err.detail =
                "physics constraint owner has no authored UUID (cannot persist)";
            return false;
        }
        out = idc->id;
        return true;
    };

    auto checkOther = [&](const UUID& owner, const UUID& other,
                          const char* kind) {
        if (other.IsNull())
            return true; // world/static frame anchor: always valid
        if (other == owner)
        {
            err.code = Error::InvalidArgument;
            err.path = owner.ToString();
            err.detail = std::string(kind) + " otherBody is the owner's own UUID " +
                         other.ToString() + " (self-constraint); owner " +
                         owner.ToString();
            return false;
        }
        const entt::entity target = doc.FindByUuid(other);
        if (target == entt::null || !registry.valid(target))
        {
            err.code = Error::InvalidArgument;
            err.path = owner.ToString();
            err.detail = std::string(kind) + " otherBody " + other.ToString() +
                         " does not resolve to a live entity; owner " +
                         owner.ToString();
            return false;
        }
        if (!registry.all_of<PhysicsBodyComponent>(target))
        {
            err.code = Error::InvalidArgument;
            err.path = owner.ToString();
            err.detail = std::string(kind) + " otherBody " + other.ToString() +
                         " names an entity without PhysicsBodyComponent; owner " +
                         owner.ToString();
            return false;
        }
        return true;
    };

    // Hinges: owner must carry a body, then the reference must resolve.
    for (const auto entity :
         registry.view<PhysicsHingeComponent, EntityIdComponent>())
    {
        UUID owner;
        if (!ownerUuid(entity, owner))
            return false;
        if (!registry.all_of<PhysicsBodyComponent>(entity))
        {
            err.code = Error::InvalidArgument;
            err.path = owner.ToString();
            err.detail =
                "PhysicsHingeComponent owner " + owner.ToString() +
                " carries no PhysicsBodyComponent (constraint has no body)";
            return false;
        }
        const auto& hinge = registry.get<PhysicsHingeComponent>(entity);
        if (!checkOther(owner, hinge.otherBody, "PhysicsHingeComponent"))
            return false;
    }

    // Sliders: same contract.
    for (const auto entity :
         registry.view<PhysicsSliderComponent, EntityIdComponent>())
    {
        UUID owner;
        if (!ownerUuid(entity, owner))
            return false;
        if (!registry.all_of<PhysicsBodyComponent>(entity))
        {
            err.code = Error::InvalidArgument;
            err.path = owner.ToString();
            err.detail =
                "PhysicsSliderComponent owner " + owner.ToString() +
                " carries no PhysicsBodyComponent (constraint has no body)";
            return false;
        }
        const auto& slider = registry.get<PhysicsSliderComponent>(entity);
        if (!checkOther(owner, slider.otherBody, "PhysicsSliderComponent"))
            return false;
    }

    return true;
}

} // namespace rt2::core
