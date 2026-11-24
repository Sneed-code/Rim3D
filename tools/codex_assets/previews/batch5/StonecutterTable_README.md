# StonecutterTable

StonecutterTable is a 3 × 1 Rim3D workstation with a limestone worktop on a timber and steel frame. It includes a guarded motor-driven stone saw, a half-cut chunk, three finished blocks, two chisels, two mallets, a framing square, a spirit level, and a water pail.

## Dimensions and geometry

| Measurement | Value |
| --- | --- |
| Overall size (Blender X × Y × Z) | 2.900 × 0.900 × 1.276 m |
| Worktop height | 0.880 m |
| Triangle count | 7,637 of 8,000 |
| Material | One shared material, `StonecutterTable` |
| UVs | One non-overlapping set, UV0 (`TEXCOORD_0` in glTF) |

The overall height includes the stack of finished blocks. The model uses metres and applied unit scale. In Blender, +Z is up, the working side faces −Y, and the footprint is centred at the origin with the feet at Z = 0. Blender's default glTF exporter converts this to +Y up. The working side faces +Z in the GLB.

| Mesh | Triangles | Pivot in Blender (X, Y, Z), metres | Use |
| --- | ---: | --- | --- |
| `Body` | 6,617 | (0, 0, 0) | Fixed bench, saw housing, and accessories |
| `SawBlade` | 1,020 | (0.885, −0.042, 0.912) | Blade and arbor hardware. Rotate around local Y in Blender, or local Z in the GLB. |

## Textures

All maps are original 2048 × 2048 PNGs. The GLB embeds all three, and the source PNGs are included beside it.

| File | Encoding | Contents |
| --- | --- | --- |
| `StonecutterTable_BaseColor.png` | sRGB | Unlit surface colour, including pigment, oxidation, dust, and stains |
| `StonecutterTable_Normal.png` | Linear | Tangent-space OpenGL normals with +Y green |
| `StonecutterTable_ORM.png` | Linear | R ambient occlusion, G roughness, B metallic |

## Previews and review

The five 1600 × 1200 PNGs were rendered in Cycles on the RTX 5080 from a fresh Blender import of the final GLB. `preview_front.png` shows the working side, `preview_side.png` shows the saw end, `preview_34.png` shows the workstation from above and in front, and `preview_close.png` shows the cut and the drivetrain. `preview_scale.png` includes a plain 1.800 m grey figure. `contact_sheet.jpg` assembles the five views in a two-row, three-column grid.

The first render showed the short lower rails ending in open space, the worktop obscuring the arbor, and the cut reading as a plain rock. The final pass ties the rails into both end frames, raises the arbor, improves the kerf and runoff hose, and adds offcuts, worn guard markings, and timber splits. The close-up was moved higher so the kerf is visible. Interior mounting triangles were removed, and the final UVs were checked for triangle intersections.

## Rebuild

Run `python rebuild.py` from this folder with Blender 5.x and Python with NumPy, Pillow, and SciPy. The default Blender path targets the installed Blender 5.3 Beta. Pass `--blender PATH` for another installation or `--cpu` to render without a supported GPU.

`build_asset.py` models the meshes and packs UV0. `bake_textures.py` generates the original PBR maps and the contact sheet. `studio.py` exports the GLB, verifies a fresh import, and renders the five views. `python rebuild.py --first-pass` recreates the initial review set under `.stonecutter_work/draft`.

All files requested in the brief are complete. The GLB was verified in Blender. Unreal import and gameplay integration were outside this asset delivery.
