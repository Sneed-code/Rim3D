# Table1x2 — Rim3D Codex Asset

A sturdy, hand-built tropical-frontier 1×2 plank dining/work table for **Rim3D** (Unreal Engine 5.8). Constructed with four individual longitudinal planks, mortise-and-tenon breadboard ends pinned by proud octagonal drawbore pegs, softened worn edges, chunky chamfered square legs, recessed apron rails, eight diagonal timber knee-braces, under-top cross cleats, and a low H-stretcher frame secured by wedged through-tenons.

The albedo is authored as a **light, neutral grey-beige weathered wood grain** with zero baked lighting or AO, designed so Unreal Engine can multiply any player-selected material tint cleanly across the entire asset while preserving realistic wear in the Normal and ORM maps.

---

## Final Asset Specifications

| Property | Value |
|---|---|
| **GLB File** | `Table1x2.glb` (glTF 2.0 binary, default Blender export: `+Y` up, metres, textures embedded) |
| **Dimensions (X × Y × Z)** | **0.951 m × 1.900 m × 0.783 m** (tabletop surface at `0.780 m`; proud breadboard pegs reach `0.783 m`; 1 × 2 cell footprint) |
| **Triangle Count** | **3,840 triangles** (2,024 faces: 1,816 quads, 208 tris, 0 n-gons; budget: 5,000 tris) |
| **Mesh Integrity** | 0 n-gons, 0 loose vertices, 0 non-manifold (>2-face) edges, 0 zero-area faces, 0 hidden internal mating faces, consistent outward normals, applied transforms (`scale = 1.0`) |
| **UVs (`UV0`)** | 148 grain-aligned, positive-winding islands; **72.2% UV area utilization** with **0.0% overlap** and 0 out-of-bounds vertices |
| **Materials** | **1** PBR material (`M_Table1x2`) |

---

## Parts & Pivots

| Object Name | Pivot / Origin (Blender Space) | Notes |
|---|---|---|
| `Body` | `(0.0, 0.0, 0.0)` — footprint floor centre, bottom of legs at `Z = 0.0` | Single static mesh combining tabletop, pegs, legs, aprons, cleats, corner knee-braces, H-stretcher, and tusk wedges. |

---

## Axis Convention

- **Blender authoring space**: `+Z` up, metres, building convention — footprint centred on `(0, 0, 0)`, bottom resting on `Z = 0`, long side along `Y` (`[-0.950, +0.950]`), short width along `X` (`[-0.475, +0.475]`), front facing `-Y`.
- **glTF binary (`Table1x2.glb`)**: Exported with Blender 5.2's default glTF 2.0 exporter (`+Y` up, textures embedded). Verified by re-importing `Table1x2.glb` into a fresh Blender scene prior to rendering all previews.

---

## PBR Textures (2048 × 2048 PNG, Procedural + Cycles High-to-Low Bake)

| File | Color Space | Channels / Purpose |
|---|---|---|
| `Table1x2_BaseColor.png` | sRGB | Light neutral grey-beige wood grain (`mean sRGB ≈ [188, 180, 170]`), individual board variation, open pores, subtle watermarks, dusty lower leg tips, and denser end-grain pegs. **No baked lighting, shadows, or AO.** |
| `Table1x2_Normal.png` | Non-Color (Linear) | Tangent-space **OpenGL (`+Y` up)** normal map combining Cycles high-to-low bevel/undulation bake with RNM-blended wood grain fibers, open pores, seasoning checks, carved knife nicks, cup-ring rims, and edge impact dings. |
| `Table1x2_ORM.png` | Non-Color (Linear) | Packed **R = Ambient Occlusion** (96-sample Cycles geometric AO + micro-cavity AO), **G = Roughness** (`0.42–0.94`: satin hand-rubbed tabletop/edges & boot-worn stretcher vs matte open pores, cup rings, knife cuts, end-grain, and floor dust), **B = Metallic** (`0.0` everywhere for clean tintable wood). |

---

## Previews & Animation

- `preview_front.png` (1600×1200) — Front (`-Y`) studio view rendered from re-imported `Table1x2.glb`.
- `preview_side.png` (1600×1200) — Long side (`-X`) studio view.
- `preview_34.png` (1600×1200) — Three-quarter above-front hero view.
- `preview_close.png` (1600×1200) — Macro detail close-up of breadboard seam, proud pegs, plank V-groove, knife nicks, cup rings, and softened worn edges.
- `preview_scale.png` (1600×1200) — Scale verification alongside a plain `1.80 m` grey human reference silhouette.
- `contact_sheet.jpg` (2400×1200, JPEG Q85) — 2×3 grid combining all five previews plus spec card & texture swatches.
- **`preview_anim.gif` omitted**: As instructed in `BRIEF.md` (*"This asset has no moving parts and no animation: skip `preview_anim.gif` and say so in README.md"*), `Table1x2` is a static prop with no moving parts and no animation.

---

## Reproducible Build Pipeline

All build scripts are retained in this folder so the asset can be regenerated end-to-end:

1. `blender -b --factory-startup -P build_table1x2.py` — Builds low-poly `Body` + high-poly `Body_HighPoly`, generates grain-aligned `UV0`, validates topology, bakes Cycles float32 maps, and writes `Table1x2_work.blend`.
2. `python generate_textures.py` — Synthesizes `Table1x2_BaseColor.png`, `Table1x2_Normal.png`, and `Table1x2_ORM.png` (2048×2048).
3. `blender -b --factory-startup -P export_glb.py` — Builds single PBR material `M_Table1x2` (with `glTF Material Output` Occlusion) and exports `Table1x2.glb`.
4. `blender -b --factory-startup -P render_previews.py` — Re-imports `Table1x2.glb` into a fresh Blender scene, audits the imported mesh, and renders all five 1600×1200 Cycles studio previews.
5. `python make_contact_sheet.py` — Assembles `contact_sheet.jpg`.

---

## Known Limitations / Unfinished Items

- None. All geometry, UV, PBR texture, GLB export/re-import, preview, contact sheet, and self-critique refinement requirements from `BRIEF.md` are complete.
