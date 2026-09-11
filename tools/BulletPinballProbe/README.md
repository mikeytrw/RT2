# RT2 core-only Bullet pinball spike

This standalone probe answers one narrow question: can Bullet's mature rigid-body
core supply the mechanics needed by RT2's first pinball table without bringing
the rest of Bullet into the engine?

It pins Bullet 3.25 at commit
`2c204c49e56ed15ec5fcfa71d199ab6d6570b3f5` and configures only:

- `LinearMath`
- `BulletCollision`
- `BulletDynamics`

The probe covers a fast CCD sphere, a motorized hinge flipper, a slider/motor
plunger, a static triangle-mesh ramp, and a non-blocking ghost trigger.

## Build and run

```powershell
$build = Join-Path $env:TEMP "rt2-bullet-pinball-spike"
cmake -S tools/BulletPinballProbe -B $build -A x64
cmake --build $build --config Release --target RT2BulletPinballProbe -- /m
& "$build/Release/RT2BulletPinballProbe.exe"
```

The executable returns non-zero if any proof fails and prints measurements for
each mechanism. It is deliberately separate from the RT2 solution: this is a
dependency/mechanics spike, not the engine integration.

## Asset preparation companion

`tools/PinballAssetProbe/split_obj_components.py` finds disconnected geometry
islands in the corrected pinball OBJ and emits stable component names plus a
JSON manifest. RT2's OBJ import currently merges those shapes, so
`repack_glb_pbr.py` instead combines a node-preserving GLB conversion with the
materials and embedded textures from the corrected full-PBR GLB.

Example:

```powershell
python tools/PinballAssetProbe/repack_glb_pbr.py `
  --geometry pinball-001-RT2-components.glb `
  --material-donor pinball-001-RT2.glb `
  --output pinball-001-RT2-components-pbr.glb
```

Emissive overrides require explicit node regexes. The tool does not guess which
components are lamps, flippers, bumpers, or the plunger; those roles must be
visually identified before the gameplay asset is finalized.
