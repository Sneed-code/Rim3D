# Stool — Rim3D Codex Asset

A sturdy, hand-made colonial frontier wooden stool built for **Rim3D** (Unreal Engine 5.8, photoreal survival look). Designed to fit a single 1 × 1 m cell with a thick three-plank round dished seat slab, rounded bullnose edges, four wedged through-tenons on the seat top, four splayed lathe-turned legs, staggered stretcher rungs with protruding wooden dowel pegs, and a visibly boot-scuffed front footrest rung. The base colour is kept a light, neutral, low-saturation grey-beige wood grain (with no baked shadows or AO) so the game engine can tint it cleanly for wood, steel, or stone material variants, while fine grain, pores, wear, and boot abrasion are carried primarily in the Normal and ORM maps.

## Final Dimensions & Mesh Stats
- **Bounding Box Size (X × Y × Z)**: `0.3826 m × 0.3814 m × 0.4539 m` (footprint centred on origin, bottom resting flat at `z = 0.0000 m`, fits cleanly inside one 1 × 1 m cell)
- **Seat Height**: `0.4539 m` at outer rim / `0.4455 m` at dished seat centre (~0.45 m)
- **Seat Diameter**: `~0.382 m` (~0.38 m)
- **Triangle Count**: `2,864` triangles (`1,536` faces; strictly within the `3,000` tri budget)
- **Mesh Hygiene**: Zero n-gons, zero hidden internal faces, 100% consistent outward normals, applied transforms (`scale = 1.0`), clean non-overlapping UV0 (`UVMap`)
- **Materials**: 1 material (`M_Stool`)

## Parts & Pivots
| Object Name | Type | Pivot / World Location (Blender +Z up) | Notes |
|---|---|---|---|
| `Body` | Mesh | `(0.0000, 0.0000, 0.0000)` | Joined stool body: 3-plank dished seat slab, 4 wedged through-tenons, 4 splayed turned legs, 4 staggered stretcher rungs (front rung boot-scuffed), 8 protruding wooden dowel pegs |
| `Seat` | Empty | `(0.0000, 0.0000, 0.4455)` | Sitting spot anchor at top centre of the dished seat |

## Axis Convention
- **Blender Source Convention (Building / Prop)**: `+Z` up, footprint centred on `(0, 0, 0)`, bottom resting flat on `z = 0`, **front facing `-Y`** (the boot-scuffed footrest rung is on the `-Y` side).
- **glTF Export (`Stool.glb`)**: Exported with Blender 5.2's default glTF 2.0 binary settings (`export_yup=True`, `+Y` up as required by the glTF specification, metres, embedded textures).

## Delivered Files
### Asset & Textures
- `Stool.glb` — Game-ready glTF 2.0 binary with embedded 2048×2048 PBR textures, `Body` mesh, and `Seat` Empty.
- `Stool_BaseColor.png` — `2048×2048`, sRGB, light neutral grey-beige tintable wood grain (unlit pure albedo; no baked lighting, shadows, or AO).
- `Stool_Normal.png` — `2048×2048`, Linear / Non-Color, Tangent-Space OpenGL (`+Y`), baked per-part from high-poly detail meshes with 3D procedural grain pores, plank seams, lathe ridges, tenon kerfs, peg bevels, and front-rung boot scratches.
- `Stool_ORM.png` — `2048×2048`, Linear / Non-Color, channel-packed:
  - **R**: Ambient Occlusion (128-sample raytraced Cycles full-assembly contact AO)
  - **G**: Roughness (burnished satin dished seat ~0.40, leg shafts ~0.55–0.65, matte pores ~0.78, dusty foot bottoms ~0.87, abraded boot-scuffed front rung ~0.92)
  - **B**: Metallic (`0.0`, non-metallic)

### Previews (Rendered from Re-imported `Stool.glb`)
- `preview_front.png` — Front view (`-Y`), `1600×1200` Cycles neutral grey studio
- `preview_side.png` — Side view (`+X`), `1600×1200`
- `preview_34.png` — Three-quarter view from above-front, `1600×1200`
- `preview_close.png` — Detail close-up (dished top, wedged through-tenons, bullnose end-grain, turned leg, dowel pegs, boot-scuffed front rung), `1600×1200`
- `preview_scale.png` — Scale reference next to a `1.80 m` neutral grey human silhouette, `1600×1200`
- `contact_sheet.jpg` — 2×3 contact sheet (`2400×1200`, JPEG quality 85) combining all five views and asset summary

### Reproducible Build Scripts
- `build_stool.py` — Builds low-poly `Body`, Empty `Seat`, and high-poly detail meshes; bakes 2048×2048 PBR maps via Cycles; packs ORM; exports `Stool.glb`.
- `render_previews.py` — Re-imports `Stool.glb` into a fresh Blender scene, validates objects/bounds/triangles, and renders all 5 studio previews.
- `make_contact_sheet.py` — Assembles `contact_sheet.jpg` via Pillow.

To rebuild from scratch:
```powershell
& "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" -b --factory-startup -P build_stool.py
& "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" -b --factory-startup -P render_previews.py
python make_contact_sheet.py
```

## Animation
- **No animation (`preview_anim.gif` skipped)**: Per `BRIEF.md`, this asset has no moving parts and no animation, so `preview_anim.gif` is intentionally omitted.

## Unfinished Items / Notes
- None — all required geometry, UVs, 2048×2048 PBR maps, `Stool.glb`, five studio previews, `contact_sheet.jpg`, and multi-pass art-director self-critique improvements (fixing dowel peg normal handedness, per-part matched baking to eliminate joint cross-projection seams, wood grain refinement, albedo luminance calibration, and front-rung boot wear) are complete.
