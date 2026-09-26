#pragma once

#ifndef RT2_AUDIO_INSPECTOR_STATE_H
#define RT2_AUDIO_INSPECTOR_STATE_H

#include "AudioComponents.h"
#include "core/UUID.h"

#include <optional>
#include <string>

// ============================================================================
// AudioInspectorState — CPU-testable working-copy policy for the A7 audio
// Inspector.
//
// The ImGui layer edits a working copy, not the live component; Apply commits
// exactly one SetAudioSourceCommand (before = live read fresh at Apply time,
// after = working copy), Add/Remove commit immediately. This struct owns the
// resync/conflict/reset rules so they are probe-tested without ImGui:
//
//   - Reseed on selection (target) change. A clean copy also follows live
//     presence changes (component added/removed out-of-band); a dirty copy
//     instead conflicts and remains available for an explicit Revert.
//   - Clean-copy resync: a non-dirty copy whose live values drifted (Undo of
//     an earlier edit while this entity stayed selected) is resynced to live,
//     so the next edit-and-Apply cannot overwrite fields Undo restored.
//   - Dirty/live conflict: a dirty copy whose live state moved under it
//     (Undo/Redo of another command) raises a conflict flag. Policy is
//     explicit: Apply stays disabled until Revert (which reseeds from live).
//     The user's unapplied edits are never silently discarded AND never
//     applied over restored history state.
//   - Document reset clears target, copies, seeds, dirty, and conflict flags,
//     so a same-UUID replacement document cannot inherit values.
//   - Clip path changes clear the stale asset ID (ID-first resolution would
//     otherwise name the previous file); Browse/drop repairs identity through
//     the host import callback. Unchanged paths preserve the ID.
//
// CPU-only: plain data + equality, no ImGui/Walnut/Vulkan/miniaudio/device.
// ============================================================================

struct AudioInspectorWork
{
    rt2::core::UUID target{};
    std::optional<AudioSourceComponent> work;
    std::optional<AudioSourceComponent> seed;
    bool dirty = false;
    bool conflict = false;

    bool HasTarget() const { return !target.IsNull(); }

    // Reconcile working state with the live document for the selected target.
    // Live presence is derived from the optional (nullopt = absent).
    void Sync(const rt2::core::UUID& selected,
              const std::optional<AudioSourceComponent>& live)
    {
        if (target != selected)
        {
            target = selected;
            Reseed(live);
            return;
        }
        SyncSide(work, seed, live, dirty, conflict);
    }

    // Reseed both copies from live, dropping dirt and conflict. Used on
    // selection change, explicit Revert, and successful Apply.
    void Reseed(const std::optional<AudioSourceComponent>& live)
    {
        work = seed = live;
        dirty = false;
        conflict = false;
    }

    // A successful Apply advances both copies to the fresh live state.
    void Applied(const std::optional<AudioSourceComponent>& live)
    {
        Reseed(live);
    }

    // Explicit revert reseeds from live.
    void Revert(const std::optional<AudioSourceComponent>& live)
    {
        Reseed(live);
    }

    // Document reset (including same-UUID replacement): drop everything.
    void Clear()
    {
        target = rt2::core::UUID{};
        work.reset();
        seed.reset();
        dirty = false;
        conflict = false;
    }

    // Clip path edit: assign the new path (AudioClip kind unless empty) and
    // clear any prior asset ID, which named the previous file. The stale
    // sourceKey is cleared alongside it. Returns true when the path actually
    // changed. Strict Apply validation then refuses a bound-but-identityless
    // reference loudly until Browse/drop repairs it; the working copy itself
    // is preserved so no edit is dropped.
    static bool NoteAudioClipPathChanged(AssetReference& ref,
                                         const std::string& newPath)
    {
        if (ref.path == newPath)
            return false;
        ref.path = newPath;
        ref.kind =
            newPath.empty() ? AssetKind::Unknown : AssetKind::AudioClip;
        ref.assetId = rt2::core::UUID{};
        ref.sourceKey.clear();
        return true;
    }

private:
    template <typename T>
    static void SyncSide(std::optional<T>& work, std::optional<T>& seed,
                         const std::optional<T>& live, bool& dirty,
                         bool& conflict)
    {
        if (work.has_value() != live.has_value())
        {
            // Preserve a dirty edit when Undo/Redo removes its component.
            // The UI keeps rendering the working copy until explicit Revert.
            if (dirty)
            {
                conflict = true;
                return;
            }
            work = live;
            seed = live;
            conflict = false;
            return;
        }
        if (!dirty)
        {
            // Clean copy follows live values (Undo/Redo of value edits).
            if (!(work == live))
                work = live;
            seed = live;
            conflict = false;
            return;
        }
        // Dirty copy: conflict iff live moved away from the seed. The copy
        // is kept (nothing is silently discarded); Apply must stay disabled
        // until Revert reseeds. A dirty copy edited back to live values is
        // not dirty at all (return-to-start needs no history entry).
        if (work == live && live == seed)
        {
            dirty = false;
            conflict = false;
            return;
        }
        conflict = !(live == seed);
    }
};

// Audio A7 Apply gating (CPU-only, probe-tested; SceneEditorUI consults it
// for BeginDisabled and for the defensive Apply guard). While malformed clip
// text is retained, Apply must refuse even when another field made the
// working copy dirty: committing would silently revert the invalid edit to
// the old model value on success. The raw text, error, and working copy
// survive until a valid parse or an explicit Revert.
inline bool AudioClipTextBlocksApply(bool textActive, const std::string& error)
{
    return textActive && !error.empty();
}

inline bool AudioInspectorApplyBlocked(bool conflict, bool textActive,
                                       const std::string& error)
{
    return conflict || AudioClipTextBlocksApply(textActive, error);
}

// Preview reconciliation policy shared by the inspector and the probe
// (CPU-only, probe-tested with real ImGui widget events; SceneEditorUI
// executes exactly these decisions, so the probe covers the production
// wiring rather than a surrogate).
enum class AudioAuthoringPreviewAction : uint8_t
{
    None = 0,
    Stop,    // the previewed source is gone: stop explicitly with status
    Restart, // the previewed clip identity changed: restart the audition
};

// Preview controls (including Stop) disable only when there is no live
// source AND this entity owns no preview. A removed-while-previewing
// source therefore keeps Stop reachable: the voice can never be left
// sounding with no way to stop it from its own row.
inline bool AudioPreviewControlsDisabled(bool editable, bool hasLiveSource,
                                         bool previewingThis)
{
    return !editable || (!hasLiveSource && !previewingThis);
}

// A successful source removal stops the preview only when the removed
// entity owns it; other entities' auditions are untouched.
inline AudioAuthoringPreviewAction DecidePreviewActionOnRemove(
    bool previewingThis)
{
    return previewingThis ? AudioAuthoringPreviewAction::Stop
                          : AudioAuthoringPreviewAction::None;
}

// A successful Apply restarts the audition only when this entity owns the
// preview AND the committed clip identity moved (kind, path, or asset ID).
// Field-only edits (gain, pitch, distances) leave the running voice alone;
// a clip replacement always reflects applied state (the ticket's
// "replaces only the preview voice" acceptance).
inline AudioAuthoringPreviewAction DecidePreviewActionOnApply(
    bool previewingThis, const AssetReference& beforeClip,
    const AssetReference& afterClip)
{
    if (!previewingThis)
        return AudioAuthoringPreviewAction::None;
    const bool identityMoved =
        beforeClip.kind != afterClip.kind ||
        beforeClip.path != afterClip.path ||
        beforeClip.assetId != afterClip.assetId;
    return identityMoved ? AudioAuthoringPreviewAction::Restart
                         : AudioAuthoringPreviewAction::None;
}

// Typed clip-path text parser shared by the inspector field (CPU-only,
// probe-tested; the ImGui layer only retains text and renders the error).
//
// Empty text unbinds the source (ok, no error). A path whose extension is not
// .wav/.flac/.mp3 refuses with a typed error naming the field; the caller
// retains the raw text and surfaces the error instead of silently reverting
// to the model value.
inline bool TryParseAudioClipPathText(const std::string& text,
                                      std::string& error)
{
    if (text.empty())
    {
        error.clear();
        return true;
    }
    if (!IsAudioClipPath(text))
    {
        error = "clip '" + text +
                "' must use a .wav, .flac, or .mp3 extension; edit preserved";
        return false;
    }
    error.clear();
    return true;
}

#endif // RT2_AUDIO_INSPECTOR_STATE_H
