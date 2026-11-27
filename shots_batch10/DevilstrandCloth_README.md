# DevilstrandCloth

A folded bolt of dense fungal fabric for Rim3D. The bolt has four continuous layers, a loose maroon hem, and a single twisted cord tied around the stack.

| Property | Value |
| --- | --- |
| Game asset | `DevilstrandCloth.glb` |
| Blender dimensions (X × Y × Z) | **0.3007 × 0.2302 × 0.1047 m** |
| Triangle count | **1,466** |
| Mesh and pivot | `Body`, bottom centre at (0, 0, 0), scale (1, 1, 1) |
| Parts | One static mesh includes both the folded fabric and the binding. |
| Axes | Blender uses +Z up and −Y toward the draped front. The default Blender exporter writes glTF +Y up and +Z toward the draped front, in metres. |
| UVs | One non-overlapping UV0 atlas and one material. |

## Textures

All three textures are original procedural 1024 × 1024 PNGs, embedded in the GLB and supplied separately.

| File | Interpretation |
| --- | --- |
| `DevilstrandCloth_BaseColor.png` | sRGB dye and fiber color. Contains no baked lighting, shadow, or AO. |
| `DevilstrandCloth_Normal.png` | Linear tangent-space normal, OpenGL +Y. For manual hookup with a DirectX normal convention, invert green. |
| `DevilstrandCloth_ORM.png` | Linear packed texture. R = AO (white), G = roughness, B = metallic (black). |

## Previews and review

`preview_front.png`, `preview_side.png`, `preview_34.png`, `preview_close.png`, and `preview_scale.png` are 1600 × 1200 studio renders of the GLB re-imported into a clean Blender scene. The scale image uses a grey 1.8 m reference figure. `contact_sheet.jpg` collects the five views in a two-row, three-column grid.

The first renders exposed washed-out dye, a faint weave, and twine that disappeared into the drape. The revision deepened the color, added fiber and vein detail, reshaped the hem to clear the inner folds, and rebuilt the binding as one continuous cord with visible tips. The final GLB was re-imported and its triangle count, UV0, dimensions, and embedded maps were checked.

## Rebuild

Run `python build_devilstrand.py` with Python, NumPy, Pillow, and Blender 5.x installed. The launcher calls `devilstrand_blender.py` headlessly and regenerates the GLB, textures, five previews, and contact sheet in this folder. The optional `--draft` flag regenerates the first art pass and overwrites those same files.

All files requested in the brief are complete. Verification used Blender 5.3 Beta and Cycles on the RTX 5080. Unreal Engine 5.8 import was outside this folder-only build and remains untested.
