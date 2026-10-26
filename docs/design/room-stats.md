# Room stats, beauty and filth

Code: `ColonySim/Floors.h`, `Filth.h`, `RoomStats.h` and their `Private/` files, `Private/CleanJobs.cpp`
(the cleaning job), the staged situations in `Mood.cpp`. Defs: `data/defs/Filth/Filth.json`,
`FilthRules`, `RoomStatRules` and `BeautyNeedRules` in `data/defs/Rules/RoomStatRules.json`, five
thoughts in `data/defs/Thoughts/Thoughts2.json`. Tests: `Tests/RoomStatsTests.cpp`. Task C17.
Builds on [rooms](rooms.md), [mood](mood.md), [thoughts-data](thoughts-data.md) and [jobs](jobs.md).
A number marked `ours` in a def is not RimWorld's; `unverified` was recalled, not read from the wiki.

## Floors

A floor is a Terrain def with `designationCategory` Floors (`Floors::isFloorDef`), laid over the
ground. `Floors` is a map from cell to floor def, not entities. `PlaceFloorCommand{area, floor}` lays
a floor over a rectangle at once (an empty `floor` takes them off). **There is no floor-building
job yet**: floors appear by command only (BACKLOG). A floor's `beauty`, `cleanliness` and
`marketValue` come from its def.

## Filth

Filth is a layer too: per cell, a few piles, each a Filth def with a thickness from 1 to
`maxThickness` (3, unverified). Kinds: Dirt, Blood, Vomit, Ash, AnimalFilth; each has `beauty`,
`cleanliness` and `cleaningWork` (RimWorld ticks, times `FilthRules.ticksToSeconds` 1.44 for game
seconds). A layer's beauty and cleanliness count once per thickness. Outdoors a pile's beauty is
times `outdoorBeautyFactor` (0.3).

Where filth comes from, so far:

- **Tracked dirt.** A pawn walking on bare ground (soil, sand, rough stone with no floor) may pick up
  dirt, and drops what it carries on floors, with chance 0.05 per new cell (ours), up to 3 carried,
  scaled by the pawn def's `stats.filthRate`. Other trackable kinds are picked up from the cells.
- **Blood.** A bleeding pawn drops blood: `bloodPerBleedDay` (ours) drops an hour per point of
  bleed-per-day.
- `AddFilthCommand{cell, kind, layers}` puts filth down (dev, and the hook for later sources).
- Not yet: vomit, ash (fire, E4), animal filth (animals). The API takes them already.

Filth has its own `Rng`, seeded from the world seed, and is in the save and the state hash.

## Cleaning

`CleanFilth` is a job (`JobType::CleanFilth`, steps GotoCell then CleanWork) from the work type
Cleaning (priority 0 stops it). The giver picks the nearest filthy cell **indoors**; outdoor cleaning
waits for the home area (BACKLOG). One layer takes `cleaningWork × 1.44` game seconds, divided by
manipulation × `FilthRules.cleaningSpeed`. The job sends `PawnAction::Clean` when it starts a layer.
It reserves the cell. Ends when the cell is clean or the pawn is hurt out of reach.

## Room stats (RimWorld's formulas)

`RoomStats::statsAt(cell)` / `statsOf(room)` give a `RoomStatValues`: `beauty`, `space`, `wealth`,
`cleanliness`, `impressiveness`, each also with a stage name (`beautyLabel` and so on in
`RoomStats.h`). Open-to-the-sky rooms have none.

- **Cell beauty:** floor + things standing in it (building `beauty`, stuff-adjusted by
  `stuffBeauty`) + filth, plus nearby plants outdoors. Cleanliness is the same sum of cleanliness.
- **Room beauty:** total of the room's cells and the ring of cells around it, over the room's size
  (a room under 40 cells counts as 20 + size/2).
- **Space:** 1.4 per cell, minus 0.9 for each cell holding something you cannot stand on.
- **Wealth:** market value of everything in the room: items, plants (outdoors parts), buildings
  (materials + stuff + work × 0.0036) and floors.
- **Cleanliness:** average cell cleanliness.
- **Impressiveness:** `roomImpressiveness()`; the wiki example (wealth, beauty, space, cleanliness)
  gives 70, which is a test.

Stats are recomputed when anything they depend on changes (a version made of the room
rebuild counter, building changes, floors, filth, item changes, plants and the room count). That
makes them a pure function of the game state, so they are **not saved**; they are recomputed after a
load and after construction.

## The beauty need

Every colonist has one: 0 to 1, 35% to 65% neutral. Every 30 game seconds (staggered by pawn id) a
pawn judges the average beauty of the cells within 8.7 m it can see (`Cover::lineOfSight`), and the
need heads for a target: a straight line from 0.35 at beauty -3.5 to 0.65 at 2.4. It rises 0.12 an
hour and falls 0.04 an hour (ours), only while awake. Stored in `RoomStats` by pawn, in the
`roomstats` save section.

## Thoughts

`Mood` reads staged situations from `RoomStats::conditionValue(pawn, condition)`. A thought def with
`condition`, `stageThresholds` and `stages` (offset, label, optional `roles` / `roomRoles`) is one
thought whose stage follows the value; a stage with offset 0 shows nothing.

| Thought | Condition | Note |
| --- | --- | --- |
| `Beauty` | `beautyNeed` | -15 to +15 across six stages |
| `Space` | `roomSpace` | cramped -3, normal 0, spacious +2 |
| `RoomImpressive` | `roomImpressiveness` | bedroom, barracks, dining room only; ten stages |
| `Filthy` | `roomCleanliness` | dirty -3, clean +2 |
| `Barracks` | `sleepSharedRoom` | -1 |

## What Unreal sees

- **Snapshot:** `floors` and `filth` (cell order; rewritten only when `floorChanges` /
  `filthChanges` goes up), `ThingView::beautyNeed`, and on each `RoomView` `hasStats`, `beauty`,
  `space`, `wealth`, `cleanliness`, `impressiveness`.
- **Events:** `PawnAction` `Clean`.
- **Commands:** `PlaceFloor`, `AddFilth`.
- **Read:** `sim.roomStats()`, `sim.filth()`, `sim.floors()`.

## Saves

Section `roomstats` version 1: floors, filth piles, the carried filth per pawn, the filth `Rng`, the
beauty needs. In the state hash. The stats themselves are recomputed.

## Art

Sculptures, whose quality scales their beauty and value, are in [art](art.md).

## Not yet

A floor-building job and architect tool; a home area (cleaning and filth outdoors); vomit, ash and
animal filth sources; room stats while the sky is partly open; temperature; the room role "prison";
bedroom owners' thoughts about other people's rooms.
