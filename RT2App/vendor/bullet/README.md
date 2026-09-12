# Bullet 3.25 vendored core — pin record

Pinned upstream revision: `2c204c49e56ed15ec5fcfa71d199ab6d6570b3f5`
(tag 3.25, https://github.com/bulletphysics/bullet3.git).

Vendored scope (deliberate subset of that revision):

- `src/LinearMath/` (full module tree)
- `src/BulletCollision/` (full module tree)
- `src/BulletDynamics/` (full module tree)
- `src/btBulletCollisionCommon.h`, `src/btBulletDynamicsCommon.h`
  (module root headers referenced by the upstream module CMakeLists)
- `src/*/CMakeLists.txt` (kept as provenance for the mirrored file lists)
- `LICENSE.txt` (upstream bytes, verbatim)

Not vendored: top-level demos, `Extras/` (including SoftBody),
`Bullet3/`, Python bindings, `examples/`, `test/`, `data/`, docs.

Scope honesty: the upstream `BulletDynamics` module itself compiles
Vehicle (`btRaycastVehicle`/`btWheelInfo`), Featherstone/MultiBody
(`btMultiBody*`), Character and MLCP solver sources. Those objects ship
inside the `BulletDynamics` static library as dead code; RT2 exposes and
uses only rigid bodies plus `btHingeConstraint`/`btSliderConstraint`.
"Out of scope" means not exposed, not used, not tested — not "not
compiled". Upstream `premake4.lua` scaffolding was removed; the build is
defined solely by `premake5.lua` in this directory (exactly three
`StaticLib` projects: `LinearMath`, `BulletCollision`, `BulletDynamics`).

Attribution: Bullet Continuous Collision Detection and Physics Library,
http://bulletphysics.org, zlib-licensed (see `LICENSE.txt`). No
GPL-family sources are pulled in: demos/Extras are never added, and the
vendored subset contains only the three modules above.
