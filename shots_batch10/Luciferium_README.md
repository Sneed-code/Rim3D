# Luciferium

Luciferium is a graphite metal and smoked-glass spacer case with five blood-red capsules, a dark red seal, and gold contacts. The case is modeled in its display pose with the lid 23° above closed.

| Property | Value |
| --- | --- |
| Size in Blender X × Y × Z | 0.109800 × 0.061250 × 0.036821 m |
| Exported triangle count | 1,942 |
| Mesh object | `Body` |
| Pivot | (0, 0, 0) m at the center of the case base |
| Object scale | 1, 1, 1 |
| Blender axes | +Z up, −Y toward the latch, +X along the case |
| GLB axes | Y up, metres, using Blender’s default glTF axis conversion |
| UV sets | UV0, one padded atlas across both material slots |

The lid, window, clasp, contacts, and capsules are joined into `Body` in the fixed display pose. The case and capsules share one textured material. The window uses a second, alpha-blended material.

## Textures

All four maps are 1024 × 1024 procedural PNGs and are embedded in `Luciferium.glb`.

| File | Channels |
| --- | --- |
| `Luciferium_BaseColor.png` | sRGB surface color and wear |
| `Luciferium_Normal.png` | OpenGL tangent-space normals, +Y |
| `Luciferium_ORM.png` | Linear R ambient occlusion, G roughness, B metallic |
| `Luciferium_Emissive.png` | sRGB, base-color-tinted capsule emission |

## Previews and review

The five 1600 × 1200 previews were rendered in Cycles after importing the exported GLB into a fresh Blender scene. `preview_scale.png` includes a 1.8 m silhouette. `contact_sheet.jpg` arranges the five views in two rows of three at 2160 × 1080, saved at JPEG quality 85.

The first five-view render showed an aluminum-bright case, a cloudy window, and red capsules with little visible contrast. The revision darkened the graphite, reduced the window's opacity and glare, strengthened the dark capsule swirls, increased capsule silhouette resolution, and added a modeled lower case seam. The close-up shows the engraved chevron and recessed screw detail.

## Rebuild

Run `python build_luciferium.py` from this folder with NumPy, Pillow, and Blender 5.x installed. The build selects the installed Blender 5.3 Beta by default on this workstation. Set the `LUCIFERIUM_BLENDER` environment variable to override the executable. `luciferium_blender.py` contains the mesh, glTF export, and studio setup.

All brief deliverables are complete. GLB import and appearance were verified in Blender 5.3 Beta. Unreal Engine import was outside this asset build.
