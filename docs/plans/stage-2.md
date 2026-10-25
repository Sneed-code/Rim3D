# Stage 2: a colony that lasts a year

_Planned 2026-10-25. Builds on [`architecture.md`](../architecture.md), the Stage 1 systems in
`STATUS.md` and [`stage-1.md`](stage-1.md). Target: most of the "Vanilla mechanics" milestone in
[`game-design.md`](../game-design.md). The setting and names are ours ([`lore.md`](../design/lore.md),
[`naming.md`](../design/naming.md)), year 6650._

## What "done" looks like

Stage 2 is done when sneed can play this in a packaged build:

- A new game is on a 1 km map with rivers, marsh, ruins and geysers, in one of three biomes.
- A year goes by: four seasons, day and night, rain, fog, snow, heat waves and cold snaps. Rooms
  have their own temperature; heaters, coolers and campfires change it; colonists freeze or overheat.
- Colonists farm in growing zones, hunt, butcher, tame, train and breed animals, cook on a fuelled
  stove, craft at work tables with quality, make and wear clothes, research new buildings, and run a
  power grid with generators, batteries, lamps and switches.
- Fire spreads and is put out. Darkness slows work. Food rots unless it is kept cold.
- Doctors tend and rescue; wounds get infected; diseases come and go; surgery fits prosthetics.
- Colonists have recreation, comfort, beauty and room needs, friendships and rivalries, social
  fights and mental breaks.
- A narrator (Orrin, Maren or Jinx) sends events by RimWorld's threat-point rules: raids of
  several kinds, manhunter packs, disease, blight, short circuits, cargo pods, wanderers, traders.
- Downed raiders can be captured and recruited. Traders visit and you trade for silver.
- Two-storey buildings with stairs and ladders; upper floors collapse without support.
- The protagonist carries several weapons, fights in melee, throws grenades, dodge-rolls, has a
  talent tree, repairs and upgrades gear, and can skip the night and fast-travel.
- A 60-day colony-year scenario runs nightly in CI and passes.

**Not in Stage 2** (Stage 3 and later): the world map, caravans, quests and other maps; orbital
trade; drugs and addiction; sieges and mortars; Remnant clusters, chitinid brood mounds and binders;
creeds, the Black Spires and skyhulls; leaving the planet; art objects; the styled UI (panels stay
ImGui); sound; dismemberment visuals and full anatomy (Stage 4). Animals are the 15 in D5.

## Rules for every Stage 2 task

Stage 1's rules still hold (read `STATUS.md`, your row, and the one design doc you touch; a new
system writes `docs/design/<system>.md`; RimWorld numbers from the wiki, `unverified` and `ours`
marks; new sim state goes into the save, the save round-trip test and `Sim::stateHash()`; Unreal
tasks write code in the cloud first, then ask "Is the PC free for about 45 minutes?"; finish by
ticking your row, updating `STATUS.md`, merging once CI is green and telling sneed in plain words).
Also read the **bold rules in `STATUS.md`** (pawns with `PlayerControl` or `Fighter` are skipped by
new systems, new colonist behaviour gets a line in the colony scenario). On top of that:

1. **Many threads run at once, so keep merges clean.** New systems go in their own `.h`/`.cpp`
   and their own test file. Shared files (`Sim.cpp`, `Commands.h`, `Events.h`, `Snapshot.h`,
   `Save.cpp`, `simrun`) get short additions at the end of a list, never reorders. Whoever merges
   second fixes the conflict; it is never sneed's problem.
2. **Saves after S7.** Every system saves its own named section with its own version (S7 sets
   this up), so parallel tasks no longer fight over one save format number. Until S7 merges, only
   the wave 1 tasks run.
3. **Data tasks write new files.** A D task adds its own files (for example
   `data/defs/Things/Buildings_Power.json`) and touches shared files only to fix a mistake. Sim
   tasks that need a def the D task didn't write add it the same way, in a file of their own.
4. **Names.** Proper names from `naming.md` only. Animal, drug and fabric words in its "Proposed"
   section stay RimWorld's until sneed picks; a new proper name gets a row there first.
5. **Keep CI quick.** Unit tests and the 5-day colony scenario run on every PR. Anything longer
   (the year scenario, soaks, 300-animal runs) runs in the nightly job F7 sets up.
6. **Show it.** New state that Unreal should draw goes into the snapshot or events; the U task for
   it draws it. Until then `simrun` pictures and reports are how it is checked.

**Where it runs** and **Model** mean the same as in Stage 1.

## Tasks

Times are one sitting of thread work. "Needs" lists tasks that must be merged first. F6, F7, G1,
G2, A1 and A2 are Stage 1 tasks.

### Data (D)

| ✓ | ID | Task | Where | Model | Time | Needs |
| --- | --- | --- | --- | --- | --- | --- |
| ✓ | D4 | **Farming, climate and weather data.** Crops (rice, potatoes, corn, strawberries, haygrass, cotton, healroot, devilstrand) with growth days, yield, sow and harvest work, growing temperatures and fertility sensitivity; climate for the temperate forest plus boreal forest and arid shrubland (temperature by season, daily swing, rain, plants, which animals); weather kinds with chance, length and effects on light, speed and aim. | Cloud | Sonnet, medium | 60 min | none |
| ✓ | D5 | **Animal data.** Hare, squirrel, deer, wild boar, muffalo, alpaca, cow, chicken, husky, cat, timber wolf, grizzly bear, cougar, boomalope and thrumbo: bodies (four-legged and bird), race stats, meat and leather (written here), wildness, trainability, food, gestation or eggs, life stages, herd sizes, milk, wool and eggs, which biomes. | Cloud | Sonnet, medium | 90 min | none |
| ✓ | D6 | **Apparel and textile data.** Tribalwear, T-shirt, button-down shirt, pants, duster, jacket, parka, cowboy hat, tuque, simple helmet; layers, body coverage, armor, insulation, stuff categories; cloth, devilstrand, synthread and hyperweave; quality levels and their multipliers (`QualityRules`); the tailoring benches (hand and electric) and the apparel recipes. | Cloud | Sonnet, medium | 90 min | none |
| ✓ | D7 | **Medicine and disease data.** Herbal medicine, medicine, lumen medicine; tend rules; infection, flu, plague, malaria, sleeping sickness, gut worms, muscle parasites, food poisoning, hypothermia, heatstroke, frostbite, toxic buildup; scars and old wounds; peg leg, wooden hand, prosthetic and bionic arms, legs and eyes as items; surgery recipes; hospital bed stats. | Cloud | Sonnet, medium | 90 min | none |
| ✓ | D8 | **Production data.** Crafting spot, butcher table, stonecutter's table, smithy (fueled and electric), machining table, electric stove; stone blocks, butchering and smithing recipes; melee weapons (knife, club, gladius, spear, mace, longsword, breach axe) with RimWorld's tools, damage and cooldowns. | Cloud | Sonnet, medium | 90 min | none |
| ✓ | D9 | **Thoughts, recreation and mental break data.** Thoughts for temperature, clothes, nudity, rooms, beauty, recreation, comfort, outdoors, darkness, pain, deaths and social life; recreation kinds and the recreation buildings (horseshoes pin, chess table, billiards table); mental break kinds with thresholds and lengths; social interactions (chitchat, deep talk, slight, insult, kind words, romance and marriage as bonds only). | Cloud | Sonnet, medium | 90 min | none |
| | D10 | **Narrator, incident and faction data.** Orrin, Maren and Jinx with RimWorld's storyteller numbers and difficulty levels; incident defs (chance, cooldown, earliest day, points); threat points from wealth and colonists; the factions in `naming.md` with goodwill; visitor, trader and guard pawn kinds; trader kinds and price rules. Builds on F6's raider defs. | Cloud | Sonnet, medium | 90 min | F6 |
| ✓ | D11 | **Building data 2.** Power: wood-fired generator, solar generator, wind turbine, battery, conduit, switch. Climate: heater, cooler, vent, passive cooler, campfire. Light: standing lamp, sun lamp, torch lamp (glow radius and colour). Floors: wood floor, stone tiles, flagstone, carpet, smoothed stone. Furniture: armchair, end table, dresser, plant pot, columns. | Cloud | Sonnet, medium | 60 min | none |
| ✓ | D12 | **Research data.** The vanilla neolithic and industrial projects that unlock what D6, D7, D8 and D11 added (cost, tech level, prerequisites, bench needed, what each unlocks). | Cloud | Sonnet, medium | 60 min | D6, D7, D8, D11 |

### Sim foundations (S)

| ✓ | ID | Task | Where | Model | Time | Needs |
| --- | --- | --- | --- | --- | --- | --- |
| ✓ | S7 | **Saves and commands that merge in parallel.** Turn the save into named sections, each with its own version, listed in `saves.md`; today's formats 6 to 19 still load. Commands saved by name, not by their position in the `Command` variant. A short "how to add a system" checklist in `saves.md` and `bridge.md`. No behaviour change: every existing test and replay hash stays the same. | Cloud | Opus, high | 90 min | none |
| | S8 | **Map generation 2 and 1 km games.** Rivers (shallow and deep water, crossing costs), marsh and mud, the D4 biomes, old ruins (broken walls), steam geysers; new games default to 1,024 m. `simrun --png` shows it; generation time stays under a second. Old saves whose map no longer matches fail with a clear message. | Cloud | Sonnet, high | 90 min | D4, E1 |
| | S9 | **Sim cost at Stage 2 size.** `simrun` on 1 km with 30 colonists, 300 animals, crops, fire and room temperature at 3x: time per system, fix the worst, write the budget into the next `docs/decisions/` file. Runs nightly. | Cloud | Sonnet, high | 90 min | N1, E2, E4, C13 |

### Environment (E)

| ✓ | ID | Task | Where | Model | Time | Needs |
| --- | --- | --- | --- | --- | --- | --- |
| ✓ | E1 | **Seasons, weather and outdoor temperature.** RimWorld's 60-day year in four seasons, outdoor temperature from the biome's curve and the time of day, weather chosen by the seeded random stream with lengths and transitions, snow cover on the ground, rain and fog effects on speed, sight and aim. Events and snapshot. | Cloud | Sonnet, high | 90 min | S7, D4 |
| ✓ | E2 | **Room temperature.** Each room's temperature moves toward outdoors through walls, doors, vents and open roof, by RimWorld's rule; heaters, coolers and campfires push it. Pawns' comfortable range (apparel adds to it after C15), hypothermia, heatstroke and frostbite, temperature thoughts. | Cloud | Sonnet, high | 90 min | E1, D7, D9, D11 |
| ✓ | E3 | **Light and darkness.** A light grid from the sun (time of day, roofs) and from buildings that glow; darkness slows work and movement as in RimWorld; `lightAt` for others to use; `simrun` picture. Rebuilt, not saved, so it can start before S7. The darkness thought arrives with D9. | Cloud | Sonnet, medium | 60 min | none |
| | E4 | **Fire.** Fire per cell in active chunks: starts from heat sources and flammable stuff, spreads to plants, buildings, items and pawns, burns them, leaves ash and burnt ground; rain and snow put it out; pawns on fire beat it out; a home area and the Firefighter work type's job. | Cloud | Sonnet, high | 90 min | E1 |
| ✓ | E5 | **Power.** Power nets from conduits and buildings; wood-fired (fuel from C11), solar (from E3's sun) and wind generators; batteries; switches; buildings without power stop (lamps, heaters, electric stove, benches). A short-circuit hook for T3. | Cloud | Sonnet, high | 90 min | C11, D11, E3 |

### Multi-storey (M)

| ✓ | ID | Task | Where | Model | Time | Needs |
| --- | --- | --- | --- | --- | --- | --- |
| | M1 | **Upper levels.** Sparse storage for levels above the ground (about 3 m each); a floor on level 1 is the roof of level 0; building on an upper level needs support below (walls or columns); regions and rooms per level. Every interface already takes a level (`CellPos`); this fills them in. | Cloud | Opus, high | 120 min | S7 |
| | M2 | **Stairs and ladders.** Buildings that link two levels; region links and reachability across levels; the region-graph search and cell paths climb them; climbing speed; doors and stockpiles on upper levels. | Cloud | Opus, high | 90 min | M1 |
| | M3 | **Roof support and collapse.** RimWorld's roof support range, collapse when support is lost (falling roof damage), overhead mountain collapse when mining under thick roof, upper floors that fall when the walls under them go. | Cloud | Sonnet, high | 90 min | M1 |
| | M4 | **Two-storey scenario test.** Colonists build a two-storey house with stairs, sleep upstairs and haul up and down for 3 days; `simrun --png` per level. In CI. | Cloud | Sonnet, high | 60 min | M2, M3 |

### Colony life (C)

| ✓ | ID | Task | Where | Model | Time | Needs |
| --- | --- | --- | --- | --- | --- | --- |
| | C11 | **Fuel.** A refuelable part for buildings: the stove burns wood per meal (BACKLOG), campfires, torches, later generators; a refuel job under Hauling; out of fuel means stopped. | Cloud | Sonnet, medium | 60 min | S7 |
| | C12 | **Skill learning everywhere.** XP from every job that uses a skill (building, cooking, doctoring later), passions, RimWorld's daily XP cap and the slow loss above level 10; a mood bonus for passion work. | Cloud | Sonnet, medium | 60 min | S7 |
| | C13 | **Growing zones and farming.** Growing zones with a chosen crop, sow and harvest jobs under Growing, growth from fertility, light and temperature, frost kills, harvest yield from the Plants skill. | Cloud | Sonnet, high | 90 min | E1, D4 |
| | C14 | **Work tables, crafting and quality.** Bills on every bench, not just the stove; products with quality from the crafter's skill (RimWorld's curve); stone blocks, smithing, tailoring and butchering; the bill extras in BACKLOG (pause at a full stock, where products go, ingredient filters, skill range). | Cloud | Sonnet, high | 90 min | C11, D6, D8 |
| ✓ | C15 | **Apparel.** Wearing clothes in layers; armor per covered part (F2's flak rule for every piece); insulation widens the comfortable temperature range; wear lowers hit points; tattered, naked and dead man's clothes thoughts; outfit policies and a change-clothes job; stripping the dead and downed. | Cloud | Sonnet, high | 90 min | S7, D6, D9 |
| ✓ | C16 | **Rot and storage.** Food rots by temperature (cold slows it, freezing stops it), items left outdoors deteriorate, food poisoning from cooking skill and a dirty kitchen, meals remember their ingredients. | Cloud | Sonnet, medium | 60 min | E2, C17 |
| ✓ | C17 | **Room stats, beauty and filth.** Beauty per cell from things and floors; filth (dirt walked in, blood, ash) and a cleaning job; room space, beauty, wealth, cleanliness and impressiveness by RimWorld's formulas; room thoughts; the beauty need. | Cloud | Sonnet, high | 90 min | S7, D9, D11 |
| ✓ | C18 | **Recreation, comfort and outdoors.** The recreation need with kinds and tolerance, recreation jobs (horseshoes, chess, billiards, walking, cloud watching, chatting at a table) in Joy hours; comfort from chairs and beds; the outdoors need; their thoughts. | Cloud | Sonnet, high | 90 min | S7, D9, D11 |
| | C19 | **Mental breaks.** RimWorld's break chances by mood band; minor (wander sadly, food binge, hide in room), major (tantrum, sad wander, insulting spree) and extreme (berserk, give up and leave, fire-starting spree after E4) breaks; catharsis. | Cloud | Sonnet, high | 90 min | S7, D9 |
| | C20 | **Social life.** Opinions, interactions at RimWorld's rates, friends, rivals, lovers and spouses as bonds (no sex, no pregnancy), social fights with fists (F8), social thoughts, Social skill XP. | Cloud | Sonnet, high | 90 min | F8, D9 |
| ✓ | C21 | **Research.** Research benches, a current project, the research job (Intellectual), unlocks that gate building plans and bills; commands to pick a project. | Cloud | Sonnet, medium | 60 min | S7, D12 |
| | C22 | **Prisoners.** Capture downed enemies to a prison bed; warden feeding, chats and recruiting against resistance as in RimWorld; release; prisoner thoughts; prison-break hook for T3. | Cloud | Sonnet, high | 90 min | H1 |
| | C23 | **Visitors and traders.** Faction goodwill; visitor groups walk in, stay a while and leave; trader caravans with stock; a trade command with RimWorld's prices (Social skill), silver as money. | Cloud | Sonnet, high | 90 min | T1, D10 |
| ✓ | C24 | **Carry weight and hauling 2.** Item mass, carry capacity, carrying several stacks at once, slower walking when loaded, stacks merging in stockpiles. | Cloud | Sonnet, medium | 60 min | S7 |
| | C25 | **Colony-year scenario.** `simrun --scenario year`: the Crashlanded colony on a 1 km map for 60 days with farming, clothes, fuel, temperature, animals and the narrator. Checks: nobody starves or freezes, nobody stuck for a game hour, food stays above zero. Nightly. | Cloud | Sonnet, high | 90 min | C13, C14, C15, E2, E4, H2, N3, T2 |

### Health and medicine (H)

| ✓ | ID | Task | Where | Model | Time | Needs |
| --- | --- | --- | --- | --- | --- | --- |
| | H1 | **Doctors, medicine and rescue.** Medicine items; the Doctor job tends patients in bed, quality from Medicine skill, medicine and bed; patient and bed-rest work; medical beds; rescuing downed colonists to a bed; scars from healed wounds. | Cloud | Sonnet, high | 90 min | S7, D7 |
| | H2 | **Infections and diseases.** Infection chance from wound tending; RimWorld's immunity race (immunity against severity); flu, plague, malaria, sleeping sickness, gut worms, muscle parasites, food poisoning; a hook for T3's outbreaks. | Cloud | Sonnet, high | 90 min | H1 |
| | H3 | **Surgery and prosthetics.** Operation bills on a pawn (amputate, install or remove a part); success chance by RimWorld's rule (skill, bed, medicine, cleanliness); body parts as items; prosthetics and bionics change capacities. | Cloud | Sonnet, high | 90 min | H1, C14 |

### Animals (N)

| ✓ | ID | Task | Where | Model | Time | Needs |
| --- | --- | --- | --- | --- | --- | --- |
| ✓ | N1 | **Animals on the map.** Animal races with their bodies and health; wild herds placed by biome at map generation and wandering in and out at the edges; grazing from the plant layer, sleep, corpses. Hundreds stay cheap (`simrun --animals`). | Cloud | Sonnet, high | 90 min | S7, D5 |
| | N2 | **Hunting and butchering.** A hunt mark and the Hunting job (shoot, carry the corpse home); butchering at the butcher table for meat and leather (so the fine meal can be cooked); hunted animals may turn on the hunter. | Cloud | Sonnet, high | 90 min | N1, C14 |
| ✓ | N3 | **Taming, training and pens.** Tame mark and Handling job (wildness, food), tamed animals join the colony, training (obedience, release, rescue, haul), animal zones and pens, feeding hay and kibble, slaughter. | Cloud | Sonnet, high | 90 min | N1 |
| ✓ | N4 | **Breeding and products.** Animal mating, gestation and eggs, young animals growing up, milking and shearing jobs, egg laying; a 500-chicken farm stays cheap. | Cloud | Sonnet, high | 90 min | N3 |
| ✓ | N5 | **Animal fights and predators.** Bites and scratches through F8's melee; predators hunt prey and lone pawns; revenge; the manhunter state; animals flee from fights. | Cloud | Sonnet, high | 90 min | N1, F8 |

### Fighting (F)

| ✓ | ID | Task | Where | Model | Time | Needs |
| --- | --- | --- | --- | --- | --- | --- |
| ✓ | F8 | **Melee.** Melee weapons and fists from D8 with wind-up, hit moment and recovery in the sim; reach checked against hitboxes; blocks and dodges with timing (Melee skill); the fighter AI charges only when armed for it or the trait says so (Brawler). | Cloud | Opus, high | 90 min | S7, D8 |
| ✓ | F9 | **Explosives and grenades.** An explosion system (radius, falloff, damage to pawns and buildings, knock-down); frag grenades thrown as arcing projectiles; fighters throw them at groups in cover and dive away from them (F5's sidestep grown into a dive). Molotovs once E4 is in. | Cloud | Sonnet, high | 90 min | S7 |
| | F10 | **Medics, firefighters and rescue under fire.** The alarm's Medic and Firefighter roles from `game-design.md`; fighters drag downed friends to safety when the route is reasonably safe; scenario checks in F7's nightly run. | Cloud | Opus, high | 90 min | F6, H1, E4 |
| ✓ | F11 | **Turrets and traps.** Powered mini-turret (F2's guns, auto targeting, blows up when destroyed), spike trap and IED trap; the fighter AI avoids known traps. | Cloud | Sonnet, medium | 60 min | E5, F9 |
| | F12 | **Raid strategies.** Sappers who dig or blast through walls, drop-pod raids landing inside the base, raiders who steal or kidnap and run, staged attacks at night; picked by T1's points. | Cloud | Opus, high | 120 min | T1, F9 |

### Protagonist (P)

| ✓ | ID | Task | Where | Model | Time | Needs |
| --- | --- | --- | --- | --- | --- | --- |
| ✓ | P1 | **Several weapons.** Carry a rifle, a sidearm and a melee weapon; a switch command with a switch time; the fighter AI picks by range; weight counts (C24 if merged). | Cloud | Sonnet, high | 90 min | F8 |
| ✓ | P2 | **Recoil kicks and dodge roll.** Per-weapon recoil kick lists in weapon defs for the player's shots; a dodge-roll command with start-up, a short no-hit window and recovery, moved by the sim (`shooter-feel.md`). | Cloud | Sonnet, high | 60 min | S7 |
| ✓ | P3 | **Talent tree.** Talent XP and points for the protagonist, perks from data (`Talents/`, numbers `ours`: faster building, steadier aim, more carry weight, better trading), applied as stat changes; a spend-point command. | Cloud | Sonnet, medium | 60 min | S7 |
| | P4 | **Repair and weapon upgrades.** Repair clothes, armor and weapons at a bench; upgrade parts (scope, longer magazine, better barrel; `ours`) fitted by recipe and changing the weapon's stats. | Cloud | Sonnet, high | 90 min | C14, C15 |
| | P5 | **Night skip and fast travel.** Skip to morning when everyone off night shift is asleep and no threat is active, moving needs, growth and healing forward in one go and holding the narrator; fast travel to visited places when no threat. Same seed and commands still give the same game. | Cloud | Opus, high | 90 min | T1, E1 |

### Narrator and events (T)

| ✓ | ID | Task | Where | Model | Time | Needs |
| --- | --- | --- | --- | --- | --- | --- |
| | T1 | **Narrator, incidents and threat points.** Colony wealth; threat points from wealth and colonists by RimWorld's curves; an incident system (weights, cooldowns, earliest day); Orrin, Maren and Jinx as RimWorld's three storytellers; letters as events; F6's raid becomes the first incident. | Cloud | Opus, high | 90 min | F6, D10 |
| | T2 | **Events: nature and good news.** Wanderer joins, cargo pods, crashed pod with a downed survivor, herd migration, animal joins, cold snap, heat wave, eclipse, aurora, berry or healroot bloom. | Cloud | Sonnet, medium | 90 min | T1, E1, E3, N1 |
| | T3 | **Events: trouble.** Manhunter pack, mad animal, disease outbreaks, blight, short circuit, solar flare, lightning fires, toxic fallout, resonance drone, prison break. | Cloud | Sonnet, medium | 90 min | T1, N5, H2, E4, E5, C13 |

### Expansion pieces (X), when a thread is free

| ✓ | ID | Task | Where | Model | Time | Needs |
| --- | --- | --- | --- | --- | --- | --- |
| ✓ | X1 | **Genes and lineages.** Genes as defs with stat effects; Emberkin, Frostkin, Warbred, Ashborn and Delver as gene sets; starting colonists can roll a lineage; looks (A3) read the genes. | Cloud | Sonnet, high | 90 min | S7 |
| | X2 | **Resonance, first powers.** Attunement levels, resonance strain and focus (names added to `naming.md`), four powers from Royalty's first level (stun, painblock, burden, short jump) through the sim's commands and the fighter AI. | Cloud | Opus, high | 90 min | S7, F9 |

### Unreal side (U)

| ✓ | ID | Task | Where | Model | Time | Needs |
| --- | --- | --- | --- | --- | --- | --- |
| | U13 | **Sky, seasons and weather.** Sun and moon from the sim clock, sky, fog and clouds; rain and snow effects; snow on the ground; wet look; a date, season and temperature readout. | PC Unreal | Sonnet, high | 90 min | E1 |
| | U14 | **Light and fire on screen.** Lamps, torches and campfires as real lights from the sim's light sources; Niagara fire, smoke and embers from the fire layer; burnt ground and ash. | PC Unreal | Sonnet, high | 90 min | E3, E4 |
| ✓ | U15 | **Animals on screen.** Animal actors on shared skeletons with animation by distance (as U5); far herds as cheap instanced meshes with baked animation. | PC Unreal | Sonnet, high | 90 min | N1, A4 |
| | U16 | **Clothes on pawns.** Each worn layer drawn on the MetaHuman body with the body-type shapes, torn look from hit points, clothes that come off when destroyed. | PC Unreal | Sonnet, high | 90 min | C15, A5 |
| | U17 | **Upper floors on screen.** Upper levels, stairs and ladders drawn; the overseer view cuts away levels above the chosen one (Page Up and Page Down); direct mode climbs stairs and ladders through the sim. | PC Unreal | Opus, high | 90 min | M2, M3 |
| | U18 | **Panels 2.** Health tab with operations, research tab, room stats in the inspect panel, prisoner tab, growing zone and home area tools. | PC Unreal | Sonnet, medium | 90 min | H3, C21, C17, C22, C13 |
| | U19 | **Panels 3.** Animals tab, outfits, trade window, power, temperature and beauty overlays. | PC Unreal | Sonnet, medium | 90 min | N3, C15, C23, E5, E2 |
| | U20 | **Letters and the narrator.** Letters (raids, events) with a jump-to button, an alert list, choosing the narrator and difficulty at game start. | PC Unreal | Sonnet, medium | 60 min | T1 |
| | U21 | **Shooter feel on screen.** Aim-hold camera blend, recoil camera kick, camera shake and motion blur from sim events (with settings sliders), dodge-roll animation, grenade and explosion effects with Chaos debris (`shooter-feel.md`). | PC Unreal | Sonnet, high | 90 min | P2, F9 |
| | U22 | **Melee and weapon switching on screen.** Swing, block and dodge animations timed to the sim, hit-stop and flinch, the protagonist's melee input, weapon switch key and holstered weapons. | PC Unreal | Sonnet, high | 60 min | F8, P1 |

### Art (A)

| ✓ | ID | Task | Where | Model | Time | Needs |
| --- | --- | --- | --- | --- | --- | --- |
| ✓ | A4 | **Animal art.** Free animal packs (Fab) for the D5 animals, retargeted onto a few shared skeletons; logged in `assets/manifest.json`. The thread sends sneed the download steps. | PC Unreal | Sonnet, medium | 90 min | U5, sneed's Fab download |
| ✓ | A5 | **Clothes art.** Free clothing that fits MetaHuman bodies (MetaHuman's own outfits first) for the D6 apparel, per layer; logged in the manifest. | PC Unreal | Sonnet, high | 90 min | A3, sneed's download if needed |
| ✓ | A6 | **Placeholder art 2.** Crops, work tables, power and climate buildings, furniture and floors, set in the defs' `visual` fields by an editor script. | PC Unreal | Sonnet, medium | 60 min | A1, D8, D11 |

### Wrap-up (G)

| ✓ | ID | Task | Where | Model | Time | Needs |
| --- | --- | --- | --- | --- | --- | --- |
| | G3 | **Performance check in a packaged build.** 1 km map, 30 colonists, 300 animals, rain and a fire, then a 40-raider fight; record fps, game thread, GPU and sim cost in the next `docs/decisions/` file; decide whether the sim moves to its own thread. | PC Unreal | Sonnet, high | 90 min | S9, U14, U15 |
| | G4 | **Playtest build 2.** Package Stage 2, update `docs/setup/playtest.md`, sneed plays a season or more; notes go to `BACKLOG.md` and the Stage 3 plan. | PC Unreal | Sonnet, medium | 60 min | everything above it except X |

## Order

74 tasks, roughly 105 hours of thread work, about 85 of it in the cloud. Cloud tasks with their
needs met run side by side (rule 1 keeps merges clean); Unreal tasks go one at a time. Within a
wave, start any task whose needs are merged.

| Wave | Cloud | PC |
| --- | --- | --- |
| 1 (now) | S7, E3, D4, D5, D6, D7, D8, D9, D11 (and Stage 1's F7 when F6 is in) | Stage 1's A1, G1, G2 |
| 2 | E1, C11, C12, C15, C17, C18, C19, H1, N1, F8, F9, M1, P2, P3, C24, D10, D12 | A4, A6 |
| 3 | E2, E4, E5, C13, C14, C20, C21, C22, H2, N3, N5, M2, M3, P1, T1, S8 | U13, A5 |
| 4 | C16, H3, N2, N4, M4, F10, F11, F12, C23, T2, T3, S9, P4, P5 | U14, U15, U20 |
| 5 | C25, X1, X2 | U16, U17, U21, U22, U18 |
| 6 | | U19, G3 |
| 7 | | G4 |

The long chains are S7, E1, E2 or E4, then the events (T3) and the year scenario (C25); and S7,
M1, M2, M3, then U17. Opus is used for S7, M1, M2, F8, F10, F12, T1, P5, X2 and U17 (10 tasks);
everything else is Sonnet. G2's playtest notes from Stage 1 may add or reorder tasks here.

**If we fall behind**, these move to Stage 3 first, in this order: X2, X1, F11, P4, N4, C24,
F12. The year still plays without them.

## What sneed does in Stage 2

- **Say when the PC is free** when a thread asks, and **approve Remote Control**.
- **Download the animal and clothing packs** for A4 and A5 when those threads send the
  click-by-click steps.
- **Pick names** (optional): animals and some items still have RimWorld's words; `naming.md`'s
  "Proposed" table has ours waiting for a yes.
- **Look at screenshots** along the way, and **play a season or more** at G4.
