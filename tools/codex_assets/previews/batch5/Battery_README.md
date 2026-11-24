# Battery

Battery is a weathered frontier power-storage cabinet for Rim3D. Its working end has a five-segment charge display and two service panels. The side louvres cover three numbered cell tiers. Recessed copper terminals sit in the top tray, and a corrugated conduit joins glands on the rear bulkhead.

## Model

| Property | Value |
| --- | --- |
| File | `Battery.glb`, glTF 2.0 binary with embedded textures |
| Blender bounds (X × Y × Z) | **0.940 × 1.940 × 0.805 m** |
| Placement | 1 × 2 cells, long side along Blender Y |
| Triangles | **6,490** of 8,000 |
| Materials | `Battery_Cabinet` and `Battery_LampEmission` share one 2048 × 2048 atlas |
| UVs | Non-overlapping UV0 across both parts |

The Blender scene uses metres, +Z up and −Y toward the working end. The footprint is centred at X = Y = 0 and the skid shoes sit at Z = 0. Blender’s default glTF export writes +Y up and +Z toward the working end, using the conversion `(x, y, z) → (x, z, −y)`. Both parts have unit scale.

| Mesh | Triangles | Pivot in Blender metres (X, Y, Z) | Function |
| --- | ---: | --- | --- |
| `Body` | 5,960 | `(0, 0, 0)` | Fixed cabinet, service panels, vent banks, terminals and conduit |
| `ChargeLamps` | 530 | `(0.023000, −0.945430, 0.606174)` | Centre of the five-lens bank, aligned with the middle lens |

The preview state shows four bright segments and a dim fifth.

## Textures

| File | Interpretation |
| --- | --- |
| `Battery_BaseColor.png` | sRGB surface colour with procedural paint, chips, dust, oxidation and stencils. Lighting and AO are excluded. |
| `Battery_Normal.png` | Linear tangent-space OpenGL (+Y) normal. Includes fastener sockets, optical ribs, grip relief and fine surface wear. |
| `Battery_ORM.png` | Linear packed channels. R = ambient occlusion, G = roughness, B = metallic. |

## Preview and review

`preview_front.png`, `preview_side.png`, `preview_34.png`, `preview_close.png` and `preview_scale.png` are 1600 × 1200 studio renders from the final GLB, re-imported into a fresh Blender scene and rendered in Cycles on the RTX 5080. The scale render places a plain 1.800 m human figure beside the cabinet. `contact_sheet.jpg` collects the five views in a 2 × 3 grid at 2200 px wide, saved at JPEG quality 85.

The first render showed washed-out upper hazard stripes, overly regular edge damage, a clipped front view and a scale figure overlapping the cabinet. The revised asset uses darker stripe pigment, localized tool scratches and terminal oxidation, and clearer cartridge latches. The revised cameras frame the cabinet, show the rear conduit, and leave space between the cabinet and figure. The final geometry pass also removed buried mounting faces and corrected collapsed bevels on thin panels.

## Rebuild and verification

Run `python build_battery.py` from this folder with Python 3.13, NumPy, Pillow, SciPy and Blender 5.x. The script defaults to the installed Blender 5.3 Beta and accepts `--blender PATH`. It rebuilds the GLB and textures, renders all five views, makes the contact sheet, and validates the export. Use `--draft` to reproduce the review version in `.battery-work/draft`.

The delivered GLB and image set passed the export checks. Unreal Editor import and gameplay integration were outside this delivery.
