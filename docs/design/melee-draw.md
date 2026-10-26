# Melee and weapon switching on screen

Code: `game/Source/Rim3D/WeaponDrawSubsystem.*` (weapons on the body, the player's keys, the events),
`MeleePose.h` (the swing as key poses), `PawnActor.*` (the body's lean, twist, lunge, hit-stop, dodge),
`FightSubsystem.*` (the blood and sparks of a blow, `SpawnMeleeImpact`). Data: `visual.parts` on the melee
weapons (`Things/MeleeWeapons.json`). Smoke test: `melee` (`tools\smoke.ps1 -Test melee`). Task U22. Unreal
side only; the sim is untouched. It stands on melee ([melee.md](melee.md)), several weapons
([weapons.md](weapons.md)), fights on screen ([fights.md](fights.md)) and direct control
([protagonist.md](protagonist.md)).

**The sim decides, Unreal shows.** Every swing, hit, block, dodge and switch is timed by the sim; this
draws it. A bug in these pictures cannot change a fight. The one way the player affects a fight is a
command (`MeleeCommand`, `SwitchWeaponCommand`), like any pawn. `Weapons.Draw 0` turns the weapons and the
swings off; `Fight.Effects 0` turns the blood and sparks off.

## No human melee animations yet

The project has no human swing, block or dodge animation (the A-series art has only animal attacks). So
a swing is **procedural**: a short track of key poses for the weapon and the body, played over the time the
sim gives it. The mannequin's arms do not move (the weapon floats beside the hand, as the gun did in
[fights.md](fights.md)); the body leans, twists and lunges, and the weapon swings. When real animation
packs arrive, the timing here (below) is what they have to be cut to.

## The swing, timed to the sim

`MeleeSwing` gives the steps to the hit (`amount`: the wind-up, 30% of the weapon's cooldown). The
pawn actor runs a clock in sim steps (real time x game speed x 30) through `FMeleeSwingTrack`:

| Phase | Lasts | Weapon and body |
| --- | --- | --- |
| Draw back | the wind-up less 6 steps | rest to the wound-up pose (blade raised behind the head, body leaning back and twisted away, or a spear pulled back) |
| Strike | 6 steps (the sim's `commitSteps`) | speeds up into the blow (blade down and forward, body lunging 16 to 22 cm and leaning 14 to 16 degrees forward) |
| Wait | until the sim's `MeleeHit` | the clock stops at the blow: it never lands before the sim says so |
| Follow through | 5 steps | the blow is held and carried a little further |
| Recover | 14 steps | back to rest |

`MeleeHit` lands the blow at once if the picture is still short of it. At 1x the picture is on time
(the smoke test counts the blows that arrived with the picture within 2.5 steps of landing: all of them).
Styles: **Chop** (sword, club, mace, axe), **Thrust** (spear, knife), **Punch** (fists, teeth: no weapon,
the body lunges). A swing the sim no longer has (the attacker went down) is recovered from after a quarter
of a second.

## What a blow does on screen

| Sim says (`MeleeHit.message`) | On screen |
| --- | --- |
| `hit` | blood spray and wound decals at the sim's hit point, the struck pawn flinches (the U11 lean, stronger for more damage), the hit marker flashes for the player, **hit-stop** |
| `deflected` | sparks, a small flinch, a short hit-stop |
| `blocked` | sparks where the blow met the weapon, the blocker staggers, a longer hit-stop |
| `dodged`, `missed` | nothing (a miss that hit a building makes chips and sparks) |

**Hit-stop**: for 0.05 s plus 0.004 s a point of damage (0.12 s at most; 0.04 s deflected, 0.08 s blocked)
of *real* time both pawns freeze: the swing clock, the animation and the flinch hold, and then go on. It does
not stop the sim, and it is skipped while the game is paused.

## Blocks and dodges

- **Block**: while the snapshot says `blocking`, the weapon comes up across the chest in about an eighth of
  a second and stays while it does. Both the AI's blocks and the player's use the same flag.
- **Dodge**: the sim walks the pawn one cell aside (`PawnAction::Dodge`); the pawn actor follows the
  sim's position and adds a dip and a tip of the body toward the step for a third of a second.
- Pawns that swing, block or are mid-swing turn to the sim's facing, as shooters do.

## Weapons on the body and the switch

Every weapon a pawn carries (`ThingView::carried`) is drawn, not just the one in the hands:

| Slot | Where it rides |
| --- | --- |
| Primary (rifle) | slung across the back, muzzle up over the shoulder |
| Sidearm | on the right hip, pointing down, at 55% of the rifle's size (the gun defs share one shape) |
| Melee | on the left hip, hanging back and down |

A weapon switch (`SwitchWeaponCommand`) is shown over the steps the sim takes (`switchStepsLeft`): in the
first half the weapon in the hands goes to its holster, in the second the new one comes up to the hand.
The sim swaps them at the end, so the picture is already there. The old gun-at-the-chest pose (and the
camera pitch for the protagonist's gun) is unchanged.

Melee weapons now have shapes in their defs (`visual.parts`, as guns have): grip at the origin, the weapon
along +x.

## The player's keys (direct mode)

| Key | Does |
| --- | --- |
| left mouse button | a gun in the hands fires (as before); a melee weapon or no weapon swings |
| right mouse button | holds the melee weapon up to block (`MeleeCommand` Block, again each time it runs out) |
| Q | brings up the next carried weapon: rifle, sidearm, melee weapon, rifle... (`SwitchWeaponCommand`) |

The swing goes at the nearest enemy pawn within reach that is in front of the colonist (within 70 degrees of
where the camera looks; another side only), else straight ahead along the colonist's facing: the sim has no
command for a direction, so with no enemy in front the blow goes where the colonist last walked or aimed.
One order a step, only when the sim says the colonist is ready (no swing, switch or reload in progress). With
a building in hand ([handson.md](handson.md)) both mouse buttons and Q belong to the build tool.
The HUD shows the weapon in hand, the keys for it and a line of what else is carried, the one in hand in
brackets.

## Smoke test `melee` (about 70 s)

On a 128 m flat map with the Crashlanded start, a rifle, a revolver and a gladius on the ground by the
protagonist:

1. `setup`: the sim, the weapons, the protagonist in direct mode, nothing carried or drawn.
2. `holsters`: all three given by command: the rifle is in the hands, the revolver and gladius on the body,
   three weapons drawn, each where the table says.
3. `switch_key`: Q brings up the revolver: one switch started and finished, the rifle went to the back and
   the revolver to the hand (both moved more than 20 cm on the way), the swing key does nothing with a gun in hand.
4. `switch_frozen`: a switch back to the rifle stopped at three quarters: the rifle is part way up from the back.
5. `draw_melee`: the gladius comes to the hand and rests at the side; the fire button does nothing with it.
6. `swing_windup`: an enemy one metre ahead; the swing key picks it as the target; the game stops near the top
   of the wind-up: the blade is above 135 cm and the body leans back.
7. `swing_blow`: the game stops just after the hit moment: the blade is low and forward, the body leans forward,
   hit-stop is on both pawns, there is blood and the struck pawn flinched.
8. `swing_flow`: nine seconds of swings at normal speed: every blow is on the picture within 2.5 steps of the
   sim's hit moment, every hit has a hit-stop, blood and a flinch, the body bends 20 degrees or more.
9. `block`: the right mouse button: the blade is across the chest (y near 0, turned 90 degrees) and the sim
   reported the raised weapon.
10. `duel`: two fighters with a club and a gladius (Melee 20) swing, sidestep and block at three times speed for
    26 s: at least eight swings, a sidestep both reported and played, a block raised to the full.

Screenshots of each phase are in `game/Saved/Smoke/melee/`. The keyboard and mouse themselves are not covered
(the test drives the same methods): a person has to try the left and right mouse buttons and Q once.

## Not done here

- **Real animation.** The arms do not swing, the hands do not hold the weapon, the feet do not step; no
  melee animation packs yet. Hit-stop does not slow the camera.
- **Sounds, weapon trails and wound decals that fit the weapon** (a cut and a bruise look the same).
- **A direction for the player's swing** with nobody in front (needs a sim command).
- **Animals** swing with their own attack animations when U15's animals come in; here only humanlike pawns.
- First person: the body is hidden, so the swing shows only as the blood and sparks (and the weapons are hidden too).
