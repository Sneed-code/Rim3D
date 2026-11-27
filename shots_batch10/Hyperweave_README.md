# Hyperweave

Hyperweave is a pristine, tightly folded bolt of spacer fabric for Rim3D. It has five continuous plies, a graphite clamp with a formed clasp, and a small cyan status light. The blue-violet hex weave and its subtle sheen come from procedural color, tangent-space normal, and roughness maps.

| Asset property | Final value |
| --- | --- |
| Runtime file | `Hyperweave.glb`, binary glTF 2.0 |
| Blender dimensions (X × Y × Z) | 0.300258 × 0.222400 × 0.082298 m |
| Triangles | 1,492 |
| Mesh / material | One mesh object, `Body`, with one PBR material |
| UVs | One non-overlapping UV0 with padded islands |
| `Body` pivot | Bottom center, `(0, 0, 0)` m, with identity transform |
| Axes | Blender is Z-up with X along the bolt and the studio front facing −Y. Default glTF export is Y-up, with `(x, y, z)` mapped to `(x, z, −y)`. Units are meters. |

## Textures

All four 1024 × 1024 PNG maps are supplied separately and embedded in the GLB. Their decoded pixels were checked against the embedded copies.

| File | Interpretation |
| --- | --- |
| `Hyperweave_BaseColor.png` | sRGB surface color, free of baked lighting, shadows, and AO |
| `Hyperweave_Normal.png` | Linear tangent-space OpenGL normal, +Y |
| `Hyperweave_ORM.png` | Linear packed map with R = occlusion, G = roughness, B = metallic |
| `Hyperweave_Emissive.png` | sRGB cyan status light mask |

## Previews and art review

`preview_front.png`, `preview_side.png`, `preview_34.png`, `preview_close.png`, and `preview_scale.png` are 1600 × 1200 Cycles renders from a fresh GLB re-import. The grey reference figure in `preview_scale.png` is 1.80 m tall. `contact_sheet.jpg` collects the five previews in a 2400 × 1200 grid at JPEG quality 85.

The first full render showed silver glare on the fabric, sharp cut edges, and excessive clearance at the clamp. The final pass raised textile roughness, softened the light, bevelled the compressed hems, and narrowed the clamp around the stack. Dedicated UV islands carry the hex texture continuously around the visible fold returns.

## Rebuild

Run `python build_hyperweave.py` with NumPy, Pillow, and Blender 5.x installed. `hyperweave_blender.py` handles geometry, validation, export, and rendering. Set `HYPERWEAVE_BLENDER` to select a different Blender 5.x executable. To render the initial art pass separately, run `python build_hyperweave.py --first-pass`.

All brief-listed deliverables are complete. Verification covered the GLB file, topology, UV0, embedded maps, and Blender re-import. Unreal Engine import was outside this verification.
