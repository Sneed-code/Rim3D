# EndTable

An original Rim3D bedside table with a recessed drawer, slatted shelf, clothbound notebook, and an unlit candle in a patinated bronze dish. The timber has a neutral base tone for use with the game's stuff tint.

## Mesh and placement

| Property | Value |
| --- | --- |
| Asset bounds in Blender (X × Y × Z) | 0.450 × 0.450 × 0.618 m, including the tabletop props |
| Tabletop height | 0.550 m |
| Triangles | 3,988 of 4,000 |
| Mesh and pivot | `Body` at (0, 0, 0) m, centered on the footprint at floor height |
| Parts | The drawer, shelf, hardware, book, and candle assembly are fixed within `Body` |
| Material and UVs | One `EndTable_PBR` material, one non-overlapping UV0 atlas, unit object scale |
| Blender axes | +Z up, −Y toward the drawer front, +X toward the right-side repair plate |
| GLB axes | Blender's default glTF Y-up export in metres. +Y is up and +Z faces the drawer front. |

## Textures

The three 2048 × 2048 PNGs are procedurally authored and embedded in [EndTable.glb](EndTable.glb).

| Texture | Encoding |
| --- | --- |
| [EndTable_BaseColor.png](EndTable_BaseColor.png) | sRGB material color. Grain, wear, patina, and cup marks are intrinsic to the surfaces. Lighting, shadows, and ambient occlusion are excluded. |
| [EndTable_Normal.png](EndTable_Normal.png) | Linear tangent-space normal, OpenGL +Y. Includes grain, cloth weave, scored marks, and slotted fasteners. |
| [EndTable_ORM.png](EndTable_ORM.png) | Linear. Red is ambient occlusion, green is roughness, and blue is metallic. |

## Previews and review

The 1600 × 1200 Cycles renders use a fresh Blender import of the exported GLB. The scale view includes a plain 1.80 m grey figure.

[Front](preview_front.png) · [Right side](preview_side.png) · [Three-quarter](preview_34.png) · [Tabletop detail](preview_close.png) · [Scale](preview_scale.png) · [Contact sheet](contact_sheet.jpg)

The initial renders showed open shelf ends, freshly sanded-looking wood, and gaps around the book binding. The revision seated the slats in taller side rails, improved the timber grain and edge wear, closed the book binding, and softened the candle dish reflections. The candle also gained an uneven rim and a hardened wax run.

Verified the exported triangle count and winding, bounds, transforms, UV0 overlap, and texture bindings. The embedded texture pixels match the standalone PNGs. All brief deliverables are complete.

## Rebuild

Run `python build_endtable.py` from this folder with NumPy, Pillow, and Blender 5.x installed. The script prefers Blender 5.3 Beta on this machine and accepts `--blender <path>` for another installation. It generates the mesh and textures, exports and re-imports the GLB, renders the previews, and validates the outputs before copying them into this folder.
