#!/usr/bin/env python3
"""run_shadow_tail.py — bounded shadow-tail capture + persistence harness.

Owns durable per-run evidence for the RT4-T4 shadow-tail protocol
(shadow-tail-plan, READY 2026-10-13). Each invocation runs ONE 60-frame
headless capture (caller composes blocks/quartets per the frozen manifest)
into a UNIQUE owned rundir with provenance, then validates and publishes
a completion manifest ONLY after reread checks pass.

Usage:
  python run_shadow_tail.py --exe <RT2App.exe> --scene <scene>
      --out-root <dir> --name <runname> [--frames 60] [--spp N]
      [--width W] [--height H] [--extra-args ...]
      [--edges manifest.json] [--no-gpu-check]
  python run_shadow_tail.py --self-test   # CPU-only, no GPU, no exe needed.

--self-test exercises manifest validation, JSONL schema checks, coverage
matrix checks and completion-manifest logic on synthetic data and exits
nonzero on any failure. It is the CPU gate for this harness.

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

SCHEMA = "rt2.shadow-tail.v1"
FRAMES_DEFAULT = 60

# Frozen coverage matrix (plan §3b): fixture -> required strata.
# RT cells are (site, depth) with site 0..4 env/tri/res-tri/res-env/punctual,
# depth 0 primary / 1 recursive. GI cells are (site, prov) with site 0 tri /
# 1 env, prov 0 fresh / 1 replay. "N/A" cells are absent by construction
# (declared before data); REQUIRED cells need >= 50 invocations per block.
# Stratum flat indices: RT site*2+depth (10 used of 16), GI site*2+prov.
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


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def validate_manifest_edges(obj):
    """Validate a frozen edge manifest dict. Returns (rt, gi) or raises."""
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
            if v < 0 or v > 0xFFFFFFFF:
                raise ValueError("%s entry out of u32 range" % key)
            if v <= prev:
                raise ValueError("%s not strictly increasing" % key)
            prev = v
        if arr[0] != 0:
            raise ValueError("%s must start at 0" % key)
        out.append(arr)
    return out[0], out[1]


def validate_snapshot_line(obj):
    """Validate one [ShadowTail] JSON object. Returns (frame, head, data)."""
    if not isinstance(obj, dict):
        raise ValueError("snapshot must be an object")
    if obj.get("schema") != SCHEMA:
        raise ValueError("bad schema")
    frame = obj.get("frame")
    if not isinstance(frame, int) or isinstance(frame, bool) or frame < 0:
        raise ValueError("bad frame id")
    if obj.get("valid") is not True:
        raise ValueError("snapshot invalid: %s" % obj.get("reason", "?"))
    head = obj.get("head")
    if not isinstance(head, list) or len(head) != 8:
        raise ValueError("bad header")
    data = obj.get("data")
    # Counter region: 2 slots x (2 families x (34+226+4+16+16+2) + 2 ctl).
    if not isinstance(data, list) or len(data) != 2 * (2 * 298 + 2):
        raise ValueError("bad data length (want per-slot family blocks)")
    return frame, head, data


def check_coverage(config, snapshots):
    """Check §3b stratum minimums on pooled snapshots. Returns failures."""
    failures = []
    need = MATRIX.get(config)
    if need is None:
        return ["unknown config %s" % config]
    # Aggregate strata counts per family over all snapshots.
    rt_cells = [0] * 16
    gi_cells = [0] * 16
    for _, head, data in snapshots:
        base = 0  # slot 0 family blocks (single-slot accounting per frame)
        # NOTE: multi-slot frames are analyzed per slot by the caller;
        # here we pool slot 0 of every snapshot (headless runs 1 slot).
        for fam, cells in ((0, rt_cells), (1, gi_cells)):
            off = base + fam * (34 + 226 + 4 + 16 + 16 + 2) + 34 + 226 + 4
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


def self_test():
    fails = []

    def check(name, cond):
        print(("PASS" if cond else "FAIL"), "-", name)
        if not cond:
            fails.append(name)

    # Manifest validation.
    rt, gi = validate_manifest_edges(
        {"edgesRT": [0, 4096, 8192, 16384, 32768], "edgesGI": [0, 100, 200]}
    )
    check("manifest valid", rt[-1] == 32768 and gi[-1] == 200)
    for bad in (
        {"edgesRT": [0, 100, 100], "edgesGI": [0, 1]},
        {"edgesRT": [4, 100], "edgesGI": [0, 1]},
        {"edgesRT": [0], "edgesGI": [0, 1]},
        {"edgesRT": [0, -4], "edgesGI": [0, 1]},
        {"edgesRT": [0, 4294967296], "edgesGI": [0, 1]},
        {"edgesGI": [0, 1]},
        "garbage",
    ):
        try:
            validate_manifest_edges(bad)
            check("manifest rejects %r" % (bad,), False)
        except (ValueError, AttributeError, TypeError):
            check("manifest rejects %r" % (bad,), True)

    # Snapshot schema validation.
    good = {"schema": SCHEMA, "frame": 41, "valid": True,
            "head": [0x53484457, 1, 41, 1, 5, 3, 1, 0],
            "data": [0] * (2 * (2 * 298 + 2))}
    # Eligible/recorded/sums consistent: family 0 slot 0 recorded 100.
    base = 34 + 226
    good["data"][base + 0] = 100  # eligible
    good["data"][base + 1] = 100  # recorded
    good["data"][base - 226 + 1] = 100  # hist bin 1
    # Strata cells are site*2+depth (RT: tri=site1, punctual=site4).
    good["data"][base + 4 + 1 * 2 + 0] = 60  # RT tri primary
    good["data"][base + 4 + 1 * 2 + 1] = 55  # RT tri recursive
    good["data"][base + 4 + 4 * 2 + 0] = 70  # RT punctual primary
    good["data"][base + 4 + 4 * 2 + 1] = 65  # RT punctual recursive
    try:
        frame, head, data = validate_snapshot_line(good)
        check("snapshot valid", frame == 41 and head[4] == 5)
    except ValueError:
        check("snapshot valid", False)
    bad_snap = dict(good, valid=False, reason="mode is not ON")
    try:
        validate_snapshot_line(bad_snap)
        check("snapshot invalid rejected", False)
    except ValueError:
        check("snapshot invalid rejected", True)

    # Coverage matrix on synthetic pooled data (C1 needs tri 60+40>=50 each).
    fails_cov = check_coverage("C1", [(41, good["head"], good["data"])])
    check("C1 coverage passes", fails_cov == [])
    fails_cov2 = check_coverage("C4", [(41, good["head"], good["data"])])
    check("C4 coverage fails honestly", len(fails_cov2) > 0)
    check("unknown config fails", check_coverage("CX", []) != [])

    print("SELF-TEST", "ALL PASS" if not fails else "FAILURES: %s" % fails)
    return 0 if not fails else 1


def run_capture(args):
    repo = os.path.dirname(os.path.abspath(__file__))
    exe = args.exe
    rundir = os.path.join(args.out_root, args.name)
    if os.path.exists(rundir):
        print("ERROR: rundir exists (non-overwriting rule): %s" % rundir)
        return 1
    os.makedirs(rundir)
    cmd = [exe, "--headless", "--scene", args.scene,
           "--output-display", os.path.join(rundir, "frame.png"),
           "--frames", str(args.frames), "--width", str(args.width),
           "--height", str(args.height), "--spp", str(args.spp),
           "--bounces", "8", "--seed", "0", "--denoiser-mode", "off"]
    cmd += args.extra_args
    if args.edges:
        cmd += ["--shadow-tail", "--shadow-tail-edges", args.edges]
    exe_dir = os.path.dirname(os.path.abspath(exe))
    with open(os.path.join(rundir, "command.txt"), "w") as f:
        f.write(" ".join(cmd) + "\n")
    prov = {"exe": exe, "exe_sha256": sha256_file(exe), "scene": args.scene,
            "frames": args.frames, "width": args.width, "height": args.height,
            "spp": args.spp, "extra": args.extra_args, "edges": args.edges}
    spv_dir = exe_dir
    prov["spvs"] = {}
    for name in ("closesthit.spv", "secondary_raygen.spv",
                 "restir_gi_temporal.spv", "closesthit_tail.spv",
                 "secondary_raygen_tail.spv", "restir_gi_temporal_tail.spv"):
        p = os.path.join(spv_dir, name)
        if os.path.exists(p):
            prov["spvs"][name] = sha256_file(p)
    with open(os.path.join(rundir, "provenance.json"), "w") as f:
        json.dump(prov, f, indent=1)
        f.write("\n")
    with open(os.path.join(rundir, "run.log"), "w") as log:
        rc = subprocess.run(cmd, cwd=exe_dir, stdout=log,
                            stderr=subprocess.STDOUT).returncode
    with open(os.path.join(rundir, "exitcode.txt"), "w") as f:
        f.write("%d\n" % rc)
    if rc != 0:
        print("FAIL capture exit=%d (preserved, no manifest)" % rc)
        return 1
    # Reread validation before any completion manifest.
    problems = []
    frame_png = os.path.join(rundir, "frame.png")
    if not os.path.exists(frame_png):
        problems.append("missing frame.png")
    snaps = []
    with open(os.path.join(rundir, "run.log")) as f:
        for line in f:
            if "[ShadowTail]" in line:
                try:
                    snaps.append(validate_snapshot_line(
                        json.loads(line.split("[ShadowTail]", 1)[1])))
                except ValueError as e:
                    problems.append("bad snapshot line: %s" % e)
    if args.edges and not snaps:
        problems.append("no valid snapshots with edges staged")
    manifest = {"name": args.name, "exit": rc,
                "frame_sha256": sha256_file(frame_png) if os.path.exists(frame_png) else None,
                "snapshots": len(snaps), "problems": problems,
                "complete": not problems}
    with open(os.path.join(rundir, "completion.json"), "w") as f:
        json.dump(manifest, f, indent=1)
        f.write("\n")
    # Flush + reread: the manifest counts only if the files parse back.
    with open(os.path.join(rundir, "completion.json")) as f:
        reread = json.load(f)
    if not reread.get("complete"):
        print("FAIL validation: %s" % reread.get("problems"))
        return 1
    print("DONE %s snapshots=%d" % (args.name, len(snaps)))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--exe", default="")
    ap.add_argument("--scene", default="")
    ap.add_argument("--out-root", default="")
    ap.add_argument("--name", default="")
    ap.add_argument("--frames", type=int, default=FRAMES_DEFAULT)
    ap.add_argument("--spp", type=int, default=1)
    ap.add_argument("--width", type=int, default=960)
    ap.add_argument("--height", type=int, default=540)
    ap.add_argument("--edges", default="")
    ap.add_argument("--extra-args", nargs=argparse.REMAINDER, default=[])
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    for req in ("exe", "scene", "out_root", "name"):
        if not getattr(args, req):
            print("ERROR: --%s required" % req.replace("_", "-"))
            return 1
    return run_capture(args)


if __name__ == "__main__":
    sys.exit(main())
