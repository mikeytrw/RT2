#include "AssetResolver.h"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace rt2::core {

namespace {

std::filesystem::path DefaultCanonicalAssetPathProbe(
    const std::filesystem::path& path, std::error_code& ec)
{
    return std::filesystem::weakly_canonical(path, ec);
}

} // namespace

std::filesystem::path CanonicalAssetPathWithProbe(
    const std::filesystem::path& path, CanonicalAssetPathProbe probe)
{
    if (!probe) probe = &DefaultCanonicalAssetPathProbe;
    std::error_code ec;
    const auto canonical = probe(path, ec);
    auto normalized = ec ? path.lexically_normal() : canonical.lexically_normal();
#ifdef _WIN32
    auto folded = normalized.generic_string();
    std::transform(folded.begin(), folded.end(), folded.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    normalized = std::filesystem::u8path(folded);
#endif
    return normalized;
}

std::filesystem::path CanonicalAssetPath(const std::filesystem::path& path)
{
    return CanonicalAssetPathWithProbe(path, &DefaultCanonicalAssetPathProbe);
}

Result<CapturedAssetIdentity> ResolveCapturedAssetIdentity(
    const AssetReference& ref, const AssetResolutionContext& ctx,
    const std::filesystem::path& capturedPath, const UUID& capturedSidecarId)
{
    if (ref.kind != AssetKind::Prefab ||
        (ref.path.empty() && ref.assetId.IsNull() && capturedPath.empty()))
        return Result<CapturedAssetIdentity>::Fail(
            Error::InvalidArgument, ref.path,
            "captured identity requires a prefab path or durable asset ID");
    const bool hasReferenceId = !ref.assetId.IsNull();
    const bool hasCapturedId = !capturedSidecarId.IsNull();
    if (hasReferenceId && hasCapturedId && ref.assetId != capturedSidecarId)
        return Result<CapturedAssetIdentity>::Fail(
            Error::InvalidArgument, ref.path,
            "Conflict: captured sidecar ID disagrees with the asset reference");

    const UUID effectiveId = hasCapturedId ? capturedSidecarId : ref.assetId;
    const AssetRecord* uniqueRecord = nullptr;
    if (!effectiveId.IsNull() && ctx.database != nullptr)
    {
        const auto lookup = ctx.database->LookupById(effectiveId);
        if (lookup.status == AssetIdLookupResult::Status::Ambiguous)
            return Result<CapturedAssetIdentity>::Fail(
                Error::InvalidArgument, ref.path,
                "Conflict: effective asset ID is claimed by multiple database records");
        if (lookup.status == AssetIdLookupResult::Status::Unique)
            uniqueRecord = lookup.record;
    }

    // A unique database claimant is authoritative when its recorded path is
    // usable without inventing a process-CWD root.  This check intentionally
    // precedes authored relative-path validation: an absolute DB claimant is
    // sufficient even when a watcher supplied no asset root.  If the claimant
    // is stale, CapturePrefabSource performs the existence-tested authored
    // fallback and calls this resolver again with the captured path.
    std::filesystem::path databasePath;
    if (uniqueRecord != nullptr && !uniqueRecord->sourcePath.empty())
    {
        const std::filesystem::path recorded(uniqueRecord->sourcePath);
        if (recorded.is_absolute())
            databasePath = CanonicalAssetPath(recorded);
        else if (!ctx.assetRoot.empty() && ctx.assetRoot.is_absolute())
            databasePath = CanonicalAssetPath(ctx.assetRoot / recorded);
    }

    const bool hasCapturedPath = !capturedPath.empty();
    std::filesystem::path authoredPath;
    if (!ref.path.empty())
    {
        authoredPath = std::filesystem::path(ref.path);
        if (authoredPath.is_relative())
        {
            if (ctx.assetRoot.empty() || !ctx.assetRoot.is_absolute())
                authoredPath.clear();
            else
                authoredPath = CanonicalAssetPath(ctx.assetRoot / authoredPath);
        }
        else
        {
            authoredPath = CanonicalAssetPath(authoredPath);
        }
    }

    // Before Capture, the same authority used by load and watcher selection
    // chooses a healthy unique database claimant.  Only an unusable claimant
    // falls back to the authored reference; no authored spelling is promoted
    // to captured evidence.  The regular-file probe is metadata-only: source
    // and sidecar bytes are still observed exclusively by Capture.
    std::error_code databaseProbeError;
    const bool healthyDatabasePath = !databasePath.empty() &&
        std::filesystem::is_regular_file(databasePath, databaseProbeError);
    if (!hasCapturedPath && !healthyDatabasePath && !ref.path.empty() &&
        authoredPath.empty() && std::filesystem::path(ref.path).is_relative())
        return Result<CapturedAssetIdentity>::Fail(
            Error::InvalidArgument, ref.path,
            "relative prefab identity requires an absolute asset root");
    std::filesystem::path path = capturedPath;
    if (path.empty() && healthyDatabasePath) path = databasePath;
    if (path.empty() && !authoredPath.empty()) path = authoredPath;
    if (path.empty() && !databasePath.empty()) path = databasePath;
    if (path.empty())
        return Result<CapturedAssetIdentity>::Fail(
            Error::MissingAsset, ref.path,
            "captured identity has no authored or database source path");
    if (path.is_relative())
    {
        if (ctx.assetRoot.empty() || !ctx.assetRoot.is_absolute())
            return Result<CapturedAssetIdentity>::Fail(
                Error::InvalidArgument, ref.path,
                "relative prefab identity requires an absolute asset root");
        path = ctx.assetRoot / path;
    }
    path = CanonicalAssetPath(path);

    // Before Capture reads the sidecar, a nil reference ID is a valid
    // path-only candidate.  The post-capture call supplies capturedSidecarId
    // and therefore enforces the durable-ID requirement below.
    if (effectiveId.IsNull())
        return Result<CapturedAssetIdentity>::Ok({path, UUID::Nil()});

    if (uniqueRecord != nullptr)
    {
        if (!databasePath.empty())
        {
            if (databasePath != path &&
                (authoredPath.empty() || authoredPath != path))
                return Result<CapturedAssetIdentity>::Fail(
                    Error::InvalidArgument, path.string(),
                    "Conflict: effective asset ID and captured path disagree");
        }
    }
    return Result<CapturedAssetIdentity>::Ok({path, effectiveId});
}

namespace {

struct PathCandidate
{
    std::filesystem::path path;
    std::filesystem::path attemptedPath;
    bool invalidRoot = false;
};

// Resolve a relative-or-absolute path against the asset root. No CWD
// fallback (W3-Q8). Relative candidates require an explicit absolute root;
// otherwise no filesystem query is made.
PathCandidate ResolvePathNoCwd(const std::string& refPath,
                               const std::filesystem::path& assetRoot)
{
    if (refPath.empty()) return {};

    std::filesystem::path p(refPath);
    p.make_preferred();
    p = p.lexically_normal();

    std::error_code ec;
    if (p.is_absolute())
    {
        PathCandidate candidate;
        candidate.attemptedPath = p;
        if (std::filesystem::exists(p, ec) &&
            std::filesystem::is_regular_file(p, ec))
            candidate.path = p;
        return candidate;
    }

    if (assetRoot.empty() || !assetRoot.is_absolute())
    {
        PathCandidate candidate;
        candidate.invalidRoot = true;
        return candidate;
    }

    PathCandidate candidate;
    candidate.attemptedPath =
        (assetRoot.lexically_normal() / p).lexically_normal();
    const auto& resolved = candidate.attemptedPath;
    if (std::filesystem::exists(resolved, ec) &&
        std::filesystem::is_regular_file(resolved, ec))
        candidate.path = resolved;
    return candidate;
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

const char* AssetDiagnosticSeverityName(
    AssetDiagnostic::Severity severity)
{
    switch (severity)
    {
        case AssetDiagnostic::Stale:       return "Stale";
        case AssetDiagnostic::NonPortable: return "NonPortable";
        case AssetDiagnostic::Missing:     return "Missing";
        case AssetDiagnostic::Malformed:   return "Malformed";
        case AssetDiagnostic::Unresolved:  return "Unresolved";
        case AssetDiagnostic::Conflict:    return "Conflict";
    }
    return "Unknown";
}

bool IsTerminalAssetDiagnostic(AssetDiagnostic::Severity severity)
{
    return severity >= AssetDiagnostic::Missing;
}

std::string FormatNonPortableAssetSummary(
    const std::vector<AssetDiagnostic>& diagnostics)
{
    std::vector<const AssetDiagnostic*> nonPortable;
    for (const auto& diagnostic : diagnostics)
    {
        if (diagnostic.severity == AssetDiagnostic::NonPortable)
            nonPortable.push_back(&diagnostic);
    }
    if (nonPortable.empty())
        return {};

    std::stable_sort(
        nonPortable.begin(), nonPortable.end(),
        [](const AssetDiagnostic* left, const AssetDiagnostic* right) {
            return AssetDiagnosticSortKey(*left) <
                   AssetDiagnosticSortKey(*right);
        });

    if (nonPortable.size() == 1)
    {
        return "non-portable asset reference: " +
               nonPortable.front()->refPath;
    }
    return std::to_string(nonPortable.size()) +
           " non-portable asset references; first: " +
           nonPortable.front()->refPath;
}

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
    // Successful advisory context precedes the terminal/decode consequence
    // for the same reference (for example Stale sidecar metadata followed by
    // Malformed image bytes). The remaining severities retain enum order.
    uint8_t severityRank = 0;
    switch (d.severity)
    {
        case AssetDiagnostic::Stale:
        case AssetDiagnostic::NonPortable:
            severityRank = 0; break;
        case AssetDiagnostic::Missing:    severityRank = 1; break;
        case AssetDiagnostic::Malformed:  severityRank = 2; break;
        case AssetDiagnostic::Unresolved: severityRank = 3; break;
        case AssetDiagnostic::Conflict:   severityRank = 4; break;
    }
    key.push_back(static_cast<char>(severityRank));
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
    const bool hasId = !ref.assetId.IsNull();

    // Non-nil ID: authoritative, looked up first. When no database is
    // supplied (the pre-W4 scene-load case), ID lookup is skipped and we
    // proceed directly to path+sidecar verification, which still detects
    // conflicts between the requested ID and the sidecar's claim.
    AssetIdLookupResult::Status idStatus = AssetIdLookupResult::Status::Missing;
    const AssetRecord* idRecord = nullptr;
    if (hasId && ctx.database != nullptr)
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
            const PathCandidate fallback =
                ResolvePathNoCwd(ref.path, ctx.assetRoot);
            if (!fallback.path.empty())
                detail += "; fallback path=" +
                          fallback.path.lexically_normal().string();
            diagnostics.push_back(MakeDiag(
                AssetDiagnostic::Conflict, ref.kind, ref.path,
                fallback.path.empty()
                    ? std::string{}
                    : fallback.path.lexically_normal().string(),
                entityUuid, entityName, ref.sourceKey, detail));
            return result;
        }
        if (idStatus == AssetIdLookupResult::Status::Unique &&
            idRecord != nullptr)
        {
            const PathCandidate dbCandidate =
                ResolvePathNoCwd(idRecord->sourcePath, ctx.assetRoot);
            if (dbCandidate.invalidRoot)
            {
                diagnostics.push_back(MakeDiag(
                    AssetDiagnostic::Malformed, ref.kind, ref.path, {},
                    entityUuid, entityName, ref.sourceKey,
                    "relative asset reference requires an absolute asset root"));
                return result;
            }
            if (!dbCandidate.path.empty())
            {
                // Case 1/2: a unique ID whose file exists wins. Only inspect
                // a valid fallback candidate for stale-path advice; an
                // otherwise unused invalid root cannot defeat ID success.
                result.success = true;
                result.resolvedPath = dbCandidate.path;
                result.source = AssetResolutionSource::Id;
                result.effectiveId = idRecord->assetId;
                result.identityRepairRequired = false;

                if (!ref.path.empty())
                {
                    const PathCandidate fallback =
                        ResolvePathNoCwd(ref.path, ctx.assetRoot);
                    if (!fallback.invalidRoot && fallback.path.empty())
                    {
                        diagnostics.push_back(MakeDiag(
                            AssetDiagnostic::Stale, ref.kind, ref.path,
                            fallback.attemptedPath.string(),
                            entityUuid, entityName, ref.sourceKey,
                            "reference path stale; resolved by asset ID"));
                    }
                }
                return result;
            }
        }
    }

    // Nil ID (case 8), missing ID, absent database, or a unique database
    // record whose file is gone all converge on the authored path fallback.
    const PathCandidate candidate =
        ResolvePathNoCwd(ref.path, ctx.assetRoot);
    if (candidate.invalidRoot)
    {
        diagnostics.push_back(MakeDiag(
            AssetDiagnostic::Malformed, ref.kind, ref.path, {},
            entityUuid, entityName, ref.sourceKey,
            "relative asset reference requires an absolute asset root"));
        return result;
    }

    const std::string candidatePathStr =
        candidate.path.empty()
            ? candidate.attemptedPath.string()
            : candidate.path.lexically_normal().string();
    if (candidate.path.empty())
    {
        diagnostics.push_back(MakeDiag(
            AssetDiagnostic::Missing, ref.kind, ref.path,
            candidatePathStr,
            entityUuid, entityName, ref.sourceKey,
            hasId
                ? "asset file not found and ID not in database"
                : "asset file not found"));
        return result;
    }

    Error sidecarErr;
    const UUID sidecarId = ReadSidecarId(
        AssetSidecarPath(candidate.path), sidecarErr);
    if (!sidecarErr.IsOk())
    {
        diagnostics.push_back(MakeDiag(
            AssetDiagnostic::Malformed, ref.kind, ref.path,
            candidatePathStr, entityUuid, entityName, ref.sourceKey,
            "sidecar malformed: " + sidecarErr.detail));
        return result;
    }

    if (!hasId)
    {
        result.success = true;
        result.resolvedPath = candidate.path;
        result.source = AssetResolutionSource::PathFallback;
        result.effectiveId = sidecarId;
        result.identityRepairRequired = sidecarId.IsNull();
        if (sidecarId.IsNull())
        {
            diagnostics.push_back(MakeDiag(
                AssetDiagnostic::Stale, ref.kind, ref.path,
                candidatePathStr, entityUuid, entityName, ref.sourceKey,
                "asset has no identity sidecar; identity repair required"));
        }
        return result;
    }

    if (sidecarId.IsNull())
    {
        // Case 4: ID not located, path exists, sidecar absent -> fallback
        // succeeds with repair signal.
        result.success = true;
        result.resolvedPath = candidate.path;
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
        // Case 3: path sidecar confirms the requested identity. With no
        // database (the normal pre-W4 host) this is fully healthy and emits
        // no diagnostic. A supplied database that failed to locate the ID is
        // genuinely stale and gets the advisory.
        result.success = true;
        result.resolvedPath = candidate.path;
        result.source = AssetResolutionSource::PathFallback;
        result.effectiveId = sidecarId;
        result.identityRepairRequired = false; // sidecar confirms
        if (ctx.database != nullptr)
        {
            diagnostics.push_back(MakeDiag(
                AssetDiagnostic::Stale, ref.kind, ref.path,
                candidatePathStr, entityUuid, entityName, ref.sourceKey,
                "database stale; resolved by path sidecar matching requested ID"));
        }
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
