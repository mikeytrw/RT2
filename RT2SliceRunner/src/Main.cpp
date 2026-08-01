#include "SceneSerializer.h"
#include "SceneDocument.h"
#include "RuntimeSceneController.h"
#include "ISceneRenderBridge.h"
#include "ECSComponents.h"
#include "ECSScene.h"
#include "PrimitiveGeometry.h"
#include "EditorSettings.h"
#include "SceneRecoveryService.h"
#include "SceneManager.h"
#include "Phase1AFixtureGenerator.h"
#include "UnsavedChangesCoordinator.h"
#include "core/UUID.h"
#include "core/Error.h"
#include "GPUSceneData.h"
#include "ScriptSystem.h"
#include "ScriptScenarioCompare.h"
#include "IRuntimeCommandSink.h"
#include "IRuntimeScriptDispatch.h"
#include "RuntimeLifecycleObserver.h"
#include "InputTypes.h"
#include "ProjectContext.h"
#include "SceneAssetResolver.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cmath>
#include <algorithm>
#include "json.hpp"

using namespace rt2::core;
using json = nlohmann::json;

// ============================================================================
// NullSceneRenderBridge — no GPU, records calls for verification
// ============================================================================

class NullSceneRenderBridge final : public ISceneRenderBridge
{
public:
    int fullSyncs = 0;
    int transformSyncs = 0;
    int renders = 0;

    void FullSync(GPUSceneData&) override     { ++fullSyncs; }
    void MaterialSync(GPUSceneData&) override  { ++transformSyncs; /* unused */ }
    void TransformSync(GPUSceneData&) override { ++transformSyncs; }
    void ResetTemporalState() override         {}
    void RequestRender() override              { ++renders; }
};

// ============================================================================
// NullInputService — inert input for the headless script scenario.
// All actions return None; axes return 0; mouse/scroll return zero. This
// proves the script-scenario runs without a real window/input system.
// ============================================================================

class NullInputService final : public IInputService
{
public:
    ActionState GetActionState(const std::string&) const override
    { return ActionState::None; }
    float GetAxisValue(const std::string&) const override { return 0.0f; }
    glm::vec2 GetMouseDelta() const override { return {0.0f, 0.0f}; }
    float GetScrollDelta() const override { return 0.0f; }
    void RequestCursorCapture(bool) override {}
    bool IsCursorCaptureRequested() const override { return false; }
};

// ============================================================================
// JSON report output
// ============================================================================

void PrintTransformJSON(FILE* out, const glm::vec3& t, const glm::quat& r, const glm::vec3& s)
{
    fprintf(out, "    \"translation\": [%.9g, %.9g, %.9g],\n", t.x, t.y, t.z);
    fprintf(out, "    \"rotation\":    [%.9g, %.9g, %.9g, %.9g],\n", r.x, r.y, r.z, r.w);
    fprintf(out, "    \"scale\":       [%.9g, %.9g, %.9g]\n", s.x, s.y, s.z);
}

// ============================================================================
// Main
// ============================================================================

void PrintUsage()
{
    printf("RT2SliceRunner — CPU-only vertical slice verification\n");
    printf("Usage: RT2SliceRunner [--project <path.rt2proj>] [--scene <path.rt2scene>] [--steps <N>] [--out <report.json>]\n");
    printf("Options:\n");
    printf("  --scene <path>         .rt2scene file to load and run\n");
    printf("  --project <path>       Project context; --scene then names an asset-root-relative scene\n");
    printf("  --steps <N>            Number of fixed update steps (default 60)\n");
    printf("  --out <path>           Write JSON report to file instead of stdout\n");
    printf("  --recovery-scenario    Run the Phase 1B recovery regression scenario\n");
    printf("  --script-scenario <p>  Run the Phase 6C headless script scenario\n");
    printf("  --out <path>           (with --script-scenario) write report to file\n");
    printf("  --help                 Show this help\n");
}

// ============================================================================
// Phase 1B recovery regression scenario (CPU-only)
// ============================================================================
//
// 1. Generate and load a textured GLB plus EXR environment.
// 2. Save it explicitly to a temp dir; record the file bytes.
// 3. Apply deterministic transform and material edits.
// 4. Force an autosave via an injected clock.
// 5. Drop the first session (do NOT discard — simulate unclean exit).
// 6. Construct a second session; discover + restore the recovery record.
// 7. Verify the edit is present, UUIDs match, and the explicit file is
//    byte-for-byte unchanged.
// 8. Discard the recovery record; verify it's gone.
// 9. Emit a structured JSON result; exit non-zero on failure.

static int RunRecoveryScenario(const std::string& outPath)
{
    namespace fs = std::filesystem;
    bool ok = true;
    std::string failReason;

    // Temp working dir.
    auto workDir = fs::temp_directory_path() / ("rt2_recovery_scenario_" + std::to_string(std::rand()));
    fs::create_directories(workDir);

    auto emitReport = [&](FILE* out) {
        fprintf(out, "{\n");
        fprintf(out, "  \"recoveryScenario\": \"%s\",\n", ok ? "pass" : "fail");
        fprintf(out, "  \"assetBacked\": true,\n");
        if (!ok && !failReason.empty())
            fprintf(out, "  \"reason\": \"%s\",\n", failReason.c_str());
        fprintf(out, "  \"workDir\": \"%s\"\n", workDir.string().c_str());
        fprintf(out, "}\n");
    };

    // 1. Build a real asset-backed document with a texture, imported mesh,
    // material provenance, and decoded environment map.
    Error err;
    auto glbPath = workDir / "tiny_textured.glb";
    auto exrPath = workDir / "tiny_env.exr";
    if (!GenerateTinyTexturedGlb(glbPath, err) ||
        !GenerateTinyExrEnv(exrPath, err))
    {
        ok = false;
        failReason = "fixture generation failed: " + err.Format();
        if (outPath.empty()) emitReport(stdout);
        else { FILE* f = fopen(outPath.c_str(), "w"); if (f) { emitReport(f); fclose(f); } }
        return 1;
    }

    SceneManager manager;
    if (!manager.LoadScene(glbPath.string()) || !manager.LoadEnvMap(exrPath.string()))
    {
        ok = false;
        failReason = "asset-backed scene load failed";
        if (outPath.empty()) emitReport(stdout);
        else { FILE* f = fopen(outPath.c_str(), "w"); if (f) { emitReport(f); fclose(f); } }
        return 1;
    }
    SceneDocument& doc = manager.AuthoringDoc();
    doc.metadata.name = "RecoveryScenario";
    auto scenePath = workDir / "scene.rt2scene";
    doc.metadata.sourcePath = scenePath.string();

    // 2. Save explicitly; record bytes.
    std::vector<AssetDiagnostic> saveDiagnostics;
    if (!SceneSerializer::Save(doc, scenePath, saveDiagnostics, err))
    {
        ok = false;
        failReason = "explicit save failed: " + err.Format();
        if (outPath.empty()) emitReport(stdout);
        else { FILE* f = fopen(outPath.c_str(), "w"); if (f) { emitReport(f); fclose(f); } }
        return 1;
    }
    std::string explicitBefore;
    {
        std::ifstream in(scenePath, std::ios::binary);
        std::stringstream ss; ss << in.rdbuf();
        explicitBefore = ss.str();
    }

    // Capture an imported entity UUID and author a transform/material edit.
    UUID originalUuid;
    entt::entity importedEntity = entt::null;
    {
        auto view = doc.ecs.registry.view<ImportedMeshSourceComponent>();
        if (view.empty())
        {
            ok = false;
            failReason = "fixture contains no imported entity provenance";
            if (outPath.empty()) emitReport(stdout);
            else { FILE* f = fopen(outPath.c_str(), "w"); if (f) { emitReport(f); fclose(f); } }
            return 1;
        }
        importedEntity = *view.begin();
        originalUuid = doc.ecs.registry.get<EntityIdComponent>(importedEntity).id;
    }

    const int materialIndex = doc.ecs.registry.get<MeshRef>(importedEntity).materialIndex;
    SceneMaterial edited = manager.GetMaterial(materialIndex);
    edited.roughness = 0.123f;
    edited.baseColor = {0.1f, 0.9f, 0.1f};
    manager.SetMaterialProperties(materialIndex, edited);
    manager.SetTransform({importedEntity}, {5.0f, 0.0f, 0.0f});
    uint64_t revision = manager.AuthoringRevision();

    // 4. Force an autosave via an injected clock.
    auto recoveryRoot = workDir / "Recovery";
    int64_t fakeNow = 1000;
    SceneRecoveryService svc(recoveryRoot,
        [&fakeNow]() { return fakeNow; }, 8, 60.0);
    std::vector<AssetDiagnostic> recoveryDiagnostics;
    if (svc.MaybeSnapshot(
            doc, revision, "unused", workDir, recoveryDiagnostics, err))
    {
        ok = false;
        failReason = "autosave wrote before the configured interval";
        if (outPath.empty()) emitReport(stdout);
        else { FILE* f = fopen(outPath.c_str(), "w"); if (f) { emitReport(f); fclose(f); } }
        return 1;
    }
    fakeNow += 60;
    if (!svc.MaybeSnapshot(
            doc, revision, "unused", workDir, recoveryDiagnostics, err))
    {
        ok = false;
        failReason = "autosave failed: " + err.Format();
        if (outPath.empty()) emitReport(stdout);
        else { FILE* f = fopen(outPath.c_str(), "w"); if (f) { emitReport(f); fclose(f); } }
        return 1;
    }

    // 5. Drop the first session — do NOT discard. Simulate unclean exit by
    //    simply letting `svc` go out of scope (we construct a new one below).

    // 6. Construct a second session; discover + restore.
    SceneRecoveryService svc2(recoveryRoot,
        [&fakeNow]() { return fakeNow; }, 8, 60.0);
    auto recs = svc2.Discover(err);
    if (recs.size() != 1)
    {
        ok = false;
        failReason = "expected 1 recovery record, got " + std::to_string(recs.size());
        if (outPath.empty()) emitReport(stdout);
        else { FILE* f = fopen(outPath.c_str(), "w"); if (f) { emitReport(f); fclose(f); } }
        return 1;
    }
    if (!recs[0].valid)
    {
        ok = false;
        failReason = "recovery record invalid: " + recs[0].diagnostic;
        if (outPath.empty()) emitReport(stdout);
        else { FILE* f = fopen(outPath.c_str(), "w"); if (f) { emitReport(f); fclose(f); } }
        return 1;
    }

    SceneDocument restored;
    DeterministicUuidProvider provider2;
    restored.SetUuidProvider(&provider2);
    std::vector<AssetDiagnostic> diags;
    if (!svc2.Restore(recs[0], restored, diags, err))
    {
        ok = false;
        failReason = "restore failed: " + err.Format();
        if (outPath.empty()) emitReport(stdout);
        else { FILE* f = fopen(outPath.c_str(), "w"); if (f) { emitReport(f); fclose(f); } }
        return 1;
    }

    // 7. Verify the edit is present.
    {
        auto view = restored.ecs.registry.view<EntityIdComponent>();
        bool foundUuid = false;
        bool transformOk = false;
        for (auto ent : view)
        {
            const auto& idc = view.get<EntityIdComponent>(ent);
            if (idc.id == originalUuid)
            {
                foundUuid = true;
                if (auto* t = restored.ecs.registry.try_get<Transform>(ent))
                {
                    transformOk = (glm::length(t->translation - glm::vec3{5.0f, 0.0f, 0.0f}) < 0.001f);
                }
            }
        }
        if (!foundUuid)
        {
            ok = false;
            failReason = "restored UUID does not match original";
        }
        if (!transformOk)
        {
            ok = false;
            failReason = "restored transform does not match the edit (expected 5,0,0)";
        }
    }

    const entt::entity restoredEntity = restored.FindByUuid(originalUuid);
    if (restoredEntity == entt::null ||
        !restored.ecs.registry.all_of<ImportedMeshSourceComponent,
                                      MaterialOverrideComponent>(restoredEntity))
    {
        ok = false;
        failReason = "import provenance or material override was not restored";
    }
    else
    {
        const auto& materialOverride =
            restored.ecs.registry.get<MaterialOverrideComponent>(restoredEntity);
        if (!materialOverride.authored ||
            std::abs(materialOverride.material.roughness - 0.123f) > 0.0001f)
        {
            ok = false;
            failReason = "authored material override was not restored";
        }
    }
    if (restored.ecs.meshRegistry.GetCount() == 0 ||
        restored.ecs.textures.empty() ||
        restored.environment.floatPixels.empty() ||
        restored.environment.width <= 0 || restored.environment.height <= 0)
    {
        ok = false;
        failReason = "asset-backed mesh, texture, or environment payload was not restored";
    }

    // Verify the explicit file is byte-for-byte unchanged.
    std::string explicitAfter;
    {
        std::ifstream in(scenePath, std::ios::binary);
        std::stringstream ss; ss << in.rdbuf();
        explicitAfter = ss.str();
    }
    if (explicitBefore != explicitAfter)
    {
        ok = false;
        failReason = "explicit scene file was modified by recovery";
    }

    // Verify the restored doc is dirty.
    if (!restored.metadata.dirty)
    {
        ok = false;
        failReason = "restored document is not dirty";
    }

    // 8. Discard the recovery record; verify it's gone.
    svc2.Discard(recs[0], err);
    {
        SceneRecoveryService svc3(recoveryRoot,
            [&fakeNow]() { return fakeNow; }, 8, 60.0);
        auto recs2 = svc3.Discover(err);
        if (!recs2.empty())
        {
            ok = false;
            failReason = "recovery record not discarded";
        }
    }

    // 9. Emit result.
    if (outPath.empty())
        emitReport(stdout);
    else
    {
        FILE* f = fopen(outPath.c_str(), "w");
        if (f) { emitReport(f); fclose(f); }
    }

    // Cleanup.
    std::error_code ec;
    fs::remove_all(workDir, ec);

    if (ok)
    {
        printf("[RecoveryScenario] PASS\n");
        return 0;
    }
    else
    {
        fprintf(stderr, "[RecoveryScenario] FAIL: %s\n", failReason.c_str());
        return 1;
    }
}

// ============================================================================
// Phase 6C/W7: Headless script scenario
// ============================================================================
//
// Loads a scene, wires up ScriptSystem + RuntimeCommandSink + a null input
// service, plays it for N fixed steps, then compares entity transforms
// against expected values from the scenario JSON. This proves the full
// script lifecycle (on_create → on_fixed_update → on_update) runs end-to-
// end without the editor, GPU, or efsw. The scenario JSON schema is:
//
//   {
//     "scenePath":   "assets/script-scenario.rt2scene",
//     "frames":      60,
//     "uuidSeed":    42,
//     "forbidSpawn": false,
//     "expectedTransforms": {
//       "<uuid-string>": {
//         "position": [x, y, z],
//         "rotation": [x, y, z, w],
//         "scale":    [x, y, z]
//       }
//     }
//   }
//
// Exit codes are ScenarioExit (ScriptScenarioCompare.h) — that enum is the
// contract, this comment is a convenience copy:
//
//   0  pass                 4  --out could not be opened
//   1  scenario JSON bad    5  expectation failed (mismatch/missing/spawn)
//   2  scene load failed    6  script error (quarantined / none survived)
//   3  Play failed
//
// The comparison logic itself lives in ScriptScenarioCompare.h as pure
// functions over plain structs, so RT2Tests can exercise it without a scene.

static int RunScriptScenario(const std::string& scenarioPath,
                             const std::string& outPath)
{
    namespace fs = std::filesystem;

    // Exit codes come from ScenarioExit (ScriptScenarioCompare.h), which is
    // the single source of truth shared with the docs and run_script_test.ps1.
    auto exitCode = [](ScenarioExit e) { return static_cast<int>(e); };

    // --- Load and parse the scenario JSON ---
    std::string scenarioContent;
    {
        std::ifstream f(scenarioPath);
        if (!f)
        {
            fprintf(stderr, "[ScriptScenario] Cannot open scenario: %s\n",
                    scenarioPath.c_str());
            return exitCode(ScenarioExit::ScenarioParse);
        }
        std::stringstream ss; ss << f.rdbuf();
        scenarioContent = ss.str();
    }

    nlohmann::json scenario;
    try
    {
        scenario = nlohmann::json::parse(scenarioContent);
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "[ScriptScenario] JSON parse error: %s\n", e.what());
        return exitCode(ScenarioExit::ScenarioParse);
    }

    auto warnType = [&](const char* key, const char* expected) {
        if (scenario.contains(key) && !scenario[key].is_null())
            fprintf(stderr, "[ScriptScenario] Warning: \"%s\" is present "
                    "but not %s; using default\n", key, expected);
    };
    auto getString = [&](const char* key,
                         const std::string& def) -> std::string {
        if (scenario.contains(key) && scenario[key].is_string())
            return scenario[key].get<std::string>();
        warnType(key, "a string");
        return def;
    };
    auto getInt = [&](const char* key, int def) -> int {
        if (scenario.contains(key) && scenario[key].is_number_integer())
            return scenario[key].get<int>();
        warnType(key, "an integer");
        return def;
    };
    auto getBool = [&](const char* key, bool def) -> bool {
        if (scenario.contains(key) && scenario[key].is_boolean())
            return scenario[key].get<bool>();
        warnType(key, "a boolean");
        return def;
    };

    // Resolve scenePath relative to the scenario's directory.
    fs::path scenarioDir = fs::path(scenarioPath).parent_path();
    std::string scenePathStr = getString("scenePath", "");
    if (scenePathStr.empty())
    {
        fprintf(stderr, "[ScriptScenario] Missing \"scenePath\" in scenario\n");
        return exitCode(ScenarioExit::ScenarioParse);
    }
    std::error_code pathEc;
    fs::path scenePath = fs::weakly_canonical(
        scenarioDir / scenePathStr, pathEc);
    if (pathEc)
    {
        fprintf(stderr, "[ScriptScenario] Cannot resolve scene path: %s\n",
                pathEc.message().c_str());
        return exitCode(ScenarioExit::SceneLoad);
    }

    int frames = std::max(1, getInt("frames", 60));
    uint64_t uuidSeed = static_cast<uint64_t>(
        std::max(0, getInt("uuidSeed", 42)));
    bool forbidSpawn = getBool("forbidSpawn", false);

    // --- Load the scene ---
    // Build a UUID seed from the integer seed (low 64 bits).
    std::array<uint8_t, 16> seedBytes{};
    for (int i = 0; i < 8; ++i)
        seedBytes[i] = static_cast<uint8_t>(
            (uuidSeed >> (i * 8)) & 0xFF);
    UUID seedUuid(seedBytes);
    DeterministicUuidProvider uuidProv(seedUuid);
    SceneDocument authoring;
    authoring.SetUuidProvider(&uuidProv);

    Error err;
    SceneLoadReport loadReport;
    if (!SceneSerializer::Load(authoring, scenePath, loadReport, err))
    {
        fprintf(stderr, "[ScriptScenario] Scene load failed: %s\n",
                err.Format().c_str());
        return exitCode(ScenarioExit::SceneLoad);
    }
    for (const auto& d : loadReport.fieldDiagnostics)
        fprintf(stderr, "[ScriptScenario] Field diagnostic: %s\n",
                d.message.c_str());

    // --- Wire up the script system ---
    NullSceneRenderBridge bridge;
    RuntimeSceneController ctrl;
    AssetResolutionContext scriptAssetContext{
        scenePath.parent_path(), nullptr};
    std::vector<AssetDiagnostic> scriptAssetDiagnostics;
    ScriptSystem scriptSys(
        uuidProv, scriptAssetContext, scriptAssetDiagnostics);
    RuntimeCommandSink sink(ctrl);
    NullInputService input;

    ctrl.SetRuntimeUuidProvider(&uuidProv);
    ctrl.SetLifecycleObserver(&scriptSys);
    ctrl.SetScriptDispatch(&scriptSys);
    ctrl.SetInputService(&input);
    ctrl.SetRuntimeCommandSink(&sink);

    // --- Capture authoring entity count as the forbidSpawn baseline ---
    size_t authoringEntityCount = 0;
    {
        auto view = authoring.ecs.registry.view<EntityIdComponent>();
        authoringEntityCount = view.size();
    }

    // --- Play ---
    if (!ctrl.Play(authoring, bridge, err))
    {
        fprintf(stderr, "[ScriptScenario] Play failed: %s\n",
                err.Format().c_str());
        return exitCode(ScenarioExit::PlayFailed);
    }

    // --- Run N fixed steps at kFixedDt (deterministic) ---
    for (int i = 0; i < frames; ++i)
        ctrl.Update(kFixedDt, bridge);

    for (const auto& diagnostic : scriptAssetDiagnostics)
    {
        fprintf(stderr,
                "[ScriptScenario] Asset diagnostic: severity=%d ref=%s "
                "entity=%s detail=%s\n",
                static_cast<int>(diagnostic.severity),
                diagnostic.refPath.c_str(),
                diagnostic.entityUuid.ToString().c_str(),
                diagnostic.detail.c_str());
    }

    // --- Capture runtime entity state ---
    std::vector<ScenarioEntityState> entities;
    const SceneDocument* runtime = ctrl.TryGetRuntimeScene();
    if (runtime)
    {
        auto view = runtime->ecs.registry.view<EntityIdComponent>();
        for (auto e : view)
        {
            const auto& idc = view.get<EntityIdComponent>(e);
            ScenarioEntityState r;
            r.uuid = idc.id.ToString();
            if (auto* nc = runtime->ecs.registry.try_get<NameComponent>(e))
                r.name = nc->name;
            if (auto* tf = runtime->ecs.registry.try_get<Transform>(e))
            {
                r.translation = tf->translation;
                r.rotation    = tf->rotation;
                r.scale       = tf->scale;
            }
            if (auto* vc = runtime->ecs.registry.try_get<VisibleComponent>(e))
                r.visible = vc->visible;
            entities.push_back(r);
        }
    }

    // --- Assemble the verdict (pure logic lives in ScriptScenarioCompare.h) ---
    ScenarioResult result;
    result.liveInstances        = scriptSys.LiveInstanceCount();
    result.quarantinedInstances = scriptSys.QuarantinedInstanceCount();
    result.authoringEntityCount = authoringEntityCount;
    result.runtimeEntityCount   = entities.size();
    result.scriptError = DetectScriptError(result.liveInstances,
                                           result.quarantinedInstances,
                                           result.runtimeEntityCount);
    result.spawnViolation = DetectSpawnViolation(forbidSpawn,
                                                 result.runtimeEntityCount,
                                                 authoringEntityCount);

    // --- Stop ---
    ctrl.Stop(authoring, bridge);

    // --- Parse expectedTransforms into plain structs, then compare ---
    std::vector<ScenarioExpectation> expectations;
    if (scenario.contains("expectedTransforms") &&
        scenario["expectedTransforms"].is_object())
    {
        for (const auto& [uuid, exp] : scenario["expectedTransforms"].items())
        {
            ScenarioExpectation e;
            e.uuid = uuid;

            if (exp.contains("position"))
            {
                const auto& ep = exp["position"];
                if (ep.is_array() && ep.size() >= 3)
                {
                    e.hasPosition = true;
                    e.position = glm::vec3(ep[0].get<float>(),
                                           ep[1].get<float>(),
                                           ep[2].get<float>());
                }
            }
            if (exp.contains("rotation"))
            {
                const auto& er = exp["rotation"];
                if (er.is_array() && er.size() >= 4)
                {
                    e.hasRotation = true;
                    // Scenario JSON stores [x, y, z, w]; glm::quat takes
                    // (w, x, y, z).
                    e.rotation = glm::quat(er[3].get<float>(),
                                           er[0].get<float>(),
                                           er[1].get<float>(),
                                           er[2].get<float>());
                }
            }
            if (exp.contains("scale"))
            {
                const auto& es = exp["scale"];
                if (es.is_array() && es.size() >= 3)
                {
                    e.hasScale = true;
                    e.scale = glm::vec3(es[0].get<float>(),
                                        es[1].get<float>(),
                                        es[2].get<float>());
                }
            }
            expectations.push_back(std::move(e));
        }
    }

    result.mismatches = CompareTransforms(entities, expectations);
    const auto& mismatches = result.mismatches;

    // --- Emit JSON report ---
    auto emitReport = [&](FILE* out) {
        fprintf(out, "{\n");
        fprintf(out, "  \"scenario\": \"%s\",\n", scenarioPath.c_str());
        fprintf(out, "  \"scene\": \"%s\",\n", scenePath.string().c_str());
        fprintf(out, "  \"frames\": %d,\n", frames);
        fprintf(out, "  \"forbidSpawn\": %s,\n", forbidSpawn ? "true" : "false");
        fprintf(out, "  \"spawnViolation\": %s,\n",
                result.spawnViolation ? "true" : "false");
        fprintf(out, "  \"scriptError\": %s,\n",
                result.scriptError ? "true" : "false");
        fprintf(out, "  \"liveInstances\": %zu,\n", result.liveInstances);
        fprintf(out, "  \"quarantinedInstances\": %zu,\n",
                result.quarantinedInstances);
        fprintf(out, "  \"exitCode\": %d,\n",
                static_cast<int>(result.Exit()));
        fprintf(out, "  \"mismatchCount\": %zu,\n", mismatches.size());
        if (!mismatches.empty())
        {
            fprintf(out, "  \"mismatches\": [\n");
            for (size_t i = 0; i < mismatches.size(); ++i)
            {
                fprintf(out, "    {\n");
                fprintf(out, "      \"uuid\": \"%s\",\n",
                        mismatches[i].uuid.c_str());
                fprintf(out, "      \"field\": \"%s\",\n",
                        mismatches[i].field.c_str());
                fprintf(out, "      \"expected\": \"%s\",\n",
                        mismatches[i].expected.c_str());
                fprintf(out, "      \"actual\": \"%s\"\n",
                        mismatches[i].actual.c_str());
                fprintf(out, "    }%s\n",
                        (i + 1 < mismatches.size()) ? "," : "");
            }
            fprintf(out, "  ],\n");
        }
        fprintf(out, "  \"entities\": [\n");
        for (size_t i = 0; i < entities.size(); ++i)
        {
            fprintf(out, "    {\n");
            fprintf(out, "      \"uuid\": \"%s\",\n",
                    entities[i].uuid.c_str());
            fprintf(out, "      \"name\": \"%s\",\n",
                    entities[i].name.c_str());
            fprintf(out, "      \"visible\": %s,\n",
                    entities[i].visible ? "true" : "false");
            fprintf(out, "      \"position\": [%.9g, %.9g, %.9g],\n",
                    entities[i].translation.x, entities[i].translation.y,
                    entities[i].translation.z);
            fprintf(out, "      \"rotation\":    [%.9g, %.9g, %.9g, %.9g],\n",
                    entities[i].rotation.x, entities[i].rotation.y,
                    entities[i].rotation.z, entities[i].rotation.w);
            fprintf(out, "      \"scale\":       [%.9g, %.9g, %.9g]\n",
                    entities[i].scale.x, entities[i].scale.y,
                    entities[i].scale.z);
            fprintf(out, "    }%s\n",
                    (i + 1 < entities.size()) ? "," : "");
        }
        fprintf(out, "  ]\n");
        fprintf(out, "}\n");
    };

    if (outPath.empty())
        emitReport(stdout);
    else
    {
        FILE* f = fopen(outPath.c_str(), "w");
        if (!f)
        {
            fprintf(stderr, "[ScriptScenario] Cannot open output: %s\n",
                    outPath.c_str());
            return exitCode(ScenarioExit::OutputFailed);
        }
        emitReport(f);
        fclose(f);
    }

    // --- Exit code ---
    if (result.Pass())
    {
        printf("[ScriptScenario] PASS: %d frames, %zu entities, "
               "no mismatches\n", frames, entities.size());
        return exitCode(ScenarioExit::Pass);
    }

    if (result.scriptError)
        fprintf(stderr, "[ScriptScenario] FAIL: script error "
                "(live=%zu, quarantined=%zu)\n",
                result.liveInstances, result.quarantinedInstances);
    if (result.spawnViolation)
        fprintf(stderr, "[ScriptScenario] FAIL: spawn violation "
                "(forbidSpawn=true, entities %zu > authoring %zu)\n",
                result.runtimeEntityCount, result.authoringEntityCount);
    for (const auto& m : mismatches)
        fprintf(stderr, "[ScriptScenario] FAIL: %s %s mismatch "
                "(expected %s, got %s)\n",
                m.uuid.c_str(), m.field.c_str(),
                m.expected.c_str(), m.actual.c_str());
    return exitCode(result.Exit());
}

int main(int argc, char** argv)
{
    std::string scenePath;
    std::string projectPath;
    int steps = 60;
    std::string outPath;
    bool recoveryScenario = false;
    std::string scriptScenarioPath;

    for (int i = 1; i < argc; ++i)
    {
        const char* a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 < argc) return argv[++i];
            return nullptr;
        };

        if (std::strcmp(a, "--scene") == 0)
        {
            if (const char* v = next()) scenePath = v;
        }
        else if (std::strcmp(a, "--project") == 0)
        {
            if (const char* v = next()) projectPath = v;
        }
        else if (std::strcmp(a, "--steps") == 0)
        {
            if (const char* v = next()) steps = std::max(1, std::atoi(v));
        }
        else if (std::strcmp(a, "--out") == 0)
        {
            if (const char* v = next()) outPath = v;
        }
        else if (std::strcmp(a, "--recovery-scenario") == 0)
        {
            recoveryScenario = true;
        }
        else if (std::strcmp(a, "--script-scenario") == 0)
        {
            if (const char* v = next()) scriptScenarioPath = v;
        }
        else if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-?") == 0)
        {
            PrintUsage();
            return 0;
        }
        else
        {
            fprintf(stderr, "[SliceRunner] Unknown argument: %s\n", a);
        }
    }

    if (recoveryScenario)
        return RunRecoveryScenario(outPath);

    if (!scriptScenarioPath.empty())
        return RunScriptScenario(scriptScenarioPath, outPath);

    ProjectContext projectContext;
    AssetResolutionContext assetContext;
    if (!projectPath.empty())
    {
        Error projectErr;
        if (!LoadProjectContext(projectPath, projectContext, projectErr))
        {
            fprintf(stderr, "[SliceRunner] Failed to load project: %s\n",
                    projectErr.Format().c_str());
            return 1;
        }
        if (scenePath.empty())
        {
            if (projectContext.project.startupScene.empty())
            {
                fprintf(stderr,
                    "[SliceRunner] Project has no startup scene; pass --scene\n");
                return 1;
            }
            scenePath = projectContext.project.startupScene.u8string();
        }
        else
        {
            const std::filesystem::path locator =
                std::filesystem::u8path(scenePath);
            bool escapes = false;
            for (const auto& part : locator)
                escapes = escapes || part == "..";
            if (locator.is_absolute() || locator.has_root_name() ||
                locator.has_root_directory() || escapes)
            {
                fprintf(stderr,
                    "[SliceRunner] --scene must be contained and relative when --project is used\n");
                return 1;
            }
            scenePath = (projectContext.project.assetRoot / locator).
                lexically_normal().u8string();
        }
        assetContext = projectContext.Assets();
    }

    if (scenePath.empty())
    {
        fprintf(stderr, "[SliceRunner] --scene is required\n");
        PrintUsage();
        return 1;
    }

    if (projectPath.empty())
    {
        std::error_code pathError;
        const auto absoluteScene = std::filesystem::absolute(
            std::filesystem::u8path(scenePath), pathError).lexically_normal();
        if (pathError)
        {
            fprintf(stderr, "[SliceRunner] Cannot resolve scene path: %s\n",
                    pathError.message().c_str());
            return 1;
        }
        scenePath = absoluteScene.u8string();
        assetContext = AssetResolutionContext{
            absoluteScene.parent_path(), nullptr};
    }

    // --- Load the scene ---
    DeterministicUuidProvider provider;
    SceneDocument authoring;
    authoring.SetUuidProvider(&provider);

    Error err;
    SceneLoadReport loadReport;
    if (!SceneSerializer::Load(authoring, scenePath, loadReport, err))
    {
        fprintf(stderr, "[SliceRunner] Failed to load scene: %s\n", err.Format().c_str());
        return 2;
    }
    std::vector<AssetDiagnostic> assetDiagnostics;
    if (!SceneAssetResolver::ResolveAll(
            authoring, assetContext, assetDiagnostics, err))
    {
        fprintf(stderr, "[SliceRunner] Asset resolution failed: %s\n",
                err.Format().c_str());
        return 2;
    }
    for (const auto& diagnostic : loadReport.fieldDiagnostics)
        fprintf(stderr, "[SliceRunner] Script field diagnostic: %s\n",
                diagnostic.message.c_str());
    if (loadReport.droppedScriptFieldData)
    {
        fprintf(stderr,
            "[SliceRunner] Refusing scene because malformed script fields were dropped\n");
        return 2;
    }

    // --- Snapshot the authoring transforms before Play ---
    struct TransformSnapshot
    {
        UUID uuid;
        std::string name;
        glm::vec3 translation;
        glm::quat rotation;
        glm::vec3 scale;
    };

    std::vector<TransformSnapshot> authBefore;
    {
        auto view = authoring.ecs.registry.view<EntityIdComponent>();
        for (auto e : view)
        {
            const auto& idc = view.get<EntityIdComponent>(e);
            TransformSnapshot s;
            s.uuid = idc.id;
            if (auto* nc = authoring.ecs.registry.try_get<NameComponent>(e))
                s.name = nc->name;
            if (auto* tf = authoring.ecs.registry.try_get<Transform>(e))
            {
                s.translation = tf->translation;
                s.rotation    = tf->rotation;
                s.scale       = tf->scale;
            }
            authBefore.push_back(s);
        }
    }

    // --- Play ---
    NullSceneRenderBridge bridge;
    RuntimeSceneController ctrl;

    if (!ctrl.Play(authoring, bridge, err))
    {
        fprintf(stderr, "[SliceRunner] Play failed: %s\n", err.Format().c_str());
        return 3;
    }

    // --- Run N fixed steps ---
    for (int i = 0; i < steps; ++i)
        ctrl.Update(kFixedDt, bridge);

    // --- Capture runtime transforms ---
    std::vector<TransformSnapshot> rtAfter;
    {
    const SceneDocument* runtime = ctrl.TryGetRuntimeScene();
    if (runtime)
    {
        auto view = runtime->ecs.registry.view<EntityIdComponent>();
        for (auto e : view)
        {
            const auto& idc = view.get<EntityIdComponent>(e);
            TransformSnapshot s;
            s.uuid = idc.id;
            if (auto* nc = runtime->ecs.registry.try_get<NameComponent>(e))
                s.name = nc->name;
            if (auto* tf = runtime->ecs.registry.try_get<Transform>(e))
            {
                s.translation = tf->translation;
                s.rotation    = tf->rotation;
                s.scale       = tf->scale;
            }
            rtAfter.push_back(s);
        }
    }
    }

    // --- Stop ---
    ctrl.Stop(authoring, bridge);

    // --- Verify authoring transforms are unchanged ---
    bool authoringIntact = true;
    {
        auto view = authoring.ecs.registry.view<EntityIdComponent>();
        for (auto e : view)
        {
            const auto& idc = view.get<EntityIdComponent>(e);
            auto* tf = authoring.ecs.registry.try_get<Transform>(e);
            if (!tf) continue;

            // Find the matching snapshot
            for (const auto& before : authBefore)
            {
                if (before.uuid == idc.id)
                {
                    float eps = 1e-6f;
                    if (glm::length(tf->translation - before.translation) > eps ||
                        glm::length(tf->rotation - before.rotation) > eps ||
                        glm::length(tf->scale - before.scale) > eps)
                    {
                        authoringIntact = false;
                        fprintf(stderr, "[SliceRunner] Authoring transform changed for entity %s!\n",
                                before.name.c_str());
                    }
                    break;
                }
            }
        }
    }

    // --- Emit JSON report ---
    auto emitReport = [&](FILE* out) {
        fprintf(out, "{\n");
        fprintf(out, "  \"scene\": \"%s\",\n", scenePath.c_str());
        fprintf(out, "  \"steps\": %d,\n", steps);
        fprintf(out, "  \"authoringIntact\": %s,\n", authoringIntact ? "true" : "false");
        fprintf(out, "  \"bridge\": {\n");
        fprintf(out, "    \"fullSyncs\": %d,\n", bridge.fullSyncs);
        fprintf(out, "    \"transformSyncs\": %d,\n", bridge.transformSyncs);
        fprintf(out, "    \"renders\": %d\n", bridge.renders);
        fprintf(out, "  },\n");
        fprintf(out, "  \"runtimeTransforms\": [\n");
        for (size_t i = 0; i < rtAfter.size(); ++i)
        {
            fprintf(out, "  {\n");
            fprintf(out, "    \"uuid\": \"%s\",\n", rtAfter[i].uuid.ToString().c_str());
            fprintf(out, "    \"name\": \"%s\",\n", rtAfter[i].name.c_str());
            PrintTransformJSON(out, rtAfter[i].translation, rtAfter[i].rotation, rtAfter[i].scale);
            fprintf(out, "  }%s\n", (i + 1 < rtAfter.size()) ? "," : "");
        }
        fprintf(out, "  ]\n");
        fprintf(out, "}\n");
    };

    if (outPath.empty())
        emitReport(stdout);
    else
    {
        FILE* f = fopen(outPath.c_str(), "w");
        if (!f)
        {
            fprintf(stderr, "[SliceRunner] Failed to open output file: %s\n", outPath.c_str());
            return 4;
        }
        emitReport(f);
        fclose(f);
    }

    // --- Exit code ---
    if (!authoringIntact)
    {
        fprintf(stderr, "[SliceRunner] FAIL: authoring scene was modified during Play\n");
        return 5;
    }

    printf("[SliceRunner] PASS: %d steps, authoring intact\n", steps);
    return 0;
}
