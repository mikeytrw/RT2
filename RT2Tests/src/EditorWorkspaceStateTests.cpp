#include <doctest/doctest.h>

#include "EditorWorkspaceState.h"
#include "core/Error.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// ============================================================================
// EditorWorkspaceState tests — persistence-foundation package acceptance.
//
// Every case uses a unique temporary directory; the developer's real
// profile is never touched. Rows map to the accepted plan's verification
// table: U/P/V precedence, empty + unreadable portable ini, empty +
// unreadable view_config, legacy omitted flags + unknown keys, malformed
// known values + perfDetail bounds, read/write/flush/replace failures,
// backups, partial save/retry, headless no-write.
// ============================================================================

using namespace rt2::core;
namespace fs = std::filesystem;
using Clock = EditorWorkspaceState::Clock;

namespace {

int g_TempCounter = 0;

fs::path UniqueTempDir(const std::string& tag)
{
    auto dir = fs::temp_directory_path() /
        ("rt2_ws_" + tag + "_" + std::to_string(++g_TempCounter));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

void WriteBytes(const fs::path& p, const std::string& content)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string ReadBytes(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

Clock::time_point T(double seconds)
{
    return Clock::time_point{} + std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(seconds));
}

} // anonymous namespace

// ---- Startup classification: the U/P/V presence matrix ----------------------

TEST_CASE("Workspace: new profile only when U/P/V all confirmed absent")
{
    auto dir = UniqueTempDir("newprofile");
    auto exe = UniqueTempDir("newprofile_exe");
    EditorWorkspaceState ws(dir, exe);
    const auto c = ws.ClassifyStartup();
    CHECK(c.isNewProfile);
    CHECK(c.geometrySource == WorkspaceGeometrySource::NewDefault);
    CHECK(c.visibilityBasis == WorkspaceVisibilityBasis::NewProfile);
    CHECK_FALSE(c.userIniProtected);
    CHECK_FALSE(c.viewConfigProtected);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::remove_all(exe, ec);
}

TEST_CASE("Workspace: U/P/V presence matrix picks geometry and visibility basis")
{
    // U absent, P absent, V present -> new-default geometry, legacy basis.
    {
        auto dir = UniqueTempDir("matrix_v");
        auto exe = UniqueTempDir("matrix_v_exe");
        WriteBytes(dir / "view_config.txt", "showScene=0\n");
        EditorWorkspaceState ws(dir, exe);
        const auto c = ws.ClassifyStartup();
        CHECK_FALSE(c.isNewProfile);
        CHECK(c.geometrySource == WorkspaceGeometrySource::NewDefault);
        CHECK(c.visibilityBasis == WorkspaceVisibilityBasis::Legacy);
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::remove_all(exe, ec);
    }
    // U absent, P present, V absent -> portable seed, legacy basis.
    {
        auto dir = UniqueTempDir("matrix_p");
        auto exe = UniqueTempDir("matrix_p_exe");
        WriteBytes(exe / "imgui.ini", "[Window][Viewport]\n");
        EditorWorkspaceState ws(dir, exe);
        const auto c = ws.ClassifyStartup();
        CHECK_FALSE(c.isNewProfile);
        CHECK(c.geometrySource == WorkspaceGeometrySource::PortableSeed);
        CHECK(c.visibilityBasis == WorkspaceVisibilityBasis::Legacy);
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::remove_all(exe, ec);
    }
    // U present wins over P present.
    {
        auto dir = UniqueTempDir("matrix_up");
        auto exe = UniqueTempDir("matrix_up_exe");
        WriteBytes(dir / "imgui.ini", "[Window][User]\n");
        WriteBytes(exe / "imgui.ini", "[Window][Portable]\n");
        WriteBytes(dir / "view_config.txt", "showScene=0\n");
        EditorWorkspaceState ws(dir, exe);
        const auto c = ws.ClassifyStartup();
        CHECK(c.geometrySource == WorkspaceGeometrySource::UserIni);
        CHECK(c.visibilityBasis == WorkspaceVisibilityBasis::Legacy);
        EditorWorkspaceState::GeometryLoad load = ws.LoadGeometryBytes();
        REQUIRE(load.IsOk());
        CHECK(load.bytes == "[Window][User]\n");
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::remove_all(exe, ec);
    }
}

// ---- Empty and unreadable files are protected, never absent ------------------

TEST_CASE("Workspace: readable empty user ini is protected session-only")
{
    auto dir = UniqueTempDir("emptyini");
    auto exe = UniqueTempDir("emptyini_exe");
    WriteBytes(dir / "imgui.ini", "");
    WriteBytes(exe / "imgui.ini", "[Window][Portable]\n"); // must NOT seed
    EditorWorkspaceState ws(dir, exe);
    const auto c = ws.ClassifyStartup();
    CHECK_FALSE(c.isNewProfile);
    CHECK(c.userIniProtected);
    CHECK(c.geometrySource == WorkspaceGeometrySource::SessionDefault);
    // Empty carries no typed error, but the state itself is the status.
    CHECK(ws.Status().geometry.state == WorkspaceFileState::SessionOnlyProtected);
    EditorWorkspaceState::GeometryLoad load = ws.LoadGeometryBytes();
    CHECK(load.bytes.empty());
    // A normal save is refused loudly; only Reset (after backup) may proceed.
    Error err;
    CHECK_FALSE(ws.SaveGeometry(T(10.0), true, "[Window][X]\n", err));
    CHECK_FALSE(err.IsOk());
    CHECK(ReadBytes(dir / "imgui.ini").empty()); // original untouched
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::remove_all(exe, ec);
}

TEST_CASE("Workspace: unreadable user ini never falls back to portable")
{
    auto dir = UniqueTempDir("badini");
    auto exe = UniqueTempDir("badini_exe");
    // A directory at the file path exists but is not readable as a file.
    std::error_code ec;
    fs::create_directories(dir / "imgui.ini", ec);
    WriteBytes(exe / "imgui.ini", "[Window][Portable]\n");
    EditorWorkspaceState ws(dir, exe);
    const auto c = ws.ClassifyStartup();
    CHECK(c.userIniProtected);
    CHECK(c.geometrySource == WorkspaceGeometrySource::SessionDefault);
    // Reset stays unavailable until access is repaired: no destructive bypass.
    Error err;
    CHECK_FALSE(ws.CanReset(err));
    CHECK_FALSE(err.IsOk());
    fs::remove_all(dir, ec);
    fs::remove_all(exe, ec);
}

TEST_CASE("Workspace: empty portable seed is protected until Reset, never written")
{
    auto dir = UniqueTempDir("emptyseed");
    auto exe = UniqueTempDir("emptyseed_exe");
    WriteBytes(exe / "imgui.ini", "");
    EditorWorkspaceState ws(dir, exe);
    const auto c = ws.ClassifyStartup();
    CHECK_FALSE(c.isNewProfile);
    CHECK(c.portableSeedProtected);
    CHECK(c.geometrySource == WorkspaceGeometrySource::SessionDefault);
    // Reset is available (nothing unreadable) and must not modify the seed.
    Error err;
    REQUIRE(ws.ResetToDefaults(T(10.0), "[Window][SessionBaseline]\n", err));
    CHECK(ReadBytes(exe / "imgui.ini").empty());
    CHECK(fs::exists(dir / "imgui.ini"));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::remove_all(exe, ec);
}

TEST_CASE("Workspace: readable empty view_config uses legacy defaults, backed up before rewrite")
{
    auto dir = UniqueTempDir("emptyview");
    EditorWorkspaceState ws(dir, dir);
    WriteBytes(dir / "view_config.txt", "");
    REQUIRE(ws.ClassifyStartup().visibilityBasis == WorkspaceVisibilityBasis::Legacy);
    const auto loaded = ws.LoadVisibility(WorkspaceVisibility::LegacyDefaults());
    REQUIRE(loaded.IsOk());
    CHECK(loaded.visibility == WorkspaceVisibility::LegacyDefaults());
    // A normal save proceeds (unlike the empty ini) but backs up first.
    WorkspaceVisibility changed = WorkspaceVisibility::LegacyDefaults();
    changed.showScene = false;
    ws.MarkVisibilitySnapshot(changed, {}, T(0.0));
    Error err;
    REQUIRE(ws.SaveVisibility(T(10.0), true, err));
    CHECK(fs::exists(dir / "view_config.txt.bak"));
    CHECK(ReadBytes(dir / "view_config.txt.bak").empty());
    CHECK(ReadBytes(dir / "view_config.txt").find("showScene=0") != std::string::npos);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("Workspace: unreadable view_config gets the same protection")
{
    auto dir = UniqueTempDir("badview");
    EditorWorkspaceState ws(dir, dir);
    std::error_code ec;
    fs::create_directories(dir / "view_config.txt", ec);
    const auto c = ws.ClassifyStartup();
    CHECK(c.viewConfigProtected);
    CHECK(ws.Status().visibility.state == WorkspaceFileState::SessionOnlyProtected);
    const auto loaded = ws.LoadVisibility(WorkspaceVisibility::LegacyDefaults());
    CHECK_FALSE(loaded.IsOk());
    CHECK(loaded.visibility == WorkspaceVisibility::LegacyDefaults());
    Error err;
    CHECK_FALSE(ws.SaveVisibility(T(10.0), true, err));
    CHECK_FALSE(err.IsOk());
    fs::remove_all(dir, ec);
}

// ---- Legacy defaults, unknown keys, malformed values -------------------------

TEST_CASE("Workspace: omitted keys keep legacy defaults, unknown keys retained")
{
    const auto parsed = ParseWorkspaceViewConfig(
        "showScene=0\nfutureGizmo=2\nshowScene=1\n", WorkspaceVisibility::LegacyDefaults());
    CHECK(parsed.visibility.showScene); // last wins, like the legacy loader
    CHECK(parsed.visibility.showCamera); // omitted -> legacy default
    CHECK_FALSE(parsed.visibility.showInputBindings); // omitted -> legacy false
    CHECK_FALSE(parsed.visibility.showContentBrowser);
    REQUIRE(parsed.unknownLines.size() == 1);
    CHECK(parsed.unknownLines[0] == "futureGizmo=2");
    CHECK_FALSE(parsed.hadMalformedKnownValue);
    // Round trip preserves the unknown line verbatim.
    const std::string out = SerializeWorkspaceViewConfig(parsed.visibility, parsed.unknownLines);
    CHECK(out.find("futureGizmo=2\n") != std::string::npos);
    CHECK(out.find("showInputBindings=0\n") != std::string::npos);
    CHECK(out.find("showContentBrowser=0\n") != std::string::npos);
    const auto reparsed = ParseWorkspaceViewConfig(out, WorkspaceVisibility::NewProfileDefaults());
    CHECK(reparsed.unknownLines == parsed.unknownLines);
}

TEST_CASE("Workspace: malformed known values keep base and are reported, bounds enforced")
{
    const auto parsed = ParseWorkspaceViewConfig(
        "showScene=maybe\nperfDetail=99\n", WorkspaceVisibility::LegacyDefaults());
    CHECK(parsed.hadMalformedKnownValue);
    CHECK(parsed.visibility.showScene); // legacy default kept
    CHECK(parsed.visibility.perfDetail == 0);
    const auto badInt = ParseWorkspaceViewConfig(
        "perfDetail=abc\n", WorkspaceVisibility::LegacyDefaults());
    CHECK(badInt.hadMalformedKnownValue);
    const auto ok = ParseWorkspaceViewConfig(
        "perfDetail=2\n", WorkspaceVisibility::LegacyDefaults());
    CHECK_FALSE(ok.hadMalformedKnownValue);
    CHECK(ok.visibility.perfDetail == 2);
}

TEST_CASE("Workspace: new-profile defaults match the accepted plan")
{
    const auto v = WorkspaceVisibility::NewProfileDefaults();
    CHECK(v.showOutliner);
    CHECK(v.showInspector);
    CHECK(v.showContentBrowser);
    CHECK(v.showSession);
    CHECK(v.showPerformance);
    CHECK_FALSE(v.showCamera);
    CHECK_FALSE(v.showRenderSettings);
    CHECK_FALSE(v.showInputBindings);
    // INTERIM migration policy: the accepted final default has Scene off,
    // but Scene stays on until the package-3 persistent toolbar ships so
    // transport/environment remain discoverable for new profiles.
    CHECK(v.showScene);
}

// ---- Checked persistence: backups, failures, partial retry -------------------

TEST_CASE("Workspace: first rewrite backs up original bytes")
{
    auto dir = UniqueTempDir("backup");
    EditorWorkspaceState ws(dir, dir);
    WriteBytes(dir / "view_config.txt", "showScene=0\n");
    ws.ClassifyStartup();
    const auto loaded = ws.LoadVisibility(WorkspaceVisibility::LegacyDefaults());
    REQUIRE(loaded.IsOk());
    CHECK_FALSE(loaded.visibility.showScene);
    WorkspaceVisibility changed = loaded.visibility;
    changed.showScene = true;
    ws.MarkVisibilitySnapshot(changed, loaded.unknownLines, T(0.0));
    Error err;
    REQUIRE(ws.SaveVisibility(T(10.0), true, err));
    REQUIRE(fs::exists(dir / "view_config.txt.bak"));
    CHECK(ReadBytes(dir / "view_config.txt.bak") == "showScene=0\n");
    CHECK(ReadBytes(dir / "view_config.txt").find("showScene=1") != std::string::npos);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("Workspace: write failure is loud, destination intact, failures coalesce")
{
    auto dir = UniqueTempDir("writefail_parent");
    // The app-data root is an existing FILE, so directory creation fails.
    const fs::path rootFile = dir / "rootfile";
    WriteBytes(rootFile, "x");
    EditorWorkspaceState ws(rootFile, rootFile);
    ws.ClassifyStartup();
    ws.MarkVisibilitySnapshot(WorkspaceVisibility::LegacyDefaults(), {}, T(0.0));
    Error err;
    CHECK_FALSE(ws.SaveVisibility(T(10.0), true, err));
    CHECK(err.code == Error::Io);
    CHECK_FALSE(err.path.empty());
    CHECK(ws.Status().visibility.state == WorkspaceFileState::SaveFailed);
    CHECK(ws.Status().visibility.failureCount == 1);
    // A repeated identical failure coalesces (count grows, one durable error).
    Error err2;
    CHECK_FALSE(ws.SaveVisibility(T(20.0), true, err2));
    CHECK(err2 == err);
    CHECK(ws.Status().visibility.failureCount == 2);
    // Nothing was created through the file-root.
    CHECK_FALSE(fs::exists(rootFile / "view_config.txt"));
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("Workspace: replacement failure keeps the old destination")
{
    auto dir = UniqueTempDir("replacefail");
    EditorWorkspaceState ws(dir, dir);
    // A directory at the destination path defeats atomic replacement.
    std::error_code ec;
    fs::create_directories(dir / "view_config.txt", ec);
    // Bypass the protected-classification gate the way Reset does: classify
    // first is protected, so exercise the writer directly for the
    // replacement-error row.
    Error err;
    CHECK_FALSE(WriteFileBytesCheckedAtomic(dir / "view_config.txt", "showScene=0\n", err));
    CHECK(err.code == Error::Io);
    CHECK(fs::is_directory(dir / "view_config.txt", ec)); // original intact
    CHECK_FALSE(fs::exists(dir / "view_config.txt.tmp"));
    fs::remove_all(dir, ec);
}

TEST_CASE("Workspace: partial save reports, retains backups, retries only unsaved parts")
{
    auto dir = UniqueTempDir("partial");
    EditorWorkspaceState ws(dir, dir);
    WriteBytes(dir / "imgui.ini", "[Window][A]\n");
    WriteBytes(dir / "view_config.txt", "showScene=0\n");
    ws.ClassifyStartup();
    REQUIRE(ws.LoadGeometryBytes().IsOk());
    REQUIRE(ws.LoadVisibility(WorkspaceVisibility::LegacyDefaults()).IsOk());
    // Break the visibility side only: directory defeats its replacement.
    // (Backups were taken on the first successful saves below, so the
    // partial failure below still retains them.)
    WorkspaceVisibility changed = WorkspaceVisibility::LegacyDefaults();
    changed.showScene = false;
    ws.MarkVisibilitySnapshot(changed, {}, T(0.0));
    ws.MarkGeometryDirty(T(0.0));
    Error err;
    REQUIRE(ws.SaveGeometry(T(10.0), true, "[Window][B]\n", err));
    REQUIRE(fs::exists(dir / "imgui.ini.bak"));
    CHECK(ReadBytes(dir / "imgui.ini.bak") == "[Window][A]\n");
    std::error_code ec;
    fs::remove(dir / "view_config.txt", ec);
    fs::create_directories(dir / "view_config.txt", ec);
    // Force through the protected gate the way a pre-existing protected
    // file would: classification already ran, so save and watch it fail.
    CHECK_FALSE(ws.SaveVisibility(T(10.0), true, err));
    CHECK(ws.Status().PartiallySaved());
    CHECK(std::string(ws.Status().SummaryMessage()) == "workspace changes not fully saved");
    CHECK(ReadBytes(dir / "imgui.ini") == "[Window][B]\n"); // good side kept
    // Repair access; retry saves only the still-unsaved visibility part.
    fs::remove_all(dir / "view_config.txt", ec);
    ws.MarkVisibilitySnapshot(changed, {}, T(20.0));
    REQUIRE(ws.SaveVisibility(T(30.0), true, err));
    CHECK_FALSE(ws.Status().HasUnsaved());
    CHECK(std::string(ws.Status().SummaryMessage()) == "Workspace settings saved");
    fs::remove_all(dir, ec);
}

TEST_CASE("Workspace: debounce and bounded retry delay gate automatic saves")
{
    auto dir = UniqueTempDir("debounce");
    EditorWorkspaceState ws(dir, dir);
    ws.ClassifyStartup();
    WorkspaceVisibility changed = WorkspaceVisibility::LegacyDefaults();
    changed.showScene = false;
    ws.MarkVisibilitySnapshot(changed, {}, T(100.0));
    REQUIRE(ws.VisibilityDirty());
    CHECK_FALSE(ws.VisibilitySaveDue(T(100.5))); // debounce: 1.0 s
    CHECK(ws.VisibilitySaveDue(T(101.5)));
    // After a failure, automatic saves wait out the retry delay.
    const fs::path rootFile = dir / "rootfile2";
    WriteBytes(rootFile, "x");
    EditorWorkspaceState bad(rootFile, rootFile);
    bad.ClassifyStartup();
    WorkspaceVisibility badChanged = WorkspaceVisibility::LegacyDefaults();
    badChanged.showSession = false;
    bad.MarkVisibilitySnapshot(badChanged, {}, T(0.0));
    REQUIRE(bad.VisibilityDirty());
    Error err;
    CHECK_FALSE(bad.SaveVisibility(T(10.0), true, err)); // forced: fails now
    CHECK_FALSE(bad.VisibilitySaveDue(T(11.0))); // delay: 5.0 s
    CHECK(bad.VisibilitySaveDue(T(16.0))); // due again, still failing loudly
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ---- Headless, Reset, backup recovery ----------------------------------------

TEST_CASE("Workspace: headless operation never rewrites preferences")
{
    auto dir = UniqueTempDir("headless");
    EditorWorkspaceState ws(dir, dir);
    ws.SetHeadless(true);
    ws.ClassifyStartup();
    ws.MarkVisibilitySnapshot(WorkspaceVisibility::LegacyDefaults(), {}, T(0.0));
    ws.MarkGeometryDirty(T(0.0));
    Error err;
    REQUIRE(ws.SaveVisibility(T(10.0), true, err));
    REQUIRE(ws.SaveGeometry(T(10.0), true, "[Window][X]\n", err));
    REQUIRE(ws.Flush(T(10.0), WorkspaceVisibility::LegacyDefaults(), {},
        static_cast<const std::string*>(nullptr), err));
    CHECK_FALSE(fs::exists(dir / "view_config.txt"));
    CHECK_FALSE(fs::exists(dir / "imgui.ini"));
    CHECK(std::string(ws.Status().SummaryMessage()) ==
        "Workspace persistence disabled (headless)");
    // Reset is refused headless: no destructive path without a UI to recover.
    CHECK_FALSE(ws.ResetToDefaults(T(10.0), "[Window][SessionBaseline]\n", err));
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("Workspace: Reset backs up, installs defaults, force-saves both, spares the seed")
{
    auto dir = UniqueTempDir("reset");
    auto exe = UniqueTempDir("reset_exe");
    const std::string userIni = "[Window][Custom]\n";
    const std::string viewCfg = "showScene=0\nfutureFlag=9\n";
    const std::string seed = "[Window][Seed]\n";
    WriteBytes(dir / "imgui.ini", userIni);
    WriteBytes(dir / "view_config.txt", viewCfg);
    WriteBytes(exe / "imgui.ini", seed);
    EditorWorkspaceState ws(dir, exe);
    ws.ClassifyStartup();
    Error err;
    REQUIRE(ws.CanReset(err));
    REQUIRE(ws.ResetToDefaults(T(10.0), "[Window][SessionBaseline]\n", err));
    // Backups hold the original bytes.
    REQUIRE(fs::exists(dir / "imgui.ini.bak"));
    REQUIRE(fs::exists(dir / "view_config.txt.bak"));
    CHECK(ReadBytes(dir / "imgui.ini.bak") == userIni);
    CHECK(ReadBytes(dir / "view_config.txt.bak") == viewCfg);
    // Visibility is new-profile defaults with unknown lines retained.
    const std::string savedView = ReadBytes(dir / "view_config.txt");
    CHECK(savedView.find("showContentBrowser=1") != std::string::npos);
    CHECK(savedView.find("showScene=1") != std::string::npos); // interim default
    CHECK(savedView.find("futureFlag=9") != std::string::npos);
    // Geometry file agrees with the session baseline supplied by the host
    // (1.87 has no public call to drop loaded settings, so the file is
    // never cleared behind ImGui's back); the seed never written.
    CHECK(ReadBytes(dir / "imgui.ini") == "[Window][SessionBaseline]\n");
    CHECK(ReadBytes(exe / "imgui.ini") == seed);
    CHECK_FALSE(ws.Status().HasUnsaved());
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::remove_all(exe, ec);
}

TEST_CASE("Workspace: Reset on an empty user ini backs up the empty file and proceeds")
{
    auto dir = UniqueTempDir("resetempty");
    EditorWorkspaceState ws(dir, dir);
    WriteBytes(dir / "imgui.ini", "");
    ws.ClassifyStartup();
    REQUIRE(ws.Classification().userIniProtected);
    Error err;
    REQUIRE(ws.CanReset(err)); // readable empties are recoverable
    REQUIRE(ws.ResetToDefaults(T(10.0), "[Window][SessionBaseline]\n", err));
    CHECK(fs::exists(dir / "imgui.ini.bak"));
    CHECK(ReadBytes(dir / "imgui.ini.bak").empty());
    // Protection cleared: normal saves proceed after Reset.
    ws.MarkGeometryDirty(T(20.0));
    REQUIRE(ws.SaveGeometry(T(30.0), true, "[Window][New]\n", err));
    CHECK(ReadBytes(dir / "imgui.ini") == "[Window][New]\n");
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("Workspace: Reset Window Visibility restores defaults, spares geometry")
{
    auto dir = UniqueTempDir("visreset");
    const std::string userIni = "[Window][Custom]\n";
    WriteBytes(dir / "imgui.ini", userIni);
    WriteBytes(dir / "view_config.txt", "showScene=0\nshowSession=0\nfutureFlag=9\n");
    EditorWorkspaceState ws(dir, dir);
    ws.ClassifyStartup();
    Error err;
    REQUIRE(ws.CanResetVisibility(err));
    REQUIRE(ws.ResetVisibilityToDefaults(T(10.0), err));
    // Backup holds the original visibility bytes; geometry file untouched
    // and no geometry backup claimed.
    REQUIRE(fs::exists(dir / "view_config.txt.bak"));
    CHECK(ReadBytes(dir / "view_config.txt.bak") ==
        "showScene=0\nshowSession=0\nfutureFlag=9\n");
    CHECK(ReadBytes(dir / "imgui.ini") == userIni);
    CHECK_FALSE(fs::exists(dir / "imgui.ini.bak"));
    // Defaults installed (interim: Scene on), unknown lines retained.
    const std::string savedView = ReadBytes(dir / "view_config.txt");
    CHECK(savedView.find("showScene=1") != std::string::npos);
    CHECK(savedView.find("showSession=1") != std::string::npos);
    CHECK(savedView.find("showContentBrowser=1") != std::string::npos);
    CHECK(savedView.find("futureFlag=9") != std::string::npos);
    CHECK_FALSE(ws.Status().HasUnsaved());
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("Workspace: Reset Window Visibility never clears geometry protection")
{
    auto dir = UniqueTempDir("visresetprot");
    std::error_code ec;
    fs::create_directories(dir / "imgui.ini", ec); // unreadable geometry
    WriteBytes(dir / "view_config.txt", "showScene=0\n");
    EditorWorkspaceState ws(dir, dir);
    ws.ClassifyStartup();
    REQUIRE(ws.Classification().userIniProtected);
    Error err;
    REQUIRE(ws.ResetVisibilityToDefaults(T(10.0), err));
    // Geometry still protected: its save stays refused after a
    // visibility-only reset, and the full Reset stays unavailable.
    CHECK(ws.Classification().userIniProtected);
    CHECK_FALSE(ws.SaveGeometry(T(20.0), true, "[Window][X]\n", err));
    CHECK_FALSE(ws.CanReset(err));
    fs::remove_all(dir, ec);
}

TEST_CASE("Workspace: Reset Window Visibility blocked on unreadable view_config")
{
    auto dir = UniqueTempDir("visresetbad");
    EditorWorkspaceState ws(dir, dir);
    std::error_code ec;
    fs::create_directories(dir / "view_config.txt", ec);
    ws.ClassifyStartup();
    Error err;
    CHECK_FALSE(ws.CanResetVisibility(err));
    CHECK_FALSE(err.IsOk());
    CHECK_FALSE(ws.ResetVisibilityToDefaults(T(10.0), err));
    fs::remove_all(dir, ec);
}

TEST_CASE("Workspace: paired backups restore through checked replacement")
{
    auto dir = UniqueTempDir("restore");
    EditorWorkspaceState ws(dir, dir);
    WriteBytes(dir / "imgui.ini", "[Window][Good]\n");
    WriteBytes(dir / "view_config.txt", "showScene=1\n");
    ws.ClassifyStartup();
    REQUIRE(ws.LoadGeometryBytes().IsOk());
    REQUIRE(ws.LoadVisibility(WorkspaceVisibility::LegacyDefaults()).IsOk());
    ws.MarkGeometryDirty(T(0.0));
    WorkspaceVisibility changed = WorkspaceVisibility::LegacyDefaults();
    changed.showScene = false;
    ws.MarkVisibilitySnapshot(changed, {}, T(0.0));
    Error err;
    REQUIRE(ws.SaveGeometry(T(10.0), true, "[Window][Changed]\n", err));
    REQUIRE(ws.SaveVisibility(T(10.0), true, err));
    // Corrupt both, then recover from the paired backups.
    WriteBytes(dir / "imgui.ini", "garbage");
    WriteBytes(dir / "view_config.txt", "garbage");
    REQUIRE(ws.RestoreBackupGeometry(err));
    REQUIRE(ws.RestoreBackupVisibility(err));
    CHECK(ReadBytes(dir / "imgui.ini") == "[Window][Good]\n");
    CHECK(ReadBytes(dir / "view_config.txt") == "showScene=1\n");
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("Workspace: portable seed saves go to the user file, never the seed")
{
    auto dir = UniqueTempDir("seedwrite");
    auto exe = UniqueTempDir("seedwrite_exe");
    WriteBytes(exe / "imgui.ini", "[Window][Seed]\n");
    EditorWorkspaceState ws(dir, exe);
    REQUIRE(ws.ClassifyStartup().geometrySource == WorkspaceGeometrySource::PortableSeed);
    const auto load = ws.LoadGeometryBytes();
    REQUIRE(load.IsOk());
    CHECK(load.bytes == "[Window][Seed]\n");
    ws.MarkGeometryDirty(T(0.0));
    Error err;
    REQUIRE(ws.SaveGeometry(T(10.0), true, "[Window][Customized]\n", err));
    CHECK(ReadBytes(dir / "imgui.ini") == "[Window][Customized]\n");
    CHECK(ReadBytes(exe / "imgui.ini") == "[Window][Seed]\n");
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::remove_all(exe, ec);
}

TEST_CASE("Workspace: Retry Load replaces protected state after access is repaired")
{
    auto dir = UniqueTempDir("retryload");
    EditorWorkspaceState ws(dir, dir);
    std::error_code ec;
    fs::create_directories(dir / "imgui.ini", ec); // unreadable at first
    ws.ClassifyStartup();
    REQUIRE(ws.Classification().userIniProtected);
    // Repair: replace the directory with a real file, then retry.
    fs::remove_all(dir / "imgui.ini", ec);
    WriteBytes(dir / "imgui.ini", "[Window][Repaired]\n");
    Error err;
    REQUIRE(ws.ReloadAll(T(10.0), err));
    CHECK_FALSE(ws.Classification().userIniProtected);
    CHECK(ws.LoadGeometryBytes().bytes == "[Window][Repaired]\n");
    fs::remove_all(dir, ec);
}
