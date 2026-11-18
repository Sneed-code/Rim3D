# Earthworks, ground fit and getting unstuck

Code: `ColonySim/Map.h` (`planGround`, `planCells`, `digEarth`, `earthUnits`, `earthPending`, `cancelEarth`,
`earthCorners`, `EarthState`), `Things.h` (`GroundFit`, `groundFitOf`), `Construction.h` (`planBetweenWalls`,
`planLevelEarth`, `cancelEarthworks`, the steepness check in `whyNotBlueprint`), `Private/EarthJobs.cpp` (the
Earthworks job and its scanner), the earth part of `JobSystem::buildWork`, the stuck watchdog in
`Private/MovementSystem.cpp`, `Sim::unstuckPawn`. Tests: `Tests/EarthworksTests.cpp`, `Tests/LevellingTests.cpp`.
Asked for by sneed on 2026-11-18 (screenshots: sandbags flattening a hillside, leaves left floating over a dug
pit, a trench left between two walls). Builds on [levelling.md](levelling.md): the bases, steps and joining
rules are the same; what changed is *when* the ground moves and *who* moves it.

All numbers here are ours: RimWorld's ground is flat.

## Ground fit

Every building def has a `building.groundFit` (`GroundFit`):

| Fit | What it does | Default for |
| --- | --- | --- |
| `Level` | its footprint is dug or filled flat (levelling.md), and ground between walls is too | `designationCategory` Structure, or none |
| `Flat` | the same, but it **can't be planned on ground that rises more than `kFlatMaxRiseM` (25 cm)** under it: "too steep: level the earth first". The ground counts as it will be once its planned earthworks are done, and cells all planned to one base (Level earth) count as flat | every other category (furniture, production, joy, power, turrets...) |
| `Conform` | **the ground is left as it is**; the building leans with it (Unreal tilts it to the plane of its corners) | set in the data: sandbags, barricades, spike traps, IED traps |

## Earthworks

- **Plans only mark ground.** Placing a plan (`planGround`) sets its cells' bases at once, so later plans join
  them as before, but no corner moves. Each corner that must move gets a **target** (`earthTarget`); the
  corners waiting are `Map::earthCorners()`.
- **One unit** is one corner `kEarthUnitM` (0.25 m) up or down, about one square metre of earth a quarter metre
  deep. It costs `ConstructionRules.earthWorkPerUnit` (51) work at construction speed, which is 30 game seconds,
  one real second at 1x, for an average builder. A steep 5 m slope dug flat over 5 m is roughly 5 + 4 + 3 + 2 + 1
  units per row of corners, as sneed described. Earth just goes or comes: no dirt is hauled (yet).
- **Under a building plan** the builder digs first, as the start of its build work (`buildWork`): only once all
  materials are delivered and work starts does the ground move. The first unit clears every plant on the
  footprint (trees too, as completion always did).
- **Everywhere else** (between walls, roofed rooms, Level earth) the **Earthworks** job does it (task
  `Earthworks` in the Construction work type): a pawn walks to a walkable cell round a waiting corner and digs
  that cell's corners, the farthest from its target first. Corners touching a building plan wait for its
  builder; corners touching a **tree** wait until it is cut, and the tree is **marked for cutting**
  automatically (`JobSystem::markEarthTrees`), so the wood is not lost.
- **Plants where the ground moved** go: each moved corner removes the grass and bushes on the four cells
  round it (`earthMoved`). Unreal redraws those cells (terrain, plants, ground cover) from
  `Map::levelledCorners()` as before.
- **Between walls** (`planBetweenWalls`): when a wall or door plan goes down, every open ground cell within
  `betweenWallsCells` (6) of it that meets a wall both ways along a row or column (a corridor, two parallel
  walls) or one way along a row and one along a column (inside an L) is planned flat to the lower of those walls'
  bases. Rock, water or furniture in the way closes a line. Cells already planned keep their plan.
- **Roofed rooms** are planned to their walls' base as before (levelling.md), now as earthworks.
- **Level earth** (`LevelEarthCommand{area, baseCm}`, the Structure tab's tool): every ground cell of the area
  with no building or plan is planned to the height, snapped to 0.25 m. Unreal takes the height from the
  ground where the drag starts (Raise and Lower buttons nudge it 25 cm). **No earthworks dig or fill more than `earthMaxM`
  (6 m, twice a wall)** from any corner; cells beyond are left out. `CancelEarthworksCommand{area}` drops what
  no building plan needs.
- **Cancelling** a plan drops its earthworks that have not started (`cancelEarth`): bases of cells none of whose
  corners moved, and targets nothing else wants. Dug ground stays dug.
- **Instant levelling** (`levelGround`, `levelCells`) is still there for map generation, scenarios, spawned
  buildings and the dev floor command, and clears any target on the corners it moves.

## Stuck pawns

- **The watchdog** (`MovementSystem`): a pawn that wants to walk but stays within a metre of one spot for 300
  steps (10 s at 1x) walks straight at its waypoint for 90 steps (3 s), through walls, trunks and pawns
  (`Mover::stuckSteps`, `phaseSteps`; `pawnsPhased()` counts them). Nothing on screen marks it.
- **F12, Unstuck** (the player): Unreal holds the player still for 5 seconds (no actions), then sends
  `UnstuckCommand{pawn}`: the nearest cell within 12 m that is walkable, has room for the pawn, is not deep water
  and has at least three walkable cells round it. The walk is stopped.

## Saves and hashes

Section `terrain` 3 adds `earthCorners`/`earthTargets` (cm); `movement` 3 adds the watchdog's four fields.
`Map::levelHash` adds the targets, `Sim::stateHash` adds `phaseSteps` only while a pawn phases.

## Unreal (PC)

Written but not compiled yet, so kept out of the repo as a patch in the project files
(`earthworks/unreal-earthworks.patch`, against this merge): conforming buildings tilt to their corners, ground cover and rocks tilt to the ground, the
Level earth tool (drag, Raise/Lower 25 cm buttons and a height label), waiting earthworks drawn as an outline, F12 Unstuck
with a 5-second bar. Moonlight is brighter in `data/look.json` (full moon 3 lux, new moon 40% of it, exposure
`follow` 0.88).
