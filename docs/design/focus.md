# Focus, footwork and fight responses

sneed, 2026-11-17 (thread "Add focus lock and smart melee AI"). Builds on [real-time-combat.md](real-time-combat.md),
[melee.md](melee.md), [fighters.md](fighters.md) and [body-motion.md](body-motion.md). All numbers are ours (Dark Souls
and WoW have none to copy) and live in `data/defs/Rules/MeleeRules.json` and `FighterRules.json`.

## What sneed asked for

- **Focus like Dark Souls**: pawns face the enemy they fight. The player locks on in third person with **Tab**, which
  cycles to the next nearest target like WoW; **F1** ends focus; a game setting turns lock-on off. Focus makes melee
  easier: the swing goes at the centre of the focused body.
- **Smart melee movement**: fighters don't stand still. They follow an enemy that runs and move round a bit between
  blows without hurting their attack, varying with who they are.
- **Blows at the downed** aim down at the body (poke, smash, hack).
- **Faces**: a brief pain face when hit; an angry face while wielding a melee weapon in a fight.
- **Fight responses**: a pawn set to flee does so as soon as it spots an enemy; the combat range has some that flee when
  attacked, some on sight, some that don't, some that fight back with fists.

Stealth (what a pawn can see or hear) is the "Add stealth, sight and alerts" thread's. Fleeing on sight uses whatever the
fighter knows (`Fighter::known`), so it follows that work automatically.

## Focus (sim)

State is on the `Melee` component (melee save section 4): `focus`, `focusPlayer`.

- **FocusCommand** `{pawn, target, mode}`:
  - `Set`: lock on to `target`, any living pawn within `focusBreakRange` (25 m).
  - `Next` (Tab): the pawns within `focusRange` (20 m), never its own side; enemies first (another side that isn't a
    visitor or prisoner, or a mad animal), and only others if there are no enemies; nearest first, with pawns more than
    60 degrees off its facing counted `focusBehindFactor` (2x) as far. Picks the one after the current focus, round again.
  - `Clear` (F1): let go. Fails if not focused.
- A focus lets go by itself when the target dies, leaves, changes level or goes beyond 25 m (the AI's beyond
  `aiFocusRange` + 2 m), or the pawn is downed. A downed target keeps the focus (to finish it).
- **Facing** (`MeleeSystem::faceFocus`, after the combat step): every focused pawn faces its focus, so it walks sideways
  or backwards facing it (strafing). Not during a roll, nor while aiming a gun (it faces its aim).
- **The fighter AI** locks a fighter with no gun on to its target when it is within `aiFocusRange` (8 m), in Fight mode,
  not falling back or fleeing; it lets go otherwise or when it stands down. Never over a focus the player set.
- **The player's swing** (`MeleeCommand` with no target, `MeleeSystem::playerTarget`): the focus if it is in reach, so the
  blow goes at its centre whichever way the camera looks; else a body on the ground in reach within 60 degrees ahead; else
  straight ahead as before.

The lock-on **setting** is Unreal-side: with it off, Tab and F1 send nothing (the controls thread adds the keys and the
checkbox: `FocusNextTarget`, `EndFocus`, `Rim3DKeys::IsLockOnFocusEnabled()`).

Snapshot: `focusTarget`, `focusPlayer` (the lock-on camera follows it, a marker on the target).

## Footwork (sim)

`MeleeSystem::footwork`, called by the fighter AI's melee step when its victim is in reach and it is not swinging: between
its own blows, or while it waits out a raised guard. At most one choice every `footworkEverySteps` (0.8 s):

1. **Back out of a blow it can't stop**: the enemy is winding up at it while it is recovering from its own swing, just
   sidestepped, or its guard is broken: a step back of `backMetres` (0.6 m in 0.4 s).
2. **Too close** (under `crowdedShare` of its engage distance): a step back to it.
3. Otherwise **maybe a step round** the enemy (`circleMetres` 0.6 m in 0.6 s), pulled to the distance it fights at, the same
   way as last time or the other way (30%). The chance: 35% at Melee 0 to 80% at Melee 20, +20% Nimble, half for a
   reckless side (raiders press), times its Moving (a hurt leg keeps it still).

Moves go through the body motion system (`push`, collision sweeps, never through walls or bodies), and it still swings the
moment it is ready: footwork never delays a blow. Snapshot `footwork` is true while it steps (Unreal plays a strafe or back
step from the velocity, facing the focus).

**Following an enemy that runs**: a fighter that swung at an enemy within `chaseSeconds` (6 s) follows it even when its
side otherwise holds back for friends (the colonists' ambush rule). Raiders always charged.

## Blows at the downed (sim)

A swing at a downed or prone target sets `Melee::low` (snapshot `meleeLow` while the swing is under way). The strike already
aims at the body's aim point on the ground; Unreal plays a downward blow (a stab down for knives and spears, an overhead
smash for clubs and hammers, a hack for blades and axes).

## Fight responses (sim)

`SetFightResponseCommand {pawn, response}`, kept on `Melee::response`:

| Response | What it does |
| --- | --- |
| `Attack` | as before (the default) |
| `Fists` | charges like a brawler, with its fists if unarmed; a working colonist hit by an enemy fights back unarmed |
| `Flee` | hurt by an enemy, it runs (falls back away from enemies) for `fleeResponseSeconds` (20 s), never swings or shoots meanwhile; a working colonist runs for shelter even armed |
| `FleeOnSight` | as Flee, and also whenever it sees an enemy within `fleeSightRange` (40 m), renewed while it keeps seeing one |

The arm's-length "stand and fight" rule does not hold a fleeing pawn. Snapshot `fightResponse`.

## Unreal (PC step, after the controls thread merges)

- Tab sends `FocusCommand Next`, F1 `Clear`, behind the lock-on setting. Third-person lock-on camera: keeps the player and
  the focus in frame; a small marker on the focused body. First person: the view turns toward the focus gently.
- The player's mesh turns to the sim's facing while locked on (strafe animation from velocity).
- Faces: a 0.6 s pain expression on every hit (`ProjectileHit`/`MeleeHit` events), an angry expression while the pawn
  is a fighter in a fight with a melee weapon (or fists) in hand; both on the MakeHuman expression shape keys.
- Downward attack clips for `meleeLow` (Mixamo has stabbing-down and overhead clips; new mocap if they look wrong).
- Footwork: strafe/back-step locomotion from velocity while `footwork`.
