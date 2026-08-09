#include <doctest/doctest.h>

#include "PersistedComponents.h"
#include "PrefabComponentKey.h"
#include "PrefabSerializer.h"
#include "SceneManager.h"
#include "SceneHierarchy.h"
#include "SceneSerializer.h"
#include "SceneSerializerTestSupport.h"
#include "SubtreeSnapshot.h"
#include "core/Error.h"
#include "core/UUID.h"
#include "json.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace rt2::core;

// ============================================================================
// Phase 8 W3, S1 — the prefab component classification table
// (implementation spec, docs/game-engine-development-plan.md "Phase 8 W3 —
// overrides", D3/D4 and Work step S1).
//
// S1 is a standalone header (RT2App/src/PrefabComponentKey.h) plus these
// tests; it has no callers yet. The header must stay in lock-step with
// PersistedComponents::ForEach (PersistedComponents.h:18-38): the whole point
// is that the table cannot silently drift away from the set of persisted
// components, because a drifted table attaches overrides to the wrong
// component.
//
// Each runtime test carries a discrimination proof (inject a named fault,
// confirm red, revert, confirm green) recorded in the verification report.
// Faults are described in the header comment of each test. The two required
// compile-time assertions live in the header itself (table size ==
// PersistedComponents::Count, overridable count == 8) and are recorded rather
// than run; each one's discriminating fault is documented in the report.
// ============================================================================

namespace
{

// Drive a real PersistedComponents::ForEach visitor and check, at each visited
// position, that the type's PrefabComponentKeyFor specialization agrees with
// the frozen table entry at that same position. Returns the number of visited
// components.
std::size_t ForEachTableAgreement(std::size_t& mismatches)
{
    std::size_t index = 0;
    mismatches = 0;
    PersistedComponents::ForEach([&](auto tag) {
        using T = typename decltype(tag)::Type;
        if (index >= kPrefabTable.size())
        {
            ++mismatches; // ForEach visits more components than the table has
            ++index;
            return;
        }
        const auto specialized = PrefabComponentKeyFor<T>::value;
        if (!(specialized == kPrefabTable[index])) ++mismatches;
        if (!specialized.valid()) ++mismatches;
        ++index;
    });
    return index;
}

} // namespace


// ============================================================================
// Phase 8 W3, S2 — the override field, the scene codec at v6, and the
// metadata.schemaVersion upgrade rule (W3-D2/D6, Work step S2).
//
// S2 is the data model + codec half of overrides, with no marking API yet
// (that is S5/S6). It builds on S1's PrefabComponentKey table.
//
// S2's tests (spec tests 1, 2, 3, 4, 6):
//   1. the override set round-trips save/load/save byte-identically AND
//      asserts the vector's contents (not merely that scene JSON round-trips);
//   2. a v5 scene loads with every override set empty;
//   3. the critical one: v5 load -> add an override -> recovery-path SaveTo ->
//      the output is v6 AND the override survives. The fault is removal of the
//      metadata.schemaVersion upgrade;
//   4. an unknown component key raises an observable diagnostic and leaves the
//      destination transactional;
//   6. a prefab record carrying every material/resource shape contains no
//      scene-side link or override data (the prefab file format must never
//      carry the override set).
// Phase 5 (both compile-time assertions) is recorded in the report and
// carried by the static_asserts in PrefabComponentKey.h.
//
// The string_view hazard: PrefabComponentKey::wire() is a string_view. Every
// key in kPrefabTable points into static constexpr storage, so keys obtained
// from FindComponentByWire / PrefabComponentKeyFor<T> are safe to store
// indefinitely. The override vectors are therefore ONLY ever populated with
// table-resolved keys — never with a key constructed from a transient parser
// buffer (which would dangle once the buffer dies, very likely passing every
// test before crashing much later in an unrelated place). Tests here set the
// vectors directly (S2 has no marking API) but always from PrefabComponentKeyFor
// values, never from raw string literals through the key constructor.
// ============================================================================

namespace
{

std::filesystem::path S2UniqueTempDir(const std::string& tag)
{
    const auto dir = std::filesystem::temp_directory_path() / ("rt2_" + tag);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void S2WriteRaw(const std::filesystem::path& p, const std::string& content)
{
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
}

std::string S2ReadFile(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct S2Fixture
{
    DeterministicUuidProvider ids;
    SceneManager manager;

    S2Fixture()
    {
        manager.SetUuidProvider(&ids);
        manager.AddMaterial(SceneMaterial{});
    }

    UUID CreateEmpty(const char* name)
    {
        return manager.CreateEmpty(name).affectedEntities.front();
    }

    UUID CreateChild(const char* name, const UUID& parent)
    {
        return manager.CreateEmpty(name, parent).affectedEntities.front();
    }

    // Create a real prefab from a root+child, instantiate it once, and return
    // the instance's two member handles (root first). The scene ends up with
    // genuine PrefabMemberComponent values (instanceId + templateId).
    std::pair<entt::entity, entt::entity> MakeInstance(
        const std::filesystem::path& dir)
    {
        const auto root = CreateEmpty("Root");
        const auto child = CreateChild("Child", root);
        const auto prefabPath = dir / "s2.rt2prefab";
        REQUIRE(manager.CreatePrefabFromSubtree({ root }, prefabPath).ok);

        std::vector<AssetDiagnostic> diags;
        const auto uuids = manager.ReserveKnownUuids(2);
        const auto inst =
            manager.InstantiatePrefabWithUuids(prefabPath, uuids, diags);
        REQUIRE(inst.mutation.success);

        auto& reg = manager.GetECS().registry;
        const auto rootHandle = manager.FindEntityByUuid(uuids[0]);
        const auto childHandle = manager.FindEntityByUuid(uuids[1]);
        REQUIRE(static_cast<uint32_t>(rootHandle) != static_cast<uint32_t>(entt::null));
        REQUIRE(static_cast<uint32_t>(childHandle) != static_cast<uint32_t>(entt::null));
        REQUIRE(reg.all_of<PrefabMemberComponent>(rootHandle));
        REQUIRE(reg.all_of<PrefabMemberComponent>(childHandle));
        return { rootHandle, childHandle };
    }
};

// A minimal, valid v5 (or v6) scene file whose single entity is a prefab
// member. `overrides` is optionally injected for the discriminating v5 test.
void S2WriteMemberScene(const std::filesystem::path& p, uint32_t version,
                        const std::vector<std::string>& overrides)
{
    std::string ovText;
    if (!overrides.empty())
    {
        ovText = ",\"overrides\":[";
        for (std::size_t i = 0; i < overrides.size(); ++i)
        {
            if (i) ovText += ",";
            ovText += "\"" + overrides[i] + "\"";
        }
        ovText += "]";
    }
    std::string u0 = "11111111-1111-4111-8111-111111111111";
    std::string iid = "22222222-2222-4222-8222-222222222222";
    std::string tid = "33333333-3333-4333-8333-333333333333";
    std::ostringstream body;
    body << "{\n";
    body << " \"version\":" << version << ",\n";
    body << " \"metadata\":{\"name\":\"s2member\"},\n";
    body << " \"entities\":[\n";
    body << "  {\"uuid\":\"" << u0 << "\",\"name\":\"Member\",\"parent\":\"\","
        << "\"visible\":true,\"prefabMember\":{\"instanceId\":\"" << iid
        << "\",\"templateId\":\"" << tid << "\"" << ovText << "}}\n";
    body << " ],\n";
    body << " \"materials\":[],\n";
    body << " \"textures\":[]\n";
    body << "}\n";
    S2WriteRaw(p, body.str());
}

PrefabComponentKey S2Key(const char* wire)
{
    const auto key = FindComponentByWire(wire);
    REQUIRE(key.has_value());
    return *key;
}

} // namespace


// ---------------------------------------------------------------------------
// Spec test 1 — the override set round-trips save -> load -> save
// byte-identically, and the vector's CONTENTS are asserted (not merely that
// the scene JSON round-trips).
//
// Using a real instance gives genuine PrefabMemberComponent values (fresh
// instanceId/templateId). Vectors are set directly on the live components in
// sorted order (S2 has no marking API), the invariant the read path also
// enforces.
//
// Discrimination faults:
//   Sequence a) write path fault: don't emit the "overrides" member in
//   EntityRecordToJson — the saved JSON lacks "overrides" -> RED.
//   Sequence b) read path fault: parse "overrides" into r.prefabMember.overrides
//   but never copy it into the emplaced component (drop it in
//   BuildDocumentFromRecords) — the loaded member's vector is empty -> RED.
//   Sequence c) read path fault: don't sort the parsed overrides. NOTE — the
//   byte-identical check below does NOT discriminate this fault (the live
//   vectors at the top are set already-sorted, so load preserves a canonical
//   order and rounds 1 and 2 still match). It is the `unsorted-v6` fixture
//   further down that actually proves the sort: a hand-written file whose
//   overrides are out of wire order must load into canonical [name, transform]
//   order, not the file's order. The byte-identical assertion proves
//   save -> load -> save idempotence; the unsorted fixture proves the sort.
//   Sequence d) read path fault: write "overrides" only when the version is
//   current but read it for any version, with a stray overrides set on a v5
//   file — not part of this test (spec test 2 covers the gate).
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: override set round-trips and loads with the exact contents")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_s2_roundtrip");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;
    auto* rootMember = reg.try_get<PrefabMemberComponent>(rootHandle);
    auto* childMember = reg.try_get<PrefabMemberComponent>(childHandle);
    REQUIRE(rootMember);
    REQUIRE(childMember);

    // Sorted, unique sets (name < transform; script only).
    rootMember->overrides = { S2Key("name"), S2Key("transform") };
    childMember->overrides = { S2Key("script") };

    // Save the live scene and confirm the stored wire names are present.
    const auto scenePath0 = dir / "scene0.rt2scene";
    Error saveErr0;
    REQUIRE(SaveSceneForTest(f.manager.AuthoringDoc(), scenePath0, saveErr0));
    nlohmann::json saved0;
    { std::ifstream in(scenePath0); in >> saved0; }
    bool foundRootOverrides = false;
    bool foundChildOverrides = false;
    for (auto& e : saved0["entities"])
    {
        if (!e.contains("prefabMember")) continue;
        const auto& pm = e["prefabMember"];
        std::vector<std::string> names;
        if (pm.contains("overrides"))
            for (auto& n : pm["overrides"]) names.push_back(n);
        if (names.size() == 2 && names[0] == "name" && names[1] == "transform")
            foundRootOverrides = true;
        if (names.size() == 1 && names[0] == "script")
            foundChildOverrides = true;
    }
    REQUIRE(foundRootOverrides);
    REQUIRE(foundChildOverrides);

    // Load into a fresh document and assert the vector CONTENTS.
    DeterministicUuidProvider p2;
    SceneDocument loaded;
    loaded.SetUuidProvider(&p2);
    Error loadErr;
    REQUIRE(SceneSerializer::Load(loaded, scenePath0, loadErr));
    const auto lr = loaded.FindByUuid(reg.get<EntityIdComponent>(rootHandle).id);
    const auto lc = loaded.FindByUuid(reg.get<EntityIdComponent>(childHandle).id);
    REQUIRE(static_cast<uint32_t>(lr) != static_cast<uint32_t>(entt::null));
    REQUIRE(static_cast<uint32_t>(lc) != static_cast<uint32_t>(entt::null));
    const auto* lRoot = loaded.ecs.registry.try_get<PrefabMemberComponent>(lr);
    const auto* lChild = loaded.ecs.registry.try_get<PrefabMemberComponent>(lc);
    REQUIRE(lRoot);
    REQUIRE(lChild);
    REQUIRE(lRoot->overrides.size() == 2);
    CHECK(lRoot->overrides[0] == S2Key("name"));
    CHECK(lRoot->overrides[1] == S2Key("transform"));
    REQUIRE(lChild->overrides.size() == 1);
    CHECK(lChild->overrides[0] == S2Key("script"));

    // Round 1: save the loaded doc; round 2: reload and save again. Both
    // documents are loaded (sourcePath = scenePath0) so reference rebasing is
    // identical; the two saves must be byte-for-byte equal.
    const auto scenePath1 = dir / "scene1.rt2scene";
    const auto scenePath2 = dir / "scene2.rt2scene";
    Error saveErr1, saveErr2;
    std::vector<AssetDiagnostic> saveDiag1, saveDiag2;
    REQUIRE(SceneSerializer::Save(loaded, scenePath1, saveDiag1, saveErr1));
    SceneDocument doc2;
    doc2.SetUuidProvider(&p2);
    Error loadErr2;
    REQUIRE(SceneSerializer::Load(doc2, scenePath1, loadErr2));
    REQUIRE(SceneSerializer::Save(doc2, scenePath2, saveDiag2, saveErr2));
    CHECK(S2ReadFile(scenePath1) == S2ReadFile(scenePath2));

    // Canonicalization: the read path sorts the parsed set, so a hand-written
    // v6 file whose overrides are stored out of wire order loads into the
    // canonical [name, transform] order — not the file's order.
    // Discrimination fault: delete the std::sort in JsonToEntityRecord — the
    // loaded vector then keeps the file's ["transform","name"] order and the
    // CHECK below fails -> RED. Revert -> GREEN.
    const auto unsortedPath = dir / "unsorted-v6.rt2scene";
    S2WriteMemberScene(unsortedPath, 6, {"transform", "name"});
    SceneDocument docU;
    DeterministicUuidProvider idsU;
    docU.SetUuidProvider(&idsU);
    Error loadErrU;
    REQUIRE(SceneSerializer::Load(docU, unsortedPath, loadErrU));
    const auto uh = docU.FindByUuid(
        UUID::Parse("11111111-1111-4111-8111-111111111111"));
    const auto* uMember = docU.ecs.registry.try_get<PrefabMemberComponent>(uh);
    REQUIRE(uMember);
    REQUIRE(uMember->overrides.size() == 2);
    CHECK(uMember->overrides[0] == S2Key("name"));
    CHECK(uMember->overrides[1] == S2Key("transform"));

    std::filesystem::remove_all(dir);
}


// ---------------------------------------------------------------------------
// Spec test 2 — a v5 scene loads with every override set empty. The fixture
// is a v5 file that (hypothetically) contains an "overrides" member; the read
// path must gate the field on the current schema and ignore it, so the loaded
// member carries an empty set — the correct meaning of "a v5 scene loaded by a
// v6 binary", not a migration failure.
//
// Discrimination fault: remove the `schemaVersion >= SchemaVersion` gate in
// JsonToEntityRecord and parse "overrides" regardless of version — the loaded
// member's set then pops as ["transform","script"], RED. Revert -> GREEN.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: a v5 scene loads with every override set empty")
{
    const auto dir = S2UniqueTempDir("p8w3_s2_v5_empty");
    const auto scenePath = dir / "v5.rt2scene";
    S2WriteMemberScene(scenePath, 5, {"transform", "script"});

    SceneDocument doc;
    DeterministicUuidProvider idsV5;
    doc.SetUuidProvider(&idsV5);
    Error err;
    SceneLoadReport report;
    REQUIRE(SceneSerializer::Load(doc, scenePath, report, err));
    const auto handle = doc.FindByUuid(UUID::Parse("11111111-1111-4111-8111-111111111111"));
    REQUIRE(static_cast<uint32_t>(handle) != static_cast<uint32_t>(entt::null));
    const auto* member = doc.ecs.registry.try_get<PrefabMemberComponent>(handle);
    REQUIRE(member);
    // Fault for red: without the read gate the "transform"/"script" members
    // above populate this set and the assertions below fail.
    CHECK(member->overrides.empty());

    std::filesystem::remove_all(dir);
}


// ---------------------------------------------------------------------------
// Spec test 3 — THE one that matters. Load a v5 scene, add an override, save
// through the recovery path (SaveTo), and assert the output is v6 AND the
// override survives. SaveTo deliberately preserves an older schema when
// untouched; without W3-D6's upgrade rule the recovery snapshot would be
// written as v5 and the override set silently dropped.
//
// Also asserts the preservation baseline: an untouched v5 doc, without calling
// PromoteSchemaVersion, still writes v5 via SaveTo (today's recovery
// semantics — pinned by Phase7W5Tests.cpp:98).
//
// Discrimination fault: break SceneSerializer::PromoteSchemaVersion so it no
// longer assigns (returns false for a below-current doc). doc stays v5, SaveTo
// writes version 5, and the recovery output has no "overrides" — RED on both
// the schema-version check and the override-presence check. Revert -> GREEN.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: recovery SaveTo writes v6 and keeps an added override (upgrade rule)")
{
    const auto dir = S2UniqueTempDir("p8w3_s2_recovery_upgrade");
    const auto scenePath = dir / "v5.rt2scene";
    S2WriteMemberScene(scenePath, 5, {});

    SceneDocument scene;
    DeterministicUuidProvider idsScene;
    scene.SetUuidProvider(&idsScene);
    Error loadErr;
    REQUIRE(SceneSerializer::Load(scene, scenePath, loadErr));
    REQUIRE(scene.metadata.schemaVersion == 5);

    // Preserved: SaveTo of an UNTOUCHED v5 doc still writes v5.
    const auto untouchedOut = dir / "untouched-v5.rt2scene";
    std::vector<AssetDiagnostic> diag0;
    Error e0;
    REQUIRE(SceneSerializer::SaveTo(scene, untouchedOut, scenePath, diag0, e0));
    {
        nlohmann::json j; { std::ifstream in(untouchedOut); in >> j; }
        REQUIRE(j["version"].get<uint32_t>() == 5);
        // No override set (empty) -> the member is present, no overrides.
        bool anyOverrides = false;
        for (auto& e : j["entities"])
            if (e.contains("prefabMember") && e["prefabMember"].contains("overrides"))
                anyOverrides = true;
        CHECK_FALSE(anyOverrides);
    }

    // Add an override directly (no marking API in S2), then apply the upgrade
    // rule — the operation that adds the first override sets schemaVersion.
    const auto handle = scene.FindByUuid(UUID::Parse("11111111-1111-4111-8111-111111111111"));
    REQUIRE(static_cast<uint32_t>(handle) != static_cast<uint32_t>(entt::null));
    auto* member = scene.ecs.registry.try_get<PrefabMemberComponent>(handle);
    REQUIRE(member);
    member->overrides = { S2Key("materialOverride") };
    // Fault for red: if PromoteSchemaVersion stops assigning, this returns
    // false and metadata stays 5 -> the recovery capture below writes v5 and
    // drops the override.
    REQUIRE(SceneSerializer::PromoteSchemaVersion(scene));
    CHECK(scene.metadata.schemaVersion == SceneSerializer::SchemaVersion);

    const auto recoveryOut = dir / "recovery.rt2scene";
    std::vector<AssetDiagnostic> diag;
    Error e;
    REQUIRE(SceneSerializer::SaveTo(scene, recoveryOut, scenePath, diag, e));
    nlohmann::json out;
    { std::ifstream in(recoveryOut); in >> out; }
    // Fault for red: without the upgrade the version written is 5.
    REQUIRE(out["version"].get<uint32_t>() == SceneSerializer::SchemaVersion);
    bool foundOverride = false;
    for (auto& entity : out["entities"])
    {
        if (!entity["prefabMember"].is_object()) continue;
        const auto& pm = entity["prefabMember"];
        if (pm.contains("overrides") && pm["overrides"].size() == 1 &&
            pm["overrides"][0].get<std::string>() == "materialOverride")
            foundOverride = true;
    }
    REQUIRE(foundOverride);

    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// 1. All 13 persisted components have a wire key, and the table's order
//    matches PersistedComponents::ForEach order — driven by an actual ForEach
//    visitor, not a hand-written list that could drift the same way.
//
//    Discrimination fault: reorder two rows of kPrefabTable (e.g. swap
//    "visible" and "meshRef") without touching PersistedComponents. The
//    specialized key for MeshRef then disagrees with the table entry at the
//    same position -> RED. Revert -> GREEN.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: prefab key table matches PersistedComponents::ForEach")
{
    std::size_t mismatches = 0;
    const std::size_t visited = ForEachTableAgreement(mismatches);

    REQUIRE(visited == PersistedComponents::Count);
    REQUIRE(mismatches == 0);

    // Table size mirrors the frozen Count rather than trusting the visitor
    // alone (the visitor could have been extended to agree with an oversized
    // table). This is the runtime shadow of static_assert 1.
    CHECK(kPrefabTable.size() == PersistedComponents::Count);
    CHECK(kPrefabTable.size() == 13);
}

// ---------------------------------------------------------------------------
// 2. Exactly 8 are overridable, and each of the 5 excluded components is
//    rejected by name.
//
//    Discrimination faults:
//      a) flip NameComponent's bit to false in kPrefabTable -> count drops to
//         7 and the "name" by-name check fails -> RED.
//      b) flip PrimitiveComponent's bit to true in kPrefabTable -> count rises
//         to 9 and the "primitive" by-name rejection fails -> RED.
//    Revert both -> GREEN.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: exactly 8 components overridable; 5 excluded by name")
{
    std::size_t overridable = 0;
    for (const auto& key : kPrefabTable)
    {
        if (IsOverridable(key)) ++overridable;
        CHECK(key.valid());
    }
    REQUIRE(overridable == 8);

    // Type form of the predicate agrees with the table-derived count.
    REQUIRE(IsOverridable<NameComponent>());
    REQUIRE(IsOverridable<Transform>());
    REQUIRE(IsOverridable<VisibleComponent>());
    REQUIRE(IsOverridable<MaterialOverrideComponent>());
    REQUIRE(IsOverridable<LightComponent>());
    REQUIRE(IsOverridable<CameraComponent>());
    REQUIRE(IsOverridable<MotionComponent>());
    REQUIRE(IsOverridable<ScriptComponent>());

    REQUIRE_FALSE(IsOverridable<MeshRef>());
    REQUIRE_FALSE(IsOverridable<PrimitiveComponent>());
    REQUIRE_FALSE(IsOverridable<ImportedMeshSourceComponent>());
    REQUIRE_FALSE(IsOverridable<PrefabInstanceComponent>());
    REQUIRE_FALSE(IsOverridable<PrefabMemberComponent>());

    // Rejection by name (boundary): a wire name of an excluded component must
    // resolve to a non-overridable classification, not fall through to a
    // permissive default.
    const char* excluded[] = { "meshRef", "primitive", "importedSource",
                               "prefabInstance", "prefabMember" };
    for (const char* wire : excluded)
    {
        const auto key = FindComponentByWire(wire);
        REQUIRE(key.has_value());
        CHECK_FALSE(key->overridable());
    }

    const char* overridableWires[] = { "name", "transform", "visible",
                                       "materialOverride", "light", "camera",
                                       "motion", "script" };
    for (const char* wire : overridableWires)
    {
        const auto key = FindComponentByWire(wire);
        REQUIRE(key.has_value());
        CHECK(key->overridable());
    }
}

// ---------------------------------------------------------------------------
// 3. Wire key <-> name round-trips for all 13: every specialized type's wire
//    resolves back to the classification that produced it, and no two table
//    rows share a wire (a duplicated wire would make one component's override
//    ambiguous and the reverse lookup would return the wrong row).
//
//    Discrimination faults:
//      a) give CameraComponent the same wire as ScriptComponent in the table
//         -> the wire set has 12 distinct members instead of 13, and the
//         round-trip for the duplicated row resolves to the earlier one ->
//         RED.
//      b) rename a wire constant in the specialization only (e.g.
//         PrefabComponentKeyFor<MotionComponent> to "motion2") while the table
//         keeps "motion" -> the ForEach round-trip fails -> RED.
//    Revert both -> GREEN.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: wire key <-> name round-trips for all 13 components")
{
    std::size_t index = 0;
    PersistedComponents::ForEach([&](auto tag) {
        using T = typename decltype(tag)::Type;
        REQUIRE(index < kPrefabTable.size());

        const auto specialized = PrefabComponentKeyFor<T>::value;

        // Forward: the specialization's wire must resolve back to the very
        // classification the table holds at this position.
        const auto byWire = FindComponentByWire(specialized.wire());
        REQUIRE(byWire.has_value());
        CHECK(*byWire == kPrefabTable[index]);
        CHECK(*byWire == specialized);

        // Reverse: the table row's wire resolves back to itself.
        const auto byTableWire = FindComponentByWire(kPrefabTable[index].wire());
        REQUIRE(byTableWire.has_value());
        CHECK(*byTableWire == kPrefabTable[index]);

        ++index;
    });
    REQUIRE(index == PersistedComponents::Count);

    // No two rows share a wire: 13 distinct wires for 13 rows.
    std::size_t distinct = 0;
    for (std::size_t i = 0; i < kPrefabTable.size(); ++i)
    {
        bool seen = false;
        for (std::size_t j = 0; j < i; ++j)
        {
            if (kPrefabTable[i].wire() == kPrefabTable[j].wire()) { seen = true; break; }
        }
        if (!seen) ++distinct;
    }
    CHECK(distinct == kPrefabTable.size());
    CHECK(distinct == 13);
}

// ---------------------------------------------------------------------------
// 4. An unrecognised name is loudly unresolvable, not silently mapped to
//    anything.
//
//    Discrimination fault: replace FindComponentByWire's not-found branch with
//    `return kPrefabTable[0];` (index-0 default). The empty-string and unknown
//    lookups below then report a valid-looking key instead of an empty
//    optional -> RED. Revert -> GREEN. This is the header's contract: a
//    lookup that falls back to index 0, or returns a default key for an
//    unrecognised name, is exactly the swallowed-failure bug class this step
//    exists to prevent.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: unrecognised wire names are loudly unresolvable")
{
    const std::string unknownNames[] = {
        "",                         // empty string
        "noSuchComponent",
        "Name",                     // case differs from the real wire
        "name ",                    // trailing whitespace
        "prefabmember",             // wrong casing of a real wire
    };

    for (const auto& wire : unknownNames)
    {
        const auto key = FindComponentByWire(wire);
        REQUIRE_FALSE(key.has_value());
    }
}


// ---------------------------------------------------------------------------
// Spec test 4 — an unknown component key in a scene file raises an observable
// diagnostic AND leaves the destination transactional. A v6 scene lists an
// override wire that the frozen table does not know ("noSuchComponent"); load
// must fail loudly (an Error naming the offending wire) and must not replace
// an already-populated destination document. Silent dropping is exactly the
// swallowed-failure defect class this codebase guards against — it would
// convert "this instance diverged" into "this instance tracks the source".
//
// Discrimination fault: replace the unknown-key diagnostic in JsonToEntityRecord
// with a `continue` (skip the unresolvable entry) — the load then succeeds and
// the destination is replaced, so REQUIRE_FALSE(Load) fails -> RED. Revert ->
// GREEN.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: an unknown override component key fails loudly and leaves the destination untouched")
{
    const auto dir = S2UniqueTempDir("p8w3_s2_unknown_key");

    // Pre-populate the destination with a VALID v6 scene whose member has a
    // DISTINCT uuid (4444...). The bad scene targets a different uuid (1111...),
    // so "unchanged" means the 4444 entity is still alive and the 1111 one is
    // not present.
    const auto goodPath = dir / "good.rt2scene";
    S2WriteMemberScene(goodPath, 6, {});
    // Rewrite the member uuid for the good scene to 4444... without touching
    // S2WriteMemberScene's fixed uuids.
    {
        nlohmann::json j; { std::ifstream in(goodPath); in >> j; }
        j["entities"][0]["uuid"] = "44444444-4444-4444-8444-444444444444";
        std::ofstream out(goodPath, std::ios::trunc); out << j.dump(2);
    }

    SceneDocument doc;
    DeterministicUuidProvider idsGood;
    doc.SetUuidProvider(&idsGood);
    Error goodErr;
    REQUIRE(SceneSerializer::Load(doc, goodPath, goodErr));
    REQUIRE(static_cast<uint32_t>(doc.FindByUuid(
        UUID::Parse("44444444-4444-4444-8444-444444444444"))) != static_cast<uint32_t>(entt::null));

    // The bad scene: an unknown override key in a v6 prefabMember.
    const auto badPath = dir / "bad.rt2scene";
    S2WriteMemberScene(badPath, 6, {"transform", "noSuchComponent"});

    Error err;
    SceneLoadReport report;
    // Fault for red: replacing the unknown-key diagnostic with `continue` makes
    // this REQUIRE succeed (load returns true) -> RED.
    REQUIRE_FALSE(SceneSerializer::Load(doc, badPath, report, err));
    CHECK(err.code == Error::Parse);
    CHECK(err.detail.find("noSuchComponent") != std::string::npos);

    // Transactional: the destination is byte-for-byte the pre-populated scene.
    CHECK(static_cast<uint32_t>(doc.FindByUuid(
        UUID::Parse("44444444-4444-4444-8444-444444444444"))) != static_cast<uint32_t>(entt::null));
    CHECK(static_cast<uint32_t>(doc.FindByUuid(
        UUID::Parse("11111111-1111-4111-8111-111111111111"))) == static_cast<uint32_t>(entt::null));

    std::filesystem::remove_all(dir);
}


// ---------------------------------------------------------------------------
// Spec test 6 — a prefab RECORD carrying every material and resource shape
// still contains no scene-side link or override data. Asserting only that a
// prefab asset exists is vacuous; this pins the file-format boundary
// (PrefabSerializer.h): the .rt2prefab payload must never carry
// PrefabInstanceComponent/PrefabMemberComponent OR the override set.
//
// Part A: serialize a record that has a MeshRef, PrimitiveComponent,
// ImportedMeshSourceComponent, MaterialOverrideComponent (full material shape),
// Light, Camera, Motion and Script — but no prefab link. The output payload
// must contain the material/resource shapes and none of prefabInstance,
// prefabMember, "overrides";
// Part B: a record that DOES carry a prefab member (with an override set) is
// REFUSED loudly by PrefabRecordToJson (Error::InvalidArgument) — the existing
// W1 guard at the prefab codec boundary.
//
// Discrimination faults:
//   Part A: make PrefabRecordToJson serialize the prefab member (and its
//   override set) into the payload instead of never carrying it — the output
//   then contains "prefabMember"/"overrides" -> RED.
//   Part B: remove the `hasPrefabInstance || hasPrefabMember` refusal in
//   PrefabRecordToJson — the linked record serializes instead of failing ->
//   RED.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: a prefab record with all shapes carries no scene-side link or override data")
{
    // A prefab record carrying every material/resource shape (all of the
    // durable component payloads a prefab file is allowed to hold).
    auto makePlain = []() {
        PrefabEntityRecord rec;
        rec.templateId = UUID::Parse("aaaaaaa1-1111-4111-8111-111111111111");
        SubtreeEntityRecord& s = rec.record;
        s.uuid    = UUID::Parse("bbbbbbb2-2222-4222-8222-222222222222");
        s.name    = "Rich";
        s.visible = true;
        s.translation = { 1.0f, 2.0f, 3.0f };

        s.hasMeshRef = true; s.meshIndex = 7; s.materialIndex = 3;
        s.hasPrimitive = true; s.primitive.kind = PrimitiveComponent::Cube;

        s.hasImportedSource = true;
        s.importedSource.model.kind = AssetKind::Model;
        s.importedSource.model.path = "model.glb";
        s.importedSource.model.assetId = UUID::Parse("ccccccc3-3333-4333-8333-333333333333");

        s.hasMaterialOverride = true;
        s.materialOverride.authored = true;
        s.materialOverride.sourceMaterialKey = "gltf:material:name=Mat";
        s.materialOverride.material.baseColor = { 0.2f, 0.3f, 0.4f };
        s.materialOverride.material.baseAlpha = 1.0f;
        s.materialOverride.material.roughness = 0.7f;

        s.hasLight = true; s.light.color = { 1.0f, 0.0f, 0.0f }; s.light.intensity = 2.0f;
        s.hasCamera = true; s.camera.verticalFOV = 60.0f;
        s.hasMotion = true; s.motion.linearVelocity = { 0.0f, 1.0f, 0.0f };
        s.hasScript = true;
        s.script.asset.kind = AssetKind::Script;
        s.script.asset.path = "a.lua";
        s.script.asset.sourceKey = "lua:asset=a.lua";
        return rec;
    };

    // ---- Part A: no prefab link, all shapes serialized. ----
    PrefabEntityRecord plain = makePlain();
    nlohmann::json out;
    std::vector<AssetDiagnostic> diags;
    Error err;
    REQUIRE(PrefabRecordToJson(plain, diags, err, out));
    REQUIRE(out.is_object());
    REQUIRE(out["record"].is_object());
    const auto& payload = out["record"];
    // The rich shapes ARE present (the record is real, not vacuous).
    CHECK(payload.contains("materialOverride"));
    CHECK(payload.contains("importedSource"));
    CHECK(payload.contains("primitive"));
    CHECK(payload.contains("light"));
    CHECK(payload.contains("camera"));
    CHECK(payload.contains("script"));
    // And none of the scene-side link / override data.
    CHECK_FALSE(payload.contains("prefabInstance"));
    CHECK_FALSE(payload.contains("prefabMember"));
    CHECK_FALSE(payload.contains("overrides"));
    // HARD RULE: no transient resource-table indices.
    CHECK_FALSE(payload["meshRef"].contains("materialIndex"));
    CHECK_FALSE(payload["materialOverride"]["material"].contains("baseColorTextureIndex"));

    // The serialized prefab FILE also carries none of it (deterministic dump).
    PrefabDocument doc;
    doc.version = PrefabSerializer::FormatVersion;
    doc.entities.push_back(plain);
    std::string content;
    Error serErr;
    REQUIRE(PrefabSerializer::Serialize(doc, content, serErr));
    CHECK(content.find("prefabMember") == std::string::npos);
    CHECK(content.find("prefabInstance") == std::string::npos);
    CHECK(content.find("overrides") == std::string::npos);

    // ---- Part B: a record carrying a prefab link is refused loudly. ----
    PrefabEntityRecord linked = makePlain();
    linked.record.hasPrefabMember = true;
    linked.record.prefabMember.instanceId = UUID::Parse("55555555-5555-4555-8555-555555555555");
    linked.record.prefabMember.templateId = UUID::Parse("66666666-6666-4666-8666-666666666666");
    // Only ever table-resolved keys (no dangling view).
    linked.record.prefabMember.overrides = { S2Key("transform") };
    nlohmann::json linkedOut;
    Error linkedErr;
    // Fault for red: remove the `hasPrefabInstance || hasPrefabMember` refusal
    // in PrefabRecordToJson — this then serializes and REQUIRE_FALSE fails.
    REQUIRE_FALSE(PrefabRecordToJson(linked, diags, linkedErr, linkedOut));
    CHECK(linkedErr.code == Error::InvalidArgument);
}


// ---------------------------------------------------------------------------
// Spec test 4b (Fix 2, S2 review finding 1) — a v6 scene whose override set
// names a component the table classifies as NEVER overridable ("meshRef") is
// rejected loudly and leaves the destination transactional. The reader was
// checking "is the name known" but not "is the name overridable", so
// ["meshRef"] installed a forbidden key the classification table explicitly
// excludes — the same silent-divergence class the unknown-key branch guards
// against, with the asymmetry that a name the table has never heard of fails
// while a name the table forbids passes.
//
// Discrimination fault: remove the `!key->overridable()` branch in
// JsonToEntityRecord — the load then succeeds and the destination is replaced,
// so REQUIRE_FALSE(Load) fails -> RED. Revert -> GREEN.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: a non-overridable override component key fails loudly and leaves the destination untouched")
{
    const auto dir = S2UniqueTempDir("p8w3_s2_non_overridable_key");

    // Pre-populate the destination with a VALID v6 scene whose member has a
    // DISTINCT uuid (4444...). The bad scene targets a different uuid (1111...),
    // so "unchanged" means the 4444 entity is still alive and the 1111 one is
    // not present.
    const auto goodPath = dir / "good.rt2scene";
    S2WriteMemberScene(goodPath, 6, {});
    {
        nlohmann::json j; { std::ifstream in(goodPath); in >> j; }
        j["entities"][0]["uuid"] = "44444444-4444-4444-8444-444444444444";
        std::ofstream out(goodPath, std::ios::trunc); out << j.dump(2);
    }

    SceneDocument doc;
    DeterministicUuidProvider idsBad;
    doc.SetUuidProvider(&idsBad);
    Error goodErr;
    REQUIRE(SceneSerializer::Load(doc, goodPath, goodErr));
    REQUIRE(static_cast<uint32_t>(doc.FindByUuid(
        UUID::Parse("44444444-4444-4444-8444-444444444444"))) != static_cast<uint32_t>(entt::null));

    // The bad scene: an override naming the NEVER-overridable "meshRef".
    const auto badPath = dir / "bad.rt2scene";
    S2WriteMemberScene(badPath, 6, {"transform", "meshRef"});

    Error err;
    SceneLoadReport report;
    // Fault for red: removing the overridable() branch lets this REQUIRE
    // succeed (load returns true) -> RED.
    REQUIRE_FALSE(SceneSerializer::Load(doc, badPath, report, err));
    CHECK(err.code == Error::Parse);
    CHECK(err.detail.find("meshRef") != std::string::npos);

    // Transactional: the destination is byte-for-byte the pre-populated scene.
    CHECK(static_cast<uint32_t>(doc.FindByUuid(
        UUID::Parse("44444444-4444-4444-8444-444444444444"))) != static_cast<uint32_t>(entt::null));
    CHECK(static_cast<uint32_t>(doc.FindByUuid(
        UUID::Parse("11111111-1111-4111-8111-111111111111"))) == static_cast<uint32_t>(entt::null));

    std::filesystem::remove_all(dir);
}


// ---------------------------------------------------------------------------
// Fix 8 (S2 review finding 8) — the reader de-duplicates the parsed set. A v6
// file with ["transform","transform"] must load into a single canonical
// "transform" entry, telling "this diverged on transform" once rather than
// twice (a duplicate would corrupt the invariant that the override vector is a
// SET).
//
// Discrimination fault: delete the `std::unique` in JsonToEntityRecord — the
// loaded set then keeps both copies and the size/contents checks fail -> RED.
// Revert -> GREEN.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: duplicate override names are de-duplicated on read")
{
    const auto dir = S2UniqueTempDir("p8w3_s2_dedup");
    const auto scenePath = dir / "dedup.rt2scene";
    S2WriteMemberScene(scenePath, 6, {"transform", "transform"});

    DeterministicUuidProvider idsDedup;
    SceneDocument doc;
    doc.SetUuidProvider(&idsDedup);
    Error err;
    REQUIRE(SceneSerializer::Load(doc, scenePath, err));
    const auto handle = doc.FindByUuid(UUID::Parse("11111111-1111-4111-8111-111111111111"));
    REQUIRE(static_cast<uint32_t>(handle) != static_cast<uint32_t>(entt::null));
    const auto* member = doc.ecs.registry.try_get<PrefabMemberComponent>(handle);
    REQUIRE(member);
    // Fault for red: without std::unique the set keeps both "transform" copies.
    REQUIRE(member->overrides.size() == 1);
    CHECK(member->overrides[0] == S2Key("transform"));

    std::filesystem::remove_all(dir);
}


// ---------------------------------------------------------------------------
// Fix 8 (S2 review finding 8) — the malformed-input branches: an array entry
// that is not a string (a number). Rejected loudly with Error::Parse.
//
// Discrimination fault: drop the `!item.is_string()` branch in
// JsonToEntityRecord — the load then succeeds on a numeric entry -> RED.
// Revert -> GREEN.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: a non-string override array entry is rejected")
{
    const auto dir = S2UniqueTempDir("p8w3_s2_non_string_override");
    const auto p = dir / "non-string.rt2scene";
    {
        std::string body =
            "{\n"
            " \"version\":6,\n"
            " \"metadata\":{\"name\":\"s2member\"},\n"
            " \"entities\":[\n"
            "  {\"uuid\":\"11111111-1111-4111-8111-111111111111\","
            "   \"name\":\"Member\",\"parent\":\"\",\"visible\":true,\n"
            "   \"prefabMember\":{\"instanceId\":\"22222222-2222-4222-8222-222222222222\","
            "                       \"templateId\":\"33333333-3333-4333-8333-333333333333\",\n"
            "                       \"overrides\":[\"transform\", 7]}}\n"
            " ],\n"
            " \"materials\":[],\n"
            " \"textures\":[]\n"
            "}\n";
        S2WriteRaw(p, body);
    }
    SceneDocument doc;
    DeterministicUuidProvider idsNS;
    doc.SetUuidProvider(&idsNS);
    Error err;
    REQUIRE_FALSE(SceneSerializer::Load(doc, p, err));
    CHECK(err.code == Error::Parse);

    std::filesystem::remove_all(dir);
}


// ---------------------------------------------------------------------------
// Fix 8 (S2 review finding 8) — the malformed-input branch: "overrides" present
// but not an array (here an object whose VALUES are all valid wire strings).
// Rejected loudly with Error::Parse.
//
// Discrimination fault: drop the `!ov.is_array()` branch in JsonToEntityRecord —
// the load then iterates the object's string values as if they were array
// entries and succeeds -> RED (REQUIRE_FALSE fails). Revert -> GREEN. An object
// whose values were non-strings would fall through to the non-string branch and
// error regardless, so the values MUST be valid wires for the branch to be
// actually load-bearing here.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: a non-array overrides member is rejected")
{
    const auto dir = S2UniqueTempDir("p8w3_s2_non_array_override");
    const auto p = dir / "non-array.rt2scene";
    {
        std::string body =
            "{\n"
            " \"version\":6,\n"
            " \"metadata\":{\"name\":\"s2member\"},\n"
            " \"entities\":[\n"
            "  {\"uuid\":\"11111111-1111-4111-8111-111111111111\","
            "   \"name\":\"Member\",\"parent\":\"\",\"visible\":true,\n"
            "   \"prefabMember\":{\"instanceId\":\"22222222-2222-4222-8222-222222222222\","
            "                       \"templateId\":\"33333333-3333-4333-8333-333333333333\",\n"
            "                       \"overrides\":{\"first\":\"name\",\"second\":\"transform\"}}}\n"
            " ],\n"
            " \"materials\":[],\n"
            " \"textures\":[]\n"
            "}\n";
        S2WriteRaw(p, body);
    }
    SceneDocument doc;
    DeterministicUuidProvider idsNA;
    doc.SetUuidProvider(&idsNA);
    Error err;
    REQUIRE_FALSE(SceneSerializer::Load(doc, p, err));
    CHECK(err.code == Error::Parse);

    std::filesystem::remove_all(dir);
}


// ---------------------------------------------------------------------------
// Fix 8 (S2 review finding 8) — CloneInMemory preserves the override set.
// CloneInMemory never touches JSON: it goes CollectRecords ->
// BuildDocumentFromRecords, both of which copy the whole PrefabMemberComponent,
// so overrides survive regardless of schemaVersion. Nothing proved this; this
// test pins it (S5 relies on the Play-mode clone carrying overrides).
//
// Discrimination fault: drop the override set in BuildDocumentFromRecords when
// it copies the record's PrefabMemberComponent into the emplacement — the
// cloned member then has an empty set -> RED. Revert -> GREEN.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: CloneInMemory preserves the override set")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_s2_clone_overrides");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;
    auto* rootMember = reg.try_get<PrefabMemberComponent>(rootHandle);
    REQUIRE(rootMember);
    rootMember->overrides = { S2Key("name"), S2Key("transform") };

    SceneDocument clone;
    DeterministicUuidProvider idsClone;
    clone.SetUuidProvider(&idsClone);
    Error cloneErr;
    REQUIRE(SceneSerializer::CloneInMemory(f.manager.AuthoringDoc(), clone, cloneErr));
    const auto ch = clone.FindByUuid(reg.get<EntityIdComponent>(rootHandle).id);
    REQUIRE(static_cast<uint32_t>(ch) != static_cast<uint32_t>(entt::null));
    const auto* cMember = clone.ecs.registry.try_get<PrefabMemberComponent>(ch);
    REQUIRE(cMember);
    // Fault for red: dropping the override set in the clone path leaves this empty.
    REQUIRE(cMember->overrides.size() == 2);
    CHECK(cMember->overrides[0] == S2Key("name"));
    CHECK(cMember->overrides[1] == S2Key("transform"));

    std::filesystem::remove_all(dir);
}


// ---------------------------------------------------------------------------
// Spec test 3b (Fix 3, S2 review finding 2) — the save choke point. A below-
// current output (v5) that would DROP a non-empty override set must fail the
// save loudly, not silently write v5 and lose the set. This is the invariant
// PromoteSchemaVersion maintains; SaveInternal enforces it so the eleven W3-D5
// mutation entry points don't each have to remember to call Promote.
//
// Discrimination fault: remove the new validation block in SaveInternal — the
// recovery-path SaveTo then succeeds on the below-current doc and writes v5,
// silently dropping the override set -> RED. Revert -> GREEN.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: a save that would drop a non-empty override set fails")
{
    const auto dir = S2UniqueTempDir("p8w3_s2_save_guard");
    const auto scenePath = dir / "v5.rt2scene";
    S2WriteMemberScene(scenePath, 5, {});

    // Load a v5 doc (schemaVersion == 5), then add an override WITHOUT calling
    // PromoteSchemaVersion — the state PromoteSchemaVersion is meant to
    // prevent. SaveTo (recovery path) preserves the below-current version, so
    // outputVersion = 5 < SchemaVersion with a non-empty set -> must fail.
    SceneDocument doc;
    DeterministicUuidProvider idsSav;
    doc.SetUuidProvider(&idsSav);
    Error loadErr;
    REQUIRE(SceneSerializer::Load(doc, scenePath, loadErr));
    REQUIRE(doc.metadata.schemaVersion == 5);
    const auto handle = doc.FindByUuid(UUID::Parse("11111111-1111-4111-8111-111111111111"));
    REQUIRE(static_cast<uint32_t>(handle) != static_cast<uint32_t>(entt::null));
    auto* member = doc.ecs.registry.try_get<PrefabMemberComponent>(handle);
    REQUIRE(member);
    member->overrides = { S2Key("script") };

    const auto out = dir / "out.rt2scene";
    std::vector<AssetDiagnostic> diag;
    Error e;
    // Fault for red: without the SaveInternal guard this REQUIRE succeeds and
    // the output is v5 with the override dropped -> RED.
    REQUIRE_FALSE(SceneSerializer::SaveTo(doc, out, scenePath, diag, e));
    CHECK(e.code == Error::InvalidArgument);

    std::filesystem::remove_all(dir);
}


// ---------------------------------------------------------------------------
// Spec test 1b (Fix 5, S2 review finding 5) — the writer canonicalizes the
// override set, so memory->file matches the canonical wire order the reader
// produces file->memory. S5 will populate vectors in edit order; without the
// write-side sort the same logical scene writes different bytes depending on
// who populated the vector, breaking the save -> load -> save byte-identity
// that spec test 1 asserts. The vectors here are deliberately set UNSORTED
// (transform before name) — the sorted-set invariant is a codec guarantee, not
// a caller contract.
//
// Discrimination fault: remove the sort in EntityRecordToJson's override
// emission — the stored wire list then keeps the in-memory ["transform","name"]
// order instead of the canonical ["name","transform"] -> RED. Revert -> GREEN.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: the writer stores the override set in canonical wire order")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_s2_write_sort");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;
    auto* rootMember = reg.try_get<PrefabMemberComponent>(rootHandle);
    REQUIRE(rootMember);
    // Deliberately unsorted in memory: the writer must canonicalize.
    rootMember->overrides = { S2Key("transform"), S2Key("name") };

    const auto scenePath = dir / "scene.rt2scene";
    Error saveErr;
    REQUIRE(SaveSceneForTest(f.manager.AuthoringDoc(), scenePath, saveErr));
    nlohmann::json saved;
    { std::ifstream in(scenePath); in >> saved; }
    bool foundCanonical = false;
    for (auto& e : saved["entities"])
    {
        if (!e.contains("prefabMember")) continue;
        const auto& pm = e["prefabMember"];
        if (!pm.contains("overrides")) continue;
        std::vector<std::string> names;
        for (auto& n : pm["overrides"]) names.push_back(n);
        if (names.size() == 2 && names[0] == "name" && names[1] == "transform")
            foundCanonical = true;
    }
    // Fault for red: without the write-side sort the stored order is the
    // in-memory ["transform","name"] and this fails.
    REQUIRE(foundCanonical);

    std::filesystem::remove_all(dir);
}


// ============================================================================
// Phase 8 W3, S3 — the snapshot verifier sees the override set
// (implementation spec, W3-D2; Work step S3).
//
// EntityMatchesRecord (SceneManager.cpp:1815) is the guard that
// RemoveSubtreesExact (SceneManager.cpp:2182) runs over every entity before
// destroying anything, failing the whole operation on a mismatch (:2197-2200).
// Pre-S3 it compared only instanceId + templateId on the PrefabMemberComponent
// branch, so an override-set change was invisible to it: duplicate an
// instance, edit the duplicate's overrides, undo the duplication, and the
// verifier still matched and destroyed the edited copy — a guard that had
// stopped guarding.
//
// S3 extends the comparison to the override vector. Order decision: compare as
// a SET (order-insensitive). The codec sorts and de-duplicates on read and
// write (SceneSerializer.cpp:853-856, :1261-1270), so any vector that has
// passed through a file round-trip is canonical — but an in-memory vector has
// not necessarily been through the codec (the S5/S6 marking path records in
// edit order; S2's write-sort test deliberately builds {transform, name}
// unsorted and relies on the writer to canonicalize). An order-sensitive
// compare would report a false mismatch for two logically-equal sets and
// break legitimate structural undo. Set comparison still catches every real
// divergence, which is all the guard exists to catch. EntityMatchesRecord is
// file-local (not in a header), so these tests drive the guard through
// RemoveSubtreesExact — the exact proof the brief requires.
//
// Discrimination fault (recorded in the verification report):
//   revert EntityMatchesRecord's PrefabMember branch to compare instanceId +
//   templateId only (drop the override comparison) — the mutate-then-remove
//   case below then reports a MATCH and DESTROYS the edited entity -> RED on
//   "must fail and leave the entity intact". Reverting to the final form turns
//   both cases GREEN.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: verifier sees an override-set change and refuses to remove")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_s3_mismatch");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);

    auto& reg = f.manager.GetECS().registry;
    auto* rootMember = reg.try_get<PrefabMemberComponent>(rootHandle);
    REQUIRE(rootMember);
    // Non-empty before capture: an empty->non-empty jump would only prove the
    // guard sees presence. Non-empty->different proves it sees the SET.
    rootMember->overrides = { S2Key("name"), S2Key("transform") };

    const UUID rootUuid = reg.get<EntityIdComponent>(rootHandle).id;
    REQUIRE(rootUuid != UUID::Nil());

    // Capture exactly the shape the duplicate/delete commands capture.
    auto snapshot = f.manager.CaptureSubtreeSnapshot({ rootUuid });
    REQUIRE(snapshot.entities.size() == 2);
    const auto snapRoot = std::find_if(
        snapshot.entities.begin(), snapshot.entities.end(),
        [&](const SubtreeEntityRecord& r) { return r.uuid == rootUuid; });
    REQUIRE(snapRoot != snapshot.entities.end());
    REQUIRE(snapRoot->prefabMember.overrides.size() == 2);

    // Post-copy edit: the live override set now differs from the snapshot.
    rootMember->overrides = { S2Key("name"), S2Key("transform"), S2Key("light") };

    // The guard must refuse: authored override state no longer matches.
    const auto result = f.manager.RemoveSubtreesExact(snapshot);
    REQUIRE_FALSE(result.success);
    REQUIRE(result.error.code == Error::InvalidEntity);
    REQUIRE(result.error.detail.find("authored state does not match")
            != std::string::npos);

    // Zero mutation on failure (:2189-2201 contract): the entity must still
    // exist and its override set must be the edited value, untouched by the
    // failed operation.
    const auto liveHandle = f.manager.FindEntityByUuid(rootUuid);
    REQUIRE(static_cast<uint32_t>(liveHandle)
            != static_cast<uint32_t>(entt::null));
    REQUIRE(reg.valid(liveHandle));
    const auto* liveMember = reg.try_get<PrefabMemberComponent>(liveHandle);
    REQUIRE(liveMember);
    REQUIRE(liveMember->overrides.size() == 3);
    CHECK(liveMember->overrides[2] == S2Key("light"));
    CHECK(std::find(liveMember->overrides.begin(), liveMember->overrides.end(),
                    S2Key("light")) != liveMember->overrides.end());

    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// The mirror case: an UNCHANGED override set must still verify and remove.
// This is what proves S3 did not simply make the verifier reject everything.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: an unchanged override set still verifies and removes")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_s3_match");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);

    auto& reg = f.manager.GetECS().registry;
    auto* rootMember = reg.try_get<PrefabMemberComponent>(rootHandle);
    REQUIRE(rootMember);
    rootMember->overrides = { S2Key("name"), S2Key("transform") };

    const UUID rootUuid = reg.get<EntityIdComponent>(rootHandle).id;
    REQUIRE(rootUuid != UUID::Nil());

    auto snapshot = f.manager.CaptureSubtreeSnapshot({ rootUuid });
    REQUIRE(snapshot.entities.size() == 2);

    // No edit since capture: the guard must still match and the remove goes
    // through.
    const auto result = f.manager.RemoveSubtreesExact(snapshot);
    REQUIRE(result.success);

    // Entities were really removed (the guard passed and destruction ran).
    REQUIRE_FALSE(reg.valid(rootHandle));
    REQUIRE_FALSE(reg.valid(childHandle));

    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// S3 review-fix: a duplicate in one side must not let the verifier report a
// match for genuinely different sets. The first S3 draft compared sizes then
// checked, one-directionally, that every key in `live` appears in `record`.
// That is true set equality only when both sides are duplicate-free. Here
// `live = {transform, transform}` against the captured `record =
// {transform, light}`: sizes match at 2 and every live key exists in the
// record, so the one-directional compare reports MATCH — and would destroy
// the edited entity, exactly the failure S3 exists to prevent. The S3 fix
// compares as multisets (sorted copies, element-wise), so this must be
// REFUSED.
//
// Discrimination fault (recorded in the verification report):
//   revert to the one-directional containment form (drop the sorted
//   element-wise compare) — this case then reports a MATCH and DESTROYS the
//   edited entity -> RED on "must fail and leave the entity intact". Reverting
//   to the multiset compare turns it GREEN.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: a duplicate cannot make the verifier miss a divergence")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_s3_dup");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);

    auto& reg = f.manager.GetECS().registry;
    auto* rootMember = reg.try_get<PrefabMemberComponent>(rootHandle);
    REQUIRE(rootMember);

    // Capture first with two distinct keys...
    rootMember->overrides = { S2Key("transform"), S2Key("light") };
    const UUID rootUuid = reg.get<EntityIdComponent>(rootHandle).id;
    REQUIRE(rootUuid != UUID::Nil());
    auto snapshot = f.manager.CaptureSubtreeSnapshot({ rootUuid });
    REQUIRE(snapshot.entities.size() == 2);
    const auto snapRoot = std::find_if(
        snapshot.entities.begin(), snapshot.entities.end(),
        [&](const SubtreeEntityRecord& r) { return r.uuid == rootUuid; });
    REQUIRE(snapRoot != snapshot.entities.end());

    // ...then mutate the live set to a same-size set the one-directional
    // compare cannot distinguish: {transform, transform} vs captured
    // {transform, light}.
    rootMember->overrides = { S2Key("transform"), S2Key("transform") };

    const auto result = f.manager.RemoveSubtreesExact(snapshot);
    REQUIRE_FALSE(result.success);
    REQUIRE(result.error.code == Error::InvalidEntity);
    REQUIRE(result.error.detail.find("authored state does not match")
            != std::string::npos);

    // Zero mutation on failure: the duplicate-valued entity survives intact.
    const auto liveHandle = f.manager.FindEntityByUuid(rootUuid);
    REQUIRE(static_cast<uint32_t>(liveHandle)
            != static_cast<uint32_t>(entt::null));
    REQUIRE(reg.valid(liveHandle));
    const auto* liveMember = reg.try_get<PrefabMemberComponent>(liveHandle);
    REQUIRE(liveMember);
    REQUIRE(liveMember->overrides.size() == 2);
    CHECK(liveMember->overrides[0] == S2Key("transform"));
    CHECK(liveMember->overrides[1] == S2Key("transform"));

    std::filesystem::remove_all(dir);
}


// ============================================================================
// Phase 8 W3, S4 — duplicating an instance creates a NEW instance identity
// (implementation spec, W3-D8; Work step S4).
//
// CopyAuthoredComponents copies all 13 persisted components verbatim, so a
// copy of an instance shares the SOURCE's instanceId. W3 groups overrides by
// instanceId; two instances sharing one id would merge into a single override
// group (W3-D8). S4 runs MintCopiedPrefabLinks on every copy path: each copied
// subtree mints ONE fresh instanceId and pushes it onto every copied
// PrefabMemberComponent, leaving templateIds and override vectors untouched
// (a copy of a diverged instance stays diverged, W3-D4).
//
// This section covers the duplicate, paste, restore and partial sides of S4.
// Tests 1-2 drive the duplicate paths — the ordinary non-command
// DuplicateSubtrees path and the UUID-aware DuplicateSubtreesWithUuids path
// (the one the editor's undoable DuplicateSelectionCommand uses,
// SceneEditorUI.cpp:396-415). Tests 3-5 drive the paste paths — the ordinary
// non-command PasteSubtreesFrom path and the UUID-aware PasteSubtreesWithUuids
// path (the one the editor's PasteCommand uses, SceneEditorUI.cpp:417-449). A
// test covering only one path would pass while the editor's real path stayed
// broken, so both must be pinned on each side. Tests 6-7 pin the two
// non-minting / different-policy sides; test 8 pins multi-root isolation.
//
// All four copy-shaped paths funnel through the SAME shared helper
// MintCopiedPrefabLinks (SceneManager.cpp:164): it mints ONE fresh instanceId
// per copied subtree, pushes it onto every copied PrefabMemberComponent and
// PrefabInstanceComponent, and leaves templateIds and override vectors
// untouched (a copy of a diverged instance stays diverged, W3-D4).
//
// The diverged-override case (test 5) is covered on the editor paste path
// (PasteSubtreesWithUuids) because that is the strongest discriminator for
// the shared helper: the source comes from a genuinely separate clipboard
// document (SceneSerializer::CloneInMemory), so the test proves the override
// set survives the whole chain — authoring doc -> clipboard clone -> paste
// copy -> mint — verbatim. That is exactly the W3-D4 compiler-failure mode;
// any mint or copy step that drops the set goes red.
//
// Structural restore (test 6) is deliberately NOT a copy path: ApplySubtreeRecord
// reinstates the recorded instanceId verbatim (W3-D4), so restore must never
// collide with MintCopiedPrefabLinks. Partial copies (test 7) strip both
// prefab components and surface a recoveryWarning instead of fabricating an
// instance root. Multi-root copies (test 8) mint per subtree, never one shared
// id and never one per member.
//
// Discrimination faults (recorded in the verification report), per test:
//   test 1 fault: delete the MintCopiedPrefabLinks call inside
//   SceneManager::DuplicateSubtrees (SceneManager.cpp:1603) — the copied
//   member then keeps the source's instanceId, so CHECK(dupRootId !=
//   srcInstanceId) fails -> RED. Revert -> GREEN.
//   test 2 fault: delete the MintCopiedPrefabLinks call inside
//   SceneManager::DuplicateSubtreesWithUuids (SceneManager.cpp:3457) — same
//   failure on the editor's command path -> RED. Revert -> GREEN.
//   test 3 fault: delete the MintCopiedPrefabLinks call inside
//   SceneManager::PasteSubtreesFrom (SceneManager.cpp:1712) — the pasted
//   member then keeps the clipboard's instanceId, so CHECK(pastedRootId !=
//   srcInstanceId) fails -> RED. Revert -> GREEN.
//   test 4 fault: delete the MintCopiedPrefabLinks call inside
//   SceneManager::PasteSubtreesWithUuids (SceneManager.cpp:3620) — same
//   failure on the editor's paste path -> RED. Revert -> GREEN.
//   test 5 fault: in MintCopiedPrefabLinks, in the full-instance branch,
//   clear each copied member's override vector when setting the fresh
//   instanceId (SceneManager.cpp:200-201) — the pasted diverged member then
//   has an empty set, so CHECK(pastedRootMember->overrides == srcRoot->overrides)
//   fails -> RED. Revert -> GREEN.
//   test 6 fault: in RestoreSubtrees, right after ApplySubtreeRecord
//   (SceneManager.cpp:2533), mint a fresh instanceId onto every restored
//   PrefabMemberComponent — the restored members then diverge from the
//   recorded id, so CHECK(restoredRootId == recordedId) fails -> RED.
//   Revert -> GREEN.
//   test 7 fault: delete the two component removals in MintCopiedPrefabLinks'
//   partial branch (SceneManager.cpp:220-226) — the copied member then keeps
//   its PrefabMemberComponent, so CHECK(copied has no PrefabMemberComponent)
//   fails -> RED. Revert -> GREEN.
//   test 8 fault: in MintCopiedPrefabLinks (SceneManager.cpp:195), reuse ONE
//   minted id for every root — the two copied subtrees then share a single
//   instanceId, so CHECK(copiedARootId != copiedBRootId) fails -> RED.
//   Revert -> GREEN.
// ============================================================================

namespace
{

// Collect every entity of the subtree rooted at a UUID (pre-order), via the
// live registry. Used to enumerate the freshly created duplicate members.
std::vector<entt::entity> S4SubtreeEntities(SceneManager& manager,
                                            const UUID& rootUuid)
{
    const auto root = manager.FindEntityByUuid(rootUuid);
    REQUIRE(static_cast<uint32_t>(root) != static_cast<uint32_t>(entt::null));
    std::vector<entt::entity> out;
    SceneHierarchy::CollectSubtreePreOrder(
        manager.GetECS().registry, root, out);
    return out;
}

UUID S4MemberInstanceId(SceneManager& manager, entt::entity e)
{
    const auto* m = manager.GetECS().registry.try_get<PrefabMemberComponent>(e);
    REQUIRE(m);
    return m->instanceId;
}

UUID S4MemberTemplateId(SceneManager& manager, entt::entity e)
{
    const auto* m = manager.GetECS().registry.try_get<PrefabMemberComponent>(e);
    REQUIRE(m);
    return m->templateId;
}

} // namespace

// ---------------------------------------------------------------------------
// Test 1 — ordinary DuplicateSubtrees mints ONE fresh instanceId for the whole
// copied instance, every copied member carries it, and templateIds are
// preserved positionally. The source instance's own identity is untouched.
//
// Fault for red: delete the MintCopiedPrefabLinks call inside
// SceneManager::DuplicateSubtrees — the duplicated members then keep the
// source's instanceId and CHECK(dupRootId != srcInstanceId) fails -> RED.
// Revert -> GREEN.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: a duplicated instance gets a fresh instanceId shared by all copied members")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_s4_dup_ordinary");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;

    const auto* srcRoot = reg.try_get<PrefabMemberComponent>(rootHandle);
    const auto* srcChild = reg.try_get<PrefabMemberComponent>(childHandle);
    REQUIRE(srcRoot);
    REQUIRE(srcChild);
    const UUID srcInstanceId = srcRoot->instanceId;
    REQUIRE(srcInstanceId != UUID::Nil());
    // Sanity: one instance, one identity — both members share it.
    CHECK(srcChild->instanceId == srcInstanceId);

    const auto rootUuid = reg.get<EntityIdComponent>(rootHandle).id;
    const auto result = f.manager.DuplicateSubtrees({ rootUuid });
    REQUIRE(result.success);
    REQUIRE_FALSE(result.recoveryWarning.has_value());

    // The duplicate root is the sole new root.
    REQUIRE(result.affectedEntities.size() == 1);
    const auto dupRootUuid = result.affectedEntities.front();
    REQUIRE(dupRootUuid != rootUuid);

    const auto dupEntities = S4SubtreeEntities(f.manager, dupRootUuid);
    REQUIRE(dupEntities.size() == 2);

    // Every copied member carries the same FRESH instanceId.
    const auto dupRootId = S4MemberInstanceId(f.manager, dupEntities[0]);
    const auto dupChildId = S4MemberInstanceId(f.manager, dupEntities[1]);
    CHECK(dupRootId != srcInstanceId);
    CHECK(dupChildId == dupRootId);

    // The duplicate root still carries a PrefabInstanceComponent, grouped
    // under the SAME fresh id its members carry.
    const auto* dupPic =
        reg.try_get<PrefabInstanceComponent>(dupEntities[0]);
    REQUIRE(dupPic);
    CHECK(dupPic->instanceId == dupRootId);

    // templateIds preserved positionally: the child copy keeps the source
    // child's templateId, the root copy keeps the source root's.
    CHECK(S4MemberTemplateId(f.manager, dupEntities[1]) == srcChild->templateId);
    CHECK(S4MemberTemplateId(f.manager, dupEntities[0]) == srcRoot->templateId);

    // The source instance is untouched — still its own id.
    const auto* srcRootAfter = reg.try_get<PrefabMemberComponent>(rootHandle);
    REQUIRE(srcRootAfter);
    CHECK(srcRootAfter->instanceId == srcInstanceId);

    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Test 2 — DuplicateSubtreesWithUuids (the editor's undoable command path)
// mints ONE fresh instanceId for the whole copied instance, every copied
// member carries it, templateIds are preserved, and the source is untouched.
// Mirrors SceneEditorUI::DuplicateSelectionCommand: count the canonical
// subtree, reserve exactly that many known UUIDs, pass them in.
//
// Fault for red: delete the MintCopiedPrefabLinks call inside
// SceneManager::DuplicateSubtreesWithUuids — the duplicated members then keep
// the source's instanceId and CHECK(dupRootId != srcInstanceId) fails -> RED.
// Revert -> GREEN.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: the command duplicate path mints a fresh instanceId for the copied instance")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_s4_dup_uuids");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;

    const auto* srcRoot = reg.try_get<PrefabMemberComponent>(rootHandle);
    REQUIRE(srcRoot);
    const UUID srcInstanceId = srcRoot->instanceId;
    REQUIRE(srcInstanceId != UUID::Nil());
    const auto rootUuid = reg.get<EntityIdComponent>(rootHandle).id;

    // Host pattern (SceneEditorUI.cpp:401-404): count, reserve, pass known.
    auto count = f.manager.CountCanonicalSubtreeEntities({ rootUuid });
    REQUIRE(count.IsOk());
    REQUIRE(count.value == 2);
    const auto known = f.manager.ReserveKnownUuids(count.value);
    REQUIRE(known.size() == 2);

    auto dup = f.manager.DuplicateSubtreesWithUuids({ rootUuid }, known);
    REQUIRE(dup.mutation.success);
    REQUIRE_FALSE(dup.mutation.recoveryWarning.has_value());
    REQUIRE(dup.createdRoots.size() == 1);
    const auto dupRootUuid = dup.createdRoots.front();
    REQUIRE(dupRootUuid != rootUuid);

    const auto dupEntities = S4SubtreeEntities(f.manager, dupRootUuid);
    REQUIRE(dupEntities.size() == 2);

    const auto dupRootId = S4MemberInstanceId(f.manager, dupEntities[0]);
    const auto dupChildId = S4MemberInstanceId(f.manager, dupEntities[1]);
    CHECK(dupRootId != srcInstanceId);
    CHECK(dupChildId == dupRootId);

    const auto* dupPic =
        reg.try_get<PrefabInstanceComponent>(dupEntities[0]);
    REQUIRE(dupPic);
    CHECK(dupPic->instanceId == dupRootId);

    const auto* srcChild = reg.try_get<PrefabMemberComponent>(childHandle);
    REQUIRE(srcChild);
    CHECK(S4MemberTemplateId(f.manager, dupEntities[1]) == srcChild->templateId);
    CHECK(S4MemberTemplateId(f.manager, dupEntities[0]) == srcRoot->templateId);

    const auto* srcRootAfter = reg.try_get<PrefabMemberComponent>(rootHandle);
    REQUIRE(srcRootAfter);
    CHECK(srcRootAfter->instanceId == srcInstanceId);

    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Test 3 — the ordinary non-command paste path (PasteSubtreesFrom) mints ONE
// fresh instanceId for the whole pasted instance. The clipboard document is a
// whole-scene clone (SceneSerializer::CloneInMemory), exactly as
// EditorSceneState::Copy fills the clipboard. Root UUIDs resolve AGAINST THE
// CLIPBOARD, not the live scene — the fixture's instance lives in both, so the
// same UUID works. templateIds are preserved and neither the live source nor
// the clipboard source is mutated.
//
// Fault for red: delete the MintCopiedPrefabLinks call inside
// SceneManager::PasteSubtreesFrom — the pasted members then keep the
// clipboard's instanceId and CHECK(pastedRootId != srcInstanceId) fails ->
// RED. Revert -> GREEN.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: a pasted instance gets a fresh instanceId shared by all copied members")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_s4_paste_ordinary");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;

    const auto* srcRoot = reg.try_get<PrefabMemberComponent>(rootHandle);
    const auto* srcChild = reg.try_get<PrefabMemberComponent>(childHandle);
    REQUIRE(srcRoot);
    REQUIRE(srcChild);
    const UUID srcInstanceId = srcRoot->instanceId;
    REQUIRE(srcInstanceId != UUID::Nil());
    CHECK(srcChild->instanceId == srcInstanceId);
    const auto rootUuid = reg.get<EntityIdComponent>(rootHandle).id;

    // Clipboard = a whole-scene clone of the authoring doc (mirrors
    // EditorSceneState::Copy -> ClipboardDocument). UUIDs survive cloning.
    SceneDocument clipboard;
    DeterministicUuidProvider idsClip;
    clipboard.SetUuidProvider(&idsClip);
    Error cloneErr;
    REQUIRE(SceneSerializer::CloneInMemory(f.manager.AuthoringDoc(), clipboard, cloneErr));
    const auto clipRoot = clipboard.FindByUuid(rootUuid);
    REQUIRE(static_cast<uint32_t>(clipRoot) != static_cast<uint32_t>(entt::null));
    const auto* clipMember =
        clipboard.ecs.registry.try_get<PrefabMemberComponent>(clipRoot);
    REQUIRE(clipMember);
    CHECK(clipMember->instanceId == srcInstanceId); // clipboard carries the live id

    const auto result = f.manager.PasteSubtreesFrom(clipboard, { rootUuid });
    REQUIRE(result.success);
    REQUIRE_FALSE(result.recoveryWarning.has_value());
    REQUIRE(result.affectedEntities.size() == 1);
    const auto pastedRootUuid = result.affectedEntities.front();
    REQUIRE(pastedRootUuid != rootUuid);

    const auto pastedEntities = S4SubtreeEntities(f.manager, pastedRootUuid);
    REQUIRE(pastedEntities.size() == 2);

    const auto pastedRootId = S4MemberInstanceId(f.manager, pastedEntities[0]);
    const auto pastedChildId = S4MemberInstanceId(f.manager, pastedEntities[1]);
    CHECK(pastedRootId != srcInstanceId);
    CHECK(pastedChildId == pastedRootId);

    const auto* pastedPic =
        reg.try_get<PrefabInstanceComponent>(pastedEntities[0]);
    REQUIRE(pastedPic);
    CHECK(pastedPic->instanceId == pastedRootId);

    CHECK(S4MemberTemplateId(f.manager, pastedEntities[1]) == srcChild->templateId);
    CHECK(S4MemberTemplateId(f.manager, pastedEntities[0]) == srcRoot->templateId);

    // Neither the live source nor the clipboard source is mutated.
    const auto* srcRootAfter = reg.try_get<PrefabMemberComponent>(rootHandle);
    REQUIRE(srcRootAfter);
    CHECK(srcRootAfter->instanceId == srcInstanceId);
    const auto* clipMemberAfter =
        clipboard.ecs.registry.try_get<PrefabMemberComponent>(clipRoot);
    REQUIRE(clipMemberAfter);
    CHECK(clipMemberAfter->instanceId == srcInstanceId);

    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Test 4 — the UUID-aware paste path (PasteSubtreesWithUuids, the editor's
// undoable PasteCommand) mints ONE fresh instanceId for the whole pasted
// instance. Mirrors SceneEditorUI::PasteCommand: count the clipboard subtree
// by walking the clipboard document itself (clipboard roots live in the
// clipboard, not the live scene, so CountCanonicalSubtreeEntities would fail),
// reserve exactly that many known UUIDs, pass them in.
//
// Fault for red: delete the MintCopiedPrefabLinks call inside
// SceneManager::PasteSubtreesWithUuids — the pasted members then keep the
// clipboard's instanceId and CHECK(pastedRootId != srcInstanceId) fails ->
// RED. Revert -> GREEN.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: the command paste path mints a fresh instanceId for the pasted instance")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_s4_paste_uuids");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;

    const auto* srcRoot = reg.try_get<PrefabMemberComponent>(rootHandle);
    REQUIRE(srcRoot);
    const UUID srcInstanceId = srcRoot->instanceId;
    REQUIRE(srcInstanceId != UUID::Nil());
    const auto rootUuid = reg.get<EntityIdComponent>(rootHandle).id;

    SceneDocument clipboard;
    DeterministicUuidProvider idsClip;
    clipboard.SetUuidProvider(&idsClip);
    Error cloneErr;
    REQUIRE(SceneSerializer::CloneInMemory(f.manager.AuthoringDoc(), clipboard, cloneErr));

    // Host pattern (SceneEditorUI.cpp:438-446): count the clipboard subtree by
    // walking the clipboard document, then reserve known UUIDs for the paste.
    std::size_t count = 0;
    {
        const auto clipRoot = clipboard.FindByUuid(rootUuid);
        REQUIRE(static_cast<uint32_t>(clipRoot) != static_cast<uint32_t>(entt::null));
        std::vector<entt::entity> subtree;
        SceneHierarchy::CollectSubtreePreOrder(
            clipboard.ecs.registry, clipRoot, subtree);
        count = subtree.size();
    }
    REQUIRE(count == 2);
    const auto known = f.manager.ReserveKnownUuids(count);
    REQUIRE(known.size() == 2);

    auto paste = f.manager.PasteSubtreesWithUuids(
        clipboard, { rootUuid }, std::nullopt, known);
    REQUIRE(paste.mutation.success);
    REQUIRE_FALSE(paste.mutation.recoveryWarning.has_value());
    REQUIRE(paste.createdRoots.size() == 1);
    const auto pastedRootUuid = paste.createdRoots.front();
    REQUIRE(pastedRootUuid != rootUuid);

    const auto pastedEntities = S4SubtreeEntities(f.manager, pastedRootUuid);
    REQUIRE(pastedEntities.size() == 2);

    const auto pastedRootId = S4MemberInstanceId(f.manager, pastedEntities[0]);
    const auto pastedChildId = S4MemberInstanceId(f.manager, pastedEntities[1]);
    CHECK(pastedRootId != srcInstanceId);
    CHECK(pastedChildId == pastedRootId);

    const auto* pastedPic =
        reg.try_get<PrefabInstanceComponent>(pastedEntities[0]);
    REQUIRE(pastedPic);
    CHECK(pastedPic->instanceId == pastedRootId);

    const auto* srcChild = reg.try_get<PrefabMemberComponent>(childHandle);
    REQUIRE(srcChild);
    CHECK(S4MemberTemplateId(f.manager, pastedEntities[1]) == srcChild->templateId);
    CHECK(S4MemberTemplateId(f.manager, pastedEntities[0]) == srcRoot->templateId);

    const auto* srcRootAfter = reg.try_get<PrefabMemberComponent>(rootHandle);
    REQUIRE(srcRootAfter);
    CHECK(srcRootAfter->instanceId == srcInstanceId);

    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Test 5 — a copy of a diverged instance stays diverged (W3-D4). The live
// source member carries a non-empty override set; it survives the clipboard
// clone (Fix 8), gets copied verbatim into the paste, and MintCopiedPrefabLinks
// must leave it untouched while still minting a fresh instanceId. Covered on
// the editor paste path because that is the strongest discriminator for the
// shared helper: the override set must survive the FULL chain — authoring doc
// -> clipboard clone -> paste copy -> mint.
//
// Fault for red: in MintCopiedPrefabLinks, in the full-instance branch, clear
// each copied member's override vector when setting the fresh instanceId
// (SceneManager.cpp:200-201) — the pasted diverged member then has an empty
// set, so CHECK(pastedRootMember->overrides == srcRoot->overrides) fails ->
// RED. Revert -> GREEN.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: a pasted diverged instance keeps its overrides while minting a fresh instanceId")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_s4_paste_diverged");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;

    // Diverge the live source root member BEFORE cloning to the clipboard.
    auto* srcRoot = reg.try_get<PrefabMemberComponent>(rootHandle);
    REQUIRE(srcRoot);
    srcRoot->overrides = { S2Key("name"), S2Key("transform") };
    const auto* srcChild = reg.try_get<PrefabMemberComponent>(childHandle);
    REQUIRE(srcChild);
    const UUID srcInstanceId = srcRoot->instanceId;
    REQUIRE(srcInstanceId != UUID::Nil());
    const auto rootUuid = reg.get<EntityIdComponent>(rootHandle).id;

    SceneDocument clipboard;
    DeterministicUuidProvider idsClip;
    clipboard.SetUuidProvider(&idsClip);
    Error cloneErr;
    REQUIRE(SceneSerializer::CloneInMemory(f.manager.AuthoringDoc(), clipboard, cloneErr));
    const auto clipRoot = clipboard.FindByUuid(rootUuid);
    REQUIRE(static_cast<uint32_t>(clipRoot) != static_cast<uint32_t>(entt::null));
    const auto* clipMember = clipboard.ecs.registry.try_get<PrefabMemberComponent>(clipRoot);
    REQUIRE(clipMember);
    CHECK(clipMember->overrides.size() == 2); // clone carried the set (Fix 8)

    std::size_t count = 0;
    {
        std::vector<entt::entity> subtree;
        SceneHierarchy::CollectSubtreePreOrder(
            clipboard.ecs.registry, clipRoot, subtree);
        count = subtree.size();
    }
    REQUIRE(count == 2);
    const auto known = f.manager.ReserveKnownUuids(count);

    auto paste = f.manager.PasteSubtreesWithUuids(
        clipboard, { rootUuid }, std::nullopt, known);
    REQUIRE(paste.mutation.success);
    REQUIRE_FALSE(paste.mutation.recoveryWarning.has_value());
    REQUIRE(paste.createdRoots.size() == 1);
    const auto pastedRootUuid = paste.createdRoots.front();
    REQUIRE(pastedRootUuid != rootUuid);

    const auto pastedEntities = S4SubtreeEntities(f.manager, pastedRootUuid);
    REQUIRE(pastedEntities.size() == 2);

    // Fresh instance identity on the diverged copy...
    const auto pastedRootId = S4MemberInstanceId(f.manager, pastedEntities[0]);
    const auto pastedChildId = S4MemberInstanceId(f.manager, pastedEntities[1]);
    CHECK(pastedRootId != srcInstanceId);
    CHECK(pastedChildId == pastedRootId);
    const auto* pastedPic =
        reg.try_get<PrefabInstanceComponent>(pastedEntities[0]);
    REQUIRE(pastedPic);
    CHECK(pastedPic->instanceId == pastedRootId);

    // ...while the divergence is preserved VERBATIM: the pasted root member
    // carries exactly the source's override set, and templateId is untouched.
    const auto* pastedRootMember =
        reg.try_get<PrefabMemberComponent>(pastedEntities[0]);
    REQUIRE(pastedRootMember);
    CHECK(pastedRootMember->overrides == srcRoot->overrides);
    CHECK(S4MemberTemplateId(f.manager, pastedEntities[0]) == srcRoot->templateId);

    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Test 6 — the structural restore path is NOT a copy path. Undo/Redo
// re-creates an instance by reinstating the snapshot's recorded
// PrefabInstanceComponent/PrefabMemberComponent payloads VERBATIM
// (ApplySubtreeRecord, SceneManager.cpp:1874). The restored root and every
// member must carry the EXACT recorded instanceId — a fresh identity is
// never minted here (the snapshot's ids are the authoritative identity).
// templateIds and the override set must survive the round-trip too, so the
// restored link is internally coherent: members group under the same id, the
// root's PrefabInstanceComponent agrees, and the divergence is intact.
//
// Flow mirrors the command layer's Undo path: CaptureSubtreeSnapshot ->
// RemoveSubtreesExact -> RestoreSubtrees.
//
// Fault for red: in RestoreSubtrees, right after ApplySubtreeRecord
// (SceneManager.cpp:2533), mint a fresh instanceId onto every restored
// PrefabMemberComponent (e.g. overwrite its instanceId with ReserveKnownUuid).
// The restored members then no longer match the recorded id, so
// CHECK(restoredRootId == recordedId) fails -> RED. Revert -> GREEN. The
// fault lives ONLY on the restore path; instantiate/duplicate/paste are
// untouched.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: structural restore reinstates the exact recorded instanceId (no mint)")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_s4_restore");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;

    const auto* srcRoot = reg.try_get<PrefabMemberComponent>(rootHandle);
    const auto* srcChild = reg.try_get<PrefabMemberComponent>(childHandle);
    REQUIRE(srcRoot);
    REQUIRE(srcChild);
    const UUID recordedId = srcRoot->instanceId;
    REQUIRE(recordedId != UUID::Nil());
    CHECK(srcChild->instanceId == recordedId);
    const auto rootUuid = reg.get<EntityIdComponent>(rootHandle).id;
    const auto childUuid = reg.get<EntityIdComponent>(childHandle).id;

    // Diverge the source before capturing: the divergence must survive.
    auto* rootMemberLive = reg.try_get<PrefabMemberComponent>(rootHandle);
    REQUIRE(rootMemberLive);
    rootMemberLive->overrides = { S2Key("name"), S2Key("transform") };

    // Capture the instance to a structural snapshot (the Undo record).
    const auto snapshot = f.manager.CaptureSubtreeSnapshot({ rootUuid });
    REQUIRE(snapshot.entities.size() == 2);
    // The recorded member payloads carry the exact identity and divergence.
    const UUID recordedRootId = snapshot.entities[0].prefabMember.instanceId;
    const UUID recordedChildId = snapshot.entities[1].prefabMember.instanceId;
    REQUIRE(recordedRootId == recordedId);
    REQUIRE(recordedChildId == recordedId);
    REQUIRE(snapshot.entities[0].hasPrefabInstance);
    CHECK(snapshot.entities[0].prefabInstance.instanceId == recordedId);
    CHECK(snapshot.entities[0].prefabMember.overrides.size() == 2);

    // Undo: remove exactly what was captured.
    const auto removed = f.manager.RemoveSubtreesExact(snapshot);
    REQUIRE(removed.success);
    REQUIRE_FALSE(removed.recoveryWarning.has_value());
    REQUIRE(static_cast<uint32_t>(f.manager.FindEntityByUuid(rootUuid)) ==
            static_cast<uint32_t>(entt::null));

    // Redo: restore reinstates the recorded state.
    const auto restored = f.manager.RestoreSubtrees(snapshot);
    REQUIRE(restored.success);
    REQUIRE_FALSE(restored.recoveryWarning.has_value());

    const auto restoredEntities = S4SubtreeEntities(f.manager, rootUuid);
    REQUIRE(restoredEntities.size() == 2);

    // EXACT recorded identity — not a freshly minted one.
    const auto restoredRootId = S4MemberInstanceId(f.manager, restoredEntities[0]);
    const auto restoredChildId = S4MemberInstanceId(f.manager, restoredEntities[1]);
    CHECK(restoredRootId == recordedId);
    CHECK(restoredChildId == recordedId);

    // The restored link is internally coherent: root PIC agrees, templateIds
    // are preserved, and the divergence survived verbatim.
    const auto* restoredPic =
        reg.try_get<PrefabInstanceComponent>(restoredEntities[0]);
    REQUIRE(restoredPic);
    CHECK(restoredPic->instanceId == recordedId);
    CHECK(S4MemberTemplateId(f.manager, restoredEntities[1]) == srcChild->templateId);
    CHECK(S4MemberTemplateId(f.manager, restoredEntities[0]) == srcRoot->templateId);
    const auto* restoredRootMember =
        reg.try_get<PrefabMemberComponent>(restoredEntities[0]);
    REQUIRE(restoredRootMember);
    CHECK(restoredRootMember->overrides == rootMemberLive->overrides);

    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Test 7 — a PARTIAL copy (prefab member(s) present, NO PrefabInstanceComponent
// root) is converted into ordinary entities, not fabricated into an instance.
// Driven through the editor's UUID-aware duplicate path
// (DuplicateSubtreesWithUuids) with only the member subtree selected, exactly
// as the user selecting a member in the Outliner and duplicating it. The
// copied member must have BOTH prefab components stripped — no
// PrefabInstanceComponent, no PrefabMemberComponent, therefore no retained
// override payload — the mutation succeeds, and a recoveryWarning with the
// InvalidHierarchy diagnostic is surfaced (SceneManager.cpp:3488-3496).
//
// Fault for red: delete the two removal calls in MintCopiedPrefabLinks'
// partial branch (SceneManager.cpp:220-226) — the copied member then keeps
// its PrefabMemberComponent, so CHECK(copied has no PrefabMemberComponent)
// fails -> RED. Revert -> GREEN.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: a partial copy strips prefab links and surfaces a recovery warning")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_s4_partial");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;

    // Diverge the member we will copy: a non-empty override payload that must
    // NOT survive as a fabricated link.
    auto* childMember = reg.try_get<PrefabMemberComponent>(childHandle);
    REQUIRE(childMember);
    childMember->overrides = { S2Key("script") };
    const auto childUuid = reg.get<EntityIdComponent>(childHandle).id;

    // Select ONLY the member subtree (the child), not the instance root.
    auto count = f.manager.CountCanonicalSubtreeEntities({ childUuid });
    REQUIRE(count.IsOk());
    REQUIRE(count.value == 1);
    const auto known = f.manager.ReserveKnownUuids(count.value);
    REQUIRE(known.size() == 1);

    auto dup = f.manager.DuplicateSubtreesWithUuids({ childUuid }, known);
    REQUIRE(dup.mutation.success);
    // The operation must succeed AND surface the recovery warning — never a
    // silent success, never a hard failure.
    REQUIRE(dup.mutation.recoveryWarning.has_value());
    CHECK(dup.mutation.recoveryWarning->code == rt2::core::Error::InvalidHierarchy);
    REQUIRE(dup.createdRoots.size() == 1);

    const auto copiedUuid = dup.createdRoots.front();
    REQUIRE(copiedUuid != childUuid);
    const auto copiedEntity = f.manager.FindEntityByUuid(copiedUuid);
    REQUIRE(static_cast<uint32_t>(copiedEntity) != static_cast<uint32_t>(entt::null));

    // BOTH prefab components are gone: an ordinary entity. No fabricated
    // instance root, no retained override payload (the member is gone).
    CHECK_FALSE(reg.all_of<PrefabInstanceComponent>(copiedEntity));
    CHECK_FALSE(reg.all_of<PrefabMemberComponent>(copiedEntity));

    // The source member is untouched — still a member, still diverged.
    const auto* childAfter = reg.try_get<PrefabMemberComponent>(childHandle);
    REQUIRE(childAfter);
    CHECK(childAfter->overrides == childMember->overrides);

    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Test 8 — a MULTI-ROOT duplicate mints ONE fresh instanceId PER COPIED
// SUBTREE (not one id, not one per member). Two complete prefab instances are
// duplicated in one UUID-aware editor operation; each copied subtree gets its
// own fresh non-nil instanceId, the two ids differ from each other and from
// both source ids, every member agrees with its own copied root, and
// templateIds plus override sets remain intact.
//
// Fault for red: in MintCopiedPrefabLinks (SceneManager.cpp:195), replace
// `const UUID fresh = mint()` with a single id minted once and reused for
// every root — the two copied subtrees then SHARE one instanceId, so
// CHECK(copiedARootId != copiedBRootId) fails -> RED. Revert -> GREEN.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: a multi-root copy mints one distinct fresh instanceId per subtree")
{
    S2Fixture f;
    const auto dirA = S2UniqueTempDir("p8w3_s4_multi_a");
    const auto dirB = S2UniqueTempDir("p8w3_s4_multi_b");
    const auto [rootA, childA] = f.MakeInstance(dirA);
    const auto [rootB, childB] = f.MakeInstance(dirB);
    auto& reg = f.manager.GetECS().registry;

    const auto* srcRootA = reg.try_get<PrefabMemberComponent>(rootA);
    const auto* srcRootB = reg.try_get<PrefabMemberComponent>(rootB);
    const auto* srcChildA = reg.try_get<PrefabMemberComponent>(childA);
    const auto* srcChildB = reg.try_get<PrefabMemberComponent>(childB);
    REQUIRE(srcRootA);
    REQUIRE(srcRootB);
    REQUIRE(srcChildA);
    REQUIRE(srcChildB);
    const UUID srcAId = srcRootA->instanceId;
    const UUID srcBId = srcRootB->instanceId;
    REQUIRE(srcAId != UUID::Nil());
    REQUIRE(srcBId != UUID::Nil());
    REQUIRE(srcAId != srcBId); // the two source instances are distinct
    const auto rootAUuid = reg.get<EntityIdComponent>(rootA).id;
    const auto rootBUuid = reg.get<EntityIdComponent>(rootB).id;

    // Diverge both roots before copying: divergences must survive verbatim.
    auto* rootAMember = reg.try_get<PrefabMemberComponent>(rootA);
    auto* rootBMember = reg.try_get<PrefabMemberComponent>(rootB);
    REQUIRE(rootAMember);
    REQUIRE(rootBMember);
    rootAMember->overrides = { S2Key("name") };
    rootBMember->overrides = { S2Key("light") };

    // One UUID-aware duplicate operation for both roots.
    auto count = f.manager.CountCanonicalSubtreeEntities({ rootAUuid, rootBUuid });
    REQUIRE(count.IsOk());
    REQUIRE(count.value == 4);
    const auto known = f.manager.ReserveKnownUuids(count.value);
    REQUIRE(known.size() == 4);

    auto dup = f.manager.DuplicateSubtreesWithUuids({ rootAUuid, rootBUuid }, known);
    REQUIRE(dup.mutation.success);
    REQUIRE_FALSE(dup.mutation.recoveryWarning.has_value());
    REQUIRE(dup.createdRoots.size() == 2);

    // Resolve the two copied roots.
    const auto copiedAUuid = dup.createdRoots[0];
    const auto copiedBUuid = dup.createdRoots[1];
    REQUIRE(copiedAUuid != copiedBUuid);
    const auto copiedA = S4SubtreeEntities(f.manager, copiedAUuid);
    const auto copiedB = S4SubtreeEntities(f.manager, copiedBUuid);
    REQUIRE(copiedA.size() == 2);
    REQUIRE(copiedB.size() == 2);

    // Each copied subtree carries its own FRESH id.
    const auto copiedARootId = S4MemberInstanceId(f.manager, copiedA[0]);
    const auto copiedAChildId = S4MemberInstanceId(f.manager, copiedA[1]);
    const auto copiedBRootId = S4MemberInstanceId(f.manager, copiedB[0]);
    const auto copiedBChildId = S4MemberInstanceId(f.manager, copiedB[1]);
    // Fresh: differ from every source id.
    CHECK(copiedARootId != srcAId);
    CHECK(copiedARootId != srcBId);
    CHECK(copiedBRootId != srcAId);
    CHECK(copiedBRootId != srcBId);
    // Distinct from each other: per-subtree mints.
    CHECK(copiedARootId != copiedBRootId);
    CHECK(copiedARootId != UUID::Nil());
    CHECK(copiedBRootId != UUID::Nil());
    // Internally coherent: members agree with their own copied root.
    CHECK(copiedAChildId == copiedARootId);
    CHECK(copiedBChildId == copiedBRootId);
    CHECK(copiedAChildId != copiedBRootId);
    CHECK(copiedBChildId != copiedARootId);

    // Root PrefabInstanceComponents agree with their own subtree's id.
    const auto* copiedAPic = reg.try_get<PrefabInstanceComponent>(copiedA[0]);
    const auto* copiedBPic = reg.try_get<PrefabInstanceComponent>(copiedB[0]);
    REQUIRE(copiedAPic);
    REQUIRE(copiedBPic);
    CHECK(copiedAPic->instanceId == copiedARootId);
    CHECK(copiedBPic->instanceId == copiedBRootId);

    // templateIds and divergences survive verbatim.
    CHECK(S4MemberTemplateId(f.manager, copiedA[1]) == srcChildA->templateId);
    CHECK(S4MemberTemplateId(f.manager, copiedA[0]) == srcRootA->templateId);
    CHECK(S4MemberTemplateId(f.manager, copiedB[1]) == srcChildB->templateId);
    CHECK(S4MemberTemplateId(f.manager, copiedB[0]) == srcRootB->templateId);
    const auto* copiedARootMember =
        reg.try_get<PrefabMemberComponent>(copiedA[0]);
    const auto* copiedBRootMember =
        reg.try_get<PrefabMemberComponent>(copiedB[0]);
    REQUIRE(copiedARootMember);
    REQUIRE(copiedBRootMember);
    CHECK(copiedARootMember->overrides == rootAMember->overrides);
    CHECK(copiedBRootMember->overrides == rootBMember->overrides);

    std::filesystem::remove_all(dirA);
    std::filesystem::remove_all(dirB);
}
