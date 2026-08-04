#include <doctest/doctest.h>

#include "EntityReferenceRemapper.h"
#include "SceneManager.h"
#include "SceneSerializer.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

using namespace rt2::core;

namespace
{

UUID ParseUuid(const char* text)
{
    const auto uuid = UUID::Parse(text);
    REQUIRE_FALSE(uuid.IsNull());
    return uuid;
}

ScriptComponent& ScriptFor(SceneManager& manager, const UUID& entity)
{
    const auto handle = manager.FindEntityByUuid(entity);
    REQUIRE(static_cast<uint32_t>(handle) != static_cast<uint32_t>(entt::null));
    auto& registry = manager.GetECS().registry;
    auto* existing = registry.try_get<ScriptComponent>(handle);
    auto& script = existing
        ? *existing
        : registry.emplace<ScriptComponent>(handle);
    script.asset.kind = AssetKind::Script;
    script.asset.path = "shared.lua";
    script.asset.sourceKey = "lua:asset=shared.lua";
    return script;
}

void SetUuidField(SceneManager& manager,
                  const UUID& entity,
                  const char* name,
                  const UUID& value)
{
    auto& script = ScriptFor(manager, entity);
    script.fieldValues[name] = ScriptFieldEntry{ ScriptFieldType::Uuid, value };
}

UUID FieldUuid(const ScriptComponent& script, const char* name)
{
    const auto it = script.fieldValues.find(name);
    REQUIRE(it != script.fieldValues.end());
    const auto* value = std::get_if<UUID>(&it->second.value);
    REQUIRE(value != nullptr);
    return *value;
}

UUID MappedUuid(const SceneManager::DuplicationResult& result,
                const UUID& source)
{
    const auto it = std::find_if(
        result.sourceToDuplicate.begin(), result.sourceToDuplicate.end(),
        [&](const auto& pair) { return pair.first == source; });
    REQUIRE(it != result.sourceToDuplicate.end());
    return it->second;
}

struct SubtreeFixture
{
    DeterministicUuidProvider ids;
    SceneManager manager;
    UUID root;
    UUID child;
    UUID external;

    SubtreeFixture()
    {
        manager.SetUuidProvider(&ids);
        root = manager.CreateEmpty("A").affectedEntities.front();
        child = manager.CreateEmpty("B", root).affectedEntities.front();
        external = manager.CreateEmpty("C").affectedEntities.front();
    }
};

} // namespace

// Fault for red: remove the RemapEntityReferences call from the UUID-supplied
// duplicate path. A's copied field then still names the original B.
TEST_CASE("Phase 8 W2: duplicate remaps an internal script UUID to the copy")
{
    SubtreeFixture fixture;
    SetUuidField(fixture.manager, fixture.root, "sibling", fixture.child);

    const auto duplicateUuids = fixture.manager.ReserveKnownUuids(2);
    const auto duplicated = fixture.manager.DuplicateSubtreesWithUuids(
        { fixture.root }, duplicateUuids);
    REQUIRE(duplicated.mutation.success);

    const auto duplicateRoot = MappedUuid(duplicated, fixture.root);
    const auto duplicateChild = MappedUuid(duplicated, fixture.child);
    const auto duplicateScript = fixture.manager.GetScriptState(duplicateRoot);
    REQUIRE(duplicateScript.has_value());
    CHECK(FieldUuid(*duplicateScript, "sibling") == duplicateChild);
    CHECK(FieldUuid(*duplicateScript, "sibling") != fixture.child);
}

// Fault for red: replace the remapper's map lookup with an unconditional
// rewrite. A reference to C is outside the copied subtree and must survive.
TEST_CASE("Phase 8 W2: duplicate preserves an external script UUID")
{
    SubtreeFixture fixture;
    SetUuidField(fixture.manager, fixture.root, "external", fixture.external);

    const auto duplicateUuids = fixture.manager.ReserveKnownUuids(2);
    const auto duplicated = fixture.manager.DuplicateSubtreesWithUuids(
        { fixture.root }, duplicateUuids);
    REQUIRE(duplicated.mutation.success);

    const auto duplicateRoot = MappedUuid(duplicated, fixture.root);
    const auto duplicateScript = fixture.manager.GetScriptState(duplicateRoot);
    REQUIRE(duplicateScript.has_value());
    CHECK(FieldUuid(*duplicateScript, "external") == fixture.external);
}

// Fault for red: remove the RemapEntityReferences call from the paste path.
// The same internal/external rule must hold when the source comes from the
// clipboard document rather than the live destination registry.
TEST_CASE("Phase 8 W2: paste remaps internal and preserves external script UUIDs")
{
    SubtreeFixture fixture;
    SetUuidField(fixture.manager, fixture.root, "internal", fixture.child);
    SetUuidField(fixture.manager, fixture.root, "external", fixture.external);

    SceneDocument clipboard;
    clipboard.SetUuidProvider(&fixture.ids);
    Error cloneError;
    REQUIRE(SceneSerializer::CloneInMemory(
        fixture.manager.AuthoringDoc(), clipboard, cloneError));
    REQUIRE(cloneError.IsOk());

    const auto pastedUuids = fixture.manager.ReserveKnownUuids(2);
    const auto pasted = fixture.manager.PasteSubtreesWithUuids(
        clipboard, { fixture.root }, std::nullopt, pastedUuids);
    REQUIRE(pasted.mutation.success);

    const auto pastedRoot = MappedUuid(pasted, fixture.root);
    const auto pastedChild = MappedUuid(pasted, fixture.child);
    const auto pastedScript = fixture.manager.GetScriptState(pastedRoot);
    REQUIRE(pastedScript.has_value());
    CHECK(FieldUuid(*pastedScript, "internal") == pastedChild);
    CHECK(FieldUuid(*pastedScript, "external") == fixture.external);
}

// Decision: a stale UUID is not in the mapping, so preserve it. We cannot
// infer whether it was intended for an entity that was deleted, lives in a
// different scene, or is simply authored data. Fault for red: rewrite every
// UUID value to the mapping's default/missing target.
TEST_CASE("Phase 8 W2: stale script UUID is preserved")
{
    ScriptComponent script;
    const auto stale = ParseUuid("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    script.fieldValues["stale"] =
        ScriptFieldEntry{ ScriptFieldType::Uuid, stale };
    std::vector<ScriptComponent*> components{ &script };
    const EntityUuidRemap remap{
        { ParseUuid("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"),
          ParseUuid("cccccccc-cccc-4ccc-8ccc-cccccccccccc") }
    };

    RemapEntityReferences(remap, components);
    CHECK(FieldUuid(script, "stale") == stale);
}

// Fault for red: remap any UUID variant without respecting the semantic type
// tag. Valid Color/Float/String entries and a malformed non-Uuid-tagged UUID
// payload must all remain untouched.
TEST_CASE("Phase 8 W2: non-Uuid script fields are untouched")
{
    const auto target = ParseUuid("dddddddd-dddd-4ddd-8ddd-dddddddddddd");
    const auto replacement = ParseUuid("eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee");
    ScriptComponent script;
    script.fieldValues["color"] =
        ScriptFieldEntry{ ScriptFieldType::Color, glm::vec3(0.1f, 0.2f, 0.3f) };
    script.fieldValues["float"] =
        ScriptFieldEntry{ ScriptFieldType::Float, 12.5 };
    script.fieldValues["string"] =
        ScriptFieldEntry{ ScriptFieldType::String, std::string("target") };
    script.fieldValues["mismatched"] =
        ScriptFieldEntry{ ScriptFieldType::Float, target };
    const auto before = script.fieldValues;

    std::vector<ScriptComponent*> components{ &script };
    RemapEntityReferences({ { target, replacement } }, components);

    CHECK(script.fieldValues == before);
}
