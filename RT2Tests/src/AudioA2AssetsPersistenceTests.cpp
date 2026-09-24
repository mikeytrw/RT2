// ============================================================================
// A2 - Audio assets and persistence.
//
// Authored audio sources without owning a decoder or device handle in scene
// data. CPU-only: no miniaudio, no device/backend, no Lua, no UI preview,
// no pinball content. Decoding/playback arrive in A3/A4 and must reuse the
// definitions pinned here verbatim.
//
// Boundaries pinned by this file (acceptance in the A2 ticket):
// - Persisted coverage: 18 components incl AudioSourceComponent; the kind
//   codec round-trips "audioclip"; the visitor sees clip refs unconditionally
//   (bad inactive refs fail Save rather than disappearing).
// - v9 round-trip/migration: named save/load exact; v3-v8 migrate by
//   absence/default; strict v9 invalid inputs fail with entity UUID and
//   dotted field path (wrong kind, nil identity, bad bus/spatial combos,
//   non-finite/range values, missing Transform).
// - Decoded-channel seam: channel-count/mono validation needs the A4
//   decoder, so persistence exposes the seam and documents the deferral —
//   it never imports miniaudio into CPU targets and never pretends the
//   check executes without decoder output.
// - Provider: app-owned AudioClipAssetProvider returns canonical path,
//   effective identity, fingerprint, and immutable source bytes from an
//   OWNED database snapshot taken at refresh (no dangling project pointer);
//   same-size/same-mtime rewrites yield new fingerprints while old resolved
//   bytes stay immutable; alias spellings share one entry and report one
//   canonical identity.
// - Content Browser first import: audio drops dispatch to the host
//   importAudioClip callback, which performs sidecar first assignment
//   (ResolveOrAssign, no decode); repeat drops reuse the minted identity.
// - Routes: clone, duplicate, copy/paste, recovery, prefab (audioSource key,
//   non-overridable presence/fields) carry the source exactly.
// - Dependency protection: audio dependants resolve by ID and path fallback.
// - CPU isolation: the hard #error guard fails the build if miniaudio.h ever
//   becomes reachable; tracked premake lists carry the new translation unit.
//
// Discrimination: removing AudioSourceComponent from PersistedComponents,
// removing its visitor entry, or deleting any one manual codec path turns
// named cases below red.
// ============================================================================

#if __has_include("miniaudio.h")
#error "A2 boundary: RT2Tests must not import miniaudio (decoded-channel checks are deferred to A4)"
#endif

#include <doctest/doctest.h>

#include "AssetDatabase.h"
#include "AssetIdentity.h"
#include "AssetResolver.h"
#include "AssetWatchPolicy.h"
#include "AudioClipAssetProvider.h"
#include "AudioComponents.h"
#include "ContentBrowserOperations.h"
#include "ECSComponents.h"
#include "PersistedComponents.h"
#include "PhysicsCollisionGeometry.h"
#include "PrefabComponentKey.h"
#include "PrefabComponentValueEquality.h"
#include "PrefabPropagationComponentAdapter.h"
#include "PrefabSerializer.h"
#include "ProjectAssetScanner.h"
#include "SceneAssetReferenceVisitor.h"
#include "SceneDocument.h"
#include "SceneManager.h"
#include "SceneSerializer.h"
#include "SceneSerializerTestSupport.h"
#include "core/Error.h"
#include "core/UUID.h"
#include "json.hpp"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <typeindex>
#include <vector>

using namespace rt2::core;
using json = nlohmann::json;

namespace
{

const UUID kClipId = UUID::Parse("550e8400-e29b-41d4-a716-446655440201");
const UUID kOtherClipId = UUID::Parse("550e8400-e29b-41d4-a716-446655440205");
const UUID kEntityId = UUID::Parse("550e8400-e29b-41d4-a716-446655440202");

std::filesystem::path UniqueTempDir(const std::string& tag)
{
    auto dir = std::filesystem::temp_directory_path() / ("rt2_a2_" + tag);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void WriteFileBinary(const std::filesystem::path& path,
                     const std::string& content)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

void WriteFileBytes(const std::filesystem::path& path,
                    const std::vector<char>& content)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string ReadFileBinary(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct AudioFixture
{
    DeterministicUuidProvider ids;
    SceneManager manager;

    AudioFixture()
    {
        manager.SetUuidProvider(&ids);
    }

    UUID CreateEmpty(const char* name)
    {
        return manager.CreateEmpty(name).affectedEntities.front();
    }

    entt::entity Handle(const UUID& uuid) const
    {
        return manager.FindEntityByUuid(uuid);
    }
};

// Non-default authored source exercising every persisted field: music bus,
// non-spatial (so no Transform coupling), looped autoplay, custom
// attenuation, and a non-default priority.
AudioSourceComponent MakeSource(const UUID& clipId)
{
    AudioSourceComponent source;
    source.clip.kind = AssetKind::AudioClip;
    source.clip.path = "sfx/hit.wav";
    source.clip.assetId = clipId;
    source.bus = AudioBus::Music;
    source.autoplay = true;
    source.loop = true;
    source.spatial = false;
    source.gain = 0.8f;
    source.pitch = 1.05f;
    source.minDistance = 2.0f;
    source.maxDistance = 40.0f;
    source.rolloff = 2.0f;
    source.priority = 200;
    return source;
}

// Default spatial source (Transform-coupled, effects bus).
AudioSourceComponent MakeSpatialSource(const UUID& clipId)
{
    AudioSourceComponent source;
    source.clip.kind = AssetKind::AudioClip;
    source.clip.path = "sfx/loop.flac";
    source.clip.assetId = clipId;
    return source;
}

const AudioSourceComponent* SourceOf(SceneManager& manager, const UUID& uuid)
{
    return manager.GetECS().registry.try_get<AudioSourceComponent>(
        manager.FindEntityByUuid(uuid));
}

bool HasAudioSlot(const std::vector<SceneAssetReferenceSlot>& slots,
                  const std::string& path)
{
    for (const auto& slot : slots)
    {
        if (slot.reference != nullptr && slot.reference->path == path)
            return true;
    }
    return false;
}

AssetRecord Record(const std::string& path, const UUID& id)
{
    AssetRecord record;
    record.assetId = id;
    record.sourcePath = path;
    record.identityAuthority = AssetIdentityAuthority::Sidecar;
    return record;
}

std::string ReadRepoFile(const char* relativePath, bool& found)
{
    // RT2Tests runs from the repository root (AGENTS.md).
    std::ifstream in(relativePath, std::ios::binary);
    found = in.good();
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Minimal v9 entity carrying one raw audio fragment. The fragment is spliced
// verbatim so malformed shapes reach the codec exactly as a hostile or
// hand-edited file would carry them.
std::string MalformedAudioDoc(const std::string& uuid,
                              const std::string& audioFragment,
                              bool withTransform = true)
{
    std::string transform;
    if (withTransform)
        transform = R"("transform": {"translation": [0,0,0], "rotation": [0,0,0,1], "scale": [1,1,1]},)";
    return std::string(R"({
  "version": 9,
  "metadata": {"name": "malformed-audio"},
  "entities": [{
    "uuid": ")") + uuid + R"(",
    "name": "Suspicious",
    "parent": "",
    "visible": true,
    )" + transform + R"(
    )" + audioFragment + R"(
  }],
  "materials": [], "textures": [],
  "camera": {"position": [0,0,0], "forward": [0,0,-1], "fov": 45},
  "envMap": {"kind": "unknown", "path": "", "sourceKey": ""}
})";
}

void ExpectAudioParseFail(const std::string& tag,
                          const std::string& audioFragment,
                          const std::string& dottedField,
                          bool withTransform = true)
{
    const std::string uuid = "11111111-1111-4111-8111-111111111111";
    const auto dir = UniqueTempDir("a2_malformed_" + tag);
    const auto path = dir / "malformed.rt2scene";
    WriteFileBinary(path,
                    MalformedAudioDoc(uuid, audioFragment, withTransform));
    SceneDocument loaded;
    Error err;
    CHECK_MESSAGE(SceneSerializer::Load(loaded, path, err) == false, tag);
    CHECK_MESSAGE(err.code == Error::Parse, tag);
    // Load reports the file path in err.path on record failure (established
    // convention); the entity UUID and dotted field travel in err.detail,
    // which is the durable carrier the ticket requires.
    CHECK_MESSAGE(err.detail.find(uuid) != std::string::npos, tag);
    CHECK_MESSAGE(err.detail.find(dottedField) != std::string::npos,
                  tag << " expected dotted field " << dottedField
                      << " in: " << err.detail);
    std::filesystem::remove_all(dir);
}

std::string ClipJson(const std::string& kind, const std::string& path,
                     const std::string& assetId)
{
    return std::string(R"("clip": {"kind": ")") + kind +
           R"(", "path": ")" + path + R"(", "sourceKey": "", "assetId": ")" +
           assetId + R"("})";
}

} // namespace

TEST_CASE("A2_PersistedCoverage18: generic list visits AudioSourceComponent")
{
    // Tripwire for required check 1. Removing AudioSourceComponent from
    // PersistedComponents::ForEach turns this red while Count still reads 18.
    CHECK(PersistedComponents::Count == 18);

    std::set<std::type_index> seen;
    size_t visits = 0;
    PersistedComponents::ForEach(
        [&](auto tag)
        {
            ++visits;
            seen.insert(std::type_index(typeid(typename decltype(tag)::Type)));
        });
    CHECK(visits == 18);
    CHECK(seen.size() == 18);
    CHECK(seen.count(std::type_index(typeid(AudioSourceComponent))) == 1);
}

TEST_CASE("A2_AssetKindCodecAudioClip: audioclip wire round-trips")
{
    // Removing the AudioClip codec entry turns this red; an audio-shaped
    // path can never smuggle itself through as Unknown.
    CHECK(std::string(AssetKindName(AssetKind::AudioClip)) == "audioclip");
    CHECK(AssetKindFromName("audioclip") == AssetKind::AudioClip);
    CHECK(AssetKindFromName("audio") == AssetKind::Unknown);
    CHECK(AssetKindFromName("wav") == AssetKind::Unknown);

    AssetReference clip;
    clip.kind = AssetKind::AudioClip;
    clip.path = "sfx/hit.wav";
    CHECK(clip.IsValid());
}

TEST_CASE("A2_ClipExtensionClassification: WAV/FLAC/MP3 only, case-insensitive")
{
    CHECK(IsAudioClipPath("sfx/hit.wav"));
    CHECK(IsAudioClipPath("sfx/hit.flac"));
    CHECK(IsAudioClipPath("sfx/hit.mp3"));
    CHECK(IsAudioClipPath("sfx/HIT.WAV"));
    CHECK(IsAudioClipPath("sfx/Hit.FlAc"));
    CHECK(IsAudioClipPath("sfx/Hit.MP3"));

    // Ogg/Vorbis and Opus are explicitly out of scope for the first delivery.
    CHECK_FALSE(IsAudioClipPath("sfx/hit.ogg"));
    CHECK_FALSE(IsAudioClipPath("sfx/hit.opus"));
    CHECK_FALSE(IsAudioClipPath("sfx/hit.txt"));
    CHECK_FALSE(IsAudioClipPath("sfx/hit"));
    CHECK_FALSE(IsAudioClipPath("sfx.wav/hit"));
    CHECK_FALSE(IsAudioClipPath(""));

    AudioBus bus = AudioBus::Effects;
    CHECK(AudioBusFromName("master", bus));
    CHECK(bus == AudioBus::Master);
    CHECK(AudioBusFromName("music", bus));
    CHECK(bus == AudioBus::Music);
    CHECK(AudioBusFromName("effects", bus));
    CHECK(bus == AudioBus::Effects);
    CHECK(AudioBusFromName("ui", bus));
    CHECK(bus == AudioBus::UI);
    CHECK_FALSE(AudioBusFromName("Mono", bus));
    CHECK_FALSE(AudioBusFromName("", bus));
    CHECK_FALSE(AudioBusFromName("unknown", bus));
    CHECK(std::string(AudioBusName(AudioBus::Music)) == "music");

    // An out-of-range enum value is rejected by the shared validator with
    // the "bus" dotted field — otherwise Save would serialize AudioBusName's
    // "unknown" fallback and write a scene Load rejects.
    std::string busDetail;
    std::string busField;
    AudioSourceComponent garbageBus = MakeSource(kClipId);
    garbageBus.bus = static_cast<AudioBus>(255);
    CHECK_FALSE(ValidateAudioSourceComponent(garbageBus, true, std::nullopt,
                                             busDetail, &busField));
    CHECK(busField == "bus");
}

TEST_CASE("A2_WatchPolicyAudioRefresh: clip extensions invalidate the database")
{
    // Removing the audio arms from AssetWatchPolicy turns this red: clip
    // edits would silently keep a stale provider entry.
    using Action = AssetFileAction;
    for (const char* name : {"sfx/hit.wav", "sfx/hit.flac", "sfx/hit.mp3",
                             "sfx/HIT.WAV", "sfx/Hit.FlAc"})
    {
        const auto path = std::filesystem::path("C:/proj/Assets") / name;
        CHECK(ClassifyAssetFileEvent(path, Action::Modified) ==
              AssetFileEventKind::DatabaseRefresh);
        CHECK(AssetFileNeedsDatabaseRefresh(path, Action::Modified));
        CHECK(AssetFileNeedsDatabaseRefresh(path, Action::Add));
    }
    const auto ogg = std::filesystem::path("C:/proj/Assets/sfx/hit.ogg");
    CHECK(ClassifyAssetFileEvent(ogg, Action::Modified) ==
          AssetFileEventKind::Ignore);
    CHECK_FALSE(AssetFileNeedsDatabaseRefresh(ogg, Action::Modified));
}

TEST_CASE("A2_VisitorSeesAudioClipUnconditionally: malformed and unbound refs included")
{
    // Discrimination: removing the audio visitor entry lets the malformed
    // ref below through Save; this census turns red first.
    DeterministicUuidProvider provider;
    SceneDocument doc;
    doc.SetUuidProvider(&provider);

    entt::entity bound = doc.ecs.registry.create();
    doc.ecs.registry.emplace<NameComponent>(bound, "Bound");
    doc.AssignNewUuid(bound);
    AudioSourceComponent audio;
    audio.clip.kind = AssetKind::AudioClip;
    audio.clip.path = "sfx/hit.wav";
    doc.ecs.registry.emplace<AudioSourceComponent>(bound, audio);

    entt::entity malformed = doc.ecs.registry.create();
    doc.ecs.registry.emplace<NameComponent>(malformed, "Malformed");
    doc.AssignNewUuid(malformed);
    AudioSourceComponent bad;
    bad.clip.path = "sfx/stale.wav"; // Unknown kind + non-empty path
    doc.ecs.registry.emplace<AudioSourceComponent>(malformed, bad);

    entt::entity unbound = doc.ecs.registry.create();
    doc.ecs.registry.emplace<NameComponent>(unbound, "Unbound");
    doc.AssignNewUuid(unbound);
    AudioSourceComponent empty;
    doc.ecs.registry.emplace<AudioSourceComponent>(unbound, empty);

    const std::vector<SceneAssetReferenceSlot> slots =
        CollectSceneAssetReferences(doc);
    REQUIRE(slots.size() == 3);
    CHECK(HasAudioSlot(slots, "sfx/hit.wav"));
    CHECK(HasAudioSlot(slots, "sfx/stale.wav"));
}

TEST_CASE("A2_SaveRefusesBadInactiveClipRef: malformed ref fails instead of vanishing")
{
    // The visitor sees the malformed inactive reference (previous case), so
    // Save validation refuses it loudly. If the visitor entry were removed,
    // Save would succeed on a file Load refuses — this CHECK_FALSE is the
    // discriminator for required check 2.
    AudioFixture f;
    const auto emitter = f.CreateEmpty("Emitter");
    auto& registry = f.manager.GetECS().registry;
    AudioSourceComponent bad;
    bad.clip.path = "sfx/stale.wav"; // Unknown kind: Save must refuse
    bad.spatial = false;
    registry.emplace_or_replace<AudioSourceComponent>(f.Handle(emitter), bad);

    const auto dir = UniqueTempDir("a2_bad_ref");
    const auto path = dir / "bad.rt2scene";
    Error err;
    std::vector<AssetDiagnostic> diagnostics;
    CHECK_FALSE(
        SceneSerializer::Save(f.manager.AuthoringDoc(), path, diagnostics, err));
    CHECK_FALSE(err.IsOk());
    std::filesystem::remove_all(dir);
}

TEST_CASE("A2_V9RoundTripExact: named source survives save and load")
{
    AudioFixture f;
    const auto emitter = f.CreateEmpty("Emitter");
    auto& registry = f.manager.GetECS().registry;
    registry.emplace_or_replace<AudioSourceComponent>(
        f.Handle(emitter), MakeSource(kClipId));

    const auto dir = UniqueTempDir("a2_v9_roundtrip");
    const auto path = dir / "audio.rt2scene";
    Error err;
    REQUIRE(SaveSceneForTest(f.manager.AuthoringDoc(), path, err));

    const json saved = json::parse(ReadFileBinary(path));
    REQUIRE(saved.contains("version"));
    CHECK(saved["version"].get<uint32_t>() == 9u);
    CHECK(saved["version"].get<uint32_t>() == SceneSerializer::SchemaVersion);

    SceneDocument loaded;
    REQUIRE(SceneSerializer::Load(loaded, path, err));
    CHECK(loaded.metadata.schemaVersion == 9u);

    const auto e = loaded.FindByUuid(emitter);
    REQUIRE(static_cast<uint32_t>(e) != static_cast<uint32_t>(entt::null));
    const auto* source =
        loaded.ecs.registry.try_get<AudioSourceComponent>(e);
    REQUIRE(source != nullptr);
    CHECK(*source == MakeSource(kClipId));
    CHECK(PrefabCanonicalComponentEqual(*source, MakeSource(kClipId)));

    // The exact wire key is present in the file.
    bool foundAudio = false;
    for (const auto& entity : saved["entities"])
    {
        if (entity.contains("audioSource"))
        {
            foundAudio = true;
            CHECK(entity["audioSource"]["bus"].get<std::string>() == "music");
            CHECK(entity["audioSource"]["priority"].get<uint64_t>() == 200u);
        }
    }
    CHECK(foundAudio);
    std::filesystem::remove_all(dir);
}

TEST_CASE("A2_V3V8MigrationAbsence: scenes without audio load sourceless")
{
    AudioFixture f;
    const auto emitter = f.CreateEmpty("Emitter");
    auto& registry = f.manager.GetECS().registry;
    registry.emplace_or_replace<AudioSourceComponent>(
        f.Handle(emitter), MakeSource(kClipId));

    const auto dir = UniqueTempDir("a2_v3v8_migration");
    const auto path = dir / "audio.rt2scene";
    Error err;
    REQUIRE(SaveSceneForTest(f.manager.AuthoringDoc(), path, err));

    for (uint32_t version = 3; version <= 8; ++version)
    {
        json relabeled = json::parse(ReadFileBinary(path));
        relabeled["version"] = version;
        for (auto& entity : relabeled["entities"])
            entity.erase("audioSource");
        const auto versionPath =
            dir / ("audio_v" + std::to_string(version) + ".rt2scene");
        WriteFileBinary(versionPath, relabeled.dump(2));

        SceneDocument loaded;
        Error loadErr;
        REQUIRE_MESSAGE(SceneSerializer::Load(loaded, versionPath, loadErr),
                        "v" << version << ": " << loadErr.detail);
        CHECK(loaded.metadata.schemaVersion == version);
        const auto e = loaded.FindByUuid(emitter);
        REQUIRE(static_cast<uint32_t>(e) !=
                static_cast<uint32_t>(entt::null));
        CHECK_FALSE(
            loaded.ecs.registry.all_of<AudioSourceComponent>(e));
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("A2_V9StrictInvalidInputs: malformed blocks fail with UUID and dotted path")
{
    const std::string clipId = kClipId.ToString();
    // Wrong kind with a non-empty path.
    ExpectAudioParseFail("wrong-kind",
        std::string(R"("audioSource": {)") + ClipJson("model", "sfx/hit.wav", clipId) +
            R"(, "bus": "effects", "autoplay": false, "loop": false, "spatial": false})",
        "audioSource.clip.kind");
    // Unsupported extension.
    ExpectAudioParseFail("bad-extension",
        std::string(R"("audioSource": {)") + ClipJson("audioclip", "sfx/hit.ogg", clipId) +
            R"(, "bus": "effects", "autoplay": false, "loop": false, "spatial": false})",
        "audioSource.clip.path");
    // Nil identity on a bound reference. The shared codec tolerates a
    // missing assetId as repairable migration state for other kinds, but v9
    // audio is strict: a bound clip requires a non-nil identity.
    ExpectAudioParseFail("nil-identity",
        std::string(R"("audioSource": {"clip": {"kind": "audioclip", "path": "sfx/hit.wav", "sourceKey": ""})") +
            R"(, "bus": "effects", "autoplay": false, "loop": false, "spatial": false})",
        "audioSource.clip.assetId");
    // Unknown bus string.
    ExpectAudioParseFail("bad-bus",
        std::string(R"("audioSource": {)") + ClipJson("audioclip", "sfx/hit.wav", clipId) +
            R"(, "bus": "mono", "autoplay": false, "loop": false, "spatial": false})",
        "audioSource.bus");
    // Master cannot be a source bus.
    ExpectAudioParseFail("master-bus",
        std::string(R"("audioSource": {)") + ClipJson("audioclip", "sfx/hit.wav", clipId) +
            R"(, "bus": "master", "autoplay": false, "loop": false, "spatial": false})",
        "audioSource.bus");
    // UI sources are always non-spatial.
    ExpectAudioParseFail("ui-spatial",
        std::string(R"("audioSource": {)") + ClipJson("audioclip", "sfx/hit.wav", clipId) +
            R"(, "bus": "ui", "autoplay": false, "loop": false, "spatial": true})",
        "audioSource.spatial");
    // Out-of-range and non-float-representable scalars. (JSON carries no
    // NaN/Infinity literal, so the non-finite wire case is pinned through
    // the in-memory Save path below; here 1e300 exceeds float range.)
    ExpectAudioParseFail("gain-huge",
        std::string(R"("audioSource": {)") + ClipJson("audioclip", "sfx/hit.wav", clipId) +
            R"(, "bus": "effects", "autoplay": false, "loop": false, "spatial": false, "gain": 1e300})",
        "audioSource.gain");
    ExpectAudioParseFail("gain-range",
        std::string(R"("audioSource": {)") + ClipJson("audioclip", "sfx/hit.wav", clipId) +
            R"(, "bus": "effects", "autoplay": false, "loop": false, "spatial": false, "gain": 4.5})",
        "audioSource.gain");
    ExpectAudioParseFail("pitch-range",
        std::string(R"("audioSource": {)") + ClipJson("audioclip", "sfx/hit.wav", clipId) +
            R"(, "bus": "effects", "autoplay": false, "loop": false, "spatial": false, "pitch": 0.1})",
        "audioSource.pitch");
    ExpectAudioParseFail("min-distance",
        std::string(R"("audioSource": {)") + ClipJson("audioclip", "sfx/hit.wav", clipId) +
            R"(, "bus": "effects", "autoplay": false, "loop": false, "spatial": false, "minDistance": 0.0})",
        "audioSource.minDistance");
    ExpectAudioParseFail("max-inverted",
        std::string(R"("audioSource": {)") + ClipJson("audioclip", "sfx/hit.wav", clipId) +
            R"(, "bus": "effects", "autoplay": false, "loop": false, "spatial": false, "minDistance": 5.0, "maxDistance": 2.0})",
        "audioSource.maxDistance");
    ExpectAudioParseFail("rolloff-range",
        std::string(R"("audioSource": {)") + ClipJson("audioclip", "sfx/hit.wav", clipId) +
            R"(, "bus": "effects", "autoplay": false, "loop": false, "spatial": false, "rolloff": 9.0})",
        "audioSource.rolloff");
    ExpectAudioParseFail("priority-range",
        std::string(R"("audioSource": {)") + ClipJson("audioclip", "sfx/hit.wav", clipId) +
            R"(, "bus": "effects", "autoplay": false, "loop": false, "spatial": false, "priority": 256})",
        "audioSource.priority");
    // Spatial sources require a Transform.
    ExpectAudioParseFail("missing-transform",
        std::string(R"("audioSource": {)") + ClipJson("audioclip", "sfx/hit.wav", clipId) +
            R"(, "bus": "effects", "autoplay": false, "loop": false, "spatial": true})",
        "audioSource.transform", false);
    // Structural malformation.
    ExpectAudioParseFail("non-object",
        R"("audioSource": 42)",
        "audioSource");
    ExpectAudioParseFail("bus-type",
        std::string(R"("audioSource": {)") + ClipJson("audioclip", "sfx/hit.wav", clipId) +
            R"(, "bus": 42, "autoplay": false, "loop": false, "spatial": false})",
        "audioSource.bus");
}

TEST_CASE("A2_SaveInvalidInputs: in-memory violations fail with UUID and dotted path")
{
    auto expectSaveFail = [](AudioSourceComponent source, bool withTransform,
                             const std::string& dottedField)
    {
        SceneDocument doc;
        DeterministicUuidProvider provider;
        doc.SetUuidProvider(&provider);
        const auto e = doc.ecs.registry.create();
        doc.ecs.registry.emplace<NameComponent>(e, "Emitter");
        doc.AssignNewUuid(e);
        const UUID id =
            doc.ecs.registry.get<EntityIdComponent>(e).id;
        if (withTransform)
            doc.ecs.registry.emplace<Transform>(e);
        doc.ecs.registry.emplace<AudioSourceComponent>(e, source);

        const auto dir = UniqueTempDir("a2_save_fail");
        const auto path = dir / "fail.rt2scene";
        Error err;
        std::vector<AssetDiagnostic> diagnostics;
        CHECK_MESSAGE(
            SceneSerializer::Save(doc, path, diagnostics, err) == false,
            dottedField);
        CHECK_MESSAGE(err.path.find(id.ToString()) != std::string::npos,
                      dottedField);
        CHECK_MESSAGE(err.path.find(dottedField) != std::string::npos,
                      dottedField << " in: " << err.path);
        std::filesystem::remove_all(dir);
    };

    AudioSourceComponent master = MakeSource(kClipId);
    master.bus = AudioBus::Master;
    expectSaveFail(master, true, "audioSource.bus");

    // Out-of-range enum: the shared validator refuses it before the codec
    // could serialize an "unknown" bus Load rejects.
    AudioSourceComponent garbage = MakeSource(kClipId);
    garbage.bus = static_cast<AudioBus>(255);
    expectSaveFail(garbage, true, "audioSource.bus");

    AudioSourceComponent ui = MakeSource(kClipId);
    ui.bus = AudioBus::UI;
    ui.spatial = true;
    expectSaveFail(ui, true, "audioSource.spatial");

    AudioSourceComponent gain = MakeSource(kClipId);
    gain.gain = std::numeric_limits<float>::infinity();
    expectSaveFail(gain, true, "audioSource.gain");

    AudioSourceComponent inverted = MakeSource(kClipId);
    inverted.minDistance = 9.0f;
    inverted.maxDistance = 2.0f;
    expectSaveFail(inverted, true, "audioSource.maxDistance");

    // Spatial source with no Transform in the registry.
    expectSaveFail(MakeSpatialSource(kClipId), false, "audioSource.transform");

    AudioSourceComponent kind = MakeSpatialSource(kClipId);
    kind.clip.kind = AssetKind::Model;
    expectSaveFail(kind, true, "audioSource.clip.kind");
}

TEST_CASE("A2_DecodedChannelSeam: mono rule waits for the A4 decoder")
{
    // The persistence context (nullopt) never enforces the decoded channel
    // count — it cannot know it without running the A4 decoder. The
    // enforcement below is the seam A4 must call with real decoder output
    // before Play or Preview commits; pretending otherwise would be a silent
    // reinterpretation of stereo spatial content.
    std::string detail;
    std::string field;

    AudioSourceComponent spatial = MakeSpatialSource(kClipId);
    CHECK(ValidateAudioSourceComponent(spatial, true, std::nullopt, detail,
                                       &field));
    CHECK(ValidateAudioSourceComponent(spatial, true, 1, detail, &field));
    CHECK_FALSE(ValidateAudioSourceComponent(spatial, true, 2, detail,
                                             &field));
    CHECK(field == "clip");

    AudioSourceComponent flat = MakeSource(kClipId); // non-spatial
    CHECK(ValidateAudioSourceComponent(flat, true, 2, detail, &field));
    CHECK(ValidateAudioSourceComponent(flat, true, std::nullopt, detail,
                                       &field));
}

TEST_CASE("A2_ProviderResolvesImmutableBytes: fingerprint, identity, and overlap")
{
    // Same-size/same-mtime byte replacement yields a new fingerprint while
    // old resolved bytes remain immutable (required check 3, A2 half: the
    // PCM-overlap half belongs to A4's decoded cache).
    const auto dir = UniqueTempDir("a2_provider");
    const auto clipPath = dir / "sfx" / "hit.wav";
    std::filesystem::create_directories(clipPath.parent_path());
    const std::vector<char> first = {'R', 'I', 'F', 'F', '0', '1', '2', '3'};
    WriteFileBytes(clipPath, first);
    Error err;
    REQUIRE(WriteSidecarId(AssetSidecarPath(clipPath), kClipId, err));

    AudioClipAssetProvider provider;
    AssetResolutionContext ctx;
    ctx.assetRoot = dir;
    provider.SetContext(ctx);

    AssetReference ref;
    ref.kind = AssetKind::AudioClip;
    ref.path = "sfx/hit.wav";
    ref.assetId = kClipId;

    auto resolved = provider.ResolveClip(ref, kEntityId, "Emitter");
    REQUIRE(resolved.IsOk());
    CHECK(resolved.value.effectiveId == kClipId);
    CHECK(resolved.value.IsValid());
    REQUIRE(resolved.value.bytes != nullptr);
    CHECK(*resolved.value.bytes == first);
    const uint64_t firstPrint = resolved.value.fingerprint;
    CHECK(firstPrint == FnV1a64(first.data(), first.size()));
    const auto oldBytes = resolved.value.bytes;
    CHECK(provider.CacheEntryCount() == 1);

    // Same-size rewrite with different bytes (mtime/size hints unchanged in
    // spirit): the fingerprint must change and the old owner must not mutate.
    const std::vector<char> second = {'R', 'I', 'F', 'F', '9', '8', '7', '6'};
    REQUIRE(second.size() == first.size());
    WriteFileBytes(clipPath, second);

    auto again = provider.ResolveClip(ref, kEntityId, "Emitter");
    REQUIRE(again.IsOk());
    CHECK(again.value.fingerprint != firstPrint);
    CHECK(again.value.fingerprint == FnV1a64(second.data(), second.size()));
    REQUIRE(again.value.bytes != nullptr);
    CHECK(*again.value.bytes == second);
    CHECK(*oldBytes == first); // previously resolved bytes are immutable
    std::filesystem::remove_all(dir);
}

TEST_CASE("A2_ProviderLoudFailures: wrong kind, extension, and missing file")
{
    const auto dir = UniqueTempDir("a2_provider_fail");
    AudioClipAssetProvider provider;
    AssetResolutionContext ctx;
    ctx.assetRoot = dir;
    provider.SetContext(ctx);

    AssetReference wrongKind;
    wrongKind.kind = AssetKind::Model;
    wrongKind.path = "sfx/hit.wav";
    auto r1 = provider.ResolveClip(wrongKind, kEntityId, "Emitter");
    CHECK_FALSE(r1.IsOk());
    CHECK(r1.error.path == kEntityId.ToString());

    AssetReference badExt;
    badExt.kind = AssetKind::AudioClip;
    badExt.path = "sfx/hit.ogg";
    auto r2 = provider.ResolveClip(badExt, kEntityId, "Emitter");
    CHECK_FALSE(r2.IsOk());
    CHECK(r2.error.path == kEntityId.ToString());

    AssetReference missing;
    missing.kind = AssetKind::AudioClip;
    missing.path = "sfx/gone.wav";
    auto r3 = provider.ResolveClip(missing, kEntityId, "Emitter");
    CHECK_FALSE(r3.IsOk());
    CHECK(r3.error.path == kEntityId.ToString());
    CHECK(r3.error.detail.find(kEntityId.ToString()) != std::string::npos);
    std::filesystem::remove_all(dir);
}

TEST_CASE("A2_ProviderOwnsDatabaseSnapshot: replacement cannot dangle or leak")
{
    // The provider snapshots the database at SetContext: mutating or
    // destroying the caller's database afterwards must neither corrupt
    // later resolves nor read freed memory. Discrimination: with a retained
    // raw pointer, the post-mutation resolve below fails Conflict (the live
    // database is ambiguous); with the owned snapshot it still succeeds.
    const auto dir = UniqueTempDir("a2_provider_snapshot");
    const auto sfx = dir / "sfx";
    std::filesystem::create_directories(sfx);
    const std::vector<char> hitBytes = {'R', 'I', 'F', 'F', 'h', 'i', 't', '!'};
    const std::vector<char> otherBytes = {'R', 'I', 'F', 'F', 'o', 't', 'h', 'r'};
    WriteFileBytes(sfx / "hit.wav", hitBytes);
    WriteFileBytes(sfx / "other.wav", otherBytes);
    Error err;
    REQUIRE(WriteSidecarId(AssetSidecarPath(sfx / "hit.wav"), kClipId, err));
    REQUIRE(WriteSidecarId(AssetSidecarPath(sfx / "other.wav"), kOtherClipId, err));

    AssetReference hitRef;
    hitRef.kind = AssetKind::AudioClip;
    hitRef.path = "sfx/hit.wav";
    hitRef.assetId = kClipId;
    AssetReference otherRef;
    otherRef.kind = AssetKind::AudioClip;
    otherRef.path = "sfx/other.wav";
    otherRef.assetId = kClipId;

    AudioClipAssetProvider provider;
    AssetDatabase live;
    std::vector<AssetDatabaseDiagnostic> dbDiags;
    live.AddOrUpdate(Record("sfx/hit.wav", kClipId), dbDiags);
    provider.SetContext(AssetResolutionContext{dir, &live});

    // Mutate the live database after the snapshot: the same ID is now
    // claimed by two paths, so the LIVE database is ambiguous.
    live.AddOrUpdate(Record("sfx/other.wav", kClipId), dbDiags);
    CHECK(live.LookupById(kClipId).status ==
          AssetIdLookupResult::Status::Ambiguous);

    // The provider still resolves through its pre-mutation snapshot.
    auto held = provider.ResolveClip(hitRef, kEntityId, "Emitter");
    REQUIRE(held.IsOk());
    REQUIRE(held.value.bytes != nullptr);
    CHECK(*held.value.bytes == hitBytes);
    CHECK(held.value.effectiveId == kClipId);

    // A refresh replaces the snapshot: the new database is live immediately.
    AssetDatabase second;
    second.AddOrUpdate(Record("sfx/other.wav", kClipId), dbDiags);
    provider.SetContext(AssetResolutionContext{dir, &second});
    auto moved = provider.ResolveClip(otherRef, kEntityId, "Emitter");
    REQUIRE(moved.IsOk());
    REQUIRE(moved.value.bytes != nullptr);
    CHECK(*moved.value.bytes == otherBytes);

    // The source database may die with its scope; the snapshot outlives it.
    {
        AssetDatabase scoped;
        scoped.AddOrUpdate(Record("sfx/hit.wav", kClipId), dbDiags);
        provider.SetContext(AssetResolutionContext{dir, &scoped});
    }
    auto afterScope = provider.ResolveClip(hitRef, kEntityId, "Emitter");
    REQUIRE(afterScope.IsOk());
    CHECK(*afterScope.value.bytes == hitBytes);
    std::filesystem::remove_all(dir);
}

TEST_CASE("A2_ProviderCanonicalizesAliasPaths: hit and miss report one identity")
{
    // Both the miss and hit branches must advertise the COMPUTED canonical
    // path, not the raw resolution spelling: a linked/case-variant alias of
    // the same file shares the cache entry and reports the same identity
    // the A4 decoded-generation key is built from. Discrimination: with the
    // raw spelling advertised, the alias resolve below reports a different
    // canonicalPath than the first resolve.
    const auto dir = UniqueTempDir("a2_provider_alias");
    const auto sfx = dir / "sfx";
    std::filesystem::create_directories(sfx);
    const std::vector<char> bytes = {'R', 'I', 'F', 'F', 'a', 'l', 'i', 'a', 's'};
    WriteFileBytes(sfx / "hit.wav", bytes);
    Error err;
    REQUIRE(WriteSidecarId(AssetSidecarPath(sfx / "hit.wav"), kClipId, err));

    AudioClipAssetProvider provider;
    provider.SetContext(AssetResolutionContext{dir, nullptr});

    AssetReference ref;
    ref.kind = AssetKind::AudioClip;
    ref.path = "sfx/hit.wav";
    auto first = provider.ResolveClip(ref, kEntityId, "Emitter");
    REQUIRE(first.IsOk());
    const std::filesystem::path expected =
        CanonicalAssetPath(dir / "sfx" / "hit.wav");
    CHECK(first.value.canonicalPath == expected);

#ifdef _WIN32
    // Case-variant spelling: the filesystem is case-insensitive, so this
    // names the same file through a different lexical spelling.
    AssetReference upper;
    upper.kind = AssetKind::AudioClip;
    upper.path = "SFX/HIT.WAV";
    auto alias = provider.ResolveClip(upper, kEntityId, "Emitter");
    REQUIRE(alias.IsOk());
    CHECK(alias.value.canonicalPath == expected);
    CHECK(alias.value.canonicalPath == first.value.canonicalPath);
    CHECK(alias.value.fingerprint == first.value.fingerprint);
    CHECK(alias.value.bytes == first.value.bytes); // shared cache owner
    CHECK(provider.CacheEntryCount() == 1);
#endif

    // Directory-link spelling when the platform grants it. Junctions and
    // symlinks need no fixture: a missing privilege skips this block with
    // a message while the case-variant discriminator above still guards.
    std::error_code linkError;
    std::filesystem::create_directory_symlink(sfx, dir / "link", linkError);
    if (linkError)
    {
        MESSAGE("directory links unavailable; linked-alias probe skipped: " <<
                linkError.message());
    }
    else
    {
        AssetReference linked;
        linked.kind = AssetKind::AudioClip;
        linked.path = "link/hit.wav";
        auto viaLink = provider.ResolveClip(linked, kEntityId, "Emitter");
        REQUIRE(viaLink.IsOk());
        CHECK(viaLink.value.canonicalPath == expected);
        CHECK(provider.CacheEntryCount() == 1);
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("A2_SidecarIdentityFlow: scanner sees clips; import mints identity")
{
    // The project scanner is sidecar-driven: a clip plus its sidecar yields
    // a database record without any kind-specific scan step.
    const auto dir = UniqueTempDir("a2_sidecar");
    const auto assets = dir / "Assets";
    std::filesystem::create_directories(assets / "sfx");
    const auto clip = assets / "sfx" / "hit.wav";
    WriteFileBytes(clip, {'R', 'I', 'F', 'F'});
    Error err;
    REQUIRE(WriteSidecarId(AssetSidecarPath(clip), kClipId, err));

    ProjectAssetScanResult scan;
    REQUIRE(ScanProjectAssets(assets, scan, err));
    REQUIRE(scan.database != nullptr);
    const AssetRecord* record = scan.database->FindByPath("sfx/hit.wav");
    REQUIRE(record != nullptr);
    CHECK(record->assetId == kClipId);

    // First assignment mints and persists a fresh sidecar, exactly as other
    // source assets do.
    DeterministicUuidProvider provider;
    const auto fresh = assets / "sfx" / "new.mp3";
    WriteFileBytes(fresh, {'I', 'D', '3', '!'});
    bool minted = false;
    const UUID assigned =
        ResolveOrAssign(fresh, provider, minted, err);
    CHECK(err.IsOk());
    CHECK(minted);
    CHECK_FALSE(assigned.IsNull());
    CHECK(ReadSidecarId(AssetSidecarPath(fresh), err) == assigned);
    std::filesystem::remove_all(dir);
}

TEST_CASE("A2_CloneCopyPasteRecovery: every route carries the source exactly")
{
    AudioFixture f;
    const auto emitter = f.CreateEmpty("Emitter");
    auto& registry = f.manager.GetECS().registry;
    registry.emplace_or_replace<AudioSourceComponent>(
        f.Handle(emitter), MakeSource(kClipId));

    auto requireSource = [&](SceneManager& manager, const UUID& uuid)
    {
        const auto* source = SourceOf(manager, uuid);
        REQUIRE(source != nullptr);
        CHECK(*source == MakeSource(kClipId));
        CHECK(PrefabCanonicalComponentEqual(*source, MakeSource(kClipId)));
    };

    // CloneInMemory (the Play-clone path).
    {
        SceneDocument clone;
        Error cloneErr;
        REQUIRE(SceneSerializer::CloneInMemory(
            f.manager.AuthoringDoc(), clone, cloneErr));
        const auto e = clone.FindByUuid(emitter);
        REQUIRE(static_cast<uint32_t>(e) !=
                static_cast<uint32_t>(entt::null));
        const auto* source =
            clone.ecs.registry.try_get<AudioSourceComponent>(e);
        REQUIRE(source != nullptr);
        CHECK(*source == MakeSource(kClipId));
    }

    // Duplicate with fresh UUIDs (verbatim values).
    UUID duplicateUuid;
    {
        const auto uuids = f.manager.ReserveKnownUuids(1);
        const auto duplicated =
            f.manager.DuplicateSubtreesWithUuids({emitter}, uuids);
        REQUIRE(duplicated.mutation.success);
        REQUIRE(uuids.size() == 1);
        duplicateUuid = uuids.front();
        requireSource(f.manager, duplicateUuid);
    }

    // Copy/paste through the clipboard document (verbatim values, fresh IDs).
    {
        SceneDocument clipboard;
        clipboard.SetUuidProvider(&f.ids);
        Error cloneError;
        REQUIRE(SceneSerializer::CloneInMemory(
            f.manager.AuthoringDoc(), clipboard, cloneError));
        const auto pastedUuids = f.manager.ReserveKnownUuids(1);
        const auto pasted = f.manager.PasteSubtreesWithUuids(
            clipboard, {emitter}, std::nullopt, pastedUuids);
        REQUIRE(pasted.mutation.success);
        REQUIRE(pastedUuids.size() == 1);
        requireSource(f.manager, pastedUuids.front());
    }

    // Recovery snapshot (SaveTo) followed by restore.
    {
        const auto dir = UniqueTempDir("a2_recovery");
        const auto snapshotPath = dir / "snapshot.rt2scene";
        const auto logicalPath = dir / "scene.rt2scene";
        std::vector<AssetDiagnostic> diags;
        Error saveErr;
        REQUIRE(SceneSerializer::SaveTo(f.manager.AuthoringDoc(),
                                        snapshotPath, logicalPath, diags,
                                        saveErr));
        SceneDocument restored;
        DeterministicUuidProvider ids;
        restored.SetUuidProvider(&ids);
        REQUIRE(SceneSerializer::Load(restored, snapshotPath, saveErr));
        const auto e = restored.FindByUuid(emitter);
        REQUIRE(static_cast<uint32_t>(e) !=
                static_cast<uint32_t>(entt::null));
        const auto* source =
            restored.ecs.registry.try_get<AudioSourceComponent>(e);
        REQUIRE(source != nullptr);
        CHECK(*source == MakeSource(kClipId));
        const auto d = restored.FindByUuid(duplicateUuid);
        REQUIRE(static_cast<uint32_t>(d) !=
                static_cast<uint32_t>(entt::null));
        const auto* dupSource =
            restored.ecs.registry.try_get<AudioSourceComponent>(d);
        REQUIRE(dupSource != nullptr);
        CHECK(*dupSource == MakeSource(kClipId));
        std::filesystem::remove_all(dir);
    }
}

TEST_CASE("A2_PrefabAudioWire: non-overridable presence/fields with exact payload")
{
    // The audioSource key resolves non-overridable; the overridable total
    // stays 9; propagation has no audio adapter (matching physics).
    CHECK_FALSE(IsOverridable<AudioSourceComponent>());
    const auto key = FindComponentByWire("audioSource");
    REQUIRE(key.has_value());
    CHECK_FALSE(key->overridable());
    CHECK(*key == PrefabComponentKeyFor<AudioSourceComponent>::value);
    CHECK_FALSE(IsPropagationComponentV<AudioSourceComponent>);

    // Prefab record conversion carries the payload on a link-free sibling
    // (prefab files never carry scene-side link components — loud refusal).
    AudioFixture f;
    const auto sibling = f.CreateEmpty("RecordSibling");
    auto& registry = f.manager.GetECS().registry;
    registry.emplace_or_replace<AudioSourceComponent>(
        f.Handle(sibling), MakeSource(kClipId));
    const auto snapshot = f.manager.CaptureSubtreeSnapshot({sibling});
    REQUIRE(snapshot.entities.size() == 1);
    REQUIRE(snapshot.entities.front().hasAudioSource);
    PrefabEntityRecord record;
    record.templateId = f.ids.CreateV4();
    record.record = snapshot.entities.front();
    std::vector<AssetDiagnostic> diags;
    Error recordErr;
    json out;
    REQUIRE(PrefabRecordToJson(record, diags, recordErr, out));
    PrefabEntityRecord parsed;
    REQUIRE(JsonToPrefabRecord(out, recordErr, parsed));
    CHECK(parsed.templateId == record.templateId);
    REQUIRE(parsed.record.hasAudioSource);
    CHECK(parsed.record.audioSource == MakeSource(kClipId));

    // Scene load rejects an override naming the audio wire: member edits are
    // refused unless made at the prefab source.
    const std::string uuid = "11111111-1111-4111-8111-111111111111";
    const std::string instance = "22222222-2222-4222-8222-222222222222";
    const std::string templ = "33333333-3333-4333-8333-333333333333";
    const std::string content = std::string(R"({
  "version": 9,
  "metadata": {"name": "audio-wire-reject"},
  "entities": [{
    "uuid": ")") + uuid + R"(",
    "name": "Member",
    "parent": "",
    "visible": true,
    "transform": {"translation": [0,0,0], "rotation": [0,0,0,1],
                  "scale": [1,1,1]},
    "prefabMember": {"instanceId": ")" + instance + R"(",
                     "templateId": ")" + templ + R"(",
                     "overrides": ["audioSource"]}
  }],
  "materials": [], "textures": [],
  "camera": {"position": [0,0,0], "forward": [0,0,-1], "fov": 45},
  "envMap": {"kind": "unknown", "path": "", "sourceKey": ""}
})";
    const auto dir = UniqueTempDir("a2_wire_reject");
    const auto path = dir / "wire.rt2scene";
    WriteFileBinary(path, content);
    SceneDocument loaded;
    Error err;
    CHECK_FALSE(SceneSerializer::Load(loaded, path, err));
    CHECK(err.code == Error::Parse);
    CHECK(err.detail.find("audioSource") != std::string::npos);

    // The CPU mutation-layer guard refuses the same override on a live
    // linked member and leaves state unchanged.
    const auto root = f.CreateEmpty("Rig");
    const auto prefabPath = dir / "rig.rt2prefab";
    REQUIRE(f.manager.CreatePrefabFromSubtree({root}, prefabPath).ok);
    const auto uuids = f.manager.ReserveKnownUuids(1);
    std::vector<AssetDiagnostic> instDiags;
    const auto inst = f.manager.InstantiatePrefabWithUuids(
        prefabPath, uuids, instDiags);
    REQUIRE(inst.mutation.success);
    const UUID member = uuids.front();
    const auto before = f.manager.GetOverrides(member);
    REQUIRE(before.IsOk());
    const auto rejected = f.manager.IsOverridden(
        member, PrefabComponentKeyFor<AudioSourceComponent>::value);
    CHECK_FALSE(rejected.IsOk());
    CHECK(rejected.error.code == Error::InvalidArgument);
    CHECK(rejected.error.detail.find("audioSource") != std::string::npos);
    const auto after = f.manager.GetOverrides(member);
    REQUIRE(after.IsOk());
    CHECK(after.value.size() == before.value.size());
    std::filesystem::remove_all(dir);
}

TEST_CASE("A2_DependantsSeeAudioClip: ID-exact and path-fallback protection")
{
    // Rename/delete confirmation gates see audio dependants, so a clip in
    // use cannot be removed without a loud warning.
    const auto root = UniqueTempDir("a2_dependants");
    const auto assets = root / "Assets";
    std::error_code ec;
    std::filesystem::create_directories(assets, ec);

    SceneDocument document;
    DeterministicUuidProvider provider;
    document.SetUuidProvider(&provider);
    const auto entity = document.ecs.registry.create();
    REQUIRE(document.AssignKnownUuid(entity, kEntityId));
    document.ecs.registry.emplace<NameComponent>(entity, "Emitter");
    AudioSourceComponent source = MakeSource(kClipId);
    document.ecs.registry.emplace<AudioSourceComponent>(entity, source);

    auto byId = FindContentBrowserDependants(
        document, Record("sfx/hit.wav", kClipId), assets);
    REQUIRE(byId.size() == 1);
    CHECK(byId[0].entityUuid == kEntityId);
    CHECK(byId[0].entityName == "Emitter");
    CHECK(byId[0].kind == AssetKind::AudioClip);
    CHECK(byId[0].sourcePath == "sfx/hit.wav");

    // Nil-ID scene references still match by path so the warning cannot
    // under-report a dependant.
    AudioSourceComponent legacy = MakeSource(UUID::Nil());
    document.ecs.registry.emplace_or_replace<AudioSourceComponent>(entity,
                                                                   legacy);
    auto byPath = FindContentBrowserDependants(
        document, Record("sfx/hit.wav", kClipId), assets);
    REQUIRE(byPath.size() == 1);
    CHECK(byPath[0].entityUuid == kEntityId);
    std::filesystem::remove_all(root);
}

TEST_CASE("A2_ContentBrowserAudioFirstImport: drop assigns the clip sidecar")
{
    // The A2-promised Content Browser first-import action: dropping a new
    // WAV/FLAC/MP3 runs the production dispatch arm into the production
    // ImportAudioClipAsset action (sidecar ResolveOrAssign, no decode). A
    // second drop of the same file reuses the minted identity. The
    // installed dispatch callback is the same one-line production shape the
    // host installs — not test-only logic — and the inspector clip
    // browse/drop authoring plus Preview surface remain A7 scope.
    const auto dir = UniqueTempDir("a2_audio_import");
    const auto clip = dir / "new.wav";
    WriteFileBytes(clip, {'R', 'I', 'F', 'F', 'n', 'e', 'w', '!'});
    const std::string clipString = clip.u8string();
    REQUIRE_FALSE(std::filesystem::exists(AssetSidecarPath(clip)));

    // Production adapter: the exact callback shape the host installs.
    // All loud logic lives in ImportAudioClipAsset; this only adapts its
    // result struct to the dispatch contract.
    DeterministicUuidProvider ids;
    auto productionCallback =
        [&](const std::string& dropped, Error& error) {
            AudioClipFirstImportResult result;
            return ImportAudioClipAsset(dropped, ids, result, error);
        };

    // No callback wired: loud failure naming the drop, no sidecar minted.
    {
        ContentBrowserDropCallbacks empty;
        Error error;
        CHECK_FALSE(
            DispatchContentBrowserAssetDrop(clipString, empty, error));
        CHECK(error.code == Error::InvalidArgument);
        CHECK(error.detail.find("audio clip drop has no import callback") !=
              std::string::npos);
        CHECK_FALSE(std::filesystem::exists(AssetSidecarPath(clip)));
    }

    ContentBrowserDropCallbacks callbacks;
    bool gltfCalled = false;
    bool objCalled = false;
    bool prefabCalled = false;
    callbacks.importGltf = [&](const std::string&) { gltfCalled = true; };
    callbacks.importObj = [&](const std::string&, const ImportSettings&) {
        objCalled = true;
    };
    callbacks.instantiatePrefab = [&](const std::string&) {
        prefabCalled = true;
    };
    callbacks.importAudioClip = productionCallback;

    // First drop mints and writes the sidecar through the dispatcher.
    Error error;
    REQUIRE(DispatchContentBrowserAssetDrop(clipString, callbacks, error));
    CHECK(error.IsOk());
    Error readError;
    const UUID assigned = ReadSidecarId(AssetSidecarPath(clip), readError);
    CHECK(readError.IsOk());
    CHECK_FALSE(assigned.IsNull());
    CHECK_FALSE(gltfCalled);
    CHECK_FALSE(objCalled);
    CHECK_FALSE(prefabCalled);

    // A second drop of the same clip reuses the minted identity.
    REQUIRE(DispatchContentBrowserAssetDrop(clipString, callbacks, error));
    CHECK(error.IsOk());
    CHECK(ReadSidecarId(AssetSidecarPath(clip), readError) == assigned);

    // The refreshed database lists the clip: this is what makes it visible
    // in the Content Browser, which shows sidecar-backed records only.
    ProjectAssetScanResult scan;
    REQUIRE(ScanProjectAssets(dir, scan, error));
    REQUIRE(scan.database != nullptr);
    const AssetRecord* record = scan.database->FindByPath("new.wav");
    REQUIRE(record != nullptr);
    CHECK(record->assetId == assigned);

    // Ogg stays unsupported: out of scope for the first delivery.
    const auto ogg = dir / "hit.ogg";
    WriteFileBytes(ogg, {'O', 'g', 'g', 'S'});
    CHECK_FALSE(DispatchContentBrowserAssetDrop(ogg.u8string(), callbacks,
                                               error));
    CHECK(error.code == Error::InvalidArgument);
    std::filesystem::remove_all(dir);
}

TEST_CASE("A2_AudioFirstImportWriteFailure: dispatcher reports the loud error")
{
    // A sidecar that cannot be written (a directory blocks the sidecar
    // path) must fail the drop loudly. Discrimination: with the previous
    // void callback shape the dispatcher returned success here; the
    // fallible contract returns false with the write Error instead.
    const auto dir = UniqueTempDir("a2_audio_import_fail");
    const auto clip = dir / "blocked.wav";
    WriteFileBytes(clip, {'R', 'I', 'F', 'F', 'x', 'x', 'x', 'x'});
    std::error_code ec;
    std::filesystem::create_directories(AssetSidecarPath(clip), ec);
    REQUIRE_FALSE(ec);

    // Direct production action first: loud Io failure, no identity adopted.
    DeterministicUuidProvider ids;
    AudioClipFirstImportResult result;
    Error actionError;
    CHECK_FALSE(ImportAudioClipAsset(clip.u8string(), ids, result,
                                     actionError));
    CHECK_FALSE(actionError.IsOk());
    CHECK(actionError.code == Error::Io);
    const bool namesSidecarOrClip =
        actionError.detail.find("sidecar") != std::string::npos ||
        actionError.path.find("blocked.wav") != std::string::npos;
    CHECK(namesSidecarOrClip);

    // Then through the dispatcher with the production callback installed:
    // the same failure propagates instead of reporting success.
    ContentBrowserDropCallbacks callbacks;
    callbacks.importAudioClip =
        [&](const std::string& dropped, Error& error) {
            AudioClipFirstImportResult inner;
            return ImportAudioClipAsset(dropped, ids, inner, error);
        };
    Error dispatchError;
    CHECK_FALSE(DispatchContentBrowserAssetDrop(clip.u8string(), callbacks,
                                               dispatchError));
    CHECK_FALSE(dispatchError.IsOk());
    CHECK(dispatchError.code == Error::Io);
    std::filesystem::remove_all(dir);
}

TEST_CASE("A2_AudioFirstImportMalformedSidecar: repair is refused loudly")
{
    // A malformed sidecar is not silently adopted or overwritten by the
    // import action: the drop fails with the parse Error so the user fixes
    // or removes the sidecar first.
    const auto dir = UniqueTempDir("a2_audio_import_malformed");
    const auto clip = dir / "odd.wav";
    WriteFileBytes(clip, {'R', 'I', 'F', 'F', 'o', 'd', 'd', '!'});
    WriteFileBinary(AssetSidecarPath(clip), "not-a-uuid");

    DeterministicUuidProvider ids;
    AudioClipFirstImportResult result;
    Error error;
    CHECK_FALSE(
        ImportAudioClipAsset(clip.u8string(), ids, result, error));
    CHECK(error.code == Error::Parse);

    // A separate malformed clip for the dispatch half: the direct action
    // above repairs by overwrite, so reusing the same file would no longer
    // be malformed.
    const auto clip2 = dir / "odd2.wav";
    WriteFileBytes(clip2, {'R', 'I', 'F', 'F', 'o', 'd', 'd', '2'});
    WriteFileBinary(AssetSidecarPath(clip2), "not-a-uuid-either");
    ContentBrowserDropCallbacks callbacks;
    callbacks.importAudioClip =
        [&](const std::string& dropped, Error& error) {
            AudioClipFirstImportResult inner;
            return ImportAudioClipAsset(dropped, ids, inner, error);
        };
    Error dispatchError;
    CHECK_FALSE(DispatchContentBrowserAssetDrop(clip2.u8string(), callbacks,
                                               dispatchError));
    CHECK(dispatchError.code == Error::Parse);
    std::filesystem::remove_all(dir);
}

TEST_CASE("A2_CpuIsolationAndProjectWiring: no miniaudio; tracked lists carry the unit")
{
    // The hard #error guard at the top of this file fails the build if
    // miniaudio.h ever becomes reachable from RT2Tests; this case keeps the
    // guarantee visible in test listings (same pattern as the A0/A1 CPU
    // boundary cases).
    CHECK(true);

    // Full tracked project wiring: the new CPU translation unit is listed in
    // both CPU targets, so a hand-maintained project regen cannot silently
    // drop the provider from tests or the slice runner.
    bool testsFound = false;
    const std::string tests =
        ReadRepoFile("RT2Tests/premake5.lua", testsFound);
    REQUIRE_MESSAGE(testsFound,
                    "Run RT2Tests from the repository root");
    CHECK_MESSAGE(tests.find("AudioClipAssetProvider.cpp") != std::string::npos,
                  "RT2Tests/premake5.lua must list AudioClipAssetProvider.cpp");

    bool sliceFound = false;
    const std::string slice =
        ReadRepoFile("RT2SliceRunner/premake5.lua", sliceFound);
    REQUIRE_MESSAGE(sliceFound,
                    "Run RT2Tests from the repository root");
    CHECK_MESSAGE(slice.find("AudioClipAssetProvider.cpp") != std::string::npos,
                  "RT2SliceRunner/premake5.lua must list AudioClipAssetProvider.cpp");
}
