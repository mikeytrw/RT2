#include <doctest/doctest.h>

#include "AssetReference.h"
#include "AssetIdentity.h"
#include "AssetWatchPolicy.h"
#include "ContentBrowserOperations.h"
#include "PrefabSerializer.h"
#include "core/Error.h"
#include "core/UUID.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

using namespace rt2::core;

// ============================================================================
// Phase 8 W0 — prefab asset kind and file envelope (implementation spec,
// docs/game-engine-development-plan.md "Phase 8 — Prefabs", W0).
//
// W0 delivers the AssetKind::Prefab arm through every findings-Q5 site (enum
// arm, name codec both ways, reader acceptance, watcher extension list,
// sidecar minting via ResolveOrAssign, content-browser reimport policy) plus
// the .rt2prefab file envelope with its own version constant and an empty
// record list that round-trips. Subtree capture, entity records, templateId
// minting, and instantiation are W1 and deliberately absent.
//
// Each test carries a discrimination proof (fault, confirm red, revert,
// confirm green) recorded in the verification report.
// ============================================================================

namespace
{

std::filesystem::path UniqueTempDir(const std::string& tag)
{
	auto dir = std::filesystem::temp_directory_path() / ("rt2_" + tag);
	std::error_code ec;
	std::filesystem::remove_all(dir, ec);
	std::filesystem::create_directories(dir, ec);
	return dir;
}

std::string ReadFile(const std::filesystem::path& p)
{
	std::ifstream in(p, std::ios::binary);
	std::stringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

void WriteRaw(const std::filesystem::path& p, const std::string& content)
{
	std::ofstream out(p, std::ios::binary | std::ios::trunc);
	out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

} // namespace

// ---------------------------------------------------------------------------
// C1: the kind name codec round-trips Prefab in both directions and the
// genuine-unknown rejection path stays intact. Fault for red: drop the Prefab
// arm from AssetKindFromName (or AssetKindName).
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W0: AssetKind name codec round-trips Prefab and rejects unknowns")
{
	CHECK(AssetKindName(AssetKind::Prefab) == std::string("prefab"));
	CHECK(AssetKindFromName("prefab") == AssetKind::Prefab);
	CHECK(std::string(AssetKindName(AssetKindFromName("prefab"))) == "prefab");

	// Every existing kind still round-trips both ways.
	const AssetKind all[] = { AssetKind::Model, AssetKind::Texture,
	                          AssetKind::Environment, AssetKind::Script };
	for (AssetKind k : all)
	{
		const std::string name = AssetKindName(k);
		CHECK(AssetKindFromName(name) == k);
	}

	// The genuinely-unknown rejection path stays intact: an unknown name maps
	// to Unknown (the reader then hard-fails on an unknown kind with a
	// non-empty path), and an unknown kind maps to "unknown".
	CHECK(AssetKindFromName("definitely-not-an-asset-kind") == AssetKind::Unknown);
	CHECK(std::string(AssetKindName(static_cast<AssetKind>(255))) == "unknown");
}

// ---------------------------------------------------------------------------
// C2: a structurally valid but empty .rt2prefab (header, version, empty record
// list) round-trips and re-saves byte-identically. Fault for red: omit the
// "entities" array in Save.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W0: empty rt2prefab round-trips structurally equal")
{
	auto dir = UniqueTempDir("p8w0_roundtrip");
	auto path = dir / "empty.rt2prefab";
	auto path2 = dir / "empty2.rt2prefab";

	PrefabDocument in;
	REQUIRE(in.entities.empty());
	CHECK(in.version == PrefabSerializer::FormatVersion);

	Error saveErr;
	REQUIRE(PrefabSerializer::Save(in, path, saveErr));
	REQUIRE(saveErr.IsOk());

	PrefabDocument out;
	Error loadErr;
	REQUIRE(PrefabSerializer::Load(out, path, loadErr));
	REQUIRE(loadErr.IsOk());
	CHECK(out.version == in.version);
	CHECK(out.entities.empty());

	// Deterministic output: re-saving the loaded document is byte-identical.
	Error saveErr2;
	REQUIRE(PrefabSerializer::Save(out, path2, saveErr2));
	REQUIRE(saveErr2.IsOk());
	CHECK(ReadFile(path) == ReadFile(path2));

	std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// C3: a .rt2prefab gets a sidecar identity on first import and that identity
// is stable across a second import (the assign-once property of
// ResolveOrAssign). Fault for red: remove the reuse-existing-sidecar early
// return in AssetIdentity.cpp so a second call re-mints a fresh ID.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W0: rt2prefab sidecar identity is assigned once and stable")
{
	auto dir = UniqueTempDir("p8w0_sidecar");
	const auto prefab = dir / "crate.rt2prefab";
	{ std::ofstream out(prefab); out << "{}"; } // asset need not be real

	DeterministicUuidProvider provider;
	Error err1, err2;
	bool minted1 = false, minted2 = false;
	const UUID id1 = ResolveOrAssign(prefab, provider, minted1, err1);
	REQUIRE(err1.IsOk());
	CHECK_FALSE(id1.IsNull());
	CHECK(minted1 == true);
	CHECK(ReadSidecarId(AssetSidecarPath(prefab), err1) == id1);

	// Second import reuses the committed sidecar; it must not re-mint.
	const UUID id2 = ResolveOrAssign(prefab, provider, minted2, err2);
	REQUIRE(err2.IsOk());
	CHECK(id2 == id1);
	CHECK(minted2 == false);

	std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// C4: the watcher classifies .rt2prefab (and its sidecar) as a database
// refresh, without disturbing the existing classifications. Fault for red:
// remove .rt2prefab from the AssetWatchPolicy extension list.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W0: watcher classifies .rt2prefab as a database refresh")
{
	using std::filesystem::path;
	CHECK(ClassifyAssetFileEvent(path("C:/p/crate.rt2prefab"), AssetFileAction::Add)
		== AssetFileEventKind::DatabaseRefresh);
	CHECK(AssetFileNeedsDatabaseRefresh(path("C:/p/crate.rt2prefab"), AssetFileAction::Modified));
	// The prefab's own sidecar also triggers a database refresh.
	CHECK(ClassifyAssetFileEvent(path("C:/p/crate.rt2prefab.rt2meta"), AssetFileAction::Modified)
		== AssetFileEventKind::DatabaseRefresh);

	// Regression: existing classifications are unchanged.
	CHECK(ClassifyAssetFileEvent(path("C:/p/a.glb"), AssetFileAction::Modified)
		== AssetFileEventKind::DatabaseRefresh);
	CHECK(ClassifyAssetFileEvent(path("C:/p/b.lua"), AssetFileAction::Modified)
		== AssetFileEventKind::ScriptReload);
	CHECK(ClassifyAssetFileEvent(path("C:/p/c.txt"), AssetFileAction::Modified)
		== AssetFileEventKind::Ignore);
}

// ---------------------------------------------------------------------------
// C5: a prefab version mismatch is rejected with a clear SchemaVersion
// diagnostic (never silently accepted), and a wrong header is a Parse error.
// Fault for red: remove the version-range check in PrefabSerializer::Load.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W0: prefab version mismatch is rejected with a clear diagnostic")
{
	auto dir = UniqueTempDir("p8w0_version");
	auto badVersion = dir / "future.rt2prefab";
	WriteRaw(badVersion,
		"{\n\"header\": \"rt2prefab\",\n\"version\": 99,\n\"entities\": []\n}\n");

	PrefabDocument out;
	Error err;
	CHECK_FALSE(PrefabSerializer::Load(out, badVersion, err));
	CHECK(err.code == Error::SchemaVersion);
	CHECK(err.detail.find("99") != std::string::npos);       // names the bad version
	CHECK(err.detail.find("supported") != std::string::npos); // and the supported one

	// A wrong/absent header is a hard parse error, not a silent accept.
	auto wrongHeader = dir / "wrong.rt2prefab";
	WriteRaw(wrongHeader,
		"{\n\"header\": \"rt2scene\",\n\"version\": 1,\n\"entities\": []\n}\n");
	Error hdrErr;
	CHECK_FALSE(PrefabSerializer::Load(out, wrongHeader, hdrErr));
	CHECK(hdrErr.code == Error::Parse);

	std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// C6: the content-browser reimport policy names prefabs explicitly (a clear
// "not implemented in W0" diagnostic) rather than funneling them through the
// generic model-only message. Fault for red: drop the .rt2prefab branch in
// ContentBrowserOperations.cpp — the generic message does not mention prefab.
// ---------------------------------------------------------------------------
TEST_CASE("Phase 8 W0: reimport rejects .rt2prefab with a prefab-specific diagnostic")
{
	auto dir = UniqueTempDir("p8w0_reimport");
	// UniqueTempDir returns an absolute path; ValidateAssetPair requires that.
	REQUIRE(dir.is_absolute());

	const auto source = dir / "crate.rt2prefab";
	{ std::ofstream out(source); out << "{}"; }
	const UUID id = UUID::Parse("11111111-2222-4333-8444-555555555555");
	Error sidecarErr;
	REQUIRE(WriteSidecarId(AssetSidecarPath(source), id, sidecarErr));

	AssetRecord record;
	record.assetId = id;
	record.sourcePath = "crate.rt2prefab";
	record.observedKinds = { AssetKind::Prefab };

	ContentBrowserReimportCallback callback =
		[](const AssetRecord&, const std::filesystem::path&,
		   std::vector<AssetDiagnostic>&, Error&) -> bool { return true; };
	ContentBrowserOperationReport report;
	Error error;
	CHECK_FALSE(ReimportContentBrowserAsset(dir, record, callback, report, error));
	CHECK(error.code == Error::InvalidArgument);
	CHECK(error.detail.find("prefab") != std::string::npos);

	std::filesystem::remove_all(dir);
}
