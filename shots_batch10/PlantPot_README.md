# PlantPot — Rim3D Codex Asset

A game-ready, photoreal decorative potted tropical plant built for **Rim3D** (Unreal Engine 5.8). Designed for a single `1 x 1 m` floor cell: a hand-thrown neutral stony grey-beige stoneware vessel (`~0.45 m` wide, `~0.45 m` tall) with a thick rolled rim and wheel-incised geometric chevron band, filled with dark organic potting soil and embedded pebbles/bark mulch, holding a lush Bird-of-Paradise (*Strelitzia*) and small pleated fan-palm cluster (`~0.93 m` tall overall) featuring glossy bent double-sided alpha-cut leaves, natural wind-splits, and a few dry brown leaf tips.

---

## Final Asset Specifications

| Property | Value |
|---|---|
| **Asset Name** | `PlantPot` (`PlantPot.glb`) |
| **Final Size (X × Y × Z)** | **0.799 m × 0.791 m × 0.928 m** (fits cleanly inside one `1.0 × 1.0 m` cell; pot itself is `0.450 m` max diameter × `0.450 m` tall) |
| **Bounds in Blender** | `X: [-0.397, +0.401] m`, `Y: [-0.385, +0.407] m`, `Z: [0.000, +0.928] m` |
| **Triangle Count** | **3,604 tris** / 4,000 budget (`Pot`: 1,620, `Soil`: 336, `Foliage`: 1,648) |
| **N-gons / Hidden Internal Faces** | `0` n-gons; pot interior stops cleanly below soil perimeter with no wasted hidden faces |
| **Transforms** | Applied (`Location: (0,0,0)`, `Rotation: (0,0,0)`, `Scale: (1,1,1)`) |
| **Materials** | 3 materials (`M_PlantPot_Pot`, `M_PlantPot_Soil`, `M_PlantPot_Foliage`) |
| **UVs** | Single `UVMap` (`UV0`); non-overlapping UVs across pot body/base and soil/debris; foliage cards map cleanly onto 6 non-overlapping leaf tiles + 2 stem/sheath strips (standard foliage atlas instancing) |
| **Animation** | **None — `preview_anim.gif` skipped** per `BRIEF.md` (static decorative prop with no moving parts) |

---

## Part Names & Pivots

| Object Name | Pivot / Origin | Description |
|---|---|---|
| `Body` | `(0.000, 0.000, 0.000)` | Single combined mesh object with origin at the `1 × 1 m` cell floor centre, resting flat on `Z = 0`. Contains three material slots so the game can tint the pot independently of the soil and plant. |

---

## Axis Convention

- **Blender Authoring Space**: Building / large prop convention — `+Z` up, footprint centred on `(0, 0, 0)`, bottom resting at `Z = 0.000 m`, front facing `-Y`, units in **metres**.
- **glTF Export (`PlantPot.glb`)**: Exported with Blender 5.2's default glTF 2.0 exporter settings (`export_yup=True`, textures embedded), converting cleanly to glTF `+Y` up for Unreal Engine 5.8 import.

---

## Texture & Material List

All textures are procedurally generated at **2048 × 2048** (PNG) and embedded directly into `PlantPot.glb` as well as delivered in this folder. No baked lighting or shadows are present in BaseColor maps.

- **Normal convention**: OpenGL (`+Y` up)
- **ORM packing**: `R` = Ambient Occlusion, `G` = Roughness, `B` = Metallic

| Material Slot | Shader Type | Texture Files | Notes |
|---|---|---|---|
| `M_PlantPot_Pot` | Opaque, single-sided | `PlantPot_Pot_BaseColor.png` (sRGB)<br>`PlantPot_Pot_Normal.png` (OpenGL +Y)<br>`PlantPot_Pot_ORM.png` (Linear) | Light neutral stony grey-beige (`~RGB 166, 159, 148`) designed to be tinted cleanly by Rim3D's chosen material; wear, wheel ribs, incised chevron band, mineral efflorescence, rim chips, and burnished handling sheen live in Normal/ORM. |
| `M_PlantPot_Soil` | Opaque, single-sided | `PlantPot_Soil_BaseColor.png` (sRGB)<br>`PlantPot_Soil_Normal.png` (OpenGL +Y)<br>`PlantPot_Soil_ORM.png` (Linear) | Natural dark organic potting loam/humus with damp-to-dry roughness gradient, perlite grit, shredded bark fibers, and dedicated non-overlapping river pebble & bark mulch islands. |
| `M_PlantPot_Foliage` | Alpha-Masked (`MASK`, cutoff `0.5`), double-sided | `PlantPot_Foliage_BaseColor.png` (sRGB RGBA)<br>`PlantPot_Foliage_Normal.png` (OpenGL +Y)<br>`PlantPot_Foliage_ORM.png` (Linear) | 6 distinct glossy tropical leaves (Bird-of-Paradise blades with natural lateral wind-splits, subtle botanical venation, ragged dry matte brown leaf tips, plus a pleated fan-palm frond) with 20 px edge color dilation to prevent mipmap fringes, plus opaque petiole, clasping sheath, and unfurling shoot strips. |

---

## Previews & Contact Sheet

Rendered in Blender 5.2 (Cycles OptiX, 96 samples, neutral grey studio cyclorama, 1600 × 1200) directly from `PlantPot.glb` re-imported into a clean `--factory-startup` scene:

- `preview_front.png` — Front (`-Y`) view
- `preview_side.png` — Right side (`+X`) view
- `preview_34.png` — Elevated front three-quarter view
- `preview_close.png` — Detail close-up of rolled rim, incised band, soil/pebbles, crown, and dry leaf tip
- `preview_scale.png` — Scale reference alongside a plain `1.80 m` grey human silhouette
- `contact_sheet.jpg` — All five views plus spec summary in a 2 × 3 grid (`2400 × 1200`, JPEG quality 85)
- `preview_anim.gif` — **Skipped** (no moving parts or animation per `BRIEF.md`)

---

## Self-Critique & Refinement Pass Summary

After rendering and inspecting the first full pass, a second pass addressed every visual issue found:
1. **Plant Crown**: Replaced chunky brown basal cylinders with thin clasping green-olive leaf-sheath bracts rooted organically into the soil.
2. **Petiole-to-Leaf Transition**: Upgraded petioles to smooth 4-sided tapered stalks extending into each leaf's midrib centerline to eliminate visible attachment steps up close.
3. **Leaf Silhouette & Curvature**: Increased drooping leaf card subdivisions (`6 × 4` / `5 × 4`) to eliminate polygonal creases, softened overly uniform pinstripe veins into subtler organic botanical venation, and refined dry brown leaf tips with crisp ragged necrotic margins.
4. **Pot Material & Framing**: Deepened the neutral stony grey-beige base value and incised band relief so the pot never blows out to flat white under studio lighting, slightly raised the soil mound for visibility, added a seamless cyclorama backdrop, and adjusted camera framing so the full pot base and full `1.80 m` scale silhouette fit cleanly.

---

## Rebuilding from Source

All build scripts are included in this folder:

```powershell
python generate_textures.py
& "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" -b --factory-startup -P build_asset.py
& "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" -b --factory-startup -P render_previews.py
python make_contact_sheet.py
```

---

## Unfinished / Known Notes

- Nothing unfinished; all required deliverables (excluding `preview_anim.gif`, intentionally skipped per `BRIEF.md`) are complete and verified from the re-imported `PlantPot.glb`.
- In UE5.8, enable **Two Sided** and **Masked** blend mode (automatically imported from glTF `doubleSided: true` and `alphaMode: "MASK"`) on `M_PlantPot_Foliage`, and optionally apply a subtle **Two Sided Foliage** subsurface tint in-engine if desired for sunlit tropical interiors.
