# PlantFibre

A folded hank of dried tropical plant fibre with a twisted cord lashing, loose ends and a short hand-woven mat.

| Property | Value |
| --- | --- |
| Dimensions in Blender (X × Y × Z) | 0.316 × 0.186 × 0.069 m |
| Triangle count | 1,996 |
| Mesh and pivot | `Body`. Origin at the bottom centre of the full footprint (0, 0, 0). |
| Axis and units | Metres. Blender +Z up, front toward −Y. The default glTF export converts to +Y up and +Z front. |
| UVs and material | One material and one non-overlapping UV set. The thin woven strips and flyaway fibres use double-sided surfaces. |

## Textures

All three procedural 1024 × 1024 maps are embedded in `PlantFibre.glb` and also supplied as PNGs.

- `PlantFibre_BaseColor.png` stores sRGB fibre pigment without baked lighting or occlusion.
- `PlantFibre_Normal.png` stores an OpenGL tangent-space normal map (+Y).
- `PlantFibre_ORM.png` packs ambient occlusion in red, roughness in green and metallic in blue.

## Review

The five studio previews use a fresh Blender import of the GLB. The scale image includes a 1.80 m figure. The contact sheet presents the five views at half resolution.

The first renders showed blunt, uniform cut ends, a smooth-looking hank and wide gaps in the mat. The improvement pass added tapered fibre locks, finer flyaways and rolled cut edges. It deepened lengthwise colour and normal detail, tightened the spacing and crimp of the weave, and fitted the cord to the hank’s cross-section.

No unfinished items from the brief. The GLB and its packed material were checked through Blender re-import. Unreal import was outside this build.

## Rebuild

Run `python build_plantfibre.py` in this directory with Pillow, NumPy and Blender 5.x available. Set `PLANTFIBRE_BLENDER` to the Blender executable if it is installed elsewhere. Use `--draft-first` to rerun both art passes.
