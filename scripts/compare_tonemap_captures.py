#!/usr/bin/env python3
"""Bounded GPU/headless differential checks for camera-owned tone mapping.

Compares headless RT2App captures (PNG display output + PFM scene-linear
source) against an independent CPU port of ToneMapMath, kept in lockstep
with RT2App/src/ToneMapMath.h by construction (same constants and order;
float64 here, so <=1 code value of slack is expected).

Checks:
  parity Bowie  : PNG bytes vs CPU(PFM pixels) for each variant (max<=1).
  isolation     : PFM sources byte-identical across operators (same seed,
                  frames and transport; only the look differs).
  effect        : PNGs differ across operators/EV (the look takes effect).
  debug         : gbuffer-debug PNGs are byte-identical across presentations
                  (diagnostic views bypass the camera look).
  alpha         : opaque scene stays opaque (alpha 255).

Usage: compare_tonemap_captures.py <workdir>
Exits 0 when every check passes, 1 otherwise.
"""
import math
import struct
import sys
import zlib


def read_png_rgba8(path):
    with open(path, "rb") as f:
        sig = f.read(8)
    assert sig == b"\x89PNG\r\n\x1a\n", "not a PNG: %s" % path
    data = open(path, "rb").read()[8:]
    width = height = None
    ctype = bitdepth = None
    raw = b""
    pos = 0
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype4 = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        if ctype4 == b"IHDR":
            width, height, bitdepth, ctype, _, _, _ = struct.unpack(">IIBBBBB", chunk)
            assert bitdepth == 8 and ctype == 6, "need 8-bit RGBA PNG"
        elif ctype4 == b"IDAT":
            raw += chunk
        elif ctype4 == b"IEND":
            break
        pos += 12 + length
    pixels = zlib.decompress(raw)
    stride = width * 4
    rows = []
    prev = bytearray(stride)
    off = 0
    for _ in range(height):
        filt = pixels[off]
        off += 1
        cur = bytearray(pixels[off:off + stride])
        off += stride
        if filt == 1:
            for i in range(4, stride):
                cur[i] = (cur[i] + cur[i - 4]) & 0xFF
        elif filt == 2:
            for i in range(stride):
                cur[i] = (cur[i] + prev[i]) & 0xFF
        elif filt == 3:
            for i in range(stride):
                a = cur[i - 4] if i >= 4 else 0
                cur[i] = (cur[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif filt == 4:
            for i in range(stride):
                a = cur[i - 4] if i >= 4 else 0
                b = prev[i]
                c = prev[i - 4] if i >= 4 else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                cur[i] = (cur[i] + pr) & 0xFF
        elif filt != 0:
            raise AssertionError("unsupported PNG filter %d" % filt)
        rows.append(bytes(cur))
        prev = cur
    return width, height, rows


def read_pfm_rgb(path):
    with open(path, "rb") as f:
        magic = f.readline().strip()
        assert magic == b"PF", "not a color PFM: %s" % path
        w, h = [int(x) for x in f.readline().split()]
        scale = float(f.readline().strip())
        assert scale < 0, "need little-endian PFM"
        n = w * h * 3
        raw = f.read(n * 4)
        assert len(raw) == n * 4, "truncated PFM"
        vals = struct.unpack("<%df" % n, raw)
    # Writer stores bottom-to-top; flip to top-down to match PNG rows.
    rows = []
    for y in range(h):
        src = h - 1 - y
        rows.append(vals[src * w * 3:(src + 1) * w * 3])
    return w, h, rows


# --- CPU reference port (lockstep with ToneMapMath.h) ---

AGX_INSET = (
    (0.842479062253094, 0.0784335999999992, 0.0792237451477643),
    (0.0423282422610123, 0.878468636469772, 0.0791661274605434),
    (0.0423756549057051, 0.0784336, 0.879142973793104),
)
AGX_OUTSET = (
    (1.19687900512017, -0.0980208811401368, -0.0990297440797205),
    (-0.0528968517574562, 1.15190312990417, -0.0989611768448433),
    (-0.0529716355144438, -0.0980434501171241, 1.15107367264116),
)
AGX_MIN_EV, AGX_MAX_EV = -12.47393, 4.026069
ACES_IN = (
    (0.59719, 0.35458, 0.04823),
    (0.07600, 0.90834, 0.01566),
    (0.02840, 0.13383, 0.83777),
)
ACES_OUT = (
    (1.60475, -0.53108, -0.07367),
    (-0.10208, 1.10813, -0.00605),
    (-0.00327, -0.07276, 1.07602),
)


def matvec(m, v):
    return tuple(sum(m[i][j] * v[j] for j in range(3)) for i in range(3))


def agx_sigmoid(x):
    x2, x4 = x * x, x * x * x * x
    return (15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4
            - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232)


def aces_fit(v):
    return (v * (v + 0.0245786) - 0.000090537) / (v * (0.983729 * v + 0.4329510) + 0.238081)


def clamp01(v):
    return 0.0 if v < 0.0 else (1.0 if v > 1.0 else v)


def agx_display(r, g, b):
    x, y, z = matvec(AGX_INSET, (r, g, b))
    x = min(max(math.log2(x), AGX_MIN_EV), AGX_MAX_EV) if x > 0 else AGX_MIN_EV
    y = min(max(math.log2(y), AGX_MIN_EV), AGX_MAX_EV) if y > 0 else AGX_MIN_EV
    z = min(max(math.log2(z), AGX_MIN_EV), AGX_MAX_EV) if z > 0 else AGX_MIN_EV
    span = AGX_MAX_EV - AGX_MIN_EV
    x, y, z = agx_sigmoid((x - AGX_MIN_EV) / span), agx_sigmoid((y - AGX_MIN_EV) / span), agx_sigmoid((z - AGX_MIN_EV) / span)
    r2, g2, b2 = matvec(AGX_OUTSET, (x, y, z))
    return (clamp01(max(r2, 0.0) ** 2.2), clamp01(max(g2, 0.0) ** 2.2), clamp01(max(b2, 0.0) ** 2.2))


def aces_display(r, g, b):
    rgb = tuple(aces_fit(v) for v in matvec(ACES_IN, (r, g, b)))
    return tuple(clamp01(v) for v in matvec(ACES_OUT, rgb))


def reinhard_display(r, g, b):
    return tuple(clamp01(v / (1.0 + v)) for v in (r, g, b))


def srgb8(v):
    v = max(v, 0.0)
    e = 12.92 * v if v <= 0.0031308 else 1.055 * (v ** (1.0 / 2.4)) - 0.055
    e = min(max(e, 0.0), 1.0)
    return int(e * 255.0 + 0.5)


def convert(r, g, b, op, mult):
    if not (math.isfinite(r) and math.isfinite(g) and math.isfinite(b)):
        return None
    r, g, b = max(r, 0.0) * mult, max(g, 0.0) * mult, max(b, 0.0) * mult
    if op == "aces":
        rgb = aces_display(r, g, b)
    elif op == "reinhard":
        rgb = reinhard_display(r, g, b)
    else:
        rgb = agx_display(r, g, b)
    return tuple(srgb8(v) for v in rgb)


def main():
    workdir = sys.argv[1]
    failures = []

    def note(ok, label, detail=""):
        print(("[PASS] " if ok else "[FAIL] ") + label + (" " + detail if detail else ""))
        if not ok:
            failures.append(label)

    variants = {"agx": ("agx", 0.0), "reinhard": ("reinhard", 0.0), "aces": ("aces", 2.0),
                "native": ("agx", 0.0)}
    pngs, pfms = {}, {}
    for name in variants:
        w, h, pngs[name] = read_png_rgba8("%s/%s.png" % (workdir, name))
        pw, ph, pfms[name] = read_pfm_rgb("%s/%s.pfm" % (workdir, name))
        if (w, h) != (pw, ph):
            note(False, "extent", "%s png=%dx%d pfm=%dx%d" % (name, w, h, pw, ph))
            return 1

    # Parity: GPU PNG vs CPU reference over the completed PFM source.
    for name, (op, ev) in variants.items():
        mult = 2.0 ** ev
        rows = pngs[name]
        frows = pfms[name]
        h = len(rows)
        w = len(rows[0]) // 4
        maxdiff, total, n = 0, 0, 0
        for y in range(h):
            prow, frow = rows[y], frows[y]
            for x in range(w):
                r, g, b = frow[x * 3], frow[x * 3 + 1], frow[x * 3 + 2]
                ref = convert(r, g, b, op, mult)
                if ref is None:
                    note(False, "parity-finite", "%s has nonfinite HDR at %d,%d" % (name, x, y))
                    return 1
                for c in range(3):
                    d = abs(prow[x * 4 + c] - ref[c])
                    maxdiff = max(maxdiff, d)
                    total += d
                    n += 1
        mean = total / max(n, 1)
        note(maxdiff <= 1, "parity-%s" % name, "max=%d mean=%.4f" % (maxdiff, mean))

    # Isolation: the linear source is byte-identical across operators.
    base = open("%s/agx.pfm" % workdir, "rb").read()
    for name in ("reinhard", "aces"):
        other = open("%s/%s.pfm" % (workdir, name), "rb").read()
        note(base == other, "isolation-%s" % name,
             "pfm bytes %d vs %d" % (len(base), len(other)))

    # Effect: the look changes the display output.
    def diff_count(a, b):
        ra, rb = pngs[a], pngs[b]
        n = 0
        for y in range(len(ra)):
            for i in range(len(ra[y])):
                if ra[y][i] != rb[y][i]:
                    n += 1
        return n

    note(diff_count("agx", "reinhard") > 1000, "effect-reinhard",
         "differing bytes=%d" % diff_count("agx", "reinhard"))
    note(diff_count("agx", "aces") > 1000, "effect-aces",
         "differing bytes=%d" % diff_count("agx", "aces"))

    # Debug bypass: diagnostic PNGs ignore the camera look.
    _, _, dbg_agx = read_png_rgba8("%s/debug_agx.png" % workdir)
    _, _, dbg_aces = read_png_rgba8("%s/debug_aces.png" % workdir)
    note(dbg_agx == dbg_aces, "debug-bypass", "diagnostic PNGs must be byte-identical")

    # Alpha stays opaque on this scene.
    opaque = all(pngs["agx"][y][x] == 255
                 for y in range(len(pngs["agx"]))
                 for x in range(3, len(pngs["agx"][y]), 4))
    note(opaque, "alpha-opaque", "")

    print("tonemap differential checks: %s" % ("ALL PASS" if not failures else "FAILURES: %s" % failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
