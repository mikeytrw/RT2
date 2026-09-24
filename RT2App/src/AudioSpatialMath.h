#pragma once

#ifndef RT2_AUDIO_SPATIAL_MATH_H
#define RT2_AUDIO_SPATIAL_MATH_H

#include "AudioBackend.h"
#include "AudioComponents.h"

#include <cmath>
#include <string>

// ============================================================================
// AudioSpatialMath — deterministic RT2-owned attenuation and equal-power
// stereo pan (audio integration, A3 policy core).
//
// CPU-only, header-only: standard library only. No miniaudio, device,
// Vulkan, ImGui, or Walnut. Links into RT2Tests and RT2SliceRunner unchanged.
// No gameplay or backend code may duplicate this math (READY plan,
// Deterministic spatial math).
//
// Contract (READY plan):
//   1. d = distance from source to listener.
//   2. Gain is 1 at or inside minDistance, 0 at or beyond maxDistance.
//      Between: t = (d-min)/(max-min), distanceGain = pow(1-t, rolloff);
//      rolloff 0 means unity until the explicit max-distance cutoff.
//   3. right = normalized cross(forward, up).
//   4. p = clamped dot(right, normalized source direction).
//   5. angle = (p+1)*pi/4; left = cos(angle), right = sin(angle).
//   Final mix = source gain * distance attenuation per channel, plus pitch.
//   Child-bus and Master gains are applied exactly once by backend sound
//   groups and are never baked in here.
//   Music/UI and spatial=false use center gains with no attenuation.
//   Spatial sources must decode as mono; stereo spatial content is refused.
//   Zero distance selects center. A degenerate/non-finite listener basis or
//   non-finite/huge-overflowing source position is a typed failure: the
//   caller retains its last valid mix and records a diagnostic.
// ============================================================================

namespace rt2::audio {

inline constexpr float kAudioCenterPanGain = 0.70710678f; // cos(pi/4)
inline constexpr float kAudioPi = 3.14159265358979323846f;

// Upper bound for a sane scene position. Anything at or beyond this is
// treated as overflowing rather than spatialized (typed failure).
inline constexpr float kAudioMaxSanePosition = 1.0e20f;

struct AudioSpatialInput
{
    AudioSourceComponent component;
    // Final world-pose translation of the source (A5 feeds world-matrix
    // translation; A3 takes it as a direct input).
    float sourcePosition[3] = { 0.0f, 0.0f, 0.0f };
    bool hasTransform = true;
    // Decoded channel count of the clip generation (A4 seam): spatial
    // sources require mono. Non-spatial sources accept any count.
    int decodedChannels = 1;
    AudioListenerPose listener;
};

// Computes the explicit final L/R mix. Returns Ok(mix) or Fail with a typed
// error (path = source context supplied by the caller, e.g. entity UUID).
inline core::Result<BackendVoiceMix> ComputeSpatialMix(
    const AudioSpatialInput& in, const std::string& contextPath)
{
    auto fail = [&](core::Error::Code code, const std::string& detail) {
        return core::Result<BackendVoiceMix>::Fail(code, contextPath, detail);
    };

    // Authored ranges are enforced structurally here as well so a mix can
    // never be computed from values persistence would refuse.
    if (!std::isfinite(in.component.gain) || in.component.gain < 0.0f ||
        in.component.gain > 4.0f)
        return fail(core::Error::InvalidArgument, "gain must be finite and in [0, 4]");
    if (!std::isfinite(in.component.pitch) || in.component.pitch < 0.25f ||
        in.component.pitch > 4.0f)
        return fail(core::Error::InvalidArgument, "pitch must be finite and in [0.25, 4]");
    if (!std::isfinite(in.component.minDistance) || in.component.minDistance <= 0.0f)
        return fail(core::Error::InvalidArgument, "minDistance must be finite and > 0");
    if (!std::isfinite(in.component.maxDistance) ||
        in.component.maxDistance < in.component.minDistance)
        return fail(core::Error::InvalidArgument,
                    "maxDistance must be finite and >= minDistance");
    if (!std::isfinite(in.component.rolloff) || in.component.rolloff < 0.0f ||
        in.component.rolloff > 8.0f)
        return fail(core::Error::InvalidArgument, "rolloff must be finite and in [0, 8]");
    if (in.component.bus == AudioBus::Master)
        return fail(core::Error::InvalidArgument,
                    "Master is a mixer parent and cannot be a source bus");
    if (in.component.bus == AudioBus::UI && in.component.spatial)
        return fail(core::Error::InvalidArgument, "UI-bus sources are always non-spatial");

    // Non-spatial sources: center gains, no distance attenuation, any
    // channel layout.
    if (!in.component.spatial)
    {
        BackendVoiceMix mix;
        mix.left = in.component.gain * kAudioCenterPanGain;
        mix.right = in.component.gain * kAudioCenterPanGain;
        mix.pitch = in.component.pitch;
        return core::Result<BackendVoiceMix>::Ok(mix);
    }

    // Spatial path: mono input only, Transform required.
    if (in.decodedChannels != 1)
        return fail(core::Error::InvalidArgument, "spatial audio clips must decode to mono");
    if (!in.hasTransform)
        return fail(core::Error::InvalidArgument,
                    "spatial audio sources require a Transform");

    for (int i = 0; i < 3; ++i)
    {
        if (!std::isfinite(in.sourcePosition[i]) ||
            std::fabs(in.sourcePosition[i]) >= kAudioMaxSanePosition)
            return fail(core::Error::InvalidTransform,
                        "non-finite or overflowing source world position");
        if (!std::isfinite(in.listener.position[i]) ||
            !std::isfinite(in.listener.forward[i]) || !std::isfinite(in.listener.up[i]))
            return fail(core::Error::InvalidTransform,
                        "non-finite listener pose");
    }

    const float fx = in.listener.forward[0];
    const float fy = in.listener.forward[1];
    const float fz = in.listener.forward[2];
    const float fLen = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (!(fLen > 1e-6f))
        return fail(core::Error::InvalidTransform, "degenerate listener forward basis");

    const float ux = in.listener.up[0];
    const float uy = in.listener.up[1];
    const float uz = in.listener.up[2];
    // right = cross(forward, up), normalized.
    float rx = fy * uz - fz * uy;
    float ry = fz * ux - fx * uz;
    float rz = fx * uy - fy * ux;
    const float rLen = std::sqrt(rx * rx + ry * ry + rz * rz);
    if (!(rLen > 1e-6f))
        return fail(core::Error::InvalidTransform, "degenerate listener right basis");
    rx /= rLen;
    ry /= rLen;
    rz /= rLen;

    const float dx = in.sourcePosition[0] - in.listener.position[0];
    const float dy = in.sourcePosition[1] - in.listener.position[1];
    const float dz = in.sourcePosition[2] - in.listener.position[2];
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

    float distanceGain = 1.0f;
    if (dist <= in.component.minDistance)
    {
        distanceGain = 1.0f;
    }
    else if (dist >= in.component.maxDistance)
    {
        distanceGain = 0.0f;
    }
    else
    {
        const float t = (dist - in.component.minDistance) /
                        (in.component.maxDistance - in.component.minDistance);
        if (in.component.rolloff == 0.0f)
            distanceGain = 1.0f;
        else
            distanceGain = std::pow(1.0f - t, in.component.rolloff);
    }

    // Zero distance selects center; otherwise pan from the right-axis cosine.
    float p = 0.0f;
    if (dist > 1e-6f)
    {
        const float nx = dx / dist;
        const float ny = dy / dist;
        const float nz = dz / dist;
        p = rx * nx + ry * ny + rz * nz;
        if (p < -1.0f) p = -1.0f;
        if (p > 1.0f) p = 1.0f;
    }

    const float angle = (p + 1.0f) * (kAudioPi / 4.0f);
    BackendVoiceMix mix;
    mix.left = in.component.gain * distanceGain * std::cos(angle);
    mix.right = in.component.gain * distanceGain * std::sin(angle);
    mix.pitch = in.component.pitch;
    return core::Result<BackendVoiceMix>::Ok(mix);
}

} // namespace rt2::audio

#endif // RT2_AUDIO_SPATIAL_MATH_H
