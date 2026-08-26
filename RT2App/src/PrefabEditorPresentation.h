#pragma once

#ifndef RT2_PREFAB_EDITOR_PRESENTATION_H
#define RT2_PREFAB_EDITOR_PRESENTATION_H

#include "AssetReference.h"
#include "PrefabPropagationLive.h"
#include "SceneDocument.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rt2::core {

enum class PrefabLinkPresentationKind : std::uint8_t
{
    Ordinary,
    Root,
    Member,
    Broken,
};

struct PrefabEntityPresentation
{
    PrefabLinkPresentationKind kind = PrefabLinkPresentationKind::Ordinary;
    std::string hierarchyTag;
    bool hasSource = false;
    AssetReference source;
    UUID instanceId;
    UUID templateId;
    UUID rootUuid;
    std::vector<std::string> overrideLabels;
    std::string warning;

    bool IsLinked() const noexcept
    { return kind != PrefabLinkPresentationKind::Ordinary; }
};

PrefabEntityPresentation DescribePrefabEntity(
    const SceneDocument& document, const UUID& entityUuid);

std::string PrefabOverrideTooltip(std::string_view componentLabel);

struct PrefabPropagationPresentation
{
    bool visible = false;
    bool warning = false;
    std::string summary;
    std::vector<PrefabPropagationDiagnostic> diagnostics;
    std::vector<UUID> warningInstanceIds;

    bool Warns(const UUID& instanceId) const noexcept
    {
        return !instanceId.IsNull() &&
            std::find(warningInstanceIds.begin(), warningInstanceIds.end(),
                instanceId) != warningInstanceIds.end();
    }
};

// Non-durable latest-result holder shared by the CPU tests and the editor UI.
// A new report replaces the complete prior snapshot; context resets clear it.
class PrefabPropagationPresentationState
{
public:
    void Replace(PrefabPropagationPresentation presentation)
    { m_Current = std::move(presentation); }
    void Clear() { m_Current = {}; }
    const PrefabPropagationPresentation& Current() const noexcept
    { return m_Current; }

private:
    PrefabPropagationPresentation m_Current;
};

PrefabPropagationPresentation DescribePrefabPropagation(
    const PrefabPropagationLiveReport& report);

} // namespace rt2::core

#endif // RT2_PREFAB_EDITOR_PRESENTATION_H
