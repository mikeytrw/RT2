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
