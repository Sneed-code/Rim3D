# Soft body (flesh that wobbles, gives on hits and dents on contact)

sneed (2026-11-16) asked for the realistic, rubbery skin seen in some SFM renders: flesh that jiggles, gives way where
something presses on it, ripples from a blast, and is lit like real skin, only on pawns close to the camera. The
explainer with the options, costs and risks is the Claude Doc "Soft skin and flesh for pawns: can we do it?" (project
thread "Test soft-body skin physics on pawns"). Pure looks: nothing here touches the sim.

## The plan (three PC steps)

1. **Whole-body wobble and hit kicks** (this file's first part, built on the creator bodies).
2. **Squish on contact**: a Deformer Graph skin deformer on the graphics card that pushes the skin out of what touches
   it (the floor lying down, the seat sitting, hands, a held weapon, other bodies) with a soft falloff and springs it
   back, recomputing normals where it moves points. Only the nearest 8 pawns within 6 m.
3. **Clothes, beds and the budget**: worn clothes follow the same springs and deformer, beds and cushions dent, a
   "Soft body" setting (off / springs / full), a 40-pawn fight to measure the cost.

Chaos Flesh and the ML Deformer were ruled out: Chaos Flesh is an offline-grade simulation whose tetrahedral filling is
made for one fixed body (our sliders make a new body each time), and the ML Deformer is trained for one fixed body and
does not react to touch. Both may come back later for the protagonist only (muscle flex).

## Why it works on slider bodies

Every creator body is the same MakeHuman mesh moved by morphs, so soft bones and (in step 2) a softness map made once on
the base by script fit every variant. The springs and the deformer run after the morphs and the skeleton, on the final
shape. How soft a body is comes from its sliders at run time.

## Step 1: soft bones and kicks

`mh_common.add_body_soft_bones` (called by `mh_export.py` after `add_soft_bones`; `"body_soft_bones": false` in a
config skips it) adds, for any MakeHuman body, with no hand work:
- `belly`: 7 cm inside the fullest point of the belly between the pelvis and spine_02, parented to spine_01, weighted
  onto the front of the trunk within 17 cm (0.75 at the bone, falling off with the square, not below 12 cm under it);
- `thigh_soft_l/r` and `arm_soft_l/r`: at the middle of the thigh and upper arm, parented to it, weighted as a bell
  along the bone (nothing in the end 15 % at each joint, so knees, hips, shoulders and elbows stay put), thighs 0.55
  all round within 13 cm, upper arms 0.35 plus up to 0.35 more on the back (the triceps hangs looser) within 8 cm.
Other bones' weights are scaled down to make room, as for the buttock bones.

`FMakeHumanJiggleNode` springs them like the breasts and buttocks (`MhSoftParts` in `MakeHumanAnimInstance.cpp`):

| Bone | Stiffness (1/s²) | Damping (1/s) | Room (cm) | Body softness scales it |
| --- | --- | --- | --- | --- |
| breast_l/r | 150 (breast slider down to 65) | 5.5 (to 3.2) | 4 (to 7.5) | no |
| butt_l/r | 110 | 5 | 4 | yes |
| genital | 70 | 3.5 | 3.5 | no |
| belly | 120 | 5 | 3 | yes |
| thigh_soft_l/r | 220 | 7 | 1.8 | yes |
| arm_soft_l/r | 240 | 7 | 1.5 | yes |

**Body softness** (`UMakeHumanAnimInstance::SetBodySoftness(Weight, Muscle, Age)`, the creator calls it from its
sliders): 0.5 + 0.4 weight - 0.3 muscle + 0.1 older, clamped 0 to 1. At 1 the soft parts' stiffness is 0.42 times and
their room 1.8 times; at 0, 1.45 and 0.45 times.

**Kicks**: `AddHitKick(At, Velocity, Radius)` adds Velocity (cm/s) to every soft part within Radius of the hit, more the
nearer, and a tenth of it to the whole body (250 cm); `AddBlastKick(At, Speed, Radius)` pushes every part away from the
blast, falling off with the square of distance. The creator's weapon demo kicks the defender on every body hit (club
160 cm/s over 40 cm, blades 110 over 30, times 0.6 to 1.2 by how heavy the attack is). `Pawns.SoftBodyScale` scales all
kicks. Kicks are queued on the game thread and handed to the node in `PreUpdate`.

**Distance**: the springs run fully within `Pawns.SoftBodyNear` (1300 cm) of the camera and fade out to
`Pawns.SoftBodyFar` (1500 cm); beyond that the soft parts stay as animated and cost nothing.

**Creator**: the Body tab shows "Flesh softness" and has "Slap belly" and "Blast nearby" buttons; `-CreatorBlast=<s>`
sets off a blast 3 m in front of the picked one every s seconds (for clips).

Bodies exported before this have no belly, thigh or arm bones; their springs simply aren't made.

`-CreatorSoftDemo` (with `-CreatorRecord=<seconds>`, about 16) is the judging clip: the picked one stands still close up
with a heavier, softer body (weight 0.6, muscle -0.4), is slapped on the belly at 1.5 and 6.5 s and blasted at 4 s with
the soft parts on, then the same at 9.5, 12 and 14.5 s with them off. `-CwScriptStyle=<0-3>` makes the scripted weapon
demos use one style (0 club).

**First build (2026-11-16):** weights woman belly 205, thighs 276 each, arms 187 each (man 184, 274, 165); springs
measured up to 6 cm, but the wobble was not visible in the 14 s creator clip (its own slider demo hid it) or the club
clip at their camera distance. A first rebuild had lopsided limb weights: `bone.head_local` goes stale in edit mode,
so `add_body_soft_bones` copies it first.

## Making it visible (second round)

The springs now have three times the room (belly 6 cm, buttocks 5, thighs 3, arms 2.5) and kicks twice to three times
as hard (creator slap 300 cm/s, blast 900, weapon hits club 320 and blades 220). The belly and buttocks also **squash
and stretch** (`FBody::Squash` 0.45 and 0.35): pushed in, the bone scales down along the line out from its parent and
up sideways by half as much; pulled out, the reverse. The scale goes on whichever of the bone's own axes lies nearest
that line, so no bone axis convention is assumed.

**Skin dents**: every `AddHitKick` also dents the skin at the hit (up to two at a time): a smooth bowl round the point
(radius 0.45 of the kick's reach, 8 to 18 cm), as deep as the hit is hard (0.03 cm per cm/s, at most 5 cm), in
over 40 ms, held 0.3 s, then springing back past flat into a small bulge and settling by 1.8 s (`exp(-3.5t) cos(9t)`;
a dent that lasted 0.1 s read as a shadow flicker). The point
rides on the nearest bone. `UMakeHumanAnimInstance::NativeUpdateAnimation` writes it to custom primitive data 16 to 25
(x, y, z, depth, radius twice) and `M_MH_SkinDetail`'s `SkinDent` node moves the skin along its normal in world position
offset. The first version moved the skin only and hardly showed (2026-11-16: 2.9 cm at 170 cm away read as nothing), so the depth and radius were raised and `SkinDentTilt` bends the normal into the bowl: its slope along the skin (dDepth/dr) turned into tangent space and added to the skin's normal, times 1.5. `Pawns.SkinDentScale` scales the
depth; dents fade with the soft-body distance fade. The creator's smoke note gives the deepest dent.

## Step 2: squish on contact (sneed said go, 2026-11-16)

Built in world position offset like the dents, not a Deformer Graph: the skin material already runs after the morphs
and skinning, it needs no new plugin, and every body (any slider mix, any kind) gets it for free. What it can't do that
a deformer could: keep a per-point state over frames (the springs and dents carry that on the CPU instead).

`USoftContactSubsystem` (Rim3D) each frame:
- picks the nearest 8 MakeHuman bodies within `Pawns.SoftContactFar` (600 cm) of the camera whose soft parts are on
  (`Pawns.SoftContact 0` turns it off);
- gathers what can press: every body's capsules (hands and head as spheres, forearms, upper arms, thighs, calves and the
  trunk), a capsule for each static mesh held or worn by a body (along its box's longest side), and a test probe
  (`SetProbe`); a body's own capsules and what it holds never press on it;
- for each near body lists up to 8 pressers within 3 cm of its flesh (its own capsules plus 8 cm) in its row of
  `T_MH_SoftContacts` (16 by 8 float texels, made by the MakeHumanArt commandlet, overwritten with
  `UpdateTextureRegions`), and puts the row in custom primitive data 13 (-1 none).

`M_MH_SkinDetail`'s `SkinSquish` node (after `SkinDent`) pushes each skin point facing a presser out of it, along the
line from the capsule's axis, by softplus(penetration, 2.5 cm) (at most 3 cm: 10 cm smeared the face), so skin inside is pressed flat onto the
presser and skin just outside eases in. `SkinSquishTilt` turns the normal towards lying on the presser where it is
pressed flat (in tangent space, like the dent's light). Both read the skin's position before any offset.

Creator: `-CreatorSoftDemo` now runs 24 s (record about 25): slaps and a blast with the soft parts on, then a 5 cm grey
ball pressing 4 cm into the belly (8-10.5 s), a cheek (11-13.5 s) and a thigh (14-16.5 s), then with the soft parts off
a slap, a blast and the ball on the belly again (21-23.5 s). The smoke note counts the frames the ball reached the
table.

Not yet: the face's teeth and jaw as hidden pressers from inside (so lips press against teeth), clothes following the
squish, and beds or seats pressing (a seat or bed is a static mesh not held by a body).

**First squish build (2026-11-16):** builds and runs (the ball reached the table in 222 frames), but: from the front
camera the ball hid the belly; the cheek target was 10 cm high and pushing up to 10 cm smeared the eye and brow; the
normal turned 80 % towards the presser left a hard black crescent round the ball on the thigh. Now: at most 3 cm of
push, the normal turned at most 35 % and only where the skin is really inside, the cheek 10 cm lower, the camera to the
side while the ball presses, and `Pawns.SoftContact` off by default (the soft demo turns it on) until it looks right.
