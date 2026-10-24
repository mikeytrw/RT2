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
   campaign     Executes the tables for real: calib (18 runs, 90-min cap)
                or accept (64 runs, 150-min cap) with freeze gating,
                legacy OFF parity and a campaign manifest. No GPU work
                happens in --self-test (injected fake processes only).

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
import re
import shutil
import subprocess
import uuid
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
# Configs whose GI dispatch must produce timing records (plan §7: the GI
# temporal dispatch is an affected dispatch wherever GI runs). C1/C2 run
# with GI off, so absence there is correct — never an exemption excuse.
GI_TIMING_REQUIRED = {"C1": False, "C2": False, "C3": True, "C4": True}

# Frozen per-config workloads (plan §9 + §3b). The campaign builds EVERY
# capture command SOLELY from this table — one shared --spp/--extra set
# cannot encode the contract (C1 pure-path SPP 8 vs raster-first DI/GI
# fixtures, tiny_env only on C2, courtyard camera only on C4), and
# caller-supplied extras must never override it. Candidate counts stay
# at renderer defaults (recorded as 0 = default for the pinned exe);
# denoiser-off is in the base command for all configs. Provenance binds
# each run to its workload entry; derive/check/analyze verify the seal.
TINY_ENV = "RT2App/assets/phase1a-fixtures/tiny_env.exr"
WORKLOADS = {
    "C1": {"spp": 8, "width": 960, "height": 540, "extra": []},
    "C2": {"spp": 1, "width": 960, "height": 540,
           "extra": ["--raster-first", "--restir", "--env", TINY_ENV]},
    "C3": {"spp": 1, "width": 960, "height": 540,
           "extra": ["--raster-first", "--restir-gi"]},
    "C4": {"spp": 1, "width": 1470, "height": 609,
           "extra": ["--raster-first", "--restir-gi",
                     "--camera-pos", "0", "1.6", "0",
                     "--camera-forward", "1", "0", "0"]},
}
# Affected dispatch regions for the observer gates (plan §7): RT shading
# everywhere, plus GI temporal wherever GI runs.
OBSERVER_REGIONS = ("RT Shading", "ReSTIR GI Temporal")
CALIB_HEADROOM = 1.03  # per-config ON/OFF dispatch ceiling (§3/§8)


def check_workload(prov, config):
    """Frozen-workload seal check: provenance must carry exactly the
    WORKLOADS[config] entry. Returns error string or ''."""
    want = WORKLOADS.get(config)
    if want is None:
        return "unknown config %s" % config
    got = prov.get("workload")
    if not isinstance(got, dict):
        return "run has no workload provenance"
    for key in ("spp", "width", "height", "extra"):
        if got.get(key) != want[key]:
            return ("workload mismatch for %s: %s=%r want %r"
                    % (config, key, got.get(key), want[key]))
    if prov.get("config", "") != config:
        return ("config mismatch: prov %r vs %s"
                % (prov.get("config", ""), config))
    return ""


def sha256_text(text):
    """SHA-256 of a unicode string (UTF-8)."""
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def canonical_edges(edges_rt, edges_gi):
    """Canonical edge-array encoding for the lineage seal."""
    return json.dumps({"edgesRT": edges_rt, "edgesGI": edges_gi},
                      sort_keys=True)


def hash_exe_spvs(exe_path):
    """SHA-256 of the six shadow SPVs beside an executable (missing
    files map to None — callers decide whether absence fails)."""
    exe_dir = os.path.dirname(os.path.abspath(exe_path))
    out = {}
    for name in ("closesthit.spv", "secondary_raygen.spv",
                 "restir_gi_temporal.spv", "closesthit_tail.spv",
                 "secondary_raygen_tail.spv",
                 "restir_gi_temporal_tail.spv"):
        path = os.path.join(exe_dir, name)
        try:
            out[name] = sha256_file(path)
        except OSError:
            out[name] = None
    return out


def check_freeze_seal(manifest, manifest_hash=None):
    """Reference-only lineage seal: the manifest must carry the frozen
    WORKLOADS table and a lineage record naming the exact calibration
    trials that produced it. A structurally valid but hand-authored
    edge file has no lineage and cannot unlock acceptance. Returns
    error string or ''."""
    if not isinstance(manifest, dict):
        return "manifest root must be an object"
    if manifest.get("workload") != WORKLOADS:
        return ("manifest workload seal mismatch: not the frozen "
                "per-config table")
    lineage = manifest.get("lineage")
    if not isinstance(lineage, dict):
        return "manifest has no calibration lineage"
    trials = lineage.get("trials")
    if not isinstance(trials, dict) or set(trials.keys()) != set(CONFIGS):
        return ("lineage must name exactly the C1..C4 calibration trials, "
                "got %r" % sorted(trials.keys() if isinstance(trials, dict)
                                        else []))
    if not lineage.get("ref_exe_sha256"):
        return "lineage has no reference exe identity"
    try:
        edges_rt, edges_gi = validate_manifest_edges(manifest)
    except ValueError as e:
        return "manifest edges invalid: %s" % e
    if lineage.get("edges_sha256") != sha256_text(canonical_edges(
            edges_rt, edges_gi)):
        return ("lineage edge seal mismatch: edge arrays were altered "
                "after derivation")
    if (manifest_hash is not None and lineage.get("manifest_sha256") and
            lineage.get("manifest_sha256") != manifest_hash):
        return "lineage manifest hash does not match this file"
    return ""


def seal_path_for(freeze_path):
    """Sidecar path for the calibration seal of a freeze manifest."""
    return freeze_path + ".calibration.json"


def seal_calibration(freeze_path, campdir, legacy_exe):
    """Seal a campaign-derived freeze with full calibration evidence.

    Called by a COMPLETED calib campaign on its own freeze file. The
    seal lives in a SIDECAR file (`<freeze>.calibration.json`), never
    by rewriting the freeze: every edges_sha256 hash recorded against
    the frozen file during the campaign must keep matching it byte for
    byte. The sidecar binds the freeze hash to the reread campaign
    record (complete, mode calib, within cap) with the full gate
    families (4 check-freeze-*, 4 parity-*, 4 headroom-*, 2
    response-* all complete) plus every evidence directory. The legacy
    authority comes from the ACTUAL campaign input binary (path +
    current file hash + SPV set) — never from copies of each run's
    self-designated hash; all four legacy runs must already use
    exactly that binary or sealing fails. A trials-only freeze has no
    sidecar and can never unlock acceptance. Returns '' or FAIL (and
    never writes on failure)."""
    try:
        with open(os.path.join(campdir, "campaign.json")) as f:
            camp = json.load(f)
    except (OSError, ValueError) as e:
        return "cannot read campaign record: %s" % e
    if not camp.get("complete") or camp.get("mode") != "calib":
        return "campaign record is not a completed calibration"
    if camp.get("elapsed_min", 1e9) > camp.get("cap_min", 0):
        return "campaign record exceeds its cap"
    stages = dict(camp.get("stages", []))
    required = (["check-freeze-%s" % c for c in CONFIGS] +
                ["parity-%s" % c for c in CONFIGS] +
                ["headroom-%s" % c for c in CONFIGS] +
                ["response-control-RT", "response-control-GI"])
    for name in required:
        if stages.get(name) != "complete":
            return "calibration stage missing/incomplete: %s" % name
    try:
        freeze_hash = sha256_file(freeze_path)
    except OSError as e:
        return "cannot hash freeze file: %s" % e
    # Explicit legacy authority from the ACTUAL campaign input —
    # path, current file hash and SPV set. Every legacy run must
    # already use exactly this binary (same path AND same hashes);
    # four arbitrary different binaries, or runs disagreeing with the
    # input, fail sealing here. Byte-identical binaries at other
    # paths do not satisfy the designated-path requirement.
    legacy_abs = os.path.abspath(legacy_exe)
    try:
        authority = {"path": legacy_abs,
                     "sha256": sha256_file(legacy_abs),
                     "spvs": hash_exe_spvs(legacy_abs)}
    except OSError as e:
        return "cannot hash designated legacy exe: %s" % e
    for cfg in CONFIGS:
        try:
            with open(os.path.join(os.path.abspath(campdir),
                                   "legacy-%s" % cfg,
                                   "provenance.json")) as f:
                leg_prov = json.load(f)
        except (OSError, ValueError) as e:
            return "cannot read legacy provenance %s: %s" % (cfg, e)
        if os.path.abspath(leg_prov.get("exe", "")) != legacy_abs:
            return ("legacy run %s uses a different binary than the "
                    "designated production exe" % cfg)
        if leg_prov.get("exe_sha256") != authority["sha256"]:
            return ("legacy run %s exe hash differs from designated "
                    "production exe" % cfg)
        if leg_prov.get("spvs", {}) != authority["spvs"]:
            return ("legacy run %s SPV set differs from designated "
                    "production set" % cfg)
    sidecar = {
        "sealed": True,
        "freeze_sha256": freeze_hash,
        "campaign": os.path.abspath(campdir),
        "campaign_sha256": sha256_file(
            os.path.join(campdir, "campaign.json")),
        "stages": {k: stages[k] for k in required},
        "legacy_authority": authority,
        "confirms": {c: os.path.join(os.path.abspath(campdir),
                                     "confirm-%s" % c) for c in CONFIGS},
        "offs": {c: os.path.join(os.path.abspath(campdir), "off-%s" % c)
                 for c in CONFIGS},
        "legacys": {c: os.path.join(os.path.abspath(campdir),
                                    "legacy-%s" % c) for c in CONFIGS},
        "controls": {"control-RT": os.path.join(os.path.abspath(campdir),
                                                 "control-RT"),
                     "control-GI": os.path.join(os.path.abspath(campdir),
                                                 "control-GI")},
    }
    err = write_text_checked(seal_path_for(freeze_path),
                             json.dumps(sidecar, indent=1) + "\n")
    if err:
        return "seal write failed: %s" % err
    return ""


def revalidate_calibration(manifest, freeze_path, ref_exe):
    """Recompute the calibration gates behind a sealed freeze.

    Evidence comes ONLY from canonical campaign-owned run
    directories — never from editable sidecar strings. The sidecar
    paths are cross-checked against the canonical locations
    (alias/symlink substitution fails); the campaign stage table must
    be EXACTLY the 33 expected entries in order with the exact
    18-run census and no aliased directories; every run is
    identity/mode/window-validated for its ROLE (pilot trials,
    confirmations, true-OFF, designated-production legacy, mode-2
    controls) before ANY gate is recomputed (confirmations,
    OFF/legacy parity, headroom, control responses). A hash of an
    otherwise unchanged campaign file does not authenticate anything
    by itself. Returns '' or FAIL."""
    try:
        with open(seal_path_for(freeze_path)) as f:
            cal = json.load(f)
    except (OSError, ValueError) as e:
        return ("no calibration seal sidecar: a trials-only freeze "
                "cannot unlock acceptance (%s)" % e)
    if cal.get("sealed") is not True:
        return ("no calibration seal: a trials-only freeze cannot "
                "unlock acceptance")
    try:
        if sha256_file(freeze_path) != cal.get("freeze_sha256"):
            return ("freeze file changed since calibration sealing "
                    "(edges file differs from sealed evidence)")
    except OSError as e:
        return "freeze reread failed: %s" % e
    campdir = cal.get("campaign", "")
    try:
        with open(os.path.join(campdir, "campaign.json")) as f:
            camp = json.load(f)
    except (OSError, ValueError) as e:
        return "calibration campaign unreadable: %s" % e
    if not camp.get("complete") or camp.get("mode") != "calib":
        return "calibration campaign not complete"
    if camp.get("elapsed_min", 1e9) > camp.get("cap_min", 0):
        return "calibration campaign exceeds its cap"
    try:
        if sha256_file(os.path.join(campdir, "campaign.json")) != \
                cal.get("campaign_sha256"):
            return "calibration campaign record changed since sealing"
    except OSError as e:
        return "calibration campaign reread failed: %s" % e
    # Exact stage table: names in order, all complete. A mutated table
    # (even with a re-pointed hash) cannot pass.
    if camp.get("stages") != [[n, "complete"]
                               for n in expected_calib_stages()]:
        return ("calibration stage table differs from the approved "
                "18-run schedule")
    if camp.get("environment_authorities") != {
            cfg: manifest.get("lineage", {}).get("trials", {}).get(
                cfg, {}).get("environment") for cfg in CONFIGS}:
        return "calibration campaign environment authorities differ from trials"
    # Exact run census: precisely the 18 canonical run directories
    # plus the three campaign files — nothing missing, nothing extra.
    try:
        entries = sorted(os.listdir(campdir))
    except OSError as e:
        return "calibration campaign listing failed: %s" % e
    freeze_base = os.path.basename(freeze_path)
    want_entries = sorted(expected_calib_runs() +
                          ["campaign.json", freeze_base,
                           freeze_base + ".calibration.json"])
    if entries != want_entries:
        return ("calibration run census differs: missing %s extra %s"
                % (sorted(set(want_entries) - set(entries)),
                   sorted(set(entries) - set(want_entries))))
    canonical = {name: os.path.join(campdir, name)
                 for name in expected_calib_runs()}
    # Alias rejection: all 18 canonical dirs must resolve to 18
    # DISTINCT real locations (a symlinked off→confirm collapses).
    realpaths = {}
    for name, path in canonical.items():
        if not os.path.isdir(path):
            return "calibration run dir missing: %s" % name
        realpaths[name] = os.path.realpath(path)
    if len(set(realpaths.values())) != len(realpaths):
        dupes = sorted(n for n in realpaths
                       if list(realpaths.values()).count(
                           realpaths[n]) > 1)
        return "calibration run dirs aliased: %s" % dupes
    # Sidecar cross-check: recorded evidence paths must resolve to the
    # canonical campaign-owned locations (relabeling confirms as OFF
    # or legacy fails here even if the campaign file is untouched).
    for section, names in (("confirms", CONFIGS), ("offs", CONFIGS),
                           ("legacys", CONFIGS)):
        for cfg in names:
            recorded = cal.get(section, {}).get(cfg, "")
            if not recorded or os.path.realpath(recorded) != \
                    realpaths[{"confirms": "confirm-%s" % cfg,
                               "offs": "off-%s" % cfg,
                               "legacys": "legacy-%s" % cfg}[section]]:
                return ("sidecar path substitution for %s-%s"
                        % (section, cfg))
    for tag in ("control-RT", "control-GI"):
        recorded = cal.get("controls", {}).get(tag, "")
        if not recorded or os.path.realpath(recorded) != \
                realpaths[tag]:
            return "sidecar path substitution for %s" % tag
    try:
        ref_hash = sha256_file(ref_exe)
    except OSError as e:
        return "cannot hash reference exe: %s" % e
    ref_exe_abs = os.path.abspath(ref_exe)
    if manifest.get("lineage", {}).get("ref_exe_sha256") != ref_hash:
        return "freeze lineage exe differs from current reference"
    # Legacy authority: the designated production binary recorded at
    # seal time — re-hash the CURRENT file (a post-seal swap fails)
    # and require its SPV set unchanged.
    authority = cal.get("legacy_authority", {})
    if not isinstance(authority, dict) or not authority.get("path"):
        return "calibration seal has no legacy authority"
    try:
        if sha256_file(authority["path"]) != authority.get("sha256", ""):
            return ("designated legacy exe changed since sealing: %s"
                    % authority.get("path", ""))
        if hash_exe_spvs(authority["path"]) != authority.get("spvs", {}):
            return "designated legacy SPV set changed since sealing"
    except OSError as e:
        return "designated legacy exe unreadable: %s" % e
    # Phase 1 — identity/mode/window per ROLE, before any gate math.
    # Trial scenes come from the canonical trial provenance records
    # (source, not sidecar strings).
    trial_scenes = {}
    try:
        for cfg in CONFIGS:
            with open(os.path.join(canonical["trial-%s" % cfg],
                                   "provenance.json")) as f:
                trial_prov = json.load(f)
            trial_rec = manifest.get("lineage", {}).get("trials", {}).get(
                cfg, {})
            trial_scenes[cfg] = {
                "path": trial_rec.get("scene", ""),
                "sha256": trial_rec.get("scene_sha256", ""),
                "environment": trial_rec.get("environment")}
            if (not trial_scenes[cfg]["path"] or
                    os.path.normcase(os.path.realpath(
                        trial_prov.get("scene", ""))) !=
                    os.path.normcase(os.path.realpath(
                        trial_scenes[cfg]["path"])) or
                    trial_prov.get("scene_sha256") !=
                    trial_scenes[cfg]["sha256"] or
                    trial_prov.get("environment") !=
                    trial_scenes[cfg]["environment"]):
                return "calibration trial fixture identity differs: %s" % cfg
    except (OSError, ValueError) as e:
        return "calibration trial provenance unreadable: %s" % e
    if not all(v["path"] and v["sha256"] for v in trial_scenes.values()):
        return "calibration trial scenes unhashed"
    for cfg in CONFIGS:
        confirm = canonical["confirm-%s" % cfg]
        cargs = argparse.Namespace(run_dirs=[confirm],
                                   manifest=freeze_path,
                                   config=cfg, ref_exe=ref_exe)
        if cmd_check_freeze(cargs) != 0:
            return "calibration confirmation revalidation failed: %s" % cfg
    for cfg in CONFIGS:
        err = validate_off_run(canonical["off-%s" % cfg], ref_exe_abs,
                               ref_hash, cfg, trial_scenes[cfg])
        if err:
            return err
    for cfg in CONFIGS:
        err = validate_legacy_run(canonical["legacy-%s" % cfg],
                                  authority, ref_hash, cfg,
                                  trial_scenes[cfg])
        if err:
            return err
    controls_cfg = {"control-RT": "C1", "control-GI": "C3"}
    for tag, cfg in controls_cfg.items():
        err = validate_control_run(canonical[tag], ref_exe_abs, ref_hash,
                                   cfg, trial_scenes[cfg])
        if err:
            return err
    # Phase 2 — gate recomputation on validated evidence only.
    for cfg in CONFIGS:
        confirm = canonical["confirm-%s" % cfg]
        off = canonical["off-%s" % cfg]
        legacy = canonical["legacy-%s" % cfg]
        if check_legacy_parity(off, legacy) != "complete":
            return "calibration OFF/legacy parity revalidation failed: %s" \
                % cfg
        if check_calib_headroom(confirm, off, cfg) != "":
            return "calibration headroom revalidation failed: %s" % cfg
    try:
        with open(freeze_path) as f:
            frozen = json.load(f)
        fRT, fGI = validate_manifest_edges(frozen)
    except (OSError, ValueError) as e:
        return "frozen edges unreadable: %s" % e
    pairs = (("control-RT", "C1", "confirm-C1"),
             ("control-GI", "C3", "confirm-C3"))
    for tag, cfg, ordinary in pairs:
        control = canonical[tag]
        ordinary_dir = os.path.join(campdir, ordinary)
        resp = check_control_response(control, ordinary_dir, cfg, (fRT, fGI))
        if resp != "":
            return ("calibration control-response revalidation failed "
                    "%s: %s" % (tag, resp))
    return ""


def expected_calib_stages():
    """Exact 33-stage table of a completed calibration campaign, in
    order: 4 trials, derive, 4 confirms, 4 check-freezes, per config
    off+legacy+parity+headroom, 2 controls + 2 responses."""
    stages = []
    for cfg in CONFIGS:
        stages.append("trial-%s" % cfg)
    stages.append("derive-freeze")
    for cfg in CONFIGS:
        stages.append("confirm-%s" % cfg)
    for cfg in CONFIGS:
        stages.append("check-freeze-%s" % cfg)
    for cfg in CONFIGS:
        stages.append("off-%s" % cfg)
        stages.append("legacy-%s" % cfg)
        stages.append("parity-%s" % cfg)
        stages.append("headroom-%s" % cfg)
    stages.append("control-RT")
    stages.append("response-control-RT")
    stages.append("control-GI")
    stages.append("response-control-GI")
    return stages


def expected_calib_runs():
    """Canonical 18 run-directory names owned by a calibration."""
    runs = []
    for cfg in CONFIGS:
        runs.append("trial-%s" % cfg)
    for cfg in CONFIGS:
        runs.append("confirm-%s" % cfg)
    for cfg in CONFIGS:
        runs.append("off-%s" % cfg)
        runs.append("legacy-%s" % cfg)
    runs.append("control-RT")
    runs.append("control-GI")
    return runs


def check_run_fixture(prov, completion, rundir, role, cfg,
                      trial_scene, expected_exe_abs):
    """Central immutable RunSpec check for ONE calibration run.

    Every role (trial/confirm/OFF/legacy/control) must satisfy the
    SAME fixture contract — validators may add role-specific mode
    checks but must never skip these or trust editable provenance
    for them:
    - prov declares EXACTLY FRAMES_REQUIRED (60); the window checks
      always run against the fixed constant, never a prov value.
    - frozen config/workload seal matches (check_workload).
    - scene path/hash equal the frozen trial fixture, and current bytes
      at that path re-hash to the expected digest.
    - executable path equals the expected authority binary.
    - completion complete; frame.png present with a mandatory matching
      recorded frame_sha256.
    - run.log has NGX and effective environment evidence; the retained
      native log proves submitted Off on measured frames 41..60.
    Returns '' or FAIL."""
    if prov.get("frames") != FRAMES_REQUIRED:
        return ("%s run %s declares frames=%r, contract requires %d"
                % (role, cfg, prov.get("frames"), FRAMES_REQUIRED))
    w_err = check_workload(prov, cfg)
    if w_err:
        return "%s run %s: %s" % (role, cfg, w_err)
    if not trial_scene.get("sha256") or \
            prov.get("scene_sha256") != trial_scene["sha256"]:
        return ("%s run %s scene differs from calibration fixture"
                % (role, cfg))
    scene_path = prov.get("scene", "")
    if not scene_path:
        return "%s run %s has no scene path" % (role, cfg)
    if (os.path.normcase(os.path.realpath(scene_path)) !=
            os.path.normcase(os.path.realpath(trial_scene.get("path", "")))):
        return "%s run %s scene path differs from frozen fixture" % (role, cfg)
    try:
        if sha256_file(scene_path) != trial_scene["sha256"]:
            return ("%s run %s scene file changed since calibration"
                    % (role, cfg))
    except OSError as e:
        return "%s run %s scene unreadable: %s" % (role, cfg, e)
    expected_env = trial_scene.get("environment")
    recorded_env = prov.get("environment")
    if recorded_env != expected_env:
        return "%s run %s environment differs from calibration fixture" % (role, cfg)
    if expected_env:
        try:
            if (scene_environment(scene_path, cfg, prov.get("extra", [])) !=
                    expected_env):
                return "%s run %s environment changed on disk" % (role, cfg)
        except (OSError, ValueError, TypeError) as e:
            return "%s run %s environment unreadable: %s" % (role, cfg, e)
    if os.path.abspath(prov.get("exe", "")) != \
            os.path.abspath(expected_exe_abs):
        return ("%s run %s exe is not the designated binary: %r"
                % (role, cfg, prov.get("exe", "")))
    if completion.get("complete") is not True:
        return "%s run %s incomplete" % (role, cfg)
    try:
        native_hash = sha256_file(os.path.join(rundir, "native-rt2.log"))
    except OSError as e:
        return "%s run %s native log unreadable: %s" % (role, cfg, e)
    if completion.get("native_log_sha256") != native_hash:
        return "%s run %s native log differs from retained evidence" % (role, cfg)
    frame_png = os.path.join(rundir, "frame.png")
    try:
        frame_hash = sha256_file(frame_png)
    except OSError as e:
        return "%s run %s frame unreadable: %s" % (role, cfg, e)
    if (not isinstance(completion.get("frame_sha256"), str) or
            completion["frame_sha256"] != frame_hash):
        return "%s run %s frame differs from retained evidence" \
            % (role, cfg)
    ngx, den, env = read_effective_mode(rundir)
    (_valid, _pilot, _control, _timings, log_problems,
     _ngx, _den, _env) = classify_log_lines(
         os.path.join(rundir, "run.log"))
    if log_problems:
        return "%s run %s log problem: %s" % (role, cfg, log_problems[0])
    if ngx is None or den is None:
        return "%s run %s lacks backend/mode evidence" % (role, cfg)
    if cfg in ("C2", "C4") and env is None:
        return "%s run %s lacks loaded-env evidence" % (role, cfg)
    if cfg == "C4" and not env.startswith("[Scene] Env map:"):
        return "%s run %s lacks effective saved-scene env/CDF" % (role, cfg)
    return ""


def validate_off_run(off_dir, ref_exe_abs, ref_hash, cfg, trial_scene_hash):
    """OFF-leg identity: central fixture contract plus true-OFF mode
    (diagnostic flags clear) and the reference executable.
    Returns '' or FAIL."""
    try:
        with open(os.path.join(off_dir, "completion.json")) as f:
            completion = json.load(f)
        with open(os.path.join(off_dir, "provenance.json")) as f:
            prov = json.load(f)
    except (OSError, ValueError) as e:
        return "unreadable OFF run %s: %s" % (off_dir, e)
    spec_err = check_run_fixture(prov, completion, off_dir, "OFF", cfg,
                                 trial_scene_hash, ref_exe_abs)
    if spec_err:
        return spec_err
    if prov.get("diag"):
        return "OFF run is diagnostic-ON, not OFF: %s" % off_dir
    if prov.get("control"):
        return "OFF run carries control flag: %s" % off_dir
    if prov.get("exe_sha256") != ref_hash:
        return "OFF run exe is not the reference: %s" % off_dir
    return ""


def validate_legacy_run(legacy_dir, authority, ref_hash,
                        cfg, trial_scene_hash):
    """Legacy-run identity: central fixture contract plus the EXPLICIT
    production authority sealed from the campaign input (same path AND
    same current file hash — provably the designated binary, provably
    not the reference) and its recorded SPV identity. Byte-identical
    binaries at other paths do not satisfy the designated-path
    requirement. Returns '' or FAIL."""
    try:
        with open(os.path.join(legacy_dir, "completion.json")) as f:
            completion = json.load(f)
        with open(os.path.join(legacy_dir, "provenance.json")) as f:
            prov = json.load(f)
    except (OSError, ValueError) as e:
        return "unreadable legacy run %s: %s" % (legacy_dir, e)
    spec_err = check_run_fixture(prov, completion, legacy_dir, "legacy",
                                 cfg, trial_scene_hash,
                                 authority.get("path", ""))
    if spec_err:
        return spec_err
    if prov.get("diag"):
        return "legacy run is diagnostic-ON: %s" % legacy_dir
    if prov.get("exe_sha256") != authority.get("sha256", ""):
        return "legacy run exe differs from designated production exe: %s" \
            % legacy_dir
    if prov.get("exe_sha256") == ref_hash:
        return "legacy run uses the reference exe, not production: %s" \
            % legacy_dir
    if prov.get("spvs", {}) != authority.get("spvs", {}):
        return "legacy run SPV identity differs from designated set: %s" \
            % legacy_dir
    return ""


def validate_control_run(control_dir, ref_exe_abs, ref_hash, cfg,
                         trial_scene_hash):
    """Control-run identity: central fixture contract plus diagnostic
    control mode and the exact measured mode-2 window (against the
    FIXED contract frame count, never an editable prov value). The
    histogram SHIFT itself is checked separately by
    check_control_response; this gate establishes the run IS a control
    run on the calibration fixture. Returns '' or FAIL."""
    try:
        with open(os.path.join(control_dir, "completion.json")) as f:
            completion = json.load(f)
        with open(os.path.join(control_dir, "provenance.json")) as f:
            prov = json.load(f)
    except (OSError, ValueError) as e:
        return "unreadable control run %s: %s" % (control_dir, e)
    spec_err = check_run_fixture(prov, completion, control_dir,
                                 "control", cfg, trial_scene_hash,
                                 ref_exe_abs)
    if spec_err:
        return spec_err
    if not prov.get("diag") or not prov.get("control"):
        return "control run lacks control mode flags: %s" % control_dir
    if prov.get("exe_sha256") != ref_hash:
        return "control run exe is not the reference: %s" % control_dir
    reval = argparse.Namespace(frames=FRAMES_REQUIRED,
                               diag=True, edges="manifest",
                               control=True, config=cfg,
                               extra_args=prov.get("extra", []))
    re_problems, _, _, n_control = validate_rundir(control_dir, reval)
    if re_problems:
        return ("control run re-validation %s: %s"
                % (control_dir, re_problems[:3]))
    if n_control == 0:
        return "control run has no mode-2 snapshots: %s" % control_dir
    return ""


def revalidate_freeze_lineage(manifest, ref_exe):
    """Prove the freeze came from reference-only calibration evidence.

    Reopens every lineage trial directory and re-verifies: completion +
    provenance file hashes (post-derive tampering with source records
    fails), pilot revalidation (window/mode/coverage/workload/env),
    scene file hash, and CURRENT reference identity (exe AND all six
    SPVs re-hashed now, not just the strings recorded at derive time).
    Finally re-derives Q/edges from the trial logs and requires
    equality with the manifest edges. A hand-authored manifest with
    plausible strings and a recomputed digest has no source evidence
    and fails here: a checksum detects changes, it does not establish
    provenance. Returns '' or FAIL."""
    seal_err = check_freeze_seal(manifest)
    if seal_err:
        return seal_err
    lineage = manifest["lineage"]
    try:
        ref_hash = sha256_file(ref_exe)
    except OSError as e:
        return "cannot hash reference exe: %s" % e
    if lineage.get("ref_exe_sha256") != ref_hash:
        return ("lineage reference exe differs from current --ref-exe "
                "(binary was rebuilt or swapped since calibration)")
    current_spvs = hash_exe_spvs(ref_exe)
    recorded_spvs = lineage.get("ref_spvs", {})
    for name, digest in current_spvs.items():
        if digest is None:
            return ("current reference SPV missing: %s" % name)
        if recorded_spvs.get(name) != digest:
            return ("reference SPV identity changed since calibration: "
                    "%s" % name)
    trials = lineage["trials"]
    if set(trials.keys()) != set(CONFIGS):
        return "lineage trials must name exactly C1..C4"
    pooled = [[0] * 34, [0] * 34]
    for cfg in CONFIGS:
        rec = trials[cfg]
        tdir = rec.get("dir", "")
        if not tdir or not os.path.isdir(tdir):
            return ("lineage trial dir missing: %s=%r" % (cfg, tdir))
        try:
            if (sha256_file(os.path.join(tdir, "completion.json")) !=
                    rec.get("completion_sha256")):
                return ("trial completion record changed since "
                        "derivation: %s" % cfg)
            if (sha256_file(os.path.join(tdir, "provenance.json")) !=
                    rec.get("provenance_sha256")):
                return ("trial provenance record changed since "
                        "derivation: %s" % cfg)
            with open(os.path.join(tdir, "completion.json")) as f:
                completion = json.load(f)
            with open(os.path.join(tdir, "provenance.json")) as f:
                prov = json.load(f)
        except (OSError, ValueError) as e:
            return "lineage trial unreadable %s: %s" % (cfg, e)
        if not completion.get("complete"):
            return "lineage trial no longer complete: %s" % cfg
        if prov.get("exe_sha256") != ref_hash:
            return "lineage trial exe mismatch: %s" % cfg
        if os.path.abspath(prov.get("exe", "")) != \
                os.path.abspath(ref_exe):
            return "lineage trial exe is not the reference binary: %s" % cfg
        if prov.get("frames") != FRAMES_REQUIRED:
            return ("lineage trial declares frames=%r, contract "
                    "requires %d: %s"
                    % (prov.get("frames"), FRAMES_REQUIRED, cfg))
        scene_path = rec.get("scene", "")
        if not scene_path or not prov.get("scene"):
            return "lineage trial has no scene path: %s" % cfg
        if (os.path.normcase(os.path.realpath(prov["scene"])) !=
                os.path.normcase(os.path.realpath(scene_path))):
            return "lineage trial scene path differs from freeze: %s" % cfg
        try:
            if sha256_file(scene_path) != rec.get("scene_sha256"):
                return ("lineage trial scene changed since derivation: "
                        "%s" % cfg)
        except OSError as e:
            return "lineage trial scene unreadable %s: %s" % (cfg, e)
        try:
            if (not isinstance(completion.get("frame_sha256"), str) or
                    completion["frame_sha256"] != sha256_file(
                        os.path.join(tdir, "frame.png"))):
                return "lineage trial frame differs from completion: %s" % cfg
        except OSError as e:
            return "lineage trial frame unreadable %s: %s" % (cfg, e)
        w_err = check_workload(prov, cfg)
        if w_err:
            return "lineage trial %s: %s" % (cfg, w_err)
        if not completion.get("ngx_effective"):
            return "lineage trial lacks backend evidence: %s" % cfg
        reval = argparse.Namespace(frames=FRAMES_REQUIRED,
                                   diag=True, edges="", control=False,
                                   config=cfg,
                                   extra_args=prov.get("extra", []))
        re_problems, _, n_pilot, _ = validate_rundir(tdir, reval)
        if re_problems:
            return ("lineage trial re-validation %s: %s"
                    % (cfg, re_problems[:3]))
        if n_pilot != rec.get("pilot_window_snapshots"):
            return ("lineage trial pilot count changed: %s" % cfg)
        try:
            with open(os.path.join(tdir, "run.log")) as f:
                lines = [l for l in f if "[ShadowTail]" in l]
        except OSError as e:
            return "lineage trial log unreadable %s: %s" % (cfg, e)
        for line in lines:
            try:
                obj = json.loads(line.split("[ShadowTail]", 1)[1])
            except ValueError:
                continue
            try:
                frame, slot, _, head, data = parse_snapshot_data(obj)
            except ValueError:
                continue
            if not 41 <= frame <= 60:
                continue
            if head[3] != 1 or head[4] != 0 or head[5] != 0:
                return "lineage trial has non-pilot data: %s" % cfg
            base = slot * (2 * 298 + 2)
            for fam in (0, 1):
                for i in range(34):
                    pooled[fam][i] += data[base + fam * 298 + i]
    scenes = {c: trials[c].get("scene_sha256", "") for c in CONFIGS}
    if scenes["C2"] != scenes["C3"] or not scenes["C2"]:
        return "lineage C2/C3 scene pairing broken"
    if len({scenes["C1"], scenes["C2"], scenes["C4"]}) != 3:
        return "lineage fixture relations broken"
    try:
        rederived = derive_freeze_edges(pooled)
    except ValueError as e:
        return "lineage re-derivation failed: %s" % e
    if (rederived["edgesRT"] != manifest["edgesRT"] or
            rederived["edgesGI"] != manifest["edgesGI"]):
        return ("re-derived edges differ from manifest: manifest was "
                "altered after calibration or trials changed")
    return ""

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
    extra = set(obj.keys()) - {"edgesRT", "edgesGI", "meta",
                               "workload", "lineage"}
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
    """Check §3b stratum minimums using each snapshot's OWN slot segment.

    Measured window ONLY (plan §9): strata observed solely in warmup
    frames 1..40 must not satisfy confirmation minimums."""
    failures = []
    need = MATRIX.get(config)
    if need is None:
        return ["unknown config %s" % config]
    rt_cells = [0] * 16
    gi_cells = [0] * 16
    for frame, slot, _, _, data in snapshots:
        if not 41 <= frame <= 60:
            continue
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

def default_run_fn(exe, cmd, cwd, logpath, timeout_min=0):
    """Blocking run with an optional hard timeout. On expiry the whole
    process tree is killed (taskkill /T on Windows, kill elsewhere)
    and 124 is returned — a captured timeout, never a silent hang."""
    timeout = timeout_min * 60 if timeout_min and timeout_min > 0 else None
    with open(logpath, "w") as log:
        if timeout is None:
            return subprocess.run(cmd, cwd=cwd, stdout=log,
                                  stderr=subprocess.STDOUT).returncode
        proc = subprocess.Popen(cmd, cwd=cwd, stdout=log,
                                stderr=subprocess.STDOUT)
        try:
            return proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            try:
                if os.name == "nt":
                    subprocess.run(
                        ["taskkill", "/PID", str(proc.pid), "/T", "/F"],
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL, timeout=30)
                else:
                    proc.kill()
            finally:
                proc.wait()
            try:
                log.write("\n[TIMEOUT] process killed after %.1f min\n"
                          % timeout_min)
                log.flush()
            except (OSError, ValueError):
                pass
            return 124


def resolve_launch_path(path):
    """Absolute launch path: the capture process runs with the exe
    directory as CWD, so repo-relative scene/edge/env arguments must be
    resolved against the INVOCATION directory before launch — passing
    them through verbatim points the binary at nonexistent files."""
    if os.path.isabs(path):
        return path
    return os.path.abspath(path)


def extract_env_path(extra_args):
    """Value of --env/-e in an extras list, or '' when absent."""
    for i, token in enumerate(extra_args):
        if token in ("--env", "-e") and i + 1 < len(extra_args):
            return extra_args[i + 1]
    return ""


def with_abs_env(extra_args, env_abs):
    """Extras with the --env/-e value replaced by its absolute path."""
    out = list(extra_args)
    for i, token in enumerate(out):
        if token in ("--env", "-e") and i + 1 < len(out):
            out[i + 1] = env_abs
    return out


def scene_environment(scene_abs, config, extra_args):
    """Resolve the environment the pinned standalone scene loader will use.

    An explicit C2 --env is absolute at launch. A C4 saved-scene reference
    resolves relative to the scene's parent (the no-project asset root),
    with its sidecar identity checked as the production path fallback does.
    C1/C3 must have no environment authority in the frozen workload.
    """
    explicit = extract_env_path(extra_args)
    if config == "C2":
        if not explicit:
            raise ValueError("C2 requires its explicit --env workload")
        with open(scene_abs, encoding="utf-8") as f:
            doc = json.load(f)
        ref = doc.get("envMap")
        if isinstance(ref, dict) and ref.get("path"):
            raise ValueError("C2 scene has unexpected saved environment")
        path = resolve_launch_path(explicit)
        kind, asset_id = "explicit", ""
    elif config == "C4":
        if explicit:
            raise ValueError("C4 environment must come from its saved scene")
        with open(scene_abs, encoding="utf-8") as f:
            doc = json.load(f)
        ref = doc.get("envMap")
        if not isinstance(ref, dict) or not ref.get("path"):
            raise ValueError("C4 scene has no saved environment reference")
        if doc.get("metadata", {}).get("projectId"):
            raise ValueError("C4 standalone scene unexpectedly project-bound")
        authored = ref["path"]
        path = os.path.realpath(authored if os.path.isabs(authored) else
                                os.path.join(os.path.dirname(scene_abs), authored))
        asset_id = ref.get("assetId", "")
        if asset_id and asset_id != "00000000-0000-0000-0000-000000000000":
            sidecar = path + ".rt2meta"
            with open(sidecar, encoding="utf-8") as f:
                sidecar_id = f.read().strip()
            if sidecar_id.lower() != asset_id.lower():
                raise ValueError("C4 environment sidecar ID conflicts with scene")
        kind = "saved-scene"
    else:
        if explicit:
            raise ValueError("%s must not request an environment" % config)
        if config in ("C1", "C3"):
            with open(scene_abs, encoding="utf-8") as f:
                doc = json.load(f)
            ref = doc.get("envMap")
            if isinstance(ref, dict) and ref.get("path"):
                raise ValueError("%s scene unexpectedly has environment" % config)
        return None
    if not os.path.isfile(path):
        raise ValueError("environment file missing: %s" % path)
    return {"kind": kind, "path": os.path.realpath(path),
            "sha256": sha256_file(path), "asset_id": asset_id}


def native_mode_evidence(native_path):
    """Bind submitted Off to each measured frame in the fresh RT_LOG file."""
    frames, problems, current = {}, [], None
    try:
        with open(native_path, encoding="utf-8", errors="replace") as f:
            for line in f:
                match = re.search(r"\[Render\] submitting frame (\d+) ", line)
                if match:
                    current = int(match.group(1))
                match = re.search(r"\[Denoiser\] submitted=([^\s]+) backend=([^\s]+)", line)
                if match:
                    if current is None or current in frames:
                        problems.append("native denoiser record unpaired/duplicate")
                    else:
                        frames[current] = (match.group(1), line.strip())
                    current = None
                if ("VUID-" in line or "[Validation] ERROR" in line or
                        "[UpdateDS] ERROR" in line):
                    problems.append("native validation/descriptor error: %s" % line.strip()[:160])
    except OSError as e:
        return None, ["missing/unreadable fresh native rt2_log.txt: %s" % e]
    bad = sorted(f for f, (mode, _) in frames.items() if mode != "Off")
    if bad:
        problems.append("native submitted denoiser is not Off on frames %s" % bad[:8])
    missing = sorted(set(range(41, 61)) - set(frames))
    if missing:
        problems.append("native submitted Off window incomplete, missing %s" % missing[:8])
    effective = frames[60][1] if 60 in frames else None
    return effective, problems


def read_effective_mode(rundir):
    """First [NGX] + [Denoiser] + [EnvMap] Loaded lines: effective
    backend/mode evidence (§10).

    Returns (ngx, denoiser, env_loaded), each a string or None when
    absent. Missing NGX/native Off evidence is a completion problem;
    missing effective environment evidence fails C2 and C4."""
    ngx, den, env, scene_adopted = None, None, None, False
    try:
        with open(os.path.join(rundir, "run.log")) as f:
            for line in f:
                if line.startswith("[OpenRt2Scene] adopting document"):
                    scene_adopted = True
                    env = None
                if line.startswith("[NGX]") and ngx is None:
                    ngx = line.strip()
                if scene_adopted and line.startswith("[Scene] Env map:"):
                    env = line.strip()
                elif line.startswith("[EnvMap] Loaded") and env is None:
                    env = line.strip()
                if (ngx is not None and den is not None and
                        env is not None):
                    break
    except OSError:
        pass
    den, _ = native_mode_evidence(os.path.join(rundir, "native-rt2.log"))
    return ngx, den, env


def run_capture(args, run_fn=None, timeout_min=0):
    """One 60-frame capture. run_fn injectable for CPU-only tests.

    run_fn receives (exe, cmd, cwd, logpath, timeout_min); the timeout
    is a hard remaining-budget kill, not a suggestion. Exit 124 means
    the budget killed the process: preserved logs, no manifest."""
    if run_fn is None:
        run_fn = default_run_fn
    # Own-directory absolutization: the capture process runs with the
    # exe directory as CWD, so the rundir (and everything under it,
    # including --output-display) must be absolute — a relative
    # --out-root would otherwise write the frame elsewhere and fail
    # validation against the requested target.
    rundir = os.path.abspath(os.path.join(args.out_root, args.name))
    if os.path.exists(rundir):
        print("ERROR: rundir exists (non-overwriting rule): %s" % rundir)
        return 1
    try:
        os.makedirs(rundir)
    except OSError as e:
        print("ERROR: cannot create rundir: %s" % e)
        return 1
    # Launch-path resolution (round-5 P0, round-6 P1): the capture
    # process runs with the exe directory as CWD, so the executable
    # itself plus every input file the binary opens (scene, edge
    # manifest, environment map) is resolved to an absolute path here.
    # A relative --exe would otherwise be reinterpreted under the exe
    # directory (nested bin/... lookup); cmd[0] must be absolute.
    exe_abs = resolve_launch_path(args.exe)
    scene_abs = resolve_launch_path(args.scene)
    edges_abs = resolve_launch_path(args.edges) if args.edges else ""
    env_req = extract_env_path(args.extra_args)
    env_abs = ""
    if env_req:
        env_abs = resolve_launch_path(env_req)
        if not os.path.exists(env_abs):
            print("FAIL missing environment file: %s (from %s)"
                  % (env_abs, env_req))
            return 1
    fixture_config = getattr(args, "fixture_config", args.config)
    if not fixture_config and env_req:
        fixture_config = "C2"
    try:
        env_authority = scene_environment(scene_abs, fixture_config,
                                           args.extra_args)
    except (OSError, ValueError, TypeError) as e:
        print("FAIL environment authority: %s" % e)
        return 1
    cmd = [exe_abs, "--headless", "--scene", scene_abs,
           "--output-display", os.path.join(rundir, "frame.png"),
           "--frames", str(args.frames), "--width", str(args.width),
           "--height", str(args.height), "--spp", str(args.spp),
           "--bounces", "8", "--seed", "0", "--denoiser-mode", "off"]
    cmd += with_abs_env(args.extra_args, env_abs) if env_abs else args.extra_args
    diag = bool(args.diag)
    if diag:
        cmd += ["--shadow-tail"]
        if edges_abs:
            # The RESOLVED manifest path goes on the command line: the
            # process CWD (exe dir) cannot see invocation-relative
            # paths. Hashing the absolute file alone is not enough.
            cmd += ["--shadow-tail-edges", edges_abs]
        if args.control:
            cmd += ["--shadow-tail-control"]
    exe_dir = os.path.dirname(exe_abs)
    err = write_text_checked(os.path.join(rundir, "command.txt"),
                             " ".join(cmd) + "\n")
    if err:
        print("ERROR: %s" % err)
        return 1
    run_id = uuid.uuid4().hex
    prov = {"run_id": run_id,
            "exe": exe_abs, "scene": scene_abs, "frames": args.frames,
            "width": args.width, "height": args.height, "spp": args.spp,
            "extra": args.extra_args, "diag": diag, "edges": edges_abs,
            "control": bool(args.control), "config": args.config,
            "fixture_config": fixture_config,
            # Workload seal: --benchmark-timings is harness
            # instrumentation (added by quartet/campaign on every leg),
            # never part of the frozen fixture workload.
            "workload": {"spp": args.spp, "width": args.width,
                         "height": args.height,
                         "extra": [a for a in args.extra_args
                                   if a != "--benchmark-timings"]}}
    try:
        prov["exe_sha256"] = sha256_file(exe_abs)
        prov["scene_sha256"] = sha256_file(scene_abs)
        if edges_abs:
            prov["edges_sha256"] = sha256_file(edges_abs)
        if env_abs:
            prov["env_abs"] = env_abs
            prov["env_sha256"] = sha256_file(env_abs)
        if env_authority:
            prov["environment"] = env_authority
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
    # RT_LOG writes rt2_log.txt under the executable CWD and opens it with
    # mode 'w' on first use. Preserve any old file, then require a fresh
    # process-created file; fake and real runs share this evidence boundary.
    native_source = os.path.join(exe_dir, "rt2_log.txt")
    native_copy = os.path.join(rundir, "native-rt2.log")
    lock_path = os.path.join(exe_dir, "rt2_shadow_tail_capture.lock")
    try:
        lock_fd = os.open(lock_path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except OSError as e:
        print("FAIL cannot own executable CWD native log: %s" % e)
        return 1
    try:
        with os.fdopen(lock_fd, "w") as lock:
            lock.write(run_id + "\n")
            lock.flush()
            os.fsync(lock.fileno())
        if os.path.lexists(native_source):
            os.replace(native_source,
                       os.path.join(rundir, "prior-native-rt2.log"))
        if os.path.lexists(native_source):
            raise OSError("native log exists before launch")
        rc = run_fn(exe_abs, cmd, exe_dir, os.path.join(rundir, "run.log"),
                    timeout_min)
        if not os.path.isfile(native_source):
            if rc == 124:
                native_digest = None
            else:
                raise OSError("process did not create rt2_log.txt")
        else:
            shutil.copyfile(native_source, native_copy)
            native_digest = sha256_file(native_source)
            if sha256_file(native_copy) != native_digest:
                raise OSError("native log copy hash differs from source")
    except OSError as e:
        print("FAIL native log ownership/persistence: %s" % e)
        return 1
    finally:
        os.remove(lock_path)
    err = write_text_checked(os.path.join(rundir, "exitcode.txt"), "%d\n" % rc)
    if err:
        print("ERROR: %s" % err)
        return 1
    if rc == 124:
        print("FAIL capture timeout: budget killed %s (preserved, "
              "no manifest)" % args.name)
        return 2
    if rc != 0:
        print("FAIL capture exit=%d (preserved, no manifest)" % rc)
        return 1
    # Hash reread: inputs hashed before execution must match after it
    # (replacement mid-run fails here, never silently) — including all
    # six recorded SPV hashes, not just exe/scene/edges.
    try:
        if sha256_file(exe_abs) != prov["exe_sha256"]:
            print("FAIL exe hash changed during run (tamper/race)")
            return 1
        if sha256_file(scene_abs) != prov["scene_sha256"]:
            print("FAIL scene hash changed during run (tamper/race)")
            return 1
        if edges_abs and sha256_file(edges_abs) != prov["edges_sha256"]:
            print("FAIL edges hash changed during run (tamper/race)")
            return 1
        if env_abs and sha256_file(env_abs) != prov["env_sha256"]:
            print("FAIL env hash changed during run (tamper/race)")
            return 1
        if env_authority and scene_environment(
                scene_abs, fixture_config, args.extra_args) != env_authority:
            print("FAIL environment authority changed during run")
            return 1
        for name, digest in prov["spvs"].items():
            if sha256_file(os.path.join(exe_dir, name)) != digest:
                print("FAIL SPV hash changed during run: %s" % name)
                return 1
        # Command lineage: the recorded launch line must still match
        # what this function built (no mid-run arg substitution).
        with open(os.path.join(rundir, "command.txt")) as f:
            if f.read() != " ".join(cmd) + "\n":
                print("FAIL command line changed during run (tamper/race)")
                return 1
    except (OSError, ValueError) as e:
        print("FAIL reread hash failed: %s" % e)
        return 1
    problems, n_valid, n_pilot, n_control = validate_rundir(rundir, args)
    ngx_line, denoiser_line, env_line = read_effective_mode(rundir)
    manifest = {"name": args.name, "run_id": run_id,
                "exit": rc, "problems": problems,
                "snapshots_valid": n_valid, "snapshots_pilot": n_pilot,
                "snapshots_control": n_control,
                "ngx_effective": ngx_line, "denoiser_effective": denoiser_line,
                 "env_effective": env_line,
                 "native_log_sha256": native_digest,
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
    print("DONE %s valid=%d pilot=%d control=%d"
          % (args.name, reread.get("snapshots_valid", 0),
             reread.get("snapshots_pilot", 0),
             reread.get("snapshots_control", 0)))
    return 0


def classify_log_lines(logpath):
    """Parse one run.log into window-agnostic classes.

    Returns (valid, pilot, control, timings, problems, ngx, denoiser,
    env_loaded): acceptance-grade valid snapshots, data-bearing
    pilot/control snapshots (header-distinguished), ALL [HeadlessTiming]
    records, structural problems, first [NGX]/[Denoiser] lines, and the
    first [EnvMap] Loaded line. Callers apply window/policy rules; this
    function only classifies, so every gate shares one parsing path."""
    valid, pilot, control, timings, problems = [], [], [], [], []
    ngx_line, denoiser_line, env_line = None, None, None
    scene_adopted = False
    try:
        with open(logpath) as f:
            for line in f:
                if line.startswith("[OpenRt2Scene] adopting document"):
                    scene_adopted = True
                    env_line = None
                if line.startswith("[NGX]") and ngx_line is None:
                    ngx_line = line.strip()
                if scene_adopted and line.startswith("[Scene] Env map:"):
                    env_line = line.strip()
                elif line.startswith("[EnvMap] Loaded") and env_line is None:
                    env_line = line.strip()
                if ("VUID-" in line or "[Validation] ERROR" in line or
                        "[EnvMap] Failed" in line):
                    problems.append("stdout/stderr validation or env error: %s"
                                    % line.strip()[:160])
                if "[HeadlessTiming]" in line:
                    try:
                        rec = json.loads(
                            line.split("[HeadlessTiming]", 1)[1])
                        timings.append(rec)
                    except ValueError:
                        problems.append("unparseable timing line")
                    continue
                if "[ShadowTail]" not in line:
                    continue
                try:
                    obj = json.loads(line.split("[ShadowTail]", 1)[1])
                except ValueError as e:
                    problems.append("unparseable snapshot line: %s" % e)
                    continue
                try:
                    valid.append(validate_snapshot_line(obj))
                    continue
                except ValueError:
                    pass
                # Non-acceptance but data-bearing: pilot (zero edges) or
                # control (mode 2), distinguished by header content.
                try:
                    frame, slot, serial, head, data = parse_snapshot_data(obj)
                    if head[4] == 0 and head[5] == 0:
                        pilot.append((frame, slot, serial, head, data))
                    elif head[3] == 2:
                        control.append((frame, slot, serial, head, data))
                    else:
                        problems.append(
                            "bad snapshot line: structurally sound but "
                            "neither acceptance, pilot nor control")
                except ValueError as e:
                    problems.append("bad snapshot line: %s" % e)
    except OSError as e:
        problems.append("unreadable run.log: %s" % e)
    denoiser_line, native_problems = native_mode_evidence(
        os.path.join(os.path.dirname(logpath), "native-rt2.log"))
    problems.extend(native_problems)
    return (valid, pilot, control, timings, problems, ngx_line,
            denoiser_line, env_line)


def window_only(records):
    """Measured-window filter (plan §9: frames 41..60 of a 60-frame run).

    Snapshots carry (frame, slot, serial, head, data); timing records
    carry a frame field. Warmup data is excluded, never pooled."""
    out = []
    for rec in records:
        frame = rec[0] if isinstance(rec, tuple) else rec.get("frame", -1)
        if 41 <= frame <= 60:
            out.append(rec)
    return out


def validate_rundir(rundir, args):
    """All completion rules, by run mode.

    Returns (problems, n_valid, n_pilot, n_control). problems empty =
    complete. Three disjoint policies (a snapshot belongs to exactly one
    class by its header: acceptance needs valid:true; pilot needs zero
    edge counts; control needs mode 2). EVERY diagnostic mode requires
    the EXACT measured window 41..60 in its own class — one snapshot
    can never complete a 60-frame run — plus unique (frame,slot) pairs
    and serial submission order:
    - acceptance (diag + edges, not control): valid covers 41..60; any
      pilot/control line inside the window is a wrong-mode emission.
    - pilot (diag, no edges): pilot covers 41..60; valid:true is
      impossible without edges (stale/misrouted success), control
      lines are wrong-mode emissions.
    - control (diag + edges + control flag): mode-2 covers 41..60;
      valid:true is a mode-1 leak, pilot lines are wrong-mode.
    OFF runs (not diag): exit/frame/image/effective-mode only (timing
    + parity handled by the quartet, coverage never applies).
    Effective backend evidence ([NGX] + [Denoiser] log lines) is
    required on EVERY run and recorded for the campaign gates.
    """
    problems = []
    frame_png = os.path.join(rundir, "frame.png")
    if not os.path.exists(frame_png):
        problems.append("missing frame.png")
    (snaps_valid, snaps_pilot, snaps_control, _timings, log_problems,
     ngx_line, denoiser_line, env_line) = classify_log_lines(
        os.path.join(rundir, "run.log"))
    problems.extend(log_problems)
    if ngx_line is None:
        problems.append("missing [NGX] effective-backend line in run.log")
    if denoiser_line is None:
        problems.append("missing submitted Off evidence in native-rt2.log")
    extras = getattr(args, "extra_args", [])
    fixture_config = getattr(args, "fixture_config", args.config)
    if ("--env" in extras or "-e" in extras or
            fixture_config == "C4") and env_line is None:
        problems.append("missing [EnvMap] Loaded line: workload requests "
                        "an environment map (not loaded or load failed)")
    if fixture_config == "C4" and not (
            env_line and env_line.startswith("[Scene] Env map:")):
        problems.append("C4 lacks effective saved-scene env/CDF evidence")
    seen = set()
    for frame, slot, _, _, _ in snaps_valid + snaps_pilot + snaps_control:
        if (frame, slot) in seen:
            problems.append("duplicate frame/slot (%d,%d)" % (frame, slot))
        seen.add((frame, slot))

    def check_window(snaps, label):
        # Exact measured window + serial submission order. Applies to
        # every diag mode: a 60-frame run with a missing measured frame
        # (or out-of-order serials) cannot certify anything.
        if args.frames != 60:
            return
        have_frames = set(f for f, _, _, _, _ in snaps)
        missing = [f for f in range(41, 61) if f not in have_frames]
        if missing:
            problems.append("%s window incomplete, missing %s"
                            % (label, missing[:8]))
        by_frame = {}
        for frame, _, serial, _, _ in snaps:
            by_frame.setdefault(frame, []).append(serial)
        prev, ordered_ok = -1, True
        for frame in sorted(by_frame):
            for serial in sorted(by_frame[frame]):
                if serial <= prev:
                    ordered_ok = False
                    break
                prev = serial
            if not ordered_ok:
                break
        if not ordered_ok:
            problems.append("%s serial order violation" % label)

    def window_frames(snaps):
        return set(f for f, _, _, _, _ in snaps
                   if 41 <= f <= 60)

    if args.diag:
        if args.control:
            if not snaps_control:
                problems.append("no mode-2 control snapshots produced")
            else:
                check_window(snaps_control, "control")
            if snaps_valid:
                problems.append(
                    "mode-1 leak in control run: %d valid snapshot(s) "
                    "with edges staged under --shadow-tail-control"
                    % len(snaps_valid))
            if window_frames(snaps_pilot):
                problems.append(
                    "pilot emission inside control window: %s"
                    % sorted(window_frames(snaps_pilot))[:8])
        elif args.edges:
            if not snaps_valid:
                problems.append("no valid snapshots with edges staged")
            else:
                check_window(snaps_valid, "measured")
            if window_frames(snaps_pilot):
                problems.append(
                    "pilot emission inside acceptance window: %s"
                    % sorted(window_frames(snaps_pilot))[:8])
            if window_frames(snaps_control):
                problems.append(
                    "control emission inside acceptance window: %s"
                    % sorted(window_frames(snaps_control))[:8])
        else:
            if not snaps_pilot and not snaps_valid:
                problems.append("no pilot snapshots produced")
            else:
                check_window(snaps_pilot, "pilot")
            if snaps_valid:
                problems.append(
                    "stale success in pilot run: %d valid snapshot(s) "
                    "without staged edges" % len(snaps_valid))
            if window_frames(snaps_control):
                problems.append(
                    "control emission inside pilot window: %s"
                    % sorted(window_frames(snaps_control))[:8])
    elif snaps_valid or snaps_pilot or snaps_control:
        problems.append("shadow-tail snapshot emitted in OFF run")
    if args.config and args.diag and not args.control:
        # Coverage gates diagnostic ON runs ONLY (never control runs:
        # their histograms carry the dependent-work shift by design)...
        cov = check_coverage(args.config, snaps_valid + snaps_pilot)
        problems.extend("coverage: " + c for c in cov)
    if args.config and args.diag and args.control:
        # ...while control runs check coverage on the same matrix
        # (sensitivity is meaningless on uncovered strata).
        cov = check_coverage(args.config, snaps_control)
        problems.extend("control coverage: " + c for c in cov)
    return (problems, len(snaps_valid), len(snaps_pilot),
            len(snaps_control))


def check_timing_window(sdir, regions):
    """Measured-window timing completeness: every region needs records
    on ALL unique frames 41..60 — one timing sample is not a window,
    and a duplicated frame is not two frames. Returns a list of
    problems (empty = complete window)."""
    (_v, _p, _c, timings, _prob, _ngx,
     _den, _env) = classify_log_lines(os.path.join(sdir, "run.log"))
    by_region = {}
    counts = {}
    for rec in window_only(timings):
        frame = rec.get("frame", -1)
        regs = rec.get("regions_ms", {})
        for region in regions:
            value = regs.get(region)
            if isinstance(value, (int, float)) and value > 0:
                by_region.setdefault(region, set()).add(frame)
                counts[region] = counts.get(region, 0) + 1
    want = set(range(41, 61))
    problems = []
    for region in regions:
        have = by_region.get(region, set())
        n = counts.get(region, 0)
        if have != want or n != 20:
            problems.append(
                "%s timing window %d/20 unique measured frames"
                % (region, len(have)))
    return problems


def check_off_parity(blockdir, on_name, off_name, config=""):
    """OFF-leg timing presence + ON/OFF image parity for one build.

    Returns (timing_result, image_result), each "complete" or a FAIL
    string. Both are hard gates: every leg needs the COMPLETE 20-frame
    measured window on every affected dispatch (RT always, GI Temporal
    on GI fixtures — a single timing record proves nothing), and the
    full PNG outputs must be byte-identical."""

    def regions():
        regs = ["RT Shading"]
        if GI_TIMING_REQUIRED.get(config, False):
            regs.append("ReSTIR GI Temporal")
        return regs

    timing, image = "complete", "complete"
    for sub in (on_name, off_name):
        probs = check_timing_window(os.path.join(blockdir, sub), regions())
        if probs:
            timing = "FAIL (timing window %s: %s)" % (sub, probs[0])
            break
    try:
        a = sha256_file(os.path.join(blockdir, on_name, "frame.png"))
        b = sha256_file(os.path.join(blockdir, off_name, "frame.png"))
        if a != b:
            image = "FAIL (ON/OFF image mismatch %s vs %s)" % (on_name, off_name)
    except OSError as e:
        image = "FAIL (image compare error: %s)" % e
    return timing, image


def _window_bracket(snaps, fam, edges):
    """Pooled measured-window p95 bracket over finite bins.

    Returns ((lo, hi), None) or (None, reason) — never raises."""
    n_edges = len(edges)
    pooled = [0] * 226
    for _, slot, _, _, data in window_only(snaps):
        base = slot * (2 * 298 + 2) + fam * 298
        for i in range(n_edges - 1):
            pooled[i] += data[base + 34 + i]
    n = sum(pooled)
    if n == 0:
        return None, "empty"
    rank = (n * 95 + 99) // 100
    acc = 0
    for i in range(n_edges - 1):
        acc += pooled[i]
        if rank <= acc:
            return (edges[i], edges[i + 1]), None
    return None, "unbounded"


def check_control_response(control_dir, ordinary_dir, config, edges):
    """Positive-control sensitivity gate (plan §6/§8).

    The control run (mode-2, dependent work inside the brackets) must
    show a REAL upward quantile shift against the matched ordinary ON
    run for the SAME config — identical histograms are an unresponsive
    control, never a pass. Requires, per required family: control
    coverage on the control snapshots, window minimums on both sides,
    an upward bracket shift (control lower >= ordinary lower AND
    control upper strictly above ordinary upper), a nonzero control
    checksum, and present blocked/visible outcomes on both sides.
    `edges` is the frozen (edgesRT, edgesGI) pair. Returns '' or FAIL.
    """
    req_fams = FAMILY_REQUIRED.get(config)
    if req_fams is None:
        return "FAIL (unknown config %s)" % config
    (c_valid, _c_pilot, c_ctl, _c_tim, c_prob, _c_ngx,
     _c_den, _c_env) = classify_log_lines(
        os.path.join(control_dir, "run.log"))
    (o_valid, _o_pilot, _o_ctl, _o_tim, o_prob, _o_ngx,
     _o_den, _o_env) = classify_log_lines(
        os.path.join(ordinary_dir, "run.log"))
    if c_prob:
        return "FAIL (control log: %s)" % c_prob[0]
    if o_prob:
        return "FAIL (ordinary log: %s)" % o_prob[0]
    if not c_ctl:
        return "FAIL (no mode-2 control snapshots)"
    cov = check_coverage(config, window_only(c_ctl))
    if cov:
        return "FAIL (control coverage: %s)" % cov[0]
    for fam, edge_list in ((0, edges[0]), (1, edges[1])):
        if fam not in req_fams:
            continue
        n_edges = len(edge_list)
        mass_c = sum_window_mass(c_ctl, fam, n_edges)
        mass_o = sum_window_mass(o_valid, fam, n_edges)
        if mass_c < FAMILY_MIN:
            return ("FAIL (control family %d N=%d < %d)"
                    % (fam, mass_c, FAMILY_MIN))
        if mass_o < FAMILY_MIN:
            return ("FAIL (ordinary family %d N=%d < %d)"
                    % (fam, mass_o, FAMILY_MIN))
        c_bracket, c_why = _window_bracket(c_ctl, fam, edge_list)
        o_bracket, o_why = _window_bracket(o_valid, fam, edge_list)
        if c_bracket is None or o_bracket is None:
            return ("FAIL (bracket invalid R:%s C:%s)"
                    % (o_why, c_why))
        (c_lo, c_hi), (o_lo, o_hi) = c_bracket, o_bracket
        if not (c_lo >= o_lo and c_hi > o_hi):
            return ("FAIL (no upward shift family %d: ordinary "
                    "[%d,%d) control [%d,%d))" % (fam, o_lo, o_hi, c_lo, c_hi))
    cks = sum_control_checksums(c_ctl)
    if cks <= 0:
        return "FAIL (control checksum zero: dependent work not consumed)"
    for tag, snaps in (("control", c_ctl), ("ordinary", o_valid)):
        blocked = visible = 0
        for _, slot, _, _, data in window_only(snaps):
            base = slot * (2 * 298 + 2)
            for fam in req_fams:
                blocked += data[base + fam * 298 + 262]
                visible += data[base + fam * 298 + 263]
        if blocked <= 0 or visible <= 0:
            return ("FAIL (%s outcomes absent: blocked=%d visible=%d)"
                    % (tag, blocked, visible))
    return ""


def sum_window_mass(snaps, fam, n_edges):
    """Finite-bin pooled mass over the measured window."""
    mass = 0
    for _, slot, _, _, data in window_only(snaps):
        base = slot * (2 * 298 + 2) + fam * 298
        mass += sum(data[base + 34 + i] for i in range(n_edges - 1))
    return mass


def sum_control_checksums(snaps):
    """Control checksum words (slot-segment +596/+597) over the window."""
    total = 0
    for _, slot, _, _, data in window_only(snaps):
        total += data[slot * (2 * 298 + 2) + 596]
        total += data[slot * (2 * 298 + 2) + 597]
    return total


def window_median_timings(sdir, region):
    """Sorted measured-window timing values for one region, or []."""
    vals = []
    (_v, _p, _c, timings, _prob, _ngx,
     _den, _env) = classify_log_lines(os.path.join(sdir, "run.log"))
    for rec in window_only(timings):
        v = rec.get("regions_ms", {}).get(region)
        if isinstance(v, (int, float)) and v > 0:
            vals.append(v)
    return sorted(vals)


def check_calib_headroom(on_dir, off_dir, config):
    """Per-config ON-vs-OFF observer check at fixed rate 1 (plan §8).

    Every affected dispatch (RT Shading everywhere, GI Temporal where
    GI runs) needs the COMPLETE 20-frame measured timing window on
    BOTH compared legs before any median is computed — a one-frame
    sample cannot certify a 3% ceiling. The ratio of window medians
    must satisfy medianON/medianOFF <= 1.03, else the configuration is
    invalid at rate 1. Missing/incomplete records for a REQUIRED
    region fail — never a silent exemption. Returns '' or FAIL."""
    import statistics
    for region in OBSERVER_REGIONS:
        required = (region == "RT Shading" or
                    (region == "ReSTIR GI Temporal" and
                     GI_TIMING_REQUIRED.get(config, False)))
        if required:
            for tag, sdir in (("ON", on_dir), ("OFF", off_dir)):
                window_probs = check_timing_window(sdir, [region])
                if window_probs:
                    return ("FAIL (%s %s %s leg: %s)"
                            % (config, region, tag, window_probs[0]))
        on = window_median_timings(on_dir, region)
        off = window_median_timings(off_dir, region)
        if not on or not off:
            if required:
                return ("FAIL (%s %s: missing dispatch timings "
                        "ON=%d OFF=%d)" % (config, region, len(on), len(off)))
            continue
        ratio = statistics.median(on) / statistics.median(off)
        if ratio > CALIB_HEADROOM:
            return ("FAIL (%s %s observer ratio %.4f > 1.03)"
                    % (config, region, ratio))
    return ""


def split_budget(remaining_min, stages_left):
    """Equal share of the ACTUAL remaining wall time per unfinished
    stage. The share never exceeds the remaining budget (no floor):
    flooring would allot processes more time than the cap holds and
    let work continue past the first-reached stop. Callers launch at
    most this share and skip the stage when it is not positive."""
    if stages_left <= 0:
        return 0.0
    return remaining_min / stages_left


def remaining_timeout(deadline, units_left):
    """Launch-gate timeout for capped orchestration.

    Returns None when deadline is None (standalone run: explicitly
    uncapped, no budget applies); "SKIP" when no budget remains
    (zero or negative — this must NEVER reach the uncapped execution
    mode, so the stage is marked INCOMPLETE without spawning);
    otherwise a strictly positive equal share. Callers must check for
    "SKIP" before spawning any process."""
    if deadline is None:
        return None
    if units_left <= 0:
        return "SKIP"
    remaining = (deadline - time.monotonic()) / 60
    if remaining <= 0:
        return "SKIP"
    return remaining / units_left


def stage_result(rc):
    """Map a capture/quartet return code to a manifest status: 0 is
    complete; 2 (budget timeout / deadline expiry) is INCOMPLETE — the
    gate is undecided at the cap, never a failure verdict; anything
    else is FAIL."""
    if rc == 0:
        return "complete"
    if rc == 2:
        return "INCOMPLETE (timeout)"
    return "FAIL"


def exe_authority(exe_path):
    """Executable authority record: absolute path + current file hash
    + SPV set beside it. Authorities identify binaries by ROLE
    (declared path), never by demanding numerical hash differences —
    two distinct supplied binaries may be byte-identical."""
    exe_abs = os.path.abspath(exe_path)
    return {"path": exe_abs,
            "sha256": sha256_file(exe_abs),
            "spvs": hash_exe_spvs(exe_abs)}


def campaign_authority_error(authorities):
    """Require both roles to retain their launch-time exe/SPV bytes."""
    for role in ("ref", "cand"):
        recorded = authorities.get(role, {})
        if not recorded.get("path"):
            return "%s campaign authority missing" % role
        try:
            current = exe_authority(recorded["path"])
        except OSError as e:
            return "%s campaign authority unreadable: %s" % (role, e)
        if current != recorded:
            return "%s campaign executable/SPV authority changed" % role
    return ""


def distinct_role_paths(ref_exe, cand_exe):
    return (os.path.normcase(os.path.realpath(ref_exe)) !=
            os.path.normcase(os.path.realpath(cand_exe)))


def check_leg_executable(prov, authority, role):
    """Bind one leg's provenance to its role authority: path must equal
    the declared binary AND the recorded hash/SPVs must match a fresh
    reread (mid-run substitution fails). Returns '' or FAIL."""
    if os.path.abspath(prov.get("exe", "")) != authority["path"]:
        return ("%s leg exe %r is not the designated %s binary %r"
                % (role, prov.get("exe", ""), role, authority["path"]))
    try:
        if sha256_file(authority["path"]) != authority["sha256"]:
            return ("%s authority binary changed on disk: %s"
                    % (role, authority["path"]))
    except OSError as e:
        return "%s authority binary unreadable: %s" % (role, e)
    if prov.get("exe_sha256") != authority["sha256"]:
        return "%s leg exe hash differs from designated binary" % role
    if prov.get("spvs", {}) != authority["spvs"]:
        return "%s leg SPV set differs from designated set" % role
    if hash_exe_spvs(authority["path"]) != authority["spvs"]:
        return "%s authority SPV set changed on disk" % role
    return ""


def check_block_uniformity(leg_provs):
    """Block-wide fixture uniformity: all four legs share scene hash,
    workload, and frame count; ON legs carry their config with
    diagnostic mode, OFF legs carry no shadow config. `leg_provs` maps
    sub-name to provenance. Returns '' or FAIL."""
    scenes = {p.get("scene_sha256", "") for p in leg_provs.values()}
    if len(scenes) != 1 or not next(iter(scenes)):
        return "block legs span different scenes: %s" % sorted(scenes)
    workloads = {json.dumps(p.get("workload", {}), sort_keys=True)
                 for p in leg_provs.values()}
    if len(workloads) != 1:
        return "block legs span different workloads"
    frames = {p.get("frames", -1) for p in leg_provs.values()}
    if frames != {FRAMES_REQUIRED}:
        return "block legs declare frames=%s, contract requires %d" \
            % (sorted(frames), FRAMES_REQUIRED)
    for sub, prov in leg_provs.items():
        want_diag = sub.endswith("_ON")
        if bool(prov.get("diag")) != want_diag:
            return "block leg %s has wrong diagnostic mode" % sub
        if not want_diag and prov.get("config", ""):
            return "block leg %s OFF leg carries shadow config" % sub
        if prov.get("control") and sub != "C_ON":
            return "block leg %s carries unexpected control flag" % sub
    on_cfgs = {leg_provs[s].get("config", "") for s in ("R_ON", "C_ON")}
    if len(on_cfgs) != 1:
        return "block ON legs span different configs: %s" % sorted(on_cfgs)
    return ""


def run_quartet(args, run_fn=None, deadline_min=0):
    """One block = R_ON/R_OFF/C_ON/C_OFF with block manifest + deadline."""
    blockdir = os.path.abspath(os.path.join(args.out_root, args.name))
    if not distinct_role_paths(args.ref_exe, args.cand_exe):
        print("ERROR: quartet requires distinct reference/candidate paths")
        return 1
    if os.path.exists(blockdir):
        print("ERROR: blockdir exists: %s" % blockdir)
        return 1
    try:
        os.makedirs(blockdir)
    except OSError as e:
        print("ERROR: cannot create blockdir: %s" % e)
        return 1
    try:
        ref_auth = exe_authority(args.ref_exe)
        cand_auth = exe_authority(args.cand_exe)
    except OSError as e:
        print("ERROR: cannot read designated binary: %s" % e)
        return 1
    order = [("R_ON", args.ref_exe, True, args.edges, False),
             ("R_OFF", args.ref_exe, False, "", False),
             ("C_ON", args.cand_exe, True, args.edges, args.control),
             ("C_OFF", args.cand_exe, False, "", False)]
    deadline = time.monotonic() + deadline_min * 60 if deadline_min > 0 else None
    results = {}
    legs = list(order)
    for idx, (sub, exe, diag, edges, control) in enumerate(legs):
        if deadline is not None and time.monotonic() >= deadline:
            results[sub] = "INCOMPLETE (deadline)"
            continue
        extra = list(args.extra_args)
        # Every leg carries benchmark timings: ON legs need them for the
        # observer-bound analysis, OFF legs for timing parity. (Coverage
        # gates ON runs only — see validate_rundir.)
        extra = extra + ["--benchmark-timings"]
        sub_args = argparse.Namespace(
            exe=exe, scene=args.scene, out_root=blockdir, name=sub,
            frames=args.frames, spp=args.spp, width=args.width,
            height=args.height, edges=edges, diag=diag, control=control,
            config=args.config if diag else "",
            fixture_config=args.config, extra_args=extra)
        # Hard per-leg timeout via the launch gate: a "SKIP" means
        # zero/negative budget remains, so the leg is marked
        # INCOMPLETE WITHOUT spawning (spawning with timeout 0 would
        # run uncapped in default_run_fn).
        leg_timeout = remaining_timeout(deadline, len(legs) - idx)
        if leg_timeout == "SKIP":
            results[sub] = "INCOMPLETE (deadline)"
            continue
        rc = run_capture(sub_args, run_fn=run_fn, timeout_min=leg_timeout)
        results[sub] = stage_result(rc)
        if rc != 0:
            break
    # OFF-leg timing parity + ON/OFF image parity (per build). Every
    # leg needs the full measured timing window; ON/OFF frame images
    # must be byte-identical (instrumentation must not change the
    # image). The quartet config selects the GI requirement.
    if all(results.get(k) == "complete" for k in ("R_ON", "R_OFF")):
        results["R_timing_parity"], results["R_image_parity"] = \
            check_off_parity(blockdir, "R_ON", "R_OFF", args.config)
    if all(results.get(k) == "complete" for k in ("C_ON", "C_OFF")):
        results["C_timing_parity"], results["C_image_parity"] = \
            check_off_parity(blockdir, "C_ON", "C_OFF", args.config)
    if all(results.get(k) == "complete" for k in
           ("R_ON", "R_OFF", "C_ON", "C_OFF")):
        try:
            provs = {}
            for sub in ("R_ON", "R_OFF", "C_ON", "C_OFF"):
                with open(os.path.join(blockdir, sub, "provenance.json")) as f:
                    provs[sub] = json.load(f)
            uniformity = check_block_uniformity(provs)
            results["uniformity"] = ("FAIL (%s)" % uniformity if uniformity
                                     else "complete")
            errors = ["%s: %s" % (sub, err)
                      for sub, auth, role in
                      (("R_ON", ref_auth, "R"), ("R_OFF", ref_auth, "R"),
                       ("C_ON", cand_auth, "C"), ("C_OFF", cand_auth, "C"))
                      for err in [check_leg_executable(provs[sub], auth, role)]
                      if err]
            results["executable_identity"] = (
                "FAIL (%s)" % errors[0] if errors else "complete")
        except (OSError, ValueError) as e:
            results["uniformity"] = "FAIL (provenance unreadable: %s)" % e
            results["executable_identity"] = "FAIL (provenance unreadable)"
    manifest = {"block": args.name, "order": [o[0] for o in order],
                "results": results,
                "authorities": {"ref": ref_auth, "cand": cand_auth},
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


# PAIRED GOLDENS: generated by the compiled C++ SnapshotJson path
# (RT2Tests --test-case="*golden*" with ST_WRITE_GOLDENS=1) and consumed
# here — never hand-written. If the C++ goldens change, regenerate and
# re-verify in the same commit. Paths resolve relative to this script
# (repo root); a missing file fails loudly, never silently.
def _load_golden(name):
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "RT2Tests", "src", name)
    with open(path) as f:
        return f.read()


TEST_GOLDEN_PILOT_JSON = None
TEST_GOLDEN_CONTROL_JSON = None


def golden_jsons():
    global TEST_GOLDEN_PILOT_JSON, TEST_GOLDEN_CONTROL_JSON
    if TEST_GOLDEN_PILOT_JSON is None:
        TEST_GOLDEN_PILOT_JSON = _load_golden("shadowtail_golden_pilot.json")
        TEST_GOLDEN_CONTROL_JSON = _load_golden(
            "shadowtail_golden_control.json")
    return TEST_GOLDEN_PILOT_JSON, TEST_GOLDEN_CONTROL_JSON


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
    if len(args.block_dirs) != 4:
        print("INCONCLUSIVE analyze: need exactly 4 blocks, got %d"
              % len(args.block_dirs))
        return 2
    block_paths = [os.path.normcase(os.path.realpath(p))
                   for p in args.block_dirs]
    if len(set(block_paths)) != 4:
        print("FAIL analyze: block directories alias/reuse one run block")
        return 1
    expected_ids = {"block-%s-%d" % (args.config, k)
                    for k in range(1, 5)}
    seen_ids, seen_legs, seen_run_ids = set(), set(), set()
    req_fams = FAMILY_REQUIRED[args.config]
    try:
        manifest_hash = sha256_file(args.manifest)
    except OSError as e:
        print("FAIL analyze: cannot hash manifest: %s" % e)
        return 1
    # Sealed freeze only: a hand-authored edge file without the
    # reference-only lineage + workload seal cannot unlock acceptance.
    seal_err = check_freeze_seal(manifest, manifest_hash)
    if seal_err:
        print("FAIL analyze: %s" % seal_err)
        return 1
    if not args.ref_exe:
        print("FAIL analyze: --ref-exe required (lineage revalidation)")
        return 1
    lineage_err = revalidate_freeze_lineage(manifest, args.ref_exe)
    if lineage_err:
        print("FAIL analyze: %s" % lineage_err)
        return 1
    # Acceptance recomputes the calibration behind the seal: confirms,
    # OFF/legacy parity, headroom and control responses are re-run
    # from retained evidence. A trials-only freeze fails here.
    calib_err = revalidate_calibration(manifest, args.manifest,
                                       args.ref_exe)
    if calib_err:
        print("FAIL analyze: %s" % calib_err)
        return 1
    blocks = []
    on_identities, off_identities = set(), set()
    # Claimed acceptance authorities from EXPLICIT inputs (rebuilt
    # NOW): blocks must bind to these, never to dir labels or
    # per-leg self-assertion alone.
    if not args.cand_exe:
        print("FAIL analyze: --cand-exe required (authority binding)")
        return 1
    if not distinct_role_paths(args.ref_exe, args.cand_exe):
        print("FAIL analyze: reference and candidate resolve to one path")
        return 1
    try:
        claim_ref = exe_authority(args.ref_exe)
        claim_cand = exe_authority(args.cand_exe)
    except OSError as e:
        print("FAIL analyze: cannot hash claimed exe: %s" % e)
        return 1
    parents = {os.path.normcase(os.path.realpath(os.path.dirname(p)))
               for p in args.block_dirs}
    authority_file = getattr(args, "authority_file", "")
    campaign_dir = next(iter(parents)) if len(parents) == 1 else ""
    if not authority_file and campaign_dir:
        candidate_file = os.path.join(campaign_dir,
                                      "accept-authorities.json")
        if os.path.exists(candidate_file):
            authority_file = candidate_file
    campaign_record = (os.path.join(campaign_dir, "campaign.json")
                       if campaign_dir else "")
    recorded_campaign = None
    if campaign_record and os.path.exists(campaign_record):
        try:
            with open(campaign_record) as f:
                recorded_campaign = json.load(f)
        except (OSError, ValueError) as e:
            print("FAIL analyze: campaign record unreadable: %s" % e)
            return 1
    if (getattr(args, "campaign_authorities", None) is not None or
            (recorded_campaign and recorded_campaign.get("mode") == "accept")) \
            and not authority_file:
        print("FAIL analyze: acceptance campaign authority file missing")
        return 1
    if authority_file:
        try:
            with open(authority_file) as f:
                pinned = json.load(f)
            pin_hash = sha256_file(authority_file)
        except (OSError, ValueError) as e:
            print("FAIL analyze: campaign authority unreadable: %s" % e)
            return 1
        if (pinned != {"ref": claim_ref, "cand": claim_cand} or
                (getattr(args, "campaign_authorities", None) is not None and
                 pinned != args.campaign_authorities) or
                campaign_authority_error(pinned)):
            print("FAIL analyze: campaign authority differs from frozen "
                  "reference/candidate binaries")
            return 1
        if (recorded_campaign and recorded_campaign.get("mode") == "accept"
                and (recorded_campaign.get("authorities") != pinned or
                     recorded_campaign.get("authority_sha256") != pin_hash)):
            print("FAIL analyze: campaign record authority differs")
            return 1
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
        block_id = block.get("block")
        if block_id not in expected_ids or block_id in seen_ids:
            print("FAIL analyze: duplicate or unexpected block identity %r"
                  % block_id)
            return 1
        seen_ids.add(block_id)
        for k in ("R_image_parity", "C_image_parity",
                  "R_timing_parity", "C_timing_parity",
                  "uniformity", "executable_identity"):
            if block.get("results", {}).get(k) != "complete":
                print("FAIL analyze: parity gate %s not complete in %s"
                      % (k, bdir))
                return 1
        subs = {}
        provs = {}
        for sub in ("R_ON", "R_OFF", "C_ON", "C_OFF"):
            sdir = os.path.join(bdir, sub)
            leg_path = os.path.normcase(os.path.realpath(sdir))
            if leg_path in seen_legs:
                print("FAIL analyze: reused/aliased leg directory: %s" % sdir)
                return 1
            seen_legs.add(leg_path)
            try:
                with open(os.path.join(sdir, "completion.json")) as f:
                    completion = json.load(f)
                with open(os.path.join(sdir, "provenance.json")) as f:
                    prov = json.load(f)
            except (OSError, ValueError) as e:
                print("FAIL analyze: unreadable run %s: %s" % (sdir, e))
                return 1
            if not completion.get("complete"):
                print("FAIL analyze: run incomplete: %s" % sdir)
                return 1
            try:
                if (not completion.get("native_log_sha256") or
                        sha256_file(os.path.join(sdir, "native-rt2.log")) !=
                        completion["native_log_sha256"]):
                    print("FAIL analyze: native log evidence changed: %s" % sdir)
                    return 1
            except OSError as e:
                print("FAIL analyze: native log unreadable %s: %s" % (sdir, e))
                return 1
            run_id = prov.get("run_id")
            if (not isinstance(run_id, str) or not run_id or
                    completion.get("run_id") != run_id or
                    run_id in seen_run_ids):
                print("FAIL analyze: missing, mismatched or reused capture "
                      "identity: %s" % sdir)
                return 1
            seen_run_ids.add(run_id)
            # The actual capture command contains its owned output path.
            # A copied leg directory with edited block labels is not a
            # distinct process run, even when its frame bytes match.
            try:
                with open(os.path.join(sdir, "command.txt")) as f:
                    command = f.read()
            except OSError as e:
                print("FAIL analyze: run command unreadable %s: %s"
                      % (sdir, e))
                return 1
            output_marker = ("--output-display " +
                             os.path.join(os.path.abspath(sdir),
                                          "frame.png") + " --frames 60")
            if command.count(output_marker) != 1:
                print("FAIL analyze: run command output identity differs: %s"
                      % sdir)
                return 1
            # Fixture/settings identity: every analyzed run must be the
            # same 60-frame scene/config/workload with the frozen edge
            # manifest. A mixed-identity quartet cannot certify anything.
            if prov.get("frames") != 60 or not prov.get("scene_sha256"):
                print("FAIL analyze: run identity incomplete: %s" % sdir)
                return 1
            wl = WORKLOADS[args.config]
            if (prov.get("spp") != wl["spp"] or
                    prov.get("width") != wl["width"] or
                    prov.get("height") != wl["height"] or
                    prov.get("extra") != wl["extra"] +
                    ["--benchmark-timings"] or prov.get("control") is not False):
                print("FAIL analyze: capture settings differ from frozen "
                      "workload: %s" % sdir)
                return 1
            try:
                if completion.get("frame_sha256") != sha256_file(
                        os.path.join(sdir, "frame.png")):
                    print("FAIL analyze: frame changed since completion: %s"
                          % sdir)
                    return 1
            except OSError as e:
                print("FAIL analyze: frame unreadable: %s: %s" % (sdir, e))
                return 1
            fixture = manifest.get("lineage", {}).get("trials", {}).get(
                args.config, {})
            expected_scene = fixture.get("scene_sha256")
            if prov.get("scene_sha256") != expected_scene:
                print("FAIL analyze: run scene differs from frozen fixture: %s"
                      % sdir)
                return 1
            if (os.path.normcase(os.path.realpath(prov.get("scene", ""))) !=
                    os.path.normcase(os.path.realpath(
                        fixture.get("scene", "")))):
                print("FAIL analyze: run scene path differs from frozen "
                      "fixture: %s" % sdir)
                return 1
            try:
                if sha256_file(prov["scene"]) != expected_scene:
                    print("FAIL analyze: run scene changed on disk: %s" % sdir)
                    return 1
            except (OSError, KeyError) as e:
                print("FAIL analyze: run scene unreadable: %s: %s" % (sdir, e))
                return 1
            w_err = check_workload(prov, args.config)
            if sub.endswith("_ON") and w_err:
                print("FAIL analyze: %s: %s" % (sdir, w_err))
                return 1
            if not sub.endswith("_ON"):
                off_workload = dict(prov, config=args.config)
                w_err = check_workload(off_workload, args.config)
                if w_err or prov.get("config", ""):
                    print("FAIL analyze: OFF workload/config mismatch: %s: %s"
                          % (sdir, w_err))
                    return 1
            if args.config in ("C2", "C4"):
                try:
                    frozen_env = fixture.get("environment")
                    current_env = scene_environment(
                        prov["scene"], args.config,
                        WORKLOADS[args.config]["extra"])
                    if (not frozen_env or
                            prov.get("environment") != frozen_env or
                            current_env != frozen_env):
                        print("FAIL analyze: environment identity mismatch: %s"
                              % sdir)
                        return 1
                except (OSError, ValueError, KeyError, TypeError) as e:
                    print("FAIL analyze: environment unreadable: %s: %s"
                          % (sdir, e))
                    return 1
            elif prov.get("environment") is not None:
                print("FAIL analyze: unexpected environment in %s" % sdir)
                return 1
            expected_on = sub.endswith("_ON")
            run_args = argparse.Namespace(
                frames=60, diag=expected_on, control=False,
                edges=args.manifest if expected_on else "",
                config=args.config if expected_on else "",
                fixture_config=args.config,
                extra_args=WORKLOADS[args.config]["extra"])
            run_problems, _, _, _ = validate_rundir(sdir, run_args)
            if run_problems:
                print("FAIL analyze: run validation %s: %s"
                      % (sdir, run_problems[0]))
                return 1
            # ON legs bind the full shadow identity (fixture + workload
            # + frozen edges); OFF legs bind fixture + workload only —
            # they carry no shadow config by design (quartet passes
            # config="" there), but must still be the same fixture.
            if sub.endswith("_ON"):
                on_identities.add(
                    (prov.get("scene_sha256"), prov.get("frames"),
                     prov.get("config", ""),
                     json.dumps(prov.get("workload", {}), sort_keys=True),
                     prov.get("edges_sha256", "")))
            else:
                off_identities.add(
                    (prov.get("scene_sha256"), prov.get("frames"),
                     json.dumps(prov.get("workload", {}), sort_keys=True)))
            if sub.endswith("_ON"):
                if prov.get("edges_sha256") != manifest_hash:
                    print("FAIL analyze: edge manifest mismatch: %s" % sdir)
                    return 1
                if not completion.get("ngx_effective"):
                    print("FAIL analyze: no effective-backend evidence: %s"
                          % sdir)
                    return 1
                # Environment-gated fixtures (C2) must prove the map
                # actually loaded — NGX/Denoiser lines alone are not
                # sufficient evidence for an env-lit scene.
                if (args.config in ("C2", "C4") and
                        not completion.get("env_effective")):
                    print("FAIL analyze: no loaded-env evidence: %s" % sdir)
                    return 1
            (snaps, _pilot, _ctl, timings, log_prob, _ngx,
             _den, _env) = classify_log_lines(
                os.path.join(sdir, "run.log"))
            if log_prob:
                print("FAIL analyze %s: %s" % (sdir, log_prob[0]))
                return 1
            # Acceptance analysis consumes valid snapshots only; pilot /
            # control lines in an acceptance run are a wrong-mode leak
            # that completion already rejects — their presence here
            # means the completion gate was bypassed, so fail.
            if _pilot or _ctl:
                print("FAIL analyze: non-acceptance snapshots in %s "
                      "(completion bypass?)" % sdir)
                return 1
            if sub.endswith("_ON"):
                snaps = window_only(snaps)
                if not snaps:
                    print("FAIL analyze: no measured-window snapshots: %s"
                          % sdir)
                    return 1
            subs[sub] = (snaps, {"frames": window_only(timings)})
            provs[sub] = prov
        # Block authority verification: the persisted block record must
        # bind to the claimed ref/cand authorities, and every leg must
        # verifiably run its role binary with a uniform fixture. A
        # swapped R/C, single-binary, wrong-hash/SPV or mixed block
        # fails here with the responsible leg named.
        recorded = block.get("authorities", {})
        if recorded.get("ref") != claim_ref:
            print("FAIL analyze: block ref authority differs from "
                  "--ref-exe in %s" % bdir)
            return 1
        if recorded.get("cand") != claim_cand:
            print("FAIL analyze: block cand authority differs from "
                  "--cand-exe in %s" % bdir)
            return 1
        uni_err = check_block_uniformity(provs)
        if uni_err:
            print("FAIL analyze %s: %s" % (bdir, uni_err))
            return 1
        for sub, role in (("R_ON", "R"), ("R_OFF", "R"),
                          ("C_ON", "C"), ("C_OFF", "C")):
            auth = recorded["ref"] if role == "R" else recorded["cand"]
            exe_err = check_leg_executable(provs[sub], auth, role)
            if exe_err:
                print("FAIL analyze %s %s: %s" % (bdir, sub, exe_err))
                return 1
        for on, off, label in (("R_ON", "R_OFF", "R"),
                               ("C_ON", "C_OFF", "C")):
            timing, image = check_off_parity(bdir, on, off, args.config)
            if timing != "complete":
                print("INCONCLUSIVE analyze: %s measured timing incomplete: %s"
                      % (label, timing))
                return 2
            if image != "complete":
                print("FAIL analyze: %s persisted image parity changed: %s"
                      % (label, image))
                return 1
        blocks.append(subs)
    if len(on_identities) != 1:
        print("FAIL analyze: mixed ON identity across blocks: %s"
              % sorted(on_identities))
        return 1
    if len(off_identities) != 1:
        print("FAIL analyze: mixed OFF identity across blocks: %s"
              % sorted(off_identities))
        return 1
    if (seen_ids != expected_ids or len(seen_legs) != 16 or
            len(seen_run_ids) != 16):
        print("FAIL analyze: four distinct complete block identities required")
        return 1
    failures, inconclusive = [], []

    for fam, edges in ((0, edgesRT), (1, edgesGI)):
        if fam not in req_fams:
            continue
        # Endpoint-conservative block log-ratios: candidate UPPER over
        # reference LOWER, per block; paired-t bound over the 4 blocks.
        # Brackets come from the shared measured-window helper: warmup
        # never pools, empty/unbounded is typed INCONCLUSIVE.
        xs = []
        bad = False
        for bi, b in enumerate(blocks):
            # Per-block minimums before inference (§8): a thin block
            # cannot certify a tail (window mass only — warmup excluded).
            for tag in ("R_ON", "C_ON"):
                mass = sum_window_mass(b[tag][0], fam, len(edges))
                if mass < FAMILY_MIN:
                    inconclusive.append(
                        "family %d block %d %s N=%d < %d"
                        % (fam, bi, tag, mass, FAMILY_MIN))
                    bad = True
            if bad:
                break
            r_bracket, r_why = _window_bracket(b["R_ON"][0], fam, edges)
            c_bracket, c_why = _window_bracket(b["C_ON"][0], fam, edges)
            if r_bracket is None or c_bracket is None:
                inconclusive.append(
                    "family %d block %d bracket invalid (R:%s C:%s)"
                    % (fam, bi, r_why, c_why))
                bad = True
                break
            (rslow, _), (cslow, csup) = r_bracket, c_bracket
            if rslow <= 0 or cslow <= 0:
                inconclusive.append(
                    "family %d block %d zero lower edge" % (fam, bi))
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

    # Observer bounds per build per affected dispatch (plan §7: RT
    # shading everywhere, GI temporal wherever GI runs). Missing
    # records for a REQUIRED region on any paired block fail the gate
    # as INCONCLUSIVE — a GI run without GI timings proves nothing
    # about the GI dispatch, and lack of records is never an exemption.
    for build in ("R", "C"):
        for region in OBSERVER_REGIONS:
            required = (region == "RT Shading" or
                        (region == "ReSTIR GI Temporal" and
                         GI_TIMING_REQUIRED.get(args.config, False)))
            ys = []
            bad = False
            for bi, b in enumerate(blocks):
                on = [f["regions_ms"].get(region) for f in b[build + "_ON"][1].get("frames", [])]
                off = [f["regions_ms"].get(region) for f in b[build + "_OFF"][1].get("frames", [])]
                on = sorted(v for v in on if isinstance(v, (int, float)) and v > 0)
                off = sorted(v for v in off if isinstance(v, (int, float)) and v > 0)
                if not on or not off:
                    if required:
                        inconclusive.append(
                            "%s %s: missing dispatch timings on block "
                            "%d (ON=%d OFF=%d)" % (build, region, bi,
                                                   len(on), len(off)))
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
The `campaign` subcommand executes these tables with the elapsed caps,
freeze gating and legacy parity enforced (never implicit runs).
"""


CALIB_CAP_MIN = 90
ACCEPT_CAP_MIN = 150
CONFIGS = ("C1", "C2", "C3", "C4")
# Fixed contract frame count (plan §9: 60-frame runs, frames 41..60
# measured). Revalidation ALWAYS uses this constant — an editable
# prov.frames must never decide whether exact-window checks run.
FRAMES_REQUIRED = 60


def check_legacy_parity(off_dir, legacy_dir):
    """Fresh-production OFF parity (plan §8): the R_OFF frame must be
    byte-identical to the fresh legacy production frame for the same
    fixture — the old 12-frame PNGs are never reused as baselines."""
    try:
        a = sha256_file(os.path.join(off_dir, "frame.png"))
        b = sha256_file(os.path.join(legacy_dir, "frame.png"))
    except OSError as e:
        return "FAIL (unreadable frame: %s)" % e
    if a != b:
        return "FAIL (OFF vs legacy frame mismatch)"
    return "complete"


def run_campaign(args, run_fn=None):
    """Execute the plan §8/§9 tables with hard elapsed caps.

    calib:  4 trials -> derive-freeze -> 4 confirms -> check-freeze
            -> 4 R_OFF + 4 legacy + parity -> 2 controls (18 runs).
    accept: per config, 4 quartets (16 runs) then analyze (64 runs).
    Anything unfinished at the cap stays INCOMPLETE (exit 2); any
    stage failure fails closed (exit 1). No GPU work happens here
    beyond the injected run_fn (CPU tests) or real captures."""
    if run_fn is None:
        run_fn = default_run_fn
    cap_min = CALIB_CAP_MIN if args.mode == "calib" else ACCEPT_CAP_MIN
    campdir = os.path.abspath(os.path.join(args.out_root, args.name))
    if os.path.exists(campdir):
        print("ERROR: campaign dir exists: %s" % campdir)
        return 1
    try:
        os.makedirs(campdir)
    except OSError as e:
        print("ERROR: cannot create campaign dir: %s" % e)
        return 1
    for cfg in CONFIGS:
        scene = getattr(args, "scene_" + cfg)
        if not scene or not os.path.exists(scene):
            print("ERROR: missing fixture scene for %s: %s" % (cfg, scene))
            return 1
    try:
        campaign_envs = {
            cfg: scene_environment(
                resolve_launch_path(getattr(args, "scene_" + cfg)),
                cfg, WORKLOADS[cfg]["extra"]) for cfg in CONFIGS}
    except (OSError, ValueError, TypeError) as e:
        print("ERROR: campaign environment preflight: %s" % e)
        return 1
    start = time.monotonic()
    deadline = start + cap_min * 60
    stages = []
    accept_authorities = None
    authority_file = ""
    authority_digest = ""

    def expired():
        return time.monotonic() >= deadline

    def campaign_env_error(cfg):
        try:
            current = scene_environment(
                resolve_launch_path(getattr(args, "scene_" + cfg)),
                cfg, WORKLOADS[cfg]["extra"])
            if current != campaign_envs[cfg]:
                return "campaign %s environment changed" % cfg
        except (OSError, ValueError, TypeError) as e:
            return "campaign %s environment unreadable: %s" % (cfg, e)
        return ""

    def capture_campaign(capture_args, timeout):
        cfg = capture_args.fixture_config
        err = campaign_env_error(cfg)
        if err:
            print("FAIL %s" % err)
            return 1
        rc = run_capture(capture_args, run_fn=run_fn,
                         timeout_min=timeout)
        err = campaign_env_error(cfg)
        if err:
            print("FAIL %s" % err)
            return 1
        return rc

    def pinned_error():
        if accept_authorities is None:
            return ""
        err = campaign_authority_error(accept_authorities)
        if err:
            return err
        try:
            if sha256_file(authority_file) != authority_digest:
                return "acceptance authority record changed"
        except OSError as e:
            return "acceptance authority record unreadable: %s" % e
        return ""

    def cap_args(exe, scene, name, cfg, frames=60, diag=False, edges="",
                 control=False, timed=True):
        # EVERY capture command comes from the frozen per-config
        # workload: spp/dims/extras are contract, never caller choice.
        wl = WORKLOADS[cfg]
        extra = list(wl["extra"])
        if timed:
            extra = extra + ["--benchmark-timings"]
        return argparse.Namespace(
            exe=exe, scene=scene, out_root=campdir, name=name,
            frames=frames, spp=wl["spp"], width=wl["width"],
            height=wl["height"], edges=edges, diag=diag, control=control,
            config=cfg, fixture_config=cfg, extra_args=extra)

    def stage_timeout(stages_left):
        # Launch-gate share via remaining_timeout: "SKIP" when the
        # budget is exhausted (zero/negative — this must NEVER reach
        # the uncapped execution mode), else a strictly positive
        # share. Callers MUST check for "SKIP" before spawning.
        return remaining_timeout(deadline, stages_left)

    if args.mode == "calib":
        # 18 runs + derive + 4 checks + 4 parities + 4 headrooms +
        # 2 responses = 33 stages; timeout shares divide evenly.
        trials = []
        for cfg in CONFIGS:
            if expired():
                stages.append((cfg + "-trial", "INCOMPLETE (cap)"))
                continue
            a = cap_args(args.ref_exe, getattr(args, "scene_" + cfg),
                         "trial-%s" % cfg, cfg, diag=True)
            to = stage_timeout(33 - len(stages))
            if to == "SKIP":
                stages.append((a.name, "INCOMPLETE (cap)"))
                continue
            rc = capture_campaign(a, to)
            stages.append((a.name, stage_result(rc)))
            if rc != 0:
                break
            trials.append(os.path.join(campdir, a.name))
        freeze_out = os.path.join(campdir, "frozen-edges.json")
        if len(trials) == 4:
            da = argparse.Namespace(trial_dirs=trials,
                                    ref_exe=args.ref_exe, out=freeze_out)
            rc = cmd_derive_freeze(da)
            stages.append(("derive-freeze", "complete" if rc == 0 else "FAIL"))
            if rc != 0:
                trials = []
        else:
            stages.append(("derive-freeze", "INCOMPLETE (trials)"))
            trials = []
        confirms = []
        for cfg in CONFIGS:
            if expired():
                stages.append((cfg + "-confirm", "INCOMPLETE (cap)"))
                continue
            if not trials:
                stages.append((cfg + "-confirm", "INCOMPLETE (freeze)"))
                continue
            a = cap_args(args.ref_exe, getattr(args, "scene_" + cfg),
                         "confirm-%s" % cfg, cfg, diag=True,
                         edges=freeze_out)
            to = stage_timeout(33 - len(stages))
            if to == "SKIP":
                stages.append((a.name, "INCOMPLETE (cap)"))
                continue
            rc = capture_campaign(a, to)
            stages.append((a.name, stage_result(rc)))
            if rc != 0:
                break
            confirms.append(os.path.join(campdir, a.name))
        if confirms:
            # Per-config check-freeze: group confirms by fixture.
            for cfg in CONFIGS:
                mates = [d for d in confirms
                         if os.path.basename(d) == "confirm-%s" % cfg]
                if not mates:
                    continue
                c1 = argparse.Namespace(run_dirs=mates, manifest=freeze_out,
                                        config=cfg, ref_exe=args.ref_exe)
                rc = cmd_check_freeze(c1)
                stages.append(("check-freeze-%s" % cfg,
                               "complete" if rc == 0 else "FAIL"))
        for cfg in CONFIGS:
            if expired():
                stages.append((cfg + "-off/legacy", "INCOMPLETE (cap)"))
                continue
            scene = getattr(args, "scene_" + cfg)
            a = cap_args(args.ref_exe, scene, "off-%s" % cfg, cfg)
            to = stage_timeout(33 - len(stages))
            if to == "SKIP":
                stages.append((a.name, "INCOMPLETE (cap)"))
                continue
            rc = capture_campaign(a, to)
            stages.append((a.name, stage_result(rc)))
            if rc != 0:
                break
            b = cap_args(args.legacy_exe, scene, "legacy-%s" % cfg, cfg)
            to = stage_timeout(33 - len(stages))
            if to == "SKIP":
                stages.append((b.name, "INCOMPLETE (cap)"))
                continue
            rc = capture_campaign(b, to)
            stages.append((b.name, stage_result(rc)))
            if rc != 0:
                break
            par = check_legacy_parity(os.path.join(campdir, a.name),
                                      os.path.join(campdir, b.name))
            stages.append(("parity-%s" % cfg, par))
            if par != "complete":
                break
            # 3% observer headroom on the matched confirm/OFF pair:
            # rate-1 validity is measured here, before any freeze use.
            confirm_dir = os.path.join(campdir, "confirm-%s" % cfg)
            if not os.path.exists(os.path.join(confirm_dir,
                                               "completion.json")):
                stages.append(("headroom-%s" % cfg, "INCOMPLETE (confirm)"))
                break
            head = check_calib_headroom(confirm_dir,
                                        os.path.join(campdir, a.name), cfg)
            stages.append(("headroom-%s" % cfg,
                           "complete" if not head else head))
            if head:
                break
        for tag, cfg in (("control-RT", "C1"), ("control-GI", "C3")):
            if expired():
                stages.append((tag, "INCOMPLETE (cap)"))
                continue
            if not trials:
                stages.append((tag, "INCOMPLETE (freeze)"))
                continue
            a = cap_args(args.ref_exe, getattr(args, "scene_" + cfg), tag,
                         cfg, diag=True, edges=freeze_out, control=True)
            to = stage_timeout(33 - len(stages))
            if to == "SKIP":
                stages.append((a.name, "INCOMPLETE (cap)"))
                continue
            rc = capture_campaign(a, to)
            stages.append((a.name, stage_result(rc)))
            if rc != 0:
                break
            # Sensitivity proof: the control must shift UP against the
            # matched ordinary confirm — identical histograms are an
            # unresponsive control, never a pass.
            try:
                with open(freeze_out) as f:
                    frozen = json.load(f)
                fRT, fGI = validate_manifest_edges(frozen)
            except (OSError, ValueError) as e:
                stages.append(("response-%s" % tag, "FAIL (freeze: %s)" % e))
                break
            resp = check_control_response(
                os.path.join(campdir, a.name),
                os.path.join(campdir, "confirm-%s" % cfg), cfg, (fRT, fGI))
            stages.append(("response-%s" % tag,
                           "complete" if not resp else resp))
            if resp:
                break
    else:
        if not args.freeze or not os.path.exists(args.freeze):
            print("ERROR: accept requires --freeze <frozen manifest>")
            return 1
        if not args.cand_exe:
            print("ERROR: accept requires --cand-exe <candidate build>")
            return 1
        if not distinct_role_paths(args.ref_exe, args.cand_exe):
            print("ERROR: accept requires distinct reference and "
                  "candidate builds (R/C comparison is vacuous on one "
                  "binary)")
            return 1
        try:
            with open(args.freeze) as f:
                frozen_acc = json.load(f)
            validate_manifest_edges(frozen_acc)
            seal_err = check_freeze_seal(
                frozen_acc, sha256_file(args.freeze))
        except (OSError, ValueError) as e:
            print("ERROR: bad freeze manifest: %s" % e)
            return 1
        if seal_err:
            print("ERROR: unsealed freeze manifest (no lineage): %s"
                  % seal_err)
            return 1
        # Acceptance binds to the calibration evidence: the candidate
        # campaign must run the SAME fixtures with the SAME reference
        # binary the freeze was derived from. A sealed manifest from
        # another fixture set or binary cannot unlock this campaign.
        try:
            freeze_lineage = frozen_acc.get("lineage", {})
            for cfg in CONFIGS:
                if (sha256_file(getattr(args, "scene_" + cfg)) !=
                        freeze_lineage.get("trials", {}).get(
                            cfg, {}).get("scene_sha256")):
                    print("ERROR: accept scene %s differs from calibration "
                          "fixture" % cfg)
                    return 1
                if (os.path.normcase(os.path.realpath(
                        getattr(args, "scene_" + cfg))) !=
                        os.path.normcase(os.path.realpath(
                            freeze_lineage["trials"][cfg].get("scene", "")))):
                    print("ERROR: accept scene %s path differs from "
                          "calibration fixture" % cfg)
                    return 1
            if sha256_file(args.ref_exe) != freeze_lineage.get(
                    "ref_exe_sha256"):
                print("ERROR: accept --ref-exe differs from calibration "
                      "reference")
                return 1
        except OSError as e:
            print("ERROR: accept identity check failed: %s" % e)
            return 1
        lineage_err = revalidate_freeze_lineage(frozen_acc, args.ref_exe)
        if lineage_err:
            print("ERROR: accept lineage revalidation: %s" % lineage_err)
            return 1
        calib_err = revalidate_calibration(frozen_acc, args.freeze,
                                           args.ref_exe)
        if calib_err:
            print("ERROR: accept calibration revalidation: %s" % calib_err)
            return 1
        try:
            accept_authorities = {"ref": exe_authority(args.ref_exe),
                                  "cand": exe_authority(args.cand_exe)}
        except OSError as e:
            print("ERROR: accept authority unreadable: %s" % e)
            return 1
        for role, record in accept_authorities.items():
            if any(v is None for v in record["spvs"].values()):
                print("ERROR: accept %s authority has missing SPVs" % role)
                return 1
        authority_file = os.path.join(campdir, "accept-authorities.json")
        auth_write_err = write_text_checked(
            authority_file, json.dumps(accept_authorities, indent=1) + "\n")
        if auth_write_err:
            print("ERROR: accept authority persistence: %s" % auth_write_err)
            return 1
        authority_digest = sha256_file(authority_file)
        order = [(c2, qn) for c2 in CONFIGS for qn in range(1, 5)]
        n_quartets = len(order)
        for cfg in CONFIGS:
            scene = getattr(args, "scene_" + cfg)
            for k in range(1, 5):
                qname = "block-%s-%d" % (cfg, k)
                if expired():
                    stages.append((qname, "INCOMPLETE (cap)"))
                    continue
                auth_err = pinned_error()
                if not auth_err:
                    auth_err = campaign_env_error(cfg)
                if auth_err:
                    stages.append((qname, "FAIL (" + auth_err + ")"))
                    break
                qa = argparse.Namespace(
                    ref_exe=args.ref_exe, cand_exe=args.cand_exe,
                    scene=scene, out_root=campdir, name=qname,
                    frames=60, spp=WORKLOADS[cfg]["spp"],
                    width=WORKLOADS[cfg]["width"],
                    height=WORKLOADS[cfg]["height"], edges=args.freeze,
                    control=False, config=cfg,
                    extra_args=list(WORKLOADS[cfg]["extra"]))
                # Equal share of the remaining budget per quartet via
                # the launch gate: "SKIP" stops without spawning.
                done = len([s for s in stages
                            if s[0].startswith("block-")])
                share = remaining_timeout(deadline, n_quartets - done)
                if share == "SKIP":
                    stages.append((qname, "INCOMPLETE (cap)"))
                    continue
                rc = run_quartet(qa, run_fn=run_fn, deadline_min=share)
                auth_err = pinned_error()
                if not auth_err:
                    auth_err = campaign_env_error(cfg)
                stages.append((qa.name,
                               "FAIL (" + auth_err + ")" if auth_err else
                               ("complete" if rc == 0 else
                                ("INCOMPLETE" if rc == 2 else "FAIL"))))
                if rc != 0 or auth_err:
                    break
            mates = [os.path.join(campdir, "block-%s-%d" % (cfg, k))
                     for k in range(1, 5)]
            if all(os.path.exists(os.path.join(m, "block.json"))
                   for m in mates):
                an = argparse.Namespace(block_dirs=mates,
                                        manifest=args.freeze, config=cfg,
                                        ref_exe=args.ref_exe,
                                        cand_exe=args.cand_exe,
                                        authority_file=authority_file,
                                        campaign_authorities=accept_authorities)
                rc = cmd_analyze(an)
                auth_err = pinned_error()
                stages.append(("analyze-%s" % cfg,
                               "FAIL (" + auth_err + ")" if auth_err else
                               "complete" if rc == 0 else
                               ("INCOMPLETE" if rc == 2 else "FAIL")))
        final_auth_err = pinned_error()
        if final_auth_err:
            stages.append(("authority-final", "FAIL (" + final_auth_err + ")"))
    final_env_errors = [campaign_env_error(cfg) for cfg in CONFIGS]
    final_env_errors = [err for err in final_env_errors if err]
    if final_env_errors:
        stages.append(("environment-final", "FAIL (" + final_env_errors[0] + ")"))
    elapsed_min = (time.monotonic() - start) / 60
    # No over-cap completion: even with all stages green, ending past
    # the cap is INCOMPLETE (the budget covers warmup, readback,
    # freeze and analysis — not just process launches).
    over_cap = elapsed_min > cap_min
    manifest = {"campaign": args.name, "mode": args.mode,
                "stages": stages, "elapsed_min": elapsed_min,
                "cap_min": cap_min, "over_cap": over_cap,
                "environment_authorities": campaign_envs,
                "complete": all(v == "complete" for _, v in stages) and
                len(stages) > 0 and not over_cap}
    if accept_authorities is not None:
        manifest["authorities"] = accept_authorities
        manifest["authority_sha256"] = authority_digest
    err = write_text_checked(os.path.join(campdir, "campaign.json"),
                             json.dumps(manifest, indent=1) + "\n")
    if err:
        print("ERROR: %s" % err)
        return 1
    late_env_errors = [campaign_env_error(cfg) for cfg in CONFIGS]
    late_env_errors = [err for err in late_env_errors if err]
    if late_env_errors:
        manifest["complete"] = False
        manifest["stages"].append(
            ("environment-postwrite", "FAIL (" + late_env_errors[0] + ")"))
        err = write_text_checked(os.path.join(campdir, "campaign.json"),
                                 json.dumps(manifest, indent=1) + "\n")
        if err:
            print("ERROR: cannot revoke campaign env completion: %s" % err)
            return 1
    if accept_authorities is not None:
        # Catch a substitution during the final manifest write itself:
        # never leave a persisted complete:true marker for changed bytes.
        late_auth_err = pinned_error()
        if late_auth_err:
            manifest["complete"] = False
            manifest["stages"].append(
                ("authority-postwrite", "FAIL (" + late_auth_err + ")"))
            err = write_text_checked(os.path.join(campdir, "campaign.json"),
                                     json.dumps(manifest, indent=1) + "\n")
            if err:
                print("ERROR: cannot revoke campaign completion: %s" % err)
                return 1
    if args.mode == "calib" and manifest["complete"]:
        # Seal the campaign freeze with the full calibration evidence:
        # without this seal the freeze is trials-only and can never
        # unlock acceptance, no matter what its edges say.
        seal_err = seal_calibration(freeze_out, campdir, args.legacy_exe)
        if seal_err:
            print("ERROR: calibration seal failed: %s" % seal_err)
            return 1
    print("CAMPAIGN %s %s stages=%d elapsed=%.1fmin" %
          (args.name, "complete" if manifest["complete"] else "INCOMPLETE",
           len(stages), elapsed_min))
    if manifest["complete"]:
        return 0
    return 2 if any("INCOMPLETE" in v for _, v in stages) else 1


def cmd_derive_freeze(args):
    """Reference-only edge derivation from pilot trial snapshots.

    Each --trial-dir must be a COMPLETED pilot trial rundir: the trial
    re-validates as pilot-complete (exact 41..60 window, workload seal,
    effective backend), provenance exe hash == --ref-exe, and the four
    trials must cover exactly the C1..C4 configs with the PRESCRIBED
    scene relationships — C2 and C3 SHARE the aperture-zero adaptation
    scene (plan §6/§9); C1, C4 and the adaptation scene are pairwise
    distinct. Four distinct hashes would reject the legitimate pairing.
    Acceptance/control snapshots, mixed configs, hand-assembled dirs,
    candidate builds and incomplete runs all fail closed."""
    if not args.ref_exe:
        print("FAIL derive-freeze: --ref-exe required (trial identity)")
        return 1
    try:
        ref_hash = sha256_file(args.ref_exe)
    except OSError as e:
        print("FAIL derive-freeze: cannot hash ref exe: %s" % e)
        return 1
    ref_spvs = hash_exe_spvs(args.ref_exe)
    pilot = [[0] * 34, [0] * 34]
    nfiles = 0
    trial_recs = {}
    for trial in args.trial_dirs:
        trial_abs = os.path.abspath(trial)
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
        if os.path.abspath(prov.get("exe", "")) != \
                os.path.abspath(args.ref_exe):
            print("FAIL derive-freeze: trial exe is not the reference "
                  "binary: %s" % trial)
            return 1
        if prov.get("frames") != FRAMES_REQUIRED:
            print("FAIL derive-freeze: trial declares frames=%r, contract "
                  "requires %d: %s"
                  % (prov.get("frames"), FRAMES_REQUIRED, trial))
            return 1
        cfg = prov.get("config", "")
        if cfg not in CONFIGS:
            print("FAIL derive-freeze: trial has no frozen config: %s"
                  % trial)
            return 1
        if cfg in trial_recs:
            print("FAIL derive-freeze: duplicate trial config %s" % cfg)
            return 1
        w_err = check_workload(prov, cfg)
        if w_err:
            print("FAIL derive-freeze: %s: %s" % (trial, w_err))
            return 1
        try:
            if prov.get("environment") != scene_environment(
                    prov.get("scene", ""), cfg, prov.get("extra", [])):
                print("FAIL derive-freeze: trial environment mismatch: %s"
                      % trial)
                return 1
        except (OSError, ValueError, TypeError) as e:
            print("FAIL derive-freeze: trial environment unreadable: %s"
                  % e)
            return 1
        if not completion.get("ngx_effective"):
            print("FAIL derive-freeze: no effective-backend evidence: %s"
                  % trial)
            return 1
        # Re-validate the trial as pilot-complete: the completion flag
        # alone is not lineage — the window, mode and coverage must hold.
        # Provenance extras travel along so the env-loaded gate applies
        # to C2 trials exactly as at capture time. Frame count is the
        # FIXED contract constant, never the editable prov value.
        reval = argparse.Namespace(frames=FRAMES_REQUIRED,
                                   diag=True, edges="", control=False,
                                   config=cfg,
                                   extra_args=prov.get("extra", []))
        re_problems, _, n_pilot, _ = validate_rundir(trial, reval)
        if re_problems:
            print("FAIL derive-freeze: trial re-validation %s: %s"
                  % (trial, re_problems[:3]))
            return 1
        if n_pilot == 0:
            print("FAIL derive-freeze: trial has no pilot snapshots: %s"
                  % trial)
            return 1
        trial_recs[cfg] = {
            "dir": trial_abs,
            "scene": prov.get("scene", ""),
            "scene_sha256": prov.get("scene_sha256", ""),
            "environment": prov.get("environment"),
            "exe_sha256": prov.get("exe_sha256", ""),
            "workload": prov.get("workload", {}),
            "config": cfg,
            "completion_sha256": sha256_file(
                os.path.join(trial_abs, "completion.json")),
            "provenance_sha256": sha256_file(
                os.path.join(trial_abs, "provenance.json")),
            "pilot_window_snapshots": n_pilot,
        }
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
                frame, slot, _, head, data = parse_snapshot_data(obj)
            except ValueError:
                continue
            # Measured window ONLY: warmup pilot mass must not shape Q.
            if not 41 <= frame <= 60:
                continue
            # Reference trials are pilot-mode only: acceptance or
            # control snapshots pooled here would corrupt Q silently.
            if head[3] != 1 or head[4] != 0 or head[5] != 0:
                print("FAIL derive-freeze: non-pilot snapshot in %s"
                      % trial)
                return 1
            base = slot * (2 * 298 + 2)
            for fam in (0, 1):
                for i in range(34):
                    pilot[fam][i] += data[base + fam * 298 + i]
            nfiles += 1
    if set(trial_recs.keys()) != set(CONFIGS):
        print("FAIL derive-freeze: trials must cover exactly C1..C4, got %s"
              % sorted(trial_recs.keys()))
        return 1
    scenes = {c: trial_recs[c]["scene_sha256"] for c in CONFIGS}
    if not all(scenes.values()):
        print("FAIL derive-freeze: trial scenes must all be hashed")
        return 1
    if scenes["C2"] != scenes["C3"]:
        print("FAIL derive-freeze: C2/C3 must share the adaptation scene "
              "(plan §6/§9), got distinct fixtures")
        return 1
    if len({scenes["C1"], scenes["C2"], scenes["C4"]}) != 3:
        print("FAIL derive-freeze: C1/adaptation/C4 fixtures must be "
              "pairwise distinct")
        return 1
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
    manifest["workload"] = WORKLOADS
    manifest["lineage"] = {"trials": trial_recs,
                           "ref_exe_sha256": ref_hash,
                           "ref_spvs": ref_spvs,
                           "edges_sha256": sha256_text(canonical_edges(
                               manifest["edgesRT"], manifest["edgesGI"]))}
    err = write_text_checked(args.out, json.dumps(manifest, indent=1) + "\n")
    if err:
        print("ERROR: %s" % err)
        return 1
    print("FREEZE manifest=%s" % args.out)
    return 0


# (derive_freeze_edges is defined once, near pilot_bracket/build_edges.)


def cmd_check_freeze(args):
    """Confirmation gate on explicit confirmation rundirs + manifest.

    Each run dir re-validates as acceptance-complete for the REQUESTED
    config (window, workload seal, exe/scene/edge identity,
    effective backend); the manifest itself must carry the
    reference-only lineage seal. Pooling arbitrary dirs cannot pass."""
    with open(args.manifest) as f:
        manifest = json.load(f)
    try:
        edgesRT, edgesGI = validate_manifest_edges(manifest)
    except ValueError as e:
        print("FAIL check-freeze: bad manifest: %s" % e)
        return 1
    try:
        seal_err = check_freeze_seal(manifest, sha256_file(args.manifest))
    except OSError as e:
        print("FAIL check-freeze: cannot hash manifest: %s" % e)
        return 1
    if seal_err:
        print("FAIL check-freeze: %s" % seal_err)
        return 1
    required_cfg = args.config
    if required_cfg not in CONFIGS:
        print("FAIL check-freeze: --config required (one of C1..C4)")
        return 1
    try:
        ref_hash = sha256_file(args.ref_exe)
    except OSError as e:
        print("FAIL check-freeze: cannot hash ref exe: %s" % e)
        return 1
    if manifest.get("lineage", {}).get("ref_exe_sha256") != ref_hash:
        print("FAIL check-freeze: manifest lineage exe differs from "
              "--ref-exe")
        return 1
    # Source proof: reopen the calibration trials and re-derive — a
    # well-formed manifest without evidence fails here.
    lineage_err = revalidate_freeze_lineage(manifest, args.ref_exe)
    if lineage_err:
        print("FAIL check-freeze: %s" % lineage_err)
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
        if os.path.abspath(prov.get("exe", "")) != \
                os.path.abspath(args.ref_exe):
            print("FAIL check-freeze: run exe is not the reference "
                  "binary: %s" % rundir)
            return 1
        if prov.get("frames") != FRAMES_REQUIRED:
            print("FAIL check-freeze: run declares frames=%r, contract "
                  "requires %d: %s"
                  % (prov.get("frames"), FRAMES_REQUIRED, rundir))
            return 1
        w_err = check_workload(prov, required_cfg)
        if w_err:
            print("FAIL check-freeze: %s: %s" % (rundir, w_err))
            return 1
        # Same-fixture binding: confirmations must run the trial
        # fixture for their config (a valid run on another scene is
        # not confirmation evidence for this one).
        trial_scene = manifest.get("lineage", {}).get(
            "trials", {}).get(required_cfg, {}).get("scene_sha256", "")
        if not trial_scene or prov.get("scene_sha256") != trial_scene:
            print("FAIL check-freeze: run scene differs from %s trial "
                  "fixture: %s" % (required_cfg, rundir))
            return 1
        trial_path = manifest.get("lineage", {}).get("trials", {}).get(
            required_cfg, {}).get("scene", "")
        if (not trial_path or not prov.get("scene") or
                os.path.normcase(os.path.realpath(prov["scene"])) !=
                os.path.normcase(os.path.realpath(trial_path))):
            print("FAIL check-freeze: run scene path differs from %s "
                  "trial fixture: %s" % (required_cfg, rundir))
            return 1
        try:
            if (not isinstance(completion.get("frame_sha256"), str) or
                    completion["frame_sha256"] != sha256_file(
                        os.path.join(rundir, "frame.png"))):
                print("FAIL check-freeze: frame differs from completion: %s"
                      % rundir)
                return 1
        except OSError as e:
            print("FAIL check-freeze: frame unreadable %s: %s" % (rundir, e))
            return 1
        if prov.get("edges_sha256") != sha256_file(args.manifest):
            print("FAIL check-freeze: run staged different edges: %s"
                  % rundir)
            return 1
        if not completion.get("ngx_effective"):
            print("FAIL check-freeze: no effective-backend evidence: %s"
                  % rundir)
            return 1
        # Re-validate: the completion flag alone is not lineage.
        reval = argparse.Namespace(frames=FRAMES_REQUIRED,
                                   diag=True, edges="manifest",
                                   control=False, config=required_cfg,
                                   extra_args=prov.get("extra", []))
        re_problems, n_valid, _, _ = validate_rundir(rundir, reval)
        if re_problems:
            print("FAIL check-freeze: run re-validation %s: %s"
                  % (rundir, re_problems[:3]))
            return 1
        if n_valid == 0:
            print("FAIL check-freeze: run has no valid snapshots: %s"
                  % rundir)
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
                obj = json.loads(line.split("[ShadowTail]", 1)[1])
            except ValueError as e:
                print("FAIL check-freeze: %s" % e)
                return 1
            # Measured window only: warmup mass must not pad minimums.
            if not 41 <= obj.get("frame", -1) <= 60:
                continue
            try:
                snaps.append(validate_snapshot_line(obj))
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

    # Golden serialized outputs: the COMMITTED files below are byte output
    # of the compiled C++ SnapshotJson path (regenerate with
    # ST_WRITE_GOLDENS=1); when ST_RT2TESTS_EXE points at a built
    # RT2Tests binary this self-test additionally RE-EXECUTES that
    # native path into a scratch dir, requires byte identity with the
    # committed files, and consumes the REGENERATED bytes below — so
    # the run/derive/control integration tests exercise actual native
    # serialization, never mirror literals.
    repo_root = os.path.dirname(os.path.abspath(__file__))

    def native_golden(name):
        committed = os.path.join(repo_root, "RT2Tests", "src", name)
        with open(committed) as f:
            pinned = f.read()
        exe = os.environ.get("ST_RT2TESTS_EXE")
        if not exe:
            return pinned
        regen_dir = tempfile.mkdtemp(prefix="st-goldens-")
        env = dict(os.environ, ST_WRITE_GOLDENS=regen_dir)
        try:
            rc = subprocess.run(
                [exe, "--test-case=*golden*"], cwd=repo_root, env=env,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=300).returncode
        except (OSError, subprocess.SubprocessError) as e:
            check("native golden regen executes (%s)" % name, False)
            print("  regen error: %s" % e)
            return pinned
        check("native golden regen executes (%s)" % name, rc == 0)
        if rc != 0:
            return pinned
        with open(os.path.join(regen_dir, name)) as f:
            fresh = f.read()
        check("native golden byte-identical (%s)" % name, fresh == pinned)
        return fresh

    try:
        pilot_golden = native_golden("shadowtail_golden_pilot.json")
        ctl_golden = native_golden("shadowtail_golden_control.json")
    except OSError as e:
        check("golden files present", False)
        print("SELF-TEST FAILURES: %s" % fails)
        return 1
    gp = json.loads(pilot_golden)
    check("golden pilot prefix/schema",
          pilot_golden.startswith(
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
    gc = json.loads(ctl_golden)
    check("golden control prefix/schema",
          ctl_golden.startswith(
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
    def fake_native(cwd, mode="Off", present=True):
        if not present:
            return
        with open(os.path.join(cwd, "rt2_log.txt"), "w") as native:
            native.write("=== RT2 Log ===\n")
            for fr in range(1, 61):
                native.write("[Render] submitting frame %d (NRD=0 composeCached=0)\n" % fr)
                native.write("[Denoiser] submitted=%s backend=none\n" % mode)

    def fake_run(exe_, cmd, cwd, logpath, snaps_fn, png=True, rc=0,
                 mode_lines=True, timeout_min=None, env_lines=None):
        from PIL import Image
        idx = cmd.index("--output-display") + 1
        if png:
            Image.new("RGB", (8, 8), (10, 20, 30)).save(cmd[idx])
        with open(logpath, "w") as lf:
            if mode_lines:
                lf.write("[NGX] supported=yes backend=none (fake)\n")
            # env_lines None = emit exactly when the command requests
            # --env (mirrors a real loader); True forces, False
            # suppresses (for the missing-evidence negative test).
            if env_lines or (env_lines is None and
                             ("--env" in cmd or "-e" in cmd)):
                lf.write("[EnvMap] Loaded 4x2 EXR (fake)\n")
            if "--shadow-tail" in cmd:
                for line in snaps_fn():
                    lf.write(line)
            if "--benchmark-timings" in cmd:
                # Complete measured window on every emitted dispatch:
                # single-record timing presence proves nothing.
                for fr in range(41, 61):
                    lf.write('[HeadlessTiming] {"frame":%d,"regions_ms":'
                             '{"RT Shading":0.5,"ReSTIR GI Temporal":0.3,'
                              '"GPU Frame":1.0}}\n' % fr)
        fake_native(cwd)
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
        cand_dir = os.path.join(tmp, "candidate")
        os.makedirs(cand_dir)
        cand_exe = os.path.join(cand_dir, "RT2App.exe")
        with open(cand_exe, "wb") as f:
            f.write(b"fake-exe")  # equal bytes, separately declared role
        for spv in ("closesthit.spv", "secondary_raygen.spv",
                    "restir_gi_temporal.spv", "closesthit_tail.spv",
                    "secondary_raygen_tail.spv",
                    "restir_gi_temporal_tail.spv"):
            with open(os.path.join(cand_dir, spv), "wb") as f:
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
                          run_fn=lambda e, c, w, l, t=0: fake_run(
                              e, c, w, l, lambda: snap_lines())) == 0)
        with open(os.path.join(tmp, "happy", "run.log")) as f:
            happy_stdout = f.read()
        check("explicit Off needs no stdout Denoiser line",
              "[Denoiser]" not in happy_stdout and
              "submitted=Off" in json.load(open(os.path.join(
                  tmp, "happy", "completion.json")))["denoiser_effective"])
        with open(os.path.join(tmp, "rt2_log.txt"), "w") as f:
            f.write("old stale [Denoiser] submitted=Off backend=none\n")
        check("stale native log archived and fresh run succeeds",
              run_capture(mkargs("prior-native"),
                          run_fn=lambda e, c, w, l, t=0: fake_run(
                              e, c, w, l, lambda: snap_lines())) == 0 and
              "old stale" in open(os.path.join(
                  tmp, "prior-native", "prior-native-rt2.log")).read())

        def no_native_fake(e, c, w, l, t=0):
            fake_run(e, c, w, l, lambda: snap_lines())
            os.remove(os.path.join(w, "rt2_log.txt"))
            with open(l, "a") as f:
                f.write("[Denoiser] submitted=Off backend=none (forged stdout)\n")
            return 0

        check("stale native and forged stdout cannot mask missing new file",
              run_capture(mkargs("missing-native"), run_fn=no_native_fake) == 1 and
              not os.path.exists(os.path.join(
                  tmp, "missing-native", "completion.json")))

        def wrong_native_fake(e, c, w, l, t=0):
            fake_run(e, c, w, l, lambda: snap_lines())
            fake_native(w, mode="NRD")
            with open(l, "a") as f:
                f.write("[Denoiser] submitted=Off backend=none (forged stdout)\n")
            return 0

        check("actual submitted wrong mode rejects forged stdout Off",
              run_capture(mkargs("wrong-native"), run_fn=wrong_native_fake) == 1)
        native_lock = os.path.join(tmp, "rt2_shadow_tail_capture.lock")
        with open(native_lock, "w") as f:
            f.write("other capture owns this CWD\n")
        try:
            check("concurrent native CWD owner rejects capture",
                  run_capture(mkargs("locked-native"),
                              run_fn=lambda e, c, w, l, t=0: fake_run(
                                  e, c, w, l, lambda: snap_lines())) == 1)
        finally:
            os.remove(native_lock)
        check("fake nonzero exit fails",
              run_capture(mkargs("rc1"),
                          run_fn=lambda e, c, w, l, t=0: fake_run(
                              e, c, w, l, lambda: snap_lines(), rc=1)) == 1)
        check("fake missing frame fails",
              run_capture(mkargs("noframe"),
                          run_fn=lambda e, c, w, l, t=0: fake_run(
                              e, c, w, l, lambda: snap_lines(),
                              png=False)) == 1)
        dup_lines = snap_lines(n=1) + snap_lines(n=1)
        check("fake duplicate frames fail",
              run_capture(mkargs("dup"),
                          run_fn=lambda e, c, w, l, t=0: fake_run(
                              e, c, w, l, lambda: dup_lines)) == 1)
        check("fake OOB data fails",
              run_capture(mkargs("oob"),
                          run_fn=lambda e, c, w, l, t=0: fake_run(
                              e, c, w, l,
                              lambda: snap_lines(oob=True))) == 1)
        check("fake missing coverage fails",
              run_capture(mkargs("nocov", config="C4"),
                          run_fn=lambda e, c, w, l, t=0: fake_run(
                              e, c, w, l,
                              lambda: snap_lines(empty=True))) == 1)
        check("fake tamper setup",
              run_capture(mkargs("tamper"),
                          run_fn=lambda e, c, w, l, t=0: fake_run(
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
              run_capture(a8, run_fn=lambda e, c, w, l, t=0: fake_run(
                  e, c, w, l, lambda: snap_lines())) != 0)

        # derive-freeze on sealed pilot trial rundirs (one per config,
        # full 41..60 window, strata-rich pilot lines, workload seals,
        # effective-backend evidence). Trial scenes are distinct per
        # config so the uniqueness gate is genuinely exercised.
        def write_trial(parent, tag, cfg, scene_file):
            tdir = os.path.join(parent, tag)
            os.makedirs(tdir)
            from PIL import Image as _TrialImage
            _TrialImage.new("RGB", (8, 8), (10, 20, 30)).save(
                os.path.join(tdir, "frame.png"))
            with open(os.path.join(tdir, "run.log"), "w") as f:
                f.write("[NGX] supported=yes backend=none (fake)\n")
                if cfg == "C2":
                    f.write("[EnvMap] Loaded 4x2 EXR (fake)\n")
                if cfg == "C4":
                    f.write("[OpenRt2Scene] adopting document...\n")
                    f.write("[Scene] Env map: idx=4 4x2, CDF built\n")
                for fr in range(41, 61):
                    pl = {"schema": SCHEMA, "frame": fr, "slot": 0,
                          "serial": fr, "valid": False,
                          "reason": "pilot-only",
                          "head": [0x53484457, 1, fr, 1, 0, 0, 1, 0],
                          "data": [0] * 1196}
                    pl["data"][0 + 0 * 298 + 14] = 5000
                    pl["data"][0 + 1 * 298 + 14] = 5000
                    for fam in (0, 1):
                        for site in range(5):
                            pl["data"][fam * 298 + 264 + site * 2 + 0] = 100
                            pl["data"][fam * 298 + 264 + site * 2 + 1] = 100
                    f.write("[ShadowTail] %s\n" % json.dumps(pl))
            fake_native(tdir)
            shutil.copyfile(os.path.join(tdir, "rt2_log.txt"),
                            os.path.join(tdir, "native-rt2.log"))
            with open(os.path.join(tdir, "completion.json"), "w") as f:
                json.dump({"complete": True,
                            "ngx_effective": "[NGX] fake",
                            "denoiser_effective": "[Denoiser] submitted=Off backend=none",
                            "native_log_sha256": sha256_file(
                                os.path.join(tdir, "native-rt2.log"))}, f)
            with open(os.path.join(tdir, "provenance.json"), "w") as f:
                json.dump({"exe": os.path.abspath(exe),
                           "exe_sha256": sha256_file(exe),
                           "scene": os.path.abspath(scene_file),
                           "scene_sha256": sha256_file(scene_file),
                           "frames": 60, "config": cfg,
                           "extra": WORKLOADS[cfg]["extra"],
                           "environment": scene_environment(
                               scene_file, cfg, WORKLOADS[cfg]["extra"]),
                           "workload": WORKLOADS[cfg]}, f)
            return tdir

        pdir = os.path.join(tmp, "pilot")
        os.makedirs(pdir)
        # Prescribed fixture relations: C2/C3 SHARE the adaptation
        # scene; C1, C4 and adaptation are pairwise distinct.
        adapt = os.path.join(tmp, "trial-scene-adapt.rt2scene")
        with open(adapt, "w") as f:
            f.write('{"fixture": "adapt"}')
        trial_scenes = {}
        for cfg in ("C1", "C2", "C3", "C4"):
            if cfg in ("C2", "C3"):
                trial_scenes[cfg] = adapt
            else:
                sp = os.path.join(tmp, "trial-scene-%s.rt2scene" % cfg)
                with open(sp, "w") as f:
                    if cfg == "C4":
                        json.dump({"fixture": cfg,
                                   "envMap": {"path": "trial-c4.exr",
                                              "assetId": ""}}, f)
                    else:
                        f.write('{"fixture": "%s"}' % cfg)
                trial_scenes[cfg] = sp
        with open(os.path.join(tmp, "trial-c4.exr"), "wb") as f:
            f.write(b"fake trial C4 environment")
        trial_dirs = [write_trial(pdir, "T%d" % (t + 1), cfg,
                                  trial_scenes[cfg])
                      for t, cfg in enumerate(("C1", "C2", "C3", "C4"))]
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

        # Campaign driver: full calib (18 runs) + accept (64 runs) tables
        # with injected fake processes. Trial/confirm mass sits in the
        # k=13 doubling bin by construction (5000 counts/line); the
        # campaign's own derive-freeze verifies the resulting 226-edge
        # freeze, and the sealed-equality check below pins the
        # assumption. Confirms carry generous strata for every config's
        # coverage matrix.
        import bisect as _bisect_campaign
        birt_c = _bisect_campaign.bisect_right(
            build_edges(8192, 16384), 10000) - 1
        scenes = {}
        written = set()
        for cfg in ("C1", "C2", "C3", "C4"):
            # C2 and C3 SHARE the aperture-zero adaptation scene (plan
            # §6/§9: same file, different DI/GI flag sets); C1 and C4
            # are distinct fixtures. The derive gate enforces exactly
            # this relationship — fakes must honor it, not dodge it
            # with four distinct files.
            tag = "adapt" if cfg in ("C2", "C3") else cfg
            p = os.path.join(tmp, "scene-%s.rt2scene" % tag)
            if tag not in written:
                with open(p, "w") as f:
                    if cfg == "C4":
                        json.dump({"campaign_fixture": tag,
                                   "envMap": {"path": "fake-c4.exr",
                                              "assetId": ""}}, f)
                    else:
                        f.write('{"campaign_fixture": "%s"}' % tag)
                written.add(tag)
            scenes[cfg] = p
        with open(os.path.join(tmp, "fake-c4.exr"), "wb") as f:
            f.write(b"fake saved-scene environment")
        legacy_exe = os.path.join(tmp, "legacy.exe")
        with open(legacy_exe, "wb") as f:
            f.write(b"fake-legacy")

        def rich_pilot_lines():
            lines = []
            for f in range(41, 61):
                o = {"schema": SCHEMA, "frame": f, "slot": 0,
                     "serial": f, "valid": False,
                     "reason": "pilot-only: no manifest staged",
                     "head": [0x53484457, 1, f, 1, 0, 0, 1, 0],
                     "data": [0] * 1196}
                o["data"][0 * 298 + 14] = 5000
                o["data"][1 * 298 + 14] = 5000
                # Pilot snapshots carry stratum labels (§3a), so trial
                # coverage is meaningful on pilot data.
                for fam in (0, 1):
                    for site in range(5):
                        o["data"][fam * 298 + 264 + site * 2 + 0] = 100
                        o["data"][fam * 298 + 264 + site * 2 + 1] = 100
                lines.append("[ShadowTail] %s\n" % json.dumps(o))
            return lines

        def rich_valid_lines(shift=0):
            lines = []
            for f in range(41, 61):
                s = {"schema": SCHEMA, "frame": f, "slot": 0,
                     "serial": f, "valid": True,
                     "head": [0x53484457, 1, f, 1, 226, 226, 1, 0],
                     "data": [0] * 1196}
                for fam in (0, 1):
                    s["data"][fam * 298 + 34 + birt_c + shift] = 1000
                    s["data"][fam * 298 + 260] = 1000
                    s["data"][fam * 298 + 261] = 1000
                    s["data"][fam * 298 + 262] = 400
                    s["data"][fam * 298 + 263] = 600
                    for site in range(5):
                        s["data"][fam * 298 + 264 + site * 2 + 0] = 100
                        s["data"][fam * 298 + 264 + site * 2 + 1] = 100
                lines.append("[ShadowTail] %s\n" % json.dumps(s))
            return lines

        def rich_control_lines(shift=20, checksum=700):
            # Dependent-work control: histogram shifted UP by `shift`
            # bins, checksum consumed outside the bracket, outcomes
            # present. shift=0 models a FLAT (unresponsive) control.
            lines = []
            for f in range(41, 61):
                s = {"schema": SCHEMA, "frame": f, "slot": 0,
                     "serial": f, "valid": False,
                     "reason": "positive-control mode",
                     "head": [0x53484457, 1, f, 2, 226, 226, 1, 0],
                     "data": [0] * 1196}
                for fam in (0, 1):
                    s["data"][fam * 298 + 34 + birt_c + shift] = 1000
                    s["data"][fam * 298 + 260] = 1000
                    s["data"][fam * 298 + 261] = 1000
                    s["data"][fam * 298 + 262] = 400
                    s["data"][fam * 298 + 263] = 600
                    for site in range(5):
                        s["data"][fam * 298 + 264 + site * 2 + 0] = 100
                        s["data"][fam * 298 + 264 + site * 2 + 1] = 100
                # Control checksums live past both family blocks in the
                # slot segment (slot*598 + 596/597 — same addressing the
                # golden control test pins).
                s["data"][596] = checksum
                s["data"][597] = checksum
                lines.append("[ShadowTail] %s\n" % json.dumps(s))
            return lines

        def camp_fake(exe_, cmd, cwd, logpath, timeout_min=None):
            from PIL import Image as _I2
            _I2.new("RGB", (8, 8), (10, 20, 30)).save(
                cmd[cmd.index("--output-display") + 1])
            with open(logpath, "w") as lf:
                lf.write("[NGX] supported=yes backend=none (fake)\n")
                if "--env" in cmd or "-e" in cmd:
                    lf.write("[EnvMap] Loaded 4x2 EXR (fake)\n")
                if os.path.basename(cmd[cmd.index("--scene") + 1]) == "scene-C4.rt2scene":
                    lf.write("[OpenRt2Scene] adopting document...\n")
                    lf.write("[Scene] Env map: idx=4 4x2, CDF built\n")
                if "--shadow-tail-control" in cmd:
                    for line in rich_control_lines():
                        lf.write(line)
                elif "--shadow-tail-edges" in cmd:
                    for line in rich_valid_lines():
                        lf.write(line)
                elif "--shadow-tail" in cmd:
                    for line in rich_pilot_lines():
                        lf.write(line)
                if "--benchmark-timings" in cmd:
                    for f in range(41, 61):
                        lf.write('[HeadlessTiming] {"frame":%d,'
                                 '"regions_ms":{"RT Shading":0.5,'
                                 '"ReSTIR GI Temporal":0.3,'
                                 '"GPU Frame":1.0}}\n' % f)
            fake_native(cwd)
            return 0

        calib = argparse.Namespace(
            mode="calib", ref_exe=exe, cand_exe="", legacy_exe=legacy_exe,
            scene_C1=scenes["C1"], scene_C2=scenes["C2"],
            scene_C3=scenes["C3"], scene_C4=scenes["C4"],
            out_root=tmp, name="calib1", freeze="", extra_args=[])
        check("fake calib campaign completes",
              run_campaign(calib, run_fn=camp_fake) == 0)
        with open(os.path.join(tmp, "calib1", "campaign.json")) as f:
            camp = json.load(f)
        check("fake calib 33 stages staged",
              len(camp["stages"]) == 33 and camp["complete"] is True)
        # The campaign seals its own freeze with the full calibration
        # evidence; downstream acceptance tests consume THIS sealed
        # file (a trials-only freeze can never unlock acceptance).
        sealed = os.path.join(tmp, "calib1", "frozen-edges.json")
        with open(sealed) as f:
            sealed_manifest = json.load(f)
        with open(seal_path_for(sealed)) as f:
            sealed_sidecar = json.load(f)
        check("fake calib freeze sealed",
              sealed_sidecar.get("sealed") is True and
              sealed_sidecar.get("freeze_sha256") == sha256_file(sealed))
        check("fake sealed edges match fixture assumption",
              sealed_manifest["edgesRT"] == build_edges(8192, 16384) and
              len(sealed_manifest["edgesRT"]) == 226)
        sealed_birt = _bisect_campaign.bisect_right(
            sealed_manifest["edgesRT"], 10000) - 1
        check("fake sealed assumption consistent", sealed_birt == birt_c)
        check("fake calibration revalidation baseline passes",
              revalidate_calibration(sealed_manifest, sealed, exe) == "")
        c4_env_file = os.path.join(tmp, "fake-c4.exr")
        with open(c4_env_file, "rb") as f:
            c4_env_bytes = f.read()
        with open(c4_env_file, "wb") as f:
            f.write(c4_env_bytes + b"tampered")
        check("fake C4 external env byte tamper rejects sealed calibration",
              revalidate_calibration(sealed_manifest, sealed, exe) != "")
        with open(c4_env_file, "wb") as f:
            f.write(c4_env_bytes)
        check("fake C4 external env restored revalidates",
              revalidate_calibration(sealed_manifest, sealed, exe) == "")
        os.rename(c4_env_file, c4_env_file + ".held")
        missing_calib = argparse.Namespace(**{**vars(calib),
            "name": "calib-missing-c4-env"})
        try:
            check("fake campaign missing C4 saved EXR fails before launch",
                  run_campaign(missing_calib, run_fn=camp_fake) == 1 and
                  not os.path.exists(os.path.join(
                      tmp, missing_calib.name, "campaign.json")))
        finally:
            os.rename(c4_env_file + ".held", c4_env_file)
        foreign_scene = os.path.join(tmp, "foreign-c4.rt2scene")
        with open(foreign_scene, "w") as f:
            json.dump({"envMap": {"path": "fake-c4.exr",
                                  "assetId": "foreign-id"}}, f)
        with open(c4_env_file + ".rt2meta", "w") as f:
            f.write("actual-id\n")
        foreign_calib = argparse.Namespace(**{**vars(calib),
            "name": "calib-foreign-c4-env", "scene_C4": foreign_scene})
        check("fake campaign foreign C4 sidecar ID fails before launch",
              run_campaign(foreign_calib, run_fn=camp_fake) == 1 and
              not os.path.exists(os.path.join(
                  tmp, foreign_calib.name, "campaign.json")))
        os.remove(c4_env_file + ".rt2meta")
        c4_capture = argparse.Namespace(**{**vars(mkargs("c4-no-load")),
            "scene": scenes["C4"], "diag": False, "edges": "",
            "config": "C4", "fixture_config": "C4", "extra_args":
            WORKLOADS["C4"]["extra"]})

        def c4_no_load_fake(e, c, w, l, t=0):
            camp_fake(e, c, w, l, t)
            with open(l) as f:
                lines = f.readlines()
            with open(l, "w") as f:
                f.writelines(line for line in lines
                             if not line.startswith("[Scene] Env map:"))
            return 0

        check("fake C4 load failure cannot pass with scene label alone",
              run_capture(c4_capture, run_fn=c4_no_load_fake) == 1)
        copied_calib_scene = os.path.join(tmp, "copied-calib-C1.rt2scene")
        shutil.copyfile(scenes["C1"], copied_calib_scene)
        for role in ("confirm-C1", "off-C1"):
            prov_path = os.path.join(tmp, "calib1", role,
                                     "provenance.json")
            with open(prov_path, "rb") as f:
                original_prov = f.read()
            try:
                changed = json.loads(original_prov)
                changed["scene"] = copied_calib_scene
                with open(prov_path, "w") as f:
                    json.dump(changed, f)
                check("fake %s same-bytes scene path rejected" % role,
                      revalidate_calibration(
                          sealed_manifest, sealed, exe) != "")
                if role == "confirm-C1":
                    confirm_args = argparse.Namespace(
                        run_dirs=[os.path.join(tmp, "calib1", role)],
                        manifest=sealed, config="C1", ref_exe=exe)
                    check("fake check-freeze scene path rejected",
                          cmd_check_freeze(confirm_args) == 1)
            finally:
                with open(prov_path, "wb") as f:
                    f.write(original_prov)
            check("fake %s scene path control restored" % role,
                  revalidate_calibration(sealed_manifest, sealed, exe) == "")
        confirm_dir = os.path.join(tmp, "calib1", "confirm-C1")
        completion_path = os.path.join(confirm_dir, "completion.json")
        confirm_frame = os.path.join(confirm_dir, "frame.png")
        with open(completion_path, "rb") as f:
            original_completion = f.read()
        with open(confirm_frame, "rb") as f:
            original_frame = f.read()
        try:
            changed = json.loads(original_completion)
            del changed["frame_sha256"]
            with open(completion_path, "w") as f:
                json.dump(changed, f)
            with open(confirm_frame, "wb") as f:
                f.write(b"modified frame without digest")
            check("fake confirm missing digest and changed frame rejected",
                  revalidate_calibration(sealed_manifest, sealed, exe) != "")
        finally:
            with open(completion_path, "wb") as f:
                f.write(original_completion)
            with open(confirm_frame, "wb") as f:
                f.write(original_frame)
        check("fake confirm frame evidence control restored",
              revalidate_calibration(sealed_manifest, sealed, exe) == "")
        # Legacy parity is byte identity: corrupt one legacy frame and
        # the parity gate must fail (fail-closed, never silent) — then
        # RESTORE the evidence byte-for-byte (acceptance revalidates
        # the same calibration evidence below).
        legacy_frame = os.path.join(tmp, "calib1", "legacy-C1", "frame.png")
        with open(legacy_frame, "rb") as pf:
            legacy_bytes = pf.read()
        with open(legacy_frame, "wb") as pf:
            pf.write(b"tampered!")
        check("fake legacy parity detects tamper",
              check_legacy_parity(
                  os.path.join(tmp, "calib1", "off-C1"),
                  os.path.join(tmp, "calib1", "legacy-C1")) != "complete")
        with open(legacy_frame, "wb") as pf:
            pf.write(legacy_bytes)
        check("fake legacy evidence restored",
              check_legacy_parity(
                  os.path.join(tmp, "calib1", "off-C1"),
                  os.path.join(tmp, "calib1", "legacy-C1")) == "complete")


        # check-freeze pass + fail paths on an explicit confirmation
        # rundir (full measured window, sealed provenance).
        cdir = os.path.join(tmp, "confirm")
        os.makedirs(cdir)
        from PIL import Image as _CfmImage
        _CfmImage.new("RGB", (8, 8), (10, 20, 30)).save(
            os.path.join(cdir, "frame.png"))
        import bisect
        er, eg = (sealed_manifest["edgesRT"], sealed_manifest["edgesGI"])
        birt = bisect.bisect_right(er, 10000) - 1
        big = bisect.bisect_right(eg, 10000) - 1

        def confirm_line(fr):
            cl = {"schema": SCHEMA, "frame": fr, "slot": 0, "serial": fr,
                  "valid": True,
                  "head": [0x53484457, 1, fr, 1, 226, 226, 1, 0],
                  "data": [0] * 1196}
            # Mass inside narrow bins of the frozen edges (bin of [9984,10112)).
            cl["data"][0 + 0 * 298 + 34 + birt] = 1000
            cl["data"][0 + 0 * 298 + 260] = 1000
            cl["data"][0 + 0 * 298 + 261] = 1000
            cl["data"][0 + 1 * 298 + 34 + big] = 1000
            cl["data"][0 + 1 * 298 + 260] = 1000
            cl["data"][0 + 1 * 298 + 261] = 1000
            # C3 strata: RT tri primary/recursive + punctual, GI tri fresh/replay.
            cl["data"][0 + 0 * 298 + 264 + 1 * 2 + 0] = 120
            cl["data"][0 + 0 * 298 + 264 + 1 * 2 + 1] = 110
            cl["data"][0 + 0 * 298 + 264 + 4 * 2 + 0] = 100
            cl["data"][0 + 0 * 298 + 264 + 4 * 2 + 1] = 100
            cl["data"][0 + 1 * 298 + 264 + 0 * 2 + 0] = 110
            cl["data"][0 + 1 * 298 + 264 + 0 * 2 + 1] = 105
            return cl

        with open(os.path.join(cdir, "run.log"), "w") as f:
            f.write("[NGX] supported=yes backend=none (fake)\n")
            for fr in range(41, 61):
                f.write("[ShadowTail] %s\n" % json.dumps(confirm_line(fr)))
        fake_native(cdir)
        shutil.copyfile(os.path.join(cdir, "rt2_log.txt"),
                        os.path.join(cdir, "native-rt2.log"))
        with open(os.path.join(cdir, "completion.json"), "w") as f:
            json.dump({"complete": True,
                       "frame_sha256": sha256_file(
                           os.path.join(cdir, "frame.png")),
                       "ngx_effective": "[NGX] fake",
                       "denoiser_effective": "[Denoiser] submitted=Off backend=none",
                       "native_log_sha256": sha256_file(
                           os.path.join(cdir, "native-rt2.log"))}, f)
        with open(os.path.join(cdir, "provenance.json"), "w") as f:
            json.dump({"exe": os.path.abspath(exe),
                       "exe_sha256": sha256_file(exe),
                       "scene": os.path.abspath(scenes["C3"]),
                       "scene_sha256": sha256_file(scenes["C3"]),
                       "frames": 60, "config": "C3",
                       "workload": WORKLOADS["C3"],
                       "edges_sha256": sha256_file(sealed)}, f)
        ca = argparse.Namespace(run_dirs=[cdir], manifest=sealed,
                               config="C3", ref_exe=exe)
        check("fake check-freeze passes", cmd_check_freeze(ca) == 0)
        ovf = confirm_line(45)
        ovf["data"][0 + 0 * 298 + 34 + 225] = 20000  # all overflow
        ovf["data"][0 + 0 * 298 + 34 + birt] = 0
        with open(os.path.join(cdir, "run.log"), "w") as f:
            f.write("[NGX] supported=yes backend=none (fake)\n")
            f.write("[Denoiser] submitted=Off backend=none (fake)\n")
            for fr in range(41, 61):
                o = confirm_line(fr) if fr != 45 else ovf
                f.write("[ShadowTail] %s\n" % json.dumps(o))
        check("fake check-freeze overflow fails",
              cmd_check_freeze(ca) == 1)

        # Quartet happy path (fake, instant) + deadline INCOMPLETE.
        qa = argparse.Namespace(
            ref_exe=exe, cand_exe=cand_exe, scene=scene, out_root=tmp,
            name="b1", frames=60, spp=1, width=64, height=64,
            edges=mf, control=False, config="", extra_args=[])
        check("fake quartet complete",
              run_quartet(qa, run_fn=lambda e, c, w, l, t=0: fake_run(
                  e, c, w, l, lambda: snap_lines())) == 0)
        # Budget kill maps to INCOMPLETE (gate undecided at the cap),
        # never to a failure verdict or a silent pass.
        check("fake capture timeout incomplete",
              run_capture(mkargs("tmout"),
                          run_fn=lambda e, c, w, l, t=None: 124) == 2)
        qa_to = argparse.Namespace(
            ref_exe=exe, cand_exe=cand_exe, scene=scene, out_root=tmp,
            name="btimeout", frames=60, spp=1, width=64, height=64,
            edges=mf, control=False, config="", extra_args=[])
        check("fake quartet timeout leg incomplete",
              run_quartet(qa_to,
                          run_fn=lambda e, c, w, l, t=None: 124) == 2)

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
                  run_quartet(qa2, run_fn=lambda e, c, w, l, t=0: fake_run(
                      e, c, w, l, lambda: snap_lines()),
                      deadline_min=1) == 2)
        finally:
            time.monotonic = real_mono

        # Analyze: fake 4-block campaign (C1, identical R/C → PASS).
        # Bin placement follows the SEALED calibration edges (same
        # frozen formula the campaign derived).
        from PIL import Image as _PILImage
        adir = os.path.join(tmp, "campaign")
        os.makedirs(adir)
        er = sealed_manifest["edgesRT"]
        birt_a = bisect.bisect_right(er, 10000) - 1

        def make_block(bname, slow_cand=False, empty_tail=False,
                       cfg="C1", gi_mass=False, gi_timings=True):
            # cfg selects the declared workload seal + GI requirements;
            # gi_mass fills family-1 histograms (needed for C3/C4
            # minimums); gi_timings controls GI Temporal records.
            # Distinct declared paths have equal binary/SPV hashes;
            # role authority is the path plus current bytes.
            bdir = os.path.join(adir, bname)
            os.makedirs(bdir)
            for sub, shift in (("R_ON", 0), ("R_OFF", 0),
                               ("C_ON", 1 if slow_cand else 0),
                               ("C_OFF", 0)):
                role_exe = cand_exe if sub.startswith("C_") else exe
                sdir = os.path.join(bdir, sub)
                fake_run_id = uuid.uuid5(uuid.NAMESPACE_URL, sdir).hex
                os.makedirs(sdir)
                lines = []
                lines.append("[NGX] supported=yes backend=none (fake)")
                lines.append("[Denoiser] submitted=Off backend=none (fake)")
                for f in range(41, 61):
                    s = {"schema": SCHEMA, "frame": f, "slot": 0,
                         "serial": f, "valid": True,
                         "head": [0x53484457, 1, f, 1, 226, 226, 1, 0],
                         "data": [0] * 1196}
                    b = birt_a + (20 if (shift and sub == "C_ON") else 0)
                    if not empty_tail:
                        s["data"][0 + 0 * 298 + 34 + b] = 1000
                        s["data"][0 + 0 * 298 + 260] = 1000
                        s["data"][0 + 0 * 298 + 261] = 1000
                        if gi_mass:
                            s["data"][0 + 1 * 298 + 34 + b] = 1000
                            s["data"][0 + 1 * 298 + 260] = 1000
                            s["data"][0 + 1 * 298 + 261] = 1000
                            s["data"][0 + 1 * 298 + 264] = 120
                            s["data"][0 + 1 * 298 + 265] = 110
                    else:
                        # Structurally valid but zero-mass: the analyzer
                        # must return INCONCLUSIVE, never raise TypeError.
                        s["data"][0 + 0 * 298 + 260] = 0
                        s["data"][0 + 0 * 298 + 261] = 0
                    s["data"][0 + 0 * 298 + 264 + 1 * 2 + 0] = 600
                    s["data"][0 + 0 * 298 + 264 + 1 * 2 + 1] = 550
                    s["data"][0 + 0 * 298 + 264 + 4 * 2 + 0] = 700
                    s["data"][0 + 0 * 298 + 264 + 4 * 2 + 1] = 650
                    if sub.endswith("_ON"):
                        lines.append("[ShadowTail] %s" % json.dumps(s))
                    regions = '"RT Shading":0.5,'
                    if gi_timings:
                        regions += '"ReSTIR GI Temporal":0.3,'
                    regions += '"GPU Frame":1.0'
                    lines.append('[HeadlessTiming] {"frame":%d,'
                                 '"regions_ms":{%s}}' % (f, regions))
                with open(os.path.join(sdir, "run.log"), "w") as lf:
                    lf.write("\n".join(lines) + "\n")
                fake_native(sdir)
                shutil.copyfile(os.path.join(sdir, "rt2_log.txt"),
                                os.path.join(sdir, "native-rt2.log"))
                with open(os.path.join(sdir, "completion.json"), "w") as cf:
                    json.dump({"complete": True, "run_id": fake_run_id,
                               "ngx_effective": "[NGX] fake",
                               "denoiser_effective": "[Denoiser] submitted=Off backend=none",
                               "native_log_sha256": sha256_file(
                                   os.path.join(sdir, "native-rt2.log"))}, cf)
                with open(os.path.join(sdir, "provenance.json"), "w") as pf:
                    json.dump({"run_id": fake_run_id,
                               "exe": os.path.abspath(role_exe),
                               "exe_sha256": sha256_file(role_exe),
                               "scene": os.path.abspath(scenes[cfg]),
                                "scene_sha256": sha256_file(scenes[cfg]),
                                "environment": scene_environment(
                                    scenes[cfg], cfg, WORKLOADS[cfg]["extra"]),
                               "frames": 60, "diag": sub.endswith("_ON"),
                               "control": False,
                               "spp": WORKLOADS[cfg]["spp"],
                               "width": WORKLOADS[cfg]["width"],
                               "height": WORKLOADS[cfg]["height"],
                               "extra": WORKLOADS[cfg]["extra"] +
                               ["--benchmark-timings"],
                               "config": cfg if sub.endswith("_ON") else "",
                               "workload": WORKLOADS[cfg],
                               "spvs": hash_exe_spvs(role_exe),
                               "edges_sha256": sha256_file(sealed)}, pf)
                _PILImage.new("RGB", (8, 8), (10, 20, 30)).save(
                    os.path.join(sdir, "frame.png"))
                with open(os.path.join(sdir, "completion.json")) as cf:
                    completed = json.load(cf)
                completed["frame_sha256"] = sha256_file(
                    os.path.join(sdir, "frame.png"))
                with open(os.path.join(sdir, "completion.json"), "w") as cf:
                    json.dump(completed, cf)
                with open(os.path.join(sdir, "command.txt"), "w") as cf:
                    cf.write("%s --output-display %s --frames 60\n" %
                             (role_exe, os.path.join(sdir, "frame.png")))
            with open(os.path.join(bdir, "block.json"), "w") as bf:
                json.dump({"block": "block-%s-%s" % (cfg, bname[1:]),
                           "complete": True, "results": {
                    "R_ON": "complete", "R_OFF": "complete",
                    "C_ON": "complete", "C_OFF": "complete",
                    "R_timing_parity": "complete",
                    "C_timing_parity": "complete",
                    "R_image_parity": "complete",
                    "C_image_parity": "complete",
                    "uniformity": "complete",
                    "executable_identity": "complete"},
                    "authorities": {
                        "ref": exe_authority(exe),
                        "cand": exe_authority(cand_exe)}}, bf)

        for bname in ("k1", "k2", "k3", "k4"):
            make_block(bname)
        ana = argparse.Namespace(
            block_dirs=[os.path.join(adir, b) for b in
                        ("k1", "k2", "k3", "k4")],
            manifest=sealed, ref_exe=exe, cand_exe=cand_exe, config="C1")
        check("fake analyze passes", cmd_analyze(ana) == 0)
        same_block = argparse.Namespace(**{
            **vars(ana), "block_dirs": [ana.block_dirs[1]] * 4})
        check("fake analyze one completed block four times rejected",
              cmd_analyze(same_block) == 1)
        alias_blocks = argparse.Namespace(**{
            **vars(ana), "block_dirs": [
                ana.block_dirs[0], ana.block_dirs[1], ana.block_dirs[2],
                os.path.join(adir, "k1", "..", "k1")]})
        check("fake analyze realpath block alias rejected",
              cmd_analyze(alias_blocks) == 1)
        reused_leg = os.path.join(adir, "k4", "R_ON")
        saved_leg = reused_leg + ".saved"
        os.rename(reused_leg, saved_leg)
        try:
            shutil.copytree(os.path.join(adir, "k1", "R_ON"), reused_leg)
            check("fake analyze copied leg evidence rejected",
                  cmd_analyze(ana) == 1)
        finally:
            shutil.rmtree(reused_leg)
            os.rename(saved_leg, reused_leg)
        check("fake analyze distinct leg evidence restored",
              cmd_analyze(ana) == 0)
        with open(os.path.join(adir, "k1", "R_ON",
                               "provenance.json")) as f:
            reused_id = json.load(f)["run_id"]
        id_files = [os.path.join(adir, "k4", "R_ON", name)
                    for name in ("provenance.json", "completion.json")]
        id_originals = []
        for path in id_files:
            with open(path, "rb") as f:
                id_originals.append(f.read())
        try:
            for path, original in zip(id_files, id_originals):
                changed = json.loads(original)
                changed["run_id"] = reused_id
                with open(path, "w") as f:
                    json.dump(changed, f)
            check("fake analyze reused capture ID rejected",
                  cmd_analyze(ana) == 1)
        finally:
            for path, original in zip(id_files, id_originals):
                with open(path, "wb") as f:
                    f.write(original)
        check("fake analyze capture IDs restored", cmd_analyze(ana) == 0)
        block_id_path = os.path.join(adir, "k4", "block.json")
        with open(block_id_path, "rb") as f:
            original_block = f.read()
        try:
            duplicate_id = json.loads(original_block)
            duplicate_id["block"] = "block-C1-1"
            with open(block_id_path, "w") as f:
                json.dump(duplicate_id, f)
            check("fake analyze duplicate canonical block ID rejected",
                  cmd_analyze(ana) == 1)
        finally:
            with open(block_id_path, "wb") as f:
                f.write(original_block)
        with open(completion_path, "rb") as f:
            original_completion = f.read()
        with open(confirm_frame, "rb") as f:
            original_frame = f.read()
        try:
            changed = json.loads(original_completion)
            del changed["frame_sha256"]
            with open(completion_path, "w") as f:
                json.dump(changed, f)
            with open(confirm_frame, "wb") as f:
                f.write(b"modified frame without digest")
            check("fake analyze rejects missing calibration frame digest",
                  cmd_analyze(ana) == 1)
        finally:
            with open(completion_path, "wb") as f:
                f.write(original_completion)
            with open(confirm_frame, "wb") as f:
                f.write(original_frame)
        check("fake analyze calibration evidence restored",
              cmd_analyze(ana) == 0)
        check("fake analyze same-path roles rejected",
              cmd_analyze(argparse.Namespace(
                  **{**vars(ana), "cand_exe": exe})) == 1)

        def tamper_analyze(sub, field, value, label):
            path = os.path.join(adir, "k1", sub, "provenance.json")
            with open(path, "rb") as f:
                original = f.read()
            try:
                prov = json.loads(original)
                prov[field] = value
                with open(path, "w") as f:
                    json.dump(prov, f)
                check(label, cmd_analyze(ana) == 1)
            finally:
                with open(path, "wb") as f:
                    f.write(original)

        tamper_analyze("R_ON", "exe", cand_exe,
                       "fake analyze swapped R/C binary rejected")
        tamper_analyze("C_ON", "exe_sha256", "0" * 64,
                       "fake analyze wrong candidate hash rejected")
        bad_spvs = hash_exe_spvs(cand_exe)
        bad_spvs["closesthit_tail.spv"] = "0" * 64
        tamper_analyze("C_ON", "spvs", bad_spvs,
                       "fake analyze wrong candidate SPV rejected")
        tamper_analyze("R_OFF", "workload",
                       dict(WORKLOADS["C1"], spp=1),
                       "fake analyze mixed OFF workload rejected")
        tamper_analyze("C_OFF", "spp", 1,
                       "fake analyze top-level capture settings rejected")
        tamper_analyze("C_ON", "diag", False,
                       "fake analyze wrong ON mode rejected")
        tamper_analyze("C_ON", "control", True,
                       "fake analyze control-mode leak rejected")
        tamper_analyze("C_OFF", "frames", 12,
                       "fake analyze short OFF run rejected")
        copied_scene = os.path.join(tmp, "copied-C1.rt2scene")
        shutil.copyfile(scenes["C1"], copied_scene)
        tamper_analyze("C_ON", "scene", copied_scene,
                       "fake analyze same-bytes wrong fixture path rejected")
        window_log = os.path.join(adir, "k1", "R_ON", "run.log")
        with open(window_log, "rb") as f:
            window_original = f.read()
        try:
            lines = window_original.decode("utf-8").splitlines(True)
            lines = [line for line in lines if not (
                line.startswith("[ShadowTail]") and
                '"frame": 55,' in line)]
            with open(window_log, "w") as f:
                f.writelines(lines)
            check("fake analyze incomplete measured window rejected",
                  cmd_analyze(ana) == 1)
        finally:
            with open(window_log, "wb") as f:
                f.write(window_original)
        off_log = os.path.join(adir, "k1", "R_OFF", "run.log")
        with open(off_log, "rb") as f:
            off_original = f.read()
        try:
            with open(off_log, "ab") as f:
                f.write(("[ShadowTail] %s\n" % json.dumps(
                    confirm_line(41))).encode("utf-8"))
            check("fake analyze OFF snapshot leak rejected",
                  cmd_analyze(ana) == 1)
        finally:
            with open(off_log, "wb") as f:
                f.write(off_original)
        frame_path = os.path.join(adir, "k1", "R_ON", "frame.png")
        with open(frame_path, "rb") as f:
            frame_original = f.read()
        try:
            with open(frame_path, "wb") as f:
                f.write(b"tampered image")
            check("fake analyze changed frame rejected", cmd_analyze(ana) == 1)
        finally:
            with open(frame_path, "wb") as f:
                f.write(frame_original)
        check("fake quartet same-path roles rejected",
              run_quartet(argparse.Namespace(
                  ref_exe=exe, cand_exe=exe, scene=scene, out_root=tmp,
                  name="same-roles", frames=60, spp=1, width=64, height=64,
                  edges=mf, control=False, config="", extra_args=[]),
                  run_fn=camp_fake) == 1)
        for bname in ("j1", "j2", "j3", "j4"):
            make_block(bname, slow_cand=True)
        ana2 = argparse.Namespace(
            block_dirs=[os.path.join(adir, b) for b in
                        ("j1", "j2", "j3", "j4")],
            manifest=sealed, ref_exe=exe, cand_exe=cand_exe, config="C1")
        check("fake analyze slow-candidate fails",
              cmd_analyze(ana2) == 1)
        ana3 = argparse.Namespace(
            block_dirs=[os.path.join(adir, b) for b in ("k1", "k2", "k3")],
            manifest=sealed, ref_exe=exe, cand_exe=cand_exe, config="C1")
        check("fake analyze 3-block inconclusive",
              cmd_analyze(ana3) == 2)
        # Empty-tail campaign (Sol round-3 repro): structurally valid
        # but zero-mass snapshots must yield INCONCLUSIVE exit 2 —
        # never TypeError from unpacking a missing p95 bracket.
        for bname in ("e1", "e2", "e3", "e4"):
            make_block(bname, empty_tail=True)
        ana4 = argparse.Namespace(
            block_dirs=[os.path.join(adir, b) for b in
                        ("e1", "e2", "e3", "e4")],
            manifest=sealed, ref_exe=exe, cand_exe=cand_exe, config="C1")
        try:
            rc_empty = cmd_analyze(ana4)
        except TypeError as e:
            print("FAIL - empty-tail analyze raised TypeError: %s" % e)
            rc_empty = -1
        check("fake analyze empty-tail inconclusive (no TypeError)",
              rc_empty == 2)
        # Mixed scene identity across blocks fails closed (never pooled).
        with open(os.path.join(adir, "k1", "R_ON",
                               "provenance.json")) as pf:
            k1prov = json.load(pf)
        k1prov["scene_sha256"] = "0" * 64
        with open(os.path.join(adir, "k1", "R_ON",
                               "provenance.json"), "w") as pf:
            json.dump(k1prov, pf)
        check("fake analyze mixed identity fails",
              cmd_analyze(ana) == 1)
        k1prov["scene_sha256"] = sha256_file(scenes["C1"])
        with open(os.path.join(adir, "k1", "R_ON",
                               "provenance.json"), "w") as pf:
            json.dump(k1prov, pf)

        # Quartet WITH config: OFF legs must skip coverage (they carry
        # none by design) while ON legs enforce it. snap_lines carries
        # C1 strata; C1 ON legs pass, OFF legs pass without coverage.
        def c1_lines():
            return snap_lines()
        qa3 = argparse.Namespace(
            ref_exe=exe, cand_exe=cand_exe, scene=scene, out_root=tmp,
            name="b3", frames=60, spp=1, width=64, height=64,
            edges=mf, control=False, config="C1", extra_args=[])
        # good[] carries C1 RT tri/punctual strata (>=50 each) — reuse it.
        check("fake quartet with config passes OFF legs",
              run_quartet(qa3, run_fn=lambda e, c, w, l, t=0: fake_run(
                  e, c, w, l, c1_lines)) == 0)

        # Acceptance runs against the SEALED calibration freeze from
        # the campaign above (same fixtures, same reference binary).
        # No separate pilot-only freeze is derived for acceptance.
        accept = argparse.Namespace(
            mode="accept", ref_exe=exe, cand_exe=cand_exe, legacy_exe="",
            scene_C1=scenes["C1"], scene_C2=scenes["C2"],
            scene_C3=scenes["C3"], scene_C4=scenes["C4"],
            out_root=tmp, name="accept1", freeze=sealed)
        check("fake accept campaign completes",
              run_campaign(accept, run_fn=camp_fake) == 0)
        with open(os.path.join(tmp, "accept1", "campaign.json")) as f:
            acc = json.load(f)
        check("fake accept 64 runs + 4 analyzes staged",
              len(acc["stages"]) == 20 and acc["complete"] is True)
        with open(os.path.join(tmp, "accept1",
                               "accept-authorities.json")) as f:
            frozen_accept_auth = json.load(f)
        check("fake accept persisted one whole-campaign authority",
              acc.get("authorities") == frozen_accept_auth and
              acc.get("authority_sha256") == sha256_file(
                  os.path.join(tmp, "accept1",
                               "accept-authorities.json")))
        persisted_analyze = argparse.Namespace(
            block_dirs=[os.path.join(tmp, "accept1",
                                     "block-C1-%d" % k)
                        for k in range(1, 5)],
            manifest=sealed, config="C1", ref_exe=exe,
            cand_exe=cand_exe)
        check("fake persisted campaign analysis authority passes",
              cmd_analyze(persisted_analyze) == 0)
        authority_path = os.path.join(tmp, "accept1",
                                      "accept-authorities.json")
        with open(authority_path, "rb") as f:
            original_authority_file = f.read()
        try:
            tampered_authority = json.loads(original_authority_file)
            tampered_authority["cand"]["sha256"] = "0" * 64
            with open(authority_path, "w") as f:
                json.dump(tampered_authority, f)
            check("fake persisted campaign authority tamper rejected",
                  cmd_analyze(persisted_analyze) == 1)
        finally:
            with open(authority_path, "wb") as f:
                f.write(original_authority_file)
        check("fake persisted campaign authority restored",
              cmd_analyze(persisted_analyze) == 0)
        for kind, target in (("exe", cand_exe),
                             ("spv", os.path.join(
                                 cand_dir, "closesthit_tail.spv"))):
            with open(target, "rb") as f:
                original_target = f.read()
            original_analyze = cmd_analyze
            mutated = [False]

            def change_after_c1(an):
                rc = original_analyze(an)
                if an.config == "C1" and rc == 0 and not mutated[0]:
                    with open(target, "wb") as f:
                        f.write(original_target + b"-changed-between-configs")
                    mutated[0] = True
                return rc

            globals()["cmd_analyze"] = change_after_c1
            changed_name = "accept-changed-" + kind
            try:
                changed_accept = argparse.Namespace(
                    **{**vars(accept), "name": changed_name})
                changed_rc = run_campaign(changed_accept, run_fn=camp_fake)
            finally:
                globals()["cmd_analyze"] = original_analyze
                with open(target, "wb") as f:
                    f.write(original_target)
            with open(os.path.join(tmp, changed_name,
                                   "campaign.json")) as f:
                changed_campaign = json.load(f)
            check("fake accept %s swap between configs fails" % kind,
                  mutated[0] and changed_rc != 0 and
                  changed_campaign.get("complete") is False and
                  changed_campaign.get("authorities") == frozen_accept_auth)
        check("fake accept authority restored after swaps",
              exe_authority(cand_exe) == frozen_accept_auth["cand"])
        off_prov_path = os.path.join(tmp, "calib1", "off-C1",
                                     "provenance.json")
        with open(off_prov_path, "rb") as f:
            off_original = f.read()
        try:
            changed = json.loads(off_original)
            changed["scene"] = copied_calib_scene
            with open(off_prov_path, "w") as f:
                json.dump(changed, f)
            wrong_fixture_accept = argparse.Namespace(
                **{**vars(accept), "name": "accept-wrong-calib-path"})
            check("fake accept rejects same-bytes wrong calibration path",
                  run_campaign(wrong_fixture_accept, run_fn=camp_fake) == 1)
        finally:
            with open(off_prov_path, "wb") as f:
                f.write(off_original)
        check("fake accept calibration path control restored",
              revalidate_calibration(sealed_manifest, sealed, exe) == "")
        accept_same = argparse.Namespace(
            **{**vars(accept), "name": "accept-same-path", "cand_exe": exe})
        check("fake accept same-path roles rejected",
              run_campaign(accept_same, run_fn=camp_fake) == 1)
        # Accept with a pilot-only freeze (mout: trials but no
        # calibration seal, different fixtures) must fail — a valid
        # trial-derived Q alone does not certify the calibration.
        accept_foreign = argparse.Namespace(
            mode="accept", ref_exe=exe, cand_exe=cand_exe, legacy_exe="",
            scene_C1=scenes["C1"], scene_C2=scenes["C2"],
            scene_C3=scenes["C3"], scene_C4=scenes["C4"],
            out_root=tmp, name="accept-foreign", freeze=mout)
        check("fake accept pilot-only freeze rejected",
              run_campaign(accept_foreign, run_fn=camp_fake) != 0)

        # --- Round-5 adversarial cases (each review repro, actual paths) ---
        # C2 environment: relative EXR resolved from the invocation
        # directory, hashed in provenance, Loaded evidence required.
        tiny_abs = os.path.join(repo_root, TINY_ENV)
        tiny_rel = os.path.relpath(tiny_abs, os.getcwd())
        c2wl = WORKLOADS["C2"]
        a_env = argparse.Namespace(
            exe=exe, scene=scene, out_root=tmp, name="c2env",
            frames=60, spp=c2wl["spp"], width=c2wl["width"],
            height=c2wl["height"], edges=mf, diag=True, control=False,
            config="", extra_args=["--raster-first", "--restir",
                                   "--env", tiny_rel])
        check("fake C2 env relative path loads",
              run_capture(a_env, run_fn=lambda e, c, w, l, t=None: fake_run(
                  e, c, w, l, lambda: snap_lines())) == 0
              if os.path.exists(tiny_abs) else True)
        if os.path.exists(tiny_abs):
            with open(os.path.join(tmp, "c2env",
                                   "provenance.json")) as f:
                c2prov = json.load(f)
            check("fake C2 env hashed",
                  c2prov.get("env_sha256") == sha256_file(tiny_abs))
        else:
            check("fake C2 env hashed (no fixture from this CWD)", True)
        a_noenv = argparse.Namespace(
            exe=exe, scene=scene, out_root=tmp, name="c2noenv",
            frames=60, spp=1, width=64, height=64, edges=mf, diag=True,
            control=False, config="",
            extra_args=["--restir", "--env",
                        os.path.join(tmp, "no-such-env.exr")])
        check("fake missing env file fails",
              run_capture(a_noenv, run_fn=lambda e, c, w, l, t=None: fake_run(
                  e, c, w, l, lambda: snap_lines())) == 1)
        a_noev = argparse.Namespace(
            exe=exe, scene=scene, out_root=tmp, name="c2noev",
            frames=60, spp=c2wl["spp"], width=c2wl["width"],
            height=c2wl["height"], edges=mf, diag=True, control=False,
            config="", extra_args=["--raster-first", "--restir",
                                   "--env", tiny_rel])
        check("fake missing Loaded evidence fails",
              run_capture(a_noev, run_fn=lambda e, c, w, l, t=None: fake_run(
                  e, c, w, l, lambda: snap_lines(), env_lines=False)) == 1
              if os.path.exists(tiny_abs) else True)

        # --- Round-4 adversarial cases (each review repro, actual paths) ---
        fRT, fGI = (sealed_manifest["edgesRT"], sealed_manifest["edgesGI"])

        def write_runlog(parent, name, lines, prov_extra):
            rdir = os.path.join(parent, name)
            os.makedirs(rdir)
            with open(os.path.join(rdir, "run.log"), "w") as f:
                f.write("[NGX] supported=yes backend=none (fake)\n")
                f.write("[Denoiser] submitted=Off backend=none (fake)\n")
                for line in lines:
                    f.write(line)
            fake_native(rdir)
            shutil.copyfile(os.path.join(rdir, "rt2_log.txt"),
                            os.path.join(rdir, "native-rt2.log"))
            with open(os.path.join(rdir, "completion.json"), "w") as f:
                json.dump({"complete": True,
                           "ngx_effective": "[NGX] fake",
                           "denoiser_effective": "[Denoiser] fake"}, f)
            prov = {"exe_sha256": sha256_file(exe),
                    "scene_sha256": sha256_file(scene),
                    "frames": 60, "config": "C1",
                    "workload": WORKLOADS["C1"],
                    "edges_sha256": sha256_file(sealed)}
            prov.update(prov_extra)
            with open(os.path.join(rdir, "provenance.json"), "w") as f:
                json.dump(prov, f)
            _PILImage.new("RGB", (8, 8), (10, 20, 30)).save(
                os.path.join(rdir, "frame.png"))
            return rdir

        respdir = os.path.join(tmp, "response")
        os.makedirs(respdir)
        ord_dir = write_runlog(respdir, "ordinary", rich_valid_lines(), {})
        # Flat control (Sol round-4 repro): identical histograms must be
        # rejected as unresponsive — completion alone is not sensitivity.
        flat_dir = write_runlog(respdir, "flat",
                                rich_control_lines(shift=0), {})
        check("fake flat control rejected",
              check_control_response(flat_dir, ord_dir, "C1",
                                     (fRT, fGI)) != "")
        # Responsive control passes the same gate.
        resp_dir = write_runlog(respdir, "shifted",
                                rich_control_lines(shift=20), {})
        check("fake shifted control passes",
              check_control_response(resp_dir, ord_dir, "C1",
                                     (fRT, fGI)) == "")
        # Zero checksum: histogram shift without consumed work fails.
        nocheck_dir = write_runlog(respdir, "nochecksum",
                                   rich_control_lines(shift=20, checksum=0),
                                   {})
        check("fake zero-checksum control rejected",
              "checksum" in check_control_response(
                  nocheck_dir, ord_dir, "C1", (fRT, fGI)))
        # Unconfigured control (config bypass) fails coverage, not silence.
        check("fake control coverage enforced",
              check_coverage("C4", []) != [])

        # C3 analyze without GI Temporal records → INCONCLUSIVE (exit 2),
        # never a silent GI skip.
        for bname in ("g1", "g2", "g3", "g4"):
            make_block(bname, cfg="C3", gi_mass=True, gi_timings=False)
        ana_gi = argparse.Namespace(
            block_dirs=[os.path.join(adir, b) for b in
                        ("g1", "g2", "g3", "g4")],
            manifest=sealed, ref_exe=exe, cand_exe=cand_exe, config="C3")
        check("fake analyze missing-GI inconclusive",
              cmd_analyze(ana_gi) == 2)
        # C3 WITH GI records analyzes clean (gate is about evidence).
        for bname in ("h1", "h2", "h3", "h4"):
            make_block(bname, cfg="C3", gi_mass=True, gi_timings=True)
        ana_gi2 = argparse.Namespace(
            block_dirs=[os.path.join(adir, b) for b in
                        ("h1", "h2", "h3", "h4")],
            manifest=sealed, ref_exe=exe, cand_exe=cand_exe, config="C3")
        check("fake analyze C3 with GI passes", cmd_analyze(ana_gi2) == 0)

        # Workload-tampered provenance fails analysis (frozen seal).
        with open(os.path.join(adir, "k1", "R_ON",
                               "provenance.json")) as pf:
            k1prov = json.load(pf)
        k1prov["scene_sha256"] = sha256_file(scene)
        k1prov["workload"] = dict(WORKLOADS["C1"], spp=1)
        with open(os.path.join(adir, "k1", "R_ON",
                               "provenance.json"), "w") as pf:
            json.dump(k1prov, pf)
        check("fake analyze workload tamper fails",
              cmd_analyze(ana) == 1)

        # Hand-authored freeze (valid edges, no lineage) unlocks nothing.
        hand = os.path.join(tmp, "hand.json")
        with open(hand, "w") as f:
            json.dump({"edgesRT": list(fRT), "edgesGI": list(fGI)}, f)
        ana_hand = argparse.Namespace(
            block_dirs=[os.path.join(adir, b) for b in
                        ("k2", "k3", "k4",
                         "k1")],
            manifest=hand, ref_exe=exe, cand_exe=cand_exe, config="C1")
        check("fake analyze hand-authored freeze rejected",
              cmd_analyze(ana_hand) == 1)
        ca_hand = argparse.Namespace(run_dirs=[cdir], manifest=hand,
                                     config="C3", ref_exe=exe)
        check("fake check-freeze hand-authored freeze rejected",
              cmd_check_freeze(ca_hand) == 1)
        # Tampered edge values break the lineage edge seal.
        tam = os.path.join(tmp, "tampered-freeze.json")
        with open(sealed) as f:
            mcopy = json.load(f)
        mcopy["edgesRT"] = list(mcopy["edgesRT"])
        mcopy["edgesRT"][1] += 1
        with open(tam, "w") as f:
            json.dump(mcopy, f)
        check("freeze edge seal detects tamper",
              check_freeze_seal(mcopy) != "")

        # check-freeze binds the requested config: C3 evidence presented
        # as C1 must fail.
        ca_wrong = argparse.Namespace(run_dirs=[cdir], manifest=sealed,
                                      config="C1", ref_exe=exe)
        check("fake check-freeze wrong config rejected",
              cmd_check_freeze(ca_wrong) == 1)
        # derive rejects duplicate-config and acceptance-poisoned trials.
        dup_dirs = [write_trial(pdir, "D1", "C1", trial_scenes["C1"]),
                    write_trial(pdir, "D2", "C1", trial_scenes["C2"])]
        da_dup = argparse.Namespace(trial_dirs=dup_dirs, ref_exe=exe,
                                    out=os.path.join(tmp, "dup.json"))
        check("fake derive duplicate config rejected",
              cmd_derive_freeze(da_dup) == 1)
        # Wrong scene relations: C2/C3 split across distinct files, and
        # C1 sharing the adaptation file, both violate the prescribed
        # pairing and must fail closed.
        rel_dir = os.path.join(tmp, "rel-trials")
        os.makedirs(rel_dir)
        split_c2 = os.path.join(tmp, "split-c2.rt2scene")
        with open(split_c2, "w") as f:
            f.write('{"fixture": "split-c2"}')
        rel_dirs = [write_trial(rel_dir, "R1", "C1", trial_scenes["C1"]),
                    write_trial(rel_dir, "R2", "C2", split_c2),
                    write_trial(rel_dir, "R3", "C3", trial_scenes["C3"]),
                    write_trial(rel_dir, "R4", "C4", trial_scenes["C4"])]
        da_rel = argparse.Namespace(trial_dirs=rel_dirs, ref_exe=exe,
                                    out=os.path.join(tmp, "rel.json"))
        check("fake derive split C2/C3 rejected",
              cmd_derive_freeze(da_rel) == 1)
        shr_dirs = [write_trial(rel_dir, "S1", "C1", trial_scenes["C2"]),
                    write_trial(rel_dir, "S2", "C2", trial_scenes["C2"]),
                    write_trial(rel_dir, "S3", "C3", trial_scenes["C3"]),
                    write_trial(rel_dir, "S4", "C4", trial_scenes["C4"])]
        da_shr = argparse.Namespace(trial_dirs=shr_dirs, ref_exe=exe,
                                    out=os.path.join(tmp, "shr.json"))
        check("fake derive C1-sharing-adaptation rejected",
              cmd_derive_freeze(da_shr) == 1)

        # Over-cap campaign: the clock jumps 200 min mid-run — stages
        # after the jump are INCOMPLETE and the manifest records
        # over_cap (never complete:true past the budget).
        real_mono_oc = time.monotonic
        clock_oc = [0.0]

        def jump_fake(exe_, cmd, cwd, logpath, timeout_min=0):
            clock_oc[0] += 200.0 * 60
            return camp_fake(exe_, cmd, cwd, logpath, timeout_min)

        try:
            time.monotonic = lambda: clock_oc[0]
            oc = argparse.Namespace(
                mode="calib", ref_exe=exe, cand_exe="",
                legacy_exe=legacy_exe,
                scene_C1=scenes["C1"], scene_C2=scenes["C2"],
                scene_C3=scenes["C3"], scene_C4=scenes["C4"],
                out_root=tmp, name="overcap", freeze="")
            rc_oc = run_campaign(oc, run_fn=jump_fake)
        finally:
            time.monotonic = real_mono_oc
        check("fake over-cap campaign incomplete", rc_oc == 2)
        with open(os.path.join(tmp, "overcap", "campaign.json")) as f:
            ocm = json.load(f)
        check("fake over-cap recorded",
              ocm["complete"] is False and ocm.get("over_cap") is True)

        # Real subprocess timeout: the budget kill path executes (exit
        # 124), not just the fake clock above.
        import subprocess as _sp
        to_exe = _sp.sys.executable if hasattr(_sp, "sys") else sys.executable
        rc_to = default_run_fn(
            to_exe, [to_exe, "-c", "import time; time.sleep(60)"],
            tmp, os.path.join(tmp, "timeout.log"), timeout_min=0.01)
        check("real process timeout kills (124)", rc_to == 124)

        # Calibration headroom unit gates: 20% slower ON fails; missing
        # GI on C3 fails; missing GI on C1 is contract-exempt.
        hrdir = os.path.join(tmp, "headroom")
        os.makedirs(hrdir)

        def write_timed(name,legung_r, legung_gi=True, frames=None):
            rdir = os.path.join(hrdir, name)
            os.makedirs(rdir)
            with open(os.path.join(rdir, "run.log"), "w") as f:
                for fr in frames if frames is not None else range(41, 61):
                    regions = '"RT Shading":%s,' % legung_r
                    if legung_gi:
                        regions += '"ReSTIR GI Temporal":0.3,'
                    regions += '"GPU Frame":1.0'
                    f.write('[HeadlessTiming] {"frame":%d,'
                            '"regions_ms":{%s}}\n' % (fr, regions))
            return rdir

        on_ok = write_timed("on-ok", 0.5)
        off_ok = write_timed("off-ok", 0.5)
        check("headroom parity passes",
              check_calib_headroom(on_ok, off_ok, "C1") == "")
        on_slow = write_timed("on-slow", 0.6)
        check("headroom over-3pct fails",
              check_calib_headroom(on_slow, off_ok, "C1") != "")
        on_nogi = write_timed("on-nogi", 0.5, legung_gi=False)
        off_nogi = write_timed("off-nogi", 0.5, legung_gi=False)
        check("headroom C3 missing GI fails",
              check_calib_headroom(on_nogi, off_nogi, "C3") != "")
        check("headroom C1 GI absence exempt",
              check_calib_headroom(on_nogi, off_nogi, "C1") == "")
        # One-frame timing samples fail in the calibration path for
        # both RT (C1) and GI (C3) — medians over a 1/20 window prove
        # no 3% ceiling.
        on_thin = write_timed("on-thin", 0.5, frames=[41])
        off_thin = write_timed("off-thin", 0.5, frames=[41])
        check("headroom one-frame RT fails",
              check_calib_headroom(on_thin, off_thin, "C1") != "")
        check("headroom one-frame GI fails",
              check_calib_headroom(on_thin, off_thin, "C3") != "")

        # --- Round-5 adversarial cases (each review repro, actual paths) ---
        # Forged lineage: plausible strings, valid configs, recomputed
        # edge digest — but trial dirs that do not exist. The seal
        # arithmetic passes; source revalidation must fail.
        with open(sealed) as f:
            forged = json.load(f)
        forged["lineage"]["trials"] = {
            cfg: dict(rec, dir=os.path.join(tmp, "no-such-trial-%s" % cfg))
            for cfg, rec in forged["lineage"]["trials"].items()}
        forged_path = os.path.join(tmp, "forged.json")
        with open(forged_path, "w") as f:
            json.dump(forged, f)
        check("forged seal arithmetic passes (control)",
              check_freeze_seal(forged) == "")
        ana_forged = argparse.Namespace(
            block_dirs=[os.path.join(adir, b) for b in
                        ("k1", "k2", "k3", "k4")],
            manifest=forged_path, ref_exe=exe, cand_exe=cand_exe, config="C1")
        check("fake analyze forged lineage rejected",
              cmd_analyze(ana_forged) == 1)
        # Gutted trial: flip a real calibration trial's completion
        # after sealing → lineage revalidation fails; then restore.
        gut_path = os.path.join(tmp, "calib1", "trial-C1",
                                "completion.json")
        with open(gut_path) as f:
            gut_orig = f.read()
        with open(gut_path, "w") as f:
            json.dump({"complete": False,
                       "ngx_effective": "[NGX] fake",
                       "denoiser_effective": "[Denoiser] fake"}, f)
        check("fake check-freeze gutted trial rejected",
              cmd_check_freeze(ca) == 1)
        with open(gut_path, "w") as f:
            f.write(gut_orig)

        # --- Round-6 adversarial cases (each review repro, actual paths) ---
        # Sol's exact alias repro: completed campaign.json and freeze
        # UNCHANGED; sidecar offs/legacys relabeled to confirms. The
        # revalidator must fail — confirmation images equal themselves
        # and confirm/confirm timing ratio is 1, so without ownership
        # binding the relabeled runs would pass as OFF/legacy.
        sidecar_path = seal_path_for(sealed)
        with open(sidecar_path) as f:
            sidecar_orig = f.read()
        aliased = json.loads(sidecar_orig)
        for cfg in CONFIGS:
            aliased["offs"][cfg] = aliased["confirms"][cfg]
            aliased["legacys"][cfg] = aliased["confirms"][cfg]
        with open(sidecar_path, "w") as f:
            json.dump(aliased, f)
        with open(sealed) as f:
            sealed_for_alias = json.load(f)
        check("fake alias substitution rejected",
              revalidate_calibration(sealed_for_alias, sealed, exe) != "")
        ana_alias = argparse.Namespace(
            block_dirs=[os.path.join(adir, b) for b in
                        ("k1", "k2", "k3", "k4")],
            manifest=sealed, ref_exe=exe, cand_exe=cand_exe, config="C1")
        check("fake analyze aliased sidecar rejected",
              cmd_analyze(ana_alias) == 1)
        with open(sidecar_path, "w") as f:
            f.write(sidecar_orig)

        def mutate_json(path, transform):
            with open(path) as f:
                original = f.read()
            with open(path, "w") as f:
                json.dump(transform(json.loads(original)), f)
            return original

        def restore_file(path, original):
            with open(path, "w") as f:
                f.write(original)

        # OFF-as-ON: an OFF run carrying the diagnostic flag is not an
        # OFF run, even with passing timings and images.
        off2_prov = os.path.join(tmp, "calib1", "off-C2",
                                 "provenance.json")
        off2_orig = mutate_json(
            off2_prov, lambda p: dict(p, diag=True))
        check("fake OFF-as-ON rejected",
              revalidate_calibration(sealed_for_alias, sealed, exe) != "")
        restore_file(off2_prov, off2_orig)
        # Legacy exe swapped for the reference: not the designated
        # production executable.
        leg3_prov = os.path.join(tmp, "calib1", "legacy-C3",
                                 "provenance.json")
        leg3_orig = mutate_json(
            leg3_prov,
            lambda p: dict(p, exe_sha256=sha256_file(exe)))
        check("fake legacy-equals-reference rejected",
              revalidate_calibration(sealed_for_alias, sealed, exe) != "")
        restore_file(leg3_prov, leg3_orig)
        # Control mode flag cleared: not the intended mode-2 run.
        ctl_prov = os.path.join(tmp, "calib1", "control-RT",
                                "provenance.json")
        ctl_orig = mutate_json(
            ctl_prov, lambda p: dict(p, control=False))
        check("fake control-mode-cleared rejected",
              revalidate_calibration(sealed_for_alias, sealed, exe) != "")
        restore_file(ctl_prov, ctl_orig)
        # Missing completion record fails loudly, never as absence.
        leg4_comp = os.path.join(tmp, "calib1", "legacy-C4",
                                 "completion.json")
        with open(leg4_comp) as f:
            leg4_orig = f.read()
        os.remove(leg4_comp)
        check("fake missing completion rejected",
              revalidate_calibration(sealed_for_alias, sealed, exe) != "")
        restore_file(leg4_comp, leg4_orig)
        # Stage-table mutation WITH re-pointed campaign hash: the hash
        # matches, but the schedule is no longer the approved 18-run
        # table — exact table comparison must still fail.
        camp_path = os.path.join(tmp, "calib1", "campaign.json")
        with open(camp_path) as f:
            camp_orig = f.read()
        camp_mut = json.loads(camp_orig)
        camp_mut["stages"] = [s for s in camp_mut["stages"]
                              if s[0] != "response-control-GI"]
        with open(camp_path, "w") as f:
            json.dump(camp_mut, f)
        with open(sidecar_path) as f:
            sidecar_mut = json.load(f)
        sidecar_mut["campaign_sha256"] = sha256_file(camp_path)
        with open(sidecar_path, "w") as f:
            json.dump(sidecar_mut, f)
        check("fake stage-table mutation rejected",
              revalidate_calibration(sealed_for_alias, sealed, exe) != "")
        restore_file(camp_path, camp_orig)
        with open(sidecar_path, "w") as f:
            f.write(sidecar_orig)
        # Restored evidence revalidates clean (proves the mutations,
        # not the fixture, caused each rejection above).
        check("fake restored evidence revalidates",
              revalidate_calibration(sealed_for_alias, sealed, exe) == "")

        # --- Round-6b exact-mutation battery (each review repro, each
        # through the production revalidator AND an accept/analyze
        # entry; every mutation restored with revalidate-clean proof).
        cal1 = os.path.join(tmp, "calib1")

        def write_calib_prov(role_dir, text):
            with open(os.path.join(cal1, role_dir,
                                   "provenance.json"), "w") as f:
                f.write(text)

        # Earlier adversarial tests left k1/R_ON provenance tampered
        # (scene zeros, then workload spp=1). Rewrite it pristine so
        # the analyze-entry rejections below are caused by the new
        # mutations, not stale fixture damage.
        with open(os.path.join(adir, "k1", "R_ON",
                               "provenance.json"), "w") as f:
            json.dump({"exe": os.path.abspath(exe),
                       "exe_sha256": sha256_file(exe),
                       "scene_sha256": sha256_file(scene),
                       "frames": 60, "config": "C1",
                       "workload": WORKLOADS["C1"],
                       "edges_sha256": sha256_file(sealed)}, f)

        def mutate_prov_field(role_dir, **fields):
            with open(os.path.join(cal1, role_dir,
                                   "provenance.json")) as f:
                prov = json.load(f)
            original = json.dumps(prov)
            prov.update(fields)
            with open(os.path.join(cal1, role_dir,
                                   "provenance.json"), "w") as f:
                json.dump(prov, f)
            return original

        # Wrong scene hash (zeros) in off-C1, confirm-C1, control-RT.
        for role in ("off-C1", "confirm-C1", "control-RT"):
            orig = mutate_prov_field(role, scene_sha256="0" * 64)
            check("fake wrong-scene %s rejected" % role,
                  revalidate_calibration(sealed_for_alias, sealed, exe)
                  != "")
            write_calib_prov(role, orig)
        # Confirm-C1 wrong scene also fails through the analyze entry.
        orig = mutate_prov_field("confirm-C1", scene_sha256="0" * 64)
        check("fake analyze wrong-scene confirm rejected",
              cmd_analyze(ana) == 1)
        write_calib_prov("confirm-C1", orig)
        # 12-frame declarations in the same three roles.
        for role in ("off-C1", "confirm-C1", "control-RT"):
            orig = mutate_prov_field(role, frames=12)
            check("fake 12-frame %s rejected" % role,
                  revalidate_calibration(sealed_for_alias, sealed, exe)
                  != "")
            write_calib_prov(role, orig)
        # Off-C1 12-frame also fails through the analyze entry.
        orig = mutate_prov_field("off-C1", frames=12)
        check("fake analyze 12-frame OFF rejected",
              cmd_analyze(ana) == 1)
        write_calib_prov("off-C1", orig)
        # Deleted legacy run.log fails (evidence reread, not assumed).
        leg1_log = os.path.join(cal1, "legacy-C1", "run.log")
        with open(leg1_log, "rb") as f:
            leg1_log_bytes = f.read()
        os.remove(leg1_log)
        check("fake missing legacy log rejected",
              revalidate_calibration(sealed_for_alias, sealed, exe) != "")
        with open(leg1_log, "wb") as f:
            f.write(leg1_log_bytes)
        check("fake mutations restored revalidate clean",
              revalidate_calibration(sealed_for_alias, sealed, exe) == "")

        # Legacy authority: four arbitrary different binaries fail
        # sealing AND revalidation (seal binds the campaign input, not
        # per-run self-designations).
        alt_exes = []
        for i in range(4):
            alt = os.path.join(tmp, "alt-legacy-%d.exe" % i)
            with open(alt, "wb") as f:
                f.write(b"alt-legacy-%d" % i)
            alt_exes.append(alt)
        leg_origs = {}
        for cfg, alt in zip(CONFIGS, alt_exes):
            role = "legacy-%s" % cfg
            with open(os.path.join(cal1, role,
                                   "provenance.json")) as f:
                leg_origs[role] = f.read()
            mutate_prov_field(role, exe=os.path.abspath(alt),
                              exe_sha256=sha256_file(alt))
        check("fake four-legacy-binaries seal rejected",
              seal_calibration(sealed, cal1, legacy_exe) != "")
        check("fake four-legacy-binaries revalidate rejected",
              revalidate_calibration(sealed_for_alias, sealed, exe) != "")
        for role, original in leg_origs.items():
            write_calib_prov(role, original)
        # Byte-identical copy at another path: same hash, but NOT the
        # designated path — still rejected (path binding, not hash
        # inequality alone).
        twin = os.path.join(tmp, "legacy-twin.exe")
        shutil.copy(legacy_exe, twin)
        twin_orig = mutate_prov_field(
            "legacy-C2", exe=os.path.abspath(twin))
        check("fake legacy twin-path rejected",
              revalidate_calibration(sealed_for_alias, sealed, exe) != "")
        write_calib_prov("legacy-C2", twin_orig)
        check("fake authority mutations restored",
              revalidate_calibration(sealed_for_alias, sealed, exe) == "")

        # Relative exe under a nested directory: cmd[0] must be the
        # resolved absolute path (the child CWD is the exe dir, which
        # cannot see invocation-relative paths).
        nest = os.path.join(tmp, "nest", "bin")
        os.makedirs(nest)
        nest_exe = os.path.join(nest, "RT2App.exe")
        shutil.copy(exe, nest_exe)
        for spv in ("closesthit.spv", "secondary_raygen.spv",
                    "restir_gi_temporal.spv", "closesthit_tail.spv",
                    "secondary_raygen_tail.spv",
                    "restir_gi_temporal_tail.spv"):
            shutil.copy(os.path.join(tmp, spv), os.path.join(nest, spv))
        nest_rel = os.path.relpath(nest_exe, os.getcwd())
        seen_exe_argv = []

        def exe_spy_fake(exe_, cmd, cwd, logpath, timeout_min=None):
            seen_exe_argv.append((list(cmd), cwd, exe_))
            return fake_run(exe_, cmd, cwd, logpath,
                            lambda: snap_lines())

        a_exe = mkargs("relexe")
        a_exe.exe = nest_rel
        check("fake relative exe capture completes",
              run_capture(a_exe, run_fn=exe_spy_fake) == 0)
        if seen_exe_argv:
            cmd_seen, cwd_seen, exe_seen = seen_exe_argv[0]
            check("fake exe argv absolute",
                  os.path.isabs(cmd_seen[0]) and
                  cmd_seen[0] == os.path.abspath(nest_exe))
            check("fake exe CWD is nested exe dir",
                  cwd_seen == os.path.abspath(nest))
            check("fake exe run_fn receives absolute",
                  os.path.isabs(exe_seen))
        else:
            check("fake exe argv recorded", False)

        # Warmup-only strata (Sol round-5 repro): pilot window counts
        # present but stratum mass lives only in frames 1..40 → the
        # FULL run path must fail coverage, and derive must fail the
        # trial revalidation.
        def warmup_strata_lines():
            lines = []
            for fr in list(range(1, 41)) + list(range(41, 61)):
                o = {"schema": SCHEMA, "frame": fr, "slot": 0,
                     "serial": fr, "valid": False,
                     "reason": "pilot-only: no manifest staged",
                     "head": [0x53484457, 1, fr, 1, 0, 0, 1, 0],
                     "data": [0] * 1196}
                o["data"][0 * 298 + 14] = 5000
                o["data"][1 * 298 + 14] = 5000
                if fr < 41:
                    for fam in (0, 1):
                        for site in range(5):
                            o["data"][fam * 298 + 264 + site * 2 + 0] = 100
                            o["data"][fam * 298 + 264 + site * 2 + 1] = 100
                lines.append("[ShadowTail] %s\n" % json.dumps(o))
            return lines

        a_warm = mkargs("warmcov", edges="", config="C1")
        check("fake warmup-only strata fail full run path",
              run_capture(a_warm, run_fn=lambda e, c, w, l, t=None: fake_run(
                  e, c, w, l, warmup_strata_lines)) == 1)
        wdir = os.path.join(tmp, "warmtrial")
        os.makedirs(wdir)
        from PIL import Image as _WImage
        _WImage.new("RGB", (8, 8), (10, 20, 30)).save(
            os.path.join(wdir, "frame.png"))
        with open(os.path.join(wdir, "run.log"), "w") as f:
            f.write("[NGX] supported=yes backend=none (fake)\n")
            f.write("[Denoiser] submitted=Off backend=none (fake)\n")
            for line in warmup_strata_lines():
                f.write(line)
        with open(os.path.join(wdir, "completion.json"), "w") as f:
            json.dump({"complete": True,
                       "ngx_effective": "[NGX] fake",
                       "denoiser_effective": "[Denoiser] fake"}, f)
        with open(os.path.join(wdir, "provenance.json"), "w") as f:
            json.dump({"exe_sha256": sha256_file(exe),
                       "scene_sha256": sha256_file(trial_scenes["C1"]),
                       "frames": 60, "config": "C1",
                       "workload": WORKLOADS["C1"]}, f)
        da_warm = argparse.Namespace(
            trial_dirs=[wdir,
                        write_trial(pdir, "W2", "C2", trial_scenes["C2"]),
                        write_trial(pdir, "W3", "C3", trial_scenes["C3"]),
                        write_trial(pdir, "W4", "C4", trial_scenes["C4"])],
            ref_exe=exe, out=os.path.join(tmp, "warm.json"))
        check("fake derive warmup-only strata rejected",
              cmd_derive_freeze(da_warm) == 1)

        # Missing-19-timings: a quartet leg with a single timing record
        # fails parity on the real orchestration path (not a unit probe).
        def thin_fake(exe_, cmd, cwd, logpath, timeout_min=None):
            from PIL import Image as _TImage
            _TImage.new("RGB", (8, 8), (10, 20, 30)).save(
                cmd[cmd.index("--output-display") + 1])
            with open(logpath, "w") as lf:
                lf.write("[NGX] supported=yes backend=none (fake)\n")
                lf.write("[Denoiser] submitted=Off backend=none (fake)\n")
                for line in snap_lines():
                    lf.write(line)
                if "--benchmark-timings" in cmd:
                    lf.write('[HeadlessTiming] {"frame":60,"regions_ms":'
                             '{"RT Shading":0.5,"GPU Frame":1.0}}\n')
            return 0

        qa_thin = argparse.Namespace(
            ref_exe=exe, cand_exe=cand_exe, scene=scene, out_root=tmp,
            name="thin1", frames=60, spp=1, width=64, height=64,
            edges=mf, control=False, config="", extra_args=[])
        check("fake quartet thin timings fail", run_quartet(qa_thin,
              run_fn=thin_fake) == 1)

        # Nearly-exhausted clock: 0.05 min remains for 33 stages — the
        # first stage launches with a 0.0015-min share (never more than
        # remaining: split_budget tripwire below), everything after is
        # INCOMPLETE without launching, and the manifest is over_cap.
        check("split budget never exceeds remaining",
              split_budget(0.05, 33) <= 0.05 and
              split_budget(90.0, 33) <= 90.0 and
              split_budget(0.0, 4) <= 0 and
              split_budget(10.0, 0) <= 0)
        # first stage launches with a tiny share (never more than
        # remaining: split_budget tripwire below), and wall-time
        # accounting stays honest. Two variants: a timeout-HONORING
        # fake (launches consume exactly their share — shares must
        # partition the budget, which a floor would break) and an
        # instant fake with a mid-campaign lump (over-cap INCOMPLETE).
        check("split budget never exceeds remaining",
              split_budget(0.05, 33) <= 0.05 and
              split_budget(90.0, 33) <= 90.0 and
              split_budget(0.0, 4) <= 0 and
              split_budget(10.0, 0) <= 0)
        real_mono_hon = time.monotonic
        clock_hon = [0.0]
        seen_hon = []
        lump_hon = [False]

        def hon_fake(exe_, cmd, cwd, logpath, timeout_min=None):
            remaining = 90.0 - clock_hon[0] / 60
            seen_hon.append((timeout_min, remaining))
            if len(seen_hon) == 3:
                clock_hon[0] += 10.0 * 60
                lump_hon[0] = True
            elif timeout_min:
                # Launched processes finish marginally inside their
                # share (realistic earliness); the shares still sum to
                # ~the whole budget, proving the partition. A floor
                # would overshoot the cap here instead.
                clock_hon[0] += timeout_min * 60 * 0.999
            return camp_fake(exe_, cmd, cwd, logpath, timeout_min)

        try:
            time.monotonic = lambda: clock_hon[0]
            hon = argparse.Namespace(
                mode="calib", ref_exe=exe, cand_exe="",
                legacy_exe=legacy_exe,
                scene_C1=scenes["C1"], scene_C2=scenes["C2"],
                scene_C3=scenes["C3"], scene_C4=scenes["C4"],
                out_root=tmp, name="honored", freeze="")
            rc_hon = run_campaign(hon, run_fn=hon_fake)
        finally:
            time.monotonic = real_mono_hon
        check("fake honored-budget campaign completes", rc_hon == 0)
        check("fake allotments never exceed remaining",
              all(t is None or t <= r + 1e-9 for t, r in seen_hon))
        with open(os.path.join(tmp, "honored", "campaign.json")) as f:
            honm = json.load(f)
        check("fake honored elapsed within cap",
              honm["elapsed_min"] <= honm["cap_min"] and
              honm["complete"] is True)

        # Same lump with an INSTANT fake: downstream stages launch
        # (budget technically remains) but the wall clock proves the
        # over-cap path records INCOMPLETE, never complete:true.
        real_mono_nc = time.monotonic
        clock_nc = [0.0]
        lump_nc = [False]

        def nc_fake(exe_, cmd, cwd, logpath, timeout_min=None):
            if lump_nc[0] is False and any("trial-C3" in c for c in cmd):
                clock_nc[0] += 200.0 * 60
                lump_nc[0] = True
            return camp_fake(exe_, cmd, cwd, logpath, timeout_min)

        try:
            time.monotonic = lambda: clock_nc[0]
            nc = argparse.Namespace(
                mode="calib", ref_exe=exe, cand_exe="",
                legacy_exe=legacy_exe,
                scene_C1=scenes["C1"], scene_C2=scenes["C2"],
                scene_C3=scenes["C3"], scene_C4=scenes["C4"],
                out_root=tmp, name="nearlycapped", freeze="")
            rc_nc = run_campaign(nc, run_fn=nc_fake)
        finally:
            time.monotonic = real_mono_nc
        check("fake lump over-cap campaign incomplete", rc_nc == 2)
        with open(os.path.join(tmp, "nearlycapped",
                               "campaign.json")) as f:
            ncm = json.load(f)
        check("fake lump over-cap recorded",
              ncm["complete"] is False and ncm.get("over_cap") is True)

        # Launch-gate units: None means standalone-uncapped, zero and
        # negative mean SKIP (never the uncapped mode), positives pass
        # through as shares.
        real_mono_gate = time.monotonic
        try:
            time.monotonic = lambda: 1000.0
            check("launch gate standalone uncapped",
                  remaining_timeout(None, 4) is None)
            check("launch gate past deadline skips",
                  remaining_timeout(999.0, 4) == "SKIP")
            check("launch gate exact-zero skips",
                  remaining_timeout(1000.0, 4) == "SKIP")
            check("launch gate positive shares",
                  remaining_timeout(1060.0, 4) == 0.25)
        finally:
            time.monotonic = real_mono_gate
        # Scripted-clock race: the expiry check passes (clock <
        # deadline) but the clock reaches the deadline before the share
        # is computed — the leg must NOT launch (zero launches) and
        # must record INCOMPLETE, never an uncapped spawn.
        race_clock = [0.0, 0.0, 60.0, 60.0, 60.0, 60.0, 60.0, 60.0,
                      60.0, 60.0]
        race_launches = [0]

        def race_fake(exe_, cmd, cwd, logpath, timeout_min=None):
            race_launches[0] += 1
            return fake_run(exe_, cmd, cwd, logpath, lambda: snap_lines())

        real_mono_race = time.monotonic
        try:
            time.monotonic = lambda: (race_clock.pop(0)
                                      if race_clock else 60.0)
            # deadline_min is a run_quartet parameter (not a Namespace
            # field): the scripted clock passes the expiry check
            # (0 < 60) then reports zero remaining at share time.
            rc_race = run_quartet(
                argparse.Namespace(
                    ref_exe=exe, cand_exe=cand_exe, scene=scene, out_root=tmp,
                    name="race1", frames=60, spp=1, width=64, height=64,
                    edges=mf, control=False, config="", extra_args=[]),
                run_fn=race_fake, deadline_min=1)
        finally:
            time.monotonic = real_mono_race
        check("fake race zero-budget never launches",
              race_launches[0] == 0 and rc_race == 2)

        # Relative-root capture: launched argv carries ABSOLUTE scene,
        # edges and output-display paths, CWD is the exe dir, and the
        # frame is written/read at the real requested target.
        seen_argv = []

        def spy_fake(exe_, cmd, cwd, logpath, timeout_min=None):
            seen_argv.append((list(cmd), cwd))
            return fake_run(exe_, cmd, cwd, logpath,
                            lambda: snap_lines())

        rel_root = os.path.relpath(tmp, os.getcwd())
        a_rel = mkargs("relroot", edges=mf)
        a_rel.out_root = rel_root
        a_rel.name = "relroot-%s" % os.path.basename(tmp)
        check("fake relative-root capture completes",
              run_capture(a_rel, run_fn=spy_fake) == 0)
        if seen_argv:
            argv, argv_cwd = seen_argv[0]
            edge_idx = argv.index("--shadow-tail-edges") + 1
            out_idx = argv.index("--output-display") + 1
            scene_idx = argv.index("--scene") + 1
            check("fake argv paths absolute",
                  os.path.isabs(argv[edge_idx]) and
                  os.path.isabs(argv[out_idx]) and
                  os.path.isabs(argv[scene_idx]))
            check("fake argv CWD is exe dir",
                  argv_cwd == os.path.dirname(os.path.abspath(exe)))
            check("fake frame at requested target",
                  os.path.exists(argv[out_idx]))
        else:
            check("fake argv recorded", False)

        # Calibration recompute on tampered evidence: corrupt a
        # retained OFF frame inside the sealed campaign → parity
        # recomputation fails (proves gates rerun, not trusted).
        off2_frame = os.path.join(tmp, "calib1", "legacy-C2", "frame.png")
        with open(off2_frame, "rb") as pf:
            off2_bytes = pf.read()
        with open(off2_frame, "wb") as pf:
            pf.write(b"tampered!")
        with open(sealed) as f:
            sealed_for_recompute = json.load(f)
        check("fake recompute detects tampered evidence",
              revalidate_calibration(sealed_for_recompute, sealed, exe)
              != "")
        with open(off2_frame, "wb") as pf:
            pf.write(off2_bytes)

        # Missing tail SPVs fail diag captures loudly.
        bare = os.path.join(tmp, "bare")
        os.makedirs(bare)
        bare_exe = os.path.join(bare, "RT2App.exe")
        with open(bare_exe, "wb") as f:
            f.write(b"fake-exe")
        a9 = mkargs("nospv")
        a9.exe = bare_exe
        check("fake missing tail SPVs fail",
              run_capture(a9, run_fn=lambda e, c, w, l, t=0: fake_run(
                  e, c, w, l, lambda: snap_lines())) == 1)

        # Measured-window rule: frames 41..60 must all be present.
        def gap_lines():
            return [l for l in snap_lines()
                    if json.loads(l.split("[ShadowTail]", 1)[1])["frame"] != 55]
        a10 = mkargs("gap")
        check("fake window gap fails",
              run_capture(a10, run_fn=lambda e, c, w, l, t=0: fake_run(
                  e, c, w, l, gap_lines)) == 1)

        # Missing effective-backend evidence fails every mode.
        a11 = mkargs("nomode")
        check("fake missing NGX/Denoiser lines fail",
              run_capture(a11, run_fn=lambda e, c, w, l, t=0: fake_run(
                  e, c, w, l, lambda: snap_lines(),
                  mode_lines=False)) == 1)

        # Native-output integration: the C++-serialized golden pilot /
        # control JSON (regenerated above when ST_RT2TESTS_EXE is set)
        # drives FULL-window fake captures — frame/serial/head-frame
        # varied per frame exactly as the binary emits per submitted
        # frame; counter words stay byte-identical native output.
        def golden_window_lines(golden_text):
            obj = json.loads(golden_text)
            lines = []
            for f in range(41, 61):
                o = dict(obj)
                o["frame"] = f
                o["serial"] = f
                head = list(obj["head"])
                head[2] = f
                o["head"] = head
                lines.append("[ShadowTail] %s\n" % json.dumps(o))
            return lines

        a12 = mkargs("nativepilot", edges="")
        check("fake native pilot window completes",
              run_capture(a12, run_fn=lambda e, c, w, l, t=0: fake_run(
                  e, c, w, l,
                  lambda: golden_window_lines(pilot_golden))) == 0)
        # A valid:true line in a pilot run is stale success, not data.
        def pilot_plus_valid():
            return (golden_window_lines(pilot_golden) +
                    snap_lines(n=1, start=41))
        a13 = mkargs("pilotstale", edges="")
        check("fake pilot stale-valid fails",
              run_capture(a13, run_fn=lambda e, c, w, l, t=0: fake_run(
                  e, c, w, l, pilot_plus_valid)) == 1)
        a14 = mkargs("nativecontrol", control=True)
        check("fake native control window completes",
              run_capture(a14, run_fn=lambda e, c, w, l, t=0: fake_run(
                  e, c, w, l,
                  lambda: golden_window_lines(ctl_golden))) == 0)
        # A mode-1 valid line in a control run is a leak, not success.
        def control_plus_valid():
            return (golden_window_lines(ctl_golden) +
                    snap_lines(n=1, start=41))
        a15 = mkargs("controlleak", control=True)
        check("fake control mode-1 leak fails",
              run_capture(a15, run_fn=lambda e, c, w, l, t=0: fake_run(
                  e, c, w, l, control_plus_valid)) == 1)
        # One pilot snapshot can never complete a 60-frame pilot run.
        a16 = mkargs("pilotone", edges="")
        check("fake single pilot snapshot fails",
              run_capture(a16, run_fn=lambda e, c, w, l, t=0: fake_run(
                  e, c, w, l,
                  lambda: golden_window_lines(pilot_golden)[:1])) == 1)

        # derive-freeze on a NATIVE pilot-only trial must fail CLOSED:
        # the golden carries tiny RT mass (precision-inadequate bracket)
        # and an empty GI family — neither can freeze edges, never
        # silently.
        ndir = os.path.join(tmp, "native-pilot")
        os.makedirs(ndir)
        with open(os.path.join(ndir, "run.log"), "w") as f:
            for line in golden_window_lines(pilot_golden):
                f.write(line)
        with open(os.path.join(ndir, "completion.json"), "w") as f:
            json.dump({"complete": True}, f)
        with open(os.path.join(ndir, "provenance.json"), "w") as f:
            json.dump({"exe_sha256": sha256_file(exe)}, f)
        nda = argparse.Namespace(trial_dirs=[ndir], ref_exe=exe,
                                 out=os.path.join(tmp, "frozen-native.json"))
        check("fake native GI-empty derive fails closed",
              cmd_derive_freeze(nda) == 1)

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
    an.add_argument("--ref-exe", required=True)
    an.add_argument("--cand-exe", required=True)
    sub.add_parser("plan")
    cp = sub.add_parser("campaign")
    cp.add_argument("--mode", choices=("calib", "accept"), required=True)
    cp.add_argument("--ref-exe", required=True)
    cp.add_argument("--cand-exe", default="")
    cp.add_argument("--legacy-exe", default="")
    cp.add_argument("--scene-C1", default="")
    cp.add_argument("--scene-C2", default="")
    cp.add_argument("--scene-C3", default="")
    cp.add_argument("--scene-C4", default="")
    cp.add_argument("--out-root", required=True)
    cp.add_argument("--name", required=True)
    cp.add_argument("--freeze", default="")
    # No --spp/--width/--height/extra-args: the frozen per-config
    # WORKLOADS table owns every capture command (round-4 P0).
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
    if args.cmd == "campaign":
        if args.mode == "accept" and not args.cand_exe:
            print("ERROR: accept mode requires --cand-exe")
            return 1
        if args.mode == "calib" and not args.legacy_exe:
            print("ERROR: calib mode requires --legacy-exe")
            return 1
        return run_campaign(args)
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
