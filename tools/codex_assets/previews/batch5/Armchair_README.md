# Armchair

A one-cell Rim3D prop with an exposed timber frame, rolled canvas arms, a hand-stitched seat repair, and a striped wool throw. The timber is a muted grey-brown for material tinting.

| Runtime specification | Value |
| --- | --- |
| Blender bounds, X × Y × Z | 0.8603 × 0.8305 × 0.9341 m |
| Ground footprint | Centred on X and Y, with all four feet at Z = 0 |
| Geometry | 6,763 triangles, one mesh, one material |
| UVs | Non-overlapping UV0 with five pixels of edge bleed per island |
| Blender axes | +Z up, front facing −Y, metres |
| GLB axes | +Y up, front facing +Z, metres. Exported with Blender's default glTF settings. |

| Node | Blender local position in metres | Purpose |
| --- | --- | --- |
| `Body` | (0, 0, 0) | Static chair mesh with its origin at the ground-level footprint centre and unit scale. |
| `Seat` | (−0.00513, 0.03775, 0.48400) | Sitting marker at the cushion surface, parented to `Body`. Its local orientation matches `Body`. |

## Deliverables

- [Armchair.glb](Armchair.glb) contains the mesh, seat marker, material, and three embedded 2048 × 2048 maps.
- [Armchair.blend](Armchair.blend) is the packed source scene. Use the **Blender 5.3 Beta** target in Codex Open.
- [Armchair_BaseColor.png](Armchair_BaseColor.png) is the sRGB pigment and wear atlas. Studio lighting and AO are excluded from this map.
- [Armchair_Normal.png](Armchair_Normal.png) is the linear OpenGL tangent normal map (+Y). It carries the weave, patch stitches, wood pores, and screw slots.
- [Armchair_ORM.png](Armchair_ORM.png) is the linear packed data map. Red is ambient occlusion, green is roughness, and blue is metallic.
- [Front](preview_front.png), [side](preview_side.png), [three-quarter](preview_34.png), [detail](preview_close.png), and [scale](preview_scale.png) are 1600 × 1200 studio renders from the GLB re-imported into a clean Blender scene. The scale render includes a 1.800 m figure.
- [Contact sheet](contact_sheet.jpg) combines the five previews in two rows and three columns at 2376 × 1194, JPEG quality 85.
- [Build script](build_armchair.py) rebuilds the model, UVs, textures, export, and renders. Run `python build_armchair.py` from this folder with Pillow and Blender 5.3 Beta installed. The script uses Blender's bundled NumPy. Use `--blender` to point to another Blender 5.x executable.

## Render review

The first full render showed repetitive wood grain, a faint cushion repair, and a gap along the throw's crown. The second pass broke up the grain with irregular pores and local abrasion, made the patch and stitching more legible, and drew the throw closer to the roll. The revised hem has uneven folds and short knotted fringe. The rear fasteners now sit on the timber splices.

The final GLB re-import confirmed dimensions, the triangle count, unit scale, and the seating marker. The UV audit checked for overlaps and collapsed triangles. All three embedded images were checked against the standalone maps pixel for pixel. The packed `.blend` reopened in Blender 5.3 Beta with its external textures absent. Unreal Engine import and in-game seating remain unverified.
