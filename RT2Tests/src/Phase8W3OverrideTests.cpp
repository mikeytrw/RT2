#include <doctest/doctest.h>

#include "PersistedComponents.h"
#include "PrefabComponentKey.h"
#include "PrefabSerializer.h"
#include "EditorCommandHistory.h"
#include "EditorSceneState.h"
#include "EditorStructuralCommands.h"
#include "SceneManager.h"
#include "SceneHierarchy.h"
#include "SceneSerializer.h"
#include "SceneSerializerTestSupport.h"
#include "SubtreeSnapshot.h"
#include "AssetIdentity.h"
#include "core/Error.h"
#include "core/UUID.h"
#include "json.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
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
// group (W3-D8). S4 runs the plan -> reserve -> apply pipeline (PlanCopiedPrefabLinks,
// ReserveValidEntityUuids/ReserveFreshInstanceId, ApplyCopiedPrefabLinks) on
// every copy path: each copied subtree reserves ONE fresh instanceId and APPLY
// pushes it onto every copied PrefabMemberComponent, leaving templateIds and
// override vectors untouched (a copy of a diverged instance stays diverged,
// W3-D4).
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
// All four copy-shaped paths chain the SAME plan -> reserve -> apply pipeline:
// PLAN (PlanCopiedPrefabLinks, SceneManager.cpp:214) classifies the copied
// forest into complete instance groups from the ACTUAL copied instance roots —
// a complete instance nested under an ordinary container or under another
// copied root is a first-class instance of its own and is reminted
// independently, never merged into the enclosing group; member fragments whose
// original instanceId has no copied root are classified as orphan fragments
// (never a fabricated instance, test 7). RESERVE (ReserveValidEntityUuids +
// ReserveFreshInstanceId, SceneManager.cpp:304) draws ONE validated
// non-colliding entity UUID per copied entity and ONE fresh instanceId per
// COMPLETE COPIED INSTANCE, ALL BEFORE any destination mutation. APPLY
// (ApplyCopiedPrefabLinks, SceneManager.cpp:334) then stamps each group's
// fresh instanceId onto every copied PrefabMemberComponent and
// PrefabInstanceComponent belonging to that instance's original group and
// strips the orphan fragments, leaving templateIds and override vectors
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
// collide with the plan/reserve/apply prefab-link pipeline. Partial copies (test 7) strip both
// prefab components and surface a recoveryWarning instead of fabricating an
// instance root. Multi-root copies (test 8) mint per subtree, never one shared
// id and never one per member.
//
// Tests 9-12 pin the forest-wide classification fix (S4 review fix 1): an
// ordinary folder holding a complete instance duplicates (9) and pastes (10)
// with the folder staying ordinary and the instance gaining ONE fresh coherent
// id; a mixed tree with a complete instance under an ordinary root plus an
// orphan member fragment (11) preserves and remints the complete instance and
// strips only the fragment (with an explicit 1-fragment recovery warning); two
// distinct complete instance roots — one nested under a member of the other
// (12) — each receive their own distinct fresh id rather than being merged.
//
// Discrimination faults (recorded in the verification report), per test:
//   test 1 fault: delete the ApplyCopiedPrefabLinks call inside
//   SceneManager::DuplicateSubtrees — the copied member then keeps the
//   source's instanceId, so CHECK(dupRootId != srcInstanceId) fails -> RED.
//   Revert -> GREEN.
//   test 2 fault: delete the ApplyCopiedPrefabLinks call inside
//   SceneManager::DuplicateSubtreesWithUuids — same failure on the editor's
//   command path -> RED. Revert -> GREEN.
//   test 3 fault: delete the ApplyCopiedPrefabLinks call inside
//   SceneManager::PasteSubtreesFrom — the pasted member then keeps the
//   clipboard's instanceId, so CHECK(pastedRootId != srcInstanceId) fails ->
//   RED. Revert -> GREEN.
//   test 4 fault: delete the ApplyCopiedPrefabLinks call inside
//   SceneManager::PasteSubtreesWithUuids — same failure on the editor's paste
//   path -> RED. Revert -> GREEN.
//   test 5 fault: in ApplyCopiedPrefabLinks, in the full-instance branch,
//   clear each copied member's override vector when setting the fresh
//   instanceId — the pasted diverged member then has an empty set, so
//   CHECK(pastedRootMember->overrides == srcRoot->overrides) fails -> RED.
//   Revert -> GREEN.
//   test 6 fault: in RestoreSubtrees, right after ApplySubtreeRecord
//   (SceneManager.cpp:2533), mint a fresh instanceId onto every restored
//   PrefabMemberComponent — the restored members then diverge from the
//   recorded id, so CHECK(restoredRootId == recordedId) fails -> RED.
//   Revert -> GREEN.
//   test 7 fault: delete the two component removals in ApplyCopiedPrefabLinks'
//   orphan-fragment branch — the copied member then keeps its
//   PrefabMemberComponent, so CHECK(copied has no PrefabMemberComponent)
//   fails -> RED. Revert -> GREEN.
//   test 8 fault: in the DuplicateSubtrees instance-ID reservation loop,
//   reuse ONE reserved id for every group — the two copied subtrees then
//   share a single instanceId, so CHECK(copiedARootId != copiedBRootId)
//   fails -> RED. Revert -> GREEN.
//   test 9 fault: in PlanCopiedPrefabLinks, go back to classifying from the
//   SELECTED root copy (the old `isInstanceRoot` check): an ordinary folder
//   copy has no PrefabInstanceComponent, so the instance below it is treated
//   as a member fragment and BOTH prefab components are stripped from the copy
//   — CHECK(copiedRootId != UUID::Nil()) and CHECK(copied[s] is member) fail
//   -> RED. Revert -> GREEN.
//   test 10 fault: same selected-root regression on the command paste path —
//   the pasted instance under an ordinary folder gets stripped the same way ->
//   RED. Revert -> GREEN.
//   test 11 fault: same selected-root regression on a MIXED forest (ordinary
//   container holding BOTH a complete instance and an orphan member fragment)
//   — the complete instance's copy is stripped too (not reminted), so
//   CHECK(copiedCompleteRoot has PIC) fails -> RED. Revert -> GREEN.
//   test 12 fault: in PlanCopiedPrefabLinks, fall back to ONE id for the whole
//   selected subtree (the old full-instance branch): a nested complete
//   instance under the outer instance gets the OUTER's fresh id, so
//   CHECK(copiedNestedId != copiedOuterRootId) fails -> RED. Revert -> GREEN.
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
// Fault for red: delete the ApplyCopiedPrefabLinks call inside
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
// Fault for red: delete the ApplyCopiedPrefabLinks call inside
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
// Fault for red: delete the ApplyCopiedPrefabLinks call inside
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
// Fault for red: delete the ApplyCopiedPrefabLinks call inside
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
// clone (Fix 8), gets copied verbatim into the paste, and ApplyCopiedPrefabLinks
// must leave it untouched while still stamping a fresh instanceId. Covered on
// the editor paste path because that is the strongest discriminator for the
// shared helper: the override set must survive the FULL chain — authoring doc
// -> clipboard clone -> paste copy -> apply.
//
// Fault for red: in ApplyCopiedPrefabLinks, in the full-instance branch, clear
// each copied member's override vector when setting the fresh instanceId —
// the pasted diverged member then has an empty set, so
// CHECK(pastedRootMember->overrides == srcRoot->overrides) fails -> RED.
// Revert -> GREEN.
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

    // Snapshot the expected member payloads NOW, as VALUES, before any entity
    // is destroyed: RemoveSubtreesExact destroys the registry entities below,
    // and the restored components may reuse the same storage. Retaining EnTT
    // component pointers across entity destruction is undefined behaviour, so
    // every post-restore comparison must use these pre-copied values.
    const UUID expectedRootTemplateId = srcRoot->templateId;
    const UUID expectedChildTemplateId = srcChild->templateId;

    // Diverge the source before capturing: the divergence must survive.
    auto* rootMemberLive = reg.try_get<PrefabMemberComponent>(rootHandle);
    REQUIRE(rootMemberLive);
    rootMemberLive->overrides = { S2Key("name"), S2Key("transform") };
    const auto expectedRootOverrides = rootMemberLive->overrides; // a value copy

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
    // are preserved, and the divergence survived verbatim. All expected values
    // were copied before RemoveSubtreesExact destroyed the source entities.
    const auto* restoredPic =
        reg.try_get<PrefabInstanceComponent>(restoredEntities[0]);
    REQUIRE(restoredPic);
    CHECK(restoredPic->instanceId == recordedId);
    CHECK(S4MemberTemplateId(f.manager, restoredEntities[1]) == expectedChildTemplateId);
    CHECK(S4MemberTemplateId(f.manager, restoredEntities[0]) == expectedRootTemplateId);
    const auto* restoredRootMember =
        reg.try_get<PrefabMemberComponent>(restoredEntities[0]);
    REQUIRE(restoredRootMember);
    CHECK(restoredRootMember->overrides == expectedRootOverrides);

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
// Fault for red: delete the two removal calls in ApplyCopiedPrefabLinks'
// orphan-fragment branch — the copied member then keeps its
// PrefabMemberComponent, so CHECK(copied has no PrefabMemberComponent)
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
// Fault for red: in the DuplicateSubtrees instance-ID reservation loop, reuse
// ONE reserved id for every group — the two copied subtrees then SHARE one
// instanceId, so CHECK(copiedARootId != copiedBRootId) fails -> RED. Revert
// -> GREEN.
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

// ---------------------------------------------------------------------------
// Test 9 — the S4 review fix 1 shape, on the editor's UUID-aware duplicate
// path: an ordinary container (Folder) holding a COMPLETE instance. The copied
// forest must be classified from the ACTUAL instance root inside it, so the
// folder copy stays ordinary and the contained instance is preserved and
// reminted as ONE group — not stripped as if it were a member fragment.
//
// Fault for red (old selected-root classification): in PlanCopiedPrefabLinks,
// gate classification on the SELECTED root copy (the old `isInstanceRoot`
// check). The folder copy has no PrefabInstanceComponent, so the whole tree is
// treated as a member fragment and BOTH prefab components are stripped from
// every copy — CHECK(copiedPic->instanceId != UUID::Nil()) fails -> RED.
// Revert -> GREEN.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: UUID-aware duplicate of an ordinary folder preserves and remints its contained instance")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_s4_folder_dup");
    const auto folderUuid = f.CreateEmpty("Folder");
    REQUIRE(folderUuid != UUID::Nil());
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;

    const auto* srcRoot = reg.try_get<PrefabMemberComponent>(rootHandle);
    const auto* srcChild = reg.try_get<PrefabMemberComponent>(childHandle);
    REQUIRE(srcRoot);
    REQUIRE(srcChild);
    const UUID srcInstanceId = srcRoot->instanceId;
    REQUIRE(srcInstanceId != UUID::Nil());

    // Nest the complete instance UNDER the ordinary folder (editor-supported).
    const auto rootUuid = reg.get<EntityIdComponent>(rootHandle).id;
    REQUIRE(f.manager.Reparent({ rootUuid }, folderUuid, ReparentMode::PreserveLocal).success);

    auto count = f.manager.CountCanonicalSubtreeEntities({ folderUuid });
    REQUIRE(count.IsOk());
    REQUIRE(count.value == 3); // folder + instance root + instance child
    const auto known = f.manager.ReserveKnownUuids(count.value);
    REQUIRE(known.size() == 3);

    auto dup = f.manager.DuplicateSubtreesWithUuids({ folderUuid }, known);
    REQUIRE(dup.mutation.success);
    REQUIRE_FALSE(dup.mutation.recoveryWarning.has_value());
    REQUIRE(dup.createdRoots.size() == 1);
    const auto copiedFolderUuid = dup.createdRoots.front();

    const auto copied = S4SubtreeEntities(f.manager, copiedFolderUuid);
    REQUIRE(copied.size() == 3);

    // The copied folder stays ORDINARY — no fabricated prefab link.
    CHECK_FALSE(reg.all_of<PrefabInstanceComponent>(copied[0]));
    CHECK_FALSE(reg.all_of<PrefabMemberComponent>(copied[0]));

    // The contained complete instance is preserved and reminted as ONE group.
    const auto* copiedPic = reg.try_get<PrefabInstanceComponent>(copied[1]);
    REQUIRE(copiedPic);
    REQUIRE(copiedPic->instanceId != UUID::Nil());
    CHECK(copiedPic->instanceId != srcInstanceId);
    CHECK(S4MemberInstanceId(f.manager, copied[1]) == copiedPic->instanceId);
    CHECK(S4MemberInstanceId(f.manager, copied[2]) == copiedPic->instanceId);
    CHECK(S4MemberTemplateId(f.manager, copied[2]) == srcChild->templateId);
    CHECK(S4MemberTemplateId(f.manager, copied[1]) == srcRoot->templateId);

    // Source untouched.
    const auto* srcRootAfter = reg.try_get<PrefabMemberComponent>(rootHandle);
    REQUIRE(srcRootAfter);
    CHECK(srcRootAfter->instanceId == srcInstanceId);

    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Test 10 — the same shape on the editor's UUID-aware PASTE path
// (PasteSubtreesWithUuids).
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: UUID-aware paste of an ordinary folder preserves and remints its contained instance")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_s4_folder_paste");
    const auto folderUuid = f.CreateEmpty("Folder");
    REQUIRE(folderUuid != UUID::Nil());
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;

    const auto* srcRoot = reg.try_get<PrefabMemberComponent>(rootHandle);
    REQUIRE(srcRoot);
    const UUID srcInstanceId = srcRoot->instanceId;
    REQUIRE(srcInstanceId != UUID::Nil());

    const auto rootUuid = reg.get<EntityIdComponent>(rootHandle).id;
    REQUIRE(f.manager.Reparent({ rootUuid }, folderUuid, ReparentMode::PreserveLocal).success);

    // Clipboard clone of the authoring doc (includes the folder tree).
    SceneDocument clipboard;
    DeterministicUuidProvider idsClip;
    clipboard.SetUuidProvider(&idsClip);
    Error cloneErr;
    REQUIRE(SceneSerializer::CloneInMemory(f.manager.AuthoringDoc(), clipboard, cloneErr));

    // Host pattern: walk the clipboard subtree to count.
    std::size_t count = 0;
    {
        const auto clipFolder = clipboard.FindByUuid(folderUuid);
        REQUIRE(static_cast<uint32_t>(clipFolder) != static_cast<uint32_t>(entt::null));
        std::vector<entt::entity> subtree;
        SceneHierarchy::CollectSubtreePreOrder(clipboard.ecs.registry, clipFolder, subtree);
        count = subtree.size();
    }
    REQUIRE(count == 3);
    const auto known = f.manager.ReserveKnownUuids(count);
    REQUIRE(known.size() == 3);

    auto paste = f.manager.PasteSubtreesWithUuids(
        clipboard, { folderUuid }, std::nullopt, known);
    REQUIRE(paste.mutation.success);
    REQUIRE_FALSE(paste.mutation.recoveryWarning.has_value());
    REQUIRE(paste.createdRoots.size() == 1);
    const auto pastedFolderUuid = paste.createdRoots.front();

    const auto pasted = S4SubtreeEntities(f.manager, pastedFolderUuid);
    REQUIRE(pasted.size() == 3);

    // The pasted folder stays ordinary; the contained instance is reminted.
    CHECK_FALSE(reg.all_of<PrefabInstanceComponent>(pasted[0]));
    CHECK_FALSE(reg.all_of<PrefabMemberComponent>(pasted[0]));
    const auto* pastedPic = reg.try_get<PrefabInstanceComponent>(pasted[1]);
    REQUIRE(pastedPic);
    REQUIRE(pastedPic->instanceId != UUID::Nil());
    CHECK(pastedPic->instanceId != srcInstanceId);
    CHECK(S4MemberInstanceId(f.manager, pasted[1]) == pastedPic->instanceId);
    CHECK(S4MemberInstanceId(f.manager, pasted[2]) == pastedPic->instanceId);

    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Test 11 — a MIXED copied forest: an ordinary folder holding BOTH a complete
// instance AND an orphan member fragment. The complete instance is preserved
// and reminted as one group; only the orphan fragment (instance B's child,
// whose root is NOT part of the copied set) is stripped of both prefab
// components. The recovery warning must surface with an explicit count of 1
// orphaned member.
//
// Fault for red (old selected-root classification): the folder copy has no
// PrefabInstanceComponent, so the old code treats the WHOLE tree as a member
// fragment and strips the complete instance copy too — CHECK(copiedAPic has
// PIC) fails -> RED. Revert -> GREEN.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: a mixed forest keeps its complete instance and strips only the orphan member fragment")
{
    S2Fixture f;
    const auto dirA = S2UniqueTempDir("p8w3_s4_mixed_a");
    const auto dirB = S2UniqueTempDir("p8w3_s4_mixed_b");
    const auto folderUuid = f.CreateEmpty("Folder");
    REQUIRE(folderUuid != UUID::Nil());
    const auto [rootA, childA] = f.MakeInstance(dirA);
    const auto [rootB, childB] = f.MakeInstance(dirB);
    auto& reg = f.manager.GetECS().registry;

    const auto* srcRootA = reg.try_get<PrefabMemberComponent>(rootA);
    REQUIRE(srcRootA);
    const UUID srcAId = srcRootA->instanceId;
    REQUIRE(srcAId != UUID::Nil());
    const auto* srcChildB = reg.try_get<PrefabMemberComponent>(childB);
    REQUIRE(srcChildB);

    const auto rootAUuid = reg.get<EntityIdComponent>(rootA).id;
    const auto childBUuid = reg.get<EntityIdComponent>(childB).id;

    // Complete instance A nested under the folder.
    REQUIRE(f.manager.Reparent({ rootAUuid }, folderUuid, ReparentMode::PreserveLocal).success);
    // Orphan fragment: ONLY instance B's child member lands under the folder;
    // instance B's root stays OUTSIDE the copied forest.
    REQUIRE(f.manager.Reparent({ childBUuid }, folderUuid, ReparentMode::PreserveLocal).success);

    auto count = f.manager.CountCanonicalSubtreeEntities({ folderUuid });
    REQUIRE(count.IsOk());
    REQUIRE(count.value == 4); // folder + rootA + childA + childB
    const auto known = f.manager.ReserveKnownUuids(count.value);
    REQUIRE(known.size() == 4);

    auto dup = f.manager.DuplicateSubtreesWithUuids({ folderUuid }, known);
    REQUIRE(dup.mutation.success);
    // The orphan fragment is stripped -> one recovery warning, explicit count.
    REQUIRE(dup.mutation.recoveryWarning.has_value());
    CHECK(dup.mutation.recoveryWarning->code == rt2::core::Error::InvalidHierarchy);
    CHECK(dup.mutation.recoveryWarning->detail.find("1 copied member") != std::string::npos);
    REQUIRE(dup.createdRoots.size() == 1);
    const auto copiedFolderUuid = dup.createdRoots.front();

    const auto copied = S4SubtreeEntities(f.manager, copiedFolderUuid);
    REQUIRE(copied.size() == 4);

    // The folder copy stays ordinary.
    CHECK_FALSE(reg.all_of<PrefabInstanceComponent>(copied[0]));
    CHECK_FALSE(reg.all_of<PrefabMemberComponent>(copied[0]));

    // The COMPLETE instance A copy is preserved and reminted as one group.
    const auto* copiedAPic = reg.try_get<PrefabInstanceComponent>(copied[1]);
    REQUIRE(copiedAPic);
    REQUIRE(copiedAPic->instanceId != UUID::Nil());
    CHECK(copiedAPic->instanceId != srcAId);
    CHECK(S4MemberInstanceId(f.manager, copied[1]) == copiedAPic->instanceId);
    CHECK(S4MemberInstanceId(f.manager, copied[2]) == copiedAPic->instanceId);

    // The ORPHAN member fragment lost BOTH prefab components.
    CHECK_FALSE(reg.all_of<PrefabMemberComponent>(copied[3]));
    CHECK_FALSE(reg.all_of<PrefabInstanceComponent>(copied[3]));

    std::filesystem::remove_all(dirA);
    std::filesystem::remove_all(dirB);
}

// ---------------------------------------------------------------------------
// Test 12 — TWO distinct complete instance roots, one nested under the other
// (the inverse shape from the review): the outer instance-root tree contains
// a second complete instance. Each copied instance gets its OWN distinct fresh
// id — the nested copy is never merged into the outer's remint.
//
// Fault for red (old full-instance branch): fall back to minting ONE id for
// the whole selected subtree — the nested root copy then carries the OUTER's
// fresh id, so CHECK(copiedNestedId != copiedOuterId) fails -> RED.
// Revert -> GREEN.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: nested complete instances each receive their own distinct fresh instanceId")
{
    S2Fixture f;
    const auto dirA = S2UniqueTempDir("p8w3_s4_nested_a");
    const auto dirB = S2UniqueTempDir("p8w3_s4_nested_b");
    const auto [rootA, childA] = f.MakeInstance(dirA);
    const auto [rootB, childB] = f.MakeInstance(dirB);
    auto& reg = f.manager.GetECS().registry;

    const auto* srcRootA = reg.try_get<PrefabMemberComponent>(rootA);
    const auto* srcRootB = reg.try_get<PrefabMemberComponent>(rootB);
    REQUIRE(srcRootA);
    REQUIRE(srcRootB);
    const UUID srcAId = srcRootA->instanceId;
    const UUID srcBId = srcRootB->instanceId;
    REQUIRE(srcAId != srcBId);

    const auto rootAUuid = reg.get<EntityIdComponent>(rootA).id;
    const auto childAUuid = reg.get<EntityIdComponent>(childA).id;
    const auto rootBUuid = reg.get<EntityIdComponent>(rootB).id;

    // Nest instance B's ROOT under instance A's member subtree.
    REQUIRE(f.manager.Reparent({ rootBUuid }, childAUuid, ReparentMode::PreserveLocal).success);

    auto count = f.manager.CountCanonicalSubtreeEntities({ rootAUuid });
    REQUIRE(count.IsOk());
    REQUIRE(count.value == 4); // rootA + childA + rootB + childB
    const auto known = f.manager.ReserveKnownUuids(count.value);
    REQUIRE(known.size() == 4);

    auto dup = f.manager.DuplicateSubtreesWithUuids({ rootAUuid }, known);
    REQUIRE(dup.mutation.success);
    REQUIRE_FALSE(dup.mutation.recoveryWarning.has_value());
    REQUIRE(dup.createdRoots.size() == 1);

    const auto copied = S4SubtreeEntities(f.manager, dup.createdRoots.front());
    REQUIRE(copied.size() == 4);

    const auto* copiedOuterPic = reg.try_get<PrefabInstanceComponent>(copied[0]);
    const auto* copiedNestedRootPic = reg.try_get<PrefabInstanceComponent>(copied[2]);
    REQUIRE(copiedOuterPic);
    REQUIRE(copiedNestedRootPic);
    REQUIRE(copiedOuterPic->instanceId != UUID::Nil());
    REQUIRE(copiedNestedRootPic->instanceId != UUID::Nil());
    const UUID copiedOuterId = copiedOuterPic->instanceId;
    const UUID copiedNestedId = copiedNestedRootPic->instanceId;

    // Both reminted: differ from every source id AND differ from each other.
    CHECK(copiedOuterId != srcAId);
    CHECK(copiedNestedId != srcBId);
    CHECK(copiedOuterId != copiedNestedId);

    // Members agree with their own copied group.
    CHECK(S4MemberInstanceId(f.manager, copied[1]) == copiedOuterId); // A's child
    CHECK(S4MemberInstanceId(f.manager, copied[2]) == copiedNestedId); // B's root member
    CHECK(S4MemberInstanceId(f.manager, copied[3]) == copiedNestedId); // B's child

    std::filesystem::remove_all(dirA);
    std::filesystem::remove_all(dirB);
}

// ---------------------------------------------------------------------------
// Test 13 — malformed copied data with ambiguous group ownership: TWO copied
// instance roots claim the SAME original instanceId. PlanCopiedPrefabLinks
// does NOT silently guess — it diagnoses loudly (recovery warning naming the
// ambiguous group) and remints the shared group as ONE coherent fresh id, so
// the copies never keep the malformed source id and never split arbitrarily.
//
// Fault for red: skip the ambiguous-group detection (drop the ambiguousGroups
// counter) — the operation then succeeds WITHOUT the diagnosis -> CHECK(warning
// has_value) fails -> RED. Revert -> GREEN.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: duplicate roots sharing one original instanceId are diagnosed and kept as one reminted group")
{
    S2Fixture f;
    const auto dirA = S2UniqueTempDir("p8w3_s4_amb_a");
    const auto dirB = S2UniqueTempDir("p8w3_s4_amb_b");
    const auto folderUuid = f.CreateEmpty("Folder");
    REQUIRE(folderUuid != UUID::Nil());
    const auto [rootA, childA] = f.MakeInstance(dirA);
    const auto [rootB, childB] = f.MakeInstance(dirB);
    auto& reg = f.manager.GetECS().registry;

    const auto* srcRootA = reg.try_get<PrefabMemberComponent>(rootA);
    REQUIRE(srcRootA);
    const UUID srcAId = srcRootA->instanceId;
    REQUIRE(srcAId != UUID::Nil());

    // Malformed source: force instance B's components to claim instance A's id.
    reg.get<PrefabInstanceComponent>(rootB).instanceId = srcAId;
    reg.get<PrefabMemberComponent>(rootB).instanceId = srcAId;
    reg.get<PrefabMemberComponent>(childB).instanceId = srcAId;

    const auto rootAUuid = reg.get<EntityIdComponent>(rootA).id;
    const auto rootBUuid = reg.get<EntityIdComponent>(rootB).id;
    REQUIRE(f.manager.Reparent({ rootAUuid }, folderUuid, ReparentMode::PreserveLocal).success);
    REQUIRE(f.manager.Reparent({ rootBUuid }, folderUuid, ReparentMode::PreserveLocal).success);

    auto count = f.manager.CountCanonicalSubtreeEntities({ folderUuid });
    REQUIRE(count.IsOk());
    REQUIRE(count.value == 5); // folder + rootA + childA + rootB + childB
    const auto known = f.manager.ReserveKnownUuids(count.value);
    REQUIRE(known.size() == 5);

    auto dup = f.manager.DuplicateSubtreesWithUuids({ folderUuid }, known);
    REQUIRE(dup.mutation.success);
    // Loud diagnosis: never a silent success.
    REQUIRE(dup.mutation.recoveryWarning.has_value());
    CHECK(dup.mutation.recoveryWarning->code == rt2::core::Error::InvalidHierarchy);
    CHECK(dup.mutation.recoveryWarning->detail.find("multiple copied roots") != std::string::npos);
    REQUIRE(dup.createdRoots.size() == 1);

    const auto copied = S4SubtreeEntities(f.manager, dup.createdRoots.front());
    REQUIRE(copied.size() == 5);

    // Both copied roots share ONE fresh id (never the malformed source id).
    const auto* copiedAPic = reg.try_get<PrefabInstanceComponent>(copied[1]);
    const auto* copiedBPic = reg.try_get<PrefabInstanceComponent>(copied[3]);
    REQUIRE(copiedAPic);
    REQUIRE(copiedBPic);
    CHECK(copiedAPic->instanceId == copiedBPic->instanceId);
    CHECK(copiedAPic->instanceId != srcAId);
    CHECK(copiedAPic->instanceId != UUID::Nil());
    // All four members agree with the shared fresh id.
    CHECK(S4MemberInstanceId(f.manager, copied[1]) == copiedAPic->instanceId);
    CHECK(S4MemberInstanceId(f.manager, copied[2]) == copiedAPic->instanceId);
    CHECK(S4MemberInstanceId(f.manager, copied[3]) == copiedAPic->instanceId);
    CHECK(S4MemberInstanceId(f.manager, copied[4]) == copiedAPic->instanceId);

    std::filesystem::remove_all(dirA);
    std::filesystem::remove_all(dirB);
}

// ============================================================================
// Phase 8 W3, S4 — review fix 2, hostile UUID providers
// (implementation spec, docs/game-engine-development-plan.md "Phase 8 W3";
// S4 review finding 2). The copy/instantiate paths must reserve every fresh
// instanceId BEFORE any destination mutation, retry hostile provider output
// (nil, live-ID collision, operation-local duplicate) with a finite budget,
// and fail loudly and transactionally on exhaustion.
//
// Tests T14-T18 drive those guarantees with a SCRIPTED, call-logging provider:
// the provider queue is finite and its over-consumption fails the test loudly
// via REQUIRE, so "exactly N provider calls" is enforced by construction, not
// hand-waved. Every scenario uses caller-supplied entity UUIDs (WithUuids
// paths) or a pre-staged reserve (ordinary path) so the SCRIPT is consumed
// ONLY by the instance-ID reservation under test.
//
// Discrimination faults (recorded in the verification report), per test:
//   test T14 fault: in ReserveFreshInstanceId (SceneManager.cpp) delete the
//   `if (forbidden.count(lastAttempt) != 0) continue;` line — the FIRST draw
//   (the source's own instanceId) is then accepted, so the copied group claims
//   the SOURCE's identity and CHECK(copiedRootId == validId) fails -> RED.
//   Revert -> GREEN.
//   test T15 fault: in ReserveFreshInstanceId delete the
//   `if (!operationLocal.insert(lastAttempt).second) continue;` line — the
//   second draw (a repeat of the first group's fresh id) is accepted for a
//   second group, so two copied groups share one id and
//   CHECK(cA->instanceId != cB->instanceId) fails -> RED. Revert -> GREEN.
//   test T16 fault: set kFreshInstanceIdMaxAttempts to 2 (attempt-limit
//   bypass) — exhaustion stops after 2 provider draws, so the "exactly 16"
//   assertion fails and the operation partially consumed the hostile queue
//   -> RED on both the duplicate and the paste scenario. Revert -> GREEN.
//   test T17 fault: same attempt-limit bypass (kFreshInstanceIdMaxAttempts
//   = 2) — the success half is robbed of its 3 retries, failing the
//   successful instantiate -> RED. Revert -> GREEN.
//   test T18 fault: in SceneManager::DuplicateSubtrees move the instance-ID
//   reservation block ABOVE the `duplicateUuids = ReserveKnownUuids(...)`
//   staging line (provider-order swap) — the provider then draws the instance
//   id FIRST, so the staged entity UUIDs land shifted and
//   CHECK(copied[0].id == e0) fails -> RED. Revert -> GREEN.
// ============================================================================

namespace
{

// Finite scripted v4 queue + full call log. Fails loudly (doctest REQUIRE,
// propagating an exception through the manager into the test) if the code
// under test draws MORE ids than the script provides — that is how the exact
// provider-call expectations are enforced rather than assumed.
struct ScriptedUuidProvider final : IUuidProvider
{
    std::vector<UUID> script;
    std::size_t cursor = 0;
    std::vector<UUID> log;

    UUID CreateV4() override
    {
        REQUIRE(cursor < script.size());
        const auto value = script[cursor++];
        log.push_back(value);
        return value;
    }
};

// Everything a failing operation must leave unchanged: entity/component
// counts, the entity UUID index, the authoring revision (NotifyAuthoringChanged
// bumps it), and the resource-table sizes. Equality is the transactional
// zero-mutation proof.
struct S4SceneSnapshot
{
    std::size_t entities = 0;
    std::size_t uuidIndex = 0;
    std::size_t hierarchy = 0;
    std::size_t pic = 0;
    std::size_t pmic = 0;
    std::size_t meshes = 0;
    std::size_t materials = 0;
    std::size_t textures = 0;
    uint64_t revision = 0;
    uint64_t docGen = 0;
    uint64_t resourceGen = 0;

    friend bool operator==(const S4SceneSnapshot& a, const S4SceneSnapshot& b)
    {
        return a.entities == b.entities && a.uuidIndex == b.uuidIndex &&
               a.hierarchy == b.hierarchy && a.pic == b.pic &&
               a.pmic == b.pmic && a.meshes == b.meshes &&
               a.materials == b.materials && a.textures == b.textures &&
               a.revision == b.revision && a.docGen == b.docGen &&
               a.resourceGen == b.resourceGen;
    }
};

S4SceneSnapshot S4Snapshot(SceneManager& manager)
{
    const auto& reg = manager.GetECS().registry;
    S4SceneSnapshot s;
    s.entities = reg.view<EntityIdComponent>().size();
    s.uuidIndex = manager.AuthoringDoc().uuidIndex.Size();
    s.hierarchy = reg.view<Hierarchy>().size();
    s.pic = reg.view<PrefabInstanceComponent>().size();
    s.pmic = reg.view<PrefabMemberComponent>().size();
    s.meshes = manager.GetECS().meshRegistry.GetCount();
    s.materials = manager.GetECS().materials.size();
    s.textures = manager.GetECS().textures.size();
    s.revision = manager.AuthoringRevision();
    s.docGen = manager.DocumentGeneration();
    s.resourceGen = manager.ResourceGeneration();
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// Test T14 — a hostile provider yields (source instanceId, nil, another live
// instanceId, valid). The reservation must reject the first three draws and
// accept only the valid one, install it coherently on the copied group, and
// leave the source and the other live instance untouched. Provider consumption
// is asserted exactly: 4 draws, logged in order.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: hostile collision queue on duplicate reservation retries to a valid fresh instanceId")
{
    S2Fixture f;
    const auto dirA = S2UniqueTempDir("p8w3_s4_t14_a");
    const auto dirB = S2UniqueTempDir("p8w3_s4_t14_b");
    const auto [rootA, childA] = f.MakeInstance(dirA);
    const auto [rootB, childB] = f.MakeInstance(dirB);
    auto& reg = f.manager.GetECS().registry;

    const auto* srcA = reg.try_get<PrefabMemberComponent>(rootA);
    const auto* srcB = reg.try_get<PrefabMemberComponent>(rootB);
    REQUIRE(srcA);
    REQUIRE(srcB);
    const UUID srcAId = srcA->instanceId;
    const UUID srcBId = srcB->instanceId;
    REQUIRE(srcAId != UUID::Nil());
    REQUIRE(srcBId != UUID::Nil());
    REQUIRE(srcAId != srcBId);

    const auto rootAUuid = reg.get<EntityIdComponent>(rootA).id;

    auto count = f.manager.CountCanonicalSubtreeEntities({ rootAUuid });
    REQUIRE(count.IsOk());
    REQUIRE(count.value == 2);
    const auto known = f.manager.ReserveKnownUuids(count.value);
    REQUIRE(known.size() == 2);
    const auto validId = f.manager.ReserveKnownUuid();
    REQUIRE(validId != UUID::Nil());

    ScriptedUuidProvider hostile;
    hostile.script = { srcAId, UUID::Nil(), srcBId, validId };
    f.manager.SetUuidProvider(&hostile);

    auto dup = f.manager.DuplicateSubtreesWithUuids({ rootAUuid }, known);
    REQUIRE(dup.mutation.success);
    REQUIRE_FALSE(dup.mutation.recoveryWarning.has_value());
    REQUIRE(dup.createdRoots.size() == 1);

    // Exact provider consumption: 4 draws, fully consuming the queue in order.
    REQUIRE(hostile.cursor == 4);
    CHECK(hostile.log.size() == 4);
    CHECK(hostile.log[0] == srcAId);    // collides with a live source==dest id
    CHECK(hostile.log[1] == UUID::Nil());
    CHECK(hostile.log[2] == srcBId);    // collides with another live instance
    CHECK(hostile.log[3] == validId);

    const auto copied = S4SubtreeEntities(f.manager, dup.createdRoots.front());
    REQUIRE(copied.size() == 2);
    const auto copiedRootId = S4MemberInstanceId(f.manager, copied[0]);
    const auto copiedChildId = S4MemberInstanceId(f.manager, copied[1]);
    CHECK(copiedRootId == validId);     // the first non-colliding draw won
    CHECK(copiedChildId == validId);
    const auto* copiedPic = reg.try_get<PrefabInstanceComponent>(copied[0]);
    REQUIRE(copiedPic);
    CHECK(copiedPic->instanceId == validId);

    // Source and the other live instance are untouched.
    const auto* srcAAfter = reg.try_get<PrefabMemberComponent>(rootA);
    REQUIRE(srcAAfter);
    CHECK(srcAAfter->instanceId == srcAId);
    const auto* srcBAfter = reg.try_get<PrefabMemberComponent>(rootB);
    REQUIRE(srcBAfter);
    CHECK(srcBAfter->instanceId == srcBId);

    std::filesystem::remove_all(dirA);
    std::filesystem::remove_all(dirB);
}

// ---------------------------------------------------------------------------
// Test T15 — two complete groups in one operation; the provider returns a
// valid id, then the SAME id again, then a second valid id. The second draw
// must be rejected as an operation-local duplicate and retried, so the two
// copied groups get DISTINCT fresh ids (one each from the script).
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: operation-local duplicate draws are retried per group")
{
    S2Fixture f;
    const auto dirA = S2UniqueTempDir("p8w3_s4_t15_a");
    const auto dirB = S2UniqueTempDir("p8w3_s4_t15_b");
    const auto folderUuid = f.CreateEmpty("Folder");
    REQUIRE(folderUuid != UUID::Nil());
    const auto [rootA, childA] = f.MakeInstance(dirA);
    const auto [rootB, childB] = f.MakeInstance(dirB);
    auto& reg = f.manager.GetECS().registry;

    const auto* srcA = reg.try_get<PrefabMemberComponent>(rootA);
    const auto* srcB = reg.try_get<PrefabMemberComponent>(rootB);
    REQUIRE(srcA);
    REQUIRE(srcB);
    const UUID srcAId = srcA->instanceId;
    const UUID srcBId = srcB->instanceId;
    REQUIRE(srcAId != srcBId);

    const auto rootAUuid = reg.get<EntityIdComponent>(rootA).id;
    const auto rootBUuid = reg.get<EntityIdComponent>(rootB).id;
    REQUIRE(f.manager.Reparent({ rootAUuid }, folderUuid, ReparentMode::PreserveLocal).success);
    REQUIRE(f.manager.Reparent({ rootBUuid }, folderUuid, ReparentMode::PreserveLocal).success);

    auto count = f.manager.CountCanonicalSubtreeEntities({ folderUuid });
    REQUIRE(count.IsOk());
    REQUIRE(count.value == 5); // folder + A(root+child) + B(root+child)
    const auto known = f.manager.ReserveKnownUuids(count.value);
    REQUIRE(known.size() == 5);
    const auto x = f.manager.ReserveKnownUuid();
    const auto y = f.manager.ReserveKnownUuid();
    REQUIRE(x != y);

    ScriptedUuidProvider hostile;
    hostile.script = { x, x, y };   // x reserved, x repeated -> retry, y
    f.manager.SetUuidProvider(&hostile);

    auto dup = f.manager.DuplicateSubtreesWithUuids({ folderUuid }, known);
    REQUIRE(dup.mutation.success);
    REQUIRE_FALSE(dup.mutation.recoveryWarning.has_value());
    REQUIRE(dup.createdRoots.size() == 1);

    REQUIRE(hostile.cursor == 3);
    CHECK(hostile.log.size() == 3);
    CHECK(hostile.log[0] == x);
    CHECK(hostile.log[1] == x);     // the operation-local repeat was rejected
    CHECK(hostile.log[2] == y);

    const auto copied = S4SubtreeEntities(f.manager, dup.createdRoots.front());
    REQUIRE(copied.size() == 5);

    // The folder copy stays ordinary; both copied instance roots reminted.
    CHECK_FALSE(reg.all_of<PrefabInstanceComponent>(copied[0]));
    CHECK_FALSE(reg.all_of<PrefabMemberComponent>(copied[0]));
    const auto* cA = reg.try_get<PrefabInstanceComponent>(copied[1]);
    const auto* cB = reg.try_get<PrefabInstanceComponent>(copied[3]);
    REQUIRE(cA);
    REQUIRE(cB);
    CHECK(cA->instanceId != UUID::Nil());
    CHECK(cB->instanceId != UUID::Nil());
    CHECK(cA->instanceId != cB->instanceId);
    CHECK(cA->instanceId != srcAId);
    CHECK(cB->instanceId != srcAId);
    CHECK(cA->instanceId != srcBId);
    CHECK(cB->instanceId != srcBId);
    // One group got x, the other y (iteration order over the group map is
    // unspecified, but the pair of fresh ids must be exactly {x, y}).
    CHECK(((cA->instanceId == x) || (cA->instanceId == y)));   // each got x or y
    CHECK(((cB->instanceId == x) || (cB->instanceId == y)));   // and the two differ
    CHECK(cA->instanceId != cB->instanceId);
    CHECK(S4MemberInstanceId(f.manager, copied[2]) == cA->instanceId);
    CHECK(S4MemberInstanceId(f.manager, copied[4]) == cB->instanceId);

    std::filesystem::remove_all(dirA);
    std::filesystem::remove_all(dirB);
}

// ---------------------------------------------------------------------------
// Test T16 — an always-nil provider exhausts the reservation on BOTH
// UUID-aware paths (duplicate and paste). Result: DuplicateUuid naming the
// exhaustion, EXACTLY 16 provider attempts, zero created entities, unchanged
// entity UUID index / hierarchy / component counts / authoring revision /
// sync impact, and the clipboard (source) untouched.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: instance-ID reservation exhaustion on duplicate+paste leaves zero destination change")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_s4_t16");
    const auto [rootA, childA] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;

    const auto* srcA = reg.try_get<PrefabMemberComponent>(rootA);
    REQUIRE(srcA);
    const UUID srcAId = srcA->instanceId;
    const auto rootAUuid = reg.get<EntityIdComponent>(rootA).id;

    const auto pre = S4Snapshot(f.manager);
    REQUIRE(pre.pic == 1);

    // Duplicate path: caller-supplied entity UUIDs pre-drawn, then the
    // provider is swapped to an always-nil script.
    auto count = f.manager.CountCanonicalSubtreeEntities({ rootAUuid });
    REQUIRE(count.IsOk());
    REQUIRE(count.value == 2);
    const auto knownDup = f.manager.ReserveKnownUuids(count.value);
    REQUIRE(knownDup.size() == 2);

    // Clipboard for the paste path, cloned and counted BEFORE the swap; its
    // entity UUIDs are caller-supplied and pre-drawn from the manager.
    SceneDocument clipboard;
    DeterministicUuidProvider idsClip;
    clipboard.SetUuidProvider(&idsClip);
    Error cloneErr;
    REQUIRE(SceneSerializer::CloneInMemory(f.manager.AuthoringDoc(), clipboard, cloneErr));
    const auto clipRoot = clipboard.FindByUuid(rootAUuid);
    REQUIRE(static_cast<uint32_t>(clipRoot) != static_cast<uint32_t>(entt::null));
    std::vector<entt::entity> clipSources;
    SceneHierarchy::CollectSubtreePreOrder(clipboard.ecs.registry, clipRoot, clipSources);
    REQUIRE(clipSources.size() == 2);
    const auto knownPaste = f.manager.ReserveKnownUuids(clipSources.size());
    REQUIRE(knownPaste.size() == 2);
    const auto clipPicBefore = clipboard.ecs.registry.view<PrefabInstanceComponent>().size();
    const auto clipPmicBefore = clipboard.ecs.registry.view<PrefabMemberComponent>().size();

    ScriptedUuidProvider hostile1;
    hostile1.script.assign(16, UUID::Nil());
    f.manager.SetUuidProvider(&hostile1);

    auto dup = f.manager.DuplicateSubtreesWithUuids({ rootAUuid }, knownDup);
    REQUIRE_FALSE(dup.mutation.success);
    CHECK(dup.mutation.error.code == rt2::core::Error::DuplicateUuid);
    CHECK(dup.mutation.error.detail.find("16") != std::string::npos);
    CHECK(hostile1.cursor == 16);       // exactly 16 attempts, queue consumed
    CHECK(hostile1.log.size() == 16);
    for (const auto& id : hostile1.log)
        CHECK(id.IsNull());
    CHECK(dup.createdRoots.empty());
    CHECK(dup.mutation.affectedEntities.empty());
    CHECK(dup.mutation.syncImpact == rt2::core::SyncImpact::None);
    REQUIRE(S4Snapshot(f.manager) == pre);   // zero mutation

    ScriptedUuidProvider hostile2;
    hostile2.script.assign(16, UUID::Nil());
    f.manager.SetUuidProvider(&hostile2);

    auto paste = f.manager.PasteSubtreesWithUuids(
        clipboard, { rootAUuid }, std::nullopt, knownPaste);
    REQUIRE_FALSE(paste.mutation.success);
    CHECK(paste.mutation.error.code == rt2::core::Error::DuplicateUuid);
    CHECK(paste.mutation.error.detail.find("16") != std::string::npos);
    CHECK(hostile2.cursor == 16);
    CHECK(paste.createdRoots.empty());
    CHECK(paste.mutation.affectedEntities.empty());
    CHECK(paste.mutation.syncImpact == rt2::core::SyncImpact::None);
    REQUIRE(S4Snapshot(f.manager) == pre);   // still zero mutation

    // Clipboard (the paste source) untouched.
    CHECK(clipboard.ecs.registry.view<PrefabInstanceComponent>().size() == clipPicBefore);
    CHECK(clipboard.ecs.registry.view<PrefabMemberComponent>().size() == clipPmicBefore);
    const auto* clipRootAfter = clipboard.ecs.registry.try_get<PrefabMemberComponent>(clipRoot);
    REQUIRE(clipRootAfter);
    CHECK(clipRootAfter->instanceId == srcAId);

    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Test T17 — InstantiatePrefabWithUuids. Success half: the reservation
// retries past a live instanceId and nil to a valid id, installed coherently.
// Exhaustion half: an always-nil provider fails BEFORE any resource/entity
// mutation — prefab file bytes, entity/component counts, UUID index,
// mesh/material/texture table sizes, diagnostics and authoring revision are
// all unchanged.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: instantiate reservation retries to a valid id and exhausts before mutation")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_s4_t17");
    const auto [rootA, childA] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;
    const auto* srcA = reg.try_get<PrefabMemberComponent>(rootA);
    REQUIRE(srcA);
    const UUID srcAId = srcA->instanceId;
    const auto prefabPath = dir / "s2.rt2prefab";

    // Pre-draw ALL caller-supplied entity UUIDs before swapping the provider,
    // so the scripted scripts are consumed only by instance-ID reservation.
    const auto knownEnt = f.manager.ReserveKnownUuids(2);
    REQUIRE(knownEnt.size() == 2);
    const auto validId = f.manager.ReserveKnownUuid();
    const auto knownEnt2 = f.manager.ReserveKnownUuids(2);
    REQUIRE(knownEnt2.size() == 2);

    // Success half: collide with the live instance id, yield nil, then valid.
    ScriptedUuidProvider hostile;
    hostile.script = { srcAId, UUID::Nil(), validId };
    f.manager.SetUuidProvider(&hostile);

    std::vector<AssetDiagnostic> diags;
    auto inst = f.manager.InstantiatePrefabWithUuids(prefabPath, knownEnt, diags);
    REQUIRE(inst.mutation.success);
    REQUIRE(inst.instanceId.has_value());
    CHECK(*inst.instanceId == validId);
    CHECK(hostile.cursor == 3);
    CHECK(hostile.log.size() == 3);
    CHECK(hostile.log[0] == srcAId);
    CHECK(hostile.log[1] == UUID::Nil());
    CHECK(hostile.log[2] == validId);

    const auto newRoot = f.manager.FindEntityByUuid(knownEnt[0]);
    REQUIRE(static_cast<uint32_t>(newRoot) != static_cast<uint32_t>(entt::null));
    REQUIRE(reg.all_of<PrefabInstanceComponent>(newRoot));
    CHECK(reg.get<PrefabInstanceComponent>(newRoot).instanceId == validId);
    CHECK(S4MemberInstanceId(f.manager, newRoot) == validId);
    const auto newChild = f.manager.FindEntityByUuid(knownEnt[1]);
    REQUIRE(static_cast<uint32_t>(newChild) != static_cast<uint32_t>(entt::null));
    CHECK(S4MemberInstanceId(f.manager, newChild) == validId);

    // MakeInstance leaves the prefab SOURCE entities (root+child) in the scene
    // plus one instantiated copy (root+child): 4 entities, 1 instance. The
    // successful instantiate adds another 2-member instance -> 6/2.
    const auto afterSuccess = S4Snapshot(f.manager);
    REQUIRE(afterSuccess.entities == 6);
    REQUIRE(afterSuccess.pic == 2);
    const auto diagsBeforeExhaust = diags.size();
    const std::string prefabBytes = S2ReadFile(prefabPath);
    REQUIRE(!prefabBytes.empty());

    // Exhaustion half: always-nil provider -> fail before resource/entity
    // mutation; prefab file bytes and every table stay byte-identical.
    ScriptedUuidProvider hostile2;
    hostile2.script.assign(16, UUID::Nil());
    f.manager.SetUuidProvider(&hostile2);

    std::vector<AssetDiagnostic> diags2;
    auto inst2 = f.manager.InstantiatePrefabWithUuids(prefabPath, knownEnt2, diags2);
    REQUIRE_FALSE(inst2.mutation.success);
    CHECK(inst2.mutation.error.code == rt2::core::Error::DuplicateUuid);
    CHECK(inst2.mutation.error.detail.find("16") != std::string::npos);
    CHECK(hostile2.cursor == 16);
    CHECK(hostile2.log.size() == 16);
    CHECK(inst2.createdRoots.empty());
    CHECK(inst2.mutation.affectedEntities.empty());
    CHECK(inst2.mutation.syncImpact == rt2::core::SyncImpact::None);
    CHECK(diags2.size() == diagsBeforeExhaust);
    CHECK(S2ReadFile(prefabPath) == prefabBytes); // file untouched
    REQUIRE(S4Snapshot(f.manager) == afterSuccess); // zero resource/entity change

    // The pre-existing source instance is still intact.
    const auto* srcAAfter = reg.try_get<PrefabMemberComponent>(rootA);
    REQUIRE(srcAAfter);
    CHECK(srcAAfter->instanceId == srcAId);

    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Test T18 — the ORDINARY DuplicateSubtrees path must consume the provider in
// the documented exact order: N staged entity-UUID draws first, then G
// instance-ID draws (G == number of complete instance groups in the forest).
// A scripted provider pins the order by value: the first N ids become the
// copies' entity UUIDs, the final G ids become the copied instance's id.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: ordinary duplicate provider order is entity UUIDs then instance IDs")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_s4_t18");
    const auto [rootA, childA] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;

    const auto* srcA = reg.try_get<PrefabMemberComponent>(rootA);
    REQUIRE(srcA);
    const UUID srcAId = srcA->instanceId;
    const auto rootAUuid = reg.get<EntityIdComponent>(rootA).id;

    // Pre-draw the exact provider order the ordinary path MUST follow:
    // entity UUIDs first (root copy, child copy), then the instance id.
    const auto e0 = f.manager.ReserveKnownUuid();
    const auto e1 = f.manager.ReserveKnownUuid();
    const auto iid = f.manager.ReserveKnownUuid();
    REQUIRE(e0 != e1);
    REQUIRE(e1 != iid);
    REQUIRE(iid != srcAId);

    ScriptedUuidProvider hostile;
    hostile.script = { e0, e1, iid };
    f.manager.SetUuidProvider(&hostile);

    auto result = f.manager.DuplicateSubtrees({ rootAUuid });
    REQUIRE(result.success);
    REQUIRE_FALSE(result.recoveryWarning.has_value());

    // Exact provider-consumption order: 2 entity UUIDs, then 1 instance id.
    REQUIRE(hostile.cursor == 3);
    CHECK(hostile.log.size() == 3);
    CHECK(hostile.log[0] == e0);
    CHECK(hostile.log[1] == e1);
    CHECK(hostile.log[2] == iid);

    REQUIRE(result.affectedEntities.size() == 1);
    const auto copied = S4SubtreeEntities(f.manager, result.affectedEntities.front());
    REQUIRE(copied.size() == 2);
    CHECK(reg.get<EntityIdComponent>(copied[0]).id == e0);
    CHECK(reg.get<EntityIdComponent>(copied[1]).id == e1);
    CHECK(S4MemberInstanceId(f.manager, copied[0]) == iid);
    CHECK(S4MemberInstanceId(f.manager, copied[1]) == iid);
    const auto* copiedPic = reg.try_get<PrefabInstanceComponent>(copied[0]);
    REQUIRE(copiedPic);
    CHECK(copiedPic->instanceId == iid);

    // Source untouched.
    const auto* srcAAfter = reg.try_get<PrefabMemberComponent>(rootA);
    REQUIRE(srcAAfter);
    CHECK(srcAAfter->instanceId == srcAId);

    std::filesystem::remove_all(dir);
}

// ============================================================================
// Tests T19-T22 close the S4 REVIEW FIX 3 gap (ordinary-paste/duplicate entity
// UUID reservation). T14-T18 exercise ONLY the WithUuids paths — the ORDINARY
// DuplicateSubtrees / PasteSubtreesFrom paths staged their destination entity
// UUIDs via the unvalidated `ReserveKnownUuids`, so a degraded/hostile
// provider could hand the create loop a nil, an id already live in the
// authoring index, a repeat, or (for a paste) the source entity's own id, and
// the operation either died midway in the create-loop rollback or — worse —
// adopted a live identity. Fix 3 routes BOTH ordinary paths through the same
// validated, finite pre-mutation reservation the WithUuids paths already used
// (ReserveValidEntityUuids), preserving the pinned provider order from T18:
// all entity UUIDs first, then one instanceId per complete group.
//
// Discrimination faults (recorded in the verification report), per test:
//   test T19 fault: in SceneManager::DuplicateSubtrees replace the validated
//   entity staging with the old raw `ReserveKnownUuids(sources.size())` — a
//   staged nil then trips the create-loop rollback -> REQUIRE(success) fails
//   -> RED. Revert -> GREEN.
//   test T20 fault: same raw-staging bypass on the duplicate path — the
//   operation then consumes its full script through the instance-ID stage (2
//   entity draws + 16 instance-ID draws) and the scripted provider's loud
//   over-consumption REQUIRE throws before CHECK(cursor == 16) -> RED. Revert
//   -> GREEN.
//   test T21 fault: in SceneManager::PasteSubtreesFrom use
//   FreshInstanceIdForbiddenSet (dropping the distinct-source entity-ids
//   rule) for entity staging — a provider draw of the clipboard's re-stamped
//   id is then accepted, so the pasted root adopts the clipboard's identity
//   and REQUIRE(success)/the entity-id CHECK fails -> RED. Revert -> GREEN.
//   test T22 fault: same raw-staging bypass on the paste path — the paste
//   over-consumes the hostile script through the instance-ID stage and the
//   loud REQUIRE throws -> RED. Revert -> GREEN.
// ============================================================================

// ---------------------------------------------------------------------------
// Test T19 — the ORDINARY DuplicateSubtrees path validates its staged entity
// UUIDs before mutation: a nil draw, the source root's own live id, and a
// staged-id repeat are all rejected and retried to valid ids, then per-group
// instance IDs follow with an operation-local repeat — in the documented
// entity-UUIDs-first provider order. The copy gets EXACTLY the staged entity
// ids and coherent per-group fresh instance ids; the source forest is
// untouched.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: ordinary duplicate retries staged entity UUIDs past nil, live and repeated draws")
{
    S2Fixture f;
    const auto dirA = S2UniqueTempDir("p8w3_s4_t19_a");
    const auto dirB = S2UniqueTempDir("p8w3_s4_t19_b");
    const auto folderUuid = f.CreateEmpty("Folder");
    REQUIRE(folderUuid != UUID::Nil());
    const auto [rootA, childA] = f.MakeInstance(dirA);
    const auto [rootB, childB] = f.MakeInstance(dirB);
    auto& reg = f.manager.GetECS().registry;

    const auto srcAId = reg.get<PrefabMemberComponent>(rootA).instanceId;
    const auto srcBId = reg.get<PrefabMemberComponent>(rootB).instanceId;
    REQUIRE(srcAId != srcBId);
    const auto rootAUuid = reg.get<EntityIdComponent>(rootA).id;
    REQUIRE(f.manager.Reparent({ rootAUuid }, folderUuid, ReparentMode::PreserveLocal).success);
    const auto rootBUuid = reg.get<EntityIdComponent>(rootB).id;
    REQUIRE(f.manager.Reparent({ rootBUuid }, folderUuid, ReparentMode::PreserveLocal).success);

    // Pre-draw every VALID id the script hands out, so none of them is nil,
    // live, or otherwise colliding when the reservation accepts them.
    const auto X = f.manager.ReserveKnownUuid();
    const auto e0 = f.manager.ReserveKnownUuid();
    const auto e1 = f.manager.ReserveKnownUuid();
    const auto e2 = f.manager.ReserveKnownUuid();
    const auto e3 = f.manager.ReserveKnownUuid();
    const auto iidA = f.manager.ReserveKnownUuid();
    const auto iidB = f.manager.ReserveKnownUuid();
    REQUIRE(iidA != iidB);

    // 5 entities, 2 complete groups: entity UUIDs first, then instance ids.
    ScriptedUuidProvider hostile;
    hostile.script = {
        UUID::Nil(), rootAUuid, X, X,   // nil, live collision, staged, repeat
        e0, e1, e2, e3,                 // the remaining 4 staged entity uuids
        iidA, iidA, iidB                // per-group instance ids, one repeat
    };
    f.manager.SetUuidProvider(&hostile);

    auto result = f.manager.DuplicateSubtrees({ folderUuid });
    REQUIRE(result.success);
    REQUIRE_FALSE(result.recoveryWarning.has_value());

    // Exact provider-consumption order: nil+live rejected, X staged, X repeat
    // rejected, e0..e3 staged, then iidA accepted, iidA repeat rejected, iidB.
    REQUIRE(hostile.cursor == 11);
    CHECK(hostile.log.size() == 11);
    CHECK(hostile.log[0] == UUID::Nil());
    CHECK(hostile.log[1] == rootAUuid);
    CHECK(hostile.log[2] == X);
    CHECK(hostile.log[3] == X);
    CHECK(hostile.log[4] == e0);
    CHECK(hostile.log[5] == e1);
    CHECK(hostile.log[6] == e2);
    CHECK(hostile.log[7] == e3);
    CHECK(hostile.log[8] == iidA);
    CHECK(hostile.log[9] == iidA);
    CHECK(hostile.log[10] == iidB);

    REQUIRE(result.affectedEntities.size() == 1);
    const auto copied = S4SubtreeEntities(f.manager, result.affectedEntities.front());
    REQUIRE(copied.size() == 5);
    // Entity UUIDs land in the exact staged order (folder, A root, A child,
    // B root, B child).
    CHECK(reg.get<EntityIdComponent>(copied[0]).id == X);
    CHECK(reg.get<EntityIdComponent>(copied[1]).id == e0);
    CHECK(reg.get<EntityIdComponent>(copied[2]).id == e1);
    CHECK(reg.get<EntityIdComponent>(copied[3]).id == e2);
    CHECK(reg.get<EntityIdComponent>(copied[4]).id == e3);

    // The folder copy stays ordinary; both instances are reminted, each member
    // agreeing with its own root, the two groups DIFFERENT and never the
    // source identities.
    CHECK_FALSE(reg.all_of<PrefabInstanceComponent>(copied[0]));
    CHECK_FALSE(reg.all_of<PrefabMemberComponent>(copied[0]));
    const auto* cA = reg.try_get<PrefabInstanceComponent>(copied[1]);
    const auto* cB = reg.try_get<PrefabInstanceComponent>(copied[3]);
    REQUIRE(cA);
    REQUIRE(cB);
    CHECK(cA->instanceId != srcAId);
    CHECK(cA->instanceId != srcBId);
    CHECK(cB->instanceId != srcAId);
    CHECK(cB->instanceId != srcBId);
    CHECK(((cA->instanceId == iidA) || (cA->instanceId == iidB)));   // one per group
    CHECK(((cB->instanceId == iidA) || (cB->instanceId == iidB)));
    CHECK(cA->instanceId != cB->instanceId);
    CHECK(S4MemberInstanceId(f.manager, copied[1]) == cA->instanceId);
    CHECK(S4MemberInstanceId(f.manager, copied[2]) == cA->instanceId);
    CHECK(S4MemberInstanceId(f.manager, copied[3]) == cB->instanceId);
    CHECK(S4MemberInstanceId(f.manager, copied[4]) == cB->instanceId);

    // Source forest untouched.
    CHECK(reg.get<PrefabMemberComponent>(rootA).instanceId == srcAId);
    CHECK(reg.get<PrefabMemberComponent>(rootB).instanceId == srcBId);

    std::filesystem::remove_all(dirA);
    std::filesystem::remove_all(dirB);
}

// ---------------------------------------------------------------------------
// Test T20 — the ORDINARY DuplicateSubtrees entity-UUID reservation exhausts
// its finite budget against an always-colliding provider (every draw is the
// source root's own live id) and fails with a stage-specific DuplicateUuid
// BEFORE any destination mutation: EXACTLY 16 provider attempts, zero created
// entities, unchanged entity UUID index / hierarchy / component counts /
// authoring revision / sync impact, and the source untouched.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: ordinary duplicate entity-UUID reservation exhaustion leaves zero destination change")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_s4_t20");
    const auto [rootA, childA] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;

    const auto* srcA = reg.try_get<PrefabMemberComponent>(rootA);
    REQUIRE(srcA);
    const UUID srcAId = srcA->instanceId;
    const auto rootAUuid = reg.get<EntityIdComponent>(rootA).id;

    const auto pre = S4Snapshot(f.manager);
    REQUIRE(pre.pic == 1);

    ScriptedUuidProvider hostile;
    hostile.script.assign(16, rootAUuid);   // live collision, every draw
    f.manager.SetUuidProvider(&hostile);

    auto result = f.manager.DuplicateSubtrees({ rootAUuid });
    REQUIRE_FALSE(result.success);
    CHECK(result.error.code == rt2::core::Error::DuplicateUuid);
    CHECK(result.error.detail.find("DuplicateSubtrees") != std::string::npos);
    CHECK(result.error.detail.find("16") != std::string::npos);
    CHECK(hostile.cursor == 16);            // exactly 16 attempts, queue consumed
    CHECK(hostile.log.size() == 16);
    for (const auto& id : hostile.log)
        CHECK(id == rootAUuid);
    CHECK(result.affectedEntities.empty());
    CHECK(result.syncImpact == rt2::core::SyncImpact::None);
    REQUIRE(S4Snapshot(f.manager) == pre);  // zero mutation

    // Source untouched.
    const auto* srcAAfter = reg.try_get<PrefabMemberComponent>(rootA);
    REQUIRE(srcAAfter);
    CHECK(srcAAfter->instanceId == srcAId);

    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Test T21 — the ORDINARY PasteSubtreesFrom path validates its staged entity
// UUIDs before mutation, INCLUDING the distinct-source rule: a provider draw
// equal to a CLIPBOARD (source document) entity's id is rejected even though
// that id is absent from the destination authoring index. Script: nil, the
// source root's own live id, the clipboard root's re-stamped id, a staged-id
// repeat, then the valid ids, then per-group instance ids with a repeat — all
// in the documented entity-UUIDs-first order. The pasted forest gets coherent
// per-group fresh instance ids and the clipboard source is untouched.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: ordinary paste retries staged entity UUIDs past nil, live and distinct-source draws")
{
    S2Fixture f;
    const auto dirA = S2UniqueTempDir("p8w3_s4_t21_a");
    const auto dirB = S2UniqueTempDir("p8w3_s4_t21_b");
    const auto folderUuid = f.CreateEmpty("Folder");
    REQUIRE(folderUuid != UUID::Nil());
    const auto [rootA, childA] = f.MakeInstance(dirA);
    const auto [rootB, childB] = f.MakeInstance(dirB);
    auto& reg = f.manager.GetECS().registry;

    const auto srcAId = reg.get<PrefabMemberComponent>(rootA).instanceId;
    const auto srcBId = reg.get<PrefabMemberComponent>(rootB).instanceId;
    REQUIRE(srcAId != srcBId);
    const auto rootAUuid = reg.get<EntityIdComponent>(rootA).id;
    REQUIRE(f.manager.Reparent({ rootAUuid }, folderUuid, ReparentMode::PreserveLocal).success);
    const auto rootBUuid = reg.get<EntityIdComponent>(rootB).id;
    REQUIRE(f.manager.Reparent({ rootBUuid }, folderUuid, ReparentMode::PreserveLocal).success);

    // Clipboard = whole-scene clone. Re-stamp the clipboard ROOT entity's id
    // with a fresh UUID that exists NOWHERE in the destination, so a provider
    // draw of that value is a DISTINCT-SOURCE collision only (the destination
    // authoring index does not contain it).
    SceneDocument clipboard;
    DeterministicUuidProvider idsClip;
    clipboard.SetUuidProvider(&idsClip);
    Error cloneErr;
    REQUIRE(SceneSerializer::CloneInMemory(f.manager.AuthoringDoc(), clipboard, cloneErr));
    const auto clipFolder = clipboard.FindByUuid(folderUuid);
    REQUIRE(static_cast<uint32_t>(clipFolder) != static_cast<uint32_t>(entt::null));
    const auto faceUuid = f.manager.ReserveKnownUuid();
    clipboard.ecs.registry.get<EntityIdComponent>(clipFolder).id = faceUuid;
    const auto clipPicBefore = clipboard.ecs.registry.view<PrefabInstanceComponent>().size();
    const auto clipPmicBefore = clipboard.ecs.registry.view<PrefabMemberComponent>().size();
    const auto clipRootA = clipboard.FindByUuid(rootAUuid);
    REQUIRE(static_cast<uint32_t>(clipRootA) != static_cast<uint32_t>(entt::null));
    const auto clipARootId = clipboard.ecs.registry.get<PrefabMemberComponent>(clipRootA).instanceId;

    const auto X = f.manager.ReserveKnownUuid();
    const auto e0 = f.manager.ReserveKnownUuid();
    const auto e1 = f.manager.ReserveKnownUuid();
    const auto e2 = f.manager.ReserveKnownUuid();
    const auto e3 = f.manager.ReserveKnownUuid();
    const auto iidA = f.manager.ReserveKnownUuid();
    const auto iidB = f.manager.ReserveKnownUuid();
    REQUIRE(iidA != iidB);
    REQUIRE(faceUuid != rootAUuid);

    ScriptedUuidProvider hostile;
    hostile.script = {
        UUID::Nil(), rootAUuid, faceUuid, X, X,
        e0, e1, e2, e3,
        iidA, iidA, iidB
    };
    f.manager.SetUuidProvider(&hostile);

    auto result = f.manager.PasteSubtreesFrom(clipboard, { folderUuid }, std::nullopt);
    REQUIRE(result.success);
    REQUIRE_FALSE(result.recoveryWarning.has_value());

    // nil and the two distinct rejections, then X staged, X repeat rejected,
    // e0..e3 staged, then per-group instance ids with one repeat.
    REQUIRE(hostile.cursor == 12);
    CHECK(hostile.log.size() == 12);
    CHECK(hostile.log[0] == UUID::Nil());
    CHECK(hostile.log[1] == rootAUuid);
    CHECK(hostile.log[2] == faceUuid);
    CHECK(hostile.log[3] == X);
    CHECK(hostile.log[4] == X);
    CHECK(hostile.log[5] == e0);
    CHECK(hostile.log[6] == e1);
    CHECK(hostile.log[7] == e2);
    CHECK(hostile.log[8] == e3);
    CHECK(hostile.log[9] == iidA);
    CHECK(hostile.log[10] == iidA);
    CHECK(hostile.log[11] == iidB);

    REQUIRE(result.affectedEntities.size() == 1);
    const auto pasted = S4SubtreeEntities(f.manager, result.affectedEntities.front());
    REQUIRE(pasted.size() == 5);
    CHECK(reg.get<EntityIdComponent>(pasted[0]).id == X);
    CHECK(reg.get<EntityIdComponent>(pasted[1]).id == e0);
    CHECK(reg.get<EntityIdComponent>(pasted[2]).id == e1);
    CHECK(reg.get<EntityIdComponent>(pasted[3]).id == e2);
    CHECK(reg.get<EntityIdComponent>(pasted[4]).id == e3);

    CHECK_FALSE(reg.all_of<PrefabInstanceComponent>(pasted[0]));
    CHECK_FALSE(reg.all_of<PrefabMemberComponent>(pasted[0]));
    const auto* pA = reg.try_get<PrefabInstanceComponent>(pasted[1]);
    const auto* pB = reg.try_get<PrefabInstanceComponent>(pasted[3]);
    REQUIRE(pA);
    REQUIRE(pB);
    CHECK(pA->instanceId != srcAId);
    CHECK(pA->instanceId != srcBId);
    CHECK(pB->instanceId != srcAId);
    CHECK(pB->instanceId != srcBId);
    CHECK(((pA->instanceId == iidA) || (pA->instanceId == iidB)));
    CHECK(((pB->instanceId == iidA) || (pB->instanceId == iidB)));
    CHECK(pA->instanceId != pB->instanceId);
    CHECK(S4MemberInstanceId(f.manager, pasted[1]) == pA->instanceId);
    CHECK(S4MemberInstanceId(f.manager, pasted[2]) == pA->instanceId);
    CHECK(S4MemberInstanceId(f.manager, pasted[3]) == pB->instanceId);
    CHECK(S4MemberInstanceId(f.manager, pasted[4]) == pB->instanceId);

    // Clipboard (source) untouched — pic/pmic counts and instance roots intact.
    CHECK(clipboard.ecs.registry.view<PrefabInstanceComponent>().size() == clipPicBefore);
    CHECK(clipboard.ecs.registry.view<PrefabMemberComponent>().size() == clipPmicBefore);
    CHECK(clipboard.ecs.registry.get<PrefabMemberComponent>(clipRootA).instanceId == clipARootId);

    std::filesystem::remove_all(dirA);
    std::filesystem::remove_all(dirB);
}

// ---------------------------------------------------------------------------
// Test T22 — the ORDINARY PasteSubtreesFrom entity-UUID reservation exhausts
// its finite budget against an always-colliding provider (every draw is the
// source root's own live id — double-forbidden since the clipboard root
// carries the same id and it is already in the destination index) and fails
// with a stage-specific DuplicateUuid BEFORE any destination mutation: EXACTLY
// 16 provider attempts, zero created entities, unchanged destination snapshot,
// clipboard untouched.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: ordinary paste entity-UUID reservation exhaustion leaves zero destination change")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_s4_t22");
    const auto [rootA, childA] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;

    const auto* srcA = reg.try_get<PrefabMemberComponent>(rootA);
    REQUIRE(srcA);
    const UUID srcAId = srcA->instanceId;
    const auto rootAUuid = reg.get<EntityIdComponent>(rootA).id;

    SceneDocument clipboard;
    DeterministicUuidProvider idsClip;
    clipboard.SetUuidProvider(&idsClip);
    Error cloneErr;
    REQUIRE(SceneSerializer::CloneInMemory(f.manager.AuthoringDoc(), clipboard, cloneErr));
    const auto clipRoot = clipboard.FindByUuid(rootAUuid);
    REQUIRE(static_cast<uint32_t>(clipRoot) != static_cast<uint32_t>(entt::null));

    const auto pre = S4Snapshot(f.manager);
    REQUIRE(pre.pic == 1);
    const auto clipPicBefore = clipboard.ecs.registry.view<PrefabInstanceComponent>().size();
    const auto clipPmicBefore = clipboard.ecs.registry.view<PrefabMemberComponent>().size();

    ScriptedUuidProvider hostile;
    hostile.script.assign(16, rootAUuid);
    f.manager.SetUuidProvider(&hostile);

    auto result = f.manager.PasteSubtreesFrom(clipboard, { rootAUuid }, std::nullopt);
    REQUIRE_FALSE(result.success);
    CHECK(result.error.code == rt2::core::Error::DuplicateUuid);
    CHECK(result.error.detail.find("PasteSubtreesFrom") != std::string::npos);
    CHECK(result.error.detail.find("16") != std::string::npos);
    CHECK(hostile.cursor == 16);
    CHECK(hostile.log.size() == 16);
    for (const auto& id : hostile.log)
        CHECK(id == rootAUuid);
    CHECK(result.affectedEntities.empty());
    CHECK(result.syncImpact == rt2::core::SyncImpact::None);
    REQUIRE(S4Snapshot(f.manager) == pre);  // zero destination mutation

    // Clipboard (source) untouched.
    CHECK(clipboard.ecs.registry.view<PrefabInstanceComponent>().size() == clipPicBefore);
    CHECK(clipboard.ecs.registry.view<PrefabMemberComponent>().size() == clipPmicBefore);
    const auto* clipRootAfter = clipboard.ecs.registry.try_get<PrefabMemberComponent>(clipRoot);
    REQUIRE(clipRootAfter);
    CHECK(clipRootAfter->instanceId == srcAId);

    std::filesystem::remove_all(dir);
}

// ============================================================================
// Phase 8 W3, S4 FIXUP — the shared clipboard-generation paste guard.
//
// SceneEditorUI::PasteCommand used to bypass EditorSceneState's clipboard
// guards entirely: it read the raw clipboard document + roots and called
// SceneManager::PasteSubtreesWithUuids directly. That path only range-checks
// resource indices (SceneManager.cpp:3985-4005), so after CompactMeshRegistry
// renames an in-range index the paste silently binds the wrong material. The
// fix: one shared EditorSceneState validator used by BOTH paste paths, plus a
// CPU-linkable PasteWithUuidsForCommand that SceneEditorUI::PasteCommand now
// routes through.
//
// Each stale preparation below must return ClipboardStale with ZERO provider
// UUID draws (an empty script hard-fails on any draw, so zero draws is
// enforced), no history entry, and an untouched destination.
// ============================================================================

// ---------------------------------------------------------------------------
// Test T23 — the discriminating P1 case: materials A/B/C, copy the object on
// B (index 1), delete it, and compaction remaps C onto index 1. Index 1 is
// now IN RANGE but names C. The shared guard must reject via ClipboardStale
// (resource generation changed) with zero draws and zero mutation. A direct,
// unguarded manager paste then demonstrates the hole the guard closes: it
// succeeds and silently rebinds the pasted object to C.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: shared paste guard rejects an in-range stale material index after compaction (A/B/C)")
{
    SceneManager manager;
    DeterministicUuidProvider ids;
    manager.SetUuidProvider(&ids);
    auto& reg = manager.GetECS().registry;

    SceneMaterial matA;
    matA.sourceKey = "A";
    const int mA = manager.AddMaterial(matA);
    SceneMaterial matB;
    matB.sourceKey = "B";
    const int mB = manager.AddMaterial(matB);
    SceneMaterial matC;
    matC.sourceKey = "C";
    const int mC = manager.AddMaterial(matC);
    REQUIRE(mA == 0);
    REQUIRE(mB == 1);
    REQUIRE(mC == 2);

    // Use CreatePrimitiveEntity (not AddObjectWithGeometry): CloneInMemory /
    // EditorSceneState::Copy needs a PrimitiveComponent or importedSource.
    const auto makeCube = [&](const char* name, int matIdx) {
        const UUID uuid = ids.CreateV4();
        EditableTRS trs;
        const auto r = manager.CreatePrimitiveEntity(
            uuid, name, PrimitiveComponent::Cube, 1.0f, trs, matIdx);
        REQUIRE(r.success);
        return uuid;
    };
    const UUID eaUuid = makeCube("A", mA);
    const UUID ebUuid = makeCube("B", mB);
    const UUID ecUuid = makeCube("C", mC);

    // Before the delete, index 1 is B.
    REQUIRE(manager.GetECS().materials[1].sourceKey == "B");

    EditorSceneState state;
    rt2::core::Error err;
    INFO(err.Format());
    REQUIRE(state.Copy(manager, {ebUuid}, err));

    // Delete B's object -> compaction drops material B, C remaps to index 1,
    // and index 1 is now IN RANGE but names C.
    const auto removeResult = manager.RemoveSubtrees({ebUuid});
    REQUIRE(removeResult.success);
    REQUIRE(manager.GetECS().materials.size() == 2);
    REQUIRE(manager.GetECS().materials[0].sourceKey == "A");
    REQUIRE(manager.GetECS().materials[1].sourceKey == "C");

    // Hostile provider with an EMPTY script: any UUID draw is an instant hard
    // failure (REQUIRE inside CreateV4), so "zero draws" is enforced.
    ScriptedUuidProvider hostile;
    manager.SetUuidProvider(&hostile);

    const auto pre = S4Snapshot(manager);
    const uint64_t resourceGenBefore = manager.ResourceGeneration();

    const auto paste = state.PasteWithUuidsForCommand(manager);
    REQUIRE_FALSE(paste.mutation.success);
    CHECK(paste.mutation.error.code == rt2::core::Error::ClipboardStale);
    CHECK(paste.createdRoots.empty());
    CHECK(hostile.cursor == 0);                 // zero UUID draws
    REQUIRE(S4Snapshot(manager) == pre);        // zero destination mutation
    CHECK(manager.ResourceGeneration() == resourceGenBefore);

    // The hole the shared guard closes: the manager's own range check cannot
    // see an in-range stale index. A direct paste of the same clipboard binds
    // material index 1 — which now names C, not B.
    manager.SetUuidProvider(&ids);
    std::size_t clipCount = 0;
    {
        const auto* clip = state.ClipboardDocument();
        REQUIRE(clip);
        const auto clipRoot = clip->FindByUuid(ebUuid);
        REQUIRE(static_cast<uint32_t>(clipRoot) != static_cast<uint32_t>(entt::null));
        std::vector<entt::entity> subtree;
        SceneHierarchy::CollectSubtreePreOrder(clip->ecs.registry, clipRoot, subtree);
        clipCount = subtree.size();
    }
    REQUIRE(clipCount == 1);
    const auto direct = manager.PasteSubtreesWithUuids(
        *state.ClipboardDocument(), state.ClipboardRoots(), std::nullopt,
        manager.ReserveKnownUuids(clipCount));
    REQUIRE(direct.mutation.success);           // range check passes...
    REQUIRE(direct.createdRoots.size() == 1);   // ...so the paste succeeds
    const auto pasted = manager.FindEntityByUuid(direct.createdRoots.front());
    REQUIRE(static_cast<uint32_t>(pasted) != static_cast<uint32_t>(entt::null));
    const auto* pastedRef = reg.try_get<MeshRef>(pasted);
    REQUIRE(pastedRef);
    REQUIRE(pastedRef->materialIndex >= 0);
    REQUIRE(static_cast<size_t>(pastedRef->materialIndex) < manager.GetECS().materials.size());
    CHECK(manager.GetECS().materials[pastedRef->materialIndex].sourceKey == "C"); // silent rebind
}

// ---------------------------------------------------------------------------
// Test T24 — a texture-only resource change (meshes and materials untouched,
// every material texture index still in range) must ALSO invalidate the
// clipboard. The manager's range checks cannot see it: they validate material
// INDICES, never the clipboard material's texture references. The shared
// generation guard is the only protection.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: texture-only resource generation change invalidates the clipboard paste")
{
    SceneManager manager;
    DeterministicUuidProvider ids;
    manager.SetUuidProvider(&ids);

    auto& scene = manager.GetECS();
    scene.textures.resize(4);
    for (size_t i = 0; i < scene.textures.size(); ++i)
        scene.textures[i].ref.path = "texture" + std::to_string(i);

    // All four textures referenced so the pruned texture is only the added one.
    SceneMaterial matA;
    matA.baseColorTextureIndex = 0;
    matA.normalTextureIndex = 1;
    const int mA = manager.AddMaterial(matA);
    SceneMaterial matB;
    matB.baseColorTextureIndex = 2;
    matB.metallicRoughnessTextureIndex = 3;
    const int mB = manager.AddMaterial(matB);
    REQUIRE(mA == 0);
    REQUIRE(mB == 1);

    // Use CreatePrimitiveEntity (not AddObjectWithGeometry): CloneInMemory /
    // EditorSceneState::Copy needs a PrimitiveComponent or importedSource.
    const auto makeCube = [&](const char* name, int matIdx) {
        const UUID uuid = ids.CreateV4();
        EditableTRS trs;
        const auto r = manager.CreatePrimitiveEntity(
            uuid, name, PrimitiveComponent::Cube, 1.0f, trs, matIdx);
        REQUIRE(r.success);
        return uuid;
    };
    const UUID eaUuid = makeCube("A", mA);
    const UUID ebUuid = makeCube("B", mB);

    EditorSceneState state;
    rt2::core::Error err;
    INFO(err.Format());
    REQUIRE(state.Copy(manager, {ebUuid}, err));

    const uint64_t resourceGenBefore = manager.ResourceGeneration();

    // Texture-only change: add an unreferenced texture, then compact. Meshes
    // and materials are untouched (indices unrebased); only the texture table
    // prunes and the resource generation bumps.
    scene.textures.push_back(SceneTexture{});
    scene.textures.back().ref.path = "texture-unreferenced";
    REQUIRE(manager.CompactMeshRegistry());
    REQUIRE(manager.ResourceGeneration() == resourceGenBefore + 1);
    REQUIRE(manager.GetECS().materials.size() == 2);
    REQUIRE(manager.GetECS().materials[0].baseColorTextureIndex == 0);
    REQUIRE(manager.GetECS().materials[1].baseColorTextureIndex == 2);
    REQUIRE(manager.GetECS().textures.size() == 4);

    ScriptedUuidProvider hostile;
    manager.SetUuidProvider(&hostile);

    // Zero-mutation baseline is captured AFTER the resource change and BEFORE
    // the paste: the paste must leave the (already-changed) scene untouched.
    const auto pre = S4Snapshot(manager);
    const auto paste = state.PasteWithUuidsForCommand(manager);
    REQUIRE_FALSE(paste.mutation.success);
    CHECK(paste.mutation.error.code == rt2::core::Error::ClipboardStale);
    CHECK(paste.createdRoots.empty());
    CHECK(hostile.cursor == 0);                 // zero UUID draws
    REQUIRE(S4Snapshot(manager) == pre);        // zero destination mutation
}

// ---------------------------------------------------------------------------
// Test T25 — a document generation change (the whole authoring document is
// replaced) invalidates the clipboard via the document-generation branch of
// the shared guard.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: document generation change invalidates the clipboard paste")
{
    SceneManager manager;
    DeterministicUuidProvider ids;
    manager.SetUuidProvider(&ids);
    auto& reg = manager.GetECS().registry;

    SceneMaterial defaultMat;
    const int m0 = manager.AddMaterial(defaultMat);
    REQUIRE(m0 == 0);
    SceneMaterial matB;
    matB.sourceKey = "B";
    const int mB = manager.AddMaterial(matB); // index 1
    REQUIRE(mB == 1);

    // Use CreatePrimitiveEntity (not AddObjectWithGeometry): CloneInMemory /
    // EditorSceneState::Copy needs a PrimitiveComponent or importedSource.
    const UUID ebUuid = ids.CreateV4();
    EditableTRS trs;
    const auto createResult = manager.CreatePrimitiveEntity(
        ebUuid, "B", PrimitiveComponent::Cube, 1.0f, trs, mB);
    REQUIRE(createResult.success);
    (void)reg;

    EditorSceneState state;
    rt2::core::Error err;
    INFO(err.Format());
    REQUIRE(state.Copy(manager, {ebUuid}, err));

    // Replace the document (Clear() bumps DocumentGeneration AND
    // ResourceGeneration); the guard rejects on the document branch first.
    manager.Clear();

    ScriptedUuidProvider hostile;
    manager.SetUuidProvider(&hostile);

    const auto pre = S4Snapshot(manager);
    const auto paste = state.PasteWithUuidsForCommand(manager);
    REQUIRE_FALSE(paste.mutation.success);
    CHECK(paste.mutation.error.code == rt2::core::Error::ClipboardStale);
    CHECK(paste.createdRoots.empty());
    CHECK(hostile.cursor == 0);                 // zero UUID draws
    REQUIRE(S4Snapshot(manager) == pre);        // zero destination mutation
}

// ---------------------------------------------------------------------------
// Test T26 — the valid-path control: a fresh clipboard pastes via the SAME
// state-level method the UI calls. createdRoots and sourceToDuplicate come
// back for undo, exactly one UUID is reserved, and the result records into an
// EditorCommandHistory and undoes.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: valid clipboard paste reserves exactly, creates roots, and records undo")
{
    SceneManager manager;
    DeterministicUuidProvider ids;
    manager.SetUuidProvider(&ids);
    auto& reg = manager.GetECS().registry;

    SceneMaterial defaultMat;
    const int m0 = manager.AddMaterial(defaultMat);
    REQUIRE(m0 == 0);
    SceneMaterial matB;
    matB.sourceKey = "B";
    const int mB = manager.AddMaterial(matB);
    REQUIRE(mB == 1);

    // Use CreatePrimitiveEntity (not AddObjectWithGeometry): CloneInMemory /
    // EditorSceneState::Copy needs a PrimitiveComponent or importedSource.
    const UUID ebUuid = ids.CreateV4();
    EditableTRS trs;
    const auto createResult = manager.CreatePrimitiveEntity(
        ebUuid, "B", PrimitiveComponent::Cube, 1.0f, trs, mB);
    REQUIRE(createResult.success);
    (void)reg;

    EditorSceneState state;
    rt2::core::Error err;
    INFO(err.Format());
    REQUIRE(state.Copy(manager, {ebUuid}, err));

    const auto pre = S4Snapshot(manager);

    // Scripted provider serving exactly one fresh UUID (the clipboard subtree
    // is one entity). The guarded path must reserve exactly that one.
    const UUID freshUuid = ids.CreateV4();
    ScriptedUuidProvider scripted;
    scripted.script = { freshUuid };
    manager.SetUuidProvider(&scripted);

    const auto paste = state.PasteWithUuidsForCommand(manager);
    REQUIRE(paste.mutation.success);
    CHECK(paste.mutation.error.IsOk());
    REQUIRE(paste.createdRoots.size() == 1);
    REQUIRE(paste.sourceToDuplicate.size() == 1);
    CHECK(paste.sourceToDuplicate[0].first == ebUuid);   // clipboard-doc source
    CHECK(paste.sourceToDuplicate[0].second == freshUuid);
    CHECK(scripted.cursor == 1);                // exact reservation: one entity
    CHECK(scripted.log.size() == 1);
    CHECK(scripted.log[0] == freshUuid);

    // The pasted entity really exists with the reserved UUID.
    const auto pasted = manager.FindEntityByUuid(freshUuid);
    REQUIRE(static_cast<uint32_t>(pasted) != static_cast<uint32_t>(entt::null));
    REQUIRE(S4Snapshot(manager).entities == pre.entities + 1);

    // Undo wiring mirrors SceneEditorUI::PasteCommand: capture + build the
    // command + RecordApplied, then undo removes the pasted entity.
    auto snapshot = manager.CaptureSubtreeSnapshot(paste.createdRoots);
    REQUIRE_FALSE(snapshot.entities.empty());
    auto cmd = MakePasteSubtreesCommand(std::move(snapshot), paste.createdRoots);
    REQUIRE(cmd != nullptr);
    EditorCommandHistory history;
    history.RecordApplied(std::move(cmd), manager, paste.mutation);
    REQUIRE(history.CanUndo());

    const auto undoResult = history.Undo(manager);
    REQUIRE(undoResult.success);
    REQUIRE(static_cast<uint32_t>(manager.FindEntityByUuid(freshUuid))
            == static_cast<uint32_t>(entt::null));
    REQUIRE(S4Snapshot(manager).entities == pre.entities);
}

// ============================================================================
// Phase 8 W3, S4 FIXUP 2 (re-review 3, P1) — the overlapping-duplicate-root
// reservation gap.
//
// PasteWithUuidsForCommand used to count each RAW clipboard root's subtree and
// sum them (EditorSceneState.cpp:102-115), then reserve that total and hand it
// to PasteSubtreesWithUuids. The manager canonicalizes the same roots first —
// duplicate roots are deduplicated and a selected descendant covered by a
// selected ancestor is removed (SceneManager.cpp:35-71, 3965-3988) — and
// requires the supplied UUID count to equal that smaller canonical forest
// (SceneManager.cpp:4009-4014). For a {parent, child} two-entity tree the old
// counting reserved 2 + 1 = 3 UUIDs while the manager found 2 sources and
// failed with "known UUID count does not match the canonical subtree size",
// rejecting a valid multi-selection paste in the editor.
//
// Fix: PasteWithUuidsForCommand now counts through the manager's
// document-parameterized CountCanonicalDocumentSubtreeEntities, which applies
// the SAME validation-first canonicalization and pre-order traversal as
// PasteSubtreesWithUuids, so the reserved count always equals the canonical
// forest exactly.
//
// T27 drives a {parent, child} selection through the real
// EditorSceneState::Copy + PasteWithUuidsForCommand path, plus a duplicate-root
// input, and asserts success, exact provider draws == canonical entity count,
// one created root, the complete source mapping, and a working undo command.
// The old raw-root counting is RED-injected per the verification report: with
// a two-slot script, the over-reservation draws a third UUID and the script's
// REQUIRE(cursor < script.size()) fails the test loudly.
// ============================================================================

// ---------------------------------------------------------------------------
// Test T27 — overlapping {parent, child} and duplicate-root selections paste
// via the SAME state-level method the UI calls: the reservation uses the
// canonical forest count (2), one canonical root is created, the full parent +
// child source mapping comes back for undo, and the recorded command undoes.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: overlapping parent+child and duplicate-root clipboard selections reserve the canonical count and undo")
{
    SceneManager manager;
    DeterministicUuidProvider ids;
    manager.SetUuidProvider(&ids);
    auto& reg = manager.GetECS().registry;

    SceneMaterial defaultMat;
    const int m0 = manager.AddMaterial(defaultMat);
    REQUIRE(m0 == 0);

    // Parent + child primitive hierarchy (both reserved, so ranks are stable).
    const UUID parentUuid = ids.CreateV4();
    EditableTRS trs;
    const auto parentResult = manager.CreatePrimitiveEntity(
        parentUuid, "Parent", PrimitiveComponent::Cube, 1.0f, trs, m0);
    REQUIRE(parentResult.success);
    const UUID childUuid = ids.CreateV4();
    const auto childResult = manager.CreatePrimitiveEntity(
        childUuid, "Child", PrimitiveComponent::Sphere, 0.5f, trs, m0, parentUuid);
    REQUIRE(childResult.success);

    const auto parentEntity = manager.FindEntityByUuid(parentUuid);
    REQUIRE(static_cast<uint32_t>(parentEntity) != static_cast<uint32_t>(entt::null));
    const auto childEntity = manager.FindEntityByUuid(childUuid);
    REQUIRE(static_cast<uint32_t>(childEntity) != static_cast<uint32_t>(entt::null));
    const auto* childHier = reg.try_get<Hierarchy>(childEntity);
    REQUIRE(childHier);
    CHECK(childHier->parent == parentEntity);

    const auto pre = S4Snapshot(manager);

    // Copy BOTH selected roots: the child is covered by the parent.
    EditorSceneState state;
    rt2::core::Error err;
    INFO(err.Format());
    REQUIRE(state.Copy(manager, {parentUuid, childUuid}, err));

    // Scripted provider serving exactly TWO fresh UUIDs (the canonical forest
    // of a parent + child tree). Raw-root counting would demand a third draw
    // and blow up the script here -> loud RED.
    const UUID freshParent = ids.CreateV4();
    const UUID freshChild = ids.CreateV4();
    ScriptedUuidProvider scripted;
    scripted.script = { freshParent, freshChild };
    manager.SetUuidProvider(&scripted);

    const auto paste = state.PasteWithUuidsForCommand(manager);
    REQUIRE(paste.mutation.success);
    CHECK(paste.mutation.error.IsOk());
    REQUIRE(paste.createdRoots.size() == 1);        // only the canonical parent
    REQUIRE(paste.sourceToDuplicate.size() == 2);   // parent AND child mapped
    CHECK(paste.sourceToDuplicate[0].first == parentUuid);
    CHECK(paste.sourceToDuplicate[0].second == freshParent);
    CHECK(paste.sourceToDuplicate[1].first == childUuid);
    CHECK(paste.sourceToDuplicate[1].second == freshChild);
    CHECK(scripted.cursor == 2);        // exact reservation: canonical count, not raw sum
    CHECK(scripted.log.size() == 2);
    CHECK(scripted.log[0] == freshParent);
    CHECK(scripted.log[1] == freshChild);
    CHECK(paste.createdRoots[0] == freshParent);

    // Both pasted entities exist; the pasted child is wired under the pasted
    // parent.
    const auto pastedParent = manager.FindEntityByUuid(freshParent);
    REQUIRE(static_cast<uint32_t>(pastedParent) != static_cast<uint32_t>(entt::null));
    const auto pastedChild = manager.FindEntityByUuid(freshChild);
    REQUIRE(static_cast<uint32_t>(pastedChild) != static_cast<uint32_t>(entt::null));
    const auto* pastedChildHier = reg.try_get<Hierarchy>(pastedChild);
    REQUIRE(pastedChildHier);
    CHECK(pastedChildHier->parent == pastedParent);
    REQUIRE(S4Snapshot(manager).entities == pre.entities + 2);
    // Pasted child wires under the pasted parent; wiring a child also adds a
    // Hierarchy container on the pasted parent (SceneManager.cpp:4113-4119).
    REQUIRE(S4Snapshot(manager).hierarchy == pre.hierarchy + 2);

    // Undo wiring mirrors SceneEditorUI::PasteCommand: capture + build the
    // command + RecordApplied, then undo removes both pasted entities.
    auto snapshot = manager.CaptureSubtreeSnapshot(paste.createdRoots);
    REQUIRE_FALSE(snapshot.entities.empty());
    auto cmd = MakePasteSubtreesCommand(std::move(snapshot), paste.createdRoots);
    REQUIRE(cmd != nullptr);
    EditorCommandHistory history;
    history.RecordApplied(std::move(cmd), manager, paste.mutation);
    REQUIRE(history.CanUndo());

    const auto undoResult = history.Undo(manager);
    REQUIRE(undoResult.success);
    REQUIRE(static_cast<uint32_t>(manager.FindEntityByUuid(freshParent))
            == static_cast<uint32_t>(entt::null));
    REQUIRE(static_cast<uint32_t>(manager.FindEntityByUuid(freshChild))
            == static_cast<uint32_t>(entt::null));
    // Undo restores the structural state; the authoring revision is monotonic
    // (T26 asserts the same entities-only equality).
    const auto afterUndo = S4Snapshot(manager);
    REQUIRE(afterUndo.entities == pre.entities);
    REQUIRE(afterUndo.uuidIndex == pre.uuidIndex);
    REQUIRE(afterUndo.hierarchy == pre.hierarchy);
    REQUIRE(afterUndo.pic == pre.pic);
    REQUIRE(afterUndo.pmic == pre.pmic);
    REQUIRE(afterUndo.meshes == pre.meshes);
    REQUIRE(afterUndo.materials == pre.materials);
    REQUIRE(afterUndo.textures == pre.textures);
    REQUIRE(afterUndo.docGen == pre.docGen);
    REQUIRE(afterUndo.resourceGen == pre.resourceGen);

    // Duplicate-root phase: {parent, parent, child} dedups to {parent} and the
    // canonical forest is still 2 entities. A fresh clipboard plus a fresh
    // two-slot script must paste cleanly again.
    REQUIRE(state.Copy(manager, {parentUuid, parentUuid, childUuid}, err));

    const UUID freshParent2 = ids.CreateV4();
    const UUID freshChild2 = ids.CreateV4();
    ScriptedUuidProvider scripted2;
    scripted2.script = { freshParent2, freshChild2 };
    manager.SetUuidProvider(&scripted2);

    const auto paste2 = state.PasteWithUuidsForCommand(manager);
    REQUIRE(paste2.mutation.success);
    REQUIRE(paste2.createdRoots.size() == 1);
    REQUIRE(paste2.sourceToDuplicate.size() == 2);
    CHECK(paste2.sourceToDuplicate[0].second == freshParent2);
    CHECK(paste2.sourceToDuplicate[1].second == freshChild2);
    CHECK(scripted2.cursor == 2);
    REQUIRE(static_cast<uint32_t>(manager.FindEntityByUuid(freshParent2))
            != static_cast<uint32_t>(entt::null));
    REQUIRE(static_cast<uint32_t>(manager.FindEntityByUuid(freshChild2))
            != static_cast<uint32_t>(entt::null));
    REQUIRE(S4Snapshot(manager).entities == pre.entities + 2);

    (void)reg;
}

// ============================================================================
// Phase 8 W3, S5 — the overrides query + marker-edit helper API
// (implementation spec, W3 D3.10 and D6; Work step S5).
//
// S5 delivers the query API (IsOverridden / GetOverrides) and the shared
// marker helper (ApplyPrefabMarkerEdits) that every S6 command factory will
// call, WITHOUT wiring any setter yet. Its tests therefore assert the three
// contracts the wiring depends on:
//
//   1. Query correctness on a real instance: empty set until marked, and
//      never falsely marked by existence of the member component alone.
//   2. The helper maintains the canonical (wire-sorted, de-duplicated) order
//      the codec writes and the reader normalizes, and promotes the document
//      schema version (D6) the first time a marker actually appears.
//   3. Loud rejection: an edit whose member is not a prefab member, does not
//      exist, or carries a non-overridable key is rejected (reported in the
//      result) while valid edits still apply — the fail-atomic contract the
//      S6 command layer builds transactional undo on top of.
//
// The no-op/undo/redo breadth (spec tests 8, 9 and the SetCameraPoseState
// two-marker case, test 12) belongs to S6, where the concrete setters are
// wired and command construction computes the edits; S5 proves the helper the
// commands will call.
// ============================================================================

namespace
{

UUID S5NonMemberUuid(S2Fixture& f)
{
    // A plain entity with no PrefabMemberComponent.
    return f.CreateEmpty("Folk");
}

// Adopt a GENUINE v5 .rt2scene (one prefab member, no overrides) into a bare
// manager. The scene-registered member keeps its durable UUID and the document
// keeps schemaVersion == 5 — the state a pre-override (W3-D6) authored file
// has on disk. Returns the member's durable UUID.
UUID S5AdoptV5MemberScene(SceneManager& mgr, const std::filesystem::path& dir,
                          const std::string& tag)
{
    const auto p = dir / "v5.rt2scene";
    S2WriteMemberScene(p, 5, {});
    SceneDocument doc;
    DeterministicUuidProvider ids;
    doc.SetUuidProvider(&ids);
    Error err;
    REQUIRE(SceneSerializer::Load(doc, p, err));
    REQUIRE(doc.metadata.schemaVersion == 5);
    mgr.ReplaceAuthoringDocument(std::move(doc), 0);
    return UUID::Parse("11111111-1111-4111-8111-111111111111");
}

} // namespace

// ---------------------------------------------------------------------------
// Test S5-Q — the query API answers empty/false on a fresh real instance, on
// a non-member entity, and on an absent UUID. The D3.9 contract requires that
// no member starts marked; the query API must not treat "has a member
// component" as "is overridden".
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: override queries are empty until marked")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_s5_query");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;
    const UUID rootUuid = reg.get<EntityIdComponent>(rootHandle).id;
    const UUID childUuid = reg.get<EntityIdComponent>(childHandle).id;
    REQUIRE(rootUuid != UUID::Nil());
    REQUIRE(childUuid != UUID::Nil());

    auto q = f.manager.IsOverridden(rootUuid, S2Key("transform"));
    REQUIRE(q.IsOk());
    REQUIRE_FALSE(q.value);
    auto g = f.manager.GetOverrides(rootUuid);
    REQUIRE(g.IsOk());
    REQUIRE(g.value.empty());
    q = f.manager.IsOverridden(childUuid, S2Key("name"));
    REQUIRE(q.IsOk());
    REQUIRE_FALSE(q.value);
    g = f.manager.GetOverrides(childUuid);
    REQUIRE(g.IsOk());
    REQUIRE(g.value.empty());

    // Typed query distinction: a valid empty member is Ok(false/empty); an
    // ordinary entity is NotPrefabMember; an absent UUID is InvalidEntity.
    const UUID folkUuid = S5NonMemberUuid(f);
    g = f.manager.GetOverrides(folkUuid);
    REQUIRE_FALSE(g.IsOk());
    REQUIRE(g.error.code == rt2::core::Error::NotPrefabMember);
    q = f.manager.IsOverridden(folkUuid, S2Key("transform"));
    REQUIRE_FALSE(q.IsOk());
    REQUIRE(q.error.code == rt2::core::Error::NotPrefabMember);

    g = f.manager.GetOverrides(UUID::Nil());
    REQUIRE_FALSE(g.IsOk());
    REQUIRE(g.error.code == rt2::core::Error::InvalidEntity);
    q = f.manager.IsOverridden(UUID::Nil(), S2Key("transform"));
    REQUIRE_FALSE(q.IsOk());
    REQUIRE(q.error.code == rt2::core::Error::InvalidEntity);
}

// ---------------------------------------------------------------------------
// Test S5-A — ApplyPrefabMarkerEdits adds markers in canonical order, dedups
// re-marks, and promotes the document schema on the first add (D6). The RAW
// registry vector (what the writer emits verbatim) must be wire-sorted and
// unique even though the batch was supplied in a different order: the writer
// only sorts at SceneSerializer.cpp:853-856, it does NOT de-duplicate, so a
// duplicate in the raw vector would survive into the saved file and break the
// save -> load -> save byte round-trip below. Reading through GetOverrides
// would NOT discriminate (it normalises on read), so the assertions here read
// the component directly.
//
// Discrimination faults:
//   Sequence a) insertion fault: insert via push_back instead of the sorted
//   lower_bound insert — the raw vector is not in wire order -> is_sorted
//   CHECK below goes RED. (Inserting a duplicate while keeping sorted order
//   is caught by the byte round-trip at the end.)
//   Sequence b) promotion fault: remove the PromoteSchemaVersion call — the
//   schemaVersion stays below current -> the D6 REQUIRE goes RED.
//   Sequence c) dedup fault: drop the alreadyPresent guard — a re-mark
//   duplicates the key, the raw size check goes RED (and the byte round-trip
//   also fails on reload).
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: marker helper maintains canonical order and promotes schema on first add")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_s5_apply");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;
    const UUID rootUuid = reg.get<EntityIdComponent>(rootHandle).id;
    const auto beforeVersion = f.manager.AuthoringDoc().metadata.schemaVersion;
    REQUIRE(beforeVersion < SceneSerializer::SchemaVersion);

    // Deliberately supplied NOT in wire order (transform > name > motion).
    const auto transform = S2Key("transform");
    const auto name = S2Key("name");
    const auto motion = S2Key("motion");
    auto planRes = f.manager.PreparePrefabMarkerEdits({
        { rootUuid, transform, false, true },
        { rootUuid, name,     false, true },
        { rootUuid, motion,   false, true },
    }, PrefabMarkerDirection::After, beforeVersion, SceneSerializer::SchemaVersion);
    REQUIRE(planRes.IsOk());
    REQUIRE(planRes.value.anyStateChange);
    REQUIRE(planRes.value.members.size() == 1);
    REQUIRE(planRes.value.members[0].member == rootUuid);
    PrefabMarkerApplyResult r = f.manager.CommitPrefabMarkerPlan(std::move(planRes.value));
    REQUIRE(r.anyStateChange);
    REQUIRE(r.appliedMembers == 1);

    // RAW vector must already be canonical — the writer emits it verbatim.
    const auto* raw = reg.try_get<PrefabMemberComponent>(rootHandle);
    REQUIRE(raw);
    REQUIRE(raw->overrides.size() == 3);
    CHECK(std::is_sorted(raw->overrides.begin(), raw->overrides.end(),
          [](const PrefabComponentKey& a, const PrefabComponentKey& b)
          { return a.wire() < b.wire(); }));
    CHECK(raw->overrides[0] == motion);
    CHECK(raw->overrides[1] == name);
    CHECK(raw->overrides[2] == transform);
    auto q1 = f.manager.IsOverridden(rootUuid, transform);
    REQUIRE(q1.IsOk());
    CHECK(q1.value);
    q1 = f.manager.IsOverridden(rootUuid, name);
    REQUIRE(q1.IsOk());
    CHECK(q1.value);
    q1 = f.manager.IsOverridden(rootUuid, motion);
    REQUIRE(q1.IsOk());
    CHECK(q1.value);

    // The same order is what the query API reports.
    const auto overrides = f.manager.GetOverrides(rootUuid);
    REQUIRE(overrides.IsOk());
    REQUIRE(overrides.value == raw->overrides);

    // D6: the document was promoted the moment a marker first appeared.
    REQUIRE(f.manager.AuthoringDoc().metadata.schemaVersion == SceneSerializer::SchemaVersion);

    // Re-marking an already-present key stages a genuine no-op: the plan
    // carries no state change, commit writes nothing and notifies zero times,
    // the vector does not duplicate, and the schema is not re-promoted.
    const auto revisionBefore = f.manager.AuthoringRevision();
    auto noopRes = f.manager.PreparePrefabMarkerEdits({
        { rootUuid, transform, true, true },
    }, PrefabMarkerDirection::After, SceneSerializer::SchemaVersion, SceneSerializer::SchemaVersion);
    REQUIRE(noopRes.IsOk());
    REQUIRE_FALSE(noopRes.value.anyStateChange);
    const auto noopCommit = f.manager.CommitPrefabMarkerPlan(std::move(noopRes.value));
    REQUIRE_FALSE(noopCommit.anyStateChange);
    REQUIRE(reg.try_get<PrefabMemberComponent>(rootHandle)->overrides.size() == 3);
    REQUIRE(f.manager.AuthoringRevision() == revisionBefore);
    REQUIRE(f.manager.AuthoringDoc().metadata.schemaVersion == SceneSerializer::SchemaVersion);

    // Byte round-trip: a raw vector with a duplicate (the write path does not
    // dedup) would write the duplicate, and the reloaded doc would differ once
    // the reader de-duplicates — so save -> load -> save must be byte-equal.
    const auto scenePath0 = dir / "s5a0.rt2scene";
    Error saveErr0;
    REQUIRE(SaveSceneForTest(f.manager.AuthoringDoc(), scenePath0, saveErr0));
    DeterministicUuidProvider p2;
    SceneDocument loaded;
    loaded.SetUuidProvider(&p2);
    Error loadErr;
    REQUIRE(SceneSerializer::Load(loaded, scenePath0, loadErr));
    const auto scenePath1 = dir / "s5a1.rt2scene";
    Error saveErr1;
    std::vector<AssetDiagnostic> diag1;
    REQUIRE(SceneSerializer::Save(loaded, scenePath1, diag1, saveErr1));
    REQUIRE(S2ReadFile(scenePath0) == S2ReadFile(scenePath1));

    // Marking a second member independently is a second genuine add.
    const UUID childUuid = reg.get<EntityIdComponent>(childHandle).id;
    auto childPlan = f.manager.PreparePrefabMarkerEdits({
        { childUuid, S2Key("script"), false, true },
    }, PrefabMarkerDirection::After, SceneSerializer::SchemaVersion, SceneSerializer::SchemaVersion);
    REQUIRE(childPlan.IsOk());
    f.manager.CommitPrefabMarkerPlan(std::move(childPlan.value));
    auto childOverrides = f.manager.GetOverrides(childUuid);
    REQUIRE(childOverrides.IsOk());
    REQUIRE(childOverrides.value.size() == 1);
    CHECK(childOverrides.value[0] == S2Key("script"));
}

// ---------------------------------------------------------------------------
// Test S5-B — removal edits (afterPresent=false) unmark, and undo semantics
// of the payload are satisfied at the helper level: applying the inverse
// presence delta restores the prior membership. A removal of an absent key is
// accepted but mutates nothing (no revision bump, no schema promotion).
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: marker helper removal restores membership and respects the presence delta")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_s5_remove");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;
    const UUID rootUuid = reg.get<EntityIdComponent>(rootHandle).id;

    const auto transform = S2Key("transform");
    const auto name = S2Key("name");
    const auto schemaAfter = SceneSerializer::SchemaVersion;
    const auto schemaBefore = f.manager.AuthoringDoc().metadata.schemaVersion;
    {
        auto plan = f.manager.PreparePrefabMarkerEdits({
            { rootUuid, transform, false, true },
            { rootUuid, name,     false, true },
        }, PrefabMarkerDirection::After, schemaBefore, schemaAfter);
        REQUIRE(plan.IsOk());
        REQUIRE(f.manager.CommitPrefabMarkerPlan(std::move(plan.value)).anyStateChange);
    }

    // Remove only `transform`: name survives, transform gone. The removal
    // payload carries a present source (beforePresent=true), which the After
    // direction validates against the live (present) member.
    auto removePlan = f.manager.PreparePrefabMarkerEdits({
        { rootUuid, transform, true, false },
    }, PrefabMarkerDirection::After, schemaAfter, schemaAfter);
    REQUIRE(removePlan.IsOk());
    REQUIRE(f.manager.CommitPrefabMarkerPlan(std::move(removePlan.value)).anyStateChange);
    auto overrides = f.manager.GetOverrides(rootUuid);
    REQUIRE(overrides.IsOk());
    REQUIRE(overrides.value.size() == 1);
    CHECK(overrides.value[0] == name);
    auto q1 = f.manager.IsOverridden(rootUuid, transform);
    REQUIRE(q1.IsOk());
    CHECK_FALSE(q1.value);
    auto q2 = f.manager.IsOverridden(rootUuid, name);
    REQUIRE(q2.IsOk());
    CHECK(q2.value);

    // Undo of that removal (the S6 layer's restore-beforePresent step): the
    // SAME payload now targets the before side. Before validates
    // afterPresent=false against the live (absent) member, and the key
    // reappears exactly once.
    auto undoPlan = f.manager.PreparePrefabMarkerEdits({
        { rootUuid, transform, true, false },
    }, PrefabMarkerDirection::Before, schemaAfter, schemaAfter);
    REQUIRE(undoPlan.IsOk());
    REQUIRE(f.manager.CommitPrefabMarkerPlan(std::move(undoPlan.value)).anyStateChange);
    auto afterUndo = f.manager.GetOverrides(rootUuid);
    REQUIRE(afterUndo.IsOk());
    REQUIRE(afterUndo.value.size() == 2);

    // Removal of an absent key is a genuine no-op: no revision bump, no
    // further schema churn, no notification.
    const auto revisionBefore = f.manager.AuthoringRevision();
    auto noopPlan = f.manager.PreparePrefabMarkerEdits({
        { rootUuid, S2Key("motion"), false, false },
    }, PrefabMarkerDirection::After, schemaAfter, schemaAfter);
    REQUIRE(noopPlan.IsOk());
    REQUIRE_FALSE(noopPlan.value.anyStateChange);
    const auto noopCommit = f.manager.CommitPrefabMarkerPlan(std::move(noopPlan.value));
    REQUIRE_FALSE(noopCommit.anyStateChange);
    REQUIRE(f.manager.AuthoringRevision() == revisionBefore);
    auto afterNoop = f.manager.GetOverrides(rootUuid);
    REQUIRE(afterNoop.IsOk());
    REQUIRE(afterNoop.value.size() == 2);
}

// ---------------------------------------------------------------------------
// Test S5-C — loud rejection, fail-atomic by construction. A batch mixing a
// valid edit with (a) an absent member UUID, (b) a member that is not a prefab
// member, and (c) a non-overridable key fails the WHOLE batch in Prepare: the
// valid prefix does not land, and membership, schema, dirty flag, and authoring
// revision stay unchanged.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: marker helper rejects bad members and non-overridable keys loudly, atomically")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_s5_reject");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;
    const UUID rootUuid = reg.get<EntityIdComponent>(rootHandle).id;
    const UUID folkUuid = S5NonMemberUuid(f);

    const auto schemaBefore = f.manager.AuthoringDoc().metadata.schemaVersion;
    const bool dirtyBefore = f.manager.IsDirty();
    const auto revisionBefore = f.manager.AuthoringRevision();

    const auto transform = S2Key("transform");
    const auto script = S2Key("script");
    const auto meshRef = S2Key("meshRef"); // overridable bit is false
    auto plan = f.manager.PreparePrefabMarkerEdits({
        { rootUuid, transform, false, true },   // otherwise valid
        { UUID::Nil(), script, false, true },   // absent member entity
        { folkUuid, script, false, true },      // not a prefab member
        { rootUuid, meshRef, false, true },     // excluded key
    }, PrefabMarkerDirection::After, schemaBefore, schemaBefore);
    REQUIRE_FALSE(plan.IsOk());
    REQUIRE(plan.error.code == rt2::core::Error::InvalidEntity); // first bad edit

    // The valid prefix did NOT land: zero partial mutation.
    auto overrides = f.manager.GetOverrides(rootUuid);
    REQUIRE(overrides.IsOk());
    REQUIRE(overrides.value.empty());
    auto scriptQ = f.manager.IsOverridden(rootUuid, script);
    REQUIRE(scriptQ.IsOk());
    REQUIRE_FALSE(scriptQ.value);
    REQUIRE(f.manager.AuthoringDoc().metadata.schemaVersion == schemaBefore);
    REQUIRE(f.manager.IsDirty() == dirtyBefore);
    REQUIRE(f.manager.AuthoringRevision() == revisionBefore);
}

// ---------------------------------------------------------------------------
// Test S5-C2 — typed query key errors. An unknown wire and each of the five
// excluded canonical wires are distinguishable from a valid member query.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: override queries reject unknown and excluded keys as structured errors")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_s5_query_keys");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;
    const UUID rootUuid = reg.get<EntityIdComponent>(rootHandle).id;

    // Unknown wire: not in the frozen table.
    auto q = f.manager.IsOverridden(rootUuid, PrefabComponentKey(std::string_view("noSuchWire"), true));
    REQUIRE_FALSE(q.IsOk());
    REQUIRE(q.error.code == rt2::core::Error::InvalidArgument);
    REQUIRE(q.error.detail.find("noSuchWire") != std::string::npos);

    // All five excluded wires are rejected regardless of the caller-supplied
    // classification bit (forged true still rejected — bit not trusted).
    for (const char* wire : { "meshRef", "primitive", "importedSource",
                              "prefabInstance", "prefabMember" })
    {
        q = f.manager.IsOverridden(rootUuid, PrefabComponentKey(std::string_view(wire), true));
        REQUIRE_FALSE(q.IsOk());
        REQUIRE(q.error.code == rt2::core::Error::InvalidArgument);
        REQUIRE(q.error.detail.find(wire) != std::string::npos);
    }
}

// ===========================================================================
// Checkpoint A - canonical keys and atomic batch discrimination
// ===========================================================================
// The helper guarantees every decision is made against the frozen table and
// as a WHOLE batch. The cases below pin each guarantee's discriminating
// assertion. Rule: every canonical wire keeps its table-derived overridable
// bit regardless of what the caller supplied; a batch fails on the first bad
// edit and neither its valid prefix nor any silent worsening of live state
// survives; duplicate edits never produce a duplicated raw vector; and a
// stored vector that is malformed but canonicalizable is normalized into the
// committed raw registry vector rather than silently left malformed.

// ---------------------------------------------------------------------------
// Test S5-D - key canonicalization in the mutation API. An edit whose wire is
// not in the frozen table, or whose wire canonicalizes to one of the five
// excluded components, fails the WHOLE batch with a structured InvalidArgument
// whose detail names the wire - regardless of the caller's classification bit.
// A valid wire whose bit was forged false must canonicalize (NOT be rejected):
// the committed raw vector carries the table-derived entry.
//
// Discrimination faults (RED observed, then restored GREEN):
//   a) rejection fault: drop the !canonical->overridable() guard in
//   PreparePrefabMarkerEdits (SceneManager.cpp:5478-5482) - the five excluded
//   wires stage silently and REQUIRE_FALSE(plan.IsOk()) below goes RED.
//   b) structured-code fault: change the key-rejection Error to InvalidEntity
//   (SceneManager.cpp:5473-5482) - REQUIRE(plan.error.code == InvalidArgument)
//   below goes RED (excluded and unknown wires collapse into the member code).
//   c) canonicalization fault: store edit.key instead of *canonical
//   (SceneManager.cpp:5537-5538) - the forged-transform CHECK below goes RED.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: marker helper canonicalizes keys and rejects forged classification loudly")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_s5_keys");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;
    const UUID rootUuid = reg.get<EntityIdComponent>(rootHandle).id;

    const auto schemaBefore = f.manager.AuthoringDoc().metadata.schemaVersion;
    const bool dirtyBefore = f.manager.IsDirty();
    const auto revisionBefore = f.manager.AuthoringRevision();

    // Every excluded wire is rejected even with the overridable bit forged to
    // true: the bit is never trusted.
    for (const char* wire : { "meshRef", "primitive", "importedSource",
                              "prefabInstance", "prefabMember" })
    {
        auto plan = f.manager.PreparePrefabMarkerEdits({
            { rootUuid, PrefabComponentKey(std::string_view(wire), true), false, true },
        }, PrefabMarkerDirection::After, schemaBefore, schemaBefore);
        REQUIRE_FALSE(plan.IsOk());
        REQUIRE(plan.error.code == rt2::core::Error::InvalidArgument);
        REQUIRE(plan.error.detail.find(wire) != std::string::npos);
    }

    // Unknown wire (not in the frozen table) is rejected identically.
    auto unknown = f.manager.PreparePrefabMarkerEdits({
        { rootUuid, PrefabComponentKey(std::string_view("noSuchWire"), true), false, true },
    }, PrefabMarkerDirection::After, schemaBefore, schemaBefore);
    REQUIRE_FALSE(unknown.IsOk());
    REQUIRE(unknown.error.code == rt2::core::Error::InvalidArgument);
    REQUIRE(unknown.error.detail.find("noSuchWire") != std::string::npos);

    // All six rejections above are zero-mutation.
    auto overrides = f.manager.GetOverrides(rootUuid);
    REQUIRE(overrides.IsOk());
    REQUIRE(overrides.value.empty());
    REQUIRE(f.manager.AuthoringDoc().metadata.schemaVersion == schemaBefore);
    REQUIRE(f.manager.IsDirty() == dirtyBefore);
    REQUIRE(f.manager.AuthoringRevision() == revisionBefore);

// A valid wire with a forged-false bit canonicalizes: the committed raw
    // vector holds the table-derived transform entry, not a falsely
    // non-overridable copy of the caller's key. The FIRST marker must land at
    // the schema version that can represent overrides (SchemaVersion), so the
    // add rides the v5->v6 transition even though the caller forged the bit.
    auto forged = f.manager.PreparePrefabMarkerEdits({
        { rootUuid, PrefabComponentKey(std::string_view("transform"), false), false, true },
    }, PrefabMarkerDirection::After, schemaBefore, SceneSerializer::SchemaVersion);
    REQUIRE(forged.IsOk());
    REQUIRE(forged.value.anyStateChange);
    REQUIRE(f.manager.CommitPrefabMarkerPlan(std::move(forged.value)).anyStateChange);
    const auto* pm = reg.try_get<PrefabMemberComponent>(rootHandle);
    REQUIRE(pm);
    REQUIRE(pm->overrides.size() == 1);
    CHECK(pm->overrides[0] == S2Key("transform")); // table-derived bit (true)
    REQUIRE(f.manager.AuthoringDoc().metadata.schemaVersion == SceneSerializer::SchemaVersion);
}
// ---------------------------------------------------------------------------
// Test S5-E - a batch mixing a valid edit with a bad one fails atomically in
// EITHER input order with an identical structured error, and the scene's
// markers, schema, dirty flag, and authoring revision stay byte-identical to
// the pre-batch snapshot: the valid prefix never lands.
//
// Discrimination faults (RED observed, then restored GREEN):
//   a) partial-land fault: remove the Prepare-fail return for the bad edit so
//   the staged valid prefix is committed - the overrides/revision asserts
//   below go RED (transform would have landed, schema would have bumped).
//   b) order-dependence fault: keep the bad-key rejection only when the key
//   appears before a member is staged (reject at SceneManager.cpp:5473 but
//   tolerate a bad key reaching an already-staged member) - REQUIRE_FALSE
//   (planA.IsOk()) for the valid-first order goes RED.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: marker helper fails a mixed valid+invalid batch atomically in either order")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_s5_atomic");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;
    const UUID rootUuid = reg.get<EntityIdComponent>(rootHandle).id;

    const auto schemaBefore = f.manager.AuthoringDoc().metadata.schemaVersion;
    const bool dirtyBefore = f.manager.IsDirty();
    const auto revisionBefore = f.manager.AuthoringRevision();

    const PrefabMarkerEdit valid = { rootUuid, S2Key("transform"), false, true };
    const PrefabMarkerEdit invalid = { rootUuid, S2Key("meshRef"), false, true }; // excluded

    auto planA = f.manager.PreparePrefabMarkerEdits({ valid, invalid },
        PrefabMarkerDirection::After, schemaBefore, schemaBefore);
    REQUIRE_FALSE(planA.IsOk());
    REQUIRE(planA.error.code == rt2::core::Error::InvalidArgument);
    REQUIRE(planA.error.detail.find("meshRef") != std::string::npos);

    auto planB = f.manager.PreparePrefabMarkerEdits({ invalid, valid },
        PrefabMarkerDirection::After, schemaBefore, schemaBefore);
    REQUIRE_FALSE(planB.IsOk());
    REQUIRE(planB.error.code == planA.error.code);
    REQUIRE(planB.error.path == planA.error.path);
    REQUIRE(planB.error.detail == planA.error.detail);

    // Byte-identical zero change after both attempts: no marker landed, the
    // schema was not promoted, dirty/revision are untouched.
    auto overrides = f.manager.GetOverrides(rootUuid);
    REQUIRE(overrides.IsOk());
    REQUIRE(overrides.value.empty());
    auto q1 = f.manager.IsOverridden(rootUuid, S2Key("transform"));
    REQUIRE(q1.IsOk());
    REQUIRE_FALSE(q1.value);
    REQUIRE(f.manager.AuthoringDoc().metadata.schemaVersion == schemaBefore);
    REQUIRE(f.manager.IsDirty() == dirtyBefore);
    REQUIRE(f.manager.AuthoringRevision() == revisionBefore);
}

// ---------------------------------------------------------------------------
// Test S5-F - the directional source presence is validated against the live
// pre-batch snapshot. For the After direction the SOURCE side is
// beforePresent; for the Before direction it is afterPresent. A payload whose
// source side contradicts live membership (claiming present when absent, or
// absent when present) fails loudly with zero change - a stale or hand-forged
// boolean is never silently ignored.
//
// Discrimination faults (RED observed, then restored GREEN):
//   a) direction fault: use edit.afterPresent (the target side) as the source
//   for After at SceneManager.cpp:5510 - the After-absent rejection below goes
//   RED (the stale add is treated as a valid add).
//   b) silent-ignore fault: remove the expectedSource mismatch return
//   (SceneManager.cpp:5511-5518) and feed targetPresent straight through -
//   both the absent-claim and the present-claim rejections below go RED.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: marker helper validates the directional source presence with zero change")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_s5_dir");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;
    const UUID rootUuid = reg.get<EntityIdComponent>(rootHandle).id;

    const auto schemaBefore = f.manager.AuthoringDoc().metadata.schemaVersion;
    const bool dirtyBefore = f.manager.IsDirty();
    const auto revisionBefore = f.manager.AuthoringRevision();
    const auto transform = S2Key("transform");
    const auto name = S2Key("name");

    // ABSENT live set: an After edit claiming beforePresent=true is stale on
    // the source side.
    auto afterAbsent = f.manager.PreparePrefabMarkerEdits({
        { rootUuid, transform, true, true },
    }, PrefabMarkerDirection::After, schemaBefore, schemaBefore);
    REQUIRE_FALSE(afterAbsent.IsOk());
    REQUIRE(afterAbsent.error.code == rt2::core::Error::InvalidArgument);
    REQUIRE(afterAbsent.error.detail.find("beforePresent") != std::string::npos);

    // ABSENT live set: a Before edit claiming afterPresent=true is stale.
    auto beforeAbsent = f.manager.PreparePrefabMarkerEdits({
        { rootUuid, transform, false, true },
    }, PrefabMarkerDirection::Before, schemaBefore, schemaBefore);
    REQUIRE_FALSE(beforeAbsent.IsOk());
    REQUIRE(beforeAbsent.error.code == rt2::core::Error::InvalidArgument);
    REQUIRE(beforeAbsent.error.detail.find("afterPresent") != std::string::npos);

    // Zero change from the two failures.
    REQUIRE(f.manager.AuthoringDoc().metadata.schemaVersion == schemaBefore);
    REQUIRE(f.manager.IsDirty() == dirtyBefore);
    REQUIRE(f.manager.AuthoringRevision() == revisionBefore);

// Bring the member up to {name, transform} legitimately (After, source
    // verified absent, riding the fresh-doc -> SchemaVersion promotion).
    auto addPlan = f.manager.PreparePrefabMarkerEdits({
        { rootUuid, name, false, true },
        { rootUuid, transform, false, true },
    }, PrefabMarkerDirection::After, schemaBefore, SceneSerializer::SchemaVersion);
    REQUIRE(addPlan.IsOk());
    REQUIRE(f.manager.CommitPrefabMarkerPlan(std::move(addPlan.value)).anyStateChange);
    const auto schemaMid = f.manager.AuthoringDoc().metadata.schemaVersion;
    const bool dirtyMid = f.manager.IsDirty();
    const auto revisionMid = f.manager.AuthoringRevision();

    // PRESENT live set: After edit claiming beforePresent=false is stale.
    auto afterPresentNow = f.manager.PreparePrefabMarkerEdits({
        { rootUuid, transform, false, true },
    }, PrefabMarkerDirection::After, schemaMid, schemaMid);
    REQUIRE_FALSE(afterPresentNow.IsOk());
    REQUIRE(afterPresentNow.error.code == rt2::core::Error::InvalidArgument);
    REQUIRE(afterPresentNow.error.detail.find("beforePresent") != std::string::npos);

    // PRESENT live set: Before edit claiming afterPresent=false is stale.
    auto beforePresentNow = f.manager.PreparePrefabMarkerEdits({
        { rootUuid, transform, true, false },
    }, PrefabMarkerDirection::Before, schemaMid, schemaMid);
    REQUIRE_FALSE(beforePresentNow.IsOk());
    REQUIRE(beforePresentNow.error.code == rt2::core::Error::InvalidArgument);
    REQUIRE(beforePresentNow.error.detail.find("afterPresent") != std::string::npos);

    // Zero change from the two post-add failures, and neither touched the set.
    REQUIRE(f.manager.AuthoringDoc().metadata.schemaVersion == schemaMid);
    REQUIRE(f.manager.IsDirty() == dirtyMid);
    REQUIRE(f.manager.AuthoringRevision() == revisionMid);
    auto overrides = f.manager.GetOverrides(rootUuid);
    REQUIRE(overrides.IsOk());
    REQUIRE(overrides.value.size() == 2);
}
// ---------------------------------------------------------------------------
// Test S5-G - duplicate policy, input-order-independent. Byte-identical
// duplicate edits for the same (member, canonical wire) coalesce into one
// pending entry; a contradictory duplicate (same wire, different target
// presence) fails the WHOLE batch with an identical structured error in either
// input order, leaving zero change.
//
// Discrimination faults (RED observed, then restored GREEN):
//   a) contradiction fault: drop the member.pendingPresent[idx] != targetPresent
//   check at SceneManager.cpp:5530 (coalesce everything) - REQUIRE_FALSE
//   (planA.IsOk()) for the contradictory order goes RED.
//   b) duplicate-land fault: drop the already-present guard so a re-mark
//   appends a second pending entry - the members[0].target.size() REQUIRE
//   below goes RED (the raw writer would emit the duplicate).
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: marker helper coalesces exact duplicates and rejects contradictory ones order-independently")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_s5_dup");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;
    const UUID rootUuid = reg.get<EntityIdComponent>(rootHandle).id;

const auto schemaBefore = f.manager.AuthoringDoc().metadata.schemaVersion;
    const auto schemaAfter = SceneSerializer::SchemaVersion;
    const auto transform = S2Key("transform");
    const auto name = S2Key("name");

    // Byte-identical duplicates coalesce: one transition, unique committed set.
    // The first marker rides the fresh-doc -> SchemaVersion promotion.
    auto dup = f.manager.PreparePrefabMarkerEdits({
        { rootUuid, transform, false, true },
        { rootUuid, transform, false, true },
    }, PrefabMarkerDirection::After, schemaBefore, schemaAfter);
    REQUIRE(dup.IsOk());
    REQUIRE(dup.value.members.size() == 1);
    REQUIRE(dup.value.members[0].target.size() == 1);
    REQUIRE(f.manager.CommitPrefabMarkerPlan(std::move(dup.value)).anyStateChange);
    auto afterStart = f.manager.GetOverrides(rootUuid);
    REQUIRE(afterStart.IsOk());
    REQUIRE(afterStart.value.size() == 1);
    const auto revisionAfterAdd = f.manager.AuthoringRevision();
    const bool dirtyAfterAdd = f.manager.IsDirty();

    // Contradictory duplicates fail in BOTH orders with the identical
    // structured error (same code, path, and detail). The source side of both
    // edits matches live state (absent, beforePresent=false); only the target
    // presence differs, so the contradiction check -- not the directional
    // source check -- is what rejects the batch.
    auto planA = f.manager.PreparePrefabMarkerEdits({
        { rootUuid, name, false, true },
        { rootUuid, name, false, false },
    }, PrefabMarkerDirection::After, schemaAfter, schemaAfter);
    REQUIRE_FALSE(planA.IsOk());
    REQUIRE(planA.error.code == rt2::core::Error::InvalidArgument);
    REQUIRE(planA.error.path == rootUuid.ToString());
    REQUIRE(planA.error.detail.find("contradictory") != std::string::npos);

    auto planB = f.manager.PreparePrefabMarkerEdits({
        { rootUuid, name, false, false },
        { rootUuid, name, false, true },
    }, PrefabMarkerDirection::After, schemaAfter, schemaAfter);
    REQUIRE_FALSE(planB.IsOk());
    REQUIRE(planB.error.code == planA.error.code);
    REQUIRE(planB.error.path == planA.error.path);
    REQUIRE(planB.error.detail == planA.error.detail);

    // Zero change: neither contradictory attempt mutated or bumped anything.
    auto overrides = f.manager.GetOverrides(rootUuid);
    REQUIRE(overrides.IsOk());
    REQUIRE(overrides.value.size() == 1); // only the coalesced transform
    REQUIRE(f.manager.AuthoringRevision() == revisionAfterAdd);
    REQUIRE(f.manager.IsDirty() == dirtyAfterAdd);
    REQUIRE(f.manager.AuthoringDoc().metadata.schemaVersion == schemaAfter);
}

// ---------------------------------------------------------------------------
// Test S5-H - stored raw-vector policy. A stored vector containing an unknown
// or excluded wire fails Prepare LOUDLY (the malformed state is never silently
// read through). A stored vector that is malformed but CANONICALIZABLE -
// unsorted, duplicated, or carrying a forged classification bit - normalizes
// into the committed raw registry vector even when the requested membership
// edit is otherwise a no-op: anyStateChange is true, commit writes the
// canonical target, and no malformed raw state is left unchanged.
//
// Discrimination faults (RED observed, then restored GREEN):
//   a) loud-fail fault: remove the S5CanonicalizeVector failure propagation in
//   Prepare (SceneManager.cpp:5492-5495) - the unknown and excluded raw-wire
//   REQUIRE_FALSE(plan.IsOk()) blocks below go RED.
//   b) silent-normalize fault: delete the member.raw != member.live
//   anyStateChange line (SceneManager.cpp:5576-5577) - every no-op edit below
//   reports anyStateChange=false and the committed raw vector stays malformed:
//   the REQUIRE(plan.value.anyStateChange) and the raw-size/order CHECKs go RED.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: marker helper normalizes canonicalizable stored vectors even on a membership no-op")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_s5_raw");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;
    const UUID rootUuid = reg.get<EntityIdComponent>(rootHandle).id;

    const auto schema = f.manager.AuthoringDoc().metadata.schemaVersion;
    const auto transform = S2Key("transform");
    const auto name = S2Key("name");
    // The membership edit below is a no-op everywhere: transform is present in
    // each seeded vector after canonicalization, so anyStateChange can only be
    // true when the raw stored vector itself needs normalization.

    // (a) Unknown raw wire: loud failure, zero change.
    reg.try_get<PrefabMemberComponent>(rootHandle)->overrides =
        { PrefabComponentKey(std::string_view("noSuchWire"), true) };
    auto unknownRaw = f.manager.PreparePrefabMarkerEdits({
        { rootUuid, transform, true, true },
    }, PrefabMarkerDirection::After, schema, schema);
    REQUIRE_FALSE(unknownRaw.IsOk());
    REQUIRE(unknownRaw.error.code == rt2::core::Error::InvalidArgument);
    REQUIRE(unknownRaw.error.detail.find("noSuchWire") != std::string::npos);

    // (b) Excluded raw wire: loud failure, zero change.
    reg.try_get<PrefabMemberComponent>(rootHandle)->overrides = { S2Key("meshRef") };
    auto excludedRaw = f.manager.PreparePrefabMarkerEdits({
        { rootUuid, transform, true, true },
    }, PrefabMarkerDirection::After, schema, schema);
    REQUIRE_FALSE(excludedRaw.IsOk());
    REQUIRE(excludedRaw.error.code == rt2::core::Error::InvalidArgument);
    REQUIRE(excludedRaw.error.detail.find("meshRef") != std::string::npos);

// (c) Unsorted raw vector + membership no-op: normalization is a real state
    // change and the committed RAW registry vector becomes canonical. The no-op
    // rides the fresh-doc -> SchemaVersion promotion because the (non-empty)
    // stored set cannot live at the below-current schema.
    reg.try_get<PrefabMemberComponent>(rootHandle)->overrides = { transform, name }; // reverse wire order
    auto sortNoop = f.manager.PreparePrefabMarkerEdits({
        { rootUuid, transform, true, true },
    }, PrefabMarkerDirection::After, schema, SceneSerializer::SchemaVersion);
    REQUIRE(sortNoop.IsOk());
    REQUIRE(sortNoop.value.anyStateChange); // raw != canonical: detected
    REQUIRE(f.manager.CommitPrefabMarkerPlan(std::move(sortNoop.value)).anyStateChange);
    REQUIRE(f.manager.AuthoringDoc().metadata.schemaVersion == SceneSerializer::SchemaVersion);
    auto sortedRaw = reg.try_get<PrefabMemberComponent>(rootHandle);
    REQUIRE(sortedRaw);
    REQUIRE(sortedRaw->overrides.size() == 2);
    CHECK(sortedRaw->overrides[0] == name);       // canonical sort restored
    CHECK(sortedRaw->overrides[1] == transform);

    // (d) Duplicated raw vector collapses to a unique set.
    reg.try_get<PrefabMemberComponent>(rootHandle)->overrides = { transform, transform };
    auto dupNoop = f.manager.PreparePrefabMarkerEdits({
        { rootUuid, transform, true, true },
    }, PrefabMarkerDirection::After, SceneSerializer::SchemaVersion, SceneSerializer::SchemaVersion);
    REQUIRE(dupNoop.IsOk());
    REQUIRE(dupNoop.value.anyStateChange);
    REQUIRE(f.manager.CommitPrefabMarkerPlan(std::move(dupNoop.value)).anyStateChange);
    auto dedupRaw = reg.try_get<PrefabMemberComponent>(rootHandle);
    REQUIRE(dedupRaw);
    REQUIRE(dedupRaw->overrides.size() == 1);
    CHECK(dedupRaw->overrides[0] == transform);

    // (e) Forged classification bit re-derives from the frozen table.
    reg.try_get<PrefabMemberComponent>(rootHandle)->overrides =
        { PrefabComponentKey(std::string_view("transform"), false) };
    auto forgedRawNoop = f.manager.PreparePrefabMarkerEdits({
        { rootUuid, transform, true, true },
    }, PrefabMarkerDirection::After, SceneSerializer::SchemaVersion, SceneSerializer::SchemaVersion);
    REQUIRE(forgedRawNoop.IsOk());
    REQUIRE(forgedRawNoop.value.anyStateChange);
    REQUIRE(f.manager.CommitPrefabMarkerPlan(std::move(forgedRawNoop.value)).anyStateChange);
    auto canonicalRaw = reg.try_get<PrefabMemberComponent>(rootHandle);
    REQUIRE(canonicalRaw);
    REQUIRE(canonicalRaw->overrides.size() == 1);
    CHECK(canonicalRaw->overrides[0] == transform); // table-derived bit (true)
}

// ===========================================================================
// Checkpoint B - reversible schema and stale/forged-plan integrity
// ===========================================================================
// The plan/commit split carries the command-captured before/after schema pair
// through the SAME staged operation (W3 D3.6/D3.10): the After direction
// targets the command-after schema (execute) and the Before direction targets
// the command-before schema (undo/restore), so undoing a first add restores
// the captured prior version. The cases below pin the schema-transition rules
// (source==live, marker-version floor, downgrade only when no override
// remains anywhere) and that Commit re-validates the WHOLE plan against live
// state before any write, because every PrefabMarkerPlan field is public and
// a hand-forged or stale plan must fail loudly with zero mutation.

// ---------------------------------------------------------------------------
// Test S5-I - the reversible first-marker cycle on a GENUINE v5 document.
// After applies the first marker and promotes v5 -> v6 in one atomic commit;
// Before then applies the inverse presence side of the SAME payload and
// restores the captured v5. The raw registry vector, schema version, dirty
// flag, and authoring revision are asserted across both commits.
//
// Discrimination faults (RED observed, then restored GREEN):
//   a) directional fault: swap the source/target schema transport in Prepare
//   (SceneManager.cpp:5540-5543) so Before targets the command-after schema -
//   the undo leaves the document at v6 and REQUIRE(undo.afterSchemaVersion
//   == 5) / the schema CHECK below goes RED.
//   b) inverse-side fault: apply afterPresent instead of beforePresent for the
//   Before direction (SceneManager.cpp:5642) - the undone key stays present
//   and REQUIRE(afterUndo->overrides.empty()) goes RED.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: marker helper undo of a first add restores the captured v5")
{
    SceneManager mgr;
    const auto dir = S2UniqueTempDir("p8w3_s5_v5cycle");
    const UUID memberUuid = S5AdoptV5MemberScene(mgr, dir, "p8w3_s5_v5cycle");
    auto& reg = mgr.GetECS().registry;
    const auto handle = mgr.FindEntityByUuid(memberUuid);
    REQUIRE(static_cast<uint32_t>(handle) != static_cast<uint32_t>(entt::null));
    REQUIRE(reg.all_of<PrefabMemberComponent>(handle));

    const auto transform = S2Key("transform");
    const auto revision0 = mgr.AuthoringRevision();
    const bool dirty0 = mgr.IsDirty();

    // Execute: After direction applies afterPresent=true; the first marker
    // must land at the schema version that can represent overrides (v6).
    auto execPlan = mgr.PreparePrefabMarkerEdits({
        { memberUuid, transform, false, true },
    }, PrefabMarkerDirection::After, /*before*/ 5,
      /*after*/ SceneSerializer::SchemaVersion);
    REQUIRE(execPlan.IsOk());
    PrefabMarkerApplyResult exec = mgr.CommitPrefabMarkerPlan(std::move(execPlan.value));
    REQUIRE(exec.anyStateChange);
    REQUIRE(exec.appliedMembers == 1);
    REQUIRE(exec.beforeSchemaVersion == 5);
    REQUIRE(exec.afterSchemaVersion == SceneSerializer::SchemaVersion);

    const auto* raw = reg.try_get<PrefabMemberComponent>(handle);
    REQUIRE(raw);
    REQUIRE(raw->overrides.size() == 1);
    CHECK(raw->overrides[0] == transform);
    CHECK(mgr.AuthoringDoc().metadata.schemaVersion == SceneSerializer::SchemaVersion);
    CHECK(mgr.IsDirty());
    CHECK(mgr.AuthoringRevision() == revision0 + 1); // exactly one revision bump
    auto q = mgr.IsOverridden(memberUuid, transform);
    REQUIRE(q.IsOk());
    CHECK(q.value);

    // Undo: the SAME payload with Before direction applies beforePresent=false
    // (removes the key) and targets the captured command-before schema (v5).
    auto undoPlan = mgr.PreparePrefabMarkerEdits({
        { memberUuid, transform, false, true },
    }, PrefabMarkerDirection::Before, /*before*/ 5,
      /*after*/ SceneSerializer::SchemaVersion);
    REQUIRE(undoPlan.IsOk());
    REQUIRE(undoPlan.value.targetSchemaVersion == 5);
    PrefabMarkerApplyResult undo = mgr.CommitPrefabMarkerPlan(std::move(undoPlan.value));
    REQUIRE(undo.anyStateChange);
    REQUIRE(undo.beforeSchemaVersion == SceneSerializer::SchemaVersion);
    REQUIRE(undo.afterSchemaVersion == 5);

    auto afterUndo = reg.try_get<PrefabMemberComponent>(handle);
    REQUIRE(afterUndo);
    REQUIRE(afterUndo->overrides.empty());
    REQUIRE(mgr.AuthoringDoc().metadata.schemaVersion == 5); // captured v5 restored
    CHECK(mgr.AuthoringRevision() == revision0 + 2);         // exactly one more bump
    CHECK(mgr.IsDirty());
    q = mgr.IsOverridden(memberUuid, transform);
    REQUIRE(q.IsOk());
    CHECK_FALSE(q.value);
    auto g = mgr.GetOverrides(memberUuid);
    REQUIRE(g.IsOk());
    REQUIRE(g.value.empty());

    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Test S5-J - Prepare rejects every schema-transition violation it is asked to
// police, all with zero change: (a) a caller-supplied directional source
// schema that differs from the live document (stale or hand-forged schema
// pair); (b) an empty edit list cannot drive a schema-only transition;
// (c) a non-empty override target cannot be stored below SceneSerializer::
// SchemaVersion, so a first marker cannot target a below-current schema; and
// (d) a downgrade below the live schema is valid only when NO override remains
// anywhere in the document after the batch applies.
//
// Discrimination faults (RED observed, then restored GREEN):
//   a) drop the Prepare source-schema equality return (SceneManager.cpp:
//   5550-5555) - the mismatch case below returns Ok and REQUIRE_FALSE goes RED.
//   b) drop the empty-edit guard (SceneManager.cpp:5561-5564) - an empty list
//   with a non-constant schema pair stages silently; REQUIRE_FALSE goes RED.
//   c) drop the marker-version floor check (SceneManager.cpp:5445-5452) - the
//   below-floor add stages; the schema/override zero-change CHECKs go RED.
//   d) drop the S5RemainingOverrides gate (SceneManager.cpp:5454-5466) - the
//   outside-override downgrade stages; the schema CHECK below goes RED.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: Prepare rejects schema-transition violations with zero change")
{
    SceneManager mgr;
    const auto dir = S2UniqueTempDir("p8w3_s5_schema");
    const UUID memberUuid = S5AdoptV5MemberScene(mgr, dir, "p8w3_s5_schema");
    auto& reg = mgr.GetECS().registry;
    const auto handle = mgr.FindEntityByUuid(memberUuid);
    REQUIRE(static_cast<uint32_t>(handle) != static_cast<uint32_t>(entt::null));
    REQUIRE(reg.all_of<PrefabMemberComponent>(handle));

    const auto transform = S2Key("transform");
    const auto schema0 = mgr.AuthoringDoc().metadata.schemaVersion; // 5
    REQUIRE(schema0 == 5);
    const bool dirty0 = mgr.IsDirty();
    const auto revision0 = mgr.AuthoringRevision();
    const auto checkZeroChange = [&] {
        CHECK(mgr.AuthoringDoc().metadata.schemaVersion == schema0);
        CHECK(mgr.IsDirty() == dirty0);
        CHECK(mgr.AuthoringRevision() == revision0);
        auto g = mgr.GetOverrides(memberUuid);
        REQUIRE(g.IsOk());
        REQUIRE(g.value.empty());
    };

    // (a) live/source schema mismatch: claimed source (v6) != live (v5).
    auto mismatch = mgr.PreparePrefabMarkerEdits({
        { memberUuid, transform, false, true },
    }, PrefabMarkerDirection::After, SceneSerializer::SchemaVersion,
      SceneSerializer::SchemaVersion);
    REQUIRE_FALSE(mismatch.IsOk());
    REQUIRE(mismatch.error.code == rt2::core::Error::SchemaVersion);
    checkZeroChange();

    // (b) an empty edit list cannot drive a schema-only transition.
    auto emptyOnly = mgr.PreparePrefabMarkerEdits({}, PrefabMarkerDirection::After,
        5, SceneSerializer::SchemaVersion);
    REQUIRE_FALSE(emptyOnly.IsOk());
    REQUIRE(emptyOnly.error.code == rt2::core::Error::InvalidArgument);
    CHECK(emptyOnly.error.detail.find("empty edit list") != std::string::npos);
    checkZeroChange();

    // (c) first marker below the marker schema: a non-empty target at v5.
    auto belowFloor = mgr.PreparePrefabMarkerEdits({
        { memberUuid, transform, false, true },
    }, PrefabMarkerDirection::After, /*before*/ 5, /*after*/ 5);
    REQUIRE_FALSE(belowFloor.IsOk());
    REQUIRE(belowFloor.error.code == rt2::core::Error::InvalidArgument);
    CHECK(belowFloor.error.detail.find("at least") != std::string::npos);
    checkZeroChange();
    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Test S5-J(2) - the downgrade gate: a Before undo that targets the captured
// before schema is rejected while ANY override remains elsewhere in the
// document, and the SAME undo succeeds once the outside member is reverted
// too - proving the gate is "no override remains anywhere", not "downgrades
// are always illegal".
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: Prepare rejects a downgrade while an override remains elsewhere")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_s5_downgrade");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;
    const UUID rootUuid = reg.get<EntityIdComponent>(rootHandle).id;
    const UUID childUuid = reg.get<EntityIdComponent>(childHandle).id;
    const auto name = S2Key("name");
    const auto script = S2Key("script");
    const auto schema0 = f.manager.AuthoringDoc().metadata.schemaVersion;

    // Build genuine v6 state: root = {name}, child = {script}.
    auto a = f.manager.PreparePrefabMarkerEdits({ { rootUuid, name, false, true } },
        PrefabMarkerDirection::After, schema0, SceneSerializer::SchemaVersion);
    REQUIRE(a.IsOk());
    REQUIRE(f.manager.CommitPrefabMarkerPlan(std::move(a.value)).anyStateChange);
    auto b = f.manager.PreparePrefabMarkerEdits({ { childUuid, script, false, true } },
        PrefabMarkerDirection::After, SceneSerializer::SchemaVersion, SceneSerializer::SchemaVersion);
    REQUIRE(b.IsOk());
    REQUIRE(f.manager.CommitPrefabMarkerPlan(std::move(b.value)).anyStateChange);
    REQUIRE(f.manager.AuthoringDoc().metadata.schemaVersion == SceneSerializer::SchemaVersion);

    const bool dirty6 = f.manager.IsDirty();
    const auto revision6 = f.manager.AuthoringRevision();

    // Undo root at the captured before schema (v5): the child still holds
    // {script}, so the downgrade would strand-and-drop that marker.
    auto blocked = f.manager.PreparePrefabMarkerEdits({ { rootUuid, name, false, true } },
        PrefabMarkerDirection::Before, /*before*/ 5, /*after*/ SceneSerializer::SchemaVersion);
    REQUIRE_FALSE(blocked.IsOk());
    REQUIRE(blocked.error.code == rt2::core::Error::InvalidArgument);
    CHECK(blocked.error.detail.find("remains elsewhere") != std::string::npos);

    // Zero change: schema, revision, dirty, and BOTH override sets untouched.
    CHECK(f.manager.AuthoringDoc().metadata.schemaVersion == SceneSerializer::SchemaVersion);
    CHECK(f.manager.AuthoringRevision() == revision6);
    CHECK(f.manager.IsDirty() == dirty6);
    auto rootG = f.manager.GetOverrides(rootUuid);
    REQUIRE(rootG.IsOk());
    REQUIRE(rootG.value.size() == 1);
    auto childG = f.manager.GetOverrides(childUuid);
    REQUIRE(childG.IsOk());
    REQUIRE(childG.value.size() == 1);

    // Control: revert the outside member first (constant-schema removal), then
    // the same undo succeeds and restores v5.
    auto childUndo = f.manager.PreparePrefabMarkerEdits({ { childUuid, script, false, true } },
        PrefabMarkerDirection::Before, SceneSerializer::SchemaVersion, SceneSerializer::SchemaVersion);
    REQUIRE(childUndo.IsOk());
    REQUIRE(f.manager.CommitPrefabMarkerPlan(std::move(childUndo.value)).anyStateChange);
    auto rootUndo = f.manager.PreparePrefabMarkerEdits({ { rootUuid, name, false, true } },
        PrefabMarkerDirection::Before, /*before*/ 5, /*after*/ SceneSerializer::SchemaVersion);
    REQUIRE(rootUndo.IsOk());
    REQUIRE(rootUndo.value.targetSchemaVersion == 5);
    REQUIRE(f.manager.CommitPrefabMarkerPlan(std::move(rootUndo.value)).anyStateChange);
    REQUIRE(f.manager.AuthoringDoc().metadata.schemaVersion == 5);
    auto rootG2 = f.manager.GetOverrides(rootUuid);
    REQUIRE(rootG2.IsOk());
    REQUIRE(rootG2.value.empty());
    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Test S5-K - Commit is infallible only against the SAME document the plan was
// prepared against. A plan made stale by an INDEPENDENT change - a member
// override vector moved by another commit, or the document schema rewritten
// underneath - fails loudly with zero mutation, before any write.
//
// Discrimination faults (RED observed, then restored GREEN):
//   a) drop the Commit source==live re-derivation (SceneManager.cpp:5780-5789)
//   - the independently-moved member writes the stale target; overrides stays
//   {name} below goes RED.
//   b) drop the Commit schema re-validation (SceneManager.cpp:5816-5819) - the
//   schema-rewritten commit writes the child marker; REQUIRE(r2.error.code ==
//   SchemaVersion) below goes RED (and the schema/override CHECKs go RED).
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: Commit rejects a stale plan with zero mutation")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_s5_stale");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;
    const UUID rootUuid = reg.get<EntityIdComponent>(rootHandle).id;
    const UUID childUuid = reg.get<EntityIdComponent>(childHandle).id;
    const auto name = S2Key("name");
    const auto script = S2Key("script");
    const auto transform = S2Key("transform");
    const auto schema0 = f.manager.AuthoringDoc().metadata.schemaVersion;

    // (a) member vector moved independently between staging and commit.
    auto p1 = f.manager.PreparePrefabMarkerEdits({ { rootUuid, transform, false, true } },
        PrefabMarkerDirection::After, schema0, SceneSerializer::SchemaVersion);
    REQUIRE(p1.IsOk());
    auto pInd = f.manager.PreparePrefabMarkerEdits({ { rootUuid, name, false, true } },
        PrefabMarkerDirection::After, schema0, SceneSerializer::SchemaVersion);
    REQUIRE(pInd.IsOk());
    REQUIRE(f.manager.CommitPrefabMarkerPlan(std::move(pInd.value)).anyStateChange);
    const auto revisionA = f.manager.AuthoringRevision();
    const bool dirtyA = f.manager.IsDirty();

    PrefabMarkerApplyResult r = f.manager.CommitPrefabMarkerPlan(std::move(p1.value));
    REQUIRE_FALSE(r.error.IsOk());
    REQUIRE(r.error.code == rt2::core::Error::InvalidArgument);
    CHECK(r.error.detail.find("changed since the plan was prepared") != std::string::npos);
    // Zero mutation: the independent {name} survives, schema not moved, no bump.
    const auto* rawA = reg.try_get<PrefabMemberComponent>(rootHandle);
    REQUIRE(rawA);
    REQUIRE(rawA->overrides.size() == 1);
    CHECK(rawA->overrides[0] == name);
    CHECK(f.manager.AuthoringDoc().metadata.schemaVersion == SceneSerializer::SchemaVersion);
    CHECK(f.manager.AuthoringRevision() == revisionA);
    CHECK(f.manager.IsDirty() == dirtyA);

    // (b) document schema rewritten independently between staging and commit.
    auto p2 = f.manager.PreparePrefabMarkerEdits({ { childUuid, script, false, true } },
        PrefabMarkerDirection::After, SceneSerializer::SchemaVersion, SceneSerializer::SchemaVersion);
    REQUIRE(p2.IsOk());
    f.manager.AuthoringDoc().metadata.schemaVersion = 4; // external rewrite underneath
    const auto revisionB = f.manager.AuthoringRevision();
    const bool dirtyB = f.manager.IsDirty();
    PrefabMarkerApplyResult r2 = f.manager.CommitPrefabMarkerPlan(std::move(p2.value));
    REQUIRE_FALSE(r2.error.IsOk());
    REQUIRE(r2.error.code == rt2::core::Error::SchemaVersion);
    // Zero mutation: the child marker was never written, revision untouched.
    auto childRaw = reg.try_get<PrefabMemberComponent>(childHandle);
    REQUIRE(childRaw);
    REQUIRE(childRaw->overrides.empty());
    REQUIRE(f.manager.AuthoringDoc().metadata.schemaVersion == 4);
    CHECK(f.manager.AuthoringRevision() == revisionB);
    CHECK(f.manager.IsDirty() == dirtyB);
    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Test S5-L - PrefabMarkerPlan fields are PUBLIC, so Commit must treat every
// plan as untrusted and re-validate the WHOLE thing against live state: a
// forged target holding an excluded or unknown wire, a non-canonical target, a
// forged source, and an arbitrary (fabricated) schema pair all fail loudly
// with zero mutation. Commit also re-derives anyStateChange: a forged
// anyStateChange=true on a genuine no-op still commits nothing and notifies
// zero times.
//
// Discrimination faults (RED observed, then restored GREEN):
//   a) accept forged targets: drop the Commit target re-canonicalization /
//   identity check (SceneManager.cpp:5795-5804) - the meshRef and noSuchWire
//   targets below commit and REQUIRE_FALSE(r.error.IsOk()) goes RED.
//   b) trust forged source: drop the Commit source check
//   (SceneManager.cpp:5780-5789) - the forged-source plan writes {transform};
//   REQUIRE(rawForge->overrides.empty()) goes RED.
//   c) trust fabricated schema: drop the Commit schema-only re-check
//   (SceneManager.cpp:5828-5830) - the fabricated 4->6 promotion with no
//   member change bumps schema; the schema CHECK goes RED.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: Commit rejects forged plan fields with zero mutation")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_s5_forge");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;
    const UUID rootUuid = reg.get<EntityIdComponent>(rootHandle).id;
    const auto schema0 = f.manager.AuthoringDoc().metadata.schemaVersion;
    const bool dirty0 = f.manager.IsDirty();
    const auto revision0 = f.manager.AuthoringRevision();

    const auto forge = [&](std::vector<PrefabComponentKey> source,
                           std::vector<PrefabComponentKey> target,
                           std::uint32_t sourceSchema,
                           std::uint32_t targetSchema)
    {
        PrefabMarkerPlan plan;
        plan.direction = PrefabMarkerDirection::After;
        plan.sourceSchemaVersion = sourceSchema;
        plan.targetSchemaVersion = targetSchema;
        plan.documentGeneration = f.manager.DocumentGeneration();
        plan.anyStateChange = target != source || sourceSchema != targetSchema;
        PrefabMarkerPlan::MemberTransition t;
        t.member = rootUuid;
        t.source = std::move(source);
        t.target = std::move(target);
        plan.members.push_back(std::move(t));
        return f.manager.CommitPrefabMarkerPlan(std::move(plan));
    };
    const auto checkZero = [&] {
        CHECK(f.manager.AuthoringDoc().metadata.schemaVersion == schema0);
        CHECK(f.manager.IsDirty() == dirty0);
        CHECK(f.manager.AuthoringRevision() == revision0);
        auto g = f.manager.GetOverrides(rootUuid);
        REQUIRE(g.IsOk());
        REQUIRE(g.value.empty());
    };

    // (a) forged excluded target wire.
    auto r1 = forge({}, { S2Key("meshRef") }, schema0, schema0);
    REQUIRE_FALSE(r1.error.IsOk());
    REQUIRE(r1.error.code == rt2::core::Error::InvalidArgument);
    CHECK(r1.error.detail.find("meshRef") != std::string::npos);
    checkZero();

    // (b) forged unknown target wire.
    auto r2 = forge({}, { PrefabComponentKey(std::string_view("noSuchWire"), true) },
        schema0, schema0);
    REQUIRE_FALSE(r2.error.IsOk());
    REQUIRE(r2.error.code == rt2::core::Error::InvalidArgument);
    CHECK(r2.error.detail.find("noSuchWire") != std::string::npos);
    checkZero();

    // (c) non-canonical target (both wires real, so it canonicalizes, but the
    // staged order is not the canonical one).
    auto r3 = forge({}, { S2Key("transform"), S2Key("name") }, schema0, schema0);
    REQUIRE_FALSE(r3.error.IsOk());
    REQUIRE(r3.error.code == rt2::core::Error::InvalidArgument);
    CHECK(r3.error.detail.find("not canonical") != std::string::npos);
    checkZero();

    // (d) forged source: claims {name} was the pre-batch set; live is empty,
    // and the forged target {transform} WOULD have written a marker.
    auto r4 = forge({ S2Key("name") }, { S2Key("transform") }, schema0, schema0);
    REQUIRE_FALSE(r4.error.IsOk());
    REQUIRE(r4.error.code == rt2::core::Error::InvalidArgument);
    CHECK(r4.error.detail.find("changed since the plan was prepared") != std::string::npos);
    checkZero();

    // (e) fabricated schema pair with no member change: a 4->6 promotion whose
    // target is byte-identical to every live vector.
    auto r5 = forge({}, {}, schema0, SceneSerializer::SchemaVersion);
    REQUIRE_FALSE(r5.error.IsOk());
    REQUIRE(r5.error.code == rt2::core::Error::InvalidArgument);
    CHECK(r5.error.detail.find("schema-only transition") != std::string::npos);
    checkZero();

    // (f) source-schema pair that does not match the live document.
    auto r6 = forge({}, { S2Key("transform") }, SceneSerializer::SchemaVersion,
        SceneSerializer::SchemaVersion);
    REQUIRE_FALSE(r6.error.IsOk());
    REQUIRE(r6.error.code == rt2::core::Error::SchemaVersion);
    checkZero();

    // (g) a forged anyStateChange=true on a genuine no-op: Commit re-derives
    // the decision and commits nothing, notifies zero times.
    auto p = f.manager.PreparePrefabMarkerEdits({ { rootUuid, S2Key("motion"), false, true } },
        PrefabMarkerDirection::After, schema0, SceneSerializer::SchemaVersion);
    REQUIRE(p.IsOk());
    REQUIRE(f.manager.CommitPrefabMarkerPlan(std::move(p.value)).anyStateChange);
    const auto revisionAfter = f.manager.AuthoringRevision();
    auto noop = f.manager.PreparePrefabMarkerEdits({ { rootUuid, S2Key("motion"), true, true } },
        PrefabMarkerDirection::After, SceneSerializer::SchemaVersion, SceneSerializer::SchemaVersion);
    REQUIRE(noop.IsOk());
    REQUIRE_FALSE(noop.value.anyStateChange);
    noop.value.anyStateChange = true; // forged
    PrefabMarkerApplyResult r7 = f.manager.CommitPrefabMarkerPlan(std::move(noop.value));
    REQUIRE_FALSE(r7.anyStateChange); // decision re-derived, not trusted
    REQUIRE(f.manager.AuthoringRevision() == revisionAfter);
    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Test S5-M - notification/revision accounting. A genuine no-op commits zero
// revision, dirty, schema, and notification changes. A successful two-member
// plan changes BOTH members with EXACTLY ONE revision bump (NotifyAuthoring
// Changed fires once). A stale/failed plan bumps zero times (covered per-case
// in S5-K/S5-L too; re-pinned here end-to-end).
//
// Discrimination faults (RED observed, then restored GREEN):
//   a) notify-per-member: call NotifyAuthoringChanged() inside the commit write
//   loop (SceneManager.cpp:5835-5840) - the two-member plan below bumps twice
//   and REQUIRE(... == revBefore + 1) goes RED.
//   b) notify-noop: remove the Commit no-op early return (SceneManager.cpp:
//   5831-5832) - the genuine no-op below notifies and CHECK(revision unchanged)
//   goes RED.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: marker helper two-member plan bumps the revision exactly once; no-op bumps zero")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_s5_rev");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;
    const UUID rootUuid = reg.get<EntityIdComponent>(rootHandle).id;
    const UUID childUuid = reg.get<EntityIdComponent>(childHandle).id;
    const auto name = S2Key("name");
    const auto script = S2Key("script");
    const auto transform = S2Key("transform");
    const auto schema0 = f.manager.AuthoringDoc().metadata.schemaVersion;

    // Bring the doc to v6 with {name} on the root.
    auto first = f.manager.PreparePrefabMarkerEdits({ { rootUuid, name, false, true } },
        PrefabMarkerDirection::After, schema0, SceneSerializer::SchemaVersion);
    REQUIRE(first.IsOk());
    REQUIRE(f.manager.CommitPrefabMarkerPlan(std::move(first.value)).anyStateChange);
    const auto revisionAfterAdd = f.manager.AuthoringRevision();
    const bool dirtyAfterAdd = f.manager.IsDirty();

    // (a) genuine no-op: re-mark name at the current schema.
    auto noop = f.manager.PreparePrefabMarkerEdits({ { rootUuid, name, true, true } },
        PrefabMarkerDirection::After, SceneSerializer::SchemaVersion, SceneSerializer::SchemaVersion);
    REQUIRE(noop.IsOk());
    REQUIRE_FALSE(noop.value.anyStateChange);
    PrefabMarkerApplyResult rn = f.manager.CommitPrefabMarkerPlan(std::move(noop.value));
    REQUIRE_FALSE(rn.anyStateChange);
    REQUIRE(rn.appliedMembers == 0);
    REQUIRE(rn.beforeSchemaVersion == SceneSerializer::SchemaVersion);
    REQUIRE(rn.afterSchemaVersion == SceneSerializer::SchemaVersion);
    CHECK(f.manager.AuthoringRevision() == revisionAfterAdd);
    CHECK(f.manager.IsDirty() == dirtyAfterAdd);
    CHECK(f.manager.AuthoringDoc().metadata.schemaVersion == SceneSerializer::SchemaVersion);
    auto noopG = f.manager.GetOverrides(rootUuid);
    REQUIRE(noopG.IsOk());
    REQUIRE(noopG.value.size() == 1);

    // (b) one plan, two members, exactly one revision bump and one notification.
    const auto revisionBeforeTwo = f.manager.AuthoringRevision();
    auto two = f.manager.PreparePrefabMarkerEdits({
        { rootUuid, transform, false, true },
        { childUuid, script, false, true },
    }, PrefabMarkerDirection::After, SceneSerializer::SchemaVersion, SceneSerializer::SchemaVersion);
    REQUIRE(two.IsOk());
    REQUIRE(two.value.members.size() == 2);
    PrefabMarkerApplyResult rt = f.manager.CommitPrefabMarkerPlan(std::move(two.value));
    REQUIRE(rt.anyStateChange);
    REQUIRE(rt.appliedMembers == 2);
    REQUIRE(f.manager.AuthoringRevision() == revisionBeforeTwo + 1);
    REQUIRE(f.manager.IsDirty());
    REQUIRE(f.manager.AuthoringDoc().metadata.schemaVersion == SceneSerializer::SchemaVersion);
    auto rootT = f.manager.GetOverrides(rootUuid);
    REQUIRE(rootT.IsOk());
    REQUIRE(rootT.value.size() == 2);
    CHECK(rootT.value[0] == name);
    CHECK(rootT.value[1] == transform);
    auto childT = f.manager.GetOverrides(childUuid);
    REQUIRE(childT.IsOk());
    REQUIRE(childT.value.size() == 1);
    CHECK(childT.value[0] == script);

    // (c) a failed (stale) commit bumps zero times: stage a plan, move the
    // member underneath with an unrelated commit, then commit the stale plan -
    // it fails and the revision is unchanged from the unrelated commit.
    const auto motion = S2Key("motion");
    auto stale = f.manager.PreparePrefabMarkerEdits({ { rootUuid, motion, false, true } },
        PrefabMarkerDirection::After, SceneSerializer::SchemaVersion, SceneSerializer::SchemaVersion);
    REQUIRE(stale.IsOk());
    auto mover = f.manager.PreparePrefabMarkerEdits({ { rootUuid, S2Key("light"), false, true } },
        PrefabMarkerDirection::After, SceneSerializer::SchemaVersion, SceneSerializer::SchemaVersion);
    REQUIRE(mover.IsOk());
    REQUIRE(f.manager.CommitPrefabMarkerPlan(std::move(mover.value)).anyStateChange);
    const auto revisionAfterMover = f.manager.AuthoringRevision();
    PrefabMarkerApplyResult rs = f.manager.CommitPrefabMarkerPlan(std::move(stale.value));
    REQUIRE_FALSE(rs.error.IsOk());
    REQUIRE(rs.error.code == rt2::core::Error::InvalidArgument);
    REQUIRE(f.manager.AuthoringRevision() == revisionAfterMover); // zero bump
    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Test S5-N - raw source/target normalization re-confirmed through the Commit
// revalidation seam. A stored vector that is malformed but canonicalizable
// (unsorted, duplicated, or carrying a forged classification bit) makes a
// membership no-op a REAL state change: the raw registry vector is normalized
// to the canonical wire-sorted, de-duplicated form on commit, because the
// writer emits the raw vector verbatim. The staged plan's source is the
// CANONICAL pre-batch set and the committed target is canonical, so the
// revalidation still accepts the normalization-only plan and rewrites only the
// stored bytes.
//
// Discrimination fault (RED observed, then restored GREEN): drop the Commit
// per-member rawLive != target detection (SceneManager.cpp:5824-5826) and the
// normalization-only plans below report no state change; REQUIRE(commit
// .anyStateChange) and the raw-order CHECKs go RED.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: Commit normalizes raw originals through plan revalidation")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_s5_norm");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;
    const UUID rootUuid = reg.get<EntityIdComponent>(rootHandle).id;
    const auto motion = S2Key("motion");
    const auto name = S2Key("name");
    const auto transform = S2Key("transform");
    const auto schema0 = f.manager.AuthoringDoc().metadata.schemaVersion;

    // Promote the document to the marker schema first.
    auto first = f.manager.PreparePrefabMarkerEdits({ { rootUuid, motion, false, true } },
        PrefabMarkerDirection::After, schema0, SceneSerializer::SchemaVersion);
    REQUIRE(first.IsOk());
    REQUIRE(f.manager.CommitPrefabMarkerPlan(std::move(first.value)).anyStateChange);
    const auto revisionAfter = f.manager.AuthoringRevision();

    // (a) unsorted raw vector + membership no-op: rewrites in canonical order.
    reg.try_get<PrefabMemberComponent>(rootHandle)->overrides = { transform, motion };
    auto sortNoop = f.manager.PreparePrefabMarkerEdits({ { rootUuid, motion, true, true } },
        PrefabMarkerDirection::After, SceneSerializer::SchemaVersion, SceneSerializer::SchemaVersion);
    REQUIRE(sortNoop.IsOk());
    REQUIRE(sortNoop.value.anyStateChange);
    REQUIRE(f.manager.CommitPrefabMarkerPlan(std::move(sortNoop.value)).anyStateChange);
    auto sortedRaw = reg.try_get<PrefabMemberComponent>(rootHandle);
    REQUIRE(sortedRaw);
    REQUIRE(sortedRaw->overrides.size() == 2);
    CHECK(sortedRaw->overrides[0] == motion);
    CHECK(sortedRaw->overrides[1] == transform);
    CHECK(f.manager.AuthoringDoc().metadata.schemaVersion == SceneSerializer::SchemaVersion);

    // (b) duplicated raw vector collapses to a unique set.
    reg.try_get<PrefabMemberComponent>(rootHandle)->overrides = { motion, transform, motion };
    auto dupNoop = f.manager.PreparePrefabMarkerEdits({ { rootUuid, motion, true, true } },
        PrefabMarkerDirection::After, SceneSerializer::SchemaVersion, SceneSerializer::SchemaVersion);
    REQUIRE(dupNoop.IsOk());
    REQUIRE(dupNoop.value.anyStateChange);
    REQUIRE(f.manager.CommitPrefabMarkerPlan(std::move(dupNoop.value)).anyStateChange);
    auto dedupRaw = reg.try_get<PrefabMemberComponent>(rootHandle);
    REQUIRE(dedupRaw);
    REQUIRE(dedupRaw->overrides.size() == 2);
    CHECK(dedupRaw->overrides[0] == motion);
    CHECK(dedupRaw->overrides[1] == transform);

    // (c) forged classification bit re-derives the frozen-table entry.
    reg.try_get<PrefabMemberComponent>(rootHandle)->overrides =
        { PrefabComponentKey(std::string_view("transform"), false), motion };
    auto forgedNoop = f.manager.PreparePrefabMarkerEdits({ { rootUuid, motion, true, true } },
        PrefabMarkerDirection::After, SceneSerializer::SchemaVersion, SceneSerializer::SchemaVersion);
    REQUIRE(forgedNoop.IsOk());
    REQUIRE(forgedNoop.value.anyStateChange);
    REQUIRE(f.manager.CommitPrefabMarkerPlan(std::move(forgedNoop.value)).anyStateChange);
    auto canonicalRaw = reg.try_get<PrefabMemberComponent>(rootHandle);
    REQUIRE(canonicalRaw);
    REQUIRE(canonicalRaw->overrides.size() == 2);
    CHECK(canonicalRaw->overrides[0] == motion);
    CHECK(canonicalRaw->overrides[1] == transform); // table-derived bit (true)

    // Every normalization above is one real change with one revision bump.
    CHECK(f.manager.AuthoringRevision() == revisionAfter + 3);
    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Test C1-1 - Commit treats the public plan member list as a set of durable
// identities. Duplicate transitions are rejected before the downgrade scan or
// any write, in either order. This includes a forged 6 -> 5 plan whose first
// transition would otherwise make the document appear marker-free.
//
// Discrimination fault (RED observed, then restored GREEN): remove the
// Commit `seenMembers` duplicate guard (SceneManager.cpp:5793-5799). The first
// order downgrades with the marker left by the second write; the zero-change
// schema/marker assertions below go RED.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: marker helper rejects duplicate public member transitions")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_c1_duplicates");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;
    const UUID rootUuid = reg.get<EntityIdComponent>(rootHandle).id;
    const auto transform = S2Key("transform");

    auto add = f.manager.PreparePrefabMarkerEdits({ { rootUuid, transform, false, true } },
        PrefabMarkerDirection::After, f.manager.AuthoringDoc().metadata.schemaVersion,
        SceneSerializer::SchemaVersion);
    REQUIRE(add.IsOk());
    REQUIRE(f.manager.CommitPrefabMarkerPlan(std::move(add.value)).anyStateChange);
    REQUIRE(f.manager.AuthoringDoc().metadata.schemaVersion == SceneSerializer::SchemaVersion);

    const auto revision0 = f.manager.AuthoringRevision();
    const bool dirty0 = f.manager.IsDirty();
    const auto generation = f.manager.DocumentGeneration();
    const auto checkZero = [&] {
        CHECK(f.manager.AuthoringDoc().metadata.schemaVersion == SceneSerializer::SchemaVersion);
        CHECK(f.manager.AuthoringRevision() == revision0);
        CHECK(f.manager.IsDirty() == dirty0);
        auto current = f.manager.GetOverrides(rootUuid);
        REQUIRE(current.IsOk());
        REQUIRE(current.value.size() == 1);
        CHECK(current.value[0] == transform);
    };
    const auto attempt = [&](bool emptyFirst) {
        PrefabMarkerPlan plan;
        plan.direction = PrefabMarkerDirection::Before;
        plan.sourceSchemaVersion = SceneSerializer::SchemaVersion;
        plan.targetSchemaVersion = 5;
        plan.documentGeneration = generation;
        plan.anyStateChange = true;
        PrefabMarkerPlan::MemberTransition empty;
        empty.member = rootUuid;
        empty.source = { transform };
        empty.target = {};
        PrefabMarkerPlan::MemberTransition keep;
        keep.member = rootUuid;
        keep.source = { transform };
        keep.target = { transform };
        if (emptyFirst)
            plan.members = { empty, keep };
        else
            plan.members = { keep, empty };
        const auto result = f.manager.CommitPrefabMarkerPlan(std::move(plan));
        REQUIRE_FALSE(result.error.IsOk());
        REQUIRE(result.error.code == rt2::core::Error::InvalidArgument);
        CHECK(result.error.detail.find("duplicate member") != std::string::npos);
        checkZero();
    };

    attempt(true);
    attempt(false);
    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Test C1-2 - a downgrade cannot prove that no marker remains when a prefab
// member is not durably addressable. Both Prepare's shared scan and Commit's
// revalidation fail loudly with zero marker/schema/dirty/revision change.
//
// Discrimination fault (RED observed, then restored GREEN): restore the old
// `if (!id) continue` in S5RemainingOverrides (SceneManager.cpp:5392-5397).
// The downgrade succeeds despite the malformed member and the schema check
// below goes RED.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: marker helper downgrade rejects an unaddressable prefab member")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_c1_unaddressable");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;
    const UUID rootUuid = reg.get<EntityIdComponent>(rootHandle).id;
    const auto transform = S2Key("transform");
    const auto schema0 = f.manager.AuthoringDoc().metadata.schemaVersion;

    auto add = f.manager.PreparePrefabMarkerEdits({ { rootUuid, transform, false, true } },
        PrefabMarkerDirection::After, schema0, SceneSerializer::SchemaVersion);
    REQUIRE(add.IsOk());
    REQUIRE(f.manager.CommitPrefabMarkerPlan(std::move(add.value)).anyStateChange);

    // This prefab member has a non-empty marker set but no durable entity UUID.
    const auto malformed = reg.create();
    auto& malformedMember = reg.emplace<PrefabMemberComponent>(malformed);
    malformedMember.overrides = { transform };

    const auto revision0 = f.manager.AuthoringRevision();
    const bool dirty0 = f.manager.IsDirty();
    const auto checkZero = [&] {
        CHECK(f.manager.AuthoringDoc().metadata.schemaVersion == SceneSerializer::SchemaVersion);
        CHECK(f.manager.AuthoringRevision() == revision0);
        CHECK(f.manager.IsDirty() == dirty0);
        auto current = f.manager.GetOverrides(rootUuid);
        REQUIRE(current.IsOk());
        REQUIRE(current.value.size() == 1);
        CHECK(current.value[0] == transform);
    };

    auto prepare = f.manager.PreparePrefabMarkerEdits(
        { { rootUuid, transform, false, true } }, PrefabMarkerDirection::Before,
        5, SceneSerializer::SchemaVersion);
    REQUIRE_FALSE(prepare.IsOk());
    REQUIRE(prepare.error.code == rt2::core::Error::InvalidEntity);
    CHECK(prepare.error.detail.find("uniquely resolvable") != std::string::npos);
    checkZero();

    PrefabMarkerPlan forged;
    forged.direction = PrefabMarkerDirection::Before;
    forged.sourceSchemaVersion = SceneSerializer::SchemaVersion;
    forged.targetSchemaVersion = 5;
    forged.documentGeneration = f.manager.DocumentGeneration();
    forged.anyStateChange = true;
    PrefabMarkerPlan::MemberTransition transition;
    transition.member = rootUuid;
    transition.source = { transform };
    transition.target = {};
    forged.members.push_back(std::move(transition));
    const auto committed = f.manager.CommitPrefabMarkerPlan(std::move(forged));
    REQUIRE_FALSE(committed.error.IsOk());
    REQUIRE(committed.error.code == rt2::core::Error::InvalidEntity);
    CHECK(committed.error.detail.find("uniquely resolvable") != std::string::npos);
    checkZero();
    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Test C1-3 - a plan prepared against document A cannot be applied to a
// same-looking replacement document B. UUIDs, schema, and markers match, but
// ReplaceAuthoringDocument advances the generation token.
//
// Discrimination fault (RED observed, then restored GREEN): remove the Commit
// document-generation equality check (SceneManager.cpp:5789-5791). The plan
// removes the marker from replacement B and the zero-change checks go RED.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: Commit rejects a plan from a replaced document")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_c1_generation");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;
    const UUID rootUuid = reg.get<EntityIdComponent>(rootHandle).id;
    const auto transform = S2Key("transform");
    const auto schema0 = f.manager.AuthoringDoc().metadata.schemaVersion;

    auto add = f.manager.PreparePrefabMarkerEdits({ { rootUuid, transform, false, true } },
        PrefabMarkerDirection::After, schema0, SceneSerializer::SchemaVersion);
    REQUIRE(add.IsOk());
    REQUIRE(f.manager.CommitPrefabMarkerPlan(std::move(add.value)).anyStateChange);

    auto removal = f.manager.PreparePrefabMarkerEdits({ { rootUuid, transform, true, false } },
        PrefabMarkerDirection::After, SceneSerializer::SchemaVersion,
        SceneSerializer::SchemaVersion);
    REQUIRE(removal.IsOk());

    SceneDocument replacement;
    DeterministicUuidProvider replacementIds;
    replacement.SetUuidProvider(&replacementIds);
    Error cloneError;
    REQUIRE(SceneSerializer::CloneInMemory(f.manager.AuthoringDoc(), replacement, cloneError));
    f.manager.ReplaceAuthoringDocument(std::move(replacement), f.manager.AuthoringRevision());
    const auto generationB = f.manager.DocumentGeneration();
    const auto revisionB = f.manager.AuthoringRevision();
    const bool dirtyB = f.manager.IsDirty();

    const auto committed = f.manager.CommitPrefabMarkerPlan(std::move(removal.value));
    REQUIRE_FALSE(committed.error.IsOk());
    REQUIRE(committed.error.code == rt2::core::Error::InvalidArgument);
    CHECK(committed.error.detail.find("generation changed") != std::string::npos);
    CHECK(f.manager.DocumentGeneration() == generationB);
    CHECK(f.manager.AuthoringRevision() == revisionB);
    CHECK(f.manager.IsDirty() == dirtyB);
    CHECK(f.manager.AuthoringDoc().metadata.schemaVersion == SceneSerializer::SchemaVersion);
    auto current = f.manager.GetOverrides(rootUuid);
    REQUIRE(current.IsOk());
    REQUIRE(current.value.size() == 1);
    CHECK(current.value[0] == transform);
    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Test C1-4 - both directional schema fields are bounded at the public seam.
// Prepare rejects ordinary unsupported command-captured versions; Commit
// rejects forged source/target values before any member, schema, dirty, or
// revision write.
//
// Discrimination fault (RED observed, then restored GREEN): remove the
// inSupportedRange guard in S5ValidateSchemaTransition (SceneManager.cpp:
// 5438-5452). The above-current and below-readable cases stage or commit and
// the SchemaVersion assertions below go RED.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: Prepare and Commit reject schema bounds tampering")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_c1_schema_bounds");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;
    const UUID rootUuid = reg.get<EntityIdComponent>(rootHandle).id;
    const auto transform = S2Key("transform");
    const auto liveSchema = f.manager.AuthoringDoc().metadata.schemaVersion;
    const auto above = SceneSerializer::SchemaVersion + 1;
    const auto below = SceneSerializer::MinReadVersion - 1;
    const auto revision0 = f.manager.AuthoringRevision();
    const bool dirty0 = f.manager.IsDirty();
    const auto generation0 = f.manager.DocumentGeneration();
    const auto checkZero = [&] {
        CHECK(f.manager.AuthoringDoc().metadata.schemaVersion == liveSchema);
        CHECK(f.manager.AuthoringRevision() == revision0);
        CHECK(f.manager.IsDirty() == dirty0);
        CHECK(f.manager.DocumentGeneration() == generation0);
        auto current = f.manager.GetOverrides(rootUuid);
        REQUIRE(current.IsOk());
        CHECK(current.value.empty());
    };

    auto prepareAbove = f.manager.PreparePrefabMarkerEdits(
        { { rootUuid, transform, false, true } }, PrefabMarkerDirection::After,
        liveSchema, above);
    REQUIRE_FALSE(prepareAbove.IsOk());
    REQUIRE(prepareAbove.error.code == rt2::core::Error::SchemaVersion);
    checkZero();

    auto prepareBelow = f.manager.PreparePrefabMarkerEdits(
        { { rootUuid, transform, false, true } }, PrefabMarkerDirection::After,
        liveSchema, below);
    REQUIRE_FALSE(prepareBelow.IsOk());
    REQUIRE(prepareBelow.error.code == rt2::core::Error::SchemaVersion);
    checkZero();

    auto emptyAbove = f.manager.PreparePrefabMarkerEdits(
        {}, PrefabMarkerDirection::After, liveSchema, above);
    REQUIRE_FALSE(emptyAbove.IsOk());
    REQUIRE(emptyAbove.error.code == rt2::core::Error::SchemaVersion);
    auto emptyBelow = f.manager.PreparePrefabMarkerEdits(
        {}, PrefabMarkerDirection::After, liveSchema, below);
    REQUIRE_FALSE(emptyBelow.IsOk());
    REQUIRE(emptyBelow.error.code == rt2::core::Error::SchemaVersion);
    checkZero();

    const auto forge = [&](std::uint32_t source, std::uint32_t target) {
        PrefabMarkerPlan plan;
        plan.direction = PrefabMarkerDirection::After;
        plan.sourceSchemaVersion = source;
        plan.targetSchemaVersion = target;
        plan.documentGeneration = generation0;
        plan.anyStateChange = true;
        PrefabMarkerPlan::MemberTransition t;
        t.member = rootUuid;
        t.source = {};
        t.target = { transform };
        plan.members.push_back(std::move(t));
        return f.manager.CommitPrefabMarkerPlan(std::move(plan));
    };
    for (const auto [source, target] : {
             std::pair<std::uint32_t, std::uint32_t>{ liveSchema, above },
             { liveSchema, below },
             { above, liveSchema },
             { below, liveSchema },
         })
    {
        const auto committed = forge(source, target);
        REQUIRE_FALSE(committed.error.IsOk());
        REQUIRE(committed.error.code == rt2::core::Error::SchemaVersion);
        checkZero();
    }
    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// C2 typed composite transaction. The value and marker halves validate before
// either writes, share one generation, and notify once for a real change.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3: typed composite value-marker transaction is atomic")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_c2_composite");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;
    const UUID rootUuid = reg.get<EntityIdComponent>(rootHandle).id;
    const auto transform = S2Key("transform");
    const auto schema0 = f.manager.AuthoringDoc().metadata.schemaVersion;
    const auto name0 = f.manager.GetEntityName({ rootHandle });
    const auto revision0 = f.manager.AuthoringRevision();
    const bool dirty0 = f.manager.IsDirty();

    const auto checkInitial = [&] {
        CHECK(f.manager.AuthoringRevision() == revision0);
        CHECK(f.manager.IsDirty() == dirty0);
        CHECK(f.manager.AuthoringDoc().metadata.schemaVersion == schema0);
        auto marker = f.manager.GetOverrides(rootUuid);
        REQUIRE(marker.IsOk());
        CHECK(marker.value.empty());
        CHECK(f.manager.GetEntityName({ rootHandle }) == name0);
    };

    // Value validation failure: marker and value remain untouched.
    PrefabValueEdit badValue{ PrefabValueKind::EntityName, rootUuid,
        PrefabMarkerDirection::After, std::string("not-live"), std::string("new") };
    auto markerAdd = PrefabMarkerEdit{ rootUuid, transform, false, true };
    auto bad = f.manager.PreparePrefabCompositeEdits({ badValue }, { markerAdd },
        PrefabMarkerDirection::After, schema0, SceneSerializer::SchemaVersion);
    REQUIRE_FALSE(bad.IsOk());
    REQUIRE(bad.error.code == rt2::core::Error::InvalidArgument);
    checkInitial();

    // Marker validation failure: the valid value is not staged into live state.
    PrefabValueEdit goodValue{ PrefabValueKind::EntityName, rootUuid,
        PrefabMarkerDirection::After, name0, std::string("Composite") };
    auto badMarker = PrefabMarkerEdit{ rootUuid, S2Key("meshRef"), false, true };
    auto badMarkerPlan = f.manager.PreparePrefabCompositeEdits({ goodValue }, { badMarker },
        PrefabMarkerDirection::After, schema0, SceneSerializer::SchemaVersion);
    REQUIRE_FALSE(badMarkerPlan.IsOk());
    REQUIRE(badMarkerPlan.error.code == rt2::core::Error::InvalidArgument);
    checkInitial();

    // Successful value+marker: one revision and one authoritative result.
    auto goodPlan = f.manager.PreparePrefabCompositeEdits({ goodValue }, { markerAdd },
        PrefabMarkerDirection::After, schema0, SceneSerializer::SchemaVersion);
    REQUIRE(goodPlan.IsOk());
    const auto committed = f.manager.CommitPrefabCompositePlan(std::move(goodPlan.value));
    REQUIRE(committed.error.IsOk());
    REQUIRE(committed.anyStateChange);
    REQUIRE(committed.appliedValueOperations == 1);
    REQUIRE(committed.appliedMarkerMembers == 1);
    REQUIRE(committed.syncImpact == rt2::core::SyncImpact::None);
    REQUIRE(f.manager.AuthoringRevision() == revision0 + 1);
    REQUIRE(f.manager.GetEntityName({ rootHandle }) == "Composite");
    auto marked = f.manager.GetOverrides(rootUuid);
    REQUIRE(marked.IsOk());
    REQUIRE(marked.value.size() == 1);
    CHECK(marked.value[0] == transform);
    CHECK(std::find(committed.affectedEntities.begin(), committed.affectedEntities.end(), rootUuid)
        != committed.affectedEntities.end());

    // Genuine combined no-op: zero revision/dirty churn.
    PrefabValueEdit noopValue{ PrefabValueKind::EntityName, rootUuid,
        PrefabMarkerDirection::After, std::string("Composite"), std::string("Composite") };
    auto noopMarker = PrefabMarkerEdit{ rootUuid, transform, true, true };
    auto noopPlan = f.manager.PreparePrefabCompositeEdits({ noopValue }, { noopMarker },
        PrefabMarkerDirection::After, SceneSerializer::SchemaVersion,
        SceneSerializer::SchemaVersion);
    REQUIRE(noopPlan.IsOk());
    const auto revisionAfter = f.manager.AuthoringRevision();
    const auto noopResult = f.manager.CommitPrefabCompositePlan(std::move(noopPlan.value));
    REQUIRE(noopResult.error.IsOk());
    REQUIRE_FALSE(noopResult.anyStateChange);
    CHECK(noopResult.appliedValueOperations == 0);
    CHECK(noopResult.appliedMarkerMembers == 0);
    CHECK(f.manager.AuthoringRevision() == revisionAfter);

    // Stale value source after preparation: no marker or value write.
    auto stalePlan = f.manager.PreparePrefabCompositeEdits({ noopValue }, {},
        PrefabMarkerDirection::After, SceneSerializer::SchemaVersion,
        SceneSerializer::SchemaVersion);
    REQUIRE(stalePlan.IsOk());
    reg.get<NameComponent>(rootHandle).name = "changed underneath";
    const auto staleRevision = f.manager.AuthoringRevision();
    const auto staleResult = f.manager.CommitPrefabCompositePlan(std::move(stalePlan.value));
    REQUIRE_FALSE(staleResult.error.IsOk());
    CHECK(f.manager.AuthoringRevision() == staleRevision);
    CHECK(f.manager.GetEntityName({ rootHandle }) == "changed underneath");

    // A same-looking replacement document is still a different transaction
    // source. The generation check must reject it before any write.
    auto generationPlan = f.manager.PreparePrefabCompositeEdits(
        { PrefabValueEdit{ PrefabValueKind::EntityName, rootUuid,
            PrefabMarkerDirection::After, std::string("changed underneath"),
            std::string("replacement must not apply") } }, {},
        PrefabMarkerDirection::After, schema0, schema0);
    REQUIRE(generationPlan.IsOk());
    SceneDocument replacement;
    DeterministicUuidProvider replacementIds;
    replacement.SetUuidProvider(&replacementIds);
    Error cloneError;
    REQUIRE(SceneSerializer::CloneInMemory(f.manager.AuthoringDoc(), replacement, cloneError));
    f.manager.ReplaceAuthoringDocument(std::move(replacement), f.manager.AuthoringRevision());
    const auto replacementRevision = f.manager.AuthoringRevision();
    const bool replacementDirty = f.manager.IsDirty();
    const auto generationResult = f.manager.CommitPrefabCompositePlan(std::move(generationPlan.value));
    REQUIRE_FALSE(generationResult.error.IsOk());
    CHECK(generationResult.error.detail.find("generation changed") != std::string::npos);
    CHECK(f.manager.AuthoringRevision() == replacementRevision);
    CHECK(f.manager.IsDirty() == replacementDirty);
    CHECK(f.manager.GetEntityName({ rootHandle }) == "changed underneath");

    std::filesystem::remove_all(dir);
}

TEST_CASE("Phase 8 W3: typed composite material-slot fan-out is atomic")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_c2_material");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;
    const UUID rootUuid = reg.get<EntityIdComponent>(rootHandle).id;
    const UUID childUuid = reg.get<EntityIdComponent>(childHandle).id;
    reg.emplace_or_replace<MeshRef>(rootHandle, MeshRef{ 0, 0 });
    reg.emplace_or_replace<MeshRef>(childHandle, MeshRef{ 0, 0 });
    reg.emplace_or_replace<ImportedMeshSourceComponent>(rootHandle);
    reg.emplace_or_replace<ImportedMeshSourceComponent>(childHandle);
    const auto before = f.manager.GetMaterial(0);
    auto after = before;
    after.baseColor = { 0.2f, 0.4f, 0.6f };
    const auto schema0 = f.manager.AuthoringDoc().metadata.schemaVersion;
    const auto revision0 = f.manager.AuthoringRevision();
    PrefabValueEdit slot;
    slot.kind = PrefabValueKind::MaterialSlotProperties;
    slot.direction = PrefabMarkerDirection::After;
    slot.before = PrefabMaterialSlotValue{ 0, before, {} };
    slot.after = PrefabMaterialSlotValue{ 0, after, {} };
    const auto marker = PrefabMarkerEdit{ rootUuid, S2Key("transform"), false, true };
    auto plan = f.manager.PreparePrefabCompositeEdits({ slot }, { marker },
        PrefabMarkerDirection::After, schema0, SceneSerializer::SchemaVersion);
    REQUIRE(plan.IsOk());
    const auto result = f.manager.CommitPrefabCompositePlan(std::move(plan.value));
    REQUIRE(result.error.IsOk());
    REQUIRE(result.anyStateChange);
    REQUIRE(result.syncImpact == rt2::core::SyncImpact::Material);
    REQUIRE(f.manager.AuthoringRevision() == revision0 + 1);
    CHECK(f.manager.GetMaterial(0).baseColor == after.baseColor);
    auto* rootOverride = reg.try_get<MaterialOverrideComponent>(rootHandle);
    auto* childOverride = reg.try_get<MaterialOverrideComponent>(childHandle);
    REQUIRE(rootOverride);
    REQUIRE(childOverride);
    CHECK(rootOverride->material.baseColor == after.baseColor);
    CHECK(childOverride->material.baseColor == after.baseColor);
    CHECK(std::find(result.affectedEntities.begin(), result.affectedEntities.end(), childUuid)
        != result.affectedEntities.end());

    // Missing fan-out member after preparation: all raw state remains intact.
    auto after2 = after;
    after2.baseColor = { 0.7f, 0.3f, 0.1f };
    PrefabValueEdit slot2;
    slot2.kind = PrefabValueKind::MaterialSlotProperties;
    slot2.direction = PrefabMarkerDirection::After;
    slot2.before = PrefabMaterialSlotValue{ 0, after, {} };
    slot2.after = PrefabMaterialSlotValue{ 0, after2, {} };
    auto stale = f.manager.PreparePrefabCompositeEdits({ slot2 }, {},
        PrefabMarkerDirection::After, SceneSerializer::SchemaVersion,
        SceneSerializer::SchemaVersion);
    REQUIRE(stale.IsOk());
    const auto materialBeforeFailure = f.manager.GetMaterial(0);
    const auto revisionBeforeFailure = f.manager.AuthoringRevision();

    // A fan-out member without a durable ID cannot be represented in a
    // public plan and must fail at Prepare, without touching any state.
    reg.remove<EntityIdComponent>(childHandle);
    const auto missingIdRevision = f.manager.AuthoringRevision();
    const auto missingIdMaterial = f.manager.GetMaterial(0);
    auto missingId = f.manager.PreparePrefabCompositeEdits({ slot2 }, {},
        PrefabMarkerDirection::After, SceneSerializer::SchemaVersion,
        SceneSerializer::SchemaVersion);
    REQUIRE_FALSE(missingId.IsOk());
    REQUIRE(missingId.error.code == rt2::core::Error::InvalidEntity);
    CHECK(missingId.error.detail.find("durable") != std::string::npos);
    CHECK(f.manager.AuthoringRevision() == missingIdRevision);
    CHECK(f.manager.GetMaterial(0).baseColor == missingIdMaterial.baseColor);

    reg.destroy(childHandle);
    const auto failed = f.manager.CommitPrefabCompositePlan(std::move(stale.value));
    REQUIRE_FALSE(failed.error.IsOk());
    CHECK(f.manager.AuthoringRevision() == revisionBeforeFailure);
    CHECK(f.manager.GetMaterial(0).baseColor == materialBeforeFailure.baseColor);
    CHECK(reg.try_get<MaterialOverrideComponent>(rootHandle) != nullptr);
    std::filesystem::remove_all(dir);
}

// RED proof: bypassing the duplicate-UUID insertion guard makes the first
// duplicate-order REQUIRE_FALSE below fail; restore the guard before GREEN.
TEST_CASE("Phase 8 W3: material fan-out plans require an exact live UUID set and bytes")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_c3b_material_exact");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;
    const UUID root = reg.get<EntityIdComponent>(rootHandle).id;
    const UUID child = reg.get<EntityIdComponent>(childHandle).id;
    reg.emplace_or_replace<MeshRef>(rootHandle, MeshRef{ 0, 0 });
    reg.emplace_or_replace<MeshRef>(childHandle, MeshRef{ 0, 0 });
    reg.emplace_or_replace<ImportedMeshSourceComponent>(rootHandle);
    reg.emplace_or_replace<ImportedMeshSourceComponent>(childHandle);

    const auto before = f.manager.GetMaterial(0);
    auto after = before;
    after.baseColor = { 0.15f, 0.35f, 0.85f };
    auto makeOverride = [&](const SceneMaterial& material) {
        MaterialOverrideComponent value;
        value.material = material;
        value.authored = true;
        value.materialIndex = 0;
        return std::optional<MaterialOverrideComponent>{ value };
    };
    const auto sourceMembers = std::vector<std::pair<UUID,
        std::optional<MaterialOverrideComponent>>>{ { root, std::nullopt },
        { child, std::nullopt } };
    const auto targetMembers = std::vector<std::pair<UUID,
        std::optional<MaterialOverrideComponent>>>{ { root, makeOverride(after) },
        { child, makeOverride(after) } };
    const auto makeEdit = [&](const auto& source, const auto& target) {
        return PrefabValueEdit{ PrefabValueKind::MaterialSlotProperties, {},
            PrefabMarkerDirection::After,
            PrefabMaterialSlotValue{ 0, before, source },
            PrefabMaterialSlotValue{ 0, after, target } };
    };
    const auto schema = f.manager.AuthoringDoc().metadata.schemaVersion;

    // RED proof: bypassing canonical authored/source-key target validation
    // makes this forged-target REQUIRE_FALSE fail; restore it before GREEN.
    auto forgedTarget = targetMembers;
    for (auto& member : forgedTarget)
    {
        member.second->authored = false;
        member.second->sourceMaterialKey = "forged-source";
    }
    auto forgedTargetPlan = f.manager.PreparePrefabCompositeEdits(
        { makeEdit(sourceMembers, forgedTarget) }, {},
        PrefabMarkerDirection::After, schema, schema);
    REQUIRE_FALSE(forgedTargetPlan.IsOk());

    auto duplicate = sourceMembers;
    duplicate.push_back({ root, std::nullopt });
    auto duplicateFirst = f.manager.PreparePrefabCompositeEdits(
        { makeEdit(duplicate, targetMembers) }, {},
        PrefabMarkerDirection::After, schema, schema);
    REQUIRE_FALSE(duplicateFirst.IsOk());
    auto duplicateLast = sourceMembers;
    duplicateLast.insert(duplicateLast.begin(), { root, std::nullopt });
    auto duplicateSecond = f.manager.PreparePrefabCompositeEdits(
        { makeEdit(duplicateLast, targetMembers) }, {},
        PrefabMarkerDirection::After, schema, schema);
    REQUIRE_FALSE(duplicateSecond.IsOk());

    auto missing = std::vector<std::pair<UUID,
        std::optional<MaterialOverrideComponent>>>{ { root, std::nullopt } };
    auto missingResult = f.manager.PreparePrefabCompositeEdits(
        { makeEdit(missing, targetMembers) }, {},
        PrefabMarkerDirection::After, schema, schema);
    REQUIRE_FALSE(missingResult.IsOk());
    auto extra = sourceMembers;
    extra.push_back({ UUID::Parse("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"), std::nullopt });
    auto extraResult = f.manager.PreparePrefabCompositeEdits(
        { makeEdit(extra, targetMembers) }, {},
        PrefabMarkerDirection::After, schema, schema);
    REQUIRE_FALSE(extraResult.IsOk());

    // RED proof: bypassing source-byte comparison makes the stale commit
    // REQUIRE_FALSE below fail; restore the comparison before GREEN.
    auto valid = f.manager.PreparePrefabCompositeEdits(
        { makeEdit(sourceMembers, targetMembers) }, {},
        PrefabMarkerDirection::After, schema, schema);
    REQUIRE(valid.IsOk());
    auto* childOverride = reg.try_get<MaterialOverrideComponent>(childHandle);
    REQUIRE(childOverride == nullptr);
    auto staleOverride = makeOverride(before);
    reg.emplace_or_replace<MaterialOverrideComponent>(childHandle, *staleOverride);
    const auto materialBefore = f.manager.GetMaterial(0);
    const auto revisionBefore = f.manager.AuthoringRevision();
    const auto stale = f.manager.CommitPrefabCompositePlan(std::move(valid.value));
    REQUIRE_FALSE(stale.error.IsOk());
    CHECK(f.manager.GetMaterial(0).baseColor == materialBefore.baseColor);
    CHECK(f.manager.AuthoringRevision() == revisionBefore);
    reg.remove<MaterialOverrideComponent>(childHandle);

    auto changed = f.manager.PreparePrefabCompositeEdits(
        { makeEdit(sourceMembers, targetMembers) }, {},
        PrefabMarkerDirection::After, schema, schema);
    REQUIRE(changed.IsOk());
    reg.get<MeshRef>(childHandle).materialIndex = 1;
    const auto revisionBeforeMeshChange = f.manager.AuthoringRevision();
    const auto changedResult = f.manager.CommitPrefabCompositePlan(std::move(changed.value));
    REQUIRE_FALSE(changedResult.error.IsOk());
    CHECK(f.manager.AuthoringRevision() == revisionBeforeMeshChange);
    CHECK(f.manager.GetMaterial(0).baseColor == materialBefore.baseColor);
    std::filesystem::remove_all(dir);
}

// RED proof: bypassing the resolver's sidecar-conflict diagnostic makes the
// conflict REQUIRE_FALSE below fail; restore it before the focused GREEN run.
TEST_CASE("Phase 8 W3: composite script binding stages durable identity before commit")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_c3b_script_binding");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    const UUID root = f.manager.GetECS().registry.get<EntityIdComponent>(rootHandle).id;
    rt2::core::AssetResolutionContext context;
    context.assetRoot = dir;
    f.manager.SetAssetResolutionContext(context);
    const auto scriptAPath = dir / "first.lua";
    const auto scriptBPath = dir / "second.lua";
    const auto scriptCPath = dir / "conflict.lua";
    S2WriteRaw(scriptAPath, "return 1\n");
    S2WriteRaw(scriptBPath, "return 2\n");
    S2WriteRaw(scriptCPath, "return 3\n");
    ScriptComponent scriptA;
    scriptA.asset.kind = AssetKind::Script;
    scriptA.asset.path = "first.lua";
    scriptA.asset.sourceKey = "lua:asset=first.lua";
    ScriptComponent scriptB = scriptA;
    scriptB.asset.path = "second.lua";
    scriptB.asset.sourceKey = "lua:asset=second.lua";
    ScriptComponent scriptC = scriptA;
    scriptC.asset.path = "conflict.lua";
    scriptC.asset.sourceKey = "lua:asset=conflict.lua";
    const auto schema = f.manager.AuthoringDoc().metadata.schemaVersion;
    const auto absent = std::optional<ScriptComponent>{};
    const auto revisionBeforeAdd = f.manager.AuthoringRevision();
    const auto sidecarA = rt2::core::AssetSidecarPath(scriptAPath);
    auto invalidMarker = f.manager.PreparePrefabCompositeEdits({
        { PrefabValueKind::ScriptState, root, PrefabMarkerDirection::After,
            absent, std::optional<ScriptComponent>{ scriptA } } },
        { PrefabMarkerEdit{ root, S2Key("meshRef"), false, true } },
        PrefabMarkerDirection::After, schema, SceneSerializer::SchemaVersion);
    REQUIRE_FALSE(invalidMarker.IsOk());
    CHECK_FALSE(std::filesystem::exists(sidecarA));
    auto add = f.manager.PreparePrefabCompositeEdits({
        { PrefabValueKind::ScriptState, root, PrefabMarkerDirection::After,
            absent, std::optional<ScriptComponent>{ scriptA } } }, {},
        PrefabMarkerDirection::After, schema, schema);
    REQUIRE(add.IsOk());
    REQUIRE(std::filesystem::exists(sidecarA));
    Error sidecarError;
    const auto assignedA = rt2::core::ReadSidecarId(sidecarA, sidecarError);
    REQUIRE(sidecarError.IsOk());
    REQUIRE_FALSE(assignedA.IsNull());
    const auto sidecarBytesBeforeCommit = S2ReadFile(sidecarA);
    const auto addResult = f.manager.CommitPrefabCompositePlan(std::move(add.value));
    REQUIRE(addResult.error.IsOk());
    REQUIRE(f.manager.AuthoringRevision() == revisionBeforeAdd + 1);
    REQUIRE(f.manager.GetECS().registry.get<ScriptComponent>(rootHandle).asset.assetId == assignedA);
    CHECK(S2ReadFile(sidecarA) == sidecarBytesBeforeCommit);
    scriptA.asset.assetId = assignedA;

    const auto revisionAfterAdd = f.manager.AuthoringRevision();
    auto noop = f.manager.PreparePrefabCompositeEdits({
        { PrefabValueKind::ScriptState, root, PrefabMarkerDirection::After,
            std::optional<ScriptComponent>{ f.manager.GetECS().registry.get<ScriptComponent>(rootHandle) },
            std::optional<ScriptComponent>{ scriptA } } }, {},
        PrefabMarkerDirection::After, schema, schema);
    REQUIRE(noop.IsOk());
    const auto noopResult = f.manager.CommitPrefabCompositePlan(std::move(noop.value));
    REQUIRE(noopResult.error.IsOk());
    CHECK_FALSE(noopResult.anyStateChange);
    CHECK(f.manager.AuthoringRevision() == revisionAfterAdd);

    auto remove = f.manager.PreparePrefabCompositeEdits({
        { PrefabValueKind::ScriptState, root, PrefabMarkerDirection::After,
            std::optional<ScriptComponent>{ f.manager.GetECS().registry.get<ScriptComponent>(rootHandle) },
            absent } }, {}, PrefabMarkerDirection::After, schema, schema);
    REQUIRE(remove.IsOk());
    REQUIRE(f.manager.CommitPrefabCompositePlan(std::move(remove.value)).error.IsOk());

    auto rebind = f.manager.PreparePrefabCompositeEdits({
        { PrefabValueKind::ScriptState, root, PrefabMarkerDirection::After,
            absent, std::optional<ScriptComponent>{ scriptB } } }, {},
        PrefabMarkerDirection::After, schema, schema);
    REQUIRE(rebind.IsOk());
    const auto sidecarB = rt2::core::AssetSidecarPath(scriptBPath);
    REQUIRE(std::filesystem::exists(sidecarB));
    REQUIRE(f.manager.CommitPrefabCompositePlan(std::move(rebind.value)).error.IsOk());
    const auto assignedB = f.manager.GetECS().registry.get<ScriptComponent>(rootHandle).asset.assetId;
    REQUIRE_FALSE(assignedB.IsNull());

    auto samePath = scriptB;
    samePath.asset.assetId = UUID::Nil();
    auto reuse = f.manager.PreparePrefabCompositeEdits({
        { PrefabValueKind::ScriptState, root, PrefabMarkerDirection::After,
            std::optional<ScriptComponent>{ f.manager.GetECS().registry.get<ScriptComponent>(rootHandle) },
            std::optional<ScriptComponent>{ samePath } } }, {},
        PrefabMarkerDirection::After, schema, schema);
    REQUIRE(reuse.IsOk());
    CHECK(std::get<std::optional<ScriptComponent>>(
        reuse.value.values.operations.front().target).value().asset.assetId == assignedB);
    REQUIRE(f.manager.CommitPrefabCompositePlan(std::move(reuse.value)).error.IsOk());
    CHECK(f.manager.GetECS().registry.get<ScriptComponent>(rootHandle).asset.assetId == assignedB);

    UUID conflictingId = f.manager.ReserveKnownUuid();
    Error writeError;
    REQUIRE(rt2::core::WriteSidecarId(rt2::core::AssetSidecarPath(scriptCPath), conflictingId, writeError));
    scriptC.asset.assetId = assignedA;
    const auto revisionBeforeConflict = f.manager.AuthoringRevision();
    auto conflict = f.manager.PreparePrefabCompositeEdits({
        { PrefabValueKind::ScriptState, root, PrefabMarkerDirection::After,
            std::optional<ScriptComponent>{ f.manager.GetECS().registry.get<ScriptComponent>(rootHandle) },
            std::optional<ScriptComponent>{ scriptC } } }, {},
        PrefabMarkerDirection::After, schema, schema);
    REQUIRE_FALSE(conflict.IsOk());
    CHECK(f.manager.AuthoringRevision() == revisionBeforeConflict);

    auto removeB = f.manager.PreparePrefabCompositeEdits({
        { PrefabValueKind::ScriptState, root, PrefabMarkerDirection::After,
            std::optional<ScriptComponent>{ f.manager.GetECS().registry.get<ScriptComponent>(rootHandle) },
            absent } }, {}, PrefabMarkerDirection::After, schema, schema);
    REQUIRE(removeB.IsOk());
    REQUIRE(f.manager.CommitPrefabCompositePlan(std::move(removeB.value)).error.IsOk());
    auto beforeUndo = f.manager.PreparePrefabCompositeEdits({
        { PrefabValueKind::ScriptState, root, PrefabMarkerDirection::After,
            absent, std::optional<ScriptComponent>{ scriptA } } }, {},
        PrefabMarkerDirection::After, schema, schema);
    REQUIRE(beforeUndo.IsOk());
    REQUIRE(f.manager.CommitPrefabCompositePlan(std::move(beforeUndo.value)).error.IsOk());
    auto before = f.manager.PreparePrefabCompositeEdits({
        { PrefabValueKind::ScriptState, root, PrefabMarkerDirection::Before,
            absent, std::optional<ScriptComponent>{ scriptA } } }, {},
        PrefabMarkerDirection::Before, schema, schema);
    REQUIRE(before.IsOk());
    REQUIRE(f.manager.CommitPrefabCompositePlan(std::move(before.value)).error.IsOk());
    CHECK_FALSE(f.manager.GetECS().registry.all_of<ScriptComponent>(rootHandle));

    ScriptComponent missingScript = scriptA;
    missingScript.asset.path = "missing.lua";
    missingScript.asset.sourceKey = "lua:asset=missing.lua";
    missingScript.asset.assetId = UUID::Nil();
    const auto missingSidecar = rt2::core::AssetSidecarPath(dir / "missing.lua");
    auto missingPlan = f.manager.PreparePrefabCompositeEdits({
        { PrefabValueKind::ScriptState, root, PrefabMarkerDirection::After,
            absent, std::optional<ScriptComponent>{ missingScript } } }, {},
        PrefabMarkerDirection::After, schema, schema);
    REQUIRE(missingPlan.IsOk());
    REQUIRE(f.manager.CommitPrefabCompositePlan(std::move(missingPlan.value)).error.IsOk());
    CHECK_FALSE(std::filesystem::exists(missingSidecar));
    CHECK(f.manager.GetECS().registry.get<ScriptComponent>(rootHandle).asset.assetId.IsNull());
    REQUIRE(f.manager.SetScriptState(root, absent).error.IsOk());

    // A malformed sidecar is a resolver failure, not a missing source file:
    // ordinary and composite paths must reject it before authoring mutation.
    const auto malformedAsset = dir / "malformed.lua";
    S2WriteRaw(malformedAsset, "return 6\n");
    const auto malformedSidecar = rt2::core::AssetSidecarPath(malformedAsset);
    S2WriteRaw(malformedSidecar, "not-a-uuid\n");
    ScriptComponent malformedScript = scriptA;
    malformedScript.asset.path = "malformed.lua";
    malformedScript.asset.sourceKey = "lua:asset=malformed.lua";
    malformedScript.asset.assetId = UUID::Nil();
    const auto malformedRevision = f.manager.AuthoringRevision();
    const auto ordinaryMalformed = f.manager.SetScriptState(root,
        std::optional<ScriptComponent>{ malformedScript });
    REQUIRE_FALSE(ordinaryMalformed.error.IsOk());
    CHECK(ordinaryMalformed.error.detail.find("resolution failed") != std::string::npos);
    CHECK_FALSE(f.manager.GetECS().registry.all_of<ScriptComponent>(rootHandle));
    CHECK(f.manager.AuthoringRevision() == malformedRevision);
    auto compositeMalformed = f.manager.PreparePrefabCompositeEdits({
        { PrefabValueKind::ScriptState, root, PrefabMarkerDirection::After,
            absent, std::optional<ScriptComponent>{ malformedScript } } }, {},
        PrefabMarkerDirection::After, schema, schema);
    REQUIRE_FALSE(compositeMalformed.IsOk());
    CHECK(compositeMalformed.error.detail.find("resolution failed") != std::string::npos);
    CHECK_FALSE(f.manager.GetECS().registry.all_of<ScriptComponent>(rootHandle));
    CHECK(f.manager.AuthoringRevision() == malformedRevision);

    // A genuinely different explicit destination ID is durable even when the
    // destination has no sidecar; Commit must then be read-only for identity.
    const auto explicitAsset = dir / "explicit.lua";
    S2WriteRaw(explicitAsset, "return 7\n");
    ScriptComponent explicitScript = scriptA;
    explicitScript.asset.path = "explicit.lua";
    explicitScript.asset.sourceKey = "lua:asset=explicit.lua";
    const UUID explicitId = f.manager.ReserveKnownUuid();
    explicitScript.asset.assetId = explicitId;
    const auto explicitSidecar = rt2::core::AssetSidecarPath(explicitAsset);
    auto explicitPlan = f.manager.PreparePrefabCompositeEdits({
        { PrefabValueKind::ScriptState, root, PrefabMarkerDirection::After,
            absent, std::optional<ScriptComponent>{ explicitScript } } }, {},
        PrefabMarkerDirection::After, schema, schema);
    REQUIRE(explicitPlan.IsOk());
    Error explicitReadError;
    CHECK(rt2::core::ReadSidecarId(explicitSidecar, explicitReadError) == explicitId);
    REQUIRE(explicitReadError.IsOk());
    const auto explicitSidecarBytes = S2ReadFile(explicitSidecar);
    REQUIRE(f.manager.CommitPrefabCompositePlan(std::move(explicitPlan.value)).error.IsOk());
    CHECK(f.manager.GetECS().registry.get<ScriptComponent>(rootHandle).asset.assetId == explicitId);
    CHECK(S2ReadFile(explicitSidecar) == explicitSidecarBytes);

    const auto writeFailAsset = dir / "write-fail.lua";
    S2WriteRaw(writeFailAsset, "return 5\n");
    const auto writeFailSidecar = rt2::core::AssetSidecarPath(writeFailAsset);
    std::error_code sidecarDirectoryError;
    std::filesystem::create_directory(writeFailSidecar, sidecarDirectoryError);
    REQUIRE_FALSE(sidecarDirectoryError);
    bool mintedOnWriteFailure = false;
    Error writeFailure;
    const auto failureId = rt2::core::ResolveOrAssign(writeFailAsset,
        f.ids, mintedOnWriteFailure, writeFailure);
    CHECK_FALSE(writeFailure.IsOk());
    CHECK(mintedOnWriteFailure);
    CHECK_FALSE(failureId.IsNull());
    std::filesystem::remove_all(writeFailSidecar);
    std::filesystem::remove_all(dir);
}

TEST_CASE("Phase 8 W3: composite direction reverses typed value source and target")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_c3a_direction");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;
    const UUID root = reg.get<EntityIdComponent>(rootHandle).id;
    const auto schema = f.manager.AuthoringDoc().metadata.schemaVersion;

    auto cycle = [&](PrefabValueEdit edit) {
        edit.direction = PrefabMarkerDirection::After;
        auto execute = f.manager.PreparePrefabCompositeEdits({ edit }, {},
            PrefabMarkerDirection::After, schema, schema);
        REQUIRE(execute.IsOk());
        REQUIRE(f.manager.CommitPrefabCompositePlan(std::move(execute.value)).error.IsOk());
        edit.direction = PrefabMarkerDirection::Before;
        auto undo = f.manager.PreparePrefabCompositeEdits({ edit }, {},
            PrefabMarkerDirection::Before, schema, schema);
        REQUIRE(undo.IsOk());
        REQUIRE(f.manager.CommitPrefabCompositePlan(std::move(undo.value)).error.IsOk());
    };

    const auto name = f.manager.GetEntityName({ rootHandle });
    cycle({ PrefabValueKind::EntityName, root, PrefabMarkerDirection::After,
        name, std::string("direction-name") });
    CHECK(f.manager.GetEntityName({ rootHandle }) == name);

    LightComponent lightBefore;
    reg.emplace_or_replace<LightComponent>(rootHandle, lightBefore);
    auto lightAfter = lightBefore; lightAfter.intensity = 3.0f;
    cycle({ PrefabValueKind::LightProperties, root, PrefabMarkerDirection::After,
        lightBefore, lightAfter });
    CHECK(reg.get<LightComponent>(rootHandle).intensity == lightBefore.intensity);

    CameraComponent cameraBefore;
    reg.emplace_or_replace<CameraComponent>(rootHandle, cameraBefore);
    auto cameraAfter = cameraBefore; cameraAfter.verticalFOV = 63.0f;
    cycle({ PrefabValueKind::CameraProperties, root, PrefabMarkerDirection::After,
        cameraBefore, cameraAfter });
    CHECK(reg.get<CameraComponent>(rootHandle).verticalFOV == cameraBefore.verticalFOV);

    const auto& tf = reg.get<Transform>(rootHandle);
    EditableTRS localBefore{ tf.translation, tf.rotation, tf.scale };
    auto localAfter = localBefore; localAfter.translation.x = 2.0f;
    cycle({ PrefabValueKind::LocalTransform, root, PrefabMarkerDirection::After,
        localBefore, localAfter });
    CHECK(reg.get<Transform>(rootHandle).translation == localBefore.translation);

    PrefabCameraPoseValue poseBefore{ localBefore, cameraBefore };
    auto poseAfter = poseBefore; poseAfter.local.translation.y = 4.0f;
    cycle({ PrefabValueKind::CameraPose, root, PrefabMarkerDirection::After,
        poseBefore, poseAfter });
    CHECK(reg.get<Transform>(rootHandle).translation == localBefore.translation);

    std::optional<MotionComponent> motionBefore;
    auto motionAfter = MotionComponent{}; motionAfter.linearVelocity = { 1.0f, 2.0f, 3.0f };
    cycle({ PrefabValueKind::MotionState, root, PrefabMarkerDirection::After,
        motionBefore, std::optional<MotionComponent>{ motionAfter } });
    CHECK_FALSE(reg.all_of<MotionComponent>(rootHandle));

    // Script add/remove is a genuine directional cycle, including durable
    // identity revalidation on the Before half.
    S2WriteRaw(dir / "direction.lua", "return 4\n");
    rt2::core::AssetResolutionContext scriptContext;
    scriptContext.assetRoot = dir;
    f.manager.SetAssetResolutionContext(scriptContext);
    ScriptComponent scriptAfter;
    scriptAfter.asset.kind = AssetKind::Script;
    scriptAfter.asset.path = "direction.lua";
    scriptAfter.asset.sourceKey = "lua:asset=direction.lua";
    auto scriptAdd = f.manager.PreparePrefabCompositeEdits({
        { PrefabValueKind::ScriptState, root, PrefabMarkerDirection::After,
            std::optional<ScriptComponent>{}, std::optional<ScriptComponent>{ scriptAfter } } }, {},
        PrefabMarkerDirection::After, schema, schema);
    REQUIRE(scriptAdd.IsOk());
    REQUIRE(f.manager.CommitPrefabCompositePlan(std::move(scriptAdd.value)).error.IsOk());
    const auto storedScript = reg.get<ScriptComponent>(rootHandle);
    auto scriptUndo = f.manager.PreparePrefabCompositeEdits({
        { PrefabValueKind::ScriptState, root, PrefabMarkerDirection::Before,
            std::optional<ScriptComponent>{}, std::optional<ScriptComponent>{ storedScript } } }, {},
        PrefabMarkerDirection::Before, schema, schema);
    REQUIRE(scriptUndo.IsOk());
    REQUIRE(f.manager.CommitPrefabCompositePlan(std::move(scriptUndo.value)).error.IsOk());
    CHECK_FALSE(reg.all_of<ScriptComponent>(rootHandle));

    f.manager.AddMaterial(SceneMaterial{});
    reg.emplace_or_replace<MeshRef>(rootHandle, MeshRef{ 0, 0 });
    MaterialOverrideComponent indexBeforeOverride;
    indexBeforeOverride.material = f.manager.GetMaterial(0);
    indexBeforeOverride.authored = true;
    indexBeforeOverride.sourceMaterialKey = indexBeforeOverride.material.sourceKey;
    indexBeforeOverride.materialIndex = 0;
    MaterialOverrideComponent indexAfterOverride = indexBeforeOverride;
    indexAfterOverride.material = f.manager.GetMaterial(1);
    indexAfterOverride.sourceMaterialKey = indexAfterOverride.material.sourceKey;
    indexAfterOverride.materialIndex = 1;
    reg.emplace_or_replace<MaterialOverrideComponent>(rootHandle, indexBeforeOverride);
    PrefabMaterialIndexValue indexBefore{ 0, indexBeforeOverride };
    PrefabMaterialIndexValue indexAfter{ 1, indexAfterOverride };
    cycle({ PrefabValueKind::MaterialIndex, root, PrefabMarkerDirection::After,
        indexBefore, indexAfter });
    CHECK(reg.get<MeshRef>(rootHandle).materialIndex == 0);
    CHECK(reg.get<MaterialOverrideComponent>(rootHandle).materialIndex == 0);
    CHECK(reg.get<MaterialOverrideComponent>(rootHandle).material.baseAlpha == indexBeforeOverride.material.baseAlpha);

    const auto slotBefore = f.manager.GetMaterial(0);
    auto slotAfter = slotBefore; slotAfter.baseAlpha = 0.8f;
    reg.emplace_or_replace<MeshRef>(childHandle, MeshRef{ 0, 0 });
    reg.emplace_or_replace<ImportedMeshSourceComponent>(rootHandle);
    reg.emplace_or_replace<ImportedMeshSourceComponent>(childHandle);
    MaterialOverrideComponent slotBeforeOverride;
    slotBeforeOverride.material = slotBefore;
    slotBeforeOverride.authored = true;
    slotBeforeOverride.sourceMaterialKey = slotBefore.sourceKey;
    slotBeforeOverride.materialIndex = 0;
    MaterialOverrideComponent slotAfterOverride = slotBeforeOverride;
    slotAfterOverride.material = slotAfter;
    slotAfterOverride.sourceMaterialKey = slotAfter.sourceKey;
    const std::vector<std::pair<UUID, std::optional<MaterialOverrideComponent>>> slotSource{
        { root, slotBeforeOverride }, { reg.get<EntityIdComponent>(childHandle).id, slotBeforeOverride } };
    const std::vector<std::pair<UUID, std::optional<MaterialOverrideComponent>>> slotTarget{
        { root, slotAfterOverride },
        { reg.get<EntityIdComponent>(childHandle).id, slotAfterOverride } };
    const std::vector<std::pair<UUID, std::optional<MaterialOverrideComponent>>> slotBeforeTarget{
        { root, slotBeforeOverride },
        { reg.get<EntityIdComponent>(childHandle).id, slotBeforeOverride } };
    reg.emplace_or_replace<MaterialOverrideComponent>(rootHandle, slotBeforeOverride);
    reg.emplace_or_replace<MaterialOverrideComponent>(childHandle, slotBeforeOverride);
    cycle({ PrefabValueKind::MaterialSlotProperties, {}, PrefabMarkerDirection::After,
        PrefabMaterialSlotValue{ 0, slotBefore, slotBeforeTarget },
        PrefabMaterialSlotValue{ 0, slotAfter, slotTarget } });
    CHECK(f.manager.GetMaterial(0).baseAlpha == slotBefore.baseAlpha);
    CHECK(reg.get<MaterialOverrideComponent>(rootHandle).material.baseAlpha == slotBefore.baseAlpha);
    CHECK(reg.get<MaterialOverrideComponent>(childHandle).material.baseAlpha == slotBefore.baseAlpha);
    std::filesystem::remove_all(dir);
}

// RED proof: forcing the MaterialSlot Before target through canonical After
// derivation makes the undo REQUIRE_FALSE below fail; restore direction-aware
// target staging before the GREEN run.  The captured snapshot deliberately
// includes an absent override and an authored=false historical override.
TEST_CASE("Phase 8 W3: material-slot Before restores exact captured fan-out snapshots")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_s5_material_before_exact");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;
    const UUID root = reg.get<EntityIdComponent>(rootHandle).id;
    const UUID child = reg.get<EntityIdComponent>(childHandle).id;
    reg.emplace_or_replace<MeshRef>(rootHandle, MeshRef{ 0, 0 });
    reg.emplace_or_replace<MeshRef>(childHandle, MeshRef{ 0, 0 });
    reg.emplace_or_replace<ImportedMeshSourceComponent>(rootHandle);
    reg.emplace_or_replace<ImportedMeshSourceComponent>(childHandle);

    const auto beforeMaterial = f.manager.GetMaterial(0);
    auto afterMaterial = beforeMaterial;
    afterMaterial.sourceKey = "canonical-after";
    afterMaterial.baseColor = { 0.31f, 0.42f, 0.53f };
    MaterialOverrideComponent historical;
    historical.material = beforeMaterial;
    historical.authored = false;
    historical.sourceMaterialKey = "historical-source";
    historical.materialIndex = 0;
    MaterialOverrideComponent canonical;
    canonical.material = afterMaterial;
    canonical.authored = true;
    canonical.sourceMaterialKey = afterMaterial.sourceKey;
    canonical.materialIndex = 0;
    const std::vector<std::pair<UUID, std::optional<MaterialOverrideComponent>>> capturedBefore{
        { root, std::nullopt }, { child, historical } };
    const std::vector<std::pair<UUID, std::optional<MaterialOverrideComponent>>> capturedAfter{
        { root, canonical }, { child, canonical } };
    reg.emplace_or_replace<MaterialOverrideComponent>(childHandle, historical);
    const auto schema = f.manager.AuthoringDoc().metadata.schemaVersion;
    const PrefabValueEdit edit{ PrefabValueKind::MaterialSlotProperties, {},
        PrefabMarkerDirection::After,
        PrefabMaterialSlotValue{ 0, beforeMaterial, capturedBefore },
        PrefabMaterialSlotValue{ 0, afterMaterial, capturedAfter } };

    auto execute = f.manager.PreparePrefabCompositeEdits({ edit }, {},
        PrefabMarkerDirection::After, schema, schema);
    REQUIRE(execute.IsOk());
    REQUIRE(f.manager.CommitPrefabCompositePlan(std::move(execute.value)).error.IsOk());
    REQUIRE(reg.get<MaterialOverrideComponent>(rootHandle).authored);
    REQUIRE(reg.get<MaterialOverrideComponent>(childHandle).authored);

    auto undoEdit = edit;
    undoEdit.direction = PrefabMarkerDirection::Before;
    auto undo = f.manager.PreparePrefabCompositeEdits({ undoEdit }, {},
        PrefabMarkerDirection::Before, schema, schema);
    REQUIRE(undo.IsOk());
    REQUIRE(f.manager.CommitPrefabCompositePlan(std::move(undo.value)).error.IsOk());
    CHECK_FALSE(reg.all_of<MaterialOverrideComponent>(rootHandle));
    const auto* restored = reg.try_get<MaterialOverrideComponent>(childHandle);
    REQUIRE(restored);
    CHECK_FALSE(restored->authored);
    CHECK(restored->sourceMaterialKey == historical.sourceMaterialKey);
    CHECK(restored->material.baseColor == historical.material.baseColor);
    CHECK(restored->materialIndex == historical.materialIndex);

    auto redo = f.manager.PreparePrefabCompositeEdits({ edit }, {},
        PrefabMarkerDirection::After, schema, schema);
    REQUIRE(redo.IsOk());
    REQUIRE(f.manager.CommitPrefabCompositePlan(std::move(redo.value)).error.IsOk());
    CHECK(reg.get<MaterialOverrideComponent>(rootHandle).authored);
    CHECK(reg.get<MaterialOverrideComponent>(childHandle).authored);
    CHECK(reg.get<MaterialOverrideComponent>(childHandle).sourceMaterialKey
        == afterMaterial.sourceKey);
    std::filesystem::remove_all(dir);
}

TEST_CASE("Phase 8 W3: composite schema and canonical write-set invariants")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_c3a_writes");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;
    const UUID root = reg.get<EntityIdComponent>(rootHandle).id;
    const auto schema = f.manager.AuthoringDoc().metadata.schemaVersion;
    const auto name = f.manager.GetEntityName({ rootHandle });
    const auto noChange = EditableTRS{ reg.get<Transform>(rootHandle).translation,
        reg.get<Transform>(rootHandle).rotation, reg.get<Transform>(rootHandle).scale };
	reg.emplace_or_replace<CameraComponent>(rootHandle, CameraComponent{});

    auto valueSchema = f.manager.PreparePrefabCompositeEdits(
        { PrefabValueEdit{ PrefabValueKind::EntityName, root,
            PrefabMarkerDirection::After, name, std::string("value") } }, {},
        PrefabMarkerDirection::After, schema, schema + 1);
    REQUIRE_FALSE(valueSchema.IsOk());
    REQUIRE(valueSchema.error.code == rt2::core::Error::InvalidArgument);

    auto emptySchemaPlan = f.manager.PreparePrefabCompositeEdits({}, {},
        PrefabMarkerDirection::After, schema, schema);
    REQUIRE(emptySchemaPlan.IsOk());
    emptySchemaPlan.value.markers.targetSchemaVersion = schema - 1;
    const auto emptySchemaRevision = f.manager.AuthoringRevision();
    const bool emptySchemaDirty = f.manager.IsDirty();
    const auto forgedSchemaResult = f.manager.CommitPrefabCompositePlan(
        std::move(emptySchemaPlan.value));
    REQUIRE_FALSE(forgedSchemaResult.error.IsOk());
    CHECK(f.manager.AuthoringDoc().metadata.schemaVersion == schema);
    CHECK(f.manager.AuthoringRevision() == emptySchemaRevision);
    CHECK(f.manager.IsDirty() == emptySchemaDirty);

    const auto poseBefore = PrefabCameraPoseValue{ noChange, CameraComponent{} };
    auto poseAfter = poseBefore; poseAfter.local.translation.x = 1.0f;
    auto cameraAfter = poseAfter.camera; cameraAfter.verticalFOV = 50.0f;
    const auto camera = PrefabValueEdit{ PrefabValueKind::CameraProperties, root,
        PrefabMarkerDirection::After, poseBefore.camera, cameraAfter };
    const auto pose = PrefabValueEdit{ PrefabValueKind::CameraPose, root,
        PrefabMarkerDirection::After, poseBefore, poseAfter };
    auto conflict = f.manager.PreparePrefabCompositeEdits({ camera, pose }, {},
        PrefabMarkerDirection::After, schema, schema);
    REQUIRE_FALSE(conflict.IsOk());
    REQUIRE(conflict.error.code == rt2::core::Error::InvalidArgument);
    auto reverseConflict = f.manager.PreparePrefabCompositeEdits({ pose, camera }, {},
        PrefabMarkerDirection::After, schema, schema);
    REQUIRE_FALSE(reverseConflict.IsOk());

    f.manager.AddMaterial(SceneMaterial{});
    const auto material0 = f.manager.GetMaterial(0);
    const auto material1 = f.manager.GetMaterial(1);
    auto material0After = material0; material0After.baseAlpha = 0.5f;
    auto material1After = material1; material1After.baseAlpha = 0.25f;
    const auto slot0 = PrefabValueEdit{ PrefabValueKind::MaterialSlotProperties,
        {}, PrefabMarkerDirection::After, PrefabMaterialSlotValue{ 0, material0, {} },
        PrefabMaterialSlotValue{ 0, material0After, {} } };
    const auto slot1 = PrefabValueEdit{ PrefabValueKind::MaterialSlotProperties,
        {}, PrefabMarkerDirection::After, PrefabMaterialSlotValue{ 1, material1, {} },
        PrefabMaterialSlotValue{ 1, material1After, {} } };
    auto disjoint = f.manager.PreparePrefabCompositeEdits({ slot0, slot1 }, {},
        PrefabMarkerDirection::After, schema, schema);
    REQUIRE(disjoint.IsOk());
    const auto disjointResult = f.manager.CommitPrefabCompositePlan(std::move(disjoint.value));
    REQUIRE(disjointResult.error.IsOk());
    CHECK(f.manager.GetMaterial(0).baseAlpha == material0After.baseAlpha);
    CHECK(f.manager.GetMaterial(1).baseAlpha == material1After.baseAlpha);

    PrefabValueEdit forged{ PrefabValueKind::EntityName, root,
        PrefabMarkerDirection::After, name, std::string("forged") };
    auto valid = f.manager.PreparePrefabCompositeEdits({ forged }, {},
        PrefabMarkerDirection::After, schema, schema);
    REQUIRE(valid.IsOk());
    valid.value.values.operations[0].kind = static_cast<PrefabValueKind>(255);
    const auto forgedResult = f.manager.CommitPrefabCompositePlan(std::move(valid.value));
    REQUIRE_FALSE(forgedResult.error.IsOk());
    REQUIRE(forgedResult.error.code == rt2::core::Error::InvalidArgument);

    // Material-bearing plans pin the resource generation independently of
    // document bytes. Compaction of an unreferenced mesh invalidates it.
    reg.emplace_or_replace<MeshRef>(rootHandle, MeshRef{ 0, 0 });
    f.manager.GetECS().meshRegistry.AddMesh(MeshData{});
    f.manager.GetECS().meshRegistry.AddMesh(MeshData{});
    MaterialOverrideComponent overrideValue;
    overrideValue.material = f.manager.GetMaterial(0);
    auto materialPlan = f.manager.PreparePrefabCompositeEdits(
        { PrefabValueEdit{ PrefabValueKind::MaterialIndex, root,
            PrefabMarkerDirection::After,
            PrefabMaterialIndexValue{ 0, std::nullopt },
            PrefabMaterialIndexValue{ 0, overrideValue } } }, {},
        PrefabMarkerDirection::After, schema, schema);
    REQUIRE(materialPlan.IsOk());
    const auto resourceBefore = f.manager.ResourceGeneration();
    REQUIRE(f.manager.CompactMeshRegistry());
    REQUIRE(f.manager.ResourceGeneration() != resourceBefore);
    const auto resourceRevision = f.manager.AuthoringRevision();
    const auto resourceResult = f.manager.CommitPrefabCompositePlan(std::move(materialPlan.value));
    REQUIRE_FALSE(resourceResult.error.IsOk());
    CHECK(f.manager.AuthoringRevision() == resourceRevision);
    std::filesystem::remove_all(dir);
}

TEST_CASE("Phase 8 W3: composite canonical no-op does not dirty transform during marker change")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_c3a_canonical");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;
    const UUID root = reg.get<EntityIdComponent>(rootHandle).id;
    const auto schema = f.manager.AuthoringDoc().metadata.schemaVersion;
    auto& tf = reg.get<Transform>(rootHandle);
    tf.dirty = false;
    CameraComponent camera;
    camera.forwardDirection = { 1.0f, 0.0f, 0.0f };
    reg.emplace_or_replace<CameraComponent>(rootHandle, camera);
    const EditableTRS nonCanonical{ tf.translation, glm::quat(2.0f, 0.0f, 0.0f, 0.0f), tf.scale };
    PrefabValueEdit noop{ PrefabValueKind::LocalTransform, root,
        PrefabMarkerDirection::After, nonCanonical, nonCanonical };
    auto plan = f.manager.PreparePrefabCompositeEdits({ noop },
        { PrefabMarkerEdit{ root, S2Key("transform"), false, true } },
        PrefabMarkerDirection::After, schema, SceneSerializer::SchemaVersion);
    REQUIRE(plan.IsOk());
    const auto result = f.manager.CommitPrefabCompositePlan(std::move(plan.value));
    REQUIRE(result.error.IsOk());
    REQUIRE(result.anyStateChange);
    CHECK(result.appliedValueOperations == 0);
    CHECK(result.syncImpact == rt2::core::SyncImpact::None);
    REQUIRE(result.affectedEntities.size() == 1);
    CHECK(result.affectedEntities.front() == root);
    CHECK_FALSE(reg.get<Transform>(rootHandle).dirty);
    CHECK(reg.get<Transform>(rootHandle).rotation == glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    CHECK(reg.get<CameraComponent>(rootHandle).forwardDirection == camera.forwardDirection);
    std::filesystem::remove_all(dir);
}

// RED proof: removing the finite/degenerate payload guard makes the zero
// quaternion and non-finite camera-pose REQUIRE_FALSE checks fail.
TEST_CASE("Phase 8 W3: composite transform payloads reject non-finite values")
{
    S2Fixture f;
    const auto dir = S2UniqueTempDir("p8w3_c3b_finite");
    const auto [rootHandle, childHandle] = f.MakeInstance(dir);
    auto& reg = f.manager.GetECS().registry;
    const UUID root = reg.get<EntityIdComponent>(rootHandle).id;
    const auto schema = f.manager.AuthoringDoc().metadata.schemaVersion;
    const auto& live = reg.get<Transform>(rootHandle);
    const EditableTRS before{ live.translation, live.rotation, live.scale };

    auto zeroQuaternion = before;
    zeroQuaternion.rotation = glm::quat(0.0f, 0.0f, 0.0f, 0.0f);
    auto zeroPlan = f.manager.PreparePrefabCompositeEdits({
        { PrefabValueKind::LocalTransform, root, PrefabMarkerDirection::After,
            before, zeroQuaternion } }, {},
        PrefabMarkerDirection::After, schema, schema);
    REQUIRE_FALSE(zeroPlan.IsOk());

    auto nonFiniteTranslation = before;
    nonFiniteTranslation.translation.x = std::numeric_limits<float>::quiet_NaN();
    auto nonFinitePlan = f.manager.PreparePrefabCompositeEdits({
        { PrefabValueKind::LocalTransform, root, PrefabMarkerDirection::After,
            before, nonFiniteTranslation } }, {},
        PrefabMarkerDirection::After, schema, schema);
    REQUIRE_FALSE(nonFinitePlan.IsOk());

    reg.emplace_or_replace<CameraComponent>(rootHandle, CameraComponent{});
    REQUIRE(reg.all_of<CameraComponent>(rootHandle));
    PrefabCameraPoseValue poseBefore{ before, reg.get<CameraComponent>(rootHandle) };
    auto poseAfter = poseBefore;
    poseAfter.camera.forwardDirection.y = std::numeric_limits<float>::infinity();
    auto posePlan = f.manager.PreparePrefabCompositeEdits({
        { PrefabValueKind::CameraPose, root, PrefabMarkerDirection::After,
            poseBefore, poseAfter } }, {},
        PrefabMarkerDirection::After, schema, schema);
    REQUIRE_FALSE(posePlan.IsOk());
    std::filesystem::remove_all(dir);
}
