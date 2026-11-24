# MachiningTable

Rim3D frontier workshop prop for a 3 × 1 cell footprint. The steel bench carries a compact lathe, a swivel vise, a drill press, and a tool pegboard. An oil rag holds a fictional receiver-shaped casting and loose fittings. A parts tray and curled swarf dress the work surface.

## Dimensions and axes

| Measurement | Metres |
| --- | ---: |
| X, overall width | 2.9000 |
| Y, overall depth | 0.9000 |
| Z, overall height | 1.7875 |
| Worktop height | 0.9500 |

Blender uses metres and +Z up. The working side faces −Y. The centre of the footprint is at the origin and the feet meet Z = 0. The GLB uses the Blender exporter’s default +Y-up conversion. In glTF the working side faces +Z.

| Mesh object | Triangles | Pivot in Blender metres |
| --- | ---: | --- |
| `Body` | 8,617 | `(0, 0, 0)` at the footprint centre on the ground plane |
| `LatheChuck` | 414 | `(−0.978, 0.106, 1.247)` on the spindle. Rotate about local X. |
| **Total** | **9,031** | |

The chuck includes all three jaws. Both objects have unit scale and share one material and atlas. The GLB has triangulated geometry and a single, non-overlapping UV0 channel.

## Textures

| File | Resolution | Encoding |
| --- | --- | --- |
| [MachiningTable_BaseColor.png](MachiningTable_BaseColor.png) | 2048 × 2048 | sRGB surface colour, authored without baked lighting, shadows, or AO |
| [MachiningTable_Normal.png](MachiningTable_Normal.png) | 2048 × 2048 | Tangent-space OpenGL normal, +Y green, linear |
| [MachiningTable_ORM.png](MachiningTable_ORM.png) | 2048 × 2048 | Linear RGB with ambient occlusion in R, roughness in G, and metallic in B |

All maps were generated procedurally. The pixels in the three standalone maps match the images embedded in [MachiningTable.glb](MachiningTable.glb).

## Previews

The studio renders were made in Cycles from the GLB imported into a fresh Blender scene. Each PNG is 1600 × 1200. The grey scale figure measures 1.800 m from sole to crown and appears in the scale preview.

[Front](preview_front.png) · [Side](preview_side.png) · [Three-quarter](preview_34.png) · [Detail](preview_close.png) · [Scale](preview_scale.png) · [Contact sheet](contact_sheet.jpg)

The first render set revealed cropped feet in the side view, a gap at the lathe tailstock, and overly mottled steel reflections. The revision backed off the side camera, seated the quill collar in a taller casting, softened steel microrelief, and added localized wear, machine mounts, and pegboard hardware. The final previews were rendered again from the revised export.

## Source and rebuild

Select [MachiningTable.blend](MachiningTable.blend) in Codex and choose **Blender 5.3 Beta** from **Open** for native inspection. The editable scene has packed textures.

Run `python run_blender.py` from this folder with Pillow installed in the host Python. The launcher runs [build_machining_table.py](build_machining_table.py) in headless Blender 5.3 Beta with Blender’s NumPy. Set `MACHINING_BLENDER` to the executable path to select a different compatible Blender installation. The default run replaces the asset files and all previews with a fresh build.

The asset package is complete. Validation was performed on the GLB and its Blender re-import. Unreal project integration was outside this delivery.
