#pragma once

// ============================================================================
// ShadowTailBins — CPU-only edge/quantile/inference math for the RT4-T4
// shadow-tail diagnostic (shadow-tail-plan contract, READY 2026-10-13).
//
// Pure C++ (<cstdint>, <cstddef>, <cmath> only): links into RT2Tests with
// no Vulkan/ImGui/Walnut. The GPU side (SSBO writes, clock reads) lives
// in shaders + ShadowTailDiag wiring; this header is the single source
// of truth for every number both sides must agree on, so the contract's
// capacity/edge/quantile rules are pinned by unit tests, not prose.
//
// Invariants (from the contract — do not relax without review):
//  - Pilot: 32 doubling bins P_i = 2^i (i = 0..31) + underflow {0} +
//    overflow [2^32, +inf): 34 counters, Q-independent.
//  - Acceptance: at most 225 finite bins + 1 overflow per family/slot.
//  - Edge generation in 64-bit; limit = min(2*Qhi, UINT32_MAX); stop at
//    the first next edge >= limit, append the limit ONCE, discard later
//    edges. Never generate out-of-range intermediates and clamp only
//    the last element.
//  - A nonzero duration high word ALWAYS routes to overflow (no
//    truncation to the low word).
// ============================================================================

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace shadowtail
{

constexpr uint32_t kPilotBins = 32u;
constexpr uint32_t kPilotCounters = 34u;      // 32 doubling + underflow + overflow
constexpr uint32_t kAcceptanceBinsMax = 225u; // finite bins; +1 overflow
constexpr uint32_t kAcceptanceEdgesMax = kAcceptanceBinsMax + 1u; // 226 edges
constexpr uint32_t kSlots = 2u;               // matches MAX_FRAMES_IN_FLIGHT
constexpr uint32_t kFamilies = 2u;            // 0 = RT shadow, 1 = GI shadow
constexpr uint64_t kUint32Max = 0xFFFFFFFFull;

// SSBO layout constants shared with shaders/shadow_tail.glsl and
// ShadowTailDiag (offsets in u32 words; see that header for the map).
constexpr uint32_t kHeaderU32 = 8u;
constexpr uint32_t kStrataN = 16u;
constexpr uint32_t kOrdN = 16u; // per-invocation ordinal distribution
constexpr uint32_t kCountersN = 4u; // eligible, recorded, blocked, visible
constexpr uint32_t kReservedN = 2u;
constexpr uint32_t kFamStride =
    kPilotCounters + kAcceptanceEdgesMax + kCountersN + kStrataN + kOrdN + kReservedN; // 298
constexpr uint32_t kCtlN = 2u; // positive-control checksums (RT, GI)
constexpr uint32_t kSlotStride = 2u * kFamStride + kCtlN; // 598
constexpr uint32_t kTotalU32 =
    kHeaderU32 + 2u * kAcceptanceEdgesMax + kSlots * kSlotStride; // 1656

// In-family word offsets (mirror shaders/shadow_tail.glsl exactly).
constexpr uint32_t kOffPilot = 0u;
constexpr uint32_t kOffHist = kPilotCounters;                        // 34
constexpr uint32_t kOffEligible = kOffHist + kAcceptanceEdgesMax;    // 260
constexpr uint32_t kOffRecorded = kOffEligible + 1u;                 // 261
constexpr uint32_t kOffBlocked = kOffEligible + 2u;                  // 262
constexpr uint32_t kOffVisible = kOffEligible + 3u;                  // 263
constexpr uint32_t kOffStrata = kOffEligible + 4u;                   // 264
constexpr uint32_t kOffOrd = kOffStrata + kStrataN;                  // 280
// Header indices.
constexpr uint32_t kHeadMagic = 0u, kHeadVersion = 1u, kHeadFrame = 2u;
constexpr uint32_t kHeadMode = 3u, kHeadEdgeRT = 4u, kHeadEdgeGI = 5u;
constexpr uint32_t kHeadVariant = 6u;

// Pilot bin/sentinel codes from PilotBracket.
constexpr int kPilotEmpty = -1;     // N == 0: no samples at all
constexpr int kPilotUnderflow = -2; // p95 rank lands in {0}: precision-inadequate
constexpr int kPilotOverflow = -3;  // p95 rank lands in [2^32, +inf): fixture invalid
// Otherwise returns the doubling index k in [0, 31]: bracket [2^k, 2^(k+1)).
inline int PilotBracket(const uint32_t counts[kPilotCounters], uint64_t* outLo, uint64_t* outHi)
{
    uint64_t total = 0;
    for (uint32_t i = 0; i < kPilotCounters; ++i)
        total += counts[i];
    if (total == 0)
        return kPilotEmpty;
    // Rank is 1-based: ceil(0.95 * N) over the ordered pilot bins
    // (underflow {0}, then P_0..P_31, then overflow).
    const uint64_t rank = (total * 95u + 99u) / 100u; // ceil(0.95*N)
    uint64_t acc = 0;
    acc += counts[0];
    if (rank <= acc)
    {
        if (outLo) *outLo = 0;
        if (outHi) *outHi = 0;
        return kPilotUnderflow;
    }
    for (uint32_t k = 0; k < kPilotBins; ++k)
    {
        acc += counts[1u + k];
        if (rank <= acc)
        {
            if (outLo) *outLo = (k == 0) ? 1ull : (1ull << k);
            if (outHi) *outHi = 1ull << (k + 1u);
            return static_cast<int>(k);
        }
    }
    if (outLo) *outLo = 1ull << 32;
    if (outHi) *outHi = 0; // unbounded
    return kPilotOverflow;
}

// BuildAcceptanceEdges implements the contract invariant exactly:
//   limit = min(2*Qhi, UINT32_MAX); w = max(1, round_half_up(Qhi/128));
//   E_0 = 0; E_1 = Qlo/2 (dropped if zero); narrow E_i = E_1+(i-1)*w
//   while the next edge is < limit; then append limit ONCE and stop.
// edgesOut must hold at least kAcceptanceEdgesMax entries; returns the
// edge count (0 = caller buffer too small — loud failure, never silent
// truncation). Qhi == 0 (degenerate pilot) yields just {0, 1}: the
// confirmation precision rule will reject it downstream.
inline uint32_t BuildAcceptanceEdges(uint64_t qlo, uint64_t qhi,
                                     uint32_t* edgesOut, uint32_t capacity)
{
    if (capacity < kAcceptanceEdgesMax || edgesOut == nullptr)
        return 0;
    const uint64_t limit = (qhi > (kUint32Max / 2u)) ? kUint32Max : (2u * qhi);
    const uint64_t w = (qhi / 128u) + ((qhi % 128u) >= 64u ? 1u : 0u);
    const uint64_t width = (w == 0u) ? 1u : w;
    const uint64_t e1 = qlo / 2u;
    uint32_t n = 0;
    edgesOut[n++] = 0u;
    uint64_t e = 0u;
    if (e1 > 0u)
    {
        // e1 < limit always holds: e1 = Qlo/2 <= Qhi/2 < 2*Qhi, and
        // e1 <= 2^31 < UINT32_MAX. No clamp needed here by construction.
        edgesOut[n++] = static_cast<uint32_t>(e1);
        e = e1;
    }
    for (;;)
    {
        const uint64_t next = e + width;
        if (next >= limit)
        {
            edgesOut[n++] = static_cast<uint32_t>(limit);
            break;
        }
        edgesOut[n++] = static_cast<uint32_t>(next);
        e = next;
        if (n >= kAcceptanceEdgesMax)
            break; // capacity bound: never write past the manifest size
    }
    return n;
}

// BinLookup routes a two-word (hi, lo) duration into a bin index:
//   hi != 0  -> overflow bucket (never truncated to the low word).
//   else linear scan of the monotone edge array; bin i covers
//   [edges[i], edges[i+1]); values >= edges[n-1] land in overflow.
// Returns 0..edgeCount-2 for finite bins, edgeCount-1 for overflow.
inline uint32_t BinLookup(const uint32_t* edges, uint32_t edgeCount,
                          uint32_t lo, uint32_t hi)
{
    const uint32_t overflow = edgeCount - 1u;
    if (hi != 0u)
        return overflow;
    for (uint32_t i = 0; i + 1u < edgeCount; ++i)
    {
        if (lo < edges[i + 1u])
            return i;
    }
    return overflow;
}

// P95Bracket walks a histogram (nbins entries, LAST entry = overflow)
// for rank ceil(0.95*N). Returns the bin index, kP95Empty (N==0), or
// kP95Overflow (rank in the unbounded bucket -> inconclusive).
constexpr int kP95Empty = -1;
constexpr int kP95Overflow = -2;
inline int P95Bracket(const uint64_t* counts, uint32_t nbins, uint64_t total)
{
    if (total == 0)
        return kP95Empty;
    const uint64_t rank = (total * 95u + 99u) / 100u;
    uint64_t acc = 0;
    for (uint32_t i = 0; i < nbins; ++i)
    {
        acc += counts[i];
        if (rank <= acc)
            return (i + 1u == nbins) ? kP95Overflow : static_cast<int>(i);
    }
    return kP95Empty; // sum < total: caller consistency failure, never a pass
}

// PrecisionOk: relative bracket width at the LOWER edge vs the 5% rule.
// width and lowerEdge are in ticks; returns false when the rule cannot
// resolve the margin (e.g. w=1 floor on tiny Q).
inline bool PrecisionOk(uint64_t width, uint64_t lowerEdge)
{
    if (lowerEdge == 0u)
        return false;
    // width/lower <= 0.05  <=>  20*width <= lower (exact integer math).
    return 20u * width <= lowerEdge;
}

// CounterBoundOk: proved pre-run 32-bit bound without overflow-prone
// multiplication (pixels==0 is an invalid configuration, not a pass).
inline bool CounterBoundOk(uint64_t pixels, uint64_t maxCallsPerPx)
{
    if (pixels == 0u)
        return false;
    return maxCallsPerPx < (0x80000000ull / pixels);
}

// HistSumEqualsRecorded: the corrected consistency check — histogram
// bin sum must equal RECORDED (not eligible); recorded <= eligible is
// checked by the caller from the header counters.
inline bool HistSumEqualsRecorded(const uint64_t* binCounts, uint32_t nbins,
                                  uint64_t recorded)
{
    uint64_t sum = 0;
    for (uint32_t i = 0; i < nbins; ++i)
        sum += binCounts[i];
    return sum == recorded;
}

// One-sided 95% t critical values, df = 1..8 (for B = 2..9 blocks).
inline bool T95Crit(int blocks, double* out)
{
    static const double kTable[9] = {
        0.0,         // unused (blocks < 2 invalid)
        6.31375151,  // df 1
        2.91998558,  // df 2
        2.35336343,  // df 3 (B = 4)
        2.13184679,  // df 4
        2.01504837,  // df 5
        1.94318028,  // df 6
        1.89457861,  // df 7
        1.85954804,  // df 8
    };
    if (blocks < 2 || blocks > 9 || out == nullptr)
        return false;
    *out = kTable[blocks - 1];
    return true;
}

// PairedT95UpperBound: exp(mean(x) + t(.95,B-1)*sd(x)/sqrt(B)) over the
// endpoint-conservative block log-ratios. Sample sd (ddof=1). Returns
// false for B < 2, B > 9, null input, or non-finite data — never NaN.
inline bool PairedT95UpperBound(const double* x, int blocks, double* out)
{
    if (x == nullptr || out == nullptr)
        return false;
    double t = 0.0;
    if (!T95Crit(blocks, &t))
        return false;
    double mean = 0.0;
    for (int i = 0; i < blocks; ++i)
    {
        if (!(x[i] > -1e308 && x[i] < 1e308))
            return false;
        mean += x[i];
    }
    mean /= blocks;
    double var = 0.0;
    for (int i = 0; i < blocks; ++i)
    {
        const double d = x[i] - mean;
        var += d * d;
    }
    var /= (blocks - 1);
    *out = std::exp(mean + t * std::sqrt(var / blocks));
    return true;
}

// ---------------------------------------------------------------------------
// SSBO index authority (single formula shared conceptually with
// shaders/shadow_tail.glsl stFamBase/stCtlBase — the GLSL spells the
// same arithmetic; these CPU functions pin it under unit test for ALL
// slots/families/controls, and any divergence is a hard contract break).
// Contiguous per-slot blocks (slot*598 + fam*298); indices relative to
// stSlots (NOT including the 460 header/edge words).
// ---------------------------------------------------------------------------
inline uint32_t StSlotOffset(uint32_t slot, uint32_t fam)
{
    return slot * kSlotStride + fam * kFamStride;
}
inline uint32_t StCtlOffset(uint32_t slot)
{
    return slot * kSlotStride + 2u * kFamStride;
}
inline uint32_t StSlotCapacity()
{
    return kSlots * kSlotStride; // 1196 u32 in stSlots
}

// ---------------------------------------------------------------------------
// Frozen edge manifest (fixed schema, strict hand-rolled parse — zero
// dependencies so calibration tooling and unit tests share it):
//   {"edgesRT":[e0,e1,...],"edgesGI":[...]}
// Each array: 2..226 monotone strictly increasing uint32 values starting
// at 0. Anything else (unsorted, duplicate, out of range, wrong types,
// trailing garbage) is rejected with a reason — never defaulted.
// ---------------------------------------------------------------------------
inline bool ParseEdgeManifest(const std::string& text,
                              std::vector<uint32_t>& edgesRT,
                              std::vector<uint32_t>& edgesGI,
                              std::string& err)
{
    edgesRT.clear();
    edgesGI.clear();
    size_t pos = 0;
    const size_t n = text.size();
    auto skipWs = [&]() {
        while (pos < n && (text[pos] == ' ' || text[pos] == '\t' ||
                           text[pos] == '\n' || text[pos] == '\r'))
            ++pos;
    };
    auto expectLit = [&](const char* lit) -> bool {
        for (const char* p = lit; *p; ++p, ++pos)
        {
            if (pos >= n || text[pos] != *p)
                return false;
        }
        return true;
    };
    // Parses one [u0,u1,...] array with full validation. No negatives
    // (a '-' fails the digit check), no floats ('.' fails), no overflow
    // past 2^32-1 (accumulate in uint64 and range-check).
    auto parseArray = [&](std::vector<uint32_t>& out) -> bool {
        skipWs();
        if (pos >= n || text[pos] != '[')
        {
            err = "expected '['";
            return false;
        }
        ++pos;
        skipWs();
        if (pos < n && text[pos] == ']')
        {
            ++pos;
            err = "empty edge array";
            return false;
        }
        for (;;)
        {
            skipWs();
            if (pos >= n || text[pos] < '0' || text[pos] > '9')
            {
                err = "expected unsigned integer";
                return false;
            }
            uint64_t v = 0;
            while (pos < n && text[pos] >= '0' && text[pos] <= '9')
            {
                v = v * 10u + static_cast<uint64_t>(text[pos] - '0');
                if (v > kUint32Max)
                {
                    err = "edge value exceeds u32";
                    return false;
                }
                ++pos;
            }
            if (!out.empty() && v <= out.back())
            {
                err = "edges must be strictly increasing";
                return false;
            }
            out.push_back(static_cast<uint32_t>(v));
            if (out.size() > kAcceptanceEdgesMax)
            {
                err = "too many edges";
                return false;
            }
            skipWs();
            if (pos >= n)
            {
                err = "unterminated array";
                return false;
            }
            if (text[pos] == ',')
            {
                ++pos;
                continue;
            }
            if (text[pos] == ']')
            {
                ++pos;
                break;
            }
            err = "expected ',' or ']'";
            return false;
        }
        if (out.size() < 2u)
        {
            err = "fewer than 2 edges";
            return false;
        }
        if (out[0] != 0u)
        {
            err = "first edge must be 0";
            return false;
        }
        return true;
    };

    skipWs();
    if (!expectLit("{\"edgesRT\":")) { err = "expected {\"edgesRT\":"; return false; }
    if (!parseArray(edgesRT)) return false;
    skipWs();
    if (!expectLit(",\"edgesGI\":")) { err = "expected ,\"edgesGI\":"; return false; }
    if (!parseArray(edgesGI)) return false;
    skipWs();
    if (!expectLit("}")) { err = "expected closing }"; return false; }
    skipWs();
    if (pos != n) { err = "trailing content"; return false; }
    return true;
}

// ---------------------------------------------------------------------------
// Snapshot validation + serialization core (pure: operates on caller-owned
// word arrays, no Vulkan). ShadowTailDiag.cpp is a thin lifecycle wrapper
// around these; unit tests pin golden outputs here, and the python runner
// self-test embeds the SAME golden strings (paired by comment) so the
// actual serialized path — not hand-written JSON — is integration-tested.
// ---------------------------------------------------------------------------

// ValidateSnapshotWords checks structural integrity AND acceptance state:
//   magic, version, frame equality, mode in {1,2}, edge counts within
//   capacity and equal to the staged expectation, variant tag.
// Returns true only for acceptance-usable snapshots. Pilot (zero edges)
// and control (mode 2) snapshots return FALSE with reason set — but
// their data words are ALWAYS valid for recovery (outData assigned
// whenever magic/version/frame hold). copies nwords on structural pass.
// reasonOut receives a short machine-readable code (never empty on false).
inline bool ValidateSnapshotWords(const uint32_t* words, uint32_t nwords,
                                  uint64_t frameIndex,
                                  uint32_t expEdgeRT, uint32_t expEdgeGI,
                                  uint32_t headOut[kHeaderU32],
                                  uint32_t* dataOut, uint32_t dataCap,
                                  std::string& reasonOut)
{
    reasonOut.clear();
    if (words == nullptr || headOut == nullptr || dataOut == nullptr)
    {
        reasonOut = "null buffer";
        return false;
    }
    if (nwords < kTotalU32)
    {
        reasonOut = "short buffer";
        return false;
    }
    for (uint32_t i = 0; i < kHeaderU32; ++i)
        headOut[i] = words[i];
    if (headOut[0] != 0x53484457u) { reasonOut = "bad magic"; return false; }
    if (headOut[1] != 1u) { reasonOut = "version mismatch"; return false; }
    if (headOut[2] != static_cast<uint32_t>(frameIndex & 0xFFFFFFFFull))
    {
        reasonOut = "frame mismatch";
        return false;
    }
    if (headOut[3] != 1u && headOut[3] != 2u)
    {
        reasonOut = "mode is not ON";
        return false;
    }
    if (headOut[6] != 1u) { reasonOut = "variant tag mismatch"; return false; }
    // Data words are ALWAYS recoverable past this point (pilot + control
    // carry real counters); only acceptance validity is still open.
    if (dataCap < kTotalU32)
    {
        reasonOut = "data capacity short";
        return false;
    }
    for (uint32_t i = 0; i < kTotalU32; ++i)
        dataOut[i] = words[i];
    if (headOut[4] < 2u || headOut[4] > kAcceptanceEdgesMax ||
        headOut[5] < 2u || headOut[5] > kAcceptanceEdgesMax)
    {
        // Pilot state (zero edges) or corrupt counts: data preserved,
        // acceptance closed. Pilot-only output is never valid.
        reasonOut = (headOut[4] == 0u && headOut[5] == 0u)
                        ? "pilot-only: no manifest staged"
                        : "edge counts outside manifest capacity";
        return false;
    }
    if (headOut[4] != expEdgeRT || headOut[5] != expEdgeGI)
    {
        reasonOut = "edge counts differ from staged manifest";
        return false;
    }
    if (headOut[3] == 2u)
    {
        // Positive-control state: well-formed and data-bearing, but never
        // an acceptance snapshot (separate sensitivity gate consumes it).
        reasonOut = "positive-control mode";
        return false;
    }
    return true;
}

// SnapshotJson serializes head + FULL counter region + pilot flag + mode.
// Data is always present (see above); `valid` distinguishes structural
// acceptance usability, `pilot` marks the no-manifest state explicitly.
inline std::string SnapshotJson(uint64_t frameIndex, uint32_t slot,
                                uint64_t serial, bool valid,
                                const std::string& reason,
                                const uint32_t head[kHeaderU32],
                                const uint32_t* data)
{
    std::string out = "{\"schema\":\"rt2.shadow-tail.v1\",\"frame\":";
    out += std::to_string(frameIndex);
    out += ",\"slot\":" + std::to_string(slot);
    out += ",\"serial\":" + std::to_string(serial);
    out += ",\"valid\":";
    out += (valid ? "true" : "false");
    out += ",\"pilot\":";
    out += ((head[4] == 0u && head[5] == 0u) ? "true" : "false");
    out += ",\"mode\":" + std::to_string(head[3]);
    if (!valid)
    {
        out += ",\"reason\":\"" + reason + "\"";
    }
    out += ",\"head\":[";
    for (uint32_t i = 0; i < kHeaderU32; ++i)
    {
        if (i) out += ",";
        out += std::to_string(head[i]);
    }
    constexpr uint32_t kDataBase = kHeaderU32 + 2u * kAcceptanceEdgesMax;
    constexpr uint32_t kDataCount = kTotalU32 - kDataBase;
    out += "],\"data\":[";
    for (uint32_t i = 0; i < kDataCount; ++i)
    {
        if (i) out += ",";
        out += std::to_string(data[kDataBase + i]);
    }
    out += "]}";
    return out;
}

} // namespace shadowtail
