# Gun range (first person)

A scene to play, asked for by sneed (2026-11-16): first person with real gun models, unlimited ammunition but real
reloads, and random creator women to shoot at who stand, walk, flinch, fall down and die. `tools\play-gunrange.bat`
(smoke test `gunrange`, which checks nothing unless `-GunRangeScript` is given).

**The sim decides, Unreal shows.** Shooting, spread, hits, armour, wounds, downing and death are the game's own
(docs/design/fights.md, armour.md, aiming.md): the player's left mouse button sends the same `ShootCommand` as anywhere in
direct mode, R the `ReloadCommand`. Magazines already refill without ammunition items (aiming.md), so ammunition is
unlimited and only the reload time counts. Everything below is drawing.

Code: `game/Source/Rim3D/GunRangeSubsystem.*` (the arms and guns), `GunViewAnim.*` (their animator),
`SmokeTestGunRange.cpp` (the scene), `CreatorPeople.h` (the creator's randomizer for other scenes, in
`SmokeTestCreator.cpp`), `Rim3DEditor/GunArtCommandlet.*` (model import). Data: `assets/guns.json`.

## The scene

The Crashlanded start on a flat 128 m map, the middle cleared. The first colonist is the player (a random creator
woman, so the arms you see are hers); the rest leave. One of each gun lies in a rack at the player's feet. Eight random
creator women (baseline humans, fair-weighted skin, seeded: `-GunRangeSeed=<n>`) stand at 10 to 30 m: the near four
stand, the far four walk across the field and back. They are the player's own side, drafted and unarmed, so they never
shoot back. They are drawn by the game's usual pawn drawing: hit reactions, blood, wound decals and ragdolls when downed
or killed (FightSubsystem). N brings a new group; when all are down for 6 seconds a new group comes by itself.

## The arms and the gun

Hung on the view camera, so they move with the gun kick and shake of the shooter feel:

- A hidden mannequin plays the gun's **stance** clip (Mixamo rifle aiming idle, the mocap pistol aim) through
  `UGunViewAnimInstance`, which samples and cross-fades clips natively (no animation graph). A **reload** clip blends
  over it, stretched to the sim's reload time.
- The player's own MakeHuman body is retargeted from it like every pawn's (`UMakeHumanAnimInstance`), with the head,
  hair and legs hidden (`HideBoneByName`), no shadow. Skin, build and materials are copied from the player's body.
- The **gun** sits in the right hand from the knuckles: the grip runs from the little finger's knuckle to the index
  finger's, the barrel along the hand, tilted back by the gun's grip `rake`; the gun's `grip` point lands in the palm.
- **Aim**: the body hangs from its head, which sits at `headInView` (top of guns.json: a little behind and below the
  eye), and turns so the barrel runs along the view. The right hand then reaches (a two-bone IK on `hand_r`, its
  rotation kept from the clip) until the gun's `sight` point sits `sightDistance` before the eye, or `hip` lower right
  of that from the hip; through a reload it reaches to `reloadSpot` instead, so the gun stays in view while the clip
  turns it. (Moving the whole body to bring the sight there put the camera inside a shoulder: the Mixamo stances don't
  hold the sight before the eye.) Right mouse button blends to the sights (the field of view narrows as anywhere in
  direct mode). `"reach": false` leaves a gun in the clip's own hold.
- **Scope**: a gun with `"scope": {"fov": n}` (the bolt-action rifle's side-mounted scope) goes through it once fully
  aimed: the arms and gun hide, the view narrows to `fov`, and a scope ring and post reticle are drawn over it. On the
  way up the scope's eyepiece comes to the eye (its `sight` is the eyepiece on the scope's axis).
- The **left hand** goes to the `offHand` point (the fore-end) with the MakeHuman two-bone IK, off through the reload.
- **Moving parts**: the bolt-action rifle's bolt lifts and runs back after each shot and opens for the reload (the Codex
  model's own parts and pivots), triggers pull. A gun with a separate magazine mesh drops the empty one and the left
  hand brings the full one.
- **Kick** (a snap back and up per shot, fading by the gun's `recover`), **sway** against the mouse, **bob** with the
  walk, a **dip** when switching guns, lowered while sprinting, and a short warm light at the muzzle.

Keys: 1-4 or the mouse wheel pick a gun (`EquipCommand` on the rack's item), F6 reads `assets/guns.json` again so the
numbers can be tuned while it runs.

## Guns and models

| Key | Sim def | Model |
|---|---|---|
| 1 | Gun_BoltActionRifle | Codex bolt-action rifle (ours; body, bolt, trigger) |
| 2 | Gun_AssaultRifle | "Soviet Assault Rifle" by SpatialNeglect (Fab, free) |
| 3 | Gun_Autopistol | "Pistol 9mm Custom" by DTry (Fab, free; separate magazine) |
| 4 | Gun_PumpShotgun | "Model 12 Pump Action Shotgun" by the 9th Earl (Fab, free) |

Models are not in Git (public repo): they go in `assets/raw/guns/<Name>/` and the GunArt commandlet imports them into
`/Game/Rim3D/Generated/Guns/<Name>/` (each FBX object its own mesh; textures onto `M_GunPBR`). A gun without a model
shows a dark box the gun's size, and the panel says "no model yet". The commandlet skips a gun whose FBX is missing
with a warning, and builds `M_GunPBR` again on every run (grey maps are Masks samplers with its own linear
`T_GunMaskWhite`/`T_GunMaskBlack` defaults; the engine's squares are sRGB and broke the compile).

The Codex rifle (`BoltActionRifle_Game.fbx`) imports as `SM_BoltActionRifle_Body` (bounds -42.5..69.5 x -5.5..2.4 x
-8.1..12.0 cm), `_PIVOT_Bolt` and `_PIVOT_Trigger`, already in the gun frame (+X muzzle, +Y right, +Z up, grip at
the origin): muzzle socket (69.5, 0, 3.2), fore-end (34, 0, -2), scope axis at y -2.6, z 8.9, eyepiece at x -7.

## Not done

- The shotgun's pump and the slide of the pistol don't move (the shotgun comes as one mesh; its own rig and reload
  animation are not used yet).
- No sounds of its own (the game's gunshot cues play from the sim's events as anywhere), no shell ejection.
- Targets have no clothes or armour (MakeHuman bodies have no clothes yet); the combat voices aren't on this branch.
