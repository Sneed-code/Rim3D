# Status

Updated 2026-10-26.

Stage 3 W2 done: the game can be played on a world. `SimSettings::world` (off by default) makes the world before the map; the start tile (the player's, or picked by the scenario's biome) gives the colony's map its seed, its biome (plants, wild animals, weather) and its yearly mean temperature (the biome's seasons, moved to the tile's climate). `Sim::world()` and `Sim::site()` are what W3 (world screen), S8 (hills, rivers, roads and terrain from the site) and W4/W5 build on. Save section `world` 1 (made again on load and checked by hash), replays carry `settings.world`; `simrun --on-world [--world-tile N]` (`docs/design/world-map.md`, tests `WorldSimTests`, CTest `simrun_on_world`). Settlement state (goodwill per settlement, destroyed bases) waits for the tasks that change it.

Stage 3 W1 done: the world map. A seeded planet of 120 by 60 wrapping tiles with elevation, temperature, rainfall, a biome from the Biomes defs by data rules, hills, rivers that run downhill into the sea or a lake, about 86 settlements of the humanlike factions with territory, roads between near settlements, and choosing a start tile (`World::generate`, `chooseStartTile`, `suggestStartTile`, `siteFor` for the map generator); `simrun --world world.png --check` draws it (`docs/design/world-map.md`, `docs/plans/stage-3.md`, defs `WorldGen`, tests `WorldTests`). Sim only.

Stage 2 F12 done: raid strategies. `StartStrategyRaid` (and `RaidSystem::startRaid`/`pickStrategy` for the narrator) sends a raid as Assault, Sappers (dig through the wall that shuts the colony in, 4 hit points a second each), DropPods (land 8 to 24 m from the target, inside walls too), Thieves (each lifts the most valuable loose item and runs off the map with it) or Kidnappers (carry a downed colonist off the map for good; never the protagonist), or `Auto` by weights (sappers favoured when walls shut the colony off), and `hour` holds a raid for a night attack (`docs/design/raid-strategies.md`, tests `RaidStrategyTests`, `simrun --raid POINTS --raid-strategy NAME`). Save section `raids` 2. Sim only; T1 (PR #98) should call `pickStrategy`.
Stage 2 F7 fix done: the raid soak's findings are fixed. Fighters pick spots by the real effort of the walk (slopes, hurt pawns), keep their spot when held up in a crowd, don't plan shots at the very edge of their gun's range, advancing raiders follow the squad's route when no spot gives a shot, and a squad of hurt-but-up pawns routs. Over 80 seeds the open share is 6 to 9% (was 13.6), raids still on after a quarter day 0 to 4 (was 4 of 37) and stuck fighters 1 to 2 (was 18 of 40 seeds); the soak's limits are now the 10% rule and one seed in 20 (`docs/design/raid-soak.md`). Doctors no longer chase a hurt colonist who is walking about in a mental break; they tend it when the break ends.
Stage 2 C25 done: good-mood colonists get inspired. Every 48 minutes of game time a colonist at mood 50 or more rolls (mean wait 210 days at mood 50, 15 at mood 100) and may get inspired creativity (its next craft is two quality levels better, the only way to legendary), surgery (its next tend keeps the medicine), recruitment (the next attempt has twice the chance), trade (better prices once trading exists) or a shooting frenzy (less spread, faster cooldown), for about 2.5 days or until used (`docs/design/inspirations.md`, tests `InspirationTests`, events `InspirationStarted` / `InspirationEnded`, save section `inspirations`; no work frenzy yet).
Stage 2 H2 done: wounds can turn infected (tending in time stops it), pawns catch flu, plague, malaria, sleeping sickness, gut worms, muscle parasites and food poisoning, and fight them in RimWorld's immunity race; doctors tend them, the sick rest in bed, T3 can start outbreaks (`docs/design/diseases.md`).

Stage 2 smelter and crematorium done: an electric smelter smelts weapons and clothes into 25% of their metal (components and cloth are lost) or just destroys them, and an electric crematorium (worked by haulers) burns corpses, weapons and clothes; checked against the vanilla bench list, the missing ones are in the open gear (#170), drug (#163) and art (#155) PRs (`docs/design/crafting.md`, tests `SmeltingTests`, `HuntingTests`). Sim only.
Stage 2 long-lasting foods done: pemmican (16 from meat and plants at a stove, `Pemmican` research, keeps 150 days, traded by the clan shaman), the packaged survival meal can be cooked (Cooking 7, keeps 60 days) and both go through the C16 rot rules; a recipe's `fuelUses` sets the wood a run burns, so 50 kibble or 16 pemmican burn one wood, not 50 or 16 (`docs/design/crafting.md`, tests `PreservedFoodTests`, `FuelTests`). Raid and cargo-pod loot of survival meals is not in yet (BACKLOG).
Stage 2 adaptation done: colony wealth (items, buildings at half, colonists and tame animals) and the raid points formula were already in from F6 and T1; what was missing is now in: the wiki's adaptation. The colony's AdaptDays start at 0 (factor 0.8), grow every 12 hours (not above 0 in the first 30 days), fall by 30 when a colonist dies and by 3.5 to 8 when one goes down, and the difficulty's adaptationGrowth and adaptationImpact scale them, so raids get tougher over a calm year and eased after losses (`docs/design/narrator.md`, tests `RaidTests`). Save section `raids` 2. The growth table's per-day reading is unverified. Sim only.
Stage 2 T1 done: the narrator (Orrin, Maren, Jinx) sends incidents by RimWorld's rules (threat cycle, mean days between, weights, earliest days, cooldowns) with threat points from wealth, colonists, difficulty and narrator; the raid is the first incident; letters are `Letter` events (`docs/design/narrator.md`). **The narrator is off until a `SetNarratorCommand`: U20 sends it at game start. Event tasks add a worker per incident (`sim.narrator().setWorker`).**
Stage 2 quests done (sim side): a quest is an offer with rewards that waits a few days for accept or decline. Four kinds need no world travel: refugees run in with raiders behind them (paid when the raid is over), a wanderer asks to join, a supply cache is dropped for the colony, and "hold the line" sends a raid for a reward. The narrator offers them with `quests().offerRandom` or the `Quest` command; the snapshot lists them (`docs/design/quests.md`, tests `QuestTests`, defs `Quests/`, `Rules/QuestRules`). Save section `quests`. Sim only; no quest screen yet.
Stage 2 spaceship ending done (sim): research chain (Starflight basics, ship cryptosleep, starship reactor, ship drive, Sleeper persuasion), six ship parts with the wiki's sizes and costs, `ShipCommand` StartReactor / Board / Launch: the reactor takes 15 days while waves of raiders come, launch needs 3 engines, a core, a sensor cluster and a rider in a casket touching a beam, then `Sim::ship().won()` (`docs/design/ship.md`, tests `ShipTests`). Save section `ship` 1. Research costs and the sensor cluster are ours (unverified).
Stage 2 colonist needs extras done: a colonist outside its safe temperature walks to the nearest comfortable (else safe) cell and waits there (`SeekShelter` job, `docs/design/temperature.md`); Gourmand's faster hunger, Quick sleeper's faster rest, Night owl's moods awake at night or by day and Greedy's mood without a bedroom are in (`docs/design/needs.md`, `mood.md`). Tests: `ColonistTraitNeedsTests`. Save format unchanged (new job type number at the end).
Stage 2 animal extras done: tame animals can be named (`SetAnimalName`), one command marks every wild animal in a rectangle to tame or hunt, or every tame one to slaughter (`MarkAnimalsInArea`), and doors are gates to animals: they never walk through one (`docs/design/husbandry.md`, tests `HusbandryTests`). Save section `husbandry` 3. Sim only.

Stage 2 hydroponics done: a powered hydroponics basin (new research `Hydroponics`, 70 steel, 70 W) gives its cell fertility 2.8 and takes a growing zone, plant pots count as fertility 1.0 spots, and a powered sun lamp within 5 m lets a lit crop grow through the night as well, so a heated, roofed room with basins and a sun lamp feeds the colony through winter (`docs/design/farming.md`, tests `HydroponicsTests`). Sim only.
Art stage planned: `docs/plans/art-stage.md` (26 tasks V1 to V26: faces and hair, body hair, anatomy, jiggle, wounds, dismemberment, destructible clothes, real animation, Niagara fire, real buildings and items, destruction, sound, styled UI). Docs only.
Art stage V21 cloud half done: `assets/sound_sources.json` names free sources (Kenney, OpenGameArt, Sonniss, Freesound CC0) with licences and a cue map for all 140 cues, `tools/sound_assets.py` downloads them (PC) and `--check` runs in CI. Gap: no free animal calls found; fire, hums, weather and ambience need Sonniss/Freesound by hand (`docs/design/sound.md`). Docs and tools only.

Stage 2 heat armour done: a burning pawn's flames now go through the heat armour of the clothes over the part they hit (bounce, half damage, wear on the piece), so a hyperweave duster and hat save most of the burns; clothes already carried the heat stat from RimWorld's numbers (`docs/design/fire.md`, test in `FireTests`). Sim only. Also folded in (from PR #132): hunters skip animals that turn manhunter when hurt at 10% or more, and a butcher table can cut up dead people (`Meat_Human`, `Leather_Human`).
Art stage V20 cloud half done: `docs/design/sound.md`. 140 sound cues are defs (`data/defs/SoundCues`, each with range, priority, volume and a placeholder tone), and defs name theirs under `visual.sound` (gun `shot`, damage type `hit`, terrain `step`, door `open`/`close`, bench `work`, animal `idle`/`hurt`/`death`, weather `loop`/`thunder`, biome `day`/`night`...). `Sound.h` `soundForEvent` turns sim events into cues (tested), and `check_defs.py` fails on a cue that is missing or unused. Not yet: the Unreal half (`USoundSubsystem`, voice budget, occlusion, ambience, mixer) is the PC task V20, and real sounds are V21.
Stage 2 heat armour done: a burning pawn's flames now go through the heat armour of the clothes over the part they hit (bounce, half damage, wear on the piece), so a hyperweave duster and hat save most of the burns; clothes already carried the heat stat from RimWorld's numbers (`docs/design/fire.md`, test in `FireTests`). Sim only.

Stage 2 human flesh moods done: butchering a person gives the butcher a bad memory (-15, 6 days), eating human meat -20 (-10 in a meal cooked from it), wearing human leather -4; cannibals don't mind the butchering and like the meat (+20 raw, +10 in a meal) and the leather (+4), psychopaths feel nothing for butchering or eating and like the leather (+4) (`docs/design/hunting.md`, tests `HumanFleshTests`). Numbers are unverified. Sim only.
Stage 2 colony "knows" moods done: when someone butchers a person every other colonist gets "knows someone butchered a person" (-5, 6 days; not cannibals or psychopaths), and when a colonist dies every other living colonist (psychopaths aside) gets "colonist died" (-5, 15 days). Prisoners, guests and enemies do not count. Executions and organ harvesting have no sim yet (BACKLOG). Numbers are unverified. Tests `HumanFleshTests`. Sim only.
Stage 2 U14 done: lamps, fires and burnt ground are on screen. Every lamp the sim has lit (campfire, torch, standing and sun lamps) is a real warm light that goes out when its fuel or power does; campfires and torches have flames and embers; each burning cell has flames, embers and smoke in proportion to its size, with an orange light per 4 m block of fire; burning pawns have flames and a light; burnt ground is a charcoal skin and ash a grey patch, exactly the sim's cells (`docs/design/fire-draw.md`, smoke test `fire`; `UFireSubsystem`; defs got `glower.lift` and `visual.flame`; no sim changes). Particles are instanced meshes like the fight effects (the engine's unlit material does not work on instances), so flames are lit; real Niagara fire is later.

Stage 2 year-test fix: a pawn carrying a corpse or a downed pawn (butcher, corpse hauling, rescue) no longer stops dead because of the body it carries: the movement system ignored nothing, so the carried body, lying exactly where the carrier stands and unable to move, counted as a pawn in the way. Before it, butchers froze for up to 2,000 game minutes mid-walk (and a colonist starved with 180 meals in stock because the only other healthy colonist was frozen); with it the stuck-butcher reports on seeds 1, 7 and 42 are gone (`docs/design/movement.md`).
Stage 2 year-test fixes (not a plan task): colonists no longer sow a crop that frost would kill before it ripens and gets picked (they look at the weather ahead, `docs/design/farming.md`), filth under the open sky fades over about six days and faster in rain so animal blood no longer piles up (`room-stats.md`), an unarmed colonist runs for shelter when the alarm sounds instead of punching a wolf and an armed one stands and shoots when something as fast as it is at arm's length (`raids.md`, `fighters.md`), and the year run arms its best shot, hunts, butchers and fixed a crash at its end. On seeds 1, 7 and 42 no crop is lost to frost any more and one colonist of six is alive on day 60 (all six died by day 51 before); what still kills the rest (winter cold, one armed colonist, predators, no winter food) is in `BACKLOG.md` and `docs/design/colony-scenario.md`.
Stage 2 early year test (not a plan task): `simrun --scenario colony --year` plays the colony for 60 game days with animals, a rice field and a stockpile. On seeds 1, 7 and 42 nothing crashed, nobody got stuck and the run is deterministic, but all six colonists starve by day 37 to 51 once autumn frost kills the crops; the list of what is missing is the first entry in `BACKLOG.md` and C25 should start from it (`docs/design/colony-scenario.md`, "The year run").
Stage 2 N2 done: a hunt mark on a wild animal sends a colonist with a gun and the Hunting work to shoot it dead (it follows a runner, and a hurt animal may turn on the hunter through N5), haulers carry hunted and slaughtered animals' corpses to a stockpile, and a butcher table's bill carries a corpse to the table and cuts it up into meat and leather by RimWorld's butchery efficiency (75% plus 2.5% per Cooking level), so the fine meal can now be cooked from hunted meat (`docs/design/hunting.md`, tests `HuntingTests`). Save section `husbandry` 2.
Stage 2 M4 done: `simrun --scenario house` (CTest `house_scenario`, seeds 42 and 7, in CI) has the six Crashlanded colonists build a two-storey steel house (ground walls and door, stairs, a wood upper floor over all of it, upper walls, six beds), sleep upstairs and haul cloth up and stone blocks down the stairs for three days, and writes `house-level0.png` and `house-level1.png`. It found that an upper corner wall cannot be reached once its two neighbours stand (nothing to stand on outside the ring), so the script builds corners first (`docs/design/house-scenario.md`). Sim only, no sim code changed.
Stage 2 M3 done: roofs fall when their support goes. A built roof, the mountain's roof over a mined-out cell (a cave wider than about 14 m with no wall inside) and an upper floor all need a wall, column or rock within 6.9 m; when the last one goes the roof or floor falls and crushes what is under it, floors drop their pawns and items a level and lose their buildings, and a chain of falls ends in one step (`docs/design/roofs.md`, event `RoofCollapsed`, `Snapshot::roofCollapses`, save section `roofs`).
Stage 2 U21 done: shooter feel on screen. In direct mode holding the right mouse button with a gun blends the view to over the shoulder (or first person with `Feel.AimView 1`); the sim's gun kick lifts the view while the crosshair, and so the shots, stay put; shots, hits, knock-downs and blasts shake the camera and pulse motion blur, vignette and fringe; Left Shift rolls (the body somersaults in time with the sim's 27 steps, the camera ducks); G throws a grenade; grenades in flight are drawn and blasts make a fireball, smoke, sparks, a flash and Chaos debris thrown over an invisible floor; the overseer's folded Feel panel has sliders for shake, blur, kick and aim view (`docs/design/shooter-feel-draw.md`, smoke test `feel`; `UShooterFeelSubsystem`; no sim changes).
Stage 2 M2 done: stairs and ladders link levels; colonists climb them, build upper floors with materials and work, and haul, build and sleep across levels (`docs/design/levels.md`).
Stage 2 C14 done: every work table takes bills, done by colonists with the matching work type (smithy, machining table, hand and electric tailor bench, crafting spot, stonecutter's table, electric stove); what a bill makes can be made of its stuff (clothes and weapons take cloth, leather, steel or wood, one kind at a time) and gets a quality from the crafter's skill by RimWorld's table (no legendary without an inspiration); bills have a skill range, an ingredient radius, an ingredient filter, "store in this stockpile", "pause when satisfied, restart at" and can be moved up and down (`SetBillOptions`, `MoveBill`). (Butchering was left to N2 and is done.) Melee weapons carry stuff and quality but their damage does not use them yet (`docs/design/crafting.md`). Save sections `bills` 2 and `crafting` 1.
Stage 2 U16 done: pawns wear what the sim says they wear. Kellan's own hoodie and trousers are hidden, a skin layer in the pawn's skin colour shows what nothing covers, and each worn piece's A5 meshes go on over it with the pawn's body shape, tinted by its stuff, torn (holes and dirt) as its hit points drop, and gone when it is taken off, stripped or worn out; tuques and helmets hide the hair (`docs/design/clothes-draw.md`, smoke test `clothes`; `ASimPawnActor::ApplyApparel`; no sim changes). **Run the `ClothesArt` commandlet again on each machine** (`docs/design/looks.md`, about 1 minute) for the skin meshes and the torn-look material; without it Kellan's own clothes stay on.
Stage 2 firefoam extras done: a popper leaves firefoam on the ground (nothing catches there until it fades, about 12 hours), bursts when it is destroyed, a powered foam turret (new `FoamTurret`, `Firefoam` research) shoots fires in 20 m, and a burning colonist runs for water within 12 cells and is put out (`docs/design/fire.md`, tests `FireTests`).
Stage 2 E4 follow-up done: burning colonists run about in panic (a colonist within arm's reach beats the flames out) and the firefoam popper (new `Firefoam` research) puts out all fires within 9 m when one comes within 3 m, then is used up (`docs/design/fire.md`).
Stage 2 U13 done: the sim's clock and weather are on screen. The sun and moon cross a sky atmosphere with clouds and fog (sun height by season, moon phase over 30 days), rain and snow fall in a block of air round the camera and stop under roofs, the ground gets wet in the rain and white with the sim's snow, thunderstorms flash, and the overseer's top bar and direct mode's time box show "Spring 6, Year 1   12°C, rain" (`docs/design/sky.md`, smoke test `sky`; `USkySubsystem`, `USnowSubsystem`; no sim changes). **Run the `TerrainMaterials` commandlet again on each machine to give the ground its `Wetness` input** (`assets/manifest.json`); without it everything else works and the ground just does not get wet.
Stage 2 skills check done: XP per level, decay, passion factors and the quality table already matched the wiki. Fixed: research speed is 8% + 11.5% a level (was 20% + 10%), tend quality by Medical skill follows the wiki table (1.0 at level 8, 1.55 at 20) with a plus or minus 25% roll, surgery success by skill uses the wiki table (10% at 0 to 110% at 20; nothing reads it yet), and the daily 4,000 XP cap counts net experience (decay is taken off first). Fast/slow learner and Great Memory are in the traits PR (#162).
Stage 2 social extras done: the Social skill makes good chats count more and romance likelier, lovers may stray (the betrayed one is upset, thinks worse of both and may fight the new lover), and a wedding gives the couple and every free colonist a party thought (`docs/design/social.md`).
Stage 2 C20 done: colonists talk (chitchat, deep talk, kind words, slights, insults, romance, proposals, breakups), hold opinions of each other, become friends, rivals, lovers and spouses, and an insult can start a fistfight (`docs/design/social.md`).
Stage 2 P1 done: a pawn carries a rifle, a sidearm and a melee weapon (`docs/design/weapons.md`).
Stage 2 E4 done: fire spreads over flammable ground, burns things to ash, heats rooms, sets pawns alight and is put out by rain, snow and colonists with the Firefighter work type in the home area (`docs/design/fire.md`).
Stage 2 animal products checked against the wiki: milk only from cow (14/day), yak (11/day), goat (12 per 3 days), caribou (10 per 2) and dromedary (18 per 2); wool alpaca and sheep 45 per 10 days, muffalo and muskox 120 per 15 (wool stuff stats fixed); chickens lay daily; ostrich and turkey lay only fertilized eggs (timer waits at half until a male has mated her); new animals goat, sheep, yak, duck, goose, turkey with meat, eggs and wool defs (`docs/design/breeding.md`, tests `BreedingTests`). Sim only. Odyssey animals (emu, cassowary, tortoise, elk, bison) not added.
Stage 2 N4 done: tame animals mate and bear young, hens lay eggs, calves and chicks grow up, colonists milk and shear (`docs/design/breeding.md`).
Stage 2 D10 done: narrators (Orrin, Maren, Jinx), difficulty levels, 30 incidents, ten factions with goodwill, visitor, trader and guard pawn kinds, trader kinds and price rules (`docs/design/narrator-data.md`).
Stage 2 F11 done: turrets and traps (`docs/design/defense.md`).
Stage 2 C13 done: growing zones with a chosen crop, colonists sow and harvest them (Growing work), crops grow by light and temperature, frost and heat kill them (`docs/design/farming.md`).
Stage 2 C22 done: wardens carry downed enemies to prison beds, feed prisoners, talk their resistance down and recruit or release them; `PrisonerSystem::breakOut` is the hook for T3's prison break (`docs/design/prisoners.md`).
Stage 2 C24 done: carrying is by mass (capacity over item mass), a hauler takes neighbouring stacks of the same def in one trip, a load slows walking up to 25% (`docs/design/jobs.md`).
Stage 2 U22 done: melee and weapon switching on screen. In direct mode the left mouse button swings a melee weapon (or fists), the right holds it up to block, Q brings up the next carried weapon; every carried weapon is drawn (rifle on the back, sidearm and melee weapon on the hips); swings are procedural poses timed to the sim's hit step, with hit-stop, flinch, blood and sparks, a dipping sidestep and a raised blade for blocks (`docs/design/melee-draw.md`, smoke test `melee`). No human melee animations exist yet, so the arms do not move.

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

Since D7 the data for medicine and disease is in `data/defs` (`docs/design/medicine-data.md`, test "medicine and disease data"): herbal, ordinary and lumen medicine with their potencies, diseases and sicknesses (infection, flu, plague, malaria, sleeping sickness, gut worms, muscle parasites, food poisoning, hypothermia, heatstroke, frostbite, toxic buildup), scars, nine artificial parts with install surgeries, the hospital bed and a crafting spot, and `MedicineRules`. Nothing in the sim reads most of it yet (H1 and E2 do).

Since U3 the game draws the ground and plants (`docs/design/terrain.md`, smoke test `world`): `UTerrainSubsystem` starts
by itself with any sim and builds one mesh per 32 m chunk (a 1 km map is 32 x 32 chunks, about 220 ms
to build). The ground has a vertex per cell corner at the sim's height, coloured by terrain type and soil fertility; **rock stands on it as a
solid 4 m block** (`TerrainDraw::RockHeightM`, picking and the overseer camera know), and a mined cell becomes a rough stone floor, with the mountain's thick
roof still over it. The thin roof of C5's rooms is a second section of the chunk; **roofs are hidden in the overseer view and shown in direct mode**
(`Terrain.Roofs` overrides). Mining rebuilds only that chunk (about 0.1 ms). Plants are instanced low-poly meshes made at start from
each plant def's new `visual` field (stem and crown shapes with colours; A1 can set `visual.mesh` instead), one component per chunk and kind, with cull
distances for grass; the sim's plant events and a rotating scan of chunk plant sizes keep them current, no snapshot copy. Since A2 the ground
has CC0 PBR textures (grass, sand, gravel, rock blended by terrain type and slope; the generated material is not in Git, without it the old flat colours are drawn). The old Stage 0 terrain test is still the `terrain` smoke test; the other smoke tests now
get their ground from this subsystem. No sim changes except `USimSubsystem::GetSimGeneration`.

Since F1 every pawn with a body (`race.body`) has per-part health: the Human body's 64 parts, injuries
with pain and bleeding, blood loss, downed (pain shock, unconscious, can't move) and death, slow natural
healing and tending (`sim.health().applyDamage`, the dev commands `DamagePawn` and `TendPawn`, events
`PawnInjured`/`PartLost`/`Downed`/`GotUp`/`Died`, `downed` and `dead` in the snapshot, `sim.healthReport`;
`docs/design/health.md`). Injured pawns walk slower and downed ones stop. Capacities now work per limb
(one lost leg is 50% moving), see `starting-data.md`. The protagonist from the Crashlanded start has 2x
part hit points. Infections and diseases came with H2 (below).

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
pawns must skip pawns with `Fighter`** as the job system does.

Since C20 colonists have a social life (`docs/design/social.md`, tests `SocialTests`, the colony scenario prints `talks`): every
pawn with needs gets a `Social` component (opinion memories, stored bonds). A pawn thinks of another by a hash-based compatibility
(-10 to 10), the other's traits (`opinionOfMe`: Abrasive -20), bonds (lover +40, spouse +60, ex -25) and fading memories; friends
(both 20 or more) and rivals (-20 or less) are worked out, never stored. About every 6 game hours a pawn with someone in the same room within
6 m picks an interaction by D9's weights and the opinion limits (chitchat, deep talk, kind words, slight, insult, romance attempt,
proposal, breakup); at a table or game beside others every 15 minutes. Each changes the listener's opinion, starts mood thoughts and
gives the talker Social experience. Romance and marriage are bonds only (one at a time, adults, no orientations yet); a death gives
the spouse, lover, friends and rivals their thought. An insult can start a fistfight (`SocialFight`: both walk up and swing fists even
with a gun in hand, ends at pain 0.25, a downed pawn, 5 minutes or a draft). Events `Interaction`, `BondChanged`, `SocialFight`,
dev command `InteractCommand`, snapshot `partner` and `socialFight`, save section `social`. **A new system that acts on pawns must skip
pawns with `SocialFight`, as for `Fighter`.**

Since F5 fighters aim like players (`docs/design/aiming.md`): Shooting skill sets a reaction time
before shooting a new target (0.5 s at skill 0, 0.17 s at 20), how well a shot leads a moving target
(a skill 20 shooter hits a man walking past about as often as one standing, a novice almost never)
and reload speed. Guns have magazines (revolver 6, assault rifle 30...; ours) that reload by themselves
when empty, `ReloadCommand` reloads early, and fighters top up in cover when they have no shot.
Fighters sidestep slow projectiles they see coming in time (bullets are far too fast; for grenades
and arrows later). Events `PawnAction` `Reload` and `Dodge`; snapshot `rounds`, `magazineSize`,
`reloading`; save format 17.

Since F6 there are raids and a colony defense (`docs/design/raids.md`): `StartRaidCommand` (dev until
the narrator) buys pirates with RimWorld's raid points (from colony wealth and colonists, the wiki's
curves; about 50 points for the Crashlanded start) among three gunner kinds (`data/defs/PawnKinds`,
`Factions`), each a whole person with a gun and some flak, and walks them in at a map edge toward the
colony. The squad advances, holds or falls back by comparing its strength (health, gun, armor) with
the enemies it knows, and routs off the map after losing half its pawns or after 10 to 15 game hours.
The colony alarm sounds by itself when a colonist spots an enemy (or `SoundAlarmCommand`) and stops
when no enemy can fight any more; colonists then follow their defense role (`SetDefenseRoleCommand`):
Defender (holds its defense spot, or a line 15 m from home toward the threat and fights freely once
in contact), Shelter (goes to the room marked with `MarkShelterCommand`) or Keep working (until an
enemy is within 20 m). Drafting still wins. Events `RaidArrived`, `SquadChanged`, `AlarmChanged`,
`PawnLeftMap`; snapshot `alarm`, `squads` and per pawn `defenseRole`, `defending`, `sheltering`,
`fleeing`, `squad`; save section `raids` and `fighters` 2. `simrun --raid POINTS --armed` plays a raid and reports it (CTest
`simrun_raid`). Findings for F7: on open ground, crouched pawns are hit about 1 shot in 100 at 20 to
30 m, so fights take long and downed colonists bleed out (no rescue or doctors yet).

Since D3 the game has real starting data in `data/defs/`, written by hand with RimWorld's numbers
from the RimWorld wiki (there is no RimWorld install to import from; `docs/decisions/0003`):
terrain fertility, wild plants and the temperate forest, basic items, the Human, skills, work
types, traits, our own backstories and names, the Crashlanded start, the Human body, capacities,
injuries and blood loss (`docs/design/starting-data.md`). `check_defs.py` now also checks that
defs name defs that exist. `simrun --pawns/--items` runs on the real data.
Not written yet: sandbags and thoughts; the tasks that use them
(C9) add them the same way. F6 wrote the raiders (the Pirate faction, three gunner pawn kinds, `RaidRules`). C7 wrote the recipes (simple meal, four simple meals, fine meal) and `BillRules`. Guns, bullets, flak gear and hitboxes came with F2. C4 wrote the buildings (wall, door, bed, stool, three tables,
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
`Cook`; save format 18 (formats 6 to 17 still load). **Not built: skill
learning, bill reordering.** (Stove fuel came with C11, below.)

Since 0004 the game has its own setting (`docs/design/lore.md`: the year 6650, slow star travel,
the fallen Lattice, magic called resonance) and our own names for RimWorld's proper names
(`docs/design/naming.md`: Impids are Emberkin, mechanoids the Remnant, insectoids chitinids,
storytellers are narrators named Orrin, Maren and Jinx). **Use those names in new data, code and
docs**; plain words like steel and rifle stay. Item words like plasteel are only proposed so far.

Since E5 buildings can have power (`docs/design/power.md`, tests `PowerTests`, `simrun --power`): conduits and closed switches join the
buildings beside them into nets; generators (wood-fired, solar by the sun and the open share of its roof, wind turbine by wind and clear air),
consumers (lamps, heaters, benches; any def with `power.consumption`) and batteries (charge kept at 50%, `idleDrain`) balance every step, and a
net that cannot cover its consumers goes dark as a whole, coming back only after its batteries hold a minute of the shortfall. `sim.power()` answers
`running(b)` (**a building that draws power must be checked with it before it works**; bill jobs already are, via `JobSystem::buildingRuns`),
`powered`, `netOf`, `nets`, `wattsOf`, `storedIn`, and does `flick` (`FlickCommand`: switches, consumers, generators), `setDraw` (E2's heaters ask for
less when warm) and `shortCircuit` (`ShortCircuitCommand`; empties the net's batteries, `ShortCircuit` event with the energy for T3, F9 and E4).
Events `PowerChanged`; `ThingView` `power`, `powerNet`, `powerWatts`, `batteryFill` and `Snapshot::powerNets` for U19. Save section `power` (version 1).
Solar panels follow E3's `lights().skyGlow()` and lamps that draw power glow only while `running` (the lamp layer is made again when power changes). **Two things are stubbed until their tasks land, each a one-line hook on `sim.power()`:** wind is
a constant until E1 calls `setWind`, and **wood-fired generators run without fuel until C11 calls `setFuel`** (whoever merges second wires it).
Since E3 every cell has a light level (`docs/design/light.md`, tests `LightTests`, `simrun --light FILE --hour H`): the sun by the hour of the day on cells with no roof, plus lamps (`glower`, with new `TorchLamp` and `Campfire` defs) falling off to their radius and stopped by walls; `sim.lights().lightAt(cell)`. Dark cells slow work (mining, plants, building, cooking) to 70% and walking to 85% at full dark. Rebuilt on change, not saved (no save format change). **The sim starts at midnight, so a test with the real defs that counts work steps needs a lamp beside the worker**; the tests' own small defs have no `LightRules` and no night.

Since E1 the sim has a calendar, an outdoor temperature, weather and snow (`docs/design/weather.md`, tests `WeatherTests`, `simrun --days 60 --weather` prints a line a day): the 60-day year in four seasons (`SeasonChanged`), `sim.weather().outdoorTemperature()` from the biome's season means, a day cycle (coldest about 04:20, warmest about 16:20) and a slow random drift, and weather chosen by the biome's commonalities among the kinds that can happen at that temperature, lasting its `durationHours`, blending in over 1.6 hours (`WeatherChanged`, snapshot `weather`). Under the open sky rain, fog, storms and snow slow walking, cut the hit chance, shorten how far a fighter sees and dim the sun (`Lights::skyGlow`). `weather().wind()` drives wind turbines. Snow falls and melts hourly per cell (`weather().snowDepth(cell)`) and slows walking by depth. `SetWeatherCommand{weather, hours}` is the dev command. E1 checked D4's weather numbers against the wiki and fixed them (`weather.md` lists what). New save section `weather` (the sim and snapshot gained a Weather system; no save format change). **The weather is random from the first hours, so a test with the real defs that needs a clear sky or an exact walking speed sends `SetWeatherCommand{"Clear", 1000}` first** (`LightTests` and `PowerTests` do); the tests' own small defs have no climate and always clear weather.

## Stage 1 progress

40 of 43 tasks done (D1, D2, D3, U1, S1, S2, S5, S6, F3, S3, U2, S4, U6, U5, C8, U12, F1, C1, U9, C6, C2, C4, C9, F2, C5, C3, F4, A3, F5, U3, U4, U7, C7, U8, U11, U10, C10, A1, A2, F6); G1 and G2 are left (F7 done). Tick tasks in `docs/plans/stage-1.md` as they merge.

## Stage 2

Planned 2026-10-25 in `docs/plans/stage-2.md`: "a colony that lasts a year", 74 tasks in seven
waves (seasons, weather, temperature, light, fire, power, farming, crafting, apparel, research,
medicine, animals, melee and grenades, two-storey buildings, the narrator and events, prisoners,
traders, protagonist talents and loadout). Its extra rules keep parallel threads from colliding.
**Wave 1 is open now in the cloud, alongside the rest of Stage 1:** S7 (saves and commands that
merge in parallel; every sim task that saves state waits for it), E3 light, and the data tasks D4,
D5, D6, D7, D8, D9, D11. D4 and D6 done (crops, three climates, weather kinds; apparel, textiles and tailor benches: `docs/design/starting-data.md`). S7 done: a save is
now named sections, each with its own version, and commands are saved by name, so **sim tasks that
save state can start; none of them bumps the save format number again** (it stays 20; add your own
section, `docs/design/saves.md` "Adding to the save"; a new system's checklist is in
`docs/design/bridge.md`). Format 6 to 19 saves still load. Count in the plan. Tick tasks in `docs/plans/stage-2.md` as they merge.

Stage 2 done so far: **D7** medicine and disease data (`docs/design/medicine-data.md`), **D11** building data 2 (power, climate, light, floors, furniture;
`docs/design/starting-data.md`).
**D5** animal data (15 animals, bodies, meat, leather, milk, eggs, wool: `docs/design/animals.md`).
**E5** power (`docs/design/power.md`).
**E3** light and darkness (`docs/design/light.md`).
**E1** seasons, weather, outdoor temperature and snow (`docs/design/weather.md`).
**D8** production data (work tables, stone blocks, melee weapons with tools; the meal recipes still list only the fueled stove, C14 adds the electric stove).
**C18** recreation, comfort and outdoors (`docs/design/recreation.md`, tests `RecreationTests`, colony scenario): colonists have three new needs (`Joy` component; save section `recreation`). The standard schedule now has **Joy 19:00 to 21:59**; in Joy hours (or when bored with no work, Anything hours) they play horseshoes, chess or billiards, chat at a table, stroll or watch the clouds (`JobType::Recreate`), kinds wear in with tolerance. Comfort follows the bed or chair in use, Outdoors the roof. The D9 thoughts apply by stage (`MoodSystem::conditionValue` is where a task adds its own staged condition). Snapshot has `joy`, `comfort`, `outdoors`. Not yet: talk while chatting (C20), weather for the clouds (E1), the Unreal side (animations, a need bar; a U task).

**C21** research (`docs/design/research.md`, tests `ResearchTests`): one current project, `SetResearchCommand` picks it; colonists with the Research work type sit at a research bench and add points per second by Intellectual skill and bench speed (`JobType::Research`); finished projects open the defs with a `researchPrerequisite` (plans and bills fail until then); `Snapshot::research`; save section `research`. A new colony has nothing researched (a scenario may list `startingResearch`). Not yet: multi-analyzer bonus and facility check, skill XP, gating floors/plants, the Unreal research tab (U18).

**H1** doctors, medicine and rescue (`docs/design/medicine.md`, tests `MedicineTests`): the Doctor work type tends colonists' wounds (`JobType::TendPatient`, the bleeding first) with the strongest medicine their care setting allows (`SetMedicalCareCommand`: NoCare, NoMedicine, Herbal, Normal, Best), quality from Medicine skill, medicine and bed offset (`MedicineSystem::tendQuality`); downed colonists are carried to a medical bed, else their own or a free bed (`JobType::Rescue`, the patient's position follows the rescuer's); hurt colonists rest in bed until healed (`JobType::BedRest`, work types Patient and PatientBedRest); hospital beds are not slept in; healed injuries may leave scars or old wounds (`Injury::peak`, `PawnScarred`); emergency work types are asked first at each priority and a pawn on a droppable job looks for them every 10 s; events `PawnTended`, `PawnScarred`, actions `Tend`, `LiftPawn`, `LayPawn`; save section `medicine`, health section 3. Also fixed: saves made while someone was cleaning filth could not be loaded.

**H2** infections and diseases (`docs/design/diseases.md`, tests `DiseaseTests`): `Health::diseases` and `immunities`; every `diseaseUpdateSeconds` (60) immunity rises and severity moves (not immune up, immune down, tended down by quality) until the pawn dies at the lethal severity (`DeathCause::Disease`) or the disease ends (immunity stays, except infection); wounds roll `infectionChance` when dealt (`Injury::infectAt`, 6 to 18 h, tending first may stop it; cleanliness scales the chance); `tendAll` tends diseases for `tend.durationDays` and `needsTending`/`needsRest`/`urgentlyNeedsTending` know them; gut worms and muscle parasites cure at a total tend quality of 3; stages add capacity offsets, caps and pain; `foodRateFactor`/`restFallFactor` feed `NeedsSystem`; poisoned meals give food poisoning (the spoilage handler); mood situation `illness`; `ContractDiseaseCommand`; **T3 hook:** `HealthSystem::outbreak(disease, candidates, victims, rng)` and `onsetMtbDays`; events `PawnSick`, `PawnRecovered`; health section 4; D7's disease numbers replaced by the wiki's. Not yet: vomiting, animals' diseases, rotten-food poisoning, the health tab's disease list.
**C22** prisoners (`docs/design/prisoners.md`, tests `PrisonerTests`, save section `prisoners`): a bed marked with `SetPrisonBedCommand` becomes a prison bed (`PrisonBed`; only prisoners sleep in it, `findBed`, `findPatientBed` and `assignBed` keep the two apart); the Warden work type has three scanners, `CapturePrisoners` (a downed enemy of another side goes to a free prison bed by the Rescue job's steps, `JobType::CapturePrisoner`, unless an able enemy is within 25 m), `FeedPrisoners` (`JobType::FeedPrisoner`, food rises, `PawnAte` "fed") and `ChatWithPrisoners` (`JobType::WardenInteract`, every 6 game hours); a `Prisoner` component (mode, resistance 5 to 9 at the start, times) keeps its `Faction` but it has no `Fighter`, no `Raider`, no Work tab row and a prisoner thinks only in `JobSystem::stepPrisoner` (sleeps in its bed, never fetches food); fighters, turrets, the alarm and raid squads ignore prisoners (**a new system that picks enemies must skip pawns with `Prisoner`**); `SetPrisonerModeCommand` (NoInteraction, ReduceResistance, AttemptRecruit, Release); a talk lowers resistance by 1.0 x mood factor x the warden's negotiation (Social, Talking); recruiting joins the colony, release walks off the map; doctors tend prisoners; events `PrisonerCaptured`, `PrisonerChatted`, `PrisonerRecruited`, `PrisonerReleased`, `PrisonBreak`, actions `Chat`, `FeedPawn`; thoughts `Captive`, `Captured`, `WardenTalk`; snapshot `prisoner`, `prisonerMode`, `resistance`, `prisonBed`. **T3 calls `PrisonerSystem::breakOut` for the prison break incident.**

**P2** recoil kicks and a dodge roll (`docs/design/shooter-feel.md` "Built", tests `ShooterFeelTests`, save section `feel`): each gun has a `recoilKicks` list (degrees up and to the right per shot of a spray); the player's shots (`ShootCommand` with `player`) fly where the shots before have lifted the aim, the kick holds 5 steps then wears off, and the spray restarts after 15 steps; `Snapshot` has `kickUp`/`kickSide` for the camera. `DodgeRollCommand{pawn, dx, dy}` rolls the protagonist 4 m: 3 steps start-up, 15 rolling (bullets pass through for the first 12), 9 recovery, 6 waiting; the sim moves the pawn (**`DirectMove` is ignored while `rollLocked`**), shooting is off, a roll cancels aiming and firing but not a reload; snapshot `rollPhase`/`rollStepsLeft`/`rollDir`/`hitProof`, `PawnAction::Roll`. Numbers are ours (`CombatRules`, `Weapons.json`). **Not verified on screen: U21 binds the key, plays the animation over the sim's motion, applies the camera kick from the snapshot and must follow the sim's position during a roll (it must not keep sending its own).**
**P3** talent tree (`docs/design/talents.md`, tests `TalentTests`): the protagonist earns talent XP from finished jobs (100, 125, 150... XP per point), spends points with `SpendTalentPointCommand` on seven talents (build speed, aim spread, carry capacity, trade price; second tier needs rank 3 of the first); stats applied through `TalentSystem::statFactor` in `JobSystem` and `CombatSystem`; `Snapshot::talents`, event `TalentPoint`, save section `talents`. Not yet: combat XP, the trade stat (no trade system), the Unreal talent screen.
**P1** several weapons (`docs/design/weapons.md`, tests `WeaponsTests`, save section `weapons`): `Gear::stowed` holds the weapons carried on the body besides `Gear::weapon` (the one in the hands, which everything else still reads); one weapon to a slot (`WeaponSlot` Primary, Sidearm, Melee; guns say `weaponSlot` in their def, the revolver and autopistol are sidearms). `CarryWeaponCommand{pawn, item}` adds a weapon, `SwitchWeaponCommand{pawn, slot}` brings one up in `CombatRules.weaponSwitchSteps` (rifle 36, sidearm 18, melee 27 steps) during which the pawn can't shoot, reload or swing; each gun keeps its own magazine. The fighter AI (`chooseWeapon`) takes a melee weapon within 2.5 m of its target, a sidearm within 9 m, else the rifle, with a 2 m margin to stop flip-flopping; pawns with one weapon behave as before. Weapons on the body count in C24's load (`bodyWeaponMass`: slower walking, less room for hauling). Snapshot `carried[3]`, `switchSlot`, `switchStepsLeft`; `PawnAction::Switch` and `WeaponSwitched`. **U22 binds the switch key and draws holstered weapons; nothing is on screen yet.**

**D12** research data (22 projects, tech levels, two research benches, content gated by `researchPrerequisite`: `docs/design/research-data.md`).
**E2** room temperature is in (`docs/design/temperature.md`, tests `TemperatureTests`, `simrun --outdoor C`, CTest `simrun_temperature`): every roofed enclosed room has a temperature that follows the outdoors through its roof, walls, doors (open ones much faster), vents and rock, mixes when rooms join and split, and is pushed by heaters, coolers and campfires (`heatPusher` in the defs; **E5 power decides which run**: `Sim` wires `PowerGrid::running` into `TemperatureSystem::setSourceRunning`, a campfire needs none, and C11 fuel will add its test the same way). Pawns get Cold and Hot thoughts outside their comfortable range, and hypothermia or heatstroke (new *environment hediffs* in `Health`, with stages on the capacities and death at 1.0) outside the safe range, and a rare frostbite injury below -10. **Outdoor temperature is a stand-in until E1** (`Sim::outdoorTemperature()`: the biome's season and a daily swing; E1 replaces its body and everything else reads `temperature().outdoor()`); **what a pawn wears widens its comfort range** (C15 replaced the flat 12 degree stand-in: `Sim` wires `ApparelSystem::insulation` into `setComfortModifier`; a naked human has the plain 16 to 26). Colonists seek shelter since the colonist needs extras (see the top of this file). Save: section `temperature` 1 and `health` 2, no format change.

**F8** melee is in (`docs/design/melee.md`, tests `MeleeTests`): fists and the D8 melee weapons swing with a
wind-up, a hit moment and a recovery (the whole swing is the tool's cooldown, so RimWorld's damage per second
holds); at the hit a level strike is traced against hitboxes as far as the weapon reaches (fists 0.85 m, spear
1.9 m), aimed with a spread that hits a standing man as often as RimWorld's melee hit chance; damage goes through
armor like a bullet's (`CombatSystem::hitPawn`). Defenders read a swing after a reaction time from Melee skill,
then sidestep (fighters, as often as RimWorld's dodge chance) or block with a melee weapon from the front (ours).
Fighters with a melee weapon or the Brawler trait and no gun charge; others strike back at enemies in reach, and
unarmed fighters no longer walk at their target; a fighter falling back stands and fights an enemy at arm's
length. `MeleeCommand` (Attack, Block) for the player and dev use, events `MeleeSwing`, `MeleeHit`, `PawnAction`
`Block`, snapshot `meleePhase`, `meleeStepsLeft`, `meleeTarget`, `blocking`, save section `melee`. **C20's
social fights and N5's animal fights start swings with `MeleeSystem::attack`.**

**F9** explosives and grenades are in (`docs/design/explosives.md`, tests `ExplosiveTests`): `CombatSystem::explode`
is the one blast (radius, falloff to 40% at the edge, three sample points per pawn traced from just above the
ground so cover and walls shield, a prone pawn takes less, 2 to 4 fragments each through `armorPass`, a knock-down
for a big hit, and buildings take x2/x4/x8 by material with walls between shielding). A frag grenade (50 damage,
radius 1.9 m) is a Things item on a pawn's belt (`Belt`, 10 per pawn, `ExplosiveSystem::take`/`give`); `ThrowCommand`
winds up (none for the player), throws it in a flat or steep arc planned against walls and cover, and it explodes
50 steps after landing. A grenade is a `Grenade` entity (no `Thing`) in the snapshot's `projectiles`. Fighters
**dive** from a grenade they see (run out of the radius, or lie prone if there is no room; F5's `dodgeUntil`) and
**throw** at two or more enemies together or at one they cannot shoot, never with friends in the radius.
`ExplodeCommand` is the dev/player blast. Events `GrenadeThrown`, `GrenadeLanded`, `Explosion`, `PawnKnockedDown`,
`PawnAction` `Throw` and `Dive`; save section `explosives` 1. **Molotovs wait for E4 (fire); F11's IED trap, F12's
sappers and U21's effects call `CombatSystem::explode`.**

**F11** turrets and traps are in (`docs/design/defense.md`, tests `DefenseTests`): `Turret_Mini` is a powered building (100 W, research
`GunTurrets`) that picks the nearest seen, able, non-animal enemy in its gun's range, winds up, fires bursts of real bullets
(`CombatSystem::fireFrom`, a shot from a gun no pawn holds) and blows up when combat destroys it (any building with `explosion` and
`explodeWhenDestroyed` does; deconstructing never does). `TrapSpike` (5 stabs, 100 total) and `TrapIEDExplosive` (a 3.9 m blast through
`CombatSystem::explode`) spring under any pawn that does not know them: the colony's side always does, an enemy side learns a trap when one
of its pawns springs it or one of its fighters notices it within 4 m, and then its fighters keep their spots off it and bend their walks round it
(`DefenseSystem::avoidsCell` and `detour`). `RearmTrapCommand` sets a sprung spike trap again; **no colonist job rearms traps yet and raiders do not
shoot at turrets (both in `docs/design/defense.md` "Not done yet")**. New events `TrapSprung`, `TrapRearmed`; snapshot `turretPhase`, `turretTarget`,
`trapState`; save section `defense` 1. Research now has 23 projects.

D9 (thoughts, recreation, mental break and social data) is in: `docs/design/thoughts-data.md`. Data only; C15, C17, C18, C19, C20, E2 and E3 read it.
**C15** apparel (`docs/design/apparel.md`): clothes worn in layers with armor and insulation per piece (stuff times quality), wear and tear (absorbed damage and 0.4 HP a day), tattered, naked, nudist and dead man's clothes thoughts, outfit policies (`SetOutfit`, `EditOutfit`, `AddOutfit`, `RemoveOutfit`), a change-clothes think node and a strip job for the downed and dead (`Strip`). Colonists start dressed. Insulation widens the comfortable temperature range (see E2). The starting T-shirt and pants hold a colonist to about 4 degrees outdoors; parkas (C14 tailoring) are what winter needs. Save section `apparel` 1.
**C14** crafting (`docs/design/crafting.md`): a building def's `workGiver` names the work giver that offers its bills (`DoBillsGiver` in `WorkGivers.cpp`; a new bench needs that and its recipes). `CraftingSystem` rolls a product's quality from the crafter's skill (`CraftingRules.qualityChances`, the wiki's table) and picks its stuff; bills have `skillMin`/`skillMax`, `radius`, `storeZone`, `unpauseAt` and `disallowed` (`SetBillOptions`) and an order (`MoveBill`). Save sections `bills` 2 and `crafting` 1. Butchering is N2 (done); the stuff and quality of weapons are stored but not used in damage yet.
**C17** room stats, beauty and filth (`docs/design/room-stats.md`): floors and filth layers, a cleaning job (indoors only for now), space, beauty, wealth, cleanliness and impressiveness by RimWorld's formulas, the beauty need and the room thoughts. Floors come only by `PlaceFloor` command: no floor-building job exists in any plan task yet (BACKLOG). Save section `roomstats` 1.
**C16** rot and storage (`docs/design/spoilage.md`): food rots by the temperature of its cell (stops below 0, full speed from 10 degrees), items with a deterioration rate wear out under open sky, meals remember their ingredients and may be poisoned by a dirty kitchen or a poor cook; `SpoilageSystem::setPoisonHandler` is the hook **H2 must wire to give the `FoodPoisoning` hediff**; E1 can call `setOutdoorFactor` for rain. Save section `spoilage` 1.
D9 (thoughts, recreation, mental break and social data) is in: `docs/design/thoughts-data.md`. Data only; C15, C17, C18, C20, E2 and E3 read it.

**C19** mental breaks are in (`docs/design/mental-breaks.md`, tests `MentalBreakTests`): a colonist under a break threshold gets a roll every 216 game seconds at RimWorld's mean waits (4, 1.5 and 0.5 days, shorter as mood sinks); the band picks the break (minor: sad wander, food binge, hide in room; major: tantrum, long wander, insulting spree; extreme: berserk, give up and leave; fire-starting needs E4). While it lasts **the job system skips the colonist** (like a fighter) and `MentalBreakSystem` walks it; it ends when its time is up and the mood is 10 above the threshold, then Catharsis (+40) lifts it. New: events `PawnMentalBreak`, `PawnMentalBreakEnded`, `PawnInsulted`, `PawnLeftColony`, `PawnAction::Punch`, snapshot `mentalBreak`, dev commands `StartMentalBreak` / `EndMentalBreak`, defs `Rules/MentalBreakBehaviour.json` (our numbers), save section `mentalBreaks`. Orders to a colonist in a break are refused. C20 can turn `PawnInsulted` into a real interaction. **Now hooked up:** berserk fights pawns with F8 melee (`MeleeSystem::attack`, real swings, dodges, blocks) and fire-starting lights fires with E4 (`Fire::ignite`); `Sim` wires both with `setMelee` / `setFire`. The colony scenario prints a `mentalBreaks` count (seed 42 has one minor binge on day 2).

**N1** animals on the map (`docs/design/animal-life.md`, tests `AnimalTests`, `simrun --animals`): `SimSettings::wildAnimals`
(percent of the biome's density; 0 is the default, so nothing else changes) puts wild herds on the map when it is made; animals are pawns
with `Animal` (no `Worker`, side 100), wander in herds behind the lowest-id leader, graze plants, sleep by day or night, walk in from and off
the edges to keep the biome's density, and rot away 15 days after dying. About 300 animals on a 1 km map cost 0.36 ms a step, 80% of it in
`MovementSystem` (for S9). Save section `animals`. Fighters never take an animal for an enemy on their own. Unreal's own start turns `wildAnimals` on at 100% (U15; `-SimWildAnimals=<percent>`, 0 for none, off on flat maps); **code that counts pawns of another faction as raiders
must skip `Animal`**.

**X1** genes and lineages (`docs/design/genes.md`, tests `GenesTests`): 55 genes as defs (`data/defs/Genes`) with stat offsets and factors, skill aptitudes and looks; Emberkin, Frostkin, Warbred, Ashborn and Delver as gene sets (wiki totals for metabolism and complexity; most single-gene numbers are recalled and marked `unverified`). `PawnGenes` on every humanlike pawn is made from its lineage (not saved; rebuilt on creation, `SetLineage` and load). Read today: walking speed, comfortable temperature range, hunger rate, mood, damage taken, wound healing, mining and plant speed, skill levels; every other stat is summed and available through `geneStat`. Looks take skin, hair and horns from genes (the lineage's own `visual` block is gone). Scenario `lineageWeights` rolls colonists' lineages (never the protagonist's); `CrashlandedMixed` does, `Crashlanded` is unchanged. **A new rule that a gene should change reads `geneStat(*sim.genesOf(pawn), "stat", base)`**; Unreal's horn lookup changed in `PawnSubsystem.cpp` (uncompiled until the PC builds).


**N3** taming, training and pens (`docs/design/husbandry.md`, tests `HusbandryTests`): a `MarkAnimal` Tame mark makes a Handling colonist fetch hay (or whatever the animal eats), walk to it and roll
`(0.5 + 0.05 x Animals) x (1 - wildness)`; a failed try keeps the mark. Tame animals join side 1, eat hay, kibble, meat and plants (walking round walls by path tickets), are hand-fed under 25% food and starve after 5 days.
Training (Obedience, Release, Rescue, Haul: 10/5/20/30 steps, one try per 6 h, fades over 6 days) is by `SetTraining`; **Haul makes the animal a Hauling-only worker**. Animal zones and pens (`isPen`: no walkable way out except doors), `AssignAnimalZone`, `Release`, slaughter (the corpse is carried and butchered since N2).
Save section `husbandry` 1, five new commands (`MarkAnimal`, `SetTraining`, `CreateAnimalZone`, `RemoveAnimalZoneCells`, `AssignAnimalZone`). **Obedience, Release and Rescue only count steps; their effects wait for later tasks.** Animals walk through doors (only the pen test treats them as barriers). Not done: names for tamed animals, manhunters on a failed try (N5), a taming line in the colony scenario (C25 can add it).

**N2** hunting and butchering (`docs/design/hunting.md`, tests `HuntingTests`): `MarkAnimal` with mark `Hunt` (wild animals; snapshot `markHunt`), the `HuntDesignated` scanner (Hunting work type; the hunter needs a gun in its hands) and the `Hunt` job (walks at the animal and shoots from 80% of the gun's range with line of sight, up to 15 minutes), the `HaulCorpses` scanner (Hauling; job `HaulCorpse`: the rescue steps plus `DropCorpse` into a stockpile cell with nothing on it) and the `Butcher` job from a butcher table's `ButcherCorpse` bill (`Recipe::butcher`; 648 s of work; yield = the animal def's `meatAmount`/`leatherAmount` x body size against an adult's x (75% + 2.5% per Cooking level) x the Manipulation factor, at most 150%; "until you have" counts all raw meat). A corpse is still the dead animal pawn: `Husbandry::isCorpse`, `wantsHauling`, `butcherYield`. New events `AnimalHunted`, `AnimalButchered`; `PawnAction` `Butcher`, `DropCorpse`. Save section `husbandry` 2 (`markHunt`, `collect`, `hunted`); the hash adds them only when set. `JobSystem::setCombat` wires the hunters' shots. Not done: butcher spot and tool cabinets, human corpses, the -6 mood for butchering people, rotting corpses, skipping dangerous animals (BACKLOG).

**N4** breeding and products (`docs/design/breeding.md`, tests `BreedingTests`, ctest `simrun_farm`): tame, grown animals mate (a male rolls about every 12 hours, picks the nearest willing female of his kind and zone within 40 m, walks to her if she is further than 12 m, and 50% of matings leave a gestating female pregnant); litters are born tame into the mother's zone after the def's `gestationDays`; hens lay an egg every 1.5 days (unfertilized ones stack, a mated hen's next egg is fertilized and hatches into a tame chick after 3.8 days); cows and muffalo fill with milk, muffalo and alpacas with wool, and the Handling tasks **Milk** and **Shear** send a colonist (new job `GatherAnimal`). Everything runs from one pass a minute over the tame animals (`Breeding::step`), so it costs about 0.005 ms a step for 500 chickens (`simrun --farm 500`). Sex is fixed per animal from the seed and its id (nothing saved); save section `breeding` 1; events `AnimalMated`, `AnimalBorn`, `EggLaid`, `EggHatched`, `AnimalGathered`; snapshot `sex`, `pregnant`, `milkFullness`, `woolFullness`. Young animals aren't adults until their last life stage: no mating, products or hauling before. **Also fixed:** a save made while a colonist was taming or feeding an animal used to be refused on load (the job check stopped at `StripPawn`). **Worth knowing for S9:** 500 animals cost about 0.8 ms a step, mostly movement (N1 put 80% of it in `MovementSystem::movePawns`), not breeding. **N4 leftovers done:** young animals have a share of their adult hit points (`healthScaleFactor`), fertilized eggs only progress between 0 and 50 degrees, and `SetBreeding` switches breeding off per animal or per zone (save sections `breeding` and `husbandry` now version 2).

**N5** animal fights and predators (`docs/design/animal-fights.md`, tests `AnimalFightTests`, `simrun --animals` prints the counters): animals have hitboxes now (shots hit them) and reach with teeth and claws. `Animal::temper` (Calm, Hunting, Revenge, Manhunter); anything but Calm is an enemy to the fighter AI and the colony alarm. A hurt wild animal turns manhunter by chance (0.5 tusker, 0.2 predator, 0.02 other), else predators and tuskers take revenge for a while and the rest run; shots, blows and blasts frighten calm animals within 22 m. Hungry wild predators hunt prey up to 1.5 x their size and lone colonists, the prey freezes when caught, the kill is eaten (corpse gone). **`AnimalSystem::enrage` is the hook T3 (manhunter pack) and a failed taming use.** Save section `animals` 2. Not done: packs, pathing round walls for chasers, manhunters from failed taming.
**E4** fire and firefighting (`docs/design/fire.md`, tests `FireTests`, ctest `simrun_fire`): a sparse fire layer (`sim.fire()`), one pass a minute over only the cells that burn. A fire starts from an `Ignite` command, a Burn-damage explosion (molotovs for F9/F11 are now one projectile def away), a burning pawn or a room above 235 degrees, grows on the cell's flammability (building times stuff, items, plant, floor; grass 1.3, wood 1.0, stone and steel 0), burns those things down (a plant, building or item at 0 is destroyed and leaves ash), pushes heat into its room (`TemperatureSystem::addHeat`), and spreads up to two cells away (knight's move, embers need a big fire). Rain and 10% snow put it out outdoors, not under a roof. Pawns in a fire catch fire (a `Burning` component, 2 damage per RimWorld second, drops their job, fire-resistant skin never catches); beating out the fire under a burning pawn puts them out. The **home area** (`SetHomeArea` command, grows by itself round buildings and colonists until painted) is where colonists with the Firefighter work type go: job `ExtinguishFire`, nothing reserved so several fight one fire; a burning colonist is rescued anywhere. Burnt ground (fades in 6 days) and ash are left behind. Save section `fire` 1; events `FireStarted`, `FireOut`, `PawnCaughtFire`, `PawnFireOut`, `PawnAction::Extinguish`; snapshot `fires`, `burnt`, `homeArea`, `onFire`. `simrun --fire` lights a grass fire near the colonists (nothing burns more than two cells in the Crashlanded start, the colonists beat it out in minutes). **Also:** wood logs now have flammability 1.0 (they had none), and wood floor and carpet 1.0 (unverified). Numbers in `FireRules` are partly recalled and partly ours; tune `spreadPerTick`, `plantDamageFactor` and `beatPerTick` after a playtest. Not done (BACKLOG): firefoam, panic running and water for burning pawns, per-colonist allowed areas, fuel buildings catching fire, a home-area tool.

**U15** animals on screen (`docs/design/animal-life.md`, smoke test `animals`): `USimAnimalSubsystem` draws every pawn with a snapshot `lifeStage` of 0 to 2 as an `ASimAnimalActor` on its A4 skeletal mesh (model and scale from `assets/animal_art.json`, fitted to its `lengthM`), playing idle, walk, gallop, eating and death clips picked from the snapshot; `USimPawnSubsystem` skips animals. Animation by distance: every frame within 40 m, 8 Hz within 120 m, a frozen pose within 400 m, hidden beyond (`Animals.NearDistance`, `MidDistance`, `HideDistance`). Not done: far herds as instanced meshes with baked animation (G3 decides if the frozen skinned pose is cheap enough), a lying-down pose for sleep (it slows the idle clip), and the models' facing is unchecked by eye.

**M1** upper levels (`docs/design/levels.md`, tests `LevelsTests`): the map has four levels, 3 m apart; upper levels are stored in
16 m chunks made only where something is built, so empty sky costs nothing. A cell up there is `Terrain::Air` until an **upper
floor** is laid (`PlaceUpperFloorCommand`, instant for now; `UpperFloors::lay`): it needs a wall or column within the roof support
radius (6.9 m) on the level below, never over a mountain, and is the roof of the cell under it (`Map::roof` says `Thin`, Rooms puts
no roof of its own there). Buildings and items go only on a floor. Regions, rooms and room temperature work on every level; an upper
room beside open air is outdoors (`Room::openToAir`); heat passes through a floor like a thin roof. Save section `levels`; snapshot `z`
includes the level. **Systems that only know the ground use `Map::onGround(c)` instead of `inBounds`** (plants, zones, growing zones, filth,
fire, snow, lights, cover, threat maps, floors, designations); `Map::inBounds` now takes levels 0 to 3. No stairs yet (M2), no collapse (M3), nothing drawn (U17).

**M2** stairs and ladders (`docs/design/levels.md`, tests `StairsTests`): `Stairs` (1 x 3, 60% walking speed) and `Ladder` (1 x 1,
30%) climb one level towards their facing, from the cell in front of the foot to the cell beyond the head one level up
(`Map::levelLinks`, kept by `Things`). Regions link the two ends, so ground and upper floor share a reachability group; the
pathfinder's region route and cell search climb them at their cost, and a path's cells carry their level. A pawn climbs standing
at the foot while `Mover::climbed` counts up, then stands at the head; the snapshot has `climb` (0 to 1) and moves `x`, `y`, `z`
along it. **Upper floors are built by colonists** now (`UpperFloor` def: 4 stuff, 216 s, planned per cell, worked from below or
beside). Stockpiles and doors work upstairs, and hauling, delivering, building, taking down, food and beds look across levels
(a level counts as 10 m more, `levelDistSq`); other scanners stay on their own level. Save section `movement` 2. Numbers are ours
(RimWorld has no levels). Not done: direct mode climbing and the upper-floor and stairs tools on screen (U17), floors falling (M3).

**C11** fuel (`docs/design/fuel.md`, tests `FuelTests`): a building with a `fuel` field in its def holds fuel
(`Fuel` component, new section `fuel` in the save, format stays 20). The fueled stove holds 50 wood and burns one per meal it makes
and is **stopped when empty**; a campfire and a torch lamp burn all the time (`perDay` or `burnHoursPerWood`, the generator's will too, E5); a new
building starts empty and an old save's stove loads full. A `Refuel` scanner is first in Hauling: low buildings (at or below 30%, or ordered
filled) get the nearest wood stack carried to them (`Refuel` job, 345.6 s of work). `SetRefuelCommand{building, autoRefuel, fillNow}`;
events `OutOfFuel`, `Refueled`, `PawnAction::Refuel`; snapshot `fuel`, `fuelCapacity`, `autoRefuel`, `fillOrdered`. The colony scenario now
checks wood was put in (`woodIn`). A lamp with no fuel is dark and a wood-fired generator with none makes no power (it burns
only while switched on); **new fuel buildings start empty, so tests about something else fill them with `testFueled` (`Tests/FuelTestHelper.h`)**. **A new thing that burns fuel puts a `fuel` field in its def and reads `sim.jobs().fuel()` (`lit`, `canUse`, `use`).**

**Guessed-number check (partial):** checked against the wiki: melee weapon mass and work (knife, mace, spear, gladius, longsword), gun turrets research cost (800 to 500), multi-analyzer and fabrication tech level (Industrial). Still marked `unverified`: furniture and bench costs, floors, crop product mass and values, genes, recipes, and the melee weapon tool powers; the wiki pages fetched did not give those.

**C12** skill learning (`docs/design/skills.md`): every job that uses a skill teaches it (mining, plants, building, taking down, every bill's recipe), with passion factors, the 4,000 XP daily soft cap, the slow loss above level 10 at midnight, and a mood lift (+8 minor, +14 major) while working at a passion. **A new job that uses a skill calls `JobSystem::learnSkill` each working second and is listed in `JobSystem::activeSkill`.** Save: `colonists` section version 2.

**Biomes (all 12 vanilla)** are in `data/defs/Biomes/Biomes.json`: temperate forest, boreal forest, arid shrubland (the savanna-like one), tundra, ice sheet, sea ice, cold bog, temperate swamp, tropical rainforest, tropical swamp, desert, extreme desert. Each has season temperatures, daily swing, rainfall, growing seasons (none on ice), a weather table (no snow in the tropics, almost no rain in deserts), wild plant mix, wild animals with commonalities and a `terrain` mix for S8's map generation. New data: 14 animals (caribou, muskox, arctic fox, arctic wolf, polar bear, dromedary, gazelle, ostrich, rhinoceros, elephant, lion, monkey, capybara, iguana, with meat), 9 plants (pine, willow, palm, cecropia, teak, bamboo, saguaro, agave, moss) and ice terrain. Numbers are recalled wiki values (listed as `unverified`). **`Sim` is still fixed to the temperate forest; S8 adds the choice and reads each biome's `terrain`.** Test `BiomeTests`.

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
`docs/design/terrain.md`); ~~**U4** buildings and items~~ done (PC Unreal; smoke test `buildings`; `docs/design/building-draw.md`); ~~**U7** orders~~ done (PC Unreal; smoke test `orders`; `docs/design/orders.md`).

Wave 6, open now that C1 is in: ~~**C4** building~~ done (`docs/design/construction.md`; save format 10; ~~**C5** doors,
roofs and rooms~~ done (`docs/design/rooms.md`; save format 13), ~~**C7** bills and cooking~~ done (`docs/design/bills.md`; save format 18; ~~**U8** colony panels~~ done (PC Unreal; smoke test `panels`; `docs/design/panels.md`)), ~~**C6** food, rest and
schedules~~ done (`docs/design/needs.md`; save format 8), ~~**F2** guns and hitboxes~~ done; ~~**C3** mining, chopping
and harvesting~~ done (`docs/design/mining.md`; save format 14). On the PC, ~~**U9**~~ and ~~**A3** seeded looks~~ are done (smoke test `looks`; save format 16).
U6's work tab and a colonist panel can now read `sim.jobs()` (work types, priorities).
~~**F4** fighter AI: where to stand~~ done (`docs/design/fighters.md`; save format 15), ~~**F5** fighter
AI: aiming~~ done (`docs/design/aiming.md`; save format 17), ~~**F6** raid and defense~~ done
(`docs/design/raids.md`; save section `raids`, `fighters` 2), so ~~**F7** raid scenario tests and nightly soak~~ is done (`simrun --raid-soak N`, `.github/workflows/nightly.yml` at 03:17 UTC, `docs/design/raid-soak.md`; it found stuck fighters, 13.6% waiting in the open and some raids that drag on, now in BACKLOG); with U9 and F2 in, ~~**U11** fights on screen~~ is done (PC; smoke test `fights`; `docs/design/fights.md`; it
binds R to reload; reload and sidestep animations are not drawn yet). ~~**C10** colony scenario test~~ done
(`docs/design/colony-scenario.md`); ~~**U10** hands-on work~~ is done (PC; smoke test `handson`; `docs/design/handson.md`); ~~**A1** placeholder art~~ is done (PC; free Poly Haven meshes on the bed, door, table, stool, stove, logs, meals, berries, trees and bushes; `docs/design/building-draw.md`, `python tools/placeholder_art.py` then the `PlaceholderArt` commandlet). ~~**A2** terrain materials~~ is done (PC; free CC0 Poly Haven textures instead of Megascans, so nobody had to download anything: grass, sand, gravel and rock blended by terrain type and slope, placed by world position; `docs/design/terrain.md`, `python tools/terrain_textures.py` then the `TerrainMaterials` commandlet; swapping in Megascans later is a change in `assets/terrain_textures.json`). ~~**A6** placeholder art 2~~ is done (PC; 30 more free Poly Haven meshes on crops, work tables, power, climate, lamps and furniture, same list and commandlet as A1; walls, floors, solar panel, wind turbine and a few small things stay shapes). ~~**A5** clothes art~~ is done (PC; worn meshes for all 12 apparel defs by layer, made from the morphed hoodie and trousers plus a free CC0 Poly Haven hat, no downloads by sneed; `docs/design/looks.md`, `python tools/clothes_art.py` then the `ClothesArt` commandlet; not yet seen in game, U16 draws them). **G1** is what is left on the PC.

## Waiting on sneed

- Nothing now. (Megascans for A2 are no longer needed: the free textures are in; Megascans can replace them
  later through `assets/terrain_textures.json`.)

**Biome and animal numbers checked on the wiki** (after PR #160): all 12 biomes now have the wiki's plant and animal density, disease days, weather commonalities, and animal and plant commonalities (plants we have no def for are folded into a similar tree or left out, listed in each biome's `ours`). 12 of the 14 new animals have the wiki's speed, value, size, health, life span, meat, leather, wildness, food, gestation and litter. Still `unverified`: biome `climate` and `terrain` (the wiki only gives a minimum or maximum temperature), animal `tools`, and the lion and monkey (their wiki pages could not be fetched).
