#pragma once

#ifndef RT2_CORE_CONTENT_BROWSER_OPERATIONS_H
#define RT2_CORE_CONTENT_BROWSER_OPERATIONS_H

#include "AssetDatabase.h"
#include "AssetResolver.h"
#include "core/Error.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace rt2::core {

class SceneDocument;

// CPU-only result state shared by the content-browser host and tests. A
// partial failure is deliberately distinct from a clean failure: the source
// tree may already have changed and must not be followed by an automatic
// database refresh that hides the orphan or half-moved pair.
struct ContentBrowserOperationReport
{
    bool changed = false;
    bool partialFailure = false;
    std::vector<AssetDiagnostic> diagnostics;
};

struct ContentBrowserDependant
{
    UUID        entityUuid;
    std::string entityName;
    AssetKind   kind = AssetKind::Unknown;
    std::string sourceKey;
    std::string sourcePath;
};

// Filesystem seams make the irreversible ordering and partial-failure
// contract testable without touching repository assets. Empty hooks use the
// real std::filesystem operations.
struct ContentBrowserIoHooks
{
    std::function<bool(const std::filesystem::path&,
                       const std::filesystem::path&,
                       Error&)> moveFile;
    std::function<bool(const std::filesystem::path&, Error&)> removeFile;
    std::function<bool(const std::filesystem::path&, Error&)> createDirectories;
};

using ContentBrowserReimportCallback = std::function<bool(
    const AssetRecord& record,
    const std::filesystem::path& sourcePath,
    std::vector<AssetDiagnostic>& diagnostics,
    Error& error)>;

// Host policy is intentionally a small CPU seam so standalone mode and
// confirmation gates cannot be accidentally bypassed by UI call sites.
bool ContentBrowserCanOperate(bool projectActive);
bool ContentBrowserDeleteAllowed(bool confirmed, size_t dependantCount);

// Search the current immutable database snapshot. The query matches the
// portable source path and canonical asset ID; Windows matching is folded.
std::vector<AssetRecord> SearchContentBrowserAssets(
    const AssetDatabase& database, std::string_view query);

// Dependants are derived from the live scene, not AssetDatabase's optional
// cached dependency fields. ID is authoritative when present; sourcePath is
// the fallback for nil-ID legacy references.
std::vector<ContentBrowserDependant> FindContentBrowserDependants(
    const SceneDocument& document,
    const AssetRecord& record,
    const std::filesystem::path& assetRoot);

bool RenameContentBrowserAsset(
    const std::filesystem::path& assetRoot,
    const AssetRecord& record,
    std::string_view newName,
    ContentBrowserOperationReport& report,
    Error& error,
    const ContentBrowserIoHooks& hooks = {});

bool MoveContentBrowserAsset(
    const std::filesystem::path& assetRoot,
    const AssetRecord& record,
    const std::filesystem::path& destinationDirectory,
    ContentBrowserOperationReport& report,
    Error& error,
    const ContentBrowserIoHooks& hooks = {});

// Delete is source-first. If source deletion succeeds but sidecar deletion
// fails, the orphaned sidecar remains by design and its path is named in the
// Conflict diagnostic; callers must not roll back or silently rescan.
bool DeleteContentBrowserAsset(
    const std::filesystem::path& assetRoot,
    const AssetRecord& record,
    ContentBrowserOperationReport& report,
    Error& error,
    const ContentBrowserIoHooks& hooks = {});

// Reimport dispatch is supplied by the host so this module remains linkable
// into CPU-only tests and RT2SliceRunner. The callback must use the existing
// SceneManager import path and must not mint or replace the sidecar ID.
bool ReimportContentBrowserAsset(
    const std::filesystem::path& assetRoot,
    const AssetRecord& record,
    const ContentBrowserReimportCallback& reimport,
    ContentBrowserOperationReport& report,
    Error& error);

} // namespace rt2::core

#endif // RT2_CORE_CONTENT_BROWSER_OPERATIONS_H
