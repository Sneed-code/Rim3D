# TorchLamp

TorchLamp is a one-cell frontier light for Rim3D. The split timber post stands in a dry-stacked fieldstone footing. Leather bands hold the forged cage against the charred timber, and the iron oil cup carries a folded, oil-soaked wick. The game supplies the flame at the attachment point.

## Geometry and axes

| Property | Value |
| --- | --- |
| Size in Blender (X × Y × Z) | 0.5921 × 0.5703 × 1.4028 m |
| Grid space | 1 × 1 m cell |
| Triangles | 2,865 |
| Materials | One opaque material |
| UVs | One non-overlapping UV0 atlas |
| Blender axes | +Z up, front toward −Y |
| GLB axes | +Y up, front toward +Z, metres |

| Part | Type | Origin in Blender (X, Y, Z), metres | Origin in GLB (X, Y, Z), metres |
| --- | --- | --- | --- |
| `Body` | Static mesh | (0, 0, 0), centre of the footprint at ground level | (0, 0, 0) |
| `Flame` | Empty attachment node | (0.013, −0.005, 1.350), centred within the top folds of the wick | (0.013, 1.350, 0.005) |

## Textures

The three 2048 × 2048 PNGs are embedded in [TorchLamp.glb](TorchLamp.glb) and also supplied as loose files. The material recipes are procedural. The BaseColor contains surface colour only, and the baked ambient occlusion is stored in the ORM red channel.

| File | Colour space | Contents |
| --- | --- | --- |
| [TorchLamp_BaseColor.png](TorchLamp_BaseColor.png) | sRGB | Base colour |
| [TorchLamp_Normal.png](TorchLamp_Normal.png) | Linear | Tangent-space OpenGL normal, +Y |
| [TorchLamp_ORM.png](TorchLamp_ORM.png) | Linear | Red ambient occlusion, green roughness, blue metallic |

## Previews and review

The five 1600 × 1200 previews were rendered in Cycles from a fresh Blender import of the final GLB. The scale view includes a 1.8 m human silhouette. [Front](preview_front.png) · [Side](preview_side.png) · [Three-quarter](preview_34.png) · [Close-up](preview_close.png) · [Scale](preview_scale.png) · [Contact sheet](contact_sheet.jpg)

After the first full render, the timber was reworked into longer grain and checking, the bands and cage were fitted against the timber and cup, and the wick gained a separate folded edge. The footing received clearer rock strata and localized moss. The final mesh also drops 115 fully buried triangles.

All files in the brief are complete. Validation covered the exported GLB geometry, UVs, embedded textures, attachment transform, and Blender re-import.

## Rebuild

Run from this folder with Blender 5.x and a Python installation with NumPy and Pillow. Match Python's major and minor version to Blender's bundled Python. The launcher selects the installed Blender 5.3 Beta by default and accepts an alternate executable with `--blender`.

```powershell
python -B make_torchlamp.py
```

The launcher builds the mesh and textures, exports and checks the GLB, then renders the previews in a separate headless Blender process and composes the contact sheet. [Launcher](make_torchlamp.py) · [Blender build and render script](build_torchlamp.py) · [Surface recipes](torchlamp_surface.py)
