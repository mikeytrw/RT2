#include "EditorSceneState.h"
#include "SceneHierarchy.h"
#include "SceneSerializer.h"

bool EditorSceneState::IsLocked(const rt2::core::UUID& entity) const
{
    return m_DirectLocks.count(entity) != 0;
}

void EditorSceneState::SetLocked(const rt2::core::UUID& entity, bool locked)
{
    if (entity.IsNull()) return;
    if (locked) m_DirectLocks.insert(entity);
    else m_DirectLocks.erase(entity);
}

void EditorSceneState::ToggleLocked(const rt2::core::UUID& entity)
{
    SetLocked(entity, !IsLocked(entity));
}

bool EditorSceneState::AnyDirectlyLocked(
    const std::vector<rt2::core::UUID>& entities) const
{
    for (const auto& entity : entities)
        if (IsLocked(entity)) return true;
    return false;
}

bool EditorSceneState::Copy(const SceneManager& manager,
                            const std::vector<rt2::core::UUID>& roots,
                            rt2::core::Error& error)
{
    error = {};
    if (roots.empty())
    {
        error.code = rt2::core::Error::InvalidEntity;
        error.detail = "copy requires at least one selected entity";
        return false;
    }
    for (const auto& root : roots)
        if (manager.FindEntityByUuid(root) == entt::null)
        {
            error.code = rt2::core::Error::InvalidEntity;
            error.path = root.ToString();
            error.detail = "copy source is not present in the authoring scene";
            return false;
        }
    auto snapshot = std::make_unique<rt2::core::SceneDocument>();
    // CloneInMemory preserves dst's UUID provider; set it from the source
    // so any internal AssignNewUuid calls (if any) have a valid provider.
    snapshot->SetUuidProvider(manager.AuthoringDoc().GetUuidProvider());
    if (!rt2::core::SceneSerializer::CloneInMemory(
            manager.AuthoringDoc(), *snapshot, error))
        return false;
    m_Clipboard = std::move(snapshot);
    m_ClipboardRoots = roots;
    m_ClipboardDocumentGeneration = manager.DocumentGeneration();
    m_ClipboardResourceGeneration = manager.ResourceGeneration();
    return true;
}

EditorMutationResult EditorSceneState::ValidateClipboardPaste(
    const SceneManager& manager) const
{
    if (!HasClipboard())
        return EditorMutationResult::Failure(rt2::core::Error::ClipboardStale,
            {}, "editor clipboard is empty");
    if (manager.DocumentGeneration() != m_ClipboardDocumentGeneration)
        return EditorMutationResult::Failure(rt2::core::Error::ClipboardStale,
            {}, "clipboard belongs to a different scene document");
    if (manager.ResourceGeneration() != m_ClipboardResourceGeneration)
        return EditorMutationResult::Failure(rt2::core::Error::ClipboardStale,
            {}, "scene resources changed after copy; copy the selection again");
    return {};
}

EditorMutationResult EditorSceneState::Paste(
    SceneManager& manager,
    const std::optional<rt2::core::UUID>& parent) const
{
    auto validation = ValidateClipboardPaste(manager);
    if (!validation.success) return validation;
    return manager.PasteSubtreesFrom(*m_Clipboard, m_ClipboardRoots, parent);
}

SceneManager::DuplicationResult EditorSceneState::PasteWithUuidsForCommand(
    SceneManager& manager,
    const std::optional<rt2::core::UUID>& parent) const
{
    SceneManager::DuplicationResult out;
    // Shared guard BEFORE any counting, reservation, or mutation. The manager's
    // own resource checks inside PasteSubtreesWithUuids are range-only and
    // cannot see an in-range-but-stale index after compaction; the generation
    // check here is the only defense.
    auto validation = ValidateClipboardPaste(manager);
    if (!validation.success)
    {
        out.mutation = validation;
        return out;
    }
    // Count the clipboard document's subtree entities. The clipboard roots live
    // in the clipboard document, not the live scene, so they cannot be counted
    // via CountCanonicalSubtreeEntities (which walks the authoring scene).
    std::size_t count = 0;
    for (const auto& root : m_ClipboardRoots)
    {
        const auto rootEntity = m_Clipboard->FindByUuid(root);
        if (rootEntity == entt::null) continue;
        std::vector<entt::entity> subtree;
        SceneHierarchy::CollectSubtreePreOrder(
            m_Clipboard->ecs.registry, rootEntity, subtree);
        count += subtree.size();
    }
    auto knownUuids = manager.ReserveKnownUuids(count);
    return manager.PasteSubtreesWithUuids(
        *m_Clipboard, m_ClipboardRoots, parent, knownUuids);
}

bool EditorSceneState::CaptureCameraBookmark(size_t slot,
                                             const EditorCameraPose& requested)
{
    if (slot >= m_CameraBookmarks.size()) return false;
    EditorCameraPose pose = requested;
    if (!TryNormalizeEditorCameraPose(pose)) return false;
    m_CameraBookmarks[slot] = pose;
    return true;
}

const EditorCameraPose* EditorSceneState::CameraBookmark(size_t slot) const
{
    if (slot >= m_CameraBookmarks.size() || !m_CameraBookmarks[slot])
        return nullptr;
    return &*m_CameraBookmarks[slot];
}

bool EditorSceneState::ClearCameraBookmark(size_t slot)
{
    if (slot >= m_CameraBookmarks.size() || !m_CameraBookmarks[slot])
        return false;
    m_CameraBookmarks[slot].reset();
    return true;
}

void EditorSceneState::Prune(const rt2::core::SceneDocument& document)
{
    m_Selection.Prune(document);
    for (auto it = m_DirectLocks.begin(); it != m_DirectLocks.end();)
        if (document.FindByUuid(*it) == entt::null)
            it = m_DirectLocks.erase(it);
        else
            ++it;
}

void EditorSceneState::ResetForDocument()
{
    m_Selection.Clear();
    m_DirectLocks.clear();
    m_SearchText.clear();
    m_Clipboard.reset();
    m_ClipboardRoots.clear();
    m_ClipboardDocumentGeneration = 0;
    m_ClipboardResourceGeneration = 0;
    for (auto& bookmark : m_CameraBookmarks)
        bookmark.reset();
}
