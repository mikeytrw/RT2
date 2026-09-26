#include "AudioPreviewController.h"
#include "AudioSpatialMath.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace rt2::audio {
namespace {

// Preview session IDs come from their own counter (never zero). Preview and
// runtime sessions never share an ID, so a backend completion can never be
// attributed to the wrong owner.
std::atomic<uint64_t> s_PreviewSessionCounter{ 1 };

constexpr uint32_t kPreviewNoDeviceSampleRate = 48000;
constexpr uint32_t kPreviewNoDeviceMaxChunk = 4096;
constexpr float kPreviewMaxFrameTime = 0.1f;

} // namespace

AudioPreviewController::AudioPreviewController() = default;

AudioPreviewController::~AudioPreviewController()
{
    DetachVoiceQuiet();
}

void AudioPreviewController::ClearBindings()
{
    m_Backend = nullptr;
    m_Provider = nullptr;
    m_KeyBuilder = nullptr;
}

bool AudioPreviewController::StartPreview(
    const core::UUID& source, const std::string& sourceName,
    const AudioSourceComponent& component, bool hasTransform,
    const float sourcePosition[3], const AudioListenerPose& listener,
    bool spatialAudition, core::Error& outError)
{
    outError = core::Error{};
    // Replacement first: at most one preview voice is ever live, and a new
    // start never observes the old voice.
    DetachVoiceQuiet();

    auto fail = [&](core::Error::Code code, const std::string& path,
                    const std::string& detail) {
        outError.code = code;
        outError.path = path;
        outError.detail = detail;
        RecordFailure(outError, path);
        return false;
    };

    if (m_Backend == nullptr || m_Provider == nullptr || !m_KeyBuilder)
        return fail(core::Error::InvalidRuntimeState,
                    source.ToString() + ":audioSource.preview",
                    "Preview refused: the preview controller has no "
                    "backend/clip-provider seams bound");
    if (source.IsNull())
        return fail(core::Error::InvalidEntity,
                    ":audioSource.preview",
                    "Preview refused: no source entity is selected");

    std::string detail;
    std::string field;
    if (!ValidateAudioSourceComponent(component, hasTransform, std::nullopt,
                                      detail, &field))
    {
        return fail(core::Error::InvalidArgument,
                    source.ToString() + ":audioSource." + field,
                    "Preview refused: invalid audio source (" + field + ": " +
                    detail + ")");
    }
    if (component.clip.path.empty())
    {
        return fail(core::Error::MissingAsset,
                    source.ToString() + ":audioSource.clip",
                    "Preview refused: the audio source has no clip bound");
    }

    if (!m_SessionValid)
    {
        uint64_t id = s_PreviewSessionCounter.fetch_add(
            1, std::memory_order_relaxed);
        if (id == 0)
            id = s_PreviewSessionCounter.fetch_add(
                1, std::memory_order_relaxed);
        m_Session.value = id;
        m_SessionValid = true;
    }

    core::Result<std::string> key = m_KeyBuilder(
        component.clip, source, sourceName);
    if (!key.IsOk())
    {
        return fail(key.error.code,
                    key.error.path.empty()
                        ? source.ToString() + ":audioSource.clip"
                        : key.error.path,
                    "Preview refused: audio clip failed to resolve (" +
                    key.error.detail + ")");
    }

    core::Result<std::shared_ptr<const DecodedAudioGeneration>> fetched =
        m_Provider->FetchDecodedGeneration(key.value);
    if (!fetched.IsOk())
    {
        return fail(fetched.error.code,
                    fetched.error.path.empty() ? key.value
                                               : fetched.error.path,
                    "Preview refused: audio clip '" + component.clip.path +
                    "' failed to resolve/decode: " + fetched.error.detail);
    }
    if (!fetched.value || fetched.value->channels == 0)
    {
        return fail(core::Error::InvalidArgument, key.value,
                    "Preview refused: audio clip '" + component.clip.path +
                    "' decoded to an empty generation");
    }
    if (component.spatial &&
        static_cast<int>(fetched.value->channels) != 1)
    {
        return fail(core::Error::InvalidArgument,
                    source.ToString() + ":audioSource.clip",
                    "Preview refused: spatial audio source decoded to stereo "
                    "(spatial sources must be mono)");
    }

    core::Result<BackendClipHandle> registered =
        m_Backend->RegisterDecodedGeneration(fetched.value);
    if (!registered.IsOk())
    {
        return fail(registered.error.code, key.value,
                    "Preview refused: backend would not register clip '" +
                    component.clip.path + "': " + registered.error.detail);
    }

    BackendVoiceMix mix = CenterMix(component);
    if (spatialAudition && component.spatial)
    {
        AudioSpatialInput in;
        in.component = component;
        in.sourcePosition[0] = sourcePosition[0];
        in.sourcePosition[1] = sourcePosition[1];
        in.sourcePosition[2] = sourcePosition[2];
        in.hasTransform = hasTransform;
        in.decodedChannels =
            static_cast<int>(fetched.value->channels);
        in.listener = listener;
        core::Result<BackendVoiceMix> spatial = ComputeSpatialMix(
            in, source.ToString() + ":audioSource.preview");
        if (!spatial.IsOk())
        {
            core::Error releaseError;
            (void)m_Backend->ReleaseDecodedGeneration(
                registered.value, releaseError);
            return fail(spatial.error.code, spatial.error.path,
                        "Preview refused: spatial audition failed (" +
                        spatial.error.detail + ")");
        }
        mix = spatial.value;
    }

    BackendVoiceStart start;
    start.session = m_Session;
    start.owner = AudioOwnerKind::Preview;
    start.loop = component.loop;
    start.initialPaused = false;
    start.bus = component.bus;
    start.initialLeft = mix.left;
    start.initialRight = mix.right;
    start.pitch = mix.pitch;
    core::Result<BackendVoiceToken> token = m_Backend->StartVoice(
        registered.value, start);
    if (!token.IsOk())
    {
        core::Error releaseError;
        (void)m_Backend->ReleaseDecodedGeneration(
            registered.value, releaseError);
        return fail(token.error.code,
                    source.ToString() + ":audioSource.preview",
                    "Preview refused: backend would not start the voice (" +
                    token.error.detail + ")");
    }

    m_Source = source;
    m_SourceName = sourceName;
    m_Component = component;
    m_HasTransform = hasTransform;
    m_SourcePosition[0] = sourcePosition[0];
    m_SourcePosition[1] = sourcePosition[1];
    m_SourcePosition[2] = sourcePosition[2];
    m_SpatialAudition = spatialAudition;
    m_VoiceValid = true;
    m_VoiceToken = token.value;
    m_ClipHandle = registered.value;
    m_Generation = fetched.value;
    m_ClipKey = key.value;
    m_LastMix = mix;
    m_LastError = core::Error{};
    m_LastAffectedAsset.clear();
    ++m_StartCount;
    return true;
}

bool AudioPreviewController::StopPreview(core::Error& outError)
{
    outError = core::Error{};
    if (!m_VoiceValid)
        return true;
    // Teardown detaches even on backend error (backend teardown contract),
    // so the census is correct while the failure stays loud.
    bool ok = true;
    if (m_Backend != nullptr)
    {
        core::Error stopError;
        if (!m_Backend->StopVoice(m_VoiceToken, stopError))
        {
            outError = stopError;
            ok = false;
        }
        core::Error releaseError;
        if (m_ClipHandle.IsValid() &&
            !m_Backend->ReleaseDecodedGeneration(m_ClipHandle, releaseError))
        {
            if (ok)
                outError = releaseError;
            ok = false;
        }
    }
    ClearVoiceState();
    return ok;
}

bool AudioPreviewController::Shutdown(core::Error& outError)
{
    const bool ok = StopPreview(outError);
    ClearBindings();
    return ok;
}

void AudioPreviewController::Update(const AudioListenerPose& listener,
                                    const float sourcePosition[3],
                                    bool hasTransform, bool spatialAudition)
{
    if (!m_VoiceValid || m_Backend == nullptr)
        return;
    m_SourcePosition[0] = sourcePosition[0];
    m_SourcePosition[1] = sourcePosition[1];
    m_SourcePosition[2] = sourcePosition[2];
    m_HasTransform = hasTransform;

    // Reap whatever the backend reports for our session. Only our token can
    // clear our voice: DrainCompletions carries opaque backend tokens and
    // the session ID keeps runtime completions out.
    core::Result<std::vector<BackendVoiceCompletion>> drained =
        m_Backend->DrainCompletions(m_Session);
    if (!drained.IsOk())
    {
        // A failed drain is not silent: the typed error reaches the status
        // UI (bounded — only when no failure is already sticky, so a
        // persistently failing backend cannot grow the failure count every
        // frame) and a diagnostic names it every frame it persists.
        m_LastDiagnostic = "preview completion drain failed (" +
                           drained.error.detail + ")";
        if (m_LastError.IsOk())
            RecordFailure(drained.error, m_ClipKey);
    }
    else
    {
        for (const BackendVoiceCompletion& completion : drained.value)
        {
            if (!(completion.token == m_VoiceToken))
                continue;
            if (completion.reason ==
                BackendCompletionReason::Failed)
            {
                RecordFailure(completion.error.code == core::Error::None
                                  ? core::Error{ core::Error::InvalidRuntimeState,
                                                 m_ClipKey,
                                                 "preview voice failed without detail" }
                                  : completion.error,
                              completion.error.path.empty()
                                  ? m_ClipKey
                                  : completion.error.path);
            }
            else
            {
                ++m_NaturalCompletions;
            }
            DetachVoiceQuiet();
            return;
        }
    }

    if (!m_VoiceValid)
        return;
    // A7 review P2: the inspector checkbox is live state, not a start-time
    // constant. A change republishes the mix for the new mode (spatial math
    // toward the source, or the center mix for 2D) and only then commits
    // the mode, so the status label and the sound switch together. A
    // compute/publish failure retains the last valid mix AND the old mode
    // with a diagnostic, and retries on the next frame.
    if (spatialAudition != m_SpatialAudition)
    {
        BackendVoiceMix target = CenterMix(m_Component);
        if (spatialAudition && m_Component.spatial)
        {
            AudioSpatialInput in;
            in.component = m_Component;
            in.sourcePosition[0] = m_SourcePosition[0];
            in.sourcePosition[1] = m_SourcePosition[1];
            in.sourcePosition[2] = m_SourcePosition[2];
            in.hasTransform = m_HasTransform;
            in.decodedChannels = m_Generation
                ? static_cast<int>(m_Generation->channels)
                : 1;
            in.listener = listener;
            core::Result<BackendVoiceMix> spatial = ComputeSpatialMix(
                in, m_Source.ToString() + ":audioSource.preview");
            if (!spatial.IsOk())
            {
                NoteMixFailure(spatial.error,
                               "preview audition mode retained");
                return;
            }
            target = spatial.value;
        }
        core::Error publishError;
        if (!m_Backend->SetVoiceMix(m_VoiceToken, target, publishError))
        {
            NoteMixFailure(publishError,
                           "preview audition mode publish failed");
            return;
        }
        m_SpatialAudition = spatialAudition;
        m_LastMix = target;
        return;
    }
    if (!(m_SpatialAudition && m_Component.spatial))
        return;
    AudioSpatialInput in;
    in.component = m_Component;
    in.sourcePosition[0] = m_SourcePosition[0];
    in.sourcePosition[1] = m_SourcePosition[1];
    in.sourcePosition[2] = m_SourcePosition[2];
    in.hasTransform = m_HasTransform;
    in.decodedChannels = m_Generation
        ? static_cast<int>(m_Generation->channels)
        : 1;
    in.listener = listener;
    core::Result<BackendVoiceMix> mix = ComputeSpatialMix(
        in, m_Source.ToString() + ":audioSource.preview");
    if (!mix.IsOk())
    {
        // Retain the last valid mix (spatial contract); the preview keeps
        // playing while the failure reaches the host status.
        NoteMixFailure(mix.error, "preview spatial mix retained");
        return;
    }
    core::Error mixError;
    if (!m_Backend->SetVoiceMix(m_VoiceToken, mix.value, mixError))
    {
        NoteMixFailure(mixError, "preview spatial mix publish failed");
        return;
    }
    m_LastMix = mix.value;
}

uint32_t AudioPreviewController::PumpNoDeviceFrames(float frameDt)
{
    if (!m_VoiceValid || m_Backend == nullptr)
        return 0;
    if (!m_Backend->Status().productionNoDevice)
        return 0;
    if (!(frameDt > 0.0f) || !std::isfinite(frameDt))
        return 0;
    const float dt = std::min(frameDt, kPreviewMaxFrameTime);
    const double exact = static_cast<double>(dt) *
                             kPreviewNoDeviceSampleRate +
                         m_FrameFrac;
    uint32_t frames = static_cast<uint32_t>(std::floor(exact));
    m_FrameFrac = exact - static_cast<double>(frames);
    if (frames == 0)
        return 0;
    if (m_Scratch.size() < static_cast<size_t>(kPreviewNoDeviceMaxChunk) * 2)
        m_Scratch.resize(static_cast<size_t>(kPreviewNoDeviceMaxChunk) * 2);
    uint32_t remaining = frames;
    uint32_t renderedTotal = 0;
    while (remaining > 0)
    {
        const uint32_t chunk = std::min(remaining, kPreviewNoDeviceMaxChunk);
        AudioPcmWriteBuffer sink;
        sink.data = m_Scratch.data();
        sink.sampleCapacity = m_Scratch.size();
        core::Result<uint32_t> rendered =
            m_Backend->RenderNoDeviceFrames(sink, chunk);
        if (!rendered.IsOk())
        {
            RecordFailure(rendered.error, m_ClipKey);
            break;
        }
        renderedTotal += rendered.value;
        if (rendered.value < chunk)
            break; // short read: successful prefix only
        remaining -= chunk;
    }
    return renderedTotal;
}

bool AudioPreviewController::LiveBackendToken(
    BackendVoiceToken& outToken) const
{
    if (!m_VoiceValid)
        return false;
    outToken = m_VoiceToken;
    return true;
}

void AudioPreviewController::RecordFailure(const core::Error& error,
                                           const std::string& asset)
{
    m_LastError = error;
    m_LastAffectedAsset = asset;
    ++m_FailureCount;
}

void AudioPreviewController::NoteMixFailure(const core::Error& error,
                                            const char* context)
{
    m_LastDiagnostic = std::string(context) + " (" + error.detail + ")";
    if (m_LastError.IsOk())
        RecordFailure(error, m_ClipKey);
}

void AudioPreviewController::ClearVoiceState()
{
    m_VoiceValid = false;
    m_VoiceToken = BackendVoiceToken{};
    m_ClipHandle = BackendClipHandle{};
    m_Generation.reset();
    m_ClipKey.clear();
}

void AudioPreviewController::DetachVoiceQuiet()
{
    if (!m_VoiceValid)
        return;
    if (m_Backend != nullptr)
    {
        core::Error ignored;
        (void)m_Backend->StopVoice(m_VoiceToken, ignored);
        if (m_ClipHandle.IsValid())
            (void)m_Backend->ReleaseDecodedGeneration(m_ClipHandle, ignored);
    }
    ClearVoiceState();
}

BackendVoiceMix AudioPreviewController::CenterMix(
    const AudioSourceComponent& component) const
{
    BackendVoiceMix mix;
    mix.left = component.gain * kAudioCenterPanGain;
    mix.right = component.gain * kAudioCenterPanGain;
    mix.pitch = component.pitch;
    return mix;
}

} // namespace rt2::audio
