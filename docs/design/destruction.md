# Destruction: buildings that crack and come down

sneed (2026-11-20): "what about destruction mechanics? is that automatically applied to everything (buildings etc)?"

In the game's rules it already applied to everything: every building, wall, door and piece of furniture has hit points
(`stats.maxHitPoints` times its stuff's factor), and bullets, explosions, fire and melee (raid sappers) take them away;
at 0 the building is gone. But on screen it just vanished: no damage showing as it was hit, no rubble, nothing falling.
This adds what RimWorld does when a building is destroyed, and what that looks like in 3D. Art-stage task V17 covered
this; this is its first sitting.

## The rules (sim)

A building wrecked by damage (`CombatSystem::damageBuilding`, a bullet hitting it, a blast, a sapper's blows: all go
through the combat system's before-destroy hook) is noted with its def, stuff, footprint and whether it was finished.
At the end of that step, once its footprint is free:
- **Rubble** (RimWorld's `filthLeaving`, Filth_RubbleBuilding; our Filth def `Rubble`): 1 to 3 layers on every cell of the
  footprint. It doesn't fade or wash away in rain; colonists clean it like any filth. Beauty -5, cleanliness -5, 35
  cleaning ticks a layer (recalled, marked unverified).
- **Leftovers** (RimWorld's KillFinalize leave calculator): `ConstructionRules.killLeaveFactor` (0.25, recalled, marked
  unverified) of a finished building's materials, rounded at random, dropped on and around the footprint. A frame leaves
  nothing. (Taking a building down with the deconstruct order gives back 0.5 as before.)
- **BuildingWrecked** event: the building, its def, footprint origin and size, middle (x, y, z in metres), stuff and
  whether it was finished. A ThingDestroyed came before it.

A dev destroy command, deconstructing, and a building burnt up by fire are not wrecks (fire leaves ash, as before).
Code: `Sim::noteWreck`, `Sim::settleWrecks`, `Construction::leaveWreck`. Tests: `ConstructionTests.cpp`, "Destruction: ...".

## What it looks like (Unreal)

- **Damage look**: a finished building that has lost hit points shows it. `UBuildingSubsystem` writes how much is lost (in
  eight steps, so a chunk isn't rebuilt for every bullet) into per-instance custom data 7, and every building and surface
  material patched by `-run=WetLook` ([wetness.md](wetness.md)) draws a network of cracks from it: the edges of a
  world-space cell pattern about 40 cm across, on the plane the face lies in; more of the cells crack and the cracks widen
  as the damage grows, dark inside; the face greys and dirties a little. Anything else reads 0 and looks as before.
- **Coming down** (`UWreckSubsystem`, on BuildingWrecked): the building falls apart into broken pieces of its own stuff
  (the stuff's real surface material, so a granite wall breaks into granite blocks, a wooden one into wood; plain grey
  steel for a building of no stuff), up to 18 a cell for a wall and fewer for low furniture, through its whole height,
  tumbling under Chaos physics onto an invisible floor. Dust bursts out of it and rolls over the rubble (the blast art's
  smoke sprites, so it needs `-run=BlastArt`). After 6 seconds the pieces freeze where they lie and stay as the rubble.
- **Cleaning up**: when the sim's Rubble filth is all cleaned off a wreck's cells, its pieces go with it. At most
  `Wreck.MaxPieces` (1500) lie about; the oldest wreck goes first.

Console: `Wreck.Enabled`, `Wreck.MaxPieces`. Smoke test `buildings`, phase `buildings_wreck`: one wall of the test room is
battered to 40% (cracks), the next is wrecked (pieces, dust, rubble); the note gives the wrecks, settled pieces and
rubble layers.

## Later

Pre-fractured meshes (Chaos geometry collections) for the Codex and Fab buildings, so a turret or a bench breaks along
its own shape; trees that topple when felled or blown over; roofs and upper floors that fall in when their walls go
(the sim's RoofCollapse already decides it; debris and dust for it); scorch on burnt walls; bullet holes and chips.
