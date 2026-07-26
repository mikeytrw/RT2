#pragma once

#ifndef RT2_CORE_SCRIPT_FIELD_RESOLVER_H
#define RT2_CORE_SCRIPT_FIELD_RESOLVER_H

#include "AssetResolver.h"
#include "ScriptFieldReconcile.h"

#include <cstddef>
#include <vector>

namespace rt2::core {

class SceneDocument;
class ScriptFieldRegistry;

struct ScriptFieldResolutionResult
{
    bool   changed = false;
    size_t resolvedEntities = 0;
    size_t skippedEntities = 0;
};

class ScriptFieldResolver
{
public:
    // Reconcile UUID-bearing ScriptComponents in UUID order. Components with
    // no EntityIdComponent are counted as skipped and diagnosed after the
    // ordered pass. A failed declaration parse is non-destructive: that
    // component is left byte-for-byte unchanged and receives a ParseFailed
    // diagnostic.
    //
    // This function does not change SceneMetadata::dirty. The host decides
    // whether reconciliation during load is a migration that should mark the
    // adopted document dirty.
    static ScriptFieldResolutionResult ResolveDocument(
        SceneDocument& document,
        ScriptFieldRegistry& registry,
        const AssetResolutionContext& assetContext,
        std::vector<AssetDiagnostic>& assetDiagnostics,
        std::vector<FieldDiagnostic>& outDiagnostics);
};

} // namespace rt2::core

#endif // RT2_CORE_SCRIPT_FIELD_RESOLVER_H
