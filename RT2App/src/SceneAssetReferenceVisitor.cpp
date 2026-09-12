#include "SceneAssetReferenceVisitor.h"

#include "ECSComponents.h"
#include "SceneDocument.h"

#include <algorithm>

namespace rt2::core {
namespace {

template<typename Slot>
bool SlotLess(const Slot& left, const Slot& right)
{
    if (left.entityUuid != right.entityUuid)
        return left.entityUuid < right.entityUuid;
    const AssetKind leftKind = left.reference
        ? left.reference->kind : AssetKind::Unknown;
    const AssetKind rightKind = right.reference
        ? right.reference->kind : AssetKind::Unknown;
    if (leftKind != rightKind)
        return static_cast<uint8_t>(leftKind) <
               static_cast<uint8_t>(rightKind);
    if (left.reference && right.reference &&
        left.reference->sourceKey != right.reference->sourceKey)
        return left.reference->sourceKey < right.reference->sourceKey;
    const std::string leftPath = left.reference ? left.reference->path : std::string{};
    const std::string rightPath = right.reference ? right.reference->path : std::string{};
    return leftPath < rightPath;
}

template<typename Slot, typename Document>
std::vector<Slot> Collect(Document& document)
{
    std::vector<Slot> result;
    auto view = document.ecs.registry.view<EntityIdComponent>();
    for (const auto entity : view)
    {
        const auto& id = view.template get<EntityIdComponent>(entity).id;
        std::string name;
        if (const auto* nameComponent =
                document.ecs.registry.template try_get<NameComponent>(entity))
            name = nameComponent->name;

        if (auto* imported =
                document.ecs.registry.template try_get<ImportedMeshSourceComponent>(entity))
            result.push_back(Slot{&imported->model, id, name});
        if (auto* script =
                document.ecs.registry.template try_get<ScriptComponent>(entity))
            result.push_back(Slot{&script->asset, id, name});
        // T2 physics persistence foundation: BOTH stored collision-geometry
        // references of a physics shape, unconditionally — matching
        // imported/script/prefab reference behavior. Both fields are exact
        // authored payload and both are serialized, so save validation
        // (which rejects a non-empty path with an unknown kind),
        // migration (assign/rebase/validate), and content-browser
        // dependency protection must see them even when inactive, empty,
        // or invalid. Filtering here would let Save succeed on a v8 file
        // that Load refuses, and would hide a stale reference until a
        // later shape-kind switch reveals it.
        if (auto* shape =
                document.ecs.registry.template try_get<PhysicsShapeComponent>(entity))
        {
            result.push_back(Slot{&shape->hull, id, name});
            result.push_back(Slot{&shape->triMesh, id, name});
        }
        if (auto* instance =
                document.ecs.registry.template try_get<PrefabInstanceComponent>(entity))
            result.push_back(Slot{&instance->prefab, id, name});
    }

    if (!document.environment.ref.path.empty())
        result.push_back(Slot{&document.environment.ref, UUID::Nil(), {}});

    std::sort(result.begin(), result.end(), SlotLess<Slot>);
    return result;
}

} // namespace

std::vector<SceneAssetReferenceSlot> CollectSceneAssetReferences(
    SceneDocument& document)
{
    return Collect<SceneAssetReferenceSlot>(document);
}

std::vector<ConstSceneAssetReferenceSlot> CollectSceneAssetReferences(
    const SceneDocument& document)
{
    return Collect<ConstSceneAssetReferenceSlot>(document);
}

} // namespace rt2::core
