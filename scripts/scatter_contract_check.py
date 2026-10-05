#!/usr/bin/env python3
"""RR5-T2 sampling-weight contract check (scripts/scatter_contract_check.py).

Analyzes a headless contract-battery capture produced by:
  RT2App.exe --headless --scene <any> --denoiser-mode off --frames 1
      --width 160 --height 32 --gbuffer-debug 27 --output-hdr <out.pfm>
(any scene works: mode 27 replaces path tracing with the battery; the
image must hold at least 192 pixels: 64 cases x 3 for att/exp/P-flags,
since PFM keeps RGB only).

The battery (RT2App/shaders/scatter_contract.comp) invokes the CONSUMED
scatterPrimaryHit on matched materials and records, per case, the raw
attenuation, the selection probability actually used, the lobe outcome
and the unweighted factor recomputed through the exact shared helpers
the implementation calls. This script asserts attenuation * P ==
unweighted within a tight relative tolerance (exact for deterministic
delta with effective P == 1, few-ULP for survivors) over regular
specular, regular diffuse and deterministic delta branches. The
stochastic smooth-dielectric branch is recorded, never asserted
(documented legacy defect). All float products are evaluated in
float64, which is exact for binary32 operands, so the tolerance
measures genuine arithmetic deviation, not analysis rounding.

Exit 0 (PASS) or 1 (FAIL); prints per-material coverage and worst case.
"""

import sys

import numpy as np

CASES = 64
CELLS = 16
TOL_REL = 1e-6
ABS_FLOOR = 1e-30
FLAG_SPEC = 1
FLAG_DELTA = 2
FLAG_NOSCATTER = 4
FLAG_LEGACY = 8


def read_pfm(path):
    with open(path, "rb") as f:
        magic = f.readline().decode("ascii").strip()
        assert magic == "PF", f"want color PFM, got {magic!r}"
        dims = f.readline().decode("ascii").strip()
        while dims.startswith("#"):
            dims = f.readline().decode("ascii").strip()
        w, h = (int(x) for x in dims.split())
        scale = float(f.readline().decode("ascii").strip())
        assert scale != 0.0
        endian = "<" if scale < 0 else ">"
        n = w * h * 3
        data = np.frombuffer(f.read(n * 4), dtype=endian + "f4")
        assert data.size == n, (data.size, n)
        return np.flipud(data.reshape(h, w, 3)).astype(np.float64), w, h


def main(path):
    img, w, h = read_pfm(path)
    failures = []
    if w * h < 3 * CASES:
        print(f"FAIL: extent {w}x{h} holds fewer than {3 * CASES} case pixels")
        return 1

    def px(linear):
        return img[linear // w, linear % w]

    worst = (0.0, -1)
    coverage = {}
    legacy_vals = []
    for k in range(CASES):
        mat = k // CELLS
        att = px(3 * k)
        exp = px(3 * k + 1)
        meta = px(3 * k + 2)
        p, flags = float(meta[0]), int(meta[1])
        if not (np.all(np.isfinite(att)) and np.all(np.isfinite(exp))
                and np.isfinite(p)):
            failures.append(f"case {k}: non-finite recorded values")
            continue
        key = (mat, bool(flags & FLAG_SPEC), bool(flags & FLAG_DELTA),
               bool(flags & FLAG_NOSCATTER), bool(flags & FLAG_LEGACY))
        coverage[key] = coverage.get(key, 0) + 1
        if flags & FLAG_LEGACY:
            legacy_vals.append((k, att.tolist(), p))
            continue
        if flags & FLAG_NOSCATTER:
            failures.append(f"case {k}: no-scatter path taken (battery geometry?)")
            continue
        num = np.abs(att * p - exp)
        den = np.maximum(np.abs(exp), ABS_FLOOR)
        rel = num / den
        m = float(rel.max())
        if m > worst[0]:
            worst = (m, k)
        if np.any(rel > TOL_REL):
            failures.append(
                f"case {k} mat{mat} relerr={rel.tolist()} "
                f"att={att.tolist()} P={p} exp={exp.tolist()} flags={flags}")

    # Coverage: assert materials saw specular, diffuse M0/M1 saw diffuse,
    # M2 saw only delta, M3 stayed legacy.
    def seen(mat, spec=None, delta=None, legacy=None, noscat=None):
        return [c for c in coverage
                if c[0] == mat
                and (spec is None or c[1] == spec)
                and (delta is None or c[2] == delta)
                and (noscat is None or c[3] == noscat)
                and (legacy is None or c[4] == legacy)]

    for mat in (0, 1):
        if not seen(mat, spec=True, delta=False, legacy=False, noscat=False):
            failures.append(f"material {mat}: regular specular never observed")
        if not seen(mat, spec=False, delta=False, legacy=False, noscat=False):
            failures.append(f"material {mat}: regular diffuse never observed")
    if seen(2, delta=False) or not seen(2, delta=True):
        failures.append("material 2: expected only deterministic delta outcomes")
    if seen(3, legacy=False):
        failures.append("material 3: expected legacy-recorded outcomes only")

    print(f"cases={CASES} worst_relerr={worst[0]:.3e} at case {worst[1]}")
    print(f"legacy recorded cells: {len(legacy_vals)} (values logged, not asserted)")
    for k, att, p in legacy_vals[:4]:
        print(f"  legacy case {k}: att={att} P={p}")
    if failures:
        print(f"FAIL ({len(failures)}):")
        for f in failures[:10]:
            print(f"  {f}")
        return 1
    print("VERDICT=PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
