# WoodenFoot

WoodenFoot is a ground pickup for Rim3D. The hand-carved wooden foot has a shallow, cloth-lined socket, a stitched leather rim, and two loose leather straps with forged iron buckles. The footprint includes the trailing strap and buckle ends.

| Property | Value |
| --- | --- |
| Size in Blender X × Y × Z | 0.2600 × 0.1142 × 0.1425 m |
| Triangle count | 2,920 |
| Mesh | `Body` |
| Origin and pivot | Bottom centre at (0, 0, 0) m |
| Blender axes | +X toward the toes, +Z up |
| glTF axes | +X toward the toes, +Y up, coordinates in metres |
| Material | `WoodenFoot_PBR` |
| UVs | One non-overlapping atlas in UV0 |

The socket, padding, straps, and buckles are fixed parts of `Body` in the ground pose.

## Files

| File | Contents |
| --- | --- |
| `WoodenFoot.glb` | Triangulated mesh and one material with all three maps embedded |
| `WoodenFoot_BaseColor.png` | 1024 × 1024 sRGB surface colour, painted procedurally without baked lighting, shadows, or ambient occlusion |
| `WoodenFoot_Normal.png` | 1024 × 1024 linear tangent-space normal, OpenGL +Y |
| `WoodenFoot_ORM.png` | 1024 × 1024 linear packed map, with occlusion in R, roughness in G, and metallic in B |
| `preview_front.png` | 1600 × 1200 view from the toe end |
| `preview_side.png` | 1600 × 1200 view from the negative Y side in Blender |
| `preview_34.png` | 1600 × 1200 view from above and in front |
| `preview_close.png` | 1600 × 1200 detail of the socket and near strap anchor |
| `preview_scale.png` | 1600 × 1200 studio view beside a 1.8 m grey mannequin |
| `contact_sheet.jpg` | All five previews in a two-row, three-column grid, 2400 × 1304, JPEG quality 85 |
| `WoodenFoot_build.py` | Procedural geometry and texture source, preview rendering, and validation |

## Rebuild and review

Run `python WoodenFoot_build.py` from this folder with NumPy and Pillow installed. The script uses Blender 5.3 Beta by default. Set `WOODENFOOT_BLENDER` to a Blender 5.x executable to override the path. The build exports with Blender's default glTF settings and renders the five previews in Cycles after re-importing the GLB into an empty studio scene. Run `python WoodenFoot_build.py --verify` to check the existing outputs.

After reviewing the first set of renders, the toe and instep were reshaped, the straps were fitted to the carved sides and given rivets and stitching, and the wood was darkened with less regular grain, lower sheen, and tapered blade marks. A second review led to softer grain variation, a clearer stitch line, and more space between the item and the scale figure.

All requested files are complete. The GLB was re-imported in Blender 5.3 Beta. Import into the Unreal project remains unverified.
