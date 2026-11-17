# Body motion: clips that really move the body

sneed (2026-11-16): "when an animation occurs it restores the pawn's location before it started ... it is critical
this does not happen. When an animation occurs it physically moves its body in real time with real collisions, so it
can't clip stuff and it actually moves." This is the system for that, for every pawn and animal.

Code: `ColonySim/BodyMotion.h`, `Private/BodyMotion.cpp` (the system, owned by `CombatSystem::motion()` and stepped
with combat). Tests: `Tests/BodyMotionTests.cpp`. Data: `data/defs/MotionClips/` (one def per clip, made on the PC),
`Rules/MotionRules.json`, the style lists in `Rules/MeleeRules.json`.

## Why bodies snapped back

The sim decides where a pawn is; Unreal draws it there and plays animations on top with root motion dropped
([pawns.md](pawns.md)). Our Mixamo clips carry their travel in the hips, not in a root bone. So a stagger or a roll
moved the drawn body away from the pawn, and when the clip blended out the body came back to the pawn, which had
never moved. The creator's fight demo did the same with its own offsets.

## The rule

**The sim moves the body along the clip's own path. Nothing remembers where the clip started.**

1. **The path is data.** A `MotionClips` def holds a clip's root path: at 30 samples a second, how far the body has
   gone forward and to its right (metres, for a 1.80 m man) and how far it has turned right (degrees), all in the
   frame the body faced when the clip began. `hitSeconds` marks an attack's blow, `keepWhenDown` lets a knockdown go
   on while the pawn is down. The `MotionClips` commandlet on the PC measures this from the very animation Unreal
   plays (pelvis travel on the ground, moved into the root bone so the clip plays in place) and writes the def.
2. **A pawn in a clip has a `BodyMotion`.** Each step the system takes the next stretch of the path, scales it by
   the body's height, turns it into the world by the facing the clip began with, and **sweeps** the body along it.
3. **The sweep** (`BodyMotionSystem::sweep`) moves in pieces of 5 cm:
   - **Ground it can't stand on** (walls, rock, cliffs, deep water, the map's edge): the piece is tried whole, then
     along each axis (sliding along the wall), else the body stops. Between walls a body is as wide as the pathfinder
     thinks (0.3 m radius), so a clip can never leave a body where no path gets it out.
   - **Other standing bodies**: a body's radius is 0.25 m times the root of its body size (a man 0.25, an elephant
     0.5). Where two overlap, the other is shoved out by the share its weight allows
     (`mine / (mine + theirs)`, by body size) as far as its own ground lets it, and the mover is pushed back by the
     rest. So a man shoves a man half way, bounces off an elephant, and an elephant walks a man backwards. Bodies on
     the ground (downed, knocked down, prone) are stepped over.
   - Motion lost against something is **lost**: the body doesn't catch up later.
4. **When the clip ends the component is removed.** The body is where the sweep left it.
5. While a clip has the body, steering and the player's walking are ignored (`MovementSystem`, `DirectMoveCommand`).

Unreal reads `motionClip`, `motionTime` and `motionRate` from the snapshot and plays that clip at
that time, root motion dropped. The actor follows the sim, which follows the same path, so feet stay planted. If the
sim was stopped by a wall the body stops there and the clip plays on against it.

## What uses it

| Move | Clip (MotionRules) | Without a clip def |
| --- | --- | --- |
| Dodge roll (`CombatSystem::dodgeRoll`) | `rollClip`, stretched over start-up + roll + recovery | a push of `dodgeRoll.distance` over the rolling steps |
| Sidestep (`MeleeSystem::sidestep`) | `sidestepClip.left/right` | a push of `sidestepMetres` (0.7) over `sidestepSteps` (9) |
| Stagger by a blow by hand (`CombatSystem::stagger`, `HitFrom::blow`) | `staggerClip.back/forward/left/right`, by the way the blow throws the body seen from the body (the measured ones stumble about 0.9 to 1.2 m) | a push of `staggerMetres` (0.5) away from it |
| Stagger by a bullet | `flinchClip.*` (none measured yet: a bullet flinches on the spot, it doesn't send a body stumbling a metre) | nothing |
| Knockdown by poise (`CombatSystem::jolt`) | `knockdownClip.*` | a push of `knockdownMetres` (1.0) |
| Blast (`CombatSystem::explode`) | none | a push of the blast's knockback over `blastSteps` (12) |
| Melee swing (`MeleeSystem::attack`) | one of the weapon style's attack clips | no body movement |

Animals have no human clips: they always get the plain push, shorter the bigger they are (divided by the root of
their body size). Until the PC step has written the clip defs, people get the plain pushes too.

## Fast melee

sneed: "all attacks should be fast, vicious, and sped up for higher speeds."

- A weapon has a **style** (`MeleeRules.weaponStyle`: Club, OneHand, TwoHand; fists are Unarmed) and a style has a
  list of attack clips (`styleAttacks`).
- A clip is **left out** if it is longer than `maxAttackClipSeconds` (1.6), its blow comes later than
  `maxAttackHitSeconds` (0.8), or it carries the body further than `maxAttackTravel` (0.8 m: a running charge like
  the 2.4 m sword lunge would run past the target). The six-second club flourish and the spins can never be picked in a fight.
- A swing picks one of the remaining clips (by the swing's seed) and plays it so that the clip's blow lands exactly
  on the sim's hit step (`playTimed`). The sim's wind-up already shortens with Melee skill, traits and light armour,
  so a faster fighter plays the same clip faster; the rest of the clip runs at the same rate and is over well inside
  the recovery.
- The clip's step forward moves the body through the sweep, so a lunge into a wall or a body stops there.

## How close fighters stand

sneed: "pawns need to be closer depending on size and weapon ... automatically."

`MeleeSystem::engageDistance(pawn, target)` is the distance between centres from which a swing lands:

- the weapon's reach (`weaponReach`, or the body's), scaled by the attacker's height (`reachHeightFollow` 0.5: the
  arm grows with the body, the weapon doesn't);
- plus how much a big target's body sticks out past a man's (its body radius minus a man's): a fighter stops at an
  elephant's flank, not a reach away from its middle;
- plus the least step forward its attack clips take before the blow.

`inReach` and the charging fighter's stopping distance use it. The blow itself is still a traced line (and, for
cuts and blunt blows, its arc of ±30°) from the body as it stands at the hit moment, so reach stays physical.

## Left and right

A path's "right" is the pawn's own right as the game draws it. Unreal looks down on the map with +y to the right of
+x, so the right of a facing (x, y) is (-y, x). (The gun kick's "side" in `Combat.cpp` uses the other sense.)

## A side effect fixed: two hurt sides that both backed off

Bodies thrown about changed how fights unfold, and one test fight ended with all six fighters badly hurt, all
falling back, and nobody moving again. `FighterSystem` now lets a badly hurt fighter back off only from enemies
that are pressing it (an enemy that is itself falling back doesn't count), or to patch a bad bleed.

## Deterministic

Paths are plain sums and linear interpolation. Turning uses a polynomial sine and cosine written out in the code
(`motionSinCos`), not the C library's, so Linux and Windows give the same bits. Bodies are gathered and moved in
entity order. `BodyMotion` is saved (section `motion`) and hashed.

## What the tests guard

`BodyMotionTests.cpp`: a clip takes the body exactly along its path and for ten seconds afterwards nothing brings it
back; rolls in all eight directions end 4 m away and stay; walls stop bodies and no step ever leaves a body inside
one; bodies never pass through each other (equal bodies shoved, heavy ones stop you); staggers throw the body away
from the blow and it stays thrown; a swing plays a short clip timed to the hit and never the long one; engage
distance follows height, weapon and target size; a motion survives save and load.

## The measured clips (PC, 2026-11-17)

`assets/motion_clips.json` lists 23 Mixamo clips with their trims; `-run=MotionClips` wrote
`data/defs/MotionClips/MotionClips.json` from them (`-List` and `-Trace=A_Name` show travel and hand speed, which is
how the trims and hit times were chosen). Roll 3.6 m in 1.55 s; sidesteps about 1 m in 0.4 s; staggers 0.9 to 1.2 m;
knockdowns (death falls that end lying) about 1 m; club, one-hand and two-hand attacks of 1.0 to 1.5 s with the blow
at 0.47 to 0.63 s. No clean punch clip exists yet, so fists swing without a clip.

The dodge roll's distance now comes from its clip (3.6 m for a 1.80 m body), not from `dodgeRoll.distance`, which
only the plain push uses; the clip is spread over start-up, roll and recovery, so the body leans in during start-up.

Unreal (`ASimPawnActor::UpdateMotionClip`): plays the clip as a montage at the sim's `motionTime` and sets the mesh
back by the drawn pelvis's own travel (read from the animation, times the montage's blend weight), so the hips sit on
the sim position through blend-in and blend-out. The actor keeps the sim's facing while a clip plays. The smoke test
`motion` (`tools\smoke.ps1 -Test motion`) measures pelvis against actor every frame: within 4 to 7 cm, no pop at
the end.

## Next

- **PC:** weapons in the hand follow `hand_r` during a clip (now they stay in the ready pose while the arms swing)
  and holstered guns follow the spine; flinch clips for bullets; punch clips; separate get-ups for face-up and
  face-down; the creator's fight demo moving its people the same way.
- **World:** done in the sim ([plant-bodies.md](plant-bodies.md)): tree trunks as solid posts, plants slowing the walk a little, loose props shoved by weight. Foliage bending round bodies is the PC part left.
