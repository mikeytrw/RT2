---
title: "RR-neutral guide production validation"
---

# RR-neutral guide production validation

Grounded implementation checkpoint: `cdac3cc` plus the bounded W3 guide
changes on `rendering-dlss-rr-neutral-guides`. The tracked machine-readable
4K result is [rr-guide-report-4k-rtx3090.json](rr-guide-report-4k-rtx3090.json).

The report was produced on 2026-08-31 with an RTX 3090, native
`4096x2160` RenderExtent, `sofa_and_lamp.glb`, raster-first, NRD enabled,
one frame, and `--rr-guide-report`. It is synchronous GPU readback after
`vkDeviceWaitIdle` and `vkCmdCopyImageToBuffer`, not a CPU prediction.

Measured result: five dedicated images/five allocations, actual RT2-owned
device-local allocation `213,909,504` bytes (204 MiB), under the `224 MiB`
ceiling. Every guide had zero nonfinite samples; noisy HDR had 3,618 nonzero
channels, diffuse albedo 9,069,303, specular albedo 35,389,440, and hit
distance 12,376 nonzero pixels. The report records the exact per-resource
allocation sizes and numeric ranges.

A second two-frame RTX 3090 run with `--camera-sweep 0.1 0 4` produced 859
nonzero motion channels (versus zero on the static pose), exercising the
camera/sky and moving-geometry path while keeping jitter ownership in the
existing raster motion calculation.

CPU contract tests include named RED/GREEN checks for unique bindings,
RenderExtent-only rows, the 24-byte corrected budget arithmetic, shared
`EnvBRDFApprox2`, and canonical-output/debug independence. Release targeted
guide tests passed `6/6` cases and `66/66` assertions. The complete Release
test run retains the repository baseline; Debug retains the documented OBJ
fixture-generation baseline.

Validation and synchronization-validation runs completed without new RR
guide layout/transfer-source errors. Existing repository validation warnings
remain recorded by the baseline (shader environment and synchronization2
messages). No NGX feature was created or evaluated (`rr_feature_created=0`),
and Walnut/NVIDIA submodules were not modified.

The ledger correction is append-only in `rr-guide-resource-ledger.md`: the
original 28-byte table used 2048 instead of 2160 for the 4K payload. Production
uses three-channel `B10G11R11_UFLOAT_PACK32` noisy HDR (24 bytes/pixel total)
and rejects measured allocations above the ceiling. Shared depth remains
positive view distance and shared motion remains the existing RG16F resource;
neither is aliased into an NRD slot.
