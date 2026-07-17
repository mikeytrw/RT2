# Render Measurement Harness

RT2's measurement harness turns renderer settings into repeatable captures and
machine-readable evidence. It is intended for estimator, denoiser, temporal,
and performance work; it does not replace interactive visual inspection.

## Native capture options

Headless mode supports both presentation and transport outputs:

```text
--output <file.png>       Reinhard-tonemapped, exact-sRGB display image
--output-hdr <file.exr>   Linear RGB, full 32-bit float EXR
--output-hdr <file.pfm>   Linear RGB, full 32-bit float PFM
--seed <integer>          Deterministic sampling stream selector
```

The seed is mixed into the conventional path-tracing, ReSTIR DI, and ReSTIR
GI random streams. Frame number remains a separate dimension, so a fixed seed
produces a repeatable sequence rather than identical noise on every frame.

Camera sequences can move for a fixed number of complete cycles and then
return to the supplied pose:

```text
--camera-sweep <amplitude> <warmup> <period>
--camera-sweep-mode <lateral|forward|yaw>
--camera-sweep-cycles <count>
--capture-every <frames>
```

With `--camera-sweep-cycles 1`, captures are tagged `still`, `move`, and
`hold`. This prevents a correct final stationary image from hiding a failure
which occurs only while the camera is moving.

## Manifest runner

The example manifest at
[`scripts/render_harness.example.json`](../scripts/render_harness.example.json)
contains a high-SPP reference and a ReSTIR DI motion case. Paths in a manifest
are resolved relative to that manifest; absolute Windows paths are accepted.

Run it with the bundled Codex Python or any Python containing NumPy and Pillow:

```powershell
python scripts/render_harness.py scripts/render_harness.example.json
```

Useful options:

```text
--case <name>          Run only a selected case; repeat for several cases
--app <path>           Override the executable in the manifest
--output-dir <path>    Override the timestamped run-output directory
--dry-run              Print commands without launching RT2
--update-baselines     Replace baseline_dir with successful case artifacts
```

Install the two analysis dependencies, when required, with:

```powershell
python -m pip install -r scripts/requirements-render-harness.txt
```

## Manifest fields

Top-level fields are `app`, `output_dir`, `baseline_dir`, `defaults`, and
`cases`. Each case requires a unique `name`; values in a case override
`defaults` recursively.

Common case fields are:

- `scene`, `env`, `width`, `height`, `frames`, `spp`, `bounces`, and `seed`;
- `camera.position` and `camera.forward`;
- `raster_first`, `nrd`, `no_accumulate`, `restir_di`, and `restir_gi`;
- `restir_candidates`, `restir_gi_candidates`, and ReSTIR reuse switches;
- `sweep.amplitude`, `mode`, `warmup`, `period`, `cycles`, `hold_frames`, and
  `capture_every`;
- `reference`, naming another case or a PFM file to compare;
- `extra_args`, for renderer switches which have not yet gained named fields.

When `frames` is omitted, it is derived from warmup, cycles, period, and the
stationary tail.

## Outputs and metrics

Each timestamped run contains per-case PNG/PFM captures, sequence frames, the
renderer log, and a SHA-256 hash. `report.json` records the full command,
return status, wall time, every GPU timestamp region, image statistics,
temporal frame-to-frame deltas, and reference comparison metrics.

The compact `report.md` table reports:

- GPU frame time;
- mean linear luminance and the 99.99th percentile;
- linear-RGB relative MSE against the selected reference;
- PSNR after RT2's fixed Reinhard plus sRGB display transform.

Comparisons also produce a robustly exposed difference heatmap. PFM is used
for automated analysis because it is a simple, lossless float format that
NumPy can read without an EXR binding. EXR is available for external tools.

## Current boundary

This slice covers deterministic capture, linear HDR output, repeatable
motion-plus-hold paths, timestamp collection, image metrics, temporal deltas,
hashes, difference images, and baseline storage. Phase 0 still needs native
GPU counters for ray families, invalid samples, history rejection and age,
plus VRAM telemetry and checked-in known-good scene baselines.

## Performance benchmark runner

For multi-scene GPU timing studies, use `scripts/perf_benchmark.py` with a
manifest such as `benchmarking/restir_di_benchmark.json`. Unlike the image
comparison harness, the performance runner accepts an array of camera poses
per model, requires an environment map, records a warmup-free window of raw
per-frame GPU timestamps, and writes `results.json`, `summary.csv`, and
`report.md`. Pass `--baseline <results.json>` to produce matched median deltas.
