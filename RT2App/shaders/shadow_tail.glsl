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
//     edgeCountGI, reserved, reserved.
//   edgesRT[226], edgesGI[226]: frozen acceptance edges (u32 ticks).
//   per slot (camera.position.w & 1) per family (RT=0, GI=1):
//     pilot[34], hist[226], eligible, recorded, blocked, visible,
//     strata[16], reserved[2]  => 282 u32 per family-slot
//     (1588 u32 = 6352 bytes total with header/edges).
// SSBO writes are the ONLY observer effect: no rngState, payload, or
// shading input is touched, and OFF emits no clock/SSBO code at all.
// ============================================================================

#ifdef SHADOW_TAIL_DIAG

#extension GL_EXT_shader_realtime_clock : require
#extension GL_ARB_gpu_shader_int64 : enable

#define ST_FAMILY_RT 0u
#define ST_FAMILY_GI 1u
#define ST_EDGE_MAX 226u
#define ST_PILOT_N 34u
#define ST_STRATA_N 16u
#define ST_MAGIC 0x53484457u
#define ST_VERSION 1u

layout(set = 1, binding = 11, std430) buffer ShadowTailDiag
{
    uint stHead[8];
    uint stEdgesRT[226];
    uint stEdgesGI[226];
    uint stSlots[2 * 2 * 282];
};

// Per-invocation context, set by callers (ON-only assignments, appended
// to existing lines so OFF emits the original code byte-identically):
//  - gShadowDepth: closesthit/secondary_raygen mains (0 primary, 1 recursive)
//  - gGIProv/gGICand: restir_gi_temporal fresh (0, loop i) / replay (1, 0)
//  - gGISite: giSecondaryNEE triangle (0) / environment (1) branches
uint gShadowDepth;
uint gGIProv;
uint gGICand;
uint gGISite;

uint stSlot()
{
    return uint(camera.position.w) & 1u;
}

uint stFamBase(uint fam)
{
    return 460u + (stSlot() * 2u + fam) * 282u;
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

void stSplitDelta(uint64_t d, out uint lo, out uint hi)
{
    lo = uint(d & uint64_t(0xFFFFFFFFu));
    hi = uint(d >> 32);
}

void stRecord(uint fam, uint stratum, uint outcome, uint64_t t0, uint64_t t1)
{
    uint base = stFamBase(fam);
    // Eligible + recorded (identical at fixed rate 1; both counted).
    atomicAdd(stSlots[base + 260u], 1u);
    atomicAdd(stSlots[base + 261u], 1u);
    // Duration, full 64-bit modular difference (unsigned wrap).
    uint dlo, dhi;
    stSplitDelta(t1 - t0, dlo, dhi);
    // Pilot (fixed doubling edges — Q-independent by construction).
    atomicAdd(stSlots[base + stPilotIndex(dlo, dhi)], 1u);
    // Acceptance (manifest edges; active count bounds the search).
    uint ec = (fam == 0u) ? stHead[4] : stHead[5];
    uint hbin = stAcceptBin(fam, dlo, dhi, ec);
    atomicAdd(stSlots[base + 34u + hbin], 1u);
    // Outcome + stratum (stratum index validated by construction:
    // RT site*2+depth <= 11, GI site*2+prov <= 3).
    atomicAdd(stSlots[base + 262u + outcome], 1u); // blocked=0, visible=1
    atomicAdd(stSlots[base + 264u + min(stratum, 15u)], 1u);
}

// RT bracket: site 0 env, 1 tri, 2 res-tri, 3 res-env, 4 punctual.
// shadowVisible + gShadowDepth are in scope at every call site.
#define ST_RT_BEGIN(var) uint64_t var = clockRealtimeEXT()
#define ST_RT_END(var, site) stRecord(0u, (site) * 2u + min(gShadowDepth, 1u), (shadowVisible < 0.5) ? 0u : 1u, var, clockRealtimeEXT())
#define ST_SET_DEPTH(d) gShadowDepth = (d)

// GI bracket: outcome from committedType (gl_RayQueryCommittedIntersectionNoneEXT
// = unoccluded = visible). Site/provenance from ON-only globals.
#define ST_GI_BEGIN(var) uint64_t var = clockRealtimeEXT()
#define ST_GI_END(var) stRecord(1u, gGISite * 2u + min(gGIProv, 1u), (committedType == gl_RayQueryCommittedIntersectionNoneEXT) ? 1u : 0u, var, clockRealtimeEXT())
#define ST_SET_GIPROV(p, c) gGIProv = (p); gGICand = (c)
#define ST_SET_GISITE(s) gGISite = (s)

#else // OFF: everything below expands to nothing; emitted code is original.

#define ST_RT_BEGIN(var)
#define ST_RT_END(var, site)
#define ST_SET_DEPTH(d)
#define ST_GI_BEGIN(var)
#define ST_GI_END(var)
#define ST_SET_GIPROV(p, c)
#define ST_SET_GISITE(s)

#endif // SHADOW_TAIL_DIAG

#endif // RT2_SHADOW_TAIL_GLSL
