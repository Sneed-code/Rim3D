# Flake

Flake is a static Rim3D inventory prop. The mesh includes a crumpled translucent pouch, a folded neck tied with a twist, warm off-white crystalline contents, and three loose shards.

## Asset specification

| Property | Value |
|---|---|
| Export | `Flake.glb`, glTF 2.0 binary, Blender 5.3 Beta exporter defaults |
| Size in Blender X × Y × Z | 0.10502 × 0.07105 × 0.02719 m |
| Triangle count | 1,494 total, including 600 crystal triangles and 894 film triangles |
| Mesh and pivot | `Body`, origin (0, 0, 0) at the bottom centre of the full footprint. All parts are static. |
| Axes | Metres, Blender +Z up. The glTF export uses +Y up and maps Blender (X, Y, Z) to glTF (X, Z, −Y). |
| UVs | One non-overlapping UV0 atlas shared by both materials |
| Materials | `Flake \| Crystals` is opaque and `Flake \| Tied Film` is double-sided with alpha blending. |

## Textures

All three procedural 1024 × 1024 PNGs are delivered alongside the GLB and embedded in it.

| File | Encoding and use |
|---|---|
| `Flake_BaseColor.png` | sRGB RGB reflectance with linear film opacity in alpha. RGB contains material colour and dirt only. |
| `Flake_Normal.png` | Linear OpenGL tangent-space normal (+Y) for creases, seam ridges, fingerprints, and mineral striae. |
| `Flake_ORM.png` | Linear packed texture. R is ambient occlusion, G is roughness, and B is metallic. |

## Previews and rebuild

The five 1600 × 1200 previews were rendered in Cycles from the exported GLB re-imported into a clean Blender scene. The scale view places the prop beside a 1.80 m grey reference figure. `contact_sheet.jpg` arranges the five views in a 2 × 3 grid at 2400 × 1200.

The review pass reduced the milky opacity of the clear film, sharpened the heat-seal and crease detail, tightened the tied end, and varied the size, placement, and gloss of the crystals.

Run from this folder with Python, NumPy, Pillow, and Blender 5.x installed:

```powershell
python build_flake.py
```

The script finds the installed Blender 5.3 Beta on the build machine. For another installation, pass `--blender "path/to/blender.exe"`. The default rebuild regenerates the final GLB, maps, five previews, and contact sheet.

All brief deliverables are complete. Unreal Engine import and in-engine translucency are unverified.
