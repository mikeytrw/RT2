# Headless CLI and Automation

RT2 can render headlessly, save display and linear-HDR images, run scripted
camera motion, and emit GPU timing records. This is the command-line contract
used by the render-comparison harness and performance benchmark runner.

## Basic invocation

```powershell
.\bin\Release-windows-x86_64\RT2App\RT2App.exe --headless `
  --scene C:\path\to\scene.glb `
  --env C:\path\to\environment.exr `
  --output artifacts\manual\frame.png `
  --output-hdr artifacts\manual\frame.pfm `
  --frames 8 --width 1280 --height 720 --spp 1 --bounces 8
```

`--headless` renders the requested frames, writes outputs, then exits. With no
`--frames`, it renders one frame. With no output option, it writes
`screenshot.png`; use an explicit ignored `artifacts/` path instead.

`--output` writes a Reinhard-tonemapped sRGB PNG. `--output-hdr` writes the
linear beauty output as `.exr` or `.pfm`; PFM is the preferred format for the
Python comparison harness.

## Scene, render, and denoiser options

| Option | Meaning |
|---|---|
| `--scene <path>`, `-s <path>` | Load a `.glb`, `.gltf`, or `.obj` scene. |
| `--project <path.rt2proj>`, `-p <path.rt2proj>` | Load a portable project and its startup scene. With `--scene`, the scene is an asset-root-relative `.rt2scene` locator. |
| `--env <path>`, `-e <path>` | Load an HDR or EXR environment map. |
| `--output <path>`, `-o <path>` | Save the display PNG. |
| `--output-hdr <path>` | Save the linear HDR image as EXR or PFM. |
| `--frames <N>`, `-f <N>` | Number of frames to render. |
| `--width <N>`, `-w <N>` / `--height <N>`, `-h <N>` | Headless render resolution. |
| `--spp <N>` | Samples-per-pixel setting. |
| `--bounces <N>` | Maximum path depth. |
| `--seed <N>` | Sampling seed; decimal and `0x`-prefixed values are accepted. |
| `--raster-first` | Enable raster-first primary visibility. |
| `--nrd` | Enable NRD denoising. |
| `--nrd-accum-frames <N>` | Override NRD maximum history length. |
| `--nrd-responsive-roughness <R>` | Override responsive-history roughness. |
| `--nrd-responsive-min-frames <N>` | Override responsive minimum history. |
| `--no-accumulate` | Disable non-NRD beauty accumulation. |
| `--gbuffer-debug <N>` | Select a G-buffer debug mode. |

The seed selects a deterministic random stream. Frame index stays separate, so
the same seed reproduces a sequence instead of repeating identical noise.

## ReSTIR options

| Option | Meaning |
|---|---|
| `--restir` | Enable ReSTIR DI and raster-first. |
| `--ris` | Backwards-compatible alias for `--restir`. |
| `--restir-candidates <N>` | Fresh DI candidates per pixel. |
| `--restir-no-temporal` | Disable DI temporal reuse. |
| `--restir-no-spatial` | Disable DI spatial reuse. |
| `--restir-gi` | Enable ReSTIR GI and raster-first. |
| `--restir-gi-candidates <N>` | Fresh GI candidates per pixel. |

For an A/B comparison, set ReSTIR mode and candidate counts explicitly.

## Camera pose and motion sequence

| Option | Meaning |
|---|---|
| `--camera-pos <x> <y> <z>` | Override loaded camera position. |
| `--camera-forward <x> <y> <z>` | Override loaded camera forward direction. |
| `--camera-sweep <amplitude> <warmup> <period>` | Animate a sweep after stationary warmup; `period` is frames per full cycle. |
| `--camera-sweep-mode <lateral\|forward\|yaw>` | Choose lateral (default), forward, or yaw motion. Yaw amplitude is radians. |
| `--camera-sweep-cycles <N>` | Run N cycles, return to base pose, then hold. `0` repeats. |
| `--capture-every <N>` | Save sequence outputs every N motion/hold frames. |

With a finite cycle count, sequence files gain `_still`, `_move_####`, or
`_hold_####` suffixes. The un-suffixed output is also saved at the end. This
keeps moving-camera failures distinct from the final stationary image.

## Benchmark and diagnostic options

| Option | Meaning |
|---|---|
| `--benchmark-timings` | In headless mode, emit a JSON `HeadlessTiming` record per completed GPU frame. |
| `--verbose`, `-v` | Print startup and per-frame information. |
| `--validate` | Enable Vulkan validation layers. |
| `--sync-validate` | Enable synchronization validation; implies `--validate`. |
| `--list`, `--dry-run` | Print selected scene/environment instead of loading them. |
| `--help`, `-?` | Print the executable's built-in option summary. |

Timing records contain a frame index and the regions that ran: GPU frame,
raster, ReSTIR DI temporal/spatial, ReSTIR GI temporal/history, RT shading,
NRD, compose, and tone map. Timestamp readback uses a two-frame ring, so these
records represent completed GPU frames rather than CPU frame time.

## Automation entry points

- [`render_measurement_harness.md`](render_measurement_harness.md) covers
  image-quality and temporal-regression testing with `scripts/render_harness.py`.
- [`restir_di_performance_plan.md`](restir_di_performance_plan.md) covers
  multi-scene timing tests with `scripts/perf_benchmark.py`.

Both runners write under ignored `artifacts/` or `baselines/` directories.
Commit manifests, scripts, and compact reports only; never commit captures,
PFM/EXR images, renderer logs, or generated result directories.

## RT2SliceRunner — CPU-only vertical slice verification

`RT2SliceRunner` is a standalone console target that links only CPU scene
code (no Vulkan, Walnut, ImGui, GLFW, NRD, or NRI). It loads a `.rt2scene`
file, enters Play, runs N fixed update steps, Stops, and verifies that the
authoring scene is unchanged. It emits a JSON report with per-entity final
runtime transforms and exits non-zero on failure.

It also accepts a project context. `--project` without `--scene` requires the
project's `startupScene`; when both are supplied, `--scene` is interpreted
relative to the project asset root and cannot escape it. Standalone
`--scene` remains an ordinary filesystem path.

```powershell
.\bin\Release-windows-x86_64\RT2SliceRunner\RT2SliceRunner.exe `
  --scene RT2App\assets\vertical-slice.rt2scene `
  --steps 60 `
  --out artifacts\slice_report.json
```

| Option | Meaning |
|---|---|
| `--scene <path>` | `.rt2scene` file to load and run. |
| `--project <path>` | Load `.rt2proj`; use its startup scene or interpret `--scene` below its asset root. |
| `--steps <N>` | Number of fixed update steps (default 60). |
| `--out <path>` | Write JSON report to file instead of stdout. |
| `--help` | Print usage. |

The regression script `run_slice_test.ps1` invokes the slice runner on the
checked-in fixture with 60 steps and asserts exit code 0, authoring
intactness, and the expected final cube transform (x ≈ 1.0).

`RT2SliceRunner` links `SceneAssetResolver` (CPU-only) so it can resolve
imported assets and environment maps referenced by a `.rt2scene` file without
requiring Vulkan. The JSON report fields are unchanged in Phase 1A; the
runner continues to emit `scene`, `steps`, `authoringIntact`, `bridge`, and
`runtimeTransforms`. If a loaded scene references external assets, the
runner loads them through the same CPU importer path used by the editor.

### Phase 1B recovery regression

```powershell
.\bin\Release-windows-x86_64\RT2SliceRunner\RT2SliceRunner.exe `
  --recovery-scenario `
  --out artifacts\recovery_report.json
```

| Option | Meaning |
|---|---|
| `--recovery-scenario` | Run the Phase 1B recovery regression scenario. |
| `--out <path>` | Write JSON report to file instead of stdout. |

The scenario generates and loads a tiny textured GLB plus a tiny EXR
environment, saves the native scene explicitly, authors a transform and
material override, advances an injected clock through the autosave interval,
drops the session (simulating an unclean exit), and restores the atomic
recovery envelope. It verifies imported-asset provenance, texture and decoded
environment data, the override, transform, UUID, and dirty state; proves the
explicit file remains byte-for-byte unchanged; then discards the record.
Emits:

```json
{
  "recoveryScenario": "pass",
  "assetBacked": true,
  "workDir": "<temp dir>"
}
```

Exit code is 0 on pass, 1 on fail. The regression script
`run_recovery_test.ps1` invokes this scenario and asserts the report
contains both `"recoveryScenario": "pass"` and `"assetBacked": true`.
