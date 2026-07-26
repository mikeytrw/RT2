#include "AssetResolver.h"

#include <algorithm>
#include <system_error>

namespace rt2::core {

namespace {

// Normalize a path for comparison and storage. Forward slashes are kept
// portable; absolute results use the platform's preferred separators for
// filesystem calls. The locator never persists a path (W3-Q8); this is only
// for the in-memory `resolvedPath` and diagnostic display.
std::filesystem::path NormalizeResolved(const std::filesystem::path& p)
{
    std::error_code ec;
    auto lex = p.lexically_normal();
    if (lex.is_absolute())
        return lex;
    // Make absolute against the asset root for stable diagnostic output.
    return lex;
}

// Resolve a relative-or-absolute path against the asset root. No CWD
// fallback (W3-Q8). Returns empty if no regular file exists at the candidate.
std::filesystem::path ResolvePathNoCwd(const std::string& refPath,
                                       const std::filesystem::path& assetRoot)
{
    if (refPath.empty()) return {};

    std::filesystem::path p(refPath);
    p.make_preferred();

    std::error_code ec;
    if (p.is_absolute())
    {
        if (std::filesystem::exists(p, ec) &&
            std::filesystem::is_regular_file(p, ec))
            return p;
        return {};
    }

    auto resolved = assetRoot / p;
    if (std::filesystem::exists(resolved, ec) &&
        std::filesystem::is_regular_file(resolved, ec))
        return resolved;
    return {};
}

AssetDiagnostic MakeDiag(AssetDiagnostic::Severity sev, AssetKind kind,
                         const std::string& refPath,
                         const std::string& resolvedPath,
                         const UUID& entityUuid,
                         const std::string& entityName,
                         const std::string& sourceKey,
                         const std::string& detail)
{
    AssetDiagnostic d;
    d.severity     = sev;
    d.kind         = kind;
    d.refPath      = refPath;
    d.resolvedPath = resolvedPath;
    d.entityUuid   = entityUuid;
    d.entityName   = entityName;
    d.sourceKey    = sourceKey;
    d.detail       = detail;
    return d;
}

} // anonymous namespace

std::string AssetDiagnosticSortKey(const AssetDiagnostic& d)
{
    // Order: kind, refPath, entityUuid, sourceKey, severity, detail.
    std::string key;
    key.append(reinterpret_cast<const char*>(&d.kind), sizeof(d.kind));
    key += d.refPath;
    key.push_back('\0');
    key.append(reinterpret_cast<const char*>(&d.entityUuid), sizeof(d.entityUuid));
    key.push_back('\0');
    key += d.sourceKey;
    key.push_back('\0');
    key.append(reinterpret_cast<const char*>(&d.severity), sizeof(d.severity));
    key.push_back('\0');
    key += d.detail;
    return key;
}

AssetResolutionResult Resolve(const AssetReference& ref,
                              const AssetResolutionContext& ctx,
                              const UUID& entityUuid,
                              const std::string& entityName,
                              std::vector<AssetDiagnostic>& diagnostics)
{
    AssetResolutionResult result;
    const std::filesystem::path candidatePath =
        ResolvePathNoCwd(ref.path, ctx.assetRoot);
    const std::string candidatePathStr =
        candidatePath.empty() ? std::string()
                              : candidatePath.lexically_normal().string();

    const bool hasId = !ref.assetId.IsNull();

    // Nil ID (case 8): pure path fallback. Sidecar state is observed but
    // never compared against a requested ID (there is none). A present
    // sidecar contributes the effective ID; an absent sidecar signals
    // repair; a malformed sidecar is loud.
    if (!hasId)
    {
        if (candidatePath.empty())
        {
            diagnostics.push_back(MakeDiag(
                AssetDiagnostic::Missing, ref.kind, ref.path,
                (ctx.assetRoot / ref.path).lexically_normal().string(),
                entityUuid, entityName, ref.sourceKey,
                "asset file not found"));
            return result;
        }
        Error sidecarErr;
        const UUID sidecarId = ReadSidecarId(
            AssetSidecarPath(candidatePath), sidecarErr);
        if (sidecarErr.IsOk() && sidecarId.IsNull())
        {
            // Sidecar absent. Legal fallback; signal repair for later save.
            result.success = true;
            result.resolvedPath = candidatePath;
            result.source = AssetResolutionSource::PathFallback;
            result.effectiveId = UUID::Nil();
            result.identityRepairRequired = true;
            diagnostics.push_back(MakeDiag(
                AssetDiagnostic::Stale, ref.kind, ref.path,
                candidatePathStr, entityUuid, entityName, ref.sourceKey,
                "asset has no identity sidecar; identity repair required"));
            return result;
        }
        if (!sidecarErr.IsOk())
        {
            // Malformed sidecar. Fail loud; do not silently substitute.
            diagnostics.push_back(MakeDiag(
                AssetDiagnostic::Malformed, ref.kind, ref.path,
                candidatePathStr, entityUuid, entityName, ref.sourceKey,
                "sidecar malformed: " + sidecarErr.detail));
            return result;
        }
        // Sidecar parsed and is non-nil. Effective identity from sidecar.
        result.success = true;
        result.resolvedPath = candidatePath;
        result.source = AssetResolutionSource::PathFallback;
        result.effectiveId = sidecarId;
        result.identityRepairRequired = false; // sidecar authoritative
        return result;
    }

    // Non-nil ID: authoritative, looked up first. When no database is
    // supplied (the pre-W4 scene-load case), ID lookup is skipped and we
    // proceed directly to path+sidecar verification, which still detects
    // conflicts between the requested ID and the sidecar's claim.
    AssetIdLookupResult::Status idStatus = AssetIdLookupResult::Status::Missing;
    std::filesystem::path dbResolvedPath;
    const AssetRecord* idRecord = nullptr;
    if (ctx.database != nullptr)
    {
        const AssetIdLookupResult lookup = ctx.database->LookupById(ref.assetId);
        idStatus = lookup.status;
        idRecord = lookup.record;
        if (idStatus == AssetIdLookupResult::Status::Ambiguous)
        {
            // Case 7: ambiguous -> Conflict regardless of path.
            std::string detail = "asset ID claimed by multiple records";
            for (const auto& cp : lookup.candidatePaths)
                detail += "; " + cp;
            if (!candidatePath.empty())
                detail += "; fallback path=" + candidatePathStr;
            diagnostics.push_back(MakeDiag(
                AssetDiagnostic::Conflict, ref.kind, ref.path, candidatePathStr,
                entityUuid, entityName, ref.sourceKey, detail));
            return result;
        }
        if (idStatus == AssetIdLookupResult::Status::Unique &&
            idRecord != nullptr)
        {
            dbResolvedPath =
                ResolvePathNoCwd(idRecord->sourcePath, ctx.assetRoot);
        }
    }

    if (idStatus == AssetIdLookupResult::Status::Unique &&
        !dbResolvedPath.empty())
    {
        // Case 1/2: unique ID whose file exists wins. Stale reference path
        // is observable but does not defeat ID resolution.
        result.success = true;
        result.resolvedPath = dbResolvedPath;
        result.source = AssetResolutionSource::Id;
        result.effectiveId = idRecord->assetId;
        result.identityRepairRequired = false;
        if (!ref.path.empty() && candidatePath.empty())
        {
            diagnostics.push_back(MakeDiag(
                AssetDiagnostic::Stale, ref.kind, ref.path,
                (ctx.assetRoot / ref.path).lexically_normal().string(),
                entityUuid, entityName, ref.sourceKey,
                "reference path stale; resolved by asset ID"));
        }
        return result;
    }

    // ID did not locate an existing file (no database, ID missing, or DB
    // record's file gone). Try path fallback, but verify the sidecar against
    // the requested ID.
    if (candidatePath.empty())
    {
        // Case 6: neither ID nor path locates a regular file.
        diagnostics.push_back(MakeDiag(
            AssetDiagnostic::Missing, ref.kind, ref.path,
            (ctx.assetRoot / ref.path).lexically_normal().string(),
            entityUuid, entityName, ref.sourceKey,
            "asset file not found and ID not in database"));
        return result;
    }

    // Path exists. Check sidecar against the requested ID.
    Error sidecarErr;
    const UUID sidecarId = ReadSidecarId(
        AssetSidecarPath(candidatePath), sidecarErr);

    if (!sidecarErr.IsOk())
    {
        // Malformed sidecar is loud.
        diagnostics.push_back(MakeDiag(
            AssetDiagnostic::Malformed, ref.kind, ref.path,
            candidatePathStr, entityUuid, entityName, ref.sourceKey,
            "sidecar malformed: " + sidecarErr.detail));
        return result;
    }

    if (sidecarId.IsNull())
    {
        // Case 4: ID not located, path exists, sidecar absent -> fallback
        // succeeds with repair signal.
        result.success = true;
        result.resolvedPath = candidatePath;
        result.source = AssetResolutionSource::PathFallback;
        result.effectiveId = UUID::Nil();
        result.identityRepairRequired = true;
        diagnostics.push_back(MakeDiag(
            AssetDiagnostic::Stale, ref.kind, ref.path,
            candidatePathStr, entityUuid, entityName, ref.sourceKey,
            "asset ID not in database and sidecar absent; identity repair required"));
        return result;
    }

    if (sidecarId == ref.assetId)
    {
        // Case 3: database stale/missing but path sidecar claims same ID.
        // Fallback succeeds and reports the stale database state via the
        // Stale severity (W3-Q5): the file was found and resolution
        // SUCCEEDED, so Missing would mislabel it; the durable identity is
        // confirmed by the sidecar, but the database-side state needs
        // repair by a later save/migration.
        result.success = true;
        result.resolvedPath = candidatePath;
        result.source = AssetResolutionSource::PathFallback;
        result.effectiveId = sidecarId;
        result.identityRepairRequired = false; // sidecar confirms
        diagnostics.push_back(MakeDiag(
            AssetDiagnostic::Stale, ref.kind, ref.path,
            candidatePathStr, entityUuid, entityName, ref.sourceKey,
            "database stale; resolved by path sidecar matching requested ID"));
        return result;
    }

    // Case 5: sidecar claims a different ID. Never substitute identity.
    diagnostics.push_back(MakeDiag(
        AssetDiagnostic::Conflict, ref.kind, ref.path,
        candidatePathStr, entityUuid, entityName, ref.sourceKey,
        "sidecar ID " + sidecarId.ToString() +
        " disagrees with reference ID " + ref.assetId.ToString()));
    return result;
}

bool ResolveBatch(const std::vector<AssetBatchEntry>& entries,
                  const AssetResolutionContext& ctx,
                  std::vector<AssetDiagnostic>& diagnostics)
{
    const size_t base = diagnostics.size();
    bool allOk = true;
    for (const auto& entry : entries)
    {
        auto r = Resolve(entry.ref, ctx, entry.entityUuid, entry.entityName,
                         diagnostics);
        if (!r.success) allOk = false;
    }
    // Deterministic sort of the freshly appended entries. Use a stable sort
    // on the composite key so equal-key diagnostics keep insertion order.
    std::vector<AssetDiagnostic> fresh(
        diagnostics.begin() + base, diagnostics.end());
    std::stable_sort(fresh.begin(), fresh.end(),
        [](const AssetDiagnostic& a, const AssetDiagnostic& b) {
            return AssetDiagnosticSortKey(a) < AssetDiagnosticSortKey(b);
        });
    std::copy(fresh.begin(), fresh.end(),
              diagnostics.begin() + base);
    return allOk;
}

} // namespace rt2::core
