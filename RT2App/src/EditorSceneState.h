#pragma once

#ifndef RT2_EDITOR_SCENE_STATE_H
#define RT2_EDITOR_SCENE_STATE_H

#include "EditorSelection.h"
#include "EditorCameraWorkflow.h"
#include "SceneManager.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>

// Document-scoped editor-only state. None of this is serialized into a scene
// or cloned into Play mode.
class EditorSceneState
{
public:
    static constexpr size_t kCameraBookmarkCount = 9;

    EditorSelection& Selection() { return m_Selection; }
    const EditorSelection& Selection() const { return m_Selection; }

    bool IsLocked(const rt2::core::UUID& entity) const;
    void SetLocked(const rt2::core::UUID& entity, bool locked);
    void ToggleLocked(const rt2::core::UUID& entity);
    bool AnyDirectlyLocked(const std::vector<rt2::core::UUID>& entities) const;

    bool Copy(const SceneManager& manager,
              const std::vector<rt2::core::UUID>& roots,
              rt2::core::Error& error);
    EditorMutationResult Paste(SceneManager& manager,
        const std::optional<rt2::core::UUID>& parent = std::nullopt) const;
    bool HasClipboard() const { return m_Clipboard != nullptr && !m_ClipboardRoots.empty(); }

    // Shared clipboard-generation validation for BOTH paste paths. An empty
    // clipboard or a document/resource generation mismatch returns a
    // ClipboardStale failure; otherwise success. No manager mutation, no UUID
    // reservation. Ordinary Paste and PasteWithUuidsForCommand must both route
    // through this guard so the undoable editor path cannot silently rebind an
    // in-range but stale resource index after CompactMeshRegistry.
    EditorMutationResult ValidateClipboardPaste(const SceneManager& manager) const;

    // UUID-aware paste preparation used by SceneEditorUI::PasteCommand.
    // Runs ValidateClipboardPaste FIRST, canonical-counts the clipboard
    // document's subtree entities, reserves destination UUIDs only after
    // validation, then calls SceneManager::PasteSubtreesWithUuids. Returns the
    // full DuplicationResult the host needs to build the undo snapshot and
    // command. On a validation failure the mutation is the ClipboardStale
    // result, with zero UUIDs consumed and nothing mutated.
    SceneManager::DuplicationResult PasteWithUuidsForCommand(
        SceneManager& manager,
        const std::optional<rt2::core::UUID>& parent = std::nullopt) const;

    // Phase 3B1: read-only access to the clipboard document and roots for
    // command construction (PasteSubtreesWithUuids needs the clipboard
    // document). Returns nullptr when the clipboard is empty.
    const rt2::core::SceneDocument* ClipboardDocument() const { return m_Clipboard.get(); }
    const std::vector<rt2::core::UUID>& ClipboardRoots() const { return m_ClipboardRoots; }

    bool CaptureCameraBookmark(size_t slot, const EditorCameraPose& pose);
    const EditorCameraPose* CameraBookmark(size_t slot) const;
    bool ClearCameraBookmark(size_t slot);

    void Prune(const rt2::core::SceneDocument& document);
    void ResetForDocument();

    std::string& SearchText() { return m_SearchText; }
    const std::string& SearchText() const { return m_SearchText; }

private:
    EditorSelection m_Selection;
    std::unordered_set<rt2::core::UUID> m_DirectLocks;
    std::string m_SearchText;

    std::unique_ptr<rt2::core::SceneDocument> m_Clipboard;
    std::vector<rt2::core::UUID> m_ClipboardRoots;
    uint64_t m_ClipboardDocumentGeneration = 0;
    uint64_t m_ClipboardResourceGeneration = 0;
    std::array<std::optional<EditorCameraPose>, kCameraBookmarkCount> m_CameraBookmarks{};
};

#endif
