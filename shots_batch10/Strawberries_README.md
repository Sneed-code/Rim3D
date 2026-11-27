# Strawberries

A palm-leaf tray holding ten strawberries, with three spilled onto the ground. The fruit includes two paler berries and one lightly bruised berry. The fruit, tray and sepals use modeled silhouettes and share one material.

| Property | Value |
| --- | --- |
| Final size in Blender X × Y × Z | 0.18897 × 0.14357 × 0.06620 m |
| Triangle count | 2,478 of 2,500 |
| Mesh and pivot | `Body`, at the bottom center of the full footprint |
| Transforms | Identity, with scale 1 on all axes |
| Axes | Metres and +Z up in Blender. The front preview faces the asset from −Y. Blender’s default glTF export writes +Y up. |
| Material and UVs | One material, one non-overlapping UV0 atlas |

## Textures

All three procedural textures are 1024 × 1024 PNGs. They are embedded in [Strawberries.glb](Strawberries.glb) and also supplied separately.

| File | Interpretation |
| --- | --- |
| [Strawberries_BaseColor.png](Strawberries_BaseColor.png) | sRGB surface color, free of baked lighting, shadows and occlusion |
| [Strawberries_Normal.png](Strawberries_Normal.png) | Linear tangent-space normal, OpenGL (+Y). Encodes seed dimples, palm fibers and sepal veins. |
| [Strawberries_ORM.png](Strawberries_ORM.png) | Linear packed map. Red is ambient occlusion, green is roughness and blue is metallic. |

## Previews

Cycles rendered all five 1600 × 1200 images from the exported GLB re-imported into a fresh Blender scene. The scale image includes a 1.8 m grey human silhouette.

- [Front](preview_front.png)
- [Side](preview_side.png)
- [Three-quarter](preview_34.png)
- [Close-up](preview_close.png)
- [Scale](preview_scale.png)
- [Contact sheet](contact_sheet.jpg), 2 columns × 3 rows, 1584 × 1902, JPEG quality 85

## Build and review

Run `python build_strawberries.py` with NumPy, Pillow and Blender 5.3 Beta installed. Set `STRAWBERRIES_BLENDER` to override the Blender executable path. The script regenerates the textures, exports the GLB and renders the five previews and contact sheet.

The first render pass showed that several berries hovered above the tray and the ripe skin looked too pink under the studio lights. The final pass settled the fruit onto support surfaces, concentrated geometry on the exposed berries, added a third spill, adjusted the reds and gloss, and strengthened contact in the preview lighting.

All requested deliverables are complete. The final stack is approximately 14 mm shorter than the 80 mm nominal height. The shallow contacts between fruit are part of the assembled stack. Verification covered the GLB and its Blender re-import, including the UVs, outward winding of the closed surfaces and all three embedded maps.
