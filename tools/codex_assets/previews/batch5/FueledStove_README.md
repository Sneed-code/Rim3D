# FueledStove

FueledStove is a wood-burning cooking range for Rim3D. It combines a chipped enamel housing, two seasoned iron hotplates, a brick plinth, and a timber prep counter. Glowing coals sit behind a modeled grate. The pot, pan, chopping board, vegetables, utensils, and split firewood are part of the stove mesh.

## Game asset

| Property | Value |
| --- | --- |
| Export | [FueledStove.glb](FueledStove.glb) |
| Blender dimensions (X × Y × Z) | 2.900 × 0.900 × 1.468 m |
| Hotplate height | 0.902 m |
| Prep counter height | 0.890 m |
| Footprint | 3 × 1 cells at 1 m per cell |
| Geometry | 7,945 triangles, one mesh, one material, non-overlapping UV0 |
| Origin and transforms | Footprint center at floor level. Body scale 1, rotation 0. |
| Axes | Blender uses +Z up and −Y toward the working side. Default glTF export uses +Y up and +Z toward the working side. Units are metres. |

| Name | Type | Pivot or placement in Blender XYZ (m) |
| --- | --- | --- |
| `Body` | Static mesh | `(0, 0, 0)` at the footprint center on the floor |
| `Flame` | Empty | `(-0.805, -0.351, 0.490)` at the center of the firebox. The same point is `(-0.805, 0.490, 0.351)` in glTF XYZ. |

## Texture maps

All four textures measure 2048 × 2048 and are embedded in the GLB. Blender uses the same maps in the [native scene](FueledStove.blend), where they are packed for offline inspection.

| Map | Encoding and contents |
| --- | --- |
| [FueledStove_BaseColor.png](FueledStove_BaseColor.png) | sRGB surface color with procedural pigment, soot, oxidation, and wear. Lighting and occlusion are excluded from this map. |
| [FueledStove_Normal.png](FueledStove_Normal.png) | Linear tangent-space normal, OpenGL +Y. Includes cast texture, shallow scratches, fasteners, and surface seams. |
| [FueledStove_ORM.png](FueledStove_ORM.png) | Linear RGB. Red is ambient occlusion, green is roughness, and blue is metallic. |
| [FueledStove_Emissive.png](FueledStove_Emissive.png) | sRGB ember radiance. The GLB carries the emission strength in `KHR_materials_emissive_strength`. |

## Review

[Contact sheet](contact_sheet.jpg) · [Front](preview_front.png) · [Left side](preview_side.png) · [Three-quarter](preview_34.png) · [Close-up](preview_close.png) · [Scale](preview_scale.png)

The five 1600 × 1200 previews were rendered in Cycles after importing the GLB into an empty Blender scene. The grey figure in the scale view measures exactly 1.80 m and belongs to the review scene.

The first render made the iron read too bright, exposed an oversized ash drawer gap, and showed two unsupported logs. The revision darkened and roughened the cast iron, added localized soot, ash and brick relief, seated the logs and produce, tightened the drawer, and fused the scale figure into a continuous silhouette. A final export check verified triangle winding and shading normals, UV0 overlap, metric bounds, and the embedded pixels against the four standalone textures.

All brief deliverables are complete. Verification covered the exported files and Blender re-import. Unreal integration and any runtime effect attached to `Flame` remain game-side work.

## Rebuild

Run from this directory with Blender 5.3 Beta and a Python installation that has NumPy and Pillow.

```powershell
python -B reproduce.py all
python -B reproduce.py verify
```

[reproduce.py](reproduce.py) launches the build and render scripts headlessly. The geometry lives in [stove_model.py](stove_model.py) and [stove_geometry.py](stove_geometry.py). [stove_textures.py](stove_textures.py) generates the maps from the atlas layout. Use `--blender` to point the runner to a different Blender 5.x installation, or `--first` to reproduce the pre-review design in `.build/first`.
