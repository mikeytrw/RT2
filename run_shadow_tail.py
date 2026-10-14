#!/usr/bin/env python3
"""run_shadow_tail.py — bounded shadow-tail capture + persistence harness.

Owns durable per-run evidence for the RT4-T4 shadow-tail protocol
(shadow-tail-plan, READY 2026-10-13 + Sol implementation reviews).
Subcommands (run `... <cmd> --help` for exact flags):

  capture      ONE 60-frame headless capture into a UNIQUE owned rundir
               with provenance; validates + publishes completion manifest.
               --diag enables --shadow-tail; --edges stages a frozen
               manifest (absent = pilot mode: snapshots carry pilot counts
               with valid:false, never acceptance-valid).
  quartet      ONE block = R_ON/R_OFF/C_ON/C_OFF captures with a block
               manifest, frozen order and --deadline-min hard stop
               (remaining runs marked INCOMPLETE, never silently dropped).
  derive-freeze  Pilot snapshots dir -> frozen edge manifest (reference-only
               trial data; pilot Q per family via doubling brackets; edges
               by the frozen formula; precision/overflow rules enforced).
  check-freeze Confirmation dir + manifest -> gate verdict (p95 width,
               overflow fraction, minimums, coverage). No candidate
               adaptive calibration: fails closed on any violation.
  plan         Prints the exact 18-run calibration + 64-run acceptance
               tables (documentation + accounting reference; the driver
               executes quartet/capture commands, never implicit runs).

  python run_shadow_tail.py --self-test   # CPU-only gates (no GPU/exe).

--self-test exercises manifest validation, edge-derivation golden vectors,
snapshot schema, coverage matrix, and orchestrated fake-process runs
covering invalid/zero/missing/stale/OOB/missing-coverage/wrong-hash/
nonzero-exit/persistence failures plus derive/check-freeze paths. Exits
nonzero on any failure.

Persistence contract (§10): unique non-overwriting rundirs; checked
create/write/flush/close with reread validation (frame counts, JSON
schema, file hashes); completion manifest published only after outputs
validate; persistence failure = non-success exit — a pass is never
reported from unpersisted results. No GPU work happens in --self-test.
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import time

SCHEMA = "rt2.shadow-tail.v1"
FRAMES_DEFAULT = 60

# Frozen coverage matrix (plan §3b): config -> required strata.
# RT cells are (site, depth): site 0..4 env/tri/res-tri/res-env/punctual,
# depth 0 primary / 1 recursive. GI cells are (site, prov): site 0 tri /
# 1 env, prov 0 fresh / 1 replay. Stratum flat index = site*2+sub.
MATRIX = {
    "C1": {"rt": [(1, 0), (1, 1), (4, 0), (4, 1)],
           "gi": []},
    "C2": {"rt": [(0, 0), (0, 1), (1, 0), (1, 1), (2, 0), (2, 1),
                   (3, 0), (3, 1), (4, 0), (4, 1)],
           "gi": []},
    "C3": {"rt": [(1, 0), (1, 1), (4, 0), (4, 1)],
           "gi": [(0, 0), (0, 1)]},
    "C4": {"rt": [(0, 0), (0, 1), (1, 0), (1, 1), (4, 0), (4, 1)],
           "gi": [(0, 0), (0, 1), (1, 0), (1, 1)]},
}
STRATUM_MIN = 50
FAMILY_MIN = 10000

U32MAX = 0xFFFFFFFF
DATA_WORDS = 2 * (2 * 298 + 2)  # 1200-word counter region per snapshot
FAM_STRIDE = 298
STRATA_OFF = 264
HIST_OFF = 34
HIST_BINS = 226


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def write_text_checked(path, text):
    """Write + flush + fsync + reread. Returns error string or ''."""
    try:
        with open(path, "w") as f:
            f.write(text)
            f.flush()
            os.fsync(f.fileno())
        with open(path) as f:
            if f.read() != text:
                return "reread mismatch: %s" % path
    except OSError as e:
        return "write failure %s: %s" % (path, e)
    return ""


# ---------------------------------------------------------------------------
# Manifest + snapshot validation (mirrors ShadowTailBins.h rules).
# ---------------------------------------------------------------------------

def validate_manifest_edges(obj):
    if not isinstance(obj, dict):
        raise ValueError("manifest root must be an object")
    out = []
    for key in ("edgesRT", "edgesGI"):
        arr = obj.get(key)
        if not isinstance(arr, list) or len(arr) < 2 or len(arr) > 226:
            raise ValueError("%s must be a list of 2..226 uints" % key)
        prev = -1
        for v in arr:
            if not isinstance(v, int) or isinstance(v, bool):
                raise ValueError("%s has non-integer entry" % key)
            if v < 0 or v > U32MAX:
                raise ValueError("%s entry out of u32 range" % key)
            if v <= prev:
                raise ValueError("%s not strictly increasing" % key)
            prev = v
        if arr[0] != 0:
            raise ValueError("%s must start at 0" % key)
        out.append(arr)
    extra = set(obj.keys()) - {"edgesRT", "edgesGI", "meta"}
    if extra:
        raise ValueError("unexpected manifest keys: %s" % sorted(extra))
    return out[0], out[1]


def parse_snapshot_data(obj):
    """Structural parse shared by valid + pilot snapshots.

    Returns (frame, slot, head, data). Does NOT judge validity —
    callers decide (acceptance needs valid:true; pilot needs data)."""
    if not isinstance(obj, dict):
        raise ValueError("snapshot must be an object")
    if obj.get("schema") != SCHEMA:
        raise ValueError("bad schema")
    frame = obj.get("frame")
    if not isinstance(frame, int) or isinstance(frame, bool) or frame < 0:
        raise ValueError("bad frame id")
    slot = obj.get("slot")
    if slot not in (0, 1):
        raise ValueError("bad slot id")
    head = obj.get("head")
    if not isinstance(head, list) or len(head) != 8:
        raise ValueError("bad header")
    data = obj.get("data")
    if not isinstance(data, list) or len(data) != DATA_WORDS:
        raise ValueError("bad data length (want %d-word counter region)"
                         % DATA_WORDS)
    for v in data:
        if not isinstance(v, int) or isinstance(v, bool) or v < 0 \
                or v > U32MAX:
            raise ValueError("bad data word")
    return frame, slot, head, data


def validate_snapshot_line(obj):
    """Acceptance-grade parse: rejects invalid snapshots loudly."""
    frame, slot, head, data = parse_snapshot_data(obj)
    if obj.get("valid") is not True:
        raise ValueError("snapshot invalid: %s" % obj.get("reason", "?"))
    return frame, slot, head, data


def pilot_bracket(counts34):
    """Port of ShadowTailBins::PilotBracket. Returns (code, lo, hi)."""
    total = sum(counts34)
    if total == 0:
        return ("empty", None, None)
    rank = (total * 95 + 99) // 100
    acc = counts34[0]
    if rank <= acc:
        return ("underflow", 0, 0)
    for k in range(32):
        acc += counts34[1 + k]
        if rank <= acc:
            return (k, (1 if k == 0 else 1 << k), 1 << (k + 1))
    return ("overflow", 1 << 32, None)


def build_edges(qlo, qhi):
    """Port of BuildAcceptanceEdges incl. the limit invariant:
    limit=min(2*Qhi, U32MAX); stop at first next edge >= limit, append
    the limit ONCE, discard later edges."""
    limit = U32MAX if qhi > U32MAX // 2 else 2 * qhi
    w = max(1, (qhi + 64) // 128)
    e1 = qlo // 2
    edges = [0]
    e = 0
    if e1 > 0:
        edges.append(e1)
        e = e1
    while True:
        nxt = e + w
        if nxt >= limit:
            edges.append(limit)
            break
        edges.append(nxt)
        e = nxt
        if len(edges) >= 226:
            break
    return edges


def derive_freeze_edges(pilot):
    """Reference-only edge derivation from pooled pilot counts.

    pilot = [fam0counts34, fam1counts34]. Returns manifest dict or raises.
    Precision rule: w/Qlo <= 5% (else precision-inadequate)."""
    edges = {}
    for fam, key in ((0, "edgesRT"), (1, "edgesGI")):
        code, lo, hi = pilot_bracket(pilot[fam])
        if not isinstance(code, int):
            raise ValueError("family %d pilot %s: no finite bracket" % (fam, code))
        w = max(1, (hi + 64) // 128)
        if w / lo > 0.05:
            raise ValueError("family %d precision inadequate" % fam)
        edges[key] = build_edges(lo, hi)
    return {"edgesRT": edges["edgesRT"], "edgesGI": edges["edgesGI"]}


def check_coverage(config, snapshots):
    """Check §3b stratum minimums using each snapshot's OWN slot segment."""
    failures = []
    need = MATRIX.get(config)
    if need is None:
        return ["unknown config %s" % config]
    rt_cells = [0] * 16
    gi_cells = [0] * 16
    for _, slot, _, data in snapshots:
        base = slot * (2 * 298 + 2)
        for fam, cells in ((0, rt_cells), (1, gi_cells)):
            off = base + fam * 298 + STRATA_OFF
            for i in range(16):
                cells[i] += data[off + i]
    for fam, cells, req in ((0, rt_cells, need["rt"]), (1, gi_cells, need["gi"])):
        for (site, sub) in req:
            if cells[site * 2 + sub] < STRATUM_MIN:
                failures.append(
                    "fam %d site %d sub %d: %d < %d"
                    % (fam, site, sub, cells[site * 2 + sub], STRATUM_MIN)
                )
    return failures


# ---------------------------------------------------------------------------
# Capture orchestration.
# ---------------------------------------------------------------------------

def default_run_fn(exe, cmd, cwd, logpath):
    with open(logpath, "w") as log:
        return subprocess.run(cmd, cwd=cwd, stdout=log,
                              stderr=subprocess.STDOUT).returncode


def run_capture(args, run_fn=None):
    """One 60-frame capture. run_fn injectable for CPU-only tests."""
    if run_fn is None:
        run_fn = default_run_fn
    rundir = os.path.join(args.out_root, args.name)
    if os.path.exists(rundir):
        print("ERROR: rundir exists (non-overwriting rule): %s" % rundir)
        return 1
    try:
        os.makedirs(rundir)
    except OSError as e:
        print("ERROR: cannot create rundir: %s" % e)
        return 1
    cmd = [args.exe, "--headless", "--scene", args.scene,
           "--output-display", os.path.join(rundir, "frame.png"),
           "--frames", str(args.frames), "--width", str(args.width),
           "--height", str(args.height), "--spp", str(args.spp),
           "--bounces", "8", "--seed", "0", "--denoiser-mode", "off"]
    cmd += args.extra_args
    diag = bool(args.diag)
    if diag:
        cmd += ["--shadow-tail"]
        if args.edges:
            cmd += ["--shadow-tail-edges", args.edges]
        if args.control:
            cmd += ["--shadow-tail-control"]
    exe_dir = os.path.dirname(os.path.abspath(args.exe))
    err = write_text_checked(os.path.join(rundir, "command.txt"),
                             " ".join(cmd) + "\n")
    if err:
        print("ERROR: %s" % err)
        return 1
    prov = {"exe": args.exe, "scene": args.scene, "frames": args.frames,
            "width": args.width, "height": args.height, "spp": args.spp,
            "extra": args.extra_args, "diag": diag, "edges": args.edges,
            "control": bool(args.control), "config": args.config}
    try:
        prov["exe_sha256"] = sha256_file(args.exe)
        prov["scene_sha256"] = sha256_file(args.scene)
        if args.edges:
            prov["edges_sha256"] = sha256_file(args.edges)
    except OSError as e:
        print("ERROR: provenance hash failed: %s" % e)
        return 1
    prov["spvs"] = {}
    for name in ("closesthit.spv", "secondary_raygen.spv",
                 "restir_gi_temporal.spv", "closesthit_tail.spv",
                 "secondary_raygen_tail.spv", "restir_gi_temporal_tail.spv"):
        p = os.path.join(exe_dir, name)
        if os.path.exists(p):
            prov["spvs"][name] = sha256_file(p)
    err = write_text_checked(os.path.join(rundir, "provenance.json"),
                             json.dumps(prov, indent=1) + "\n")
    if err:
        print("ERROR: %s" % err)
        return 1
    rc = run_fn(args.exe, cmd, exe_dir, os.path.join(rundir, "run.log"))
    err = write_text_checked(os.path.join(rundir, "exitcode.txt"), "%d\n" % rc)
    if err:
        print("ERROR: %s" % err)
        return 1
    if rc != 0:
        print("FAIL capture exit=%d (preserved, no manifest)" % rc)
        return 1
    problems, n_valid, n_pilot = validate_rundir(rundir, args)
    manifest = {"name": args.name, "exit": rc, "problems": problems,
                "snapshots_valid": n_valid, "snapshots_pilot": n_pilot,
                "complete": not problems}
    frame_png = os.path.join(rundir, "frame.png")
    manifest["frame_sha256"] = (
        sha256_file(frame_png) if os.path.exists(frame_png) else None)
    err = write_text_checked(os.path.join(rundir, "completion.json"),
                             json.dumps(manifest, indent=1) + "\n")
    if err:
        print("ERROR: %s" % err)
        return 1
    try:
        with open(os.path.join(rundir, "completion.json")) as f:
            reread = json.load(f)
    except (OSError, ValueError) as e:
        print("FAIL completion reread: %s" % e)
        return 1
    if not reread.get("complete"):
        print("FAIL validation: %s" % reread.get("problems"))
        return 1
    if reread.get("frame_sha256") != sha256_file(frame_png):
        print("FAIL frame hash changed after manifest (tamper/race)")
        return 1
    print("DONE %s valid=%d pilot=%d"
          % (args.name, reread.get("snapshots_valid", 0),
             reread.get("snapshots_pilot", 0)))
    return 0


def validate_rundir(rundir, args):
    """All completion rules.

    Returns (problems, n_valid, n_pilot). problems empty = complete.
    Acceptance runs (edges staged) require valid snapshots; pilot runs
    (no edges) require data-bearing pilot snapshots instead — both are
    hard failures when absent, never silent."""
    problems = []
    frame_png = os.path.join(rundir, "frame.png")
    if not os.path.exists(frame_png):
        problems.append("missing frame.png")
    snaps_valid = []
    snaps_pilot = []
    logpath = os.path.join(rundir, "run.log")
    try:
        with open(logpath) as f:
            for line in f:
                if "[ShadowTail]" not in line:
                    continue
                try:
                    obj = json.loads(line.split("[ShadowTail]", 1)[1])
                except ValueError as e:
                    problems.append("unparseable snapshot line: %s" % e)
                    continue
                try:
                    snaps_valid.append(validate_snapshot_line(obj))
                    continue
                except ValueError:
                    pass
                # Pilot fallback: data-bearing but not acceptance-valid.
                try:
                    frame, slot, head, data = parse_snapshot_data(obj)
                    snaps_pilot.append((frame, slot, head, data))
                except ValueError as e:
                    problems.append("bad snapshot line: %s" % e)
    except OSError as e:
        problems.append("unreadable run.log: %s" % e)
    seen = set()
    for frame, slot, _, _ in snaps_valid + snaps_pilot:
        if (frame, slot) in seen:
            problems.append("duplicate frame/slot (%d,%d)" % (frame, slot))
        seen.add((frame, slot))
    if args.diag:
        if args.edges:
            if not snaps_valid:
                problems.append("no valid snapshots with edges staged")
        else:
            if not snaps_valid and not snaps_pilot:
                problems.append("no pilot snapshots produced")
    if args.config:
        cov = check_coverage(args.config, snaps_valid + snaps_pilot)
        problems.extend("coverage: " + c for c in cov)
    return problems, len(snaps_valid), len(snaps_pilot)


def run_quartet(args, run_fn=None, deadline_min=0):
    """One block = R_ON/R_OFF/C_ON/C_OFF with block manifest + deadline."""
    blockdir = os.path.join(args.out_root, args.name)
    if os.path.exists(blockdir):
        print("ERROR: blockdir exists: %s" % blockdir)
        return 1
    try:
        os.makedirs(blockdir)
    except OSError as e:
        print("ERROR: cannot create blockdir: %s" % e)
        return 1
    order = [("R_ON", args.ref_exe, True, args.edges, False),
             ("R_OFF", args.ref_exe, False, "", False),
             ("C_ON", args.cand_exe, True, args.edges, args.control),
             ("C_OFF", args.cand_exe, False, "", False)]
    deadline = time.monotonic() + deadline_min * 60 if deadline_min > 0 else None
    results = {}
    for sub, exe, diag, edges, control in order:
        if deadline is not None and time.monotonic() > deadline:
            results[sub] = "INCOMPLETE (deadline)"
            continue
        sub_args = argparse.Namespace(
            exe=exe, scene=args.scene, out_root=blockdir, name=sub,
            frames=args.frames, spp=args.spp, width=args.width,
            height=args.height, edges=edges, diag=diag, control=control,
            config=args.config, extra_args=args.extra_args)
        rc = run_capture(sub_args, run_fn=run_fn)
        results[sub] = "complete" if rc == 0 else "FAIL"
        if rc != 0:
            break
    manifest = {"block": args.name, "order": [o[0] for o in order],
                "results": results,
                "complete": all(v == "complete" for v in results.values())}
    err = write_text_checked(os.path.join(blockdir, "block.json"),
                             json.dumps(manifest, indent=1) + "\n")
    if err:
        print("ERROR: %s" % err)
        return 1
    if not manifest["complete"]:
        print("BLOCK %s: %s" % (args.name, results))
        return 2 if any("INCOMPLETE" in v for v in results.values()) else 1
    print("BLOCK %s complete" % args.name)
    return 0


PLAN_TEXT = """\
18-run calibration table (reference-only, HARD max 18 runs + 90 min wall):
  trial:        4 runs R_ON (T1 C1, T2 C2, T3 C3, T4 C4)
  confirm:      4 runs R_ON (repeat under derived edges)
  OFF:          4 runs R_OFF (one per config C1-C4)
  legacy:       4 fresh production runs (one per config, 60-frame protocol)
  positive:     2 runs R_ON+control (RT bracket, GI bracket)
  derive-freeze after trials; check-freeze after confirmation; freeze
  manifest before any candidate outcome. No qualifying config INCOMPLETE.
64-run acceptance table (HARD max 64 runs + 150 min wall):
  per fixture: 4 blocks x (R_ON, R_OFF, C_ON, C_OFF) = 16 runs (quartet).
  F=4 (C1, C2, C3, C4). Pair order frozen. First-reached cap stops;
  over-budget unfinished work INCOMPLETE, no replacements, no B runs.
The driver executes quartet/capture commands; nothing here runs implicitly.
"""


def cmd_derive_freeze(args):
    """Reference-only edge derivation from pilot trial snapshots."""
    pilot = [[0] * 34, [0] * 34]
    counts = [0, 0]
    nfiles = 0
    for root, _, files in os.walk(args.pilot_dir):
        for fn in sorted(files):
            if not fn.endswith(".jsonl") and fn != "run.log":
                continue
            with open(os.path.join(root, fn)) as f:
                for line in f:
                    if "[ShadowTail]" not in line:
                        continue
                    try:
                        obj = json.loads(line.split("[ShadowTail]", 1)[1])
                    except ValueError:
                        continue
                    try:
                        _, slot, _, data = parse_snapshot_data(obj)
                    except ValueError:
                        continue
                    base = slot * (2 * 298 + 2)
                    for fam in (0, 1):
                        for i in range(34):
                            pilot[fam][i] += data[base + fam * 298 + i]
                    counts[0] += 1
                    nfiles += 1
    if nfiles == 0:
        print("FAIL derive-freeze: no pilot snapshots found")
        return 1
    try:
        manifest = derive_freeze_edges(pilot)
    except ValueError as e:
        print("FAIL derive-freeze: %s" % e)
        return 1
    manifest["meta"] = {"derived": "reference-only pilot",
                        "pilot_snapshots": nfiles}
    err = write_text_checked(args.out, json.dumps(manifest, indent=1) + "\n")
    if err:
        print("ERROR: %s" % err)
        return 1
    print("FREEZE manifest=%s" % args.out)
    return 0


def derive_freeze_edges(pilot):
    edges = {}
    for fam, key in ((0, "edgesRT"), (1, "edgesGI")):
        code, lo, hi = pilot_bracket(pilot[fam])
        if not isinstance(code, int):
            raise ValueError("family %d pilot %s: no finite bracket" % (fam, code))
        w = max(1, (hi + 64) // 128)
        if w / lo > 0.05:
            raise ValueError("family %d precision inadequate" % fam)
        edges[key] = build_edges(lo, hi)
    return {"edgesRT": edges["edgesRT"], "edgesGI": edges["edgesGI"]}


def cmd_check_freeze(args):
    """Confirmation gate on a directory of snapshots + manifest."""
    with open(args.manifest) as f:
        manifest = json.load(f)
    try:
        edgesRT, edgesGI = validate_manifest_edges(manifest)
    except ValueError as e:
        print("FAIL check-freeze: bad manifest: %s" % e)
        return 1
    snaps = []
    for root, _, files in os.walk(args.dir):
        for fn in sorted(files):
            if not fn.endswith(".jsonl") and fn != "run.log":
                continue
            with open(os.path.join(root, fn)) as f:
                for line in f:
                    if "[ShadowTail]" not in line:
                        continue
                    try:
                        snaps.append(validate_snapshot_line(
                            json.loads(line.split("[ShadowTail]", 1)[1])))
                    except ValueError as e:
                        print("FAIL check-freeze: %s" % e)
                        return 1
    if not snaps:
        print("FAIL check-freeze: no valid snapshots")
        return 1
    failures = []
    pooled = [[0] * 226, [0] * 226]
    overflow = [0, 0]
    totals = [0, 0]
    for _, slot, head, data in snaps:
        if head[4] != len(edgesRT) or head[5] != len(edgesGI):
            failures.append("edge-count/header mismatch")
            continue
        base = slot * (2 * 298 + 2)
        for fam, edges in ((0, edgesRT), (1, edgesGI)):
            off = base + fam * 298
            for i in range(len(edges) - 1):
                pooled[fam][i] += data[off + 34 + i]
            overflow[fam] += data[off + 34 + len(edges) - 1]
            totals[fam] += sum(data[off + 34 + i] for i in range(len(edges)))
            # Bins beyond the frozen manifest must be empty: anything
            # there means the GPU used edges the manifest does not define.
            for i in range(len(edges), 226):
                if data[off + 34 + i] != 0:
                    failures.append(
                        "family %d counts beyond manifest edges" % fam)
    for fam, edges in ((0, edgesRT), (1, edgesGI)):
        n = totals[fam]
        if n < FAMILY_MIN:
            failures.append("family %d N=%d < %d" % (fam, n, FAMILY_MIN))
            continue
        rank = (n * 95 + 99) // 100
        acc, bink = 0, None
        for i in range(len(edges) - 1):
            acc += pooled[fam][i]
            if rank <= acc:
                bink = i
                break
        if bink is None:
            failures.append("family %d p95 unbounded/inconclusive" % fam)
            continue
        lo, hi = edges[bink], edges[bink + 1]
        if lo <= 0 or (hi - lo) / lo > 0.05:
            failures.append("family %d p95 width exceeds 5%%" % fam)
        if overflow[fam] / n >= 0.005:
            failures.append("family %d overflow %.4f >= 0.5%%"
                            % (fam, overflow[fam] / n))
    if args.config:
        failures.extend("coverage: " + c for c in check_coverage(
            args.config, snaps))
    if failures:
        print("CHECK-FREEZE FAILURES:")
        for f in failures:
            print("  - %s" % f)
        return 1
    print("CHECK-FREEZE PASS")
    return 0


def self_test():
    fails = []

    def check(name, cond):
        print(("PASS" if cond else "FAIL"), "-", name)
        if not cond:
            fails.append(name)

    # Manifest validation (mirrors C++ ParseEdgeManifest cases).
    rt, gi = validate_manifest_edges(
        {"edgesRT": [0, 4096, 8192, 16384, 32768], "edgesGI": [0, 100, 200]})
    check("manifest valid", rt[-1] == 32768 and gi[-1] == 200)
    for bad in ({"edgesRT": [0, 100, 100], "edgesGI": [0, 1]},
                {"edgesRT": [4, 100], "edgesGI": [0, 1]},
                {"edgesRT": [0], "edgesGI": [0, 1]},
                {"edgesRT": [0, -4], "edgesGI": [0, 1]},
                {"edgesRT": [0, 4294967296], "edgesGI": [0, 1]},
                {"edgesGI": [0, 1]}, "garbage"):
        try:
            validate_manifest_edges(bad)
            check("manifest rejects %r" % (bad,), False)
        except (ValueError, AttributeError, TypeError):
            check("manifest rejects %r" % (bad,), True)

    # Edge-derivation golden vectors (verified against the C++ contract).
    counts = [0] * 34
    check("pilot empty", pilot_bracket(counts)[0] == "empty")
    counts[0] = 100
    check("pilot underflow", pilot_bracket(counts)[0] == "underflow")
    counts = [0] * 34
    counts[14] = 1000  # doubling bin k=13 -> [8192, 16384)
    code, lo, hi = pilot_bracket(counts)
    check("pilot bracket k=13", code == 13 and lo == 8192 and hi == 16384)
    e = build_edges(8192, 16384)
    check("edges Q=10000 golden",
          len(e) == 226 and e[0] == 0 and e[1] == 4096 and e[2] == 4224
          and e[-1] == 32768 and all(b > a for a, b in zip(e, e[1:])))
    e8 = build_edges(8, 16)
    check("edges Q=8 counter-example", len(e8) == 30 and e8[-1] == 32)
    e30 = build_edges(1 << 30, 1 << 31)
    check("edges Q=2^30 clamp", e30[-1] == 0xFFFFFFFF)
    e = build_edges(1 << 20, 1 << 21)
    check("edges Q=2^20 shape", len(e) == 226 and e[-1] == 1 << 22)

    # Snapshot schema validation (slot-aware).
    good = {"schema": SCHEMA, "frame": 41, "slot": 0, "serial": 7,
            "valid": True, "head": [0x53484457, 1, 41, 1, 5, 3, 1, 0],
            "data": [0] * 1196}
    base = 0 + 0 * 298
    good["data"][base + 260] = 100
    good["data"][base + 261] = 100
    good["data"][base + 34 + 1] = 100
    good["data"][base + 264 + 1 * 2 + 0] = 60
    good["data"][base + 264 + 1 * 2 + 1] = 55
    good["data"][base + 264 + 4 * 2 + 0] = 70
    good["data"][base + 264 + 4 * 2 + 1] = 65
    try:
        frame, slot, _, _ = validate_snapshot_line(good)
        check("snapshot valid slot-aware", frame == 41 and slot == 0)
    except ValueError:
        check("snapshot valid slot-aware", False)
    bad_snap = dict(good, valid=False, reason="mode is not ON")
    try:
        validate_snapshot_line(bad_snap)
        check("snapshot invalid rejected", False)
    except ValueError:
        check("snapshot invalid rejected", True)

    # Coverage matrix on synthetic pooled data.
    fails_cov = check_coverage("C1", [(41, 0, good["head"], good["data"])])
    check("C1 coverage passes", fails_cov == [])
    fails_cov2 = check_coverage("C4", [(41, 0, good["head"], good["data"])])
    check("C4 coverage fails honestly", len(fails_cov2) > 0)
    check("unknown config fails", check_coverage("CX", []) != [])

    # Fake-process orchestration: end-to-end run_capture without GPU.
    def fake_run(exe_, cmd, cwd, logpath, snaps_fn, png=True, rc=0):
        idx = cmd.index("--output-display") + 1
        if png:
            with open(cmd[idx], "wb") as pf:
                pf.write(b"fakepng")
        with open(logpath, "w") as lf:
            for line in snaps_fn():
                lf.write(line)
        return rc

    def snap_lines(n=20, start=41, valid=True, oob=False, empty=False):
        lines = []
        for f in range(start, start + n):
            s = dict(good, frame=f)
            if not valid:
                s = dict(s, valid=False, reason="pilot-only")
            if oob:
                s = dict(s, data=[0] * 10)
            if empty:
                s = dict(s, data=[0] * 1200)
            lines.append("[ShadowTail] %s\n" % json.dumps(s))
        return lines

    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "RT2App.exe")
        with open(exe, "wb") as f:
            f.write(b"fake-exe")
        scene = os.path.join(tmp, "s.rt2scene")
        with open(scene, "w") as f:
            f.write("{}")
        mf = os.path.join(tmp, "edges.json")
        with open(mf, "w") as f:
            json.dump({"edgesRT": [0, 1, 2, 3, 4, 5],
                       "edgesGI": [0, 1, 2]}, f)

        def mkargs(name, **kw):
            d = dict(exe=exe, scene=scene, out_root=tmp, name=name,
                     frames=60, spp=1, width=64, height=64, edges=mf,
                     diag=True, control=False, config="", extra_args=[])
            d.update(kw)
            return argparse.Namespace(**d)

        check("fake happy capture",
              run_capture(mkargs("happy"),
                          run_fn=lambda e, c, w, l: fake_run(
                              e, c, w, l, lambda: snap_lines())) == 0)
        check("fake nonzero exit fails",
              run_capture(mkargs("rc1"),
                          run_fn=lambda e, c, w, l: fake_run(
                              e, c, w, l, lambda: snap_lines(), rc=1)) == 1)
        check("fake missing frame fails",
              run_capture(mkargs("noframe"),
                          run_fn=lambda e, c, w, l: fake_run(
                              e, c, w, l, lambda: snap_lines(),
                              png=False)) == 1)
        dup_lines = snap_lines(n=1) + snap_lines(n=1)
        check("fake duplicate frames fail",
              run_capture(mkargs("dup"),
                          run_fn=lambda e, c, w, l: fake_run(
                              e, c, w, l, lambda: dup_lines)) == 1)
        check("fake OOB data fails",
              run_capture(mkargs("oob"),
                          run_fn=lambda e, c, w, l: fake_run(
                              e, c, w, l,
                              lambda: snap_lines(oob=True))) == 1)
        check("fake missing coverage fails",
              run_capture(mkargs("nocov", config="C4"),
                          run_fn=lambda e, c, w, l: fake_run(
                              e, c, w, l,
                              lambda: snap_lines(empty=True))) == 1)
        check("fake tamper setup",
              run_capture(mkargs("tamper"),
                          run_fn=lambda e, c, w, l: fake_run(
                              e, c, w, l, lambda: snap_lines())) == 0)
        with open(os.path.join(tmp, "tamper", "frame.png"), "wb") as pf:
            pf.write(b"tampered!")
        with open(os.path.join(tmp, "tamper", "completion.json")) as f:
            man = json.load(f)
        check("fake tamper detected",
              man.get("frame_sha256") != sha256_file(
                  os.path.join(tmp, "tamper", "frame.png")))
        a8 = mkargs("y")
        blocker = os.path.join(tmp, "blocker-file")
        with open(blocker, "w") as f:
            f.write("x")
        a8.out_root = blocker  # makedirs under a file -> NotADirectoryError
        check("fake persistence failure fails",
              run_capture(a8, run_fn=lambda e, c, w, l: fake_run(
                  e, c, w, l, lambda: snap_lines())) != 0)

        # derive-freeze on synthetic pilot data (golden edges).
        pdir = os.path.join(tmp, "pilot")
        os.makedirs(pdir)
        pilot_counts = [0] * 34
        pilot_counts[14] = 100000
        pline = {"schema": SCHEMA, "frame": 41, "slot": 0, "serial": 1,
                 "valid": False, "reason": "pilot-only",
                 "head": [0x53484457, 1, 41, 1, 0, 0, 1, 0],
                 "data": [0] * 1196}
        pline["data"][0 + 0 * 298 + 14] = 100000
        pline["data"][0 + 1 * 298 + 14] = 100000
        with open(os.path.join(pdir, "pilot.jsonl"), "w") as f:
            f.write("[ShadowTail] %s\n" % json.dumps(pline))
        mout = os.path.join(tmp, "frozen.json")
        da = argparse.Namespace(pilot_dir=pdir, out=mout)
        check("fake derive-freeze", cmd_derive_freeze(da) == 0)
        with open(mout) as f:
            frozen = json.load(f)
        check("frozen edges golden",
              len(frozen["edgesRT"]) == 226 and frozen["edgesRT"][1] == 4096
              and frozen["edgesRT"][-1] == 32768)

        # check-freeze pass + fail paths.
        cdir = os.path.join(tmp, "confirm")
        os.makedirs(cdir)
        cline = {"schema": SCHEMA, "frame": 45, "slot": 0, "serial": 9,
                 "valid": True,
                 "head": [0x53484457, 1, 45, 1, 226, 226, 1, 0],
                 "data": [0] * 1196}
        # Mass inside narrow bins of the frozen edges (bin of [9984,10112)).
        import bisect
        er, eg = frozen["edgesRT"], frozen["edgesGI"]
        birt = bisect.bisect_right(er, 10000) - 1
        big = bisect.bisect_right(eg, 10000) - 1
        cline["data"][0 + 0 * 298 + 34 + birt] = 20000
        cline["data"][0 + 0 * 298 + 260] = 20000
        cline["data"][0 + 0 * 298 + 261] = 20000
        cline["data"][0 + 1 * 298 + 34 + big] = 20000
        cline["data"][0 + 1 * 298 + 260] = 20000
        cline["data"][0 + 1 * 298 + 261] = 20000
        with open(os.path.join(cdir, "run.log"), "w") as f:
            f.write("[ShadowTail] %s\n" % json.dumps(cline))
        ca = argparse.Namespace(dir=cdir, manifest=mout, config="")
        check("fake check-freeze passes", cmd_check_freeze(ca) == 0)
        ovf = dict(cline)
        ovf["data"] = list(cline["data"])
        ovf["data"][0 + 0 * 298 + 34 + 225] = 20000  # all overflow
        ovf["data"][0 + 0 * 298 + 34 + birt] = 0
        with open(os.path.join(cdir, "run.log"), "w") as f:
            f.write("[ShadowTail] %s\n" % json.dumps(ovf))
        check("fake check-freeze overflow fails",
              cmd_check_freeze(ca) == 1)

        # Quartet happy path (fake, instant) + deadline INCOMPLETE.
        qa = argparse.Namespace(
            ref_exe=exe, cand_exe=exe, scene=scene, out_root=tmp,
            name="b1", frames=60, spp=1, width=64, height=64,
            edges=mf, control=False, config="", extra_args=[])
        check("fake quartet complete",
              run_quartet(qa, run_fn=lambda e, c, w, l: fake_run(
                  e, c, w, l, lambda: snap_lines())) == 0)
        # Fake clock: each monotonic() call advances 100s, so a 1-minute
        # deadline expires deterministically after the first sub-run.
        real_mono = time.monotonic
        clock = [0.0]

        def fake_mono():
            clock[0] += 100.0
            return clock[0]

        try:
            time.monotonic = fake_mono
            qa2 = argparse.Namespace(**{**vars(qa), "name": "b2"})
            check("fake quartet deadline incomplete",
                  run_quartet(qa2, run_fn=lambda e, c, w, l: fake_run(
                      e, c, w, l, lambda: snap_lines()),
                      deadline_min=1) == 2)
        finally:
            time.monotonic = real_mono

    print("SELF-TEST", "ALL PASS" if not fails else "FAILURES: %s" % fails)
    return 0 if not fails else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--self-test", action="store_true")
    sub = ap.add_subparsers(dest="cmd")
    cap = sub.add_parser("capture")
    cap.add_argument("--exe", required=True)
    cap.add_argument("--scene", required=True)
    cap.add_argument("--out-root", required=True)
    cap.add_argument("--name", required=True)
    cap.add_argument("--frames", type=int, default=FRAMES_DEFAULT)
    cap.add_argument("--spp", type=int, default=1)
    cap.add_argument("--width", type=int, default=960)
    cap.add_argument("--height", type=int, default=540)
    cap.add_argument("--edges", default="")
    cap.add_argument("--diag", action="store_true")
    cap.add_argument("--control", action="store_true")
    cap.add_argument("--config", default="")
    cap.add_argument("extra_args", nargs=argparse.REMAINDER, default=[])
    q = sub.add_parser("quartet")
    q.add_argument("--ref-exe", required=True)
    q.add_argument("--cand-exe", required=True)
    q.add_argument("--scene", required=True)
    q.add_argument("--out-root", required=True)
    q.add_argument("--name", required=True)
    q.add_argument("--frames", type=int, default=FRAMES_DEFAULT)
    q.add_argument("--spp", type=int, default=1)
    q.add_argument("--width", type=int, default=960)
    q.add_argument("--height", type=int, default=540)
    q.add_argument("--edges", default="")
    q.add_argument("--control", action="store_true")
    q.add_argument("--config", default="")
    q.add_argument("--deadline-min", type=float, default=0)
    q.add_argument("extra_args", nargs=argparse.REMAINDER, default=[])
    df = sub.add_parser("derive-freeze")
    df.add_argument("--pilot-dir", required=True)
    df.add_argument("--out", required=True)
    cf = sub.add_parser("check-freeze")
    cf.add_argument("--dir", required=True)
    cf.add_argument("--manifest", required=True)
    cf.add_argument("--config", default="")
    sub.add_parser("plan")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if args.cmd == "plan":
        print(PLAN_TEXT)
        return 0
    if args.cmd == "derive-freeze":
        return cmd_derive_freeze(args)
    if args.cmd == "check-freeze":
        return cmd_check_freeze(args)
    if args.cmd == "quartet":
        qa = argparse.Namespace(
            ref_exe=args.ref_exe, cand_exe=args.cand_exe, scene=args.scene,
            out_root=args.out_root, name=args.name, frames=args.frames,
            spp=args.spp, width=args.width, height=args.height,
            edges=args.edges, control=args.control, config=args.config,
            extra_args=args.extra_args)
        return run_quartet(qa, deadline_min=args.deadline_min)
    if args.cmd == "capture":
        ca = argparse.Namespace(
            exe=args.exe, scene=args.scene, out_root=args.out_root,
            name=args.name, frames=args.frames, spp=args.spp,
            width=args.width, height=args.height, edges=args.edges,
            diag=True, control=args.control, config=args.config,
            extra_args=args.extra_args)
        for req in ("exe", "scene", "out_root", "name"):
            if not getattr(ca, req):
                print("ERROR: --%s required" % req.replace("_", "-"))
                return 1
        return run_capture(ca)
    ap.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())
