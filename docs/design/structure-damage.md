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
  saplings from 0.2 of full size). `tracesMeetTrees`: bullets and grenades in flight and melee blows stop at a trunk
  (`ProjectileHitKind::Plant`; `CombatSystem::trace(..., trees = true)`). Sight checks (can I shoot him, which turret
  sees whom, how much of a body a blast sees) look past trunks: a shooter fires past a tree and the bullet may meet it,
  so trees give cover by chance. Grass, bushes and crops don't stop anything. Older fight tests clear their maps' plants
  (`Tests/TestPlants.h`).
- **Solid trunks.** The player's body (DirectMove) can't go into a trunk: the part of the move going in is taken off, so
  it slides round the tree. Everyone else does too (next section).

## Trees in the way (sim)

sneed, 2026-11-21: colonists and raiders must not walk through trees.

- **Trunk layer.** `Map::trunkAt` / `hasTrunk` / `groundTrunks`: every standing tree's trunk (at `Map::plantSpot`, the
  spot Unreal draws it, radius as `Plants::trunkOf`, to the centimetre). `Plants::attachTrunks(&map)` keeps it in step:
  a tree placed, growing, cut, burnt or killed changes it at once, and a load rebuilds it (plants load before the path
  queue, so a half-done search is redone on the saved trees). Not saved itself.
- **Paths go round trees.** A ground cell with a trunk costs `Pathfinder::treeCost` extra metres: RimWorld's tree
  `pathCost` 42 ticks (TreeBase) over the 13 ticks of a step (a 4.6 cell/s pawn), about 3.2 m; tuning `treePathCost`.
  A diagonal step squeezing between two trunk cells costs it once more, and a step too tight to slip through between trunks (`Map::tightSteps`: the line between the two cells' walk points doesn't pass the bark with `kTrunkBodyRadius` to spare; `refreshTrunkSteps` works it out again for the cells within 2 of a trunk that changes, the whole map when the trees are attached or loaded) costs `Pathfinder::kTightTreeSteps` (3, ours) times that. So a path steps round a lone tree, and through a forest only where going round is longer. Never blocked: a wall of
  trees with no way round is walked through, between the trunks.
- **Smoothing keeps clear.** `smoothPath` only skips cells along lines that pass every trunk with a pawn's width to
  spare (`trunksClearOfLine`), and walks through each kept cell's `cellWalkPoint`: its centre, or where a trunk stands
  too close to the centre, the nearest point of the cell with room beside the bark (a goal under a tree is reached; the walk only ends once the walker stands in the goal cell itself).
- **Walkers steer round trunks.** `MovementSystem` treats the trunks within 2 cells as posts: each steering candidate
  pays for meeting one (as for a pawn standing still) and for passing too close, so a walker curves round a tree on its
  line; and the step it takes never goes into the bark (the part going in is taken off, so it slides round; candidates are judged by the slide they come to, so one pressed against the bark slides on instead of stopping). Against trunks a walker counts as `kTrunkBodyRadius` 0.22 m wide each way (ours, narrower than the 0.3 m between walls: a person turns the shoulders to slip between trees). One already
  inside a trunk (a tree grew round it, or it was spawned there) only loses the part going deeper, so it walks out without a jump, and pays no meeting cost for a trunk it is already that close to. The trunks' costs only steer, never hold a walker back: when they would make it stand (braking into a goal beside a trunk, a gap between two trunks), it takes the best step by the other costs, still slid off the bark (found by the raid soak and the stairs tests: walkers stood for good beside a tree). The stuck
  watchdog's phase still walks through everything.
- **Bodies.** Staggers, shoves and other clip moves (`BodyMotionSystem::fits`) stop at the bark too.

## Falling trees

sneed, 2026-11-21: a cut tree falls over, physically, no snapping.

- **Sim.** A tree cut down by a woodcutter ("felled") or killed by blows, shots or a blast ("broken", from
  `CombatSystem::hurtPlant`) sends PlantRemoved with (x, y) the unit vector it falls along: away from the woodcutter
  (`thing`), from the side the blow or shot struck, from the blast's centre. `count` is its growth in millionths, so
  Unreal can draw the same size. The wood is dropped as before.
- **Unreal** (`UTreeFallSubsystem`, cosmetic). The chunk drops the tree; a copy (`UTerrainSubsystem::GetPlantLook`: the
  same mesh, materials, size and turn) takes its place and falls as a rigid pole hinged at its foot (angular
  acceleration 3 g sin(lean) / 2 x height, a little air drag): a felled one starts from a 0.8 degree lean barely moving
  (0.015 rad/s), so it leans slowly for a second or two and then faster, and a grown poplar is down in about 3 to 4 s
  like a real felled tree (the first try, 1.5 degrees and 0.04 rad/s, took about 2.5 s); a broken one is pushed harder. It stops at the lean where trunk or crown
  first meets the ground or a building along the way (map heights, building heights; the crown's branches hold the
  trunk up by `TreeFall.CrownRest` of its half width), bounces (`TreeFall.Restitution`) and settles, with `Tree_Crack`
  as it goes and `Tree_Fall` and dust where it lands. It lies `TreeFall.LieSeconds` (25), then sinks into the ground over
  `TreeFall.SinkSeconds` (8) and is gone. A paused game holds it; at most `TreeFall.Max` (24) lie about.
- Smoke `handson`, last phase `tree_falls`: the poplar chopped by hand earlier fell already; a second one is felled
  (`UTreeFallSubsystem::PlayEventForTest`) at 11:00, watched from the side and above, with pictures through the fall
  (`Saved/Smoke/handson/tree_fall_0.png` to `_6`). It fails unless both fell and landed, none jumped more than 12 degrees
  in a frame, and the felled one took 1.5 to 8 s to come down; it logs the falls and the fall time.

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
half, a blow whole; a blast kills grass and hurts a tree at its edge; the player slides round a trunk. `Tests/TreeTrunkTests.cpp`: the trunk layer follows growth, death and loads; paths go
round a lone tree and through a wall of trees; smoothing never cuts through a trunk; a walker steers round a trunk on
its line and reaches a goal under a tree in a thick forest; one inside a trunk walks off it, and a goal cell holding a
trunk is reached from every side; a tree killed by a blow falls away from it. The `simrun_raid_soak` stuck rule catches
walkers standing for good beside trees.
`MiningTests` checks the felled tree's way and growth.

## Not yet

- Visible damage on buildings (cracks, rubble): the thread "Audit the to-do list, wetness and destruction".
- A falling tree that hurts whoever it lands on, or hangs up in a neighbour's crown.
- Real recorded tree sounds (the cues use Kenney's heavy wood and soft impacts for now).
