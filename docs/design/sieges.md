# Sieges

Code: `ColonySim/Raids.h` (`RaidStrategy::Siege`, `SiegeStage`, `Siege`, `SiegeBuild`), `Private/Sieges.cpp`, the
siege hooks in `Private/Raids.cpp` and the foreign crew in `Private/Artillery.cpp`. Tests: `Tests/SiegeTests.cpp`, the
raid strategy round in `Tests/EventScenarioTests.cpp`; `simrun --raid POINTS --raid-strategy Siege`. Data: the siege
numbers in `data/defs/Rules/RaidRules.json`, `Sandbags` in `Things/Defense.json`, the mortar and its shell in
`Things/DefenseArtillery.json`. It stands on raid strategies ([raid-strategies.md](raid-strategies.md)) and the mortar
([artillery.md](artillery.md)).

RimWorld's siege: raiders walk in, dig in out of reach, have supplies dropped to them, build sandbags and mortars, and
shell the colony; they attack after a few days, when their mortars are lost, or after enough casualties (wiki,
"Events"). Ours is the same, and the supplies run out: the siege also ends when the shells do.

## How a siege goes

`StartStrategyRaidCommand{faction, points, edge, "Siege", hour}`, or the narrator (below). A siege squad is an ordinary
raid squad (morale, rout, the alarm) with a camp. `Squad::siege.stage` goes:

| Stage | What happens | Ends when |
| --- | --- | --- |
| **March** | Every raider walks to the camp (`FightMode::Seize`: it doesn't stop to fight on the way). | one is within `campArrive` (8 m) of the camp's middle: `SiegeChanged` "camp", and the first shells drop in (`shellsPerMortar` x mortars, a `DropPodLanded` per stack) |
| **Build** | Builders (the first `builderShare` of the free members) go beside the next unbuilt thing, mortars first, and work on it: each one within `buildReach` adds its Construction speed a second. Done, it is built through `Construction` (blueprint, then complete: pawns and items in the way are moved off). A mortar that is up gets a crew and fires at once. The rest guard the camp: they fight from within `campLeash` (10 m) of it. | every mortar is built (or given up): `SiegeChanged` "shelling" |
| **Shell** | Crews man the mortars and load them from the shells lying in the camp; builders finish the sandbags, then guard. | an assault (below) |
| **Assault** | An ordinary assault from here: the crews leave the guns, everyone goes for the colony, the give-up clock (10.4 to 15.2 h) starts. Walls in the way are dug through as sappers do. | the raid ends as any raid does |

**It assaults** (`SiegeChanged` "assault: why") when it has lost `siegeAssaultLost` (40%) of its pawns ("losses";
the rout rule at 50% still holds), after `siegeDays` (2 days, "time"), when no mortar stands and none is left to build
("no mortars"), or when it is out of shells ("out of shells": none in the camp or the guns, and no drop left).

**Supplies.** Whenever the camp holds fewer than `resupplyAt` (4) shells while shelling, `resupplyShells` (10) more
drop in, at most `resupplyMax` (3) times. With two mortars that is 40 shells, about six hours of shelling. Shells lie in
the camp as items: the colony can take them (or blow them up) once it gets there.

**Builders defend.** An enemy any guard knows of within `campDefend` (30 m) of the camp sends the builders to fight;
the crews stay at their guns.

## The camp

Picked when the raid starts (a raid that can't get one fails, and the narrator sends an assault instead): open,
unroofed ground the raid can walk to from its entry, 7 x 7 cells clear, `campDistanceMax` (70 m) down to
`campDistanceMin` (35 m, outside the mortar's 29.9 m minimum range) from the colonist the raid goes for, on the entry's
side first, turning 15 degrees at a time.

- **Mortars**: `points / pointsPerMortar` (60), rounded, 1 to `mortarsMax` (2), side by side across the line to the
  colony, 6 m apart, a little behind the camp's middle. A ring round each is kept clear for its crew.
- **Sandbags**: 2 to 4 lines (`coverLines*`) of 2 to 7 (`coverLength*`), 4 to 6 m in front of the camp toward the colony,
  of `siegeCoverStuff` (cloth). RimWorld's sandbags: 5 fabric or leather, 300 HP, 180 ticks of work, cover 55%, beauty
  -10, not flammable (wiki).
- **Shells** drop 4 m behind the camp's middle, on free cells nothing is built on, within `campSupplies` (12 m).

What it builds stays when the siege is over: the colony can man the mortars (they are not anybody's) or take them down.

## The mortars

A siege's mortar is the colony's mortar ([artillery.md](artillery.md)) with a raider crew
(`ArtillerySystem::setForeignCrew`): the crew mans it while it stands within `reach` of it (the raid system walks it
there), and a gun manned by another faction picks the colony's pawns as its targets (the nearest able one in range, as
RimWorld). With nobody in reach of its shells it shells the colony's building nearest the colonist the raid goes for.
Its auto-refuel is off so the colony's haulers don't load it. A colonist ordered onto a siege mortar takes it over.

## The narrator

`RaidSystem::pickStrategy(points, faction)` gives Siege weight `weightSiege` (8, of 108 with the others) from
`siegeMinPoints` (300) up, for a faction that isn't neolithic, medieval or not human (RimWorld: tribes can't siege).
The narrator's letter says "They are digging in to shell the colony with mortars."

## Numbers

Recalled from RimWorld's siege code, not checked (`unverified` in the def): builder share 0.4 (RimWorld 0.25 to 0.4),
mortars points / 60 clamped 1 to 2, 2 to 4 sandbag lines of 2 to 7, 5 shells a mortar, another drop under 4 shells of
10, assault at 40% lost. Ours: the weight, minimum points, camp distances, leash, defend radius, supply radius, build
reach, 3 drops, 2 days.

## Save, snapshot, hash

- Save section `raids` 3: per squad its `siege` (stage, camp, end time, resupplies, builds). Older files load with an
  empty siege. All of it is in `Sim::stateHash`.
- Snapshot: `SquadView::strategy` 5, `siegeStage`, `camp`. Events: `SiegeChanged` (appended to `EventType`), and
  `DropPodLanded` with `thing` the shell stack and `count` its shells.

## Not done yet

- Meals in the drops (raiders don't eat yet) and the steel and cloth RimWorld drops for building; the builders don't
  carry materials.
- Raiders bringing reinforced barrels the colony could take; RimWorld 1.6's siege mortar.
- Unreal: the camp, the drops coming down, the crews' pose. The sim gives `siegeStage`, `camp`, the builds as
  ordinary buildings and the shells as items.
- Sandbags slow walking (`pathCost` 42 is in the def; the pathfinder doesn't read it for buildings yet).
