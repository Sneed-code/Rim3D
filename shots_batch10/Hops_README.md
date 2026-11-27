# Hops

Hops is a harvested crop stack for Rim3D. The heap contains twelve modelled pale-green hop cones, two serrated leaves, and short curling bine stems. The bracts carry fine veins and scattered yellow lupulin flecks, and two cones have tan tips.

| Property | Value |
| --- | --- |
| Asset | `Hops.glb` (binary glTF 2.0 with embedded textures) |
| Blender X × Y × Z | 0.220 × 0.160 × 0.080 m |
| glTF X × Y × Z | 0.220 × 0.080 × 0.160 m |
| Geometry | 2,000 triangles, one mesh, one double-sided material |
| UVs | One non-overlapping set, Blender `UV0` / glTF `TEXCOORD_0` |
| Mesh and pivot | `Body`, at the footprint centre on the bottom plane, (0, 0, 0) m |
| Transform | Identity rotation and unit scale |
| Orientation | Blender uses +Z up and the front preview faces −Y. The default glTF export maps Blender (X, Y, Z) to glTF (X, Z, −Y), so +Y is up and +Z faces front. |

## Textures

All three maps are original procedural 1024 × 1024 RGB PNGs. The GLB embeds the same image data.

| File | Colour space and content |
| --- | --- |
| `Hops_BaseColor.png` | sRGB surface colour, including green bracts, tan tips and yellow lupulin. No baked lighting or ambient occlusion. |
| `Hops_Normal.png` | Linear tangent-space OpenGL (+Y) normals for fine veins and papery surface relief. |
| `Hops_ORM.png` | Linear. Red is ambient occlusion, green is roughness, and blue is metallic (0 for vegetation). |

## Previews

The five 1600 × 1200 PNGs were rendered in Cycles on an RTX 5080 after re-importing `Hops.glb` into a fresh Blender scene. `preview_scale.png` places a matte grey 1.800 m reference figure beside the pile. The studio and figure are preview-only geometry.

- `preview_front.png`
- `preview_side.png`
- `preview_34.png`
- `preview_close.png`
- `preview_scale.png`
- `contact_sheet.jpg` (3 columns × 2 rows, 2400 × 1260, JPEG quality 85)

## Rebuild and review

Run `python build_hops.py all` from this folder. The launcher uses Blender 5.3 Beta in background mode and accepts `--blender "path/to/blender.exe"` for another Blender installation. Use Python with NumPy and Pillow, with a Python major/minor version matching Blender's bundled interpreter. `hops_blender.py` creates and exports the mesh and renders the GLB. `hops_atlas.py` paints the atlas and lays out the contact sheet.

The first full review exposed a floating base layer, wide gaps in the bracts, and blown-out greens. The subsequent passes settled the base cones, revised the cone taper and bract overlap, strengthened the veins and tan tips, and rebalanced the grey studio lighting. All five views were rendered again from the revised GLB. Validation checked the exported bounds and pivot, triangle count, winding against vertex normals, non-overlapping UVs, and the embedded texture channels and dimensions.

All deliverables listed in the brief are complete. Unreal Engine 5.8 import has not been tested.
