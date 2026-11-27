# Synthread

A Rim3D stack item with two dark polymer reels of silvery synthetic thread and a folded swatch of fine woven fabric. The swatch carries a generic barcode-style label.

| Property | Value |
| --- | --- |
| Asset | `Synthread.glb` |
| Blender dimensions (X × Y × Z) | 0.281953 × 0.218850 × 0.097800 m |
| GLB dimensions (X × Y × Z) | 0.281953 × 0.097800 × 0.218850 m |
| Geometry | 1,496 triangles, one mesh, one material |
| UVs | One non-overlapping UV0 atlas with edge padding |
| Units and axes | Metres. Blender +Z up, preview front from −Y. GLB +Y up with Blender's default exporter conversion. |
| Part and pivot | `Body`, at (0, 0, 0) on the bottom centre of the footprint. Both reels and the swatch are fixed. Scale is (1, 1, 1). |

The footprint is about 19 mm deeper than the nominal 0.20 m target to give the three pieces clearance.

## PBR textures

All three procedural maps are 1024 × 1024 PNGs, delivered both loose and embedded in the GLB.

| File | Interpretation |
| --- | --- |
| `Synthread_BaseColor.png` | sRGB. Surface colour, dust, wear and label ink. No baked lighting, shadows or ambient occlusion. |
| `Synthread_Normal.png` | Linear tangent-space OpenGL (+Y). Winding, weave, molded reel details and microtexture. |
| `Synthread_ORM.png` | Linear packed data. R ambient occlusion, G roughness, B metallic. |

The GLB reuses the BaseColor atlas for a cool grazing sheen via `KHR_materials_sheen`. Small warm-to-cool variations in the fibre colour contribute to the pearlescent look. The loose normal file uses the requested +Y convention. When importing that PNG separately with Unreal's DirectX normal convention, flip its green channel.

## Previews

`preview_front.png`, `preview_side.png`, `preview_34.png`, `preview_close.png` and `preview_scale.png` are 1600 × 1200 Cycles renders made after importing the exported GLB into a fresh Blender scene. The scale view includes a 1.8 m silhouette. `contact_sheet.jpg` contains the five views in a two-row, three-column grid at 2400 × 1200, JPEG quality 85.

## Review and verification

The first render pass exposed a reel intersecting the cloth, a hidden label, directional texture stretching on the plastic, and indistinct fibre detail. The final pass repositioned the parts, moved the label to the top ply, replaced the stretched scuffs with face-space wear, increased the reel silhouettes from 24 to 28 segments, and refined the fold, winding and weave.

Verification covered outward and consistent winding, closed source shells, no shell intersections, no overlapping UV0 triangle interiors in a 3072 × 3072 coverage check, the exported triangle count and pivot, and pixel equality between embedded and loose maps. All requested files are complete. Native Unreal import and in-game rendering were outside this folder-only build and were not tested.

## Rebuild

Run `python build_synthread.py` from this folder with NumPy, Pillow and Blender 5.x installed. Pass `--blender <path>` to select a specific executable. The launcher generates the atlas and runs `synthread_blender.py` headlessly. The optional `--draft` switch runs the first-pass variant and writes into `.review`.
