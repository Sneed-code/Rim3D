# Yayo

A folded wax-paper packet with powder exposed at its torn corner, beside a black-capped glass vial. Both pieces form one static inventory asset.

| Property | Value |
| --- | --- |
| Asset | `Yayo.glb` |
| Size in Blender X × Y × Z | 0.093181 × 0.067000 × 0.027227 m |
| Triangle count | 1,496 |
| Mesh object | `Body` |
| Pivot | `Body` origin at the bottom centre of the combined bounds (0, 0, 0) |
| Axes and units | Metres. Blender +Z up, front toward −Y. Default glTF export converts to +Y up. |
| UVs and materials | One non-overlapping UV0 atlas. Two material slots for the opaque surfaces and clear vial glass. |

## Textures

All three procedural maps are 1024 × 1024 PNGs embedded in the GLB and supplied separately.

| File | Encoding |
| --- | --- |
| `Yayo_BaseColor.png` | sRGB surface colour without baked lighting or AO |
| `Yayo_Normal.png` | Linear tangent-space normal, OpenGL +Y |
| `Yayo_ORM.png` | Linear data. Red = ambient occlusion, green = roughness, blue = metallic. |

The opaque material covers wax paper, powder and the screw cap. The glass uses `KHR_materials_transmission` and `KHR_materials_ior`.

## Previews

`preview_front.png`, `preview_side.png`, `preview_34.png`, `preview_close.png` and `preview_scale.png` are 1600 × 1200 renders of the GLB re-imported into a fresh Blender scene. The last view places the asset beside a 1.8 m human silhouette. `contact_sheet.jpg` collects the five views in a two-column, three-row grid.

After reviewing the first set of renders, the second pass added relief along the packet fold, stronger paper and powder microdetail, deeper ridges on the cap and a fuller vial with a settled powder surface.

## Rebuild and verification

Run `python build_yayo.py` on Windows with NumPy, Pillow and Blender 5.x installed. Pass `--blender <path>` to select a specific binary. The script generates the maps, exports the GLB, re-imports it for the previews and assembles the contact sheet.

All brief deliverables are complete. The exported GLB was checked for mesh count, bounds, triangle count, UV overlap and embedded map resolution. Glass was verified in the Blender re-import. Unreal Engine 5.8 import and glass appearance were not tested.
