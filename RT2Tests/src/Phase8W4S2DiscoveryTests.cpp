#include <doctest/doctest.h>

#include "AssetIdentity.h"
#include "PrefabPropagationDiscovery.h"

#include <filesystem>
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

std::filesystem::path TempPrefab()
{
    const auto dir = std::filesystem::temp_directory_path() / "rt2_w4_s2_discovery";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir / "source.rt2prefab";
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
    return request;
}
}

TEST_CASE("Phase 8 W4 S2: identity discovery quarantines one sibling and loads once")
{
    const auto source = TempPrefab();
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

    const auto beforeSchema = document.metadata.schemaVersion;
    const auto beforeDirty = document.metadata.dirty;
    const auto beforeSize = document.ecs.registry.storage<EntityIdComponent>().size();
    auto request = Request(document, source);
    std::size_t loadCount = 0;
    std::size_t fingerprintCount = 0;
    request.load = [&](PrefabDocument& out, const std::filesystem::path& path,
                       Error& loadError) {
        ++loadCount;
         return PrefabSerializer::Load(out, path, loadError);
     };
    request.fingerprint = [&](const std::string& bytes, const UUID& sidecar) {
        ++fingerprintCount;
        return std::string("batch-fingerprint-") +
               std::to_string(bytes.size()) + "-" + sidecar.ToString();
    };
    const auto prepared = PreparePrefabPropagation(request);
    if (!prepared.IsOk()) MESSAGE(prepared.error.Format());
    REQUIRE(prepared.IsOk());
    CHECK(loadCount == 1);
    CHECK(fingerprintCount == 1);
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
    CHECK(document.metadata.schemaVersion == beforeSchema);
    CHECK(document.metadata.dirty == beforeDirty);
    CHECK(document.ecs.registry.storage<EntityIdComponent>().size() == beforeSize);
}

TEST_CASE("Phase 8 W4 S2: deterministic maps use template IDs and one fingerprint")
{
    const auto source = TempPrefab();
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
    std::size_t firstLoads = 0;
    std::size_t secondLoads = 0;
    std::size_t firstFingerprints = 0;
    std::size_t secondFingerprints = 0;
    firstRequest.load = [&](PrefabDocument& out, const std::filesystem::path& path,
                            Error& loadError) {
        ++firstLoads;
        return PrefabSerializer::Load(out, path, loadError);
    };
    secondRequest.load = [&](PrefabDocument& out, const std::filesystem::path& path,
                             Error& loadError) {
        ++secondLoads;
        return PrefabSerializer::Load(out, path, loadError);
    };
    firstRequest.fingerprint = [&](const std::string& bytes, const UUID& sidecar) {
        ++firstFingerprints;
        return std::to_string(bytes.size()) + sidecar.ToString();
    };
    secondRequest.fingerprint = [&](const std::string& bytes, const UUID& sidecar) {
        ++secondFingerprints;
        return std::to_string(bytes.size()) + sidecar.ToString();
    };
    const auto firstResult = PreparePrefabPropagation(firstRequest);
    const auto secondResult = PreparePrefabPropagation(secondRequest);
    REQUIRE(firstResult.IsOk());
    REQUIRE(secondResult.IsOk());
    CHECK(firstLoads == 1);
    CHECK(secondLoads == 1);
    CHECK(firstFingerprints == 1);
    CHECK(secondFingerprints == 1);
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
    const auto source = TempPrefab();
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
    const auto source = TempPrefab();
    PrefabDocument prefab;
    prefab.entities = {
        {kTemplateRoot, SubtreeEntityRecord{kTemplateRoot, "Root"}},
        {kTemplateChild, SubtreeEntityRecord{kTemplateChild, "Child", kTemplateRoot}}
    };
    Error error;
    REQUIRE(PrefabSerializer::Save(prefab, source, error));
    REQUIRE(WriteSidecarId(AssetSidecarPath(source), kAsset, error));

    SceneDocument document;
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
    const auto source = TempPrefab();
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

TEST_CASE("Phase 8 W4 S2: hierarchy mismatch quarantines the whole instance")
{
    const auto source = TempPrefab();
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

TEST_CASE("Phase 8 W4 S2: duplicate roots and unidentifiable members quarantine")
{
    const auto source = TempPrefab();
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
        const auto source = TempPrefab();
        PrefabDocument prefab;
        prefab.entities = {
            {kTemplateRoot, SubtreeEntityRecord{kTemplateRoot, "Root"}},
            {kTemplateChild, SubtreeEntityRecord{kTemplateChild, "Child", kTemplateRoot}}
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
    const auto source = TempPrefab();
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
    parseRequest.load = [](PrefabDocument&, const std::filesystem::path& path,
                           Error& loadError) {
        loadError = {Error::Parse, path.string(), "named checked source parse failure"};
        return false;
    };
    CHECK_FALSE(PreparePrefabPropagation(parseRequest).IsOk());

    auto duplicateRequest = Request(document, source);
    duplicateRequest.load = [](PrefabDocument& out, const std::filesystem::path&,
                               Error&) {
        out.entities = {
            {kTemplateRoot, SubtreeEntityRecord{kTemplateRoot, "A"}},
            {kTemplateRoot, SubtreeEntityRecord{kTemplateChild, "B"}}
        };
        return true;
    };
        CHECK_FALSE(PreparePrefabPropagation(duplicateRequest).IsOk());
    
    auto duplicateRecordRequest = Request(document, source);
    duplicateRecordRequest.load = [](PrefabDocument& out, const std::filesystem::path&,
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
