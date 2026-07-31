#pragma once

#ifndef RT2_CORE_INPUT_CONFIG_H
#define RT2_CORE_INPUT_CONFIG_H

#include "InputTypes.h"
#include "core/Error.h"
#include "json.hpp"

#include <string>
#include <vector>

namespace rt2::core {

struct InputContextRecord
{
    std::string contextId;
    std::vector<InputMapping> mappings;
};

enum class InputConfigScope
{
    BuiltIn,
    ProjectDefaults,
    UserOverrides,
};

// Parse and validate the portable input-context array shared by
// project.rt2proj and per-user editor settings. Invalid or duplicate records
// fail loudly; callers never choose a duplicate by insertion order. Project
// defaults may not target editor-owned contexts. UserOverrides accept
// editor-owned contexts when explicitly authored in v3; the v2 migration path
// separately drops those records because v2 inputContexts were inert and must
// not become live during upgrade.
bool ParseInputContextRecords(const nlohmann::json& value,
                              InputConfigScope scope,
                              std::vector<InputContextRecord>& out,
                              Error& err);

nlohmann::json InputContextRecordsToJson(
    const std::vector<InputContextRecord>& records);

// Built-ins -> project defaults -> user overrides. Empty bindings are an
// explicit unbind and remove the inherited mapping. Output contexts and
// mappings are sorted, so JSON order cannot affect the result.
bool ComposeInputContexts(
    const std::vector<InputContextRecord>& builtIns,
    const std::vector<InputContextRecord>& projectDefaults,
    const std::vector<InputContextRecord>& userOverrides,
    std::vector<InputContextRecord>& out,
    Error& err);

bool IsEditorOwnedInputContext(const std::string& contextId);

} // namespace rt2::core

#endif // RT2_CORE_INPUT_CONFIG_H
