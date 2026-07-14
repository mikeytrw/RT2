// surface_history_shared.glsl — receiver history encoding shared by
// ReSTIR DI (restir_shared.glsl) and ReSTIR GI (restir_gi_shared.glsl).
//
// Defines the SurfaceHistory struct (32 bytes, matches SISurfaceHistory in
// shader_interface.h) plus its oct-encode/decode, make, validate and accessor
// helpers. Kept DI/GI-agnostic: no reservoir type or target-density code here.
//
// Include this AFTER the buffers it references (instanceMeshInfo, indices,
// vertices, uvs, instanceTransforms) have been declared.

#ifndef SURFACE_HISTORY_SHARED_GLSL
#define SURFACE_HISTORY_SHARED_GLSL

// ---- SurfaceHistory struct (32 bytes) ---------------------------------------
struct SurfaceHistory
{
    uvec4 data0;  // x = packed normal oct, y = uint(viewZ), z = materialID, w = packed flags
    uvec4 data1;  // xyz = uint(worldPos.xyz), w = pad
};

// ---- Octahedral normal encoding (preserves full direction) ------------------
vec2 octEncode(vec3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec2 o = n.xy;
    if (n.z < 0.0)
        o = (1.0 - abs(o.yx)) * sign(o.xy);
    return o;
}

vec3 octDecode(vec2 o)
{
    vec3 n = vec3(o.x, o.y, 1.0 - abs(o.x) - abs(o.y));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * sign(n.xy);
    return normalize(n);
}

// ---- Surface history helpers ------------------------------------------------

SurfaceHistory makeSurfaceHistory(vec3 normal, float viewZ, uint matIdx, bool valid,
                                   vec3 worldPos)
{
    SurfaceHistory sh;
    vec2 oct = octEncode(normal);
    uint ox = uint(clamp(oct.x * 0.5 + 0.5, 0.0, 1.0) * 65535.0);
    uint oy = uint(clamp(oct.y * 0.5 + 0.5, 0.0, 1.0) * 65535.0);
    sh.data0.x = ox | (oy << 16u);
    sh.data0.y = floatBitsToUint(viewZ);
    sh.data0.z = matIdx;
    sh.data0.w = valid ? 1u : 0u;
    sh.data1.x = floatBitsToUint(worldPos.x);
    sh.data1.y = floatBitsToUint(worldPos.y);
    sh.data1.z = floatBitsToUint(worldPos.z);
    sh.data1.w = 0u;
    return sh;
}

vec3 surfaceHistoryNormal(SurfaceHistory sh)
{
    uint ox_bits = sh.data0.x & 0xFFFFu;
    uint oy_bits = (sh.data0.x >> 16u) & 0xFFFFu;
    float ox = (float(ox_bits) / 65535.0) * 2.0 - 1.0;
    float oy = (float(oy_bits) / 65535.0) * 2.0 - 1.0;
    return octDecode(vec2(ox, oy));
}

float surfaceHistoryViewZ(SurfaceHistory sh)
{
    return uintBitsToFloat(sh.data0.y);
}

uint surfaceHistoryMatIdx(SurfaceHistory sh)
{
    return sh.data0.z;
}

vec3 surfaceHistoryWorldPos(SurfaceHistory sh)
{
    return vec3(uintBitsToFloat(sh.data1.x),
                uintBitsToFloat(sh.data1.y),
                uintBitsToFloat(sh.data1.z));
}

bool surfaceHistoryValid(SurfaceHistory sh)
{
    return sh.data0.w != 0u;
}

// ---- Surface history validation ---------------------------------------------
// Receiver-based: compares normal, depth, world-position and material ID.
// currentIsSkyOrEmissive excludes pixels that should never carry history.
bool validateSurfaceHistory(SurfaceHistory sh, vec3 currentN, float currentViewZ,
                            uint currentMatIdx, bool currentIsSkyOrEmissive,
                            vec3 currentWorldPos,
                            float depthThreshold, float normalThreshold,
                            float worldPosThreshold)
{
    if (currentIsSkyOrEmissive) return false;
    if (!surfaceHistoryValid(sh)) return false;

    uint histMatIdx = surfaceHistoryMatIdx(sh);
    if (histMatIdx != currentMatIdx) return false;
    if (histMatIdx == 0xFFFFFFFFu) return false;

    vec3 histN = surfaceHistoryNormal(sh);
    if (dot(histN, currentN) < normalThreshold) return false;

    float histZ = surfaceHistoryViewZ(sh);
    float depthDiff = abs(histZ - currentViewZ) / max(abs(currentViewZ), 1e-6);
    if (depthDiff > depthThreshold) return false;

    vec3 histPos = surfaceHistoryWorldPos(sh);
    float posDiff = distance(histPos, currentWorldPos);
    if (posDiff > worldPosThreshold) return false;

    return true;
}

#endif // SURFACE_HISTORY_SHARED_GLSL