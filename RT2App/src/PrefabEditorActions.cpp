#include "PrefabEditorActions.h"

#include "EditorCommandHistory.h"
#include "EditorStructuralCommands.h"
#include "SceneManager.h"
#include "core/PathTransaction.h"

#include <fstream>
#include <iterator>
#include <algorithm>
#include <cctype>

namespace rt2::core {
namespace {

bool CaptureOptionalFile(const std::filesystem::path& path, bool& exists,
                         std::vector<uint8_t>& bytes, Error& error)
{
    error = Error{};
    std::error_code ec;
    exists = std::filesystem::exists(path, ec);
    if (ec)
    {
        error.code = Error::Io;
        error.path = path.u8string();
        error.detail = "failed to inspect prefab destination: " + ec.message();
        return false;
    }
    bytes.clear();
    if (!exists) return true;
    if (!std::filesystem::is_regular_file(path, ec) || ec)
    {
        error.code = Error::Io;
        error.path = path.u8string();
        error.detail = "prefab destination is not a readable regular file";
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        error.code = Error::Io;
        error.path = path.u8string();
        error.detail = "failed to read existing prefab destination";
        return false;
    }
    bytes.assign(std::istreambuf_iterator<char>(input),
                 std::istreambuf_iterator<char>());
    if (input.bad())
    {
        error.code = Error::Io;
        error.path = path.u8string();
        error.detail = "failed while reading existing prefab destination";
        return false;
    }
    return true;
}

bool RestorePrefabPair(const std::filesystem::path& prefabPath,
                       bool assetExisted, const std::vector<uint8_t>& assetBytes,
                       bool sidecarExisted, const std::vector<uint8_t>& sidecarBytes,
                       Error& error)
{
    error = Error{};
    auto transaction = PrefabFileTransaction::Begin(
        prefabPath, AssetSidecarPath(prefabPath), true);
    if (!transaction.IsOk())
    {
        error = transaction.error;
        return false;
    }
    auto fail = [&](const Error& original) {
        error = original;
        const auto rollback = transaction.value->Rollback();
        if (!rollback.IsOk())
            error.detail += "; compensation rollback failed: " + rollback.error.detail;
        return false;
    };
    const auto captured = transaction.value->CapturePair();
    if (!captured.IsOk()) return fail(captured.error);
    const auto staged = transaction.value->Stage(
        sidecarExisted
            ? std::optional<std::vector<uint8_t>>(sidecarBytes)
            : std::nullopt,
        assetExisted
            ? std::optional<std::vector<uint8_t>>(assetBytes)
            : std::nullopt);
    if (!staged.IsOk()) return fail(staged.error);
    const auto installed = transaction.value->InstallSidecarThenAsset();
    if (!installed.IsOk()) return fail(installed.error);
    const auto committed = transaction.value->Finalize();
    if (!committed.IsOk()) return fail(committed.error);
    return true;
}

PrefabEditorActionResult Failure(const Error& error,
                                 const std::filesystem::path& path)
{
    PrefabEditorActionResult out;
    out.prefabPath = path;
    out.mutation = EditorMutationResult::Failure(
        error.code, error.path.empty() ? path.u8string() : error.path,
        error.detail);
    return out;
}

bool HasPrefabExtension(const std::filesystem::path& path)
{
    std::string extension = path.extension().u8string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".rt2prefab";
}

bool IsWithinAssetRoot(const std::filesystem::path& path,
                       const std::filesystem::path& assetRoot)
{
    if (assetRoot.empty()) return false;
    if (!assetRoot.is_absolute() || !path.is_absolute()) return false;
    const auto canonicalRoot = CanonicalAssetPath(assetRoot);
    const auto canonicalPath = CanonicalAssetPath(path);
    const auto relative = canonicalPath.lexically_relative(canonicalRoot);
    if (relative.empty() || relative.is_absolute()) return false;
    for (const auto& part : relative)
        if (part == "..") return false;
    return true;
}

} // namespace

PrefabEditorActionResult CreatePrefabAssetFromRoot(
    SceneManager& scene, EditorCommandHistory& history,
    const UUID& selectedRoot, const std::filesystem::path& prefabPath,
    const std::filesystem::path& assetRoot)
{
    if (selectedRoot.IsNull())
        return Failure(Error{Error::InvalidEntity, {},
            "select one ordinary scene root before creating a prefab"}, prefabPath);
    if (prefabPath.empty() || !HasPrefabExtension(prefabPath))
        return Failure(Error{Error::InvalidArgument, prefabPath.u8string(),
            "prefab destination must use the .rt2prefab extension"}, prefabPath);
    if (!IsWithinAssetRoot(prefabPath, assetRoot))
        return Failure(Error{Error::InvalidArgument, prefabPath.u8string(),
            "prefab destination must be inside the active project asset root"},
            prefabPath);

    const auto entity = scene.FindEntityByUuid(selectedRoot);
    if (entity == entt::null)
        return Failure(Error{Error::InvalidEntity, selectedRoot.ToString(),
            "selected prefab source entity no longer exists"}, prefabPath);
    const auto wrapped = SceneManager::EntityId{entity};
    if (scene.GetParent(wrapped).IsValid())
        return Failure(Error{Error::InvalidEntity, selectedRoot.ToString(),
            "prefab creation requires a scene-root entity"}, prefabPath);
    const auto& registry = scene.GetECS().registry;
    if (registry.all_of<PrefabInstanceComponent>(entity) ||
        registry.all_of<PrefabMemberComponent>(entity))
        return Failure(Error{Error::InvalidArgument, selectedRoot.ToString(),
            "linked prefab entities cannot be used as a new prefab root"}, prefabPath);
    const auto sourceSnapshot = scene.CaptureSubtreeSnapshot({selectedRoot});
    const auto linkedDescendant = std::find_if(
        sourceSnapshot.entities.begin(), sourceSnapshot.entities.end(),
        [](const SubtreeEntityRecord& record) {
            return record.hasPrefabInstance || record.hasPrefabMember;
        });
    if (linkedDescendant != sourceSnapshot.entities.end())
        return Failure(Error{Error::InvalidArgument, selectedRoot.ToString(),
            "prefab creation requires an ordinary subtree with no prefab links"},
            prefabPath);

    bool existedBefore = false;
    std::vector<uint8_t> beforeBytes;
    bool sidecarExistedBefore = false;
    std::vector<uint8_t> beforeSidecarBytes;
    Error captureError;
    if (!CaptureOptionalFile(prefabPath, existedBefore, beforeBytes, captureError))
        return Failure(captureError, prefabPath);
    if (!CaptureOptionalFile(AssetSidecarPath(prefabPath), sidecarExistedBefore,
            beforeSidecarBytes, captureError))
        return Failure(captureError, prefabPath);

    auto created = scene.CreatePrefabFromSubtree({selectedRoot}, prefabPath);
    if (!created.ok) return Failure(created.error, prefabPath);

    bool afterExists = false;
    std::vector<uint8_t> afterBytes;
    Error afterError;
    if (!CaptureOptionalFile(prefabPath, afterExists, afterBytes, afterError) ||
        !afterExists)
    {
        Error rollbackError;
        const bool restored = RestorePrefabPair(prefabPath,
            existedBefore, beforeBytes, sidecarExistedBefore,
            beforeSidecarBytes, rollbackError);
        Error error = afterError.IsOk()
            ? Error{Error::Io, prefabPath.u8string(),
                "prefab create reported success but produced no asset file"}
            : afterError;
        if (!restored)
            error.detail += "; pair rollback failed: " + rollbackError.detail;
        return Failure(error, prefabPath);
    }

    auto command = MakeCreatePrefabCommand(
        prefabPath, created, beforeBytes, existedBefore);
    if (!command)
    {
        Error rollbackError;
        const bool restored = RestorePrefabPair(prefabPath,
            existedBefore, beforeBytes, sidecarExistedBefore,
            beforeSidecarBytes, rollbackError);
        Error error{Error::InvalidRuntimeState, prefabPath.u8string(),
            "failed to construct prefab creation history entry"};
        if (!restored)
            error.detail += "; pair rollback failed: " + rollbackError.detail;
        return Failure(error, prefabPath);
    }

    EditorMutationResult applied;
    applied.success = true;
    applied.effective = true;
    applied.syncImpact = SyncImpact::None;
    applied.recoveryWarning = created.recoveryWarning;
    const auto recorded = history.RecordApplied(std::move(command), scene, applied);
    if (!recorded.success)
    {
        Error rollbackError;
        const bool restored = RestorePrefabPair(prefabPath,
            existedBefore, beforeBytes, sidecarExistedBefore,
            beforeSidecarBytes, rollbackError);
        Error error = recorded.error;
        if (!restored)
            error.detail += "; prefab pair rollback failed: " + rollbackError.detail;
        return Failure(error, prefabPath);
    }

    PrefabEditorActionResult out;
    out.mutation = recorded;
    out.prefabPath = prefabPath;
    out.selectedRoot = selectedRoot;
    out.message = "Prefab asset created; current selection unchanged: " +
        prefabPath.filename().u8string();
    return out;
}

PrefabEditorActionResult InstantiatePrefabAsset(
    SceneManager& scene, EditorCommandHistory& history,
    const std::filesystem::path& prefabPath)
{
    if (prefabPath.empty() || !HasPrefabExtension(prefabPath))
        return Failure(Error{Error::InvalidArgument, prefabPath.u8string(),
            "only .rt2prefab assets can be instantiated"}, prefabPath);

    const auto count = scene.CountCanonicalPrefabEntities(prefabPath);
    if (!count.IsOk()) return Failure(count.error, prefabPath);
    if (count.value == 0)
        return Failure(Error{Error::InvalidArgument, prefabPath.u8string(),
            "prefab contains no entities"}, prefabPath);

    const auto knownUuids = scene.ReserveKnownUuids(count.value);
    const auto checkpoint = scene.CapturePrefabInstantiationCheckpoint();
    std::vector<AssetDiagnostic> diagnostics;
    auto instantiated = scene.InstantiatePrefabWithUuids(
        prefabPath, knownUuids, diagnostics);
    if (!instantiated.mutation.success)
    {
        auto out = Failure(instantiated.mutation.error, prefabPath);
        out.diagnostics = std::move(diagnostics);
        return out;
    }

    const auto snapshot = scene.CaptureSubtreeSnapshot(instantiated.createdRoots);
    auto command = MakeInstantiatePrefabCommand(snapshot, instantiated.createdRoots);
    if (!command)
    {
        const auto rollback = scene.RollbackPrefabInstantiation(snapshot, checkpoint);
        Error error{Error::InvalidRuntimeState, prefabPath.u8string(),
            "failed to construct prefab instantiation history entry"};
        if (!rollback.success)
            error.detail += "; rollback failed: " + rollback.error.detail;
        return Failure(error, prefabPath);
    }

    const auto recorded = history.RecordApplied(
        std::move(command), scene, instantiated.mutation);
    if (!recorded.success)
    {
        const auto rollback = scene.RollbackPrefabInstantiation(snapshot, checkpoint);
        Error error = recorded.error;
        if (!rollback.success)
            error.detail += "; prefab rollback failed: " + rollback.error.detail;
        auto out = Failure(error, prefabPath);
        out.diagnostics = std::move(diagnostics);
        return out;
    }

    PrefabEditorActionResult out;
    out.mutation = recorded;
    out.prefabPath = prefabPath;
    out.diagnostics = std::move(diagnostics);
    if (!instantiated.createdRoots.empty())
        out.selectedRoot = instantiated.createdRoots.front();
    out.message = "Prefab instantiated: " + prefabPath.filename().u8string();
    return out;
}

} // namespace rt2::core
