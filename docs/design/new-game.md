# Title screen and new game

sneed (2026-11-21): a title screen with saves and scenarios, nicer than RimWorld's. One scenario for now, **Crash
landed**; character creation with the 3D person; humans only (other races shown, locked); one difficulty, "standard",
not shown; world generation with a landing site (tropical only for now). Later the same day: creation in **two parts**
(You, cosmetic only; then the two companions with a trait-point pool), you are an **amnesiac**, everyone lands
**naked** with cotton clothes in the cargo pack, no backstories, and our own UI style instead of the ChatGPT concepts.

Code: sim `Private/Scenario.cpp` (`editColonist`, `startingColonistSlots`), `Sim::applyPregameCommands`; Unreal
`ColonyHudNewGame.cpp` (pages), `ColonyHudScreens.cpp` (`StartNewColony`, `DrawMainMenu`), `WorldScreenSubsystem`
(new-game mode), `CreatorPeople.h` / `SmokeTestCreator.cpp` (`CreatorPeople::FLook`), `PawnSubsystem.cpp` and
`PawnActor.cpp` (`ApplyCreatorLook`). Tests: `Tests/NewGameTests.cpp`, smoke test `newgame`. Data: `Scenarios`
`CrashLanded`.

## The scenario

`CrashLanded` (`"menu": true`; only defs with `menu` are offered, by `menuOrder`): the protagonist, an amnesiac, and 2
human strangers aged 18 (`colonistAge`; baseline, no relations), all naked, with 50 survival meals, 200 silver, 50 steel,
30 components, one bolt-action rifle, and 3 cotton (`Cloth`) t-shirts and 3 trousers lying beside them (a
`startingThings` entry may name a `stuff`; the cargo pack itself comes later). `traitPoints` 10: no backstories, traits
from a point pool ([traits.md](traits.md) "Trait points"); you have none. Biome `TropicalRainforest`; `siteBiomes`
(rainforest and tropical swamp) are the world tiles it may land on. All of this is ours (sneed's list). The old
`Crashlanded`, `CrashlandedRainforest` and `PerfColony` stay for tests and smoke tests, with backstories.

## The steps

1. **Title** (`MainMenuPage` 0): shown at launch without `-SimScenario` (not in smoke tests; `-NoMenu` skips it) and
   from Menu > Main menu. Continue (the newest save), New colony, Load a game (newest first, with the save's date),
   Settings, Debug, Quit. "Back to the game" while one runs.
   **Its own scene** (sneed 2026-11-22: not the engine's sample level, not the gun range): a plain launch starts the
   `TitleBackdrop` scenario (a wild tropical rainforest tile, 256 m, no people or cargo, animals on, 7:00) off the sample
   level (`-SimOriginX` 300000), flagged `FSimStartOptions::bTitle` so it never counts as a game (no "Back to the game";
   New colony or a load replaces it; `-SimNoTitleScene` keeps the old empty start). What the title shows is the
   **starship stage** below; `Title.Stage 0` brings back the old camera circling the rainforest glade (the open spot
   with the fewest trees, trunks on its circle hidden, the sky held at a calm morning hour).
   **Layout**: no box. The name in big, widely spaced capitals with a jade bar and the setting line under it, then the
   menu as big plain words (the hovered one brightens, steps in and gets the jade bar; Continue or New colony in jade),
   fading and sliding in one by one; a deep shade on the left fading out by the middle, darker top and bottom edges; the
   land's name bottom right. The load page uses the Canopy panel too.
2. **Scenario** (1): the menu scenarios on the left; the picked one's description, people and kit on the right, the
   narrator, and a folded "World seed and map size". Difficulty is `StriveToSurvive`, never shown. **Choose landing
   site** or **Land somewhere random** (the world's own pick in the scenario's biome).
3. **Landing site**: the world screen in new-game mode (`SetNewGame(biomes)`): tiles outside `siteBiomes` are dimmed
   and say "Only tropical land can be settled for now"; the button reads **Land here**; **Pick a random site**; Back or
   Esc returns to step 2. Landing starts the sim (paused) and opens step 4.
4. **You** (page 3): the colony is made and waits at game second 0 (speed 0). Left, "Who you are": the amnesia, your
   name (first, nick, last; sent when the field is left) with **Random name**, and a note that there is nothing to
   choose about what you can do. Right, **Your looks** (below). You start as the creator's untouched default woman
   (`seed=-1`, the gun range's), so every hair and body update reaches you while you leave her be; **Default look**
   goes back to her. **Next: your companions**.
5. **Your companions** (page 4): the races on top (Human open; Impid, Orcoid, drow and cat-strain locked); left, a card
   per stranger (mugshot, name, age 18, traits) with **Reroll** and **Reroll both**, and the picked one's name; right,
   **Traits and skills** (points left in large figures; the chosen traits as chips, click to take back; the shop of
   strengths with their cost and drawbacks with the points they give, greyed with the reason when they don't fit;
   **Random traits** spends all points, **Clear**; skills with bars to 20, skill-trait levels in jade past them, and
   passions) or **Looks**. **Land and start** lands them: the game runs at speed 1 in the overseer view.

**Looks** (both screens): woman or man, height, thin to heavy, soft to muscular, breasts, more masculine or feminine,
older (companions only a little), skin, hair style (the styles the body has) and colour, eyes, body hair below, Random
looks. Changes show at once on the body and go to the sim when a slider is let go.

Under the body on both screens: a **clothes on** tick (the cargo pack's cotton shirt and trousers on everyone, drawn
through the real clothes path, `USimPawnSubsystem::SetApparelPreview`, nothing in the sim) and the turntable: dragging
spins the body itself (`ASimPawnActor::SetPreviewSpin`), so soft tissue and hair swing; it eases to a stop, up and down
tilt, the wheel zooms. The stage is cosmetic too: the sky is held at mid-morning while the screen is open
(`USkySubsystem::SetHourOverride`; the sim's clock is untouched; the hour nearest 10:30, earlier, whose exposure stays
under 12.25 EV, because higher the engine prints a clipping warning), plants within 3.5 m of each person on show are not
drawn (`UTerrainSubsystem::SetPlantClearings`), a soft key light stands by the camera, and for each
pawn the camera picks the direction nearest its front with no tree trunk or other person in the way, turning the body to
face it. The mannequin a MakeHuman body is retargeted from is hidden even without a MetaHuman (it showed through).

**Canopy**, the screens' own look (sneed: no reference UIs; he judges, may change it): dark green-black glass panels with
square corners and a short jade bar on top, bone text, jade for what is chosen and ember for costs, three text sizes
(titles 2.2x, headings 1.25x, body), one pad (24) and one gap (10). Title and scenario pages use it too.

## Editing before the start

`EditColonistCommand{slot, firstName, nickName, lastName, female, setCreator, creator}` changes a starting colonist in
place before the first step: empty names keep theirs; `female` 0 or 1 sets the gender and makes the seeded looks again
for it (seed from the game seed, slot and gender; lineage and creator look kept); `setCreator` stores the creator look
text. Names over 24 letters, a bad slot, or any step after the first fail. It goes through saves and replays.

`SetColonistTraitsCommand{slot, traits, randomize, roll}` gives a starting companion traits from the scenario's pool
("Def:degree;..." or random from the game seed, slot and roll); it fails for the protagonist, after the first step, or
for a set `traitSetFits` refuses. Skills keep their base and take the new traits' gains; work follows the traits.

`Sim::applyPregameCommands()` applies the queued commands without stepping while `time().steps` is 0, logged at step
0; a replay applies them in its first step before anything runs, so it comes out the same (tested). The people page
calls it every frame through `USimSubsystem::ApplyPregameCommands`, which also refreshes the snapshot.

## The 3D people (the live pipeline)

`PawnLooks::creator` (save section `looks` 4, snapshot `PawnLooksView::creator`, in `stateHash`) holds the creator's
look as text: `seed=123;female=1;height=1.02;hairStyle=3;skinR=0.9;...`. Only Unreal reads it. `CreatorPeople::FLook`
decodes it: the creator's random person for the seed (`CreatorRandomize`) with the named values on top. A pawn with a
creator look is drawn as that MakeHuman person: `LooksFor` (base, skin, hair) and `ASimPawnActor::ApplyCreatorLook`,
which runs the creator's own `CreatorApply` on the body once it is on (again when the text changes or the body is
remade). So the people page, the game and the creator share one path: hair, strand hair, clothes and body updates
show everywhere. Each starting colonist gets a look on the people page the first time it is seen (seeded by game,
slot and rerolls). Pawns without one (joiners, raiders) are still the MetaHumans until colony pawns move to MakeHuman.

"Clothes on" draws the cotton shirt and trousers as MakeHuman garments (#425). The scenario's `startHour` (Crash Landed:
7, ours) sets the calendar's first hour so the landing is by daylight; steps still start at 0.

## The starship stage (title backdrop)

sneed (2026-11-23): "on title screen I'd prefer it's from a space ship window in orbit with the current character in
some glitterworld wear looking at the viewer with a world at her back and the galaxy background. Rizz up her attire."

`ATitleStage` (`TitleStage.cpp`), all cosmetic, built by `UColonyHudSubsystem::UpdateTitleStage` 30 km over the title
map's corner, where nothing else is drawn. The TitleBackdrop sim still runs underneath (the menus work as before);
leaving the title (New colony's landing, a load, a debug scene, the people pages) destroys the stage in
`EndTitleCamera`, and the sky comes back.

- **Cabin**: run-time meshes (`UProceduralMeshComponent`) of `M_GunPBR` instances: a dark glossy floor, the window's
  sill, header and mullions in gunmetal, thin glowing cyan strips on the sill and mullions and a gold one under the
  header. Without `M_GunPBR` the plain vertex-colour material, no glow.
- **Her**: the player's character. Her creator look text (`PawnLooks::creator`) is remembered in
  `game/Saved/TitleArt/Character.txt` whenever a game is saved or loaded (the protagonist's) and when "You" is edited
  on the character screen; with nothing remembered yet, the newest save is read once; with no save, the creator's
  default woman with long hair. A stand-alone `ASimPawnActor` (no sim pawn) with the creator look and strand hair,
  standing idle (the animation's standing idle: breathing, weight shifts), facing the camera, framed from mid-thigh up
  on the right third so the menu has the left. Smoke tests never write the file.
- **Her clothes** (lumen-world wear): the fitted `UniformJacket` in pearl white with a soft sheen over glossy black
  `Trousers` (first imported of `UniformJacket, Blouse, Tank, TeeTucked` and `Trousers, JeansClassic, JeansStraight`):
  the garments' tint (custom primitive data), no wear, and a lower `Roughness` on their own material instances. The
  garment material has no emissive, so the glowing accents are the cabin's.
- **Through the window**: a planet 2 km across seen 35 degrees wide, its limb just under the middle of the picture,
  the surface map with the cloud map laid over it, turning slowly; a thin rim of air (rings of `M_BuildingGhost` just in
  front of the planet, brightest on the sun's side); and the galaxy, a 9 km sphere seen from inside with the galaxy map
  as glow, on its own light channel so the sun leaves it black.
- **Light**: the stage's own sun from screen right and a little behind her (a rim on her, the planet half lit), a soft
  key up left of the camera, a cyan glow from the sill and a warm light over her hair. The exposure is held at 9 EV
  (no auto exposure); the sky's lights, atmosphere, fog, clouds, stars and exposure are put away
  (`USkySubsystem::SetStageMode`).
- **Camera**: 45 degrees wide, 1.38 m up, drifting a few centimetres and a fraction of a degree.
- **Maps**: `tools/title_art.py` (art step `title-art`, run through Blender's Python for numpy) paints
  `planet_color.png`, `planet_clouds.png` and `galaxy.png` (4096 x 2048, equirectangular, seeded) into
  `game/Saved/TitleArt/`. The game reads them at run time; without them the planet is plain blue and space plain dark.

## Smoke test `newgame`

Title (the starship stage: fails if she, her body, her clothes, the planet or the galaxy is missing, or the sky's
sun still shines there; `NEWGAME title stage` names where her look came from and her outfit), scenario page, "Land somewhere random" on seed 42 (You: three colonists, each with a creator look on its
MakeHuman body, you the default woman with no traits, game still at step 0, `NEWGAME person` lines), clothes on and
spinning (`NEWGAME clothes on`: shirt and trousers drawn as garments, nothing worn in the sim), the companions after random traits (`NEWGAME companion`: 18,
10 of 10 points), their Looks tab after a reroll of slot 2 (`NEWGAME reroll`), and Start (the game runs, looks kept).
One 1080p screenshot each with the UI.

## Debug scenes

The title's **Debug** button (sneed, 2026-11-21: one game, every merge reaches everything) opens a page of test scenes
that used to need their own launch file or their own copy of the game (`DebugScenes.h`): **play** scenes that run until
you leave (the gun range, the gun range with unaware guards, the character creator, the body showcase, the rainforest
colony straight from the menus) and **watch** scenes, the smoke tests that show something and end (finishers, melee,
gore, wounds and dirt, a gunfight, hands-on work, sky, fire, water, animals, clothes, the shuttle, sounds).

**Start scene** loads the scene inside the running game (sneed 2026-11-22: the window closing and reopening felt like a
separate program): the scene's arguments (exactly what its old `.bat` passed) go on the game's command line and the level
loads again, so every world system starts fresh and reads them as in a fresh copy. The console settings a scene changes
(graphics, the sky's hour, the overseer UI, the near clip plane, screen messages) are remembered when it starts and put
back when it ends. The command line carries `-DebugScene=<id>`: **F10**, or the scene ending, loads the title screen
again (the command line without any scene's arguments, `DebugScenes::StripSceneArgs`) instead of quitting.
The old launch files still work and run the same scenes in the same copy of the game.

## Not done yet

sneed's verdict on the Canopy look; the cargo pack as a thing to unpack; save pictures and day/wealth
on the load list; age; face and expression sliders; a 3D globe for the landing site.
