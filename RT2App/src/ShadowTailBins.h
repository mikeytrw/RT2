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

namespace shadowtail
{

constexpr uint32_t kPilotBins = 32u;
constexpr uint32_t kPilotCounters = 34u;      // 32 doubling + underflow + overflow
constexpr uint32_t kAcceptanceBinsMax = 225u; // finite bins; +1 overflow
constexpr uint32_t kAcceptanceEdgesMax = kAcceptanceBinsMax + 1u; // 226 edges
constexpr uint32_t kSlots = 2u;               // matches MAX_FRAMES_IN_FLIGHT
constexpr uint32_t kFamilies = 2u;            // 0 = RT shadow, 1 = GI shadow
constexpr uint64_t kUint32Max = 0xFFFFFFFFull;

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

} // namespace shadowtail
