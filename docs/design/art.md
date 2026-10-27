# Art: sculptures and quality

Code: `Construction::isArt`, `JobSystem::buildWork` (the quality roll), `ConstructArtGiver` in
`Private/WorkGivers.cpp`, `RoomStats::buildingBeauty`. Defs: `data/defs/Things/Art.json`,
`QualityRules` (`beautyOffset`, `marketValueFactor`) in `Rules.json`. Tests: `Tests/ArtTests.cpp`.
Builds on [room stats](room-stats.md), [construction](construction.md) and [crafting](crafting.md).
Numbers are from the RimWorld wiki (Sculpture); `unverified` in a def marks recalled ones.

## What a sculpture is

A building def with `building.art`: `SculptureSmall` (1x1, beauty 50, 50 stuff), `SculptureLarge`
(1x2, beauty 100, 100 stuff) and `SculptureGrand` (2x2, beauty 400, 400 stuff), made of a metallic,
woody or stony stuff. It is placed like any building (architect, Furniture) and delivered to like any
frame, but **an artist builds it**: the new work giver `BuildArt` belongs to the Art work type, the
speed follows the Artistic skill (same formula as construction speed), and the Artistic skill learns
from it. Everyone else's Construction work skips art frames.

Work is `workToBuild` (RimWorld's 18,000 / 30,000 / 105,000 ticks times 1.44) times the stuff's
`workToMake` factor: wood 0.7, steel 1.0, granite, limestone and slate blocks 1.3, sandstone 1.1,
marble 1.15.

## Quality

When the sculpture is finished the artist rolls a quality from the same table as crafting
(`CraftingRules.qualityChances`, by Artistic level), stored as the `Quality` component on the
building (saved like every quality). Beauty and value then scale:

| Quality | Beauty factor | Market value factor |
| --- | --- | --- |
| awful | -0.1 | 0.5 |
| poor | 0.5 | 0.75 |
| normal | 1 | 1 |
| good | 2 | 1.25 |
| excellent | 3 | 1.5 |
| masterwork | 5 | 2.5 |
| legendary | 8 | 5 |

Beauty of an art building = (def beauty x stuff `beauty` factor + stuff `statOffsets.beauty`) x quality
factor. Marble blocks have factor 1.35 and offset 0.5 (68 for a small sculpture); the offset also
applies to any other building made of it. Wealth: stuff cost plus work, times the value factor.
Room beauty, wealth and so impressiveness (and the mood from them) follow with no other change: a
large normal sculpture in a 5 by 5 room adds 100 / 32.5 = 3 beauty; an awful one makes it uglier.

## Snapshot

A finished sculpture's `ThingView` has `stuff` and `quality`.

## Not yet

Sculpture stuffs gold, silver, jade, uranium, plasteel and obsidian (the numbers are in the wiki and
need those stuffs as items; the offset mechanism is there); art descriptions; the rule that a sculptor
cannot work on another's piece; inspirations shifting quality; meditation focus (Royalty); the Terror
sculpture (Ideology); minifying and moving a sculpture (we deconstruct and rebuild).
