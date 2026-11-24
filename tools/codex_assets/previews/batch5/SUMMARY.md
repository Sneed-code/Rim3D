# Batch 5 previews (temporary)

Extracted from each model's README; full READMEs are alongside as <Name>_README.md.

## Mech_Scyther

| **Final Dimensions (Rest Pose)** | `0.875 m (X width) × 1.011 m (Y depth) × 2.086 m (Z height)` *(head crown ~1.94 m, antenna tip 2.086 m, feet flat on `z = 0.000 m`)* |
| **Triangle Count** | `23,864` triangles *(budget: 25,000 tris; 0 n-gons)* |
| **Rig / Skinning** | `38` bones, max `2` bone influences per vertex *(limit: 4)*, zero unweighted vertices, zero runtime constraints/drivers/shape keys |
**Axis & Orientation Convention**

- **Blender build space**: `+Z` up, **`-Y` forward** (direction the mech faces), `+X` = mech's Left (`_L`), `-X` = mech's Right (`_R`). Origin `(0, 0, 0)` and `Root` bone sit on the ground directly beneath the body centre; feet rest cleanly on `z = 0.000 m`.
- **glTF 2.0 export space** (`Mech_Scyther.glb`, default Blender glTF exporter with `export_yup = True`): `+Y` up, **`+Z` forward**, metres, scale `1.0`.

**Skeleton Hierarchy & Pivots (`38` Bones)**

All armour plates, joints, pistons, and blades use rigid 100% single-bone skinning; flexible braided waist, neck, and shoulder/knee hoses use smooth 2-bone blending. All 6 hydraulic piston pairs (`WaistPiston`, `KneePiston`, `AnklePiston`, L/R) telescope analytically and are baked directly to bone keyframes with zero runtime constraints.

- `Root` `(0.000, 0.000, 0.000)` — ground origin under body centre (stationary for in-place locomotion)
  - `Pelvis` `(0.000, 0.020, 1.040)` — hip saddle & gyro chassis
    - `Spine_Lower` `(0.000, 0.015, 1.120)` — lower segmented mechanical waist
      - `Spine_Upper` `(0.000, 0.000, 1.270)` — upper waist & brass gyro ring
        - `Chest` `(0.000, -0.015, 1.400)` — layered sternum/pectoral carapace, recessed vents, dorsal radiator pack
          - `Neck` `(0.000, -0.045, 1.670)` — articulated ringed hydraulic neck
            - `Head` `(0.000, -0.095, 1.790)` — stealth sensor head & cold blue optical slit
              - `Antenna` `(0.052, -0.045, 1.905)` — short tactical comms whip
          - `Shoulder_L` / `Shoulder_R` `(±0.195, -0.025, 1.600)` — shoulder gimbal & layered pauldron
            - `UpperArm_L` / `UpperArm_R` `(±0.315, -0.020, 1.575)` — humerus frame & bicep piston
              - `Forearm_L` / `Forearm_R` `(±0.375, 0.055, 1.255)` — elbow drum & blade-mount gauntlet
                - `Blade_L` / `Blade_R` `(±0.385, -0.170, 1.020)` — ~0.70 m curved mirror-steel scythe blade with chipped edge & parrying spur
          - `WaistPistonRod_L` / `WaistPistonRod_R` `(±0.118, -0.010, 1.385)` — upper waist hydraulic rod
    - `WaistPistonCyl_L` / `WaistPistonCyl_R` `(±0.095, 0.020, 1.085)` — lower waist hydraulic barrel
    - `Thigh_L` / `Thigh_R` `(±0.165, 0.015, 1.035)` — upper digitigrade thigh & forward knee shield
      - `KneePistonCyl_L` / `KneePistonCyl_R` `(±0.168, 0.035, 0.775)` — upper shin hydraulic barrel
      - `Shin_L` / `Shin_R` `(±0.185, -0.155, 0.665)` — backward-angled shin & hock spur
        - `KneePistonRod_L` / `KneePistonRod_R` `(±0.172, 0.162, 0.395)` — lower shin hydraulic rod
        - `AnklePistonCyl_L` / `AnklePistonCyl_R` `(±0.178, 0.145, 0.365)` — upper ankle hydraulic barrel
        - `Metatarsal_L` / `Metatarsal_R` `(±0.178, 0.110, 0.345)` — lower forward-angled digitigrade strut
          - `AnklePistonRod_L` / `AnklePistonRod_R` `(±0.172, 0.055, 0.088)` — lower ankle hydraulic rod
          - `Foot_L` / `Foot_R` `(±0.172, -0.020, 0.062)` — ankle drum, arch pad, rear heel spur
            - `Toe_L` / `Toe_R` `(±0.172, -0.105, 0.032)` — splayed articulated front raptor claws

**Animations (30 fps, Exported via NLA Tracks)**

All 7 animations were verified by re-importing `Mech_Scyther.glb` into a fresh Blender 5.2 scene.

| Action | Frames | Length | Loop | Notes |
| --- | --- | --- | --- | --- |
| `Idle` | `0–120` | `4.00 s` | Yes | Hydraulic breathing sway, weight shift, alert predator head scan, subtle blade & antenna motion |
| `Walk` | `0–30` | `1.00 s` | Yes | In place, **1.6 m/s**, stride length **`1.60 m` / cycle**; planted foot translates rearward at exact `1.60 m/s` on `z = 0` with zero sliding |
| `Run` | `0–18` | `0.60 s` | Yes | In place, **4.7 m/s**, stride length **`2.82 m` / cycle**; predatory forward-leaning sprint with arms & blades swept back and ballistic flight phase |
| `Charge` | `0–18` | `0.60 s` | No | Deep crouch-and-coil starting from `Idle` frame 0 and launching seamlessly into `Run` frame 0 |
| `Attack` | `0–27` | `0.90 s` | No | Fast two-blade slash (right blade cross-slash stepping in, then left blade follow-through), starts and ends in `Idle` pose |
| `Hit` | `0–12` | `0.40 s` | No | Sharp kinetic stagger/flinch backward with damped servo recovery back to `Idle` pose |
| `Death` | `0–45` | `1.50 s` | No | Servo-failure knee buckle and collapse onto the ground (`min_z = 0.003 m` across all frames; zero floor clipping), held still on final frames `39–45` |

- **Self-critique pass completed**: Pass 1 revealed overly broad orange rust triggered by AO thresholds, fragmented UV island margins (16.2% coverage), an overly flat upper chest core, slightly tight camera framing at the antenna tip, and a slightly elevated upper blade on Death settle. Pass 2 recalibrated seam-localized dark oxide rust so dark forged gunmetal and faded olive-grey armour read accurately, switched to Smart UV Project + island packing for sharper micro-detail, sculpted forward-projecting layered chest armour + visible radiator vents + shoulder yokes, tucked the knee pistons neatly behind the thighs, improved the grounded `Death` collapse, and widened all studio camera framing.

## Mech_Lancer

**Axis Convention**

- **Blender Scene**: `+Z` up, metres, origin and `Root` bone at ground centre `(0, 0, 0)` with feet flat on `Z = 0`. Character faces **`-Y`** (mech's right/charge-lance side toward `-X`, mech's left/manipulator side toward `+X`).
- **glTF Export (`Mech_Lancer.glb`)**: Exported with Blender's default glTF `+Y` up conversion (`export_yup=True`), so the character stands `+Y` up and faces **`+Z` forward** in glTF space.
- **`Muzzle` Bone**: Positioned at the exact Charge Lance muzzle tip, pointing straight out of the barrel along its bone `+Y` (head-to-tail) firing axis.

**Skeleton & Bone Hierarchy (28 Bones)**

All hard-surface armor plates and limb segments use clean rigid 100% single-bone skinning; flexible waist hydraulic cables blend smoothly across adjacent spine bones (max 2 influences).

| Bone | Parent | Pivot / Role |
| :--- | :--- | :--- |
| `Root` | *(none)* | Ground origin `(0, 0, 0)` under body centre (stationary in `Walk`/`Run`) |
| `Pelvis` | `Root` | Hip/pelvic core pivot `(0.000, 0.015, 1.120)` |
| `Spine_Lower` | `Pelvis` | Lower mechanical abdomen & exposed waist pistons `(0.000, 0.010, 1.250)` |
| `Spine_Upper` | `Spine_Lower` | Chest carapace & structural shoulder yoke `(0.000, 0.002, 1.460)` |
| `Backpack` | `Spine_Upper` | Rear power cell, radiator cooling fins & glowing core window `(0.000, 0.135, 1.630)` |
| `Neck` | `Spine_Upper` | Articulated mechanical neck & hydraulic tilt struts `(0.000, -0.008, 1.780)` |
| `Head` | `Neck` | Elongated helmet-like head & horizontal violet-white sensor bar `(0.000, -0.022, 1.895)` |
| `Thigh_L` / `Thigh_R` | `Pelvis` | Hip joints `(±0.145, 0.012, 1.115)` |
| `Shin_L` / `Shin_R` | `Thigh_L` / `Thigh_R` | Knee drum joints `(±0.158, -0.062, 0.615)` |
| `Foot_L` / `Foot_R` | `Shin_L` / `Shin_R` | Ankle joints `(±0.168, 0.032, 0.115)` |
| `Toe_L` / `Toe_R` | `Foot_L` / `Foot_R` | Splayed twin armored front toe hinges `(±0.168, -0.095, 0.038)` |
| `Shoulder_R` | `Spine_Upper` | Right clavicle / pauldron pivot `(-0.135, 0.000, 1.715)` |
| `UpperArm_R` | `Shoulder_R` | Right shoulder drum joint `(-0.235, -0.005, 1.705)` |
| `Forearm_R` | `UpperArm_R` | Right elbow drum joint `(-0.245, -0.055, 1.355)` |
| `Lance` | `Forearm_R` | Integrated Charge Lance receiver, barrel, rail spars & accelerator coils |
| `Muzzle` | `Lance` | Lance muzzle tip socket, bone `+Y` pointing out of barrel |
| `MuzzleFlare` | `Muzzle` | Emissive muzzle energy core (scales radially/axially during `Shoot` coil flare) |
| `Shoulder_L` | `Spine_Upper` | Left clavicle / pauldron pivot `(0.135, 0.000, 1.715)` |
| `UpperArm_L` | `Shoulder_L` | Left shoulder drum joint `(0.235, -0.025, 1.705)` |
| `Forearm_L` | `UpperArm_L` | Left elbow drum joint |
| `Hand_L` | `Forearm_L` | Left wrist pivot & manipulator palm steadying the lance foregrip |
| `Finger1_L`, `Finger2_L`, `Thumb_L` | `Hand_L` | Articulated three-fingered manipulator digits |

**Animations (8 Actions, 30 fps, NLA Tracks -> glTF)**

Verified by re-importing `Mech_Lancer.glb` into a fresh 30 fps Blender scene.

| Action | Frames (30 fps) | Duration | Type | Ground Speed / Stride | Notes |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `Idle` | `1–121` (120 steps) | `4.000 s` | Seamless loop | — | Servo breathing, weight shift, alert horizontal head sensor scan, feet locked on `Z = 0` |
| `Walk` | `1–37` (36 steps) | `1.200 s` | Seamless loop (in-place) | **1.50 m/s** / **1.800 m per cycle** | Constant-velocity ground stance (zero foot slide), patrol lance carry with left hand locked to foregrip |
| `Run` | `1–21` (20 steps) | `0.667 s` | Seamless loop (in-place) | **4.00 m/s** / **2.667 m per cycle** | Athletic forward sprint, zero foot slide during 34% stance phase, Charge Lance held diagonally across body |
| `Attack` | `1–28` (27 steps) | `0.900 s` | One-shot (Idle -> Idle) | — | Wind-up and forceful forward rifle-butt / forearm melee bash, returning cleanly to `Idle` |
| `Hit` | `1–13` (12 steps) | `0.400 s` | One-shot (Idle -> Idle) | — | Sharp ballistic torso/head flinch and servo recovery back to `Idle` |
| `Death` | `1–46` (45 steps) | `1.500 s` | One-shot (held at end) | — | Power-down collapse flat onto `Z = 0` (verified all geometry ≥ `0.000 m`, Charge Lance & body grounded), held still on final frames |
| `Aim` | `1–31` (30 steps) | `1.000 s` | Seamless loop | — | Bladed marksman stance, Charge Lance levelled straight forward along `-Y`, left manipulator bracing foregrip, subtle servo micro-corrections |
| `Shoot` | `1–16` (15 steps) | `0.500 s` | One-shot (Aim -> Aim) | — | Brief charge settle, recoil kick of lance/arm/torso + bright `MuzzleFlare` scale pulse, hydraulic recovery back to `Aim` |

**1. Build mesh, rig, UV0, 2048x2048 PBR textures, 8 NLA animations and export Mech_Lancer.glb**
& "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" -b --factory-startup --python-exit-code 1 -P build_mech_lancer.py

**2. Re-import Mech_Lancer.glb into a fresh scene, verify rig/actions, and render studio previews + anim frames**
& "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" -b --factory-startup --python-exit-code 1 -P render_previews.py

**3. Assemble contact_sheet.jpg and preview_anim.gif (requires Python with Pillow)**
python assemble_media.py
```


## Mech_Pikeman

**Axis Convention**
- **Blender Source**: `+Z` up, metres, origin and `Root` bone at `(0, 0, 0)` on the ground under the body centre, all four flat clawed feet resting flush on `z = 0.000`, facing **`-Y`** forward (`+X` = mech's Left, `-X` = mech's Right).
- **glTF Export (`Mech_Pikeman.glb`)**: Exported with Blender's default glTF 2.0 `+Y` up setting (`export_yup=True`), making the mech stand `+Y` up and face **`+Z` forward** in glTF space.

**Armature & Bone Hierarchy (25 Bones)**
All pivots listed in Blender rest-pose coordinates `(X, Y, Z)` in metres:

| Bone | Parent | Pivot `(X, Y, Z)` m | Role |
|---|---|---|---|
| `Root` | *(none)* | `(0.000, 0.000, 0.000)` | Ground root under body centre (stays in place for locomotion) |
| `Body` | `Root` | `(0.000, 0.000, 0.800)` | Armoured hull chassis & rear powerpack |
| `SensorHead` | `Body` | `(0.000, -0.580, 0.780)` | 3-eye sensor gimbal under overhanging brow plate |
| `Antenna_L` | `Body` | `(+0.270, +0.440, 1.030)` | Rear-left flexible comms whip antenna |
| `Antenna_R` | `Body` | `(-0.270, +0.440, 1.030)` | Rear-right telemetry mast & beacon |
| `Turret` | `Body` | `(0.000, -0.020, 1.000)` | Top remote weapon turret (yaw) |
| `Gun` | `Turret` | `(0.000, -0.100, 1.150)` | Railgun receiver & breech cradle (pitch) |
| `Barrel` | `Gun` | `(0.000, -0.220, 1.150)` | 1.40 m needle railgun barrel assembly (recoil slide) |
| `Muzzle` | `Barrel` | `(0.000, -1.620, 1.150)` | Barrel tip VFX/projectile socket |
| `Leg_FL_Hip` | `Body` | `(+0.360, -0.340, 0.720)` | Front-left coxa (yaw) |
| `Leg_FL_Upper` | `Leg_FL_Hip` | `(+0.477, -0.449, 0.720)` | Front-left femur (pitch) |
| `Leg_FL_Lower` | `Leg_FL_Upper` | `(+0.890, -0.833, 0.977)` | Front-left tibia/shin (knee pitch) |
| `Leg_FL_Foot` | `Leg_FL_Lower` | `(+0.940, -0.880, 0.110)` | Front-left flat clawed foot (ankle pitch, sole on `z=0`) |
| `Leg_FR_Hip` | `Body` | `(-0.360, -0.340, 0.720)` | Front-right coxa (yaw) |
| `Leg_FR_Upper` | `Leg_FR_Hip` | `(-0.477, -0.449, 0.720)` | Front-right femur (pitch) |
| `Leg_FR_Lower` | `Leg_FR_Upper` | `(-0.890, -0.833, 0.977)` | Front-right tibia/shin (knee pitch) |
| `Leg_FR_Foot` | `Leg_FR_Lower` | `(-0.940, -0.880, 0.110)` | Front-right flat clawed foot (ankle pitch, sole on `z=0`) |
| `Leg_RL_Hip` | `Body` | `(+0.360, +0.340, 0.720)` | Rear-left coxa (yaw) |
| `Leg_RL_Upper` | `Leg_RL_Hip` | `(+0.477, +0.449, 0.720)` | Rear-left femur (pitch) |
| `Leg_RL_Lower` | `Leg_RL_Upper` | `(+0.890, +0.833, 0.977)` | Rear-left tibia/shin (knee pitch) |
| `Leg_RL_Foot` | `Leg_RL_Lower` | `(+0.940, +0.880, 0.110)` | Rear-left flat clawed foot (ankle pitch, sole on `z=0`) |
| `Leg_RR_Hip` | `Body` | `(-0.360, +0.340, 0.720)` | Rear-right coxa (yaw) |
| `Leg_RR_Upper` | `Leg_RR_Hip` | `(-0.477, +0.449, 0.720)` | Rear-right femur (pitch) |
| `Leg_RR_Lower` | `Leg_RR_Upper` | `(-0.890, +0.833, 0.977)` | Rear-right tibia/shin (knee pitch) |
| `Leg_RR_Foot` | `Leg_RR_Lower` | `(-0.940, +0.880, 0.110)` | Rear-right flat clawed foot (ankle pitch, sole on `z=0`) |

**Animations (8 Actions, 30 fps, Exported via NLA Tracks)**
All actions are baked via analytical 3D spider-leg IK and verified on re-import of `Mech_Pikeman.glb` into a fresh 30 fps Blender scene:

| Action | Frames (30 fps) | Duration | Type | Notes / Speed & Stride Length |
|---|---|---|---|---|
| `Idle` | `0–120` | `4.00 s` | Seamless loop | Hydraulic suspension sway, sensor cluster scan, subtle turret sentry sweep, weight shift. |
| `Walk` | `0–24` | `0.80 s` | Seamless loop (in place) | Diagonal scuttling gait (`FL+RR` / `FR+RL`), **1.30 m/s**, **stride length = `1.04 m` / cycle**, zero foot sliding (`z = 0` stance lock). |
| `Run` | `0–12` | `0.40 s` | Seamless loop (in place) | Fast aggressive diagonal scuttling gait, **3.20 m/s**, **stride length = `1.28 m` / cycle**, crouched forward pitch, gyro-stabilized turret. |
| `Attack` | `0–27` | `0.90 s` | One-shot (Idle → Idle) | Pneumatic melee stab with front-left leg (`Leg_FL`) while bracing weight on the other three legs. |
| `Hit` | `0–12` | `0.40 s` | One-shot (Idle → Idle) | Sharp ballistic impact flinch & shock-absorber compression, recovering to Idle. |
| `Death` | `0–45` | `1.50 s` | One-shot (held at frame 45) | Power-loss stagger and collapse onto `z = 0.000` (verified `min_z = 0.000 m`, nothing below ground), held motionless on final frames. |
| `Shoot` | `0–18` | `0.60 s` | One-shot (Idle → Idle) | Railgun discharge with `Barrel` recoiling `0.20 m` backward along the gun axis and the hull rocking back to brace against all 4 legs. |
| `Aim` | `0–30` | `1.00 s` | Seamless loop | Legs braced wide and low on `z = 0`, gun levelled horizontally with fine servo tracking micro-corrections. |


## Mech_Centipede

| **Final Size (X, Y, Z)** | **1.837 m × 4.170 m × 1.419 m** (width × length × height) |
| **Rest Bounds (Blender)** | `X: [-0.919, +0.919] m`, `Y: [-2.100, +2.070] m`, `Z: [0.000, +1.419] m` |
| **Triangle Count** | **30,466 tris** (16,191 verts; budget: 40,000 tris; 0 n-gons; consistent outward normals) |
**Axis Conventions**

- **In Blender**: `+Z` up, **`-Y` forward** (head & cannon face `-Y`), `+X` = crawler's left (`_L`), `-X` = crawler's right (`_R`). Origin `(0, 0, 0)` and `Root` bone sit on the ground under the body centre; all 12 stamping feet rest flat on `z = 0.000 m`.
- **In exported glTF (`Mech_Centipede.glb`)**: Blender's default glTF exporter converts `-Y` forward / `+Z` up to glTF's **`+Z` forward / `+Y` up** right-handed metre space.

**Skeleton Hierarchy (51 Bones)**

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

**Animations (All 8 Actions @ 30 fps, NLA Tracks)**

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


## Megaspider

| **Dimensions (Rest Pose)** | **X (width):** `2.025 m` · **Y (length):** `2.362 m` · **Z (height):** `1.137 m` |
**Axis Convention & Facing Direction**

- **In Blender (source scene):** `+Z` up, **`-Y` forward** (head and mandibles face `-Y`, abdomen extends toward `+Y`), Creature Left = `+X`, Creature Right = `-X`. Origin `(0, 0, 0)` and `Root` bone sit on the ground (`z = 0`) directly beneath the body centre.
- **In `Megaspider.glb` (standard glTF 2.0 export):** Blender's default glTF `+Y` up conversion maps the creature to **`+Y` up** and **`+Z` forward** (right-handed glTF standard, ready for Unreal Engine 5.8 import).

---

**Skeleton & Bone Hierarchy (35 Bones)**

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

**Animations (30 fps, Exported via NLA Tracks)**

All six animations were verified on a clean fresh-scene re-import of `Megaspider.glb`. Locomotion (`Walk`, `Run`) uses an alternating tripod gait (`{F_L, M_R, B_L}` / `{F_R, M_L, B_R}`) solved analytically in world space so planted feet move backward at exact ground speed with zero sliding.

| Action | Frames (30 fps) | Duration | Loop | Stride / Motion Notes |
|---|---|---|---|---|
| **`Idle`** | `0 – 120` | `4.00 s` | Yes (`f0 == f120`) | Heavy abdomen/thorax breathing, weight shift with planted feet, predatory head scan, mandible grind & pedipalp probing |
| **`Walk`** | `0 – 18` | `0.60 s` | Yes (`f0 == f18`) | In-place alternating tripod gait at **`1.4 m/s`** -> **Stride length: `0.84 m / cycle`** (55% stance duty, verified stance foot speed `1.42 m/s`) |
| **`Run`** | `0 – 12` | `0.40 s` | Yes (`f0 == f12`) | In-place fast alternating tripod charge at **`3.8 m/s`** -> **Stride length: `1.52 m / cycle`** (42% stance duty, verified stance foot speed `3.91 m/s`) |
| **`Attack`** | `0 – 27` | `0.90 s` | No (Idle -> Idle) | Anticipation crouch, dramatic high rear-up spreading front legs & mandibles wide, explosive downward front-leg slash & snapping mandible clamp, recovery to Idle |
| **`Hit`** | `0 – 12` | `0.40 s` | No (Idle -> Idle) | Sharp recoil flinch back and to the side with mandible flare and front-leg stagger, recovery to Idle |
| **`Death`** | `0 – 45` | `1.50 s` | No (held at `f45`) | Shock convulsion followed by heavy collapse onto `z = 0` with classic arachnid leg curl, final twitch, and lifeless hold (verified last-frame mesh `min_z = +0.013 m`, nothing below ground) |


## Spelopede

| **Dimensions (Rest Pose, Blender)** | **1.043 m (X width) × 1.624 m (Y length) × 0.564 m (Z height)** |
| **Triangle Count** | **16,700 triangles** (budget: 20,000; `0` n-gons; consistent outward normals) |
| **Rig / Skeleton** | **67 bones** (limit: ~80), max **2** bone influences per vertex (limit: 4) |
**Axis Convention & Pivot**

- **Blender authoring space**: `+Z` up, `-Y` forward (head & mandibles face `-Y`, tail points `+Y`), real-world metres, applied transforms (`scale = 1`).
- **glTF exported space (`Spelopede.glb`)**: Standard glTF `+Y` up, `+Z` forward (default Blender glTF exporter conversion with `export_yup=True`).
- **Origin & Root Pivot**: Origin `(0, 0, 0)` and `Root` bone sit on the ground plane (`z = 0.000`) directly beneath the creature's body centre, with all 16 tarsal claws resting squarely on `z = 0.000`.

---

**Skeleton & Bone Hierarchy (67 Bones)**

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

**Animations (6 NLA Tracks @ 30 fps)**

All 6 actions are exported as individual glTF animations (via NLA tracks) and verified on re-import into a fresh Blender scene. Locomotion loops are **in place** (`Root` stays fixed at origin) with exact analytical 3-bone leg IK keeping every planted foot locked on `z = 0.000` at the target ground speed (**zero foot sliding**).

| Action | Frames (30 fps) | Duration | Looping / Pose Notes | Speed & Stride Length |
| --- | --- | --- | --- | --- |
| `Idle` | `0–120` (120 frames) | **4.000 s** | Seamless loop (`f0 == f120`): breathing carapace, weight shift, scanning head, mandible twitch, probing antennae, tail sway, planted feet | Stationary (`0.0 m/s`) |
| `Walk` | `0–12` (12 frames) | **0.400 s** | Seamless loop (`f0 == f12`): metachronal leg wave (`β = 0.42`, `0.269 m` ground sweep) + subtle S-wave spine undulation | **1.6 m/s**, stride **0.640 m / cycle** |
| `Run` | `0–8` (8 frames) | **0.267 s** | Seamless loop (`f0 == f8`): fast predatory ripple (`β = 0.25`, `0.280 m` ground sweep), crouched body, snaking spine, swept-back antennae | **4.2 m/s**, stride **1.120 m / cycle** |
| `Attack` | `0–27` (27 frames) | **0.900 s** | Starts & ends in `Idle` f0: anterior rear-up + wide mandible flare -> explosive forward lunge with pincers snapping shut -> recovery | Melee lunge strike |
| `Hit` | `0–12` (12 frames) | **0.400 s** | Starts & ends in `Idle` f0: sharp upward-backward impact recoil rippling down spine, defensive jaw flare, crisp recovery | Flinch / stagger |
| `Death` | `0–45` (45 frames) | **1.500 s** | Starts in `Idle` f0: lethal spasm -> heavy belly-down slumped collapse onto `z = 0` (`min_z = +0.0001 m`, final `min_z = +0.002 m`), held still on final frames | Grounded collapse (zero floor clipping) |


## ButcherTable

| Overall Blender bounds (X × Y × Z) | 1.8996 × 0.9000 × 1.3777 m |
| Triangle count | 7,928 |
**Parts and axes**


Blender uses metres, +Z up, and −Y toward the working side. Blender's default glTF export writes metres with +Y up and +Z toward the working side. The scale preview places a 1.80 m gray figure beside the table.


## StonecutterTable

| Overall size (Blender X × Y × Z) | 2.900 × 0.900 × 1.276 m |
| Triangle count | 7,637 of 8,000 |
The overall height includes the stack of finished blocks. The model uses metres and applied unit scale. In Blender, +Z is up, the working side faces −Y, and the footprint is centred at the origin with the feet at Z = 0. Blender's default glTF exporter converts this to +Y up. The working side faces +Z in the GLB.
| Mesh | Triangles | Pivot in Blender (X, Y, Z), metres | Use |
| --- | ---: | --- | --- |
| `Body` | 6,617 | (0, 0, 0) | Fixed bench, saw housing, and accessories |
| `SawBlade` | 1,020 | (0.885, −0.042, 0.912) | Blade and arbor hardware. Rotate around local Y in Blender, or local Z in the GLB. |


## MachiningTable

Blender uses metres and +Z up. The working side faces −Y. The centre of the footprint is at the origin and the feet meet Z = 0. The GLB uses the Blender exporter’s default +Y-up conversion. In glTF the working side faces +Z.
| Mesh object | Triangles | Pivot in Blender metres |
| --- | ---: | --- |
| `Body` | 8,617 | `(0, 0, 0)` at the footprint centre on the ground plane |
| `LatheChuck` | 414 | `(−0.978, 0.106, 1.247)` on the spindle. Rotate about local X. |
| **Total** | **9,031** | |


## TailorBenchHand

| Blender dimensions in X × Y × Z | 2.900 × 0.900 × 1.390 m |
| Triangle count | 7,403 of 8,000 |
| Mesh | Triangles | Pivot and purpose |
| --- | ---: | --- |
| `Body` | 6,871 | Ground at footprint centre `(0, 0, 0)`. The frame, treadle, belt, lower flywheel and workbench contents form one static mesh. |
| `HandWheel` | 532 | Axle at `(-0.419, -0.112, 1.238)` m in Blender. Rotate around local X. The wheel, crank and grip move together. |

Blender uses metres with +Z up and the working side facing −Y. The default Blender glTF export uses metres with +Y up and the working side facing +Z. The wheel pivot in GLB coordinates is `(-0.419, 1.238, 0.112)` m. Both objects have unit scale.

## FueledStove

| Blender dimensions (X × Y × Z) | 2.900 × 0.900 × 1.468 m |
| Origin and transforms | Footprint center at floor level. Body scale 1, rotation 0. |
| Name | Type | Pivot or placement in Blender XYZ (m) |
| --- | --- | --- |
| `Body` | Static mesh | `(0, 0, 0)` at the footprint center on the floor |
| `Flame` | Empty | `(-0.805, -0.351, 0.490)` at the center of the firebox. The same point is `(-0.805, 0.490, 0.351)` in glTF XYZ. |


## ElectricStove

| Overall size (Blender X × Y × Z) | 2.900 × 0.900 × 1.248 m |
| Triangle count | 7,983 |
**Mesh and pivot**

`Body` is the single static mesh. It includes the oven and racks, hardware, counter and sink, cookware, food, utensils, and rear cable. The pivot is `(0, 0, 0)` at the footprint centre on the floor. The object transform is identity and one unit equals one metre.

Blender uses +Z up, -Y toward the working side, and +X along the width toward the sink. Blender’s default glTF exporter converts this to +Y up and +Z toward the working side in the GLB.


## HospitalBed

| Bounds in Blender XYZ | 0.950 m wide × 2.000 m long × 1.796 m tall |
| Triangles | 9,980 total · 6,788 Frame · 3,192 Bedding |
Blender uses metres, +Z up, +X across the bed, +Y toward the head and −Y toward the working foot end. The footprint is centred on the origin and the castors touch Z = 0. Blender’s default glTF export uses +Y up, with +Z toward the foot.

## ChessTable

| Overall size (Blender X × Y × Z) | 0.940 × 1.940 × 0.835 m, including pieces |
| Triangles | 5,648 |
The Blender scene uses metres with +Z up and the front short end facing −Y. The footprint is centred on the origin and the steel feet rest at Z = 0. Blender’s default glTF conversion exports +Y up and points the front toward glTF +Z.

## BilliardsTable

| Triangles | 9,896 |

## Battery

Battery is a weathered frontier power-storage cabinet for Rim3D. Its working end has a five-segment charge display and two service panels. The side louvres cover three numbered cell tiers. Recessed copper terminals sit in the top tray, and a corrugated conduit joins glands on the rear bulkhead.
| Blender bounds (X × Y × Z) | **0.940 × 1.940 × 0.805 m** |
| Triangles | **6,490** of 8,000 |
The Blender scene uses metres, +Z up and −Y toward the working end. The footprint is centred at X = Y = 0 and the skid shoes sit at Z = 0. Blender’s default glTF export writes +Y up and +Z toward the working end, using the conversion `(x, y, z) → (x, z, −y)`. Both parts have unit scale.
| Mesh | Triangles | Pivot in Blender metres (X, Y, Z) | Function |
| --- | ---: | --- | --- |
| `Body` | 5,960 | `(0, 0, 0)` | Fixed cabinet, service panels, vent banks, terminals and conduit |
| `ChargeLamps` | 530 | `(0.023000, −0.945430, 0.606174)` | Centre of the five-lens bank, aligned with the middle lens |


## Heater

| Size in Blender (X × Y × Z) | 0.9559 × 0.7624 × 1.1030 m, including handles and cable |
