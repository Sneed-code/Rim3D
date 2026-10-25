# Status

Updated 2026-10-25.

## Where we are

Stage 0 (foundation) is done. Stage 1 (the playable prototype) is planned and starting.

Stage 0 in short:
- Step 1: game design and architecture docs (`docs/`).
- Step 2: repo, sim plugin, tests, `simrun`, CI on Linux and Windows, data checker, Unreal
  project and the `USimSubsystem` bridge. sneed's PC has Unreal 5.8.3 and Visual Studio 2026;
  the project is at `C:\Dev\Rim3D` and compiles (`docs/setup/pc-setup.md`).
- Step 3: Stage 0 tests, results in `docs/decisions/0001` (sim speed) and `0002` (characters
  and terrain). The Game Animation Sample is in `game/Content/` (not in Git, see
  `assets/manifest.json`).
- Step 4: the Stage 0 results are locked into `docs/architecture.md` (hierarchical pathfinding
  with a per-step budget counted in work, not time; animation detail by distance; real body
  types and our own skin material; runtime terrain; levels in every grid interface; sim on the
  game thread through Stage 1). Stage 1 is planned as 43 one-sitting tasks in
  `docs/plans/stage-1.md`.

Things (pawns, items, buildings) are EnTT entities made from defs since S1. Since S2 the map has
regions (16 m blocks, doors as their own regions, links, reachability groups; `Map::regions()`).
Since S5 Unreal talks to the sim only through commands (`Sim::submit`), per-step events and a
read-only snapshot; every session can be saved as a replay file and played back with
`simrun --replay`, and CI checks Linux and Windows give the same state hashes
(`docs/design/bridge.md`). Since S3 paths are hierarchical: region route, then cell A\* inside that
corridor, asked for through a queue with a work budget per step (`sim.pathfinder().request`, an
answer the next step at the earliest); a long 1 km search costs about 1,500 cells instead of
34,000 (`docs/design/pathfinding.md`). Since F3 buildings and ground have heights, so the
sim can say how hidden a pawn is from each of eight sides (`Map::cover()`), whether one pawn can see
another (`lineOfSight`, with standing, crouching and prone) and which ground a faction's enemies
can shoot at (`Sim::threatMap`); `docs/design/cover.md`. Since S6 the whole game state saves to
one binary file and loads back to exactly the same game (`saveSim` / `loadSim`, `simrun --save`,
`--load`, `--save-json`; `docs/design/saves.md`). Since S4 pawns walk instead of hopping from cell to
cell: the cell path is straightened where a pawn fits, speed follows the ground (soil 87%, sand 76%, as
RimWorld) and the slope, and pawns steer around each other deterministically; the snapshot has each
pawn's velocity, ground height and route ahead (`docs/design/movement.md`). Since U2 `USimSubsystem` steps the sim one second at a
time, passes events to `OnSimEvent`, keeps the last two snapshots and blends positions between them
(`GetBlendedLocation`, the one place sim metres become Unreal centimetres), loads `data/defs`, shows
sim timings on screen, and has keys: Space pause, 1/2/3 speed, `.` step, F7 saves a replay to
`Saved/Replays/` (the bug-replay key U12 planned). Since U6 there is an overseer camera (`-Overseer` or console `Overseer.Enter`): WASD pan, Q/E turn, wheel zoom, middle-drag orbit, click to select a pawn, thing or ground cell (picked against the sim, not Unreal collision), with an ImGui speed bar and inspect panel; ImGui is the free VesCodes plugin in `game/Plugins/ImGui/` (not in Git, `assets/manifest.json` says how to get it back) (`docs/design/overseer.md`). Since U5 every sim pawn is drawn as an actor without a movement component that follows the snapshot: the 20 nearest the camera are motion matched (Epic's motion matching run from our own C++, no Animation Blueprint; trajectory from the sim's route), the rest play walk, run and idle loops updated 20 or 8 times a second; the sim's `PawnAction` events (pick up, put down) play a short action animation; `Pawns.MetaHumans 1` draws them as MetaHumans with strand hair only up close (`docs/design/pawns.md`). Since U12 the game saves and loads: F5 quicksave, F9 quickload, an autosave every game day (three files in turn) and F8 writes a bug report folder (replay, save, readable dump, notes) to hand to the cloud (`docs/design/saves.md`, smoke test `saves`). **Every task that adds sim state must add it to the
save** (the doc has the steps) and to the save round-trip test. Since C2 the map has plants: a grid layer (kind, growth, age per cell, `sim.plants()`) filled at map generation from fertility and the temperate forest biome (about 119,000 on a 512 m map) and grown hourly in the growing hours 06:00 to 19:00, dying at their lifespan; `simrun --png` draws them; save format 9 (`docs/design/plants.md`). Since U9 you can walk a colonist yourself: Tab switches between the
overseer view and direct mode on the protagonist (or the selected colonist), in first or third person (V), T takes over the colonist
under the crosshair. In the sim that is two commands, `ControlPawn` and `DirectMove` (a position every step, kept out of walls); while a colonist
is controlled its job ends and the job system and movement system leave it alone until control is let go (**a new think-list node or system
that acts on colonists must skip pawns with `PlayerControl`**); a controlled colonist who goes down or dies is let go. Direct mode holds the game at
1x or paused. Start it with `-SimScenario=Crashlanded -Direct` (the game's default start still has no colonists); save format 7
(`docs/design/protagonist.md`, smoke test `protagonist`).

Since F1 every pawn with a body (`race.body`) has per-part health: the Human body's 64 parts, injuries
with pain and bleeding, blood loss, downed (pain shock, unconscious, can't move) and death, slow natural
healing and tending (`sim.health().applyDamage`, the dev commands `DamagePawn` and `TendPawn`, events
`PawnInjured`/`PartLost`/`Downed`/`GotUp`/`Died`, `downed` and `dead` in the snapshot, `sim.healthReport`;
`docs/design/health.md`). Injured pawns walk slower and downed ones stop. Capacities now work per limb
(one lost leg is 50% moving), see `starting-data.md`. The protagonist from the Crashlanded start has 2x
part hit points. Not built yet: scars, infections, diseases, medicine items and the doctor job.

Since C1 pawns work through a job system like RimWorld's (`docs/design/jobs.md`): a think list
(work, then idle; emergencies, defense, needs and schedule slot in ahead of work later), the 20 work
types from the data in Work tab order with priorities 1 to 4 per pawn (`SetWorkPriority`; colonists
can't do the work their backstories and traits rule out), work scanners that take the nearest target
and skip ones nobody can reach, reservations so two pawns never go for the same thing, and jobs as
steps with fail checks (a pawn that fails remembers the target for 10 minutes). Stockpile zones
replace the placeholder stockpile: priorities Low to Critical, a filter, one stack per cell, better
stockpiles pull from worse ones (`CreateStockpile`, `RemoveZoneCells`, `SetStockpilePriority`,
`SetStockpileAllowed`), and `OrderHaul` is "prioritize hauling". Hauling is the first job on it;
items stack up to their stack limit. A downed pawn drops its job and what it carries. Events
`JobStarted`/`JobEnded`, and the snapshot shows each pawn's job, load and the zones. Save format 6:
**saves from before C1 (U12's quicksaves too) no longer load** (`docs/decisions/0005`).

Since C6 colonists have food and rest (`docs/design/needs.md`): food falls 1.6 a day and rest 0.95 a
day in bands, as the RimWorld wiki says; a hungry pawn picks the tastiest food it can reach, takes it to
a chair next to a table if there is one (else eats where it lay) and eats; a pawn sleeps in its own bed,
else the nearest free one (which it then owns), else on the ground; the hourly schedule (Anything, Work,
Joy, Sleep; standard Sleep 22:00 to 05:59) sends tired pawns to bed, and a pawn hauling or waiting puts
the job down when it gets hungry or tired. Starving kills (`DeathCause::Starvation`). Commands
`SetSchedule` and `AssignBed`; events `PawnAte`, `PawnWokeUp`, actions `Eat`, `LieDown`, `GetUp`; the
snapshot has `food`, `rest`, `asleep`. Needs exist only where the data has a `NeedRules` def. The Crashlanded
colonists eat through their starting food in about 5 days and begin to starve around day 8, so **farming (C2, C3) and cooking (C7) are now the way to
keep them alive**. Beds, tables and chairs are C4's defs: it reads `building.bed`, `building.isTable`,
`building.isSittable` and `stats.bedRestEffectiveness`. Save format 8 (formats 6 and 7 still load).

Since F2 pawns can shoot (`docs/design/combat.md`): five guns from the wiki, real bullets that fly
with speed (RimWorld's x3, `tuning.json`) and drop, a spread cone instead of a hit roll (sized so a
standing man in the open is hit as often as RimWorld's hit chance; Shooting skill tightens it),
recoil, warm-up, bursts and cooldown. Bullets stop at the ground, rock, walls and sandbags (which
lose hit points and break) and pawn hitboxes for standing, crouching and prone (downed pawns lie
prone); armor (flak vest, flak pants, helmet) deflects or softens hits by RimWorld's rule, then the
part hit is injured. Commands `Equip`, `Shoot` (aimed, or the player's trigger) and `SetStance`;
events `ShotFired` and `ProjectileHit`; snapshot projectiles, stance, facing and gun; save format 11.
No fighter AI yet (F4, F5): pawns shoot only when told to.

Since D3 the game has real starting data in `data/defs/`, written by hand with RimWorld's numbers
from the RimWorld wiki (there is no RimWorld install to import from; `docs/decisions/0003`):
terrain fertility, wild plants and the temperate forest, basic items, the Human, skills, work
types, traits, our own backstories and names, the Crashlanded start, the Human body, capacities,
injuries and blood loss (`docs/design/starting-data.md`). `check_defs.py` now also checks that
defs name defs that exist. `simrun --pawns/--items` runs on the real data.
Not written yet: sandbags, thoughts, recipes and raiders; the tasks that use them
(C7, C9, F6) add them the same way. Guns, bullets, flak gear and hitboxes came with F2. C4 wrote the buildings (wall, door, bed, stool, three tables,
fueled stove).

Since C4 colonists build (`docs/design/construction.md`): `PlaceBlueprintCommand{def, stuff, cell, rot}`
plans a building, pawns with Construction work carry the materials to it (the blueprint becomes a frame
with the first delivery), build it at a speed from their Construction skill and Manipulation, and
`DeconstructCommand` takes a building down for half its materials back (`CancelBuildCommand` takes back
a plan and gives every delivered material back). Blueprints and frames are things that block nothing and
are not on the building layer; a finished building spawns, rebuilds the regions and pushes out pawns and
items it lands on. Wall, door, bed, stool, three tables and the fueled stove are in
`data/defs/Things/Buildings.json` (costs, work and hit points from the wiki), numbers in
`ConstructionRules`. Save format 10 (formats 6 to 9 still load); the snapshot has each plan's stage, progress
and materials. Not built yet: failed builds and skill learning, repair, roofs and rooms (C5).

Since 0004 the game has its own setting (`docs/design/lore.md`: the year 6650, slow star travel,
the fallen Lattice, magic called resonance) and our own names for RimWorld's proper names
(`docs/design/naming.md`: Impids are Emberkin, mechanoids the Remnant, insectoids chitinids,
storytellers are narrators named Orrin, Maren and Jinx). **Use those names in new data, code and
docs**; plain words like steel and rifle stay. Item words like plasteel are only proposed so far.

## Stage 1 progress

23 of 43 tasks done (D1, D2, D3, U1, S1, S2, S5, S6, F3, S3, U2, S4, U6, U5, C8, U12, F1, C1, U9, C6, C2, C4, F2). Tick tasks in `docs/plans/stage-1.md` as they merge.

## Next

Wave 1:
- ~~**D1** def database~~ done: `docs/design/defs.md`.
- ~~**D2** RimWorld importer~~ done: `docs/design/rimworld-import.md`. It has only run on test
  fixtures; nothing is imported yet.
- ~~**U1** smoke test harness~~ done: `tools/smoke.ps1`, `docs/design/smoke-tests.md`.

Wave 2:
- ~~**S1** things and the world layout~~ done: `docs/design/things.md`. `ColonySim.Build.cs` now
  exposes `ThirdParty/` (EnTT) as a public system include path; the next PC Unreal task is the
  first to compile it in Unreal.
- ~~**D3**~~ replaced: hand-written starting data instead of importing RimWorld's files
  (`docs/design/starting-data.md`). The importer stays in `tools/` unused.

Wave 3, both cloud:
- ~~**S2** regions and reachability~~ done: `docs/design/regions.md`. A door is a Things def with
  `thingClass` `Building_Door`. `simrun --regions FILE` draws the regions.
- ~~**S5** commands, events, snapshot and replays~~ done: `docs/design/bridge.md`. Only two dev
  commands so far (spawn and destroy a thing); each later system adds its own. New sim state
  must go into `Sim::stateHash()`.

Wave 4, now open: ~~**S3** hierarchical pathfinding~~ done (`docs/design/pathfinding.md`; CI runs
the 1 km, 100 pawn check as `simrun_paths`; the path queue is in the save, save format 2),
~~**S6** save and load~~ done
(`docs/design/saves.md`; saves keep the command log, so a loaded session still records a full replay;
the entity-registry hash order changed, so replays recorded before S6 have different hash values),
~~**F3** cover, sight and threat maps~~ done (`docs/design/cover.md`; `simrun --sight
FILE` draws what a pawn sees; sandbags and barricades were added to the importer list, and
trees and rocks get their height from `fillPercent` when C2 adds plants) and ~~**U2** bridge v2~~
done (PC Unreal; `docs/design/bridge.md`, smoke test `sim`; the first Unreal compile of EnTT and the
S5 bridge, so S1's include path is confirmed). ~~**C2** plants~~ done (`docs/design/plants.md`; save format 9), ~~**C8** colonists
from a seed~~ done (`docs/design/colonists.md`; `simrun --start Crashlanded` lists the six; save format 4) and
~~**F1** health basics~~ done (`docs/design/health.md`; save format 5) and ~~**F2** guns, projectiles
and hitboxes~~ done (`docs/design/combat.md`; save format 11; U11 can now draw tracers and hits);
their defs are in `data/defs/`. Numbers marked `unverified` in a def were recalled, not read from the
wiki; check them when a task leans on them.

Wave 5: ~~**S4** smooth movement~~ done (`docs/design/movement.md`; save format 3; the hauling
placeholder asks for a new path when a pawn gives up a walk; path *choice* still ignores slopes and
sand, the doc says why). ~~**C1** job system~~ done (`docs/design/jobs.md`; save format 6; new jobs add a
`JobType`, their steps and a `WorkGiver`, the doc says how). ~~**U5** pawns with animation by distance~~ done (PC Unreal; smoke test `pawns`;
`docs/design/pawns.md`; the sim's new `PawnAction` event, which C1's jobs should send for their
actions too). ~~**U6** overseer camera and selection~~ done (PC Unreal;
smoke test `overseer`; `docs/design/overseer.md`). ~~**U9** protagonist~~ done (PC Unreal; smoke test
`protagonist`; `docs/design/protagonist.md`; save format 7). **U3** terrain and plants (PC Unreal, Sonnet high)
has C2 now, so it is open.

Wave 6, open now that C1 is in: ~~**C4** building~~ done (`docs/design/construction.md`; save format 10; C5 doors,
roofs and rooms and C7 bills and cooking can start now), ~~**C6** food, rest and
schedules~~ done (`docs/design/needs.md`; save format 8), ~~**F2** guns and hitboxes~~ done; **C3** mining, chopping
and harvesting (cloud, Sonnet high) is open now that C2 is in. On the PC, ~~**U9**~~ is done and **A3** needs only done tasks.
U6's work tab and a colonist panel can now read `sim.jobs()` (work types, priorities).
With F2, F3 and C1 in, **F4** fighter AI: where to stand (cloud, Opus high) is open, and with U9 and
F2 in, **U11** fights on screen (PC) is too.

## Waiting on sneed

- Megascans: download terrain material packs from Fab for task A2 (not needed before wave 10;
  that thread sends the steps).
