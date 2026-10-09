"""RT4-T4 bounded visibility discriminators (independent MASK / BLEND gates).

Reads control/candidate frame pairs produced by run_rt4t4_disc.ps1 and checks
TIGHT, per-path ROIs — never a broad full-frame blend check:

Override fixture (override path: per-instance materials on primitive cubes,
raster-first primaries, emissive white cubes on black):
- MASK_TIGHT must change ~fully (pre-fix renders the MASK cube opaque white,
  post-fix discards it to background black). This is the override-path
  any-hit-execution proof.
- BLEND_CUBE and OPAQUE_CUBE must be byte-identical: this pair makes NO
  BLEND-override visibility claim (override BLEND renders identically here;
  BLEND visibility is proved by the sun pair below).

Mixed-sun fixture (per-triangle path: imported whole-model OBJ, meshRef -1,
directional sun, pure-path primaries via scene aperture 0.02):
- BLEND_CORE / BLEND_WHOLE must change (post-fix blends the BLEND quad with
  the background; pre-fix renders it opaque). This is the per-triangle
  any-hit-execution proof — actual ray visibility through BLEND triangles.
- OPAQUE quad, floor and top strip must be byte-identical (no collateral).

Synth regression must be byte-identical. Sponza (if present) is record-only:
the decal correction is expected change, not a gate.

Usage: python rt4t4_disc_assert.py <OutRoot>
Exits 0 iff every gate passes.
"""

import sys
from pathlib import Path

import numpy as np
from PIL import Image


def load(root: Path, name: str) -> np.ndarray:
    return np.asarray(
        Image.open(root / name / "frame.png").convert("RGB"), dtype=np.float64
    )


def stats(name, pre, post, r0, r1, c0, c1):
    d = np.abs(pre - post).sum(axis=-1)[r0:r1, c0:c1]
    frac = float((d > 30).mean())
    dm = float(pre[r0:r1, c0:c1].mean() - post[r0:r1, c0:c1].mean())
    print(
        "%-12s frac>30=%.4f premean=%.1f postmean=%.1f delta(pre-post)=%+.1f"
        % (name, frac, pre[r0:r1, c0:c1].mean(), post[r0:r1, c0:c1].mean(), dm)
    )
    return frac, dm


def check(desc, cond):
    print(("PASS" if cond else "FAIL"), "-", desc)
    return bool(cond)


def main():
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    ok = True

    # --- Override fixture: independent per-cube ROIs (960x540) ---
    pre = load(root, "over-control")
    post = load(root, "over-candidate")
    frac, _ = stats("MASK_TIGHT", pre, post, 240, 390, 660, 780)
    ok &= check("override MASK discarded (frac>0.9)", frac > 0.9)
    ok &= check(
        "override MASK direction (pre bright, post background)",
        pre[240:390, 660:780].mean() > 150.0
        and post[240:390, 660:780].mean() < 20.0,
    )
    frac, _ = stats("BLEND_CUBE", pre, post, 210, 420, 387, 573)
    ok &= check("override BLEND identical (no claim, frac==0)", frac == 0.0)
    frac, _ = stats("OPAQUE_CUBE", pre, post, 210, 420, 145, 331)
    ok &= check("override opaque identical (frac==0)", frac == 0.0)
    frac, _ = stats("OVER_BG", pre, post, 30, 150, 300, 660)
    ok &= check("override background identical (frac==0)", frac == 0.0)

    # --- Mixed-sun fixture: per-triangle BLEND proof (960x540) ---
    pre = load(root, "sun-control")
    post = load(root, "sun-candidate")
    frac, dm = stats("BLEND_CORE", pre, post, 340, 400, 530, 650)
    ok &= check("mixed BLEND core changes (frac>0.5)", frac > 0.5)
    ok &= check("mixed BLEND direction (blend-down, delta>15)", dm > 15.0)
    frac, _ = stats("BLEND_WHOLE", pre, post, 200, 405, 520, 720)
    ok &= check("mixed BLEND quad changes (frac>0.10)", frac > 0.10)
    frac, _ = stats("SUN_OPAQUE", pre, post, 200, 400, 250, 420)
    ok &= check("mixed opaque identical (frac==0)", frac == 0.0)
    frac, _ = stats("SUN_FLOOR_L", pre, post, 430, 530, 100, 300)
    ok &= check("mixed floor-L identical (frac==0)", frac == 0.0)
    frac, _ = stats("SUN_FLOOR_R", pre, post, 430, 530, 660, 900)
    ok &= check("mixed floor-R identical (frac==0)", frac == 0.0)

    # --- Synth regression: must be identical ---
    pre = load(root, "synth-control")
    post = load(root, "synth-candidate")
    d = np.abs(pre - post).sum(axis=-1)
    frac = float((d > 30).mean())
    print("SYNTH frac>30=%.6f" % frac)
    ok &= check("synth identical (frac==0)", frac == 0.0)

    # --- Sponza: record only (decal correction is expected change) ---
    if (root / "yard-control" / "frame.png").exists():
        a = load(root, "yard-control")
        b = load(root, "yard-candidate")
        d = np.abs(a - b).sum(axis=-1)
        print(
            "YARD record-only frac>30=%.6f changedpx=%d"
            % ((d > 30).mean(), int((d > 30).sum()))
        )

    print("DISCRIMINATORS", "ALL PASS" if ok else "FAILURES PRESENT")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
