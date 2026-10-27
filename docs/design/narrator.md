# Narrator, incidents and threat points

Code: `ColonySim/Narrator.h`, `Private/Narrator.cpp`; colony wealth and raid points in `Raids.cpp`
(`colonyWealth`, `threatPoints`). Tests: `Tests/NarratorTests.cpp`; `simrun --narrator NAME`
(CTest `simrun_narrator`). Data: D10's `Narrators/`, `Difficulties/`, `Incidents/` and
`Rules/NarratorRules.json` ([narrator-data.md](narrator-data.md)), F6's `Rules/RaidRules.json`.
Task T1.

The **narrator** decides when something happens to the colony, as RimWorld's storyteller does.
Orrin is RimWorld's Cassandra (a steady build-up), Maren is Phoebe (long calm spells), Jinx is
Randy (anything at any time). The player picks one and a **difficulty** at game start.

## On and off

The narrator is **off** until a `SetNarratorCommand{narrator, difficulty}` picks one (`""` turns it
off again; an empty difficulty keeps the current one, Strive to survive by default). U20 sends it from
the new-game screen; `simrun --narrator Orrin [--difficulty LosingIsFun]` sends it at the first step.
Being off by default keeps every test, scenario and old save free of raids they never asked for.
Changing narrator throws away the old one's planned threats; the new one plans its own at its next check.

## When things happen

The narrator **checks every 24 game minutes** (RimWorld's 1,000 ticks; `NarratorRules.checkSeconds`
if it is ever set). At each check:

1. **Threat cycle** (Orrin and Maren, `threatCycle`): big threats come in cycles of `onDays` with
   threats, then `offDays` of calm. At the start of a cycle the narrator picks `incidentsPerCycle`
   moments at random within the "on" days. The first cycle starts on `firstThreatDay` (Orrin day 11,
   Maren day 13), or when the narrator is picked if that is later. Orrin's 4.6 + 6.0 days give one or
   two big threats every 10.6 days; Maren's 8 + 8, one every 16.
2. **By chance**, per category: each category the narrator has a `mtbDays` for (mean days between)
   rolls once: it happens with chance (check length / mean days), RimWorld's own MTB roll. Jinx rolls
   big threats this way too; Orrin and Maren leave them to the cycle. Diseases are rolled with the
   mean days divided by the difficulty's `diseaseFrequency` (Peaceful's 0.33: three times rarer).

When a category comes up, one **incident** of it is picked by weight among those that can happen now:

- the day is at least its `earliestDay`, and `cooldownDays` have passed since it last happened;
- the colony has at least `minColonists` living colonists;
- something can carry it out: it has a **worker** (below).

Its weight is `baseChance`, times the **population factor** for incidents marked `populationScaled`
(wanderers, crashed survivors): the wiki's population-intent curve by colonists (8 with none, 1 at
five, 0.4 at nine, below 0 from twelve), faded in over the first ten days from a neutral 1
(`populationIntentByDay`; our reading of "fades in"), never below `minIncidentChanceFactor` (0.05;
Jinx 0.2). A worker that says it can't after all (no hostile faction, no way in) is dropped and
another drawn. A big threat on the cycle with nothing that can happen is missed, not saved up.

## Threat points

A threat's points are the colony's raid points (F6, [raids.md](raids.md)) times the difficulty's
`threatScale` (Peaceful 0.1 to Losing is fun 2.2) and the narrator's `pointsFactor` (Maren 0.85),
times the incident's `points.factor` and capped by its `points.max`, kept between RaidRules'
35 and 10,000. Jinx also scales each threat by a random 0.5 to 1.5.

The raid points themselves follow the wiki's "Raid points" page: **colony wealth** is items on the
map and carried by the player's pawns, plus the player's own pawns (colonists and tame animals, T1:
"creatures count in full"), at their market value, plus buildings at half theirs. The wealth gives
wealth points (0 up to 14,000, 2,400 at 400,000, 4,200 at 1,000,000), each colonist gives 15 to 200 by
the same wealth, and the sum is scaled by the days passed (0.7 to day 10, 1 from day 40) and
adaptation (below). Six fresh Crashlanded colonists are worth about 16,000 with their
things: around 65 points, one pirate.

**Adaptation** (`RaidSystem::thinkAdaptation`, `adaptationFactor`) follows the wiki. The colony has
*AdaptDays*, starting at 0 (factor 0.8) and kept between -60 and 100. Every 12 hours they grow
(3 a day below 0, 1 from 0, 0.5 from 60, 0.25 from 120, each times the difficulty's
`adaptationGrowth`), but never above 0 in the first 30 days. Each colonist who goes down costs 8 days
(6 with 2 or 3 colonists, 5 with 11, 3.5 with 20) and each who dies costs 30 (20 at 20 colonists); the
colony's books are checked every 5 game seconds, so a colonist counts once per going down, and a
newcomer already down costs nothing. The factor is the wiki's curve of AdaptDays (-30: 0.4, 0: 0.8,
30: 1.0, 60: 1.2, 120: 1.6, 180: 2.0), blended with 1 by the difficulty's `adaptationImpact` (Losing
is fun 0.4 gives 0.75 to 1.19). The narrator hands the difficulty's two numbers to the raid system
when a narrator is picked or a save loads. The wiki's growth table is unclear on per check or per
day; we took per day (unverified, in `RaidRules.json`). Saved in `raids` 2.

## Incidents and workers

An incident happens through its worker, set by name with `NarratorSystem::setWorker(name, fn)`.
The worker gets an `IncidentFiring` (name, points, whether a dev command forced it), does the thing,
fills in the letter's text and place, and returns true; or returns false with a reason, having
changed nothing. Incidents without a worker never come. The event tasks add theirs (T2 good news and
nature, T3 trouble, C23 visitors and traders).

**The raid (`RaidEnemy`) is the first**, built in: it picks at random a faction that is always the
colony's enemy (`permanentEnemy`), can raid and has pawn kinds (now the Pirates and the savage
clan), and starts F6's raid with the points, at a random edge. Goodwill (C23) will let others turn
hostile.

`FireIncidentCommand{incident, points}` makes an incident happen now whatever its timing (points 0:
the colony's), for testing and for U20's debug menu. It works with the narrator off, and fails
in plain words for an unknown incident or one with no worker ("nothing can make a aurora happen yet").

## Letters

Each incident that happens sends a **`Letter` event**: `def` the Incidents def, `message` the letter's
text ("Raid: 2 raiders from the pirates have come to attack the colony."), `count` its `LetterKind`
(ThreatBig red, NegativeEvent orange, NeutralEvent grey, PositiveEvent blue: the incident's `letter`),
`amount` the threat points, `cell` where it happened if it has a place (the raid's entry: the
jump-to button), `squad` a raid's squad. U20 shows them; U8's log can list them too.

The snapshot's `narrator` has the narrator and difficulty (invalid while off), the wealth and threat
points as of the last check, and the incidents so far.

## Saved

Section `narrator` (version 1, [saves.md](saves.md)): the narrator and difficulty by name, its random
stream, the current cycle's start and the threats still planned in it, the last time each incident
happened (for cooldowns), the count, and the last wealth and points. A save from before T1 loads with
the narrator off. It is all in `Sim::stateHash`.

## Not yet

RimWorld's spacing rules between big threats beyond the cycle; Peaceful turning big
threats off (the data has only its 0.1 threat scale); the difficulty's `infestationRate` (for T3's
infestation); goodwill-hostile factions (C23); letters kept in a list after loading (the events are
the letters for now). The chances, days and cooldowns are D10's recalled numbers: tune them with
`tuning.json`.
