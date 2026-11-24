# Mech_Centipede

Heavy armoured war machine of the Remnant (*Rim3D* frontier survival game, Unreal Engine 5.8). Built procedurally from scratch in Blender 5.2 + Python/NumPy/Pillow, rigged to a 51-bone skeleton, textured with a single 2048×2048 PBR material set, animated with 8 game-ready 30 fps NLA actions, and verified via clean GLB re-import.

## Asset Specifications

| Property | Value |
|---|---|
| **Final Size (X, Y, Z)** | **1.837 m × 4.170 m × 1.419 m** (width × length × height) |
| **Rest Bounds (Blender)** | `X: [-0.919, +0.919] m`, `Y: [-2.100, +2.070] m`, `Z: [0.000, +1.419] m` |
| **Triangle Count** | **30,466 tris** (16,191 verts; budget: 40,000 tris; 0 n-gons; consistent outward normals) |
| **Skinned Mesh Object** | `Mech_Centipede` (1 mesh object, Armature modifier, max 2 bone influences/vert) |
| **Armature** | `Armature` (**51 bones**, zero runtime constraints/drivers/shape keys/IK bones) |
| **Materials** | 1 PBR Material (`M_Mech_Centipede`) |
| **UVs** | Clean non-overlapping `UV0` (`UVMap`) |
| **Export File** | `Mech_Centipede.glb` (glTF 2.0 binary, default Blender exporter +Y up, textures embedded) |

## Axis Conventions

- **In Blender**: `+Z` up, **`-Y` forward** (head & cannon face `-Y`), `+X` = crawler's left (`_L`), `-X` = crawler's right (`_R`). Origin `(0, 0, 0)` and `Root` bone sit on the ground under the body centre; all 12 stamping feet rest flat on `z = 0.000 m`.
- **In exported glTF (`Mech_Centipede.glb`)**: Blender's default glTF exporter converts `-Y` forward / `+Z` up to glTF's **`+Z` forward / `+Y` up** right-handed metre space.

## Texture Set (2048×2048 PNG, Embedded in GLB)

All textures are procedurally synthesized (`generate_textures.py`) from Cycles-baked geometric passes — zero downloaded textures, zero baked lighting or shadows in BaseColor:

| File | Colour Space | Contents |
|---|---|---|
| `Mech_Centipede_BaseColor.png` | sRGB | Pure physical albedo: dark rust-red & charcoal-black riveted armour plates, metallic chipped bevel edges, hazard chevrons, generic stencils, battle-damage scorch soot, glossy oil leaks, splattered tropical jungle mud, copper & coloured exposed wiring inside cracked flank plate. |
| `Mech_Centipede_Normal.png` | Non-Color (OpenGL `+Y`) | Raised domed rivets, recessed panel seams, weld bead ripples, cast-iron micro-pitting, paint-chip height steps, ribbed rubber bellows, lathe-turned gunmetal, granular mud bump. |
| `Mech_Centipede_ORM.png` | Non-Color (Linear) | Channel-packed: **R** = Raytraced Cycles Ambient Occlusion, **G** = Roughness (matte clay mud, worn paint, wet glossy oil streaks, mirror chrome pistons), **B** = Metallic. |
| `Mech_Centipede_Emissive.png` | sRGB | Fiery amber-orange recessed head sensor visor & optics, incandescent plasma/heat cannon chamber & flare core, and diagnostic amber/green status LEDs. |

## Skeleton Hierarchy (51 Bones)

- `Root` — Ground origin `(0.00, 0.00, 0.00)` (remains fixed at origin for in-place locomotion)
  - `Body` — Central spine hub `(0.00, 0.00, 0.56)`
    - `Seg3` — Mid-front body segment `(0.00, 0.00, 0.56)` *(houses left-flank cracked armour plate & exposed wiring)*
      - `Leg3_L_Upper` → `Leg3_L_Lower` → `Leg3_L_Foot`
      - `Leg3_R_Upper` → `Leg3_R_Lower` → `Leg3_R_Foot`
      - `Seg2` — Weapon-platform thorax segment `(0.00, -0.50, 0.56)`
        - `Gun` — Heavy twin-barrelled plasma cannon yaw/pitch gimbal pivot `(0.00, -0.50, 1.18)`
          - `Gun_Recoil` — Twin cooling-finned barrel recoil slide `(0.00, -0.93, 1.18)`
          - `Gun_Flare` — Internal plasma chamber flare core `(0.00, -0.76, 1.18)`
          - `Muzzle` — Projectile / muzzle-flash socket `(0.00, -1.34, 1.18)`
        - `Leg2_L_Upper` → `Leg2_L_Lower` → `Leg2_L_Foot`
        - `Leg2_R_Upper` → `Leg2_R_Lower` → `Leg2_R_Foot`
        - `Seg1` — Forward body segment `(0.00, -1.00, 0.56)`
          - `Head` — Heavy armoured head & sensor visor `(0.00, -1.45, 0.56)`
            - `Mandible_L` — Left articulated armoured pincer jaw `(+0.30, -1.68, 0.44)`
            - `Mandible_R` — Right articulated armoured pincer jaw `(-0.30, -1.68, 0.44)`
          - `Leg1_L_Upper` → `Leg1_L_Lower` → `Leg1_L_Foot`
          - `Leg1_R_Upper` → `Leg1_R_Lower` → `Leg1_R_Foot`
      - `Seg4` — Mid-rear body segment `(0.00, +0.50, 0.56)` *(modelled shell impact dent on right shoulder)*
        - `Leg4_L_Upper` → `Leg4_L_Lower` → `Leg4_L_Foot`
        - `Leg4_R_Upper` → `Leg4_R_Lower` → `Leg4_R_Foot`
        - `Seg5` — Rear body segment `(0.00, +1.00, 0.56)`
          - `Leg5_L_Upper` → `Leg5_L_Lower` → `Leg5_L_Foot`
          - `Leg5_R_Upper` → `Leg5_R_Lower` → `Leg5_R_Foot`
          - `Seg6` — Rear power-pack & twin exhaust tail segment `(0.00, +1.50, 0.56)`
            - `Leg6_L_Upper` → `Leg6_L_Lower` → `Leg6_L_Foot`
            - `Leg6_R_Upper` → `Leg6_R_Lower` → `Leg6_R_Foot`

## Animations (All 8 Actions @ 30 fps, NLA Tracks)

All actions were verified by re-importing `Mech_Centipede.glb` into a fresh 30 fps Blender 5.2 scene.

| Action | Frames | Duration | Loop | Stride / Speed & Notes |
|---|---|---|---|---|
| `Idle` | `0–120` | `4.00 s` | Seamless | Breathing/hydraulic servo sway, head sensor scan, mandible twitch, gentle weight shift; feet locked on `z = 0`. |
| `Walk` | `0–24` | `0.80 s` | Seamless (in-place) | **0.80 m/s**, **0.64 m stride/cycle** (duty factor 0.58, 0.371 m stance travel). Metachronal tail-to-head leg ripple with exact analytical 2-bone IK foot lock (zero foot sliding) and subtle S-wave segment sway. |
| `Run` | `0–15` | `0.50 s` | Seamless (in-place) | **1.80 m/s**, **0.90 m stride/cycle** (duty factor 0.48, 0.432 m stance travel). Aggressive crouched charge, stronger body undulation, higher stamping leg lift, zero foot sliding. |
| `Attack` | `0–27` | `0.90 s` | One-shot (Idle → Idle) | Front thorax (`Seg2`, `Seg1`, `Head`) rears high with front legs lifting and mandibles spreading wide, then slams violently downward with mandibles snapping shut and shockwave recovery. |
| `Hit` | `0–12` | `0.40 s` | One-shot (Idle → Idle) | Sharp front-left directional impact flinch, head/turret whip, front-left leg stagger, fast recovery to Idle. |
| `Death` | `0–45` | `1.50 s` | One-shot (held at end) | Brief power-failure spasm followed by sequential leg buckle and heavy collapse onto `z = 0.000 m` (verified final frame `min Z = 0.0000 m`, zero ground penetration). |
| `Shoot` | `0–24` | `0.80 s` | One-shot (Idle → Idle) | Plasma chamber charges and flares incandescently inside the containment cage (`Gun_Flare`), twin barrels kick sharply back on hydraulic recoil rails (`Gun_Recoil`, 0.16 m stroke) with muzzle rise and hull recoil absorption. |
| `Aim` | `0–30` | `1.00 s` | Seamless | Braced low-crouch firing stance with `Gun` smoothly tracking left–right and micro-pitching forward (`-Y`) alongside head sensor scan. |

## Build & Reproduction Scripts

- `build_mech_centipede.py`: Full procedural modelling, UV unwrapping, Cycles GPU data-map baking, texture synthesis trigger, 51-bone rigging, analytical-IK animation baking, and GLB export.
  ```powershell
  blender -b --factory-startup -P build_mech_centipede.py
  ```
- `generate_textures.py`: 2048×2048 PBR texture synthesizer (NumPy + Pillow).
- `render_previews.py`: Re-imports `Mech_Centipede.glb` into a fresh Blender scene to verify rig/mesh/animations and renders `preview_front.png`, `preview_side.png`, `preview_34.png`, `preview_close.png`, `preview_scale.png`, `contact_sheet.jpg`, and `preview_anim.gif`.
  ```powershell
  blender -b --factory-startup -P render_previews.py
  ```

## Status / Notes

- All required deliverables (`Mech_Centipede.glb`, four 2048×2048 PBR PNGs, five 1600×1200 studio previews, `contact_sheet.jpg`, `preview_anim.gif`, build scripts, and `README.md`) are complete and verified.
- Completed two full visual self-critique & refinement passes (tightened pixel-accurate metallic edge chipping to preserve rich dark rust-red & charcoal-black armour, refined curved pincer mandibles and hollow segmented plasma muzzles, added 3D hexagonal bolts, bounded plasma chamber flare inside cage ribs, fixed pitch axes for `Attack`/`Shoot`/`Hit`/`Death`, welded mandible tips & brow bolts flush, and verified `Death` final frame `min Z = 0.0000 m`).
