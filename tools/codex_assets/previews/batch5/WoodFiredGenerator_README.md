# WoodFiredGenerator

Game-ready, photorealistic 3D asset for **Rim3D** (Unreal Engine 5.8, tropical frontier world, year 6650).
An original rugged frontier/industrial wood-fired steam power generator featuring a riveted cast-iron firebox with hollow ember chamber, hinged grille-and-quartz fire door, horizontal saddle-mounted steam boiler with brass pressure gauge and red shutoff handwheel, compact horizontal steam engine with visible reciprocating piston crosshead, 0.88 m spoked cast-iron flywheel driving a flat belt at an exact 3:1 ratio to an open-frame copper-wound dynamo with coiled armored power output cable, heat-blued/sooty exhaust chimney with spark-arrestor cap, and a steel log rack stacked with split firewood.

---

## Final Asset Specifications

| Property | Value |
|---|---|
| **GLB File** | `WoodFiredGenerator.glb` (12.74 MB, glTF 2.0 binary, default `+Y` up export, textures embedded) |
| **Final Size (X × Y × Z)** | **1.815 m × 1.811 m × 1.326 m** (2 × 2 cell footprint, ~1.3 m tall) |
| **Bounding Box (Blender +Z up)** | Min `(-0.906, -0.905, 0.000)` m → Max `(+0.909, +0.905, +1.326)` m |
| **Total Triangle Count** | **14,972 tris** (budget: 15,000 tris; 0 n-gons; consistent outward normals; scale `(1, 1, 1)`) |
| **UVs** | Clean, non-overlapping `UV0` across all meshes (dedicated high-res disc for pressure gauge dial) |
| **Materials** | 3 total (`Mat_WoodFiredGenerator`, `Mat_Embers`, `Mat_FireGlass`) |

---

## Axis Convention

- **Blender authoring space**: Right-handed, **+Z up**, metres, footprint centred on `(0, 0)`, bottom grounded strictly at `Z = 0.000 m`, front (firebox door / colonist work side) facing **-Y**.
- **glTF 2.0 export (`.glb`)**: Exported with Blender's glTF 2.0 exporter default `+Y` up conversion (`export_yup=True`), real-world metres, embedded textures.

---

## Separate Mesh Objects & Pivots

All coordinates in Blender authoring space (`+Z` up, metres). Rest transforms have scale `(1.0, 1.0, 1.0)` and rotation `(0, 0, 0)`.

| Object Name | Triangles | Pivot / Origin `(X, Y, Z)` m | Role & Motion Axis |
|---|---:|---|---|
| `Body` | 10,436 | `( 0.000,  0.000, 0.000)` | Static chassis: I-beam skid frame, deck plate, hollow riveted firebox & grate, boiler drum, pressure gauge, steam dome, valves, pipes, chimney, engine bed & cylinder, bearing pedestals, safety guard, drive belt, dynamo stator & junction box, coiled power cable, log rack & 10 split logs. **Never animated.** |
| `Flywheel` | 1,730 | `(+0.620, +0.220, +0.560)` | 0.88 m spoked cast-iron flywheel, main axle, eccentric cam, and coaxial belt pulley (`R = 0.210 m`). Pivot on axle centre; rotates around local **X**. |
| `DynamoRotor` | 1,068 | `(+0.220, +0.680, +0.420)` | Rotor shaft, 12-coil copper-wound armature, commutator, cooling fan, and flanged belt pulley (`R = 0.070 m`, exact 3:1 ratio). Pivot on axle centre; rotates around local **X**. |
| `Door` | 758 | `(-0.470, -0.700, +0.390)` | Cast-iron firebox door with cutout grille, heat-stained quartz glass pane (`Mat_FireGlass`), hinge straps, and greased wood/brass latch handle. Pivot on hinge pin axis; rotates around local **Z**. |
| `Piston` | 496 | `(+0.340, +0.020, +0.440)` | Machined steel piston rod, brass-shoed crosshead block, wrist pin, and yoke bar sliding inside open engine guide rails. Pivot at rest centre; translates along local **Y**. |
| `Embers` | 484 | `(-0.260, -0.470, +0.350)` | Sculpted glowing 3D firebed of charred burning logs and incandescent coals inside the firebox cavity (`Mat_Embers`). Pivot at firebed centre; scales unevenly on **X/Y/Z**. |

---

## Textures (2048 × 2048 PBR, Procedurally Generated)

All maps are authored procedurally with Python (NumPy/Pillow) guided by Cycles-baked linear 3D geometry maps (AO, world position, world normal) — **no baked lighting, shadows, or AO in BaseColor**.

### Main Material (`Mat_WoodFiredGenerator`)
- `WoodFiredGenerator_BaseColor.png` — 2048 × 2048 sRGB albedo (pure reflectance: cast iron, weathered olive enamel, yellow/black hazard stripes, tarnished brass, wound copper, red valve paint, split bark/heartwood, pressure gauge dial, soot, oil stains, light rust)
- `WoodFiredGenerator_Normal.png` — 2048 × 2048 OpenGL (`+Y` up) tangent-space normal map (sand-cast iron grain, diamond treadplate, parallel copper wire coils, bark ridges, split wood grain, woven leather belt, chipped paint edges)
- `WoodFiredGenerator_ORM.png` — 2048 × 2048 Linear packed (`R` = Ambient Occlusion, `G` = Roughness, `B` = Metallic)

### Embers Material (`Mat_Embers`)
- `WoodFiredGenerator_Embers_BaseColor.png` — 2048 × 2048 sRGB matte charred black wood & wood ash
- `WoodFiredGenerator_Embers_Emissive.png` — 2048 × 2048 sRGB fiery crimson/orange/gold incandescent coal fissures (`Emission Strength = 3.8`)
- `WoodFiredGenerator_Embers_Normal.png` — 2048 × 2048 OpenGL (`+Y` up) cracked alligator-char relief
- `WoodFiredGenerator_Embers_ORM.png` — 2048 × 2048 Linear packed (`R` = AO, `G` = Roughness, `B` = Metallic `0.0`)

### Firebox Window Glass (`Mat_FireGlass`)
- Translucent smoky heat-tinted quartz glass (`Alpha = 0.16`, `Roughness = 0.08`, `IOR = 1.52`, `BLEND` mode), assigned exclusively to the 6 recessed window pane polygons of `Door`.

---

## Animations (glTF 2.0 NLA Tracks, 24 fps)

All three animations are embedded in `WoodFiredGenerator.glb` as separate named glTF animations at **24 fps** using linear interpolation for seamless looping and exact quaternion steps. Only the specified moving parts are animated (`Body` has zero animation tracks).

| Action Name | Frame Range | Duration | Frame Rate | Interpolation | Motion Description |
|---|---|---|---|---|---|
| `Running` | `0 – 48` | `2.000 s` (seamless loop) | 24 fps | Linear | `Flywheel`: 2 full turns (`720°`, local X); `DynamoRotor`: 6 full turns (`2160°`, local X, 3:1 belt ratio); `Piston`: 4 full back-and-forth sinusoidal strokes (`±0.072 m`, local Y) in step with flywheel; `Embers`: uneven periodic flicker (`scale 0.95 – 1.05` on X/Y/Z). |
| `DoorOpen` | `0 – 24` | `1.000 s` (`0.60 s` swing + `0.40 s` hold) | 24 fps | Linear-sampled smooth ease | `Door` swings outward `-100°` around local Z (frames `0–14`, `0.6 s`) and holds open at `-100°` (frames `15–24`). |
| `DoorClose` | `0 – 24` | `1.000 s` (`0.60 s` swing + `0.40 s` hold) | 24 fps | Linear-sampled smooth ease | `Door` swings smoothly closed from `-100°` back to `0°` (frames `0–14`, `0.6 s`) and holds latched at `0°` (frames `15–24`). |

---

## Previews & Build Scripts Included

### Previews (rendered from the re-imported `WoodFiredGenerator.glb` in a fresh Blender 5.2 Cycles scene)
- `preview_front.png` (1600 × 1200) — Front (`-Y`) view
- `preview_side.png` (1600 × 1200) — Side (`+X`) view showing spoked flywheel, belt, dynamo, and cable
- `preview_34.png` (1600 × 1200) — Hero three-quarter view from above-front
- `preview_close.png` (1600 × 1200) — Detail close-up (pressure gauge, valve, firebox grille & embers, rivets, pipes)
- `preview_scale.png` (1600 × 1200) — Scale verification alongside a 1.80 m plain grey human silhouette
- `contact_sheet.jpg` (2400 × 1200, JPEG quality 85) — 2 × 3 grid of all 5 previews + spec card
- `preview_anim.gif` (800 × 600, 48 frames looping) — Three-quarter animation preview showing `Running` + `DoorOpen` + `DoorClose`

### Reproducible Build Pipeline
1. `blender -b --factory-startup -P build_generator.py` — Generates all 6 meshes, packs non-overlapping UV0, checks tri budget & topology, and bakes linear Cycles utility maps.
2. `python generate_textures.py` — Synthesizes all 2048 × 2048 PBR + Emissive maps.
3. `blender -b --factory-startup -P finalize_and_export.py` — Assigns PBR materials, builds NLA animations, exports `WoodFiredGenerator.glb`, and inspects the binary GLB.
4. `blender -b --factory-startup -P render_previews.py` — Re-imports `WoodFiredGenerator.glb` into a fresh scene and renders all 5 studio stills + 48 animation frames.
5. `python make_deliverables.py` — Assembles `contact_sheet.jpg` and `preview_anim.gif`.

---

## Status / Notes
- Everything specified in `BRIEF.md` is complete and verified after multiple self-critique visual passes (fixing linear bake encoding, normal-gradient edge wear, smoky quartz door glass assignment, micro-normal refinement, smooth bent steam/exhaust/cable tubing, and balanced ember emissive contrast). Nothing unfinished.
