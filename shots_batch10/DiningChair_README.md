# DiningChair (Rim3D Codex)

A game-ready, photoreal colonial/tropical-plantation style dining chair built procedurally in Blender 5.2 and Python (`numpy` / `Pillow`) for Unreal Engine 5.8 (`Rim3D`).

## Final Dimensions (Metres)
- **Width (X):** `0.4840 m` (`-0.2420` to `+0.2420`)
- **Depth (Y):** `0.5320 m` (`-0.2450` to `+0.2870`)
- **Height (Z):** `0.9502 m` (`0.0000` to `+0.9502`)
- **Seat Height:** `0.460 m` (molded seat frame top) / `0.449 m` (contoured woven rattan cane center sag)
- **Footprint:** Fits well inside one `1.0 × 1.0 m` colony grid cell, bottom flush at `Z = 0.000 m`.

## Geometry & Budget
- **Triangle Count:** `3,384` triangles (budget: `5,000` tris)
- **Vertices / Polygons:** `1,847` verts / `1,722` faces (quads & triangles only; `0` n-gons)
- **Topology & Normals:** Consistent outward normals, weighted face-area custom normals applied to beveled edges, zero hidden internal faces, identity transforms (`location = (0, 0, 0)`, `rotation = (0, 0, 0)`, `scale = (1, 1, 1)`).
- **UVs:** Clean, non-overlapping `UVMap` (`UV0`) with 31 packed islands, longitudinal wood grain strictly aligned to `V` per member, undistorted planar mapping on the woven cane seat panel, and generous mipmap bleed margins.

## Parts, Materials & Pivots
- **`Body`** (`MESH`, pivot at `(0.000, 0.000, 0.000)` — cell floor centre):
  - Complete dining chair frame (curved continuous rear stiles, tapered beveled square front legs with stepped collars, sculpted serpentine colonial yoke crest rail, curved lower stay rail, three shaped vertical slats, molded trapezoidal bullnose seat frame, low arched H-stretcher, under-seat corner glue blocks, and two discrete underside frontier-repair steel brackets) plus the contoured woven rattan/cane seat panel and coiled perimeter binding spline.
  - **Material Slots (2, sharing one unified embedded 2048×2048 PBR texture atlas):**
    - `M_DiningChair_Frame` — Light, neutral, near-uncoloured weathered hardwood grain (light grey-beige, mean sRGB ~`168, 162, 153`) with softened worn edges, open wood pores, mortise dowel pegs, hand-oil wear on the crest rail, boot scuffs on lower legs, and discreet steel underside repair brackets. Designed so UE5 can cleanly tint the frame to any player-chosen material.
    - `M_DiningChair_Cane` — Natural pale straw woven rattan/cane 2-over-2 twill seat panel and coiled perimeter binding spline (mean sRGB ~`201, 182, 143`) with waxy silica reed sheen and subtle worn sitting patina. Kept as a separate material slot so tinting the frame leaves the cane seat its natural pale straw colour.
- **`Seat`** (`EMPTY`, pivot at `(0.000, -0.035, 0.449)` in Blender coordinates):
  - Sitting target marker positioned at the centre of the contoured cane seat surface, oriented with the chair's `-Y` front convention.

## Textures (2048×2048, PNG, Embedded in `DiningChair.glb`)
| File | Colour Space | Channels / Notes |
| --- | --- | --- |
| `DiningChair_BaseColor.png` | sRGB | Clean albedo (no baked lighting, shadows, or AO); neutral grey-beige wood grain + natural pale straw cane + brushed/lightly rusted steel underside brackets |
| `DiningChair_Normal.png` | Linear (Non-Color) | Tangent-space OpenGL (`+Y` up): woven rattan twill relief, coiled spline lashing, hardwood pores & grain fibers, scratches, mortise dowel pins, countersunk screw heads |
| `DiningChair_ORM.png` | Linear (Non-Color) | Packed PBR: `R` = Ambient Occlusion (Cycles 3D geometric AO × micro-weave/pore AO), `G` = Roughness (hand-oiled crest rail & waxy cane crowns vs matte scuffs/crevices), `B` = Metallic (`0.0` wood/cane, up to `0.88` underside steel repair brackets) |

## Axis Convention
- **Blender Source Convention (Building / Prop):** `+Z` up, `+X` right, front (sitting side) facing `-Y`, footprint centred on `(0, 0, 0)`, floor contact at `Z = 0.000 m`.
- **glTF 2.0 Export (`DiningChair.glb`):** Exported using Blender's default glTF binary settings (`export_yup=True`, metres, textures embedded, custom normals preserved), writing standard glTF `+Y` up for direct UE5 import.

## Animation (`preview_anim.gif`)
- **Skipped per brief:** This asset has no moving parts and no animation, so `preview_anim.gif` is intentionally omitted as instructed in `BRIEF.md`.

## Previews & Contact Sheet
All previews were rendered at `1600×1200` in Cycles (GPU OPTIX) from `DiningChair.glb` re-imported into a fresh Blender scene:
- `preview_front.png` — Front elevation (`-Y`)
- `preview_side.png` — Side elevation (`+X`)
- `preview_34.png` — Three-quarter view from above-front
- `preview_close.png` — Macro detail close-up (woven rattan cane, coiled spline, molded frame, wood grain, slats)
- `preview_scale.png` — Real-world scale check alongside a `1.80 m` plain grey human silhouette
- `contact_sheet.jpg` — 2×3 composite grid (`2400×1200`, JPEG quality 85)

## Rebuild Instructions
All procedural build scripts are preserved in this folder so the asset can be regenerated end-to-end:
```powershell
$B = "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe"
& $B -b --factory-startup -P build_chair_mesh.py
python generate_textures.py
& $B -b --factory-startup -P export_glb.py
& $B -b --factory-startup -P render_previews.py
python make_contact_sheet.py
```

## Notes / Unfinished Items
- None — all geometry, UVs, 2K PBR textures, glTF export, re-import verification, studio renders, self-critique improvement pass, and contact sheet are complete.
