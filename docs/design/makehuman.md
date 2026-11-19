# MakeHuman characters (MPFB)

A second way to make people, next to the MetaHuman route ([looks.md](looks.md)), after the review of the real
MetaHuman body: MakeHuman's one shared base body (both sexes from one mesh, anatomy proxies, consistent nude skins),
built with MPFB in Blender and retargeted to the game's animations. This first step proves the pipeline end to end
with one example woman and a comparison scene; the live in-game creator (sliders as morphs, joints that follow
proportions, clothes that follow) is the next step.

Code: `tools/makehuman/mh_export.py` (headless Blender + MPFB), `tools/makehuman/woman.json` (the example),
`assets/makehuman.json` (what to import), Unreal `Rim3DEditor/MakeHumanArtCommandlet.*` (import),
`Rim3D/MakeHumanArt.h` (names), `Rim3D/MakeHumanAnimInstance.*` (retargeting), `PawnActor.cpp` (`ApplyMakeHuman`),
smoke test `compare`.

## Making a character

1. `blender.exe -b -noaudio --python tools/makehuman/mh_export.py -- tools/makehuman/woman.json` builds the human
   with MPFB (macro sliders: gender, age, muscle, weight, height, proportions, cup size, firmness, ethnicity), adds the
   `game_engine_with_breast` skeleton (the UE4 mannequin's bone names plus breast bones for jiggle later), the body
   proxy (the AGPL3 anatomy bodies: check the licence before shipping), the skin and any clothes (pubic hair), bakes
   it, and writes `game/Saved/MakeHuman/<Name>/<Name>.fbx` with the skin's colour map beside it as `skin.png`. The
   export is in centimetres with an unscaled, unrotated root (Unreal's retargeter measures bone heights). `{mh_user}`
   and `{mh_system}` in a config are MakeHuman's data folders (`MH_USER_DATA`, `MH_SYSTEM_DATA`). About 5 seconds.
2. `UnrealEditor-Cmd.exe game\Rim3D.uproject -run=MakeHumanArt -unattended -nosplash -nullrhi` imports every
   character in `assets/makehuman.json` into `/Game/Rim3D/Generated/MakeHuman` (not in Git; it starts clean):
   - the skeletal mesh, its skeleton and physics asset;
   - its skin on our skin material (`MI_MH_<Name>_Skin` on `M_PawnSkin`, so the pawn's skin colour drives it);
   - an IK rig (a copy of the UE4 mannequin's: same bone names) and a retargeter from the game's (UEFN) mannequin, a
     copy of the Game Animation Sample's UE4 one with every op pointed at the new rig, the two UE4-tuned leg ops
     ("Blend to Source", "Offset Goals", which planted a smaller body's feet where the mannequin's are) off, and the
     target retarget pose turned so each chain bone points where the mannequin's does.
   Run PawnLooks first (the skin material). Models that bring their own textures list `materials` (slot -> colour,
   normal, opacity, skin) and, for other skeletons, `chains` and `retargetRoot`.

## In the game

`FSimPawnLooks::MakeHuman` (a character's name) draws that character instead of the MetaHuman: a skeletal mesh
attached to the hidden mannequin, animated by `UMakeHumanAnimInstance` (one `RetargetPoseFromMesh` node with the
character's retargeter; the sample's generic retarget animation picked retargeters from its own list and ignored
ours). Its mesh has its own height, so `Height` is 1 for it. For now only a looks override sets it (smoke tests).

## Jiggle and hair

`UMakeHumanAnimInstance` runs, after the retarget, our own soft-tissue node (`FMakeHumanJiggleNode`) and long hair's
strands (`FMakeHumanHairNode`, [hair-physics.md](hair-physics.md)) on the long, bob and braid styles' chains and a
ponytail's bones (MakeHuman's ponytail sub-rig, joined into the main skeleton under the head by `mh_export.py`; enable
Blender's Rigify add-on, which MPFB needs to read older sub-rigs). `Pawns.MakeHumanPhysics 0` turns both off (read when
a character's animation starts).

Soft tissue: each soft bone's point follows where the animation puts it through a damped spring in world space (120 Hz
steps), so it lags behind steps, starts, stops, turns and landings and bounces back, within a limit: the breast bones
(MPFB's `game_engine_with_breast` skeleton; stiffness 160, damping 6, up to 4 cm) and buttock bones (130, 6.5, up to
3 cm). The buttock bones are made by `mh_export.py` for any MakeHuman body, with no hand work (`add_soft_bones` in
`mh_common.py`): one per side 6 cm inside the fullest point of the buttock behind the hip joint, parented to the pelvis,
weighted onto the skin within 11 cm (back of the body only) with the other bones' weights scaled down to make room. The
test `compare` notes how far the soft tissue lagged (about 2 to 3 cm walking and jumping).

The graph is linked before the engine's `FAnimInstanceProxy::Initialize`, which takes the custom root node there and
keeps it: linking it afterwards silently left every physics node out (why the first AnimDynamics jiggle never showed).

**Skin detail.** MakeHuman's skins are colour only. `mh_export.py` makes a normal map from the colour map's fine detail
(`make_skin_normal`: high-passed lightness as height, darker deeper; `skin_normal_strength`, default 6) and lists it as
the skin's normal; the MakeHumanArt commandlet puts MakeHuman skins on `M_MH_SkinDetail`, a copy of `M_PawnSkin` with the
MetaHumans' skin micro-normal tiled over that (`MicroTiling` 70, `MicroStrength` 0.4), so pores show close up.

**Non-human parts, made for the body they go on** (`mh_common.py`, no hand modelling): `"targets"` in a config sets
MakeHuman's own detail sliders by name (1258 of them: pointed ears, face shape...); `"horns"` (`add_horns`: tapered
horns out of the top of the forehead, found from the skin, weighted to the head) and `"tail"` (`add_tail`: a chain of
`tail_01`... bones under the pelvis and a tube skinned along it, in the body's own skin). Without a `"proxy"` the plain
MakeHuman body is used. `tools/makehuman/imp.json` is the test: a short wiry imp with pointed ears, horns, a tail and
yellow eyes; its red is the pawn's skin colour, like any colonist's (the tests use red for `Imp`). Tail physics
(`Pawns.MakeHumanTailPhysics`) is off: AnimDynamics let it sag and kink, so it stays as modelled until it has a
rotational spring of its own.

**Pubic hair** (MakeHuman's `natural_pubic_hair`) sits too high on these bodies, stopping well above where the legs
meet: `lower_pubic_hair` squeezes it to 60% of its height, narrows its top, and moves it down so its lowest point is
2.5 cm over the crotch (the lowest body point on its middle line), laid back onto the skin (`pubic_top_share`,
`pubic_above_crotch` in a character's config).

sneed's Petite Natural Pale Adult is rebuilt from his MakeHuman save (`tools/makehuman/petite_pale.json`, `"mhm"`) with
this skeleton rather than exported from his finished .blend (`blend_export.py`, Mixamo skeleton, no breast bones).

## Showcase (play)

`tools\play-showcase.bat` starts the editor build on the compare scene for play (smoke test `showcase`, which checks
nothing): the Crashlanded start on a cleared flat map, its first two colonists drawn as the ponytail woman (the player's:
direct control) and the long-haired example woman (drafted, standing), the rest removed, graphics turned up (epic
scalability, full resolution, TSR, Lumen GI and reflections, virtual shadows, contact shadows). `-ShowcasePlayer=` and
`-ShowcaseNpc=` pick other characters. Hardware ray tracing is off in the project (Lumen traces in software).

Controls in direct control (for every colonist, not only here): WASD, mouse to look, Ctrl walk, **Shift sprint** (1.5 times
the colonist's cruise speed, at least 6.5 m/s, with the sample's sprint animations, which only it reaches), **Space jump**
(`ASimPawnActor::StartJump`: a 45 cm arc over 0.62 s of game time with the sample's take-off by speed, fall loop and
landing; only the picture, the sim keeps the colonist on the ground), Left Alt dodge roll (it was Left Shift), **F2
pause** (Space still pauses in the overseer view), V view, Tab overseer.

**Feet.** Every pawn's motion matching predicted it would speed up to its def's cruise speed, which pawns often never
reach (drafted, hurt, slow ground), and the player's colonist (no route) was predicted to stop: moving pawns played
walk-to-run starts and standing turns while the feet glided. The sim now reports each pawn's real pace
(`Mover::pace`, `ThingView::pace`), the prediction aims for that pace once the pawn holds its speed, keeps a routeless
moving pawn going, and smooths the velocity; the player's colonist is drawn at the player's body's own position and
velocity (smooth every frame) instead of the sim's uneven steps. `-CompareRecord` and `-ShowcaseFeet` film the feet.

`-ShowcaseDemo` drives her by itself (run away from the view, sprint across it and towards it, jumping) and notes the run
and sprint speeds and jumps; with `-ShowcaseRecord=<seconds>` every frame of that long is saved at a fixed 30 per second
for a clip (`tools\smoke.ps1 -Test showcase -Resolution 1280x720 -ExtraArgs @("-ShowcaseMinutes=0.45","-ShowcaseDemo","-ShowcaseRecord=22")`).

## Character creator (first version)

`tools\play-creator.bat` (smoke test `creator`) opens four colonists in a row (a baseline woman and man, a woman imp,
a man orcoid) with the graphics up and a panel (top left) that changes the picked one live, in tabs: **Gender** (woman
or man: which base, chosen apart from the kind; more masculine or feminine), **Kind** (Baseline, Imp, Orcoid presets,
then pointed ears, imp face, orc face, horns, tusks, tail), **Body** (height as mesh scale, older, thin to heavy, soft
to muscular, breasts on the woman, random), **Hair and skin** (pubic hair none, natural, trimmed or strip, and lower;
skin colour). "Walk back and forth" and the view sliders sit under the tabs. Adults only: there is no younger slider.

The bases (`CreatorFemale`, `CreatorMale`) come from `python tools/makehuman/mh_creator.py`, which reads
`tools/makehuman/creator.json`: each base is a normal MakeHuman character (with horns and a tail) exported with a morph
target per slider, each morph being the difference to a build with one setting changed (`mh_export.py`
`"dump_vertices"` and `"morphs"`; the horns and tail morphs shrink them away into the skull and back). About 4 seconds a
build, a minute a base. The skeleton is fitted to the base, so sliders that would move joints a lot (proportions, leg
length) are left out; height scales the whole mesh.

The man's base has MakeHuman's male anatomy body; `add_genital_bone` (`"genital_bone"`) finds the genitals from the
shape (what stands in front of the thighs at the crotch) and gives them a soft bone weighted by how far a point hangs
from the root, which the jiggle node swings loosely (stiffness 70, damping 3.5, up to 3.5 cm).

**Grown pubic hair** (`grow_pubic_hair`, config `"pubic_grow"`) replaces MakeHuman's flat pubic hair card: about 3000
thin tapered curly ribbons rooted on the skin, growing out and lying down towards the middle line, each weighted like the
skin at its root (so they follow the genitals' jiggle). The area is found from the shape for any body: on a woman the
mons, an inverted triangle down to the top of the vulva with a few along the labia; on a man the pubis above the penis,
a few on the scrotum and a thin trail up. The layout (triangle and barycentric per hair) is chosen once on the base
(`mh_creator.py` runs a first pass) and reused by every variant, so morphs move the same hairs; the creator's
Live controls: each hair carries a random number (texture U) and how far out it grows (V); `M_MH_PubicHair` cuts
hairs out by the pawn's density (custom primitive data 4: even thinning) and coverage (5: edges and the top go
first), and the `PubicShort` and `PubicStraight` morphs trim and straighten them. The female top edge arches (highest
on the middle line); the male trail is gone (body hair later). The creator's "Jiggle" box turns the soft-tissue
springs off (`UMakeHumanAnimInstance::SetSoftTissueEnabled`), keeping breasts, buttocks and genitals still. "Shaved (stubble)" is the `PubicStubble` morph (hairs cut to
a few millimetres, straight); coverage shrinks the patch evenly towards its middle and bottom (no per-hair jitter).

`round_hair_ends` gives long scalp hair (MakeHuman's long01) a soft U at the back, longest in the middle, instead of
a straight cut. The orcoid has an `OrcBody` morph (muscle and weight up, broad back, chest, neck and arms, from
MakeHuman's own targets: no joints move) beside `OrcFace`, and tusks just under the mouth corners. Creator pictures:
`-CreatorNoPanel`, `-CreatorFocusZ=`, `-CreatorPitch=`.

The creator's view is free: middle mouse turns it, the wheel zooms, right mouse or WASD/QE move the point it looks at
(not while the mouse is over the panel). The panel text is half as big again as the other panels.

**Genital size and pigment.** The creator's genital slider is the `GenitalLarge`/`GenitalSmall` morphs: on the man
MakeHuman's own penis length, girth and testicle targets (kept to a natural range); on the woman `fuller_labia` pushes
the vulva out (or in) along its normals. `make_pigment_mask` writes `pigment.png` (areolae and genitals, found from the
shape, rasterised into the skin's texture space without blur and skipping seam-stretched triangles, which drew lines
on other skin); `M_MH_SkinDetail` darkens and warms the skin there by the pawn's pigment (custom primitive data 3, the
creator's "Areola and genital pigment" slider). It multiplies the skin colour, so it follows the skin tone. The crotch
is found leaving out what stands in front of the thighs (`_perineum_heights`), so male pubic hair sits on the pubis.

**Tail**: `FMakeHumanTailNode` swishes any `tail_01`... chain: a wave running down the tail, quicker and wider when moving,
and a swing out against turns (no physics; AnimDynamics let it sag). `-CreatorDemo` (with `-CreatorRecord=<seconds>`)
moves the sliders by itself for a clip.

## Smoke test `compare`

Three pawns side by side on a cleared flat map in the morning sun: the MakeHuman example woman, a local-only
comparison model (`-CompareLocal=<name>`, default Shadowheart), and the MetaHuman showcase woman with a ponytail.
Standing, close up, each face and body, then drafted and walking back and forth (24 frames for a clip:
`ffmpeg -framerate 5 -i compare_walk_%02d.png ...`). Every frame it checks each MakeHuman character shows its own
retargeted mesh and the MetaHuman is hidden. Measured: the retargeted thigh and upper arm point within 1 degree of the
mannequin's.

**Local-only comparison models** (other people's characters, such as a fan model of a game character) are listed in
`assets/makehuman.local.json`, which is not in Git, and import into `/Game/ThirdParty/Local`, which is not in Git
either. They are never committed or shipped.

## 4K skin (switch)

`tools/makehuman/skin_4k.py` (run by `mh_creator.py` for each base) makes `skin_4k.png` (a sharp 4096 resize of the
2K skin with faint mottling and pores added, so tones, the pawn's skin colour and pigment are kept), `skin_normal_4k.png`
and `skin_rough_4k.png`; the MakeHumanArt commandlet makes `MI_MH_<base>_Skin4K` beside the 2K skin (`RoughnessMap`,
`RoughnessMapAmount` 0.35 on `M_MH_SkinDetail`). The creator's "4K skin" box swaps them on every body, so the two can
be compared; the 2K skin stays the meshes' own. An AI upscale (Real-ESRGAN x4plus, `upscale_skin.py`) was tried: it
smooths skin and shifts its tone, so it is not used. `-Creator4K` for pictures.

## Facial expressions

No face bones: MakeHuman's 34 expression units (MPFB `targets/expression/units/caucasian`: eyelids, brows, mouth
corners, jaw open, nose, neck) are morphs `Face_<Unit>` on the creator bases (`creator.json` variants; a "folder/name"
target picks one file), so the skeleton and every animation stay as they are and the eyes, teeth, brows and lashes
follow. The creator's Face tab has expressions made of them (smile, grin, frown, sad, angry, surprised, afraid,
disgusted, in pain, pout, asleep) with a strength, the 34 units for fine tuning, an automatic blink and talking.
`-CreatorFace=<name>` for pictures.

## Eyes, wrinkles, brows and lashes

`M_MH_Eye` (MakeHumanArt, for the eye slots): glossy wet surface, the iris set back behind the cornea by parallax
(`BumpOffset`), a darker limbal ring. Wrinkles: `make_wrinkle_map` (config `"wrinkles"`: groups of face-unit morphs)
writes `wrinkles.png` (forehead, brow furrow, smile, nose in RGBA) from how each group squeezes the skin, creases
across the group's mean squeeze direction on each side of the face; `M_MH_SkinDetail` adds them to the normal by
custom primitive data 8 to 11, which the creator sets from the face (checked: forehead and brow creases when
angry or surprised, smile lines when smiling). Hair, brow and lash cards use a dithered cut-out with a lower
cut-off (0.2), so they are softer and fuller. The creator lights faces with the 7:30 morning
sun (`-CreatorHour=`). `MH_CREATOR_REUSE=1` rebuilds only the bases, keeping the variant builds.

## Hair styles, markings, jewellery

**Scalp hair**: each base carries several MPFB hairs (`creator.json` `"hairs"`) as their own meshes; the creator shows
one with `ShowMaterialSection` (woman: long, bob, ponytail, braid, short, afro; man: short, crop, messy, long, ponytail,
afro; or bald). **Markings**: `tools/makehuman/markings.py` draws 16 original designs into an atlas (8 tattoos, 4 scars,
a brand, 3 body paints); `write_body_areas` writes `areas.json` per base (a point on the skin per body area, found from
the bones, with which way is up and the texture per metre both ways, measured away from seams); `M_MH_SkinDetail` stamps
up to four (`MarkNA/B/C`: place, design, kind, colour; scars paler and raised, brands dark and raised, paint opaque).
Pawns never get markings on their own: only the creator's Markings tab sets them. **Jewellery**: `add_jewellery`
generates earrings, a nose ring, nipple barbells, a navel ring, a genital ring, a finger ring, a necklace, a collar,
wrist cuffs and ankle shackles fitted to the body and weighted to the bone each rides on (piercings follow the jiggle);
`M_MH_Metal`; the creator's Jewellery tab shows each and picks its metal. Note: MPFB's breast bones start at the
sternum, their tail is in the breast (the pigment mask now uses the tail too).

## Beast-Strain (cat)

A fourth kind in the creator's Kind tab beside Baseline, Imp and Orcoid (sneed, 2026-11-15: cat only for now). It looks
the same as a person except for these parts, all generated for whatever body they go on (`mh_common.py`):
- `add_cat_ears`: cat ears coming up out of the top of the head either side of the crown, tilted out and turned slightly
  outwards. They have a crescent cross-section with a cupped front, and are weighted to the head. Outside material
  CatFur, inside CatEarInner (skin, pinker).
- `add_cat_claws`: one small curved claw per fingertip, starting at the nail side and curling towards the palm, weighted
  to the last finger bone. Material CatClaw.
- A furred CatTail: `add_tail` called a second time, so it uses the imp tail's bones. That means the tail swish node
  moves it.
- The human ears: removed by the CatHead morph (`flatten_human_ears`). Each ear is laid onto the side of the skull,
  using the skull just in front of and behind it, and unfolded by smoothing so it leaves no creases.

`creator.json` sets `"cat"` on both bases, which gives every variant the parts so the morphs fit them too. The creator
shows the cat parts' material sections only on this kind and hides the imp tail (NoTail). CatFur's colour is the hair
colour with a shade slider (darker to lighter), or its own colour. Its texture is `make_fur_textures` (strands along the
tail, with a normal map). Earrings are hidden on this kind. `-CreatorKind=3` puts everyone in it for pictures.

## Creator round 3 (sneed, 2026-11-16)

**Drow** is a fifth kind with these defaults:
- Long pointed ears: the ElfEars morph, MakeHuman ear targets beyond 1.
- Red irises with a strength slider. The eye material has IrisColour and IrisAmount: the texture's lightness is kept and
  the colour laid over it. Everyone can set an eye colour.
- Platinum hair; any hair colour can still be picked.
- Skin picked along `CreatorDrowSkins`, from light ash violet through slate and midnight purple to obsidian.

**Ears under headgear:** the creator's headgear test shrinks the ImpEars and ElfEars weights: ear tops covered keeps
35%, whole ears covered keeps 0, so nothing pokes out of a helmet. The game's headgear should do the same once
clothes have an "ears covered" flag.

**Jewellery** (`add_jewellery`):
- Ear piercings: a lobe hoop through the lobe, a stud, a dangle, a helix ring, a tragus stud and an industrial bar.
- Navel: a ring, a gem stud and a dangle.
- A woman's genital ring goes through the clitoral hood: the point where the front of the body, coming up from
  between the legs, turns upright. It used to sit on the mons.
- The finger ring and collar are fitted to the limb's own cross-section (`fit_ring`). MakeHuman's finger bones run near
  the back of the finger, so a ring round the bone sat off-centre and looked like a halo.
- The necklace is a chain of links lying on the skin, lower at the front. Each point takes the skin weights under it.
- Piercings take the skin weights of the skin they go through, so a nipple barbell moves exactly as the breast does.

**Animation:**
- Breast jiggle slider: 0 is the old default; 1 is softer and bouncier, still damped.
- Long hair rests on the body (since 2026-11-19: strands with collision capsules, [hair-physics.md](hair-physics.md)).
- People who are standing play a seamless breathing idle (a Mixamo unarmed idle) instead of the game's idle, which
  restarted.
- Picking another clip while one plays switches to it.

**Faces:** the nose wrinkles stay on the nose and the smile wrinkles stay off the upper lip, so disgust and pain lost
their lip crease. Forehead creases are softer and only partial for inner-brow raises. Sad, surprised, disgusted, pain
and pout are redone. The old pout is now "Sip", and there is a new "Smile (teeth)".

The menu text is 15% smaller, and the kinds sit two to a row (the fourth had fallen off the panel).

## Wetness and long hair

**Wetness** is one value per pawn, custom primitive data 12 (0 dry to 1 soaked). `MhAddWetness` in the MakeHumanArt
commandlet adds it to every MakeHuman material:
- Skin darkens to 0.78× and becomes glossy (roughness down to 0.12).
- Hair and cards darken to 0.55× (roughness 0.3).
- Pubic hair darkens to 0.55× (roughness 0.3).
- Metal stays the same colour but gets shinier (roughness 0.1).

The plain materials are now `M_MH_*_Wet`, new assets, because the commandlet keeps an existing plain material as it is.
The creator's "Hair and skin" tab has a Wet slider, and `-CreatorWet=<0-1>` sets it for pictures. Next: the weather
sets it (rain raises it, shelter or a fire dries it), clothes and armour get the same step, and droplets.

**Long hair** swings: `add_hair_bones` ("hair_bones" in a config) adds five chains, hair_c1_1 to hair_c5_4, under the
head. They hang down the back, the back corners and the sides, shaped along the longest style, and are shared by long01,
bob02 and braid01. The points of each style are weighted to the two nearest chains, with the head holding the top.
`UMakeHumanAnimInstance` simulates them as strands that fall, swing and rest on the body ([hair-physics.md](hair-physics.md)). `feather_hair_ends` rewrites the
long style's alpha so the strands end at different lengths: wispy ends instead of one straight cut.

## Animation clips

Clips are listed in `assets/animations.json` (name, FBX, loop). Markerless mocap (FlowHMR) stands metres from the origin
and may face another way than its skeleton's rest pose: `tools/makehuman/clip_prep.py` centres the hips over the clip
and turns the motion to face as the rest pose does ("source" -> "fbx"). `UnrealEditor-Cmd ... -run=MakeHumanArt -Clips`
imports each FBX with its skeleton (Mixamo), builds an IK rig with the mannequin rig's chain names, a retargeter from it
onto the game's mannequin (the template's ops, the mannequin's pose turned to the T pose), and retargets the clip with
`UIKRetargetBatchOperation::RunBatchRetarget` into `/Game/Rim3D/Generated/Animations/<name>/A_<name>`. The pawns play
mannequin animations, so every body (each through its own retargeter) plays every clip. The creator's ANIMATIONS panel
(right) lists the clips and plays one on the picked person or everyone (loop, speed); `-CreatorPlay=<name>` with
`-CreatorRecord=<seconds>` films it. Clips: Skip (jump rope, FlowHMR, sneed) and 16 from the mocap thread (`mocap/`: mining, chopping, digging, planting,
sweeping, carrying firewood, eating seated, lying down and getting up, shadowboxing, punches, kick, sword lunge, bow draw,
rifle aim, shotgun reload; no props). `clip_prep.py` bakes the clip's own frame range (Blender's default 1 to 250 cut
or padded every clip to 8.3 s).

Mixamo set: 436 clips from sneed's downloads in `mocap/mixamo/` (10 packs under `packs/` plus 56 singles, on the Y Bot,
mostly without skin). `tools/makehuman/mixamo_prep.py` loads them all in one Blender run. For each clip it:
1. Moves the hips so the clip starts over the origin.
2. Bakes Mixamo's object transform (a 90° turn about X and 0.01 scale) into the bones and location keys.
3. Moves the clip onto PetitePale's rig, the rig the other clips use (it has the Mixamo bone names). Each
   target bone's rest is first turned to point where the Y Bot bone's rest points (T pose against A pose). The Y Bot's
   turns from rest are then copied onto it in world space, and the hips are moved as the Y Bot's, scaled by hip height.
4. Writes `game/Saved/Mocap/Mixamo/<Name>.fbx` (the armature alone).

It also writes `Rig.fbx` (the rig with its mesh, the shared skeleton) and `mixamo.json` (name, category and file).

Three Unreal problems led to this design:
- Unreal reads the Mixamo object transform one way for a skeleton and another for an animation, so the clips played
  lying on the ground.
- Unreal drops a top node named "Armature" (the armature is exported as MixamoRig) and converts only the top bone's
  rest position to centimetres, so the bodies sank to the waist.
- Every clip, the older ones too, leaned 30 to 90 degrees more than its source: the bodies folded at the waist or fell
  "sideways into the ground". The cause was the clip retargeter's IK step. The mannequin's full-body IK bent its
  upper spine to reach the hand goals. Clip retargeters now run FK only (`-ClipIK` turns IK back on), and
  `-Probe -ProbeAnim=<anims>` prints each clip's pelvis-to-head lean over time to check this. Each body's own
  retargeter still plants the feet.

`assets/animations.json` "sets" points at the manifest. The commandlet imports the rig once and every clip onto its
skeleton (saving the raw clips under `Source/Raw` for `-ProbeAnim`), then retargets a category at a time into
`Animations/Mixamo/<category>/A_<name>`. The categories are each pack, plus the singles grouped as Injured, Falls,
Crawling, Fear, Cover, Rolls, Fighting, Firearms, Interact and Other. The creator's ANIMATIONS panel folds them by category and
has a search box.

`-Clips -Only=<name>,<name>` imports just those clips (from `animations.json` or the manifest) and adds them to what is
there: nothing is deleted and the set's IK rig and retargeter are reused. New Mixamo downloads go the same way: run
`mixamo_prep.py` on a folder holding only them (same target rig), copy the prepared FBX next to the others and add
them to `mixamo.json`, then `-Clips -Only=...` (how the punches went in, 2026-11-17).

## Not done (next, as of 2026-11-14)

- Scalp hair styles in the creator: put several MPFB hairs (long01, bob02, short02, ponytail01, braid01, afro01) in each
  base and show one with `ShowMaterialSection`; bald.
- Tattoos and markings (a UV-space layer in the skin material) and generated jewellery (piercings, rings, collars,
  shackles, chains), as planned with sneed.
- Every test pawn from the creator's seed or randomizer (sneed's rule); MakeHuman characters in the sim's looks,
  clothes with the same morphs (and the soft-bone weights copied), LODs.
- Jagged, square shadows on bodies (look at virtual shadow map resolution, hair cards casting shadows, contact
  shadows); hardware ray tracing is off in the project (Lumen traces in software); DLSS (NVIDIA's plugin) not set up.
- Pubic hair styles beyond natural/trimmed/stubble; body hair; a lower placement option.
- Foot sliding is better but motion matching still picks odd clips at times (`-ShowcaseFeet`, `-CompareRecord`).
