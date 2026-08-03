#pragma once

#ifndef RT2_CORE_INPUT_BINDING_EDITOR_H
#define RT2_CORE_INPUT_BINDING_EDITOR_H

#include "InputTypes.h"
#include "InputConfig.h"

#include <string>
#include <vector>

namespace rt2::core {

// CPU-only authoring helpers for the W8 Input Bindings panel. The panel owns
// persistence and runtime application; this module only constructs, describes
// and validates the values that cross the input-config boundary.

struct InputBindingConflict
{
    std::string contextId;
    std::string mappingName;
    std::string conflictingMappingName;
    std::string bindingDescription;
};

ActionBinding CaptureActionBinding(InputDeviceKind device,
                                   uint16_t code,
                                   ModifierBits modifiers = ModifierBits::None,
                                   int gamepadSlot = -1);

AxisBinding CaptureAxisBinding(InputDeviceKind device,
                               uint16_t code,
                               uint16_t positive,
                               uint16_t negative,
                               int gamepadSlot = -1,
                               float deadZone = 0.15f,
                               bool invert = false);

// The overloads keep action and axis binding vectors type-safe at the call
// site while retaining the spec's single BuildOverrideRecord operation.
InputContextRecord BuildOverrideRecord(
    const std::string& contextId,
    const std::string& mappingName,
    bool isAxis,
    const std::vector<ActionBinding>& bindings);

InputContextRecord BuildOverrideRecord(
    const std::string& contextId,
    const std::string& mappingName,
    bool isAxis,
    const std::vector<AxisBinding>& bindings);

// Human-readable descriptions are intentionally independent of Walnut/GLFW;
// codes use the same numeric values as the desktop backend.
std::string DescribeMapping(const InputMapping& mapping);

std::vector<InputBindingConflict> FindConflicts(
    const std::vector<InputContextRecord>& records);

} // namespace rt2::core

#endif // RT2_CORE_INPUT_BINDING_EDITOR_H
