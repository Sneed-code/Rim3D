# Caravans (W4)

Colonists leave the map with goods, travel the world map tile by tile and arrive at settlements or
come home. Sim only: Unreal reads `Snapshot::caravans` and sends `CaravanCommand`. Code in
`Caravans.h` / `Caravans.cpp`, numbers in `data/defs/Rules/CaravanRules.json` and each biome's
`movementDifficulty`, tests in `Tests/CaravanTests.cpp`, CTest `simrun_caravan`.

## Commands (`CaravanCommand`, saved by name as "Caravan")

| action | fields | does |
|---|---|---|
| `Form` | `pawns`, `items`, `tile` | colonists and the item stacks they take set out for `tile` (-1: stay on the start tile) |
| `Move` | `caravan`, `tile` | a caravan on the world sets off for `tile` by the cheapest path |
| `Stop` | `caravan` | it waits where it is |
| `Enter` | `caravan` | a caravan on the start tile walks back onto the map (it does so by itself when it arrives home) |
| `Visit` | `caravan` | W5: a caravan on another tile walks onto that tile's map ([other-maps.md](other-maps.md)) |

`Form` with `map` set to a tile forms the caravan from colonists on that other map (W5).

Who can join: a colonist on the map, alive, up, not drafted (no `Fighter`), not a prisoner, not in a
mental break, not the pawn the player walks. The load must not be more than the members can carry,
and the sim must have a world (`SimSettings::world`).

## Forming

The exit cell is on the map edge facing the first step of the path (north is map row 0), the walkable
cell `exitDepth` (2) cells in nearest the middle of that edge that the first member can reach. Members
stop their jobs and walk there; each steps off the map (loses its `Position`) within 1.5 m. A member
is left behind if it goes down, is drafted, has a mental break, is taken over by the player, can't
find a way after 5 tries or isn't there after `formHours` (12, ours). When the last one is off, the
items still on the map are packed (taken off the map) and the caravan sets out.

## Travel (RimWorld's numbers, from the wiki)

- A tile of movement difficulty 1 takes 4412 ticks (6353 game seconds) for a fully loaded caravan;
  RimWorld's 13.6 tiles a day.
- Difficulty is the biome's (1 for most, 1.5 ice sheet and sea ice, 2 tropical rainforest, 4 swamp
  and cold bog) plus hills (0, 0.5, 1.5, 3) plus winter (0 above a season mean of -17.1 C, rising to
  +2 at -22.5 C and colder). Water and impassable tiles can't be entered.
- A road between the two tiles halves the cost. A diagonal step costs x1.414 (ours: our world is a
  square grid, RimWorld's is hexagons).
- Speed by load: x2 empty to x1 full (lerp by mass over capacity; recalled from RimWorld's code,
  marked unverified). Capacity is 35 kg per body size per member able to walk; downed members are
  carried and count as load.
- Caravans move from 6:00 to 22:00 and rest the rest of the night; members sleep then (needs gain rest
  as on the ground).
- The cost of a tile is fixed when the caravan sets out for it; the path is the cheapest by Dijkstra
  (ties to the lower tile index) in the season it was asked.

## On the road

Members keep their needs and health. A member whose food falls below `foodSeekBelow` eats the best
food in the caravan (meals before raw food, never humanlike meat) until full (`PawnAte`, message
"ground", `count` = caravan id). A member who dies becomes a corpse item in the caravan; when no member
is left able to walk, `CaravanLost` and everything in it is destroyed.

Arriving at the destination: `CaravanArrived` (message = the settlement's name, `amount` = the tile)
and the caravan waits. At the start tile it walks onto the map at the edge facing the tile it came
from, items on free cells next to the members (`CaravanEntered`).

Events (`count` = caravan id): `CaravanFormed`, `CaravanMemberLost`, `CaravanCancelled`,
`CaravanDeparted`, `CaravanArrived`, `CaravanEntered`, `CaravanLost`.

## Snapshot (`CaravanView`)

id, state (Forming, Travelling, Resting, Waiting), tile, next tile, destination, progress to the next
tile (0..1), pawns, items, mass and capacity in kg, tiles per day at the current speed.

## Saves

Section `caravans` 1: the next id, then each caravan with its state, members (pawn, walk state,
tries, path ticket), items, tiles, path, progress, cost, exit cell and when it formed. `CaravanMember`
is rebuilt from it.

## Not yet (in BACKLOG.md)

Hauling goods to the exit (packing is instant), animals and pack animals, food rotting and temperature
off the map, trading at settlements (C23), the gear members wear counting as load. Visiting is W5 ([other-maps.md](other-maps.md)).
