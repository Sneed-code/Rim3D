# WeaponPart_HairTrigger

A static ground prop for Rim3D. The weathered gunmetal case has a backed ornamental recess, cast ribs, screw-head details, and a red anodised insert. Two loose pins rest on oil-stained twill. The recess and insert use fictional, nonfunctional geometry.

## Model

| Property | Value |
| --- | --- |
| File | `WeaponPart_HairTrigger.glb` |
| Bounds in Blender (X × Y × Z) | 0.137940 × 0.107462 × 0.056000 m, including cloth and pins |
| Triangles | 2,662 |
| Materials | One shared PBR material |
| UVs | `UV0`, exported as `TEXCOORD_0`, with non-overlapping islands |
| Mesh and pivot | `Body` at the bottom centre of the full prop, `(0, 0, 0)` m |
| Transforms | Unit scale and identity rotation |
| Axes | Blender uses metres, +Z up, and −Y toward the front. The default Blender glTF export converts to +Y up. |

`Body` includes the case, insert, cloth, and both loose pins. The prop uses a single static pivot.

## Textures

The three procedural 1024 × 1024 PNGs are supplied separately and embedded in the GLB.

| File | Encoding |
| --- | --- |
| `WeaponPart_HairTrigger_BaseColor.png` | sRGB surface colour, free of baked lighting and ambient occlusion |
| `WeaponPart_HairTrigger_Normal.png` | Linear tangent-space normal, OpenGL +Y |
| `WeaponPart_HairTrigger_ORM.png` | Linear data with ambient occlusion in R, roughness in G, and metallic in B |

## Previews and review

`preview_front.png`, `preview_side.png`, `preview_34.png`, `preview_close.png`, and `preview_scale.png` are 1600 × 1200 Cycles renders of a fresh GLB import. The scale view includes a plain 1.80 m human silhouette. `contact_sheet.jpg` arranges the five views in a two-row, three-column grid at 2400 × 1336 px and JPEG quality 85.

The first close-up showed broad, cloudy reflections on the case, and the base appeared to hover over the cloth. The revision tightened the brushed-metal grain, narrowed and rounded the edge highlights, added a clearer fabric weave and oil marks, and seated the case and both pins against the cloth surface. The final review checked the complete set of re-imported views, the texture payloads, geometry, and UVs.

All requested files are complete. Runtime verification was performed in Blender 5.3 Beta. Unreal project import remains unverified.

## Rebuild

Run `python build_asset.py` using Python 3.13 with Pillow installed. The driver invokes the installed Blender 5.3 Beta headlessly, uses Blender's bundled NumPy, and rebuilds the GLB, maps, renders, and contact sheet. To use another Blender 5.x build with matching Python, pass `--blender "PATH_TO_BLENDER.EXE"`.
