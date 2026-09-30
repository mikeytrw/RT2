#include "EditorOutlinerFilter.h"

#include "AudioComponents.h"
#include "ECSComponents.h"
#include "PhysicsComponents.h"

#include <cctype>

namespace rt2::core {

namespace {

std::string FoldForMatch(std::string_view text)
{
    std::string out(text);
    for (auto& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

} // anonymous namespace

BrowserTypeToken TokenForBrowserTypes(std::uint32_t types)
{
    using T = EntityBrowserType;
    const auto has = [&](T flag) {
        return (types & static_cast<std::uint32_t>(flag)) != 0;
    };
    if (has(T::Camera)) return { "[Camera]", 0.55f, 0.60f, 1.00f };
    if (has(T::Light)) return { "[Light]", 1.00f, 0.80f, 0.35f };
    if (has(T::Physics)) return { "[Physics]", 1.00f, 0.55f, 0.20f };
    if (has(T::Audio)) return { "[Audio]", 0.70f, 0.50f, 1.00f };
    if (has(T::Script)) return { "[Script]", 0.45f, 0.85f, 0.50f };
    if (has(T::Motion)) return { "[Motion]", 0.35f, 0.75f, 0.75f };
    if (has(T::Mesh)) return { "[Mesh]", 0.75f, 0.75f, 0.75f };
    return { "[Entity]", 0.50f, 0.50f, 0.50f };
}

std::uint32_t ClassifyEntityForBrowser(
    const SceneManager& manager, SceneManager::EntityId entity)
{
    if (!entity.IsValid()) return 0;
    std::uint32_t types = 0;
    const auto set = [&](EntityBrowserType flag) {
        types |= static_cast<std::uint32_t>(flag);
    };
    // Authoritative helpers where they exist; registry probes otherwise.
    // try_get on a dead entity is safe (null) — never hidden by error.
    if (manager.HasCamera(entity)) set(EntityBrowserType::Camera);
    if (manager.HasMeshRef(entity)) set(EntityBrowserType::Mesh);
    const auto& registry = manager.GetECS().registry;
    if (registry.try_get<LightComponent>(entity.id) != nullptr)
        set(EntityBrowserType::Light);
    if (registry.try_get<PhysicsBodyComponent>(entity.id) != nullptr ||
        registry.try_get<PhysicsShapeComponent>(entity.id) != nullptr ||
        registry.try_get<PhysicsHingeComponent>(entity.id) != nullptr ||
        registry.try_get<PhysicsSliderComponent>(entity.id) != nullptr)
        set(EntityBrowserType::Physics);
    if (registry.try_get<AudioSourceComponent>(entity.id) != nullptr)
        set(EntityBrowserType::Audio);
    if (registry.try_get<ScriptComponent>(entity.id) != nullptr)
        set(EntityBrowserType::Script);
    if (registry.try_get<MotionComponent>(entity.id) != nullptr)
        set(EntityBrowserType::Motion);
    return types;
}

bool MatchesBrowserFilter(std::string_view name, std::uint32_t types,
    std::string_view query, std::uint32_t enabledMask)
{
    if (enabledMask != 0 && (types & enabledMask) == 0) return false;
    if (query.empty()) return true;
    return FoldForMatch(name).find(FoldForMatch(query)) != std::string::npos;
}

} // namespace rt2::core
