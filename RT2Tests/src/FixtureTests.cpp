#include <doctest/doctest.h>

#include "FixtureGenerator.h"
#include "SceneSerializer.h"
#include "SceneDocument.h"
#include "ECSComponents.h"
#include "core/UUID.h"
#include "core/Error.h"

#include <filesystem>
#include <fstream>

using namespace rt2::core;

// ============================================================================
// VS-5 Fixture: generate and verify the checked-in slice fixture
// ============================================================================

TEST_CASE("VS-5 Fixture: generate slice fixture")
{
    auto path = std::filesystem::current_path() / "RT2App" / "assets" / "vertical-slice.rt2scene";
    Error err;
    CHECK(GenerateSliceFixture(path, err));
    CHECK(err.IsOk());
    CHECK(std::filesystem::exists(path));
}

TEST_CASE("VS-5 Fixture: load slice fixture and verify structure")
{
    auto path = std::filesystem::current_path() / "RT2App" / "assets" / "vertical-slice.rt2scene";
    REQUIRE(std::filesystem::exists(path));

    DeterministicUuidProvider provider;
    SceneDocument doc;
    doc.SetUuidProvider(&provider);
    Error err;
    CHECK(SceneSerializer::Load(doc, path, err));
    CHECK(err.IsOk());

    // 3 entities: cube, light, camera
    CHECK(doc.ecs.registry.view<EntityIdComponent>().size() == 3);

    // 1 material
    CHECK(doc.ecs.materials.size() == 1);

    // 1 mesh (cube)
    CHECK(doc.ecs.meshRegistry.GetCount() == 1);

    // Find the cube and verify velocity
    bool foundCube = false;
    auto view = doc.ecs.registry.view<EntityIdComponent>();
    for (auto e : view)
    {
        if (doc.ecs.registry.all_of<MotionComponent>(e))
        {
            foundCube = true;
            auto& mc = doc.ecs.registry.get<MotionComponent>(e);
            CHECK(mc.linearVelocity.x == doctest::Approx(1.0f));
            CHECK(mc.linearVelocity.y == doctest::Approx(0.0f));
            CHECK(mc.linearVelocity.z == doctest::Approx(0.0f));
        }
    }
    CHECK(foundCube);
}

TEST_CASE("VS-5 Fixture: fixture is deterministic across regenerations")
{
    // Save twice to the SAME path to verify byte-determinism. The
    // metadata.sourcePath is serialized, so different paths produce
    // different JSON — which is correct behavior.
    auto path = std::filesystem::temp_directory_path() / "rt2_fix_det.rt2scene";
    Error err;

    CHECK(GenerateSliceFixture(path, err));
    std::ifstream f1(path, std::ios::binary);
    std::string c1((std::istreambuf_iterator<char>(f1)), std::istreambuf_iterator<char>());
    f1.close();

    CHECK(GenerateSliceFixture(path, err));
    std::ifstream f2(path, std::ios::binary);
    std::string c2((std::istreambuf_iterator<char>(f2)), std::istreambuf_iterator<char>());
    f2.close();

    CHECK(c1 == c2);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}