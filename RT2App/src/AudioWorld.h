#pragma once

#ifndef RT2_AUDIO_WORLD_H
#define RT2_AUDIO_WORLD_H

#include "AudioBackend.h"
#include "core/UUID.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// ============================================================================
// AudioWorld — CPU-only deterministic session policy (audio integration, A3
// policy core).
//
// CPU-only: standard library + AudioBackend + core/UUID only. No miniaudio,
// device, filesystem, Vulkan, ImGui, or Walnut. Links into RT2Tests and
// RT2SliceRunner unchanged.
//
// Owns, per Play session:
//   - source UUID to live voice-handle sets;
//   - the runtime session ID;
//   - a bounded 256-command FIFO queue;
//   - bus gains and pause state;
//   - the last valid source transforms and listener pose;
//   - diagnostics and a voice census.
//
// Policy (READY plan, AudioWorld and voice policy + Script API):
//   - Startup-configurable runtime-voice cap, 64 default. Queue overflow
//     returns false without mutation and records a diagnostic.
//   - A request first drains backend completions and reclaims matching
//     current world generations. If still full, steal the lowest-priority
//     non-looping voice; ties resolve by oldest start sequence then world
//     handle slot. Looping voices are protected unless the incoming request
//     has strictly greater priority. No legal victim: typed failure, voices
//     unchanged.
//   - Non-looping Play retriggers and may overlap up to the cap. Looping
//     Play is idempotent per source. Stop stops every voice owned by a
//     source. World and backend identities are distinct: completions are
//     accepted only when the token matches the backend token currently
//     mapped by that generation.
//   - The drain freezes the queue into a local vector once per presentation
//     frame and processes it FIFO; re-entrant submissions land in the next
//     queue. Destroying UUIDs refuse new commands, drop queued ones, and
//     stop live voices. Pause/Step never advance sample time; a voice
//     created while paused carries initialPaused.
//   - Every accepted command receives a monotonically increasing
//     per-source sequence; every play slot stores its creating play
//     sequence, world generation, and backend token. Aggregate status is
//     overlap-aware: an older voice's completion/failure cannot replace a
//     newer voice's sequence-scoped result.
//
// Clip identity seam: A3 Play requests carry an opaque clipKey standing in
// for the A4 (asset ID, canonical path, content fingerprint, decode format)
// tuple. The world registers one synthetic mono/stereo generation per key
// with the backend. A4 replaces the key with ResolvedAudioClip decode flow;
// the slot/token/status contracts below do not change.
// ============================================================================

namespace rt2::audio {

inline constexpr uint32_t kAudioDefaultVoiceCap = 64;
inline constexpr uint32_t kAudioCommandQueueCapacity = 256;
inline constexpr size_t kAudioMaxDiagnostics = 128;

struct AudioWorldConfig
{
    uint32_t maxVoices = kAudioDefaultVoiceCap; // 0 means "use the default"
};

struct AudioPlayRequest
{
    core::UUID source;
    AudioSourceComponent component;
    float sourcePosition[3] = { 0.0f, 0.0f, 0.0f };
    bool hasTransform = true;
    std::string clipKey; // empty = unbound source (refused loudly)
    int decodedChannels = 1;
};

struct AudioSourcePose
{
    core::UUID source;
    float position[3] = { 0.0f, 0.0f, 0.0f };
    bool hasTransform = true;
};

enum class AudioSourceAggregate : uint8_t
{
    Idle = 0,
    Queued = 1,
    Playing = 2,
    Paused = 3,
    Completed = 4,
    Failed = 5,
};

struct AudioSourceStatus
{
    AudioSourceAggregate aggregate = AudioSourceAggregate::Idle;
    uint32_t liveVoiceCount = 0;
    uint32_t pausedVoiceCount = 0;
    uint64_t newestAcceptedSequence = 0;
    bool newestQueued = false;
    uint64_t lastResultSequence = 0;
    bool hasResult = false;
    bool lastResultOk = false;
    core::Error lastError;
};

struct AudioUpdateStats
{
    uint32_t commandsExecuted = 0;
    uint32_t completionsReclaimed = 0;
    uint32_t staleCompletions = 0;
    uint32_t mixFailures = 0;
    uint32_t voicesStolen = 0;
};

class AudioWorld
{
public:
    // `backend` is borrowed and must outlive the world (the host owns the
    // device/backend; the session owns the world). `session` must be valid.
    // A zero maxVoices config selects the 64-voice default.
    AudioWorld(IAudioBackend* backend, AudioSessionId session,
               AudioOwnerKind owner, AudioWorldConfig config = {});
    ~AudioWorld(); // best-effort StopSessionVoices; use Shutdown for loud errors

    AudioWorld(const AudioWorld&) = delete;
    AudioWorld& operator=(const AudioWorld&) = delete;

    // Queued commands. `true` means validated and accepted into the bounded
    // FIFO, not "audible execution succeeded". Synchronous `false` is
    // limited to destroying targets, queue-full, unbound/malformed
    // requests, and out-of-range values. `outSequence` carries the accepted
    // per-source sequence on success and is untouched on refusal.
    bool QueuePlay(const AudioPlayRequest& request, uint64_t& outSequence);
    bool QueueStop(const core::UUID& source, uint64_t& outSequence);
    bool QueuePause(const core::UUID& source, bool paused, uint64_t& outSequence);
    bool QueueSetGain(const core::UUID& source, float gain, uint64_t& outSequence);
    bool QueueSetPitch(const core::UUID& source, float pitch, uint64_t& outSequence);

    // Presentation frame. Drains backend completions, executes the frozen
    // FIFO exactly once, refreshes mixes from final world poses, then
    // drains completions again. Update advances sample cursors of unpaused
    // voices by framesToAdvance; Step is sample-frozen (advance of zero).
    AudioUpdateStats Update(const AudioListenerPose& listener,
                            const AudioSourcePose* poses, size_t poseCount,
                            uint32_t framesToAdvance);
    AudioUpdateStats Step(const AudioListenerPose& listener,
                          const AudioSourcePose* poses, size_t poseCount);

    // Immediate mixer/session controls (not queued).
    bool SetBusGain(AudioBus bus, float gain, core::Error& outError);
    float BusGain(AudioBus bus) const;
    bool SetSessionPaused(bool paused, core::Error& outError);
    bool IsSessionPaused() const { return m_SessionPaused; }

    // Destruction safe point. Marks the exact destroying set, drops its
    // queued commands, and stops its live voices before ECS removal. The
    // mark stays active until ClearDestroying or StopAllVoices so
    // re-entrant commands during callbacks are refused.
    void NotifySourcesDestroying(const core::UUID* sources, size_t count);
    void ClearDestroying();
    bool IsDestroying(const core::UUID& source) const;

    // Defensive queue clear (Stop/reload/quarantine path; AudioWorld Stop
    // also clears defensively).
    void ClearQueuedCommands();

    // Explicit teardown. Reports the backend result loudly; world state is
    // still returned to baseline (teardown detaches even on backend error).
    // Shutdown additionally releases cached clip generations.
    bool StopAllVoices(core::Error& outError);
    bool Shutdown(core::Error& outError);

    // Identity-safe direct voice operations. A handle names a live voice
    // only when its generation matches the slot's current generation;
    // stale handles refuse with InvalidArgument and mutate nothing.
    bool TryStopVoice(AudioWorldVoiceHandle handle, core::Error& outError);
    bool TrySetVoiceMix(AudioWorldVoiceHandle handle, const BackendVoiceMix& mix,
                        core::Error& outError);

    // Observation (tests, Lua status in A6, mixer UI in A7).
    AudioSourceStatus GetSourceStatus(const core::UUID& source) const;
    size_t LiveVoiceCount() const;
    size_t QueuedCommandCount() const;
    uint32_t SessionVoiceCap() const { return m_MaxVoices; }
    AudioSessionId Session() const { return m_Session; }
    AudioOwnerKind Owner() const { return m_Owner; }
    const std::vector<std::string>& Diagnostics() const { return m_Diagnostics; }
    uint64_t DroppedDiagnosticCount() const { return m_DroppedDiagnostics; }
    uint64_t StealCount() const { return m_StealCount; }
    uint64_t QueueOverflowCount() const { return m_QueueOverflowCount; }
    uint64_t StaleCompletionCount() const { return m_StaleCompletions; }

    // Test/observer access to live slots.
    std::vector<AudioWorldVoiceHandle> LiveVoicesForSource(const core::UUID& source) const;
    bool GetVoiceMix(AudioWorldVoiceHandle handle, BackendVoiceMix& outMix) const;
    bool GetVoiceCursor(AudioWorldVoiceHandle handle, double& outCursorFrames) const;
    bool GetVoicePlaySequence(AudioWorldVoiceHandle handle, uint64_t& outSequence) const;
    bool GetVoiceBackendToken(AudioWorldVoiceHandle handle, BackendVoiceToken& outToken) const;

private:
    enum class CommandKind : uint8_t { Play, Stop, Pause, SetGain, SetPitch };

    struct Command
    {
        CommandKind kind = CommandKind::Play;
        core::UUID source;
        uint64_t sequence = 0;
        // Play payload.
        AudioSourceComponent component;
        float position[3] = { 0.0f, 0.0f, 0.0f };
        bool hasTransform = true;
        std::string clipKey;
        int decodedChannels = 1;
        // Pause/SetGain/SetPitch payload.
        bool pauseValue = false;
        float scalarValue = 0.0f;
    };

    struct VoiceSlot
    {
        bool live = false;
        uint32_t generation = 0;
        core::UUID source;
        uint64_t playSequence = 0;
        uint64_t startOrder = 0;
        uint8_t priority = 128;
        bool loop = false;
        bool paused = false;
        BackendVoiceToken backendToken;
        BackendClipHandle clipHandle;
        std::string clipKey;
        int decodedChannels = 1;
        BackendVoiceMix lastMix;
        double cursorFrames = 0.0;
    };

    struct SourceState
    {
        uint64_t newestAcceptedSequence = 0;
        uint64_t queuedCount = 0;
        bool newestQueued = false;
        uint64_t lastResultSequence = 0;
        bool hasResult = false;
        bool lastResultOk = false;
        core::Error lastError;
        // Newest terminal play outcome. Set when a play executes; refreshed
        // when that play's final voice completes; cleared by Stop/destroy.
        uint64_t terminalPlaySequence = 0;
        bool terminalPlayOk = false;
        bool hasTerminalPlay = false;
        AudioSourceComponent component;
        bool hasComponent = false;
        float lastPosition[3] = { 0.0f, 0.0f, 0.0f };
        bool lastHasTransform = true;
        bool hasPose = false;
    };

    const VoiceSlot* FindSlot(AudioWorldVoiceHandle handle) const;
    VoiceSlot* FindSlot(AudioWorldVoiceHandle handle);
    void RecordDiagnostic(const std::string& message);
    void RecordSourceResult(const core::UUID& source, uint64_t sequence,
                            bool ok, const core::Error& error);
    // A refused/failed play is a failed newest terminal play: it surfaces
    // as Failed once no live or queued voice remains. Never regresses a
    // newer terminal outcome already recorded for the source.
    void NotePlayFailure(const core::UUID& source, uint64_t sequence);
    SourceState& StateFor(const core::UUID& source);
    const SourceState* FindState(const core::UUID& source) const;
    bool ResolveClipHandle(const std::string& clipKey, int decodedChannels,
                           BackendClipHandle& outHandle, core::Error& outError);
    bool ComputeVoiceMix(const VoiceSlot& slot, const SourceState& state,
                         const AudioListenerPose& listener, BackendVoiceMix& outMix,
                         core::Error& outError) const;
    AudioUpdateStats DrainCompletions();
    bool ExecuteCommand(const Command& cmd, const AudioListenerPose& listener,
                        AudioUpdateStats& stats);
    bool ExecutePlay(const Command& cmd, AudioUpdateStats& stats);
    bool ExecuteStop(const Command& cmd);
    bool ExecutePause(const Command& cmd);
    bool ExecuteSetGain(const Command& cmd);
    bool ExecuteSetPitch(const Command& cmd);
    void ReclaimSlot(size_t index, BackendCompletionReason reason,
                     const core::Error& error, AudioUpdateStats* stats);
    void StopVoiceInternal(size_t index, bool countAsStolen);
    int FindStealVictim(uint8_t incomingPriority) const;

    IAudioBackend* m_Backend = nullptr;
    AudioSessionId m_Session;
    AudioOwnerKind m_Owner = AudioOwnerKind::Runtime;
    uint32_t m_MaxVoices = kAudioDefaultVoiceCap;

    std::vector<VoiceSlot> m_Slots;
    std::vector<Command> m_Queue;
    bool m_Draining = false;

    // Per-source bookkeeping.
    std::vector<std::pair<core::UUID, SourceState>> m_Sources;
    std::vector<core::UUID> m_Destroying;

    // clipKey -> backend handle cache for this session.
    std::vector<std::pair<std::string, BackendClipHandle>> m_ClipHandles;

    float m_BusGains[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    bool m_SessionPaused = false;

    AudioListenerPose m_LastListener;
    bool m_HasListener = false;

    std::vector<std::string> m_Diagnostics;
    uint64_t m_DroppedDiagnostics = 0;
    uint64_t m_StealCount = 0;
    uint64_t m_QueueOverflowCount = 0;
    uint64_t m_StaleCompletions = 0;
    uint64_t m_StartOrderCounter = 0;
};

} // namespace rt2::audio

#endif // RT2_AUDIO_WORLD_H
