# ReSTIR DI Benchmarking and Performance Plan

## Objective

Measure ReSTIR DI cost repeatably across representative scenes, explain the
scene-dependent cost, optimize the triangle-light path, and compare identical
before/after runs. Every benchmark must load an environment map so environment
and emissive-triangle sampling are exercised together.

## Measurement contract

The exact renderer switches and JSON timing protocol are documented in
[`headless-cli.md`](headless-cli.md). Generated outputs belong under the
ignored `artifacts/` directory; only manifests and compact summaries are
eligible for version control.

The JSON manifest defines:

- the executable and output directory;
- defaults shared by all tests;
- model and environment-map paths;
- resolution, SPP, bounce count, NRD and ReSTIR DI/GI toggles;
- ReSTIR candidate counts and temporal/spatial reuse switches;
- warmup frames, measured frames, repetitions, and seed;
- one or more named camera poses per model.

Each model/pose/repetition is a separate process. RT2 emits GPU timestamp
records for completed frames without synchronizing the render loop. The runner
discards warmup records and summarizes the requested measurement window.

The structured result contains raw per-frame region timings plus count, mean,
median, standard deviation, minimum, maximum, p90, p95, and p99. Derived
metrics include combined DI, combined GI, milliseconds per megapixel, and each
stage's percentage of the GPU frame.

## Benchmark matrix

Use the same bright directional EXR for all cases and initially run at
930x730, one sample per pixel, eight bounces, raster-first, NRD, ReSTIR DI and
ReSTIR GI. Cover these scene classes:

1. no emissive triangles (Saepinum and Calcata);
2. many textured emissive triangles (Sci-fi Door Wall);
3. many emissive triangles and instances (Spaceship);
4. very large geometry buffers with a smaller light set (San Miguel).

Run at least 12 warmup and 30 measured frames. Use multiple poses when known;
the manifest permits a loaded/default camera pose when a stable explicit pose
is not yet available.

## Baseline protocol

1. Add timing export only; do not alter rendering or light selection.
2. Build Release and record the executable hash.
3. Run the full manifest and retain raw logs, captures, and `results.json`.
4. Confirm every test loaded its requested environment map and produced the
   requested number of timestamp samples.

## Issues to resolve

### Duplicate triangle reconstruction

Fresh DI candidates currently reconstruct the selected world-space triangle
once for target-density evaluation and again for proposal-PDF evaluation.
Fuse those calculations so geometry, normal, direction, distance, and texture
coordinates are loaded once per candidate.

### Sparse textured emitters

RT2 currently registers every triangle using a material with a non-zero
emissive factor. For textures containing large black regions this produces
large light lists full of zero-energy candidates. Build a conservative
CPU-side emissive occupancy mask and omit a triangle only when the expanded
texel bounding box of its UV triangle contains no emitting texel. This removes
provably black triangles without changing the estimator.

### Longer-term light sampling

After the safe changes are measured, add power-weighted triangle selection and
coherent presampled light tiles. Those require a proposal distribution/alias
table change and therefore belong in a separately validated optimization.

## Acceptance criteria

- all benchmark cases load an environment map and complete successfully;
- raw timing samples and aggregate statistics are machine readable;
- before and after use the same executable settings, poses, and manifest;
- image captures remain finite and visually equivalent apart from stochastic
  differences;
- triangle-heavy scenes show a material reduction in DI Temporal median;
- no-light/environment-only scenes do not regress materially.

## First execution (2026-07-17)

The matched baseline and optimized runs used the manifest in
`benchmarking/restir_di_benchmark.json`, the Kloofendal 4K EXR, 930x730,
12 warmup frames, and 30 measured frames per pose.

| Scene / pose | Baseline DI total | Optimized DI total | Change | Light-list change |
|---|---:|---:|---:|---:|
| Saepinum | 1.373 ms | 1.376 ms | +0.2% | 0 -> 0 |
| Calcata | 0.727 ms | 0.706 ms | -2.9% | 0 -> 0 |
| Sci-fi wall | 4.760 ms | 3.359 ms | -29.4% | 104,698 -> 54,609 |
| Spaceship | 12.735 ms | 11.736 ms | -7.8% | 263,040 -> 229,129 |
| San Miguel courtyard | 0.690 ms | 0.692 ms | +0.3% | 4,032 -> 4,032 |
| San Miguel blue wall | 0.903 ms | 0.911 ms | +1.0% | 4,032 -> 4,032 |

The stable Saepinum, Calcata, and San Miguel comparisons show no systematic
no-light or small-light-list regression.

The conservative occupancy filter removed 50,089 provably black textured
emitters from Sci-fi Wall and 33,911 from Spaceship. The remaining Spaceship
cost confirms that list filtering and duplicate-load removal are not enough
for large emissive working sets. The next performance slice should introduce
power-weighted selection from a compact light record, followed by coherent
presampled light tiles if random-access pressure remains dominant.
