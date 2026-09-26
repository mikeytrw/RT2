#pragma once

#ifndef RT2_AUDIO_STATUS_SNAPSHOT_H
#define RT2_AUDIO_STATUS_SNAPSHOT_H

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

// ============================================================================
// AudioStatusSnapshot — CPU-only production status snapshot + line formatter
// (audio A7, review P2/P2-finding-4).
//
// The Walnut Performance window renders exactly the lines
// FormatAudioStatusLines returns for the snapshot the host fills from live
// backend/world/controller state; the RT2ImGuiProbe drives the same struct
// and formatter with fake-backend/world/controller values (including
// deferred runtime voice failures, stop failures, and drain failures), so
// the status paths are covered by production code rather than a surrogate.
//
// CPU-only: standard library only. No miniaudio, device, Vulkan, ImGui, or
// Walnut types. Links anywhere the backend interface links.
// ============================================================================

struct AudioStatusLine
{
    std::string text;
    bool muted = false;  // UI renders dimmed (informational, not a failure)
    bool wrapped = false; // UI renders wrapped (failure/detail text)
};

struct AudioStatusSnapshot
{
    bool backendReady = false;
    bool productionNoDevice = false;
    std::string backendDetail;

    size_t runtimeVoices = 0;
    size_t previewVoices = 0;

    bool previewActive = false;
    std::string previewSourceName;
    bool previewSpatial = false;

    // Live Play-session bus gains when gainsLive; defaults otherwise.
    bool gainsLive = false;
    float gains[4] = { 1.0f, 1.0f, 1.0f, 1.0f }; // M, Mus, Fx, UI

    // One observable latest failure: preview start, refused Play, failed
    // Stop, failed completion drain, or a deferred runtime voice failure.
    bool hasFailure = false;
    std::string failureText;  // typed Error.Format(), never empty when set
    std::string failureAsset; // affected asset/voice, may be empty

    bool cacheReady = false;
    size_t decodedEntries = 0;
    size_t decodedBytes = 0;
    size_t providerEntries = 0;
};

inline std::vector<AudioStatusLine> FormatAudioStatusLines(
    const AudioStatusSnapshot& snapshot)
{
    std::vector<AudioStatusLine> lines;
    char buffer[256];

    if (!snapshot.backendReady)
    {
        lines.push_back({ "  Backend: not initialized", true, false });
    }
    else
    {
        lines.push_back(
            { snapshot.productionNoDevice ? "  Backend: no-device (diagnosed)"
                                          : "  Backend: hardware",
              false, false });
        if (!snapshot.backendDetail.empty())
            lines.push_back(
                { "  Reason: " + snapshot.backendDetail, false, true });
    }

    std::snprintf(buffer, sizeof(buffer),
                  "  Voices: runtime %u / preview %u / total %u",
                  static_cast<unsigned>(snapshot.runtimeVoices),
                  static_cast<unsigned>(snapshot.previewVoices),
                  static_cast<unsigned>(snapshot.runtimeVoices +
                                        snapshot.previewVoices));
    lines.push_back({ buffer, false, false });

    if (snapshot.previewActive)
    {
        lines.push_back(
            { "  Preview: '" + snapshot.previewSourceName + "' (" +
                  (snapshot.previewSpatial ? "spatial audition" : "2D") + ")",
              false, false });
    }

    if (snapshot.gainsLive)
    {
        std::snprintf(buffer, sizeof(buffer),
                      "  Gains: M %.2f / Mus %.2f / Fx %.2f / UI %.2f",
                      static_cast<double>(snapshot.gains[0]),
                      static_cast<double>(snapshot.gains[1]),
                      static_cast<double>(snapshot.gains[2]),
                      static_cast<double>(snapshot.gains[3]));
        lines.push_back({ buffer, false, false });
    }
    else
    {
        lines.push_back(
            { "  Gains: M 1.00 / Mus 1.00 / Fx 1.00 / UI 1.00 "
              "(defaults, no Play session)",
              true, false });
    }

    if (!snapshot.hasFailure)
    {
        lines.push_back({ "  Last failure: none", false, false });
    }
    else if (snapshot.failureAsset.empty())
    {
        lines.push_back(
            { "  Last failure: " + snapshot.failureText, false, true });
    }
    else
    {
        lines.push_back({ "  Last failure: " + snapshot.failureText + " [" +
                              snapshot.failureAsset + "]",
                          false, true });
    }

    if (snapshot.cacheReady)
    {
        std::snprintf(buffer, sizeof(buffer),
                      "  Cache: %u entries / %u bytes (decoded); "
                      "provider %u entries",
                      static_cast<unsigned>(snapshot.decodedEntries),
                      static_cast<unsigned>(snapshot.decodedBytes),
                      static_cast<unsigned>(snapshot.providerEntries));
        lines.push_back({ buffer, false, false });
    }
    return lines;
}

#endif // RT2_AUDIO_STATUS_SNAPSHOT_H
