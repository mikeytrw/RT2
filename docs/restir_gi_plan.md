# ReSTIR GI Implementation Plan

## Status and Intent

This document defines an implementation-ready, temporal-first prototype for
one-bounce diffuse global illumination in RT2.

The design is inspired by Ouyang et al. 2021, "ReSTIR GI: Path Resampling for
Real-Time Path Tracing," but it is intentionally narrower:

- one diffuse indirect bounce;
- temporal reuse first;
- no spatial path shifting in the initial implementation;
- raster-first mode only;
- the existing stochastic diffuse/specular lobe policy remains in place.

This should therefore be described as a ReSTIR-style one-bounce GI estimator,
not as a complete reproduction of the published multi-bounce ReSTIR GI
algorithm.

Reference:

- https://doi.org/10.1111/cgf.14378

## Goal

For a diffuse primary-scatter event, estimate the path

`camera -> primary receiver -> secondary surface or environment -> light`

using a per-primary-pixel GI reservoir. The reservoir stores a complete
evaluated sample: the selected direction, its current outgoing radiance
estimate, its hit distance, the random state needed to reproduce the
secondary direct-light sample, and the usual reservoir statistics.

Temporal reuse amortizes the expensive secondary traversal across frames.
Final RT shading consumes the evaluated sample directly and does not trace the
selected direction a third time.

## Scope

- **One-bounce diffuse GI.** A GI sample traces from the primary receiver to a
  secondary surface or environment. At a secondary surface, it evaluates
  emission plus one direct-light NEE sample and then terminates.
- **Existing lobe mixture preserved.** When the primary scatter selects the
  diffuse lobe, the raygen shader consumes the GI reservoir and compensates
  for the diffuse-lobe selection probability. When it selects a specular or
  transmission lobe, the existing recursive BSDF path remains unchanged.
- **Raster-first mode only.** The GI compute pass requires the primary
  G-buffer. RT-primary rendering remains unchanged.
- **Temporal reuse first.** Spatial reuse needs path shifting/reconnection and
  is deferred until the temporal estimator is validated.
- **NRD integration.** Diffuse GI radiance and the selected GI hit distance
  feed the existing diffuse NRD channel. Specular/transmission paths continue
  to feed the specular channel.
- **No secondary-screen DI lookup.** Current DI reservoirs represent visible
  primary receivers and cannot be assumed to exist at arbitrary secondary
  surfaces.

## Non-Goals

- Full multi-bounce ReSTIR GI or ReSTIR PT.
- Spatial reuse by simply copying a neighbor's direction.
- Rough-specular or transmission reservoirs.
- Replacing the existing ReSTIR DI pipeline.
- Adding new G-buffer images unless profiling later justifies a shared
  primary-lobe mask.

## Current Architecture

```text
Frame:
  1. Raster G-buffer
  2. ReSTIR DI temporal compute
  3. ReSTIR DI spatial compute
  4. RT shading
     - primary direct lighting from the DI reservoir
     - stochastic diffuse/specular/transmission scatter
     - recursive secondary path
  5. NRD denoise
  6. Compose/remodulate
  7. Tonemap
```

The GI pass is inserted after DI and before RT shading. It does not consume a
DI reservoir at the secondary hit.

## Sample Domain and Estimator

### Receiver and Sample

The reservoir is stored per primary pixel.

- **Receiver:** the current primary world position, shading normal, geometric
  normal, view direction, and material.
- **Sample:** a primary-scatter direction together with a root random seed or
  compact key that reproduces every stochastic choice used to evaluate it.
- **Evaluated payload:** the current `Lo` along that direction and its hit
  distance.

Storing the root sample seed is important. Re-evaluating a history direction
with unrelated random light or alpha choices would change the sample being
weighted. Derive independent, salted substreams for radiance-ray alpha,
secondary NEE, and NEE-shadow alpha. Replay those substreams during history
re-evaluation. Scene topology or light-list changes invalidate all GI history.

The secondary random variables are part of an extended sample domain. In that
domain the proposal is `q_direction * q_random`, with `q_random = 1` for a
uniform root random-number sample. The light-selection and geometry PDFs are
already accounted for by the NEE estimator stored in `Lo`. If the seed is
later replaced by an explicit light sample key, its proposal density must be
included consistently instead of assuming `q_random = 1`.

### Diffuse Contribution

For selected direction `wi`:

```text
cosTheta = max(dot(N, wi), 0)
C        = f_diffuse(wo, wi) * Lo(wi) * cosTheta
p_hat    = luminance(C)
q        = pdf_diffuse(wi) = cosTheta / pi
w        = p_hat / q
```

The cosine term is part of both the target density and the final vector
contribution. It must not be omitted merely because the proposal is
cosine-weighted.

The reservoir estimator is:

```text
W          = weightSum / (M * targetPdf)
diffuseGI  = C_selected * W
pixelGI    = diffuseGI / P_diffuse_lobe
```

`P_diffuse_lobe` is the probability with which RT2's existing material scatter
logic selected the diffuse lobe. Dividing by it preserves the current
one-lobe mixture estimator. If `P_diffuse_lobe <= 0`, the GI reservoir is not
consumed.

The GI target contains only the diffuse primary BRDF component. It must not
use the combined diffuse-plus-specular BRDF.

### Evaluating `Lo`

Trace the candidate direction with `rayQueryEXT`.

- **Environment miss:** `Lo` is the environment radiance in the candidate
  direction; `hitT` uses RT2's existing NRD miss convention.
- **Emissive hit:** include the hit surface's emitted radiance.
- **Non-emissive hit:** evaluate one standard direct-light NEE sample at the
  secondary surface, including its secondary BRDF, cosine, light PDF and
  visibility.
- **Emissive non-terminal hit:** `Lo = emission + secondary NEE`.
- **No further recursive bounce:** this prototype terminates after the
  secondary surface's direct-light estimate.

The secondary NEE sample must be generated from a dedicated stored seed.
Replaying the seed during temporal re-evaluation preserves the extended
sample. A later implementation may store an explicit light sample key instead
if light-distribution changes make seed replay unsuitable.

## Updated Execution Topology

```text
Raster G-buffer
  -> ReSTIR DI temporal compute
  -> ReSTIR DI spatial compute
  -> ReSTIR GI temporal compute
       - fresh direction + secondary NEE sample
       - optional history re-evaluation and canonical merge
       - write current evaluated Lo and hitT into output reservoir
       - write current primary receiver history
  -> RT shading
       - primary direct lighting from ReSTIR DI
       - diffuse lobe: consume stored GI sample; no GI retrace
       - specular/transmission lobe: existing recursive BSDF path
       - GI disabled/invalid fallback: existing secondary path
  -> NRD
  -> Compose/remodulate
  -> Tonemap
```

For a diffuse GI sample there is no recursive depth 2+ path after the GI
secondary hit. Recursive depth 2+ remains only for GI-disabled rendering,
invalid-reservoir fallback, and the existing specular/transmission path.

## GPU Data Layout

### `SIGIReservoir`

Use a 48-byte baseline layout. It deliberately stores evaluated radiance and
hit distance so final shading does not retrace the selected direction.

```c
// 48 bytes: 3 x uvec4.
struct SIGIReservoir
{
    SI_UVEC4 data0;  // xyz = floatBitsToUint(direction.xyz)
                     // w   = floatBitsToUint(hitT)

    SI_UVEC4 data1;  // xyz = floatBitsToUint(Lo.xyz)
                     // w   = floatBitsToUint(weightSum)

    SI_UVEC4 data2;  // x = floatBitsToUint(targetPdf)
                     // y = M
                     // z = packed age/flags/sample type
                     // w = root sample seed or compact sample key
};
```

Required flags distinguish at least:

- empty/invalid;
- environment miss;
- geometry hit;
- history-derived sample;
- whether secondary NEE was valid.

When a streamed or merged candidate is selected, all sample payload fields
(direction, `Lo`, `hitT` and root seed/key) must be copied together.

Possible later packing, after correctness is established:

- octahedral direction encoding;
- packed HDR radiance;
- reduced hit-distance precision;
- a 32-byte reservoir.

Do not start with packed radiance because it makes numerical validation more
difficult.

### Receiver History

Reuse the existing 32-byte `SISurfaceHistory` layout, but allocate a GI-owned
history region because the DI pass overwrites its own surface history before
the GI pass executes.

```c
// Existing 32-byte layout.
struct SISurfaceHistory
{
    SI_UVEC4 data0;  // packed normal, viewZ, material ID, validity/flags
    SI_UVEC4 data1;  // world position.xyz, padding
};
```

Temporal validation is receiver-based. Do not store or compare a newly
randomized current scatter direction.

### One Monolithic GI Buffer at Binding 11

Set 0's texture array at binding 18 is variable descriptor count and must
remain the highest binding. Therefore do not add bindings 19-21.

Use the currently unused set-0 binding 11:

```c
#define SI_BINDING_GI_DATA 11
```

One storage buffer contains three aligned regions:

```text
reservoir A       pixelCount * 48 bytes
reservoir B       pixelCount * 48 bytes
receiver history  pixelCount * 32 bytes
```

Reservoir A/B ping-pong by frame parity. Shader helpers calculate region
offsets in `uvec4` units, so internal region starts need 16-byte alignment.
`minStorageBufferOffsetAlignment` applies only if the implementation later
binds regions through nonzero or dynamic descriptor offsets; it is not needed
for manual indexing through one whole-buffer descriptor.

This layout is legal with the existing variable texture array and requires
only one descriptor write.

### Memory Cost

The baseline is 128 bytes per full-resolution pixel:

```text
2 * 48-byte reservoirs + 32-byte receiver history = 128 bytes/pixel
```

| Resolution | Decimal | Binary |
|---|---:|---:|
| 1920 x 1080 | 265.42 MB | 253.13 MiB |
| 3840 x 2160 | 1061.68 MB | 1012.50 MiB (0.99 GiB) |

A future 32-byte packed reservoir reduces this to 96 bytes/pixel, which is
still 199.07 MB / 189.84 MiB at 1080p. Half-resolution baseline GI uses one
quarter of the full-resolution storage, approximately 63.3 MiB at 1080p
output.

## Shader and Pipeline Interfaces

### GI Push Constants

Use a GI-specific push-constant struct owned by `ReSTIRGIPass`. Do not grow
`SIReSTIRPushConstants`, because the DI and GI compute pipelines are
independent.

```c
struct SIGIPushConstants
{
    SI_UINT freshCandidateCount;
    SI_UINT temporalMCap;
    SI_UINT maxTemporalAge;
    SI_UINT flags;                 // bit 0: temporal enabled

    SI_FLOAT depthThreshold;
    SI_FLOAT normalThreshold;
    SI_FLOAT worldPosThreshold;
    SI_UINT frameIndex;

    SI_VEC4 jitter;                // current.xy, previous.zw if required
};
```

Expected size: 48 bytes. Add a C++ static assertion.

### RT-Shading Controls

`secondary_raygen.rgen` needs only:

- GI enabled;
- current reservoir region/parity.

Use the two spare fields in `SINRDUniformData`, renaming them to explicit GI
fields, rather than adding a push-constant range to the RT pipeline:

```c
struct SINRDUniformData
{
    SI_UINT nrdEnabled;
    SI_UINT lobeDither;
    SI_UINT restirGIEnabled;
    SI_UINT restirGIReservoirIndex;
};
```

### Descriptor Stage Visibility

Change the set-0 TLAS binding stage flags to include
`VK_SHADER_STAGE_COMPUTE_BIT`. The GI data binding must be visible to compute
and raygen stages. Existing scene buffers and textures needed by the GI
compute shader must also retain compute visibility.

## Ray Queries from Compute

`VK_KHR_ray_query` and its feature are enabled during Walnut device creation.
`NRDIntegration.cpp` only tells NRI which device extensions are already
enabled; it is not the enabling location.

The GI pass uses `GL_EXT_ray_query` and the existing TLAS. A ray query performs
intersection traversal only; it does not invoke RT2's closest-hit or any-hit
shaders.

### Required Hit Reconstruction

Add shared shader helpers that reconstruct from the committed intersection:

- instance and primitive indices;
- triangle indices and barycentrics;
- object/world positions;
- geometric and shading normals;
- UVs and tangent basis as required;
- instance material index plus material offset;
- base color, metallic, roughness, emission and normal textures;
- front/back-face orientation.

The GI binding include must expose the TLAS, normal buffer, instance material
indices and instance material offsets in addition to the resources already
used by ReSTIR DI.

### Alpha-Tested Geometry

Ray-query candidate intersections must reproduce `anyhit.rahit` and
`shadow.rahit` semantics:

1. inspect each candidate triangle;
2. reconstruct UV and material;
3. sample base-alpha;
4. reject MASK intersections below cutoff;
5. reproduce the existing stochastic BLEND policy with a dedicated traversal
   seed;
6. call `rayQueryConfirmIntersectionEXT` only for accepted candidates.

Using an opaque ray flag without this loop would incorrectly make cutout and
blended geometry opaque.

### Secondary NEE Visibility

Secondary NEE visibility is another ray query. Its alpha behavior must match
the existing shadow any-hit shader.

## Fresh Candidate Generation

For every valid, non-emissive primary receiver with a nonzero diffuse
component:

1. Read the primary G-buffer and reconstruct the receiver material.
2. Generate `M` cosine-weighted diffuse directions.
3. For each direction:
   1. reserve and store a root sample seed, then derive independent alpha and
      secondary-NEE substreams;
   2. trace the direction with a radiance ray query;
   3. evaluate environment, emission and/or one secondary NEE sample;
   4. compute `C = f_diffuse * Lo * cosTheta`;
   5. compute `p_hat = luminance(C)` and `q = cosTheta / pi`;
   6. stream `w = p_hat / q` into the reservoir;
   7. copy the full payload if the candidate is selected.
4. Sanitize non-finite and non-positive values.
5. Write the current ping-pong reservoir region.

The first implementation may generate GI for every eligible diffuse-capable
material even if RT shading later selects the specular lobe. This is correct
but wastes work. If profiling shows this is significant, add a shared,
deterministic primary-lobe decision or a compact lobe mask used by both the
GI compute pass and raygen. Do not independently randomize the lobe in the two
passes.

## Temporal Reuse

For each current receiver:

1. Reproject through the existing motion vector.
2. Read the previous GI receiver history and reservoir.
3. Validate:
   - previous sample is valid;
   - normal similarity;
   - relative depth;
   - world-position distance;
   - material ID;
   - history age;
   - reprojection is in bounds.
4. Do not compare the history direction with a fresh random direction.
5. Re-evaluate the historical sample at the current receiver:
   - trace the stored direction;
   - replay the stored root seed/key and its deterministic substreams;
   - compute current `Lo`, `hitT`, `C` and `p_hat`.
6. Canonically merge using the current target:
   `w_merge = p_hat_current * W_history * M_capped`.
7. If history is selected, store the newly evaluated current `Lo` and `hitT`,
   not the stale previous-frame values.
8. Write current receiver metadata for the next frame.
9. Increment age only when history contributes; reset it for fresh-only output.

History must be invalidated on:

- scene or light-list topology changes;
- environment-map change;
- resize or render-resolution change;
- camera mode change between raster-first and RT-primary;
- material/texture replacement;
- GI setting changes that alter the sample domain;
- explicit renderer restart.

Rigid motion and moving lights should normally be handled by reprojection and
re-evaluation, but their object/material identity must remain stable.

## Final Shading

When the current scatter event is diffuse and the GI reservoir is valid:

```glsl
SIGIReservoir r = loadCurrentGIReservoir(pixel);
vec3 wi = giDirection(r);
vec3 Lo = giRadiance(r);
float hitT = giHitT(r);
float W = giReservoirW(r);

float cosTheta = max(dot(N, wi), 0.0);
vec3 C = evalDiffuseBRDFOnly(wo, wi, N, material) * Lo * cosTheta;
vec3 indirectRad = C * W / max(P_d, GI_EPSILON);
```

No `traceRayEXT` is issued for this diffuse GI sample during final shading.

If the reservoir is empty or invalid, use the existing diffuse secondary path
as a correctness fallback and record a debug counter. Once the implementation
is stable, profiling can determine whether a zero-contribution fallback is
preferable.

When the current lobe is specular or transmission, use the existing scatter
direction and recursive trace unchanged.

## Ray-Cost Accounting

Count radiance and shadow traversals separately.

For `M = 1` on a diffuse pixel with accepted history:

- one fresh secondary radiance query;
- one history re-evaluation radiance query;
- up to one secondary NEE shadow query for each radiance query;
- zero final-shading GI retraces.

That is two radiance queries and up to two shadow queries. Misses and emissive
hits can avoid the NEE shadow query.

Fresh-only mode uses one radiance query plus up to one shadow query. An
invalid-reservoir fallback adds the existing secondary path. Pixels whose
specular lobe is selected may still pay for unused GI compute until the
optional shared lobe mask is implemented.

Compare GPU time and traversal counts against the current path tracer; do not
compare only nominal bounce counts.

## NRD Integration

- Diffuse GI goes to `gDiffRadianceHitDist`.
- The selected reservoir's `hitT` goes with that same diffuse sample.
- Existing specular/transmission indirect remains in
  `gSpecRadianceHitDist`.
- Match current NRD conventions for environment misses and invalid hit
  distance.
- ReSTIR temporal reuse does not mathematically double-count NRD history, but
  it increases temporal correlation. Test responsiveness, disocclusion and
  ghosting rather than assuming the two histories are independent.
- All GI invalidation paths that affect image history must also request the
  appropriate NRD reset.

Compose remains a remodulation pass. Tonemapping remains a separate pass.

## Resources and C++ Ownership

### New Files

| File | Purpose |
|---|---|
| `RT2App/shaders/restir_gi_shared.glsl` | GI reservoir access, streaming, target and validation helpers |
| `RT2App/shaders/restir_gi_temporal.comp` | Fresh generation and temporal reuse |
| `RT2App/shaders/restir_gi_bindings.glsl` | GI buffer, TLAS, scene and G-buffer declarations |
| `RT2App/shaders/ray_query_scene.glsl` | Shared ray-query hit reconstruction and alpha traversal |
| `RT2App/src/ReservoirGIResources.h/.cpp` | Monolithic GI buffer allocation, aligned region layout and clears |
| `RT2App/src/ReSTIRGIPass.h/.cpp` | GI compute pipeline and dispatch |

### Files to Modify

| File | Changes |
|---|---|
| `RT2App/shaders/shader_interface.h` | Add binding 11, `SIGIReservoir` and `SIGIPushConstants`; name the spare NRD UBO fields |
| `RT2App/src/PathTracePass.cpp` | Add binding 11, add compute visibility to TLAS, write the GI descriptor |
| `RT2App/src/RenderSettings.h` | Add GI controls |
| `RT2App/src/RendererGPU.h/.cpp` | Own GI resources/pass, update settings and centralized invalidation |
| `RT2App/src/FrameRenderer.h/.cpp` | Dispatch GI, update GI UBO controls, and add barriers |
| `RT2App/shaders/secondary_raygen.rgen` | Consume stored diffuse GI payload with lobe-probability compensation |
| `RT2App/shaders/scatter_shared.glsl` | Expose diffuse-only BRDF and lobe-probability helpers |
| `RT2App/src/WalnutApp.cpp` | Add GI controls and debug views |
| `RT2Tests/src/GpuSceneDataTests.cpp` | Add layout/size assertions |
| Shader build configuration | Compile the new compute shader/includes |

Do not add bindings above the variable-count texture binding. Do not add RT
push constants solely for GI enable/parity.

## Render Settings

```c
bool     restirGIEnabled          = false;
bool     restirGITemporalEnabled  = true;
uint32_t restirGIFreshCandidates  = 1;
uint32_t restirGITemporalMCap     = 20;
uint32_t restirGIMaxTemporalAge   = 4;
float    restirGIDepthThreshold   = 0.10f;
float    restirGINormalThreshold  = 0.90f;
float    restirGIWorldPosThreshold = 0.10f;
```

Do not expose a scatter-direction similarity threshold for temporal reuse.

Useful debug views:

- GI selected direction;
- GI `Lo`;
- GI `hitT`;
- reservoir `M` and age;
- fresh versus history selection;
- receiver-validation rejection reason;
- invalid/fallback counter.

## Implementation Phases

### Phase 0: Infrastructure

Goal: allocate legal resources and create an idle pipeline with no visual
change.

1. Add the new structs and static assertions.
2. Add monolithic `SI_BINDING_GI_DATA = 11`.
3. Add the binding to the set-0 schema in `PathTracePass.cpp`.
4. Add compute visibility to the TLAS descriptor.
5. Implement aligned GI buffer regions and ping-pong indexing.
6. Add `ReSTIRGIPass` with its own push constants.
7. Name and populate the spare NRD UBO fields for GI enable/parity.
8. Add centralized GI history invalidation and clears.
9. Compile shaders, build and run the complete current test suite.
10. Confirm no validation-layer warnings and no visual change while disabled.

### Phase 1: Fresh-Only Correctness

Goal: establish a correct one-sample estimator before temporal reuse.

1. Implement ray-query scene traversal and committed-hit reconstruction.
2. Match alpha MASK/BLEND behavior for radiance and shadow queries.
3. Implement environment miss, emissive hit and secondary NEE evaluation.
4. Implement the 48-byte reservoir and full-payload streaming.
5. Generate one cosine-weighted fresh candidate.
6. Consume stored `Lo` and `hitT` in raygen without a final retrace.
7. Apply the cosine term and diffuse-lobe probability compensation.
8. Preserve the old path for GI-disabled, invalid and specular/transmission
   cases.
9. Compare long-run mean luminance against the existing path tracer before
   enabling temporal reuse.

Phase 1 is not complete until environment-only, emissive-only, textured,
normal-mapped, alpha-cutout and moving-light scenes behave correctly.

### Phase 2: Temporal Reuse

Goal: reuse the previous sample without changing the estimator's sample
identity.

1. Reproject and validate receiver history.
2. Replay the stored root seed/key and all derived substreams while
   re-evaluating history.
3. Merge with the current target and capped history `M`.
4. Store current `Lo`/`hitT` for whichever candidate wins.
5. Track age and rejection reasons.
6. Exercise every invalidation path.
7. Profile two radiance queries plus secondary shadow queries separately.

### Phase 3: NRD Integration and Optimization

Goal: integrate the stable estimator with denoising and remove avoidable work.

1. Verify diffuse/specular routing and hit-distance ownership.
2. Test NRD reset, disocclusion and responsiveness.
3. Add split-screen and debug visualizations.
4. Profile unused GI work on specular-selected pixels.
5. If needed, add a shared deterministic lobe decision/mask.
6. Evaluate half-resolution GI and packed-reservoir prototypes only after the
   full-resolution reference is correct.
7. Decide whether invalid reservoirs should trace the correctness fallback or
   return zero in performance mode.

### Phase 4: Spatial Reuse - Deferred Research Gate

Do not implement spatial reuse as "trace the neighbor's direction from the
center and merge." Published ReSTIR GI spatial reuse operates on path samples
and requires a defined shift/reconnection mapping, compatibility tests and
the corresponding Jacobian/MIS treatment.

Before Phase 4 begins:

1. select and document a path-shift mapping;
2. derive its target and Jacobian for RT2's stored sample;
3. define visibility and reconnection failure handling;
4. add a biased-versus-unbiased mode if approximations are used;
5. budget at least one radiance query and potentially one NEE shadow query per
   accepted neighbor;
6. validate temporal-only quality first.

### Phase 5: Rough-Specular GI - Future

Rough-specular reuse requires a GGX/VNDF proposal, stricter receiver
compatibility, lobe-aware target evaluation and robust path shifting. Keep it
out of the initial diffuse implementation.

## Synchronization

| Producer | Consumer | Resource | Required dependency |
|---|---|---|---|
| Raster G-buffer | GI compute | Primary G-buffer images | Raster/image writes -> compute shader reads |
| GI history clear | GI compute | Monolithic GI buffer | Transfer writes -> compute reads/writes |
| Previous frame | GI compute | Previous reservoir/history regions | Queue/frame ordering plus compute read visibility |
| GI compute | RT shading | Current reservoir region | Compute shader writes -> ray-tracing shader reads |
| RT shading | NRD | Diffuse/specular NRD inputs | Existing RT writes -> compute reads |

The DI-spatial-to-GI ordering is part of frame scheduling, but GI does not
need a DI-reservoir memory dependency because it does not read DI output.

## Risk Assessment

| Risk | Impact | Mitigation |
|---|---|---|
| Hit reconstruction differs from RT shaders | Wrong materials, normals or UVs | Share reconstruction helpers and compare ray-query debug output against closest-hit output |
| Alpha traversal differs | Opaque foliage/glass or light leaks | Reproduce any-hit and shadow-any-hit behavior in candidate loops |
| Stochastic `Lo` or alpha changes sample identity | Bias, flicker and unstable weights | Store/replay one root seed with deterministic substreams, or an explicit sample key |
| Missing cosine or lobe compensation | Systematically incorrect energy | Unit-test target and compare long-run mean luminance |
| Temporal validation accepts disocclusion | Ghosting | Keep world position, depth, normal and material validation; expose rejection debug view |
| Memory pressure | Allocation failure or poor cache behavior | Start with measured 48-byte reference; test half resolution and packing later |
| Extra specular-pixel GI work | Higher cost than predicted | Profile, then add a shared lobe decision/mask |
| Ray-query shader complexity | Long Phase 1 | Factor shared hit/material/alpha helpers; keep a raygen-pipeline fallback |
| Temporal correlation with NRD | Slow response or trails | Test motion/disocclusion and reset both histories together |
| Spatial reuse bias | Incorrect future implementation | Require an explicit shift mapping and derivation before Phase 4 |

## Validation and Tests

### Structural

- C++/GLSL size assertions for `SIGIReservoir` and `SIGIPushConstants`.
- Region offsets aligned and non-overlapping at odd resolutions.
- Binding 18 remains the largest set-0 binding.
- TLAS is visible to compute.
- All shader variants compile.
- Vulkan validation layers report no descriptor, barrier or ray-query errors.

### Estimator Correctness

- Fresh-only long-run mean matches the existing path tracer within a stated
  statistical tolerance.
- White Lambertian Cornell box conserves expected energy.
- Environment-only lighting works for hits and misses.
- Emissive secondary surfaces contribute correctly.
- No NaN, Inf, negative `M`, invalid `targetPdf` or unbounded `W`.
- Diffuse-lobe probability changes do not change long-run mean.
- GI-enabled specular/transmission samples match the existing path.

### Geometry and Materials

- Textured and normal-mapped secondary hits match closest-hit shading.
- Double-sided/front-face behavior matches.
- MASK foliage has correct holes.
- BLEND geometry follows the same stochastic policy as RT rays.
- Instance material offsets and transformed instances resolve correctly.

### Temporal

- Static-scene variance is lower than fresh-only when measured over repeated
  runs or image regions.
- Camera motion does not create persistent ghosting or bright flashes.
- Moving lights and rigid objects update through re-evaluation.
- Scene, environment, material and resolution changes clear history.
- History rejection reasons agree with debug expectations.

### NRD

- Diffuse GI and its own `hitT` stay paired.
- Specular/transmission remains in the specular channel.
- NRD reset coincides with GI invalidation where required.
- NRD on/off comparisons show no mean-energy drift.
- Anti-firefly settings do not hide reservoir explosions in the raw view.

### Performance

Record GPU timings and, where available, counters for:

- fresh radiance queries;
- temporal re-evaluation radiance queries;
- secondary NEE shadow queries;
- invalid-reservoir fallback rays;
- GI compute work discarded by specular lobe selection;
- GI buffer bandwidth;
- NRD cost before and after GI.

## What Must Remain Unchanged Initially

- ReSTIR DI sampling and reservoir mathematics.
- The existing specular/transmission recursive path.
- RT-primary mode.
- NRD channel layout and compose/remodulation structure.
- The separate tonemap pass.
- The variable-count texture array at binding 18.
- The current G-buffer layout, unless Phase 3 profiling justifies a lobe mask.
