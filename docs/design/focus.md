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

1. **Back out of a blow it can't stop**: the enemy is winding up at it while it is recovering from its own swing (after
   the punish window right after the swing, which stays open) or its guard is broken: sometimes (`backShare`, half the
   footwork chance) a step back of `backMetres` (0.6 m in 0.4 s). Stepping back every time made three-on-three fights
   last three times as long.
2. **Too close** (under `crowdedShare` of its engage distance): a step back to it.
3. Otherwise **maybe a step round** the enemy (`circleMetres` 0.6 m in 0.6 s), pulled to the distance it fights at, the same
   way as last time or the other way (30%), never into a friend's line of fire at the enemy (and not at all while a friend
   is aiming at it). The chance is `circleShare` (0.6) of the footwork chance: 35% at Melee 0 to 80% at Melee 20, +20%
   Nimble, half for a reckless side (raiders press), times its Moving (a hurt leg keeps it still).

A gunman whose friend steps into its line while it aims ends the shot and lines up again.

Moves go through the body motion system (`push`, collision sweeps, never through walls or bodies), and it still swings the
moment it is ready: footwork never delays a blow. Snapshot `footwork` is true while it steps (Unreal plays a strafe or back
step from the velocity, facing the focus).

**Following an enemy that runs**: a fighter that swung at an enemy within `chaseSeconds` (6 s) follows it even when its
side otherwise holds back for friends (the colonists' ambush rule). Raiders always charged.

## Sprinting and the focus HP bar (sim)

sneed (later the same day): enemies that flee sprint, and enemies running in to melee sprint. `FighterSystem::sprinting`:
an AI fighter walking a path sprints (`Sim::pawnSprinting`, at RimWorld's 4/3 `sprintSpeedFactor`) while it is routed, heading for
shelter from enemies it knows of, falling back or fleeing by its response with enemies known, or charging in to melee
with no gun at a target between `sprintChargeMin` (3 m) and `sprintChargeMax` (30 m). Never the player's pawn (its keys
sprint). Snapshot `sprinting`.

The HP bar top-middle of the screen for the focused target: snapshot `lowestPart`, the health of its worst body part 0
to 1 (a body zone for people, its most hurt part for animals), filled for pawns the player is focused on and the
player's own pawn.

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

## Unreal (built on the controls thread's key list)

Built (2026-11-18):

- **Keys** (`UProtagonistSubsystem::UpdateFocus`): in direct control, `FocusNextTarget` (Tab) sends `FocusCommand Next`,
  `EndFocus` (F1) sends `Clear`; nothing with Settings > Controls "Lock-on focus" off, and turning it off while locked on
  lets go. While locked on F1 does not toggle the key help (`USimSubsystem::HandleKey`). The overseer switch is O.
- **HP bar** top middle, under the clock (`DrawFocus`): the target's name (`Sim::feedName`: a colonist's or raider's name, a named animal's, a spawned pawn's `PawnLooks::name`, else its kind) and a red bar of
  `lowestPart` with the percentage.
- **Marker**: a small dot on the target's chest (low over a body on the ground), `GetFocusPoint`.
- **Camera** (`AProtagonistPawn::FollowFocus`): the view turns to keep the focus ahead, 360 degrees a second in third
  person (the mouse only tilts it), 100 in first person (the mouse still turns, the focus pulls back). Not while aiming a
  gun or rolling.
- **Bodies**: every focused pawn's actor faces the sim's facing while it moves (`ASimPawnActor`), so it strafes and backs
  off facing its enemy; the walk blends play from the velocity.
- Smoke test `melee`, phase `focus`.

Still to do: pain and angry faces, downward attack clips for `meleeLow`, a strafe check by eye.
