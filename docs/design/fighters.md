# Fighters: where to stand, whom to shoot, when to fall back

Code: `ColonySim/Fighters.h`, `Private/Fighters.cpp`. Tests: `Tests/FighterTests.cpp`, the fighter
commands in `Tests/SaveTests.cpp`. Data: `data/defs/Rules/FighterRules.json`. Task F4. It stands on
guns and bullets ([combat.md](combat.md)), cover and line of sight ([cover.md](cover.md)), health
([health.md](health.md)) and the job system ([jobs.md](jobs.md)).

A **fighter** is a pawn that fights instead of working: a colonist the player drafted, or any pawn
of another faction. Every half real second it **re-plans**: it looks for enemies, picks a target,
scores the cells around it and picks a spot. Every step it **acts** on that plan: walks to the
spot, crouches there, and when its gun is ready stands up or steps out to shoot, then ducks back.
Aiming itself is still F2's (warm-up, spread from skill); reaction time, leading and reloading are
F5. Squads, morale, raids and the colony alarm are F6.

## Sides

`Faction{id}` on a pawn says its side; a pawn without one is the player's (`kPlayerFaction`, 1).
Different sides are enemies; there are no allies or relations yet (F6 brings the raider faction).
Until then `SetFactionCommand{pawn, faction}` is the dev way to make an enemy: a pawn put on another
side gets a `Fighter` at once (it advances on its enemies) and its job ends; put back on side 1 it
works again.

## Orders (RimWorld's draft, move and attack)

| Command | Does |
| --- | --- |
| `DraftCommand{pawn, true}` | the job ends ("interrupted: drafted"), the job system leaves the pawn alone, the fighter AI takes it. It stays within 10 m of where it was drafted (`draftLeash`) and picks cover there |
| `DraftCommand{pawn, false}` | back to work: the fighter state goes, the pawn stands up |
| `MoveOrderCommand{pawn, cell}` | go there and hold: the spot is the best cell within 2 m of it (`holdRadius`), so "go to that wall" ends behind the wall, not in front of it |
| `AttackOrderCommand{pawn, target}` | shoot this one whenever it can; `kNoEntity` lets it choose again |

The player can still fire by hand (`ShootCommand`) or take a drafted colonist over (U9): while
`PlayerControl` is on it, the fighter AI leaves it alone, as the job system does.

## What a fighter knows

Each fighter keeps a list of **known enemies** (`Fighter::known`): who, where it was last seen, when.
At every plan it looks at each enemy within 60 m (`sightRange`) from its standing eye height to the
enemy's eyes in the enemy's own stance (`Cover::lineOfSight`), so a crouched pawn behind sandbags
is not spotted until it stands up to shoot. Friends share: a fighter first takes what the other
fighters of its side know (the newer sighting wins), and doesn't look again at an enemy a friend saw
in this very step. An enemy not seen for 300 game seconds (10 real seconds, `memorySeconds`) is
forgotten; the dead are dropped at once.

So fighters react to what their side has seen, not to what the sim knows. An enemy nobody has seen
is safe until it shows itself. (Raiders in F6 will know where the colony is from their raid plan.)

## Whom to shoot

The game design doc's rule: **the closest real threat; the downed only when nothing else is left.**
An attack order wins. Otherwise, among the known enemies: able before downed, seen this step before
only remembered, then the nearest by where it was last seen.

## Where to stand

The plan scores every cell within 10 m (`searchRadius`) that the pawn can reach, stand on and isn't
another fighter's spot (and, for a drafted pawn, is inside its leash). Doors and cells with a
building that stops sight are not spots. The score, in metres of walking each term is worth:

| Term | How | Weight |
| --- | --- | --- |
| Cover | `coverAgainst` crouched, against each of the 4 nearest known able enemies (weights 1, 0.7, 0.5, 0.35) | +8 for full cover |
| Exposed | line of sight from each of those enemies' eyes to a crouched pawn here | -6 if all of them see it |
| Can shoot | from here, see below | -6 if it can't shoot its target from here (-1 when falling back) |
| Range | metres beyond the gun's range to the target, and metres closer than 4 m | -1 a metre |
| Advance | other factions only: metres beyond 70% of the gun's range | -0.5 a metre |
| Walking | straight-line metres from where the pawn is | -0.4 a metre |
| Crowding | friends' spots within 1.5 m | -3 for one right next to it |
| Staying | the current spot | +1.5, so it doesn't hop between two equal cells |
| Falling back | metres from the nearest threat, up to 40 | +0.8 a metre |

The cheap terms are worked out for every cell; the 10 best (`refine`) then get the line of sight
checks (exposed, can shoot), and the best of those wins. Ties go to the lower cell (y, then x), so
floats alone never decide the order.

**Can shoot** looks for a way to see the target, where it stands when it shows itself (standing up,
or lying if downed), within the gun's range, in this order: crouched here; standing here (over low
cover such as sandbags, "popping up"); crouched, then standing, from the cell beside this one on
either side of the line to the target ("leaning out" past the end of a wall). A cell beside it has to
be walkable, free of sight-blocking buildings and other fighters' spots, and reachable in a straight
step. Standing costs -0.5 and stepping out -1, so a fighter prefers the safest pose that still
shoots. Shooting at a target that is crouched behind sandbags is allowed on purpose: the sandbags
take the hits until it stands up to fire back, which is how firefights between two lines of cover go.

**Falling back** (game design doc: "falls back when outnumbered, badly hurt or out-gunned"): a fighter
falls back when the able enemies it knows of within 35 m are at least twice the able fighters of its
side within 35 m (itself included), or when it is badly hurt (pain 35%, blood loss 30%, or Moving
at 60% or less). It then scores distance from the enemy, cares less about having a shot, and keeps
moving back plan after plan; a drafted pawn only as far as its leash. Out-gunned (weapons and armor)
waits for F6's squad strength numbers. Fleeing the map is F6's rout.

## Acting

Every step, for each fighter in id order (after the job system, before movement):

1. Downed, dead, off the map or controlled by the player: it stops, and forgets its spot.
2. Re-plan when due (`planEvery` 15 steps). New fighters' first plans are spread over those 15 steps
   by pawn id, so 30 colonists drafted at once don't all plan in one step.
3. Mid-shot (the gun aiming or firing a burst): hold still, whatever the plan says now.
4. Walking to the spot: a path from the pathfinder queue, then the walk, upright. A pawn held up for
   good asks for a new path, three times at most, then plans again.
5. At the spot with a target it can shoot and a ready gun: step to the fire cell if that is the cell
   beside (a one-cell straight step), take the fire stance, check it still sees the target and that
   it is in range, and give the aimed `shoot` order (F2's warm-up, spread and burst follow).
6. Between shots: back to the spot if it stepped out (unless the gun is ready again within 20 steps,
   `stepBackBelow`), and crouch. A fighter that knows no enemy stands.

So a rifleman behind sandbags crouches while his rifle cools down and stands up through the warm-up
and the shot; behind a wall a fighter steps out past its end, fires and steps back.

## Numbers

All in `FighterRules` (`data/defs/Rules/FighterRules.json`), all ours: RimWorld's pawns don't seek
cover this way, so there is nothing to copy. The code falls back to the same values without the def.
Change them there to tune.

## Cost

Measured in the cloud, 30 drafted colonists with assault rifles against 40 raiders with rifles among
scattered walls and sandbags (`Fighters: 30 colonists against 40 raiders` test): the whole sim about
**0.35 ms a step** on average, at most 7 plans in one step, about 80 line of sight checks a plan.
At 3x speed that is roughly 1 ms per real frame on sneed's PC or less. Most of the time goes to
`plan` itself, line of sight and `coverAgainst`.

**The faction threat map (F3) is not used.** Rebuilding it for 40 enemies costs about 27 ms, more
than a few seconds' worth of all the fighters' plans, and it would be state that has to be saved or
rebuilt exactly after loading. Scoring only the cells near each fighter against its 4 nearest
threats is cheaper and needs no cache. The threat map stays for whole-side questions F6 will ask
(where is the safe side of the base, where to put a fallback line), rebuilt rarely and spread over
steps as cover.md says.

## Snapshot, saves, hash

- **Snapshot** (`ThingView`): `faction` for every pawn; for fighters `fighting`, `drafted`,
  `fightTarget`, `fightSpot` and `fallingBack`. The overseer view (U6, U8) can show the draft
  state and draw the spot; U11 can show who is aiming at whom.
- **Saves** (format 15): `Faction` and the whole `Fighter` (orders, plan, walk, path ticket, known
  enemies). A loaded fight goes on exactly as it would have (test "a firefight is the same after a
  save and load").
- Both are in `Sim::stateHash`; the commands are in replays (`Draft`, `MoveOrder`, `AttackOrder`,
  `SetFaction`).

## The tests

- A drafted rifleman takes the sandbags 5 m away, crouches and pops up to shoot.
- Behind a short wall a fighter holds the cell at its end and steps out past it to fire, then back.
- Target choice: the closest able enemy; the downed one only when nobody else is left; an attack order wins.
- One raider against three colonists falls back (13 m in the test); one against one holds.
- A move order takes a drafted pawn across the map and it holds within 2 m.
- Three against three among sandbags and walls: saving and loading mid-fight changes nothing, and
  fighters waiting in place with an enemy known are in cover (from the nearest one) over 90% of the
  time, the game design doc's "under 10% of a firefight in the open" in its first form. F7 runs the
  real version over many seeds.
- 30 against 40: plans are spread over the steps.
- The orders go through a replay file and play back the same fight.

## Not done yet

- Aiming like a player: reaction time, leading moving targets, reloading, dodging slow shots (F5).
- Squads, morale, rout and fleeing, raid plans, the colony alarm and defense roles (F6). Undrafted
  colonists don't react to enemies yet.
- Strafing while shooting, prone, diving away from grenades, melee, rescuing the downed.
- Danger in path costs: a fighter walks to its spot by the shortest path, even across open ground.
- Leaning is a one-cell step: the body is in the open cell while it shoots. Finer lean (part of the
  body out) waits for hitboxes that can lean.
- Trees and plants as cover come with C2's plants in the obstacle layer; doors open and close with C5.
