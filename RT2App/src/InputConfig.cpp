#include "InputConfig.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace rt2::core {

namespace {

bool Fail(Error& err, const std::string& path, const std::string& detail)
{
    err.code = Error::Parse;
    err.path = path;
    err.detail = detail;
    return false;
}

bool ValidDevice(int value)
{
    return value >= static_cast<int>(InputDeviceKind::KeyboardKey) &&
           value <= static_cast<int>(InputDeviceKind::GamepadAxis);
}

bool ValidSlot(int slot)
{
    return slot >= -1 && slot <= 3;
}

bool ParseMapping(const nlohmann::json& value,
                  const std::string& path,
                  InputMapping& out,
                  Error& err)
{
    if (!value.is_object() || !value.contains("name") ||
        !value["name"].is_string())
        return Fail(err, path, "input mapping must contain a string name");

    out = InputMapping{};
    out.name = value["name"].get<std::string>();
    if (out.name.empty())
        return Fail(err, path, "input mapping name must not be empty");
    if (value.contains("isAxis") && !value["isAxis"].is_boolean())
        return Fail(err, path + ".isAxis", "isAxis must be boolean");
    out.isAxis = value.value("isAxis", false);

    if (value.contains("actions") && !value["actions"].is_array())
        return Fail(err, path + ".actions", "actions must be an array");
    if (value.contains("axes") && !value["axes"].is_array())
        return Fail(err, path + ".axes", "axes must be an array");

    if (value.contains("actions"))
    {
        size_t index = 0;
        for (const auto& action : value["actions"])
        {
            const std::string itemPath = path + ".actions[" +
                std::to_string(index++) + "]";
            if (!action.is_object())
                return Fail(err, itemPath, "action binding must be an object");
            const int device = action.value("device", -1);
            const int code = action.value("code", -1);
            const int modifiers = action.value("modifiers", 0);
            const int slot = action.value("gamepadSlot", -1);
            if (!ValidDevice(device) ||
                device == static_cast<int>(InputDeviceKind::GamepadAxis))
                return Fail(err, itemPath + ".device",
                            "invalid action-binding device");
            if (code < 0 || code > 0xffff)
                return Fail(err, itemPath + ".code", "invalid action code");
            if (modifiers < 0 || (modifiers & ~0x0f) != 0)
                return Fail(err, itemPath + ".modifiers",
                            "invalid modifier bit set");
            if (!ValidSlot(slot))
                return Fail(err, itemPath + ".gamepadSlot",
                            "gamepad slot must be -1..3");

            ActionBinding binding;
            binding.device = static_cast<InputDeviceKind>(device);
            binding.code = static_cast<uint16_t>(code);
            binding.modifiers = static_cast<ModifierBits>(modifiers);
            binding.gamepadSlot = slot;
            out.actions.push_back(binding);
        }
    }

    if (value.contains("axes"))
    {
        size_t index = 0;
        for (const auto& axis : value["axes"])
        {
            const std::string itemPath = path + ".axes[" +
                std::to_string(index++) + "]";
            if (!axis.is_object())
                return Fail(err, itemPath, "axis binding must be an object");
            const int device = axis.value("device", -1);
            const int code = axis.value("code", 0);
            const int positive = axis.value("positive", 0);
            const int negative = axis.value("negative", 0);
            const int slot = axis.value("gamepadSlot", -1);
            const float deadZone = axis.value("deadZone", 0.15f);
            if (device != static_cast<int>(InputDeviceKind::KeyboardKey) &&
                device != static_cast<int>(InputDeviceKind::GamepadAxis))
                return Fail(err, itemPath + ".device",
                            "invalid axis-binding device");
            if (code < 0 || code > 0xffff || positive < 0 || positive > 0xffff ||
                negative < 0 || negative > 0xffff)
                return Fail(err, itemPath, "invalid axis code");
            if (!ValidSlot(slot))
                return Fail(err, itemPath + ".gamepadSlot",
                            "gamepad slot must be -1..3");
            if (!std::isfinite(deadZone) || deadZone < 0.0f || deadZone > 1.0f)
                return Fail(err, itemPath + ".deadZone",
                            "dead zone must be finite and in [0,1]");

            AxisBinding binding;
            binding.device = static_cast<InputDeviceKind>(device);
            binding.code = static_cast<uint16_t>(code);
            binding.positive = static_cast<uint16_t>(positive);
            binding.negative = static_cast<uint16_t>(negative);
            binding.gamepadSlot = slot;
            binding.deadZone = deadZone;
            binding.invert = axis.value("invert", false);
            out.axes.push_back(binding);
        }
    }

    if (out.isAxis && !out.actions.empty())
        return Fail(err, path, "axis mapping cannot contain action bindings");
    if (!out.isAxis && !out.axes.empty())
        return Fail(err, path, "action mapping cannot contain axis bindings");
    return true;
}

bool ValidateScope(const InputContextRecord& record,
                   InputConfigScope scope,
                   Error& err)
{
    if (record.contextId.empty())
        return Fail(err, "inputContexts", "contextId must not be empty");
    if (scope == InputConfigScope::ProjectDefaults &&
        IsEditorOwnedInputContext(record.contextId))
        return Fail(err, "inputContexts." + record.contextId,
                    "project input cannot override an editor-owned context");
    if (scope == InputConfigScope::ProjectDefaults &&
        record.contextId != "runtime")
        return Fail(err, "inputContexts." + record.contextId,
                    "project v1 supports the runtime context only");
    if (scope == InputConfigScope::UserOverrides &&
        !IsEditorOwnedInputContext(record.contextId) &&
        record.contextId != "runtime")
        return Fail(err, "inputContexts." + record.contextId,
                    "user override context has no runtime owner");
    return true;
}

using ContextMap = std::map<std::string, std::map<std::string, InputMapping>>;

bool Overlay(const std::vector<InputContextRecord>& records,
             InputConfigScope scope,
             ContextMap& result,
             Error& err)
{
    std::set<std::string> seenContexts;
    for (const auto& record : records)
    {
        if (!ValidateScope(record, scope, err)) return false;
        if (!seenContexts.insert(record.contextId).second)
            return Fail(err, "inputContexts." + record.contextId,
                        "duplicate input context");
        std::set<std::string> seenMappings;
        auto& mappings = result[record.contextId];
        for (const auto& mapping : record.mappings)
        {
            if (mapping.name.empty())
                return Fail(err, "inputContexts." + record.contextId,
                            "mapping name must not be empty");
            if (!seenMappings.insert(mapping.name).second)
                return Fail(err, "inputContexts." + record.contextId + "." +
                            mapping.name, "duplicate input mapping");
            const bool unbound = mapping.actions.empty() && mapping.axes.empty();
            if (unbound)
                mappings.erase(mapping.name);
            else
                mappings[mapping.name] = mapping;
        }
    }
    return true;
}

} // namespace

bool IsEditorOwnedInputContext(const std::string& contextId)
{
    return contextId == "editor" || contextId == "viewport" ||
           contextId == "viewport.look";
}

bool ParseInputContextRecords(const nlohmann::json& value,
                              InputConfigScope scope,
                              std::vector<InputContextRecord>& out,
                              Error& err)
{
    err = Error{};
    if (!value.is_array())
        return Fail(err, "inputContexts", "input contexts must be an array");

    try
    {

    std::vector<InputContextRecord> parsed;
    std::set<std::string> contexts;
    size_t contextIndex = 0;
    for (const auto& context : value)
    {
        const std::string path = "inputContexts[" +
            std::to_string(contextIndex++) + "]";
        if (!context.is_object() || !context.contains("contextId") ||
            !context["contextId"].is_string())
            return Fail(err, path, "input context must contain contextId");

        InputContextRecord record;
        record.contextId = context["contextId"].get<std::string>();
        if (!ValidateScope(record, scope, err)) return false;
        if (!contexts.insert(record.contextId).second)
            return Fail(err, path, "duplicate input context " + record.contextId);
        if (context.contains("mappings") && !context["mappings"].is_array())
            return Fail(err, path + ".mappings", "mappings must be an array");

        std::set<std::string> mappings;
        if (context.contains("mappings"))
        {
            size_t mappingIndex = 0;
            for (const auto& mappingValue : context["mappings"])
            {
                InputMapping mapping;
                const std::string mappingPath = path + ".mappings[" +
                    std::to_string(mappingIndex++) + "]";
                if (!ParseMapping(mappingValue, mappingPath, mapping, err))
                    return false;
                if (!mappings.insert(mapping.name).second)
                    return Fail(err, mappingPath,
                                "duplicate input mapping " + mapping.name);
                record.mappings.push_back(std::move(mapping));
            }
        }
        std::sort(record.mappings.begin(), record.mappings.end(),
                  [](const InputMapping& a, const InputMapping& b) {
                      return a.name < b.name;
                  });
        parsed.push_back(std::move(record));
    }
    std::sort(parsed.begin(), parsed.end(),
              [](const InputContextRecord& a, const InputContextRecord& b) {
                  return a.contextId < b.contextId;
              });
    out = std::move(parsed);
    return true;
    }
    catch (const std::exception& exception)
    {
        return Fail(err, "inputContexts",
                    std::string("invalid input binding value: ") +
                    exception.what());
    }
}

nlohmann::json InputContextRecordsToJson(
    const std::vector<InputContextRecord>& records)
{
    std::vector<InputContextRecord> sorted = records;
    std::sort(sorted.begin(), sorted.end(),
              [](const InputContextRecord& a, const InputContextRecord& b) {
                  return a.contextId < b.contextId;
              });
    nlohmann::json contexts = nlohmann::json::array();
    for (auto& record : sorted)
    {
        std::sort(record.mappings.begin(), record.mappings.end(),
                  [](const InputMapping& a, const InputMapping& b) {
                      return a.name < b.name;
                  });
        nlohmann::json context;
        context["contextId"] = record.contextId;
        context["mappings"] = nlohmann::json::array();
        for (const auto& mapping : record.mappings)
        {
            nlohmann::json item;
            item["name"] = mapping.name;
            item["isAxis"] = mapping.isAxis;
            item["actions"] = nlohmann::json::array();
            for (const auto& binding : mapping.actions)
            {
                item["actions"].push_back({
                    { "device", static_cast<int>(binding.device) },
                    { "code", static_cast<int>(binding.code) },
                    { "modifiers", static_cast<int>(binding.modifiers) },
                    { "gamepadSlot", binding.gamepadSlot },
                });
            }
            item["axes"] = nlohmann::json::array();
            for (const auto& binding : mapping.axes)
            {
                item["axes"].push_back({
                    { "device", static_cast<int>(binding.device) },
                    { "code", static_cast<int>(binding.code) },
                    { "positive", static_cast<int>(binding.positive) },
                    { "negative", static_cast<int>(binding.negative) },
                    { "gamepadSlot", binding.gamepadSlot },
                    { "deadZone", binding.deadZone },
                    { "invert", binding.invert },
                });
            }
            context["mappings"].push_back(std::move(item));
        }
        contexts.push_back(std::move(context));
    }
    return contexts;
}

bool ComposeInputContexts(
    const std::vector<InputContextRecord>& builtIns,
    const std::vector<InputContextRecord>& projectDefaults,
    const std::vector<InputContextRecord>& userOverrides,
    std::vector<InputContextRecord>& out,
    Error& err)
{
    err = Error{};
    ContextMap composed;
    if (!Overlay(builtIns, InputConfigScope::BuiltIn, composed, err) ||
        !Overlay(projectDefaults, InputConfigScope::ProjectDefaults,
                 composed, err) ||
        !Overlay(userOverrides, InputConfigScope::UserOverrides,
                 composed, err))
        return false;

    std::vector<InputContextRecord> result;
    for (auto& [contextId, mappings] : composed)
    {
        InputContextRecord record;
        record.contextId = contextId;
        for (auto& [name, mapping] : mappings)
        {
            (void)name;
            record.mappings.push_back(std::move(mapping));
        }
        result.push_back(std::move(record));
    }
    out = std::move(result);
    return true;
}

} // namespace rt2::core
