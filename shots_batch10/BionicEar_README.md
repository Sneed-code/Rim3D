# BionicEar

An open graphite carry case holding an ivory composite bionic ear, a cochlear implant module, and a coiled lead with a three-pin gold connector. The ear has a recessed speaker grille and a blue status light. The kit is near-new with fine dust on the foam and light handling marks on the case.

## Game asset

| Property | Value |
| --- | --- |
| File | [BionicEar.glb](BionicEar.glb) |
| Size in Blender, X × Y × Z | 0.099700 × 0.069576 × 0.039497 m |
| Triangles | 3,914 |
| Mesh object | `Body` |
| Materials | One PBR material, `BionicEar` |
| UVs | One non-overlapping `TEXCOORD_0` atlas |
| Body pivot | `(0, 0, 0)` at the bottom centre of the open kit's overall bounds |
| Part configuration | Static open case. The lid, latch, ear, module, lead, and plug are geometry in `Body`. |

Blender uses metres, +Z up, and −Y toward the latch. Blender's default glTF export converts that to +Y up and +Z toward the latch. The exported node uses identity transforms.

## Textures

The GLB embeds all four 1024 × 1024 procedural PNGs. The case and device share one atlas.

| File | Interpretation |
| --- | --- |
| [BionicEar_BaseColor.png](BionicEar_BaseColor.png) | sRGB surface colour with no baked lighting, shadows, or ambient occlusion |
| [BionicEar_Normal.png](BionicEar_Normal.png) | Linear tangent-space normal, OpenGL (+Y) |
| [BionicEar_ORM.png](BionicEar_ORM.png) | Linear packed channels. R = ambient occlusion, G = roughness, B = metallic. |
| [BionicEar_Emissive.png](BionicEar_Emissive.png) | sRGB blue status-light mask |

## Previews

[Contact sheet](contact_sheet.jpg) · [Front](preview_front.png) · [Right side](preview_side.png) · [Three-quarter](preview_34.png) · [Close-up](preview_close.png) · [1.80 m scale comparison](preview_scale.png)

The five 1600 × 1200 renders come from a fresh GLB import in Blender 5.3 Beta, rendered in Cycles on the RTX 5080. The 2-column × 3-row contact sheet is 2155 × 2613 pixels and uses JPEG quality 85.

## Rebuild and verification

Run `python .\make_bionicear.py` from this folder. The launcher uses Blender 5.x in background factory-startup mode, writes the atlas and GLB, then reimports the GLB to render the previews and assemble the contact sheet. It uses Python with NumPy and a Pillow installation compatible with Blender's bundled Python. The installed Blender 5.3 Beta uses Python 3.13. Pass `--blender` with an executable path for another Blender 5.x installation.

The first set of renders showed clipped highlights, coarse foam relief, a visible join in the grille's perforation pattern, and a tight three-quarter crop. The revision lowered the studio lights, reduced the foam and metal normal strength, mapped the grille as one continuous dish, refined the ear's tragus and concha, and settled the plug onto the foam. The revised views were reviewed after a fresh export and import.

Checked the exported triangle count, outward winding, UV overlap, embedded texture pixels, origin, and preview dimensions. All brief deliverables are complete. Unreal Engine import was outside the verification performed here.
