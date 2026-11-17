# Plants, trees and loose props against bodies

sneed (2026-11-16, part of "animations must physically move bodies"): "physical structural things (ground, cliffs,
rocks, structures) are blocked by bodies. Smaller loose props with a minor tag get moved based on weight physics when
interacted ... trees, leaves, grass, crops etc that are attached to something move and bend and don't stop movement
(maybe slow it slightly), or change it if wading in swamp."

This is the world half of [body-motion.md](body-motion.md). The sim decides all of it; Unreal only draws it.

Code: `Plants::trunkAt`, `Plants::walkFactor`, `Plants::stemOffset` (`Plants.h`), the trunk helpers in `Movement.h`
(`trunkOverlap`, `trunksAllow`, `pushOutOfTrunks`, `trunkNear`), `MovementSystem`, `BodyMotionSystem::fits`,
`BodyMotionSystem::shoveProps`, `Sim` (DirectMoveCommand, snapshot `moveFactor`). Tests: `Tests/PlantBodyTests.cpp`.

## What blocks a body, what doesn't

| Thing | What a body does |
| --- | --- |
| Walls, buildings that block, rock, cliffs, deep water, the map's edge | stops or slides along it (as before: `pawnFitsAt`) |
| Another standing body | shoves it by weight (body-motion.md) |
| **A tree trunk** | stops at it or slides round it: a solid round post |
| Grass, bushes, crops, a tree's leaves | walks through; they bend (Unreal) and slow the walk a little |
| **A loose prop** (an item whose def is `minor`) | pushes it aside by weight, within its own cell |
| A heavy item (over half the body's weight) or a stone chunk | steps round or over it; it doesn't move |
| Swamp: marsh and mud, shallow and deep water | slows by RimWorld's path cost and the water's depth (unchanged, see below) |

## Tree trunks

- A trunk stands **where Unreal draws the tree**: the cell's centre moved by up to 0.35 m each way by a hash of the
  cell's index (`Plants::stemOffset`, the same mix as `TerrainSubsystem::GatherChunkPlants`). If one side changes,
  the other must too.
- It is `plant.trunkRadius` metres wide at full growth (ours, Things defs: teak and oak 0.3, willow 0.25, pine 0.22,
  poplar 0.18, palm and cecropia 0.15, saguaro 0.2), scaled by the size the tree is drawn at for its growth (0.2 for
  a sprout to 1 in six steps). Bamboo (cane palm clumps) has no trunk: its thin canes bend.
- Against trunks a body is its body radius (0.25 m for a man, MotionRules `bodyRadius`), at most
  `trunkBodyRadiusMax` (0.4): an elephant or a tyrannosaurus still gets through jungle the pathfinder thinks it can.
- **Walking**: a trunk near the pawn is steered round like a wall (every candidate velocity that would put the body
  into a trunk, now or where it would come to rest, costs the wall penalty), a waypoint whose cell centre is inside a
  trunk moves beside it, and if the chosen move still ends in a trunk the body is set beside the trunk instead (it
  slides round). A body never ends a step deeper in a trunk.
- **Never trapped**: the pathfinder doesn't know about trunks (they don't fill a cell), so in dense jungle two trunks
  can stand closer than a body is wide. A pawn held up that long already walks through the others for a while
  (`ghostSteps`); while it does it squeezes past trunks too. Test: a tree in every cell of a 12 m block, crossed.
- **Thrown bodies** (rolls, staggers, knockdowns, blasts, lunges): the sweep stops at a trunk as at a wall
  (`BodyMotionSystem::fits`). A body already in one (a tree grew round it) may always move out.
- **The player's colonist**: DirectMoveCommand slides it round trunks the same way.
- A tree's own RimWorld path cost (42) is not used for walking: the trunk is solid instead and the canopy doesn't slow.

## Plants slow walking a little

`Plants::walkFactor(cell)` = `1 / (1 + pathCost x walkCostShare x growth x 0.077)`: RimWorld's path cost of the plant
(Things def `pathCost`: tall grass and crops 14, bush and berry bush 30, short grass 0) times PlantRules
`walkCostShare` (0.1, ours: sneed asked for plants to bend and slow only slightly), scaled by how grown it is. A
grown bush keeps 81% of the pace, tall grass 90%. The AI's walk gets it in `MovementSystem` (on top of the ground and
slope); the player's walk gets it through the snapshot's `moveFactor`, which Unreal already multiplies in.

The pathfinder still costs every walkable cell the same (it never had terrain costs; path smoothing weighs the
ground's speed only). With plants at a tenth of RimWorld's cost, that changes little.

## Swamp

Already slower before this: marsh and mud cost RimWorld's 14 (48% of the pace), shallow water 30, and water deeper
than the waist slows by its depth against the body, then swimming (water.md). Plants in a swamp slow on top of that.

## Loose props

- An item is a loose prop when its def (or a parent) has `minor: true`: ItemBase (resources, meals, medicine...),
  GunBase and ApparelBase. ChunkBase says `minor: false` (stone chunks are big rocks).
- Every step after walking and motion (`CombatSystem::step` calls `BodyMotionSystem::shoveProps`), each standing body
  pushes the props it overlaps (the prop is `propRadius` 0.15 m) out of its way by the share its weight allows:
  `body / (body + prop)` in kg, a body weighing `bodyKgPerSize` (60, RimWorld's human mass) per body size and a prop
  its `stats.mass` times its stack count. A prop heavier than `propMaxMassShare` (0.5) of the body doesn't move: a man
  steps round a 75-stack of steel (37.5 kg) but kicks a rifle aside.
- A prop **stays in its own cell** (`propCellMargin` 0.1 m from the edge): storage, hauling and the one-stack-a-cell
  rule go by cell. The snapshot's item `x`, `y` are its position, so Unreal draws it where it was pushed.
- Bodies on the ground (downed, prone) don't push props.

## Cost

Trunks are looked up from the plant layer (no new state, nothing saved): a few cells round a pawn, and the candidate
tests only when a trunk is within reach. Props: a few item-per-cell lookups per standing body per step.

## Next (PC)

- **Foliage bending round bodies**: grass, bushes, crops and low leaves pushed aside where a body walks (a material
  world-position offset from the nearest pawns' positions, sent each frame), springing back after.
- Unreal's plant drawing could call `colony::Plants::stemOffset` instead of its own copy of the hash.
- The protagonist pawn's own capsule in Unreal should not need tree collision: the sim slides it round trunks.
