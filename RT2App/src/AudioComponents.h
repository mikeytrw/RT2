#pragma once

#ifndef RT2_AUDIO_COMPONENTS_H
#define RT2_AUDIO_COMPONENTS_H

#include "AssetReference.h"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

// ============================================================================
// AudioComponents — authored audio-source data (audio integration, A2
// persistence foundation).
//
// CPU-only: AssetReference + standard library only. No miniaudio, Vulkan,
// ImGui, Walnut, or device types. Links into RT2Tests and RT2SliceRunner
// unchanged.
//
// These components are plain authored data (the ECSComponents.h POD rule):
// what the user authors in Edit and what the .rt2scene v9 codec persists.
// They carry NO decoder state, NO device/backend handles, and NO transient
// PCM. Decoding and playback arrive in A3/A4 and must reuse these definitions
// verbatim.
//
// A2 owns persistence only: component definition, exact equality, every
// manual codec (EntityRecord / scene JSON v9 / CloneInMemory /
// SubtreeEntityRecord / prefab records), v3-v8 migration (absent audio =
// no source), non-overridable prefab table entry, clip rebasing, and
// AssetReference visitation.
//
// Decoded-channel validation seam: a spatial clip's decoded mono channel
// count can only be known after A4's decoder runs. ValidateAudioSourceComponent
// therefore takes an optional decoded channel count: nullopt skips the check
// (persistence context), while a supplied count enforces mono for spatial
// sources. A4 must refuse decoded stereo spatial clips before Play or Preview
// commits. This stages the existing product rule without adding a decoder to
// CPU persistence targets.
// ============================================================================

enum class AudioBus : uint8_t
{
    Master  = 0,
    Music   = 1,
    Effects = 2,
    UI      = 3,
};

inline const char* AudioBusName(AudioBus bus)
{
    switch (bus)
    {
        case AudioBus::Master:  return "master";
        case AudioBus::Music:   return "music";
        case AudioBus::Effects: return "effects";
        case AudioBus::UI:      return "ui";
    }
    return "unknown";
}

inline bool AudioBusFromName(const std::string& name, AudioBus& out)
{
    if (name == "master")  { out = AudioBus::Master;  return true; }
    if (name == "music")   { out = AudioBus::Music;   return true; }
    if (name == "effects") { out = AudioBus::Effects; return true; }
    if (name == "ui")      { out = AudioBus::UI;      return true; }
    return false;
}

// Authored audio source. One per entity at most. Semantics (READY plan):
//   - clip references an AudioClip asset (WAV/FLAC/MP3). Empty path = unbound
//     source (no sound); validation of the clip applies only when bound.
//   - Master is a mixer parent and is rejected as a source bus.
//   - UI-bus sources are always non-spatial.
//   - Spatial sources require a Transform and decoded mono content.
//   - gain in [0, 4]; pitch in [0.25, 4]; minDistance > 0;
//     maxDistance >= minDistance; rolloff in [0, 8].
struct AudioSourceComponent
{
    AssetReference clip;              // kind == AudioClip when bound
    AudioBus bus = AudioBus::Effects;
    bool autoplay = false;
    bool loop = false;
    bool spatial = true;
    float gain = 1.0f;                // [0, 4]
    float pitch = 1.0f;               // [0.25, 4]
    float minDistance = 1.0f;         // > 0
    float maxDistance = 30.0f;        // >= minDistance
    float rolloff = 1.0f;             // [0, 8]
    uint8_t priority = 128;

    bool operator==(const AudioSourceComponent& o) const
    {
        return clip.kind == o.clip.kind && clip.path == o.clip.path &&
               clip.importSettings == o.clip.importSettings &&
               clip.sourceKey == o.clip.sourceKey &&
               clip.assetId == o.clip.assetId &&
               bus == o.bus && autoplay == o.autoplay && loop == o.loop &&
               spatial == o.spatial && gain == o.gain && pitch == o.pitch &&
               minDistance == o.minDistance && maxDistance == o.maxDistance &&
               rolloff == o.rolloff && priority == o.priority;
    }
    bool operator!=(const AudioSourceComponent& o) const { return !(*this == o); }
};

// Audio clip file extensions (lowercase, with leading dot). Case-insensitive
// matching: callers fold the candidate extension before comparing.
inline bool IsAudioClipExtensionLowered(const std::string& loweredExtension)
{
    return loweredExtension == ".wav" || loweredExtension == ".flac" ||
           loweredExtension == ".mp3";
}

// Fold an extension to lowercase for comparison (ASCII only; extensions are
// ASCII by construction).
inline std::string FoldAudioExtension(const std::string& extension)
{
    std::string out = extension;
    for (auto& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// True when the path's extension names an audio clip asset (WAV/FLAC/MP3,
// case-insensitive). Used by Content Browser classification and watch-policy
//-adjacent seams; the project scanner itself remains sidecar-driven.
inline bool IsAudioClipPath(const std::string& path)
{
    const std::string::size_type dot = path.find_last_of('.');
    const std::string::size_type sep = path.find_last_of("/\\");
    if (dot == std::string::npos ||
        (sep != std::string::npos && dot < sep))
        return false;
    return IsAudioClipExtensionLowered(FoldAudioExtension(path.substr(dot)));
}

// Validate authored audio-source fields without touching a registry, decoder,
// device, or renderer. Returns true when valid; otherwise false with `detail`
// describing the violation and `field` naming the dotted authored field
// (e.g. "clip.kind", "bus", "gain", "spatial").
//
// `hasTransform` reports whether the owning entity carries a Transform;
// spatial sources without one fail with field "transform".
//
// `decodedChannels` is the A4 validation seam: nullopt skips the decoded
// channel-count check (persistence/Save/Load context — the decoder has not
// run), while a supplied count enforces mono for spatial sources
// (A4 Play/Preview commit context). Non-spatial sources accept any channel
// count. This documents the deferred check rather than pretending persistence
// can execute it.
inline bool ValidateAudioSourceComponent(
    const AudioSourceComponent& source,
    bool hasTransform,
    const std::optional<int>& decodedChannels,
    std::string& detail,
    std::string* field = nullptr)
{
    detail.clear();
    if (field) field->clear();
    auto fail = [&](const char* f, const std::string& d) {
        if (field) *field = f;
        detail = d;
        return false;
    };

    if (!source.clip.path.empty())
    {
        if (source.clip.kind != AssetKind::AudioClip)
            return fail("clip.kind",
                        "audio clip reference must use the audioclip kind");
        if (!IsAudioClipPath(source.clip.path))
            return fail("clip.path",
                        "audio clip path must use a .wav, .flac, or .mp3 "
                        "extension: " + source.clip.path);
        if (source.clip.assetId.IsNull())
            return fail("clip.assetId",
                        "a bound audio clip reference requires a non-nil "
                        "asset identity");
    }

    // The bus is a closed set. An out-of-range value (for example a stale
    // cast or a corrupted payload) must not reach the codec: Save would
    // serialize AudioBusName's "unknown" fallback and write a scene Load
    // rejects. Reject it here with the same UUID+dotted-path contract as
    // every other authored violation.
    if (source.bus != AudioBus::Master && source.bus != AudioBus::Music &&
        source.bus != AudioBus::Effects && source.bus != AudioBus::UI)
        return fail("bus", "audio source bus must be master, music, effects, or ui");

    if (source.bus == AudioBus::Master)
        return fail("bus", "Master is a mixer parent and cannot be a source bus");

    if (source.bus == AudioBus::UI && source.spatial)
        return fail("spatial", "UI-bus sources are always non-spatial");

    if (source.spatial && !hasTransform)
        return fail("transform",
                    "spatial audio sources require a Transform");

    if (!std::isfinite(source.gain) || source.gain < 0.0f || source.gain > 4.0f)
        return fail("gain", "gain must be finite and in [0, 4]");
    if (!std::isfinite(source.pitch) || source.pitch < 0.25f ||
        source.pitch > 4.0f)
        return fail("pitch", "pitch must be finite and in [0.25, 4]");
    if (!std::isfinite(source.minDistance) || source.minDistance <= 0.0f)
        return fail("minDistance", "minDistance must be finite and > 0");
    if (!std::isfinite(source.maxDistance) ||
        source.maxDistance < source.minDistance)
        return fail("maxDistance",
                    "maxDistance must be finite and >= minDistance");
    if (!std::isfinite(source.rolloff) || source.rolloff < 0.0f ||
        source.rolloff > 8.0f)
        return fail("rolloff", "rolloff must be finite and in [0, 8]");

    // Deferred decoded-channel seam (see header note): only enforced when the
    // caller supplies the A4 decoder's channel count.
    if (decodedChannels.has_value() && source.spatial &&
        *decodedChannels != 1)
        return fail("clip",
                    "spatial audio clips must decode to mono");

    return true;
}

#endif // RT2_AUDIO_COMPONENTS_H
