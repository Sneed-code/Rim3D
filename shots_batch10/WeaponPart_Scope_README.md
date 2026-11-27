# WeaponPart_Scope

A loose, weathered optic for Rim3D. The mesh includes the anodized tube, olive rubber armour, knurled turrets, raised objective cover, coated lenses, and a quick-release mount. The design and all texture maps were made procedurally for this asset.

## Mesh and axes

| Property | Value |
| --- | --- |
| File | `WeaponPart_Scope.glb` |
| Size in Blender X, Y, Z | **0.33039 × 0.07068 × 0.14377 m** in the open-cover pose |
| Triangles | **4,770** |
| Mesh objects | **1**, named `Body` |
| Pivot | `Body` at **(0, 0, 0) m**, ground level at the centre of the mounting footprint |
| Blender axes | Objective faces **+X**, **+Z** is up, and the side turret faces **−Y** |
| glTF axes | **+X** remains the objective direction, **+Y** is up, and the side turret faces **+Z** |
| Transforms | Identity at export with 1 unit = 1 metre |
| UVs | Single non-overlapping UV0 atlas with dilated gutters |

The cover, turrets, and latch are part of `Body` in a fixed display pose. The preview scene puts the scope beside a 1.80 m silhouette to show its scale.

## Textures

The GLB embeds all three 1024 × 1024 PNG maps in one material. The blue lens coating uses opaque, low-roughness PBR so the glass appearance shares the atlas.

| Texture | Interpretation |
| --- | --- |
| `WeaponPart_Scope_BaseColor.png` | sRGB pigment, coating tint, wear and dust |
| `WeaponPart_Scope_Normal.png` | Linear tangent-space normal, OpenGL **+Y** |
| `WeaponPart_Scope_ORM.png` | Linear packed map, **R** ambient occlusion, **G** roughness, **B** metallic |

## Previews

The five 1600 × 1200 previews were rendered in Cycles after importing the exported GLB into a clean Blender 5.3 Beta scene. `contact_sheet.jpg` combines them in two rows of three cells at 2400 px wide.

- `preview_front.png` shows the objective and open cover.
- `preview_side.png` shows the full profile and mounting footprint.
- `preview_34.png` shows the scope from above and in front of the turret side.
- `preview_close.png` shows the lens, cover hinge, and grip details.
- `preview_scale.png` shows the scope beside the 1.80 m human reference.

## Review and rebuild

The first render pass exposed an armour seam on the visible side and lighting that washed out the rubber and metal. The final pass moved the seam underneath, added moulded grip relief, concentrated aluminium wear along exposed edges, and tuned the lens surface to catch a distinct studio reflection. The final mesh also removes fully buried source faces.

The GLB checks passed for the triangle budget, mesh name and transform, UV0 overlap at twice the texture resolution, and pixel equality between each embedded map and its standalone PNG. All requested files are complete. Unreal import remains untested.

Run `python build_scope.py` in this folder with NumPy and Pillow installed. The script locates Blender 5.x, then builds and verifies the asset and renders the previews headlessly. Pass `--blender-exe PATH` to use a specific Blender executable.
