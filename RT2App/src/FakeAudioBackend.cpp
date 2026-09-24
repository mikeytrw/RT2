#include "FakeAudioBackend.h"

namespace rt2::audio {

core::Result<std::shared_ptr<const DecodedAudioGeneration>> RecordingFakeAudioBackend::DecodeClip(
    const AudioClipBytes& clip)
{
    auto errorIt = m_ScriptedDecodeErrors.find(clip.clipKey);
    if (errorIt != m_ScriptedDecodeErrors.end())
        return core::Result<std::shared_ptr<const DecodedAudioGeneration>>::Fail(
            errorIt->second.code, errorIt->second.path, errorIt->second.detail);
    auto it = m_ScriptedDecodes.find(clip.clipKey);
    if (it != m_ScriptedDecodes.end())
        return core::Result<std::shared_ptr<const DecodedAudioGeneration>>::Ok(it->second);
    return core::Result<std::shared_ptr<const DecodedAudioGeneration>>::Fail(
        core::Error::InvalidRuntimeState, clip.clipKey,
        "A3 fake: no scripted decode for this clip key (decode is an A4 seam)");
}

core::Result<BackendClipHandle> RecordingFakeAudioBackend::RegisterDecodedGeneration(
    std::shared_ptr<const DecodedAudioGeneration> generation)
{
    ++registerCalls;
    if (m_FailNextRegister)
    {
        m_FailNextRegister = false;
        return core::Result<BackendClipHandle>::Fail(
            m_NextRegisterError.code, m_NextRegisterError.path, m_NextRegisterError.detail);
    }
    BackendClipHandle handle;
    handle.opaque = m_NextHandleId++;
    m_Generations[handle.opaque] = std::move(generation);
    return core::Result<BackendClipHandle>::Ok(handle);
}

bool RecordingFakeAudioBackend::ReleaseDecodedGeneration(
    BackendClipHandle handle, core::Error& outError)
{
    if (m_FailNextRelease)
    {
        m_FailNextRelease = false;
        outError = m_NextReleaseError;
        return false;
    }
    auto it = m_Generations.find(handle.opaque);
    if (it == m_Generations.end())
    {
        outError.code = core::Error::InvalidArgument;
        outError.path = "audio clip generation";
        outError.detail = "unknown backend clip handle";
        return false;
    }
    releases.push_back(handle);
    m_Generations.erase(it);
    outError = core::Error{};
    return true;
}

core::Result<BackendVoiceToken> RecordingFakeAudioBackend::StartVoice(
    BackendClipHandle clip, const BackendVoiceStart& start)
{
    if (m_FailNextStart)
    {
        m_FailNextStart = false;
        return core::Result<BackendVoiceToken>::Fail(
            m_NextStartError.code, m_NextStartError.path, m_NextStartError.detail);
    }
    if (m_Generations.find(clip.opaque) == m_Generations.end())
    {
        core::Error error;
        error.code = core::Error::InvalidArgument;
        error.path = "audio voice start";
        error.detail = "unknown backend clip handle";
        return core::Result<BackendVoiceToken>::Fail(error.code, error.path, error.detail);
    }
    BackendVoiceToken token;
    token.opaque = m_NextTokenId++;
    m_LiveTokens[token.opaque] = start.session;
    starts.push_back(StartRecord{ clip, start, token });
    if (onStartVoice)
        onStartVoice(token);
    return core::Result<BackendVoiceToken>::Ok(token);
}

bool RecordingFakeAudioBackend::StopVoice(BackendVoiceToken token, core::Error& outError)
{
    // Teardown detaches even on error: the token is invalidated before
    // returning so a failure cannot leave a session-owned sound reachable.
    const bool known = (m_LiveTokens.find(token.opaque) != m_LiveTokens.end());
    m_LiveTokens.erase(token.opaque);
    stops.push_back(token);
    auto scripted = m_StopErrorsByToken.find(token.opaque);
    if (scripted != m_StopErrorsByToken.end())
    {
        outError = scripted->second;
        m_StopErrorsByToken.erase(scripted);
        return false;
    }
    if (m_FailNextStop)
    {
        m_FailNextStop = false;
        outError = m_NextStopError;
        return false;
    }
    if (!known)
    {
        outError.code = core::Error::InvalidArgument;
        outError.path = "audio voice stop";
        outError.detail = "unknown backend voice token";
        return false;
    }
    outError = core::Error{};
    return true;
}

bool RecordingFakeAudioBackend::PauseVoice(
    BackendVoiceToken token, bool paused, core::Error& outError)
{
    pauses.emplace_back(token, paused);
    if (m_FailNextPause)
    {
        m_FailNextPause = false;
        outError = m_NextPauseError;
        return false;
    }
    if (m_LiveTokens.find(token.opaque) == m_LiveTokens.end())
    {
        outError.code = core::Error::InvalidArgument;
        outError.path = "audio voice pause";
        outError.detail = "unknown backend voice token";
        return false;
    }
    outError = core::Error{};
    return true;
}

bool RecordingFakeAudioBackend::SetVoiceMix(
    BackendVoiceToken token, const BackendVoiceMix& mix, core::Error& outError)
{
    mixes.emplace_back(token, mix);
    if (m_FailNextMix)
    {
        m_FailNextMix = false;
        outError = m_NextMixError;
        return false;
    }
    if (m_LiveTokens.find(token.opaque) == m_LiveTokens.end())
    {
        outError.code = core::Error::InvalidArgument;
        outError.path = "audio voice mix";
        outError.detail = "unknown backend voice token";
        return false;
    }
    outError = core::Error{};
    return true;
}

core::Result<std::vector<BackendVoiceCompletion>> RecordingFakeAudioBackend::DrainCompletions(
    AudioSessionId session)
{
    ++drainCalls;
    if (m_FailNextDrain)
    {
        m_FailNextDrain = false;
        return core::Result<std::vector<BackendVoiceCompletion>>::Fail(
            m_NextDrainError.code, m_NextDrainError.path, m_NextDrainError.detail);
    }
    std::vector<BackendVoiceCompletion> due;
    std::vector<BackendVoiceCompletion> kept;
    for (const auto& completion : m_PendingCompletions)
    {
        if (completion.session == session)
            due.push_back(completion);
        else
            kept.push_back(completion);
    }
    m_PendingCompletions.swap(kept);
    // Completed/stopped tokens leave the live set; failed tokens are
    // detached the same way (teardown is loud but final).
    for (const auto& completion : due)
        m_LiveTokens.erase(completion.token.opaque);
    return core::Result<std::vector<BackendVoiceCompletion>>::Ok(std::move(due));
}

bool RecordingFakeAudioBackend::SetSessionPaused(
    AudioSessionId session, bool paused, core::Error& outError)
{
    sessionPauses.emplace_back(session, paused);
    if (m_FailNextSessionPause)
    {
        m_FailNextSessionPause = false;
        outError = m_NextSessionPauseError;
        return false;
    }
    outError = core::Error{};
    return true;
}

bool RecordingFakeAudioBackend::SetBusGain(AudioBus bus, float gain, core::Error& outError)
{
    busGains.emplace_back(bus, gain);
    if (m_FailNextBusGain)
    {
        m_FailNextBusGain = false;
        outError = m_NextBusGainError;
        return false;
    }
    outError = core::Error{};
    return true;
}

bool RecordingFakeAudioBackend::StopSessionVoices(AudioSessionId session, core::Error& outError)
{
    stopSessions.push_back(session);
    for (auto it = m_LiveTokens.begin(); it != m_LiveTokens.end();)
    {
        if (it->second == session)
            it = m_LiveTokens.erase(it);
        else
            ++it;
    }
    if (m_FailNextStopSession)
    {
        m_FailNextStopSession = false;
        outError = m_NextStopSessionError;
        return false;
    }
    outError = core::Error{};
    return true;
}

core::Result<uint32_t> RecordingFakeAudioBackend::RenderNoDeviceFrames(
    AudioPcmWriteBuffer interleavedStereo, uint32_t requestedFrames)
{
    (void)interleavedStereo;
    (void)requestedFrames;
    return core::Result<uint32_t>::Fail(
        core::Error::InvalidRuntimeState, "audio no-device render",
        "A3 fake has no PCM path (production rendering is an A4 seam)");
}

AudioBackendStatus RecordingFakeAudioBackend::Status() const
{
    AudioBackendStatus status;
    status.productionNoDevice = false;
    status.detail = "recording fake";
    return status;
}

void RecordingFakeAudioBackend::FailStopForToken(
    BackendVoiceToken token, const core::Error& error)
{
    m_StopErrorsByToken[token.opaque] = error;
}

void RecordingFakeAudioBackend::CompleteToken(
    BackendVoiceToken token, BackendCompletionReason reason, const core::Error& error)
{
    auto it = m_LiveTokens.find(token.opaque);
    AudioSessionId session;
    if (it != m_LiveTokens.end())
        session = it->second;
    BackendVoiceCompletion completion;
    completion.session = session;
    completion.token = token;
    completion.reason = reason;
    completion.error = error;
    m_PendingCompletions.push_back(completion);
}

void RecordingFakeAudioBackend::EnqueueCompletion(
    AudioSessionId session, BackendVoiceToken token,
    BackendCompletionReason reason, const core::Error& error)
{
    BackendVoiceCompletion completion;
    completion.session = session;
    completion.token = token;
    completion.reason = reason;
    completion.error = error;
    m_PendingCompletions.push_back(completion);
}

void RecordingFakeAudioBackend::ScriptDecode(
    const std::string& clipKey,
    std::shared_ptr<const DecodedAudioGeneration> generation)
{
    m_ScriptedDecodes[clipKey] = std::move(generation);
}

void RecordingFakeAudioBackend::ScriptDecodeError(
    const std::string& clipKey, const core::Error& error)
{
    m_ScriptedDecodeErrors[clipKey] = error;
}

bool RecordingFakeAudioBackend::IsTokenLive(BackendVoiceToken token) const
{
    return m_LiveTokens.find(token.opaque) != m_LiveTokens.end();
}

void RecordingFakeAudioBackend::ClearRecords()
{
    starts.clear();
    stops.clear();
    pauses.clear();
    mixes.clear();
    sessionPauses.clear();
    busGains.clear();
    stopSessions.clear();
    releases.clear();
    registerCalls = 0;
    drainCalls = 0;
}

} // namespace rt2::audio
