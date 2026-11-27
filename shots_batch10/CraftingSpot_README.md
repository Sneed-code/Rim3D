# CraftingSpot — Rim3D Codex Asset

Game-ready, photorealistic primitive outdoor **CraftingSpot** for Rim3D (Unreal Engine 5.8). Built and baked procedurally in Blender 5.2.2 LTS with Cycles OPTIX high-poly-to-low-poly baking.

## Asset Specifications

| Property | Value |
|---|---|
| **Asset File** | `CraftingSpot.glb` (glTF 2.0 binary, embedded PNG textures, `+Y` up) |
| **Final Size (X × Y × Z)** | `0.901 × 0.708 × 0.252 m` (fits within one `1.0 × 1.0 m` cell, under `0.35 m` height) |
| **Bounding Box (Blender)** | `X: [-0.4505, +0.4505]`, `Y: [-0.3541, +0.3541]`, `Z: [0.0000, +0.2523]` m |
| **Triangle Count** | `3,906` triangles (budget: `4,000` tris; `0` n-gons; outward normals; scale `1, 1, 1`) |
| **UV Channels** | `UVMap` (`UV0`, deterministic 9-tile non-overlapping atlas with mipmap edge dilation) |
| **Materials** | `1` unified PBR material (`M_CraftingSpot`) |

## Parts & Pivots

| Mesh Object | Pivot / Origin (Blender) | Role |
|---|---|---|
| `Body` | `(0.0, 0.0, 0.0)` | Static joined mesh containing the woven palm-leaf mat (`~0.90 × 0.71 m`), squat tree-stump chopping block, flat volcanic-basalt stone anvil, half-finished carved hardwood bowl, scattered 3D wood chips and curled wood shavings, stone-headed mallet with lashing, knapped flint knife, bone awl, and coiled plant-fibre cord. All pieces sit flush on the mat/ground (`min Z = 0.000 m`) with zero floating geometry. |

## Axis Convention

- **Building convention** (Blender authoring space): `+Z` up, footprint centered on origin `(0, 0, 0)`, bottom resting flat on `z = 0.0 m`, and the open working side facing **`-Y`** so a colonist kneeling at `-Y` faces `+Y` toward the bowl, tools, stump, and anvil.
- **glTF Export**: Exported with Blender's default glTF 2.0 settings (`export_format='GLB'`, `export_yup=True`), converting to glTF's standard `+Y` up coordinate system with embedded textures.

## Texture List (`2048 × 2048` PNG)

All textures are authored procedurally and baked in Cycles OPTIX (no downloaded textures; pure albedo with zero baked lighting/shadows/AO in BaseColor):

| File | Resolution | Color Space | Contents |
|---|---|---|---|
| `CraftingSpot_BaseColor.png` | `2048 × 2048` | sRGB | Pure unlit albedo: natural dried palm frond variation & dirt wear, furrowed tropical bark & ringed chopping top, dark speckled basalt with chalky impact top, warm carved hardwood & raw bark heel, fresh sapwood shavings, dark smoky knapped flint, aged ivory bone, and golden-tan twisted plant-fibre cord. |
| `CraftingSpot_Normal.png` | `2048 × 2048` | Linear / Non-Color | Tangent-space normal map (OpenGL `+Y`): 2×2 herringbone palm weave + leaf veins, vertical bark fissures & top axe scars/cracks, basalt pitting, bowl adze facets, glossy conchoidal flint flake scallops, and 3-ply helical cord twist. |
| `CraftingSpot_ORM.png` | `2048 × 2048` | Linear / Non-Color | Packed PBR channels: **R** = raytraced Ambient Occlusion (64-sample Cycles AO with ground-plane contact), **G** = Roughness (glossy flint `0.20–0.34`, polished bone/oiled handles `0.28–0.56`, carved wood `0.50–0.72`, dry bark/mat/cord `0.70–0.94`), **B** = Metallic (`0.0` dielectric). |

## Previews & Animation

- **Rendered Previews** (`1600 × 1200` Cycles OPTIX from re-imported `CraftingSpot.glb` in a fresh studio scene):
  - `preview_34.png` — three-quarter hero view from above-front
  - `preview_front.png` — front view facing the `-Y` open working side
  - `preview_side.png` — side profile (`+X`) verifying low silhouette and flat ground contact
  - `preview_close.png` — detail close-up of carved bowl, wood chips/shavings, flint knife, mallet, and palm weave
  - `preview_scale.png` — scale verification alongside a `1.80 m` grey human silhouette
  - `contact_sheet.jpg` — `2 × 3` summary grid (`2400 × 1215`, JPEG quality 85)
- **Animation (`preview_anim.gif`)**: Intentionally **skipped**, per `BRIEF.md` (*"This asset has no moving parts and no animation: skip `preview_anim.gif` and say so in README.md."*).

## Rebuilding from Source

```powershell
# 1. Build geometry, UV atlas, bake 2048x2048 PBR maps, and export CraftingSpot.glb
& "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" -b --factory-startup -P build_crafting_spot.py

# 2. Re-import CraftingSpot.glb into a fresh scene and render all 5 studio previews
& "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" -b --factory-startup -P render_previews.py

# 3. Assemble contact_sheet.jpg
python make_contact_sheet.py
```

## Status / Unfinished Items

None — all deliverables, quality checks, and multi-pass art-director refinements are complete.
