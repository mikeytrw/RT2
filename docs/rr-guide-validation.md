---
title: "RR-neutral guide production validation"
---

# RR-neutral guide production validation

Grounded implementation checkpoint: `cdac3cc`, `7bc70bc`, plus the bounded
review-fixup commits on `rendering-dlss-rr-neutral-guides`. The tracked
machine-readable results are [4K](rr-guide-report-4k-rtx3090.json),
[non-NRD](rr-guide-report-256x256-nonnrd.json), and
[camera sweep](rr-guide-report-256x256-motion.json).

Reports were produced on 2026-08-31 with an RTX 3090 and
`sofa_and_lamp.glb` at native RenderExtent, raster-first, and NRD disabled.
The exact command, runtime case/NRD/camera/frame inputs, and exit code are
captured by the checked-in writer in the same seven-guide JSON object. Readback
is synchronous after `vkDeviceWaitIdle` and `vkCmdCopyImageToBuffer`, not a CPU
prediction; the report's two canonical reads are labelled internal readback
stability only. Canonical equivalence is accepted by the separate
`scripts/rr-guide-pair.ps1` harness, which runs no-report and report processes
with identical scene/seed/frame settings and retains both commands and hashes.

Measured result: five dedicated images/five allocations, actual RT2-owned
device-local allocation `213,909,504` bytes (204 MiB), under the `224 MiB`
ceiling. The fresh 4K report had zero nonfinite samples and zero remaining
producer sentinels; it records exact per-resource allocation sizes and numeric
ranges, plus `miss_with_nonzero_hit_count=0` and normal max-length error
`0.000775516`.

A second six-frame yaw run with `--camera-sweep 0.25 1 4` retained a separate
report with actual GPU motion magnitude up to `105.028` render pixels and
expected-observed error `0`. Motion is written once in render pixels, with no
jitter delta; CPU projection tests cover static, translation, yaw, rigid,
emissive, and sky cases at <=0.25 px.

CPU contract tests include named compiling RED/GREEN checks for unique
bindings, RenderExtent-only rows, the 24-byte corrected budget arithmetic,
shared material/F0/diffuse semantics, motion projection, and
canonical-output/debug independence. Release targeted guide tests passed
`12/12` cases and `147/147` assertions; earlier `66/69` counts are superseded.
The complete Release test run retains the repository baseline; Debug retains
the documented OBJ fixture-generation baseline.

Validation and synchronization-validation runs completed without new RR
guide layout/transfer-source errors. Existing repository validation warnings
remain recorded by the baseline (shader environment and synchronization2
messages). Allocation, budget, producer-pipeline, Vulkan format-feature,
report open/write/flush/close, and semantic faults are loud; the injectable
`RT2_RR_GUIDE_INJECT_CLOSE_FAILURE` path returns nonzero without the success
marker. No NGX feature was created or evaluated (`rr_feature_created=0`), and
Walnut/NVIDIA submodules were not modified.

The report's `valid`/`failures` fields validate finite ranges, explicit
preclear-sentinel coverage, per-pixel normal-length error, miss-to-zero-hit
correlation, and expected-versus-observed motion tolerance, alongside the
allocation budget and canonical checksum. The `R11G11B10F` amendment is
grounded by the checked-in pinned guide
`RT2App/vendor/DLSS/doc/DLSS-RR Integration Guide.pdf`, §3.4.5 (PDF page 13),
which specifies the noisy input as any standard 3-channel format. Startup
still checks selected-device STORAGE_IMAGE, SAMPLED_IMAGE, and TRANSFER_SRC/DST
support and fails before production if any bit is absent.

## 2026-08-31 decisive-review fixup

The sky guide helper now writes only guide/G-buffer resources. In non-NRD mode
the current sky sample is passed to `temporalAccumulate` before `outputImage`
is written; NRD mode stores the beauty sample explicitly after guide
production. Every report-mode frame preclears all five dedicated images plus
shared depth and motion to format-representable sentinels; the report rejects
any remaining sentinel across all seven rows.
Normal length, depth-miss/zero-hit correlation, motion density/magnitude, and
static-or-camera-sweep expected-versus-observed error are computed from actual
GPU readback bytes.

The retained non-NRD and camera-sweep JSON files are direct outputs of the
checked-in CLI/reporter, with no post-processing or hand-shaped summary. They
retain all seven guide rows, GPU/device provenance, runtime inputs, complete
format feature bits, exact command, and exit code. The static case reports
`sentinel_remaining_count=0` and `miss_with_nonzero_hit_count=0`; the camera-
yaw case records actual motion up to `105.028` render pixels with a nonzero
class-density check. The paired harness is the canonical-output acceptance;
adjacent readbacks in a report are not overclaimed as that proof.
Moving reports classify sky/geometry/emissive coverage from actual shared
direct-emission and guide readbacks and retain per-class motion counters.

## Reproducible gates

The bounded fixup gates were run with these commands (the three report files
were produced by the paired harness, so each report process consumed the
manifest from its separate no-report process; all returned exit code
0 except the intentional close-fault mutant, which returned 1):

```text
msbuild RT2App.sln -p:Configuration=Release -p:Platform=x64 -m
msbuild RT2App.sln -p:Configuration=Debug -p:Platform=x64 -m
bin\Release-windows-x86_64\RT2Tests\RT2Tests.exe                 # full suite
bin\Release-windows-x86_64\RT2Tests\RT2Tests.exe --test-case="RR guides*"
bin\Debug-windows-x86_64\RT2Tests\RT2Tests.exe --test-case="RR guides*"
scripts\rr-guide-pair.ps1 ... -Report docs\rr-guide-report-4k-rtx3090.json # 0
scripts\rr-guide-pair.ps1 ... -Report docs\rr-guide-report-256x256-nonnrd.json # 0
scripts\rr-guide-pair.ps1 ... -Frames 6 -CameraSweepAmplitude 0.25 -CameraSweepWarmup 1 -CameraSweepPeriod 4 -CameraSweepMode yaw -Report docs\rr-guide-report-256x256-motion.json # 0
bin\Debug-windows-x86_64\RT2App\RT2App.exe --headless --scene C:\Users\mikey\Downloads\sofa_and_lamp.glb --width 128 --height 128 --frames 1 --spp 1 --bounces 1 --raster-first --validate --sync-validate --rr-guide-report artifacts\rr-validation.json
RT2_RR_GUIDE_INJECT_CLOSE_FAILURE=1 bin\Release-windows-x86_64\RT2App\RT2App.exe --headless --scene C:\Users\mikey\Downloads\sofa_and_lamp.glb --width 64 --height 64 --frames 1 --spp 1 --bounces 1 --raster-first --rr-guide-report artifacts\rr-close-fault.json # 1
graphify update .
git diff --check
git status --short                                      # clean after fixture/report cleanup
```

Durable paired canonical-output gate:

```text
powershell -File scripts\rr-guide-pair.ps1 -Executable bin\Release-windows-x86_64\RT2App\RT2App.exe -Scene C:\Users\mikey\Downloads\sofa_and_lamp.glb -Manifest artifacts\rr-pair.manifest -Report artifacts\rr-pair-report.json -Evidence artifacts\rr-pair-evidence.json
```

The validation run additionally enables Vulkan validation and synchronization
validation; generated runtime reports and fixtures were removed after each
gate so the commit contains only frozen evidence.

The ledger correction is append-only in `rr-guide-resource-ledger.md`: the
original 28-byte table used 2048 instead of 2160 for the 4K payload. Production
uses three-channel `B10G11R11_UFLOAT_PACK32` noisy HDR (24 bytes/pixel total)
and rejects measured allocations above the ceiling. Shared depth remains
positive view distance and shared motion remains the existing RG16F resource;
neither is aliased into an NRD slot.
