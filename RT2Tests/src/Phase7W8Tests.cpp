#include <doctest/doctest.h>

#include "AssetIdentity.h"
#include "InputBindingEditor.h"
#include "ScriptAssetPath.h"
#include "SceneManager.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace rt2::core;

namespace {

InputMapping AxisMapping(const char* name, KeyCode positive, KeyCode negative)
{
    InputMapping mapping;
    mapping.name = name;
    mapping.isAxis = true;
    mapping.axes.push_back(CaptureAxisBinding(
        InputDeviceKind::KeyboardKey, 0,
        static_cast<uint16_t>(positive), static_cast<uint16_t>(negative),
        -1, 0.0f, false));
    return mapping;
}

struct ScriptRebindFixture
{
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        "rt2_phase7_w8_script_rebind";
    std::filesystem::path oldPath = root / "old.lua";
    std::filesystem::path newPath = root / "new.lua";
    UUID oldId = UUID::Parse("550e8400-e29b-41d4-a716-446655440121");
    UUID newId = UUID::Parse("550e8400-e29b-41d4-a716-446655440122");

    ScriptRebindFixture()
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        REQUIRE(std::filesystem::create_directories(root));
        std::ofstream(oldPath) << "return {}\n";
        std::ofstream(newPath) << "return {}\n";
        Error error;
        REQUIRE(WriteSidecarId(AssetSidecarPath(oldPath), oldId, error));
        REQUIRE(WriteSidecarId(AssetSidecarPath(newPath), newId, error));
    }

    ~ScriptRebindFixture()
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

} // namespace

TEST_CASE("Phase7 W8 InputBindingEditor builds and round-trips an override")
{
    const auto binding = CaptureActionBinding(
        InputDeviceKind::KeyboardKey, static_cast<uint16_t>(KeyCode::Up));
    const auto record = BuildOverrideRecord(
        "runtime", "jump", false, std::vector<ActionBinding>{binding});
    Error error;
    std::vector<InputContextRecord> parsed;
    REQUIRE(ParseInputContextRecords(
        InputContextRecordsToJson({record}),
        InputConfigScope::UserOverrides, parsed, error));
    REQUIRE(parsed.size() == 1);
    REQUIRE(parsed.front().mappings.size() == 1);
    CHECK(parsed.front().contextId == "runtime");
    CHECK(parsed.front().mappings.front().name == "jump");
    REQUIRE(parsed.front().mappings.front().actions.size() == 1);
    CHECK(parsed.front().mappings.front().actions.front().code ==
          static_cast<uint16_t>(KeyCode::Up));
}

TEST_CASE("Phase7 W8 user input override wins over built-in composition")
{
    const std::vector<InputContextRecord> builtIns{
        {"runtime", {AxisMapping("move_forward", KeyCode::W, KeyCode::S)}}};
    const auto overrideRecord = BuildOverrideRecord(
        "runtime", "move_forward", true,
        std::vector<AxisBinding>{CaptureAxisBinding(
            InputDeviceKind::KeyboardKey, 0,
            static_cast<uint16_t>(KeyCode::Up),
            static_cast<uint16_t>(KeyCode::Down), -1, 0.0f, false)});

    std::vector<InputContextRecord> composed;
    Error error;
    REQUIRE(ComposeInputContexts(builtIns, {}, {overrideRecord}, composed, error));
    REQUIRE(composed.size() == 1);
    REQUIRE(composed.front().mappings.size() == 1);
    CHECK(composed.front().mappings.front().axes.front().positive ==
          static_cast<uint16_t>(KeyCode::Up));
}

TEST_CASE("Phase7 W8 explicit unbind removes inherited mapping")
{
    const std::vector<InputContextRecord> builtIns{
        {"runtime", {AxisMapping("move_forward", KeyCode::W, KeyCode::S)}}};
    const auto unbind = BuildOverrideRecord(
        "runtime", "move_forward", true, std::vector<AxisBinding>{});

    std::vector<InputContextRecord> composed;
    Error error;
    REQUIRE(ComposeInputContexts(builtIns, {}, {unbind}, composed, error));
    REQUIRE(composed.size() == 1);
    CHECK(composed.front().mappings.empty());
}

TEST_CASE("Phase7 W8 FindConflicts detects shared bindings in one context")
{
    const auto w = CaptureActionBinding(
        InputDeviceKind::KeyboardKey, static_cast<uint16_t>(KeyCode::W));
    const std::vector<InputContextRecord> records{
        {"runtime", {
            BuildOverrideRecord("runtime", "first", false,
                                std::vector<ActionBinding>{w}).mappings.front(),
            BuildOverrideRecord("runtime", "second", false,
                                std::vector<ActionBinding>{w}).mappings.front(),
        }},
        {"editor", {
            BuildOverrideRecord("editor", "same_key", false,
                                std::vector<ActionBinding>{w}).mappings.front(),
        }},
    };

    const auto conflicts = FindConflicts(records);
    REQUIRE(conflicts.size() == 1);
    CHECK(conflicts.front().contextId == "runtime");
    CHECK(conflicts.front().mappingName == "first");
    CHECK(conflicts.front().conflictingMappingName == "second");
}

TEST_CASE("Phase7 W8 DescribeMapping names keyboard mouse and gamepad bindings")
{
    InputMapping keyboard;
    keyboard.name = "jump";
    keyboard.actions.push_back(CaptureActionBinding(
        InputDeviceKind::KeyboardKey, static_cast<uint16_t>(KeyCode::W)));
    CHECK(DescribeMapping(keyboard) == "W (Keyboard)");

    InputMapping mouse;
    mouse.name = "look";
    mouse.actions.push_back(CaptureActionBinding(
        InputDeviceKind::MouseButton,
        static_cast<uint16_t>(MouseButton::Button1)));
    CHECK(DescribeMapping(mouse) == "Right Mouse");

    InputMapping gamepad;
    gamepad.name = "jump";
    gamepad.actions.push_back(CaptureActionBinding(
        InputDeviceKind::GamepadButton,
        static_cast<uint16_t>(GamepadButton::A)));
    CHECK(DescribeMapping(gamepad) == "Gamepad A");
}

TEST_CASE("Phase7 W8 script rebind resolves the new file identity")
{
    ScriptRebindFixture fixture;
    DeterministicUuidProvider ids;
    SceneManager manager;
    manager.SetUuidProvider(&ids);
    manager.SetAssetResolutionContext({fixture.root, nullptr});

    const auto entity = manager.AuthoringDoc().ecs.registry.create();
    manager.AuthoringDoc().AssignNewUuid(entity);
    const auto entityUuid = manager.GetEntityUuid({entity});

    ScriptComponent initial;
    initial.asset.kind = AssetKind::Script;
    initial.asset.path = "old.lua";
    auto initialResult = manager.SetScriptState(entityUuid, initial);
    REQUIRE(initialResult.success);
    REQUIRE(manager.GetScriptState(entityUuid).has_value());
    CHECK(manager.GetScriptState(entityUuid)->asset.assetId == fixture.oldId);

    auto rebound = manager.GetScriptState(entityUuid).value();
    rebound.asset.path = "new.lua";
    const auto result = manager.SetScriptState(entityUuid, rebound);
    REQUIRE(result.success);

    const auto after = manager.GetScriptState(entityUuid);
    REQUIRE(after.has_value());
    CHECK(after->asset.path == "new.lua");
    CHECK(after->asset.sourceKey == "lua:asset=new.lua");
    CHECK(after->asset.assetId == fixture.newId);

    std::vector<AssetDiagnostic> diagnostics;
    const auto resolved = ResolveScriptAssetPath(
        *after, manager.AssetContext(), entityUuid, "", diagnostics);
    REQUIRE(resolved.success);
    CHECK(resolved.resolvedPath == fixture.newPath);
}
