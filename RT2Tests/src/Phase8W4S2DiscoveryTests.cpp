#include <doctest/doctest.h>

#include "AssetIdentity.h"
#include "PrefabPropagationDiscovery.h"
#include "PrefabPropagationLive.h"
#include "PrefabPropagationService.h"

#include <filesystem>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string_view>

using namespace rt2::core;

namespace
{
const UUID kAsset = UUID::Parse("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
const UUID kTemplateRoot = UUID::Parse("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
const UUID kTemplateChild = UUID::Parse("cccccccc-cccc-4ccc-8ccc-cccccccccccc");
const UUID kRecordRoot = UUID::Parse("77777777-7777-4777-8777-777777777777");
const UUID kRecordChild = UUID::Parse("88888888-8888-4888-8888-888888888888");
const UUID kRootA = UUID::Parse("11111111-1111-4111-8111-111111111111");
const UUID kChildA = UUID::Parse("11111111-1111-4111-8111-111111111112");
const UUID kRootB = UUID::Parse("22222222-2222-4222-8222-222222222222");
const UUID kChildB = UUID::Parse("22222222-2222-4222-8222-222222222223");
const UUID kRootBad = UUID::Parse("33333333-3333-4333-8333-333333333333");
const UUID kInstanceA = UUID::Parse("44444444-4444-4444-8444-444444444444");
const UUID kInstanceB = UUID::Parse("55555555-5555-4555-8555-555555555555");
const UUID kInstanceBad = UUID::Parse("66666666-6666-4666-8666-666666666666");
const UUID kWrongAsset = UUID::Parse("99999999-9999-4999-8999-999999999999");

struct TempPrefabGuard
{
    std::filesystem::path directory;

    TempPrefabGuard() = default;
    TempPrefabGuard(const TempPrefabGuard&) = delete;
    TempPrefabGuard& operator=(const TempPrefabGuard&) = delete;
    TempPrefabGuard(TempPrefabGuard&& other) noexcept
        : directory(std::move(other.directory))
    {
        other.directory.clear();
    }
    TempPrefabGuard& operator=(TempPrefabGuard&& other) noexcept
    {
        if (this == &other) return *this;
        std::error_code ec;
        std::filesystem::remove_all(directory, ec);
        directory = std::move(other.directory);
        other.directory.clear();
        return *this;
    }

    std::filesystem::path source() const { return directory / "source.rt2prefab"; }

    ~TempPrefabGuard()
    {
        if (directory.empty()) return;
        std::error_code ec;
        std::filesystem::remove_all(directory, ec);
        if (ec || std::filesystem::exists(directory, ec)) std::abort();
    }
};

TempPrefabGuard TempPrefab()
{
    static std::atomic<std::uint64_t> sequence{0};
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    TempPrefabGuard guard;
    guard.directory = std::filesystem::temp_directory_path() /
        ("rt2_w4_s2_discovery_" + std::to_string(ticks) + "_" +
         std::to_string(sequence.fetch_add(1)));
    std::error_code ec;
    if (!std::filesystem::create_directories(guard.directory, ec) || ec)
        throw std::runtime_error("could not create unique S2 test directory: " +
                                 guard.directory.string());
    return guard;
}

std::string ReadTestBytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("test snapshot file could not be opened");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool SameAssetReference(const AssetReference& a, const AssetReference& b)
{
    return a.kind == b.kind && a.path == b.path &&
           a.importSettings == b.importSettings && a.sourceKey == b.sourceKey &&
           a.assetId == b.assetId;
}

bool SameMaterial(const SceneMaterial& a, const SceneMaterial& b)
{
    return a.type == b.type && a.baseColor == b.baseColor &&
           a.baseAlpha == b.baseAlpha && a.metallic == b.metallic &&
           a.roughness == b.roughness && a.ior == b.ior &&
           a.transmissionFactor == b.transmissionFactor &&
           a.emissiveColor == b.emissiveColor &&
           a.emissiveIntensity == b.emissiveIntensity &&
           a.baseColorTextureIndex == b.baseColorTextureIndex &&
           a.normalTextureIndex == b.normalTextureIndex &&
           a.emissiveTextureIndex == b.emissiveTextureIndex &&
           a.metallicRoughnessTextureIndex == b.metallicRoughnessTextureIndex &&
           a.alphaMode == b.alphaMode && a.alphaCutoff == b.alphaCutoff &&
           a.sourceKey == b.sourceKey;
}

bool SameTexture(const SceneTexture& a, const SceneTexture& b)
{
    return SameAssetReference(a.ref, b.ref) && a.width == b.width &&
           a.height == b.height && a.channels == b.channels &&
           a.pixels == b.pixels && a.isHDR == b.isHDR &&
           a.floatPixels == b.floatPixels && a.isSRGB == b.isSRGB;
}

bool SameMesh(const MeshData& a, const MeshData& b)
{
    return a.vertices == b.vertices && a.indices == b.indices &&
           a.normals == b.normals && a.uvs == b.uvs &&
           a.tangents == b.tangents &&
           a.materialIndices == b.materialIndices && a.name == b.name &&
           a.boundsMin == b.boundsMin && a.boundsMax == b.boundsMax &&
           a.boundsValid == b.boundsValid;
}

bool SameEnvironment(const EnvironmentSettings& a, const EnvironmentSettings& b)
{
    return SameAssetReference(a.ref, b.ref) && a.width == b.width &&
           a.height == b.height && a.floatPixels == b.floatPixels;
}

bool SameCamera(const SceneCamera& a, const SceneCamera& b)
{
    return a.position == b.position &&
           a.forwardDirection == b.forwardDirection &&
           a.verticalFOV == b.verticalFOV && a.aperture == b.aperture &&
           a.focusDistance == b.focusDistance;
}

void AddEntity(SceneDocument& document, const UUID& uuid, const UUID& instance,
               const UUID& templ, entt::entity parent = entt::null)
{
    auto entity = document.ecs.registry.create();
    REQUIRE(document.AssignKnownUuid(entity, uuid));
    document.ecs.registry.emplace<Hierarchy>(entity, parent,
                                               std::vector<entt::entity>{});
    document.ecs.registry.emplace<PrefabMemberComponent>(
        entity, PrefabMemberComponent{instance, templ, {}});
    if (parent != entt::null)
        document.ecs.registry.get<Hierarchy>(parent).children.push_back(entity);
}

PrefabPropagationDiscoveryRequest Request(SceneDocument& document,
                                          const std::filesystem::path& source)
{
    PrefabPropagationDiscoveryRequest request;
    request.document = &document;
    request.assets.assetRoot = source.parent_path();
    request.changedSource = AssetReference{AssetKind::Prefab, source.string(),
                                           {}, {}, kAsset};
    request.documentGeneration = 71;
    request.resourceGeneration = 19;
    request.documentGenerationCaptured = true;
    request.resourceGenerationCaptured = true;
    request.authoringRevision = 23;
    const auto captured = CapturePrefabSource(request.changedSource,
                                              request.assets);
    if (captured.IsOk()) request.capturedSource = captured.value;
    return request;
}

TEST_CASE("Phase 8 W4 Ticket 2: captured identity preserves ambiguity, stale fallback, and one read pair")
{
    const auto temp = TempPrefab();
    const auto source = temp.source();
    Error error;
    REQUIRE(PrefabSerializer::WriteBytesAtomic(source, "ticket2-source", error));
    REQUIRE(WriteSidecarId(AssetSidecarPath(source), kAsset, error));

    AssetDatabase staleDatabase;
    std::vector<AssetDatabaseDiagnostic> databaseDiagnostics;
    staleDatabase.AddOrUpdate(
        AssetRecord{kAsset, (temp.directory / "stale.rt2prefab").string(), {},
                    AssetIdentityAuthority::Reference, {}, {}, {}},
        databaseDiagnostics);
    AssetReference reference{AssetKind::Prefab, source.string(), {}, {}, kAsset};
    std::size_t reads = 0;
    const auto captured = CapturePrefabSource(
        reference, AssetResolutionContext{temp.directory, &staleDatabase},
        [&](const std::filesystem::path& path, std::string& bytes, Error& readError) {
            ++reads;
            std::ifstream input(path, std::ios::binary);
            if (!input)
            {
                readError = {Error::Io, path.string(), "ticket2 capture read failed"};
                return false;
            }
            bytes.assign(std::istreambuf_iterator<char>(input),
                         std::istreambuf_iterator<char>());
            return true;
        });
    REQUIRE(captured.IsOk());
    CHECK(captured.value.fingerprint.normalizedPath ==
          CanonicalAssetPath(source));
    CHECK(captured.value.fingerprint.assetId == kAsset);
    CHECK(reads == 2);

    AssetDatabase ambiguous;
    ambiguous.AddOrUpdate(
        AssetRecord{kAsset, (temp.directory / "a.rt2prefab").string(), {},
                    AssetIdentityAuthority::Reference, {}, {}, {}},
        databaseDiagnostics);
    ambiguous.AddOrUpdate(
        AssetRecord{kAsset, (temp.directory / "b.rt2prefab").string(), {},
                    AssetIdentityAuthority::Reference, {}, {}, {}},
        databaseDiagnostics);
    const auto rejected = ResolveCapturedAssetIdentity(
        reference, AssetResolutionContext{temp.directory, &ambiguous});
    CHECK_FALSE(rejected.IsOk());
    CHECK(rejected.error.detail.find("Conflict:") == 0);

    const auto conflictingSidecar = ResolveCapturedAssetIdentity(
        reference, AssetResolutionContext{temp.directory, nullptr},
        CanonicalAssetPath(source), kWrongAsset);
    CHECK_FALSE(conflictingSidecar.IsOk());
    CHECK(conflictingSidecar.error.detail.find("Conflict:") == 0);
    // Named RED/GREEN faults: path-only dedupe or ambiguous-ID fallback would
    // either accept the database conflict or read a second source/sidecar pair.
}

TEST_CASE("Phase 8 W4 Ticket 2: healthy DB authority precedes rootless authored validation")
{
    const auto temp = TempPrefab();
    const auto source = temp.source();
    Error error;
    REQUIRE(PrefabSerializer::WriteBytesAtomic(source, "ticket2-healthy-db", error));
    REQUIRE(WriteSidecarId(AssetSidecarPath(source), kAsset, error));

    AssetDatabase database;
    std::vector<AssetDatabaseDiagnostic> databaseDiagnostics;
    database.AddOrUpdate(
        AssetRecord{kAsset, source.string(), {},
                    AssetIdentityAuthority::Sidecar, {}, {}, {}},
        databaseDiagnostics);

    // The authored spelling is relative and there is deliberately no asset
    // root.  The unique absolute DB claimant is still a valid authority and
    // must be selected before the no-CWD relative-path guard.
    const AssetReference relative{AssetKind::Prefab, "source.rt2prefab", {}, {}, kAsset};
    const auto resolved = ResolveCapturedAssetIdentity(
        relative, AssetResolutionContext{{}, &database});
    REQUIRE(resolved.IsOk());
    CHECK(resolved.value.normalizedPath == CanonicalAssetPath(source));
    CHECK(resolved.value.effectiveId == kAsset);

    std::size_t reads = 0;
    const auto captured = CapturePrefabSource(
        relative, AssetResolutionContext{{}, &database},
        [&](const std::filesystem::path& path, std::string& bytes, Error& readError) {
            ++reads;
            std::ifstream input(path, std::ios::binary);
            if (!input)
            {
                readError = {Error::Io, path.string(), "healthy DB capture read failed"};
                return false;
            }
            bytes.assign(std::istreambuf_iterator<char>(input),
                         std::istreambuf_iterator<char>());
            return true;
        });
    REQUIRE(captured.IsOk());
    CHECK(captured.value.fingerprint.normalizedPath == CanonicalAssetPath(source));
    CHECK(captured.value.fingerprint.assetId == kAsset);
    CHECK(reads == 2);

    SceneDocument document;
    const auto root = document.ecs.registry.create();
    REQUIRE(document.AssignKnownUuid(root, kRootA));
    document.ecs.registry.emplace<PrefabInstanceComponent>(root,
        PrefabInstanceComponent{relative, kInstanceA});
    document.ecs.registry.emplace<PrefabMemberComponent>(root,
        PrefabMemberComponent{kInstanceA, kTemplateRoot, {}});
    std::vector<AssetDiagnostic> diagnostics;
    const auto watcher = CollectReferencedPrefabSources(
        document, AssetResolutionContext{temp.directory, &database},
        {std::filesystem::path{"source.rt2prefab"}}, false, diagnostics);
    REQUIRE(watcher.size() == 1);
    CHECK(watcher.front().path == "source.rt2prefab");
    // Named RED/GREEN faults: restoring the early relative-root rejection,
    // canonicalizing the relative watcher path against CWD, or overwriting
    // the authored spelling with the DB record makes one of these assertions
    // fail.  A healthy alias must remain one validated (canonical path, ID).
}

TEST_CASE("Phase 8 W4 Ticket 2: healthy aliases dedupe in either watcher and load order")
{
    const auto temp = TempPrefab();
    const auto source = temp.source();
    Error error;
    REQUIRE(PrefabSerializer::WriteBytesAtomic(source, "ticket2-alias", error));
    REQUIRE(WriteSidecarId(AssetSidecarPath(source), kAsset, error));
    const std::string canonicalSpelling = source.string();
    const std::string aliasSpelling =
        (temp.directory / "." / source.filename()).string();

    const auto makeDocument = [&](bool reverse) {
        SceneDocument document;
        const std::string spellings[] = {canonicalSpelling, aliasSpelling};
        const UUID roots[] = {kRootA, kRootB};
        const UUID instances[] = {kInstanceA, kInstanceB};
        for (int n = 0; n != 2; ++n)
        {
            const int index = reverse ? 1 - n : n;
            const auto root = document.ecs.registry.create();
            REQUIRE(document.AssignKnownUuid(root, roots[index]));
            document.ecs.registry.emplace<PrefabInstanceComponent>(root,
                PrefabInstanceComponent{AssetReference{
                    AssetKind::Prefab, spellings[index], {}, {}, kAsset},
                    instances[index]});
            document.ecs.registry.emplace<PrefabMemberComponent>(root,
                PrefabMemberComponent{instances[index], kTemplateRoot, {}});
        }
        return document;
    };

    SceneDocument watcherFirst = makeDocument(false);
    SceneDocument watcherSecond = makeDocument(true);
    std::vector<AssetDiagnostic> firstDiagnostics;
    std::vector<AssetDiagnostic> secondDiagnostics;
    const auto firstWatcher = CollectReferencedPrefabSources(
        watcherFirst, AssetResolutionContext{temp.directory, nullptr},
        {source}, false, firstDiagnostics);
    const auto secondWatcher = CollectReferencedPrefabSources(
        watcherSecond, AssetResolutionContext{temp.directory, nullptr},
        {source}, false, secondDiagnostics);
    REQUIRE(firstWatcher.size() == 1);
    REQUIRE(secondWatcher.size() == 1);
    CHECK(firstWatcher.front().path == secondWatcher.front().path);
    CHECK(CanonicalAssetPath(firstWatcher.front().path) == CanonicalAssetPath(source));

    const auto runLoad = [&](SceneDocument& document, std::string& capturedPath) {
        std::size_t captures = 0;
        PrefabPropagationLoadHooks hooks;
        hooks.capture = [&](const AssetReference& reference,
                            const AssetResolutionContext&) {
            ++captures;
            capturedPath = reference.path;
            CapturedPrefabSource captured;
            captured.fingerprint = {CanonicalAssetPath(source), kAsset, "alias-digest"};
            captured.prefabBytes = "captured";
            captured.sidecarBytes = "sidecar";
            return Result<CapturedPrefabSource>::Ok(std::move(captured));
        };
        hooks.prepare = [](const PrefabPropagationDiscoveryRequest& request) {
            DiscoveredPropagationPlan plan;
            plan.source = request.capturedSource.fingerprint;
            plan.capturedSource = request.capturedSource;
            plan.documentGeneration = request.documentGeneration;
            plan.resourceGeneration = request.resourceGeneration;
            plan.documentGenerationCaptured = request.documentGenerationCaptured;
            plan.resourceGenerationCaptured = request.resourceGenerationCaptured;
            return Result<DiscoveredPropagationPlan>::Ok(std::move(plan));
        };
        const auto result = ReconcilePrefabPropagationForLoad(
            document, AssetResolutionContext{temp.directory, nullptr}, hooks);
        REQUIRE(result.IsOk());
        CHECK(captures == 1);
    };
    std::string firstLoadPath;
    std::string secondLoadPath;
    runLoad(watcherFirst, firstLoadPath);
    runLoad(watcherSecond, secondLoadPath);
    CHECK(firstLoadPath == secondLoadPath);
    // Named RED/GREEN fault: path-only/raw-order dedupe retains two reads or
    // lets the first registry spelling choose a different source result.
}

TEST_CASE("Phase 8 W4 Ticket 2: load and watcher share healthy DB identity before capture")
{
    const auto temp = TempPrefab();
    const auto authored = temp.directory / "authored.rt2prefab";
    const auto databaseSourceA = temp.directory / "database-a.rt2prefab";
    const auto databaseSourceB = temp.directory / "database-b.rt2prefab";
    Error error;
    REQUIRE(PrefabSerializer::WriteBytesAtomic(authored, "authored-spelling", error));
    REQUIRE(WriteSidecarId(AssetSidecarPath(authored), kAsset, error));
    REQUIRE(PrefabSerializer::WriteBytesAtomic(
        databaseSourceA, "healthy-database-a", error));
    REQUIRE(WriteSidecarId(AssetSidecarPath(databaseSourceA), kAsset, error));
    REQUIRE(PrefabSerializer::WriteBytesAtomic(
        databaseSourceB, "healthy-database-b", error));
    REQUIRE(WriteSidecarId(AssetSidecarPath(databaseSourceB), kWrongAsset, error));

    AssetDatabase database;
    std::vector<AssetDatabaseDiagnostic> databaseDiagnostics;
    database.AddOrUpdate(
        AssetRecord{kAsset, databaseSourceA.string(), {},
                    AssetIdentityAuthority::Sidecar, {}, {}, {}},
        databaseDiagnostics);
    database.AddOrUpdate(
        AssetRecord{kWrongAsset, databaseSourceB.string(), {},
                    AssetIdentityAuthority::Sidecar, {}, {}, {}},
        databaseDiagnostics);
    const AssetResolutionContext assets{temp.directory, &database};

    // The scene retains an existing authored spelling that differs from the
    // healthy DB claimant.  Selection must match the DB path event while
    // leaving the authored spelling untouched for Capture to resolve.
    SceneDocument watcherDocument;
    const auto watcherRoot = watcherDocument.ecs.registry.create();
    REQUIRE(watcherDocument.AssignKnownUuid(watcherRoot, kRootA));
    watcherDocument.ecs.registry.emplace<PrefabInstanceComponent>(watcherRoot,
        PrefabInstanceComponent{AssetReference{
            AssetKind::Prefab, authored.string(), {}, {}, kAsset}, kInstanceA});
    watcherDocument.ecs.registry.emplace<PrefabMemberComponent>(watcherRoot,
        PrefabMemberComponent{kInstanceA, kTemplateRoot, {}});
    std::vector<AssetDiagnostic> watcherDiagnostics;
    const auto watcher = CollectReferencedPrefabSources(
        watcherDocument, assets, {databaseSourceA}, false, watcherDiagnostics);
    REQUIRE(watcher.size() == 1);
    CHECK(watcher.front().path == authored.string());
    CHECK(watcher.front().assetId == kAsset);

    std::vector<std::filesystem::path> capturedReads;
    const auto reader = [&](const std::filesystem::path& path,
                            std::string& bytes, Error& readError) {
        capturedReads.push_back(CanonicalAssetPath(path));
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            readError = {Error::Io, path.string(), "identity authority test read failed"};
            return false;
        }
        bytes.assign(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
        return true;
    };
    const auto captured = CapturePrefabSource(watcher.front(), assets, reader);
    REQUIRE(captured.IsOk());
    REQUIRE(capturedReads.size() == 2);
    CHECK(capturedReads[0] == CanonicalAssetPath(databaseSourceA));
    CHECK(capturedReads[1] == CanonicalAssetPath(AssetSidecarPath(databaseSourceA)));
    CHECK(captured.value.fingerprint.normalizedPath ==
          CanonicalAssetPath(databaseSourceA));
    CHECK(captured.value.fingerprint.assetId == kAsset);

    const auto makeDocument = [&](const std::vector<AssetReference>& references) {
        SceneDocument document;
        const UUID roots[] = {kRootA, kRootB};
        const UUID instances[] = {kInstanceA, kInstanceB};
        for (std::size_t index = 0; index != references.size(); ++index)
        {
            const auto root = document.ecs.registry.create();
            REQUIRE(document.AssignKnownUuid(root, roots[index]));
            document.ecs.registry.emplace<PrefabInstanceComponent>(root,
                PrefabInstanceComponent{references[index], instances[index]});
            document.ecs.registry.emplace<PrefabMemberComponent>(root,
                PrefabMemberComponent{instances[index], kTemplateRoot, {}});
        }
        return document;
    };

    const auto runLoad = [&](SceneDocument& document,
                             std::vector<std::filesystem::path>& reads,
                             std::size_t expectedCaptures) {
        std::size_t captures = 0;
        PrefabPropagationLoadHooks hooks;
        hooks.capture = [&](const AssetReference& reference,
                            const AssetResolutionContext& context) {
            ++captures;
            return CapturePrefabSource(reference, context,
                [&](const std::filesystem::path& path, std::string& bytes,
                    Error& readError) {
                    reads.push_back(CanonicalAssetPath(path));
                    std::ifstream input(path, std::ios::binary);
                    if (!input)
                    {
                        readError = {Error::Io, path.string(),
                                     "identity load read failed"};
                        return false;
                    }
                    bytes.assign(std::istreambuf_iterator<char>(input),
                                 std::istreambuf_iterator<char>());
                    return true;
                });
        };
        hooks.prepare = [](const PrefabPropagationDiscoveryRequest& request) {
            DiscoveredPropagationPlan plan;
            plan.source = request.capturedSource.fingerprint;
            plan.capturedSource = request.capturedSource;
            plan.documentGeneration = request.documentGeneration;
            plan.resourceGeneration = request.resourceGeneration;
            plan.documentGenerationCaptured = request.documentGenerationCaptured;
            plan.resourceGenerationCaptured = request.resourceGenerationCaptured;
            return Result<DiscoveredPropagationPlan>::Ok(std::move(plan));
        };
        const auto report = ReconcilePrefabPropagationForLoad(document, assets, hooks);
        REQUIRE(report.IsOk());
        CHECK(captures == expectedCaptures);
    };

    const AssetReference aliasA{AssetKind::Prefab,
        (temp.directory / "alias-z.rt2prefab").string(), {}, {}, kAsset};
    const AssetReference aliasB{AssetKind::Prefab,
        (temp.directory / "alias-a.rt2prefab").string(), {}, {}, kAsset};
    auto aliasFirst = makeDocument({aliasA, aliasB});
    auto aliasSecond = makeDocument({aliasB, aliasA});
    std::vector<std::filesystem::path> aliasReadsFirst;
    std::vector<std::filesystem::path> aliasReadsSecond;
    runLoad(aliasFirst, aliasReadsFirst, 1);
    runLoad(aliasSecond, aliasReadsSecond, 1);
    REQUIRE(aliasReadsFirst.size() == 2);
    REQUIRE(aliasReadsSecond.size() == 2);
    CHECK(aliasReadsFirst == aliasReadsSecond);
    CHECK(aliasReadsFirst[0] == CanonicalAssetPath(databaseSourceA));

    // Two distinct durable IDs may retain the same authored spelling when
    // their healthy DB claimants are different.  Dedupe is by the validated
    // (canonical path, durable ID), so both sources are captured exactly once
    // in either registry order rather than falsely conflicting by spelling.
    const AssetReference sharedA{AssetKind::Prefab, "shared.rt2prefab",
                                {}, {}, kAsset};
    const AssetReference sharedB{AssetKind::Prefab, "shared.rt2prefab",
                                {}, {}, kWrongAsset};
    auto distinctFirst = makeDocument({sharedA, sharedB});
    auto distinctSecond = makeDocument({sharedB, sharedA});
    std::vector<std::filesystem::path> distinctReadsFirst;
    std::vector<std::filesystem::path> distinctReadsSecond;
    runLoad(distinctFirst, distinctReadsFirst, 2);
    runLoad(distinctSecond, distinctReadsSecond, 2);
    REQUIRE(distinctReadsFirst.size() == 4);
    REQUIRE(distinctReadsSecond.size() == 4);
    CHECK(distinctReadsFirst == distinctReadsSecond);
    CHECK(distinctReadsFirst[0] == CanonicalAssetPath(databaseSourceA));
    CHECK(distinctReadsFirst[2] == CanonicalAssetPath(databaseSourceB));
    // Named compiling RED/GREEN fault: restoring the former authored-path /
    // reference-ID arguments makes the DB-path watcher event select nothing,
    // aliases capture twice or choose by registry order, and shared spelling
    // falsely conflicts instead of selecting both healthy claimants.
}

TEST_CASE("Phase 8 W4 Ticket 2: load rejects same-path conflicting IDs before path-only dedupe")
{
    const auto temp = TempPrefab();
    SceneDocument document;
    const auto addRoot = [&](const UUID& entityId, const UUID& assetId,
                             const UUID& instanceId) {
        const auto root = document.ecs.registry.create();
        REQUIRE(document.AssignKnownUuid(root, entityId));
        document.ecs.registry.emplace<Hierarchy>(root);
        document.ecs.registry.emplace<PrefabInstanceComponent>(root,
            PrefabInstanceComponent{AssetReference{
                AssetKind::Prefab, "same.rt2prefab", {}, {}, assetId},
                instanceId});
        document.ecs.registry.emplace<PrefabMemberComponent>(root,
            PrefabMemberComponent{instanceId, kTemplateRoot, {}});
    };
    addRoot(kRootA, kAsset, kInstanceA);
    addRoot(kRootB, kWrongAsset, kInstanceB);
    const auto result = ReconcilePrefabPropagationForLoad(
        document, AssetResolutionContext{temp.directory, nullptr});
    CHECK_FALSE(result.IsOk());
    CHECK(result.error.detail.find("Conflict:") == 0);
    // Named RED/GREEN fault: replacing the durable (path, ID) key with path
    // alone would silently discard one root and make the result order-dependent.
}

TEST_CASE("Phase 8 W4 Ticket 2: authored fallback survives watcher and load identity dedupe")
{
    const auto temp = TempPrefab();
    const auto source = temp.source();
    const std::string authoredReference = "source.rt2prefab";
    Error error;
    REQUIRE(PrefabSerializer::WriteBytesAtomic(source, "ticket2-authored", error));
    REQUIRE(WriteSidecarId(AssetSidecarPath(source), kAsset, error));

    AssetDatabase staleDatabase;
    std::vector<AssetDatabaseDiagnostic> databaseDiagnostics;
    staleDatabase.AddOrUpdate(
        AssetRecord{kAsset, (temp.directory / "stale-a.rt2prefab").string(), {},
                    AssetIdentityAuthority::Reference, {}, {}, {}},
        databaseDiagnostics);

    SceneDocument document;
    const auto root = document.ecs.registry.create();
    REQUIRE(document.AssignKnownUuid(root, kRootA));
    document.ecs.registry.emplace<PrefabInstanceComponent>(root,
        PrefabInstanceComponent{AssetReference{
            AssetKind::Prefab, authoredReference, {}, {}, kAsset}, kInstanceA});
    document.ecs.registry.emplace<PrefabMemberComponent>(root,
        PrefabMemberComponent{kInstanceA, kTemplateRoot, {}});

    AssetResolutionContext assets{temp.directory, &staleDatabase};
    std::vector<AssetDiagnostic> watcherDiagnostics;
    const auto incremental = CollectReferencedPrefabSources(
        document, assets, {source}, false, watcherDiagnostics);
    REQUIRE(incremental.size() == 1);
    CHECK(incremental.front().path == authoredReference);
    const auto full = CollectReferencedPrefabSources(
        document, assets, {}, true, watcherDiagnostics);
    REQUIRE(full.size() == 1);
    CHECK(full.front().path == authoredReference);

    std::filesystem::path capturedLoadPath;
    PrefabPropagationLoadHooks hooks;
    hooks.capture = [&](const AssetReference& reference,
                        const AssetResolutionContext&) {
        capturedLoadPath = reference.path;
        CapturedPrefabSource captured;
        captured.fingerprint = {CanonicalAssetPath(source), kAsset, "load-digest"};
        captured.prefabBytes = "captured";
        captured.sidecarBytes = "sidecar";
        return Result<CapturedPrefabSource>::Ok(std::move(captured));
    };
    hooks.prepare = [](const PrefabPropagationDiscoveryRequest& request) {
        DiscoveredPropagationPlan plan;
        plan.source = request.capturedSource.fingerprint;
        plan.capturedSource = request.capturedSource;
        plan.documentGeneration = request.documentGeneration;
        plan.resourceGeneration = request.resourceGeneration;
        plan.documentGenerationCaptured = request.documentGenerationCaptured;
        plan.resourceGenerationCaptured = request.resourceGenerationCaptured;
        return Result<DiscoveredPropagationPlan>::Ok(std::move(plan));
    };
    const auto loaded = ReconcilePrefabPropagationForLoad(
        document, assets, hooks);
    REQUIRE(loaded.IsOk());
    CHECK(capturedLoadPath == authoredReference);

    staleDatabase.AddOrUpdate(
        AssetRecord{kWrongAsset, (temp.directory / "stale-b.rt2prefab").string(), {},
                    AssetIdentityAuthority::Reference, {}, {}, {}},
        databaseDiagnostics);
    const auto conflictingRoot = document.ecs.registry.create();
    REQUIRE(document.AssignKnownUuid(conflictingRoot, kRootB));
    document.ecs.registry.emplace<PrefabInstanceComponent>(conflictingRoot,
        PrefabInstanceComponent{AssetReference{
            AssetKind::Prefab, authoredReference, {}, {}, kWrongAsset}, kInstanceB});
    document.ecs.registry.emplace<PrefabMemberComponent>(conflictingRoot,
        PrefabMemberComponent{kInstanceB, kTemplateRoot, {}});
    const auto conflictingLoad = ReconcilePrefabPropagationForLoad(
        document, assets, hooks);
    CHECK_FALSE(conflictingLoad.IsOk());
    CHECK(conflictingLoad.error.detail.find("Conflict:") == 0);

    SceneDocument relative;
    const auto relativeRoot = relative.ecs.registry.create();
    REQUIRE(relative.AssignKnownUuid(relativeRoot, kRootB));
    relative.ecs.registry.emplace<PrefabInstanceComponent>(relativeRoot,
        PrefabInstanceComponent{AssetReference{
            AssetKind::Prefab, "relative.rt2prefab", {}, {}, kAsset}, kInstanceB});
    relative.ecs.registry.emplace<PrefabMemberComponent>(relativeRoot,
        PrefabMemberComponent{kInstanceB, kTemplateRoot, {}});
    std::vector<AssetDiagnostic> relativeDiagnostics;
    const auto relativeSources = CollectReferencedPrefabSources(
        relative, AssetResolutionContext{}, {"relative.rt2prefab"}, false,
        relativeDiagnostics);
    CHECK(relativeSources.empty());
    CHECK_FALSE(relativeDiagnostics.empty());
    const auto relativeLoad = ReconcilePrefabPropagationForLoad(
        relative, AssetResolutionContext{}, hooks);
    CHECK_FALSE(relativeLoad.IsOk());
    CHECK(relativeLoad.error.code == Error::InvalidArgument);
    // Named RED/GREEN faults: overwriting source.path with the stale DB path
    // breaks both watcher capture and load; path-only dedupe misses validated
    // authored identity conflicts; CanonicalAssetPath on a relative path with
    // no explicit root would incorrectly probe the process CWD.
}
}

TEST_CASE("Phase 8 W4 S2: identity discovery quarantines one sibling and loads once")
{
    const auto temp = TempPrefab();
    const auto source = temp.source();
    PrefabDocument prefab;
    prefab.entities = {
        // Deliberately serialized child-first: discovery keys by durable IDs,
        // never by record order or display names.
        {kTemplateChild, SubtreeEntityRecord{kRecordChild, "Root", kRecordRoot}},
        {kTemplateRoot, SubtreeEntityRecord{kRecordRoot, "Child"}}
    };
    Error error;
    REQUIRE(PrefabSerializer::Save(prefab, source, error));
    REQUIRE(WriteSidecarId(AssetSidecarPath(source), kAsset, error));

    SceneDocument document;
    document.metadata.schemaVersion = SceneSerializer::PrefabOverrideSchemaVersion;
    const auto rootA = document.ecs.registry.create();
    REQUIRE(document.AssignKnownUuid(rootA, kRootA));
    document.ecs.registry.emplace<Hierarchy>(rootA);
    PrefabInstanceComponent linkA;
    linkA.prefab = AssetReference{AssetKind::Prefab, "source.rt2prefab", {}, {}, kAsset};
    linkA.instanceId = kInstanceA;
    document.ecs.registry.emplace<PrefabInstanceComponent>(rootA, linkA);
    document.ecs.registry.emplace<PrefabMemberComponent>(
        rootA, PrefabMemberComponent{kInstanceA, kTemplateRoot, {}});
    AddEntity(document, kChildA, kInstanceA, kTemplateChild, rootA);

    const auto rootB = document.ecs.registry.create();
    REQUIRE(document.AssignKnownUuid(rootB, kRootB));
    document.ecs.registry.emplace<Hierarchy>(rootB);
    PrefabInstanceComponent linkB = linkA;
    linkB.instanceId = kInstanceB;
    document.ecs.registry.emplace<PrefabInstanceComponent>(rootB, linkB);
    document.ecs.registry.emplace<PrefabMemberComponent>(
        rootB, PrefabMemberComponent{kInstanceB, kTemplateRoot, {}});
    AddEntity(document, kChildB, kInstanceB, kTemplateChild, rootB);

    const auto rootBad = document.ecs.registry.create();
    REQUIRE(document.AssignKnownUuid(rootBad, kRootBad));
    document.ecs.registry.emplace<Hierarchy>(rootBad);
    PrefabInstanceComponent linkBad = linkA;
    linkBad.instanceId = kInstanceBad;
    document.ecs.registry.emplace<PrefabInstanceComponent>(rootBad, linkBad);
    document.ecs.registry.emplace<PrefabMemberComponent>(
        rootBad, PrefabMemberComponent{kInstanceBad, kTemplateRoot, {}});

    SceneMaterial material;
    material.baseColor = {0.17f, 0.29f, 0.41f};
    material.emissiveColor = {0.03f, 0.05f, 0.07f};
    material.emissiveIntensity = 2.5f;
    material.sourceKey = "s2:material";
    document.ecs.materials.push_back(material);
    SceneTexture texture;
    texture.ref = AssetReference{AssetKind::Texture, "s2-texture.png", {},
                                 "s2:texture", kAsset};
    texture.width = 2;
    texture.height = 1;
    texture.channels = 4;
    texture.pixels = {1, 2, 3, 4, 5, 6, 7, 8};
    texture.isSRGB = true;
    document.ecs.textures.push_back(texture);
    MeshData mesh;
    mesh.name = "s2-mesh";
    mesh.vertices = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    mesh.indices = {0, 1, 2};
    mesh.normals = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    mesh.uvs = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    mesh.materialIndices = {0};
    document.ecs.meshRegistry.AddMesh(mesh);
    document.environment.ref = AssetReference{AssetKind::Environment,
                                                "s2-environment.exr", {},
                                                "s2:environment", kAsset};
    document.environment.width = 1;
    document.environment.height = 1;
    document.environment.floatPixels = {0.11f, 0.22f, 0.33f, 1.0f};
    document.ecs.camera.position = {3.0f, 4.0f, 5.0f};
    document.ecs.camera.verticalFOV = 37.0f;

    const auto beforeSchema = document.metadata.schemaVersion;
    const auto beforeDirty = document.metadata.dirty;
    const auto beforeEntityId = document.ecs.registry.storage<EntityIdComponent>().size();
    const auto beforeUuidIndex = document.uuidIndex.All();
    const auto beforeMaterials = document.ecs.materials;
    const auto beforeTextures = document.ecs.textures;
    const auto beforeMeshes = document.ecs.meshRegistry.GetMeshes();
    const auto beforeEnvironment = document.environment;
    const auto beforeCamera = document.ecs.camera;
    const auto beforeDocumentGeneration = std::uint64_t{71};
    const auto beforeResourceGeneration = std::uint64_t{19};
    std::vector<AssetDiagnostic> snapshotDiagnostics;
    Error snapshotError;
    const auto beforePath = temp.directory / "before.rt2scene";
    const auto afterPath = temp.directory / "after.rt2scene";
    REQUIRE(SceneSerializer::Save(document, beforePath, snapshotDiagnostics, snapshotError));
    const auto beforeSceneBytes = ReadTestBytes(beforePath);
    const auto beforeSourceBytes = ReadTestBytes(source);
    const auto beforeSidecarBytes = ReadTestBytes(AssetSidecarPath(source));
    auto request = Request(document, source);
    std::size_t loadCount = 0;
    request.parseBytes = [&](PrefabDocument& out, const std::string& bytes,
                             const std::filesystem::path& path,
                             Error& loadError) {
        ++loadCount;
        return PrefabSerializer::LoadBytes(out, bytes, path, loadError);
    };
    const auto prepared = PreparePrefabPropagation(request);
    if (!prepared.IsOk()) MESSAGE(prepared.error.Format());
    REQUIRE(prepared.IsOk());
    CHECK(loadCount == 1);
    CHECK(prepared.value.source.contentDigest ==
          prepared.value.capturedSource.fingerprint.contentDigest);
    CHECK(prepared.value.documentGeneration == 71);
    CHECK(prepared.value.resourceGeneration == 19);
    REQUIRE(prepared.value.instances.size() == 3);
    CHECK(prepared.value.instances.front().instanceId == kInstanceA);
    CHECK(prepared.value.source.assetId == kAsset);
    CHECK(!prepared.value.source.contentDigest.empty());
    std::size_t valid = 0;
    std::size_t quarantined = 0;
    for (const auto& instance : prepared.value.instances)
    {
        if (instance.disposition == PrefabPropagationInstanceDisposition::Propagate)
        {
            ++valid;
            CHECK(instance.affectedEntities.size() == 2);
        }
        if (instance.disposition == PrefabPropagationInstanceDisposition::Quarantined)
        {
            ++quarantined;
            REQUIRE(instance.diagnostics.size() == 1);
            CHECK(instance.diagnostics.front().instanceId == kInstanceBad);
        }
    }
    CHECK(valid == 2);
    CHECK(quarantined == 1);
    snapshotDiagnostics.clear();
    snapshotError = Error{};
    REQUIRE(SceneSerializer::Save(document, afterPath, snapshotDiagnostics, snapshotError));
    CHECK(document.metadata.schemaVersion == beforeSchema);
    CHECK(document.metadata.dirty == beforeDirty);
    CHECK(document.ecs.registry.storage<EntityIdComponent>().size() == beforeEntityId);
    CHECK(document.uuidIndex.All() == beforeUuidIndex);
    CHECK(document.ecs.materials.size() == beforeMaterials.size());
    for (std::size_t i = 0; i < beforeMaterials.size() &&
                            i < document.ecs.materials.size(); ++i)
        CHECK(SameMaterial(document.ecs.materials[i], beforeMaterials[i]));
    CHECK(document.ecs.textures.size() == beforeTextures.size());
    for (std::size_t i = 0; i < beforeTextures.size() &&
                            i < document.ecs.textures.size(); ++i)
        CHECK(SameTexture(document.ecs.textures[i], beforeTextures[i]));
    CHECK(document.ecs.meshRegistry.GetCount() == beforeMeshes.size());
    for (std::size_t i = 0; i < beforeMeshes.size() &&
                            i < document.ecs.meshRegistry.GetMeshes().size(); ++i)
        CHECK(SameMesh(document.ecs.meshRegistry.GetMesh(static_cast<uint32_t>(i)),
                       beforeMeshes[i]));
    CHECK(SameEnvironment(document.environment, beforeEnvironment));
    CHECK(SameCamera(document.ecs.camera, beforeCamera));
    CHECK(prepared.value.documentGeneration == beforeDocumentGeneration);
    CHECK(prepared.value.resourceGeneration == beforeResourceGeneration);
    CHECK(ReadTestBytes(afterPath) == beforeSceneBytes);
    CHECK(ReadTestBytes(source) == beforeSourceBytes);
    CHECK(ReadTestBytes(AssetSidecarPath(source)) == beforeSidecarBytes);
}

TEST_CASE("Phase 8 W4 S2: deterministic maps use template IDs and one fingerprint")
{
    const auto temp = TempPrefab();
    const auto source = temp.source();
    PrefabDocument prefab;
    prefab.entities = {
        {kTemplateChild, SubtreeEntityRecord{kRecordChild, "Child", kRecordRoot}},
        {kTemplateRoot, SubtreeEntityRecord{kRecordRoot, "Root"}}
    };
    Error error;
    REQUIRE(PrefabSerializer::Save(prefab, source, error));
    REQUIRE(WriteSidecarId(AssetSidecarPath(source), kAsset, error));

    auto populate = [&](SceneDocument& document, bool reverse) {
        const UUID rootUuid = reverse ? kRootB : kRootA;
        const UUID childUuid = reverse ? kChildB : kChildA;
        const UUID instance = reverse ? kInstanceB : kInstanceA;
        const auto root = document.ecs.registry.create();
        REQUIRE(document.AssignKnownUuid(root, rootUuid));
        document.ecs.registry.emplace<Hierarchy>(root);
        PrefabInstanceComponent link;
        link.prefab = AssetReference{AssetKind::Prefab, "source.rt2prefab", {}, {}, kAsset};
        link.instanceId = instance;
        document.ecs.registry.emplace<PrefabInstanceComponent>(root, link);
        document.ecs.registry.emplace<PrefabMemberComponent>(
            root, PrefabMemberComponent{instance, kTemplateRoot, {}});
        AddEntity(document, childUuid, instance, kTemplateChild, root);
    };

    SceneDocument first;
    SceneDocument second;
    populate(first, false);
    populate(second, true);
    auto firstRequest = Request(first, source);
    auto secondRequest = Request(second, source);
    const auto firstResult = PreparePrefabPropagation(firstRequest);
    const auto secondResult = PreparePrefabPropagation(secondRequest);
    REQUIRE(firstResult.IsOk());
    REQUIRE(secondResult.IsOk());
    REQUIRE(firstResult.value.instances.size() == 1);
    REQUIRE(secondResult.value.instances.size() == 1);
    CHECK(firstResult.value.instances.front().disposition ==
          PrefabPropagationInstanceDisposition::Propagate);
    CHECK(secondResult.value.instances.front().disposition ==
          PrefabPropagationInstanceDisposition::Propagate);
    CHECK(firstResult.value.instances.front().affectedEntities.size() == 2);
    CHECK(secondResult.value.instances.front().affectedEntities.size() == 2);
}

TEST_CASE("Phase 8 W4 S2: shuffled registry and source order produce identical plans")
{
    const auto temp = TempPrefab();
    const auto source = temp.source();
    Error error;
    PrefabDocument prefab;
    prefab.entities = {
        {kTemplateChild, SubtreeEntityRecord{kRecordChild, "Child", kRecordRoot}},
        {kTemplateRoot, SubtreeEntityRecord{kRecordRoot, "Root"}}
    };
    REQUIRE(PrefabSerializer::Save(prefab, source, error));
    REQUIRE(WriteSidecarId(AssetSidecarPath(source), kAsset, error));

    auto populate = [&](SceneDocument& document, bool reverse) {
        const UUID roots[] = {kRootA, kRootB, kRootBad};
        const UUID children[] = {kChildA, kChildB,
            UUID::Parse("33333333-3333-4333-8333-333333333334")};
        const UUID instances[] = {kInstanceA, kInstanceB, kInstanceBad};
        for (int n = 0; n != 3; ++n)
        {
            const int i = reverse ? 2 - n : n;
            const auto root = document.ecs.registry.create();
            REQUIRE(document.AssignKnownUuid(root, roots[i]));
            document.ecs.registry.emplace<Hierarchy>(root);
            PrefabInstanceComponent link;
            link.prefab = AssetReference{AssetKind::Prefab,
                                         reverse ? "./source.rt2prefab"
                                                 : "source.rt2prefab",
                                         {}, {}, kAsset};
            link.instanceId = instances[i];
            document.ecs.registry.emplace<PrefabInstanceComponent>(root, link);
            document.ecs.registry.emplace<PrefabMemberComponent>(
                root, PrefabMemberComponent{instances[i], kTemplateRoot, {}});
            AddEntity(document, children[i], instances[i], kTemplateChild, root);
        }
    };
    SceneDocument first;
    SceneDocument second;
    populate(first, false);
    populate(second, true);
    const auto firstResult = PreparePrefabPropagation(Request(first, source));
    const auto secondResult = PreparePrefabPropagation(Request(second, source));
    REQUIRE(firstResult.IsOk());
    REQUIRE(secondResult.IsOk());
    CHECK(firstResult.value == secondResult.value);
    REQUIRE(firstResult.value.instances.size() == 3);
    CHECK(firstResult.value.instances[0].instanceId == kInstanceA);
    CHECK(firstResult.value.instances[1].instanceId == kInstanceB);
    CHECK(firstResult.value.instances[2].instanceId == kInstanceBad);
}

TEST_CASE("Phase 8 W4 S2: topology and excluded override keys quarantine an instance")
{
    const auto temp = TempPrefab();
    const auto source = temp.source();
    PrefabDocument prefab;
    prefab.entities = {
        {kTemplateRoot, SubtreeEntityRecord{kTemplateRoot, "Root"}},
        {kTemplateChild, SubtreeEntityRecord{kTemplateChild, "Child", kTemplateRoot}}
    };
    Error error;
    REQUIRE(PrefabSerializer::Save(prefab, source, error));
    REQUIRE(WriteSidecarId(AssetSidecarPath(source), kAsset, error));

    SceneDocument document;
    document.metadata.schemaVersion = SceneSerializer::PrefabOverrideSchemaVersion;
    const auto root = document.ecs.registry.create();
    REQUIRE(document.AssignKnownUuid(root, kRootA));
    document.ecs.registry.emplace<Hierarchy>(root);
    PrefabInstanceComponent link;
    link.prefab = AssetReference{AssetKind::Prefab, "source.rt2prefab", {}, {}, kAsset};
    link.instanceId = kInstanceA;
    document.ecs.registry.emplace<PrefabInstanceComponent>(root, link);
    document.ecs.registry.emplace<PrefabMemberComponent>(
        root, PrefabMemberComponent{kInstanceA, kTemplateRoot,
                                     {PrefabComponentKeyFor<MeshRef>::value}});
    AddEntity(document, kChildA, kInstanceA, kTemplateChild, root);

    const auto result = PreparePrefabPropagation(Request(document, source));
    if (!result.IsOk()) MESSAGE(result.error.Format());
    REQUIRE(result.IsOk());
    REQUIRE(result.value.instances.size() == 1);
    CHECK(result.value.instances.front().disposition ==
          PrefabPropagationInstanceDisposition::Quarantined);
    REQUIRE(result.value.diagnostics.size() == 1);
    CHECK(result.value.diagnostics.front().reason.find("excluded") != std::string::npos);
}

TEST_CASE("Phase 8 W4 S2: durable identity rejects a stale database claim")
{
    const auto temp = TempPrefab();
    const auto source = temp.source();
    PrefabDocument prefab;
    prefab.entities = {{kTemplateRoot, SubtreeEntityRecord{kTemplateRoot, "Root"}}};
    Error error;
    REQUIRE(PrefabSerializer::Save(prefab, source, error));
    REQUIRE(WriteSidecarId(AssetSidecarPath(source), kAsset, error));

    AssetDatabase database;
    AssetRecord stale;
    stale.assetId = kWrongAsset;
    stale.sourcePath = source.string();
    stale.identityAuthority = AssetIdentityAuthority::Reference;
    std::vector<AssetDatabaseDiagnostic> databaseDiagnostics;
    database.AddOrUpdate(stale, databaseDiagnostics);

    SceneDocument document;
    const auto root = document.ecs.registry.create();
    REQUIRE(document.AssignKnownUuid(root, kRootA));
    document.ecs.registry.emplace<Hierarchy>(root);
    PrefabInstanceComponent link;
    link.prefab = AssetReference{AssetKind::Prefab, source.string(), {}, {}, kWrongAsset};
    link.instanceId = kInstanceA;
    document.ecs.registry.emplace<PrefabInstanceComponent>(root, link);
    document.ecs.registry.emplace<PrefabMemberComponent>(
        root, PrefabMemberComponent{kInstanceA, kTemplateRoot, {}});

    auto request = Request(document, source);
    request.assets.database = &database;
    const auto result = PreparePrefabPropagation(request);
    REQUIRE(result.IsOk());
    CHECK(result.value.instances.empty());
}

TEST_CASE("Phase 8 W4 S2: override admission follows scene schema representability")
{
    auto run = [&](std::uint32_t schema, PrefabComponentKey key) {
        const auto temp = TempPrefab();
        const auto source = temp.source();
        PrefabDocument prefab;
        prefab.entities = {{kTemplateRoot, SubtreeEntityRecord{kTemplateRoot, "Root"}}};
        Error error;
        REQUIRE(PrefabSerializer::Save(prefab, source, error));
        REQUIRE(WriteSidecarId(AssetSidecarPath(source), kAsset, error));
        SceneDocument document;
        document.metadata.schemaVersion = schema;
        const auto root = document.ecs.registry.create();
        REQUIRE(document.AssignKnownUuid(root, kRootA));
        document.ecs.registry.emplace<Hierarchy>(root);
        PrefabInstanceComponent link;
        link.prefab = AssetReference{AssetKind::Prefab, source.string(), {}, {}, kAsset};
        link.instanceId = kInstanceA;
        document.ecs.registry.emplace<PrefabInstanceComponent>(root, link);
        document.ecs.registry.emplace<PrefabMemberComponent>(
            root, PrefabMemberComponent{kInstanceA, kTemplateRoot, {key}});
        const auto result = PreparePrefabPropagation(Request(document, source));
        REQUIRE(result.IsOk());
        REQUIRE(result.value.instances.size() == 1);
        return result.value.instances.front().disposition;
    };

    CHECK(run(SceneSerializer::PrefabOverrideSchemaVersion,
              PrefabComponentKeyFor<VisibleComponent>::value) ==
          PrefabPropagationInstanceDisposition::Propagate);
    CHECK(run(SceneSerializer::PrefabOverrideSchemaVersion,
              PrefabComponentKeyFor<PrimitiveComponent>::value) ==
          PrefabPropagationInstanceDisposition::Quarantined);
    CHECK(run(SceneSerializer::PrefabOverrideSchemaVersion - 1,
              PrefabComponentKeyFor<VisibleComponent>::value) ==
          PrefabPropagationInstanceDisposition::Quarantined);
}

TEST_CASE("Phase 8 W4 S2: hierarchy mismatch quarantines the whole instance")
{
    const auto temp = TempPrefab();
    const auto source = temp.source();
    PrefabDocument prefab;
    prefab.entities = {
        {kTemplateRoot, SubtreeEntityRecord{kTemplateRoot, "Root"}},
        {kWrongAsset, SubtreeEntityRecord{kWrongAsset, "Middle", kTemplateRoot}},
        {kTemplateChild, SubtreeEntityRecord{kTemplateChild, "Child", kWrongAsset}}
    };
    Error error;
    REQUIRE(PrefabSerializer::Save(prefab, source, error));
    REQUIRE(WriteSidecarId(AssetSidecarPath(source), kAsset, error));
    SceneDocument document;
    const auto root = document.ecs.registry.create();
    REQUIRE(document.AssignKnownUuid(root, kRootA));
    document.ecs.registry.emplace<Hierarchy>(root);
    PrefabInstanceComponent link;
    link.prefab = AssetReference{AssetKind::Prefab, source.string(), {}, {}, kAsset};
    link.instanceId = kInstanceA;
    document.ecs.registry.emplace<PrefabInstanceComponent>(root, link);
    document.ecs.registry.emplace<PrefabMemberComponent>(
        root, PrefabMemberComponent{kInstanceA, kTemplateRoot, {}});
    AddEntity(document, kRootBad, kInstanceA, kWrongAsset, root);
    AddEntity(document, kChildA, kInstanceA, kTemplateChild, root);
    const auto result = PreparePrefabPropagation(Request(document, source));
    REQUIRE(result.IsOk());
    REQUIRE(result.value.instances.size() == 1);
    CHECK(result.value.instances.front().disposition ==
          PrefabPropagationInstanceDisposition::Quarantined);
    REQUIRE(result.value.diagnostics.size() == 1);
    CHECK(result.value.diagnostics.front().reason.find("hierarchy") != std::string::npos);
}

TEST_CASE("Phase 8 W4 S2: root and member durable UUID corruption quarantines")
{
    enum class Fault { RootMissing, RootNil, RootMisindexed,
                       MemberMissing, MemberNil, MemberMisindexed, MemberDuplicate };
    const Fault faults[] = {Fault::RootMissing, Fault::RootNil, Fault::RootMisindexed,
                            Fault::MemberMissing, Fault::MemberNil,
                            Fault::MemberMisindexed, Fault::MemberDuplicate};
    for (const Fault fault : faults)
    {
        const auto temp = TempPrefab();
        const auto source = temp.source();
        PrefabDocument prefab;
        prefab.entities = {{kTemplateRoot, SubtreeEntityRecord{kTemplateRoot, "Root"}}};
        Error error;
        REQUIRE(PrefabSerializer::Save(prefab, source, error));
        REQUIRE(WriteSidecarId(AssetSidecarPath(source), kAsset, error));
        SceneDocument document;
        const auto root = document.ecs.registry.create();
        if (fault != Fault::RootMissing)
            REQUIRE(document.AssignKnownUuid(root, kRootA));
        document.ecs.registry.emplace<Hierarchy>(root);
        PrefabInstanceComponent link;
        link.prefab = AssetReference{AssetKind::Prefab, source.string(), {}, {}, kAsset};
        link.instanceId = kInstanceA;
        document.ecs.registry.emplace<PrefabInstanceComponent>(root, link);
        document.ecs.registry.emplace<PrefabMemberComponent>(
            root, PrefabMemberComponent{kInstanceA, kTemplateRoot, {}});
        if (fault == Fault::RootNil)
            document.ecs.registry.get<EntityIdComponent>(root).id = UUID::Nil();
        if (fault == Fault::RootMisindexed)
            document.ecs.registry.get<EntityIdComponent>(root).id = kRootB;
        if (fault == Fault::MemberMissing || fault == Fault::MemberNil ||
            fault == Fault::MemberMisindexed || fault == Fault::MemberDuplicate)
        {
            const auto member = document.ecs.registry.create();
            if (fault != Fault::MemberMissing)
                REQUIRE(document.AssignKnownUuid(member, kChildA));
            document.ecs.registry.emplace<PrefabMemberComponent>(
                member, PrefabMemberComponent{kInstanceA, kTemplateChild, {}});
            if (fault == Fault::MemberNil)
                document.ecs.registry.get<EntityIdComponent>(member).id = UUID::Nil();
            if (fault == Fault::MemberMisindexed)
                document.ecs.registry.get<EntityIdComponent>(member).id = kRootB;
            if (fault == Fault::MemberDuplicate)
            {
                document.ecs.registry.get<EntityIdComponent>(member).id = kRootA;
            }
        }
        const auto result = PreparePrefabPropagation(Request(document, source));
        CAPTURE(static_cast<int>(fault));
        REQUIRE(result.IsOk());
        REQUIRE(result.value.instances.size() == 1);
        CHECK(result.value.instances.front().disposition ==
              PrefabPropagationInstanceDisposition::Quarantined);
        CHECK(result.value.diagnostics.size() == 1);
    }
}

TEST_CASE("Phase 8 W4 S2: duplicate roots and unidentifiable members quarantine")
{
    const auto temp = TempPrefab();
    const auto source = temp.source();
    PrefabDocument prefab;
    prefab.entities = {{kTemplateRoot, SubtreeEntityRecord{kTemplateRoot, "Root"}}};
    Error error;
    REQUIRE(PrefabSerializer::Save(prefab, source, error));
    REQUIRE(WriteSidecarId(AssetSidecarPath(source), kAsset, error));

    SceneDocument document;
    auto addRoot = [&](const UUID& rootUuid) {
        const auto root = document.ecs.registry.create();
        REQUIRE(document.AssignKnownUuid(root, rootUuid));
        document.ecs.registry.emplace<Hierarchy>(root);
        PrefabInstanceComponent link;
        link.prefab = AssetReference{AssetKind::Prefab, source.string(), {}, {}, kAsset};
        link.instanceId = kInstanceA;
        document.ecs.registry.emplace<PrefabInstanceComponent>(root, link);
        document.ecs.registry.emplace<PrefabMemberComponent>(
            root, PrefabMemberComponent{kInstanceA, kTemplateRoot, {}});
    };
    addRoot(kRootA);
    addRoot(kRootB);
    const auto malformed = document.ecs.registry.create();
    document.ecs.registry.emplace<PrefabMemberComponent>(
        malformed, PrefabMemberComponent{kInstanceA, kTemplateChild, {}});

    const auto result = PreparePrefabPropagation(Request(document, source));
    REQUIRE(result.IsOk());
    REQUIRE(result.value.instances.size() == 2);
    CHECK(result.value.diagnostics.size() == 2);
    for (const auto& instance : result.value.instances)
        CHECK(instance.disposition == PrefabPropagationInstanceDisposition::Quarantined);
}

TEST_CASE("Phase 8 W4 S2: structural validation rejects each malformed member shape")
{
    enum class Fault { NilInstance, MismatchedInstance, NilTemplate, DuplicateTemplate,
                       MissingTemplate, ExtraTemplate, WrongRoot, AddedChild, Orphan,
                       HierarchyMismatch, UnknownKey, ExcludedKey, NonCanonicalKey };
    const Fault faults[] = {
        Fault::NilInstance, Fault::MismatchedInstance, Fault::NilTemplate,
        Fault::DuplicateTemplate, Fault::MissingTemplate, Fault::ExtraTemplate,
        Fault::WrongRoot, Fault::AddedChild, Fault::Orphan, Fault::HierarchyMismatch,
        Fault::UnknownKey, Fault::ExcludedKey, Fault::NonCanonicalKey };
    for (const Fault fault : faults)
    {
        const auto temp = TempPrefab();
        const auto source = temp.source();
        PrefabDocument prefab;
        prefab.entities = {
            {kTemplateRoot, SubtreeEntityRecord{kTemplateRoot, "Root"}},
            {kTemplateChild, SubtreeEntityRecord{kTemplateChild, "Child", kTemplateRoot}}
        };
        Error error;
        REQUIRE(PrefabSerializer::Save(prefab, source, error));
        REQUIRE(WriteSidecarId(AssetSidecarPath(source), kAsset, error));
        SceneDocument document;
        document.metadata.schemaVersion = SceneSerializer::PrefabOverrideSchemaVersion;
        const auto root = document.ecs.registry.create();
        REQUIRE(document.AssignKnownUuid(root, kRootA));
        document.ecs.registry.emplace<Hierarchy>(root);
        PrefabInstanceComponent link;
        link.prefab = AssetReference{AssetKind::Prefab, source.string(), {}, {}, kAsset};
        link.instanceId = fault == Fault::NilInstance ? UUID::Nil() : kInstanceA;
        document.ecs.registry.emplace<PrefabInstanceComponent>(root, link);
        std::vector<PrefabComponentKey> overrides;
        if (fault == Fault::UnknownKey)
            overrides = {PrefabComponentKey(std::string_view("unknown"), true)};
        if (fault == Fault::ExcludedKey)
            overrides = {PrefabComponentKeyFor<MeshRef>::value};
        if (fault == Fault::NonCanonicalKey)
            overrides = {PrefabComponentKeyFor<ScriptComponent>::value,
                         PrefabComponentKeyFor<NameComponent>::value};
        const UUID rootTemplate = fault == Fault::WrongRoot ? kTemplateChild : kTemplateRoot;
        const UUID rootMemberInstance = fault == Fault::MismatchedInstance
            ? kInstanceB : kInstanceA;
        document.ecs.registry.emplace<PrefabMemberComponent>(
            root, PrefabMemberComponent{rootMemberInstance,
                                        fault == Fault::NilTemplate ? UUID::Nil() : rootTemplate,
                                        overrides});
        if (fault != Fault::MissingTemplate)
        {
            const UUID childTemplate = fault == Fault::DuplicateTemplate
                ? rootTemplate
                : (fault == Fault::ExtraTemplate ? kWrongAsset : kTemplateChild);
            AddEntity(document, kChildA, kInstanceA, childTemplate, root);
        }
        if (fault == Fault::Orphan)
            document.ecs.registry.get<Hierarchy>(root).children.clear();
        if (fault == Fault::HierarchyMismatch)
        {
            const auto child = entt::entity{1};
            if (document.ecs.registry.valid(child) &&
                document.ecs.registry.all_of<Hierarchy>(child))
                document.ecs.registry.get<Hierarchy>(child).parent = entt::null;
        }
        if (fault == Fault::AddedChild)
        {
            const auto extra = document.ecs.registry.create();
            REQUIRE(document.AssignKnownUuid(extra, UUID::Parse(
                "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee")));
            document.ecs.registry.emplace<Hierarchy>(extra, root);
            document.ecs.registry.get<Hierarchy>(root).children.push_back(extra);
        }
        const auto result = PreparePrefabPropagation(Request(document, source));
        CAPTURE(static_cast<int>(fault));
        REQUIRE(result.IsOk());
        REQUIRE(result.value.instances.size() == 1);
        CHECK(result.value.instances.front().disposition ==
              PrefabPropagationInstanceDisposition::Quarantined);
        CHECK(result.value.diagnostics.size() == 1);
    }
}

TEST_CASE("Phase 8 W4 S2: global source and sidecar failures are loud and transactional")
{
    const auto temp = TempPrefab();
    const auto source = temp.source();
    PrefabDocument prefab;
    prefab.entities = {{kTemplateRoot, SubtreeEntityRecord{kTemplateRoot, "Root"}}};
    Error error;
    REQUIRE(PrefabSerializer::Save(prefab, source, error));

    SceneDocument document;
    const auto root = document.ecs.registry.create();
    REQUIRE(document.AssignKnownUuid(root, kRootA));
    document.ecs.registry.emplace<Hierarchy>(root);
    PrefabInstanceComponent link;
    link.prefab = AssetReference{AssetKind::Prefab, source.string(), {}, {}, kAsset};
    link.instanceId = kInstanceA;
    document.ecs.registry.emplace<PrefabInstanceComponent>(root, link);
    document.ecs.registry.emplace<PrefabMemberComponent>(
        root, PrefabMemberComponent{kInstanceA, kTemplateRoot, {}});

    // No sidecar: a durable identity cannot be guessed during read-only
    // preparation.
    CHECK_FALSE(PreparePrefabPropagation(Request(document, source)).IsOk());

    REQUIRE(WriteSidecarId(AssetSidecarPath(source), kAsset, error));
    auto parseRequest = Request(document, source);
    parseRequest.parseBytes = [](PrefabDocument&, const std::string&,
                                 const std::filesystem::path& path,
                                 Error& loadError) {
        loadError = {Error::Parse, path.string(), "named checked source parse failure"};
        return false;
    };
    CHECK_FALSE(PreparePrefabPropagation(parseRequest).IsOk());

    auto duplicateRequest = Request(document, source);
    duplicateRequest.parseBytes = [](PrefabDocument& out, const std::string&,
                                     const std::filesystem::path&,
                                     Error&) {
        out.entities = {
            {kTemplateRoot, SubtreeEntityRecord{kTemplateRoot, "A"}},
            {kTemplateRoot, SubtreeEntityRecord{kTemplateChild, "B"}}
        };
        return true;
    };
        CHECK_FALSE(PreparePrefabPropagation(duplicateRequest).IsOk());

    auto duplicateRecordRequest = Request(document, source);
    duplicateRecordRequest.parseBytes = [](PrefabDocument& out, const std::string&,
                                           const std::filesystem::path&,
                                           Error&) {
        out.entities = {
            {kTemplateRoot, SubtreeEntityRecord{kRecordRoot, "A"}},
            {kTemplateChild, SubtreeEntityRecord{kRecordRoot, "B"}}
        };
        return true;
    };
    CHECK_FALSE(PreparePrefabPropagation(duplicateRecordRequest).IsOk());

    REQUIRE(WriteSidecarId(AssetSidecarPath(source), kWrongAsset, error));
    CHECK_FALSE(PreparePrefabPropagation(Request(document, source)).IsOk());
}

TEST_CASE("Phase 8 W4 S2: malformed changed source fails with zero dependents")
{
    const auto temp = TempPrefab();
    const auto source = temp.source();
    Error error;
    REQUIRE(PrefabSerializer::WriteBytesAtomic(source, "{ malformed", error));
    REQUIRE(WriteSidecarId(AssetSidecarPath(source), kAsset, error));
    SceneDocument document;
    auto request = Request(document, source);
    const auto result = PreparePrefabPropagation(request);
    CHECK_FALSE(result.IsOk());
    CHECK(result.error.code == Error::Parse);
}

TEST_CASE("Phase 8 W4 S2: parser consumes the fingerprinted immutable snapshot")
{
    const auto temp = TempPrefab();
    const auto source = temp.source();
    PrefabDocument prefab;
    prefab.entities = {{kTemplateRoot, SubtreeEntityRecord{kTemplateRoot, "Root"}}};
    Error error;
    REQUIRE(PrefabSerializer::Save(prefab, source, error));
    REQUIRE(WriteSidecarId(AssetSidecarPath(source), kAsset, error));
    SceneDocument document;
    const auto root = document.ecs.registry.create();
    REQUIRE(document.AssignKnownUuid(root, kRootA));
    document.ecs.registry.emplace<Hierarchy>(root);
    PrefabInstanceComponent link;
    link.prefab = AssetReference{AssetKind::Prefab, source.string(), {}, {}, kAsset};
    link.instanceId = kInstanceA;
    document.ecs.registry.emplace<PrefabInstanceComponent>(root, link);
    document.ecs.registry.emplace<PrefabMemberComponent>(
        root, PrefabMemberComponent{kInstanceA, kTemplateRoot, {}});
    auto request = Request(document, source);
    const auto captured = CapturePrefabSource(request.changedSource, request.assets);
    REQUIRE(captured.IsOk());
    request.capturedSource = captured.value;
    PrefabDocument replacement;
    replacement.entities = {{kTemplateRoot, SubtreeEntityRecord{kTemplateRoot, "Changed"}},
                            {kTemplateChild, SubtreeEntityRecord{kTemplateChild, "Extra"}}};
    REQUIRE(PrefabSerializer::Save(replacement, source, error));
    const auto result = PreparePrefabPropagation(request);
    REQUIRE(result.IsOk());
    REQUIRE(result.value.instances.size() == 1);
    CHECK(result.value.instances.front().disposition ==
          PrefabPropagationInstanceDisposition::Propagate);
    CHECK(result.value.source.contentDigest == captured.value.fingerprint.contentDigest);
    CHECK(result.value.instances.front().affectedEntities.size() == 1);
}

TEST_CASE("Phase 8 W4 S2: captured source is read once and Prepare consumes it")
{
    const auto temp = TempPrefab();
    const auto source = temp.source();
    PrefabDocument prefab;
    prefab.entities = {{kTemplateRoot, SubtreeEntityRecord{kTemplateRoot, "Root"}}};
    Error error;
    REQUIRE(PrefabSerializer::Save(prefab, source, error));
    REQUIRE(WriteSidecarId(AssetSidecarPath(source), kAsset, error));

    SceneDocument document;
    const auto root = document.ecs.registry.create();
    REQUIRE(document.AssignKnownUuid(root, kRootA));
    document.ecs.registry.emplace<Hierarchy>(root);
    document.ecs.registry.emplace<PrefabInstanceComponent>(root,
        PrefabInstanceComponent{
            AssetReference{AssetKind::Prefab, source.string(), {}, {}, kAsset},
            kInstanceA});
    document.ecs.registry.emplace<PrefabMemberComponent>(root,
        PrefabMemberComponent{kInstanceA, kTemplateRoot, {}});

    std::size_t captureReads = 0;
    const auto captured = CapturePrefabSource(
        AssetReference{AssetKind::Prefab, source.string(), {}, {}, kAsset},
        AssetResolutionContext{source.parent_path(), nullptr},
        [&](const std::filesystem::path& path, std::string& bytes,
            Error& readError) {
            ++captureReads;
            std::ifstream input(path, std::ios::binary);
            if (!input)
            {
                readError = {Error::Io, path.string(), "capture read failed"};
                return false;
            }
            bytes.assign(std::istreambuf_iterator<char>(input),
                         std::istreambuf_iterator<char>());
            return true;
        });
    REQUIRE(captured.IsOk());
    REQUIRE(captureReads == 2);

    auto request = Request(document, source);
    request.capturedSource = captured.value;
    const auto prepared = PreparePrefabPropagation(request);
    REQUIRE(prepared.IsOk());
    CHECK(prepared.value.source == captured.value.fingerprint);
    CHECK(prepared.value.capturedSource.prefabBytes == captured.value.prefabBytes);
    CHECK(prepared.value.capturedSource.sidecarBytes == captured.value.sidecarBytes);
}
