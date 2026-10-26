# Art stage: how the game looks, moves and sounds

_Planned 2026-10-26 from sneed's wishes in `game-design.md`, `architecture.md`, decisions 0001 and
0002, the design docs, `BACKLOG.md` and the memory notes. It runs beside Stage 2 and Stage 3
(art needs the PC; the systems are mostly cloud) and is finished before the "vanilla core" is
called done. Rules: free assets only (logged in `assets/manifest.json`), our own setting and
names, adults only, nudity yes and sex no (`game-design.md` "Content rules")._

## Where each wish stands (checked 2026-10-26)

| Wish | State | Where |
| --- | --- | --- |
| Realistic graphics (terrain, light, sky, weather) | Partly done | Terrain A2, sky and weather U13, fire and lamps U14 done. Lighting and post-processing pass, grass and ground clutter: **V18, V19** |
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
| Buildings, items, weapons: one real look | Placeholders (A1, A6) | **V16** |
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

## Tasks

### Faces, hair and skin (V1 to V3)

| ✓ | ID | Task | Where | Model | Time | Needs |
| --- | --- | --- | --- | --- | --- | --- |
| | V1 | **Looks seed, part 2 (sim).** Add to `PawnLooks` a face seed (face shape, eye colour, brow, nose, jaw, each a small number), hair style, beard (men), body-hair amount, tattoo and scar slots; filled from the seed and the lineage genes, saved, hashed and tested like A3. Scars come from healed injuries (`PawnInjured` healing into a scar, `ours`). | Cloud | Sonnet, high | 90 min | A3 |
| | V2 | **Faces and hair on screen.** Free face variety: MetaHuman face morph targets mixed by the face seed (same DNA route as A3's bodies), eye colour, brows, 8 or more groom hair styles and beards from MetaHuman's own grooms; far pawns use hair cards. Ten colonists side by side must not look alike. | PC Unreal | Opus, high | 120 min (two sittings) | V1 |
| | V3 | **Body hair, scars, tattoos.** Body hair as a texture layer in `M_PawnSkin` (chest, arms, legs, by amount and hair colour, fading with distance), scars and tattoos as decal layers on the skin material by body part. | PC Unreal | Sonnet, high | 90 min | V1 |

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
| | V7 | **Wounds on bodies.** Snapshot gets each pawn's injuries by part and kind (cut, bruise, burn, bite, gunshot, infection, scar) if it does not have them yet (cloud half, small, tested); Unreal draws them as skin-material layers per body part, bandages on tended wounds, a blood-soaked look on bleeding, burns blackened. | Cloud, then PC Unreal | Sonnet, high | 90 min cloud + 90 min PC | A3, H3 |
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
| | V16 | **Real art for buildings, items and weapons.** One consistent free set replacing the placeholders (walls and floors by material, doors, furniture, work tables, crops, power, guns, food, resources); icons rendered from the meshes for the UI later; set by the same editor script. May take two sittings. | PC Unreal | Sonnet, high | 90 min x 2 | A6 |
| | V17 | **Destruction and falling things.** Pre-fractured Chaos versions of walls and buildings that collapse into debris when the sim destroys them, falling trees with a toppling physics body, roof and floor collapse debris from M3, dust clouds; debris is cosmetic and cleaned up after a minute. | PC Unreal | Opus, high | 120 min | U17, M3 |
| | V18 | **World look.** Grass, flowers, pebbles and clutter from PCG (free Fab packs), wind in trees and grass, rock and cliff materials, rivers and marsh water, snow and wet looks reaching the clutter; budget checked against 1 km. | PC Unreal | Sonnet, high | 90 min | A2, U13 |
| | V19 | **Lighting and camera look.** Lumen settings, shadow quality scaled by distance, colour grade and film look per season and time of day, interior lighting through windows, depth of field in direct mode, a `Look.Quality` low/medium/high setting for sneed's PC. | PC Unreal | Opus, high | 90 min | V15, V16 |

### Sound (V20 and V21)

| ✓ | ID | Task | Where | Model | Time | Needs |
| --- | --- | --- | --- | --- | --- | --- |
| | V20 | **Sound system.** Cloud half: `visual.sound` cues in the defs (weapon shots, hits, footsteps by terrain, work, doors, animals, fire, weather) and a `docs/design/sound.md`; Unreal half: a `USoundSubsystem` playing cues from sim events with distance attenuation, a voice budget (32 voices), occlusion by roofs and walls, ambience beds by biome, time of day and weather, a mixer with sliders. Placeholder tones until V21. | Cloud, then PC Unreal | Sonnet, high | 60 min cloud + 90 min PC | U-tasks merged |
| | V21 | **Sound and music content.** Free CC0 packs (freesound.org CC0, Kenney, Sonniss GDC free bundles, free music such as Kevin MacLeod or Incompetech with attribution logged) downloaded by script and wired to the cues; calm, tense and combat music that follows the narrator's threat. | PC light | Sonnet, medium | 90 min | V20 |

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

Biggest effects for the money, if time is short: V11 and V12 (real animation), V2 (faces), V16
(art that is not placeholder) and V8 (gore). Sound (V20, V21) and the styled UI (V22 to V24)
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
