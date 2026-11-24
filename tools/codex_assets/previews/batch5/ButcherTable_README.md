# ButcherTable

An original Rim3D workstation with a stained hardwood cutting block, a steel drain bay, and an enamel tool rack. The cleaver, knives, bone saw, two hooks, drainpipe, and bucket are integrated into the static prop.

| Measurement | Value |
| --- | --- |
| Overall Blender bounds (X × Y × Z) | 1.8996 × 0.9000 × 1.3777 m |
| Footprint | 2 × 1 cells |
| Cutting surface height | 0.909 m |
| Triangle count | 7,928 |
| Materials | 1 |
| UV sets | 1, non-overlapping UV0 |

## Parts and axes

| Node | Type | Origin or position in Blender metres (X, Y, Z) |
| --- | --- | --- |
| `Body` | Static mesh | `(0, 0, 0)`, centered at floor level |
| `WorkSpot` | Empty | `(-0.260, -0.048, 0.909)`, centered on the exposed chopping surface |

Blender uses metres, +Z up, and −Y toward the working side. Blender's default glTF export writes metres with +Y up and +Z toward the working side. The scale preview places a 1.80 m gray figure beside the table.

## Texture maps

All textures are original procedural 2048 × 2048 PNGs and are embedded in [ButcherTable.glb](ButcherTable.glb).

| File | Encoding and channels |
| --- | --- |
| [ButcherTable_BaseColor.png](ButcherTable_BaseColor.png) | sRGB, unlit surface color |
| [ButcherTable_Normal.png](ButcherTable_Normal.png) | Linear tangent-space normal, OpenGL +Y |
| [ButcherTable_ORM.png](ButcherTable_ORM.png) | Linear, R ambient occlusion, G roughness, B metallic |

## Previews

[Contact sheet](contact_sheet.jpg) · [Three-quarter](preview_34.png) · [Front](preview_front.png) · [Right side](preview_side.png) · [Detail](preview_close.png) · [Scale](preview_scale.png)

All five 1600 × 1200 views were rendered in Cycles on the RTX 5080 after importing the final GLB into a fresh Blender scene. The 2 × 3 contact sheet is 2400 px wide and uses JPEG quality 85.

## Review and verification

The first render showed a pale, smooth cutting block, strong reflections on the steel bay, and an overly tight side camera. The revision darkened the stain in the end grain, added cut marks and pitting, and increased the steel roughness. The side view also exposed a bucket-to-leg intersection. The final bucket has a new cradle with clearance around the leg, and the drain terminates above the bucket with a recessed outlet.

The exported GLB passed checks for scale, node transforms, triangle count, outward face winding, unique UV coverage, and exact pixel matches between the embedded and standalone maps. The asset files are complete. Unreal project integration has not been tested.

## Open and rebuild

Select [ButcherTable.blend](ButcherTable.blend) and choose **Blender 5.3 Beta** in the Codex Open menu. The scene contains the editable asset and packed maps.

To regenerate the asset and its previews in this folder, run the following with Python 3, NumPy and Pillow installed.

```powershell
python run_asset.py all
```

`run_asset.py` uses the installed Blender 5.3 Beta by default and accepts `--blender PATH`. The build entry point is `build_butchertable.py`, with `paint_atlas.py` for the textures and `verify_asset.py` for GLB validation and contact-sheet generation.
