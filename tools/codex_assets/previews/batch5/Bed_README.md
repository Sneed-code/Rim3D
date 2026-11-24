# Bed

Single frontier bed for Rim3D. The bed occupies **1 × 2 cells** and measures
**0.940 × 2.000 × 0.953 m** (Blender X × Y × Z). `Bed.glb` contains
**7,900 triangles** and one shared PBR material. Both meshes use the same
non-overlapping UV0 atlas and have unit scale.

## Parts and coordinates

| Node | Contents | Origin in Blender metres (X, Y, Z) |
| --- | --- | --- |
| `Frame` | Solid timber posts, rails, headboard and mattress slats | `(0, 0, 0)` |
| `Bedding` | Mattress with fitted sheet, turned woven blanket and pillow | `(0, 0, 0)`, shared with the frame for swapping |
| `Sleeper1` | Empty marking the sleeping position over the mattress centreline, toward the head | `(0, 0.405, 0.630)` |

Blender uses metres, **+Z up**, **+Y toward the headboard**, and **−Y toward the foot
and working side**. The footprint is centred at the ground origin. Blender’s
default glTF conversion gives the GLB **+Y up**, with **−Z toward the headboard**
and **+Z toward the foot**. The GLB sleeper marker is at `(0, 0.630, −0.405)`.
The timber has a neutral base tone for applying a stuff tint to the `Frame` slot.

## Textures

All three **2048 × 2048** PNGs are embedded in the GLB and supplied separately.
They were generated procedurally for this asset.

| File | Encoding and use |
| --- | --- |
| [Bed_BaseColor.png](Bed_BaseColor.png) | sRGB wood and textile colour, free of baked lighting, shadows and AO |
| [Bed_Normal.png](Bed_Normal.png) | Linear tangent-space OpenGL normal, +Y |
| [Bed_ORM.png](Bed_ORM.png) | Linear packed map. Red is ambient occlusion, green is roughness, blue is metallic. |

## Previews

The [contact sheet](contact_sheet.jpg) is a 2-row by 3-column JPEG at
2362 × 1284, saved at quality 85. Each view is a 1600 × 1200 Cycles render
of the GLB re-imported into a fresh Blender 5.3 Beta scene.

- [Front](preview_front.png)
- [Side](preview_side.png)
- [Three-quarter](preview_34.png)
- [Detail](preview_close.png)
- [Scale with the 1.8 m reference](preview_scale.png)

The first render pass exposed clipping between the blanket and mattress at the
foot, cropped posts in the front view, and overly even shading on the wood and
cloth. The second pass reshaped the hem and blanket return, refined the pillow
profile, added local wear and weave variation, and reframed the front. The final
geometry check found zero intersections between the mattress, pillow and blanket.

## Rebuild

Run the following in this folder with Python, NumPy, Pillow and Blender 5.x.

```powershell
python -B build_bed.py --pass final
```

[build_bed.py](build_bed.py) runs the export, checks the GLB, re-imports it for
rendering and assembles the contact sheet. [bed_geometry.py](bed_geometry.py)
builds the meshes and UV0 layout. [bed_textures.py](bed_textures.py) paints the
atlas. Use `--blender` with a custom executable path. `--pass draft` writes the
draft geometry and preview variant under `.bed-review/draft`.

All brief deliverables are complete. The GLB was verified in Blender. Unreal
project integration was outside this build.
