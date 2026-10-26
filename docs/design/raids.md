# Raids and colony defense

Code: `ColonySim/Raids.h`, `Private/Raids.cpp`; the fighter side in `Fighters.cpp` (`FightMode`,
`planGoal`). Tests: `Tests/RaidTests.cpp`; `simrun --raid POINTS` (CTest `simrun_raid`). Data:
`data/defs/Factions/Factions.json`, `data/defs/PawnKinds/PawnKinds.json`,
`data/defs/Rules/RaidRules.json`. Task F6. It stands on the fighter AI ([fighters.md](fighters.md),
[aiming.md](aiming.md)), colonists made from a seed ([colonists.md](colonists.md)) and the job
system ([jobs.md](jobs.md)).

A **raid** is a squad of another faction's pawns that walks in at a map edge and goes for the
colony, as RimWorld's "assault colony" raid does. Its pawns are ordinary fighters; the **squad**
decides together whether to advance, hold, fall back or run. The colony answers with the
**alarm**: it sounds by itself when a colonist spots an enemy (or the player sounds it), and every
colonist then does what its **defense role** says. Drafting still works and always wins: a drafted
colonist is the player's, not the alarm's.

## Raid points and who comes

`StartRaidCommand{faction, points, edge}` starts a raid (a dev command until the narrator sends
them). `points` 0 means "what the colony is worth now", RimWorld's rule from the wiki's Raid points
page:

- **Wealth**: items on the map and the gear the player's pawns carry at their `marketValue`, plus
  buildings at half theirs (`RaidSystem::colonyWealth`). Defs without a `marketValue` (our guns,
  apparel and buildings so far) count nothing.
- **Points** = (wealth points + 15 to 200 per colonist, both from the wiki's curves by wealth) x
  the difficulty's threat scale (Strive to survive, 1) x the days-passed factor (0.7 up to day 10,
  1 from day 40) x adaptation (0.8, its start value; adaptation isn't tracked yet), kept between
  35 and 10,000. The six Crashlanded colonists on day 0 make about 50 points: one pirate.

The points **buy pawns**, as RimWorld does: a pawn kind at random by weight among those the points
left can still pay for (`combatPower`), until none can; at least one. The **Pirate** faction (a
name we keep, see [naming.md](naming.md)) sends three gunner kinds from the wiki: scavenger gunner
(50), pirate gunner (65) and mercenary gunner (85). Each is a whole person made like a colonist
(name, backstory, traits, skills; a backstory that can't fight is drawn again), with looks, a gun
from its kind's list and gear by chance (helmets, flak). Melee kinds, grenadiers, snipers and
heavies wait for melee, grenades and their guns.

They **walk in** a few cells in from the chosen edge (or a random one), at a spot that can reach the
colony, a metre apart. Each gets a `Faction` (the faction's id: 2 for the first faction met, and so
on), a `Raider{squad, kind}` and a `Fighter` that advances, with the squad's target as its **goal**:
a fighter that knows no enemy walks to its goal. That is how raiders "know where the colony is"
(fighters.md): they don't see the colonists until they are in sight, but they head for them.

## The squad (morale)

Each squad thinks once a real second (`squadThinkEvery`, 30 steps):

1. **Rout** (RimWorld's rule, recalled): when half its pawns (`fleeLostFraction`) are downed, dead or
   gone, or when its time is up (`giveUpHours`, RimWorld's 26,000 to 38,000 ticks: 10.4 to 15.2 game
   hours), it runs. Every member that can still walk gets `FightMode::Flee`: no more looking or
   shooting, just the nearest edge cell it can reach. At the edge it **leaves the map** with its gear
   (`PawnLeftMap`, then `ThingDestroyed`). Downed raiders stay where they fell.
2. Otherwise **strength** decides: its able members' strength against the strength of the enemies its
   members know of. A pawn's strength is its health ((1 - pain) x Moving), times 1 with a gun (0.3
   without), times 1 + half its armor's sharp rating. At least as strong (`advanceRatio` 1):
   **Advance** (fighters advance). At least half as strong (`holdRatio`): **Hold** (they stop advancing
   and trade shots from cover). Weaker: **FallBack** (every member falls back, `Fighter::retreat`).
   A margin of 25% (`hysteresis`) favours the state it is in, so it doesn't flip at a threshold.
3. **Target**: the colony's pawn nearest the squad. The goal moves only when that pawn is over 8 m
   from the old target, so raiders don't stop and ask for a new path every second.

The strength rule is ours: RimWorld's raids only count losses. A squad with no member left standing
on the map is over and leaves `RaidSystem::state().squads`.

## The alarm and defense roles

Every half real second (`alarmThinkEvery`, 15 steps) the alarm looks for **enemies**: another
faction's pawns on the map that can act and aren't fleeing. One is **spotted** when a colony fighter
knows of it or one of the colony's pawns sees it within 60 m (line of sight from standing eyes, as
fighters see). Spotting one **sounds the alarm** by itself; the middle of the colony's buildings
(else of its colonists) becomes **home**, and the middle of the spotted enemies the **threat**.

While the alarm is on, every colonist that isn't drafted or controlled by the player does what its
`DefensePlan` says (`SetDefenseRoleCommand`; none is a Defender):

| Role | What it does |
| --- | --- |
| **Defender** | Its job ends and it becomes a fighter (`defending`). With a **defense spot** it holds that spot, choosing cover within 3 m. Without one it goes to **the line**: a point 15 m (`defenseLine`, at most half way) from home toward the threat, choosing cover within 10 m of it; the line moves when the threat has moved it over 6 m. **Once it knows of an enemy it leaves the line** and fights like any fighter (cover, range, falling back), so an enemy out of range of the line can't shoot it from afar; with no enemy known again it goes back to the line. |
| **Shelter** | Goes to the shelter room (`MarkShelterCommand`) and stays there, each in its own cell; with no shelter marked, the nearest roofed cell; with none, 25 m away from the threat. It never shoots. |
| **Keep working** | Works on, until a spotted enemy comes within 20 m (`keepWorkingRadius`); then it defends itself where it is, until the alarm stops. |

The alarm **stops by itself** when no enemy is left on the map that can still fight (all dead, downed
or fleeing), and everyone it called up goes back to work. One the player sounded
(`SoundAlarmCommand{true}`) stays on until the player stops it; stopping it while enemies are about
keeps it from sounding again by itself until they are gone.

**Drafting a called-up colonist** hands it to the player where it stands; undrafting it gives it back
to the alarm at the next check while the alarm is on.

## Snapshot, saves, hash

- **Snapshot**: `alarm`, `alarmManual` and `squads` (id, faction, state, members, started, target);
  per pawn `defenseRole` (colonists), `defending`, `sheltering`, `fleeing` and `squad` (raiders).
  The overseer view (U8) can show the alarm button and roles, U11 can show routs.
- **Events**: `RaidArrived`, `SquadChanged`, `AlarmChanged`, `PawnLeftMap` (bridge.md).
- **Saves** (section `raids`, and `fighters` 2): the whole `RaidState`, `Raider` and `DefensePlan` components and the new
  fighter fields. A raid goes on the same after a save and load (test).
- All of it is in `Sim::stateHash`; the four commands are in replays.

## Numbers

RimWorld's (wiki, Raid points): the wealth and per-colonist curves, threat scale, days-passed
factor, minimum and maximum points; combat power and the gunner kinds' guns and headgear chances
(wiki pawn kind pages). Recalled, not verified: the rout share (half), the give-up time and the
straight line of the days-passed factor between day 10 and 40. Ours: everything about strength,
morale thresholds, the alarm, the line, roles and timings (`RaidRules`, all marked in the def).

## What a raid looks like now

`simrun --seed 1 --size 256 --days 0.25 --start Crashlanded --raid 300 --armed` (the six colonists with
assault rifles against 300 points of pirates, 4 or 5 of them, on open ground): the raiders walk in,
the alarm sounds when they are seen 12 to 26 game minutes later, the fight runs from a few minutes
to a few game hours, and in most seeds the raid routs after losing half its pawns and the rest run
off the map. On open ground with no cover the shooting is slow and both sides lose people: crouched
pawns in the open are hit about 1 shot in 100 at 20 to 30 m in these fights, and downed colonists
bleed out because nobody rescues or tends them yet. F7 measures and tunes this over many seeds.

## Strategies

How a raid comes (walking in, drop pods, at night) and what it goes for (the colony, a wall to dig
through, loose items, downed colonists) is F12's: [raid-strategies.md](raid-strategies.md).

## Not done yet

- The narrator sending raids by itself, raid warnings and other strategies (siege, sappers, drop
  pods, kidnapping and stealing); RimWorld's `PawnGroupMaker` subtleties (the most expensive pawn
  limited by the points).
- Adaptation (the factor stays at 0.8), market values for guns, apparel and buildings.
- Medic and Firefighter roles (with medicine and fire, Stage 2), rescuing the downed, fallback lines
  and using the F3 threat map to choose the safe side.
- Raiders don't yet use squad-level tactics like flanking; each fighter picks its own cover.
