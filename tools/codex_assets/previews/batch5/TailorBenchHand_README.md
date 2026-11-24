# TailorBenchHand

TailorBenchHand is a non-powered tailoring station for Rim3D. A cast sewing head and foot treadle share a long hardwood bench with a chalk-marked cutting area, shears, a wooden yardstick, a box of thread spools, a rolled hide, and a tucked stool. The model and textures were made for this asset.

## Size and geometry

| Property | Value |
| --- | --- |
| Blender dimensions in X × Y × Z | 2.900 × 0.900 × 1.390 m |
| Footprint | 3 × 1 one-metre cells |
| Worktop height | 0.900 m |
| Triangle count | 7,403 of 8,000 |
| Material | One opaque PBR atlas shared by both parts |
| UVs | One non-overlapping UV0 with padded islands |

| Mesh | Triangles | Pivot and purpose |
| --- | ---: | --- |
| `Body` | 6,871 | Ground at footprint centre `(0, 0, 0)`. The frame, treadle, belt, lower flywheel and workbench contents form one static mesh. |
| `HandWheel` | 532 | Axle at `(-0.419, -0.112, 1.238)` m in Blender. Rotate around local X. The wheel, crank and grip move together. |

Blender uses metres with +Z up and the working side facing −Y. The default Blender glTF export uses metres with +Y up and the working side facing +Z. The wheel pivot in GLB coordinates is `(-0.419, 1.238, 0.112)` m. Both objects have unit scale.

## Files

| File | Contents |
| --- | --- |
| `TailorBenchHand.glb` | glTF 2.0 binary with both meshes and all three embedded maps. |
| `TailorBenchHand_BaseColor.png` | 2048 × 2048 sRGB surface color. Pigment, staining and wear are painted without baked lighting, shadow or AO. |
| `TailorBenchHand_Normal.png` | 2048 × 2048 tangent-space normal, OpenGL (+Y). Includes wood fibres, textile grain, fastener slots and grip details. |
| `TailorBenchHand_ORM.png` | 2048 × 2048 linear data. Red is ambient occlusion, green is roughness and blue is metallic. |
| `preview_front.png`, `preview_side.png` | 1600 × 1200 front and right-side studio views. |
| `preview_34.png`, `preview_close.png` | 1600 × 1200 elevated three-quarter and detail views. |
| `preview_scale.png` | 1600 × 1200 studio view with a 1.800 m grey scale figure. |
| `contact_sheet.jpg` | 2400 × 1238 JPEG at quality 85. Five views and a title panel in two rows of three. |

## Review and verification

The first full render showed hard highlight bands on the casting, pale wood, floating tools, an obscured upper drawer and a belt that appeared to stop at the top. The revision softened the casting normals, reduced gloss, concentrated wood wear around the working area, seated the tools on the cloth, exposed both drawers and cut actual belt openings through two planks. It also added pedal supports, removed fully concealed triangles at component joins, and reframed the side view to show all the feet.

The final previews were rendered in Cycles on an RTX 5080 after re-importing the delivered GLB into a fresh Blender scene. Verification covered the re-imported dimensions, mesh names, pivots, triangle count, UV intersections, embedded map contents and output image sizes. All brief deliverables are complete. Unreal project integration was outside this asset delivery.

## Rebuild

With Blender 5.x, Python, NumPy and Pillow installed, run `python make_asset.py all` from this folder. Use `--blender PATH` to select a specific Blender executable. `build_asset.py` models and exports the GLB, `texture_atlas.py` paints the three maps and `render_asset.py` imports and renders the export. The build writes intermediate data and logs to `.tailor_work`. Run `python make_asset.py verify` to check the finished files without re-rendering.
