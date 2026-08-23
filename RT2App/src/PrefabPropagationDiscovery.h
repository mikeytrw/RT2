#pragma once

#ifndef RT2_PREFAB_PROPAGATION_DISCOVERY_H
#define RT2_PREFAB_PROPAGATION_DISCOVERY_H

#include "PrefabPropagationContracts.h"
#include "PrefabSerializer.h"
#include "SceneSerializer.h"
#include "SceneDocument.h"

#include <functional>

namespace rt2::core {

// Read and fingerprint one durable prefab source without touching a scene.
// This is the shared identity seam for explicit and watcher-triggered live
// propagation; callers may inject the byte reader to prove one immutable read.
Result<CapturedPrefabSource> CapturePrefabSource(
    const AssetReference& source, const AssetResolutionContext& assets,
    const std::function<bool(const std::filesystem::path&, std::string&, Error&)>&
        readBytes = {});

Result<PrefabSourceFingerprint> ReadPrefabSourceFingerprint(
    const AssetReference& source, const AssetResolutionContext& assets,
    const std::function<bool(const std::filesystem::path&, std::string&, Error&)>&
        readBytes = {});

// All inputs are borrowed. PreparePrefabPropagation performs no writes to the
// document, asset database, source file, history, or resource tables.
struct PrefabPropagationDiscoveryRequest
{
    const SceneDocument* document = nullptr;
    AssetResolutionContext assets;
    AssetReference changedSource;
    std::uint64_t documentGeneration = 0;
    std::uint64_t resourceGeneration = 0;
    std::uint64_t authoringRevision = 0;

    // Preferred lifecycle input: one checked source/sidecar snapshot captured
    // after the owning context is refreshed. Prepare never rereads it.
    CapturedPrefabSource capturedSource;

    // Narrow byte-parser seam for tests and alternate CPU hosts. The path is
    // diagnostic context only; production parses the captured buffer and
    // never reopens it.
    std::function<bool(PrefabDocument&, const std::string&,
                       const std::filesystem::path&, Error&)> parseBytes;

};

// Discovers all live roots for changedSource and validates each complete
// instance independently. A source read/identity/parse failure is global and
// returns an Error; structural instance defects are represented as one
// deterministic quarantine diagnostic per instance, allowing valid siblings
// to remain eligible.
Result<DiscoveredPropagationPlan> PreparePrefabPropagation(
    const PrefabPropagationDiscoveryRequest& request);

} // namespace rt2::core

#endif // RT2_PREFAB_PROPAGATION_DISCOVERY_H
