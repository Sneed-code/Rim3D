# Art stage: how the game looks, moves and sounds

_Planned 2026-10-26 from sneed's wishes in `game-design.md`, `architecture.md`, decisions 0001 and
0002, the design docs, `BACKLOG.md` and the memory notes. It runs beside Stage 2 and Stage 3
(art needs the PC; the systems are mostly cloud) and is finished before the "vanilla core" is
called done. Rules: free assets only (logged in `assets/manifest.json`), our own setting and
names, adults only, nudity yes and sex no (`game-design.md` "Content rules")._

## Where each wish stands (checked 2026-10-26)

| Wish | State | Where |
| --- | --- | --- |
| Realistic graphics (terrain, light, sky, weather) | Placeholder look (sneed, 2026-10-27: "it looked awful") | Terrain A2, sky and weather U13, fire and lamps U14 work but look like a prototype. The **look pass L1 to L10** (below) replaces V18 and V19 and goes first |
| Realistic physics | Partly done | Ragdolls U11, grenade debris U21 done. Building collapse, falling trees, roof collapse debris: **V17** |
| MetaHuman bodies, seeded variety (body type, height, skin, hair colour) | Done | A3 (`looks.md`) |
| Seeded **faces** and hair styles | Not planned until now | **V1, V2** |
| Body hair, scars, tattoos | Written in the design, not built | **V3** |
| True nudity with anatomy parts (adult, non-sexual) | Planned "Stage 4", no task | Skin layer under clothes exists (U16). Anatomy: **V4, V5** |
| Jiggle physics | Planned "Stage 4", no task | **V6** |
| Wounds that show on the body | Partly done | Blood and decals done (U11, U22). Per-part wounds, bandages: **V7** |
| Gore and dismemberment | Planned "Stage 4", no task | The sim already loses body parts (`PawnPartLost`). Drawing it: **V8, V9** |
| Corpses that rot | Sim rots them; the look is not drawn | **V9** |
| Destructible clothes | Half done | U16: torn look and gone when worn out. Shredding where hit, loose pieces: **V10** |
| Real human animations (melee, roll, climb, work, carry, sleep) | Not done | Melee is procedural poses (U22), roll is a somersault (U21), climbing and work have none (`handson.md`). **V11 to V14** |
| Realistic fire (Niagara) | Not done | U14 uses instanced meshes and lights; "real Niagara fire is later". **V15** |
| Explosions and smoke | Basic (U21) | **V15** |
| Animals | Done | A4, U15 (15 animals) |
| Buildings, items, weapons: one real look | Placeholders (A1, A6) | **L4, L7, L8** (replace V16) |
| Sound (guns, steps, work, ambience, music) | Not done anywhere | **V20, V21** |
| Styled UI (the panels are ImGui) | Not done | **V22 to V24** |
| Remnant, chitinids, other expansion creatures | Not in this stage | Come with their expansion pieces |

## Rules for every art task

1. Read `STATUS.md`, your row, and the design doc it names (`looks.md`, `clothes-draw.md`,
   `fights.md`, `fire-draw.md`, `smoke-tests.md`).
2. Free assets only. Download by script (`tools/*.py`, like `placeholder_art.py`) where the
   licence allows, otherwise the thread gives sneed click-by-click steps. Log every pack in
   `assets/manifest.json` with its licence and a reinstall line. Sites that need a sign-in
   (Mixamo needs Adobe) are the fallback, not the plan.
3. Everything generated is made by an editor commandlet (text over binary), with a smoke test
   and screenshots in `Saved/`. Anything big stays out of Git.
4. **Look system stays swappable** (`architecture.md`): body parts, hair and wounds attach
   through the existing `ApplyLooks` interface so the body source can still change.
5. Cost: every task states its CPU/GPU budget. The target stays 60 fps with 100 pawns; G3's
   performance check covers the finished result. Far pawns skip the new detail.
6. Each Unreal task ends with before and after screenshots that sneed can judge, then the
   usual tick, STATUS.md, PR and merge. **PC Unreal** tasks need the PC free (one at a time,
   low priority); **Cloud** tasks do not.

## Look pass (planned 2026-10-27): make the world look real first

sneed saw the screenshots on 2026-10-27 and said the game "looked awful". They were right: the
**pawns** (MetaHuman bodies and faces, V1 to V3) look like a modern game, but **everything round
them is still prototype art**. This pass fixes the world first, biggest change on screen first.
It replaces V16, V18 and V19 (their rows below say so) and runs before the rest of the art stage on
the PC. Real animation (V11, V12) can slot in after L4.

### What is wrong now (from the screenshots in the project files)

| Problem | Why it looks like that | Fix |
| --- | --- | --- |
| Hundreds of yellow-green **pyramids** on the ground | Grass, tall grass, dandelions and brambles are drawn as coloured cones and spheres (`Plants.json` `visual`), and the rock chunks as tinted boxes | **L2**, **L8** |
| Flat, washed-out, purple-tinted light, no depth | The sun is 10 lux with the engine's default exposure (`sky.md`: "found by eye"), no colour grade, no tuned Lumen, fog colour not from the sky | **L1** |
| **Walls are plain white boxes**; floors are flat colours | Walls were left as shapes until "the real wall art" (`building-draw.md`); the stuff colour is the only look | **L4** |
| Ground looks like a blurry tiled carpet with stair-step edges | 1k textures, four layers, weights per cell corner; water is a coloured layer, not water | **L3** |
| Furniture and trees look flat and plasticky | `M_ArtLit` uses the colour texture only, no normal or roughness maps; trees are the wrong species stretched to fit | **L6**, **L7** |
| Guns, steel, components, medicine are boxes | No free model was found in Poly Haven | **L8** |
| Animals are cartoon low-poly (Quaternius) next to realistic people | First free set that came with animations | **L9** |
| Debug numbers and the key help over the picture | `Sim.ShowTimings` is on by default; the help box is always shown | **L1**, **L5** |

What "realistic" can mean with free assets: a good-looking indie survival game, close to the
Electric Dreams demo's forest at its best, not a big-studio game. Nothing here needs a paid asset.

### Where the art comes from

| Source | Licence | How it is fetched | Used for |
| --- | --- | --- | --- |
| **Electric Dreams Environment** (Epic's free sample on Fab: Megascans plants, rocks, cliffs, ground textures, made for Nanite) | Epic content, usable in Unreal Engine projects | sneed's clicks once (Fab sign-in, below), then a commandlet copies only what we use into `game/Content/ThirdParty/Environment/` | grass, flowers, ferns, bushes, trees, rocks, cliffs, ground textures (L2, L3, L6) |
| **Poly Haven** (already used) | CC0 | script (`tools/placeholder_art.py`, `terrain_textures.py`), now at 2k with normal and roughness maps | furniture, ground and rock textures, more grass and plant models; the fallback for everything if Electric Dreams will not open in 5.8 |
| **ambientCG** | CC0 | script | wall, floor and roof materials: planks, logs, bricks, stone blocks, plaster, concrete, metal plates, tiles (L4) |
| **Fab free Epic content** (weapon packs such as Military Weapons, melee weapons from the Infinity Blade sets, animal packs) | Epic content, usable in Unreal Engine projects | sneed's clicks ("Add to My Library"), checked as the task starts because Fab's free list changes | guns, melee weapons, maybe animals (L8, L9) |
| **Fab's free packs every two weeks** | as listed | sneed claims any that fit, when they appear | anything; logged like the rest |
| Quixel Megascans on their own | paid on Fab since 2025, except what Epic's samples bundle | not used unless free | |

Every pack goes in `assets/manifest.json` with its licence and reinstall line, as before. Nothing
sexual, adults only, our own names (`naming.md`).

### Rules for the look pass (on top of the art rules above)

1. **L1 makes the look bench**: a `look` smoke test that loads one fixed seed and takes the same
   shots every time (overseer high, overseer low across a field, inside a lit room, a colonist close
   up, dusk, a rainy night, a snowy day) into `Saved/Look/<date>/`, plus a side-by-side sheet with the
   last run. **Every L task ends with that sheet** for sneed to judge, and attaches it to the thread.
2. Everything cosmetic stays cosmetic: the extra grass carpet, pebbles and debris are not sim things.
   Where they stand comes from a hash of the cell, like the plants now.
3. Frame budget: the G3 scene (1 km, 30 colonists, 580 animals, a raid, rain, 1440p) must stay above
   90 fps on sneed's PC at High, and every task says what it cost. Wind in grass and trees may move
   vertices only with velocity output on (`terrain.md` "Upscalers"), so DLSS stays possible.
4. A `Look.Quality` setting (Low, Medium, High, Epic) from L1 on; each task says what it does at each.
5. Big downloads stay out of Git and under the project's 250 GB disk budget; the task checks free
   space before downloading.

### Look tasks (in this order, one Unreal task at a time)

| ✓ | ID | Task | Where | Model | Time | Needs |
| --- | --- | --- | --- | --- | --- | --- |
| ✓ | L1 | **Light, camera and the look bench.** _(done 2026-10-28: `docs/design/look.md`, `data/look.json`, smoke test `look`)_ The look bench (rule 1). Real-world light: the sun about 100,000 lux at a clear noon and the moon, sky light and lightning scaled to match, with a fixed auto-exposure range so day, dusk and night each read right; Lumen global illumination and reflections at High, virtual shadow maps, contact shadows, ambient occlusion; fog and haze coloured by the sky atmosphere (no more purple), volumetric fog with light shafts at dawn and dusk; a neutral film tone curve and one colour grade per season (`sky.md` numbers move to a data file). `Look.Quality` Low to Epic. `Sim.ShowTimings` defaults to 0 and the key help hides behind F1. No downloads. sneed does the Fab clicks (below) while this runs. | PC Unreal | Opus, high | 90 min | |
| | L2 | **Grass, flowers and ground cover.** Grass, tall grass, dandelions, brambles and the other wild small plants get real meshes (Electric Dreams, else Poly Haven), several variants each, picked by the cell hash; a dense **cosmetic grass carpet** on fertile ground (thicker on rich soil, thin on poor, none on floors, under buildings, on paths pawns use a lot or where the sim's grass was eaten), pebbles and twigs on gravel and sand; wind sway; fades out with distance; snow and wetness reach it. The cones are gone. | PC Unreal | Opus, high | 120 min | L1, Electric Dreams downloaded (or Poly Haven fallback) |
| | L3 | **Ground and water.** 2k ground textures with more layers (soil, rich soil, dry grass, moss, mud, sand, gravel, rock, snow-friendly), blended by height maps and noise so the cell edges vanish, far-distance texture variation so nothing repeats; the rock blocks' sides get a cliff material with large-scale detail; **water becomes a surface** (the engine's water shading: depth colour, ripples, reflections, a shore line, wet banks) for lakes, rivers and marsh; floors (wood, stone tiles, concrete, carpet) become textured surfaces by their def. | PC Unreal | Opus, high | 120 min | L1 |
| | L4 | **Walls, doors and roofs.** Walls stop being boxes: straight, corner, T, cross and end pieces chosen by the neighbours (bevelled edges, a base and a top), with real materials per stuff (wood planks, logs, granite, limestone, marble, sandstone and slate blocks, steel and plasteel plates, from ambientCG); doors get a frame and the right material; the thin roof gets a roof material seen from outside and eaves; frames under construction look like a part-built wall with scaffolding planks. Columns and fences too. Two sittings if needed. | PC Unreal | Opus, high | 120 min (maybe x 2) | L1 |
| | L5 | **Quick UI style.** Until V22's styled UI: a dark frontier ImGui theme (colours, rounded frames, spacing, one readable OFL font at two sizes), panels that don't cover the middle of the screen, icons on the menu row, dev text only behind a dev toggle. | PC Unreal | Sonnet, medium | 60 min | L1 |
| | L6 | **Trees, bushes and rocks.** One real mesh per tree species (oak, poplar, pine, willow, teak, palm, cecropia) and bush, from Electric Dreams or Poly Haven, Nanite where it helps, wind, a felled log and a stump left after chopping, dead trees grey; rock chunks become real rock meshes (several shapes by hash); boulders and outcrops scattered along rock edges so mountains don't look like stacked boxes. | PC Unreal | Sonnet, high | 120 min | L2 |
| | L7 | **Full materials on furniture and items.** `M_ArtLit` gets normal and roughness maps (and Poly Haven 2k); wooden furniture takes the stuff's wood or metal look; the still-shape buildings (conduit, power switch, solar panel, wind turbine, vent, passive cooler, horseshoes pin) get models; stoves and lamps glow, beds show bedding. | PC Unreal | Sonnet, high | 90 min | L1 |
| | L8 | **Weapons and resources.** Guns and melee weapons from Fab's free Epic weapon packs (held in hand by pawns and lying on the ground); steel ingots, components, silver coins, medicine kits, cloth, leather, chunks; piles that grow with the stack size; icons rendered from the meshes for the UI later. | PC Unreal | Sonnet, high | 120 min | L7, sneed's Fab clicks |
| | L9 | **Realistic animals.** Try free realistic animal packs on Fab first; where none fits, keep Quaternius' animations but give the meshes realistic fur materials and proportions. sneed judges from the bench before anything is swapped. | PC Unreal | Sonnet, high | 120 min | L1 |
| | L10 | **Look check.** The G3 scene and the look bench in a packaged build at each `Look.Quality`; tune cut-offs; sneed plays 30 minutes and says what still looks wrong; notes go to `BACKLOG.md` and this plan. | PC Unreal | Sonnet, medium | 60 min | L1 to L9 |

Then the rest of the art stage as planned: V15 (Niagara fire), V11 and V12 (real animation; they can
go right after L4 if sneed prefers people moving well over trees looking right), V7, V17, V13, V14.

### What sneed does for the look pass

**Once, while L1 runs: get Epic's free Electric Dreams sample** (about 20 to 30 GB; the thread checks the
size and free space first, and can do these clicks itself through the computer link if sneed allows):

1. Open https://www.fab.com and sign in with your Epic Games account (the one in the Epic launcher).
2. Search for **Electric Dreams Environment** (seller: Epic Games). Click it, then **Add to My Library**
   (it is free).
3. Open the **Epic Games Launcher**, click **Unreal Engine** on the left, then **Fab Library** at the top.
4. Find Electric Dreams, click **Create Project**, set the folder to `C:\Dev\Samples` and the engine to
   **5.8** (or the newest one offered), and click **Create**. Leave it downloading; nothing else is needed.

For **L8 and L9** the thread sends the same kind of steps for the weapon and animal packs it picks.
After each task: **look at the before and after sheet** and say what still looks wrong.

## Tasks

### Faces, hair and skin (V1 to V3)

| ✓ | ID | Task | Where | Model | Time | Needs |
| --- | --- | --- | --- | --- | --- | --- |
| ✓ | V1 | **Looks seed, part 2 (sim).** Add to `PawnLooks` a face seed (face shape, eye colour, brow, nose, jaw, each a small number), hair style, beard (men), body-hair amount, tattoo and scar slots; filled from the seed and the lineage genes, saved, hashed and tested like A3. Scars come from healed injuries (`PawnInjured` healing into a scar, `ours`). | Cloud | Sonnet, high | 90 min | A3 |
| ✓ | V2 | **Faces and hair on screen.** Free face variety: MetaHuman face morph targets mixed by the face seed (same DNA route as A3's bodies), eye colour, brows, 8 or more groom hair styles and beards from MetaHuman's own grooms; far pawns use hair cards. Ten colonists side by side must not look alike. | PC Unreal | Opus, high | 120 min (two sittings) | V1 |
| ✓ | V3 | **Body hair, scars, tattoos.** Body hair as a texture layer in `M_PawnSkin` (chest, arms, legs, by amount and hair colour, fading with distance), scars and tattoos as decal layers on the skin material by body part. | PC Unreal | Sonnet, high | 90 min | V1 |

### Nudity and anatomy (V4 to V6)

All adult and non-sexual, as in the content rules: complete bodies when unclothed, nothing
else. No sexual animations, poses or interactions are planned anywhere.

| ✓ | ID | Task | Where | Model | Time | Needs |
| --- | --- | --- | --- | --- | --- | --- |
| | V4 | **Anatomy decision test.** Compare the two routes side by side on the same pawn: (a) MetaHuman plus attached anatomy parts and skin textures, (b) MPFB2 (Blender, free) or sneed's MakeHuman assets (`assets/` and the `architecture.md` risk table). Judge by looks under clothes layers, morph-to-body-type fit, and cost. sneed picks from screenshots. Result in a `docs/decisions/` file. | PC Unreal (sneed supplies the MakeHuman files if route b) | Opus, high | 120 min | A3, V3 |
| | V5 | **Anatomy parts in the game.** Build the winning route: part variants chosen by the pawn's seed, fitted to each body type, tinted by skin, hidden under clothes, shown when stripped, destroyed or in a nudist's thought. Sim side is only a looks field plus the existing nudity thoughts (check `mood.md`). | PC Unreal | Opus, high | 120 min | V4 |
| | V6 | **Jiggle physics.** Soft-body secondary motion on chest, belly and hips (and hair and cloth ends) from helper bones driven by Kawaii Physics (free) or the engine's AnimDynamics; amount per body type and weight; off beyond 15 m so the CPU budget holds; a `Looks.Jiggle` slider. | PC Unreal | Sonnet, high | 90 min | V5 |

### Wounds, gore and clothes (V7 to V10)

| ✓ | ID | Task | Where | Model | Time | Needs |
| --- | --- | --- | --- | --- | --- | --- |
| ½ | V7 | **Wounds on bodies.** _(cloud half done 2026-10-26: `docs/design/wounds.md`; Unreal half left)_ Snapshot gets each pawn's injuries by part and kind (cut, bruise, burn, bite, gunshot, infection, scar) if it does not have them yet (cloud half, small, tested); Unreal draws them as skin-material layers per body part, bandages on tended wounds, a blood-soaked look on bleeding, burns blackened. | Cloud, then PC Unreal | Sonnet, high | 90 min cloud + 90 min PC | A3, H3 |
| | V8 | **Dismemberment.** On `PawnPartLost` the limb's bones are hidden and a physical severed limb (a mesh cut off at the joint with a stump cap and a bone-end disc) flies off with blood; stumps stay with bandages; explosions can give gibs (chunks and blood) instead of a ragdoll. Legs lost: the pawn crawls or uses a prosthetic mesh. | PC Unreal | Opus, high | 120 min | V7 |
| | V9 | **Corpses, blood and rot.** Pools and trails that dry and darken, flies and a rot tint on corpses by the sim's rot stage, bones and dust at the end; ground blood stays in the filth layer's look; a toggle for gore level. | PC Unreal | Sonnet, medium | 90 min | V7 |
| | V10 | **Destructible clothes.** Clothes shred where they were hit (cut and bullet holes grow from the hit point instead of random tears), loose cloth pieces fall away as physics when a piece is destroyed, burning clothes char and flame. | PC Unreal | Sonnet, high | 90 min | V7, U16 |

### Real animation (V11 to V14)

Free routes in order: the Game Animation Sample's own library, Epic's free animation packs on
Fab, CC0 packs (Quaternius' universal animation library, Kenney), then Mixamo (sign-in) if
sneed wants it. Everything is retargeted to the MetaHuman skeleton with the engine's IK
retargeter, by commandlet.

| ✓ | ID | Task | Where | Model | Time | Needs |
| --- | --- | --- | --- | --- | --- | --- |
| | V11 | **Animation library.** Pick, download and retarget one coherent free set (locomotion variants, melee swings and blocks, roll, climb, ladder, crouch, carry, sit, sleep, hit reactions, death); log it; one montage table in data (`visual.anim`) mapping each `PawnAction` to a montage. | PC Unreal | Opus, high | 120 min | A3 |
| | V12 | **Fighting animation.** Real melee swings, blocks, dodges and the dodge roll replace the procedural poses and the somersault, still timed to the sim's hit step; hit reactions and physical-animation blends so pawns stagger and then fall; death animations before the ragdoll. | PC Unreal | Opus, high | 120 min | V11, U22 |
| | V13 | **Climbing and moving between levels.** Stair, ladder and climb animations on the sim's climb links, foot IK on slopes and steps. | PC Unreal | Sonnet, high | 90 min | V11, U17 |
| | V14 | **Work animation.** Chop, mine, build, sow, harvest, cook, haul and carry (hands visibly hold the item), eat, sleep in beds, tend, research, social chat and the mental break states, tools in hand, played from `PawnAction` and the job. | PC Unreal | Sonnet, high | 120 min | V11, C-tasks merged |

### Effects, physics and world (V15 to V19)

| ✓ | ID | Task | Where | Model | Time | Needs |
| --- | --- | --- | --- | --- | --- | --- |
| | V15 | **Niagara fire, smoke and explosions.** Replace the instanced-mesh fire with Niagara (free systems from Epic's content examples or Fab): fire that grows with the cell's size, embers, heat shimmer, smoke columns, explosion fireballs and shockwaves, muzzle flash and tracers. Budget: one system per 4 m fire block. | PC Unreal | Sonnet, high | 90 min | U14 |
| → L4, L7, L8 | V16 | _(replaced by the look pass)_ **Real art for buildings, items and weapons.** One consistent free set replacing the placeholders (walls and floors by material, doors, furniture, work tables, crops, power, guns, food, resources); icons rendered from the meshes for the UI later; set by the same editor script. May take two sittings. | PC Unreal | Sonnet, high | 90 min x 2 | A6 |
| | V17 | **Destruction and falling things.** Pre-fractured Chaos versions of walls and buildings that collapse into debris when the sim destroys them, falling trees with a toppling physics body, roof and floor collapse debris from M3, dust clouds; debris is cosmetic and cleaned up after a minute. | PC Unreal | Opus, high | 120 min | U17, M3 |
| → L2, L3, L6 | V18 | _(replaced by the look pass)_ **World look.** Grass, flowers, pebbles and clutter from PCG (free Fab packs), wind in trees and grass, rock and cliff materials, rivers and marsh water, snow and wet looks reaching the clutter; budget checked against 1 km. | PC Unreal | Sonnet, high | 90 min | A2, U13 |
| → L1 | V19 | _(replaced by L1, except depth of field in direct mode, which joins V12)_ **Lighting and camera look.** Lumen settings, shadow quality scaled by distance, colour grade and film look per season and time of day, interior lighting through windows, depth of field in direct mode, a `Look.Quality` low/medium/high setting for sneed's PC. | PC Unreal | Opus, high | 90 min | V15, V16 |

### Sound (V20 and V21)

| ✓ | ID | Task | Where | Model | Time | Needs |
| --- | --- | --- | --- | --- | --- | --- |
| ✓ | V20 | **Sound system.** Cloud half: `visual.sound` cues in the defs (weapon shots, hits, footsteps by terrain, work, doors, animals, fire, weather) and a `docs/design/sound.md`; Unreal half: a `USoundSubsystem` playing cues from sim events with distance attenuation, a voice budget (32 voices), occlusion by roofs and walls, ambience beds by biome, time of day and weather, a mixer with sliders. Placeholder tones until V21. | Cloud, then PC Unreal | Sonnet, high | 60 min cloud + 90 min PC | U-tasks merged |
| PC part ✓ | V21 | **Sound and music content.** Free CC0 packs (freesound.org CC0, Kenney, Sonniss GDC free bundles, free music such as Kevin MacLeod or Incompetech with attribution logged) downloaded by script and wired to the cues; calm, tense and combat music that follows the narrator's threat. | PC light | Sonnet, medium | 90 min | V20 |

### Styled UI (V22 to V24)

| ✓ | ID | Task | Where | Model | Time | Needs |
| --- | --- | --- | --- | --- | --- | --- |
| | V22 | **UI style foundation.** A colonial-frontier style guide in `docs/design/ui-style.md` (colours, fonts, spacing), free fonts (Google Fonts, OFL), a C++ Slate widget library (panel frame, buttons, tabs, tooltips, bars, icon slots, colonist bar), a theme data file; one panel (the speed bar) converted as proof. | PC Unreal | Opus, high | 120 min | V16 icons helpful |
| | V23 | **Convert the panels.** Inspect, architect, work and schedule tabs, colonist bar, letters, health, research, animals and trade panels move from ImGui to the new widgets, one batch per sitting. ImGui stays as the developer overlay. Two to three threads. | PC Unreal | Sonnet, medium | 90 min x 3 | V22, U18, U19, U20 |
| | V24 | **Menus and polish.** Main menu, new game screen (narrator, difficulty, scenario), pause and settings (graphics quality, sound, gore level, controls), loading screen, cursor and map markers. | PC Unreal | Sonnet, medium | 90 min | V23 |

### Wrap-up

| ✓ | ID | Task | Where | Model | Time | Needs |
| --- | --- | --- | --- | --- | --- | --- |
| | V25 | **Art performance check.** 100 pawns with faces, hair, anatomy, jiggle, wounds and clothes, a fire, rain and a 40-raider fight in a packaged build; tune distance cut-offs; record in `docs/decisions/`. Joins or replaces G3 if run after it. | PC Unreal | Sonnet, high | 90 min | V6, V8, V15, V19 |
| | V26 | **Art playtest.** Package, sneed plays and judges, notes go to `BACKLOG.md` and the next plan. | PC Unreal | Sonnet, medium | 60 min | V25 |

## Order

**Since 2026-10-27 the look pass (L1 to L10) goes first on the PC**, in its own order above; the
waves below are the rest. Done so far: L1, V1, V2, V3, V20, V21's PC part, V7's cloud half.

26 tasks, about 50 hours of thread work, 6 of them cloud halves. Unreal tasks go one at a time.
The three-way split lets the cloud do sim parts while the PC does art.

| Wave | Cloud | PC |
| --- | --- | --- |
| 1 | V1, V20 cloud half, V7 cloud half (after H3) | V11, V15, V16 |
| 2 | | V2, V3, V12, V18 |
| 3 | | V4 (sneed picks), V7, V13, V14 |
| 4 | | V5, V6, V8, V9, V10, V17 |
| 5 | | V19, V20 PC half, V21, V22 |
| 6 | | V23, V24, V25, V26 |

Biggest effects for the money, if time is short: the look pass L1 to L4, then V11 and V12 (real
animation) and V8 (gore). V2 (faces) is done. Sound (V20, V21) and the styled UI (V22 to V24)
cost the most thread time for the least change to how the game plays.

## What sneed does

- **Say when the PC is free** for each Unreal task, as before.
- **V4: choose the anatomy route** from screenshots, and hand over the MakeHuman files if
  route (b) is picked (the thread gives the copy steps).
- **V11, V16, V18, V21: download packs** where a licence forbids scripts (click-by-click steps
  come with the thread).
- **Say how much gore** (V8, V9) and how strong jiggle (V6) you want; both get sliders.
- **Judge screenshots** at the end of each task, and play at V26.

## Open points for sneed

1. Mixamo animations need a free Adobe sign-in; the plan uses other free sets first. Say if you
   want Mixamo anyway.
2. Paid anatomy or character tools (Character Creator, Daz, the AVC Avatar Creator add-on)
   stay a fallback if V4's free routes look bad.
3. Order versus Stage 2/3: this stage can start now on the PC while the cloud finishes Stage 2.
