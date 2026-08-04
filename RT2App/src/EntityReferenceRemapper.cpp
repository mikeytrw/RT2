#include "EntityReferenceRemapper.h"

#include "ECSComponents.h"

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

} // namespace rt2::core
