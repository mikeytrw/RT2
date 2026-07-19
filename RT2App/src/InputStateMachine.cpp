#include "InputStateMachine.h"

#include <algorithm>
#include <cmath>

namespace rt2::core {

// ============================================================================
// Context stack
// ============================================================================

void InputStateMachine::PushContext(InputContext* ctx)
{
    if (ctx)
        m_ContextStack.push_back(ctx);
}

void InputStateMachine::PopContext()
{
    if (!m_ContextStack.empty())
        m_ContextStack.pop_back();
    ResetMouseHistory();
}

void InputStateMachine::ClearContextStack()
{
    m_ContextStack.clear();
    ResetMouseHistory();
}

void InputStateMachine::ResetMouseHistory()
{
    m_ResetMouseHistory = true;
}

// ============================================================================
// Helpers
// ============================================================================

int InputStateMachine::ResolveGamepadSlot(int requested,
                                           const RawInputSnapshot& s) const
{
    if (requested >= 0 && requested < RawInputSnapshot::kMaxGamepadSlots)
        return s.gamepadPresent[requested] ? requested : -1;
    if (requested == -1)
    {
        // "any connected gamepad" → first present slot.
        for (int i = 0; i < RawInputSnapshot::kMaxGamepadSlots; ++i)
            if (s.gamepadPresent[i]) return i;
        return -1;
    }
    return -1;
}

bool InputStateMachine::IsSourceDown(const SourceKey& k,
                                      const RawInputSnapshot& s) const
{
    switch (k.device)
    {
        case InputDeviceKind::KeyboardKey:
            return s.keysDown.count(k.code) != 0;
        case InputDeviceKind::MouseButton:
            return s.mouseButtonsDown.count(k.code) != 0;
        case InputDeviceKind::GamepadButton:
        {
            int slot = ResolveGamepadSlot(k.gamepadSlot, s);
            if (slot < 0) return false;
            if (k.code >= 15) return false;
            return s.gamepadButtons[slot][k.code];
        }
        case InputDeviceKind::GamepadAxis:
            return false;   // axis down-state is not binary; handled in axis path
    }
    return false;
}

// Modifier match: binding's requested modifiers must be a subset of
// the snapshot's active modifiers.
static bool ModifiersMatch(ModifierBits requested, const RawInputSnapshot& s)
{
    if (static_cast<uint8_t>(requested) == 0) return true;
    uint8_t have = 0;
    if (s.ctrl)  have |= static_cast<uint8_t>(ModifierBits::Ctrl);
    if (s.shift) have |= static_cast<uint8_t>(ModifierBits::Shift);
    if (s.alt)   have |= static_cast<uint8_t>(ModifierBits::Alt);
    if (s.super) have |= static_cast<uint8_t>(ModifierBits::Super);
    return (have & static_cast<uint8_t>(requested)) ==
           static_cast<uint8_t>(requested);
}

static SourceKey BindingSource(const ActionBinding& b)
{
    return SourceKey{ b.device, b.code, b.gamepadSlot };
}

static SourceKey BindingSourceAxis(const AxisBinding& b)
{
    return SourceKey{ b.device, b.code, b.gamepadSlot };
}

bool InputStateMachine::IsSourceClaimedAbove(const SourceKey& k,
                                              size_t ctxIndex) const
{
    auto it = m_Claimed.find(k);
    if (it == m_Claimed.end()) return false;
    return it->second < ctxIndex;
}

InputStateMachine::ResolvedMapping
InputStateMachine::ResolveMapping(const std::string& name) const
{
    for (size_t i = m_ContextStack.size(); i-- > 0; )
    {
        const InputContext* ctx = m_ContextStack[i];
        if (!ctx) continue;
        const InputMapping* m = ctx->FindMapping(name);
        if (m)
            return ResolvedMapping{ m, i };
    }
    return ResolvedMapping{ nullptr, 0 };
}

// ============================================================================
// BeginFrame — advance the state machine by one frame
// ============================================================================

void InputStateMachine::BeginFrame(const RawInputSnapshot& snapshot)
{
    // ---- Focus-loss tracking ----
    const bool focused = snapshot.windowFocused;
    // Detect refocus transition: if we were focus-lost last frame and
    // are focused now, clear m_FocusLost BEFORE computing effectiveFocus
    // so this frame samples normally. We also seed previousDown =
    // currentDown for all sources after computing currentDown, so no
    // spurious Pressed edges fire on the refocus frame.
    const bool refocusingThisFrame = m_FocusLost && focused;
    if (refocusingThisFrame)
        m_FocusLost = false;

    if (!focused && !m_FocusLost)
    {
        // Transition: focused → unfocused. Mark all currently-down
        // sources as Released for one frame. We implement this by
        // keeping currentDown = false next frame and letting the edge
        // computation produce Released (previousDown = true,
        // currentDown = false).
        m_FocusLost = true;
    }

    // If focus is lost, ignore raw samples (treat everything as up).
    // The previous frame's Released edges were already produced; this
    // frame and subsequent frames produce None until refocus.
    const bool effectiveFocus = m_FocusLost ? false : focused;

    // ---- Per-source current-down ----
    // Walk every binding in every context and record currentDown for
    // its source. We rebuild m_SourceStates each frame (we need
    // previousDown from last frame, which we saved at EndFrame).
    //
    // We need to preserve previousDown for sources that existed last
    // frame. So we don't clear m_SourceStates; we update currentDown
    // for sources we visit and reset currentDown = false for sources
    // we don't visit (they're no longer bound in any context).
    //
    // First, reset currentDown for all existing entries (we'll set
    // them true as we visit).
    for (auto& kv : m_SourceStates)
        kv.second.currentDown = false;

    auto touchSource = [&](const SourceKey& k, bool down)
    {
        auto& state = m_SourceStates[k];
        state.currentDown = state.currentDown || down;
    };

    for (const InputContext* ctx : m_ContextStack)
    {
        if (!ctx) continue;
        for (const auto& kv : ctx->All())
        {
            const InputMapping& m = kv.second;
            for (const ActionBinding& b : m.actions)
            {
                if (!ModifiersMatch(b.modifiers, snapshot)) continue;
                SourceKey k = BindingSource(b);
                touchSource(k, effectiveFocus && IsSourceDown(k, snapshot));
            }
            for (const AxisBinding& b : m.axes)
            {
                if (b.device == InputDeviceKind::KeyboardKey)
                {
                    // Two sources: positive and negative.
                    SourceKey kp{ b.device, b.positive, b.gamepadSlot };
                    SourceKey kn{ b.device, b.negative, b.gamepadSlot };
                    touchSource(kp, effectiveFocus && IsSourceDown(kp, snapshot));
                    touchSource(kn, effectiveFocus && IsSourceDown(kn, snapshot));
                }
                else if (b.device == InputDeviceKind::GamepadAxis)
                {
                    // Axis sources are not binary; we don't track them
                    // in m_SourceStates. Axis values are computed in
                    // RecomputeActionsAndAxes.
                }
            }
        }
    }

    // ---- Refocus edge special case ----
    // If we just refocused this frame, we want previousDown = currentDown
    // so no spurious Pressed edges fire (keys held through refocus read
    // Held, not Pressed).
    if (refocusingThisFrame)
    {
        for (auto& kv : m_SourceStates)
            kv.second.previousDown = kv.second.currentDown;
    }

    // ---- Mouse delta ----
    if (m_ResetMouseHistory)
    {
        m_MouseDelta = {0.0f, 0.0f};
        m_LastMousePos = snapshot.mousePos;
        m_ResetMouseHistory = false;
    }
    else
    {
        m_MouseDelta = snapshot.mousePos - m_LastMousePos;
        m_LastMousePos = snapshot.mousePos;
    }
    m_ScrollDelta = snapshot.scrollDelta;

    // ---- Recompute actions and axes ----
    RecomputeActionsAndAxes(snapshot);
}

void InputStateMachine::RecomputeActionsAndAxes(
    const RawInputSnapshot& snapshot)
{
    m_Claimed.clear();
    m_Actions.clear();
    m_Axes.clear();

    // Walk the stack from top to bottom. For each context, claim its
    // sources (if not already claimed by a higher context). Then
    // compute action edges and axis values from the surviving bindings.
    //
    // We compute per-action state by aggregating across ALL contexts in
    // the stack that map the action AND whose bindings survive the
    // claim check. A binding survives if its source is not claimed by
    // a higher context.

    // First pass: claim sources, top context wins.
    for (size_t i = m_ContextStack.size(); i-- > 0; )
    {
        const InputContext* ctx = m_ContextStack[i];
        if (!ctx) continue;
        for (const auto& kv : ctx->All())
        {
            const InputMapping& m = kv.second;
            for (const ActionBinding& b : m.actions)
            {
                SourceKey k = BindingSource(b);
                if (m_Claimed.find(k) == m_Claimed.end())
                    m_Claimed[k] = i;
            }
            for (const AxisBinding& b : m.axes)
            {
                if (b.device == InputDeviceKind::KeyboardKey)
                {
                    SourceKey kp{ b.device, b.positive, b.gamepadSlot };
                    SourceKey kn{ b.device, b.negative, b.gamepadSlot };
                    if (m_Claimed.find(kp) == m_Claimed.end())
                        m_Claimed[kp] = i;
                    if (m_Claimed.find(kn) == m_Claimed.end())
                        m_Claimed[kn] = i;
                }
                else if (b.device == InputDeviceKind::GamepadAxis)
                {
                    SourceKey k = BindingSourceAxis(b);
                    if (m_Claimed.find(k) == m_Claimed.end())
                        m_Claimed[k] = i;
                }
            }
        }
    }

    // Second pass: per action, aggregate currentDown/previousDown across
    // all bindings that survive the claim check.
    //
    // We iterate every action name known to any context. For each, find
    // every context that maps it and check each binding's source; if
    // the binding's source is claimed by this context or below (i.e.
    // not claimed by a higher context), it contributes to the action.
    //
    // The claim map stores the index of the context that claimed the
    // source. A binding in context[i] survives if the claim map entry
    // for its source is >= i (i.e. no higher context claimed it).
    std::unordered_set<std::string> actionNames;
    std::unordered_set<std::string> axisNames;
    for (const InputContext* ctx : m_ContextStack)
    {
        if (!ctx) continue;
        for (const auto& kv : ctx->All())
        {
            if (kv.second.isAxis) axisNames.insert(kv.first);
            else                  actionNames.insert(kv.first);
        }
    }

    for (const std::string& name : actionNames)
    {
        bool currentDown = false;
        bool previousDown = false;
        for (size_t i = 0; i < m_ContextStack.size(); ++i)
        {
            const InputContext* ctx = m_ContextStack[i];
            if (!ctx) continue;
            const InputMapping* m = ctx->FindMapping(name);
            if (!m || m->isAxis) continue;
            for (const ActionBinding& b : m->actions)
            {
                if (!ModifiersMatch(b.modifiers, snapshot)) continue;
                SourceKey k = BindingSource(b);
                auto it = m_Claimed.find(k);
                // A binding in context[i] survives if its source is NOT
                // claimed by a higher context. The claim map stores the
                // index of the HIGHEST context that claimed the source
                // (top wins). So the binding survives if the claim was
                // made by this context or below (it->second <= i), or
                // if the source is unclaimed.
                if (it == m_Claimed.end() || it->second <= i)
                {
                    auto st = m_SourceStates.find(k);
                    if (st != m_SourceStates.end())
                    {
                        currentDown  = currentDown  || st->second.currentDown;
                        previousDown = previousDown || st->second.previousDown;
                    }
                }
            }
        }
        ActionState state;
        if (currentDown && !previousDown)      state = ActionState::Pressed;
        else if (currentDown && previousDown)  state = ActionState::Held;
        else if (!currentDown && previousDown) state = ActionState::Released;
        else                                   state = ActionState::None;
        m_Actions[name].state = state;
    }

    for (const std::string& name : axisNames)
    {
        float value = 0.0f;
        for (size_t i = 0; i < m_ContextStack.size(); ++i)
        {
            const InputContext* ctx = m_ContextStack[i];
            if (!ctx) continue;
            const InputMapping* m = ctx->FindMapping(name);
            if (!m || !m->isAxis) continue;
            for (const AxisBinding& b : m->axes)
            {
                if (b.device == InputDeviceKind::KeyboardKey)
                {
                    SourceKey kp{ b.device, b.positive, b.gamepadSlot };
                    SourceKey kn{ b.device, b.negative, b.gamepadSlot };
                    // A binding survives if its source is not claimed by
                    // a higher context (it->second <= i).
                    bool posSurvives = true, negSurvives = true;
                    auto itp = m_Claimed.find(kp);
                    auto itn = m_Claimed.find(kn);
                    if (itp != m_Claimed.end() && itp->second > i) posSurvives = false;
                    if (itn != m_Claimed.end() && itn->second > i) negSurvives = false;
                    if (!posSurvives && !negSurvives) continue;
                    float v = 0.0f;
                    if (posSurvives)
                    {
                        auto st = m_SourceStates.find(kp);
                        if (st != m_SourceStates.end() && st->second.currentDown) v += 1.0f;
                    }
                    if (negSurvives)
                    {
                        auto st = m_SourceStates.find(kn);
                        if (st != m_SourceStates.end() && st->second.currentDown) v -= 1.0f;
                    }
                    value += v;
                }
                else if (b.device == InputDeviceKind::GamepadAxis)
                {
                    SourceKey k = BindingSourceAxis(b);
                    auto it = m_Claimed.find(k);
                    if (it != m_Claimed.end() && it->second > i) continue;
                    int slot = ResolveGamepadSlot(b.gamepadSlot, snapshot);
                    if (slot < 0) continue;
                    if (b.code >= 6) continue;
                    float raw = snapshot.gamepadAxes[slot][b.code];
                    float dz = b.deadZone;
                    float scaled;
                    if (std::abs(raw) <= dz) scaled = 0.0f;
                    else
                    {
                        float sign = (raw < 0.0f) ? -1.0f : 1.0f;
                        scaled = sign * (std::abs(raw) - dz) / (1.0f - dz);
                    }
                    if (b.invert) scaled = -scaled;
                    value += scaled;
                }
            }
        }
        if (value > 1.0f)  value = 1.0f;
        if (value < -1.0f) value = -1.0f;
        m_Axes[name] = value;
    }
}

// ============================================================================
// Suppression (applied by InputService::ResolveUI)
// ============================================================================

void InputStateMachine::SuppressKeyboardActions()
{
    m_SuppressKeyboard = true;
    for (auto& kv : m_Actions)
    {
        // We don't know which actions are keyboard-sourced without
        // re-walking the bindings; the InputService applies suppression
        // by action name via SuppressAction. This flag is used to
        // blanket-suppress in a second pass below.
    }
    // Apply blanket suppression now: any action whose mapping in any
    // context has a KeyboardKey binding is suppressed.
    std::unordered_set<std::string> keyboardActions;
    for (const InputContext* ctx : m_ContextStack)
    {
        if (!ctx) continue;
        for (const auto& kv : ctx->All())
        {
            const InputMapping& m = kv.second;
            if (m.isAxis) continue;
            for (const ActionBinding& b : m.actions)
                if (b.device == InputDeviceKind::KeyboardKey)
                    keyboardActions.insert(m.name);
        }
    }
    for (const std::string& n : keyboardActions)
    {
        m_Actions[n].state = ActionState::None;
        m_Actions[n].suppressed = true;
    }
    // Also zero keyboard-sourced axes.
    std::unordered_set<std::string> keyboardAxes;
    for (const InputContext* ctx : m_ContextStack)
    {
        if (!ctx) continue;
        for (const auto& kv : ctx->All())
        {
            const InputMapping& m = kv.second;
            if (!m.isAxis) continue;
            for (const AxisBinding& b : m.axes)
                if (b.device == InputDeviceKind::KeyboardKey)
                    keyboardAxes.insert(m.name);
        }
    }
    for (const std::string& n : keyboardAxes)
        m_Axes[n] = 0.0f;
}

void InputStateMachine::SuppressMouseActions()
{
    m_SuppressMouse = true;
    std::unordered_set<std::string> mouseActions;
    for (const InputContext* ctx : m_ContextStack)
    {
        if (!ctx) continue;
        for (const auto& kv : ctx->All())
        {
            const InputMapping& m = kv.second;
            if (m.isAxis) continue;
            for (const ActionBinding& b : m.actions)
                if (b.device == InputDeviceKind::MouseButton)
                    mouseActions.insert(m.name);
        }
    }
    for (const std::string& n : mouseActions)
    {
        m_Actions[n].state = ActionState::None;
        m_Actions[n].suppressed = true;
    }
}

void InputStateMachine::SuppressAction(const std::string& name)
{
    m_Actions[name].state = ActionState::None;
    m_Actions[name].suppressed = true;
    m_SuppressedActions.insert(name);
}

// ============================================================================
// EndFrame
// ============================================================================

void InputStateMachine::EndFrame()
{
    // Commit current → previous for next frame.
    for (auto& kv : m_SourceStates)
        kv.second.previousDown = kv.second.currentDown;

    // Clear per-frame deltas.
    m_MouseDelta = {0.0f, 0.0f};
    m_ScrollDelta = 0.0f;

    // Clear suppression flags.
    m_SuppressKeyboard = false;
    m_SuppressMouse = false;
    m_SuppressedActions.clear();
}

// ============================================================================
// Queries
// ============================================================================

ActionState InputStateMachine::GetActionState(const std::string& name) const
{
    auto it = m_Actions.find(name);
    if (it == m_Actions.end()) return ActionState::None;
    return it->second.state;
}

float InputStateMachine::GetAxisValue(const std::string& name) const
{
    auto it = m_Axes.find(name);
    if (it == m_Axes.end()) return 0.0f;
    return it->second;
}

} // namespace rt2::core