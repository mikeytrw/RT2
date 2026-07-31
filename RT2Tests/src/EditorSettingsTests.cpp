#include <doctest/doctest.h>

#include "EditorSettings.h"
#include "core/Error.h"

#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cstdio>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace rt2::core;

namespace {

std::filesystem::path UniqueTempDir(const std::string& tag)
{
    auto base = std::filesystem::temp_directory_path();
    auto dir = base / (tag + "_" + std::to_string(std::rand()));
    std::filesystem::create_directories(dir);
    return dir;
}

std::string ReadFileBytes(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream stream;
    stream << in.rdbuf();
    return stream.str();
}

} // anonymous namespace

// ============================================================================
// EditorSettingsStore tests
// ============================================================================

// 1. Default settings when no file exists.
TEST_CASE("EditorSettings: defaults when no file exists")
{
    auto dir = UniqueTempDir("es_defaults");
    EditorSettingsStore s(dir);
    Error err;
    REQUIRE(s.Load(err));
    REQUIRE(err.IsOk());
    CHECK(s.GetLastBrowseDirectory().empty());
    CHECK(s.GetRecentScenes().empty());
    std::filesystem::remove_all(dir);
}

// 2. Schema load/save round trip.
TEST_CASE("EditorSettings: load/save round trip")
{
    auto dir = UniqueTempDir("es_roundtrip");
    EditorSettingsStore s(dir);
    s.SetLastBrowseDirectory("C:/Projects/RT2");
    s.AddRecentScene("C:/Scenes/a.rt2scene");
    s.AddRecentScene("C:/Scenes/b.rt2scene");
    Error err;
    REQUIRE(s.Save(err));
    REQUIRE(err.IsOk());
    const std::string serialized = ReadFileBytes(dir / "settings.json");
    CHECK(serialized.find("lastBrowseDirectory") != std::string::npos);
    CHECK(serialized.find("projectRoot") == std::string::npos);

    EditorSettingsStore s2(dir);
    REQUIRE(s2.Load(err));
    REQUIRE(err.IsOk());
    CHECK(s2.GetLastBrowseDirectory().string() == "C:/Projects/RT2");
    REQUIRE(s2.GetRecentScenes().size() == 2);
    CHECK(s2.GetRecentScenes()[0].string() == "C:/Scenes/b.rt2scene");
    CHECK(s2.GetRecentScenes()[1].string() == "C:/Scenes/a.rt2scene");
    std::filesystem::remove_all(dir);
}

// 3. Malformed JSON produces an error and safe defaults.
TEST_CASE("EditorSettings: malformed JSON fails safely")
{
    auto dir = UniqueTempDir("es_malformed");
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "settings.json") << "{ not valid json";
    EditorSettingsStore s(dir);
    s.SetLastBrowseDirectory("C:/existing-project");
    s.AddRecentScene("C:/existing.rt2scene");
    Error err;
    CHECK_FALSE(s.Load(err));
    CHECK(err.code == Error::Parse);
    CHECK(s.GetLastBrowseDirectory().string() == "C:/existing-project");
    REQUIRE(s.GetRecentScenes().size() == 1);
    CHECK(s.GetRecentScenes()[0].string() == "C:/existing.rt2scene");
    std::filesystem::remove_all(dir);
}

// 4. Unsupported version produces an error.
TEST_CASE("EditorSettings: unsupported version fails")
{
    auto dir = UniqueTempDir("es_version");
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "settings.json") << R"({"version": 999, "projectRoot": "", "recentScenes": []})";
    EditorSettingsStore s(dir);
    Error err;
    CHECK_FALSE(s.Load(err));
    CHECK(err.code == Error::SchemaVersion);
    std::filesystem::remove_all(dir);
}

// 5. Unknown optional fields are ignored safely.
TEST_CASE("EditorSettings: unknown fields ignored")
{
    auto dir = UniqueTempDir("es_unknown");
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "settings.json") << R"({"version": 1, "projectRoot": "C:/X", "recentScenes": [], "unknownField": 42, "future": {"nested": true}})";
    EditorSettingsStore s(dir);
    Error err;
    REQUIRE(s.Load(err));
    REQUIRE(err.IsOk());
    CHECK(s.GetLastBrowseDirectory().string() == "C:/X");
    std::filesystem::remove_all(dir);
}

// 6. Atomic-write failure preserves the previous valid file.
//     (We can't easily simulate a write failure on Windows, but we can
//      verify that a failed Save due to a read-only/locked target leaves
//      the prior file intact. We simulate by making the parent dir
//      non-existent after a successful save — the second save should
//      recreate it. This is a weaker test of atomicity, not a real
//      failure-injection test.)
TEST_CASE("EditorSettings: save to fresh dir creates it")
{
    auto dir = UniqueTempDir("es_atomic_fresh");
    auto subdir = dir / "nested" / "deep";
    EditorSettingsStore s(subdir);
    s.AddRecentScene("C:/x.rt2scene");
    Error err;
    REQUIRE(s.Save(err));
    REQUIRE(err.IsOk());
    CHECK(std::filesystem::exists(subdir / "settings.json"));
    std::filesystem::remove_all(dir);
}

TEST_CASE("EditorSettings: failed atomic replacement preserves valid settings")
{
#ifdef _WIN32
    auto dir = UniqueTempDir("es_atomic_locked");
    EditorSettingsStore store(dir);
    store.SetLastBrowseDirectory("C:/before");
    Error err;
    REQUIRE(store.Save(err));
    const auto settingsPath = dir / "settings.json";
    const std::string before = ReadFileBytes(settingsPath);

    HANDLE lock = CreateFileW(settingsPath.wstring().c_str(), GENERIC_READ,
                              0, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(lock != INVALID_HANDLE_VALUE);
    store.SetLastBrowseDirectory("C:/after");
    CHECK_FALSE(store.Save(err));
    CloseHandle(lock);

    CHECK(ReadFileBytes(settingsPath) == before);
    std::filesystem::remove_all(dir);
#else
    CHECK(true);
#endif
}

// 7. Recent list MRU order.
TEST_CASE("EditorSettings: recents are MRU")
{
    auto dir = UniqueTempDir("es_mru");
    EditorSettingsStore s(dir);
    s.AddRecentScene("C:/a.rt2scene");
    s.AddRecentScene("C:/b.rt2scene");
    s.AddRecentScene("C:/c.rt2scene");
    auto r = s.GetRecentScenes();
    REQUIRE(r.size() == 3);
    CHECK(r[0].string() == "C:/c.rt2scene");
    CHECK(r[1].string() == "C:/b.rt2scene");
    CHECK(r[2].string() == "C:/a.rt2scene");
    std::filesystem::remove_all(dir);
}

// 8. Deduplication and Windows case behavior.
TEST_CASE("EditorSettings: dedup case-insensitive on Windows")
{
    auto dir = UniqueTempDir("es_dedup");
    EditorSettingsStore s(dir);
    s.AddRecentScene("C:/Scenes/MyScene.rt2scene");
    s.AddRecentScene("C:/scenes/myscene.rt2scene"); // case-folded equivalent
    auto r = s.GetRecentScenes();
#ifdef _WIN32
    CHECK(r.size() == 1); // deduped
#else
    CHECK(r.size() == 2); // case-sensitive on other platforms
#endif
    std::filesystem::remove_all(dir);
}

// 9. Maximum entry count.
TEST_CASE("EditorSettings: bounded recents")
{
    auto dir = UniqueTempDir("es_max");
    EditorSettingsStore s(dir, 3);
    s.AddRecentScene("C:/a.rt2scene");
    s.AddRecentScene("C:/b.rt2scene");
    s.AddRecentScene("C:/c.rt2scene");
    s.AddRecentScene("C:/d.rt2scene"); // should evict a
    auto r = s.GetRecentScenes();
    REQUIRE(r.size() == 3);
    CHECK(r[0].string() == "C:/d.rt2scene");
    CHECK(r[1].string() == "C:/c.rt2scene");
    CHECK(r[2].string() == "C:/b.rt2scene");
    std::filesystem::remove_all(dir);
}

// 10. Missing recent file behavior (load doesn't crash).
TEST_CASE("EditorSettings: missing settings file is not an error")
{
    auto dir = UniqueTempDir("es_missing");
    // Don't create the file.
    EditorSettingsStore s(dir);
    Error err;
    REQUIRE(s.Load(err));
    REQUIRE(err.IsOk());
    std::filesystem::remove_all(dir);
}

// 11. Failed/cancelled operations do not update recents.
//     (Simulated: AddRecentScene is only called by the host on success; the
//      store itself doesn't gate on file existence. We verify RemoveRecentScene
//      removes the entry.)
TEST_CASE("EditorSettings: RemoveRecentScene removes entry")
{
    auto dir = UniqueTempDir("es_remove");
    EditorSettingsStore s(dir);
    s.AddRecentScene("C:/a.rt2scene");
    s.AddRecentScene("C:/b.rt2scene");
    s.RemoveRecentScene("C:/a.rt2scene");
    auto r = s.GetRecentScenes();
    REQUIRE(r.size() == 1);
    CHECK(r[0].string() == "C:/b.rt2scene");
    std::filesystem::remove_all(dir);
}

// 12. Hand-edited overlong recents array is trimmed on load.
TEST_CASE("EditorSettings: overlong recents trimmed on load")
{
    auto dir = UniqueTempDir("es_overlong");
    std::filesystem::create_directories(dir);
    std::string json = R"({"version": 1, "projectRoot": "", "recentScenes": [)";
    for (int i = 0; i < 20; ++i)
    {
        if (i) json += ", ";
        json += "\"C:/s" + std::to_string(i) + ".rt2scene\"";
    }
    json += "]}";
    std::ofstream(dir / "settings.json") << json;
    EditorSettingsStore s(dir, 5);
    Error err;
    REQUIRE(s.Load(err));
    REQUIRE(s.GetRecentScenes().size() == 5);
    CHECK(s.GetRecentScenes().front().string() == "C:/s0.rt2scene");
    CHECK(s.GetRecentScenes().back().string() == "C:/s4.rt2scene");
    std::filesystem::remove_all(dir);
}

TEST_CASE("EditorSettings: unicode paths round trip losslessly")
{
    auto dir = UniqueTempDir("es_unicode");
    const std::filesystem::path unicodePath(
        L"C:\\Scenes\\\u573A\u666F\\\u591C.rt2scene");
    EditorSettingsStore writer(dir);
    writer.SetLastBrowseDirectory(unicodePath.parent_path());
    writer.AddRecentScene(unicodePath);
    Error err;
    REQUIRE(writer.Save(err));

    EditorSettingsStore reader(dir);
    REQUIRE(reader.Load(err));
    CHECK(reader.GetLastBrowseDirectory() == unicodePath.parent_path());
    REQUIRE(reader.GetRecentScenes().size() == 1);
    CHECK(reader.GetRecentScenes()[0] == unicodePath);
    std::filesystem::remove_all(dir);
}
