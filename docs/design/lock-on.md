# Lock-on, Elden Ring style

sneed, 2026-11-18 (thread "Rebuild lock-on like Elden Ring"), after playing the combat range. Replaces the player-facing
parts of [focus.md](focus.md) (Tab/F1 lock-on, facing, the player's swing); the sim's `FocusCommand`, the fighter AI's
focus, footwork and the HP bar stay. Queued after "Make melee move freely and staggers keep legs" and "Make gun hits
match the aim": this doc is the design; the code waits for those two.

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
  ahead (as `FollowFocus` does now); the mouse tilts it and flicks switch targets.
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
2. **Wind-up tracking**: during the wind-up the body turns toward the focus point at `windupTurnDegPerSec` (720), then
   the turn locks when the strike starts (`strikeTurnDegPerSec` 90), so a dodge at the last moment still works.
3. **Aim**: the swing's line goes at the focus point's height (no camera aim needed). Spear and knife thrusts go straight
   at it; cuts and smashes pass through it in their arc.
4. **Short lunge**: if the focus point is up to `lungeMetres` (0.5 m) beyond reach, the swing's root motion steps in that
   far (through the body motion system, collision swept). Further than that, the blow whiffs.
5. **The hit stays physical**: the strike is the existing swept line and arc against hitboxes (`MeleeSystem::strike`).
   Homing only decides where it is aimed. An arm or shield in the way takes the blow; a head that ducks is missed.

Without a lock, swings go where the camera looks, as now.

### Pawns

The fighter AI already locks a melee fighter on within `aiFocusRange` (8 m). Pawns use the same homing (1 to 5) and the
same movement rules: melee pawns circle with footwork facing the target, gun pawns keep the lock without being turned
by it (they face their aim). The AI's wobble by skill (`aimWobbleBySkill`) stays so skill still matters.

### Spells and abilities (later)

An aimed ability asks `focusPoint(target)` for its aim when the caster is locked on, else the camera's aim. Guns never do
(sneed: free aim).

## Building it

- **Sim (cloud)**: `focusPoint`, wind-up turn rate, lunge, letting go (turn-away and sight timers), "locked but don't
  face" with a gun (`faceFocus` skips a pawn with a gun drawn, not only one aiming), circle-strafe move for the player's
  `MoveCommand` while locked with melee. Doctest tests for each, a scenario test of a locked duel.
- **Unreal (one short PC compile)**: middle-mouse toggle, nearest-to-view-centre pick, mouse-flick switch, F1 freed,
  camera follow only with melee, marker on the focus point (moves to the head with fists). Smoke test `melee` phase
  `focus` extended; checked in the combat range.

It depends on the two fixes queued before it: moving freely while swinging (the root motion change), and staggers with
working legs. Homing rides on the swing motion they rebuild.
