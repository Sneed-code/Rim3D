# BionicEye

Bionic eye in an open graphite transit case for Rim3D. The eye has a nominal 25 mm ceramic shell, a machined iris with a cyan emitter and glass objective, and a four-wire harness with a gold-pinned connector seated in fitted foam.

| Property | Exported value |
| --- | --- |
| File | `BionicEye.glb` |
| Size in Blender X × Y × Z | 0.100000 × 0.071106 × 0.040333 m |
| Triangle count | 3,996 |
| Mesh object | `Body`, including the case, eye, and connector |
| Pivot | `(0, 0, 0)`, at ground level and centred on the full open item's projected bounds |
| Transforms | Unit scale, zero rotation and translation |
| Axes | Blender +Z up, case front facing −Y. The default glTF export maps Blender `(X, Y, Z)` to glTF `(X, Z, −Y)`. |
| UVs | One non-overlapping UV0 atlas |
| Materials | One atlas material for the assembly and one transmissive material for the objective glass |

## Textures

All four 1024 × 1024 PNGs are embedded in the GLB and supplied separately.

| Texture | Encoding and use |
| --- | --- |
| `BionicEye_BaseColor.png` | sRGB surface colour. Lighting, shadows, and AO are excluded. |
| `BionicEye_Normal.png` | Linear tangent-space OpenGL normal map (+Y). |
| `BionicEye_ORM.png` | Linear data. Red is ambient occlusion, green is roughness, and blue is metallic. |
| `BionicEye_Emissive.png` | sRGB colour for the cyan pupil ring. |

## Previews and review

`preview_front.png`, `preview_side.png`, `preview_34.png`, `preview_close.png`, and `preview_scale.png` are 1600 × 1200 Cycles renders of the GLB re-imported into a fresh Blender scene. The scale view places the asset next to a 1.80 m grey silhouette. `contact_sheet.jpg` arranges the five views in a 2 × 3 grid at 2352 × 1194 and JPEG quality 85.

The first render made the graphite look pale, exposed too much space around the eye, and clipped the connector pins in the detail view. The finished pass reduced the studio lighting, tightened the foam cradle, reseated the connector, and gave the cable more twist. It also refined the iris machining, added subtle case dust and markings, and adjusted the framing.

## Rebuild and verification

Run `python build_bioniceye.py` with NumPy and Pillow installed. The build uses Blender 5.x in the background and accepts `--blender PATH`. `bioniceye_atlas.py` paints the maps and `bioniceye_studio.py` renders the re-import. To reproduce the first review separately, run `python build_bioniceye.py --draft`.

Verified the triangle count, dimensions, UV interiors, embedded texture pixels, and Blender 5.3 Beta re-import. The glass uses glTF transmission and IOR extensions. The iris uses the glTF emissive strength extension. Unreal Engine 5.8 import remains unverified.
