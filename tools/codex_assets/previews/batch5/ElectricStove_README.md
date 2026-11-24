# ElectricStove

ElectricStove is a static cooking and wash station for Rim3D. It fits a 3 × 1 cell placement. Two of the four coil hotplates and the pilot lamp emit a faint orange glow.

| Property | Value |
| --- | --- |
| Runtime asset | `ElectricStove.glb` |
| Overall size (Blender X × Y × Z) | 2.900 × 0.900 × 1.248 m |
| Cooktop and counter height | 0.900 m |
| Triangle count | 7,983 |
| Materials | Shared PBR finish and alpha-blended oven glazing |
| UVs | One non-overlapping UV0 atlas with 5 px of dilation per island edge |

## Mesh and pivot

`Body` is the single static mesh. It includes the oven and racks, hardware, counter and sink, cookware, food, utensils, and rear cable. The pivot is `(0, 0, 0)` at the footprint centre on the floor. The object transform is identity and one unit equals one metre.

Blender uses +Z up, -Y toward the working side, and +X along the width toward the sink. Blender’s default glTF exporter converts this to +Y up and +Z toward the working side in the GLB.

## 2048 × 2048 textures

Both material slots share the four embedded images. The PNGs alongside the GLB contain the same pixels.

| File | Encoding and use |
| --- | --- |
| `ElectricStove_BaseColor.png` | sRGB material color with procedural wear and grime. Lighting, shadows, and ambient occlusion are excluded. |
| `ElectricStove_Normal.png` | Linear tangent-space normal, OpenGL +Y. Encodes brushing, dents, scratches, and hardware relief. |
| `ElectricStove_ORM.png` | Linear packed texture. R is ambient occlusion, G is roughness, and B is metallic. |
| `ElectricStove_Emissive.png` | sRGB mask for the two warm hotplates and the pilot lamp. |

## Previews and review

The five 1600 × 1200 PNGs were rendered in Cycles on the RTX 5080 from the GLB imported into a fresh Blender scene. `preview_scale.png` includes a neutral gray 1.800 m reference figure. [contact_sheet.jpg](contact_sheet.jpg) presents the full set in a 2-column, 3-row grid.

The first review exposed rear burner rims partly intersecting the backguard, an opaque-looking oven window, an unsupported cable connection, and hard joints on the scale figure. The final pass moved the backguards, recessed the sink under a distinct rolled rim, added glazing and interior racks, mounted the cable at a rear junction box, and smoothed the scale figure. It also refined the coil profiles, utensil spacing, and brushed-steel wear.

The export was checked for dimensions, identity transforms, triangle and material counts, degenerate and duplicate triangles, normal direction, UV0 overlap, and pixel equality between the embedded and standalone textures. All brief deliverables are complete. Runtime verification used Blender’s GLB importer. An Unreal Editor import was outside this build.

## Rebuild

With Blender 5.3 Beta and Python 3.13 with NumPy and Pillow installed, run the following from this folder.

```powershell
python build_electric_stove.py
```

The script models and unwraps the range, uses `stove_textures.py` to generate the PBR atlas, exports and validates the GLB, then launches a separate Blender process to re-import it and render the previews. It assembles the JPEG contact sheet at quality 85. Use `--blender PATH` to set a different Blender executable.
