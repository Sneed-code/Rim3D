# Playtest build (G2)

The packaged game, started on a **tropical rainforest** map: the Crashlanded start (five colonists and
you, the protagonist) on a rainforest tile of world 42. The project's biome focus is the tropics
(rainforest, swamp); the other biomes still work.

## Make the build (Claude does this on the PC)

```
powershell -ExecutionPolicy Bypass -File C:\Dev\Rim3D\tools\package.ps1
```

It packages Development Win64 into `C:\Dev\Rim3D-packaged\Windows\Rim3D.exe` at low priority (about
30 to 60 minutes from clean, log in `%TEMP%\rim3d-package.log`). The game reads its defs from the
repo's `data` folder, so data changes need no repackage. Generated art (faces, clothes, sounds,
materials) must have been made on this PC first (`docs/setup/pc-setup.md`).

## Start it (sneed)

1. Open File Explorer and go to `C:\Dev\Rim3D\tools`.
2. Double-click **play-rainforest.bat**. (Windows may ask "Do you want to run this file?": click
   **Run**.) A black window flashes and the game opens; the first start takes up to a minute while
   shaders warm up.
3. The game opens on the overseer camera above the colony (Tab walks the protagonist). It starts at
   midnight with the colonists asleep, running at normal speed. **F2** (or Space in the overseer view) pauses,
   **2** / **3** run faster until morning.
4. Press **F1** any time for the list of keys (bottom right).
5. To quit: **Alt+F4**. **F5** saves, **F9** loads the quick save.

Another map: run it from a command prompt with a seed, `play-rainforest.bat -SimSeed=7` (each seed
is another world and another rainforest tile). A windowed game: add `-windowed`.

## Keys

| Mode | Key | What it does |
|---|---|---|
| Any | F2 (or Space in the overseer view) | pause / unpause |
| Any | 1, 2, 3 | game speed |
| Any | . (full stop) | one step while paused |
| Any | F1 | show / hide the key list |
| Any | F5 / F9 | quick save / quick load |
| Any | F8 | write a bug report (please press it when something looks wrong) |
| Any | F7 | save a replay |
| Any | Tab | switch between the overseer camera and walking the protagonist |
| Overseer | W A S D or arrows | move the camera (Shift: faster) |
| Overseer | Q / E | turn the camera; middle mouse drag also turns it |
| Overseer | mouse wheel | zoom |
| Overseer | left click / drag | select colonists and things |
| Overseer | right click | order the selected colonists (walk, attack, work) |
| Overseer | R | draft / undraft the selected colonists (rotates a building while placing one) |
| Overseer | F | centre on the selection; Home resets the camera |
| Overseer | Page Up / Page Down | floor levels |
| Overseer | Esc | clear the selection |
| Protagonist | W A S D | walk (Ctrl: slow walk) |
| Protagonist | mouse | look; right mouse aims, left mouse fires |
| Protagonist | R | reload; Shift sprints; Space jumps; Left Alt rolls; G throws a grenade |
| Protagonist | E | pick up the nearest gun; Q switches weapon; C crouch, Z prone |
| Protagonist | V | first / third person; T takes over the colonist you look at |
| Protagonist | B | build mode (left click places, R turns, Q changes the material, right click or Esc cancels) |
| Protagonist | F (hold) | hands-on work on what you look at; X puts down what you carry |

Top left of the screen: **Work**, **Schedule** and **Log** buttons and a button per colonist (click
selects, double click centres).

## What to look at

- Does the rainforest feel tropical (plants, animals, weather, heat)? Notes on looks go to the art plan.
- Can you get the colony going: shelter, food, beds, power?
- Raids: colonists currently lose small gunfights (`BACKLOG.md`), so expect trouble around day 30.
- Anything confusing, broken or slow: press **F8** and tell Claude what happened. Notes go to
  `BACKLOG.md` and the Stage 2 plan.
