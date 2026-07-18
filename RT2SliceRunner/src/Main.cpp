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

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cmath>

using namespace rt2::core;

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
    printf("Usage: RT2SliceRunner --scene <path.rt2scene> [--steps <N>] [--out <report.json>]\n");
    printf("Options:\n");
    printf("  --scene <path>         .rt2scene file to load and run\n");
    printf("  --steps <N>            Number of fixed update steps (default 60)\n");
    printf("  --out <path>           Write JSON report to file instead of stdout\n");
    printf("  --recovery-scenario    Run the Phase 1B recovery regression scenario\n");
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
    if (!SceneSerializer::Save(doc, scenePath, err))
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
    if (svc.MaybeSnapshot(doc, revision, "unused", workDir, err))
    {
        ok = false;
        failReason = "autosave wrote before the configured interval";
        if (outPath.empty()) emitReport(stdout);
        else { FILE* f = fopen(outPath.c_str(), "w"); if (f) { emitReport(f); fclose(f); } }
        return 1;
    }
    fakeNow += 60;
    if (!svc.MaybeSnapshot(doc, revision, "unused", workDir, err))
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

int main(int argc, char** argv)
{
    std::string scenePath;
    int steps = 60;
    std::string outPath;
    bool recoveryScenario = false;

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

    if (scenePath.empty())
    {
        fprintf(stderr, "[SliceRunner] --scene is required\n");
        PrintUsage();
        return 1;
    }

    // --- Load the scene ---
    DeterministicUuidProvider provider;
    SceneDocument authoring;
    authoring.SetUuidProvider(&provider);

    Error err;
    if (!SceneSerializer::Load(authoring, scenePath, err))
    {
        fprintf(stderr, "[SliceRunner] Failed to load scene: %s\n", err.Format().c_str());
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
