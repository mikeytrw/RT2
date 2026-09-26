#pragma once

#ifndef RT2_AUDIO_CLIP_PROVIDER_H
#define RT2_AUDIO_CLIP_PROVIDER_H

#include "AudioBackend.h"

#include <memory>
#include <string>

// ============================================================================
// AudioClipProvider — CPU-only clip-generation provider seam (audio
// integration, A3 policy core).
//
// CPU-only: standard library + AudioBackend only. No miniaudio, device,
// filesystem, Vulkan, ImGui, or Walnut. Links into RT2Tests and
// RT2SliceRunner unchanged.
//
// The provider owns clip identity and immutable decoded generations;
// AudioWorld owns voice/session policy; IAudioBackend owns device voices.
// AudioWorld asks the injected provider for the generation behind a clip
// key and hands that shared generation to the backend for registration, so
// the decoded channel count is owned by actual decoded content, never by a
// caller-supplied claim. A3 tests serve scripted generations through the
// recording fake; A4 implements real miniaudio decoding behind this same
// interface without changing the world policy path.
// ============================================================================

namespace rt2::audio {

class IAudioClipProvider
{
public:
    virtual ~IAudioClipProvider() = default;

    // Returns the immutable decoded generation for `clipKey`, or a typed
    // error (missing/unreadable/unsupported content). The returned object
    // is shared: the provider, the backend registration, and every voice
    // reading it hold references, so overlapping old/new generations of
    // one key stay valid independently.
    virtual core::Result<std::shared_ptr<const DecodedAudioGeneration>> FetchDecodedGeneration(
        const std::string& clipKey) = 0;
};

} // namespace rt2::audio

#endif // RT2_AUDIO_CLIP_PROVIDER_H
