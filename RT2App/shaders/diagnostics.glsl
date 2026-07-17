#ifndef RT2_DIAGNOSTICS_GLSL
#define RT2_DIAGNOSTICS_GLSL

// The owning shader must declare CameraData as `camera` before including this
// file. camera.sampling.y contains the renderer frame-slot index, allowing
// overlapping frames to write independent counter segments.
layout(set = 1, binding = SI_BINDING_DIAGNOSTICS, std430) buffer DiagnosticsBuffer
{
    uint diagnosticCounters[];
};

uint diagnosticCounterOffset(uint counter)
{
    uint slot = camera.sampling.y % SI_DIAGNOSTIC_FRAME_SLOTS;
    return slot * SI_DIAGNOSTIC_COUNTER_COUNT + counter;
}

void diagnosticAdd(uint counter, uint value)
{
    atomicAdd(diagnosticCounters[diagnosticCounterOffset(counter)], value);
}

void diagnosticIncrement(uint counter)
{
    diagnosticAdd(counter, 1u);
}

void diagnosticRecordAge(uint baseCounter, uint age)
{
    diagnosticIncrement(baseCounter + min(age, SI_DIAGNOSTIC_AGE_BIN_COUNT - 1u));
}

#endif
