#pragma once

#ifndef RT2_CORE_SCRIPT_SCENARIO_COMPARE_H
#define RT2_CORE_SCRIPT_SCENARIO_COMPARE_H

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

// ============================================================================
// ScriptScenarioCompare — Phase 6C/W7 headless scenario verdict logic.
//
// The pure decision core of RT2SliceRunner's --script-scenario mode, lifted
// out of Main.cpp so it is reachable from RT2Tests. Main.cpp keeps the JSON
// parsing, the Play loop, and the report writer; everything here is a pure
// function over plain structs — no JSON, no ECS, no I/O, no globals.
//
// ScenarioExit is the SINGLE SOURCE OF TRUTH for the runner's exit codes.
// docs/scripting.md and run_script_test.ps1 must agree with it; if a new
// failure mode needs a code, add it here first.
// ============================================================================

namespace rt2::core {

enum class ScenarioExit : int
{
    Pass            = 0,  // ran to completion, every expectation met
    ScenarioParse   = 1,  // scenario JSON missing, unreadable, or malformed
    SceneLoad       = 2,  // scenePath unresolvable, or SceneSerializer::Load failed
    PlayFailed      = 3,  // RuntimeSceneController::Play returned false
    OutputFailed    = 4,  // --out path could not be opened for writing
    // Ran to completion, but a declared expectation did not hold: a
    // transform mismatch, a missing entity, or a spawn violation. Named for
    // the category rather than one member — a spawn violation is not a
    // transform mismatch, and a log line saying so misdiagnoses the failure.
    // The report's structured fields say which it was.
    ExpectationFailed = 5,
    ScriptError       = 6,  // an instance quarantined, or none survived the run
};

// Default comparison tolerances.
//
// These measure DIFFERENT quantities and deliberately do not share a value:
//
//  - kPositionEpsilon is a Euclidean distance in world units.
//  - kRotationEpsilon is a deviation in |dot| between unit quaternions. The
//    orientation angle is theta = 2 * acos(|dot|), so 1e-4 here admits
//    2 * acos(0.9999) ~= 0.028 rad ~= 1.6 degrees. Tighten it if a scenario
//    needs sub-degree rotation precision.
constexpr float kPositionEpsilon = 1e-4f;
constexpr float kRotationEpsilon = 1e-4f;

// One entity's post-run state, captured from the runtime document.
struct ScenarioEntityState
{
    std::string uuid;
    std::string name;
    glm::vec3   translation{0.0f, 0.0f, 0.0f};
    glm::quat   rotation{1.0f, 0.0f, 0.0f, 0.0f};  // (w, x, y, z)
    glm::vec3   scale{1.0f, 1.0f, 1.0f};
    bool        visible = true;
};

// One entry from the scenario's "expectedTransforms" map. Each field is
// independently optional: a scenario may pin position only, or all three.
struct ScenarioExpectation
{
    std::string uuid;
    bool        hasPosition = false;
    glm::vec3   position{0.0f, 0.0f, 0.0f};
    bool        hasRotation = false;
    glm::quat   rotation{1.0f, 0.0f, 0.0f, 0.0f};
    bool        hasScale = false;
    glm::vec3   scale{1.0f, 1.0f, 1.0f};
};

struct ScenarioMismatch
{
    std::string uuid;
    std::string field;      // "entity", "position", "rotation", or "scale"
    std::string expected;
    std::string actual;
};

// The full verdict. Main.cpp fills this in, then asks it for the exit code
// so the precedence rule lives in one place.
struct ScenarioResult
{
    std::vector<ScenarioMismatch> mismatches;
    bool   spawnViolation       = false;
    bool   scriptError          = false;
    size_t liveInstances        = 0;
    size_t quarantinedInstances = 0;
    size_t authoringEntityCount = 0;
    size_t runtimeEntityCount   = 0;

    bool Pass() const
    {
        return mismatches.empty() && !spawnViolation && !scriptError;
    }

    // A script error outranks a failed expectation: if the script died, the
    // transforms it failed to write are a symptom, not the diagnosis.
    ScenarioExit Exit() const
    {
        if (scriptError) return ScenarioExit::ScriptError;
        if (!mismatches.empty() || spawnViolation)
            return ScenarioExit::ExpectationFailed;
        return ScenarioExit::Pass;
    }
};

inline std::string FormatVec3(const glm::vec3& v)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "[%.6f, %.6f, %.6f]", v.x, v.y, v.z);
    return buf;
}

inline std::string FormatQuat(const glm::quat& q)
{
    char buf[80];
    snprintf(buf, sizeof(buf), "[%.6f, %.6f, %.6f, %.6f]", q.x, q.y, q.z, q.w);
    return buf;
}

// Compare captured entity state against the scenario's expectations.
//
// Driven by the EXPECTATION list, not the entity list: an expectation naming
// a UUID that is absent from the run is reported as a mismatch on the
// pseudo-field "entity". Iterating entities instead would silently pass a
// scenario whose subject the script destroyed.
//
// Position and scale use Euclidean distance, so `posEps` is a distance in
// world units, not a per-component tolerance. Rotation is compared via |dot|
// to absorb quaternion double-cover (q and -q are the same orientation);
// `rotEps` is therefore a dot-product deviation, NOT an angle and NOT
// commensurable with `posEps` — see kRotationEpsilon above for the
// conversion.
inline std::vector<ScenarioMismatch> CompareTransforms(
    const std::vector<ScenarioEntityState>& actual,
    const std::vector<ScenarioExpectation>& expected,
    float posEps = kPositionEpsilon,
    float rotEps = kRotationEpsilon)
{
    std::vector<ScenarioMismatch> mismatches;

    for (const auto& exp : expected)
    {
        const ScenarioEntityState* ent = nullptr;
        for (const auto& a : actual)
        {
            if (a.uuid == exp.uuid) { ent = &a; break; }
        }

        if (!ent)
        {
            mismatches.push_back({exp.uuid, "entity", "present", "<missing>"});
            continue;
        }

        if (exp.hasPosition &&
            glm::length(ent->translation - exp.position) > posEps)
        {
            mismatches.push_back({exp.uuid, "position",
                                  FormatVec3(exp.position),
                                  FormatVec3(ent->translation)});
        }

        if (exp.hasRotation)
        {
            const float dot = glm::dot(ent->rotation, exp.rotation);
            if (std::abs(1.0f - std::abs(dot)) > rotEps)
            {
                mismatches.push_back({exp.uuid, "rotation",
                                      FormatQuat(exp.rotation),
                                      FormatQuat(ent->rotation)});
            }
        }

        if (exp.hasScale && glm::length(ent->scale - exp.scale) > posEps)
        {
            mismatches.push_back({exp.uuid, "scale",
                                  FormatVec3(exp.scale),
                                  FormatVec3(ent->scale)});
        }
    }

    return mismatches;
}

// A run is a script error if anything quarantined, or if every instance
// vanished while entities remain — the latter catches a scenario whose
// script never loaded at all, which would otherwise report a clean pass
// with every transform sitting at its authored value.
inline bool DetectScriptError(size_t liveInstances,
                              size_t quarantinedInstances,
                              size_t runtimeEntityCount)
{
    return quarantinedInstances > 0 ||
           (liveInstances == 0 && runtimeEntityCount > 0);
}

// forbidSpawn is a ratchet against the AUTHORING count, not against the
// count at the previous frame: a script that spawns and then destroys still
// violates it only if it ends up ahead.
inline bool DetectSpawnViolation(bool forbidSpawn,
                                 size_t runtimeEntityCount,
                                 size_t authoringEntityCount)
{
    return forbidSpawn && runtimeEntityCount > authoringEntityCount;
}

} // namespace rt2::core

#endif // RT2_CORE_SCRIPT_SCENARIO_COMPARE_H
