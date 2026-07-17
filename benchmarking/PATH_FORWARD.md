# Real-Time Path Tracing: Research Summary & Path Forward for RT2

## Executive Summary

After surveying the techniques used by commercial path-traced games (Cyberpunk 2077,
Alan Wake 2, Portal with RTX, Indiana Jones) and NVIDIA's RTX Path Tracing SDK (RTXPT),
the path to high-resolution, high-FPS path tracing is clear. The industry has converged
on a **single architecture**: pure path tracing (1 path/pixel) + ReSTIR sampling +
NRD/DLSS-RR denoising + temporal upscaling. RT2 already has the foundational pieces;
the gap is in sampling quality, denoiser sophistication, and performance optimization.

---

## 1. Current State of the Art (What Commercial Engines Do)

### NVIDIA RTX Path Tracing (RTXPT) — The Reference Architecture
- **Pure path tracer** — no rasterization for primary visibility; all light transport
  in a single ray tracing pass
- **RTXDI** for ReSTIR DI (direct lighting) + ReSTIR GI (indirect lighting) + ReSTIR PT
  (full path resampling)
- **NRD** (ReLAX + ReBLUR) with up to **3-layer path space decomposition** (diffuse,
  specular, and a third layer for rough/specular-diffuse)
- **DLSS-RR** (Ray Reconstruction) — neural network replaces traditional denoiser,
  trained on path-traced inputs. Produces cleaner images than NRD at 1 path/pixel
- **NEE-AT** — feedback-based temporally adaptive guided importance sampling for
  emissive triangles and environment maps
- **Shader Execution Reordering (SER)** — 20-50% perf gain by reordering threads
  for coherent material evaluation after ray tracing
- **Opacity Micromaps (OMM)** — fast ray-traced alpha testing for foliage/grates
- **RayCones** for texture MIP selection (avoid texture aliasing without manual LOD)
- **RTXTF** — Stochastic Texture Filtering for anisotropic filtering in ray tracing
- **Streamline** integration: DLSS SR (super resolution), DLSS FG (frame generation),
  DLSS AA, DLSS MFG (multi-frame generation)

### Cyberpunk 2077 RT Overdrive
- Uses RTXPT as the base with ReSTIR DI + ReSTIR GI for importance sampling
- NRD for denoising (later replaced by DLSS-RR in the 3.5 update)
- DLSS Super Resolution for upsampling (renders at ~1080p, upscales to 4K)
- Frame Generation for doubling FPS
- The SIGGRAPH 2023 course ("A Gentle Introduction to ReSTIR") includes a dedicated
  talk by CD Projekt Red on their ReSTIR integration (see cyberpunk PDF)

### Key Performance Numbers (from NRD README, RTX 4080 @ 1440p native)
- REBLUR_DIFFUSE_SPECULAR: 2.55 ms (3.40 ms in SH mode)
- RELAX_DIFFUSE_SPECULAR: 3.25 ms (4.80 ms in SH mode)
- SIGMA_SHADOW: 0.40 ms
- Total denoising budget: ~3-5 ms per frame

---

## 2. The Technology Stack (Ordered by Impact)

### Tier 1: Sampling (Biggest Quality Impact)
| Technique | What It Does | RT2 Status |
|-----------|-------------|------------|
| **ReSTIR DI** | Importance-samples direct lighting from many lights | **Done** (temporal + spatial) |
| **ReSTIR GI** | Resamples indirect diffuse paths across pixels/frames | Not implemented |
| **ReSTIR PT** | Resamples full paths (multi-bounce, specular + diffuse) | Not implemented |
| **GRIS theory** | Generalized RIS for unbiased cross-domain reuse | Not needed (theoretical foundation) |
| **NEE-AT** | Temporally-adaptive light importance via feedback | Basic NEE only |

**Key insight**: RT2's ReSTIR DI is working but only handles direct lighting. The
next step is **ReSTIR GI** for indirect diffuse, then **ReSTIR PT** for full path
reuse. The "ReSTIR PT Enhanced" paper (2026, best paper at I3D) provides the most
practical algorithmic improvements for production use.

### Tier 2: Denoising (Critical for 1 spp)
| Technique | What It Does | RT2 Status |
|-----------|-------------|------------|
| **NRD REBLUR** | Recurrent blur denoiser for diffuse + specular | **Done** (integrated) |
| **NRD SH mode** | Spherical harmonic/gaussian mode for directional signal | Not used |
| **DLSS-RR** | Neural denoiser replacing NRD | Not available (proprietary) |
| **Path-space decomposition** | Separate diffuse/specular/3rd-layer signals | 2-layer (diffuse + specular) |
| **Material demodulation** | Decouple BRDF from radiance before denoising | **Done** |
| **History confidence** | Per-pixel confidence for anti-lag | Not used |
| **Blue noise** | Low-discrepancy sampling for better spatial filtering | White noise only |

**Key insight**: RT2's NRD integration is solid but only uses 2-layer decomposition.
Adding a 3rd layer (rough specular / intermediate) and enabling NRD's SH mode would
significantly improve quality. Blue noise sampling (Heitz's Owen-Scrambled Sobol)
is a cheap win that reduces residual boiling.

### Tier 3: Performance (Critical for High FPS)
| Technique | What It Does | RT2 Status |
|-----------|-------------|------------|
| **SER** | Thread reordering for coherent shading | Not implemented (needs NVAPI) |
| **RayCones** | Automatic texture LOD for ray tracing | Not implemented |
| **OMM** | Opacity micromaps for alpha-tested geometry | Not implemented |
| **TLAS compaction** | Reduce TLAS rebuild cost | Basic rebuild only |
| **GPU timestamp profiling** | Identify GPU bottlenecks | **Done** (GpuTimestampProfiler) |
| **Render at lower res** | Render RT at 1080p, upscale to 4K | Not implemented (no upscaler) |

**Key insight**: SER is the single biggest performance win available (20-50% on
RTX 40 series). It requires NVAPI integration but is relatively straightforward —
replace `traceRayEXT` with `NvTraceRayHitObject` + `NvReorderThread` + `NvInvokeHitObject`.
RayCones eliminates texture aliasing artifacts and is a self-contained addition.

### Tier 4: Upscaling (For High Resolution)
| Technique | What It Does | RT2 Status |
|-----------|-------------|------------|
| **DLSS SR** | AI super resolution (1080p -> 4K) | Not integrated |
| **FSR 2/3** | Open temporal upscaler | Not integrated |
| **TAA** | Basic temporal antialiasing | Not implemented |
| **Frame Generation** | DLSS FG / MFG for FPS doubling | Not integrated |

**Key insight**: The most practical path for RT2 is integrating **FSR 2** (AMD's
open-source temporal upscaler) since DLSS requires NVIDIA's proprietary SDK and
Streamline integration. FSR 2 is MIT-licensed, works on all GPUs, and provides
quality close to DLSS when fed good motion vectors and depth — which RT2 already
produces for NRD.

---

## 3. Recommended Path Forward for RT2

### Phase 1: Improve Sampling Quality (Highest ROI)
**Goal**: Reduce noise at 1 spp so the denoiser has less work to do.

1. **Upgrade ReSTIR DI to ReSTIR GI** for indirect diffuse illumination
   - Reuse the 1-bounce indirect diffuse path across pixels and frames
   - Uses the same reservoir infrastructure already in place
   - Reference: ReSTIR GI paper (Ouyang et al. 2021), RTXDI v2.0 docs

2. **Add NEE-AT (Adaptive Temporal NEE)**
   - Build a temporal feedback texture that records which lights were important
   - Use it to guide initial candidate generation in the ReSTIR temporal pass
   - This is what RTXPT uses for its "feedback-based guided importance sampling"

3. **Switch to blue noise sampling**
   - Replace white noise RNG with Heitz's Owen-Scrambled Sobol (no memory cost,
     unlike STBN)
   - Add a global temporal shift (Weyl sequence) for per-frame rotation
   - Reduces residual boiling that NRD struggles with

### Phase 2: Performance Optimization
**Goal**: Get frame time under 16ms at 1080p for complex scenes.

4. **Integrate Shader Execution Reordering (SER)**
   - Add NVAPI to the build (premake dependency)
   - Replace `traceRayEXT` in closesthit/raygen with SER pattern
   - Expected: 20-50% improvement in RT shading time
   - Reference: SER whitepaper (in research/), NVIDIA blog post

5. **Add RayCones for texture LOD**
   - Track ray differentials (cone spread angle) along path
   - Select texture mip level based on cone footprint at hit
   - Eliminates texture aliasing without manual LOD hacks
   - Reference: Ray Tracing Gems Chapter 20

6. **Optimize TLAS rebuilding**
   - Only rebuild TLAS when instances actually move (dirty flag per instance)
   - Use refit instead of rebuild when only transforms change
   - Consider 2-level TLAS for static vs dynamic geometry

### Phase 3: Denoiser Improvements
**Goal**: Cleaner images with less temporal lag and fewer artifacts.

7. **Enable 3-layer path space decomposition**
   - Add a "rough specular" layer between diffuse and sharp specular
   - Route paths by roughness threshold: <0.3 = specular, 0.3-0.7 = rough spec, >0.7 = diffuse
   - Each layer gets its own NRD denoiser instance

8. **Enable NRD SH (Spherical Harmonic/Gaussian) mode**
   - Stores directional information alongside radiance
   - Improves specular tracking on rough surfaces
   - Requires passing first-bounce ray direction to NRD

9. **Add history confidence inputs**
   - Per-pixel confidence [0,1] based on sample variance or ReSTIR M count
   - Helps NRD's anti-lag respond faster to disocclusions
   - Cheap to compute, significant quality improvement in motion

### Phase 4: Upscaling for High Resolution
**Goal**: Render at 1080p, display at 4K.

10. **Integrate FSR 2 (or FSR 3)**
    - MIT-licensed, works on all GPUs
    - Needs: current color, depth, motion vectors, previous color — all available
    - Renders RT at 1080p, upscales to native resolution
    - ~2-3x effective performance improvement at 4K

11. **Add TAA fallback**
    - Simple temporal antialiasing for when upscaling is disabled
    - Uses the same jitter + motion vector infrastructure already in place

### Phase 5: Advanced (Future)
**Goal**: Match commercial quality.

12. **ReSTIR PT** — Full path resampling (replaces ReSTIR DI + GI with unified
    path-level reuse). Reference: GRIS paper, ReSTIR PT Enhanced paper.
13. **DLSS-RR** — If NVIDIA SDK access becomes available, replace NRD with
    neural denoising for significantly cleaner 1 spp output.
14. **Opacity Micromaps** — For scenes with alpha-tested foliage/grates.
15. **Stochastic Texture Filtering** — For anisotropic filtering in ray tracing.

---

## 4. Priority Order (What to Do First)

Based on implementation effort vs. quality/performance impact:

| Priority | Task | Effort | Impact |
|----------|------|--------|--------|
| 1 | Blue noise sampling | Low | Medium (reduces boiling) |
| 2 | SER integration | Medium | High (20-50% perf) |
| 3 | RayCones | Medium | Medium (quality) |
| 4 | ReSTIR GI | High | High (quality) |
| 5 | FSR 2 integration | Medium | High (enables 4K) |
| 6 | 3-layer decomposition | Medium | Medium (quality) |
| 7 | NRD SH mode | Medium | Medium (quality) |
| 8 | NEE-AT | Medium | Medium (quality) |
| 9 | History confidence | Low | Low-Medium (quality) |
| 10 | ReSTIR PT | Very High | High (quality) |

**Recommended starting point**: Blue noise (1 day) + SER (2-3 days) + FSR 2 (2-3 days).
These three give the biggest bang-for-buck and don't require algorithmic changes to
the path tracer itself.

---

## 5. Papers Downloaded (in research/)

| File | Paper | Year | Relevance |
|------|-------|------|-----------|
| restir_original_2020.pdf | Spatiotemporal Reservoir Resampling (ReSTIR DI) | 2020 | Foundation — what RT2 has now |
| restir_rearchitecting_2021.pdf | Rearchitecting Spatiotemporal Resampling for Production | 2021 | Production optimizations (7x speedup) |
| restir_pt_gris_2022.pdf | Generalized RIS: Foundations of ReSTIR (GRIS/ReSTIR PT) | 2022 | Theory for full path resampling |
| restir_conditional_ris_2023.pdf | Conditional RIS and ReSTIR | 2023 | Final gather approach, reduces blotchiness |
| restir_course_notes_2023.pdf | A Gentle Introduction to ReSTIR (SIGGRAPH course) | 2023 | Best starting point for learning |
| restir_cyberpunk_integration_2023.pdf | ReSTIR Integration in Cyberpunk 2077 | 2023 | Production integration details |
| restir_pt_enhanced_2026.pdf | ReSTIR PT Enhanced (Best Paper I3D) | 2026 | Latest practical ReSTIR PT improvements |
| restir_compatibility_guided_2026.pdf | Compatibility-Guided Neighbor Selection | 2026 | Better spatial neighbor picking |
| restir_multilayer_reservoir_2026.pdf | Multi-Layer Reservoir Splatting | 2026 | Handles disocclusions in temporal reuse |
| restir_gradient_domain_2026.pdf | Gradient-Domain ReSTIR PT | 2026 | Gradient-domain reconstruction |
| restir_lod_2026.pdf | Real-Time LoD Rendering with ReSTIR | 2026 | LoD for complex scenes |
| restir_stochastic_pairwise_mis_2026.pdf | Stochastic Pairwise MIS for Large-Kernel Reuse | 2026 | Unbiased large spatial reuse |
| svgf_2017.pdf | Spatiotemporal Variance-Guided Filtering (SVGF) | 2017 | Foundation for NRD's approach |
| ser_whitepaper.pdf | Shader Execution Reordering Whitepaper | 2022 | SER hardware feature |

## 6. Key Open-Source References

- **RTXPT**: https://github.com/NVIDIA-RTX/RTXPT — NVIDIA's reference path tracer (DX12 + Vulkan)
- **RTXDI**: https://github.com/NVIDIA-RTX/RTXDI — ReSTIR DI/GI/PT library (v3.0)
- **NRD**: https://github.com/NVIDIA-RTX/NRD — Denoiser library (REBLUR, RELAX, SIGMA)
- **ReSTIR PT reference code**: https://github.com/DQLin/ReSTIR_PT
- **SIGGRAPH 2023 ReSTIR course**: https://intro-to-restir.cwyman.org/
- **FSR 2**: https://github.com/GPUOpen-Tools/FidelityFX-FSR2
- **NRD Sample (simplex branch)**: https://github.com/NVIDIA-RTX/NRD-Sample (best practices)