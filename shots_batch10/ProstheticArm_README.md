# ProstheticArm

Original static pickup prop for Rim3D. The industrial prosthesis rests on its side with a slight elbow bend. It has a lined shoulder socket, sewn harness straps, a tubular steel frame, an exposed two-groove elbow drive, a forearm battery pod, and a three-finger gripper with an opposing thumb.

## Geometry and placement

| Property | Value |
| --- | --- |
| File | `ProstheticArm.glb` |
| Blender dimensions in metres (X × Y × Z) | 0.718162 × 0.301395 × 0.118000 |
| Geometry | 7,911 triangles |
| Mesh object | `Body` |
| Pivot | (0, 0, 0), centred across the ground footprint with the lowest contact at ground level |
| Blender axes | +X toward the fingertips, +Z up |
| glTF axes | +X toward the fingertips, +Y up, glTF Z = −Blender Y |
| UVs | One non-overlapping channel, `UV0` in Blender and `TEXCOORD_0` in glTF |
| Material | One opaque PBR material with three embedded PNGs |

The elbow, tendons, fingers, covers, and harness are fixed in the resting pose and belong to `Body`. The mesh has unit scale.

## Textures

The three 2048 × 2048 maps are original procedural textures. The standalone PNGs match the maps embedded in the GLB.

| File | Interpretation |
| --- | --- |
| `ProstheticArm_BaseColor.png` | sRGB surface colour. Carries paint, exposed substrate, oxidation, grease, and markings with no baked lighting, shadows, or AO. |
| `ProstheticArm_Normal.png` | Linear tangent-space OpenGL normal, +Y. Includes surface grain, screw slots, braided wire, webbing, and grip tread. |
| `ProstheticArm_ORM.png` | Linear RGB. Red is ambient occlusion, green is roughness, and blue is metallic. |

## Previews

All five 1600 × 1200 PNGs were rendered in Cycles from a fresh import of the exported GLB. The studio and the 1.800 m silhouette exist only in the preview scene.

- `preview_front.png` shows the near profile.
- `preview_side.png` shows the reverse profile, harness loop, and tendon guides.
- `preview_34.png` shows the socket opening and overall resting pose.
- `preview_close.png` shows the elbow, battery, and gripper.
- `preview_scale.png` shows the asset beside the human silhouette.
- `contact_sheet.jpg` presents the five views in a 2400 × 1320, two-row grid at JPEG quality 85.

## Review and rebuild

The first render pass exposed pale paint, angular straps, and a wrist that appeared to float. The revision reduced the studio exposure, differentiated painted covers from steel, rounded and anchored the webbing, added a rubber palm heel, and exposed more of the forearm rails. Close-up review led to a second adjustment that seated both main tendons in their pulley grooves and routed the finger tendons under the palm cover.

The build checks triangle count, unit scale, ground pivot, geometry and normals, UV bounds and overlap at texel centres, and the embedded texture pixels and channel assignments. The asset was re-imported and rendered in Blender 5.3 Beta on the RTX 5080 using OptiX. Unreal Engine import remains unverified. All files specified in the brief are complete.

Run `python build_prosthetic_arm.py` in this folder with Python 3, NumPy, Pillow, and Blender 5.x to recreate the GLB, textures, and previews. Pass `--blender <path-to-blender.exe>` to use another Blender installation. Blender runs in the background.
