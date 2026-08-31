# W2 typed extent call-site ledger

Grounded against commit `a6ba62e` on 31 August 2026. This is the checked
closure ledger for the typed render/output boundary. `RenderExtent` is used
for internal raster, ray, ReSTIR, G-buffer and NRD work; `OutputExtent` is used
for camera aspect, display, viewport and readback.

| Boundary | Source citation | Type / evidence |
| --- | --- | --- |
| Value types and native translation | `RT2App/src/RenderExtents.h:8-77` | Checked nonzero constructors; no implicit conversion; explicit native plan |
| Camera aspect/projection | `RT2App/src/Camera.h:18-36`, `RT2App/src/Camera.cpp:115-123` | `OutputExtent` |
| Viewport normalization and picking map | `RT2App/src/ViewportCoordinates.h:9-36`, `RT2App/src/ViewportCoordinates.cpp:8-47` | Output pixel normalization and explicit Render pixel mapping |
| Renderer resize and ownership | `RT2App/src/RendererGPU.h:45-94`, `RT2App/src/RendererGPU.cpp:261-315` | Output input; native Render plan; output image/display vs internal resources |
| Camera UBO dispatch dimensions | `RT2App/src/RendererGPU.cpp:838` | Render extent |
| Raster / ray / ReSTIR dispatch | `RT2App/src/FrameRenderer.h:55-60`, `RT2App/src/FrameRenderer.cpp:167,235,273,362,402,429,438,470` | Render extent |
| G-buffer allocation | `RT2App/src/GBufferTarget.h:88-119`, `RT2App/src/GBufferTarget.cpp:10-16` | Render extent |
| ReSTIR reservoir allocation | `RT2App/src/ReservoirResources.h:35-47`, `RT2App/src/ReservoirGIResources.h:42-61` | Render extent |
| NRD resources/settings | `RT2App/src/NRDIntegration.h:20-50`, `RT2App/src/NRDIntegration.cpp:37-40,152-163` | Render extent |
| Compose and debug | `RT2App/src/ComposePass.h:22-36`, `RT2App/src/GBufferDebugPass.h:21-24` | Render extent (native policy) |
| Tone map / display | `RT2App/src/TonemapPass.h:19-21`, `RT2App/src/FrameRenderer.cpp:637` | Output extent |
| Headless readback | `RT2App/src/RendererGPU.cpp:1098-1270` | Output extent for allocation, copy and reported dimensions |
| ImGui display and host resize | `RT2App/src/WalnutApp.cpp:737,1100-1114` | Output extent |
| CLI meaning | `RT2App/src/CLIArgs.h:17-18,95-99` | `--width/--height` remain output dimensions |

## Search closure

`RendererGPU::GetWidth` / `RendererGPU::GetHeight` and all
`m_RendererGPU.GetWidth()` / `m_RendererGPU.GetHeight()` call sites are absent.
The remaining `GetWidth`/`GetHeight` methods belong to asset/resource classes
whose dimensions are not renderer output accessors. The repository search was:

```text
git grep -n -E "RendererGPU::GetWidth|m_RendererGPU\.GetWidth|m_RendererGPU\.GetHeight"
```

The native plan intentionally keeps `RenderExtent == OutputExtent` so existing
native and NRD rendering semantics remain unchanged. Resolution scaling and
non-native planning are out of scope for W2.
