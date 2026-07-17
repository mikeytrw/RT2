#include "SceneSerializer.h"
#include "SceneDocument.h"
#include "RuntimeSceneController.h"
#include "ISceneRenderBridge.h"
#include "ECSComponents.h"
#include "ECSScene.h"
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
    printf("  --scene <path>    .rt2scene file to load and run\n");
    printf("  --steps <N>       Number of fixed update steps (default 60)\n");
    printf("  --out <path>      Write JSON report to file instead of stdout\n");
    printf("  --help            Show this help\n");
}

int main(int argc, char** argv)
{
    std::string scenePath;
    int steps = 60;
    std::string outPath;

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