// ============================================================================
// RT4-T4 shadow-tail diagnostic: edge/quantile/inference contract tests.
//
// Pins ShadowTailBins.h (RT2App/src) — the shared CPU/GPU number contract
// for the shadow-tail plan (READY 2026-10-13). Every invariant the frozen
// manifest depends on is tested here: the critical limit rule
// (limit=min(2*Qhi,UINT32_MAX), stop-append-once), all 32 nonzero pilot
// brackets, zero/high-word/precision/overflow cases, capacity bounds,
// and the paired-t acceptance math. A failure here blocks the gate
// before any GPU run, by design.
// ============================================================================

#include <doctest/doctest.h>
#include "ShadowTailBins.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace
{
constexpr uint64_t kU32Max = 0xFFFFFFFFull;
} // namespace

TEST_CASE("ShadowTailBins pilot brackets cover all 32 nonzero doublings")
{
    // For every k in [0, 31]: bracket [2^k, 2^(k+1)) must build strictly
    // monotone edges ending EXACTLY at limit = min(2*Qhi, U32MAX),
    // fitting the 226-edge manifest capacity.
    for (uint32_t k = 0; k < 32u; ++k)
    {
        const uint64_t qlo = 1ull << k;
        const uint64_t qhi = 1ull << (k + 1u);
        const uint64_t limit = (qhi > (kU32Max / 2u)) ? kU32Max : (2u * qhi);
        uint32_t edges[shadowtail::kAcceptanceEdgesMax] = {};
        const uint32_t n = shadowtail::BuildAcceptanceEdges(qlo, qhi, edges,
                                                            shadowtail::kAcceptanceEdgesMax);
        CHECK(n >= 2u);
        CHECK(n <= shadowtail::kAcceptanceEdgesMax);
        CHECK(edges[n - 1u] == static_cast<uint32_t>(limit));
        for (uint32_t i = 0; i + 1u < n; ++i)
            CHECK(edges[i] < edges[i + 1u]); // strictly monotone integers
    }
}

TEST_CASE("ShadowTailBins Q10000 worked example matches recorded arithmetic")
{
    // Pilot bracket [8192, 16384): w=128, 226 edges, Elast=32768.
    uint32_t edges[shadowtail::kAcceptanceEdgesMax] = {};
    const uint32_t n = shadowtail::BuildAcceptanceEdges(8192u, 16384u, edges,
                                                        shadowtail::kAcceptanceEdgesMax);
    CHECK(n == 226u);
    CHECK(edges[0] == 0u);
    CHECK(edges[1] == 4096u);
    CHECK(edges[n - 1u] == 32768u);
    // Precision is width/bin-lower: 128/9984 ~= 1.29% near Q, 1.56% worst.
    CHECK(shadowtail::PrecisionOk(128u, 9984u));
    CHECK(shadowtail::PrecisionOk(128u, 8192u));
    CHECK_FALSE(shadowtail::PrecisionOk(128u, 2000u)); // 6.4% > 5%
}

TEST_CASE("ShadowTailBins degenerate Q8 is precision-inadequate")
{
    // w=1 floor binds: 1/8 = 12.5% > 5% -> INCOMPLETE by rule (kept honest).
    uint32_t edges[shadowtail::kAcceptanceEdgesMax] = {};
    const uint32_t n = shadowtail::BuildAcceptanceEdges(8u, 16u, edges,
                                                        shadowtail::kAcceptanceEdgesMax);
    CHECK(n == 30u);
    CHECK(edges[n - 1u] == 32u);
    CHECK_FALSE(shadowtail::PrecisionOk(1u, 8u));
}

TEST_CASE("ShadowTailBins u32 clamp engages at Q2pow30")
{
    uint32_t edges[shadowtail::kAcceptanceEdgesMax] = {};
    const uint32_t n = shadowtail::BuildAcceptanceEdges(1ull << 30, 1ull << 31, edges,
                                                        shadowtail::kAcceptanceEdgesMax);
    CHECK(n <= shadowtail::kAcceptanceEdgesMax);
    CHECK(edges[n - 1u] == 0xFFFFFFFFu); // clamped limit, overflow decides
    for (uint32_t i = 0; i + 1u < n; ++i)
        CHECK(edges[i] < edges[i + 1u]);
}

TEST_CASE("ShadowTailBins capacity backstop never truncates silently")
{
    // Undersized caller buffer -> loud 0, never a truncated edge list.
    uint32_t edges[16] = {};
    CHECK(shadowtail::BuildAcceptanceEdges(8192u, 16384u, edges, 16u) == 0u);
    CHECK(shadowtail::BuildAcceptanceEdges(8192u, 16384u, nullptr,
                                           shadowtail::kAcceptanceEdgesMax) == 0u);
}

TEST_CASE("ShadowTailBins pilot bracket codes")
{
    uint32_t counts[shadowtail::kPilotCounters] = {};
    uint64_t lo = 0, hi = 0;
    CHECK(shadowtail::PilotBracket(counts, &lo, &hi) == shadowtail::kPilotEmpty);

    counts[0] = 100u; // all mass in {0}: zero-p95 -> precision-inadequate
    CHECK(shadowtail::PilotBracket(counts, &lo, &hi) == shadowtail::kPilotUnderflow);

    for (uint32_t i = 0; i < shadowtail::kPilotCounters; ++i)
        counts[i] = 0u;
    counts[33] = 50u; // overflow [2^32, +inf): fixture invalid
    int code = shadowtail::PilotBracket(counts, &lo, &hi);
    CHECK(code == shadowtail::kPilotOverflow);
    CHECK(lo == (1ull << 32));

    for (uint32_t i = 0; i < shadowtail::kPilotCounters; ++i)
        counts[i] = 0u;
    counts[1u + 13u] = 1000u; // doubling bin k=13: [8192, 16384)
    code = shadowtail::PilotBracket(counts, &lo, &hi);
    CHECK(code == 13);
    CHECK(lo == 8192u);
    CHECK(hi == 16384u);
}

TEST_CASE("ShadowTailBins high word always routes to overflow")
{
    // Bin lookup must never truncate to the low word.
    const uint32_t edges[] = { 0u, 4096u, 8192u, 16384u, 32768u };
    constexpr uint32_t kEdges = 5u; // 4 finite bins + overflow index 4
    CHECK(shadowtail::BinLookup(edges, kEdges, 100u, 0u) == 0u);
    CHECK(shadowtail::BinLookup(edges, kEdges, 5000u, 0u) == 1u);
    CHECK(shadowtail::BinLookup(edges, kEdges, 40000u, 0u) == 4u);
    CHECK(shadowtail::BinLookup(edges, kEdges, 0u, 1u) == 4u); // hi != 0
    CHECK(shadowtail::BinLookup(edges, kEdges, 0xFFFFFFFFu, 7u) == 4u);
}

TEST_CASE("ShadowTailBins p95 bracket and precision rules")
{
    const uint64_t counts[] = { 0u, 9500u, 400u, 100u }; // N=10000, 3 finite + overflow
    CHECK(shadowtail::P95Bracket(counts, 4u, 10000u) == 1); // rank 9500 -> bin 1
    const uint64_t countsOv[] = { 0u, 100u, 100u, 9800u };
    CHECK(shadowtail::P95Bracket(countsOv, 4u, 10000u) == shadowtail::kP95Overflow);
    CHECK(shadowtail::P95Bracket(counts, 4u, 0u) == shadowtail::kP95Empty);
    // PrecisionOk is width/lower <= 5% in exact integer math.
    CHECK(shadowtail::PrecisionOk(128u, 9984u));   // ~= 1.29%
    CHECK(shadowtail::PrecisionOk(128u, 8192u));   // ~= 1.56%
    CHECK_FALSE(shadowtail::PrecisionOk(1u, 8u));  // 12.5%
    CHECK_FALSE(shadowtail::PrecisionOk(128u, 0u)); // degenerate lower edge
}

TEST_CASE("ShadowTailBins counter bound without overflow-prone multiply")
{
    CHECK(shadowtail::CounterBoundOk(960ull * 540ull, 64u)); // 33M < 2^31
    CHECK_FALSE(shadowtail::CounterBoundOk(0x80000000ull, 1u)); // == 2^31, not <
    CHECK_FALSE(shadowtail::CounterBoundOk(0u, 64u));           // invalid config
    CHECK_FALSE(shadowtail::CounterBoundOk(1ull << 40, 1ull << 10)); // no UB
    // Histogram sum must equal RECORDED (not eligible) — corrected equality.
    const uint64_t bins[] = { 900u, 90u, 10u };
    CHECK(shadowtail::HistSumEqualsRecorded(bins, 3u, 1000u));
    CHECK_FALSE(shadowtail::HistSumEqualsRecorded(bins, 3u, 2000u));
}

TEST_CASE("ShadowTailBins paired-t 95% upper bound")
{
    // Zero-variance blocks: bound == exp(mean) exactly.
    const double flat[] = { 0.02, 0.02, 0.02, 0.02 };
    double out = 0.0;
    CHECK(shadowtail::PairedT95UpperBound(flat, 4, &out));
    CHECK(std::fabs(out - std::exp(0.02)) < 1e-12);
    // Hand-computed reference (python): mean .0125, sd .0170782513.
    const double x[] = { 0.01, 0.02, -0.01, 0.03 };
    CHECK(shadowtail::PairedT95UpperBound(x, 4, &out));
    CHECK(std::fabs(out - 1.0331327241) < 1e-9);
    // t(.95,3) = 2.35336343 pins B = 4.
    double t = 0.0;
    CHECK(shadowtail::T95Crit(4, &t));
    CHECK(std::fabs(t - 2.35336343) < 1e-8);
    // Loud failures, never NaN.
    CHECK_FALSE(shadowtail::PairedT95UpperBound(x, 1, &out));
    CHECK_FALSE(shadowtail::PairedT95UpperBound(x, 10, &out));
    CHECK_FALSE(shadowtail::PairedT95UpperBound(nullptr, 4, &out));
    const double bad[] = { 0.01, 0.02, std::numeric_limits<double>::infinity(), 0.03 };
    CHECK_FALSE(shadowtail::PairedT95UpperBound(bad, 4, &out));
}

TEST_CASE("ShadowTailBins SSBO index authority covers all slots/families/controls")
{
    // Mirrors shaders/shadow_tail.glsl stFamBase/stCtlBase EXACTLY
    // (contiguous per-slot blocks; relative to stSlots, no header
    // offset). Any divergence is a hard contract break; the old +460
    // bug and the interleaved-stride bug both fail every CHECK here.
    CHECK(shadowtail::StSlotOffset(0u, 0u) == 0u);
    CHECK(shadowtail::StSlotOffset(0u, 1u) == 298u);
    CHECK(shadowtail::StSlotOffset(1u, 0u) == 598u);
    CHECK(shadowtail::StSlotOffset(1u, 1u) == 896u);
    CHECK(shadowtail::StCtlOffset(0u) == 596u);
    CHECK(shadowtail::StCtlOffset(1u) == 1194u);
    CHECK(shadowtail::StSlotCapacity() == 1196u);
    // Every legal word index lands inside the array (no OOB by construction).
    for (uint32_t s = 0; s < 2u; ++s)
        for (uint32_t f = 0; f < 2u; ++f)
        {
            const uint32_t base = shadowtail::StSlotOffset(s, f);
            CHECK(base + 33u < 1196u);   // pilot last
            CHECK(base + 259u < 1196u);  // hist last (34+225)
            CHECK(base + 263u < 1196u);  // visible
            CHECK(base + 279u < 1196u);  // strata last
            CHECK(base + 295u < 1196u);  // ord last
        }
    CHECK(shadowtail::StCtlOffset(1u) + 1u < 1196u); // control last
    // Stride consistency with the layout constants.
    CHECK(shadowtail::StSlotOffset(1u, 0u) - shadowtail::StSlotOffset(0u, 0u)
          == shadowtail::kSlotStride);
    CHECK(shadowtail::kSlotStride == 598u);
}

TEST_CASE("ShadowTailBins edge manifest strict parsing")
{
    std::vector<uint32_t> rt, gi;
    std::string err;
    CHECK(shadowtail::ParseEdgeManifest(
        "{\"edgesRT\":[0,4096,8192,16384,32768],\"edgesGI\":[0,100,200]}",
        rt, gi, err));
    CHECK(rt.size() == 5u);
    CHECK(rt.back() == 32768u);
    CHECK(gi.size() == 3u);
    CHECK(err.empty());

    const char* rejects[] = {
        "{\"edgesRT\":[0,100,100],\"edgesGI\":[0,1]}",   // duplicate
        "{\"edgesRT\":[0,200,100],\"edgesGI\":[0,1]}",   // unsorted
        "{\"edgesRT\":[4,100],\"edgesGI\":[0,1]}",       // first != 0
        "{\"edgesRT\":[0],\"edgesGI\":[0,1]}",           // fewer than 2
        "{\"edgesRT\":[],\"edgesGI\":[0,1]}",            // empty
        "{\"edgesRT\":[0,-4],\"edgesGI\":[0,1]}",        // negative
        "{\"edgesRT\":[0,4.5],\"edgesGI\":[0,1]}",       // float
        "{\"edgesRT\":[0,4294967296],\"edgesGI\":[0,1]}",// u32 overflow
        "{\"edgesRT\":[0,4],\"edgesGI\":[0,1]}trailing", // trailing content
        "{\"edgesRT\":[0,4],\"edgesGI\":[0,1]",          // unterminated
        "{\"edgesRT\":[0,4],\"edgesGI\":[0,1],}",        // trailing comma brace
        "{\"edgesRT\":[0,4,],\"edgesGI\":[0,1]}",        // trailing comma array
        "{\"edgesRT\":[0,4]}",                           // missing GI
        "not json at all",                               // garbage
        "",                                              // empty
    };
    for (const char* bad : rejects)
    {
        rt.clear();
        gi.clear();
        err.clear();
        CHECK_FALSE(shadowtail::ParseEdgeManifest(bad, rt, gi, err));
        CHECK_FALSE(err.empty()); // every rejection carries a reason
    }
    // 227 edges exceeds capacity.
    std::string big = "{\"edgesRT\":[0";
    for (uint32_t i = 1; i < 227u; ++i)
    {
        big += ",";
        big += std::to_string(i * 128u);
    }
    big += "],\"edgesGI\":[0,1]}";
    CHECK_FALSE(shadowtail::ParseEdgeManifest(big, rt, gi, err));
}

TEST_CASE("ShadowTailBins snapshot validation separates pilot/control/acceptance")
{
    // Word buffer sized to the frozen total; tests fill header + slot 0.
    std::vector<uint32_t> words(shadowtail::kTotalU32, 0u);
    uint32_t head[shadowtail::kHeaderU32] = {};
    uint32_t data[shadowtail::kTotalU32] = {};
    std::string reason;
    auto base = [&](uint32_t slot, uint32_t fam) {
        return shadowtail::kHeaderU32 + 2u * shadowtail::kAcceptanceEdgesMax
               + slot * shadowtail::kSlotStride + fam * shadowtail::kFamStride;
    };

    // Pilot state: no manifest staged (edge counts zero), real counters.
    words[0] = 0x53484457u;
    words[1] = 1u;
    words[2] = 41u;
    words[3] = 1u;
    words[6] = 1u;
    words[base(0u, 0u) + 260u] = 100u; // eligible RT
    words[base(0u, 0u) + 261u] = 100u; // recorded RT
    words[base(0u, 0u) + 1u] = 100u;   // pilot bin k=13-ish mass
    CHECK_FALSE(shadowtail::ValidateSnapshotWords(
        words.data(), shadowtail::kTotalU32, 41u, 5u, 3u, head, data,
        shadowtail::kTotalU32, reason));
    CHECK(reason == "pilot-only: no manifest staged");
    CHECK(data[base(0u, 0u) + 260u] == 100u); // data ALWAYS recovered

    // Positive-control state: edges staged, mode 2.
    words[4] = 5u;
    words[5] = 3u;
    words[3] = 2u;
    CHECK_FALSE(shadowtail::ValidateSnapshotWords(
        words.data(), shadowtail::kTotalU32, 41u, 5u, 3u, head, data,
        shadowtail::kTotalU32, reason));
    CHECK(reason == "positive-control mode");
    CHECK(data[base(0u, 0u) + 261u] == 100u);

    // Acceptance state: mode 1, matching edges.
    words[3] = 1u;
    CHECK(shadowtail::ValidateSnapshotWords(
        words.data(), shadowtail::kTotalU32, 41u, 5u, 3u, head, data,
        shadowtail::kTotalU32, reason));
    CHECK(reason.empty());

    // Structural failures never expose data.
    words[0] = 0xDEADBEEFu;
    CHECK_FALSE(shadowtail::ValidateSnapshotWords(
        words.data(), shadowtail::kTotalU32, 41u, 5u, 3u, head, data,
        shadowtail::kTotalU32, reason));
    CHECK(reason == "bad magic");
    words[0] = 0x53484457u;
    CHECK_FALSE(shadowtail::ValidateSnapshotWords(
        words.data(), shadowtail::kTotalU32, 99u, 5u, 3u, head, data,
        shadowtail::kTotalU32, reason));
    CHECK(reason == "frame mismatch");
}

TEST_CASE("ShadowTailBins snapshot JSON golden file roundtrip")
{
    // Paired with run_shadow_tail.py, which READS these files (no embedded
    // literals): the python derive/control integration tests consume the
    // ACTUAL serialized output of SnapshotJson. Regenerate with
    // ST_WRITE_GOLDENS=1 (writes RT2Tests/src/shadowtail_golden_*.json
    // relative to the repo-root CWD the suite runs from); the committed
    // files are then compared byte-for-byte here. A format change fails
    // this test until goldens are explicitly regenerated AND the python
    // consumer re-verified in the same commit.
    auto buildPilot = []() {
        uint32_t head[shadowtail::kHeaderU32] = {
            0x53484457u, 1u, 41u, 1u, 0u, 0u, 1u, 0u
        };
        uint32_t data[shadowtail::kTotalU32] = {};
        data[460u + 260u] = 100u; // slot 0 RT eligible
        data[460u + 261u] = 100u; // slot 0 RT recorded
        data[460u + 1u] = 100u;   // slot 0 RT pilot bin
        return shadowtail::SnapshotJson(
            41u, 0u, 7u, false, "pilot-only: no manifest staged", head, data);
    };
    auto buildControl = []() {
        uint32_t chead[shadowtail::kHeaderU32] = {
            0x53484457u, 1u, 42u, 2u, 5u, 3u, 1u, 1u
        };
        uint32_t cdata[shadowtail::kTotalU32] = {};
        cdata[460u + 598u + 596u + 1u] = 305u; // slot 1 GI control word
        return shadowtail::SnapshotJson(
            42u, 1u, 9u, false, "positive-control mode", chead, cdata);
    };
    const char* genDir = std::getenv("ST_WRITE_GOLDENS");
    if (genDir != nullptr)
    {
        // Generation mode: write the goldens called out above. This path
        // is for explicit regeneration only, never for passing silently.
        std::string pilotPath = std::string(genDir) + "/shadowtail_golden_pilot.json";
        std::string ctlPath = std::string(genDir) + "/shadowtail_golden_control.json";
        std::ofstream pilotOut(pilotPath, std::ios::binary | std::ios::trunc);
        std::ofstream ctlOut(ctlPath, std::ios::binary | std::ios::trunc);
        REQUIRE_MESSAGE(pilotOut.good(), "cannot write golden pilot file");
        REQUIRE_MESSAGE(ctlOut.good(), "cannot write golden control file");
        pilotOut << buildPilot();
        ctlOut << buildControl();
        pilotOut.flush();
        ctlOut.flush();
        CHECK(pilotOut.good());
        CHECK(ctlOut.good());
        return;
    }
    // Verification mode: byte-compare live output against committed goldens
    // (repo-root CWD, same as fixture resolution for the whole suite).
    auto readFile = [](const char* path, std::string& out) {
        std::ifstream in(path, std::ios::binary);
        if (!in.good())
            return false;
        out.assign(std::istreambuf_iterator<char>(in),
                   std::istreambuf_iterator<char>());
        return true;
    };
    std::string pilotGold, ctlGold;
    CHECK_MESSAGE(readFile("RT2Tests/src/shadowtail_golden_pilot.json", pilotGold),
                  "missing golden pilot JSON — regenerate with ST_WRITE_GOLDENS=1 "
                  "pointing at RT2Tests/src (suite runs from the repo root)");
    CHECK_MESSAGE(readFile("RT2Tests/src/shadowtail_golden_control.json", ctlGold),
                  "missing golden control JSON — regenerate as above");
    if (!pilotGold.empty())
        CHECK(buildPilot() == pilotGold);
    if (!ctlGold.empty())
        CHECK(buildControl() == ctlGold);
}

TEST_CASE("ShadowTailBins shader-clock opt-in truth table")
{
    // Mirrors the Walnut device-create decision (untestable there — no
    // Vulkan in this closure): enable iff supported && requested &&
    // extension present. In particular supported-but-unrequested and
    // requested-but-unsupported must both stay off (ordinary launches
    // never request the feature, with or without the extension).
    CHECK(shadowtail::ShaderClockEnable(true, true, true));
    CHECK_FALSE(shadowtail::ShaderClockEnable(true, true, false));
    CHECK_FALSE(shadowtail::ShaderClockEnable(true, false, true));
    CHECK_FALSE(shadowtail::ShaderClockEnable(true, false, false));
    CHECK_FALSE(shadowtail::ShaderClockEnable(false, true, true));
    CHECK_FALSE(shadowtail::ShaderClockEnable(false, true, false));
    CHECK_FALSE(shadowtail::ShaderClockEnable(false, false, true));
    CHECK_FALSE(shadowtail::ShaderClockEnable(false, false, false));
}

TEST_CASE("ShadowTailBins wrapper invalid readbacks emit bounded JSON")
{
    // End-to-end composition of the two thin wrappers in
    // ShadowTailDiag.cpp (SnapshotFrom = ValidateSnapshotWords + assign;
    // SnapshotToJson = SnapshotJson with zero-fill for short payloads).
    // ShadowTailDiag.cpp itself needs Vulkan and cannot link into this
    // CPU-only suite, so this test executes the EXACT same call sequence
    // on the shared header core and asserts EVERY serialized field —
    // schema/frame/slot/serial/valid/pilot/mode/reason/head/full data.
    // Structural failures must serialize explicit zeros, never
    // uninitialized words; pilot/control failures must keep real data.
    auto wrapperJson = [](const uint32_t* words, uint32_t nwords,
                          uint64_t frame, uint32_t slot, uint64_t serial,
                          uint32_t expRT, uint32_t expGI) {
        uint32_t head[shadowtail::kHeaderU32] = {};
        uint32_t data[shadowtail::kTotalU32] = {};
        std::string reason = "no snapshot read yet";
        bool valid = false;
        if (words != nullptr && nwords >= shadowtail::kTotalU32)
        {
            valid = shadowtail::ValidateSnapshotWords(
                words, nwords, frame, expRT, expGI, head, data,
                shadowtail::kTotalU32, reason);
        }
        else if (words == nullptr)
        {
            reason = "null buffer";
        }
        else
        {
            reason = "short buffer";
        }
        return shadowtail::SnapshotJson(frame, slot, serial, valid, reason,
                                        head, data);
    };
    auto dataAllZero = [](const std::string& json) {
        const std::string key = "\"data\":[";
        const size_t at = json.find(key);
        if (at == std::string::npos)
            return false;
        const size_t end = json.find(']', at);
        if (end == std::string::npos)
            return false;
        const std::string body =
            json.substr(at + key.size(), end - at - key.size());
        size_t pos = 0;
        size_t count = 0;
        while (pos <= body.size())
        {
            const size_t comma = body.find(',', pos);
            const std::string tok = body.substr(
                pos, comma == std::string::npos ? comma : comma - pos);
            if (tok != "0")
                return false;
            ++count;
            if (comma == std::string::npos)
                break;
            pos = comma + 1;
        }
        return count == 1196u; // full counter region, bounded payload
    };
    auto headField = [](const std::string& json, uint32_t idx) {
        const std::string key = "\"head\":[";
        const size_t at = json.find(key);
        REQUIRE_MESSAGE(at != std::string::npos, "head array missing");
        const size_t end = json.find(']', at);
        REQUIRE_MESSAGE(end != std::string::npos, "head array unterminated");
        std::string body =
            json.substr(at + key.size(), end - at - key.size()) + ",";
        for (uint32_t i = 0; i < idx; ++i)
        {
            const size_t comma = body.find(',');
            REQUIRE_MESSAGE(comma != std::string::npos, "head field missing");
            body = body.substr(comma + 1);
        }
        return body.substr(0, body.find(','));
    };

    std::vector<uint32_t> words(shadowtail::kTotalU32, 0u);
    words[0] = 0x53484457u; words[1] = 1u; words[2] = 41u;
    words[3] = 1u; words[4] = 5u; words[5] = 3u; words[6] = 1u;
    words[460u + 260u] = 100u; // eligible behind every case below
    words[460u + 261u] = 100u; // recorded behind every case below

    // Bad magic: head echoes the corrupt word, data is all zeros.
    words[0] = 0xDEADBEEFu;
    std::string badMagic =
        wrapperJson(words.data(), shadowtail::kTotalU32, 41u, 0u, 7u, 5u, 3u);
    CHECK(badMagic.find("\"valid\":false") != std::string::npos);
    CHECK(badMagic.find("\"reason\":\"bad magic\"") != std::string::npos);
    CHECK(headField(badMagic, 0) == "3735928559"); // echoed 0xDEADBEEF
    CHECK(dataAllZero(badMagic));
    words[0] = 0x53484457u;

    // Version mismatch, frame mismatch, bad mode: same bounded shape.
    words[1] = 2u;
    std::string badVer =
        wrapperJson(words.data(), shadowtail::kTotalU32, 41u, 0u, 7u, 5u, 3u);
    CHECK(badVer.find("\"reason\":\"version mismatch\"") != std::string::npos);
    CHECK(dataAllZero(badVer));
    words[1] = 1u;
    std::string badFrame =
        wrapperJson(words.data(), shadowtail::kTotalU32, 99u, 0u, 7u, 5u, 3u);
    CHECK(badFrame.find("\"reason\":\"frame mismatch\"") != std::string::npos);
    CHECK(dataAllZero(badFrame));
    words[3] = 0u;
    std::string badMode =
        wrapperJson(words.data(), shadowtail::kTotalU32, 41u, 0u, 7u, 5u, 3u);
    CHECK(badMode.find("\"reason\":\"mode is not ON\"") != std::string::npos);
    CHECK(badMode.find("\"mode\":0") != std::string::npos);
    CHECK(dataAllZero(badMode));
    words[3] = 1u;

    // Short buffer and null buffer: bounded zeros, loud reason.
    std::string shortBuf =
        wrapperJson(words.data(), 100u, 41u, 0u, 7u, 5u, 3u);
    CHECK(shortBuf.find("\"reason\":\"short buffer\"") != std::string::npos);
    CHECK(dataAllZero(shortBuf));
    std::string nullBuf = wrapperJson(nullptr, 0u, 41u, 0u, 7u, 5u, 3u);
    CHECK(nullBuf.find("\"reason\":\"null buffer\"") != std::string::npos);
    CHECK(dataAllZero(nullBuf));

    // Pilot failure keeps REAL counters (data-bearing, never valid).
    words[4] = 0u; words[5] = 0u;
    std::string pilot =
        wrapperJson(words.data(), shadowtail::kTotalU32, 41u, 0u, 7u, 5u, 3u);
    CHECK(pilot.find("\"pilot\":true") != std::string::npos);
    CHECK(pilot.find("\"reason\":\"pilot-only: no manifest staged\"") !=
          std::string::npos);
    CHECK_FALSE(dataAllZero(pilot));
    CHECK(pilot.find(",100,100,0,") != std::string::npos);

    // Control failure keeps REAL counters with mode 2.
    words[4] = 5u; words[5] = 3u; words[3] = 2u;
    std::string ctl =
        wrapperJson(words.data(), shadowtail::kTotalU32, 41u, 0u, 7u, 5u, 3u);
    CHECK(ctl.find("\"mode\":2") != std::string::npos);
    CHECK(ctl.find("\"reason\":\"positive-control mode\"") != std::string::npos);
    CHECK_FALSE(dataAllZero(ctl));
}

TEST_CASE("ShadowTailBins SSBO layout constants match the frozen map")
{
    // Header + edges + slots; mirrors ShadowTailDiag + shadow_tail.glsl.
    CHECK(shadowtail::kFamStride == 298u);
    CHECK(shadowtail::kSlotStride == 598u);
    CHECK(shadowtail::kTotalU32 == 1656u);
    CHECK(shadowtail::kTotalU32 * 4u == 6624u);
    // Per-family offsets inside a family block starting at `base`.
    constexpr uint32_t kPilot = 0u, kHist = 34u;
    constexpr uint32_t kEligible = 260u, kRecorded = 261u;
    constexpr uint32_t kBlocked = 262u, kVisible = 263u;
    constexpr uint32_t kStrata = 264u, kOrd = 280u;
    CHECK(kHist == shadowtail::kPilotCounters);
    CHECK(kEligible == kHist + shadowtail::kAcceptanceEdgesMax);
    CHECK(kStrata == kEligible + 4u);
    CHECK(kOrd == kStrata + shadowtail::kStrataN);
    CHECK(shadowtail::kFamStride == kOrd + shadowtail::kOrdN + 2u);
    (void)kPilot;
    (void)kBlocked;
    (void)kVisible;
}

