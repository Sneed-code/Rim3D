# Table2x4 — Rim3D Codex Asset

Photoreal, game-ready **2 × 4 cell heavy refectory banquet table** built for **Rim3D** (Unreal Engine 5.8). Designed as a colonist-built frontier dining table with a thick 5-plank tabletop joined by 12 inlaid 3D butterfly (bowtie) keys, underside anti-cupping end cleats, two outer trestle frames plus a matching centre support trestle, drawbore wooden pegs, four longitudinal knee braces, and a continuous stretcher beam locked with protruding wedged through-tenons. Surface storytelling includes worn rounded dining edges, boot-scuffed stretcher wear, knife marks and drag scratches, overlapping cup/mug rings, and three melted candle wax puddle clusters (including drips flowing down the outer plank edge).

The BaseColor is calibrated as a **light, neutral, near-uncoloured grey-beige wood grain** (mean sRGB ~`[0.66, 0.64, 0.60]`, no baked shadows or AO) so Rim3D can dynamically tint the table to match whichever material the player builds it from, while wear and surface character live in the Normal and ORM maps.

---

## Asset Specifications

| Property | Value |
| --- | --- |
| **Asset Name** | `Table2x4` |
| **Final Size (X × Y × Z)** | **1.905 m × 3.904 m × 0.784 m** (2 × 4 cells footprint, tabletop at ~0.78 m) |
| **World Bounds (Blender)** | `X: [-0.9500, +0.9549] m`, `Y: [-1.9520, +1.9520] m`, `Z: [0.0000, +0.7842] m` |
| **Triangle Count** | **5,384 triangles** (3,032 polygons; budget ≤ 6,000 tris) |
| **Mesh Hygiene** | 0 n-gons, no hidden internal faces, consistent outward normals, weighted beveled shading, applied transforms (`Loc 0,0,0`, `Rot 0,0,0`, `Scale 1,1,1`) |
| **UVs** | Single clean, non-overlapping `UVMap` (UV0), grain-aligned along U, ~8 px island margin at 2048 × 2048 |
| **Materials** | 1 material (`M_Table2x4`) |

---

## Parts & Pivots

| Object Name | Pivot (Blender World Space) | Role |
| --- | --- | --- |
| `Body` | `(0.000, 0.000, 0.000)` — footprint floor centre | Single joined static mesh containing the planked top, butterfly keys, cleats, three trestle frames, pegs, braces, stretcher beam, wedged through-tenons, and melted wax drips. |

---

## Axis Convention

- **Authoring (Blender)**: Building convention — `+Z` up, front facing `-Y`, long axis along `Y`, footprint centred on `(0, 0, 0)` with sled feet resting flush on `Z = 0.000 m`.
- **Export (`Table2x4.glb`)**: Exported with Blender 5.2's glTF 2.0 exporter at default settings (`export_yup=True`, metres, tangents & normals included, all 3 textures embedded). Verified by re-importing into a fresh empty Blender scene.

---

## Textures (2048 × 2048 PNG, embedded in `Table2x4.glb`)

| File | Color Space | Contents / Channel Packing |
| --- | --- | --- |
| `Table2x4_BaseColor.png` | sRGB | Light, neutral, near-uncoloured grey-beige wood grain, plank-to-plank variation, subtle hardwood butterfly keys & pegs, faint cup rings, pale exposed scratches, and tallow wax drips. **No baked lighting, shadows, or AO.** |
| `Table2x4_Normal.png` | Linear (Non-Color) | OpenGL (`+Y`) tangent-space normal map combining Cycles high-poly-to-low-poly baked detail (80,820-tri sculpted HP) with procedural wood fibers, open pores, curved end-grain rings, saw kerfs, scratches, and boot scuffs via Reoriented Normal Mapping (RNM). |
| `Table2x4_ORM.png` | Linear (Non-Color) | Packed ORM: **R** = Ambient Occlusion (Cycles 64-sample GPU bake + micro-cavity pores/kerfs), **G** = Roughness (hand-rubbed satin edges `~0.35`, matte crevices/scratches `~0.88`, cup-ring sheen contrast, smooth tallow wax `~0.24–0.33`), **B** = Metallic (`0.0`). |

---

## Animation (`preview_anim.gif`)

This asset has **no moving parts and no animation**. Per `BRIEF.md`, `preview_anim.gif` is intentionally skipped.

---

## Delivered Files

- `Table2x4.glb` — Final glTF 2.0 binary with embedded 2048 × 2048 PBR textures.
- `Table2x4_BaseColor.png`, `Table2x4_Normal.png`, `Table2x4_ORM.png` — Source 2048 × 2048 PBR texture maps.
- `preview_front.png`, `preview_side.png`, `preview_34.png`, `preview_close.png`, `preview_scale.png` — 1600 × 1200 Cycles neutral grey studio renders generated from the re-imported `Table2x4.glb`.
- `contact_sheet.jpg` — 2400 × 1200 (2 × 3 grid, quality 85) preview overview and technical spec sheet.
- `README.md` — This file.
- Reproducible build scripts:
  - `build_table2x4.py` — Builds low-poly `Body` and 80.8k-tri high-poly meshes, unwraps grain-aligned UV0, and bakes Cycles OPTIX AO/Normal/World utility maps.
  - `generate_textures.py` — Synthesizes 2048 × 2048 `BaseColor`, `Normal`, and `ORM` maps in NumPy/Pillow.
  - `export_and_render.py` — Assigns `M_Table2x4` with glTF ORM wiring, exports `Table2x4.glb`, re-imports into a fresh scene, and renders all 5 previews.
  - `make_contact_sheet.py` — Generates `contact_sheet.jpg`.

### Rebuilding from Scratch

```powershell
$B = "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe"
& $B -b --factory-startup -P build_table2x4.py
python generate_textures.py
& $B -b --factory-startup -P export_and_render.py
python make_contact_sheet.py
```

---

## Self-Critique Improvement Passes

1. **UV & Baking Audit**: Caught an initial missing BMesh face-normal refresh before UV unwrapping that compressed plank islands; fixed normal updates and calibrated real-world texel scaling to achieve full `[0.004, 0.996]` UV packing with grain aligned along U.
2. **Pass 2 (Geometry, End-Grain & Albedo Fix)**: Fixed a BaseColor blending bug that over-brightened the initial albedo, seated all upright posts, cross-transoms, and knee braces flush into their mortise shoulders to eliminate joint bevel gaps, smoothed wax puddles to remove faceted star-pinching, replaced crossing end-grain sine waves with authentic curved end-grain rings and linear saw kerfs, widened butterfly inlays, and pulled back cameras for clean studio framing.
3. **Pass 3 (Surface Polish & Studio Finish)**: Aligned diagonal wood grain lengthwise on both XZ outer knee braces and YZ longitudinal braces, toned down coarse ring bump to eliminate corduroy ridging in close-ups, added 3×3 Gaussian anti-aliasing to diagonal scratches to remove pixel stair-stepping, calibrated neutral grey-beige albedo (~0.66 mean sRGB) with butterfly inlay kerfs, and faded the studio floor seamlessly into the grey horizon.

## Unfinished Items

None — all deliverables listed in `BRIEF.md` are complete and verified.
