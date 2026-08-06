#include <doctest/doctest.h>

#include "PrefabComponentKey.h"
#include "PersistedComponents.h"

#include <cstddef>
#include <optional>
#include <string>

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