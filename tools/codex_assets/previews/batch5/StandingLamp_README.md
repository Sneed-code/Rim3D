# StandingLamp

Powered frontier floor lamp for Rim3D. The built-in base, fixed-pose hinged arm, spun enamel shade, and attached cord fit one 1 m × 1 m cell.

## Asset

| Property | Value |
| --- | --- |
| Overall size in Blender X × Y × Z | 0.6730 × 0.7722 × 1.5740 m, including the cord and plug |
| Triangles | 3,988 total (3,672 Body, 316 Bulb) |
| UVs | One non-overlapping UV0 set |
| Materials | `StandingLamp_Painted_Metal` and `StandingLamp_Warm_Bulb` |
| Blender orientation | Metres, +Z up, −Y front, floor at Z = 0 |
| GLB orientation | Default Blender glTF conversion, +Y up and +Z front |

| Part | Pivot in Blender coordinates (m) | Use |
| --- | --- | --- |
| `Body` | (0, 0, 0), footprint centre at floor level | Cast base, mast, switch, fixed hinge and shade, cord and plug |
| `Bulb` | (-0.0545, -0.1630, 1.4891), on the socket centreline at the bulb seat | Swappable warm emissive bulb |

## Textures

| File | Channels |
| --- | --- |
| `StandingLamp_BaseColor.png` | sRGB base colour with painted wear and exposed material |
| `StandingLamp_Normal.png` | Linear, tangent-space OpenGL (+Y) normal |
| `StandingLamp_ORM.png` | Linear, R ambient occlusion, G roughness, B metallic |

All three maps are original 2048 × 2048 PNGs embedded in `StandingLamp.glb`. The bulb uses a separate warm emissive material with `KHR_materials_emissive_strength`.

## Build and review

Run `python build_standing_lamp.py` from this directory with NumPy, Pillow, and Blender 5.x. Set `STANDING_LAMP_BLENDER` to override the Blender executable. The script models, textures, exports, re-imports, validates, and renders the asset in the background.

The first render review exposed a tucked plug, suspended cable grommet, stretched cast-base wear, a visible shade seam, and a mostly hidden bulb. The final pass rerouted the cord and seated its grommet, projected the cast deck UVs in plan view, blended the tapered shade seams, concentrated enamel wear at the rim, refined the hinge silhouette, and lowered the close-up camera to show the emitter.

The five 1600 × 1200 PNGs were rendered in Cycles from a fresh GLB import with an RTX 5080. The scale view uses a 1.80 m neutral grey reference figure. `contact_sheet.jpg` presents those views in two columns and three rows.

All brief deliverables are complete. Verification covered the GLB, embedded maps, geometry, UV0 and Blender round trip. Unreal Engine import was outside this build's verification.
