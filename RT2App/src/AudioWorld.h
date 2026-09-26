#pragma once

#ifndef RT2_AUDIO_WORLD_H
#define RT2_AUDIO_WORLD_H

#include "AudioBackend.h"
#include "AudioClipProvider.h"
#include "core/UUID.h"

#include <cstddef>
#include <cstdint>
#include <map>
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
// tuple. The world fetches one immutable generation per key from the
// injected IAudioClipProvider and registers that shared generation with the
// backend, so decoded channels, identity, and failure are owned by real
// provider content rather than caller claims. A4 replaces the test
// provider with ResolvedAudioClip decode flow; the slot/token/status
// contracts below do not change.
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
    // A6: one-shot positional override (audio_play_at). Play leaves this
    // false and the play-time pose is the source pose; PlayAt sets it so
    // the override survives final-pose landing and per-voice mix refresh.
    bool positionalOverride = false;
    // NOTE: no channel-count claim. The decoded channel count is owned by
    // the generation the injected IAudioClipProvider returns and is
    // enforced at drain time; a caller can never talk the world into
    // treating stereo bytes as mono.
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
    // `backend` and `provider` are borrowed and must outlive the world (the
    // host owns the device/backend; the session owns the world). `session`
    // must be valid.
    // A zero maxVoices config selects the 64-voice default.
    AudioWorld(IAudioBackend* backend, IAudioClipProvider* provider,
               AudioSessionId session, AudioOwnerKind owner,
               AudioWorldConfig config = {});
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

    // --- A5 fixup: split semantic publication from sample accounting ---
    //
    // Hosts with a PCM sink must render between the two: UpdateSemantic
    // drains commands (including first-frame autoplay), lands final poses,
    // and publishes mixes WITHOUT moving cursors; the host then renders
    // exactly the voices/mixes the semantic phase published; AdvanceCursors
    // moves only live unpaused voices of an unpaused session by the frames
    // actually rendered. Rendering before the semantic phase would emit a
    // silent first block while advancing the new voice's cursor, and would
    // delay same-frame Stop/motion by one frame. Update above is the exact
    // composition (semantic, then advance) so queue/drain/status contracts
    // are unchanged for sink-free hosts.
    AudioUpdateStats UpdateSemantic(const AudioListenerPose& listener,
                                    const AudioSourcePose* poses,
                                    size_t poseCount);
    void AdvanceCursors(uint32_t frames);

    // --- A5 re-review P2: post-render completion reconciliation ---
    //
    // Non-blocking backend completion poll for the PCM block the host just
    // rendered. A voice that naturally completed mid-block (one-shot source
    // cursor exhausted, device at end) is reclaimed here — status, census,
    // and cursor settle before the frame returns instead of lingering
    // live/Playing into the next script update. The controller calls this
    // after rendering and before advancing survivors, so a completed voice
    // never advances and only rendered voices move. Safe to call with no
    // render (polls nothing new) and on sink-free hosts (equivalent to the
    // tail drain already inside UpdateSemantic).
    AudioUpdateStats ReconcilePostRender();

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

    // --- A5 autoplay staging (runtime lifecycle) ---
    //
    // The Play candidate records every bound autoplay source here WITHOUT
    // queueing: no sequence is consumed and no backend/provider call is
    // made, so a refused Play publishes no voice, callback, or bridge
    // operation. The first Update/Step drains accepted commands (e.g.
    // on_create work) first and only then synthesizes one play per
    // still-enabled, unsuppressed, non-destroying staged source, giving
    // on_create deterministic precedence over first-frame autoplay. A
    // per-source Stop/Pause (queued or executed) suppresses its pending
    // autoplay even when no voice exists. An explicitly executed Play for a
    // staged source consumes its staging, so gain/pitch-then-Play applies
    // in FIFO order without a duplicate voice. Staging is one-shot: the
    // first Update/Step consumes the whole set.
    void StageAutoplay(const AudioPlayRequest& request);
    bool HasStagedAutoplay(const core::UUID& source) const;
    size_t StagedAutoplayCount() const { return m_StagedAutoplay.size(); }

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
    size_t CachedClipGenerationCount() const { return m_ClipHandles.size(); }

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
        // A6: true when the play-time pose is a one-shot positional
        // override (audio_play_at) rather than the source pose. The drain
        // maps it onto the voice so final-pose landing (source-level) and
        // mix refresh (per-voice) cannot clobber it — overlapping one-shots
        // keep distinct positions.
        bool positionalOverride = false;
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
        // A6: one-shot positional override for this voice (audio_play_at).
        // Mix refresh spatializes from here while set, so the source-level
        // final pose never drags an override voice back to the entity.
        bool hasPositionalOverride = false;
        float overridePosition[3] = { 0.0f, 0.0f, 0.0f };
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
    bool ResolveClipHandle(const std::string& clipKey, BackendClipHandle& outHandle,
                           std::shared_ptr<const DecodedAudioGeneration>& outGeneration,
                           core::Error& outError);
    bool ComputeVoiceMix(const VoiceSlot& slot, const SourceState& state,
                         const AudioListenerPose& listener, BackendVoiceMix& outMix,
                         core::Error& outError) const;
    AudioUpdateStats DrainCompletions();
    bool ExecuteCommand(const Command& cmd, const AudioListenerPose& listener,
                        AudioUpdateStats& stats);
    bool ExecutePlay(const Command& cmd, AudioUpdateStats& stats);
    // Records autoplay suppression for a staged source. Inserts only while
    // its staging is still pending, so post-synthesis stops cannot pollute
    // a future session's one-shot set.
    void SuppressAutoplay(const core::UUID& source);
    bool IsAutoplaySuppressed(const core::UUID& source) const;
    // Synthesizes one play per pending staged source after the frozen FIFO
    // has drained and final poses have landed. Consumes the whole staged
    // set (enabled or suppressed) exactly once.
    void SynthesizeStagedAutoplay(AudioUpdateStats& stats);
    bool ExecuteStop(const Command& cmd);
    bool ExecutePause(const Command& cmd);
    bool ExecuteSetGain(const Command& cmd);
    bool ExecuteSetPitch(const Command& cmd);
    void ReclaimSlot(size_t index, BackendCompletionReason reason,
                     const core::Error& error, AudioUpdateStats* stats);
    // Stops and frees a live slot. Always detaches (even when the backend
    // reports an error) and reports the backend result, so explicit Stop
    // callers observe typed failures while the census stays correct.
    bool StopVoiceInternal(size_t index, bool countAsStolen, core::Error& outError);
    int FindStealVictim(uint8_t incomingPriority) const;

    IAudioBackend* m_Backend = nullptr;
    IAudioClipProvider* m_Provider = nullptr;
    AudioSessionId m_Session;
    AudioOwnerKind m_Owner = AudioOwnerKind::Runtime;
    uint32_t m_MaxVoices = kAudioDefaultVoiceCap;

    std::vector<VoiceSlot> m_Slots;
    std::vector<Command> m_Queue;
    bool m_Draining = false;

    // Per-source bookkeeping. A node-based map (never reallocated by
    // insert) so a backend callback that registers a new source cannot
    // invalidate state other commands are working with (finding 3). Every
    // backend boundary below additionally re-finds its state by UUID.
    std::map<core::UUID, SourceState> m_Sources;
    std::vector<core::UUID> m_Destroying;

    // clipKey -> backend handle cache for this session. The entry binds
    // the registered backend handle to the exact generation object it was
    // registered for: a re-fetched generation under the same key is a new
    // immutable identity and registers anew, so a new channel layout can
    // never pair with an old backend clip (finding R2). Old entries stay
    // cached until Shutdown, so overlapping old/new generations of one key
    // remain valid independently.
    struct ClipCacheEntry
    {
        std::string clipKey;
        std::shared_ptr<const DecodedAudioGeneration> generation;
        BackendClipHandle handle;
    };
    std::vector<ClipCacheEntry> m_ClipHandles;

    // A5 staged autoplay (source UUID -> Play-time request). Populated by
    // the Play candidate only; consumed once by the first Update/Step.
    std::map<core::UUID, AudioPlayRequest> m_StagedAutoplay;
    // A5 pending-autoplay suppression marks. Populated by per-source
    // Stop/Pause while staging is pending; consumed with the staged set.
    std::vector<core::UUID> m_AutoplaySuppressed;

    float m_BusGains[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    bool m_SessionPaused = false;
    // Bumped by StopAllVoices (and therefore Shutdown). A start prepared
    // before a reentrant session stop must not be mapped afterwards; the
    // world compares this across backend callbacks (finding R3).
    uint64_t m_SessionEpoch = 0;

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
