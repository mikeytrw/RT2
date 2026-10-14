#ifndef RT2_SHADOW_TAIL_GLSL
#define RT2_SHADOW_TAIL_GLSL

// ============================================================================
// Shadow-tail diagnostic (RT4-T4, contract READY 2026-10-13).
//
// Included by scatter_shared.glsl (RT pipeline, after pathtracer_shared
// declares `camera`) and by restir_gi_bindings.glsl immediately before
// ray_query_scene.glsl (after its `camera` declaration) — AFTER
// the includer declares `camera` (CameraData, position.w = frame index).
// Compiled two ways from the SAME sources:
//   OFF (default, no -D): every ST_* macro below expands to nothing, so
//     the emitted code is the original — OFF/production parity is
//     verified by SPV byte comparison where reproducible.
//   ON (-DSHADOW_TAIL_DIAG=1): brackets wrap the 5 RT shadow trace sites
//     and giTraceShadow, recording device-clock deltas into the set-1
//     binding-11 SSBO. Built to separate *.tail.spv files; production
//     loads are untouched.
//
// Layout contract (frozen; MUST match ShadowTailDiag.h + manifest):
//   header[8]: magic, version, frameIndex, mode, edgeCountRT,
//     edgeCountGI, variantTag, slotSelect.
//   edgesRT[226], edgesGI[226]: frozen acceptance edges (u32 ticks).
//   per slot (stHead[7], CPU-staged per frame) per family (RT=0, GI=1):
//     pilot[34], hist[226], eligible, recorded, blocked, visible,
//     strata[16], ord[16], reserved[2]  => 298 u32 per family;
//     contiguous per-slot blocks (slot*598 + fam*298) plus 2
//     positive-control checksums per slot.
//     (1656 u32 = 6624 bytes total with header/edges).
// SSBO writes are the ONLY observer effect: no rngState, payload, or
// shading input is touched, and OFF emits no clock/SSBO code at all.
// ============================================================================

#ifdef SHADOW_TAIL_DIAG

// Device-scope two-word clock ONLY (GL_EXT_shader_realtime_clock).
// Deliberately no shaderInt64: clockRealtime2x32EXT + low-word borrow
// give full modular 64-bit deltas (Khronos-confirmed), keeping ordinary
// device support requirements minimal.
#extension GL_EXT_shader_realtime_clock : require

#define ST_FAMILY_RT 0u
#define ST_FAMILY_GI 1u
#define ST_EDGE_MAX 226u
#define ST_PILOT_N 34u
#define ST_STRATA_N 16u
#define ST_ORD_N 16u
#define ST_MAGIC 0x53484457u
#define ST_VERSION 1u
// Positive-control trip count (diagnostic validation only; fixed).
#define ST_CONTROL_ITERS 1024u

layout(set = 1, binding = 11, std430) buffer ShadowTailDiag
{
    uint stHead[8];
    uint stEdgesRT[226];
    uint stEdgesGI[226];
    uint stSlots[2 * 2 * 298 + 2 * 2];
};

// Per-invocation context, set by callers (ON-only assignments, appended
// to existing lines so OFF emits the original code byte-identically):
//  - gShadowDepth: closesthit/secondary_raygen mains (0 primary, 1 recursive)
//  - gCallOrd: per-invocation monotonic call ordinal, reset in mains
//    (closesthit/secondary entry) — encodes repeated NEE calls within one
//    invocation; reset scope is the shader invocation (pixel x frame).
//  - gGIProv/gGICand: restir_gi_temporal fresh (0, loop i) / replay (1, 0)
//  - gGISite: giSecondaryNEE triangle (0) / environment (1) branches
uint gShadowDepth;
uint gCallOrd;
uint gGIProv;
uint gGICand;
uint gGISite;

uint stSlot()
{
    // Slot authority is the CPU-staged header word (set per frame in
    // BeginFrame, before any dispatch of that frame): the single fixed
    // counter segment for this frame. camera.position.w is NOT used:
    // it disagrees with the fence slot (initial values, resets,
    // negative without accumulation) and cannot be an authority.
    return stHead[7] & 1u;
}

uint stFamBase(uint fam)
{
    // Contiguous per-slot blocks: slot*598 + fam*298. Matches the CPU
    // authority StSlotOffset exactly (same formula, verified by test).
    return stSlot() * 598u + fam * 298u;
}

uint stCtlBase()
{
    return stSlot() * 598u + 596u;
}

// Bin a two-word delta. High word nonzero ALWAYS routes to overflow
// (never truncated to the low word). Returns the acceptance-bin index
// (0..ec-2 finite, ec-1 overflow) or, for the pilot, the doubling index
// is computed by the caller via stPilotIndex.
uint stAcceptBin(uint fam, uint lo, uint hi, uint ec)
{
    uint capped = min(ec, 226u); // allocation bound; manifest mismatch is
                                 // caught by CPU cross-check, never OOB here
    if (capped < 2u)
        return 0u; // degenerate manifest: eligible still counted below
    if (hi != 0u)
        return capped - 1u;
    uint l = 1u, r = capped - 1u, ans = capped;
    while (l <= r)
    {
        uint m = (l + r) >> 1u;
        uint e = (fam == 0u) ? stEdgesRT[m] : stEdgesGI[m];
        if (e > lo) { ans = m; r = m - 1u; }
        else { l = m + 1u; }
    }
    return (ans >= capped) ? capped - 1u : ans - 1u;
}

uint stPilotIndex(uint lo, uint hi)
{
    if (lo == 0u && hi == 0u)
        return 0u; // underflow {0}: zero-p95 pilot, precision-inadequate
    if (hi != 0u)
        return 33u; // overflow [2^32, +inf): fixture invalid
    return 1u + uint(findMSB(lo)); // doubling bins k=0..31 -> 1..32
}

// Dependent-work positive control (validation only): bounded extra work
// with a checksum consumed into the control counter (never into lighting
// payloads or RNG). The trip count is fixed; a responsive timer shifts
// the measured bracket upward with larger... (single fixed level here;
// the shift is verified against the zero-work baseline).
uint stPositiveWork()
{
    uint acc = 0u;
    for (uint i = 0u; i < ST_CONTROL_ITERS; i++)
        acc = acc * 747796405u + 2891336453u + i;
    return acc;
}

void stRecord(uint fam, uint stratum, uint outcome, uint ord, uvec2 t0, uvec2 t1)
{
    uint base = stFamBase(fam);
    // Eligible + recorded (identical at fixed rate 1; both counted).
    atomicAdd(stSlots[base + 260u], 1u);
    atomicAdd(stSlots[base + 261u], 1u);
    // Duration, full 64-bit modular difference (low-word borrow).
    uint borrow = (t1.x < t0.x) ? 1u : 0u;
    uint dlo = t1.x - t0.x;
    uint dhi = t1.y - t0.y - borrow;
    // Pilot (fixed doubling edges — Q-independent by construction).
    atomicAdd(stSlots[base + stPilotIndex(dlo, dhi)], 1u);
    // Acceptance (manifest edges; active count bounds the search).
    uint ec = (fam == 0u) ? stHead[4] : stHead[5];
    uint hbin = stAcceptBin(fam, dlo, dhi, ec);
    atomicAdd(stSlots[base + 34u + hbin], 1u);
    // Outcome + stratum + call-ordinal distribution (stratum/ordinal
    // indices validated by construction: RT site*2+depth <= 11,
    // GI site*2+prov <= 3, ordinal saturates at 15).
    atomicAdd(stSlots[base + 262u + outcome], 1u); // blocked=0, visible=1
    atomicAdd(stSlots[base + 264u + min(stratum, 15u)], 1u);
    atomicAdd(stSlots[base + 280u + min(ord, 15u)], 1u);
}

// Control-aware tail clock: the dependent work runs INSIDE the timed
// bracket (between trace and this read) when header mode == 2
// (positive-control runs); the checksum is consumed OUTSIDE the bracket
// by the caller (stRecord never sees it). Ordinary ON runs (mode 1) pay
// one uniform header read and nothing else.
#define ST_CTL_TAIL(fam) (stHead[3] == 2u ? stCtlTail(fam) : clockRealtime2x32EXT())
uvec2 stCtlTail(uint fam)
{
    uint acc = stPositiveWork();
    uvec2 t = clockRealtime2x32EXT();
    atomicAdd(stSlots[stCtlBase() + fam], acc);
    return t;
}

// RT bracket: site 0 env, 1 tri, 2 res-tri, 3 res-env, 4 punctual.
// shadowVisible + gShadowDepth + gCallOrd are in scope at every call site.
#define ST_RT_BEGIN(var) uvec2 var = clockRealtime2x32EXT()
#define ST_RT_END(var, site) stRecord(0u, (site) * 2u + min(gShadowDepth, 1u), (shadowVisible < 0.5) ? 0u : 1u, (gCallOrd++), var, ST_CTL_TAIL(0u))
#define ST_SET_DEPTH(d) gShadowDepth = (d)
#define ST_RESET_ORD() gCallOrd = 0u

// GI bracket: outcome from committedType (gl_RayQueryCommittedIntersectionNoneEXT
// = unoccluded = visible). Site/provenance/ordinal from ON-only globals.
#define ST_GI_BEGIN(var) uvec2 var = clockRealtime2x32EXT()
#define ST_GI_END(var) stRecord(1u, gGISite * 2u + min(gGIProv, 1u), (committedType == gl_RayQueryCommittedIntersectionNoneEXT) ? 1u : 0u, gGICand, var, ST_CTL_TAIL(1u))
#define ST_SET_GIPROV(p, c) gGIProv = (p); gGICand = (c)
#define ST_SET_GISITE(s) gGISite = (s)

#else // OFF: everything below expands to nothing; emitted code is original.

#define ST_RT_BEGIN(var)
#define ST_RT_END(var, site)
#define ST_SET_DEPTH(d)
#define ST_RESET_ORD()
#define ST_GI_BEGIN(var)
#define ST_GI_END(var)
#define ST_SET_GIPROV(p, c)
#define ST_SET_GISITE(s)

#endif // SHADOW_TAIL_DIAG

#endif // RT2_SHADOW_TAIL_GLSL
