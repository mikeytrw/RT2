#pragma once

#ifndef RT2_PREFAB_PROPAGATION_DISCOVERY_H
#define RT2_PREFAB_PROPAGATION_DISCOVERY_H

#include "PrefabPropagationContracts.h"
#include "PrefabSerializer.h"
#include "SceneDocument.h"

#include <functional>

namespace rt2::core {

// All inputs are borrowed. PreparePrefabPropagation performs no writes to the
// document, asset database, source file, history, or resource tables.
struct PrefabPropagationDiscoveryRequest
{
    const SceneDocument* document = nullptr;
    AssetResolutionContext assets;
    AssetReference changedSource;
    std::uint64_t documentGeneration = 0;
    std::uint64_t resourceGeneration = 0;

    // Tests and recovery hosts may inject a checked, transactional loader.
    // The default is PrefabSerializer::Load.
    std::function<bool(PrefabDocument&, const std::filesystem::path&, Error&)> load;

    // The source fingerprint is computed exactly once after the source bytes
    // and sidecar identity have been read.  Hosts normally leave this empty;
    // the seam is injectable so tests can prove the batch never recomputes a
    // fingerprint for each dependent root.
    std::function<std::string(const std::string&, const UUID&)> fingerprint;
};

// Discovers all live roots for changedSource and validates each complete
// instance independently. A source read/identity/parse failure is global and
// returns an Error; structural instance defects are represented as one
// deterministic quarantine diagnostic per instance, allowing valid siblings
// to remain eligible.
Result<PrefabPropagationPlan> PreparePrefabPropagation(
    const PrefabPropagationDiscoveryRequest& request);

inline Result<PrefabPropagationPlan> DiscoverPrefabDependents(
    const PrefabPropagationDiscoveryRequest& request)
{
    return PreparePrefabPropagation(request);
}

} // namespace rt2::core

#endif // RT2_PREFAB_PROPAGATION_DISCOVERY_H
