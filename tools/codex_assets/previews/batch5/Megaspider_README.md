# Megaspider — Rim3D Codex Asset

Game-ready, photorealistic, rigged and animated **Chitinid Warrior (Megaspider)** for **Rim3D** (Unreal Engine 5.8).  
Original design: a heavily armoured, rock-breaking hexapod arachnid-insect with glossy dark brown-black scratched chitin plates, venomous burnt-amber joint markings, soft corrugated pale articular membranes, modelled chitin bristles/spines, a ridged segmented abdomen, sculpted patella knee shields, clusters of 14 glossy obsidian-black eyes, and large serrated caliper mandibles.

---

## Final Specifications

| Property | Value |
|---|---|
| **GLB File** | `Megaspider.glb` (7.23 MB, textures embedded, Blender 5.2 default glTF +Y up export) |
| **Dimensions (Rest Pose)** | **X (width):** `2.025 m` · **Y (length):** `2.362 m` · **Z (height):** `1.137 m` |
| **Ground Alignment** | Standing in rest pose with all six clawed feet on `z = 0.000 m` |
| **Geometry** | **21,874 triangles** (budget: 25,000 tris) · 14,440 verts · 11,988 polys · **0 n-gons** |
| **Mesh Structure** | 1 skinned mesh object (`Megaspider`) parented to 1 `Armature` (Armature modifier) |
| **UVs** | Clean, non-overlapping `UV0` (`UVMap`, 234 shelf-packed islands, 68.3% coverage, 16 px gutter dilation) |
| **Material** | 1 PBR material (`M_Megaspider`) with BaseColor, Normal (+Y), packed ORM, and Emissive |
| **Skeleton** | **35 bones** (limit: ~80) · **Max 2 bone influences/vertex** (limit: 4) · 0 constraints, 0 drivers, 0 shape keys |

---

## Axis Convention & Facing Direction

- **In Blender (source scene):** `+Z` up, **`-Y` forward** (head and mandibles face `-Y`, abdomen extends toward `+Y`), Creature Left = `+X`, Creature Right = `-X`. Origin `(0, 0, 0)` and `Root` bone sit on the ground (`z = 0`) directly beneath the body centre.
- **In `Megaspider.glb` (standard glTF 2.0 export):** Blender's default glTF `+Y` up conversion maps the creature to **`+Y` up** and **`+Z` forward** (right-handed glTF standard, ready for Unreal Engine 5.8 import).

---

## Textures (2048 × 2048 PNG, Procedural)

All textures were procedurally synthesized from UV-space mesh attribute bakes and raytraced geometric ambient occlusion (no downloaded textures; zero baked lighting or shadows in `BaseColor`):

| File | Color Space | Contents |
|---|---|---|
| `Megaspider_BaseColor.png` | sRGB | Pure unlit albedo: deep dark espresso/brown-black chitin, organic burnt-amber joint & tergite-lip markings, fine abraded rock scratches, subtle crevice rock dust, pale fleshy joint membranes, obsidian black eyes |
| `Megaspider_Normal.png` | Non-Color (OpenGL `+Y`) | Micro-cuticle pitting, longitudinal chitin striations, fine carved rock scratches, and deep concentric accordion folds on soft joint membranes |
| `Megaspider_ORM.png` | Non-Color (Linear) | **R:** True BVH raytraced geometric + micro-cavity Ambient Occlusion · **G:** Roughness (`0.035` wet eyes, `0.16–0.28` glossy chitin, `0.48–0.62` clammy membrane, `0.68–0.88` matte scratches & dust) · **B:** Metallic (`0.0`, realistic organic dielectric) |
| `Megaspider_Emissive.png` | sRGB | Whisper-subtle deep-amber inner eye tapetum glint (keeps eye clusters reading as glossy black eyes while adding subtle predator depth) |

---

## Skeleton & Bone Hierarchy (35 Bones)

All hard armour plates are rigidly weighted (100% single bone); soft pale joint membranes smoothly blend across adjacent parent/child bones (max 2 influences/vertex).

```text
Root                              (0.000,  0.000, 0.000)  # Ground centre pivot (stationary in locomotion)
└─ Pelvis                         (0.000,  0.000, 0.540)  # Central thorax & scutum
   ├─ Thorax_Front                (0.000, -0.220, 0.560)  # Anterior pronotum shield
   │  ├─ Head                     (0.000, -0.520, 0.550)  # Plated head capsule, brow hood, 14 eyes
   │  │  ├─ Mandible_L            ( 0.140, -0.740, 0.430) # Left serrated rock-crushing mandible
   │  │  ├─ Mandible_R            (-0.140, -0.740, 0.430) # Right serrated rock-crushing mandible
   │  │  ├─ Pedipalp_L            ( 0.210, -0.680, 0.420) # Left armoured sensory pedipalp
   │  │  └─ Pedipalp_R            (-0.210, -0.680, 0.420) # Right armoured sensory pedipalp
   │  ├─ Leg_F_L_Coxa -> Femur -> Tibia -> Tarsus         # Front-Left leg (hip at +0.22, -0.30, 0.50)
   │  └─ Leg_F_R_Coxa -> Femur -> Tibia -> Tarsus         # Front-Right leg (hip at -0.22, -0.30, 0.50)
   ├─ Abdomen_01                  (0.000,  0.160, 0.560)  # Anterior segmented tergites
   │  └─ Abdomen_02               (0.000,  0.480, 0.620)  # Mid ridged tergites
   │     └─ Abdomen_03            (0.000,  0.780, 0.600)  # Posterior tergites & tail cerci
   ├─ Leg_M_L_Coxa -> Femur -> Tibia -> Tarsus            # Mid-Left leg (hip at +0.26, -0.02, 0.50)
   ├─ Leg_M_R_Coxa -> Femur -> Tibia -> Tarsus            # Mid-Right leg (hip at -0.26, -0.02, 0.50)
   ├─ Leg_B_L_Coxa -> Femur -> Tibia -> Tarsus            # Rear-Left leg (hip at +0.23, +0.24, 0.50)
   └─ Leg_B_R_Coxa -> Femur -> Tibia -> Tarsus            # Rear-Right leg (hip at -0.23, +0.24, 0.50)
```

---

## Animations (30 fps, Exported via NLA Tracks)

All six animations were verified on a clean fresh-scene re-import of `Megaspider.glb`. Locomotion (`Walk`, `Run`) uses an alternating tripod gait (`{F_L, M_R, B_L}` / `{F_R, M_L, B_R}`) solved analytically in world space so planted feet move backward at exact ground speed with zero sliding.

| Action | Frames (30 fps) | Duration | Loop | Stride / Motion Notes |
|---|---|---|---|---|
| **`Idle`** | `0 – 120` | `4.00 s` | Yes (`f0 == f120`) | Heavy abdomen/thorax breathing, weight shift with planted feet, predatory head scan, mandible grind & pedipalp probing |
| **`Walk`** | `0 – 18` | `0.60 s` | Yes (`f0 == f18`) | In-place alternating tripod gait at **`1.4 m/s`** -> **Stride length: `0.84 m / cycle`** (55% stance duty, verified stance foot speed `1.42 m/s`) |
| **`Run`** | `0 – 12` | `0.40 s` | Yes (`f0 == f12`) | In-place fast alternating tripod charge at **`3.8 m/s`** -> **Stride length: `1.52 m / cycle`** (42% stance duty, verified stance foot speed `3.91 m/s`) |
| **`Attack`** | `0 – 27` | `0.90 s` | No (Idle -> Idle) | Anticipation crouch, dramatic high rear-up spreading front legs & mandibles wide, explosive downward front-leg slash & snapping mandible clamp, recovery to Idle |
| **`Hit`** | `0 – 12` | `0.40 s` | No (Idle -> Idle) | Sharp recoil flinch back and to the side with mandible flare and front-leg stagger, recovery to Idle |
| **`Death`** | `0 – 45` | `1.50 s` | No (held at `f45`) | Shock convulsion followed by heavy collapse onto `z = 0` with classic arachnid leg curl, final twitch, and lifeless hold (verified last-frame mesh `min_z = +0.013 m`, nothing below ground) |

---

## Delivered Files

- `Megaspider.glb` — Final rigged & animated glTF 2.0 binary (textures embedded)
- `Megaspider_BaseColor.png`, `Megaspider_Normal.png`, `Megaspider_ORM.png`, `Megaspider_Emissive.png` — 2048×2048 PBR texture set
- `preview_front.png`, `preview_side.png`, `preview_34.png`, `preview_close.png`, `preview_scale.png` — 1600×1200 studio renders from re-imported GLB
- `contact_sheet.jpg` — 2400×1200 2×3 contact sheet (JPEG quality 85)
- `preview_anim.gif` — 800×600 three-quarter animation showcase (all 6 actions labelled sequentially)
- `build_megaspider.py`, `generate_textures.py`, `render_previews.py`, `assemble_media.py` — Full reproducible procedural pipeline
- `README.md` — This documentation

---

## Rebuilding from Scratch

```powershell
& "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" -b --factory-startup --python-exit-code 1 -P build_megaspider.py
& "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" -b --factory-startup --python-exit-code 1 -P render_previews.py
```

---

## Self-Critique & Iteration Summary

1. **Pass 1 -> Pass 2 (Major Visual & Structural Overhaul):**
   - Fixed floating femur bristles by rooting all setae cleanly inside the bowed limb centerline.
   - Replaced blunt cylindrical leg joint transitions with sculpted Bezier overlapping patella knee shields, muscular faceted-keeled femora, blade-keeled tibiae, and longer curved rock-gripping tarsal claws.
   - Enlarged and deepened the ridged segmented abdomen tergites and smoothed organic cheek plates.
   - Overhauled PBR textures from washed-out grey/peach to deep glossy espresso brown-black chitin with tight organic burnt-amber joint markings, fine anti-aliased hairline scratches, subtle cuticle micro-grain, and pitch-black 14-segment glossy eye domes.
   - Fixed verification script to evaluate the skinned `Megaspider` mesh directly rather than Blender's glTF bone-widget icosphere.
2. **Pass 2 -> Pass 3 (Macro Detail & Animation Floor Clearance Polish):**
   - Sculpted prominent interlocking serrated mandible saw-teeth projecting cleanly from the inner cutting edges.
   - Repositioned all 14 glossy black eye ocelli onto the outer anterior-lateral brow ridge surface for zero polygon overlap and strong readability from every angle.
   - Refined `Attack` downward strike and `Death` collapse floor clearances so `Death` rests strictly above `z = 0` (`min_z = +0.013 m`) and `Attack` slams cleanly onto the ground without head/mandible clipping.

## Known Limitations / Notes
- All required deliverables and animations are complete and verified on fresh glTF re-import; nothing left unfinished.
