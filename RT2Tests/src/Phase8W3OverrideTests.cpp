#include <doctest/doctest.h>

#include "PrefabComponentKey.h"
#include "PersistedComponents.h"
#include "PrefabSerializer.h"
#include "SceneManager.h"
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
//   Sequence c) read path fault: don't sort the parsed overrides — after one
//   reload round the ordering is non-canonical and round 2's bytes differ from
//   round 1, so the byte-identical assertion fails -> RED.
//   Sequence d) read path fault: write "overrides" only when the version is
//   current but read it for any version, with a stray overrides set on a v5
//   file — not part of this test (spec test 2 covers the gate).
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W3 S2: override set round-trips and loads with the exact contents")
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
    docU.SetUuidProvider(&DeterministicUuidProvider());
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
TEST_CASE("S2: a v5 scene loads with every override set empty")
{
    const auto dir = S2UniqueTempDir("p8w3_s2_v5_empty");
    const auto scenePath = dir / "v5.rt2scene";
    S2WriteMemberScene(scenePath, 5, {"transform", "script"});

    SceneDocument doc;
    doc.SetUuidProvider(&DeterministicUuidProvider());
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
TEST_CASE("S2: recovery SaveTo writes v6 and keeps an added override (upgrade rule)")
{
    const auto dir = S2UniqueTempDir("p8w3_s2_recovery_upgrade");
    const auto scenePath = dir / "v5.rt2scene";
    S2WriteMemberScene(scenePath, 5, {});

    SceneDocument scene;
    scene.SetUuidProvider(&DeterministicUuidProvider());
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
TEST_CASE("S2: an unknown override component key fails loudly and leaves the destination untouched")
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
    doc.SetUuidProvider(&DeterministicUuidProvider());
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
TEST_CASE("S2: a prefab record with all shapes carries no scene-side link or override data")
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