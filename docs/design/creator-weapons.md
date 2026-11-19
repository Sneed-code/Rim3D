# Creator weapons and fight demo

The character creator (`tools\play-creator.bat`, smoke test `creator`) has a **WEAPONS** window (bottom right). The
picked person holds a weapon, you pick a fighting style and an attack to watch, and a fight demo puts two people face to
face. It shows sneed what weapons, staggers and blood could look like before the game's own melee uses them. All of it is
cosmetic and lives in the creator: it plays Mixamo clips on the pawns and moves nothing in the sim.

Code: `game/Source/Rim3D/CreatorWeapons.h/.cpp` (namespace `CreatorWeapons`, helpers prefixed `Cw`). The creator calls
`CreatorWeapons::Tick` once a frame and `CreatorWeapons::DrawWindow` inside its ImGui frame (`SmokeTestCreator.cpp`,
three lines). Blood splats use `M_BloodSplat`, made by the FightMaterials commandlet (below).

## The window

- **Hold a weapon**: the picked person holds the style's weapons. Nobody else does, except the two fighters in the demo.
- **Fighting style**: Club (one hand), Sword (two hands), Sword and shield, Dual wield.
- **Attack**: picking one plays it straight away, then again and again with a short pause in the style's stance between
  swings. "Stance (idle)" just stands ready, and "Guard"/"Block" holds the guard. Speed slider.
- **Fight demo**: "Start the fight" stands the picked person and the next one face to face ("Apart", 110 to 260 cm,
  default 160). They take turns: one attacks with a random attack of the style (or only the picked one), and the other
  either guards (35% when its style has guard clips) or takes the blow. **Finish** appears once someone has taken 3
  hits: the next blow lands and plays a death clip. The body lies still for 4 seconds, gets up and the fight goes on.
  Stop ends it.
- **Blood**: on or off, and **Clean up** clears every stain, spot and pool.
- **Grip (fine tuning)**: moves and turns each hand's weapon for the style, for fitting the grip by eye.

## Weapons in the hands

The weapons hang on the **visible MakeHuman body** (`ASimPawnActor::GetMakeHumanMesh`), not the hidden mannequin:
- The club, longsword and gladius are the defs' own procedural meshes (`visual.parts` of `Melee_Club`,
  `Melee_Longsword`, `Melee_Gladius`, built with `VisualMesh` like `UWeaponDrawSubsystem`).
- The shield is generated: a round wooden board 50 cm across with a slight dome, an iron rim and an iron boss (24 sides: four six-sided discs turned 15 degrees apart).

A weapon's place in the hand is worked out each frame from the hand's bones (the UE4 mannequin names that MakeHuman's
game_engine skeleton has: `hand_r`, `index_01_r`, `middle_01_r`, `pinky_01_r`, `index_03_r`, and the left ones):
- The grip runs from the little finger's knuckle to the index finger's, so the blade leaves the fist on the thumb
  side.
- The blade's edge points the way the fingers point.
- The grip sits 2.5 cm in from the knuckles, on the palm side. The palm side is found from where the curled index
  finger points.
- The blade leans about 15 degrees the way the fingers point, and any part of it that runs back along the forearm
  is taken out.
- The shield rides the left forearm, 55% of the way to the wrist, its face out from the back of the arm.

Each weapon is attached to its hand bone (the shield to `lowerarm_l`) so it moves with the hand between updates. The
two-handed sword is held in the right hand; the left hand follows the clip, which roughly keeps it on the grip. There
are no two-sword clips, so dual wield holds a gladius in each hand through one-handed moves, and the window says so.

## Clips (under `/Game/Rim3D/Generated/Animations`)

| Style | Stance | Attacks | Guard / blow on guard | Hit from the front | Death |
|---|---|---|---|---|---|
| Club | MeleeAxe StandingIdle | MeleeAxe StandingMeleeAttack Downward (Overhead), Horizontal, Backhand, 360High, 360Low, ComboAttackVer1-3, RunJumpAttack, KickVer1; HeavySwing (mocap) | StandingBlockIdle / StandingBlockReactLarge | StandingReactLargeGut, Injured HitReaction, HitToHead | Magic StandingReactDeathBackward/Forward |
| Sword (two hands) | GreatSword Idle | GreatSword Slash 1-5, Attack, HighSpinAttack, JumpAttack, SlideAttack, Kick | Blocking 1-3 / Blocking3 | GreatSword Impact 1-5 | TwoHandedSwordDeath 1-2 |
| Sword and shield | SwordAndShield Idle | SwordAndShield Attack 1-4, Slash 1-5, Kick (Block: BlockIdle) | Block 1-2 / Block2 | SwordAndShield Impact 1-3 | SwordAndShield Death 1-2 |
| Dual wield | MeleeAxe StandingIdle | DualSticks (mocap), SwordAndShield Slash 1/3/5, Attack2, MeleeAxe Backhand | GreatSword Blocking / Blocking3 | SwordAndShield Impact 1-3 | SwordAndShield Death2, Magic DeathBackward |

Hits from the side or behind play the Magic pack's `StandingReactSmallFrom<Right|Back|Left>`. Heavy hits play
`StandingReactLargeFrom<...>`, or `Injured/StumbleBackwards` from the front (60%; otherwise the style's own impact). A
finishing blow from the side or behind plays `Magic/StandingReactDeath<Left|Forward|Right>`. Each attack has a weight
from 0.4 (a kick) to 1.0 (heavy swing, jump attacks). The weight decides how hard the blow is: the stagger chance and
how much blood.

Jump clips (the club's running jump attack, the great sword's jump attack) lift the body much higher than a real leap
(the man sailed 1.5 to 2 m up over the other's head). In the fight demo the pelvis may rise at most 30 cm above its
stance height; the visible mesh is lowered by the rest, at once on the way up and gently as it lands (log line "Jump
capped").

The attacks that carry the body metres forward (the club's running jump attack, the great sword's jump and slide
attacks) are for the one-person preview only: in the fight demo the running jump ran him straight through her, she
folded over him and both were carried out of the frame. The demo never picks them, even with "Only the picked
attack" on (it picks another).

## Blades that stop

Each frame of a swing, between 10% and 90% of the clip, the attacker's blade (hilt to tip) is swept from where it was
last frame to where it is now in 6 steps. Each step is tested against:
- the defender's shield, as a disc 50 cm across: a point of the blade within 3 cm of its face, or crossing it;
- the defender's blade, within 4 cm;
- the defender's body, as 12 capsules from bone to bone (torso, neck, head, arms, legs; radii 4.5 to 15 cm);
- the world: a line trace along the tip's path and along the blade (ECC_Visibility, the two pawns left out), and the
  ground under the attacker.

On contact:
1. The attacker's clip is set back to the moment of contact, so the blade doesn't show inside what it hit.
2. Both freeze for a hit-stop: 0.08 s, or 0.12 s for a heavy hit.
3. The attacker's clip runs backwards for 0.16 s (the blade bounces off) and blends out to the stance.

A blow that reaches the body of a defender guarding from the front counts as taken on the guard. The guard clips
don't always put the weapon in the way, so this keeps blocks reading as blocks. Blocks throw sparks (and wood chips off
a club or the shield), and strikes on the ground throw dust, through `UFightSubsystem::SpawnMeleeImpact`.

## Blood

- **Spray**: 10 to 44 droplets per hit, by how hard the blow was (a club makes about half as much as a blade):
  - 70% fly with the blow in a 32° cone;
  - 20% are thrown up and back off the blade;
  - 10% just fall.
  Droplets are instanced balls stretched along their velocity, with gravity and a little drag. Where one lands
  (75% of them) it leaves a spot on the floor, drawn out the way it flew. At most 400 droplets at once.
- **Body stains**: a decal at the hit point, on the nearest bone of the visible body, so it moves with the body and
  marks clothes too. A run of blood goes below it unless the blow was light. A new blow near an old stain makes it
  bigger and fresh again. Fresh wounds drip for 25 seconds, adding spots under the person. At most 24 stain decals
  per body.
- **Pool**: once someone has bled enough (about three good hits), a pool spreads under them to 16 to 42 cm; up to
  70 cm while dying or down.
- **Drying**: every decal starts wet, red and glossy (colour 0.28, 0.01, 0.008; roughness 0.1), and darkens towards
  near-black and dull (0.075, 0.005, 0.004; roughness 0.55) over about a minute.
- **Caps**: 64 floor spots (the oldest goes first).
- **M_BloodSplat** (`-run=FightMaterials`, which now makes `M_WoundDecal` and `M_BloodSplat` if they are missing):
  - a translucent deferred decal with a ragged sharp edge from a coarse noise;
  - small thrown-off drops from a fine noise peaking near the rim;
  - each decal looks different because of its `Seed`;
  - parameters `Color`, `Roughness` and `Opacity`.
  Without it the blood falls back to the round `M_WoundDecal`, and the window says so.

## Not done

- The left hand isn't pinned to a two-handed grip (no IK), and dual wield has no mirrored or two-sword clips.
- Body contact uses bone capsules, not the physics asset, and the shield is a flat disc.
- The two bodies are kept at least 55 cm apart (pelvis to pelvis) during lunges: their spots slide apart and drift back.
- Staggers come back to the spot they started from when the clip blends out (no root motion carried over).
- The demo turns and moves the visible body, not the pawn. Only the creator does this; the game's own melee
  (`UWeaponDrawSubsystem`, procedural swings) is unchanged.

## Sized to the body

Weapons and the shield follow [gear-size.md](gear-size.md): the grip sits on the body's own hand bones, the per-style
fine-tune offsets scale with its height (`gearSizeFor(...).place`), and the weapon and shield are drawn at
`gearSizeFor(...).size` (half way to the body's height), blades and the shield's reach included.
