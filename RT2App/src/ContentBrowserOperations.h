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
    // A successful callback may be a durable no-op (for example a prefab
    // source revision already applied). Keep that distinction through the
    // browser dispatcher so no phantom "changed" report is emitted.
    bool noOp = false;
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

// The browser's drag payload is dispatched through the existing scene-import
// callbacks. Keeping this seam CPU-only makes the payload contract permanent
// and testable without constructing ImGui or Walnut.
//
// Audio first import (A2): importAudioClip assigns or validates the clip's
// sidecar identity (the ImportAudioClipAsset production action below: the
// ResolveOrAssign flow, no decode) and reports success only when the durable
// identity is confirmed. Unlike model import there is no session-local
// fallback — an audio import with no written sidecar has no product, because
// the browser lists sidecar-backed records only. A failed sidecar write or
// malformed sidecar therefore returns false with a loud Error, and the
// dispatcher propagates it instead of reporting success. The inspector clip
// browse/drop authoring and Preview surface belong to A7; this callback is
// the Content Browser "new clip appears in the project" action only, and the
// host refreshes the project database after success (WalnutApp mirrors the
// glTF arm) so the clip becomes visible.
struct ContentBrowserDropCallbacks
{
    std::function<void(const std::string&)> importGltf;
    std::function<void(const std::string&, const ImportSettings&)> importObj;
    std::function<void(const std::string&)> instantiatePrefab;
    std::function<bool(const std::string&, Error&)> importAudioClip;
};

// Production audio first-import action (A2, CPU-only). Returns true with an
// empty error only when the clip's durable sidecar identity is confirmed on
// disk: a fresh mint must be written, a reuse must read back a valid
// sidecar. Any ResolveOrAssign error — failed sidecar write or malformed
// sidecar content — returns false with a loud Error naming the clip or
// sidecar path. Decodes nothing; mutates no database (the host refreshes,
// exactly like the model import arms).
struct AudioClipFirstImportResult
{
    bool minted = false;
    UUID assetId;
};
bool ImportAudioClipAsset(const std::string& droppedPath,
                          IUuidProvider& uuids,
                          AudioClipFirstImportResult& result,
                          Error& error);

// Resolve the file a first-import initiation picked into the in-project
// absolute path to import. A pick already under the active asset root is
// returned (normalized) as is; an external pick is copied to
// assetRoot/<filename> first. Loud failures: non-absolute or missing
// inputs, non-clip extensions, missing source file, and destination name
// clashes (an existing file is never silently overwritten). The Content
// Browser "Import Audio..." button and any future sidecar-less listing both
// route through this policy, so drag payloads are not required to import.
bool ResolveAudioClipImportSource(const std::filesystem::path& picked,
                                  const std::filesystem::path& assetRoot,
                                  std::filesystem::path& inProject,
                                  Error& error);

// Confirm the refreshed database exposes the imported clip: the record at
// the project-relative path must exist and carry the imported asset ID.
// The host checks this after its refresh before reporting success, so a
// refresh that silently dropped the clip cannot be announced as imported.
bool AudioClipRecordMatches(const AssetDatabase& database,
                            const std::string& relativePath,
                            const UUID& assetId);

bool DispatchContentBrowserAssetDrop(
    std::string_view path,
    const ContentBrowserDropCallbacks& callbacks,
    Error& error);

// Host policy is intentionally a small CPU seam so standalone mode and
// confirmation gates cannot be accidentally bypassed by UI call sites.
bool ContentBrowserCanOperate(bool projectActive);
bool ContentBrowserDeleteAllowed(bool confirmed, size_t dependantCount);

// Search the current immutable database snapshot. The query matches the
// portable source path and canonical asset ID; Windows matching is folded.
std::vector<AssetRecord> SearchContentBrowserAssets(
    const AssetDatabase& database, std::string_view query);

// Dependants are derived from the live scene, not AssetDatabase's optional
// cached dependency fields. IDs match exactly when both sides have one;
// sourcePath is the fallback when the scene has a nil or conflicting ID so
// this safety warning cannot under-report a dependant.
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
