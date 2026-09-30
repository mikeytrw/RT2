#include <doctest/doctest.h>

#include "EditorOutlinerFilter.h"
#include "SceneManager.h"
#include "core/UUID.h"

// ============================================================================
// Editor outliner filter tests — Scene Browser classification and matching.
//
// Per-entity type bits from a live SceneManager fixture, the pure
// name/type/query predicate, primary-tag priority, and the status wording
// split (debounced pending vs failed). Render (tree, colors, chips) stays
// native-side; the token table pins tag text and colors here.
// ============================================================================

using namespace rt2::core;

namespace {

struct BrowserSceneFixture
{
    DeterministicUuidProvider ids;
    SceneManager manager;

    BrowserSceneFixture()
    {
        manager.SetUuidProvider(&ids);
        manager.AddMaterial(SceneMaterial{});
    }

    UUID AddNamed(const char* name)
    {
        const auto uuid = manager.CreateEmpty(name).affectedEntities.front();
        return uuid;
    }

    entt::entity EntityOf(const UUID& uuid)
    {
        return manager.FindEntityByUuid(uuid);
    }
};

} // anonymous namespace

TEST_CASE("BrowserFilter: classification covers every component family")
{
    BrowserSceneFixture f;
    const UUID plain = f.AddNamed("Plain");
    const UUID light = f.AddNamed("Lamp");
    const UUID audio = f.AddNamed("Speaker");
    const UUID script = f.AddNamed("Brain");
    REQUIRE((f.EntityOf(light) != entt::null));
    f.manager.GetECS().registry.emplace<LightComponent>(f.EntityOf(light));
    f.manager.GetECS().registry.emplace<AudioSourceComponent>(f.EntityOf(audio));
    f.manager.GetECS().registry.emplace<ScriptComponent>(f.EntityOf(script));

    CHECK(ClassifyEntityForBrowser(f.manager,
        SceneManager::EntityId{ f.EntityOf(plain) }) == 0u);
    const auto lightBits = ClassifyEntityForBrowser(f.manager,
        SceneManager::EntityId{ f.EntityOf(light) });
    CHECK((lightBits & static_cast<std::uint32_t>(EntityBrowserType::Light)) != 0);
    const auto audioBits = ClassifyEntityForBrowser(f.manager,
        SceneManager::EntityId{ f.EntityOf(audio) });
    CHECK((audioBits & static_cast<std::uint32_t>(EntityBrowserType::Audio)) != 0);
    const auto scriptBits = ClassifyEntityForBrowser(f.manager,
        SceneManager::EntityId{ f.EntityOf(script) });
    CHECK((scriptBits & static_cast<std::uint32_t>(EntityBrowserType::Script)) != 0);
    // Invalid entities classify empty (shown plain, never hidden by error).
    CHECK(ClassifyEntityForBrowser(f.manager, SceneManager::EntityId{}) == 0u);
}

TEST_CASE("BrowserFilter: physics matches any of body/shape/hinge/slider")
{
    BrowserSceneFixture f;
    const UUID body = f.AddNamed("Body");
    const UUID shape = f.AddNamed("Shape");
    f.manager.GetECS().registry.emplace<PhysicsBodyComponent>(f.EntityOf(body));
    f.manager.GetECS().registry.emplace<PhysicsShapeComponent>(f.EntityOf(shape));
    const auto physicsBit = static_cast<std::uint32_t>(EntityBrowserType::Physics);
    CHECK((ClassifyEntityForBrowser(f.manager,
        SceneManager::EntityId{ f.EntityOf(body) }) & physicsBit) != 0);
    CHECK((ClassifyEntityForBrowser(f.manager,
        SceneManager::EntityId{ f.EntityOf(shape) }) & physicsBit) != 0);
}

TEST_CASE("BrowserFilter: primary tag priority and colors")
{
    using T = EntityBrowserType;
    const auto bit = [](T f) { return static_cast<std::uint32_t>(f); };
    CHECK(std::string(TokenForBrowserTypes(0u).tag) == "[Entity]");
    CHECK(std::string(TokenForBrowserTypes(bit(T::Mesh)).tag) == "[Mesh]");
    CHECK(std::string(TokenForBrowserTypes(bit(T::Camera)).tag) == "[Camera]");
    // Multi-type entities show the priority winner, never plain.
    CHECK(std::string(TokenForBrowserTypes(
        bit(T::Mesh) | bit(T::Physics)).tag) == "[Physics]");
    CHECK(std::string(TokenForBrowserTypes(
        bit(T::Mesh) | bit(T::Light)).tag) == "[Light]");
    // Tags are tinted, never blue-channel dominant except camera/indigo.
    const auto light = TokenForBrowserTypes(bit(T::Light));
    CHECK(light.r > light.b);
    const auto script = TokenForBrowserTypes(bit(T::Script));
    CHECK(script.g > script.r);
}

TEST_CASE("BrowserFilter: predicate combines type mask and name query")
{
    using T = EntityBrowserType;
    const auto bit = [](T f) { return static_cast<std::uint32_t>(f); };
    const std::uint32_t mesh = bit(T::Mesh);
    const std::uint32_t light = bit(T::Light);
    // Empty mask passes everything (no stuck-empty browser).
    CHECK(MatchesBrowserFilter("Lamp", light, "", 0u));
    CHECK(MatchesBrowserFilter("Lamp", light, "", kEntityBrowserAllTypes));
    // Type dimension.
    CHECK(MatchesBrowserFilter("Lamp", light, "", light));
    CHECK_FALSE(MatchesBrowserFilter("Lamp", light, "", mesh));
    CHECK(MatchesBrowserFilter("Box", mesh | bit(T::Physics), "", bit(T::Physics)));
    // Name dimension: case-insensitive substring.
    CHECK(MatchesBrowserFilter("Lamps_1stFloor", light, "lamps", light));
    CHECK_FALSE(MatchesBrowserFilter("Lamps_1stFloor", light, "column", light));
    CHECK(MatchesBrowserFilter("Box", mesh, "", mesh));
    // Both must pass.
    CHECK_FALSE(MatchesBrowserFilter("Lamp", light, "lamp", mesh));
}
