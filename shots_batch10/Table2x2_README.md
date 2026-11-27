# Table2x2 — Rim3D Codex Asset

A realistic, game-ready, photoreal square frontier dining table (`Table2x2`) built for **Rim3D** (Unreal Engine 5.8). Designed with a chunky 6-plank tabletop, perpendicular breadboard ends pinned by 16 proud bevelled wooden drawbore pegs, a recessed skirt apron with moulded lower lip and underside cross-battens/corner braces, and four thick lathe-turned colonial baluster legs pinned at the square pommels.

---

## Asset Summary

| Property | Value |
|---|---|
| **Asset Name** | `Table2x2` |
| **Delivered Binary** | `Table2x2.glb` (glTF 2.0 binary, default Blender exporter, `+Y` up, metres, embedded textures) |
| **Final Dimensions (X, Y, Z)** | `1.9000 m × 1.9000 m × 0.7839 m` (2 × 2 cells with 5 cm perimeter clearance; tabletop surface at `0.780 m`, proud breadboard pegs peak at `0.7839 m`) |
| **Bounding Box (Blender)** | Min `(-0.9500, -0.9500, 0.0000)` m — Max `(+0.9500, +0.9500, +0.7839)` m |
| **Triangle Count** | **4,804 tris** (budget: 5,000 tris; 2,538 polygons, 0 n-gons, consistent outward normals, applied scale `1, 1, 1`) |
| **UVs** | `UVMap` (UV0), hand-authored, 100% isotropic, grain-aligned per board/leg/peg, zero overlaps |
| **Materials** | 1 (`M_Table2x2`) |

---

## Parts & Pivots

| Object Name | Role | Pivot / Origin (Blender) | Notes |
|---|---|---|---|
| `Body` | Complete static table mesh (planked top, breadboard ends, 16 tabletop pegs, 8 leg-pommel pegs, 4 moulded apron rails, 4 turned legs, 2 underside battens, 4 corner braces) | `(0.000, 0.000, 0.000)` | Footprint floor centre; all four turned feet rest flat on `Z = 0.000 m` |

---

## Axis Convention

- **Building / Big Prop Convention (Blender):** `+Z` up, front facing `-Y`, footprint centred on `(0, 0, 0)`, bottom resting on `Z = 0.0 m`.
- **glTF Export (`Table2x2.glb`):** Exported with Blender 5.2's default glTF 2.0 settings (`export_yup=True`), writing standard glTF `+Y` up with metres scale and all PBR textures embedded.

---

## Texture List (2048 × 2048 PNG, Procedural + High-Poly Cycles Bake)

| File | Resolution | Color Space | Contents & Purpose |
|---|---|---|---|
| `Table2x2_BaseColor.png` | `2048 × 2048` | sRGB | Light, neutral, near-uncoloured grey-beige weathered wood grain (mean sRGB ~`(170, 163, 152)`, low saturation ~`0.11`) ready for UE5 player-chosen material tinting. **Zero baked lighting or AO shadows.** Includes per-plank tonal variation, darker cut end-grain pegs, subtle neutral cup-ring stains, and a neutral charcoal scorch burn mark. |
| `Table2x2_Normal.png` | `2048 × 2048` | Linear (Non-Color), OpenGL `+Y` | Exploded high-poly Cycles bake (`63,160` tris: 64-segment turned legs, rounded multi-segment worn bevels, organic hand-planed surface undulation & edge dents) combined via Reoriented Normal Mapping (RNM) with 3D wood grain, open hardwood pores, incised knife nicks with displaced fibre lips, water-swollen cup-ring grain, and alligator-cracked burn char. |
| `Table2x2_ORM.png` | `2048 × 2048` | Linear (Non-Color) | Packed channels (shared single image for `occlusionTexture` and `metallicRoughnessTexture` inside `Table2x2.glb`):<br>• **R:** Cycles 3D Ambient Occlusion + micro-AO in pores, knife cuts & burn cracks<br>• **G:** Roughness (`0.34–0.97`: rubbed satin bevels & elbow-wear zones vs. matte pores, raw knife nicks, water-damaged cup rings, matte end-grain pegs, crevice dust & dead-matte charred burn core)<br>• **B:** Metallic (`0.0`, pure wood dielectric) |

---

## Animation

**Skipped (`preview_anim.gif` omitted):** Per `BRIEF.md`, `Table2x2` is a static asset with no moving parts and no animation. Time was invested into high-poly exploded normal baking, 20-segment turned leg silhouettes, realistic wear detailing, and a full self-critique improvement pass.

---

## Previews Included

All previews are rendered directly from the **re-imported `Table2x2.glb`** in a fresh Blender 5.2 Cycles studio scene (`1600 × 1200`, neutral grey cyclorama, soft key/fill/rim + studio sky dome):

- `preview_front.png` — Front elevation (`-Y`)
- `preview_side.png` — Side elevation (`+X`, breadboard end profile)
- `preview_34.png` — Hero three-quarter from above-front
- `preview_close.png` — Macro detail close-up (breadboard joint, proud bevelled pegs, chamfered plank grooves, knife nicks, overlapping cup rings, charred burn mark)
- `preview_scale.png` — Real-world scale check alongside a plain `1.80 m` matte-grey human silhouette
- `contact_sheet.jpg` — `2400 × 1200` (2 × 3 grid, JPEG quality 85) combining all five previews plus asset specifications

---

## Rebuilding the Asset

All procedural build, texture synthesis, export, and render scripts are kept in this folder:

```powershell
$bl = "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe"
& $bl -b --factory-startup -P build_table2x2.py
python generate_textures.py
& $bl -b --factory-startup -P export_table2x2.py
& $bl -b --factory-startup -P render_previews.py
python make_contact_sheet.py
```

---

## Status / Known Limitations

- **All brief deliverables completed and verified.** No unfinished items.
