# MegascreenTelevision

MegascreenTelevision is a 3 × 1 cell recreation prop for Rim3D. A recessed, emissive display sits in a painted steel chassis with twin speaker bars, a media box, and service cables routed behind the stand.

## Geometry and axes

- Overall size is **2.900 × 0.900 × 1.900 m** in Blender X, Y, Z. The active screen measures **2.603 × 1.445 m**.
- The model has **6,272 triangles**, two meshes, and two materials. Each mesh has one non-overlapping UV0 layout and unit scale.
- Blender uses metres, +Z up, and −Y facing the viewer. The footprint is centred at the origin and rests at Z = 0. The default glTF export uses metres, +Y up, and +Z toward the viewer.

| Mesh | Triangles | Blender origin in metres (X, Y, Z) | Purpose |
| --- | ---: | --- | --- |
| `Body` | 6,248 | (0, 0, 0) | Ground-centre placement pivot. Includes the cabinet, stands, speaker bars, media box, and harness. |
| `Screen` | 24 | (0, −0.186, 1.135) | Centre of the recessed display face. Separate mesh and emissive material for swapping the display. |

The screen origin in glTF coordinates is (0, 1.135, 0.186).

## Textures

All seven PNGs are 2048 × 2048 and embedded in [MegascreenTelevision.glb](MegascreenTelevision.glb). The standalone maps match the embedded pixels.

| Material | Standalone files |
| --- | --- |
| Field-service chassis | [Body BaseColor](MegascreenTelevision_Body_BaseColor.png), [Body Normal](MegascreenTelevision_Body_Normal.png), [Body ORM](MegascreenTelevision_Body_ORM.png) |
| Laminated emissive display | [Screen BaseColor](MegascreenTelevision_Screen_BaseColor.png), [Screen Normal](MegascreenTelevision_Screen_Normal.png), [Screen ORM](MegascreenTelevision_Screen_ORM.png), [Screen Emissive](MegascreenTelevision_Screen_Emissive.png) |

BaseColor and Emissive use sRGB. BaseColor contains surface colour and wear. The normal maps use the OpenGL (+Y) convention. ORM is linear and packs ambient occlusion in red, roughness in green, and metallic in blue.

## Review

All five 1600 × 1200 views were rendered in Cycles on the RTX 5080 after re-importing the exported GLB into a clean Blender 5.3 Beta scene.

- [Front](preview_front.png)
- [Right side and rear cabling](preview_side.png)
- [Three-quarter](preview_34.png)
- [Detail](preview_close.png)
- [Scale with 1.80 m silhouette](preview_scale.png)
- [Contact sheet](contact_sheet.jpg), 1598 × 1993, JPEG quality 85

The first render review exposed overly mottled rear paint, loose-looking cable ends, and a dark band at the bottom of the scale view. The final pass toned down the paint and concentrated dust around the vents and bottom edge, added cable terminations and a media-box bracket, smoothed the bevel shading, softened the landscape on the display, and corrected the scale camera. The final export was checked for dimensions, triangle count, normal orientation, overlapping UV triangles, and texture embedding.

All requested deliverables are present. Unreal Engine 5.8 import remains unverified.

## Rebuild

Run `python build.py` from this folder with Python 3, NumPy, Pillow, and Blender 5.x. Use `--blender <path>` or set `BLENDER_EXE` to select an installation. The build runs Blender headlessly, generates the maps locally, exports the GLB, re-imports it for rendering, and assembles the contact sheet.

The source is in [build.py](build.py), [model.py](model.py), [textures.py](textures.py), and [previews.py](previews.py).
