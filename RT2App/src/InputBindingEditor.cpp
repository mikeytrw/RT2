#include "InputBindingEditor.h"

#include <algorithm>
#include <map>
#include <sstream>
#include <tuple>

namespace rt2::core {

namespace {

std::string KeyboardName(uint16_t code)
{
    if (code >= static_cast<uint16_t>(KeyCode::A) &&
        code <= static_cast<uint16_t>(KeyCode::Z))
        return std::string(1, static_cast<char>(code));
    if (code >= static_cast<uint16_t>(KeyCode::D0) &&
        code <= static_cast<uint16_t>(KeyCode::D9))
        return std::string(1, static_cast<char>(code));
    if (code >= static_cast<uint16_t>(KeyCode::F1) &&
        code <= static_cast<uint16_t>(KeyCode::F12))
        return "F" + std::to_string(code - static_cast<uint16_t>(KeyCode::F1) + 1);

    switch (static_cast<KeyCode>(code))
    {
    case KeyCode::Space: return "Space";
    case KeyCode::Escape: return "Escape";
    case KeyCode::Enter: return "Enter";
    case KeyCode::Tab: return "Tab";
    case KeyCode::Backspace: return "Backspace";
    case KeyCode::Insert: return "Insert";
    case KeyCode::Delete: return "Delete";
    case KeyCode::Right: return "Right Arrow";
    case KeyCode::Left: return "Left Arrow";
    case KeyCode::Down: return "Down Arrow";
    case KeyCode::Up: return "Up Arrow";
    case KeyCode::PageUp: return "Page Up";
    case KeyCode::PageDown: return "Page Down";
    case KeyCode::Home: return "Home";
    case KeyCode::End: return "End";
    case KeyCode::LeftShift: return "Left Shift";
    case KeyCode::RightShift: return "Right Shift";
    case KeyCode::LeftControl: return "Left Ctrl";
    case KeyCode::RightControl: return "Right Ctrl";
    case KeyCode::LeftAlt: return "Left Alt";
    case KeyCode::RightAlt: return "Right Alt";
    case KeyCode::LeftSuper: return "Left Super";
    case KeyCode::RightSuper: return "Right Super";
    case KeyCode::Apostrophe: return "'";
    case KeyCode::Comma: return ",";
    case KeyCode::Minus: return "-";
    case KeyCode::Period: return ".";
    case KeyCode::Slash: return "/";
    case KeyCode::Semicolon: return ";";
    case KeyCode::Equal: return "=";
    case KeyCode::LeftBracket: return "[";
    case KeyCode::Backslash: return "\\";
    case KeyCode::RightBracket: return "]";
    case KeyCode::GraveAccent: return "`";
    default: return "Key " + std::to_string(code);
    }
}

std::string ActionDescription(const ActionBinding& binding)
{
    switch (binding.device)
    {
    case InputDeviceKind::KeyboardKey:
    {
        std::string value = KeyboardName(binding.code) + " (Keyboard)";
        if (Any(binding.modifiers))
        {
            std::string prefix;
            if ((binding.modifiers & ModifierBits::Ctrl) != ModifierBits::None)
                prefix += "Ctrl+";
            if ((binding.modifiers & ModifierBits::Shift) != ModifierBits::None)
                prefix += "Shift+";
            if ((binding.modifiers & ModifierBits::Alt) != ModifierBits::None)
                prefix += "Alt+";
            if ((binding.modifiers & ModifierBits::Super) != ModifierBits::None)
                prefix += "Super+";
            value = prefix + KeyboardName(binding.code) + " (Keyboard)";
        }
        return value;
    }
    case InputDeviceKind::MouseButton:
        switch (static_cast<MouseButton>(binding.code))
        {
        case MouseButton::Left: return "Left Mouse";
        case MouseButton::Right: return "Right Mouse";
        case MouseButton::Middle: return "Middle Mouse";
        default: return "Mouse Button " + std::to_string(binding.code);
        }
    case InputDeviceKind::GamepadButton:
        switch (static_cast<GamepadButton>(binding.code))
        {
        case GamepadButton::A: return "Gamepad A";
        case GamepadButton::B: return "Gamepad B";
        case GamepadButton::X: return "Gamepad X";
        case GamepadButton::Y: return "Gamepad Y";
        case GamepadButton::LeftBumper: return "Gamepad Left Bumper";
        case GamepadButton::RightBumper: return "Gamepad Right Bumper";
        case GamepadButton::Back: return "Gamepad Back";
        case GamepadButton::Start: return "Gamepad Start";
        case GamepadButton::LeftThumb: return "Gamepad Left Thumb";
        case GamepadButton::RightThumb: return "Gamepad Right Thumb";
        case GamepadButton::DpadUp: return "Gamepad D-pad Up";
        case GamepadButton::DpadRight: return "Gamepad D-pad Right";
        case GamepadButton::DpadDown: return "Gamepad D-pad Down";
        case GamepadButton::DpadLeft: return "Gamepad D-pad Left";
        default: return "Gamepad Button " + std::to_string(binding.code);
        }
    case InputDeviceKind::GamepadAxis:
        switch (static_cast<GamepadAxis>(binding.code))
        {
        case GamepadAxis::LeftX: return "Gamepad Left X";
        case GamepadAxis::LeftY: return "Gamepad Left Y";
        case GamepadAxis::RightX: return "Gamepad Right X";
        case GamepadAxis::RightY: return "Gamepad Right Y";
        case GamepadAxis::LeftTrigger: return "Gamepad Left Trigger";
        case GamepadAxis::RightTrigger: return "Gamepad Right Trigger";
        default: return "Gamepad Axis " + std::to_string(binding.code);
        }
    }
    return "Unknown binding";
}

std::string AxisDescription(const AxisBinding& binding)
{
    if (binding.device == InputDeviceKind::KeyboardKey)
        return KeyboardName(binding.positive) + " / " +
               KeyboardName(binding.negative) + " (Keyboard)";
    return ActionDescription(ActionBinding{
        InputDeviceKind::GamepadAxis, binding.code, ModifierBits::None,
        binding.gamepadSlot});
}

struct BindingKey
{
    InputDeviceKind device = InputDeviceKind::KeyboardKey;
    uint16_t code = 0;
    ModifierBits modifiers = ModifierBits::None;
    int gamepadSlot = -1;

    bool operator<(const BindingKey& other) const
    {
        return std::tie(device, code, modifiers, gamepadSlot) <
               std::tie(other.device, other.code, other.modifiers,
                        other.gamepadSlot);
    }
};

struct BindingReference
{
    BindingKey key;
    std::string mappingName;
    std::string description;
};

void AddActionReferences(const InputMapping& mapping,
                         std::vector<BindingReference>& out)
{
    for (const auto& binding : mapping.actions)
    {
        out.push_back({
            {binding.device, binding.code, binding.modifiers,
             binding.gamepadSlot},
            mapping.name, ActionDescription(binding)});
    }
}

void AddAxisReferences(const InputMapping& mapping,
                       std::vector<BindingReference>& out)
{
    for (const auto& binding : mapping.axes)
    {
        if (binding.device == InputDeviceKind::KeyboardKey)
        {
            out.push_back({
                {binding.device, binding.positive, ModifierBits::None, -1},
                mapping.name, KeyboardName(binding.positive) + " (Keyboard)"});
            out.push_back({
                {binding.device, binding.negative, ModifierBits::None, -1},
                mapping.name, KeyboardName(binding.negative) + " (Keyboard)"});
        }
        else
        {
            out.push_back({
                {binding.device, binding.code, ModifierBits::None,
                 binding.gamepadSlot},
                mapping.name, AxisDescription(binding)});
        }
    }
}

InputContextRecord MakeRecord(const std::string& contextId,
                              const std::string& mappingName,
                              bool isAxis)
{
    InputContextRecord record;
    record.contextId = contextId;
    InputMapping mapping;
    mapping.name = mappingName;
    mapping.isAxis = isAxis;
    record.mappings.push_back(std::move(mapping));
    return record;
}

} // namespace

ActionBinding CaptureActionBinding(InputDeviceKind device,
                                   uint16_t code,
                                   ModifierBits modifiers,
                                   int gamepadSlot)
{
    ActionBinding binding;
    binding.device = device;
    binding.code = code;
    binding.modifiers = modifiers;
    binding.gamepadSlot = gamepadSlot;
    return binding;
}

AxisBinding CaptureAxisBinding(InputDeviceKind device,
                               uint16_t code,
                               uint16_t positive,
                               uint16_t negative,
                               int gamepadSlot,
                               float deadZone,
                               bool invert)
{
    AxisBinding binding;
    binding.device = device;
    binding.code = code;
    binding.positive = positive;
    binding.negative = negative;
    binding.gamepadSlot = gamepadSlot;
    binding.deadZone = deadZone;
    binding.invert = invert;
    return binding;
}

InputContextRecord BuildOverrideRecord(
    const std::string& contextId,
    const std::string& mappingName,
    bool isAxis,
    const std::vector<ActionBinding>& bindings)
{
    auto record = MakeRecord(contextId, mappingName, isAxis);
    if (isAxis)
        record.mappings.front().actions.clear();
    else
        record.mappings.front().actions = bindings;
    return record;
}

InputContextRecord BuildOverrideRecord(
    const std::string& contextId,
    const std::string& mappingName,
    bool isAxis,
    const std::vector<AxisBinding>& bindings)
{
    auto record = MakeRecord(contextId, mappingName, isAxis);
    if (isAxis)
        record.mappings.front().axes = bindings;
    else
        record.mappings.front().axes.clear();
    return record;
}

std::string DescribeMapping(const InputMapping& mapping)
{
    std::vector<std::string> descriptions;
    if (mapping.isAxis)
    {
        for (const auto& binding : mapping.axes)
            descriptions.push_back(AxisDescription(binding));
    }
    else
    {
        for (const auto& binding : mapping.actions)
            descriptions.push_back(ActionDescription(binding));
    }

    if (descriptions.empty()) return "Unbound";
    std::ostringstream result;
    for (size_t i = 0; i < descriptions.size(); ++i)
    {
        if (i != 0) result << " or ";
        result << descriptions[i];
    }
    return result.str();
}

std::vector<InputBindingConflict> FindConflicts(
    const std::vector<InputContextRecord>& records)
{
    std::vector<InputBindingConflict> conflicts;
    for (const auto& record : records)
    {
        std::vector<BindingReference> bindings;
        for (const auto& mapping : record.mappings)
        {
            if (mapping.isAxis)
                AddAxisReferences(mapping, bindings);
            else
                AddActionReferences(mapping, bindings);
        }

        std::map<BindingKey, BindingReference> first;
        for (const auto& binding : bindings)
        {
            const auto it = first.find(binding.key);
            if (it == first.end())
            {
                first.emplace(binding.key, binding);
                continue;
            }
            if (it->second.mappingName == binding.mappingName)
                continue;
            conflicts.push_back({
                record.contextId, it->second.mappingName,
                binding.mappingName, binding.description});
        }
    }

    std::sort(conflicts.begin(), conflicts.end(),
        [](const InputBindingConflict& a, const InputBindingConflict& b) {
            return std::tie(a.contextId, a.mappingName,
                            a.conflictingMappingName,
                            a.bindingDescription) <
                   std::tie(b.contextId, b.mappingName,
                            b.conflictingMappingName,
                            b.bindingDescription);
        });
    return conflicts;
}

} // namespace rt2::core
