# Title screen and new game

sneed (2026-11-21): a title screen with saves and scenarios, nicer than RimWorld's. One scenario for now, **Crash
landed**; a character screen in RimWorld's format with the 3D person; humans only (other races shown, locked); one
difficulty, "standard", not shown; world generation with a landing site (tropical only for now). sneed will add details.

Code: sim `Private/Scenario.cpp` (`editColonist`, `startingColonistSlots`), `Sim::applyPregameCommands`; Unreal
`ColonyHudNewGame.cpp` (pages), `ColonyHudScreens.cpp` (`StartNewColony`, `DrawMainMenu`), `WorldScreenSubsystem`
(new-game mode), `CreatorPeople.h` / `SmokeTestCreator.cpp` (`CreatorPeople::FLook`), `PawnSubsystem.cpp` and
`PawnActor.cpp` (`ApplyCreatorLook`). Tests: `Tests/NewGameTests.cpp`, smoke test `newgame`. Data: `Scenarios`
`CrashLanded`.

## The scenario

`CrashLanded` (`"menu": true`; only defs with `menu` are offered, by `menuOrder`): the protagonist and 2 human strangers
(baseline, no relations: nobody starts with any), with 50 survival meals, 200 silver, 50 steel, 30 components and one
bolt-action rifle lying beside them, in t-shirts and trousers (`CrashlandedSettler`). Biome `TropicalRainforest`;
`siteBiomes` (rainforest and tropical swamp) are the world tiles it may land on. All of this is ours (sneed's list). The
old `Crashlanded`, `CrashlandedRainforest` and `PerfColony` stay for tests and smoke tests.

## The steps

1. **Title** (`MainMenuPage` 0): shown at launch without `-SimScenario` (not in smoke tests; `-NoMenu` skips it) and
   from Menu > Main menu. Continue (the newest save), New colony, Load a game (newest first, with the save's date),
   Settings, Quit. "Back to the game" while one runs.
2. **Scenario** (1): the menu scenarios on the left; the picked one's description, people and kit on the right, the
   narrator, and a folded "World seed and map size". Difficulty is `StriveToSurvive`, never shown. **Choose landing
   site** or **Land somewhere random** (the world's own pick in the scenario's biome).
3. **Landing site**: the world screen in new-game mode (`SetNewGame(biomes)`): tiles outside `siteBiomes` are dimmed
   and say "Only tropical land can be settled for now"; the button reads **Land here**; **Pick a random site**; Back or
   Esc returns to step 2. Landing starts the sim (paused) and opens step 4.
4. **People** (3): the colony is made and waits at game second 0 (speed 0). Left: a card per colonist (YOU or
   STRANGER, mugshot, name, adulthood, age) with **Reroll** (`RerollColonistCommand`) and **Reroll the strangers**.
   Top: the races, Human open, Impid, Orcoid, drow and cat-strain locked. Middle: the colonist's real pawn on the map,
   seen by a camera that turns with a drag and zooms with the wheel. Right, **Story and skills**: first, nick and last
   name (sent when the field is left), gender and age, both backstories with their text, traits, work they won't do,
   skills with bars and passion flames. **Looks**: woman or man, height, thin to heavy, soft to muscular, breasts, more
   masculine or feminine, older, skin, hair style (the styles the body has) and colour, eyes, body hair below, Random
   looks. Changes show at once on the body and go to the sim when a slider is let go. **Start** lands them: the game
   runs at speed 1 in the overseer view.

The look sliders and the camera are first-version layout in the "Frontier Glass" style (glass panels, amber lines);
the final look follows sneed's pick of the ChatGPT concepts in `/mnt/project-files/ui-concepts/`.

## Editing before the start

`EditColonistCommand{slot, firstName, nickName, lastName, female, setCreator, creator}` changes a starting colonist in
place before the first step: empty names keep theirs; `female` 0 or 1 sets the gender and makes the seeded looks again
for it (seed from the game seed, slot and gender; lineage and creator look kept); `setCreator` stores the creator look
text. Names over 24 letters, a bad slot, or any step after the first fail. It goes through saves and replays.

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

Until clothes on MakeHuman bodies (#425) are in, creator people show without clothes.

## Smoke test `newgame`

Title, scenario page, "Land somewhere random" on seed 42 (people page: three colonists, each with a creator look on
its MakeHuman body, game still at step 0, `NEWGAME person` lines), the Looks tab after a reroll of slot 2 (`NEWGAME
reroll`), and Start (the game runs, looks kept). One 1080p screenshot each with the UI.

## Not done yet

The final look (after sneed's pick); a live title-screen camera through the rainforest; save pictures and day/wealth
on the load list; picking backstories and traits; age; face and expression sliders; a 3D globe for the landing site.
