# WeaponPart_Compensator

A loose muzzle compensator and crush washer for Rim3D. The steel has a worn black nitride finish with scuffs, exposed machining on the edges, and carbon residue in the port pockets. The six angled pockets and both end wells are modeled as blind recesses. The rear well carries interrupted thread-like surface relief for the nonfunctional game prop.

## Asset specifications

| Property | Value |
| --- | --- |
| Export | [WeaponPart_Compensator.glb](WeaponPart_Compensator.glb), glTF 2.0 binary |
| Overall size in Blender X × Y × Z | 0.08047 × 0.05036 × 0.03020 m, including the loose washer |
| Triangle count | 2,988 of 3,000 |
| Materials | One shared PBR material |
| UVs | One non-overlapping UV set, UV0 |
| Mesh and pivot | `Body`, pivot at `(0, 0, 0)` on the ground plane at the center of the combined footprint. The fixed washer is a disconnected shell in `Body`. |
| Axes and units | Metres. Blender uses +Z up with the front along +X. Blender's default glTF export writes +Y up and preserves +X as the front. |

## Textures

All three procedurally authored 1024 × 1024 PNGs are embedded in the GLB and supplied separately.

| File | Encoding |
| --- | --- |
| [BaseColor](WeaponPart_Compensator_BaseColor.png) | sRGB surface color, free of baked lighting and ambient occlusion |
| [Normal](WeaponPart_Compensator_Normal.png) | Linear tangent-space normal, OpenGL +Y |
| [ORM](WeaponPart_Compensator_ORM.png) | Linear data. Red is ambient occlusion, green is roughness, and blue is metallic. |

## Previews

Each 1600 × 1200 preview was rendered in Cycles from a fresh Blender 5.3 Beta import of the final GLB. The scale view places the asset on the same ground plane as the 1.8 m silhouette.

[Front](preview_front.png) · [Side](preview_side.png) · [Three-quarter](preview_34.png) · [Detail](preview_close.png) · [Scale](preview_scale.png) · [Contact sheet](contact_sheet.jpg)

## Rebuild and review

From this folder, run `python build_asset.py` with Python, NumPy, Pillow, and Blender 5.x. Set `BLENDER_EXE` if Blender is installed in a different location. The script builds the mesh, paints the atlas, exports and validates the GLB, renders all five previews, and assembles the contact sheet. Pass `--draft` to build the review variant under `.build/draft`.

The first full render made the steel look too bright and the tool marks too coarse. The improvement passes softened the grain, darkened the nitride, concentrated the bright wear at the cut edges, and deepened the port pockets and front well. The end floors received planar UVs to remove the radial stretch visible in the front and detail views. The washer silhouette and wrap seams were also refined.

All requested files are complete. The GLB was re-imported and checked in Blender. Unreal integration was outside this delivery.
