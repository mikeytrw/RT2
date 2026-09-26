#include <doctest/doctest.h>

// ============================================================================
// A3 - CPU audio world and voice policy (grounded at 834a141).
//
// Device-independent session policy behind IAudioBackend plus a recording
// fake. Every case below is discriminating: each names the single mutation
// that must turn it red (rather than merely checking that a call happened).
//
// Out of scope (explicit): miniaudio decode/device (A4), ECS Play wiring
// and listener injection (A5), Lua controls (A6), inspector/preview/status
// UI (A7), acceptance scene/docs (A8), and any pinball sound content.
//
// CPU boundary: this file includes only portable audio headers plus the
// standard library. The hard #error guard fails the build if miniaudio.h
// ever becomes reachable from RT2Tests (required check 17).
// ============================================================================

#if __has_include("miniaudio.h")
#error "A3 boundary: RT2Tests must not import miniaudio (check 17; the adapter stays in RT2AudioBackend)"
#endif

#include "AudioBackend.h"
#include "AudioSpatialMath.h"
#include "AudioWorld.h"
#include "FakeAudioBackend.h"
#include "core/UUID.h"

#include <array>
#include <cmath>
#include <limits>
#include <string>

using namespace rt2::audio;
using rt2::core::Error;
using rt2::core::UUID;

namespace
{

constexpr float kCenter = 0.70710678f; // cos(pi/4): equal-power center

uint64_t g_UuidCounter = 0;

UUID NextUuid()
{
    ++g_UuidCounter;
    std::array<uint8_t, 16> bytes{};
    uint64_t n = g_UuidCounter;
    for (int i = 0; i < 8; ++i)
        bytes[15 - i] = static_cast<uint8_t>(n >> (8 * i));
    return UUID(bytes);
}

AudioSessionId TestSession()
{
    AudioSessionId session;
    session.value = 1234;
    return session;
}

AudioListenerPose TestListener()
{
    AudioListenerPose listener;
    listener.position[0] = 0.0f;
    listener.position[1] = 0.0f;
    listener.position[2] = 0.0f;
    listener.forward[0] = 0.0f;
    listener.forward[1] = 0.0f;
    listener.forward[2] = -1.0f;
    listener.up[0] = 0.0f;
    listener.up[1] = 1.0f;
    listener.up[2] = 0.0f;
    return listener;
}

AudioSourceComponent OneShot(float gain = 1.0f, uint8_t priority = 128)
{
    AudioSourceComponent component;
    component.bus = AudioBus::Effects;
    component.autoplay = false;
    component.loop = false;
    component.spatial = false;
    component.gain = gain;
    component.pitch = 1.0f;
    component.minDistance = 1.0f;
    component.maxDistance = 30.0f;
    component.rolloff = 1.0f;
    component.priority = priority;
    return component;
}

AudioSourceComponent SpatialOneShot(float gain = 1.0f, uint8_t priority = 128)
{
    AudioSourceComponent component = OneShot(gain, priority);
    component.spatial = true;
    return component;
}

AudioPlayRequest PlayReq(const UUID& source, const AudioSourceComponent& component,
                         const std::string& clipKey)
{
    AudioPlayRequest request;
    request.source = source;
    request.component = component;
    request.sourcePosition[0] = 0.0f;
    request.sourcePosition[1] = 0.0f;
    request.sourcePosition[2] = 0.0f;
    request.hasTransform = true;
    request.clipKey = clipKey;
    return request;
}

uint64_t QueuePlayChecked(AudioWorld& world, const AudioPlayRequest& request)
{
    uint64_t sequence = 0;
    REQUIRE(world.QueuePlay(request, sequence));
    return sequence;
}

AudioSourcePose PoseFor(const UUID& source, float x, float y, float z)
{
    AudioSourcePose pose;
    pose.source = source;
    pose.position[0] = x;
    pose.position[1] = y;
    pose.position[2] = z;
    pose.hasTransform = true;
    return pose;
}

BackendVoiceMix RequireVoiceMix(AudioWorld& world, const UUID& source)
{
    std::vector<AudioWorldVoiceHandle> voices = world.LiveVoicesForSource(source);
    REQUIRE(voices.size() == 1);
    BackendVoiceMix mix;
    REQUIRE(world.GetVoiceMix(voices[0], mix));
    return mix;
}

std::shared_ptr<const DecodedAudioGeneration> TestGeneration(uint32_t channels)
{
    auto generation = std::make_shared<DecodedAudioGeneration>();
    generation->channels = channels;
    generation->sampleRate = 48000;
    generation->frameCount = 8;
    generation->pcmInterleaved.assign(
        static_cast<size_t>(generation->frameCount) * channels, 0.25f);
    return generation;
}

} // namespace

TEST_CASE("A3_SpatialCenter_NonSpatialUsesEqualPowerCenter")
{
    // Mutation: replacing center gains with 1.0/1.0 (or baking bus gain)
    // turns this red. Non-spatial voices ignore distance and layout.
    RecordingFakeAudioBackend backend;
    AudioWorld world(&backend, &backend, TestSession(), AudioOwnerKind::Runtime);

    const UUID source = NextUuid();
    QueuePlayChecked(world, PlayReq(source, OneShot(1.0f), "center-a"));
    world.Update(TestListener(), nullptr, 0, 0);

    const BackendVoiceMix mix = RequireVoiceMix(world, source);
    CHECK(mix.left == doctest::Approx(kCenter).epsilon(1e-5));
    CHECK(mix.right == doctest::Approx(kCenter).epsilon(1e-5));
    CHECK(mix.pitch == doctest::Approx(1.0f));
    // Bus gains ride backend groups exactly once: they are not baked in.
    CHECK(backend.busGains.empty());
}

TEST_CASE("A3_SpatialHardLeftCenterRight_ExactGains")
{
    // Mutation: a linear pan law (1-p)/(1+p) in place of the equal-power
    // cos/sin law turns the center case red; negating the right axis flips
    // left and right. minDistance 10 keeps distance gain at unity.
    RecordingFakeAudioBackend backend;
    AudioWorld world(&backend, &backend, TestSession(), AudioOwnerKind::Runtime);

    AudioSourceComponent component = SpatialOneShot(1.0f);
    component.minDistance = 10.0f;
    component.maxDistance = 30.0f;

    const UUID leftSource = NextUuid();
    const UUID centerSource = NextUuid();
    const UUID rightSource = NextUuid();
    const UUID zeroSource = NextUuid();
    QueuePlayChecked(world, PlayReq(leftSource, component, "hard-l"));
    QueuePlayChecked(world, PlayReq(centerSource, component, "hard-c"));
    QueuePlayChecked(world, PlayReq(rightSource, component, "hard-r"));
    QueuePlayChecked(world, PlayReq(zeroSource, component, "hard-z"));

    const AudioSourcePose poses[] = {
        PoseFor(leftSource, -5.0f, 0.0f, 0.0f),
        PoseFor(centerSource, 0.0f, 0.0f, -5.0f),
        PoseFor(rightSource, 5.0f, 0.0f, 0.0f),
        PoseFor(zeroSource, 0.0f, 0.0f, 0.0f),
    };
    world.Update(TestListener(), poses, 4, 0);

    const BackendVoiceMix left = RequireVoiceMix(world, leftSource);
    CHECK(left.left == doctest::Approx(1.0f).epsilon(1e-5));
    CHECK(left.right == doctest::Approx(0.0f).epsilon(1e-5));

    const BackendVoiceMix center = RequireVoiceMix(world, centerSource);
    CHECK(center.left == doctest::Approx(kCenter).epsilon(1e-5));
    CHECK(center.right == doctest::Approx(kCenter).epsilon(1e-5));

    const BackendVoiceMix right = RequireVoiceMix(world, rightSource);
    CHECK(right.left == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(right.right == doctest::Approx(1.0f).epsilon(1e-5));

    // Endpoint clamp (A8 fixup P1): float cos(pi/2) rounds to about
    // -4.37e-8, which the production backend refuses as a negative gain.
    // The pan law clamps to [0, 1], so exact cardinal silence is exactly
    // zero. Reverting the clamp turns these red while the approximate
    // checks above stay green (the fake backend never validates gains).
    CHECK(right.left == 0.0f);
    CHECK(left.right == 0.0f);

    // Zero distance selects center rather than dividing by zero.
    const BackendVoiceMix zero = RequireVoiceMix(world, zeroSource);
    CHECK(zero.left == doctest::Approx(kCenter).epsilon(1e-5));
    CHECK(zero.right == doctest::Approx(kCenter).epsilon(1e-5));
}

TEST_CASE("A3_SpatialDistance_RolloffCutoffAndZeroRolloff")
{
    // Mutation: dropping the pow(1-t, rolloff) term (or the max-distance
    // cutoff) turns the mid/far cases red.
    RecordingFakeAudioBackend backend;
    AudioWorld world(&backend, &backend, TestSession(), AudioOwnerKind::Runtime);

    AudioSourceComponent component = SpatialOneShot(1.0f);
    component.minDistance = 1.0f;
    component.maxDistance = 11.0f;
    component.rolloff = 1.0f;

    const UUID nearSource = NextUuid();
    const UUID midSource = NextUuid();
    const UUID farSource = NextUuid();
    const UUID beyondSource = NextUuid();
    QueuePlayChecked(world, PlayReq(nearSource, component, "dist-near"));
    QueuePlayChecked(world, PlayReq(midSource, component, "dist-mid"));
    QueuePlayChecked(world, PlayReq(farSource, component, "dist-far"));
    QueuePlayChecked(world, PlayReq(beyondSource, component, "dist-beyond"));

    const AudioSourcePose poses[] = {
        PoseFor(nearSource, 0.0f, 0.0f, -1.0f),   // d = min -> unity
        PoseFor(midSource, 0.0f, 0.0f, -6.0f),    // t = 0.5 -> 0.5
        PoseFor(farSource, 0.0f, 0.0f, -11.0f),   // d = max -> silence
        PoseFor(beyondSource, 0.0f, 0.0f, -20.0f),
    };
    world.Update(TestListener(), poses, 4, 0);

    const BackendVoiceMix near = RequireVoiceMix(world, nearSource);
    CHECK(near.left == doctest::Approx(kCenter).epsilon(1e-5));

    const BackendVoiceMix mid = RequireVoiceMix(world, midSource);
    CHECK(mid.left == doctest::Approx(0.5f * kCenter).epsilon(1e-5));
    CHECK(mid.right == doctest::Approx(0.5f * kCenter).epsilon(1e-5));

    const BackendVoiceMix far = RequireVoiceMix(world, farSource);
    CHECK(far.left == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(far.right == doctest::Approx(0.0f).epsilon(1e-5));

    const BackendVoiceMix beyond = RequireVoiceMix(world, beyondSource);
    CHECK(beyond.left == doctest::Approx(0.0f).epsilon(1e-5));

    // Zero rolloff means unity until the explicit cutoff.
    AudioSourceComponent flat = SpatialOneShot(1.0f);
    flat.minDistance = 1.0f;
    flat.maxDistance = 11.0f;
    flat.rolloff = 0.0f;
    const UUID flatSource = NextUuid();
    QueuePlayChecked(world, PlayReq(flatSource, flat, "dist-flat"));
    const AudioSourcePose flatPose = PoseFor(flatSource, 0.0f, 0.0f, -6.0f);
    world.Update(TestListener(), &flatPose, 1, 0);
    const BackendVoiceMix flatMix = RequireVoiceMix(world, flatSource);
    CHECK(flatMix.left == doctest::Approx(kCenter).epsilon(1e-5));
}

TEST_CASE("A3_SpatialRefusals_StereoMissingTransformAndBadPoses")
{
    // Mutation: accepting stereo spatial clips (implicit downmix) turns the
    // first two checks red; skipping the finite/degenerate guards turns the
    // Update cases red (NaN mix published instead of last-valid retained).
    // Channel ownership lives in the provider generation: the request
    // carries no channel claim, so scripting a stereo generation is the
    // only way to feed stereo bytes, and the drain refuses them loudly.
    RecordingFakeAudioBackend backend;
    AudioWorld world(&backend, &backend, TestSession(), AudioOwnerKind::Runtime);
    backend.ScriptGeneration("spatial-stereo", TestGeneration(2));

    const UUID stereoSource = NextUuid();
    AudioPlayRequest stereo = PlayReq(stereoSource, SpatialOneShot(), "spatial-stereo");
    uint64_t sequence = 0;
    // Admission cannot know the decoded layout: true means queued, and the
    // typed refusal surfaces as the sequence-scoped result at drain.
    REQUIRE(world.QueuePlay(stereo, sequence));
    CHECK(sequence == 1);
    world.Update(TestListener(), nullptr, 0, 0);
    CHECK(world.LiveVoicesForSource(stereoSource).empty());
    CHECK(backend.starts.empty());
    AudioSourceStatus stereoStatus = world.GetSourceStatus(stereoSource);
    CHECK(stereoStatus.hasResult);
    CHECK_FALSE(stereoStatus.lastResultOk);
    CHECK(stereoStatus.lastResultSequence == 1);
    CHECK(stereoStatus.lastError.code == Error::InvalidArgument);
    CHECK(stereoStatus.aggregate == AudioSourceAggregate::Failed);

    const UUID noTransformSource = NextUuid();
    AudioPlayRequest noTransform = PlayReq(noTransformSource, SpatialOneShot(), "spatial-notransform");
    noTransform.hasTransform = false;
    CHECK_FALSE(world.QueuePlay(noTransform, sequence));

    const UUID goodSource = NextUuid();
    QueuePlayChecked(world, PlayReq(goodSource, SpatialOneShot(), "spatial-good"));
    const AudioSourcePose goodPose = PoseFor(goodSource, 0.0f, 0.0f, -5.0f);
    world.Update(TestListener(), &goodPose, 1, 0);
    const BackendVoiceMix baseline = RequireVoiceMix(world, goodSource);

    // Non-finite source position: typed update failure, last valid retained.
    AudioSourcePose nanPose = PoseFor(goodSource,
        std::numeric_limits<float>::quiet_NaN(), 0.0f, -5.0f);
    AudioUpdateStats stats = world.Update(TestListener(), &nanPose, 1, 0);
    CHECK(stats.mixFailures == 1);
    CHECK(world.LiveVoiceCount() == 1);
    const BackendVoiceMix retained = RequireVoiceMix(world, goodSource);
    CHECK(retained.left == doctest::Approx(baseline.left).epsilon(1e-6));
    CHECK(retained.right == doctest::Approx(baseline.right).epsilon(1e-6));
    CHECK_FALSE(world.Diagnostics().empty());

    // Degenerate listener basis: same retain-and-diagnose contract.
    AudioListenerPose badListener = TestListener();
    badListener.forward[0] = 0.0f;
    badListener.forward[1] = 0.0f;
    badListener.forward[2] = 0.0f;
    stats = world.Update(badListener, &goodPose, 1, 0);
    CHECK(stats.mixFailures == 1);
    CHECK(world.LiveVoiceCount() == 1);
}

TEST_CASE("A3_QueueOverflow_RefusesWithoutMutation")
{
    // Mutation: silently growing past 256 (or dropping an old command to
    // make room) turns the refusal and census checks red. Check 8/11.
    RecordingFakeAudioBackend backend;
    AudioWorld world(&backend, &backend, TestSession(), AudioOwnerKind::Runtime);

    for (uint32_t i = 0; i < kAudioCommandQueueCapacity; ++i)
    {
        uint64_t sequence = 0;
        REQUIRE(world.QueueStop(NextUuid(), sequence));
    }
    CHECK(world.QueuedCommandCount() == kAudioCommandQueueCapacity);

    uint64_t refused = 0;
    CHECK_FALSE(world.QueueStop(NextUuid(), refused));
    CHECK(refused == 0);
    CHECK(world.QueuedCommandCount() == kAudioCommandQueueCapacity);
    CHECK(world.QueueOverflowCount() == 1);
    CHECK(world.LiveVoiceCount() == 0);

    world.Update(TestListener(), nullptr, 0, 0);
    CHECK(world.QueuedCommandCount() == 0);
}

TEST_CASE("A3_DrainFifo_OrderAndReentrantNextFrame")
{
    // Mutation: executing the live queue without freezing (LIFO, or
    // same-frame re-entrant execution) turns the order and frame checks red.
    RecordingFakeAudioBackend backend;
    AudioWorld world(&backend, &backend, TestSession(), AudioOwnerKind::Runtime);

    const UUID first = NextUuid();
    const UUID second = NextUuid();
    const UUID third = NextUuid();
    QueuePlayChecked(world, PlayReq(first, OneShot(0.5f), "fifo-a"));
    QueuePlayChecked(world, PlayReq(second, OneShot(0.6f), "fifo-b"));
    QueuePlayChecked(world, PlayReq(third, OneShot(0.7f), "fifo-c"));

    // Re-entrant submission during the drain waits for the next frame.
    const UUID reentrant = NextUuid();
    bool hookFired = false;
    backend.onStartVoice = [&](BackendVoiceToken) {
        if (!hookFired)
        {
            hookFired = true;
            uint64_t sequence = 0;
            CHECK(world.QueuePlay(PlayReq(reentrant, OneShot(), "fifo-d"), sequence));
        }
    };
    world.Update(TestListener(), nullptr, 0, 0);
    backend.onStartVoice = nullptr;

    REQUIRE(backend.starts.size() == 3);
    CHECK(backend.starts[0].start.initialLeft ==
          doctest::Approx(0.5f * kCenter).epsilon(1e-5));
    CHECK(backend.starts[1].start.initialLeft ==
          doctest::Approx(0.6f * kCenter).epsilon(1e-5));
    CHECK(backend.starts[2].start.initialLeft ==
          doctest::Approx(0.7f * kCenter).epsilon(1e-5));
    // The re-entrant command did not execute in the frame that accepted it.
    CHECK(world.LiveVoicesForSource(reentrant).empty());
    CHECK(world.QueuedCommandCount() == 1);

    world.Update(TestListener(), nullptr, 0, 0);
    CHECK(world.LiveVoicesForSource(reentrant).size() == 1);
    CHECK(world.QueuedCommandCount() == 0);
}

TEST_CASE("A3_Steal_LowestPriorityOldestSlotVictim")
{
    // Mutation: stealing newest-first, highest-slot-first, or ignoring
    // priority turns the victim assertions red. Check 8.
    AudioWorldConfig config;
    config.maxVoices = 2;
    RecordingFakeAudioBackend backend;
    AudioWorld world(&backend, &backend, TestSession(), AudioOwnerKind::Runtime, config);

    const UUID low = NextUuid();
    const UUID mid = NextUuid();
    QueuePlayChecked(world, PlayReq(low, OneShot(1.0f, 10), "steal-low"));
    QueuePlayChecked(world, PlayReq(mid, OneShot(1.0f, 50), "steal-mid"));
    world.Update(TestListener(), nullptr, 0, 0);
    REQUIRE(world.LiveVoiceCount() == 2);
    const uint32_t lowSlot = world.LiveVoicesForSource(low)[0].slot;

    const UUID incoming = NextUuid();
    QueuePlayChecked(world, PlayReq(incoming, OneShot(1.0f, 200), "steal-high"));
    AudioUpdateStats stats = world.Update(TestListener(), nullptr, 0, 0);

    CHECK(stats.voicesStolen == 1);
    CHECK(world.StealCount() == 1);
    CHECK(world.LiveVoicesForSource(low).empty());
    CHECK(world.LiveVoicesForSource(mid).size() == 1);
    CHECK(world.LiveVoicesForSource(incoming).size() == 1);

    // Equal priorities steal the oldest start, then the lowest world slot.
    AudioWorldConfig pair;
    pair.maxVoices = 2;
    RecordingFakeAudioBackend backend2;
    AudioWorld world2(&backend2, &backend2, TestSession(), AudioOwnerKind::Runtime, pair);
    const UUID a = NextUuid();
    const UUID b = NextUuid();
    QueuePlayChecked(world2, PlayReq(a, OneShot(1.0f, 50), "tie-a"));
    QueuePlayChecked(world2, PlayReq(b, OneShot(1.0f, 50), "tie-b"));
    world2.Update(TestListener(), nullptr, 0, 0);
    const uint32_t slotA = world2.LiveVoicesForSource(a)[0].slot;
    const UUID c = NextUuid();
    QueuePlayChecked(world2, PlayReq(c, OneShot(1.0f, 50), "tie-c"));
    world2.Update(TestListener(), nullptr, 0, 0);
    CHECK(world2.LiveVoicesForSource(a).empty());
    CHECK(world2.LiveVoicesForSource(b).size() == 1);
    REQUIRE(world2.LiveVoicesForSource(c).size() == 1);
    CHECK(world2.LiveVoicesForSource(c)[0].slot == slotA);
}

TEST_CASE("A3_Steal_ProtectedLoopsRefuseUnlessStrictlyGreater")
{
    // Mutation: allowing equal-priority loop preemption (<= instead of <)
    // turns the refusal red; dropping protection entirely loses the loop.
    AudioWorldConfig config;
    config.maxVoices = 1;
    RecordingFakeAudioBackend backend;
    AudioWorld world(&backend, &backend, TestSession(), AudioOwnerKind::Runtime, config);

    AudioSourceComponent loop = OneShot(1.0f, 10);
    loop.loop = true;
    const UUID loopSource = NextUuid();
    QueuePlayChecked(world, PlayReq(loopSource, loop, "prot-loop"));
    world.Update(TestListener(), nullptr, 0, 0);
    REQUIRE(world.LiveVoiceCount() == 1);

    const UUID equalSource = NextUuid();
    QueuePlayChecked(world, PlayReq(equalSource, OneShot(1.0f, 10), "prot-eq"));
    world.Update(TestListener(), nullptr, 0, 0);
    CHECK(world.LiveVoicesForSource(loopSource).size() == 1);
    CHECK(world.LiveVoicesForSource(equalSource).empty());
    AudioSourceStatus refused = world.GetSourceStatus(equalSource);
    CHECK(refused.hasResult);
    CHECK_FALSE(refused.lastResultOk);
    CHECK(refused.aggregate == AudioSourceAggregate::Failed);
    CHECK(world.StealCount() == 0);

    const UUID greaterSource = NextUuid();
    QueuePlayChecked(world, PlayReq(greaterSource, OneShot(1.0f, 11), "prot-gt"));
    world.Update(TestListener(), nullptr, 0, 0);
    CHECK(world.LiveVoicesForSource(loopSource).empty());
    CHECK(world.LiveVoicesForSource(greaterSource).size() == 1);
    CHECK(world.StealCount() == 1);
}

TEST_CASE("A3_StaleHandle_CannotControlRecycledSlot")
{
    // Mutation: matching slots by index without the generation check lets
    // the stale stop kill the recycled voice. Check 7.
    RecordingFakeAudioBackend backend;
    AudioWorld world(&backend, &backend, TestSession(), AudioOwnerKind::Runtime);

    const UUID source = NextUuid();
    QueuePlayChecked(world, PlayReq(source, OneShot(), "stale-a"));
    world.Update(TestListener(), nullptr, 0, 0);
    REQUIRE(world.LiveVoicesForSource(source).size() == 1);
    const AudioWorldVoiceHandle first = world.LiveVoicesForSource(source)[0];

    Error error;
    REQUIRE(world.TryStopVoice(first, error));

    QueuePlayChecked(world, PlayReq(source, OneShot(), "stale-b"));
    world.Update(TestListener(), nullptr, 0, 0);
    REQUIRE(world.LiveVoicesForSource(source).size() == 1);
    const AudioWorldVoiceHandle second = world.LiveVoicesForSource(source)[0];
    CHECK(second.slot == first.slot);
    CHECK(second.generation != first.generation);

    CHECK_FALSE(world.TryStopVoice(first, error));
    CHECK(error.code == Error::InvalidArgument);
    BackendVoiceMix mix{ 0.1f, 0.2f, 1.0f };
    CHECK_FALSE(world.TrySetVoiceMix(first, mix, error));
    CHECK(world.LiveVoicesForSource(source).size() == 1);
    CHECK(world.LiveVoicesForSource(source)[0] == second);
}

TEST_CASE("A3_StaleCompletion_CannotReclaimReusedGeneration")
{
    // Mutation: reclaiming by backend token without matching the slot's
    // current generation frees the wrong (reused) voice. Check 7/8.
    RecordingFakeAudioBackend backend;
    AudioWorld world(&backend, &backend, TestSession(), AudioOwnerKind::Runtime);

    const UUID source = NextUuid();
    QueuePlayChecked(world, PlayReq(source, OneShot(), "stalecomp-a"));
    world.Update(TestListener(), nullptr, 0, 0);
    REQUIRE(backend.starts.size() == 1);
    const BackendVoiceToken tokenA = backend.starts[0].token;

    uint64_t stopSequence = 0;
    REQUIRE(world.QueueStop(source, stopSequence));
    world.Update(TestListener(), nullptr, 0, 0);
    REQUIRE(world.LiveVoiceCount() == 0);

    QueuePlayChecked(world, PlayReq(source, OneShot(), "stalecomp-b"));
    world.Update(TestListener(), nullptr, 0, 0);
    REQUIRE(world.LiveVoiceCount() == 1);

    backend.EnqueueCompletion(TestSession(), tokenA, BackendCompletionReason::Completed);
    AudioUpdateStats stats = world.Update(TestListener(), nullptr, 0, 0);
    CHECK(stats.staleCompletions == 1);
    CHECK(world.StaleCompletionCount() == 1);
    CHECK(world.LiveVoiceCount() == 1);
}

TEST_CASE("A3_LoopPlay_IsIdempotentPerSource")
{
    // Mutation: allocating a second slot for the duplicate loop turns the
    // census red; dropping the refresh loses the mix update.
    RecordingFakeAudioBackend backend;
    AudioWorld world(&backend, &backend, TestSession(), AudioOwnerKind::Runtime);

    AudioSourceComponent loop = OneShot(0.5f, 10);
    loop.loop = true;
    const UUID source = NextUuid();
    QueuePlayChecked(world, PlayReq(source, loop, "idem-loop"));
    QueuePlayChecked(world, PlayReq(source, loop, "idem-loop"));
    world.Update(TestListener(), nullptr, 0, 0);

    CHECK(world.LiveVoiceCount() == 1);
    CHECK(backend.starts.size() == 1);
    // The refresh carries its play sequence onto the slot, so a later
    // terminal failure for this voice replaces the refreshed success.
    std::vector<AudioWorldVoiceHandle> idemVoices = world.LiveVoicesForSource(source);
    REQUIRE(idemVoices.size() == 1);
    uint64_t storedSequence = 0;
    REQUIRE(world.GetVoicePlaySequence(idemVoices[0], storedSequence));
    CHECK(storedSequence == 2);
    AudioSourceStatus status = world.GetSourceStatus(source);
    CHECK(status.aggregate == AudioSourceAggregate::Playing);
    CHECK(status.liveVoiceCount == 1);
    CHECK(status.hasResult);
    CHECK(status.lastResultOk);
    CHECK(status.lastResultSequence == 2);
}

TEST_CASE("A3_Stop_StopsEveryVoiceOwnedBySource")
{
    // Mutation: stopping only the newest voice leaves two live and turns
    // the census red; forgetting the terminal reset leaves Completed.
    RecordingFakeAudioBackend backend;
    AudioWorld world(&backend, &backend, TestSession(), AudioOwnerKind::Runtime);

    const UUID source = NextUuid();
    QueuePlayChecked(world, PlayReq(source, OneShot(), "stop-a"));
    QueuePlayChecked(world, PlayReq(source, OneShot(), "stop-b"));
    QueuePlayChecked(world, PlayReq(source, OneShot(), "stop-c"));
    world.Update(TestListener(), nullptr, 0, 0);
    REQUIRE(world.LiveVoiceCount() == 3);

    uint64_t stopSequence = 0;
    REQUIRE(world.QueueStop(source, stopSequence));
    world.Update(TestListener(), nullptr, 0, 0);

    CHECK(world.LiveVoiceCount() == 0);
    CHECK(backend.stops.size() == 3);
    AudioSourceStatus status = world.GetSourceStatus(source);
    CHECK(status.aggregate == AudioSourceAggregate::Idle);
    CHECK(status.lastResultSequence == stopSequence);
    CHECK(status.lastResultOk);
}

TEST_CASE("A3_Destroying_RefusesDropsAndStopsBeforeRemoval")
{
    // Mutation: accepting commands for destroying UUIDs, keeping queued
    // ones, or leaving live voices turns the respective checks red.
    RecordingFakeAudioBackend backend;
    AudioWorld world(&backend, &backend, TestSession(), AudioOwnerKind::Runtime);

    const UUID source = NextUuid();
    QueuePlayChecked(world, PlayReq(source, OneShot(), "destroy-a"));
    world.Update(TestListener(), nullptr, 0, 0);
    REQUIRE(world.LiveVoiceCount() == 1);

    uint64_t queued = 0;
    REQUIRE(world.QueuePlay(PlayReq(source, OneShot(), "destroy-b"), queued));
    REQUIRE(world.QueuedCommandCount() == 1);

    world.NotifySourcesDestroying(&source, 1);
    CHECK(world.QueuedCommandCount() == 0);
    CHECK(world.LiveVoiceCount() == 0);
    CHECK(backend.stops.size() == 1);

    uint64_t refused = 0;
    CHECK_FALSE(world.QueuePlay(PlayReq(source, OneShot(), "destroy-c"), refused));
    CHECK_FALSE(world.QueueStop(source, refused));
    world.Update(TestListener(), nullptr, 0, 0);
    CHECK(world.LiveVoiceCount() == 0);

    world.ClearDestroying();
    QueuePlayChecked(world, PlayReq(source, OneShot(), "destroy-d"));
    world.Update(TestListener(), nullptr, 0, 0);
    CHECK(world.LiveVoiceCount() == 1);
}

TEST_CASE("A3_SessionCensus_StopAndShutdownReturnToBaseline")
{
    // Mutation: leaking a slot, a queued command, or a clip generation
    // across Stop/Shutdown turns the baseline checks red. Check 6.
    RecordingFakeAudioBackend backend;
    AudioWorld world(&backend, &backend, TestSession(), AudioOwnerKind::Runtime);

    const UUID a = NextUuid();
    const UUID b = NextUuid();
    QueuePlayChecked(world, PlayReq(a, OneShot(), "census-a"));
    QueuePlayChecked(world, PlayReq(b, OneShot(), "census-b"));
    uint64_t queued = 0;
    REQUIRE(world.QueueStop(NextUuid(), queued));
    world.Update(TestListener(), nullptr, 0, 0);
    REQUIRE(world.LiveVoiceCount() == 2);

    Error error;
    REQUIRE(world.StopAllVoices(error));
    CHECK(error.IsOk());
    CHECK(world.LiveVoiceCount() == 0);
    CHECK(world.QueuedCommandCount() == 0);
    REQUIRE(backend.stopSessions.size() == 1);
    CHECK(backend.stopSessions[0] == TestSession());
    CHECK(backend.LiveTokenCount() == 0);

    QueuePlayChecked(world, PlayReq(a, OneShot(), "census-a"));
    world.Update(TestListener(), nullptr, 0, 0);
    REQUIRE(world.LiveVoiceCount() == 1);
    REQUIRE(world.Shutdown(error));
    CHECK(error.IsOk());
    CHECK(world.LiveVoiceCount() == 0);
    CHECK(world.QueuedCommandCount() == 0);
    CHECK(backend.LiveTokenCount() == 0);
    CHECK(backend.RetainedGenerationCount() == 0);
    // Release failures are loud but still return state to baseline.
    CHECK(backend.releases.size() == 2);
}

TEST_CASE("A3_PauseStep_SampleTimeFrozenAndInitiallyPaused")
{
    // Mutation: advancing cursors while paused, during Step, or for an
    // initially-paused start turns the cursor checks red. Check 12 (A3
    // share: pause/Step/initialPaused never advance sample time).
    RecordingFakeAudioBackend backend;
    AudioWorld world(&backend, &backend, TestSession(), AudioOwnerKind::Runtime);

    const UUID source = NextUuid();
    QueuePlayChecked(world, PlayReq(source, OneShot(), "pause-a"));
    world.Update(TestListener(), nullptr, 0, 100);
    REQUIRE(world.LiveVoicesForSource(source).size() == 1);
    const AudioWorldVoiceHandle voice = world.LiveVoicesForSource(source)[0];
    double cursor = 0.0;
    REQUIRE(world.GetVoiceCursor(voice, cursor));
    CHECK(cursor == doctest::Approx(100.0));

    Error error;
    REQUIRE(world.SetSessionPaused(true, error));
    REQUIRE(backend.sessionPauses.size() == 1);
    world.Update(TestListener(), nullptr, 0, 100);
    REQUIRE(world.GetVoiceCursor(voice, cursor));
    CHECK(cursor == doctest::Approx(100.0));

    // A voice started while paused carries initialPaused and stays frozen.
    const UUID pausedSource = NextUuid();
    QueuePlayChecked(world, PlayReq(pausedSource, OneShot(), "pause-b"));
    world.Update(TestListener(), nullptr, 0, 50);
    REQUIRE(world.LiveVoicesForSource(pausedSource).size() == 1);
    const AudioWorldVoiceHandle pausedVoice = world.LiveVoicesForSource(pausedSource)[0];
    REQUIRE(world.GetVoiceCursor(pausedVoice, cursor));
    CHECK(cursor == doctest::Approx(0.0));
    REQUIRE(backend.starts.size() == 2);
    CHECK(backend.starts[1].start.initialPaused);

    // Step processes state (pose-driven mix moves) without PCM progress.
    const AudioSourcePose moved = PoseFor(source, 50.0f, 0.0f, 0.0f);
    AudioSourceComponent spatial = SpatialOneShot();
    spatial.minDistance = 10.0f;
    const UUID stepSource = NextUuid();
    QueuePlayChecked(world, PlayReq(stepSource, spatial, "pause-step"));
    const AudioSourcePose stepPose = PoseFor(stepSource, -5.0f, 0.0f, 0.0f);
    world.Step(TestListener(), &stepPose, 1);
    const BackendVoiceMix stepped = RequireVoiceMix(world, stepSource);
    CHECK(stepped.left == doctest::Approx(1.0f).epsilon(1e-5));
    (void)moved;
    REQUIRE(world.GetVoiceCursor(voice, cursor));
    CHECK(cursor == doctest::Approx(100.0));

    REQUIRE(world.SetSessionPaused(false, error));
    world.Update(TestListener(), nullptr, 0, 50);
    REQUIRE(world.GetVoiceCursor(voice, cursor));
    CHECK(cursor == doctest::Approx(150.0));

    // Voice-level pause freezes that voice only.
    uint64_t pauseSequence = 0;
    REQUIRE(world.QueuePause(source, true, pauseSequence));
    world.Update(TestListener(), nullptr, 0, 50);
    REQUIRE(world.GetVoiceCursor(voice, cursor));
    CHECK(cursor == doctest::Approx(150.0));
    REQUIRE(world.GetVoiceCursor(pausedVoice, cursor));
    CHECK(cursor == doctest::Approx(100.0));
    AudioSourceStatus status = world.GetSourceStatus(source);
    CHECK(status.aggregate == AudioSourceAggregate::Paused);
}

TEST_CASE("A3_OverlapAB_CompletionOrFailureKeepsBLive")
{
    // Mutation: letting A's completion overwrite B's sequence-scoped result
    // (or terminalize the source while B is audible) turns the status
    // checks red. Check 20, completion-first half.
    RecordingFakeAudioBackend backend;
    AudioWorld world(&backend, &backend, TestSession(), AudioOwnerKind::Runtime);

    const UUID source = NextUuid();
    const uint64_t seqA = QueuePlayChecked(world, PlayReq(source, OneShot(), "overlap-a"));
    const uint64_t seqB = QueuePlayChecked(world, PlayReq(source, OneShot(), "overlap-b"));
    world.Update(TestListener(), nullptr, 0, 0);
    REQUIRE(world.LiveVoiceCount() == 2);
    REQUIRE(backend.starts.size() == 2);
    CHECK(seqA == 1);
    CHECK(seqB == 2);

    backend.CompleteToken(backend.starts[0].token, BackendCompletionReason::Completed);
    world.Update(TestListener(), nullptr, 0, 0);

    AudioSourceStatus status = world.GetSourceStatus(source);
    CHECK(status.liveVoiceCount == 1);
    CHECK(status.aggregate == AudioSourceAggregate::Playing);
    CHECK(status.lastResultSequence == seqB);
    CHECK(status.lastResultOk);

    backend.CompleteToken(backend.starts[1].token, BackendCompletionReason::Completed);
    world.Update(TestListener(), nullptr, 0, 0);
    status = world.GetSourceStatus(source);
    CHECK(status.liveVoiceCount == 0);
    CHECK(status.aggregate == AudioSourceAggregate::Completed);
    CHECK(status.lastResultSequence == seqB);
}

TEST_CASE("A3_OverlapAB_FailureOfADoesNotReplaceBResult")
{
    // Check 20, failure-first half: B's failure stays visible while A
    // remains audible, and only B's completion terminalizes.
    RecordingFakeAudioBackend backend;
    AudioWorld world(&backend, &backend, TestSession(), AudioOwnerKind::Runtime);

    const UUID source = NextUuid();
    QueuePlayChecked(world, PlayReq(source, OneShot(), "overlapfail-a"));
    const uint64_t seqB = QueuePlayChecked(world, PlayReq(source, OneShot(), "overlapfail-b"));
    world.Update(TestListener(), nullptr, 0, 0);
    REQUIRE(world.LiveVoiceCount() == 2);

    Error backendError;
    backendError.code = Error::Io;
    backendError.path = "audio device";
    backendError.detail = "injected B failure";
    backend.CompleteToken(backend.starts[1].token, BackendCompletionReason::Failed, backendError);
    world.Update(TestListener(), nullptr, 0, 0);

    AudioSourceStatus status = world.GetSourceStatus(source);
    CHECK(status.liveVoiceCount == 1);
    CHECK(status.aggregate == AudioSourceAggregate::Playing);
    // A's earlier success cannot resurface, and B's failure is published
    // as the sequence-scoped result without terminalizing the still-
    // audible source. Mutation: leaving lastResult at B's successful start
    // turns the next three checks red (finding 1).
    CHECK(status.lastResultSequence == seqB);
    CHECK_FALSE(status.lastResultOk);
    CHECK(status.lastError.code == Error::Io);
    CHECK_FALSE(world.Diagnostics().empty());

    backend.CompleteToken(backend.starts[0].token, BackendCompletionReason::Completed);
    world.Update(TestListener(), nullptr, 0, 0);
    status = world.GetSourceStatus(source);
    CHECK(status.aggregate == AudioSourceAggregate::Failed);
}

TEST_CASE("A3_PartialBackendFailures_LeaveNoLeakedSlot")
{
    // Mutation: consuming a slot on a failed Start, reclaiming on a failed
    // Stop, or dropping voices on a failed completion drain turns the
    // census checks red.
    RecordingFakeAudioBackend backend;
    AudioWorld world(&backend, &backend, TestSession(), AudioOwnerKind::Runtime);

    Error startError;
    startError.code = Error::Io;
    startError.path = "audio device";
    startError.detail = "injected start failure";
    backend.FailNextStart(startError);

    const UUID source = NextUuid();
    QueuePlayChecked(world, PlayReq(source, OneShot(), "partial-a"));
    world.Update(TestListener(), nullptr, 0, 0);
    CHECK(world.LiveVoiceCount() == 0);
    AudioSourceStatus status = world.GetSourceStatus(source);
    CHECK(status.hasResult);
    CHECK_FALSE(status.lastResultOk);
    CHECK(status.lastError.code == Error::Io);
    CHECK(status.aggregate == AudioSourceAggregate::Failed);

    // No slot leaked: the retry lands and behaves normally.
    QueuePlayChecked(world, PlayReq(source, OneShot(), "partial-a"));
    world.Update(TestListener(), nullptr, 0, 0);
    CHECK(world.LiveVoiceCount() == 1);

    // A failing Stop still reclaims the slot (teardown is loud but final).
    Error stopError;
    stopError.code = Error::Io;
    stopError.path = "audio device";
    stopError.detail = "injected stop failure";
    REQUIRE(backend.starts.size() == 1);
    backend.FailStopForToken(backend.starts[0].token, stopError);
    uint64_t stopSequence = 0;
    REQUIRE(world.QueueStop(source, stopSequence));
    world.Update(TestListener(), nullptr, 0, 0);
    CHECK(world.LiveVoiceCount() == 0);
    CHECK_FALSE(world.Diagnostics().empty());
    // The typed backend failure surfaces as the Stop result (finding 4)
    // while the census still returns to baseline.
    AudioSourceStatus stopStatus = world.GetSourceStatus(source);
    CHECK(stopStatus.hasResult);
    CHECK_FALSE(stopStatus.lastResultOk);
    CHECK(stopStatus.lastResultSequence == stopSequence);
    CHECK(stopStatus.lastError.code == Error::Io);

    // A failing completion drain changes nothing.
    QueuePlayChecked(world, PlayReq(source, OneShot(), "partial-a"));
    world.Update(TestListener(), nullptr, 0, 0);
    REQUIRE(world.LiveVoiceCount() == 1);
    Error drainError;
    drainError.code = Error::Io;
    drainError.path = "audio device";
    drainError.detail = "injected drain failure";
    backend.FailNextDrain(drainError);
    world.Update(TestListener(), nullptr, 0, 0);
    CHECK(world.LiveVoiceCount() == 1);
    CHECK_FALSE(world.Diagnostics().empty());
}

TEST_CASE("A3_BusAndSessionErrors_AreTypedAndNonMutating")
{
    // Mutation: storing a bus gain the backend refused (or accepting NaN)
    // turns the gain checks red; claiming pause after a refused atomic
    // turns the session check red.
    RecordingFakeAudioBackend backend;
    AudioWorld world(&backend, &backend, TestSession(), AudioOwnerKind::Runtime);

    Error error;
    CHECK_FALSE(world.SetBusGain(AudioBus::Music,
        std::numeric_limits<float>::quiet_NaN(), error));
    CHECK(error.code == Error::InvalidArgument);
    CHECK(backend.busGains.empty());

    Error busError;
    busError.code = Error::Io;
    busError.path = "audio mixer";
    busError.detail = "injected bus failure";
    backend.FailNextBusGain(busError);
    CHECK_FALSE(world.SetBusGain(AudioBus::Music, 0.5f, error));
    CHECK(error.code == Error::Io);
    CHECK(world.BusGain(AudioBus::Music) == doctest::Approx(1.0f));

    REQUIRE(world.SetBusGain(AudioBus::Master, 0.8f, error));
    CHECK(world.BusGain(AudioBus::Master) == doctest::Approx(0.8f));

    backend.FailNextSessionPause(busError);
    CHECK_FALSE(world.SetSessionPaused(true, error));
    CHECK_FALSE(world.IsSessionPaused());
}

TEST_CASE("A3_VoiceIdentity_StoresSequenceGenerationAndToken")
{
    // Mutation: recording the wrong play sequence on a slot (or sharing one
    // backend token across two slots) turns the identity checks red.
    RecordingFakeAudioBackend backend;
    AudioWorld world(&backend, &backend, TestSession(), AudioOwnerKind::Runtime);

    const UUID source = NextUuid();
    const uint64_t seqA = QueuePlayChecked(world, PlayReq(source, OneShot(), "ident-a"));
    const uint64_t seqB = QueuePlayChecked(world, PlayReq(source, OneShot(), "ident-b"));
    world.Update(TestListener(), nullptr, 0, 0);

    std::vector<AudioWorldVoiceHandle> voices = world.LiveVoicesForSource(source);
    REQUIRE(voices.size() == 2);
    uint64_t storedA = 0;
    uint64_t storedB = 0;
    REQUIRE(world.GetVoicePlaySequence(voices[0], storedA));
    REQUIRE(world.GetVoicePlaySequence(voices[1], storedB));
    CHECK(((storedA == seqA && storedB == seqB) || (storedA == seqB && storedB == seqA)));

    BackendVoiceToken tokenA;
    BackendVoiceToken tokenB;
    REQUIRE(world.GetVoiceBackendToken(voices[0], tokenA));
    REQUIRE(world.GetVoiceBackendToken(voices[1], tokenB));
    CHECK(tokenA.IsValid());
    CHECK(tokenB.IsValid());
    CHECK(tokenA != tokenB);
    CHECK(voices[0].generation == voices[1].generation);
}

TEST_CASE("A3_CapDefaults64AndQueueCapacity256")
{
    // Mutation: changing either bound without updating the suite turns
    // these pins red. Check 8 (command 257 refuses).
    RecordingFakeAudioBackend backend;
    AudioWorld world(&backend, &backend, TestSession(), AudioOwnerKind::Runtime);
    CHECK(world.SessionVoiceCap() == 64);
    CHECK(kAudioCommandQueueCapacity == 256);
    CHECK(kAudioDefaultVoiceCap == 64);

    AudioWorldConfig small;
    small.maxVoices = 3;
    RecordingFakeAudioBackend backend2;
    AudioWorld world2(&backend2, &backend2, TestSession(), AudioOwnerKind::Runtime, small);
    CHECK(world2.SessionVoiceCap() == 3);
}

TEST_CASE("A3_BusPolicy_MasterSourceAndUiSpatialRefused")
{
    // Mutation: accepting Master as a source bus (or spatial UI) turns
    // these refusals red; both rules come from A2 validation via the READY
    // plan and must hold at the queue boundary too.
    RecordingFakeAudioBackend backend;
    AudioWorld world(&backend, &backend, TestSession(), AudioOwnerKind::Runtime);

    AudioSourceComponent master = OneShot();
    master.bus = AudioBus::Master;
    uint64_t sequence = 0;
    CHECK_FALSE(world.QueuePlay(PlayReq(NextUuid(), master, "bus-master"), sequence));

    AudioSourceComponent uiSpatial = OneShot();
    uiSpatial.bus = AudioBus::UI;
    uiSpatial.spatial = true;
    CHECK_FALSE(world.QueuePlay(PlayReq(NextUuid(), uiSpatial, "bus-ui"), sequence));

    AudioSourceComponent uiFlat = OneShot();
    uiFlat.bus = AudioBus::UI;
    uiFlat.spatial = false;
    QueuePlayChecked(world, PlayReq(NextUuid(), uiFlat, "bus-ui-flat"));
    world.Update(TestListener(), nullptr, 0, 0);
    CHECK(world.LiveVoiceCount() == 1);
}

TEST_CASE("A3_StealFailedStart_KeepsVictimAndCountsNothing")
{
    // Mutation: stopping the victim before the replacement starts (or
    // counting the steal first) ends with zero voices and one recorded
    // steal when StartVoice fails. Finding 2.
    AudioWorldConfig config;
    config.maxVoices = 1;
    RecordingFakeAudioBackend backend;
    AudioWorld world(&backend, &backend, TestSession(), AudioOwnerKind::Runtime, config);

    AudioSourceComponent loop = OneShot(1.0f, 10);
    loop.loop = true;
    const UUID victim = NextUuid();
    QueuePlayChecked(world, PlayReq(victim, loop, "atomic-victim"));
    world.Update(TestListener(), nullptr, 0, 0);
    REQUIRE(world.LiveVoiceCount() == 1);

    Error startError;
    startError.code = Error::Io;
    startError.path = "audio device";
    startError.detail = "injected incoming start failure";
    backend.FailNextReplace(startError);

    const UUID incoming = NextUuid();
    QueuePlayChecked(world, PlayReq(incoming, OneShot(1.0f, 11), "atomic-incoming"));
    world.Update(TestListener(), nullptr, 0, 0);

    // The victim is untouched, the incoming voice is absent, and no steal
    // was counted: prepare-then-commit is atomic.
    CHECK(world.LiveVoicesForSource(victim).size() == 1);
    CHECK(world.LiveVoicesForSource(incoming).empty());
    CHECK(world.StealCount() == 0);
    CHECK(backend.stops.empty());
    AudioSourceStatus incomingStatus = world.GetSourceStatus(incoming);
    CHECK(incomingStatus.hasResult);
    CHECK_FALSE(incomingStatus.lastResultOk);
    CHECK(incomingStatus.lastError.code == Error::Io);
    CHECK(incomingStatus.aggregate == AudioSourceAggregate::Failed);
    AudioSourceStatus victimStatus = world.GetSourceStatus(victim);
    CHECK(victimStatus.aggregate == AudioSourceAggregate::Playing);
    CHECK(victimStatus.lastResultOk);
}

TEST_CASE("A3_StealHardCap_ReplaceCommitsWithinCapacity")
{
    // Mutation: starting the replacement as an extra voice (StartVoice
    // while the victim is live) always fails against the hard backend cap,
    // so the legal steal never lands. Finding R1.
    AudioWorldConfig config;
    config.maxVoices = 1;
    RecordingFakeAudioBackend backend;
    backend.SetMaxLiveVoices(1);
    AudioWorld world(&backend, &backend, TestSession(), AudioOwnerKind::Runtime, config);

    AudioSourceComponent loop = OneShot(1.0f, 10);
    loop.loop = true;
    const UUID victim = NextUuid();
    QueuePlayChecked(world, PlayReq(victim, loop, "hardcap-victim"));
    world.Update(TestListener(), nullptr, 0, 0);
    REQUIRE(world.LiveVoiceCount() == 1);

    const UUID incoming = NextUuid();
    QueuePlayChecked(world, PlayReq(incoming, OneShot(1.0f, 11), "hardcap-incoming"));
    world.Update(TestListener(), nullptr, 0, 0);

    // The atomic swap committed: exactly one voice, and the backend never
    // observed two live voices at once.
    CHECK(world.LiveVoicesForSource(victim).empty());
    REQUIRE(world.LiveVoicesForSource(incoming).size() == 1);
    CHECK(world.StealCount() == 1);
    CHECK(backend.LiveTokenCount() == 1);
    CHECK(backend.PeakLiveTokens() == 1);
    AudioSourceStatus incomingStatus = world.GetSourceStatus(incoming);
    CHECK(incomingStatus.aggregate == AudioSourceAggregate::Playing);
    CHECK(incomingStatus.lastResultOk);

    // A refused replacement preserves the new victim the same way a
    // refused start does.
    const UUID third = NextUuid();
    Error replaceError;
    replaceError.code = Error::Io;
    replaceError.path = "audio device";
    replaceError.detail = "injected replace failure";
    backend.FailNextReplace(replaceError);
    QueuePlayChecked(world, PlayReq(third, OneShot(1.0f, 12), "hardcap-third"));
    world.Update(TestListener(), nullptr, 0, 0);
    CHECK(world.LiveVoicesForSource(incoming).size() == 1);
    CHECK(world.LiveVoicesForSource(third).empty());
    CHECK(world.StealCount() == 1);
    CHECK(backend.PeakLiveTokens() == 1);
}

TEST_CASE("A3_SameKeyGenerationReplace_RegistersNewHandle")
{
    // Mutation: reusing the cached backend handle for a re-fetched
    // generation pairs the old stereo clip with the new mono metadata, so
    // the second start reuses the first handle. Finding R2.
    RecordingFakeAudioBackend backend;
    AudioWorld world(&backend, &backend, TestSession(), AudioOwnerKind::Runtime);
    backend.ScriptGeneration("shared-key", TestGeneration(2));

    const UUID flatSource = NextUuid();
    QueuePlayChecked(world, PlayReq(flatSource, OneShot(), "shared-key"));
    world.Update(TestListener(), nullptr, 0, 0);
    REQUIRE(world.LiveVoiceCount() == 1);
    REQUIRE(backend.starts.size() == 1);

    // The same key now resolves a mono generation (old voices keep the old
    // stereo generation alive independently).
    auto mono = TestGeneration(1);
    backend.ScriptGeneration("shared-key", mono);
    const UUID spatialSource = NextUuid();
    QueuePlayChecked(world, PlayReq(spatialSource, SpatialOneShot(), "shared-key"));
    const AudioSourcePose pose = PoseFor(spatialSource, 0.0f, 0.0f, -5.0f);
    world.Update(TestListener(), &pose, 1, 0);

    REQUIRE(world.LiveVoiceCount() == 2);
    REQUIRE(backend.starts.size() == 2);
    CHECK(backend.starts[1].clip != backend.starts[0].clip);
    // The handle used for the spatial start is registered for the exact
    // mono generation the validation observed.
    std::shared_ptr<const DecodedAudioGeneration> registered =
        backend.RegisteredGeneration(backend.starts[1].clip);
    REQUIRE(registered.get() != nullptr);
    CHECK(registered.get() == mono.get());
    CHECK(registered->channels == 1);
    const BackendVoiceMix spatialMix = RequireVoiceMix(world, spatialSource);
    // Exact attenuation oracle: distance 5 with min 1 / max 30 /
    // rolloff 1 gives (1 - 4/29) = 25/29.
    CHECK(spatialMix.left == doctest::Approx((25.0f / 29.0f) * kCenter).epsilon(1e-5));
    CHECK(spatialMix.right == doctest::Approx((25.0f / 29.0f) * kCenter).epsilon(1e-5));
}

TEST_CASE("A3_ReentrantDestroyDuringStart_DiscardsVoice")
{
    // Mutation: mapping the just-started token without rechecking the
    // destruction mark leaves a live backend voice on a destroyed source.
    // Finding R3, destruction half.
    RecordingFakeAudioBackend backend;
    AudioWorld world(&backend, &backend, TestSession(), AudioOwnerKind::Runtime);

    const UUID doomed = NextUuid();
    backend.onStartVoice = [&](BackendVoiceToken) {
        world.NotifySourcesDestroying(&doomed, 1);
    };
    QueuePlayChecked(world, PlayReq(doomed, OneShot(), "doomed-a"));
    world.Update(TestListener(), nullptr, 0, 0);
    backend.onStartVoice = nullptr;

    CHECK(world.LiveVoiceCount() == 0);
    CHECK(world.LiveVoicesForSource(doomed).empty());
    CHECK(backend.LiveTokenCount() == 0);
    AudioSourceStatus status = world.GetSourceStatus(doomed);
    CHECK_FALSE(status.lastResultOk);
    CHECK(status.lastResultSequence == 1);
    CHECK(world.QueuedCommandCount() == 0);
}

TEST_CASE("A3_ReentrantStopDuringStart_DiscardsVoice")
{
    // Finding R3, session-stop half: a session Stop issued from the start
    // callback must leave zero session voices and no mapped slot.
    RecordingFakeAudioBackend backend;
    AudioWorld world(&backend, &backend, TestSession(), AudioOwnerKind::Runtime);

    const UUID source = NextUuid();
    backend.onStartVoice = [&](BackendVoiceToken) {
        Error error;
        REQUIRE(world.StopAllVoices(error));
    };
    QueuePlayChecked(world, PlayReq(source, OneShot(), "stopped-a"));
    world.Update(TestListener(), nullptr, 0, 0);
    backend.onStartVoice = nullptr;

    CHECK(world.LiveVoiceCount() == 0);
    CHECK(world.LiveVoicesForSource(source).empty());
    CHECK(backend.LiveTokenCount() == 0);
    CHECK(world.QueuedCommandCount() == 0);
}

TEST_CASE("A3_ReentrantDestroyDuringReplace_ReconcilesVictim")
{
    // Mutation: discarding the incoming token before freeing the committed
    // victim slot leaves the dead victim mapped as Playing while the
    // backend holds zero tokens. Finding R4: the replacement callback
    // fires after the commit, and the victim slot must agree with the
    // backend on every return path.
    AudioWorldConfig config;
    config.maxVoices = 1;
    RecordingFakeAudioBackend backend;
    backend.SetMaxLiveVoices(1);
    AudioWorld world(&backend, &backend, TestSession(), AudioOwnerKind::Runtime, config);

    AudioSourceComponent loop = OneShot(1.0f, 10);
    loop.loop = true;
    const UUID victim = NextUuid();
    QueuePlayChecked(world, PlayReq(victim, loop, "replace-doom-victim"));
    world.Update(TestListener(), nullptr, 0, 0);
    REQUIRE(world.LiveVoiceCount() == 1);

    const UUID incoming = NextUuid();
    backend.onStartVoice = [&](BackendVoiceToken) {
        world.NotifySourcesDestroying(&incoming, 1);
    };
    QueuePlayChecked(world, PlayReq(incoming, OneShot(1.0f, 11), "replace-doom-incoming"));
    world.Update(TestListener(), nullptr, 0, 0);
    backend.onStartVoice = nullptr;

    // Backend and world censuses agree at zero: the victim is freed
    // world-side and the incoming token was discarded, never mapped.
    CHECK(backend.LiveTokenCount() == 0);
    CHECK(world.LiveVoiceCount() == 0);
    CHECK(world.LiveVoicesForSource(victim).empty());
    CHECK(world.LiveVoicesForSource(incoming).empty());
    // The committed swap still counts, and the peak never exceeded the
    // hard cap; the victim is stole-Completed, never false-Playing.
    CHECK(world.StealCount() == 1);
    CHECK(backend.PeakLiveTokens() == 1);
    CHECK(backend.stops.size() == 2);
    AudioSourceStatus victimStatus = world.GetSourceStatus(victim);
    CHECK(victimStatus.liveVoiceCount == 0);
    CHECK(victimStatus.aggregate == AudioSourceAggregate::Completed);
    AudioSourceStatus incomingStatus = world.GetSourceStatus(incoming);
    CHECK(incomingStatus.lastResultSequence == 1);
    CHECK_FALSE(incomingStatus.lastResultOk);
    CHECK(incomingStatus.aggregate == AudioSourceAggregate::Failed);
}

TEST_CASE("A3_ReentrantStopDuringReplace_LeavesZeroCensus")
{
    // Finding R4, session-stop half: a session Stop issued from the
    // replacement callback must leave zero backend and world voices even
    // though the victim swap already committed.
    AudioWorldConfig config;
    config.maxVoices = 1;
    RecordingFakeAudioBackend backend;
    backend.SetMaxLiveVoices(1);
    AudioWorld world(&backend, &backend, TestSession(), AudioOwnerKind::Runtime, config);

    AudioSourceComponent loop = OneShot(1.0f, 10);
    loop.loop = true;
    const UUID victim = NextUuid();
    QueuePlayChecked(world, PlayReq(victim, loop, "replace-stop-victim"));
    world.Update(TestListener(), nullptr, 0, 0);
    REQUIRE(world.LiveVoiceCount() == 1);

    const UUID incoming = NextUuid();
    backend.onStartVoice = [&](BackendVoiceToken) {
        Error error;
        REQUIRE(world.StopAllVoices(error));
    };
    QueuePlayChecked(world, PlayReq(incoming, OneShot(1.0f, 11), "replace-stop-incoming"));
    world.Update(TestListener(), nullptr, 0, 0);
    backend.onStartVoice = nullptr;

    CHECK(backend.LiveTokenCount() == 0);
    CHECK(world.LiveVoiceCount() == 0);
    CHECK(world.LiveVoicesForSource(victim).empty());
    CHECK(world.LiveVoicesForSource(incoming).empty());
    CHECK(world.QueuedCommandCount() == 0);
    CHECK(backend.PeakLiveTokens() == 1);
}

TEST_CASE("A3_ReentrantFirstPlay_CreatesSecondSourceSafely")
{
    // Mutation: retaining a SourceState& across the StartVoice callback
    // (vector storage) dangles when the callback registers a second source,
    // corrupting the first voice's result, terminal, or mix. Finding 3.
    RecordingFakeAudioBackend backend;
    AudioWorld world(&backend, &backend, TestSession(), AudioOwnerKind::Runtime);

    const UUID first = NextUuid();
    const UUID second = NextUuid();
    backend.onStartVoice = [&](BackendVoiceToken) {
        uint64_t sequence = 0;
        REQUIRE(world.QueuePlay(PlayReq(second, OneShot(0.9f), "reentrant-second"), sequence));
        CHECK(sequence == 1);
    };
    // Only the first source exists when the callback fires, so any spare
    // state capacity cannot hide the reallocation.
    QueuePlayChecked(world, PlayReq(first, OneShot(0.5f), "reentrant-first"));
    world.Update(TestListener(), nullptr, 0, 0);
    backend.onStartVoice = nullptr;

    REQUIRE(world.LiveVoicesForSource(first).size() == 1);
    const BackendVoiceMix firstMix = RequireVoiceMix(world, first);
    CHECK(firstMix.left == doctest::Approx(0.5f * kCenter).epsilon(1e-5));
    uint64_t firstSequence = 0;
    REQUIRE(world.GetVoicePlaySequence(world.LiveVoicesForSource(first)[0], firstSequence));
    CHECK(firstSequence == 1);
    AudioSourceStatus firstStatus = world.GetSourceStatus(first);
    CHECK(firstStatus.lastResultSequence == 1);
    CHECK(firstStatus.lastResultOk);
    CHECK(world.LiveVoicesForSource(second).empty());
    CHECK(world.QueuedCommandCount() == 1);

    world.Update(TestListener(), nullptr, 0, 0);
    REQUIRE(world.LiveVoicesForSource(second).size() == 1);
    const BackendVoiceMix secondMix = RequireVoiceMix(world, second);
    CHECK(secondMix.left == doctest::Approx(0.9f * kCenter).epsilon(1e-5));
    CHECK(world.LiveVoicesForSource(first).size() == 1);
}

TEST_CASE("A3_StopFailure_DirectApiPropagatesTypedError")
{
    // Mutation: clearing outError after a failed StopVoice (or refusing to
    // detach) turns the error and census checks red. Finding 4.
    RecordingFakeAudioBackend backend;
    AudioWorld world(&backend, &backend, TestSession(), AudioOwnerKind::Runtime);

    const UUID source = NextUuid();
    QueuePlayChecked(world, PlayReq(source, OneShot(), "stopdirect-a"));
    world.Update(TestListener(), nullptr, 0, 0);
    REQUIRE(world.LiveVoicesForSource(source).size() == 1);
    const AudioWorldVoiceHandle voice = world.LiveVoicesForSource(source)[0];
    BackendVoiceToken token;
    REQUIRE(world.GetVoiceBackendToken(voice, token));

    Error stopError;
    stopError.code = Error::Io;
    stopError.path = "audio device";
    stopError.detail = "injected direct stop failure";
    backend.FailStopForToken(token, stopError);

    Error error;
    CHECK_FALSE(world.TryStopVoice(voice, error));
    CHECK(error.code == Error::Io);
    // Detachment is final even though the result is loud.
    CHECK(world.LiveVoiceCount() == 0);
    CHECK_FALSE(backend.IsTokenLive(token));
}

TEST_CASE("A3_ProviderFailure_RefusesPlayLoudly")
{
    // Mutation: bypassing the provider (or swallowing its error) turns the
    // fetch, start-absence, and typed-result checks red. Finding 5.
    RecordingFakeAudioBackend backend;
    AudioWorld world(&backend, &backend, TestSession(), AudioOwnerKind::Runtime);

    Error fetchError;
    fetchError.code = Error::Io;
    fetchError.path = "clip store";
    fetchError.detail = "injected fetch failure";
    backend.ScriptGenerationError("missing-clip", fetchError);

    const UUID source = NextUuid();
    QueuePlayChecked(world, PlayReq(source, OneShot(), "missing-clip"));
    world.Update(TestListener(), nullptr, 0, 0);

    REQUIRE(backend.generationFetches.size() == 1);
    CHECK(backend.generationFetches[0] == "missing-clip");
    CHECK(backend.starts.empty());
    CHECK(world.LiveVoiceCount() == 0);
    AudioSourceStatus status = world.GetSourceStatus(source);
    CHECK(status.hasResult);
    CHECK_FALSE(status.lastResultOk);
    CHECK(status.lastResultSequence == 1);
    CHECK(status.lastError.code == Error::Io);
    CHECK(status.aggregate == AudioSourceAggregate::Failed);
}

TEST_CASE("A3_LoopRefreshFailure_ReplacesRefreshedResult")
{
    // Mutation: keeping the original play sequence on an idempotent loop
    // refresh lets the later failure fall below the refreshed terminal and
    // the source wrongly reads Completed. Finding 1, loop half.
    RecordingFakeAudioBackend backend;
    AudioWorld world(&backend, &backend, TestSession(), AudioOwnerKind::Runtime);

    AudioSourceComponent loop = OneShot(1.0f, 10);
    loop.loop = true;
    const UUID source = NextUuid();
    QueuePlayChecked(world, PlayReq(source, loop, "loopfail-a"));
    world.Update(TestListener(), nullptr, 0, 0);
    QueuePlayChecked(world, PlayReq(source, loop, "loopfail-a"));
    world.Update(TestListener(), nullptr, 0, 0);
    REQUIRE(world.LiveVoiceCount() == 1);
    REQUIRE(backend.starts.size() == 1);

    Error backendError;
    backendError.code = Error::Io;
    backendError.path = "audio device";
    backendError.detail = "injected loop failure";
    backend.CompleteToken(backend.starts[0].token, BackendCompletionReason::Failed, backendError);
    world.Update(TestListener(), nullptr, 0, 0);

    AudioSourceStatus status = world.GetSourceStatus(source);
    CHECK(status.liveVoiceCount == 0);
    CHECK(status.aggregate == AudioSourceAggregate::Failed);
    CHECK(status.lastResultSequence == 2);
    CHECK_FALSE(status.lastResultOk);
    CHECK(status.lastError.code == Error::Io);
}

TEST_CASE("A3_SpatialOverflow_FiniteIntermediatesFailLoudly")
{
    // Mutation: accepting infinite squared distances or cross products
    // publishes a silent cutoff/NaN mix instead of failing. Finding 6.
    // Each coordinate below individually passes the 1e20 position guard.
    RecordingFakeAudioBackend backend;
    AudioWorld world(&backend, &backend, TestSession(), AudioOwnerKind::Runtime);

    const UUID source = NextUuid();
    QueuePlayChecked(world, PlayReq(source, SpatialOneShot(), "overflow-good"));
    const AudioSourcePose goodPose = PoseFor(source, 0.0f, 0.0f, -5.0f);
    world.Update(TestListener(), &goodPose, 1, 0);
    const BackendVoiceMix baseline = RequireVoiceMix(world, source);

    const AudioSourcePose bigPose = PoseFor(source, 9.0e19f, 9.0e19f, 9.0e19f);
    AudioUpdateStats stats = world.Update(TestListener(), &bigPose, 1, 0);
    CHECK(stats.mixFailures == 1);
    CHECK(world.LiveVoiceCount() == 1);
    const BackendVoiceMix retained = RequireVoiceMix(world, source);
    CHECK(retained.left == doctest::Approx(baseline.left).epsilon(1e-6));
    CHECK(retained.right == doctest::Approx(baseline.right).epsilon(1e-6));

    // A large finite listener basis overflows the right-axis cross product.
    AudioListenerPose bigListener = TestListener();
    bigListener.forward[0] = 1.0e19f;
    bigListener.forward[1] = 1.0e19f;
    bigListener.forward[2] = 0.0f;
    bigListener.up[0] = 1.0e19f;
    bigListener.up[1] = -1.0e19f;
    bigListener.up[2] = 1.0e19f;
    stats = world.Update(bigListener, &goodPose, 1, 0);
    CHECK(stats.mixFailures == 1);
    CHECK(world.LiveVoiceCount() == 1);
    const BackendVoiceMix retainedListener = RequireVoiceMix(world, source);
    CHECK(std::isfinite(retainedListener.left));
    CHECK(std::isfinite(retainedListener.right));
    CHECK_FALSE(world.Diagnostics().empty());
}

TEST_CASE("A3_ShutdownReleaseFailure_RetainsHandleForRetry")
{
    // Mutation: clearing the cache on release failure loses the retry
    // handle and leaks the backend generation. Finding 7.
    RecordingFakeAudioBackend backend;
    AudioWorld world(&backend, &backend, TestSession(), AudioOwnerKind::Runtime);

    const UUID source = NextUuid();
    QueuePlayChecked(world, PlayReq(source, OneShot(), "retry-a"));
    world.Update(TestListener(), nullptr, 0, 0);
    REQUIRE(world.LiveVoiceCount() == 1);
    REQUIRE(world.CachedClipGenerationCount() == 1);

    Error releaseError;
    releaseError.code = Error::Io;
    releaseError.path = "audio device";
    releaseError.detail = "injected release failure";
    backend.FailNextRelease(releaseError);

    Error shutdownError;
    CHECK_FALSE(world.Shutdown(shutdownError));
    CHECK(shutdownError.code == Error::Io);
    CHECK(world.LiveVoiceCount() == 0);
    CHECK(world.CachedClipGenerationCount() == 1);
    CHECK(backend.RetainedGenerationCount() == 1);

    REQUIRE(world.Shutdown(shutdownError));
    CHECK(shutdownError.IsOk());
    CHECK(world.CachedClipGenerationCount() == 0);
    CHECK(backend.RetainedGenerationCount() == 0);
}
