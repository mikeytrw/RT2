#pragma once

#ifndef RT2_AUDIO_PREVIEW_CONTROLLER_H
#define RT2_AUDIO_PREVIEW_CONTROLLER_H

#include "AudioBackend.h"
#include "AudioClipProvider.h"
#include "AudioComponents.h"
#include "core/Error.h"
#include "core/UUID.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// ============================================================================
// AudioPreviewController — edit-mode Preview/Stop owner (audio A7).
//
// A Preview session is NOT a Play session: it lives outside
// RuntimeSceneController, owns exactly one replaceable preview voice, and
// never consumes runtime voice capacity. Starting a new preview replaces the
// previous preview voice; selection/document change, Play, project close, and
// app shutdown all route through one centralized host stop (WalnutApp owns
// the controller between the backend and the runtime controller so reverse
// member destruction tears down runtime first, preview second, backend last;
// the backend owns no pointer into this controller).
//
// Preview is center/2D by default so asset quality is judged independent of
// scene distance; spatialAudition (the inspector's Preview Spatial checkbox)
// auditions the current source/listener positions through the same
// ComputeSpatialMix math as runtime, without changing authored state.
//
// CPU-only: standard library + audio headers + core only. No miniaudio,
// device, Vulkan, ImGui, or Walnut types. Links anywhere the backend
// interface links; ImGui event probes live in RT2ImGuiProbe, not RT2Tests.
// ============================================================================

namespace rt2::audio {

class AudioPreviewController
{
public:
    // Maps a bound AudioSourceComponent clip to the opaque generation key
    // the injected IAudioClipProvider serves (the host's frozen
    // AudioClipAssetProvider snapshot, same builder shape as Play).
    using ClipKeyBuilder = std::function<core::Result<std::string>(
        const AssetReference& clip, const core::UUID& entityUuid,
        const std::string& entityName)>;

    AudioPreviewController();
    ~AudioPreviewController(); // best-effort stop; use Shutdown for loud errors

    AudioPreviewController(const AudioPreviewController&) = delete;
    AudioPreviewController& operator=(const AudioPreviewController&) = delete;

    // Borrowed seams, owned by the host for the host session. All three must
    // be set before StartPreview; ClearBindings forgets them (the host calls
    // it only after StopPreview, so no voice can outlive the backend).
    void SetBackend(IAudioBackend* backend) { m_Backend = backend; }
    void SetClipProvider(IAudioClipProvider* provider) { m_Provider = provider; }
    void SetClipKeyBuilder(ClipKeyBuilder builder) { m_KeyBuilder = std::move(builder); }
    void ClearBindings();

    // Starts a preview, replacing any live preview voice first. Returns true
    // with an empty Error only when the new preview voice is live. Any
    // failure (unbound seams, invalid component, missing/corrupt/unsupported
    // clip, spatial stereo, backend refusal) returns false with a typed
    // Error plus the affected asset path, leaves zero preview voices, and
    // records the failure for the status UI. Never throws.
    bool StartPreview(const core::UUID& source, const std::string& sourceName,
                      const AudioSourceComponent& component, bool hasTransform,
                      const float sourcePosition[3],
                      const AudioListenerPose& listener, bool spatialAudition,
                      core::Error& outError);

    // Idempotent stop: true with an empty Error when no preview was live or
    // the live voice detached cleanly. A backend stop/release failure is
    // loud (outError carries it) but still detaches: the census returns to
    // zero and no session-owned sound keeps running (backend teardown
    // contract). Never throws.
    bool StopPreview(core::Error& outError);

    // Explicit teardown for host shutdown paths. Stops any live preview
    // (loud result) and forgets the borrowed seams.
    bool Shutdown(core::Error& outError);

    // Per-frame editor tick: reaps naturally completed/failed preview
    // voices (a finished one-shot ends the preview instead of lingering
    // live) and refreshes the spatial audition mix from the current
    // source/listener poses. Mix failures retain the last valid mix and
    // record a diagnostic; they never stop the preview. Never throws.
    void Update(const AudioListenerPose& listener,
                const float sourcePosition[3], bool hasTransform);

    // No-device pump for the editor frame loop. Renders the elapsed frame
    // time through the production no-device engine into a preallocated
    // scratch buffer (bounded chunks); returns the frames rendered, or zero
    // when there is no live preview voice or the backend is not in
    // production no-device mode. A render failure records the typed error
    // (visible in LastError) and keeps the voice; the next Update reaps
    // whatever the backend reports. Never throws.
    uint32_t PumpNoDeviceFrames(float frameDt);

    bool HasPreview() const { return m_VoiceValid; }
    // 0 when idle, 1 when a preview voice is live. Preview capacity is
    // structural, not borrowed from the runtime cap.
    size_t PreviewVoiceCount() const { return m_VoiceValid ? 1 : 0; }
    core::UUID PreviewSource() const { return m_Source; }
    const std::string& PreviewSourceName() const { return m_SourceName; }
    bool PreviewSpatialAudition() const { return m_SpatialAudition; }
    AudioSessionId Session() const { return m_Session; }

    // Latest typed preview failure plus the affected asset path (clip key or
    // source field path). Cleared on the next successful start. Sticky
    // otherwise, so the status UI can show it after the initiating click.
    const core::Error& LastError() const { return m_LastError; }
    const std::string& LastAffectedAsset() const { return m_LastAffectedAsset; }
    // Latest non-fatal diagnostic (e.g. a retained-mix spatial failure
    // during Update). Never affects the voice.
    const std::string& LastDiagnostic() const { return m_LastDiagnostic; }

    uint64_t StartCount() const { return m_StartCount; }
    uint64_t FailureCount() const { return m_FailureCount; }
    uint64_t NaturalCompletionCount() const { return m_NaturalCompletions; }

    // Observer access for probes: the live backend token, if any.
    bool LiveBackendToken(BackendVoiceToken& outToken) const;

private:
    void RecordFailure(const core::Error& error, const std::string& asset);
    void ClearVoiceState();
    // Best-effort detach used by replacement and destruction: never fails
    // loudly, always leaves zero preview voices.
    void DetachVoiceQuiet();
    BackendVoiceMix CenterMix(const AudioSourceComponent& component) const;

    IAudioBackend* m_Backend = nullptr;
    IAudioClipProvider* m_Provider = nullptr;
    ClipKeyBuilder m_KeyBuilder;

    AudioSessionId m_Session;
    bool m_SessionValid = false;

    core::UUID m_Source;
    std::string m_SourceName;
    AudioSourceComponent m_Component;
    bool m_HasTransform = true;
    float m_SourcePosition[3] = { 0.0f, 0.0f, 0.0f };
    bool m_SpatialAudition = false;

    bool m_VoiceValid = false;
    BackendVoiceToken m_VoiceToken;
    BackendClipHandle m_ClipHandle;
    std::shared_ptr<const DecodedAudioGeneration> m_Generation;
    std::string m_ClipKey;
    BackendVoiceMix m_LastMix;

    core::Error m_LastError;
    std::string m_LastAffectedAsset;
    std::string m_LastDiagnostic;

    uint64_t m_StartCount = 0;
    uint64_t m_FailureCount = 0;
    uint64_t m_NaturalCompletions = 0;

    std::vector<float> m_Scratch;
    double m_FrameFrac = 0.0;
};

} // namespace rt2::audio

#endif // RT2_AUDIO_PREVIEW_CONTROLLER_H
