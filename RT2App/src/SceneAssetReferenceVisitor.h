#pragma once

#ifndef RT2_CORE_SCENE_ASSET_REFERENCE_VISITOR_H
#define RT2_CORE_SCENE_ASSET_REFERENCE_VISITOR_H

#include "AssetReference.h"
#include "core/UUID.h"

#include <string>
#include <vector>

namespace rt2::core {

class SceneDocument;

// One durable source-asset reference in a scene. Native persistence owns
// imported models, scripts, and the environment; derived SceneTexture records
// are deliberately not part of this visitor.
struct SceneAssetReferenceSlot
{
    AssetReference* reference = nullptr;
    UUID entityUuid;
    std::string entityName;
};

struct ConstSceneAssetReferenceSlot
{
    const AssetReference* reference = nullptr;
    UUID entityUuid;
    std::string entityName;
};

// Collect every durable scene reference and return it in a stable order. The
// returned pointers refer to the supplied document and are valid until it is
// mutated or destroyed.
std::vector<SceneAssetReferenceSlot> CollectSceneAssetReferences(
    SceneDocument& document);

std::vector<ConstSceneAssetReferenceSlot> CollectSceneAssetReferences(
    const SceneDocument& document);

} // namespace rt2::core

#endif // RT2_CORE_SCENE_ASSET_REFERENCE_VISITOR_H
