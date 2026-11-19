# Lock-on, Elden Ring style

sneed, 2026-11-18 (thread "Rebuild lock-on like Elden Ring"), after playing the combat range. Replaces the player-facing
parts of [focus.md](focus.md) (Tab/F1 lock-on, facing, the player's swing); the sim's `FocusCommand`, the fighter AI's
focus, footwork and the HP bar stay. Built on "Make melee move freely and staggers keep legs" (#361) and the range follow-up
(#363, middle mouse toggles the lock); merged after them.

Elden Ring was not watched on the PC: the design is from how its lock-on is known to play. All numbers below are ours
(FromSoftware publishes none) and go in `data/defs/Rules/MeleeRules.json`.

## What sneed asked for

- Lock-on that plays like Elden Ring, for the player **and** every pawn.
- **Melee in hand**: the body faces the target, WASD walks round it, and every swing, stab or punch goes to the target's
  **focus point** so melee is easy to land. It only lands if you are actually in reach, and the damage is whatever the
  blow physically hits on the way.
- Focus point: a humanoid's **torso** centre; for **fists**, a humanoid's **head**; for a big creature, the centre of
  its **body part nearest you**.
- **Gun in hand**: the lock stays (marker, HP bar) but the body does not turn to it and the aim is completely free.
- Switching between melee and gun keeps the lock.
- **Turning away and moving away** from the target lets go.
- Not on **F1**: in the editor build F1 is Unreal's wireframe view (the "polygons"), and other F keys are view modes too.
- Later: spells and aimed abilities use the same lock to aim.

## How Elden Ring plays (what we copy)

1. One button toggles the lock (middle mouse / right stick press); the lock picks the enemy nearest the centre of the
   view. Flicking the mouse or stick left or right switches to the next enemy on that side.
2. Locked on, the character walks facing the enemy: forward closes in, back steps away still facing it, left and right
   circle round it. The camera sits behind you looking past you at the enemy.
3. **Sprinting** turns you to run where you press, so you can run off; a roll goes the way you press.
4. An attack **turns toward the enemy during its wind-up** (fast at first, then locked once the blade moves), with a short
   lunge built into the swing. It doesn't teleport you or bend the swing: if the enemy is out of reach the blow whiffs.
5. Large enemies have several lock points (head, legs, body); the lock can move between them.
6. The lock drops when the enemy dies, gets far away, or is out of sight for a few seconds.

## Ours

### Controls

| Key (rebindable) | What |
| --- | --- |
| **Middle mouse** | lock on to the enemy nearest the centre of the view; press again to let go |
| **Tab** | switch to the next enemy (as now) |
| **Mouse flick** while locked with a melee weapon | a quick sideways flick switches to the nearest enemy on that side |
| F1 | nothing by default (editor build: wireframe) |

`EndFocus` becomes `LockOnToggle` (default `MiddleMouseButton`); `FocusNextTarget` stays on Tab. Old saved key files
that had EndFocus on F1 move to the new default.

### Facing and movement

- **Melee weapon or fists in hand**: the sim turns the body to the target (as `MeleeSystem::faceFocus` does now). WASD is
  read **around the target**: W toward it, S straight back, A/D walk a circle round it at the current distance (the move
  direction is the tangent, corrected each step so the distance holds). The third-person camera turns to keep the target
  ahead (as `FollowFocus` does now); the mouse tilts it and flicks switch targets. The turn is eased, not a fixed
  spin: it closes its gap with a 0.08 s time constant, capped at 360 degrees a second (100 in first person). After a
  roll the cap starts at a tenth and eases up to full over 0.5 s, so the view swings back to the enemy gently instead of
  whipping round (sneed 2026-11-19).
- **Gun in hand**: no auto-face, no camera pull. WASD and the mouse work exactly as unlocked. The marker and HP bar stay.
- **Sprint while locked**: the body turns to the run direction (as Elden Ring), facing returns when sprint stops.
- **Roll**: goes the way you press, relative to the target when locked with melee.

### Letting go

The lock drops when any of these hold:

- the target dies, leaves, changes level, or is beyond `focusBreakRange` (25 m, as now);
- it has been out of sight for `lockSightLostSeconds` (3 s);
- **turned and moving away** (sneed's rule): for `lockTurnAwaySeconds` (0.75 s) you are moving away from it (more than
  `lockTurnAwayMetresPerSecond` 1.5 m/s of your speed is away from it) **and** your body or camera faces more than 100
  degrees off it. With melee that happens when you sprint away; with a gun when you turn the view and walk off.
  Backing away while still facing it (S with melee) keeps the lock;
- Middle mouse again.

Switching weapon never drops it.

### Melee homing (the big one)

When a swing, stab or punch starts while locked on (player or pawn):

1. **Focus point** (`MeleeSystem::focusPoint`): humanoid torso centre (the chest hitbox); with fists, the head hitbox;
   for a creature of body size over `lockBigBodySize` (2.0, a bear and up), the centre of its hitbox nearest the
   attacker, picked fresh at each swing. Animals without hitboxes yet: their body centre as now.
2. **Wind-up tracking**: during the wind-up the body follows the target, and the strike's line is fixed `commitSteps`
   (0.2 s) before the blow (as built for real-time combat), so a dodge at the last moment still works.
3. **Aim**: the swing's line goes at the focus point's height (no camera aim needed). Spear and knife thrusts go straight
   at it; cuts and smashes pass through it in their arc.
4. **Short lunge**: the swing's own step forward already counts in its reach (`engageDistance` adds the attack clips'
   `lunge`, and the player's free-feet swing reaches that far, #361). Beyond that the blow whiffs. No extra number.
5. **The hit stays physical**: the strike is the existing swept line and arc against hitboxes (`MeleeSystem::strike`).
   Homing only decides where it is aimed. An arm or shield in the way takes the blow; a head that ducks is missed.

Without a lock, swings go where the camera looks, as now.

### Pawns

sneed (2026-11-18, after approving the plan): the AI's lock-on is mainly for melee fighters; every other AI rule stays
(fleeing, cover when shot at, weapon swaps by range). Added with it:

- **The nearer threat first**: a fighter without a gun counts an enemy swinging at it, or within reach of it while going
  for it, `meleeThreatMetres` (4 m) nearer when it picks its target (`FighterSystem::threatensInMelee`), and a new enemy
  winding up at it, nearer than its target, makes it think again at once rather than at its next plan
  (`attackedByOther`). It doesn't let one man cut it down while it goes for another further off.
- **Gunmen backpedal**: an enemy without a gun within `backpedalRange` (5 m) that it knows of: between shots the gunman
  steps back `backpedalMetres` (0.8 m in 0.5 s) away from it through the body motion system, and fires at it from where
  it stands as soon as its gun is ready (`FighterSystem::backpedal`). Closer than `switchMeleeRange` (2.5 m) it takes up a
  melee weapon if it carries one, as before.
- Smarter squad play (pinning and flanking, call-outs) is a later thread.

The fighter AI already locks a melee fighter on within `aiFocusRange` (8 m). Pawns use the same homing (1 to 5) and the
same movement rules: melee pawns circle with footwork facing the target, gun pawns keep the lock without being turned
by it (they face their aim). The AI's wobble by skill (`aimWobbleBySkill`) stays so skill still matters.

### Spells and abilities (later)

An aimed ability asks `focusPoint(target)` for its aim when the caster is locked on, else the camera's aim. Guns never do
(sneed: free aim).

## As built (2026-11-18)

- Sim: `MeleeSystem::focusPoint` (used by every swing at a standing target, player or AI), `orbitMove` (DirectMove while
  locked with melee: a step whose part toward or away from the target is under `orbitRadialShare` (35%) of it is put back
  on the circle), `faceFocus` skips a pawn with a gun in hand and lets the player's lock go after `lockSightLostSteps` (90)
  without line of sight (melee save section 5: `focusSeenAt`). Tests: `LockOnTests.cpp`.
- Unreal (not compiled yet): `UProtagonistSubsystem::UpdateTurnAway` (turned away and moving away at over 1.5 m/s for
  0.75 s lets go: with a gun the view more than 100 degrees off, with melee sprinting), `AProtagonistPawn::FollowFocus`
  doesn't pull the view with a gun in hand or while sprinting.
- Not yet: the mouse-flick target switch, the marker moving to the head with fists, nearest-to-view-centre first pick
  (Tab's nearest-in-front pick is used), and the body turning to run while sprinting locked on (the lock lets go after
  0.75 s instead).

## Tab, flicks and landing blows (sneed 2026-11-19, after the gun range)

sneed: "tab doesn't shuffle or work intuitively to target closest", "when I'm swinging at someone it doesn't always hit,
and they don't dodge it or anything", swing speeds up (club x2, the rest +50%), a spear or knife at a downed target stabs
down into the body.

- **Why Tab felt stuck**: it took the one after the held target in a list sorted by distance at each press. Walking to
  the second nearest made it the nearest, so Tab swapped between the two nearest for ever. **Now a round**: each Tab
  within `focusRoundSteps` (5 s) of the last gives the nearest enemy not given yet (standing ones before downed ones,
  those in view before those behind), then round again from the nearest. After 5 s without Tab a new round starts
  from the nearest other than the one held. "In view" is where the **camera** looks (FocusCommand `viewX/viewY`), not
  where the body faces.
- **Mouse flick** (Unreal `UProtagonistSubsystem::UpdateFlick`): locked on with melee or fists, a quick sideways mouse
  move (`LockOn.FlickMouse`, 40 units within 0.15 s, 0.4 s between flicks) sends FocusMode `Left`/`Right`: the enemy
  with the smallest turn that way from the one held (within `flickMaxDegrees` 150). Not with a gun (free aim).
- **The practice range** picks its own-side targets through FocusCommand `anySide` (it used to pick them itself, Tab
  only, so middle mouse found nobody there).
- **Faster swings**: `data/tuning.json` cooldown multipliers (RimWorld's numbers stay in the defs): club 0.5, knife
  0.222 (x3 then +50%), longsword, breach axe, spear and monosword 0.333 (x2 then +50%), gladius, mace, plasma sword and
  zeushammer 0.667. The wind-up is `windupShare` of it (at least `minWindup`, 0.3 s) and the clip plays to fit.
- **Why swings whiffed**: (1) a swing started with the lock just out of reach went straight ahead at nobody; (2) the
  line fixed 0.2 s before the blow missed a target that only shuffled (footwork) without dodging. Now the player's swing
  takes its lock up to `playerReachSlack` (0.6 m) past its reach (it walks on under it; the blow is still only as long as
  the reach), else the nearest enemy within `assistCos` of ahead, else whoever walked into it by the commit. A target
  that didn't dodge is followed to the blow if it moved no more than `strikeTrackDegrees` (35) off the line.
- **Why nobody dodged**: the faster swings wind up in 9 steps; reading one took 14 steps at skill 0 and 5 at skill 20
  (plus or minus 4), so most were too quick to read. Reaction is now `reactionStepsBySkill` 9 to 3 (plus or minus 3),
  and a swing read in time is defended `defendChanceBySkill` (35% at skill 0, 85% at 20): with a weapon a parry
  (`parryShare` 60% for a fighter), else (and the rest) a roll from a heavy blow, a **step back** out of reach
  (`backstepShare` 40%, 1.3 m in 8 steps, clear of the attacker's own step in) or a sidestep. A dodge read in time keeps the fixed line, so it gets away.
- **Blows at the downed**: the sim already picked the downward stab (`Motion_Spear_Stab_Low`, `Motion_Knife_Stab_Low`)
  for a swing at a downed target; the PC's range build lacked those clips and played the standing one. In first person
  the hands now drive down into the body for a low blow instead of straight ahead.

## The original build plan

- **Sim (cloud)**: `focusPoint`, wind-up turn rate, lunge, letting go (turn-away and sight timers), "locked but don't
  face" with a gun (`faceFocus` skips a pawn with a gun drawn, not only one aiming), circle-strafe move for the player's
  `MoveCommand` while locked with melee. Doctest tests for each, a scenario test of a locked duel.
- **Unreal (one short PC compile)**: middle-mouse toggle, nearest-to-view-centre pick, mouse-flick switch, F1 freed,
  camera follow only with melee, marker on the focus point (moves to the head with fists). Smoke test `melee` phase
  `focus` extended; checked in the combat range.

It depends on the two fixes queued before it: moving freely while swinging (the root motion change), and staggers with
working legs. Homing rides on the swing motion they rebuild.
