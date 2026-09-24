#pragma once

#ifndef RT2_FAKE_AUDIO_BACKEND_H
#define RT2_FAKE_AUDIO_BACKEND_H

#include "AudioBackend.h"
#include "AudioClipProvider.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

// ============================================================================
// RecordingFakeAudioBackend — scripted CPU test double for IAudioBackend
// (audio integration, A3 policy core).
//
// CPU-only test support: records every call, scripts typed failures, and
// advances scripted completions without decoding. It also implements
// IAudioClipProvider, serving scripted decoded generations so A3 policy
// tests exercise provider failure, clip identity, and real decoded-channel
// ownership without a miniaudio decoder (which remains A4's work). Links
// into RT2Tests and RT2SliceRunner. Never used by production (A4 owns the
// miniaudio adapter).
//
// Teardown semantics mirror the production contract: StopVoice and
// StopSessionVoices detach/invalidate tokens even when they report an
// error, so an injected failure can never leave a session-owned sound
// reachable afterwards.
//
// Unscripted clip keys yield an explicit test-double default (mono 48 kHz
// silent generation), recorded in generationFetches. This default belongs
// to the double, not the engine: AudioWorld never fabricates PCM and fails
// loudly when the provider does.
// ============================================================================

namespace rt2::audio {

class RecordingFakeAudioBackend final : public IAudioBackend,
                                          public IAudioClipProvider
{
public:
    RecordingFakeAudioBackend() = default;

    // --- IAudioClipProvider ---
    core::Result<std::shared_ptr<const DecodedAudioGeneration>> FetchDecodedGeneration(
        const std::string& clipKey) override;
    void ScriptGeneration(const std::string& clipKey,
                          std::shared_ptr<const DecodedAudioGeneration> generation);
    void ScriptGenerationError(const std::string& clipKey, const core::Error& error);

    // --- IAudioBackend ---
    core::Result<std::shared_ptr<const DecodedAudioGeneration>> DecodeClip(
        const AudioClipBytes& clip) override;
    core::Result<BackendClipHandle> RegisterDecodedGeneration(
        std::shared_ptr<const DecodedAudioGeneration> generation) override;
    bool ReleaseDecodedGeneration(BackendClipHandle handle, core::Error& outError) override;
    core::Result<BackendVoiceToken> StartVoice(
        BackendClipHandle clip, const BackendVoiceStart& start) override;
    // Atomic victim replacement (finding R1): the victim is detached and
    // the replacement started as one capacity-unit commit. Recorded in
    // both stops and starts so the atomic stop+start stays auditable.
    core::Result<BackendVoiceToken> ReplaceVoice(
        BackendVoiceToken victim, BackendClipHandle clip,
        const BackendVoiceStart& start) override;
    void FailNextReplace(const core::Error& error)
    { m_NextReplaceError = error; m_FailNextReplace = true; }
    // Hard backend voice cap (finding R1 discriminator). Zero (default)
    // means unbounded. StartVoice beyond the cap fails loudly; ReplaceVoice
    // is exempt because it commits within one capacity unit.
    void SetMaxLiveVoices(size_t maxLive) { m_MaxLiveVoices = maxLive; }
    size_t PeakLiveTokens() const { return m_PeakLiveTokens; }
    // Generation bound to a registered handle (finding R2 observability).
    // Null when the handle is unknown or released.
    std::shared_ptr<const DecodedAudioGeneration> RegisteredGeneration(
        BackendClipHandle handle) const;
    bool StopVoice(BackendVoiceToken token, core::Error& outError) override;
    bool PauseVoice(BackendVoiceToken token, bool paused, core::Error& outError) override;
    bool SetVoiceMix(BackendVoiceToken token, const BackendVoiceMix& mix,
                     core::Error& outError) override;
    core::Result<std::vector<BackendVoiceCompletion>> DrainCompletions(
        AudioSessionId session) override;
    bool SetSessionPaused(AudioSessionId session, bool paused, core::Error& outError) override;
    bool SetBusGain(AudioBus bus, float gain, core::Error& outError) override;
    bool StopSessionVoices(AudioSessionId session, core::Error& outError) override;
    core::Result<uint32_t> RenderNoDeviceFrames(
        AudioPcmWriteBuffer interleavedStereo, uint32_t requestedFrames) override;
    AudioBackendStatus Status() const override;

    // --- Failure scripts (each fires once unless noted) ---
    void FailNextStart(const core::Error& error) { m_NextStartError = error; m_FailNextStart = true; }
    void FailNextStop(const core::Error& error) { m_NextStopError = error; m_FailNextStop = true; }
    void FailStopForToken(BackendVoiceToken token, const core::Error& error);
    void FailNextPause(const core::Error& error) { m_NextPauseError = error; m_FailNextPause = true; }
    void FailNextMix(const core::Error& error) { m_NextMixError = error; m_FailNextMix = true; }
    void FailNextDrain(const core::Error& error) { m_NextDrainError = error; m_FailNextDrain = true; }
    void FailNextSessionPause(const core::Error& error)
    { m_NextSessionPauseError = error; m_FailNextSessionPause = true; }
    void FailNextBusGain(const core::Error& error) { m_NextBusGainError = error; m_FailNextBusGain = true; }
    void FailNextStopSession(const core::Error& error)
    { m_NextStopSessionError = error; m_FailNextStopSession = true; }
    void FailNextRegister(const core::Error& error)
    { m_NextRegisterError = error; m_FailNextRegister = true; }
    void FailNextRelease(const core::Error& error)
    { m_NextReleaseError = error; m_FailNextRelease = true; }

    // --- Completion scripts ---
    void CompleteToken(BackendVoiceToken token, BackendCompletionReason reason,
                       const core::Error& error = core::Error{});
    // Raw injection for stale/foreign completion tests: enqueues exactly
    // the given event without consulting live-token state.
    void EnqueueCompletion(AudioSessionId session, BackendVoiceToken token,
                           BackendCompletionReason reason,
                           const core::Error& error = core::Error{});
    void ScriptDecode(const std::string& clipKey,
                      std::shared_ptr<const DecodedAudioGeneration> generation);
    void ScriptDecodeError(const std::string& clipKey, const core::Error& error);

    // Re-entrancy hook invoked synchronously inside StartVoice after the
    // token is allocated. Tests use it to prove drain-frozen FIFO timing:
    // commands queued here wait for the next presentation frame.
    std::function<void(BackendVoiceToken)> onStartVoice;

    // --- Recorded traffic ---
    struct StartRecord
    {
        BackendClipHandle clip;
        BackendVoiceStart start;
        BackendVoiceToken token;
    };
    std::vector<StartRecord> starts;
    std::vector<BackendVoiceToken> stops;
    std::vector<std::pair<BackendVoiceToken, bool>> pauses;
    std::vector<std::pair<BackendVoiceToken, BackendVoiceMix>> mixes;
    std::vector<std::pair<AudioSessionId, bool>> sessionPauses;
    std::vector<std::pair<AudioBus, float>> busGains;
    std::vector<AudioSessionId> stopSessions;
    std::vector<BackendClipHandle> releases;
    std::vector<std::string> generationFetches;
    size_t registerCalls = 0;
    size_t drainCalls = 0;

    size_t LiveTokenCount() const { return m_LiveTokens.size(); }
    bool IsTokenLive(BackendVoiceToken token) const;
    size_t RetainedGenerationCount() const { return m_Generations.size(); }
    void ClearRecords();

private:
    uint64_t m_NextHandleId = 1;
    uint64_t m_NextTokenId = 1;

    std::map<uint64_t, std::shared_ptr<const DecodedAudioGeneration>> m_Generations;
    std::map<uint64_t, AudioSessionId> m_LiveTokens; // token -> session
    std::vector<BackendVoiceCompletion> m_PendingCompletions;

    std::map<std::string, std::shared_ptr<const DecodedAudioGeneration>> m_ScriptedDecodes;
    std::map<std::string, core::Error> m_ScriptedDecodeErrors;
    std::map<std::string, std::shared_ptr<const DecodedAudioGeneration>> m_ScriptedGenerations;
    std::map<std::string, core::Error> m_ScriptedGenerationErrors;
    std::map<std::string, std::shared_ptr<const DecodedAudioGeneration>> m_DefaultGenerations;
    std::map<uint64_t, core::Error> m_StopErrorsByToken;

    bool m_FailNextStart = false;
    core::Error m_NextStartError;
    bool m_FailNextStop = false;
    core::Error m_NextStopError;
    bool m_FailNextPause = false;
    core::Error m_NextPauseError;
    bool m_FailNextMix = false;
    core::Error m_NextMixError;
    bool m_FailNextDrain = false;
    core::Error m_NextDrainError;
    bool m_FailNextSessionPause = false;
    core::Error m_NextSessionPauseError;
    bool m_FailNextBusGain = false;
    core::Error m_NextBusGainError;
    bool m_FailNextStopSession = false;
    core::Error m_NextStopSessionError;
    bool m_FailNextRegister = false;
    core::Error m_NextRegisterError;
    bool m_FailNextRelease = false;
    core::Error m_NextReleaseError;
    bool m_FailNextReplace = false;
    core::Error m_NextReplaceError;
    size_t m_MaxLiveVoices = 0;
    size_t m_PeakLiveTokens = 0;

    void NoteLiveChanged()
    {
        if (m_LiveTokens.size() > m_PeakLiveTokens)
            m_PeakLiveTokens = m_LiveTokens.size();
    }
};

} // namespace rt2::audio

#endif // RT2_FAKE_AUDIO_BACKEND_H
