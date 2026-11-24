# Spelopede — Rim3D Chitinid Stalker

Photorealistic, rigged, and animated game-ready creature asset for **Rim3D** (Unreal Engine 5.8).  
Original design of a mid-sized bio-engineered rock-breaking chitinid stalker: flat armored head with serrated curved pincer-mandibles, recessed glossy dark-amber compound ocelli, segmented sweeping antennae, 8 overlapping dorsal tergite carapace plates with twin keels and flared paratergite flanges, pale pleated intersegmental joint membranes, 8 pairs (16) of 3-segment spined arthropod legs, and a tapering whip-tail with flanking cerci prongs.

---

## Asset Summary

| Property | Value |
| --- | --- |
| **GLB File** | `Spelopede.glb` (10.45 MB, textures embedded, Y-up glTF 2.0 default) |
| **Dimensions (Rest Pose, Blender)** | **1.043 m (X width) × 1.624 m (Y length) × 0.564 m (Z height)** |
| **Bounding Box (Blender)** | X: `-0.521..+0.521 m`, Y: `-0.811..+0.813 m`, Z: `0.000..+0.564 m` |
| **Triangle Count** | **16,700 triangles** (budget: 20,000; `0` n-gons; consistent outward normals) |
| **Mesh Objects** | `1` skinned mesh (`Spelopede`) bound to `1` Armature (`Armature`) |
| **Rig / Skeleton** | **67 bones** (limit: ~80), max **2** bone influences per vertex (limit: 4) |
| **Constraints / Drivers / Shape Keys** | None exported (all animations baked to pose keyframes at 30 fps) |
| **UVs** | `UVMap` (UV0): 531 clean, non-overlapping islands inside `[0.004, 0.996]²` |
| **Materials** | `1` unified PBR material (`M_Spelopede`) |

---

## Axis Convention & Pivot

- **Blender authoring space**: `+Z` up, `-Y` forward (head & mandibles face `-Y`, tail points `+Y`), real-world metres, applied transforms (`scale = 1`).
- **glTF exported space (`Spelopede.glb`)**: Standard glTF `+Y` up, `+Z` forward (default Blender glTF exporter conversion with `export_yup=True`).
- **Origin & Root Pivot**: Origin `(0, 0, 0)` and `Root` bone sit on the ground plane (`z = 0.000`) directly beneath the creature's body centre, with all 16 tarsal claws resting squarely on `z = 0.000`.

---

## PBR Textures (2048×2048 PNG, Procedural / Float32-Baked, Embedded in GLB)

All four textures are saved alongside `Spelopede.glb` and embedded inside the GLB:

1. `Spelopede_BaseColor.png` — 2048×2048 sRGB pure albedo (no baked lighting, shadows, or AO): high-contrast dark espresso-brown & bold ochre-yellow carapace banding, pale parchment joint membranes, abraded rock scratches, limestone crevice dust, and deep amber ocelli.
2. `Spelopede_Normal.png` — 2048×2048 Non-Color OpenGL (`+Y`) tangent-space normal map: punctate beetle-carapace micro-pores, directional rock-abrasion gouges, ribbed joint membrane creases, and mandible growth striations.
3. `Spelopede_ORM.png` — 2048×2048 Non-Color channel-packed map:
   - **R**: Ambient Occlusion (Cycles 64-sample geometric AO + micro-pore cavity AO, wired via `glTF Material Output` -> `Occlusion`)
   - **G**: Roughness (glossy waxy chitin `0.17–0.30`, polished mandibles `0.14–0.24`, matte-leathery membranes `0.58–0.74`, wet mirror eyes `0.05`, rough scratches & dust `0.74–0.88`)
   - **B**: Metallic (`0.0` dielectric organic chitin)
4. `Spelopede_Emissive.png` — 2048×2048 sRGB subtle deep-amber pinpoint tapetum glint inside the 8 recessed compound ocelli lenses (`0` everywhere else).

---

## Skeleton & Bone Hierarchy (67 Bones)

Hard-surface chitin plates, mandibles, and leg shafts use rigid 100% single-bone skinning; intersegmental soft membranes, neck, joint condyles, antennae transitions, and whip-tail segments use smooth 2-bone blending.

- **`Root`** `(0.000, 0.000, 0.000)` — ground origin under body centre
  - **`Body`** `(0.000, +0.005, 0.266)` — central pelvis / Segment 4 anchor (+ `Leg_4_L/R_{Upper,Mid,Lower}`)
    - **Forward Spine Chain**:
      - `Spine_F1` (Segment 3, + `Leg_3_L/R_{Upper,Mid,Lower}`)
        - `Spine_F2` (Segment 2, + `Leg_2_L/R_{Upper,Mid,Lower}`)
          - `Thorax` (Segment 1 pronotum, + `Leg_1_L/R_{Upper,Mid,Lower}`)
            - `Head` `(0.000, -0.440, 0.250)` — flat cephalic shield & 8 ocelli
              - `Mandible_L` `(-0.095, -0.578, 0.205)` / `Mandible_R` `(+0.095, -0.578, 0.205)` — curved serrated rock-cracking pincers
              - `Antenna_L_1` -> `Antenna_L_2` / `Antenna_R_1` -> `Antenna_R_2` — front-lateral segmented sensory whips
    - **Backward Spine & Tail Chain**:
      - `Spine_B1` (Segment 5, + `Leg_5_L/R_{Upper,Mid,Lower}`)
        - `Spine_B2` (Segment 6, + `Leg_6_L/R_{Upper,Mid,Lower}`)
          - `Spine_B3` (Segment 7, + `Leg_7_L/R_{Upper,Mid,Lower}`)
            - `Spine_B4` (Segment 8, + `Leg_8_L/R_{Upper,Mid,Lower}`)
              - `Tail_1` (pygidium & flanking cerci prongs) -> `Tail_2` -> `Tail_3` (upturned whip lash tip)
- **Legs (16 legs × 3 bones = 48 bones)**: `Leg_{1..8}_{L,R}_Upper` (coxa/femur), `Leg_{1..8}_{L,R}_Mid` (tibia + traction spurs), `Leg_{1..8}_{L,R}_Lower` (tarsus + rock-gripping claw resting on `z = 0`).

---

## Animations (6 NLA Tracks @ 30 fps)

All 6 actions are exported as individual glTF animations (via NLA tracks) and verified on re-import into a fresh Blender scene. Locomotion loops are **in place** (`Root` stays fixed at origin) with exact analytical 3-bone leg IK keeping every planted foot locked on `z = 0.000` at the target ground speed (**zero foot sliding**).

| Action | Frames (30 fps) | Duration | Looping / Pose Notes | Speed & Stride Length |
| --- | --- | --- | --- | --- |
| `Idle` | `0–120` (120 frames) | **4.000 s** | Seamless loop (`f0 == f120`): breathing carapace, weight shift, scanning head, mandible twitch, probing antennae, tail sway, planted feet | Stationary (`0.0 m/s`) |
| `Walk` | `0–12` (12 frames) | **0.400 s** | Seamless loop (`f0 == f12`): metachronal leg wave (`β = 0.42`, `0.269 m` ground sweep) + subtle S-wave spine undulation | **1.6 m/s**, stride **0.640 m / cycle** |
| `Run` | `0–8` (8 frames) | **0.267 s** | Seamless loop (`f0 == f8`): fast predatory ripple (`β = 0.25`, `0.280 m` ground sweep), crouched body, snaking spine, swept-back antennae | **4.2 m/s**, stride **1.120 m / cycle** |
| `Attack` | `0–27` (27 frames) | **0.900 s** | Starts & ends in `Idle` f0: anterior rear-up + wide mandible flare -> explosive forward lunge with pincers snapping shut -> recovery | Melee lunge strike |
| `Hit` | `0–12` (12 frames) | **0.400 s** | Starts & ends in `Idle` f0: sharp upward-backward impact recoil rippling down spine, defensive jaw flare, crisp recovery | Flinch / stagger |
| `Death` | `0–45` (45 frames) | **1.500 s** | Starts in `Idle` f0: lethal spasm -> heavy belly-down slumped collapse onto `z = 0` (`min_z = +0.0001 m`, final `min_z = +0.002 m`), held still on final frames | Grounded collapse (zero floor clipping) |

---

## Previews & Verification

All previews were rendered from **re-imported `Spelopede.glb`** in a fresh factory-startup Blender 5.2 scene using Cycles (OpenImageDenoise, neutral grey studio stage, soft 3-point + sky lighting):

- `preview_front.png` — Front (`-Y` face) view (1600×1200)
- `preview_side.png` — Side profile view (1600×1200)
- `preview_34.png` — Three-quarter hero view from above-front (1600×1200)
- `preview_close.png` — Macro close-up of head, serrated mandibles, recessed glossy amber ocelli, thorax plates, pores & scratches (1600×1200)
- `preview_scale.png` — Scale reference beside a 1.80 m plain grey human silhouette (1600×1200)
- `contact_sheet.jpg` — 2×3 studio grid (2400×1200, JPEG quality 85)
- `preview_anim.gif` — 800×600 three-quarter animated showcase of all 6 actions sequentially with corner labels

---

## Rebuilding the Asset

All procedural build scripts are kept in this folder:

- `build_geometry.py` — Sculpted mesh, non-overlapping UV0, 67-bone Armature, skinning, float32 utility bakes
- `generate_textures.py` — Procedural 2048×2048 PBR texture synthesis (NumPy + Pillow)
- `animate_and_export.py` — Unified PBR material (`M_Spelopede`), analytical 3-bone leg IK animations, NLA tracks, glTF export
- `verify_and_render.py` — Fresh-scene GLB re-import verification & Cycles preview rendering
- `compose_outputs.py` — Contact sheet & `ffmpeg` labelled animation GIF composer
- `build_spelopede.py` — Master end-to-end runner (`python build_spelopede.py`)

---

## Notes / Unfinished Items

Nothing unfinished. Every requirement in `BRIEF.md` has been implemented, verified via fresh GLB re-import, visually critiqued across two full build-and-render passes, and polished.
