#include "AudioWorld.h"
#include "AudioSpatialMath.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace rt2::audio {

namespace {

bool IsValidBusValue(AudioBus bus)
{
    return bus == AudioBus::Master || bus == AudioBus::Music ||
           bus == AudioBus::Effects || bus == AudioBus::UI;
}

bool MixChanged(const BackendVoiceMix& a, const BackendVoiceMix& b)
{
    return std::fabs(a.left - b.left) > 1.0e-6f ||
           std::fabs(a.right - b.right) > 1.0e-6f ||
           std::fabs(a.pitch - b.pitch) > 1.0e-6f;
}

} // namespace

AudioWorld::AudioWorld(IAudioBackend* backend, AudioSessionId session,
                       AudioOwnerKind owner, AudioWorldConfig config)
    : m_Backend(backend)
    , m_Session(session)
    , m_Owner(owner)
    , m_MaxVoices(config.maxVoices == 0 ? kAudioDefaultVoiceCap : config.maxVoices)
{
    m_Slots.resize(m_MaxVoices);
    m_Queue.reserve(kAudioCommandQueueCapacity);
    m_BusGains[static_cast<int>(AudioBus::Master)] = 1.0f;
    m_BusGains[static_cast<int>(AudioBus::Music)] = 1.0f;
    m_BusGains[static_cast<int>(AudioBus::Effects)] = 1.0f;
    m_BusGains[static_cast<int>(AudioBus::UI)] = 1.0f;
}

AudioWorld::~AudioWorld()
{
    // Best-effort teardown only. The explicit StopAllVoices/Shutdown path
    // reports backend failures loudly; the destructor must not throw or
    // depend on a backend that may already be gone in tests (the host
    // guarantees backend-outlives-world member order in production).
    if (m_Backend != nullptr)
    {
        core::Error ignored;
        m_Backend->StopSessionVoices(m_Session, ignored);
    }
}

AudioWorld::SourceState& AudioWorld::StateFor(const core::UUID& source)
{
    for (auto& entry : m_Sources)
        if (entry.first == source)
            return entry.second;
    m_Sources.emplace_back(source, SourceState{});
    return m_Sources.back().second;
}

const AudioWorld::SourceState* AudioWorld::FindState(const core::UUID& source) const
{
    for (const auto& entry : m_Sources)
        if (entry.first == source)
            return &entry.second;
    return nullptr;
}

void AudioWorld::RecordDiagnostic(const std::string& message)
{
    if (m_Diagnostics.size() >= kAudioMaxDiagnostics)
    {
        m_Diagnostics.erase(m_Diagnostics.begin());
        ++m_DroppedDiagnostics;
    }
    m_Diagnostics.push_back(message);
}

void AudioWorld::RecordSourceResult(const core::UUID& source, uint64_t sequence,
                                    bool ok, const core::Error& error)
{
    SourceState& state = StateFor(source);
    state.lastResultSequence = sequence;
    state.hasResult = true;
    state.lastResultOk = ok;
    state.lastError = error;
}

void AudioWorld::NotePlayFailure(const core::UUID& source, uint64_t sequence)
{
    SourceState& state = StateFor(source);
    if (!state.hasTerminalPlay || sequence >= state.terminalPlaySequence)
    {
        state.terminalPlaySequence = sequence;
        state.terminalPlayOk = false;
        state.hasTerminalPlay = true;
    }
}

const AudioWorld::VoiceSlot* AudioWorld::FindSlot(AudioWorldVoiceHandle handle) const
{
    if (handle.slot >= m_Slots.size())
        return nullptr;
    const VoiceSlot& slot = m_Slots[handle.slot];
    if (!slot.live || slot.generation != handle.generation)
        return nullptr;
    return &slot;
}

AudioWorld::VoiceSlot* AudioWorld::FindSlot(AudioWorldVoiceHandle handle)
{
    return const_cast<VoiceSlot*>(static_cast<const AudioWorld*>(this)->FindSlot(handle));
}

bool AudioWorld::QueuePlay(const AudioPlayRequest& request, uint64_t& outSequence)
{
    if (IsDestroying(request.source))
        return false;
    if (m_Queue.size() >= kAudioCommandQueueCapacity)
    {
        ++m_QueueOverflowCount;
        RecordDiagnostic("audio command queue full (256): Play refused without mutation");
        return false;
    }
    if (request.clipKey.empty())
        return false;
    std::string detail;
    const std::optional<int> channels(request.decodedChannels);
    if (!ValidateAudioSourceComponent(request.component, request.hasTransform,
                                      channels, detail, nullptr))
        return false;

    SourceState& state = StateFor(request.source);
    const uint64_t sequence = state.newestAcceptedSequence + 1;
    state.newestAcceptedSequence = sequence;
    state.newestQueued = true;
    ++state.queuedCount;

    Command cmd;
    cmd.kind = CommandKind::Play;
    cmd.source = request.source;
    cmd.sequence = sequence;
    cmd.component = request.component;
    cmd.position[0] = request.sourcePosition[0];
    cmd.position[1] = request.sourcePosition[1];
    cmd.position[2] = request.sourcePosition[2];
    cmd.hasTransform = request.hasTransform;
    cmd.clipKey = request.clipKey;
    cmd.decodedChannels = request.decodedChannels;
    m_Queue.push_back(std::move(cmd));
    outSequence = sequence;
    return true;
}

bool AudioWorld::QueueStop(const core::UUID& source, uint64_t& outSequence)
{
    if (IsDestroying(source))
        return false;
    if (m_Queue.size() >= kAudioCommandQueueCapacity)
    {
        ++m_QueueOverflowCount;
        RecordDiagnostic("audio command queue full (256): Stop refused without mutation");
        return false;
    }
    SourceState& state = StateFor(source);
    const uint64_t sequence = state.newestAcceptedSequence + 1;
    state.newestAcceptedSequence = sequence;
    state.newestQueued = true;
    ++state.queuedCount;

    Command cmd;
    cmd.kind = CommandKind::Stop;
    cmd.source = source;
    cmd.sequence = sequence;
    m_Queue.push_back(std::move(cmd));
    outSequence = sequence;
    return true;
}

bool AudioWorld::QueuePause(const core::UUID& source, bool paused, uint64_t& outSequence)
{
    if (IsDestroying(source))
        return false;
    if (m_Queue.size() >= kAudioCommandQueueCapacity)
    {
        ++m_QueueOverflowCount;
        RecordDiagnostic("audio command queue full (256): Pause refused without mutation");
        return false;
    }
    SourceState& state = StateFor(source);
    const uint64_t sequence = state.newestAcceptedSequence + 1;
    state.newestAcceptedSequence = sequence;
    state.newestQueued = true;
    ++state.queuedCount;

    Command cmd;
    cmd.kind = CommandKind::Pause;
    cmd.source = source;
    cmd.sequence = sequence;
    cmd.pauseValue = paused;
    m_Queue.push_back(std::move(cmd));
    outSequence = sequence;
    return true;
}

bool AudioWorld::QueueSetGain(const core::UUID& source, float gain, uint64_t& outSequence)
{
    if (IsDestroying(source))
        return false;
    if (!std::isfinite(gain) || gain < 0.0f || gain > 4.0f)
        return false;
    if (m_Queue.size() >= kAudioCommandQueueCapacity)
    {
        ++m_QueueOverflowCount;
        RecordDiagnostic("audio command queue full (256): SetGain refused without mutation");
        return false;
    }
    SourceState& state = StateFor(source);
    const uint64_t sequence = state.newestAcceptedSequence + 1;
    state.newestAcceptedSequence = sequence;
    state.newestQueued = true;
    ++state.queuedCount;

    Command cmd;
    cmd.kind = CommandKind::SetGain;
    cmd.source = source;
    cmd.sequence = sequence;
    cmd.scalarValue = gain;
    m_Queue.push_back(std::move(cmd));
    outSequence = sequence;
    return true;
}

bool AudioWorld::QueueSetPitch(const core::UUID& source, float pitch, uint64_t& outSequence)
{
    if (IsDestroying(source))
        return false;
    if (!std::isfinite(pitch) || pitch < 0.25f || pitch > 4.0f)
        return false;
    if (m_Queue.size() >= kAudioCommandQueueCapacity)
    {
        ++m_QueueOverflowCount;
        RecordDiagnostic("audio command queue full (256): SetPitch refused without mutation");
        return false;
    }
    SourceState& state = StateFor(source);
    const uint64_t sequence = state.newestAcceptedSequence + 1;
    state.newestAcceptedSequence = sequence;
    state.newestQueued = true;
    ++state.queuedCount;

    Command cmd;
    cmd.kind = CommandKind::SetPitch;
    cmd.source = source;
    cmd.sequence = sequence;
    cmd.scalarValue = pitch;
    m_Queue.push_back(std::move(cmd));
    outSequence = sequence;
    return true;
}

bool AudioWorld::ResolveClipHandle(const std::string& clipKey, int decodedChannels,
                                   BackendClipHandle& outHandle, core::Error& outError)
{
    for (const auto& entry : m_ClipHandles)
        if (entry.first == clipKey)
        {
            outHandle = entry.second;
            return true;
        }

    auto generation = std::make_shared<DecodedAudioGeneration>();
    generation->channels = static_cast<uint32_t>(decodedChannels > 0 ? decodedChannels : 1);
    generation->sampleRate = 48000;
    generation->frameCount = 48000;
    generation->pcmInterleaved.assign(
        static_cast<size_t>(generation->frameCount) * generation->channels, 0.0f);

    core::Result<BackendClipHandle> registered =
        m_Backend->RegisterDecodedGeneration(std::move(generation));
    if (!registered.IsOk())
    {
        outError = registered.error;
        return false;
    }
    m_ClipHandles.emplace_back(clipKey, registered.value);
    outHandle = registered.value;
    return true;
}

bool AudioWorld::ComputeVoiceMix(const VoiceSlot& slot, const SourceState& state,
                                 const AudioListenerPose& listener,
                                 BackendVoiceMix& outMix, core::Error& outError) const
{
    AudioSpatialInput input;
    input.component = state.component;
    input.sourcePosition[0] = state.lastPosition[0];
    input.sourcePosition[1] = state.lastPosition[1];
    input.sourcePosition[2] = state.lastPosition[2];
    input.hasTransform = state.lastHasTransform;
    input.decodedChannels = slot.decodedChannels;
    input.listener = listener;
    core::Result<BackendVoiceMix> result =
        ComputeSpatialMix(input, "audio source voice");
    if (!result.IsOk())
    {
        outError = result.error;
        return false;
    }
    outMix = result.value;
    return true;
}

int AudioWorld::FindStealVictim(uint8_t incomingPriority) const
{
    int victim = -1;
    for (size_t i = 0; i < m_Slots.size(); ++i)
    {
        const VoiceSlot& slot = m_Slots[i];
        if (!slot.live)
            continue;
        // Looping voices are protected unless the incoming request has
        // strictly greater priority.
        if (slot.loop && incomingPriority <= slot.priority)
            continue;
        if (victim < 0)
        {
            victim = static_cast<int>(i);
            continue;
        }
        const VoiceSlot& best = m_Slots[static_cast<size_t>(victim)];
        if (slot.priority < best.priority ||
            (slot.priority == best.priority && slot.startOrder < best.startOrder))
            victim = static_cast<int>(i);
        // Further ties resolve by world handle slot: the scan visits slots
        // in ascending order and only replaces on a strict improvement, so
        // the lowest slot wins.
    }
    return victim;
}

void AudioWorld::StopVoiceInternal(size_t index, bool countAsStolen)
{
    VoiceSlot& slot = m_Slots[index];
    if (!slot.live)
        return;
    core::Error error;
    if (!m_Backend->StopVoice(slot.backendToken, error))
        RecordDiagnostic("StopVoice failed for stolen/stopped voice: " + error.Format());
    slot.live = false;
    slot.backendToken = BackendVoiceToken{};
    if (countAsStolen)
        ++m_StealCount;
}

void AudioWorld::ReclaimSlot(size_t index, BackendCompletionReason reason,
                             const core::Error& error, AudioUpdateStats* stats)
{
    VoiceSlot& slot = m_Slots[index];
    if (!slot.live)
        return;
    const core::UUID source = slot.source;
    const uint64_t playSequence = slot.playSequence;
    slot.live = false;
    slot.backendToken = BackendVoiceToken{};
    if (reason == BackendCompletionReason::Failed)
        RecordDiagnostic("backend voice failed: " + error.Format());

    // Overlap rules. A Failed completion refreshes the terminal outcome
    // immediately (when it is not older than the recorded one): only the
    // newest play's failure may stand, and an older voice's later
    // completion must not clear it. Completed/Stopped completions refresh
    // the terminal only for the final voice, so an older voice cannot
    // terminalize a still-audible newer one.
    SourceState& state = StateFor(source);
    if (reason == BackendCompletionReason::Failed)
    {
        if (!state.hasTerminalPlay || playSequence >= state.terminalPlaySequence)
        {
            state.terminalPlaySequence = playSequence;
            state.terminalPlayOk = false;
            state.hasTerminalPlay = true;
        }
        if (stats != nullptr)
            ++stats->completionsReclaimed;
        return;
    }
    bool anyLive = false;
    for (const auto& candidate : m_Slots)
        if (candidate.live && candidate.source == source)
        {
            anyLive = true;
            break;
        }
    if (!anyLive && state.queuedCount == 0 &&
        (!state.hasTerminalPlay || playSequence >= state.terminalPlaySequence))
    {
        state.terminalPlaySequence = playSequence;
        state.terminalPlayOk = (reason == BackendCompletionReason::Completed);
        state.hasTerminalPlay = true;
    }
    if (stats != nullptr)
        ++stats->completionsReclaimed;
}

AudioUpdateStats AudioWorld::DrainCompletions()
{
    AudioUpdateStats stats;
    core::Result<std::vector<BackendVoiceCompletion>> drained =
        m_Backend->DrainCompletions(m_Session);
    if (!drained.IsOk())
    {
        RecordDiagnostic("DrainCompletions failed: " + drained.error.Format());
        return stats;
    }
    for (const BackendVoiceCompletion& completion : drained.value)
    {
        if (completion.session != m_Session)
        {
            ++m_StaleCompletions;
            ++stats.staleCompletions;
            RecordDiagnostic("stale backend completion refused (foreign session)");
            continue;
        }
        int found = -1;
        for (size_t i = 0; i < m_Slots.size(); ++i)
        {
            if (m_Slots[i].live && m_Slots[i].backendToken == completion.token)
            {
                found = static_cast<int>(i);
                break;
            }
        }
        if (found < 0)
        {
            ++m_StaleCompletions;
            ++stats.staleCompletions;
            RecordDiagnostic("stale backend completion refused (no live slot maps the token)");
            continue;
        }
        ReclaimSlot(static_cast<size_t>(found), completion.reason, completion.error, &stats);
    }
    return stats;
}

bool AudioWorld::ExecutePlay(const Command& cmd, AudioUpdateStats& stats)
{
    SourceState& state = StateFor(cmd.source);
    state.component = cmd.component;
    state.hasComponent = true;
    state.lastPosition[0] = cmd.position[0];
    state.lastPosition[1] = cmd.position[1];
    state.lastPosition[2] = cmd.position[2];
    state.lastHasTransform = cmd.hasTransform;
    state.hasPose = true;

    // Looping Play is idempotent per source: a live loop is refreshed, not
    // duplicated.
    for (size_t i = 0; i < m_Slots.size(); ++i)
    {
        VoiceSlot& slot = m_Slots[i];
        if (slot.live && slot.source == cmd.source && slot.loop)
        {
            BackendVoiceMix mix;
            core::Error error;
            if (!ComputeVoiceMix(slot, state, m_LastListener, mix, error))
            {
                RecordSourceResult(cmd.source, cmd.sequence, false, error);
                NotePlayFailure(cmd.source, cmd.sequence);
                RecordDiagnostic("loop refresh mix failed: " + error.Format());
                return false;
            }
            if (!m_Backend->SetVoiceMix(slot.backendToken, mix, error))
            {
                RecordSourceResult(cmd.source, cmd.sequence, false, error);
                NotePlayFailure(cmd.source, cmd.sequence);
                RecordDiagnostic("loop refresh SetVoiceMix failed: " + error.Format());
                return false;
            }
            slot.lastMix = mix;
            RecordSourceResult(cmd.source, cmd.sequence, true, core::Error{});
            state.terminalPlaySequence = cmd.sequence;
            state.terminalPlayOk = true;
            state.hasTerminalPlay = true;
            return true;
        }
    }

    // Find a free slot; otherwise steal deterministically.
    int target = -1;
    for (size_t i = 0; i < m_Slots.size(); ++i)
        if (!m_Slots[i].live)
        {
            target = static_cast<int>(i);
            break;
        }
    if (target < 0)
    {
        target = FindStealVictim(cmd.component.priority);
        if (target < 0)
        {
            core::Error error;
            error.code = core::Error::InvalidRuntimeState;
            error.path = "audio source voice";
            error.detail = "voice cap reached and only protected loops remain";
            RecordSourceResult(cmd.source, cmd.sequence, false, error);
            NotePlayFailure(cmd.source, cmd.sequence);
            RecordDiagnostic("voice steal refused: " + error.Format());
            return false;
        }
        StopVoiceInternal(static_cast<size_t>(target), true);
        ++stats.voicesStolen;
    }

    BackendClipHandle clipHandle;
    {
        core::Error error;
        if (!ResolveClipHandle(cmd.clipKey, cmd.decodedChannels, clipHandle, error))
        {
            RecordSourceResult(cmd.source, cmd.sequence, false, error);
            NotePlayFailure(cmd.source, cmd.sequence);
            RecordDiagnostic("clip generation register failed: " + error.Format());
            return false;
        }
    }

    // Initial mix from the play-time pose; the same frame's mix refresh
    // re-derives it from final world poses when provided.
    AudioSpatialInput input;
    input.component = cmd.component;
    input.sourcePosition[0] = cmd.position[0];
    input.sourcePosition[1] = cmd.position[1];
    input.sourcePosition[2] = cmd.position[2];
    input.hasTransform = cmd.hasTransform;
    input.decodedChannels = cmd.decodedChannels;
    input.listener = m_LastListener;
    core::Result<BackendVoiceMix> initial = ComputeSpatialMix(input, "audio source voice");
    if (!initial.IsOk())
    {
        RecordSourceResult(cmd.source, cmd.sequence, false, initial.error);
        NotePlayFailure(cmd.source, cmd.sequence);
        RecordDiagnostic("play mix failed: " + initial.error.Format());
        return false;
    }

    BackendVoiceStart start;
    start.session = m_Session;
    start.owner = m_Owner;
    start.loop = cmd.component.loop;
    start.initialPaused = m_SessionPaused;
    start.bus = cmd.component.bus;
    start.initialLeft = initial.value.left;
    start.initialRight = initial.value.right;
    start.pitch = initial.value.pitch;

    core::Result<BackendVoiceToken> started = m_Backend->StartVoice(clipHandle, start);
    if (!started.IsOk())
    {
        // No slot consumed: the failure leaves no leaked or half-mapped
        // voice behind.
        RecordSourceResult(cmd.source, cmd.sequence, false, started.error);
        NotePlayFailure(cmd.source, cmd.sequence);
        RecordDiagnostic("backend StartVoice failed: " + started.error.Format());
        return false;
    }

    VoiceSlot& slot = m_Slots[static_cast<size_t>(target)];
    slot.live = true;
    ++slot.generation;
    if (slot.generation == 0)
        ++slot.generation; // never issue generation 0 to a live voice
    slot.source = cmd.source;
    slot.playSequence = cmd.sequence;
    slot.startOrder = m_StartOrderCounter++;
    slot.priority = cmd.component.priority;
    slot.loop = cmd.component.loop;
    // Session-level freezing is governed by m_SessionPaused (and the
    // backend's initialPaused flag); the voice-level flag tracks only
    // explicit per-source pauses. A voice created while paused therefore
    // advances as soon as the session resumes.
    slot.paused = false;
    slot.backendToken = started.value;
    slot.clipHandle = clipHandle;
    slot.clipKey = cmd.clipKey;
    slot.decodedChannels = cmd.decodedChannels;
    slot.lastMix = initial.value;
    slot.cursorFrames = 0.0;

    RecordSourceResult(cmd.source, cmd.sequence, true, core::Error{});
    state.terminalPlaySequence = cmd.sequence;
    state.terminalPlayOk = true;
    state.hasTerminalPlay = true;
    return true;
}

bool AudioWorld::ExecuteStop(const Command& cmd)
{
    for (size_t i = 0; i < m_Slots.size(); ++i)
        if (m_Slots[i].live && m_Slots[i].source == cmd.source)
            StopVoiceInternal(i, false);
    SourceState& state = StateFor(cmd.source);
    state.hasTerminalPlay = false;
    state.terminalPlayOk = false;
    state.terminalPlaySequence = 0;
    RecordSourceResult(cmd.source, cmd.sequence, true, core::Error{});
    return true;
}

bool AudioWorld::ExecutePause(const Command& cmd)
{
    bool ok = true;
    core::Error firstError;
    for (size_t i = 0; i < m_Slots.size(); ++i)
    {
        VoiceSlot& slot = m_Slots[i];
        if (!slot.live || slot.source != cmd.source)
            continue;
        core::Error error;
        if (!m_Backend->PauseVoice(slot.backendToken, cmd.pauseValue, error))
        {
            ok = false;
            if (firstError.IsOk())
                firstError = error;
            RecordDiagnostic("PauseVoice failed: " + error.Format());
        }
        // The world stays authoritative for the sample-time freeze even
        // when the backend reports an error loudly.
        slot.paused = cmd.pauseValue;
    }
    RecordSourceResult(cmd.source, cmd.sequence, ok, firstError);
    return ok;
}

bool AudioWorld::ExecuteSetGain(const Command& cmd)
{
    SourceState& state = StateFor(cmd.source);
    if (!state.hasComponent)
    {
        // No authored snapshot yet: record the value for future plays so
        // gain-then-Play applies in FIFO order without a duplicate voice.
        state.component.gain = cmd.scalarValue;
        state.hasComponent = true;
    }
    else
    {
        state.component.gain = cmd.scalarValue;
    }
    bool ok = true;
    core::Error firstError;
    for (size_t i = 0; i < m_Slots.size(); ++i)
    {
        VoiceSlot& slot = m_Slots[i];
        if (!slot.live || slot.source != cmd.source)
            continue;
        BackendVoiceMix mix;
        core::Error error;
        if (!ComputeVoiceMix(slot, state, m_LastListener, mix, error))
        {
            ok = false;
            if (firstError.IsOk())
                firstError = error;
            RecordDiagnostic("set-gain mix failed: " + error.Format());
            continue;
        }
        if (!m_Backend->SetVoiceMix(slot.backendToken, mix, error))
        {
            ok = false;
            if (firstError.IsOk())
                firstError = error;
            RecordDiagnostic("set-gain SetVoiceMix failed: " + error.Format());
            continue;
        }
        slot.lastMix = mix;
    }
    RecordSourceResult(cmd.source, cmd.sequence, ok, firstError);
    return ok;
}

bool AudioWorld::ExecuteSetPitch(const Command& cmd)
{
    SourceState& state = StateFor(cmd.source);
    if (!state.hasComponent)
    {
        state.component.pitch = cmd.scalarValue;
        state.hasComponent = true;
    }
    else
    {
        state.component.pitch = cmd.scalarValue;
    }
    bool ok = true;
    core::Error firstError;
    for (size_t i = 0; i < m_Slots.size(); ++i)
    {
        VoiceSlot& slot = m_Slots[i];
        if (!slot.live || slot.source != cmd.source)
            continue;
        BackendVoiceMix mix;
        core::Error error;
        if (!ComputeVoiceMix(slot, state, m_LastListener, mix, error))
        {
            ok = false;
            if (firstError.IsOk())
                firstError = error;
            RecordDiagnostic("set-pitch mix failed: " + error.Format());
            continue;
        }
        if (!m_Backend->SetVoiceMix(slot.backendToken, mix, error))
        {
            ok = false;
            if (firstError.IsOk())
                firstError = error;
            RecordDiagnostic("set-pitch SetVoiceMix failed: " + error.Format());
            continue;
        }
        slot.lastMix = mix;
    }
    RecordSourceResult(cmd.source, cmd.sequence, ok, firstError);
    return ok;
}

bool AudioWorld::ExecuteCommand(const Command& cmd, const AudioListenerPose& listener,
                                AudioUpdateStats& stats)
{
    (void)listener;
    switch (cmd.kind)
    {
        case CommandKind::Play:     return ExecutePlay(cmd, stats);
        case CommandKind::Stop:     return ExecuteStop(cmd);
        case CommandKind::Pause:    return ExecutePause(cmd);
        case CommandKind::SetGain:  return ExecuteSetGain(cmd);
        case CommandKind::SetPitch: return ExecuteSetPitch(cmd);
    }
    return false;
}

AudioUpdateStats AudioWorld::Update(const AudioListenerPose& listener,
                                    const AudioSourcePose* poses, size_t poseCount,
                                    uint32_t framesToAdvance)
{
    AudioUpdateStats stats = DrainCompletions();

    // Freeze the queue once per presentation frame. Re-entrant submissions
    // made during backend/error callbacks append to the now-empty live
    // queue and wait for the next frame.
    std::vector<Command> frozen;
    frozen.reserve(m_Queue.size());
    frozen.swap(m_Queue);
    m_Draining = true;

    m_LastListener = listener;
    m_HasListener = true;

    // Execute the frozen FIFO first: play-time request poses seed each new
    // voice, and must not clobber the final world poses stored below.
    for (const Command& cmd : frozen)
    {
        SourceState& state = StateFor(cmd.source);
        if (state.queuedCount > 0)
            --state.queuedCount;
        if (cmd.sequence == state.newestAcceptedSequence)
            state.newestQueued = false;
        if (IsDestroying(cmd.source))
        {
            RecordDiagnostic("queued command dropped for destroying source");
            continue;
        }
        ExecuteCommand(cmd, listener, stats);
        ++stats.commandsExecuted; // counts drained commands, including recorded failures
    }
    m_Draining = false;

    // Final world poses land after command execution so the same frame's
    // mix refresh observes A5's post-transform state, not Lua-time poses.
    if (poses != nullptr)
    {
        for (size_t i = 0; i < poseCount; ++i)
        {
            SourceState& state = StateFor(poses[i].source);
            state.lastPosition[0] = poses[i].position[0];
            state.lastPosition[1] = poses[i].position[1];
            state.lastPosition[2] = poses[i].position[2];
            state.lastHasTransform = poses[i].hasTransform;
            state.hasPose = true;
        }
    }

    // Refresh mixes from final world poses. Failures retain the last valid
    // mix and record a diagnostic without touching command results.
    for (size_t i = 0; i < m_Slots.size(); ++i)
    {
        VoiceSlot& slot = m_Slots[i];
        if (!slot.live)
            continue;
        const SourceState* state = FindState(slot.source);
        if (state == nullptr || !state->hasComponent)
            continue;
        BackendVoiceMix mix;
        core::Error error;
        if (!ComputeVoiceMix(slot, *state, listener, mix, error))
        {
            ++stats.mixFailures;
            RecordDiagnostic("mix update failed, last valid mix retained: " + error.Format());
            continue;
        }
        if (MixChanged(slot.lastMix, mix))
        {
            if (m_Backend->SetVoiceMix(slot.backendToken, mix, error))
                slot.lastMix = mix;
            else
                RecordDiagnostic("mix publish failed, last valid mix retained: " +
                                 error.Format());
        }
    }

    AudioUpdateStats tail = DrainCompletions();
    stats.completionsReclaimed += tail.completionsReclaimed;
    stats.staleCompletions += tail.staleCompletions;

    // Sample-time advance: only ordinary Updates move unpaused voices.
    if (framesToAdvance > 0 && !m_SessionPaused)
    {
        for (auto& slot : m_Slots)
            if (slot.live && !slot.paused)
                slot.cursorFrames += static_cast<double>(framesToAdvance);
    }
    return stats;
}

AudioUpdateStats AudioWorld::Step(const AudioListenerPose& listener,
                                  const AudioSourcePose* poses, size_t poseCount)
{
    // Step processes queued commands, source/listener state, and mix
    // refresh while the session remains sample-frozen.
    return Update(listener, poses, poseCount, 0);
}

bool AudioWorld::SetBusGain(AudioBus bus, float gain, core::Error& outError)
{
    if (!IsValidBusValue(bus))
    {
        outError.code = core::Error::InvalidArgument;
        outError.path = "audio bus gain";
        outError.detail = "unknown bus value";
        return false;
    }
    if (!std::isfinite(gain) || gain < 0.0f || gain > 4.0f)
    {
        outError.code = core::Error::InvalidArgument;
        outError.path = "audio bus gain";
        outError.detail = "bus gain must be finite and in [0, 4]";
        return false;
    }
    if (!m_Backend->SetBusGain(bus, gain, outError))
    {
        RecordDiagnostic("SetBusGain failed: " + outError.Format());
        return false;
    }
    m_BusGains[static_cast<int>(bus)] = gain;
    return true;
}

float AudioWorld::BusGain(AudioBus bus) const
{
    if (!IsValidBusValue(bus))
        return 0.0f;
    return m_BusGains[static_cast<int>(bus)];
}

bool AudioWorld::SetSessionPaused(bool paused, core::Error& outError)
{
    // The single atomic backend boundary: the world claims the new pause
    // state only when the backend accepts it.
    if (!m_Backend->SetSessionPaused(m_Session, paused, outError))
    {
        RecordDiagnostic("SetSessionPaused failed: " + outError.Format());
        return false;
    }
    m_SessionPaused = paused;
    return true;
}

void AudioWorld::NotifySourcesDestroying(const core::UUID* sources, size_t count)
{
    if (sources == nullptr)
        return;
    for (size_t n = 0; n < count; ++n)
    {
        const core::UUID& source = sources[n];
        bool already = false;
        for (const auto& marked : m_Destroying)
            if (marked == source)
            {
                already = true;
                break;
            }
        if (!already)
            m_Destroying.push_back(source);

        // Drop queued commands for the destroying source.
        SourceState& state = StateFor(source);
        std::vector<Command> kept;
        kept.reserve(m_Queue.size());
        for (const Command& cmd : m_Queue)
        {
            if (cmd.source == source)
            {
                if (state.queuedCount > 0)
                    --state.queuedCount;
                if (cmd.sequence == state.newestAcceptedSequence)
                    state.newestQueued = false;
                continue;
            }
            kept.push_back(cmd);
        }
        m_Queue.swap(kept);

        // Stop live voices before ECS removal. An entity-owned voice may
        // not outlive its entity.
        for (size_t i = 0; i < m_Slots.size(); ++i)
            if (m_Slots[i].live && m_Slots[i].source == source)
                StopVoiceInternal(i, false);
        state.hasTerminalPlay = false;
        RecordDiagnostic("source destroying: voices stopped and queued commands dropped");
    }
}

void AudioWorld::ClearDestroying()
{
    m_Destroying.clear();
}

bool AudioWorld::IsDestroying(const core::UUID& source) const
{
    for (const auto& marked : m_Destroying)
        if (marked == source)
            return true;
    return false;
}

void AudioWorld::ClearQueuedCommands()
{
    for (const Command& cmd : m_Queue)
    {
        SourceState* state = nullptr;
        for (auto& entry : m_Sources)
            if (entry.first == cmd.source)
            {
                state = &entry.second;
                break;
            }
        if (state == nullptr)
            continue;
        if (state->queuedCount > 0)
            --state->queuedCount;
        if (cmd.sequence == state->newestAcceptedSequence)
            state->newestQueued = false;
    }
    m_Queue.clear();
}

bool AudioWorld::StopAllVoices(core::Error& outError)
{
    core::Error backendError;
    const bool backendOk = m_Backend->StopSessionVoices(m_Session, backendError);
    // Teardown detaches even when the backend reports an error loudly.
    for (size_t i = 0; i < m_Slots.size(); ++i)
    {
        m_Slots[i].live = false;
        m_Slots[i].backendToken = BackendVoiceToken{};
    }
    ClearQueuedCommands();
    for (auto& entry : m_Sources)
        entry.second.hasTerminalPlay = false;
    m_Destroying.clear();
    if (!backendOk)
    {
        outError = backendError;
        RecordDiagnostic("StopSessionVoices failed: " + backendError.Format());
        return false;
    }
    outError = core::Error{};
    return true;
}

bool AudioWorld::Shutdown(core::Error& outError)
{
    bool ok = StopAllVoices(outError);
    core::Error firstError = outError;
    for (const auto& entry : m_ClipHandles)
    {
        core::Error releaseError;
        if (!m_Backend->ReleaseDecodedGeneration(entry.second, releaseError))
        {
            ok = false;
            if (firstError.IsOk())
                firstError = releaseError;
            RecordDiagnostic("ReleaseDecodedGeneration failed: " + releaseError.Format());
        }
    }
    m_ClipHandles.clear();
    outError = firstError;
    return ok;
}

bool AudioWorld::TryStopVoice(AudioWorldVoiceHandle handle, core::Error& outError)
{
    VoiceSlot* slot = FindSlot(handle);
    if (slot == nullptr)
    {
        outError.code = core::Error::InvalidArgument;
        outError.path = "audio voice handle";
        outError.detail = "stale or unknown world voice generation";
        return false;
    }
    const size_t index = static_cast<size_t>(handle.slot);
    StopVoiceInternal(index, false);
    outError = core::Error{};
    return true;
}

bool AudioWorld::TrySetVoiceMix(AudioWorldVoiceHandle handle, const BackendVoiceMix& mix,
                                core::Error& outError)
{
    VoiceSlot* slot = FindSlot(handle);
    if (slot == nullptr)
    {
        outError.code = core::Error::InvalidArgument;
        outError.path = "audio voice handle";
        outError.detail = "stale or unknown world voice generation";
        return false;
    }
    if (!m_Backend->SetVoiceMix(slot->backendToken, mix, outError))
    {
        RecordDiagnostic("TrySetVoiceMix failed: " + outError.Format());
        return false;
    }
    slot->lastMix = mix;
    return true;
}

AudioSourceStatus AudioWorld::GetSourceStatus(const core::UUID& source) const
{
    AudioSourceStatus status;
    const SourceState* state = FindState(source);
    if (state == nullptr)
        return status;

    uint32_t live = 0;
    uint32_t paused = 0;
    for (const auto& slot : m_Slots)
        if (slot.live && slot.source == source)
        {
            ++live;
            if (slot.paused || m_SessionPaused)
                ++paused;
        }

    status.liveVoiceCount = live;
    status.pausedVoiceCount = paused;
    status.newestAcceptedSequence = state->newestAcceptedSequence;
    status.newestQueued = state->newestQueued;
    status.lastResultSequence = state->lastResultSequence;
    status.hasResult = state->hasResult;
    status.lastResultOk = state->lastResultOk;
    status.lastError = state->lastError;

    if (state->newestQueued || state->queuedCount > 0)
        status.aggregate = AudioSourceAggregate::Queued;
    else if (live > 0 && paused < live)
        status.aggregate = AudioSourceAggregate::Playing;
    else if (live > 0)
        status.aggregate = AudioSourceAggregate::Paused;
    else if (!state->hasTerminalPlay)
        status.aggregate = AudioSourceAggregate::Idle;
    else if (state->terminalPlayOk)
        status.aggregate = AudioSourceAggregate::Completed;
    else
        status.aggregate = AudioSourceAggregate::Failed;
    return status;
}

size_t AudioWorld::LiveVoiceCount() const
{
    size_t count = 0;
    for (const auto& slot : m_Slots)
        if (slot.live)
            ++count;
    return count;
}

size_t AudioWorld::QueuedCommandCount() const
{
    return m_Queue.size();
}

std::vector<AudioWorldVoiceHandle> AudioWorld::LiveVoicesForSource(
    const core::UUID& source) const
{
    std::vector<AudioWorldVoiceHandle> handles;
    for (size_t i = 0; i < m_Slots.size(); ++i)
        if (m_Slots[i].live && m_Slots[i].source == source)
        {
            AudioWorldVoiceHandle handle;
            handle.slot = static_cast<uint32_t>(i);
            handle.generation = m_Slots[i].generation;
            handles.push_back(handle);
        }
    return handles;
}

bool AudioWorld::GetVoiceMix(AudioWorldVoiceHandle handle, BackendVoiceMix& outMix) const
{
    const VoiceSlot* slot = FindSlot(handle);
    if (slot == nullptr)
        return false;
    outMix = slot->lastMix;
    return true;
}

bool AudioWorld::GetVoiceCursor(AudioWorldVoiceHandle handle, double& outCursorFrames) const
{
    const VoiceSlot* slot = FindSlot(handle);
    if (slot == nullptr)
        return false;
    outCursorFrames = slot->cursorFrames;
    return true;
}

bool AudioWorld::GetVoicePlaySequence(AudioWorldVoiceHandle handle, uint64_t& outSequence) const
{
    const VoiceSlot* slot = FindSlot(handle);
    if (slot == nullptr)
        return false;
    outSequence = slot->playSequence;
    return true;
}

bool AudioWorld::GetVoiceBackendToken(AudioWorldVoiceHandle handle,
                                      BackendVoiceToken& outToken) const
{
    const VoiceSlot* slot = FindSlot(handle);
    if (slot == nullptr)
        return false;
    outToken = slot->backendToken;
    return true;
}

} // namespace rt2::audio
