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

Since A3 pawns have seeded looks (`docs/design/looks.md`, smoke test `looks`): every humanlike pawn
gets a lineage, body type, weight, height, skin and hair colour from a seed (`PawnLooks`, save format 14;
`data/defs/Lineages`, `LooksRules`; dev command `SetLineage`). As MetaHumans they get real body shapes
(thin, heavy, female, from the engine's MetaHuman body type files) as morph targets on the body *and*
the clothes, our own skin material coloured per pawn, their height as scale, and Emberkin get red skin
and horns. The shapes and materials are made by the `PawnLooks` commandlet (new editor module
`Rim3DEditor`) into `game/Content/Rim3D/Generated/` (not in Git; `assets/manifest.json` says how to
make them again). Everyone still has Kellan's face and outfit.

Since U4 the game draws buildings and items (`docs/design/building-draw.md`, smoke test `buildings`): `UBuildingSubsystem` reads the snapshot and keeps one instanced mesh component per kind of thing per 32 m chunk, rebuilding only chunks whose things changed. Walls, doors, beds, stools, tables and the stove are boxes and cylinders from each def's `visual.parts`, coloured by their stuff; blueprints are blue see-through ghosts, frames rise inside their ghosts as materials arrive and work is done; a door's leaf slides into the wall as the sim's door opens and shuts; items are little piles that grow with the stack. Pieces of this are shared with the terrain's plants (`VisualMesh.*`). Ghosts use a material the `BuildingMaterials` commandlet makes (`assets/manifest.json`; the engine's own translucent materials draw black on instanced meshes). Picking uses the drawn height of a building. Everything stays on the ordinary static-mesh path, so DLSS stays possible.

Since U7 the player gives orders (`docs/design/orders.md`, smoke test `orders`): `UOrdersSubsystem` holds a tool for the left mouse button. The bottom bar has Orders (Mine, Chop trees, Harvest, Deconstruct, Cancel), Zones (Stockpile, Remove zone) and one button per architect category from the defs' `designationCategory`; a building is chosen with a stuff, shown as a grid-snapped ghost that is green where it can be planned and red where it can't (R turns it; a 1 x 1 building can be dragged out along a line), and everything else is dragged out as a rectangle. The sim's marks are drawn from the snapshot as instanced meshes (orange mine, green cut, yellow harvest, red deconstruct, blue stockpile cells). R drafts the selected colonist, a right click with a drafted colonist selected is a move order (on ground) or an attack order (on a pawn of another side), drafted colonists get a red ring and lines to their spot and target, and the Alarm button drafts everyone (a placeholder until F6's defence plans). Orders given while the game is paused make the sim step once so they show. Esc puts a tool down before it clears the selection. No sim changes.

Since U8 the overseer has colony panels (`docs/design/panels.md`, smoke test `panels`): `UPanelsSubsystem` draws a top-left bar with Work, Schedule and Log buttons and one button per colonist coloured by mood; the Work tab (a priority 0 to 4 per colonist and kind of work, left click up, right click back) and the Schedule tab (anything, work, joy or sleep per hour); with a colonist selected a panel with needs, mood and its thoughts, skills and passions, health and bed (assign or free); with the stove selected its bills (add, change mode and count, pause, remove); and a message log under the alarm (deaths, wounds, mood trouble, finished bills, refused orders). All through commands, none changes the sim.

Since U11 fights show on screen (`docs/design/fights.md`, smoke test `fights`; no sim changes): `UFightSubsystem` draws a muzzle flash (blobs, a short light, smoke) at every `ShotFired`, a tracer that flies from the muzzle to where the bullet ended at the bullet's own speed in real time, and an impact when it arrives (dust, sparks and chips, blood); a pawn that is hit flinches (a lean); downed and dead pawns become physics ragdolls on an invisible floor made under them; hits on flesh leave a dark patch on the body at the nearest bone and blood on the ground (**needs `game/Content/Rim3D/Generated/Fights/M_WoundDecal`, made once per machine by the `FightMaterials` commandlet, `assets/manifest.json`**); armed pawns show a placeholder gun floating at the chest (from the weapon def's `visual.parts`); standing pawns that aim face the sim's facing. In direct mode the left mouse button fires at the crosshair through the sim (`ShootCommand` with `player`, aimed by the sim's own `trace`), R reloads, C crouches, Z lies down, E picks up the nearest gun, and a small HUD shows the gun and rounds. `Fight.Hitboxes 1` draws every pawn's hitboxes, and every pawn hit is checked to lie on them (within 5 cm in the smoke test). `Fight.Effects 0` turns the effects off. Not here: a real gun in the hands or aim animations, sounds, first-person gun.

Since U10 the player works with their own hands (`docs/design/handson.md`, smoke test `handson`, tests `HandsOnTests`): in direct mode the crosshair finds a rock, plant, item, building plan or frame (against the sim, not Unreal collision) and **holding F** runs the sim's own job on it in place with the colonist's skill and speed: mine, chop, pick, pick up (at once), put materials into a plan, build a frame; **X** puts down what is carried; **B** takes the architect menu's Build tool in hand with a grid-snapped ghost aimed by the crosshair (wheel picks the building, Q the stuff, R turns, click plans, and the gun stays quiet meanwhile). Sim side: `HandsOnCommand{pawn, mode, thing, cell}` (Work, PutDown, Stop), jobs with a `handsOn` field that skip the walking steps and end as interrupted if the player steps out of reach (**a job on a `PlayerControl` pawn is ticked only if `handsOn`; the think list still skips it**), a made-for-the-job designation that goes again if the work is dropped, `JobSystem::planHandsOn` (the prompt), `jobProgress` (the bar; `ThingView::jobProgress`), save format 19. Letting go of the colonist puts down what it carries. No work animation or sound yet.

Since U3 the game draws the ground and plants (`docs/design/terrain.md`, smoke test `world`): `UTerrainSubsystem` starts
by itself with any sim and builds one mesh per 32 m chunk (a 1 km map is 32 x 32 chunks, about 220 ms
to build). The ground has a vertex per cell corner at the sim's height, coloured by terrain type and soil fertility; **rock stands on it as a
solid 4 m block** (`TerrainDraw::RockHeightM`, picking and the overseer camera know), and a mined cell becomes a rough stone floor, with the mountain's thick
roof still over it. The thin roof of C5's rooms is a second section of the chunk; **roofs are hidden in the overseer view and shown in direct mode**
(`Terrain.Roofs` overrides). Mining rebuilds only that chunk (about 0.1 ms). Plants are instanced low-poly meshes made at start from
each plant def's new `visual` field (stem and crown shapes with colours; A1 can set `visual.mesh` instead), one component per chunk and kind, with cull
distances for grass; the sim's plant events and a rotating scan of chunk plant sizes keep them current, no snapshot copy. Looks are placeholders
(flat colours, engine vertex-colour material) until A2. The old Stage 0 terrain test is still the `terrain` smoke test; the other smoke tests now
get their ground from this subsystem. No sim changes except `USimSubsystem::GetSimGeneration`.

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

Since C3 colonists mine, chop and harvest on the player's marks (`docs/design/mining.md`): the map has a rock layer
(`sim.mineables()`: 2 or 3 rock types in patches and ore lumps inside them, each cell with hit points), `DesignateCommand` and
`CancelDesignationsCommand` mark rock, trees and bushes, and the Mining and Plant cutting work types dig, fell and pick. Mined rock
leaves chunks (or steel, silver, components) and the cell becomes rough stone, so regions and paths open up. Speed and yield follow the
Mining and Plants skills, which also gain XP. Events `CellMined`, `PlantRemoved`, `PlantHarvested`, `DesignationsChanged`; the
snapshot lists the marks; save format 14. `simrun --gather RADIUS` marks everything near the colonists and reports what got done.

Since C9 colonists have a mood, 0 to 100 (`docs/design/mood.md`): base 50 plus trait offsets plus thoughts, the
level following at 12 an hour up and 8 down and standing still asleep. Situations (hungry, ravenous, starving;
drowsy, tired, exhausted) count while they hold; memories (ate without a table, ate raw food, ate a fine or
lavish meal, slept on the ground) last 0.6 days. The three break thresholds (35%, 20%, 5%, moved by traits
like Nerves) are recorded as `MoodBand` with a `PawnMoodBand` event; **no mental breaks yet**. "Slept outside"
starts when a pawn wakes up outdoors (C5). `thoughts()` is what the colonist panel (U8) reads. Thought
numbers other than Hungry are recalled, not verified. Save format 11.

Since C10 there is one test that runs the whole colony together (`docs/design/colony-scenario.md`,
`simrun --scenario colony`, in CI on seeds 42 and 7): the six Crashlanded colonists build a walled room with a door,
six beds, a table with stools and a stove, mine, chop and pick on marks, cook from a bill and sleep in assigned beds for 5 days. It fails
if anyone dies or starves, stands stuck for a game hour, or a mark or building is left undone. Its first runs found four real
bugs, now fixed: a wall finished under a walking pawn left the pawn thinking it had arrived; pawns walking past shoved builders and
cooks out of their working cell (new `Mover::hold`, set by the job system for stand-still steps); two pawns slid along a wall for
65 game hours without giving up (sliding is no longer progress, so they time out and re-path) and starved; steel lying under a new
wall was destroyed instead of moved, failing the delivery that was fetching it. **Anything new that colonists do should get a line
in that script.** No save format change (`Mover::hold` is not saved).

Since F2 pawns can shoot (`docs/design/combat.md`): five guns from the wiki, real bullets that fly
with speed (RimWorld's x3, `tuning.json`) and drop, a spread cone instead of a hit roll (sized so a
standing man in the open is hit as often as RimWorld's hit chance; Shooting skill tightens it),
recoil, warm-up, bursts and cooldown. Bullets stop at the ground, rock, walls and sandbags (which
lose hit points and break) and pawn hitboxes for standing, crouching and prone (downed pawns lie
prone); armor (flak vest, flak pants, helmet) deflects or softens hits by RimWorld's rule, then the
part hit is injured. Commands `Equip`, `Shoot` (aimed, or the player's trigger) and `SetStance`;
events `ShotFired` and `ProjectileHit`; snapshot projectiles, stance, facing and gun; save format 12.
The fighter AI that uses them is F4's (next paragraph).

Since F4 pawns fight sensibly (`docs/design/fighters.md`): `DraftCommand` takes a colonist off work and
the fighter AI on; `MoveOrderCommand` (go there and hold, in the best cover within 2 m) and
`AttackOrderCommand` (shoot this one) are RimWorld's move and attack. Pawns have sides (`Faction`;
none means the player's) and every other side's pawn fights by itself and advances; until raids (F6)
the dev command `SetFactionCommand` makes enemies. Twice a second a fighter looks for enemies (its side
shares what it sees), picks the closest able one, scores the cells within 10 m for cover against the 4
nearest threats, being seen, having a shot, range, walking and crowding, then walks there, crouches,
and pops up over sandbags or steps out past a wall's end to shoot, then ducks back. Outnumbered two to
one or badly hurt it falls back. The F3 threat map isn't used (too slow to rebuild; fighters.md says
why); 30 against 40 costs about 0.35 ms a step in the cloud. Snapshot `faction`, `drafted`,
`fighting`, `fightTarget`, `fightSpot`, `fallingBack`; save format 15. **A new system that acts on
pawns must skip pawns with `Fighter`** as the job system does. Not yet: squads, morale, raids and
the alarm (F6); undrafted colonists ignore enemies.

Since F5 fighters aim like players (`docs/design/aiming.md`): Shooting skill sets a reaction time
before shooting a new target (0.5 s at skill 0, 0.17 s at 20), how well a shot leads a moving target
(a skill 20 shooter hits a man walking past about as often as one standing, a novice almost never)
and reload speed. Guns have magazines (revolver 6, assault rifle 30...; ours) that reload by themselves
when empty, `ReloadCommand` reloads early, and fighters top up in cover when they have no shot.
Fighters sidestep slow projectiles they see coming in time (bullets are far too fast; for grenades
and arrows later). Events `PawnAction` `Reload` and `Dodge`; snapshot `rounds`, `magazineSize`,
`reloading`; save format 17.

Since D3 the game has real starting data in `data/defs/`, written by hand with RimWorld's numbers
from the RimWorld wiki (there is no RimWorld install to import from; `docs/decisions/0003`):
terrain fertility, wild plants and the temperate forest, basic items, the Human, skills, work
types, traits, our own backstories and names, the Crashlanded start, the Human body, capacities,
injuries and blood loss (`docs/design/starting-data.md`). `check_defs.py` now also checks that
defs name defs that exist. `simrun --pawns/--items` runs on the real data.
Not written yet: sandbags, thoughts and raiders; the tasks that use them
(C9, F6) add them the same way. C7 wrote the recipes (simple meal, four simple meals, fine meal) and `BillRules`. Guns, bullets, flak gear and hitboxes came with F2. C4 wrote the buildings (wall, door, bed, stool, three tables,
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
and materials. Not built yet: failed builds and skill learning, repair.

Since C5 doors open and cost time, enclosed rooms get a roof, and rooms have roles (`docs/design/rooms.md`). A pawn
can't walk into a closed door's cell: it waits at the edge while the door opens (23 steps) and the door stays open
55 steps after the last pawn; paths count a door as 3.5 m extra; an open door stops blocking sight. A room is
the regions joined across every border except doors; a room that reaches the map edge or is over 2,000 cells is the
outdoors; any other room gets a thin roof by itself (cells within 6.9 m of a wall, door or rock) and a role from what is
in it (2 beds Barracks, 1 Bedroom, a stove Kitchen, a table DiningRoom). `sim.rooms()` answers `roomAt`, `roleAt`,
`indoors`; the snapshot has the rooms, the roof cells and each door's openness; "slept outside" now starts. Rooms and
roofs are rebuilt from the walls, not saved; save format 13 adds door states. Not built yet: roof collapse, room
temperature, cleanliness and beauty, locked and per-faction doors, owner-assigned bedrooms.

Since C7 the stove cooks (`docs/design/bills.md`): `AddBillCommand{building, recipe, mode, count}` puts a bill
on a work building (modes: do X times, do until you have X, do forever; `RemoveBillCommand`, `SetBillCommand` change
or pause it), and colonists with Cooking work fetch the ingredients (the nearest raw food to the stove, up to 8 stacks,
all reserved), work at a speed from their Cooking skill and Manipulation, and drop the meals by the stove, where
haulers and hungry pawns find them. Recipes are a new def kind (`data/defs/Recipes/`): simple meal (10 berries, 300
ticks), four simple meals at once, fine meal (needs Cooking 6 and meat, which no item is yet). Simple and fine meals
are in `Items.json`. A job that ends early gives the ingredients back. New job `DoBill`, event `RecipeDone`, action
`Cook`; save format 18 (formats 6 to 17 still load). **Not built: stove fuel (it cooks for free; in BACKLOG), skill
learning, bill reordering.**

Since 0004 the game has its own setting (`docs/design/lore.md`: the year 6650, slow star travel,
the fallen Lattice, magic called resonance) and our own names for RimWorld's proper names
(`docs/design/naming.md`: Impids are Emberkin, mechanoids the Remnant, insectoids chitinids,
storytellers are narrators named Orrin, Maren and Jinx). **Use those names in new data, code and
docs**; plain words like steel and rifle stay. Item words like plasteel are only proposed so far.

## Stage 1 progress

36 of 43 tasks done (D1, D2, D3, U1, S1, S2, S5, S6, F3, S3, U2, S4, U6, U5, C8, U12, F1, C1, U9, C6, C2, C4, C9, F2, C5, C3, F4, A3, F5, U3, U4, U7, C7, U8, U11, U10). Tick tasks in `docs/plans/stage-1.md` as they merge.

## Stage 2

Planned 2026-10-25 in `docs/plans/stage-2.md`: "a colony that lasts a year", 74 tasks in seven
waves (seasons, weather, temperature, light, fire, power, farming, crafting, apparel, research,
medicine, animals, melee and grenades, two-storey buildings, the narrator and events, prisoners,
traders, protagonist talents and loadout). Its extra rules keep parallel threads from colliding.
**Wave 1 is open now in the cloud, alongside the rest of Stage 1:** S7 (saves and commands that
merge in parallel; every sim task that saves state waits for it), E3 light, and the data tasks D4,
D5, D6, D7, D8, D9, D11. D6 (apparel and textile data) is in; 1 of 74 done. Tick tasks in `docs/plans/stage-2.md` as they merge.

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
and hitboxes~~ done (`docs/design/combat.md`; save format 12; U11 can now draw tracers and hits);
their defs are in `data/defs/`. Numbers marked `unverified` in a def were recalled, not read from the
wiki; check them when a task leans on them.

Wave 5: ~~**S4** smooth movement~~ done (`docs/design/movement.md`; save format 3; the hauling
placeholder asks for a new path when a pawn gives up a walk; path *choice* still ignores slopes and
sand, the doc says why). ~~**C1** job system~~ done (`docs/design/jobs.md`; save format 6; new jobs add a
`JobType`, their steps and a `WorkGiver`, the doc says how). ~~**U5** pawns with animation by distance~~ done (PC Unreal; smoke test `pawns`;
`docs/design/pawns.md`; the sim's new `PawnAction` event, which C1's jobs should send for their
actions too). ~~**U6** overseer camera and selection~~ done (PC Unreal;
smoke test `overseer`; `docs/design/overseer.md`). ~~**U9** protagonist~~ done (PC Unreal; smoke test
`protagonist`; `docs/design/protagonist.md`; save format 7). ~~**U3** terrain and plants~~ done (PC Unreal; smoke test `world`;
`docs/design/terrain.md`); ~~**U4** buildings and items~~ done (PC Unreal; smoke test `buildings`; `docs/design/building-draw.md`); ~~**U7** orders~~ done (PC Unreal; smoke test `orders`; `docs/design/orders.md`); **A2** terrain materials (needs sneed's Megascans) is open now.

Wave 6, open now that C1 is in: ~~**C4** building~~ done (`docs/design/construction.md`; save format 10; ~~**C5** doors,
roofs and rooms~~ done (`docs/design/rooms.md`; save format 13), ~~**C7** bills and cooking~~ done (`docs/design/bills.md`; save format 18; ~~**U8** colony panels~~ done (PC Unreal; smoke test `panels`; `docs/design/panels.md`)), ~~**C6** food, rest and
schedules~~ done (`docs/design/needs.md`; save format 8), ~~**F2** guns and hitboxes~~ done; ~~**C3** mining, chopping
and harvesting~~ done (`docs/design/mining.md`; save format 14). On the PC, ~~**U9**~~ and ~~**A3** seeded looks~~ are done (smoke test `looks`; save format 16).
U6's work tab and a colonist panel can now read `sim.jobs()` (work types, priorities).
~~**F4** fighter AI: where to stand~~ done (`docs/design/fighters.md`; save format 15), ~~**F5** fighter
AI: aiming~~ done (`docs/design/aiming.md`; save format 17), so **F6** raid and defense (cloud, Opus high)
is open once C8 is in (it is); with U9 and F2 in, ~~**U11** fights on screen~~ is done (PC; smoke test `fights`; `docs/design/fights.md`; it
binds R to reload; reload and sidestep animations are not drawn yet). ~~**C10** colony scenario test~~ done
(`docs/design/colony-scenario.md`); ~~**U10** hands-on work~~ is done (PC; smoke test `handson`; `docs/design/handson.md`); open on the PC: **A1** placeholder art.

## Waiting on sneed

- Megascans: download terrain material packs from Fab for task A2 (not needed before wave 10;
  that thread sends the steps).
