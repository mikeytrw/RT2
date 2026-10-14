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
# Families with required minimums per config (plan §3b): C1/C2 run with
# GI disabled, so GI minimums never apply there — enforcing them would
# fail every C1/C2 confirmation on empty-but-correct data.
FAMILY_REQUIRED = {"C1": [0], "C2": [0], "C3": [0, 1], "C4": [0, 1]}

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

    Returns (frame, slot, serial, head, data). Does NOT judge validity —
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
    serial = obj.get("serial")
    if not isinstance(serial, int) or isinstance(serial, bool) or serial < 0:
        raise ValueError("bad submit serial")
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
    return frame, slot, serial, head, data


def validate_snapshot_line(obj):
    """Acceptance-grade parse: rejects invalid snapshots loudly.

    Returns (frame, slot, serial, head, data). Serial order is the
    submission order used by window/sequence checks."""
    frame, slot, serial, head, data = parse_snapshot_data(obj)
    if obj.get("valid") is not True:
        raise ValueError("snapshot invalid: %s" % obj.get("reason", "?"))
    return frame, slot, serial, head, data


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
    for _, slot, _, _, data in snapshots:
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
    prod_spvs = ("closesthit.spv", "secondary_raygen.spv",
                 "restir_gi_temporal.spv")
    tail_spvs = ("closesthit_tail.spv", "secondary_raygen_tail.spv",
                 "restir_gi_temporal_tail.spv")
    for name in prod_spvs + tail_spvs:
        p = os.path.join(exe_dir, name)
        if os.path.exists(p):
            prov["spvs"][name] = sha256_file(p)
    # SPV requirements: diag runs need all six (production + tail ON
    # variants — an ON pipeline cannot exist without them); plain runs
    # need the three production modules. Missing files fail loudly here,
    # never as a mysterious pipeline error later.
    need = prod_spvs + tail_spvs if diag else prod_spvs
    missing = [n for n in need if n not in prov["spvs"]]
    if missing:
        print("FAIL missing required SPVs: %s" % missing)
        return 1
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
    # Hash reread: inputs hashed before execution must match after it
    # (replacement mid-run fails here, never silently).
    try:
        if sha256_file(args.exe) != prov["exe_sha256"]:
            print("FAIL exe hash changed during run (tamper/race)")
            return 1
        if sha256_file(args.scene) != prov["scene_sha256"]:
            print("FAIL scene hash changed during run (tamper/race)")
            return 1
        if args.edges and sha256_file(args.edges) != prov["edges_sha256"]:
            print("FAIL edges hash changed during run (tamper/race)")
            return 1
    except OSError as e:
        print("FAIL reread hash failed: %s" % e)
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
                    frame, slot, serial, head, data = parse_snapshot_data(obj)
                    snaps_pilot.append((frame, slot, serial, head, data))
                except ValueError as e:
                    problems.append("bad snapshot line: %s" % e)
    except OSError as e:
        problems.append("unreadable run.log: %s" % e)
    seen = set()
    for frame, slot, _, _, _ in snaps_valid + snaps_pilot:
        if (frame, slot) in seen:
            problems.append("duplicate frame/slot (%d,%d)" % (frame, slot))
        seen.add((frame, slot))
    if args.diag and args.edges and snaps_valid:
        # Measured-window authority (contract: frames 41..60 of a
        # 60-frame run): every measured frame must be present. Warmup
        # frames may be absent (drain timing), measured frames may not.
        # Serial order must follow frame order (submission order).
        have_frames = set(f for f, _, _, _, _ in snaps_valid)
        if args.frames == 60:
            missing = [f for f in range(41, 61) if f not in have_frames]
            if missing:
                problems.append("measured window incomplete, missing %s"
                                % missing[:8])
        by_frame = {}
        for frame, _, serial, _, _ in snaps_valid:
            by_frame.setdefault(frame, []).append(serial)
        prev = -1
        ordered_ok = True
        for frame in sorted(by_frame):
            for serial in sorted(by_frame[frame]):
                if serial <= prev:
                    ordered_ok = False
                    break
                prev = serial
            if not ordered_ok:
                break
        if not ordered_ok:
            problems.append("serial order violation (not submission order)")
    if args.diag:
        if args.edges:
            if not snaps_valid:
                problems.append("no valid snapshots with edges staged")
        else:
            if not snaps_valid and not snaps_pilot:
                problems.append("no pilot snapshots produced")
    if args.config and args.diag:
        # Coverage gates diagnostic ON runs ONLY: OFF legs (diag=False)
        # produce no shadow snapshots by design, so requiring coverage
        # there would fail every quartet at R_OFF. OFF legs validate
        # exit/frame/image/timing/parity separately (see run_quartet).
        cov = check_coverage(args.config, snaps_valid + snaps_pilot)
        problems.extend("coverage: " + c for c in cov)
    return problems, len(snaps_valid), len(snaps_pilot)


def check_off_parity(blockdir, on_name, off_name):
    """OFF-leg timing presence + ON/OFF image parity for one build.

    Returns (timing_result, image_result), each "complete" or a FAIL
    string. Both are hard gates: missing timing records or any pixel
    difference fails the block."""
    timing, image = "complete", "complete"
    logpath = os.path.join(blockdir, off_name, "run.log")
    try:
        with open(logpath) as f:
            timed = sum(1 for line in f if "[HeadlessTiming]" in line)
    except OSError:
        timed = 0
    if timed == 0:
        timing = "FAIL (no timing records in %s)" % off_name
    try:
        from PIL import Image
        a = Image.open(os.path.join(blockdir, on_name, "frame.png")).convert("RGB")
        b = Image.open(os.path.join(blockdir, off_name, "frame.png")).convert("RGB")
        if a.size != b.size or list(a.getdata()) != list(b.getdata()):
            image = "FAIL (ON/OFF image mismatch %s vs %s)" % (on_name, off_name)
    except Exception as e:
        image = "FAIL (image compare error: %s)" % e
    return timing, image


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
        extra = list(args.extra_args)
        if not diag:
            # OFF legs carry benchmark timings so their dispatch/frame
            # timing parity is checkable (validated below); they carry
            # no shadow snapshots and no coverage requirement by design.
            extra = extra + ["--benchmark-timings"]
        sub_args = argparse.Namespace(
            exe=exe, scene=args.scene, out_root=blockdir, name=sub,
            frames=args.frames, spp=args.spp, width=args.width,
            height=args.height, edges=edges, diag=diag, control=control,
            config=args.config if diag else "", extra_args=extra)
        rc = run_capture(sub_args, run_fn=run_fn)
        results[sub] = "complete" if rc == 0 else "FAIL"
        if rc != 0:
            break
    # OFF-leg timing parity + ON/OFF image parity (per build). OFF legs
    # must each contain timing records; ON/OFF frame images must be
    # pixel-identical (instrumentation must not change the image).
    if all(results.get(k) == "complete" for k in ("R_ON", "R_OFF")):
        results["R_timing_parity"], results["R_image_parity"] = \
            check_off_parity(blockdir, "R_ON", "R_OFF")
    if all(results.get(k) == "complete" for k in ("C_ON", "C_OFF")):
        results["C_timing_parity"], results["C_image_parity"] = \
            check_off_parity(blockdir, "C_ON", "C_OFF")
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


# PAIRED GOLDENS (generated into goldens.py, inlined below verbatim).
# Paired with RT2Tests ShadowTailBinsTests snapshot-JSON golden
# outputs (same strings the C++ test pins by prefix/markers). If
# the C++ goldens change, update these in the same commit.
TEST_GOLDEN_PILOT_JSON = (
    '{"schema":"rt2.shadow-tail.v1","frame":41,"slot":0,"serial":7,"valid":false,"pilot":true,"mode":1,"reason":"pilot-only: no manifest staged","head":[1397245015,1,41,1,0,0,1,0],"data":[0,100,0,0,0,0,0,0'
    ',0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0'
    ',0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0'
    ',0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,100,100,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0'
    ',0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0'
    ',0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0'
    ',0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0'
    ',0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0'
    ',0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0'
    ',0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0'
    ',0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0'
    ',0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0'
    ',0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]}'
)
TEST_GOLDEN_CONTROL_JSON = (
    '{"schema":"rt2.shadow-tail.v1","frame":42,"slot":1,"serial":9,"valid":false,"pilot":false,"mode":2,"reason":"positive-control mode","head":[1397245015,1,42,2,5,3,1,1],"data":[0,0,0,0,0,0,0,0,0,0,0,0,0'
    ',0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0'
    ',0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0'
    ',0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0'
    ',0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0'
    ',0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0'
    ',0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0'
    ',0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0'
    ',0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0'
    ',0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0'
    ',0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0'
    ',0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0'
    ',0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,305]}'
)


T95_B4 = 2.35336343  # one-sided 95% t, df=3 (B=4 fixed by contract)


def paired_t_bound(xs):
    """One-sided 95% paired-t upper bound on exp(mean) of log-ratios.

    B must be exactly 4 (contract); anything else is INCONCLUSIVE, never
    silently generalized. Returns (bound, None) or (None, reason)."""
    import math
    if len(xs) != 4:
        return None, "need exactly 4 blocks, got %d" % len(xs)
    for x in xs:
        if not math.isfinite(x):
            return None, "non-finite log-ratio"
    mean = sum(xs) / 4.0
    var = sum((x - mean) ** 2 for x in xs) / 3.0
    return math.exp(mean + T95_B4 * math.sqrt(var / 4.0)), None


def cmd_analyze(args):
    """Paired 4-block t-bound + observer analysis over block dirs.

    Each --block-dirs entry must hold a complete quartet block.json for
    the SAME fixture/config. Fails closed (INCONCLUSIVE, exit 2) on any
    structural deviation; FAIL (exit 1) on a bound violation."""
    import math
    with open(args.manifest) as f:
        manifest = json.load(f)
    try:
        edgesRT, edgesGI = validate_manifest_edges(manifest)
    except ValueError as e:
        print("FAIL analyze: bad manifest: %s" % e)
        return 1
    if args.config not in FAMILY_REQUIRED:
        print("FAIL analyze: unknown config %s" % args.config)
        return 1
    req_fams = FAMILY_REQUIRED[args.config]
    blocks = []
    for bdir in args.block_dirs:
        try:
            with open(os.path.join(bdir, "block.json")) as f:
                block = json.load(f)
        except (OSError, ValueError) as e:
            print("FAIL analyze: bad block %s: %s" % (bdir, e))
            return 1
        if not block.get("complete"):
            print("FAIL analyze: block incomplete: %s" % bdir)
            return 1
        for k in ("R_image_parity", "C_image_parity",
                  "R_timing_parity", "C_timing_parity"):
            if block.get("results", {}).get(k) != "complete":
                print("FAIL analyze: parity gate %s not complete in %s"
                      % (k, bdir))
                return 1
        subs = {}
        for sub in ("R_ON", "R_OFF", "C_ON", "C_OFF"):
            sdir = os.path.join(bdir, sub)
            try:
                with open(os.path.join(sdir, "run.log")) as f:
                    lines = f.readlines()
            except OSError as e:
                print("FAIL analyze: unreadable run %s: %s" % (sdir, e))
                return 1
            snaps, timings = [], {}
            for line in lines:
                if "[ShadowTail]" in line:
                    try:
                        snaps.append(validate_snapshot_line(
                            json.loads(line.split("[ShadowTail]", 1)[1])))
                    except ValueError as e:
                        print("FAIL analyze: %s" % e)
                        return 1
                if "[HeadlessTiming]" in line:
                    try:
                        timings.setdefault("frames", []).append(json.loads(
                            line.split("[HeadlessTiming]", 1)[1]))
                    except ValueError:
                        pass
            subs[sub] = (snaps, timings)
        blocks.append(subs)
    if len(blocks) != 4:
        print("INCONCLUSIVE analyze: need exactly 4 blocks, got %d"
              % len(blocks))
        return 2
    failures, inconclusive = [], []

    def p95bracket(snaps, fam, edges):
        pooled = [0] * 226
        for _, slot, _, _, data in snaps:
            base = slot * (2 * 298 + 2) + fam * 298
            for i in range(len(edges) - 1):
                pooled[i] += data[base + 34 + i]
        n = sum(pooled)
        if n == 0:
            return None, "empty"
        rank = (n * 95 + 99) // 100
        acc = 0
        for i in range(len(edges) - 1):
            acc += pooled[i]
            if rank <= acc:
                return (edges[i], edges[i + 1]), None
        return None, "unbounded"

    for fam, edges in ((0, edgesRT), (1, edgesGI)):
        if fam not in req_fams:
            continue
        # Endpoint-conservative block log-ratios: candidate UPPER over
        # reference LOWER, per block; paired-t bound over the 4 blocks.
        xs = []
        bad = False
        for b in blocks:
            (rslow, rsup), _ = p95bracket(b["R_ON"][0], fam, edges)
            (cslow, csup), _ = p95bracket(b["C_ON"][0], fam, edges)
            if rslow is None or cslow is None or rslow <= 0 or cslow <= 0:
                inconclusive.append("family %d block bracket invalid" % fam)
                bad = True
                break
            xs.append(math.log(csup / rslow))
        if bad:
            continue
        bound, err = paired_t_bound(xs)
        if err is not None:
            inconclusive.append("family %d: %s" % (fam, err))
        elif bound > 1.05:
            failures.append("family %d C/R p95 bound %.4f > 1.05"
                            % (fam, bound))
        else:
            print("family %d C/R p95 bound %.4f PASS" % (fam, bound))

    # Observer bounds per build per dispatch (RT shading + GI temporal).
    for build in ("R", "C"):
        for region in ("RT Shading", "ReSTIR GI Temporal"):
            ys = []
            bad = False
            for b in blocks:
                on = [f["regions_ms"].get(region) for f in b[build + "_ON"][1].get("frames", [])]
                off = [f["regions_ms"].get(region) for f in b[build + "_OFF"][1].get("frames", [])]
                on = sorted(v for v in on if isinstance(v, (int, float)) and v > 0)
                off = sorted(v for v in off if isinstance(v, (int, float)) and v > 0)
                if not on or not off:
                    # GI temporal absent on non-GI configs: not applicable,
                    # never a pass; RT shading must always be present.
                    if region == "RT Shading":
                        inconclusive.append(
                            "%s %s: missing dispatch timings" % (build, region))
                        bad = True
                    break
                import statistics
                ys.append(math.log(statistics.median(on) / statistics.median(off)))
            if bad:
                continue
            if not ys:
                continue
            bound, err = paired_t_bound(ys)
            if err is not None:
                inconclusive.append("%s %s: %s" % (build, region, err))
            elif bound > 1.05:
                failures.append("%s %s observer bound %.4f > 1.05"
                                % (build, region, bound))
            else:
                print("%s %s observer bound %.4f PASS" % (build, region, bound))
    if inconclusive:
        print("ANALYZE INCONCLUSIVE:")
        for f in inconclusive:
            print("  - %s" % f)
        return 2
    if failures:
        print("ANALYZE FAILURES:")
        for f in failures:
            print("  - %s" % f)
        return 1
    print("ANALYZE PASS")
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
    """Reference-only edge derivation from pilot trial snapshots.

    Each --trial-dir must be a completed reference trial rundir
    (completion.json complete:true, provenance exe hash == --ref-exe).
    Anything else (hand-assembled dirs, candidate builds, incomplete
    runs) fails closed — a hand-authored edge file is not evidence."""
    if not args.ref_exe:
        print("FAIL derive-freeze: --ref-exe required (trial identity)")
        return 1
    try:
        ref_hash = sha256_file(args.ref_exe)
    except OSError as e:
        print("FAIL derive-freeze: cannot hash ref exe: %s" % e)
        return 1
    pilot = [[0] * 34, [0] * 34]
    nfiles = 0
    for trial in args.trial_dirs:
        try:
            with open(os.path.join(trial, "completion.json")) as f:
                completion = json.load(f)
            with open(os.path.join(trial, "provenance.json")) as f:
                prov = json.load(f)
        except (OSError, ValueError) as e:
            print("FAIL derive-freeze: bad trial dir %s: %s" % (trial, e))
            return 1
        if not completion.get("complete"):
            print("FAIL derive-freeze: trial incomplete: %s" % trial)
            return 1
        if prov.get("exe_sha256") != ref_hash:
            print("FAIL derive-freeze: trial exe mismatch: %s" % trial)
            return 1
        logpath = os.path.join(trial, "run.log")
        try:
            with open(logpath) as f:
                lines = [l for l in f if "[ShadowTail]" in l]
        except OSError as e:
            print("FAIL derive-freeze: unreadable run.log %s: %s" % (trial, e))
            return 1
        if not lines:
            print("FAIL derive-freeze: no pilot snapshots: %s" % trial)
            return 1
        for line in lines:
            try:
                obj = json.loads(line.split("[ShadowTail]", 1)[1])
            except ValueError:
                continue
            try:
                _, slot, _, _, data = parse_snapshot_data(obj)
            except ValueError:
                continue
            base = slot * (2 * 298 + 2)
            for fam in (0, 1):
                for i in range(34):
                    pilot[fam][i] += data[base + fam * 298 + i]
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
                        "pilot_snapshots": nfiles,
                        "trials": args.trial_dirs,
                        "ref_exe_sha256": ref_hash}
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
    """Confirmation gate on explicit confirmation rundirs + manifest."""
    with open(args.manifest) as f:
        manifest = json.load(f)
    try:
        edgesRT, edgesGI = validate_manifest_edges(manifest)
    except ValueError as e:
        print("FAIL check-freeze: bad manifest: %s" % e)
        return 1
    try:
        ref_hash = sha256_file(args.ref_exe)
    except OSError as e:
        print("FAIL check-freeze: cannot hash ref exe: %s" % e)
        return 1
    snaps = []
    for rundir in args.run_dirs:
        try:
            with open(os.path.join(rundir, "completion.json")) as f:
                completion = json.load(f)
            with open(os.path.join(rundir, "provenance.json")) as f:
                prov = json.load(f)
        except (OSError, ValueError) as e:
            print("FAIL check-freeze: bad run dir %s: %s" % (rundir, e))
            return 1
        if not completion.get("complete"):
            print("FAIL check-freeze: run incomplete: %s" % rundir)
            return 1
        if prov.get("exe_sha256") != ref_hash:
            print("FAIL check-freeze: exe mismatch: %s" % rundir)
            return 1
        try:
            with open(os.path.join(rundir, "run.log")) as f:
                lines = [l for l in f if "[ShadowTail]" in l]
        except OSError as e:
            print("FAIL check-freeze: unreadable run.log %s: %s" % (rundir, e))
            return 1
        if not lines:
            print("FAIL check-freeze: no snapshots: %s" % rundir)
            return 1
        for line in lines:
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
    for _, slot, _, head, data in snaps:
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
    required_fams = FAMILY_REQUIRED.get(args.config)
    if required_fams is None:
        print("FAIL check-freeze: --config required (one of C1..C4)")
        return 1
    for fam, edges in ((0, edgesRT), (1, edgesGI)):
        n = totals[fam]
        if fam not in required_fams:
            continue  # N/A family: validated sums only, no minimums
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

    # Golden serialized outputs (paired with the C++ SnapshotJson test):
    # the derive/control integration below consumes these ACTUAL strings,
    # so a format drift breaks here instead of silently passing on
    # hand-written JSON.
    gp = json.loads(TEST_GOLDEN_PILOT_JSON)
    check("golden pilot prefix/schema",
          TEST_GOLDEN_PILOT_JSON.startswith(
              '{"schema":"rt2.shadow-tail.v1","frame":41,'
              '"slot":0,"serial":7,"valid":false,'
              '"pilot":true,"mode":1,'
              '"reason":"pilot-only: no manifest staged",'
              '"head":[1397245015,1,41,1,0,0,1,0],"data":['))
    try:
        frame, slot, _, _, data = parse_snapshot_data(gp)
        pilot = [[0] * 34, [0] * 34]
        base = slot * (2 * 298 + 2)
        for fam in (0, 1):
            for i in range(34):
                pilot[fam][i] += data[base + fam * 298 + i]
        code, lo, hi = pilot_bracket(pilot[0])
        check("golden pilot RT bracket",
              code == 0 and lo == 1 and hi == 2)
        check("golden pilot GI empty",
              pilot_bracket(pilot[1])[0] == "empty")
        ert = build_edges(lo, hi)
        check("golden pilot RT edges",
              len(ert) == 5 and ert[0] == 0 and ert[-1] == 4)
    except (ValueError, KeyError, IndexError):
        check("golden pilot derives edges", False)
    gc = json.loads(TEST_GOLDEN_CONTROL_JSON)
    check("golden control prefix/schema",
          TEST_GOLDEN_CONTROL_JSON.startswith(
              '{"schema":"rt2.shadow-tail.v1","frame":42,'
              '"slot":1,"serial":9,"valid":false,'
              '"pilot":false,"mode":2,'
              '"reason":"positive-control mode",'
              '"head":[1397245015,1,42,2,5,3,1,1],"data":['))
    try:
        frame, slot, _, _, data = parse_snapshot_data(gc)
        ctl = data[slot * (2 * 298 + 2) + 596 + 1]
        check("golden control checksum readable", ctl == 305)
    except (ValueError, KeyError, IndexError):
        check("golden control checksum readable", False)

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
        frame, slot, _, _, _ = validate_snapshot_line(good)
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
    fails_cov = check_coverage("C1", [(41, 0, 7, good["head"], good["data"])])
    check("C1 coverage passes", fails_cov == [])
    fails_cov2 = check_coverage("C4", [(41, 0, 7, good["head"], good["data"])])
    check("C4 coverage fails honestly", len(fails_cov2) > 0)
    check("unknown config fails", check_coverage("CX", []) != [])

    # Fake-process orchestration: end-to-end run_capture without GPU.
    def fake_run(exe_, cmd, cwd, logpath, snaps_fn, png=True, rc=0):
        from PIL import Image
        idx = cmd.index("--output-display") + 1
        if png:
            Image.new("RGB", (8, 8), (10, 20, 30)).save(cmd[idx])
        with open(logpath, "w") as lf:
            for line in snaps_fn():
                lf.write(line)
            if "--benchmark-timings" in cmd:
                lf.write('[HeadlessTiming] {"frame":60,"regions_ms":'
                         '{"RT Shading":0.5,"GPU Frame":1.0}}\n')
        return rc

    def snap_lines(n=20, start=41, valid=True, oob=False, empty=False):
        lines = []
        for f in range(start, start + n):
            s = dict(good, frame=f, serial=f)
            if not valid:
                s = dict(s, valid=False, reason="pilot-only")
            if oob:
                s = dict(s, data=[0] * 10)
            if empty:
                s = dict(s, data=[0] * 1196)
            lines.append("[ShadowTail] %s\n" % json.dumps(s))
        return lines

    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "RT2App.exe")
        with open(exe, "wb") as f:
            f.write(b"fake-exe")
        for spv in ("closesthit.spv", "secondary_raygen.spv",
                    "restir_gi_temporal.spv", "closesthit_tail.spv",
                    "secondary_raygen_tail.spv",
                    "restir_gi_temporal_tail.spv"):
            with open(os.path.join(tmp, spv), "wb") as f:
                f.write(b"fake-spv")
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

        # derive-freeze on synthetic pilot trial rundirs (golden edges).
        # Each trial dir carries completion + provenance (ref-exe matched).
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
        trial_dirs = []
        for t in range(2):
            tdir = os.path.join(pdir, "T%d" % (t + 1))
            os.makedirs(tdir)
            with open(os.path.join(tdir, "run.log"), "w") as f:
                f.write("[ShadowTail] %s\n" % json.dumps(pline))
            with open(os.path.join(tdir, "completion.json"), "w") as f:
                json.dump({"complete": True}, f)
            with open(os.path.join(tdir, "provenance.json"), "w") as f:
                json.dump({"exe_sha256": sha256_file(exe)}, f)
            trial_dirs.append(tdir)
        mout = os.path.join(tmp, "frozen.json")
        da = argparse.Namespace(trial_dirs=trial_dirs, ref_exe=exe, out=mout)
        check("fake derive-freeze", cmd_derive_freeze(da) == 0)
        with open(mout) as f:
            frozen = json.load(f)
        check("frozen edges golden",
              len(frozen["edgesRT"]) == 226 and frozen["edgesRT"][1] == 4096
              and frozen["edgesRT"][-1] == 32768)
        # Wrong-exe trial fails closed.
        other = os.path.join(tmp, "other.exe")
        with open(other, "wb") as f:
            f.write(b"other")
        da_bad = argparse.Namespace(trial_dirs=trial_dirs, ref_exe=other,
                                    out=os.path.join(tmp, "frozen-bad.json"))
        check("fake derive-freeze exe mismatch fails",
              cmd_derive_freeze(da_bad) == 1)

        # check-freeze pass + fail paths on explicit confirmation rundirs.
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
        # C3 strata: RT tri primary/recursive + punctual, GI tri fresh/replay.
        cline["data"][0 + 0 * 298 + 264 + 1 * 2 + 0] = 120
        cline["data"][0 + 0 * 298 + 264 + 1 * 2 + 1] = 110
        cline["data"][0 + 0 * 298 + 264 + 4 * 2 + 0] = 100
        cline["data"][0 + 0 * 298 + 264 + 4 * 2 + 1] = 100
        cline["data"][0 + 1 * 298 + 264 + 0 * 2 + 0] = 110
        cline["data"][0 + 1 * 298 + 264 + 0 * 2 + 1] = 105
        with open(os.path.join(cdir, "run.log"), "w") as f:
            f.write("[ShadowTail] %s\n" % json.dumps(cline))
        with open(os.path.join(cdir, "completion.json"), "w") as f:
            json.dump({"complete": True}, f)
        with open(os.path.join(cdir, "provenance.json"), "w") as f:
            json.dump({"exe_sha256": sha256_file(exe)}, f)
        ca = argparse.Namespace(run_dirs=[cdir], manifest=mout,
                               config="C3", ref_exe=exe)
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

        # Analyze: fake 4-block campaign (C1, identical R/C → PASS).
        from PIL import Image as _PILImage
        adir = os.path.join(tmp, "campaign")
        os.makedirs(adir)
        er = frozen["edgesRT"]
        birt_a = bisect.bisect_right(er, 10000) - 1

        def make_block(bname, slow_cand=False):
            bdir = os.path.join(adir, bname)
            os.makedirs(bdir)
            for sub, shift in (("R_ON", 0), ("R_OFF", 0),
                               ("C_ON", 1 if slow_cand else 0),
                               ("C_OFF", 0)):
                sdir = os.path.join(bdir, sub)
                os.makedirs(sdir)
                lines = []
                for f in range(41, 61):
                    s = {"schema": SCHEMA, "frame": f, "slot": 0,
                         "serial": f, "valid": True,
                         "head": [0x53484457, 1, f, 1, 226, 226, 1, 0],
                         "data": [0] * 1196}
                    b = birt_a + (20 if (shift and sub == "C_ON") else 0)
                    s["data"][0 + 0 * 298 + 34 + b] = 1000
                    s["data"][0 + 0 * 298 + 260] = 1000
                    s["data"][0 + 0 * 298 + 261] = 1000
                    s["data"][0 + 0 * 298 + 264 + 1 * 2 + 0] = 600
                    s["data"][0 + 0 * 298 + 264 + 1 * 2 + 1] = 550
                    s["data"][0 + 0 * 298 + 264 + 4 * 2 + 0] = 700
                    s["data"][0 + 0 * 298 + 264 + 4 * 2 + 1] = 650
                    lines.append("[ShadowTail] %s" % json.dumps(s))
                    lines.append('[HeadlessTiming] {"frame":%d,'
                                 '"regions_ms":{"RT Shading":0.5,'
                                 '"GPU Frame":1.0}}' % f)
                with open(os.path.join(sdir, "run.log"), "w") as lf:
                    lf.write("\n".join(lines) + "\n")
                with open(os.path.join(sdir, "completion.json"), "w") as cf:
                    json.dump({"complete": True}, cf)
                with open(os.path.join(sdir, "provenance.json"), "w") as pf:
                    json.dump({"exe_sha256": "x"}, pf)
                _PILImage.new("RGB", (8, 8), (10, 20, 30)).save(
                    os.path.join(sdir, "frame.png"))
            with open(os.path.join(bdir, "block.json"), "w") as bf:
                json.dump({"block": bname, "complete": True, "results": {
                    "R_ON": "complete", "R_OFF": "complete",
                    "C_ON": "complete", "C_OFF": "complete",
                    "R_timing_parity": "complete",
                    "C_timing_parity": "complete",
                    "R_image_parity": "complete",
                    "C_image_parity": "complete"}}, bf)

        for bname in ("k1", "k2", "k3", "k4"):
            make_block(bname)
        ana = argparse.Namespace(
            block_dirs=[os.path.join(adir, b) for b in
                        ("k1", "k2", "k3", "k4")],
            manifest=mout, config="C1")
        check("fake analyze passes", cmd_analyze(ana) == 0)
        for bname in ("j1", "j2", "j3", "j4"):
            make_block(bname, slow_cand=True)
        ana2 = argparse.Namespace(
            block_dirs=[os.path.join(adir, b) for b in
                        ("j1", "j2", "j3", "j4")],
            manifest=mout, config="C1")
        check("fake analyze slow-candidate fails",
              cmd_analyze(ana2) == 1)
        ana3 = argparse.Namespace(
            block_dirs=[os.path.join(adir, b) for b in ("k1", "k2", "k3")],
            manifest=mout, config="C1")
        check("fake analyze 3-block inconclusive",
              cmd_analyze(ana3) == 2)

        # Quartet WITH config: OFF legs must skip coverage (they carry
        # none by design) while ON legs enforce it. snap_lines carries
        # C1 strata; C1 ON legs pass, OFF legs pass without coverage.
        def c1_lines():
            return snap_lines()
        qa3 = argparse.Namespace(
            ref_exe=exe, cand_exe=exe, scene=scene, out_root=tmp,
            name="b3", frames=60, spp=1, width=64, height=64,
            edges=mf, control=False, config="C1", extra_args=[])
        # good[] carries C1 RT tri/punctual strata (>=50 each) — reuse it.
        check("fake quartet with config passes OFF legs",
              run_quartet(qa3, run_fn=lambda e, c, w, l: fake_run(
                  e, c, w, l, c1_lines)) == 0)

        # Missing tail SPVs fail diag captures loudly.
        bare = os.path.join(tmp, "bare")
        os.makedirs(bare)
        bare_exe = os.path.join(bare, "RT2App.exe")
        with open(bare_exe, "wb") as f:
            f.write(b"fake-exe")
        a9 = mkargs("nospv")
        a9.exe = bare_exe
        check("fake missing tail SPVs fail",
              run_capture(a9, run_fn=lambda e, c, w, l: fake_run(
                  e, c, w, l, lambda: snap_lines())) == 1)

        # Measured-window rule: frames 41..60 must all be present.
        def gap_lines():
            return [l for l in snap_lines()
                    if json.loads(l.split("[ShadowTail]", 1)[1])["frame"] != 55]
        a10 = mkargs("gap")
        check("fake window gap fails",
              run_capture(a10, run_fn=lambda e, c, w, l: fake_run(
                  e, c, w, l, gap_lines)) == 1)

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
    df.add_argument("--trial-dirs", nargs="+", required=True)
    df.add_argument("--ref-exe", required=True)
    df.add_argument("--out", required=True)
    cf = sub.add_parser("check-freeze")
    cf.add_argument("--run-dirs", nargs="+", required=True)
    cf.add_argument("--manifest", required=True)
    cf.add_argument("--config", required=True)
    cf.add_argument("--ref-exe", required=True)
    an = sub.add_parser("analyze")
    an.add_argument("--block-dirs", nargs="+", required=True)
    an.add_argument("--manifest", required=True)
    an.add_argument("--config", required=True)
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
    if args.cmd == "analyze":
        return cmd_analyze(args)
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
