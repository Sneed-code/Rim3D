# Damage to buildings and trees

sneed, 2026-11-20: "things like buildings / trees (anything that can take damage) even walls are affected by melee
weapons (but normal weapons do for now 50% less damage). Pawns that chop trees will spawn a hand axe when they chop with
chopping animation (2 hand etc, but it's not an item for now). Also trees need collision."

## Rules (sim)

All numbers in `data/defs/Rules/StructureDamageRules.json`.

| What | Buildings and walls | Trees and plants |
|---|---|---|
| Melee blow | whole damage (`meleeBuildingFactor` 1) | whole damage (`meleePlantFactor` 1) |
| Bullet | half (`rangedBuildingFactor` 0.5, sneed) | half (`rangedPlantFactor` 0.5) |
| Blast | as before (DamageTypes `explosion.*` factors) | `blastPlantFactor` 1 (RimWorld: blasts hurt plants) |

- **Melee.** `MeleeSystem::strike` already traced the blow and stopped at a wall; now the building or tree it meets loses
  the blow's damage (after skill, weapon quality and stuff), through `CombatSystem::damageStruck`. A swing at nobody (the
  player's free swing) also catches a wall or a tree in its arc, not only on the centre line. The MeleeHit event says
  `hit` Building or Plant, `amount` the hit points taken, `message` "hit"; for a tree `cell` is the tree's cell and
  `struckDef` the plant. `MeleeSystem::structureHits()` counts them.
- **Bullets.** A bullet into a building takes half its damage; the ProjectileHit `amount` is what it took.
- **Plants have hit points.** `stats.maxHitPoints` of the plant def (RimWorld: grass 85, a tree 200). Damage piles up
  in the plant layer (`Plants::damage`, `hitPoints`, one 16-bit number per cell), stays while the plant grows, goes with
  the plant, and is saved (section `plants` v2, list `damage`). At 0 the plant dies and the cell is empty (no wood: a
  tree knocked down by blows or shots is lost, as in RimWorld; cutting it is how you get wood).
- **Trunks.** `Plants::trunkOf` gives a tree's trunk as a standing cylinder: at `Plants::stemPoint` (the cell's centre
  moved up to 0.35 m by a hash of the cell; Unreal's `TerrainSubsystem` draws the tree at that same spot by calling it),
  radius `visual.stem.radius` (oak 0.28 m) and height `visual.stem.height`, both times 0.2 + 0.8 x growth (Unreal draws
  saplings from 0.2 of full size). `tracesMeetTrees`: bullets, sight lines of shots and melee blows stop at a trunk
  (`ProjectileHitKind::Plant`). Grass, bushes and crops don't stop anything.
- **Solid trunks.** The player's body (DirectMove) can't go into a trunk: the part of the move going in is taken off, so
  it slides round the tree. Colonists and raiders on paths still walk through tree cells as in RimWorld (trees are
  pass-through cells there); making path followers steer round trunks is a later step.

## Felling trees (Unreal)

`ThingView::felling` is true while a pawn is at work on a tree (CutPlant with work done above 0, the plant a tree).
`UWorkToolSubsystem` then:
- hangs a hand axe in the pawn's right fist (`HoldDraw::BladeFrame`), drawn from `visual.parts` of the Rules def
  `WorkTools` ("fellingAxe"), not an item; `Work.AxeRoll`, `Work.AxePitch`, `Work.AxeYaw` turn it in the fist;
- plays the chop on a loop in the action slot (`ASimPawnActor::SetWorkLoop`): `Work.ChopClip` 0 the two-handed mocap
  chop `A_ChopAxe` (FlowHMR clip 02), 1 Mixamo's horizontal axe swing; `Work.ChopRate` its speed;
- `UWeaponDrawSubsystem` holsters the pawn's own weapon and lets go of its hands while it fells.
When it stops, the axe goes and the clip blends out. Hits on trunks throw bark and wood chips (`UFightSubsystem`).

Smoke `handson` (chop by hand) fails if no axe was shown and logs the axes and chop loops.

## Tests

`Tests/StructureDamageTests.cpp`: a mace knocks a wall down blow by blow; a bullet does half to a wall; plant hit points,
damage kept through growth and a save; the trunk is where Unreal draws it and traces meet it; a bullet into a tree takes
half, a blow whole; a blast kills grass and hurts a tree at its edge; the player slides round a trunk.

## Not yet

- Visible damage on buildings (cracks, rubble): the thread "Audit the to-do list, wetness and destruction".
- Path followers steering round trunks; a falling tree.
