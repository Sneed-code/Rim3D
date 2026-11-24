# Dresser

Rim3D bedroom furniture for a 2 × 1 cell footprint. The dresser has six drawers in two columns, worn oiled timber, and forged steel pulls. A folded cloth, dressing mirror, salve tin, comb, and enamel dish with a ring sit on top.

## Mesh and scale

| Property | Final value |
| --- | --- |
| Dimensions in Blender X × Y × Z | 1.940 × 0.940 × 1.238 m, including the mirror and open pull |
| Height to the timber top | 0.916 m |
| Triangles | 5,652 |
| Mesh and pivot | `Body`, at (0, 0, 0) on the floor at the centre of the footprint |
| Material | One shared PBR material |
| UVs | One non-overlapping UV0 atlas |

The middle-right drawer is posed 91 mm open and has a modeled floor, sides, and rear wall. It shares the `Body` mesh and pivot with the other drawers, hardware, and tabletop items. Scale is applied at 1.

Blender uses metres, +Z up, and −Y toward the working side. Blender's default glTF export converts this to metres, +Y up, and +Z toward the working side.

## Textures

All three 2048 × 2048 PNGs are embedded in [Dresser.glb](Dresser.glb) and supplied separately. Every texture was generated for this asset.

| File | Channels and color space |
| --- | --- |
| [Dresser_BaseColor.png](Dresser_BaseColor.png) | sRGB surface color, free of baked lighting, shadows, and ambient occlusion |
| [Dresser_Normal.png](Dresser_Normal.png) | Linear tangent-space normal, OpenGL +Y |
| [Dresser_ORM.png](Dresser_ORM.png) | Linear packed data. Red is ambient occlusion, green is roughness, blue is metallic. |

## Previews

All five 1600 × 1200 PNGs were rendered in Cycles from a fresh GLB import using the RTX 5080. The grey reference silhouette is 1.800 m tall.

- [Front](preview_front.png)
- [Side](preview_side.png)
- [Three-quarter above front](preview_34.png)
- [Detail](preview_close.png)
- [Scale](preview_scale.png)
- [Contact sheet](contact_sheet.jpg), 2378 × 1293 px JPEG at quality 85

## Review and rebuild

The first studio render showed a dark seam beneath the top, wide drawer gaps, pale timber, and flat side panels. The revised model closes the top join, tightens the drawer reveals, and adds recessed side panels. Higher-contrast grain, localized edge wear, darker hardware, and a raised cloth fold give the surfaces more definition. The final side view frames the feet with room below them, and the scale view uses a clearer silhouette.

The exported GLB was checked for dimensions, transforms, triangle count, normals, UV overlap, and embedded textures. The embedded pixels match the supplied PNGs. All five views were checked after re-import into Blender. Unreal Engine import remains untested.

Run `python rebuild.py` in this folder with NumPy and Pillow installed. The launcher defaults to the installed Blender 5.3 Beta. Use `--blender` to specify another Blender executable. The build, procedural texturing, studio rendering, and export verification are in [build_dresser.py](build_dresser.py), [dresser_textures.py](dresser_textures.py), [render_dresser.py](render_dresser.py), and [verify_dresser.py](verify_dresser.py). Use `--version first` to recreate the first-pass materials and form in the staging folder.
